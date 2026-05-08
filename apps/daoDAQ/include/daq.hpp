/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-29 13:55:06
 * @ Description: DAQ Resource Definitions.
 */

#pragma once

#include <configuration.hpp>
#include <unordered_map>
#include <filesystem>
#include <functional>
#include <daoShm.h>
#include <utility>
#include <thread>
#include <queue>
#include <mutex>

namespace Dao::DAQ
{
    /* Defines the common interface between all DAQ resource implementations.
     * Each DAQ resource enables the capture of data from a specific data source;
     * they all implement this common DAQ interface to enable the DAQ server to
     * interface with them.
    */
    struct IDAQ {
        IDAQ(std::function<void()> doneCallback, std::function<void()> errorCallback)
            : doneCallback_(doneCallback), errorCallback_(errorCallback) {
        }

        virtual ~IDAQ() = default;

        virtual void startCapture(std::filesystem::path const& outputPath) = 0;
        virtual void finishCapture() = 0;

        protected:
        std::function<void()> doneCallback_;
        std::function<void()> errorCallback_;
    };

    /* DAQ resource for capturing files.
    */
    struct FileDAQ final : public IDAQ {
        FileDAQ(FileParameters const& params, std::function<void()> doneCallback, std::function<void()> errorCallback);

        void startCapture(std::filesystem::path const& outputPath) override;
        void finishCapture() override;

        auto const& params() const { return params_; }

        private:
        FileParameters const params_;
    };

    /* DAQ resource for capturing samples from Dao shared memory.
    */
    struct SmemDAQ final : public IDAQ {
        using QueueType = std::pair<IMAGE_METADATA, std::unique_ptr<std::byte[]>>;

        SmemDAQ(SmemParameters const& params, std::function<void()> doneCallback, std::function<void()> errorCallback);
        SmemDAQ(SmemDAQ const&) = delete;
        SmemDAQ(SmemDAQ&&) = delete;
        SmemDAQ& operator=(SmemDAQ const&) = delete;
        SmemDAQ& operator=(SmemDAQ&&) = delete;
        ~SmemDAQ();

        void startCapture(std::filesystem::path const& outputPath) override;
        void finishCapture() override;

        auto const& params() const { return params_; }

        private:
        SmemParameters const params_;
        std::thread pollThread_;
        std::thread exportThread_;
        std::atomic<bool> stopThreads_;
        std::atomic<bool> stopSession_;
        std::atomic<size_t> nExportedSamples_;
        std::mutex queueLock_;
        std::queue <QueueType> exportQueue_;
        std::unique_ptr<ExportBackend> exporter_;
        std::atomic<std::filesystem::path> outputDirectory_;
        IMAGE smem_;
        IMAGE_METADATA const volatile* smInfo_;
        size_t sampleByteSize_;


        void configureThread(Optional<CoreID> const& core);
        void createExporter();
        void connectToSharedMemory();
        void serviceExportQueue();
        void DAQThreadEntry();
        void serviceDAQSession();
        void runSessionExport();
    };
};
