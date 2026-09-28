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
int semNb = DAO_SEM_AUTO;   // -s: a fixed semaphore; default: one of its own
char maskShmName[DAO_SHM_NAME_LEN];
char extractShmName[DAO_SHM_NAME_LEN];

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
    printf("   -S <input SHM> <mask SHM> <extract SHM> [-s <semNb>] -L\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session
    IMAGE *inShm;
    IMAGE *maskShm;
    IMAGE *extractShm;

    inShm = (IMAGE*) malloc(sizeof(IMAGE));
    maskShm = (IMAGE*) malloc(sizeof(IMAGE));
    extractShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(inShmName, &inShm[0]);
    daoToolsShmOpen(maskShmName, &maskShm[0]);
    daoToolsShmOpen(extractShmName, &extractShm[0]);

    daoInfo("Starting loop, %s/%s \n",inShmName, extractShmName);
    fflush(stdout);
    usleep(2000000);
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(inShm, semNb, 1.0) == DAO_SUCCESS)
        {
            daoToolsLoopStatusStart(&status);
            daoToolsShmExtract(inShm, maskShm, extractShm);

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
    daoToolsShmRelease(&maskShm);
    daoToolsShmRelease(&extractShm);
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
                daoToolsArgNameNext(&argc, &argv, str, inShmName, sizeof inShmName);
                daoToolsArgNameNext(&argc, &argv, str, maskShmName, sizeof maskShmName);
                daoToolsArgNameNext(&argc, &argv, str, extractShmName, sizeof extractShmName);
                daoInfo("image in         : %s\n", inShmName);
                daoInfo("mask             : %s\n", maskShmName);
                daoInfo("extracted image  : %s\n", extractShmName);
                break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                daoInfo("inputShm sem     : %d \n", semNb);
                break;
            case 'L':
                        daoInfo("Extract SHM real time control\n");
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

