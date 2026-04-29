/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2025-09-12 16:02:50
 * @ Description: Telemetry Capture Tool Entry Point.
 */

#define APP_NAME            "daoTelCapture"
#define APP_VERSION_TAG     "v2.0.0"
#define DEFAULT_TCP_PORT    62000

#include <CLI/CLI.hpp>
#include <daoLog.hpp>
#include <server.hpp>
#include <fstream>
#include <string>

bool terminateRuntime {};

void endme([[maybe_unused]] int signal)
{
    terminateRuntime = true;
}

int main(int argc, char* argv[])
{
    //
    std::uint16_t tcpPort { DEFAULT_TCP_PORT };
    std::string policyFilePath {};
    std::string logsFilePath {};

    CLI::App app("Dao Telemetry Capture Tool", APP_NAME);

    app.add_flag_callback("--version, -v", []() {
        std::cout << "daoTelCapture " << APP_VERSION_TAG << std::endl;
        throw CLI::Success();
    });

    app.add_option(
        "--policy-file, -f",
        policyFilePath,
        "Specify an intial session policy file (default: None)"
    );

    app.add_option(
        "--logs-file, -l",
        logsFilePath,
        "Specify path to file where logs shall be written (default: stdout)"
    );

    app.add_option(
        "--port, -p",
        tcpPort,
        std::string("Specify service host port (default: ") + std::to_string(DEFAULT_TCP_PORT) + ")"
    );

    CLI11_PARSE(app, argc, argv);

    // create logger ..
    auto const logSink = logsFilePath.length() ? Dao::Log::Logger::DESTINATION::FILE : Dao::Log::Logger::DESTINATION::SCREEN;
    Dao::Log::Logger logger(APP_NAME, logSink, logsFilePath);
    logger.SetLevel(Dao::Log::LEVEL::DEBUG);

    // run service ..
    Dao::Telemetry::Server service(logger, DEFAULT_TCP_PORT);

    try {
        if (policyFilePath.length()) {
            std::ifstream policyFile(policyFilePath);
            std::string const fileContents { std::istreambuf_iterator<char>(policyFile), std::istreambuf_iterator<char>() };
            service.storePolicyDocument(fileContents);
        }
    } catch (std::exception const& e) {
        std::string const err { e.what() };
        std::string const msg = "failed to load initial policy file from disk: " + err;
        logger.Error(msg.c_str());
    }

    service.runHousekeeping(terminateRuntime);
}

