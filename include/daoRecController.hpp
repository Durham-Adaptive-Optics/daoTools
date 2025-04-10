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
            RecorderController(const std::string &configFilePath, const std::string &ip, 
                const std::size_t port, Log::Logger &logger) 
                :
                // todo: sync recording name with main somehow?
                Component("RecController", logger, ip, port),
                mConfigPath(configFilePath),
                mRecordingDirectory("."),
                mRecordingFileCapacity(0),
                mLogger(logger),
                mSharedCore(0),
                mOkay(true)
            {
                mLogger.Trace("RecorderController()");
                mLogger.Debug("Recordings will be stored to %s", mRecordingDirectory.c_str());
            }

            bool isOkay() const { return mOkay; }

        private:
            void PROCESS_OTHER(std::string payload) override
            {
                mLogger.Trace("PROCESS_OTHER");
                mLogger.Debug("Parsing frame targets: ", payload.c_str());

                // todo: in what states should we accept & reject the payload?
                /*
                    We expect the payload to have the following format:

                    Payload: "n1,n2,n3..." or "n"

                    So you can specify the frameCounts for each shm (in the config order)
                    or you can set them all to the same value. Note, we default to 0
                    which means record all frames forever.
                */

                mFrameTargets.clear(); //? What happens if the parsing crashes, now we have no targets!
                std::size_t ridx = 0;
                do {
                    // Extract token substring from payload.
                    const auto delimIdx = payload.find(",", ridx);
                    const std::size_t tokenLen = delimIdx - ridx; //! this isn't always right.
                    const std::string token = payload.substr(delimIdx, tokenLen);
                    mLogger.Debug("Extracted target token: %s", token.c_str());
                    ridx = delimIdx;

                    // Store the desired frame target.
                    const std::size_t frameTarget = std::atoi(token);
                    mFrameTargets.emplace_back(frameTarget);

                } while(delimIdx != std::string::npos);
            }

            void transition_Off_Standby() override
            {
                mLogger.Trace("transition_Off_Standby()");

                // Parse out YAML configuration file.
                try {
                    mConfig = YAML::LoadFile(mConfigPath);
                }
                catch(const YAML::ParserException& e) {
                    mLogger.Error("Failed to parse configuration file: %s", e.what());
                    mOkay = false;
                    return;
                }
                catch(const YAML::BadFile& e) {
                    mLogger.Error("Failed to load configuration file: %s", e.what());
                    mOkay = false;
                    return;
                }

                mSharedCore = mConfig["PeriodicCore"].as<std::size_t>();
                mLogger.Debug("Using core %d as shared recording core", mSharedCore);
                
                mDedicatedCores = mConfig["RealtimeCores"].as<std::vector<std::size_t>>();
                mLogger.Debug("Assigned %d cores as dedicated recording cores", mDedicatedCores.size());

                const auto fileCapacityField = mConfig["FileCapacity"];
                if(fileCapacityField) 
                {
                    mRecordingFileCapacity = fileCapacityField.as<std::size_t>();                
                }

                if(mRecordingFileCapacity)
                {
                    mLogger.Debug("Recorded data will be spread across several FITS files (%d frames / file)", 
                        mRecordingFileCapacity);
                }
                else
                {
                    mLogger.Debug("Recorded data will occupy a single FITS file");
                }

                const auto recordingDirField = mConfig["RecordingDirectory"];
                if(recordingDirField) 
                {
                    mRecordingDirectory = recordingDirField.as<std::string>();
                }
                mLogger.Debug("FITS files will be stored in the directory: %s", mRecordingDirectory.c_str());

                mLogger.Debug("Configuration successfully loaded");
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
                        Recorder *recorder = new Recorder
                        (
                            shmPath, 
                            mRecordingDirectory, 
                            recordingCore, 
                            mRecordingFileCapacity,
                            mTargetFrameCount,
                            mLogger,
                            mOkay
                        );
            
                        mRecorders.push_back(recorder);
                        recorder->Spawn();
                    }
                    catch(const std::exception &e) {
                        mLogger.Error("Failed to create recorder for %s: %s", 
                            shmPath.c_str(), 
                            e.what()
                        );
                        
                        mOkay = false;
                        return;
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
                for (auto &recorder : mRecorders) 
                { 
                    recorder->Join();
                    delete recorder; 
                }
                mRecorders.clear();
            }

            void entry_Error() override
            {
                m_log.Trace("entry_Error()");
                for (auto &recorder : mRecorders) { recorder->Stop(); }
            }

            void entry_Off() override
            {
                m_log.Trace("entry_Off()");
                mOkay = true; // Reset.
            }

            std::vector<std::size_t> mDedicatedCores;
            std::vector<std::size_t> mFrameTargets;
            std::size_t mRecordingFileCapacity;
            std::vector<Recorder *> mRecorders; // TODO: make more cache friendly by moving to contigouous objects.
            std::string mRecordingDirectory;
            std::size_t mSharedCore;
            std::string mConfigPath;
            Log::Logger &mLogger;
            YAML::Node mConfig;
            bool mOkay;
        };

    }; // namespace Telemetry
}; // namespace Dao

#endif