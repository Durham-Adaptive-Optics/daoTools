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

/* ==========================================================
                    Recording Interface                         
   ========================================================== */
/**
 * Interface for defining a common recorder object API.
*/
class Recorder
{
    public:
    virtual ~Recorder() = default;
    virtual void Start(const std::string &sessionDirectory) = 0;
    virtual bool IsRecording() = 0;
    virtual void Stop() = 0;
};

/* ==========================================================
                            Target                          
   ========================================================== */
/**
 * Struct housing target configuration information
 * and a reference to its recording object.
 */
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
/**
 * Macro for cfitsio error handling.
 */
#define FITS_CHECK(expr)                                                                            \
        (expr);                                                                                     \
        if (fitsError) {                                                                            \
            char err_msg[FLEN_ERRMSG];                                                              \
            fits_get_errstatus(fitsError, err_msg);                                                 \
            m_log.Error("failed to export data (%s): %s", m_thread_name.c_str(), err_msg);          \
            TriggerError();                                                                         \
            return;                                                                                 \
        }

/**
 * Struct housing all shared memory metadata
 * and array data that is to be recorded.
 */
struct SharedMemoryFrame
{
    // The following fields change per shared memory instance.
    std::vector<long> size;
    uint64_t nElements;
    uint8_t atype;

    // The following fields change per shared memory frame.
    int64_t atime;
    uint64_t cnt0;
    uint64_t cnt1;
    uint64_t cnt2;
    int8_t *data;
};

class SharedMemoryPoller : public Dao::Thread
{
    public:
    /**
     * Constructs SharedMemoryPoller, creating a dedicated dao thread
     * for shared memory polling, and loads the target shared memory.
     * @param frameQueue Reference to a shared thread-safe queue where frames will be deposited.
     * @param logger Application logger.
     * @param shmPath Target shared memory path. 
     * @param core Desired core affinity for polling thread.
     */
    SharedMemoryPoller(
        Dao::ThreadSafeQueue<SharedMemoryFrame> &frameQueue, 
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

    /**
     * Destructs SharedMemoryPoller, unloading the shared memory
     * and ensuring the dedicated polling thread is terminated
     * correctly.
     */
    ~SharedMemoryPoller()
    {
        UnloadSharedMemory();
        Exit();
    }

    /**
     * Routine that runs before polling begins to setup 
     * internal state.
     */
    void OnceOnStart() override
    {
        mInitialGrab = true;
    }

    private:
    /**
     * Opens dao shared memory from path and stores required
     * information to be used when queuing frames.
     * @param shmName Shared memory path
     */
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

    /**
     * Ensures the dao shared memory handle is correctly closed.
     */
    void UnloadSharedMemory()
    {
        daoShmCloseShm(&mImage);
    }

    /**
     * Core polling thread loop. Awaits a frame to be written
     * into the shared memory and then queues its data for recording.
     */
    void RestartableThread() override 
    {
        const uint64_t cnt0 = mMetadata->cnt0;
        if (mInitialGrab || cnt0 > mCnt0) {
            //
            mInitialGrab = false;
            mCnt0 = cnt0;
            
            // Copy frame data out of shared memory.
            SharedMemoryFrame frame;
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

    /**
      * Member Variables
    */
    Dao::ThreadSafeQueue<SharedMemoryFrame> &mFrameQueue;
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
    /**
     * Constructs SharedMemoryExporter, creating a dedicated dao thread for exporting
     * queued shared memory frames to FITS data files on disk. 
     * @param frameQueue Reference to a shared thread-safe queue where frames are deposited for recording.
     * @param logger Application logger.
     * @param shmPath Target shared memory path.
     * @param datafileCapacity Desired frame capacity for each FITS data file.
     * @param exportLimit Desired session recording limit for this target.
     * @param errorFlag Application error flag.
     */
    SharedMemoryExporter(
        Dao::ThreadSafeQueue<SharedMemoryFrame> &frameQueue, 
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

    /**
     * Destructs SharedMemoryExporter, ensuring the dedicated export
     * thread is correctly terminated.
     */
    ~SharedMemoryExporter()
    {
        Exit();
    }

    /**
     * Sets the directory to be used for storing target telemetry files.
     * @param directory Directory path.
     */
    void SetStorageDirectory(const std::string &directory)
    {
        mStorageDirectory = directory;
    }

    /**
     * Provides a method for querying if the export thread is actively
     * exporting frames from the queue, or if instead it is waiting to
     * be started. 
     */
    bool IsRecording() const { return mRecording; }

    private:
    /**
     * Sets the application error flag and ensures the export thread
     * is stopped.
     */
    void TriggerError()
    {
        mLogger.Info("%s triggered application error", m_thread_name.c_str());
        mErrorFlag = true;
        Stop();
    }

    /**
     * Handles closing a FITS datafile.
     */
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

    /**
     * Routine that runs before the export thread's core loop
     * begins.
     */
    void OnceOnStart() override
    {
        assert(mStorageDirectory != "");
        mDatafile = nullptr;
        mExportedFrames = 0;
        mDatafileCount = 0;
        mDatafileSize = 0;
        mRecording = true;
    }

    /**
     * Routine that runs after the export thread's core loop
     * is stopped. 
     */
    void OnceOnStop() override
    {
        mRecording = false;
        if(mDatafile && !CloseDatafile())
            TriggerError();
    }

    /**
     * Export thread core loop.
     */
    void RestartableThread() override
    {
        // Automatically stop once we have exported the desired number of frames (if applicable).
        if(mExportLimit && mExportedFrames == mExportLimit) {
            mLogger.Debug("%s reached export limit", m_thread_name.c_str());
            Stop();
            return;
        }

        // Close the current datafile if it has reach capacity (if applicable).
        if(mDatafile && mDatafileCapacity && mDatafileSize == mDatafileCapacity) {
            mLogger.Debug("%s datafile reached capacity (%d/%d)", m_thread_name.c_str(), mDatafileSize, mDatafileCapacity);
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
            mLogger.Debug("Creating datafile %s [%s]", filePath.c_str(), m_thread_name.c_str());
            fits_create_file(&mDatafile, filePath.c_str(), &status);
            
            if (status) {
                char err_msg[FLEN_ERRMSG];
                fits_get_errstatus(status, err_msg);
                mLogger.Error("Failed to create datafile because: %s [%s]", err_msg, m_thread_name.c_str());
                TriggerError();
                return;
            }

            mDatafileSize = 0;
            ++mDatafileCount;
        }

        // Export frame from queue.
        if(mFrameQueue.size()) {
            int fitsError = 0;
            SharedMemoryFrame frame = mFrameQueue.pop();
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
        }
    }

    /**
      * Member Variables
    */
    std::string mShmLocalName;
    Dao::ThreadSafeQueue<SharedMemoryFrame> &mFrameQueue;
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
    /**
     * Constructs SharedMemoryRecorder class, creating exporter
     * and poller objects and spawing their threads.
     * @param target Shared memory target information that is to be recorded by this class.
     * @param logger Application logger.
     * @param errorFlag Application error flag. 
     */
    SharedMemoryRecorder(const Target &target, Dao::Log::Logger &logger, volatile bool &errorFlag)
        :
        mExporter(mFrameQueue, logger, target.path, target.capacity, target.limit, errorFlag),
        mPoller(mFrameQueue, logger, target.path, target.core)
    {
        mExporter.Spawn();
        mPoller.Spawn();
    }
    
    /**
     * Destructs SharedMemoryRecorder class, ensures exporter and poller
     * threads terminate correctly.
     */
    ~SharedMemoryRecorder()
    {
        mExporter.Join();
        mPoller.Join();
    }

    /**
     * Starts data acquisition for the provided shared memory target.
     * @param sessionDirectory Directory path for where data files should be stored.
     */
    void Start(const std::string &sessionDirectory) override
    {
        mExporter.SetStorageDirectory(sessionDirectory);
        mExporter.Start();
        mPoller.Start();
    }

    /**
     * Stops data acquisition for the provided shared memory target.
     */
    void Stop() override
    {
        mExporter.Stop();
        mPoller.Stop();
    }

    /**
     * Provides a method for querying if data acquisition for the shared
     * memory target is in-progress.
     */
    bool IsRecording() override
    {
        return mExporter.IsRecording();
    }

    private:
    /**
      * Member Variables
     */
    Dao::ThreadSafeQueue<SharedMemoryFrame> mFrameQueue;
    SharedMemoryExporter mExporter;
    SharedMemoryPoller mPoller;
};

/* ==========================================================
                    AppComponent Class                         
   ========================================================== */
class AppComponent : public Dao::Component
{
    public:
    /**
     * Constructs AppComponent class, in-turn constructing the network interface thread
     * and loading a default configuration from file if one is provided. 
     * @param logger Dao logger class to be used for logging.
     * @param ip IP address of the application network interface.
     * @param port Port of the application network interface.
     * @param configFile Path to a default configuration file (optional).
     */
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

    /**
     * Destructs AppComponent class.
     */
    ~AppComponent() {}

    /**
     * Periodically polls the current application state and
     * handles invoking the error transition if a recorder
     * has flagged an issue while running. Also handles 
     * transitioning to Idle if all recorders have completed.
     */
    void Manage()
    {
        mLogger.Info("State monitoring active");

        while (true) {
            if (GetStateText() == "Running") {
                // goto error state if recorder(s) have set the error flag.
                if (mErrorFlag) {
                    while (GetStateText() != "Error") OnFailure();
                    continue;
                }

                // count how many targets have finished recording (if any).
                size_t nFinished = 0;
                for(const Target &target : mTargets) {
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
    /**
     * Sets the active YAML configuration string to be used
     * when configuring a recording session.
     * @param configStr Session configuration YAML string.
     */
    void SetConfig(const std::string &configStr)
    {
        mConfigString = configStr;
        mLogger.Debug("Configuration set");
    }

    /**
     * Loads YAML configuration from provided file
     * and sets the active configuration string. 
     * @param filePath Path to YAML configuration file.
     */
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

    /**
     * Parses the active YAML configuration string and
     * populates the list of recording targets to be used
     * in recording sessions.
     */
    void Configure()
    {
        mLogger.Debug("Configuring session..");
        if (!mConfigString.length()) {
            throw std::invalid_argument("No session configuration present");
        }

        const YAML::Node &config = YAML::Load(mConfigString);
        mLogger.Debug("Configuration parsed");

        if (!config["telemetry_root"]) {
            throw std::invalid_argument("No telemetry root specified");
        }
        mTelemetryRoot = config["telemetry_root"].as<std::string>();
        mLogger.Debug("Telemetry root: %s", mTelemetryRoot.c_str());

        int16_t nominalCore = config["nominal_core"] ? config["nominal_core"].as<int16_t>() : -1;
        mLogger.Debug("Nominal polling core: %d", nominalCore);

        // construct a Target struct for each telemetry target in the config
        // and add to the list of session targets. 
        if (config["telemetry"]) {
            for (const YAML::Node &tc : config["telemetry"]) {
                Target target;

                // get target source path.
                if (tc["target"]) target.path = tc["target"].as<std::string>();
                else throw std::invalid_argument("Telemetry item has no source specified");

                // get target datafile capacity.
                target.capacity = tc["capacity"] ? tc["capacity"].as<size_t>() : 0;
                mLogger.Debug("telemetry capacity: %zu", target.capacity);

                // get target recording limit.
                target.limit = tc["limit"] ? tc["limit"].as<size_t>() : 0;
                mLogger.Debug("telemetry limit: %zu", target.limit);

                // get target polling core.
                target.core = tc["core"] ? tc["core"].as<int16_t>() : nominalCore;
                mLogger.Debug("telemetry core: %d", target.core);

                // add to list of session targets.
                mTargets.push_back(target);
                mLogger.Debug("Configured target '%s' (type=shared-memory, capacity=%d,limit=%d,core=%d)", 
                    target.path.c_str(), target.capacity, target.limit, target.core
                );
            }
        }
        else {
            throw std::invalid_argument("No telemetry targets specified");
        }

        // add the filepath for each file listed in the config, these
        // will be copied to the session directory when recording begins.
        if (config["files"]) {
            for (const YAML::Node &fc : config["files"]) {
                const std::string filePath = fc.as<std::string>();
                mFilesList.push_back(filePath);
                mLogger.Debug("Configured target '%s' (type=file)", filePath.c_str());
            }
        }

        mLogger.Info("Session configured");
    }

    /**
     * Clears the list of recording targets.
     */
    void ClearConfiguration()
    {
        mLogger.Debug("Clearing session configuration..");
        mTargets.clear();
        mLogger.Info("Session configuration cleared");
    }

    /**
     * Creates the recording resources required by each 
     * target in the list of recording targets.
     */
    void CreateRecordingResources()
    {
        mLogger.Debug("Allocating session resources..");
        for (Target &target : mTargets) 
            target.recorder = new SharedMemoryRecorder(target, mLogger, mErrorFlag);
        mLogger.Info("Session resources allocated");
    }

    /**
     * Destroys the recording resources held by each 
     * target in the list of recording targets.
     */
    void DestoryRecordingResources()
    {
        mLogger.Debug("Freeing session resources..");
        for (Target &target : mTargets) 
            delete target.recorder;
        mLogger.Info("Session resources freed");
    }

    /**
     * Prepares a new recording session. This involves
     * creating a dedicated data directory for the session
     * under the configured root, in-which the session datafiles
     * will be stored. Any file targets are also copied to this
     * session directory at this time. Then the recorder for each
     * target is started - at this point telemetry is being recorded.
     */
    void PrepareRecordingSession()
    {
        //
        mLogger.Debug("Starting session..");

        // Create dedicated session directory under the configured root directory
        // to house the session data files that are produced by the recorders.
        char timestamp[16];
        time_t t = time(nullptr);
        tm *td = localtime(&t);
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", td);
        const std::string sessionDir = mTelemetryRoot + "/" + timestamp;
        
        mLogger.Debug("Creating session directory: %s", sessionDir.c_str());
        if (mkdir(sessionDir.c_str(), 0755)) {
            mLogger.Error("Failed to create session directory because: %s\n", strerror(errno));
            throw std::runtime_error("Failed to create session directory");
        }
        mLogger.Info("Created session directory: %s", sessionDir.c_str());

        // Copy over each file target into the session directory. 
        for (const std::string &path : mFilesList) {
            // construct file's full destination path. 
            std::string filename = path;
            auto x = path.find_last_of("/");
            if (x != std::string::npos) {
                filename = path.substr(x + 1);
            }
            const std::string destPath = sessionDir + "/" + filename;

            // copy file to destination.
            mLogger.Debug("Copying file target %s to %s", path.c_str(), destPath.c_str());
            std::filesystem::copy_file(path, destPath); // throws if the copy fails.
            mLogger.Info("Copied file target %s to %s", path.c_str(), destPath.c_str());
        }

        // Start recorder for each target.
        mLogger.Debug("Starting recorders..");
        for (Target &t : mTargets) 
            t.recorder->Start(sessionDir);
        mLogger.Info("Started recorders");

        //
        mLogger.Info("Session started");
    }

    /**
     * Stops telemetry acquisition for each target by stopping
     * its recorder - at this point the session has ended and
     * telemetry is no longer being recorded.
     */
    void EndRecordingSession()
    {
        //
        mLogger.Debug("Ending session..");

        mLogger.Debug("Stopping recorders..");
        for (Target &t : mTargets) 
            t.recorder->Stop();
        mLogger.Info("Stopped recorders");

        //
        mLogger.Info("Session ended");
    }

    /**
     * Handles resotring application from the Error to the Idle
     * state. This involves resetting the error flag, destroying
     * the recording resources, and re-creating them - at this point
     * recording sessions can now resume. 
     */
    void RecoveryRoutine()
    {
        //
        mLogger.Debug("Recovering..");
        
        mErrorFlag = false; // put 1st so any recovery errors are raised correctly. 
        DestoryRecordingResources();
        CreateRecordingResources();

        //
        mLogger.Info("Recovered");
    }

    /**
     * Component base class overloads. These dispatch to the appropriate
     * application function for the various state machine transitions. 
     */
    void PROCESS_OTHER(std::string payload) override { SetConfig(payload); }
    void transition_Off_Standby() override { Configure(); }
    void transition_Standby_Idle() override { CreateRecordingResources(); }
    void transition_Idle_Running() override { PrepareRecordingSession(); }
    void transition_Running_Idle() override { EndRecordingSession(); }
    void transition_Idle_Standby() override { DestoryRecordingResources(); }
    void transition_Standby_Off() override { ClearConfiguration(); }
    void transition_Running_Error() { EndRecordingSession(); }
    void transition_Error_Idle() { RecoveryRoutine(); }

    /**
      * Member Variables
     */
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
/**
 * Stores values of application command-line arguments.
 */
struct CliArguments {
    std::string ip = "127.0.0.1";
    std::string logfile = "";
    std::string configFile;
    size_t port;
};

int main(int argc, char *argv[]) {
    // Parse CLI.
    CliArguments args;
    CLI::App app("DAO Telemetry");
    app.add_option("port", args.port, "Telemetry tool interface port")->required();
    app.add_option("--ip", args.ip, "Telemetry agent ip address");
    app.add_option("-c,--config", args.configFile, "Telemetry session configuration file");
    app.add_option("-l,--logfile", args.logfile, "Log file");
    CLI11_PARSE(app, argc, argv);

    // Create application logger.
    Dao::Log::Logger logger(
        "daoTelemetry",
        args.logfile.length() ? Dao::Log::Logger::DESTINATION::FILE : Dao::Log::Logger::DESTINATION::SCREEN,
        args.logfile
    );
    logger.SetLevel(Dao::Log::LEVEL::DEBUG);

    // Create application network interface and start state management loop.
    AppComponent appInterface(logger, args.ip, args.port, args.configFile);
    appInterface.Manage();
}
/* ========================================================== */

