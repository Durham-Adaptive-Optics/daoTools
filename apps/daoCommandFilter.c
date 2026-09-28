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
static int	sNdx=0;							/* board index */
static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;

struct timespec tnow;
double tnowdouble;
double tlastupdatedouble;

char inShmName[DAO_SHM_NAME_LEN];
char servoShmName[DAO_SHM_NAME_LEN];
char offsetShmName[DAO_SHM_NAME_LEN];
char outShmName[DAO_SHM_NAME_LEN];
char lpCmdShmName[DAO_SHM_NAME_LEN];

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
    printf("   -L inShm servoShm offsetShm outShm              real time control loop\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    daoInfo("Starting loop, %s/%s/%s/%s \n", inShmName, servoShmName, offsetShmName, outShmName);
    fflush(stdout);
    IMAGE *inShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *servoShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *offsetShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *outShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *lpCmdShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(inShmName, &inShm[0]);
    daoToolsShmOpen(servoShmName, &servoShm[0]);
    daoToolsShmOpen(offsetShmName, &offsetShm[0]);
    daoToolsShmOpen(outShmName, &outShm[0]);
    daoToolsShmOpen(lpCmdShmName, &lpCmdShm[0]);

    int inSize = inShm[0].md[0].size[0]*inShm[0].md[0].size[1];
    if (inSize > RES_MAX_VAL)
    {
        daoError("%s: %d values, the filter holds at most %d\n", inShmName, inSize, RES_MAX_VAL);
        exit(EXIT_FAILURE);
    }
    daoToolsShmCheck(inShm, inShmName, _DATATYPE_FLOAT, inSize);
    daoToolsShmCheck(outShm, outShmName, _DATATYPE_FLOAT, inSize);
    daoToolsShmCheck(offsetShm, offsetShmName, _DATATYPE_FLOAT, inSize);
    daoToolsShmCheck(lpCmdShm, lpCmdShmName, _DATATYPE_UINT32, 1);
    daoToolsShmCheck(servoShm, servoShmName, _DATATYPE_FLOAT, 2 * FILTER_ORDER + 1);   // the filter coefficients
    daoFilterHistory filterHistory;
    filterHistory.step = 0;
    int k,j;
    for(k=0; k<inSize;k++)
    {
        // Use this loop to reset filter to zero
        filterHistory.precal[k] = 0.0;
        for (j=0; j< FILTER_ORDER; j++)
        {
            filterHistory.dlCmd[j][k] = filterHistory.dlRes[j][k] = 0.0;
        }
    }
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(inShm, DAO_SEM_AUTO, 1.0) != DAO_SUCCESS)
        {
            daoToolsLoopStatusWait(&status);
            continue;
        }
        daoToolsLoopStatusStart(&status);
        outShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
        if (lpCmdShm[0].array.UI32[0] == 1)
        {
            daoToolsCommandFilter(inShm[0].array.F, inSize,
                                 &filterHistory, 
                                 servoShm[0].array.F, 
                                 offsetShm[0].array.F,
                                 outShm[0].array.F);
        }
        else
        {
            for(k=0; k<inSize;k++)
            {
                // Use this loop to reset filter to zero
                filterHistory.precal[k] = 0.0;
                for (j = 0; j < FILTER_ORDER; j++)
                {
                    filterHistory.dlCmd[j][k] = filterHistory.dlRes[j][k] = 0.0;
                }
            }
            for (j=0; j< inSize; j++)
            {
                outShm[0].array.F[j] = 0.0;
            }
        }
        daoShmSetDataPartFinalize(&outShm[0]);
        daoToolsLoopStatusEnd(&status, ", filter %s", lpCmdShm[0].array.UI32[0] == 1 ? "on" : "off");
    }
    printf("\n");
    daoInfo("EXITING MAIN LOOP\n");
    daoToolsShmRelease(&inShm);
    daoToolsShmRelease(&servoShm);
    daoToolsShmRelease(&offsetShm);
    daoToolsShmRelease(&outShm);
    daoToolsShmRelease(&lpCmdShm);
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

            case 'b':
                sNdx = daoToolsArgInt(&argc, &argv, str);
                break;
            case 'u':
                a1 = daoToolsArgInt(&argc, &argv, str);
                daoDebug("will sleep for %d usec\n", a1);
                (void)usleep(a1);
                break;
                break;
            case 'L':
                        daoInfo("Simple filter from SHM real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, inShmName, sizeof inShmName);
                        daoToolsArgNameNext(&argc, &argv, str, servoShmName, sizeof servoShmName);
                        daoToolsArgNameNext(&argc, &argv, str, offsetShmName, sizeof offsetShmName);
                        daoToolsArgNameNext(&argc, &argv, str, outShmName, sizeof outShmName);
                        daoToolsArgNameNext(&argc, &argv, str, lpCmdShmName, sizeof lpCmdShmName);
                        daoInfo("inShmName = %s\n", inShmName);
                        daoInfo("servoShmName = %s\n", servoShmName);
                        daoInfo("offsetShmName = %s\n", offsetShmName);
                        daoInfo("outShmName = %s\n", outShmName);
                        daoInfo("lpCmdShmName = %s\n", lpCmdShmName);
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

