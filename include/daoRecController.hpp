/**
 * @file    daoRecorderController.h
 * @brief   daoComponent to manage multiple daoRecorders.
 * 
 * @author  T.N Davies
 * 
 * @date    10/01/2025
 */

#ifndef DAO_REC_CONTROLLER_HPP
#define DAO_REC_CONTROLLER_HPP

#include <yaml-cpp/yaml.h>
#include <daoRecorder.hpp>
#include <daoShmIfce.hpp>
#include <daoComponent.hpp>
#include <daoLog.hpp>
#include <vector>

namespace Dao
{
    namespace Telemetry
    {
        class RecorderController : public Component 
        {
        public:
            RecorderController(const std::string &configFilePath, const std::string &recordingRoot, 
                const std::string &ip, const std::size_t port, Log::Logger &logger) 
                :
                Component("RecController", logger, ip, port),
                mRecordingFileCapacity(DAO_REC_NOBINNING),
                mRecordingRoot(recordingRoot),
                mConfigPath(configFilePath),
                mLogger(logger),
                mSharedCore(0)
            {
                mLogger.Trace("RecorderController()");
                mLogger.Debug("Recordings will be stored to %s", mRecordingRoot);
            }

            void transition_Off_Standby() override
            {
                mLogger.Trace("transition_Off_Standby()");

                mConfig = YAML::LoadFile(mConfigPath);
                mSharedCore = mConfig["PeriodicCore"].as<std::size_t>();
                mDedicatedCores = mConfig["RealtimeCores"].as<std::vector<std::size_t>>();
                const auto fileCapacityConfig = mConfig["FileCapacity"];
                if(fileCapacityConfig)
                {
                    mRecordingFileCapacity = fileCapacityConfig.as<std::size_t>();
                }
                
                mLogger.Debug("Configuration loaded");
                mLogger.Debug("Using core %d as shared recording core", mSharedCore);
                mLogger.Debug("Assigned %d cores as dedicated recording cores", mDedicatedCores.size());
            }

            void transition_Standby_Idle() override
            {
                mLogger.Trace("transition_Standby_Idle()");

                for (const auto &recConfig : mConfig["Recorders"])
                {
                    // Parse configuration recorder's data.
                    const std::string &shmPath = recConfig["shm"].as<std::string>();
                    const bool realtime = recConfig["realtime"].as<bool>();

                    // Assign it a core.
                    std::size_t recordingCore = mSharedCore;
                    if(realtime && !mDedicatedCores.size()) {
                        mLogger.Error("No dedicated cores available for recording %s", shmPath.c_str());
                        mLogger.Warning("The data for %s will not be recorded!", shmPath.c_str());
                        continue;
                    }
                    else if (realtime) {
                        recordingCore = mDedicatedCores.back();
                        mDedicatedCores.pop_back();
                    }

                    // Allocate recording object.
                    try {
                        Recorder *recorder = new Recorder(shmPath, mRecordingRoot, recordingCore, mLogger, mRecordingFileCapacity);
                        mRecorders.push_back(recorder);
                        recorder->Spawn();
                    }
                    catch(const std::exception &e) {
                        mLogger.Error("Failed to create recorder for %s - %s", shmPath.c_str(), e.what());
                        mLogger.Warning("The data for %s will not be recorded!", shmPath.c_str());
                        continue;
                    }
                }

                mLogger.Info("%d recorders created", mRecorders.size());
                if (!mRecorders.size()) 
                {
                    mLogger.Warning("No data has been configured to be recorded!");
                }
            }

            void transition_Idle_Running() override
            {
                mLogger.Trace("transition_Idle_Running()");
                for (auto &recorder : mRecorders) { recorder->Start(); }
            }

            void transition_Running_Idle() override
            {
                mLogger.Trace("transition_Running_Idle()");
                for (auto &recorder : mRecorders) { recorder->Stop(); }
            }

            void transition_Idle_Standby() override
            {
                mLogger.Trace("transition_Idle_Standby()");
                for (auto &recorder : mRecorders) { recorder->Join(); }
                mRecorders.clear();
            }

        private:
            std::vector<std::size_t> mDedicatedCores;
            std::size_t mRecordingFileCapacity;
            std::vector<Recorder *> mRecorders; // todo: make more cache friendly by moving to contigouous objects.
            std::string mRecordingRoot;
            std::size_t mSharedCore;
            std::string mConfigPath;
            Log::Logger &mLogger;
            YAML::Node mConfig;
        };

    }; // namespace Telemetry
}; // namespace Dao

#endif