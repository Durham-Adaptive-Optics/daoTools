/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-05-08 22:03:13
 * @ Description: Implementation of shared-memory (smem) DAQ resource.
 */

#include <daqs.hpp>

using namespace Dao::DAQ;

SmemDAQ::SmemDAQ(SmemParameters const& params, std::function<void()> doneCallback, std::function<void()> errorCallback, Dao::Log::Logger& log) :
    IDAQ(doneCallback, errorCallback, log),
    stopToken_(false), runSession_(false)
    daqThread_([this]() { this->daqThreadEntry(); }),
    sinkThread_([this]() { this->sinkThreadEntry(); }),
    smem_ {},
    sampleByteSize_ {} {

    establishResourceConnection();
}

Smem::~Smem() {
    endThreads();
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
        std::string const err = fmt::format("could not pin smem thread because {}", strerror(errno));
        throw std::runtime_error(err);
    }
}

/* Signals both the DAQ and sink threads of this resource to begin a new DAQ
 * session; the session will either auto-finish (if applicable) or is ended
 * by calling the session finish method.
*/
void SmemDAQ::beginDAQSession(std::filesystem::path const& outputPath) {
    runSession_.store(true);
    bSignal_.notify_all();
}

/* Signals both the DAQ and sink threads to end their in-progress DAQ session
 * and return to their blocked state where they await an unblock signal
 * to either begin a new DAQ session or to exit.
*/
void SmemDAQ::finishDAQSession() {
    runSession_.store(false);
}

/* Signals DAQ and sink threads to exit gracefully and blocks until they
 * have exited.
*/
void SmemDAQ::endThreads() {
    stopToken_.store(true);
    bSignal_.notify_all();

    daqThread_.join();
    sinkThread_.join();
}

/* Entry point for the DAQ thread; this is the thread that captures the
 * data samples from the shared memory resource and enqueues them for
 * sinking to the disk.
*/
void SmemDAQ::daqThreadEntry() {
    log_.Info(LOGFMT("DAQ thread has started for SMEM resource {}", params_.absPath));
    configureThread(params_.daqThreadAffinity);

    while (1) {
        bSignal_.wait(bLock_); // @todo learn how to use CVs.

        log_.Info(LOGFMT("DAQ thread has unblocked for SMEM resource {}", params_.absPath));
        if (!stopToken_)
            break;

        try {
            runSessionDAQ();
        } catch (std::exception const& e) {
            finishDAQSession();
            errorCallback_();
            continue;
        }
    }

    log_.Info(LOGFMT("DAQ thread has finished for SMEM resource {}", params_.absPath));
}

/* Captures data-samples from the shared memory resource an enqueues them for
 * saving to the disk by the sink thread; samples are captured for the duration
 * of a DAQ session which ends when either the sink thread has met the desired
 * sample target (if one exists) or if the DAQ server has signalled the DAQ session
 * to finish.
*/
void SmemDAQ::runSessionDAQ() {
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
            bool const copyInterupted = (1 == smInfo_->write || smInfo_->cnt0 > sampleId);

            std::lock_guard qGuard(qLock_);
            bool const queueHasSpace = (params_.bufferLimit.value() < exportQueue_.size()) : true;
            if (!copyInterupted && params_.bufferLimit && queueHasSpace) {
                exportQueue_.push(std::move(qSample));
            }

            lastSampleId = sampleId;
        }

        sampleAvailable = smInfo_->cnt0 > lastSampleId;
    }
}

/* Entry point for the sink thread; this is the thread that processes
 * samples enqueued by the DAQ thread and writes them to the disk
 * in the data-format specified by our resource parameters.
*/
void SmemDAQ::sinkThreadEntry() {
    log_.Info(LOGFMT("Sink thread has started for SMEM resource {}", params_.absPath));
    configureThread(params_.sinkThreadAffinity);

    while (1) {
        bSignal_.wait(bLock_);

        log_.Info(LOGFMT("Sink thread has unblocked for SMEM resource {}", params_.absPath));
        if (!stopToken_)
            break;

        try {
            runSessionSink();
        } catch (std::exception const& e) {
            finishDAQSession();
            errorCallback_();
            continue;
        }
    }

    log_.Info(LOGFMT("Sink thread has finished for SMEM resource {}", params_.absPath));
}

/* Writes samples that are enqueued by the DAQ thread, to the disk in the desired
 * data-format for the duration of a DAQ session; note that a DAQ session lifetime
 * is defined either when the total number of samples have been successfully persisted
 * to the disk, or the DAQ server sends a signal to end our DAQ session.
*/
void SmemDAQ::runSessionSink() {
    void sink_sample(std::unique_ptr<std::byte[]> const&, IMAGE_METADATA const&);

    log_.Info(LOGFMT("Sink thread has started new session for SMEM resource {}", params_.absPath));
    size_t nSamplesSinked {};

    {
        std::lock_guard qguard(qLock_);
        while (!queue_.empty())
            queue_.pop();
    }

    while (runSession_) {
        if (params_.nSamples && params_.nSamples.value() == nSamplesSinked) {
            log_.Info(LOGFMT("DAQ session has reached sample-target for SMEM resource {} ({} samples sinked)", params_.absPath, nSamplesSinked));
            finishDAQSession();
            doneCallback_();
            continue;
        }

        std::optional<QueueType> qSample {};
        if (std::lock_guard qGuard(qLock_); !queue_.empty()) {
            qSample = std::move(queue_.front());
            queue_.pop();
        }

        if (qSample) {
            auto const& [metadata, sample] = qSample.value();
            sink_sample(sample, metadata);
            ++nSamplesSinked;
        }
    }
}




