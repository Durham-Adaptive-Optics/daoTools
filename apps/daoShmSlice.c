/*****************************************************************************
  DAO project
  daoShmSlice -- copy a range of one SHM into another, each time it is published
 *****************************************************************************/

/*==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "daoTools.h"

/*==========================================================================*/
static int sExit = 0;                           /* program exit code */

char inShmName[DAO_SHM_NAME_LEN];
char outShmName[DAO_SHM_NAME_LEN];
int semNb = 0;
long offset = 0;                                /* first input value copied (-o) */
long count = -1;                                /* number of values (-n), -1: the output's size */

static int end = 0;                             // termination flag
// termination function for SIGINT callback
static void endme(int _a)
{
    (void)_a;
    end = 1;
}

/*--------------------------------------------------------------------------*/
static char *sArgv0 = NULL;                     /* name of executable */

static void ShowHelp(void)
{
    daoInfo("%s of " __DATE__ " at " __TIME__ "\n", sArgv0);
    daoInfo("   Each time <in SHM> is published, copy its values [offset, offset + n)\n");
    daoInfo("   into <out SHM> (from its first value) and publish it, with in's cnt2.\n");
    daoInfo("   Both SHMs have the same data type.\n");
    daoInfo("   arguments:\n");
    daoInfo("   -h               display this message and exit\n");
    daoInfo("   -d               display program debug output\n");
    daoInfo("   -S               list of SHM (full path separated by space)\n");
    daoInfo("   -s               semaphore number of <in SHM> to wait on (default 0)\n");
    daoInfo("   -o <offset>      first value of <in SHM> to copy (default 0)\n");
    daoInfo("   -n <n>           number of values (default: the size of <out SHM>)\n");
    daoInfo("   -L               start real-time loop\n");
    daoInfo("   usage (options must precede -L):\n");
    daoInfo("   -S <in SHM> <out SHM> [-o <offset>] [-n <n>] -s <semNb> -L\n");
    daoInfo("\n");
}

static size_t elemSize(uint8_t atype)
{
    switch (atype) {
    case _DATATYPE_UINT8: case _DATATYPE_INT8: return 1;
    case _DATATYPE_UINT16: case _DATATYPE_INT16: return 2;
    case _DATATYPE_UINT32: case _DATATYPE_INT32: case _DATATYPE_FLOAT: return 4;
    case _DATATYPE_UINT64: case _DATATYPE_INT64: case _DATATYPE_DOUBLE:
    case _DATATYPE_COMPLEX_FLOAT: return 8;
    case _DATATYPE_COMPLEX_DOUBLE: return 16;
    default: return 0;
    }
}

/*--------------------------------------------------------------------------*/
static int realTimeLoop()
{
    // register interrupt signal to terminate the main loop
    signal(SIGINT, endme);

    IMAGE *inShm = (IMAGE*) malloc(sizeof(IMAGE));
    IMAGE *outShm = (IMAGE*) malloc(sizeof(IMAGE));
    daoToolsShmOpen(inShmName, &inShm[0]);
    daoToolsShmOpen(outShmName, &outShm[0]);

    long inSize = (long) inShm[0].md[0].nelement;
    long outSize = (long) outShm[0].md[0].nelement;
    size_t es = elemSize(inShm[0].md[0].atype);
    if (count < 0)
        count = outSize;
    if (inShm[0].md[0].atype != outShm[0].md[0].atype || es == 0)
    {
        daoError("%s and %s must have the same data type\n", inShmName, outShmName);
        return DAO_ERROR;
    }
    if (offset < 0 || count < 1 || offset + count > inSize || count > outSize)
    {
        daoError("cannot copy %ld values from offset %ld: %s has %ld, %s %ld\n", count, offset,
                 inShmName, inSize, outShmName, outSize);
        return DAO_ERROR;
    }
    if (daoShmIsGpu(&outShm[0]) && count != outSize)
    {
        daoError("%s is a GPU SHM: the copy must fill it (%ld values)\n", outShmName, outSize);
        return DAO_ERROR;
    }
    daoInfo("%s [%ld, %ld) -> %s (semaphore %d)\n", inShmName, offset, offset + count, outShmName, semNb);
    fflush(stdout);

    struct timespec timeout, t0, t1, tPrint;
    double busy = 0;
    long frames = 0;
    clock_gettime(CLOCK_MONOTONIC, &tPrint);
    while (end == 0)
    {
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        if (daoShmWaitSemTimeout(inShm, semNb, &timeout) != DAO_SUCCESS)
            continue;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        // the input's host data (for a GPU SHM, its host copy)
        void *in = NULL;
        uint32_t idx;
        uint64_t cnt0;
        if (daoShmGetData(&inShm[0], &in, &idx, &cnt0) != DAO_SUCCESS || in == NULL)
            continue;
        const char *src = (const char *) in + (size_t) offset * es;

        outShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
        if (daoShmIsGpu(&outShm[0]))
            daoShmSetData(&outShm[0], (void *) src, (uint32_t) count);
        else
        {
            memcpy(outShm[0].array.V, src, (size_t) count * es);
            daoShmSetDataPartFinalize(&outShm[0]);
        }

        // status once per second: no terminal write in the loop otherwise
        clock_gettime(CLOCK_MONOTONIC, &t1);
        busy += (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
        frames++;
        double sincePrint = (t1.tv_sec - tPrint.tv_sec) + (t1.tv_nsec - tPrint.tv_nsec) / 1e9;
        if (sincePrint >= 1.0)
        {
            printf("\rcopy %.2f us, %.1f Hz      ", busy / frames, frames / sincePrint);
            fflush(stdout);
            busy = 0;
            frames = 0;
            tPrint = t1;
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
    char *str;

    argv += 1;	argc -= 1;					/* skip program name */

    while (argc-- > 0)
    {
        daoDebug("DecodeArgs: working on '%s'/%d\n", *argv, argc);
        str = *argv++;
        if (str[0] != '-')
        {
            daoError("Do not know arg '%s'\n", str);
            ShowHelp();
            exit(1);
        }

        switch (str[1]) {
            case 'h':
                        ShowHelp();
                        exit(0);
            case 'd':
                        (void)sscanf(*argv++, "%d", &daoLogLevel); argc -= 1;
                        break;
            case 'S':
                        daoToolsArgName(inShmName, sizeof inShmName, *argv++); argc -= 1;
                        daoToolsArgName(outShmName, sizeof outShmName, *argv++); argc -= 1;
                        break;
            case 's':
                        (void)sscanf(*argv++, "%d", &semNb); argc -= 1;
                        break;
            case 'o':
                        (void)sscanf(*argv++, "%ld", &offset); argc -= 1;
                        break;
            case 'n':
                        (void)sscanf(*argv++, "%ld", &count); argc -= 1;
                        break;
            case 'L':
                        sExit = realTimeLoop();
                        break;
            default:
                        daoError("Do not know arg '%s'\n", str);
                        ShowHelp();
                        exit(2);
        }
    }

    return;
}

/*==========================================================================*/
int main(int argc, char **argv)
{
    daoToolsSetRtPriority(93); //any number from 0-99; falls back + warns if not permitted

    sArgv0 = *argv;

    DecodeArgs(argc, argv);

    return(sExit);
}
/*==========================================================================*/
