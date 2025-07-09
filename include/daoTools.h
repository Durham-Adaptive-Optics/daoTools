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
typedef struct
{
    float precal[RES_MAX_VAL];
    float dlCmd[FILTER_ORDER][RES_MAX_VAL];
    float dlRes[FILTER_ORDER][RES_MAX_VAL];
    int step;
} daoFilterHistory;

uint32_t daoComputeChecksum(const void *data, size_t length_bytes);

unsigned daoToolsIp2Int(const char * ip); 
void daoToolsInsertShmNamePrefix(const char* base_string,
                                 const char* prefix,
                                 char* final_string); 

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
                                     int imageSize,
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

int_fast8_t daoToolsShmSubstractExtract(IMAGE *inAShm,
                                        IMAGE *inBShm,
                                        IMAGE *maskShm,
                                        IMAGE *outShm);

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

#ifdef __APPLE__

#include <semaphore.h>
#include <time.h>
#include <unistd.h>

// Define missing constants
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

// Fallback for clock_nanosleep
int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec *request, struct timespec *remain);

// Fallback for sched_
#include <sched.h>  // needed for struct sched_param

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);

#endif // __APPLE__

                             
#endif
