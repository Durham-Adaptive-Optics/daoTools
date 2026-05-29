Process Management
==================

``daoLaunch`` provides a unified interface for starting, stopping, and health-checking DAO RTC processes inside **tmux** sessions, optionally on remote machines via SSH.

Source: ``src/python/daoLaunch.py``


manage_process
--------------

The primary entry point for process lifecycle management:

.. code-block:: python

    import daoLaunch

    # Launch a process in a local tmux session
    daoLaunch.manage_process(
        action='launch',
        tmuxname='centroid_loop',
        processExe='daoCentroid',
        processArgs='-v /tmp/camera.im.shm -o /tmp/slopes.im.shm',
        workingDir='/opt/dao'
    )

    # Kill the process and destroy its tmux session
    daoLaunch.manage_process(
        action='kill',
        tmuxname='centroid_loop'
    )

    # Launch on a remote machine as a different user
    daoLaunch.manage_process(
        action='launch',
        tmuxname='mvm_loop',
        user='rtc',
        machine='rtchost',
        processExe='daoMvM',
        processArgs='-m /tmp/CM.im.shm -v /tmp/slopes.im.shm -o /tmp/dm.im.shm'
    )

.. list-table::
   :widths: 20 15 65
   :header-rows: 1

   * - Parameter
     - Required
     - Description

   * - ``action``
     - Yes
     - ``'launch'`` to start the process, ``'kill'`` to stop it and destroy the session.

   * - ``tmuxname``
     - Yes
     - Name of the tmux session to create or destroy.

   * - ``user``
     - No
     - Remote user for SSH. If omitted the command runs locally.

   * - ``machine``
     - No
     - Remote hostname or IP for SSH. Must be set together with ``user``.

   * - ``processExe``
     - Launch only
     - Executable name or path to run inside the tmux session.

   * - ``processArgs``
     - No
     - Arguments string passed directly to ``processExe``.

   * - ``workingDir``
     - No
     - Working directory inside the tmux session before launching (default: ``'.'``).

**Launch sequence** (when ``action='launch'``):

1. Send ``Ctrl-C`` to any existing session with the same name.
2. Create a new tmux session (``tmux new -d -s <tmuxname>``).
3. ``cd`` to ``workingDir`` inside the session.
4. Execute ``processExe processArgs`` inside the session.

**Kill sequence** (when ``action='kill'``):

1. Send ``Ctrl-C`` to interrupt the running process.
2. Send ``exit`` to close the shell.
3. Kill the tmux session (``tmux kill-session -t <tmuxname>``).


check_tmux_session
------------------

Queries whether a named tmux session exists:

.. code-block:: python

    alive = daoLaunch.check_tmux_session(
        tmuxname='centroid_loop',
        machine='rtchost',   # optional
        user='rtc'           # optional
    )

    if alive:
        print("Session is running")

Returns ``True`` if the session appears in ``tmux list-sessions``, ``False`` otherwise. Works locally or over SSH.


Integration with ProcessController
-----------------------------------

All :doc:`process_controllers` call ``daoLaunch.manage_process`` internally when their ``launch()`` and ``kill()`` methods are invoked. Direct use of ``daoLaunch`` is only needed for processes that do not have a dedicated controller class.

.. code-block:: python

    # Equivalent: use a controller (preferred)
    ctrl = MVMController(input_shm='/tmp/slopes.im.shm',
                         output_shm='/tmp/dm.im.shm',
                         process='daoMvM',
                         tmuxname='mvm_loop', ...)
    ctrl.launch()

    # Or call daoLaunch directly (for ad-hoc / scripted use)
    daoLaunch.manage_process(action='launch', tmuxname='mvm_loop',
                             processExe='daoMvM', ...)
