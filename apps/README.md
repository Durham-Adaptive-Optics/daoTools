# apps/ — input/output reference

Every process below reads/writes plain `dao.shm` shared memory (see daoBase);
most follow the same shape: `-S <shm list> -s <semNb> -L` starts a real-time
loop that blocks on the semaphore of the **first input** listed and republishes
its output(s) each time new data arrives. Run any tool with `-h` for the
authoritative, up-to-date flag list — this file is the input/output map, not
a substitute for `-h`.

Excluded here: `daoRandWriter.c` (source file present but has no build rule —
not actually built/shipped) and the project-scaffolding tools
(`daoNewPipeline.py`, `templates/`) which get their own documentation.

## Wavefront sensing & centroiding

| Tool | Inputs | Outputs | Notes |
|---|---|---|---|
| `daoComputeCentroid` | image, reference, threshold | centroids | Core Shack-Hartmann centre-of-gravity. `-S <in> <ref> <threshold> <centroid> <subaSize> <nbSuba>` |
| `daoComputeCentroidRelative` | image, reference | centroids | Differential COG (no threshold). `-S <in> <ref> <centroid> <subaSize> <nbSuba>` |
| `daoComputeCentroidRelativeRef` | image, sub-aperture centres, reference | centroids | Like Relative, with an explicit sub-aperture-centres SHM. `-S <in> <subApCentres> <reference> <centroid> <subaSize> <nbSuba>` |
| `daoComputeCentroidCorrelation` | image, sub-aperture centres, reference-spot image, threshold | centroids, (reference-spot image, if `-a` is used) | Cross-correlates each spot against a reference template (windowed shift search) instead of COG. Ref image: one `subaSize`×`subaSize` template per subaperture, stacked row-wise. `-a <alpha>` turns on a background EMA update of the reference from each frame's own re-aligned spot (off by default). `-S <in> <centroid> <subApCentres> <refImage> <threshold> <subaSize> <nbSuba> <searchRange>` |
| `daoComputeCentroidCorrelationFFT` | same as Correlation, minus `searchRange` | same as Correlation | Same idea, full periodic correlation via FFT (needs FFTW, see top-level README) — searches the whole box for one FFT round trip. float32/float64 auto-detected from the input image; all 5 SHMs must share that atype. `-S <in> <centroid> <subApCentres> <refImage> <threshold> <subaSize> <nbSuba>` |
| `daoComputeCentroidPws` | image, reference, threshold | centroids | Pyramid-WFS centroiding. `-S <in> <ref> <centroid> <threshold> <nbPix>` |
| `daoComputeIntensityPws` | image, valid-subaperture-pixel map | intensity, valid-pixel-count | Pyramid-WFS intensity measurement. `-S <in> <intensity> <validPix> <validSubaPix>` |
| `daoComputeCentroids.py` | image, threshold | centroids | Python reference COG implementation. `-i <imShm> -t <thresholdShm> -o <offset> -s <subaSize> -n <nbSuba> -c <centroidShm>` |
| `daoComputeCentroidsSlow.py` | image, LUT | centroids | Slow/debug Python centroider driven by a pixel look-up table. `-i <imShm> -l <lutShm> -c <centroidShm>` |
| `daoPrepCentroidLut.py` | image (for geometry) | LUT | Builds the sub-aperture LUT consumed by `daoComputeCentroidsSlow.py`. `-i <imShm> -n <nbSuba> -l <lutShm>` |
| `daoPrepPwfs.py` | — | background, flat-field, flux (fixed `/tmp/wsBg`, `/tmp/wsFf`, `/tmp/wsFlux`) | One-shot setup script: creates the pyramid-WFS reference SHMs with hardcoded 128×128 defaults. No CLI args. |

## Matrix-vector multiply

| Tool | Inputs | Outputs | Notes |
|---|---|---|---|
| `daoMvM` | vector, matrix | vector | `y = M x`, CPU (BLAS, single call per frame). `-S <input> <matrix> <output> -s <semNb> [-C <cpu>] [-N <blasThreads>]` |
| `daoMvMGPU` | vector, matrix | vector | Same, CUDA. GPU SHMs are used in place (no host copy; the loop runs on their GPU). `-S <input> <matrix> <output> -s <semNb> [-C <cpu>] [-G <gpu>]` |
| `daoMvM.py` | vector, matrix | vector | Python reference (NumPy or CuPy if available). `-m <matrix> -v <vector> -o <output> [-g]` |
| `daoReconstructor.py` | vector, matrix | vector | Minimal Python MVM (nice +10, debug/offline use). `-i <input> -r <recon> -o <output>` |

## GPU pipeline

| Tool | Inputs | Outputs | Notes |
|---|---|---|---|
| `daoGpuPipeline` | the configuration's trigger and input SHMs, parameter SHMs | every stage's output SHM, published each frame | A chain of GPU stages (GPU versions of the tools in this file: pixel calibration / extraction, centroiders, MVM, gain) in one process, one CUDA graph per frame. `-c <config.yaml> [-s <stage>] [-C <cpu>] -L`; `-s N` runs only stage N, triggered by its input. See `docs/source/gpu_pipeline.rst`. |

## Pixel calibration

| Tool | Inputs | Outputs | Notes |
|---|---|---|---|
| `daoPixelCalibrate` | raw image, flat-field, background | calibrated image | `cal = (raw - bg) * ff`, float or double (auto-selected from the output SHM's atype). `-S <in> <flatfield> <background> <output> -s <semNb> [-C <cpu>]` |
| `daoPixelCalibratePws` | raw image, background, flat-field, mask | calibrated image, flux | Pyramid-WFS variant, also emits total flux. `-S <in> <background> <flatfield> <mask> <cal> <flux>` |
| `daoCalIntensity` | raw image, flat-field, background, reference | reference (recomputed once), valid-pixel mask, illuminated-pixel mask, intensity | Calibrate + extract + normalize in one pass. `-S <raw> <ff> <bg> <ref> <validPix> <illumPix> <intensity>` |
| `daoCalIntensityNorm` | same as `daoCalIntensity` | same as `daoCalIntensity` | Same, but the reference is normalized against its own illuminated sum. |
| `daoTakeBg.py` | camera image | `<cam>Bg` (mean background), `<cam>BgPercent` (progress, 0-100) | Averages N frames from `<camShm>` into a new `<camBaseName>Bg.im.shm`. `<camShm> <nFrames>` |

## SHM utilities

| Tool | Inputs | Outputs | Notes |
|---|---|---|---|
| `daoShmMonitoring` | one or more SHMs | terminal only | Prints live per-frame stats; `-t <nloops>` times raw SHM I/O instead. |
| `daoShmMonitoring1Value` | one or more SHMs | terminal only | Same, for single-scalar SHMs. |
| `daoShm2Fits` | SHM | FITS file | `-L <shm> <outputPath>` |
| `daoFits2Shm.py` | FITS file | SHM (created if absent) | Streams a FITS file into a SHM at a fixed rate. `-f <fits> -s <shm> -p <pause>` |
| `daoNpy2Shm.py` | `.npy` file | SHM (created if absent) | Same, from a NumPy file. `-n <npy> -s <shm> -p <pause>` |
| `daoTakeDataCubeFITS.py` | SHM | FITS cube, `<base>Percent` (progress) | Records an ROI from N frames to `$DAODATA/data/<base><timestamp>Cube<description>.fits`. Usage: `<shm> <nFrames> <description> <width> <cx> <cy>`. The FITS header includes `FPS`, `DIT`, `Nframes`, `ROI`, `CX`, and `CY`; `FPS` and `DIT` are read from `<base>Fps.im.shm` and `<base>Dit.im.shm`. |
| `daoTakeDataCubeNPY.py` | SHM | `.npy` cube, `<base>Percent` (progress) | Records an ROI from N frames to `$DAODATA/data/<base>Cube<description>.npy`. Usage: `<shm> <nFrames> <description> <width> <cx> <cy>`. |
| `daoSnapshot.py` | SHM | FITS or `.npy` file | Single-frame grab. |
| `daoShmRate.py` | SHM | terminal only | Prints the live update rate. `<SHM> [--interval <s>]` |
| `daoShmSlice` | SHM | SHM | Each time the input is published, copies its values `[offset, offset + n)` into the output (same data type; the input may be a GPU SHM) and publishes it, with the input's `cnt2`. `-S <in> <out> [-o <offset>] [-n <n>] -s <semNb>` (`n` defaults to the output's size) |
| `daoStrCmd.py` / `daoReadStr.py` / `daoWriteStr.py` | `<name>SCmd` (write) / `<name>SRsp` (read) | matching command/reply SHM | Generic string-over-SHM command helpers: pack a null-terminated string into a `uint8` SHM (`StrCmd`/`WriteStr`) or unpack one back to text (`ReadStr`). |

## SHM arithmetic

| Tool | Inputs | Outputs | Notes |
|---|---|---|---|
| `daoShmAdd` | `in1`, `in2` | `out` | Element-wise `out = in1 + in2`. `-S <out> <in1> <in2>`; `-m <1\|2>` finalizes only on that input's update instead of on either. |
| `daoShmCombiner` | `<name>00 .. <name>(N-1)` | `<name>` | Sums N channels into the base-named SHM. `-m <masterChannel#> -S <name> <nbShm>` |
| `daoDmCombiner` | `<name>00 .. <name>(N-1)` | `<name>` | Same, DM-command specific: optional piston removal and clipping. `-m <masterChannel#> -p -c <clip> -S <name> <nbShm>` |
| `daoShmConcatenate` | `in1`, `in2` | `out` | Concatenates two arrays along the first axis into `out`. `-S <out> <in1> <in2>`; same `-m` semantics as `daoShmAdd`. |
| `daoShmConcatenateFine` | `SHM1 .. SHMn` | `out` | N-way concatenation with an explicit count. `-n <nbShm> -O <out> -S <SHM1>..<SHMn>` |
| `daoAvgShm` / `daoAvgDoubleShm` | SHM | terminal only | Running average/RMS telemetry (float / double), printed, not written back. `-S <SHM> -n <nbAverage>` |
| `daoStatShm` / `daoStatDoubleShm` | SHM | terminal only | Same telemetry family. `-S <SHM> -n <nbFrame>` |
| `daoDownsample` | source image (`uint16`) | output image (`uint16`, smaller) | `sourceImageSHM outputImageSHM [-s/--sum]` (sum instead of average). CLI11-based. |
| `daoPixelExtract` | image, mask | extracted image | `-S <in> <mask> <extract>` |
| `daoPixelSubstractExtract` | `inA`, `inB`, mask | extracted image, (norm, with `-N`) | `extract = (inA - inB)` restricted to `mask`; `-N` also emits a normalized output. `-S <inA> <inB> <mask> <extract> <norm>` |
| `daoPixelSubstractExtractNorm` | `inA`, `inB`, mask | extracted image, norm | Same family, normalization always on. |
| `daoPixelSubstractExtractNormImage` | `inA`, `inB` (norm image), mask | extracted image, norm | Same family; `inB` is itself an image used for normalization rather than a plain subtrahend. |
| `daoApplyGain` | image, gain | output image | `out = in * gain` (`-m`: modal — gain is a per-mode array). `-S <in> <gain> <out>` |
| `daoDescrambleOcam2` | OCAM2K raw frame, pixel LUT | descrambled image | Reorders OCAM2K's scrambled readout. `-S <ocamRaw> <ocamShm> <lut> -b <binning>` |

## Loop & filter

| Tool | Inputs | Outputs | Notes |
|---|---|---|---|
| `daoLeakyIntegrator` | input, offset, loop-state, leak, gain (all control SHMs) | output | `out(t) = leak*out(t-1) + gain*(in - offset)` when the loop-state SHM is non-zero. `-m`: modal (leak/gain are per-mode arrays). `-S <in> <offset> <out> <loopCmd> <leak> <gain>` |
| `daoLeakyIntegratorMap` | same + an actuator map | output | Same, with a map SHM restricting which actuators are integrated. `-S <in> <offset> <out> <loopCmd> <leak> <gain> <map>` |
| `daoClock` | — | clock counter | Increments a SHM counter at a target frequency; other processes trigger off its semaphore. `-S <clock> <freq> <frequency>` |
| `daoHighPassFilter` | input, cutoff freq, sample rate, enable | output | Per-pixel temporal high-pass. `-S <in> <out> <fc> <fps> <ena>` |
| `daoModesCutoff` / `daoModesCutoffFull` | modes | modes (cutoff applied) | Zeroes modes above a cutoff index. `-S <modes> <modesCutoff>` |
| `daoCommandFilter` | command, servo state, offset | filtered command | Passes the DM command SHM through a configurable transfer function. `-L <in> <servo> <offset> <out>` |

## Timing & latency

| Tool | Inputs | Outputs | Notes |
|---|---|---|---|
| `daoTimeDiff` | `SHM1`, `SHM2` (each with its own semaphore) | measurement | Latency between two SHM timestamps. `-S <SHM1> <SHM2> <sem1> <sem2> <measurement>` |
| `daoTimeDiffNCurse` | same | measurement + live `ncurses` display | Same tool, terminal UI. |
| `daoTimeDiffStat` | a latency/time SHM | running-average SHM | Statistical summary (min/max/mean/std) over N samples. `-S <time> -s <semNb> -n <nbMeas>` |
| `daoSetLatency` | — | — | Not a SHM tool: writes a value to `/dev/cpu_dma_latency` (Linux PM QoS) to disable deep CPU idle states, then blocks forever holding it open. `<latency in us>` |
| `daoPlotLatency.py` | a latency SHM | matplotlib window | Reads N samples and plots a histogram + trace. `<nData> <latencyShm>` |

## Data conversion & I/O

| Tool | Inputs | Outputs | Notes |
|---|---|---|---|
| `daoRandImageU16Write` | clock | random `uint16` image | Test data generator, ticks on an external clock SHM. `-S <out> <clock> -s <semNb>` |
| `daoRandWriterSync` | clock | random data | Same idea, simpler CLI. `-L <shm> <clockShm>` |
| `daoDMSend` | SHM | UDP packet | One-shot: reads a DM command SHM and sends it over the network. `<SHM> <IP> <PORT>` |
| `daoReadWrite` | input | output | Minimal passthrough copy, mostly a template/example. `-L <in> <out>` |

## Simulation

| Tool | Inputs | Outputs | Notes |
|---|---|---|---|
| `daoTurbulenceSimulator.py` | — | phase-screen SHM | Kolmogorov turbulence, streamed continuously. `-f <fileShm> -d <diameter> -r <r0> -s <speed> -o <orientation> -t <dt> -p <period>` |
| `daoPwfsSimulator.py` | DM command | simulated pyramid-WFS image | Hardcoded DM map; simulates the PWFS signal produced by a given DM shape. |
| `daoNoisyPsfGenerator.py` | — | `/tmp/psf.im.shm` | Fixed 512×512 Gaussian PSF, randomly jittered each frame. No CLI args. |

## Real-time display (RTD)

Standalone matplotlib scripts (as opposed to the PyQt5 apps in `gui/`); all
read-only, no output SHM.

| Tool | Inputs | Notes |
|---|---|---|
| `daoImageRTD.py` / `daoImageRTDFloat.py` | image | False-colour terminal-window display. `-s <shmimName> [-a]` (`-a`: flip axis) |
| `daoBarRTD.py` | 1-D vector | Bar-chart display. `-s <shmimName>` |
| `daoShRTD.py` | image, centroids, references, pupil mask | Shack-Hartmann overlay: spots + measured/reference centroids on the pupil mask. `-i <image> -c <centroids> -r <references> -p <pupilMask>` |
| `daoWavefrontRTD.py` | wavefront/phase map | Reconstructed-wavefront display. `-s <shmimName> [-a]` |
| `daoPlotRTD.py` | scalar SHM stream | General-purpose live line plot. `-s <shmimName>` |

## Logging & communication

| Tool | Inputs | Outputs | Notes |
|---|---|---|---|
| `daoRecvLogs.py` | ZMQ log stream | terminal | Subscribes to the log broker and displays messages. |
| `daoSendLogs.py` | — | ZMQ log stream | Publishes *randomly generated* test log messages (not file-forwarded, despite the name) — for exercising `daoRecvLogs`/`daoLogMonitor`. `-i <ip> -p <port>` |
| `daoLogToScreen.py` | ZMQ log stream | terminal + file | Pretty-prints live logs and tees them to a file. `-f <logFile> -i <ip> -p <port>` |
| `daoProxyLog.py` | ZMQ (sub 5558) | ZMQ (pub 5559) | Log broker: relays subscriber messages to publisher port. No SHM I/O. |
| `daoSendCommand.py` | — | `daoCommand` protobuf message over the network | CLI to EXEC/STATE/PING/QUERY/etc. any `daoComponent`-based process. `-i <ip> -p <port> -c <command> [-a <args>]` |
| `daoRemoteShmFileServer.py` | local SHM directory | ZMQ file/data server | Exposes local SHMs to a remote `daoRemoteShmFileClient`. `--port <port>` |
| `daoRedisCheck.py` | an input SHM (optional) | Redis key | Health-checks connectivity to the Redis telemetry database. |
| `daoDAQCli.py` | — | daoDAQ session control (network) | CLI to a daoDAQ server: ping/upload-config/start/stop a recording session. No direct SHM I/O. |
