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
                mRecordingFileCapacity(0),
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

            void transition_Off_Standby() override
            {
                mLogger.Trace("transition_Off_Standby()");

                // Parse out YAML configuration file.
                try {
                    mConfig = YAML::LoadFile(mConfigPath);
                }
                catch (const YAML::ParserException &e) {
                    mLogger.Error("Failed to parse configuration file: %s", e.what());
                    mError = true;
                    return;
                }
                catch (const YAML::BadFile &e) {
                    mLogger.Error("Failed to load configuration file: %s", e.what());
                    mError = true;
                    return;
                }

                mSharedCore = mConfig["PeriodicCore"].as<std::size_t>();
                mLogger.Debug("Using core %d as shared recording core", mSharedCore);

                mDedicatedCores = mConfig["RealtimeCores"].as<std::vector<std::size_t>>();
                mLogger.Debug("Assigned %d cores as dedicated recording cores", mDedicatedCores.size());

                const auto fileCapacityField = mConfig["FileCapacity"];
                if (fileCapacityField) {
                    mRecordingFileCapacity = fileCapacityField.as<std::size_t>();
                }

                if (mRecordingFileCapacity) {
                    mLogger.Debug("Recorded data will be spread across several FITS files (%d frames / file)",
                        mRecordingFileCapacity);
                }
                else {
                    mLogger.Debug("Recorded data will occupy a single FITS file");
                }

                const auto recordingDirField = mConfig["RecordingDirectory"];
                if (recordingDirField) {
                    mRecordingDirectory = recordingDirField.as<std::string>();
                }
                mLogger.Debug("FITS files will be stored in the directory: %s", mRecordingDirectory.c_str());

                mLogger.Debug("Configuration successfully loaded");
            }

            void transition_Standby_Idle() override
            {
                mLogger.Trace("transition_Standby_Idle()");

                // Allocate recorders.
                for (const auto &recConfig : mConfig["Recorders"]) {
                    // Get configuration.
                    const std::string &shmPath = recConfig["shm"].as<std::string>();
                    const bool realtime = recConfig["realtime"].as<bool>();
                    
                    // Assign it a core.
                    std::size_t recordingCore = mSharedCore;
                    if (realtime && !mDedicatedCores.size()) {
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
                        Recorder *recorder = new Recorder
                        (
                            shmPath,
                            mRecordingDirectory,
                            recordingCore,
                            mRecordingFileCapacity,
                            mGlobalFrameTarget,
                            mLogger
                        );

                        mRecorders.push_back(recorder);
                    }
                    catch (const std::exception &e) {
                        mLogger.Error("Failed to create recorder for %s: %s",
                            shmPath.c_str(),
                            e.what()
                        );

                        mError = true;
                        return;
                    }
                }

                if(mRecorders.size()) {
                    mLogger.Info("%d recorders created", mRecorders.size());
                }
                else {
                    mLogger.Warning("No recorders created!");
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
                for (auto &recorder : mRecorders) {
                    recorder->Join();
                    delete recorder;
                }
                mRecorders.clear();
            }

            void entry_Error() override
            {
                mLogger.Trace("entry_Error()");
                mLogger.Debug("Controller has entered error state");
                for (auto &recorder : mRecorders) { recorder->Stop(); }
            }

            void RestartableThread() override
            {
                if (GetStateText() == "Running") {
                    std::size_t nFinished = 0;

                    for (Recorder *recorder : mRecorders) {
                        if (recorder->isRunning()) continue;

                        if (recorder->inError()) {
                            mLogger.Debug("Detected %s's recorder is stopped and in error", recorder->getShmName());
                            OnFailure();
                        }
                        else {
                            mLogger.Debug("Detected %s's recorder has met target", recorder->getShmName().c_str());
                            ++nFinished;
                        }
                    }

                    if (nFinished == mRecorders.size()) {
                        mLogger.Debug("All recorders have met their targets");
                        Idle();
                        Disable();
                    }
                }
                else if(mError) {
                    mLogger.Debug("Controller flagged an error - commanding it into error state");
                    mError = false; // reset.
                    OnFailure();
                }
            }

            std::vector<std::size_t> mDedicatedCores;
            std::size_t mRecordingFileCapacity;
            std::vector<Recorder *> mRecorders;
            std::string mRecordingDirectory;
            std::int64_t mGlobalFrameTarget;
            std::size_t mSharedCore;
            std::string mConfigPath;
            Log::Logger &mLogger;
            YAML::Node mConfig;
            bool mError;
        };

    }; // namespace Telemetry
}; // namespace Dao

#endif