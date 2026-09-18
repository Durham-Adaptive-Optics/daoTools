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
#define CORR_FFT_PRINT_INTERVAL_S 1.0   /* throttle telemetry: print once every N seconds of wall time,
                                          * not every N frames -- a frame-count throttle would make the
                                          * print rate depend on the loop's own speed. */

static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;

struct timespec tnow;
double tnowdouble;
double tlastupdatedouble;

char inShmName[256];
char refImageShmName[256];
char centroidShmName[256];
char thresholdShmName[256];
char subApCentreShmName[256];
int subaSize;
int nbSuba;
int semNb = 0;
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
    daoInfo("%s of " __DATE__ " at " __TIME__ "\n",sArgv0);
    daoInfo("   arguments:\n");
    daoInfo("   -h               display this message and exit\n");
    daoInfo("   -d               display program debug output\n");
    daoInfo("   -S               list of SHM (full path separated by space)\n");
    daoInfo("   -s               semaphore number\n");
    daoInfo("   -a <alpha>       running-average reference update rate in (0,1], 0=disabled (default)\n");
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("   -S <in SHM> <centroid SHM> <subAp Centres SHM> <ref image SHM> <threshold SHM> <subaSize> <nbSuba> -s <semNb> [-a <alpha>] -L\n");
    daoInfo("\n");
    daoInfo("   FFT correlation centroider: same idea as daoComputeCentroidCorrelation,\n");
    daoInfo("   but computes the full periodic correlation surface for each subaperture\n");
    daoInfo("   via FFT instead of a windowed shift search -- the whole box is searched\n");
    daoInfo("   for the price of one FFT round trip, no searchRange argument needed.\n");
    daoInfo("   The ref image SHM holds one <subaSize>x<subaSize> template per\n");
    daoInfo("   subaperture, stacked row-wise, same layout as the windowed version.\n");
    daoInfo("   Precision (float32 vs float64) is auto-detected from the input image\n");
    daoInfo("   SHM's atype; all 5 SHMs must share that same atype.\n");
    daoInfo("\n");
    daoInfo("   -a enables a running-average (EMA) update of the reference: after each\n");
    daoInfo("   frame's centroids are computed and PUBLISHED, the just-observed spot\n");
    daoInfo("   (re-aligned by that frame's own centroid) is blended into the reference\n");
    daoInfo("   at rate alpha, so the reference tracks slow drift (e.g. seeing-induced\n");
    daoInfo("   spot elongation) instead of staying fixed at its initial calibration.\n");
    daoInfo("   This runs strictly after the centroid SHM is published, off the\n");
    daoInfo("   critical path, and mirrors the updated reference back into the ref\n");
    daoInfo("   image SHM. Disabled (alpha=0) by default: known to be less stable\n");
    daoInfo("   than the plain reference in bad seeing with high loop gain -- see\n");
    daoInfo("   pcpbStart's notes on daoComputeCentroidCorrelation(FFT) stability.\n");
    daoInfo("\n");
}



/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    daoInfo("Starting loop, %s/%s/%s/%s/%s \n", inShmName, centroidShmName, subApCentreShmName, refImageShmName, thresholdShmName);
    fflush(stdout);
    IMAGE *inShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *centroidShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *subApCentreShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *refImageShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *thresholdShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(inShmName, &inShm[0]);
    daoShmShm2Img(centroidShmName, &centroidShm[0]);
    daoShmShm2Img(subApCentreShmName, &subApCentreShm[0]);
    daoShmShm2Img(refImageShmName, &refImageShm[0]);
    daoShmShm2Img(thresholdShmName, &thresholdShm[0]);

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

    int inSize = inShm[0].md[0].size[0]*inShm[0].md[0].size[1];
    struct timespec t[3];
    struct timespec timeout;
    double elapsedTime;
    double compTime;
    int cnt=0;
    unsigned long iter = 0;
    double compAccum = 0.0, fpsAccum = 0.0;
    struct timespec tLastPrint;
    clock_gettime(CLOCK_REALTIME, &t[1]);
    tLastPrint = t[1];
    usleep(2000000);
    while (end ==0)
    {
        t[0] = t[1];
        // Wait for new image
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        if (daoShmWaitForSemaphoreTimeout(inShm, semNb, &timeout) != -1)
        {
            clock_gettime(CLOCK_REALTIME, &t[2]);
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
            daoShmImagePart2ShmFinalize(&centroidShm[0]);

            clock_gettime(CLOCK_REALTIME, &t[1]);
            elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            compTime = (t[1].tv_sec - t[2].tv_sec) * 1e3;
            compTime += (t[1].tv_nsec - t[2].tv_nsec) / 1e6;

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
                daoShmImagePart2ShmFinalize(&refImageShm[0]);
            }

            // Accumulate telemetry; print only once every CORR_FFT_PRINT_INTERVAL_S
            // seconds of wall time (not every N frames -- a frame-count throttle
            // would make the print rate track the loop's own speed) so the
            // per-iteration fflush(stdout) stays off the critical path.
            compAccum += compTime;
            fpsAccum  += (elapsedTime > 0.0) ? 1e3 / elapsedTime : 0.0;
            iter++;
            double sinceLastPrint = (t[1].tv_sec - tLastPrint.tv_sec)
                                   + (t[1].tv_nsec - tLastPrint.tv_nsec) / 1e9;
            if (sinceLastPrint >= CORR_FFT_PRINT_INTERVAL_S && iter > 0)
            {
                float in0, in1, inN, out0, out1, out2;
                if (gIsFloat) {
                    in0 = inShm[0].array.F[0]; in1 = inShm[0].array.F[1]; inN = inShm[0].array.F[inSize];
                    out0 = centroidShm[0].array.F[0]; out1 = centroidShm[0].array.F[1]; out2 = centroidShm[0].array.F[2];
                } else {
                    in0 = (float)inShm[0].array.D[0]; in1 = (float)inShm[0].array.D[1]; inN = (float)inShm[0].array.D[inSize];
                    out0 = (float)centroidShm[0].array.D[0]; out1 = (float)centroidShm[0].array.D[1]; out2 = (float)centroidShm[0].array.D[2];
                }
                printf("\rcompTime = %9.3f ms, fps = %8.3f Hz (avg/%.1fs, %lu frames), %d in=[%6.3f,%6.3f,...,%6.3f], out[%6.3f, %6.3f,...,%6.3f]",
                       compAccum / iter, fpsAccum / iter, sinceLastPrint, iter,
                       inSize, in0, in1, inN, out0, out1, out2);
                fflush(stdout);
                compAccum = 0.0;
                fpsAccum  = 0.0;
                iter = 0;
                tLastPrint = t[1];
            }
        }
        else
        {
            printf("\r WAIT %d", cnt);
            cnt++;
            fflush(stdout);
        }
    }

    if (gIsFloat) {
        daoCentroidSpotsCorrelationFFTFree(ctx);
    } else {
        daoCentroidSpotsCorrelationFFTDoubleFree(ctxD);
    }

    daoInfo("EXITING MAIN LOOP\n");
    fflush(stdout);



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
                        (void)sscanf(*argv++,"%d",&daoLogLevel); argc -= 1;
                        break;
            case 'l':
                        daoInfo("%s\n",*argv);
                        argv += 1; argc -= 1;
                        break;
            case 'u':
                        (void)sscanf(*argv++,"%d",&a1); argc -= 1;
                        daoDebug("will sleep for %d usec\n",a1);
                        (void)usleep(a1);
                        break;
            case 'S':
                        daoInfo("FFT correlation centroider from SHM real time control\n");
                    	(void)sscanf(*argv++,"%255s", inShmName); argc -= 1;
                        (void)sscanf(*argv++,"%255s", centroidShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%255s", subApCentreShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%255s", refImageShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%255s", thresholdShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%d", &subaSize); argc -= 1;
                    	(void)sscanf(*argv++,"%d", &nbSuba); argc -= 1;
                        daoInfo("inShmName = %s\n", inShmName);
                        daoInfo("centroidShmName = %s\n", centroidShmName);
                        daoInfo("subApCentreShmName = %s\n", subApCentreShmName);
                        daoInfo("refImageShmName = %s\n", refImageShmName);
                        daoInfo("thresholdShmName = %s\n", thresholdShmName);
                        daoInfo("subaSize = %d\n", subaSize);
                        daoInfo("nbSuba = %d\n", nbSuba);
                        break;
            case 's':
                        (void)sscanf(*argv++,"%d", &semNb); argc -= 1;
                        daoInfo("inputShm sem       = %d \n", semNb);
                        break;
            case 'a':
                        (void)sscanf(*argv++,"%lf", &refAlpha); argc -= 1;
                        daoInfo("refAlpha           = %g %s\n", refAlpha, refAlpha > 0.0 ? "" : "(disabled)");
                        break;
            case 'L':
                        realTimeLoop();
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

    DecodeArgs(argc,argv);

    return(sExit);
}
/*==========================================================================*/
