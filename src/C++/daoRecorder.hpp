/******************************************************************************
 * Project:        daoRecorder
 * Description:    A thread that awaits shared-memory updates and records the
                   new data to disk.
 Author:           Thomas Davies Created:        10/01/2025
 ******************************************************************************/

#ifndef DAO_RECORDING_TARGET_HPP
#define DAO_RECORDING_TARGET_HPP

 // === Includes ===

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
#include <fitsio.h>

/*
    By default, the recorder object bins every N seconds of recorded
    data into seperate FITS files. If instead you require all data
    recorded to be saved into a single FITS file, please define the
    following preprocessor symbol.
*/
#define DAO_RECORDER_DISABLE_BINNING

// === Code ===

// Bpp: 8
// FITS dtype: 12
// DAO dtype: 

// Lookup table that provides a mapping from
// Dao data types to cfitsio data types.
const std::uint8_t kDataTypes[] =
{
    0,           // Padding.
    TBYTE,       // atype=1 (uint8_t)
    TSBYTE,      // atype=2 (int8_t)
    TUSHORT,     // atype=3 (uint16_t)
    TSHORT,      // atype=4 (int16_t)
    TUINT,       // atype=5 (uint32_t)
    TINT,        // atype=6 (int32_t)
    TULONGLONG,  // atype=7 (uint64_t)
    TLONGLONG,   // atype=8 (int64_t)
    TFLOAT,      // atype=9 (Real float32)
    TCOMPLEX,    // atype=10 (Complex float32)
    TDOUBLE,     // atype=11 (Real float64)
    TDBLCOMPLEX  // atype=12 (Complex float64)
};

// Lookup table that provides a mapping from
// Dao data types to cfitsio data type sizes.
const std::int8_t kBitsPerPixel[] =
{
    0,              // Padding.
    BYTE_IMG,       // atype=1 (uint8_t)
    BYTE_IMG,       // atype=2 (int8_t)
    SHORT_IMG,      // atype=3 (uint16_t)
    SHORT_IMG,      // atype=4 (int16_t)
    LONG_IMG,       // atype=5 (uint32_t)
    LONG_IMG,       // atype=6 (int32_t)
    LONGLONG_IMG,   // atype=7 (uint64_t)
    LONGLONG_IMG,   // atype=8 (int64_t)
    FLOAT_IMG,      // atype=9 (Real float32)
    0,              // atype=10 (Complex float32)
    DOUBLE_IMG,     // atype=11 (Real float64)
    0               // atype=12 (Complex float64)
};

struct RecordingProfile {
    std::chrono::milliseconds total;
    std::chrono::milliseconds ts;
    std::chrono::milliseconds stamp;
    std::chrono::milliseconds bcalc;
    std::chrono::milliseconds valarr;
    std::chrono::milliseconds addimg;
    std::chrono::milliseconds write;
    std::chrono::milliseconds csum;

    void Log(Dao::Log::Logger &logger)
    {
        std::string msg;
        msg += "\n================\n";
        msg += "Recording Profile\n";
        msg += "================\n";
        msg += "TOTAL: %dms\n";
        msg += "Read Timestamp: %dms\n";
        msg += "Stamp bin: %dms\n";
        msg += "Byte calc: %dms\n";
        msg += "ValArray: %dms\n";
        msg += "Add img: %dms\n";
        msg += "Write img: %dms\n";
        msg += "Add checksum: %dms\n";
        msg += "----------------\n";

        logger.Debug(msg.c_str(), 
           total.count(), 
           ts.count(), 
           stamp.count(), 
           bcalc.count(), 
           valarr.count(), 
           addimg.count(), 
           write.count(), 
           csum.count()
        );
    }
};

#define PROFILE_START(x)
#define PROFILE_END(x)

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
                m_FITS_dtype(0),
                m_FITS_bpp(0),
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
                m_FITS_dtype = kDataTypes[m_img.md->atype];
                m_FITS_bpp = kBitsPerPixel[m_img.md->atype];

                m_log.Debug("Dao Data type: %d",  m_img.md->atype);
                m_log.Debug("FITS Data type: %d", m_FITS_dtype);
                m_log.Debug("FITS Bpp: %d", m_FITS_bpp);

                // todo: support complex types.
                if(m_img.md->atype == 10 || m_img.md->atype == 12) {
                    m_log.Error("Complex data types are not currently supported!");
                    assert(false); // todo: how to handle unsupported data format?
                }

                // Infer data dims from shm metadata.
                m_data_dims.reserve(3);
                for (std::size_t k = 0; k < m_img.md->naxis; ++k) 
                {
                    m_data_dims.push_back(m_img.md->size[k]);
                }

                //
                CreateBin();
            }

            ~Recorder()
            {
                m_shm->CloseShm();
                delete m_shm;
            }

        private:

            void OnceOnStart() override
            {
                m_lastRecordedCnt = m_shm->GetFrameCounter();
            };

            void OnceOnStop() override
            {
                //
            }

            void RestartableThread() override
            {
                const auto frameCnt = m_shm->GetFrameCounter();
                const std::size_t delta = frameCnt - m_lastRecordedCnt;
                if(!delta) return; // Already recorded this frame.
                
                // Missed one or more frames.
                if (delta > 1) {
                    m_log.Warning("Missed recording the last %d frames", delta - 1);
                }

                //
                RecordingProfile profile = {};
                RecordFrame(profile);
                profile.Log(m_log);

                //
                m_lastRecordedCnt = frameCnt;
            }

            void RecordFrame(RecordingProfile &profile)
            {
                PROFILE_START("Record")
                //

                PROFILE_START("Read Timestamp")
                auto timestamp = *m_tsShm->GetPtr();
                PROFILE_END()

                // Ensure we have the correct bin ready to receive data.
                #ifndef DAO_RECORDER_DISABLE_BINNING
                PROFILE_START("Bin Eval")
                const auto elapsed = (timestamp - m_bin_origin) / 1e9; // seconds.
                if (elapsed >= m_bin_capacity) {
                    CreateBin();
                }
                PROFILE_END()
                #endif

                PROFILE_START("Bin stamping")
                if (!m_bin_has_origin) {
                    m_bin_origin = timestamp;
                    m_bin_has_origin = true;
                }
                PROFILE_END()

                PROFILE_START("Write HDU metadata")
                {
                    int status = 0;
                    fits_create_img(m_bin, m_FITS_bpp, m_data_dims.size(), m_data_dims.data(), &status);
                    if(status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        m_log.Error("Failed to create FITS hdu: %s", errbuff);
                        return; // Bail recording the frame.
                    }
                }
                PROFILE_END()

                PROFILE_START("Write timestamp")
                {
                    int status = 0;
                    fits_write_key(m_bin, TLONGLONG, "TIME-OBS", &timestamp, "", &status);
                    if(status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        m_log.Warning("Failed to write timestamp to FITS hdu: %s", errbuff);
                    }
                }
                PROFILE_END()

                PROFILE_START("Write data")
                {
                    int status = 0;
                    fits_write_img(m_bin, m_FITS_dtype, 1, m_img.md->nelement, m_shm->GetPtr(), &status);
                    if(status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        m_log.Error("Failed to write data to FITS file: %s", errbuff);
                        return; // Bail recording the frame.
                    }
                }
                PROFILE_END()

                //
                PROFILE_END()
                // profile.total = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                // profile.ts = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
                // profile.stamp = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2);
                // profile.bcalc = std::chrono::duration_cast<std::chrono::milliseconds>(t5 - t4);
                // profile.valarr = std::chrono::duration_cast<std::chrono::milliseconds>(t7 - t6);
                // profile.addimg = std::chrono::duration_cast<std::chrono::milliseconds>(t9 - t8);
                // profile.write = std::chrono::duration_cast<std::chrono::milliseconds>(t11 - t10);
                // profile.csum = std::chrono::duration_cast<std::chrono::milliseconds>(t13 - t12);
            }

            void CreateBin()
            {
                // Release old bin.
                if(m_bin) {
                    int status = 0;
                    fits_close_file(m_bin, &status);
                    if(status) { // non-zero code means error.
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        m_log.Error("Failed to close FITS file: %s", errbuff);
                        assert(false); // todo: what should we do if we failed to close a bin?
                    }
                }

                // Create new bin.
                std::string bin_name = m_rec_path;
                #ifndef DAO_RECORDER_DISABLE_BINNING
                bin_name += "-" + std::to_string(m_bin_count);
                #endif
                bin_name += ".fits";

                int status = 0;
                fits_create_file(&m_bin, bin_name.c_str(), &status); 
                if(status) {
                    char errbuff[FLEN_STATUS];
                    fits_get_errstatus(status, errbuff);
                    m_log.Error("Failed to create FITS file: %s", errbuff);
                    assert(false); // todo: what should we do if we failed to create a bin?
                }

                m_bin_has_origin = false;
                ++m_bin_count;
            }

            Log::Logger &m_log;
            std::string m_shm_path;
            std::string m_rec_path;

            std::vector<long> m_data_dims;
            int m_FITS_dtype;
            int m_FITS_bpp;

            ShmIfce<std::uint8_t> *m_shm;
            ShmIfce<std::uint32_t> *m_tsShm;
            std::size_t m_lastRecordedCnt;
            IMAGE m_img, m_tsImg;

            std::size_t m_bin_capacity; // # seconds before bin is considered full.
            std::size_t m_bin_count;
            bool m_bin_has_origin;
            float m_bin_origin;
            fitsfile *m_bin;
        };
    }; // namespace Telemetry
}; // namespace Dao

#endif