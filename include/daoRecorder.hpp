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
#include <iomanip>
#include <chrono>
#include <thread>
#include <fstream>
#include <fitsio.h>
#include <map>
#include <functional>

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
                mRecordingFileSize(0),
                mNumRecordedFrames(0),
                mCurrentFilePath(""),
                mShmBufferSize(0),
                mShmPath(shmPath),
                mFitsDataType(0),
                mElementCount(0),
                mLogger(logger),
                mLocalName(""),
                mError(false),
                mFitsBPP(0),
                mAtype(0),
                mCnt0(0)
            {
                mShmInterface = new ShmIfce<std::uint8_t>(mLogger);
                mShmInterface->OpenShm(mShmPath.c_str(), &mShmImage, m_node);

                mElementCount = mShmImage.md->nelement;
                mAtype = mShmImage.md->atype;
                mFitsBPP = mFitsBppMap.at(mAtype);
                mFitsDataType = mFitsTypeMap.at(mAtype);

                if (mAtype == 10 || mAtype == 12) {
                    throw std::logic_error("Dao complex-valued shared-memory is currently unsupported");
                }

                mShmBufferSize = mDaoBppMap.at(mAtype) * mElementCount;
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

                // Infer array dimensions for fits header. 
                for (std::int64_t i = mShmImage.md->naxis - 1; i >= 0; --i) {
                    const auto nAxisElements = mShmImage.md->size[i];
                    mDataDimensions.push_back(nAxisElements);
                }

                if (!CreateFitsFile()) {
                    throw std::runtime_error("Failed to create initial recordings file");
                }

                Spawn();
            }

            ~Recorder()
            {
                CloseFitsFile();
                delete mShmInterface;
                delete[] mInternalBuffer;
            }

            bool inError() const { return mError; }
            std::string getShmName() const { return mShmPath.c_str(); }

        private:
            void OnceOnStart() override
            {
                mCnt0 = mShmInterface->GetFrameCounter();
                mLogger.Info("%s's recorder has started (target=%d)", mShmPath.c_str(), mFrameTarget);
            }

            void OnceOnStop() override
            {
                mLogger.Info("%s's recorder has stopped", mShmPath.c_str());
            }

            void RestartableThread() override
            {
                // Check if we have met our target.
                if (mNumRecordedFrames == mFrameTarget) {
                    mLogger.Info("Successfully recorded %d frames from %s",
                        mNumRecordedFrames,
                        mShmPath.c_str()
                    );

                    Exit();
                    return;
                }

                // Switch over to a new FITS file if the current has met the specified capacity.
                if (mRecordingFileCapacity && mRecordingFileSize == mRecordingFileCapacity) 
                {
                    if(!CloseFitsFile()) {
                        mLogger.Error("%s's recorder couldn't close its current FITS file", mShmPath.c_str());
                        SignalError();
                        return;
                    }

                    if(!CreateFitsFile()) {
                    mLogger.Error("%s's recorder couldn't create a new FITS file", mShmPath.c_str());
                        SignalError();
                        return;
                    }
                }

                // Wait until a new frame is pushed and record it.
                const auto cnt0_ = mShmInterface->GetFrameCounter();
                const auto delta = cnt0_ - mCnt0;
               
                if(delta > 1) {
                    const auto nMissedFrames = delta - 1;
                    mLogger.Warning("Missed %d frames from %s", nMissedFrames, mShmPath.c_str());
                }

                if (delta) {
                    mCnt0 = cnt0_;
                    
                    if(!CopyFrameData()) {
                        mLogger.Warning("%s didn't record frame %d as copy was interrupted",
                            mShmPath.c_str(),
                            mCnt0
                        );
                        return;
                    }

                    if(!RecordFrame()) {
                        mLogger.Error("Failed to record frame %d for %s", mCnt0, mShmPath.c_str());
                        SignalError();
                        return;
                    }
                }
            }

            bool CopyFrameData()
            {
                // Copy frame metadata.
                mFrameData.cnt0 = mShmImage.md->cnt0;
                mFrameData.cnt1 = mShmImage.md->cnt1;
                mFrameData.cnt2 = mShmImage.md->cnt2;
                mFrameData.timestamp = (double)mShmImage.md->atime.ts.tv_sec + mShmImage.md->atime.ts.tv_nsec / 1e9;

                // Copy frame data.
                std::memcpy(mInternalBuffer, mShmInterface->GetPtr(), mShmBufferSize);

                // Return true if the copy was successful, and false if it was interrupted.
                return mShmInterface->GetFrameCounter() == mCnt0;
            }

            bool RecordFrame()
            {
                /* === Write the HDU for this frame into the FITS file === */
                {
                    int status = 0;
                    fits_create_img(mRecordingFile, mFitsBPP, mDataDimensions.size(), mDataDimensions.data(), &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        mLogger.Error("Failed to create FITS HDU: %s", errbuff);
                        return false;
                    }
                }

                 /* === Write the HDU key-pairs === */
                {
                    int status = 0;
                    fits_write_key(mRecordingFile, TULONGLONG, "cnt0", &mFrameData.cnt0, "", &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        mLogger.Error("Failed to write cnt0 field to HDU: %s", errbuff);
                        return false;
                    }
                }

                {
                    int status = 0;
                    fits_write_key(mRecordingFile, TULONGLONG, "cnt1", &mFrameData.cnt1, "", &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        mLogger.Error("Failed to write cnt1 field to HDU: %s", errbuff);
                        return false;
                    }
                }

                {
                    int status = 0;
                    fits_write_key(mRecordingFile, TULONGLONG, "cnt2", &mFrameData.cnt2, "", &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        mLogger.Error("Failed to write cnt2 field to HDU: %s", errbuff);
                        return false;
                    }
                }

                {
                    int status = 0;
                    fits_write_key(mRecordingFile, TBYTE, "atype", &mAtype, "", &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        mLogger.Error("Failed to write atype field to HDU: %s", errbuff);
                        return false;
                    }
                }

                {
                    int status = 0;
                    fits_write_key(mRecordingFile, TDOUBLE, "atime", &mFrameData.timestamp, "", &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        mLogger.Error("Failed to write timestamp field to HDU: %s", errbuff);
                        return false;
                    }
                }

                /* === Write the buffered frame data === */
                {
                    int status = 0;
                    fits_write_img(mRecordingFile, mFitsDataType, 1, mElementCount, mInternalBuffer, &status);
                    if (status) {
                        char errbuff[FLEN_STATUS];
                        fits_get_errstatus(status, errbuff);
                        mLogger.Error("Failed to write frame data: %s", errbuff);
                        return false;
                    }
                    
                }
                
                ++mRecordingFileSize;
                ++mNumRecordedFrames;

                mLogger.Debug("Recorded frame of %d elements (FITS type: %d)", 
                    mElementCount, 
                    mFitsDataType
                );

                return true;
            }

            bool CreateFitsFile()
            {
                // Determine the new file's name.
                char fmtBuffer[256];
                const std::time_t unixTimestamp = std::time(nullptr);
                const std::tm *utcDate = std::gmtime(&unixTimestamp);
                std::strftime(fmtBuffer, sizeof(fmtBuffer), "%Y-%m-%d-%H%M%S-%Z", utcDate);
                const std::string fileName = mLocalName + "_" + std::string(fmtBuffer);

                // Determine the new file's path.
                const std::string filePath = mRecordingDirectory + "/" + fileName + ".fits";
                mLogger.Debug("Creating new FITS file: %s", filePath.c_str());
                
                // Attempt to create it.
                int status = 0;
                fits_create_file(&mRecordingFile, filePath.c_str(), &status);
                
                // Housekeeping upon creation.
                if(!status) {
                    mLogger.Info("Recording %s to %s", mShmPath.c_str(), filePath.c_str());
                    mCurrentFilePath = filePath;
                    mRecordingFileSize = 0;
                }

                return !status;
            }

            bool CloseFitsFile()
            {
                int status = 0;
                fits_close_file(mRecordingFile, &status);

                if(!status) {
                    mLogger.Info("Succesfully closed %s", mCurrentFilePath.c_str());
                }

                mRecordingFile = nullptr;
                mCurrentFilePath = "";
                return !status;
            }
            
            void SignalError()
            {
                mError = true;
                Exit();
            }

            // Cfitsio
            std::vector<long> mDataDimensions;
            int mFitsDataType;
            int mFitsBPP;

            // Shared Memory
            ShmIfce<std::uint8_t> *mShmInterface;
            std::uint8_t *mInternalBuffer;
            std::size_t mShmBufferSize;
            std::uint64_t mElementCount;
            std::uint8_t mAtype;
            std::size_t mCnt0;
            IMAGE mShmImage;

            // Recording File.
            std::size_t mRecordingFileCapacity; // How many frames a file will contain.
            std::size_t mRecordingFileSize;     // How many frames are in the current recording file.
            std::size_t mNumRecordedFrames;      // How many frames in total we have recorded.
            fitsfile *mRecordingFile;

            //
            std::string mRecordingDirectory;
            std::string mCurrentFilePath;      // Path of the current fits file we are recording to.
            std::int64_t mFrameTarget;         // How many frames to record, or -1 to record indefinitely.
            std::string mLocalName;
            Log::Logger &mLogger;
            std::string mShmPath;
            bool mError;

            struct {
                std::uint64_t cnt0;
                std::uint64_t cnt1;
                std::uint64_t cnt2;
                double timestamp;
            } mFrameData;                   // Holds frame-specific data to be recorded.

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