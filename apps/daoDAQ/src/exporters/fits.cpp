/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-05-05 22:05:45
 * @ Description: Implements FITS sample exporter.
 */

#include <exporters.hpp>

namespace Dao::Telemetry
{
    FitsExporter::FitsExporter(SmemPolicy const& policies, IMAGE_METADATA const& smInfo) :
        ExportBackend(policies),
        store_(nullptr),
        nSamplesStored_(0),
        nStores_(0),
        diskDataType_(daoToBPIX_.at(smInfo.atype)),
        srcDatatype_(daoToFits_.at(smInfo.atype)),
        nElements_(smInfo.nelement),
        nDims_(smInfo.naxis),
        dims_(std::reverse_iterator(smInfo.size + nDims_), std::reverse_iterator(smInfo.size))
    {
    }

    void FitsExporter::reset(std::filesystem::path const& output)
    {
        outputDirectory_ = output;
        store_ = nullptr;
        nSamplesStored_ = 0;
        nStores_ = 0;
    }

    void FitsExporter::closeStore()
    {
        int err {};
        fits_close_file(store_, &err);
        store_ = nullptr;
    }

    void FitsExporter::newStore()
    {
        std::string const storeName = policies_.fileRollover ? storeBaseName_ : storeBaseName_ + "_" + std::to_string(nStores_++);
        std::filesystem::path const storePath { outputDirectory_ / storeName };

        int err {};
        fits_create_file(&store_, storePath.string().c_str(), &err);
    }

    bool FitsExporter::storeFull() const { return policies_.fileRollover && nSamplesStored_ == policies_.fileRollover.value(); }

    void FitsExporter::finish()
    {
        if (store_)
            closeStore();
    }

    void FitsExporter::put(QueueType const& sample)
    {
        if (store_ && storeFull())
            closeStore();

        if (!store_)
            newStore();

        int err {};
        auto& [info, buffer] = const_cast<QueueType&>(sample);
        fits_create_img(store_, diskDataType_, nDims_, dims_.data(), &err);
        fits_write_key(store_, TBYTE, "atype", &info.atype, NULL, &err);
        fits_write_key(store_, TLONGLONG, "atime", &info.atime.tsfixed.secondlong, "nanoseconds", &err);
        fits_write_key(store_, TULONGLONG, "cnt0", &info.cnt0, NULL, &err);
        fits_write_key(store_, TULONGLONG, "cnt1", &info.cnt1, NULL, &err);
        fits_write_key(store_, TULONGLONG, "cnt2", &info.cnt2, NULL, &err);
        fits_write_img(store_, srcDatatype_, 1, nElements_, buffer.get(), &err);
    }
};
