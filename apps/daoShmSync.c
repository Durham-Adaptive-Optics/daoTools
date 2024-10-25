/*****************************************************************************
  DAO project
  s.cetre
 *****************************************************************************/

/*==========================================================================*/
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <semaphore.h>
#include <sched.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <sys/file.h>
#include <errno.h>
#include <sys/mman.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/time.h>
#include <pthread.h>
#include <zmq.h>

#include "dao.h"
#include "daoTools.h"

/*==========================================================================*/
static int	sNdx=0;							/* board index */
static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;

void *contextSend;
void *socketSend;  // ZMQ_PAIR for bi-directional communication amd ZMQ RADIO for UDP
void *contextRecv;
void *socketRecv;  // ZMQ_PAIR for bi-directional communication and ZMQ_DISH for UDP
IMAGE *shm;

char protocol[32];
char shmName[32];
char serverAddr[32];
int portSend=5555;
int portRecv=5556;

long unsigned int lastReceivedCnt=0; // Used to sync recv and send and avoid infinite loop to not resend what you just received
// Thread
pthread_t sendThread;
pthread_t recvThread;
int threadIdSend = 0;
int threadIdRecv = 0;

static int   		end     = 0;		           // termination flag
// termination function for SIGINT callback
static void endme()
{
    end = 1;
}

/*--------------------------------------------------------------------------*/
static char	*sArgv0=NULL;					/* name of executable */

static void ShowHelp(void)
{
    printf("%s of " __DATE__ " at " __TIME__ "\n",sArgv0);
    printf("   arguments:\n");
    printf("   -h               display this message and exit\n");
    printf("   -d               display program debug output\n");
    /*
     **	Post init tests
     */
    printf("   -L <shm> <servAddr>             real time control loop, servAddr form is tcp://localhost:5555 5555\n");
    printf("\n");
}

void * recvRealTimeLoop(void *thread_data)
{
    daoInfo("ThreadId=%p\n", thread_data);
    daoInfo("Starting receiving on port %d\n", portRecv);
    // MAIN LOOP
    daoInfo("ENTERING LOOP\n");
    fflush(stdout);

    int cnt=0;
    int res = DAO_SUCCESS;
    while (end ==0)
    {
        if (strncmp(protocol, "tcp", 3) == 0)
        {
            res = zmqReceiveImageTCP(shm, socketRecv);
        }
        else if (strncmp(protocol, "udp", 3) == 0)
        {
            res = zmqReceiveImageUDP(shm, socketRecv, shmName);
        }
        else
        {
            daoError("Invalid protocol %s\n", protocol);
        }
        
        if (res == DAO_SUCCESS)
        {
            // Finalize, release semaphore
            daoShmImagePart2ShmFinalize(&shm[0]);
            lastReceivedCnt = shm[0].md[0].cnt0;
            printf("\r \t\t\t     RECEIVING %d\t", cnt);
        }
        else
        {
            printf("\r \t\t\twait receiving %d\t", cnt);
        }
        cnt++;
        fflush(stdout);
    }
    return DAO_SUCCESS;
}

void * sendRealTimeLoop(void *thread_data)
{
    daoInfo("ThreadId=%p\n", thread_data);
    daoInfo("Starting sending %s to %s:%d\n",shmName, serverAddr, portSend);
    // MAIN LOOP
    daoInfo("ENTERING LOOP\n");
    fflush(stdout);

    struct timespec timeout;
    int cnt=0;
    while (end ==0)
    {
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        // Wait for new image
        if (sem_timedwait(shm[0].semptr[9], &timeout) != -1)
        {
            // Send only if it is a new image not from another receveive to avoid loop
            if (shm[0].md[0].cnt0 != lastReceivedCnt)
            {
                printf("\r      SENDING %d\t", cnt);
                if (strncmp(protocol, "tcp", 3) == 0)
                {
                    // Send the IMAGE structure
                    zmqSendImageTCP(shm, socketSend);
                }
                else if (strncmp(protocol, "udp", 3) == 0)
                {
                    // Send the IMAGE structure
                    zmqSendImageUDP(shm, socketSend, shmName, 1400);
                }
                else
                {
                    daoError("Invalid protocol %s\n", protocol);
                }
            }
        }
        else
        {
            printf("\r wait sending %d\t", cnt);
            fflush(stdout);
        }
        cnt++;
        fflush(stdout);
    }
    return DAO_SUCCESS;
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);
    daoInfo("Building ZeroMQ context and socket\n");


    if (strncmp(protocol, "tcp", 3) == 0)
    {
        // Set a 1-second receive timeout for send and receive
        int timeout = 1000; // in milliseconds
        char sendEndPoint[256];
        char recvEndPoint[256];

        daoInfo("Setting up ZMQ for protocol %s\n", protocol);

        // Initialize ZeroMQ context and socket
        contextSend = zmq_ctx_new();
        socketSend = zmq_socket(contextSend, ZMQ_PAIR);  // ZMQ_PAIR for bi-directional communication (TCP)
        zmq_setsockopt(socketSend, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));
        snprintf(sendEndPoint, sizeof(sendEndPoint), "tcp://%s:%d", serverAddr, portSend);
        daoInfo("Sending to %s\n", sendEndPoint);
        zmq_connect(socketSend, sendEndPoint);  // Connect to server

        contextRecv = zmq_ctx_new();
        socketRecv = zmq_socket(contextRecv, ZMQ_PAIR);  // ZMQ_PAIR for bi-directional communication (TCP_)
        zmq_setsockopt(socketRecv, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    
        snprintf(recvEndPoint, sizeof(recvEndPoint), "tcp://*:%d", portRecv);
        daoInfo("Receiving to %s\n", recvEndPoint);
        zmq_bind(socketRecv, recvEndPoint);  // Bind to portRecv to receive the image from sender
    }
    else if (strncmp(protocol, "udp", 3) == 0)
    {
        // Set up ZMQ for UDP with 1-second timeout for send and receive
        int timeout = 1000; // in milliseconds
        char sendEndPoint[256];
        char recvEndPoint[256];
        //const char *serverAddr = "239.192.1.1"; // Choose a suitable multicast address for UDP

        daoInfo("Setting up ZMQ for protocol %s\n", protocol);

        // Initialize ZeroMQ context and socket for sending
        contextSend = zmq_ctx_new();
        socketSend = zmq_socket(contextSend, ZMQ_RADIO);  // ZMQ_RADIO for UDP sending
        zmq_setsockopt(socketSend, ZMQ_SNDTIMEO, &timeout, sizeof(timeout)); // Set send timeout

        snprintf(sendEndPoint, sizeof(sendEndPoint), "udp://%s:%d", serverAddr, portSend);
        daoInfo("Sending to %s\n", sendEndPoint);
        zmq_connect(socketSend, sendEndPoint);  // Connect to multicast address

        // Initialize ZeroMQ context and socket for receiving
        contextRecv = zmq_ctx_new();
        socketRecv = zmq_socket(contextRecv, ZMQ_DISH);  // ZMQ_DISH for UDP receiving
        zmq_setsockopt(socketRecv, ZMQ_RCVTIMEO, &timeout, sizeof(timeout)); // Set receive timeout

        snprintf(recvEndPoint, sizeof(recvEndPoint), "udp://%s:%d", serverAddr, portRecv);
        daoInfo("Receiving from %s\n", recvEndPoint);
        zmq_bind(socketRecv, recvEndPoint);  // Bind to multicast address and port

        // Join a group to filter messages (for example, "image")
        zmq_join(socketRecv, shmName);  // Join the group "image" for receiving
    }
    else
    {
        daoError("Invalid protocol %s\n", protocol);
        return DAO_ERROR;
    }
    shm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(shmName, &shm[0]);


    int sendThreadVal=0;
    sendThreadVal = pthread_create(&sendThread, NULL, sendRealTimeLoop, (void *)&threadIdSend);
    if (sendThreadVal != 0)
    {
        daoError("Cannot create send thread, err\n");
        return DAO_ERROR;
    }
    int recvThreadVal=0;
    recvThreadVal = pthread_create(&recvThread, NULL, recvRealTimeLoop, (void *)&threadIdRecv);
    if (recvThreadVal != 0)
    {
        daoError("Cannot create recv thread, err\n");
        return DAO_ERROR;
    }
    pthread_join(sendThread, NULL);

    daoInfo("EXITING MAIN LOOP\n");
    fflush(stdout);

    return DAO_SUCCESS;
}

/**
 *	Parse the input arguments.
 */
static void DecodeArgs(int argc, char **argv)
{
    char	*str;
    int a1;

    argv += 1;	argc -= 1;					/* skip program name */

    while (argc-- > 0) {
        daoDebug("DecodeArgs: working on '%s'/%d\n",*argv,argc);
        str = *argv++;
        if (str[0] != '-') {
            daoError("Do not know arg '%s'\n",str);
            ShowHelp();
            exit(1);
        }

        switch (str[1]) {
            case 'h':	ShowHelp(); exit(0);
            case 'd':	
                        (void)sscanf(*argv++,"%d",&daoLogLevel); argc -= 1;
                        break;
            case 'l':
                        daoInfo("%s\n",*argv);
                        argv += 1; argc -= 1;
                        break;

            case 'b':	(void)sscanf(*argv++,"%d",&sNdx); argc -= 1;	break;
            case 'u':
                        (void)sscanf(*argv++,"%d",&a1); argc -= 1;
                        daoDebug("will sleep for %d usec\n",a1);
                        (void)usleep(a1);
                        break;
            case 's':	(void)sscanf(*argv++,"%s",shmName); argc -= 1;
                        break;
            case 'L':
                        daoInfo("continuously send SHM to remote machine real time control\n");
                        (void)sscanf(*argv++,"%s", protocol);
                        (void)sscanf(*argv++,"%s", shmName);
                        (void)sscanf(*argv++,"%s", serverAddr);
                        (void)sscanf(*argv++,"%d", &portSend);
                        (void)sscanf(*argv++,"%d", &portRecv);
                        daoInfo("Using protocol=%s\n", protocol);
                        daoInfo("Sending shmName=%s to serverAddr=%s:%d\n", shmName, serverAddr, portSend);
                        daoInfo("Receiving on port %d...\n", portRecv);
                        realTimeLoop();
                        break;
            default:
                        daoError("Do not know arg '%s'\n",str);
                        ShowHelp();
                        exit(2);
        }
    }

    return;
}

/*==========================================================================*/
int main(int argc, char **argv)
    /*
     **	Fetch the arguments and do what is requested
     */
{
    int RT_priority = 93; //any number from 0-99
    struct sched_param schedpar;

    schedpar.sched_priority = RT_priority;
    // r = seteuid(euid_called); //This goes up to maximum privileges
    sched_setscheduler(0, SCHED_FIFO, &schedpar); //other option is SCHED_RR, might be faster
    // r = seteuid(euid_real);//Go back to normal privileges

    sArgv0 = *argv;

    DecodeArgs(argc,argv);

    return(sExit);
}
/*==========================================================================*/
