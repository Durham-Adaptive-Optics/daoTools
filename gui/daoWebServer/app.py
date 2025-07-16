#!/usr/bin/env python3

import os
import sys
import json
import numpy as np
from flask import Flask, render_template, jsonify, request, send_from_directory
from flask_socketio import SocketIO, emit
import threading
import time
import subprocess
from astropy.io import fits
import dao
import gc
from datetime import datetime

import eventlet
import eventlet.wsgi
app = Flask(__name__)
app.config['SECRET_KEY'] = 'dao_shm_viewer_secret_key'
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="eventlet")

class ShmManager:
    """Manages shared memory connections and data streaming."""
    
    def __init__(self):
        self.connections = {}  # filename -> shm object
        self.counters = {}     # filename -> last counter
        self.update_thread = None
        self.update_running = False
        self.clients = set()   # Set of connected clients
        
    def connect_shm(self, filename):
        """Connect to a shared memory file."""
        try:
            if filename in self.connections:
                self.connections[filename].close()
                
            shm_path = f"/tmp/{filename}"
            shm = dao.shm(shm_path, logLevel=0)
            self.connections[filename] = shm
            self.counters[filename] = shm.get_counter()
            
            return {
                'success': True,
                'shape': shm.get_data().shape,
                'dtype': str(shm.get_data().dtype),
                'data': self._serialize_data(shm.get_data())
            }
        except Exception as e:
            return {'success': False, 'error': str(e)}
    
    def disconnect_shm(self, filename):
        """Disconnect from a shared memory file."""
        if filename in self.connections:
            self.connections[filename].close()
            del self.connections[filename]
            del self.counters[filename]
    
    def disconnect_all_except(self, keep_filename):
        """Disconnect from all shared memory files except the specified one."""
        to_disconnect = [f for f in self.connections.keys() if f != keep_filename]
        for filename in to_disconnect:
            self.connections[filename].close()
            del self.connections[filename]
            if filename in self.counters:
                del self.counters[filename]
        return len(to_disconnect)
    
    def get_file_list(self):
        """Get list of shared memory files."""
        try:
            files = []
            for file in os.listdir('/tmp'):
                if file.endswith('.im.shm'):
                    files.append(file)
            return sorted(files)
        except Exception:
            return []
    
    def get_metadata(self, filename):
        """Get metadata for a shared memory file."""
        if filename not in self.connections:
            return None
            
        try:
            shm = self.connections[filename]
            data = shm.get_data()
            
            metadata = {
                'filename': filename,
                'shape': data.shape,
                'dtype': str(data.dtype),
                'counter': shm.get_counter(),
                'timestamp': datetime.now().isoformat()
            }
            
            # Add statistics for numerical data
            if np.issubdtype(data.dtype, np.number):
                metadata.update({
                    'min': float(np.min(data)),
                    'max': float(np.max(data)),
                    'mean': float(np.mean(data)),
                    'std': float(np.std(data))
                })
            
            return metadata
        except Exception as e:
            return {'error': str(e)}
    
    def _serialize_data(self, data):
        """Serialize numpy data for JSON transmission."""
        # Handle different array shapes for DAO compatibility
        original_shape = data.shape
        
        # Check if this is a 1D-like array (shape like (N, 1) or (1, N))
        is_1d_like = (len(original_shape) == 2 and 
                      (original_shape[0] == 1 or original_shape[1] == 1))
        
        if data.size > 10000:  # Limit data size for web transmission
            # For large arrays, send a downsampled version
            if len(data.shape) == 2 and not is_1d_like:
                # For 2D arrays, downsample
                factor = max(1, max(data.shape) // 100)
                downsampled = data[::factor, ::factor]
                return {
                    'data': downsampled.tolist(),
                    'downsampled': True,
                    'factor': factor,
                    'original_shape': original_shape,
                    'is_1d_like': False
                }
            elif is_1d_like:
                # For 1D-like arrays, flatten and downsample
                flattened = data.flatten()
                factor = max(1, flattened.shape[0] // 1000)
                downsampled = flattened[::factor]
                return {
                    'data': downsampled.tolist(),
                    'downsampled': True,
                    'factor': factor,
                    'original_shape': original_shape,
                    'is_1d_like': True
                }
            elif len(data.shape) == 3:
                # For 3D arrays, downsample each dimension
                factor = max(1, max(data.shape) // 50)
                downsampled = data[::factor, ::factor, ::factor]
                return {
                    'data': downsampled.tolist(),
                    'downsampled': True,
                    'factor': factor,
                    'original_shape': original_shape,
                    'is_1d_like': False
                }
        
        return {
            'data': data.tolist(),
            'downsampled': False,
            'original_shape': original_shape,
            'is_1d_like': is_1d_like
        }
    
    def start_updates(self):
        """Start the update thread for streaming data."""
        if not self.update_running:
            self.update_running = True
            self.update_thread = threading.Thread(target=self._update_loop)
            self.update_thread.daemon = True
            self.update_thread.start()
    
    def stop_updates(self):
        """Stop the update thread."""
        self.update_running = False
        if self.update_thread and self.update_thread != threading.current_thread():
            self.update_thread.join(timeout=1)
    
    def _update_loop(self):
        """Main update loop for streaming data to clients."""
        while self.update_running:
            try:
                if not self.connections:
                    time.sleep(0.1)
                    continue
                    
                for filename, shm in self.connections.items():
                    new_counter = shm.get_counter()
                    old_counter = self.counters.get(filename, 0)
                    
                    if new_counter != old_counter:
                        diff = new_counter - old_counter
                        self.counters[filename] = new_counter
                        
                        # Send updated data to clients
                        data_info = {
                            'filename': filename,
                            'counter': new_counter,
                            'frequency': 10.0 / diff if diff > 0 else 0,
                            'data': self._serialize_data(shm.get_data()),
                            'metadata': self.get_metadata(filename)
                        }
                        
                        # Emit to all connected clients
                        for client_id in self.clients:
                            socketio.emit('data_update', data_info, to=client_id)
                
                time.sleep(0.1)  # 10 Hz update rate
                
            except Exception as e:
                print(f"Error in update loop: {e}")
                time.sleep(1)

# Global shared memory manager
shm_manager = ShmManager()

@app.route('/')
def index():
    """Main page."""
    return render_template('index.html')

@app.route('/api/files')
def get_files():
    """Get list of shared memory files."""
    files = shm_manager.get_file_list()
    return jsonify({'files': files})

@app.route('/api/connect', methods=['POST'])
def connect_file():
    """Connect to a shared memory file."""
    data = request.get_json()
    filename = data.get('filename')
    
    if not filename:
        return jsonify({'success': False, 'error': 'No filename provided'})
    
    result = shm_manager.connect_shm(filename)
    return jsonify(result)

@app.route('/api/disconnect', methods=['POST'])
def disconnect_file():
    """Disconnect from a shared memory file."""
    data = request.get_json()
    filename = data.get('filename')
    
    if filename:
        shm_manager.disconnect_shm(filename)
    
    return jsonify({'success': True})

@app.route('/api/metadata/<filename>')
def get_metadata(filename):
    """Get metadata for a file."""
    metadata = shm_manager.get_metadata(filename)
    return jsonify(metadata or {})

@app.route('/api/create_shm', methods=['POST'])
def create_shm():
    """Create a new shared memory array."""
    data = request.get_json()
    name = data.get('name')
    shape_str = data.get('shape')
    dtype_str = data.get('dtype')
    
    try:
        shape = tuple(map(int, shape_str.split(',')))
        shape = tuple(dim for dim in shape if dim > 0)
        
        # DAO doesn't support 1D arrays, convert to 2D
        if len(shape) == 1:
            shape = (shape[0], 1)
        elif len(shape) == 0:
            shape = (1, 1)
        
        array = np.zeros(shape, dtype=dtype_str)
        filename = f'/tmp/{name}.im.shm'
        shm = dao.shm(filename, array, logLevel=0)
        
        return jsonify({
            'success': True,
            'message': f'Shared memory created with shape {shape} and dtype {dtype_str}'
        })
        
    except Exception as e:
        return jsonify({'success': False, 'error': str(e)})

@app.route('/api/set_data', methods=['POST'])
def set_data():
    """Set data in shared memory."""
    data = request.get_json()
    filename = data.get('filename')
    mode = data.get('mode')
    
    if filename not in shm_manager.connections:
        return jsonify({'success': False, 'error': 'File not connected'})
    
    try:
        shm = shm_manager.connections[filename]
        current_data = shm.get_data()
        
        if mode == 'set_value':
            value = data.get('value', 0)
            result = np.full_like(current_data, value)
        elif mode == 'randomize':
            min_val = data.get('min', 0)
            max_val = data.get('max', 1)
            
            if np.issubdtype(current_data.dtype, np.integer):
                result = np.random.randint(
                    low=int(min_val),
                    high=int(max_val) + 1,
                    size=current_data.shape,
                    dtype=current_data.dtype
                )
            else:
                result = np.random.uniform(
                    low=min_val,
                    high=max_val,
                    size=current_data.shape
                ).astype(current_data.dtype)
        else:
            return jsonify({'success': False, 'error': 'Invalid mode'})
        
        shm.set_data(result)
        return jsonify({'success': True})
        
    except Exception as e:
        return jsonify({'success': False, 'error': str(e)})

@app.route('/api/load_file', methods=['POST'])
def load_file():
    """Load data from file into shared memory."""
    data = request.get_json()
    filename = data.get('filename')
    filepath = data.get('filepath')
    
    if filename not in shm_manager.connections:
        return jsonify({'success': False, 'error': 'Shared memory not connected'})
    
    try:
        shm = shm_manager.connections[filename]
        
        if filepath.endswith('.npy'):
            file_data = np.load(filepath)
        elif filepath.endswith('.fits'):
            with fits.open(filepath) as hdul:
                shm_data = shm.get_data()
                file_data = None
                
                for hdu in hdul:
                    if (hdu.data is not None and 
                        hasattr(hdu.data, 'shape') and
                        hdu.data.shape == shm_data.shape):
                        file_data = hdu.data
                        break
                
                if file_data is None:
                    return jsonify({'success': False, 'error': 'No matching HDU found'})
        else:
            return jsonify({'success': False, 'error': 'Unsupported file format'})
        
        # Convert to shared memory dtype if needed
        if file_data.dtype != shm.get_data().dtype:
            file_data = file_data.astype(shm.get_data().dtype)
        
        shm.set_data(file_data)
        return jsonify({'success': True})
        
    except Exception as e:
        return jsonify({'success': False, 'error': str(e)})

@app.route('/api/debug/connections')
def debug_connections():
    """Debug endpoint to check current connections."""
    return jsonify({
        'connected_files': list(shm_manager.connections.keys()),
        'client_count': len(shm_manager.clients),
        'update_running': shm_manager.update_running,
        'counters': dict(shm_manager.counters)
    })

@app.route('/api/poll/<filename>')
def poll_data(filename):
    """Poll for updated data from a shared memory file."""
    last_counter = request.args.get('counter', type=int, default=0)
    
    if filename not in shm_manager.connections:
        return jsonify({'success': False, 'error': 'File not connected'})
    
    try:
        shm = shm_manager.connections[filename]
        current_counter = shm.get_counter()
        
        # Only return data if counter has changed
        if current_counter != last_counter:
            data_info = {
                'success': True,
                'filename': filename,
                'counter': current_counter,
                'data': shm_manager._serialize_data(shm.get_data()),
                'metadata': shm_manager.get_metadata(filename),
                'updated': True
            }
        else:
            # No update, just return current counter
            data_info = {
                'success': True,
                'filename': filename,
                'counter': current_counter,
                'updated': False
            }
            
        return jsonify(data_info)
        
    except Exception as e:
        return jsonify({'success': False, 'error': str(e)})

@socketio.on('connect')
def handle_connect():
    """Handle client connection."""
    shm_manager.clients.add(request.sid)
    if not shm_manager.update_running:
        shm_manager.start_updates()

@socketio.on('disconnect')
def handle_disconnect():
    """Handle client disconnection."""
    shm_manager.clients.discard(request.sid)

@socketio.on('subscribe')
def handle_subscribe(data):
    """Handle subscription to a specific shared memory file."""
    filename = data.get('filename')
    client_id = request.sid
    
    if filename:
        # Disconnect from all other files first to save resources
        shm_manager.disconnect_all_except(filename)
        
        # Ensure the file is connected
        if filename not in shm_manager.connections:
            result = shm_manager.connect_shm(filename)
            emit('subscription_result', result)
            if not result.get('success'):
                return
        else:
            emit('subscription_result', {'success': True, 'message': 'Already connected'})
        
        # Ensure update thread is running
        if not shm_manager.update_running:
            shm_manager.start_updates()
        
        # Send current data immediately
        try:
            shm = shm_manager.connections[filename]
            current_counter = shm.get_counter()
            
            data_info = {
                'filename': filename,
                'counter': current_counter,
                'frequency': 0,
                'data': shm_manager._serialize_data(shm.get_data()),
                'metadata': shm_manager.get_metadata(filename)
            }
            emit('data_update', data_info, to=client_id)
        except Exception as e:
            emit('subscription_result', {'success': False, 'error': str(e)})
    else:
        emit('subscription_result', {'success': False, 'error': 'No filename provided'})

if __name__ == '__main__':
    # Start the web server with eventlet for WebSocket support
    print("Starting DAO Shared Memory Web Viewer...")
    print("Access the viewer at: http://localhost:5000")
    socketio.run(app, host='0.0.0.0', port=5000, debug=True, use_reloader=False)
