/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-10-19 12:08:00
 * @ Description: Sample writer for MUDPI version 1 protocol.
 */

#include <sinks.hpp>
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
    SampleSize_{ DaoTypeSize_.at(smemInfo.atype) * smemInfo.nelement },
    socketHandle_ { -1 }
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

/* Takes a shared memory sample and sends it to the network
 * endpoint using MUDPI packet(s).
*/
void Mudpi1Writer::write(const QueueType& sample) 
{
    const auto& [SampleMetadata, pSampleBuffer] = sample;
    
    /* Safe endian conversion for double type.
    */
    const auto htond = [](const double Value) -> std::uint64_t {
        std::uint64_t ValueView {};
        std::memcpy(&ValueView, &Value, sizeof(double));
        return htobe64(ValueView);
    };

    /* Define sample-specific MUDPI fields.
    */
    if(SampleMetadata.cnt0 > std::numeric_limits<std::uint32_t>::max()) {
        log_.Warning(LOGFMT("[{}] dangerous downcast of sample cnt0={} for MUDPI!",
            parentID_, SampleMetadata.cnt0
        ));
    }
    const std::uint32_t SampleID = ::htonl(static_cast<std::uint32_t>(SampleMetadata.cnt0));
    const std::uint64_t Timestamp = htond(static_cast<double>(SampleMetadata.atime.tsfixed.secondlong));
    const std::uint16_t FrameID = ::htons(1);
   
    /* Copy sample-specific MUDPI fields into datagram buffer.
    */
    std::byte* head = datagram_.data();
    std::memcpy(head + 12,  &SampleID,              sizeof(SampleID));
    std::memcpy(head + 16,  &Timestamp,             sizeof(Timestamp));
    std::memcpy(head + 24,  &FrameID,               sizeof(FrameID));
    std::memcpy(head + 26,  &FrameTotal_,           sizeof(FrameTotal_));
    std::memcpy(head + 30,  pSampleBuffer.get(),    SampleSize_);

    /* Send the datagram buffer to the endpoint via UDP.
    */
    const auto SendStatus = 
        ::sendto(
            socketHandle_, 
            datagram_.data(), 
            datagram_.size(), 
            0, 
            nullptr, 
            socklen_t {}
        );
    
    if(SendStatus < 0) {
        log_.Error(LOGFMT(
            "[{}] failed to send MUDPI frame ({}/{}) for sample {}: {}",
            parentID_, ntohs(FrameID), ntohs(FrameTotal_), ntohl(SampleID), ::strerror(errno)
        ));

        return;
    }

    totalSamplesWritten_++;
}

void Mudpi1Writer::CreateNetworkSocket_()
{
    /* Register UDP socket w/ kernel.
    */
    socketHandle_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle_ < 0) {
        throw std::runtime_error(LOGFMT(
            "failed to create socket for smem {} mudpi sink: {}", 
            parentID_, ::strerror(errno)
        ));
    }

    /* Configure socket w/ desired network endpoint (ip, port), saves
     * us from needing to provide the endpoint upon every sample write.
    */
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
    /* Allocate the datagram buffer.
    */
    constexpr std::size_t MudpiOverhead { 32 };
    const std::size_t DatagramSize = MudpiOverhead + SampleSize_;
    datagram_.resize(DatagramSize); // elements are zeroed here.

    /* Define protocol field values that don't change across samples,
     * encoding them as big-endian as per the MUDPI spec.
    */
    const std::uint16_t MudpiVersion = ::htons(1);
    const std::uint16_t ComponentID = ::htons('D' << 8 | 'U'); // DU = Durham University. 
    const std::int32_t TopicID = ::htonl(params_.topic);
    FrameTotal_ = ::htons(1);

    // @TODO handle shm samples larger that a single MUDPI frame can transmit.
    assert(SampleSize_ <= std::numeric_limits<std::uint16_t>::max());
    const std::uint16_t SampleSize = ::htons(static_cast<std::uint16_t>(SampleSize_));

    /* Copy static MUDPI fields into datagram buffer.
    */
    std::byte* head = datagram_.data();
    std::memcpy(head,       &TopicID,       sizeof(TopicID));
    std::memcpy(head + 4,   &ComponentID,   sizeof(ComponentID));
    std::memcpy(head + 10,  &MudpiVersion,  sizeof(MudpiVersion));
    std::memcpy(head + 26,  &FrameTotal_,   sizeof(FrameTotal_));
    std::memcpy(head + 28,  &SampleSize,    sizeof(SampleSize));
}

};