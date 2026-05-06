/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:43:11
 * @ Description: Implements a file capture resource.
 */

#include <record.hpp>
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
        std::filesystem::path const fileOutputPath = outputPath / fileSourcePath.filename().string();

        try {
            std::filesystem::copy_file(fileSourcePath, fileOutputPath);
            sampleGoalMet_ = true;
        } catch (std::exception const& e) {
            raiseCaptureError_();
            return;
        }
    }

    void FileCaptureResource::endCapture() {}
}

