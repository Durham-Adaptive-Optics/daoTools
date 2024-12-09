import sys
from PyQt5.QtWidgets import (QApplication, QWidget, QVBoxLayout, QHBoxLayout, QTableWidget, QTableWidgetItem, 
                             QCheckBox, QHeaderView, QLineEdit, QPushButton, QFileDialog, QLabel, QSpinBox, 
                             QTabWidget, QSplitter, QFrame, QTextEdit, QListWidget, QMessageBox, QErrorMessage)
from PyQt5.QtCore import QDir, Qt, QTimer
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure
from PyQt5 import QtWidgets

import magicplot
from astropy.io import fits
import numpy as np
import dao


class daoShmViewer2(QWidget):
    def __init__(self):
        super().__init__()
        self.initUI()
        self.shm = None
        self.lastCounter = 0
        self.FLAT = False
        self.TABLE = False
        self.ShowTable = False

    def initUI(self):
        self.timer = QTimer()
        self.timer.timeout.connect(self.on_timer_triggered)
        mainLayout = QVBoxLayout(self)
        
        # Top layout: File table and Graph side by side
        self.topSplitter = QSplitter(Qt.Horizontal)
        
        # Left side: File table
        self.tableWidget = QTableWidget(self)
        self.tableWidget.setColumnCount(2)
        self.tableWidget.setHorizontalHeaderLabels(["Select", "Filename"])
        self.tableWidget.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        self.tableWidget.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        self.tableWidget.setSelectionBehavior(QTableWidget.SelectRows)
        self.tableWidget.cellClicked.connect(self.onCellClicked)
        
        dir = QDir("/tmp")
        filters = ["*.im.shm"]
        fileList = dir.entryInfoList(filters, QDir.Files)
        self.tableWidget.setRowCount(len(fileList))
        
        for row, fileInfo in enumerate(fileList):
            checkBox = QCheckBox(self)
            filenameItem = QTableWidgetItem(fileInfo.fileName())
            self.tableWidget.setCellWidget(row, 0, checkBox)
            self.tableWidget.setItem(row, 1, filenameItem)
        
        self.topSplitter.addWidget(self.tableWidget)
        
        # Right side: Graph
        self.graphWidget = magicplot.MagicPlot()
        self.topSplitter.addWidget(self.graphWidget)
        
        # Main splitter: Top layout and Bottom tab widget
        self.mainSplitter = QSplitter(Qt.Vertical)
        self.mainSplitter.addWidget(self.topSplitter)
        
        # Bottom: Tabbed and collapsible controls
        tabWidget = QTabWidget()
        tabWidget.setMinimumSize(400, 200)
        
        # Tab 1: Recording controls
        recordingTab = QWidget()
        recordingLayout = QVBoxLayout()
        
        self.filenameEdit = QLineEdit(self)
        self.filenameEdit.setReadOnly(True)
        self.filenameEdit.mousePressEvent = self.openFileDialog
        
        self.frameCounter = QSpinBox(self)
        self.frameCounter.setRange(1, 10000)
        
        self.recordButton = QPushButton("Record", self)
        self.snapshotButton = QPushButton("Snapshot", self)
        
        recordingLayout.addWidget(QLabel("Filename:", self))
        recordingLayout.addWidget(self.filenameEdit)
        recordingLayout.addWidget(QLabel("Number of Frames:", self))
        recordingLayout.addWidget(self.frameCounter)
        recordingLayout.addWidget(self.recordButton)
        self.recordButton.clicked.connect(lambda: self.record_file(self.filenameEdit.text() , self.frameCounter.value()))
        recordingLayout.addWidget(self.snapshotButton)
        
        recordingTab.setLayout(recordingLayout)
        tabWidget.addTab(recordingTab, "Recording")
        
        # Tab 2: File Metadata
        metadataTab = QWidget()
        metadataLayout = QVBoxLayout()
        
        self.metadataText = QTextEdit(self)
        self.metadataText.setReadOnly(True)
        
        metadataLayout.addWidget(QLabel("File Metadata:", self))
        metadataLayout.addWidget(self.metadataText)
        self.tableButton = QPushButton("Show as Table")
        self.tableButton.clicked.connect(self.showTable)
        metadataLayout.addWidget(self.tableButton)
        metadataTab.setLayout(metadataLayout)
        tabWidget.addTab(metadataTab, "Metadata")
        
        # Tab 3: Recording Multiple Files
        multiRecordTab = QWidget()
        multiRecordLayout = QVBoxLayout()
        
        self.multiRecordList = QListWidget(self)
        self.multiRecordFilenameEdit = QLineEdit(self)
        self.multiRecordFilenameEdit.setReadOnly(True)
        self.multiRecordFilenameEdit.mousePressEvent = self.openFolderDialog
        
        self.multiFrameCounter = QSpinBox(self)
        self.multiFrameCounter.setRange(0, 1000)
        
        self.multiRecordButton = QPushButton("Record", self)
        
        multiRecordLayout.addWidget(QLabel("Selected Files:", self))
        multiRecordLayout.addWidget(self.multiRecordList)
        multiRecordLayout.addWidget(QLabel("Save to Folder:", self))
        multiRecordLayout.addWidget(self.multiRecordFilenameEdit)
        multiRecordLayout.addWidget(QLabel("Number of Frames:", self))
        multiRecordLayout.addWidget(self.multiFrameCounter)
        multiRecordLayout.addWidget(self.multiRecordButton)
        
        multiRecordTab.setLayout(multiRecordLayout)
        tabWidget.addTab(multiRecordTab, "Multi-Record")
        
        # Tab 4: Load
        loadTab = QWidget()
        loadLayout = QVBoxLayout()
        
        self.loadFilenameEdit = QLineEdit(self)
        self.loadFilenameEdit.setReadOnly(True)
        self.loadFilenameEdit.mousePressEvent = self.openLoadFileDialog
        
        self.loadButton = QPushButton("Load", self)
        self.loadButton.clicked.connect(self.loadFile)
        
        self.zeroButton = QPushButton("Zero", self)
        self.zeroButton.clicked.connect(self.zeroFunction)
        
        loadLayout.addWidget(QLabel("Filename:", self))
        loadLayout.addWidget(self.loadFilenameEdit)
        loadLayout.addWidget(self.loadButton)
        loadLayout.addWidget(self.zeroButton)
        
        loadTab.setLayout(loadLayout)
        tabWidget.addTab(loadTab, "Load")
        
        self.mainSplitter.addWidget(tabWidget)
        # self.mainSplitter.setSizes([400, 200])  # Initial sizes
        
        mainLayout.addWidget(self.mainSplitter)
        self.setLayout(mainLayout)

    def onCellClicked(self, row, column):
        filenameItem = self.tableWidget.item(row, 1)
        print(f"Row {row} clicked with filename: {filenameItem.text()}")
        if filenameItem:
            filename = filenameItem.text().replace(".im.shm", ".npy")
            self.filenameEdit.setText(filename)
            # self.mainSplitter.setSizes([400, 200])  # Expand the bottom widget when a row is clicked
            self.shm = dao.shm(f"/tmp/{filenameItem.text()}")
            self.lastCounter = self.shm.get_counter()
            self.updateMetadata(filenameItem.text())
            self.updateMultiRecordList()
            
        data = self.shm.get_data()
        if np.shape(data) == (1,1):
            self.TABLE=True
        elif(np.min(data.shape) == 1):
            self.FLAT = True
            self.TABLE = False
        else:
            self.FLAT = False
            self.TABLE = False
            
        index = self.topSplitter.indexOf(self.graphWidget)
        if self.TABLE:
            self.graphWidget = QtWidgets.QTableWidget(1,1)
            self.graphWidget.setItem(0, 0, QTableWidgetItem(str(data[0, 0])))
            # Optional: Set text alignment for better readability
            self.graphWidget.item(0, 0).setTextAlignment(Qt.AlignCenter)

            # Optional: Customize the font or font size
            font = self.graphWidget.font()
            font.setPointSize(12)  # Set font size (adjust as needed)
            # Resize the column and row to fit the content
            self.graphWidget.setFont(font)
            self.graphWidget.resizeColumnsToContents()
            self.graphWidget.resizeRowsToContents()
            
        elif self.ShowTable:
            self.graphWidget = QtWidgets.QTableWidget(data.shape[0], data.shape[1])
            for i in range(data.shape[0]):
                for j in range(data.shape[1]):
                    self.graphWidget.setItem(i, j, QTableWidgetItem(str(data[i, j])))
                    # Optional: Set text alignment for better readability
                    self.graphWidget.item(i, j).setTextAlignment(Qt.AlignCenter)
            self.graphWidget.resizeColumnsToContents()
            self.graphWidget.resizeRowsToContents()
        else:
            self.graphWidget = magicplot.MagicPlot()
        
        self.topSplitter.replaceWidget(index, self.graphWidget)    
        
        if self.TABLE or self.ShowTable:
            pass
        else:
            if(self.FLAT == True):
                self.im = self.graphWidget.getDataItem()
                self.im.setData(self.shm.get_data().flatten())
            else:
                # self.graphWidget.plot(data)
                self.im = self.graphWidget.getImageItem()
                self.im.setData(self.shm.get_data())
           
            self.graphWidget.updatePanBounds()
            self.graphWidget.viewBox.autoRange()
        # Display metadata
        # self.show_metadata(file_path)
        
        self.timer.start(100)  # Set the timer to 5 seconds (5000 milliseconds)

    def on_timer_triggered(self):
        self.newCounter = self.shm.get_counter()

        diff = self.newCounter - self.lastCounter
        self.lastCounter = self.newCounter
        if diff == 0:
            frequency = 0
        else:
            frequency = 10/diff
        self.updateMetadata(self.filenameEdit.text(), frequency)
        if(self.TABLE):
            self.graphWidget.setItem(0, 0, QTableWidgetItem(str(self.shm.get_data()[0, 0])))
        elif self.ShowTable:
            data = self.shm.get_data()
            rows, cols = data.shape
            for i in range(rows):
                for j in range(cols):
                    value = str(data[i, j])  # Convert each value to a string
                    self.graphWidget.setItem(i, j, QTableWidgetItem(str(value)))

            # Resize the table cells to fit the content
            self.table_widget.resizeColumnsToContents()
            self.table_widget.resizeRowsToContents()
        else:   
            if(self.FLAT):
                self.im.setData(self.shm.get_data().flatten())
            else:
                self.im.setData(self.shm.get_data())   


    def record_file(self, filename, frames):
        # Placeholder for actual recording logic
        print(f"Recording file: {filename} with {frames} frames")
        data = self.shm.get_data()
        if frames == 1:
            buffer = self.shm.get_data()
        else:
            buffer = np.zeros((frames, *data.shape), dtype=data.dtype)
            for i in range(frames): 
                buffer[i] = self.shm.get_data(check=True)
            
        try:
            np.save(filename, buffer)
            print("Data saved successfully")
        except Exception as e:
            error_string = f"Error saving data: {e}"
            msg = QMessageBox()
            msg.setIcon(QMessageBox.Critical)
            msg.setText("Error")
            msg.setInformativeText(error_string)
            msg.setWindowTitle("Error")
            msg.exec_()
            return
        
    def openFileDialog(self, event):
        options = QFileDialog.Options()
        fileName, _ = QFileDialog.getSaveFileName(self, "Save File", self.filenameEdit.text(), "Numpy Files (*.npy)", options=options)
        if fileName:
            self.filenameEdit.setText(fileName)

    def openLoadFileDialog(self, event):
        options = QFileDialog.Options()
        fileName, _ = QFileDialog.getOpenFileName(self, "Load File", "", "All Files (*);;Numpy Files (*.npy)", options=options)
        if fileName:
            self.loadFilenameEdit.setText(fileName)

    def openFolderDialog(self, event):
        options = QFileDialog.Options()
        folderName = QFileDialog.getExistingDirectory(self, "Select Folder", options=options)
        if folderName:
            self.multiRecordFilenameEdit.setText(folderName)

    def updateMetadata(self, filename, frequency=0):
        shape = self.shm.get_data().shape
        dtype = self.shm.get_data().dtype
        counter = self.shm.get_counter()
        metadata = f"Metadata for {filename}:\n- Shape: {shape}\n- Dtype: {dtype}\n- Counter: {counter}\n - Frequency: {frequency}"
        # Placeholder for actual metadata extraction logic
        self.metadataText.setText(metadata)

    def updateMultiRecordList(self):
        self.multiRecordList.clear()
        for row in range(self.tableWidget.rowCount()):
            checkBox = self.tableWidget.cellWidget(row, 0)
            if checkBox.isChecked():
                filenameItem = self.tableWidget.item(row, 1)
                if filenameItem:
                    self.multiRecordList.addItem(filenameItem.text())

    def loadFile(self):
        
        if self.shm is None:
            msg = QMessageBox()
            msg.setIcon(QMessageBox.Critical)
            msg.setText("Error")
            msg.setInformativeText("No file selected. Please select a file to load.")
            msg.setWindowTitle("Error")
            msg.exec_()
            return
        
        filename = self.loadFilenameEdit.text()
        # Placeholder for actual file loading logic
        print(f"Loading file: {filename}")
        try:
            if filename.endswith('.npy'):
                data = self.loadNpyFile(filename)
            elif filename.endswith('.fits'):
                data = self.loadFitsFile(filename)
        except Exception as e:
            error_string = f"Error loading file: {e}"
            msg = QMessageBox()
            msg.setIcon(QMessageBox.Critical)
            msg.setText("Error")
            msg.setInformativeText(error_string)
            msg.setWindowTitle("Error")
            msg.exec_()
            return
            
        
        # check if data is not None and is correct shape.
        check = self.shm.get_data()
        dtype = check.dtype
        shape = check.shape
        if data is not None and data.shape == shape and data.dtype == dtype:
            self.shm.set_data(data)
            print("Data loaded successfully")
        else:
            error_string = f"Error loading data. Data shape or dtype does not match expected shape {shape} go {data.shape} and dtype {dtype} got {data.dtype}"
            msg = QMessageBox()
            msg.setIcon(QMessageBox.Critical)
            msg.setText("Error")
            msg.setInformativeText(error_string)
            msg.setWindowTitle("Error")
            msg.exec_()
            return
            
    def loadNpyFile(self, filename):
        # Placeholder for actual .npy file loading logic
        print(f"Loading .npy file: {filename}")
        data = np.load(filename)
        print(data)
        print(data.shape)
        print(data.dtype)
        return data

    def loadFitsFile(self, filename):
        # Placeholder for actual .fits file loading logic
        print(f"Loading .fits file: {filename}")
        # load fits file
        with fits.open(filename) as hdul:
            data = hdul[0].data
            print(data)
            print(data.shape)
            print(data.dtype)
            return data
        

    def zeroFunction(self):
        # Placeholder for zero function logic
        try:
            print("Zero function called")
            self.shm.set_data(self.shm.get_data()*0)
        except Exception as e:
            error_string = f"Error zeroing data: {e}"
            msg = QMessageBox()
            msg.setIcon(QMessageBox.Critical)
            msg.setText("Error")
            msg.setInformativeText(error_string)
            msg.setWindowTitle("Error")
            msg.exec_()
            return
    def showTable(self):
        if self.ShowTable==False:
            self.ShowTable = True
            self.tableButton.setText("Show as Graph")
        else:
            self.ShowTable = False
            self.tableButton.setText("Show as Table")
        self.onCellClicked(self.tableWidget.currentRow(), 0)

class GraphWidget(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.figure = Figure()
        self.canvas = FigureCanvas(self.figure)
        layout = QVBoxLayout(self)
        layout.addWidget(self.canvas)
        self.setLayout(layout)
        self.plot()

    def plot(self):
        ax = self.figure.add_subplot(111)
        ax.plot([0, 1, 2, 3], [10, 1, 20, 3])
        self.canvas.draw()

def main():
    app = QApplication(sys.argv)
    browser = daoShmViewer2()
    browser.show()
    sys.exit(app.exec_())

if __name__ == '__main__':
    main()