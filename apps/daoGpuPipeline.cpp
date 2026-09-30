/*
 * daoGpuPipeline - run a chain of GPU stages in one process, one CUDA graph per frame.
 *
 * Replaces a chain of processes (daoCalIntensityNorm -> daoMvMGPU -> daoApplyGain
 * -> daoMvMGPU ...) connected by SHMs: the same SHMs are read and written, but
 * the whole chain is launched at once when the trigger SHM is updated, and
 * intermediate data stays on the GPU (make the intermediate SHMs GPU SHMs,
 * daoShmCreateGpu, mirrored if CPU tools read them). Every output SHM is still
 * published (cnt0, timestamp, semaphores) each frame, in stage order.
 *
 * Usage: daoGpuPipeline -c <config.yaml> [-s <stage>] [-C <cpu>] [-d <level>] -L
 *
 * -s N runs only stage N (0 = first) of the configuration, as its own process,
 * triggered by that stage's input SHM (on a semaphore of its own): one process per
 * stage, like the separate daoTools processes but on the GPU SHMs. Useful while
 * developing and tuning; the same configuration then runs as one pipeline.
 *
 * Configuration (YAML):
 *   device: 0                          # CUDA device (GPU SHMs must be on it)
 *   trigger: {shm: /tmp/pyrIm.im.shm}  # waits on a semaphore of its own (sem: N forces one)
 *   hostAccess: map                    # host SHMs: map into the GPU (zero copy, default:
 *                                      # kernels fetch only what they use) or copy
 *   stages:                            # in order; each is the daoTools app of that name
 *     # pixels
 *     - pixelCalibrate:   {in: ..., ff: ..., bg: ..., out: ...}
 *     - calIntensityNorm: {in: ..., ff: ..., bg: ..., ref: ..., validPix: ..., illumPix: ..., out: ...}
 *     - calIntensity:     {in: ..., ff: ..., bg: ..., ref: ..., validPix: ..., illumPix: ..., out: ...}
 *     - pixelExtract:     {in: ..., mask: ..., out: ...}
 *     - pixelSubstractExtract:          {in: ..., sub: ..., mask: ..., out: ..., norm: ...}  # norm optional
 *     - pixelSubstractExtractNorm:      {in: ..., sub: ..., mask: ..., out: ...}
 *     - pixelSubstractExtractNormImage: {in: ..., sub: ..., mask: ..., out: ...}
 *     - descrambleOcam2:  {in: ..., lut: ..., out: ..., binning: 1}
 *     # Shack-Hartmann
 *     - centroid:            {in: ..., ref: ..., threshold: ..., out: ..., subaSize: 20, nbSuba: 52}
 *     - centroidRelative:    {in: ..., ref: ..., threshold: ..., out: ..., subaSize: 20, nbSuba: 52}
 *     - centroidRelativeRef: {in: ..., subApCentre: ..., ref: ..., threshold: ..., out: ...,
 *                             subaSize: 20, nbSuba: 52}
 *     - centroidCorrelation:    {in: ..., subApCentre: ..., refImage: ..., threshold: ..., out: ...,
 *                                subaSize: 20, nbSuba: 52, searchRange: 5, alpha: 0}
 *     - centroidCorrelationFFT: {in: ..., subApCentre: ..., refImage: ..., threshold: ..., out: ...,
 *                                subaSize: 20, nbSuba: 52, alpha: 0}
 *       every centroid stage also takes minFlux: ... (optional): a 1-value SHM; a
 *       sub-aperture with less light (sum of its raw pixels) gets cx, cy = 0
 *     # vectors
 *     - mvm:       {in: ..., matrix: ..., out: ...}
 *     - applyGain: {in: ..., gain: ..., out: ..., modal: false}
 *     - leakyIntegrator: {in: ..., out: ..., loop: ..., gain: ..., leak: ...,
 *                         enable: ..., offset: ..., modal: false, keepPiston: false, clip: 10}
 *       daoLeakyIntegrator; enable and offset optional. The output is the state: keep it
 *       a GPU SHM (mirrored if CPU tools read it). keepPiston: true on modes (the mean
 *       is only removed from actuator commands).
 *     - slice:     {in: ..., out: ..., offset: 0, count: n}      # count: default the size of out
 *
 * Stage types from other libraries (see daoGpuStages.h, daoGpuRegisterStage):
 *   plugins: [/path/to/libmyStages.so]    # each exports daoGpuPluginRegister()
 *
 * Parameter SHMs (ff, bg, ref, masks, matrix, gain, ...) are reloaded to the GPU
 * when their cnt0 changes, between frames.
 *
 * A stage can be silent for a frame (e.g. a disabled integrator): its output is not
 * published. A stage whose input comes from a silent stage is silent too, and is not
 * run: an M2A after a disabled integrator leaves its output SHM alone (another
 * process may be writing it).
 */
#include <yaml-cpp/yaml.h>
#include <cuda_runtime.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "dao.h"
extern "C" {
#include "daoTools.h"      /* C header without extern "C" guards */
}
#include "daoGpuStages.h"

static volatile sig_atomic_t stop = 0;
static void onSignal(int) { stop = 1; }

static double nowUs()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec * 1e-3;
}

struct Pipeline {
    int device = 0;
    std::string triggerName;
    int triggerSem = DAO_SEM_AUTO;                            // trigger.sem: a fixed semaphore
    int mapHost = 1;                                          // hostAccess: map (default) | copy
    int onlyStage = -1;                                       // -s: run only this stage
    std::map<std::string, IMAGE *> shms;                     // every opened SHM
    std::map<std::string, std::unique_ptr<daoGpuPort>> ports; // data SHMs
    std::vector<daoGpuStage *> stages;
    std::vector<daoGpuPort *> uploads;                        // host SHMs read from outside
    std::vector<daoGpuPort *> outputs;                        // in stage order
    std::vector<int> producer;                                // stage writing each stage's input, or -1
    std::vector<char> upstream;                               // its input was published this frame: run it
    std::vector<char> pub;                                    // its output is published this frame

    IMAGE *shm(const std::string &name)
    {
        auto it = shms.find(name);
        if (it != shms.end())
            return it->second;
        IMAGE *im = (IMAGE *) calloc(1, sizeof(IMAGE));
        if (daoShmOpen(name.c_str(), im) != DAO_SUCCESS)
            throw std::runtime_error("cannot open " + name);
        shms[name] = im;
        return im;
    }

    /* map < 0: hostAccess; 0: copy (for scattered reads, slow over PCIe) */
    daoGpuPort *port(const std::string &name, int map = -1)
    {
        auto it = ports.find(name);
        if (it != ports.end())
            return it->second.get();
        std::unique_ptr<daoGpuPort> p(new daoGpuPort);
        if (!daoGpuPortInit(p.get(), shm(name), map < 0 ? mapHost : map))
            throw std::runtime_error("cannot use " + name + " on the GPU");
        return (ports[name] = std::move(p)).get();
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

/* Arguments of a plugin stage: its YAML keys and the pipeline's SHMs / ports. */
struct PluginArgs {
    Pipeline *p;
    YAML::Node node;
    std::map<std::string, std::string> values;            // keeps the returned strings alive
};

static const char *pluginValue(void *ctx, const char *key)
{
    PluginArgs *a = (PluginArgs *) ctx;
    if (!a->node[key])
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

static daoGpuPort *pluginPort(void *ctx, const char *key)
{
    const char *name = pluginValue(ctx, key);
    try {
        return name ? ((PluginArgs *) ctx)->p->port(name) : nullptr;
    } catch (const std::exception &e) {
        daoError("%s\n", e.what());
        return nullptr;
    }
}

/* Load the configuration's plugins: each registers its stage types. */
static void loadPlugins(const YAML::Node &cfg)
{
    if (!cfg["plugins"])
        return;
    for (const auto &item : cfg["plugins"]) {
        std::string path = item.as<std::string>();
        void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            throw std::runtime_error("plugin " + path + ": " + dlerror());
        auto reg = (void (*)(void)) dlsym(h, "daoGpuPluginRegister");
        if (!reg)
            throw std::runtime_error("plugin " + path + ": no daoGpuPluginRegister()");
        reg();
        daoInfo("plugin %s loaded\n", path.c_str());
    }
}

static void build(Pipeline &p, const YAML::Node &cfg)
{
    std::set<daoGpuPort *> produced;
    int index = -1;
    for (const auto &item : cfg["stages"]) {
        if (!item.IsMap() || item.size() != 1)
            throw std::runtime_error("each stage is a map with one key (the stage type)");
        if (p.onlyStage >= 0 && ++index != p.onlyStage)
            continue;
        std::string type = item.begin()->first.as<std::string>();
        YAML::Node a = item.begin()->second;
        if (p.onlyStage >= 0)                            // one stage: triggered by its own input
            p.triggerName = need(a, "in", type);
        auto shm = [&](const char *key) { return p.shm(need(a, key, type)); };
        auto optShm = [&](const char *key) { return a[key] ? shm(key) : (IMAGE *) nullptr; };
        const int one = 1;
        const float zero = 0.f;
        const bool no = false;
        // the descrambler reads the raw frame in scattered order: copy it in whole
        daoGpuPort *in = p.port(need(a, "in", type), type == "descrambleOcam2" ? 0 : -1);
        daoGpuPort *out = p.port(need(a, "out", type));
        daoGpuStage *s = nullptr;
        if (type == "pixelCalibrate")
            s = daoGpuPixelCalibrateCreate(in, shm("ff"), shm("bg"), out);
        else if (type == "calIntensityNorm")
            s = daoGpuCalIntensityNormCreate(in, shm("ff"), shm("bg"), shm("ref"), shm("validPix"),
                                             shm("illumPix"), out);
        else if (type == "calIntensity")
            s = daoGpuCalIntensityCreate(in, shm("ff"), shm("bg"), shm("ref"), shm("validPix"), shm("illumPix"),
                                         out);
        else if (type == "pixelExtract")
            s = daoGpuPixelExtractCreate(in, shm("mask"), out);
        else if (type == "pixelSubstractExtract")
            s = daoGpuPixelSubstractExtractCreate(in, shm("sub"), shm("mask"), a["norm"] ? shm("norm") : nullptr,
                                                  out);
        else if (type == "pixelSubstractExtractNorm")
            s = daoGpuPixelSubstractExtractNormCreate(in, shm("sub"), shm("mask"), out);
        else if (type == "pixelSubstractExtractNormImage")
            s = daoGpuPixelSubstractExtractNormImageCreate(in, shm("sub"), shm("mask"), out);
        else if (type == "descrambleOcam2")
            s = daoGpuDescrambleOcam2Create(in, shm("lut"), num<int>(a, "binning", type, &one), out);
        else if (type == "centroid")
            s = daoGpuCentroidCreate(in, shm("ref"), shm("threshold"), optShm("minFlux"), num<int>(a, "subaSize", type),
                                     num<int>(a, "nbSuba", type), out);
        else if (type == "centroidRelative")
            s = daoGpuCentroidRelativeCreate(in, shm("ref"), shm("threshold"), optShm("minFlux"),
                                             num<int>(a, "subaSize", type),
                                             num<int>(a, "nbSuba", type), out);
        else if (type == "centroidRelativeRef")
            s = daoGpuCentroidRelativeRefCreate(in, shm("subApCentre"), shm("ref"), shm("threshold"), optShm("minFlux"),
                                                num<int>(a, "subaSize", type), num<int>(a, "nbSuba", type), out);
        else if (type == "centroidCorrelation")
            s = daoGpuCentroidCorrelationCreate(in, shm("subApCentre"), shm("refImage"), shm("threshold"),
                                                optShm("minFlux"),
                                                num<int>(a, "subaSize", type), num<int>(a, "nbSuba", type),
                                                num<int>(a, "searchRange", type), num<float>(a, "alpha", type, &zero),
                                                out);
        else if (type == "centroidCorrelationFFT")
            s = daoGpuCentroidCorrelationFFTCreate(in, shm("subApCentre"), shm("refImage"), shm("threshold"),
                                                   optShm("minFlux"),
                                                   num<int>(a, "subaSize", type), num<int>(a, "nbSuba", type),
                                                   num<float>(a, "alpha", type, &zero), out);
        else if (type == "mvm")
            s = daoGpuMvmCreate(in, shm("matrix"), out);
        else if (type == "applyGain")
            s = daoGpuApplyGainCreate(in, shm("gain"), out, num<bool>(a, "modal", type, &no));
        else if (type == "leakyIntegrator") {
            const float clip = 10.f;                     // daoLeakyIntegrator's default
            s = daoGpuLeakyIntegratorCreate(in, shm("loop"), shm("gain"), shm("leak"), optShm("enable"),
                                            optShm("offset"), num<bool>(a, "modal", type, &no),
                                            num<bool>(a, "keepPiston", type, &no), num<float>(a, "clip", type, &clip),
                                            out);
        }
        else if (type == "slice") {
            const long zero = 0, all = -1;
            s = daoGpuSliceCreate(in, num<long>(a, "offset", type, &zero), num<long>(a, "count", type, &all), out);
        } else if (daoGpuStageFactory factory = daoGpuFindStage(type.c_str())) {
            PluginArgs pa{&p, a, {}};
            daoGpuStageArgs args{type.c_str(), &pa, pluginValue, pluginShm, pluginPort};
            s = factory(&args);
        } else
            throw std::runtime_error("unknown stage type '" + type + "' (a plugin missing under plugins:?)");
        if (!s)
            throw std::runtime_error(type + ": could not be created");
        // a host SHM read before any stage wrote it comes from outside: upload it each frame
        if (!produced.count(in) && !in->onGpu && !in->mapped)
            p.uploads.push_back(in);
        int from = -1;                                   // the last stage before this one writing its input
        for (size_t k = 0; k < p.outputs.size(); k++)
            if (p.outputs[k] == in)
                from = (int) k;
        p.producer.push_back(from);
        produced.insert(out);
        p.stages.push_back(s);
        p.outputs.push_back(out);
    }
    if (p.stages.empty())
        throw std::runtime_error(p.onlyStage >= 0 ? "no stage " + std::to_string(p.onlyStage) : "no stages");
    p.upstream.assign(p.stages.size(), 1);
    p.pub.assign(p.stages.size(), 1);
}

/* Which stages run and publish this frame (after the stages' update): a stage is
 * published if it says so and its input's stage was published; it runs if its
 * input's stage was published. */
static void decidePublish(Pipeline &p)
{
    for (size_t k = 0; k < p.stages.size(); k++) {
        int from = p.producer[k];
        p.upstream[k] = from < 0 || p.pub[from];
        p.pub[k] = p.upstream[k] && daoGpuStagePublishes(p.stages[k]);
    }
}

/* Per-step GPU timing (DAO_GPU_PIPELINE_PROFILE=1): no graph, events between steps. */
struct Profile {
    bool on = false;
    std::vector<cudaEvent_t> ev;
    std::vector<std::string> names;
    std::vector<double> acc;
    long frames = 0;
    size_t i = 0;
    void mark(cudaStream_t st, const std::string &name)
    {
        if (!on)
            return;
        if (i >= ev.size()) {
            cudaEvent_t e;
            cudaEventCreate(&e);
            ev.push_back(e);
            names.push_back(name);
            acc.push_back(0);
        }
        cudaEventRecord(ev[i++], st);
    }
    void collect()                                  // after the stream is synchronised
    {
        for (size_t k = 1; k < i; k++) {
            float ms = 0;
            cudaEventElapsedTime(&ms, ev[k - 1], ev[k]);
            acc[k] += ms * 1e3;
        }
        frames++;
        i = 0;
    }
    std::string report()
    {
        std::string r;
        char buf[128];
        for (size_t k = 1; k < names.size(); k++) {
            snprintf(buf, sizeof buf, " %s %.1f", names[k].c_str(), acc[k] / frames);
            r += buf;
            acc[k] = 0;
        }
        frames = 0;
        return r;
    }
};
static Profile prof;

/* Copies of intermediate results to the host (mirrors of GPU SHMs, host SHMs read
 * by the next stage from the host) run on a side stream, in parallel with the next
 * stages: they delay only the publishing, not the chain. */
struct SideCopies {
    cudaStream_t st = nullptr;
    std::vector<cudaEvent_t> ready;                   // one per stage
    cudaEvent_t done = nullptr;
};
static SideCopies side;

static bool needsCopyOut(const daoGpuPort *o) { return (!o->onGpu && !o->mapped) || o->mirror; }

/* Everything one frame does on the GPU, in order (captured once as a graph). */
static bool enqueueFrame(Pipeline &p, cudaStream_t st)
{
    prof.mark(st, "start");
    for (daoGpuPort *u : p.uploads)
        daoGpuPortUpload(u, st);
    if (!p.uploads.empty())
        prof.mark(st, "upload");
    bool anySide = false;
    for (size_t k = 0; k < p.stages.size(); k++) {
        daoGpuStage *s = p.stages[k];
        if (!p.upstream[k])                              // its input's stage is silent: nothing new
            continue;
        if (!daoGpuStageRun(s, st))
            return false;
        prof.mark(st, daoGpuStageName(s));
        daoGpuPort *o = daoGpuStageOutput(s);
        if (!needsCopyOut(o) || !p.pub[k])              // not published this frame: host SHM left alone
            continue;
        bool last = k + 1 == p.stages.size();
        if (last || prof.on) {                       // nothing left to overlap with
            daoGpuPortDownload(o, st);
            prof.mark(st, "copyOut");
        } else {                                     // copy while the next stages run
            cudaEventRecord(side.ready[k], st);
            cudaStreamWaitEvent(side.st, side.ready[k], 0);
            daoGpuPortDownload(o, side.st);
            anySide = true;
        }
    }
    if (anySide) {                                   // publish only once every copy is done
        cudaEventRecord(side.done, side.st);
        cudaStreamWaitEvent(st, side.done, 0);
    }
    return true;
}

static void publish(Pipeline &p)
{
    for (size_t k = 0; k < p.stages.size(); k++) {
        daoGpuStage *s = p.stages[k];
        if (!p.pub[k])                                   // the stage skips this frame
            continue;
        IMAGE *out = daoGpuStageOutput(s)->shm;
        long long cnt2 = daoGpuStageOutputCnt2(s);
        if (cnt2 >= 0)
            out->md[0].cnt2 = (uint64_t) cnt2;
        daoShmSetDataPartFinalize(out);                  // cnt0, timestamp, semaphores
    }
}

static int run(const char *configPath, int cpu, int onlyStage)
{
    YAML::Node cfg = YAML::LoadFile(configPath);
    Pipeline p;
    p.onlyStage = onlyStage;
    p.device = cfg["device"] ? cfg["device"].as<int>() : 0;
    p.triggerName = need(cfg["trigger"], "shm", "trigger");
    p.triggerSem = cfg["trigger"]["sem"] ? cfg["trigger"]["sem"].as<int>() : DAO_SEM_AUTO;
    if (cfg["hostAccess"]) {
        std::string mode = cfg["hostAccess"].as<std::string>();
        if (mode != "map" && mode != "copy")
            throw std::runtime_error("hostAccess: 'map' or 'copy'");
        p.mapHost = mode == "map";
    }

    // device and spin scheduling before any CUDA context exists in this process
    if (cudaSetDevice(p.device) != cudaSuccess || cudaSetDeviceFlags(cudaDeviceScheduleSpin) != cudaSuccess)
        daoWarning("could not select GPU %d with spin scheduling\n", p.device);
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0)
            daoWarning("could not pin to CPU %d\n", cpu);
    }

    loadPlugins(cfg);
    build(p, cfg);
    IMAGE *trigger = p.shm(p.triggerName);
    for (auto &kv : p.ports)
        if (kv.second->onGpu && kv.second->shm->md[0].gpu_device != p.device)
            daoWarning("%s was created on GPU %d, the pipeline runs on GPU %d\n", kv.first.c_str(),
                       kv.second->shm->md[0].gpu_device, p.device);

    cudaStream_t st;
    cudaStreamCreateWithFlags(&st, cudaStreamNonBlocking);
    cudaStreamCreateWithFlags(&side.st, cudaStreamNonBlocking);
    side.ready.resize(p.stages.size());
    for (auto &e : side.ready)
        cudaEventCreateWithFlags(&e, cudaEventDisableTiming);
    cudaEventCreateWithFlags(&side.done, cudaEventDisableTiming);
    for (daoGpuStage *s : p.stages)
        if (daoGpuStageUpdate(s, st) < 0)
            throw std::runtime_error(std::string(daoGpuStageName(s)) + ": cannot load its parameters");
    decidePublish(p);
    // warm-up (cuBLAS workspace, kernel loading), then capture one frame as a graph
    if (!enqueueFrame(p, st) || cudaStreamSynchronize(st) != cudaSuccess)
        throw std::runtime_error("first frame failed");
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t exec = nullptr;
    const char *profEnv = getenv("DAO_GPU_PIPELINE_PROFILE");
    prof.on = profEnv && *profEnv && strcmp(profEnv, "0") != 0;
    std::vector<char> capturedPublish(p.stages.size());   // which outputs the graph copies back
    auto capture = [&]() {                           // one frame's work as a graph
        for (size_t k = 0; k < p.stages.size(); k++)
            capturedPublish[k] = p.pub[k];
        if (exec)
            cudaGraphExecDestroy(exec);
        if (graph)
            cudaGraphDestroy(graph);
        exec = nullptr;
        graph = nullptr;
        return cudaStreamBeginCapture(st, cudaStreamCaptureModeThreadLocal) == cudaSuccess
               && enqueueFrame(p, st) && cudaStreamEndCapture(st, &graph) == cudaSuccess
               && cudaGraphInstantiateWithFlags(&exec, graph, 0) == cudaSuccess;
    };
    bool useGraph = !prof.on && capture();
    if (!useGraph) {
        cudaGetLastError();
        daoWarning("CUDA graph unavailable, launching the stages one by one\n");
    }
    daoInfo("%zu stages, %zu upload(s), triggered by %s (semaphore %d)%s\n", p.stages.size(), p.uploads.size(),
            p.triggerName.c_str(),
            p.triggerSem >= 0 ? p.triggerSem : daoShmClaimSem(trigger), useGraph ? ", CUDA graph" : "");
    if (p.onlyStage < 0)
        daoInfo("one process for the whole chain: MPS is only useful if other processes compute on GPU %d\n",
                p.device);

    daoToolsOnExitSignals(onSignal);   // Ctrl+C, kill, tmux kill-session
    double acc = 0, worst = 0, tPrint = nowUs();
    double accWake = 0, accUpd = 0, accGpu = 0, accPub = 0;   // breakdown, us
    long n = 0;
    long waits = 0;
    while (!stop) {
        if (daoToolsWait(trigger, p.triggerSem, 1.0) != DAO_SUCCESS)
        {
            printf("\rwaiting for %s (%ld s)                    ", p.triggerName.c_str(), ++waits);
            fflush(stdout);
            tPrint = nowUs();                            // the rate counts from the next frame
            acc = worst = accWake = accUpd = accGpu = accPub = 0;
            n = 0;
            continue;
        }
        waits = 0;
        double t0 = nowUs();
        {   // wake-up delay: trigger timestamp (CLOCK_REALTIME) to now, in full seconds and ns
            struct timespec rt;
            clock_gettime(CLOCK_REALTIME, &rt);
            const struct timespec &ts = trigger->md[0].atime.ts;
            accWake += ((double)(rt.tv_sec - ts.tv_sec) * 1e9 + (double)(rt.tv_nsec - ts.tv_nsec)) * 1e-3;
        }
        bool paramsOk = true, recapture = false;
        for (daoGpuStage *s : p.stages) {                // rare: new flat, matrix, gain...
            int r = daoGpuStageUpdate(s, st);
            paramsOk = r >= 0 && paramsOk;
            recapture = recapture || r == DAO_GPU_RECAPTURE;
        }
        decidePublish(p);
        for (size_t k = 0; k < p.stages.size() && !recapture; k++)   // a stage starts / stops publishing:
            recapture = p.pub[k] != capturedPublish[k];   // its copy-back (and what runs) changes
        if (!paramsOk)                                   // the stage said why; skip this frame
            continue;
        if (recapture && useGraph && !capture()) {       // a stage's work changed (e.g. sizes)
            cudaGetLastError();
            useGraph = false;
            daoWarning("CUDA graph capture failed, launching the stages one by one\n");
        }
        double t1 = nowUs();
        for (size_t k = 0; k < p.stages.size(); k++)     // outputs about to be written
            if (p.pub[k])
                daoShmBeginWrite(daoGpuStageOutput(p.stages[k])->shm);
        if (useGraph)
            cudaGraphLaunch(exec, st);
        else
            enqueueFrame(p, st);
        if (cudaStreamSynchronize(st) != cudaSuccess) {
            daoError("GPU error: %s\n", cudaGetErrorString(cudaGetLastError()));
            break;
        }
        double t2 = nowUs();
        if (prof.on)
            prof.collect();
        publish(p);
        double dt = nowUs() - t0;
        for (daoGpuStage *s : p.stages)                  // after publishing: reference updates...
            daoGpuStagePost(s, st);
        accUpd += t1 - t0;
        accGpu += t2 - t1;
        accPub += nowUs() - t2;
        acc += dt;
        worst = dt > worst ? dt : worst;
        n++;
        if (nowUs() - tPrint > 1e6) {
            printf("\r%s: avg %.1f us  max %.1f us  %.1f Hz  [wake %.1f  params %.1f  gpu %.1f  "
                   "publish %.1f us]      ", p.onlyStage >= 0 ? daoGpuStageName(p.stages[0]) : "pipeline",
                   acc / n, worst, n * 1e6 / (nowUs() - tPrint), accWake / n,
                   accUpd / n, accGpu / n, accPub / n);
            if (prof.on)
                printf("\n  GPU us per step:%s\n", prof.report().c_str());
            fflush(stdout);
            acc = worst = accWake = accUpd = accGpu = accPub = 0;
            n = 0;
            tPrint = nowUs();
        }
    }
    printf("\n");
    if (useGraph) {
        cudaGraphExecDestroy(exec);
        cudaGraphDestroy(graph);
    }
    for (daoGpuStage *s : p.stages)
        daoGpuStageDestroy(s);
    for (auto &kv : p.ports)
        daoGpuPortFree(kv.second.get());
    for (auto &kv : p.shms) {
        daoShmClose(kv.second);
        free(kv.second);
    }
    cudaStreamDestroy(st);
    return 0;
}

static void help(const char *argv0)
{
    printf("usage: %s -c <config.yaml> [-s <stage>] [-C <cpu>] [-d <level>] -L\n"
           "  -h  display this message and exit\n"
           "  -c  pipeline configuration (see the header of daoGpuPipeline.cpp)\n"
           "  -s  run only this stage (0 = first), triggered by its input SHM\n"
           "  -C  pin the loop to this CPU core\n"
           "  -d  log level: 0 warnings and errors (default), 1 info, 2 debug, 3 trace\n"
           "  -L  run\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *config = nullptr;
    int cpu = -1, go = 0, stage = -1;
    // the options, from the first argument (daoToolsArg* exit with an error when a value is missing)
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
        else if (!strcmp(opt, "-s"))
        {
            stage = (int)daoToolsArgInt(&left, &arg, opt);
        }
        else if (!strcmp(opt, "-d"))
        {
            daoLogLevel = (int)daoToolsArgInt(&left, &arg, opt);
            daoSetLogLevel(daoLogLevel);
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
    // The whole chain runs in this process: no GPU hand-over between processes, so
    // daoBase's MPS warning (made for chains of separate processes) does not apply.
    // With -s it does: the stages are separate processes again.
    if (stage < 0)
        setenv("DAO_GPU_NO_MPS_WARNING", "1", 0);
    daoToolsSetRtPriority(93);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        daoWarning("mlockall failed: run scripts/daoToolSetCap to grant RT capabilities\n");
    try {
        return run(config, cpu, stage);
    } catch (const std::exception &e) {
        daoError("%s\n", e.what());
        return 1;
    }
}
