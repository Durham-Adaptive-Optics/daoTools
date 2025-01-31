/******************************************************************************
 * Project:        daoRecorderController
 * Description:    A daoComponent that creates and manages daoRecorders.
 * Author:         Thomas Davies
 * Created:        10/01/2025
 ******************************************************************************/

#ifndef DAO_RECORDER_CONTROLLER_HPP
#define DAO_RECORDER_CONTROLLER_HPP

 // === Includes ===

#include <yaml-cpp/yaml.h>
#include "daoRecorder.hpp"
#include <daoShmIfce.hpp>
#include <daoComponent.hpp>
#include <daoLog.hpp>
#include <vector>

// === Code ===

namespace Dao
{
    namespace Telemetry
    {
        using CorePool = std::vector<std::size_t>;

        //
        class RecorderController : public Component {
        public:
            RecorderController(std::string zmqIP, std::size_t zmqPort, Dao::Log::Logger &logger,
                const std::string &conf_path = "config.yaml") :
                Component("RecordController", logger, zmqIP, zmqPort),
                m_conf_path(conf_path),
                m_periodic_core(0),
                m_log(logger)
            {
                m_log.Trace("Controller-Component constructed");
            }

            void transition_Off_Standby() override
            {
                m_config = YAML::LoadFile(m_conf_path);
                m_periodic_core = m_config["PeriodicCore"].as<std::size_t>();
                m_rt_cores = m_config["RealtimeCores"].as<CorePool>();

                m_log.Debug("Configuration loaded");
            }

            void transition_Standby_Idle() override
            {
                m_log.Trace("On_Enable");

                // Determine where the recording files should be stored.
                const char *rec_root_env = std::getenv("DAO_RECORDINGS_ROOT");
                const std::string rec_root = rec_root_env ? rec_root_env : "";
                m_log.Debug("Recording root set as: %s", rec_root.c_str());

                for (const auto &rec_conf : m_config["Recorders"]) {
                    //
                    const std::string &shm_path = rec_conf["shm"].as<std::string>();
                    const bool realtime = rec_conf["realtime"].as<bool>();

                    // Extract the shm name from the path.
                    auto pos = shm_path.find('.');
                    std::string shm_name = pos == std::string::npos ? shm_path : shm_path.substr(0, pos);
                    std::string FITS_name = rec_root + shm_name;

                    // Determine the optimal core to run the recorder on.
                    std::size_t rec_core = m_periodic_core;
                    if (realtime) {
                        if (m_rt_cores.size()) {
                            rec_core = m_rt_cores.back();
                            m_rt_cores.pop_back();
                        }
                        else {
                            m_log.Error("No realtime cores available to dedicate to %s's recorder - This data will not be recorded!!", shm_name);
                            continue;
                        }
                    }

                    //
                    Recorder *rec = new Recorder(shm_name, shm_path, FITS_name, rec_core, m_log);
                    m_recorders.push_back(rec);
                    rec->Spawn();
                }

                if (m_recorders.size()) {
                    m_log.Info("%d recorders have been created", m_recorders.size());
                }
                else {
                    m_log.Warning("No recorders were created - No data will be recorded!!");
                }
            }

            void transition_Idle_Running() override
            {
                m_log.Trace("On_Run");

                for (auto &rec : m_recorders) {
                    rec->Start();
                }

                const std::string log_msg = m_recorders.size() ? "Recorders started" : "No recorders to start!";
                m_log.Debug(log_msg.c_str());
            }

            void transition_Running_Idle() override
            {
                m_log.Trace("On_Idle");

                for (auto &rec : m_recorders) {
                    rec->Stop();
                }

                const std::string log_msg = m_recorders.size() ? "Recorders stopped" : "No recorders to stop";
                m_log.Debug(log_msg.c_str());
            }

            void transition_Idle_Standby() override
            {
                m_log.Trace("On_Disable");

                for (auto &rec : m_recorders) {
                    rec->Join();
                    delete rec;
                }

                const std::string log_msg = m_recorders.size() ? "Recorders freed" : "No recorders to free";
                m_log.Debug(log_msg.c_str());
            }

        private:
            std::vector<Recorder *> m_recorders;
            std::size_t m_periodic_core;
            std::string m_conf_path;
            CorePool m_rt_cores;
            YAML::Node m_config;
            Log::Logger &m_log;
        };
        
    }; // namespace Telemetry
}; // namespace Dao

#endif