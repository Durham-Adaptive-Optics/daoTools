Process Controllers
===================

daoTools provides a hierarchy of Python controller classes that manage DAO RTC processes through shared memory (SHM) and :doc:`daolaunch`. Each specialised controller extends the common ``ProcessController`` base class.

Source: ``src/python/``


ProcessController (Base Class)
-------------------------------

``ProcessController`` (``daoProcessController.py``) is the common base for all process controllers. It handles SHM naming conventions, path parsing, and tmux session naming.

.. code-block:: python

    from daoProcessController import ProcessController

    ctrl = ProcessController(
        input_shm='/tmp/camera.im.shm',
        output_shm='/tmp/slopes.im.shm',
        process='daoProcess',
        tmuxname='my_process',          # optional; auto-generated if omitted
        input_shape=(128, 128),
        output_shape=(400, 2),
        input_datatype=np.uint16,
        output_datatype=np.float32
    )

.. list-table::
   :widths: 25 15 60
   :header-rows: 1

   * - Parameter
     - Default
     - Description

   * - ``input_shm``
     - —
     - Full path to the input shared memory file.

   * - ``output_shm``
     - —
     - Full path to the output shared memory file.

   * - ``process``
     - —
     - Executable name of the RTC process to manage.

   * - ``tmuxname``
     - auto
     - tmux session name. Defaults to ``<process>_<input_name>``.

   * - ``input_shape``
     - ``(1,1)``
     - Shape of the input SHM array.

   * - ``output_shape``
     - ``(1,1)``
     - Shape of the output SHM array.

   * - ``input_datatype``
     - ``np.float32``
     - NumPy dtype for input SHM.

   * - ``output_datatype``
     - ``np.float32``
     - NumPy dtype for output SHM.

Common methods inherited by all subclasses:

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - Method
     - Description

   * - ``createShm()``
     - Create input and output SHM files with the configured shapes and dtypes.

   * - ``loadShm()``
     - Open existing SHM files.

   * - ``launch()``
     - Start the process in a tmux session via :doc:`daolaunch`.

   * - ``kill()``
     - Kill the tmux session and process.


CentroidController
------------------

Manages a Shack-Hartmann centroiding process (``src/python/daoCentroidController.py``).

Additional SHM: sub-aperture reference map (``subApRef``), threshold map.

.. code-block:: python

    from daoCentroidController import CentroidController

    ctrl = CentroidController(
        input_shm='/tmp/camera.im.shm',
        output_shm='/tmp/slopes.im.shm',
        process='daoCentroid',
        input_shape=(128, 128),
        output_shape=(400, 2),   # nValidSubs × 2 (x,y slopes)
        nPix=8,                  # sub-aperture size in pixels
        nSubs=50,                # total sub-apertures on grid
        threshold=100            # pixel threshold
    )

    ctrl.createShm()
    ctrl.launch()

Extra constructor parameters:

.. list-table::
   :widths: 25 15 60
   :header-rows: 1

   * - Parameter
     - Default
     - Description

   * - ``nPix``
     - 1
     - Sub-aperture size in pixels.

   * - ``nSubs``
     - 1
     - Total number of sub-apertures (including vignetted ones).

   * - ``threshold``
     - 0
     - Initial pixel threshold applied before centroiding.

   * - ``subApRef_shm``
     - auto
     - Override path for the sub-aperture reference SHM.

   * - ``threshold_shm``
     - auto
     - Override path for the threshold SHM.


MVMController
-------------

Manages a matrix-vector multiply (MVM) wavefront reconstructor process (``src/python/daoMVMController.py``).

Additional SHM: control matrix (``CM``).

.. code-block:: python

    from daoMVMController import MVMController

    ctrl = MVMController(
        input_shm='/tmp/slopes.im.shm',
        output_shm='/tmp/dm.im.shm',
        process='daoMvM',
        input_shape=(400,),   # nSubs * 2 slopes
        output_shape=(97,)    # nActuators
    )

    ctrl.createShm()
    ctrl.setCM(control_matrix)   # set the (nActs × nSubs) interaction matrix
    ctrl.launch()

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - Method
     - Description

   * - ``setCM(CM)``
     - Write a new control matrix to the CM SHM. Shape: ``(nActuators, nSubApertures)``.

   * - ``getCM()``
     - Read and return the current control matrix.

Launch arguments passed to the process: ``-m <CM_shm> -v <input_shm> -o <output_shm>``


LoopController
--------------

Extends ``MVMController`` with closed-loop control SHMs: open/close loop flag, gain, and leak (``src/python/daoLoopController.py``).

Additional SHMs: offset, loop enable flag, loop gain, loop leak.

.. code-block:: python

    from daoLoopController import LoopController

    ctrl = LoopController(
        input_shm='/tmp/slopes.im.shm',
        output_shm='/tmp/dm.im.shm',
        process='daoLoop',
        input_shape=(400,),
        output_shape=(97,)
    )

    ctrl.createShm()
    ctrl.launch()

    # Runtime control
    ctrl.loop_shm.set_data(np.array([[1]], dtype=np.uint32))       # open loop
    ctrl.loop_gain_shm.set_data(np.array([[0.3]], dtype=np.float32))  # set gain
    ctrl.loop_leak_shm.set_data(np.array([[0.99]], dtype=np.float32)) # set leak

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - SHM
     - Description

   * - ``offset_shm``
     - Static offset added to the reconstructed command (shape: ``output_shape``).

   * - ``loop_shm``
     - Loop open/close flag (``uint32``). 0 = closed, 1 = open.

   * - ``loop_gain_shm``
     - Scalar loop gain (``float32``).

   * - ``loop_leak_shm``
     - Scalar integrator leak factor (``float32``), typically close to 1.


PixCalController
----------------

Manages pixel calibration (flat field + background subtraction) for a camera stream (``src/python/daoPixCalController.py``).

Additional SHMs: flat field, background.

.. code-block:: python

    from daoPixCalController import PixCalController

    ctrl = PixCalController(
        input_shm='/tmp/camera_raw.im.shm',
        output_shm='/tmp/camera_cal.im.shm',
        process='daoPixelCalibrate',
        input_shape=(128, 128),
        output_shape=(128, 128)
    )

    ctrl.createShm()
    ctrl.flatField_shm.set_data(flat)         # set flat field (ones by default)
    ctrl.background_shm.set_data(background)  # set background (zeros by default)
    ctrl.launch()

Launch arguments: ``-L <input_shm> <flatField_shm> <background_shm> <output_shm>``


DMCombinerController
--------------------

Manages a DM command combiner that sums multiple input channels into a single DM output (``src/python/daoDMCombinerController.py``).

Additional SHMs: ``nChannels`` individual channel SHMs (auto-named ``<output>00.im.shm``, ``<output>01.im.shm``, …).

.. code-block:: python

    from daoDMCombinerController import DMCombinderController

    ctrl = DMCombinderController(
        input_shm='/tmp/dm_in.im.shm',
        output_shm='/tmp/dm.im.shm',
        process='daoDMCombiner',
        output_shape=(97,),
        nChannels=4        # 4 additive command channels
    )

    ctrl.createShm()
    ctrl.launch()

    # Write to individual channels
    ctrl.channel_shm[0].set_data(flat_command)
    ctrl.channel_shm[1].set_data(ao_command)


clockController
---------------

Manages a real-time clock process that ticks shared memory at a set frequency (``src/python/daoClockController.py``).

.. code-block:: python

    from daoClockController import clockController

    clk = clockController(
        clockName='/tmp/clock.im.shm',
        frequnecy=1000.0,    # Hz
        controller='daoClock'
    )

    clk.createShm()
    clk.launch()

.. list-table::
   :widths: 25 15 60
   :header-rows: 1

   * - Parameter
     - Default
     - Description

   * - ``clockName``
     - —
     - Full path to the clock SHM file.

   * - ``frequnecy``
     - —
     - Target clock frequency in Hz.

   * - ``frequnecyName``
     - ``'Freq'``
     - Override name for the frequency SHM (basename, no extension).

   * - ``controller``
     - ``'daoClock'``
     - Executable name for the clock process.

   * - ``tmuxname``
     - auto
     - tmux session name (defaults to the clock SHM basename).
