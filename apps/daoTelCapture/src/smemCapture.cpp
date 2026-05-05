/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:43:06
 * @ Description: Implements a (dao) shared-memory capture resource.
 */

#include <captureResource.hpp>
#include <unordered_map>
#include <pthread.h>
#include <cstring>

namespace Dao::Telemetry
{
    SmemCaptureResource::SmemCaptureResource(SmemPolicy const& policy, std::function<void()> raiseCaptureError) :
        CaptureResource { raiseCaptureError },
        policies { policy },
        pollThread_ { [this]() { try { pollMain(); } catch (...) { raiseCaptureError_(); } } },
        exportThread_ { [this]() { try { exportMain(); } catch (...) { raiseCaptureError_(); } } },
        run_ { true },
        capture_ { false },
        smem_ {}
    {
        smemConnect();
    }

    SmemCaptureResource::~SmemCaptureResource()
    {
        run_.store(false);
        pollThread_.join();
        exportThread_.join();
    }

    void SmemCaptureResource::configureThread(Optional<CoreID> const core)
    {
        // attempt to pin calling thread to the specified core (if provided).
        if (core) {
            cpu_set_t affinitySet;
            CPU_ZERO(&affinitySet);
            CPU_SET(core.value(), &affinitySet);
            if (!pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &affinitySet)) {
                // @todo log pinning failure. 
            }
        }
    }

    void SmemCaptureResource::smemConnect()
    {
        if (DAO_SUCCESS != daoShmShm2Img(policies.absPath.c_str(), &smem_)) {
            throw std::runtime_error("failed to connect smem resource");
        }
    }

    void SmemCaptureResource::beginCapture(std::filesystem::path const& outputPath)
    {
        targetAchieved_.store(false);
        capture_.store(true);
    }

    void SmemCaptureResource::endCapture() { capture_.store(false); }

    void SmemCaptureResource::pollMain()
    {
        // thread setup..
        configureThread(policies.pollThreadAffinity);
        IMAGE_METADATA const volatile* smInfo = static_cast<IMAGE_METADATA const volatile*>(smem_.md);
        size_t const sampleByteSize = smem_.memsize - sizeof(IMAGE_METADATA) - smem_.md->NBkw * sizeof(IMAGE_KEYWORD);

        // restartable capture session..
        while (run_.load()) {
            size_t sampleId {}, lastSampleId {};
            bool sampleReady { true };

            while (capture_.load()) { // capture session samples..
                if (sampleReady && !targetAchieved_) {
                    std::scoped_lock qlock(queueLock_);

                    // copy sample
                    QueueType entry { IMAGE_METADATA {}, std::make_unique<std::byte[]>(sampleByteSize) };
                    std::memcpy(&entry.first, smem_.md, sizeof(IMAGE_METADATA));
                    std::memcpy(entry.second.get(), smem_.array.V, sampleByteSize);

                    // enqueue sample
                    bool const copyInterupted = (1 == smInfo->write || smInfo->cnt0 > sampleId);
                    bool const queueHasSpace = policies.bufferLimit ? (policies.bufferLimit.value() < exportQueue_.size()) : true;
                    if (!copyInterupted && queueHasSpace) {
                        exportQueue_.push(std::move(entry));
                    }

                    // housekeeping
                    lastSampleId = sampleId;
                }

                // await new sample..
                sampleId = smInfo->cnt0;
                sampleReady = (sampleId > lastSampleId);
            }
        }
    }

    void SmemCaptureResource::exportMain()
    {
        configureThread(policies.exportThreadAffinity);
    }
}

