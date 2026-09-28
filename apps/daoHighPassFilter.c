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

char inShmName[DAO_SHM_NAME_LEN];
char outShmName[DAO_SHM_NAME_LEN];
char fcShmName[DAO_SHM_NAME_LEN];
char fpsShmName[DAO_SHM_NAME_LEN];
char enaShmName[DAO_SHM_NAME_LEN];
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
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   daoHighPassFilter -S <in SHM> <out SHM> <fc SHM> <fps SHM> <ena SHM> [-s <semNb>] -L\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    uint32_t size[2];
    IMAGE *inShm;
    IMAGE *inShmPrev;
    IMAGE *outShm;
    IMAGE *outShmPrev;

    IMAGE *fcShm;
    IMAGE *fpsShm;
    IMAGE *enaShm;

    fcShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(fcShmName, &fcShm[0]);
    fpsShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(fpsShmName, &fpsShm[0]);
    enaShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(enaShmName, &enaShm[0]);

    char inShmNamePrev[DAO_SHM_NAME_LEN];
    char outShmNamePrev[DAO_SHM_NAME_LEN];

    inShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(inShmName, &inShm[0]);
    // Create Prev SHM
    inShmPrev = (IMAGE*) malloc(sizeof(IMAGE));
    if (daoToolsInsertShmNamePrefixN(inShmName, "Prev", inShmNamePrev, sizeof inShmNamePrev) != DAO_SUCCESS)
        exit(EXIT_FAILURE);
    size[0] = inShm[0].md[0].size[0];
    size[1] = inShm[0].md[0].size[1];
    daoShmCreate(inShmPrev, inShmNamePrev, 2, size, inShm[0].md[0].atype, 1, 0);

    outShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(outShmName, &outShm[0]);
    // Create Prev SHM
    outShmPrev = (IMAGE*) malloc(sizeof(IMAGE));
    if (daoToolsInsertShmNamePrefixN(outShmName, "Prev", outShmNamePrev, sizeof outShmNamePrev) != DAO_SUCCESS)
        exit(EXIT_FAILURE);
    size[0] = inShm[0].md[0].size[0];
    size[1] = inShm[0].md[0].size[1];
    daoShmCreate(outShmPrev, inShmNamePrev, 2, size, inShm[0].md[0].atype, 1, 0);

    printf("Starting loop, (%s,%s) -> (%s,%s)\n",
           inShmName, inShmNamePrev, outShmName, outShmNamePrev);

    fflush(stdout);

    int inSize = inShm[0].md[0].size[0]*inShm[0].md[0].size[1];

    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(inShm, semNb, 1.0) == DAO_SUCCESS)
        {
            daoToolsLoopStatusStart(&status);

            if (enaShm[0].array.UI32[0] == 1)
            {
                if (inShm[0].md[0].atype == _DATATYPE_DOUBLE)
                {
                    daoToolsHighPassFilterDouble(outShm[0].array.D,
                                                 inShm[0].array.D,  
                                                 outShmPrev[0].array.D,  
                                                 inShmPrev[0].array.D,  
                                                 fcShm[0].array.D[0],
                                                 fpsShm[0].array.D[0],
                                                 inSize);
                }
                else
                {
                    daoToolsHighPassFilter(outShm[0].array.F,
                                           inShm[0].array.F,  
                                           outShmPrev[0].array.F,  
                                           inShmPrev[0].array.F,  
                                           fcShm[0].array.F[0],
                                           fpsShm[0].array.F[0],
                                           inSize);
                }
            }
            else
            {
                if (inShm[0].md[0].atype == _DATATYPE_DOUBLE)
                {
                    daoShmSetData(outShm, inShm[0].array.D, inSize);
                }
                else
                {
                    daoShmSetData(outShm, inShm[0].array.F, inSize);
                }
            }

            daoShmSetDataPartFinalize(&outShm[0]);
            
            // After the call, update previous frame buffers
            for (int i = 0; i < inSize; ++i) 
            {
                outShmPrev[0].array.D[i] = outShm[0].array.D[i];
                inShmPrev[0].array.D[i] = inShm[0].array.D[i];
            }
            daoToolsLoopStatusEnd(&status, ", high-pass %s", enaShm[0].array.UI32[0] ? "on" : "off");
        }
        else
        {
            daoToolsLoopStatusWait(&status);
        }
    }
    printf("\n");


    daoInfo("EXITING MAIN LOOP\n");


    daoToolsShmRelease(&fcShm);
    daoToolsShmRelease(&fpsShm);
    daoToolsShmRelease(&enaShm);
    daoToolsShmRelease(&inShm);
    daoToolsShmRelease(&inShmPrev);
    daoToolsShmRelease(&outShm);
    daoToolsShmRelease(&outShmPrev);
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
                daoToolsArgNameNext(&argc, &argv, str, inShmName, sizeof inShmName);
                daoToolsArgNameNext(&argc, &argv, str, outShmName, sizeof outShmName);
                daoToolsArgNameNext(&argc, &argv, str, fcShmName, sizeof fcShmName);
                daoToolsArgNameNext(&argc, &argv, str, fpsShmName, sizeof fpsShmName);
                daoToolsArgNameNext(&argc, &argv, str, enaShmName, sizeof enaShmName);
                daoInfo("inShmName          = %s\n", inShmName);
                daoInfo("outShmName         = %s\n", outShmName);
                daoInfo("fcShmName          = %s\n", fcShmName);
                daoInfo("fpsShmName         = %s\n", fpsShmName);
                daoInfo("enaShmName         = %s\n", enaShmName);
                break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                daoInfo("inputShm sem       = %d \n", semNb);
                break;
            case 'L':
                        printf("HPF real time control\n");
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
