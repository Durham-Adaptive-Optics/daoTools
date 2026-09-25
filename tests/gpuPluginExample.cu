/*
 * gpuPluginExample -- a daoGpuPipeline plugin with one stage type, "scale":
 *
 *     plugins: [build/tests/libgpuPluginExample.so]
 *     stages:
 *       - scale: {in: <float SHM>, k: <float SHM, 1 or n values>, out: <float SHM>}
 *
 * out = k * in (k scalar or per value). The pattern of a stage written outside
 * daoTools (daoGpuStages.h): a factory reading its arguments, and update / run /
 * destroy functions; k is uploaded again between frames when it changes.
 * Used by tests/test_gpu_plugin.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include "daoGpuStages.h"

struct Scale {
    daoGpuPort *in, *out;
    IMAGE *k;
    uint64_t c0;                 /* k's cnt0 when last uploaded */
    int n, kn;
    float *dk;
};

__global__ void scaleKernel(const float *in, const float *k, float *out, int n, int kn)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        out[i] = in[i] * k[kn == 1 ? 0 : i];
}

static int scaleUpdate(void *self, cudaStream_t)
{
    Scale *s = (Scale *) self;
    if (s->k->md[0].cnt0 == s->c0)
        return 0;
    s->c0 = s->k->md[0].cnt0;
    return cudaMemcpy(s->dk, s->k->array.F, s->kn * sizeof(float), cudaMemcpyHostToDevice) == cudaSuccess ? 1 : -1;
}

static int scaleRun(void *self, cudaStream_t st)
{
    Scale *s = (Scale *) self;
    scaleKernel<<<(s->n + 255) / 256, 256, 0, st>>>((const float *) s->in->d, s->dk, (float *) s->out->d, s->n,
                                                     s->kn);
    return cudaGetLastError() == cudaSuccess;
}

static void scaleDestroy(void *self)
{
    Scale *s = (Scale *) self;
    cudaFree(s->dk);
    free(s);
}

static daoGpuStage *scaleCreate(const daoGpuStageArgs *a)
{
    daoGpuPort *in = daoGpuArgPort(a, "in"), *out = daoGpuArgPort(a, "out");
    IMAGE *k = daoGpuArgShm(a, "k");
    if (!in || !out || !k)
        return NULL;
    int n = (int) in->shm->md[0].nelement, kn = (int) k->md[0].nelement;
    if (in->shm->md[0].atype != _DATATYPE_FLOAT || out->shm->md[0].atype != _DATATYPE_FLOAT
        || k->md[0].atype != _DATATYPE_FLOAT || (int) out->shm->md[0].nelement < n || (kn != 1 && kn < n)) {
        fprintf(stderr, "scale: float in, out and k (1 or %d values) expected\n", n);
        return NULL;
    }
    Scale *s = (Scale *) calloc(1, sizeof *s);
    s->in = in;
    s->out = out;
    s->k = k;
    s->n = n;
    s->kn = kn;
    s->c0 = k->md[0].cnt0 - 1;                   /* upload at the first update */
    cudaMalloc(&s->dk, kn * sizeof(float));
    daoGpuStageOps ops = {scaleUpdate, scaleRun, NULL, scaleDestroy};
    return daoGpuStageCreate("scale", in, out, 1, &ops, s);
}

extern "C" void daoGpuPluginRegister(void)
{
    daoGpuRegisterStage("scale", scaleCreate);
}
