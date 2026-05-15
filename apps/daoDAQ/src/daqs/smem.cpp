/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-05-08 22:03:13
 * @ Description: Implementation of shared-memory (smem) DAQ resource.
 */

#include <daqs.hpp>

using namespace Dao::DAQ;

SmemDAQ::SmemDAQ(SmemParameters const& params, ServerCallback doneCallback, ServerCallback errorCallback, Dao::Log::Logger& log) :
    IDAQ(doneCallback, errorCallback, params.absPath, log),
    params_(params),
    stopToken_(false),
    runSession_(false),
    smem_ {},
    cvPredicate_(false),
    daqThread_([this]() { this->daqThreadEntry(); }),
    sinkThread_([this]() { this->sinkThreadEntry(); }) {
    //
    establishResourceConnection();
}

/* Signals DAQ and sink threads to exit gracefully and blocks until they
 * have exited.
*/
SmemDAQ::~SmemDAQ() {
    {
        std::unique_lock lock(cvLock_);
        stopToken_ = true;
        cvPredicate_ = true;
        cvSignal_.notify_all();
    }

    log_.Debug(LOGFMT("joining daq thread for smem DAQ resource '{}'", resourceID_));
    daqThread_.join();

    log_.Debug(LOGFMT("joining sink thread for smem DAQ resource '{}'", resourceID_));
    sinkThread_.join();

    log_.Debug(LOGFMT("smem DAQ resource has been destroyed '{}'", resourceID_));
}

/* Connect to the shared memory resource; an exception is thrown
 * in the case connection fails.
*/
void SmemDAQ::establishResourceConnection() {
    if (DAO_SUCCESS != daoShmShm2Img(params_.absPath.c_str(), &smem_)) {
        std::string const err = fmt::format("could not connect to smem resource `{}`", params_.absPath);
        throw std::runtime_error(err);
    }

    sampleMemSize_ = smem_.memsize - sizeof(IMAGE_METADATA) - smem_.md->NBkw * sizeof(IMAGE_KEYWORD);
}

/* Configures calling thread according to the provided parameters
 * for high-throughput capture.
*/
void SmemDAQ::configureThread(Optional<CoreID> const& core) {
    if (!core)
        return;

    // pin thread to desired cpu core.
    cpu_set_t affinitySet;
    CPU_ZERO(&affinitySet);
    CPU_SET(core.value(), &affinitySet);
    if (!pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &affinitySet)) {
        log_.Error(LOGFMT("could not pin smem thread because {}", strerror(errno)));
    }
}

/* Signals both the DAQ and sink threads of this resource to begin a new DAQ
 * session; the session will either auto-finish (if applicable) or is ended
 * by calling the session finish method.
*/
void SmemDAQ::beginAcquisition(std::filesystem::path const& outputPath) {
    std::unique_lock lock(cvLock_);
    sessionOutputDir_ = outputPath;
    runSession_ = true;
    cvPredicate_ = true;
    cvSignal_.notify_all();
}

/* Signals both the DAQ and sink threads to end their in-progress DAQ session
 * and return to their blocked state where they await an unblock signal
 * to either begin a new DAQ session or to exit.
*/
void SmemDAQ::finishAcquisition() {
    std::unique_lock lock(cvLock_);
    cvPredicate_ = false;
    runSession_ = false;
}

/* Entry point for the DAQ thread; this is the thread that captures the
 * data samples from the shared memory resource and enqueues them for
 * sinking to the disk.
*/
void SmemDAQ::daqThreadEntry() {
    log_.Info(LOGFMT("DAQ thread has launched for smem resource {}", params_.absPath));
    configureThread(params_.daqThreadAffinity);

    while (1) {
        {
            std::unique_lock cvGuard(cvLock_);
            cvSignal_.wait(cvGuard, [&]() {
                log_.Debug(LOGFMT(
                    "DAQ thread for smem resource '{}' evaluated cv-predicate ({})",
                    resourceID_,
                    cvPredicate_ ? "resuming" : "waiting"
                ));
                return cvPredicate_;
            });
        }

        log_.Debug(LOGFMT("DAQ thread has awoken for smem resource {}", params_.absPath));

        if (stopToken_)
            break;

        try {
            acquireSamples();
        } catch (std::exception const& e) {
            finishAcquisition();
            errorCallback_(resourceID_);
            continue;
        }
    }

    log_.Info(LOGFMT("DAQ thread has terminated for smem resource {}", params_.absPath));
}

/* Entry point for the sink thread; this is the thread that processes
 * samples enqueued by the DAQ thread and writes them to the disk
 * in the data-format specified by our resource parameters.
*/
void SmemDAQ::sinkThreadEntry() {
    log_.Info(LOGFMT("Sink thread has launched for smem resource {}", params_.absPath));
    configureThread(params_.sinkThreadAffinity);

    while (1) {
        {
            std::unique_lock cvGuard(cvLock_);
            cvSignal_.wait(cvGuard, [&]() {
                log_.Debug(LOGFMT(
                    "Sink thread for smem resource '{}' evaluated cv-predicate ({})",
                    resourceID_,
                    cvPredicate_ ? "resuming" : "waiting"
                ));
                return cvPredicate_;
            });
        }

        log_.Debug(LOGFMT("Sink thread has awoken for smem resource {}", params_.absPath));

        if (stopToken_)
            break;

        try {
            sinkSamples();
        } catch (std::exception const& e) {
            finishAcquisition();
            errorCallback_(resourceID_);
            continue;
        }
    }

    log_.Info(LOGFMT("Sink thread has terminated for smem resource {}", params_.absPath));
}

/* Captures data-samples from the shared memory resource an enqueues them for
 * saving to the disk by the sink thread; samples are captured for the duration
 * of a DAQ session which ends when either the sink thread has met the desired
 * sample target (if one exists) or if the DAQ server has signalled the DAQ session
 * to finish.
*/
void SmemDAQ::acquireSamples() {
    log_.Debug(LOGFMT("DAQ thread has started sample collection for smem resource {}", params_.absPath));

    IMAGE_METADATA const volatile* smInfo_ = static_cast<IMAGE_METADATA const volatile*>(smem_.md);
    bool sampleAvailable { params_.eagerStart };
    size_t lastSampleId = smInfo_->cnt0;

    while (runSession_) {
        if (sampleAvailable) {
            size_t const sampleId = smInfo_->cnt0;

            QueueType qSample {
                IMAGE_METADATA {},
                std::make_unique<std::byte[]>(sampleMemSize_)
            };
            std::memcpy(&qSample.first, smem_.md, sizeof(IMAGE_METADATA));
            std::memcpy(qSample.second.get(), smem_.array.V, sampleMemSize_);
            bool const copyInterupted = (smInfo_->cnt0 > sampleId || 1 == smInfo_->write);

            std::lock_guard qGuard(qLock_);
            bool const queueHasSpace = params_.bufferLimit ? (queue_.size() < params_.bufferLimit.value()) : true;
            if (!copyInterupted && queueHasSpace) {
                queue_.push(std::move(qSample));
            }

            lastSampleId = sampleId;
        }

        sampleAvailable = smInfo_->cnt0 > lastSampleId;
    }

    log_.Debug(LOGFMT("DAQ thread has stopped sample collection for smem resource {}", params_.absPath));
}

/* Writes samples that are enqueued by the DAQ thread, to the disk in the desired
 * data-format for the duration of a DAQ session; note that a DAQ session lifetime
 * is defined either when the total number of samples have been successfully persisted
 * to the disk, or the DAQ server sends a signal to end our DAQ session.
*/
void SmemDAQ::sinkSamples() {
    log_.Debug(LOGFMT("Sink thread has started sample export for smem resource {}", params_.absPath));

    auto writer = std::make_unique<FitsWriter>(params_, sessionOutputDir_, *smem_.md);
    size_t nSamplesWritten {};

    {
        std::lock_guard qGuard(qLock_);
        while (!queue_.empty())
            queue_.pop();
    }

    while (runSession_) {
        if (params_.nSamples && params_.nSamples.value() == nSamplesWritten) {
            log_.Info(LOGFMT("DAQ session has reached sample-target for smem resource {} ({} samples written)", params_.absPath, nSamplesWritten));
            finishAcquisition();
            doneCallback_(resourceID_);
            continue;
        }

        std::optional<QueueType> qSample {};
        if (std::lock_guard qGuard(qLock_); !queue_.empty()) {
            qSample = std::move(queue_.front());
            queue_.pop();
        }

        if (qSample) {
            auto const& sample = qSample.value();
            writer->write(sample);
            ++nSamplesWritten;
            log_.Trace(LOGFMT(
                "Sink thread for daq resource '{}' has exported {} samples",
                resourceID_,
                nSamplesWritten,
                params_.nSamples ? fmt::format("{}/{}", nSamplesWritten, params_.nSamples.value()) : fmt::format("{}", nSamplesWritten)
            ));
        }
    }

    log_.Debug(LOGFMT("Sink thread has stopped sample export for smem resource {}", params_.absPath));
}


