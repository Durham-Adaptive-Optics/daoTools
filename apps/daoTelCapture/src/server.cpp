/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-29 10:07:25
 * @ Description:
 */

#include <daoComponent.hpp>
#include <yaml-cpp/yaml.h>
#include <policies.hpp>
#include <filesystem>
#include <vector>
#include <chrono>

struct CaptureResource {};
struct FileCaptureResource : CaptureResource {};
struct SmemCaptureResource : CaptureResource {};

namespace Dao::Telemetry
{
    class Server : public Dao::Component
    {
        public:

        Server(Dao::Log::Logger& logger, size_t const tcpPort) :
            Component("telCapture", logger, "", tcpPort)
        {
        }

        private:
        std::string policyDocument_;
        std::unique_ptr<CapturePolicies> policies_;
        std::vector<CaptureResource> captureResources_;

        /* Accept new policy document from client; rejected if
         * not presently in the Off state.
        */
        void acceptPolicyDocument(std::string const ymlPolicyDocument)
        {
            if ("Off" != currentState()) {
                m_log.Warning("New session policy document was ignored as you must be in Off state first");
                return;
            }

            policyDocument_ = ymlPolicyDocument;
        }

        /* Processes API::Init client request; accepts the new YAML policy document.
        */
        void PROCESS_OTHER(std::string ymlPolicyDocument) override
        {
            acceptPolicyDocument(ymlPolicyDocument);
        }

        /* Processes API::Init client request;
         * parses the stored YAML policy document into a set of policies,
         * forcing Error state if the parse fails.
        */
        void transition_Off_Standby() override
        {
            policies_ = std::make_unique<CapturePolicies>(policyDocument_);
        }

        /* Processes API::Enable client request;
         * creates resources required for a capture session
         * as configured by the current session policies;
         * forcing Error state if resource creation fails.
        */
        void transition_Standby_Idle() override
        {
            // create file capture resources..
            for (auto const& filePolicy : policies_->filePolicies) {
                FileCaptureResource fileRes(filePolicy);
                captureResources_.push_back(fileRes);
            }

            // create shared-memory capture resources..
            for (auto const& smemPolicy : policies_->smemPolicies) {
                SmemCaptureResource smemRes(smemPolicy);
                captureResources_.push_back(smemRes);
            }
        }

        /* Generate a new output group name given by a timestamp.
        */
        std::string genGroupName()
        {
            std::stringstream ss;
            auto const& now = std::chrono::system_clock::now();
            auto const& time = std::chrono::system_clock::to_time_t(now);
            ss << std::put_time(std::localtime(&time), "%Y-%m-%d_%H-%M-%S");
            return ss.str();
        }

        /* Creates and returns capture session output directory; throws
         * exception if output directory cannot be prepared.
        */
        std::filesystem::path createSessionGroup()
        {
            std::string const groupName = policies_->generalPolicies.groupName ? genGroupName() : policies_->generalPolicies.groupName.value();
            std::filesystem::path const rootPath(policies_->generalPolicies.rootStorage);
            std::filesystem::path const groupPath = rootPath / groupName;

            if (!std::filesystem::create_directory(groupPath)) {
                throw std::runtime_error("failed to create session group directory");
            }

            return groupPath;
        }

        /* Processes API::Run client request; begins the
         * capture session; forces Error state if
         * session fails to begin.
        */
        void transition_Idle_Running() override
        {
            std::filesystem::path sessionOutputDirectory(policies_->generalPolicies.rootStorage);
            if (policies_->generalPolicies.groupingEnabled) {
                sessionOutputDirectory = createSessionGroup();
            }

            for (auto const& capRes : captureResources_) {
                capRes.StartNewCapture(sessionOutputDirectory);
            }
        }

        void transition_Running_Idle() override { EndRecordingSession(); }
        void transition_Idle_Standby() override { DestoryRecordingResources(); }
        void transition_Standby_Off() override { ClearConfiguration(); }
        void transition_Running_Error() { EndRecordingSession(); }
        void transition_Error_Idle() { RecoveryRoutine(); }

        // void Manage()
        // {
        //     m_log.Info("State monitoring active");

        //     while (true) {
        //         if (GetStateText() == "Running") {
        //             // goto error state if recorder(s) have set the error flag.
        //             if (mErrorFlag) {
        //                 while (GetStateText() != "Error") OnFailure();
        //                 continue;
        //             }

        //             // count how many targets have finished recording (if any).
        //             size_t nFinished = 0;
        //             for (const Target& target : mTargets) {
        //                 if (!target.recorder->IsRecording()) nFinished++;
        //             }

        //             // if all targets have finished recording then change state to reflect this.
        //             if (nFinished == mTargets.size()) {
        //                 m_log.Info("All targets finished recording");
        //                 while (GetStateText() == "Running") Idle();
        //             }
        //         }
        //         sleep(1);
        //     }
        // }

        private:
        /**
         * Sets the active YAML configuration string to be used
         * when configuring a recording session.
         * @param configStr Session configuration YAML string.
         */
        void SetConfig(const std::string& configStr)
        {
            mConfigString = configStr;
            m_log.Debug("Configuration set");
        }

        /**
         * Loads YAML configuration from provided file
         * and sets the active configuration string.
         * @param filePath Path to YAML configuration file.
         */
        void ConfigFromFile(const std::string& filePath)
        {
            std::ifstream file(filePath);
            if (!file) {
                m_log.Warning("Configuration file failed to load (%s)", filePath.c_str());
                return;
            }

            std::ostringstream ss;
            ss << file.rdbuf();
            SetConfig(ss.str());
        }

        /**
         * Clears the list of recording targets.
         */
        void ClearConfiguration()
        {
            m_log.Debug("Clearing session configuration..");
            mTargets.clear();
            m_log.Info("Session configuration cleared");
        }

        /**
         * Creates the recording resources required by each
         * target in the list of recording targets.
         */
        void CreateRecordingResources()
        {
            m_log.Debug("Allocating session resources..");
            for (Target& target : mTargets) {
                switch (target.type) {
                    case Target::Type::SHARED_MEMORY:
                        target.recorder = new SharedMemoryRecorder(target, m_log, mErrorFlag);
                        break;

                    case Target::Type::FILE:
                        target.recorder = new FileRecorder(m_log, target.source, mErrorFlag);
                        break;

                    default:
                        m_log.Critical("Target '%s' has unknown source type", target.source.c_str());
                        assert(false);
                        break;
                }
            }

            m_log.Info("Session resources allocated");
        }

        /**
         * Destroys the recording resources held by each
         * target in the list of recording targets.
         */
        void DestoryRecordingResources()
        {
            m_log.Debug("Freeing session resources..");
            for (Target& target : mTargets)
                delete target.recorder;
            m_log.Info("Session resources freed");
        }

        /**
         * Prepares a new recording session. This involves
         * creating a dedicated data directory for the session
         * under the configured root, in-which the session datafiles
         * will be stored. Any file targets are also copied to this
         * session directory at this time. Then the recorder for each
         * target is started - at this point telemetry is being recorded.
         */
        void PrepareRecordingSession()
        {
            //
            m_log.Debug("Starting session..");

            // Create dedicated session directory under the configured root directory
            // to house the session data files that are produced by the recorders.
            char timestamp[16];
            time_t t = time(nullptr);
            tm* td = localtime(&t);
            strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", td);
            const std::string sessionDir = mDataRoot + "/" + timestamp;

            m_log.Debug("Creating session directory: %s", sessionDir.c_str());
            if (mkdir(sessionDir.c_str(), 0755)) {
                m_log.Error("Failed to create session directory because: %s\n", strerror(errno));
                throw std::runtime_error("Failed to create session directory");
            }
            m_log.Info("Created session directory: %s", sessionDir.c_str());



            // Start recorder for each target.
            m_log.Debug("Starting recorders..");
            for (Target& t : mTargets)
                t.recorder->Start(sessionDir);
            m_log.Info("Started recorders");

            //
            m_log.Info("Session started");
        }

        /**
         * Stops telemetry acquisition for each target by stopping
         * its recorder - at this point the session has ended and
         * telemetry is no longer being recorded.
         */
        void EndRecordingSession()
        {
            //
            m_log.Debug("Ending session..");

            m_log.Debug("Stopping recorders..");
            for (Target& t : mTargets)
                t.recorder->Stop();
            m_log.Info("Stopped recorders");

            //
            m_log.Info("Session ended");
        }

        /**
         * Handles resotring application from the Error to the Idle
         * state. This involves resetting the error flag, destroying
         * the recording resources, and re-creating them - at this point
         * recording sessions can now resume.
         */
        void RecoveryRoutine()
        {
            //
            m_log.Debug("Recovering..");

            mErrorFlag = false; // put 1st so any recovery errors are raised correctly. 

            DestoryRecordingResources();
            ClearConfiguration();

            Configure();
            CreateRecordingResources();

            //
            m_log.Info("Recovered");
        }

        /**
         * Component base class overloads. These dispatch to the appropriate
         * application function for the various state machine transitions.
         */
        void PROCESS_OTHER(std::string payload) override { SetConfig(payload); }
        void transition_Off_Standby() override { Configure(); }
        void transition_Standby_Idle() override { CreateRecordingResources(); }
        void transition_Idle_Running() override { PrepareRecordingSession(); }
        void transition_Running_Idle() override { EndRecordingSession(); }
        void transition_Idle_Standby() override { DestoryRecordingResources(); }
        void transition_Standby_Off() override { ClearConfiguration(); }
        void transition_Running_Error() { EndRecordingSession(); }
        void transition_Error_Idle() { RecoveryRoutine(); }

        /**
          * Member Variables
         */
        std::vector<Target> mTargets;
        std::vector<std::string> mFilesList;
        std::string mDataRoot;
        std::string mConfigString;
        volatile bool mErrorFlag;
    };
};