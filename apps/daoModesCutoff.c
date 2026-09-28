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

char inShmName[DAO_SHM_NAME_LEN];
char mcShmName[DAO_SHM_NAME_LEN];
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
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage:\n");
    daoInfo("   daoModesCutOff -S <modes SHM> <modes cutoff SHM> -s <semNb> -L\n");
    printf("\n");
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    uint32_t size[2];
    IMAGE *inShm;
    IMAGE *mcShm;
    IMAGE *loShm;
    IMAGE *hoShm;

    char loShmName[DAO_SHM_NAME_LEN];
    char hoShmName[DAO_SHM_NAME_LEN];

    inShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(inShmName, &inShm[0]);
    mcShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(mcShmName, &mcShm[0]);

    daoInfo("%dx%d\n", inShm[0].md[0].size[0], inShm[0].md[0].size[1]);
    // Create LO SHM
    loShm = (IMAGE*) malloc(sizeof(IMAGE));
    if (daoToolsInsertShmNamePrefixN(inShmName, "LO", loShmName, sizeof loShmName) != DAO_SUCCESS)
        exit(EXIT_FAILURE);
    size[0] = mcShm[0].array.UI32[0];
    size[1] = inShm[0].md[0].size[1];
    daoShmCreate(loShm, loShmName, 2, size, inShm[0].md[0].atype, 1, 0);

    // Create HO SHM
    hoShm = (IMAGE*) malloc(sizeof(IMAGE));
    if (daoToolsInsertShmNamePrefixN(inShmName, "HO", hoShmName, sizeof hoShmName) != DAO_SUCCESS)
        exit(EXIT_FAILURE);
    size[0] = inShm[0].md[0].size[0]-mcShm[0].array.UI32[0];
    size[1] = inShm[0].md[0].size[1];
    daoShmCreate(hoShm, hoShmName, 2, size, inShm[0].md[0].atype, 1, 0);

    printf("Starting loop, (%s) -> (%s,%s)\n",
           inShmName, loShmName, hoShmName);

    fflush(stdout);

    uint32_t inSize = inShm[0].md[0].size[0]*inShm[0].md[0].size[1];

    struct timespec t[3];
    struct timespec timeout;
    double elapsedTime;
    double compTime;
    int cnt=0;
    uint32_t k=0;
    clock_gettime(CLOCK_REALTIME, &t[1]);
    while (end ==0)
    {
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        // Wait for new image
        if (daoShmWaitSemTimeout(inShm, semNb, &timeout) != DAO_TIMEOUT)
        {
            clock_gettime(CLOCK_REALTIME, &t[2]);

            for (k=0; k<mcShm[0].array.UI32[0]; k++)
            {
                loShm[0].array.F[k] = inShm[0].array.F[k];
            }
            for (k=mcShm[0].array.UI32[0]; k<inSize; k++)
            {
                hoShm[0].array.F[k-mcShm[0].array.UI32[0]] = inShm[0].array.F[k];
            }

            daoShmSetDataPartFinalize(&loShm[0]);
            daoShmSetDataPartFinalize(&hoShm[0]);
            
            t[0]=t[1];        
            clock_gettime(CLOCK_REALTIME, &t[1]);
            elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            compTime = (t[1].tv_sec - t[2].tv_sec) * 1e3;
            compTime += (t[1].tv_nsec - t[2].tv_nsec) / 1e6;
            printf("\rcompTime = %.3f us, fps = %8.3f Hz", compTime, 1e6/(1000*elapsedTime));
        }
        else
        {
            printf("\r WAIT %d", cnt);
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
                        daoToolsArgName(inShmName, sizeof inShmName, *argv++);
                        daoToolsArgName(mcShmName, sizeof mcShmName, *argv++);
                        daoInfo("inShmName          = %s\n", inShmName);
                        daoInfo("mcShmName          = %s\n", mcShmName);
                        break;
            case 's':	
                        (void)sscanf(*argv++,"%d", &semNb); argc -= 1;
                        daoInfo("inputShm sem       = %d \n", semNb);
                        break;
            case 'L':
                        printf("HPF real time control\n");
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
