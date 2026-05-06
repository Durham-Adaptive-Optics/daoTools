/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-29 15:20:15
 * @ Description:
 */

#pragma once

#include <captureResource.hpp>
#include <daoComponent.hpp>
#include <yaml-cpp/yaml.h>
#include <policies.hpp>
#include <filesystem>
#include <vector>
#include <chrono>
#include <thread>

namespace Dao::Telemetry
{
    class Server : public Dao::Component
    {
        public:
        Server(Dao::Log::Logger& logger, size_t const tcpPort);

        void runHousekeeping(bool const& runtimeTerminated);
        void storePolicyDocument(std::string const& ymlPolicyDocument);

        private:
        std::string policyDocument_;
        std::unique_ptr<CapturePolicies> policies_;
        std::vector<std::unique_ptr<CaptureResource>> captureResources_;

        // -- Session Management Methods --
        void loadPolicyFromDocument();
        void createSessionResources();
        void startSession();
        void endSession();
        void freeSessionResources();
        void clearSessionPolicy();
        void captureErrorHandler();

        // -- Server API Hooks -- 
        void PROCESS_OTHER(std::string ymlPolicyDocument) override;
        void transition_Off_Standby() override;
        void transition_Standby_Idle() override;
        void transition_Idle_Running() override;
        void transition_Running_Idle() override;
        void transition_Idle_Standby() override;
        void transition_Standby_Off() override;
        void transition_Running_Error();
        void entry_Error() override;
        void transition_Error_Idle() override;

        // -- Utility Methods -- 
        std::string genGroupTimestamp();
        std::filesystem::path createSessionGroup();
    };
};