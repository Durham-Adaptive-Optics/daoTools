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
#include <daoComponent.hpp>
#include <yaml-cpp/yaml.h>
#include <CLI/CLI.hpp>
#include <daoLog.hpp>
#include <string>
/* ========================================================== */

/* ==========================================================
                    Recording Interface                         
   ========================================================== */
class IRecorder
{
    public:
    virtual void start(const std::string &sessionDirectory) = 0;
    virtual void stop() = 0;
};

class ShmRecorder : public IRecorder
{
    public:
    ShmRecorder() 
    {

    }

    ~ShmRecorder()
    {

    }

    void start(const std::string &sessionDirectory) override
    {

    }

    void stop() override
    {

    }
};

/* ==========================================================
                    AppComponent Class                         
   ========================================================== */
struct TargetConfiguration
{
    IRecorder *recorder;
    std::string name;
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

class AppComponent : public Dao::Component
{
    public:
    AppComponent(Dao::Log::Logger &logger, const std::string &ip, size_t port, const std::string &configFile = "")
        :
        Component("daoTelemetry", logger, ip, port),
        mConfigString(""),
        mLogger(logger),
        mErrFlag(false)
    {
        mLogger.Info("App interface is now available");

        if (configFile.length()) { // pull initial configuration from a file at startup.
            configFromFile(configFile);
        }
    }

    ~AppComponent()
    {
        //
    }

    void manage()
    {
        mLogger.Debug("State machine monitoring now running");

        //? can we make daoComponentStateMachine nicer for doing this kind of stuff.
        while (true) {
            if (mErrFlag) {
                while (GetStateText() != "Error") OnFailure();
            }
            else if (GetStateText() == "Running") {
                size_t nExited = 0;
                for (const TargetConfiguration &t : mTelemetryList) {
                    assert(false);
                    //! if (!t.collector->isRunning()) ++nExited;
                }
                if (nExited == mTelemetryList.size()) {
                    mLogger.Debug("Auto resetting state");
                    while (GetStateText() == "Running") Idle();
                    while (GetStateText() == "Idle") Disable();
                    while (GetStateText() == "Standby") Stop();
                }
            }
            sleep(1);
        }
    }

    private:
    /* CONFIGURATION */
    void setConfig(const std::string &configStr)
    {
        mConfigString = configStr;
        mLogger.Debug("Configuration set");
    }

    void configFromFile(const std::string &filePath)
    {
        std::ifstream file(filePath);
        if (!file) {
            mLogger.Warning("Configuration file failed to load (%s)", filePath.c_str());
            return;
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        setConfig(ss.str());
    }

    void configuration()
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
            TargetConfiguration telemetryItem;
            for (const YAML::Node &tc : config["telemetry"]) {
                const size_t telemetryNum = mTelemetryList.size();
                mLogger.Debug("configuring telemetry-%zu..", telemetryNum);

                if (tc["target"]) {
                    telemetryItem.name = tc["target"].as<std::string>();
                    mLogger.Debug("telemetry target: %s", telemetryItem.name.c_str());
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

                mTelemetryList.push_back(telemetryItem);

                mLogger.Debug("configured telemetry-%zu (%s)..", telemetryNum, telemetryItem.name.c_str());
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

    void clearConfiguration()
    {
        mTelemetryList.clear();
    }

    /* RECORDING RESOURCE MANAGEMENT */
    void allocateResources()
    {
        for (TargetConfiguration &t : mTelemetryList) {
            mLogger.Debug("allocating telemetry collector for %s..", t.name.c_str());
            t.recorder = new ShmRecorder();
        }
        mLogger.Info("recording resources allocated");
    }

    void deallocateResources()
    {
        for (TargetConfiguration &t : mTelemetryList) {
            delete t.recorder;
        }
        mLogger.Info("collectors resources freed");
    }

    /* SESSION MANAGEMENT */
    void beginSession()
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

        for (TargetConfiguration &t : mTelemetryList) {
            mLogger.Debug("starting telemetry collector for %s..", t.name.c_str());
            t.recorder->start(sessionDir);
        }

        mLogger.Info("telemetry session started");
    }

    void endSession()
    {
        for (TargetConfiguration &t : mTelemetryList) {
            mLogger.Debug("stopping telemetry collector for %s..", t.name.c_str());
            t.recorder->stop();
        }
        mLogger.Info("telemetry session ended");
    }

    /* COMPONENT API OVERLOADS */
    void PROCESS_OTHER(std::string payload) override { setConfig(payload); }
    void transition_Off_Standby() override { configuration(); }
    void transition_Standby_Idle() override { allocateResources(); }
    void transition_Idle_Running() override { beginSession(); }
    void transition_Running_Idle() override { endSession(); }
    void transition_Idle_Standby() override { deallocateResources(); }
    void transition_Standby_Off() override { clearConfiguration(); }

    void transition_Error_Idle()
    {
        deallocateResources();
        mErrFlag = false;
        configuration();
        allocateResources();
    }

    /* MEMBER VARIABLES */
    std::vector<TargetConfiguration> mTelemetryList;
    std::vector<std::string> mFilesList;
    std::string mTelemetryRoot;
    std::string mConfigString;
    Dao::Log::Logger &mLogger;
    volatile bool mErrFlag;
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
    appInterface.manage();
}
/* ========================================================== */


/*
struct collector_t;

struct telemetry_t {
    collector_t *collector = nullptr;     // object handling the collection of this telemetry.
    std::string target;
    size_t capacity;    // if zero all data goes into one file.
    size_t limit;      // if zero we collect until stopped.
    int16_t core;
};

#define FITS_CHECK(expr) \
        (expr); \
        if (status) { \
            char err_msg[FLEN_ERRMSG]; \
            fits_get_errstatus(status, err_msg); \
            m_log.Error("collector for %s experienced an error when recording frame: %s", m_telemetry.target.c_str(), err_msg); \
            m_agent_err_flag = true; \
            return; \
        }

class collector_t : public Dao::Thread {
public:
    collector_t(Dao::Log::Logger &logger, const telemetry_t &t, volatile bool &agent_err_flag)
        : Thread(t.target, logger, t.core), m_telemetry(t), m_logger(logger),
        m_sifce(logger), m_shmname(t.target), m_nfiles(0), m_currfile_sz(0),
        m_agent_err_flag(agent_err_flag), m_fits(nullptr), m_total_frames(0) {
        // open shared memory..
        if (daoShmShm2Img(t.target.c_str(), &m_shm) != DAO_SUCCESS) {
            throw std::runtime_error("collector failed to open shared memory");
        }
        m_shm_md = (volatile IMAGE_METADATA *)m_shm.md;

        // get shared memory name from path..
        auto x = m_shmname.find_last_of('/');
        if (x != std::string::npos) {
            m_shmname = m_shmname.substr(x + 1);
        }
        auto y = m_shmname.find('.');
        if (y != std::string::npos) {
            m_shmname = m_shmname.substr(0, y);
        }
        m_log.Debug("target %s has name: %s", t.target.c_str(), m_shmname.c_str());

        // get data shape..
        for (size_t i = 0; i < m_shm_md->naxis; ++i) m_axes_sizes.push_back(m_shm_md->size[i]);
        std::reverse(m_axes_sizes.begin(), m_axes_sizes.end()); // todo dont need this, do i=n etc 

        // allocate internal data buffer..
        const uint8_t atype = m_shm.md->atype;
        if (m_dt2s.find(atype) == m_dt2s.end()) {
            m_logger.Error("telemetry target %s has unknown data type %d", t.target.c_str(), atype);
            throw std::runtime_error("telemetry target has unknown data-type");
        }

        m_databuffer_sz = m_dt2s.at(atype) * m_shm_md->nelement;
        m_databuffer = malloc(m_databuffer_sz);
        m_mdbuffer = (IMAGE_METADATA *)malloc(sizeof(IMAGE_METADATA));
        if (!m_databuffer) {
            throw std::runtime_error("collector buffer allocation failed");
        }
    }

    ~collector_t() {
        m_sifce.CloseShm();
        free(m_mdbuffer);
        free(m_databuffer);

        DAO_PROFILE_EXPORT(m_profile)
    }

    void set_fsroot(const std::string &fsroot) { m_fsroot = fsroot; }

private:
    bool close_fits() {
        int status = 0;
        fits_close_file(m_fits, &status);
        m_fits = nullptr;

        if (status) {
            char err_msg[FLEN_ERRMSG];
            fits_get_errstatus(status, err_msg);
            m_log.Error("collector for %s experienced an error when closing a datafile: %s", m_telemetry.target.c_str(), err_msg);
        }

        return !status;
    }

    bool new_fits() {
        // figure out new file name..
        const std::string name = m_shmname + "_" + std::to_string(m_nfiles + 1);

        // create file..
        int status = 0;
        std::string fpath = m_fsroot + "/" + name + ".fits";
        m_log.Debug("collector for %s creating new datafile: %s", m_telemetry.target.c_str(), fpath.c_str());

        fits_create_file(&m_fits, fpath.c_str(), &status);
        if (status) {
            char err_msg[FLEN_ERRMSG];
            fits_get_errstatus(status, err_msg);
            m_log.Error("collector for %s experienced an error when creating a datafile: %s", m_telemetry.target.c_str(), err_msg);
        }
        else {
            m_currfile_sz = 0;
            ++m_nfiles;
        }

        return !status;
    }

    void OnceOnStart() override {
        m_cnt0 = m_shm_md->cnt0;
        m_currfile_sz = 0;
        m_nfiles = 0;
    }

    void OnceOnStop() override {
        if (m_fits && !close_fits()) {
            m_agent_err_flag = true;
            return;
        }
    }

    void OnceOnExit() override { m_logger.Debug("%s collector has exited", m_telemetry.target.c_str()); }

    void RestartableThread() override {
        // stop collection (if needed)..
        if (m_agent_err_flag || (m_telemetry.limit && m_total_frames >= m_telemetry.limit)) {
            Exit();
            return;
        }

        // collect next telemetry frame (or first, if we've only just started)
        const uint64_t cnt0_ = m_shm_md->cnt0;
        if (cnt0_ > m_cnt0 || !m_total_frames) {
            DAO_PROFILE_NEW_FRAME(m_profile)

            DAO_PROFILE_START(m_profile, "Copy");
            m_cnt0 = cnt0_;
            memcpy(m_mdbuffer, m_shm.md, sizeof(IMAGE_METADATA)); // copy out metadata to prevent overwrite corruption.
            memcpy(m_databuffer, m_shm.array.V, m_databuffer_sz); // copy out data to prevent overwrite corruption.
            if (m_shm_md->cnt0 > m_cnt0) return; // drop frame, could be corrupted.
            DAO_PROFILE_STOP(m_profile, "Copy");

            // retire fits file if reached capacity..
            DAO_PROFILE_START(m_profile, "FileChange");
            if (m_fits && m_telemetry.capacity && m_currfile_sz >= m_telemetry.capacity) {
                m_log.Debug("datafile for %s has reached capacity", m_telemetry.target.c_str());
                if (!close_fits()) {
                    m_agent_err_flag = true;
                    return;
                }
            }

            // create fits file if we don't have one..
            if (!m_fits && !new_fits()) {
                m_agent_err_flag = true;
                return;
            }
            DAO_PROFILE_STOP(m_profile, "FileChange");

            // record frame..
            int status = 0;
            DAO_PROFILE_START(m_profile, "Export");
            FITS_CHECK(fits_create_img(m_fits, m_d2fd.at(m_mdbuffer->atype), m_mdbuffer->naxis, m_axes_sizes.data(), &status));
            FITS_CHECK(fits_write_key(m_fits, TBYTE, "atype", &m_mdbuffer->atype, nullptr, &status));
            FITS_CHECK(fits_write_key(m_fits, TLONGLONG, "atime", &m_mdbuffer->atime.tsfixed.secondlong, nullptr, &status));
            FITS_CHECK(fits_write_key(m_fits, TULONGLONG, "cnt0", &m_mdbuffer->cnt0, nullptr, &status));
            FITS_CHECK(fits_write_key(m_fits, TULONGLONG, "cnt1", &m_mdbuffer->cnt1, nullptr, &status));
            FITS_CHECK(fits_write_key(m_fits, TULONGLONG, "cnt2", &m_mdbuffer->cnt2, nullptr, &status));
            FITS_CHECK(fits_write_img(m_fits, m_d2fs.at(m_mdbuffer->atype), 1, m_mdbuffer->nelement, m_databuffer, &status));
            DAO_PROFILE_STOP(m_profile, "Export");

            ++m_currfile_sz;
            ++m_total_frames;
        }
    }

    const std::unordered_map<uint8_t, uint8_t> m_dt2s{ // lookup table from dao types to byte sizes.
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

    const std::unordered_map<uint8_t, int> m_d2fd{ // lookup table from dao to fits disk data types.
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

    const std::unordered_map<uint8_t, int> m_d2fs{ // lookup table from dao to fits source data types.
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

    DAO_PROFILE(m_profile, std::chrono::microseconds, "Copy", "FileChange", "Export")
    volatile IMAGE_METADATA * m_shm_md;
    volatile bool &m_agent_err_flag;
    const telemetry_t &m_telemetry;
    Dao::ShmIfce<uint8_t> m_sifce;
    std::vector<long> m_axes_sizes;
    Dao::Log::Logger &m_logger;
    IMAGE_METADATA *m_mdbuffer;
    size_t m_databuffer_sz;
    std::string m_shmname;
    size_t m_total_frames;
    void *m_databuffer;
    fitsfile *m_fits;
    size_t m_currfile_sz;
    std::string m_fsroot;
    uint64_t m_cnt0;
    size_t m_nfiles;
    IMAGE m_shm;
};
*/

