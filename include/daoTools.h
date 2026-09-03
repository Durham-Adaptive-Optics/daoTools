/**
 * @file    daoTools.h
 * @brief   Durham AO RTC library
 *
 * Durham AO RTC Tools library description file.
 *
 * @author  S. Cetre
 * @date    28/10/2022
 *
 *
 */

#ifndef _DAOTOOLS_H
#define _DAOTOOLS_H
#include "dao.h"

#define RES_MAX_VAL 8192
#define FILTER_ORDER 3

 /**
  * RES filter structure to store precalc, cmd and residual
  */
typedef struct {
    float precal[RES_MAX_VAL];
    float dlCmd[FILTER_ORDER][RES_MAX_VAL];
    float dlRes[FILTER_ORDER][RES_MAX_VAL];
    int step;
} daoFilterHistory;

#ifdef __cplusplus
extern "C"
#endif
int daoToolsLocalName(const char* shmPath, char* localName, int* len);

uint32_t daoComputeChecksum(const void* data, size_t length_bytes);

unsigned daoToolsIp2Int(const char* ip);
void daoToolsInsertShmNamePrefix(const char* base_string,
                                 const char* prefix,
                                 char* final_string);

/**
 * @brief Append one line to a log file, in
 * "<UTC ISO8601 with milliseconds>Z <errorId> <message>" format, with
 * per-tag throttling and size-capped rotation.
 *
 * Throttling: at most one line is actually written per `errorId` per
 * second. Calls arriving inside that 1-second window only increment an
 * in-memory suppressed-count for that tag; when the window closes and a
 * line is finally written, a trailing " (x<N> suppressed)" is appended to
 * the message if any calls were suppressed while waiting. `errorId` is
 * the throttle key, so callers must pass a stable, unique-per-call-site
 * tag (e.g. "DMRX001", "WFSRXHRT016" -- the established hab convention)
 * rather than a dynamically formatted string -- a dynamic tag creates a
 * new throttle-table entry every call and defeats throttling entirely.
 *
 * The throttle table holds 256 entries; if a process somehow exceeds
 * that many distinct tags, further unknown tags are logged unthrottled
 * (fail open) rather than dropped or crashing.
 *
 * Rotation: before writing, if the target file is already larger than
 * 20MB, it is renamed to "<path>.old" (replacing any previous .old) so a
 * fresh file is started. One old generation is kept; there is no
 * unbounded growth. This applies per file path, so every path passed
 * through this function gets it.
 *
 * Note for callers: errorId/fmt content should match the audience of the
 * file being written to -- a path meant as an operator-facing error log
 * should get short, plain-language messages (no errno/hex/byte-counts),
 * while a path meant as a technical/diagnostic log should carry that
 * detail. This function has no way to know which log a given path is, so
 * this is a convention for callers to follow, not something enforced here.
 *
 * @param fileName Path to the log file, or NULL/empty to use "$HOME/dao.log"
 *                 (falls back to "./dao.log" if $HOME is not set).
 * @param errorId Short identifier for the logged event; also the throttle key.
 * @param fmt printf-style format string for the message.
 *
 * @return DAO_SUCCESS on success (including throttled/suppressed calls),
 *         DAO_ERROR if the file could not be opened or written.
 */
int_fast8_t daoLogToFile(const char *fileName, const char *errorId, const char *fmt, ...);

int_fast8_t daoToolsShmCalibrate(IMAGE *inShm,
                                 IMAGE *ffShm,
                                 IMAGE *bgShm,
                                 IMAGE *calShm);

int_fast8_t daoToolsShmCalibrate64(IMAGE *inShm,
                                    IMAGE *ffShm,
                                    IMAGE *bgShm,
                                    IMAGE *calShm);

int_fast8_t daoToolsShmCalibratePws(IMAGE *inShm, 
                                    IMAGE *ffShm,
                                    IMAGE *bgShm, 
                                    IMAGE *maskShm, 
                                    IMAGE *calShm, 
                                    IMAGE *fluxShm);

int_fast8_t daoToolCog(float *img,
                       int height,
                       int width,
                       float *centX,
                       float *centY);
int_fast8_t daoToolsCommandFilter(float *command,
                                  int nbVal,
                                  daoFilterHistory *filterHistory,
                                  float *servoFilter,
                                  float *commandOffset,
                                  float *filteredCommand);
int_fast8_t daoToolsLeakyIntegrator(float *command,
                                     int nbVal,
                                     float leaky,
                                     float gain,
                                     float *commandOffset,
                                     float *filteredCommand);
int_fast8_t daoToolsLeakyIntegratorDouble(double *command,
                                           int nbVal,
                                           double leaky,
                                           double gain,
                                           double *commandOffset,
                                           double *filteredCommand);
int_fast8_t daoToolsLeakyModalIntegrator(float *command,
                                         int nbVal,
                                         float *leaky,
                                         float *gain,
                                         float *commandOffset,
                                         float *filteredCommand);
int_fast8_t daoToolsLeakyModalIntegratorDouble(double *command,
                                              int nbVal,
                                              double *leaky,
                                              double *gain,
                                              double *commandOffset,
                                              double *filteredCommand);
int_fast8_t daoCentroidSpots(float * image,
                             int imageSize,
                             float * ref,
                             int boxSize,
                             int nSuba,
                             float threshold,
                             float * cent);
int_fast8_t daoCentroidSpotsRelative(float *image,
                                     int imageSizeX,
                                     int imageSizeY,
                                     float *ref,
                                     int boxSize,
                                     int nSuba,
                                     float threshold,
                                     float *cent);

int_fast8_t daoCentroidSpotsRelativeRef(float *image,
                                     int imageSizeX,
                                     int imageSizeY,
                                     float *subApCentre,
                                     float *ref,
                                     int boxSize,
                                     int nSuba,
                                     float threshold,
                                     float *cent);

int_fast8_t daoCentroidPws(float *im, float *slopes,
                           float *slopesRef, int *wfsPixId, int *wfsPixIdMap,
                           float *flux, int nbPix,
                           int imSize, int pupSize);

void daoDescrambleOcam2Image(uint8_t img[], int imgRows, int imgCols, uint16_t *img16[],
                             int descrambler[], int descramblerSize, uint16_t output[]);

int_fast8_t daoToolsShmExtract(IMAGE *inShm,
                               IMAGE *maskShm,
                               IMAGE *outShm);

int_fast8_t daoToolsShmSubstractExtractFinalize(IMAGE *inAShm,
                                IMAGE *inBShm,
                                IMAGE *maskShm,
                                IMAGE *outShm,
                                IMAGE *normShm,
                                int normalize);

int_fast8_t daoToolsShmSubstractExtract(IMAGE *inAShm,
                                IMAGE *inBShm,
                                IMAGE *maskShm,
                                IMAGE *outShm,
                                IMAGE *normShm,
                                int normalize);

int_fast8_t daoToolsShmSubstractExtractNorm(IMAGE* inAShm,
                                            IMAGE* inBShm,
                                            IMAGE* maskShm,
                                            IMAGE* outShm);

int_fast8_t daoToolsShmSubstractExtractNormImage(IMAGE* inAShm,
                                            IMAGE* inBShm,
                                            IMAGE* maskShm,
                                            IMAGE* outShm);

/* Enable flush-to-zero / denormals-are-zero on the calling thread. Call once at
 * RT app start-up to avoid subnormal-FP microcode stalls in the hot loop. */
void daoToolsEnableFTZ(void);

int_fast8_t daoToolsShmSubstractExtractDualNorm(IMAGE *inAShm,
                                IMAGE *inBShm,
                                IMAGE *maskShm,
                                IMAGE *outShm,
                                IMAGE *normAShm,
                                IMAGE *normBShm,
                                int normalize);

int_fast8_t daoToolsShmSubstractExtractDualNormFinalize(IMAGE *inAShm,
                                IMAGE *inBShm,
                                IMAGE *maskShm,
                                IMAGE *outShm,
                                IMAGE *normAShm,
                                IMAGE *normBShm,
                                int normalize);

int_fast8_t daoToolsShmSubstractExtractNormA(IMAGE *inAShm,
                                IMAGE *inBShm,
                                IMAGE *maskShm,
                                IMAGE *outShm,
                                IMAGE *normAShm,
                                int normalize);

int_fast8_t daoToolsShmSubstractExtractNormAFinalize(IMAGE *inAShm,
                                IMAGE *inBShm,
                                IMAGE *maskShm,
                                IMAGE *outShm,
                                IMAGE *normAShm,
                                int normalize);

int_fast8_t daoToolsImgNormalize(IMAGE *img);

int_fast8_t daoToolsHighPassFilter(float *H,           
                                   const float *C,  
                                   const float *CPrev,
                                   const float *HPrev,
                                   float fCutoff, 
                                   float fLoop,
                                   int size);

int_fast8_t daoToolsHighPassFilterDouble(double *H,           
                                         const double *C,  
                                         const double *CPrev,
                                         const double *HPrev,
                                         double fCutoff, 
                                         double fLoop,
                                         int size);

int_fast8_t daoDmCombine(IMAGE **imageCube, IMAGE *image, int nbChannel, int nbVal, int removePiston, double clipping);
int_fast8_t daoShmCopyToPosition(IMAGE *imageIn, IMAGE *imageOut, int nbVal, int position, int finalize);

#ifdef __APPLE__

#include <semaphore.h>
#include <time.h>
#include <unistd.h>

// Define missing constants
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

// Fallback for clock_nanosleep
int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec* request, struct timespec* remain);

// Fallback for sched_
#include <sched.h>  // needed for struct sched_param

int sched_setscheduler(pid_t pid, int policy, const struct sched_param* param);

#endif // __APPLE__

void daoRtSetup(int rt_priority);

#endif
