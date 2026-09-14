/*****************************************************************************
  DAO project
  FFT-based (periodic) correlation centroider -- see daoToolsCorrFFT.h
  s.cetre
 *****************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fftw3.h>

#include "daoTools.h"
#include "daoToolsCorrFFT.h"

/* ========================================================================
 * Single precision (float, fftw3f)
 * ======================================================================== */

struct daoCentroidCorrFFTCtx {
    int boxSize;
    int nSuba;
    int halfSpec;                  /* boxSize/2 + 1 */
    fftwf_plan planR2C;
    fftwf_plan planC2R;
    float* obsBuf;                 /* boxSize x boxSize scratch (fftw-aligned) */
    fftwf_complex* obsFreq;        /* boxSize x halfSpec scratch */
    fftwf_complex* prodFreq;       /* boxSize x halfSpec scratch (consumed by c2r) */
    float* corrBuf;                /* boxSize x boxSize scratch: periodic correlation surface */
    fftwf_complex* refFreq;        /* nSuba * boxSize * halfSpec, precomputed + conjugated */
    float* refReal;                /* nSuba * boxSize * boxSize: real-space mirror of refFreq,
                                     * kept so UpdateRef has something to blend into before
                                     * re-transforming (see daoCentroidSpotsCorrelationFFTUpdateRef) */
};

void daoCentroidSpotsCorrelationFFTFree(daoCentroidCorrFFTCtx* ctx) {
    if (!ctx) return;
    if (ctx->planR2C) fftwf_destroy_plan(ctx->planR2C);
    if (ctx->planC2R) fftwf_destroy_plan(ctx->planC2R);
    if (ctx->obsBuf)   fftwf_free(ctx->obsBuf);
    if (ctx->obsFreq)  fftwf_free(ctx->obsFreq);
    if (ctx->prodFreq) fftwf_free(ctx->prodFreq);
    if (ctx->corrBuf)  fftwf_free(ctx->corrBuf);
    if (ctx->refFreq)  fftwf_free(ctx->refFreq);
    if (ctx->refReal)  fftwf_free(ctx->refReal);
    free(ctx);
}

/* FFT + conjugate every subaperture's real-space reference (ctx->refReal)
 * into the cached spectrum (ctx->refFreq) used by the per-frame correlation.
 * Shared by Init (first load) and UpdateRef (after blending in new data). */
static void daoCentroidCorrFFTRefreshSpectra(daoCentroidCorrFFTCtx* ctx) {
    const int nSpec = ctx->boxSize * ctx->halfSpec;
    for (int s = 0; s < ctx->nSuba; ++s) {
        const float* subRef = ctx->refReal + (size_t)s * ctx->boxSize * ctx->boxSize;
        memcpy(ctx->obsBuf, subRef, (size_t)ctx->boxSize * ctx->boxSize * sizeof(float));
        fftwf_execute_dft_r2c(ctx->planR2C, ctx->obsBuf, ctx->obsFreq);

        fftwf_complex* dst = ctx->refFreq + (size_t)s * nSpec;
        for (int k = 0; k < nSpec; ++k) {
            dst[k][0] =  ctx->obsFreq[k][0];
            dst[k][1] = -ctx->obsFreq[k][1];   /* conjugate */
        }
    }
}

daoCentroidCorrFFTCtx* daoCentroidSpotsCorrelationFFTInit(int boxSize, int nSuba, const float* refImage) {
    if (boxSize <= 0 || nSuba <= 0 || refImage == NULL) {
        daoError("daoCentroidSpotsCorrelationFFTInit: invalid arguments\n");
        return NULL;
    }

    daoCentroidCorrFFTCtx* ctx = (daoCentroidCorrFFTCtx*)calloc(1, sizeof(daoCentroidCorrFFTCtx));
    if (!ctx) {
        daoError("daoCentroidSpotsCorrelationFFTInit: calloc failed\n");
        return NULL;
    }
    ctx->boxSize  = boxSize;
    ctx->nSuba    = nSuba;
    ctx->halfSpec = boxSize / 2 + 1;

    ctx->obsBuf   = (float*)fftwf_malloc((size_t)boxSize * boxSize * sizeof(float));
    ctx->corrBuf  = (float*)fftwf_malloc((size_t)boxSize * boxSize * sizeof(float));
    ctx->obsFreq  = (fftwf_complex*)fftwf_malloc((size_t)boxSize * ctx->halfSpec * sizeof(fftwf_complex));
    ctx->prodFreq = (fftwf_complex*)fftwf_malloc((size_t)boxSize * ctx->halfSpec * sizeof(fftwf_complex));
    ctx->refFreq  = (fftwf_complex*)fftwf_malloc((size_t)nSuba * boxSize * ctx->halfSpec * sizeof(fftwf_complex));
    ctx->refReal  = (float*)fftwf_malloc((size_t)nSuba * boxSize * boxSize * sizeof(float));

    if (!ctx->obsBuf || !ctx->corrBuf || !ctx->obsFreq || !ctx->prodFreq || !ctx->refFreq || !ctx->refReal) {
        daoError("daoCentroidSpotsCorrelationFFTInit: allocation failed\n");
        daoCentroidSpotsCorrelationFFTFree(ctx);
        return NULL;
    }
    memcpy(ctx->refReal, refImage, (size_t)nSuba * boxSize * boxSize * sizeof(float));

    /* Measured once here; reused every subaperture/frame via the new-array
     * execute interface (fftwf_execute_dft_r2c/c2r), which is safe as long
     * as every buffer passed to it is fftwf_malloc-aligned, as they are. */
    ctx->planR2C = fftwf_plan_dft_r2c_2d(boxSize, boxSize, ctx->obsBuf, ctx->obsFreq, FFTW_MEASURE);
    ctx->planC2R = fftwf_plan_dft_c2r_2d(boxSize, boxSize, ctx->prodFreq, ctx->corrBuf, FFTW_MEASURE);
    if (!ctx->planR2C || !ctx->planC2R) {
        daoError("daoCentroidSpotsCorrelationFFTInit: FFTW plan creation failed\n");
        daoCentroidSpotsCorrelationFFTFree(ctx);
        return NULL;
    }

    /* Precompute + cache every subaperture's reference spectrum, already
     * conjugated, so the per-frame path below is a plain complex multiply. */
    daoCentroidCorrFFTRefreshSpectra(ctx);

    return ctx;
}

int_fast8_t daoCentroidSpotsCorrelationFFT(daoCentroidCorrFFTCtx* ctx,
    const float* image, int imageSizeX, int imageSizeY,
    const float* ref, float threshold, float* cent) {
    (void)imageSizeY;
    daoTrace("\n");
    if (!ctx) {
        daoError("daoCentroidSpotsCorrelationFFT: NULL context\n");
        return DAO_ERROR;
    }

    const int boxSize  = ctx->boxSize;
    const int nSuba    = ctx->nSuba;
    const int halfSpec = ctx->halfSpec;
    const int half     = boxSize / 2;
    const int nSpec    = boxSize * halfSpec;

    const float* refX = ref;
    const float* refY = ref + nSuba;
    float* cxOut   = cent;
    float* cyOut   = cent + nSuba;
    float* peakOut = cent + 2 * nSuba;

    for (int s = 0; s < nSuba; ++s) {
        const int x0 = (int)roundf(refX[s]) - boxSize / 2;
        const int y0 = (int)roundf(refY[s]) - boxSize / 2;

        /* Extract + threshold the observed window (background suppression,
         * same convention as daoCentroidSpotsCorrelation). */
        for (int i = 0; i < boxSize; ++i) {
            const float* imRow = image + (size_t)(y0 + i) * imageSizeX + x0;
            float* dst = ctx->obsBuf + (size_t)i * boxSize;
            for (int j = 0; j < boxSize; ++j) {
                float pixel = imRow[j];
                dst[j] = (pixel < threshold) ? 0.0f : pixel;
            }
        }

        fftwf_execute_dft_r2c(ctx->planR2C, ctx->obsBuf, ctx->obsFreq);

        const fftwf_complex* subRefFreq = ctx->refFreq + (size_t)s * nSpec;
        for (int k = 0; k < nSpec; ++k) {
            float ar = subRefFreq[k][0], ai = subRefFreq[k][1];   /* conj(FFT(ref)), cached */
            float br = ctx->obsFreq[k][0], bi = ctx->obsFreq[k][1];
            ctx->prodFreq[k][0] = ar * br - ai * bi;
            ctx->prodFreq[k][1] = ar * bi + ai * br;
        }

        fftwf_execute_dft_c2r(ctx->planC2R, ctx->prodFreq, ctx->corrBuf);

        /* Peak search over the periodic surface, excluding the wrap
         * boundary (keeps a 1px margin so the parabola always has real
         * neighbours -- a shift landing there is clamped, same as the
         * windowed search does at +-searchRange). */
        float peak = -INFINITY;
        int bestKx = 0, bestKy = 0;
        for (int ky = 0; ky < boxSize; ++ky) {
            int dy = (ky <= half) ? ky : ky - boxSize;
            if (dy <= -half || dy >= half) continue;
            for (int kx = 0; kx < boxSize; ++kx) {
                int dx = (kx <= half) ? kx : kx - boxSize;
                if (dx <= -half || dx >= half) continue;
                float v = ctx->corrBuf[ky * boxSize + kx];
                if (v > peak) {
                    peak = v;
                    bestKx = kx;
                    bestKy = ky;
                }
            }
        }
        int bestDx = (bestKx <= half) ? bestKx : bestKx - boxSize;
        int bestDy = (bestKy <= half) ? bestKy : bestKy - boxSize;

        /* Sub-pixel refinement: 3-point parabolic fit, reading neighbours
         * directly from the already-computed surface (circular indices). */
        int kxm1 = (bestKx - 1 + boxSize) % boxSize;
        int kxp1 = (bestKx + 1) % boxSize;
        int kym1 = (bestKy - 1 + boxSize) % boxSize;
        int kyp1 = (bestKy + 1) % boxSize;

        float cLx = ctx->corrBuf[bestKy * boxSize + kxm1];
        float cRx = ctx->corrBuf[bestKy * boxSize + kxp1];
        float cLy = ctx->corrBuf[kym1 * boxSize + bestKx];
        float cRy = ctx->corrBuf[kyp1 * boxSize + bestKx];

        float subDx = 0.0f, subDy = 0.0f;
        float denomX = cLx - 2.0f * peak + cRx;
        if (fabsf(denomX) > 1e-12f) subDx = 0.5f * (cLx - cRx) / denomX;
        float denomY = cLy - 2.0f * peak + cRy;
        if (fabsf(denomY) > 1e-12f) subDy = 0.5f * (cLy - cRy) / denomY;

        cxOut[s]   = (float)bestDx + subDx;
        cyOut[s]   = (float)bestDy + subDy;
        peakOut[s] = peak / (float)(boxSize * boxSize);   /* undo FFTW's unnormalized scale */
    }

    return DAO_SUCCESS;
}

/**
 * @brief Blend this frame's centroid-aligned observation into the cached
 * reference (EMA), refresh the cached spectrum, and mirror the result out.
 *
 * Call this *after* publishing the centroids from the matching
 * daoCentroidSpotsCorrelationFFT call -- it is not on the real-time critical
 * path: that call already used the previous reference, so this only affects
 * the *next* frame. See daoCentroidSpotsUpdateReference (daoTools.h) for the
 * alignment/blending semantics, which this wraps; on top of that, this also
 * re-FFTs the updated reference so the next daoCentroidSpotsCorrelationFFT
 * call sees it, and mirrors the blended result into @p refImageShmOut (e.g.
 * the SHM the reference was originally loaded from) so it doesn't silently
 * drift out of sync with what any external tool (or a future restart) reads.
 *
 * @param[in]  ctx            Context from daoCentroidSpotsCorrelationFFTInit
 * @param[in]  image          Same observed image passed to daoCentroidSpotsCorrelationFFT
 * @param[in]  imageSizeX     Image width (pixels)
 * @param[in]  imageSizeY     Image height (pixels), unused (kept for API symmetry)
 * @param[in]  ref            Same subaperture reference positions
 * @param[in]  cent           This frame's centroid output (cx/cy read; see daoCentroidSpotsUpdateReference)
 * @param[in]  threshold      Same absolute threshold used for centroiding
 * @param[in]  alpha          EMA rate in (0, 1]; alpha <= 0 is a no-op (returns DAO_SUCCESS immediately)
 * @param[out] refImageShmOut Mirrors the blended reference (size `nSuba*boxSize*boxSize`),
 *                             e.g. pass the refImage SHM's own array so external
 *                             readers see the live reference too. May be NULL to skip mirroring.
 *
 * @return DAO_SUCCESS on success, DAO_ERROR if ctx is NULL
 */
int_fast8_t daoCentroidSpotsCorrelationFFTUpdateRef(daoCentroidCorrFFTCtx* ctx,
    const float* image, int imageSizeX, int imageSizeY,
    const float* ref, const float* cent, float threshold, float alpha,
    float* refImageShmOut) {
    if (!ctx) {
        daoError("daoCentroidSpotsCorrelationFFTUpdateRef: NULL context\n");
        return DAO_ERROR;
    }
    if (alpha <= 0.0f) return DAO_SUCCESS;

    daoCentroidSpotsUpdateReference(image, imageSizeX, imageSizeY, ref, cent,
                                     ctx->boxSize, ctx->nSuba, threshold, alpha, ctx->refReal);
    daoCentroidCorrFFTRefreshSpectra(ctx);

    if (refImageShmOut) {
        memcpy(refImageShmOut, ctx->refReal,
               (size_t)ctx->nSuba * ctx->boxSize * ctx->boxSize * sizeof(float));
    }

    return DAO_SUCCESS;
}

/* ========================================================================
 * Double precision (double, fftw3)
 * ======================================================================== */

struct daoCentroidCorrFFTDoubleCtx {
    int boxSize;
    int nSuba;
    int halfSpec;
    fftw_plan planR2C;
    fftw_plan planC2R;
    double* obsBuf;
    fftw_complex* obsFreq;
    fftw_complex* prodFreq;
    double* corrBuf;
    fftw_complex* refFreq;
    double* refReal;               /* nSuba * boxSize * boxSize: real-space mirror of refFreq,
                                     * kept so UpdateRef has something to blend into before
                                     * re-transforming (see daoCentroidSpotsCorrelationFFTUpdateRefDouble) */
};

void daoCentroidSpotsCorrelationFFTDoubleFree(daoCentroidCorrFFTDoubleCtx* ctx) {
    if (!ctx) return;
    if (ctx->planR2C) fftw_destroy_plan(ctx->planR2C);
    if (ctx->planC2R) fftw_destroy_plan(ctx->planC2R);
    if (ctx->obsBuf)   fftw_free(ctx->obsBuf);
    if (ctx->obsFreq)  fftw_free(ctx->obsFreq);
    if (ctx->prodFreq) fftw_free(ctx->prodFreq);
    if (ctx->corrBuf)  fftw_free(ctx->corrBuf);
    if (ctx->refFreq)  fftw_free(ctx->refFreq);
    if (ctx->refReal)  fftw_free(ctx->refReal);
    free(ctx);
}

/* Double-precision counterpart of daoCentroidCorrFFTRefreshSpectra. */
static void daoCentroidCorrFFTRefreshSpectraDouble(daoCentroidCorrFFTDoubleCtx* ctx) {
    const int nSpec = ctx->boxSize * ctx->halfSpec;
    for (int s = 0; s < ctx->nSuba; ++s) {
        const double* subRef = ctx->refReal + (size_t)s * ctx->boxSize * ctx->boxSize;
        memcpy(ctx->obsBuf, subRef, (size_t)ctx->boxSize * ctx->boxSize * sizeof(double));
        fftw_execute_dft_r2c(ctx->planR2C, ctx->obsBuf, ctx->obsFreq);

        fftw_complex* dst = ctx->refFreq + (size_t)s * nSpec;
        for (int k = 0; k < nSpec; ++k) {
            dst[k][0] =  ctx->obsFreq[k][0];
            dst[k][1] = -ctx->obsFreq[k][1];
        }
    }
}

daoCentroidCorrFFTDoubleCtx* daoCentroidSpotsCorrelationFFTDoubleInit(int boxSize, int nSuba, const double* refImage) {
    if (boxSize <= 0 || nSuba <= 0 || refImage == NULL) {
        daoError("daoCentroidSpotsCorrelationFFTDoubleInit: invalid arguments\n");
        return NULL;
    }

    daoCentroidCorrFFTDoubleCtx* ctx = (daoCentroidCorrFFTDoubleCtx*)calloc(1, sizeof(daoCentroidCorrFFTDoubleCtx));
    if (!ctx) {
        daoError("daoCentroidSpotsCorrelationFFTDoubleInit: calloc failed\n");
        return NULL;
    }
    ctx->boxSize  = boxSize;
    ctx->nSuba    = nSuba;
    ctx->halfSpec = boxSize / 2 + 1;

    ctx->obsBuf   = (double*)fftw_malloc((size_t)boxSize * boxSize * sizeof(double));
    ctx->corrBuf  = (double*)fftw_malloc((size_t)boxSize * boxSize * sizeof(double));
    ctx->obsFreq  = (fftw_complex*)fftw_malloc((size_t)boxSize * ctx->halfSpec * sizeof(fftw_complex));
    ctx->prodFreq = (fftw_complex*)fftw_malloc((size_t)boxSize * ctx->halfSpec * sizeof(fftw_complex));
    ctx->refFreq  = (fftw_complex*)fftw_malloc((size_t)nSuba * boxSize * ctx->halfSpec * sizeof(fftw_complex));
    ctx->refReal  = (double*)fftw_malloc((size_t)nSuba * boxSize * boxSize * sizeof(double));

    if (!ctx->obsBuf || !ctx->corrBuf || !ctx->obsFreq || !ctx->prodFreq || !ctx->refFreq || !ctx->refReal) {
        daoError("daoCentroidSpotsCorrelationFFTDoubleInit: allocation failed\n");
        daoCentroidSpotsCorrelationFFTDoubleFree(ctx);
        return NULL;
    }
    memcpy(ctx->refReal, refImage, (size_t)nSuba * boxSize * boxSize * sizeof(double));

    ctx->planR2C = fftw_plan_dft_r2c_2d(boxSize, boxSize, ctx->obsBuf, ctx->obsFreq, FFTW_MEASURE);
    ctx->planC2R = fftw_plan_dft_c2r_2d(boxSize, boxSize, ctx->prodFreq, ctx->corrBuf, FFTW_MEASURE);
    if (!ctx->planR2C || !ctx->planC2R) {
        daoError("daoCentroidSpotsCorrelationFFTDoubleInit: FFTW plan creation failed\n");
        daoCentroidSpotsCorrelationFFTDoubleFree(ctx);
        return NULL;
    }

    daoCentroidCorrFFTRefreshSpectraDouble(ctx);

    return ctx;
}

int_fast8_t daoCentroidSpotsCorrelationFFTDouble(daoCentroidCorrFFTDoubleCtx* ctx,
    const double* image, int imageSizeX, int imageSizeY,
    const double* ref, double threshold, double* cent) {
    (void)imageSizeY;
    daoTrace("\n");
    if (!ctx) {
        daoError("daoCentroidSpotsCorrelationFFTDouble: NULL context\n");
        return DAO_ERROR;
    }

    const int boxSize  = ctx->boxSize;
    const int nSuba    = ctx->nSuba;
    const int halfSpec = ctx->halfSpec;
    const int half     = boxSize / 2;
    const int nSpec    = boxSize * halfSpec;

    const double* refX = ref;
    const double* refY = ref + nSuba;
    double* cxOut   = cent;
    double* cyOut   = cent + nSuba;
    double* peakOut = cent + 2 * nSuba;

    for (int s = 0; s < nSuba; ++s) {
        const int x0 = (int)round(refX[s]) - boxSize / 2;
        const int y0 = (int)round(refY[s]) - boxSize / 2;

        for (int i = 0; i < boxSize; ++i) {
            const double* imRow = image + (size_t)(y0 + i) * imageSizeX + x0;
            double* dst = ctx->obsBuf + (size_t)i * boxSize;
            for (int j = 0; j < boxSize; ++j) {
                double pixel = imRow[j];
                dst[j] = (pixel < threshold) ? 0.0 : pixel;
            }
        }

        fftw_execute_dft_r2c(ctx->planR2C, ctx->obsBuf, ctx->obsFreq);

        const fftw_complex* subRefFreq = ctx->refFreq + (size_t)s * nSpec;
        for (int k = 0; k < nSpec; ++k) {
            double ar = subRefFreq[k][0], ai = subRefFreq[k][1];
            double br = ctx->obsFreq[k][0], bi = ctx->obsFreq[k][1];
            ctx->prodFreq[k][0] = ar * br - ai * bi;
            ctx->prodFreq[k][1] = ar * bi + ai * br;
        }

        fftw_execute_dft_c2r(ctx->planC2R, ctx->prodFreq, ctx->corrBuf);

        double peak = -INFINITY;
        int bestKx = 0, bestKy = 0;
        for (int ky = 0; ky < boxSize; ++ky) {
            int dy = (ky <= half) ? ky : ky - boxSize;
            if (dy <= -half || dy >= half) continue;
            for (int kx = 0; kx < boxSize; ++kx) {
                int dx = (kx <= half) ? kx : kx - boxSize;
                if (dx <= -half || dx >= half) continue;
                double v = ctx->corrBuf[ky * boxSize + kx];
                if (v > peak) {
                    peak = v;
                    bestKx = kx;
                    bestKy = ky;
                }
            }
        }
        int bestDx = (bestKx <= half) ? bestKx : bestKx - boxSize;
        int bestDy = (bestKy <= half) ? bestKy : bestKy - boxSize;

        int kxm1 = (bestKx - 1 + boxSize) % boxSize;
        int kxp1 = (bestKx + 1) % boxSize;
        int kym1 = (bestKy - 1 + boxSize) % boxSize;
        int kyp1 = (bestKy + 1) % boxSize;

        double cLx = ctx->corrBuf[bestKy * boxSize + kxm1];
        double cRx = ctx->corrBuf[bestKy * boxSize + kxp1];
        double cLy = ctx->corrBuf[kym1 * boxSize + bestKx];
        double cRy = ctx->corrBuf[kyp1 * boxSize + bestKx];

        double subDx = 0.0, subDy = 0.0;
        double denomX = cLx - 2.0 * peak + cRx;
        if (fabs(denomX) > 1e-12) subDx = 0.5 * (cLx - cRx) / denomX;
        double denomY = cLy - 2.0 * peak + cRy;
        if (fabs(denomY) > 1e-12) subDy = 0.5 * (cLy - cRy) / denomY;

        cxOut[s]   = (double)bestDx + subDx;
        cyOut[s]   = (double)bestDy + subDy;
        peakOut[s] = peak / (double)(boxSize * boxSize);
    }

    return DAO_SUCCESS;
}

/** @brief Double-precision counterpart of daoCentroidSpotsCorrelationFFTUpdateRef. */
int_fast8_t daoCentroidSpotsCorrelationFFTUpdateRefDouble(daoCentroidCorrFFTDoubleCtx* ctx,
    const double* image, int imageSizeX, int imageSizeY,
    const double* ref, const double* cent, double threshold, double alpha,
    double* refImageShmOut) {
    if (!ctx) {
        daoError("daoCentroidSpotsCorrelationFFTUpdateRefDouble: NULL context\n");
        return DAO_ERROR;
    }
    if (alpha <= 0.0) return DAO_SUCCESS;

    daoCentroidSpotsUpdateReferenceDouble(image, imageSizeX, imageSizeY, ref, cent,
                                           ctx->boxSize, ctx->nSuba, threshold, alpha, ctx->refReal);
    daoCentroidCorrFFTRefreshSpectraDouble(ctx);

    if (refImageShmOut) {
        memcpy(refImageShmOut, ctx->refReal,
               (size_t)ctx->nSuba * ctx->boxSize * ctx->boxSize * sizeof(double));
    }

    return DAO_SUCCESS;
}
