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
    daoTrace("\n");
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
 * @brief 
 * 
 * @param base_string 
 * @param prefix 
 * @param suffix 
 * @param final_string 
 */
void daoToolsInsertShmNamePrefix(const char* base_string, const char* prefix, char* final_string) 
{
    daoTrace("\n");
    const char* suffix = ".im.shm";
    size_t suffix_length = strlen(suffix);
    size_t base_string_length = strlen(base_string);

    if (base_string_length <= suffix_length) 
    {
        printf("Invalid string format.\n");
        return;
    }

    size_t prefix_index = base_string_length - suffix_length;

    snprintf(final_string, 128, "%.*s%s%s", (int)prefix_index, base_string, prefix, suffix);
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
    else if (inShm[0].md[0].atype == _DATATYPE_UINT32)
    {
        for (k = 0; k < calSize; k++)
        {
            calShm[0].array.F[k] = ((float)inShm[0].array.UI32[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT32)
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
 * @brief Calibrate an image by applying flatfield and background to DOUBLE precision
 * 
 * @param inShm raw image 
 * @param ffShm flat field
 * @param bgShm background
 * @param calShm output calibrated image
 * @return int_fast8_t 
 */
 int_fast8_t daoToolsShmCalibrate64(IMAGE *inShm, IMAGE *ffShm, IMAGE *bgShm, IMAGE *calShm)
 {
     daoTrace("\n");
     int k;
     int calSize = calShm[0].md[0].size[0] * calShm[0].md[0].size[1];
     calShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
     if (inShm[0].md[0].atype == _DATATYPE_UINT8)
     {
         for (k = 0; k < calSize; k++)
         {
             calShm[0].array.D[k] = ((double)inShm[0].array.UI8[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
         }
     }
     else if (inShm[0].md[0].atype == _DATATYPE_INT8)
     {
         for (k = 0; k < calSize; k++)
         {
             calShm[0].array.D[k] = ((double)inShm[0].array.SI8[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
         }
     }
     else if (inShm[0].md[0].atype == _DATATYPE_UINT16)
     {
         for (k = 0; k < calSize; k++)
         {
             calShm[0].array.D[k] = ((double)inShm[0].array.UI16[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
         }
     }
     else if (inShm[0].md[0].atype == _DATATYPE_INT16)
     {
         for (k = 0; k < calSize; k++)
         {
             calShm[0].array.D[k] = ((double)inShm[0].array.SI16[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
         }
     }
     else if (inShm[0].md[0].atype == _DATATYPE_UINT32)
     {
         for (k = 0; k < calSize; k++)
         {
             calShm[0].array.D[k] = ((double)inShm[0].array.UI32[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
         }
     }
     else if (inShm[0].md[0].atype == _DATATYPE_INT32)
     {
         for (k = 0; k < calSize; k++)
         {
             calShm[0].array.D[k] = ((double)inShm[0].array.SI32[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
         }
     }
     else if (inShm[0].md[0].atype == _DATATYPE_UINT64)
     {
         for (k = 0; k < calSize; k++)
         {
             calShm[0].array.D[k] = ((double)inShm[0].array.UI64[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
         }
     }
     else if (inShm[0].md[0].atype == _DATATYPE_INT64)
     {
         for (k = 0; k < calSize; k++)
         {
             calShm[0].array.D[k] = ((double)inShm[0].array.SI64[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
         }
     }
     else if (inShm[0].md[0].atype == _DATATYPE_FLOAT)
     {
         for (k = 0; k < calSize; k++)
         {
             calShm[0].array.D[k] = ((double)inShm[0].array.D[k] * ffShm[0].array.F[k]) - bgShm[0].array.D[k];
         }
     }
     else if (inShm[0].md[0].atype == _DATATYPE_DOUBLE)
     {
         for (k = 0; k < calSize; k++)
         {
             calShm[0].array.D[k] = ((double)inShm[0].array.D[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
         }
     }
     daoShmImagePart2ShmFinalize(&calShm[0]);
 
     return DAO_SUCCESS;
 }

/**
 * @brief Calibrate an image by applying flatfield and background
 * 
 * @param inShm raw image 
 * @param ffShm flat field
 * @param bgShm background
 * @param maskShm mask for the calibrated image
 * @param calShm output calibrated image
 * @return int_fast8_t 
 */
int_fast8_t daoToolsShmCalibratePws(IMAGE *inShm, IMAGE *ffShm, IMAGE *bgShm, IMAGE *maskShm, IMAGE *calShm, IMAGE *fluxShm)
{
    daoTrace("\n");
    int k;
    int inSize = inShm[0].md[0].size[0] * inShm[0].md[0].size[1];
    calShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
    if (inShm[0].md[0].atype == _DATATYPE_UINT8)
    {
        for (k = 0; k < inSize; k++)
        {
            if (maskShm[0].array.SI32[k] != -1)
            {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.UI8[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT8)
    {
        for (k = 0; k < inSize; k++)
        {
            if (maskShm[0].array.SI32[k] != -1)
            {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.SI8[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT16)
    {
        for (k = 0; k < inSize; k++)
        {
            if (maskShm[0].array.SI32[k] != -1)
            {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.UI16[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT16)
    {
        for (k = 0; k < inSize; k++)
        {
            if (maskShm[0].array.SI32[k] != -1)
            {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.SI16[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT32)
    {
        for (k = 0; k < inSize; k++)
        {
            if (maskShm[0].array.SI32[k] != -1)
            {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.SI32[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT32)
    {
        for (k = 0; k < inSize; k++)
        {
            if (maskShm[0].array.SI32[k] != -1)
            {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.UI32[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT64)
    {
        for (k = 0; k < inSize; k++)
        {
            if (maskShm[0].array.SI32[k] != -1)
            {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.UI64[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT64)
    {
        for (k = 0; k < inSize; k++)
        {
            if (maskShm[0].array.SI32[k] != -1)
            {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.SI64[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_FLOAT)
    {
        for (k = 0; k < inSize; k++)
        {
            if (maskShm[0].array.SI32[k] != -1)
            {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.F[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_DOUBLE)
    {
        for (k = 0; k < inSize; k++)
        {
            if (maskShm[0].array.SI32[k] != -1)
            {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.D[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
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
    daoTrace("\n");
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
    for (i=0;i<nbVal;i++)
    {
        i2 = 2 * FILTER_ORDER;
        for (i1 = filterHistory->step; i1 < FILTER_ORDER; i1++, i2--)
        {
            filterHistory->precal[i] -= servoFilter[i2] * filterHistory->dlCmd[i1][i];
        }
        for (i1 = 0; i1 < filterHistory->step; i1++, i2--)
        {
            filterHistory->precal[i] -= servoFilter[i2] * filterHistory->dlCmd[i1][i];
        }
        for (i1 = filterHistory->step; i1 < FILTER_ORDER; i1++, i2--)
        {
            filterHistory->precal[i] += servoFilter[i2] * filterHistory->dlRes[i1][i];
        }
        for (i1 = 0; i1 < filterHistory->step; i1++, i2--)
        {
            filterHistory->precal[i] += servoFilter[i2] * filterHistory->dlRes[i1][i];
        }
    }

    return DAO_SUCCESS;
}

/*
 * Apply Integrator to command
 */
int_fast8_t daoToolsLeakyIntegrator(float *command, int nbVal, float leaky, float gain, float *commandOffset, float *filteredCommand)
{
    daoTrace("\n");
    // 
    float commandMoff[nbVal];
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
        commandMoff[pp] = command[pp] - commandOffset[pp];
        filteredCommand[pp] = leaky * filteredCommand[pp] - gain * commandMoff[pp]; // * mixingFactor;
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
    daoTrace("\n");

    // ASSUME centroid organized as follow XYXYXYXY.... (not XXXX...YYYY....)
    // variables to hold counters and start/end
    // coordinates for x and y
    int x, y, x1, x2, y1, y2;
    
    // variables to accumulate moment (numerators) and
    // total energy (denominator)
    float xNumerator, yNumerator, denominator, pixel;
    int n=0;
    int count=0;
    // iterate through the search boxes
    for (n = 0; n < 2*nSuba; n += 2)
    {
        //printf("Search box %d computed in thread number %d\n",n,omp_get_thread_num());
        // floating point accumulators for coordinate*intensity
        // (x_ and yNumerator) and intensity (denominator)
        xNumerator = 0.0;
        yNumerator = 0.0;
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
                yNumerator += pixel*(float)y;
            }
        }
        if (denominator!=0)
        {
            cent[n+count] = xNumerator/denominator - ref[n];
            cent[n+count+1] = yNumerator/denominator - ref[n+1];
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

/**
 * @brief 
 * 
 * @param image 
 * @param imageSize 
 * @param ref 
 * @param boxSize 
 * @param nSuba 
 * @param threshold 
 * @param cent 
 * @return int_fast8_t 
 */
int_fast8_t daoCentroidSpotsRelative(float * image,
                             int imageSize,
                             float * ref,
                             int boxSize,
                             int nSuba,
                             float threshold,
                             float * cent)
{ 
    daoTrace("\n");
    // ASSUME centroid organized as follow XYXYXYXY.... (not XXXX...YYYY....)
    // variables to hold counters and start/end
    // coordinates for x and y
    int x, y, x1, x2, y1, y2;
    float localMax=0;
    float relativeThreshold=0;
    // variables to accumulate moment (numerators) and
    // total energy (denominator)
    float xNumerator, yNumerator, denominator, pixel, flux, weight;
    int n=0;
    int count=0;
    // iterate through the search boxes
    for (n = 0; n < 2*nSuba; n += 2)
    {
        //printf("Search box %d computed in thread number %d\n",n,omp_get_thread_num());
        // floating point accumulators for coordinate*intensity
        // (x_ and yNumerator) and intensity (denominator)
        xNumerator = 0.0;
        yNumerator = 0.0;
        denominator = 0.0;
        flux = 0.0;
        weight = 0.0;
        localMax=0;

        // [x1,x2] and [y1,y2] are closed intervals for
        // computing center of mass (that is, x2 and y2
        // are included in the computation; use <= in
        // associated for loops
        // 
        x1 = (unsigned int)round(ref[n]) - boxSize/2; // truncate to int
        x2 = (unsigned int)round(ref[n]) + boxSize/2;
        y1 = (unsigned int)round(ref[n+1]) - boxSize/2;
        y2 = (unsigned int)round(ref[n+1]) + boxSize/2;

        // Compute max value in teh subaperture
        for (x = x1; x <= x2; x++)
        {
            for (y = y1; y <= y2; y++)
            {
                if ((float)image[y * imageSize + x] > localMax)
                {
                    localMax = (float)image[y * imageSize + x];
                }
            }
        }
        relativeThreshold = threshold * localMax;
        //printf("%d:%d , %d,%d,%d, %d\n", (unsigned int)round(ref[n]), (unsigned int)round(ref[n+1]), x1,x2,y1,y2);
        for (x = x1; x <= x2; x++)
        {
            for (y = y1; y <= y2; y++)
            {
                pixel = (float)image[y * imageSize + x];
                flux += pixel;
                if (pixel < relativeThreshold)
                {
                    pixel = 0;
                }
                else
                {
                    pixel = pixel - relativeThreshold;
                }
                weight += pixel;
                //printf("%f\n", pixel);
                denominator += pixel;
                xNumerator += pixel*(float)x;
                yNumerator += pixel*(float)y;
            }
        }
        if (denominator!=0)
        {
            cent[n+count] = xNumerator/denominator - ref[n];
            cent[n+count+1] = yNumerator/denominator - ref[n+1];
        }
        else
        {
            cent[n+count] = 0;
            cent[n+count+1] = 0;
        }
        cent[n+count+2] = flux;//denominator;
        cent[n+count+3] = weight;//denominator;
        count+=2;
    }
    return DAO_SUCCESS;
}

/**
 * compute slopes at CPU level.
 */
int_fast8_t daoCentroidPws(float *im, float *slopes,
                           float *slopesRef, int *wfsPixId, int *wfsPixIdMap,
                           float *flux, int nbPix,
                           int imSize, int pupSize)
{
    daoTrace("\n");
    float q1,q2,q3,q4;
    // Get our global thread ID
    int id=0;
    float avg=*flux/(4*nbPix);
    daoDebug("avg = %.3f\n", avg);
    // do the comutation only if there is flux
    for (id=0; id<pupSize; id++)
    {    
        if (*flux > 0)
        {
            if (wfsPixId[id] != -1)
            {
                //daoInfo("id=%d, pixid[id]=%d, pixIdMap[id]=%d\n", id, wfsPixId[id], wfsPixIdMap[id]);
                q1=im[wfsPixIdMap[id]];
                q2=im[wfsPixIdMap[id]+imSize];
                q3=im[wfsPixIdMap[id]+imSize*2*imSize];
                q4=im[wfsPixIdMap[id]+imSize*2*imSize+imSize];
                daoInfo("%f,%f,%f,%f\n", q1,q2,q3,q4);
                slopes[wfsPixId[id]] =  (q1+q3-q2-q4) / avg - slopesRef[wfsPixId[id]];
                slopes[wfsPixId[id]+nbPix] =  (q1+q2-q3-q4) / avg - slopesRef[wfsPixId[id]+nbPix];
            }
        }
    }

    return DAO_SUCCESS;
}

/**
 * @brief Descrambles and processes an OCam2 image.
 * 
 * This function takes a flattened 8-bit image array (img), converts it to 
 * a 16-bit format, and then reorders its pixels according to a descrambler array. 
 * The output is a descrambled 16-bit image.
 *
 * @param img A pointer to the flattened 8-bit image array.
 * @param imgRows The number of rows in the image.
 * @param imgCols The number of columns in the image.
 * @param img16 A pointer to a pre-allocated 2D array for intermediate 16-bit image data.
 * @param descrambler An array used for descrambling the image pixels.
 * @param descramblerSize The size of the descrambler array.
 * @param output A pointer to a pre-allocated array where the processed image data will be stored.
 */
void daoDescrambleOcam2Image(uint8_t img[], int imgRows, int imgCols, uint16_t *img16[],
                             int descrambler[], int descramblerSize, uint16_t output[])
{
    int img16Cols = imgCols / 2;

    // Convert flattened img to 16-bit img16
    for (int i = 0; i < imgRows; i++) 
    {
        for (int j = 0; j < imgCols; j += 2) \
        {
            // Combine two adjacent 8-bit values into one 16-bit value
            img16[i][j / 2] = (uint16_t)(img[i * imgCols + j + 1] << 8) + img[i * imgCols + j];
        }
    }

    // Use descrambler array to reorder the pixels in the output
    for (int i = 0; i < descramblerSize; i++) 
    {
        int row = descrambler[i] / img16Cols;
        int col = descrambler[i] % img16Cols;
        output[i] = img16[row][col];
    }
}

/**
 * @brief Extract an image by applying mask
 * 
 * @param inShm raw image 
 * @param maskShm flat field assumes uint32
 * @param outShm output extracted image as vector
 * @return int_fast8_t 
 */
int_fast8_t daoToolsShmExtract(IMAGE *inShm, IMAGE *maskShm, IMAGE *outShm)
{
    daoTrace("\n");
    int k;
    int inSize = inShm[0].md[0].size[0] * inShm[0].md[0].size[1];
    outShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
    int cnt = 0;
    if (inShm[0].md[0].atype == _DATATYPE_UINT8)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.UI8[cnt] = inShm[0].array.UI8[k];
               cnt++; 
           }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT8)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.SI8[cnt] = inShm[0].array.SI8[k];
               cnt++; 
           }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT16)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.UI16[cnt] = inShm[0].array.UI16[k];
               cnt++; 
           }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT16)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.SI16[cnt] = inShm[0].array.SI16[k];
               cnt++; 
           }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT32)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.SI32[cnt] = inShm[0].array.SI32[k];
               cnt++; 
           }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT32)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.UI32[cnt] = inShm[0].array.UI32[k];
               cnt++; 
           }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT64)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.UI64[cnt] = inShm[0].array.UI64[k];
               cnt++; 
           }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT64)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.SI64[cnt] = inShm[0].array.SI64[k];
               cnt++; 
           }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_FLOAT)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.F[cnt] = inShm[0].array.F[k];
               cnt++; 
           }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_DOUBLE)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.D[cnt] = inShm[0].array.D[k];
               cnt++; 
           }
        }
    }
    daoShmImagePart2ShmFinalize(&outShm[0]);

    return DAO_SUCCESS;
}

/**
 * @brief Subastract and Extract an image by applying mask, assume same type for A and B
 * 
 * @param inAShm image 
 * @param inBShm image  - to substract
 * @param maskShm flat field assumes uint32
 * @param outShm output extracted image as vector
 * @return int_fast8_t 
 */
int_fast8_t daoToolsShmSubstractExtract(IMAGE *inAShm, IMAGE *inBShm, IMAGE *maskShm, IMAGE *outShm)
{
    daoTrace("\n");
    int k;
    int inSize = inAShm[0].md[0].size[0] * inAShm[0].md[0].size[1];
    outShm[0].md[0].cnt2 = inAShm[0].md[0].cnt2;
    int cnt = 0;
    if (inAShm[0].md[0].atype == _DATATYPE_UINT8)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.UI8[cnt] = inAShm[0].array.UI8[k] - inBShm[0].array.UI8[k];
               cnt++; 
           }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_INT8)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.SI8[cnt] = inAShm[0].array.SI8[k] - inBShm[0].array.SI8[k];
               cnt++; 
           }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_UINT16)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.UI16[cnt] = inAShm[0].array.UI16[k] - inBShm[0].array.UI16[k];
               cnt++; 
           }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_INT16)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.SI16[cnt] = inAShm[0].array.SI16[k] - inBShm[0].array.SI16[k];
               cnt++; 
           }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_INT32)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.SI32[cnt] = inAShm[0].array.SI32[k] - inBShm[0].array.SI32[k];
               cnt++; 
           }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_UINT32)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.UI32[cnt] = inAShm[0].array.UI32[k] - inBShm[0].array.UI32[k];
               cnt++; 
           }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_UINT64)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.UI64[cnt] = inAShm[0].array.UI64[k] - inBShm[0].array.UI64[k];
               cnt++; 
           }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_INT64)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.SI64[cnt] = inAShm[0].array.SI64[k] - inBShm[0].array.SI64[k];
               cnt++; 
           }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_FLOAT)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.F[cnt] = inAShm[0].array.F[k] - inBShm[0].array.F[k];
               cnt++; 
           }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_DOUBLE)
    {
        for (k = 0; k < inSize; k++)
        {
           if (maskShm[0].array.UI32[k] == 1)
           {
               outShm[0].array.D[cnt] = inAShm[0].array.D[k] - inBShm[0].array.D[k];
               cnt++; 
           }
        }
    }
    daoShmImagePart2ShmFinalize(&outShm[0]);

    return DAO_SUCCESS;
}
 
#ifdef __APPLE__

#include <errno.h>
#include <stdio.h>
#include <mach/mach_time.h>
#include <sys/time.h>

// Fallback for sched_setscheduler
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param) {
    // macOS does not support real-time policies (SCHED_FIFO, etc.)
    // Just log and return success as a no-op
    (void)pid;
    (void)policy;
    (void)param;
    fprintf(stderr, "Warning: sched_setscheduler is not supported on macOS, ignoring.\n");
    return 0;
}

// Fallback for clock_nanosleep
int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec *request, struct timespec *remain) {
    if (flags == TIMER_ABSTIME) {
        // Absolute time mode: wait until the specified time
        struct timespec now;
        clock_gettime(clock_id, &now);

        time_t sec_diff = request->tv_sec - now.tv_sec;
        long nsec_diff = request->tv_nsec - now.tv_nsec;

        if (nsec_diff < 0) {
            sec_diff -= 1;
            nsec_diff += 1000000000L;
        }

        if (sec_diff < 0 || (sec_diff == 0 && nsec_diff <= 0)) {
            return 0; // Already past deadline
        }

        struct timespec delay = { sec_diff, nsec_diff };
        return nanosleep(&delay, remain);
    } else {
        // Relative sleep
        return nanosleep(request, remain);
    }
}

#endif // __APPLE__
