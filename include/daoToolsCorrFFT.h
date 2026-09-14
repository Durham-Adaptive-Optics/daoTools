/**
 * @file    daoToolsCorrFFT.h
 * @brief   FFT-based (periodic) correlation centroider
 *
 * Alternative to daoCentroidSpotsCorrelation (spatial-domain, windowed
 * search): computes the *entire* periodic cross-correlation surface for each
 * subaperture via FFT (O(boxSize^2 log boxSize) instead of
 * O(searchRange^2 * boxSize^2)), so the full box is searched for the price
 * of one FFT round trip -- no searchRange/clipping trade-off. See Poyneer,
 * "Scene-based Shack-Hartmann wave-front sensing: analysis and simulation,"
 * Appl. Opt. 42(29), 5807-5815 (2003), sections 2B-2D and 4.
 *
 * Unlike the other daoCentroidSpots* functions, this one is stateful: FFTW
 * plan creation is not real-time-safe, and the reference templates do not
 * change frame to frame, so both are precomputed once by *Init and reused by
 * every subsequent per-frame call. Usage:
 *
 *     ctx = daoCentroidSpotsCorrelationFFTInit(boxSize, nSuba, refImage);
 *     while (...) {
 *         daoCentroidSpotsCorrelationFFT(ctx, image, ..., cent);
 *     }
 *     daoCentroidSpotsCorrelationFFTFree(ctx);
 *
 * Single- and double-precision variants are separate (mirroring
 * daoToolsLeakyIntegrator / daoToolsLeakyIntegratorDouble in daoTools.h):
 * the float API is backed by fftw3f, the double API by fftw3. Both are only
 * compiled in when the build was configured with FFTW available (see
 * DAO_TOOLS_HAVE_CORR_FFT below).
 *
 * @author  S. Cetre
 */

#ifndef _DAOTOOLSCORRFFT_H
#define _DAOTOOLSCORRFFT_H

/* Defined by the build when FFTW (both fftw3f and fftw3) was found and this
 * file's implementation was actually compiled in -- guard callers on this,
 * not on the presence of the header, since the header is always installed. */
#define DAO_TOOLS_HAVE_CORR_FFT 1

#include "dao.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque: holds the FFTW plans and the pre-transformed reference templates.
 * Created once (Init), reused every frame, released once (Free). */
typedef struct daoCentroidCorrFFTCtx daoCentroidCorrFFTCtx;
typedef struct daoCentroidCorrFFTDoubleCtx daoCentroidCorrFFTDoubleCtx;

/* ---------------------------------------------------------------------- */
/* Single precision (float, fftw3f)                                       */
/* ---------------------------------------------------------------------- */

/**
 * @brief Build FFTW plans and precompute reference-template FFTs.
 *
 * @p refImage uses the same per-subaperture stacked layout as
 * daoCentroidSpotsCorrelation: one `boxSize x boxSize` template per
 * subaperture, stacked row-wise (size `nSuba * boxSize * boxSize`). It is
 * copied/transformed once here; the caller does not need to keep it alive
 * afterwards.
 *
 * @return A context to pass to daoCentroidSpotsCorrelationFFT, or NULL on
 *         failure (e.g. FFTW plan creation failed).
 */
daoCentroidCorrFFTCtx* daoCentroidSpotsCorrelationFFTInit(int boxSize, int nSuba, const float* refImage);

/**
 * @brief Per-frame call: full periodic correlation + sub-pixel peak for every subaperture.
 *
 * For each subaperture: threshold the observed `boxSize x boxSize` window
 * (pixels below @p threshold treated as zero, same convention as
 * daoCentroidSpotsCorrelation), FFT it, multiply by the cached conjugated
 * reference spectrum, inverse-FFT to get the full periodic correlation
 * surface, then find its peak and refine to sub-pixel precision with the
 * same independent 3-point parabolic fit used by daoCentroidSpotsCorrelation.
 *
 * Output layout (SoA), @p cent size >= `3*nSuba`, identical to
 * daoCentroidSpotsCorrelation:
 * - `cent[0 .. nSuba-1]`         : X centroids (cx), relative to ref X
 * - `cent[nSuba .. 2*nSuba-1]`   : Y centroids (cy), relative to ref Y
 * - `cent[2*nSuba .. 3*nSuba-1]` : Correlation peak value (quality/flux diagnostic)
 *
 * Because the correlation is periodic (not padded), a shift is only
 * meaningful up to +-boxSize/2 before it aliases; peaks are searched only in
 * that interior range for exactly this reason. This assumes, as
 * daoCentroidSpotsCorrelation's windowed search also implicitly does, that
 * each spot's energy stays inside its subaperture box.
 *
 * @param[in]  ctx         Context from daoCentroidSpotsCorrelationFFTInit
 * @param[in]  image       Pointer to the input image (row-major, float)
 * @param[in]  imageSizeX  Image width (pixels)
 * @param[in]  imageSizeY  Image height (pixels), unused (kept for API symmetry)
 * @param[in]  ref         Reference positions, SoA layout (size `2*nSuba`):
 *                         `ref[0..nSuba-1]` = X, `ref[nSuba..2*nSuba-1]` = Y
 * @param[in]  threshold   Absolute pixel intensity threshold applied to the observed image
 * @param[out] cent        Output array (size >= `3*nSuba`, layout above)
 *
 * @return DAO_SUCCESS on success
 */
int_fast8_t daoCentroidSpotsCorrelationFFT(daoCentroidCorrFFTCtx* ctx,
    const float* image, int imageSizeX, int imageSizeY,
    const float* ref, float threshold, float* cent);

/**
 * @brief Blend this frame's centroid-aligned observation into the cached
 * reference (EMA), refresh the cached spectrum, and mirror the result out.
 *
 * Call this *after* publishing the centroids from the matching
 * daoCentroidSpotsCorrelationFFT call -- not on the real-time critical path.
 * See daoCentroidSpotsUpdateReference (daoTools.h) for the alignment/blending
 * semantics this wraps. @p refImageShmOut (e.g. the refImage SHM's own array)
 * receives a copy of the blended reference so it doesn't silently drift out
 * of sync with what any external tool, or a future restart, would read; pass
 * NULL to skip that.
 *
 * @param[in]  alpha  EMA rate in (0, 1]; alpha <= 0 is a no-op.
 * @return DAO_SUCCESS on success, DAO_ERROR if ctx is NULL
 */
int_fast8_t daoCentroidSpotsCorrelationFFTUpdateRef(daoCentroidCorrFFTCtx* ctx,
    const float* image, int imageSizeX, int imageSizeY,
    const float* ref, const float* cent, float threshold, float alpha,
    float* refImageShmOut);

/** @brief Release everything created by daoCentroidSpotsCorrelationFFTInit. */
void daoCentroidSpotsCorrelationFFTFree(daoCentroidCorrFFTCtx* ctx);

/* ---------------------------------------------------------------------- */
/* Double precision (double, fftw3)                                       */
/* ---------------------------------------------------------------------- */

/** @brief Double-precision counterpart of daoCentroidSpotsCorrelationFFTInit. */
daoCentroidCorrFFTDoubleCtx* daoCentroidSpotsCorrelationFFTDoubleInit(int boxSize, int nSuba, const double* refImage);

/** @brief Double-precision counterpart of daoCentroidSpotsCorrelationFFT. */
int_fast8_t daoCentroidSpotsCorrelationFFTDouble(daoCentroidCorrFFTDoubleCtx* ctx,
    const double* image, int imageSizeX, int imageSizeY,
    const double* ref, double threshold, double* cent);

/** @brief Double-precision counterpart of daoCentroidSpotsCorrelationFFTUpdateRef. */
int_fast8_t daoCentroidSpotsCorrelationFFTUpdateRefDouble(daoCentroidCorrFFTDoubleCtx* ctx,
    const double* image, int imageSizeX, int imageSizeY,
    const double* ref, const double* cent, double threshold, double alpha,
    double* refImageShmOut);

/** @brief Release everything created by daoCentroidSpotsCorrelationFFTDoubleInit. */
void daoCentroidSpotsCorrelationFFTDoubleFree(daoCentroidCorrFFTDoubleCtx* ctx);

#ifdef __cplusplus
}
#endif

#endif
