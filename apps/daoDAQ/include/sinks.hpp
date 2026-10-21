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
        ISampleWriter(SmemParameters const& params, Dao::Log::Logger& log, std::string const& parentID) :
            params_(params),
            log_(log),
            parentID_(parentID),
            totalSamplesWritten_(0) {}

        virtual void write(QueueType const& sample) = 0;
        virtual ~ISampleWriter() = default;

        auto WriteCount() const { return totalSamplesWritten_; }

        protected:
        SmemParameters const& params_;
        Dao::Log::Logger& log_;
        std::string const& parentID_;
        std::size_t totalSamplesWritten_;
    };

    struct Mudpi1Writer final: public ISampleWriter {
        Mudpi1Writer(
            SmemParameters const& params, 
            std::string const& parentID,
            Dao::Log::Logger& log,
            IMAGE_METADATA const& smemInfo
        );
        Mudpi1Writer(Mudpi1Writer const&) = delete;
        Mudpi1Writer(Mudpi1Writer&&) = delete;
        Mudpi1Writer& operator=(Mudpi1Writer const&) = delete;
        Mudpi1Writer& operator=(Mudpi1Writer&&) = delete;
        ~Mudpi1Writer();

        void write(QueueType const& sample) override;

        private:
        /* MUDPI specification states packets shall not fragment when
         * sent across a 9000 byte MTU interface, this is the 
         * max packet size such that the IP packet is 9000 bytes and
         * does not fragment. 
        */
        static inline constexpr std::size_t MaxMudpiPacketSize_ { 8'972 };

        static inline constexpr std::size_t MudpiProtocolOverhead_ { 32 };
        static inline constexpr std::size_t MaxMudpiPayloadSize_ { MaxMudpiPacketSize_ - MudpiProtocolOverhead_};

        void CreateNetworkSocket_();
        void InitDatagramBuffer_();
        
        std::vector<std::byte> datagram_;
        std::size_t SmemSampleSize_;
        std::uint16_t FrameTotal_ {};
        int socketHandle_;
        std::uint16_t NumFrames_ {};

        /* Maps from Dao data-type to type-size.
        */
        const inline static
        std::unordered_map<std::uint8_t, std::uint8_t> DaoTypeSize_
        {
            {_DATATYPE_UINT8, SIZEOF_DATATYPE_UINT8},
            {_DATATYPE_INT8, SIZEOF_DATATYPE_INT8},
            {_DATATYPE_UINT16, SIZEOF_DATATYPE_UINT16},
            {_DATATYPE_INT16, SIZEOF_DATATYPE_INT16},
            {_DATATYPE_UINT32, SIZEOF_DATATYPE_UINT32},
            {_DATATYPE_INT32, SIZEOF_DATATYPE_INT32},
            {_DATATYPE_UINT64, SIZEOF_DATATYPE_UINT64},
            {_DATATYPE_INT64, SIZEOF_DATATYPE_INT64},
            {_DATATYPE_FLOAT, SIZEOF_DATATYPE_FLOAT},
            {_DATATYPE_DOUBLE, SIZEOF_DATATYPE_DOUBLE},
            {_DATATYPE_COMPLEX_FLOAT, SIZEOF_DATATYPE_COMPLEX_FLOAT},
            {_DATATYPE_COMPLEX_DOUBLE, SIZEOF_DATATYPE_COMPLEX_DOUBLE}
        };
    };

    struct FitsWriter final : public ISampleWriter {
        FitsWriter(
            SmemParameters const& params, 
            std::filesystem::path const& sessionOutputDir, 
            IMAGE_METADATA const& smInfo, 
            Dao::Log::Logger& log, 
            std::string const& parentID
        );
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
        std::filesystem::path sessionOutputDir_;

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

