/******************************************************************************
 * Project:        daoRecorder
 * Description:    Records data to disk in FITS format from shared-memory sources.
 * Author:         Thomas Davies
 * Created:        10/01/2025
 ******************************************************************************/

#ifndef DAO_RECORDER_HPP
#define DAO_RECORDER_HPP

// === Includes ===

#include <iostream>
#include <vector>
#include <thread>

#include <yaml-cpp/yaml.h>
#include <daoLog.hpp>
#include <daoShmIfce.hpp>

// === Code ===

namespace Dao
{
    namespace Recording
    {
        //
        enum class Type : std::uint8_t
        {
            REALTIME = 0,
            PERIODIC
        };

        struct Target
        {
            std::string data_location;
            Type type;
            std::thread *thread = nullptr;
            bool record = false;
        };

        //
        class Recorder
        {
        public:
            Recorder(const std::string &config_path)
                : m_config_path(config_path)
            {
                Dao::Log::Logger log("recorder", Dao::Log::Logger::DESTINATION::SCREEN);
                log.SetLevel(Dao::Log::LEVEL::DEBUG);
    
                //
                YAML::Node config = YAML::LoadFile(config_path);
                std::cout << "Parsed configuration file\n";
                CreateTargets(config);
            }

            ~Recorder() 
            {
                // Free all target threads.
                for(const auto &target : m_targets)
                {
                    delete target.thread;
                }
            }

        private:
            void Save(Dao::Log::Logger &log)
            {
                log.Debug("Saved data to disk");
            }

            // Waits for the target data to be updated and then saves it off to disk.
            void Record(const Target *target)
            {
                // Each recording thread has its own logger.
                Dao::Log::Logger log(target->data_location, Dao::Log::Logger::DESTINATION::SCREEN);
                log.SetLevel(Dao::Log::LEVEL::DEBUG);

                //
                int numa_cpu_node = -1; 
                switch(target->type) {
                    case Type::PERIODIC {
                        log.Debug("Getting shared NUMA node")
                    }
                    break;

                    case Type::REALTIME: {
                        log.Debug("Getting dedicated NUMA node")
                    }
                    break;
                }

                //
                IMAGE shm;
                Dao::ShmIfce<std::uint8_t*> shm_ifce(log);
                shm_ifce.OpenShm(target->data_location, &shm, numa_cpu_node);

                auto lastCnt = shm.GetFrameCounter();
                while(target->record)
                {
                    auto cnt = shm.GetFrameCounter();
                    if(cnt > lastCnt) {
                        log.Debug("Data was updated");
                        const auto latest_data = shm.GetPtr(); // is this the data?
                        Save(latest_data);
                        lastCnt = cnt;
                    }
                }
            }

            // Creates a list of the specified targets within the
            // provided YAML configuration.
            void CreateTargets(const YAML::Node &config) 
            {
                for (const auto &target_spec : config["targets"]) 
                {
                    // Validate target fields.
                    const auto &shmLocField = target_spec["shm"];
                    const auto &realtimeField = target_spec["realtime"];
                    if (!shmLocField) {
                        std::cout << "Target missing shared-memory location\n";
                        continue;
                    }
                    if (!realtimeField) {
                        std::cout << "Target missing realtime specifier\n";
                        continue;
                    }

                    // Create the target
                    const bool isRealtime = realtimeField.as<bool>();
                    Recording::Target target = {
                        .data_location = shmLocField.as<std::string>(),
                        .type = isRealtime ? Type::REALTIME : Type::PERIODIC,
                        .thread = new std::thread()
                    };

                    //                    
                    m_targets.push_back(target);
                    const std::string typeStr = target.type == Type::REALTIME ? "Realtime" : "Periodic";
                    std::cout << "Target: shm=" << target.data_location << " (" << typeStr << ")\n";
                }

                std::cout << m_targets.size() << " targets configured\n";
                if (!m_targets.size()) {
                    std::cout << "No targets were defined within the configuration file\n";
                }
            }

            // Member variables.
            std::string m_config_path;
            std::vector<Target> m_targets;
        };
    };
};

#endif