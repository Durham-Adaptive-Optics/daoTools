/******************************************************************************
 * Project:        daoRecordingTarget
 * Description:    A thread that awaits shared-memory updates and records the new data to disk.
 * Author:         Thomas Davies
 * Created:        10/01/2025
 ******************************************************************************/

#ifndef DAO_RECORDING_TARGET_HPP
#define DAO_RECORDING_TARGET_HPP

// === Includes ===

#include <yaml-cpp/yaml.h>
#include <daoLog.hpp>
#include <daoShmIfce.hpp>
#include <daoNuma.hpp>
#include <daoThread.hpp>
#include <string>
#include <cstdlib>
#include <CCfits/CCfits>

// === Code ===

namespace Dao
{
    namespace Recording
    {
        class Recorder : public Thread
        {
        public:
            Recorder(const std::string shm_path, const std::string rec_path, int core, Log::Logger &logger) : 
                Thread(shm_path, logger, core),
                m_shm_path(shm_path),
                m_rec_path(rec_path),
                m_log(logger),
                m_shmCnt(0)
            {
                //
                m_log.Info("Recording data from %s to %s", shm_path.c_str(), m_rec_path.c_str());

                //
                m_FITS = new CCfits::FITS(m_rec_path, CCfits::Write);

                // Open the shm
                m_shm = new ShmIfce<std::uint8_t>(m_log);
                m_shm->OpenShm(m_shm_path.c_str(), &m_img, Dao::Numa::Core2Node(m_core));
            }

            ~Recorder()
            {
                m_log.Trace("~Recorder");
                m_shm->CloseShm();
                delete m_shm;
                delete m_FITS;
            }

        private:
            void OnceOnStart() override
            {
                m_shmCnt = m_shm->GetFrameCounter();
            };

            void RestartableThread() override
            {
                const auto cnt = m_shm->GetFrameCounter();
                if (cnt > m_shmCnt)
                {
                    m_log.Debug("Data was updated");
                    m_shmCnt = cnt;

                    const std::uint8_t *new_data = m_shm->GetPtr();

                    // Now we need to write the data into the FITS file.
                    // TODO.
                }
            }

            Log::Logger &m_log;
            ShmIfce<std::uint8_t> *m_shm;
            std::string m_shm_path;
            std::string m_rec_path;
            std::size_t m_shmCnt;
            CCfits::FITS *m_FITS;
            IMAGE m_img;
        };
    };
};

#endif