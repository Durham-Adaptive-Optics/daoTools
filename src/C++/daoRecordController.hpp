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
        //
        class Controller : public Component
        {
        public:
            Controller(const std::string &config_path, std::string zmqIP, std::size_t zmqPort, Dao::Log::Logger &logger) : 
                Component("RecordController", logger, zmqIP, zmqPort),
                m_config_path(config_path),
                m_log(logger)
            {
                m_log.Trace("Controller-Component constructed");
            }

            void transition_Off_Standby() override
            {
                m_log.Trace("On_Init");

                try {
                    m_config = YAML::LoadFile(m_config_path);
                    m_log.Debug("Configuration loaded");
                }
                catch(const YAML::BadFile &e) {
                    m_log.Warning("No configuration file was provided: %s", e.what());
                }
                catch(const YAML::ParserException &e) {
                    m_log.Critical("Failed to parse configuration file: %s", e.what());
                }
            }

            void transition_Standby_Idle() override
            {
                m_log.Trace("On_Enable");

                // Determine where the recording files should be stored.
                const char *rec_root_env = std::getenv("DAO_RECORDINGS_ROOT");
                std::string rec_root = rec_root_env ? rec_root_env : "";

                for (const auto &rec_conf : m_config["Recorders"])
                {
                    //
                    const auto &shmField = rec_conf["shm"];
                    const auto &realtimeField = rec_conf["realtime"];

                    // Ensure the recorder's shm & relatime fields were specified.
                    if (!shmField) {
                        m_log.Error("Recorder shared-memory location not specified - Recorder will not be created!");
                        continue;
                    }
                    if (!realtimeField) {
                        m_log.Error("Recorder realtime specifier not provided - Recorder will not be created!");
                        continue;
                    }

                    //
                    const std::string &shm_path = shmField.as<std::string>();
                    const bool isRealtime = realtimeField.as<bool>();

                    // Extract the shm name from the path.
                    auto pos = shm_path.find('.');
                    std::string shm_name = pos == std::string::npos ? shm_path : shm_path.substr(0, pos);
                    std::string FITS_path = rec_root + shm_name + ".fits";

                    // Determine the optimal core to run the recorder on.
                    std::size_t rec_core = 1;

                    //
                    Recorder *rec = new Recorder(shm_path, FITS_path, rec_core, m_log);
                    m_recorders.push_back(rec);
                    rec->Spawn();
                }

                if(m_recorders.size()) {
                    m_log.Info("%d recorders have been created", m_recorders.size());
                }
                else {
                    m_log.Warning("No recorders were specified - No data will be recorded!!");
                }
            }

            void transition_Idle_Running() override
            {
                m_log.Trace("On_Run");
                
                if(m_recorders.size()) {
                    for(auto &rec : m_recorders) {
                        rec->Start();
                    }                

                    m_log.Debug("Recorders started");
                }
                else {
                    m_log.Debug("No recorders to start");
                }
            }

            void transition_Running_Idle() override
            {
                m_log.Trace("On_Stop");

                if(m_recorders.size()) {
                    for(auto &rec : m_recorders) {
                        rec->Stop();
                    }                

                    m_log.Debug("Recorders stopped");
                }
                else {
                    m_log.Debug("No recorders to stop");
                }
            }

            void transition_Idle_Standby() override
            {
                m_log.Trace("On_Disable");

                if(m_recorders.size()) {
                    for(auto &rec : m_recorders) {
                        rec->Join();
                        delete rec;
                    }                

                    m_log.Debug("Recorders freed");
                }
                else {
                    m_log.Debug("No recorders to free");
                }
            }

        private:
            std::vector<Recorder*> m_recorders;
            std::string m_config_path;
            YAML::Node m_config;
            Log::Logger &m_log;
        };
    };
};

#endif