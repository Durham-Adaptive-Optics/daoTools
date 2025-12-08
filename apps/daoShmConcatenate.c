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
#include <termios.h>
#include <pthread.h>
#include <ncurses.h>

// DAO header
#include "dao.h" 
#include "daoTools.h"

typedef int bool_t;
#ifndef TRUE
#define TRUE	1
#endif
#ifndef FALSE
#define FALSE	0
#endif

/*==========================================================================*/
static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;


struct timespec tnow;
double tlastupdatedouble;


#define CONCATENATE_MAX 16

IMAGE *shmOut;
char shmOutName[32];
int nbShm;
int masterChannel=-1;
// Max 16 different SHM to combine
IMAGE *shmIn[CONCATENATE_MAX];
char shmName[CONCATENATE_MAX][32];
int updateCnt[CONCATENATE_MAX];
int shmPos[CONCATENATE_MAX];

float frequency;

// Thread
pthread_t controllerThread[CONCATENATE_MAX];
int threadIdCtrl = 0;
pthread_t finalCtrlThread;
int threadIdFinalCtrl = 0;

struct arg_struct {
    int shmId;
};

// termination flag
static int end     = 0;

// termination function for SIGINT callback
static void endme() 
{
    end = 1;
}

static char	*sArgv0=NULL;					/* name of executable */

static void ShowHelp(void)
{
    daoInfo("%s of " __DATE__ " at " __TIME__ "\n",sArgv0);
    daoInfo("   arguments:\n");
    daoInfo("   -h               display this message and exit\n");
    daoInfo("   -d               display program debug output\n");
    daoInfo("   -n               number of SHM to concanenate\n");
    daoInfo("   -O               name of output SHM\n");
    daoInfo("   -S               list of SHM (full path separated by space)\n");
    daoInfo("   -s               semaphore number\n");
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("      -n <nbShm> -O <SHM> -S <SHM1>..<SHMnbShm> -L\n");
    daoInfo("\n");
}
/*--------------------------------------------------------------------------*/
void * shmNRealTimeLoop(void *thread_data)
{
    struct arg_struct *args = (struct arg_struct *)thread_data;
    daoInfo("ThreadId=%d ENTERING LOOP\n", args->shmId);

    fflush(stdout);
    struct timespec t[2];
    clock_gettime(CLOCK_REALTIME, &t[1]);
    int nbVal = shmIn[args->shmId][0].md[0].size[0] * shmIn[args->shmId][0].md[0].size[1];
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &t[0]);
    while (end==0) 
    {
        // Wait for SHM semaphore
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec +=1;
        if (daoShmWaitForSemaphoreTimeout(shmIn[args->shmId], 0, &timeout) != -1)
        {
            daoShmCopyToPosition(shmIn[args->shmId], shmOut, nbVal, shmPos[args->shmId], 0);
            clock_gettime(CLOCK_REALTIME, &t[0]);
            updateCnt[args->shmId] ++;
        }
    }

    daoInfo("EXITING MAIN LOOP\n");
    fflush(stdout);

    return DAO_SUCCESS;
}
    
void * finalizeRealTimeLoop(void *thread_data)
{
    daoInfo("ThreadId=%p\n", thread_data);
    fflush(stdout);
    struct timespec t[2];
    clock_gettime(CLOCK_REALTIME, &t[1]);
    int sum=0;
    int k, j;
    int cnt=0;
    clock_gettime(CLOCK_REALTIME, &t[0]);
    while (end==0) 
    {
        for (k=0; k<nbShm; k++)
        {
            if (updateCnt[k] == 1)
            {
                sum+=1;
            }
            else if (updateCnt[k] > 1)
            {
                daoWarning("overcount for shm%d (%d), resetting all\n", k+1, updateCnt[k]);
                for(j=0; j<nbShm; j++)
                {
                    updateCnt[j]=0;
                }
                sum = 0;
            }
        }
        if (sum == nbShm)
        {
            for (k=0; k<nbShm; k++)
            {
                updateCnt[k] = 0;
            } 
            daoShmImagePart2ShmFinalize(&shmOut[0]);
            cnt++;
            printf("\r concatenate finalized: %d", cnt);
            fflush(stdout);
            sum=0;
        }
    }
    daoInfo("EXITING Finalizer LOOP\n");
    fflush(stdout);
    return DAO_SUCCESS;
}

/*--------------------------------------------------------------------------*/
static int prepRealTime()
{
    int status;
    int k;
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    shmOut = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(shmOutName, &shmOut[0]);
    daoInfo("%s shm created\n", shmOutName);
    
    // create shm (/tmp/<shmName><nameId>.im.shm)
    for (k=0; k<nbShm; k++)
    {
        shmIn[k] = (IMAGE *)malloc(sizeof(IMAGE));
        daoShmShm2Img(shmName[k], &shmIn[k][0]);
        daoInfo("%s shm created\n", shmName[k]);
        updateCnt[k] = 0;
        if (k==0)
        {
            shmPos[k] = 0;
        }
        else
        {
            shmPos[k] = shmPos[k-1]+shmIn[k-1][0].md[0].size[0] * shmIn[k-1][0].md[0].size[1];
        }
        daoInfo("SHM%d to position %d\n", k, shmPos[k]);
    }

    clock_t launch, done;
    double diff;
    launch=clock();
    status=1; 
    usleep(1000);
    done=clock();
    diff = (double)(done - launch) / CLOCKS_PER_SEC;
    daoInfo("\n%ld, %ld, %ld\n",done, launch, CLOCKS_PER_SEC);
    daoInfo("SHM Concatenate init status = %d, init time=%.3f\n", status, diff);
    fflush(stdout);

    int threadVal[nbShm];
    struct arg_struct args[nbShm];
    int threadCounter = 0;
    for (threadCounter=0; threadCounter<nbShm; threadCounter++)
    {
        daoInfo("Starting Thread %d\n", threadCounter);
        args[threadCounter].shmId = threadCounter;
        threadVal[threadCounter] = pthread_create(&controllerThread[threadCounter],
                                                     NULL,
                                                     shmNRealTimeLoop,
                                                     (void *)&args[threadCounter]);
        if (threadVal[threadCounter] != 0)
        {
            daoError("Cannot create thread %d\n", threadCounter);
            return DAO_ERROR;
        }
    }
    // Create finalizer Thread
    if (pthread_create(&finalCtrlThread, NULL, finalizeRealTimeLoop, (void *)&threadIdFinalCtrl) != 0)
    {
        daoError("Cannot create finalizer controller thead\n");
        return DAO_ERROR;
    }
    // join last thread
    pthread_join(finalCtrlThread, NULL);

    return DAO_SUCCESS;
}

static void DecodeArgs(int argc, char **argv)
    /*
     **	Parse the input arguments.
     */
{
    char	*str;
    int a1;
    int k;

    argv += 1;	argc -= 1;					/* skip program name */

    while (argc-- > 0) 
    {
        daoDebug("DecodeArgs: working on '%s'/%d\n",*argv,argc);
        str = *argv++;
        if (str[0] != '-') 
        {
            daoError("Do not know arg '%s'\n",str);
            ShowHelp();
            exit(1);
        }

        switch (str[1])
        {
            case 'h':	
                        ShowHelp(); 
                        exit(0);
	        case 'd':	
			            (void)sscanf(*argv++,"%d",&daoLogLevel); 
                        argc -= 1;
			            break;
            case 'l':
                        daoInfo("%s\n",*argv);
                        argv += 1; argc -= 1;
                        break;
            case 'u':
                        (void)sscanf(*argv++,"%d",&a1); argc -= 1;
                        daoDebug("will sleep for %d usec\n",a1);
                        (void)usleep(a1);
                        break;
            case 'n':	
                        (void)sscanf(*argv++,"%d",&nbShm); 
                        daoInfo("nbShm  : %d \n", nbShm);
                        argc -= 1;	
                        break;
            case 'O':
                        (void)sscanf(*argv++,"%s", shmOutName);
                        daoInfo("shmOutName: %s \n", shmOutName);
                        break;
            case 'S':
                        for (k=0; k<nbShm; k++)
                        {
                            (void)sscanf(*argv++,"%s", shmName[k]);
                            daoInfo("shm%dName: %s \n", k, shmName[k]);
                        }
                        break;
            case 'L':
                        daoInfo("SHM Combiner real time control\n");
                        prepRealTime();
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



