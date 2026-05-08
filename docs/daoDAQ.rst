================
daoDAQ  v2.0.0
================

The DAO Data Acquisition tool, named daoDAQ and referred to simply as DAQ throughout this document, is a server application designed to facilitate
data acquisition within high-throughput DAO RTC systems, supporting capture from sources such as DAO shared memory and data files.

The tool ships with the ``daoTools`` package; 
check the latest version of the tool is installed on your system 
by running the following

.. code-block:: bash

   daoDAQ --version


Running a DAQ Server
======================

First you must run an instance of the Dao DAQ tool on your system. This
hosts a communication server allowing you to provide configurations and
and command DAQ sessions where you can capture your data.

By default the tool's server is hosted locally on port ``62000`` and logs are emitted
to a file named ``daoDAQ.logs`` under the working directory of the program instance.

Basic launch:

.. code-block:: bash

   daoDAQ

Custom port:

.. code-block:: bash

   daoDAQ --port 73000

Log to standard output instead of a log file:

.. code-block:: bash

   daoDAQ --stdout-logging

For all available options:

.. code-block:: bash

   daoDAQ --help


DAQ Configuration
==================

Before you can capture data from the RTC, you must first provide the tool with a 
configuration that informs it what data to capture and how, at which point you
can proceed to carrying out DAQ sessions whereby the data is captured and stored
according to your provided configuration.

DAQ session configuration is specified using a YAML v1.2 document. The following
describes the structure of a valid DAQ configuration.

Configuration Structure
-----------------------

A DAQ configuration contains two main sections:

- **Session parameters**: General settings for the DAQ session
- **Sources**: List of sources to acquire data from

.. important::

   All required parameters must be correctly specified. Omission or invalid values will cause a parse error and transition the tool to the Error state.

Session Parameters
~~~~~~~~~~~~~~~~~~

Session parameters are specified at the YAML root level under ``session_parameters``:

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

Acquires data samples in real-time from dao shared memory and writes them to output datafiles on the disk.

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

   * - ``eager_start``
     - Optional
     - Boolean
     - True
     - An eager start captures the sample already present in shared memory when the session begins; a non-eager start waits for the next sample to arrive.

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
       eager_start: true

Providing a Configuration
-------------------------

A DAQ configuration can be provided to the tool in two ways:

1. **At tool launch** (configuration does not change):

   .. code-block:: bash

      daoDAQ --daq-configuration /path/to/daq-config.yaml

2. **Via API after launch** (configuration can be updated anytime):

   .. code-block:: python

      from daoDAQ import DAQClient
      client = DAQClient()
      with open("path/to/config-file.yml", "r") as daq_config_file:
         daq_config = daq_config_file.read()
         client.daq_session_configure_upload(daq_config)
         client.daq_session_configure_apply()

DAQ Sessions
============

Starting a Session
------------------

.. code-block:: python

    from daoDAQ import DAQClient

    client = DAQClient()
    ...
    client.daq_session_begin()

DAQ sessions will automatically finish once all data sources have been fully captured.  
For example, in sessions that involve only files or shared memory with a finite number of samples, 
the session will complete automatically once all samples have been acquired from all sources.  

You can wait for such sessions to finish using:

.. code-block:: python

    from daoDAQ import DAQClient

    client = DAQClient()
    ...
    client.daq_session_await_finish()

Finishing a Session
-------------------

If at least one data source can capture data indefinitely, the DAQ session will **not** finish automatically 
and must be ended manually. You can do this using:

.. code-block:: python

    from daoDAQ import DAQClient

    client = DAQClient()
    ...
    client.daq_session_finish()

After a session completes successfully, you can immediately start a new session using the same DAQ configuration.  
Alternatively, you can upload a new DAQ configuration to run a completely different set of sessions.

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


Recovering from Errors
======================

In the case where the DAQ tool encounters a serious issue at anytime
during its operation, it will enter into the Error state. 

In such state the tool will ensure any DAQ session that was in-progess at the time
of the error is gracefully stopped and any resources allocated for
carrying out DAQ sessions will be destroyed. 

The logs must be consulted to understand the root cause
of the error. Once the issue has been resolved, 
then recovery can be attempted.

Recovery will attempt to bring the tool back into a state where
it can carry out DAQ sessions. Therefore a valid DAQ configuration
must be applied before recovery is attempted.

The following demonstrates how to attempt a recovery.

.. code-block:: python

   from daoDAQ import DAQClient
   client = DAQClient()
   client.recover()

If recovery proves unsuccessful, the last resort is to simply terminate the DAQ server 
instance and re-launch it. For example, on Linux systems the following will gracefully terminate
any and all DAQ server instances:

.. code:: bash

   kill -2 $(pgrep daoDAQ) 

DAQ Client Library
==================

The **daoDAQ** Python module provides a high-level client library that abstracts away 
the details of communicating with the DAQ server.

.. note::
   
   All client methods will raise an exception if the tasks fails for any reason.

The following demonstrates a simple use-case of the client library, creating
a connection to the DAQ server (default endpoint), uploads a DAQ configuration
from a file on disk, and then performs a three second capture session. 

.. code-block:: python

   from daoDAQ import DAQClient

   client = DAQClient()
   with open("/path/to/daq-config.yaml", "r") as file:
      daq_config: str = file.read()
      client.daq_session_configure_upload(daq_config)
      client.daq_session_configure_apply()

   client.daq_session_begin()
   sleep(3)
   client.daq_session_finish()

For more details see ``daoDAQClient.py`` for a detailed reference of the client
API and examples on how to use it.

Command-Line Interface
======================

The **daoDAQClient** tool provides stateless CLI access to all DAQ operations, suitable for shell scripts and manual control;
built on top of the aforementioned client library.

For a complete list of available commands and options:

.. code-block:: bash

   python daoDAQClient.py --help

User Support & Feedback
=======================

For issues, questions, or feature requests, please contact thomas.n.davies@durham.ac.uk