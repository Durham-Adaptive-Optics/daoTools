GPU Pipeline
============

``daoGpuPipeline`` runs a chain of real-time processing steps on one GPU, in one
process: each camera frame goes through all the steps as a single CUDA graph,
and the intermediate data stays on the GPU. Each step (a *stage*) is the GPU
version of a daoTools application and gives the same result; the SHMs it reads
and writes are the same, so the tools around it (displays, telemetry, other
processes) keep working unchanged.

Built only when CUDA is found (``daoGpuPipeline`` also needs yaml-cpp); see
:ref:`gpu-pipeline-build`.

Source: ``apps/daoGpuPipeline.cpp``, ``include/daoGpuStages.h``,
``src/gpu/daoGpuStages.cu`` (``libdaoToolsGpu``).


How it works
------------

- The pipeline waits for the **trigger** SHM (typically the camera frame), then
  runs every stage, in order, as one CUDA graph: one launch per frame, no
  process hand-over between stages.
- Every **output SHM is published each frame** (``cnt0``, timestamp,
  semaphores), in stage order, so any process can still wait on an
  intermediate result. They are all published at the end of the frame, and by
  default timestamped then (see :ref:`gpu-pipeline-timestamps`).
- **Intermediate SHMs** are best created as GPU SHMs (daoBase
  ``daoShmCreateGpu``, or ``dao.shm(..., gpu=N, mirror=True)`` in Python): the
  next stage reads them in place on the GPU. With ``mirror``, a host copy is
  kept up to date for the CPU tools; it is written in parallel with the
  following stages and costs nothing on the critical path.
- **Host SHMs** (camera frame, final output) are read and written in place by
  the GPU (zero copy): kernels fetch only the pixels they use. This needs the
  SHM to be in RAM, i.e. on a ``tmpfs`` filesystem (``/dev/shm``, or ``/tmp``
  when it is mounted as tmpfs); otherwise the pipeline copies the SHM each
  frame and says so at start-up. See the daoBase GPU SHM documentation.
- **Parameter SHMs** (flat, background, matrices, gains, references,
  thresholds, masks) stay host SHMs: a stage uploads them again between two
  frames when their ``cnt0`` changes, so they can be updated live.


Configuration
-------------

A YAML file lists the device, the trigger and the stages, in order:

.. code-block:: yaml

    device: 0                               # CUDA device (GPU SHMs must be on it)
    trigger: {shm: /tmp/wfsIm.im.shm}    # waits on a semaphore of its own (sem: N forces one;
                                            # wait: spin polls its cnt0 instead, see Running)
    hostAccess: map                         # host SHMs: map (zero copy, default) or copy
    stageTimestamps: false                  # true: each output timestamped when its data was ready
    stages:
      - calIntensityNorm: {in: /tmp/wfsIm.im.shm, ff: /tmp/wfsFf.im.shm, bg: /tmp/wfsBg.im.shm,
                           ref: /tmp/wfsRef.im.shm, validPix: /tmp/wfsPup.im.shm,
                           illumPix: /tmp/wfsPup.im.shm, out: /tmp/wfsSignal.im.shm}
      - mvm:       {in: /tmp/wfsSignal.im.shm, matrix: /tmp/i2m.im.shm, out: /tmp/modes.im.shm}
      - applyGain: {in: /tmp/modes.im.shm, gain: /tmp/modesGain.im.shm, out: /tmp/modesGained.im.shm}
      - mvm:       {in: /tmp/modesGained.im.shm, matrix: /tmp/m2a.im.shm, out: /tmp/dmResWf.im.shm}

The whole list of stages and their keys is in the header of
``apps/daoGpuPipeline.cpp``.


Stages
------

Each stage reproduces the daoTools application of the same name, on float data
(the real-time case). ``in`` and ``out`` are data SHMs; the other SHM keys are
parameters.

.. list-table::
   :widths: 30 25 45
   :header-rows: 1

   * - Stage
     - Replaces
     - Keys
   * - ``pixelCalibrate``
     - ``daoPixelCalibrate``
     - ``in``, ``ff``, ``bg``, ``out``
   * - ``calIntensityNorm`` / ``calIntensity``
     - ``daoCalIntensityNorm`` / ``daoCalIntensity``
     - ``in``, ``ff``, ``bg``, ``ref``, ``validPix``, ``illumPix``, ``out``
   * - ``pixelExtract``
     - ``daoPixelExtract``
     - ``in``, ``mask``, ``out``
   * - ``pixelSubstractExtract``
     - ``daoPixelSubstractExtract``
     - ``in``, ``sub``, ``mask``, ``out``, optional ``norm``
   * - ``pixelSubstractExtractNorm`` / ``...NormImage``
     - ``daoPixelSubstractExtractNorm`` / ``...NormImage``
     - ``in``, ``sub``, ``mask``, ``out``
   * - ``descrambleOcam2``
     - ``daoDescrambleOcam2``
     - ``in``, ``lut``, ``out``, ``binning`` (1 or 2)
   * - ``centroid``
     - ``daoComputeCentroid``
     - ``in``, ``ref``, ``threshold``, ``out``, ``subaSize``, ``nbSuba``, optional ``minFlux``
   * - ``centroidRelative``
     - ``daoComputeCentroidRelative``
     - ``in``, ``ref``, ``threshold``, ``out``, ``subaSize``, ``nbSuba``, optional ``minFlux``
   * - ``centroidRelativeRef``
     - ``daoComputeCentroidRelativeRef``
     - ``in``, ``subApCentre``, ``ref``, ``threshold``, ``out``, ``subaSize``, ``nbSuba``,
       optional ``minFlux``
   * - ``centroidCorrelation``
     - ``daoComputeCentroidCorrelation``
     - ``in``, ``subApCentre``, ``refImage``, ``threshold``, ``out``, ``subaSize``,
       ``nbSuba``, ``searchRange``, optional ``alpha``, ``minFlux``
   * - ``centroidCorrelationFFT``
     - ``daoComputeCentroidCorrelationFFT``
     - ``in``, ``subApCentre``, ``refImage``, ``threshold``, ``out``, ``subaSize``,
       ``nbSuba``, optional ``alpha``, ``minFlux``
   * - ``mvm``
     - ``daoMvM`` / ``daoMvMGPU``
     - ``in``, ``matrix``, ``out`` (the input may be longer than the matrix: its
       first values are used)
   * - ``applyGain``
     - ``daoApplyGain``
     - ``in``, ``gain``, ``out``, optional ``modal``
   * - ``centroidWindows``
     - ``daoComputeCentroidWindows``
     - ``in``, ``table``, ``threshold``, ``out``, ``refImage`` (not for ``cog``), optional
       ``minFlux``, ``method`` (``correlation`` | ``correlationNormalized`` | ``cog``),
       ``peak`` (``max`` | ``parabola`` | ``barycenter`` | ``barycenterThreshold`` |
       ``barycenterThresholdWeighted``)
   * - ``leakyIntegrator``
     - ``daoLeakyIntegrator``
     - ``in``, ``out`` (the state), ``loop``, ``gain``, ``leak``, optional ``enable``,
       ``offset``, ``modal`` (``-m``), ``keepPiston`` (``-P``), ``clip`` (``-c``, default 10)

With ``alpha > 0``, the correlation centroiders update their reference image
after each frame is published (off the critical path), as the ``-a`` option of
the applications does, and write it back to its SHM. ``centroidCorrelationFFT``
computes the periodic correlation directly on the GPU (same result as the FFT,
up to rounding), so it does not need FFTW.

Sub-apertures without light, in every centroider: no light (nothing above the
threshold) gives slopes (0, 0). ``minFlux`` (optional, a 1-value SHM, reloaded
when it changes, as the ``-f`` option of the applications) also gives (0, 0) to
a sub-aperture whose flux, the sum of its raw pixels, is below it, and keeps its
reference image when ``alpha > 0``. The third output row is that flux.

``leakyIntegrator`` is the application's control law: ``out = leak * out - gain *
(in - offset)`` while ``loop`` is 1 (the mean then removed, unless ``keepPiston``,
and clipped), zero while the loop is open. Its output is its state: keep it a GPU
SHM (mirrored if CPU tools read it). With ``enable`` at 0 it publishes zero once,
then is silent. The loop and enable SHMs are read each frame without recapturing
the graph.

A stage can be silent for a frame (not published, e.g. a disabled integrator). A
stage whose input comes from a silent stage is silent too and is not run: an M2A
after a disabled integrator leaves its output SHM to whatever else writes it.


Running
-------

.. code-block:: bash

    daoGpuPipeline -c <config.yaml> [-s <stage>] [-C <cpu>] [-T] [-W] [-d <level>] -L

- **One process** (default): the whole chain, one CUDA graph per frame. It needs
  no NVIDIA MPS, unless other processes compute on the same GPU.
- **One process per stage** (``-s N``, 0 = first stage): runs only stage *N*,
  triggered by its own input SHM. Starting one process per stage with the same
  configuration gives the chain as separate processes on the GPU SHMs, handy to
  develop and tune stage by stage before running it as one process. Several
  processes on one GPU need NVIDIA MPS (daoBase's ``daoGpuMps start``), or each
  hand-over costs about 100 µs.
- ``-C <cpu>`` pins the loop to a CPU core.
- ``-T`` turns on the stage timestamps (as ``stageTimestamps: true``), see
  below.
- ``-W`` waits for the trigger by **spinning** on its ``cnt0`` instead of a
  semaphore (as ``trigger: {shm: ..., wait: spin}``). The writer increments
  ``cnt0`` right after its timestamp, so the pipeline sees the frame a fraction
  of a µs later, instead of being woken up by the kernel after the semaphore is
  posted (about 5 µs on the Ekarus RTC, the *wake* of the status line). The
  cost: the CPU core is busy all the time, so pin the loop (``-C`` or
  ``taskset``) on an isolated core of its own. If the pipeline falls behind, it
  takes the latest frame (with a semaphore it would run the ones it missed), and
  it does not take one of the trigger's semaphores. The pipeline runs with a
  real-time priority, and Linux by default lets real-time tasks use only 95% of
  each second (``kernel.sched_rt_runtime_us = 950000``): a loop that never sleeps
  is then stopped for 50 ms every second and misses about 5% of the frames.
  Turn this limit off before using ``-W``
  (``sudo sysctl -w kernel.sched_rt_runtime_us=-1``, kept across reboots in
  ``/etc/sysctl.d/``), but only once nothing else can run on the pipeline's
  core: with the limit off, any other thread there (a server, a GUI, an
  interrupt thread of lower priority) never runs again. ``isolcpus`` alone does
  not ensure it: a process allowed on every core can land on an isolated core
  and stay there. Keep the other processes on the housekeeping cores (e.g.
  ``CPUAffinity=`` in systemd's ``system.conf`` and ``user.conf``) and the
  interrupts off that core first; otherwise do not use ``-W``.

Once per second the pipeline prints its average and worst latency per frame,
split into *wake* (trigger timestamp to the pipeline waking up), *params*
(parameter reloads), *gpu* (the graph) and *publish*.
``DAO_GPU_PIPELINE_PROFILE=1`` runs without the graph and adds the GPU time of
each step.


.. _gpu-pipeline-timestamps:

Timestamps
----------

The whole chain runs as one CUDA graph, and the host only knows that the frame is
finished when the graph is: every output SHM is published after the whole frame,
the first stage's output as well as the last one's.

- **Default: timestamped when published.** Every output gets the time it is
  published, i.e. the end of the frame, about a µs apart in stage order. The last
  output (e.g. the DM command) is timestamped exactly as by a separate process,
  and nothing is added to the frame: this is the mode for running the loop.
  The intermediate outputs, though, look as late as the last one: a latency
  measured on them (``daoTimeDiff`` between the camera frame and the first stage's
  output, for instance) is the latency of the whole chain, not of that stage.
- **Stage timestamps** (``-T``, or ``stageTimestamps: true`` in the configuration):
  each output is timestamped with the time its own data was ready on the GPU,
  after its stage (and its copy to the host, if any). Intermediate latencies then
  mean what they say: camera frame to descrambled frame is the descrambler's
  time, not the whole chain's. Use it to measure the latency of each stage; the
  start-up message ends with ``stage timestamps``.

How the stage timestamps are made: after each stage, a one-thread kernel writes
the GPU clock (``%globaltimer``, ns) to mapped host memory, on a branch of the
graph of its own so that it does not delay the next stage, and one more at the
end of the frame. Each output is then timestamped ``tEnd - (end - done)``, with
``tEnd`` the host clock (``CLOCK_REALTIME``) when the frame is seen finished: only
differences of the GPU clock within the frame are used, so it does not need to
be synchronised with the host clock. The timestamps are late by the host's
wake-up at the end of the frame, about 1 µs. The last output takes the end-of-
frame time.

They are not free: the extra kernels and the branch cost about 1.5 µs per stage
on the GPU, about 10 µs per frame on a 7-stage pyramid chain (A40, MPS on, 2 kHz),
measured end to end from the camera frame to the DM command. Turn them on to
measure, off to run.


.. _gpu-pipeline-build:

Build and tests
---------------

``waf configure`` enables the GPU build when it finds ``nvcc``; the CUDA code is
compiled for the GPUs of the build machine (``-arch=native``), or for every
current architecture if no GPU is visible then. It builds ``libdaoToolsGpu``,
``daoGpuPipeline`` (with yaml-cpp) and ``build/tests/testGpuStages`` (not
installed).

``testGpuStages [device]`` runs each stage and the libdaoTools function it
replaces on the same SHMs and compares their outputs; SHMs are created in
``$DAO_GPU_TEST_DIR`` (default ``/dev/shm/daoGpuStagesTest``, a folder that is not
tmpfs tests the copy mode). ``tests/bench_pyramid_pipeline.py`` and
``tests/bench_sh_pipeline.py`` time a pyramid WFS chain and a Shack-Hartmann
chain as separate processes and as ``daoGpuPipeline``; see ``tests/README.md``.


Writing a stage
---------------

A stage implements ``update`` (reload changed parameters, outside the graph),
``run`` (enqueue its kernels on a stream; they are captured in the graph) and,
optionally, ``post`` (work after the frame is published). Its data SHMs are
*ports* (``daoGpuPortInit``): a device pointer on a GPU SHM, a mapped host SHM
or a device buffer. See ``include/daoGpuStages.h`` and the existing stages in
``src/gpu/daoGpuStages.cu``; new stage types are added to the list in
``apps/daoGpuPipeline.cpp``.
