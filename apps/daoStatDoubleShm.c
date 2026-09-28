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

#include "dao.h"
#include "daoTools.h"

/*==========================================================================*/
static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;

struct timespec tnow;
double tnowdouble;
double tlastupdatedouble;

double dt_update; // time since last update
double dt_update_lim = 3600.0; // if no command is received during this time, set DM to zero V [sec]


char shmName[DAO_SHM_NAME_LEN];
char shmNameAvg[DAO_SHM_NAME_LEN];
char shmNameRms[DAO_SHM_NAME_LEN];
int popSize=100;
int semNb = DAO_SEM_AUTO;   // -s: a fixed semaphore; default: one of its own

static int   		end     = 0;		           // termination flag
// termination function for SIGINT callback
static void endme(int _a)
{
    (void)_a;
    end = 1;
}

/*--------------------------------------------------------------------------*/
static char	*sArgv0=NULL;					/* name of executable */

static void ShowHelp(void)
{
    printf("%s of " __DATE__ " at " __TIME__ "\n", sArgv0);
    printf("   arguments:\n");
    printf("   -h               display this message and exit\n");
    printf("   -d <level>       log level: 0 warnings and errors (default), 1 info, 2 debug, 3 trace\n");
    printf("   -S               list of SHM (full path separated by space)\n");
    printf("   -s <semNb>       a fixed semaphore to wait on (default: one of its own)\n");
    printf("   -n               number of frame to average\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   -S <SHM> -n <nb Frame> [-s <semNb>] -L\n");
    ;
    printf("\n");
}

/*--------------------------------------------------------------------------*/
void * statRealTimeLoop(void *thread_data)
{
    daoInfo("ThreadId=%p\n", thread_data);
    IMAGE *shm = (IMAGE *)malloc(sizeof(IMAGE));
    IMAGE *shmAvg = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *shmRms = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(shmName, &shm[0]);
    if (shm[0].md[0].atype != _DATATYPE_DOUBLE)
    {
        daoError("%s is not double: %s computes double statistics\n", shmName, sArgv0);
        exit(EXIT_FAILURE);
    }
    if (popSize < 1)
    {
        daoError("-n: at least 1 frame\n");
        exit(EXIT_FAILURE);
    }

    if (daoToolsInsertShmNamePrefixN(shmName, "Avg", shmNameAvg, sizeof shmNameAvg) != DAO_SUCCESS)
        exit(EXIT_FAILURE);
    if (daoToolsInsertShmNamePrefixN(shmName, "Rms", shmNameRms, sizeof shmNameRms) != DAO_SUCCESS)
        exit(EXIT_FAILURE);
    uint32_t size[2];
    size[0] = shm[0].md[0].size[0];
    size[1] = shm[0].md[0].size[1];
    daoShmCreate(shmAvg, shmNameAvg, 2, size, shm[0].md[0].atype, 1, 0);
    daoShmCreate(shmRms, shmNameRms, 2, size, shm[0].md[0].atype, 1, 0);
    daoInfo("Avg/Rms telemetry running for %s -> %s/%s, popSize=%d\n", shmName, shmNameAvg, shmNameRms, popSize);

    // The last popSize frames (a NaN counts as 0), in a ring buffer on the heap, and
    // their running sums in double: the mean, and the RMS about the mean, of the
    // frames in the window. The sums are recomputed exactly once per window, so
    // rounding errors do not build up.
    int nbValue = shm[0].md[0].size[0]*shm[0].md[0].size[1];
    double *buf = malloc((size_t)popSize * nbValue * sizeof(double));
    double *sum = calloc(nbValue, sizeof(double));
    double *sumSq = calloc(nbValue, sizeof(double));
    if (buf == NULL || sum == NULL || sumSq == NULL)
    {
        daoError("cannot allocate the statistics buffers (%d values x %d frames)\n", nbValue, popSize);
        exit(EXIT_FAILURE);
    }
    long count = 0, slot = 0, sinceExact = 0;
    int k;
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(shm, semNb, 1.0) != DAO_SUCCESS)
        {
            daoToolsLoopStatusWait(&status);
            continue;
        }
        daoToolsLoopStatusStart(&status);
        double *cur = buf + (size_t)slot * nbValue;
        for (k = 0; k < nbValue; k++)
        {
            double v = shm[0].array.D[k];
            if (isnan(v))
            {
                v = 0;
            }
            if (count == popSize)              // the oldest frame leaves the window
            {
                sum[k] -= cur[k];
                sumSq[k] -= (double)cur[k] * cur[k];
            }
            cur[k] = v;
            sum[k] += v;
            sumSq[k] += (double)v * v;
        }
        if (count < popSize)
        {
            count++;
        }
        slot = (slot + 1) % popSize;
        if (++sinceExact >= popSize)           // exact sums again, from the window
        {
            sinceExact = 0;
            for (k = 0; k < nbValue; k++)
            {
                sum[k] = sumSq[k] = 0.0;
            }
            for (long f = 0; f < count; f++)
            {
                double *fr = buf + (size_t)f * nbValue;
                for (k = 0; k < nbValue; k++)
                {
                    sum[k] += fr[k];
                    sumSq[k] += (double)fr[k] * fr[k];
                }
            }
        }
        for (k = 0; k < nbValue; k++)
        {
            double mean = sum[k] / count;
            double var = sumSq[k] / count - mean * mean;
            shmAvg[0].array.D[k] = (double)mean;
            shmRms[0].array.D[k] = (double)sqrt(var > 0.0 ? var : 0.0);
        }
        shmAvg[0].md[0].cnt2 = shmRms[0].md[0].cnt2 = shm[0].md[0].cnt2;
        daoShmSetDataPartFinalize(&shmAvg[0]);
        daoShmSetDataPartFinalize(&shmRms[0]);
        daoToolsLoopStatusEnd(&status, ", %ld frames", count);
    }
    printf("\n");
    daoInfo("EXITING MAIN LOOP\n");
    free(buf);
    free(sum);
    free(sumSq);
    daoToolsShmRelease(&shm);
    daoToolsShmRelease(&shmAvg);
    daoToolsShmRelease(&shmRms);
    return NULL;
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    int status;
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    clock_t launch, done;
    double diff;
    launch=clock();
    status=1; 
    usleep(1000);
    done=clock();
    diff = (double)(done - launch) / CLOCKS_PER_SEC;
    daoInfo("\n%ld, %ld, %ld\n",done, launch, CLOCKS_PER_SEC);
    daoInfo("clock init status = %d, init time=%.3f\n", status, diff);
    fflush(stdout);

    // Thread
    pthread_t controllerThread;
    int threadIdCtrl = 0;
    int statThreadVal=0;
    statThreadVal = pthread_create(&controllerThread, NULL, statRealTimeLoop, (void *)&threadIdCtrl);
    if (statThreadVal != 0)
    {
        daoError("Cannot create thread, err\n");
        return DAO_ERROR;
    }
    pthread_join(controllerThread, NULL);
    return DAO_SUCCESS;

    return 0;
}

/**
 *	Parse the input arguments.
 */
static void DecodeArgs(int argc, char **argv)
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
            case 'S':
                        daoInfo("Average & RMS Telemetry real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, shmName, sizeof shmName);
                        break;
            case 'n':
                popSize = daoToolsArgInt(&argc, &argv, str);
                break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                daoInfo("inputShm sem       = %d \n", semNb);
                break;
            case 'L':
                if (realTimeLoop() != 0)         /* could not start, or failed (see above) */
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

