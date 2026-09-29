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

char imShmName[DAO_SHM_NAME_LEN];
char intensityShmName[DAO_SHM_NAME_LEN];
char pixIdShmName[DAO_SHM_NAME_LEN];
char validPixShmName[DAO_SHM_NAME_LEN];
char validSubPixShmName[DAO_SHM_NAME_LEN];
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
    printf("   -S <in SHM> <intensity SHM> <valid Pix SHM> <validSubaPix> [-s <semNb>] -L\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    daoInfo("Starting loop, %s -> %s \n", imShmName, intensityShmName);
    IMAGE *imShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *intensityShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *validPixShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *validSubPixShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(imShmName, &imShm[0]);
    daoToolsShmOpen(intensityShmName, &intensityShm[0]);
    daoToolsShmOpen(validPixShmName, &validPixShm[0]);
    daoToolsShmOpen(validSubPixShmName, &validSubPixShm[0]);

    int validPixSize = validPixShm[0].md[0].size[0] * validPixShm[0].md[0].size[1];

    int i;
    // Compute number of valid pixels
    int validPixSum = 0;
    for (i = 0; i < validPixSize; i++)
    {
        if (validPixShm[0].array.UI32[i] == 1)
        {
            validPixSum++;
        }
    }
    daoInfo("Detected %d valid pixels\n", validPixSum);

    // Create a LUT
    int lut[validPixSum];
    int k=0;
    for (i = 0; i < validPixSize; i++)
    {
        if (validPixShm[0].array.UI32[i] == 1)
        {
            lut[k] = i;
            k++;
        }
    }
    float sum = 0;

    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(imShm, semNb, 1.0) == DAO_SUCCESS)
        {
            daoToolsLoopStatusStart(&status);
            // New image, insert something here
            intensityShm[0].md[0].cnt2 = imShm[0].md[0].cnt2;

            sum=0;
            for (k=0; k<validPixSum; k++)
            {
                //sum += imShm[0].array.F[lut[k]];
                if (validSubPixShm[0].array.UI32[lut[k]] == 1)
                {
                    sum += imShm[0].array.F[lut[k]];
                }
            }

            for (k=0; k<validPixSum; k++)
            {
                intensityShm[0].array.F[k] =  imShm[0].array.F[lut[k]]/sum * validSubPixShm[0].array.UI32[lut[k]];
            }

            daoShmSetDataPartFinalize(&intensityShm[0]);

            daoToolsLoopStatusEnd(&status, NULL);
        }
        else
        {
            daoToolsLoopStatusWait(&status);
        }
    }
    printf("\n");


    daoInfo("EXITING MAIN LOOP\n");


    daoToolsShmRelease(&imShm);
    daoToolsShmRelease(&intensityShm);
    daoToolsShmRelease(&validPixShm);
    daoToolsShmRelease(&validSubPixShm);
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
                break;
            case 'S':
                        daoInfo("real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, imShmName, sizeof imShmName);
                        daoToolsArgNameNext(&argc, &argv, str, intensityShmName, sizeof intensityShmName);
                        daoToolsArgNameNext(&argc, &argv, str, validPixShmName, sizeof validPixShmName);
                        daoToolsArgNameNext(&argc, &argv, str, validSubPixShmName, sizeof validSubPixShmName);
                        daoInfo("imShmName = %s\n", imShmName);
                        daoInfo("intensityShmName = %s\n", intensityShmName);
                        daoInfo("validPixShmName = %s\n", validPixShmName);
                        daoInfo("validSubPixShmName = %s\n", validSubPixShmName);
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

