/*
 * pipelineDriver - feed frames to a pipeline (processes or daoGpuPipeline) and time it.
 * Used by bench_pyramid_pipeline.py and bench_sh_pipeline.py.
 *
 *   pipelineDriver IN_SHM OUT_SHM FRAMES_FILE N OUT_FILE
 *
 * FRAMES_FILE holds N frames of IN_SHM's size and type. Each frame is written to
 * IN_SHM, then the driver waits until OUT_SHM is published (semaphore 2) and
 * records the round trip and OUT_SHM (float32). Prints "LAT v1 v2 ..." (us);
 * OUT_FILE receives the N outputs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dao.h"

static double nowUs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec * 1e-3;
}

static size_t elemSize(uint8_t atype)
{
    switch (atype) {
    case _DATATYPE_UINT8: case _DATATYPE_INT8: return 1;
    case _DATATYPE_UINT16: case _DATATYPE_INT16: return 2;
    case _DATATYPE_UINT64: case _DATATYPE_INT64: case _DATATYPE_DOUBLE: return 8;
    default: return 4;
    }
}

int main(int argc, char **argv)
{
    IMAGE im, out;
    if (argc < 6)
        return 2;
    int n = atoi(argv[4]);
    daoSetLogLevel(1);
    if (daoShmOpen(argv[1], &im) || daoShmOpen(argv[2], &out)) {
        printf("OPEN FAILED\n");
        return 1;
    }
    size_t npix = im.md[0].nelement, bytes = npix * elemSize(im.md[0].atype), nout = out.md[0].nelement;
    char *frames = malloc(bytes * n);
    float *res = malloc(nout * n * sizeof(float));
    double *lat = malloc(n * sizeof(double));
    FILE *f = fopen(argv[3], "rb");
    if (!f || fread(frames, bytes, n, f) != (size_t) n) {
        printf("cannot read frames\n");
        return 1;
    }
    fclose(f);

    for (int k = -20; k < n; k++) {                 /* 20 warm-up frames */
        int idx = k < 0 ? 0 : k;
        uint64_t target = out.md[0].cnt0 + 1;
        double t0 = nowUs();
        daoShmSetData(&im, frames + bytes * idx, (uint32_t) npix);
        while (out.md[0].cnt0 < target) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 2;
            if (daoShmWaitSemTimeout(&out, 2, &ts) != DAO_SUCCESS && out.md[0].cnt0 < target) {
                printf("TIMEOUT at frame %d\n", k);
                return 1;
            }
        }
        double t1 = nowUs();
        if (k >= 0) {
            void *p;
            uint32_t i;
            uint64_t c;
            lat[k] = t1 - t0;
            daoShmGetData(&out, &p, &i, &c);
            memcpy(res + nout * k, p, nout * sizeof(float));
        }
    }
    f = fopen(argv[5], "wb");
    fwrite(res, sizeof(float), nout * n, f);
    fclose(f);
    printf("LAT");
    for (int k = 0; k < n; k++)
        printf(" %.2f", lat[k]);
    printf("\n");
    return 0;
}
