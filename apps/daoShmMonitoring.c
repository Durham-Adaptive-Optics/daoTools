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

IMAGE *shm;
char shmName[DAO_SHM_NAME_LEN];
float frequency;

// Thread
pthread_t controllerThread;
int threadIdCtrl = 0;

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
    printf("%s of " __DATE__ " at " __TIME__ "\n", sArgv0);
    printf("   arguments:\n");
    printf("   -h               display this message and exit\n");
    printf("   -d <level>       log level: 0 warnings and errors (default), 1 info, 2 debug, 3 trace\n");
    printf("   -l str           display str in output\n");
    printf("   -L <SHM> <Hz>    display the metadata of <SHM>, <Hz> times per second\n");
    printf("   usage:\n");
    printf("   -L /tmp/cam.im.shm 25\n");
    printf("\n");
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
    // the display rate (-L); the ~50 us left for drawing
    useconds_t pauseTime = (useconds_t)(1e6 / frequency > 50.0 ? 1e6 / frequency - 50.0 : 0.0);
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
        mvprintw(0, 0, "Shared Memory monitoring: %s/%s.im.shm\n", SHAREDMEMDIR, shm[0].md[0].name);
        printw("-------------------------------------------------------------------\n"); 
        //printw("pointer     %p\n", shm[0].array); 
        printw("naxis       %d\n", shm[0].md[0].naxis); 
        printw("size        %d, %d, %d\n", shm[0].md[0].size[0], shm[0].md[0].size[1], shm[0].md[0].size[2]); 
        printw("nelement    %ld\n", shm[0].md[0].nelement); 
        printw("atype       %d\n", shm[0].md[0].atype); 
        printw("cnt0        %ld\n", shm[0].md[0].cnt0); 
        printw("cnt1        %ld\n", shm[0].md[0].cnt1); 
        printw("cnt2        %ld\n", shm[0].md[0].cnt2);
        printw("timestamp   %ld.%09ld\n", (long)shm[0].md[0].atime.ts.tv_sec, (long)shm[0].md[0].atime.ts.tv_nsec);
        printw("-------------------------------------------------------------------\n"); 
        refresh();
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
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    shm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(shmName, &shm[0]);

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

    int camThreadVal=0;
    camThreadVal = pthread_create(&controllerThread, NULL, displayRealTimeLoop, (void *)&threadIdCtrl);
    if (camThreadVal != 0)
    {
        daoError("Cannot create thread, err\n");
        return DAO_ERROR;
    }
    pthread_join(controllerThread, NULL);
    daoToolsShmRelease(&shm);
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
                daoLogLevel = daoToolsArgInt(&argc, &argv, str);
                break;
            case 'l':
                daoInfo("%s\n", daoToolsArgValue(&argc, &argv, str));
                break;

            case 'b':
                sNdx = daoToolsArgInt(&argc, &argv, str);
                break;
            case 'u':
                a1 = daoToolsArgInt(&argc, &argv, str);
                daoDebug("will sleep for %d usec\n", a1);
                (void)usleep(a1);
                break;
            case 'L':
                        daoInfo("CAM real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, shmName, sizeof shmName);
                        frequency = daoToolsArgDouble(&argc, &argv, str);
                        if (frequency <= 0.0)
                        {
                            daoError("-L: the display rate must be positive\n");
                            exit(2);
                        }
                        daoInfo("%s \n", shmName);
                        daoInfo("%f \n", frequency);
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



