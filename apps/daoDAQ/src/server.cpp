/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-29 10:07:25
 * @ Description: DAQ server implementation.
 */

#include <server.hpp>
#include <daoTools.h>
#include <log.hpp>

namespace Dao::DAQ
{
    DAQServer::DAQServer(std::uint16_t const tcpPort, Dao::Log::Logger& log) :
        Dao::Component("DAQServer", log, "", tcpPort),
        daqResourcesDone_(0) {
        //
        m_log.Info(LOGFMT("DAQ server available on port {}", m_port));
    }

    DAQServer::~DAQServer() {
        m_log.Debug("DAQ-Server destroyed");
    }

    /* Takes the supplied DAQ YAML configuration string and saves it
     * internally for later use.
    */
    void DAQServer::uploadDAQConfig(std::string const& daqRawConfig) {
        auto const currentStateName = currentState();
        if ("Off" == currentStateName || "Error" == currentStateName) {
            daqRawConfig_ = daqRawConfig;
            m_log.Info("Successfully uploaded DAQ configuration");
        }
        else {
            m_log.Error(LOGFMT("Could not upload new DAQ configuration due to invalid server state of {}", currentStateName));
        }
    }

    /* Parses the currently saved DAQ configuration string; if an issue occurs during the
     * parse an exception is thrown.
    */
    void DAQServer::applyDAQConfig() {
        m_log.Debug("parsing DAQ configuration..");
        daqConfig_ = std::make_unique<DAQConfiguration>(daqRawConfig_, m_log);
        m_log.Info("DAQ configuration successfully applied");
    }

    /* Clears the currently active DAQ configuration; the saved raw
     * DAQ configuration is untouched.
    */
    void DAQServer::resetDAQConfig() {
        daqConfig_.reset();
    }

    /* Prepares any resources required to carry out DAQ sessions
     * according to the current DAQ configuration.
    */
    void DAQServer::prepareDAQResources() {
        /* In the event a DAQ resource has finished its capture
         * for the current DAQ session, it will invoke this
         * callback to inform the DAQ server of its completion.
         *
         * Within this callback the server will track how many
         * of the DAQ resources have finished up to that point
         * and in the case all have finished, the server will
         * automatically end the DAQ session.
        */
        auto doneCallback = [&](std::string const& resourceID) -> void {
            std::thread([this, resourceID]() {
                std::lock_guard lock(reportLock_);
                ++daqResourcesDone_;

                m_log.Debug(LOGFMT("DAQ resource '{}' finished capture ({} / {} done)", resourceID, daqResourcesDone_, daqResources_.size()));
                if (daqResourcesDone_ == daqResources_.size()) {
                    m_log.Info(LOGFMT("All DAQ resources have finished capturing ({} done)", daqResources_.size()));
                    Idle();
                }
            }).detach();
        };

        /* In the event a DAQ resource has encounters and error
         * during its capture, it will invoke this callback
         * to inform the DAQ server of the issue.
         *
         * Within this callback the server will transition
         * to the Error state.
        */
        auto errorCallback = [&](std::string const& resourceID, std::string const& err) -> void {
            std::thread([&, resourceID, err]() {
                std::lock_guard lock(reportLock_);
                m_log.Critical(LOGFMT("DAQ resource '{}' had session error: {}", resourceID, err));
                OnFailure();
            }).detach();
        };

        for (auto const& config : daqConfig_->fileResources()) {
            daqResources_.push_back(
                std::make_unique<FileDAQ>(config, doneCallback, errorCallback, m_log)
            );
        }

        for (auto const& config : daqConfig_->smemResources()) {
            daqResources_.push_back(
                std::make_unique<SmemDAQ>(config, doneCallback, errorCallback, m_log)
            );
        }

        m_log.Info("DAQ resources have been prepared");
    }

    /* Frees any resources that have been created to carry out DAQ sessions.
    */
    void DAQServer::freeDAQResources() {
        daqResources_.clear();
        m_log.Debug("DAQ resources have been freed");
    }

    /* Prepares a new DAQ session context and informs all DAQ resources
     * to begin capture.
    */
    void DAQServer::startDAQSession() {
        std::filesystem::path const sessionDirectory = prepareOutputDirectory();

        for (auto& res : daqResources_) {
            res->beginAcquisition(sessionDirectory);
            m_log.Debug(LOGFMT("started acquisition for DAQ resource {}", res->resourceID_));
        }
    }

    /* Enumerates all DAQ resources and informs them to finish capture
     * of their in-progress DAQ session context.
    */
    void DAQServer::finishDAQSession() {
        for (auto& res : daqResources_) {
            res->finishAcquisition();
            m_log.Debug(LOGFMT("finished acquisition for DAQ resource {}", res->resourceID_));
        }

        daqResourcesDone_ = 0;
    }

    /* The following methods provide overrides for the inherited component state-machine.
     * They link state hooks to DAQ configuration and session management routines so that
     * the user can configure and operate the DAQ server.
    */
    void DAQServer::PROCESS_OTHER(std::string daqRawConfig) { uploadDAQConfig(daqRawConfig); }
    void DAQServer::transition_Off_Standby() { applyDAQConfig(); }
    void DAQServer::transition_Standby_Idle() { prepareDAQResources(); }
    void DAQServer::transition_Idle_Running() { startDAQSession(); }
    void DAQServer::transition_Running_Idle() { finishDAQSession(); }
    void DAQServer::transition_Idle_Standby() { freeDAQResources(); }
    void DAQServer::transition_Standby_Off() { resetDAQConfig(); }

    void DAQServer::transition_Running_Error() {
        m_log.Info("Resetting DAQ server state before entering Error");
        finishDAQSession();
        freeDAQResources();
    }

    void DAQServer::transition_Error_Idle() {
        m_log.Info("Attempting recovery of DAQ server state");
        applyDAQConfig();
        prepareDAQResources();
    }

    /* Helper method for generating a formatted timestamp for use
     * in naming a new DAQ session output directory.
    */
    std::string DAQServer::timestamp() {
        std::stringstream ss;
        auto const& now = std::chrono::system_clock::now();
        auto const& time = std::chrono::system_clock::to_time_t(now);
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d_%H-%M-%S");
        return ss.str();
    }

    /* Helper method for creating a new filesystem directory; throws
     * an exception if creation fails.
    */
    void createDirectory(std::filesystem::path const dirPath) {
        try {
            if (!std::filesystem::create_directory(dirPath))
                throw std::runtime_error("directory already exists");
        } catch (std::exception const& e) {
            auto const err = fmt::format("failed to create session output directory '{}': {}", dirPath.string(), e.what());
            throw std::runtime_error(err);
        }
    }

    /* Creates and prepares the output directory for a new DAQ session;
     * if an issue is encountered then an exception is raised.
     * @return The absolute path to the freshly prepared directory.
    */
    std::filesystem::path DAQServer::prepareOutputDirectory() {
        std::filesystem::path const rootPath(daqConfig_->sessionParameters().rootStorage);
        std::filesystem::path const sessionDirectory = rootPath / timestamp();
        createDirectory(sessionDirectory);
        m_log.Info(LOGFMT("Created session directory: {}", sessionDirectory.string()));

        for (auto const& smem : daqConfig_->smemResources()) {
            if (smem.fileRollover) {
                auto const subdir = sessionDirectory / smem.localName;
                createDirectory(subdir);
                m_log.Debug(LOGFMT("Created session sub-dir for smem resource '{}': {}", smem.absPath, subdir.string()));
            }
        }

        return sessionDirectory;
    }
};