/*****************************************************************************
  DAO project
  daoDMSend: send each new DM command of an SHM over UDP
 *****************************************************************************/
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>

#include "daoTools.h"

static volatile int end = 0;               // termination flag

static void endme(int _a)
{
    (void)_a;
    end = 1;
}

static void ShowHelp(const char *argv0)
{
    printf("%s of " __DATE__ " at " __TIME__ "\n", argv0);
    printf("   Sends each new frame of <SHM> (float), as one UDP datagram, to <IP>:<PORT>.\n");
    printf("   usage:\n");
    printf("   %s <SHM> <IP> <PORT>\n", argv0);
    printf("   %s -h            display this message and exit\n", argv0);
    printf("\n");
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "-h") == 0)
    {
        ShowHelp(argv[0]);
        return 0;
    }
    if (argc != 4)
    {
        ShowHelp(argv[0]);
        return 1;
    }
    const char *shmName = argv[1];
    const char *ip = argv[2];
    char *endPort;
    long port = strtol(argv[3], &endPort, 10);
    if (*argv[3] == '\0' || *endPort != '\0' || port < 1 || port > 65535)
    {
        daoError("<PORT>: '%s' is not a port (1 to 65535)\n", argv[3]);
        return 2;
    }

    IMAGE *dmShm = (IMAGE *)malloc(sizeof(IMAGE));
    daoToolsShmOpen(shmName, &dmShm[0]);
    long nActs = daoToolsShmValues(dmShm);
    daoToolsShmCheck(dmShm, shmName, _DATATYPE_FLOAT, nActs);
    size_t dataSize = sizeof(float) * (size_t)nActs;
    if (dataSize > 65507)
    {
        daoError("%s: %zu bytes, more than a UDP datagram holds (65507)\n", shmName, dataSize);
        return 1;
    }
    daoInfo("Sending %s (%ld values) to %s:%ld\n", shmName, nActs, ip, port);

    int udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSocket < 0)
    {
        daoError("cannot create the socket: %s\n", strerror(errno));
        return 1;
    }
    struct sockaddr_in destAddr;
    memset(&destAddr, 0, sizeof(destAddr));
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &destAddr.sin_addr) <= 0)
    {
        daoError("<IP>: '%s' is not an IPv4 address\n", ip);
        close(udpSocket);
        return 2;
    }

    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    long failed = 0;
    while (end == 0)
    {
        if (daoToolsWait(dmShm, DAO_SEM_AUTO, 1.0) != DAO_SUCCESS)
        {
            daoToolsLoopStatusWait(&status);
            continue;
        }
        daoToolsLoopStatusStart(&status);
        if (sendto(udpSocket, dmShm[0].array.F, dataSize, 0, (struct sockaddr *)&destAddr, sizeof(destAddr)) < 0)
        {
            failed++;
        }
        daoToolsLoopStatusEnd(&status, failed ? ", %ld sends failed" : "", failed);
    }
    printf("\n");
    close(udpSocket);
    daoToolsShmRelease(&dmShm);
    return 0;
}
