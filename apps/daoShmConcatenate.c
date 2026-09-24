/*****************************************************************************
  DAO project
  s.cetre

  daoShmConcatenate2.c
  Concatenate 2 input SHMs into one output SHM:
      out = [in1][in2]  (flat concatenate)

  Finalize policy:
    - default (no -m): finalize on ANY input update
    - -m 1: finalize only when in1 updates
    - -m 2: finalize only when in2 updates

  Usage:
    daoShmConcatenate2 -S <outShm> <in1Shm> <in2Shm> -L
    daoShmConcatenate2 -m 1 -S <outShm> <in1Shm> <in2Shm> -L
    daoShmConcatenate2 -m 2 -S <outShm> <in1Shm> <in2Shm> -L
 *****************************************************************************/

/*==========================================================================*/
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
/* DAO header */
#include "dao.h"
#include "daoTools.h"

/*==========================================================================*/
typedef int bool_t;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/*==========================================================================*/
static int  sExit = 0;          /* program exit code */
static char *sArgv0 = NULL;     /* name of executable */

#define NB_SHM 2

static volatile int gEnd = 0;

static IMAGE *gShmOut = NULL;
static IMAGE *gShmIn[NB_SHM] = { NULL, NULL };

static char gShmOutName[256];
static char gShmInName[NB_SHM][256];

static int  gMasterChannel = -1;     /* -1 => finalize on any input update, 1 => shm1, 2 => shm2 */

static int  gNbValIn[NB_SHM] = { 0, 0 };
static int  gOutExpectedNbVal = 0;

static int  gPos[NB_SHM] = { 0, 0 }; /* flat positions in output */

static pthread_t gThread[NB_SHM];
static pthread_mutex_t gOutMutex = PTHREAD_MUTEX_INITIALIZER;

struct arg_struct {
    int shmId; /* 0 or 1 */
};

/*==========================================================================*/
static void endme(int signum)
{
    (void)signum;
    gEnd = 1;
}

/*==========================================================================*/
static void ShowHelp(void)
{
    daoInfo("%s of " __DATE__ " at " __TIME__ "\n", sArgv0);
    daoInfo("arguments:\n");
    daoInfo("  -h               display this message and exit\n");
    daoInfo("  -d <lvl>         set daoLogLevel\n");
    daoInfo("  -m <1|2>         master channel: finalize only when shm1 or shm2 updates\n");
    daoInfo("                   if not provided: finalize on ANY input update\n");
    daoInfo("  -S <out> <in1> <in2>\n");
    daoInfo("  -L               start real-time loop\n");
    daoInfo("\n");
    daoInfo("examples:\n");
    daoInfo("  %s -S /tmp/out.im.shm /tmp/in1.im.shm /tmp/in2.im.shm -L\n", sArgv0);
    daoInfo("  %s -m 1 -S /tmp/out.im.shm /tmp/in1.im.shm /tmp/in2.im.shm -L\n", sArgv0);
    daoInfo("  %s -m 2 -S /tmp/out.im.shm /tmp/in1.im.shm /tmp/in2.im.shm -L\n", sArgv0);
}

/*==========================================================================*/
static int shouldFinalize(int shmId0based)
{
    /* shmId0based: 0=>in1, 1=>in2 */
    if (gMasterChannel == -1)
        return TRUE;

    /* user passes -m 1 or -m 2 */
    if (gMasterChannel == (shmId0based + 1))
        return TRUE;

    return FALSE;
}

/*==========================================================================*/
static int validateSizesAndPositions(void)
{
    int outNbVal;

    gNbValIn[0] = gShmIn[0][0].md[0].size[0] * gShmIn[0][0].md[0].size[1];
    gNbValIn[1] = gShmIn[1][0].md[0].size[0] * gShmIn[1][0].md[0].size[1];

    gPos[0] = 0;
    gPos[1] = gNbValIn[0];

    gOutExpectedNbVal = gNbValIn[0] + gNbValIn[1];

    outNbVal = gShmOut[0].md[0].size[0] * gShmOut[0].md[0].size[1];

    daoInfo("in1 nbVal=%d\n", gNbValIn[0]);
    daoInfo("in2 nbVal=%d\n", gNbValIn[1]);
    daoInfo("out nbVal=%d (expected %d)\n", outNbVal, gOutExpectedNbVal);

    if (outNbVal != gOutExpectedNbVal)
    {
        daoError("Output SHM size mismatch: out=%d but expected in1+in2=%d\n",
                 outNbVal, gOutExpectedNbVal);
        return DAO_ERROR;
    }

    /* Optional: warn if pixel formats differ */
    if (gShmIn[0][0].md[0].atype != gShmIn[1][0].md[0].atype)
        daoWarning("Inputs have different atype (in1=%d in2=%d). This is probably wrong.\n",
                   gShmIn[0][0].md[0].atype, gShmIn[1][0].md[0].atype);

    if (gShmOut[0].md[0].atype != gShmIn[0][0].md[0].atype)
        daoWarning("Output atype differs from in1 (out=%d in1=%d). This is probably wrong.\n",
                   gShmOut[0].md[0].atype, gShmIn[0][0].md[0].atype);

    return DAO_SUCCESS;
}

/*==========================================================================*/
static void *shmThreadLoop(void *thread_data)
{
    struct arg_struct *args = (struct arg_struct *)thread_data;
    int shmId = args->shmId; /* 0 or 1 */

    struct timespec timeout;
    struct timespec t0, t1;
    double elapsedUs;

    daoInfo("Thread shm%d ENTERING LOOP\n", shmId + 1);
    fflush(stdout);

    while (!gEnd)
    {
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;

        if (daoShmWaitSemTimeout(gShmIn[shmId], 0, &timeout) == -1)
            continue;

        clock_gettime(CLOCK_REALTIME, &t0);

        pthread_mutex_lock(&gOutMutex);

        /* Copy this input into its flat position in output */
        daoShmCopyToPosition(gShmIn[shmId], gShmOut,
                             gNbValIn[shmId],
                             gPos[shmId],
                             0);

        /* Keep output cnt2 aligned with “the event that finalized”:
           - default: use triggering shm
           - master: use master shm (only finalized on master anyway) */
        gShmOut[0].md[0].cnt2 = gShmIn[shmId][0].md[0].cnt2;

        if (shouldFinalize(shmId))
        {
            daoShmSetDataPartFinalize(&gShmOut[0]);
        }

        pthread_mutex_unlock(&gOutMutex);

        clock_gettime(CLOCK_REALTIME, &t1);
        elapsedUs = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;

        /* Lightweight status */
        printf("\r update shm%d | copy+%s time = %.1f us | master=%d     ",
               shmId + 1,
               shouldFinalize(shmId) ? "finalize" : "copy-only",
               elapsedUs,
               gMasterChannel);
        fflush(stdout);
    }

    daoInfo("Thread shm%d EXITING LOOP\n", shmId + 1);
    fflush(stdout);
    return (void *)DAO_SUCCESS;
}

/*==========================================================================*/
static int prepRealTime(void)
{
    int i;
    int threadVal[NB_SHM];
    struct arg_struct args[NB_SHM];

    signal(SIGINT, endme);

    gShmOut = (IMAGE *)malloc(sizeof(IMAGE));
    gShmIn[0] = (IMAGE *)malloc(sizeof(IMAGE));
    gShmIn[1] = (IMAGE *)malloc(sizeof(IMAGE));
    if (!gShmOut || !gShmIn[0] || !gShmIn[1])
    {
        daoError("malloc failed\n");
        return DAO_ERROR;
    }

    daoShmOpen(gShmOutName, &gShmOut[0]);
    daoInfo("Connected OUT: %s\n", gShmOutName);

    daoShmOpen(gShmInName[0], &gShmIn[0][0]);
    daoInfo("Connected IN1: %s\n", gShmInName[0]);

    daoShmOpen(gShmInName[1], &gShmIn[1][0]);
    daoInfo("Connected IN2: %s\n", gShmInName[1]);

    if (validateSizesAndPositions() == DAO_ERROR)
        return DAO_ERROR;

    daoInfo("Finalize policy: %s\n",
            (gMasterChannel == -1) ? "ANY input update" :
            (gMasterChannel == 1)  ? "ONLY shm1 updates" :
            (gMasterChannel == 2)  ? "ONLY shm2 updates" :
                                     "INVALID (but continuing)");

    for (i = 0; i < NB_SHM; i++)
    {
        args[i].shmId = i;
        threadVal[i] = pthread_create(&gThread[i], NULL, shmThreadLoop, (void *)&args[i]);
        if (threadVal[i] != 0)
        {
            daoError("Cannot create thread %d\n", i);
            return DAO_ERROR;
        }
    }

    /* Join both (Ctrl+C to exit) */
    pthread_join(gThread[0], NULL);
    pthread_join(gThread[1], NULL);

    return DAO_SUCCESS;
}

/*==========================================================================*/
static void DecodeArgs(int argc, char **argv)
{
    char *str;

    argv += 1; argc -= 1;

    while (argc-- > 0)
    {
        str = *argv++;
        if (str[0] != '-')
        {
            daoError("Do not know arg '%s'\n", str);
            ShowHelp();
            exit(1);
        }

        switch (str[1])
        {
            case 'h':
                ShowHelp();
                exit(0);

            case 'd':
                (void)sscanf(*argv++, "%d", &daoLogLevel);
                argc -= 1;
                break;

            case 'm':
                (void)sscanf(*argv++, "%d", &gMasterChannel);
                argc -= 1;
                if (!(gMasterChannel == -1 || gMasterChannel == 1 || gMasterChannel == 2))
                {
                    daoWarning("Invalid -m %d (expected 1 or 2). Using default (-1).\n", gMasterChannel);
                    gMasterChannel = -1;
                }
                break;

            case 'S':
                (void)sscanf(*argv++, "%255s", gShmOutName);
                (void)sscanf(*argv++, "%255s", gShmInName[0]);
                (void)sscanf(*argv++, "%255s", gShmInName[1]);
                daoInfo("OUT : %s\n", gShmOutName);
                daoInfo("IN1 : %s\n", gShmInName[0]);
                daoInfo("IN2 : %s\n", gShmInName[1]);
                argc -= 3;
                break;

            case 'L':
                daoInfo("SHM Concatenate2 real-time loop\n");
                if (prepRealTime() == DAO_ERROR)
                    exit(2);
                break;

            default:
                daoError("Do not know arg '%s'\n", str);
                ShowHelp();
                exit(2);
        }
    }
}

/*==========================================================================*/
int main(int argc, char **argv)
{
    sArgv0 = (argc > 0) ? argv[0] : (char *)"program";

    /* RT setup early */
    daoToolsSetRtPriority(93);

    /* Your normal argument decode / program start */
    DecodeArgs(argc, argv);

    return sExit;
}
/*==========================================================================*/
