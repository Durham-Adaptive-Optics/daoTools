# DAO Shared Memory Web Viewer Configuration

# Server settings
HOST = '0.0.0.0'  # Listen on all interfaces (use '127.0.0.1' for localhost only)
PORT = 5000       # Default port
DEBUG = True      # Enable debug mode

# Shared memory settings
SHM_DIRECTORY = '/tmp'           # Directory to monitor for .shm files
SHM_EXTENSION = '.im.shm'        # File extension for shared memory files
UPDATE_FREQUENCY = 0.1           # Update frequency in seconds (10 Hz)

# Data handling
MAX_DATA_SIZE = 10000           # Maximum data size for full transmission
DOWNSAMPLE_FACTOR_2D = 100      # Downsample factor for 2D arrays
DOWNSAMPLE_FACTOR_1D = 1000     # Downsample factor for 1D arrays

# UI settings
DEFAULT_VIEW = 'plot'           # Default view mode: 'plot' or 'table'
AUTO_REFRESH_FILES = True       # Auto-refresh file list
SHOW_ADVANCED_FEATURES = False  # Show advanced features (recording, snapshots)

# Logging
LOG_LEVEL = 'INFO'              # Logging level: DEBUG, INFO, WARNING, ERROR

# Security (for future implementation)
ENABLE_AUTH = False             # Enable authentication
SECRET_KEY = 'dao_shm_viewer_secret_key'  # Session secret key
