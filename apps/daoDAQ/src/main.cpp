/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2025-09-12 16:02:50
 * @ Description: Dao Data Acquisition (DAQ) Software
 */

 /* ---------------------------------------------------------------- */

#define APP_NAME            "daoDAQ"
#define APP_VERSION_TAG     "v3.0.0"
#define DEFAULT_TCP_PORT    62000

/* ---------------------------------------------------------------- */

#include <CLI/CLI.hpp>
#include <server.hpp>
#include <log.hpp>
#include <fstream>
#include <csignal>
#include <string>
#include <cstdlib>

/* ---------------------------------------------------------------- */

#define SAFE_EXIT 166

Dao::Log::LEVEL pickLoggingVerbosity(size_t const& verbosity) {
    if (0 == verbosity) {
        return Dao::Log::LEVEL::INFO;
    }
    if (1 == verbosity) {
        return Dao::Log::LEVEL::DEBUG;
    }
    else {
        return Dao::Log::LEVEL::TRACE;
    }
}

/* ---------------------------------------------------------------- */

int main(int argc, char* argv[]) {
    // cli parsing ..
    std::uint16_t tcpPort { DEFAULT_TCP_PORT };
    std::string daqConfigPath {};
    size_t logVerbosityCount {};
    bool stdoutLogging {};

    CLI::App app("Dao Data Acquisition (DAQ) Software", APP_NAME);

    app.add_flag_callback("--version", []() {
        fmt::print("{} {}\n", APP_NAME, APP_VERSION_TAG);
        throw CLI::Success();
    }, "Print application version");

    app.add_option(
        "--daq-configuration, -c",
        daqConfigPath,
        "Provide initial DAQ session configuration from a file."
    );

    app.add_option(
        "--port, -p",
        tcpPort,
        fmt::format("Specify DAQ server host port (default: {}", DEFAULT_TCP_PORT)
    );

    app.add_flag(
        "--stdout-logging, -s",
        stdoutLogging,
        "Log to standard output instead of a log file."
    );

    app.add_flag(
        "--verbose-logging, -v",
        logVerbosityCount,
        "Increase verbosity of logs (default: INFO)"
    );

    CLI11_PARSE(app, argc, argv);

    auto const DAODATA = std::getenv("DAODATA");
    if (!DAODATA) {
        fmt::print(stderr, "ERROR: DAODATA directory must be present!\n");
        return EXIT_FAILURE;
    }

    /* Blocks the application main thread until designated termination signals
     * are received from the OS; at which point the thread is unblocked and
     * the application can proceed to terminate gracefully.
    */
    {
        auto const logSink = stdoutLogging ? Dao::Log::Logger::DESTINATION::SCREEN : Dao::Log::Logger::DESTINATION::FILE;
        auto const logVerbosity = pickLoggingVerbosity(logVerbosityCount);
        auto const logfilePath = std::filesystem::path(DAODATA) / "daoDAQ.logs";
        Dao::Log::Logger log(APP_NAME, logSink, logfilePath.string());
        log.SetLevel(logVerbosity);

        log.Info("(process) new process started");
        Dao::DAQ::DAQServer daqServer(DEFAULT_TCP_PORT, log);
        if (daqConfigPath.length()) {
            try {
                std::ifstream daqConfigFile(daqConfigPath);
                std::string const daqConfig {
                    std::istreambuf_iterator<char>(daqConfigFile),
                    std::istreambuf_iterator<char>()
                };
                daqServer.uploadDAQConfig(daqConfig);
            } catch (std::exception const& e) {
                log.Error("(process) failed to load config file %s", e.what());
            }
        }

        int signum;
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGINT);
        pthread_sigmask(SIG_BLOCK, &sigset, nullptr);
        log.Info("(process) awaiting termination signal");
        sigwait(&sigset, &signum);
        log.Info(LOGFMT("(process) received termination signal {}", strsignal(signum)));
    }

    return SAFE_EXIT;
}

