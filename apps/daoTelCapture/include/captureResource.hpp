/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-29 13:55:06
 * @ Description: Capture Resource Interface
 */

#pragma once

#include <unordered_map>
#include <policies.hpp>
#include <filesystem>
#include <functional>
#include <daoShm.h>
#include <utility>
#include <thread>
#include <queue>
#include <mutex>

namespace Dao::Telemetry
{
    struct CaptureResource
    {
        CaptureResource(std::function<void()> raiseCaptureError) : targetAchieved_ { false }, raiseCaptureError_(raiseCaptureError) {}
        virtual ~CaptureResource() = default;

        virtual void beginCapture(std::filesystem::path const& outputPath) = 0;
        virtual void endCapture() = 0;

        bool targetAchieved() const { return targetAchieved_; };

        protected:
        std::atomic<bool> targetAchieved_;
        std::function<void()> raiseCaptureError_;
    };

    struct FileCaptureResource : public CaptureResource
    {
        FileCaptureResource(FilePolicy const& policy, std::function<void()> raiseCaptureError);
        void beginCapture(std::filesystem::path const& outputPath) override;
        void endCapture() override;

        FilePolicy const policies;
    };

    struct SmemCaptureResource : public CaptureResource
    {
        using QueueType = std::pair <IMAGE_METADATA, std::unique_ptr<std::byte[]>>;

        SmemCaptureResource(SmemPolicy const& policy, std::function<void()> raiseCaptureError);
        SmemCaptureResource(SmemCaptureResource const&) = delete;
        SmemCaptureResource(SmemCaptureResource&&) = delete;
        SmemCaptureResource& operator= (SmemCaptureResource const&) = delete;
        SmemCaptureResource& operator= (SmemCaptureResource&&) = delete;
        ~SmemCaptureResource();

        void beginCapture(std::filesystem::path const& outputPath) override;
        void endCapture() override;

        void pollMain();
        void exportMain();

        SmemPolicy const policies;

        private:
        std::thread pollThread_;
        std::thread exportThread_;
        std::atomic<bool> run_;
        std::atomic<bool> capture_;
        std::mutex queueLock_;
        std::queue <QueueType> exportQueue_;
        IMAGE smem_;

        void configureThread(Optional<CoreID> const core);
        void smemConnect();
    };
};
