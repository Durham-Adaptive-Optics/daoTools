#!/usr/bin/env python3

import sys
import os
import time
import numpy as np
from PyQt5.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QHBoxLayout, QTableWidget, QTableWidgetItem,
    QHeaderView, QPushButton, QLabel, QSpinBox, QSplitter, QListWidget, QMessageBox,
    QTableView, QMainWindow, QStatusBar, QToolBar, QAction, QLineEdit, QComboBox,
    QDialog, QCheckBox, QProgressBar
)
from PyQt5.QtCore import QDir, Qt, QTimer, QAbstractTableModel, QThread, pyqtSignal, QMutex
from PyQt5.QtGui import QIcon

from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
import magicplot
import dao
import daoLaunch
import gc
import traceback
from daoRemoteShmFileClient import daoRemoteShmFileClient

class ConnectionThread(QThread):
    """Background thread for establishing connection to remote server."""
    connection_success = pyqtSignal(object)  # Emits FileClient when connection succeeds
    connection_failed = pyqtSignal(str)      # Emits error message when connection fails
    status_update = pyqtSignal(str)          # Emits status messages for progress updates
    
    def __init__(self, user, machine, port=5555, max_retries=5, retry_delay=1.5):
        super().__init__()
        self.user = user
        self.machine = machine
        self.port = port
        self.max_retries = max_retries
        self.retry_delay = retry_delay
        self.should_exit = False
        self._mutex = QMutex()
        
    def stop(self):
        """Safely stop the thread."""
        self._mutex.lock()
        self.should_exit = True
        self._mutex.unlock()
        self.wait()
    
    def run(self):
        """Thread execution code to establish connection to remote server."""
        retry_count = 0
        
        # Start file server if it's not running
        try:
            self.status_update.emit("Checking and starting remote file server...")
            self.manage_file_server()
            self.status_update.emit("Remote file server started, attempting connection...")
        except Exception as e:
            self.connection_failed.emit(f"Failed to start remote server: {str(e)}")
            return
        
        # Attempt to connect with retries
        while retry_count < self.max_retries:
            # Check if we should exit
            self._mutex.lock()
            if self.should_exit:
                self._mutex.unlock()
                return
            self._mutex.unlock()
            
            try:
                self.status_update.emit(f"Connection attempt {retry_count+1}/{self.max_retries}...")
                client = daoRemoteShmFileClient(server_address=self.machine, port=self.port)
                
                # Test the connection with a simple call
                _ = client.list_files()
                
                # If we got here, connection is successful
                self.connection_success.emit(client)
                return
                
            except Exception as e:
                retry_count += 1
                if retry_count < self.max_retries:
                    self.status_update.emit(f"Connection failed, retrying in {self.retry_delay}s... ({retry_count}/{self.max_retries})")
                    
                    # Sleep with periodic checks for cancellation
                    for _ in range(int(self.retry_delay * 2)):
                        time.sleep(0.5)
                        
                        # Check if we should exit
                        self._mutex.lock()
                        if self.should_exit:
                            self._mutex.unlock()
                            return
                        self._mutex.unlock()
                else:
                    self.connection_failed.emit(f"Failed to connect after {self.max_retries} attempts: {str(e)}")
    
    def manage_file_server(self):
        """Manage the remote file server process."""
        try:
            # Check if session already exists
            if daoLaunch.check_tmux_session('file_server', self.machine, self.user):
                self.status_update.emit("Found existing file server, restarting it...")
                # Kill existing session
                daoLaunch.manage_process(
                    action='kill',
                    tmuxname='file_server',
                    user=self.user,
                    machine=self.machine
                )
                # Small delay to ensure the session is fully terminated
                time.sleep(1)
            
            # Check if any process is using the port
            port_check_cmd = f"lsof -i :{self.port} | grep LISTEN"
            port_result = daoLaunch.send_commands([port_check_cmd], user=self.user, machine=self.machine)
            
            # If port is in use, use an alternative port
            if port_result.strip():
                self.status_update.emit(f"Default port {self.port} is in use, using alternative port...")
                alt_port = self.port + 1
                server_args = f' "file_server.py --port={alt_port}"'
                # Update port
                self.port = alt_port
            else:
                server_args = f' "file_server.py"'
                
            # Launch the server in a new tmux session
            daoLaunch.manage_process(
                action='launch',
                tmuxname='file_server',
                user=self.user,
                machine=self.machine,
                processExe='python3',
                processArgs=server_args,
                workingDir='/Users/davidbarr/Code/dao/src'
            )
            
        except Exception as e:
            raise Exception(f"Failed to manage file server: {e}")

class ConnectingDialog(QDialog):
    """Dialog showing connection progress with ability to cancel."""
    def __init__(self, user, machine, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Connecting to Remote Server")
        self.setModal(True)
        layout = QVBoxLayout(self)
        
        # Connection info
        info_label = QLabel(f"Connecting to {user}@{machine}...")
        layout.addWidget(info_label)
        
        # Progress bar
        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 0)  # Indeterminate progress
        layout.addWidget(self.progress_bar)
        
        # Status message
        self.status_label = QLabel("Initializing connection...")
        layout.addWidget(self.status_label)
        
        # Cancel button
        self.cancel_button = QPushButton("Cancel")
        self.cancel_button.clicked.connect(self.reject)
        layout.addWidget(self.cancel_button)
        
        self.setMinimumWidth(400)
        
    def update_status(self, message):
        """Update the status message."""
        self.status_label.setText(message)

class LoginDialog(QDialog):
    """Dialog for SSH connection status."""
    def __init__(self, user, machine, parent=None):
        super().__init__(parent)
        self.setWindowTitle("SSH Connection")
        self.setModal(True)
        layout = QVBoxLayout(self)
        
        # Connection info
        info_label = QLabel(f"Connecting to {user}@{machine}...")
        layout.addWidget(info_label)
        
        # Progress indicator
        self.progress_label = QLabel("Testing SSH connection...")
        self.progress_label.setStyleSheet("color: blue;")
        layout.addWidget(self.progress_label)
        
        # Cancel button
        self.cancel_button = QPushButton("Cancel")
        self.cancel_button.clicked.connect(self.reject)
        layout.addWidget(self.cancel_button)
        
        self.setMinimumWidth(300)

class NumpyTableModel(QAbstractTableModel):
    """Model for displaying and editing NumPy arrays in table view."""
    def __init__(self, data, shm, parent=None):
        super().__init__(parent)
        self._data = data
        self._shm = shm

    def rowCount(self, parent=None):
        return self._data.shape[0]

    def columnCount(self, parent=None):
        return self._data.shape[1]

    def data(self, index, role=Qt.DisplayRole):
        if role == Qt.DisplayRole:
            value = self._data[index.row(), index.column()]
            return f"{value:.3f}"
        return None

    def setData(self, index, value, role=Qt.EditRole):
        if role == Qt.EditRole:
            try:
                self._data[index.row(), index.column()] = float(value)
                self.dataChanged.emit(index, index, [Qt.DisplayRole])
                print(f"Setting data at {index.row()}, {index.column()} to {value}")
                self._shm.set_data(self._data)
                return True
            except ValueError:
                return False
        return False

    def flags(self, index):
        return Qt.ItemFlag.ItemIsSelectable | Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsEditable

class daoRemoteShmViewer(QMainWindow):
    """Main application window for the Remote DAO Shared Memory Viewer."""
    def __init__(self, user, machine):
        super().__init__()
        self.setWindowTitle("Remote DAO Shared Memory Viewer")
        self.user = user
        self.machine = machine
        self.client = None
        self.shm = None
        self.lastCounter = 0
        self.ssh_process = None
        self.connection_thread = None
        self.current_shm = None
        
        # Create a timer for deferred GUI operations
        self.error_timer = QTimer()
        self.error_timer.setSingleShot(True)
        self.error_timer.timeout.connect(self.show_deferred_error)
        self.deferred_error = None
        
        # Test SSH connection before proceeding
        if not self.check_ssh_access():
            sys.exit(0)
        
        self.init_variables()
        self.init_ui()
        self.setup_timers()
        self.setup_connections()
        self.current_slice = 0
        self.is_3d = False
        self.resize(1024, 768)
        
        # Start the connection process in the background
        self.start_connection()

    def check_ssh_access(self):
        """Test SSH key-based authentication."""
        dialog = LoginDialog(self.user, self.machine, self)
        dialog.show()
        QApplication.processEvents()
        
        try:
            # Test SSH connection without password
            result = daoLaunch.send_commands(
                ["echo 'Testing SSH connection'"],
                user=self.user,
                machine=self.machine
            )
            dialog.accept()
            return True
            
        except Exception as e:
            dialog.reject()
            msg = (
                f"SSH key-based authentication failed.\n\n"
                f"To enable password-less access, run:\n"
                f"ssh-copy-id {self.user}@{self.machine}\n\n"
                f"This will copy your SSH key to the remote machine."
            )
            QMessageBox.critical(self, "SSH Authentication Failed", msg)
            return False

    def init_variables(self):
        """Initialize class variables."""
        self.FLAT = False
        self.TABLE = False
        self.ShowTable = False
        self.dataCounter = 0
        self.stillRunning = False
        self.dir = QDir("/tmp")
        self.filters = ["*.im.shm"]

    def init_ui(self):
        """Initialize the user interface."""
        # Central widget and main layout
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_layout = QVBoxLayout(central_widget)
        
        # Create status bar and toolbar
        self.statusBar = QStatusBar()
        self.setStatusBar(self.statusBar)
        self.setup_toolbar()
        
        # Connection info bar
        conn_layout = QHBoxLayout()
        self.connection_label = QLabel(f"Connecting to: {self.user}@{self.machine}...")
        self.connection_status = QLabel("Status: Connecting...")
        self.connection_status.setStyleSheet("color: orange;")
        conn_layout.addWidget(self.connection_label)
        conn_layout.addWidget(self.connection_status)
        conn_layout.addStretch()
        
        # Add reconnect button
        self.reconnect_button = QPushButton("Reconnect")
        self.reconnect_button.clicked.connect(self.start_connection)
        self.reconnect_button.setEnabled(False)  # Disabled initially during first connection
        conn_layout.addWidget(self.reconnect_button)
        
        main_layout.addLayout(conn_layout)
        
        # Main splitter
        self.main_splitter = QSplitter(Qt.Horizontal)
        
        # Left side: File table
        self.setup_file_table()
        self.main_splitter.addWidget(self.tableWidget)
        
        # Right side: Graph/visualization
        self.graphWidget = magicplot.MagicPlot()
        self.main_splitter.addWidget(self.graphWidget)
        
        # Set splitter proportions
        self.main_splitter.setSizes([300, 700])
        main_layout.addWidget(self.main_splitter)

    def setup_toolbar(self):
        """Setup the application toolbar."""
        toolbar = QToolBar("Main Toolbar")
        self.addToolBar(toolbar)
        
        # Refresh action
        refresh_action = QAction("Refresh", self)
        refresh_action.triggered.connect(self.updateFileList)
        toolbar.addAction(refresh_action)
        
        # Toggle view action
        toggle_view_action = QAction("Toggle View", self)
        toggle_view_action.triggered.connect(self.toggle_view)
        toolbar.addAction(toggle_view_action)

    def setup_file_table(self):
        """Setup the file table widget."""
        self.tableWidget = QTableWidget(self)
        self.tableWidget.setColumnCount(1)
        self.tableWidget.setHorizontalHeaderLabels(["Filename"])
        self.tableWidget.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        self.tableWidget.setSelectionBehavior(QTableWidget.SelectRows)
        self.tableWidget.keyPressEvent = self.tableKeyPressEvent
        
        # Disable the table initially until connected
        self.tableWidget.setEnabled(False)

    def tableKeyPressEvent(self, event):
        """Handle key press events in the table."""
        currentRow = self.tableWidget.currentRow()
        if event.key() == Qt.Key_Up and currentRow > 0:
            self.tableWidget.setCurrentCell(currentRow - 1, 0)
            self.onCellClicked(currentRow - 1, 0)
        elif event.key() == Qt.Key_Down and currentRow < self.tableWidget.rowCount() - 1:
            self.tableWidget.setCurrentCell(currentRow + 1, 0)
            self.onCellClicked(currentRow + 1, 0)
        else:
            super(self.tableWidget.__class__, self.tableWidget).keyPressEvent(event)

    def setup_timers(self):
        """Setup application timers."""
        self.timer = QTimer()
        self.timer.timeout.connect(self.timer_triggered)

    def setup_connections(self):
        """Setup signal-slot connections."""
        self.tableWidget.cellClicked.connect(self.onCellClicked)

    def start_connection(self):
        """Start the connection process in a background thread."""
        # Disable reconnect button during connection
        if hasattr(self, 'reconnect_button'):
            self.reconnect_button.setEnabled(False)
        
        # Update status
        self.connection_status.setText("Status: Connecting...")
        self.connection_status.setStyleSheet("color: orange;")
        self.statusBar.showMessage("Connecting to remote server...")
        
        # Disable table during connection
        self.tableWidget.setEnabled(False)
        
        # Create and show connecting dialog
        self.connecting_dialog = ConnectingDialog(self.user, self.machine, self)
        
        # Stop any existing connection thread
        if self.connection_thread and self.connection_thread.isRunning():
            self.connection_thread.stop()
        
        # Create new connection thread
        self.connection_thread = ConnectionThread(self.user, self.machine, port=5555)
        
        # Connect signals
        self.connection_thread.connection_success.connect(self.on_connection_success)
        self.connection_thread.connection_failed.connect(self.on_connection_failed)
        self.connection_thread.status_update.connect(self.connecting_dialog.update_status)
        self.connection_thread.status_update.connect(lambda msg: self.statusBar.showMessage(msg))
        
        # Connect dialog's reject signal to cancel the connection
        self.connecting_dialog.rejected.connect(self.cancel_connection)
        
        # Start the connection thread
        self.connection_thread.start()
        
        # Show the dialog (non-modal to keep GUI responsive)
        self.connecting_dialog.setModal(False)
        self.connecting_dialog.show()

    def cancel_connection(self):
        """Cancel the current connection attempt."""
        if self.connection_thread and self.connection_thread.isRunning():
            self.statusBar.showMessage("Cancelling connection...")
            self.connection_thread.stop()
            
        # Enable reconnect button
        self.reconnect_button.setEnabled(True)
        self.connection_status.setText("Status: Disconnected")
        self.connection_status.setStyleSheet("color: red;")

    def on_connection_success(self, client):
        """Handle successful connection."""
        self.client = client
        
        # Update status
        self.connection_status.setText("Status: Connected")
        self.connection_status.setStyleSheet("color: green;")
        self.statusBar.showMessage("Connected to remote server")
        
        # Enable UI elements
        self.reconnect_button.setEnabled(True)
        self.tableWidget.setEnabled(True)
        
        # Close connecting dialog if it's open
        if hasattr(self, 'connecting_dialog') and self.connecting_dialog.isVisible():
            self.connecting_dialog.accept()
        
        # Update file list
        self.updateFileList()

    def on_connection_failed(self, error_message):
        """Handle connection failure."""
        # Update status
        self.connection_status.setText("Status: Connection failed")
        self.connection_status.setStyleSheet("color: red;")
        self.statusBar.showMessage(f"Connection failed: {error_message}")
        
        # Enable reconnect button
        self.reconnect_button.setEnabled(True)
        
        # Close connecting dialog if it's open
        if hasattr(self, 'connecting_dialog') and self.connecting_dialog.isVisible():
            self.connecting_dialog.accept()
        
        # Show error message
        QMessageBox.critical(self, "Connection Failed", 
                            f"Failed to connect to the remote server:\n\n{error_message}\n\n"
                            f"You can try reconnecting when the server is available.")

    def updateFileList(self):
        """Update the list of shared memory files from remote server."""
        if not self.client:
            return
            
        try:
            # Always fetch a fresh list from the server
            files = self.client.list_files()
            self.tableWidget.setRowCount(len(files))
            
            for row, filename in enumerate(files):
                self.tableWidget.setItem(row, 0, QTableWidgetItem(filename))
                
        except Exception as e:
            self.show_error(f"Error updating file list: {e}")

    def onCellClicked(self, row, column):
        """Handle cell click in the file table."""
        if not self.client:
            self.show_error("Not connected to server")
            return
            
        try:
            # Show loading message before starting operation
            self.statusBar.showMessage("Loading file...")
            QApplication.processEvents()  # Process events to update UI
            
            # Stop timer first to prevent race conditions
            if self.timer.isActive():           
                self.timer.stop()
            
            # Safely close existing SHM connection if it exists
            if self.shm: 
                try:
                    self.client.close_file(self.current_shm)
                except Exception as e:
                    self.statusBar.showMessage(f"Warning: Error closing previous SHM: {e}")
                
                self.shm = None
                # Force garbage collection to clean up references
                gc.collect()
                
            # Get the filename from the table
            filename = self.tableWidget.item(row, 0).text()
            self.current_shm = filename
            
            self.statusBar.showMessage(f"Loading {filename}...")
            
            # Open the remote file through the client with error handling
            try:
                # Always open fresh (client.open_file already enforces this)
                self.shm = self.client.open_file(filename)
                if not self.shm:
                    self.show_error("Failed to open file: null SHM returned")
                    return
            except Exception as e:
                self.show_error(f"Failed to open file: {e}")
                return
                
            # Store counter and get initial data
            self.lastCounter = self.shm.get_counter()
            data = self.shm.get_data()
            self.dataCounter = self.shm.get_counter()
            
            # Determine visualization type
            if len(data.shape) == 0 or np.prod(data.shape) == 0:
                self.show_error(f"Invalid data shape: {data.shape}")
                return
                
            if np.shape(data) == (1, 1):
                self.TABLE = True
                self.FLAT = False
            elif np.min(data.shape) == 1:
                self.FLAT = True
                self.TABLE = False
            else:
                self.FLAT = False
                self.TABLE = False
                
            self.update_visualisation()
            self.timer.start(100)
            self.statusBar.showMessage(f"Loaded {filename}")
            
        except Exception as e:
            self.show_error(f"Error loading file: {e}")
            # Make sure we don't leave timer running on error
            if self.timer.isActive():
                self.timer.stop()

    def toggle_view(self):
        """Toggle between table and graph views."""
        self.TABLE = not self.TABLE
        if self.shm:
            self.update_visualisation()

    def update_visualisation(self):
        """Update the visualisation based on current data."""
        if not self.shm:
            return
        
        # Remove existing slice selector if it exists
        if hasattr(self, 'slice_widget') and self.slice_widget is not None:
            self.statusBar.removeWidget(self.slice_widget)
            self.slice_widget = None
            self.slice_selector = None
        
        # Check if data is 3D and prepare slice handling
        data = self.shm.get_data()
        self.is_3d = len(data.shape) == 3

        # Remove existing visualisation widget to prepare for replacement
        index = self.main_splitter.indexOf(self.graphWidget)
        if self.graphWidget:
            self.graphWidget = None
            gc.collect()
        
        # Create appropriate visualisation widget based on display type
        if self.TABLE or self.ShowTable:
            model = NumpyTableModel(self.shm.get_data(), shm=self.shm)
            self.graphWidget = QTableView()
            self.graphWidget.setModel(model)
        else:
            self.graphWidget = magicplot.MagicPlot()
        
        # Add the new widget to the main splitter
        self.main_splitter.replaceWidget(index, self.graphWidget)

        # Set up slice selector for 3D data
        if self.is_3d:
            # Create new slice selector
            slice_layout = QHBoxLayout()
            slice_label = QLabel("Slice:")
            self.slice_selector = QSpinBox()
            self.slice_selector.setRange(0, data.shape[0]-1)
            self.slice_selector.setValue(self.current_slice)
            self.slice_selector.valueChanged.connect(self.update_slice)
            
            slice_layout.addWidget(slice_label)
            slice_layout.addWidget(self.slice_selector)
            slice_layout.addStretch()
            
            # Add the slice selector to the main window's status bar
            self.slice_widget = QWidget()
            self.slice_widget.setLayout(slice_layout)
            self.statusBar.addPermanentWidget(self.slice_widget)

        # Update visualisation data
        if not (self.TABLE or self.ShowTable):
            if self.FLAT:
                self.im = self.graphWidget.getDataItem()
                self.im.setData(data.flatten())
            else:
                self.im = self.graphWidget.getImageItem()
                if self.is_3d:
                    self.im.setData(data[self.current_slice])
                else:
                    self.im.setData(data)
                
            self.graphWidget.updatePanBounds()
            self.graphWidget.viewBox.autoRange()

    def update_slice(self, value):
        """Update the displayed slice for 3D data."""
        if self.shm and self.is_3d and not (self.TABLE or self.ShowTable):
            self.current_slice = value
            self.im.setData(self.shm.get_data()[value])
            self.graphWidget.updatePanBounds()

    def timer_triggered(self):
        """Handle timer tick events for data updates."""
        if not self.shm:
            return
            
        try:
            self.newCounter = self.shm.get_counter()
            if self.newCounter != self.lastCounter:
                # Only update the data if it's actually changed (counter changed)
                if self.TABLE:
                    # For table view, we need to preserve the existing model for edit continuity
                    if not hasattr(self, 'table_model') or self.table_model is None:
                        self.table_model = NumpyTableModel(self.shm.get_data(), self.shm)
                        self.graphWidget.setModel(self.table_model)
                    else:
                        # Update the model's data while preserving edited cells
                        current_data = self.table_model._data.copy()
                        new_data = self.shm.get_data()
                        # Only update the model's data reference, don't create a new model
                        # This preserves any active edits
                        self.table_model._data = new_data
                        self.table_model.dataChanged.emit(
                            self.table_model.index(0, 0),
                            self.table_model.index(self.table_model.rowCount()-1, self.table_model.columnCount()-1)
                        )
                else:
                    if self.FLAT:
                        self.im.setData(self.shm.get_data().flatten())
                    else:
                        data = self.shm.get_data()
                        if self.is_3d:
                            self.im.setData(data[self.current_slice])
                        else:
                            self.im.setData(data)
                            
                self.lastCounter = self.newCounter
                
        except Exception as e:
            self.timer.stop()
            self.show_error(f"Error updating data: {e}")

    def show_error(self, message):
        """Thread-safe error display."""
        if QThread.currentThread() is QApplication.instance().thread():
            # We're on the main thread, show directly
            QMessageBox.critical(self, "Error", message)
            self.statusBar.showMessage(f"Error: {message}", 5000)
        else:
            # We're on a background thread, defer to main thread
            self.deferred_error = message
            self.error_timer.start(0)

    def show_deferred_error(self):
        """Show an error message that was triggered from a background thread."""
        if self.deferred_error:
            QMessageBox.critical(self, "Error", self.deferred_error)
            self.statusBar.showMessage(f"Error: {self.deferred_error}", 5000)
            self.deferred_error = None

    def closeEvent(self, event):
        """Handle application close event."""
        try:
            # Stop any active connection thread
            if self.connection_thread and self.connection_thread.isRunning():
                self.connection_thread.stop()
                
            # Stop timer before closing
            if self.timer and self.timer.isActive():
                self.timer.stop()
                
            # Clean up client connections first
            if hasattr(self, 'client') and self.client:
                # Loop through all open files and close them properly
                if hasattr(self.client, 'active_shms'):
                    file_paths = list(self.client.active_shms.keys())
                    for file_path in file_paths:
                        try:
                            self.client.close_file(file_path)
                        except Exception as e:
                            print(f"Error closing file {file_path}: {e}")
            
            # Now clean up local shm references
            if self.shm:
                try:
                    del self.shm
                except Exception as e:
                    print(f"Error closing SHM: {e}")
                self.shm = None
                
            # Force garbage collection
            gc.collect()

            # Stop the remote server
            try:
                daoLaunch.manage_process(
                    action='kill',
                    tmuxname='file_server',
                    user=self.user,
                    machine=self.machine
                )
            except Exception as e:
                print(f"Error stopping remote server: {e}")
                
        except Exception as e:
            print(f"Error during shutdown: {str(e)}\n{traceback.format_exc()}")
            
        event.accept()

def main():
    """Main application entry point."""
    import argparse
    
    parser = argparse.ArgumentParser(description='Remote DAO Shared Memory Viewer')
    parser.add_argument('-u', '--user', required=True, help='Remote username')
    parser.add_argument('-m', '--machine', required=True, help='Remote machine name/IP')
    
    args = parser.parse_args()
    
    app = QApplication(sys.argv)
    app.setApplicationDisplayName("Remote DAO Shared Memory Viewer")
    app.setOrganizationName("DAO")
    
    # Set application icon if available
    dao_root = os.getenv('DAOROOT')
    if dao_root:
        icon_path = os.path.join(dao_root, 'data/daoLogo.png')
        if os.path.exists(icon_path):
            app.setWindowIcon(QIcon(icon_path))
    
    viewer = daoRemoteShmViewer(args.user, args.machine)
    viewer.show()
    
    sys.exit(app.exec_())

if __name__ == '__main__':
    main()