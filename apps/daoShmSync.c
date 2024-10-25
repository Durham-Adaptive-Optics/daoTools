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

void *contextWrite;
void *socketWrite;  // ZMQ_PAIR for bi-directional communication
void *contextRead;
void *socketRead;  // ZMQ_PAIR for bi-directional communication
IMAGE *shm;

char shmName[32];
char serverAddr[32];
int port=5555;

// Thread
pthread_t writeThread;
pthread_t readThread;
int threadIdWrite = 0;
int threadIdRead = 0;

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

void * readRealTimeLoop(void *thread_data)
{
    daoInfo("ThreadId=%p\n", thread_data);
    daoInfo("Starting receiving on port %d\n", port);
    // MAIN LOOP
    daoInfo("ENTERING LOOP\n");
    fflush(stdout);

    int cnt=0;
    while (end ==0)
    {
        if (zmqReceiveImage(shm, socketRead) == DAO_SUCCESS)
        {
            // Finalize, release semaphore
            daoShmImagePart2ShmFinalize(&shm[0]);
            printf("\r \t\t\tRECEIVING %d\t", cnt);
        }
        else
        {
            printf("\r \t\t\tWAIT RECEIVING %d\t", cnt);
        }
        cnt++;
        fflush(stdout);
    }
    return DAO_SUCCESS;
}

void * writeRealTimeLoop(void *thread_data)
{
    daoInfo("ThreadId=%p\n", thread_data);
    daoInfo("Starting sending %s to %s\n",shmName, serverAddr);
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
            printf("\r SENDING %d\t", cnt);
            // Send the IMAGE structure
            zmqSendImage(shm, socketWrite);
        }
        else
        {
            printf("\r WAIT SENDING %d\t", cnt);
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

    // Set a 1-second receive timeout for send and receive
    int timeout = 1000; // in milliseconds
    // Initialize ZeroMQ context and socket
    contextWrite = zmq_ctx_new();
    socketWrite = zmq_socket(contextWrite, ZMQ_PAIR);  // ZMQ_PAIR for bi-directional communication
    zmq_connect(socketWrite, serverAddr);  // Connect to server
    zmq_setsockopt(socketWrite, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));

    contextRead = zmq_ctx_new();
    socketRead = zmq_socket(contextRead, ZMQ_PAIR);  // ZMQ_PAIR for bi-directional communication
    zmq_setsockopt(socketRead, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "tcp://*:%d", port);
    zmq_bind(socketRead, endpoint);  // Bind to port to receive the image from sender

    shm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(shmName, &shm[0]);


    int writeThreadVal=0;
    writeThreadVal = pthread_create(&writeThread, NULL, writeRealTimeLoop, (void *)&threadIdWrite);
    if (writeThreadVal != 0)
    {
        daoError("Cannot create write thread, err\n");
        return DAO_ERROR;
    }
    int readThreadVal=0;
    readThreadVal = pthread_create(&readThread, NULL, readRealTimeLoop, (void *)&threadIdRead);
    if (readThreadVal != 0)
    {
        daoError("Cannot create read thread, err\n");
        return DAO_ERROR;
    }
    pthread_join(writeThread, NULL);

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
                        (void)sscanf(*argv++,"%s", shmName);
                        (void)sscanf(*argv++,"%s", serverAddr);
                        (void)sscanf(*argv++,"%d", &port);
                        daoInfo("will be sending shmName=%s to serverAddr=%s and receiving on port %d...\n", shmName, serverAddr, port);
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
