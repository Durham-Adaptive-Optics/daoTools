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
#include "daoBase.h"
#include "daoShm.h"

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

int_fast8_t daoToolsShmCalibrate(IMAGE *inShm, IMAGE *ffShm, IMAGE *bgShm, IMAGE *calShm);
int_fast8_t daoToolCog(float *img, int height, int width, float *centX, float *centY);
int_fast8_t daoToolsCommandFilter(float *command, int nbVal, daoFilterHistory *filterHistory, float *servoFilter, float *commandOffset, float *filteredCommand);

#endif