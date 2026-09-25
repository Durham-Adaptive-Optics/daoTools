#!/usr/bin/env python3
"""
Pyramid WFS chain: separate daoTools processes vs daoGpuPipeline, same SHMs, same frames.

    pyrIm (camera, uint16 560x560)
      -> calIntensityNorm (pyrFf, pyrBg, pyrImOffset, pyrPup)  -> pyrImCalExtract (9156)
      -> mvm  pyrI2M (97 x 9156)                               -> pyrModes (97)
      -> applyGain pyrModesGain[0]                             -> pyrModesWithGain (97)
      -> mvm  dm1M2A (97 x 96)                                 -> dm1ResWf (97)

"cpu"  : the current chain, 4 processes (daoCalIntensityNorm, daoMvMGPU, daoApplyGain,
         daoMvMGPU) and host SHMs.
"gpu"  : daoGpuPipeline, one process; pyrImCalExtract, pyrModes and pyrModesWithGain are
         mirrored GPU SHMs, the rest stay host SHMs.

Each frame is timed from writing pyrIm to dm1ResWf being published; outputs are checked
against a NumPy reference. Runs with and without MPS (private MPS server).

    python3 tests/bench_pyramid_pipeline.py [--frames 1000] [--pipeline PATH]
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
BIN = os.path.join(DAOROOT, "bin")
MPS_HELPER = next((p for p in (os.path.join(BIN, "daoGpuMps"),
                               os.path.expanduser("~/dao/daoBase/scripts/daoGpuMps")) if os.path.exists(p)),
                  "daoGpuMps")
sys.path.insert(0, os.path.join(DAOROOT, "python"))
import daoShm  # noqa: E402

NX = 560
NMODES = 97
NACT = 97
SHMS = ["pyrIm", "pyrFf", "pyrBg", "pyrImOffset", "pyrPup", "pyrImCalExtract", "pyrI2M", "pyrModes",
        "pyrModesGain", "pyrModesWithGain", "dm1M2A", "dm1ResWf"]


SHM_DIR = os.environ.get("DAO_BENCH_SHM_DIR", "/tmp")


def path(name):
    return f"{SHM_DIR}/{name}.im.shm"


def four_pupils():
    """4 pupils of diameter 54 px on the 560x560 frame."""
    m = np.zeros((NX, NX))
    y, x = np.ogrid[:NX, :NX]
    c, sep = NX // 2, 500
    for px, py in [(c - sep // 2, c - sep // 2), (c + sep // 2, c - sep // 2),
                   (c - sep // 2, c + sep // 2), (c + sep // 2, c + sep // 2)]:
        m[np.sqrt((x - px) ** 2 + (y - py) ** 2) <= 27] = 1
    return m.astype(np.uint32)


def make_data(rng, nframes):
    pup = four_pupils()
    nvalid = int(pup.sum())
    d = dict(
        pup=pup,
        ff=(1 + 0.05 * rng.standard_normal((NX, NX))).astype(np.float32),
        bg=(100 + 5 * rng.standard_normal((NX, NX))).astype(np.float32),
        ref=(pup * (1 + 0.1 * rng.random((NX, NX)))).astype(np.float32),
        i2m=(rng.standard_normal((NMODES, nvalid)) * 1e-2).astype(np.float32),
        m2a=(rng.standard_normal((NACT, NMODES - 1)) * 0.1).astype(np.float32),
        gain=np.full((NMODES, 1), 0.4, np.float32),
    )
    base = 100 + pup * 2000.0
    d["frames"] = np.clip(base + rng.normal(0, 60, (nframes, NX, NX)), 0, 4095).astype(np.uint16)
    return d


def reference(d, frame):
    """NumPy version of the chain (float64), for checking both implementations."""
    pup = d["pup"].ravel() == 1
    raw = frame.ravel()[pup].astype(np.float64)
    ff = d["ff"].ravel()[pup].astype(np.float64)
    w = np.where(ff > 0, ff, 0)                      # illum == valid here
    cal = (raw - d["bg"].ravel()[pup]) * w
    ref = d["ref"].ravel()[pup].astype(np.float64)
    inten = cal / cal.sum() - ref / ref.sum()
    modes = d["i2m"].astype(np.float64) @ inten
    modes_g = modes * d["gain"][0, 0]
    return d["m2a"].astype(np.float64) @ modes_g[: NMODES - 1]


def create_shms(d, gpu, device):
    """Create the SHMs (called in a short-lived process: GPU payloads outlive it)."""
    for n in SHMS:
        if os.path.exists(path(n)):
            os.remove(path(n))
    nvalid = int(d["pup"].sum())
    host = lambda n, a: daoShm.shm(path(n), a)
    inter = (lambda n, a: daoShm.shm(path(n), a, gpu=device, mirror=gpu != "nomirror")) if gpu else host
    keep = [host("pyrIm", d["frames"][0]), host("pyrFf", d["ff"]), host("pyrBg", d["bg"]),
            host("pyrImOffset", d["ref"]), host("pyrPup", d["pup"]),
            inter("pyrImCalExtract", np.zeros((nvalid, 1), np.float32)),
            host("pyrI2M", d["i2m"]), inter("pyrModes", np.zeros((NMODES, 1), np.float32)),
            host("pyrModesGain", d["gain"]), inter("pyrModesWithGain", np.zeros((NMODES, 1), np.float32)),
            host("dm1M2A", d["m2a"]), host("dm1ResWf", np.zeros((NACT, 1), np.float32))]
    return keep


def start_chain(mode, env, pipeline, cfg):
    if mode != "cpu":
        cmds = [[pipeline, "-c", cfg + (".copy" if mode == "gpu-copy" else ""), "-L"]]
    else:
        cmds = [[os.path.join(BIN, "daoCalIntensityNorm"), "-S", path("pyrIm"), path("pyrFf"), path("pyrBg"),
                 path("pyrImOffset"), path("pyrPup"), path("pyrPup"), path("pyrImCalExtract"), "-s", "1", "-L"],
                [os.path.join(BIN, "daoMvMGPU"), "-S", path("pyrImCalExtract"), path("pyrI2M"), path("pyrModes"),
                 "-s", "1", "-L"],
                [os.path.join(BIN, "daoApplyGain"), "-S", path("pyrModes"), path("pyrModesGain"),
                 path("pyrModesWithGain"), "-L"],
                [os.path.join(BIN, "daoMvMGPU"), "-S", path("pyrModesWithGain"), path("dm1M2A"), path("dm1ResWf"),
                 "-s", "1", "-L"]]
    procs = [subprocess.Popen(c, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) for c in cmds]
    time.sleep(3)                                     # CUDA init, cuBLAS warm-up, graph capture
    return procs


def stop_chain(procs):
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
    ap.add_argument("--pipeline", default=os.path.join(ROOT, "build", "apps", "daoGpuPipeline"))
    ap.add_argument("--setup", choices=["cpu", "gpu", "nomirror"], help=argparse.SUPPRESS)
    ap.add_argument("--shm-dir", default=SHM_DIR,
                    help="folder of the SHMs (default /tmp); /dev/shm is RAM (tmpfs), needed for pinning")
    args = ap.parse_args()
    os.environ["DAO_BENCH_SHM_DIR"] = args.shm_dir     # also for the --setup child
    globals()["SHM_DIR"] = args.shm_dir
    rng = np.random.default_rng(1)
    d = make_data(rng, args.frames)
    if args.setup:                                   # child: create the SHMs and exit
        create_shms(d, False if args.setup == "cpu" else args.setup, args.device)
        return
    ref = np.array([reference(d, d["frames"][k]) for k in range(0, args.frames, 50)])

    with tempfile.TemporaryDirectory(prefix="rpb") as tmp:
        driver = os.path.join(tmp, "driver")
        subprocess.run(["gcc", "-O2", "-I", os.path.join(DAOROOT, "include"), os.path.join(HERE, "pipelineDriver.c"),
                        "-o", driver, "-L", os.path.join(DAOROOT, "lib"), "-L", os.path.join(DAOROOT, "lib64"), "-ldao"],
                        check=True)
        frames_file = os.path.join(tmp, "frames.bin")
        d["frames"].tofile(frames_file)
        cfg = os.path.join(tmp, "pyramid.yaml")
        with open(cfg, "w") as f:
            f.write(f"""device: {args.device}
trigger: {{shm: {path('pyrIm')}, sem: 1}}
stages:
  - calIntensityNorm: {{in: {path('pyrIm')}, ff: {path('pyrFf')}, bg: {path('pyrBg')}, ref: {path('pyrImOffset')},
                        validPix: {path('pyrPup')}, illumPix: {path('pyrPup')}, out: {path('pyrImCalExtract')}}}
  - mvm:       {{in: {path('pyrImCalExtract')}, matrix: {path('pyrI2M')}, out: {path('pyrModes')}}}
  - applyGain: {{in: {path('pyrModes')}, gain: {path('pyrModesGain')}, out: {path('pyrModesWithGain')}}}
  - mvm:       {{in: {path('pyrModesWithGain')}, matrix: {path('dm1M2A')}, out: {path('dm1ResWf')}}}
""")
        with open(cfg) as f:
            text = f.read()
        with open(cfg + ".copy", "w") as f:
            f.write(text.replace("stages:", "hostAccess: copy\nstages:", 1))
        base = dict(os.environ, DAO_GPU_SOCKET=os.path.join(tmp, "d.sock"), DAO_GPU_NO_MPS_WARNING="1",
                    LD_LIBRARY_PATH=os.path.dirname(os.path.abspath(args.pipeline)) + ":"
                    + os.path.join(ROOT, "build", "src") + ":" + os.environ.get("LD_LIBRARY_PATH", ""))
        mps_env = dict(base, CUDA_MPS_PIPE_DIRECTORY=os.path.join(tmp, "mps"),
                       CUDA_MPS_LOG_DIRECTORY=os.path.join(tmp, "mpslog"))

        print(f"{'MPS':>4} {'chain':>28} {'median':>8} {'p99':>8} {'max':>8}  max rel. error vs NumPy"
              f"   (us from pyrIm written to dm1ResWf published, {args.frames} frames)")
        for use_mps in (False, True):
            env = mps_env if use_mps else base
            if use_mps:
                subprocess.run([MPS_HELPER, "start"], env=env, capture_output=True)
            try:
                for mode in ("cpu", "gpu-copy", "gpu", "gpu-nomirror"):
                    for n in SHMS:
                        if os.path.exists(path(n)):
                            os.remove(path(n))
                    time.sleep(1.2)                   # daoGpuShmd drops the previous payloads
                    setup = {"cpu": "cpu", "gpu-nomirror": "nomirror"}.get(mode, "gpu")
                    subprocess.run([sys.executable, __file__, "--setup", setup, "--frames", str(args.frames),
                                    "--device", str(args.device)], env=env, check=True, capture_output=True)
                    procs = start_chain(mode, env, args.pipeline, cfg)
                    out_file = os.path.join(tmp, f"out_{mode}.bin")
                    r = subprocess.run([driver, path("pyrIm"), path("dm1ResWf"), frames_file, str(args.frames), out_file], env=env,
                                       capture_output=True, text=True, timeout=600)
                    stop_chain(procs)
                    lat_line = [l for l in r.stdout.splitlines() if l.startswith("LAT ")]
                    label = {"cpu": "4 processes, host SHMs", "gpu-copy": "GPU pipeline, copy frame",
                             "gpu": "GPU pipeline, zero copy", "gpu-nomirror": "zero copy, no mirrors"}[mode]
                    if not lat_line:
                        print(f"{'on' if use_mps else 'off':>4} {label:>28}  ERROR {r.stdout[-300:]}")
                        continue
                    lat = np.array([float(x) for x in lat_line[0].split()[1:]])
                    out = np.fromfile(out_file, np.float32).reshape(args.frames, NACT)[::50]
                    err = np.max(np.abs(out - ref) / (np.abs(ref).max() + 1e-12))
                    print(f"{'on' if use_mps else 'off':>4} {label:>28} {np.median(lat):8.1f} "
                          f"{np.percentile(lat, 99):8.1f} {lat.max():8.1f}  {err:.1e}")
            finally:
                if use_mps:
                    subprocess.run([MPS_HELPER, "stop"], env=env, capture_output=True)
        for n in SHMS:
            if os.path.exists(path(n)):
                os.remove(path(n))
        out = subprocess.run([os.path.join(BIN, "daoGpuShmd"), "--ping"], env=base, capture_output=True, text=True)
        if "pid" in out.stdout:
            os.kill(int(out.stdout.split("pid")[1]), signal.SIGTERM)


if __name__ == "__main__":
    main()
