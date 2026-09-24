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

char imShmName[32];
char intensityShmName[32];
char pixIdShmName[32];
char validPixShmName[32];
char validSubPixShmName[32];
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
    daoInfo("   -S <in SHM> <intensity SHM> <valid Pix SHM> <validSubaPix> -s <semNb> -L\n");
    daoInfo("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    daoInfo("Starting loop, %s -> %s \n", imShmName, intensityShmName);
    IMAGE *imShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *intensityShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *validPixShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *validSubPixShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmOpen(imShmName, &imShm[0]);
    daoShmOpen(intensityShmName, &intensityShm[0]);
    daoShmOpen(validPixShmName, &validPixShm[0]);
    daoShmOpen(validSubPixShmName, &validSubPixShm[0]);

    int validPixSize = validPixShm[0].md[0].size[0] * validPixShm[0].md[0].size[1];

    int i;
    // Compute number of valid pixels
    int validPixSum = 0;
    for (i = 0; i < validPixSize; i++)
    {
        if (validPixShm[0].array.UI32[i] == 1)
        {
            validPixSum++;
        }
    }
    daoInfo("Detected %d valid pixels\n", validPixSum);

    // Create a LUT
    int lut[validPixSum];
    int k=0;
    for (i = 0; i < validPixSize; i++)
    {
        if (validPixShm[0].array.UI32[i] == 1)
        {
            lut[k] = i;
            k++;
        }
    }
    float sum = 0;

    struct timespec t[3];
    struct timespec timeout;
    double elapsedTime;
    double compTime;
    int cnt=0;
    clock_gettime(CLOCK_REALTIME, &t[1]);
    while (end ==0)
    {
        t[0] = t[1];
        // Wait for new image
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        if (daoShmWaitSemTimeout(imShm, semNb, &timeout) != -1)
        {
            clock_gettime(CLOCK_REALTIME, &t[2]);
            // New image, insert something here
            intensityShm[0].md[0].cnt2 = imShm[0].md[0].cnt2;

            sum=0;
            for (k=0; k<validPixSum; k++)
            {
                //sum += imShm[0].array.F[lut[k]];
                if (validSubPixShm[0].array.UI32[lut[k]] == 1)
                {
                    sum += imShm[0].array.F[lut[k]];
                }
            }

            for (k=0; k<validPixSum; k++)
            {
                intensityShm[0].array.F[k] =  imShm[0].array.F[lut[k]]/sum * validSubPixShm[0].array.UI32[lut[k]];
            }

            daoShmSetDataPartFinalize(&intensityShm[0]);

            clock_gettime(CLOCK_REALTIME, &t[1]);
            elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            compTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            compTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            printf("\rcompTime = %.3f us, fps = %8.3f Hz", compTime, 1e6/(1000*elapsedTime));
            fflush(stdout);
        }
        else
        {
            printf("\r WAIT %d", cnt);
            fflush(stdout);
            cnt++;
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
                        break;
            case 'S':
                        daoInfo("real time control\n");
                    	(void)sscanf(*argv++,"%s", imShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", intensityShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", validPixShmName); argc -= 1;
                    	(void)sscanf(*argv++,"%s", validSubPixShmName); argc -= 1;
                        daoInfo("imShmName = %s\n", imShmName);
                        daoInfo("intensityShmName = %s\n", intensityShmName);
                        daoInfo("validPixShmName = %s\n", validPixShmName);
                        daoInfo("validSubPixShmName = %s\n", validSubPixShmName);
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

