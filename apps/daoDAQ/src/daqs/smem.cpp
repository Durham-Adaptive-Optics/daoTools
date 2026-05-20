/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-05-08 22:03:13
 * @ Description: Implementation of shared-memory (smem) DAQ resource.
 */

#include <daqs.hpp>

using namespace Dao::DAQ;

SmemDAQ::SmemDAQ(SmemParameters const& params, ServerDoneCallback doneCallback, ServerErrorCallback errorCallback, Dao::Log::Logger& log) :
    IDAQ(doneCallback, errorCallback, params.absPath, log),
    params_(params),
    stopToken_(false),
    runSession_(false),
    smem_ {},
    cvPredicate_(false) {
    //
    log_.Trace("(daq.smem %s) creating..", resourceID.c_str());

    establishResourceConnection();
    daqThread_ = std::thread([this]() { this->daqThreadEntry(); });
    sinkThread_ = std::thread([this]() { this->sinkThreadEntry(); });

    log_.Debug("(daq.smem %s) created", resourceID.c_str());
}

/* Signals DAQ and sink threads to exit gracefully and blocks until they
 * have exited.
*/
SmemDAQ::~SmemDAQ() {
    log_.Trace("(daq.smem %s) destroying..", resourceID.c_str());

    {
        std::unique_lock lock(cvLock_);
        stopToken_ = true;
        cvPredicate_ = true;
        cvSignal_.notify_all();
    }

    daqThread_.join();
    log_.Debug("(daq.smem %s) daq-thread has joined", resourceID.c_str());

    sinkThread_.join();
    log_.Debug("(daq.smem %s) sink-thread has joined", resourceID.c_str());

    log_.Debug("(daq.smem %s) destroyed", resourceID.c_str());
}

/* Connect to the shared memory resource; an exception is thrown
 * in the case connection fails.
*/
void SmemDAQ::establishResourceConnection() {
    if (DAO_SUCCESS != daoShmShm2Img(params_.absPath.c_str(), &smem_)) {
        std::string const err = fmt::format("(daq.smem %s) failed to open smem {}", resourceID, params_.absPath);
        throw std::runtime_error(err);
    }

    /* note(tom):
        Currently we avoid supporting complex number types.
        It is relatively straightforward to add such support
        in the fits backend; it involes using binary tables
        instead of image HDUs. However I am not aware of a
        business case for such supprt yet, so we leave this
        for now.
    */
    if (_DATATYPE_COMPLEX_FLOAT == smem_.md->atype || _DATATYPE_COMPLEX_DOUBLE == smem_.md->atype) {
        std::string const err = fmt::format("(daq.smem %s) complex-valued arrays are not currently supported", resourceID);
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
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &affinitySet)) {
        log_.Error("(daq.smem %s) failed to pin thread ({})", strerror(errno));
    }
}

/* Signals both the DAQ and sink threads of this resource to begin a new DAQ
 * session; the session will either auto-finish (if applicable) or is ended
 * by calling the session finish method.
*/
void SmemDAQ::beginAcquisition(std::filesystem::path const& outputPath) {
    log_.Trace("(daq.smem % s) starting acquisition..", resourceID.c_str());

    {
        std::lock_guard qGuard(qLock_);
        while (!queue_.empty())
            queue_.pop();

        log_.Debug("(daq.smem %s) cleared queue", resourceID.c_str());
    }

    {
        std::unique_lock lock(cvLock_);
        sessionOutputDir_ = outputPath;
        runSession_.store(true);
        cvPredicate_ = true;
        cvSignal_.notify_all();
    }

    log_.Debug("(daq.smem %s) started acquisition", resourceID.c_str());
}

/* Signals both the DAQ and sink threads to end their in-progress DAQ session
 * and return to their blocked state where they await an unblock signal
 * to either begin a new DAQ session or to exit.
*/
void SmemDAQ::finishAcquisition() {
    log_.Trace("(daq.smem %s) finishing acquisition", resourceID.c_str());

    {
        std::unique_lock lock(cvLock_);
        cvPredicate_ = false;
        runSession_.store(false);
    }

    log_.Debug("(daq.smem %s) finished acquisition", resourceID.c_str());
}

/* Entry point for the DAQ thread; this is the thread that captures the
 * data samples from the shared memory resource and enqueues them for
 * sinking to the disk.
*/
void SmemDAQ::daqThreadEntry() {
    log_.Debug("(daq.smem %s) daq-thread launched", resourceID.c_str());

    configureThread(params_.daqThreadAffinity);

    while (1) {
        {
            std::unique_lock cvGuard(cvLock_);
            cvSignal_.wait(cvGuard, [&]() {
                log_.Debug(
                    "(daq.smem %s) daq-thread .. %s",
                    resourceID.c_str(),
                    cvPredicate_ ? "resuming" : "blocked"
                );
                return cvPredicate_;
            });
        }

        if (stopToken_)
            break;

        try {
            acquireSamples();
        } catch (std::exception const& err) {
            finishAcquisition();
            errorCallback_(resourceID, err.what());
            continue;
        }
    }

    log_.Debug("(daq.smem %s) daq-thread exited", resourceID.c_str());
}

/* Entry point for the sink thread; this is the thread that processes
 * samples enqueued by the DAQ thread and writes them to the disk
 * in the data-format specified by our resource parameters.
*/
void SmemDAQ::sinkThreadEntry() {
    log_.Debug("(daq.smem %s) sink-thread launched", resourceID.c_str());

    configureThread(params_.sinkThreadAffinity);

    while (1) {
        {
            std::unique_lock cvGuard(cvLock_);
            cvSignal_.wait(cvGuard, [&]() {
                log_.Debug(
                    "(daq.smem %s) sink-thread .. %s",
                    resourceID.c_str(),
                    cvPredicate_ ? "resuming" : "blocked"
                );
                return cvPredicate_;
            });
        }

        if (stopToken_)
            break;

        try {
            sinkSamples();
        } catch (std::exception const& err) {
            finishAcquisition();
            errorCallback_(resourceID, err.what());
            continue;
        }
    }

    log_.Debug("(daq.smem %s) sink-thread exited", resourceID.c_str());
}

/* Captures data-samples from the shared memory resource an enqueues them for
 * saving to the disk by the sink thread; samples are captured for the duration
 * of a DAQ session which ends when either the sink thread has met the desired
 * sample target (if one exists) or if the DAQ server has signalled the DAQ session
 * to finish.
*/
void SmemDAQ::acquireSamples() {
    IMAGE_METADATA const volatile* smInfo_ = static_cast<IMAGE_METADATA const volatile*>(smem_.md);
    bool sampleAvailable { params_.eagerStart.value() };
    size_t lastSampleId = smInfo_->cnt0;

    log_.Debug(
        "(daq.smem %s) collecting samples.. [eager=%s]",
        resourceID.c_str(),
        params_.eagerStart.value() ? "yes" : "no"
    );

    /* note(tom): we avoid logging in the hot-path here as we do not
     * want to pay the penalty of enqueing and formatting logs etc.
    */
    while (runSession_.load()) {
        /* Process current sample in smem upon the signal.
        */
        if (sampleAvailable) {
            size_t const sampleId = smInfo_->cnt0; // pickup id of current sample in smem.

            // copy sample information from smem into internal buffers.
            QueueType qSample {
                IMAGE_METADATA {},
                std::make_unique<std::byte[]>(sampleMemSize_)
            };
            qSample.first = *smem_.md;
            std::memcpy(qSample.second.get(), smem_.array.V, sampleMemSize_);

            // drop the sample if the copy was potentially interrupted.
            bool const copyInterupted = (smInfo_->cnt0 > sampleId || 1 == smInfo_->write);
            if (!copyInterupted) {
                std::lock_guard qGuard(qLock_);

                // enqueue the copied sample data for sinking to the disk by the sink-thread
                // - note the sample is dropped if the queue is considered full.
                bool const queueHasSpace = params_.bufferLimit ? (queue_.size() < params_.bufferLimit.value()) : true;
                if (queueHasSpace) {
                    queue_.push(std::move(qSample));
                }
            }

            lastSampleId = sampleId;
        }

        /* Detect fresh smem sample.
        */
        sampleAvailable = smInfo_->cnt0 > lastSampleId;
    }

    log_.Debug("(daq.smem %s) stopped collecting samples", resourceID.c_str());
}

/* Writes samples that are enqueued by the DAQ thread, to the disk in the desired
 * data-format for the duration of a DAQ session; note that a DAQ session lifetime
 * is defined either when the total number of samples have been successfully persisted
 * to the disk, or the DAQ server sends a signal to end our DAQ session.
*/
void SmemDAQ::sinkSamples() {
    log_.Debug("(daq.smem %s) sinking samples..", resourceID.c_str());

    // @todo sessionOutputDir_ may have a data-race here (no mutex)?
    auto writer = std::make_unique<FitsWriter>(params_, sessionOutputDir_, *smem_.md, log_, resourceID);
    size_t nSamplesWritten {};

    /* note(tom): we avoid logging in the hot-path here as we do not
     * want to pay the penalty of enqueing and formatting logs etc.
    */
    while (runSession_.load()) {
        /* Finish acquiring and report that we're done to the server.
        */
        if (params_.nSamples && params_.nSamples.value() == nSamplesWritten) {
            finishAcquisition();
            doneCallback_(resourceID);
            continue;
        }

        /* Dequeue next sample from FIFO.
        */
        std::optional<QueueType> qSample {};
        if (std::lock_guard qGuard(qLock_); !queue_.empty()) {
            qSample = std::move(queue_.front());
            queue_.pop();
        }

        /* Sink sample from FIFO to datafile on disk in the desired
         * export format.
        */
        if (qSample) {
            auto const& sample = qSample.value();
            writer->write(sample);
            ++nSamplesWritten;
        }
    }

    log_.Debug("(daq.smem %s) stopped sinking samples", resourceID.c_str());

    if (0 == nSamplesWritten)
        log_.Warning("(daq.smem %s) received no samples to export to disk -- may indicate daq-thread issue", resourceID.c_str());
}


