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
#include <omp.h>
#include <pthread.h>

// DAO header
#include "dao.h" 

/*==========================================================================*/
static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;

struct timespec tnow;
double tlastupdatedouble;

IMAGE *inputShm;
char inputShmName[32];
int semNb = 0;
IMAGE *matrixShm;
char matrixShmName[32];
IMAGE *outputShm;
char outputShmName[32];

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
#define MVM_PRINT_EVERY 2000     /* throttle telemetry: print once every N iterations */
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
    daoInfo("%s of " __DATE__ " at " __TIME__ "\n",sArgv0);
    daoInfo("   arguments:\n");
    daoInfo("   -h               display this message and exit\n");
    daoInfo("   -d               display program debug output\n");
    daoInfo("   -S               list of SHM (full path separated by space)\n");
    daoInfo("   -s               semaphore number\n");
    daoInfo("   -C <cpu>         pin the real-time thread to CPU core <cpu>\n");
    daoInfo("   -N <n>           BLAS thread count (0 = library default)\n");
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage (options must precede -L):\n");
    daoInfo("    daoMvM -S <input SHM> <matrix SHM> <output SHM> -s <semNb> [-C <cpu>] [-N <n>] -L\n");
    daoInfo("\n");
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

    struct timespec t[3];
    double elapsedTime, compTime;
    struct timespec timeout;

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

    unsigned long iter = 0;
    double compAccum = 0.0, fpsAccum = 0.0;

    clock_gettime(CLOCK_MONOTONIC, &t[1]);
    while (end == 0)
    {
        clock_gettime(CLOCK_REALTIME, &timeout);   // sem_timedwait deadline is CLOCK_REALTIME
        timeout.tv_sec += 1;
        if (daoShmWaitForSemaphoreTimeout(inputShm, semNb, &timeout) == DAO_TIMEOUT)
            continue;

        clock_gettime(CLOCK_MONOTONIC, &t[2]);

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
        daoShmImagePart2ShmFinalize(&outputShm[0]);

        t[0] = t[1];
        clock_gettime(CLOCK_MONOTONIC, &t[1]);
        elapsedTime  = (t[1].tv_sec - t[0].tv_sec) * 1e3;
        elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
        compTime  = (t[1].tv_sec - t[2].tv_sec) * 1e6;
        compTime += (t[1].tv_nsec - t[2].tv_nsec) / 1e3;

        // Accumulate telemetry and print only once every MVM_PRINT_EVERY frames:
        // a per-iteration fflush(stdout) is a syscall on the critical path.
        compAccum += compTime;
        fpsAccum  += (elapsedTime > 0.0) ? 1e3 / elapsedTime : 0.0;
        if (++iter % MVM_PRINT_EVERY == 0)
        {
            printf("\rcomp time = %9.3f us, fps = %8.3f Hz (avg/%d)   ",
                   compAccum / MVM_PRINT_EVERY, fpsAccum / MVM_PRINT_EVERY, MVM_PRINT_EVERY);
            fflush(stdout);
            compAccum = 0.0;
            fpsAccum  = 0.0;
        }
    }

    daoInfo("EXITING MAIN LOOP\n");
    fflush(stdout);

    return DAO_SUCCESS;
}
    
/*--------------------------------------------------------------------------*/
static int realTimeLoopPrep()
{
    int status;
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    inputShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(inputShmName, &inputShm[0]);
    matrixShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(matrixShmName, &matrixShm[0]);
    outputShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(outputShmName, &outputShm[0]);

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
                        (void)sscanf(*argv++,"%s", inputShmName); argc -= 1;
                        (void)sscanf(*argv++,"%s", matrixShmName); argc -= 1;
                        (void)sscanf(*argv++,"%s", outputShmName); argc -= 1;
                        daoInfo("inputShm       = %s \n", inputShmName);
                        daoInfo("matrixShm      = %s \n", matrixShmName);
                        daoInfo("outputShm      = %s \n", outputShmName);
                        break;
            case 's':
                        (void)sscanf(*argv++,"%d", &semNb); argc -= 1;
                        daoInfo("inputShm sem   = %d \n", semNb);
                        break;
            case 'C':
                        (void)sscanf(*argv++,"%d", &rtCpu); argc -= 1;
                        daoInfo("RT thread CPU  = %d \n", rtCpu);
                        break;
            case 'N':
                        (void)sscanf(*argv++,"%d", &blasThreads); argc -= 1;
                        daoInfo("BLAS threads   = %d \n", blasThreads);
                        break;
            case 'L':
                        daoInfo("MVM real time control\n");
                        realTimeLoopPrep();
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

    // Lock the address space in RAM: a page fault inside the loop is unbounded jitter.
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        daoError("mlockall failed (RT jitter may increase)\n");

    sArgv0 = *argv;

    DecodeArgs(argc,argv);

    return(sExit);
}
/*==========================================================================*/




