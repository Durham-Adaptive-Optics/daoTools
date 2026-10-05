/**
 * @file    daoToolsWindows.c
 * @brief   Slopes measured in analysis windows: Shack-Hartmann on an extended object,
 *          in one direction (one window per sub-aperture) or several (several windows
 *          per sub-aperture, each looking at another part of the field).
 *
 * Each window is one row of a table (DAO_WINDOW_COLS values, see daoTools.h): its
 * centre, width and height, search range, reference slope and N, the number of
 * values its barycentres keep. Each has its own reference image (correlation).
 */
#include <math.h>
#include <string.h>
#include "dao.h"
#include "daoTools.h"

/* Same coarse-to-fine search as daoCentroidSpotsCorrelation (daoTools.c) */
#define DAO_WINDOWS_COARSE_STRIDE 2

/* Cross-correlation of a w x h template (a refW-wide stack row) with the image at one
 * integer shift (dx, dy) from (x0, y0); image pixels below threshold count as 0.
 * The same sum, in the same order, as daoCentroidCorrSum. */
static float windowCorrSum(const float *image, int imageSizeX, int x0, int y0, int dx, int dy,
                           const float *tpl, int refW, int w, int h, float threshold)
{
    float sum = 0.0f;
    for (int i = 0; i < h; ++i) {
        const float *imRow = image + (size_t)(y0 + dy + i) * imageSizeX + (x0 + dx);
        const float *tRow = tpl + (size_t)i * refW;
        for (int j = 0; j < w; ++j) {
            float pixel = imRow[j];
            if (pixel < threshold) {
                pixel = 0.0f;
            }
            sum += pixel * tRow[j];
        }
    }
    return sum;
}

/* Zero-mean normalised cross-correlation of a w x h template with the image at one
 * shift: sum((I - mean I)(T - mean T)) / sqrt(sum((I - mean I)^2) sum((T - mean T)^2)),
 * in [-1, 1]; 0 when either is flat. tSum, tVar: the template's sum and
 * sum((T - mean T)^2), computed once per window. For an extended object: the plain product
 * peaks towards the bright parts of the scene, this one at the shift that matches. */
static float windowZnccSum(const float *image, int imageSizeX, int x0, int y0, int dx, int dy,
                           const float *tpl, int refW, int w, int h, float threshold, double tSum, double tVar)
{
    double si = 0.0, sii = 0.0, sit = 0.0;
    for (int i = 0; i < h; ++i) {
        const float *imRow = image + (size_t)(y0 + dy + i) * imageSizeX + (x0 + dx);
        const float *tRow = tpl + (size_t)i * refW;
        for (int j = 0; j < w; ++j) {
            double pixel = imRow[j] < threshold ? 0.0 : imRow[j];
            si += pixel;
            sii += pixel * pixel;
            sit += pixel * tRow[j];
        }
    }
    const double n = (double)w * h, iVar = sii - si * si / n;
    if (iVar <= 0.0 || tVar <= 0.0)
        return 0.0f;
    return (float)((sit - si * tSum / n) / sqrt(iVar * tVar));
}

/* The correlation the method asks for, at one shift */
static float corrAt(int method, const float *image, int imageSizeX, int x0, int y0, int dx, int dy,
                    const float *tpl, int refW, int w, int h, float threshold, double tSum, double tVar)
{
    if (method == DAO_WINDOWS_CORRELATION_NORMALIZED)
        return windowZnccSum(image, imageSizeX, x0, y0, dx, dy, tpl, refW, w, h, threshold, tSum, tVar);
    return windowCorrSum(image, imageSizeX, x0, y0, dx, dy, tpl, refW, w, h, threshold);
}

/* The k-th largest of v[0..n-1] (k = 0: the largest). Reorders v. */
static float kthLargest(float *v, int n, int k)
{
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        const float pivot = v[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (v[i] > pivot) i++;
            while (v[j] < pivot) j--;
            if (i <= j) {
                float t = v[i]; v[i] = v[j]; v[j] = t;
                i++; j--;
            }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else return v[k];
    }
    return v[k];
}

/* Barycentre of n samples, value v at position (x, y):
 *   DAO_PEAK_BARYCENTER                    every sample, weight = its value
 *   DAO_PEAK_BARYCENTER_THRESHOLD          the nKeep largest, weight = its value
 *   DAO_PEAK_BARYCENTER_THRESHOLD_WEIGHTED the nKeep largest, weight = value minus the
 *                                          (nKeep+1)-th largest (the first one left out)
 * nKeep <= 0 or >= n keeps every sample (weighted: by its value). Ties at the cut are kept
 * in order until nKeep is reached. Returns 0 when the weights sum to 0 (no information). */
static int barycenter(const float *v, const float *x, const float *y, int n, int nKeep, int peak,
                      float *scratch, float *bx, float *by)
{
    float cut = -INFINITY, base = 0.0f;
    int ties = n;                                     /* values equal to cut still to keep */
    if (peak != DAO_PEAK_BARYCENTER && nKeep > 0 && nKeep < n) {
        memcpy(scratch, v, (size_t)n * sizeof *v);
        cut = kthLargest(scratch, n, nKeep - 1);      /* the nKeep-th largest */
        int above = 0;
        for (int k = 0; k < n; ++k)
            above += v[k] > cut;
        ties = nKeep - above;
        if (peak == DAO_PEAK_BARYCENTER_THRESHOLD_WEIGHTED) {
            memcpy(scratch, v, (size_t)n * sizeof *v);
            base = kthLargest(scratch, n, nKeep);     /* the (nKeep+1)-th */
        }
    }
    double sw = 0.0, sx = 0.0, sy = 0.0;
    for (int k = 0; k < n; ++k) {
        if (v[k] < cut || (v[k] == cut && ties-- <= 0))
            continue;
        const double w = (double)v[k] - base;
        sw += w;
        sx += w * x[k];
        sy += w * y[k];
    }
    if (sw == 0.0)
        return 0;
    *bx = (float)(sx / sw);
    *by = (float)(sy / sw);
    return 1;
}

/* A window's geometry from its table row; 0 when it does not fit (the image, the search
 * range limit, the reference stack for correlation). */
static int windowGeometry(const float *row, int imageSizeX, int imageSizeY, int refW, int refH,
                          int method, int *x0, int *y0, int *w, int *h, int *rx, int *ry)
{
    *w = (int)lroundf(row[DAO_WINDOW_WIDTH]);
    *h = (int)lroundf(row[DAO_WINDOW_HEIGHT]);
    *rx = method != DAO_WINDOWS_COG ? (int)lroundf(row[DAO_WINDOW_SEARCH_X]) : 0;
    *ry = method != DAO_WINDOWS_COG ? (int)lroundf(row[DAO_WINDOW_SEARCH_Y]) : 0;
    *x0 = (int)roundf(row[DAO_WINDOW_X]) - *w / 2;
    *y0 = (int)roundf(row[DAO_WINDOW_Y]) - *h / 2;
    if (*w < 1 || *h < 1 || (long)*w * *h > DAO_WINDOW_MAX_PIXELS)
        return 0;
    if (*rx < 0 || *ry < 0 || *rx > DAO_CENTROID_CORR_MAX_SEARCH_RANGE || *ry > DAO_CENTROID_CORR_MAX_SEARCH_RANGE)
        return 0;
    if (method != DAO_WINDOWS_COG && (*w > refW || *h > refH))
        return 0;
    return *x0 - *rx >= 0 && *y0 - *ry >= 0 && *x0 + *w - 1 + *rx < imageSizeX && *y0 + *h - 1 + *ry < imageSizeY;
}

int_fast8_t daoCentroidWindowsCheck(const float *table, int nWin, int imageSizeX, int imageSizeY,
                                    int refW, int refH, int method, int peak)
{
    if (method != DAO_WINDOWS_CORRELATION && method != DAO_WINDOWS_CORRELATION_NORMALIZED
        && method != DAO_WINDOWS_COG) {
        daoError("daoCentroidWindows: method %d: DAO_WINDOWS_CORRELATION, _CORRELATION_NORMALIZED or _COG\n",
                 method);
        return DAO_ERROR;
    }
    if (peak < DAO_PEAK_MAX || peak > DAO_PEAK_BARYCENTER_THRESHOLD_WEIGHTED
        || (method == DAO_WINDOWS_COG && (peak == DAO_PEAK_MAX || peak == DAO_PEAK_PARABOLA))) {
        daoError("daoCentroidWindows: estimator %d does not apply to this method (a centre of gravity "
                 "takes a barycentre)\n", peak);
        return DAO_ERROR;
    }
    int bad = 0;
    for (int k = 0; k < nWin; ++k) {
        int x0, y0, w, h, rx, ry;
        if (!windowGeometry(table + (size_t)k * DAO_WINDOW_COLS, imageSizeX, imageSizeY, refW, refH, method,
                            &x0, &y0, &w, &h, &rx, &ry)) {
            if (bad++ < 5)
                daoError("daoCentroidWindows: window %d (x %g, y %g, %g x %g, search %g x %g) does not fit "
                         "the %d x %d image, the %d x %d reference stack or the limits (search <= %d, "
                         "<= %d pixels)\n", k, table[k * DAO_WINDOW_COLS + DAO_WINDOW_X],
                         table[k * DAO_WINDOW_COLS + DAO_WINDOW_Y], table[k * DAO_WINDOW_COLS + DAO_WINDOW_WIDTH],
                         table[k * DAO_WINDOW_COLS + DAO_WINDOW_HEIGHT], table[k * DAO_WINDOW_COLS + DAO_WINDOW_SEARCH_X],
                         table[k * DAO_WINDOW_COLS + DAO_WINDOW_SEARCH_Y], imageSizeX, imageSizeY, refW, refH,
                         DAO_CENTROID_CORR_MAX_SEARCH_RANGE, DAO_WINDOW_MAX_PIXELS);
        }
    }
    if (bad) {
        daoError("daoCentroidWindows: %d of %d windows do not fit: their slopes are 0\n", bad, nWin);
        return DAO_ERROR;
    }
    return DAO_SUCCESS;
}

int_fast8_t daoCentroidWindows(const float *image, int imageSizeX, int imageSizeY,
                               const float *table, int nWin, const float *refImages, int refW, int refH,
                               int method, int peak, float threshold, float minFlux, float *out)
{
    daoTrace("\n");
    float *sxOut = out, *syOut = out + nWin, *fluxOut = out + 2 * nWin;
    /* the samples a barycentre runs over: the correlation map, or the window's pixels */
    float val[DAO_WINDOW_MAX_PIXELS], px[DAO_WINDOW_MAX_PIXELS], py[DAO_WINDOW_MAX_PIXELS];
    float scratch[DAO_WINDOW_MAX_PIXELS];

    for (int k = 0; k < nWin; ++k) {
        const float *row = table + (size_t)k * DAO_WINDOW_COLS;
        int x0, y0, w, h, rx, ry;
        sxOut[k] = syOut[k] = fluxOut[k] = 0.0f;
        if (!windowGeometry(row, imageSizeX, imageSizeY, refW, refH, method, &x0, &y0, &w, &h, &rx, &ry))
            continue;                                 /* daoCentroidWindowsCheck says why */

        /* the light in the window: its raw pixels, at no shift */
        float flux = 0.0f;
        for (int i = 0; i < h; ++i) {
            const float *imRow = image + (size_t)(y0 + i) * imageSizeX + x0;
            for (int j = 0; j < w; ++j)
                flux += imRow[j];
        }
        fluxOut[k] = flux;
        if (minFlux > 0.0f && flux < minFlux)
            continue;                                 /* too little light: no information */
        const int nKeep = (int)lroundf(row[DAO_WINDOW_N]);
        float sx = 0.0f, sy = 0.0f;

        if (method == DAO_WINDOWS_COG) {
            /* centre of gravity of the window's pixels, relative to the window's centre */
            int n = 0;
            for (int i = 0; i < h; ++i) {
                const float *imRow = image + (size_t)(y0 + i) * imageSizeX + x0;
                for (int j = 0; j < w; ++j, ++n) {
                    float pixel = imRow[j];
                    val[n] = pixel < threshold ? 0.0f : pixel;
                    px[n] = (float)(x0 + j);
                    py[n] = (float)(y0 + i);
                }
            }
            float bx, by;
            if (!barycenter(val, px, py, n, nKeep, peak, scratch, &bx, &by))
                continue;
            sx = bx - row[DAO_WINDOW_X];
            sy = by - row[DAO_WINDOW_Y];
        }
        else {
            const float *tpl = refImages + (size_t)k * refW * refH;
            double tSum = 0.0, tVar = 0.0;                /* the template's, for the normalised one */
            if (method == DAO_WINDOWS_CORRELATION_NORMALIZED) {
                double tt = 0.0;
                for (int i = 0; i < h; ++i)
                    for (int j = 0; j < w; ++j) {
                        tSum += tpl[(size_t)i * refW + j];
                        tt += (double)tpl[(size_t)i * refW + j] * tpl[(size_t)i * refW + j];
                    }
                tVar = tt - tSum * tSum / ((double)w * h);
            }
#define CORR(dx_, dy_) corrAt(method, image, imageSizeX, x0, y0, (dx_), (dy_), tpl, refW, w, h, threshold, tSum, tVar)
            float peakValue = -INFINITY;
            if (peak == DAO_PEAK_MAX || peak == DAO_PEAK_PARABOLA) {
                /* the maximum, coarse then fine, as daoCentroidSpotsCorrelation */
                const int stride = DAO_WINDOWS_COARSE_STRIDE;
                int bestDx = 0, bestDy = 0;
                int fy0 = -ry, fy1 = ry, fx0 = -rx, fx1 = rx;
                if (rx > stride || ry > stride) {
                    for (int dy = -ry; dy <= ry; dy += stride)
                        for (int dx = -rx; dx <= rx; dx += stride) {
                            float sum = CORR(dx, dy);
                            if (sum > peakValue) { peakValue = sum; bestDx = dx; bestDy = dy; }
                        }
                    fy0 = bestDy - stride; if (fy0 < -ry) fy0 = -ry;
                    fy1 = bestDy + stride; if (fy1 >  ry) fy1 =  ry;
                    fx0 = bestDx - stride; if (fx0 < -rx) fx0 = -rx;
                    fx1 = bestDx + stride; if (fx1 >  rx) fx1 =  rx;
                }
                for (int dy = fy0; dy <= fy1; ++dy)
                    for (int dx = fx0; dx <= fx1; ++dx) {
                        float sum = CORR(dx, dy);
                        if (sum > peakValue) { peakValue = sum; bestDx = dx; bestDy = dy; }
                    }
                float subDx = 0.0f, subDy = 0.0f;
                if (peak == DAO_PEAK_PARABOLA) {
                    /* 3-point parabola along each axis, inside the search range */
                    if (bestDx > -rx && bestDx < rx) {
                        float cL = CORR(bestDx - 1, bestDy);
                        float cR = CORR(bestDx + 1, bestDy);
                        float denom = cL - 2.0f * peakValue + cR;
                        if (fabsf(denom) > 1e-12f)
                            subDx = 0.5f * (cL - cR) / denom;
                    }
                    if (bestDy > -ry && bestDy < ry) {
                        float cL = CORR(bestDx, bestDy - 1);
                        float cR = CORR(bestDx, bestDy + 1);
                        float denom = cL - 2.0f * peakValue + cR;
                        if (fabsf(denom) > 1e-12f)
                            subDy = 0.5f * (cL - cR) / denom;
                    }
                }
                sx = (float)bestDx + subDx;
                sy = (float)bestDy + subDy;
            }
            else {
                /* a barycentre of the whole correlation map */
                int n = 0;
                for (int dy = -ry; dy <= ry; ++dy)
                    for (int dx = -rx; dx <= rx; ++dx, ++n) {
                        val[n] = CORR(dx, dy);
                        px[n] = (float)dx;
                        py[n] = (float)dy;
                        if (val[n] > peakValue) peakValue = val[n];
                    }
                if (peakValue > 0.0f && !barycenter(val, px, py, n, nKeep, peak, scratch, &sx, &sy))
                    continue;
            }
#undef CORR
            if (peakValue <= 0.0f)
                continue;                             /* no light: a flat correlation, no peak */
        }
        sxOut[k] = sx - row[DAO_WINDOW_REF_SLOPE_X];
        syOut[k] = sy - row[DAO_WINDOW_REF_SLOPE_Y];
    }
    return DAO_SUCCESS;
}
