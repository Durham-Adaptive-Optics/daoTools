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
static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;

struct timespec tnow;
double tlastupdatedouble;

char clockName[DAO_SHM_NAME_LEN];
char freqName[DAO_SHM_NAME_LEN];
float frequency;


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
    printf("   -S               list of SHM (full path separated by space)\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   -S <clock SHM> <freq SHM> <frequency> -L\n");
    printf("\n");
}
/*--------------------------------------------------------------------------*/
void * clockRealTimeLoop(void *thread_data)
{
    daoInfo("ThreadId=%p\n", thread_data);
    IMAGE *shm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *shmFreq = (IMAGE*) malloc(sizeof(IMAGE));
    // Create size array, using 2D of 1x1... can be change to 1D
    uint32_t size[2];
    size[0] = 1;
    size[1] = 1;
    // Create SHM
    daoShmCreate(shm, clockName, 2, size, _DATATYPE_UINT32, 1, 0);
    daoShmCreate(shmFreq, freqName, 2, size, _DATATYPE_FLOAT, 1, 0);
    shmFreq[0].array.F[0] = frequency;
    // MAIN LOOP
    daoInfo("ENTERING LOOP\n");
    unsigned int clock[1] = {0};
    daoInfo("clock @ %.3f Hz\n", shmFreq[0].array.F[0]);
    // the next tick, on an absolute schedule (no drift from the time spent in between)
    struct timespec tc;
    clock_gettime(CLOCK_MONOTONIC, &tc);
    float freq = frequency;
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end==0) 
    {
        if (shmFreq[0].array.F[0] > 0.0f)       // the frequency SHM can change it; ignore 0 or less
        {
            freq = shmFreq[0].array.F[0];
        }
        float pauseTime = 1e9 / freq;          // as before: in float
        tc.tv_nsec += pauseTime;
        if (tc.tv_nsec >= 1000000000L) 
        {
            tc.tv_sec += tc.tv_nsec / 1000000000L;
            tc.tv_nsec = tc.tv_nsec % 1000000000L;
        }
        // Delay until the next timestamp
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &tc, NULL);
        daoToolsLoopStatusStart(&status);
        clock[0]++;
        shm[0].md[0].cnt2++; 
        daoShmSetData(&shm[0], (unsigned int*)clock, 1);
        daoToolsLoopStatusEnd(&status, ", target %.1f Hz", freq);
    }

    printf("\n");
    daoInfo("EXITING MAIN LOOP\n");
    daoToolsShmRelease(&shm);
    daoToolsShmRelease(&shmFreq);
    return NULL;
}
    
/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    int status;
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session


    clock_t launch, done;
    double diff;
    launch=clock();
    status=1; 
    usleep(1000);
    done=clock();
    diff = (double)(done - launch) / CLOCKS_PER_SEC;
    daoInfo("\n%ld, %ld, %ld\n",done, launch, CLOCKS_PER_SEC);
    daoInfo("clock init status = %d, init time=%.3f\n", status, diff);
    fflush(stdout);

    // Thread
    pthread_t controllerThread;
    int threadIdCtrl = 0;
    int camThreadVal=0;
    camThreadVal = pthread_create(&controllerThread, NULL, clockRealTimeLoop, (void *)&threadIdCtrl);
    if (camThreadVal != 0)
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
                        daoInfo("Clock real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, clockName, sizeof clockName);
                        daoToolsArgNameNext(&argc, &argv, str, freqName, sizeof freqName);
                        frequency = daoToolsArgDouble(&argc, &argv, str);
                        if (frequency <= 0.0f)
                        {
                            daoError("%s: the frequency must be positive\n", str);
                            exit(2);
                        }
                        daoInfo("%s \n", clockName);
                        daoInfo("%s \n", freqName);
                        daoInfo("%f \n", frequency);
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



