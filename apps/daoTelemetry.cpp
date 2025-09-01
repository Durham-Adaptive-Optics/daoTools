/**
 * @brief   Dao Telemetry Tool
 * @author  T.N Davies
 * @date    15/07/2025
 */

 // todo support dao complex-float & complex-double datatypes (requires table hdu).

#include <daoComponent.hpp>
#include <yaml-cpp/yaml.h>
#include <daoThread.hpp>
#include <sys/types.h>
#include <daoLog.hpp>
#include <filesystem>
#include <sys/stat.h>
#include <CLI11.hpp>
#include <algorithm>
#include <stdint.h>
#include <fitsio.h>
#include <assert.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <atomic>
#include <time.h>
#include <string>
#include <ctime>
#include <dao.h>

 /*--------------------------------------------------------------------------*/
struct collector_t;

struct telemetry_t {
    collector_t *collector = nullptr;     // object handling the collection of this telemetry.
    std::string target;
    size_t capacity;    // if zero all data goes into one file.
    size_t limit;      // if zero we collect until stopped.
    int16_t core;
};

/*--------------------------------------------------------------------------*/

#define FITS_CHECK(expr) \
        (expr); \
        if (status) { \
            char err_msg[FLEN_ERRMSG]; \
            fits_get_errstatus(status, err_msg); \
            m_log.Error("collector for %s experienced an error when recording frame: %s", m_telemetry.target.c_str(), err_msg); \
            m_agent_err_flag = true; \
            return; \
        }

/*--------------------------------------------------------------------------*/

class collector_t : public Dao::Thread {
public:
    collector_t(Dao::Log::Logger &logger, const telemetry_t &t, volatile bool &agent_err_flag)
        : Thread(t.target, logger, t.core), m_telemetry(t), m_logger(logger),
        m_sifce(logger), m_shmname(t.target), m_nfiles(0), m_currfile_sz(0),
        m_agent_err_flag(agent_err_flag), m_fits(nullptr), m_total_frames(0)
    {
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

    /*
        telemetry collection loop.
    */
    void RestartableThread() override {
        // stop collection (if needed)..
        if (m_agent_err_flag || (m_telemetry.limit && m_total_frames >= m_telemetry.limit)) {
            Exit();
            return;
        }

        // retire fits file if reached capacity..
        if (m_fits && m_telemetry.capacity && m_currfile_sz >= m_telemetry.capacity) {
            m_log.Debug("datafile for %s has reached capacity", m_telemetry.target.c_str());
            if (!close_fits()) {
                m_agent_err_flag = true;
                return;
            }
        }

        // collect next telemetry frame (or first, if we've only just started)
        const uint64_t cnt0_ = m_shm_md->cnt0;
        if (cnt0_ > m_cnt0 || !m_total_frames) {
            m_cnt0 = cnt0_;
            memcpy(m_mdbuffer, m_shm.md, sizeof(IMAGE_METADATA)); // copy out metadata to prevent overwrite corruption.
            memcpy(m_databuffer, m_shm.array.V, m_databuffer_sz); // copy out data to prevent overwrite corruption.
            if (m_shm_md->cnt0 > m_cnt0) return; // drop frame, could be corrupted.

            // create fits file if we don't have one..
            if (!m_fits && !new_fits()) {
                m_agent_err_flag = true;
                return;
            }

            // record frame..
            int status = 0;
            FITS_CHECK( fits_create_img(m_fits, m_d2fd.at(m_mdbuffer->atype), m_mdbuffer->naxis, m_axes_sizes.data(), &status) );
            FITS_CHECK( fits_write_key(m_fits, TBYTE, "atype", &m_mdbuffer->atype, nullptr, &status) );
            FITS_CHECK( fits_write_key(m_fits, TLONGLONG, "atime", &m_mdbuffer->atime.tsfixed.secondlong, nullptr, &status) );
            FITS_CHECK( fits_write_key(m_fits, TULONGLONG, "cnt0", &m_mdbuffer->cnt0, nullptr, &status) );
            FITS_CHECK( fits_write_key(m_fits, TULONGLONG, "cnt1", &m_mdbuffer->cnt1, nullptr, &status) );
            FITS_CHECK( fits_write_key(m_fits, TULONGLONG, "cnt2", &m_mdbuffer->cnt2, nullptr, &status) );
            FITS_CHECK( fits_write_img(m_fits, m_d2fs.at(m_mdbuffer->atype), 1, m_mdbuffer->nelement, m_databuffer, &status) );
            FITS_CHECK( fits_write_chksum(m_fits, &status) );

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

    volatile IMAGE_METADATA *m_shm_md;
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

/*--------------------------------------------------------------------------*/

class telemetry_agent_t : public Dao::Component {
public:
    telemetry_agent_t(Dao::Log::Logger &logger, const std::string &ip, size_t port, const std::string &config_file = "")
        :
        Component("telemetry", logger, ip, port),
        m_logger(logger), m_config_string(""), m_err_flag(false) 
    {
        if (config_file.length()) {
            std::ifstream file(config_file);
            if (file) {
                std::ostringstream ss;
                ss << file.rdbuf();
                set_config(ss.str());
            }
            else {
                m_log.Warning("configuration file could not be loaded (%s)", config_file.c_str());
            }
        }
    }

    // Updates agent state.
    void activate() {
        m_logger.Info("telemetry agent active");
        while (true) {
            if (m_err_flag) {
                while (GetStateText() != "Error") OnFailure();
            }
            else if (GetStateText() == "Running") {
                size_t n_exited = 0;
                for (const telemetry_t &t : m_telemetry_list) {
                    if (!t.collector->isRunning()) ++n_exited;
                }
                if (n_exited == m_telemetry_list.size()) {
                    m_logger.Debug("Auto resetting state");
                    while (GetStateText() == "Running") Idle();
                    // todo: just goto idle here to end session?
                    while (GetStateText() == "Idle") Disable();
                    while (GetStateText() == "Standby") Stop();
                }
            }
            sleep(1);
        }
    }

private:
    void configure() {
        if (!m_config_string.length()) {
            throw std::invalid_argument("configuration error: no telemetry session configuration");
        }
        m_logger.Debug("configuring telemetry session..");

        const YAML::Node &config = YAML::Load(m_config_string);
        m_logger.Debug("parsed configuration string");

        if (!config["telemetry_root"]) {
            throw std::invalid_argument("configuration error: no data telemetry directory was specified");
        }
        m_telemetry_root = config["telemetry_root"].as<std::string>();
        m_logger.Info("root directory for telemetry sessions is %s", m_telemetry_root.c_str());

        const int16_t nominal_core = config["nominal_core"] ? config["nominal_core"].as<int16_t>() : -1;
        m_logger.Info("telemetry will be collected on core %d by default", nominal_core);

        if (config["telemetry"]) {
            telemetry_t ti;
            for (const YAML::Node &tc : config["telemetry"]) {
                const size_t telnum = m_telemetry_list.size();
                m_logger.Debug("configuring telemetry-%zu..", telnum);

                if (tc["target"]) {
                    ti.target = tc["target"].as<std::string>();
                    m_logger.Debug("telemetry target: %s", ti.target.c_str());
                }
                else {
                    throw std::invalid_argument("configuration error: no telemetry target specified");
                }

                ti.capacity = tc["capacity"] ? tc["capacity"].as<size_t>() : 0;
                m_logger.Debug("telemetry capacity: %zu", ti.capacity);

                ti.limit = tc["limit"] ? tc["limit"].as<size_t>() : 0;
                m_logger.Debug("telemetry limit: %zu", ti.limit);

                ti.core = tc["core"] ? tc["core"].as<int16_t>() : nominal_core;
                m_logger.Debug("telemetry core: %d", ti.core);

                m_telemetry_list.push_back(ti);

                m_logger.Debug("configured telemetry-%zu (%s)..", telnum, ti.target.c_str());
            }
        }
        else {
            throw std::invalid_argument("configuration error: no telemetry specified");
        }

        if (config["files"]) {
            for (const YAML::Node &fc : config["files"]) {
                const std::string file_path = fc.as<std::string>();
                m_files_list.push_back(file_path);
                m_log.Info("configured %s to be copied when telemetry session starts", file_path.c_str());
            }
        }

        m_logger.Debug("configuration applied");
    }

    void clear_config() {
        m_telemetry_list.clear();
    }
    
    void alloc_recording_resources() {
        for (telemetry_t &t : m_telemetry_list) {
            m_logger.Debug("allocating telemetry collector for %s..", t.target.c_str());
            t.collector = new collector_t(m_logger, t, m_err_flag);
            t.collector->Spawn();
        }
        m_log.Info("recording resources allocated");
    }

    void start_session() {
        // create session dir..
        char timestamp[16];
        time_t t = time(nullptr);
        tm *td = localtime(&t);
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", td);
        const std::string session_dir = m_telemetry_root + "/" + timestamp;
        m_log.Info("creating session directory: %s", session_dir.c_str());
        if (mkdir(session_dir.c_str(), 0755)) {
            throw std::runtime_error("failed to create session directory");
        }

        // copy over desired files..
        for (const std::string &path : m_files_list) {
            // get file name from path..
            std::string filename = path;
            auto x = path.find_last_of("/");
            if (x != std::string::npos) {
                filename = path.substr(x + 1);
            }

            const std::string dst = session_dir + "/" + filename;
            if (!std::filesystem::copy_file(path, dst)) {
                m_log.Error("failed to copy %s to telemetry session directory", path.c_str());
                throw std::runtime_error("failed to copy file");
            }

            m_log.Debug("successfully copied file %s to %s", path.c_str(), dst.c_str());
        }

        // start collector threads..
        for (telemetry_t &t : m_telemetry_list) {
            m_logger.Debug("starting telemetry collector for %s..", t.target.c_str());
            t.collector->set_fsroot(session_dir);
            t.collector->Start();
        }
        
        m_logger.Info("telemetry session started");
    }

    void end_session() {
        for (telemetry_t &t : m_telemetry_list) {
            m_logger.Debug("stopping telemetry collector for %s..", t.target.c_str());
            t.collector->Stop();
        }
        m_logger.Info("telemetry session ended");
    }

    void dealloc_recording_resources() {
        for (telemetry_t &t : m_telemetry_list) {
            if(t.collector) {
                t.collector->Exit();
                t.collector->Join();
                delete t.collector;
            }
        }
        m_log.Info("collectors resources freed");
    }

    /* -------------------------------------------------------------------------------------------- */

    // set configuration string
    void set_config(const std::string &configstr) {
        m_config_string = configstr;
        m_logger.Info("configuration loaded");
    }

    // Receive configuration string
    void PROCESS_OTHER(std::string payload) override { set_config(payload); }

    // Off -> Running
    void transition_Off_Standby() override { configure(); }
    void transition_Standby_Idle() override { alloc_recording_resources(); }
    void transition_Idle_Running() override { start_session(); }

    // Running -> Off
    void transition_Running_Idle() override { end_session(); }
    void transition_Idle_Standby() override { dealloc_recording_resources(); }
    void transition_Standby_Off() override { clear_config(); }

    // Error recovery
    void transition_Error_Idle() override {
        dealloc_recording_resources();
        m_err_flag = false;
        configure();
        alloc_recording_resources();
    }

    /* -------------------------------------------------------------------------------------------- */

    std::vector<telemetry_t> m_telemetry_list;
    std::vector<std::string> m_files_list;
    std::string m_telemetry_root;
    std::string m_config_string;
    Dao::Log::Logger &m_logger;
    volatile bool m_err_flag;
};

/*--------------------------------------------------------------------------*/

struct context_t {
    std::string ip = "127.0.0.1";
    std::string logfile = "";
    std::string configFile;
    bool autorun = false;
    size_t port;
};

int main(int argc, char *argv[]) {
    context_t cxt;
    CLI::App app("DAO Telemetry");
    app.add_option("port", cxt.port, "Telemetry tool interface port")->required();
    app.add_option("--ip", cxt.ip, "Telemetry agent ip address");
    app.add_option("-c,--config", cxt.configFile, "Telemetry session configuration file");
    app.add_option("-l,--logfile", cxt.logfile, "Log file");
    CLI11_PARSE(app, argc, argv);

    Dao::Log::Logger logger(
        "telemetry-agent",
        cxt.logfile.length() ? Dao::Log::Logger::DESTINATION::FILE : Dao::Log::Logger::DESTINATION::SCREEN,
        cxt.logfile
    );
    logger.SetLevel(Dao::Log::LEVEL::DEBUG);

    telemetry_agent_t agent(logger, cxt.ip, cxt.port, cxt.configFile);
    agent.activate();
}