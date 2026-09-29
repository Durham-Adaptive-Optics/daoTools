/*****************************************************************************
  DAO project
  s.cetre
 *****************************************************************************/

/*==========================================================================*/
#define _GNU_SOURCE
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
#include <gsl/gsl_blas.h>
#include <pthread.h>

// DAO header
#include "dao.h"
#include "daoTools.h" 

/*==========================================================================*/
static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;

struct timespec tnow;
double tlastupdatedouble;

IMAGE *inputShm;
char inputShmName[DAO_SHM_NAME_LEN];
int semNb = DAO_SEM_AUTO;   // -s: a fixed semaphore; default: one of its own
IMAGE *matrixShm;
char matrixShmName[DAO_SHM_NAME_LEN];
IMAGE *outputShm;
char outputShmName[DAO_SHM_NAME_LEN];

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

/*--------------------------------------------------------------------------*/
/* Real-time tuning knobs                                                    */
static int rtCpu       = -1;     /* CPU core to pin the RT thread to (-1 = do not pin) */
static int blasThreads = 0;      /* BLAS thread count (0 = leave library default)     */

/* OpenBLAS runtime thread control (no public header is pulled in here). */
extern void openblas_set_num_threads(int num_threads);

static void mvmSetRtAffinity(int cpu)
{
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
        daoError("pthread_setaffinity_np(cpu=%d) failed\n", cpu);
    else
        daoInfo("RT thread pinned to CPU %d\n", cpu);
#else
    daoInfo("CPU affinity not supported on this platform (cpu=%d ignored)\n", cpu);
#endif
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
    printf("   -s <semNb>       a fixed semaphore to wait on (default: one of its own)\n");
    printf("   -C <cpu>         pin the real-time thread to CPU core <cpu>\n");
    printf("   -N <n>           BLAS thread count (0 = library default)\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage (options must precede -L):\n");
    printf("    daoMvM -S <input SHM> <matrix SHM> <output SHM> [-s <semNb>] [-C <cpu>] [-N <n>] -L\n");
    printf("\n");
}
/*--------------------------------------------------------------------------*/
void * realTimeLoop(void *thread_data)
{
    daoInfo("ThreadId=%p\n", thread_data);

    if (rtCpu >= 0)
        mvmSetRtAffinity(rtCpu);

    if (blasThreads > 0)
    {
        openblas_set_num_threads(blasThreads);
        daoInfo("BLAS threads set to %d\n", blasThreads);
    }

    // MAIN LOOP
    daoInfo("ENTERING LOOP\n");
    fflush(stdout);


    int nInputs  = matrixShm[0].md[0].size[1];
    int nOutputs = matrixShm[0].md[0].size[0];
    daoInfo("nInputs = %d, nOutputs = %d\n", nInputs, nOutputs);

    const int    isFloat = (inputShm[0].md[0].atype == _DATATYPE_FLOAT);
    const float  alpha_f = 1.0f, beta_f = 0.0f;
    const double alpha_d = 1.0,  beta_d = 0.0;

    // Fault in the matrix pages and warm up the BLAS call so the first real
    // iterations don't pay page-fault / lazy-init jitter.
    if (isFloat)
    {
        volatile float acc = 0.0f;
        for (size_t i = 0; i < (size_t)nInputs * nOutputs; i += 1024)
            acc += matrixShm[0].array.F[i];
        (void)acc;
        cblas_sgemv(CblasRowMajor, CblasNoTrans, nOutputs, nInputs, alpha_f,
                    matrixShm[0].array.F, nInputs, inputShm[0].array.F, 1,
                    beta_f, outputShm[0].array.F, 1);
    }
    else
    {
        volatile double acc = 0.0;
        for (size_t i = 0; i < (size_t)nInputs * nOutputs; i += 1024)
            acc += matrixShm[0].array.D[i];
        (void)acc;
        cblas_dgemv(CblasRowMajor, CblasNoTrans, nOutputs, nInputs, alpha_d,
                    matrixShm[0].array.D, nInputs, inputShm[0].array.D, 1,
                    beta_d, outputShm[0].array.D, 1);
    }

    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end == 0)
    {
        if (daoToolsWait(inputShm, semNb, 1.0) != DAO_SUCCESS)
        {
            daoToolsLoopStatusWait(&status);
            continue;
        }
        daoToolsLoopStatusStart(&status);
        // y = M x   with M row-major [nOutputs x nInputs], lda = nInputs
        if (isFloat)
            cblas_sgemv(CblasRowMajor, CblasNoTrans, nOutputs, nInputs, alpha_f,
                        matrixShm[0].array.F, nInputs,
                        inputShm[0].array.F, 1,
                        beta_f, outputShm[0].array.F, 1);
        else
            cblas_dgemv(CblasRowMajor, CblasNoTrans, nOutputs, nInputs, alpha_d,
                        matrixShm[0].array.D, nInputs,
                        inputShm[0].array.D, 1,
                        beta_d, outputShm[0].array.D, 1);
        // Publish the output.
        daoShmSetDataPartFinalize(&outputShm[0]);
        daoToolsLoopStatusEnd(&status, NULL);
    }
    printf("\n");
    daoInfo("EXITING MAIN LOOP\n");
    return NULL;
}
    
/*--------------------------------------------------------------------------*/
static int realTimeLoopPrep()
{
    int status;
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    inputShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(inputShmName, &inputShm[0]);
    matrixShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(matrixShmName, &matrixShm[0]);
    outputShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(outputShmName, &outputShm[0]);
    // y = M x: M is [nOutputs x nInputs] (size[0] x size[1]), all three float or all double
    int atype = matrixShm[0].md[0].atype;
    if (atype != _DATATYPE_FLOAT && atype != _DATATYPE_DOUBLE)
    {
        daoError("%s: float or double only\n", matrixShmName);
        exit(EXIT_FAILURE);
    }
    long nIn = matrixShm[0].md[0].size[1], nOut = matrixShm[0].md[0].size[0];
    daoToolsShmCheck(matrixShm, matrixShmName, atype, nIn * nOut);
    daoToolsShmCheck(inputShm, inputShmName, atype, nIn);
    daoToolsShmCheck(outputShm, outputShmName, atype, nOut);

    clock_t launch, done;
    double diff;
    launch=clock();
    status=1; 
    usleep(1000);
    done=clock();
    diff = (double)(done - launch) / CLOCKS_PER_SEC;
    daoInfo("\n%ld, %ld, %ld\n",done, launch, CLOCKS_PER_SEC);
    daoInfo("init status = %d, init time=%.3f\n", status, diff);
    fflush(stdout);

    int threadVal=0;
    threadVal = pthread_create(&controllerThread, NULL,
                               realTimeLoop, (void*)&threadIdCtrl);
    if (threadVal != 0)
    {
        daoError("Cannot create thread, err\n");
        return DAO_ERROR;
    }
    pthread_join(controllerThread, NULL);
    daoToolsShmRelease(&inputShm);
    daoToolsShmRelease(&matrixShm);
    daoToolsShmRelease(&outputShm);
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
                daoToolsArgNameNext(&argc, &argv, str, inputShmName, sizeof inputShmName);
                daoToolsArgNameNext(&argc, &argv, str, matrixShmName, sizeof matrixShmName);
                daoToolsArgNameNext(&argc, &argv, str, outputShmName, sizeof outputShmName);
                daoInfo("inputShm       = %s \n", inputShmName);
                daoInfo("matrixShm      = %s \n", matrixShmName);
                daoInfo("outputShm      = %s \n", outputShmName);
                break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                daoInfo("inputShm sem   = %d \n", semNb);
                break;
            case 'C':
                rtCpu = daoToolsArgInt(&argc, &argv, str);
                daoInfo("RT thread CPU  = %d \n", rtCpu);
                break;
            case 'N':
                blasThreads = daoToolsArgInt(&argc, &argv, str);
                daoInfo("BLAS threads   = %d \n", blasThreads);
                break;
            case 'L':
                        daoInfo("MVM real time control\n");
                        if (realTimeLoopPrep() != 0)         /* could not start, or failed (see above) */
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

    // Lock the address space in RAM: a page fault inside the loop is unbounded jitter.
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        daoWarning("mlockall failed: run scripts/daoToolSetCap to grant RT capabilities. Continuing, but not optimized for real-time.\n");

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




