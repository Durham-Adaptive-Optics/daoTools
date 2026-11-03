CPU Pipeline
============

``daoCpuPipeline`` runs a chain of real-time processing steps on the CPU, in one
process and one thread: when the camera frame arrives, the thread runs every
step, one after the other. Each step (a *stage*) calls the same libdaoTools
function as the daoTools application it replaces, on the same SHMs, and gives
the same result, bit for bit. The tools around it (displays, telemetry, other
processes) keep working unchanged.

It reads the :doc:`gpu_pipeline` configuration: the same file runs on either.

Source: ``apps/daoCpuPipeline.cpp``. Built when BLAS and yaml-cpp are found.


Why one process
---------------

In a chain of separate processes, each process waits on a semaphore of the
previous one's output: every step pays a wake-up, a few µs when its core is
busy-waiting, tens of µs when the core was idle, more when another process runs
there. In one thread only the first stage waits; the others start as soon as the
previous one returns.

As ``daoGpuPipeline`` captures one frame's work as a CUDA graph once, the stages
are set up once: the SHMs opened and their sizes checked, the pointers kept,
each stage's work reduced to one call. A frame runs those calls and nothing
else.

- Every **output SHM is published** (``cnt0``, timestamp, semaphores) as soon
  as its stage is done, as the separate processes do: a process waiting on an
  intermediate result sees no difference.
- **Parameter SHMs** (flat, background, references, matrix, gains) are read in
  place every frame, as the applications do: a new matrix is used from the next
  frame, with no copy and no pause of the loop.
- A **silent stage** (a disabled integrator: zeros once, then nothing) does not
  publish, and the stages that read its output do not run, as in
  ``daoGpuPipeline``.


Configuration
-------------

The :doc:`gpu_pipeline` configuration, with host SHMs only:

.. code-block:: yaml

    trigger: {shm: /tmp/shIm.im.shm}          # sem: N, wait: spin (as -W)
    blasThreads: 1                            # mvm (as -N); default 1
    stages:
      - pixelCalibrate:   {in: /tmp/shIm.im.shm, ff: /tmp/shFf.im.shm, bg: /tmp/shBg.im.shm,
                           out: /tmp/shImCal.im.shm}
      - centroidRelative: {in: /tmp/shImCal.im.shm, ref: /tmp/shRef.im.shm,
                           threshold: /tmp/shThreshold.im.shm, out: /tmp/shCentroids.im.shm,
                           subaSize: 20, nbSuba: 52}
      - mvm:              {in: /tmp/shCentroids.im.shm, matrix: /tmp/shRecon.im.shm,
                           out: /tmp/dmResWf.im.shm}

Stages: ``pixelCalibrate``, ``pixelExtract``, ``pixelSubstractExtract``,
``centroid``, ``centroidRelative``, ``centroidRelativeRef``,
``centroidCorrelation``, ``centroidCorrelationFFT`` (when libdaoTools has FFTW),
``centroidWindows``, ``mvm``, ``applyGain``, ``leakyIntegrator``, ``slice``,
with the keys of :doc:`gpu_pipeline`. Float data, except ``pixelCalibrate``,
``pixelExtract``, ``pixelSubstractExtract`` and ``slice``, which take what their
applications take. Not yet: ``calIntensity``, ``calIntensityNorm``,
``pixelSubstractExtractNorm``, ``pixelSubstractExtractNormImage``,
``descrambleOcam2``; run their applications next to the pipeline.

``device``, ``hostAccess`` and ``stageTimestamps`` are ignored; ``plugins``
(GPU stages) are refused, and so are GPU SHMs.


Running
-------

.. code-block:: bash

    daoCpuPipeline -c loop.yaml -L                  # semaphore wait
    daoCpuPipeline -c loop.yaml -C 4 -W -L          # pinned, spinning on the trigger's cnt0
    daoCpuPipeline -c loop.yaml -p -L               # also the time of each stage, every second

- ``-C <cpu>`` pins the loop thread; ``-W`` (or ``trigger: {wait: spin}``)
  waits by polling the trigger's ``cnt0``: a core busy all the time (see
  :doc:`realtime`).
- ``-N <n>`` (or ``blasThreads:``): BLAS threads for ``mvm``, 1 by default (0:
  the library's default). More threads only help large matrices, and they are
  real time too: OpenBLAS's threads spin between calls, and on a core another
  real-time process needs they hold it off until the kernel's real-time
  throttling stops them, tens of ms later. Give them cores of their own.
- The status line, every second: average and worst time from the wake-up to the
  last stage done, the rate, and the wake-up delay (trigger timestamp to the
  thread running).


.. _cpu-pipeline-workers:

Workers
-------

A stage with much to compute can be split over several cores:

.. code-block:: bash

    daoCpuPipeline -c loop.yaml -C 2 -W -w 4,6,8,10 -L    # also workers: [4, 6, 8, 10]

- Each worker is a thread pinned on its CPU, real time as the loop, that **spins
  between frames**, so it starts at once when handed its share: each needs a
  core of its own (see :ref:`rt-core-layout`).
- The centroiders and ``centroidWindows`` are split by sub-aperture (window),
  ``mvm`` by matrix row: the loop thread computes the first share, each worker
  another, and the stage is published once all are done. Each share calls the
  same library function on its own sub-apertures: **the same results, bit for
  bit**.
- The other stages run on the loop thread, and so does a stage given
  ``parallel: false``.
- Splitting costs a few µs per stage (handing out the shares, the frame read by
  other cores): worth it for a stage taking tens of µs or more, not for a light
  chain. Mark the light stages ``parallel: false``.
- Spinning cores can slow the others: on the desktop below, four workers
  spinning on other performance cores slowed the loop's core (its turbo clock
  drops with the number of busy cores) by about 5 µs on a 21 µs chain; on the
  efficiency cores, not at all. Measure where to put them.


Measured
--------

``tests/bench_sh_pipeline.py``: a Shack-Hartmann chain (160x160 frame, 52
sub-apertures, 97x104 matrix) as three processes (``daoPixelCalibrate``, a
centroider, ``daoMvM -N 1``), as ``daoCpuPipeline`` (the same output bits) and as
``daoGpuPipeline``; 2000 frames, from the frame written to the command
published, µs, median (99 %), on a desktop (i5-14600KF, RTX 5060 Ti):

.. list-table::
   :header-rows: 1

   * - Centroider
     - 3 processes
     - daoCpuPipeline
     - daoCpuPipeline -W
     - daoGpuPipeline -W
   * - relative
     - 47.1 (61.7)
     - 34.7 (36.0)
     - 21.3 (25.2)
     - 31.8 (56.2)
   * - correlation, search 5
     - 405.7 (442.6)
     - 388.1 (533.9)
     - 214.8 (249.7)
     - 39.8 (43.0)
   * - correlation FFT
     - 125.8 (141.6)
     - 102.3 (104.9)
     - 60.1 (64.1)
     - 64.4 (76.9)

- For a small chain the CPU pipeline is the fastest: no GPU launch and no PCIe
  transfer. The GPU wins once a stage has much to compute (the correlation).
- A spinning thread is also faster at computing: its core stays at full clock,
  where a core that sleeps between frames (``powersave`` governor) slows down.
- With ``daoMvM`` (and ``daoCpuPipeline -N 0``) at OpenBLAS's default thread
  count, up to 1 % of the frames waited about 77 ms (the throttling above).

With workers (``--spin --cpu 2 --workers ...``: the loop on CPU 2, the workers on
other performance cores, one per physical core), median (99 %), µs, the same
output bits each time:

.. list-table::
   :header-rows: 1

   * - Centroider
     - no worker
     - 1 worker
     - 3 workers
     - 4 workers
   * - relative
     - 21.4 (25.6)
     - 22.0 (23.4)
     - 29.1 (31.0)
     - 29.2 (30.7)
   * - correlation, search 5
     - 211.6 (217.1)
     - 140.6 (147.7)
     - 78.3 (84.4)
     - 71.0 (75.7)
   * - correlation FFT
     - 65.9 (87.8)
     - 46.4 (57.2)
     - 37.7 (46.2)
     - 37.4 (39.1)

The correlation runs three times faster with four workers; the FFT correlation is
then faster than on the GPU. The relative chain is too light: keep it on the loop
thread.
