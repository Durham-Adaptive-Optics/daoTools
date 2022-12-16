/*****************************************************************************
  DAO project
  RTC library funciton
  S.Cetre
 *****************************************************************************/

/*==========================================================================*/
#include "daoTools.h"

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
