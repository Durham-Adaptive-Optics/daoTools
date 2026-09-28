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
  intermediate result.
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
    trigger: {shm: /tmp/wfsIm.im.shm, sem: 1}
    hostAccess: map                         # host SHMs: map (zero copy, default) or copy
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
     - ``in``, ``ref``, ``threshold``, ``out``, ``subaSize``, ``nbSuba``
   * - ``centroidRelative``
     - ``daoComputeCentroidRelative``
     - ``in``, ``ref``, ``threshold``, ``out``, ``subaSize``, ``nbSuba``
   * - ``centroidRelativeRef``
     - ``daoComputeCentroidRelativeRef``
     - ``in``, ``subApCentre``, ``ref``, ``threshold``, ``out``, ``subaSize``, ``nbSuba``
   * - ``centroidCorrelation``
     - ``daoComputeCentroidCorrelation``
     - ``in``, ``subApCentre``, ``refImage``, ``threshold``, ``out``, ``subaSize``,
       ``nbSuba``, ``searchRange``, optional ``alpha``
   * - ``centroidCorrelationFFT``
     - ``daoComputeCentroidCorrelationFFT``
     - ``in``, ``subApCentre``, ``refImage``, ``threshold``, ``out``, ``subaSize``,
       ``nbSuba``, optional ``alpha``
   * - ``mvm``
     - ``daoMvM`` / ``daoMvMGPU``
     - ``in``, ``matrix``, ``out`` (the input may be longer than the matrix: its
       first values are used)
   * - ``applyGain``
     - ``daoApplyGain``
     - ``in``, ``gain``, ``out``, optional ``modal``

With ``alpha > 0``, the correlation centroiders update their reference image
after each frame is published (off the critical path), as the ``-a`` option of
the applications does, and write it back to its SHM. ``centroidCorrelationFFT``
computes the periodic correlation directly on the GPU (same result as the FFT,
up to rounding), so it does not need FFTW.


Running
-------

.. code-block:: bash

    daoGpuPipeline -c <config.yaml> [-s <stage>] [-C <cpu>] [-d <level>] -L

- **One process** (default): the whole chain, one CUDA graph per frame. It needs
  no NVIDIA MPS, unless other processes compute on the same GPU.
- **One process per stage** (``-s N``, 0 = first stage): runs only stage *N*,
  triggered by its own input SHM. Starting one process per stage with the same
  configuration gives the chain as separate processes on the GPU SHMs, handy to
  develop and tune stage by stage before running it as one process. Several
  processes on one GPU need NVIDIA MPS (daoBase's ``daoGpuMps start``), or each
  hand-over costs about 100 µs.
- ``-C <cpu>`` pins the loop to a CPU core.

Once per second the pipeline prints its average and worst latency per frame,
split into *wake* (trigger timestamp to the pipeline waking up), *params*
(parameter reloads), *gpu* (the graph) and *publish*.
``DAO_GPU_PIPELINE_PROFILE=1`` runs without the graph and adds the GPU time of
each step.


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
