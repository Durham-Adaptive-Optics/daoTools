/*****************************************************************************
  DAO project
  s.cetre
 *****************************************************************************/
/* daoComputeCentroidWindows -- slopes of analysis windows (daoCentroidWindows):
 * Shack-Hartmann on an extended object, in one direction (a window per sub-aperture)
 * or several (several windows per sub-aperture, each on another part of the field).
 * Each window is a row of the table SHM (DAO_WINDOW_COLS floats: centre, width,
 * height, search range, reference slope, N); it is reloaded when it changes. */
/*==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dao.h"
#include "daoTools.h"

/*==========================================================================*/
char inShmName[DAO_SHM_NAME_LEN];
char outShmName[DAO_SHM_NAME_LEN];
char tableShmName[DAO_SHM_NAME_LEN];
char refShmName[DAO_SHM_NAME_LEN];
char thresholdShmName[DAO_SHM_NAME_LEN];
char minFluxShmName[DAO_SHM_NAME_LEN] = "";      /* -f: optional minimum flux per window */
int method = DAO_WINDOWS_CORRELATION;            /* -m */
int peak = -1;                                   /* -p; default: parabola, or (cog) barycenterThreshold */
int semNb = DAO_SEM_AUTO;                        /* -s: a fixed semaphore; default: one of its own */

static int end = 0;
static void endme(int sig)
{
    (void)sig;
    end = 1;
}

static const char *methods[] = {"correlation", "cog", "correlationNormalized"};
static const char *peaks[] = {"max", "parabola", "barycenter", "barycenterThreshold", "barycenterThresholdWeighted"};

static int lookup(const char *name, const char **names, int n)
{
    for (int k = 0; k < n; k++)
        if (!strcmp(name, names[k]))
            return k;
    return -1;
}

/* the minimum flux (-f), read every frame so it can be tuned live; without -f: 0 */
static float minFluxValue(IMAGE *shm)
{
    if (shm == NULL)
        return 0.0f;
    return shm[0].md[0].atype == _DATATYPE_DOUBLE ? (float)shm[0].array.D[0] : shm[0].array.F[0];
}

static char *sArgv0 = NULL;
static void ShowHelp(void)
{
    printf("%s of " __DATE__ " at " __TIME__ "\n", sArgv0);
    printf("   arguments:\n");
    printf("   -h               display this message and exit\n");
    printf("   -d <level>       log level: 0 warnings and errors (default), 1 info, 2 debug, 3 trace\n");
    printf("   -m <method>      correlation (default) | correlationNormalized | cog\n");
    printf("   -p <estimator>   max | parabola (correlation default) | barycenter | barycenterThreshold\n");
    printf("                    (cog default) | barycenterThresholdWeighted\n");
    printf("   -f <shm>         minimum flux SHM (1 value): a window with less light (the sum of its\n");
    printf("                    raw pixels) gets slopes (0, 0); without -f: no minimum\n");
    printf("   -s <semNb>       a fixed semaphore to wait on (default: one of its own)\n");
    printf("   -L               start the real-time loop (after the other options)\n");
    printf("   usage:\n");
    printf("   -S <in SHM> <out SHM> <table SHM> <reference images SHM | -> <threshold SHM> [-m ..] [-p ..] [-f ..] -L\n");
    printf("\n");
    printf("   table: one row of %d floats per window: centre x, y (pixels), width, height,\n", DAO_WINDOW_COLS);
    printf("   search range x, y (<= %d), reference slope x, y (subtracted), N (the values the\n", DAO_CENTROID_CORR_MAX_SEARCH_RANGE);
    printf("   barycentres keep). Reference images: (windows x height) x width, window k's in\n");
    printf("   the top-left corner of rows k*height..; '-' for cog. out: slope x, slope y, flux\n");
    printf("   (3 x windows). correlationNormalized: zero-mean normalised, for an extended object.\n");
    printf("\n");
}

static int realTimeLoop(void)
{
    daoToolsOnExitSignals(endme);   /* Ctrl+C, kill, tmux kill-session */
    if (peak < 0)
        peak = method == DAO_WINDOWS_COG ? DAO_PEAK_BARYCENTER_THRESHOLD : DAO_PEAK_PARABOLA;
    IMAGE *inShm = (IMAGE *)malloc(sizeof(IMAGE)), *outShm = (IMAGE *)malloc(sizeof(IMAGE));
    IMAGE *tableShm = (IMAGE *)malloc(sizeof(IMAGE)), *thresholdShm = (IMAGE *)malloc(sizeof(IMAGE));
    IMAGE *refShm = NULL, *minFluxShm = NULL;
    daoToolsShmOpen(inShmName, &inShm[0]);
    daoToolsShmOpen(outShmName, &outShm[0]);
    daoToolsShmOpen(tableShmName, &tableShm[0]);
    daoToolsShmOpen(thresholdShmName, &thresholdShm[0]);
    if (strcmp(refShmName, "-") != 0) {
        refShm = (IMAGE *)malloc(sizeof(IMAGE));
        daoToolsShmOpen(refShmName, &refShm[0]);
    }
    if (minFluxShmName[0] != '\0') {
        minFluxShm = (IMAGE *)malloc(sizeof(IMAGE));
        daoToolsShmOpen(minFluxShmName, &minFluxShm[0]);
    }
    const int nx = inShm[0].md[0].size[1], ny = inShm[0].md[0].size[0];
    const int nWin = (int)(tableShm[0].md[0].nelement / DAO_WINDOW_COLS);
    if (nWin < 1 || tableShm[0].md[0].nelement % DAO_WINDOW_COLS) {
        daoError("%s: %lu values: not a table of %d values per window\n", tableShmName,
                 (unsigned long)tableShm[0].md[0].nelement, DAO_WINDOW_COLS);
        return 1;
    }
    if (method != DAO_WINDOWS_COG && refShm == NULL) {
        daoError("a correlation needs the reference images SHM\n");
        return 1;
    }
    int refW = 0, refH = 0;
    if (refShm) {
        refW = refShm[0].md[0].size[1];
        refH = (int)(refShm[0].md[0].size[0] / nWin);
        daoToolsShmCheck(refShm, refShmName, _DATATYPE_FLOAT, (long)nWin * refW * refH);
    }
    daoToolsShmCheck(inShm, inShmName, _DATATYPE_FLOAT, (long)nx * ny);
    daoToolsShmCheck(tableShm, tableShmName, _DATATYPE_FLOAT, (long)nWin * DAO_WINDOW_COLS);
    daoToolsShmCheck(thresholdShm, thresholdShmName, _DATATYPE_FLOAT, 1);
    daoToolsShmCheck(outShm, outShmName, _DATATYPE_FLOAT, 3L * nWin);   /* slope x, slope y, flux */
    daoInfo("%d windows, %s, %s, on %s -> %s\n", nWin, methods[method], peaks[peak], inShmName, outShmName);

    uint64_t tableCnt = (uint64_t)-1;
    daoToolsLoopStatus status;
    daoToolsLoopStatusInit(&status);
    while (end == 0) {
        if (daoToolsWait(inShm, semNb, 1.0) == DAO_SUCCESS) {
            daoToolsLoopStatusStart(&status);
            if (tableShm[0].md[0].cnt0 != tableCnt) {        /* new windows: say which do not fit */
                tableCnt = tableShm[0].md[0].cnt0;
                if (daoCentroidWindowsCheck(tableShm[0].array.F, nWin, nx, ny, refW, refH, method, peak)
                    != DAO_SUCCESS && method == DAO_WINDOWS_COG && peak < DAO_PEAK_BARYCENTER)
                    return 1;                                /* the estimator itself does not apply */
            }
            outShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
            daoCentroidWindows(inShm[0].array.F, nx, ny, tableShm[0].array.F, nWin,
                               refShm ? refShm[0].array.F : NULL, refW, refH, method, peak,
                               thresholdShm[0].array.F[0], minFluxValue(minFluxShm), outShm[0].array.F);
            daoShmSetDataPartFinalize(&outShm[0]);
            daoToolsLoopStatusEnd(&status, NULL);
        }
        else {
            daoToolsLoopStatusWait(&status);
        }
    }
    printf("\n");
    daoInfo("EXITING MAIN LOOP\n");
    daoToolsShmRelease(&inShm);
    daoToolsShmRelease(&outShm);
    daoToolsShmRelease(&tableShm);
    daoToolsShmRelease(&thresholdShm);
    if (refShm)
        daoToolsShmRelease(&refShm);
    if (minFluxShm)
        daoToolsShmRelease(&minFluxShm);
    return 0;
}

static void DecodeArgs(int argc, char **argv)
{
    char *str;
    argv += 1; argc -= 1;                            /* skip program name */
    while (argc-- > 0) {
        str = *argv++;
        if (str[0] != '-') {
            daoError("Do not know arg '%s'\n", str);
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
            case 'S':
                daoToolsArgNameNext(&argc, &argv, str, inShmName, sizeof inShmName);
                daoToolsArgNameNext(&argc, &argv, str, outShmName, sizeof outShmName);
                daoToolsArgNameNext(&argc, &argv, str, tableShmName, sizeof tableShmName);
                daoToolsArgNameNext(&argc, &argv, str, refShmName, sizeof refShmName);
                daoToolsArgNameNext(&argc, &argv, str, thresholdShmName, sizeof thresholdShmName);
                break;
            case 'm':
                if ((method = lookup(daoToolsArgValue(&argc, &argv, str), methods, 3)) < 0) {
                    daoError("-m: correlation, correlationNormalized or cog\n");
                    exit(2);
                }
                break;
            case 'p':
                if ((peak = lookup(daoToolsArgValue(&argc, &argv, str), peaks, 5)) < 0) {
                    daoError("-p: max, parabola, barycenter, barycenterThreshold or barycenterThresholdWeighted\n");
                    exit(2);
                }
                break;
            case 'f':
                daoToolsArgNameNext(&argc, &argv, str, minFluxShmName, sizeof minFluxShmName);
                break;
            case 's':
                semNb = daoToolsArgInt(&argc, &argv, str);
                break;
            case 'L':
                if (method == DAO_WINDOWS_COG && peak >= 0 && peak < DAO_PEAK_BARYCENTER) {
                    daoError("-p %s does not apply to a centre of gravity: a barycentre\n", peaks[peak]);
                    exit(2);
                }
                if (realTimeLoop() != 0)
                    exit(EXIT_FAILURE);
                break;
            default:
                daoError("Do not know arg '%s'\n", str);
                ShowHelp();
                exit(2);
        }
    }
}

/*==========================================================================*/
int main(int argc, char **argv)
{
    daoToolsSetRtPriority(93);                       /* falls back + warns if not permitted */
    sArgv0 = *argv;
    if (argc < 2) {
        ShowHelp();
        return 1;
    }
    DecodeArgs(argc, argv);
    return 0;
}
