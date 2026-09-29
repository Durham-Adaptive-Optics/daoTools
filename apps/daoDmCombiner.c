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


#define COMBINE_MAX 16

IMAGE *shm;
char shmName[DAO_SHM_NAME_LEN];
int nbShm;
int masterChannel=-1;
// Max 16 different SHM to combine
IMAGE *shmIn[COMBINE_MAX];

float frequency;
int removePiston = 0;
double clipping = 1.0;
// Thread
pthread_t controllerThread[COMBINE_MAX];
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
    printf("   Combines <out SHM>00 ... <out SHM>NN (NN = <nb> - 1) into <out SHM>, each time one\n");
    printf("   of them is updated (or only the master channel, -m).\n");
    printf("   arguments:\n");
    printf("   -h               display this message and exit\n");
    printf("   -d <level>       log level: 0 warnings and errors (default), 1 info, 2 debug, 3 trace\n");
    printf("   -m <channel>     only this input triggers a combination (default -1: any)\n");
    printf("   -r               remove the piston of the combined command\n");
    printf("   -c <clipping>    clip the combined command to [-clipping, clipping] (default 1)\n");
    printf("   -S <out SHM> <nb>  the output SHM and its number of inputs (1 to 16)\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   [-m <channel>] [-r] [-c <clipping>] -S <out SHM> <nb> -L\n");
    printf("\n");
}
/*--------------------------------------------------------------------------*/
void * shmNRealTimeLoop(void *thread_data)
{
    struct arg_struct *args = (struct arg_struct *)thread_data;
    daoInfo("ThreadId=%d ENTERING LOOP\n", args->shmId);
    int nbVal = shm[0].md[0].size[0] * shm[0].md[0].size[1];
    // as before, the threads combine independently; one of them (the master channel,
    // else input 0) reports its own frames once per second
    int reporter = (args->shmId == ((masterChannel == -1) ? 0 : masterChannel));
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end==0) 
    {
        if (daoToolsWait(shmIn[args->shmId], DAO_SEM_AUTO, 1.0) == DAO_SUCCESS)
        {
            if (reporter)
            {
                daoToolsLoopStatusStart(&status);
            }
            if (daoDmCombine(shmIn, shm, nbShm, nbVal, removePiston, clipping) == DAO_ERROR)
            {
                daoError("Combiner failed for thread %d\n", args->shmId);
            }
            if (reporter)
            {
                daoToolsLoopStatusEnd(&status, ", %d inputs", nbShm);
            }
        }
        else if (reporter)
        {
            daoToolsLoopStatusWait(&status);
        }
        if (args->shmId == 1)                      // as before: thread 1 sets the frame number
        {
            shm[0].md[0].cnt2 = shmIn[0][0].md[0].cnt2;
        }
    }
    daoInfo("EXITING MAIN LOOP (thread %d)\n", args->shmId);
    return NULL;
}
    
/*--------------------------------------------------------------------------*/
static int prepRealTime()
{
    int status;
    int k;
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    if (nbShm < 1 || nbShm > COMBINE_MAX)
    {
        daoError("-S: 1 to %d inputs\n", COMBINE_MAX);
        exit(2);
    }
    if (masterChannel < -1 || masterChannel >= nbShm)
    {
        daoError("-m: an input from 0 to %d, or -1 (any)\n", nbShm - 1);
        exit(2);
    }
    shm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(shmName, &shm[0]);
    daoInfo("%s shm created", shmName);
    
    // create shm (/tmp/<shmName><nameId>.im.shm)
    for (k=0; k<nbShm; k++)
    {
        char nameId[32];
        char shmNameId[DAO_SHM_NAME_LEN];
        sprintf(nameId, "%02d", k);
        if (daoToolsInsertShmNamePrefixN(shmName, nameId, shmNameId, sizeof shmNameId) != DAO_SUCCESS)
            exit(EXIT_FAILURE);
        shmIn[k] = (IMAGE *)malloc(sizeof(IMAGE));
        daoToolsShmOpen(shmNameId, &shmIn[k][0]);
        daoInfo("%s shm created\n", shmNameId);
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

    daoInfo("masterChannel = %d\n", masterChannel);
    // If master Channel == -1, everything channels can trigger a combination. If the master channel is not -1,
    // the master channel is the only SHM which can trigger a command
    int threadVal[nbShm];
    struct arg_struct args[nbShm];
    int threadCounter = 0;
    if (masterChannel == -1)
    {
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
        for (k = 0; k < nbShm; k++)
        {
            pthread_join(controllerThread[k], NULL);
        }
    }
    else
    {
        args[masterChannel].shmId = masterChannel;
        threadVal[masterChannel] = pthread_create(&controllerThread[masterChannel],
                                                     NULL,
                                                     shmNRealTimeLoop,
                                                     (void *)&args[masterChannel]);
        if (threadVal[masterChannel] != 0)
        {
            daoError("Cannot create thread %d\n", masterChannel);
            return DAO_ERROR;
        }
        pthread_join(controllerThread[masterChannel], NULL);
    }

    printf("\n");
    daoToolsShmRelease(&shm);
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
            case 'm':
                masterChannel = daoToolsArgInt(&argc, &argv, str);
                break;
            case 'r':
                        removePiston = 1;
                        daoInfo("removePiston set to %d\n", removePiston);
                        break;
            case 'c':
                clipping = daoToolsArgDouble(&argc, &argv, str);
                daoInfo("clipping value set to %f\n", clipping);
                break;
            case 'S':
                daoToolsArgNameNext(&argc, &argv, str, shmName, sizeof shmName);
                nbShm = daoToolsArgInt(&argc, &argv, str);
                daoInfo("shmName: %s \n", shmName);
                daoInfo("nbShm  : %d \n", nbShm);
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



