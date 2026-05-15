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
#define DEFAULT_LOGFILE     "daoDAQ.logs"
#define DEFAULT_TCP_PORT    62000

/* ---------------------------------------------------------------- */

#include <CLI/CLI.hpp>
#include <server.hpp>
#include <log.hpp>
#include <fstream>
#include <csignal>
#include <string>

/* ---------------------------------------------------------------- */

/* Read in a DAQ configuration from a file and provide it to the DAQ server instance.
 * If doing so fails for whatever reason, a log message is emitted.
*/
void uploadDAQConfig(Dao::DAQ::DAQServer& daqServer, std::string const& filePath, Dao::Log::Logger& log) {
    try {
        if (filePath.length()) {
            std::ifstream daqConfigFile(filePath);
            std::string const daqConfig {
                std::istreambuf_iterator<char>(daqConfigFile),
                std::istreambuf_iterator<char>()
            };
            daqServer.uploadDAQConfig(daqConfig);
        }
    } catch (std::exception const& e) {
        log.Warning(
            LOGFMT("failed to provide initial DAQ configuration to DAQ server because {}", e.what())
        );
    }
}

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

    app.add_flag_callback("--version, -v", []() {
        std::cout << fmt::format("{} {}", APP_NAME, APP_VERSION_TAG) << std::endl;
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
        "--verbose-logging, -l",
        logVerbosityCount,
        "Increase verbosity of logs (default: informative+)"
    );

    CLI11_PARSE(app, argc, argv);

    // setup application logger ..
    auto const logSink = stdoutLogging ? Dao::Log::Logger::DESTINATION::SCREEN : Dao::Log::Logger::DESTINATION::FILE;
    auto const logVerbosity = pickLoggingVerbosity(logVerbosityCount);
    Dao::Log::Logger log(APP_NAME, logSink, DEFAULT_LOGFILE);
    log.SetLevel(logVerbosity);

    // // setup and run the application server ..
    Dao::DAQ::DAQServer daqServer(DEFAULT_TCP_PORT, log);
    if (daqConfigPath.length())
        uploadDAQConfig(daqServer, daqConfigPath, log);

    /* Blocks the application main thread until designated termination signals
     * are received from the OS; at which point the thread is unblocked and
     * the application can proceed to terminate gracefully.
    */
    int signum;
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigset, nullptr);
    sigwait(&sigset, &signum);

    log.Info(LOGFMT("termination signal received ({}); process terminating gracefully", strsignal(signum)));
}

