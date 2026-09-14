GUI Tools
=========

daoTools includes a suite of **PyQt5** GUI applications for real-time monitoring, visualisation, and control of DAO RTC systems.

Source: ``gui/``


daoShmViewer
------------

The flagship multi-panel viewer that combines live SHM image display with the full **DAQ integration** panel.

.. code-block:: bash

    python daoShmViewer.py [--light]

Features:

- Live display of any DAO SHM stream (camera frames, wavefronts, slopes)
- Built-in DAQ configuration editor (YAML) and session management
- Start / Stop / Finish DAQ sessions without leaving the viewer
- Frame statistics overlay (mean, min, max, frame count)
- Configurable colour maps and display scaling
- Dark by default (``--light`` for light mode)

SHM Latency tab
~~~~~~~~~~~~~~~~

Pick any two SHMs from the existing file list, then Start: this launches the real ``daoTimeDiff`` binary in its own named ``tmux`` session (semaphore numbers default to 5/5, matching ``daoPlotLatency.py``'s convention so as not to steal semaphore posts from real consumers). The tab does not re-measure anything itself — it purely reads ``daoTimeDiff``'s own ``Array``/``Avg``/``Rms`` SHMs and renders a small scatter plot (with AVG/RMS reference lines) plus a histogram. Stop kills the tmux session.


daoImDisp / daoImgDisp
-----------------------

Lightweight single-image SHM viewers for displaying 2-D array streams:

.. code-block:: bash

    python daoImDisp.py [shm_file]
    python daoImgDisp.py [shm_file]

- ``daoImDisp`` — primary image display with configurable colour map and auto-scale
- ``daoImgDisp`` — alternate layout, suitable for non-square arrays


daoDmCtrl
---------

Combined DM display and control panel:

.. code-block:: bash

    python daoDmCtrl.py

Features:

- 2-D actuator map display (live SHM)
- Per-actuator push/pull controls
- Zernike mode injection (tip, tilt, focus, astigmatism, …)
- Flat, zero, and poking commands
- Load/save DM command from FITS file

UI defined in ``daoDmCtrl.ui`` (Qt Designer).


daoDmDisp / daoDmDispNoMap
---------------------------

Read-only DM actuator display panels:

.. code-block:: bash

    python daoDmDisp.py [dm_shm]

- ``daoDmDisp`` — displays actuator commands on a pupil map
- ``daoDmDispNoMap`` — raw linear actuator bar chart, no pupil mapping


daoWfDisp / daoWfDispMap
-------------------------

Wavefront display panels that render a reconstructed phase map:

.. code-block:: bash

    python daoWfDisp.py [wf_shm]
    python daoWfDispMap.py [wf_shm]

- ``daoWfDisp`` — pupil-plane wavefront phase with colour-bar and RMS readout
- ``daoWfDispMap`` — overlays the wavefront on an illumination map


daoSlopesDisp / daoShDisp
--------------------------

Shack-Hartmann slope display panels:

.. code-block:: bash

    python daoSlopesDisp.py [slopes_shm]
    python daoShDisp.py     [slopes_shm]

- ``daoSlopesDisp`` — quiver plot showing X/Y slope vectors per sub-aperture
- ``daoShDisp`` — grid display with colour-encoded slope magnitude


daoTtDisp
---------

Tip-tilt monitor that displays the residual tip and tilt signals from a TTM or WFS as a scatter plot:

.. code-block:: bash

    python daoTtDisp.py [slopes_shm]


daoLoopDisp
-----------

Generic AO loop control panel: open/close a loop and set its leaky-integrator gain and leak. Talks only to plain scalar control SHMs (default ``lpCmd``/``lpGain``/``lpLeak``) — not tied to any specific pipeline.

.. code-block:: bash

    python daoLoopDisp.py [-c <cmd_shm>] [-g <gain_shm>] [-l <leak_shm>] [--light]

Any SHM that doesn't exist yet is greyed out and picked up automatically once it appears, so this can be started before or after the processes that create those SHMs. Dark by default.


daoLoopCtrl
-----------

Another control panel for a classical (leaky) integrator loop, driving the same three scalar SHMs as ``daoLoopDisp`` (``lpCmd``/``lpGain``/``lpLeak`` by default):

.. code-block:: bash

    python daoLoopCtrl.py [--cmd <path>] [--gain <path>] [--leak <path>] \
                           [--gain-max <value>] [--light]

Opening the GUI does not write anything — it shows and polls the current values, so external changes (e.g. ``setLoopGain.py``, another GUI) are reflected live. Dark by default.


daoBarDisp
----------

Real-time bar chart of a 1-D shared-memory vector (one bar per element), e.g. DM commands or centroid vectors:

.. code-block:: bash

    python daoBarDisp.py <shm_file> [--from <N>] [--to <N>] [--fps <Hz>] [--light]

The X extent defaults to the whole vector; ``--from``/``--to`` restrict it to a sub-range. Y-axis autoscales to the visible bars each frame, or can be pinned with fixed min/max. Dark by default.


daoLogMonitor
-------------

Live log viewer for the DAO logging system:

.. code-block:: bash

    python daoLogMonitor.py

Subscribes to the ZMQ log broker and displays scrollable, colour-coded log messages (DEBUG / INFO / WARNING / ERROR / CRITICAL) with filtering by source and log level.


daoRTDMagic / daoRTDMagicSH
-----------------------------

High-framerate "magic" RTD panels that render SHM data at the fastest possible rate:

.. code-block:: bash

    python daoRTDMagic.py   [image_shm]
    python daoRTDMagicSH.py [slopes_shm]

- ``daoRTDMagic`` — generic image RTD using direct OpenGL texturing via PyOpenGL for minimal display latency
- ``daoRTDMagicSH`` — Shack-Hartmann specific variant with sub-aperture grid overlay


daoRemoteShmViewer
------------------

Mirrors and displays remote SHM streams from another machine using :doc:`python_api` ``daoRemoteShmFileClient``:

.. code-block:: bash

    python daoRemoteShmViewer.py --server 192.168.1.10 --port 5555

Browse and open remote SHM files as if they were local. Requires ``daoRemoteShmFileServer.py`` running on the target host.


daoProcessWidget
----------------

A reusable Qt widget that shows the live status of a DAO RTC process (tmux session alive / dead, CPU load, last-update timestamp). Embedded in other GUI applications such as ``daoShmViewer``.


daoShmTelemetryConfigurator
----------------------------

GUI for creating and editing **daoDAQ** YAML configurations graphically:

- Add/remove SHM sources and file sources
- Set sample counts, rollover limits, CPU affinities
- Preview the generated YAML before uploading
- Integrated upload button (calls ``daoDAQCli.py upload``)

This widget is embedded inside ``daoShmViewer`` as the DAQ configuration panel.
