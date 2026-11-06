/*
 * daoCentroidSpotsRelativeFast vs daoCentroidSpotsRelative: the outputs must be
 * identical to the last bit (memcmp), on synthetic Shack-Hartmann frames with
 * moving spots, noise, negative pixels, dark and saturated subapertures, several
 * thresholds and minimum fluxes, several box sizes and subaperture counts
 * (including counts that are not a multiple of the lane count). Then the time of
 * each on an R138-sized case.
 *
 *   build/tests/testCentroidRelativeFast        exit 0: identical, 1: a difference
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dao.h"
#include "daoTools.h"

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec * 1e-3;
}

static unsigned int rng = 12345u;
static float frand(void) /* [0, 1) */
{
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) * (1.0f / 16777216.0f);
}

/* nSub x nSub subapertures of `sub` px, image size nSub*sub + 2*margin; references at
 * the subaperture centres (+ an offset), valid subapertures in a disc. Returns nSuba. */
static int geometry(int nSub, int sub, int margin, float refOffset, int *size, float *ref)
{
    int n = 0;
    float x[4096], y[4096];
    *size = nSub * sub + 2 * margin;
    for (int j = 0; j < nSub; j++)
        for (int i = 0; i < nSub; i++)
        {
            float r = hypotf(i - (nSub - 1) / 2.0f, j - (nSub - 1) / 2.0f);
            if (r > nSub / 2.0f + 0.3f)
                continue;
            x[n] = margin + i * sub + (sub - 1) / 2.0f + refOffset;
            y[n] = margin + j * sub + (sub - 1) / 2.0f + refOffset;
            n++;
        }
    memcpy(ref, x, n * sizeof(float));
    memcpy(ref + n, y, n * sizeof(float));
    return n;
}

static void frame(float *img, int size, int nSub, int sub, int margin, int kind)
{
    float ox = (frand() - 0.5f) * sub * 0.6f, oy = (frand() - 0.5f) * sub * 0.6f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
        {
            float v = (frand() - 0.5f) * 40.0f; /* noise, negative pixels too */
            int xi = x - margin, yi = y - margin;
            if (xi >= 0 && yi >= 0 && xi < nSub * sub && yi < nSub * sub)
            {
                int si = xi / sub, sj = yi / sub;
                float cx = xi % sub - (sub - 1) / 2.0f - ox - 0.1f * si;
                float cy = yi % sub - (sub - 1) / 2.0f - oy + 0.07f * sj;
                float amp = 3000.0f * (0.5f + frand() * 0.0f + 0.5f * ((si + sj) % 3 == 0));
                if (kind == 1 && (si + 2 * sj) % 7 == 0)
                    amp = 0.0f;                        /* dark subapertures */
                if (kind == 2 && (si * sj) % 5 == 1)
                    amp = 65535.0f;                    /* saturated-looking */
                v += amp * expf(-(cx * cx + cy * cy) / (0.15f * sub * sub));
            }
            img[y * size + x] = v;
        }
}

int main(void)
{
    static float img[300 * 300], ref[2 * 4096], a[4 * 4096], b[4 * 4096];
    const int subs[] = { 6, 7, 8, 10, 20 };
    const int nSubs[] = { 5, 8, 16, 17, 23 };
    const float thresholds[] = { 0.0f, 0.1f, 0.3f, 1.0f };
    const float minFluxes[] = { 0.0f, 20000.0f };
    long cases = 0, values = 0, bad = 0;

    for (int is = 0; is < 5; is++)
        for (int in = 0; in < 5; in++)
            for (int ro = 0; ro < 2; ro++)
            {
                int sub = subs[is], nSub = nSubs[in], size;
                if (nSub * sub + 4 > 300)
                    continue;
                int n = geometry(nSub, sub, 2, ro ? 0.3f : 0.0f, &size, ref);
                for (int k = 0; k < 3; k++)
                    for (int it = 0; it < 4; it++)
                        for (int im = 0; im < 2; im++)
                            for (int rep = 0; rep < 3; rep++)
                            {
                                frame(img, size, nSub, sub, 2, k);
                                memset(a, 0xAB, sizeof(a));
                                memset(b, 0xCD, sizeof(b));
                                daoCentroidSpotsRelative(img, size, size, ref, sub, n,
                                                         thresholds[it], minFluxes[im], a);
                                daoCentroidSpotsRelativeFast(img, size, size, ref, sub, n,
                                                             thresholds[it], minFluxes[im], b);
                                cases++;
                                values += 4 * n;
                                if (memcmp(a, b, 4 * n * sizeof(float)) != 0)
                                {
                                    for (int q = 0; q < 4 * n; q++)
                                        if (memcmp(&a[q], &b[q], sizeof(float)) != 0)
                                        {
                                            if (bad < 10)
                                                printf("DIFF sub %d nSuba %d thr %g minFlux %g output %d: %.9g vs %.9g\n",
                                                       sub, n, thresholds[it], minFluxes[im], q, a[q], b[q]);
                                            bad++;
                                        }
                                }
                            }
            }
    printf("%ld cases, %ld values compared bit for bit: %ld different\n", cases, values, bad);

    /* timing, R138-like: 16 x 16 subapertures of 8 px (box 9 x 9) */
    int size;
    int n = geometry(16, 8, 2, 0.0f, &size, ref);
    frame(img, size, 16, 8, 2, 0);
    const int reps = 20000;
    double best[2] = { 1e9, 1e9 }, sum[2] = { 0, 0 };
    for (int r = 0; r < reps; r++)
        for (int f = 0; f < 2; f++)
        {
            double t0 = now_us();
            if (f == 0)
                daoCentroidSpotsRelative(img, size, size, ref, 8, n, 0.1f, 0.0f, a);
            else
                daoCentroidSpotsRelativeFast(img, size, size, ref, 8, n, 0.1f, 0.0f, b);
            double dt = now_us() - t0;
            sum[f] += dt;
            if (dt < best[f])
                best[f] = dt;
        }
    printf("%d subapertures of 8 px (box 9 x 9), %d calls each:\n", n, reps);
    printf("  daoCentroidSpotsRelative      mean %6.2f us  best %6.2f us\n", sum[0] / reps, best[0]);
    printf("  daoCentroidSpotsRelativeFast  mean %6.2f us  best %6.2f us  (x%.1f)\n",
           sum[1] / reps, best[1], sum[0] / sum[1]);
    return bad ? 1 : 0;
}
