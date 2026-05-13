#!/usr/bin/env python3

################################################
#                  IMPORTS
################################################

import gc
import os
import signal
import subprocess
import sys

import dao
import yaml
import magicplot
import numpy as np
from astropy.io import fits
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure
from PyQt5.QtCore import QAbstractTableModel, QDir, Qt, QTimer
from PyQt5.QtGui import QIcon
from PyQt5.QtWidgets import (
    QAction, QApplication, QButtonGroup, QCheckBox, QComboBox, QDialog,
    QDoubleSpinBox, QFileDialog, QFormLayout, QGridLayout, QGroupBox,
    QHBoxLayout, QLabel, QLineEdit, QListWidget, QListWidgetItem,
    QMainWindow, QMessageBox, QPushButton, QRadioButton, QSpinBox,
    QStackedWidget, QStatusBar, QSplitter, QTabWidget, QTableView,
    QAbstractItemView, QTableWidgetItem, QTextEdit, QToolBar, QVBoxLayout, QWidget,
    QTreeWidget, QTreeWidgetItem
)

################################################
#               Custom Widgets
################################################

class DaqSessionConfig(QWidget):
    ''' PyQt5 widget for configuring a DAQ session '''
    
    def __init__(self): 
        super().__init__()
        self.createUI()
    
    def createUI(self):
        groupBox = QGroupBox("Required Parameters")
        layout = QFormLayout()
        
        self.pathDisplay = QLineEdit()
        self.pathDisplay.setReadOnly(True)
        
        selectBtn = QPushButton("Pick")
        selectBtn.clicked.connect(self.selectDirectory)
        
        inputLayout = QHBoxLayout()
        inputLayout.addWidget(self.pathDisplay)
        inputLayout.addWidget(selectBtn)
        
        layout.addRow("Root Storage:", inputLayout)
        
        groupBox.setLayout(layout)
        
        mainLayout = QVBoxLayout()
        mainLayout.addWidget(groupBox)
        self.setLayout(mainLayout)

    def selectDirectory(self):
        path = QFileDialog.getExistingDirectory(self, "Select Directory")
        if path:
            self.pathDisplay.setText(path)
            
class FileDaqConfig(QWidget):
    ''' PyQt5 widget for configuring a DAQ file resource '''
    
    def __init__(self, sourcePath: str): 
        super().__init__()
        self.source = sourcePath
        self.createUI()
    
    def createUI(self):
        groupBox = QGroupBox("Required Parameters")
        layout = QFormLayout()
        
        self.pathDisplay = QLineEdit(self.source)
        self.pathDisplay.setReadOnly(True)
        layout.addRow("File Path", self.pathDisplay)

        groupBox.setLayout(layout)
        mainLayout = QVBoxLayout()
        mainLayout.addWidget(groupBox)
        self.setLayout(mainLayout)


class SmemDaqConfig(QWidget):
    ''' PyQt5 widget for configuring a DAQ smem resource '''
    NUMERIC_MAX_SENTINEL: int = 999999999
    
    def __init__(self, sourcePath: str): 
        super().__init__()
        self.source = sourcePath
        self.createUI()
        
    def createUI(self):
        mainLayout = QVBoxLayout()
        
        requiredGroup = QGroupBox("Required Parameters")
        requiredLayout = QFormLayout()
        
        self.pathDisplay = QLineEdit(self.source)
        self.pathDisplay.setReadOnly(True)
        requiredLayout.addRow("Shared Memory Path:", self.pathDisplay)
        
        self.formatInput = QComboBox()
        self.formatInput.addItems(["numpy", "fits"])
        self.formatInput.setCurrentIndex(-1)
        requiredLayout.addRow("Format:", self.formatInput)
        
        requiredGroup.setLayout(requiredLayout)
        
        optionalGroup = QGroupBox("Optional Parameters")
        optionalLayout = QFormLayout()
        
        self.samplesInput = QSpinBox()
        self.samplesInput.setMinimum(-1)
        self.samplesInput.setMaximum(self.NUMERIC_MAX_SENTINEL)
        self.samplesInput.setValue(-1)
        optionalLayout.addRow("Sample Limit:", self.samplesInput)
        
        self.fileRolloverInput = QSpinBox()
        self.fileRolloverInput.setMinimum(-1)
        self.fileRolloverInput.setMaximum(self.NUMERIC_MAX_SENTINEL)
        self.fileRolloverInput.setValue(-1)
        optionalLayout.addRow("File Rollover:", self.fileRolloverInput)
        
        self.daqAffinityInput = QSpinBox()
        self.daqAffinityInput.setMinimum(-1)
        self.daqAffinityInput.setMaximum(self.NUMERIC_MAX_SENTINEL)
        self.daqAffinityInput.setValue(-1)
        optionalLayout.addRow("DAQ Affinity:", self.daqAffinityInput)
        
        self.sinkAffinityInput = QSpinBox()
        self.sinkAffinityInput.setMinimum(-1)
        self.sinkAffinityInput.setMaximum(self.NUMERIC_MAX_SENTINEL)
        self.sinkAffinityInput.setValue(-1)
        optionalLayout.addRow("Sink Affinity:", self.sinkAffinityInput)
        
        self.bufferLimitInput = QSpinBox()
        self.bufferLimitInput.setMinimum(-1)
        self.bufferLimitInput.setMaximum(self.NUMERIC_MAX_SENTINEL)
        self.bufferLimitInput.setValue(-1)
        optionalLayout.addRow("Buffer Limit:", self.bufferLimitInput)
        
        checkboxLayout = QHBoxLayout()
        self.metadataOnlyInput = QCheckBox("Metadata Only")
        self.metadataOnlyInput.setChecked(False)
        self.eagerStartInput = QCheckBox("Eager Start")
        self.eagerStartInput.setChecked(True)
        checkboxLayout.addWidget(self.metadataOnlyInput)
        checkboxLayout.addWidget(self.eagerStartInput)
        checkboxLayout.addStretch()
        optionalLayout.addRow("", checkboxLayout)
        
        optionalGroup.setLayout(optionalLayout)
        
        mainLayout.addWidget(requiredGroup)
        mainLayout.addWidget(optionalGroup)
        mainLayout.addStretch()
        
        self.setLayout(mainLayout)
            
class QuickRecordModal(QDialog):
    ''' Modal form to collect shared memory quick record options '''
    
    def __init__(self, UI: Qt.Widget, shmPath: str):
        super().__init__(UI)
        self.setWindowTitle("Shared Memory Quick Record")
        self.initUI(shmPath)
        
    def initUI(self, shmPath: str):
        # create input to display selected saveAs path
        # and configure to open file picker on click.
        shmName = shmPath.split("/")[1].split(".")[0]
        outputDir = os.getenv("DAODATA", os.getcwd())
        defaultSaveAs = f"{os.path.join(outputDir, shmName)}.npy"
        self.saveAsInput = QLineEdit(defaultSaveAs)
        self.saveAsInput.mousePressEvent = self.pickOutputPath
        self.saveAsInput.setReadOnly(True)
        
        # create numeric input for number of frames to record.
        self.frameCounter = QSpinBox()
        self.frameCounter.setRange(1, 10000)
        self.frameCounter.setValue(1)
        
        # create button to allow user to complete the form.
        doneBtn = QPushButton("Done")
        doneBtn.clicked.connect(self.finish)

        # create label for showing errors in the modal.
        self.statusLabel = QLabel()
        self.statusLabel.hide()

        # assemble widgets into a modal form.
        form = QFormLayout()
        form.addRow("Source", QLabel(shmPath))
        form.addRow("Save as", self.saveAsInput)
        form.addRow("Frames to record", self.frameCounter)
        form.addRow(self.statusLabel)
        form.addRow(doneBtn)
        self.setMinimumWidth(300)
        self.setLayout(form)
        
    def pickOutputPath(self, mouseEvent):
        fileName, _ = QFileDialog.getSaveFileName(
            self, "Save As", os.getcwd(), "Numpy Files (*.npy)"
        )
        if fileName:
            self.saveAsInput.setText(fileName)
        
    def finish(self):
        # deny form completion if user has not provided saveAs path.
        if not self.saveAsInput.text():
            self.statusLabel.setText("You must pick a SaveAs path before you can quick record.")
            self.statusLabel.show()
            return
        
        # Close modal with success exit code - recording can then
        # be triggered as all fields are configured.
        self.accept()
        
    def getFrameCount(self):
        return self.frameCounter.value()
    
    def getSavePath(self):
        return self.saveAsInput.text()

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

################################################
#           daoShmViewer UI
################################################

class daoShmViewer(QMainWindow):
    """Main application window for the DAO Shared Memory Viewer."""
    def __init__(self):
        super().__init__()
        self.setWindowTitle("DAO Shared Memory Viewer")
        self.init_variables()
        self.init_ui()
        self.setup_timers()
        self.setup_connections()
        self.slice_selector = None  # Add slice selector variable
        self.slice_widget = None  # Add slice widget container variable
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
        self.current_slice = 0
        self.is_3d = False
        
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
        self.top_splitter.addWidget(self.file_browser_widget)
        
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
        # Create a container widget for the file browser section
        file_browser_widget = QWidget()
        file_browser_layout = QVBoxLayout(file_browser_widget)
        file_browser_layout.setContentsMargins(0, 0, 0, 0)
        
        # Add search filter
        self.search_filter = QLineEdit()
        self.search_filter.setPlaceholderText("Search files...")
        self.search_filter.textChanged.connect(self.filter_files)
        file_browser_layout.addWidget(self.search_filter)
        
        self.file_list_widget = QListWidget(self)
        self.file_list_widget.setSelectionMode(QAbstractItemView.ExtendedSelection)
        
        # Add keyboard navigation support
        self.file_list_widget.keyPressEvent = self.shmListKeyPressEvent
        
        # Add table to layout
        file_browser_layout.addWidget(self.file_list_widget)
        
        # Store reference to the container widget
        self.file_browser_widget = file_browser_widget
        
        # Initial file listing
        self.updateFileList()

    def shmListKeyPressEvent(self, event):
        """Handle key press events in the tableWidget for navigation.
        
        This allows up/down arrows to change the selected row and automatically
        trigger the same behavior as if the cell was clicked.
        """
        # Get the current row
        currentRow = self.file_list_widget.currentRow()
        
        # Handle Up/Down arrow keys
        if event.key() == Qt.Key_Up and currentRow > 0:
            # Move to the row above
            newRow = currentRow - 1
            self.file_list_widget.setCurrentRow(newRow)
            self.onShmClicked(self.file_list_widget.item(newRow))
        elif event.key() == Qt.Key_Down and currentRow < self.file_list_widget.count() - 1:
            # Move to the row below
            newRow = currentRow + 1
            self.file_list_widget.setCurrentRow(newRow)
            self.onShmClicked(self.file_list_widget.item(newRow))
        else:
            # For all other keys, use the default event handler
            super(self.file_list_widget.__class__, self.file_list_widget).keyPressEvent(event)

    def setup_tab_widget(self):
        """Setup the tabbed control panel."""
        self.tabWidget = QTabWidget()
        self.tabWidget.setMinimumSize(400, 300)  # Increased minimum height from 200 to 300
        self.tabWidget.setTabsClosable(True)
        self.tabWidget.setMovable(True)
        self.tabWidget.setTabsClosable(False)
        
        # Tab 1: File Metadata
        self.setup_metadata_tab()
        
        # Tab 2: Recording controls
        self.setup_daq_tab()
        
        # Tab 3: Load
        self.setup_load_tab()
        
        # Tab 4: Snapshot
        self.setup_snapshot_tab()
        
        # Tab 5: Tmux Sessions
        self.setup_tmux_tab()

    def setup_daq_tab(self):
        """Setup the recording tab."""
        
        self.daqSourceNames = []
        self.daqWidgetArea = QStackedWidget()
        self.daqView = QTreeWidget()
        self.daqView.header().hide()
        self.daqView.itemClicked.connect(self.displayDaqConfig)
        self.initDaqConfig()
        
        # create (bottom) action buttons
        buttonLayout = QHBoxLayout()

        self.addDaqSrcBtn = QPushButton("Add File")
        self.addDaqSrcBtn.clicked.connect(self.daqAddBtnCallback)
        buttonLayout.addWidget(self.addDaqSrcBtn)

        delDaqSrcBtn = QPushButton("Delete")
        delDaqSrcBtn.clicked.connect(self.deleteDaqSrc)
        buttonLayout.addWidget(delDaqSrcBtn)

        clearDaqSrcsBtn = QPushButton("Clear")
        clearDaqSrcsBtn.clicked.connect(self.initDaqConfig)
        buttonLayout.addWidget(clearDaqSrcsBtn)
        
        importBtn = QPushButton("Import")
        importBtn.clicked.connect(self.importDaqConfig)
        buttonLayout.addWidget(importBtn)

        exportBtn = QPushButton("Export")
        exportBtn.clicked.connect(self.exportDaqConfig)
        buttonLayout.addWidget(exportBtn)
        
        self.daqBtn = QPushButton("Acquire")
        # self.daqBtn.clicked.connect(self.acquireData)
        buttonLayout.addWidget(self.daqBtn)

        # assemble widgets into a new tab
        hLayout = QHBoxLayout()
        hLayout.addWidget(self.daqView, 1)
        hLayout.addWidget(self.daqWidgetArea, 3)
        
        layout = QVBoxLayout()
        layout.addLayout(hLayout)
        layout.addLayout(buttonLayout)
        
        widget = QWidget()
        widget.setLayout(layout)
        self.tabWidget.addTab(widget, "DAQ")
        
    def setup_metadata_tab(self):
        """Setup the metadata tab."""
        metadataTab = QWidget()
        metadataLayout = QVBoxLayout()
        
        self.metadataText = QTextEdit()
        self.metadataText.setReadOnly(True)
        
        metadataLayout.addWidget(self.metadataText)
        metadataLayout.addStretch()
        metadataTab.setLayout(metadataLayout)
        self.tabWidget.addTab(metadataTab, "Metadata")

    def setup_load_tab(self):
        """Setup the load tab."""
        loadTab = QWidget()
        loadForm = QFormLayout()
        
        self.loadFilenameEdit = QLineEdit()
        self.loadFilenameEdit.setReadOnly(True)
        self.loadFilenameEdit.mousePressEvent = self.openLoadFileDialog
        
        self.loadButton = QPushButton("Load")
        self.zeroButton = QPushButton("Zero")
        
        loadForm.addRow("Filename", self.loadFilenameEdit)
        loadForm.addWidget(self.loadButton)
        
        loadTab.setLayout(loadForm)
        self.tabWidget.addTab(loadTab, "Load")

    def setup_snapshot_tab(self):
        """Setup the snapshot tab."""
        snapShotTab = QWidget()
        snapShotForm = QFormLayout()
        
        self.snapshot_FilenameEdit = QLineEdit()
        self.snapshot_FilenameEdit.setReadOnly(True)
        self.snapshot_FilenameEdit.mousePressEvent = self.openLoadSnapshotDialog
        
        btnLayout = QHBoxLayout()
        self.snapshot_SaveButton = QPushButton("Save Snapshot")
        self.snapshot_LoadButton = QPushButton("Load Snapshot")
        btnLayout.addWidget(self.snapshot_LoadButton)
        btnLayout.addWidget(self.snapshot_SaveButton)

        snapShotForm.addRow("Filename", self.snapshot_FilenameEdit)
        snapShotForm.addRow(btnLayout)
        
        snapShotTab.setLayout(snapShotForm)
        self.tabWidget.addTab(snapShotTab, "Snapshot")

    def setup_tmux_tab(self):
        """Setup the tmux sessions tab."""
        tmuxTab = QWidget()
        tmuxLayout = QVBoxLayout()
        
        self.tmuxSessionList = QListWidget()
        self.tmuxConnectButton = QPushButton("Connect to Session")
        self.tmuxConnectButton.clicked.connect(self.connect_to_tmux_session)
        
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
        self.file_list_widget.itemClicked.connect(self.onShmClicked)
        self.file_list_widget.selectionModel().selectionChanged.connect(self.onShmSelectionChanged)
        self.loadButton.clicked.connect(self.loadFile)
        self.snapshot_SaveButton.clicked.connect(self.snapshot_SaveFunction)
        self.snapshot_LoadButton.clicked.connect(self.snapshot_LoadFunction)
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
            shm = dao.shm(filename, array, logLevel=0)
            QMessageBox.information(self, "Success", f"Shared memory created with shape {shape} and dtype {dtype_str}")
            self.updateFileList()
        except Exception as e:
            self.show_error(f"Failed to create shared memory: {e}")

    def updateFileList(self):
        """Update the list of shared memory files."""
        self.file_list_widget.clear()
        
        # Get updated file list
        fileList = self.dir.entryInfoList(self.filters, QDir.Files)
        
        # Populate table
        for fileInfo in fileList:
            filename = fileInfo.fileName()
            self.file_list_widget.addItem(filename)
        
        # Apply current search filter
        self.filter_files()

    def onShmSelectionChanged(self):
        """ Handle when the user changes the cell selected in the shm table """
        numShmsSelected = len(self.file_list_widget.selectionModel().selectedRows())
        
        # update recording tab buttons based on active shm selection.
        if numShmsSelected == 1:
            self.daqBtn.setText("Quick Acquire")
            self.addDaqSrcBtn.setText("Add Shm")
        elif numShmsSelected > 1:
            self.addDaqSrcBtn.setText("Add Shms")
            self.daqBtn.setText("Acquire")
        else:
            self.addDaqSrcBtn.setText("Add File")
            
    def onShmClicked(self, item):
        """Handle cell click in the file table."""
        if self.timer.isActive():           
            self.timer.stop()
            
        if self.shm: 
            self.shm.close()  # Properly close resources
            self.shm = None
            
            # Force garbage collection
            # time.sleep(0.1)
            gc.collect()
            
        try:
            filename = item.text()
            
            # Open the shared memory
            self.shm = dao.shm(f"/tmp/{filename}", logLevel=0)
            self.lastCounter = self.shm.get_counter()
            
            # Update metadata and selected files list
            self.updateMetadata(filename)
            # self.updateMultiRecordList()
            
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
        if self.shm is None:
            return

        # Remove existing slice selector if it exists
        if hasattr(self, 'slice_widget') and self.slice_widget is not None:
            self.statusBar.removeWidget(self.slice_widget)
            self.slice_widget = None
            self.slice_selector = None

        index = self.top_splitter.indexOf(self.graphWidget)
        if self.graphWidget:
            self.graphWidget = None
            gc.collect()

        if self.TABLE or self.ShowTable:
            model = NumpyTableModel(self.shm.get_data(), shm=self.shm)
            self.graphWidget = QTableView()
            self.graphWidget.setModel(model)
        else:
            self.graphWidget = magicplot.MagicPlot()
            
        self.top_splitter.replaceWidget(index, self.graphWidget)

        # Check if data is 3D and set up slice selector
        data = self.shm.get_data()
        self.is_3d = len(data.shape) == 3

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

        # Update visualization data
        if not (self.TABLE or self.ShowTable):
            if self.FLAT:
                self.im = self.graphWidget.getDataItem()
                self.im.setData(self.shm.get_data().flatten())
            else:
                self.im = self.graphWidget.getImageItem()
                if self.is_3d:
                    self.im.setData(self.shm.get_data()[self.current_slice])
                else:
                    self.im.setData(self.shm.get_data())
                
            self.graphWidget.updatePanBounds()
            self.graphWidget.viewBox.autoRange()

    def update_slice(self, value):
        """Update the displayed slice for 3D data."""
        if self.shm and self.is_3d and not (self.TABLE or self.ShowTable):
            self.current_slice = value
            self.im.setData(self.shm.get_data()[value])
            self.graphWidget.updatePanBounds()

    def on_timer_triggered(self):
        """Handle timer tick events for data updates."""
        if not self.shm:
            return
            
        try:
            self.newCounter = self.shm.get_counter()
            diff = self.newCounter - self.lastCounter
            self.lastCounter = self.newCounter
            
            shmSource = self.shm.get_meta_data()["name"].decode("utf-8")
            frequency = 0 if diff == 0 else 10*diff
            self.updateMetadata(shmSource, frequency)
            
            if diff != 0:
                if self.TABLE or self.ShowTable:
                    self.graphWidget.setModel(NumpyTableModel(self.shm.get_data(), self.shm))
                else:
                    if self.FLAT:
                        self.im.setData(self.shm.get_data().flatten())
                    else:
                        data = self.shm.get_data()
                        if self.is_3d:
                            self.im.setData(data[self.current_slice])
                        else:
                            self.im.setData(data)
        except Exception as e:
            self.timer.stop()
            self.show_error(f"Error updating data: {e}")

    # @todo(tom) 'quick record' can be removed once I have added numpy support to the Dao DAQ tool.
    def record_file(self, filename, frames):
        """ Perform a quick record of the shm - this saved N samples to a numpy file """
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

    def daqAddSource(self, configUI):
        '''  Adds shared-memory(s) or a file to the DAQ configuration tab '''
        self.daqWidgetArea.addWidget(configUI)
        daqViewItem = QTreeWidgetItem(self.daqViewSrcsLeaf, [configUI.source])
        daqViewItem.setData(0, Qt.UserRole, self.daqWidgetArea.indexOf(configUI))
        self.daqSourceNames.append(configUI.source)
        self.daqView.expandItem(self.daqViewSrcsLeaf)
    
    def daqAddBtnCallback(self):
        ''' Adds shared-memory(s) or a file to the DAQ configuration tab from UI '''
        selectedShms = self.file_list_widget.selectedItems()
        if len(selectedShms):
            for shmItem in selectedShms:
                shmSource = f"/tmp/{shmItem.text()}"
                if not shmSource in self.daqSourceNames:
                    configUI = SmemDaqConfig(shmSource)
                    self.daqAddSource(configUI)
        else:
            fileSource, _ = QFileDialog.getOpenFileName(self, "Select File", os.getcwd())
            if fileSource and (fileSource not in self.daqSourceNames):
                configUI = FileDaqConfig(fileSource)
                self.daqAddSource(configUI)

    def displayDaqConfig(self, item, column):
        ''' Presents the config UI of the selected DAQ item '''
        index = item.data(0, Qt.UserRole)
        if index != None:
            self.daqWidgetArea.setCurrentIndex(index)

    def initDaqConfig(self):
        ''' Ensures all DAQ config UI state is reset and then restores back to initial state '''
        # wipe config UIs.
        uiWidgets = [self.daqWidgetArea.widget(i) for i in range(self.daqWidgetArea.count())]
        for w in uiWidgets: self.daqWidgetArea.removeWidget(w)

        # reset DAQ view.
        self.daqView.clear()
        
        # reset source names list
        self.daqSourceNames.clear()
        
        # reset to initial state 
        sessionConfigUI = DaqSessionConfig()
        self.daqWidgetArea.addWidget(sessionConfigUI)
        QTreeWidgetItem(self.daqView, ["DAQ Session"]).setData(0, Qt.UserRole, self.daqWidgetArea.indexOf(sessionConfigUI))
        self.daqViewSrcsLeaf = QTreeWidgetItem(self.daqView, ["DAQ Sources"])

    def deleteDaqSrc(self):
        # ensure a DAQ source was selected to delete.
        if self.daqView.selectedItems() == 0:
            return
        
        item = self.daqView.selectedItems()[0]
        if item.parent() != self.daqViewSrcsLeaf:
            return
        
        # delete its UI widget.
        index = item.data(0, Qt.UserRole)
        w = self.daqWidgetArea.widget(index)
        self.daqWidgetArea.removeWidget(w)
        
        # remove source from lookup
        self.daqSourceNames.remove(w.source)
        
        # rebuild view to obtain correct indicies.
        self.daqViewSrcsLeaf.takeChildren()
        for  i in range(1, self.daqWidgetArea.count()):
            w = self.daqWidgetArea.widget(i)
            QTreeWidgetItem(self.daqViewSrcsLeaf, [w.source]).setData(0, Qt.UserRole, self.daqWidgetArea.indexOf(w))

    def exportDaqConfig(self):
        ''' Allows the user to select a output location and saves the current DAQ config to disk '''
        filepath, _ = QFileDialog.getSaveFileName(
            self,
            "Save DAQ Config File",
            "",
            "YAML Files (*.yml *.yaml);;All Files (*)"
        )
        
        if not filepath:
            return
        
        try:
            config = {"sources": []}
            
            for i in range(self.daqWidgetArea.count()):
                configUI = self.daqWidgetArea.widget(i)
                
                if isinstance(configUI, DaqSessionConfig):
                    config["root_storage"] = configUI.pathDisplay.text()
                elif isinstance(configUI, FileDaqConfig):
                    config["sources"].append({
                        # required params
                        "uri": f"file://{configUI.pathDisplay.text()}"
                    })
                elif isinstance(configUI, SmemDaqConfig):
                    params = {
                        # required params
                        "uri": f"smem://{configUI.pathDisplay.text()}",
                        "format": configUI.formatInput.currentText(),
                        
                        # optional params
                        "metadata_only": configUI.metadataOnlyInput.isChecked(),
                        "eager_start": configUI.eagerStartInput.isChecked()
                    }

                    # omit parameters from export that aren't specified in UI.
                    optional = lambda val: val if val != -1 else None
                    if (param := optional(configUI.samplesInput.value())) and (param is not None):      params["samples"] = param
                    if (param := optional(configUI.fileRolloverInput.value())) and (param is not None): params["file_rollover"] = param
                    if (param := optional(configUI.daqAffinityInput.value())) and (param is not None):  params["daq_affinity"] = param
                    if (param := optional(configUI.sinkAffinityInput.value())) and (param is not None): params["sink_affinity"] = param
                    if (param := optional(configUI.bufferLimitInput.value())) and (param is not None):  params["buffer_limit"] = param

                    #
                    config["sources"].append(params)
                else:
                    raise RuntimeError(f"unknown config widget {configUI}")
                
            yaml.safe_dump(config, open(filepath, "w"))
            
        except Exception as e:
            self.show_error(f"failed to export DAQ configuration: {e}")
            return

    def importDaqConfig(self):
        ''' Allows the user to select a DAQ config file and loads it into the DAQ UI tab '''
        filepath, _ = QFileDialog.getOpenFileName(
            self,
            "Select DAQ Config File",
            "",
            "YAML Files (*.yml *.yaml);;All Files (*)"
        )
        
        if not filepath:
            return
        
        self.initDaqConfig() # wipes current config to clean slate.
        
        try:
            with open(filepath, "r") as file:
                config = yaml.safe_load(file)
                
                # load DAQ session config.
                node = config
                sessionUI = self.daqWidgetArea.widget(0)
                sessionUI.pathDisplay.setText(node["root_storage"])
                
                # load each DAQ source config
                node = config["sources"]
                for sourceConfig in node:
                    optional = lambda name: sourceConfig[name] if name in sourceConfig else None
                        
                    uri = sourceConfig["uri"]
                    uriClass, uriPath = uri.split("://")
                    if uriClass == "smem":
                        # required params..
                        smemUI = SmemDaqConfig(uriPath)
                        smemUI.formatInput.setCurrentText(sourceConfig["format"])
                        
                        # optional params..
                        metadataOnlyParam = optional("metadata_only")
                        samplesParam = optional("samples")
                        fileRolloverParam = optional("file_rollover")
                        daqAffinityParam = optional("daq_affinity")
                        sinkAffinityParam = optional("sink_affinity")
                        bufferLimitParam = optional("buffer_limit")
                        eagerStartParam = optional("eager_start")
                        
                        if metadataOnlyParam != None:   smemUI.metadataOnlyInput.setChecked(metadataOnlyParam)
                        if samplesParam != None:        smemUI.samplesInput.setValue(samplesParam)
                        if fileRolloverParam != None:   smemUI.fileRolloverInput.setValue(fileRolloverParam)
                        if daqAffinityParam != None:    smemUI.daqAffinityInput.setValue(daqAffinityParam)
                        if sinkAffinityParam != None:   smemUI.sinkAffinityInput.setValue(sinkAffinityParam)
                        if bufferLimitParam != None:    smemUI.bufferLimitInput.setValue(bufferLimitParam)
                        if eagerStartParam != None:     smemUI.eagerStartInput.setChecked(eagerStartParam)
                    
                        self.daqAddSource(smemUI)
                    elif uriClass == "file":
                        # required params..
                        fileUI = FileDaqConfig(uriPath)

                        self.daqAddSource(fileUI)
                    else:
                        raise RuntimeError(f"invalid URI class '{uriClass}' for source '{uri}'")
                
        except Exception as e:
            self.show_error(f"failed to import DAQ configuration: {e}")
            return

    # def acquireData(self):
    #     ''' Performs a quick-record or full telemetry record depending on UI state'''
        
    #     numSelectedShms = len(self.file_list_widget.selectedItems())
    #     if numSelectedShms == 1: 
    #         # single shm selected so capture quick-record
    #         # parameters and execute quick-record.
    #         shmSource = self.shm.get_meta_data()["name"].decode("utf-8")
    #         inputForm = QuickRecordModal(self, shmSource)
    #         if QDialog.Accepted == inputForm.exec_() :
    #             self.record_file(inputForm.getSavePath(), inputForm.getFrameCount())
    #     else:
    #         # invoke recording session with daoTelemetry
    #         # using the current recording configuration.
    #         QMessageBox.critical(self, "Telemetry Record Error", "TODO") # @todo(tom)
       
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
            counter = self.shm.get_counter()
            
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
                    # Get shared memory properties for matching
                    shm_data = self.shm.get_data()
                    shm_shape = shm_data.shape
                    shm_base_dtype = np.dtype(shm_data.dtype.kind + str(shm_data.dtype.itemsize))
                    
                    # Search through all HDUs to find matching data
                    data = None
                    matched_hdu_index = None
                    
                    for i, hdu in enumerate(hdul):
                        if hdu.data is not None and hasattr(hdu.data, 'shape'):
                            hdu_shape = hdu.data.shape
                            hdu_base_dtype = np.dtype(hdu.data.dtype.kind + str(hdu.data.dtype.itemsize))
                            
                            # Check if this HDU matches our requirements
                            if hdu_shape == shm_shape and hdu_base_dtype == shm_base_dtype:
                                data = hdu.data
                                matched_hdu_index = i
                                break
                    
                    if data is None:
                        # If no exact match found, provide detailed error information
                        error_msg = f"No matching HDU found in FITS file.\n"
                        error_msg += f"Required: shape {shm_shape}, dtype {shm_data.dtype}\n\n"
                        error_msg += "Available HDUs:\n"
                        
                        for i, hdu in enumerate(hdul):
                            if hdu.data is not None and hasattr(hdu.data, 'shape'):
                                error_msg += f"  HDU {i}: shape {hdu.data.shape}, dtype {hdu.data.dtype}\n"
                            else:
                                error_msg += f"  HDU {i}: No data or invalid shape\n"
                        
                        self.show_error(error_msg)
                        return
                    
                    # Show which HDU was used
                    self.statusBar.showMessage(f"Loading from HDU {matched_hdu_index} in {filename}")
            else:
                self.show_error("Unsupported file format")
                return
                
            # For FITS files, we've already verified compatibility during HDU selection
            # For other files, verify shape and dtype compatibility
            if not filename.endswith('.fits'):
                shm_data = self.shm.get_data()
                
                # Check shape compatibility
                if data.shape != shm_data.shape:
                    self.show_error(
                        f"Shape mismatch: Expected shape {shm_data.shape}, "
                        f"but got shape {data.shape}"
                    )
                    return
                
                # Check dtype compatibility (handle endianness differences)
                # Convert both dtypes to their basic type for comparison
                data_base_dtype = np.dtype(data.dtype.kind + str(data.dtype.itemsize))
                shm_base_dtype = np.dtype(shm_data.dtype.kind + str(shm_data.dtype.itemsize))
                
                if data_base_dtype != shm_base_dtype:
                    self.show_error(
                        f"Data type mismatch: Expected dtype {shm_data.dtype}, "
                        f"but got dtype {data.dtype}"
                    )
                    return
            
            # Convert data to match shared memory dtype if needed
            shm_data = self.shm.get_data()
            if data.dtype != shm_data.dtype:
                data = data.astype(shm_data.dtype)
                
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

    def filter_files(self):
        """Filter the file table based on search text."""
        # Guard against calling before search_filter is created
        if not hasattr(self, 'search_filter') or self.search_filter is None:
            return
            
        search_text = self.search_filter.text().lower()
        
        for row in range(self.file_list_widget.count()):
            filenameItem = self.file_list_widget.item(row)
            if filenameItem:
                filename = filenameItem.text().lower()
                # Show row if search text is empty or if filename contains search text
                show_row = not search_text or search_text in filename
                self.file_list_widget.setRowHidden(row, not show_row)

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

if __name__ == "__main__":
    main()