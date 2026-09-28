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
int semNb = 0;
char ffShmName[DAO_SHM_NAME_LEN];
char bgShmName[DAO_SHM_NAME_LEN];
char maskShmName[DAO_SHM_NAME_LEN];
char calShmName[DAO_SHM_NAME_LEN];
char fluxShmName[DAO_SHM_NAME_LEN];

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
    daoInfo("   -S <input SHM> <background SHM> <flatfield SHM> <mask SHM> <cal SHM> <flux SHM> -s <semNb> -L\n");
    daoInfo("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    IMAGE *inShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *ffShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *bgShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *maskShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *calShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *fluxShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(inShmName, &inShm[0]);
    daoToolsShmOpen(ffShmName, &ffShm[0]);
    daoToolsShmOpen(bgShmName, &bgShm[0]);
    daoToolsShmOpen(maskShmName, &maskShm[0]);
    daoToolsShmOpen(calShmName, &calShm[0]);
    daoToolsShmOpen(fluxShmName, &fluxShm[0]);

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
    usleep(2000000);
    while (end ==0)
    {
        t[0] = t[1];
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        if (daoShmWaitSemTimeout(inShm, semNb, &timeout) != DAO_TIMEOUT)
        {
            clock_gettime(CLOCK_REALTIME, &t[2]);
            daoToolsShmCalibratePws(inShm, ffShm, bgShm, maskShm, calShm, fluxShm);

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
                    	daoToolsArgName(inShmName, sizeof inShmName, *argv++); argc -= 1;
                    	daoToolsArgName(ffShmName, sizeof ffShmName, *argv++); argc -= 1;
                    	daoToolsArgName(bgShmName, sizeof bgShmName, *argv++); argc -= 1;
                    	daoToolsArgName(maskShmName, sizeof maskShmName, *argv++); argc -= 1;
                    	daoToolsArgName(calShmName, sizeof calShmName, *argv++); argc -= 1;
                    	daoToolsArgName(fluxShmName, sizeof fluxShmName, *argv++); argc -= 1;
                        daoInfo("image in         : %s\n", inShmName);
                        daoInfo("flat field       : %s\n", ffShmName);
                        daoInfo("background       : %s\n", bgShmName);
                        daoInfo("mask             : %s\n", maskShmName);
                        daoInfo("calibrated image : %s\n", calShmName);
                        daoInfo("flux             : %s\n", fluxShmName);
                        break;
            case 's':	
                        (void)sscanf(*argv++,"%d", &semNb);
                        daoInfo("inputShm sem     : %d \n", semNb);
                        break;
            case 'L':
                        daoInfo("Apply Falt and Background from SHM of PWFS real time control\n");
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

