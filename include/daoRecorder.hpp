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
#include <functional>

 // @DAO_REC_LOGRATE_INTERVAL:
 // Specifies the period (secs) to wait
 // before estimating & logging the
 // recording rate of the assigned
 // shared memory.
#define DAO_REC_LOGRATE_INTERVAL 5

namespace Dao
{
    namespace Telemetry
    {
        class Recorder : public Thread {
        public:
            Recorder(const std::string &shmPath, const std::string &recordingRoot, const int core,
                const std::size_t recordingFileCapacity, const std::int64_t frameTarget,
                Log::Logger &logger)
                :
                mRecordingFileCapacity(recordingFileCapacity),
                Thread(shmPath, logger, core),
                mRecordingDirectory(recordingRoot),
                mFrameTarget(frameTarget),
                mInternalBuffer(nullptr),
                mRecordingFile(nullptr),
                mShmInterface(nullptr),
                mRecordingFileCount(0),
                mRecordingFileSize(0),
                mShmBufferSize(0),
                mShmPath(shmPath),
                mError(false),
                mTotalValidFrames(0),
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

                mShmBufferSize = mDaoBppMap.at(mShmImage.md->atype) * mShmImage.md->nelement;
                mInternalBuffer = new std::uint8_t[mShmBufferSize];
                mLogger.Debug("Allocated %d bytes for %s's internal buffer", mShmBufferSize, mShmPath.c_str());
                if (!mInternalBuffer) {
                    throw std::runtime_error("Failed to allocate internal buffer");
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
                for (std::int64_t i = mShmImage.md->naxis - 1; i >= 0; --i) {
                    const auto nAxisElements = mShmImage.md->size[i];
                    mDataDimensions.push_back(nAxisElements);
                }

                NewRecordingFile();

                if (!mRecordingFile) {
                    throw std::runtime_error("Failed to create initial recordings file");
                }

                Spawn();
            }

            ~Recorder()
            {
                CloseRecordingFile();
                delete mShmInterface;
                delete[] mInternalBuffer;
            }

            bool inError() const { return mError; }
            std::string getShmName() const { return mShmPath.c_str(); }

        private:
            void HandleFitsError(const int fitsStatus, const std::string &msgPreamble)
            {
                if (fitsStatus) {
                    char errbuff[FLEN_STATUS];
                    fits_get_errstatus(fitsStatus, errbuff);
                    mLogger.Error("%s: %s", msgPreamble.c_str(), errbuff);
                    mError = true;
                    Exit();
                }
            }

            void OnceOnStart() override
            {
                mShmRefCounter = mShmInterface->GetFrameCounter();
                mLogger.Info("%s's recorder has started (target=%d)", mShmPath.c_str(), mFrameTarget);
            }

            void OnceOnStop() override
            {
                mLogger.Info("%s's recorder has stopped", mShmPath.c_str());
            }

            void RestartableThread() override
            {
                // Check if we have met our target.
                if (mTotalValidFrames == mFrameTarget) {
                    mLogger.Info("Successfully recorded %d valid frames from %s to disk",
                        mTotalValidFrames,
                        mShmPath.c_str()
                    );

                    Exit();
                }

                // If binning is enabled, create new fits file if
                // the current file is full.
                if (mRecordingFileCapacity && mRecordingFileSize == mRecordingFileCapacity) 
                {
                    CloseRecordingFile();

                    NewRecordingFile();
                    if (!mRecordingFile) {
                        mLogger.Critical("Recording for %s stopped due to no recording file", mShmPath.c_str());
                        mError = true;
                        Exit();
                        return;
                    }
                }

                // Wait until a new frame is pushed and record it.
                const auto shmCounter = mShmInterface->GetFrameCounter();
                if (shmCounter > mShmRefCounter) {
                    mShmRefCounter = shmCounter;
                    mLogger.Debug("New frame available for %s", mShmPath.c_str());

                    std::memcpy(mInternalBuffer, mShmInterface->GetPtr(), mShmBufferSize);

                    // Once the copy has completed we check the shm hasn't been updated;
                    // if it has then the internal buffer may not contain the frame 
                    // we wanted.
                    const bool internalBufferCorrupted = mShmInterface->GetFrameCounter() > mShmRefCounter;
                    if (internalBufferCorrupted) {
                        mLogger.Warning("%s was updated while frame %d was being copied (recorded as corrupted)",
                            mShmPath.c_str(),
                            mShmRefCounter
                        );
                    }

                    RecordInternalBuffer(internalBufferCorrupted);
                }
            }

            void RecordInternalBuffer(const bool frameCorrupted)
            {
                /* === Write the HDU for this frame into the FITS file === */
                const double timestamp = (double)mShmImage.md->atime.ts.tv_sec + mShmImage.md->atime.ts.tv_nsec / 1e9;

                {
                    int status = 0;
                    fits_create_img(mRecordingFile, mFitsBPP, mDataDimensions.size(), mDataDimensions.data(), &status);
                    HandleFitsError(status, "Failed to create FITS HDU");
                }

                {
                    int status = 0;
                    double ts = timestamp; // avoid const.
                    fits_write_key(mRecordingFile, TDOUBLE, "TIME-OBS", &ts, "", &status);
                    HandleFitsError(status, "Failed to write timestamp key to HDU");
                }

                {
                    int status = 0;
                    bool isFrameValid = !frameCorrupted;
                    fits_write_key(mRecordingFile, TLOGICAL, "VALID", &isFrameValid, "", &status);
                    HandleFitsError(status, "Failed to write validation key to HDU");
                }

                /* === Write the buffered frame data into the FITS file === */
                {
                    int status = 0;
                    mLogger.Debug("Recorded frame of %d elements (FITS type: %d) (Valid: %s)", 
                        mShmImage.md->nelement, 
                        mFitsDataType,
                        frameCorrupted ? "F" : "T"
                    );
                    fits_write_img(mRecordingFile, mFitsDataType, 1, mShmImage.md->nelement, mInternalBuffer, &status);
                    HandleFitsError(status, "Failed to write data to FITS file");
                }

                ++mRecordingFileSize;

                if (!frameCorrupted) {
                    ++mTotalValidFrames;
                }
            }

            void CloseRecordingFile()
            {
                int status = 0;
                fits_close_file(mRecordingFile, &status);
                HandleFitsError(status, "Failed to close current recording file");
                mRecordingFile = nullptr;
            }

            void NewRecordingFile()
            {
                const std::string fileName = mLocalName + std::to_string(mRecordingFileCount) + ".fits";
                const std::string filePath = mRecordingDirectory + "/" + fileName;
                mLogger.Debug("Creating new FITS file: %s", filePath.c_str());

                int status = 0;
                fits_create_file(&mRecordingFile, filePath.c_str(), &status);
                HandleFitsError(status, "Failed to create new recordings file");
                if (status) {
                    mRecordingFile = nullptr;
                    return;
                }

                ++mRecordingFileCount;
                mRecordingFileSize = 0;
                mLogger.Info("Recording %s to %s", mShmPath.c_str(), filePath.c_str());
            }

            // Cfitsio
            std::vector<long> mDataDimensions;
            int mFitsDataType;
            int mFitsBPP;

            // Shared Memory
            ShmIfce<std::uint8_t> *mShmInterface;
            std::uint8_t *mInternalBuffer;
            std::size_t mShmRefCounter;
            std::size_t mShmBufferSize;
            IMAGE mShmImage;

            // Recording File.
            std::size_t mRecordingFileCapacity; // How many frames a file will contain.
            std::size_t mRecordingFileCount;    // How many recording files we have created.
            std::size_t mRecordingFileSize;     // How many frames are in the current recording file.
            std::size_t mTotalValidFrames;      // How many frames in total we have recorded.
            fitsfile *mRecordingFile;

            //
            std::string mRecordingDirectory;
            std::int64_t mFrameTarget;      // How many frames to record, or -1 to record indefinitely.
            std::string mLocalName;
            Log::Logger &mLogger;
            std::string mShmPath;
            bool mError;

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

            // Mapping of Dao datatypes to element sizes.
            const std::map<std::size_t, std::uint8_t> mDaoBppMap
            {
                {1, sizeof(std::uint8_t)},
                {2, sizeof(std::int8_t)},
                {3, sizeof(std::uint16_t)},
                {4, sizeof(std::int16_t)},
                {5, sizeof(std::uint32_t)},
                {6, sizeof(std::int32_t)},
                {7, sizeof(std::uint64_t)},
                {8, sizeof(std::int64_t)},
                {9, sizeof(float)},
                {10, sizeof(double)}
            };
        };
    }; // namespace Telemetry
}; // namespace Dao

#endif // DAO_RECORDER__HPP