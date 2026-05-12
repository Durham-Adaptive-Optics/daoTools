/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-29 15:20:15
 * @ Description:
 */

#pragma once

#include <configuration.hpp>
#include <daoComponent.hpp>
#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <daqs.hpp>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>

namespace Dao::DAQ
{
    class DAQServer : public Dao::Component {
        public:
        DAQServer(std::uint16_t const tcpPort, Dao::Log::Logger& log);

        void run(bool const& runtimeTerminated);
        void uploadDAQConfig(std::string const& ymlPolicyDocument);

        private:
        std::string daqRawConfig_;
        std::unique_ptr<DAQConfiguration> daqConfig_;
        std::vector<std::unique_ptr<IDAQ>> daqResources_;
        std::atomic<size_t> daqResourcesDone_;

        // -- Session Management Methods --
        void applyDAQConfig();
        void prepareDAQResources();
        void startDAQSession();
        void finishDAQSession();
        void freeDAQResources();
        void resetDAQConfig();
        void captureErrorHandler();

        // -- Server API Hooks -- 
        void PROCESS_OTHER(std::string daqRawConfig) override; // @todo update dao API to allow us to use UPDATE instead here - makes more sense.
        void transition_Off_Standby() override;
        void transition_Standby_Idle() override;
        void transition_Idle_Running() override;
        void transition_Running_Idle() override;
        void transition_Idle_Standby() override;
        void transition_Standby_Off() override;
        void transition_Error_Idle() override;
        void transition_Running_Error() override;

        // -- Utility Methods -- 
        std::string timestamp();
        std::filesystem::path prepareOutputDirectory();
    };
};