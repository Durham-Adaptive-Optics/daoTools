/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-29 10:07:25
 * @ Description: Implementation of application server.
 */

#include <server.hpp>

namespace Dao::Telemetry
{
    Server::Server(Dao::Log::Logger& logger, size_t const tcpPort) :
        Component("telCapture", logger, "", tcpPort)
    {
    }

    // -- Session Management Methods --
    void Server::runHousekeeping(bool const& runtimeTerminated)
    {
        using namespace std::chrono_literals;

        while (!runtimeTerminated) {
            std::this_thread::sleep_for(250ms); // rest to not burn core up, this is not high perf.

            if ("Running" == currentState()) {
                size_t numFinished {};
                for (auto const& res : captureResources_) {
                    if (res->targetAchieved())
                        ++numFinished;
                }

                if (numFinished == captureResources_.size())
                    Idle();
            }
        }

        // ensure session is finished and all resources cleaned up
        // before the program exits.
        Idle();
        Disable();
        Stop();
    }

    void Server::storePolicyDocument(std::string const& ymlPolicyDocument)
    {
        if ("Off" == currentState() || "Error" == currentState()) {
            policyDocument_ = ymlPolicyDocument;
        }
        else {
            m_log.Warning("New session policy document was ignored as you must be in Off state first");
            return;
        }
    }

    void Server::loadPolicyFromDocument()
    {
        policies_ = std::make_unique<CapturePolicies>(policyDocument_);
    }

    void Server::createSessionResources()
    {
        for (auto const& policy : policies_->filePolicies) {
            captureResources_.push_back(
                std::make_unique<FileCaptureResource>(policy)
            );
        }

        for (auto const& policy : policies_->smemPolicies) {
            captureResources_.push_back(
                std::make_unique<SmemCaptureResource>(policy)
            );
        }
    }

    void Server::startSession()
    {
        std::filesystem::path sessionOutputDirectory(policies_->generalPolicies.rootStorage);
        if (policies_->generalPolicies.groupingEnabled) {
            sessionOutputDirectory = createSessionGroup();
        }

        for (auto& res : captureResources_) {
            res->beginCapture(sessionOutputDirectory);
        }
    }

    void Server::endSession()
    {
        for (auto& res : captureResources_) {
            res->endCapture();
        }
    }

    void Server::freeSessionResources()
    {
        captureResources_.clear();
    }

    void Server::clearSessionPolicy()
    {
        policies_.reset();
    }

    // -- Server API Hooks -- 
    void Server::PROCESS_OTHER(std::string ymlPolicyDocument) { storePolicyDocument(ymlPolicyDocument); }
    void Server::transition_Off_Standby() { loadPolicyFromDocument(); }
    void Server::transition_Standby_Idle() { createSessionResources(); }
    void Server::transition_Idle_Running() { startSession(); }
    void Server::transition_Running_Idle() { endSession(); }
    void Server::transition_Idle_Standby() { freeSessionResources(); }
    void Server::transition_Standby_Off() { clearSessionPolicy(); }
    void Server::transition_Running_Error() { endSession(); }

    void Server::entry_Error()
    {
        freeSessionResources();
        clearSessionPolicy();
    }

    void Server::transition_Error_Idle()
    {
        loadPolicyFromDocument();
        createSessionResources();
    }

    // -- Utility Methods -- 
    std::string Server::genGroupName()
    {
        std::stringstream ss;
        auto const& now = std::chrono::system_clock::now();
        auto const& time = std::chrono::system_clock::to_time_t(now);
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d_%H-%M-%S");
        return ss.str();
    }

    std::filesystem::path Server::createSessionGroup()
    {
        std::string const groupName = policies_->generalPolicies.groupName ? genGroupName() : policies_->generalPolicies.groupName.value();
        std::filesystem::path const rootPath(policies_->generalPolicies.rootStorage);
        std::filesystem::path const groupPath = rootPath / groupName;

        if (!std::filesystem::create_directory(groupPath)) {
            throw std::runtime_error("failed to create session group directory");
        }

        return groupPath;
    }

};