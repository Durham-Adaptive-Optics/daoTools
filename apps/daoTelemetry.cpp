/**
 * @brief   DAO Telemetry Tool
 * @author  T.N Davies
 * @date    10/01/2025
 */

#include "include/CLI11.hpp"
#include <yaml-cpp/yaml.h>
#include <daoRecorder.hpp>
#include <daoThread.hpp>
#include <daoShmIfce.hpp>
#include <daoComponent.hpp>
#include <daoLog.hpp>
#include <vector>
#include <atomic>

 /*--------------------------------------------------------------------------*/

struct collector_t;

struct telemetry_t {
    collector_t *collector;
    std::string telemetry_root;
    std::string target;
    size_t capacity;    // if zero all data goes into one file.
    size_t limit;      // if zero we collect until stopped.
    int16_t core;
};

/*--------------------------------------------------------------------------*/

class telemetry_agent_t : public Component {
public:
    telemetry_agent_t(const std::string &ip, size_t port, Log::Logger &logger)
        :
        mLogger(logger),
        Component("telemetry", logger, ip, port),
        mConfigString(""),
        mNumFinished(0),
        mError(false),
    {
        printf("telemetry agent active\n");
    }

    bool error() const { return mError; }

private:
    /* --------------------------------------------------------------- */

    /*  [Off -> Standby]
        Applies the active configuration for the next telemetry session.
    */
    void transition_Off_Standby() override {
        if (!mConfig.len()) {
            throw std::invalid_argument("configuration error: no telemetry session configuration"); //? correct way to exit out?
        }

        printf("configuring telemetry session..\n")
            //
            mConfig = YAML::Load(mConfig);
        printf("successfully parsed configuration\n");

        //
        if (!mConfig["telemetry_root"]) {
            throw std::invalid_argument("configuration error: no data telemetry directory was specified");
        }
        const std::string telemetry_root = mConfig["telemetry_root"].as<std::string>();
        printf("telemetry will be stored to %s", mRecordingDirectory.c_str());

        //
        const int16_t nominal_core = mConfig["nominal_core"] ? mConfig["nominal_core"].as<int16_t>() : 0;
        printf("telemetry will be collected on core %zu by default\n", mStandardCore);

        //
        if (mConfig["telemetry"]) {
            telemetry_t ti;
            for (const YAML::Node &tc : mConfig["telemetry"]) {
                printf("configuring telemetry-%zu..\n", mTelemetry.size());

                if (tc["target"]) {
                    ti.target = tc["target"].as<std::string>();
                    printf("telemetry target: %s\n", ti.targeti.c_str());
                }
                else {
                    throw std::invalid_argument("configuration error: no telemetry target specified\n");
                }

                ti.capacity = tc["capacity"] ? tc["capacity"].as<size_t>() : 0;
                printf("telemetry capacity: %zu\n", ti.capacity);

                ti.limit = tc["limit"] ? tc["limit"].as<size_t>() : 0;
                printf("telemetry limit: %zu\n", ti.limit);

                ti.core = tc["core"] ? tc["core"].as<int16_t>() : nominal_core;
                printf("telemetry core: %zu\n", ti.core.c_str());

                mTelemetry.push_back(t);
            }
        }
        else {
            throw std::invalid_argument("configuration error: no telemetry specified\n");
        }

        printf("telemetry session successfully configured\n");
    }

    /*  [Standby -> Idle]
        Allocates telemetry collectors.
    */
    void transition_Standby_Idle() override {
        printf("allocating telemetry collectors..\n");
        for (telemetry_t &t : mTelemetry) {
            t.collector = new collector_t();
        }
        printf("telemetry collectors successfully allocated\n");
    }

    /*  [Idle -> Running]
        Starts telemetry collection.
    */
    void transition_Idle_Running() override {
        for (telemetry_t &t : mTelemetry) {
            t.collector->start();
        }
        printf("telemetry collection started\n");
    }

    /* --------------------------------------------------------------- */

    /*  [Running -> Idle]
        Stops telemetry collection.
    */
    void transition_Running_Idle() override {
        for (telemetry_t &t : mTelemetry) {
            t.collector->exit();
            t.collector->join();
        }
        printf("telemetry collection stopped\n");
    }

    /*  [Idle -> Standby]
        Frees telemetry collectors.
    */
    void transition_Idle_Standby() override {
        for (telemetry_t &t : mTelemetry) {
            delete t.collector;
        }
        printf("telemetry collection stopped\n");    
    }

    void entry_Standby() override {
        mLogger.Trace("entry_Standby()");
        mNumFinished = 0;
    }

    void entry_Error() override {
        mLogger.Trace("entry_Error()");
        mLogger.Debug("All recording has stopped as an error has occured");
        for (auto &recorder : mRecorders) { recorder->Join(); }
        mError = false;
    }

    void PROCESS_OTHER(std::string payload) override {
        mLogger.Trace("PROCESS_OTHER");

        if (GetStateText() == "Off" && payload.length()) {
            try {
                mGlobalFrameTarget = std::stoi(payload);
                mLogger.Info("New global frame target: %d", mGlobalFrameTarget);
            }
            catch (const std::exception &e) {
                mLogger.Error("Failed to parse frame target: %s", e.what());
            }
        }
    }

    std::vector<std::size_t> mDedicatedCores;
    std::vector<Recorder *> mRecorders;
    std::string mRecordingDirectory;
    std::int64_t mGlobalFrameTarget;
    std::atomic<std::size_t> mNumFinished;
    std::size_t mStandardCore;
    std::string mConfigPath;
    Log::Logger &mLogger;
    std::string mConfig;
    std::vector<telemetry_t> mTelemetry;
    bool mError;
};

/*--------------------------------------------------------------------------*/

struct context_t {
    std::string ip = "127.0.0.1";
    bool fileLogging = false;
    bool autorun = false;
    std::string config_p;
    size_t port;
};

int main(int argc, char *argv[]) {
    //
    context_t cxt;
    CLI::App app("DAO Telemetry");
    app.add_option("CONFIG", cxt.config_p, "Telemetry configuration file")->required();
    app.add_option("PORT", cxt.port, "Telemetry controller port")->required();
    app.add_option("--ip", cxt.ip, "Telemetry controller ip address");
    app.add_flag("-a,--auto-run", cxt.autorun, "Telemetry is automatically recorded upon launch");
    app.add_flag("-f,--file-logging", cxt.fileLogging, "Output logs to file");
    CLI11_PARSE(app, argc, argv);

    //
    Dao::Log::Logger logger(
        "telemetry",
        cxt.fileLogging ? Dao::Log::DESTINATION::FILE : Dao::Log::DESTINATION::SCREEN,
        "daoTelemetryLogs.txt"
    );

    //
    telemetry_agent controller(cxt.config_p, cxt.ip, cxt.port, logger);
    if (cxt.autorun) {
        logger.Info("Automatically starting telemetry session");
        controller.Init();
        controller.Enable();
        controller.Run();
    }

    //
    while (true) {
        if (controller.GetState() != "Error" && controller.error()) {
            while (controller.GetState() != "Error") controller.Error();
        }
        sleep(1);
    }
    // if (GetStateText() == "Running" &&
    //     mNumFinished == mRecorders.size()) {
    //     mLogger.Debug("Controller finish flagged");
    //     Idle();
    //     Disable();
    // }
}