/*
 * daoCpuPipeline - run a chain of CPU stages in one process, one thread, from the
 * daoGpuPipeline configuration.
 *
 * Replaces a chain of daoTools processes (daoPixelCalibrate -> daoComputeCentroid*
 * -> daoMvM -> daoLeakyIntegrator ...) connected by SHMs: the same SHMs are read and
 * written, with the same libdaoTools functions, but one thread runs the whole chain
 * when the trigger SHM is updated. Between two stages there is no semaphore
 * wake-up of another process (a few us each, much more when that process's core
 * was idle or busy): only the first stage waits. Every output SHM is still
 * published (cnt0, timestamp, semaphores) as soon as its stage is done, as the
 * separate processes do, so the other readers of the intermediate SHMs see no
 * difference.
 *
 * As daoGpuPipeline captures a frame's work once as a CUDA graph, the stages are
 * set up once: the SHMs opened and checked, their sizes and pointers kept, the
 * work of each stage a single call. A frame runs the calls in order and nothing
 * else.
 *
 * Usage: daoCpuPipeline -c <config.yaml> [-C <cpu>] [-W] [-w <cpus>] [-N <n>] [-p] [-d <level>] -L
 *
 * Configuration: the daoGpuPipeline one (see daoGpuPipeline.cpp), host SHMs only;
 * device, hostAccess and stageTimestamps are ignored (each output is timestamped
 * when published, i.e. when its data is ready).
 *   plugins: [/path/to/libmyStages.so]  # CPU stage plugins (daoCpuStages.h): each exports
 *                                      # daoCpuPluginRegister(), which registers its stage
 *                                      # types; a stage of such a type runs on the loop
 *                                      # thread, its output published by the pipeline
 *   trigger: {shm: /tmp/shIm.im.shm}  # sem: N, wait: spin (as -W), as daoGpuPipeline
 *   blasThreads: 1                     # mvm: BLAS threads (as -N; default 1, 0: the library's
 *                                      # default). More threads are real time too and spin
 *                                      # between calls: on cores another real-time process
 *                                      # uses, they hold it off until the kernel's real-time
 *                                      # throttling stops them (tens of ms)
 *   workers: [6, 8]                    # as -w: threads pinned on these CPUs, real time,
 *                                      # spinning between frames (a core of its own each);
 *                                      # the centroiders, centroidWindows and mvm are split
 *                                      # over them and the loop thread (by sub-aperture,
 *                                      # window, matrix row): the same results, bit for bit.
 *                                      # A stage with parallel: false runs on the loop thread
 *   stages:                           # in order; each as the daoTools app of that name
 *     - pixelCalibrate:        {in: ..., ff: ..., bg: ..., out: ...}         # float or double out
 *     - pixelExtract:          {in: ..., mask: ..., out: ...}
 *     - pixelSubstractExtract: {in: ..., sub: ..., mask: ..., out: ..., norm: ...}   # norm optional
 *     - centroid:            {in: ..., ref: ..., threshold: ..., out: ..., subaSize: 20, nbSuba: 52}
 *     - centroidRelative:    {in: ..., ref: ..., threshold: ..., out: ..., subaSize: 20, nbSuba: 52}
 *     - centroidRelativeRef: {in: ..., subApCentre: ..., ref: ..., threshold: ..., out: ...,
 *                             subaSize: 20, nbSuba: 52}
 *     - centroidCorrelation:    {in: ..., subApCentre: ..., refImage: ..., threshold: ..., out: ...,
 *                                subaSize: 20, nbSuba: 52, searchRange: 5, alpha: 0}
 *     - centroidCorrelationFFT: {in: ..., subApCentre: ..., refImage: ..., threshold: ..., out: ...,
 *                                subaSize: 20, nbSuba: 52, alpha: 0}
 *       every centroid stage also takes minFlux: ... (optional); alpha > 0: the reference
 *       image is updated after the frame (as the apps' -a)
 *     - centroidWindows: {in: ..., table: ..., refImage: ..., threshold: ..., out: ...,
 *                         method: correlation, peak: parabola}           # minFlux optional
 *     - mvm:       {in: ..., matrix: ..., out: ...}
 *     - applyGain: {in: ..., gain: ..., out: ..., modal: false}
 *     - leakyIntegrator: {in: ..., out: ..., loop: ..., gain: ..., leak: ...,
 *                         enable: ..., offset: ..., modal: false, keepPiston: false, clip: 10}
 *     - slice:     {in: ..., out: ..., offset: 0, count: n}      # count: default the size of out
 * Float data, as daoGpuPipeline (pixelCalibrate, pixelExtract, pixelSubstractExtract and
 * slice: any type, as their apps). Not yet: calIntensity, calIntensityNorm,
 * pixelSubstractExtractNorm(Image), descrambleOcam2: run their apps as processes.
 *
 * Parameter SHMs (ff, bg, ref, matrix, gain, ...) are read in place every frame, as
 * the apps do: a new matrix is used from the next frame, with no copy.
 *
 * A stage can be silent for a frame (a disabled integrator: zeros once, then
 * nothing): its output is not published, and a stage whose input comes from a
 * silent stage is not run (as in daoGpuPipeline).
 */
#include <yaml-cpp/yaml.h>
#include <dlfcn.h>
#include <cblas.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "dao.h"
extern "C" {
#include "daoTools.h"      /* C header without extern "C" guards */
}
#include "daoCpuStages.h"
#ifdef DAO_CPU_PIPELINE_FFT                         /* libdaoTools built with FFTW */
#include "daoToolsCorrFFT.h"
#endif

/* OpenBLAS runtime thread control (as daoMvM; no public header is pulled in here). */
extern "C" void openblas_set_num_threads(int num_threads);

static volatile sig_atomic_t stop = 0;
static void onSignal(int) { stop = 1; }

static double nowUs()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec * 1e-3;
}

static inline void cpuRelax()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

/* -W / trigger.wait: spin: as daoGpuPipeline, poll the trigger's cnt0 instead of
 * waiting on a semaphore. DAO_SUCCESS, or DAO_TIMEOUT after `seconds` or on a signal. */
static int spinWait(IMAGE *shm, uint64_t *last, double seconds)
{
    volatile IMAGE_METADATA *md = (volatile IMAGE_METADATA *) shm->md;
    double until = nowUs() + seconds * 1e6;
    for (unsigned i = 1;; i++) {
        uint64_t c = md[md[0].fifo_last_written].cnt0;
        if (c != *last) {
            *last = c;
            return DAO_SUCCESS;
        }
        cpuRelax();
        if ((i & 4095) == 0 && (stop || nowUs() > until))
            return DAO_TIMEOUT;
    }
}

/* One stage: its work for a frame, set up once. run() computes and publishes the
 * output; it returns 1 (published), 0 (silent this frame) or < 0 (error: the rest
 * of the frame is skipped). post() runs after the frame (reference updates). */
struct Stage {
    std::string type;
    IMAGE *in = nullptr, *out = nullptr;
    std::function<int()> run;
    std::function<void()> post;
    std::function<void()> destroy;
};

/* Workers (-w / workers:): threads pinned one per CPU, real time as the loop, that run
 * a share of a stage while the loop thread runs the first share. Between two jobs they
 * spin (no wake-up when the next job comes): each needs a core of its own. A job is
 * f(part, parts), part 0 being the loop thread's. */
struct Workers {
    std::vector<pthread_t> threads;
    std::vector<int> cpus;
    std::atomic<uint64_t> gen{0};                    // one more per job
    std::atomic<int> done{0};                        // workers done with the current job
    std::atomic<bool> quit{false};
    const std::function<void(int, int)> *job = nullptr;
    int parts = 1;                                   // the workers and the loop thread

    struct Arg {
        Workers *w;
        int part;
    };
    std::vector<Arg> args;

    static void *loop(void *a)
    {
        Workers *w = ((Arg *) a)->w;
        const int part = ((Arg *) a)->part;
        daoToolsEnableFTZ();
        uint64_t seen = w->gen.load(std::memory_order_acquire);
        while (!w->quit.load(std::memory_order_relaxed)) {
            uint64_t g = w->gen.load(std::memory_order_acquire);
            if (g == seen) {
                cpuRelax();
                continue;
            }
            seen = g;
            (*w->job)(part, w->parts);
            w->done.fetch_add(1, std::memory_order_release);
        }
        return nullptr;
    }

    void start(const std::vector<int> &list)
    {
        cpus = list;
        parts = (int) cpus.size() + 1;
        args.resize(cpus.size());
        threads.resize(cpus.size());
        for (size_t k = 0; k < cpus.size(); k++) {
            args[k] = {this, (int) k + 1};
            pthread_attr_t attr;
            pthread_attr_init(&attr);                // inherits the loop's real-time priority
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpus[k], &set);
            pthread_attr_setaffinity_np(&attr, sizeof set, &set);
            if (pthread_create(&threads[k], &attr, loop, &args[k]) != 0)
                throw std::runtime_error("cannot start the worker on CPU " + std::to_string(cpus[k]));
            pthread_attr_destroy(&attr);
        }
    }

    /* run f on every share, return once all are done; n: the stage's shares, 1 or parts */
    void run(const std::function<void(int, int)> &f, int n)
    {
        if (n == 1 || parts == 1) {
            f(0, 1);
            return;
        }
        job = &f;
        done.store(0, std::memory_order_relaxed);
        gen.fetch_add(1, std::memory_order_release);
        f(0, parts);
        while (done.load(std::memory_order_acquire) < parts - 1)
            cpuRelax();
    }

    void stop()
    {
        quit = true;
        for (pthread_t t : threads)
            pthread_join(t, nullptr);
        threads.clear();
    }
};

/* share part of parts of n items: [*b, *e), each share a multiple of align */
static void share(long n, int part, int parts, long align, long *b, long *e)
{
    long chunk = (n + parts - 1) / parts;
    chunk = (chunk + align - 1) / align * align;
    *b = std::min(n, part * chunk);
    *e = std::min(n, *b + chunk);
}

struct Pipeline {
    std::string triggerName;
    int triggerSem = DAO_SEM_AUTO;
    bool triggerSpin = false;
    Workers workers;
    std::map<std::string, IMAGE *> shms;
    std::vector<Stage> stages;
    std::vector<int> producer;                       // stage writing each stage's input, or -1
    std::vector<char> pub;                           // its output was published this frame
    std::vector<std::vector<float>> zeros;           // offsets of integrators without one

    IMAGE *shm(const std::string &name)
    {
        auto it = shms.find(name);
        if (it != shms.end())
            return it->second;
        IMAGE *im = (IMAGE *) calloc(1, sizeof(IMAGE));
        if (daoShmOpen(name.c_str(), im) != DAO_SUCCESS) {
            free(im);
            throw std::runtime_error("cannot open " + name);
        }
        shms[name] = im;
        if (daoShmIsGpu(im))
            throw std::runtime_error(name + " is a GPU SHM: daoCpuPipeline works on host SHMs");
        return im;
    }
};

static std::string need(const YAML::Node &n, const char *key, const std::string &stage)
{
    if (!n[key])
        throw std::runtime_error(stage + ": missing '" + key + "'");
    return n[key].as<std::string>();
}

template <typename T>
static T num(const YAML::Node &n, const char *key, const std::string &stage, const T *dflt = nullptr)
{
    if (!n[key]) {
        if (dflt)
            return *dflt;
        throw std::runtime_error(stage + ": missing '" + key + "'");
    }
    return n[key].as<T>();
}

/* an SHM of this type (-1: any) with at least this many values */
static void check(IMAGE *im, const std::string &stage, int atype, long minValues)
{
    if (atype >= 0 && im->md[0].atype != atype)
        throw std::runtime_error(stage + ": " + im->name + " has data type " + std::to_string(im->md[0].atype)
                                 + ", " + std::to_string(atype) + " expected");
    if (daoToolsShmValues(im) < minValues)
        throw std::runtime_error(stage + ": " + im->name + " has " + std::to_string(daoToolsShmValues(im))
                                 + " values, at least " + std::to_string(minValues) + " needed");
}

/* the minimum flux SHM's value, read every frame (as the apps); no SHM: 0, no minimum */
static double minFlux(const IMAGE *im)
{
    if (!im)
        return 0.0;
    return im->md[0].atype == _DATATYPE_DOUBLE ? im->array.D[0] : im->array.F[0];
}

/* a loop / enable flag, of any integer or float type */
static double scalar(const IMAGE *im)
{
    switch (im->md[0].atype) {
    case _DATATYPE_FLOAT: return im->array.F[0];
    case _DATATYPE_DOUBLE: return im->array.D[0];
    case _DATATYPE_UINT32: return im->array.UI32[0];
    case _DATATYPE_INT32: return im->array.SI32[0];
    case _DATATYPE_UINT8: return im->array.UI8[0];
    default: return 0;
    }
}

static size_t elemSize(uint8_t atype)
{
    switch (atype) {
    case _DATATYPE_UINT8: case _DATATYPE_INT8: return 1;
    case _DATATYPE_UINT16: case _DATATYPE_INT16: return 2;
    case _DATATYPE_UINT64: case _DATATYPE_INT64: case _DATATYPE_DOUBLE: return 8;
    default: return 4;
    }
}

static int nbOnes(const IMAGE *mask, long n)
{
    int c = 0;
    for (long k = 0; k < n; k++)
        c += mask->array.UI32[k] == 1;
    return c;
}

/* A stage split over the workers by sub-aperture (or window): each share runs the
 * library function on its own sub-apertures. The arrays with one row of n values per
 * quantity (x of every sub-aperture, then y, ...) are gathered into the share's
 * buffers, and its output rows scattered back. One share: the full arrays, as is. */
struct Share {
    long b = 0, e = 0;                               // its sub-apertures
    std::vector<float> ref, centre, out;             // gathered rows, its output rows
};

static std::shared_ptr<std::vector<Share>> makeShares(long n, int parts, int rowsOut, bool centre, long align = 1)
{
    auto v = std::make_shared<std::vector<Share>>(parts);
    for (int k = 0; k < parts; k++) {
        Share &q = (*v)[k];
        share(n, k, parts, align, &q.b, &q.e);
        if (parts > 1) {
            q.ref.resize(2 * (q.e - q.b));
            q.centre.resize(centre ? 2 * (q.e - q.b) : 0);
            q.out.resize(rowsOut * (q.e - q.b));
        }
    }
    return v;
}

static void gatherRows(const float *src, long n, int rows, long b, long e, float *dst)
{
    for (int r = 0; r < rows; r++)
        memcpy(dst + r * (e - b), src + r * n + b, (e - b) * sizeof(float));
}

static void scatterRows(const float *src, long n, int rows, long b, long e, float *dst)
{
    for (int r = 0; r < rows; r++)
        memcpy(dst + r * n + b, src + r * (e - b), (e - b) * sizeof(float));
}

/* the shares of a stage: the workers', unless it says parallel: false */
static int partsOf(Pipeline &p, const YAML::Node &a)
{
    return a["parallel"] && !a["parallel"].as<bool>() ? 1 : p.workers.parts;
}

typedef std::shared_ptr<std::function<void(int, int)>> Job;

/* ---------------------------------------------------------------- plugins */
/* A plugin stage's arguments (daoCpuStageArgs): its YAML keys, the pipeline's SHMs. */
struct PluginArgs {
    Pipeline *p;
    YAML::Node node;
    std::map<std::string, std::string> values;       // keeps the returned strings alive
};

static const char *pluginValue(void *ctx, const char *key)
{
    PluginArgs *a = (PluginArgs *) ctx;
    if (!key || !a->node[key])
        return nullptr;
    std::string v = a->node[key].IsScalar() ? a->node[key].as<std::string>() : YAML::Dump(a->node[key]);
    return (a->values[key] = v).c_str();
}

static IMAGE *pluginShm(void *ctx, const char *key)
{
    const char *name = pluginValue(ctx, key);
    try {
        return name ? ((PluginArgs *) ctx)->p->shm(name) : nullptr;
    } catch (const std::exception &e) {
        daoError("%s\n", e.what());
        return nullptr;
    }
}

/* plugins: [..]: each library registers its stage types */
static void loadPlugins(const YAML::Node &cfg)
{
    if (!cfg["plugins"])
        return;
    for (const auto &item : cfg["plugins"]) {
        std::string path = item.as<std::string>();
        void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            throw std::runtime_error("plugin " + path + ": " + dlerror());
        auto reg = (void (*)(void)) dlsym(h, "daoCpuPluginRegister");
        if (!reg)
            throw std::runtime_error("plugin " + path + ": no daoCpuPluginRegister() (a daoGpuPipeline plugin?)");
        reg();
        daoInfo("plugin %s loaded\n", path.c_str());
    }
}

/* A stage of a registered type: its factory says what it reads and writes and does;
 * the pipeline publishes its output. */
static Stage pluginStage(Pipeline &p, const std::string &type, const YAML::Node &a, daoCpuStageFactory factory)
{
    PluginArgs pa{&p, a, {}};
    daoCpuStageArgs args{DAO_CPU_STAGE_API, type.c_str(), &pa, pluginValue, pluginShm};
    daoCpuStageDesc d;
    memset(&d, 0, sizeof d);
    if (factory(&args, &d) != 0)
        throw std::runtime_error(type + ": could not be created");
    if (!d.in || !d.out || !d.run)
        throw std::runtime_error(type + ": its factory gave no in, out or run");
    Stage s;
    s.type = type;
    s.in = d.in;
    s.out = d.out;
    s.run = [d]() {
        int r = d.run(d.self);
        if (r < 0)
            return -1;
        if (r == 0)
            return 0;
        if (d.cnt2FromIn)
            d.out->md[0].cnt2 = d.in->md[0].cnt2;
        daoShmSetDataPartFinalize(d.out);
        return 1;
    };
    if (d.post)
        s.post = [d]() { d.post(d.self); };
    if (d.destroy)
        s.destroy = [d]() { d.destroy(d.self); };
    return s;
}

static bool builtIn(const std::string &type)
{
    static const char *const types[] = {
        "pixelCalibrate", "pixelExtract", "pixelSubstractExtract", "centroid", "centroidRelative",
        "centroidRelativeRef", "centroidCorrelation", "centroidCorrelationFFT", "centroidWindows", "mvm",
        "applyGain", "leakyIntegrator", "slice",
        // daoGpuPipeline's, not here yet (a clear message below)
        "calIntensity", "calIntensityNorm", "pixelSubstractExtractNorm", "pixelSubstractExtractNormImage",
        "descrambleOcam2"};
    for (const char *t : types)
        if (type == t)
            return true;
    return false;
}

static Stage makeStage(Pipeline &p, const std::string &type, const YAML::Node &a)
{
    if (!builtIn(type)) {                            // built-in types first, then the plugins'
        if (daoCpuStageFactory factory = daoCpuFindStage(type.c_str()))
            return pluginStage(p, type, a, factory);
        throw std::runtime_error("unknown stage type '" + type + "' (a plugin missing under plugins:?)");
    }
    Workers *W = &p.workers;
    auto shm = [&](const char *key) { return p.shm(need(a, key, type)); };
    auto optShm = [&](const char *key) { return a[key] ? shm(key) : (IMAGE *) nullptr; };
    const int F = _DATATYPE_FLOAT;
    const bool no = false;
    const float zero = 0.f;
    Stage s;
    s.type = type;
    IMAGE *in = s.in = shm("in");
    IMAGE *out = s.out = shm("out");

    if (type == "pixelCalibrate") {                  // daoPixelCalibrate
        IMAGE *ff = shm("ff"), *bg = shm("bg");
        long n = (long) out->md[0].size[0] * out->md[0].size[1];
        bool isFloat = out->md[0].atype == F;
        if (!isFloat)
            check(out, type, _DATATYPE_DOUBLE, n);
        check(in, type, -1, n);
        check(ff, type, isFloat ? F : _DATATYPE_DOUBLE, n);
        check(bg, type, isFloat ? F : _DATATYPE_DOUBLE, n);
        s.run = [=]() {
            if (isFloat)
                daoToolsShmCalibrate(in, ff, bg, out);   // publishes
            else
                daoToolsShmCalibrate64(in, ff, bg, out);
            return 1;
        };
    } else if (type == "pixelExtract" || type == "pixelSubstractExtract") {   // daoPixelExtract, ...
        bool sub = type == "pixelSubstractExtract";
        IMAGE *mask = shm("mask"), *bgShm = sub ? shm("sub") : nullptr, *norm = sub ? optShm("norm") : nullptr;
        long n = (long) in->md[0].size[0] * in->md[0].size[1];
        check(mask, type, _DATATYPE_UINT32, n);
        check(out, type, in->md[0].atype, nbOnes(mask, n));
        if (bgShm)
            check(bgShm, type, in->md[0].atype, n);
        if (norm)
            check(norm, type, in->md[0].atype, 1);
        uint64_t maskCnt = mask->md[0].cnt0;
        std::string t = type;
        s.run = [=]() mutable {
            if (mask->md[0].cnt0 != maskCnt) {           // a new mask: does it still fit?
                maskCnt = mask->md[0].cnt0;
                if (nbOnes(mask, n) > daoToolsShmValues(out)) {
                    daoError("%s: %d pixels in the new mask, %s has %ld\n", t.c_str(), nbOnes(mask, n), out->name,
                             daoToolsShmValues(out));
                    return -1;
                }
            }
            if (!sub)
                daoToolsShmExtract(in, mask, out);   // publishes
            else if (daoToolsShmSubstractExtractFinalize(in, bgShm, mask, out, norm, norm != nullptr) != DAO_SUCCESS)
                return -1;
            return 1;
        };
    } else if (type == "centroid" || type == "centroidRelative" || type == "centroidRelativeRef") {
        int boxSize = num<int>(a, "subaSize", type), nSuba = num<int>(a, "nbSuba", type);
        IMAGE *ref = shm("ref"), *thr = shm("threshold"), *mf = optShm("minFlux");
        IMAGE *centre = type == "centroidRelativeRef" ? shm("subApCentre") : nullptr;
        long nx = in->md[0].size[1], ny = in->md[0].size[0];
        check(in, type, F, nx * ny);
        check(ref, type, F, 2L * nSuba);
        if (centre)
            check(centre, type, F, 2L * nSuba);
        check(thr, type, F, 1);
        int kind = type == "centroid" ? 0 : type == "centroidRelative" ? 1 : 2;
        const int rows = kind ? 4 : 3;               // x, y, flux (, weight: the relative ones)
        check(out, type, F, (long) rows * nSuba);
        int parts = partsOf(p, a);
        // the fast relative centroider computes 8 sub-apertures at a time (SIMD lanes)
        auto sh = makeShares(nSuba, parts, rows, centre != nullptr, kind == 1 ? 8 : 1);
        Job job = std::make_shared<std::function<void(int, int)>>([=](int part, int) {
            Share &q = (*sh)[part];
            int k = (int) (q.e - q.b);
            if (k == 0)
                return;
            float t = thr->array.F[0], m = (float) minFlux(mf);
            const float *r = ref->array.F, *c = centre ? centre->array.F : nullptr;
            float *o = out->array.F;
            if (parts > 1) {
                gatherRows(r, nSuba, 2, q.b, q.e, q.ref.data());
                r = q.ref.data();
                if (c) {
                    gatherRows(c, nSuba, 2, q.b, q.e, q.centre.data());
                    c = q.centre.data();
                }
                o = q.out.data();
            }
            if (kind == 0)                           // daoComputeCentroid
                daoCentroidSpots(in->array.F, in->md[0].size[0], (float *) r, boxSize, k, t, m, o);
            else if (kind == 1)                      // daoComputeCentroidRelative
                daoCentroidSpotsRelativeFast(in->array.F, nx, ny, (float *) r, boxSize, k, t, m, o);
            else                                     // daoComputeCentroidRelativeRef
                daoCentroidSpotsRelativeRef(in->array.F, nx, ny, (float *) c, (float *) r, boxSize, k, t, m, o);
            if (parts > 1)
                scatterRows(o, nSuba, rows, q.b, q.e, out->array.F);
        });
        s.run = [=]() {
            out->md[0].cnt2 = in->md[0].cnt2;
            W->run(*job, parts);
            daoShmSetDataPartFinalize(out);
            return 1;
        };
    } else if (type == "centroidCorrelation" || type == "centroidCorrelationFFT") {
        bool fft = type == "centroidCorrelationFFT";
        int boxSize = num<int>(a, "subaSize", type), nSuba = num<int>(a, "nbSuba", type);
        int range = fft ? 0 : num<int>(a, "searchRange", type);
        float alpha = num<float>(a, "alpha", type, &zero);
        IMAGE *centre = shm("subApCentre"), *refImage = shm("refImage"), *thr = shm("threshold"),
              *mf = optShm("minFlux");
        long nx = in->md[0].size[1], ny = in->md[0].size[0];
        check(in, type, F, nx * ny);
        check(centre, type, F, 2L * nSuba);
        check(refImage, type, F, (long) nSuba * boxSize * boxSize);
        check(thr, type, F, 1);
        check(out, type, F, 3L * nSuba);
        if (!fft && (range < 1 || range > DAO_CENTROID_CORR_MAX_SEARCH_RANGE))
            throw std::runtime_error(type + ": searchRange 1 to "
                                     + std::to_string(DAO_CENTROID_CORR_MAX_SEARCH_RANGE));
        int parts = partsOf(p, a);
        auto sh = makeShares(nSuba, parts, 3, false);
        const long box2 = (long) boxSize * boxSize;  // one reference image per sub-aperture, stacked
        if (!fft) {                                  // daoComputeCentroidCorrelation
            Job job = std::make_shared<std::function<void(int, int)>>([=](int part, int) {
                Share &q = (*sh)[part];
                int k = (int) (q.e - q.b);
                if (k == 0)
                    return;
                const float *c = centre->array.F;
                float *o = out->array.F;
                if (parts > 1) {
                    gatherRows(c, nSuba, 2, q.b, q.e, q.ref.data());
                    c = q.ref.data();
                    o = q.out.data();
                }
                daoCentroidSpotsCorrelation(in->array.F, nx, ny, (float *) c, refImage->array.F + q.b * box2, boxSize,
                                            k, range, thr->array.F[0], (float) minFlux(mf), o);
                if (parts > 1)
                    scatterRows(o, nSuba, 3, q.b, q.e, out->array.F);
            });
            s.run = [=]() {
                out->md[0].cnt2 = in->md[0].cnt2;
                W->run(*job, parts);
                daoShmSetDataPartFinalize(out);
                return 1;
            };
            if (alpha > 0)                           // threshold 0, as the app (see there)
                s.post = [=]() {
                    daoCentroidSpotsUpdateReference(in->array.F, nx, ny, centre->array.F, out->array.F, boxSize,
                                                    nSuba, 0.0f, alpha, (float) minFlux(mf), refImage->array.F);
                    daoShmSetDataPartFinalize(refImage);
                };
        } else {                                     // daoComputeCentroidCorrelationFFT
#ifndef DAO_CPU_PIPELINE_FFT
            throw std::runtime_error(type + ": libdaoTools was built without FFTW");
#else
            // a context per share holds its references' spectra: made again (by the loop
            // thread: FFTW plans are made one at a time) when another process writes a
            // new reference image, not after our own update
            struct Fft {
                std::vector<daoCentroidCorrFFTCtx *> ctx;
                uint64_t cnt = 0;
                bool make(const float *refImg, int boxSize, long box2, const std::vector<Share> &sh)
                {
                    std::vector<daoCentroidCorrFFTCtx *> c(sh.size(), nullptr);
                    bool ok = true;
                    for (size_t k = 0; k < sh.size(); k++)
                        if (sh[k].e > sh[k].b) {
                            c[k] = daoCentroidSpotsCorrelationFFTInit(boxSize, (int) (sh[k].e - sh[k].b),
                                                                      refImg + sh[k].b * box2);
                            ok = ok && c[k];
                        }
                    if (ok)
                        std::swap(c, ctx);
                    release(c);
                    return ok;
                }
                static void release(std::vector<daoCentroidCorrFFTCtx *> &c)
                {
                    for (daoCentroidCorrFFTCtx *x : c)
                        if (x)
                            daoCentroidSpotsCorrelationFFTFree(x);
                    c.clear();
                }
            };
            auto f = std::make_shared<Fft>();
            if (!f->make(refImage->array.F, boxSize, box2, *sh))
                throw std::runtime_error(type + ": cannot set up the FFTs");
            f->cnt = refImage->md[0].cnt0;
            Job job = std::make_shared<std::function<void(int, int)>>([=](int part, int) {
                Share &q = (*sh)[part];
                if (q.e == q.b)
                    return;
                const float *c = centre->array.F;
                float *o = out->array.F;
                if (parts > 1) {                     // kept for the reference update (post)
                    gatherRows(c, nSuba, 2, q.b, q.e, q.ref.data());
                    c = q.ref.data();
                    o = q.out.data();
                }
                daoCentroidSpotsCorrelationFFT(f->ctx[part], in->array.F, nx, ny, c, thr->array.F[0],
                                               (float) minFlux(mf), o);
                if (parts > 1)
                    scatterRows(o, nSuba, 3, q.b, q.e, out->array.F);
            });
            s.run = [=]() {
                if (refImage->md[0].cnt0 != f->cnt) {
                    f->cnt = refImage->md[0].cnt0;
                    if (!f->make(refImage->array.F, boxSize, box2, *sh)) {
                        daoError("%s: new reference image: cannot set up the FFTs\n", out->name);
                        return -1;
                    }
                }
                out->md[0].cnt2 = in->md[0].cnt2;
                W->run(*job, parts);
                daoShmSetDataPartFinalize(out);
                return 1;
            };
            if (alpha > 0)
                s.post = [=]() {
                    for (int k = 0; k < parts; k++) {
                        const Share &q = (*sh)[k];
                        if (q.e == q.b)
                            continue;
                        daoCentroidSpotsCorrelationFFTUpdateRef(
                            f->ctx[k], in->array.F, nx, ny, parts > 1 ? q.ref.data() : centre->array.F,
                            parts > 1 ? q.out.data() : out->array.F, 0.0f, alpha, (float) minFlux(mf),
                            refImage->array.F + q.b * box2);
                    }
                    daoShmSetDataPartFinalize(refImage);
                    f->cnt = refImage->md[0].cnt0;
                };
            s.destroy = [=]() { Fft::release(f->ctx); };
#endif
        }
    } else if (type == "centroidWindows") {          // daoComputeCentroidWindows
        static const std::map<std::string, int> methods = {
            {"correlation", DAO_WINDOWS_CORRELATION}, {"cog", DAO_WINDOWS_COG},
            {"correlationNormalized", DAO_WINDOWS_CORRELATION_NORMALIZED}};
        static const std::map<std::string, int> peaks = {
            {"max", DAO_PEAK_MAX}, {"parabola", DAO_PEAK_PARABOLA}, {"barycenter", DAO_PEAK_BARYCENTER},
            {"barycenterThreshold", DAO_PEAK_BARYCENTER_THRESHOLD},
            {"barycenterThresholdWeighted", DAO_PEAK_BARYCENTER_THRESHOLD_WEIGHTED}};
        std::string m = a["method"] ? a["method"].as<std::string>() : "correlation";
        std::string k = a["peak"] ? a["peak"].as<std::string>() : (m == "cog" ? "barycenterThreshold" : "parabola");
        if (!methods.count(m) || !peaks.count(k))
            throw std::runtime_error(type + ": method correlation | correlationNormalized | cog, peak max | "
                                     "parabola | barycenter | barycenterThreshold | barycenterThresholdWeighted");
        int method = methods.at(m), peak = peaks.at(k);
        IMAGE *table = shm("table"), *refImage = optShm("refImage"), *thr = shm("threshold"),
              *mf = optShm("minFlux");
        int nx = in->md[0].size[1], ny = in->md[0].size[0];
        int nWin = (int) (table->md[0].nelement / DAO_WINDOW_COLS);
        if (nWin < 1 || table->md[0].nelement % DAO_WINDOW_COLS)
            throw std::runtime_error(type + ": " + table->name + " is not a table of "
                                     + std::to_string(DAO_WINDOW_COLS) + " values per window");
        if (method != DAO_WINDOWS_COG && !refImage)
            throw std::runtime_error(type + ": a correlation needs refImage");
        int refW = 0, refH = 0;
        if (refImage) {
            refW = refImage->md[0].size[1];
            refH = (int) (refImage->md[0].size[0] / nWin);
            check(refImage, type, F, (long) nWin * refW * refH);
        }
        check(in, type, F, (long) nx * ny);
        check(table, type, F, (long) nWin * DAO_WINDOW_COLS);
        check(thr, type, F, 1);
        check(out, type, F, 3L * nWin);
        if (daoCentroidWindowsCheck(table->array.F, nWin, nx, ny, refW, refH, method, peak) != DAO_SUCCESS
            && method == DAO_WINDOWS_COG && peak < DAO_PEAK_BARYCENTER)
            throw std::runtime_error(type + ": the peak estimator does not apply to cog");
        uint64_t tableCnt = table->md[0].cnt0;
        int parts = partsOf(p, a);
        auto sh = makeShares(nWin, parts, 3, false);  // a share: rows of the table, their references
        const long refSize = (long) refW * refH;
        Job job = std::make_shared<std::function<void(int, int)>>([=](int part, int) {
            Share &q = (*sh)[part];
            int k = (int) (q.e - q.b);
            if (k == 0)
                return;
            float *o = parts > 1 ? q.out.data() : out->array.F;
            daoCentroidWindows(in->array.F, nx, ny, table->array.F + q.b * DAO_WINDOW_COLS, k,
                               refImage ? refImage->array.F + q.b * refSize : nullptr, refW, refH, method, peak,
                               thr->array.F[0], (float) minFlux(mf), o);
            if (parts > 1)
                scatterRows(o, nWin, 3, q.b, q.e, out->array.F);
        });
        s.run = [=]() mutable {
            if (table->md[0].cnt0 != tableCnt) {         // new windows: say which do not fit
                tableCnt = table->md[0].cnt0;
                daoCentroidWindowsCheck(table->array.F, nWin, nx, ny, refW, refH, method, peak);
            }
            out->md[0].cnt2 = in->md[0].cnt2;
            W->run(*job, parts);
            daoShmSetDataPartFinalize(out);
            return 1;
        };
    } else if (type == "mvm") {                      // daoMvM: y = M x, M row-major [nOut x nIn]
        IMAGE *matrix = shm("matrix");
        int nIn = matrix->md[0].size[1], nOut = matrix->md[0].size[0];
        check(matrix, type, F, (long) nIn * nOut);
        check(in, type, F, nIn);
        check(out, type, F, nOut);
        int parts = partsOf(p, a);                   // a share: rows of the matrix
        Job job = std::make_shared<std::function<void(int, int)>>([=](int part, int n) {
            long b, e;
            share(nOut, part, n, 4, &b, &e);
            if (e > b)
                cblas_sgemv(CblasRowMajor, CblasNoTrans, (int) (e - b), nIn, 1.0f, matrix->array.F + b * nIn, nIn,
                            in->array.F, 1, 0.0f, out->array.F + b, 1);
        });
        s.run = [=]() {
            W->run(*job, parts);
            daoShmSetDataPartFinalize(out);
            return 1;
        };
    } else if (type == "applyGain") {                // daoApplyGain
        bool modal = num<bool>(a, "modal", type, &no);
        IMAGE *gain = shm("gain");
        long n = (long) in->md[0].size[0] * in->md[0].size[1];
        check(in, type, F, n);
        check(out, type, F, n);
        check(gain, type, F, modal ? n : 1);
        s.run = [=]() {
            out->md[0].cnt2 = in->md[0].cnt2;
            const float *x = in->array.F, *g = gain->array.F;
            float *y = out->array.F;
            if (modal)
                for (long k = 0; k < n; k++)
                    y[k] = x[k] * g[k];
            else
                for (long k = 0; k < n; k++)
                    y[k] = x[k] * g[0];
            daoShmSetDataPartFinalize(out);
            return 1;
        };
    } else if (type == "leakyIntegrator") {          // daoLeakyIntegrator
        bool modal = num<bool>(a, "modal", type, &no), keepPiston = num<bool>(a, "keepPiston", type, &no);
        const double tenDefault = 10.0;              // daoLeakyIntegrator's default
        double clip = num<double>(a, "clip", type, &tenDefault);
        IMAGE *loop = shm("loop"), *gain = shm("gain"), *leak = shm("leak"), *enable = optShm("enable"),
              *offset = optShm("offset");
        int n = (int) ((long) in->md[0].size[0] * in->md[0].size[1]);
        check(in, type, F, n);
        check(out, type, F, n);
        check(gain, type, F, modal ? n : 1);
        check(leak, type, F, modal ? n : 1);
        float *off;
        if (offset) {
            check(offset, type, F, n);
            off = offset->array.F;
        } else {
            p.zeros.emplace_back(n, 0.f);
            off = p.zeros.back().data();
        }
        auto zeroSent = std::make_shared<int>(0);
        s.run = [=]() {
            float *y = out->array.F;
            out->md[0].cnt2 = in->md[0].cnt2;
            if (enable && scalar(enable) != 1) {     // disabled: zeros once, then silent
                if (*zeroSent)
                    return 0;
                memset(y, 0, n * sizeof(float));
                *zeroSent = 1;
            } else if (scalar(loop) == 1) {          // closed loop
                *zeroSent = 0;
                if (modal)
                    daoToolsLeakyModalIntegrator(in->array.F, n, leak->array.F, gain->array.F, off, y);
                else
                    daoToolsLeakyIntegrator(in->array.F, n, leak->array.F[0], gain->array.F[0], off, y);
                float avg = 0;                       // float, as the app
                for (int j = 0; j < n; j++)
                    avg += y[j];
                avg = avg / n;
                if (keepPiston)
                    avg = 0;
                for (int j = 0; j < n; j++) {
                    y[j] = y[j] - avg;
                    if (y[j] > clip)
                        y[j] = clip;
                    else if (y[j] < -clip)
                        y[j] = -clip;
                }
            } else {                                 // open loop, enabled: zeros every frame
                *zeroSent = 0;
                memset(y, 0, n * sizeof(float));
            }
            daoShmSetDataPartFinalize(out);
            return 1;
        };
    } else if (type == "slice") {                    // daoShmSlice
        const long zeroL = 0, all = -1;
        long offset = num<long>(a, "offset", type, &zeroL), count = num<long>(a, "count", type, &all);
        if (count < 0)
            count = daoToolsShmValues(out);
        check(out, type, in->md[0].atype, count);
        check(in, type, in->md[0].atype, offset + count);
        size_t es = elemSize(in->md[0].atype);
        s.run = [=]() {
            out->md[0].cnt2 = in->md[0].cnt2;
            memcpy(out->array.V, (const char *) in->array.V + offset * es, count * es);
            daoShmSetDataPartFinalize(out);
            return 1;
        };
    } else if (type == "calIntensity" || type == "calIntensityNorm" || type == "pixelSubstractExtractNorm"
               || type == "pixelSubstractExtractNormImage" || type == "descrambleOcam2")
        throw std::runtime_error(type + ": not in daoCpuPipeline yet (run its daoTools app as a process)");
    else
        throw std::runtime_error("unknown stage type '" + type + "'");
    return s;
}

static void build(Pipeline &p, const YAML::Node &cfg)
{
    loadPlugins(cfg);
    for (const char *k : {"device", "hostAccess", "stageTimestamps"})
        if (cfg[k])
            daoInfo("%s: ignored (daoGpuPipeline only)\n", k);
    for (const auto &item : cfg["stages"]) {
        if (!item.IsMap() || item.size() != 1)
            throw std::runtime_error("each stage is a map with one key (the stage type)");
        std::string type = item.begin()->first.as<std::string>();
        Stage s = makeStage(p, type, item.begin()->second);
        int from = -1;                               // the last stage before this one writing its input
        for (size_t k = 0; k < p.stages.size(); k++)
            if (p.stages[k].out == s.in)
                from = (int) k;
        p.producer.push_back(from);
        daoInfo("stage %zu: %s, %s -> %s\n", p.stages.size(), type.c_str(), s.in->name, s.out->name);
        p.stages.push_back(s);
    }
    if (p.stages.empty())
        throw std::runtime_error("no stages");
    p.pub.assign(p.stages.size(), 0);
}

/* "6,8,10-12" -> 6 8 10 11 12 */
static std::vector<int> cpuList(const std::string &s)
{
    std::vector<int> v;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i);
        std::string part = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
        size_t dash = part.find('-');
        int a = std::stoi(part.substr(0, dash)), b = dash == std::string::npos ? a : std::stoi(part.substr(dash + 1));
        for (int c = a; c <= b; c++)
            v.push_back(c);
        if (j == std::string::npos)
            break;
        i = j + 1;
    }
    return v;
}

static int run(const char *configPath, int cpu, bool spin, int blasThreads, bool profile, const std::string &workers)
{
    YAML::Node cfg = YAML::LoadFile(configPath);
    Pipeline p;
    p.triggerName = need(cfg["trigger"], "shm", "trigger");
    p.triggerSem = cfg["trigger"]["sem"] ? cfg["trigger"]["sem"].as<int>() : DAO_SEM_AUTO;
    p.triggerSpin = spin;
    if (cfg["trigger"]["wait"]) {
        std::string mode = cfg["trigger"]["wait"].as<std::string>();
        if (mode != "semaphore" && mode != "spin")
            throw std::runtime_error("trigger wait: 'semaphore' or 'spin'");
        p.triggerSpin = p.triggerSpin || mode == "spin";
    }
    if (blasThreads < 0)                             // no -N
        blasThreads = cfg["blasThreads"] ? cfg["blasThreads"].as<int>() : 1;
    if (cpu >= 0) {                                  // the loop thread (BLAS's threads keep the process's CPUs)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0)
            daoWarning("could not pin to CPU %d\n", cpu);
    }
    if (blasThreads > 0)
        openblas_set_num_threads(blasThreads);
    daoToolsEnableFTZ();                             // no subnormal stalls in the loop

    std::vector<int> workerCpus = cpuList(workers);  // -w, else workers: [..]
    if (workerCpus.empty() && cfg["workers"])
        for (const auto &c : cfg["workers"])
            workerCpus.push_back(c.as<int>());
    for (int c : workerCpus) {
        if (c == cpu)
            throw std::runtime_error("worker CPU " + std::to_string(c) + " is the loop's (-C)");
        if (c < 0 || c >= CPU_SETSIZE)
            throw std::runtime_error("worker CPU " + std::to_string(c) + "?");
    }
    if (!workerCpus.empty()) {
        if (blasThreads != 1)
            daoWarning("workers with %d BLAS threads: each share of an mvm starts BLAS threads of its own\n",
                       blasThreads);
        p.workers.start(workerCpus);
        std::string list;
        for (int c : workerCpus)
            list += (list.empty() ? "" : ",") + std::to_string(c);
        daoInfo("%zu workers, on CPUs %s (spinning between frames)\n", workerCpus.size(), list.c_str());
    }

    build(p, cfg);
    IMAGE *trigger = p.shm(p.triggerName);
    std::string waitHow = p.triggerSpin ? "spinning on its cnt0"
                                        : "semaphore " + std::to_string(p.triggerSem >= 0 ? p.triggerSem
                                                                                         : daoShmClaimSem(trigger));
    daoInfo("%zu stages, triggered by %s (%s)%s\n", p.stages.size(), p.triggerName.c_str(), waitHow.c_str(),
            blasThreads > 0 ? (", " + std::to_string(blasThreads) + " BLAS thread(s)").c_str() : "");

    daoToolsOnExitSignals(onSignal);                 // Ctrl+C, kill, tmux kill-session
    const size_t ns = p.stages.size();
    std::vector<double> accStage(ns, 0);
    double acc = 0, worst = 0, accWake = 0, tPrint = nowUs();
    long n = 0, waits = 0;
    uint64_t lastCnt = trigger->md[trigger->md[0].fifo_last_written].cnt0;   // -W: the next frame
    while (!stop) {
        int w = p.triggerSpin ? spinWait(trigger, &lastCnt, 1.0) : daoToolsWait(trigger, p.triggerSem, 1.0);
        if (w != DAO_SUCCESS) {
            printf("\rwaiting for %s (%ld s)                    ", p.triggerName.c_str(), ++waits);
            fflush(stdout);
            tPrint = nowUs();
            acc = worst = accWake = 0;
            std::fill(accStage.begin(), accStage.end(), 0);
            n = 0;
            continue;
        }
        waits = 0;
        double t0 = nowUs();
        {   // wake-up delay: trigger timestamp (CLOCK_REALTIME) to now
            struct timespec rt;
            clock_gettime(CLOCK_REALTIME, &rt);
            const struct timespec &ts = trigger->md[0].atime.ts;
            accWake += ((double) (rt.tv_sec - ts.tv_sec) * 1e9 + (double) (rt.tv_nsec - ts.tv_nsec)) * 1e-3;
        }
        double t = t0;
        bool ok = true;
        for (size_t k = 0; k < ns; k++) {
            int from = p.producer[k];
            if (from >= 0 && !p.pub[from]) {         // its input's stage is silent: nothing new
                p.pub[k] = 0;
                continue;
            }
            int r = p.stages[k].run();
            if (r < 0) {                             // the stage said why; the rest of the frame is skipped
                ok = false;
                for (size_t j = k; j < ns; j++)
                    p.pub[j] = 0;
                break;
            }
            p.pub[k] = r > 0;
            if (profile) {
                double tk = nowUs();
                accStage[k] += tk - t;
                t = tk;
            }
        }
        double dt = nowUs() - t0;
        for (size_t k = 0; k < ns && ok; k++)        // after the frame: reference updates
            if (p.pub[k] && p.stages[k].post)
                p.stages[k].post();
        acc += dt;
        worst = dt > worst ? dt : worst;
        n++;
        if (nowUs() - tPrint > 1e6) {
            printf("\rpipeline: avg %.1f us  max %.1f us  %.1f Hz  [wake %.1f us]      ", acc / n, worst,
                   n * 1e6 / (nowUs() - tPrint), accWake / n);
            if (profile) {
                printf("\n  us per stage:");
                for (size_t k = 0; k < ns; k++) {
                    printf(" %s %.1f", p.stages[k].type.c_str(), accStage[k] / n);
                    accStage[k] = 0;
                }
                printf("\n");
            }
            fflush(stdout);
            acc = worst = accWake = 0;
            n = 0;
            tPrint = nowUs();
        }
    }
    printf("\n");
    p.workers.stop();
    for (Stage &s : p.stages)
        if (s.destroy)
            s.destroy();
    for (auto &kv : p.shms) {
        daoShmClose(kv.second);
        free(kv.second);
    }
    return 0;
}

static void help(const char *argv0)
{
    printf("usage: %s -c <config.yaml> [-C <cpu>] [-W] [-w <cpus>] [-N <n>] [-p] [-d <level>] -L\n"
           "  -h  display this message and exit\n"
           "  -c  pipeline configuration (see the header of daoCpuPipeline.cpp);\n"
           "      plugins: CPU stage plugins (daoCpuStages.h)\n"
           "  -C  pin the loop thread to this CPU core\n"
           "  -W  wait for the trigger by polling its cnt0, not on a semaphore\n"
           "      (also trigger: {..., wait: spin})\n"
           "  -w  workers on these CPUs (e.g. 6,8 or 6-9; also workers: [6, 8]): each runs a share of\n"
           "      the centroiders and mvm, and spins between frames (a core of its own each)\n"
           "  -N  BLAS threads for mvm (also blasThreads:); default 1, 0: the library's default\n"
           "      (its threads are real time too and spin between calls: give them cores of their own)\n"
           "  -p  print the time of each stage every second\n"
           "  -d  log level: 0 warnings and errors (default), 1 info, 2 debug, 3 trace\n"
           "  -L  run\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *config = nullptr;
    int cpu = -1, go = 0, blasThreads = -1;
    bool spin = false, profile = false;
    std::string workers;
    int left = argc - 1;
    char **arg = argv + 1;
    while (left-- > 0)
    {
        const char *opt = *arg++;
        if (!strcmp(opt, "-c"))
        {
            config = daoToolsArgValue(&left, &arg, opt);
        }
        else if (!strcmp(opt, "-C"))
        {
            cpu = (int)daoToolsArgInt(&left, &arg, opt);
        }
        else if (!strcmp(opt, "-N"))
        {
            blasThreads = (int)daoToolsArgInt(&left, &arg, opt);
        }
        else if (!strcmp(opt, "-d"))
        {
            daoLogLevel = (int)daoToolsArgInt(&left, &arg, opt);
            daoSetLogLevel(daoLogLevel);
        }
        else if (!strcmp(opt, "-W"))
        {
            spin = true;
        }
        else if (!strcmp(opt, "-w"))
        {
            workers = daoToolsArgValue(&left, &arg, opt);
        }
        else if (!strcmp(opt, "-p"))
        {
            profile = true;
        }
        else if (!strcmp(opt, "-L"))
        {
            go = 1;
        }
        else
        {
            help(argv[0]);
            return !strcmp(opt, "-h") ? 0 : 2;
        }
    }
    if (!config || !go) {
        help(argv[0]);
        return 2;
    }
    daoToolsSetRtPriority(93);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        daoWarning("mlockall failed: run scripts/daoToolSetCap to grant RT capabilities\n");
    try {
        return run(config, cpu, spin, blasThreads, profile, workers);
    } catch (const std::exception &e) {
        daoError("%s\n", e.what());
        return 1;
    }
}
