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
    SmemSampleSize_{ DaoTypeSize_.at(smemInfo.atype) * smemInfo.nelement },
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

    /* Store the destination endpoint for each datagram sent by write().
    */
    endpoint_.sin_family = AF_INET;
    endpoint_.sin_port = ::htons(params_.netPort);
    if(::inet_pton(AF_INET, params_.netAddress.value().c_str(), &endpoint_.sin_addr) != 1) {
        throw std::runtime_error(LOGFMT(
            "failed to create socket for smem {} mudpi sink: invalid address '{}'",
            parentID_, params_.netAddress.value()
        ));
    }

    /* Configure socket to disallow IP fragmentation as per the MUDPI protocol;
     * ensures tests identify fragmentation occurring or gives hard error during
     * use.
    */
    constexpr auto IP_MTU_SETTING { IP_PMTUDISC_DO };
    if(::setsockopt(socketHandle_, IPPROTO_IP, IP_MTU_DISCOVER, &IP_MTU_SETTING, sizeof(IP_MTU_SETTING)) != 0) {
         throw std::runtime_error(LOGFMT(
            "failed to disallow socket ip-fragmentation for smem {} mudpi sink: {}", 
            parentID_, ::strerror(errno)
        ));
    }
}

void Mudpi1Writer::InitDatagramBuffer_()
{
    /* Define protocol field values that don't change across samples,
     * encoding them as big-endian as per the MUDPI spec.
    */
    const std::uint16_t MudpiVersion = ::htons(1);
    const std::uint16_t ComponentID = ::htons('D' << 8 | 'U'); // DU = Durham University. 
    const std::int32_t TopicID = ::htonl(params_.topic);
    
    NumFrames_ = std::ceil((float)SmemSampleSize_ / MaxMudpiPayloadSize_);
    const std::uint16_t NumFrames = ::htons(NumFrames_);

    /* Copy static MUDPI fields into datagram buffer.
    */
    std::byte* head = datagram_.data();
    std::memcpy(head,       &TopicID,       sizeof(TopicID));
    std::memcpy(head + 4,   &ComponentID,   sizeof(ComponentID));
    std::memcpy(head + 10,  &MudpiVersion,  sizeof(MudpiVersion));
    std::memcpy(head + 26,  &NumFrames,     sizeof(NumFrames));
}

/* Takes a shared memory sample and sends it to the network
 * endpoint using MUDPI packet(s).
*/
void Mudpi1Writer::write(const QueueType& sample) 
{
    const auto& [SampleMetadata, pSampleBuffer] = sample;
    
    /* Define sample-specific MUDPI fields.
    */
    if(SampleMetadata.cnt0 > std::numeric_limits<std::uint32_t>::max()) {
        log_.Warning(LOGFMT("[{}] dangerous downcast of sample cnt0={} for MUDPI sample-id field",
            parentID_, SampleMetadata.cnt0
        ));
    }
    const std::uint32_t SampleID = ::htonl(static_cast<std::uint32_t>(SampleMetadata.cnt0));
    const std::uint64_t Timestamp = [&SampleMetadata] {
        const auto ts = static_cast<double>(SampleMetadata.atime.tsfixed.secondlong);
        std::uint64_t ValueView {};
        std::memcpy(&ValueView, &ts, sizeof(ts));
        return htobe64(ValueView);
    }();
   
    /* Copy sample-specific MUDPI fields into datagram buffer.
    */
    std::byte* head = datagram_.data();
    std::memcpy(head + 12,  &SampleID,  sizeof(SampleID));
    std::memcpy(head + 16,  &Timestamp, sizeof(Timestamp));

    /* Build and send the MUDPI frames required to transmit the full sample. 
    */
    const std::byte* payload = pSampleBuffer.get();
    for (std::uint16_t frameID { 1 }; frameID <= NumFrames_; frameID++) {
        const std::size_t BytesSent = payload - pSampleBuffer.get();
        const std::uint16_t payloadSize = std::min(MaxMudpiPayloadSize_, SmemSampleSize_ - BytesSent);
        const std::size_t DatagramSize = MudpiProtocolOverhead_ + payloadSize;

        const std::uint16_t BEframeID = ::htons(frameID);
        const std::uint16_t BEpayloadSize = ::htons(payloadSize);
        std::memcpy(head + 24,  &BEframeID,        sizeof(BEframeID));
        std::memcpy(head + 28,  &BEpayloadSize,    sizeof(BEpayloadSize));
        std::memcpy(head + 30,  payload,           payloadSize);
        std::memset(head + 30 + payloadSize, 0, sizeof(std::uint16_t)); // disable MUDPI checksum.

        payload += payloadSize;

        const auto SendStatus = ::sendto(
            socketHandle_,
            datagram_.data(),
            DatagramSize,
            0,
            reinterpret_cast<sockaddr*>(&endpoint_),
            sizeof(endpoint_)
        );

        if(SendStatus < 0) {
            throw std::runtime_error(LOGFMT(
                "[{}] failed to send MUDPI frame ({}/{}) for sample {}: {}",
                parentID_, frameID, NumFrames_, SampleMetadata.cnt0, ::strerror(errno)
            ));
        }
    }

    totalSamplesWritten_++;
}

};