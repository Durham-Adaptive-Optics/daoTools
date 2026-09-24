#!/usr/bin/env python3
"""
Round-trip latency of daoMvMGPU with host SHMs vs GPU SHMs.

For each case and matrix size: create the SHMs, start daoMvMGPU on them,
write N inputs one after the other and time each one until daoMvMGPU has
published the output. Outputs are checked against a CPU gemv.

    python3 tests/bench_mvm_gpu.py [--frames 2000] [--device 0]

Needs daoBase with GPU SHMs, daoMvMGPU built (build/apps), nvcc.
"""
import argparse
import os
import signal
import subprocess
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DAOROOT = os.environ.get("DAOROOT", os.path.expanduser("~/DAOROOT"))
MVM = os.path.join(ROOT, "build", "apps", "daoMvMGPU")
MODES = ["cpu", "gpu", "gpu-mirror", "gpu-kernel"]
SIZES = [(1024, 512), (8192, 2048)]
SHMS = ["/tmp/bmvm_in.im.shm", "/tmp/bmvm_mat.im.shm", "/tmp/bmvm_out.im.shm"]


def build(tmp):
    exe = os.path.join(tmp, "benchMvMGPU")
    subprocess.run(["nvcc", "-O2", "-I", os.path.join(DAOROOT, "include"), os.path.join(HERE, "benchMvMGPU.cu"),
                    "-o", exe, "-L", os.path.join(DAOROOT, "lib"), "-ldao"], check=True, capture_output=True)
    return exe


def clean():
    for s in SHMS:
        if os.path.exists(s):
            os.remove(s)


def run_case(exe, mode, nin, nout, frames, device):
    clean()
    time.sleep(1.2)                       # let daoGpuShmd drop the previous payloads
    out = subprocess.run([exe, "setup", mode, str(nin), str(nout), str(device)], capture_output=True, text=True)
    if "SETUP OK" not in out.stdout:
        return None, "setup failed: " + out.stdout[-200:] + out.stderr[-300:]
    mvm = subprocess.Popen([MVM, "-S", *SHMS, "-s", "0", "-G", str(device), "-L"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(2.0)                       # CUDA init, cuBLAS warm-up, graph capture
    res = subprocess.run([exe, "run", mode, str(frames)], capture_output=True, text=True, timeout=300)
    mvm.send_signal(signal.SIGINT)
    try:
        log = mvm.communicate(timeout=10)[0]
    except subprocess.TimeoutExpired:
        mvm.kill()
        log = mvm.communicate()[0]
    lat = check = None
    for line in res.stdout.splitlines():
        if line.startswith("LAT "):
            lat = np.array([float(x) for x in line.split()[1:]])
        if line.startswith("CHECK"):
            check = line
    if lat is None:
        return None, res.stdout[-300:] + res.stderr[-300:] + " | mvm: " + log[-400:]
    where = [l.split("] ")[-1].strip() for l in log.splitlines() if "input " in l and "output " in l]
    return (lat, check, where[0] if where else ""), None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--frames", type=int, default=2000)
    p.add_argument("--device", type=int, default=0)
    args = p.parse_args()
    env_ok = os.path.exists(MVM)
    if not env_ok:
        raise SystemExit(f"{MVM} not found: build daoTools first")
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        print(f"{'size (in x out)':>16} {'case':>11} {'median':>8} {'p99':>8} {'max':>8}  check   "
              f"(round trip in us, {args.frames} frames)")
        for nin, nout in SIZES:
            for mode in MODES:
                result, err = run_case(exe, mode, nin, nout, args.frames, args.device)
                if err:
                    print(f"{nin:>7} x {nout:<6} {mode:>11}  ERROR {err}")
                    continue
                lat, check, where = result
                print(f"{nin:>7} x {nout:<6} {mode:>11} {np.median(lat):8.1f} {np.percentile(lat, 99):8.1f} "
                      f"{lat.max():8.1f}  {check.split()[1]:<5}  {where}")
        clean()


if __name__ == "__main__":
    main()
