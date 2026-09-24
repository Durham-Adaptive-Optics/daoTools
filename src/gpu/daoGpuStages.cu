/**
 * @file    daoGpuStages.cu
 * @brief   GPU stages on dao SHMs (see daoGpuStages.h).
 *
 * Each stage reproduces the CPU daoTools app it is named after; the
 * parameter preparation (masks, flat, reference, ...) is done on the host with
 * the same arithmetic, and only when a parameter SHM changes.
 */
#include <cublas_v2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include "daoGpuStages.h"

/* ============================================================== helpers */
static size_t elemSize(uint8_t atype)
{
    switch (atype) {
    case _DATATYPE_UINT8:  case _DATATYPE_INT8:  return 1;
    case _DATATYPE_UINT16: case _DATATYPE_INT16: return 2;
    case _DATATYPE_UINT32: case _DATATYPE_INT32: case _DATATYPE_FLOAT: return 4;
    case _DATATYPE_UINT64: case _DATATYPE_INT64: case _DATATYPE_DOUBLE:
    case _DATATYPE_COMPLEX_FLOAT: return 8;
    case _DATATYPE_COMPLEX_DOUBLE: return 16;
    default: return 0;
    }
}

#define CUDA_OK(call, what)                                                    \
    do {                                                                       \
        cudaError_t e_ = (call);                                               \
        if (e_ != cudaSuccess) {                                               \
            daoError("%s: %s\n", what, cudaGetErrorString(e_));               \
            return 0;                                                          \
        }                                                                      \
    } while (0)

static uint64_t cnt0(const IMAGE *im) { return im->md[0].cnt0; }

/* ================================================================ ports */
extern "C" int daoGpuPortInit(daoGpuPort *p, IMAGE *shm, int mapHost)
{
    memset(p, 0, sizeof *p);
    p->shm = shm;
    p->bytes = (size_t) shm->md[0].nelement * elemSize(shm->md[0].atype);
    if (daoShmIsGpu(shm)) {
        if (!shm->d_array) {
            daoError("%s: GPU SHM not accessible from this process\n", shm->name);
            return 0;
        }
        p->onGpu = 1;
        p->d = shm->d_array;
        p->mirror = (shm->md[0].gpu_flags & DAO_GPU_MIRROR) != 0;
        return 1;
    }
    if (mapHost) {
        /* zero copy: the GPU reads/writes the host SHM itself over PCIe */
        cudaError_t e = cudaHostRegister(shm->array.V, p->bytes, cudaHostRegisterPortable | cudaHostRegisterMapped);
        if (e == cudaErrorHostMemoryAlreadyRegistered)
            cudaGetLastError();                      /* not an error here: clear it */
        if ((e == cudaSuccess || e == cudaErrorHostMemoryAlreadyRegistered)
            && cudaHostGetDevicePointer(&p->d, shm->array.V, 0) == cudaSuccess) {
            p->mapped = 1;
            return 1;
        }
        cudaGetLastError();
        struct statfs fs;
        if (statfs(shm->name, &fs) == 0 && fs.f_type != TMPFS_MAGIC)
            daoWarning("%s is not in RAM (its filesystem is not tmpfs): the GPU cannot read it in place and "
                       "copies it each frame instead, which is slower. Put the SHMs in /dev/shm, or mount /tmp "
                       "as tmpfs (see the GPU SHM documentation)\n", shm->name);
        else
            daoWarning("%s: cannot map into the GPU, copying it instead\n", shm->name);
    }
    CUDA_OK(cudaMalloc(&p->d, p->bytes), "cudaMalloc");
    /* pin the host SHM for asynchronous copies (fine if already pinned) */
    if (cudaHostRegister(shm->array.V, p->bytes, cudaHostRegisterPortable) != cudaSuccess)
        cudaGetLastError();
    return 1;
}

extern "C" void daoGpuPortFree(daoGpuPort *p)
{
    if (p->d && !p->onGpu) {
        if (!p->mapped)
            cudaFree(p->d);
        cudaHostUnregister(p->shm->array.V);
        cudaGetLastError();
    }
    p->d = NULL;
}

extern "C" void daoGpuPortUpload(daoGpuPort *p, cudaStream_t s)
{
    if (!p->onGpu && !p->mapped)
        cudaMemcpyAsync(p->d, p->shm->array.V, p->bytes, cudaMemcpyHostToDevice, s);
}

extern "C" void daoGpuPortDownload(daoGpuPort *p, cudaStream_t s)
{
    if ((!p->onGpu && !p->mapped) || p->mirror)
        cudaMemcpyAsync(p->shm->array.V, p->d, p->bytes, cudaMemcpyDeviceToHost, s);
}

/* A parameter SHM kept on the GPU: uploaded again when its cnt0 changes. */
struct Param {
    IMAGE *im;
    uint64_t c0;
    void *d;
    size_t bytes;
};

static int paramInit(Param *p, IMAGE *im, size_t bytes)
{
    p->im = im;
    p->bytes = bytes;
    p->c0 = cnt0(im) - 1;                            /* force the first upload */
    CUDA_OK(cudaMalloc(&p->d, bytes), "cudaMalloc");
    return 1;
}

/* 1: reloaded, 0: unchanged, -1: error */
static int paramSync(Param *p)
{
    if (p->c0 == cnt0(p->im))
        return 0;
    p->c0 = cnt0(p->im);
    if (cudaMemcpy(p->d, p->im->array.V, p->bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
        daoError("upload %s: %s\n", p->im->name, cudaGetErrorString(cudaGetLastError()));
        return -1;
    }
    return 1;
}

static void paramFree(Param *p)
{
    cudaFree(p->d);
    p->d = NULL;
}

static bool isFloat(const daoGpuPort *p) { return p->shm->md[0].atype == _DATATYPE_FLOAT; }
static bool isFloat(const IMAGE *im) { return im->md[0].atype == _DATATYPE_FLOAT; }
static long nelem(const daoGpuPort *p) { return (long) p->shm->md[0].nelement; }
static long nelem(const IMAGE *im) { return (long) im->md[0].nelement; }

/* atomicAdd on doubles is native from sm_60; emulate it below (sm_5x builds). */
__device__ __forceinline__ void atomicAddDouble(double *addr, double v)
{
#if !defined(__CUDA_ARCH__) || __CUDA_ARCH__ >= 600
    atomicAdd(addr, v);
#else
    unsigned long long *a = (unsigned long long *) addr, old = *a, prev;
    do {
        prev = old;
        old = atomicCAS(a, prev, __double_as_longlong(__longlong_as_double((long long) prev) + v));
    } while (old != prev);
#endif
}

/* Sum over the block, returned to every thread (all threads must call it). */
__device__ float blockSum(float v)
{
    __shared__ float part[32];
    __shared__ float total;
    for (int o = 16; o > 0; o >>= 1)
        v += __shfl_down_sync(0xffffffffu, v, o);
    __syncthreads();                                 /* previous call's readers are done */
    if ((threadIdx.x & 31) == 0)
        part[threadIdx.x >> 5] = v;
    __syncthreads();
    if (threadIdx.x < 32) {
        v = threadIdx.x < (blockDim.x + 31) / 32 ? part[threadIdx.x] : 0.f;
        for (int o = 16; o > 0; o >>= 1)
            v += __shfl_down_sync(0xffffffffu, v, o);
        if (threadIdx.x == 0)
            total = v;
    }
    __syncthreads();
    return total;
}

__device__ float blockMax(float v)
{
    __shared__ float part[32];
    __shared__ float total;
    for (int o = 16; o > 0; o >>= 1)
        v = fmaxf(v, __shfl_down_sync(0xffffffffu, v, o));
    __syncthreads();
    if ((threadIdx.x & 31) == 0)
        part[threadIdx.x >> 5] = v;
    __syncthreads();
    if (threadIdx.x < 32) {
        v = threadIdx.x < (blockDim.x + 31) / 32 ? part[threadIdx.x] : -INFINITY;
        for (int o = 16; o > 0; o >>= 1)
            v = fmaxf(v, __shfl_down_sync(0xffffffffu, v, o));
        if (threadIdx.x == 0)
            total = v;
    }
    __syncthreads();
    return total;
}

/* Positions of mask == 1 pixels (the CPU tools' extraction order). */
static int *maskLut(IMAGE *mask, long imSize, int *n)
{
    *n = 0;
    for (long i = 0; i < imSize; i++)
        *n += mask->array.UI32[i] == 1;
    int *lut = (int *) malloc((*n > 0 ? *n : 1) * sizeof(int));
    for (long i = 0, k = 0; i < imSize; i++)
        if (mask->array.UI32[i] == 1)
            lut[k++] = (int) i;
    return lut;
}

/* Raw-type dispatch: CASE(ATYPE, T) for every numeric pixel type. */
#define DAO_FOR_EACH_PIXEL_TYPE(CASE)                                          \
    CASE(_DATATYPE_UINT8, uint8_t)                                             \
    CASE(_DATATYPE_INT8, int8_t)                                               \
    CASE(_DATATYPE_UINT16, uint16_t)                                           \
    CASE(_DATATYPE_INT16, int16_t)                                             \
    CASE(_DATATYPE_UINT32, uint32_t)                                           \
    CASE(_DATATYPE_INT32, int32_t)                                             \
    CASE(_DATATYPE_UINT64, uint64_t)                                           \
    CASE(_DATATYPE_INT64, int64_t)                                             \
    CASE(_DATATYPE_FLOAT, float)                                               \
    CASE(_DATATYPE_DOUBLE, double)

/* =============================================================== stages */
struct daoGpuStage {
    const char *name;
    daoGpuPort *in, *out;
    int cnt2FromIn;                                   /* propagate in.cnt2 to out */
    int (*update)(daoGpuStage *, cudaStream_t);
    int (*run)(daoGpuStage *, cudaStream_t);
    int (*post)(daoGpuStage *, cudaStream_t);        /* optional */
    void (*destroy)(daoGpuStage *);
};

extern "C" const char *daoGpuStageName(const daoGpuStage *s) { return s->name; }
extern "C" daoGpuPort *daoGpuStageOutput(daoGpuStage *s) { return s->out; }
extern "C" long long daoGpuStageOutputCnt2(const daoGpuStage *s)
{
    return s->cnt2FromIn ? (long long) s->in->shm->md[0].cnt2 : -1;
}
extern "C" int daoGpuStageUpdate(daoGpuStage *s, cudaStream_t st) { return s->update(s, st); }
extern "C" int daoGpuStageRun(daoGpuStage *s, cudaStream_t st) { return s->run(s, st); }
extern "C" int daoGpuStagePost(daoGpuStage *s, cudaStream_t st) { return s->post ? s->post(s, st) : 1; }
extern "C" void daoGpuStageDestroy(daoGpuStage *s)
{
    if (s)
        s->destroy(s);
}

/* ------------------------------------------- calIntensityNorm / calIntensity */
/* daoCalIntensityNorm on the GPU:
 *   cal[k] = (raw[lut[k]] - bg[k]) * w[k]           w = ff>0 ? ff : 0, 0 outside illum
 *   out[k] = cal[k] / sum(cal) - refTerm[k]         refTerm = ref * illum / sum(ref * illum)
 * daoCalIntensity (norm = 0):
 *   cal[k] = (raw[lut[k]] - bg[k]) * w[k]           w = ff>0 ? ff : 0
 *   out[k] = (cal[k] - ref[k]) / sum(cal * illum) * illum[k] */
struct CalStage {
    daoGpuStage base;
    int norm;
    IMAGE *ff, *bg, *ref, *valid, *illum;
    uint64_t c0ff, c0bg, c0ref, c0illum;
    int nValid;
    int *lut;                                        /* host */
    float *w, *bgp, *refTerm, *ill;                  /* host */
    int *dLut;
    float *dW, *dBg, *dRef, *dIll, *dSum;
};

template <typename T>
__global__ void calPass1(const T *raw, const int *lut, const float *bg, const float *w,
                         const float *sumMask, float *out, float *sum, int n)
{
    __shared__ float s[256];
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    float v = 0.f;
    if (k < n) {
        v = ((float) raw[lut[k]] - bg[k]) * w[k];
        out[k] = v;
        if (sumMask)
            v *= sumMask[k];
    }
    s[threadIdx.x] = v;
    __syncthreads();
    for (int m = blockDim.x / 2; m > 0; m >>= 1) {
        if (threadIdx.x < m)
            s[threadIdx.x] += s[threadIdx.x + m];
        __syncthreads();
    }
    if (threadIdx.x == 0)
        atomicAdd(sum, s[0]);
}

__global__ void calPass2(float *out, const float *refTerm, const float *sum, int n)
{
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    if (k < n) {
        float total = *sum;
        float inv = total > 0.f ? 1.f / total : 0.f;
        out[k] = out[k] * inv - refTerm[k];
    }
}

__global__ void calPass2Plain(float *out, const float *ref, const float *ill, const float *sum, int n)
{
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    if (k < n) {
        float total = *sum;
        float inv = total > 0.f ? 1.f / total : 0.f;
        out[k] = (out[k] - ref[k]) * inv * ill[k];
    }
}

static int calUpdate(daoGpuStage *b, cudaStream_t st)
{
    CalStage *c = (CalStage *) b;
    (void) st;
    if (c->c0ff == cnt0(c->ff) && c->c0bg == cnt0(c->bg) && c->c0ref == cnt0(c->ref)
        && c->c0illum == cnt0(c->illum))
        return 0;
    /* same arithmetic as daoCalIntensityNorm / daoCalIntensity */
    float sumRef = 0.f;
    for (int k = 0; k < c->nValid; k++) {
        int idx = c->lut[k];
        int il = c->illum->array.UI32[idx] == 1;
        float ff = c->ff->array.F[idx];
        c->bgp[k] = c->bg->array.F[idx];
        c->ill[k] = (float) il;
        if (c->norm) {
            c->w[k] = il ? (ff > 0.f ? ff : 0.f) : 0.f;
            if (il)
                sumRef += c->ref->array.F[idx];
        } else {
            c->w[k] = ff > 0.f ? ff : 0.f;
            c->refTerm[k] = c->ref->array.F[idx];
        }
    }
    if (c->norm) {
        float invSumRef = sumRef > 0.f ? 1.f / sumRef : 0.f;
        for (int k = 0; k < c->nValid; k++)
            c->refTerm[k] = c->ref->array.F[c->lut[k]] * invSumRef * c->ill[k];
    }
    size_t n = c->nValid * sizeof(float);
    CUDA_OK(cudaMemcpy(c->dW, c->w, n, cudaMemcpyHostToDevice), "upload flat");
    CUDA_OK(cudaMemcpy(c->dBg, c->bgp, n, cudaMemcpyHostToDevice), "upload background");
    CUDA_OK(cudaMemcpy(c->dRef, c->refTerm, n, cudaMemcpyHostToDevice), "upload reference");
    CUDA_OK(cudaMemcpy(c->dIll, c->ill, n, cudaMemcpyHostToDevice), "upload illuminated pixels");
    c->c0ff = cnt0(c->ff);
    c->c0bg = cnt0(c->bg);
    c->c0ref = cnt0(c->ref);
    c->c0illum = cnt0(c->illum);
    daoInfo("%s: flat/background/reference (re)loaded\n", b->name);
    return 1;
}

static int calRun(daoGpuStage *b, cudaStream_t st)
{
    CalStage *c = (CalStage *) b;
    int n = c->nValid, blocks = (n + 255) / 256;
    float *out = (float *) b->out->d;
    const float *sumMask = c->norm ? NULL : c->dIll;
    cudaMemsetAsync(c->dSum, 0, sizeof(float), st);
    switch (b->in->shm->md[0].atype) {
#define CAL_CASE(ATYPE, T)                                                                 \
    case ATYPE:                                                                            \
        calPass1<T><<<blocks, 256, 0, st>>>((const T *) b->in->d, c->dLut, c->dBg, c->dW,  \
                                            sumMask, out, c->dSum, n);                     \
        break;
    DAO_FOR_EACH_PIXEL_TYPE(CAL_CASE)
#undef CAL_CASE
    default:
        daoError("%s: unsupported raw image type %d\n", b->name, (int) b->in->shm->md[0].atype);
        return 0;
    }
    if (c->norm)
        calPass2<<<blocks, 256, 0, st>>>(out, c->dRef, c->dSum, n);
    else
        calPass2Plain<<<blocks, 256, 0, st>>>(out, c->dRef, c->dIll, c->dSum, n);
    return cudaGetLastError() == cudaSuccess;
}

static void calDestroy(daoGpuStage *b)
{
    CalStage *c = (CalStage *) b;
    cudaFree(c->dLut);
    cudaFree(c->dW);
    cudaFree(c->dBg);
    cudaFree(c->dRef);
    cudaFree(c->dIll);
    cudaFree(c->dSum);
    free(c->lut);
    free(c->w);
    free(c->bgp);
    free(c->refTerm);
    free(c->ill);
    free(c);
}

static daoGpuStage *calCreate(const char *name, int norm, daoGpuPort *raw, IMAGE *ff, IMAGE *bg,
                              IMAGE *ref, IMAGE *validPix, IMAGE *illumPix, daoGpuPort *out)
{
    CalStage *c = (CalStage *) calloc(1, sizeof *c);
    long imSize = (long) raw->shm->md[0].size[0] * raw->shm->md[0].size[1];
    c->base.name = name;
    c->base.in = raw;
    c->base.out = out;
    c->base.cnt2FromIn = 1;
    c->base.update = calUpdate;
    c->base.run = calRun;
    c->base.destroy = calDestroy;
    c->norm = norm;
    c->ff = ff;
    c->bg = bg;
    c->ref = ref;
    c->valid = validPix;
    c->illum = illumPix;
    c->lut = maskLut(validPix, imSize, &c->nValid);
    if (!isFloat(out) || nelem(out) < c->nValid) {
        daoError("%s: %s must be float with at least %d values\n", name, out->shm->name, c->nValid);
        free(c->lut);
        free(c);
        return NULL;
    }
    size_t n = c->nValid * sizeof(float);
    c->w = (float *) malloc(n);
    c->bgp = (float *) malloc(n);
    c->refTerm = (float *) malloc(n);
    c->ill = (float *) malloc(n);
    cudaMalloc(&c->dLut, c->nValid * sizeof(int));
    cudaMalloc(&c->dW, n);
    cudaMalloc(&c->dBg, n);
    cudaMalloc(&c->dRef, n);
    cudaMalloc(&c->dIll, n);
    cudaMalloc(&c->dSum, sizeof(float));
    cudaMemcpy(c->dLut, c->lut, c->nValid * sizeof(int), cudaMemcpyHostToDevice);
    c->c0ff = cnt0(ff) - 1;                          /* force the first update */
    daoInfo("%s: %d valid pixels -> %s\n", name, c->nValid, out->shm->name);
    return &c->base;
}

extern "C" daoGpuStage *daoGpuCalIntensityNormCreate(daoGpuPort *raw, IMAGE *ff, IMAGE *bg, IMAGE *ref,
                                                     IMAGE *validPix, IMAGE *illumPix, daoGpuPort *out)
{
    return calCreate("calIntensityNorm", 1, raw, ff, bg, ref, validPix, illumPix, out);
}

extern "C" daoGpuStage *daoGpuCalIntensityCreate(daoGpuPort *raw, IMAGE *ff, IMAGE *bg, IMAGE *ref,
                                                 IMAGE *validPix, IMAGE *illumPix, daoGpuPort *out)
{
    return calCreate("calIntensity", 0, raw, ff, bg, ref, validPix, illumPix, out);
}

/* ------------------------------------------------------------------ mvm */
/* daoMvMGPU: out = matrix . in, matrix size[0] = outputs, size[1] = inputs */
struct MvmStage {
    daoGpuStage base;
    IMAGE *matrix;
    uint64_t c0;
    int nIn, nOut;
    float *dM;
    cublasHandle_t h;
};

static int mvmUpdate(daoGpuStage *b, cudaStream_t st)
{
    MvmStage *m = (MvmStage *) b;
    (void) st;
    if (m->c0 == cnt0(m->matrix))
        return 0;
    CUDA_OK(cudaMemcpy(m->dM, m->matrix->array.F, (size_t) m->nIn * m->nOut * sizeof(float),
                       cudaMemcpyHostToDevice), "upload matrix");
    m->c0 = cnt0(m->matrix);
    daoInfo("mvm: matrix %s (re)loaded\n", m->matrix->name);
    return 1;
}

static int mvmRun(daoGpuStage *b, cudaStream_t st)
{
    MvmStage *m = (MvmStage *) b;
    const float one = 1.f, zero = 0.f;
    cublasSetStream(m->h, st);
    return cublasSgemv(m->h, CUBLAS_OP_T, m->nIn, m->nOut, &one, m->dM, m->nIn,
                       (const float *) b->in->d, 1, &zero, (float *) b->out->d, 1)
           == CUBLAS_STATUS_SUCCESS;
}

static void mvmDestroy(daoGpuStage *b)
{
    MvmStage *m = (MvmStage *) b;
    cublasDestroy(m->h);
    cudaFree(m->dM);
    free(m);
}

extern "C" daoGpuStage *daoGpuMvmCreate(daoGpuPort *in, IMAGE *matrix, daoGpuPort *out)
{
    MvmStage *m = (MvmStage *) calloc(1, sizeof *m);
    m->base.name = "mvm";
    m->base.in = in;
    m->base.out = out;
    m->base.update = mvmUpdate;
    m->base.run = mvmRun;
    m->base.destroy = mvmDestroy;
    m->matrix = matrix;
    m->nOut = matrix->md[0].size[0];
    m->nIn = matrix->md[0].size[1];
    if (matrix->md[0].atype != _DATATYPE_FLOAT || in->shm->md[0].atype != _DATATYPE_FLOAT
        || out->shm->md[0].atype != _DATATYPE_FLOAT) {
        daoError("mvm: only float SHMs are supported (%s)\n", matrix->name);
        free(m);
        return NULL;
    }
    if ((long) in->shm->md[0].nelement < m->nIn || (long) out->shm->md[0].nelement < m->nOut) {
        daoError("mvm: %s is %d x %d, but %s has %lu and %s %lu values\n", matrix->name, m->nOut, m->nIn,
                 in->shm->name, (unsigned long) in->shm->md[0].nelement, out->shm->name,
                 (unsigned long) out->shm->md[0].nelement);
        free(m);
        return NULL;
    }
    cudaMalloc(&m->dM, (size_t) m->nIn * m->nOut * sizeof(float));
    cublasCreate(&m->h);
    cublasSetPointerMode(m->h, CUBLAS_POINTER_MODE_HOST);
    m->c0 = cnt0(matrix) - 1;
    daoInfo("mvm: %s (%d x %d) . %s -> %s\n", matrix->name, m->nOut, m->nIn, in->shm->name, out->shm->name);
    return &m->base;
}

/* ------------------------------------------------------------ applyGain */
/* daoApplyGain: out = in * gain[0], or in * gain[k] (modal) */
struct GainStage {
    daoGpuStage base;
    IMAGE *gain;
    uint64_t c0;
    int n, modal;
    float *dGain;
};

__global__ void applyGainKernel(const float *in, const float *gain, float *out, int n, int modal)
{
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    if (k < n)
        out[k] = in[k] * gain[modal ? k : 0];
}

static int gainUpdate(daoGpuStage *b, cudaStream_t st)
{
    GainStage *g = (GainStage *) b;
    (void) st;
    if (g->c0 == cnt0(g->gain))
        return 0;
    CUDA_OK(cudaMemcpy(g->dGain, g->gain->array.F, (g->modal ? g->n : 1) * sizeof(float),
                       cudaMemcpyHostToDevice), "upload gain");
    g->c0 = cnt0(g->gain);
    return 1;
}

static int gainRun(daoGpuStage *b, cudaStream_t st)
{
    GainStage *g = (GainStage *) b;
    applyGainKernel<<<(g->n + 255) / 256, 256, 0, st>>>((const float *) b->in->d, g->dGain,
                                                          (float *) b->out->d, g->n, g->modal);
    return cudaGetLastError() == cudaSuccess;
}

static void gainDestroy(daoGpuStage *b)
{
    GainStage *g = (GainStage *) b;
    cudaFree(g->dGain);
    free(g);
}

extern "C" daoGpuStage *daoGpuApplyGainCreate(daoGpuPort *in, IMAGE *gain, daoGpuPort *out, int modal)
{
    GainStage *g = (GainStage *) calloc(1, sizeof *g);
    g->base.name = "applyGain";
    g->base.in = in;
    g->base.out = out;
    g->base.cnt2FromIn = 1;
    g->base.update = gainUpdate;
    g->base.run = gainRun;
    g->base.destroy = gainDestroy;
    g->gain = gain;
    g->n = (int) in->shm->md[0].nelement;
    g->modal = modal;
    if (in->shm->md[0].atype != _DATATYPE_FLOAT || gain->md[0].atype != _DATATYPE_FLOAT
        || out->shm->md[0].atype != _DATATYPE_FLOAT || (long) out->shm->md[0].nelement < g->n
        || (modal && (long) gain->md[0].nelement < g->n)) {
        daoError("applyGain: float SHMs of matching sizes expected (%s, %s, %s)\n", in->shm->name,
                 gain->name, out->shm->name);
        free(g);
        return NULL;
    }
    cudaMalloc(&g->dGain, g->n * sizeof(float));
    g->c0 = cnt0(gain) - 1;
    return &g->base;
}

/* ======================================================== pixel stages */
/* ------------------------------------------------------- pixelCalibrate */
/* daoPixelCalibrate: out = (raw - bg) * ff over the output's size */
struct PixCalStage {
    daoGpuStage base;
    Param ff, bg;
    int n;
};

template <typename T>
__global__ void pixelCalibrateKernel(const T *raw, const float *bg, const float *ff, float *out, int n)
{
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    if (k < n)
        out[k] = ((float) raw[k] - bg[k]) * ff[k];
}

static int pixCalUpdate(daoGpuStage *b, cudaStream_t)
{
    PixCalStage *c = (PixCalStage *) b;
    int r1 = paramSync(&c->ff), r2 = paramSync(&c->bg);
    return r1 < 0 || r2 < 0 ? -1 : r1 | r2;
}

static int pixCalRun(daoGpuStage *b, cudaStream_t st)
{
    PixCalStage *c = (PixCalStage *) b;
    int blocks = (c->n + 255) / 256;
    switch (b->in->shm->md[0].atype) {
#define PC_CASE(ATYPE, T)                                                                           \
    case ATYPE:                                                                                     \
        pixelCalibrateKernel<T><<<blocks, 256, 0, st>>>((const T *) b->in->d, (const float *) c->bg.d, \
                                                        (const float *) c->ff.d, (float *) b->out->d, c->n); \
        break;
    DAO_FOR_EACH_PIXEL_TYPE(PC_CASE)
#undef PC_CASE
    default:
        daoError("%s: unsupported raw image type %d\n", b->name, (int) b->in->shm->md[0].atype);
        return 0;
    }
    return cudaGetLastError() == cudaSuccess;
}

static void pixCalDestroy(daoGpuStage *b)
{
    PixCalStage *c = (PixCalStage *) b;
    paramFree(&c->ff);
    paramFree(&c->bg);
    free(c);
}

extern "C" daoGpuStage *daoGpuPixelCalibrateCreate(daoGpuPort *raw, IMAGE *ff, IMAGE *bg, daoGpuPort *out)
{
    long n = nelem(out);
    if (!isFloat(out) || !isFloat(ff) || !isFloat(bg) || nelem(raw) < n || nelem(ff) < n || nelem(bg) < n) {
        daoError("pixelCalibrate: float %s, %s, %s at least as large as %s expected\n", out->shm->name,
                 ff->name, bg->name, out->shm->name);
        return NULL;
    }
    PixCalStage *c = (PixCalStage *) calloc(1, sizeof *c);
    c->base.name = "pixelCalibrate";
    c->base.in = raw;
    c->base.out = out;
    c->base.cnt2FromIn = 1;
    c->base.update = pixCalUpdate;
    c->base.run = pixCalRun;
    c->base.destroy = pixCalDestroy;
    c->n = (int) n;
    if (!paramInit(&c->ff, ff, n * sizeof(float)) || !paramInit(&c->bg, bg, n * sizeof(float))) {
        pixCalDestroy(&c->base);
        return NULL;
    }
    return &c->base;
}

/* ---------------------------------------------------------- pixelExtract */
/* daoPixelExtract: out[k] = in[lut[k]], lut = positions of mask == 1 */
struct ExtractStage {
    daoGpuStage base;
    IMAGE *mask;
    uint64_t c0mask;
    int n;
    int *dLut;
    size_t esize;
};

template <typename T>
__global__ void gatherKernel(const T *in, const int *lut, T *out, int n)
{
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    if (k < n)
        out[k] = in[lut[k]];
}

/* (Re)build a mask LUT on the GPU; the output must hold every extracted pixel. */
static int lutSync(IMAGE *mask, uint64_t *c0, long imSize, int **dLut, int *n, const daoGpuPort *out,
                   const char *name)
{
    if (*c0 == cnt0(mask))
        return 0;
    *c0 = cnt0(mask);
    int nn;
    int *lut = maskLut(mask, imSize, &nn);
    if (nn > nelem(out)) {
        daoError("%s: %s selects %d pixels, %s holds %ld\n", name, mask->name, nn, out->shm->name, nelem(out));
        free(lut);
        return -1;
    }
    cudaFree(*dLut);
    *dLut = NULL;
    if (cudaMalloc(dLut, (nn > 0 ? nn : 1) * sizeof(int)) != cudaSuccess
        || cudaMemcpy(*dLut, lut, nn * sizeof(int), cudaMemcpyHostToDevice) != cudaSuccess) {
        daoError("%s: cannot upload %s\n", name, mask->name);
        free(lut);
        return -1;
    }
    free(lut);
    *n = nn;
    daoInfo("%s: %d pixels selected by %s\n", name, nn, mask->name);
    return 1;
}

static int extractUpdate(daoGpuStage *b, cudaStream_t)
{
    ExtractStage *e = (ExtractStage *) b;
    return lutSync(e->mask, &e->c0mask, nelem(b->in), &e->dLut, &e->n, b->out, b->name);
}

static int extractRun(daoGpuStage *b, cudaStream_t st)
{
    ExtractStage *e = (ExtractStage *) b;
    int blocks = (e->n + 255) / 256;
    if (e->n == 0)
        return 1;
    switch (e->esize) {
    case 1: gatherKernel<uint8_t><<<blocks, 256, 0, st>>>((const uint8_t *) b->in->d, e->dLut, (uint8_t *) b->out->d, e->n); break;
    case 2: gatherKernel<uint16_t><<<blocks, 256, 0, st>>>((const uint16_t *) b->in->d, e->dLut, (uint16_t *) b->out->d, e->n); break;
    case 4: gatherKernel<uint32_t><<<blocks, 256, 0, st>>>((const uint32_t *) b->in->d, e->dLut, (uint32_t *) b->out->d, e->n); break;
    default: gatherKernel<uint64_t><<<blocks, 256, 0, st>>>((const uint64_t *) b->in->d, e->dLut, (uint64_t *) b->out->d, e->n); break;
    }
    return cudaGetLastError() == cudaSuccess;
}

static void extractDestroy(daoGpuStage *b)
{
    ExtractStage *e = (ExtractStage *) b;
    cudaFree(e->dLut);
    free(e);
}

extern "C" daoGpuStage *daoGpuPixelExtractCreate(daoGpuPort *in, IMAGE *mask, daoGpuPort *out)
{
    size_t es = elemSize(in->shm->md[0].atype);
    if (out->shm->md[0].atype != in->shm->md[0].atype || mask->md[0].atype != _DATATYPE_UINT32
        || nelem(mask) < nelem(in) || es == 0 || es > 8) {
        daoError("pixelExtract: %s and %s of the same type and a uint32 mask %s expected\n", in->shm->name,
                 out->shm->name, mask->name);
        return NULL;
    }
    ExtractStage *e = (ExtractStage *) calloc(1, sizeof *e);
    e->base.name = "pixelExtract";
    e->base.in = in;
    e->base.out = out;
    e->base.cnt2FromIn = 1;
    e->base.update = extractUpdate;
    e->base.run = extractRun;
    e->base.destroy = extractDestroy;
    e->mask = mask;
    e->c0mask = cnt0(mask) - 1;
    e->esize = es;
    return &e->base;
}

/* ------------------------------------------------ pixelSubstractExtract* */
/* mode SUB:        out = (in - sub)[lut]            (/ norm[0] with a norm SHM)
 * mode NORM:       out = (in - sub)[lut] / sum(max(in - sub, 1))  (sum in double)
 * mode NORM_IMAGE: out = in[lut] / sum(in[lut]) - sub[lut]         (sum in double) */
enum { SUBX_SUB, SUBX_NORM, SUBX_NORM_IMAGE };

struct SubExtractStage {
    daoGpuStage base;
    int mode;
    IMAGE *mask;
    uint64_t c0mask;
    Param sub, norm;
    int hasNorm;
    int n;
    int *dLut;
    double *dSum;
};

template <typename T>
__global__ void subExtractKernel(const T *in, const T *sub, const int *lut, const T *norm, T *out, int n)
{
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    if (k < n) {
        int i = lut[k];
        T v = in[i] - sub[i];
        out[k] = norm ? (T) (v / norm[0]) : v;
    }
}

__global__ void subExtractSumKernel(const float *in, const float *sub, const int *lut, double *sum, int n,
                                    int mode)
{
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    double v = 0.0;
    if (k < n) {
        int i = lut[k];
        if (mode == SUBX_NORM) {
            double pv = (double) (in[i] - sub[i]);
            v = pv < 1.0 ? 1.0 : pv;
        } else {
            v = (double) in[i];
        }
    }
    for (int o = 16; o > 0; o >>= 1)
        v += __shfl_down_sync(0xffffffffu, v, o);
    if ((threadIdx.x & 31) == 0)
        atomicAddDouble(sum, v);
}

__global__ void subExtractNormKernel(const float *in, const float *sub, const int *lut, const double *sum,
                                     float *out, int n, int mode)
{
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    double total = *sum;
    if (k < n && total != 0.0) {                     /* the CPU tools leave the output as is */
        int i = lut[k];
        float inv = (float) (1.0 / total);
        out[k] = mode == SUBX_NORM ? (in[i] - sub[i]) * inv : in[i] * inv - sub[i];
    }
}

static int subxUpdate(daoGpuStage *b, cudaStream_t)
{
    SubExtractStage *e = (SubExtractStage *) b;
    int r = lutSync(e->mask, &e->c0mask, nelem(b->in), &e->dLut, &e->n, b->out, b->name);
    int r1 = paramSync(&e->sub), r2 = e->hasNorm ? paramSync(&e->norm) : 0;
    return r < 0 || r1 < 0 || r2 < 0 ? -1 : r | r1 | r2;
}

static int subxRun(daoGpuStage *b, cudaStream_t st)
{
    SubExtractStage *e = (SubExtractStage *) b;
    int blocks = (e->n + 255) / 256;
    if (e->n == 0)
        return 1;
    if (e->mode != SUBX_SUB) {
        cudaMemsetAsync(e->dSum, 0, sizeof(double), st);
        subExtractSumKernel<<<blocks, 256, 0, st>>>((const float *) b->in->d, (const float *) e->sub.d, e->dLut,
                                                    e->dSum, e->n, e->mode);
        subExtractNormKernel<<<blocks, 256, 0, st>>>((const float *) b->in->d, (const float *) e->sub.d, e->dLut,
                                                     e->dSum, (float *) b->out->d, e->n, e->mode);
        return cudaGetLastError() == cudaSuccess;
    }
    uint8_t atype = b->in->shm->md[0].atype;
    /* like the CPU tool, the norm only applies to floating-point images */
    bool fp = atype == _DATATYPE_FLOAT || atype == _DATATYPE_DOUBLE;
    const void *norm = e->hasNorm && fp ? e->norm.d : NULL;
    switch (atype) {
#define SX_CASE(ATYPE, T)                                                                                   \
    case ATYPE:                                                                                             \
        subExtractKernel<T><<<blocks, 256, 0, st>>>((const T *) b->in->d, (const T *) e->sub.d, e->dLut,     \
                                                    (const T *) norm, (T *) b->out->d, e->n);               \
        break;
    DAO_FOR_EACH_PIXEL_TYPE(SX_CASE)
#undef SX_CASE
    default:
        daoError("%s: unsupported image type %d\n", b->name, (int) atype);
        return 0;
    }
    return cudaGetLastError() == cudaSuccess;
}

static void subxDestroy(daoGpuStage *b)
{
    SubExtractStage *e = (SubExtractStage *) b;
    cudaFree(e->dLut);
    cudaFree(e->dSum);
    paramFree(&e->sub);
    if (e->hasNorm)
        paramFree(&e->norm);
    free(e);
}

static daoGpuStage *subxCreate(const char *name, int mode, daoGpuPort *in, IMAGE *sub, IMAGE *mask, IMAGE *norm,
                               daoGpuPort *out)
{
    uint8_t atype = in->shm->md[0].atype;
    if (sub->md[0].atype != atype || out->shm->md[0].atype != atype || nelem(sub) < nelem(in)
        || mask->md[0].atype != _DATATYPE_UINT32 || nelem(mask) < nelem(in)
        || (mode != SUBX_SUB && atype != _DATATYPE_FLOAT) || (norm && norm->md[0].atype != atype)) {
        daoError("%s: %s, %s and %s of the same type%s and a uint32 mask %s expected\n", name, in->shm->name,
                 sub->name, out->shm->name, mode != SUBX_SUB ? " (float)" : "", mask->name);
        return NULL;
    }
    SubExtractStage *e = (SubExtractStage *) calloc(1, sizeof *e);
    e->base.name = name;
    e->base.in = in;
    e->base.out = out;
    e->base.cnt2FromIn = 1;
    e->base.update = subxUpdate;
    e->base.run = subxRun;
    e->base.destroy = subxDestroy;
    e->mode = mode;
    e->mask = mask;
    e->c0mask = cnt0(mask) - 1;
    e->hasNorm = norm != NULL;
    if (!paramInit(&e->sub, sub, nelem(in) * elemSize(atype))
        || (norm && !paramInit(&e->norm, norm, elemSize(atype)))
        || cudaMalloc(&e->dSum, sizeof(double)) != cudaSuccess) {
        daoError("%s: out of GPU memory\n", name);
        subxDestroy(&e->base);
        return NULL;
    }
    return &e->base;
}

extern "C" daoGpuStage *daoGpuPixelSubstractExtractCreate(daoGpuPort *in, IMAGE *sub, IMAGE *mask, IMAGE *norm,
                                                          daoGpuPort *out)
{
    return subxCreate("pixelSubstractExtract", SUBX_SUB, in, sub, mask, norm, out);
}

extern "C" daoGpuStage *daoGpuPixelSubstractExtractNormCreate(daoGpuPort *in, IMAGE *sub, IMAGE *mask,
                                                              daoGpuPort *out)
{
    return subxCreate("pixelSubstractExtractNorm", SUBX_NORM, in, sub, mask, NULL, out);
}

extern "C" daoGpuStage *daoGpuPixelSubstractExtractNormImageCreate(daoGpuPort *in, IMAGE *sub, IMAGE *mask,
                                                                   daoGpuPort *out)
{
    return subxCreate("pixelSubstractExtractNormImage", SUBX_NORM_IMAGE, in, sub, mask, NULL, out);
}

/* ------------------------------------------------------- descrambleOcam2 */
/* daoDescrambleOcam2: out[i] = little-endian uint16 at byte offset off[i] of the raw
 * frame. binning 1: off[i] from the LUT like the CPU tool; binning 2: the tool's
 * fixed 240x240 layout (two batches of 7200 samples, the rest zeroed). */
#define OCAM2_BATCH 7200
#define OCAM2_BIN_OFFSET 43200
#define OCAM2_BIN_ZERO_FROM 14400
#define OCAM2_BIN_SIZE (240 * 240)

struct DescrambleStage {
    daoGpuStage base;
    IMAGE *lut;
    uint64_t c0lut;
    int binning, n;
    int *dOff;
};

__global__ void descrambleKernel(const uint8_t *raw, const int *off, uint16_t *out, int nOff, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < nOff) {
        int o = off[i];
        out[i] = (uint16_t) ((raw[o + 1] << 8) | raw[o]);
    } else if (i < n) {
        out[i] = 0;
    }
}

static int descrambleUpdate(daoGpuStage *b, cudaStream_t)
{
    DescrambleStage *d = (DescrambleStage *) b;
    if (d->c0lut == cnt0(d->lut))
        return 0;
    d->c0lut = cnt0(d->lut);
    int imgWidth = b->in->shm->md[0].size[0];
    int nOff = d->binning == 2 ? 2 * OCAM2_BATCH : d->n;
    int *off = (int *) malloc(nOff * sizeof(int));
    const int32_t *lut = d->lut->array.SI32;
    for (int i = 0; i < nOff; i++) {
        if (d->binning == 2)          /* same samples as the CPU tool */
            off[i] = i < OCAM2_BATCH ? lut[i * 2] * 2 : lut[(i - OCAM2_BATCH) * 2 + OCAM2_BIN_OFFSET] * 2;
        else
            off[i] = lut[i] / (imgWidth / 2) * imgWidth + (lut[i] % (imgWidth / 2)) * 2;
        if (off[i] < 0 || off[i] + 1 >= nelem(b->in)) {
            daoError("%s: %s points outside %s\n", b->name, d->lut->name, b->in->shm->name);
            free(off);
            return -1;
        }
    }
    int r = cudaMemcpy(d->dOff, off, nOff * sizeof(int), cudaMemcpyHostToDevice) == cudaSuccess ? 1 : -1;
    free(off);
    return r;
}

static int descrambleRun(daoGpuStage *b, cudaStream_t st)
{
    DescrambleStage *d = (DescrambleStage *) b;
    int nOff = d->binning == 2 ? 2 * OCAM2_BATCH : d->n;
    int n = d->binning == 2 ? OCAM2_BIN_SIZE : d->n;
    /* binning 2 writes the two batches at [0, 7200) and [7200, 14400), then zeros */
    descrambleKernel<<<(n + 255) / 256, 256, 0, st>>>((const uint8_t *) b->in->d, d->dOff,
                                                       (uint16_t *) b->out->d, nOff, n);
    return cudaGetLastError() == cudaSuccess;
}

static void descrambleDestroy(daoGpuStage *b)
{
    DescrambleStage *d = (DescrambleStage *) b;
    cudaFree(d->dOff);
    free(d);
}

extern "C" daoGpuStage *daoGpuDescrambleOcam2Create(daoGpuPort *raw, IMAGE *lut, int binning, daoGpuPort *out)
{
    long n = (long) out->shm->md[0].size[0] * out->shm->md[0].size[1];
    long lutNeed = binning == 2 ? 2 * (OCAM2_BATCH - 1) + OCAM2_BIN_OFFSET + 1 : n;
    if (raw->shm->md[0].atype != _DATATYPE_UINT8 || out->shm->md[0].atype != _DATATYPE_UINT16
        || lut->md[0].atype != _DATATYPE_INT32 || nelem(lut) < lutNeed
        || (binning == 2 && n < OCAM2_BIN_SIZE) || (binning != 1 && binning != 2)) {
        daoError("descrambleOcam2: uint8 %s, uint16 %s, int32 %s (%ld values) and binning 1 or 2 expected\n",
                 raw->shm->name, out->shm->name, lut->name, lutNeed);
        return NULL;
    }
    DescrambleStage *d = (DescrambleStage *) calloc(1, sizeof *d);
    d->base.name = "descrambleOcam2";
    d->base.in = raw;
    d->base.out = out;
    d->base.update = descrambleUpdate;
    d->base.run = descrambleRun;
    d->base.destroy = descrambleDestroy;
    d->lut = lut;
    d->c0lut = cnt0(lut) - 1;
    d->binning = binning;
    d->n = (int) n;
    if (cudaMalloc(&d->dOff, (binning == 2 ? 2 * OCAM2_BATCH : n) * sizeof(int)) != cudaSuccess) {
        free(d);
        return NULL;
    }
    return &d->base;
}

/* ===================================================== SH centroiders */
/* One block per sub-aperture. Boxes are the CPU tools' closed intervals
 * [round(c) - size/2, round(c) + size/2]; pixel reads outside the image buffer
 * count as 0 (the CPU tools do not check). */
static __device__ __forceinline__ float pix(const float *img, long nImg, long idx)
{
    return idx >= 0 && idx < nImg ? img[idx] : 0.f;
}

/* ------------------------------------------ centroid (centre of gravity) */
enum { COG_ABSOLUTE, COG_RELATIVE, COG_RELATIVE_REF };

/* COG_ABSOLUTE (daoCentroidSpots):     pixel < thr -> 0;  out cx, cy, flux
 * COG_RELATIVE (daoCentroidSpotsRelative):  t = thr * max(0, box max),
 *     pixel < t -> 0 else pixel - t;  out cx, cy, raw flux, weight
 * COG_RELATIVE_REF: boxes at centre[], cx -= ref[] as well */
__global__ void cogKernel(const float *img, long nImg, int stride, const float *pos, const float *ref,
                          const float *thr, int box, int nSuba, int mode, float *out)
{
    int s = blockIdx.x;
    float px = pos[s], py = pos[nSuba + s];
    int half = box / 2, side = 2 * half + 1;
    int x1 = (int) roundf(px) - half, y1 = (int) roundf(py) - half;
    float threshold = thr[0];
    if (mode != COG_ABSOLUTE) {
        float m = 0.f;                               /* the CPU starts from 0 */
        for (int p = threadIdx.x; p < side * side; p += blockDim.x) {
            int x = x1 + p % side, y = y1 + p / side;
            m = fmaxf(m, pix(img, nImg, (long) y * stride + x));
        }
        threshold *= blockMax(m);
    }
    float flux = 0.f, den = 0.f, xn = 0.f, yn = 0.f;
    for (int p = threadIdx.x; p < side * side; p += blockDim.x) {
        int x = x1 + p % side, y = y1 + p / side;
        float v = pix(img, nImg, (long) y * stride + x);
        flux += v;
        if (v < threshold)
            v = 0.f;
        else if (mode != COG_ABSOLUTE)
            v -= threshold;
        den += v;
        xn += v * (float) x;
        yn += v * (float) y;
    }
    flux = blockSum(flux);
    den = blockSum(den);
    xn = blockSum(xn);
    yn = blockSum(yn);
    if (threadIdx.x == 0) {
        float cx = 0.f, cy = 0.f;
        if (den != 0.f) {
            cx = xn / den - px;
            cy = yn / den - py;
            if (mode == COG_RELATIVE_REF) {
                cx -= ref[s];
                cy -= ref[nSuba + s];
            }
        }
        out[s] = cx;
        out[nSuba + s] = cy;
        out[2 * nSuba + s] = mode == COG_ABSOLUTE ? den : flux;
        if (mode != COG_ABSOLUTE)
            out[3 * nSuba + s] = den;
    }
}

struct CogStage {
    daoGpuStage base;
    int mode, box, nSuba, stride;
    Param pos, ref, thr;
};

static int cogUpdate(daoGpuStage *b, cudaStream_t)
{
    CogStage *c = (CogStage *) b;
    int r1 = paramSync(&c->pos), r2 = c->mode == COG_RELATIVE_REF ? paramSync(&c->ref) : 0;
    int r3 = paramSync(&c->thr);
    return r1 < 0 || r2 < 0 || r3 < 0 ? -1 : r1 | r2 | r3;
}

static int cogRun(daoGpuStage *b, cudaStream_t st)
{
    CogStage *c = (CogStage *) b;
    cogKernel<<<c->nSuba, 256, 0, st>>>((const float *) b->in->d, nelem(b->in), c->stride,
                                        (const float *) c->pos.d, (const float *) c->ref.d,
                                        (const float *) c->thr.d, c->box, c->nSuba, c->mode, (float *) b->out->d);
    return cudaGetLastError() == cudaSuccess;
}

static void cogDestroy(daoGpuStage *b)
{
    CogStage *c = (CogStage *) b;
    paramFree(&c->pos);
    paramFree(&c->ref);
    paramFree(&c->thr);
    free(c);
}

static daoGpuStage *cogCreate(const char *name, int mode, daoGpuPort *in, IMAGE *pos, IMAGE *ref,
                              IMAGE *threshold, int box, int nSuba, daoGpuPort *out)
{
    int nOut = (mode == COG_ABSOLUTE ? 3 : 4) * nSuba;
    if (!isFloat(in) || !isFloat(out) || !isFloat(pos) || !isFloat(threshold) || (ref && !isFloat(ref))
        || nelem(pos) < 2 * nSuba || (ref && nelem(ref) < 2 * nSuba) || nelem(out) < nOut || box < 1
        || nSuba < 1) {
        daoError("%s: float SHMs expected, positions of 2 x %d values and an output of %d values\n", name,
                 nSuba, nOut);
        return NULL;
    }
    CogStage *c = (CogStage *) calloc(1, sizeof *c);
    c->base.name = name;
    c->base.in = in;
    c->base.out = out;
    c->base.cnt2FromIn = 1;
    c->base.update = cogUpdate;
    c->base.run = cogRun;
    c->base.destroy = cogDestroy;
    c->mode = mode;
    c->box = box;
    c->nSuba = nSuba;
    /* image row length as each CPU tool reads it */
    c->stride = mode == COG_ABSOLUTE ? in->shm->md[0].size[0] : in->shm->md[0].size[1];
    if (!paramInit(&c->pos, pos, 2 * nSuba * sizeof(float)) || !paramInit(&c->thr, threshold, sizeof(float))
        || (ref && !paramInit(&c->ref, ref, 2 * nSuba * sizeof(float)))) {
        cogDestroy(&c->base);
        return NULL;
    }
    daoInfo("%s: %d sub-apertures of %d px on %s -> %s\n", name, nSuba, box, in->shm->name, out->shm->name);
    return &c->base;
}

extern "C" daoGpuStage *daoGpuCentroidCreate(daoGpuPort *in, IMAGE *ref, IMAGE *threshold, int subaSize,
                                             int nbSuba, daoGpuPort *out)
{
    return cogCreate("centroid", COG_ABSOLUTE, in, ref, NULL, threshold, subaSize, nbSuba, out);
}

extern "C" daoGpuStage *daoGpuCentroidRelativeCreate(daoGpuPort *in, IMAGE *ref, IMAGE *threshold,
                                                     int subaSize, int nbSuba, daoGpuPort *out)
{
    return cogCreate("centroidRelative", COG_RELATIVE, in, ref, NULL, threshold, subaSize, nbSuba, out);
}

extern "C" daoGpuStage *daoGpuCentroidRelativeRefCreate(daoGpuPort *in, IMAGE *subApCentre, IMAGE *ref,
                                                        IMAGE *threshold, int subaSize, int nbSuba,
                                                        daoGpuPort *out)
{
    return cogCreate("centroidRelativeRef", COG_RELATIVE_REF, in, subApCentre, ref, threshold, subaSize,
                     nbSuba, out);
}

/* ----------------------------------------------------------- correlation */
/* Windowed (daoCentroidSpotsCorrelation): every shift in [-R, R]^2 is computed,
 * then the CPU's coarse-to-fine choice and parabolic refinement are replayed on
 * those values. Periodic (daoCentroidSpotsCorrelationFFT): the circular
 * correlation over the whole sub-aperture, computed directly (the FFT's result,
 * up to rounding), then the same peak search and refinement as the CPU. */
#define CORR_COARSE_STRIDE 2                         /* DAO_CENTROID_CORR_COARSE_STRIDE */

__global__ void corrWindowKernel(const float *img, long nImg, int stride, const float *pos,
                                 const float *refImage, const float *thr, int box, int nSuba, int R, float *out)
{
    extern __shared__ float sh[];
    int s = blockIdx.x, W = box + 2 * R, ns = 2 * R + 1;
    float *obs = sh, *ref = obs + W * W, *surf = ref + box * box;
    int x0 = (int) roundf(pos[s]) - box / 2, y0 = (int) roundf(pos[nSuba + s]) - box / 2;
    float threshold = thr[0];
    for (int p = threadIdx.x; p < W * W; p += blockDim.x) {
        float v = pix(img, nImg, (long) (y0 - R + p / W) * stride + (x0 - R + p % W));
        obs[p] = v < threshold ? 0.f : v;
    }
    const float *subRef = refImage + (size_t) s * box * box;
    for (int p = threadIdx.x; p < box * box; p += blockDim.x)
        ref[p] = subRef[p];
    __syncthreads();
    for (int t = threadIdx.x; t < ns * ns; t += blockDim.x) {
        int dx = t % ns, dy = t / ns;                /* offsets into obs: shift + R */
        float sum = 0.f;
        for (int i = 0; i < box; i++)
            for (int j = 0; j < box; j++)
                sum += obs[(i + dy) * W + j + dx] * ref[i * box + j];
        surf[t] = sum;
    }
    __syncthreads();
    if (threadIdx.x != 0)
        return;
#define SURF(dx, dy) surf[((dy) + R) * ns + (dx) + R]
    float peak = -INFINITY;
    int bx = 0, by = 0;
    if (R > CORR_COARSE_STRIDE) {
        for (int dy = -R; dy <= R; dy += CORR_COARSE_STRIDE)
            for (int dx = -R; dx <= R; dx += CORR_COARSE_STRIDE)
                if (SURF(dx, dy) > peak) { peak = SURF(dx, dy); bx = dx; by = dy; }
        int fy0 = max(by - CORR_COARSE_STRIDE, -R), fy1 = min(by + CORR_COARSE_STRIDE, R);
        int fx0 = max(bx - CORR_COARSE_STRIDE, -R), fx1 = min(bx + CORR_COARSE_STRIDE, R);
        for (int dy = fy0; dy <= fy1; dy++)
            for (int dx = fx0; dx <= fx1; dx++)
                if (SURF(dx, dy) > peak) { peak = SURF(dx, dy); bx = dx; by = dy; }
    } else {
        for (int dy = -R; dy <= R; dy++)
            for (int dx = -R; dx <= R; dx++)
                if (SURF(dx, dy) > peak) { peak = SURF(dx, dy); bx = dx; by = dy; }
    }
    float subx = 0.f, suby = 0.f;
    if (bx > -R && bx < R) {
        float l = SURF(bx - 1, by), r = SURF(bx + 1, by), d = l - 2.f * peak + r;
        if (fabsf(d) > 1e-12f)
            subx = 0.5f * (l - r) / d;
    }
    if (by > -R && by < R) {
        float l = SURF(bx, by - 1), r = SURF(bx, by + 1), d = l - 2.f * peak + r;
        if (fabsf(d) > 1e-12f)
            suby = 0.5f * (l - r) / d;
    }
#undef SURF
    out[s] = (float) bx + subx;
    out[nSuba + s] = (float) by + suby;
    out[2 * nSuba + s] = peak;
}

__global__ void corrPeriodicKernel(const float *img, long nImg, int stride, const float *pos,
                                   const float *refImage, const float *thr, int box, int nSuba, float *out)
{
    extern __shared__ float sh[];
    int s = blockIdx.x, N = box * box, half = box / 2, W = 2 * box;
    float *obs = sh, *ref = obs + 4 * N, *surf = ref + N;   /* obs tiled 2 x 2: no modulo below */
    int x0 = (int) roundf(pos[s]) - half, y0 = (int) roundf(pos[nSuba + s]) - half;
    float threshold = thr[0];
    for (int p = threadIdx.x; p < N; p += blockDim.x) {
        int r = p / box, c = p % box;
        float v = pix(img, nImg, (long) (y0 + r) * stride + x0 + c);
        v = v < threshold ? 0.f : v;
        obs[r * W + c] = obs[r * W + c + box] = obs[(r + box) * W + c] = obs[(r + box) * W + c + box] = v;
        ref[p] = refImage[(size_t) s * N + p];
    }
    __syncthreads();
    for (int k = threadIdx.x; k < N; k += blockDim.x) {       /* c[k] = sum_n ref[n] obs[n + k] */
        int kx = k % box, ky = k / box;
        float sum = 0.f;
        for (int i = 0; i < box; i++) {
            const float *o = obs + (i + ky) * W + kx, *r = ref + i * box;
            for (int j = 0; j < box; j++)
                sum += r[j] * o[j];
        }
        surf[k] = sum;
    }
    __syncthreads();
    if (threadIdx.x != 0)
        return;
    float peak = -INFINITY;
    int bkx = 0, bky = 0;
    for (int ky = 0; ky < box; ky++) {
        int dy = ky <= half ? ky : ky - box;
        if (dy <= -half || dy >= half)
            continue;
        for (int kx = 0; kx < box; kx++) {
            int dx = kx <= half ? kx : kx - box;
            if (dx <= -half || dx >= half)
                continue;
            if (surf[ky * box + kx] > peak) { peak = surf[ky * box + kx]; bkx = kx; bky = ky; }
        }
    }
    float cLx = surf[bky * box + (bkx - 1 + box) % box], cRx = surf[bky * box + (bkx + 1) % box];
    float cLy = surf[((bky - 1 + box) % box) * box + bkx], cRy = surf[((bky + 1) % box) * box + bkx];
    /* the CPU compares the unnormalised FFT values (N times these) with 1e-12 */
    float subx = 0.f, suby = 0.f, dX = cLx - 2.f * peak + cRx, dY = cLy - 2.f * peak + cRy;
    if (fabsf(dX * N) > 1e-12f)
        subx = 0.5f * (cLx - cRx) / dX;
    if (fabsf(dY * N) > 1e-12f)
        suby = 0.5f * (cLy - cRy) / dY;
    out[s] = (float) (bkx <= half ? bkx : bkx - box) + subx;
    out[nSuba + s] = (float) (bky <= half ? bky : bky - box) + suby;
    out[2 * nSuba + s] = peak;
}

/* daoCentroidSpotsUpdateReference with threshold 0 (what both tools pass):
 * ref = (1 - alpha) ref + alpha obs, obs taken at the rounded centroid shift
 * clamped to +-box/4 */
__global__ void corrUpdateRefKernel(const float *img, long nImg, int stride, const float *pos, const float *cent,
                                    int box, int nSuba, float alpha, float *refImage)
{
    int s = blockIdx.x, maxShift = box / 4;
    int sx = min(max((int) roundf(cent[s]), -maxShift), maxShift);
    int sy = min(max((int) roundf(cent[nSuba + s]), -maxShift), maxShift);
    int x0 = (int) roundf(pos[s]) - box / 2 + sx, y0 = (int) roundf(pos[nSuba + s]) - box / 2 + sy;
    float *r = refImage + (size_t) s * box * box;
    for (int p = threadIdx.x; p < box * box; p += blockDim.x) {
        float v = pix(img, nImg, (long) (y0 + p / box) * stride + x0 + p % box);
        if (v < 0.f)
            v = 0.f;
        r[p] = (1.f - alpha) * r[p] + alpha * v;
    }
}

struct CorrStage {
    daoGpuStage base;
    int periodic, box, nSuba, R, stride;
    float alpha;
    size_t shmem;
    Param pos, refImage, thr;
};

static int corrUpdate(daoGpuStage *b, cudaStream_t)
{
    CorrStage *c = (CorrStage *) b;
    int r1 = paramSync(&c->pos), r2 = paramSync(&c->refImage), r3 = paramSync(&c->thr);
    if (r2 > 0)
        daoInfo("%s: reference image %s (re)loaded\n", b->name, c->refImage.im->name);
    return r1 < 0 || r2 < 0 || r3 < 0 ? -1 : r1 | r2 | r3;
}

static int corrRun(daoGpuStage *b, cudaStream_t st)
{
    CorrStage *c = (CorrStage *) b;
    if (c->periodic)
        corrPeriodicKernel<<<c->nSuba, 256, c->shmem, st>>>((const float *) b->in->d, nelem(b->in), c->stride,
                                                            (const float *) c->pos.d, (const float *) c->refImage.d,
                                                            (const float *) c->thr.d, c->box, c->nSuba,
                                                            (float *) b->out->d);
    else
        corrWindowKernel<<<c->nSuba, 256, c->shmem, st>>>((const float *) b->in->d, nelem(b->in), c->stride,
                                                          (const float *) c->pos.d, (const float *) c->refImage.d,
                                                          (const float *) c->thr.d, c->box, c->nSuba, c->R,
                                                          (float *) b->out->d);
    return cudaGetLastError() == cudaSuccess;
}

/* after publishing: blend this frame into the reference and write it back to its SHM */
static int corrPost(daoGpuStage *b, cudaStream_t st)
{
    CorrStage *c = (CorrStage *) b;
    if (c->alpha <= 0.f)
        return 1;
    IMAGE *im = c->refImage.im;
    corrUpdateRefKernel<<<c->nSuba, 256, 0, st>>>((const float *) b->in->d, nelem(b->in), c->stride,
                                                  (const float *) c->pos.d, (const float *) b->out->d, c->box,
                                                  c->nSuba, c->alpha, (float *) c->refImage.d);
    cudaMemcpyAsync(im->array.V, c->refImage.d, c->refImage.bytes, cudaMemcpyDeviceToHost, st);
    if (cudaStreamSynchronize(st) != cudaSuccess) {
        daoError("%s: reference update failed: %s\n", b->name, cudaGetErrorString(cudaGetLastError()));
        return 0;
    }
    daoShmSetDataPartFinalize(im);
    c->refImage.c0 = cnt0(im);                       /* our own write: nothing to reload */
    return 1;
}

static void corrDestroy(daoGpuStage *b)
{
    CorrStage *c = (CorrStage *) b;
    paramFree(&c->pos);
    paramFree(&c->refImage);
    paramFree(&c->thr);
    if (c->alpha > 0.f) {
        cudaHostUnregister(c->refImage.im->array.V);
        cudaGetLastError();
    }
    free(c);
}

static daoGpuStage *corrCreate(const char *name, int periodic, daoGpuPort *in, IMAGE *pos, IMAGE *refImage,
                               IMAGE *threshold, int box, int nSuba, int R, float alpha, daoGpuPort *out)
{
    int dev;
    cudaDeviceProp prop;
    cudaGetDevice(&dev);
    cudaGetDeviceProperties(&prop, dev);
    size_t shmem = periodic ? 6 * (size_t) box * box * sizeof(float)
                            : ((size_t) (box + 2 * R) * (box + 2 * R) + box * box + (2 * R + 1) * (2 * R + 1))
                                  * sizeof(float);
    if (!isFloat(in) || !isFloat(out) || !isFloat(pos) || !isFloat(refImage) || !isFloat(threshold)
        || nelem(pos) < 2 * nSuba || nelem(refImage) < (long) nSuba * box * box || nelem(out) < 3 * nSuba
        || box < 2 || nSuba < 1) {
        daoError("%s: float SHMs expected: positions 2 x %d, reference image %d x %d x %d, output 3 x %d\n", name,
                 nSuba, nSuba, box, box, nSuba);
        return NULL;
    }
    if (!periodic && (R < 0 || R > 8)) {             /* DAO_CENTROID_CORR_MAX_SEARCH_RANGE */
        daoError("%s: searchRange %d out of range [0..8]\n", name, R);
        return NULL;
    }
    if (shmem > prop.sharedMemPerBlock) {
        daoError("%s: sub-apertures of %d px need %zu bytes of shared memory, the GPU has %zu\n", name, box,
                 shmem, (size_t) prop.sharedMemPerBlock);
        return NULL;
    }
    CorrStage *c = (CorrStage *) calloc(1, sizeof *c);
    c->base.name = name;
    c->base.in = in;
    c->base.out = out;
    c->base.cnt2FromIn = 1;
    c->base.update = corrUpdate;
    c->base.run = corrRun;
    c->base.post = corrPost;
    c->base.destroy = corrDestroy;
    c->periodic = periodic;
    c->box = box;
    c->nSuba = nSuba;
    c->R = R;
    c->alpha = alpha;
    c->shmem = shmem;
    c->stride = in->shm->md[0].size[1];
    if (!paramInit(&c->pos, pos, 2 * nSuba * sizeof(float))
        || !paramInit(&c->refImage, refImage, (size_t) nSuba * box * box * sizeof(float))
        || !paramInit(&c->thr, threshold, sizeof(float))) {
        corrDestroy(&c->base);
        return NULL;
    }
    if (alpha > 0.f && cudaHostRegister(refImage->array.V, c->refImage.bytes, cudaHostRegisterPortable) != cudaSuccess)
        cudaGetLastError();                          /* fine: the copy back is then synchronous */
    daoInfo("%s: %d sub-apertures of %d px%s on %s -> %s\n", name, nSuba, box,
            alpha > 0.f ? ", reference following the spots" : "", in->shm->name, out->shm->name);
    return &c->base;
}

extern "C" daoGpuStage *daoGpuCentroidCorrelationCreate(daoGpuPort *in, IMAGE *subApCentre, IMAGE *refImage,
                                                        IMAGE *threshold, int subaSize, int nbSuba,
                                                        int searchRange, float alpha, daoGpuPort *out)
{
    return corrCreate("centroidCorrelation", 0, in, subApCentre, refImage, threshold, subaSize, nbSuba,
                      searchRange, alpha, out);
}

extern "C" daoGpuStage *daoGpuCentroidCorrelationFFTCreate(daoGpuPort *in, IMAGE *subApCentre, IMAGE *refImage,
                                                           IMAGE *threshold, int subaSize, int nbSuba, float alpha,
                                                           daoGpuPort *out)
{
    return corrCreate("centroidCorrelationFFT", 1, in, subApCentre, refImage, threshold, subaSize, nbSuba, 0,
                      alpha, out);
}
