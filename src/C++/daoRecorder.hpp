/******************************************************************************
 * Project:        daoRecorder
 * Description:    Monitors a dao-shm and records frames to disk in FITS format.
 * Author:         Thomas Davies
 * Created:        10/01/2025
 ******************************************************************************/

#ifndef DAO_RECORDER__HPP
#define DAO_RECORDER__HPP

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

/* Note:
    Define the following preprocessor symbol to have every N seconds
    of data recorded, be stored into seperate FITS files.
    
    DAO_RECORDER_ENABLE_BINNING
*/

/* Note:
    Define the following preprocessor symbol to enable
    profiling of recordings.

    ENABLE_PROFILING
*/

// === Code ===

// Mapping from Dao -> cfitsio data-types.
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

// Mapping from Dao data-types -> cfitsio
// bits-per-pixel.
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
    0,              //!atype=10 (Complex float32) - Not supported. 
    DOUBLE_IMG,     // atype=11 (Real float64)
    0               //!atype=12 (Complex float64) - Not supported.
};

#ifdef ENABLE_PROFILING

#define PROFILE_START(prof_name)                                        \
const auto prof_name##0 = std::chrono::high_resolution_clock::now();

#define PROFILE_END(prof_name)                                          \
const auto prof_name##1 = std::chrono::high_resolution_clock::now();    \
const auto prof_name = prof_name##1 - prof_name##0;

#else

#define PROFILE_START(prof_name)
#define PROFILE_END(prof_name)

#endif

namespace Dao
{
    namespace Telemetry
    {
        class Recorder : public Thread {
        public:
            Recorder(const std::string &shm_name, const std::string &shm_path, 
                const std::string &rec_root, int core, Log::Logger &logger) 
                :
                Thread(shm_name, logger, core),
                m_shm_name(shm_name),
                m_shm_path(shm_path),
                m_rec_root(rec_root),
                m_log(logger),
                m_shmCntRef(0),
                m_FITS_dtype(0),
                m_FITS_bpp(0),
                m_bin_count(0),
                m_bin(nullptr),
                m_bin_has_origin(false),
                m_bin_origin(0)
            {
                //
                m_log.Debug("Recording %s on numa node %d (core %d)", 
                    m_shm_path.c_str(), m_node, m_core); 

                //
                m_shm = new ShmIfce<std::uint8_t>(m_log);
                m_shm->OpenShm(m_shm_path.c_str(), &m_img, m_node);
                m_FITS_dtype = kDataTypes[m_img.md->atype];
                m_FITS_bpp = kBitsPerPixel[m_img.md->atype];

                // todo(tom): support complex types.
                // ! We don't support recording complex-valued data
                // ! to FITS files currently, so throw exception
                // ! in this case.  
                if (m_img.md->atype == 10 || m_img.md->atype == 12) {
                    throw std::logic_error("Dao complex-valued shared-memory unsupported");
                }

                // Infer data dims from shm metadata.
                m_data_dims.reserve(3);
                for (std::size_t k = 0; k < m_img.md->naxis; ++k) {
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
            void OnceOnStart() override { m_shmCntRef = m_shm->GetFrameCounter(); };

            void RestartableThread() override
            {
                const auto m_shmCnt = m_shm->GetFrameCounter();
                const std::size_t delta = m_shmCnt - m_shmCntRef;
                m_shmCntRef = m_shmCnt;

                if (!delta) {  // Already recorded this frame.
                    return;
                }

                if (delta > 1) { // Missed one or more frames.
                    m_log.Warning("Missed recording the last %d frames", delta - 1);
                }

                //
                RecordFrame();
            }

            void RecordFrame()
            {
                PROFILE_START(prof_record)
                //

                PROFILE_START(prof_rts)
                const double timestamp = (double)m_img.md->atime.ts.tv_sec + m_img.md->atime.ts.tv_nsec / 1e9;
                PROFILE_END(prof_rts)

                // Ensure we have the correct bin ready to receive data.
                PROFILE_START(prof_bineval)
                #ifdef DAO_RECORDER_ENABLE_BINNING
                // todo: implement binning.
                // todo: if fail to create next bin,
                // todo: continue using current bin.
                #endif
                PROFILE_END(prof_bineval)

                PROFILE_START(prof_binstamp)
                if (!m_bin_has_origin) {
                    m_bin_origin = timestamp;
                    m_bin_has_origin = true;
                }
                PROFILE_END(prof_binstamp)

                PROFILE_START(prof_whdu)
                {
                    int status = 0;
                    fits_create_img(m_bin, m_FITS_bpp, m_data_dims.size(), m_data_dims.data(), &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        m_log.Error("Failed to create FITS hdu: %s", errbuff);
                        return;
                    }
                }
                {
                    int status = 0;
                    double ts = timestamp; // avoid const.
                    fits_write_key(m_bin, TDOUBLE, "TIME-OBS", &ts, "", &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        m_log.Warning("Failed to write timestamp to FITS hdu: %s", errbuff);
                        return;
                    }
                }
                PROFILE_END(prof_whdu)

                PROFILE_START(prof_wdat)
                {
                    int status = 0;
                    fits_write_img(m_bin, m_FITS_dtype, 1, m_img.md->nelement, m_shm->GetPtr(), &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        m_log.Error("Failed to write data to FITS file: %s", errbuff);
                        return;
                    }
                }
                PROFILE_END(prof_wdat)

                //
                PROFILE_END(prof_record)

                #ifdef ENABLE_PROFILING
                std::string prof_str;
                prof_str += "\n================\n";
                prof_str += "Recording Profile\n";
                prof_str += "================\n";
                prof_str += "Total: %dms\n";
                prof_str += "Read ts: %dms\n";
                prof_str += "Bin eval: %dms\n";
                prof_str += "Bin stamp: %dms\n";
                prof_str += "Write HDU: %dms\n";
                prof_str += "Write data: %dms\n";
                prof_str += "----------------\n";

                m_log.Debug(prof_str.c_str(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(prof_record).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(prof_rts).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(prof_bineval).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(prof_binstamp).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(prof_whdu).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(prof_wdat).count()
                );
                #endif
            }

            void CreateBin()
            {
                //
                if(m_bin) {
                    int status = 0;
                    fits_close_file(m_bin, &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        throw std::runtime_error("Failed to close FITS file");
                    }
                }
                
                //
                std::string bin_path = m_rec_root + "/" + m_shm_name;
                #ifdef DAO_RECORDER_ENABLE_BINNING
                bin_path += "_" + std::to_string(m_bin_count);
                #endif
                bin_path += ".fits";
                m_log.Debug("Creating new bin: %s", bin_path.c_str());

                int status = 0;
                fits_create_file(&m_bin, bin_path.c_str(), &status);
                if (status) {
                    char errbuff[FLEN_STATUS];
                    fits_get_errstatus(status, errbuff);
                    throw std::runtime_error("Failed to create FITS file");
                }

                m_bin_has_origin = false;
                ++m_bin_count;

                //
                m_log.Info("Recording from %s to %s", m_shm_path.c_str(), bin_path.c_str());
            }

            Log::Logger &m_log;
            std::string m_shm_name;
            std::string m_shm_path;
            std::string m_rec_root;

            std::vector<long> m_data_dims;
            int m_FITS_dtype;
            int m_FITS_bpp;

            ShmIfce<std::uint8_t> *m_shm;
            ShmIfce<std::uint32_t> *m_tsShm;
            std::size_t m_shmCntRef;
            IMAGE m_img, m_tsImg;

            std::size_t m_bin_count;
            bool m_bin_has_origin;
            double m_bin_origin;
            fitsfile *m_bin;
        };
    }; // namespace Telemetry
}; // namespace Dao

#endif