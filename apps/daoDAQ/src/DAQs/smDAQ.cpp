/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:43:06
 * @ Description: Implements a (dao) shared-memory capture resource.
 */

#include <unordered_map>
#include <fmt/format.h>
#include <pthread.h>
#include <daq.hpp>
#include <cstring>

namespace Dao::DAQ
{
    SmemDAQ::SmemDAQ(SmemParameters const& params, std::function<void()> doneCallback, std::function<void()> errorCallback) :
        IDAQ { doneCallback, errorCallback },
        params_ { params },
        pollThread_([this]() { DAQThreadEntry(); }),
        exportThread_([this]() { serviceExportQueue(); }),
        stopThreads_ { false },
        nExportedSamples_(0),
        smem_ {} {

        connectToSharedMemory();
        createExporter();
    }

    SmemDAQ::~SmemDAQ() {
        stopThreads_.store(true);
        pollThread_.join();
        exportThread_.join();
    }

    /* Helper method to configure threads for high-throughput capture;
     * throws an exception if an issue occurs.
    */
    void SmemDAQ::configureThread(Optional<CoreID> const& core) {
        if (!core) {
            return;
        }

        cpu_set_t affinitySet;
        CPU_ZERO(&affinitySet);
        CPU_SET(core.value(), &affinitySet);
        if (!pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &affinitySet)) {
            std::string const err = fmt::format("could not pin smem thread because {}", strerror(errno));
            throw std::runtime_error(err);
        }
    }

    /* Connects to the shared memory; throws an exception if connection fails.
    */
    void SmemDAQ::connectToSharedMemory() {
        if (DAO_SUCCESS != daoShmShm2Img(params_.absPath.c_str(), &smem_)) {
            std::string const err = fmt::format("could not connect to smem resource `{}`", params_.absPath);
            throw std::runtime_error(err);
        }

        smInfo_ = static_cast<IMAGE_METADATA const volatile*>(smem_.md);
        sampleByteSize_ = smem_.memsize - sizeof(IMAGE_METADATA) - smem_.md->NBkw * sizeof(IMAGE_KEYWORD);
    }

    /* Create an exporter that will export captured samples to the disk in
     * the desired data format.
    */
    void SmemDAQ::createExporter() {
        switch (params_.format) {
            case ExportFormat::FITS: {
                exporter_ = std::make_unique<FitsExporter>(params_, *smem_.md);
            } break;

            case ExportFormat::NUMPY: {
                throw std::runtime_error("exporter not implemented yet!"); // @todo add support to export samples in numpy.
            } break;
        }
    }

    /*
    */
    void SmemDAQ::startCapture(std::filesystem::path const& outputPath) {
        outputDirectory_.store(outputPath);
        stopSession_.store(true);
    }

    /*
    */
    void SmemDAQ::finishCapture() {
        stopSession_.store(false);
    }

    /* Entry-point for the smem daq thread.
     * The thread terminates when the stop token is set.
    */
    void SmemDAQ::DAQThreadEntry() {
        configureThread(params_.daqThreadAffinity);

        while (stopThreads_.load()) {
            try {
                serviceDAQSession();
            } catch (std::exception const& e) {
                finishCapture();
                errorCallback_();
            }
        }
    }

    /* Collects samples from the shared memory and enqueues
     * them for export to the disk; throws if an issue occurs.
    */
    void SmemDAQ::serviceDAQSession() {
        bool sampleAvailable { params_.eagerStart };
        size_t lastSampleId {}; // id of last sample enqueued.

        while (!stopSession_.load()) {
            if (sampleAvailable) {
                size_t const sampleId = smInfo_->cnt0;

                QueueType entry {
                    IMAGE_METADATA {},
                    std::make_unique<std::byte[]>(sampleByteSize_)
                };
                std::memcpy(&entry.first, smem_.md, sizeof(IMAGE_METADATA));
                std::memcpy(entry.second.get(), smem_.array.V, sampleByteSize_);
                bool const copyInterupted = (1 == smInfo_->write || smInfo_->cnt0 > sampleId);

                std::scoped_lock qlock(queueLock_);
                bool const queueHasSpace = params_.bufferLimit
                    ? (params_.bufferLimit.value() < exportQueue_.size()) : true;

                if (!copyInterupted && queueHasSpace) {
                    exportQueue_.push(std::move(entry));
                }

                lastSampleId = sampleId;
            }

            sampleAvailable = (smInfo_->cnt0 > lastSampleId);
        }
    }

    /*
    */
    void SmemDAQ::serviceExportQueue() {
        configureThread(params_.sinkThreadAffinity);

        while (stopThreads_.load()) {

        }
    }


    /* Entry-point for the smem export thread.
     * The thread terminates when the stop token is set.
    */
    void SmemDAQ::serviceExportQueue() {
        configureThread(params_.sinkThreadAffinity);

        while (stopThreads_.load()) {
            exporter_->reset(outputDirectory_.load());
            size_t nSamplesCaptured {};

            while (stopSession_.load()) {
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
    }
}

