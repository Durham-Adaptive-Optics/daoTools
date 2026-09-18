/*****************************************************************************
  Pyramid WFS project
  s.cetre
 *****************************************************************************/

/*==========================================================================*/
#include <stdio.h>
#include <inttypes.h>
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

IMAGE *shm0;
IMAGE *shm1;
IMAGE *latencyShm;
IMAGE *avgShm;
IMAGE *rmsShm;
IMAGE *arrayShm;

char shm0Name[32];
char shm1Name[32];
char latencyShmName[64];
char avgShmName[64];
char rmsShmName[64];
char arrayShmName[64];
int sem0;
int sem1;
int popSize = 100;   /* -n: sliding-window size for the AVG/RMS SHMs */
int moreInfo = 0;    /* -m: also print frame IDs / diff / negTs counter */

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
    daoInfo("%s of " __DATE__ " at " __TIME__ "\n",sArgv0);
    daoInfo("   arguments:\n");
    daoInfo("   -h               display this message and exit\n");
    daoInfo("   -d               display program debug output\n");
    daoInfo("   -S               list of SHM (full path separated by space)\n");
    daoInfo("   -n <popSize>     sliding-window size for the AVG/RMS SHMs (default 100)\n");
    daoInfo("   -m               also print frame IDs / diff / negative-timestamp counter\n");
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("    daoTimeDiff -S <SHM1> <SHM2> <sem1> <sem2> <measurement SHM> [-n <popSize>] [-m] -L\n");
    daoInfo("\n");
    daoInfo("   Latency (measurement SHM) is published every frame. This also\n");
    daoInfo("   automatically maintains a running AVG and RMS of the last <popSize>\n");
    daoInfo("   samples, published to <measurement SHM>Avg/Rms - no separate\n");
    daoInfo("   daoTimeDiffStat process needed (that tool is still available for\n");
    daoInfo("   computing AVG/RMS on any other scalar SHM). The same window, in\n");
    daoInfo("   chronological (oldest-to-newest) order, is also published as a\n");
    daoInfo("   <popSize>-element array to <measurement SHM>Array, for a GUI/plot\n");
    daoInfo("   to read and display directly with no client-side buffering.\n");
    daoInfo("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    daoInfo("Starting loop, %s/%s \n",shm0Name, shm1Name);
    fflush(stdout);
    shm0 = (IMAGE*) malloc(sizeof(IMAGE));
    shm1 = (IMAGE*) malloc(sizeof(IMAGE));
    latencyShm = (IMAGE*) malloc(sizeof(IMAGE));
    avgShm = (IMAGE*) malloc(sizeof(IMAGE));
    rmsShm = (IMAGE*) malloc(sizeof(IMAGE));
    arrayShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(shm0Name, &shm0[0]);
    daoShmShm2Img(shm1Name, &shm1[0]);
    // Create size array, using 2D of 1x1... can be change to 1D
    uint32_t size[2];
    size[0] = 1;
    size[1] = 1;
    daoShmImageCreate(latencyShm, latencyShmName, 2, size, _DATATYPE_FLOAT, 1, 0);
    daoShmImageCreate(avgShm, avgShmName, 2, size, _DATATYPE_FLOAT, 1, 0);
    daoShmImageCreate(rmsShm, rmsShmName, 2, size, _DATATYPE_FLOAT, 1, 0);
    uint32_t arraySize[2];
    arraySize[0] = (uint32_t)popSize;
    arraySize[1] = 1;
    daoShmImageCreate(arrayShm, arrayShmName, 2, arraySize, _DATATYPE_FLOAT, 1, 0);

    // Sliding window for the AVG/RMS/Array SHMs (merged in from the former
    // daoTimeDiffStat pairing - see -n). circCount tracks how many samples
    // have actually been pushed so AVG/RMS are correct even before the
    // window fills, rather than being diluted by zero-filled slots.
    float *circBuf = (float *) calloc(popSize, sizeof(float));
    // Reused each update to publish circBuf in chronological (oldest-to-
    // newest) order - circBuf's own physical layout wraps with circHead, so
    // publishing it as-is would show a jump/wrap in a plot every time it
    // does. Only the first circCount elements are ever written/valid.
    float *orderedBuf = (float *) calloc(popSize, sizeof(float));
    int circHead = 0;
    int circCount = 0;

    struct timespec t0, t1;
    int64_t elapsedTimeNs;
    int64_t frameId0 = 0, frameId1 = 0, frameIdDiff = 0;
    float latency[1] = {0};
    float avgOut[1]  = {0};
    float rmsOut[1]  = {0};
    struct timespec timeout;
    int nbNegTs=0;
    int cnt=0;

    // Throttle stdout to ~1 Hz: a write()/fflush() every frame is a syscall
    // on a path that can run at any rate, and nobody reads a terminal that
    // fast anyway (the SHMs above are already updated every frame).
    struct timespec lastPrint;
    clock_gettime(CLOCK_MONOTONIC, &lastPrint);

    while (end ==0)
    {
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        // wait for 2nd shm
        if (daoShmWaitForSemaphoreTimeout(shm1, sem1, &timeout) != DAO_TIMEOUT)
        {
            // Full sec+nsec timestamp: tsfixed.secondlong alone is only the
            // nsec-within-second component (tsfixed and ts are a union over
            // the same two int64s), so a diff across a second boundary would
            // come out hugely wrong. Use both fields.
            t0 = shm0[0].md[0].atime.ts;
            t1 = shm1[0].md[0].atime.ts;
            elapsedTimeNs = (int64_t)(t1.tv_sec - t0.tv_sec) * 1000000000LL
                          + (int64_t)(t1.tv_nsec - t0.tv_nsec);
            latency[0] = (float)elapsedTimeNs / 1e3;
            if (moreInfo)
            {
                frameId0 = daoShmGetCounter(shm0);
                frameId1 = daoShmGetCounter(shm1);
                frameIdDiff = frameId1 - frameId0;
            }
            if (elapsedTimeNs < 0)
            {
                nbNegTs++;
            }
            else
            {
                daoShmImage2Shm((float *)latency, 1, &latencyShm[0]);

                // push into the sliding window, then recompute AVG/RMS
                circBuf[circHead] = latency[0];
                circHead = (circHead + 1) % popSize;
                if (circCount < popSize) circCount++;

                double sum = 0.0;
                for (int i = 0; i < circCount; i++) sum += circBuf[i];
                double avg = sum / circCount;

                double sq = 0.0;
                for (int i = 0; i < circCount; i++)
                {
                    double diff = circBuf[i] - avg;
                    sq += diff * diff;
                }
                double rms = sqrt(sq / circCount);

                avgOut[0] = (float)avg;
                rmsOut[0] = (float)rms;
                daoShmImage2Shm((float *)avgOut, 1, &avgShm[0]);
                daoShmImage2Shm((float *)rmsOut, 1, &rmsShm[0]);

                // circBuf in chronological order: not yet full -> it hasn't
                // wrapped, so it's already in order; full -> the oldest
                // sample sits at circHead (about to be overwritten next).
                if (circCount < popSize)
                {
                    memcpy(orderedBuf, circBuf, (size_t)circCount * sizeof(float));
                }
                else
                {
                    memcpy(orderedBuf, circBuf + circHead, (size_t)(popSize - circHead) * sizeof(float));
                    memcpy(orderedBuf + (popSize - circHead), circBuf, (size_t)circHead * sizeof(float));
                }
                daoShmImage2Shm(orderedBuf, (uint32_t)circCount, &arrayShm[0]);
            }

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double sincePrint = (double)(now.tv_sec - lastPrint.tv_sec)
                              + (double)(now.tv_nsec - lastPrint.tv_nsec) / 1e9;
            if (sincePrint >= 1.0)
            {
                if (moreInfo)
                    printf("\rf1ID = %" PRId64 ", f2ID = %" PRId64 ", diff = %" PRId64 ", negTs = %d, "
                           "latency = %13.3f us, AVG = %13.3f us, RMS = %13.3f us",
                           frameId0, frameId1, frameIdDiff, nbNegTs,
                           latency[0], avgOut[0], rmsOut[0]);
                else
                    printf("\rlatency = %13.3f us, AVG = %13.3f us, RMS = %13.3f us",
                           latency[0], avgOut[0], rmsOut[0]);
                fflush(stdout);
                lastPrint = now;
            }
        }
        else
        {
            printf("\rTimeout %d: shm1", cnt);
            fflush(stdout);
            cnt++;
        }
    }
    daoInfo("EXITING MAIN LOOP\n");
    free(circBuf);
    free(orderedBuf);

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

        switch (str[1]) {
            case 'h':	
                        ShowHelp();
                         exit(0);
            case 'd':	
                        (void)sscanf(*argv++,"%d",&daoLogLevel); argc -= 1;
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
            case 'S':
                        daoInfo("Simple Camera Reader and Writer from SHM real time control\n");
                    	(void)sscanf(*argv++,"%s",shm0Name); argc -= 1;
                    	(void)sscanf(*argv++,"%s",shm1Name); argc -= 1;
                    	(void)sscanf(*argv++,"%d",&sem0); argc -= 1;
                    	(void)sscanf(*argv++,"%d",&sem1); argc -= 1;
                    	(void)sscanf(*argv++,"%s",latencyShmName); argc -= 1;
                        daoToolsInsertShmNamePrefix(latencyShmName, "Avg", avgShmName);
                        daoToolsInsertShmNamePrefix(latencyShmName, "Rms", rmsShmName);
                        daoToolsInsertShmNamePrefix(latencyShmName, "Array", arrayShmName);
                        daoInfo("measurement SHM = %s\n", latencyShmName);
                        daoInfo("AVG SHM         = %s\n", avgShmName);
                        daoInfo("RMS SHM         = %s\n", rmsShmName);
                        daoInfo("ARRAY SHM       = %s\n", arrayShmName);
                        break;
            case 'n':
                        (void)sscanf(*argv++,"%d",&popSize); argc -= 1;
                        daoInfo("AVG/RMS window (popSize) = %d\n", popSize);
                        break;
            case 'm':
                        moreInfo = 1;
                        daoInfo("verbose (frame IDs / diff / negTs) display enabled\n");
                        break;
            case 'L':
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
    // r = seteuid(euid_called); //This goes up to maximum privileges
    daoToolsSetRtPriority(93); //any number from 0-99; falls back + warns if not permitted
    // r = seteuid(euid_real);//Go back to normal privileges

    sArgv0 = *argv;

    DecodeArgs(argc,argv);

    return(sExit);
}
/*==========================================================================*/
