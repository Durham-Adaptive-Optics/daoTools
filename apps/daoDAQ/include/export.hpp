/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-05-05 21:56:55
 * @ Description: Export Backend Header
 */

#pragma once

#include <unordered_map>
#include <filesystem>
#include <daoShm.h>
#include <fitsio.h>
#include <utility>
#include <memory>

namespace Dao::DAQ
{
    enum class ExportFormat {
        FITS,
        NUMPY
    };

    inline static std::unordered_map<ExportFormat, std::string> const fmtToRepr
    {
        { ExportFormat::FITS, "fits" },
        { ExportFormat::NUMPY, "numpy" }
    };

    inline static std::unordered_map<std::string, ExportFormat> const ReprToFmt
    {
        { "fits", ExportFormat::FITS },
        { "numpy", ExportFormat::NUMPY }
    };

    using QueueType = std::pair<IMAGE_METADATA, std::unique_ptr<std::byte[]>>;

    struct ExportBackend {
        ExportBackend(SmemParameters const& policies)
            : policies_(policies), outputDirectory_() {
        }

        virtual void reset(std::filesystem::path const& output) = 0;
        virtual void put(QueueType const& sample) = 0;
        virtual void finish() = 0;
        virtual ~ExportBackend() = default;

        protected:
        SmemParameters const& policies_;
        std::filesystem::path outputDirectory_;
    };

    struct FitsExporter : public ExportBackend {
        FitsExporter(SmemParameters const& policies, IMAGE_METADATA const& smInfo);

        void reset(std::filesystem::path const& output) override;
        void put(QueueType const& sample) override;
        void finish() override;

        private:
        fitsfile* store_;
        size_t nSamplesStored_;
        std::string storeBaseName_;
        size_t nStores_;
        size_t diskDataType_;
        size_t srcDatatype_;
        size_t nDims_;
        size_t nElements_;
        std::vector<long> dims_;

        bool storeFull() const;
        void finish();
        void closeStore();
        void newStore();

        // table to convert dao datatypes to fits image pixel datatypes (BPIX parameter).
        inline static const std::unordered_map<size_t, size_t> daoToBPIX_
        {
           {_DATATYPE_UINT8, BYTE_IMG},
           {_DATATYPE_INT8, SBYTE_IMG},
           {_DATATYPE_UINT16, USHORT_IMG},
           {_DATATYPE_INT16, SHORT_IMG},
           {_DATATYPE_UINT32, ULONG_IMG},
           {_DATATYPE_INT32, LONG_IMG},
           {_DATATYPE_UINT64, ULONGLONG_IMG},
           {_DATATYPE_INT64, LONGLONG_IMG},
           {_DATATYPE_FLOAT, FLOAT_IMG},
           {_DATATYPE_DOUBLE, DOUBLE_IMG}
        };

        // table to convert dao datatypes to fits datatypes.
        inline static const std::unordered_map<size_t, size_t> daoToFits_
        {
            {_DATATYPE_UINT8, TBYTE},
            {_DATATYPE_INT8, TSBYTE},
            {_DATATYPE_UINT16, TUSHORT},
            {_DATATYPE_INT16, TSHORT},
            {_DATATYPE_UINT32, TUINT},
            {_DATATYPE_INT32, TINT},
            {_DATATYPE_UINT64, TULONGLONG},
            {_DATATYPE_INT64, TLONGLONG},
            {_DATATYPE_FLOAT, TFLOAT},
            {_DATATYPE_DOUBLE, TDOUBLE}
        };
    };
};

