Applications
============

daoTools ships a comprehensive set of command-line applications covering centroiding, matrix-vector multiply, pixel calibration, SHM utilities, data conversion, and simulation. All C programs are built by waf and installed to the system prefix; Python scripts are run directly.

Source: ``apps/``


Wavefront Sensing & Centroiding
--------------------------------

daoComputeCentroid
~~~~~~~~~~~~~~~~~~

Core Shack-Hartmann centroid computation. Reads frames from an input SHM, computes centre-of-mass slopes for each sub-aperture, and writes the result to an output SHM.

.. code-block:: bash

    daoComputeCentroid -v <camera_shm> -o <slopes_shm> \
                       -r <subap_ref_shm> -t <threshold_shm>

Variants:

- ``daoComputeCentroidPws`` — Pyramid WFS centroiding
- ``daoComputeCentroidRelative`` — Relative (differential) centroiding
- ``daoComputeCentroids.py`` — Python reference implementation
- ``daoComputeCentroidsSlow.py`` — Slow (debug) Python centroider
- ``daoComputeIntensityPws.c`` — Pyramid WFS intensity measurement

daoComputeCentroidCorrelation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Cross-correlates each sub-aperture spot against a reference template (instead of a centre-of-gravity). The reference SHM holds one ``subaSize``x``subaSize`` template per sub-aperture, stacked row-wise; the correlation peak is found by an integer-pixel windowed shift search out to ``searchRange``.

.. code-block:: bash

    daoComputeCentroidCorrelation -S <in> <centroid> <subApCentres> <refImage> <threshold> \
                                   <subaSize> <nbSuba> <searchRange> -s <semNb> [-a <alpha>] -L

``-a <alpha>`` enables an optional running-average (EMA) update of the reference: after each frame's centroids are published, the just-observed spot (re-aligned by that frame's own centroid) is blended into the reference at rate ``alpha`` in ``(0, 1]``, so the reference tracks slow drift instead of staying fixed at its initial calibration. Runs strictly after the centroid SHM is published, off the real-time critical path. Disabled (``alpha=0``) by default.

daoComputeCentroidCorrelationFFT
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Same correlation-centroiding idea, but computes the full periodic correlation surface for each sub-aperture via FFT instead of a windowed shift search — the whole box is searched for the price of one FFT round trip, so no ``searchRange`` argument is needed. Precision (float32 vs float64) is auto-detected from the input image SHM's ``atype``; all 5 SHMs must share that atype. Requires FFTW (see the top-level README) — built only when available.

.. code-block:: bash

    daoComputeCentroidCorrelationFFT -S <in> <centroid> <subApCentres> <refImage> <threshold> \
                                      <subaSize> <nbSuba> -s <semNb> [-a <alpha>] -L

Same ``-a <alpha>`` running-average reference update as ``daoComputeCentroidCorrelation``.

daoPrepCentroidLut.py
~~~~~~~~~~~~~~~~~~~~~

Generates the sub-aperture look-up table (LUT) and reference SHMs required by the centroid processes:

.. code-block:: bash

    daoPrepCentroidLut.py --config system.yaml

daoPrepPwfs.py
~~~~~~~~~~~~~~

Prepares reference data structures for pyramid WFS operation.


Matrix-Vector Multiply (MVM)
-----------------------------

daoMvM
~~~~~~

High-performance matrix-vector multiply reconstructor. Multiplies the control matrix by the incoming slope vector and writes DM commands.

.. code-block:: bash

    daoMvM -m <CM_shm> -v <slopes_shm> -o <dm_shm>

- ``daoMvM.py`` — Python reference implementation (slower, for debugging)
- ``daoMvMGPU.c`` — GPU-accelerated MVM via CUDA (requires CUDA build)


Pixel Calibration
-----------------

daoPixelCalibrate
~~~~~~~~~~~~~~~~~

Applies flat-field and background correction to a raw camera stream:

.. code-block:: bash

    daoPixelCalibrate -S <raw_shm> <flat_shm> <background_shm> <output_shm> -s <semNb> [-C <cpu>] -L

``-C <cpu>`` pins the real-time thread to a CPU core. The loop also locks memory (``mlockall``), pre-warms the calibration buffers before entering the loop, and throttles its telemetry print to once every 2000 frames — all to keep page-fault and I/O jitter off the real-time path.

- ``daoPixelCalibratePws.c`` — Pyramid WFS variant with additional corrections

daoTakeBg.py
~~~~~~~~~~~~

Records background frames and writes the mean to a SHM file:

.. code-block:: bash

    daoTakeBg.py <camera_shm> <background_shm> --nframes 200


SHM Utilities
-------------

daoShmMonitoring
~~~~~~~~~~~~~~~~

Monitors a SHM stream and prints per-frame statistics (mean, min, max, count) to the terminal:

.. code-block:: bash

    daoShmMonitoring <shm_file>

- ``daoShmMonitoring1Value.c`` — Single-scalar SHM monitor

daoShm2Fits
~~~~~~~~~~~

Records ``N`` frames from a SHM stream and saves them to a FITS file:

.. code-block:: bash

    daoShm2Fits <shm_file> <output.fits> <N_frames>

daoFits2Shm.py
~~~~~~~~~~~~~~

Writes a FITS file (or cube) into a SHM stream, one frame at a time:

.. code-block:: bash

    daoFits2Shm.py <input.fits> <shm_file>

daoSnapshot.py
~~~~~~~~~~~~~~

Captures a single frame from a SHM and saves it to FITS or NumPy:

.. code-block:: bash

    daoSnapshot.py <shm_file> <output.fits>

daoShmRate.py
~~~~~~~~~~~~~

Measures and prints the update rate of a SHM stream:

.. code-block:: bash

    daoShmRate.py <shm_file>

SHM Arithmetic
~~~~~~~~~~~~~~

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - Tool
     - Description

   * - ``daoShmAdd``
     - Adds two SHM streams element-wise and writes to an output SHM.

   * - ``daoShmCombiner``
     - Combines multiple SHM channels by summing into one output.

   * - ``daoShmConcatenate``
     - Concatenates two SHM arrays along the first axis.

   * - ``daoShmConcatenateFine``
     - Fine-grained concatenation with configurable stride.

   * - ``daoAvgShm``
     - Running average of SHM frames.

   * - ``daoAvgDoubleShm``
     - Running average for double-precision SHM.

   * - ``daoStatShm``
     - Per-pixel statistics (mean, variance) over a SHM stream.

   * - ``daoDownsample``
     - Spatial downsampling of a 2-D SHM frame.

   * - ``daoPixelExtract``
     - Extracts a sub-region from a SHM frame.

   * - ``daoApplyGain``
     - Multiplies a SHM stream by a scalar gain SHM.


Loop & Filter Utilities
-----------------------

daoLeakyIntegrator
~~~~~~~~~~~~~~~~~~

Standalone leaky integrator: accumulates an input SHM stream with configurable gain and leak:

.. code-block:: bash

    daoLeakyIntegrator -v <input_shm> -o <output_shm> \
                       -g <gain_shm> -l <leak_shm>

- ``daoLeakyIntegratorMap.c`` — Per-actuator gain/leak map variant

daoClock
~~~~~~~~

High-resolution software clock: ticks a SHM counter at a target frequency:

.. code-block:: bash

    daoClock <clock_shm> <freq_shm>

daoHighPassFilter
~~~~~~~~~~~~~~~~~

Temporal high-pass filter applied per-pixel to a SHM stream.

daoModesCutoff / daoModesCutoffFull
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Modal cutoff filters: zeroes out higher-order modes in the control matrix output.

daoCommandFilter
~~~~~~~~~~~~~~~~

Filters the DM command SHM through a user-supplied transfer function.


Timing & Latency
----------------

daoTimeDiff
~~~~~~~~~~~

Measures the latency between two SHM timestamps and now computes the running average and RMS automatically over a sliding window of ``popSize`` samples:

.. code-block:: bash

    daoTimeDiff -S <SHM1> <SHM2> <sem1> <sem2> <measurement_shm> [-n <popSize>] [-m] -L

- ``-n <popSize>`` — sliding-window size for the AVG/RMS computation (default 100)
- ``-m`` — verbose mode: also print each pair's frame IDs and raw diff (off by default; without it the print is a fixed-width, ~1 Hz-throttled line so per-iteration ``fflush`` stays off the critical path)
- Publishes ``<measurement>Avg`` and ``<measurement>Rms`` scalar SHMs plus a ``<measurement>Array`` SHM holding the sliding window in chronological order, for GUIs (e.g. ``daoShmViewer``'s "SHM Latency" tab) to plot directly without re-measuring anything themselves.

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - Tool
     - Description

   * - ``daoTimeDiffNCurse``
     - ``ncurses`` live display of SHM latency.

   * - ``daoTimeDiffStat``
     - Statistical summary (min/max/mean/std) of SHM latency over N frames. Kept as a separate tool for other uses now that ``daoTimeDiff`` computes AVG/RMS itself.

   * - ``daoSetLatency``
     - Injects a fixed artificial latency into a SHM stream for testing.

   * - ``daoPlotLatency.py``
     - Python script to plot recorded latency data.


Data Conversion & I/O
----------------------

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - Tool
     - Description

   * - ``daoNpy2Shm.py``
     - Write a NumPy ``.npy`` file into a SHM stream.

   * - ``daoTakeDataCubeFITS.py``
     - Record an ROI from N frames from a SHM stream to a FITS cube. Usage is
       ``<shm> <nFrames> <description> <width> <cx> <cy>``. The output is
       written below ``$DAODATA/data`` and includes ``FPS``, ``DIT``,
       ``Nframes``, ``ROI``, ``CX``, and ``CY`` FITS header values.

   * - ``daoTakeDataCubeNPY.py``
     - Record an ROI from N frames to a NumPy ``.npy`` file. Usage is
       ``<shm> <nFrames> <description> <width> <cx> <cy>``; output is written
       below ``$DAODATA/data``.

   * - ``daoDescrambleOcam2``
     - Reorder pixels from OCAM2k's scrambled readout format.

   * - ``daoDMSend``
     - Send a static DM command from a file to the DM SHM.

   * - ``daoRandImageU16Write``
     - Fill a SHM with random ``uint16`` data (for testing).

   * - ``daoRandWriter / daoRandWriterSync``
     - Continuous random data writers (free-running / sync'd to clock).


Simulation
----------

daoReconstructor.py
~~~~~~~~~~~~~~~~~~~

Minimal Python MVM reconstructor script for debugging and offline testing. Reads an input SHM, multiplies by a reconstruction matrix SHM, and writes to an output SHM:

.. code-block:: bash

    daoReconstructor.py -i <input_shm> -r <recon_shm> -o <output_shm>

Runs at reduced scheduling priority (``nice +10``). Intended as a reference implementation — for production use the C ``daoMvM`` binary.

daoTurbulenceSimulator.py
~~~~~~~~~~~~~~~~~~~~~~~~~

Simulates Kolmogorov atmospheric turbulence and streams phase screens into a SHM for closed-loop testing without a real atmosphere:

.. code-block:: bash

    daoTurbulenceSimulator.py --r0 0.15 --L0 25 --shm /tmp/turb.im.shm

daoPwfsSimulator.py
~~~~~~~~~~~~~~~~~~~

Simulates pyramid WFS signals from an incoming phase screen SHM.

daoNoisyPsfGenerator.py
~~~~~~~~~~~~~~~~~~~~~~~~

Generates a simulated noisy PSF and writes it to a SHM.


Logging & Communication
-----------------------

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - Tool
     - Description

   * - ``daoRecvLogs.py``
     - ZMQ subscriber that collects and displays DAO log messages.

   * - ``daoSendLogs.py``
     - Forwards log messages from a file to the ZMQ log broker.

   * - ``daoLogToScreen.py``
     - Pretty-prints live DAO logs to the terminal.

   * - ``daoProxyLog.py``
     - ZMQ log proxy / relay between publishers and subscribers.

   * - ``daoSendCommand.py``
     - CLI wrapper to send a ``daoCommand`` protobuf message to any process.

   * - ``daoStrCmd.py / daoReadStr.py / daoWriteStr.py``
     - String-based SHM command helpers.

   * - ``daoRemoteShmFileServer.py``
     - ZMQ server that exposes local SHM files to ``daoRemoteShmFileClient`` over TCP.

   * - ``daoRedisCheck.py``
     - Checks connectivity and health of the Redis telemetry database.

   * - ``daoDAQCli.py``
     - CLI interface to :doc:`daoDAQ` — upload config, start/stop sessions, query state.


Real-Time Display (RTD)
-----------------------

Lightweight Python RTD scripts that render SHM data to the terminal or a minimal window at high frame rate:

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - Script
     - Description

   * - ``daoImageRTD.py``
     - Display a 2-D image SHM as a false-colour terminal plot.

   * - ``daoImageRTDFloat.py``
     - Float-precision variant of ``daoImageRTD``.

   * - ``daoBarRTD.py``
     - Bar-chart display of a 1-D SHM vector.

   * - ``daoShRTD.py``
     - Shack-Hartmann slope display.

   * - ``daoWavefrontRTD.py``
     - Reconstructed wavefront RTD.

   * - ``daoPlotRTD.py``
     - General-purpose line-plot RTD for scalar SHM streams.
