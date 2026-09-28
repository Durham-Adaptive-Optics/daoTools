/*
 * benchMvMGPU - round-trip latency of daoMvMGPU with CPU or GPU SHMs.
 * Driven by bench_mvm_gpu.py.
 *
 *   benchMvMGPU setup MODE NIN NOUT DEVICE     create /tmp/bmvm_{in,mat,out}.im.shm
 *   benchMvMGPU run   MODE FRAMES              write inputs, time until the output is published
 *
 * MODE: cpu         host SHMs (daoMvMGPU copies in and out)
 *       gpu         GPU SHMs, input written from the host with daoShmSetData
 *       gpu-mirror  as gpu, the output keeps its /tmp host copy
 *       gpu-kernel  GPU SHMs, input written by a GPU stage (daoShmSetDataDevice)
 *
 * run prints the latency (us) of every frame on one line: "LAT v1 v2 ...",
 * then "CHECK ok|FAIL max_rel_err" from comparing outputs with a CPU gemv.
 */
#include <cuda_runtime.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dao.h"

#define IN  "/tmp/bmvm_in.im.shm"
#define MAT "/tmp/bmvm_mat.im.shm"
#define OUT "/tmp/bmvm_out.im.shm"

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec * 1e-3;
}

static float mat_value(long o, long i) { return (float) (((o * 7 + i * 13) % 17) - 8) / 64.f; }
static float in_value(long i, long k) { return (float) (((i * 3 + k * 5) % 11) - 5) / 8.f; }

static int create(IMAGE *img, const char *name, uint32_t *size, long naxis, int gpu, int device, int mirror)
{
    if (!gpu)
        return daoShmCreate(img, name, naxis, size, _DATATYPE_FLOAT, 1, 0);
    return daoShmCreateGpu(img, name, naxis, size, _DATATYPE_FLOAT, device, 0, mirror ? DAO_GPU_MIRROR : 0);
}

int main(int argc, char **argv)
{
    daoSetLogLevel(1);
    if (argc < 4)
        return 2;
    const char *mode = argv[2];
    int gpu = strcmp(mode, "cpu") != 0;

    if (!strcmp(argv[1], "setup")) {
        long nin = atol(argv[3]), nout = atol(argv[4]);
        int device = atoi(argv[5]);
        IMAGE in, mat, out;
        uint32_t sin_[2] = { (uint32_t) nin, 1 }, sout[2] = { (uint32_t) nout, 1 };   /* dao vectors: n x 1 */
        uint32_t smat[2] = { (uint32_t) nout, (uint32_t) nin };   /* daoMvMGPU: size[0]=outputs */
        float *m = (float *) malloc(sizeof(float) * nin * nout);
        for (long o = 0; o < nout; o++)
            for (long i = 0; i < nin; i++)
                m[o * nin + i] = mat_value(o, i);
        if (create(&in, IN, sin_, 2, gpu, device, 0) || create(&mat, MAT, smat, 2, gpu, device, 0)
            || create(&out, OUT, sout, 2, gpu, device, !strcmp(mode, "gpu-mirror"))) {
            printf("SETUP FAILED\n");
            return 1;
        }
        daoShmSetData(&mat, m, (uint32_t) (nin * nout));
        printf("SETUP OK\n");
        return 0;
    }

    /* run */
    int frames = atoi(argv[3]);
    IMAGE in, out;
    if (daoShmOpen(IN, &in) || daoShmOpen(OUT, &out)) {
        printf("OPEN FAILED\n");
        return 1;
    }
    long nin = (long) in.md[0].nelement, nout = (long) out.md[0].nelement;
    float *h = (float *) malloc(sizeof(float) * nin), *res = (float *) malloc(sizeof(float) * nout);
    float *d = NULL;
    cudaStream_t stream = 0;
    if (!strcmp(mode, "gpu-kernel")) {
        cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
        cudaMalloc(&d, sizeof(float) * nin);
    }

    double *lat = (double *) malloc(sizeof(double) * frames), maxerr = 0;
    int checked = 0;
    for (int k = 0; k < frames + 20; k++) {             /* 20 warm-up frames */
        for (long i = 0; i < nin; i++)
            h[i] = in_value(i, k);
        if (d)
            cudaMemcpy(d, h, sizeof(float) * nin, cudaMemcpyHostToDevice);   /* not timed */
        uint64_t target = out.md[0].cnt0 + 1;

        double t0 = now_us();
        if (d)
            daoShmSetDataDevice(&in, d, (uint32_t) nin, stream);
        else
            daoShmSetData(&in, h, (uint32_t) nin);
        while (out.md[0].cnt0 < target) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 2;
            if (daoShmWaitSemTimeout(&out, 1, &ts) != DAO_SUCCESS && out.md[0].cnt0 < target) {
                printf("TIMEOUT at frame %d\n", k);
                return 1;
            }
        }
        double t1 = now_us();
        if (k >= 20)
            lat[k - 20] = t1 - t0;

        if (k % 97 == 0) {                               /* check against a CPU gemv */
            void *p;
            uint32_t idx;
            uint64_t c;
            daoShmGetData(&out, &p, &idx, &c);
            memcpy(res, p, sizeof(float) * nout);
            for (long o = 0; o < nout; o++) {
                double ref = 0, err;
                for (long i = 0; i < nin; i++)
                    ref += (double) mat_value(o, i) * h[i];
                err = fabs(res[o] - ref) / (fabs(ref) + 1.0);
                if (err > maxerr)
                    maxerr = err;
            }
            checked++;
        }
    }
    printf("LAT");
    for (int k = 0; k < frames; k++)
        printf(" %.2f", lat[k]);
    printf("\nCHECK %s %.2e (%d frames checked)\n", maxerr < 1e-4 ? "ok" : "FAIL", maxerr, checked);
    return 0;
}
