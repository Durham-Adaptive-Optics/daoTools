# DAO Shared Memory Web Viewer

A web-based version of the DAO Shared Memory Viewer that allows you to monitor and interact with shared memory arrays through a browser interface.

## Features

- **Real-time Monitoring**: Live updates of shared memory data with WebSocket connections
- **Interactive Visualization**: 
  - 1D data: Line plots with markers
  - 2D data: Heatmaps with color scales
  - 3D data: Slice-based viewing with interactive controls
- **Dual View Modes**: Switch between plot and table views
- **File Management**: 
  - Browse available shared memory files
  - Multi-file selection for batch operations
  - Real-time file list updates
- **Data Manipulation**:
  - Create new shared memory arrays
  - Set data values (constant or random)
  - Load data from files (future feature)
- **Responsive Design**: Works on desktop and mobile devices
- **Real-time Metadata**: Live statistics and file information

## Installation

1. Navigate to the web server directory:
   ```bash
   cd /Users/davidbarr/Documents/dao/daoTools/gui/daoWebServer
   ```

2. Install the required Python packages:
   ```bash
   pip install -r requirements.txt
   ```

3. Make sure the `dao` module is available in your Python path.

## Usage

1. Start the web server:
   ```bash
   python app.py
   ```

2. Open your web browser and navigate to:
   ```
   http://localhost:5000
   ```

3. The interface will show:
   - **Left Panel**: List of available shared memory files
   - **Right Panel**: Visualization area and control tabs

## Interface Overview

### File List Panel
- Shows all `.im.shm` files in `/tmp/`
- Search/filter functionality
- Checkbox selection for multi-file operations
- Real-time status indicators

### Visualization Panel
- **Plot View**: Interactive charts using Plotly.js
- **Table View**: Editable data tables
- **3D Controls**: Slice selector for 3D arrays
- **Auto-refresh**: Real-time data updates

### Control Tabs

#### Metadata Tab
- File information (shape, dtype, size)
- Real-time statistics (min, max, mean, std)
- Update frequency monitoring

#### Recording Tab
- Set output filename
- Configure frame count
- Start recording (future feature)

#### Load Tab
- Upload files to shared memory
- Support for .npy and .fits files (future feature)

#### Snapshot Tab
- Save/load system snapshots (future feature)

## Technical Details

### Architecture
- **Backend**: Flask with Socket.IO for real-time communication
- **Frontend**: Bootstrap 5 + Plotly.js for visualization
- **Real-time Updates**: WebSocket connection for live data streaming
- **Data Handling**: Efficient numpy array serialization

### API Endpoints
- `GET /api/files` - List shared memory files
- `POST /api/connect` - Connect to a shared memory file
- `POST /api/create_shm` - Create new shared memory array
- `POST /api/set_data` - Set data values
- `GET /api/metadata/<filename>` - Get file metadata

### WebSocket Events
- `data_update` - Real-time data updates
- `subscribe` - Subscribe to file updates
- `connect/disconnect` - Connection status

## Comparison with Qt Version

### Advantages of Web Version:
- **Cross-platform**: Works on any device with a browser
- **Remote Access**: Monitor from anywhere on the network
- **No Installation**: No Qt dependencies required
- **Modern UI**: Responsive, mobile-friendly interface
- **Easy Deployment**: Can be deployed on servers

### Current Limitations:
- File upload/download not yet implemented
- Recording functionality not yet implemented
- Snapshot operations not yet implemented
- Some advanced Qt-specific features missing

## Future Enhancements

1. **File Operations**:
   - File upload for loading data
   - File download for saving data
   - Drag-and-drop support

2. **Recording System**:
   - Multi-frame recording
   - Progress indicators
   - Background recording

3. **Advanced Features**:
   - Custom plot configurations
   - Data export options
   - User preferences
   - Multiple simultaneous connections

4. **Performance**:
   - Data compression for large arrays
   - Selective updates
   - Caching mechanisms

## Security Considerations

- The web server runs on all interfaces (0.0.0.0) for network access
- Consider firewall rules for production deployment
- No authentication is currently implemented

## Troubleshooting

### Common Issues:

1. **"No shared memory files found"**:
   - Check if `/tmp/` contains `.im.shm` files
   - Verify DAO system is running

2. **Connection errors**:
   - Ensure the `dao` module is installed
   - Check file permissions on shared memory files

3. **Visualization not updating**:
   - Check browser console for JavaScript errors
   - Verify WebSocket connection status

4. **Performance issues**:
   - Large arrays are automatically downsampled
   - Consider reducing update frequency for very large datasets

## Development

To modify the web viewer:

1. **Backend Changes**: Edit `app.py`
2. **Frontend Styling**: Edit `static/css/style.css`
3. **Frontend Logic**: Edit `static/js/app.js`
4. **Templates**: Edit `templates/index.html`

The application uses hot reloading in debug mode for development.
