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

unsigned daoToolsIp2Int(const char * ip); 
void daoToolsInsertShmNamePrefix(const char* base_string,
                                 const char* prefix,
                                 char* final_string); 

int_fast8_t daoToolsShmCalibrate(IMAGE *inShm,
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
#endif
