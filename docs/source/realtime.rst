Real-time host setup
====================

A real-time loop (camera reader, :doc:`gpu_pipeline`, DM driver) runs the same
work every frame; what makes one frame later than the others is almost always
the host: a process or an interrupt on the loop's core, a CPU waking up from a
deep idle state, a GPU lowering its clocks between frames. This page lists what
to set, from the cheapest to the most intrusive, and the scripts that help.

Measure before and after each change: ``daoGpuPipeline`` prints its average and
worst latency every second, split into *wake*, *params*, *gpu* and *publish*,
and ``daoTimeDiff`` between the camera SHM and the DM command SHM gives the
end-to-end latency of every frame. Look at the worst values and the high
percentiles (99.9 %, 99.99 %), not at the average: the average hardly moves.

``daoRtCheck [-c <cpus>]`` reports how the host is set up for the loop CPUs
(everything on this page) and changes nothing.

The same applies to :doc:`cpu_pipeline`: its ``-C`` pins the loop thread only;
its workers (``-w``) spin and need a core each; keep its BLAS to one thread
(``-N 1``, the default) unless the threads have cores of their own, or the
kernel's real-time throttling stalls the loop for tens of ms.

.. _rt-priority:

Real-time priority and locked memory
------------------------------------

The real-time daoTools applications (``daoGpuPipeline``, ``daoMvM``,
``daoTimeDiff``, the calibration and centroid tools...) run ``SCHED_FIFO``
priority 93 when they are allowed to, and ``daoGpuPipeline`` and
``daoDescrambleOcam2`` also lock their memory (``mlockall``); if not, they say
so at start-up and run anyway, at normal priority. Two ways to allow them:

- An ``rtprio`` and ``memlock`` limit for the user, in
  ``/etc/security/limits.conf`` (applies to every program of that user, at the
  next login)::

      <user>  -  rtprio   99
      <user>  -  memlock  unlimited

- File capabilities on the binaries, ``sudo daoToolSetCap`` (to run again after
  each ``waf install``). A binary with capabilities ignores
  ``LD_LIBRARY_PATH``: ``$DAOROOT/lib`` must then be known to ``ldconfig`` (a
  file in ``/etc/ld.so.conf.d/`` listing it, then ``sudo ldconfig``); the script
  warns when it is not.

The semaphore wake-up of a loop at normal priority waits for whatever runs on
its core: it is the largest single effect on the worst frames.


GPU clocks and persistence
--------------------------

Left to itself, a GPU lowers its clocks between frames and raises them again
when work arrives, and it unloads its driver state when no process uses it.
``daoGpuClocks`` sets every GPU (or one, ``-i``) up for the loop:

.. code-block:: bash

    daoGpuClocks -q          # current state
    daoGpuClocks -n          # dry run: the nvidia-smi commands it would run
    sudo daoGpuClocks        # persistence on, clocks locked at their maximum, power limit at its maximum
    sudo daoGpuClocks -u     # undo: default clocks and power limit (persistence stays on)

The clocks are locked with ``nvidia-smi -lgc`` (Volta and later), or with the
application clocks (``-ac``) where ``-lgc`` is not supported. The settings last
until the next reboot or driver reload: run it from the loop's start script (or
a systemd unit). A locked GPU still slows down when it is too hot: check its
temperature under load (``nvidia-smi -q -d PERFORMANCE``, *Clocks Event
Reasons*).

When several processes compute on the same GPU, run NVIDIA MPS (daoBase's
``daoGpuMps start``); a single ``daoGpuPipeline`` process does not need it.


.. _rt-core-layout:

One core per real-time process
------------------------------

Each real-time process of the loop gets a core of its own, and nothing else
runs there. ``daoRtCpus`` lists the CPUs to use, best first (performance cores,
one thread per physical core, CPU 0's core left out); ``daoRtPin``, sourced by
a start script, places each process on its CPU by a fixed index (its *role*),
so that a process started again lands on the same core:

.. code-block:: bash

    source daoRtPin
    # roles: 0 camera reader, 1 pipeline, 2 DM driver
    daoTmux cam  "$(dao_rt_pin 0 "<camera reader>")"
    daoTmux loop "$(dao_other_pin 3 "daoGpuPipeline -c loop.yaml -C $(dao_rt_cpu 1) -L")"
    daoTmux dm   "$(dao_rt_pin 2 "<DM driver>")"
    # not real time: GUIs, displays, telemetry, daoDAQ
    daoTmux tel  "$(dao_other_pin 3 "<telemetry>")"

- ``dao_rt_pin <role> "<cmd>"`` runs the command under ``taskset -c <cpu>``:
  all its threads on that CPU.
- ``dao_rt_cpu <role>`` is the CPU alone, for an option such as
  ``daoGpuPipeline -C``.
- ``dao_other_pin <n> "<cmd>"`` runs the command on every CPU but the physical
  cores of the first *n* roles (their hyperthreads too): the processes that are
  not real time never share a core with the loop.
- ``dao_rt_core <role>`` is the role's whole physical core (the CPU and its
  hyperthreads), as a ``taskset -c`` list.
- ``DAO_RT_CPUS="2 6 10"`` chooses the CPUs by hand (``none``: no pinning).

``daoGpuPipeline -C`` pins only the loop thread. Its other threads (CUDA's,
created at start-up, also real time) keep the process's CPUs: start it with
``dao_other_pin`` as above, so they stay off the real-time cores; with
``dao_rt_pin`` they share the loop's CPU, and without either they can wake up on
any core, another process's real-time core included.

A process with **two real-time threads of the same priority** needs more than
one CPU when one of them never sleeps: on one CPU the other runs only when the
first one sleeps, i.e. never. An Aravis camera reader that polls its stream
(``daoHwAravisDeviceCtrl``) next to Aravis's own receive thread is one: pinned to
a single CPU it receives no frame at all. Give it its whole core,
``taskset -c $(dao_rt_core 0) <camera reader>`` (``dao_rt_pin`` leaves a command
that already starts with ``taskset`` unchanged).

Pinning only keeps the loop's processes in place; it does not keep the others
away. For that, see :ref:`rt-host-settings`.


.. _rt-host-settings:

Host settings
-------------

These change the whole host and need root; set them once the loop works and
the measurements show the worst frames come from the host (a large *wake*, or
worst frames far above the average).

**Isolate the loop cores** on the kernel command line (``/etc/default/grub``,
``GRUB_CMDLINE_LINUX``, then ``sudo update-grub`` and a reboot), e.g. for CPUs 2,
3, 6 and 10::

    isolcpus=managed_irq,domain,2,3,6,10 nohz_full=2,3,6,10 rcu_nocbs=2,3,6,10 irqaffinity=0,1,4,5,8,9,12-19

- ``isolcpus``: the scheduler puts nothing on these CPUs by itself, and
  (``managed_irq``) the kernel's own device interrupts avoid them; a process is
  still allowed there if its affinity says so (taskset, ``daoRtPin``).
- ``nohz_full``: no timer tick while a single task runs there (a spinning loop,
  ``-W``).
- ``rcu_nocbs``: the kernel's RCU callbacks run elsewhere.
- ``irqaffinity``: the default CPUs of interrupts.

``isolcpus`` alone does not move processes that were explicitly allowed on every
core: keep the user's and system's processes on the housekeeping CPUs with
``CPUAffinity=0,1,4,5,8,9,12-19`` in ``/etc/systemd/system.conf`` and
``/etc/systemd/user.conf``.

**Interrupts** away from the loop cores: with ``irqbalance`` stopped
(``sudo systemctl disable --now irqbalance``), or told to leave them alone
(``IRQBALANCE_BANNED_CPULIST=2,3,6,10`` in ``/etc/default/irqbalance``), set
each interrupt's CPUs in ``/proc/irq/<n>/smp_affinity_list``. Keep the
interrupts of the camera's network card on the camera reader's core, or on a
housekeeping core next to it, rather than spread over all CPUs. ``daoRtCheck``
lists the interrupts still handled on the loop CPUs.

**tuned**: the ``latency-performance`` profile sets the CPU governor to
``performance``, keeps the CPUs out of deep idle states and turns off a few
power-saving features (``sudo tuned-adm profile latency-performance``).

**CPU idle states** (C-states): a CPU in a deep idle state takes tens to
hundreds of µs to wake up, which a loop waiting on a semaphore pays on every
frame. Without tuned:

- ``performance`` governor: ``sudo cpupower frequency-set -g performance``;
- no deep idle state on the loop CPUs: ``sudo cpupower -c 2,3,6,10 idle-set -D 10``
  (disables the states slower than 10 µs to leave), or for the whole host
  ``intel_idle.max_cstate=1 processor.max_cstate=1`` on the kernel command line;
- a spinning loop (``daoGpuPipeline -W``) never idles: its core needs none of
  this, but the camera reader and DM driver, woken by their semaphores, do.

**Real-time throttling**: by default real-time tasks get at most 95 % of a CPU
while other tasks wait for it (``kernel.sched_rt_runtime_us = 950000``). A loop
that never sleeps (``-W``) on a core of its own is not affected; see
:doc:`gpu_pipeline` (Running) before turning the limit off.


What it gave on one host
------------------------

An 800 Hz Shack-Hartmann loop (160x160 camera frame from a GigE camera reader,
``pixelCalibrate``, ``centroidRelative``, ``mvm``, ``leakyIntegrator`` in
``daoGpuPipeline``), on a desktop (i5-14600KF, RTX 5060 Ti) with a stock
kernel, none of the host settings above, nothing else running. Latency from the
camera SHM to the DM command SHM (``daoTimeDiff``), 240 000 frames per case, µs:

.. list-table::
   :header-rows: 1

   * - Case
     - median
     - 99 %
     - 99.9 %
     - 99.99 %
   * - semaphore wait, no real-time priority
     - 42.6
     - 81.0
     - 645.6
     - 1198.9
   * - semaphore wait, real-time priority
     - 41.7
     - 51.9
     - 125.3
     - 144.5
   * - spinning (``-W``)
     - 30.5
     - 33.9
     - 44.9
     - 80.1
   * - spinning, one core per process (``daoRtPin``)
     - 30.4
     - 33.1
     - 36.0
     - 79.3
   * - spinning, ``hostAccess: copy``
     - 23.5
     - 31.1
     - 32.4
     - 72.3

- Without real-time priority, one frame in a thousand waits for whatever else
  runs on its core.
- Waiting on a semaphore, the loop's core went into its deepest idle state
  (127 µs to leave) about twice per frame; spinning, almost never. The
  semaphore runs varied from day to day (99.9 % between 51 and 125 µs):
  limiting the idle states (above) is what would make them repeatable.
- What is left when spinning, about one frame in ten thousand at 2.5 times the
  median, is GPU time (the graph's launch and completion), on a GPU driving the
  display or not.
