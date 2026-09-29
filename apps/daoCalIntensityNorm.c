/*****************************************************************************
  DAO project - daoCalIntensityNorm.c
  s.cetre

  Real-time loop: calibrate (bg/ff) + extract + normalize intensity
  for valid pupil pixels in a single pass — no intermediate SHM.

  Same as daoCalIntensity, but the reference is not pre-normalized: it is
  normalized here against its own sum over the illuminated sub-aperture
  pixels (invSumRef). invSumRef is only recomputed when the reference SHM
  or the illumPix SHM changes (tracked via their cnt0 counters), since both
  are updated far less often than the raw image.

    intensity = cal*invSum*illum - ref*invSumRef*illum

  Input SHMs:
    raw       : raw camera image (float or uint16)
    ff        : flat field (float)
    bg        : background (float)
    ref       : reference (float)
    validPix  : valid pixel mask (uint32, 1=valid)
    illumPix: valid sub-aperture pixel mask (uint32, 1=in subaperture)

  Output SHM:
    intensity : normalized intensity for each valid pixel (float)

  Usage:
    daoCalIntensityNorm -S <raw> <ff> <bg> <ref> <validPix> <illumPix> <intensity> [-s <semNb>] -L
 *****************************************************************************/

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
 #include <sys/mman.h>
 #include <sys/time.h>
 #include <pthread.h>

 #include "dao.h"
 #include "daoTools.h"

 /*==========================================================================*/
 static int sExit = 0;

 uid_t euid_real;
 uid_t euid_called;
 uid_t suid;

 char rawShmName[DAO_SHM_NAME_LEN];
 char ffShmName[DAO_SHM_NAME_LEN];
 char bgShmName[DAO_SHM_NAME_LEN];
 char refShmName[DAO_SHM_NAME_LEN];
 char validPixShmName[DAO_SHM_NAME_LEN];
 char illumPixShmName[DAO_SHM_NAME_LEN];
 char intensityShmName[DAO_SHM_NAME_LEN];
 int  semNb = DAO_SEM_AUTO;   // -s: a fixed semaphore; default: one of its own

 static int end = 0;
 static void endme(int _a) { (void)_a; end = 1; }

 static char *sArgv0 = NULL;

 /*--------------------------------------------------------------------------*/
 static void ShowHelp(void)
 {
     printf("%s of " __DATE__ " at " __TIME__ "\n", sArgv0);
     printf("   Calibrate (bg/ff) + extract + normalize intensity in one pass,\n");
     printf("   with the reference normalized against its own illuminated sum.\n");
     printf("   arguments:\n");
     printf("   -h               display this message and exit\n");
     printf("   -d <level>       log level: 0 warnings and errors (default), 1 info, 2 debug, 3 trace\n");
     printf("   [-s <semNb>]     semaphore on raw SHM (default: one of its own)\n");
     printf("   -S <raw> <ff> <bg> <ref> <validPix> <illumPix> <intensity>\n");
     printf("   -L               start the real-time loop (after the other options)\n");
     printf("\n");
 }

 /*--------------------------------------------------------------------------*/
 /* Gather the raw pixels listed in lut[] into a contiguous float buffer.
  * The data-type switch is hoisted out of the per-pixel loop so each
  * branch is a tight, vectorizable gather. */
 #define DAO_GATHER_RAW(MEMBER)                                  \
     do {                                                       \
         for (int k = 0; k < n; k++)                             \
         {                                                      \
             dst[k] = (float)raw->array.MEMBER[lut[k]];          \
         }                                                      \
     } while (0)

 static void gatherRawToFloat(float *dst, const IMAGE *raw,
                              const int *lut, int n, int rawType)
 {
     switch (rawType)
     {
         case _DATATYPE_UINT16: DAO_GATHER_RAW(UI16); break;
         case _DATATYPE_FLOAT:  DAO_GATHER_RAW(F);    break;
         case _DATATYPE_INT16:  DAO_GATHER_RAW(SI16); break;
         case _DATATYPE_UINT8:  DAO_GATHER_RAW(UI8);  break;
         case _DATATYPE_INT8:   DAO_GATHER_RAW(SI8);  break;
         case _DATATYPE_UINT32: DAO_GATHER_RAW(UI32); break;
         case _DATATYPE_INT32:  DAO_GATHER_RAW(SI32); break;
         case _DATATYPE_UINT64: DAO_GATHER_RAW(UI64); break;
         case _DATATYPE_INT64:  DAO_GATHER_RAW(SI64); break;
         case _DATATYPE_DOUBLE: DAO_GATHER_RAW(D);    break;
         default:
             for (int k = 0; k < n; k++)
             {
                 dst[k] = 0.0f;
             }
             break;
     }
 }

 #undef DAO_GATHER_RAW

 /*--------------------------------------------------------------------------*/
 static int realTimeLoop()
 {
     daoToolsOnExitSignals(endme);   // Ctrl+C, kill, tmux kill-session

     IMAGE *rawShm       = (IMAGE*) malloc(sizeof(IMAGE));
     IMAGE *ffShm        = (IMAGE*) malloc(sizeof(IMAGE));
     IMAGE *bgShm        = (IMAGE*) malloc(sizeof(IMAGE));
     IMAGE *refShm       = (IMAGE*) malloc(sizeof(IMAGE));
     IMAGE *validPixShm  = (IMAGE*) malloc(sizeof(IMAGE));
     IMAGE *illumPixShm  = (IMAGE*) malloc(sizeof(IMAGE));
     IMAGE *intensityShm = (IMAGE*) malloc(sizeof(IMAGE));

     daoToolsShmOpen(rawShmName,       &rawShm[0]);
     daoToolsShmOpen(ffShmName,        &ffShm[0]);
     daoToolsShmOpen(bgShmName,        &bgShm[0]);
     daoToolsShmOpen(refShmName,       &refShm[0]);
     daoToolsShmOpen(validPixShmName,  &validPixShm[0]);
     daoToolsShmOpen(illumPixShmName,  &illumPixShm[0]);
     daoToolsShmOpen(intensityShmName, &intensityShm[0]);

     int imSize = rawShm[0].md[0].size[0] * rawShm[0].md[0].size[1];
     daoToolsShmCheck(validPixShm, validPixShmName, _DATATYPE_UINT32, imSize);

     // ----------------------------------------------------------------
     // Build LUT of valid pixel indices over the full image (done once)
     // ----------------------------------------------------------------
     int nValid = 0;
     for (int i = 0; i < imSize; i++)
     {
         if (validPixShm[0].array.UI32[i] == 1)
         {
             nValid++;
         }
     }

     daoInfo("Valid pixels: %d / %d\n", nValid, imSize);

     int *lut = (int*) malloc(nValid * sizeof(int));
     int  k = 0;
     for (int i = 0; i < imSize; i++)
     {
         if (validPixShm[0].array.UI32[i] == 1)
         {
             lut[k++] = i;
         }
     }
     // the maps are read at the valid pixels only: up to the last one
     long used = nValid > 0 ? lut[nValid - 1] + 1 : 0;
     daoToolsShmCheck(ffShm, ffShmName, _DATATYPE_FLOAT, used);
     daoToolsShmCheck(bgShm, bgShmName, _DATATYPE_FLOAT, used);
     daoToolsShmCheck(refShm, refShmName, _DATATYPE_FLOAT, used);
     daoToolsShmCheck(illumPixShm, illumPixShmName, _DATATYPE_UINT32, used);
     daoToolsShmCheck(intensityShm, intensityShmName, _DATATYPE_FLOAT, nValid);

     // ----------------------------------------------------------------
     // Per-frame scratch (contiguous, indexed by valid-pixel rank k)
     //   rawf    : raw pixel gathered to float
     //   cal     : calibrated pixel  (raw - bg) * ffEff, 0 outside subaperture
     //   w       : effective flat field, folded with the (ff>0) and illum masks
     //   bgp     : packed background
     //   refTerm : ref[k] * invSumRef * illum  (the whole reference term)
     // w / bgp / refTerm only change when ff, bg, ref or illumPix change.
     // ----------------------------------------------------------------
     float *rawf    = (float*) malloc(nValid * sizeof(float));
     float *cal     = (float*) malloc(nValid * sizeof(float));
     float *w       = (float*) malloc(nValid * sizeof(float));
     float *bgp     = (float*) malloc(nValid * sizeof(float));
     float *refTerm = (float*) malloc(nValid * sizeof(float));

     // Detect raw image type
     int rawType = rawShm[0].md[0].atype;
     daoInfo("Raw image type: %d\n", rawType);

     struct timespec t[3];


     // ----------------------------------------------------------------
     // Reference normalization: invSumRef = 1 / sum(ref * illum) over
     // valid pixels. Only recomputed when refShm or illumPixShm changes.
     // ----------------------------------------------------------------
     float invSumRef = 0.0f;
     float sumRef    = 0.0f;
     unsigned long cnt0Ref   = refShm[0].md[0].cnt0 - 1;
     unsigned long cnt0Illum = illumPixShm[0].md[0].cnt0 - 1;
     unsigned long cnt0Ff    = ffShm[0].md[0].cnt0 - 1;
     unsigned long cnt0Bg    = bgShm[0].md[0].cnt0 - 1;

     clock_gettime(CLOCK_REALTIME, &t[1]);

     daoInfo("Entering real-time loop\n");
     daoToolsLoopStatus status;
     daoToolsLoopStatusInit(&status);
     while (end == 0)
     {

         if (daoToolsWait(rawShm, semNb, 1.0) == DAO_SUCCESS)
         {
             daoToolsLoopStatusStart(&status);

             // ----------------------------------------------------------------
             // ff / bg / ref / illumPix changed: repack the invariant buffers
             // and recompute invSumRef. These SHMs update far less often than
             // the raw image, so this whole block is off the hot path.
             // ----------------------------------------------------------------
             if (cnt0Ref   != refShm[0].md[0].cnt0      ||
                 cnt0Illum != illumPixShm[0].md[0].cnt0 ||
                 cnt0Ff    != ffShm[0].md[0].cnt0       ||
                 cnt0Bg    != bgShm[0].md[0].cnt0)
             {
                 daoInfo("New ff/bg/ref/illumPix detected, repacking buffers.\n");
                 sumRef = 0.0f;
                 for (k = 0; k < nValid; k++)
                 {
                     int   idx = lut[k];
                     int   il  = (illumPixShm[0].array.UI32[idx] == 1);
                     float ff  = ffShm[0].array.F[idx];
                     float ffe = (ff > 0.0f) ? ff : 0.0f;
                     bgp[k] = bgShm[0].array.F[idx];
                     w[k]   = il ? ffe : 0.0f;
                     if (il)
                     {
                         sumRef += refShm[0].array.F[idx];
                     }
                 }
                 invSumRef = (sumRef > 0.0f) ? 1.0f / sumRef : 0.0f;
                 for (k = 0; k < nValid; k++)
                 {
                     int   idx = lut[k];
                     float il  = (float)(illumPixShm[0].array.UI32[idx] == 1);
                     refTerm[k] = refShm[0].array.F[idx] * invSumRef * il;
                 }
                 cnt0Ref   = refShm[0].md[0].cnt0;
                 cnt0Illum = illumPixShm[0].md[0].cnt0;
                 cnt0Ff    = ffShm[0].md[0].cnt0;
                 cnt0Bg    = bgShm[0].md[0].cnt0;
             }

             // ----------------------------------------------------------------
             // Gather raw pixels into a contiguous float buffer (type hoisted)
             // ----------------------------------------------------------------
             gatherRawToFloat(rawf, &rawShm[0], lut, nValid, rawType);

             // ----------------------------------------------------------------
             // Pass 1: calibrate. w[k] is 0 outside the sub-aperture, so cal[k]
             //         is 0 there and the sum needs no per-pixel branch.
             // ----------------------------------------------------------------
             float sum = 0.0f;
             for (k = 0; k < nValid; k++)
             {
                 float v = (rawf[k] - bgp[k]) * w[k];
                 cal[k]  = v;
                 sum    += v;
             }

             // ----------------------------------------------------------------
             // Pass 2: normalize, subtract the (self-normalized) reference.
             //         Both terms are already 0 outside the sub-aperture.
             // ----------------------------------------------------------------
             float invSum = (sum > 0.0f) ? 1.0f / sum : 0.0f;
             float *out   = intensityShm[0].array.F;
             for (k = 0; k < nValid; k++)
             {
                 out[k] = cal[k] * invSum - refTerm[k];
             }

             // Propagate frame counter from raw image
             intensityShm[0].md[0].cnt2 = rawShm[0].md[0].cnt2;

             // Release semaphore, notify consumers
             daoShmSetDataPartFinalize(&intensityShm[0]);

             daoToolsLoopStatusEnd(&status, ", sum %.3f, sumRef %.3f, %d valid pixels", sum, sumRef, nValid);
         }
         else
         {
             daoToolsLoopStatusWait(&status);
         }
     }
     printf("\n");

     free(lut);
     free(rawf);
     free(cal);
     free(w);
     free(bgp);
     free(refTerm);

     daoInfo("EXITING MAIN LOOP\n");
     daoToolsShmRelease(&rawShm);
     daoToolsShmRelease(&ffShm);
     daoToolsShmRelease(&bgShm);
     daoToolsShmRelease(&refShm);
     daoToolsShmRelease(&validPixShm);
     daoToolsShmRelease(&illumPixShm);
     daoToolsShmRelease(&intensityShm);
     return 0;
 }

 /*--------------------------------------------------------------------------*/
 static void DecodeArgs(int argc, char **argv)
 {
     char *str;
     int   a1;

     argv += 1; argc -= 1;

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

         switch (str[1])
         {
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
                 (void)usleep(a1);
                 break;
             case 'S':
                 daoToolsArgNameNext(&argc, &argv, str, rawShmName, sizeof rawShmName);
                 daoToolsArgNameNext(&argc, &argv, str, ffShmName, sizeof ffShmName);
                 daoToolsArgNameNext(&argc, &argv, str, bgShmName, sizeof bgShmName);
                 daoToolsArgNameNext(&argc, &argv, str, refShmName, sizeof refShmName);
                 daoToolsArgNameNext(&argc, &argv, str, validPixShmName, sizeof validPixShmName);
                 daoToolsArgNameNext(&argc, &argv, str, illumPixShmName, sizeof illumPixShmName);
                 daoToolsArgNameNext(&argc, &argv, str, intensityShmName, sizeof intensityShmName);
                 daoInfo("raw        : %s\n", rawShmName);
                 daoInfo("flatfield  : %s\n", ffShmName);
                 daoInfo("background : %s\n", bgShmName);
                 daoInfo("reference  : %s\n", refShmName);
                 daoInfo("validPix   : %s\n", validPixShmName);
                 daoInfo("illumPix   : %s\n", illumPixShmName);
                 daoInfo("intensity  : %s\n", intensityShmName);
                 break;
             case 's':
                 semNb = daoToolsArgInt(&argc, &argv, str);
                 daoInfo("semNb      : %d\n", semNb);
                 break;
             case 'L':
                 if (realTimeLoop() != 0)         /* could not start, or failed (see above) */
                 {
                     exit(EXIT_FAILURE);
                 }
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
     daoToolsSetRtPriority(93);

     sArgv0 = *argv;
     if (argc < 2)
     {                    /* nothing to do: say how */
         ShowHelp();
         return 1;
     }
     DecodeArgs(argc, argv);
     return sExit;
 }
 /*==========================================================================*/
