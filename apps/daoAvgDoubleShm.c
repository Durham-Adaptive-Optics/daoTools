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

IMAGE *shm;
IMAGE *shmAvg;

char shmName[DAO_SHM_NAME_LEN];
char shmNameAvg[DAO_SHM_NAME_LEN];
int nbAvg=100;
int semNb=0;

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
    daoInfo("   -S <SHM> -n <nb Average> -s <semNb> -L\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    shm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(shmName, &shm[0]);
    // Create receiving Avg SHM
    shmAvg = (IMAGE*) malloc(sizeof(IMAGE));
    if (daoToolsInsertShmNamePrefixN(shmName, "Avg", shmNameAvg, sizeof shmNameAvg) != DAO_SUCCESS)
        exit(EXIT_FAILURE);
    uint32_t size[2];
    size[0] = shm[0].md[0].size[0];
    size[1] = shm[0].md[0].size[1];
    daoShmCreate(shmAvg, shmNameAvg, 2, size, shm[0].md[0].atype, 1, 0);
    //daoShmOpen(shmNameAvg, &shmAvg[0]);
    printf("Starting loop, %s -> %s, nAvg=%d\n",shmName, shmNameAvg, nbAvg );
    fflush(stdout);

    int nbValue = shm[0].md[0].size[0]*shm[0].md[0].size[1];
    double *avgValue = shmAvg[0].array.D;//malloc(nbValue*sizeof(double));
    double valueCircBuf[nbValue][nbAvg+1];
    int k, l;
    // reset buffer
    for (k=0; k<nbValue; k++)
    {
        for (l=0; l<(nbAvg + 1); l++)
        {
            valueCircBuf[k][l] = 0.0;
        }
        avgValue[k] = 0.0;
    }
    int tail=0;
    int head=0;
    printf("Average telemetry running for %s -> %s, nAvg=%d\n",shmName, shmNameAvg, nbAvg );
    struct timeval t[3];
    gettimeofday(&t[1],NULL);
    struct timespec timeout;
    int cnt=0;
    while (end ==0)
    {
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        // Wait for new image
        if (daoShmWaitSemTimeout(shm, semNb, &timeout) != DAO_TIMEOUT)
        {
            // if new image, add it in the cir buf.
            for (k=0; k<nbValue; k++)
            {
                valueCircBuf[k][tail] = shm[0].array.D[k]/nbAvg;
                if (isnan(valueCircBuf[k][tail]))
                {
                    valueCircBuf[k][tail] = 0.0;
                }
            }
            tail = (tail + 1) % (nbAvg + 1);
            for (k=0; k<nbValue; k++)
            {
                if (!isnan(valueCircBuf[k][head]))
                {
                    // add value in the head
                    avgValue[k] += valueCircBuf[k][head];
                }
                else
                {
                    // add value in the head
                    avgValue[k] += 0.0;
                }
                // remove the tail value
                avgValue[k] -= valueCircBuf[k][tail];
            }
            head = (head + 1) % (nbAvg + 1);

    //        daoShmSetData(&shmAvg[0], avgValue, nbValue);
            daoShmSetDataPartFinalize(&shmAvg[0]);
            printf("\r(%f,%f) -> (%.3f,%.3f)",
                    shm[0].array.D[0], shm[0].array.D[1],
                    shmAvg[0].array.D[0], shmAvg[0].array.D[1]);
        }
        else
        {
            printf("\r WAIT %d", cnt);
            fflush(stdout);
            cnt++;
        }
        fflush(stdout);
    }


    printf("EXITING MAIN LOOP\n");
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
                        argv += 1; 
                        argc -= 1;
                        break;
            case 'u':
                        (void)sscanf(*argv++,"%d",&a1); argc -= 1;
                        daoDebug("will sleep for %d usec\n",a1);
                        (void)usleep(a1);
                        break;
            case 'S':
                        printf("Average Telemetry real time control\n");
                        daoToolsArgName(shmName, sizeof shmName, *argv++);
                        daoInfo("shmName          = %s\n", shmName);
                        break;
            case 'n':	(void)sscanf(*argv++,"%d",&nbAvg); 
                        daoInfo("nb Average       = %d \n", nbAvg);
                        argc -= 1;	
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
