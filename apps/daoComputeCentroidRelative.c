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

char inShmName[DAO_SHM_NAME_LEN];
char refShmName[DAO_SHM_NAME_LEN];
char centroidShmName[DAO_SHM_NAME_LEN];
char thresholdShmName[DAO_SHM_NAME_LEN];
char minFluxShmName[DAO_SHM_NAME_LEN] = "";   /* -f: optional minimum flux per subaperture */

/* The minimum flux (-f), read every frame so it can be tuned live; without -f: 0, no minimum. */
static double daoMinFlux(IMAGE *shm)
{
    if (shm == NULL)
        return 0.0;
    return shm[0].md[0].atype == _DATATYPE_DOUBLE ? shm[0].array.D[0] : shm[0].array.F[0];
}
int subaSize;
int nbSuba;
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
    printf("   -f <shm>         minimum flux SHM (1 value): a subaperture with less light (the sum\n");
    printf("                    of its raw pixels) gets slopes (0, 0), no noise; without -f: no minimum\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   -S <in SHM> <centroid SHM> <ref SHM> <threshold SHM> <subaSize> <nbSuba> [-s <semNb>] -L\n");
    printf("\n");
}



/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    daoInfo("Starting loop, %s/%s/%s/%s \n", inShmName, refShmName, centroidShmName, thresholdShmName);
    fflush(stdout);
    IMAGE *inShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *centroidShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *refShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *thresholdShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(inShmName, &inShm[0]);
    daoToolsShmOpen(centroidShmName, &centroidShm[0]);
    daoToolsShmOpen(refShmName, &refShm[0]);
    daoToolsShmOpen(thresholdShmName, &thresholdShm[0]);
    IMAGE *minFluxShm = NULL;
    if (minFluxShmName[0] != '\0') {
        minFluxShm = (IMAGE*) malloc(sizeof(IMAGE));
        daoToolsShmOpen(minFluxShmName, &minFluxShm[0]);
    }

    int inSize = inShm[0].md[0].size[0]*inShm[0].md[0].size[1];
    daoToolsShmCheck(inShm, inShmName, _DATATYPE_FLOAT, inSize);
    daoToolsShmCheck(thresholdShm, thresholdShmName, _DATATYPE_FLOAT, 1);
    daoToolsShmCheck(refShm, refShmName, _DATATYPE_FLOAT, 2L * nbSuba);             // x then y of each sub-aperture
    daoToolsShmCheck(centroidShm, centroidShmName, _DATATYPE_FLOAT, 4L * nbSuba);   // x, y, flux, weight
    usleep(2000000);
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(inShm, semNb, 1.0) == DAO_SUCCESS)
        {
            daoToolsLoopStatusStart(&status);
            // New image, insert something here
            centroidShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;

            //usleep(10000);
            daoCentroidSpotsRelative(inShm[0].array.F,
                             inShm[0].md[0].size[1],
                             inShm[0].md[0].size[0],
                             refShm[0].array.F,
                             subaSize,
                             nbSuba,
                             thresholdShm[0].array.F[0],
                             (float)daoMinFlux(minFluxShm),
                             centroidShm[0].array.F); 
            daoShmSetDataPartFinalize(&centroidShm[0]);

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
    daoToolsShmRelease(&centroidShm);
    daoToolsShmRelease(&refShm);
    daoToolsShmRelease(&thresholdShm);
    if (minFluxShm)
        daoToolsShmRelease(&minFluxShm);
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
                break;
            case 'S':
                        daoInfo("Simple filter from SHM real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, inShmName, sizeof inShmName);
                        daoToolsArgNameNext(&argc, &argv, str, centroidShmName, sizeof centroidShmName);
                        daoToolsArgNameNext(&argc, &argv, str, refShmName, sizeof refShmName);
                        daoToolsArgNameNext(&argc, &argv, str, thresholdShmName, sizeof thresholdShmName);
                        subaSize = daoToolsArgInt(&argc, &argv, str);
                        nbSuba = daoToolsArgInt(&argc, &argv, str);
                        daoInfo("inShmName = %s\n", inShmName);
                        daoInfo("centroidShmName = %s\n", centroidShmName);
                        daoInfo("refShmName = %s\n", refShmName);
                        daoInfo("thresholdShmName = %s\n", thresholdShmName);
                        daoInfo("subaSize = %d\n", subaSize);
                        daoInfo("nbSUba = %d\n", nbSuba);
                        break;
            case 'f':
                daoToolsArgNameNext(&argc, &argv, str, minFluxShmName, sizeof minFluxShmName);
                daoInfo("minFluxShmName     = %s\n", minFluxShmName);
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

