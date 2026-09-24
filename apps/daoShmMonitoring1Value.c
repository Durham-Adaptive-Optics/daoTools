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
#include <termios.h>
#include <pthread.h>
#include <ncurses.h>

// DAO header
#include "dao.h" 
#include "daoTools.h"

typedef int bool_t;
#ifndef TRUE
#define TRUE	1
#endif
#ifndef FALSE
#define FALSE	0
#endif

/*==========================================================================*/
static int	sNdx=0;							/* board index */
static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;


struct timespec tnow;
double tlastupdatedouble;

#define NB_MAX_SHM 10

IMAGE shm[NB_MAX_SHM];
char shmName[NB_MAX_SHM][32];
int nbShm;
float frequency;

// Thread
pthread_t controllerThread;

// termination flag
static int end     = 0;

// termination function for SIGINT callback
static void endme(int _a)
{
    (void)_a;
    end = 1;
}

static char	*sArgv0=NULL;					/* name of executable */

static void ShowHelp(void)
{
    daoInfo("%s of " __DATE__ " at " __TIME__ "\n",sArgv0);
    daoInfo("   arguments:\n");
    daoInfo("   -h               display this message and exit\n");
    daoInfo("   -d               display program debug output\n");
    daoInfo("   -l str           display str in output\n");
    /*
     **	Post init tests
     */
    daoInfo("   -L Nb            real time control loop: example daoShmMonitoring -L shm 25\n");
    /*
     **	Timing tests
     */
    daoInfo("   -t nloops        test timing for i/o\n");
    daoInfo("\n");
}
/*--------------------------------------------------------------------------*/
void * displayRealTimeLoop(void *thread_data)
{
    daoInfo("ThreadId=%p\n", thread_data);
    // MAIN LOOP
    daoInfo("ENTERING LOOP\n");
    fflush(stdout);
    struct timespec t[2];
    clock_gettime(CLOCK_REALTIME, &t[1]);
    float pauseTime;
    pauseTime = 1e6/frequency-50;
    int shmCnt=0;
    // timing emulation there is a small offset of about 50 us...
    // probalby due to the usleep function... not very accurate.
    WINDOW * mainwin;
    mainwin = initscr();
    /*  Initialize ncurses  */
    if ( mainwin  == NULL ) 
    {
	    daoError("Error initialising ncurses.\n");
        return (void *)DAO_ERROR;
    }
    while (end==0) 
    {
        usleep(pauseTime);
        mvprintw(0, 0, "\n"); 
        for (shmCnt = 0; shmCnt<nbShm; shmCnt++)
        {
            printw("Shared Memory monitoring: %s/%s.im.shm\n", SHAREDMEMDIR, shm[shmCnt].md[0].name);
            printw("-------------------------------------------------------------------\n"); 
            printw("naxis       %d\n", shm[shmCnt].md[0].naxis); 
            printw("size        %d, %d, %d\n", shm[shmCnt].md[0].size[0], shm[shmCnt].md[0].size[1], shm[shmCnt].md[0].size[2]); 
            printw("nelement    %ld\n", shm[shmCnt].md[0].nelement); 
            printw("atype       %d\n", shm[shmCnt].md[0].atype); 
            printw("cnt1        %ld\n", shm[shmCnt].md[0].cnt1); 
            printw("cnt2        %ld\n", shm[shmCnt].md[0].cnt2); 
            printw("timestamp   %ld\n", shm[shmCnt].md[0].atime.tsfixed.secondlong); 
            printw("\n"); 
            printw("VALUE =     %10.3f\n", shm[shmCnt].array.F[0]); 
            printw("-------------------------------------------------------------------\n"); 
            printw("\n"); 
        }
        refresh();
        t[0]=t[1];
        clock_gettime(CLOCK_REALTIME, &t[1]);
    }
    endwin();

    daoInfo("EXITING MAIN LOOP\n");
    fflush(stdout);

    return DAO_SUCCESS;
}
    
/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    int status;
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);
    int shmCnt=0l;

    for (shmCnt = 0; shmCnt < nbShm; shmCnt++)
    {
        daoShmOpen(shmName[shmCnt], &shm[shmCnt]);
    }

    clock_t launch, done;
    double diff;
    launch=clock();
    status=1; 
    usleep(1000);
    done=clock();
    diff = (double)(done - launch) / CLOCKS_PER_SEC;
    daoInfo("\n%ld, %ld, %ld\n",done, launch, CLOCKS_PER_SEC);
    daoInfo("SHM monitoring init status = %d, init time=%.3f\n", status, diff);
    fflush(stdout);

    int shmThreadVal=0;
    int threadIdCtrl = 0;
    shmThreadVal = pthread_create(&controllerThread, NULL, displayRealTimeLoop, (void *)&threadIdCtrl);
    if (shmThreadVal != 0)
    {
        daoError("Cannot create thread, err\n");
        return DAO_ERROR;
    }
    pthread_join(controllerThread, NULL);
    return DAO_SUCCESS;
}

static void DecodeArgs(int argc, char **argv)
    /*
     **	Parse the input arguments.
     */
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
            case 'L':
                        daoInfo("shm real time control\n");
                        (void)sscanf(*argv++,"%d", &nbShm);
                        daoInfo("Nb SHM = %d\n", nbShm);
                        for (int shmCnt=0; shmCnt<nbShm; shmCnt++)
                        {
                            (void)sscanf(*argv++,"%s", shmName[shmCnt]);
                            daoInfo("SHM %d = %s \n", shmCnt, shmName[shmCnt]);
                        }
                        (void)sscanf(*argv++,"%f", &frequency);
                        daoInfo("%f \n", frequency);
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



