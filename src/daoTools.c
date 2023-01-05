/*****************************************************************************
  DAO project
  RTC library tools
  S.Cetre
 *****************************************************************************/

/*==========================================================================*/
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
 * @brief compute 
 * 
 * @param img 
 * @param height 
 * @param width 
 * @return int_fast8_t 
 */
int_fast8_t daoToolCog(float *img, int height, int width) 
{
	float sumX;
	float sumY;
	float sumPix;

	for (y=0; y < height; y++) 
    {
		for (x=0; x < width; x++) 
        {
			A =  img[x*widht + y];
			sumPix += A;
			sumX += (x * A);
			sumY += (y * A);
		}
	}
	daoInfo("Center X:" + img.width/2 + " Centroid X:" + sumX / sum_pix);
	daoInfo("Center Y:" + img.height/2 + " Centroid Y:" + sum_y / sum_pix);

    return DAO_SUCCESS;
}

