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

#define DAO_REC_INDEFINITE -1

#include <yaml-cpp/yaml.h>
#include <daoRecorder.hpp>
#include <daoThread.hpp>
#include <daoShmIfce.hpp>
#include <daoComponent.hpp>
#include <daoLog.hpp>
#include <vector>
#include <atomic>

namespace Dao
{
    namespace Telemetry
    {
        class RecorderController : public Component, Thread {
        public:
            RecorderController(const std::string &configFilePath, const std::string &ip,
                const std::size_t port, Log::Logger &logger)
                :
                Component("RecController", logger, ip, port),
                Thread("RecControllerThread", logger),
                mGlobalFrameTarget(DAO_REC_INDEFINITE),
                mConfigPath(configFilePath),
                mRecordingDirectory("."),
                mNumFinished(0),
                mLogger(logger),
                mError(false),
                mSharedCore(0)
            {
                mLogger.Trace("RecorderController()");
                mLogger.Debug("Recordings will be stored to %s", mRecordingDirectory.c_str());
                
                // Create and start our state-commander thread.
                Spawn();
                Start();
            }

        private:

            void transition_Off_Standby() override
            {
                mLogger.Trace("transition_Off_Standby()");

                try {
                    mConfig = YAML::LoadFile(mConfigPath);

                    mSharedCore = mConfig["PeriodicCore"].as<std::size_t>();
                    mLogger.Debug("Using core %d as shared recording core", mSharedCore);

                    mDedicatedCores = mConfig["RealtimeCores"].as<std::vector<std::size_t>>();
                    mLogger.Debug("Assigned %d cores as dedicated recording cores", mDedicatedCores.size());

                    const auto recordingDirField = mConfig["RecordingDirectory"];
                    if (recordingDirField) {
                        mRecordingDirectory = recordingDirField.as<std::string>();
                    }
                    mLogger.Debug("FITS files will be stored in the directory: %s", mRecordingDirectory.c_str());

                    mLogger.Debug("Configuration successfully loaded");
                }
                catch(const std::exception &e) {
                    mLogger.Critical("Failed to load configuration file: %s", e.what());
                    mError = true;
                    return;
                }
            }

            void transition_Standby_Idle() override
            {
                mLogger.Trace("transition_Standby_Idle()");

                if(mError) {
                    mLogger.Critical("Could not allocate recorders as configuration phase failed");
                    return;
                }

                // Allocate recorders.
                for (const auto &recConfig : mConfig["Recorders"]) {
                    try {
                        // Get configuration.
                        const std::string &shmPath = recConfig["shm"].as<std::string>();
                        const std::size_t capacity = recConfig["capacity"].as<std::size_t>();
                        const bool dynamic = recConfig["dynamic"].as<bool>();

                        mLogger.Debug("Loaded recorder configuration: %s, %d frames per file, %s",
                            shmPath.c_str(),
                            capacity,
                            dynamic ? "dynamic" : "shared"
                        );
                        
                        // Assign it a core.
                        std::size_t recordingCore = mSharedCore;
                        if(dynamic) {
                            if(!mDedicatedCores.size()) {
                                mLogger.Critical("No dedicated cores available for recording %s", shmPath.c_str());
                                mError = true;
                                return;
                            }

                            recordingCore = mDedicatedCores.back();
                            mDedicatedCores.pop_back();
                        }

                        // Allocate recording object.
                        Recorder *recorder = new Recorder
                        (
                            shmPath,
                            mRecordingDirectory,
                            recordingCore,
                            capacity,
                            mGlobalFrameTarget,
                            mError,
                            mNumFinished,
                            mLogger
                        );

                        mRecorders.push_back(recorder);
                    }
                    catch (const std::exception &e) {
                        mLogger.Critical("Failed to allocate recorder for: %s", e.what());
                        mError = true;
                        return;
                    }
                }

                mLogger.Info("%d recorders created", mRecorders.size());
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
                for (auto &recorder : mRecorders) {
                    recorder->Join();
                    delete recorder;
                }
                mRecorders.clear();
            }

            void entry_Standby() override
            {
                mLogger.Trace("entry_Standby()");
                mNumFinished = 0;
            }

            void entry_Error() override
            {
                mLogger.Trace("entry_Error()");
                mLogger.Debug("All recording has stopped as an error has occured");
                for (auto &recorder : mRecorders) { recorder->Join(); }
                mError = false;
            }

            void PROCESS_OTHER(std::string payload) override
            {
                mLogger.Trace("PROCESS_OTHER");

                if (GetStateText() == "Off" && payload.length()) {
                    try {
                        mGlobalFrameTarget = std::stoi(payload); 
                        mLogger.Info("New global frame target: %d", mGlobalFrameTarget);
                    }
                    catch(const std::exception& e) {
                        mLogger.Error("Failed to parse frame target: %s", e.what());
                    }
                }
            }

            void RestartableThread() override
            {
                if(mError) {
                    mLogger.Debug("Controller error flagged");
                    OnFailure();
                }

                if(GetStateText() == "Running" && 
                    mNumFinished == mRecorders.size()) 
                {
                    mLogger.Debug("Controller finish flagged");
                    Idle();
                    Disable();
                }

            }

            std::vector<std::size_t> mDedicatedCores;
            std::vector<Recorder *> mRecorders;
            std::string mRecordingDirectory;
            std::int64_t mGlobalFrameTarget;
            std::atomic<std::size_t> mNumFinished;
            std::size_t mSharedCore;
            std::string mConfigPath;
            Log::Logger &mLogger;
            YAML::Node mConfig;
            bool mError;
        };

    }; // namespace Telemetry
}; // namespace Dao

#endif