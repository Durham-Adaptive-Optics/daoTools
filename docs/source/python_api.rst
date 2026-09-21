Python API
==========

The ``daoTools`` Python modules provide core utilities shared across DAO RTC scripts and higher-level controller modules. They are installed into the Python module directory as flat modules rather than as a namespace package.

Source locations:

- ``src/python/daoTools.py`` — FIFO buffer, FITS utilities, ZMQ helpers, Shack-Hartmann utilities
- ``src/python/daoShmRecord.py`` — multi-stream SHM recording to FITS
- ``src/python/daoRemoteShmFileClient.py`` — ZMQ client for remote SHM file access


daoTools.py
-----------

Fifo (Circular Buffer)
~~~~~~~~~~~~~~~~~~~~~~

A fixed-length circular buffer suitable for maintaining rolling windows of scalar or array data:

.. code-block:: python

    from daoTools import Fifo

    buf = Fifo(100)         # 100-element buffer, initialised to zeros
    buf.Append(new_value)   # push; oldest element is dropped
    buf.GetLast()           # most-recent element
    buf.Get()               # full buffer contents as a list

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - Method
     - Description

   * - ``Fifo(nelem)``
     - Create a circular buffer of ``nelem`` elements initialised to 0.

   * - ``Append(x)``
     - Push ``x`` onto the buffer, discarding the oldest element.

   * - ``GetLast()``
     - Return the most recently appended element.

   * - ``Get()``
     - Return the full contents as a Python list (oldest → newest).


FITS Utilities
~~~~~~~~~~~~~~

.. warning::

    These functions are intentionally minimal. They wrap ``astropy.io.fits`` for simple write operations. For complex FITS workflows use astropy directly.

.. code-block:: python

    from daoTools import createFits, createCubeFits

    createFits(data, 'output.fits')           # Write a 2-D numpy array
    createCubeFits(frames_list, 'cube.fits')  # Write a list of 2-D arrays as a FITS cube

.. list-table::
   :widths: 35 65
   :header-rows: 1

   * - Function
     - Description

   * - ``createFits(data, fname)``
     - Write ``data`` (numpy array) to ``fname`` as a primary FITS HDU. Overwrites existing files.

   * - ``createCubeFits(data, fname)``
     - Write a list of 2-D arrays as image extension HDUs in a single FITS file.


daoShmRecord
------------

``daoShmRecord`` records simultaneous streams from multiple DAO shared memory files and saves them to FITS.

.. code-block:: python

    from daoShmRecord import daoShmRecord

    recorder = daoShmRecord([
        '/tmp/camera.im.shm',
        '/tmp/slopes.im.shm',
    ])

    recorder.record(nFrames=500, fitsfile='recording.fits')

All listed SHM streams must run at the same frame rate.

.. list-table::
   :widths: 35 65
   :header-rows: 1

   * - Method / Argument
     - Description

   * - ``daoShmRecord(shmFiles)``
     - Open a list of SHM file paths. Raises ``FileNotFoundError`` if any path does not exist.

   * - ``record(nFrames, fitsfile)``
     - Record ``nFrames`` frames from every SHM stream. If ``fitsfile`` is provided the result is saved to disk.

.. note::

    Multi-stream recording requires all sources to be synchronised at the same frame rate. Streams running at different rates are not currently supported.


daoRemoteShmFileClient
----------------------

A ZMQ-based client that mirrors remote DAO shared memory files across the network via the ``daoRemoteShmFileServer`` companion process.

.. code-block:: python

    from daoRemoteShmFileClient import daoRemoteShmFileClient

    client = daoRemoteShmFileClient(server_address='192.168.1.10', port=5555)

    # Discover files served by the remote host
    files = client.list_files()

    # Open a remote SHM and sync it to a local SHM
    client.open_file('/tmp/cblue.im.shm')

    # Close when done
    client.close_file('/tmp/cblue.im.shm')

.. list-table::
   :widths: 35 65
   :header-rows: 1

   * - Method
     - Description

   * - ``daoRemoteShmFileClient(server_address, port)``
     - Connect to the file server at ``server_address:port``. Socket timeout is 5 s.

   * - ``list_files()``
     - Return a list of SHM file paths available on the server.

   * - ``open_file(file_path)``
     - Request metadata from the server and set up a local SHM mirror. Re-opens if already open.

   * - ``close_file(file_path)``
     - Tear down the local mirror and notify the server.

The protocol uses ``daoFileServer.proto`` (``FileRequest`` / ``FileResponse`` messages). The ``active_shms`` dictionary tracks all currently mirrored files.
