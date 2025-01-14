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
#include <daoLog.hpp>
#include <daoShmIfce.hpp>

// === Code ===

namespace Dao
{
    namespace Recording
    {
        //
        class Controller
        {
        public:
            Controller(const std::string &config_path)
                : m_config_path(config_path)
            {
                Dao::Log::Logger log("recorder", Dao::Log::Logger::DESTINATION::SCREEN);
                log.SetLevel(Dao::Log::LEVEL::DEBUG);
    
                //
                YAML::Node config = YAML::LoadFile(config_path);
                CreateRecorders(config);
            }

            ~Controller() 
            {
                // What do we need to do here?
            }

        private:
            // Creates a list of the specified targets within the
            // provided YAML configuration.
            void CreateRecorders(const YAML::Node &config) 
            {
                for (const auto &target_spec : config["targets"]) 
                {
                    // Validate target fields.
                    const auto &shmField = target_spec["shm"];
                    const auto &realtimeField = target_spec["realtime"];
                    if (!shmField) {
                        std::cout << "Target missing shared-memory location\n";
                        continue;
                    }
                    if (!realtimeField) {
                        std::cout << "Target missing realtime specifier\n";
                        continue;
                    }

                    // Create the recorder.
                    const std::string &shm_path = shmField.as<std::string>();
                    const bool isRealtime = realtimeField.as<bool>();
                    
                    std::string recorder_name = shm_path + "-Recorder"; 
                    m_recorders.emplace_back(recorder_name, shm_path, core, m_log);
                }

                std::cout << m_recorders.size() << " recorders created\n";
                if (!m_recorders.size()) {
                    std::cout << "No recorders were defined within the configuration file\n";
                }
            }

            // Member variables.
            std::string m_config_path;
            std::Recorder<Target> m_recorders;
        };
    };
};

#endif