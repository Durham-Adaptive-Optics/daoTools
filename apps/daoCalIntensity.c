/*****************************************************************************
  DAO project - daoCalIntensity.c
  s.cetre

  Real-time loop: calibrate (bg/ff) + extract + normalize intensity
  for valid pupil pixels in a single pass — no intermediate SHM.

  Input SHMs:
    raw       : raw camera image (float or uint16)
    ff        : flat field (float)
    bg        : background (float)
    validPix  : valid pixel mask (uint32, 1=valid)
    illumPix: valid sub-aperture pixel mask (uint32, 1=in subaperture)

  Output SHM:
    intensity : normalized intensity for each valid pixel (float)

  Usage:
    daoRtcCalIntensity -S <raw> <ff> <bg> <validPix> <illumPix> <intensity> -s <semNb> -L
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
     daoInfo("   Calibrate (bg/ff) + extract + normalize intensity in one pass.\n");
     daoInfo("   arguments:\n");
     daoInfo("   -h               display this message and exit\n");
     daoInfo("   -d <level>       debug level\n");
     daoInfo("   -s <semNb>       semaphore number on raw SHM\n");
     daoInfo("   -S <raw> <ff> <bg> <ref> <validPix> <illumPix> <intensity>\n");
     daoInfo("   -L               start real-time loop\n");
     daoInfo("\n");
 }
 
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
 
     // Temporary calibrated pixel buffer (avoids recomputing for normalization)
     float *cal = (float*) malloc(nValid * sizeof(float));
     float *ref = (float*) malloc(nValid * sizeof(float));
 
     // Detect raw image type
     int rawType = rawShm[0].md[0].atype;
     daoInfo("Raw image type: %d\n", rawType);
 
     // Macro to extract one raw pixel as float, for any data type
     #define RAW_TO_FLOAT(shm, idx)                                          \
         ( (rawType == _DATATYPE_UINT8)   ? (float)(shm).array.UI8[idx]  :  \
           (rawType == _DATATYPE_INT8)    ? (float)(shm).array.SI8[idx]  :  \
           (rawType == _DATATYPE_UINT16)  ? (float)(shm).array.UI16[idx] :  \
           (rawType == _DATATYPE_INT16)   ? (float)(shm).array.SI16[idx] :  \
           (rawType == _DATATYPE_UINT32)  ? (float)(shm).array.UI32[idx] :  \
           (rawType == _DATATYPE_INT32)   ? (float)(shm).array.SI32[idx] :  \
           (rawType == _DATATYPE_UINT64)  ? (float)(shm).array.UI64[idx] :  \
           (rawType == _DATATYPE_INT64)   ? (float)(shm).array.SI64[idx] :  \
           (rawType == _DATATYPE_FLOAT)   ? (shm).array.F[idx]           :  \
           (rawType == _DATATYPE_DOUBLE)  ? (float)(shm).array.D[idx]    :  \
           0.0f )
 
     struct timespec t[3];
     struct timespec timeout;
     double elapsedTime, compTime;
     int waitCounter = 0;
 
     clock_gettime(CLOCK_REALTIME, &t[1]);
 
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
             // Pass 1: calibrate valid pixels only, accumulate sum over
             //         sub-aperture pixels
             // ----------------------------------------------------------------
             float sum = 0.0f;
 
             for (k = 0; k < nValid; k++)
             {
                 int idx  = lut[k];
                 float ff = ffShm[0].array.F[idx];
                 float bg = bgShm[0].array.F[idx];
                 float r = refShm[0].array.F[idx];
                 float raw = RAW_TO_FLOAT(rawShm[0], idx);
                 float v  = (ff > 0.0f) ? (raw - bg) * ff : 0.0f;
                 cal[k] = v;
                 ref[k] = r;
                 if (illumPixShm[0].array.UI32[idx] == 1)
                 {
                     sum += v;
                 }
             }
 
             // ----------------------------------------------------------------
             // Pass 2: normalize and write to output SHM
             // ----------------------------------------------------------------
             float invSum = (sum > 0.0f) ? 1.0f / sum : 0.0f;
             for (k = 0; k < nValid; k++)
             {
                 int idx = lut[k];
                 intensityShm[0].array.F[k] =
                     (cal[k] - ref[k]) * invSum * (float)illumPixShm[0].array.UI32[idx];
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
 
             printf("\rcomp=%.1f us  fps=%.1f Hz  sum=%.3f  nValid=%d     ",
                    compTime * 1000.0, 1e3 / elapsedTime, sum, nValid);
             fflush(stdout);
         }
         else
         {
             waitCounter++;
             printf("\rWAIT %d", waitCounter);
             fflush(stdout);
         }
     }
 
     free(lut);
     free(cal);
 
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
     int RT_priority = 93;
     struct sched_param schedpar;
     schedpar.sched_priority = RT_priority;
     sched_setscheduler(0, SCHED_FIFO, &schedpar);
 
     sArgv0 = *argv;
     DecodeArgs(argc, argv);
     return sExit;
 }
 /*==========================================================================*/