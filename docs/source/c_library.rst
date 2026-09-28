C Library
=========

The ``daoTools`` C library (``daoTools.h`` / ``daoTools.c``) provides low-level utilities used across DAO RTC processes, plus the header-only ``daoProfile.hpp`` for zero-overhead code instrumentation.

Source locations:

- ``include/daoTools.h``
- ``include/daoProfile.hpp``
- ``src/c/daoTools.c``


daoTools Core (C)
-----------------

The C library exposes a set of utility functions that bridge shared memory naming, network addressing, and data integrity verification.


SHM Name Utilities
~~~~~~~~~~~~~~~~~~

.. code-block:: c

    int daoToolsLocalName(const char* shmPath, char* localName, int* len);

Extracts the local (basename) portion of a shared memory file path, stripping the directory prefix. Used to build derived SHM names consistently across processes.

.. code-block:: c

    void daoToolsInsertShmNamePrefix(const char* base_string,
                                     const char* prefix,
                                     char* final_string);

Inserts a prefix into an SHM name, preserving the directory path and extension. Useful for generating paired SHM files (e.g. ``camera.im.shm`` → ``refcamera.im.shm``). It writes at most 128 bytes; ``daoToolsInsertShmNamePrefixN`` takes the buffer size and returns ``DAO_ERROR`` if the name does not fit.

.. code-block:: c

    int  daoToolsInsertShmNamePrefixN(const char* base_string, const char* prefix,
                                      char* final_string, size_t size);
    void daoToolsArgName(char* dst, size_t size, const char* arg);
    void daoToolsShmOpen(const char* name, IMAGE* image);

SHM names are at most ``DAO_SHM_NAME_LEN - 1`` characters (``dao.h``; with an
older ``dao.h``, the size of ``IMAGE.name``): name buffers are
``char name[DAO_SHM_NAME_LEN]``. ``daoToolsArgName`` copies a name given on the
command line and exits with an error if it does not fit, rather than
overflowing the buffer. ``daoToolsShmOpen`` is ``daoShmOpen`` exiting with an
error when the SHM cannot be opened, rather than letting a tool run on an
unopened ``IMAGE``.


Network Utilities
~~~~~~~~~~~~~~~~~

.. code-block:: c

    unsigned daoToolsIp2Int(const char* ip);

Converts a dotted-decimal IPv4 string to its 32-bit integer representation for use in network configuration structures.


Data Integrity
~~~~~~~~~~~~~~

.. code-block:: c

    uint32_t daoComputeChecksum(const void* data, size_t length_bytes);

Computes a 32-bit checksum over an arbitrary data buffer. Used to validate SHM payloads and configuration data.


IIR Filter History
~~~~~~~~~~~~~~~~~~

.. code-block:: c

    typedef struct {
        float precal[RES_MAX_VAL];
        float dlCmd[FILTER_ORDER][RES_MAX_VAL];
        float dlRes[FILTER_ORDER][RES_MAX_VAL];
        int step;
    } daoFilterHistory;

State structure for a 3rd-order IIR residual filter. Stores pre-calculated coefficients, delayed command history (``dlCmd``), and delayed residual history (``dlRes``) for up to 8192 actuators.

Constants:

- ``RES_MAX_VAL`` = 8192 — maximum actuator/subaperture count
- ``FILTER_ORDER`` = 3 — IIR filter order


daoProfile (C++ Header-Only)
-----------------------------

``daoProfile.hpp`` provides zero-overhead compile-time instrumentation macros. When ``DAO_PROFILE_ENABLED`` is **not** defined, all macros expand to empty expressions with no runtime cost.

Enabling Profiling
~~~~~~~~~~~~~~~~~~

Define the flag before including the header, or pass it on the compiler command line:

.. code-block:: cpp

    #define DAO_PROFILE_ENABLED
    #include <daoProfile.hpp>

Or via compiler flag:

.. code-block:: bash

    g++ myfile.cpp -DDAO_PROFILE_ENABLED

Usage
~~~~~

.. code-block:: cpp

    #define DAO_PROFILE_ENABLED
    #include <daoProfile.hpp>

    int main() {
        // Declare profiler with named blocks and resolution
        DAO_PROFILE(prof, std::chrono::nanoseconds, "acquire", "process")

        for (int i = 0; i < 1000; ++i) {
            DAO_PROFILE_NEW_FRAME(prof)

            DAO_PROFILE_START(prof, "acquire")
            // ... acquire frame from SHM ...
            DAO_PROFILE_STOP(prof, "acquire")

            DAO_PROFILE_START(prof, "process")
            // ... run MVM ...
            DAO_PROFILE_STOP(prof, "process")
        }

        // Export results to CSV
        DAO_PROFILE_EXPORT(prof)
    }

Macros
~~~~~~

.. list-table::
   :widths: 35 65
   :header-rows: 1

   * - Macro
     - Description

   * - ``DAO_PROFILE(name, resolution, ...)``
     - Declares a profiler instance ``name`` with timing resolution (e.g. ``std::chrono::nanoseconds``) and one or more named block strings.

   * - ``DAO_PROFILE_NEW_FRAME(name)``
     - Marks the start of a new loop iteration / frame.

   * - ``DAO_PROFILE_START(name, block)``
     - Starts the timer for block ``block`` within profiler ``name``.

   * - ``DAO_PROFILE_STOP(name, block)``
     - Stops the timer for block ``block`` and records the elapsed duration.

   * - ``DAO_PROFILE_EXPORT(name)``
     - Writes per-block statistics (min, max, mean, per-frame timings) to a CSV file.

The exported CSV file is named after the profiler and contains one column per named block, one row per frame, enabling post-hoc latency analysis in Python or MATLAB.
