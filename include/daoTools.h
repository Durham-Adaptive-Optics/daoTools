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

unsigned daoToolsIp2Int(const char * ip); 

int_fast8_t daoToolsShmCalibrate(IMAGE *inShm, IMAGE *ffShm, IMAGE *bgShm, IMAGE *calShm);

#endif