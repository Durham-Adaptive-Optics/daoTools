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
        Dao::Component("server", log, "", tcpPort),
        daqResourcesDone_(0) {
        //
        m_log.Info(LOGFMT("server endpoint on port {}", m_port));
    }

    DAQServer::~DAQServer() {
        Idle();
        Disable();
        Stop();

        m_log.Debug("server destroyed");
    }

    /* Takes the supplied DAQ YAML configuration string and saves it
     * internally for later use.
    */
    void DAQServer::uploadDAQConfig(std::string const& daqRawConfig) {
        auto const currentStateName = currentState();
        if ("Off" == currentStateName || "Error" == currentStateName) {
            daqRawConfig_ = daqRawConfig;
            m_log.Info("configuration accepted");
        }
        else {
            m_log.Error(LOGFMT("cannot accept configuration when in {} state", currentStateName.c_str()));
        }
    }

    /* Parses the currently saved DAQ configuration string; if an issue occurs during the
     * parse an exception is thrown.
    */
    void DAQServer::applyDAQConfig() {
        daqConfig_ = std::make_unique<DAQConfiguration>(daqRawConfig_, m_log);
        m_log.Info("parsed and applied configuration");
    }

    /* Clears the currently active DAQ configuration; the saved raw
     * DAQ configuration is untouched.
    */
    void DAQServer::resetDAQConfig() {
        daqConfig_.reset();
        m_log.Debug("reset configuration");
    }

    /* Prepares any resources required to carry out DAQ sessions
     * according to the current DAQ configuration.
    */
    void DAQServer::prepareDAQResources() {
        m_log.Trace("allocating DAQs..");

        /* In the event a DAQ resource has finished its capture
         * for the current DAQ session, it will invoke this
         * callback to inform the DAQ server of its completion.
         *
         * Within this callback the server will track how many
         * of the DAQ resources have finished up to that point
         * and in the case all have finished, the server will
         * automatically end the DAQ session.
        */
        auto doneCallback = [&](std::string const& resourceUri) -> void {
            std::thread([this, resourceUri]() {
                std::lock_guard lock(reportLock_);
                ++daqResourcesDone_;

                m_log.Debug(LOGFMT("{} finished acquisition ({} / {} done)", resourceUri, daqResourcesDone_, daqResources_.size()));

                if (daqResourcesDone_ == daqResources_.size()) {
                    m_log.Info(LOGFMT("all daqs have finished acquiring ({})", daqResources_.size()));
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
        auto errorCallback = [&](std::string const& resourceUri, std::string const& err) -> void {
            std::thread([&, resourceUri, err]() {
                std::lock_guard lock(reportLock_);
                m_log.Critical(LOGFMT(
                    "{} encountered aqcuisition error {}",
                    resourceUri,
                    err
                ));
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

        m_log.Debug("DAQs allocated");
    }

    /* Frees any resources that have been created to carry out DAQ sessions.
    */
    void DAQServer::freeDAQResources() {
        m_log.Trace("deallocating daqs..");
        daqResources_.clear();
    }

    /* Prepares a new DAQ session context and informs all DAQ resources
     * to begin capture.
    */
    void DAQServer::startDAQSession() {
        std::filesystem::path const sessionDirectory = prepareOutputDirectory();

        for (auto& res : daqResources_) {
            res->beginAcquisition(sessionDirectory);
        }
    }

    /* Enumerates all DAQ resources and informs them to finish capture
     * of their in-progress DAQ session context.
    */
    void DAQServer::finishDAQSession() {
        daqResourcesDone_ = 0;
        for (auto& res : daqResources_) {
            res->finishAcquisition();
        }
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
        finishDAQSession();
        freeDAQResources();
    }

    void DAQServer::transition_Error_Idle() {
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
            auto const err = fmt::format("failed to create session directory: {} ({})", e.what(), dirPath.string());
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

        m_log.Debug(LOGFMT("created session directory {}", sessionDirectory.string()));

        for (auto const& smem : daqConfig_->smemResources()) {
            if (smem.fileRollover) {
                auto const subdir = sessionDirectory / smem.localName;
                createDirectory(subdir);
                m_log.Debug(LOGFMT("created session sub-directory {}", subdir.string()));
            }
        }

        return sessionDirectory;
    }
};