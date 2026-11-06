#!/usr/bin/env python3
"""
daoCpuPipeline plugins: loads the example plugin (build/tests/libcpuPluginExample.so,
stage type "scale") in a pipeline slice -> scale -> applyGain (a built-in stage after
the plugin's), and checks, frame by frame:
  - scale's output: k * in[offset:offset + n], published with in's cnt2, including after
    k changes;
  - applyGain's, computed from it;
  - gate 0: scale silent, nothing published by it or by applyGain (slice still is);
    gate 1 again: both published again;
and that a library without daoCpuPluginRegister (a daoGpuPipeline plugin, or none) is
refused with a clear error.

    python3 tests/test_cpu_plugin.py        (after waf build; exit code = failures)
"""
import os
import signal
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(os.path.dirname(HERE), "build")
DAOROOT = os.environ.get("DAOROOT", os.path.expanduser("~/DAOROOT"))
sys.path.insert(0, os.path.join(DAOROOT, "python"))
import daoShm  # noqa: E402

D = "/dev/shm/daoCpuPluginTest"
PIPELINE = os.path.join(BUILD, "apps", "daoCpuPipeline")
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.join(BUILD, "src") + ":" + os.environ.get("LD_LIBRARY_PATH", ""))
NAMES = ("cplIn", "cplSlice", "cplK", "cplGate", "cplScaled", "cplG", "cplOut")


def P(n):
    return f"{D}/{n}.im.shm"


def config(tmp, plugin, name="p.yaml"):
    f = os.path.join(tmp, name)
    with open(f, "w") as fh:
        fh.write(f"""trigger: {{shm: {P('cplIn')}}}
plugins: [{plugin}]
stages:
  - slice: {{in: {P('cplIn')}, out: {P('cplSlice')}, offset: 50}}
  - scale: {{in: {P('cplSlice')}, k: {P('cplK')}, gate: {P('cplGate')}, out: {P('cplScaled')}}}
  - applyGain: {{in: {P('cplScaled')}, gain: {P('cplG')}, out: {P('cplOut')}}}
""")
    return f


def frames(shms, rng, n, cnt2, silent):
    """n frames; returns the failures"""
    src, sl, k, sc, out = shms
    bad = 0
    for i in range(n):
        x = rng.standard_normal((208, 1)).astype(np.float32)
        before = (sl.get_counter(), sc.get_counter(), out.get_counter())
        src.image.md[0].cnt2 = cnt2 + i
        src.set_data(x)
        t0 = time.time()
        while sl.get_counter() == before[0] and time.time() - t0 < 2:
            time.sleep(0.0005)
        if silent:
            time.sleep(0.02)                              # nothing else may come
            ok = sl.get_counter() == before[0] + 1 and sc.get_counter() == before[1] \
                and out.get_counter() == before[2]
        else:
            while out.get_counter() == before[2] and time.time() - t0 < 2:
                time.sleep(0.0005)
            ref = k.get_data().ravel()[0] * x.ravel()[50:154]
            ok = sc.get_counter() == before[1] + 1 and out.get_counter() == before[2] + 1 \
                and np.array_equal(sc.get_data().ravel(), ref.astype(np.float32)) \
                and np.array_equal(out.get_data().ravel(), (np.float32(3) * ref).astype(np.float32)) \
                and sc.image.md[0].cnt2 == cnt2 + i
        bad += not ok
    return bad


def refused(tmp, lib):
    """the pipeline exits with the plugin error for this library"""
    p = subprocess.run([PIPELINE, "-c", config(tmp, lib, "bad.yaml"), "-L"], env=ENV, capture_output=True,
                       text=True, timeout=30)
    return p.returncode != 0 and "no daoCpuPluginRegister()" in p.stdout + p.stderr


def main():
    os.makedirs(D, exist_ok=True)
    src = daoShm.shm(P("cplIn"), np.zeros((208, 1), np.float32))
    sl = daoShm.shm(P("cplSlice"), np.zeros((104, 1), np.float32))
    k = daoShm.shm(P("cplK"), np.full((1, 1), 2.0, np.float32))
    gate = daoShm.shm(P("cplGate"), np.ones((1, 1), np.uint32))
    sc = daoShm.shm(P("cplScaled"), np.zeros((104, 1), np.float32))
    daoShm.shm(P("cplG"), np.full((1, 1), 3.0, np.float32))
    out = daoShm.shm(P("cplOut"), np.zeros((104, 1), np.float32))
    shms = (src, sl, k, sc, out)
    failures = 0
    rng = np.random.default_rng(0)
    with tempfile.TemporaryDirectory(prefix="cpl") as tmp:
        p = subprocess.Popen([PIPELINE, "-c", config(tmp, os.path.join(BUILD, "tests", "libcpuPluginExample.so")),
                              "-L"], env=ENV, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        time.sleep(1.5)
        steps = [("k = 2", {}, False), ("k = -0.5", {"k": -0.5}, False), ("gate 0: silent", {"gate": 0}, True),
                 ("gate 1 again", {"gate": 1}, False)]
        for step, (name, change, silent) in enumerate(steps):
            if "k" in change:
                k.set_data(np.full((1, 1), change["k"], np.float32))
            if "gate" in change:
                gate.set_data(np.full((1, 1), change["gate"], np.uint32))
            bad = frames(shms, rng, 20, 100 * step, silent)
            print(f"{name:>16}: 20 frames {'PASS' if not bad else f'FAIL ({bad})'}")
            failures += bad
        p.send_signal(signal.SIGINT)
        p.communicate(timeout=10)
        if p.returncode != 0:
            print(f"pipeline exit code {p.returncode}: FAIL")
            failures += 1
        for name, lib in (("no daoCpuPluginRegister (libdaoTools)", os.path.join(BUILD, "src", "libdaoTools.so")),
                          ("a daoGpuPipeline plugin", os.path.join(BUILD, "tests", "libgpuPluginExample.so"))):
            if not os.path.exists(lib):
                print(f"{name:>16}: skipped ({lib} not built)")
                continue
            ok = refused(tmp, lib)
            print(f"{name}: refused {'PASS' if ok else 'FAIL'}")
            failures += not ok
    for n in os.listdir(D):
        os.remove(os.path.join(D, n))
    os.rmdir(D)
    for sem in os.listdir("/dev/shm"):
        if sem.startswith("sem.") and sem[4:].rsplit("_", 1)[0] in NAMES:
            os.remove(os.path.join("/dev/shm", sem))
    print("ALL PASSED" if not failures else f"FAILED: {failures}")
    return failures


if __name__ == "__main__":
    sys.exit(main())
