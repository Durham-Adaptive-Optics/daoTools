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
    namespace Telemetry
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
                m_log.Info("Recording data from %s to %s on core %d", shm_path.c_str(), m_rec_path.c_str(), m_core);

                // Open the shm
                m_shm = new ShmIfce<std::uint8_t>(m_log);
                m_shm->OpenShm(m_shm_path.c_str(), &m_img, Dao::Numa::Core2Node(m_core));

                // Create the FITS file.
                m_FITS = new CCfits::FITS(m_rec_path, CCfits::Write);
                CCfits::PHDU &pHDU = m_FITS->pHDU();
                pHDU.addKey("AUTHOR", "CfaI Durham University", "");
                pHDU.addKey("CONTEXT", "DKIST AO Pipeline Data", "");
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
                    //
                    const std::string record_name = "FOO"; // TODO: Put descriptive name here.
                    const int data_type = BYTE_IMG; // TODO: Data type needs to be extracted from shm and converted to FITS types.

                    std::vector<long> dims;
                    for(std::size_t k = 0; k < m_img.md->naxis; ++k){
                        dims.push_back(m_img.md->size[k]);
                    }

                    CCfits::ExtHDU *ext = m_FITS->addImage(record_name, data_type, dims);
                    ext->addKey("ATIME", m_shm->GetTimestamp(), "DAO Shared-Memory Acquisition Time");
                    // TODO: Add data to image extension.
                    const void* data = m_shm->GetPtr();

                    m_shmCnt = cnt;
                }
            }

            Log::Logger &m_log;
            ShmIfce<std::uint8_t> *m_shm;
            std::string m_shm_path;
            std::string m_rec_path;
            std::size_t m_shmCnt;
            CCfits::FITS *m_FITS;
            std::uint8_t m_data_type;
            IMAGE m_img;
        };
    };
};

#endif