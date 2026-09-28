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
char offsetShmName[DAO_SHM_NAME_LEN];
char outShmName[DAO_SHM_NAME_LEN];
char lpCmdShmName[DAO_SHM_NAME_LEN];
char gainShmName[DAO_SHM_NAME_LEN];
char leakyShmName[DAO_SHM_NAME_LEN];
char enableShmName[DAO_SHM_NAME_LEN];
int enableGiven = 0;   /* -e was passed explicitly: use enableShmName as-is, don't derive it */
int modal=0; // modal integrator flag
int keepPiston=0; // -P: do not remove the mean of the output (e.g. a tip/tilt mirror)
double clipping = 10.0; // clipping value


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
    printf("   -m               modal integrator, leaky and gain should be arrays\n");
    printf("   -c <clipping>    clip the output to [-clipping, clipping] (default 10)\n");
    printf("   -P               keep the piston: do not remove the mean of the output\n");
    printf("                    (removed by default; a 2-axis tip/tilt command needs -P)\n");
    printf("   -e <shm>         optional enable shm (default: derived from <loopCmd> as\n");
    printf("                    <loopCmd base name>Enable.im.shm, e.g. lpCmd.im.shm ->\n");
    printf("                    lpCmdEnable.im.shm)\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   -S <in SHM> <offset SHM> <out SHM> <loopCmd SHM> <leak SHM> <gain SHM> [-s <semNb>] [-c <clipping>] [-e <enableShm>] [-m] [-P] -L\n");
    printf("\n");
    printf("   -e lets an external SHM enable/disable the integrator independently of\n");
    printf("   the loopCmd open/close state: when the enable SHM reads 0, the output is\n");
    printf("   zeroed and published once, then the out SHM semaphores are no longer\n");
    printf("   posted until enable returns to 1, regardless of loopCmd.\n");
    printf("   If the enable SHM does not exist yet it is created here with value 1\n");
    printf("   (enabled), so default behaviour is unchanged whether -e is used or not.\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
/* Force the output frame to 0. The output array is also the integrator's
 * state (see daoToolsLeakyIntegrator), so this doubles as a state reset. */
static void zeroOutput(IMAGE *outShm, int atype, int size)
{
    int j;
    if (atype == _DATATYPE_FLOAT)
    {
        for (j = 0; j < size; j++)
        {
            outShm[0].array.F[j] = 0.0;
        }
    }
    else
    {
        for (j = 0; j < size; j++)
        {
            outShm[0].array.D[j] = 0.0;
        }
    }
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    daoInfo("Modal integrator: %s\n", modal ? "yes" : "no");
    daoInfo("Starting loop, %s -> %s \n", inShmName, outShmName);
    fflush(stdout);
    IMAGE *inShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *offsetShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *outShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *lpCmdShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *gainShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *leakyShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *enableShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(inShmName, &inShm[0]);
    daoToolsShmOpen(offsetShmName, &offsetShm[0]);
    daoToolsShmOpen(outShmName, &outShm[0]);
    daoToolsShmOpen(lpCmdShmName, &lpCmdShm[0]);
    daoToolsShmOpen(gainShmName, &gainShm[0]);
    daoToolsShmOpen(leakyShmName, &leakyShm[0]);

    // Enable shm: -e overrides, otherwise derive <loopCmd base>Enable.im.shm
    // from the loopCmd shm's name (same helper used for daoTimeDiff's
    // Avg/Rms/Array sibling shms). Attach if it already exists; if not,
    // create it here with value 1 (enabled) so existing setups that never
    // heard of -e keep behaving exactly as before.
    if (!enableGiven)
    {
        if (daoToolsInsertShmNamePrefixN(lpCmdShmName, "Enable", enableShmName, sizeof enableShmName) != DAO_SUCCESS)
            exit(EXIT_FAILURE);
    }
    daoInfo("enableShmName    = %s\n", enableShmName);
    if (daoShmOpen(enableShmName, &enableShm[0]) != DAO_SUCCESS)
    {
        uint32_t enableSize[2] = {1, 1};
        daoInfo("enable shm not found, creating %s with enable=1\n", enableShmName);
        daoShmCreate(enableShm, enableShmName, 2, enableSize, _DATATYPE_UINT32, 1, 0);
        enableShm[0].array.UI32[0] = 1;
    }

    int inSize = inShm[0].md[0].size[0]*inShm[0].md[0].size[1];
    struct timespec t[3];
    clock_gettime(CLOCK_REALTIME, &t[1]);
    int j;
    float avg=0;
    /* enable==0: publish a single zeroed frame, then go quiet (stop
     * finalizing) until enable goes back to 1. */
    int zeroSent = 0;
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(inShm, semNb, 1.0) == DAO_SUCCESS)
        {
            daoToolsLoopStatusStart(&status);
            // New image, insert something here
            outShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
            avg=0;
            int enabled = (enableShm[0].array.UI32[0] == 1);
            int publish = 1;   /* finalize (post the out shm semaphores) this frame? */
            if (!enabled)
            {
                /* Disabled: leave consumers on a known zeroed frame, published
                 * exactly once, then stop posting entirely so nothing
                 * downstream is woken while we are off. */
                if (zeroSent)
                {
                    publish = 0;
                }
                else
                {
                    zeroOutput(outShm, inShm[0].md[0].atype, inSize);
                    zeroSent = 1;
                }
            }
            else if (lpCmdShm[0].array.UI32[0] == 1)
            {
                zeroSent = 0;
                if (inShm[0].md[0].atype == _DATATYPE_FLOAT)
                {
                    if (modal == 0)
                    {
                        daoToolsLeakyIntegrator(inShm[0].array.F, 
                                                inSize,
                                                leakyShm[0].array.F[0], 
                                                gainShm[0].array.F[0], 
                                                offsetShm[0].array.F,
                                                outShm[0].array.F);
                    }
                    else
                    {
                        daoToolsLeakyModalIntegrator(inShm[0].array.F, 
                                                inSize,
                                                leakyShm[0].array.F, 
                                                gainShm[0].array.F, 
                                                offsetShm[0].array.F,
                                                outShm[0].array.F);
                    }
                    for (j=0; j< inSize; j++)
                    {
                        avg+=outShm[0].array.F[j];
                    }
                    avg=avg/inSize;
                    if (keepPiston)
                        avg = 0;
                    for (j=0; j< inSize; j++)
                    {
                        outShm[0].array.F[j] = outShm[0].array.F[j] - avg;
                        if (outShm[0].array.F[j] > clipping)
                        {
                            outShm[0].array.F[j] = clipping;
                        }
                        else if (outShm[0].array.F[j] < -clipping)
                        {
                            outShm[0].array.F[j] = -clipping;
                        }
                    }
                }
                else
                {
                    if (modal == 0)
                    {
                        daoToolsLeakyIntegratorDouble(inShm[0].array.D, 
                                                  inSize,
                                                  leakyShm[0].array.D[0], 
                                                  gainShm[0].array.D[0], 
                                                  offsetShm[0].array.D,
                                                  outShm[0].array.D);
                    }
                    else
                    {
                        daoToolsLeakyModalIntegratorDouble(inShm[0].array.D, 
                                                  inSize,
                                                  leakyShm[0].array.D, 
                                                  gainShm[0].array.D, 
                                                  offsetShm[0].array.D,
                                                  outShm[0].array.D);
                    }
                    for (j=0; j< inSize; j++)
                    {
                        avg+=outShm[0].array.D[j];
                    }
                    avg=avg/inSize;
                    if (keepPiston)
                        avg = 0;
                    for (j=0; j< inSize; j++)
                    {
                        outShm[0].array.D[j] = outShm[0].array.D[j] - avg;
                        if (outShm[0].array.D[j] > clipping)
                        {
                            outShm[0].array.D[j] = clipping;
                        }
                        else if (outShm[0].array.D[j] < -clipping)
                        {
                            outShm[0].array.D[j] = -clipping;
                        }
                    }                    
                }
            }
            else
            {
                /* Open loop but still enabled: keep publishing zeros every
                 * frame, as before. */
                zeroSent = 0;
                zeroOutput(outShm, inShm[0].md[0].atype, inSize);
            }
            if (publish)
            {
                daoShmSetDataPartFinalize(&outShm[0]);
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
    daoToolsShmRelease(&offsetShm);
    daoToolsShmRelease(&outShm);
    daoToolsShmRelease(&lpCmdShm);
    daoToolsShmRelease(&gainShm);
    daoToolsShmRelease(&leakyShm);
    daoToolsShmRelease(&enableShm);
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
            case 'c':
                clipping = daoToolsArgDouble(&argc, &argv, str);
                daoInfo("clipping value set to %f\n", clipping);
                break;
            case 'm':	
                        modal = 1;
                        break;
            case 'P':
                        keepPiston = 1;
                        daoInfo("piston kept (no mean removal)\n");
                        break;
            case 'S':
                        daoInfo("Simple filter from SHM real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, inShmName, sizeof inShmName);
                        daoToolsArgNameNext(&argc, &argv, str, offsetShmName, sizeof offsetShmName);
                        daoToolsArgNameNext(&argc, &argv, str, outShmName, sizeof outShmName);
                        daoToolsArgNameNext(&argc, &argv, str, lpCmdShmName, sizeof lpCmdShmName);
                        daoToolsArgNameNext(&argc, &argv, str, gainShmName, sizeof gainShmName);
                        daoToolsArgNameNext(&argc, &argv, str, leakyShmName, sizeof leakyShmName);
                        daoInfo("inShmName      = %s\n", inShmName);
                        daoInfo("offsetShmName  = %s\n", offsetShmName);
                        daoInfo("outShmName     = %s\n", outShmName);
                        daoInfo("lpCmdShmName   = %s\n", lpCmdShmName);
                        daoInfo("gainShmName    = %s\n", gainShmName);
                        daoInfo("leakyShmName   = %s\n", leakyShmName);
                        break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                daoInfo("inputShm sem     : %d \n", semNb);
                break;
            case 'e':
                daoToolsArgNameNext(&argc, &argv, str, enableShmName, sizeof enableShmName);
                enableGiven = 1;
                daoInfo("enableShmName (given) = %s\n", enableShmName);
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

