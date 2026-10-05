/*
 * testCentroidWindows -- daoCentroidWindows (daoToolsWindows.c), CPU:
 *   - square windows, parabola: the same slopes as daoCentroidSpotsCorrelation
 *   - an extended scene shifted by known amounts, rectangular windows and search ranges:
 *     every estimator finds the shift
 *   - centre of gravity of a spot at a known position
 *   - reference slope, no light, minFlux, a window that does not fit
 *
 *   build/tests/testCentroidWindows      (LD_LIBRARY_PATH=build/src)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dao.h"
#include "daoTools.h"

static int failures = 0;

static void check(const char *what, int ok, const char *fmt, double a, double b)
{
    printf("%-62s %s  ", what, ok ? "PASS" : "FAIL");
    printf(fmt, a, b);
    printf("\n");
    failures += !ok;
}

static float frand(void) { return (float)rand() / RAND_MAX; }

/* a smooth random scene: sum of Gaussian blobs, n x n */
static void scene(float *s, int n)
{
    memset(s, 0, (size_t)n * n * sizeof *s);
    for (int b = 0; b < 40; b++) {
        float cx = frand() * n, cy = frand() * n, sig = 1.5f + 3.0f * frand(), a = 50.0f + 100.0f * frand();
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                s[y * n + x] += a * expf(-((x - cx) * (x - cx) + (y - cy) * (y - cy)) / (2 * sig * sig));
    }
}

static void row(float *t, float x, float y, float w, float h, float rx, float ry, float rsx, float rsy, float n)
{
    t[DAO_WINDOW_X] = x; t[DAO_WINDOW_Y] = y; t[DAO_WINDOW_WIDTH] = w; t[DAO_WINDOW_HEIGHT] = h;
    t[DAO_WINDOW_SEARCH_X] = rx; t[DAO_WINDOW_SEARCH_Y] = ry;
    t[DAO_WINDOW_REF_SLOPE_X] = rsx; t[DAO_WINDOW_REF_SLOPE_Y] = rsy; t[DAO_WINDOW_N] = n;
}

int main(void)
{
    srand(12345);

    /* ---- 1. square windows + parabola = daoCentroidSpotsCorrelation */
    {
        const int box = 20, grid = 6, nS = grid * grid, im = box * grid + 2 * 8, sr = 5;
        float *img = calloc((size_t)im * im, sizeof(float)), *ref = malloc(2 * nS * sizeof(float));
        float *tpl = malloc((size_t)nS * box * box * sizeof(float)), *table = malloc(nS * DAO_WINDOW_COLS * sizeof(float));
        float *a = malloc(3 * nS * sizeof(float)), *b = malloc(3 * nS * sizeof(float));
        for (int s = 0; s < nS; s++) {
            float cx = 8 + (s % grid) * box + box / 2, cy = 8 + (s / grid) * box + box / 2;
            ref[s] = cx; ref[nS + s] = cy;
            float ox = (frand() - 0.5f) * 6, oy = (frand() - 0.5f) * 6;      /* spot offset, pixels */
            for (int y = 0; y < im; y++)
                for (int x = 0; x < im; x++) {
                    float dx = x - (cx - 0.5f + ox), dy = y - (cy - 0.5f + oy);
                    if (fabsf(dx) < box && fabsf(dy) < box)
                        img[y * im + x] += 1000.0f * expf(-(dx * dx + dy * dy) / 8.0f);
                }
            for (int y = 0; y < box; y++)
                for (int x = 0; x < box; x++) {
                    float dx = x - (box / 2 - 0.5f), dy = y - (box / 2 - 0.5f);
                    tpl[(size_t)s * box * box + y * box + x] = expf(-(dx * dx + dy * dy) / 8.0f);
                }
            row(table + s * DAO_WINDOW_COLS, cx, cy, box, box, sr, sr, 0, 0, 0);
        }
        daoCentroidSpotsCorrelation(img, im, im, ref, tpl, box, nS, sr, 1.0f, 0.0f, a);
        daoCentroidWindows(img, im, im, table, nS, tpl, box, box, DAO_WINDOWS_CORRELATION, DAO_PEAK_PARABOLA,
                           1.0f, 0.0f, b);
        double worst = 0;
        for (int k = 0; k < 3 * nS; k++)
            worst = fmax(worst, fabs(a[k] - b[k]));
        check("square windows + parabola = daoCentroidSpotsCorrelation", worst <= 1e-5,
              "max |diff| %g (over %g values; rounding: two compilation units)", worst, 3.0 * nS);
        free(img); free(ref); free(tpl); free(table); free(a); free(b);
    }

    /* ---- 2. an extended scene shifted by known amounts, 5 rectangular windows */
    {
        const int n = 96;
        float *sc = malloc((size_t)n * n * sizeof(float)), *img = malloc((size_t)n * n * sizeof(float));
        scene(sc, n);
        const int sx = 3, sy = -2;                                   /* the image = the scene shifted */
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                int xs = x - sx, ys = y - sy;
                img[y * n + x] = xs >= 0 && xs < n && ys >= 0 && ys < n ? sc[ys * n + xs] : 0.0f;
            }
        /* 5 directions: centre, then 4 around, with different sizes and search ranges */
        const float wins[5][6] = {{48, 48, 16, 16, 6, 6}, {30, 30, 12, 10, 5, 4}, {66, 30, 10, 14, 4, 5},
                                  {30, 66, 14, 12, 6, 4}, {66, 66, 11, 9, 4, 4}};
        const int refW = 16, refH = 16;
        float table[5 * DAO_WINDOW_COLS], tpl[5 * 16 * 16], out[15];
        memset(tpl, 0, sizeof tpl);
        for (int k = 0; k < 5; k++) {
            row(table + k * DAO_WINDOW_COLS, wins[k][0], wins[k][1], wins[k][2], wins[k][3], wins[k][4], wins[k][5],
                0, 0, 9);
            int w = (int)wins[k][2], h = (int)wins[k][3];
            int x0 = (int)roundf(wins[k][0]) - w / 2, y0 = (int)roundf(wins[k][1]) - h / 2;
            for (int y = 0; y < h; y++)                          /* the reference: the unshifted scene */
                for (int x = 0; x < w; x++)
                    tpl[k * refW * refH + y * refW + x] = sc[(y0 + y) * n + x0 + x];
        }
        check("table check: 5 rectangular windows fit",
              daoCentroidWindowsCheck(table, 5, n, n, refW, refH, DAO_WINDOWS_CORRELATION, DAO_PEAK_PARABOLA)
              == DAO_SUCCESS, "%g%g", 0, 0);
        const char *names[] = {"max", "parabola", "barycenter", "barycenter_threshold", "barycenter_threshold_weighted"};
        const double tol[] = {0.0, 0.05, 1.5, 0.3, 0.3};             /* the simple barycentre is biased
                                                                        towards the map's centre */
        (void)tol;
        for (int p = DAO_PEAK_MAX; p <= DAO_PEAK_BARYCENTER_THRESHOLD_WEIGHTED; p++) {
            daoCentroidWindows(img, n, n, table, 5, tpl, refW, refH, DAO_WINDOWS_CORRELATION_NORMALIZED, p, 0.0f,
                               0.0f, out);
            double worst = 0, diff = 0;
            for (int k = 0; k < 5; k++) {
                worst = fmax(worst, fmax(fabs(out[k] - sx), fabs(out[5 + k] - sy)));
                /* the same estimator, recomputed here from the normalised correlation map */
                const float *r = table + k * DAO_WINDOW_COLS;
                int w = (int)r[DAO_WINDOW_WIDTH], h = (int)r[DAO_WINDOW_HEIGHT];
                int rx = (int)r[DAO_WINDOW_SEARCH_X], ry = (int)r[DAO_WINDOW_SEARCH_Y], N = (int)r[DAO_WINDOW_N];
                int x0 = (int)roundf(r[DAO_WINDOW_X]) - w / 2, y0 = (int)roundf(r[DAO_WINDOW_Y]) - h / 2;
                const float *t = tpl + k * refW * refH;
                double map[17][17], ts = 0, tt = 0;
                for (int i = 0; i < h; i++)
                    for (int j = 0; j < w; j++) { ts += t[i * refW + j]; tt += (double)t[i * refW + j] * t[i * refW + j]; }
                double tv = tt - ts * ts / (w * h);
                int bx = 0, by = 0;
                for (int dy = -ry; dy <= ry; dy++)
                    for (int dx = -rx; dx <= rx; dx++) {
                        double si = 0, sii = 0, sit = 0;
                        for (int i = 0; i < h; i++)
                            for (int j = 0; j < w; j++) {
                                double v = img[(y0 + dy + i) * n + x0 + dx + j];
                                si += v; sii += v * v; sit += v * t[i * refW + j];
                            }
                        double c = (sit - si * ts / (w * h)) / sqrt((sii - si * si / (w * h)) * tv);
                        map[dy + ry][dx + rx] = c;
                        if (c > map[by + ry][bx + rx]) { bx = dx; by = dy; }
                    }
                double ex = bx, ey = by;
                if (p == DAO_PEAK_PARABOLA) {
                    if (bx > -rx && bx < rx) {
                        double l = map[by + ry][bx - 1 + rx], c = map[by + ry][bx + rx], rr = map[by + ry][bx + 1 + rx];
                        ex += 0.5 * (l - rr) / (l - 2 * c + rr);
                    }
                    if (by > -ry && by < ry) {
                        double l = map[by - 1 + ry][bx + rx], c = map[by + ry][bx + rx], rr = map[by + 1 + ry][bx + rx];
                        ey += 0.5 * (l - rr) / (l - 2 * c + rr);
                    }
                }
                else if (p >= DAO_PEAK_BARYCENTER) {
                    /* values sorted, largest first: the N kept, the (N+1)-th as the weights' base */
                    int m = (2 * rx + 1) * (2 * ry + 1), keep = p == DAO_PEAK_BARYCENTER ? m : N;
                    double v[289], vx[289], vy[289];
                    int q = 0;
                    for (int dy = -ry; dy <= ry; dy++)
                        for (int dx = -rx; dx <= rx; dx++, q++) { v[q] = map[dy + ry][dx + rx]; vx[q] = dx; vy[q] = dy; }
                    for (int a = 0; a < m; a++)            /* selection sort, descending */
                        for (int b = a + 1; b < m; b++)
                            if (v[b] > v[a]) {
                                double s0 = v[a]; v[a] = v[b]; v[b] = s0;
                                s0 = vx[a]; vx[a] = vx[b]; vx[b] = s0; s0 = vy[a]; vy[a] = vy[b]; vy[b] = s0;
                            }
                    double base = p == DAO_PEAK_BARYCENTER_THRESHOLD_WEIGHTED && keep < m ? v[keep] : 0;
                    double sw = 0, swx = 0, swy = 0;
                    for (int a = 0; a < keep; a++) { sw += v[a] - base; swx += (v[a] - base) * vx[a]; swy += (v[a] - base) * vy[a]; }
                    ex = swx / sw; ey = swy / sw;
                }
                diff = fmax(diff, fmax(fabs(out[k] - ex), fabs(out[5 + k] - ey)));
            }
            char what[96];
            snprintf(what, sizeof what, "normalised, %s: = the estimator recomputed", names[p]);
            check(what, diff <= 1e-4, "max |diff| %.2g px%.0g", diff, 0);
            snprintf(what, sizeof what, "normalised, %s: shift (3, -2) found", names[p]);
            if (p == DAO_PEAK_BARYCENTER)                  /* every value of the map: pulled to its centre */
                printf("%-62s INFO  max |error| %.3f px (a centre of mass of the whole map)\n", what, worst);
            else
                check(what, worst <= (p == DAO_PEAK_MAX ? 0.0 : 0.5), "max |error| %.3f px%.0g", worst, 0);
        }
        daoCentroidWindows(img, n, n, table, 5, tpl, refW, refH, DAO_WINDOWS_CORRELATION, DAO_PEAK_PARABOLA, 0.0f,
                           0.0f, out);
        double prod = 0;
        for (int k = 0; k < 5; k++)
            prod = fmax(prod, fmax(fabs(out[k] - sx), fabs(out[5 + k] - sy)));
        printf("%-62s INFO  max |error| %.3f px: the product peaks on bright parts\n",
               "scene, plain product correlation (for spots)", prod);
        /* the reference slope is subtracted */
        for (int k = 0; k < 5; k++) {
            table[k * DAO_WINDOW_COLS + DAO_WINDOW_REF_SLOPE_X] = 3.0f;
            table[k * DAO_WINDOW_COLS + DAO_WINDOW_REF_SLOPE_Y] = -2.0f;
        }
        daoCentroidWindows(img, n, n, table, 5, tpl, refW, refH, DAO_WINDOWS_CORRELATION_NORMALIZED, DAO_PEAK_MAX,
                           0.0f, 0.0f, out);
        double worst = 0;
        for (int k = 0; k < 10; k++)
            worst = fmax(worst, fabs(out[k]));
        check("reference slope (3, -2) subtracted: slopes 0", worst == 0.0, "max |slope| %g%g", worst, 0);
        /* minFlux, then no light */
        daoCentroidWindows(img, n, n, table, 5, tpl, refW, refH, DAO_WINDOWS_CORRELATION, DAO_PEAK_PARABOLA, 0.0f,
                           1e12f, out);
        int zero = 1;
        for (int k = 0; k < 10; k++)
            zero &= out[k] == 0.0f;
        check("minFlux above the flux: slopes 0, flux still given", zero && out[10] > 0, "flux %g%g", out[10], 0);
        memset(img, 0, (size_t)n * n * sizeof(float));
        daoCentroidWindows(img, n, n, table, 5, tpl, refW, refH, DAO_WINDOWS_CORRELATION,
                           DAO_PEAK_BARYCENTER_THRESHOLD, 0.0f, 0.0f, out);
        zero = 1;
        for (int k = 0; k < 10; k++)
            zero &= out[k] == 0.0f;
        check("no light: slopes 0", zero, "%g%g", 0, 0);
        /* a window that does not fit */
        table[DAO_WINDOW_X] = 3.0f;
        check("table check: a window off the image is reported",
              daoCentroidWindowsCheck(table, 5, n, n, refW, refH, DAO_WINDOWS_CORRELATION, DAO_PEAK_MAX) == DAO_ERROR,
              "%g%g", 0, 0);
        check("table check: max is refused for a centre of gravity",
              daoCentroidWindowsCheck(table + DAO_WINDOW_COLS, 1, n, n, refW, refH, DAO_WINDOWS_COG, DAO_PEAK_MAX)
              == DAO_ERROR, "%g%g", 0, 0);
        free(sc); free(img);
    }

    /* ---- 3. centre of gravity of a spot at a known position */
    {
        const int n = 40;
        float *img = calloc((size_t)n * n, sizeof(float));
        const float px = 21.3f, py = 17.6f;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                img[y * n + x] = 500.0f * expf(-((x - px) * (x - px) + (y - py) * (y - py)) / (2 * 1.5f * 1.5f))
                                 + 2.0f * frand();             /* a little background noise */
        float table[DAO_WINDOW_COLS], out[3];
        row(table, 20, 20, 20, 16, 0, 0, 0, 0, 25);
        const char *names[] = {"", "", "barycenter", "barycenter_threshold (N 25)", "barycenter_threshold_weighted (N 25)"};
        const double tol[] = {0, 0, 0.6, 0.05, 0.05};             /* the simple one: biased by the background */
        for (int p = DAO_PEAK_BARYCENTER; p <= DAO_PEAK_BARYCENTER_THRESHOLD_WEIGHTED; p++) {
            daoCentroidWindows(img, n, n, table, 1, NULL, 0, 0, DAO_WINDOWS_COG, p, -1e30f, 0.0f, out);
            double err = fmax(fabs(out[0] - (px - 20)), fabs(out[1] - (py - 20)));
            char what[96];
            snprintf(what, sizeof what, "centre of gravity, spot at (+1.3, -2.4): %s", names[p]);
            check(what, err <= tol[p], "error %.3f px (tolerance %.2f)", err, tol[p]);
        }
        free(img);
    }

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures;
}
