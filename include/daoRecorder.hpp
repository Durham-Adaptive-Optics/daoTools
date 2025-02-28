/**
 * @file    daoRecorder.h
 * @brief   Monitors a DAO shared memory and records frames to disk (FITS format).
 * 
 * @author  T.N Davies
 * 
 * @date    10/01/2025
 */

#ifndef DAO_RECORDER__HPP
#define DAO_RECORDER__HPP

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
#include <map>

#define DAO_REC_PROFILING

#ifdef DAO_REC_PROFILING

#define PROFILE_START(prof_name)                                        \
const auto prof_name##0 = std::chrono::high_resolution_clock::now();

#define PROFILE_END(prof_name)                                          \
const auto prof_name##1 = std::chrono::high_resolution_clock::now();    \
const auto prof_name = prof_name##1 - prof_name##0;

#else

#define PROFILE_START(prof_name)
#define PROFILE_END(prof_name)

#endif

#define DAO_REC_NOBINNING (0) // Pass this in for 'recordingFileCapacity' to disable binning.

namespace Dao
{
    namespace Telemetry
    {
        class Recorder : public Thread {
        public:
            Recorder(const std::string &shmPath, const std::string &recordingRoot, const int core, 
                Log::Logger &logger, const std::size_t recordingFileCapacity) 
                :
                mRecordingFileCapacity(recordingFileCapacity),
                Thread(shmPath, logger, core),
                mRecordingRoot(recordingRoot),
                mRecordingFile(nullptr),
                mShmInterface(nullptr),
                mRecordingFileCount(0),
                mRecordingFileSize(0),
                mShmPath(shmPath),
                mShmRefCounter(0),
                mFitsDataType(0),
                mLogger(logger),
                mLocalName(""),
                mFitsBPP(0)
            {
                mShmInterface = new ShmIfce<std::uint8_t>(mLogger);
                mShmInterface->OpenShm(mShmPath.c_str(), &mShmImage, m_node);
                if (mShmImage.md->atype == 10 || mShmImage.md->atype == 12) {
                    throw std::logic_error("Dao complex-valued shared-memory is currently unsupported");
                }

                // Extract local name from full shm name.
                const std::string shmName = mShmImage.name;
                const size_t lastSlash = shmName.find_last_of("/\\");
                mLocalName = shmName.substr(lastSlash + 1);
                const size_t extensionPos = mLocalName.find(".im.shm");
                if (extensionPos != std::string::npos) {
                    mLocalName = mLocalName.substr(0, extensionPos);
                }

                // Infer information required by Cfitsio from Dao shm metadata. 
                mFitsDataType = mFitsTypeMap.at(mShmImage.md->atype);
                mFitsBPP = mFitsBppMap.at(mShmImage.md->atype);
                for (std::size_t i = 0; i < mShmImage.md->naxis; ++i) 
                {
                    const auto nAxisElements = mShmImage.md->size[i];
                    mDataDimensions.push_back(nAxisElements);
                }

                NewRecordingFile();
            }

            ~Recorder()
            {
                CloseRecordingFile();
                delete mShmInterface;
            }

        private:
            void OnceOnStart() override 
            { 
                mShmRefCounter = mShmInterface->GetFrameCounter(); 
            }

            void RestartableThread() override
            {
                const auto counter = mShmInterface->GetFrameCounter();
                const std::size_t shmFrameDelta = counter - mShmRefCounter;
                mShmRefCounter = counter;

                if(shmFrameDelta) 
                {
                    // Setup a new recording file if needed.
                    if(mRecordingFileCapacity != DAO_REC_NOBINNING &&
                        mRecordingFileSize == mRecordingFileCapacity)
                    {
                        PROFILE_START(profNewRecFile)
                        CloseRecordingFile();
                        NewRecordingFile();
                        if(!mRecordingFile) {
                            Stop();
                            mLogger.Warning("Recording for %s stopped due to no recording file", mShmPath);
                            return;
                        }
                        PROFILE_END(profNewRecFile)

                        #ifdef DAO_REC_PROFILING
                        mLogger.Debug(
                            "Recording file was filled, a new one was created within %dms", 
                            std::chrono::duration_cast<std::chrono::milliseconds>(profNewRecFile).count()
                        );
                        #endif
                    }

                    RecordCurrentShmFrame();

                    //! To avoid (costly) double-buffering we assume that the 
                    //! time between shared memory updates is longer than the
                    //! time to retire a frame to disk.
                    if(mShmInterface->GetFrameCounter() > mShmRefCounter)
                    {
                        mLogger.Critical("%s recieved an update while frame %d was being retired to the disk", 
                            mShmPath,
                            mShmRefCounter
                        );
                    }
                }
            }

            void RecordCurrentShmFrame()
            {
                PROFILE_START(profRecord)
                PROFILE_START(profWriteHDU)
                const double timestamp = (double)mShmImage.md->atime.ts.tv_sec + mShmImage.md->atime.ts.tv_nsec / 1e9;
                {
                    int status = 0;
                    fits_create_img(mRecordingFile, mFitsBPP, mDataDimensions.size(), mDataDimensions.data(), &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        mLogger.Error("Failed to create FITS hdu: %s", errbuff);
                        return;
                    }
                }
                {
                    int status = 0;
                    double ts = timestamp; // avoid const.
                    fits_write_key(mRecordingFile, TDOUBLE, "TIME-OBS", &ts, "", &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        mLogger.Warning("Failed to write timestamp to FITS hdu: %s", errbuff);
                        return;
                    }
                }
                PROFILE_END(profWriteHDU)

                PROFILE_START(profWriteFrameData)
                {
                    int status = 0;
                    fits_write_img(mRecordingFile, mFitsDataType, 1, mShmImage.md->nelement, mShmInterface->GetPtr(), &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        mLogger.Error("Failed to write data to FITS file: %s", errbuff);
                        return;
                    }
                }

                PROFILE_END(profWriteFrameData)
                
                ++mRecordingFileSize;
                PROFILE_END(profRecord)

                #ifdef DAO_REC_PROFILING
                std::string prof_str;
                prof_str += "\n================\n";
                prof_str += "Recording Profile\n";
                prof_str += "================\n";
                prof_str += "Total: %dms\n";
                prof_str += "Write HDU: %dms\n";
                prof_str += "Write Frame Data: %dms\n";
                prof_str += "----------------\n";

                m_log.Debug(prof_str.c_str(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(profRecord).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(profWriteHDU).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(profWriteFrameData).count()
                );
                #endif
            }

            void CloseRecordingFile()
            {
                int status = 0;
                fits_close_file(mRecordingFile, &status);
                mRecordingFile = nullptr;
                if (status) {
                    char errbuff[FLEN_STATUS];
                    fits_get_errstatus(status, errbuff);
                    mLogger.Error("%s", errbuff);
                }
            }

            void NewRecordingFile()
            {
                const std::string recordingFilePath = mRecordingRoot + "/" + mLocalName +
                    std::to_string(mRecordingFileCount) + ".fits";
                
                int status = 0;
                fits_create_file(&mRecordingFile, recordingFilePath.c_str(), &status);
                if (status) {
                    char errbuff[FLEN_STATUS];
                    fits_get_errstatus(status, errbuff);
                    mLogger.Critical("Failed to create new recordings file: %s", errbuff);
                    mRecordingFile = nullptr;
                    return;
                }

                ++mRecordingFileCount;
                mLogger.Info("Recording %s to %s", mShmPath.c_str(), recordingFilePath.c_str());
            }

            // Cfitsio
            std::vector<long> mDataDimensions;
            int mFitsDataType;
            int mFitsBPP;

            // Shared Memory
            ShmIfce<std::uint8_t> *mShmInterface;
            std::size_t mShmRefCounter;
            IMAGE mShmImage;

            // Recording File.
            std::size_t mRecordingFileCapacity; // How many frames a file will contain.
            std::size_t mRecordingFileCount;    // How many recording files we have created.
            std::size_t mRecordingFileSize;     // How many frames are in the current recording file.
            fitsfile *mRecordingFile;

            //
            Log::Logger &mLogger;
            std::string mShmPath;
            std::string mLocalName;
            std::string mRecordingRoot;

            // Mapping from Dao -> Cfitsio datatypes.
            const std::map<std::uint8_t, std::uint8_t> mFitsTypeMap
            {
                {1, TBYTE},
                {2, TSBYTE},
                {3, TUSHORT},
                {4, TSHORT},
                {5, TUINT},
                {6, TINT},
                {7, TULONGLONG},
                {8, TLONGLONG},
                {9, TFLOAT},
                {10, TCOMPLEX},
                {11, TDOUBLE},
                {12, TDBLCOMPLEX}
            };

            // Mapping from Dao datatypes to Cfitsio element bit-sizes.
            const std::map<std::size_t, std::int8_t>  mFitsBppMap 
            {
                {1, BYTE_IMG},
                {2, BYTE_IMG},
                {3, SHORT_IMG},
                {4, SHORT_IMG},
                {5, LONG_IMG},
                {6, LONG_IMG},
                {7, LONGLONG_IMG},
                {8, LONGLONG_IMG},
                {9, FLOAT_IMG},
                {11, DOUBLE_IMG}
            };
        };
    }; // namespace Telemetry
}; // namespace Dao

#endif