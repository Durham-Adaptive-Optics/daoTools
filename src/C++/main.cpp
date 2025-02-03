#include "daoRecorderController.hpp"
#include "daoRecorder.hpp"
#include "daoLog.hpp"

int main()
{
    Dao::Log::Logger logger("recording-comp", Dao::Log::Logger::DESTINATION::SCREEN);
    logger.SetLevel(Dao::Log::LEVEL::DEBUG);

    Dao::Telemetry::RecorderController recon("127.0.0.1", 8700, logger);
    recon.Init();
    recon.Enable();
    recon.Run();

    while (1)
    {
    }
}