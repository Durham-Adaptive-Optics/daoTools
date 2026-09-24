/*
 * testGpuStages - each GPU stage (daoGpuStages.h) against the CPU daoTools code it
 * replaces, on the same SHMs.
 *
 * The CPU reference is the libdaoTools function the app calls, or, for the apps
 * that compute in their own loop (daoCalIntensity*, daoDescrambleOcam2 -b 2), a
 * copy of that loop. SHMs are created in $DAO_GPU_TEST_DIR (default
 * /dev/shm/daoGpuStagesTest: host SHMs mapped into the GPU) and removed at the end;
 * a folder that is not tmpfs (e.g. on disk) tests the copy mode instead.
 *
 *   testGpuStages [device]      prints one line per test, exit code = failures
 */
#include <cuda_runtime.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <random>
#include <string>
#include <vector>
#include "dao.h"
extern "C" {
#include "daoTools.h"
#include "daoToolsCorrFFT.h"
}
#include "daoGpuStages.h"

static std::string dir;
static std::vector<std::pair<std::string, IMAGE *>> created;
static std::vector<daoGpuPort *> ports;               /* uploaded before each run (copy mode) */
static std::mt19937 rng(12345);
static int failures = 0;

static IMAGE *mk(const char *name, uint32_t s0, uint32_t s1, uint8_t atype)
{
    IMAGE *im = (IMAGE *) calloc(1, sizeof(IMAGE));
    uint32_t size[2] = {s0, s1};
    std::string path = dir + "/" + name + ".im.shm";
    if (daoShmCreate(im, path.c_str(), 2, size, atype, 1, 0) != DAO_SUCCESS) {
        fprintf(stderr, "cannot create %s\n", path.c_str());
        exit(99);
    }
    memset(im->array.V, 0, im->md[0].nelement * (atype == _DATATYPE_UINT8 ? 1 : atype == _DATATYPE_UINT16 || atype == _DATATYPE_INT16 ? 2 : atype == _DATATYPE_DOUBLE ? 8 : 4));
    created.push_back({path, im});
    return im;
}

static void touch(IMAGE *im) { daoShmSetDataPartFinalize(im); }       /* new cnt0 */

static float uni(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }

/* max |gpu - cpu| / max(|cpu|, floor) over n values */
static void check(const char *what, const float *gpu, const float *cpu, long n, double tol, double floor = 1e-6)
{
    double worst = 0, scale = floor;
    long at = 0;
    for (long k = 0; k < n; k++)
        scale = fmax(scale, fabs(cpu[k]));
    for (long k = 0; k < n; k++) {
        double d = fabs((double) gpu[k] - cpu[k]);
        if (!(d <= worst)) {                          /* also catches NaN */
            worst = d;
            at = k;
        }
    }
    bool ok = worst / scale <= tol;
    printf("%-50s %s  max rel. error %.2e (at %ld: gpu %g cpu %g)\n", what, ok ? "PASS" : "FAIL", worst / scale, at,
           (double) gpu[at], (double) cpu[at]);
    failures += !ok;
}

/* SH outputs: slopes (first 2n) in pixels, absolute; the rest (flux, peak) relative */
static void checkSh(const char *what, const float *gpu, const float *cpu, int n, int parts)
{
    char name[96];
    snprintf(name, sizeof name, "%s: slopes", what);
    check(name, gpu, cpu, 2 * n, 1e-4, 1.0);                   /* scale 1: absolute, px */
    snprintf(name, sizeof name, "%s: %s", what, parts == 3 ? "flux/peak" : "flux, weight");
    check(name, gpu + 2 * n, cpu + 2 * n, (parts - 2) * n, 1e-5);
}

static void checkExact(const char *what, const void *gpu, const void *cpu, size_t bytes)
{
    bool ok = memcmp(gpu, cpu, bytes) == 0;
    printf("%-50s %s  (bit-exact over %zu bytes)\n", what, ok ? "PASS" : "FAIL", bytes);
    failures += !ok;
}

/* run one stage once (parameters loaded first), result in the output SHM's host array */
static bool runStage(daoGpuStage *s, cudaStream_t st, bool post = false)
{
    if (!s) {
        printf("stage creation failed\n");
        failures++;
        return false;
    }
    for (daoGpuPort *p : ports)                       /* no-op for mapped ports */
        if (p != daoGpuStageOutput(s))
            daoGpuPortUpload(p, st);
    if (daoGpuStageUpdate(s, st) < 0 || !daoGpuStageRun(s, st)) {
        printf("%s: run failed\n", daoGpuStageName(s));
        failures++;
        return false;
    }
    daoGpuPortDownload(daoGpuStageOutput(s), st);
    cudaStreamSynchronize(st);
    if (post)
        daoGpuStagePost(s, st);
    return cudaGetLastError() == cudaSuccess;
}

/* --------------------------------------------------------- SH test image */
struct Sh {
    int size, box, nSuba;
    IMAGE *img, *ref, *centre, *thr, *refImage;
};

/* nGrid x nGrid sub-apertures of box px, circular pupil, `margin` px around the
 * grid (the correlation search reads box/2 + searchRange around each centre) */
static Sh makeSh(const char *tag, int nGrid, int box, int margin, float threshold)
{
    Sh sh;
    sh.box = box;
    sh.size = nGrid * box + 2 * margin;
    std::vector<float> cx, cy;
    float c = (nGrid - 1) / 2.f;
    for (int j = 0; j < nGrid; j++)
        for (int i = 0; i < nGrid; i++)
            if (hypotf(i - c, j - c) <= nGrid / 2.f) {
                cx.push_back(margin + i * box + box / 2.f);
                cy.push_back(margin + j * box + box / 2.f);
            }
    sh.nSuba = (int) cx.size();
    std::string n = std::string(tag);
    sh.img = mk((n + "Img").c_str(), sh.size, sh.size, _DATATYPE_FLOAT);
    sh.ref = mk((n + "Ref").c_str(), 2 * sh.nSuba, 1, _DATATYPE_FLOAT);
    sh.centre = mk((n + "Centre").c_str(), 2 * sh.nSuba, 1, _DATATYPE_FLOAT);
    sh.thr = mk((n + "Thr").c_str(), 1, 1, _DATATYPE_FLOAT);
    sh.refImage = mk((n + "RefImage").c_str(), sh.nSuba * box, box, _DATATYPE_FLOAT);
    float *im = sh.img->array.F;
    for (int p = 0; p < sh.size * sh.size; p++)
        im[p] = uni(0.f, 20.f);                               /* background + noise */
    for (int s = 0; s < sh.nSuba; s++) {
        float sx = cx[s] + uni(-3.f, 3.f), sy = cy[s] + uni(-3.f, 3.f), w = uni(1.2f, 2.f), a = uni(500, 2000);
        for (int y = (int) cy[s] - box / 2; y < (int) cy[s] + box / 2; y++)
            for (int x = (int) cx[s] - box / 2; x < (int) cx[s] + box / 2; x++)
                im[y * sh.size + x] += a * expf(-((x - sx) * (x - sx) + (y - sy) * (y - sy)) / (2 * w * w));
        sh.ref->array.F[s] = cx[s] + uni(-0.3f, 0.3f);
        sh.ref->array.F[sh.nSuba + s] = cy[s] + uni(-0.3f, 0.3f);
        sh.centre->array.F[s] = cx[s];
        sh.centre->array.F[sh.nSuba + s] = cy[s];
        float *t = sh.refImage->array.F + (size_t) s * box * box;          /* centred Gaussian template */
        for (int y = 0; y < box; y++)
            for (int x = 0; x < box; x++)
                t[y * box + x] = expf(-((x - box / 2) * (x - box / 2) + (y - box / 2) * (y - box / 2)) / (2 * 1.5f * 1.5f));
    }
    sh.thr->array.F[0] = threshold;
    for (IMAGE *i : {sh.img, sh.ref, sh.centre, sh.thr, sh.refImage})
        touch(i);
    return sh;
}

int main(int argc, char **argv)
{
    int device = argc > 1 ? atoi(argv[1]) : 0;
    const char *d = getenv("DAO_GPU_TEST_DIR");
    dir = d ? d : "/dev/shm/daoGpuStagesTest";
    mkdir(dir.c_str(), 0775);
    daoSetLogLevel(1);
    cudaSetDevice(device);
    cudaStream_t st;
    cudaStreamCreate(&st);
    auto port = [](IMAGE *im) {
        daoGpuPort *p = new daoGpuPort;
        if (!daoGpuPortInit(p, im, 1)) {
            fprintf(stderr, "port %s failed\n", im->name);
            exit(99);
        }
        ports.push_back(p);
        return p;
    };

    /* ------------------------------------------------ pixel calibration */
    {
        const int H = 160, W = 160;
        IMAGE *raw = mk("raw16", H, W, _DATATYPE_UINT16), *ff = mk("ff", H, W, _DATATYPE_FLOAT),
              *bg = mk("bg", H, W, _DATATYPE_FLOAT), *out = mk("cal", H, W, _DATATYPE_FLOAT),
              *cpu = mk("calCpu", H, W, _DATATYPE_FLOAT);
        for (int k = 0; k < H * W; k++) {
            raw->array.UI16[k] = (uint16_t) uni(0, 4000);
            ff->array.F[k] = uni(0.8f, 1.2f);
            bg->array.F[k] = uni(90, 110);
        }
        touch(raw); touch(ff); touch(bg);
        daoGpuStage *s = daoGpuPixelCalibrateCreate(port(raw), ff, bg, port(out));
        if (runStage(s, st)) {
            daoToolsShmCalibrate(raw, ff, bg, cpu);
            check("pixelCalibrate (uint16 -> float)", out->array.F, cpu->array.F, H * W, 1e-6);
        }
    }

    /* ----------------------------------------- calIntensity(Norm): app loops */
    for (int norm = 0; norm < 2; norm++) {
        const int H = 120, W = 120;
        std::string t = norm ? "N" : "";
        IMAGE *raw = mk(("ciRaw" + t).c_str(), H, W, _DATATYPE_UINT16), *ff = mk(("ciFf" + t).c_str(), H, W, _DATATYPE_FLOAT),
              *bg = mk(("ciBg" + t).c_str(), H, W, _DATATYPE_FLOAT), *ref = mk(("ciRef" + t).c_str(), H, W, _DATATYPE_FLOAT),
              *valid = mk(("ciValid" + t).c_str(), H, W, _DATATYPE_UINT32),
              *illum = mk(("ciIllum" + t).c_str(), H, W, _DATATYPE_UINT32);
        int nValid = 0;
        for (int k = 0; k < H * W; k++) {
            raw->array.UI16[k] = (uint16_t) uni(0, 4000);
            ff->array.F[k] = uni(-0.1f, 1.2f);                  /* a few dead pixels (ff <= 0) */
            bg->array.F[k] = uni(90, 110);
            ref->array.F[k] = uni(0, 3000);
            valid->array.UI32[k] = uni(0, 1) < 0.4f;
            illum->array.UI32[k] = valid->array.UI32[k] && uni(0, 1) < 0.8f;
            nValid += valid->array.UI32[k];
        }
        for (IMAGE *i : {raw, ff, bg, ref, valid, illum})
            touch(i);
        IMAGE *out = mk(("ciOut" + t).c_str(), nValid, 1, _DATATYPE_FLOAT);
        daoGpuStage *s = norm ? daoGpuCalIntensityNormCreate(port(raw), ff, bg, ref, valid, illum, port(out))
                              : daoGpuCalIntensityCreate(port(raw), ff, bg, ref, valid, illum, port(out));
        if (!runStage(s, st))
            continue;
        std::vector<float> cpu(nValid), cal(nValid);
        std::vector<int> lut;
        for (int i = 0; i < H * W; i++)
            if (valid->array.UI32[i] == 1)
                lut.push_back(i);
        if (norm) {                                   /* daoCalIntensityNorm.c */
            float sumCal = 0, sumRef = 0;
            for (int k = 0; k < nValid; k++) {
                int i = lut[k], il = illum->array.UI32[i] == 1;
                float f = ff->array.F[i];
                cal[k] = il ? ((float) raw->array.UI16[i] - bg->array.F[i]) * (f > 0 ? f : 0) : 0;
                sumCal += cal[k];
                if (il)
                    sumRef += ref->array.F[i];
            }
            float inv = sumCal > 0 ? 1 / sumCal : 0, invRef = sumRef > 0 ? 1 / sumRef : 0;
            for (int k = 0; k < nValid; k++)
                cpu[k] = cal[k] * inv - ref->array.F[lut[k]] * invRef * (float) (illum->array.UI32[lut[k]] == 1);
        } else {                                      /* daoCalIntensity.c */
            float sum = 0;
            for (int k = 0; k < nValid; k++) {
                int i = lut[k];
                float f = ff->array.F[i];
                cal[k] = f > 0 ? ((float) raw->array.UI16[i] - bg->array.F[i]) * f : 0;
                if (illum->array.UI32[i] == 1)
                    sum += cal[k];
            }
            float inv = sum > 0 ? 1 / sum : 0;
            for (int k = 0; k < nValid; k++)
                cpu[k] = (cal[k] - ref->array.F[lut[k]]) * inv * (float) illum->array.UI32[lut[k]];
        }
        check(norm ? "calIntensityNorm" : "calIntensity", out->array.F, cpu.data(), nValid, 1e-4);
    }

    /* ----------------------------------------------- extraction family */
    {
        const int H = 100, W = 120, N = H * W;
        IMAGE *a16 = mk("xA16", H, W, _DATATYPE_UINT16), *mask = mk("xMask", H, W, _DATATYPE_UINT32);
        IMAGE *af = mk("xAf", H, W, _DATATYPE_FLOAT), *bf = mk("xBf", H, W, _DATATYPE_FLOAT);
        IMAGE *a16s = mk("xA16s", H, W, _DATATYPE_INT16), *b16s = mk("xB16s", H, W, _DATATYPE_INT16);
        IMAGE *normv = mk("xNorm", 1, 1, _DATATYPE_FLOAT);
        int n = 0;
        for (int k = 0; k < N; k++) {
            a16->array.UI16[k] = (uint16_t) uni(0, 60000);
            mask->array.UI32[k] = uni(0, 1) < 0.3f;
            af->array.F[k] = uni(-50, 3000);
            bf->array.F[k] = uni(0, 1000);
            a16s->array.SI16[k] = (int16_t) uni(-3000, 3000);
            b16s->array.SI16[k] = (int16_t) uni(-3000, 3000);
            n += mask->array.UI32[k];
        }
        normv->array.F[0] = 2.5f;
        for (IMAGE *i : {a16, mask, af, bf, a16s, b16s, normv})
            touch(i);
        /* bf already normalised for NormImage: small values */
        IMAGE *bfn = mk("xBfn", H, W, _DATATYPE_FLOAT);
        for (int k = 0; k < N; k++)
            bfn->array.F[k] = bf->array.F[k] * 1e-6f;
        touch(bfn);

        IMAGE *o = mk("xOut16", n, 1, _DATATYPE_UINT16), *c = mk("xCpu16", n, 1, _DATATYPE_UINT16);
        if (runStage(daoGpuPixelExtractCreate(port(a16), mask, port(o)), st)) {
            daoToolsShmExtract(a16, mask, c);
            checkExact("pixelExtract (uint16)", o->array.V, c->array.V, n * 2);
        }
        const char *names[] = {"pixelSubstractExtract (float)", "pixelSubstractExtract (float, norm)"};
        for (int withNorm = 0; withNorm < 2; withNorm++) {
            IMAGE *of = mk(withNorm ? "xOutFn" : "xOutF", n, 1, _DATATYPE_FLOAT),
                  *cf = mk(withNorm ? "xCpuFn" : "xCpuF", n, 1, _DATATYPE_FLOAT);
            if (runStage(daoGpuPixelSubstractExtractCreate(port(af), bf, mask, withNorm ? normv : NULL, port(of)), st)) {
                daoToolsShmSubstractExtract(af, bf, mask, cf, normv, withNorm);
                check(names[withNorm], of->array.F, cf->array.F, n, 1e-6);
            }
        }
        IMAGE *os = mk("xOutS", n, 1, _DATATYPE_INT16), *cs = mk("xCpuS", n, 1, _DATATYPE_INT16);
        if (runStage(daoGpuPixelSubstractExtractCreate(port(a16s), b16s, mask, NULL, port(os)), st)) {
            daoToolsShmSubstractExtract(a16s, b16s, mask, cs, normv, 0);
            checkExact("pixelSubstractExtract (int16)", os->array.V, cs->array.V, n * 2);
        }
        IMAGE *on = mk("xOutN", n, 1, _DATATYPE_FLOAT), *cn = mk("xCpuN", n, 1, _DATATYPE_FLOAT);
        if (runStage(daoGpuPixelSubstractExtractNormCreate(port(af), bf, mask, port(on)), st)) {
            daoToolsShmSubstractExtractNorm(af, bf, mask, cn);
            check("pixelSubstractExtractNorm", on->array.F, cn->array.F, n, 1e-5);
        }
        IMAGE *oi = mk("xOutI", n, 1, _DATATYPE_FLOAT), *ci = mk("xCpuI", n, 1, _DATATYPE_FLOAT);
        if (runStage(daoGpuPixelSubstractExtractNormImageCreate(port(af), bfn, mask, port(oi)), st)) {
            daoToolsShmSubstractExtractNormImage(af, bfn, mask, ci);
            check("pixelSubstractExtractNormImage", oi->array.F, ci->array.F, n, 1e-5);
        }
    }

    /* ------------------------------------------------ OCAM2 descrambling */
    {
        /* binning 1: raw rows of W bytes (W/2 samples), LUT = permutation */
        const int W = 240, H = 120, n = H * W / 2;
        IMAGE *raw = mk("ocRaw", W, H, _DATATYPE_UINT8), *lut = mk("ocLut", n, 1, _DATATYPE_INT32),
              *out = mk("ocOut", 120, 120, _DATATYPE_UINT16);
        for (int k = 0; k < W * H; k++)
            raw->array.UI8[k] = (uint8_t) uni(0, 255.99f);
        std::vector<int> perm(n);
        for (int k = 0; k < n; k++)
            perm[k] = k;
        std::shuffle(perm.begin(), perm.end(), rng);
        memcpy(lut->array.SI32, perm.data(), n * sizeof(int));
        touch(raw); touch(lut);
        if (runStage(daoGpuDescrambleOcam2Create(port(raw), lut, 1, port(out)), st)) {
            std::vector<uint16_t> img16(n), cpu(n);
            std::vector<uint16_t *> rows(H);
            for (int r = 0; r < H; r++)
                rows[r] = img16.data() + r * (W / 2);
            daoDescrambleOcam2Image(raw->array.UI8, H, W, rows.data(), perm.data(), n, cpu.data());
            checkExact("descrambleOcam2 (binning 1)", out->array.V, cpu.data(), n * 2);
        }
        /* binning 2: the tool's fixed 240x240 layout */
        const int n2 = 240 * 240;
        IMAGE *raw2 = mk("ocRaw2", 480, 240, _DATATYPE_UINT8), *lut2 = mk("ocLut2", n2, 1, _DATATYPE_INT32),
              *out2 = mk("ocOut2", 240, 240, _DATATYPE_UINT16);
        for (int k = 0; k < 480 * 240; k++)
            raw2->array.UI8[k] = (uint8_t) uni(0, 255.99f);
        for (int k = 0; k < n2; k++)
            lut2->array.SI32[k] = (int) uni(0, n2 - 0.01f);
        for (int k = 0; k < n2; k++)
            out2->array.UI16[k] = 0xffff;                        /* must be overwritten */
        touch(raw2); touch(lut2);
        if (runStage(daoGpuDescrambleOcam2Create(port(raw2), lut2, 2, port(out2)), st)) {
            std::vector<uint16_t> cpu(n2, 0);                   /* daoDescrambleOcam2.c, -b 2 */
            const uint8_t *src = raw2->array.UI8;
            const int32_t *l = lut2->array.SI32;
            for (int i = 0; i < 7200; i++) {
                int a = l[i * 2] * 2, b = l[i * 2 + 43200] * 2;
                cpu[i] = (src[a + 1] << 8) | src[a];
                cpu[7200 + i] = (src[b + 1] << 8) | src[b];
            }
            checkExact("descrambleOcam2 (binning 2)", out2->array.V, cpu.data(), n2 * 2);
        }
    }

    /* ------------------------------------------------------ centroiders */
    {
        /* 8x8 sub-apertures of 20 px, circular pupil; 10 px margin for the correlation */
        Sh sh = makeSh("sh", 8, 20, 10, 60.f);
        int n = sh.nSuba;
        printf("SH image %dx%d, %d sub-apertures of %d px\n", sh.size, sh.size, n, sh.box);
        IMAGE *o = mk("cOut", 4 * n, 1, _DATATYPE_FLOAT);
        std::vector<float> cpu(4 * n);

        if (runStage(daoGpuCentroidCreate(port(sh.img), sh.ref, sh.thr, sh.box, n, port(o)), st)) {
            daoCentroidSpots(sh.img->array.F, sh.img->md[0].size[0], sh.ref->array.F, sh.box, n, sh.thr->array.F[0], cpu.data());
            checkSh("centroid", o->array.F, cpu.data(), n, 3);
        }
        IMAGE *oRel = mk("cOutRel", 4 * n, 1, _DATATYPE_FLOAT);
        sh.thr->array.F[0] = 0.2f;                                /* relative threshold */
        touch(sh.thr);
        if (runStage(daoGpuCentroidRelativeCreate(port(sh.img), sh.ref, sh.thr, sh.box, n, port(oRel)), st)) {
            daoCentroidSpotsRelative(sh.img->array.F, sh.img->md[0].size[1], sh.img->md[0].size[0], sh.ref->array.F,
                                     sh.box, n, sh.thr->array.F[0], cpu.data());
            checkSh("centroidRelative", oRel->array.F, cpu.data(), n, 4);
        }
        IMAGE *oRR = mk("cOutRR", 4 * n, 1, _DATATYPE_FLOAT), *offs = mk("cRefOffsets", 2 * n, 1, _DATATYPE_FLOAT);
        for (int k = 0; k < 2 * n; k++)                          /* reference slopes */
            offs->array.F[k] = uni(-0.5f, 0.5f);
        touch(offs);
        if (runStage(daoGpuCentroidRelativeRefCreate(port(sh.img), sh.centre, offs, sh.thr, sh.box, n, port(oRR)), st)) {
            daoCentroidSpotsRelativeRef(sh.img->array.F, sh.img->md[0].size[1], sh.img->md[0].size[0],
                                        sh.centre->array.F, offs->array.F, sh.box, n, sh.thr->array.F[0], cpu.data());
            checkSh("centroidRelativeRef", oRR->array.F, cpu.data(), n, 4);
        }

        sh.thr->array.F[0] = 30.f;                                /* absolute threshold */
        touch(sh.thr);
        for (int R : {5, 2}) {                                    /* coarse-to-fine, then dense */
            char name[64];
            snprintf(name, sizeof name, "cOutCorr%d", R);
            IMAGE *oc = mk(name, 3 * n, 1, _DATATYPE_FLOAT);
            if (runStage(daoGpuCentroidCorrelationCreate(port(sh.img), sh.centre, sh.refImage, sh.thr, sh.box, n, R,
                                                         0.f, port(oc)), st)) {
                daoCentroidSpotsCorrelation(sh.img->array.F, sh.img->md[0].size[1], sh.img->md[0].size[0],
                                            sh.centre->array.F, sh.refImage->array.F, sh.box, n, R, sh.thr->array.F[0],
                                            cpu.data());
                snprintf(name, sizeof name, "centroidCorrelation (range %d)", R);
                checkSh(name, oc->array.F, cpu.data(), n, 3);
            }
        }
        IMAGE *of = mk("cOutFFT", 3 * n, 1, _DATATYPE_FLOAT);
        daoCentroidCorrFFTCtx *ctx = daoCentroidSpotsCorrelationFFTInit(sh.box, n, sh.refImage->array.F);
        if (runStage(daoGpuCentroidCorrelationFFTCreate(port(sh.img), sh.centre, sh.refImage, sh.thr, sh.box, n, 0.f,
                                                        port(of)), st)) {
            daoCentroidSpotsCorrelationFFT(ctx, sh.img->array.F, sh.img->md[0].size[1], sh.img->md[0].size[0],
                                           sh.centre->array.F, sh.thr->array.F[0], cpu.data());
            checkSh("centroidCorrelationFFT (vs FFTW)", of->array.F, cpu.data(), n, 3);
        }

        /* reference following the spots (alpha): one frame, then compare the reference */
        const float alpha = 0.1f;
        std::vector<float> refCpu(sh.refImage->array.F, sh.refImage->array.F + (size_t) n * sh.box * sh.box);
        IMAGE *oa = mk("cOutAlpha", 3 * n, 1, _DATATYPE_FLOAT);
        uint64_t c0 = sh.refImage->md[0].cnt0;
        if (runStage(daoGpuCentroidCorrelationCreate(port(sh.img), sh.centre, sh.refImage, sh.thr, sh.box, n, 5, alpha,
                                                     port(oa)), st, true)) {
            daoCentroidSpotsCorrelation(sh.img->array.F, sh.img->md[0].size[1], sh.img->md[0].size[0],
                                        sh.centre->array.F, refCpu.data(), sh.box, n, 5, sh.thr->array.F[0], cpu.data());
            daoCentroidSpotsUpdateReference(sh.img->array.F, sh.img->md[0].size[1], sh.img->md[0].size[0],
                                            sh.centre->array.F, cpu.data(), sh.box, n, 0.f, alpha, refCpu.data());
            check("centroidCorrelation reference update", sh.refImage->array.F, refCpu.data(),
                  (long) n * sh.box * sh.box, 1e-5);
            printf("%-50s %s  (reference SHM published: cnt0 %lu -> %lu)\n", "  ... written back to the SHM",
                   sh.refImage->md[0].cnt0 > c0 ? "PASS" : "FAIL", (unsigned long) c0,
                   (unsigned long) sh.refImage->md[0].cnt0);
            failures += sh.refImage->md[0].cnt0 <= c0;
        }
        daoCentroidSpotsCorrelationFFTFree(ctx);
    }

    /* ------------------------------------------------------ vectors */
    {
        const int nIn = 104, nOut = 97;
        IMAGE *in = mk("vIn", 2 * nIn, 1, _DATATYPE_FLOAT);        /* longer than the matrix input */
        IMAGE *m = mk("vM", nOut, nIn, _DATATYPE_FLOAT), *out = mk("vOut", nOut, 1, _DATATYPE_FLOAT);
        for (int k = 0; k < 2 * nIn; k++)
            in->array.F[k] = uni(-1, 1);
        for (int k = 0; k < nIn * nOut; k++)
            m->array.F[k] = uni(-1, 1);
        touch(in); touch(m);
        if (runStage(daoGpuMvmCreate(port(in), m, port(out)), st)) {
            std::vector<float> cpu(nOut);
            for (int r = 0; r < nOut; r++) {
                double acc = 0;
                for (int c = 0; c < nIn; c++)
                    acc += (double) m->array.F[r * nIn + c] * in->array.F[c];
                cpu[r] = (float) acc;
            }
            check("mvm (first 104 of 208 inputs)", out->array.F, cpu.data(), nOut, 1e-5);
        }
    }

    for (auto &c : created) {
        daoShmClose(c.second);
        unlink(c.first.c_str());
    }
    rmdir(dir.c_str());
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures;
}
