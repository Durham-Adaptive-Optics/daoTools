/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:43:11
 * @ Description: Implements a file capture resource.
 */

#include <captureResource.hpp>
#include <filesystem>

namespace Dao::Telemetry
{
    FileCaptureResource::FileCaptureResource(FilePolicy const& policy, std::function<void()> raiseCaptureError) :
        CaptureResource { raiseCaptureError },
        policies(policy)
    {
    }

    void FileCaptureResource::beginCapture(std::filesystem::path const& outputPath)
    {
        std::filesystem::path const fileSourcePath { policies.absPath };
        std::string const fileOutputName = policies.saveAsName ? policies.saveAsName.value() : fileSourcePath.filename().string();
        std::filesystem::path const fileOutputPath = outputPath / fileOutputName;

        try {
            std::filesystem::copy_file(fileSourcePath, fileOutputPath);
            targetAchieved_ = true;
        } catch (std::exception const& e) {
            raiseCaptureError_();
            return;
        }
    }

    void FileCaptureResource::endCapture() {}
}

