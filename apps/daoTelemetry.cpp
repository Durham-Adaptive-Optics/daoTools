/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2025-09-12 16:02:50
 * @ Description: Tool for recording dao shared memory frames to FITS data files.
 */

/* ==========================================================
                        Includes                         
   ========================================================== */
#include <daoThreadSafeQueue.hpp>
#include <daoComponent.hpp>
#include <yaml-cpp/yaml.h>
#include <CLI/CLI.hpp>
#include <daoLog.hpp>
#include <fitsio.h>
#include <string>
/* ========================================================== */

/* ==========================================================
                    Recording Interface                         
   ========================================================== */
class Recorder
{
    public:
    virtual ~Recorder() = default;
    virtual void Start(const std::string &sessionDirectory) = 0;
    virtual bool IsRecording() = 0;
    virtual void Stop() = 0;
};

struct Target
{
    Recorder *recorder;
    std::string path;
    size_t capacity;
    size_t limit;
    int16_t core;

    enum class Type : std::uint8_t
    {
        UNKNOWN = 0,
        SHARED_MEMORY,
        FILE
    } type = Type::SHARED_MEMORY;
};

struct Frame
{
    // static per shm
    std::vector<long> size;
    uint64_t nElements;
    uint8_t atype;

    // change per frame
    int64_t atime;
    uint64_t cnt0;
    uint64_t cnt1;
    uint64_t cnt2;
    int8_t *data;
};

std::string daoShmLocalName(const std::string &shmPath) // todo port to C and put in daoTools.
{
    std::string localName = shmPath;
    
    auto x = shmPath.find_last_of('/');
    if (x != std::string::npos) localName = shmPath.substr(x + 1);

    auto y = localName.find('.');
    if (y != std::string::npos) localName = localName.substr(0, y);

    return localName;
}

/* ==========================================================
                    Shared Memory Recorder                         
   ========================================================== */

#define FITS_CHECK(expr)                                                                            \
        (expr);                                                                                     \
        if (fitsError) {                                                                            \
            char err_msg[FLEN_ERRMSG];                                                              \
            fits_get_errstatus(fitsError, err_msg);                                                 \
            m_log.Error("failed to export data (%s): %s", m_thread_name.c_str(), err_msg);          \
            TriggerError();                                                                         \
            return;                                                                                 \
        }

class SharedMemoryPoller : public Dao::Thread
{
    public:
    SharedMemoryPoller(
        Dao::ThreadSafeQueue<Frame> &frameQueue, 
        Dao::Log::Logger &logger,
        std::string shmPath, 
        int core
    )
        :
        Dao::Thread(shmPath, logger, core), // todo give name: Poll_localName <- requires updating test_ThreadAffinity.
        mFrameQueue(frameQueue)
    {
        LoadSharedMemory(shmPath);
    }

    ~SharedMemoryPoller()
    {
        UnloadSharedMemory();
        Exit();
    }

    void OnceOnStart() override
    {
        mInitialGrab = true;
    }

    private:
    void LoadSharedMemory(const std::string &shmName)
    {
        // open shared memory.
        if (daoShmShm2Img(shmName.c_str(), &mImage) != DAO_SUCCESS) {
            throw std::runtime_error("Failed to open shared memory");
        }
        
        // re-interpret metadata pointer as volatile.
        mMetadata = (volatile IMAGE_METADATA *)mImage.md;

        // extract frame data type.
        mFrameDatatype = mMetadata->atype;

        // extract element count.
        mFrameElementCount = mMetadata->nelement;

        // extract frame dimensions (revsered order for FITS).
        mFrameSize.resize(mMetadata->naxis);
        for(size_t i = 0; i < mMetadata->naxis; ++i)
            mFrameSize[mMetadata->naxis - 1 - i] = (long)mMetadata->size[i]; // cast uint32_t -> long for cfitsio.

        // calculate memory footprint of frame data.
        mFrameFootprint = mDaoTypeSizes.at(mFrameDatatype) * mFrameElementCount;
    }

    void UnloadSharedMemory()
    {
        daoShmCloseShm(&mImage);
    }

    void RestartableThread() override 
    {
        const uint64_t cnt0 = mMetadata->cnt0;
        if (mInitialGrab || cnt0 > mCnt0) {
            //
            mInitialGrab = false;
            mCnt0 = cnt0;
            
            // Copy frame data out of shared memory.
            Frame frame;
            frame.size = mFrameSize;
            frame.atype = mFrameDatatype;
            frame.cnt0 = mMetadata->cnt0;
            frame.cnt1 = mMetadata->cnt1;
            frame.cnt2 = mMetadata->cnt2;
            frame.nElements = mFrameElementCount;
            frame.atime = mMetadata->atime.tsfixed.secondlong;
            frame.data = (int8_t*) malloc(mFrameFootprint);
            if(frame.data) memcpy(frame.data, mImage.array.V, mFrameFootprint);

            // if we cannot allocate memory to store the frame data,
            // or if the frame itself was potentially written to
            // as we were copying it out, then drop the frame.
            // todo add check for write flag back in once John patches daoBase.
            if(!frame.data || mMetadata->cnt0 > mCnt0)
                return;
                
            // Queue the frame.
            mFrameQueue.push(frame);
        }
    }

    Dao::ThreadSafeQueue<Frame> &mFrameQueue;
    volatile IMAGE_METADATA *mMetadata;
    std::vector<long> mFrameSize;
    size_t mFrameElementCount;
    size_t mFrameFootprint;
    uint8_t mFrameDatatype;
    bool mInitialGrab;
    uint64_t mCnt0;
    IMAGE mImage;

    const std::unordered_map<uint8_t, uint8_t> mDaoTypeSizes{ // lookup table from dao types to byte sizes.
        {_DATATYPE_UINT8, SIZEOF_DATATYPE_UINT8},
        {_DATATYPE_INT8, SIZEOF_DATATYPE_INT8},
        {_DATATYPE_UINT16, SIZEOF_DATATYPE_UINT16},
        {_DATATYPE_INT16, SIZEOF_DATATYPE_INT16},
        {_DATATYPE_UINT32, SIZEOF_DATATYPE_UINT32},
        {_DATATYPE_INT32, SIZEOF_DATATYPE_INT32},
        {_DATATYPE_UINT64, SIZEOF_DATATYPE_UINT64},
        {_DATATYPE_INT64, SIZEOF_DATATYPE_INT64},
        {_DATATYPE_FLOAT, SIZEOF_DATATYPE_FLOAT},
        {_DATATYPE_DOUBLE, SIZEOF_DATATYPE_DOUBLE}
    };
};

class SharedMemoryExporter : public Dao::Thread
{
    public:
    SharedMemoryExporter(
        Dao::ThreadSafeQueue<Frame> &frameQueue, 
        Dao::Log::Logger &logger,
        std::string shmPath,
        size_t datafileCapacity,
        size_t exportLimit,
        volatile bool &errorFlag
    )
        :
        Dao::Thread("Export_" + daoShmLocalName(shmPath), logger),
        mShmLocalName(daoShmLocalName(shmPath)),
        mDatafileCapacity(datafileCapacity),
        mFrameQueue(frameQueue),
        mStorageDirectory(""),
        mExportLimit(exportLimit),
        mErrorFlag(errorFlag),
        mLogger(logger),
        mRecording(false)
    {
    }

    ~SharedMemoryExporter()
    {
        Exit();
    }

    void SetStorageDirectory(const std::string &directory)
    {
        mStorageDirectory = directory;
    }

    bool IsRecording() const { return mRecording; }

    private:
    void TriggerError()
    {
        mErrorFlag = true;
        Stop();
    }

    bool CloseDatafile()
    {
        mLogger.Debug("%s closing datafile", m_thread_name.c_str());

        int error = 0;
        fits_close_file(mDatafile, &error);
        mDatafile = nullptr;

        if (error) {
            char err_msg[FLEN_ERRMSG];
            fits_get_errstatus(error, err_msg);
            mLogger.Error("failed to close fits datafile (%s): %s", m_thread_name.c_str(), err_msg);
        }

        return !error;
    }

    void OnceOnStart() override
    {
        mLogger.Debug("%s started", m_thread_name.c_str());

        assert(mStorageDirectory != "");
        mDatafile = nullptr;
        mExportedFrames = 0;
        mDatafileCount = 0;
        mDatafileSize = 0;
        mRecording = true;
    }

    void OnceOnStop() override
    {
        mLogger.Debug("%s stopped", m_thread_name.c_str());
        mRecording = false;
        if(mDatafile && !CloseDatafile())
            TriggerError();
    }

    void RestartableThread() override
    {
        // Automatically stop once we have exported the desired number of frames (if applicable).
        if(mExportLimit && mExportedFrames == mExportLimit) {
            mLogger.Debug("%s reached export limit", m_thread_name.c_str());
            Stop();
            return;
        }

        // Close the current datafile if it's full.
        if(mDatafile && mDatafileSize == mDatafileCapacity) {
            if(!CloseDatafile()) {
                TriggerError();
                return;
            }
        }

        // Create datafile if we haven't got one.
        if(!mDatafile) {
            int status = 0;
            std::string fileName = mShmLocalName + "_" + std::to_string(mDatafileCount + 1);
            std::string filePath = mStorageDirectory + "/" + fileName + ".fits";
            mLogger.Debug("creating fits datafile %s (%s)", filePath.c_str(), m_thread_name.c_str());
            fits_create_file(&mDatafile, filePath.c_str(), &status);
            
            if (status) {
                char err_msg[FLEN_ERRMSG];
                fits_get_errstatus(status, err_msg);
                mLogger.Error("failed to create fits datafile (%s): %s", m_thread_name.c_str(), err_msg);
                TriggerError();
                return;
            }

            mDatafileSize = 0;
            ++mDatafileCount;
        }

        // Export frame from queue.
        if(mFrameQueue.size()) {
            int fitsError = 0;
            Frame frame = mFrameQueue.pop();
            FITS_CHECK( fits_create_img(mDatafile, mDaoToFitsDest.at(frame.atype), frame.size.size(), frame.size.data(), &fitsError) );
            FITS_CHECK( fits_write_key(mDatafile, TBYTE, "atype", &frame.atype, nullptr, &fitsError) );
            FITS_CHECK( fits_write_key(mDatafile, TLONGLONG, "atime", &frame.atime, nullptr, &fitsError) );
            FITS_CHECK( fits_write_key(mDatafile, TULONGLONG, "cnt0", &frame.cnt0, nullptr, &fitsError) );
            FITS_CHECK( fits_write_key(mDatafile, TULONGLONG, "cnt1", &frame.cnt1, nullptr, &fitsError) );
            FITS_CHECK( fits_write_key(mDatafile, TULONGLONG, "cnt2", &frame.cnt2, nullptr, &fitsError) );
            FITS_CHECK( fits_write_img(mDatafile, mDaoToFitsSrc.at(frame.atype), 1, frame.nElements, frame.data, &fitsError) );
            free(frame.data);
            mExportedFrames++;
            mDatafileSize++;
            mLogger.Debug("%s exported %d frames", m_thread_name.c_str(), mExportedFrames);
        }
    }

    std::string mShmLocalName;
    Dao::ThreadSafeQueue<Frame> &mFrameQueue;
    std::string mStorageDirectory;
    volatile bool &mErrorFlag;
    Dao::Log::Logger &mLogger;
    size_t mDatafileCapacity;
    size_t mExportedFrames;
    size_t mDatafileCount;
    size_t mDatafileSize;
    size_t mExportLimit;
    fitsfile *mDatafile;
    bool mRecording;

    const std::unordered_map<uint8_t, int> mDaoToFitsDest{
        {_DATATYPE_UINT8, BYTE_IMG},
        {_DATATYPE_INT8, SBYTE_IMG},
        {_DATATYPE_UINT16, USHORT_IMG},
        {_DATATYPE_INT16, SHORT_IMG},
        {_DATATYPE_UINT32, ULONG_IMG},
        {_DATATYPE_INT32, LONG_IMG},
        {_DATATYPE_UINT64, ULONGLONG_IMG},
        {_DATATYPE_INT64, LONGLONG_IMG},
        {_DATATYPE_FLOAT, FLOAT_IMG},
        {_DATATYPE_DOUBLE, DOUBLE_IMG}
    };

    const std::unordered_map<uint8_t, int> mDaoToFitsSrc{
        {_DATATYPE_UINT8, TBYTE},
        {_DATATYPE_INT8, TSBYTE},
        {_DATATYPE_UINT16, TUSHORT},
        {_DATATYPE_INT16, TSHORT},
        {_DATATYPE_UINT32, TUINT},
        {_DATATYPE_INT32, TINT},
        {_DATATYPE_UINT64, TULONGLONG},
        {_DATATYPE_INT64, TLONGLONG},
        {_DATATYPE_FLOAT, TFLOAT},
        {_DATATYPE_DOUBLE, TDOUBLE}
    };
};

class SharedMemoryRecorder : public Recorder
{
    public:
    SharedMemoryRecorder(const Target &target, Dao::Log::Logger &logger, volatile bool &errorFlag)
        :
        mExporter(mFrameQueue, logger, target.path, target.capacity, target.limit, errorFlag),
        mPoller(mFrameQueue, logger, target.path, target.core)
    {
        mExporter.Spawn();
        mPoller.Spawn();
    }
    
    ~SharedMemoryRecorder()
    {
        mExporter.Join();
        mPoller.Join();
    }

    void Start(const std::string &sessionDirectory) override
    {
        mExporter.SetStorageDirectory(sessionDirectory);
        mExporter.Start();
        mPoller.Start();
    }

    void Stop() override
    {
        mExporter.Stop();
        mPoller.Stop();
    }

    bool IsRecording() override
    {
        return mExporter.IsRecording();
    }

    private:
    Dao::ThreadSafeQueue<Frame> mFrameQueue;
    SharedMemoryExporter mExporter;
    SharedMemoryPoller mPoller;
};

/* ==========================================================
                    AppComponent Class                         
   ========================================================== */
class AppComponent : public Dao::Component
{
    public:
    AppComponent(Dao::Log::Logger &logger, const std::string &ip, size_t port, const std::string &configFile = "")
        :
        Component("daoTelemetry", logger, ip, port),
        mConfigString(""),
        mLogger(logger),
        mErrorFlag(false)
    {
        mLogger.Info("App interface is now available");

        if (configFile.length()) { // pull initial configuration from a file at startup.
            ConfigFromFile(configFile);
        }
    }

    ~AppComponent()
    {
        //
    }

    void Manage()
    {
        mLogger.Debug("State machine monitoring now running");

        //? can we make daoComponentStateMachine nicer for doing this kind of stuff.
        while (true) {
            if (mErrorFlag) {
                while (GetStateText() != "Error") OnFailure();
            }
            else if (GetStateText() == "Running") {
                // count how many targets have finished recording (if any).
                size_t nFinished = 0;
                for(const Target &target : mTargets) {
                    mLogger.Debug("Target %s: %s", target.path.c_str(), target.recorder->IsRecording() ? "Recording" : "Done");
                    if(!target.recorder->IsRecording()) nFinished++;
                }

                // if all targets have finished recording then change state to reflect this.
                if(nFinished == mTargets.size()) {
                    mLogger.Info("All targets finished recording");
                    while (GetStateText() == "Running") Idle();
                }
            }

            sleep(1);
        }
    }

    private:
    /* CONFIGURATION */
    void SetConfig(const std::string &configStr)
    {
        mConfigString = configStr;
        mLogger.Debug("Configuration set");
    }

    void ConfigFromFile(const std::string &filePath)
    {
        std::ifstream file(filePath);
        if (!file) {
            mLogger.Warning("Configuration file failed to load (%s)", filePath.c_str());
            return;
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        SetConfig(ss.str());
    }

    void Configure()
    {
        if (!mConfigString.length()) {
            throw std::invalid_argument("configuration error: no telemetry session configuration");
        }
        mLogger.Debug("configuring telemetry session..");

        const YAML::Node &config = YAML::Load(mConfigString);
        mLogger.Debug("parsed configuration string");

        if (!config["telemetry_root"]) {
            throw std::invalid_argument("configuration error: no data telemetry directory was specified");
        }
        mTelemetryRoot = config["telemetry_root"].as<std::string>();
        mLogger.Info("root directory for telemetry sessions is %s", mTelemetryRoot.c_str());

        const int16_t nominalCore = config["nominal_core"] ? config["nominal_core"].as<int16_t>() : -1;
        mLogger.Info("telemetry will be collected on core %d by default", nominalCore);

        if (config["telemetry"]) {
            Target telemetryItem;
            for (const YAML::Node &tc : config["telemetry"]) {
                const size_t telemetryNum = mTargets.size();
                mLogger.Debug("configuring telemetry-%zu..", telemetryNum);

                if (tc["target"]) {
                    telemetryItem.path = tc["target"].as<std::string>();
                    mLogger.Debug("telemetry target: %s", telemetryItem.path.c_str());
                }
                else {
                    throw std::invalid_argument("configuration error: no telemetry target specified");
                }

                telemetryItem.capacity = tc["capacity"] ? tc["capacity"].as<size_t>() : 0;
                mLogger.Debug("telemetry capacity: %zu", telemetryItem.capacity);

                telemetryItem.limit = tc["limit"] ? tc["limit"].as<size_t>() : 0;
                mLogger.Debug("telemetry limit: %zu", telemetryItem.limit);

                telemetryItem.core = tc["core"] ? tc["core"].as<int16_t>() : nominalCore;
                mLogger.Debug("telemetry core: %d", telemetryItem.core);

                mTargets.push_back(telemetryItem);

                mLogger.Debug("configured telemetry-%zu (%s)..", telemetryNum, telemetryItem.path.c_str());
            }
        }
        else {
            throw std::invalid_argument("configuration error: no telemetry specified");
        }

        if (config["files"]) {
            for (const YAML::Node &fc : config["files"]) {
                const std::string filePath = fc.as<std::string>();
                mFilesList.push_back(filePath);
                mLogger.Info("configured %s to be copied when telemetry session starts", filePath.c_str());
            }
        }

        mLogger.Debug("configuration applied");
    }

    void ClearConfiguration()
    {
        mTargets.clear();
    }

    /* RECORDING RESOURCE MANAGEMENT */
    void AllocateResources()
    {
        mLogger.Info("recording resources allocated");
        for (Target &target : mTargets) {
            mLogger.Debug("allocating recorder target %s", target.path.c_str());
            target.recorder = new SharedMemoryRecorder(target, mLogger, mErrorFlag);
        }
    }

    void DeallocateResources()
    {
        mLogger.Info("collectors resources freed");
        for (Target &target : mTargets) {
            delete target.recorder;
        }
    }

    /* SESSION MANAGEMENT */
    void BeginSession()
    {
        char timestamp[16];
        time_t t = time(nullptr);
        tm *td = localtime(&t);
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", td);
        const std::string sessionDir = mTelemetryRoot + "/" + timestamp;
        mLogger.Info("creating session directory: %s", sessionDir.c_str());
        if (mkdir(sessionDir.c_str(), 0755)) {
            printf("session dir creation error: %s\n", strerror(errno));
            throw std::runtime_error("failed to create session directory");
        }

        for (const std::string &path : mFilesList) {
            std::string filename = path;
            auto x = path.find_last_of("/");
            if (x != std::string::npos) {
                filename = path.substr(x + 1);
            }

            const std::string dst = sessionDir + "/" + filename;
            if (!std::filesystem::copy_file(path, dst)) {
                mLogger.Error("failed to copy %s to telemetry session directory", path.c_str());
                throw std::runtime_error("failed to copy file");
            }

            mLogger.Debug("successfully copied file %s to %s", path.c_str(), dst.c_str());
        }

        for (Target &t : mTargets) {
            mLogger.Debug("starting telemetry collector for %s..", t.path.c_str());
            t.recorder->Start(sessionDir);
        }

        mLogger.Info("telemetry session started");
    }

    void EndSession()
    {
        for (Target &t : mTargets) {
            mLogger.Debug("stopping telemetry collector for %s..", t.path.c_str());
            t.recorder->Stop();
        }
        mLogger.Info("telemetry session ended");
    }

    void RecoveryRoutine()
    {
        mLogger.Debug("Recovering to Idle..");
        mErrorFlag = false; // put 1st so any recovery errors are raised correctly. 
        DeallocateResources();
        AllocateResources();
    }

    /* COMPONENT API OVERLOADS */
    void PROCESS_OTHER(std::string payload) override { SetConfig(payload); }
    void transition_Off_Standby() override { Configure(); }
    void transition_Standby_Idle() override { AllocateResources(); }
    void transition_Idle_Running() override { BeginSession(); }
    void transition_Running_Idle() override { EndSession(); }
    void transition_Idle_Standby() override { DeallocateResources(); }
    void transition_Standby_Off() override { ClearConfiguration(); }
    void transition_Running_Error() { EndSession(); }
    void transition_Error_Idle() { RecoveryRoutine(); }

    /* MEMBER VARIABLES */
    std::vector<Target> mTargets;
    std::vector<std::string> mFilesList;
    std::string mTelemetryRoot;
    std::string mConfigString;
    Dao::Log::Logger &mLogger;
    volatile bool mErrorFlag;
};

/* ==========================================================
                        App Entry Point                         
   ========================================================== */
struct CliArguments {
    std::string ip = "127.0.0.1";
    std::string logfile = "";
    std::string configFile;
    bool autorun = false;
    size_t port;
};

int main(int argc, char *argv[]) {
    CliArguments args;
    CLI::App app("DAO Telemetry");
    app.add_option("port", args.port, "Telemetry tool interface port")->required();
    app.add_option("--ip", args.ip, "Telemetry agent ip address");
    app.add_option("-c,--config", args.configFile, "Telemetry session configuration file");
    app.add_option("-l,--logfile", args.logfile, "Log file");
    CLI11_PARSE(app, argc, argv);

    Dao::Log::Logger logger(
        "daoTelemetry",
        args.logfile.length() ? Dao::Log::Logger::DESTINATION::FILE : Dao::Log::Logger::DESTINATION::SCREEN,
        args.logfile
    );
    logger.SetLevel(Dao::Log::LEVEL::DEBUG);

    AppComponent appInterface(logger, args.ip, args.port, args.configFile);
    appInterface.Manage();
}
/* ========================================================== */

