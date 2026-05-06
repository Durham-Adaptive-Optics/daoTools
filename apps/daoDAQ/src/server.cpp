/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-29 10:07:25
 * @ Description: Implementation of application server.
 */

#include <server.hpp>
#include <daoTools.h>

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
            std::this_thread::sleep_for(250ms);

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
                std::make_unique<FileCaptureResource>(policy, [this]() { captureErrorHandler(); })
            );
        }

        for (auto const& policy : policies_->smemPolicies) {
            captureResources_.push_back(
                std::make_unique<SmemCaptureResource>(policy, [this]() { captureErrorHandler(); })
            );
        }
    }

    void Server::startSession()
    {
        std::filesystem::path const sessionDirectory = createSessionGroup();

        for (auto& res : captureResources_) {
            res->beginCapture(sessionDirectory);
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

    /* Callback passed to all capture resources upon their construction
     * enabling them to inform the server of an issue
     * during capture; this triggers an error state whereby the
     * capture session is ended.
    */
    void Server::captureErrorHandler()
    {
        OnFailure(); // @todo does this crash if we run on a capture thread and goto error destroys things?
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
    std::string Server::genGroupTimestamp()
    {
        std::stringstream ss;
        auto const& now = std::chrono::system_clock::now();
        auto const& time = std::chrono::system_clock::to_time_t(now);
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d_%H-%M-%S");
        return ss.str();
    }

    inline void createDirectory(std::filesystem::path const dirPath)
    {
        if (!std::filesystem::create_directory(dirPath)) {
            throw std::runtime_error("failed to create session group directory");
        }
    }

    std::filesystem::path Server::createSessionGroup()
    {
        // create group subdirectory..
        std::string const groupName = policies_->generalPolicies.groupName ? genGroupTimestamp() : policies_->generalPolicies.groupName.value();
        std::filesystem::path const rootPath(policies_->generalPolicies.rootStorage);
        std::filesystem::path const groupPath = rootPath / groupName;
        createDirectory(groupPath);

        // create folder structure..
        for (auto const& smem : policies_->smemPolicies) {
            if (smem.fileRollover) {
                int buffLen {};
                if (DAO_SUCCESS != daoToolsLocalName(smem.absPath.c_str(), nullptr, &buffLen))
                    throw std::runtime_error("failed to extract shm local name length");

                std::string smLocalName(buffLen, '\0');
                if (DAO_SUCCESS != daoToolsLocalName(smem.absPath.c_str(), smLocalName.data(), nullptr))
                    throw std::runtime_error("failed to extract shm local name");

                createDirectory(groupPath / smLocalName);
            }
        }

        //
        return groupPath;
    }
};