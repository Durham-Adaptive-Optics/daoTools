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
    printf("   -S <SHM> -n <nb Average> [-s <semNb>] -L\n");
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

    if (shm[0].md[0].atype != _DATATYPE_DOUBLE)
    {
        daoError("%s is not double: daoAvgDoubleShm averages double SHMs (daoAvgShm: float)\n", shmName);
        exit(EXIT_FAILURE);
    }
    if (nbAvg < 1)
    {
        daoError("-n: at least 1 frame to average\n");
        exit(EXIT_FAILURE);
    }
    int nbValue = shm[0].md[0].size[0]*shm[0].md[0].size[1];
    double *avgValue = shmAvg[0].array.D;
    // the last nbAvg frames (each divided by nbAvg), and room for the next one:
    // on the heap, as it can be large (value k of slot l at [k * (nbAvg + 1) + l])
    double *valueCircBuf = calloc((size_t)nbValue * (nbAvg + 1), sizeof(double));
    if (valueCircBuf == NULL)
    {
        daoError("cannot allocate the average buffer\n");
        exit(EXIT_FAILURE);
    }
    int k;
    for (k = 0; k < nbValue; k++)
    {
        avgValue[k] = 0.0;
    }
    int tail=0;
    int head=0;
    printf("Average telemetry running for %s -> %s, nAvg=%d\n",shmName, shmNameAvg, nbAvg );
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(shm, semNb, 1.0) == DAO_SUCCESS)
        {
            daoToolsLoopStatusStart(&status);
            // if new image, add it in the cir buf.
            for (k=0; k<nbValue; k++)
            {
                double *slot = &valueCircBuf[(size_t)k * (nbAvg + 1) + tail];
                *slot = shm[0].array.D[k] / nbAvg;
                if (isnan(*slot))
                {
                    *slot = 0.0;
                }
            }
            tail = (tail + 1) % (nbAvg + 1);
            for (k=0; k<nbValue; k++)
            {
                // add the new value (head), remove the one leaving the window (tail)
                avgValue[k] += valueCircBuf[(size_t)k * (nbAvg + 1) + head];
                avgValue[k] -= valueCircBuf[(size_t)k * (nbAvg + 1) + tail];
            }
            head = (head + 1) % (nbAvg + 1);

            daoShmSetDataPartFinalize(&shmAvg[0]);
            daoToolsLoopStatusEnd(&status, NULL);
        }
        else
        {
            daoToolsLoopStatusWait(&status);
        }
    }
    free(valueCircBuf);

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
