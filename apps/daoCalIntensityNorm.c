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
    daoRtcCalIntensityNorm -S <raw> <ff> <bg> <ref> <validPix> <illumPix> <intensity> -s <semNb> -L
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

 char rawShmName[64];
 char ffShmName[64];
 char bgShmName[64];
 char refShmName[64];
 char validPixShmName[64];
 char illumPixShmName[64];
 char intensityShmName[64];
 int  semNb = 0;

 static int end = 0;
 static void endme(int _a) { (void)_a; end = 1; }

 static char *sArgv0 = NULL;

 /*--------------------------------------------------------------------------*/
 static void ShowHelp(void)
 {
     daoInfo("%s of " __DATE__ " at " __TIME__ "\n", sArgv0);
     daoInfo("   Calibrate (bg/ff) + extract + normalize intensity in one pass,\n");
     daoInfo("   with the reference normalized against its own illuminated sum.\n");
     daoInfo("   arguments:\n");
     daoInfo("   -h               display this message and exit\n");
     daoInfo("   -d <level>       debug level\n");
     daoInfo("   -s <semNb>       semaphore number on raw SHM\n");
     daoInfo("   -S <raw> <ff> <bg> <ref> <validPix> <illumPix> <intensity>\n");
     daoInfo("   -L               start real-time loop\n");
     daoInfo("\n");
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
     signal(SIGINT, endme);

     IMAGE *rawShm       = (IMAGE*) malloc(sizeof(IMAGE));
     IMAGE *ffShm        = (IMAGE*) malloc(sizeof(IMAGE));
     IMAGE *bgShm        = (IMAGE*) malloc(sizeof(IMAGE));
     IMAGE *refShm       = (IMAGE*) malloc(sizeof(IMAGE));
     IMAGE *validPixShm  = (IMAGE*) malloc(sizeof(IMAGE));
     IMAGE *illumPixShm  = (IMAGE*) malloc(sizeof(IMAGE));
     IMAGE *intensityShm = (IMAGE*) malloc(sizeof(IMAGE));

     daoShmShm2Img(rawShmName,       &rawShm[0]);
     daoShmShm2Img(ffShmName,        &ffShm[0]);
     daoShmShm2Img(bgShmName,        &bgShm[0]);
     daoShmShm2Img(refShmName,       &refShm[0]);
     daoShmShm2Img(validPixShmName,  &validPixShm[0]);
     daoShmShm2Img(illumPixShmName,  &illumPixShm[0]);
     daoShmShm2Img(intensityShmName, &intensityShm[0]);

     int imSize = rawShm[0].md[0].size[0] * rawShm[0].md[0].size[1];

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
     struct timespec timeout;
     struct timespec tPrint;
     double elapsedTime, compTime;
     int waitCounter = 0;

     // Status is averaged over ~1 s to avoid per-frame jitter and I/O
     double accComp    = 0.0;   // sum of compTime  [ms]
     double accElapsed = 0.0;   // sum of frame periods [ms]
     double maxComp    = 0.0;   // worst-case compTime in the window [ms]
     long   accN       = 0;

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
     unsigned long frameCnt  = 0;

     clock_gettime(CLOCK_REALTIME, &t[1]);
     tPrint = t[1];

     daoInfo("Entering real-time loop\n");
     while (end == 0)
     {
         t[0] = t[1];
         clock_gettime(CLOCK_REALTIME, &timeout);
         timeout.tv_sec += 1;

         if (daoShmWaitForSemaphoreTimeout(rawShm, semNb, &timeout) != -1)
         {
             clock_gettime(CLOCK_REALTIME, &t[2]);

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
             daoShmImagePart2ShmFinalize(&intensityShm[0]);

             clock_gettime(CLOCK_REALTIME, &t[1]);
             elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3
                         + (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
             compTime    = (t[1].tv_sec - t[2].tv_sec) * 1e3
                         + (t[1].tv_nsec - t[2].tv_nsec) / 1e6;

             // Accumulate stats; print an average once per second so the
             // status line and its write syscall stay off the RT path.
             frameCnt++;
             accComp    += compTime;
             accElapsed += elapsedTime;
             if (compTime > maxComp) maxComp = compTime;
             accN++;

             double sincePrint = (t[1].tv_sec - tPrint.tv_sec) * 1e3
                               + (t[1].tv_nsec - tPrint.tv_nsec) / 1e6;
             if (sincePrint >= 1000.0)
             {
                 double avgComp = accComp / accN;
                 double avgFps  = 1e3 / (accElapsed / accN);
                 printf("\rcomp avg=%.1f us  max=%.1f us  fps=%.1f Hz  "
                        "sum=%.3f  sumRef=%.3f  nValid=%d      ",
                        avgComp * 1000.0, maxComp * 1000.0, avgFps,
                        sum, sumRef, nValid);
                 fflush(stdout);
                 accComp = accElapsed = maxComp = 0.0;
                 accN = 0;
                 tPrint = t[1];
             }
         }
         else
         {
             waitCounter++;
             printf("\rWAIT %d", waitCounter);
             fflush(stdout);
         }
     }

     free(lut);
     free(rawf);
     free(cal);
     free(w);
     free(bgp);
     free(refTerm);

     daoInfo("\nEXITING MAIN LOOP\n");
     fflush(stdout);
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
                 (void)sscanf(*argv++, "%d", &daoLogLevel); argc -= 1;
                 break;
             case 'l':
                 daoInfo("%s\n", *argv);
                 argv += 1; argc -= 1;
                 break;
             case 'u':
                 (void)sscanf(*argv++, "%d", &a1); argc -= 1;
                 (void)usleep(a1);
                 break;
             case 'S':
                 (void)sscanf(*argv++, "%s", rawShmName);       argc -= 1;
                 (void)sscanf(*argv++, "%s", ffShmName);        argc -= 1;
                 (void)sscanf(*argv++, "%s", bgShmName);        argc -= 1;
                 (void)sscanf(*argv++, "%s", refShmName);        argc -= 1;
                 (void)sscanf(*argv++, "%s", validPixShmName);  argc -= 1;
                 (void)sscanf(*argv++, "%s", illumPixShmName); argc -= 1;
                 (void)sscanf(*argv++, "%s", intensityShmName); argc -= 1;
                 daoInfo("raw        : %s\n", rawShmName);
                 daoInfo("flatfield  : %s\n", ffShmName);
                 daoInfo("background : %s\n", bgShmName);
                 daoInfo("reference  : %s\n", refShmName);
                 daoInfo("validPix   : %s\n", validPixShmName);
                 daoInfo("illumPix   : %s\n", illumPixShmName);
                 daoInfo("intensity  : %s\n", intensityShmName);
                 break;
             case 's':
                 (void)sscanf(*argv++, "%d", &semNb); argc -= 1;
                 daoInfo("semNb      : %d\n", semNb);
                 break;
             case 'L':
                 realTimeLoop();
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
     DecodeArgs(argc, argv);
     return sExit;
 }
 /*==========================================================================*/
