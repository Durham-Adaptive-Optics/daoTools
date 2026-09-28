/*****************************************************************************
  Durham AO project
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

IMAGE *outShm;
IMAGE *clockShm;

char outShmName[DAO_SHM_NAME_LEN];
char clockShmName[DAO_SHM_NAME_LEN];
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
    printf("   -S <out SHM> <clock SHM>  write random values to <out SHM> at each frame of <clock SHM>\n");
    printf("   -s <semNb>       a fixed semaphore to wait on (default: one of its own)\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   -S <out SHM> <clock SHM> [-s <semNb>] -L\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session
    daoInfo("Starting loop, %s \n", outShmName);
    outShm = (IMAGE*) malloc(sizeof(IMAGE));
    clockShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(outShmName, &outShm[0]);
    daoToolsShmOpen(clockShmName, &clockShm[0]);
    int outSize = outShm[0].md[0].size[0]*outShm[0].md[0].size[1];
    daoToolsShmCheck(outShm, outShmName, _DATATYPE_FLOAT, outSize);
    float *outCmd = malloc((size_t)outSize * sizeof(float));
    if (outCmd == NULL)
    {
        daoError("cannot allocate %d values\n", outSize);
        exit(EXIT_FAILURE);
    }
    int k;
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        // one frame per frame of the clock SHM
        if (daoToolsWait(clockShm, semNb, 1.0) == DAO_SUCCESS)
        {
            daoToolsLoopStatusStart(&status);
            outShm[0].md[0].cnt2 = clockShm[0].md[0].cnt2;
            for (k = 0; k < outSize; k++)
                outCmd[k] = 1 - 2 * (float)rand() / (float)RAND_MAX;
            daoShmSetData(&outShm[0], outCmd, outSize);
            daoToolsLoopStatusEnd(&status, NULL);
        }
        else
        {
            daoToolsLoopStatusWait(&status);
        }
    }
    free(outCmd);
    printf("\n");
    daoInfo("EXITING MAIN LOOP\n");
    daoToolsShmRelease(&outShm);
    daoToolsShmRelease(&clockShm);
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
                        daoInfo("Simple writer from SHM real time control, listening to a SHM clock\n");
                        daoToolsArgNameNext(&argc, &argv, str, outShmName, sizeof outShmName);
                        daoToolsArgNameNext(&argc, &argv, str, clockShmName, sizeof clockShmName);
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

