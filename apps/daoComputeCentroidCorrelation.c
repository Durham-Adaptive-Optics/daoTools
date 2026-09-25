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
char subApCentreShmName[DAO_SHM_NAME_LEN];
int subaSize;
int nbSuba;
int searchRange;
int semNb = 0;
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
    daoInfo("%s of " __DATE__ " at " __TIME__ "\n",sArgv0);
    daoInfo("   arguments:\n");
    daoInfo("   -h               display this message and exit\n");
    daoInfo("   -d               display program debug output\n");
    daoInfo("   -S               list of SHM (full path separated by space)\n");
    daoInfo("   -s               semaphore number\n");
    daoInfo("   -a <alpha>       running-average reference update rate in (0,1], 0=disabled (default)\n");
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("   -S <in SHM> <centroid SHM> <subAp Centres SHM> <ref image SHM> <threshold SHM> <subaSize> <nbSuba> <searchRange> -s <semNb> [-a <alpha>] -L\n");
    daoInfo("\n");
    daoInfo("   Correlation centroider: cross-correlates each subaperture spot against\n");
    daoInfo("   a reference spot image (instead of a center-of-gravity). The ref image\n");
    daoInfo("   SHM holds one <subaSize>x<subaSize> template per subaperture, stacked\n");
    daoInfo("   row-wise: subaperture s occupies rows [s*subaSize .. (s+1)*subaSize-1].\n");
    daoInfo("   searchRange is the integer pixel search half-range for the correlation\n");
    daoInfo("   peak (must be <= %d, see DAO_CENTROID_CORR_MAX_SEARCH_RANGE).\n", DAO_CENTROID_CORR_MAX_SEARCH_RANGE);
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
    daoToolsShmOpen(inShmName, &inShm[0]);
    daoToolsShmOpen(centroidShmName, &centroidShm[0]);
    daoToolsShmOpen(subApCentreShmName, &subApCentreShm[0]);
    daoToolsShmOpen(refImageShmName, &refImageShm[0]);
    daoToolsShmOpen(thresholdShmName, &thresholdShm[0]);

    int inSize = inShm[0].md[0].size[0]*inShm[0].md[0].size[1];
    struct timespec t[3];
    struct timespec timeout;
    double elapsedTime;
    double compTime;
    int cnt=0;
    clock_gettime(CLOCK_REALTIME, &t[1]);
    usleep(2000000);
    while (end ==0)
    {
        t[0] = t[1];
        // Wait for new image
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        if (daoShmWaitSemTimeout(inShm, semNb, &timeout) != -1)
        {
            clock_gettime(CLOCK_REALTIME, &t[2]);
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
                                 refImageShm[0].array.F);
                daoShmSetDataPartFinalize(&refImageShm[0]);
            }

            clock_gettime(CLOCK_REALTIME, &t[1]);
            elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            compTime = (t[1].tv_sec - t[2].tv_sec) * 1e3;
            compTime += (t[1].tv_nsec - t[2].tv_nsec) / 1e6;
            printf("\rcompTime = %.3f ms, fps = %8.3f Hz, %d in=[%6.3f,%6.3f,...,%6.3f], out[%6.3f, %6.3f,...,%6.3f]", compTime, 1e6/(1000*elapsedTime),
                                                                                  inSize, (float)inShm[0].array.F[0],
                                                                                  (float)inShm[0].array.F[1],
                                                                                  (float)inShm[0].array.F[inSize],
                                                                                  centroidShm[0].array.F[0],
                                                                                  centroidShm[0].array.F[1],
                                                                                  centroidShm[0].array.F[2]);
        }
        else
        {
            printf("\r WAIT %d", cnt);
            cnt++;
        }
        fflush(stdout);
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
                        daoInfo("Correlation centroider from SHM real time control\n");
                    	daoToolsArgName(inShmName, sizeof inShmName, *argv++); argc -= 1;
                        daoToolsArgName(centroidShmName, sizeof centroidShmName, *argv++); argc -= 1;
                    	daoToolsArgName(subApCentreShmName, sizeof subApCentreShmName, *argv++); argc -= 1;
                    	daoToolsArgName(refImageShmName, sizeof refImageShmName, *argv++); argc -= 1;
                    	daoToolsArgName(thresholdShmName, sizeof thresholdShmName, *argv++); argc -= 1;
                    	(void)sscanf(*argv++,"%d", &subaSize); argc -= 1;
                    	(void)sscanf(*argv++,"%d", &nbSuba); argc -= 1;
                    	(void)sscanf(*argv++,"%d", &searchRange); argc -= 1;
                        daoInfo("inShmName = %s\n", inShmName);
                        daoInfo("centroidShmName = %s\n", centroidShmName);
                        daoInfo("subApCentreShmName = %s\n", subApCentreShmName);
                        daoInfo("refImageShmName = %s\n", refImageShmName);
                        daoInfo("thresholdShmName = %s\n", thresholdShmName);
                        daoInfo("subaSize = %d\n", subaSize);
                        daoInfo("nbSuba = %d\n", nbSuba);
                        daoInfo("searchRange = %d\n", searchRange);
                        break;
            case 's':
                        (void)sscanf(*argv++,"%d", &semNb); argc -= 1;
                        daoInfo("inputShm sem       = %d \n", semNb);
                        break;
            case 'a':
                        (void)sscanf(*argv++,"%f", &refAlpha); argc -= 1;
                        daoInfo("refAlpha           = %g %s\n", refAlpha, refAlpha > 0.0f ? "" : "(disabled)");
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
