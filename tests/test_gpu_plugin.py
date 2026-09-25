#!/usr/bin/env python3
"""
daoGpuPipeline plugins: loads the example plugin (build/tests/libgpuPluginExample.so,
stage type "scale") in a pipeline slice -> scale, and checks the output of each
frame (out = k * in[offset:offset + n]), including after k changes, and that
cnt2 follows the input.

    python3 tests/test_gpu_plugin.py        (after waf build; exit code = failures)
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

D = "/dev/shm/daoGpuPluginTest"


def main():
    os.makedirs(D, exist_ok=True)
    P = lambda n: f"{D}/{n}.im.shm"
    src = daoShm.shm(P("gplIn"), np.zeros((208, 1), np.float32))
    daoShm.shm(P("gplSlice"), np.zeros((104, 1), np.float32))
    k = daoShm.shm(P("gplK"), np.full((1, 1), 2.0, np.float32))
    out = daoShm.shm(P("gplOut"), np.zeros((104, 1), np.float32))
    with tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False) as f:
        f.write(f"""device: 0
trigger: {{shm: {P('gplIn')}, sem: 1}}
plugins: [{os.path.join(BUILD, 'tests', 'libgpuPluginExample.so')}]
stages:
  - slice: {{in: {P('gplIn')}, out: {P('gplSlice')}, offset: 50}}
  - scale: {{in: {P('gplSlice')}, k: {P('gplK')}, out: {P('gplOut')}}}
""")
        cfg = f.name
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(BUILD, "src") + ":" + os.environ.get("LD_LIBRARY_PATH", ""))
    p = subprocess.Popen([os.path.join(BUILD, "apps", "daoGpuPipeline"), "-c", cfg, "-L"], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(2)
    failures = 0
    rng = np.random.default_rng(0)
    for step, kval in enumerate((2.0, -0.5)):
        if step:
            k.set_data(np.full((1, 1), kval, np.float32))
        for i in range(20):
            x = rng.standard_normal((208, 1)).astype(np.float32)
            src.image.md[0].cnt2 = 100 * step + i
            c0 = out.get_counter()
            src.set_data(x)
            t0 = time.time()
            while out.get_counter() == c0 and time.time() - t0 < 2:
                time.sleep(0.0005)
            ok = np.allclose(out.get_data().ravel(), kval * x.ravel()[50:154]) and \
                out.image.md[0].cnt2 == 100 * step + i
            failures += not ok
        print(f"k = {kval:5.2f}: 20 frames {'PASS' if not failures else 'FAIL'}")
    p.send_signal(signal.SIGINT)
    p.communicate(timeout=10)
    os.remove(cfg)
    for n in os.listdir(D):
        os.remove(os.path.join(D, n))
    os.rmdir(D)
    for n in ("gplIn", "gplSlice", "gplK", "gplOut"):
        for sem in os.listdir("/dev/shm"):
            if sem.startswith(f"sem.{n}_"):
                os.remove(os.path.join("/dev/shm", sem))
    print("ALL PASSED" if not failures else f"FAILED: {failures}")
    return failures


if __name__ == "__main__":
    sys.exit(main())
