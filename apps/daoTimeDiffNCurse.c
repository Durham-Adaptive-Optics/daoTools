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

char shm0Name[DAO_SHM_NAME_LEN];
char shm1Name[DAO_SHM_NAME_LEN];
char latencyShmName[DAO_SHM_NAME_LEN];
int sem0 = DAO_SEM_AUTO;    // <sem1> <sem2>: fixed semaphores; default: one of its own
int sem1 = DAO_SEM_AUTO;

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
    printf("%s of " __DATE__ " at " __TIME__ "\n", sArgv0);
    printf("   arguments:\n");
    printf("   -h               display this message and exit\n");
    printf("   -d <level>       log level: 0 warnings and errors (default), 1 info, 2 debug, 3 trace\n");
    printf("   -S               list of SHM (full path separated by space)\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("    daoTimeDiff -S <SHM1> <SHM2> [<sem1> <sem2>] <measurement SHM> -L\n");
    printf("    (sem1 sem2: fixed semaphores; default: one of its own)\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    daoInfo("Starting loop, %s/%s \n",shm0Name, shm1Name);
    fflush(stdout);
    shm0 = (IMAGE*) malloc(sizeof(IMAGE));
    shm1 = (IMAGE*) malloc(sizeof(IMAGE));
    latencyShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(shm0Name, &shm0[0]);
    daoToolsShmOpen(shm1Name, &shm1[0]);
    // Create size array, using 2D of 1x1... can be change to 1D
    uint32_t size[2];
    size[0] = 1;
    size[1] = 1;
    daoShmCreate(latencyShm, latencyShmName, 2, size, _DATATYPE_FLOAT, 1, 0);

    WINDOW * mainwin;
    mainwin = initscr();
    /*  Initialize ncurses  */
    if ( mainwin  == NULL ) 
    {
	    daoError("Error initialising ncurses.\n");
        return DAO_ERROR;
    }

    int64_t elapsedTimeNs = 0;
    int64_t frameId0 = 0;
    int64_t frameId1 = 0;
    float latency[1] = {0};
    int nbNegTs=0;
    int validFrames=0;
    struct timespec t0 = {0, 0}, t1 = {0, 0}, now, lastDraw = {0, 0};
    int timedOut = 0;
    while (end ==0)
    {
        // wait for 2nd shm
        timedOut = (daoToolsWait(shm1, sem1, 1.0) != DAO_SUCCESS);
        if (!timedOut)
        {
            // the full timestamps (seconds and nanoseconds): a difference across
            // a second boundary is right too
            t0 = shm0[0].md[0].atime.ts;
            t1 = shm1[0].md[0].atime.ts;
            frameId0 = daoShmGetCounter(shm0);
            frameId1 = daoShmGetCounter(shm1);
            elapsedTimeNs = (int64_t)(t1.tv_sec - t0.tv_sec) * 1000000000LL + (int64_t)(t1.tv_nsec - t0.tv_nsec);
            latency[0] = (float)elapsedTimeNs / 1e3;
            if (elapsedTimeNs < 0)
            {
                nbNegTs++;
            }
            else
            {
                validFrames++;
                daoShmSetData(&latencyShm[0], (float *)latency, 1);
            }
        }
        // the screen, once per second (and at each timeout)
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!timedOut && (now.tv_sec - lastDraw.tv_sec) + (now.tv_nsec - lastDraw.tv_nsec) * 1e-9 < 1.0)
        {
            continue;
        }
        lastDraw = now;
        erase();
        mvprintw(0, 0, "Measure timing script between\n");
        printw("SHM0 %s\n", shm0Name);
        printw("SHM1 %s\n", shm1Name);
        printw(" -> %s\n", latencyShmName);
        if (timedOut)
        {
            printw("Timeout: no frame of shm1 for 1 s\n");
        }
        else
        {
            printw("Synchronized True\n");
        }
        printw("Negative TS = %d\n", nbNegTs);
        printw("Valid Frames = %d\n", validFrames);
        printw("frame ID SHM0 = %ld\n", (long)frameId0);
        printw("frame ID SHM1 = %ld\n", (long)frameId1);
        printw(" -> frame ID diff: %ld\n", (long)(frameId1 - frameId0));
        printw("timeStamp SHM0 = %ld.%09ld\n", (long)t0.tv_sec, (long)t0.tv_nsec);
        printw("timeStamp SHM1 = %ld.%09ld\n", (long)t1.tv_sec, (long)t1.tv_nsec);
        printw(" -> time difference ns = %8ld\n", (long)elapsedTimeNs);
        printw(" -> time difference us = %8.3f\n", (double)elapsedTimeNs / 1e3);
        refresh();
    }
    endwin();
    daoInfo("EXITING MAIN LOOP\n");
    fflush(stdout);

    return 0;
}

/**
 *	Parse the input arguments.
 */
/* Is s a whole (signed) integer? */
static int isInteger(const char *s)
{
    char *end;
    if (!s || !*s)
        return 0;
    (void) strtol(s, &end, 10);
    return *end == '\0';
}

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
                daoLogLevel = daoToolsArgInt(&argc, &argv, str);
                break;
            case 'l':
                daoInfo("%s\n", daoToolsArgValue(&argc, &argv, str));
                break;
            case 'u':
                a1 = daoToolsArgInt(&argc, &argv, str);
                daoDebug("will sleep for %d usec\n", a1);
                (void)usleep(a1);
                break;
            case 'S':
                        daoInfo("Simple Camera Reader and Writer from SHM real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, shm0Name, sizeof shm0Name);
                        daoToolsArgNameNext(&argc, &argv, str, shm1Name, sizeof shm1Name);
                        if (argc > 0 && isInteger(*argv)) {   /* older command lines: fixed semaphores */
                        {
                            sem0 = daoToolsArgInt(&argc, &argv, str);
                        }
                            sem1 = daoToolsArgInt(&argc, &argv, str);
                        }
                        daoToolsArgNameNext(&argc, &argv, str, latencyShmName, sizeof latencyShmName);
                        break;
            case 'L':
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

