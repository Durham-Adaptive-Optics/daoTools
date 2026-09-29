/*****************************************************************************
  DAO project
  s.cetre

  daoShmAdd.c
  Add 2 input SHMs into one output SHM:
      out[i] = in1[i] + in2[i]

  Finalize policy:
    - default (no -m): finalize on ANY input update
    - -m 1: finalize only when shm1 updates
    - -m 2: finalize only when shm2 updates

  Usage:
    daoShmAdd -S <outShm> <in1Shm> <in2Shm> -L
    daoShmAdd -m 1 -S <outShm> <in1Shm> <in2Shm> -L
    daoShmAdd -m 2 -S <outShm> <in1Shm> <in2Shm> -L
 *****************************************************************************/

/*==========================================================================*/
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>

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
static int   sExit  = 0;          /* program exit code */
static char *sArgv0 = NULL;       /* name of executable */

#define NB_SHM 2

static volatile int gEnd = 0;

static IMAGE *gShmOut = NULL;
static daoToolsLoopStatus gStatus;   /* one status line for both threads (under gOutMutex) */
static IMAGE *gShmIn[NB_SHM] = { NULL, NULL };

static char gShmOutName[DAO_SHM_NAME_LEN];
static char gShmInName[NB_SHM][DAO_SHM_NAME_LEN];

static int  gMasterChannel = -1;  /* -1 => finalize on any update, 1 => shm1, 2 => shm2 */
static int  gNbVal = 0;

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
    printf("%s of " __DATE__ " at " __TIME__ "\n", sArgv0);
    printf("arguments:\n");
    printf("   -h               display this message and exit\n");
    printf("   -d <level>       log level: 0 warnings and errors (default), 1 info, 2 debug, 3 trace\n");
    printf("  -m <1|2>         master channel: finalize only when shm1 or shm2 updates\n");
    printf("                   if not provided: finalize on ANY input update\n");
    printf("  -S <out> <in1> <in2>\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("\n");
    printf("examples:\n");
    printf("  %s -S /tmp/out.im.shm /tmp/in1.im.shm /tmp/in2.im.shm -L\n", sArgv0);
    printf("  %s -m 1 -S /tmp/out.im.shm /tmp/in1.im.shm /tmp/in2.im.shm -L\n", sArgv0);
    printf("  %s -m 2 -S /tmp/out.im.shm /tmp/in1.im.shm /tmp/in2.im.shm -L\n", sArgv0);
}

/*==========================================================================*/
static int shouldFinalize(int shmId0based)
{
    if (gMasterChannel == -1)
        return TRUE;

    if (gMasterChannel == (shmId0based + 1))
        return TRUE;

    return FALSE;
}

/*==========================================================================*/
static int validateSizesAndTypes(void)
{
    int nb1, nb2, nbOut;

    nb1   = gShmIn[0][0].md[0].size[0] * gShmIn[0][0].md[0].size[1];
    nb2   = gShmIn[1][0].md[0].size[0] * gShmIn[1][0].md[0].size[1];
    nbOut = gShmOut[0].md[0].size[0]  * gShmOut[0].md[0].size[1];

    daoInfo("in1  nbVal=%d\n", nb1);
    daoInfo("in2  nbVal=%d\n", nb2);
    daoInfo("out  nbVal=%d\n", nbOut);

    if (nb1 != nb2)
    {
        daoError("Input SHM sizes mismatch: in1=%d in2=%d\n", nb1, nb2);
        return DAO_ERROR;
    }
    if (nbOut != nb1)
    {
        daoError("Output SHM size mismatch: out=%d but expected %d\n", nbOut, nb1);
        return DAO_ERROR;
    }

    gNbVal = nb1;

    /* sums float buffers */
    if (gShmIn[0][0].md[0].atype != _DATATYPE_FLOAT || gShmIn[1][0].md[0].atype != _DATATYPE_FLOAT || gShmOut[0].md[0].atype != _DATATYPE_FLOAT)
    {
        daoError("the 3 SHMs must be float\n");
        return DAO_ERROR;
    }

    return DAO_SUCCESS;
}

/*==========================================================================*/
static void addFloatBuffers(const float *a, const float *b, float *out, int n)
{
    int i;
    for (i = 0; i < n; i++)
        out[i] = a[i] + b[i];
}

/*==========================================================================*/
static void *shmThreadLoop(void *thread_data)
{
    struct arg_struct *args = (struct arg_struct *)thread_data;
    int shmId = args->shmId; /* 0 or 1 */

    daoInfo("Thread shm%d ENTERING LOOP\n", shmId + 1);

    while (!gEnd)
    {
        if (daoToolsWait(gShmIn[shmId], DAO_SEM_AUTO, 1.0) != DAO_SUCCESS)
        {
            if (shmId == (gMasterChannel == 2 ? 1 : 0))   /* one thread reports the waits */
            {
                pthread_mutex_lock(&gOutMutex);
                daoToolsLoopStatusWait(&gStatus);
                pthread_mutex_unlock(&gOutMutex);
            }
            continue;
        }

        pthread_mutex_lock(&gOutMutex);
        daoToolsLoopStatusStart(&gStatus);

        /* Recompute out using latest in1 and in2 */
        addFloatBuffers((const float *)gShmIn[0][0].array.F,
                        (const float *)gShmIn[1][0].array.F,
                        (float *)gShmOut[0].array.F,
                        gNbVal);

        /* cnt2: align with the event that triggered this compute */
        gShmOut[0].md[0].cnt2 = gShmIn[shmId][0].md[0].cnt2;

        if (shouldFinalize(shmId))
            daoShmSetDataPartFinalize(&gShmOut[0]);

        daoToolsLoopStatusEnd(&gStatus, NULL);
        pthread_mutex_unlock(&gOutMutex);
    }

    daoInfo("Thread shm%d EXITING LOOP\n", shmId + 1);
    return NULL;
}

/*==========================================================================*/
static int prepRealTime(void)
{
    int i;
    int threadVal[NB_SHM];
    struct arg_struct args[NB_SHM];

    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    gShmOut   = (IMAGE *)malloc(sizeof(IMAGE));
    gShmIn[0] = (IMAGE *)malloc(sizeof(IMAGE));
    gShmIn[1] = (IMAGE *)malloc(sizeof(IMAGE));
    if (!gShmOut || !gShmIn[0] || !gShmIn[1])
    {
        daoError("malloc failed\n");
        return DAO_ERROR;
    }

    daoToolsShmOpen(gShmOutName, &gShmOut[0]);
    daoInfo("Connected OUT: %s\n", gShmOutName);

    daoToolsShmOpen(gShmInName[0], &gShmIn[0][0]);
    daoInfo("Connected IN1: %s\n", gShmInName[0]);

    daoToolsShmOpen(gShmInName[1], &gShmIn[1][0]);
    daoInfo("Connected IN2: %s\n", gShmInName[1]);

    if (validateSizesAndTypes() == DAO_ERROR)
        return DAO_ERROR;

    daoInfo("Finalize policy: %s\n",
        (gMasterChannel == -1) ? "ANY input update" : (gMasterChannel == 1) ? "ONLY shm1 updates"
            : (gMasterChannel == 2)                                         ? "ONLY shm2 updates"
                                                                            : "invalid");
    if (gMasterChannel != -1 && gMasterChannel != 1 && gMasterChannel != 2)
    {
        daoError("-m: 1, 2 or -1 (any input)\n");
        return DAO_ERROR;
    }
    daoToolsLoopStatusInit(&gStatus);

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

    pthread_join(gThread[0], NULL);
    pthread_join(gThread[1], NULL);

    printf("\n");
    daoToolsShmRelease(&gShmOut);
    daoToolsShmRelease(&gShmIn[0]);
    daoToolsShmRelease(&gShmIn[1]);
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
                daoLogLevel = daoToolsArgInt(&argc, &argv, str);
                break;

            case 'm':
                gMasterChannel = daoToolsArgInt(&argc, &argv, str);
                if (!(gMasterChannel == -1 || gMasterChannel == 1 || gMasterChannel == 2))
                {
                    daoWarning("Invalid -m %d (expected 1 or 2). Using default (-1).\n", gMasterChannel);
                    gMasterChannel = -1;
                }
                break;

            case 'S':
                daoToolsArgNameNext(&argc, &argv, str, gShmOutName, sizeof gShmOutName);
                daoToolsArgNameNext(&argc, &argv, str, gShmInName[0], sizeof gShmInName[0]);
                daoToolsArgNameNext(&argc, &argv, str, gShmInName[1], sizeof gShmInName[1]);
                daoInfo("OUT : %s\n", gShmOutName);
                daoInfo("IN1 : %s\n", gShmInName[0]);
                daoInfo("IN2 : %s\n", gShmInName[1]);
                break;

            case 'L':
                daoInfo("SHM Add real-time loop\n");
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
    /* Try to go RT (OK if it fails due to perms) */
    daoToolsSetRtPriority(93);

    sArgv0 = *argv;
    if (argc < 2)
    {                    /* nothing to do: say how */
        ShowHelp();
        return 1;
    }
    DecodeArgs(argc, argv);

    return sExit;
}
/*==========================================================================*/
