/******************************************************************************
 * Project:        daoRecorder
 * Description:    A thread that awaits shared-memory updates and records the
                   new data to disk.
 Author:           Thomas Davies Created:        10/01/2025
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
#include <chrono>
#include <thread>
#include <fstream>

/*
    By default, the recorder object bins every N seconds of recorded
    data into seperate FITS files. If instead you require all data
    recorded to be saved into a single FITS file, please define the
    following preprocessor symbol.
*/
#define DAO_RECORDER_DISABLE_BINNING
#define _DEBUG

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
                m_lastRecordedCnt(0),
                m_data_type(0),
                m_bin_capacity(bin_capacity),
                m_bin_count(0),
                m_bin(nullptr),
                m_bin_has_origin(false),
                m_bin_origin(0)
            {
                //
                m_log.Info("Recording data from %s to %s on core %d", shm_path.c_str(),
                    m_rec_path.c_str(), m_core);

                // Open the time-keeping shm
                m_tsShm = new ShmIfce<std::uint32_t>(m_log);
                m_tsShm->OpenShm("ts.im.shm", &m_tsImg, Dao::Numa::Core2Node(m_core));

                // Open the target shm
                m_shm = new ShmIfce<std::uint8_t>(m_log);
                m_shm->OpenShm(m_shm_path.c_str(), &m_img, Dao::Numa::Core2Node(m_core));

                //
                InferFITSDatatype();
                GetDataDimensionality();
                CreateBin();
            }

            ~Recorder()
            {
                m_log.Trace("~Recorder");
                m_shm->CloseShm();
                delete m_shm;
                delete m_bin;
            }

        private:

            void OnceOnStart() override
            {
                // Note: The frame after this is the first frame we record.
                m_lastRecordedCnt = m_shm->GetFrameCounter();

            #ifdef _DEBUG
                m_log.Debug("Creating profile file");
                m_profile = new std::ofstream("profile.csv");
                assert(m_profile->is_open());
            #endif
            };

            void OnceOnStop() override
            {
            #ifdef _DEBUG
                m_log.Debug("Closing profile file");
                m_profile->close();
                delete m_profile;
            #endif
            }

            void RestartableThread() override
            {
                const auto frameCnt = m_shm->GetFrameCounter();
                const std::size_t delta = frameCnt - m_lastRecordedCnt;

                // We've already recorded the frame.
                if (!delta) {
                    return;
                }

                // We've missed one or more frames.
                if (delta > 1) {
                    m_log.Warning("Missed recording the last %d frames", delta - 1);
                }

            #ifdef _DEBUG
                const auto write_start = std::chrono::high_resolution_clock::now();
                RecordFrame();
                const auto write_end = std::chrono::high_resolution_clock::now();
                const std::chrono::duration<double, std::milli> write_time = write_end - write_start;
                m_log.Debug("Write took %fms", write_time.count());
                *m_profile << write_time.count() << ",\n";
            #else
                RecordFrame();
            #endif

                m_lastRecordedCnt = frameCnt;
            }

            void RecordFrame()
            {
                // Ensure we have the correct bin ready to receive data.
                const auto timestamp = *m_tsShm->GetPtr();
            #ifndef DAO_RECORDER_DISABLE_BINNING
                const auto elapsed = (timestamp - m_bin_origin) / 1e9; // seconds.
                if (elapsed >= m_bin_capacity) {
                    CreateBin();
                }
            #endif

                if (!m_bin_has_origin) {
                    m_bin_origin = timestamp;
                    m_bin_has_origin = true;
                }

                // @speed: can byte count calc be moved to Init?
                const auto bytes_per_element = (m_data_type >= 0 ? m_data_type : -m_data_type) / 8;
                std::size_t bytes = bytes_per_element * m_img.md->nelement;

                std::valarray<std::uint8_t> data_array(m_shm->GetPtr(), bytes);
                CCfits::ExtHDU *ext = m_bin->addImage(
                    std::to_string(timestamp),
                    m_data_type,
                    m_data_dims
                );
                ext->write(1, bytes, data_array);
                ext->writeChecksum(); // @speed: how much time does this take?
            }

            void CreateBin()
            {
                delete m_bin;

                std::string bin_name = m_rec_path;
            #ifndef DAO_RECORDER_DISABLE_BINNING
                bin_name += "-" + std::to_string(m_bin_count);
            #endif
                bin_name += ".fits";

                m_bin = new CCfits::FITS(bin_name, m_data_type, m_data_dims.size(), m_data_dims.data());
                m_bin_has_origin = false;
                ++m_bin_count;
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
            ShmIfce<std::uint32_t> *m_tsShm;
            std::size_t m_lastRecordedCnt;
            IMAGE m_img, m_tsImg;

            std::size_t m_bin_capacity; // # seconds before bin is considered full.
            std::size_t m_bin_count;
            CCfits::FITS *m_bin;
            bool m_bin_has_origin;
            float m_bin_origin;

        #ifdef _DEBUG
            std::ofstream *m_profile;
        #endif
        };
    }; // namespace Telemetry
}; // namespace Dao

#endif