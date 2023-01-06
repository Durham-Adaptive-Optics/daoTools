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
#include "daoShm.h" 

typedef int bool_t;
#ifndef TRUE
#define TRUE	1
#endif
#ifndef FALSE
#define FALSE	0
#endif

/*==========================================================================*/
static int	sNdx=0;							/* board index */
static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;


struct timespec tnow;
double tlastupdatedouble;


#define COMBINE_MAX 16

IMAGE *shm;
char shmName[32];
int nbShm;
// Max 16 different SHM to combine
IMAGE *shmIn[COMBINE_MAX];
int updateCnt[COMBINE_MAX];

float frequency;

// Thread
pthread_t controllerThread[COMBINE_MAX];
int threadIdCtrl = 0;

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
    daoInfo("   -l str           display str in output\n");
    /*
     **	Post init tests
     */
    daoInfo("   -L Nb            real time control loop: example daoShmCombiner -L shm N \n");
    daoInfo("                    will combine shm0 + ... + shmN into shm\n");
    /*
     **	Timing tests
     */
    daoInfo("   -t nloops        test timing for i/o\n");
    daoInfo("\n");
}
/*--------------------------------------------------------------------------*/
void * shmNRealTimeLoop(void *thread_data)
{
    struct arg_struct *args = (struct arg_struct *)thread_data;
    daoInfo("ThreadId=%d ENTERING LOOP\n", args->shmId);

    fflush(stdout);
    struct timespec t[2];
    double elapsedTime;
    clock_gettime(CLOCK_REALTIME, &t[1]);
    int nbVal = shm[0].md[0].size[0] * shm[0].md[0].size[1];
    struct timespec timeout;
    int k;
    while (end==0) 
    {
        // Wait for SHM semaphore
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec +=1;
        if (sem_timedwait(shmIn[args->shmId][0].semptr[0], &timeout) != -1)
        {
            if (daoShmCombineShm2Shm(shmIn, shm, nbShm, nbVal) == DAO_ERROR)
            {
                daoError("Combiner failed for thread %d\n", args->shmId);
            }
            updateCnt[args->shmId] ++;
        }
        t[0]=t[1];
        clock_gettime(CLOCK_REALTIME, &t[1]);
        elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;    // sec to ms
        elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6; // us to ms
        elapsedTime = elapsedTime; // in sec... :-)
        if (args->shmId == 0)
        { 
            printf("\r");
            for (k=0; k< nbShm; k++)
            {
                printf("%10d ", updateCnt[k]);
            }
            fflush(stdout);
        }
    }

    daoInfo("EXITING MAIN LOOP\n");
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

    shm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(shmName, "", &shm[0]);
    daoInfo("%s shm created", shmName);
    
    char nameId[32];
    // create shm (/tmp/<shmName><nameId>.im.shm)
    for (k=0; k<nbShm; k++)
    {
        sprintf(nameId, "%02d", k);
        shmIn[k] = (IMAGE *)malloc(sizeof(IMAGE));
        daoShmShm2Img(nameId, shmName, &shmIn[k][0]);
        daoInfo("%s%s shm created\n", shmName, nameId);
        updateCnt[k] = 0;
    }

    clock_t launch, done;
    double diff;
    launch=clock();
    status=1; 
    usleep(1000);
    done=clock();
    diff = (double)(done - launch) / CLOCKS_PER_SEC;
    daoInfo("\n%ld, %ld, %ld\n",done, launch, CLOCKS_PER_SEC);
    daoInfo("SHM monitoring init status = %d, init time=%.3f\n", status, diff);
    fflush(stdout);

    int threadVal[nbShm];
    struct arg_struct args[nbShm];
    int threadCounter = 0;
    for (threadCounter=0; threadCounter<nbShm; threadCounter++)
    {
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
    // join last thread
    pthread_join(controllerThread[threadCounter-1], NULL);

    return DAO_SUCCESS;
}

static void DecodeArgs(int argc, char **argv)
    /*
     **	Parse the input arguments.
     */
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
            case 'L':
                        daoInfo("CAM real time control\n");
                        (void)sscanf(*argv++,"%s", shmName);
                        (void)sscanf(*argv++,"%d", &nbShm);
                        daoInfo("shmName: %s \n", shmName);
                        daoInfo("nbShm  : %d \n", nbShm);
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



