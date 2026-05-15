/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-05-05 21:56:55
 * @ Description: Sink Definitions.
 */

#pragma once

#include <configuration.hpp>
#include <unordered_map>
#include <filesystem>
#include <fitsio.h>
#include <utility>
#include <memory>
#include <dao.h>

namespace Dao::DAQ
{
    using QueueType = std::pair<IMAGE_METADATA, std::unique_ptr<std::byte[]>>;

    struct ISampleWriter {
        ISampleWriter(SmemParameters const& params, std::filesystem::path const& sessionOutputDir, Dao::Log::Logger& log, std::string const& parentID) :
            params_(params),
            sessionOutputDir_(sessionOutputDir),
            log_(log),
            parentID_(parentID) {
            //
        }

        virtual void write(QueueType const& sample) = 0;
        virtual ~ISampleWriter() = default;

        protected:
        SmemParameters const& params_;
        std::filesystem::path sessionOutputDir_;
        Dao::Log::Logger& log_;
        std::string const& parentID_;
    };

    struct FitsWriter : public ISampleWriter {
        FitsWriter(SmemParameters const& params, std::filesystem::path const& sessionOutputDir, IMAGE_METADATA const& smInfo, Dao::Log::Logger& log, std::string const& parentID);
        FitsWriter& operator= (FitsWriter const&) = delete;
        FitsWriter& operator= (FitsWriter&&) = delete;
        FitsWriter(FitsWriter const&) = delete;
        FitsWriter(FitsWriter&&) = delete;
        ~FitsWriter();

        void write(QueueType const& sample) override;

        private:
        size_t const nAxes_;
        std::vector<long> const axes_;
        size_t const imgType_;
        size_t const srcType_;
        size_t nSampleElements_;
        size_t nFileSamples;
        size_t nFiles_;
        fitsfile* file_;
        std::string fileName_;

        bool storeFull() const;
        void finish();
        void closeDatafile();
        void newDatafile();

        inline static const std::unordered_map<size_t, size_t> daoToImgType_
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

        inline static const std::unordered_map<size_t, size_t> daoToSrcType_
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

