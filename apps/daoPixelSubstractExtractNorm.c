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

char inAShmName[32];
int semNb = 0;
char inBShmName[32];
char maskShmName[32];
char extractShmName[32];

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
    daoInfo("   -N               normalize output image\n");
    daoInfo("   -S               list of SHM (full path separated by space)\n");
    daoInfo("   -s               semaphore number\n");
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("   -S <input A SHM> <input B SHM> <mask SHM> <extract SHM> <norm SHM> -s <semNb> -L\n");
    daoInfo("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);
    IMAGE *inAShm;
    IMAGE *inBShm;
    IMAGE *maskShm;
    IMAGE *extractShm;

    inAShm = (IMAGE*) malloc(sizeof(IMAGE));
    inBShm = (IMAGE*) malloc(sizeof(IMAGE));
    maskShm = (IMAGE*) malloc(sizeof(IMAGE));
    extractShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmOpen(inAShmName, &inAShm[0]);
    daoShmOpen(inBShmName, &inBShm[0]);
    daoShmOpen(maskShmName, &maskShm[0]);
    daoShmOpen(extractShmName, &extractShm[0]);

    daoInfo("Starting loop, (%s - %s) (%s) -> %s \n",inAShmName, inBShmName, maskShmName, extractShmName);
    fflush(stdout);
    struct timespec t[3];
    struct timespec timeout;
    double elapsedTime;
    double calTime;
    clock_gettime(CLOCK_REALTIME, &t[1]);
    int waitCounter = 0;
    float sum=0.0;
    int nbValue = extractShm[0].md[0].size[0]*extractShm[0].md[0].size[1];
    int k=0;
    usleep(2000000);
    while (end ==0)
    {
        t[0] = t[1];
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        if (daoShmWaitSemTimeout(inAShm, semNb, &timeout) != -1)
        {
            clock_gettime(CLOCK_REALTIME, &t[2]);
            daoToolsShmSubstractExtractNorm(inAShm, inBShm, maskShm, extractShm);
            
            for (k=0; k<nbValue; k++)
            {
                sum += extractShm[0].array.F[k];   
            }
            clock_gettime(CLOCK_REALTIME, &t[1]);
            elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            calTime = (t[1].tv_sec - t[2].tv_sec) * 1e3;
            calTime += (t[1].tv_nsec - t[2].tv_nsec) / 1e6;
            printf("\rcal time = %8.3f us, fps = %8.3f Hz, totalFlux = %f", 
                   1000*calTime,
                   1e6 / (1000 * elapsedTime), sum);
            sum = 0.0;
        }
        else
        {
            waitCounter += 1;
            printf("\rWAIT %d", waitCounter);
        }
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
                    	(void)sscanf(*argv++,"%s",inAShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s",inBShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s",maskShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s",extractShmName); argc -= 1;
                        daoInfo("image in A       : %s\n", inAShmName);
                        daoInfo("image in B       : %s\n", inBShmName);
                        daoInfo("mask             : %s\n", maskShmName);
                        daoInfo("extracted image  : %s\n", extractShmName);
                        break;
            case 's':	
                        (void)sscanf(*argv++,"%d", &semNb);
                        daoInfo("inputShm sem     : %d \n", semNb);
                        break;
            case 'L':
                        daoInfo("Substract and Extract from SHM real time control\n");
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

    daoToolsEnableFTZ(); // flush subnormals -> no denormal FP stalls in the loop

    sArgv0 = *argv;

    DecodeArgs(argc,argv);

    return(sExit);
}
/*==========================================================================*/

