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

Pick any two SHMs from the existing file list, then Start: this launches the real ``daoTimeDiff`` binary in its own named ``tmux`` session (it waits on a semaphore of its own, so it never takes frames from the pipeline's readers). The tab does not re-measure anything itself — it purely reads ``daoTimeDiff``'s own ``Array``/``Avg``/``Rms`` SHMs and renders a small scatter plot (with AVG/RMS reference lines) plus a histogram. Stop kills the tmux session.


daoShmWebViewer
---------------

The SHM viewer in a web browser: from the RTC itself, or from any machine that can reach it.

.. code-block:: bash

    daoShmWebViewer.py                        # http://localhost:8090, the SHMs of /tmp, read-only
    daoShmWebViewer.py --write                # also set / load / create SHMs from the page
    daoShmWebViewer.py --dir /tmp --dir /dev/shm/loop --port 8091
    ssh -L 8090:localhost:8090 rtc            # from a laptop, then http://localhost:8090

- **Several SHMs at once**, side by side: click an SHM to show it in the selected panel,
  Ctrl-click (or **+**) to open it in another. ↑ ↓ in the list step through the SHMs.
  The panels and their settings are kept in the browser, and ``?open=shmA,shmB`` opens given
  SHMs (a link to share).
- **Views**: image (colour maps, linear / sqrt / log / asinh, range min–max, percentile, ±max,
  manual or locked, rotation and flips, zoom and pan, the values drawn in the pixels once
  zoomed in), line or bars for vectors, a table (editable with ``--write``), a big number for
  a scalar. Cubes: a slice; complex SHMs: modulus, real part, imaginary part or phase.
- **Processing in the page**: average of the last N frames, difference to a reference frame.
- **Strip** under each view: the trend of the mean, std, min / max, sum, a picked pixel, or the
  frames skipped; or the histogram of the frame.
- **Status**: cnt0, the SHM's update rate, the frames shown, the delay from the write to the
  read, min / max / mean / std. An SHM that stopped updating is flagged.
- **Inspector** (ⓘ): every metadata field, the semaphores and the processes waiting on them
  (as the Qt viewer's tab), and the frame saved as ``.npy``, ``.fits``, ``.csv`` or ``.png``;
  with ``--write``: set all to a value, random values, load a ``.npy`` / ``.fits`` file.
- **Quick record** (inspector, Record): N consecutive frames with their ``cnt0``, ``cnt2`` and
  timestamps, downloaded as ``.npy``, ``.npz`` or ``.fits`` (or saved on the server with
  ``--write``). Missed frames show as gaps in ``cnt0`` and are counted. A CPU SHM wakes the
  recorder with a semaphore of its own, released at the end; a GPU SHM's host copy is polled.
  At most ``--record-max`` MB (2048).
- **Tools** (header, or ``o``):

  - *Latency*: ``daoTimeDiff -S`` between two SHMs in its own tmux session, the same session
    name as daoShmViewer's tab (``daoLatency_<a>_<b>``), its AVG / RMS, scatter and histogram;
    the sessions started by either viewer are listed.
  - *Snapshots*: ``daoSnapshot.py create`` / ``load``, files in ``--snapshot-dir``
    (``$DAODATA/daoSnapshots``), downloadable. Only files already on the server can be loaded.
  - *tmux*: the sessions, the output of each pane (read-only, refreshed every second), the
    ``ssh -t <host> tmux attach -t <session>`` line; with ``--write`` and a display on the
    server, a terminal attached to it, as the Qt viewer's Connect.
  - *DAQ*: the daoShmViewer DAQ tab: root storage, SHM and file sources with their options,
    YAML import / export, Start / Finish (and Recover) through ``DAQClient``.

The server (Python, Tornado) maps the SHM files read-only: it takes no semaphore, never writes
an SHM it only shows, and never attaches a GPU SHM (it reads its ``/tmp`` host copy when the
SHM keeps one, ``DAO_GPU_MIRROR``). Each panel gets a frame when ``cnt0`` changes, at most at
its fps (20 by default), and only once the browser took the previous one: a slow link drops
frames, it never slows the loop. Frames larger than 4 M elements are decimated. Writes go
through libdao, as the Qt viewer's.

``--write`` allows every change from the page: SHMs (set, load, create), processes (latency
measurements, snapshot loads, DAQ sessions, terminals) and files (snapshots, saved recordings).
Without it the page only reads: views, metadata, semaphores, recordings downloaded, tmux output,
DAQ state.

It listens on localhost by default. ``--host 0.0.0.0`` opens it to the network; add
``--token <secret>`` (then ``http://rtc:8090/?token=<secret>`` once). The page loads nothing
from the internet.


daoShmTui
---------

The SHM viewer in a terminal (Rust, ratatui, ``gui/daoShmTui``): over ssh, in tmux, without a
display.

.. code-block:: bash

    daoShmTui                         # the SHMs of /tmp
    daoShmTui -d /tmp -d /dev/shm/loop shIm

The list of SHMs (shape, type, update rate; filter, sort) next to the selected one: an image in
half blocks (two pixels per character cell, true colour or 256 colours) with colour maps,
scales, ranges, rotation, flips, zoom on a cursor and the values drawn in large pixels; a line or
bars for vectors, a large number for a scalar, a table; cube slices and complex parts; averages
and a reference frame; a trend or histogram strip; the metadata and the semaphores' readers
(``i``); the frame saved as ``.npy`` (``w``). Tabs ``1``–``7``, as daoShmViewer's: view,
semaphores (who waits, PID and command), quick record (``.npy`` / ``.npz`` with cnt0, cnt2,
timestamps), SHM-to-SHM latency (``daoTimeDiff`` in tmux), snapshots (``daoSnapshot.py``), tmux
sessions (output, attach) and DAQ (``daoDAQCli.py``). ``--write``: set, random, edit a value, load
a ``.npy``, create an SHM (through libdao, loaded at run time), start measurements, load
snapshots, run DAQ sessions. ``waf build install`` builds and installs it when
``cargo`` and ``rustc`` >= 1.75 are found (``waf configure --without-rust``: never); a failed cargo
build only warns. Without ``--write``, read-only as daoShmWebViewer: the files are mapped
read-only, no semaphore is taken, libdao is not loaded. ``?`` lists the keys;
``gui/daoShmTui/README.md`` has the rest.


SHM Pipeline and Process Monitor
--------------------------------

``daoShmPipeline.py`` draws the active local SHM graph. Each process is shown once, with directed connections through the SHMs it reads and writes. Click a process or SHM node for its command line, connections, data type, shape, update rate, and semaphore details. Orphaned SHMs and the viewer process itself are omitted.

``daoShmProcess.py`` shows only processes currently connected to discovered SHMs. The terminal view refreshes continuously by default; ``--once`` prints one snapshot, while ``--qt`` opens a process table. CPU cores are reported as CPU percent divided by 100, and the thread column reports each process's current thread count.

Run from the workspace root:

.. code-block:: bash

    python daoTools/gui/daoShmPipeline.py
    python daoTools/gui/daoShmProcess.py
    python daoTools/gui/daoShmProcess.py --qt
    python daoTools/gui/daoShmProcess.py --once

Both tools accept ``--directory`` to override the SHM directory. The default is ``/tmp`` on Linux and the current directory on Windows. The process tools use ``psutil`` for cross-platform process metadata; install it through ``daoTools/pyrequirements.txt``.

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


daoFastAndFuriousCtrl
---------------------

Runs the Fast & Furious focal-plane wavefront sensing (``daoFastAndFurious.py``):

.. code-block:: bash

    python daoFastAndFuriousCtrl.py -i <camera> -o <output> -z <M2A> -m <dmMap>

- Build the basis, calibrate, start / stop the loop; each step runs
  ``daoFastAndFurious.py`` as a subprocess, its output in the log
- Gain, leak and hold applied live through control SHMs (``<prefix>Gain``,
  ``<prefix>Leak``, ``<prefix>Enable``, default prefix ``/tmp/ff``)
- A test aberration put on a spare DM channel through M2A (Apply / Clear)
- PSF around the calibrated centre, the Strehl proxy of each iteration, the correction's
  pupil phase
- Files (basis, calibration, final theta) in the work directory (``-w``, default
  ``$DAODATA/fastAndFurious``); the settings are remembered between sessions


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
    python daoTtDisp.py /tmp/flCmd.im.shm --offset 292   # tip/tilt further in the vector

``--offset N`` reads the tip at index N and the tilt at N + 1 (default 0, the
first two values): a TT inside a longer command vector.


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
