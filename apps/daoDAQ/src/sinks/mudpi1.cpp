/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-10-19 12:08:00
 * @ Description: Implements MUDPI (V1) sample writer.
 */

#include <sinks.hpp>

Mudpi1Writer::Mudpi1Writer(
    SmemParameters const& params, 
    std::string const& parentID,
    Dao::Log::Logger& log
) :
    ISampleWriter(params, log, parentID)
{

}

Mudpi1Writer::~Mudpi1Writer()
{

}

void Mudpi1Writer::write(QueueType const& sample) 
{
    totalSamplesWritten++;
}

///////////

void Mudpi1Writer::CreateNetworkSocket_()
{
    // @TODO
    /* Here we need to create a networking socket with the linux kernel
     * specifically to receive UDP traffic destined to inteface=params_.netAddress
     * and port=params_.netPort.
     * 
     * The linux kernel should then create the resources required such as the recv buffer
     * and when packets are processed by its network stack that meet these requirements
     * (ie: UDP goingto interface and port) then it wil lcopy the UDP payload into
     * the recv buffer of our socket. We can then poll the socket to detect when
     * a datagram is present for us to process or we can read the socket in a blocking
     * manner, returning once a datagram is ready for us to process in the socket's recv buffer.
     * 
     * I need you to create the required socket with the kernel here and store the handle in the
     * object's private member variable of the correct type, which you should add to the class
     * definition.
     * 
     * Also be sure the implement the class destrcutor so that when the object is deallocated,
     * we inform the kernel we no longer need the socket so it can free the resources, this ensures
     * we do not leak system resources as we potentially repeadly create this object many times over
     * the program life time. 
    */
}