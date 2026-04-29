class FileRecorder : public Recorder
{
    public:
    /**
     * Constructs FileRecorder.
     * @param logger Application logger.
     * @param filePath Target file path.
     * @param errorFlag Application error flag.
     */
    FileRecorder(
        Dao::Log::Logger& logger,
        std::string filePath,
        volatile bool& errorFlag
    )
        :
        mErrorFlag(errorFlag),
        mFilePath(filePath),
        mLogger(logger),
        mCopying(false)
    {
    }

    /**
     * Destructs FileRecorder.
     */
    ~FileRecorder()
    {
    }

    /**
     * Copies target file to the specified session directory.
     * @param sessionDirectory Directory path for where the file should be copied to.
     */
    void Start(const std::string& sessionDirectory) override
    {
        mCopying = true;

        // construct destination path. 
        std::string filename = mFilePath;
        auto x = mFilePath.find_last_of("/");
        if (x != std::string::npos) filename = mFilePath.substr(x + 1);
        std::string destPath = sessionDirectory + "/" + filename;

        // copy file to destination.
        mLogger.Debug("Copying file '%s' to '%s'", mFilePath.c_str(), destPath.c_str());

        try {
            std::filesystem::copy_file(mFilePath, destPath);
            mCopying = false;
        } catch (const std::exception& e) {
            mLogger.Error("Failed to copy file '%s' because: %s", mFilePath.c_str(), e.what());
            mErrorFlag = true;
            return;
        }

        mLogger.Info("Copied file '%s'", mFilePath.c_str());
    }

    /**
     * Provides a method for querying if the file copy is in-progress.
     */
    bool IsRecording() override
    {
        return mCopying;
    }

    private:
    /**
      * Member Variables
    */
    Dao::Log::Logger& mLogger;
    volatile bool& mErrorFlag;
    std::string mFilePath;
    bool mCopying;
};