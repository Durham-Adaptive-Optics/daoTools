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

double dt_update; // time since last update
double dt_update_lim = 3600.0; // if no command is received during this time, set DM to zero V [sec]


char shmName[32];
char shmNameAvg[64];
char shmNameRms[64];
int popSize=100;
int semNb = 0;

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
    daoInfo("   -n               number of frame to average\n");
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("   -S <SHM> -n <nb Frame> -s <semNb> -L\n");;
    daoInfo("\n");
}

/*--------------------------------------------------------------------------*/
void * statRealTimeLoop(void *thread_data)
{
    daoInfo("ThreadId=%p\n", thread_data);
    IMAGE *shm = (IMAGE *)malloc(sizeof(IMAGE));
    IMAGE *shmAvg = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *shmRms = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(shmName, &shm[0]);

    daoToolsInsertShmNamePrefix(shmName, "Avg", shmNameAvg);
    daoToolsInsertShmNamePrefix(shmName, "Rms", shmNameRms);
    // Create size array, using 2D of 1x1... can be change to 1D
    uint32_t size[2];
    size[0] = shm[0].md[0].size[0];
    size[1] = shm[0].md[0].size[1];
    daoShmImageCreate(shmAvg, shmNameAvg, 2, size, shm[0].md[0].atype, 1, 0);
    daoShmImageCreate(shmRms, shmNameRms, 2, size, shm[0].md[0].atype, 1, 0);

    daoInfo("Starting loop, %s -> %s, popSize=%d\n",shmName, shmNameAvg, popSize );
    daoInfo("               %s -> %s, popSize=%d\n",shmName, shmNameRms, popSize );
    fflush(stdout);

    int nbValue = shm[0].md[0].size[0]*shm[0].md[0].size[1];
    float *avgValue = malloc(nbValue*sizeof(float));
    float *rmsValue = malloc(nbValue*sizeof(float));
    float valueCircBufAvg[nbValue][popSize+1];
    float valueCircBuf[nbValue][popSize+1];
    int k, l;
    // reset buffer
    for (k=0; k<nbValue; k++)
    {
        for (l=0; l<(popSize + 1); l++)
        {
            valueCircBufAvg[k][l] = 0.0;
            valueCircBuf[k][l] = 0.0;
        }
        avgValue[k] = 0.0;
        rmsValue[k] = 0.0;
    }
    int tail=0;
    int head=0;
    int c=0;
    daoInfo("Avg/Rms telemetry running for %s -> %s/%s, popSize=%d\n",shmName, shmNameAvg, shmNameRms, popSize );

    struct timespec timeout;

    while (end ==0)
    {
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        // Wait for new image
        if (daoShmWaitForSemaphoreTimeout(shm, semNb, &timeout) != -1)
        {
            // if new image, add it in the cir buf.
            for (k = 0; k < nbValue; k++)
            {
                valueCircBufAvg[k][tail] = shm[0].array.F[k] / popSize;
                valueCircBuf[k][tail] = shm[0].array.F[k];
                if (isnan(valueCircBufAvg[k][tail]))
                {
                    valueCircBufAvg[k][tail] = 0.0;
                    valueCircBuf[k][tail] = 0.0;
                }
            }
            tail = (tail + 1) % (popSize + 1);
            for (k = 0; k < nbValue; k++)
            {
                if (!isnan(valueCircBufAvg[k][head]))
                {
                    // add value in the head
                    avgValue[k] += valueCircBufAvg[k][head];
                }
                else
                {
                    // add value in the head
                    avgValue[k] += 0.0;
                }
                // remove the tail value
                avgValue[k] -= valueCircBufAvg[k][tail];
            }
            for (k = 0; k < nbValue; k++)
            {
                for (c = 0; c < popSize; c++)
                {
                    if (!isnan(valueCircBuf[k][c]))
                    {
                        // add value in the c
                        rmsValue[k] += pow(valueCircBuf[k][c] - avgValue[k], 2);
                    }
                    else
                    {
                        // add value in the head
                        rmsValue[k] += 0.0;
                    }
                }
                rmsValue[k] = sqrt(rmsValue[k] / popSize);
            }
            head = (head + 1) % (popSize + 1);

            daoShmImage2Shm(avgValue, nbValue, &shmAvg[0]);
            daoShmImage2Shm(rmsValue, nbValue, &shmRms[0]);
            printf("\r(%8.3f,%8.3f) -> AVG(%8.3f,%8.3f), RMS(%8.3f,%8.3f)",
                   shm[0].array.F[0], shm[0].array.F[1],
                   shmAvg[0].array.F[0], shmAvg[0].array.F[1],
                   shmRms[0].array.F[0], shmRms[0].array.F[1]);
            fflush(stdout);
        }
    }

    daoInfo("EXITING MAIN LOOP\n");
    fflush(stdout);
    free(avgValue);
    free(rmsValue);


    return DAO_SUCCESS;
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    int status;
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

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
    int statThreadVal=0;
    statThreadVal = pthread_create(&controllerThread, NULL, statRealTimeLoop, (void *)&threadIdCtrl);
    if (statThreadVal != 0)
    {
        daoError("Cannot create thread, err\n");
        return DAO_ERROR;
    }
    pthread_join(controllerThread, NULL);
    return DAO_SUCCESS;

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

        switch (str[1]) 
        {
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
                        daoInfo("Average & RMS Telemetry real time control\n");
                        (void)sscanf(*argv++,"%s", shmName); argc -= 1;
                        break;
            case 'n':	
                        (void)sscanf(*argv++,"%d",&popSize); argc -= 1;	
                        break;
            case 's':	
                        (void)sscanf(*argv++,"%d", &semNb); argc -= 1;
                        daoInfo("inputShm sem       = %d \n", semNb);
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

