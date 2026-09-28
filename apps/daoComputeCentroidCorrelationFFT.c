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
#include "daoToolsCorrFFT.h"

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
char subApCentreShmName[DAO_SHM_NAME_LEN];
int subaSize;
int nbSuba;
int semNb = DAO_SEM_AUTO;   // -s: a fixed semaphore; default: one of its own
double refAlpha = 0.0;   /* running-average reference update rate; 0 = disabled (default) */

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
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   -S <in SHM> <centroid SHM> <subAp Centres SHM> <ref image SHM> <threshold SHM> <subaSize> <nbSuba> [-s <semNb>] [-a <alpha>] -L\n");
    printf("\n");
    printf("   FFT correlation centroider: same idea as daoComputeCentroidCorrelation,\n");
    printf("   but computes the full periodic correlation surface for each subaperture\n");
    printf("   via FFT instead of a windowed shift search -- the whole box is searched\n");
    printf("   for the price of one FFT round trip, no searchRange argument needed.\n");
    printf("   The ref image SHM holds one <subaSize>x<subaSize> template per\n");
    printf("   subaperture, stacked row-wise, same layout as the windowed version.\n");
    printf("   Precision (float32 vs float64) is auto-detected from the input image\n");
    printf("   SHM's atype; all 5 SHMs must share that same atype.\n");
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

    // Precision (float32 vs float64) is decided by the input image SHM's
    // atype, matching daoMvMGPU's gIsFloat pattern; every other SHM must
    // agree, since they're all read through the same-typed union member.
    int gIsFloat = (inShm[0].md[0].atype == _DATATYPE_FLOAT);
    if (!gIsFloat && inShm[0].md[0].atype != _DATATYPE_DOUBLE) {
        daoError("daoComputeCentroidCorrelationFFT: unsupported input atype %d (need float or double)\n",
                 inShm[0].md[0].atype);
        return -1;
    }
    uint8_t wantType = gIsFloat ? _DATATYPE_FLOAT : _DATATYPE_DOUBLE;
    if (centroidShm[0].md[0].atype != wantType || subApCentreShm[0].md[0].atype != wantType ||
        refImageShm[0].md[0].atype != wantType || thresholdShm[0].md[0].atype != wantType) {
        daoError("daoComputeCentroidCorrelationFFT: all 5 SHMs must share the input image's atype (%s)\n",
                 gIsFloat ? "float" : "double");
        return -1;
    }
    daoInfo("precision: %s (from %s)\n", gIsFloat ? "float32" : "float64", inShmName);
    daoToolsShmCheck(thresholdShm, thresholdShmName, wantType, 1);
    daoToolsShmCheck(subApCentreShm, subApCentreShmName, wantType, 2L * nbSuba);
    daoToolsShmCheck(refImageShm, refImageShmName, wantType, (long)nbSuba * subaSize * subaSize);
    daoToolsShmCheck(centroidShm, centroidShmName, wantType, 3L * nbSuba);   // x, y, peak

    // FFTW plan creation + reference-template FFTs are not real-time-safe and
    // the reference doesn't change frame to frame, so this happens once here.
    daoCentroidCorrFFTCtx       *ctx  = NULL;
    daoCentroidCorrFFTDoubleCtx *ctxD = NULL;
    if (gIsFloat) {
        ctx = daoCentroidSpotsCorrelationFFTInit(subaSize, nbSuba, refImageShm[0].array.F);
        if (ctx == NULL) {
            daoError("daoCentroidSpotsCorrelationFFTInit failed, exiting\n");
            return -1;
        }
    } else {
        ctxD = daoCentroidSpotsCorrelationFFTDoubleInit(subaSize, nbSuba, refImageShm[0].array.D);
        if (ctxD == NULL) {
            daoError("daoCentroidSpotsCorrelationFFTDoubleInit failed, exiting\n");
            return -1;
        }
    }

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

            if (gIsFloat) {
                daoCentroidSpotsCorrelationFFT(ctx,
                                 inShm[0].array.F,
                                 inShm[0].md[0].size[1],
                                 inShm[0].md[0].size[0],
                                 subApCentreShm[0].array.F,
                                 thresholdShm[0].array.F[0],
                                 centroidShm[0].array.F);
            } else {
                daoCentroidSpotsCorrelationFFTDouble(ctxD,
                                 inShm[0].array.D,
                                 inShm[0].md[0].size[1],
                                 inShm[0].md[0].size[0],
                                 subApCentreShm[0].array.D,
                                 thresholdShm[0].array.D[0],
                                 centroidShm[0].array.D);
            }
            daoShmSetDataPartFinalize(&centroidShm[0]);

            daoToolsLoopStatusEnd(&status, NULL);
            // Off the critical path: this frame's centroids are already
            // published above, so there is plenty of time before the next
            // frame's semaphore wait to blend the just-observed, now-aligned
            // spot into the reference (see daoCentroidSpotsCorrelationFFTUpdateRef).
            // No-op when refAlpha <= 0 (the default).
            // threshold=0 here deliberately, NOT thresholdShm's value: that
            // threshold is tuned to suppress background for centroiding, but
            // applying it to the reference template zero-clips real signal in
            // the spot's low-amplitude wings. Since the reference feeds on its
            // own thresholded output every update, that erosion compounds.
            if (refAlpha > 0.0) {
                if (gIsFloat) {
                    daoCentroidSpotsCorrelationFFTUpdateRef(ctx,
                                     inShm[0].array.F,
                                     inShm[0].md[0].size[1],
                                     inShm[0].md[0].size[0],
                                     subApCentreShm[0].array.F,
                                     centroidShm[0].array.F,
                                     0.0f,
                                     (float)refAlpha,
                                     refImageShm[0].array.F);
                } else {
                    daoCentroidSpotsCorrelationFFTUpdateRefDouble(ctxD,
                                     inShm[0].array.D,
                                     inShm[0].md[0].size[1],
                                     inShm[0].md[0].size[0],
                                     subApCentreShm[0].array.D,
                                     centroidShm[0].array.D,
                                     0.0,
                                     refAlpha,
                                     refImageShm[0].array.D);
                }
                daoShmSetDataPartFinalize(&refImageShm[0]);
            }
        }
        else
        {
            daoToolsLoopStatusWait(&status);
        }
    }
    printf("\n");

    if (gIsFloat) {
        daoCentroidSpotsCorrelationFFTFree(ctx);
    } else {
        daoCentroidSpotsCorrelationFFTDoubleFree(ctxD);
    }

    daoInfo("EXITING MAIN LOOP\n");


    daoToolsShmRelease(&inShm);
    daoToolsShmRelease(&centroidShm);
    daoToolsShmRelease(&subApCentreShm);
    daoToolsShmRelease(&refImageShm);
    daoToolsShmRelease(&thresholdShm);
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
                        daoInfo("FFT correlation centroider from SHM real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, inShmName, sizeof inShmName);
                        daoToolsArgNameNext(&argc, &argv, str, centroidShmName, sizeof centroidShmName);
                        daoToolsArgNameNext(&argc, &argv, str, subApCentreShmName, sizeof subApCentreShmName);
                        daoToolsArgNameNext(&argc, &argv, str, refImageShmName, sizeof refImageShmName);
                        daoToolsArgNameNext(&argc, &argv, str, thresholdShmName, sizeof thresholdShmName);
                        subaSize = daoToolsArgInt(&argc, &argv, str);
                        nbSuba = daoToolsArgInt(&argc, &argv, str);
                        daoInfo("inShmName = %s\n", inShmName);
                        daoInfo("centroidShmName = %s\n", centroidShmName);
                        daoInfo("subApCentreShmName = %s\n", subApCentreShmName);
                        daoInfo("refImageShmName = %s\n", refImageShmName);
                        daoInfo("thresholdShmName = %s\n", thresholdShmName);
                        daoInfo("subaSize = %d\n", subaSize);
                        daoInfo("nbSuba = %d\n", nbSuba);
                        break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                daoInfo("inputShm sem       = %d \n", semNb);
                break;
            case 'a':
                refAlpha = daoToolsArgDouble(&argc, &argv, str);
                daoInfo("refAlpha           = %g %s\n", refAlpha, refAlpha > 0.0 ? "" : "(disabled)");
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
