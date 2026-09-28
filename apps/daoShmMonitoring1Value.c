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
char shmName[NB_MAX_SHM][DAO_SHM_NAME_LEN];
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
    printf("%s of " __DATE__ " at " __TIME__ "\n", sArgv0);
    printf("   arguments:\n");
    printf("   -h               display this message and exit\n");
    printf("   -d <level>       log level: 0 warnings and errors (default), 1 info, 2 debug, 3 trace\n");
    printf("   -l str           display str in output\n");
    printf("   -L <nb> <SHM1> ... <SHMnb> <Hz>  display the metadata and first value of the nb SHMs\n");
    printf("                    (1 to 10), <Hz> times per second\n");
    printf("   usage:\n");
    printf("   -L 2 /tmp/a.im.shm /tmp/b.im.shm 25\n");
    printf("\n");
}
/*--------------------------------------------------------------------------*/
/* the first value of an SHM, whatever its data type */
static double firstValue(IMAGE *img)
{
    switch (img->md[0].atype)
    {
        case _DATATYPE_UINT8:
            return img->array.UI8[0];
        case _DATATYPE_INT8:
            return img->array.SI8[0];
        case _DATATYPE_UINT16:
            return img->array.UI16[0];
        case _DATATYPE_INT16:
            return img->array.SI16[0];
        case _DATATYPE_UINT32:
            return img->array.UI32[0];
        case _DATATYPE_INT32:
            return img->array.SI32[0];
        case _DATATYPE_UINT64:
            return (double)img->array.UI64[0];
        case _DATATYPE_INT64:
            return (double)img->array.SI64[0];
        case _DATATYPE_FLOAT:
            return img->array.F[0];
        case _DATATYPE_DOUBLE:
            return img->array.D[0];
        default:
            return NAN;
    }
}

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
            printw("timestamp   %ld.%09ld\n", (long)shm[shmCnt].md[0].atime.ts.tv_sec, (long)shm[shmCnt].md[0].atime.ts.tv_nsec);
            printw("\n");
            printw("VALUE =     %10.3f\n", firstValue(&shm[shmCnt]));
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
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session
    int shmCnt=0l;

    for (shmCnt = 0; shmCnt < nbShm; shmCnt++)
    {
        daoToolsShmOpen(shmName[shmCnt], &shm[shmCnt]);
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
    for (shmCnt = 0; shmCnt < nbShm; shmCnt++)
    {
        daoShmClose(&shm[shmCnt]);
    }
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
                        daoInfo("shm real time control\n");
                        nbShm = daoToolsArgInt(&argc, &argv, str);
                        if (nbShm < 1 || nbShm > NB_MAX_SHM)
                        {
                            daoError("-L: 1 to %d SHMs\n", NB_MAX_SHM);
                            exit(2);
                        }
                        daoInfo("Nb SHM = %d\n", nbShm);
                        for (int shmCnt=0; shmCnt<nbShm; shmCnt++)
                        {
                            daoToolsArgNameNext(&argc, &argv, str, shmName[shmCnt], sizeof shmName[shmCnt]);
                            daoInfo("SHM %d = %s \n", shmCnt, shmName[shmCnt]);
                        }
                        frequency = daoToolsArgDouble(&argc, &argv, str);
                        if (frequency <= 0.0)
                        {
                            daoError("-L: the display rate must be positive\n");
                            exit(2);
                        }
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



