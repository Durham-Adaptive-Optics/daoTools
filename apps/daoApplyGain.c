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
char outShmName[DAO_SHM_NAME_LEN];
char gainShmName[DAO_SHM_NAME_LEN];
int modal=0; // modal integrator flag

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
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   -S <in SHM> <gain SHM> <out SHM> [-s <semNb>] -m -L\n");
    printf("\n");
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
    IMAGE *outShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *gainShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(inShmName, &inShm[0]);
    daoToolsShmOpen(outShmName, &outShm[0]);
    daoToolsShmOpen(gainShmName, &gainShm[0]);

    int inSize = inShm[0].md[0].size[0]*inShm[0].md[0].size[1];
    int outSize = outShm[0].md[0].size[0]*outShm[0].md[0].size[1];
    int gainSize = gainShm[0].md[0].size[0] * gainShm[0].md[0].size[1];
    uint8_t atype = inShm[0].md[0].atype;
    if ((atype != _DATATYPE_FLOAT && atype != _DATATYPE_DOUBLE) || outShm[0].md[0].atype != atype || gainShm[0].md[0].atype != atype)
    {
        daoError("the 3 SHMs must be all float or all double\n");
        exit(EXIT_FAILURE);
    }
    if (outSize < inSize || gainSize < (modal ? inSize : 1))
    {
        daoError("%s: %d values, for %d in (gain: %d, needs %d)\n",
            outShmName, outSize, inSize, gainSize, modal ? inSize : 1);
        exit(EXIT_FAILURE);
    }
    int k=0;
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(inShm, semNb, 1.0) == DAO_SUCCESS)
        {
            daoToolsLoopStatusStart(&status);
            outShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
            if (inShm[0].md[0].atype == _DATATYPE_FLOAT)
            {
                if (modal == 0)
                {
                    for (k=0; k< inSize; k++)
                    {
                        outShm[0].array.F[k] = inShm[0].array.F[k] * gainShm[0].array.F[0];
                    }
                }
                else
                {
                    for (k=0; k< inSize; k++)
                    {
                        outShm[0].array.F[k] = inShm[0].array.F[k] * gainShm[0].array.F[k];
                    }
                }
            }
            else
            {
                if (modal == 0)
                {
                    for (k=0; k< inSize; k++)
                    {
                        outShm[0].array.D[k] = inShm[0].array.D[k] * gainShm[0].array.D[0];
                    }
                }
                else
                {
                    for (k=0; k< inSize; k++)
                    {
                        outShm[0].array.D[k] = inShm[0].array.D[k] * gainShm[0].array.D[k];
                    }
                }
            }

            daoShmSetDataPartFinalize(&outShm[0]);
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
    daoToolsShmRelease(&outShm);
    daoToolsShmRelease(&gainShm);
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
            case 'm':	
                        modal = 1;
                        break;
            case 'S':
                        daoInfo("Simple gain application from SHM real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, inShmName, sizeof inShmName);
                        daoToolsArgNameNext(&argc, &argv, str, gainShmName, sizeof gainShmName);
                        daoToolsArgNameNext(&argc, &argv, str, outShmName, sizeof outShmName);
                        daoInfo("inShmName      = %s\n", inShmName);
                        daoInfo("gainShmName    = %s\n", gainShmName);
                        daoInfo("outShmName     = %s\n", outShmName);
                        break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                daoInfo("inputShm sem     : %d \n", semNb);
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

