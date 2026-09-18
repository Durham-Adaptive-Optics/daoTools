/*****************************************************************************
  Pyramid WFS project
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
#include <ncurses.h>

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

IMAGE *shm0;
IMAGE *shm1;
IMAGE *latencyShm;

char shm0Name[32];
char shm1Name[32];
char latencyShmName[32];
int sem0;
int sem1;

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
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("    daoTimeDiff -S <SHM1> <SHM2> <sem1> <sem2> <measurement SHM> -L\n");
    daoInfo("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    daoInfo("Starting loop, %s/%s \n",shm0Name, shm1Name);
    fflush(stdout);
    shm0 = (IMAGE*) malloc(sizeof(IMAGE));
    shm1 = (IMAGE*) malloc(sizeof(IMAGE));
    latencyShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(shm0Name, &shm0[0]);
    daoShmShm2Img(shm1Name, &shm1[0]);
    // Create size array, using 2D of 1x1... can be change to 1D
    uint32_t size[2];
    size[0] = 1;
    size[1] = 1;
    daoShmImageCreate(latencyShm, latencyShmName, 2, size, _DATATYPE_FLOAT, 1, 0);

    WINDOW * mainwin;
    mainwin = initscr();
    /*  Initialize ncurses  */
    if ( mainwin  == NULL ) 
    {
	    daoError("Error initialising ncurses.\n");
        return DAO_ERROR;
    }
    
    int64_t t[2];
    int64_t elapsedTimeNs;
    int64_t frameId0;
    int64_t frameId1;
    int64_t frameIdDiff;
    //int last = 0;
    float latency[1];
    struct timespec timeout;
    int missedFrameShm0=0;
    int missedFrameShm1=0;
    int nbNegTs=0;
    int validFrames=0;
    while (end ==0)
    {
        mvprintw(0, 0, "Measure timing script between\n");
        printw("SHM0 %s\n", shm0Name);
        printw("SHM1 %s\n", shm1Name);
        printw(" -> %s\n", latencyShmName);
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        // wait for 2nd shm
        if (daoShmWaitForSemaphoreTimeout(shm1, sem1, &timeout) != DAO_TIMEOUT)
        {
            t[0] = shm0[0].md[0].atime.tsfixed.secondlong;
            frameId0 = daoShmGetCounter(shm0);//shm0[0].md[0].cnt2;
            t[1] = shm1[0].md[0].atime.tsfixed.secondlong;
            frameId1 = daoShmGetCounter(shm1);//shm1[0].md[0].cnt2;
            // Check if timeout
            elapsedTimeNs = t[1] - t[0];
            latency[0] = (float)elapsedTimeNs / 1e3;
            frameIdDiff = frameId1 - frameId0;
            printw("Synchronized True\n");
            printw("Missed Frames SHM0 = %d\n", missedFrameShm0);
            printw("Missed Frames SHM1 = %d\n", missedFrameShm1);
            printw("Negative TS = %d\n", nbNegTs);
            printw("Valid Frames = %d\n", validFrames);
            if (elapsedTimeNs < 0)
            {
                nbNegTs++;
            }
            else
            {
                validFrames++;
                daoShmImage2Shm((float *)latency, 1, &latencyShm[0]);
            }
            printw("frame ID SHM0 = %ld\n", frameId0);
            printw("frame ID SHM1 = %ld\n", frameId1);
            printw(" -> frame ID diff: %ld\n", frameIdDiff);
            printw("timeStamp SHM0 = %12ld\n", t[0]);
            printw("timeStamp SHM1 = %12ld\n", t[1]);
            printw(" -> time difference ns = %8ld\n", elapsedTimeNs);
            printw(" -> time difference us = %8.3f\n", (double)elapsedTimeNs / 1e3);
            refresh();
        }
        else
        {
            printw("Timeout: shm1, error:%s\n", strerror(errno));
            refresh();
        }
    }
    endwin();
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
            case 'S':
                        daoInfo("Simple Camera Reader and Writer from SHM real time control\n");
                    	(void)sscanf(*argv++,"%s",shm0Name); argc -= 1;
                    	(void)sscanf(*argv++,"%s",shm1Name); argc -= 1;
                    	(void)sscanf(*argv++,"%d",&sem0); argc -= 1;
                    	(void)sscanf(*argv++,"%d",&sem1); argc -= 1;
                    	(void)sscanf(*argv++,"%s",latencyShmName); argc -= 1;
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

