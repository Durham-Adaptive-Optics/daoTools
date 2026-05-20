/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-29 13:55:06
 * @ Description: Definitions of all supported DAQ resources.
 */

#pragma once

#include <configuration.hpp>
#include <condition_variable>
#include <unordered_map>
#include <filesystem>
#include <functional>
#include <dao.h>
#include <utility>
#include <thread>
#include <queue>
#include <mutex>
#include <log.hpp>
#include <sinks.hpp>

namespace Dao::DAQ
{
    using ServerErrorCallback = std::function<void(std::string const&, std::string const&)>;
    using ServerDoneCallback = std::function<void(std::string const&)>;

    /* Defines the common interface between all DAQ resource implementations.
     * Each DAQ resource enables the capture of data from a specific data source;
     * they all implement this common DAQ interface to enable the DAQ server to
     * interface with them.
    */
    struct IDAQ {
        IDAQ(ServerDoneCallback doneCallback, ServerErrorCallback errorCallback, std::string const& resourceID, Dao::Log::Logger& log)
            : resourceID(resourceID), doneCallback_(doneCallback), errorCallback_(errorCallback), log_(log) {
        }

        virtual ~IDAQ() = default;

        virtual void beginAcquisition(std::filesystem::path const& outputPath) = 0;
        virtual void finishAcquisition() = 0;

        std::string const resourceID;

        protected:
        ServerDoneCallback doneCallback_;
        ServerErrorCallback errorCallback_;
        Dao::Log::Logger& log_;
    };

    /* File DAQ Resource.
    */
    struct FileDAQ final : public IDAQ {
        FileDAQ(FileParameters const& params, ServerDoneCallback doneCallback, ServerErrorCallback errorCallback, Dao::Log::Logger& log);
        ~FileDAQ();

        void beginAcquisition(std::filesystem::path const& outputPath) override;
        void finishAcquisition() override;

        auto const& params() const { return params_; }

        private:
        FileParameters const params_;
    };

    /* Shared Memory (SMEM) DAQ Resource.
    */
    struct SmemDAQ final : public IDAQ {
        using QueueType = std::pair<IMAGE_METADATA, std::unique_ptr<std::byte[]>>;

        SmemDAQ(SmemParameters const& params, ServerDoneCallback doneCallback, ServerErrorCallback errorCallback, Dao::Log::Logger& log);
        SmemDAQ& operator=(SmemDAQ const&) = delete;
        SmemDAQ& operator=(SmemDAQ&&) = delete;
        SmemDAQ(SmemDAQ const&) = delete;
        SmemDAQ(SmemDAQ&&) = delete;
        ~SmemDAQ();

        void beginAcquisition(std::filesystem::path const& outputPath) override;
        void finishAcquisition() override;

        auto const& params() const { return params_; }

        private:
        SmemParameters const& params_;
        bool stopToken_;
        bool runSession_;
        std::mutex qLock_;  // ensures exclusive access to sample queue between DAQ and sink threads.
        std::queue<QueueType> queue_;
        IMAGE smem_;
        size_t sampleMemSize_;
        std::mutex cvLock_;  // ensures exclusive access to CV between DAQ and sink threads.
        bool cvPredicate_;
        std::condition_variable cvSignal_;
        std::filesystem::path sessionOutputDir_;
        std::thread daqThread_;
        std::thread sinkThread_;

        void establishResourceConnection();
        void configureThread(Optional<CoreID> const& core);

        void daqThreadEntry();
        void sinkThreadEntry();
        void acquireSamples();
        void sinkSamples();
    };
};
