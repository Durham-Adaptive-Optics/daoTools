/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:43:11
 * @ Description: Implements a file capture resource.
 */

#include <filesystem>
#include <daqs.hpp>
#include <log.hpp>

namespace Dao::DAQ
{
    FileDAQ::FileDAQ(FileParameters const& params, std::function<void()> doneCallback, std::function<void()> errorCallback, Dao::Log::Logger& log) :
        IDAQ(doneCallback, errorCallback, log),
        params_(params) {
    }

    /* Launch an async task to copy the file to the DAQ session output directory.
    */
    void FileDAQ::beginAcquisition(std::filesystem::path const& outputPath) {
        std::filesystem::path const fileSourcePath { params_.absPath };
        std::filesystem::path const fileOutputPath = outputPath / fileSourcePath.filename().string();

        std::thread([&]() {
            try {
                std::filesystem::copy_file(fileSourcePath, fileOutputPath);
            } catch (std::exception const& e) {
                log_.Critical(LOGFMT("copy failed for file resource '{}' because {}", fileSourcePath.string(), e.what()));
                this->errorCallback_();
                return;
            }
            this->doneCallback_();
        }).detach();
    }
}

