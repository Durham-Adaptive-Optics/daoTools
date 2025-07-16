# Quick Start Guide - DAO Shared Memory Web Viewer

## Getting Started in 3 Steps

### Step 1: Install Dependencies
```bash
cd /Users/davidbarr/Documents/dao/daoTools/gui/daoWebServer
pip install -r requirements.txt
```

### Step 2: Create Test Data (Optional)
```bash
python test.py create
```

### Step 3: Launch the Web Viewer
```bash
python launch.py
```
OR
```bash
python app.py
```

The web interface will open automatically at: http://localhost:5000

## What You'll See

1. **Left Panel**: List of shared memory files (`.im.shm`)
2. **Right Panel**: Interactive visualization
3. **Bottom Tabs**: Metadata, Recording, Load, Snapshot controls

## Basic Usage

1. **Select a File**: Click on any file in the left panel
2. **View Data**: 
   - 1D data shows as line plots
   - 2D data shows as heatmaps  
   - 3D data shows as sliceable heatmaps
3. **Switch Views**: Click "Table View" to see raw data
4. **Create New Arrays**: Click "Create SHM" in the toolbar
5. **Modify Data**: Click "Set Data" to change values

## Test Commands

Create test arrays:
```bash
python test.py create
```

Create and start live updates:
```bash
python test.py update
```

Clean up test files:
```bash
python test.py cleanup
```

## Features Available

✅ **Real-time monitoring** - Live data updates  
✅ **Interactive plots** - Zoom, pan, hover  
✅ **Multiple data types** - 1D, 2D, 3D arrays  
✅ **Table view** - Raw data inspection  
✅ **File management** - Browse and select files  
✅ **Data creation** - Create new shared memory  
✅ **Data modification** - Set values, randomize  
✅ **Responsive design** - Works on mobile/tablet  

## Coming Soon

🔄 **File upload/download**  
🔄 **Recording functionality**  
🔄 **Snapshot operations**  
🔄 **Advanced plot options**  

## Troubleshooting

**No files showing?**
- Check if `/tmp/` contains `.im.shm` files
- Run `python test.py create` to make test files

**Can't connect to shared memory?**
- Ensure `dao` module is installed
- Check file permissions in `/tmp/`

**Web page not loading?**
- Try a different port: `python app.py` (uses port 5000)
- Check firewall settings

## Comparison with Desktop Version

| Feature | Desktop (Qt) | Web Version |
|---------|-------------|-------------|
| Real-time updates | ✅ | ✅ |
| Multiple data types | ✅ | ✅ |
| Interactive plots | ✅ | ✅ |
| File operations | ✅ | 🔄 |
| Recording | ✅ | 🔄 |
| Cross-platform | ⚠️ Qt required | ✅ Browser only |
| Remote access | ❌ | ✅ |
| Mobile support | ❌ | ✅ |

## Network Access

To access from other devices on your network:

1. Find your IP address:
   ```bash
   ifconfig | grep inet
   ```

2. The web viewer is accessible at:
   ```
   http://YOUR_IP_ADDRESS:5000
   ```

**Security Note**: The server accepts connections from any IP. Use firewall rules in production environments.
