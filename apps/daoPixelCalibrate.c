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

#include "daoShm.h"
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


char inShmName[32];
char ffShmName[32];
char bgShmName[32];
char calShmName[32];

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
    /*
     **	Post init tests
     */
    daoInfo("   -L sham              real time control loop\n");
    daoInfo("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);
    IMAGE *inShm;
    IMAGE *ffShm;
    IMAGE *bgShm;
    IMAGE *calShm;

    inShm = (IMAGE*) malloc(sizeof(IMAGE));
    ffShm = (IMAGE*) malloc(sizeof(IMAGE));
    bgShm = (IMAGE*) malloc(sizeof(IMAGE));
    calShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(inShmName, "", &inShm[0]);
    daoShmShm2Img(ffShmName, "", &ffShm[0]);
    daoShmShm2Img(bgShmName, "", &bgShm[0]);
    daoShmShm2Img(calShmName, "", &calShm[0]);

    daoInfo("Starting loop, %s/%s \n",inShmName, calShmName);
    fflush(stdout);
    int inSize = inShm[0].md[0].size[0]*inShm[0].md[0].size[1];
    int calSize = calShm[0].md[0].size[0]*calShm[0].md[0].size[1];
    struct timespec t[3];
    struct timespec timeout;
    double elapsedTime;
    double calTime;
    clock_gettime(CLOCK_REALTIME, &t[1]);

    int waitCounter = 0;
    while (end ==0)
    {
        t[0] = t[1];
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        if (sem_timedwait(inShm[0].semptr[2], &timeout) != -1)
        {
            clock_gettime(CLOCK_REALTIME, &t[2]);
            daoToolsShmCalibrate(inShm, ffShm, bgShm, calShm);


            clock_gettime(CLOCK_REALTIME, &t[1]);
            elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            calTime = (t[1].tv_sec - t[2].tv_sec) * 1e3;
            calTime += (t[1].tv_nsec - t[2].tv_nsec) / 1e6;
            printf("\rcal time = %8.3f us, fps = %8.3f Hz, %d, cal[%6.3f, %6.3f,...,%6.3f]", 
                   1000*calTime,
                   1e6 / (1000 * elapsedTime),
                   inSize, 
                   calShm[0].array.F[0],
                   calShm[0].array.F[1],
                   calShm[0].array.F[calSize]);
        }
        else
        {
            waitCounter += 1;
            printf("\rWAIT %d", waitCounter);
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
                        argv += 1; argc -= 1;
                        break;

            case 'b':	(void)sscanf(*argv++,"%d",&sNdx); argc -= 1;	break;
            case 'u':
                        (void)sscanf(*argv++,"%d",&a1); argc -= 1;
                        daoDebug("will sleep for %d usec\n",a1);
                        (void)usleep(a1);
                        break;
                        break;
            case 'L':
                        daoInfo("Apply Falt and Background from SHM real time control\n");
                    	(void)sscanf(*argv++,"%s",inShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s",ffShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s",bgShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s",calShmName); argc -= 1;
                        daoInfo("image in         : %s\n", inShmName);
                        daoInfo("flat field       : %s\n", ffShmName);
                        daoInfo("background       : %s\n", bgShmName);
                        daoInfo("calibrated image : %s\n", calShmName);
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

