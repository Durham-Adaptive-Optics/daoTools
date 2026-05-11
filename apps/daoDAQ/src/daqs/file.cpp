/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:43:11
 * @ Description: Implements a file capture resource.
 */

#include <filesystem>
#include <daqs.hpp>

namespace Dao::DAQ
{
    FileDAQ::FileDAQ(FileParameters const& params, std::function<void()> doneCallback, std::function<void()> errorCallback, Dao::Log::Logger& log) :
        IDAQ(doneCallback, errorCallback, log),
        params_(params) {
    }

    /* Copy the file to the DAQ session output directory as soon as we have been instructed
     * to begin capture.
    */
    void FileDAQ::beginAcquire(std::filesystem::path const& outputPath) {
        std::filesystem::path const fileSourcePath { params_.absPath };
        std::filesystem::path const fileOutputPath = outputPath / fileSourcePath.filename().string();

        try {
            std::filesystem::copy_file(fileSourcePath, fileOutputPath);
        } catch (std::exception const& e) {
            errorCallback_();
            return;
        }

        doneCallback_();
    }

    void FileDAQ::endAcquire() {
        //
    }
}

