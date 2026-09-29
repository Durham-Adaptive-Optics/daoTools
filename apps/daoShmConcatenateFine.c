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
char shmOutName[DAO_SHM_NAME_LEN];
int nbShm;
int masterChannel=-1;
// Max 16 different SHM to combine
IMAGE *shmIn[CONCATENATE_MAX];
char shmName[CONCATENATE_MAX][DAO_SHM_NAME_LEN];
int updateCnt[CONCATENATE_MAX];
int shmPos[CONCATENATE_MAX];

float frequency;

// Thread
pthread_t controllerThread[CONCATENATE_MAX];
int threadIdCtrl = 0;

struct arg_struct {
    int shmId;
};

// termination flag
static int end     = 0;

// termination function for SIGINT callback
static void endme(int _a)
{
    (void)_a;
    end = 1;
}

static char	*sArgv0=NULL;					/* name of executable */

static void ShowHelp(void)
{
    printf("%s of " __DATE__ " at " __TIME__ "\n", sArgv0);
    printf("   arguments:\n");
    printf("   -h               display this message and exit\n");
    printf("   -d <level>       log level: 0 warnings and errors (default), 1 info, 2 debug, 3 trace\n");
    printf("   -n <nb>          number of SHMs to concatenate (1 to 16), before -S\n");
    printf("   -O <SHM>         the output SHM (holds all the inputs, one after the other)\n");
    printf("   -S               list of SHM (full path separated by space)\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("      -n <nbShm> -O <SHM> -S <SHM1>..<SHMnbShm> -L\n");
    printf("\n");
}
/*--------------------------------------------------------------------------*/
// The inputs' updates, counted under a mutex: the output is published when each input
// was updated once since the last publication (an input updated twice starts over).
static pthread_mutex_t concatMutex = PTHREAD_MUTEX_INITIALIZER;
static daoToolsLoopStatus loopStatus;

void * shmNRealTimeLoop(void *thread_data)
{
    struct arg_struct *args = (struct arg_struct *)thread_data;
    int id = args->shmId;
    daoInfo("ThreadId=%d ENTERING LOOP\n", id);
    int nbVal = shmIn[id][0].md[0].size[0] * shmIn[id][0].md[0].size[1];
    int k;
    while (end == 0)
    {
        if (daoToolsWait(shmIn[id], DAO_SEM_AUTO, 1.0) != DAO_SUCCESS)
        {
            if (id == 0)                          /* one thread reports the waits */
            {
                pthread_mutex_lock(&concatMutex);
                daoToolsLoopStatusWait(&loopStatus);
                pthread_mutex_unlock(&concatMutex);
            }
            continue;
        }
        pthread_mutex_lock(&concatMutex);
        daoToolsLoopStatusStart(&loopStatus);
        daoShmCopyToPosition(shmIn[id], shmOut, nbVal, shmPos[id], 0);
        if (++updateCnt[id] > 1)
        {
            daoWarning("overcount for shm%d (%d), resetting only this SHM\n", id + 1, updateCnt[id]);
            updateCnt[id] = 0;
        }
        int all = 1;
        for (k = 0; k < nbShm; k++)
        {
            all &= (updateCnt[k] == 1);
        }
        if (all)
        {
            for (k = 0; k < nbShm; k++)
            {
                updateCnt[k] = 0;
            }
            daoShmSetDataPartFinalize(&shmOut[0]);
            daoToolsLoopStatusEnd(&loopStatus, ", %d inputs", nbShm);
        }
        pthread_mutex_unlock(&concatMutex);
    }
    daoInfo("EXITING MAIN LOOP (thread %d)\n", id);
    return NULL;
}

/*--------------------------------------------------------------------------*/
static int prepRealTime()
{
    int status;
    int k;
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    if (nbShm < 1 || nbShm > CONCATENATE_MAX)
    {
        daoError("-n: 1 to %d inputs\n", CONCATENATE_MAX);
        exit(2);
    }
    shmOut = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(shmOutName, &shmOut[0]);
    daoInfo("%s shm created\n", shmOutName);
    
    // create shm (/tmp/<shmName><nameId>.im.shm)
    for (k=0; k<nbShm; k++)
    {
        shmIn[k] = (IMAGE *)malloc(sizeof(IMAGE));
        daoToolsShmOpen(shmName[k], &shmIn[k][0]);
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
    // the output holds every input, one after the other
    long total = shmPos[nbShm - 1] + (long)shmIn[nbShm - 1][0].md[0].size[0] * shmIn[nbShm - 1][0].md[0].size[1];
    daoToolsShmCheck(shmOut, shmOutName, -1, total);
    daoToolsLoopStatusInit(&loopStatus);

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
    for (k = 0; k < nbShm; k++)
    {
        pthread_join(controllerThread[k], NULL);
    }

    printf("\n");
    daoToolsShmRelease(&shmOut);
    for (k = 0; k < nbShm; k++)
    {
        daoToolsShmRelease(&shmIn[k]);
    }
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
                    daoLogLevel = daoToolsArgInt(&argc, &argv, str);
                    break;
            case 'l':
                daoInfo("%s\n", daoToolsArgValue(&argc, &argv, str));
                break;
            case 'u':
                a1 = daoToolsArgInt(&argc, &argv, str);
                daoDebug("will sleep for %d usec\n", a1);
                (void)usleep(a1);
                break;
            case 'n':
                nbShm = daoToolsArgInt(&argc, &argv, str);
                daoInfo("nbShm  : %d \n", nbShm);
                break;
            case 'O':
                daoToolsArgNameNext(&argc, &argv, str, shmOutName, sizeof shmOutName);
                daoInfo("shmOutName: %s \n", shmOutName);
                break;
            case 'S':
                if (nbShm < 1 || nbShm > CONCATENATE_MAX)
                {
                    daoError("-S: give -n <nb inputs> (1 to %d) first\n", CONCATENATE_MAX);
                    exit(2);
                }
                        for (k=0; k<nbShm; k++)
                        {
                            daoToolsArgNameNext(&argc, &argv, str, shmName[k], sizeof shmName[k]);
                            daoInfo("shm%dName: %s \n", k, shmName[k]);
                        }
                        break;
            case 'L':
                        daoInfo("SHM Combiner real time control\n");
                        if (prepRealTime() != 0)         /* could not start, or failed (see above) */
                        {
                            exit(EXIT_FAILURE);
                        }
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
    // r = seteuid(euid_called); //This goes up to maximum privileges
    daoToolsSetRtPriority(93); //any number from 0-99; falls back + warns if not permitted
    // r = seteuid(euid_real);//Go back to normal privileges

    sArgv0 = *argv;
    if (argc < 2)
    {                    /* nothing to do: say how */
        ShowHelp();
        return 1;
    }

    DecodeArgs(argc,argv);

    return(sExit);
}
/*==========================================================================*/



