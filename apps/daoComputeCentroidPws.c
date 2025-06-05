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

char inShmName[32];
char refShmName[32];
char centroidShmName[32];
char thresholdShmName[32];
char pixIdShmName[32];
char pixIdMapShmName[32];
char fluxShmName[32];
int nbPix;
int semNb = 0;

static int   		end     = 0;		           // termination flag
// termination function for SIGINT callback
static void endme()
{
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
    daoInfo("   -S               list of SHM (full path separated by space) + nb pixels\n");
    daoInfo("   -s               semaphore number\n");
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("   -S <input SHM> <ref SHM> <centroid SHM> <threshold SHM> <nbPix> -s <semNb> -L\n");
    daoInfo("\n");
}



/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    daoInfo("Starting loop, %s/%s/%s/%s \n", inShmName, refShmName, centroidShmName, thresholdShmName);
    fflush(stdout);
    IMAGE *inShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *centroidShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *refShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *thresholdShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *pixIdShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *pixIdMapShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *fluxShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(inShmName, &inShm[0]);
    daoShmShm2Img(centroidShmName, &centroidShm[0]);
    daoShmShm2Img(refShmName, &refShm[0]);
    daoShmShm2Img(thresholdShmName, &thresholdShm[0]);
    daoShmShm2Img(pixIdShmName, &pixIdShm[0]);
    daoShmShm2Img(pixIdMapShmName, &pixIdMapShm[0]);
    daoShmShm2Img(fluxShmName, &fluxShm[0]);

    int inSize = inShm[0].md[0].size[0]/2;
    int pupilSize = pixIdShm[0].md[0].size[0]*pixIdShm[0].md[0].size[1];
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
        if (daoShmWaitForSemaphoreTimeout(inShm, semNb, &timeout) != DAO_TIMEOUT)
        {
            clock_gettime(CLOCK_REALTIME, &t[2]);
            // New image, insert something here
            centroidShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;

            daoCentroidPws(inShm[0].array.F,
                           centroidShm[0].array.F,
                           refShm[0].array.F, 
                           pixIdShm[0].array.SI32,
                           pixIdMapShm[0].array.SI32, 
                           fluxShm[0].array.F, 
                           nbPix,
                           inSize,
                           pupilSize);

            daoShmImagePart2ShmFinalize(&centroidShm[0]); 

            clock_gettime(CLOCK_REALTIME, &t[1]);
            elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            compTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            compTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            printf("\rcompTime = %.3f us, fps = %8.3f Hz, %d in=[%6.3f,%6.3f,...,%6.3f], out[%6.3f, %6.3f,...,%6.3f]", compTime, 1e6/(1000*elapsedTime), 
                                                                                  inSize, (float)inShm[0].array.UI8[0],
                                                                                  (float)inShm[0].array.UI8[1],
                                                                                  (float)inShm[0].array.UI8[inSize],
                                                                                  centroidShm[0].array.F[0],
                                                                                  centroidShm[0].array.F[1],
                                                                                  centroidShm[0].array.F[2]);
            fflush(stdout);
        }
        else
        {
            printf("\r WAIT %d", cnt);
            fflush(stdout);
            cnt++;
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
                        daoInfo("Simple filter from SHM real time control\n");
                    	(void)sscanf(*argv++,"%s", inShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", centroidShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", refShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", thresholdShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", pixIdShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", pixIdMapShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", fluxShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%d", &nbPix); argc -= 1;
                        daoInfo("inShmName = %s\n", inShmName);
                        daoInfo("centroidShmName = %s\n", centroidShmName);
                        daoInfo("refShmName = %s\n", refShmName);
                        daoInfo("thresholdShmName = %s\n", thresholdShmName);
                        daoInfo("pixIdShmName = %s\n", pixIdShmName);
                        daoInfo("pixIdMapShmName = %s\n", pixIdMapShmName);
                        daoInfo("fluxShmName = %s\n", fluxShmName);
                        daoInfo("nbPix = %d\n", nbPix);
                        break;
            case 's':	
                        (void)sscanf(*argv++,"%d", &semNb); argc -= 1;
                        daoInfo("inputShm sem     : %d \n", semNb);
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
    int RT_priority = 93; //any number from 0-99
    struct sched_param schedpar;

    schedpar.sched_priority = RT_priority;
    // r = seteuid(euid_called); //This goes up to maximum privileges
    sched_setscheduler(0, SCHED_FIFO, &schedpar); //other option is SCHED_RR, might be faster
    // r = seteuid(euid_real);//Go back to normal privileges

    sArgv0 = *argv;

    DecodeArgs(argc,argv);

    return(sExit);
}
/*==========================================================================*/

