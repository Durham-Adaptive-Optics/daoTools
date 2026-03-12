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
    daoInfo("   -S               list of SHM (full path separated by space)\n");
    daoInfo("   -s               semaphore number\n");
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("    daoMvM -S <input SHM> <input SHM semNb> <matrix SHM> <output SHM> -s <semNb> -L\n");
    daoInfo("\n");
}
/*--------------------------------------------------------------------------*/
void * realTimeLoop(void *thread_data)
{
    daoInfo("ThreadId=%p\n", thread_data);
    // MAIN LOOP
    daoInfo("ENTERING LOOP\n");
    fflush(stdout);
    struct timespec t[3];
    double elapsedTime, compTime;
    struct timespec timeout;
    timeout.tv_sec = 1; // 1 second timeout
    int nInputs = matrixShm[0].md[0].size[1];
    int nOutputs = matrixShm[0].md[0].size[0];
    daoInfo("nInputs = %d, nOutputs = %d\n", nInputs, nOutputs);
    clock_gettime(CLOCK_REALTIME, &t[1]);
    float alpha=1.0;
    float beta=0.0;
    while (end==0) 
    {
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec +=1;
        if (daoShmWaitForSemaphoreTimeout(inputShm, semNb, &timeout) != -1)
        {
            printf("\rcomputing output, ");        
            clock_gettime(CLOCK_REALTIME, &t[2]);
            // MATRIX 
            if (inputShm[0].md[0].atype == _DATATYPE_FLOAT)
            {
                cblas_sgemv(CblasRowMajor, CblasNoTrans, nInputs, nOutputs, alpha,
                    matrixShm[0].array.F, nInputs, inputShm[0].array.F, 1, beta, outputShm[0].array.F, 1);
            }
            else
            {
                cblas_dgemv(CblasRowMajor, CblasNoTrans, nInputs, nOutputs, (double)alpha,
                    matrixShm[0].array.D, nInputs, inputShm[0].array.D, 1, (double)beta, outputShm[0].array.D, 1);

            }
            // Writes output output
            daoShmImagePart2ShmFinalize(&outputShm[0]);

            t[0]=t[1];        
            clock_gettime(CLOCK_REALTIME, &t[1]);
            elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;   
            elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            compTime = (t[1].tv_sec - t[2].tv_sec) * 1e6;
            compTime += (t[1].tv_nsec - t[2].tv_nsec) / 1e3;
            printf("comp time = %9.3f ms, fps = %8.3f Hz,", compTime, 1e6/(1000*elapsedTime));
            fflush(stdout);
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

    sArgv0 = *argv;

    DecodeArgs(argc,argv);

    return(sExit);
}
/*==========================================================================*/




