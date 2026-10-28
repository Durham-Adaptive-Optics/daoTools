/*
 * testModalGain -- OMGI: daoToolsPseudoOpenLoop and daoToolsModalGainCost / Fit, against a
 * closed loop run with daoToolsLeakyModalIntegrator and the loop delay:
 *   - the pseudo open loop gives back the turbulence, exactly
 *   - the replayed residual is the closed loop's
 *   - the fitted gains: high for slow turbulence, the lowest for white noise; a diverging
 *     gain costs +INFINITY; a delay of 0 is refused
 *
 *   build/tests/testModalGain      (LD_LIBRARY_PATH=build/src)
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

static float gauss(void)
{
    float u = ((float)rand() + 1.0f) / ((float)RAND_MAX + 2.0f), v = (float)rand() / RAND_MAX;
    return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v);
}

#define MODES 4
#define FRAMES 3000
#define DELAY 2
#define SKIP 300

/* Each mode an AR(1) process, from white noise (a = 0) to nearly a random walk. */
static const float ar[MODES] = {0.0f, 0.9f, 0.99f, 0.999f};

/* The closed loop: residual(t) = turb(t) + out(t - DELAY), out = daoToolsLeakyModalIntegrator.
 * pol: daoToolsPseudoOpenLoop on (residual, out), cost: mean residual^2 after SKIP. */
static void closedLoop(const float *turb, float *gain, float *leak, float *pol, double *cost)
{
    float out[MODES] = {0}, offset[MODES] = {0}, residual[MODES];
    float past[DELAY + 1][MODES];
    memset(past, 0, sizeof past);
    memset(cost, 0, MODES * sizeof *cost);
    daoToolsPolHistory h;
    daoToolsPolInit(&h, MODES, DELAY);
    for (int t = 0; t < FRAMES; t++) {
        for (int m = 0; m < MODES; m++) {
            residual[m] = turb[t * MODES + m] + past[t % (DELAY + 1)][m];  // out(t - DELAY), 0 before
            if (t >= SKIP)
                cost[m] += residual[m] * residual[m];
        }
        daoToolsLeakyModalIntegrator(residual, MODES, leak, gain, offset, out);
        memcpy(past[(t + DELAY) % (DELAY + 1)], out, sizeof out);         // applied at t + DELAY
        float *p = pol + t * MODES;
        if (!daoToolsPseudoOpenLoop(&h, residual, out, p))
            for (int m = 0; m < MODES; m++)
                p[m] = NAN;
    }
    for (int m = 0; m < MODES; m++)
        cost[m] /= FRAMES - SKIP;
    daoToolsPolFree(&h);
}

int main(void)
{
    srand(1);
    float *turb = malloc(FRAMES * MODES * sizeof *turb);
    float *pol = malloc(FRAMES * MODES * sizeof *pol);
    float x[MODES] = {0};
    for (int t = 0; t < FRAMES; t++)
        for (int m = 0; m < MODES; m++)
            turb[t * MODES + m] = x[m] = ar[m] * x[m] + gauss();

    float gain[MODES] = {0.3f, 0.3f, 0.3f, 0.3f};
    float leak[MODES] = {1.0f, 1.0f, 0.98f, 1.0f};
    double loopCost[MODES], replayCost[MODES];
    closedLoop(turb, gain, leak, pol, loopCost);

    // pol = turb, from frame DELAY (no command old enough before)
    double maxErr = 0;
    int nanBefore = 1;
    for (int t = 0; t < FRAMES; t++)
        for (int m = 0; m < MODES; m++) {
            if (t < DELAY)
                nanBefore &= isnan(pol[t * MODES + m]);
            else
                maxErr = fmax(maxErr, fabs(pol[t * MODES + m] - turb[t * MODES + m]));
        }
    check("pseudo open loop = turbulence", maxErr < 1e-4, "max error %.2g%.0s", maxErr, 0);
    check("no pol for the first delay frames", nanBefore, "", 0, 0);

    // the replay on the turbulence leaves the closed loop's residual
    daoToolsModalGainCost(turb, MODES, FRAMES, DELAY, leak, gain, SKIP, replayCost);
    double maxRel = 0;
    for (int m = 0; m < MODES; m++)
        maxRel = fmax(maxRel, fabs(replayCost[m] - loopCost[m]) / loopCost[m]);
    check("replayed residual = closed loop residual (leak too)", maxRel < 1e-3, "max relative error %.2g%.0s", maxRel, 0);

    // the fit
    enum { NG = 100 };
    float gains[NG], best[MODES];
    double bestCost[MODES];
    for (int g = 0; g < NG; g++)
        gains[g] = 0.01f + 0.99f * g / (NG - 1);
    daoToolsModalGainFit(turb, MODES, FRAMES, DELAY, NULL, gains, NG, SKIP, best, bestCost);
    check("white noise: the lowest gain", best[0] == gains[0], "gain %.3f, lowest %.3f", best[0], gains[0]);
    check("nearly a random walk: a high gain", best[3] > 0.3f, "gain %.3f%.0s", best[3], 0);
    check("slower turbulence, higher gain", best[1] < best[2], "gains %.3f %.3f", best[1], best[2]);

    // the fitted gains in the closed loop leave the fitted residual, below the gain 0.3's
    float one[MODES] = {1, 1, 1, 1};
    closedLoop(turb, best, one, pol, loopCost);
    daoToolsModalGainCost(turb, MODES, FRAMES, DELAY, NULL, gain, SKIP, replayCost);
    maxRel = 0;
    int better = 1;
    for (int m = 0; m < MODES; m++) {
        maxRel = fmax(maxRel, fabs(bestCost[m] - loopCost[m]) / loopCost[m]);
        better &= bestCost[m] <= replayCost[m];
    }
    check("fitted gains: the closed loop leaves the fitted residual", maxRel < 1e-3, "max relative error %.2g%.0s", maxRel, 0);
    check("fitted residual <= gain 0.3's", better, "", 0, 0);

    // a gain that diverges with this delay, and a delay of 0
    float high[MODES] = {1.8f, 1.8f, 1.8f, 1.8f};
    daoToolsModalGainCost(turb, MODES, FRAMES, DELAY, NULL, high, SKIP, replayCost);
    check("diverging gain: +INFINITY", isinf(replayCost[3]), "cost %g%.0s", replayCost[3], 0);
    daoToolsPolHistory h;
    check("delay 0 refused", daoToolsPolInit(&h, MODES, 0) != DAO_SUCCESS, "", 0, 0);

    free(turb);
    free(pol);
    printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures != 0;
}
