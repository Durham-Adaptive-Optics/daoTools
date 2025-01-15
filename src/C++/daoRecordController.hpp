/******************************************************************************
 * Project:        daoRecordController
 * Description:    A daoComponent that creates and manages daoRecorders.
 * Author:         Thomas Davies
 * Created:        10/01/2025
 ******************************************************************************/

#ifndef DAO_RECORD_CONTROLLER_HPP
#define DAO_RECORD_CONTROLLER_HPP

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
    namespace Recording
    {
        using CorePool = std::vector<std::size_t>;

        //
        class Controller : public Component
        {
        public:
            Controller(std::string zmqIP, std::size_t zmqPort, Dao::Log::Logger &logger, const std::string &conf_path = "config.yaml") : 
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

            void
            transition_Standby_Idle() override
            {
                m_log.Trace("On_Enable");

                // Determine where the recording files should be stored.
                const char *rec_root_env = std::getenv("DAO_RECORDINGS_ROOT");
                std::string rec_root = rec_root_env ? rec_root_env : "";

                for (const auto &rec_conf : m_config["Recorders"])
                {
                    //
                    const std::string &shm_path = rec_conf["shm"].as<std::string>();
                    const bool realtime = rec_conf["realtime"].as<bool>();

                    // Extract the shm name from the path.
                    auto pos = shm_path.find('.');
                    std::string shm_name = pos == std::string::npos ? shm_path : shm_path.substr(0, pos);
                    std::string FITS_path = rec_root + shm_name + ".fits";

                    // Determine the optimal core to run the recorder on.
                    std::size_t rec_core = m_periodic_core;
                    if (realtime)
                    {
                        rec_core = m_rt_cores.back();
                        m_rt_cores.pop_back();
                    }

                    //
                    Recorder *rec = new Recorder(shm_path, FITS_path, rec_core, m_log);
                    m_recorders.push_back(rec);
                    rec->Spawn();
                }

                if (m_recorders.size())
                {
                    m_log.Info("%d recorders have been created", m_recorders.size());
                }
                else
                {
                    m_log.Warning("No recorders were specified - No data will be recorded!!");
                }
            }

            void transition_Idle_Running() override
            {
                m_log.Trace("On_Run");

                if (m_recorders.size())
                {
                    for (auto &rec : m_recorders)
                    {
                        rec->Start();
                    }

                    m_log.Debug("Recorders started");
                }
                else
                {
                    m_log.Debug("No recorders to start");
                }
            }

            void transition_Running_Idle() override
            {
                m_log.Trace("On_Idle");

                if (m_recorders.size())
                {
                    for (auto &rec : m_recorders)
                    {
                        rec->Stop();
                    }

                    m_log.Debug("Recorders stopped");
                }
                else
                {
                    m_log.Debug("No recorders to stop");
                }
            }

            void transition_Idle_Standby() override
            {
                m_log.Trace("On_Disable");

                if (m_recorders.size())
                {
                    for (auto &rec : m_recorders)
                    {
                        rec->Join();
                        delete rec;
                    }

                    m_log.Debug("Recorders freed");
                }
                else
                {
                    m_log.Debug("No recorders to free");
                }
            }

        private:
            std::vector<Recorder *> m_recorders;
            std::size_t m_periodic_core;
            std::string m_conf_path;
            CorePool m_rt_cores;
            YAML::Node m_config;
            Log::Logger &m_log;
        };
    };
};

#endif