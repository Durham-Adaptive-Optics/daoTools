/*****************************************************************************
  DAO project
  RTC library tools
  S.Cetre
 *****************************************************************************/

/*==========================================================================*/
#include <stdio.h>
#include <string.h>
#include <math.h>


#include "daoTools.h"

/**
 * @brief convert IP address (AAA.BBB.CCC.DDD) to integer
 * 
 * @param ip 
 * @return unsigned 
 */
unsigned daoToolsIp2Int (const char * ip)
{
    /* The return value. */
    unsigned v = 0;
    /* The count of the number of bytes processed. */
    int i;
    /* A pointer to the next digit to process. */
    const char * start;

    start = ip;
    for (i = 0; i < 4; i++) {
        /* The digit being processed. */
        char c;
        /* The value of this byte. */
        int n = 0;
        while (1) {
            c = * start;
            start++;
            if (c >= '0' && c <= '9') {
                n *= 10;
                n += c - '0';
            }
            /* We insist on stopping at "." if we are still parsing
               the first, second, or third numbers. If we have reached
               the end of the numbers, we will allow any character. */
            else if ((i < 3 && c == '.') || i == 3) {
                break;
            }
            else {
                return DAO_ERROR;
            }
        }
        if (n >= 256) {
            return DAO_ERROR;
        }
        v *= 256;
        v += n;
    }
    return v;
}


/**
 * @brief Calibrate an image by applying flatfield and background
 * 
 * @param inShm raw image 
 * @param ffShm flat field
 * @param bgShm background
 * @param calShm output calibrated image
 * @return int_fast8_t 
 */
int_fast8_t daoToolsShmCalibrate(IMAGE *inShm, IMAGE *ffShm, IMAGE *bgShm, IMAGE *calShm)
{
    daoTrace("\n");
    int k;
    int calSize = calShm[0].md[0].size[0] * calShm[0].md[0].size[1];
    calShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
    if (inShm[0].md[0].atype == _DATATYPE_UINT8)
    {
        for (k = 0; k < calSize; k++)
        {
            calShm[0].array.F[k] = ((float)inShm[0].array.UI8[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT8)
    {
        for (k = 0; k < calSize; k++)
        {
            calShm[0].array.F[k] = ((float)inShm[0].array.SI8[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT16)
    {
        for (k = 0; k < calSize; k++)
        {
            calShm[0].array.F[k] = ((float)inShm[0].array.UI16[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT16)
    {
        for (k = 0; k < calSize; k++)
        {
            calShm[0].array.F[k] = ((float)inShm[0].array.SI16[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT32)
    {
        for (k = 0; k < calSize; k++)
        {
            calShm[0].array.F[k] = ((float)inShm[0].array.UI32[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT32)
    {
        for (k = 0; k < calSize; k++)
        {
            calShm[0].array.F[k] = ((float)inShm[0].array.SI32[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT64)
    {
        for (k = 0; k < calSize; k++)
        {
            calShm[0].array.F[k] = ((float)inShm[0].array.UI64[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT64)
    {
        for (k = 0; k < calSize; k++)
        {
            calShm[0].array.F[k] = ((float)inShm[0].array.SI64[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_FLOAT)
    {
        for (k = 0; k < calSize; k++)
        {
            calShm[0].array.F[k] = ((float)inShm[0].array.F[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_DOUBLE)
    {
        for (k = 0; k < calSize; k++)
        {
            calShm[0].array.F[k] = ((float)inShm[0].array.D[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
        }
    }
    daoShmImagePart2ShmFinalize(&calShm[0]);

    return DAO_SUCCESS;
}

/**
 * @brief compute centroif of an image 
 * 
 * @param img 
 * @param height 
 * @param width 
 * @return int_fast8_t 
 */
int_fast8_t daoToolCog(float *img, int height, int width, float *centX, float *centY) 
{
	float sumX=0;
	float sumY=0;
	float sumPix=0;
    float A;
    int x,y;

	for (y=0; y < height; y++) 
    {
		for (x=0; x < width; x++) 
        {
			A =  img[x*width + y];
			sumPix += A;
			sumX += (x * A);
			sumY += (y * A);
		}
	}
    if (sumPix != 0)
    {
        *centX = sumX/sumPix;
        *centY = sumY/sumPix;
        daoDebug("Center X: %d  Centroid X: %f\n", width / 2, sumX / sumPix);
        daoDebug("Center Y: %d  Centroid Y: %f\n", height / 2, sumY / sumPix);
    }
    else
    {
        daoError("Not enought flux to compute centroid");
        return DAO_ERROR;
    }

    return DAO_SUCCESS;
}

/*
 * Apply 3rd order filter to command
 */
int_fast8_t daoToolsCommandFilter(float *command, int nbVal, daoFilterHistory *filterHistory, float *servoFilter, float *commandOffset, float *filteredCommand)
{
    daoTrace("\n");
    // 
    float commandMoff[nbVal];
    int c=0;
    int pp;
    for(pp = 0; pp < nbVal; pp++)
    {
        // Check that values to filter are
        // number... safety check to stop propagating nan
        if (isnan(command[pp]))
        {
            command[pp] = 0.0;
        }
        // Substract command offset
        commandMoff[c] = command[c] - commandOffset[pp];
        filterHistory->dlCmd[filterHistory->step][c] = filteredCommand[pp] = (filterHistory->precal[c] - servoFilter[0] * commandMoff[c]); // * mixingFactor;
        filterHistory->precal[c] = 0.0;
        c += 1;
    }
    memcpy(filterHistory->dlRes[filterHistory->step], commandMoff, nbVal);

    filterHistory->step++;
    if (filterHistory->step==FILTER_ORDER)
    {
        filterHistory->step=0;
    }
    // precomputation of servo loop filter for next filterHistory->step
    int i1, i2, i;
    c=0;
    for (i=0;i<nbVal;i++)
    {
        i2 = 2 * FILTER_ORDER;
        for (i1 = filterHistory->step; i1 < FILTER_ORDER; i1++, i2--)
        {
            filterHistory->precal[c] -= servoFilter[i2] * filterHistory->dlCmd[i1][c];
        }
        for (i1 = 0; i1 < filterHistory->step; i1++, i2--)
        {
            filterHistory->precal[c] -= servoFilter[i2] * filterHistory->dlCmd[i1][c];
        }
        for (i1 = filterHistory->step; i1 < FILTER_ORDER; i1++, i2--)
        {
            filterHistory->precal[c] += servoFilter[i2] * filterHistory->dlRes[i1][c];
        }
        for (i1 = 0; i1 < filterHistory->step; i1++, i2--)
        {
            filterHistory->precal[c] += servoFilter[i2] * filterHistory->dlRes[i1][c];
        }
        c++;
    }

    return DAO_SUCCESS;
}




int_fast8_t daoCentroidSpots(float * image,
                             int imageSize,
                             float * ref,
                             int boxSize,
                             int nSuba,
                             float threshold,
                             float * cent)
{ 
    // ASSUME centroid organized as follow XYXYXYXY.... (not XXXX...YYYY....)
    // variables to hold counters and start/end
    // coordinates for x and y
    int x, y, x1, x2, y1, y2;
    
    // variables to accumulate moment (numerators) and
    // total energy (denominator)
    float xNumerator, yNumarator, denominator, pixel;
    int n=0;
    int count=0;
    // iterate through the search boxes
    for (n = 0; n < 2*nSuba; n += 2)
    {
        //printf("Search box %d computed in thread number %d\n",n,omp_get_thread_num());
        // floating point accumulators for coordinate*intensity
        // (x_ and yNumarator) and intensity (denominator)
        xNumerator = 0.0;
        yNumarator = 0.0;
        denominator = 0.0;

        // [x1,x2] and [y1,y2] are closed intervals for
        // computing center of mass (that is, x2 and y2
        // are included in the computation; use <= in
        // associated for loops
        // 
        x1 = (unsigned int)round(ref[n]) - boxSize/2; // truncate to int
        x2 = (unsigned int)round(ref[n]) + boxSize/2;
        y1 = (unsigned int)round(ref[n+1]) - boxSize/2;
        y2 = (unsigned int)round(ref[n+1]) + boxSize/2;

        //printf("%d:%d , %d,%d,%d, %d\n", (unsigned int)round(ref[n]), (unsigned int)round(ref[n+1]), x1,x2,y1,y2);
        for (x = x1; x <= x2; x++)
        {
            for (y = y1; y <= y2; y++)
            {
                pixel = (float)image[y * imageSize + x];
                if (pixel < threshold)
                {
                    pixel = 0;
                }
                //printf("%f\n", pixel);
                denominator += pixel;
                xNumerator += pixel*(float)x;
                yNumarator += pixel*(float)y;
            }
        }
        if (denominator!=0)
        {
            cent[n+count] = xNumerator/denominator - ref[n];
            cent[n+count+1] = yNumarator/denominator - ref[n+1];
        }
        else
        {
            cent[n+count] = 0;
            cent[n+count+1] = 0;
        }
        cent[n+count+2] = denominator;
        count+=1;
    }
    return DAO_SUCCESS;
}