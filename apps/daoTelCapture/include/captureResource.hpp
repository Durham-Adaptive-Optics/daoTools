/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-29 13:55:06
 * @ Description: Capture Resource Interface
 */

#include <filesystem>
#include <policies.hpp>

namespace Dao::Telemetry
{
    struct CaptureResource
    {
        virtual void beginCapture(std::filesystem::path const& outputPath) = 0;
        virtual void endCapture() = 0;

        virtual bool targetAchieved() = 0;
    };

    struct FileCaptureResource : public CaptureResource
    {
        FileCaptureResource(FilePolicy const& policy);
        void beginCapture(std::filesystem::path const& outputPath) override;
        void endCapture() override;

        FilePolicy policies_;
    };

    struct SmemCaptureResource : public CaptureResource
    {
        SmemCaptureResource(SmemPolicy const& policy);
        void beginCapture(std::filesystem::path const& outputPath) override;
        void endCapture() override;

        SmemPolicy policies_;
    };
};
