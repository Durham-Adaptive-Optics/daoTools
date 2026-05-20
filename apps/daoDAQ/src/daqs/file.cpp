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
    FileDAQ::FileDAQ(FileParameters const& params, ServerDoneCallback doneCallback, ServerErrorCallback errorCallback, Dao::Log::Logger& log) :
        IDAQ(doneCallback, errorCallback, params.absPath, log),
        params_(params) {
    }

    FileDAQ::~FileDAQ() {
        log_.Debug(LOGFMT("file DAQ resource has been destroyed '{}'", resourceID));
    }

    /* Launch an async task to copy the file to the DAQ session output directory.
    */
    void FileDAQ::beginAcquisition(std::filesystem::path const& outputPath) {
        std::thread([this, outputPath]() {
            std::filesystem::path const fileSourcePath { params_.absPath };
            std::filesystem::path const fileOutputPath = outputPath / fileSourcePath.filename().string();

            try {
                std::filesystem::copy_file(fileSourcePath, fileOutputPath);
            } catch (std::exception const& e) {
                auto const err = LOGFMT("copying '{}' failed because {}", fileSourcePath.string(), e.what());
                errorCallback_(resourceID, err);
                return;
            }

            log_.Debug(LOGFMT("copy succeeded for file DAQ resource '{}'", fileSourcePath.string()));
            doneCallback_(resourceID);
        }).detach();
    }

    void FileDAQ::finishAcquisition() {};
}

