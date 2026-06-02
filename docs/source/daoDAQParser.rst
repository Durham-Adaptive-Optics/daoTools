daoDAQ Parser
=============

The ``daoDAQParser`` is a Python library for parsing and analyzing FITS output files created by the daoDAQ acquisition tool. It provides a convenient interface to read session directories, extract image data, and access metadata from multiple acquisition resources.

Installation
------------

The parser requires Python 3.7+ and the following dependencies:

.. code-block:: bash

    pip install astropy numpy

The parser is included with daoTools at ``/opt/dao/daoTools/apps/daoDAQParser.py``.


Understanding daoDAQ Output Structure
--------------------------------------

Directory Layout
~~~~~~~~~~~~~~~~

daoDAQ organizes output in timestamped session directories:

.. code-block:: text

    root_storage/
    ├── 2026-05-26_10-30-00/          # Session directory (YYYY-MM-DD_HH-MM-SS)
    │   ├── resource1.fits            # Single FITS file (no rollover)
    │   ├── resource2/                # Directory for resource with file rollover
    │   │   ├── resource2_0.fits
    │   │   ├── resource2_1.fits
    │   │   └── resource2_2.fits
    │   └── resource3.fits
    ├── 2026-05-26_14-15-30/          # Another session
    │   └── ...
    └── ...

FITS File Format
~~~~~~~~~~~~~~~~

Each FITS file contains multiple HDUs (Header Data Units), one per acquired sample/frame:

- **Image Data**: Multi-dimensional arrays (2D or 3D)
- **Metadata Keywords**:

  - ``ATYPE``: Data type code
  - ``ATIME``: Timestamp in nanoseconds (int64)
  - ``CNT0``, ``CNT1``, ``CNT2``: Counter values (uint64)
  - Standard FITS keywords: ``NAXIS``, ``NAXISn``, etc.


Quick Start
-----------

Command Line Usage
~~~~~~~~~~~~~~~~~~

.. code-block:: bash

    # List all sessions
    python daoDAQParser.py /path/to/root_storage --list

    # Parse and display latest session
    python daoDAQParser.py /path/to/root_storage --latest

    # Parse specific session
    python daoDAQParser.py /path/to/root_storage --session 2026-05-26_10-30-00

    # Show detailed info for a specific resource
    python daoDAQParser.py /path/to/root_storage --latest --resource resource1

Python API
~~~~~~~~~~

**Method 1: Parse from root storage (for multiple sessions)**

.. code-block:: python

    from daoDAQParser import DAODAQParser

    # Initialize parser with root storage path
    parser = DAODAQParser('/path/to/root_storage')

    # Parse latest session
    session = parser.parse_latest_session()

    # Access resources
    for name, resource in session.resources.items():
        print(f"Resource: {name}")
        print(f"  Samples: {resource.num_samples}")
        print(f"  Shape: {resource.shape}")

**Method 2: Parse a specific directory directly (recommended for single sessions)**

.. code-block:: python

    from daoDAQParser import DAODAQParser

    # Parse any directory containing FITS files without root storage
    session = DAODAQParser.parse_directory('/path/to/2026-05-26_10-30-00')

    # Or parse a custom output directory
    session = DAODAQParser.parse_directory('/custom/output/directory')

    for name, resource in session.resources.items():
        print(f"{name}: {resource.num_samples} samples")


Data Structures
---------------

Session
~~~~~~~

Represents a complete acquisition session.

.. code-block:: python

    session.timestamp          # Session timestamp string (e.g., "2026-05-26_10-30-00")
    session.session_dir        # Absolute path to session directory
    session.resources          # Dict of Resource objects
    session.num_resources      # Number of resources
    session.resource_names     # List of resource names

Resource
~~~~~~~~

Represents a single data acquisition resource (e.g., camera, sensor).

.. code-block:: python

    resource.name              # Resource name
    resource.samples           # List of Sample objects
    resource.num_samples       # Total number of samples
    resource.shape             # Shape of each sample (e.g., (256, 256))
    resource.dtype             # NumPy data type
    resource.file_paths        # List of FITS file paths

    # Methods
    resource.get_all_data()         # Get all samples as array (n_samples, *shape)
    resource.get_all_metadata()     # Get list of all metadata
    resource.get_timestamps()       # Get array of timestamps
    resource.get_counters()         # Get dict of counter arrays

Sample
~~~~~~

Represents a single acquired frame/sample.

.. code-block:: python

    sample.data               # NumPy array with image data
    sample.metadata           # SampleMetadata object
    sample.file_path          # Path to FITS file containing this sample

SampleMetadata
~~~~~~~~~~~~~~

Metadata for a single sample.

.. code-block:: python

    metadata.atype            # Data type code
    metadata.atime            # Timestamp (nanoseconds)
    metadata.cnt0             # Counter 0
    metadata.cnt1             # Counter 1
    metadata.cnt2             # Counter 2
    metadata.hdu_index        # HDU index in FITS file


Usage Examples
--------------

Basic Parsing
~~~~~~~~~~~~~

.. code-block:: python

    from daoDAQParser import DAODAQParser

    # Parse a specific session directory
    session = DAODAQParser.parse_directory('/path/to/2026-05-26_10-30-00')

    print(f"Session: {session.timestamp}")
    print(f"Resources: {session.resource_names}")

    for name, resource in session.resources.items():
        print(f"\n{name}:")
        print(f"  Samples: {resource.num_samples}")
        print(f"  Shape: {resource.shape}")
        print(f"  Dtype: {resource.dtype}")

Accessing Image Data
~~~~~~~~~~~~~~~~~~~~

.. code-block:: python

    # Get a specific resource
    resource = session.resources['camera1']

    # Access individual samples
    first_sample = resource.samples[0]
    print(f"First frame shape: {first_sample.data.shape}")
    print(f"First frame mean: {first_sample.data.mean()}")

    # Get all data as a single array
    all_data = resource.get_all_data()  # Shape: (n_samples, height, width)

    # Compute mean image
    mean_image = all_data.mean(axis=0)

    # Compute standard deviation image
    std_image = all_data.std(axis=0)

Timing Analysis
~~~~~~~~~~~~~~~

.. code-block:: python

    import numpy as np

    # Get timestamps
    timestamps = resource.get_timestamps()

    # Calculate frame intervals
    intervals = np.diff(timestamps)

    print(f"Mean interval: {intervals.mean() / 1e6:.2f} ms")
    print(f"Frame rate: {1e9 / intervals.mean():.2f} Hz")
    print(f"Jitter (std): {intervals.std() / 1e6:.3f} ms")

Counter Analysis
~~~~~~~~~~~~~~~~

.. code-block:: python

    # Get counter values
    counters = resource.get_counters()

    # Check if counters are monotonic
    cnt0 = counters['cnt0']
    if np.all(np.diff(cnt0) > 0):
        print("Counter cnt0 is monotonically increasing")

    # Detect gaps or rollovers
    gaps = np.where(np.diff(cnt0) != 1)[0]
    if len(gaps) > 0:
        print(f"Found {len(gaps)} gaps in counter sequence")

Filtering Samples
~~~~~~~~~~~~~~~~~

.. code-block:: python

    # Filter samples by timestamp
    timestamps = resource.get_timestamps()
    start_time = timestamps[0] + 1e9  # 1 second after start
    end_time = start_time + 5e9        # 5 second window

    filtered = [s for s in resource.samples 
                if start_time <= s.metadata.atime <= end_time]

    print(f"Found {len(filtered)} samples in time window")

    # Extract data from filtered samples
    filtered_data = np.array([s.data for s in filtered])

Multi-Resource Synchronization
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: python

    # Parse session with multiple resources
    session = DAODAQParser.parse_directory('/path/to/session')

    if len(session.resources) >= 2:
        res1 = session.resources[session.resource_names[0]]
        res2 = session.resources[session.resource_names[1]]
        
        ts1 = res1.get_timestamps()
        ts2 = res2.get_timestamps()
        
        # Find overlapping time range
        overlap_start = max(ts1[0], ts2[0])
        overlap_end = min(ts1[-1], ts2[-1])
        
        print(f"Time overlap: {(overlap_end - overlap_start) / 1e9:.2f} seconds")

Export to Dictionary
~~~~~~~~~~~~~~~~~~~~

.. code-block:: python

    # Export with data (for serialization)
    parser = DAODAQParser('/root/storage')
    session_dict = parser.to_dict(session, include_data=True)

    # Export metadata only (smaller, for logging)
    metadata_dict = parser.to_dict(session, include_data=False)

    # Can be saved as JSON (after converting arrays)
    import json
    json.dump(metadata_dict, open('session_metadata.json', 'w'), indent=2)

Parse All Sessions
~~~~~~~~~~~~~~~~~~

.. code-block:: python

    # Initialize with root storage
    parser = DAODAQParser('/path/to/root_storage')

    # Find all available sessions
    session_list = parser.find_sessions()
    print(f"Found {len(session_list)} sessions")

    # Parse all sessions
    all_sessions = parser.parse_all_sessions()

    # Aggregate statistics
    total_samples = sum(
        resource.num_samples 
        for session in all_sessions.values() 
        for resource in session.resources.values()
    )
    print(f"Total samples across all sessions: {total_samples}")


Advanced Usage
--------------

Memory-Efficient Processing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For large datasets, avoid loading all data at once:

.. code-block:: python

    # Process samples one at a time
    resource = session.resources['camera1']

    running_sum = None
    count = 0

    for sample in resource.samples:
        if running_sum is None:
            running_sum = sample.data.astype(float)
        else:
            running_sum += sample.data
        count += 1

    mean_image = running_sum / count

Custom Processing Pipeline
~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: python

    class DAQProcessor:
        def __init__(self, root_storage):
            self.parser = DAODAQParser(root_storage)
        
        def process_session(self, session_timestamp):
            session = self.parser.parse_session(
                self.parser.root_storage / session_timestamp
            )
            
            results = {}
            for name, resource in session.resources.items():
                results[name] = self.process_resource(resource)
            
            return results
        
        def process_resource(self, resource):
            data = resource.get_all_data()
            
            # Your custom processing here
            processed = {
                'mean': data.mean(axis=0),
                'std': data.std(axis=0),
                'min': data.min(axis=0),
                'max': data.max(axis=0),
                'timestamps': resource.get_timestamps(),
                'counters': resource.get_counters(),
            }
            
            return processed

    # Use it
    processor = DAQProcessor('/path/to/root_storage')
    results = processor.process_session('2026-05-26_10-30-00')

Plotting Data
~~~~~~~~~~~~~

.. code-block:: python

    import matplotlib.pyplot as plt

    resource = session.resources['camera1']

    # Create figure with subplots
    fig, axes = plt.subplots(2, 2, figsize=(12, 10))

    # Plot first frame
    axes[0, 0].imshow(resource.samples[0].data, cmap='gray')
    axes[0, 0].set_title('First Frame')
    axes[0, 0].colorbar()

    # Plot mean image
    mean_img = resource.get_all_data().mean(axis=0)
    axes[0, 1].imshow(mean_img, cmap='gray')
    axes[0, 1].set_title('Mean Image')

    # Plot temporal profile
    timestamps = resource.get_timestamps()
    mean_values = [s.data.mean() for s in resource.samples]
    axes[1, 0].plot((timestamps - timestamps[0]) / 1e9, mean_values)
    axes[1, 0].set_xlabel('Time (s)')
    axes[1, 0].set_ylabel('Mean Intensity')
    axes[1, 0].set_title('Temporal Profile')

    # Plot counter values
    counters = resource.get_counters()
    axes[1, 1].plot(counters['cnt0'], label='cnt0')
    axes[1, 1].plot(counters['cnt1'], label='cnt1')
    axes[1, 1].set_xlabel('Sample Index')
    axes[1, 1].set_ylabel('Counter Value')
    axes[1, 1].set_title('Counters')
    axes[1, 1].legend()

    plt.tight_layout()
    plt.savefig('analysis.png', dpi=150)


Error Handling
--------------

The parser includes error handling for common issues:

.. code-block:: python

    try:
        session = DAODAQParser.parse_directory('/path/to/session')
    except FileNotFoundError:
        print("Session directory not found")
    except Exception as e:
        print(f"Error parsing session: {e}")


Troubleshooting
---------------

No sessions found
~~~~~~~~~~~~~~~~~

- Check that ``root_storage`` path is correct
- Verify session directories follow the naming pattern: ``YYYY-MM-DD_HH-MM-SS``

Empty resource
~~~~~~~~~~~~~~

- Check that FITS files exist in the session directory
- Verify FITS files contain image HDUs (not just primary HDU)

Metadata missing
~~~~~~~~~~~~~~~~

- Some keywords may be optional; use ``.get()`` with defaults
- Check FITS file headers with ``astropy.io.fits.info()``


API Reference
-------------

DAODAQParser Class
~~~~~~~~~~~~~~~~~~

.. code-block:: python

    class DAODAQParser:
        """Parser for daoDAQ output files."""
        
        def __init__(self, root_storage: Union[str, Path])
            """Initialize parser with root storage directory."""
        
        @staticmethod
        def parse_directory(directory_path: Union[str, Path]) -> Session
            """Parse a specific directory directly without root_storage."""
        
        def parse_fits_file(self, fits_path: Union[str, Path]) -> List[Sample]
            """Parse a single FITS file containing daoDAQ data."""
        
        def parse_resource(self, resource_path: Union[str, Path]) -> Resource
            """Parse a single resource (FITS file or directory with rollover files)."""
        
        def parse_session(self, session_dir: Union[str, Path]) -> Session
            """Parse a complete daoDAQ session directory."""
        
        def find_sessions(self) -> List[str]
            """Find all session directories in root storage."""
        
        def parse_all_sessions(self) -> Dict[str, Session]
            """Parse all sessions in root storage."""
        
        def parse_latest_session(self) -> Optional[Session]
            """Parse the most recent session."""
        
        def to_dict(self, session: Session, include_data: bool = True) -> Dict[str, Any]
            """Convert a Session object to a dictionary."""


See Also
--------

- :doc:`daoDAQ` - daoDAQ acquisition tool documentation
- `Astropy FITS documentation <https://docs.astropy.org/en/stable/io/fits/>`_
- `NumPy documentation <https://numpy.org/doc/>`_
