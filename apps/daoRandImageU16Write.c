/*****************************************************************************
  Durham AO project
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

char outShmName[32];
char clockShmName[32];

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
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("   -S <out SHM> <clock SHM> -s <semNb> -L\n");
    daoInfo("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);
    IMAGE *outShm;
    IMAGE *clockShm;


    daoInfo("Starting loop, %s \n", outShmName);
    fflush(stdout);
    outShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmOpen(outShmName, &outShm[0]);
    clockShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmOpen(clockShmName, &clockShm[0]);

    int outSize = outShm[0].md[0].size[0]*outShm[0].md[0].size[1];
    struct timespec t[4];
    double elapsedTime;
    clock_gettime(CLOCK_REALTIME, &t[1]);
    int k;
    uint16_t outCmd[outSize];
    const uint16_t MAX14 = (1u << 14) - 1; // 16383    
    // time out for semaphore wait
    struct timespec timeout;
    timeout.tv_sec = 1; // 1 second timeout
    while (end ==0)
    {
        // Wait for the clock frame using semaphore
        if (daoShmWaitSemTimeout(clockShm, semNb, &timeout) != -1)
        {
            t[0] = t[1];
            outShm[0].md[0].cnt2 = outShm[0].md[0].cnt2 + 1;
            clock_gettime(CLOCK_REALTIME, &t[2]);
            for (k = 0; k < outSize; k++)
            {
                // Uniform in [0, MAX14]
                outCmd[k] = (uint16_t)(rand() % (MAX14 + 1));
            }

            daoShmSetData(&outShm[0], (unsigned short *)outCmd, outSize);

            clock_gettime(CLOCK_REALTIME, &t[1]);
            elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            printf("\rfps = %.3f Hz, out[%d, %d,...,%d]", 1e6 / (1000 * elapsedTime),
                   outShm[0].array.UI16[0],
                   outShm[0].array.UI16[1],
                   outShm[0].array.UI16[outSize - 1]);
            fflush(stdout);
        }
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
                        daoInfo("Simple writer from SHM real time control\n");
                    	(void)sscanf(*argv++,"%s",outShmName); argc -= 1;
                        (void)sscanf(*argv++,"%s",clockShmName); argc -= 1;
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

