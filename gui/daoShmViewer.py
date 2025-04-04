#!/usr/bin/env python3

import sys
import os
import numpy as np
from astropy.io import fits
import subprocess
import time
import signal

from PyQt5.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QHBoxLayout, QTableWidget, QTableWidgetItem,
    QCheckBox, QHeaderView, QLineEdit, QPushButton, QFileDialog, QLabel, QSpinBox,
    QTabWidget, QSplitter, QTextEdit, QListWidget, QMessageBox, QTableView,
    QComboBox, QMainWindow, QStatusBar, QToolBar, QAction, QDialog, QGridLayout,
    QRadioButton, QButtonGroup, QDoubleSpinBox
)
from PyQt5.QtCore import QDir, Qt, QTimer, QAbstractTableModel
from PyQt5.QtGui import QIcon

from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure

import magicplot
import dao
import gc

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
                self._shm.set_data(self._data)
                return True
            except ValueError:
                return False
        return False

    def flags(self, index):
        return Qt.ItemFlag.ItemIsSelectable | Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsEditable

class GraphWidget(QWidget):
    """Widget for displaying graph visualizations."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.figure = Figure()
        self.canvas = FigureCanvas(self.figure)
        layout = QVBoxLayout(self)
        layout.addWidget(self.canvas)
        self.setLayout(layout)
        self.plot()

    def plot(self, data=None):
        self.figure.clear()
        ax = self.figure.add_subplot(111)
        if data is not None:
            if data.ndim == 1:
                ax.plot(data)
            else:
                ax.imshow(data)
        else:
            ax.plot([0, 1, 2, 3], [10, 1, 20, 3])
        self.canvas.draw()

class CreateShmDialog(QDialog):
    """Dialog for creating shared memory arrays."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Create Shared Memory")
        layout = QGridLayout(self)
        
        # Name input
        layout.addWidget(QLabel("Name:"), 0, 0)
        self.nameEdit = QLineEdit()
        self.nameEdit.setPlaceholderText("Enter shm name")
        layout.addWidget(self.nameEdit, 0, 1)
        
        # Shape input
        layout.addWidget(QLabel("Shape:"), 1, 0)
        self.shapeEdit = QLineEdit()
        self.shapeEdit.setPlaceholderText("Enter shape as comma-separated values (e.g., 128,128,3)")
        layout.addWidget(self.shapeEdit, 1, 1)
        
        # Data type input
        layout.addWidget(QLabel("Data Type:"), 2, 0)
        self.dtypeComboBox = QComboBox()
        self.dtypeComboBox.addItems([
            "int8", "int16", "int32", "int64",
            "uint8", "uint16", "uint32", "uint64",
            "float32", "float64", "complex64", "complex128"
        ])
        layout.addWidget(self.dtypeComboBox, 2, 1)
        
        # Buttons
        buttonBox = QHBoxLayout()
        self.createButton = QPushButton("Create")
        self.createButton.clicked.connect(self.accept)
        self.cancelButton = QPushButton("Cancel")
        self.cancelButton.clicked.connect(self.reject)
        
        buttonBox.addWidget(self.createButton)
        buttonBox.addWidget(self.cancelButton)
        layout.addLayout(buttonBox, 3, 0, 1, 2)
        
        self.setLayout(layout)
    
    def get_values(self):
        return {
            "name": self.nameEdit.text(),
            "shape": self.shapeEdit.text(),
            "dtype": self.dtypeComboBox.currentText()
        }

class SetDataDialog(QDialog):
    """Dialog for setting shared memory data values."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Set Data")
        self.resize(350, 200)
        
        layout = QVBoxLayout(self)
        
        # Option group
        self.optionGroup = QButtonGroup(self)
        
        # Option 1: Set all to value
        self.setValueRadio = QRadioButton("Set all data to value:")
        self.optionGroup.addButton(self.setValueRadio)
        self.setValueRadio.setChecked(True)  # Default option
        
        valueLayout = QHBoxLayout()
        self.valueEdit = QDoubleSpinBox()
        self.valueEdit.setRange(-1e9, 1e9)
        self.valueEdit.setValue(0)
        self.valueEdit.setDecimals(3)
        valueLayout.addWidget(self.setValueRadio)
        valueLayout.addWidget(self.valueEdit)
        
        # Option 2: Randomize
        self.randomizeRadio = QRadioButton("Randomize data between:")
        self.optionGroup.addButton(self.randomizeRadio)
        
        randomLayout = QHBoxLayout()
        self.minEdit = QDoubleSpinBox()
        self.minEdit.setRange(-1e9, 1e9)
        self.minEdit.setValue(0)
        self.minEdit.setDecimals(3)
        
        self.maxEdit = QDoubleSpinBox()
        self.maxEdit.setRange(-1e9, 1e9)
        self.maxEdit.setValue(1)
        self.maxEdit.setDecimals(3)
        
        randomLayout.addWidget(self.randomizeRadio)
        randomLayout.addWidget(QLabel("Min:"))
        randomLayout.addWidget(self.minEdit)
        randomLayout.addWidget(QLabel("Max:"))
        randomLayout.addWidget(self.maxEdit)
        
        # Add layouts to main layout
        layout.addLayout(valueLayout)
        layout.addLayout(randomLayout)
        
        # Add buttons
        buttonBox = QHBoxLayout()
        self.applyButton = QPushButton("Apply")
        self.applyButton.clicked.connect(self.accept)
        self.cancelButton = QPushButton("Cancel")
        self.cancelButton.clicked.connect(self.reject)
        
        buttonBox.addWidget(self.applyButton)
        buttonBox.addWidget(self.cancelButton)
        
        layout.addStretch()
        layout.addLayout(buttonBox)
        
    def get_values(self):
        """Get the selected option and values."""
        if self.setValueRadio.isChecked():
            return {
                "mode": "set_value",
                "value": self.valueEdit.value()
            }
        else:
            return {
                "mode": "randomize",
                "min": self.minEdit.value(),
                "max": self.maxEdit.value()
            }

class daoShmViewer(QMainWindow):
    """Main application window for the DAO Shared Memory Viewer."""
    def __init__(self):
        super().__init__()
        self.setWindowTitle("DAO Shared Memory Viewer")
        self.init_variables()
        self.init_ui()
        self.setup_timers()
        self.setup_connections()

        self.resize(1024, 768)

    def init_variables(self):
        """Initialize class variables."""
        self.shm = None
        self.lastCounter = 0
        self.FLAT = False
        self.TABLE = False
        self.ShowTable = False
        self.dataCounter = 0
        self.stillRunning = False
        
        # Directory to monitor
        self.dir = QDir("/tmp")
        self.filters = ["*.im.shm"]

    def init_ui(self):
        """Initialize the user interface."""
        # Central widget and main layout
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_layout = QVBoxLayout(central_widget)
        
        # Create status bar
        self.statusBar = QStatusBar()
        self.setStatusBar(self.statusBar)
        
        # Create toolbar
        self.setup_toolbar()
        
        # Top splitter: File list and visualization
        self.top_splitter = QSplitter(Qt.Horizontal)
        
        # Left side: File table
        self.setup_file_table()
        self.top_splitter.addWidget(self.tableWidget)
        
        # Right side: Graph/visualization
        self.graphWidget = magicplot.MagicPlot()
        self.top_splitter.addWidget(self.graphWidget)
        
        # Main splitter: Top layout and controls
        self.main_splitter = QSplitter(Qt.Vertical)
        self.main_splitter.addWidget(self.top_splitter)
        
        # Bottom: Tabbed controls
        self.setup_tab_widget()
        self.main_splitter.addWidget(self.tabWidget)
        
        # Set splitter proportions
        self.main_splitter.setSizes([300, 500])  # Adjusted proportions
        self.top_splitter.setSizes([300, 700])
        
        main_layout.addWidget(self.main_splitter)

    def setup_toolbar(self):
        """Setup the application toolbar."""
        toolbar = QToolBar("Main Toolbar")
        self.addToolBar(toolbar)
        
        # Refresh action
        refresh_action = QAction("Refresh", self)
        refresh_action.triggered.connect(self.updateFileList)
        toolbar.addAction(refresh_action)
        
        # Create SHM action
        create_shm_action = QAction("Create SHM", self)
        create_shm_action.triggered.connect(self.show_create_shm_dialog)
        toolbar.addAction(create_shm_action)
        
        # Toggle view action
        toggle_view_action = QAction("Toggle View", self)
        toggle_view_action.triggered.connect(self.toggle_view)
        toolbar.addAction(toggle_view_action)
        
        # Set data action
        set_data_action = QAction("Set Data", self)
        set_data_action.triggered.connect(self.showSetDataDialog)
        toolbar.addAction(set_data_action)

    def setup_file_table(self):
        """Setup the file table widget."""
        self.tableWidget = QTableWidget(self)
        self.tableWidget.setColumnCount(2)
        self.tableWidget.setHorizontalHeaderLabels(["Select", "Filename"])
        self.tableWidget.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        self.tableWidget.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        self.tableWidget.setSelectionBehavior(QTableWidget.SelectRows)
        
        # Add keyboard navigation support
        self.tableWidget.keyPressEvent = self.tableKeyPressEvent
        
        # Initial file listing
        self.updateFileList()

    def tableKeyPressEvent(self, event):
        """Handle key press events in the tableWidget for navigation.
        
        This allows up/down arrows to change the selected row and automatically
        trigger the same behavior as if the cell was clicked.
        """
        # Get the current row
        currentRow = self.tableWidget.currentRow()
        
        # Handle Up/Down arrow keys
        if event.key() == Qt.Key_Up and currentRow > 0:
            # Move to the row above
            newRow = currentRow - 1
            self.tableWidget.setCurrentCell(newRow, 1)
            self.onCellClicked(newRow, 1)
        elif event.key() == Qt.Key_Down and currentRow < self.tableWidget.rowCount() - 1:
            # Move to the row below
            newRow = currentRow + 1
            self.tableWidget.setCurrentCell(newRow, 1)
            self.onCellClicked(newRow, 1)
        else:
            # For all other keys, use the default event handler
            super(self.tableWidget.__class__, self.tableWidget).keyPressEvent(event)

    def setup_tab_widget(self):
        """Setup the tabbed control panel."""
        self.tabWidget = QTabWidget()
        self.tabWidget.setMinimumSize(400, 300)  # Increased minimum height from 200 to 300
        self.tabWidget.setTabsClosable(True)
        self.tabWidget.setMovable(True)
        # self.tabWidget.setTabPosition(QTabWidget.normalGeometry)
        self.tabWidget.setTabShape(QTabWidget.Triangular)
        # self.tabWidget.setUsesScrollButtons(True)
        self.tabWidget.setDocumentMode(True)
        
        # Tab 2: File Metadata
        self.setup_metadata_tab()
        
        # Tab 1: Recording controls
        self.setup_recording_tab()
        
        # Tab 3: Multi-Record
        self.setup_multi_record_tab()
        
        # Tab 4: Load
        self.setup_load_tab()
        
        # Tab 5: Snapshot
        self.setup_snapshot_tab()
        
        # Tab 6: Tmux Sessions
        self.setup_tmux_tab()

    def setup_recording_tab(self):
        """Setup the recording tab."""
        recordingTab = QWidget()
        recordingLayout = QVBoxLayout()
        
        self.filenameEdit = QLineEdit()
        self.filenameEdit.setReadOnly(True)
        self.filenameEdit.mousePressEvent = self.openFileDialog
        
        self.frameCounter = QSpinBox()
        self.frameCounter.setRange(1, 10000)
        self.frameCounter.setValue(1)
        
        self.recordButton = QPushButton("Record")        
        recordingLayout.addWidget(QLabel("Filename:"))
        recordingLayout.addWidget(self.filenameEdit)
        recordingLayout.addWidget(QLabel("Number of Frames:"))
        recordingLayout.addWidget(self.frameCounter)
        recordingLayout.addWidget(self.recordButton)
        
        recordingTab.setLayout(recordingLayout)
        self.tabWidget.addTab(recordingTab, "Recording")

    def setup_metadata_tab(self):
        """Setup the metadata tab."""
        metadataTab = QWidget()
        metadataLayout = QVBoxLayout()
        
        self.metadataText = QTextEdit()
        self.metadataText.setReadOnly(True)
        
        metadataLayout.addWidget(QLabel("File Metadata:"))
        metadataLayout.addWidget(self.metadataText)
        metadataLayout.addStretch()
        metadataTab.setLayout(metadataLayout)
        self.tabWidget.addTab(metadataTab, "Metadata")

    def setup_multi_record_tab(self):
        """Setup the multi-record tab."""
        multiRecordTab = QWidget()
        multiRecordLayout = QVBoxLayout()
        
        self.multiRecordList = QListWidget()
        self.multiRecordFilenameEdit = QLineEdit()
        self.multiRecordFilenameEdit.setReadOnly(True)
        self.multiRecordFilenameEdit.mousePressEvent = self.openFolderDialog
        
        self.multiFrameCounter = QSpinBox()
        self.multiFrameCounter.setRange(0, 1000)
        self.multiFrameCounter.setValue(1)
        
        self.multiRecordButton = QPushButton("Record Selected Files")
        
        # Master file label will be updated when a master is selected
        self.masterFileLabel = QLabel("Selected Files: (No master selected)")
        self.masterFileLabel.setStyleSheet("font-weight: bold;")
        
        multiRecordLayout.addWidget(self.masterFileLabel)
        multiRecordLayout.addWidget(self.multiRecordList)
        
        # Connect list item selection to update master file display
        self.multiRecordList.itemSelectionChanged.connect(self.updateMasterFileDisplay)
        
        multiRecordLayout.addWidget(QLabel("Save to Folder:"))
        multiRecordLayout.addWidget(self.multiRecordFilenameEdit)
        multiRecordLayout.addWidget(QLabel("Number of Frames:"))
        multiRecordLayout.addWidget(self.multiFrameCounter)
        multiRecordLayout.addWidget(self.multiRecordButton)
        
        multiRecordTab.setLayout(multiRecordLayout)
        self.tabWidget.addTab(multiRecordTab, "Multi-Record (0)")

    def setup_load_tab(self):
        """Setup the load tab."""
        loadTab = QWidget()
        loadLayout = QVBoxLayout()
        
        self.loadFilenameEdit = QLineEdit()
        self.loadFilenameEdit.setReadOnly(True)
        self.loadFilenameEdit.mousePressEvent = self.openLoadFileDialog
        
        self.loadButton = QPushButton("Load")
        self.zeroButton = QPushButton("Zero")
        
        loadLayout.addWidget(QLabel("Filename:"))
        loadLayout.addWidget(self.loadFilenameEdit)
        loadLayout.addWidget(self.loadButton)
        # loadLayout.addWidget(self.setDataButton)
        
        loadTab.setLayout(loadLayout)
        self.tabWidget.addTab(loadTab, "Load")

    def setup_snapshot_tab(self):
        """Setup the snapshot tab."""
        snapShotTab = QWidget()
        snapShotLayout = QVBoxLayout()
        
        self.snapshot_FilenameEdit = QLineEdit()
        self.snapshot_FilenameEdit.setReadOnly(True)
        self.snapshot_FilenameEdit.mousePressEvent = self.openLoadSnapshotDialog
        
        self.snapshot_SaveButton = QPushButton("Save Snapshot")
        self.snapshot_LoadButton = QPushButton("Load Snapshot")
        
        snapShotLayout.addWidget(QLabel("Filename:"))
        snapShotLayout.addWidget(self.snapshot_FilenameEdit)
        snapShotLayout.addWidget(self.snapshot_SaveButton)
        snapShotLayout.addWidget(self.snapshot_LoadButton)
        
        snapShotTab.setLayout(snapShotLayout)
        self.tabWidget.addTab(snapShotTab, "Snapshot")

    def setup_tmux_tab(self):
        """Setup the tmux sessions tab."""
        tmuxTab = QWidget()
        tmuxLayout = QVBoxLayout()
        
        self.tmuxSessionList = QListWidget()
        self.tmuxConnectButton = QPushButton("Connect to Session")
        self.tmuxConnectButton.clicked.connect(self.connect_to_tmux_session)
        
        tmuxLayout.addWidget(QLabel("Tmux Sessions:"))
        tmuxLayout.addWidget(self.tmuxSessionList)
        tmuxLayout.addWidget(self.tmuxConnectButton)
        
        tmuxTab.setLayout(tmuxLayout)
        self.tabWidget.addTab(tmuxTab, "Tmux Sessions")

    def setup_timers(self):
        """Setup application timers."""
        # Timer for updating visualization
        self.timer = QTimer()
        self.timer.timeout.connect(self.on_timer_triggered)

    def setup_connections(self):
        """Setup signal-slot connections."""
        self.tableWidget.cellClicked.connect(self.onCellClicked)
        self.recordButton.clicked.connect(lambda: self.record_file(self.filenameEdit.text(), self.frameCounter.value()))
        self.loadButton.clicked.connect(self.loadFile)
        self.snapshot_SaveButton.clicked.connect(self.snapshot_SaveFunction)
        self.snapshot_LoadButton.clicked.connect(self.snapshot_LoadFunction)
        self.multiRecordButton.clicked.connect(self.record_multiple_files)
        self.tabWidget.currentChanged.connect(self.on_tab_changed)

    def show_create_shm_dialog(self):
        """Show dialog for creating shared memory."""
        dialog = CreateShmDialog(self)
        if dialog.exec_():
            values = dialog.get_values()
            self.create_shared_memory(values["name"], values["shape"], values["dtype"])

    def create_shared_memory(self, name, shape_str, dtype_str):
        """Create a shared memory array with the given parameters."""
        if not shape_str:
            self.show_error("Shape cannot be empty.")
            return
            
        try:
            shape = tuple(map(int, shape_str.split(',')))
            if len(shape) < 1 or len(shape) > 3:
                raise ValueError("Shape must have 1 to 3 dimensions.")
        except ValueError as e:
            self.show_error(f"Invalid shape format: {e}")
            return

        # Remove zero dimensions from shape
        shape = tuple(dim for dim in shape if dim > 0)
        if not shape:
            self.show_error("Shape cannot have zero dimensions.")
            return

        # Adjust shape based on dimensions
        if len(shape) == 1:
            shape = (shape[0], 1)
        
        # Create array and shared memory
        try:
            array = np.zeros(shape, dtype=dtype_str)
            filename = f'/tmp/{name}.im.shm'
            shm = dao.shm(filename, array)
            QMessageBox.information(self, "Success", f"Shared memory created with shape {shape} and dtype {dtype_str}")
            self.updateFileList()
        except Exception as e:
            self.show_error(f"Failed to create shared memory: {e}")

    def updateFileList(self):
        """Update the list of shared memory files."""
        # Save currently selected files
        selected_files = set()
        
        for row in range(self.tableWidget.rowCount()):
            checkBox = self.tableWidget.cellWidget(row, 0)
            if checkBox and checkBox.isChecked():
                filenameItem = self.tableWidget.item(row, 1)
                if filenameItem:
                    selected_files.add(filenameItem.text())

        # Get updated file list
        fileList = self.dir.entryInfoList(self.filters, QDir.Files)
        self.tableWidget.setRowCount(len(fileList))
        
        # Populate table
        filenames = []
        for row, fileInfo in enumerate(fileList):
            filename = fileInfo.fileName()
            filenames.append(filename)
            
            checkBox = QCheckBox(self)
            if filename in selected_files:
                checkBox.setChecked(True)
            # Connect checkbox state change to updateMultiRecordList
            checkBox.stateChanged.connect(self.updateMultiRecordList)
            filenameItem = QTableWidgetItem(filename)
            self.tableWidget.setCellWidget(row, 0, checkBox)
            self.tableWidget.setItem(row, 1, filenameItem)
        
        # Update multi-record list
        self.updateMultiRecordList()

    def onCellClicked(self, row, column):
        """Handle cell click in the file table."""
        if self.timer.isActive():           
            self.timer.stop()
            
        if self.shm: 
            self.shm.close()  # Properly close resources
            self.shm = None
            
            # Force garbage collection
            # time.sleep(0.1)
            gc.collect()
            
        filenameItem = self.tableWidget.item(row, 1)
        if not filenameItem:
            return
            
        try:
            # Get the filename and update UI
            filename = filenameItem.text()
            output_filename = filename.replace(".im.shm", ".npy")
            self.filenameEdit.setText(output_filename)
            
            # Open the shared memory
            print(f"Opening shared memory file: /tmp/{filename}")
            self.shm = dao.shm(f"/tmp/{filename}")
            self.lastCounter = self.shm.get_counter()
            
            # Update metadata and selected files list
            self.updateMetadata(filename)
            self.updateMultiRecordList()
            
            # Determine visualization type based on data shape
            data = self.shm.get_data()
            self.dataCounter = self.shm.get_counter()

            if np.shape(data) == (1, 1):
                self.TABLE = True
                self.FLAT = False
            elif np.min(data.shape) == 1:
                self.FLAT = True
                self.TABLE = False
            else:
                self.FLAT = False
                self.TABLE = False
                
            # Update visualization
            self.update_visualization()
            
            # Start update timer
            self.timer.start(100) 
            
            # Update status bar
            self.statusBar.showMessage(f"Loaded {filename}")
            
        except Exception as e:
            self.show_error(f"Error loading file: {e}")

    def update_visualization(self):
        """Update the visualization widget based on data type."""
        index = self.top_splitter.indexOf(self.graphWidget)
        if self.graphWidget:
            # Tell Qt to queue this widget for deletion when event processing returns to the main loop
            self.graphWidget = None
            # self.top_splitter.replaceWidget(index, None)
            # time.sleep(0.1)
            gc.collect()
            
        if self.TABLE or self.ShowTable:
            model = NumpyTableModel(self.shm.get_data(), shm=self.shm)
            self.graphWidget = QTableView()
            self.graphWidget.setModel(model)
        else:
            self.graphWidget = magicplot.MagicPlot()
            
        self.top_splitter.replaceWidget(index, self.graphWidget)
        
        # Update visualization data
        if not (self.TABLE or self.ShowTable):
            if self.FLAT:
                self.im = self.graphWidget.getDataItem()
                self.im.setData(self.shm.get_data().flatten())
            else:
                self.im = self.graphWidget.getImageItem()
                self.im.setData(self.shm.get_data())
                
            self.graphWidget.updatePanBounds()
            self.graphWidget.viewBox.autoRange()

    def on_timer_triggered(self):
        """Handle timer tick events for data updates."""
        if not self.shm:
            return
            
        try:
            self.newCounter = self.shm.get_counter()
            diff = self.newCounter - self.lastCounter
            self.lastCounter = self.newCounter
            
            # Calculate update frequency
            frequency = 0 if diff == 0 else 10/diff
            self.updateMetadata(self.filenameEdit.text(), frequency)
            
            # Update visualization if data changed
            if diff != 0:
                if self.TABLE or self.ShowTable:
                    self.graphWidget.setModel(NumpyTableModel(self.shm.get_data(), self.shm))
                else:
                    if self.FLAT:
                        self.im.setData(self.shm.get_data().flatten())
                    else:
                        self.im.setData(self.shm.get_data())
        except Exception as e:
            self.timer.stop()
            self.show_error(f"Error updating data: {e}")

    def record_file(self, filename, frames):
        """Record data to a file."""
        if not self.shm:
            self.show_error("No shared memory selected")
            return
            
        try:
            data = self.shm.get_data()
            
            # Handle single vs. multiple frames
            if frames == 1:
                buffer = data
            else:
                buffer = np.zeros((frames, *data.shape), dtype=data.dtype)
                self.statusBar.showMessage(f"Recording {frames} frames...")
                for i in range(frames):
                    buffer[i] = self.shm.get_data(check=True)
                    if i % 10 == 0:  # Update status every 10 frames
                        self.statusBar.showMessage(f"Recording frames: {i+1}/{frames}")
                    QApplication.processEvents()  # Keep UI responsive
            
            # Save to file
            np.save(filename, buffer)
            self.statusBar.showMessage(f"Data saved to {filename}")
            
        except Exception as e:
            self.show_error(f"Error saving data: {e}")

    def record_multiple_files(self):
        """Record multiple selected files."""
        output_dir = self.multiRecordFilenameEdit.text()
        if not output_dir:
            self.show_error("Please select an output directory")
            return
            
        frames = self.multiFrameCounter.value()
        if frames <= 0:
            self.show_error("Number of frames must be greater than 0")
            return
            
        # Get list of selected files
        selected_files = []
        for row in range(self.tableWidget.rowCount()):
            checkBox = self.tableWidget.cellWidget(row, 0)
            if checkBox and checkBox.isChecked():
                filenameItem = self.tableWidget.item(row, 1)
                if filenameItem:
                    selected_files.append(filenameItem.text())
        
        if not selected_files:
            self.show_error("No files selected")
            return
        
        # Get master file (currently selected item in multiRecordList)
        master_file = None
        if self.multiRecordList.currentItem():
            master_file = self.multiRecordList.currentItem().text()
            
        # Print the list of files with master file highlighted
        print("\nSelected Files for Recording:")
        for filename in selected_files:
            if filename == master_file:
                print(f" * {filename} (MASTER)")
            else:
                print(f"   {filename}")
        print(f"\nTotal files: {len(selected_files)}")
        
        if master_file:
            print(f"Master file: {master_file}")
        else:
            print("No master file selected")
            
        self.statusBar.showMessage(f"Files list printed to console. Recording not implemented yet.")

    def openFileDialog(self, event):
        """Open file dialog for saving."""
        options = QFileDialog.Options()
        fileName, _ = QFileDialog.getSaveFileName(
            self, "Save File", self.filenameEdit.text(), 
            "Numpy Files (*.npy)", options=options
        )
        if fileName:
            self.filenameEdit.setText(fileName)

    def openLoadFileDialog(self, event):
        """Open file dialog for loading."""
        options = QFileDialog.Options()
        fileName, _ = QFileDialog.getOpenFileName(
            self, "Load File", "", 
            "All Files (*);;Numpy Files (*.npy)", options=options
        )
        if fileName:
            self.loadFilenameEdit.setText(fileName)

    def openLoadSnapshotDialog(self, event):
        """Open file dialog for snapshot loading."""
        options = QFileDialog.Options()
        fileName, _ = QFileDialog.getOpenFileName(
            self, "Load snapshot", "", 
            "All Files (*);;dao_snapshot Files (*.dao)", options=options
        )
        if fileName:
            self.snapshot_FilenameEdit.setText(fileName)

    def openFolderDialog(self, event):
        """Open folder dialog for multi-record output."""
        options = QFileDialog.Options()
        folderName = QFileDialog.getExistingDirectory(self, "Select Folder", options=options)
        if folderName:
            self.multiRecordFilenameEdit.setText(folderName)

    def updateMetadata(self, filename, frequency=0):
        """Update the metadata display."""
        if not self.shm:
            return
            
        try:
            shape = self.shm.get_data().shape
            dtype = self.shm.get_data().dtype
            counter = 1
            # self.shm.get_counter()
            
            metadata = (
                f"Metadata for {filename}:\n"
                f"- Shape: {shape}\n"
                f"- Dtype: {dtype}\n"
                f"- Counter: {counter}\n"
                f"- Update Frequency: {frequency:.2f} Hz\n"
            )
            
            # Add statistics if data is numerical
            data = self.shm.get_data()
            if np.issubdtype(data.dtype, np.number):
                metadata += (
                    f"- Min: {np.min(data)}\n"
                    f"- Max: {np.max(data)}\n"
                    f"- Mean: {np.mean(data)}\n"
                    f"- Std Dev: {np.std(data)}\n"
                )
            
            self.metadataText.setText(metadata)
            
        except Exception as e:
            self.metadataText.setText(f"Error getting metadata: {e}")

    def updateMultiRecordList(self, *args):
        """Update the list of files for multi-recording.
        
        This function can be called directly or as a slot from a signal.
        """
        # Guard against calling before the widget is created
        if not hasattr(self, 'multiRecordList'):
            return
            
        # Remember the selected item
        current_item = self.multiRecordList.currentItem()
        current_text = current_item.text() if current_item else None
        
        self.multiRecordList.clear()
        
        # Get all currently checked files
        for row in range(self.tableWidget.rowCount()):
            checkBox = self.tableWidget.cellWidget(row, 0)
            if checkBox and checkBox.isChecked():
                filenameItem = self.tableWidget.item(row, 1)
                if filenameItem:
                    self.multiRecordList.addItem(filenameItem.text())
        
        # Restore previously selected item if still in list
        if current_text:
            items = self.multiRecordList.findItems(current_text, Qt.MatchExactly)
            if items:
                self.multiRecordList.setCurrentItem(items[0])
        
        # Update count in tab name
        if hasattr(self, 'tabWidget'):
            count = self.multiRecordList.count()
            self.tabWidget.setTabText(2, f"Multi-Record ({count})")

    def updateMasterFileDisplay(self):
        """Update the master file label based on the selected item."""
        current_item = self.multiRecordList.currentItem()
        if current_item:
            self.masterFileLabel.setText(f"Selected Files: (Master: {current_item.text()})")
        else:
            self.masterFileLabel.setText("Selected Files: (No master selected)")

    def loadFile(self):
        """Load a file into shared memory."""
        if not self.shm:
            self.show_error("No shared memory selected")
            return
            
        filename = self.loadFilenameEdit.text()
        if not filename:
            self.show_error("No file selected for loading")
            return
            
        try:
            # Load data based on file extension
            if filename.endswith('.npy'):
                data = np.load(filename)
            elif filename.endswith('.fits'):
                with fits.open(filename) as hdul:
                    data = hdul[0].data
            else:
                self.show_error("Unsupported file format")
                return
                
            # Verify shape and dtype compatibility
            shm_data = self.shm.get_data()
            if data.shape != shm_data.shape or data.dtype != shm_data.dtype:
                self.show_error(
                    f"Data mismatch: Expected shape {shm_data.shape}, dtype {shm_data.dtype}, "
                    f"but got shape {data.shape}, dtype {data.dtype}"
                )
                return
                
            # Load data into shared memory
            self.shm.set_data(data)
            self.statusBar.showMessage(f"Data loaded from {filename}")
            
        except Exception as e:
            self.show_error(f"Error loading file: {e}")

    def showSetDataDialog(self):
        """Show dialog for setting data values."""
        if not self.shm:
            self.show_error("No shared memory selected")
            return
            
        dialog = SetDataDialog(self)
        if dialog.exec_():
            self.applySetDataOperation(dialog.get_values())
    
    def applySetDataOperation(self, options):
        """Apply the selected data operation."""
        if not self.shm:
            return
            
        try:
            data = self.shm.get_data()
            mode = options["mode"]
            
            if mode == "set_value":
                # Set all data to a specific value
                value = options["value"]
                result = np.full_like(data, value)
                self.statusBar.showMessage(f"Data set to {value}")
                
            elif mode == "randomize":
                # Randomize data between min and max values
                min_val = options["min"]
                max_val = options["max"]
                
                # Use appropriate random function based on data type
                if np.issubdtype(data.dtype, np.integer):
                    result = np.random.randint(
                        low=int(min_val), 
                        high=int(max_val) + 1,  # +1 because randint upper bound is exclusive
                        size=data.shape,
                        dtype=data.dtype
                    )
                else:
                    result = np.random.uniform(
                        low=min_val,
                        high=max_val,
                        size=data.shape
                    ).astype(data.dtype)
                
                self.statusBar.showMessage(f"Data randomized between {min_val} and {max_val}")
            
            # Update the shared memory
            self.shm.set_data(result)
            
        except Exception as e:
            self.show_error(f"Error setting data: {e}")

    def toggle_view(self):
        """Toggle between table and graph views."""
        self.ShowTable = not self.ShowTable
        # self.tableButton.setText("Show as Graph" if self.ShowTable else "Show as Table")
        
        if self.shm:
            self.update_visualization()

    def snapshot_SaveFunction(self):
        """Save a snapshot of the system state."""
        filename = self.snapshot_FilenameEdit.text()
        if not filename:
            self.show_error("Please specify a snapshot filename")
            return
            
        try:
            process = subprocess.Popen(
                f'daoSnapshot.py create {filename}', 
                stdout=subprocess.PIPE, 
                stderr=subprocess.PIPE,
                text=True, 
                shell=True, 
                env=os.environ.copy()
            )
            output, error = process.communicate()
            
            if error:
                self.show_error(f"Error creating snapshot: {error}")
            else:
                self.statusBar.showMessage(f"Snapshot saved to {filename}")
                
        except Exception as e:
            self.show_error(f"Error saving snapshot: {e}")

    def snapshot_LoadFunction(self):
        """Load a system snapshot."""
        filename = self.snapshot_FilenameEdit.text()
        if not filename:
            self.show_error("Please specify a snapshot filename")
            return
            
        try:
            process = subprocess.Popen(
                f'daoSnapshot.py load {filename}', 
                stdout=subprocess.PIPE, 
                stderr=subprocess.PIPE,
                text=True, 
                shell=True, 
                env=os.environ.copy()
            )
            output, error = process.communicate()
            
            if error:
                self.show_error(f"Error loading snapshot: {error}")
            else:
                self.statusBar.showMessage(f"Snapshot loaded from {filename}")
                self.updateFileList()  # Refresh file list after loading snapshot
                
        except Exception as e:
            self.show_error(f"Error loading snapshot: {e}")

    def update_tmux_sessions(self):
        """Update the list of tmux sessions."""
        try:
            # Run tmux command to list all sessions
            process = subprocess.Popen(
                'tmux list-sessions -F "#{session_name}"',
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                shell=True
            )
            output, error = process.communicate()
            
            if process.returncode != 0:
                if "no server running" in error:
                    self.tmuxSessionList.clear()
                    self.statusBar.showMessage("No tmux server running")
                    return
                self.show_error(f"Error listing tmux sessions: {error}")
                return
                
            # Clear the list and add new sessions
            self.tmuxSessionList.clear()
            sessions = output.strip().split('\n')
            for session in sessions:
                if session:  # Skip empty lines
                    self.tmuxSessionList.addItem(session)
                    
            self.tabWidget.setTabText(5, f"Tmux Sessions ({len(sessions)})")
            self.statusBar.showMessage(f"Found {len(sessions)} tmux sessions")
            
        except Exception as e:
            self.show_error(f"Error updating tmux sessions: {e}")

    def connect_to_tmux_session(self):
        """Connect to the selected tmux session."""
        current_item = self.tmuxSessionList.currentItem()
        if not current_item:
            self.show_error("No tmux session selected")
            return
            
        session_name = current_item.text()
        try:
            # Different commands based on OS
            if sys.platform == "darwin":  # macOS
                # Launch a new Terminal window and connect to the tmux session
                cmd = f"osascript -e 'tell app \"Terminal\" to do script \"tmux attach -t {session_name}\"'"
            elif sys.platform.startswith("linux"):
                # Try to detect the terminal
                if os.environ.get('TERM_PROGRAM') == 'gnome-terminal':
                    cmd = f"gnome-terminal -- tmux attach -t {session_name}"
                elif os.environ.get('TERM_PROGRAM') == 'konsole':
                    cmd = f"konsole -e tmux attach -t {session_name}"
                else:
                    # Fallback to x-terminal-emulator
                    cmd = f"x-terminal-emulator -e tmux attach -t {session_name}"
            else:
                self.show_error("Unsupported OS for terminal launching")
                return

            # Execute the command to launch terminal
            subprocess.run(cmd, shell=True)
            self.statusBar.showMessage(f"Connected to tmux session: {session_name}")
            
        except Exception as e:
            self.show_error(f"Error connecting to tmux session: {e}")

    def on_tab_changed(self, index):
        """Handle tab change events."""
        if index == 5:  # Tmux Sessions tab
            self.update_tmux_sessions()

    def show_error(self, message):
        """Show an error message dialog."""
        QMessageBox.critical(self, "Error", message)
        self.statusBar.showMessage(f"Error: {message}", 5000)


def main():
    """Main application entry point."""
    app = QApplication(sys.argv)
    app.setApplicationDisplayName("DAO Shared Memory Viewer")
    app.setOrganizationName("DAO")
    
    # Set application icon
    dao_root = os.getenv('DAOROOT')
    if dao_root:
        icon_path = os.path.join(dao_root, 'data/daoLogo.png')
        if os.path.exists(icon_path):
            app.setWindowIcon(QIcon(icon_path))
    
    # Handle Ctrl+C keyboard interrupt
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    
    # Create and show main window
    viewer = daoShmViewer()
    viewer.show()
    
    sys.exit(app.exec_())


if __name__ == '__main__':
    main()