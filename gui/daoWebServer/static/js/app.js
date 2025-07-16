// DAO Shared Memory Web Viewer - Main JavaScript Application

class DaoShmWebViewer {
    constructor() {
        this.socket = null; // Keep for compatibility but not used
        this.currentFile = null;
        this.currentData = null;
        this.selectedFiles = new Set();
        this.currentView = 'plot'; // 'plot' or 'table'
        this.currentSlice = 0;
        this.is3D = false;
        this.isTableView = false;
        this.updateCount = 0; // Track number of real-time updates
        this.renderPending = false; // Flag to prevent render spam
        this.pollingInterval = null; // For HTTP polling
        this.lastCounter = 0; // Track last counter for polling
        
        this.init();
    }
    
    init() {
        // Using HTTP polling instead of WebSockets
        this.updateConnectionStatus(true);
        this.bindEvents();
        this.loadFiles();
        this.initSocket(); // Initialize socket for recording completion events
    }
    
    initSocket() {
        // Initialize socket connection for data updates only
        this.socket = io();
        // WebSocket recording events removed - using HTTP polling instead
    }
    
    handleRecordingComplete(data) {
        if (data.success) {
            // Show completion message with download link
            const downloadUrl = `/api/download/${data.metadata.output_filename}`;
            const downloadMessage = `
                <div>
                    <p>Recording automatically completed (frame limit reached)</p>
                    <p>${data.message}</p>
                    <p>Frames recorded: ${data.frames_recorded}</p>
                    <a href="${downloadUrl}" class="btn btn-primary btn-sm" download>
                        <i class="fas fa-download"></i> Download ${data.metadata.output_filename}
                    </a>
                </div>
            `;
            
            this.showCustomAlert('Recording Complete', downloadMessage, 'success');
            
            // Update UI to reflect recording stopped
            this.updateRecordingUI({
                recording: false,
                shm_filename: null,
                output_file: null,
                frame_count: 0,
                max_frames: null
            });
        } else {
            this.showAlert('Recording Error', data.error, 'danger');
        }
    }
    
    bindEvents() {
        // Refresh button
        document.getElementById('refreshBtn').addEventListener('click', () => {
            this.loadFiles();
        });
        
        // Create SHM modal
        document.getElementById('createShmConfirmBtn').addEventListener('click', () => {
            this.createSharedMemory();
        });
        
        // Set data modal
        document.getElementById('setDataConfirmBtn').addEventListener('click', () => {
            this.setData();
        });
        
        // Toggle view button
        document.getElementById('toggleViewBtn').addEventListener('click', () => {
            this.toggleView();
        });
        
        // Clear selected files
        document.getElementById('clearSelectedBtn').addEventListener('click', () => {
            this.clearSelectedFiles();
        });
        
        // File filter
        document.getElementById('fileFilter').addEventListener('input', (e) => {
            this.filterFiles(e.target.value);
        });
        
        // Slice selector
        document.getElementById('sliceSelector').addEventListener('input', (e) => {
            this.updateSlice(parseInt(e.target.value));
        });
        
        // Recording
        document.getElementById('recordBtn').addEventListener('click', () => {
            this.recordData();
        });
        
        // Load file
        document.getElementById('loadBtn').addEventListener('click', () => {
            this.loadFile();
        });
        
        // Snapshot operations
        document.getElementById('saveSnapshotBtn').addEventListener('click', () => {
            this.saveSnapshot();
        });
        
        document.getElementById('loadSnapshotBtn').addEventListener('click', () => {
            this.loadSnapshot();
        });
    }
    
    async loadFiles() {
        try {
            const response = await fetch('/api/files');
            const data = await response.json();
            this.renderFileList(data.files);
        } catch (error) {
            console.error('Error loading files:', error);
            this.showAlert('Error', 'Failed to load files', 'danger');
        }
    }
    
    renderFileList(files) {
        const fileList = document.getElementById('fileList');
        fileList.innerHTML = '';
        
        if (files.length === 0) {
            fileList.innerHTML = '<div class="list-group-item text-muted">No shared memory files found</div>';
            return;
        }
        
        files.forEach(file => {
            const item = document.createElement('div');
            item.className = 'list-group-item';
            item.innerHTML = `
                <div class="file-item">
                    <div class="d-flex align-items-center">
                        <input type="checkbox" class="form-check-input file-checkbox" 
                               data-file="${file}" ${this.selectedFiles.has(file) ? 'checked' : ''}>
                        <span class="file-name text-truncate" title="${file}">${file}</span>
                    </div>
                    <span class="status-indicator offline" id="status-${file}"></span>
                </div>
            `;
            
            // Add click handler for file selection
            item.addEventListener('click', (e) => {
                if (e.target.type !== 'checkbox') {
                    this.selectFile(file);
                }
            });
            
            // Add checkbox handler
            const checkbox = item.querySelector('.file-checkbox');
            checkbox.addEventListener('change', (e) => {
                e.stopPropagation();
                this.toggleFileSelection(file, e.target.checked);
            });
            
            fileList.appendChild(item);
        });
    }
    
    async selectFile(filename) {
        // Update UI to show selected file
        document.querySelectorAll('.list-group-item').forEach(item => {
            item.classList.remove('active');
        });
        
        event.currentTarget.classList.add('active');
        
        // Connect to shared memory
        try {
            const response = await fetch('/api/connect', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify({ filename })
            });
            
            const result = await response.json();
            
            if (result.success) {
                // Only reset update count if we're switching to a different file
                const switchingFiles = this.currentFile !== filename;
                
                this.currentFile = filename;
                this.currentData = result; // Store current data for view toggles
                
                if (switchingFiles) {
                    this.updateCount = 0; // Only reset when switching files
                } else {
                    // Keep existing update count
                }
                
                this.updateVisualization(result);
                this.loadMetadata(filename);
                
                // Update recording filename suggestion and check recording status
                this.updateRecordingFilename(filename);
                this.checkRecordingStatus().catch(error => {
                    console.warn('Could not check recording status:', error);
                });
                this.updateRecordingFilename(filename);
                
                // Start polling for updates
                this.startPolling(filename);
                
                // Update status indicator
                document.getElementById(`status-${filename}`).className = 'status-indicator updating';
                
                // Show visualization container
                document.getElementById('noFileMessage').style.display = 'none';
                document.getElementById('visualizationContainer').style.display = 'block';
                
            } else {
                this.showAlert('Error', result.error, 'danger');
            }
        } catch (error) {
            console.error('Error connecting to file:', error);
            this.showAlert('Error', 'Failed to connect to file', 'danger');
        }
    }
    
    updateVisualization(data) {
        // Handle different data structures to get the shape
        let shape;
        if (data.success && data.data && data.data.original_shape) {
            // Initial connection format
            shape = data.shape || data.data.original_shape;
        } else if (data.filename && data.data && data.data.original_shape) {
            // Real-time update format
            shape = data.data.original_shape;
        } else if (data.original_shape) {
            // Direct format
            shape = data.original_shape;
        } else if (data.shape) {
            // Direct format
            shape = data.shape;
        } else {
            console.error('Cannot determine shape from data:', data);
            return;
        }
        console.log('[updateVisualization] called with data:', data);
        
        this.is3D = shape.length === 3;
        
        // Update slice controls for 3D data
        if (this.is3D) {
            document.getElementById('sliceControls').style.display = 'block';
            const sliceSelector = document.getElementById('sliceSelector');
            sliceSelector.max = shape[0] - 1;
            sliceSelector.value = this.currentSlice;
            document.getElementById('sliceValue').textContent = this.currentSlice;
        } else {
            document.getElementById('sliceControls').style.display = 'none';
        }
        
        // Render data based on current view mode
        if (this.isTableView) {
            this.renderTable(data);
        } else {
            this.renderPlot(data);
        }
    }
    
    renderPlot(data) {
        try {
            // Ensure Plotly is loaded
            if (typeof Plotly === 'undefined') {
                console.error('Plotly is not loaded!');
                this.showAlert('Plotly Error', 'Plotly library is not loaded.', 'danger');
                return;
            }

            // Show plot container and hide table
            const plotContainer = document.getElementById('plotContainer');
            const tableContainer = document.getElementById('tableContainer');
            if (!plotContainer) {
                console.error('plotContainer element not found!');
                this.showAlert('Plot Error', 'Plot container not found in DOM.', 'danger');
                return;
            }
            plotContainer.style.display = 'block';
            if (tableContainer) tableContainer.style.display = 'none';


            if (!data || !data.data) {
                console.error('No data provided to renderPlot');
                this.showAlert('Plot Error', 'No data provided to renderPlot', 'danger');
                return;
            }

            let plotData;
            let arrayData, shape, is1DLike;

            if (data.success && data.data && typeof data.data === 'object' && data.data.data) {
                arrayData = data.data.data;
                shape = data.shape || data.data.original_shape;
                is1DLike = data.data.is_1d_like || (shape.length === 2 && (shape[0] === 1 || shape[1] === 1));
            } else if (data.filename && data.data && typeof data.data === 'object' && data.data.data) {
                arrayData = data.data.data;
                shape = data.data.original_shape;
                is1DLike = data.data.is_1d_like || (shape.length === 2 && (shape[0] === 1 || shape[1] === 1));
            } else {
                console.error('Unrecognized data format:', data);
                this.showAlert('Plot Error', 'Unrecognized data format', 'danger');
                return;
            }

            // Debugging output
            console.log('Shape:', shape, 'is1DLike:', is1DLike, 'arrayData type:', typeof arrayData);
            // Sanitize data for NaN/undefined
            function sanitize(arr) {
                return arr.map(v => (typeof v === 'number' && !isNaN(v) ? v : 0));
            }

            if (is1DLike) {
                let flatData;
                if (Array.isArray(arrayData) && Array.isArray(arrayData[0])) {
                    flatData = sanitize(arrayData.flat());
                } else if (Array.isArray(arrayData)) {
                    flatData = sanitize(arrayData);
                } else {
                    flatData = sanitize([arrayData]);
                }
                if (!Array.isArray(flatData)) {
                    console.error('flatData is not an array:', flatData);
                    this.showAlert('Plot Error', 'Unable to convert data to array format for plotting', 'danger');
                    return;
                }
                console.log('Plotting 1D-like data, flatData length:', flatData.length);
                plotData = [{
                    y: flatData,
                    type: 'scatter',
                    mode: 'lines+markers',
                    name: this.currentFile,
                    line: { width: 2 },
                    marker: { size: 4 }
                }];
            } else if (shape.length === 2) {
                console.log('Plotting 2D heatmap');
                plotData = [{
                    z: arrayData.map(row => sanitize(row)),
                    type: 'heatmap',
                    colorscale: 'Viridis',
                    showscale: true
                }];
            } else if (shape.length === 3) {
                console.log('Plotting 3D slice:', this.currentSlice);
                const sliceData = arrayData[this.currentSlice] || arrayData[0];
                plotData = [{
                    z: Array.isArray(sliceData) ? sliceData.map(row => sanitize(row)) : [],
                    type: 'heatmap',
                    colorscale: 'Viridis',
                    showscale: true
                }];
            } else {
                console.error('Unsupported data shape:', shape);
                this.showAlert('Plot Error', 'Unsupported data shape', 'danger');
                return;
            }

            const layout = {
                title: `${this.currentFile} ${this.is3D ? `(Slice ${this.currentSlice})` : ''}`,
                responsive: true,
                margin: { t: 50, l: 50, r: 50, b: 50 }
            };
            if (is1DLike) {
                layout.xaxis = { title: 'Index' };
                layout.yaxis = { title: 'Value' };
            }
            Plotly.react('plotContainer', plotData, layout, { responsive: true });
        } catch (error) {
            console.error('Error in renderPlot:', error);
            this.showAlert('Plotting Error', `Failed to render plot: ${error.message}`, 'danger');
        }
    }
    
    renderTable(data) {
        document.getElementById('plotContainer').style.display = 'none';
        document.getElementById('tableContainer').style.display = 'block';
        
        if (!data || !data.data) {
            console.error('No data provided to renderTable');
            return;
        }
        
        try {
            const table = document.getElementById('dataTable');
            
            // Handle different data structures:
            // 1. From real-time updates: data = {filename: "...", data: {data: [...], original_shape: [...], is_1d_like: ...}, metadata: {...}}
            // 2. From initial connection: data = {success: true, data: {data: [...], original_shape: [...], is_1d_like: ...}, shape: [...]}
            let arrayData, shape, is1DLike;
            
            if (data.success && data.data && typeof data.data === 'object' && data.data.data) {
                // Initial connection format
                arrayData = data.data.data;
                shape = data.shape || data.data.original_shape;
                is1DLike = data.data.is_1d_like || (shape.length === 2 && (shape[0] === 1 || shape[1] === 1));
            } else if (data.filename && data.data && typeof data.data === 'object' && data.data.data) {
                // Real-time update format
                arrayData = data.data.data;
                shape = data.data.original_shape;
                is1DLike = data.data.is_1d_like || (shape.length === 2 && (shape[0] === 1 || shape[1] === 1));
            } else {
                console.error('Unrecognized data format:', data);
                throw new Error('Unrecognized data format');
            }
            
            console.log('renderTable - shape:', shape, 'is1DLike:', is1DLike);
            console.log('renderTable - arrayData type:', typeof arrayData, 'is array:', Array.isArray(arrayData));
            console.log('renderTable - arrayData sample:', arrayData);
            
            // Clear existing table
            table.innerHTML = '';
            
            if (is1DLike) {
                // 1D-like array - single column
                let flatData;
                if (Array.isArray(arrayData) && Array.isArray(arrayData[0])) {
                    // If it's a 2D array, flatten it
                    flatData = arrayData.flat();
                } else if (Array.isArray(arrayData)) {
                    flatData = arrayData;
                } else {
                    // If it's not an array, create one
                    flatData = [arrayData];
                }
                
                // Ensure flatData is an array
                if (!Array.isArray(flatData)) {
                    console.error('flatData is not an array:', flatData);
                    throw new Error('Unable to convert data to array format');
                }
                
                const thead = table.createTHead();
                const headerRow = thead.insertRow();
                headerRow.insertCell().textContent = 'Index';
                headerRow.insertCell().textContent = 'Value';
                
                const tbody = table.createTBody();
                flatData.forEach((value, index) => {
                    const row = tbody.insertRow();
                    row.insertCell().textContent = index;
                    row.insertCell().textContent = typeof value === 'number' ? value.toFixed(3) : value;
                });
            } else if (shape.length === 2) {
                // 2D array - full table
                console.log('Rendering 2D table, arrayData:', arrayData, 'is array:', Array.isArray(arrayData));
                
                // Ensure arrayData is an array
                if (!Array.isArray(arrayData)) {
                    console.error('arrayData is not an array for 2D data:', arrayData);
                    throw new Error('2D data is not in array format');
                }
                
                const thead = table.createTHead();
                const headerRow = thead.insertRow();
                headerRow.insertCell().textContent = '';
                for (let j = 0; j < shape[1]; j++) {
                    headerRow.insertCell().textContent = j;
                }
                
                const tbody = table.createTBody();
                arrayData.forEach((row, i) => {
                    if (!Array.isArray(row)) {
                        console.error('Row is not an array:', row);
                        return;
                    }
                    const tableRow = tbody.insertRow();
                    tableRow.insertCell().textContent = i;
                    row.forEach(value => {
                        const cell = tableRow.insertCell();
                        cell.textContent = typeof value === 'number' ? value.toFixed(3) : value;
                    });
                });
            } else if (shape.length === 3) {
                // 3D array - show current slice
                console.log('Rendering 3D table, arrayData:', arrayData, 'currentSlice:', this.currentSlice);
                console.log('arrayData type:', typeof arrayData, 'is array:', Array.isArray(arrayData));
                
                // Ensure arrayData is an array and has the slice
                if (!Array.isArray(arrayData)) {
                    console.error('arrayData is not an array for 3D data:', arrayData);
                    throw new Error('3D data is not in array format');
                }
                
                if (this.currentSlice >= arrayData.length) {
                    console.error('currentSlice out of bounds:', this.currentSlice, 'arrayData length:', arrayData.length);
                    this.currentSlice = 0;
                }
                
                const sliceData = arrayData[this.currentSlice];
                console.log('sliceData:', sliceData, 'is array:', Array.isArray(sliceData));
                
                if (!sliceData || !Array.isArray(sliceData)) {
                    console.error('sliceData is not valid:', sliceData);
                    throw new Error('3D slice data is not in valid format');
                }
                
                const thead = table.createTHead();
                const headerRow = thead.insertRow();
                headerRow.insertCell().textContent = '';
                for (let j = 0; j < shape[2]; j++) {
                    headerRow.insertCell().textContent = j;
                }
                
                const tbody = table.createTBody();
                sliceData.forEach((row, i) => {
                    if (!Array.isArray(row)) {
                        console.error('Row in slice is not an array:', row);
                        return;
                    }
                    const tableRow = tbody.insertRow();
                    tableRow.insertCell().textContent = i;
                    row.forEach(value => {
                        const cell = tableRow.insertCell();
                        cell.textContent = typeof value === 'number' ? value.toFixed(3) : value;
                    });
                });
            }
        } catch (error) {
            console.error('Error in renderTable:', error);
            this.showAlert('Table Error', `Failed to render table: ${error.message}`, 'danger');
        }
    }
    
    updateSlice(sliceIndex) {
        this.currentSlice = sliceIndex;
        document.getElementById('sliceValue').textContent = sliceIndex;
        
        // Re-render current data with new slice
        if (this.currentFile) {
            // Get current metadata to fetch latest data
            fetch(`/api/metadata/${this.currentFile}`)
                .then(response => response.json())
                .then(metadata => {
                    if (metadata && !metadata.error) {
                        // Request fresh data
                        this.socket.emit('subscribe', { filename: this.currentFile });
                    }
                })
                .catch(error => console.error('Error fetching metadata:', error));
        }
    }
    
    toggleView() {
        this.isTableView = !this.isTableView;
        const btn = document.getElementById('toggleViewBtn');
        
        if (this.isTableView) {
            btn.innerHTML = '<i class="fas fa-chart-line"></i> Plot View';
        } else {
            btn.innerHTML = '<i class="fas fa-table"></i> Table View';
        }
        
        // Re-render with current data if we have any
        if (this.currentFile && this.currentData) {
            this.updateVisualization(this.currentData);
        }
    }
    
    toggleFileSelection(filename, selected) {
        if (selected) {
            this.selectedFiles.add(filename);
        } else {
            this.selectedFiles.delete(filename);
        }
        this.updateSelectedFilesDisplay();
    }
    
    updateSelectedFilesDisplay() {
        const container = document.getElementById('selectedFiles');
        const count = document.getElementById('selectedCount');
        
        count.textContent = this.selectedFiles.size;
        container.innerHTML = '';
        
        if (this.selectedFiles.size === 0) {
            container.innerHTML = '<div class="text-muted small">No files selected</div>';
            return;
        }
        
        this.selectedFiles.forEach(filename => {
            const item = document.createElement('div');
            const isMaster = filename === this.currentFile;
            
            item.className = `selected-file-item ${isMaster ? 'master' : ''}`;
            item.innerHTML = `
                <span class="text-truncate">${filename} ${isMaster ? '(Master)' : ''}</span>
                <button class="remove-selected" onclick="viewer.removeSelectedFile('${filename}')">
                    <i class="fas fa-times"></i>
                </button>
            `;
            
            container.appendChild(item);
        });
    }
    
    removeSelectedFile(filename) {
        this.selectedFiles.delete(filename);
        this.updateSelectedFilesDisplay();
        
        // Update checkbox
        const checkbox = document.querySelector(`[data-file="${filename}"]`);
        if (checkbox) {
            checkbox.checked = false;
        }
    }
    
    clearSelectedFiles() {
        this.selectedFiles.clear();
        this.updateSelectedFilesDisplay();
        
        // Clear all checkboxes
        document.querySelectorAll('.file-checkbox').forEach(cb => {
            cb.checked = false;
        });
    }
    
    async loadMetadata(filename) {
        try {
            const response = await fetch(`/api/metadata/${filename}`);
            const metadata = await response.json();
            this.renderMetadata(metadata);
        } catch (error) {
            console.error('Error loading metadata:', error);
        }
    }
    
    renderMetadata(metadata) {
        const container = document.getElementById('metadataContent');
        
        if (!metadata || metadata.error) {
            container.innerHTML = '<div class="text-muted">Error loading metadata</div>';
            return;
        }
        
        let html = `
            <div class="metadata-item">
                <span class="metadata-label">Filename:</span> 
                <span class="metadata-value">${metadata.filename}</span>
            </div>
            <div class="metadata-item">
                <span class="metadata-label">Shape:</span> 
                <span class="metadata-value">${JSON.stringify(metadata.shape)}</span>
            </div>
            <div class="metadata-item">
                <span class="metadata-label">Data Type:</span> 
                <span class="metadata-value">${metadata.dtype}</span>
            </div>
            <div class="metadata-item">
                <span class="metadata-label">Counter:</span> 
                <span class="metadata-value">${metadata.counter}</span>
            </div>
            <div class="metadata-item">
                <span class="metadata-label">Timestamp:</span> 
                <span class="metadata-value">${new Date(metadata.timestamp).toLocaleString()}</span>
            </div>
        `;
        
        if (metadata.min !== undefined) {
            html += `
                <div class="metadata-item">
                    <span class="metadata-label">Min:</span> 
                    <span class="metadata-value">${metadata.min.toFixed(3)}</span>
                </div>
                <div class="metadata-item">
                    <span class="metadata-label">Max:</span> 
                    <span class="metadata-value">${metadata.max.toFixed(3)}</span>
                </div>
                <div class="metadata-item">
                    <span class="metadata-label">Mean:</span> 
                    <span class="metadata-value">${metadata.mean.toFixed(3)}</span>
                </div>
                <div class="metadata-item">
                    <span class="metadata-label">Std Dev:</span> 
                    <span class="metadata-value">${metadata.std.toFixed(3)}</span>
                </div>
            `;
        }
        
        container.innerHTML = html;
    }
    
    updateRecordingFilename(shmFilename) {
        const recordFilename = shmFilename.replace('.im.shm', '.npy');
        document.getElementById('recordFilename').value = recordFilename;
    }
    
    handleDataUpdate(data) {
        if (data.filename === this.currentFile) {
            // Increment update counter
            this.updateCount++;
            
            // Update the UI indicators
            this.updateIndicators(data);
             // Store the complete data structure for re-rendering when view toggles
            this.currentData = data;
            
            // Always update the visualization with new data
            // Use requestAnimationFrame to optimize high-frequency updates
            if (!this.renderPending) {
                this.renderPending = true;
                requestAnimationFrame(() => {
                    this.updateVisualization(data);
                    this.renderPending = false;
                });
            }
            
            this.renderMetadata(data.metadata);
            
            // Update status indicator
            const statusEl = document.getElementById(`status-${data.filename}`);
            if (statusEl) {
                statusEl.className = 'status-indicator online';
            }
            
            // Update recording frame counter if recording
            this.updateRecordingFrameCounter();
        }
    }
    
    async updateRecordingFrameCounter() {
        try {
            const response = await fetch('/api/recording_status');
            const status = await response.json();
            
            // Check if recording just completed
            if (status.completed && status.completion_data) {
                this.handleRecordingComplete(status.completion_data);
                return;
            }
            
            if (status.recording) {
                const frameCounter = document.getElementById('frameCounter');
                const progressBar = document.getElementById('recordingProgress');
                
                if (frameCounter) {
                    if (status.max_frames) {
                        frameCounter.textContent = `${status.frame_count || 0} / ${status.max_frames} frames`;
                        
                        // Update progress bar
                        if (progressBar) {
                            const percentage = ((status.frame_count || 0) / status.max_frames * 100);
                            progressBar.style.width = `${percentage}%`;
                            progressBar.setAttribute('aria-valuenow', percentage);
                        }
                    } else {
                        frameCounter.textContent = `${status.frame_count || 0} frames (continuous)`;
                    }
                }
            }
        } catch (error) {
            // Silently fail - recording status check is not critical
        }
    }
     updateIndicators(data) {
        // Show and update the real-time indicator
        const indicator = document.getElementById('updateIndicator');
        const countEl = document.getElementById('updateCount');
        const lastUpdateEl = document.getElementById('lastUpdate');
        const counterEl = document.getElementById('dataCounter');

        if (indicator) {
            indicator.style.display = 'block';
            
            if (countEl) countEl.textContent = this.updateCount;
            if (lastUpdateEl) lastUpdateEl.textContent = new Date().toLocaleTimeString();
            if (counterEl) counterEl.textContent = data.counter || '-';
        }
    }
    
    updateConnectionStatus(connected) {
        const status = document.getElementById('connectionStatus');
        // Always show connected for HTTP polling
        status.className = 'badge bg-success';
        status.innerHTML = '<i class="fas fa-circle"></i> HTTP Polling';
    }
    
    filterFiles(searchText) {
        const items = document.querySelectorAll('.list-group-item');
        const searchLower = searchText.toLowerCase();
        
        items.forEach(item => {
            const filename = item.querySelector('.file-name');
            if (filename) {
                const show = filename.textContent.toLowerCase().includes(searchLower);
                item.style.display = show ? 'block' : 'none';
            }
        });
    }
    
    async createSharedMemory() {
        const name = document.getElementById('shmName').value;
        const shape = document.getElementById('shmShape').value;
        const dtype = document.getElementById('shmDtype').value;
        
        if (!name || !shape) {
            this.showAlert('Error', 'Please fill in all fields', 'warning');
            return;
        }
        
        try {
            const response = await fetch('/api/create_shm', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify({ name, shape, dtype })
            });
            
            const result = await response.json();
            
            if (result.success) {
                this.showAlert('Success', result.message, 'success');
                document.getElementById('createShmModal').querySelector('.btn-close').click();
                this.loadFiles();
                
                // Clear form
                document.getElementById('shmName').value = '';
                document.getElementById('shmShape').value = '';
            } else {
                this.showAlert('Error', result.error, 'danger');
            }
        } catch (error) {
            console.error('Error creating shared memory:', error);
            this.showAlert('Error', 'Failed to create shared memory', 'danger');
        }
    }
    
    async setData() {
        if (!this.currentFile) {
            this.showAlert('Error', 'No file selected', 'warning');
            return;
        }
        
        const mode = document.querySelector('input[name="setDataMode"]:checked').value;
        let data = { filename: this.currentFile, mode };
        
        if (mode === 'set_value') {
            data.value = parseFloat(document.getElementById('setValue').value);
        } else if (mode === 'randomize') {
            data.min = parseFloat(document.getElementById('randomMin').value);
            data.max = parseFloat(document.getElementById('randomMax').value);
        }
        
        try {
            const response = await fetch('/api/set_data', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify(data)
            });
            
            const result = await response.json();
            
            if (result.success) {
                this.showAlert('Success', 'Data updated successfully', 'success');
                document.getElementById('setDataModal').querySelector('.btn-close').click();
            } else {
                this.showAlert('Error', result.error, 'danger');
            }
        } catch (error) {
            console.error('Error setting data:', error);
            this.showAlert('Error', 'Failed to set data', 'danger');
        }
    }
    
    recordData() {
        if (!this.currentFile) {
            this.showAlert('Error', 'No file selected', 'warning');
            return;
        }
        
        // Check current recording status first
        this.checkRecordingStatus()
            .then(status => {
                if (status.recording) {
                    // Currently recording, so stop it
                    this.stopRecording();
                } else {
                    // Not recording, so start it
                    this.startRecording();
                }
            })
            .catch(error => {
                console.error('Error checking recording status:', error);
                this.showAlert('Error', 'Failed to check recording status', 'danger');
            });
    }
    
    async checkRecordingStatus() {
        try {
            const response = await fetch('/api/recording_status');
            const data = await response.json();
            
            // Update UI based on status
            this.updateRecordingUI(data);
            
            return data;
        } catch (error) {
            console.error('Error checking recording status:', error);
            throw error;
        }
    }
    
    async startRecording() {
        try {
            // Get output filename from input field
            const outputFilename = document.getElementById('recordFilename').value.trim();
            const maxFramesInput = document.getElementById('maxFrames').value.trim();
            
            if (!outputFilename) {
                this.showAlert('Error', 'Please enter a filename for recording', 'warning');
                return;
            }
            
            // Ensure it has .npy extension
            const filename = outputFilename.endsWith('.npy') ? outputFilename : outputFilename + '.npy';
            
            // Parse max frames (null for continuous recording)
            let maxFrames = null;
            if (maxFramesInput && maxFramesInput !== '') {
                maxFrames = parseInt(maxFramesInput);
                if (isNaN(maxFrames) || maxFrames <= 0) {
                    this.showAlert('Error', 'Max frames must be a positive number or left empty for continuous recording', 'warning');
                    return;
                }
            }
            
            const requestBody = {
                shm_filename: this.currentFile,
                output_filename: filename
            };
            
            if (maxFrames !== null) {
                requestBody.max_frames = maxFrames;
            }
            
            const response = await fetch('/api/start_recording', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(requestBody)
            });
            
            const result = await response.json();
            
            if (result.success) {
                this.showAlert('Success', result.message, 'success');
                this.updateRecordingUI({
                    recording: true,
                    shm_filename: this.currentFile,
                    output_file: filename,
                    frame_count: 0,
                    max_frames: maxFrames
                });
            } else {
                this.showAlert('Error', result.error, 'danger');
            }
            
        } catch (error) {
            console.error('Error starting recording:', error);
            this.showAlert('Error', 'Failed to start recording', 'danger');
        }
    }
    
    async stopRecording() {
        try {
            const response = await fetch('/api/stop_recording', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                }
            });
            
            const result = await response.json();
            
            if (result.success) {
                this.showAlert('Success', result.message, 'success');
                
                // Show download link
                const downloadUrl = `/api/download/${result.metadata.output_filename}`;
                const downloadMessage = `
                    <div>
                        <p>${result.message}</p>
                        <p>Frames recorded: ${result.frames_recorded}</p>
                        <a href="${downloadUrl}" class="btn btn-primary btn-sm" download>
                            <i class="fas fa-download"></i> Download ${result.metadata.output_filename}
                        </a>
                    </div>
                `;
                
                // Use a custom alert that allows HTML
                this.showCustomAlert('Recording Complete', downloadMessage, 'success');
                
                this.updateRecordingUI({
                    recording: false,
                    shm_filename: null,
                    output_file: null,
                    frame_count: 0
                });
            } else {
                this.showAlert('Error', result.error, 'danger');
            }
            
        } catch (error) {
            console.error('Error stopping recording:', error);
            this.showAlert('Error', 'Failed to stop recording', 'danger');
        }
    }
    
    updateRecordingUI(status) {
        const recordBtn = document.getElementById('recordBtn');
        const recordFilename = document.getElementById('recordFilename');
        const maxFrames = document.getElementById('maxFrames');
        const recordingStatus = document.getElementById('recordingStatus');
        
        if (status.recording) {
            recordBtn.innerHTML = '<i class="fas fa-stop"></i> Stop Recording';
            recordBtn.className = 'btn btn-danger btn-sm';
            recordFilename.disabled = true;
            maxFrames.disabled = true;
            
            if (recordingStatus) {
                const progressInfo = status.max_frames 
                    ? `${status.frame_count || 0} / ${status.max_frames} frames`
                    : `${status.frame_count || 0} frames (continuous)`;
                
                recordingStatus.innerHTML = `
                    <div class="alert alert-info">
                        <strong>Recording:</strong> ${status.shm_filename} → ${status.output_file}<br>
                        <strong>Progress:</strong> <span id="frameCounter">${progressInfo}</span>
                        ${status.max_frames ? `<div class="progress mt-2">
                            <div class="progress-bar" role="progressbar" style="width: ${((status.frame_count || 0) / status.max_frames * 100)}%" id="recordingProgress"></div>
                        </div>` : ''}
                    </div>
                `;
                recordingStatus.style.display = 'block';
            }
        } else {
            recordBtn.innerHTML = '<i class="fas fa-record-vinyl"></i> Record';
            recordBtn.className = 'btn btn-primary btn-sm';
            recordFilename.disabled = false;
            maxFrames.disabled = false;
            
            if (recordingStatus) {
                recordingStatus.style.display = 'none';
            }
        }
    }
    
    showCustomAlert(title, htmlContent, type) {
        // Create custom modal for HTML content
        const modalHtml = `
            <div class="modal fade" id="customAlertModal" tabindex="-1">
                <div class="modal-dialog">
                    <div class="modal-content">
                        <div class="modal-header bg-${type === 'success' ? 'success' : type === 'danger' ? 'danger' : 'info'} text-white">
                            <h5 class="modal-title">${title}</h5>
                            <button type="button" class="btn-close btn-close-white" data-bs-dismiss="modal"></button>
                        </div>
                        <div class="modal-body">
                            ${htmlContent}
                        </div>
                        <div class="modal-footer">
                            <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Close</button>
                        </div>
                    </div>
                </div>
            </div>
        `;
        
        // Remove existing modal if any
        const existingModal = document.getElementById('customAlertModal');
        if (existingModal) {
            existingModal.remove();
        }
        
        // Add new modal
        document.body.insertAdjacentHTML('beforeend', modalHtml);
        
        // Show modal
        const modal = new bootstrap.Modal(document.getElementById('customAlertModal'));
        modal.show();
        
        // Clean up after modal is hidden
        document.getElementById('customAlertModal').addEventListener('hidden.bs.modal', function() {
            this.remove();
        });
    }
    
    loadFile() {
        if (!this.currentFile) {
            this.showAlert('Error', 'No shared memory file selected', 'warning');
            return;
        }
        
        // This would need to be implemented with file upload
        this.showAlert('Info', 'File loading functionality not yet implemented in web version', 'info');
    }
    
    saveSnapshot() {
        const filename = document.getElementById('snapshotFilename').value;
        if (!filename) {
            this.showAlert('Error', 'Please enter a snapshot filename', 'warning');
            return;
        }
        
        this.showAlert('Info', 'Snapshot functionality not yet implemented in web version', 'info');
    }
    
    loadSnapshot() {
        const filename = document.getElementById('snapshotFilename').value;
        if (!filename) {
            this.showAlert('Error', 'Please enter a snapshot filename', 'warning');
            return;
        }
        
        this.showAlert('Info', 'Snapshot functionality not yet implemented in web version', 'info');
    }

    startPolling(filename) {
        // Stop any existing polling
        this.stopPolling();
        
        this.lastCounter = 0; // Reset counter
        
        const poll = async () => {
            try {
                const response = await fetch(`/api/poll/${filename}?counter=${this.lastCounter}`);
                const result = await response.json();
                
                if (result.success && result.updated) {
                    this.updateCount++;
                    this.lastCounter = result.counter;
                    
                    // Convert to format expected by handleDataUpdate
                    const updateData = {
                        filename: result.filename,
                        counter: result.counter,
                        data: result.data,
                        metadata: result.metadata
                    };
                    
                    this.handleDataUpdate(updateData);
                } else if (result.success) {
                    // No update, just track the counter
                    this.lastCounter = result.counter;
                }
            } catch (error) {
                console.error('Polling error:', error);
            }
        };
        
        // Poll immediately, then every 100ms
        poll();
        this.pollingInterval = setInterval(poll, 100);
    }
    
    stopPolling() {
        if (this.pollingInterval) {
            clearInterval(this.pollingInterval);
            this.pollingInterval = null;
        }
    }
    
    showAlert(title, message, type = 'info') {
        // Create a toast notification
        const toast = document.createElement('div');
        toast.className = `alert alert-${type} alert-dismissible fade show position-fixed`;
        toast.style.cssText = 'top: 20px; right: 20px; z-index: 9999; min-width: 300px;';
        toast.innerHTML = `
            <strong>${title}:</strong> ${message}
            <button type="button" class="btn-close" data-bs-dismiss="alert"></button>
        `;
        
        document.body.appendChild(toast);
        
        // Auto-remove after 5 seconds
        setTimeout(() => {
            if (toast.parentNode) {
                toast.parentNode.removeChild(toast);
            }
        }, 5000);
    }
}

// Initialize the application when the page loads
let viewer;
document.addEventListener('DOMContentLoaded', () => {
    viewer = new DaoShmWebViewer();
});
