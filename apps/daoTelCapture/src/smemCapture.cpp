/**
 * Macro for cfitsio error handling.
 */
#define FITS_CHECK(expr)                                                                            \
        (expr);                                                                                     \
        if (fitsError) {                                                                            \
            char err_msg[FLEN_ERRMSG];                                                              \
            fits_get_errstatus(fitsError, err_msg);                                                 \
            m_log.Error("%s failed to export data: %s", m_thread_name.c_str(), err_msg);          \
            TriggerError();                                                                         \
            return;                                                                                 \
        }

/**
 * Struct housing all shared memory metadata
 * and array data that is to be recorded.
 */
struct SharedMemoryFrame
{
    // The following fields change per shared memory instance.
    std::vector<long> size;
    uint64_t nElements;
    uint8_t atype;

    // The following fields change per shared memory frame.
    int64_t atime;
    uint64_t cnt0;
    uint64_t cnt1;
    uint64_t cnt2;
    int8_t* data;
};


class SharedMemoryRecorder : public Recorder
{
    public:
    /**
     * Constructs SharedMemoryRecorder class, creating exporter
     * and poller objects and spawing their threads.
     * @param target Shared memory target information that is to be recorded by this class.
     * @param logger Application logger.
     * @param errorFlag Application error flag.
     */
    SharedMemoryRecorder(const Target& target, Dao::Log::Logger& logger, volatile bool& errorFlag)
        :
        mExporter(mFrameQueue, logger, target.source, target.fileLimit, target.recordingLimit, errorFlag),
        mPoller(mFrameQueue, logger, target.source, target.pollingCore, target.bufferLimit)
    {
        mExporter.Spawn();
        mPoller.Spawn();
    }

    /**
     * Destructs SharedMemoryRecorder class, ensures exporter and poller
     * threads terminate correctly.
     */
    ~SharedMemoryRecorder()
    {
        mExporter.Join();
        mPoller.Join();
    }

    /**
     * Starts data acquisition for the provided shared memory target.
     * @param sessionDirectory Directory path for where data files should be stored.
     */
    void Start(const std::string& sessionDirectory) override
    {
        mExporter.SetStorageDirectory(sessionDirectory);
        mExporter.Start();
        mPoller.Start();
    }

    /**
     * Stops data acquisition for the provided shared memory target.
     */
    void Stop() override
    {
        mExporter.Stop();
        mPoller.Stop();
    }

    /**
     * Provides a method for querying if data acquisition for the shared
     * memory target is in-progress.
     */
    bool IsRecording() override
    {
        return mExporter.IsRecording();
    }

    private:
    /**
      * Member Variables
     */
    Dao::ThreadSafeQueue<SharedMemoryFrame> mFrameQueue;
    SharedMemoryExporter mExporter;
    SharedMemoryPoller mPoller;
};


class SharedMemoryExporter : public Dao::Thread
{
    public:
    /**
     * Constructs SharedMemoryExporter, creating a dedicated dao thread for exporting
     * queued shared memory frames to FITS data files on disk.
     * @param frameQueue Reference to a shared thread-safe queue where frames are deposited for recording.
     * @param logger Application logger.
     * @param shmPath Target shared memory path.
     * @param datafileCapacity Desired frame capacity for each FITS data file.
     * @param exportLimit Desired session recording limit for this target.
     * @param errorFlag Application error flag.
     */
    SharedMemoryExporter(
        Dao::ThreadSafeQueue<SharedMemoryFrame>& frameQueue,
        Dao::Log::Logger& logger,
        std::string shmPath,
        size_t datafileCapacity,
        size_t exportLimit,
        volatile bool& errorFlag
    )
        :
        Dao::Thread("Export_" + daoShmLocalName(shmPath), logger),
        mShmLocalName(daoShmLocalName(shmPath)),
        mDatafileCapacity(datafileCapacity),
        mFrameQueue(frameQueue),
        mStorageDirectory(""),
        mExportLimit(exportLimit),
        mErrorFlag(errorFlag),
        mRecording(false)
    {
    }

    /**
     * Destructs SharedMemoryExporter, ensuring the dedicated export
     * thread is correctly terminated.
     */
    ~SharedMemoryExporter()
    {
        Exit();
    }

    /**
     * Sets the directory to be used for storing target telemetry files.
     * @param directory Directory path.
     */
    void SetStorageDirectory(const std::string& directory)
    {
        mStorageDirectory = directory;
    }

    /**
     * Provides a method for querying if the export thread is actively
     * exporting frames from the queue, or if instead it is waiting to
     * be started.
     */
    bool IsRecording() const { return mRecording; }

    private:
    /**
     * Sets the application error flag and ensures the export thread
     * is stopped.
     */
    void TriggerError()
    {
        m_log.Info("%s triggered application error", m_thread_name.c_str());
        mErrorFlag = true;
        Stop();
    }

    /**
     * Handles closing a FITS datafile.
     */
    bool CloseDatafile()
    {
        m_log.Debug("%s closing datafile", m_thread_name.c_str());

        int error = 0;
        fits_close_file(mDatafile, &error);
        mDatafile = nullptr;

        if (error) {
            char err_msg[FLEN_ERRMSG];
            fits_get_errstatus(error, err_msg);
            m_log.Error("Failed to close fits datafile (%s): %s", m_thread_name.c_str(), err_msg);
        }

        return !error;
    }

    /**
     * Routine that runs before the export thread's core loop
     * begins.
     */
    void OnceOnStart() override
    {
        assert(mStorageDirectory != "");
        mDatafile = nullptr;
        mExportedFrames = 0;
        mDatafileCount = 0;
        mDatafileSize = 0;
        mRecording = true;
    }

    /**
     * Routine that runs after the export thread's core loop
     * is stopped.
     */
    void OnceOnStop() override
    {
        mRecording = false;
        if (mDatafile && !CloseDatafile())
            TriggerError();
    }

    /**
     * Export thread core loop.
     */
    void RestartableThread() override
    {
        // Automatically stop once we have exported the desired number of frames (if applicable).
        if (mExportLimit && mExportedFrames == mExportLimit) {
            m_log.Debug("%s reached export limit", m_thread_name.c_str());
            Stop();
            return;
        }

        // Close the current datafile if it has reach capacity (if applicable).
        if (mDatafile && mDatafileCapacity && mDatafileSize == mDatafileCapacity) {
            m_log.Debug("%s datafile reached capacity (%d/%d)", m_thread_name.c_str(), mDatafileSize, mDatafileCapacity);
            if (!CloseDatafile()) {
                TriggerError();
                return;
            }
        }

        // Create datafile if we haven't got one.
        if (!mDatafile) {
            int status = 0;
            std::string fileName = mShmLocalName + "_" + std::to_string(mDatafileCount + 1);
            std::string filePath = mStorageDirectory + "/" + fileName + ".fits";
            m_log.Debug("Creating datafile %s [%s]", filePath.c_str(), m_thread_name.c_str());
            fits_create_file(&mDatafile, filePath.c_str(), &status);

            if (status) {
                char err_msg[FLEN_ERRMSG];
                fits_get_errstatus(status, err_msg);
                m_log.Error("Failed to create datafile because: %s [%s]", err_msg, m_thread_name.c_str());
                TriggerError();
                return;
            }

            mDatafileSize = 0;
            ++mDatafileCount;
        }

        // Export frame from queue.
        if (mFrameQueue.size()) {
            int fitsError = 0;
            SharedMemoryFrame frame = mFrameQueue.pop();
            FITS_CHECK(fits_create_img(mDatafile, mDaoToFitsDest.at(frame.atype), frame.size.size(), frame.size.data(), &fitsError));
            FITS_CHECK(fits_write_key(mDatafile, TBYTE, "atype", &frame.atype, nullptr, &fitsError));
            FITS_CHECK(fits_write_key(mDatafile, TLONGLONG, "atime", &frame.atime, nullptr, &fitsError));
            FITS_CHECK(fits_write_key(mDatafile, TULONGLONG, "cnt0", &frame.cnt0, nullptr, &fitsError));
            FITS_CHECK(fits_write_key(mDatafile, TULONGLONG, "cnt1", &frame.cnt1, nullptr, &fitsError));
            FITS_CHECK(fits_write_key(mDatafile, TULONGLONG, "cnt2", &frame.cnt2, nullptr, &fitsError));
            FITS_CHECK(fits_write_img(mDatafile, mDaoToFitsSrc.at(frame.atype), 1, frame.nElements, frame.data, &fitsError));
            free(frame.data);
            mExportedFrames++;
            mDatafileSize++;
        }
    }

    /**
      * Member Variables
    */
    std::string mShmLocalName;
    Dao::ThreadSafeQueue<SharedMemoryFrame>& mFrameQueue;
    std::string mStorageDirectory;
    volatile bool& mErrorFlag;
    size_t mDatafileCapacity;
    size_t mExportedFrames;
    size_t mDatafileCount;
    size_t mDatafileSize;
    size_t mExportLimit;
    fitsfile* mDatafile;
    bool mRecording;

    const std::unordered_map<uint8_t, int> mDaoToFitsDest {
        {_DATATYPE_UINT8, BYTE_IMG},
        {_DATATYPE_INT8, SBYTE_IMG},
        {_DATATYPE_UINT16, USHORT_IMG},
        {_DATATYPE_INT16, SHORT_IMG},
        {_DATATYPE_UINT32, ULONG_IMG},
        {_DATATYPE_INT32, LONG_IMG},
        {_DATATYPE_UINT64, ULONGLONG_IMG},
        {_DATATYPE_INT64, LONGLONG_IMG},
        {_DATATYPE_FLOAT, FLOAT_IMG},
        {_DATATYPE_DOUBLE, DOUBLE_IMG}
    };

    const std::unordered_map<uint8_t, int> mDaoToFitsSrc {
        {_DATATYPE_UINT8, TBYTE},
        {_DATATYPE_INT8, TSBYTE},
        {_DATATYPE_UINT16, TUSHORT},
        {_DATATYPE_INT16, TSHORT},
        {_DATATYPE_UINT32, TUINT},
        {_DATATYPE_INT32, TINT},
        {_DATATYPE_UINT64, TULONGLONG},
        {_DATATYPE_INT64, TLONGLONG},
        {_DATATYPE_FLOAT, TFLOAT},
        {_DATATYPE_DOUBLE, TDOUBLE}
    };
};

class SharedMemoryPoller : public Dao::Thread
{
    public:
    /**
     * Constructs SharedMemoryPoller, creating a dedicated dao thread
     * for shared memory polling, and loads the target shared memory.
     * @param frameQueue Reference to a shared thread-safe queue where frames will be deposited.
     * @param logger Application logger.
     * @param shmPath Target shared memory path.
     * @param core Desired core affinity for polling thread.
     * @param errorFlag Application error flag.
     * @param bufferLimit Max capacity (in frames) of the frame queue (if zero then queue can grow unbounded).
     */
    SharedMemoryPoller(
        Dao::ThreadSafeQueue<SharedMemoryFrame>& frameQueue,
        Dao::Log::Logger& logger,
        std::string shmPath,
        int core,
        size_t bufferLimit
    )
        :
        Dao::Thread("Poll_" + daoShmLocalName(shmPath), logger, core),
        mFrameQueue(frameQueue),
        mBufferLimit(bufferLimit)
    {
        LoadSharedMemory(shmPath);
    }

    /**
     * Destructs SharedMemoryPoller, unloading the shared memory
     * and ensuring the dedicated polling thread is terminated
     * correctly.
     */
    ~SharedMemoryPoller()
    {
        DAO_PROFILE_EXPORT(mProfile);
        UnloadSharedMemory();
        Exit();
    }

    /**
     * Routine that runs before polling begins to setup
     * internal state.
     */
    void OnceOnStart() override
    {
        mInitialGrab = true;
    }

    private:
    /**
     * Opens dao shared memory from path and stores required
     * information to be used when queuing frames.
     * @param shmName Shared memory path
     */
    void LoadSharedMemory(const std::string& shmName)
    {
        // open shared memory.
        if (daoShmShm2Img(shmName.c_str(), &mImage) != DAO_SUCCESS) {
            throw std::runtime_error("Failed to open shared memory");
        }

        // re-interpret metadata pointer as volatile.
        mMetadata = (volatile IMAGE_METADATA*)mImage.md;

        // extract frame data type.
        mFrameDatatype = mMetadata->atype;

        // extract element count.
        mFrameElementCount = mMetadata->nelement;

        // extract frame dimensions (revsered order for FITS).
        mFrameSize.resize(mMetadata->naxis);
        for (size_t i = 0; i < mMetadata->naxis; ++i)
            mFrameSize[mMetadata->naxis - 1 - i] = (long)mMetadata->size[i]; // cast uint32_t -> long for cfitsio.

        // calculate memory footprint of frame data.
        mFrameFootprint = mDaoTypeSizes.at(mFrameDatatype) * mFrameElementCount;
    }

    /**
     * Ensures the dao shared memory handle is correctly closed.
     */
    void UnloadSharedMemory()
    {
        daoShmCloseShm(&mImage);
    }

    /**
     * Checks if the frame data is safe to record, if not
     * then it returns false and logs the reason.
     * @param arrayBuffer Pointer to allocated frame array buffer
     * @return True if frame can be recorded, False is not.
     */
    inline
        bool FrameGood(void* arrayBuffer)
    {
        bool good = true;

        if (!arrayBuffer) {
            m_log.Warning("%s dropped frame because: failed to allocate array buffer", m_thread_name.c_str());
            good = false;
        }

        if (mMetadata->write == 1 || mMetadata->cnt0 > mCnt0) {
            m_log.Warning("%s dropped frame because: shared memory written to during copy", m_thread_name.c_str());
            good = false;
        }

        if (mBufferLimit && mFrameQueue.size() == mBufferLimit) {
            m_log.Warning("%s dropped frame because: frame queue is at capacity (%d frames)", m_thread_name.c_str(), mBufferLimit);
            good = false;
        }

        return good;
    }

    /**
     * Core polling thread loop. Awaits a frame to be written
     * into the shared memory and then queues its data for recording.
     */
    void RestartableThread() override
    {
        const uint64_t cnt0 = mMetadata->cnt0;
        if (mInitialGrab || cnt0 > mCnt0) {
            //
            DAO_PROFILE_NEW_FRAME(mProfile);
            mInitialGrab = false;
            mCnt0 = cnt0;

            // Copy frame data out of shared memory.
            DAO_PROFILE_START(mProfile, "Metadata-Copy")
                SharedMemoryFrame frame;
            frame.size = mFrameSize;
            frame.atype = mFrameDatatype;
            frame.cnt0 = mMetadata->cnt0;
            frame.cnt1 = mMetadata->cnt1;
            frame.cnt2 = mMetadata->cnt2;
            frame.nElements = mFrameElementCount;
            frame.atime = mMetadata->atime.tsfixed.secondlong;
            DAO_PROFILE_STOP(mProfile, "Metadata-Copy")

                DAO_PROFILE_START(mProfile, "Array-Copy")
                frame.data = (int8_t*)malloc(mFrameFootprint);
            if (frame.data) {
                memcpy(frame.data, mImage.array.V, mFrameFootprint);
            }
            DAO_PROFILE_STOP(mProfile, "Array-Copy")

            // Validate frame and push onto queue.
                DAO_PROFILE_START(mProfile, "Enqueue")
                if (FrameGood(frame.data)) {
                    mFrameQueue.push(frame);
                }
                else if (frame.data) {
                    free(frame.data);
                }
            DAO_PROFILE_STOP(mProfile, "Enqueue")
        }
    }

    /**
      * Member Variables
    */
    DAO_PROFILE(mProfile, std::chrono::nanoseconds, "Metadata-Copy", "Array-Copy", "Enqueue");
    Dao::ThreadSafeQueue<SharedMemoryFrame>& mFrameQueue;
    volatile IMAGE_METADATA* mMetadata;
    std::vector<long> mFrameSize;
    size_t mFrameElementCount;
    size_t mFrameFootprint;
    uint8_t mFrameDatatype;
    size_t mBufferLimit;
    bool mInitialGrab;
    uint64_t mCnt0;
    IMAGE mImage {};

    const std::unordered_map<uint8_t, uint8_t> mDaoTypeSizes { // lookup table from dao types to byte sizes.
        {_DATATYPE_UINT8, SIZEOF_DATATYPE_UINT8},
        {_DATATYPE_INT8, SIZEOF_DATATYPE_INT8},
        {_DATATYPE_UINT16, SIZEOF_DATATYPE_UINT16},
        {_DATATYPE_INT16, SIZEOF_DATATYPE_INT16},
        {_DATATYPE_UINT32, SIZEOF_DATATYPE_UINT32},
        {_DATATYPE_INT32, SIZEOF_DATATYPE_INT32},
        {_DATATYPE_UINT64, SIZEOF_DATATYPE_UINT64},
        {_DATATYPE_INT64, SIZEOF_DATATYPE_INT64},
        {_DATATYPE_FLOAT, SIZEOF_DATATYPE_FLOAT},
        {_DATATYPE_DOUBLE, SIZEOF_DATATYPE_DOUBLE}
    };
};