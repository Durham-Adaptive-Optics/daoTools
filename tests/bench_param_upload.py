#!/usr/bin/env python3
"""
daoGpuPipeline parameter uploads: a matrix rewritten while the frames run, with
paramUpload: sync (the frame waits for the copy) and async (copied in the background).

One mvm stage, a matrix of --size x --size floats (7071: 200 MB), its input copied in
(hostAccess: copy, as a camera frame), frames fed back to back by pipelineDriver.c; a
thread writes 3 new matrices, one a second. For each mode: the frame latencies (median,
99.9 %, worst, the frames over 1 ms and where they are) and, for every frame, which
matrix its output came from (the first 64 rows checked): each output must come from one
of them, and never from an older one than the frame before.

    python3 tests/bench_param_upload.py [--frames 8000] [--size 7071] [--device 0]
                                        [--modes sync,async] [--cpu 4]
Exit code: the frames whose output came from no matrix or an older one.
"""
import argparse
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DAOROOT = os.environ.get("DAOROOT", os.path.expanduser("~/DAOROOT"))
sys.path.insert(0, os.path.join(DAOROOT, "python"))
import daoShm  # noqa: E402

D = "/dev/shm/daoParamUploadBench"
NEW = 3                                                   # matrices written during the run


def P(name):
    return f"{D}/{name}.im.shm"


def run(mode, args, mats, xs, frames_file, driver, tmp):
    x = daoShm.shm(P("puX"), np.zeros((args.size, 1), np.float32))
    m = daoShm.shm(P("puM"), mats[0])
    daoShm.shm(P("puY"), np.zeros((args.size, 1), np.float32))
    cfg = os.path.join(tmp, f"{mode}.yaml")
    with open(cfg, "w") as f:
        f.write(f"""device: {args.device}
trigger: {{shm: {P('puX')}, wait: spin}}
hostAccess: copy
paramUpload: {mode}
stages:
  - mvm: {{in: {P('puX')}, matrix: {P('puM')}, out: {P('puY')}}}
""")
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(ROOT, "build", "src") + ":"
               + os.environ.get("LD_LIBRARY_PATH", ""))
    p = subprocess.Popen([os.path.join(ROOT, "build", "apps", "daoGpuPipeline"), "-c", cfg, "-C", str(args.cpu), "-L"],
                         env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(4)

    def writer():
        for j in range(1, NEW + 1):
            time.sleep(1.0)
            m.set_data(mats[j])
    th = threading.Thread(target=writer)
    th.start()
    out_file = os.path.join(tmp, "out.bin")
    r = subprocess.run([driver, P("puX"), P("puY"), frames_file, str(args.frames), out_file], env=env,
                       capture_output=True, text=True, timeout=600)
    th.join()
    p.send_signal(signal.SIGINT)
    p.wait(timeout=20)
    line = [l for l in r.stdout.splitlines() if l.startswith("LAT ")]
    if not line:
        print(f"{mode:>6}: driver failed: {r.stdout[-200:]}")
        return args.frames
    lat = np.array([float(v) for v in line[0].split()[1:]])
    out = np.fromfile(out_file, np.float32).reshape(args.frames, args.size)
    used = np.full(args.frames, -1)
    for j, mat in enumerate(mats):
        ref = xs @ mat[:64].T
        ok = np.all(np.abs(out[:, :64] - ref) <= 1e-3 * (np.abs(ref) + 1), axis=1)
        used[ok & (used < 0)] = j
    seen = used[used >= 0]
    bad = int((used < 0).sum()) + int((np.diff(seen) < 0).sum())
    switches = [int(np.argmax(used == j)) for j in range(1, NEW + 1) if (used == j).any()]
    slow = [(int(i), int(lat[i])) for i in np.nonzero(lat > 1000)[0]]
    during = np.concatenate([lat[s - 30:s] for s in switches if s >= 30]) if switches else lat
    print(f"{mode:>6}: median {np.median(lat):6.0f} us  99.9 % {np.percentile(lat, 99.9):6.0f}  "
          f"worst {lat.max():7.0f}  frames > 1 ms {len(slow):3d}  matrices used {sorted(set(seen.tolist()))}"
          f"  {'OK' if not bad else f'{bad} BAD'}")
    print(f"        the 30 frames before each switch: median {np.median(during):.0f} us; "
          f"switches at frames {switches}; frames > 1 ms (frame, us): {slow[:8]}")
    for n in os.listdir(D):
        if n.endswith(".im.shm"):
            os.remove(os.path.join(D, n))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=8000)
    ap.add_argument("--size", type=int, default=7071, help="the matrix is size x size floats (7071: 200 MB)")
    ap.add_argument("--device", type=int, default=0)
    ap.add_argument("--modes", default="sync,async")
    ap.add_argument("--cpu", type=int, default=4, help="the pipeline's loop thread (-C)")
    args = ap.parse_args()
    os.makedirs(D, exist_ok=True)
    rng = np.random.default_rng(0)
    mats = [rng.standard_normal((args.size, args.size), dtype=np.float32) * np.float32(0.01) + np.float32(j)
            for j in range(NEW + 1)]
    xs = rng.standard_normal((args.frames, args.size)).astype(np.float32)
    failures = 0
    with tempfile.TemporaryDirectory(prefix="pub") as tmp:
        frames_file = os.path.join(tmp, "frames.bin")
        xs.tofile(frames_file)
        driver = os.path.join(tmp, "driver")
        subprocess.run(["gcc", "-O2", "-I", os.path.join(DAOROOT, "include"), os.path.join(HERE, "pipelineDriver.c"),
                        "-o", driver, "-L", os.path.join(DAOROOT, "lib"), "-L", os.path.join(DAOROOT, "lib64"), "-ldao"],
                       check=True)
        print(f"mvm {args.size} x {args.size} ({args.size * args.size * 4 / 1e6:.0f} MB), {args.frames} frames, "
              f"{NEW} new matrices, one a second; us from the input written to the output published")
        for mode in args.modes.split(","):
            failures += run(mode, args, mats, xs, frames_file, driver, tmp)
    for n in os.listdir(D):
        os.remove(os.path.join(D, n))
    os.rmdir(D)
    for s in os.listdir("/dev/shm"):
        if s.startswith(("sem.puX_", "sem.puM_", "sem.puY_")):
            os.remove(os.path.join("/dev/shm", s))
    return failures


if __name__ == "__main__":
    sys.exit(main())
