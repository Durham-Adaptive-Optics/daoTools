#!/usr/bin/env python3
"""
Shack-Hartmann chain: separate daoTools processes vs daoGpuPipeline, same SHMs, same frames.

    shIm (camera, uint16 160x160: 8x8 sub-apertures of 20 px, 52 in the pupil)
      -> pixelCalibrate (shFf, shBg)                    -> shImCal (float 160x160)
      -> centroider (shReferences, shThreshold[, shRefImage]) -> shCentroids (4 x 52)
      -> mvm shRecon (97 x 104)                         -> dmResWf (97)

with zonal reconstruction, for three centroiders:
relative (default), correlation (--search-range 5) and correlationFFT.

"cpu": 3 processes (daoPixelCalibrate, daoComputeCentroid*, daoMvM), host SHMs.
"gpu": daoGpuPipeline, one process; shImCal and shCentroids are mirrored GPU SHMs.

Each frame is timed from writing shIm to dmResWf being published; the GPU outputs
are compared with the CPU chain's, frame by frame.

    python3 tests/bench_sh_pipeline.py [--frames 1000] [--pipeline PATH] [--shm-dir /dev/shm/...]
"""
import argparse
import os
import signal
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DAOROOT = os.environ.get("DAOROOT", os.path.expanduser("~/DAOROOT"))
APPS = os.path.join(ROOT, "build", "apps")               # this tree's CPU tools
sys.path.insert(0, os.path.join(DAOROOT, "python"))
import daoShm  # noqa: E402

NGRID, BOX, NACT = 8, 20, 97
NX = NGRID * BOX
SHMS = ["shIm", "shFf", "shBg", "shImCal", "shCentroids", "shReferences", "shThreshold", "shRefImage",
        "shRecon", "dmResWf"]
CENTROIDERS = {                                           # typical thresholds
    "relative": 0.2,                                      # of each spot's maximum
    "correlation": 50.0,                                  # absolute, counts
    "correlationFFT": 50.0,
}
SEARCH_RANGE = 5
SHM_DIR = "/tmp"


def path(name):
    return f"{SHM_DIR}/{name}.im.shm"


def geometry():
    """8x8 grid, circular pupil of radius 4, centres from daoTools.ShackHartmannWFS."""
    c = (NGRID - 1) / 2
    gy, gx = np.mgrid[0:NGRID, 0:NGRID]
    pup = np.hypot(gx - c, gy - c) <= 4.0
    centres = np.arange(NGRID) * BOX + BOX / 2 - 0.5            # 9.5, 29.5, ...
    cx, cy = np.meshgrid(centres, centres)
    return cx[pup], cy[pup]


def make_data(rng, nframes):
    cx, cy = geometry()
    n = len(cx)
    y, x = np.mgrid[0:NX, 0:NX]
    frames = np.empty((nframes, NX, NX), np.uint16)
    tilt = np.cumsum(rng.normal(0, 0.3, (nframes, 2)), axis=0) % 4 - 2      # common drift, +-2 px
    for k in range(nframes):
        img = np.full((NX, NX), 100.0)
        dx = tilt[k, 0] + rng.normal(0, 0.5, n)
        dy = tilt[k, 1] + rng.normal(0, 0.5, n)
        for s in range(n):
            x0, y0 = int(cx[s] + 0.5) - BOX // 2, int(cy[s] + 0.5) - BOX // 2
            sl = (slice(y0, y0 + BOX), slice(x0, x0 + BOX))
            img[sl] += 1500 * np.exp(-((x[sl] - cx[s] - dx[s]) ** 2 + (y[sl] - cy[s] - dy[s]) ** 2) / (2 * 1.6 ** 2))
        frames[k] = np.clip(img + rng.normal(0, 8, img.shape), 0, 65535)
    t = np.arange(BOX) - BOX // 2
    template = np.exp(-(t[None, :] ** 2 + t[:, None] ** 2) / (2 * 1.5 ** 2))
    return dict(
        n=n, frames=frames,
        ff=(1 + 0.02 * rng.standard_normal((NX, NX))).astype(np.float32),
        bg=np.full((NX, NX), 100, np.float32),
        refs=np.concatenate([cx, cy])[:, None].astype(np.float32),
        refImage=np.tile(template, (n, 1)).astype(np.float32),
        recon=(rng.standard_normal((NACT, 2 * n)) * 0.05).astype(np.float32),
    )


def create_shms(d, centroider, gpu, device):
    """Create the SHMs (called in a short-lived process: GPU payloads outlive it)."""
    for s in SHMS:
        if os.path.exists(path(s)):
            os.remove(path(s))
    host = lambda s, a: daoShm.shm(path(s), a)
    inter = (lambda s, a: daoShm.shm(path(s), a, gpu=device, mirror=True)) if gpu else host
    return [host("shIm", d["frames"][0]), host("shFf", d["ff"]), host("shBg", d["bg"]),
            inter("shImCal", np.zeros((NX, NX), np.float32)),
            inter("shCentroids", np.zeros((4 * d["n"], 1), np.float32)),
            host("shReferences", d["refs"]),
            host("shThreshold", np.full((1, 1), CENTROIDERS[centroider], np.float32)),
            host("shRefImage", d["refImage"]), host("shRecon", d["recon"]),
            host("dmResWf", np.zeros((NACT, 1), np.float32))]


def cpu_chain(centroider, n):
    """The chain as separate daoTools processes (zonal)."""
    cent = {"relative": ["daoComputeCentroidRelative", "-S", path("shImCal"), path("shCentroids"),
                         path("shReferences"), path("shThreshold"), str(BOX), str(n), "-L"],
            "correlation": ["daoComputeCentroidCorrelation", "-S", path("shImCal"), path("shCentroids"),
                            path("shReferences"), path("shRefImage"), path("shThreshold"), str(BOX), str(n),
                            str(SEARCH_RANGE), "-L"],
            "correlationFFT": ["daoComputeCentroidCorrelationFFT", "-S", path("shImCal"), path("shCentroids"),
                               path("shReferences"), path("shRefImage"), path("shThreshold"), str(BOX), str(n),
                               "-L"]}[centroider]
    return [["daoPixelCalibrate", "-S", path("shIm"), path("shFf"), path("shBg"), path("shImCal"), "-s", "1", "-L"],
            cent,
            ["daoMvM", "-S", path("shCentroids"), path("shRecon"), path("dmResWf"), "-s", "1", "-L"]]


def gpu_config(centroider, n, device):
    common = f"in: {path('shImCal')}, threshold: {path('shThreshold')}, out: {path('shCentroids')}, " \
             f"subaSize: {BOX}, nbSuba: {n}"
    cent = {"relative": f"centroidRelative: {{{common}, ref: {path('shReferences')}}}",
            "correlation": f"centroidCorrelation: {{{common}, subApCentre: {path('shReferences')}, "
                           f"refImage: {path('shRefImage')}, searchRange: {SEARCH_RANGE}}}",
            "correlationFFT": f"centroidCorrelationFFT: {{{common}, subApCentre: {path('shReferences')}, "
                              f"refImage: {path('shRefImage')}}}"}[centroider]
    return f"""device: {device}
trigger: {{shm: {path('shIm')}, sem: 1}}
stages:
  - pixelCalibrate: {{in: {path('shIm')}, ff: {path('shFf')}, bg: {path('shBg')}, out: {path('shImCal')}}}
  - {cent}
  - mvm: {{in: {path('shCentroids')}, matrix: {path('shRecon')}, out: {path('dmResWf')}}}
"""


def stop(procs):
    for p in procs:
        p.send_signal(signal.SIGINT)
    for p in procs:
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=1000)
    ap.add_argument("--device", type=int, default=0)
    ap.add_argument("--pipeline", default=os.path.join(APPS, "daoGpuPipeline"))
    ap.add_argument("--shm-dir", default="/dev/shm/daoShBench",
                    help="folder of the SHMs; /dev/shm is RAM (tmpfs), needed for zero copy")
    ap.add_argument("--setup", nargs=2, metavar=("CENTROIDER", "MODE"), help=argparse.SUPPRESS)
    args = ap.parse_args()
    globals()["SHM_DIR"] = args.shm_dir
    os.makedirs(args.shm_dir, exist_ok=True)
    rng = np.random.default_rng(1)
    d = make_data(rng, args.frames)
    if args.setup:                                        # child: create the SHMs and exit
        create_shms(d, args.setup[0], args.setup[1] == "gpu", args.device)
        return

    with tempfile.TemporaryDirectory(prefix="shb") as tmp:
        driver = os.path.join(tmp, "driver")
        subprocess.run(["gcc", "-O2", "-I", os.path.join(DAOROOT, "include"), os.path.join(HERE, "pipelineDriver.c"),
                        "-o", driver, "-L", os.path.join(DAOROOT, "lib"), "-L", os.path.join(DAOROOT, "lib64"), "-ldao"],
                        check=True)
        frames_file = os.path.join(tmp, "frames.bin")
        d["frames"].tofile(frames_file)
        env = dict(os.environ, DAO_GPU_SOCKET=os.path.join(tmp, "d.sock"), DAO_GPU_NO_MPS_WARNING="1",
                   PATH=APPS + ":" + os.environ["PATH"],
                   LD_LIBRARY_PATH=os.path.dirname(os.path.abspath(args.pipeline)) + ":"
                   + os.path.join(ROOT, "build", "src") + ":" + os.environ.get("LD_LIBRARY_PATH", ""))

        print(f"{d['n']} sub-apertures, {args.frames} frames; us from shIm written to dmResWf published")
        print(f"{'centroider':>15} {'chain':>22} {'median':>8} {'p99':>8} {'max':>8}   GPU vs CPU output")
        for centroider in CENTROIDERS:
            outs = {}
            for mode in ("cpu", "gpu"):
                for s in SHMS:
                    if os.path.exists(path(s)):
                        os.remove(path(s))
                time.sleep(1.2)                           # daoGpuShmd drops the previous payloads
                subprocess.run([sys.executable, __file__, "--setup", centroider, mode, "--frames", str(args.frames),
                                "--device", str(args.device), "--shm-dir", args.shm_dir],
                               env=env, check=True, capture_output=True)
                if mode == "cpu":
                    cmds = cpu_chain(centroider, d["n"])
                else:
                    cfg = os.path.join(tmp, f"{centroider}.yaml")
                    with open(cfg, "w") as f:
                        f.write(gpu_config(centroider, d["n"], args.device))
                    cmds = [[args.pipeline, "-c", cfg, "-L"]]
                procs = [subprocess.Popen(c, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                         for c in cmds]
                time.sleep(3)                             # the CPU tools sleep 2 s before their loop
                out_file = os.path.join(tmp, f"out_{mode}.bin")
                r = subprocess.run([driver, path("shIm"), path("dmResWf"), frames_file, str(args.frames), out_file],
                                   env=env, capture_output=True, text=True, timeout=600)
                stop(procs)
                label = {"cpu": "3 processes (CPU)", "gpu": "daoGpuPipeline"}[mode]
                lat_line = [l for l in r.stdout.splitlines() if l.startswith("LAT ")]
                if not lat_line:
                    print(f"{centroider:>15} {label:>22}  ERROR {r.stdout[-300:]}")
                    continue
                lat = np.array([float(x) for x in lat_line[0].split()[1:]])
                outs[mode] = np.fromfile(out_file, np.float32).reshape(args.frames, NACT)
                cmp = ""
                if mode == "gpu" and "cpu" in outs:
                    diff = np.abs(outs["gpu"] - outs["cpu"]).max() / (np.abs(outs["cpu"]).max() + 1e-12)
                    cmp = f"max rel. difference {diff:.1e}"
                print(f"{centroider:>15} {label:>22} {np.median(lat):8.1f} {np.percentile(lat, 99):8.1f} "
                      f"{lat.max():8.1f}   {cmp}")
        for s in SHMS:
            if os.path.exists(path(s)):
                os.remove(path(s))
        out = subprocess.run([os.path.join(DAOROOT, "bin", "daoGpuShmd"), "--ping"], env=env, capture_output=True,
                             text=True)
        if "pid" in out.stdout:
            os.kill(int(out.stdout.split("pid")[1]), signal.SIGTERM)


if __name__ == "__main__":
    main()
