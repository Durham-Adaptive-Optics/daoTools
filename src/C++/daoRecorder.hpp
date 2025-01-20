/******************************************************************************
 * Project:        daoRecordingTarget
 * Description:    A thread that awaits shared-memory updates and records the
 *new data to disk. Author:         Thomas Davies Created:        10/01/2025
 ******************************************************************************/

#ifndef DAO_RECORDING_TARGET_HPP
#define DAO_RECORDING_TARGET_HPP

 // === Includes ===

#include <CCfits/CCfits>
#include <cstdlib>
#include <daoLog.hpp>
#include <daoNuma.hpp>
#include <daoShmIfce.hpp>
#include <daoThread.hpp>
#include <string>
#include <yaml-cpp/yaml.h>

/*
    By default, the recorder object bins every N seconds of recorded
    data into seperate FITS files. If instead you require all data
    recorded to be saved into a single FITS file, please define the
    following preprocessor symbol.

    #define DAO_RECORDER_DISABLE_BINNING
*/

// === Code ===

namespace Dao
{
    namespace Telemetry
    {
        class Recorder : public Thread {
        public:
            Recorder(const std::string &name, const std::string &shm_path, const std::string &rec_path, int core,
                Log::Logger &logger, std::size_t bin_capacity = 30) :
                Thread(name, logger, core), m_shm_path(shm_path),
                m_rec_path(rec_path),
                m_log(logger),
                m_shmCnt(0),
                m_data_type(0),
                m_bin_capacity(bin_capacity),
                m_bin_count(0),
                m_frameBeingRecorded(0)
            {
                //
                m_log.Info("Recording data from %s to %s on core %d", shm_path.c_str(),
                    m_rec_path.c_str(), m_core);

                // Open the time-keeping shm
                m_tsShm = new ShmIfce<float>(m_log);
                m_tsShm->OpenShm("ts.im.shm", &m_tsImg, Dao::Numa::Core2Node(m_core));

                // Open the target shm
                m_shm = new ShmIfce<std::uint8_t>(m_log);
                m_shm->OpenShm(m_shm_path.c_str(), &m_img, Dao::Numa::Core2Node(m_core));

                //
                InferFITSDatatype();
                GetDataDimensionality();
                CreateFITSBin();
            }

            ~Recorder()
            {
                m_log.Trace("~Recorder");
                m_shm->CloseShm();
                delete m_shm;
                delete m_bin.writer;
            }

        private:

            void OnceOnStart() override { m_shmCnt = m_shm->GetFrameCounter(); };

            void RestartableThread() override
            {
                const auto cnt = m_shm->GetFrameCounter();
                if (cnt > m_shmCnt) {
                    OnDataUpdate();
                    m_shmCnt = cnt;
                }
            }

            void CreateFITSBin()
            {
                delete m_bin.writer;

                std::string bin_name = m_rec_path;
#ifndef DAO_RECORDER_DISABLE_BINNING
                bin_name += "-" + std::to_string(m_bin_count);
#endif
                bin_name += ".fits";

                m_bin.writer = new CCfits::FITS(bin_name, m_data_type, m_data_dims.size(), m_data_dims.data());
                m_bin.has_origin = false;
                ++m_bin_count;
            }

            void OnDataUpdate()
            {
                // Check if we have missed any frames.
                const auto frame_tracker = m_tsShm->GetFrameCounter();
                const auto num_frames_catchup = frame_tracker - m_frameBeingRecorded;
                if(num_frames_catchup > 1) {
                    m_log.Warning("Missed %d frames", num_frames_catchup);
                }
                m_frameBeingRecorded = frame_tracker;


                // Ensure we have the correct bin ready to receive data.
                const auto ts_curr = *m_tsShm->GetPtr();
#ifndef DAO_RECORDER_DISABLE_BINNING
                const auto elapsed = (ts_curr - m_bin.origin) / 1e9; // seconds.
                std::cout << elapsed << std::endl;
                if (elapsed >= m_bin_capacity) {
                    CreateFITSBin();
                }
#endif

                if (!m_bin.has_origin) {
                    m_bin.origin = ts_curr;
                    m_bin.has_origin = true;
                }

                // Record the data.
                const auto bytes_per_element = (m_data_type >= 0 ? m_data_type : -m_data_type) / 8;
                std::size_t bytes = bytes_per_element * m_img.md->nelement;
                std::valarray<std::uint8_t> data_array(m_shm->GetPtr(), bytes);
                CCfits::ExtHDU *ext = m_bin.writer->addImage(
                    std::to_string(ts_curr),
                    m_data_type,
                    m_data_dims
                );
                ext->write(1, bytes, data_array);
                ext->writeChecksum();
            }

            void GetDataDimensionality()
            {
                m_data_dims.reserve(3);
                for (std::size_t k = 0; k < m_img.md->naxis; ++k) {
                    m_data_dims.push_back(m_img.md->size[k]);
                }
            }

            void InferFITSDatatype()
            {
                switch (m_img.md->atype) {
                    case 1: // uint8_t
                    case 2: // sint8_t
                    {
                        m_data_type = BYTE_IMG;
                    }
                    break;

                    case 3: // uint16_t
                    case 4: // sint16_t
                    {
                        m_data_type = SHORT_IMG;
                    }
                    break;

                    case 5: // uint32_t
                    case 6: // sint32_t
                    {
                        m_data_type = LONG_IMG;
                    }
                    break;

                    case 7: // uint64_t
                    case 8: // sint64_t
                    {
                        m_data_type = LONGLONG_IMG;
                    }
                    break;

                    case 9:  // IEEE 754 single-precision binary floating-point format:
                        // binary32
                    case 11: // Complex float
                    {
                        m_data_type = FLOAT_IMG;
                    }
                    break;

                    case 10: // IEEE 754 double-precision binary floating-point format:
                        // binary64
                    case 12: // Complex double
                    {
                        m_data_type = DOUBLE_IMG;
                    }
                    break;
                }
            }

            Log::Logger &m_log;
            std::string m_shm_path;
            std::string m_rec_path;

            int m_data_type;
            std::vector<long> m_data_dims;

            ShmIfce<std::uint8_t> *m_shm;
            ShmIfce<float> *m_tsShm;
            std::size_t m_shmCnt;
            IMAGE m_img, m_tsImg;

            std::size_t m_frameBeingRecorded;

            std::size_t m_bin_capacity; // # seconds before bin is considered full.
            std::size_t m_bin_count;
            struct {
                CCfits::FITS *writer;
                bool has_origin;
                float origin;
            } m_bin;
        };
    }; // namespace Telemetry
}; // namespace Dao

#endif