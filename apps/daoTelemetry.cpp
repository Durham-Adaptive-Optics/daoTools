/**
 * @brief   Dao Telemetry Tool
 * @author  T.N Davies
 * @date    15/07/2025
 */

#include <CLI11.hpp>
#include <yaml-cpp/yaml.h>
#include <daoThread.hpp>
#include <daoComponent.hpp>
#include <daoShmIfce.hpp>
#include <daoLog.hpp>
#include <stdint.h>
#include <vector>
#include <atomic>
#include <string>
#include <fstream>
#include <sstream>

 /*--------------------------------------------------------------------------*/
struct collector_t;

struct telemetry_t {
    collector_t *collector;     // object handling the collection of this telemetry.
    std::string telemetry_root;
    std::string target;
    size_t capacity;    // if zero all data goes into one file.
    size_t limit;      // if zero we collect until stopped.
    int16_t core;
};

/*--------------------------------------------------------------------------*/

class collector_t : public Dao::Thread {
public:
    collector_t(Dao::Log::Logger &logger, const telemetry_t &t)
        : Thread(t.target, logger, t.core), m_telemetry(t), m_logger(logger), m_sifce(logger) 
    {
        // open shared memory..
        m_sifce.OpenShm(t.target, &m_shm);
        m_shm_md = (volatile IMAGE_METADATA *)m_shm.md;
        if(!m_sifce.IsOpen()) {
            throw std::runtime_error("collector failed to open shared memory");
        }

        // allocate internal data buffer..
        const uint8_t atype = m_shm_md->atype;
        if(m_type_tbl.find(atype) == m_type_tbl.end()) {
            m_logger.Error("telemetry target %s has unknown data type %d", t.target.c_str(), atype);
            throw std::runtime_error("telemetry target has unknown data-type");
        }

        m_databuffer_sz = m_type_tbl.at(atype) * m_shm_md->nelement;
        m_databuffer = malloc(m_databuffer_sz);
        if(!m_databuffer) {
            throw std::runtime_error("collector data-buffer allocation failed");
        }
        m_logger.Debug("collector for %s allocated %zu bytes for data-buffer", t.target.c_str(), m_databuffer_sz);

        // create worker thread..
        Spawn();
    }

    void OnceOnSpawn() override {
        m_logger.Info("%s collector spawned", m_telemetry.target.c_str());
    }

    void OnceOnStart() override {
        m_logger.Info("%s collector started", m_telemetry.target.c_str());
        m_cnt0 = m_shm_md->cnt0;
    }

    /*
        data collection loop.
    */
    void RestartableThread() override {
        const uint64_t cnt0_ = m_shm_md->cnt0;
        if(cnt0_ > m_cnt0) {
            m_cnt0 = cnt0_;
            // todo copy any frame metadata + kwds etc out here.
            memcpy(m_databuffer, m_sifce.GetPtr(), m_databuffer_sz);
            if(m_shm_md->cnt0 > m_cnt0) return; // drop frame, could be corrupted.
            // todo record frame data.
        }
    }

    void OnceOnStop() override {
        m_logger.Info("%s collector stopped", m_telemetry.target.c_str());
    }

    void OnceOnExit() override {
        m_logger.Info("%s collector exited", m_telemetry.target.c_str());
    }
private:
    const std::unordered_map<uint8_t,uint8_t> m_type_tbl { // lookup table from dao data types to byte sizes.
        {_DATATYPE_UINT8, SIZEOF_DATATYPE_UINT8},
        {_DATATYPE_INT8, SIZEOF_DATATYPE_INT8},
        {_DATATYPE_UINT16, SIZEOF_DATATYPE_UINT16},
        {_DATATYPE_INT16, SIZEOF_DATATYPE_INT16},
        {_DATATYPE_UINT32, SIZEOF_DATATYPE_UINT32},
        {_DATATYPE_INT32, SIZEOF_DATATYPE_INT32},
        {_DATATYPE_UINT64, SIZEOF_DATATYPE_UINT64},
        {_DATATYPE_INT64, SIZEOF_DATATYPE_INT64},
        {_DATATYPE_FLOAT, SIZEOF_DATATYPE_FLOAT},
        {_DATATYPE_DOUBLE, SIZEOF_DATATYPE_DOUBLE},
        {_DATATYPE_COMPLEX_FLOAT, SIZEOF_DATATYPE_COMPLEX_FLOAT},
        {_DATATYPE_COMPLEX_DOUBLE, SIZEOF_DATATYPE_COMPLEX_DOUBLE}
    };

    volatile IMAGE_METADATA *m_shm_md;
    const telemetry_t &m_telemetry;
    Dao::ShmIfce<uint8_t> m_sifce;
    Dao::Log::Logger &m_logger;
    size_t m_databuffer_sz;
    void *m_databuffer;
    uint64_t m_cnt0;
    IMAGE m_shm;
};

/*--------------------------------------------------------------------------*/

/*
    oneoff mode: must have limtis set for all targets so we can auto stop.
    service mode: you can have no limit set if you wish - you must stop us then tho.
*/

class telemetry_agent_t : public Dao::Component {
public:
    telemetry_agent_t(Dao::Log::Logger &logger, const std::string &ip, size_t port, const std::string &config_file = "")
        :
        Component("telemetry", logger, ip, port),
        m_config_string(""),
        m_logger(logger) 
    {
        if(config_file.length()) {
            std::ifstream file(config_file);
            std::ostringstream ss;
            ss << file.rdbuf();
            m_config_string = ss.str();
            m_logger.Info("telemetry session configuration set");
        }
        m_logger.Info("telemetry agent active");
    }

private:
    /*
        Accept and apply new YAML telemetry session configuration.
    */
    void PROCESS_OTHER(std::string payload) override {
        m_config_string = payload;
        m_logger.Info("telemetry session configuration set");
    }

    /*  [Off -> Standby]
        Applies the active configuration for the next telemetry session.
    */
    void transition_Off_Standby() override {
        if (!m_config_string.length()) {
            throw std::invalid_argument("configuration error: no telemetry session configuration");
        }
        m_logger.Debug("configuring telemetry session..");

        //
        const YAML::Node &config = YAML::Load(m_config_string);
        m_logger.Debug("parsed configuration string");

        //
        if (!config["telemetry_root"]) {
            throw std::invalid_argument("configuration error: no data telemetry directory was specified");
        }
        const std::string telemetry_root = config["telemetry_root"].as<std::string>();
        m_logger.Info("telemetry will be stored to %s", telemetry_root.c_str());

        //
        const int16_t nominal_core = config["nominal_core"] ? config["nominal_core"].as<int16_t>() : -1;
        m_logger.Info("telemetry will be collected on core %d by default", nominal_core);

        //
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

                m_logger.Info("configured telemetry-%zu (%s)..", telnum, ti.target.c_str());
            }
        }
        else {
            throw std::invalid_argument("configuration error: no telemetry specified");
        }

        m_logger.Debug("telemetry session configured");
    }

    /*  [Standby -> Idle]
        Prepares telemetry session by allocating collectors.
    */
    void transition_Standby_Idle() override {
        for (telemetry_t &t : m_telemetry_list) {
            m_logger.Debug("allocating telemetry collector for %s..", t.target.c_str());
            t.collector = new collector_t(m_logger, t);
        }
        m_logger.Info("telemetry session ready");
    }

    /*  [Idle -> Running]
        Starts telemetry session.
    */
    void transition_Idle_Running() override {
        for (telemetry_t &t : m_telemetry_list) {
            m_logger.Debug("starting telemetry collector for %s..", t.target.c_str());
            t.collector->Start();
        }
        m_logger.Info("telemetry session started");
    }

    /*  [Running -> Idle]
        Ends telemetry session.
    */
    void transition_Running_Idle() override {
        for (telemetry_t &t : m_telemetry_list) {
            m_logger.Debug("ending telemetry collector for %s..", t.target.c_str());
            t.collector->Join();
        }
        m_logger.Info("telemetry session ended");
    }

    /*  [Idle -> Standby]
        Tidies up telemetry session resources by freeing collectors.
    */
    void transition_Idle_Standby() override {
        for (telemetry_t &t : m_telemetry_list) delete t.collector;
        m_telemetry_list.clear();
        m_logger.Info("telemetry session cleaned-up");
    }

    /* ----- Class Members ----- */
    std::vector<telemetry_t> m_telemetry_list;
    std::string m_config_string;
    Dao::Log::Logger &m_logger;
};

/*--------------------------------------------------------------------------*/

struct context_t {
    std::string ip = "127.0.0.1";
    bool fileLogging = false;
    std::string configFile;
    bool autorun = false;
    size_t port;
};

#define DEBUG_

//! todo modify daoBase so statemachine goto error if callback throws - check w/ David for this.
//! todo modify to allow service mode.
//! todo use dao c interface instead of cpp interface to avoid incorrect keywords due to templating as uint8_t? 

int main(int argc, char *argv[]) {
    //
    context_t cxt;
    CLI::App app("DAO Telemetry Agent");
    app.add_option("PORT", cxt.port, "Telemetry agent interface port")->required();
    app.add_option("--ip", cxt.ip, "Telemetry agent ip address");
    app.add_option("--config", cxt.configFile, "Telemetry session configuration file");
    app.add_flag("-a,--auto-run", cxt.autorun, "Auto-start telemetry session upon launch");
    app.add_flag("-f,--file-logging", cxt.fileLogging, "Output logs to file");
    CLI11_PARSE(app, argc, argv);

    //
    Dao::Log::Logger logger(
        "telemetry-agent",
        cxt.fileLogging ? Dao::Log::Logger::DESTINATION::FILE : Dao::Log::Logger::DESTINATION::SCREEN,
        "dao-telemetry-agent.logs" // used if file logging.
    );
    #ifdef DEBUG_
    logger.SetLevel(Dao::Log::LEVEL::DEBUG);
    #endif

    //
    telemetry_agent_t agent(logger, cxt.ip, cxt.port, cxt.configFile);
    if (cxt.autorun) {
        logger.Info("auto-starting telemetry session");
        agent.Init();
        agent.Enable();
        agent.Run();
    }

    //
    while (true) {
        // if (agent.GetState() != "Error" && agent.error()) {
        //     while (agent.GetState() != "Error") agent.Error();
        // }
        sleep(1);
    }
    // if (GetStateText() == "Running" &&
    //     mNumFinished == mRecorders.size()) {
    //     mLogger.Debug("Controller finish flagged");
    //     Idle();
    //     Disable();
    // }
}