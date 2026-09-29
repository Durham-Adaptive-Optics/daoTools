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

IMAGE *shm;
IMAGE *shmAvg;

char shmName[DAO_SHM_NAME_LEN];
char shmNameAvg[DAO_SHM_NAME_LEN];
int nbAvg=100;
double avgTime=0.0;        // > 0: average over this many seconds (-t) instead of nbAvg frames
int maxFrames=100000;      // -m: the most frames kept for a time average (memory bound)
int semNb = DAO_SEM_AUTO;   // -s: a fixed semaphore; default: one of its own

/* NaN or infinity, from the bits: daoTools builds with -ffast-math, where isnan()
 * is assumed false and compiled away */
static inline int notFinite(float v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    return (u & 0x7f800000u) == 0x7f800000u;
}

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
    printf("   -n               number of frames to average (default 100)\n");
    printf("   -t               average over this time instead, in seconds: the frames\n");
    printf("                    received in the last t s, however many (the loop rate)\n");
    printf("   -m               time average: the most frames kept (default 100000)\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage (options before -L):\n");
    printf("   -S <SHM> -n <nb Average> [-s <semNb>] -L\n");
    printf("   -S <SHM> -t <seconds> [-m <max frames>] [-s <semNb>] -L\n");
    printf("   The output, <SHM>Avg, is the mean of the frames in the window: after a\n");
    printf("   start, of those received so far.\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    shm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(shmName, &shm[0]);
    // Create receiving Avg SHM
    shmAvg = (IMAGE*) malloc(sizeof(IMAGE));
    if (daoToolsInsertShmNamePrefixN(shmName, "Avg", shmNameAvg, sizeof shmNameAvg) != DAO_SUCCESS)
        exit(EXIT_FAILURE);
    uint32_t size[2];
    size[0] = shm[0].md[0].size[0];
    size[1] = shm[0].md[0].size[1];
    daoShmCreate(shmAvg, shmNameAvg, 2, size, shm[0].md[0].atype, 1, 0);
    //daoShmOpen(shmNameAvg, &shmAvg[0]);
    printf("Starting loop, %s -> %s, nAvg=%d\n",shmName, shmNameAvg, nbAvg );
    fflush(stdout);

    if (shm[0].md[0].atype != _DATATYPE_FLOAT)
    {
        daoError("%s is not float: daoAvgShm averages float SHMs\n", shmName);
        exit(EXIT_FAILURE);
    }
    int nbValue = shm[0].md[0].size[0]*shm[0].md[0].size[1];
    float *avgValue = shmAvg[0].array.F;

    // The frames in the window, in a ring buffer (oldest at `first`, `count` of them),
    // each with its arrival time; `sum` is their running sum, in double. By frames:
    // capacity nbAvg, the oldest leaves when a new one comes in a full buffer. By time:
    // the buffer grows as needed (up to maxFrames) and the frames older than avgTime
    // leave at each new frame.
    int timeMode = (avgTime > 0.0);
    long capacity = timeMode ? 1024 : nbAvg;
    if (capacity < 1)
        capacity = 1;
    float *buf = malloc((size_t)capacity * nbValue * sizeof(float));
    double *stamp = malloc((size_t)capacity * sizeof(double));
    double *sum = calloc(nbValue, sizeof(double));
    if (buf == NULL || stamp == NULL || sum == NULL)
    {
        daoError("cannot allocate the average buffers\n");
        exit(EXIT_FAILURE);
    }
    long first = 0, count = 0;
    int k;
    for (k=0; k<nbValue; k++)
        avgValue[k] = 0.0;
    if (timeMode)
        printf("Average telemetry running for %s -> %s, over %g s\n", shmName, shmNameAvg, avgTime);
    else
        printf("Average telemetry running for %s -> %s, nAvg=%d\n", shmName, shmNameAvg, nbAvg);
    struct timespec tNow;
    int warnedFull = 0;
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(shm, semNb, 1.0) == DAO_SUCCESS)
        {
            daoToolsLoopStatusStart(&status);
            clock_gettime(CLOCK_MONOTONIC, &tNow);
            double now = tNow.tv_sec + 1e-9 * tNow.tv_nsec;
            // the oldest frames leave: by time, those out of the window; by frames, one
            // if the buffer is full
            while (count > 0 && ((timeMode && now - stamp[first] > avgTime) ||
                                 (!timeMode && count >= capacity)))
            {
                float *old = buf + (size_t)first * nbValue;
                for (k=0; k<nbValue; k++)
                    sum[k] -= old[k];
                first = (first + 1) % capacity;
                count--;
            }
            if (count == capacity)                 // time mode, full: grow, or drop the oldest
            {
                long newCap = capacity * 2 > maxFrames ? maxFrames : capacity * 2;
                float *nb = newCap > capacity ? malloc((size_t)newCap * nbValue * sizeof(float)) : NULL;
                double *ns = newCap > capacity ? malloc((size_t)newCap * sizeof(double)) : NULL;
                if (nb != NULL && ns != NULL)
                {
                    long i;
                    for (i=0; i<count; i++)        // unroll the ring: oldest first
                    {
                        long src = (first + i) % capacity;
                        memcpy(nb + (size_t)i * nbValue, buf + (size_t)src * nbValue, nbValue * sizeof(float));
                        ns[i] = stamp[src];
                    }
                    free(buf); free(stamp);
                    buf = nb; stamp = ns; capacity = newCap; first = 0;
                }
                else
                {
                    free(nb); free(ns);
                    if (!warnedFull)
                    {
                        daoWarning("%ld frames in %g s: the window is capped at %ld frames (-m)\n",
                                   count, avgTime, capacity);
                        warnedFull = 1;
                    }
                    float *old = buf + (size_t)first * nbValue;
                    for (k=0; k<nbValue; k++)
                        sum[k] -= old[k];
                    first = (first + 1) % capacity;
                    count--;
                }
            }
            // the new frame comes in (a NaN or an infinity counts as 0)
            long slot = (first + count) % capacity;
            float *cur = buf + (size_t)slot * nbValue;
            for (k=0; k<nbValue; k++)
            {
                float v = shm[0].array.F[k];
                cur[k] = notFinite(v) ? 0.0f : v;
                sum[k] += cur[k];
            }
            stamp[slot] = now;
            count++;
            for (k=0; k<nbValue; k++)
                avgValue[k] = (float)(sum[k] / count);
            daoShmSetDataPartFinalize(&shmAvg[0]);
            daoToolsLoopStatusEnd(&status, ", %ld frames averaged", count);
        }
        else
        {
            daoToolsLoopStatusWait(&status);
        }
    }
    free(buf); free(stamp); free(sum);

    printf("\n");
    daoInfo("EXITING MAIN LOOP\n");
    daoToolsShmRelease(&shm);
    daoToolsShmRelease(&shmAvg);
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
                        printf("Average Telemetry real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, shmName, sizeof shmName);
                        daoInfo("shmName          = %s\n", shmName);
                        break;
            case 'n':
                nbAvg = daoToolsArgInt(&argc, &argv, str);
                daoInfo("nb Average       = %d \n", nbAvg);
                break;
            case 't':
                avgTime = daoToolsArgDouble(&argc, &argv, str);
                daoInfo("average over     = %g s\n", avgTime);
                break;
            case 'm':
                maxFrames = daoToolsArgInt(&argc, &argv, str);
                daoInfo("max frames       = %d\n", maxFrames);
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
