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

#include "daoTools.h"

/*==========================================================================*/
static int	sNdx=0;							/* board index */
static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;

struct timespec tnow;
double tnowdouble;
double tlastupdatedouble;

char ocamRawShmName[32];
char ocamShmName[32];
char lutShmName[32];

static int   		end     = 0;		           // termination flag
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
    /*
     **	Post init tests
     */
    daoInfo("   -L ocamRawShm ocamShm lutShm              real time control loop\n");
    daoInfo("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    daoInfo("Starting loop, %s -> %s using %s \n", ocamRawShmName, ocamShmName, lutShmName);
    fflush(stdout);
    IMAGE *ocamRawShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *ocamShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *lutShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmShm2Img(ocamRawShmName, &ocamRawShm[0]);
    daoShmShm2Img(ocamShmName, &ocamShm[0]);
    daoShmShm2Img(lutShmName, &lutShm[0]);

    int imgWidth = ocamRawShm[0].md[0].size[0];
    int unscrambledSize = ocamShm[0].md[0].size[0] * ocamShm[0].md[0].size[1];
    struct timespec timeout;
    struct timespec t[3];
    double elapsedTime;
    clock_gettime(CLOCK_REALTIME, &t[1]);
    while (end ==0)
    {
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        // Wait for new image
        if (sem_timedwait(ocamRawShm[0].semptr[2], &timeout) != -1)
        {
            clock_gettime(CLOCK_REALTIME, &t[0]);
            // Process the scrambled image directly to unscrambled image
            for (int i = 0; i < unscrambledSize; i++)
            {
                int scrambled_index = lutShm[0].array.SI32[i];
                int y = scrambled_index / (imgWidth / 2);
                int x = (scrambled_index % (imgWidth / 2)) * 2;
                ocamShm[0].array.UI16[i] = (ocamRawShm[0].array.UI8[y * imgWidth + x + 1] << 8) + ocamRawShm[0].array.UI8[y * imgWidth + x];
            }
            daoShmImagePart2ShmFinalize(&ocamShm[0]);
            clock_gettime(CLOCK_REALTIME, &t[1]);
        }
        elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
        elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
        printf("\r time to descramble = %8.6f ms", elapsedTime); 
        fflush(stdout);
    }


    daoInfo("EXITING MAIN LOOP\n");
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
                        argv += 1; argc -= 1;
                        break;

            case 'b':	(void)sscanf(*argv++,"%d",&sNdx); argc -= 1;	break;
            case 'u':
                        (void)sscanf(*argv++,"%d",&a1); argc -= 1;
                        daoDebug("will sleep for %d usec\n",a1);
                        (void)usleep(a1);
                        break;
                        break;
            case 'L':
                        daoInfo("Simple filter from SHM real time control\n");
                    	(void)sscanf(*argv++,"%s", ocamRawShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", ocamShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", lutShmName); argc -= 1;
                        daoInfo("ocamRawShmName = %s\n", ocamRawShmName);
                        daoInfo("ocamShmName = %s\n", ocamShmName);
                        daoInfo("lutShmName = %s\n", lutShmName);
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

