# daoTools [![daoTools](https://github.com/Durham-Adaptive-Optics/daoTools/actions/workflows/main.yml/badge.svg?branch=CI-Workflow)](https://github.com/Durham-Adaptive-Optics/daoTools/actions/workflows/main.yml)
`daoTools` is the application layer of **DAO** (Durham Adaptive Optics), a real-time control system for adaptive optics instruments. It builds on [`daoBase`](https://github.com/Durham-Adaptive-Optics/daoBase)'s shared-memory (`dao.shm`) and messaging primitives to provide everything needed to run an AO real-time control loop end to end: pixel calibration, wavefront-sensor centroiding (Shack-Hartmann, pyramid, correlation-based), matrix-vector-multiply reconstruction (CPU/BLAS and GPU/CUDA), leaky-integrator and modal loop control, plus the calibration, simulation, timing/latency, and data-conversion tools needed to operate and debug a pipeline.

It ships three things: a C function library (`libdaoTools`) that the real-time per-pixel/per-mode math lives in, 70+ command-line applications in `apps/` that are thin real-time wrappers around it, and a set of PyQt5 GUIs in `gui/` for live monitoring and control. Everything communicates over `dao.shm`, so any of these pieces can be mixed, replaced, or driven from your own code independently.

# The daoTools library

`libdaoTools` (built from `src/c/daoTools.c`, declared in `include/daoTools.h`)
is the C function library most of the `apps/` binaries below are thin
command-line wrappers around. It covers the operations that recur across an
AO real-time pipeline, operating directly on `dao.shm` `IMAGE`s:

- **Pixel calibration** — `daoToolsShmCalibrate`/`64` (dark-subtract + flat-field,
  float and double precision), `daoToolsShmCalibratePws` (pyramid WFS variant).
- **Centroiding** — `daoCentroidSpots` (windowed center-of-gravity),
  `daoCentroidSpotsRelative`/`RelativeRef` (differential centroiding),
  `daoCentroidSpotsCorrelation` (windowed correlation search) and, when FFTW
  is available, its FFT-based counterpart (`src/c/daoToolsCorrFFT.c`,
  `include/daoToolsCorrFFT.h`) which searches the whole sub-aperture via one
  FFT round trip instead of a shift search; `daoCentroidPws` for pyramid WFS.
- **Control loop primitives** — `daoToolsLeakyIntegrator`/`Double` and the
  per-mode `daoToolsLeakyModalIntegrator`/`Double`, `daoToolsHighPassFilter`/
  `Double`, `daoToolsCommandFilter`.
- **Pyramid WFS pixel pipelines** — `daoToolsShmExtract`,
  `daoToolsShmSubstractExtract` and its normalized variants
  (`...Norm`, `...NormA`, `...DualNorm`, plus their `...Finalize` companions).
- **Misc.** — `daoDmCombine` (multi-channel DM command summing),
  `daoDescrambleOcam2Image` (OCAM2K scrambled-readout reordering),
  `daoToolsImgNormalize`, `daoComputeChecksum`, `daoRtSetup`/`daoToolsEnableFTZ`
  (real-time process setup, denormal flush-to-zero), `daoToolsSetRtPriority`
  (request `SCHED_FIFO` real-time priority with a safe, logged fallback --
  see [Real-time scheduling priority](#real-time-scheduling-priority) below),
  and `daoLogToFile` (throttled, size-rotated file logging).

Most `apps/*.c` binaries call straight into this library, add SHM
attach/argument-parsing/real-time-loop boilerplate, and nothing else — the
library is where the actual per-pixel/per-mode math lives.

# daoTools.py

`src/python/daoTools.py` is a general-purpose Python AO toolbox, independent
of the C library above: a `Fifo` circular buffer, basic FITS read/write
helpers, pupil/circular-mask generation, center-of-gravity (`cog`), a
`Hadamard` basis, PSD/PSF computation (`computePSD`, `computePsf`,
`computePsfRef`), a `ShackHartmannWFS` simulation class and a pyramid-WFS
image simulator (`pwfsImage`). It predates and is separate from the
`daoToolsLib`/pipeline-scaffolding modules also under `src/python/`.

# Tools

`apps/` ships 70+ command-line tools. Full usage/flags are in the Sphinx docs
(`waf build_docs`, or `docs/source/apps.rst`); this is the index:

| Category | Tools |
|---|---|
| **Wavefront sensing & centroiding** | `daoComputeCentroid` (core Shack-Hartmann COG) and its `Pws` / `Relative` / `RelativeRef` / `Correlation` / `CorrelationFFT` variants, `daoComputeCentroids.py` / `daoComputeCentroidsSlow.py` (Python reference), `daoComputeIntensityPws`, `daoPrepCentroidLut.py`, `daoPrepPwfs.py` |
| **Matrix-vector multiply** | `daoMvM` (CPU, needs BLAS), `daoMvMGPU` (CUDA), `daoMvM.py` (Python reference) |
| **Pixel calibration** | `daoPixelCalibrate` / `daoPixelCalibratePws`, `daoTakeBg.py` |
| **SHM utilities** | `daoShmMonitoring` / `daoShmMonitoring1Value`, `daoShm2Fits`, `daoFits2Shm.py`, `daoSnapshot.py`, `daoShmRate.py` |
| **SHM arithmetic** | `daoShmAdd`, `daoShmCombiner`, `daoShmConcatenate` / `daoShmConcatenateFine`, `daoAvgShm` / `daoAvgDoubleShm`, `daoStatShm`, `daoDownsample`, `daoPixelExtract`, `daoApplyGain` |
| **Loop & filter** | `daoLeakyIntegrator` / `daoLeakyIntegratorMap`, `daoClock`, `daoHighPassFilter`, `daoModesCutoff` / `daoModesCutoffFull`, `daoCommandFilter` |
| **Timing & latency** | `daoTimeDiff`, `daoTimeDiffNCurse`, `daoTimeDiffStat`, `daoSetLatency`, `daoPlotLatency.py` |
| **Data conversion & I/O** | `daoNpy2Shm.py`, `daoTakeDataCubeFITS.py`, `daoTakeDataCubeNPY.py`, `daoDescrambleOcam2`, `daoDMSend`, `daoRandImageU16Write`, `daoRandWriter` / `daoRandWriterSync` |
| **Simulation** | `daoReconstructor.py` (Python MVM reference), `daoTurbulenceSimulator.py`, `daoPwfsSimulator.py`, `daoNoisyPsfGenerator.py` |
| **Logging & communication** | `daoRecvLogs.py`, `daoSendLogs.py`, `daoLogToScreen.py`, `daoProxyLog.py`, `daoSendCommand.py`, `daoStrCmd.py` / `daoReadStr.py` / `daoWriteStr.py`, `daoRemoteShmFileServer.py`, `daoRedisCheck.py`, `daoDAQCli.py` |
| **Real-time display (RTD)** | `daoImageRTD.py` / `daoImageRTDFloat.py`, `daoBarRTD.py`, `daoShRTD.py`, `daoWavefrontRTD.py`, `daoPlotRTD.py` |

# GUI

`gui/` (PyQt5 + pyqtgraph, source in `docs/source/gui.rst`) ships standalone
viewer/control applications, each reading its target straight from a `dao.shm`
stream — pass the SHM name(s) on the command line, no config file needed:

| Category | Tools |
|---|---|
| **Image / SHM viewing** | `daoShmViewer` (flagship multi-panel viewer, dark by default (`--light` for light mode), built-in DAQ session panel, and a "SHM Latency" tab that launches `daoTimeDiff` in its own tmux session and live-plots the AVG/RMS it publishes for any two SHMs picked from the file list), `daoImDisp` / `daoImgDisp` (lightweight single-image viewers), `daoRTDMagic` (OpenGL-textured, lowest-latency image RTD), `daoRemoteShmViewer` (mirrors a SHM stream from another host via `daoRemoteShmFileServer.py`) |
| **DM control & display** | `daoDmCtrl` (modal control of one DM channel through its M2A matrix — any modal basis, not analytic-only), `daoDmChannelsCtrl` (overview of all DM command channels at once, opens `daoDmCtrl` per channel), `daoDmDisp` / `daoDmDispNoMap` (read-only actuator display, mapped / raw bar chart) |
| **Loop control** | `daoLoopDisp` / `daoLoopCtrl` (generic open/close-loop and gain/leak control panels — point at any loop's state/gain/leak scalar SHMs, default `lpCmd`/`lpGain`/`lpLeak`) |
| **Wavefront & slopes display** | `daoWfDisp` / `daoWfDispMap` (reconstructed phase, plain or over an illumination map), `daoSlopesDisp` (X/Y slope quiver plot), `daoShDisp` (colour-coded slope grid), `daoRTDMagicSH` (low-latency Shack-Hartmann RTD), `daoTtDisp` (tip/tilt scatter monitor), `daoBarDisp` (generic real-time bar chart of any 1-D SHM vector) |
| **Latency measurement** | `daoTimeDiffDisp` (standalone SHM-to-SHM latency GUI — takes the two SHMs on the command line, Start/Stop launches `daoTimeDiff` in its own tmux session and live-plots its AVG/RMS window as a scatter + histogram; same measurement as `daoShmViewer`'s "SHM Latency" tab, without the rest of the viewer) |
| **Logging & telemetry** | `daoLogMonitor` (live, filterable ZMQ log viewer), `daoShmTelemetryConfigurator` (graphical daoDAQ YAML editor, embedded in `daoShmViewer`) |
| **Reusable widget** | `daoProcessWidget` (live process-status widget — tmux session alive/dead, CPU load, last update — embedded in other GUIs) |

# Prerequiries
## daoBase
daoBase should be installed. See https://github.com/Durham-Adaptive-Optics/daoBase

## Required dependencies (CLI11, cfitsio, yaml-cpp, fmt, zmq, protobuf, ncurses)
`waf configure` fails without these - every app in `apps/` links at least protobuf,
zmq and CLI11.

Ubuntu/Debian:
````
sudo apt install libcli11-dev libcfitsio-dev libyaml-cpp-dev libfmt-dev libzmq3-dev \
                  libprotobuf-dev protobuf-compiler libncurses-dev
````

RHEL/Fedora/CentOS:
````
sudo yum install cfitsio-devel yaml-cpp-devel fmt-devel zeromq-devel \
                  protobuf-devel protobuf-compiler ncurses-devel
````
(CLI11 has no RHEL package as of writing; `configure` falls back to looking for
a vendored `CLI11.hpp`/`CLI/CLI.hpp` header if pkg-config can't find it.)

## Optional dependencies
`waf configure` auto-detects each of these and **silently skips** the app(s)
that need it if missing - not having them is not an error, just a smaller build.

| dependency | apt package | enables | skipped without it |
|---|---|---|---|
| BLAS (any implementation) | `libopenblas-dev` | `daoMvM` (CPU real-time matrix-vector multiply) | `daoMvM` |
| CUDA toolkit (`nvcc` + `cudart`) | see NVIDIA's install docs | `daoMvMGPU` | `daoMvMGPU` |
| FFTW, single **and** double precision | `libfftw3-dev` | the FFT-based correlation centroider (`daoToolsCorrFFT`, `daoComputeCentroidCorrelationFFT`) | that centroider only - `daoComputeCentroidCorrelation` (the windowed-search version) is unaffected |

Check `waf configure`'s output for lines like `BLAS detected: enabling BLAS
build.` / `FFTW not found ... skipping FFT correlation centroider.` to see
what your machine actually got.

We recommand to use magicPlot
Some of our plot tool uses magicPlot (optional)
```
pip install magicPlot
```

# Real-time scheduling priority

Every real-time loop in `apps/` requests `SCHED_FIFO` scheduling once at
startup via `daoToolsSetRtPriority(priority)` (`daoTools.h`, e.g. priority
93), so the OS doesn't preempt it for ordinary processes.

That request can be silently denied: Linux caps how high a priority a given
user may request (the `rtprio` resource limit), and it defaults to **0** on
most distros. Before `daoToolsSetRtPriority` existed, every tool called
`sched_setscheduler()` directly with no check on its return value -- if the
request was denied (`EPERM`), the loop just kept running at normal priority
with no indication at all. That's dangerous for a control loop with little
stability margin: it becomes vulnerable to timing jitter from anything else
that uses the CPU, even a lightweight, read-only monitor GUI.

`daoToolsSetRtPriority` still tries for the requested priority first --
identical behaviour to before when it succeeds -- and only on failure checks
this user's actual `rtprio` limit and falls back to the highest priority
currently allowed. Either way it logs which priority it actually got
(`daoInfo` on success, `daoWarning` on fallback/failure), so the operator
always knows.

### Raising the limit (Linux)

Add a line to `/etc/security/limits.conf` (or a file under
`/etc/security/limits.d/`), then start a new login session (log out/in, or a
fresh login shell) for it to take effect:

```
<username>  -  rtprio  99
```

or for everyone in a group:

```
@rtgroup  -  rtprio  99
```

Confirm it applied with:

```
ulimit -r
```

### Does this affect Windows or macOS?

No new platform support is added or removed here -- it only changes how the
*existing*, Linux-oriented real-time code behaves:

- `sched_setscheduler()`/`SCHED_FIFO` are POSIX real-time scheduling calls;
  none of `apps/`'s real-time-priority code has ever been guarded for
  Windows, so real-time scheduling in this codebase has always been
  Linux-only in practice (same as `mlockall()` and the
  `pthread_setaffinity_np` CPU-pinning code used elsewhere in the tree).
- **macOS**: `RLIMIT_RTPRIO` (the fallback-limit lookup) doesn't exist on
  macOS/BSD, so `daoToolsSetRtPriority` guards that part behind
  `#ifdef RLIMIT_RTPRIO`. This means the code now *compiles* cleanly on
  macOS (a plain, unconditional `RLIMIT_RTPRIO` would have been a hard
  compile error there) and logs a clear warning if `sched_setscheduler`
  fails, instead of silently running non-real-time as before. Functionally
  unchanged from before this fix -- just no silent failure and no compile
  break.
- **Windows**: unaffected either way -- `sched_setscheduler` has no Windows
  equivalent and this codebase was never built/run there.

# Build
```
waf configure --prefix=$DAOROOT
waf
waf install
```

# Build Documentation
Run `waf build_docs` to build the Sphinx HTML documentation (outputs to `docs/build/html`).

```
waf build_docs
```

To clean the built documentation:

```
waf clean_docs
```

# Example
It is possible to create just the shared memory for an example. See examples below
## staring a simple clock
The first example is a simple C code increasing a counter in the shared memory.
This software clock can be used to synchronize different program using built-in semaphore in the DAO SHM
### create SHM
can be skipped if SHM already created
```
import daoShm
import numpy as np
clockShm=daoShm.shm('/tmp/demoClockShm.im.shm',np.zeros((1,1)).astype(np.uint32))
``` 
### run example
Now let's run the clock at 1.5kHz in a new console
```
daoClock -L demoClockShm 1500
```
## writing data in shared memory
This example is a C program writing in the shared memory at a specific rate.
The program is waiting for a new value in another shared memory. We can use the previous example and use the clock as the trigger
### create SHM
can be skipped if SHM already created
```
import daoShm
import numpy as np
shm=daoShm.shm('/tmp/demoShm.im.shm',np.zeros((100,100)).astype(np.float32))
clockShm=daoShm.shm('/tmp/demoClockShm.im.shm',np.zeros((1,1)).astype(np.uint32))
``` 
### run example
Now run in a new console the program writing at the clock rate in the share memory random values
```
daoRandWriterSync -L demoShm demoClockShm
```
