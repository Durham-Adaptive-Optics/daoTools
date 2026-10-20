/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-10-19 12:08:00
 * @ Description: Implements MUDPI (V1) sample writer.
 */

#include <sinks.hpp>
#include <dao.h>
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace Dao::DAQ {

Mudpi1Writer::Mudpi1Writer(
    SmemParameters const& params, 
    std::string const& parentID,
    Dao::Log::Logger& log,
    IMAGE_METADATA const& smemInfo
) :
    ISampleWriter(params, log, parentID),
    socketHandle_(-1),
    smemInfo_(smemInfo),
    SampleSize_(DaoTypeSize_.at(smemInfo.atype) * smemInfo.nelement)
{
    CreateNetworkSocket_();
    InitDatagramBuffer_();
}

Mudpi1Writer::~Mudpi1Writer()
{
    if (socketHandle_ >= 0) {
        ::close(socketHandle_);
    }
}

std::uint64_t htonDouble(double const v)
{
    std::uint64_t v_;
    std::memcpy(&v_, &v, sizeof(double));
    return htobe64(v_);
}

void Mudpi1Writer::write(QueueType const& sample) 
{
    // @TODO: Maximum packet size shall not exceed MTU 9000 
    // @TODO: (support for 9000 is a requirement of the ELT Communication Infrastructure). 
    // @TODO: IP fragmentation should not occur.

    IMAGE_METADATA const& SampleMetadata = sample.first;
    auto SampleBuffer = sample.second.get();
    
    std::byte* Head = datagram_.data() + 12;
    auto const SampleID = ::htonl(static_cast<std::uint32_t>(SampleMetadata.cnt0));
    auto const Timestamp = htonDouble(static_cast<double>(SampleMetadata.atime.tsfixed.secondlong));
    auto const FrameID = htons(1);
    auto const FrameTotal = htons(1);

    std::memcpy(Head, &SampleID, 4);
    std::memcpy(Head + 4, &Timestamp, 8);
    std::memcpy(Head + 12, &FrameID, 2);
    std::memcpy(Head + 14, &FrameTotal, 2);
    std::memcpy(Head + 18, SampleBuffer, SampleSize_);

    if(::sendto(socketHandle_, datagram_.data(), datagram_.size(), 0, 0, 0) < 0) {
        log_.Error(LOGFMT(
            "[{}] failed to send MUDPI packet {}/{} for sample {}: {}",
            parentID_, FrameID, FrameTotal, SampleID, // @TODO these will be un-readable as BE.
            ::strerror(errno)
        ));
    } else {
        totalSamplesWritten_++;
    }
}

///////////

void Mudpi1Writer::CreateNetworkSocket_()
{
    socketHandle_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle_ < 0) {
        throw std::runtime_error(LOGFMT(
            "failed to create socket for smem {} mudpi sink: {}", 
            parentID_, ::strerror(errno)
        ));
    }

    sockaddr_in endpoint {};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = ::htons(params_.netPort);
    if(::inet_pton(AF_INET, params_.netAddress.value().c_str(), &endpoint.sin_addr) != 1) {
        throw std::runtime_error(LOGFMT(
            "failed to create socket for smem {} mudpi sink: invalid address '{}'",
            parentID_, params_.netAddress.value()
        ));
    }

    if(::connect(socketHandle_, reinterpret_cast<sockaddr*>(&endpoint), sizeof(sockaddr_in)) != 0) {
        throw std::runtime_error(LOGFMT(
            "failed to configure socket for smem {} mudpi sink: {}", 
            parentID_, ::strerror(errno)
        ));
    }
}

void Mudpi1Writer::InitDatagramBuffer_()
{
    std::uint16_t const ComponentTag_ { 0x5544 }; // "DU" (big-endian)
    std::uint16_t const MudpiVersion = ::htons(1);
    std::int32_t const TopicID = ::htonl(params_.topic);
    
    // @TODO: handle large packets properly..
    auto const SampleSize = htons(static_cast<std::uint16_t>(SampleSize_));

    datagram_.resize(SampleSize_ + 32);

    std::byte* head = datagram_.data();

    std::memcpy(head, &TopicID, 4);
    std::memcpy(head + 4, &ComponentTag_, 2);
    std::memcpy(head + 10, &MudpiVersion, 2);
    std::memcpy(head + 28, &SampleSize, 2);
}

};