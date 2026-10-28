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
#include <time.h>

#include "dao.h"
#include "daoTools.h"

/*==========================================================================*/
static int	sExit=0;						/* program exit code */

char polShmName[DAO_SHM_NAME_LEN];
char gainShmName[DAO_SHM_NAME_LEN];
char leakShmName[DAO_SHM_NAME_LEN];       // -K: optional
char loopGainShmName[DAO_SHM_NAME_LEN];   // -G: optional
char enableShmName[DAO_SHM_NAME_LEN];     // -e: optional
int semNb = DAO_SEM_AUTO;   // -s: a fixed semaphore; default: one of its own
int delay = 0;              // -k: the loop delay, frames (required)
int nbFrames = 1000;        // -n: frames per fit
int skip = 300;             // -z: frames ignored at the start of each replay
double gainMin = 0.01;      // -g: the gains tried
double gainMax = 1.0;
int nbGains = 100;

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
    printf("   -k <delay>       the loop delay, frames (>= 1, required; as daoPseudoOpenLoop's)\n");
    printf("   -n <frames>      frames per fit (default %d)\n", nbFrames);
    printf("   -z <frames>      frames ignored at the start of each replay, the integrator\n");
    printf("                    settling (default %d, at most a third of -n)\n", skip);
    printf("   -g <min> <max> <count>  the gains tried, evenly spaced (default %g %g %d)\n", gainMin, gainMax, nbGains);
    printf("   -K <leak SHM>    the integrator's leak, 1 value or 1 per mode (default: no leak, 1)\n");
    printf("   -G <loop gain SHM>  a scalar gain the loop also applies (e.g. a scalar\n");
    printf("                    daoLeakyIntegrator after daoApplyGain -m): the gains written\n");
    printf("                    are divided by it\n");
    printf("   -e <enable SHM>  0: no fit (hold the gains), 1: fit. Created at 1 if missing\n");
    printf("   -s <semNb>       a fixed semaphore to wait on (default: one of its own)\n");
    printf("   -L               start the loop (after the other options)\n");
    printf("   usage:\n");
    printf("   daoModalGainOptimizer -S <pol SHM> <gain SHM> -k <delay> [-n <frames>] [-z <frames>]\n");
    printf("                         [-g <min> <max> <count>] [-K <leak SHM>] [-G <loop gain SHM>]\n");
    printf("                         [-e <enable SHM>] [-s <semNb>] -L\n");
    printf("\n");
    printf("   OMGI, the optimized modal gain integrator: collects -n consecutive frames of the\n");
    printf("   pseudo open loop (daoPseudoOpenLoop), replays on them, for every mode and every gain\n");
    printf("   tried, an integrator with the loop delay, and writes the gain that leaves the smallest\n");
    printf("   residual of each mode to the gain SHM (daoLeakyIntegrator -m, or daoApplyGain -m).\n");
    printf("   Then it starts again. The fit does not depend on the gains the data was taken with.\n");
    printf("   A mode whose replay diverged for every gain keeps its gain. Not real time: a missed\n");
    printf("   pol frame restarts the collection.\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
/* Open an optional SHM, NULL when not asked for. */
static IMAGE *openOptional(const char *name)
{
    if (name[0] == '\0')
    {
        return NULL;
    }
    IMAGE *shm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(name, &shm[0]);
    daoToolsShmCheck(shm, name, _DATATYPE_FLOAT, 1);
    return shm;
}

static int optimizerLoop()
{
    if (delay < 1)
    {
        daoError("the loop delay is needed: -k <delay>, before -L\n");
        return 1;
    }
    if (nbFrames < 3 || nbGains < 1 || skip < 0)
    {
        daoError("-n %d frames, -g %d gains, -z %d: at least 3, 1, 0\n", nbFrames, nbGains, skip);
        return 1;
    }
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    IMAGE *polShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *gainShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(polShmName, &polShm[0]);
    daoToolsShmOpen(gainShmName, &gainShm[0]);
    int nbModes = daoToolsShmValues(polShm);
    daoToolsShmCheck(polShm, polShmName, _DATATYPE_FLOAT, 1);
    daoToolsShmCheck(gainShm, gainShmName, _DATATYPE_FLOAT, nbModes);
    IMAGE *leakShm = openOptional(leakShmName);
    IMAGE *loopGainShm = openOptional(loopGainShmName);
    if (leakShm != NULL && daoToolsShmValues(leakShm) != 1)
    {
        daoToolsShmCheck(leakShm, leakShmName, _DATATYPE_FLOAT, nbModes);
    }
    IMAGE *enableShm = NULL;
    if (enableShmName[0] != '\0')
    {
        enableShm = (IMAGE*) malloc(sizeof(IMAGE));
        if (daoShmOpen(enableShmName, &enableShm[0]) != DAO_SUCCESS)
        {
            uint32_t enableSize[2] = {1, 1};
            daoInfo("enable shm not found, creating %s with enable=1\n", enableShmName);
            daoShmCreate(enableShm, enableShmName, 2, enableSize, _DATATYPE_UINT32, 1, 0);
            enableShm[0].array.UI32[0] = 1;
        }
    }

    float *pol = malloc((size_t)nbFrames * nbModes * sizeof(float));
    float *gains = malloc(nbGains * sizeof(float));
    float *leak = malloc(nbModes * sizeof(float));
    float *current = malloc(nbModes * sizeof(float));
    float *best = malloc(nbModes * sizeof(float));
    double *currentCost = malloc(nbModes * sizeof(double));
    double *bestCost = malloc(nbModes * sizeof(double));
    if (!pol || !gains || !leak || !current || !best || !currentCost || !bestCost)
    {
        daoError("no memory for %d frames of %d modes\n", nbFrames, nbModes);
        return 1;
    }
    int g, m;
    for (g = 0; g < nbGains; g++)
    {
        gains[g] = nbGains == 1 ? gainMin : gainMin + (gainMax - gainMin) * g / (nbGains - 1);
    }

    printf("Starting, %s: %d modes, %d frames per fit, delay %d, gains %g to %g -> %s\n",
           polShmName, nbModes, nbFrames, delay, gainMin, gainMax, gainShmName);
    fflush(stdout);

    int n = 0;                  // frames collected
    int fits = 0;
    long restarts = 0;
    time_t lastWarning = 0;
    uint64_t lastCnt = 0;
    int haveCnt = 0;
    while (end == 0)
    {
        if (daoToolsWait(polShm, semNb, 1.0) != DAO_SUCCESS)
        {
            continue;
        }
        uint64_t cnt = polShm[0].md[0].cnt0;
        if (haveCnt && cnt == lastCnt)
        {
            continue;           // a post of a frame already read (posts left from a fit)
        }
        if (n > 0 && cnt != lastCnt + 1)
        {
            restarts++;         // a frame missed: the replay needs consecutive frames
            n = 0;
            if (time(NULL) != lastWarning)
            {
                lastWarning = time(NULL);
                daoWarning("pol frames missed, collection restarted (%ld times)\n", restarts);
            }
        }
        lastCnt = cnt;
        haveCnt = 1;
        if (enableShm != NULL && enableShm[0].array.UI32[0] == 0)
        {
            n = 0;              // on hold
            continue;
        }
        memcpy(pol + (size_t)n * nbModes, polShm[0].array.F, nbModes * sizeof(float));
        n++;
        if (n < nbFrames)
        {
            continue;
        }
        n = 0;

        // the gains in use, the integrator's (its gain SHM times the loop's scalar gain)
        float loopGain = loopGainShm != NULL ? loopGainShm[0].array.F[0] : 1.0f;
        if (loopGain == 0.0f)
        {
            daoWarning("%s is 0: no fit\n", loopGainShmName);
            continue;
        }
        for (m = 0; m < nbModes; m++)
        {
            leak[m] = leakShm == NULL ? 1.0f
                    : leakShm[0].array.F[daoToolsShmValues(leakShm) == 1 ? 0 : m];
            current[m] = gainShm[0].array.F[m] * loopGain;
            best[m] = current[m];
        }
        daoToolsModalGainCost(pol, nbModes, nbFrames, delay, leak, current, skip, currentCost);
        daoToolsModalGainFit(pol, nbModes, nbFrames, delay, leak, gains, nbGains, skip, best, bestCost);

        double sumCurrent = 0, sumBest = 0;
        float lo = INFINITY, hi = -INFINITY, mean = 0;
        int kept = 0;
        for (m = 0; m < nbModes; m++)
        {
            if (!isfinite(bestCost[m]))
            {
                kept++;         // diverged for every gain: keeps its gain
                continue;
            }
            gainShm[0].array.F[m] = best[m] / loopGain;
            sumCurrent += currentCost[m];
            sumBest += bestCost[m];
            lo = fminf(lo, best[m]);
            hi = fmaxf(hi, best[m]);
            mean += best[m];
        }
        daoShmSetDataPartFinalize(&gainShm[0]);
        fits++;
        if (kept == nbModes)
        {
            printf("fit %d: every mode diverged, gains kept\n", fits);
        }
        else
        {
            printf("fit %d: gains %.3f to %.3f (mean %.3f), residual rms %.4g -> %.4g (replayed)",
                   fits, lo, hi, mean / (nbModes - kept), sqrt(sumCurrent), sqrt(sumBest));
            if (kept > 0)
            {
                printf(", %d modes kept their gain", kept);
            }
            printf("\n");
        }
        fflush(stdout);
    }

    daoInfo("EXITING MAIN LOOP\n");

    free(pol);
    free(gains);
    free(leak);
    free(current);
    free(best);
    free(currentCost);
    free(bestCost);
    daoToolsShmRelease(&polShm);
    daoToolsShmRelease(&gainShm);
    daoToolsShmRelease(&leakShm);
    daoToolsShmRelease(&loopGainShm);
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
            case 'S':
                daoToolsArgNameNext(&argc, &argv, str, polShmName, sizeof polShmName);
                daoToolsArgNameNext(&argc, &argv, str, gainShmName, sizeof gainShmName);
                daoInfo("polShmName       = %s\n", polShmName);
                daoInfo("gainShmName      = %s\n", gainShmName);
                break;
            case 'k':
                delay = daoToolsArgInt(&argc, &argv, str);
                break;
            case 'n':
                nbFrames = daoToolsArgInt(&argc, &argv, str);
                break;
            case 'z':
                skip = daoToolsArgInt(&argc, &argv, str);
                break;
            case 'g':
                gainMin = daoToolsArgDouble(&argc, &argv, str);
                gainMax = daoToolsArgDouble(&argc, &argv, str);
                nbGains = daoToolsArgInt(&argc, &argv, str);
                break;
            case 'K':
                daoToolsArgNameNext(&argc, &argv, str, leakShmName, sizeof leakShmName);
                break;
            case 'G':
                daoToolsArgNameNext(&argc, &argv, str, loopGainShmName, sizeof loopGainShmName);
                break;
            case 'e':
                daoToolsArgNameNext(&argc, &argv, str, enableShmName, sizeof enableShmName);
                break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                break;
            case 'L':
                if (optimizerLoop() != 0)         /* could not start, or failed (see above) */
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
    // not real time: normal priority, the fits never take the CPU from the loop
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
