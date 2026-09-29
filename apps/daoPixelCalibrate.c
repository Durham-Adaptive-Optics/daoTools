/*****************************************************************************
  DAO project
  s.cetre
 *****************************************************************************/

/*==========================================================================*/
#define _GNU_SOURCE
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


char inShmName[DAO_SHM_NAME_LEN];
int semNb = DAO_SEM_AUTO;   // -s: a fixed semaphore; default: one of its own
char ffShmName[DAO_SHM_NAME_LEN];
char bgShmName[DAO_SHM_NAME_LEN];
char calShmName[DAO_SHM_NAME_LEN];

static int   		end     = 0;		           // termination flag
// termination function for SIGINT callback
static void endme(int _a)
{
    (void)_a;
    end = 1;
}

/*--------------------------------------------------------------------------*/
/* Real-time tuning knobs - same pattern already used/validated in daoMvM.c */
static int rtCpu = -1;           /* CPU core to pin the RT thread to (-1 = do not pin) */

static void calSetRtAffinity(int cpu)
{
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
        daoError("pthread_setaffinity_np(cpu=%d) failed\n", cpu);
    else
        daoInfo("RT thread pinned to CPU %d\n", cpu);
#else
    daoInfo("CPU affinity not supported on this platform (cpu=%d ignored)\n", cpu);
#endif
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
    printf("   -C <cpu>         pin the real-time thread to CPU core <cpu>\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage (options must precede -L):\n");
    printf("   -S <input SHM> <flatfield SHM> <background SHM> <output SHM> [-s <semNb>] [-C <cpu>] -L\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    if (rtCpu >= 0)
        calSetRtAffinity(rtCpu);

    IMAGE *inShm;
    IMAGE *ffShm;
    IMAGE *bgShm;
    IMAGE *calShm;

    inShm = (IMAGE*) malloc(sizeof(IMAGE));
    ffShm = (IMAGE*) malloc(sizeof(IMAGE));
    bgShm = (IMAGE*) malloc(sizeof(IMAGE));
    calShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(inShmName, &inShm[0]);
    daoToolsShmOpen(ffShmName, &ffShm[0]);
    daoToolsShmOpen(bgShmName, &bgShm[0]);
    daoToolsShmOpen(calShmName, &calShm[0]);

    daoInfo("Starting loop, %s/%s \n",inShmName, calShmName);
    fflush(stdout);
    usleep(2000000);

    // Fault in the in/ff/bg/cal buffers and run the calibration once so the
    // first real frame doesn't pay page-fault / lazy-init jitter (same
    // reasoning as daoMvM's matrix fault-in before its loop).
    if (calShm[0].md[0].atype == _DATATYPE_FLOAT)
        daoToolsShmCalibrate(inShm, ffShm, bgShm, calShm);
    else
        daoToolsShmCalibrate64(inShm, ffShm, bgShm, calShm);


    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(inShm, semNb, 1.0) == DAO_SUCCESS)
        {
            daoToolsLoopStatusStart(&status);
            if (calShm[0].md[0].atype == _DATATYPE_FLOAT)
            {
                daoToolsShmCalibrate(inShm, ffShm, bgShm, calShm);
            }
            else
            {
                daoToolsShmCalibrate64(inShm, ffShm, bgShm, calShm);
            }

            daoToolsLoopStatusEnd(&status, NULL);
        }
        else
        {
            daoToolsLoopStatusWait(&status);
        }
    }
    printf("\n");


    daoInfo("EXITING MAIN LOOP\n");


    daoToolsShmRelease(&inShm);
    daoToolsShmRelease(&ffShm);
    daoToolsShmRelease(&bgShm);
    daoToolsShmRelease(&calShm);
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
                break;
            case 'S':
                daoToolsArgNameNext(&argc, &argv, str, inShmName, sizeof inShmName);
                daoToolsArgNameNext(&argc, &argv, str, ffShmName, sizeof ffShmName);
                daoToolsArgNameNext(&argc, &argv, str, bgShmName, sizeof bgShmName);
                daoToolsArgNameNext(&argc, &argv, str, calShmName, sizeof calShmName);
                daoInfo("image in         : %s\n", inShmName);
                daoInfo("flat field       : %s\n", ffShmName);
                daoInfo("background       : %s\n", bgShmName);
                daoInfo("calibrated image : %s\n", calShmName);
                break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                daoInfo("inputShm sem     : %d \n", semNb);
                break;
            case 'C':
                rtCpu = daoToolsArgInt(&argc, &argv, str);
                daoInfo("RT thread CPU    : %d \n", rtCpu);
                break;
            case 'L':
                        daoInfo("Apply Falt and Background from SHM real time control\n");
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

    // Lock the address space in RAM: a page fault inside the loop is unbounded jitter.
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        daoWarning("mlockall failed: run scripts/daoToolSetCap to grant RT capabilities. Continuing, but not optimized for real-time.\n");

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

