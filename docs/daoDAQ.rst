========
daoDAQ
========

Dao Data Acquisition Tool (v2.0.0)

**daoDAQ** is a real-time data acquisition tool that persists data from various sources within an RTC system. 
It acquires from files and shared memory, with flexible output formats and performance tuning for high-throughput 
systems.

Installation & Verification
============================

To verify the tool is installed, run:

.. code-block:: bash

   daoDAQ --version


Getting Started
===============

The daoDAQ tool runs as a server on your RTC system. By default, it listens on port 62000.

Starting the Server
-------------------

Basic launch:

.. code-block:: bash

   daoDAQ

Custom port:

.. code-block:: bash

   daoDAQ --port 73000

Log output to file instead of standard output:

.. code-block:: bash

   daoDAQ --log-file /var/log/daoDAQ.log

For all available options:

.. code-block:: bash

   daoDAQ --help


DAQ Configuration
=================

Before you can acquire data, you must provide the tool with a **DAQ configuration**—a YAML 1.2 document that specifies
what sources to aquire from, how to aquire from them, and how the aquired data should be persisted to the disk.

Configuration can be supplied in two ways:

1. **At tool launch** (configuration does not change):

   .. code-block:: bash

      daoDAQ --daq-configuration /path/to/daq-config.yaml

2. **Via API after launch** (configuration can be updated between sessions):

   .. code-block:: python

      api = daoDAQ.API()
      api.upload_configuration(yaml_config_string)

Configuration Structure
-----------------------

A DAQ configuration contains two main sections:

- **Session parameters**: General settings for the acquisition session
- **Sources**: List of sources to aquire from

Session Parameters
~~~~~~~~~~~~~~~~~~

Session parameters are specified at the YAML root level:

.. list-table::
   :widths: 20 15 15 20 40
   :header-rows: 1

   * - Field
     - Presence
     - Type
     - Default
     - Description

   * - ``root_storage``
     - Required
     - String
     - —
     - Absolute path where session outputs are stored on disk.

   * - ``group_name``
     - Optional
     - String
     - Timestamp (e.g., 2026-04-22_14-30-45)
     - Name of the session's group subdirectory. If omitted, a timestamp is used.

.. warning::

   All required parameters must be correctly specified. Omission or invalid values will cause a parse error and transition the tool to the Error state.


Data Sources
~~~~~~~~~~~~

Two source types are supported:

**1. File Source**

Copies a file from disk to the session output directory when the session begins.

.. list-table::
   :widths: 20 15 15 20 40
   :header-rows: 1

   * - Field
     - Presence
     - Type
     - Default
     - Description

   * - ``uri``
     - Required
     - URI
     - —
     - Absolute path to the file, in the format ``file://<path>``.

**2. Shared Memory Source**

Acquires data samples in real-time from shared memory and writes them to output datafiles.

.. list-table::
   :widths: 20 15 15 20 40
   :header-rows: 1

   * - Field
     - Presence
     - Type
     - Default
     - Description

   * - ``uri``
     - Required
     - URI
     - —
     - Absolute path to the shared memory, in the format ``smem://<path>``.

   * - ``metadata_only``
     - Optional
     - Boolean
     - False
     - If True, capture only frame metadata; discard frame data.

   * - ``samples``
     - Optional
     - Integer
     - Unbounded
     - Number of samples to record. If omitted, collection continues until manually stopped.

   * - ``format``
     - Required
     - String
     - —
     - Output datafile format: ``numpy`` or ``fits``.

   * - ``file_rollover``
     - Optional
     - Integer
     - Unbounded
     - Number of samples per output datafile. When reached, a new datafile is created for subsequent samples. Ensures individual files remain manageable in size.

   * - ``daq_affinity``
     - Optional
     - Integer
     - None
     - CPU core affinity for the data acquisition thread (Linux CPU index).

   * - ``sink_affinity``
     - Optional
     - Integer
     - None
     - CPU core affinity for the writer thread (Linux CPU index).

   * - ``buffer_limit``
     - Optional
     - Integer
     - Unbounded
     - Maximum number of samples the DAQ buffer can hold before dropping new samples.


Configuration Example
~~~~~~~~~~~~~~~~~~~~~

.. code-block:: yaml

   root_storage: /path/to/data/root
   group_name: my_session

   sources:
     - uri: file:///path/to/my/file.ext

     - uri: smem:///tmp/cblue.im.shm
       metadata_only: false
       samples: 1900
       format: numpy
       file_rollover: 70
       daq_affinity: 2
       sink_affinity: 7
       buffer_limit: 900

DAQ Sessions
============

Starting a Session
------------------

First, ensure a DAQ configuration has been provided to the tool. Then use the Python API to begin acquisition:

.. code-block:: python

   api = daoDAQ.API()
   api.start_session()

By default, this call returns immediately. To block until the session completes (useful for bounded acquisitions):

.. code-block:: python

   api = daoDAQ.API()
   api.start_session(block=True)

Alternatively, start the session asynchronously and poll for completion:

.. code-block:: python

   api = daoDAQ.API()
   api.start_session()
   api.await_session_completion()

Ending a Session
----------------

To manually stop an acquisition session:

.. code-block:: python

   api = daoDAQ.API()
   api.end_session()

Automatic Completion
~~~~~~~~~~~~~~~~~~~~~

If all shared memory sources in the configuration specify a bounded ``samples`` parameter, the tool will automatically end the session once all samples have been acquired and written to disk. In such cases, simply await completion:

.. code-block:: python

   api = daoDAQ.API()
   api.start_session()
   api.await_session_completion()  # Blocks until auto-end

Multiple Sessions
-----------------

After a session completes successfully, you can immediately run another acquisition session using the same configuration. Sessions are stored in independent subdirectories under ``root_storage``.

To change the DAQ configuration:

1. End the currently active session (if running)
2. Upload a new configuration via the API
3. Start a new session with the updated configuration

Session Outputs
---------------

After a session completes, the ``root_storage`` directory contains a new subdirectory that holds
all of the datafiles aquired during the DAQ session.

Each shared memory source with ``file_rollover`` specified creates its own subdirectory within the 
session directory to organize its split datafiles; sources without rollover store their single 
datafile directly in the session directory.

.. code-block:: text

   root_storage/
   └── 2026-04-22_14-30-45/           # Session Dedicated Subdirectory
       ├── file_source_name.ext       # A copied file source
       ├── unbounded_smem.fits        # Single datafile (source with no rollover specified)
       └── rolled_smem/               # Subdirectory for source with rollover specified
           ├── rolled_smem_0.npy
           ├── rolled_smem_1.npy
           └── ...

Error Handling
==============

TODO: Document error state handling, recovery procedures, and diagnostic steps.
API.Recover() # asks tool to undergo recovery.
API.Recover(force=true) # reboot tool process.

DAQ Client Library
==================

The **daoDAQ** Python module provides a high-level client library that abstracts away 
the details of communicating with the Dao DAQ server.

.. note::
   
   All client methods will raise an exception if the tasks fails for any reason.

Example Usage
-------------

.. code-block:: python

   from daoDAQ import DAQClient
   import yaml
   
   # Connect to the server
   client = DAQClient(daq_host_addr="localhost", daq_host_port=73000)
   client.ping()  # Verify connectivity
   
   # Load and apply configuration
   with open("daq-config.yaml", "r") as f:
       config = yaml.safe_load(f)
   client.daq_session_configure(yaml.dump(config))
   
   # Begin acquisition
   client.daq_session_begin()
   
   # For bounded acquisitions (all sources specify samples limit), await completion
   client.daq_session_await_finish()
   
   # For unbounded acquisitions, manually stop
   # client.daq_session_finish()

Command-Line Interface
======================

The **daoDAQClient** tool provides stateless CLI access to all DAQ operations, suitable for shell scripts and manual control.

Basic workflow:

.. code-block:: bash

   # Check a DAQ server is present and reachable
   daoDAQClient --host <addr> --port <port> ping # Specify host IP and/or port
   daoDAQClient ping # Use localhost and default port.

   # Configure the session
   daoDAQClient configure /path/to/daq-config.yaml
   
   # Start acquisition
   daoDAQClient aquire
   daoDAQClient aquire --wait # for use with bounded session to block until they finish.
   
   # Or start and manage manually
   daoDAQClient aquire
   daoDAQClient finish

For a complete list of available commands and options:

.. code-block:: bash

   daoDAQClient --help

User Support & Feedback
=======================

For issues, questions, or feature requests, please contact thomas.n.davies@durham.ac.uk