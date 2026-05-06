/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:43:06
 * @ Description: Implements a (dao) shared-memory capture resource.
 */

#include <unordered_map>
#include <record.hpp>
#include <pthread.h>
#include <cstring>

namespace Dao::Telemetry
{
    SmemCaptureResource::SmemCaptureResource(SmemPolicy const& policy, std::function<void()> raiseCaptureError) :
        CaptureResource { raiseCaptureError },
        policies { policy },
        pollThread_([this]() { pollMain(); }),
        exportThread_([this]() { exportMain(); }),
        stopToken_ { true },
        capture_ { false },
        smem_ {},
        exporter_ {}
    {
        connectToSharedMemory();
        createExporter();
    }

    SmemCaptureResource::~SmemCaptureResource()
    {
        stopToken_.store(false);
        pollThread_.join();
        exportThread_.join();
    }

    //

    void SmemCaptureResource::pollMain()
    {
        // thread setup..
        configureThread(policies.pollThreadAffinity);

        // restartable capture session..
        while (stopToken_.load()) {
            try {
                runSessionPoll();
            } catch (std::exception const& e) {
                endCapture();
                raiseCaptureError_();
            }
        }
    }

    void SmemCaptureResource::exportMain()
    {
        // thread setup..
        configureThread(policies.exportThreadAffinity);

        // restartable capture session..
        while (stopToken_.load()) {
            try {
                runSessionExport();
            } catch (std::exception const& e) {
                endCapture();
                raiseCaptureError_();
            }
        }
    }

    void SmemCaptureResource::runSessionPoll()
    {
        IMAGE_METADATA const volatile* smInfo = static_cast<IMAGE_METADATA const volatile*>(smem_.md);
        size_t const sampleByteSize = smem_.memsize - sizeof(IMAGE_METADATA) - smem_.md->NBkw * sizeof(IMAGE_KEYWORD);
        size_t sampleId {}, lastSampleId {};
        bool sampleReady { true };

        while (capture_.load()) {
            if (sampleReady && !sampleGoalMet_) {
                std::scoped_lock qlock(queueLock_);

                QueueType entry { IMAGE_METADATA {}, std::make_unique<std::byte[]>(sampleByteSize) };
                std::memcpy(&entry.first, smem_.md, sizeof(IMAGE_METADATA));
                std::memcpy(entry.second.get(), smem_.array.V, sampleByteSize);

                bool const copyInterupted = (1 == smInfo->write || smInfo->cnt0 > sampleId);
                bool const queueHasSpace = policies.bufferLimit ? (policies.bufferLimit.value() < exportQueue_.size()) : true;
                if (!copyInterupted && queueHasSpace) {
                    exportQueue_.push(std::move(entry));
                }

                lastSampleId = sampleId;
            }

            sampleId = smInfo->cnt0;
            sampleReady = (sampleId > lastSampleId);
        }
    }

    void SmemCaptureResource::runSessionExport()
    {
        exporter_->reset(outputDirectory_.load());
        size_t nSamplesCaptured {};

        while (capture_.load()) {
            std::optional<QueueType> entry {};
            if (std::scoped_lock qlock(queueLock_); !exportQueue_.empty()) {
                entry = std::move(exportQueue_.front());
                exportQueue_.pop();
            }

            if (entry && !sampleGoalMet_) {
                QueueType const& sample = entry.value();
                exporter_->put(sample);
                ++nSamplesCaptured;
                if (policies.nSamples) {
                    sampleGoalMet_.store(nSamplesCaptured == policies.nSamples.value());
                }
            }
        }

        exporter_->finish(); // @todo what if we throw and don't do this -> make exporter RAII in this scope.
    }

    //

    void SmemCaptureResource::beginCapture(std::filesystem::path const& outputPath)
    {
        outputDirectory_.store(outputPath);
        sampleGoalMet_.store(false);
        capture_.store(true);
    }

    void SmemCaptureResource::endCapture()
    {
        capture_.store(false);
    }

    void SmemCaptureResource::configureThread(Optional<CoreID> const& core)
    {
        if (!core)
            return;

        cpu_set_t affinitySet;
        CPU_ZERO(&affinitySet);
        CPU_SET(core.value(), &affinitySet);
        if (!pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &affinitySet)) {
            // @todo log pinning failure. 
        }
    }

    void SmemCaptureResource::connectToSharedMemory()
    {
        if (DAO_SUCCESS != daoShmShm2Img(policies.absPath.c_str(), &smem_)) {
            throw std::runtime_error("failed to connect smem resource");
        }
    }

    void SmemCaptureResource::createExporter()
    {
        switch (policies.format) {
            case ExportFormat::FITS: {
                exporter_ = std::make_unique<FitsExporter>(policies, *smem_.md);
            } break;

            case ExportFormat::NUMPY: {
                throw std::runtime_error("exporter not implemented yet!");
            } break;
        }
    }
}

