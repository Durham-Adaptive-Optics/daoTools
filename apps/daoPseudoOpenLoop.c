/*****************************************************************************
  DAO project
  s.cetre
 *****************************************************************************/

/*==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <signal.h>

#include "dao.h"
#include "daoTools.h"

/*==========================================================================*/
static int	sExit=0;						/* program exit code */

char residualShmName[DAO_SHM_NAME_LEN];
char commandShmName[DAO_SHM_NAME_LEN];
char polShmName[DAO_SHM_NAME_LEN];
int semNb = DAO_SEM_AUTO;   // -s: a fixed semaphore; default: one of its own
int delay = 0;              // -k: the loop delay, frames (required)

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
    printf("   -k <delay>       the loop delay, frames (>= 1, required): the residual of frame t\n");
    printf("                    holds the command computed from the residual of frame t - delay\n");
    printf("   -s <semNb>       a fixed semaphore to wait on (default: one of its own)\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   daoPseudoOpenLoop -S <residual SHM> <command SHM> <pol SHM> -k <delay> [-s <semNb>] -L\n");
    printf("\n");
    printf("   The pseudo open loop, what the sensor would see with the DM flat:\n");
    printf("       pol(t) = residual(t) - command(t - delay)\n");
    printf("   residual: the modes measured, before any gain (what daoLeakyIntegrator integrates,\n");
    printf("   without the gain); command: daoLeakyIntegrator's output. It waits on the command SHM\n");
    printf("   (written once per frame, right after the residual) and creates the pol SHM if needed.\n");
    printf("   When the integrator is disabled (its enable SHM at 0) it stops posting, and so does\n");
    printf("   this. daoModalGainOptimizer fits the integrator's gains on the pol.\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    if (delay < 1)
    {
        daoError("the loop delay is needed: -k <delay>, before -L\n");
        return 1;
    }
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    IMAGE *residualShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *commandShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *polShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(residualShmName, &residualShm[0]);
    daoToolsShmOpen(commandShmName, &commandShm[0]);
    long nbVal = daoToolsShmValues(residualShm);
    daoToolsShmCheck(residualShm, residualShmName, _DATATYPE_FLOAT, 1);
    daoToolsShmCheck(commandShm, commandShmName, _DATATYPE_FLOAT, nbVal);

    // the pol SHM: attach if it exists, otherwise create it the size of the residual
    if (daoShmOpen(polShmName, &polShm[0]) != DAO_SUCCESS)
    {
        uint32_t size[2] = {residualShm[0].md[0].size[0], residualShm[0].md[0].size[1]};
        daoInfo("pol shm not found, creating %s (%ux%u)\n", polShmName, size[0], size[1]);
        daoShmCreate(polShm, polShmName, 2, size, _DATATYPE_FLOAT, 1, 0);
    }
    daoToolsShmCheck(polShm, polShmName, _DATATYPE_FLOAT, nbVal);

    daoToolsPolHistory history;
    if (daoToolsPolInit(&history, nbVal, delay) != DAO_SUCCESS)
    {
        return 1;
    }

    daoInfo("Starting loop, %s - %s(t-%d) -> %s\n", residualShmName, commandShmName, delay, polShmName);
    fflush(stdout);

    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(commandShm, semNb, 1.0) == DAO_SUCCESS)
        {
            daoToolsLoopStatusStart(&status);
            if (daoToolsPseudoOpenLoop(&history, residualShm[0].array.F,
                                       commandShm[0].array.F, polShm[0].array.F))
            {
                polShm[0].md[0].cnt2 = residualShm[0].md[0].cnt2;
                daoShmSetDataPartFinalize(&polShm[0]);
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

    daoToolsPolFree(&history);
    daoToolsShmRelease(&residualShm);
    daoToolsShmRelease(&commandShm);
    daoToolsShmRelease(&polShm);
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
            case 'S':
                daoToolsArgNameNext(&argc, &argv, str, residualShmName, sizeof residualShmName);
                daoToolsArgNameNext(&argc, &argv, str, commandShmName, sizeof commandShmName);
                daoToolsArgNameNext(&argc, &argv, str, polShmName, sizeof polShmName);
                daoInfo("residualShmName  = %s\n", residualShmName);
                daoInfo("commandShmName   = %s\n", commandShmName);
                daoInfo("polShmName       = %s\n", polShmName);
                break;
            case 'k':
                delay = daoToolsArgInt(&argc, &argv, str);
                daoInfo("loop delay       = %d frames\n", delay);
                break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                daoInfo("commandShm sem   = %d\n", semNb);
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
    daoToolsSetRtPriority(93); //any number from 0-99; falls back + warns if not permitted

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
