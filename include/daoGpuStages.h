/**
 * @file    daoGpuStages.h
 * @brief   GPU processing stages working on dao SHMs (libdaoToolsGpu).
 *
 * A stage is one real-time function (calibration, matrix-vector multiply,
 * gain, ...) implemented as CUDA kernels. Its data SHMs are "ports": a port
 * gives the stage a device pointer to read or write. For a GPU SHM
 * (daoShmCreateGpu) it is the SHM's own payload; for a host SHM it is a
 * device buffer, and the caller copies the SHM in or out around the stage
 * (daoGpuPortUpload / daoGpuPortDownload).
 *
 * Parameter SHMs (flat, background, matrix, gain, ...) stay host SHMs:
 * daoGpuStageUpdate() uploads them when their cnt0 changed, off the
 * critical path.
 *
 * A stage is used by daoGpuPipeline (several stages in one process, one CUDA
 * graph per frame); everything a stage enqueues in daoGpuStageRun can be
 * captured in a CUDA graph. Stages can also be written outside daoTools and
 * loaded as plugins (daoGpuStageCreate, daoGpuRegisterStage below).
 */
#ifndef DAO_GPU_STAGES_H
#define DAO_GPU_STAGES_H

#include <stdlib.h>
#include <cuda_runtime.h>
#include "dao.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ ports */
typedef struct {
    IMAGE  *shm;
    void   *d;            /* device data: shm->d_array, the mapped host SHM, or a buffer */
    size_t  bytes;
    int     onGpu;        /* the SHM is a GPU SHM: no copy needed                        */
    int     mirror;       /* GPU SHM with DAO_GPU_MIRROR                                 */
    int     mapped;       /* host SHM mapped into the GPU (zero copy): kernels read and
                             write it over PCIe, no copy needed                          */
} daoGpuPort;

/* Open a port on an opened SHM. A host SHM is mapped into the GPU when mapHost
 * is set (kernels then fetch only what they use), else copied through a device
 * buffer. */
int  daoGpuPortInit(daoGpuPort *p, IMAGE *shm, int mapHost);
void daoGpuPortFree(daoGpuPort *p);
/* host SHM -> device buffer (no-op for a GPU SHM or a mapped host SHM) */
void daoGpuPortUpload(daoGpuPort *p, cudaStream_t s);
/* device data -> host SHM, or -> the /tmp mirror of a mirrored GPU SHM */
void daoGpuPortDownload(daoGpuPort *p, cudaStream_t s);

/* ----------------------------------------------------------------- stages */
/* Each stage reproduces the daoTools app it is named after, on float data (the
 * real-time case); out.cnt2 = in.cnt2 where the app does it. Arguments that are
 * IMAGE * are parameter SHMs. */
typedef struct daoGpuStage daoGpuStage;

/* ---- pixel processing */
/* daoPixelCalibrate: out = (raw - bg) * ff, any raw type, float out */
daoGpuStage *daoGpuPixelCalibrateCreate(daoGpuPort *raw, IMAGE *ff, IMAGE *bg, daoGpuPort *out);
/* daoCalIntensityNorm: calibrate (bg, ff), extract the valid pixels, normalise
 * by their sum and subtract the self-normalised reference. */
daoGpuStage *daoGpuCalIntensityNormCreate(daoGpuPort *raw, IMAGE *ff, IMAGE *bg, IMAGE *ref,
                                          IMAGE *validPix, IMAGE *illumPix, daoGpuPort *out);
/* daoCalIntensity: calibrate, extract the valid pixels, out = (cal - ref) / sum * illum */
daoGpuStage *daoGpuCalIntensityCreate(daoGpuPort *raw, IMAGE *ff, IMAGE *bg, IMAGE *ref,
                                      IMAGE *validPix, IMAGE *illumPix, daoGpuPort *out);
/* daoPixelExtract: out = in[mask == 1], any type (out of the same type) */
daoGpuStage *daoGpuPixelExtractCreate(daoGpuPort *in, IMAGE *mask, daoGpuPort *out);
/* daoPixelSubstractExtract: out = (in - sub)[mask == 1], divided by norm[0] when
 * norm is not NULL (the app's -n) */
daoGpuStage *daoGpuPixelSubstractExtractCreate(daoGpuPort *in, IMAGE *sub, IMAGE *mask, IMAGE *norm,
                                               daoGpuPort *out);
/* daoPixelSubstractExtractNorm: out = (in - sub)[mask] / sum(max(in - sub, 1)) */
daoGpuStage *daoGpuPixelSubstractExtractNormCreate(daoGpuPort *in, IMAGE *sub, IMAGE *mask, daoGpuPort *out);
/* daoPixelSubstractExtractNormImage: out = in[mask] / sum(in[mask]) - sub[mask] */
daoGpuStage *daoGpuPixelSubstractExtractNormImageCreate(daoGpuPort *in, IMAGE *sub, IMAGE *mask,
                                                        daoGpuPort *out);
/* daoDescrambleOcam2: OCAM2 raw bytes -> uint16 image through the LUT (binning 1 or 2) */
daoGpuStage *daoGpuDescrambleOcam2Create(daoGpuPort *raw, IMAGE *lut, int binning, daoGpuPort *out);

/* ---- Shack-Hartmann centroiders (ref: 2*nSuba positions, x then y; threshold: 1 value) */
/* daoComputeCentroid: centre of gravity, absolute threshold. out: cx, cy, flux */
daoGpuStage *daoGpuCentroidCreate(daoGpuPort *in, IMAGE *ref, IMAGE *threshold, int subaSize, int nbSuba,
                                  daoGpuPort *out);
/* daoComputeCentroidRelative: threshold relative to the spot maximum. out: cx, cy, flux, weight */
daoGpuStage *daoGpuCentroidRelativeCreate(daoGpuPort *in, IMAGE *ref, IMAGE *threshold, int subaSize,
                                          int nbSuba, daoGpuPort *out);
/* daoComputeCentroidRelativeRef: same, boxes centred on subApCentre, minus ref */
daoGpuStage *daoGpuCentroidRelativeRefCreate(daoGpuPort *in, IMAGE *subApCentre, IMAGE *ref,
                                             IMAGE *threshold, int subaSize, int nbSuba, daoGpuPort *out);
/* daoComputeCentroidCorrelation: correlation with refImage (one subaSize^2 template per
 * sub-aperture, stacked) within +-searchRange. out: cx, cy, peak. alpha > 0: the
 * reference follows the spots (running average), written back to refImage after
 * each frame is published. */
daoGpuStage *daoGpuCentroidCorrelationCreate(daoGpuPort *in, IMAGE *subApCentre, IMAGE *refImage,
                                             IMAGE *threshold, int subaSize, int nbSuba, int searchRange,
                                             float alpha, daoGpuPort *out);
/* daoComputeCentroidCorrelationFFT: periodic correlation over the whole sub-aperture
 * (computed directly: same result as the FFT). out: cx, cy, peak. alpha as above. */
daoGpuStage *daoGpuCentroidCorrelationFFTCreate(daoGpuPort *in, IMAGE *subApCentre, IMAGE *refImage,
                                                IMAGE *threshold, int subaSize, int nbSuba, float alpha,
                                                daoGpuPort *out);

/* ---- vectors */
/* daoShmSlice: out[0, count) = in[offset, offset + count), any type (same as in) */
daoGpuStage *daoGpuSliceCreate(daoGpuPort *in, long offset, long count, daoGpuPort *out);
/* daoMvMGPU: out = matrix . in (matrix size[0] = outputs, size[1] = inputs; the
 * input may be longer: its first size[1] values are used) */
daoGpuStage *daoGpuMvmCreate(daoGpuPort *in, IMAGE *matrix, daoGpuPort *out);
/* daoApplyGain: out = in * gain[0], or in * gain[k] with modal */
daoGpuStage *daoGpuApplyGainCreate(daoGpuPort *in, IMAGE *gain, daoGpuPort *out, int modal);

const char *daoGpuStageName(const daoGpuStage *s);
daoGpuPort *daoGpuStageOutput(daoGpuStage *s);
/* cnt2 to give the output when published, from the stage's input (-1: keep) */
long long   daoGpuStageOutputCnt2(const daoGpuStage *s);
/* Upload changed parameters (not graph-captured). Returns 1 if something changed. */
int  daoGpuStageUpdate(daoGpuStage *s, cudaStream_t st);
/* Enqueue the stage's kernels on st (graph-capturable). */
int  daoGpuStageRun(daoGpuStage *s, cudaStream_t st);
/* Work after the frame is published, off the critical path (e.g. reference
 * update); returns once it is done. No-op for most stages. */
int  daoGpuStagePost(daoGpuStage *s, cudaStream_t st);
void daoGpuStageDestroy(daoGpuStage *s);
/* Whether the stage's output is published this frame (default: every frame). An
 * output not published is not copied back to its host SHM either: the SHM is
 * left untouched (the pipeline recaptures its graph when this changes). */
void daoGpuStageSetPublish(daoGpuStage *s, int publish);
int  daoGpuStagePublishes(const daoGpuStage *s);

/* ------------------------------------------------ stages written elsewhere */
/* A stage is its own functions and state. update() returns 0 (nothing
 * changed), 1 (parameters reloaded), DAO_GPU_RECAPTURE (the work run() enqueues
 * changed, e.g. new sizes: the runner captures its graph again) or < 0 (error:
 * the frame is skipped). run() returns 1 on success. post() is optional. */
#define DAO_GPU_RECAPTURE 2
typedef struct {
    int  (*update)(void *self, cudaStream_t st);
    int  (*run)(void *self, cudaStream_t st);
    int  (*post)(void *self, cudaStream_t st);
    void (*destroy)(void *self);
} daoGpuStageOps;

/* A stage from its functions; in, out: its data ports (out is published each
 * frame, with in's cnt2 if cnt2FromIn). ops is copied. */
daoGpuStage *daoGpuStageCreate(const char *name, daoGpuPort *in, daoGpuPort *out, int cnt2FromIn,
                               const daoGpuStageOps *ops, void *self);

/* Stage types from plugins. A plugin is a shared library listed under
 * "plugins:" in the daoGpuPipeline configuration; it exports
 *     void daoGpuPluginRegister(void);
 * which registers its stage types. A stage's factory gets its arguments from
 * the configuration (its keys, SHMs opened by the pipeline). */
typedef struct daoGpuStageArgs {
    const char *type;
    void *ctx;                                              /* the pipeline's */
    const char *(*value)(void *ctx, const char *key);       /* NULL: no such key */
    IMAGE *(*shm)(void *ctx, const char *key);              /* parameter SHM; NULL: absent or failed */
    daoGpuPort *(*port)(void *ctx, const char *key);        /* data SHM port; NULL: absent or failed */
} daoGpuStageArgs;

typedef daoGpuStage *(*daoGpuStageFactory)(const daoGpuStageArgs *args);
int daoGpuRegisterStage(const char *type, daoGpuStageFactory factory);
daoGpuStageFactory daoGpuFindStage(const char *type);

static inline const char *daoGpuArgValue(const daoGpuStageArgs *a, const char *key)
{
    return a->value(a->ctx, key);
}
static inline IMAGE *daoGpuArgShm(const daoGpuStageArgs *a, const char *key) { return a->shm(a->ctx, key); }
static inline daoGpuPort *daoGpuArgPort(const daoGpuStageArgs *a, const char *key)
{
    return a->port(a->ctx, key);
}
static inline double daoGpuArgDouble(const daoGpuStageArgs *a, const char *key, double dflt)
{
    const char *v = a->value(a->ctx, key);
    return v ? strtod(v, NULL) : dflt;
}
static inline long daoGpuArgLong(const daoGpuStageArgs *a, const char *key, long dflt)
{
    const char *v = a->value(a->ctx, key);
    return v ? strtol(v, NULL, 10) : dflt;
}

#ifdef __cplusplus
}
#endif
#endif /* DAO_GPU_STAGES_H */
