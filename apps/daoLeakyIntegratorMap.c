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
static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;

struct timespec tnow;
double tnowdouble;
double tlastupdatedouble;

char inShmName[32];
int semNb = 0;
char offsetShmName[32];
char outShmName[32];
char lpCmdShmName[32];
char gainShmName[32];
char leakyShmName[32];
char mapShmName[32];
char enableShmName[64];
int enableGiven = 0;   /* -e was passed explicitly: use enableShmName as-is, don't derive it */
int modal=0; // modal integrator flag
double clipping = 10.0; // clipping value

#define LI_PRINT_INTERVAL_S 1.0   /* throttle telemetry: print once every N seconds of wall time,
                                    * not every frame -- a frame-count throttle would make the
                                    * print rate depend on the loop's own speed. */

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
    daoInfo("   -m               modal integrator, leaky and gain should be arrays\n");
    daoInfo("   -e <shm>         optional enable shm (default: derived from <loopCmd> as\n");
    daoInfo("                    <loopCmd base name>Enable.im.shm, e.g. lpCmd.im.shm ->\n");
    daoInfo("                    lpCmdEnable.im.shm)\n");
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("   -S <in SHM> <offset SHM> <out SHM> <loopCmd SHM> <leak SHM> <gain SHM> -s <semNb> [-e <enableShm>] -m -L\n");
    daoInfo("\n");
    daoInfo("   -e lets an external SHM enable/disable the integrator independently of\n");
    daoInfo("   the loopCmd open/close state: when the enable SHM reads 0, the output is\n");
    daoInfo("   zeroed and published once, then the out SHM semaphores are no longer\n");
    daoInfo("   posted until enable returns to 1, regardless of loopCmd.\n");
    daoInfo("   If the enable SHM does not exist yet it is created here with value 1\n");
    daoInfo("   (enabled), so default behaviour is unchanged whether -e is used or not.\n");
    daoInfo("\n");
}

/*--------------------------------------------------------------------------*/
/* Force the output frame to 0. The output array is also the integrator's
 * state (see daoToolsLeakyIntegrator), so this doubles as a state reset. */
static void zeroOutput(IMAGE *outShm, int atype, int size)
{
    int j;
    if (atype == _DATATYPE_FLOAT)
    {
        for (j = 0; j < size; j++)
        {
            outShm[0].array.F[j] = 0.0;
        }
    }
    else
    {
        for (j = 0; j < size; j++)
        {
            outShm[0].array.D[j] = 0.0;
        }
    }
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    daoInfo("Modal integrator: %s\n", modal ? "yes" : "no");
    daoInfo("Starting loop, %s -> %s \n", inShmName, outShmName);
    fflush(stdout);
    IMAGE *inShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *offsetShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *outShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *lpCmdShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *gainShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *leakyShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *mapShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *enableShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(inShmName, &inShm[0]);
    daoShmShm2Img(offsetShmName, &offsetShm[0]);
    daoShmShm2Img(outShmName, &outShm[0]);
    daoShmShm2Img(lpCmdShmName, &lpCmdShm[0]);
    daoShmShm2Img(gainShmName, &gainShm[0]);
    daoShmShm2Img(leakyShmName, &leakyShm[0]);
    daoShmShm2Img(mapShmName, &mapShm[0]);

    // Enable shm: -e overrides, otherwise derive <loopCmd base>Enable.im.shm
    // from the loopCmd shm's name (same helper used for daoTimeDiff's
    // Avg/Rms/Array sibling shms). Attach if it already exists; if not,
    // create it here with value 1 (enabled) so existing setups that never
    // heard of -e keep behaving exactly as before.
    if (!enableGiven)
    {
        daoToolsInsertShmNamePrefix(lpCmdShmName, "Enable", enableShmName);
    }
    daoInfo("enableShmName    = %s\n", enableShmName);
    if (daoShmShm2Img(enableShmName, &enableShm[0]) != DAO_SUCCESS)
    {
        uint32_t enableSize[2] = {1, 1};
        daoInfo("enable shm not found, creating %s with enable=1\n", enableShmName);
        daoShmImageCreate(enableShm, enableShmName, 2, enableSize, _DATATYPE_UINT32, 1, 0);
        enableShm[0].array.UI32[0] = 1;
    }

    int inSize = inShm[0].md[0].size[0]*inShm[0].md[0].size[1];
    int outSize = outShm[0].md[0].size[0]*outShm[0].md[0].size[1];
    float inMapped[outSize];
    struct timespec timeout;
    struct timespec t[3];
    double elapsedTime;
    clock_gettime(CLOCK_REALTIME, &t[1]);
    struct timespec tLastPrint = t[1];
    unsigned long iter = 0;
    double fpsAccum = 0.0;
    int j, k;
    int cnt=0;
    int cntMap=0;
    float avg=0;
    /* enable==0: publish a single zeroed frame, then go quiet (stop
     * finalizing) until enable goes back to 1. */
    int zeroSent = 0;
    while (end ==0)
    {
        t[0] = t[1];
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        // Wait for new image
        if (daoShmWaitForSemaphoreTimeout(inShm, semNb, &timeout) != -1)
        {
            cntMap=0;
            if (inShm[0].md[0].atype == _DATATYPE_FLOAT)
            {
                for (k=0; k<outSize; k++)
                {
                    if (mapShm[0].array.UI32[k] == 1)
                    {
                        inMapped[k] = inShm[0].array.F[cntMap];
                        cntMap++;
                    }
                    else
                    {
                        inMapped[k] = 0.0;
                    }
                }
            }
            else
            {
                for (k=0; k<outSize; k++)
                {
                    if (mapShm[0].array.UI32[k] == 1)
                    {
                        inMapped[k] = inShm[0].array.D[cntMap];
                        cntMap++;
                    }
                    else
                    {
                        inMapped[k] = 0.0;
                    }
                }
            }
            // New image, insert something here
            outShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
            avg=0;
            int enabled = (enableShm[0].array.UI32[0] == 1);
            int publish = 1;   /* finalize (post the out shm semaphores) this frame? */
            if (!enabled)
            {
                /* Disabled: leave consumers on a known zeroed frame, published
                 * exactly once, then stop posting entirely so nothing
                 * downstream is woken while we are off. */
                if (zeroSent)
                {
                    publish = 0;
                }
                else
                {
                    zeroOutput(outShm, inShm[0].md[0].atype, outSize);
                    zeroSent = 1;
                }
            }
            else if (lpCmdShm[0].array.UI32[0] == 1)
            {
                zeroSent = 0;
                if (inShm[0].md[0].atype == _DATATYPE_FLOAT)
                {
                    if (modal == 0)
                    {
                        daoToolsLeakyIntegrator(inMapped, 
                                                outSize,
                                                leakyShm[0].array.F[0], 
                                                gainShm[0].array.F[0], 
                                                offsetShm[0].array.F,
                                                outShm[0].array.F);
                    }
                    else
                    {
                        daoToolsLeakyModalIntegrator(inMapped, 
                                                outSize,
                                                leakyShm[0].array.F, 
                                                gainShm[0].array.F, 
                                                offsetShm[0].array.F,
                                                outShm[0].array.F);
                    }
                    for (j=0; j< outSize; j++)
                    {
                        avg+=outShm[0].array.F[j];
                    }
                    avg=avg/outSize;
                    for (j=0; j< outSize; j++)
                    {
                        outShm[0].array.F[j] = outShm[0].array.F[j] - avg;
                        if (outShm[0].array.F[j] > clipping)
                        {
                            outShm[0].array.F[j] = clipping;
                        }
                        else if (outShm[0].array.F[j] < -clipping)
                        {
                            outShm[0].array.F[j] = -clipping;
                        }
                    }
                }
                else
                {
                    if (modal == 0)
                    {
                        daoToolsLeakyIntegratorDouble(inShm[0].array.D, 
                                                  outSize,
                                                  leakyShm[0].array.D[0], 
                                                  gainShm[0].array.D[0], 
                                                  offsetShm[0].array.D,
                                                  outShm[0].array.D);
                    }
                    else
                    {
                        daoToolsLeakyModalIntegratorDouble(inShm[0].array.D, 
                                                  outSize,
                                                  leakyShm[0].array.D, 
                                                  gainShm[0].array.D, 
                                                  offsetShm[0].array.D,
                                                  outShm[0].array.D);
                    }
                    for (j=0; j< outSize; j++)
                    {
                        avg+=outShm[0].array.D[j];
                    }
                    avg=avg/outSize;
                    for (j=0; j< outSize; j++)
                    {
                        outShm[0].array.D[j] = outShm[0].array.D[j] - avg;
                        if (outShm[0].array.D[j] > clipping)
                        {
                            outShm[0].array.D[j] = clipping;
                        }
                        else if (outShm[0].array.D[j] < -clipping)
                        {
                            outShm[0].array.D[j] = -clipping;
                        }
                    }                    
                }
            }
            else
            {
                /* Open loop but still enabled: keep publishing zeros every
                 * frame, as before. */
                zeroSent = 0;
                zeroOutput(outShm, inShm[0].md[0].atype, outSize);
            }
            if (publish)
            {
                daoShmImagePart2ShmFinalize(&outShm[0]);
            }

            clock_gettime(CLOCK_REALTIME, &t[1]);
            elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;

            // Accumulate telemetry; print only once every LI_PRINT_INTERVAL_S
            // seconds of wall time so the per-iteration fflush(stdout) stays
            // off the critical path.
            fpsAccum += (elapsedTime > 0.0) ? 1e3 / elapsedTime : 0.0;
            iter++;
            double sinceLastPrint = (t[1].tv_sec - tLastPrint.tv_sec)
                                   + (t[1].tv_nsec - tLastPrint.tv_nsec) / 1e9;
            if (sinceLastPrint >= LI_PRINT_INTERVAL_S && iter > 0)
            {
                if (inShm[0].md[0].atype == _DATATYPE_FLOAT)
                {
                    printf("\r fps = %8.3f Hz (avg/%.1fs, %lu frames), %d in=[%6.3f,%6.3f,...,%6.3f], out[%6.3f, %6.3f,...,%6.3f]",
                                                                                      fpsAccum / iter, sinceLastPrint, iter,
                                                                                      inSize, inShm[0].array.F[0],
                                                                                      inShm[0].array.F[1],
                                                                                      inShm[0].array.F[inSize],
                                                                                      outShm[0].array.F[0],
                                                                                      outShm[0].array.F[1],
                                                                                      outShm[0].array.F[outSize]);
                }
                else
                {
                    printf("\r fps = %8.3f Hz (avg/%.1fs, %lu frames), %d in=[%6.3lf,%6.3lf,...,%6.3lf], out[%6.3lf, %6.3lf,...,%6.3lf]",
                                                                                      fpsAccum / iter, sinceLastPrint, iter,
                                                                                      inSize, inShm[0].array.D[0],
                                                                                      inShm[0].array.D[1],
                                                                                      inShm[0].array.D[inSize],
                                                                                      outShm[0].array.D[0],
                                                                                      outShm[0].array.D[1],
                                                                                      outShm[0].array.D[outSize]);
                }
                fflush(stdout);
                fpsAccum = 0.0;
                iter = 0;
                tLastPrint = t[1];
            }
        }
        else
        {
            cnt++;
            printf("\rWAIT ... %d", cnt);
            fflush(stdout);
        }
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

        switch (str[1]) {
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
            case 'c':
                        (void)sscanf(*argv++, "%lf", &clipping);
                        argc -= 1;	
                        daoInfo("clipping value set to %f\n", clipping);
                        break;
            case 'm':	
                        modal = 1;
                        break;
            case 'S':
                        daoInfo("Simple filter from SHM real time control\n");
                    	(void)sscanf(*argv++,"%s", inShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", offsetShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", outShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", lpCmdShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", gainShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", leakyShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", mapShmName); argc -= 1;
                        daoInfo("inShmName      = %s\n", inShmName);
                        daoInfo("offsetShmName  = %s\n", offsetShmName);
                        daoInfo("outShmName     = %s\n", outShmName);
                        daoInfo("lpCmdShmName   = %s\n", lpCmdShmName);
                        daoInfo("gainShmName    = %s\n", gainShmName);
                        daoInfo("leakyShmName   = %s\n", leakyShmName);
                        daoInfo("mapShmName   = %s\n",   mapShmName);
                        break;
            case 's':
                        (void)sscanf(*argv++,"%d", &semNb);
                        daoInfo("inputShm sem     : %d \n", semNb);
                        break;
            case 'e':
                        (void)sscanf(*argv++, "%63s", enableShmName); argc -= 1;
                        enableGiven = 1;
                        daoInfo("enableShmName (given) = %s\n", enableShmName);
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

