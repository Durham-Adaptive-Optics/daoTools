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
static int	sExit=0;						/* program exit code */

//Need to install process with setuid.  Then, so you aren't running privileged all the time do this:
uid_t euid_real;
uid_t euid_called;
uid_t suid;

struct timespec tnow;
double tnowdouble;
double tlastupdatedouble;

char ocamRawShmName[DAO_SHM_NAME_LEN];
char ocamShmName[DAO_SHM_NAME_LEN];
char lutShmName[DAO_SHM_NAME_LEN];
int semNb = DAO_SEM_AUTO;   // -s: a fixed semaphore; default: one of its own
int binning = 1; // binning factor, default is 1 (no binning)

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
    printf("%s of " __DATE__ " at " __TIME__ "\n", sArgv0);
    printf("   arguments:\n");
    printf("   -h               display this message and exit\n");
    printf("   -d <level>       log level: 0 warnings and errors (default), 1 info, 2 debug, 3 trace\n");
    printf("   -S               list of SHM (full path separated by space)\n");
    printf("   -s <semNb>       a fixed semaphore to wait on (default: one of its own)\n");
    printf("   -b               binning\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   -S <ocamRaw SHM> <ocamShm SHM> <lut SHM> [-s <semNb>] -b <binning> -L\n");
    printf("\n");
}
#define IMG_WIDTH 1056
#define IMG_HEIGHT_BINNED 62
#define HALF_WIDTH (IMG_WIDTH / 2)  // 528
#define BATCH_SIZE (14400 / 2)      // 7200
#define BINNING_OFFSET (57600 - 14400) // 43200
/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

    daoInfo("Starting loop, %s -> %s using %s \n", ocamRawShmName, ocamShmName, lutShmName);
    fflush(stdout);
    IMAGE *ocamRawShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *ocamShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *lutShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(ocamRawShmName, &ocamRawShm[0]);
    daoToolsShmOpen(ocamShmName, &ocamShm[0]);
    daoToolsShmOpen(lutShmName, &lutShm[0]);

    int imgWidth = ocamRawShm[0].md[0].size[0];
    int unscrambledSize = ocamShm[0].md[0].size[0] * ocamShm[0].md[0].size[1];

    // lutShm is only ever loaded once above and never re-read inside the loop, so the
    // scrambled_index -> raw byte offset mapping is constant for the life of this loop.
    // Precompute it once instead of doing a division and a modulo per pixel per frame.
    int *srcOffset = NULL;
    if (binning != 2)
    {
        srcOffset = (int*) malloc(sizeof(int) * unscrambledSize);
        for (int i = 0; i < unscrambledSize; i++)
        {
            int scrambled_index = lutShm[0].array.SI32[i];
            int y = scrambled_index / (imgWidth / 2);
            int x = (scrambled_index % (imgWidth / 2)) * 2;
            srcOffset[i] = y * imgWidth + x;
        }
    }

    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end ==0)
    {
        if (daoToolsWait(ocamRawShm, semNb, 1.0) == DAO_SUCCESS)
        {
            daoToolsLoopStatusStart(&status);
            if (binning == 2)
            {
                uint8_t  *src = ocamRawShm[0].array.UI8;
                uint16_t *dst = ocamShm[0].array.UI16;
                int32_t *lut = lutShm[0].array.SI32;

                // Gather straight from the raw bytes using the LUT-selected 16-bit sample
                // index (2x it to get the byte pair). Only reconstructs the BATCH_SIZE*2
                // samples actually used, instead of the whole-frame intermediate buffer.
                for (int i = 0; i < BATCH_SIZE; i++)
                {
                    int idxA = lut[i * 2] * 2;
                    int idxB = lut[i * 2 + BINNING_OFFSET] * 2;
                    dst[i] = (src[idxA + 1] << 8) | src[idxA];
                    dst[BATCH_SIZE + i] = (src[idxB + 1] << 8) | src[idxB];
                }

                // Zero the rest of the 240x240 image
                memset(&dst[14400], 0, (240 * 240 - 14400) * sizeof(uint16_t));

                daoShmSetDataPartFinalize(&ocamShm[0]);
            }
            else
            {
                // Process the scrambled image directly to unscrambled image
                uint8_t *src = ocamRawShm[0].array.UI8;
                uint16_t *dst = ocamShm[0].array.UI16;
                for (int i = 0; i < unscrambledSize; i++)
                {
                    int off = srcOffset[i];
                    dst[i] = (src[off + 1] << 8) + src[off];
                }
                daoShmSetDataPartFinalize(&ocamShm[0]);
            }
            daoToolsLoopStatusEnd(&status, NULL);
        }
        else
        {
            daoToolsLoopStatusWait(&status);
        }
    }
    printf("\n");


    daoInfo("EXITING MAIN LOOP\n");


    daoToolsShmRelease(&ocamRawShm);
    daoToolsShmRelease(&ocamShm);
    daoToolsShmRelease(&lutShm);
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
                        daoInfo("Simple filter from SHM real time control\n");
                        daoToolsArgNameNext(&argc, &argv, str, ocamRawShmName, sizeof ocamRawShmName);
                        daoToolsArgNameNext(&argc, &argv, str, ocamShmName, sizeof ocamShmName);
                        daoToolsArgNameNext(&argc, &argv, str, lutShmName, sizeof lutShmName);
                        daoInfo("ocamRawShmName = %s\n", ocamRawShmName);
                        daoInfo("ocamShmName = %s\n", ocamShmName);
                        daoInfo("lutShmName = %s\n", lutShmName);
                        break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                daoInfo("inputShm sem       = %d \n", semNb);
                break;
            case 'b':
                binning = daoToolsArgInt(&argc, &argv, str);
                daoInfo("binning       = %d \n", binning);
                break;
            case 'L':
                if (realTimeLoop() != 0)         /* could not start, or failed (see above) */
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

