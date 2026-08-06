/*****************************************************************************
  DAO project
  RTC library tools
  S.Cetre
 *****************************************************************************/

 /*==========================================================================*/
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <sched.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <time.h>
#include "daoTools.h"

/* @brief Extracts the local name from a shared memory absolute path
 * (format: '/.../<localName>.im.shm'). Pass NULL for 'localName' with
 * valid 'len' to query required buffer size; then re-invoke with a
 * correctly-sized buffer and NULL for 'len'.
 *
 * @shmPath Shared memory absolute path.
 * @localName Output buffer for the local name (or NULL to query size).
 * @len Output for required buffer length (or NULL when writing result).
*/
int daoToolsLocalName(const char* shmPath, char* localName, int* len) {
    const char* pathEnd = strrchr(shmPath, '/');
    const char* nameEnd = strchr(shmPath, '.');
    if (!pathEnd || !nameEnd) {
        return DAO_ERROR;
    }

    int nCharsCopy = nameEnd - pathEnd; // note: extra char for null-terminator.

    if (!localName && len) {
        *len = nCharsCopy;
    }
    else if (localName && !len) {
        memcpy(localName, pathEnd + 1, nCharsCopy - 1);
        localName[nCharsCopy - 1] = '\0';
    }
    else {
        return DAO_ERROR;
    }

    return DAO_SUCCESS;
}

/** Compute 32-bit XOR checksum over buffer
 */
uint32_t daoComputeChecksum(const void* data, size_t length_bytes) {
    const uint32_t* words = (const uint32_t*)data;
    size_t num_words = length_bytes / 4;
    uint32_t checksum = 0;

    // XOR each 32-bit word
    for (size_t i = 0; i < num_words; ++i) {
        checksum ^= words[i];
    }

    return checksum;
}

/**
 * @brief convert IP address (AAA.BBB.CCC.DDD) to integer
 *
 * @param ip
 * @return unsigned
 */
unsigned daoToolsIp2Int(const char* ip) {
    daoTrace("\n");
    /* The return value. */
    unsigned v = 0;
    /* The count of the number of bytes processed. */
    int i;
    /* A pointer to the next digit to process. */
    const char* start;

    start = ip;
    for (i = 0; i < 4; i++) {
        /* The digit being processed. */
        char c;
        /* The value of this byte. */
        int n = 0;
        while (1) {
            c = *start;
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
 * @brief Insert a prefix before the `.im.shm` suffix of a shared-memory name.
 *
 * @param base_string Base shared-memory object name.
 * @param prefix String inserted before the fixed `.im.shm` suffix.
 * @param final_string Output buffer receiving the rewritten name.
 */
void daoToolsInsertShmNamePrefix(const char* base_string, const char* prefix, char* final_string) {
    daoTrace("\n");
    const char* suffix = ".im.shm";
    size_t suffix_length = strlen(suffix);
    size_t base_string_length = strlen(base_string);

    if (base_string_length <= suffix_length) {
        printf("Invalid string format.\n");
        return;
    }

    size_t prefix_index = base_string_length - suffix_length;

    snprintf(final_string, 128, "%.*s%s%s", (int)prefix_index, base_string, prefix, suffix);
}

/**
 * @brief Append one line to a log file, in
 * "<UTC ISO8601 with milliseconds>Z <errorId> <message>" format.
 *
 * A single write() on an O_APPEND fd is atomic on a local filesystem, so
 * any number of processes can share the same log file without interleaving lines.
 *
 * @param fileName Path to the log file, or NULL/empty to use "$HOME/dao.log"
 *                 (falls back to "./dao.log" if $HOME is not set).
 * @param errorId Short identifier for the logged event.
 * @param fmt printf-style format string for the message.
 *
 * @return DAO_SUCCESS on success, DAO_ERROR if the file could not be opened.
 */
int_fast8_t daoLogToFile(const char *fileName, const char *errorId, const char *fmt, ...) {
    char msg[400];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    for (char *p = msg; *p != '\0'; p++) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm tmUtc;
    gmtime_r(&now.tv_sec, &tmUtc);
    char ts[32];
    size_t tsLen = strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tmUtc);
    snprintf(ts + tsLen, sizeof(ts) - tsLen, ".%03ldZ", now.tv_nsec / 1000000);

    char line[512];
    int n = snprintf(line, sizeof(line), "%s %s %s\n", ts, errorId, msg);
    if (n <= 0) return DAO_ERROR;
    if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;

    char defaultPath[256];
    if (fileName == NULL || fileName[0] == '\0') {
        const char *home = getenv("HOME");
        snprintf(defaultPath, sizeof(defaultPath), "%s/dao.log", home ? home : ".");
        fileName = defaultPath;
    }

    int fd = open(fileName, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return DAO_ERROR;
    if (write(fd, line, (size_t)n) < 0) {
        daoError("Failed to write log %s: %s\n", fileName, strerror(errno));
        close(fd);
        return DAO_ERROR;
    }
    close(fd);
    return DAO_SUCCESS;
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
int_fast8_t daoToolsShmCalibrate(IMAGE* inShm, IMAGE* ffShm, IMAGE* bgShm, IMAGE* calShm) {
    daoTrace("\n");
    int k;
    int calSize = calShm[0].md[0].size[0] * calShm[0].md[0].size[1];
    calShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
    if (inShm[0].md[0].atype == _DATATYPE_UINT8) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.F[k] = ((float)inShm[0].array.UI8[k] - bgShm[0].array.F[k]) * ffShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT8) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.F[k] = ((float)inShm[0].array.SI8[k] - bgShm[0].array.F[k]) * ffShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT16) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.F[k] = ((float)inShm[0].array.UI16[k] - bgShm[0].array.F[k]) * ffShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT16) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.F[k] = ((float)inShm[0].array.SI16[k] - bgShm[0].array.F[k]) * ffShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT32) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.F[k] = ((float)inShm[0].array.UI32[k] - bgShm[0].array.F[k]) * ffShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT32) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.F[k] = ((float)inShm[0].array.SI32[k] - bgShm[0].array.F[k]) * ffShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT64) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.F[k] = ((float)inShm[0].array.UI64[k] - bgShm[0].array.F[k]) * ffShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT64) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.F[k] = ((float)inShm[0].array.SI64[k] - bgShm[0].array.F[k]) * ffShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_FLOAT) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.F[k] = ((float)inShm[0].array.F[k] - bgShm[0].array.F[k]) * ffShm[0].array.F[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_DOUBLE) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.F[k] = ((float)inShm[0].array.D[k] - bgShm[0].array.F[k]) * ffShm[0].array.F[k];
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
int_fast8_t daoToolsShmCalibrate64(IMAGE* inShm, IMAGE* ffShm, IMAGE* bgShm, IMAGE* calShm) {
    daoTrace("\n");
    int k;
    int calSize = calShm[0].md[0].size[0] * calShm[0].md[0].size[1];
    calShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
    if (inShm[0].md[0].atype == _DATATYPE_UINT8) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.D[k] = ((double)inShm[0].array.UI8[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT8) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.D[k] = ((double)inShm[0].array.SI8[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT16) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.D[k] = ((double)inShm[0].array.UI16[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT16) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.D[k] = ((double)inShm[0].array.SI16[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT32) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.D[k] = ((double)inShm[0].array.UI32[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT32) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.D[k] = ((double)inShm[0].array.SI32[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT64) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.D[k] = ((double)inShm[0].array.UI64[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT64) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.D[k] = ((double)inShm[0].array.SI64[k] * ffShm[0].array.D[k]) - bgShm[0].array.D[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_FLOAT) {
        for (k = 0; k < calSize; k++) {
            calShm[0].array.D[k] = ((double)inShm[0].array.D[k] * ffShm[0].array.F[k]) - bgShm[0].array.D[k];
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_DOUBLE) {
        for (k = 0; k < calSize; k++) {
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
int_fast8_t daoToolsShmCalibratePws(IMAGE* inShm, IMAGE* ffShm, IMAGE* bgShm, IMAGE* maskShm, IMAGE* calShm, IMAGE* fluxShm) {
    daoTrace("\n");
    int k;
    int inSize = inShm[0].md[0].size[0] * inShm[0].md[0].size[1];
    calShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
    if (inShm[0].md[0].atype == _DATATYPE_UINT8) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.SI32[k] != -1) {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.UI8[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT8) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.SI32[k] != -1) {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.SI8[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT16) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.SI32[k] != -1) {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.UI16[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT16) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.SI32[k] != -1) {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.SI16[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT32) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.SI32[k] != -1) {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.SI32[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT32) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.SI32[k] != -1) {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.UI32[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT64) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.SI32[k] != -1) {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.UI64[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT64) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.SI32[k] != -1) {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.SI64[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_FLOAT) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.SI32[k] != -1) {
                calShm[0].array.F[maskShm[0].array.SI32[k]] = ((float)inShm[0].array.F[k] * ffShm[0].array.F[k]) - bgShm[0].array.F[k];
                fluxShm[0].array.F[0] += calShm[0].array.F[maskShm[0].array.SI32[k]];
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_DOUBLE) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.SI32[k] != -1) {
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
int_fast8_t daoToolCog(float* img, int height, int width, float* centX, float* centY) {
    daoTrace("\n");
    float sumX = 0;
    float sumY = 0;
    float sumPix = 0;
    float A;
    int x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            A = img[x * width + y];
            sumPix += A;
            sumX += (x * A);
            sumY += (y * A);
        }
    }
    if (sumPix != 0) {
        *centX = sumX / sumPix;
        *centY = sumY / sumPix;
        daoDebug("Center X: %d  Centroid X: %f\n", width / 2, sumX / sumPix);
        daoDebug("Center Y: %d  Centroid Y: %f\n", height / 2, sumY / sumPix);
    }
    else {
        daoError("Not enought flux to compute centroid");
        return DAO_ERROR;
    }

    return DAO_SUCCESS;
}

/**
 * @brief Apply the servo filter history to a command vector.
 *
 * The function removes the command offset, protects against NaN inputs, updates
 * the delayed command and residual history, and computes the next precomputed
 * filter term used by subsequent calls.
 *
 * @param command Input command vector.
 * @param nbVal Number of command elements.
 * @param filterHistory Persistent filter state and history buffers.
 * @param servoFilter Servo filter coefficients.
 * @param commandOffset Per-element offset removed before filtering.
 * @param filteredCommand Output filtered command vector.
 *
 * @return DAO_SUCCESS on success.
 */
int_fast8_t daoToolsCommandFilter(float* command, int nbVal, daoFilterHistory* filterHistory, float* servoFilter, float* commandOffset, float* filteredCommand) {
    daoTrace("\n");
    // 
    float commandMoff[nbVal];
    int c = 0;
    int pp;
    for (pp = 0; pp < nbVal; pp++) {
        // Check that values to filter are
        // number... safety check to stop propagating nan
        if (isnan(command[pp])) {
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
    if (filterHistory->step == FILTER_ORDER) {
        filterHistory->step = 0;
    }
    // precomputation of servo loop filter for next filterHistory->step
    int i1, i2, i;
    for (i = 0;i < nbVal;i++) {
        i2 = 2 * FILTER_ORDER;
        for (i1 = filterHistory->step; i1 < FILTER_ORDER; i1++, i2--) {
            filterHistory->precal[i] -= servoFilter[i2] * filterHistory->dlCmd[i1][i];
        }
        for (i1 = 0; i1 < filterHistory->step; i1++, i2--) {
            filterHistory->precal[i] -= servoFilter[i2] * filterHistory->dlCmd[i1][i];
        }
        for (i1 = filterHistory->step; i1 < FILTER_ORDER; i1++, i2--) {
            filterHistory->precal[i] += servoFilter[i2] * filterHistory->dlRes[i1][i];
        }
        for (i1 = 0; i1 < filterHistory->step; i1++, i2--) {
            filterHistory->precal[i] += servoFilter[i2] * filterHistory->dlRes[i1][i];
        }
    }

    return DAO_SUCCESS;
}

/**
 * @brief Apply a scalar leaky integrator to a single-precision command vector.
 *
 * @param command Input command vector.
 * @param nbVal Number of command elements.
 * @param leaky Leak factor applied to the previous output.
 * @param gain Integrator gain applied to the offset-corrected input.
 * @param commandOffset Per-element offset removed before integration.
 * @param filteredCommand In-place filtered output vector.
 *
 * @return DAO_SUCCESS on success.
 */
int_fast8_t daoToolsLeakyIntegrator(float* command, int nbVal, float leaky, float gain, float* commandOffset, float* filteredCommand) {
    daoTrace("\n");
    // 
    float commandMoff[nbVal];
    int pp;
    for (pp = 0; pp < nbVal; pp++) {
        // Check that values to filter are
        // number... safety check to stop propagating nan
        if (isnan(command[pp])) {
            command[pp] = 0.0;
        }
        // Substract command offset
        commandMoff[pp] = command[pp] - commandOffset[pp];
        filteredCommand[pp] = leaky * filteredCommand[pp] - gain * commandMoff[pp]; // * mixingFactor;
    }

    return DAO_SUCCESS;
}

/**
 * @brief Apply a scalar leaky integrator to a double-precision command vector.
 *
 * @param command Input command vector.
 * @param nbVal Number of command elements.
 * @param leaky Leak factor applied to the previous output.
 * @param gain Integrator gain applied to the offset-corrected input.
 * @param commandOffset Per-element offset removed before integration.
 * @param filteredCommand In-place filtered output vector.
 *
 * @return DAO_SUCCESS on success.
 */
 int_fast8_t daoToolsLeakyIntegratorDouble(double *command, int nbVal, double leaky, double gain, double *commandOffset, double *filteredCommand)
 {
     daoTrace("\n");
     // 
     double commandMoff[nbVal];
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
/**
 * @brief Apply a per-mode leaky integrator to a single-precision vector.
 *
 * @param command Input modal command vector.
 * @param nbVal Number of modes.
 * @param leaky Per-mode leak factors.
 * @param gain Per-mode integrator gains.
 * @param commandOffset Per-mode offset removed before integration.
 * @param filteredCommand In-place filtered output vector.
 *
 * @return DAO_SUCCESS on success.
 */
int_fast8_t daoToolsLeakyModalIntegrator(float* command, int nbVal, float* leaky, float* gain, float* commandOffset, float* filteredCommand) {
    daoTrace("\n");
    // 
    float commandMoff[nbVal];
    int pp;
    for (pp = 0; pp < nbVal; pp++) {
        // Check that values to filter are
        // number... safety check to stop propagating nan
        if (isnan(command[pp])) {
            command[pp] = 0.0;
        }
        // Substract command offset
        commandMoff[pp] = command[pp] - commandOffset[pp];
        filteredCommand[pp] = leaky[pp] * filteredCommand[pp] - gain[pp] * commandMoff[pp]; // * mixingFactor;
    }

    return DAO_SUCCESS;
}

/**
 * @brief Apply a per-mode leaky integrator to a double-precision vector.
 *
 * @param command Input modal command vector.
 * @param nbVal Number of modes.
 * @param leaky Per-mode leak factors.
 * @param gain Per-mode integrator gains.
 * @param commandOffset Per-mode offset removed before integration.
 * @param filteredCommand In-place filtered output vector.
 *
 * @return DAO_SUCCESS on success.
 */
int_fast8_t daoToolsLeakyModalIntegratorDouble(double* command, int nbVal, double* leaky, double* gain, double* commandOffset, double* filteredCommand) {
    daoTrace("\n");
    // 
    double commandMoff[nbVal];
    int pp;
    for (pp = 0; pp < nbVal; pp++) {
        // Check that values to filter are
        // number... safety check to stop propagating nan
        if (isnan(command[pp])) {
            command[pp] = 0.0;
        }
        // Substract command offset
        commandMoff[pp] = command[pp] - commandOffset[pp];
        filteredCommand[pp] = leaky[pp] * filteredCommand[pp] - gain[pp] * commandMoff[pp]; // * mixingFactor;
    }

    return DAO_SUCCESS;
}

/**
 * @brief Compute spot centroids using center-of-mass with thresholding.
 *
 * This function computes relative centroids for multiple subapertures
 * in a square image. For each subaperture, a square box centered on the
 * reference position is extracted and a center-of-mass is computed
 * after discarding pixels below a fixed threshold.
 *
 * Reference positions and output centroids use a Structure-of-Arrays
 * (SoA) layout for improved cache locality and easier interoperability
 * with vectorized and GPU-based pipelines.
 *
 * ### Reference layout (SoA)
 * The reference array @p ref must contain `2 * nSuba` elements arranged as:
 *
 * - `ref[0 .. nSuba-1]`         : Reference X positions
 * - `ref[nSuba .. 2*nSuba-1]`   : Reference Y positions
 *
 * ### Output layout (SoA)
 * The output array @p cent must contain at least `3 * nSuba` elements:
 *
 * - `cent[0 .. nSuba-1]`           : X centroids (cx), relative to ref X
 * - `cent[nSuba .. 2*nSuba-1]`     : Y centroids (cy), relative to ref Y
 * - `cent[2*nSuba .. 3*nSuba-1]`   : Total flux (denominator after thresholding)
 *
 * ### Notes
 * - Pixel coordinates are treated as integer indices.
 * - Subaperture bounds are closed intervals `[x1..x2]` and `[y1..y2]`.
 * - If the flux in a subaperture is zero, the centroid is set to `(0, 0)`.
 * - No bounds checking is performed on image edges.
 *
 * @param[in]  image      Pointer to the input image (row-major, float)
 * @param[in]  imageSize  Width and height of the square image (pixels)
 * @param[in]  ref        Reference positions in SoA layout (size `2*nSuba`)
 * @param[in]  boxSize    Size of the square subaperture (pixels)
 * @param[in]  nSuba      Number of subapertures
 * @param[in]  threshold  Absolute pixel intensity threshold
 * @param[out] cent       Output centroid array (size >= `3*nSuba`)
 *
 * @return DAO_SUCCESS on success
 */
int_fast8_t daoCentroidSpots(float* image,
    int imageSize,
    float* ref,
    int boxSize,
    int nSuba,
    float threshold,
    float* cent) {
    daoTrace("\n");

    /* Loop indices and subaperture bounds */
    int x, y;
    int x1, x2, y1, y2;

    /* Accumulators for center-of-mass computation */
    float xNumerator;
    float yNumerator;
    float denominator;
    float pixel;

    /* Reference arrays (SoA layout) */
    float* refX = ref;
    float* refY = ref + nSuba;

    /* Output centroid arrays (SoA layout) */
    float* cxOut = cent;
    float* cyOut = cent + nSuba;
    float* denOut = cent + 2 * nSuba;

    /* Iterate over all subapertures */
    for (int s = 0; s < nSuba; ++s) {
        const float rx = refX[s];
        const float ry = refY[s];

        /* Reset accumulators */
        xNumerator = 0.0f;
        yNumerator = 0.0f;
        denominator = 0.0f;

        /* Compute subaperture bounds around reference position */
        x1 = (unsigned int)roundf(rx) - boxSize / 2;
        x2 = (unsigned int)roundf(rx) + boxSize / 2;
        y1 = (unsigned int)roundf(ry) - boxSize / 2;
        y2 = (unsigned int)roundf(ry) + boxSize / 2;

        /* Accumulate pixel intensities and first moments */
        for (x = x1; x <= x2; x++) {
            for (y = y1; y <= y2; y++) {
                pixel = (float)image[y * imageSize + x];

                /* Apply absolute threshold */
                if (pixel < threshold) {
                    pixel = 0.0f;
                }

                denominator += pixel;
                xNumerator += pixel * (float)x;
                yNumerator += pixel * (float)y;
            }
        }

        /* Compute relative centroid if flux is non-zero */
        if (denominator != 0.0f) {
            cxOut[s] = xNumerator / denominator - rx;
            cyOut[s] = yNumerator / denominator - ry;
        }
        else {
            cxOut[s] = 0.0f;
            cyOut[s] = 0.0f;
        }

        /* Store flux */
        denOut[s] = denominator;
    }

    return DAO_SUCCESS;
}

/**
 * @brief Compute centroids relative to reference positions using a *relative* threshold.
 *
 * For each subaperture, this function:
 * 1) extracts a square box centered on the reference position,
 * 2) finds the local maximum within that box,
 * 3) builds a relative threshold = (threshold * localMax),
 * 4) subtracts that threshold from pixels above it (zeros pixels below),
 * 5) computes a center-of-mass from the thresholded pixels,
 * 6) returns centroid values *relative* to the reference positions.
 *
 * Both reference positions and outputs use Structure-of-Arrays (SoA) layout.
 *
 * ### Reference layout (SoA)
 * The reference array @p ref must contain `2 * nSuba` elements arranged as:
 * - `ref[0 .. nSuba-1]`         : Reference X positions
 * - `ref[nSuba .. 2*nSuba-1]`   : Reference Y positions
 *
 * ### Output layout (SoA)
 * The output array @p cent must contain at least `4 * nSuba` elements arranged as:
 * - `cent[0 .. nSuba-1]`           : X centroids (cx), relative to ref X
 * - `cent[nSuba .. 2*nSuba-1]`     : Y centroids (cy), relative to ref Y
 * - `cent[2*nSuba .. 3*nSuba-1]`   : Flux = sum of raw pixels (no threshold)
 * - `cent[3*nSuba .. 4*nSuba-1]`   : Weight = sum of thresholded pixels (after subtraction)
 *
 * ### Notes
 * - Subaperture bounds are closed intervals `[x1..x2]` and `[y1..y2]`.
 * - If the thresholded weight is zero, the centroid is set to `(0, 0)`.
 * - No bounds checking is performed on image edges.
 *
 * @param[in]  image      Pointer to the input image (row-major, float)
 * @param[in]  imageSize  Width and height of the square image (pixels)
 * @param[in]  ref        Reference positions in SoA layout (size `2*nSuba`)
 * @param[in]  boxSize    Size of the square subaperture (pixels)
 * @param[in]  nSuba      Number of subapertures
 * @param[in]  threshold  Relative threshold factor in [0..1] typically (multiplied by local max)
 * @param[out] cent       Output array (size >= `4*nSuba`, layout described above)
 *
 * @return DAO_SUCCESS on success
 */
int_fast8_t daoCentroidSpotsRelative(float* image,
    int imageSizeX,
    int imageSizeY,
    float* ref,
    int boxSize,
    int nSuba,
    float threshold,
    float* cent) {
    daoTrace("\n");
    int x, y, x1, x2, y1, y2;
    float localMax = 0.0f;
    float relativeThreshold = 0.0f;

    float xNumerator, yNumerator, denominator, pixel, flux, weight;

    /* Reference arrays (SoA layout) */
    float* refX = ref;
    float* refY = ref + nSuba;

    /* Output arrays (SoA layout) */
    float* cxOut = cent;
    float* cyOut = cent + nSuba;
    float* fluxOut = cent + 2 * nSuba;
    float* wOut = cent + 3 * nSuba;

    for (int s = 0; s < nSuba; ++s) {
        const float rx = refX[s];
        const float ry = refY[s];

        /* Reset accumulators */
        xNumerator = 0.0f;
        yNumerator = 0.0f;
        denominator = 0.0f;
        flux = 0.0f;
        weight = 0.0f;
        localMax = 0.0f;

        /* Compute subaperture bounds around reference position */
        x1 = (unsigned int)roundf(rx) - boxSize / 2;
        x2 = (unsigned int)roundf(rx) + boxSize / 2;
        y1 = (unsigned int)roundf(ry) - boxSize / 2;
        y2 = (unsigned int)roundf(ry) + boxSize / 2;

        /* Compute local max in the subaperture (raw pixels) */
        for (x = x1; x <= x2; x++) {
            for (y = y1; y <= y2; y++) {
                pixel = (float)image[y * imageSizeX + x];
                if (pixel > localMax) {
                    localMax = pixel;
                }
            }
        }

        /* Relative threshold derived from the local maximum */
        relativeThreshold = threshold * localMax;

        /* Accumulate moments from thresholded/subtracted pixels */
        for (x = x1; x <= x2; x++) {
            for (y = y1; y <= y2; y++) {
                pixel = (float)image[y * imageSizeX + x];

                /* Raw flux always accumulates original pixel */
                flux += pixel;

                /* Apply relative threshold with subtraction */
                if (pixel < relativeThreshold) {
                    pixel = 0.0f;
                }
                else {
                    pixel = pixel - relativeThreshold;
                }

                /* Weight/denominator accumulates thresholded pixels */
                weight += pixel;
                denominator += pixel;

                /* First moments */
                xNumerator += pixel * (float)x;
                yNumerator += pixel * (float)y;
            }
        }

        /* Compute relative centroid if weight is non-zero */
        if (denominator != 0.0f) {
            cxOut[s] = xNumerator / denominator - rx;
            cyOut[s] = yNumerator / denominator - ry;
        }
        else {
            cxOut[s] = 0.0f;
            cyOut[s] = 0.0f;
        }

        /* Store diagnostics */
        fluxOut[s] = flux;
        wOut[s] = weight;
    }

    return DAO_SUCCESS;
}

/**
 * @brief Compute centroids relative to reference positions using a *relative* threshold.
 *
 * For each subaperture, this function:
 * 1) extracts a square box centered on the reference position,
 * 2) finds the local maximum within that box,
 * 3) builds a relative threshold = (threshold * localMax),
 * 4) subtracts that threshold from pixels above it (zeros pixels below),
 * 5) computes a center-of-mass from the thresholded pixels,
 * 6) returns centroid values *relative* to the reference positions.
 *
 * Both reference positions and outputs use Structure-of-Arrays (SoA) layout.
 *
 * ### Reference layout (SoA)
 * The reference array @p ref must contain `2 * nSuba` elements arranged as:
 * - `ref[0 .. nSuba-1]`         : Reference X positions
 * - `ref[nSuba .. 2*nSuba-1]`   : Reference Y positions
 *
 * ### Output layout (SoA)
 * The output array @p cent must contain at least `4 * nSuba` elements arranged as:
 * - `cent[0 .. nSuba-1]`           : X centroids (cx), relative to ref X
 * - `cent[nSuba .. 2*nSuba-1]`     : Y centroids (cy), relative to ref Y
 * - `cent[2*nSuba .. 3*nSuba-1]`   : Flux = sum of raw pixels (no threshold)
 * - `cent[3*nSuba .. 4*nSuba-1]`   : Weight = sum of thresholded pixels (after subtraction)
 *
 * ### Notes
 * - Subaperture bounds are closed intervals `[x1..x2]` and `[y1..y2]`.
 * - If the thresholded weight is zero, the centroid is set to `(0, 0)`.
 * - No bounds checking is performed on image edges.
 *
 * @param[in]  image      Pointer to the input image (row-major, float)
 * @param[in]  imageSize  Width and height of the square image (pixels)
 * @param[in]  ref        Reference positions in SoA layout (size `2*nSuba`)
 * @param[in]  boxSize    Size of the square subaperture (pixels)
 * @param[in]  nSuba      Number of subapertures
 * @param[in]  threshold  Relative threshold factor in [0..1] typically (multiplied by local max)
 * @param[out] cent       Output array (size >= `4*nSuba`, layout described above)
 *
 * @return DAO_SUCCESS on success
 */
int_fast8_t daoCentroidSpotsRelativeRef(float* image,
    int imageSizeX,
    int imageSizeY,
    float* subApCentre,
    float* ref,
    int boxSize,
    int nSuba,
    float threshold,
    float* cent) {
    daoTrace("\n");
    int x, y, x1, x2, y1, y2;
    float localMax = 0.0f;
    float relativeThreshold = 0.0f;

    float xNumerator, yNumerator, denominator, pixel, flux, weight;

    /* Reference arrays (SoA layout) */
    float* centreX = subApCentre;
    float* centreY = subApCentre + nSuba;

    float* refX = ref;
    float* refY = ref + nSuba;

    /* Output arrays (SoA layout) */
    float* cxOut = cent;
    float* cyOut = cent + nSuba;
    float* fluxOut = cent + 2 * nSuba;
    float* wOut = cent + 3 * nSuba;

    for (int s = 0; s < nSuba; ++s) {
        const float cx = centreX[s];
        const float cy = centreY[s];

        /* Reset accumulators */
        xNumerator = 0.0f;
        yNumerator = 0.0f;
        denominator = 0.0f;
        flux = 0.0f;
        weight = 0.0f;
        localMax = 0.0f;

        /* Compute subaperture bounds around reference position */
        x1 = (unsigned int)roundf(cx) - boxSize / 2;
        x2 = (unsigned int)roundf(cx) + boxSize / 2;
        y1 = (unsigned int)roundf(cy) - boxSize / 2;
        y2 = (unsigned int)roundf(cy) + boxSize / 2;

        /* Compute local max in the subaperture (raw pixels) */
        for (x = x1; x <= x2; x++) {
            for (y = y1; y <= y2; y++) {
                pixel = (float)image[y * imageSizeX + x];
                if (pixel > localMax) {
                    localMax = pixel;
                }
            }
        }

        /* Relative threshold derived from the local maximum */
        relativeThreshold = threshold * localMax;

        /* Accumulate moments from thresholded/subtracted pixels */
        for (x = x1; x <= x2; x++) {
            for (y = y1; y <= y2; y++) {
                pixel = (float)image[y * imageSizeX + x];

                /* Raw flux always accumulates original pixel */
                flux += pixel;

                /* Apply relative threshold with subtraction */
                if (pixel < relativeThreshold) {
                    pixel = 0.0f;
                }
                else {
                    pixel = pixel - relativeThreshold;
                }

                /* Weight/denominator accumulates thresholded pixels */
                weight += pixel;
                denominator += pixel;

                /* First moments */
                xNumerator += pixel * (float)x;
                yNumerator += pixel * (float)y;
            }
        }

        /* Compute relative centroid if weight is non-zero */
        if (denominator != 0.0f) {
            cxOut[s] = xNumerator / denominator - cx - refX[s];
            cyOut[s] = yNumerator / denominator - cy - refY[s];
        }
        else {
            cxOut[s] = 0.0f;
            cyOut[s] = 0.0f;
        }

        /* Store diagnostics */
        fluxOut[s] = flux;
        wOut[s] = weight;
    }

    return DAO_SUCCESS;
}

/**
 * @brief Compute pyramid WFS slopes on the CPU from quadrant samples.
 *
 * @param im Input image buffer.
 * @param slopes Output slopes vector.
 * @param slopesRef Reference slopes subtracted from the measurement.
 * @param wfsPixId Valid slope indices for each pupil sample.
 * @param wfsPixIdMap Image offsets used to fetch the quadrant pixels.
 * @param flux Total flux used for normalization.
 * @param nbPix Number of valid slope measurements.
 * @param imSize Linear image size.
 * @param pupSize Number of pupil samples to inspect.
 *
 * @return DAO_SUCCESS on success.
 */
int_fast8_t daoCentroidPws(float* im, float* slopes,
    float* slopesRef, int* wfsPixId, int* wfsPixIdMap,
    float* flux, int nbPix,
    int imSize, int pupSize) {
    daoTrace("\n");
    float q1, q2, q3, q4;
    // Get our global thread ID
    int id = 0;
    float avg = *flux / (4 * nbPix);
    daoDebug("avg = %.3f\n", avg);
    // do the comutation only if there is flux
    for (id = 0; id < pupSize; id++) {
        if (*flux > 0) {
            if (wfsPixId[id] != -1) {
                //daoInfo("id=%d, pixid[id]=%d, pixIdMap[id]=%d\n", id, wfsPixId[id], wfsPixIdMap[id]);
                q1 = im[wfsPixIdMap[id]];
                q2 = im[wfsPixIdMap[id] + imSize];
                q3 = im[wfsPixIdMap[id] + imSize * 2 * imSize];
                q4 = im[wfsPixIdMap[id] + imSize * 2 * imSize + imSize];
                daoInfo("%f,%f,%f,%f\n", q1, q2, q3, q4);
                slopes[wfsPixId[id]] = (q1 + q3 - q2 - q4) / avg - slopesRef[wfsPixId[id]];
                slopes[wfsPixId[id] + nbPix] = (q1 + q2 - q3 - q4) / avg - slopesRef[wfsPixId[id] + nbPix];
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
void daoDescrambleOcam2Image(uint8_t img[], int imgRows, int imgCols, uint16_t* img16[],
    int descrambler[], int descramblerSize, uint16_t output[]) {
    int img16Cols = imgCols / 2;

    // Convert flattened img to 16-bit img16
    for (int i = 0; i < imgRows; i++) {
        for (int j = 0; j < imgCols; j += 2) \
        {
            // Combine two adjacent 8-bit values into one 16-bit value
            img16[i][j / 2] = (uint16_t)(img[i * imgCols + j + 1] << 8) + img[i * imgCols + j];
        }
    }

    // Use descrambler array to reorder the pixels in the output
    for (int i = 0; i < descramblerSize; i++) {
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
int_fast8_t daoToolsShmExtract(IMAGE* inShm, IMAGE* maskShm, IMAGE* outShm) {
    daoTrace("\n");
    int k;
    int inSize = inShm[0].md[0].size[0] * inShm[0].md[0].size[1];
    outShm[0].md[0].cnt2 = inShm[0].md[0].cnt2;
    int cnt = 0;
    if (inShm[0].md[0].atype == _DATATYPE_UINT8) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                outShm[0].array.UI8[cnt] = inShm[0].array.UI8[k];
                cnt++;
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT8) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                outShm[0].array.SI8[cnt] = inShm[0].array.SI8[k];
                cnt++;
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT16) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                outShm[0].array.UI16[cnt] = inShm[0].array.UI16[k];
                cnt++;
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT16) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                outShm[0].array.SI16[cnt] = inShm[0].array.SI16[k];
                cnt++;
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT32) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                outShm[0].array.SI32[cnt] = inShm[0].array.SI32[k];
                cnt++;
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT32) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                outShm[0].array.UI32[cnt] = inShm[0].array.UI32[k];
                cnt++;
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_UINT64) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                outShm[0].array.UI64[cnt] = inShm[0].array.UI64[k];
                cnt++;
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_INT64) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                outShm[0].array.SI64[cnt] = inShm[0].array.SI64[k];
                cnt++;
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_FLOAT) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                outShm[0].array.F[cnt] = inShm[0].array.F[k];
                cnt++;
            }
        }
    }
    else if (inShm[0].md[0].atype == _DATATYPE_DOUBLE) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                outShm[0].array.D[cnt] = inShm[0].array.D[k];
                cnt++;
            }
        }
    }
    daoShmImagePart2ShmFinalize(&outShm[0]);

    return DAO_SUCCESS;
}

/**
 * @brief Subtract and extract an image by applying mask, assume same type for A and B
 *
 * If normalize != 0, output image is normalized in place after extraction.
 *
 * @param inAShm image
 * @param inBShm image to subtract
 * @param maskShm extraction mask, assumes uint32
 * @param outShm output extracted image as vector
 * @param normalize normalize output image if non-zero
 * @return DAO_SUCCESS on success, DAO_ERROR on error
 */
int_fast8_t daoToolsShmSubstractExtractFinalize(IMAGE *inAShm,
                                        IMAGE *inBShm,
                                        IMAGE *maskShm,
                                        IMAGE *outShm,
                                        IMAGE *normShm,
                                        int normalize)
{
    daoTrace("\n");

    if (daoToolsShmSubstractExtract(inAShm, inBShm, maskShm, outShm, normShm, normalize) == DAO_ERROR)
    {
        return DAO_ERROR;
    }
    daoShmImagePart2ShmFinalize(&outShm[0]);

    return DAO_SUCCESS;
}
/**
 * @brief Subtract and extract an image by applying mask, assume same type for A and B
 *
 * If normalize != 0, output image is normalized in place after extraction.
 *
 * @param inAShm image
 * @param inBShm image to subtract
 * @param maskShm extraction mask, assumes uint32
 * @param outShm output extracted image as vector
 * @param normalize normalize output image if non-zero
 * @return DAO_SUCCESS on success, DAO_ERROR on error
 */
int_fast8_t daoToolsShmSubstractExtract(IMAGE *inAShm,
                                        IMAGE *inBShm,
                                        IMAGE *maskShm,
                                        IMAGE *outShm,
                                        IMAGE *normShm,
                                        int normalize)
{
    daoTrace("\n");

    int k;
    int cnt = 0;
    int inSize;

    if ((inAShm == NULL) || (inBShm == NULL) || (maskShm == NULL) || (outShm == NULL))
    {
        daoError("NULL input pointer\n");
        return DAO_ERROR;
    }

    if ((inAShm[0].md[0].size[0] != inBShm[0].md[0].size[0]) ||
        (inAShm[0].md[0].size[1] != inBShm[0].md[0].size[1]) ||
        (inAShm[0].md[0].size[0] != maskShm[0].md[0].size[0]) ||
        (inAShm[0].md[0].size[1] != maskShm[0].md[0].size[1]))
    {
        daoError("input image sizes do not match\n");
        return DAO_ERROR;
    }

    if (inAShm[0].md[0].atype != inBShm[0].md[0].atype)
    {
        daoError("input A/B datatype mismatch : %d != %d\n",
        inAShm[0].md[0].atype,
        inBShm[0].md[0].atype);
        return DAO_ERROR;
    }

    if (inAShm[0].md[0].atype != outShm[0].md[0].atype)
    {
        daoError("input/output datatype mismatch : %d != %d\n",
        inAShm[0].md[0].atype,
        outShm[0].md[0].atype);
        return DAO_ERROR;
    }

    if (maskShm[0].md[0].atype != _DATATYPE_UINT32)
    {
        daoError("mask datatype must be UINT32, got %d\n", maskShm[0].md[0].atype);
        return DAO_ERROR;
    }

    inSize = inAShm[0].md[0].size[0] * inAShm[0].md[0].size[1];

    outShm[0].md[0].cnt2 = inAShm[0].md[0].cnt2;

    switch (inAShm[0].md[0].atype)
    {
        case _DATATYPE_UINT8:
            for (k = 0; k < inSize; k++)
            {
                if (maskShm[0].array.UI32[k] == 1)
                {
                    outShm[0].array.UI8[cnt] = inAShm[0].array.UI8[k] - inBShm[0].array.UI8[k];
                    cnt++;
                }
            }
            break;

        case _DATATYPE_INT8:
            for (k = 0; k < inSize; k++)
            {
                if (maskShm[0].array.UI32[k] == 1)
                {
                    outShm[0].array.SI8[cnt] = inAShm[0].array.SI8[k] - inBShm[0].array.SI8[k];
                    cnt++;
                }
            }
            break;

        case _DATATYPE_UINT16:
            for (k = 0; k < inSize; k++)
            {
                if (maskShm[0].array.UI32[k] == 1)
                {
                    outShm[0].array.UI16[cnt] = inAShm[0].array.UI16[k] - inBShm[0].array.UI16[k];
                    cnt++;
                }
            }
            break;

        case _DATATYPE_INT16:
            for (k = 0; k < inSize; k++)
            {
                if (maskShm[0].array.UI32[k] == 1)
                {
                    outShm[0].array.SI16[cnt] = inAShm[0].array.SI16[k] - inBShm[0].array.SI16[k];
                    cnt++;
                }
            }
        break;

        case _DATATYPE_UINT32:
            for (k = 0; k < inSize; k++)
            {
                if (maskShm[0].array.UI32[k] == 1)
                {
                    outShm[0].array.UI32[cnt] = inAShm[0].array.UI32[k] - inBShm[0].array.UI32[k];
                    cnt++;
                }
            }
            break;

        case _DATATYPE_INT32:
            for (k = 0; k < inSize; k++)
            {
                if (maskShm[0].array.UI32[k] == 1)
                {
                    outShm[0].array.SI32[cnt] = inAShm[0].array.SI32[k] - inBShm[0].array.SI32[k];
                    cnt++;
                }
            }
            break;

        case _DATATYPE_UINT64:
            for (k = 0; k < inSize; k++)
                {
                    if (maskShm[0].array.UI32[k] == 1)
                    {
                        outShm[0].array.UI64[cnt] = inAShm[0].array.UI64[k] - inBShm[0].array.UI64[k];
                        cnt++;
                    }
            }
            break;

        case _DATATYPE_INT64:
            for (k = 0; k < inSize; k++)
            {
                if (maskShm[0].array.UI32[k] == 1)
                {
                    outShm[0].array.SI64[cnt] = inAShm[0].array.SI64[k] - inBShm[0].array.SI64[k];
                    cnt++;
                }
            }
            break;

        case _DATATYPE_FLOAT:
            for (k = 0; k < inSize; k++)
            {
                if (maskShm[0].array.UI32[k] == 1)
                    {
                        if (normalize !=0)
                        {
                            outShm[0].array.F[cnt] = (inAShm[0].array.F[k] - inBShm[0].array.F[k]) / normShm[0].array.F[0];
                        }
                        else
                        {
                            outShm[0].array.F[cnt] = inAShm[0].array.F[k] - inBShm[0].array.F[k];
                        }
                        cnt++;
                    }
                }
            break;

        case _DATATYPE_DOUBLE:
            for (k = 0; k < inSize; k++)
            {
                if (maskShm[0].array.UI32[k] == 1)
                {
                    if (normalize !=0)
                    {
                        outShm[0].array.D[cnt] = (inAShm[0].array.D[k] - inBShm[0].array.D[k]) / normShm[0].array.D[0];
                    }
                    else
                    {
                        outShm[0].array.D[cnt] = inAShm[0].array.D[k] - inBShm[0].array.D[k];
                    }
                    cnt++;
                }
            }
            break;

        default:
            daoError("unsupported datatype %d\n", inAShm[0].md[0].atype);
            return DAO_ERROR;
    }

    return DAO_SUCCESS;
}

/**
 * @brief Subastract and Extract an image by applying mask with normalization, assume same type for A and B
 *
 * This function subtracts inBShm from inAShm, normalizes the result by the sum of 
 * pixels within the mask, and extracts only the masked pixels to a vector.
 *
 * @param inAShm image
 * @param inBShm image  - to substract
 * @param maskShm flat field assumes uint32
 * @param outShm output extracted normalized image as vector
 * @return int_fast8_t
 */
 int_fast8_t daoToolsShmSubstractExtractNorm(IMAGE* inAShm, IMAGE* inBShm, IMAGE* maskShm, IMAGE* outShm) {
    daoTrace("\n");
    int k;
    int inSize = inAShm[0].md[0].size[0] * inAShm[0].md[0].size[1];
    outShm[0].md[0].cnt2 = inAShm[0].md[0].cnt2;
    int cnt = 0;
    double sum = 0.0;
    
    if (inAShm[0].md[0].atype == _DATATYPE_UINT8) {
        // First pass: compute sum of subtracted pixels within mask
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                double pixelValue = (double)(inAShm[0].array.UI8[k] - inBShm[0].array.UI8[k]);
                sum += (pixelValue < 1.0) ? 1.0 : pixelValue;
            }
        }
        // Second pass: extract and normalize
        if (sum != 0.0) {
            for (k = 0; k < inSize; k++) {
                if (maskShm[0].array.UI32[k] == 1) {
                    outShm[0].array.UI8[cnt] = (uint8_t)((double)(inAShm[0].array.UI8[k] - inBShm[0].array.UI8[k]) / sum);
                    cnt++;
                }
            }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_INT8) {
        // First pass: compute sum
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                double pixelValue = (double)(inAShm[0].array.SI8[k] - inBShm[0].array.SI8[k]);
                sum += (pixelValue < 1.0) ? 1.0 : pixelValue;
            }
        }
        // Second pass: extract and normalize
        if (sum != 0.0) {
            for (k = 0; k < inSize; k++) {
                if (maskShm[0].array.UI32[k] == 1) {
                    outShm[0].array.SI8[cnt] = (int8_t)((double)(inAShm[0].array.SI8[k] - inBShm[0].array.SI8[k]) / sum);
                    cnt++;
                }
            }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_UINT16) {
        // First pass: compute sum
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                double pixelValue = (double)(inAShm[0].array.UI16[k] - inBShm[0].array.UI16[k]);
                sum += (pixelValue < 1.0) ? 1.0 : pixelValue;
            }
        }
        // Second pass: extract and normalize
        if (sum != 0.0) {
            for (k = 0; k < inSize; k++) {
                if (maskShm[0].array.UI32[k] == 1) {
                    outShm[0].array.UI16[cnt] = (uint16_t)((double)(inAShm[0].array.UI16[k] - inBShm[0].array.UI16[k]) / sum);
                    cnt++;
                }
            }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_INT16) {
        // First pass: compute sum
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                double pixelValue = (double)(inAShm[0].array.SI16[k] - inBShm[0].array.SI16[k]);
                sum += (pixelValue < 1.0) ? 1.0 : pixelValue;
            }
        }
        // Second pass: extract and normalize
        if (sum != 0.0) {
            for (k = 0; k < inSize; k++) {
                if (maskShm[0].array.UI32[k] == 1) {
                    outShm[0].array.SI16[cnt] = (int16_t)((double)(inAShm[0].array.SI16[k] - inBShm[0].array.SI16[k]) / sum);
                    cnt++;
                }
            }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_INT32) {
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                double pixelValue = (double)(inAShm[0].array.SI32[k] - inBShm[0].array.SI32[k]);
                sum += (pixelValue < 1.0) ? 1.0 : pixelValue;
            }
        }
        // Second pass: extract and normalize
        if (sum != 0.0) {
            for (k = 0; k < inSize; k++) {
                if (maskShm[0].array.UI32[k] == 1) {
                    outShm[0].array.SI32[cnt] = (int32_t)((double)(inAShm[0].array.SI32[k] - inBShm[0].array.SI32[k]) / sum);
                    cnt++;
                }
            }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_UINT32) {
        // First pass: compute sum
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                double pixelValue = (double)(inAShm[0].array.UI32[k] - inBShm[0].array.UI32[k]);
                sum += (pixelValue < 1.0) ? 1.0 : pixelValue;
            }
        }
        // Second pass: extract and normalize
        if (sum != 0.0) {
            for (k = 0; k < inSize; k++) {
                if (maskShm[0].array.UI32[k] == 1) {
                    outShm[0].array.UI32[cnt] = (uint32_t)((double)(inAShm[0].array.UI32[k] - inBShm[0].array.UI32[k]) / sum);
                    cnt++;
                }
            }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_UINT64) {
        // First pass: compute sum
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                double pixelValue = (double)(inAShm[0].array.UI64[k] - inBShm[0].array.UI64[k]);
                sum += (pixelValue < 1.0) ? 1.0 : pixelValue;
            }
        }
        // Second pass: extract and normalize
        if (sum != 0.0) {
            for (k = 0; k < inSize; k++) {
                if (maskShm[0].array.UI32[k] == 1) {
                    outShm[0].array.UI64[cnt] = (uint64_t)((double)(inAShm[0].array.UI64[k] - inBShm[0].array.UI64[k]) / sum);
                    cnt++;
                }
            }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_INT64) {
        // First pass: compute sum
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                double pixelValue = (double)(inAShm[0].array.SI64[k] - inBShm[0].array.SI64[k]);
                sum += (pixelValue < 1.0) ? 1.0 : pixelValue;
            }
        }
        // Second pass: extract and normalize
        if (sum != 0.0) {
            for (k = 0; k < inSize; k++) {
                if (maskShm[0].array.UI32[k] == 1) {
                    outShm[0].array.SI64[cnt] = (int64_t)((double)(inAShm[0].array.SI64[k] - inBShm[0].array.SI64[k]) / sum);
                    cnt++;
                }
            }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_FLOAT) {
        // First pass: compute sum
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                double pixelValue = (double)(inAShm[0].array.F[k] - inBShm[0].array.F[k]);
                sum += (pixelValue < 1.0) ? 1.0 : pixelValue;
            }
        }
        // Second pass: extract and normalize
        if (sum != 0.0) {
            for (k = 0; k < inSize; k++) {
                if (maskShm[0].array.UI32[k] == 1) {
                    outShm[0].array.F[cnt] = (float)((double)(inAShm[0].array.F[k] - inBShm[0].array.F[k]) / sum);
                    cnt++;
                }
            }
        }
    }
    else if (inAShm[0].md[0].atype == _DATATYPE_DOUBLE) {
        // First pass: compute sum
        for (k = 0; k < inSize; k++) {
            if (maskShm[0].array.UI32[k] == 1) {
                double pixelValue = inAShm[0].array.D[k] - inBShm[0].array.D[k];
                sum += (pixelValue < 1.0) ? 1.0 : pixelValue;
            }
        }
        // Second pass: extract and normalize
        if (sum != 0.0) {
            for (k = 0; k < inSize; k++) {
                if (maskShm[0].array.UI32[k] == 1) {
                    outShm[0].array.D[cnt] = (inAShm[0].array.D[k] - inBShm[0].array.D[k]) / sum;
                    cnt++;
                }
            }
        }
    }
    daoShmImagePart2ShmFinalize(&outShm[0]);

    return DAO_SUCCESS;
}

/**
 * @brief Normalize image in place
 *
 * For floating point images:
 *     output = (x - min) / (max - min)          -> [0,1]
 *
 * For integer images:
 *     output = full-scale stretch to datatype range
 *
 * If max == min, image is filled with 0.
 *
 * @param img input/output image
 * @return DAO_SUCCESS on success, DAO_ERROR on error
 */
int_fast8_t daoToolsImgNormalize(IMAGE *img)
{
    daoTrace("\n");

    if (img == NULL)
    {
        daoError("img is NULL\n");
        return DAO_ERROR;
    }

    if (img[0].md == NULL)
    {
        daoError("img metadata is NULL\n");
        return DAO_ERROR;
    }

    uint64_t nelem = img[0].md[0].nelement;
    if (nelem == 0)
    {
        daoError("img has zero element\n");
        return DAO_ERROR;
    }

    switch (img[0].md[0].atype)
    {
        case _DATATYPE_UINT8:
        {
            uint8_t *data = img[0].array.UI8;
            uint8_t minv = data[0];
            uint8_t maxv = data[0];
            uint64_t ii;

            for (ii = 1; ii < nelem; ii++)
            {
                if (data[ii] < minv) minv = data[ii];
                if (data[ii] > maxv) maxv = data[ii];
            }

            if (maxv == minv)
            {
                memset(data, 0, nelem * sizeof(uint8_t));
                return DAO_SUCCESS;
            }

            double scale = 255.0 / ((double) maxv - (double) minv);
            for (ii = 0; ii < nelem; ii++)
            {
                data[ii] = (uint8_t) (((double) data[ii] - (double) minv) * scale + 0.5);
            }
        }
        break;

        case _DATATYPE_INT8:
        {
            int8_t *data = img[0].array.SI8;
            int8_t minv = data[0];
            int8_t maxv = data[0];
            uint64_t ii;

            for (ii = 1; ii < nelem; ii++)
            {
                if (data[ii] < minv) minv = data[ii];
                if (data[ii] > maxv) maxv = data[ii];
            }

            if (maxv == minv)
            {
                memset(data, 0, nelem * sizeof(int8_t));
                return DAO_SUCCESS;
            }

            double scale = 255.0 / ((double) maxv - (double) minv);
            for (ii = 0; ii < nelem; ii++)
            {
                double v = (((double) data[ii] - (double) minv) * scale) - 128.0;
                if (v < -128.0) v = -128.0;
                if (v >  127.0) v =  127.0;
                data[ii] = (int8_t) (v + (v >= 0.0 ? 0.5 : -0.5));
            }
        }
        break;

        case _DATATYPE_UINT16:
        {
            uint16_t *data = img[0].array.UI16;
            uint16_t minv = data[0];
            uint16_t maxv = data[0];
            uint64_t ii;

            for (ii = 1; ii < nelem; ii++)
            {
                if (data[ii] < minv) minv = data[ii];
                if (data[ii] > maxv) maxv = data[ii];
            }

            if (maxv == minv)
            {
                memset(data, 0, nelem * sizeof(uint16_t));
                return DAO_SUCCESS;
            }

            double scale = 65535.0 / ((double) maxv - (double) minv);
            for (ii = 0; ii < nelem; ii++)
            {
                data[ii] = (uint16_t) (((double) data[ii] - (double) minv) * scale + 0.5);
            }
        }
        break;

        case _DATATYPE_INT16:
        {
            int16_t *data = img[0].array.SI16;
            int16_t minv = data[0];
            int16_t maxv = data[0];
            uint64_t ii;

            for (ii = 1; ii < nelem; ii++)
            {
                if (data[ii] < minv) minv = data[ii];
                if (data[ii] > maxv) maxv = data[ii];
            }

            if (maxv == minv)
            {
                memset(data, 0, nelem * sizeof(int16_t));
                return DAO_SUCCESS;
            }

            double scale = 65535.0 / ((double) maxv - (double) minv);
            for (ii = 0; ii < nelem; ii++)
            {
                double v = (((double) data[ii] - (double) minv) * scale) - 32768.0;
                if (v < -32768.0) v = -32768.0;
                if (v >  32767.0) v =  32767.0;
                data[ii] = (int16_t) (v + (v >= 0.0 ? 0.5 : -0.5));
            }
        }
        break;

        case _DATATYPE_UINT32:
        {
            uint32_t *data = img[0].array.UI32;
            uint32_t minv = data[0];
            uint32_t maxv = data[0];
            uint64_t ii;

            for (ii = 1; ii < nelem; ii++)
            {
                if (data[ii] < minv) minv = data[ii];
                if (data[ii] > maxv) maxv = data[ii];
            }

            if (maxv == minv)
            {
                memset(data, 0, nelem * sizeof(uint32_t));
                return DAO_SUCCESS;
            }

            double scale = 4294967295.0 / ((double) maxv - (double) minv);
            for (ii = 0; ii < nelem; ii++)
            {
                data[ii] = (uint32_t) (((double) data[ii] - (double) minv) * scale + 0.5);
            }
        }
        break;

        case _DATATYPE_INT32:
        {
            int32_t *data = img[0].array.SI32;
            int32_t minv = data[0];
            int32_t maxv = data[0];
            uint64_t ii;

            for (ii = 1; ii < nelem; ii++)
            {
                if (data[ii] < minv) minv = data[ii];
                if (data[ii] > maxv) maxv = data[ii];
            }

            if (maxv == minv)
            {
                memset(data, 0, nelem * sizeof(int32_t));
                return DAO_SUCCESS;
            }

            double scale = 4294967295.0 / ((double) maxv - (double) minv);
            for (ii = 0; ii < nelem; ii++)
            {
                double v = (((double) data[ii] - (double) minv) * scale) - 2147483648.0;
                if (v < -2147483648.0) v = -2147483648.0;
                if (v >  2147483647.0) v =  2147483647.0;
                data[ii] = (int32_t) (v + (v >= 0.0 ? 0.5 : -0.5));
            }
        }
        break;

        case _DATATYPE_UINT64:
        {
            uint64_t *data = img[0].array.UI64;
            uint64_t minv = data[0];
            uint64_t maxv = data[0];
            uint64_t ii;

            for (ii = 1; ii < nelem; ii++)
            {
                if (data[ii] < minv) minv = data[ii];
                if (data[ii] > maxv) maxv = data[ii];
            }

            if (maxv == minv)
            {
                memset(data, 0, nelem * sizeof(uint64_t));
                return DAO_SUCCESS;
            }

            long double scale = 18446744073709551615.0L / ((long double) maxv - (long double) minv);
            for (ii = 0; ii < nelem; ii++)
            {
                data[ii] = (uint64_t) (((long double) data[ii] - (long double) minv) * scale + 0.5L);
            }
        }
        break;

        case _DATATYPE_INT64:
        {
            int64_t *data = img[0].array.SI64;
            int64_t minv = data[0];
            int64_t maxv = data[0];
            uint64_t ii;

            for (ii = 1; ii < nelem; ii++)
            {
                if (data[ii] < minv) minv = data[ii];
                if (data[ii] > maxv) maxv = data[ii];
            }

            if (maxv == minv)
            {
                memset(data, 0, nelem * sizeof(int64_t));
                return DAO_SUCCESS;
            }

            long double scale = 18446744073709551615.0L / ((long double) maxv - (long double) minv);
            for (ii = 0; ii < nelem; ii++)
            {
                long double v = (((long double) data[ii] - (long double) minv) * scale)
                                - 9223372036854775808.0L;

                if (v < -9223372036854775808.0L) v = -9223372036854775808.0L;
                if (v >  9223372036854775807.0L) v =  9223372036854775807.0L;

                data[ii] = (int64_t) (v + (v >= 0.0L ? 0.5L : -0.5L));
            }
        }
        break;

        case _DATATYPE_FLOAT:
        {
            float *data = img[0].array.F;
            float minv = data[0];
            float maxv = data[0];
            uint64_t ii;

            for (ii = 1; ii < nelem; ii++)
            {
                if (data[ii] < minv) minv = data[ii];
                if (data[ii] > maxv) maxv = data[ii];
            }

            if (maxv == minv)
            {
                for (ii = 0; ii < nelem; ii++)
                {
                    data[ii] = 0.0f;
                }
                return DAO_SUCCESS;
            }

            float scale = 1.0f / (maxv - minv);
            for (ii = 0; ii < nelem; ii++)
            {
                data[ii] = (data[ii] - minv) * scale;
            }
        }
        break;

        case _DATATYPE_DOUBLE:
        {
            double *data = img[0].array.D;
            double minv = data[0];
            double maxv = data[0];
            uint64_t ii;

            for (ii = 1; ii < nelem; ii++)
            {
                if (data[ii] < minv) minv = data[ii];
                if (data[ii] > maxv) maxv = data[ii];
            }

            if (maxv == minv)
            {
                for (ii = 0; ii < nelem; ii++)
                {
                    data[ii] = 0.0;
                }
                return DAO_SUCCESS;
            }

            double scale = 1.0 / (maxv - minv);
            for (ii = 0; ii < nelem; ii++)
            {
                data[ii] = (data[ii] - minv) * scale;
            }
        }
        break;

        default:
            daoError("unsupported datatype %d\n", img[0].md[0].atype);
            return DAO_ERROR;
    }

    return DAO_SUCCESS;
}

/**
 * @brief Applies a high-pass filter (HPF) to modal coefficients, Single precision.
 *
 * This function applies a first-order high-pass filter (HPF) to a vector of modal coefficients.
 * It uses the recursive formula:
 *
 * \f[
 *    H_n = \alpha H_{n-1} + \alpha (C_n - C_{n-1})
 * \f]
 * where
 * \f[
 *    \alpha = \exp\left(-2\pi \frac{f_{\text{cutoff}}}{f_{\text{loop}}}\right)
 * \f]
 *
 * @param[out] H        Pointer to the output array of filtered coefficients (size `size`).
 * @param[in]  C        Pointer to the current frame unfiltered coefficients (size `size`).
 * @param[in]  CPrev    Pointer to the previous frame unfiltered coefficients (size `size`).
 * @param[in]  HPrev    Pointer to the previous frame filtered coefficients (size `size`).
 * @param[in]  fCutoff  Cutoff frequency of the high-pass filter (Hz).
 * @param[in]  fLoop    Frame rate (sampling frequency) (Hz).
 * @param[in]  size     Number of coefficients in the vectors.
 *
 * @return DAO_SUCCESS (typically 0) on success.
 *
 * @note It is assumed that all input arrays (`H`, `C`, `CPrev`, `HPrev`) have at least `size` elements.
 * @note Typically `HPrev` and `CPrev` are from the previous frame and need to be updated externally.
 */
int_fast8_t daoToolsHighPassFilter(float* H,
    const float* C,
    const float* CPrev,
    const float* HPrev,
    float fCutoff,
    float fLoop,
    int size) {
    daoTrace("\n");
    float alpha = exp(-2.0 * M_PI * fCutoff / fLoop);
    for (int i = 0; i < size; ++i) {
        H[i] = alpha * HPrev[i] + alpha * (C[i] - CPrev[i]);
    }

    return DAO_SUCCESS;
}

/**
 * @brief Applies a high-pass filter (HPF) to modal coefficients. Double precision
 *
 * This function applies a first-order high-pass filter (HPF) to a vector of modal coefficients.
 * It uses the recursive formula:
 *
 * \f[
 *    H_n = \alpha H_{n-1} + \alpha (C_n - C_{n-1})
 * \f]
 * where
 * \f[
 *    \alpha = \exp\left(-2\pi \frac{f_{\text{cutoff}}}{f_{\text{loop}}}\right)
 * \f]
 *
 * @param[out] H        Pointer to the output array of filtered coefficients (size `size`).
 * @param[in]  C        Pointer to the current frame unfiltered coefficients (size `size`).
 * @param[in]  CPrev    Pointer to the previous frame unfiltered coefficients (size `size`).
 * @param[in]  HPrev    Pointer to the previous frame filtered coefficients (size `size`).
 * @param[in]  fCutoff  Cutoff frequency of the high-pass filter (Hz).
 * @param[in]  fLoop    Frame rate (sampling frequency) (Hz).
 * @param[in]  size     Number of coefficients in the vectors.
 *
 * @return DAO_SUCCESS (typically 0) on success.
 *
 * @note It is assumed that all input arrays (`H`, `C`, `CPrev`, `HPrev`) have at least `size` elements.
 * @note Typically `HPrev` and `CPrev` are from the previous frame and need to be updated externally.
 */
int_fast8_t daoToolsHighPassFilterDouble(double* H,
    const double* C,
    const double* CPrev,
    const double* HPrev,
    double fCutoff,
    double fLoop,
    int size) {
    daoTrace("\n");
    double alpha = exp(-2.0 * M_PI * fCutoff / fLoop);

    for (int i = 0; i < size; ++i) {
        H[i] = alpha * HPrev[i] + alpha * (C[i] - CPrev[i]);
    }

    return DAO_SUCCESS;
}

/**
 * @brief Combine multiple DM channels into a single output image.
 *
 * Each channel value is clipped before accumulation, and an optional piston
 * removal step subtracts the mean value across channels for each element.
 *
 * @param imageCube Input cube of per-channel command images.
 * @param image Output image receiving the combined command.
 * @param nbChannel Number of input channels.
 * @param nbVal Number of values per channel.
 * @param removePiston Non-zero to subtract the mean channel value.
 * @param clipping Symmetric clipping threshold applied before accumulation.
 *
 * @return DAO_SUCCESS on success.
 */
int_fast8_t daoDmCombine(IMAGE **imageCube, IMAGE *image, int nbChannel, int nbVal, int removePiston, double clipping)
{
    daoTrace("\n");
    int pp;
    int k;
    image->md[0].write = 1;
    
    if (image->md[0].atype == _DATATYPE_FLOAT)
    {
        for (pp=0; pp<nbVal; pp++)
        {   
            float sum = 0.0f;
            image[0].array.F[pp] = 0;
            for (k = 0;k < nbChannel;k++) {
                if (imageCube[k][0].array.F[pp] > clipping) {
                    image[0].array.F[pp] = (float)clipping;
                }
                else if (imageCube[k][0].array.F[pp] < -clipping) {
                    image[0].array.F[pp] = -(float)clipping;
                }
                else {
                    image[0].array.F[pp] = imageCube[k][0].array.F[pp];
                }
                sum += image[0].array.F[pp];
            }
            if (removePiston) {
                for (k = 0;k < nbChannel;k++) {
                    image[0].array.F[pp] -= sum / nbChannel;
                }
            }
        }
    }
    else if (image->md[0].atype == _DATATYPE_DOUBLE)
    {
        for (pp=0; pp<nbVal; pp++)
        {   
            double sum = 0.0;
            image[0].array.D[pp] = 0;
            for(k=0;k<nbChannel;k++)
            {
                if (imageCube[k][0].array.D[pp] > clipping)
                {
                    image[0].array.D[pp] = clipping;
                }
                else if (imageCube[k][0].array.D[pp] < -clipping)
                {
                    image[0].array.D[pp] = -clipping;
                }
                else
                {
                    image[0].array.D[pp] = imageCube[k][0].array.D[pp];
                }
                sum += image[0].array.D[pp];
            }
            if (removePiston)
            {
                for(k=0;k<nbChannel;k++) 
                {
                    image[0].array.D[pp] -= sum / nbChannel;
                }
            }
        }
    }

    daoShmImagePart2ShmFinalize(image);

    return DAO_SUCCESS;
}

/**
 * @brief Copy a contiguous image buffer into a destination image at an offset.
 *
 * The destination shared-memory image is marked writable during the copy and
 * can optionally be finalized once the transfer is complete.
 *
 * @param imageIn Source image.
 * @param imageOut Destination image.
 * @param nbVal Number of scalar values to copy.
 * @param position Destination starting index.
 * @param finalize Set to `1` to finalize the destination SHM image.
 *
 * @return DAO_SUCCESS on success.
 */
int_fast8_t daoShmCopyToPosition(IMAGE *imageIn, IMAGE *imageOut,
                                 int nbVal, int position, int finalize)
{
    daoTrace("\n");

    imageOut->md[0].write = 1;

    if (imageIn->md[0].atype == _DATATYPE_FLOAT) {
        memcpy(&imageOut[0].array.F[position],
            &imageIn[0].array.F[0],
            (size_t)nbVal * sizeof(float));
    }
    else if (imageIn->md[0].atype == _DATATYPE_DOUBLE) {
        memcpy(&imageOut[0].array.D[position],
            &imageIn[0].array.D[0],
            (size_t)nbVal * sizeof(double));
    }

    imageOut->md[0].write = 0;

    if (finalize == 1) {
        daoShmImagePart2ShmFinalize(imageOut);
    }

    return DAO_SUCCESS;
}

#ifdef __APPLE__

#include <errno.h>
#include <stdio.h>
#include <mach/mach_time.h>
#include <sys/time.h>

/**
 * @brief macOS fallback for `sched_setscheduler`.
 *
 * macOS does not expose Linux real-time scheduling policies such as
 * `SCHED_FIFO`, so this implementation behaves as a no-op.
 */
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param) {
    // macOS does not support real-time policies (SCHED_FIFO, etc.)
    // Just log and return success as a no-op
    (void)pid;
    (void)policy;
    (void)param;
    fprintf(stderr, "Warning: sched_setscheduler is not supported on macOS, ignoring.\n");
    return 0;
}

/**
 * @brief macOS fallback for `sched_getscheduler`.
 *
 * @param pid Process identifier, ignored on macOS.
 *
 * @return `SCHED_OTHER` to indicate non real-time scheduling.
 */
int sched_getscheduler(pid_t pid) {
    (void)pid;
    return SCHED_OTHER;
}

/**
 * @brief macOS fallback for `sched_getparam`.
 *
 * @param pid Process identifier, ignored on macOS.
 * @param param Output scheduling parameter structure.
 *
 * @return 0 on success.
 */
int sched_getparam(pid_t pid, struct sched_param *param) {
    (void)pid;
    if (param != NULL)
    {
        param->sched_priority = 0;
    }
    return 0;
}

/**
 * @brief macOS fallback for `clock_nanosleep`.
 *
 * Absolute deadlines are converted to a relative `nanosleep` delay.
 *
 * @param clock_id Clock used to interpret absolute deadlines.
 * @param flags Sleep mode flags, including `TIMER_ABSTIME`.
 * @param request Requested sleep duration or deadline.
 * @param remain Remaining unslept time if interrupted.
 *
 * @return 0 on success, or the return value from `nanosleep`.
 */
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
    }
    else {
        // Relative sleep
        return nanosleep(request, remain);
    }
}

#endif // __APPLE__

/**
 * @brief Attempt to enable real-time scheduling for the current process.
 *
 * The function first locks current and future mappings into memory, then tries
 * to switch the process to `SCHED_FIFO` with the requested priority.
 *
 * @param rt_priority Requested real-time FIFO priority.
 */
void daoRtSetup(int rt_priority)
{
    struct sched_param sp;
    int policy;
    int cur_prio;

    /* Lock memory to avoid major page-fault jitter */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        daoTrace("mlockall failed: %s\n", strerror(errno));
    }

    /* Try to switch to RT FIFO */
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = rt_priority;

    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        daoTrace("sched_setscheduler(SCHED_FIFO,%d) failed: %s\n",
            rt_priority, strerror(errno));
        return;
    }

    /* Report what we actually got */
    policy = sched_getscheduler(0);
    cur_prio = sched_getparam(0, &sp) == 0 ? sp.sched_priority : -1;

    if (policy == SCHED_FIFO)
        daoTrace("RT enabled: SCHED_FIFO prio=%d\n", cur_prio);
    else
        daoTrace("RT not FIFO: policy=%d prio=%d\n", policy, cur_prio);
}
