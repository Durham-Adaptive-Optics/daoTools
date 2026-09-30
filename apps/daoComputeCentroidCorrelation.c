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
char refImageShmName[DAO_SHM_NAME_LEN];
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
char subApCentreShmName[DAO_SHM_NAME_LEN];
int subaSize;
int nbSuba;
int searchRange;
int semNb = DAO_SEM_AUTO;   // -s: a fixed semaphore; default: one of its own
float refAlpha = 0.0f;   /* running-average reference update rate; 0 = disabled (default) */

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
    printf("   -a <alpha>       running-average reference update rate in (0,1], 0=disabled (default)\n");
    printf("   -f <shm>         minimum flux SHM (1 value): a subaperture with less light (the sum\n");
    printf("                    of its raw pixels) gets slopes (0, 0), no noise; without -f: no minimum\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   -S <in SHM> <centroid SHM> <subAp Centres SHM> <ref image SHM> <threshold SHM> <subaSize> <nbSuba> <searchRange> [-s <semNb>] [-a <alpha>] -L\n");
    printf("\n");
    printf("   Correlation centroider: cross-correlates each subaperture spot against\n");
    printf("   a reference spot image (instead of a center-of-gravity). The ref image\n");
    printf("   SHM holds one <subaSize>x<subaSize> template per subaperture, stacked\n");
    printf("   row-wise: subaperture s occupies rows [s*subaSize .. (s+1)*subaSize-1].\n");
    printf("   searchRange is the integer pixel search half-range for the correlation\n");
    printf("   peak (must be <= %d, see DAO_CENTROID_CORR_MAX_SEARCH_RANGE).\n", DAO_CENTROID_CORR_MAX_SEARCH_RANGE);
    printf("\n");
    printf("   -a enables a running-average (EMA) update of the reference: after each\n");
    printf("   frame's centroids are computed and PUBLISHED, the just-observed spot\n");
    printf("   (re-aligned by that frame's own centroid) is blended into the reference\n");
    printf("   at rate alpha, so the reference tracks slow drift (e.g. seeing-induced\n");
    printf("   spot elongation) instead of staying fixed at its initial calibration.\n");
    printf("   This runs strictly after the centroid SHM is published, off the\n");
    printf("   critical path, and mirrors the updated reference back into the ref\n");
    printf("   image SHM. Disabled (alpha=0) by default: known to be less stable\n");
    printf("   than the plain reference in bad seeing with high loop gain -- see\n");
    printf("   pcpbStart's notes on daoComputeCentroidCorrelation(FFT) stability.\n");
    printf("\n");
}



/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    daoInfo("Starting loop, %s/%s/%s/%s/%s \n", inShmName, centroidShmName, subApCentreShmName, refImageShmName, thresholdShmName);
    fflush(stdout);
    IMAGE *inShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *centroidShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *subApCentreShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *refImageShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *thresholdShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(inShmName, &inShm[0]);
    daoToolsShmOpen(centroidShmName, &centroidShm[0]);
    daoToolsShmOpen(subApCentreShmName, &subApCentreShm[0]);
    daoToolsShmOpen(refImageShmName, &refImageShm[0]);
    daoToolsShmOpen(thresholdShmName, &thresholdShm[0]);
    IMAGE *minFluxShm = NULL;
    if (minFluxShmName[0] != '\0') {
        minFluxShm = (IMAGE*) malloc(sizeof(IMAGE));
        daoToolsShmOpen(minFluxShmName, &minFluxShm[0]);
    }

    int inSize = inShm[0].md[0].size[0]*inShm[0].md[0].size[1];
    daoToolsShmCheck(inShm, inShmName, _DATATYPE_FLOAT, inSize);
    daoToolsShmCheck(thresholdShm, thresholdShmName, _DATATYPE_FLOAT, 1);
    daoToolsShmCheck(subApCentreShm, subApCentreShmName, _DATATYPE_FLOAT, 2L * nbSuba);
    daoToolsShmCheck(refImageShm, refImageShmName, _DATATYPE_FLOAT, (long)nbSuba * subaSize * subaSize);
    daoToolsShmCheck(centroidShm, centroidShmName, _DATATYPE_FLOAT, 3L * nbSuba);   // x, y, flux
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

            daoCentroidSpotsCorrelation(inShm[0].array.F,
                             inShm[0].md[0].size[1],
                             inShm[0].md[0].size[0],
                             subApCentreShm[0].array.F,
                             refImageShm[0].array.F,
                             subaSize,
                             nbSuba,
                             searchRange,
                             thresholdShm[0].array.F[0],
                             (float)daoMinFlux(minFluxShm),
                             centroidShm[0].array.F);
            daoShmSetDataPartFinalize(&centroidShm[0]);

            // Off the critical path: this frame's centroids are already
            // published above, so there is plenty of time before the next
            // frame's semaphore wait to blend the just-observed, now-aligned
            // spot into the reference (see daoCentroidSpotsUpdateReference).
            // No-op when refAlpha <= 0 (the default).
            // threshold=0 here deliberately, NOT thresholdShm's value: that
            // threshold is tuned to suppress background for centroiding, but
            // applying it to the reference template zero-clips real signal in
            // the spot's low-amplitude wings. Since the reference feeds on its
            // own thresholded output every update, that erosion compounds.
            if (refAlpha > 0.0f) {
                daoCentroidSpotsUpdateReference(inShm[0].array.F,
                                 inShm[0].md[0].size[1],
                                 inShm[0].md[0].size[0],
                                 subApCentreShm[0].array.F,
                                 centroidShm[0].array.F,
                                 subaSize,
                                 nbSuba,
                                 0.0f,
                                 refAlpha,
                                 (float)daoMinFlux(minFluxShm),
                                 refImageShm[0].array.F);
                daoShmSetDataPartFinalize(&refImageShm[0]);
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


    daoToolsShmRelease(&inShm);
    daoToolsShmRelease(&centroidShm);
    daoToolsShmRelease(&subApCentreShm);
    daoToolsShmRelease(&refImageShm);
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
            case 'S':
                        daoInfo("Correlation centroider from SHM real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, inShmName, sizeof inShmName);
                        daoToolsArgNameNext(&argc, &argv, str, centroidShmName, sizeof centroidShmName);
                        daoToolsArgNameNext(&argc, &argv, str, subApCentreShmName, sizeof subApCentreShmName);
                        daoToolsArgNameNext(&argc, &argv, str, refImageShmName, sizeof refImageShmName);
                        daoToolsArgNameNext(&argc, &argv, str, thresholdShmName, sizeof thresholdShmName);
                        subaSize = daoToolsArgInt(&argc, &argv, str);
                        nbSuba = daoToolsArgInt(&argc, &argv, str);
                        searchRange = daoToolsArgInt(&argc, &argv, str);
                        daoInfo("inShmName = %s\n", inShmName);
                        daoInfo("centroidShmName = %s\n", centroidShmName);
                        daoInfo("subApCentreShmName = %s\n", subApCentreShmName);
                        daoInfo("refImageShmName = %s\n", refImageShmName);
                        daoInfo("thresholdShmName = %s\n", thresholdShmName);
                        daoInfo("subaSize = %d\n", subaSize);
                        daoInfo("nbSuba = %d\n", nbSuba);
                        daoInfo("searchRange = %d\n", searchRange);
                        break;
            case 'f':
                daoToolsArgNameNext(&argc, &argv, str, minFluxShmName, sizeof minFluxShmName);
                daoInfo("minFluxShmName     = %s\n", minFluxShmName);
                break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                daoInfo("inputShm sem       = %d \n", semNb);
                break;
            case 'a':
                refAlpha = daoToolsArgDouble(&argc, &argv, str);
                daoInfo("refAlpha           = %g %s\n", refAlpha, refAlpha > 0.0f ? "" : "(disabled)");
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
