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

double dt_update; // time since last update
double dt_update_lim = 3600.0; // if no command is received during this time, set DM to zero V [sec]

char inShmName[32];
char outShmName[32];
char fcShmName[32];
char fpsShmName[32];
char enaShmName[32];
int semNb=0;

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
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("   daoHighPassFilter -S <in SHM> <out SHM> <fc SHM> <fps SHM> <ena SHM> -s <semNb> -L\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    uint32_t size[2];
    IMAGE *inShm;
    IMAGE *inShmPrev;
    IMAGE *outShm;
    IMAGE *outShmPrev;

    IMAGE *fcShm;
    IMAGE *fpsShm;
    IMAGE *enaShm;

    fcShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(fcShmName, &fcShm[0]);
    fpsShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(fpsShmName, &fpsShm[0]);
    enaShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(enaShmName, &enaShm[0]);

    char inShmNamePrev[32];
    char outShmNamePrev[32];

    inShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(inShmName, &inShm[0]);
    // Create Prev SHM
    inShmPrev = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsInsertShmNamePrefix(inShmName, "Prev", inShmNamePrev);
    size[0] = inShm[0].md[0].size[0];
    size[1] = inShm[0].md[0].size[1];
    daoShmImageCreate(inShmPrev, inShmNamePrev, 2, size, inShm[0].md[0].atype, 1, 0);

    outShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(outShmName, &outShm[0]);
    // Create Prev SHM
    outShmPrev = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsInsertShmNamePrefix(outShmName, "Prev", outShmNamePrev);
    size[0] = inShm[0].md[0].size[0];
    size[1] = inShm[0].md[0].size[1];
    daoShmImageCreate(outShmPrev, inShmNamePrev, 2, size, inShm[0].md[0].atype, 1, 0);

    printf("Starting loop, (%s,%s) -> (%s,%s)\n",
           inShmName, inShmNamePrev, outShmName, outShmNamePrev);

    fflush(stdout);

    int inSize = inShm[0].md[0].size[0]*inShm[0].md[0].size[1];

    struct timespec t[3];
    struct timespec timeout;
    double elapsedTime;
    double compTime;
    int cnt=0;
    clock_gettime(CLOCK_REALTIME, &t[1]);
    while (end ==0)
    {
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        // Wait for new image
        if (daoShmWaitForSemaphoreTimeout(inShm, semNb, &timeout) != DAO_TIMEOUT)
        {
            clock_gettime(CLOCK_REALTIME, &t[2]);
            
            if (enaShm[0].array.UI32[0] == 1)
            {
                if (inShm[0].md[0].atype == _DATATYPE_DOUBLE)
                {
                    daoToolsHighPassFilterDouble(outShm[0].array.D,
                                                 inShm[0].array.D,  
                                                 outShmPrev[0].array.D,  
                                                 inShmPrev[0].array.D,  
                                                 fcShm[0].array.D[0],
                                                 fpsShm[0].array.D[0],
                                                 inSize);
                }
                else
                {
                    daoToolsHighPassFilter(outShm[0].array.F,
                                           inShm[0].array.F,  
                                           outShmPrev[0].array.F,  
                                           inShmPrev[0].array.F,  
                                           fcShm[0].array.F[0],
                                           fpsShm[0].array.F[0],
                                           inSize);
                }
            }
            else
            {
                if (inShm[0].md[0].atype == _DATATYPE_DOUBLE)
                {
                    daoShmImage2Shm(inShm[0].array.D, inSize, outShm);
                }
                else
                {
                    daoShmImage2Shm(inShm[0].array.F, inSize, outShm);
                }
            }

            daoShmImagePart2ShmFinalize(&outShm[0]);
            
            // After the call, update previous frame buffers
            for (int i = 0; i < inSize; ++i) 
            {
                outShmPrev[0].array.D[i] = outShm[0].array.D[i];
                inShmPrev[0].array.D[i] = inShm[0].array.D[i];
            }
            t[0]=t[1];        
            clock_gettime(CLOCK_REALTIME, &t[1]);
            elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            compTime = (t[1].tv_sec - t[2].tv_sec) * 1e3;
            compTime += (t[1].tv_nsec - t[2].tv_nsec) / 1e6;
            printf("\r HPF enable = %d, compTime = %.3f us, fps = %8.3f Hz", enaShm[0].array.UI32[0], compTime, 1e6/(1000*elapsedTime));
        }
        else
        {
            printf("\r WAIT %d", cnt);
            cnt++;
        }
        fflush(stdout);
    }


    printf("EXITING MAIN LOOP\n");
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
                        (void)sscanf(*argv++,"%d",&daoLogLevel); argc -= 1;
                        break;
            case 'l':
                        daoInfo("%s\n",*argv);
                        argv += 1; 
                        argc -= 1;
                        break;
            case 'u':
                        (void)sscanf(*argv++,"%d",&a1); argc -= 1;
                        daoDebug("will sleep for %d usec\n",a1);
                        (void)usleep(a1);
                        break;
            case 'S':
                        (void)sscanf(*argv++,"%s", inShmName);
                        (void)sscanf(*argv++,"%s", outShmName);
                        (void)sscanf(*argv++,"%s", fcShmName);
                        (void)sscanf(*argv++,"%s", fpsShmName);
                        (void)sscanf(*argv++,"%s", enaShmName);
                        daoInfo("inShmName          = %s\n", inShmName);
                        daoInfo("outShmName         = %s\n", outShmName);
                        daoInfo("fcShmName          = %s\n", fcShmName);
                        daoInfo("fpsShmName         = %s\n", fpsShmName);
                        daoInfo("enaShmName         = %s\n", enaShmName);
                        break;
            case 's':	
                        (void)sscanf(*argv++,"%d", &semNb); argc -= 1;
                        daoInfo("inputShm sem       = %d \n", semNb);
                        break;
            case 'L':
                        printf("HPF real time control\n");
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
