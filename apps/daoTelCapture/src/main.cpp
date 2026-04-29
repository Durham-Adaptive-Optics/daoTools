/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2025-09-12 16:02:50
 * @ Description: Telemetry Capture Tool Entry Point.
 */

#include <string>
#include <CLI/CLI.hpp>
#include <daoLog.hpp>

/**
 * Interface for defining a common recorder object API.
*/
// class Recorder
// {
//     public:
//     virtual ~Recorder() = default;
//     virtual void Start(const std::string& sessionDirectory) {};
//     virtual void Stop() {};
//     virtual bool IsRecording() = 0;
// };

// /**
//  * Struct housing target configuration information
//  * and a reference to its recording object.
//  */
// struct Target
// {
//     // Common target parameters
//     Recorder* recorder;
//     std::string source;

//     enum class Type : std::uint8_t
//     {
//         SHARED_MEMORY,
//         FILE
//     } type;

//     // Shared memory specific parameters
//     size_t fileLimit;
//     size_t bufferLimit;
//     size_t recordingLimit;
//     int16_t pollingCore;

//     // File specific parameters
// };

struct CliArguments
{
    std::string ip = "127.0.0.1";
    std::string logfile = "";
    std::string configFile;
    size_t port;
};

int main(int argc, char* argv[])
{
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
    // AppComponent appInterface(logger, args.ip, args.port, args.configFile);
    // appInterface.Manage();
}
/* ========================================================== */

