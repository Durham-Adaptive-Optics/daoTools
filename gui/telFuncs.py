'''
 # @ Author: Thomas N. Davies
 # @ Company: Centre for Advanced Instrumentation, Durham University
 # @ Contact: thomas.n.davies@durham.ac.uk
 # @ Create Time: 2026-04-21 11:31:32
 # @ Description:
 '''

from PyQt5.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QFormLayout, QGroupBox,
    QCheckBox, QLineEdit, QPushButton, QFileDialog, QLabel, QSpinBox,
    QListWidget, QListWidgetItem, QComboBox, QStackedWidget, QMessageBox,
    QDialog
)
from PyQt5.QtCore import Qt
from daoCommandIfce import daoCommandIfce
import yaml
import os

class FileConfig(QWidget):
    ''' Implements a PyQt5 widget for configuring a file telemetry item. '''
    
    def __init__(self, source: str, removeTelemetry: callable):
        super().__init__()
        self.source = source
        self.createUI(removeTelemetry)
    
    # --- UI ---
    
    def createUI(self, removeTelemetry: callable):
        layout = QVBoxLayout(self)
        layout.addWidget(self.createResourceGroup())
        layout.addWidget(self.createExportGroup())
        layout.addWidget(self.removeBtn(removeTelemetry))
        self.setLayout(layout)
        
    def createResourceGroup(self):
        group = QGroupBox("Resource Details")
        form = QFormLayout()
        
        form.addRow("Type", QLabel("File"))
        group.setLayout(form)
        
        source_field = QLineEdit(self.source)
        source_field.setReadOnly(True)
        form.addRow("Source", source_field)
        return group
    
    def createExportGroup(self):
        group = QGroupBox("Export Options")
        form = QFormLayout()
        
        self.output_name = QLineEdit()
        form.addRow("Name", self.output_name)
        
        group.setLayout(form)
        return group
    
    def removeBtn(self, removeTelemetry: callable):
        btn = QPushButton("Remove")
        btn.clicked.connect(lambda: removeTelemetry(self))
        return btn
    
     # --- Functionality ---
    
    def generateConfig(self):
        return {
            "source": f"file://{self.source}",
            "name": self.output_name.text() if self.output_name.text() else None 
        }

class ShmConfig(QWidget):
    ''' Implements a PyQt5 widget for configuring a shared memory telemetry item. '''

    def __init__(self, source: str, removeTelemetry: callable):
        super().__init__()
        self.source = source
        self.createUI(removeTelemetry)
        
    # --- UI ---
    
    def createUI(self, removeTelemetry: callable):
        layout = QVBoxLayout(self)
        layout.addWidget(self.createResourceGroup())
        layout.addWidget(self.createExportGroup())
        layout.addWidget(self.createAcquisitionGroup())
        layout.addWidget(self.removeBtn(removeTelemetry))
        self.setLayout(layout)

    def removeBtn(self, removeTelemetry: callable):
        btn = QPushButton("Remove")
        btn.clicked.connect(lambda: removeTelemetry(self))
        return btn

    def createResourceGroup(self):
        group = QGroupBox("Resource Details")
        form = QFormLayout()
       
        form.addRow("Type", QLabel("Shared Memory"))
        
        source_field = QLineEdit(self.source)
        source_field.setReadOnly(True)
        form.addRow("Source", source_field)
        
        group.setLayout(form)
        return group
    
    def createExportGroup(self):
        group = QGroupBox("Export Options")
        form = QFormLayout()
        
        self.name_field = QLineEdit()
        form.addRow("Name", self.name_field)
        
        self.format_combo = QComboBox()
        self.format_combo.addItems(["fits", "numpy"])
        self.format_combo.setCurrentText("fits")
        form.addRow("Format", self.format_combo)
        
        self.frame_count = QSpinBox()
        self.frame_count.setMinimum(0)
        form.addRow("Frame Count", self.frame_count)
        
        self.file_capacity = QSpinBox()
        self.file_capacity.setMinimum(0)
        form.addRow("File Capacity", self.file_capacity)
        
        self.headers_only = QCheckBox()
        form.addRow("Headers-Only", self.headers_only)
        
        group.setLayout(form)
        return group
    
    def createAcquisitionGroup(self):
        group = QGroupBox("Acquisition Options")
        form = QFormLayout()
        
        self.poll_affinity = QSpinBox()
        self.poll_affinity.setMinimum(0)
        form.addRow("Poll Affinity", self.poll_affinity)
        
        self.export_affinity = QSpinBox()
        self.export_affinity.setMinimum(0)
        form.addRow("Export Affinity", self.export_affinity)
        
        self.queue_capacity = QSpinBox()
        self.queue_capacity.setMinimum(0)
        form.addRow("Queue Capacity", self.queue_capacity)
        
        group.setLayout(form)
        return group
    
    # --- Functionality ---

    def generateConfig(self):
        return {
            "source": f"shm://{self.source}",
            "export-format": self.format_combo.currentText(),
            "metadata-only": "yes" if self.headers_only.isChecked() else "no",
            "frame-count": self.frame_count.value(),
            "file-capacity": self.file_capacity.value(),
            "polling-core": self.poll_affinity.value(),
            "export-core": self.export_affinity.value(),
            "buffer-limit": self.queue_capacity.value()
        }


class TelemetryUI(QWidget):
    ''' Implements a PyQt5 widget for configuring and operating the daoTelemetry tool '''
    
    def __init__(self, UI):
        super().__init__()
        self.UI = UI
        self.createUI()
    
    # --- UI ---
    
    def createUI(self):
        # create UI elements
        self.configSummary = self.createConfigSummary()
        self.configPanel = self.createConfigPanel()

        configLayout = QHBoxLayout()
        configLayout.addWidget(self.configSummary, 1)
        configLayout.addWidget(self.configPanel, 2)

        # create button panel
        buttonLayout = QHBoxLayout()
        buttonLayout.addWidget(self.createTelAddBtn())
        buttonLayout.addWidget(self.createTelClearBtn())
        buttonLayout.addWidget(self.createExportBtn())
        buttonLayout.addWidget(self.createRecordBtn())

        # assemble into UI
        masterLayout = QVBoxLayout()
        masterLayout.addLayout(configLayout)
        masterLayout.addLayout(buttonLayout)
        self.setLayout(masterLayout)
        
    def createRecordBtn(self):
        btn = QPushButton("Record")
        btn.clicked.connect(self.record)
        return btn
        
    def createExportBtn(self):
        btn = QPushButton("Export Configuration")
        btn.clicked.connect(self.exportConfiguration)
        return btn
    
    def createTelAddBtn(self):
        btn = QPushButton("Add Telemetry")
        btn.clicked.connect(self.addTelemetry)
        return btn
    
    def createTelClearBtn(self):
        btn = QPushButton("Clear Telemetry")
        btn.clicked.connect(self.clearTelemetry)
        return btn

    def createConfigSummary(self):
        configSummaryList = QListWidget()
        configSummaryList.itemClicked.connect(self.displayConfig)
        return configSummaryList

    def createConfigPanel(self):
        configPanel = QStackedWidget()
        configPanel.addWidget(QListWidget())
        return configPanel
    
    def createQuickRecordForm(self, source: str):
        form = QDialog(self)
        form.setWindowTitle("Shared Memory Quick Record")
        layout = QVBoxLayout()
        
        sourceField = QLineEdit(source)
        sourceField.setReadOnly(True)
        
        self.saveAsField = QLineEdit()
        self.saveAsField.setReadOnly(True)
        
        def openFileDialog():
            fileName, _ = QFileDialog.getSaveFileName(
                self, "Save File", self.filenameEdit.text(), 
                "Numpy Files (*.npy)",
            )
            if fileName: self.saveAsField.setText(fileName)
        self.saveAsField.mousePressEvent = openFileDialog
        
        self.frameCounter = QSpinBox()
        self.frameCounter.setRange(1, 10000)
        self.frameCounter.setValue(1)
        
        layout.addWidget(QLabel("Source: "))
        layout.addWidget(sourceField)
        layout.addWidget(QLabel("Save to:"))
        layout.addWidget(self.saveAsField)
        layout.addWidget(QLabel("Number of Frames:"))
        layout.addWidget(self.frameCounter)
        
        def finish():
            if self.saveAsField.text():
                form.accept()
            else:
                QMessageBox.critical(self.UI, "Quick Record Error", "Please select a SaveAs path!")
                return

        finishBtn = QPushButton("Start Capture")
        finishBtn.clicked.connect(finish)
        layout.addWidget(finishBtn)
        
        form.setLayout(layout)
        return form
    
    # --- Functionality ---
    
    def record(self):
        numSelectedShms = len(self.UI.tableWidget.selectedItems())
        if numSelectedShms > 1:
            QMessageBox.critical(self.UI, "Quick Record Error", "Cannot quick-record multiple shared memory targets")
            return
            
        # perform a quick capture.
        if numSelectedShms == 1:
            source = f"/tmp/{self.UI.tableWidget.selectedItems()[0].text()}"
            modalForm = self.createQuickRecordForm(source)
            if QDialog.Accepted == modalForm.exec_() :
                saveAsPath = self.saveAsField.text()
                nFrames = self.frameCounter.value()
                # self.record_file(saveAsPath, nFrames)
                print(f"Quick recorded {source}, saved {nFrames} frames to {saveAsPath}")
            return
        
        # perform proper telemetry capture.
        # @todo(tom) add telemetry tool record feature.
        QMessageBox.critical(self.UI, "Telemetry Record Error", "TODO")
    
    def exportConfiguration(self):
        if not self.configPanel.count():
            QMessageBox.critical(self.UI, "Export Error", "No telemetry configured!")
            return
        
        outputFile, _ = QFileDialog.getSaveFileName(self, "Save As", os.getcwd())
        if outputFile:
            config = {
                "session_policies": {
                    "archive": os.getenv("DAODATA", os.getcwd()),
                    "grouping": "on" if self.configPanel.count() > 1 else "off"
                },
                "telemetry_list": [
                    self.configPanel.widget(i).generateConfig()
                    for i in range(self.configPanel.count())
                ]
            }
        
            with open(outputFile, "w") as file:
                yaml.dump(config, file)
    
    def displayConfig(self, item):
        configUI = item.data(Qt.UserRole)
        self.configPanel.setCurrentWidget(configUI)

    def clearTelemetry(self):
        for ui in self.getCurrentConfigUIs(): self.configPanel.removeWidget(ui)
        self.configSummary.clear()
        
    def removeTelemetry(self, configUI):
        self.configPanel.removeWidget(configUI)
        self.configSummary.clear()
        sources = [ui.source for ui in self.getCurrentConfigUIs()]
        self.configSummary.addItems(sources)
        
    def addTelemetry(self):
        configUI = None
        shmsSelected = self.UI.tableWidget.selectedItems()
        currentSources = [ui.source for ui in self.getCurrentConfigUIs()]
        
        def summaryAppend(configUI):
            item = QListWidgetItem(configUI.source)
            item.setData(Qt.UserRole, configUI)
            self.configSummary.addItem(item)
        
        if len(shmsSelected):
            for shmCell in shmsSelected:
                source = f"/tmp/{shmCell.text()}"
                if source in currentSources:
                    continue
                
                configUI = ShmConfig(source, self.removeTelemetry)
                self.configPanel.addWidget(configUI)
                self.configPanel.setCurrentIndex(self.configPanel.count() - 1)
                summaryAppend(configUI)
        else:
            filePath, _ = QFileDialog.getOpenFileName(self.UI, "Select File", os.getcwd())
            if filePath and (not filePath in currentSources):
                configUI = FileConfig(filePath, self.removeTelemetry)
                self.configPanel.addWidget(configUI)
                self.configPanel.setCurrentWidget(configUI)
                summaryAppend(configUI)
    
    def getCurrentConfigUIs(self):
        return [self.configPanel.widget(i) for i in range(1, self.configPanel.count())]