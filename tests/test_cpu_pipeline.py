#!/usr/bin/env python3
"""
daoCpuPipeline gives the outputs of the daoTools apps it replaces, bit for bit.

Two chains, each run once as separate processes (the apps) and once as daoCpuPipeline,
on two copies of the same SHMs (prefixes "a" and "p"), fed the same frames:

    SH and control, triggered by im (uint16 48x48, 6x6 sub-apertures of 8 px):
      pixelCalibrate -> centroid -> slice (the slopes) -> mvm -> applyGain (modal)
      -> leakyIntegrator (modal, enable, clip)
    pixels, triggered by im2 (float 48x48), each stage reading im2:
      pixelExtract, pixelSubstractExtract (norm), centroidRelativeRef, centroidWindows (cog)

Every output is compared after each frame, and its cnt2, while the flat field changes
(read in place), the integrator is disabled (zeros once, then silent), its loop opened
(zeros every frame) and closed again.

    python3 tests/test_cpu_pipeline.py [--workers 4,6,8,10]    (after waf build; exit code = failures)

--workers: the pipelines split their stages over workers (daoCpuPipeline -w), the first
half of the CPUs for the first chain, the second half for the other; the apps run on
the other CPUs (a worker spins: a core of its own).
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
BUILD = os.path.join(os.path.dirname(HERE), "build")
DAOROOT = os.environ.get("DAOROOT", os.path.expanduser("~/DAOROOT"))
sys.path.insert(0, os.path.join(DAOROOT, "python"))
import daoShm  # noqa: E402

D = "/dev/shm/daoCpuPipelineTest"
BOX, NG = 8, 6
NX, NSUBA, NACT = BOX * NG, NG * NG, 40
SH_OUT = ["cal", "cent", "slopes", "modes", "modesG", "cmd"]
PIX_OUT = ["ext1", "ext2", "cent2", "win"]


def P(prefix, name):
    return f"{D}/{prefix}{name}.im.shm"


def spots(rng, raw):
    """A frame of NSUBA spots, each shifted at random by up to 2 px."""
    y, x = np.mgrid[0:NX, 0:NX]
    img = np.full((NX, NX), 100.0)
    for s in range(NSUBA):
        cx = (s % NG) * BOX + BOX / 2 - 0.5 + rng.uniform(-2, 2)
        cy = (s // NG) * BOX + BOX / 2 - 0.5 + rng.uniform(-2, 2)
        img += 2000 * np.exp(-((x - cx) ** 2 + (y - cy) ** 2) / (2 * 1.2 ** 2))
    img += rng.normal(0, 5, img.shape)
    return np.clip(img, 0, 65535).astype(np.uint16) if raw else img.astype(np.float32)


def create(prefix, rng):
    """The SHMs of both chains, for one prefix; the same values for both prefixes (same seed)."""
    c = (np.arange(NG) * BOX + BOX / 2 - 0.5).astype(np.float32)
    cx, cy = np.meshgrid(c, c)
    centres = np.concatenate([cx.ravel(), cy.ravel()])[:, None]
    mask = (rng.uniform(size=(NX, NX)) > 0.3).astype(np.uint32)
    table = np.zeros((NSUBA, 9), np.float32)                  # one cog window per sub-aperture
    table[:, 0], table[:, 1], table[:, 2], table[:, 3] = cx.ravel(), cy.ravel(), BOX, BOX
    s = {}
    def mk(name, a):
        s[name] = daoShm.shm(P(prefix, name), a)
    mk("im", np.zeros((NX, NX), np.uint16))
    mk("ff", (1 + 0.05 * rng.standard_normal((NX, NX))).astype(np.float32))
    mk("bg", np.full((NX, NX), 100, np.float32))
    mk("cal", np.zeros((NX, NX), np.float32))
    mk("ref", centres)
    mk("thr", np.full((1, 1), 0.2, np.float32))
    mk("cent", np.zeros((3 * NSUBA, 1), np.float32))
    mk("slopes", np.zeros((2 * NSUBA, 1), np.float32))
    mk("M", (0.1 * rng.standard_normal((NACT, 2 * NSUBA))).astype(np.float32))
    mk("modes", np.zeros((NACT, 1), np.float32))
    mk("g", rng.uniform(0.5, 1.5, (NACT, 1)).astype(np.float32))
    mk("modesG", np.zeros((NACT, 1), np.float32))
    mk("offset", (0.01 * rng.standard_normal((NACT, 1))).astype(np.float32))
    mk("loop", np.ones((1, 1), np.uint32))
    mk("enable", np.ones((1, 1), np.uint32))
    mk("leak", np.full((NACT, 1), 0.99, np.float32))
    mk("igain", np.full((NACT, 1), 0.4, np.float32))
    mk("cmd", np.zeros((NACT, 1), np.float32))
    mk("im2", np.zeros((NX, NX), np.float32))
    mk("mask", mask)
    mk("sub", np.full((NX, NX), 90, np.float32))
    mk("norm", np.full((1, 1), 1000, np.float32))
    mk("ext1", np.zeros((int(mask.sum()), 1), np.float32))
    mk("ext2", np.zeros((int(mask.sum()), 1), np.float32))
    mk("cent2", np.zeros((4 * NSUBA, 1), np.float32))         # x, y, flux, weight
    mk("absThr", np.full((1, 1), 150, np.float32))
    mk("table", table)
    mk("win", np.zeros((3 * NSUBA, 1), np.float32))
    return s


def apps(a):
    p = lambda n: P(a, n)
    return [
        ["daoPixelCalibrate", "-S", p("im"), p("ff"), p("bg"), p("cal"), "-L"],
        ["daoComputeCentroid", "-S", p("cal"), p("cent"), p("ref"), p("thr"), str(BOX), str(NSUBA), "-L"],
        ["daoShmSlice", "-S", p("cent"), p("slopes"), "-L"],
        ["daoMvM", "-S", p("slopes"), p("M"), p("modes"), "-N", "1", "-L"],
        ["daoApplyGain", "-S", p("modes"), p("g"), p("modesG"), "-m", "-L"],
        ["daoLeakyIntegrator", "-S", p("modesG"), p("offset"), p("cmd"), p("loop"), p("igain"), p("leak"),
         "-e", p("enable"), "-c", "0.5", "-m", "-L"],
        ["daoPixelExtract", "-S", p("im2"), p("mask"), p("ext1"), "-L"],
        ["daoPixelSubstractExtract", "-S", p("im2"), p("sub"), p("mask"), p("ext2"), p("norm"), "-N", "-L"],
        ["daoComputeCentroidRelativeRef", "-S", p("im2"), p("cent2"), p("ref"), p("ref"), p("thr"), str(BOX),
         str(NSUBA), "-L"],
        ["daoComputeCentroidWindows", "-S", p("im2"), p("win"), p("table"), "-", p("absThr"), "-m", "cog",
         "-p", "barycenter", "-L"],
    ]


def configs(q, tmp):
    p = lambda n: P(q, n)
    sh = f"""trigger: {{shm: {p('im')}}}
stages:
  - pixelCalibrate: {{in: {p('im')}, ff: {p('ff')}, bg: {p('bg')}, out: {p('cal')}}}
  - centroid: {{in: {p('cal')}, ref: {p('ref')}, threshold: {p('thr')}, out: {p('cent')}, subaSize: {BOX},
                nbSuba: {NSUBA}}}
  - slice: {{in: {p('cent')}, out: {p('slopes')}}}
  - mvm: {{in: {p('slopes')}, matrix: {p('M')}, out: {p('modes')}}}
  - applyGain: {{in: {p('modes')}, gain: {p('g')}, out: {p('modesG')}, modal: true}}
  - leakyIntegrator: {{in: {p('modesG')}, out: {p('cmd')}, loop: {p('loop')}, gain: {p('igain')},
                       leak: {p('leak')}, enable: {p('enable')}, offset: {p('offset')}, modal: true, clip: 0.5}}
"""
    pix = f"""trigger: {{shm: {p('im2')}}}
stages:
  - pixelExtract: {{in: {p('im2')}, mask: {p('mask')}, out: {p('ext1')}}}
  - pixelSubstractExtract: {{in: {p('im2')}, sub: {p('sub')}, mask: {p('mask')}, out: {p('ext2')},
                             norm: {p('norm')}}}
  - centroidRelativeRef: {{in: {p('im2')}, subApCentre: {p('ref')}, ref: {p('ref')}, threshold: {p('thr')},
                           out: {p('cent2')}, subaSize: {BOX}, nbSuba: {NSUBA}}}
  - centroidWindows: {{in: {p('im2')}, table: {p('table')}, threshold: {p('absThr')}, out: {p('win')},
                       method: cog, peak: barycenter}}
"""
    files = []
    for name, text in (("sh", sh), ("pix", pix)):
        f = os.path.join(tmp, f"{name}.yaml")
        with open(f, "w") as fh:
            fh.write(text)
        files.append(f)
    return files


def wait_counters(shms, before, timeout=2.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if all(s.get_counter() > c for s, c in zip(shms, before)):
            return True
        time.sleep(0.0005)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workers", default="", help="CPUs of the pipelines' workers, e.g. 4,6,8,10")
    args = ap.parse_args()
    cpus = [int(c) for c in args.workers.split(",") if c]
    half = (len(cpus) + 1) // 2
    workers = [cpus[:half], cpus[half:]]
    others = sorted(os.sched_getaffinity(0) - set(cpus))
    pin = (lambda: os.sched_setaffinity(0, others)) if cpus else None
    os.makedirs(D, exist_ok=True)
    A = create("a", np.random.default_rng(3))
    Q = create("p", np.random.default_rng(3))
    env = dict(os.environ, PATH=os.path.join(BUILD, "apps") + ":" + os.environ["PATH"],
               LD_LIBRARY_PATH=os.path.join(BUILD, "src") + ":" + os.environ.get("LD_LIBRARY_PATH", ""))
    failures = 0
    with tempfile.TemporaryDirectory(prefix="cpp") as tmp:
        procs = [subprocess.Popen(c, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=pin)
                 for c in apps("a")]
        for c, w in zip(configs("p", tmp), workers):
            cmd = [os.path.join(BUILD, "apps", "daoCpuPipeline"), "-c", c]
            cmd += ["-w", ",".join(map(str, w))] if w else []
            procs.append(subprocess.Popen(cmd + ["-L"], env=env, stdout=subprocess.DEVNULL,
                                          stderr=subprocess.DEVNULL, preexec_fn=pin))
        if cpus:
            print(f"workers: {workers[0]} (first chain), {workers[1]} (second chain)")
        time.sleep(3)                                     # the apps sleep 2 s before their loop
        rng = np.random.default_rng(7)
        # warm-up in open loop (zeros, the integrator's state reset): both chains then
        # start the closed loop from the same state, whatever happened at start-up
        phases = [("warm-up, loop open", 3, {"loop": 0}), ("closed loop", 20, {"loop": 1}),
                  ("new flat field", 10, {"ff": None}),
                  ("integrator disabled", 8, {"enable": 0}), ("enabled, loop open", 8, {"enable": 1, "loop": 0}),
                  ("loop closed again", 10, {"loop": 1})]
        frame = 0
        for name, nframes, change in phases:
            for key, v in change.items():
                for S in (A, Q):
                    if key == "ff":
                        S[key].set_data((1 + 0.05 * np.random.default_rng(9).standard_normal((NX, NX)))
                                        .astype(np.float32))
                    else:
                        S[key].set_data(np.full((1, 1), v, np.uint32))
            bad = 0
            for _ in range(nframes):
                im, im2 = spots(rng, True), spots(rng, False)
                silent = change.get("enable") == 0 and _ > 0     # disabled: zeros once, then nothing
                outs = [n for n in SH_OUT if not (silent and n == "cmd")] + PIX_OUT
                before = [S[n].get_counter() for S in (A, Q) for n in outs]
                silentBefore = [S["cmd"].get_counter() for S in (A, Q)]
                for S in (A, Q):
                    S["im"].image.md[0].cnt2 = frame
                    S["im2"].image.md[0].cnt2 = frame
                    S["im"].set_data(im)
                    S["im2"].set_data(im2)
                if not wait_counters([S[n] for S in (A, Q) for n in outs], before):
                    print(f"  frame {frame}: TIMEOUT")
                    bad += 1
                    frame += 1
                    continue
                if silent:
                    time.sleep(0.01)
                    if [S["cmd"].get_counter() for S in (A, Q)] != silentBefore:
                        print(f"  frame {frame}: cmd published while disabled")
                        bad += 1
                for n in SH_OUT + PIX_OUT:
                    a, q = A[n].get_data(), Q[n].get_data()
                    if not np.array_equal(a, q) or A[n].image.md[0].cnt2 != Q[n].image.md[0].cnt2:
                        d = np.abs(a.astype(np.float64) - q).max()
                        print(f"  frame {frame}: {n} differs (max {d:.3g}, cnt2 {A[n].image.md[0].cnt2} vs "
                              f"{Q[n].image.md[0].cnt2})")
                        bad += 1
                frame += 1
            zeros = not Q["cmd"].get_data().any()
            print(f"{name:>22}: {nframes} frames {'PASS' if not bad else 'FAIL'}"
                  f"{'  (cmd zero)' if zeros else ''}")
            failures += bad
        for pr in procs:
            pr.send_signal(signal.SIGINT)
        for pr in procs:
            try:
                pr.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pr.kill()
    names = set()
    for n in os.listdir(D):
        names.add(n.split(".")[0])
        os.remove(os.path.join(D, n))
    os.rmdir(D)
    for sem in os.listdir("/dev/shm"):
        if sem.startswith("sem.") and sem[4:].rsplit("_", 1)[0] in names:
            os.remove(os.path.join("/dev/shm", sem))
    print("ALL PASSED" if not failures else f"FAILED: {failures}")
    return failures


if __name__ == "__main__":
    sys.exit(main())
