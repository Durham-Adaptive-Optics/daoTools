'''
 # @ Author: Thomas N. Davies
 # @ Company: Centre for Advanced Instrumentation, Durham University
 # @ Contact: thomas.n.davies@durham.ac.uk
 # @ Create Time: 2026-04-20 11:33:44
 # @ Description: GUI for configuring and controlling daoTelemetry for telemetry recording.
 '''

import time

from PyQt5.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QListWidget, QFormLayout, QFileDialog
from PyQt5.QtWidgets import QListWidgetItem, QLabel, QLineEdit, QComboBox, QSpinBox, QCheckBox, QStackedWidget
from daoCommandIfce import daoCommandIfce
from PyQt5.QtWidgets import QPushButton
from PyQt5.QtCore import Qt
from enum import Enum
import yaml
import os

class TelemetryType(Enum):
    SharedMemory: int = 0
    File: int = 1

class SharedMemoryConfiguration(QWidget):
    def __init__(self, source: str, removeCallback: callable):
        super().__init__()
        self.initUI(source, removeCallback)

    def initUI(self, source: str, removeCallback: callable):
        main_layout = QVBoxLayout()
        self.setLayout(main_layout)
        
        form_layout = QFormLayout()
        
         # Type label
        typeLbl = QLabel(f"Type: shared memory (shm://)")
        form_layout.addRow(typeLbl)
        
        # Source with file picker
        source_layout = QHBoxLayout()
        self.source = QLineEdit(source)
        self.source.setReadOnly(True)
        source_button = QPushButton("Browse...")
        source_button.clicked.connect(self.pick_file)
        source_layout.addWidget(self.source)
        source_layout.addWidget(source_button)
        form_layout.addRow("Source:", source_layout)
        
        # Export Format
        self.export_format = QComboBox()
        self.export_format.addItems(["FITS", "NPY"])
        form_layout.addRow("Export Format:", self.export_format)
        
        # Frame Count
        self.frame_count = QSpinBox()
        form_layout.addRow("Frame Count:", self.frame_count)
        
        # File Capacity
        self.field_capacity = QSpinBox()
        form_layout.addRow("File Capacity:", self.field_capacity)
        
        # Polling Core
        self.polling_core = QSpinBox()
        form_layout.addRow("Poll Affinity:", self.polling_core)
        
        # Export Core
        self.export_core = QSpinBox()
        form_layout.addRow("Export Affinity:", self.export_core)
        
        # Buffer Limit
        self.buffer_limit = QSpinBox()
        form_layout.addRow("Queue Capacity:", self.buffer_limit)
        
        # Metadata Only
        self.metadata_only = QCheckBox()
        form_layout.addRow("Headers Only:", self.metadata_only)
        
        # Remove button
        removeBtn = QPushButton("Remove")
        removeBtn.clicked.connect(lambda: removeCallback(self))
        form_layout.addRow(removeBtn)
        
        main_layout.addLayout(form_layout)
        main_layout.addStretch()
    
    def genConfig(self):
        return {
            "source": f"shm://{self.source.text()}",
            "export-format": self.export_format.currentText(),
            "metadata-only": "yes" if self.metadata_only.isChecked() else "no",
            "frame-count": self.frame_count.value(),
            "file-capacity": self.field_capacity.value(),
            "polling-core": self.polling_core.value(),
            "export-core": self.export_core.value(),
            "buffer-limit": self.buffer_limit.value()
        }
    
    def pick_file(self):
        file_path, _ = QFileDialog.getOpenFileName(self, "Select File", self.source.text())
        if file_path:
            self.source.setText(file_path)

class FileConfiguration(QWidget):
    def __init__(self, source: str, removeCallback: callable):
        super().__init__()
        self.initUI(source, removeCallback)

    def initUI(self, source: str, removeCallback: callable):
        main_layout = QVBoxLayout()
        self.setLayout(main_layout)
        
        form_layout = QFormLayout()
        
        # Type label
        typeLbl = QLabel(f"Type: file (file://)")
        form_layout.addRow(typeLbl)
        
        # Source with file picker
        source_layout = QHBoxLayout()
        self.source = QLineEdit(source)
        self.source.setReadOnly(True)
        source_button = QPushButton("Browse...")
        source_button.clicked.connect(self.pick_file)
        source_layout.addWidget(self.source)
        source_layout.addWidget(source_button)
        form_layout.addRow("Source:", source_layout)
        
        # Remove button
        removeBtn = QPushButton("Remove")
        removeBtn.clicked.connect(lambda: removeCallback(self))
        form_layout.addRow(removeBtn)
        
        main_layout.addLayout(form_layout)
        main_layout.addStretch()
    
    def genConfig(self):
        return {
            "source": f"file://{self.source.text()}"
        }
    
    def pick_file(self):
        file_path, _ = QFileDialog.getOpenFileName(self, "Select File")
        if file_path:
            self.source.setText(file_path)

class TelemetryControlUI(QWidget):
    def __init__(self, toolIP: str = "127.0.0.1", toolPort: int = 9999):
        super().__init__()
        
        self.toolIP = toolIP
        self.toolPort = toolPort
        
        self.telemetryConfigs = QStackedWidget()
        self.telemetryList = QListWidget()
        self.clearAllBtn = QPushButton("Reset")
        self.clearAllBtn.clicked.connect(self.clearAll)
        self.exportConfigBtn = QPushButton("Export")
        self.exportConfigBtn.clicked.connect(self.exportConfig)
        self.telemetryList.itemClicked.connect(self.showTelemetryConfig)
        self.recordBtn = QPushButton("Record")
        self.recordBtn.clicked.connect(self.launchRecordSession)

        self.initUI()
        
        self.addTelemetry("/tmp/cblue.im.shm", TelemetryType.SharedMemory)
        self.addTelemetry("/tmp/calpix.im.shm", TelemetryType.SharedMemory)
        self.addTelemetry("/opt/foobar/calpix.yml", TelemetryType.File)
    
    def initUI(self):
        
        self.actionsBar = QHBoxLayout()
        self.actionsBar.addWidget(self.clearAllBtn)
        self.actionsBar.addWidget(self.exportConfigBtn)
        self.actionsBar.addWidget(self.recordBtn)

        self.masterLayout = QVBoxLayout()
        self.setLayout(self.masterLayout)
        self.masterLayout.addLayout(self.actionsBar)

        self.masterLayout2 = QHBoxLayout()

        self.overviewPane = QVBoxLayout()
        self.overviewPane.addWidget(self.telemetryList)
        self.overviewPane.addStretch()
        
        self.configPane = QVBoxLayout()
        self.configPane.addWidget(self.telemetryConfigs)
        
        self.masterLayout2.addLayout(self.overviewPane, 1)
        self.masterLayout2.addLayout(self.configPane, 1)
        self.masterLayout.addLayout(self.masterLayout2)

    def addTelemetry(self, source: str, type: TelemetryType):
        ConfigUI = {
            TelemetryType.SharedMemory: SharedMemoryConfiguration,
            TelemetryType.File: FileConfiguration
        }.get(type)
        
        # Create new telemetry config
        ui = ConfigUI(source, self.removeTelemetry)
        uiIndex = self.telemetryConfigs.count()
        self.telemetryConfigs.addWidget(ui)
        
        # Add telemetry source to overview list
        self.telemetryListAppend(source, uiIndex)

    def removeTelemetry(self, widget):
        self.telemetryConfigs.removeWidget(widget)
        self.telemetryConfigs.setCurrentIndex(0)
        self.telemetryConfigs.hide()
        self.rebuildTelemetryList()
        
    def showTelemetryConfig(self, item):
        uiIndex = item.data(Qt.UserRole)
        self.telemetryConfigs.setCurrentIndex(uiIndex)
        self.telemetryConfigs.show()
        
    def rebuildTelemetryList(self):
        self.telemetryList.clear()
        for index in range(self.telemetryConfigs.count()):
            widget = self.telemetryConfigs.widget(index)
            self.telemetryListAppend(widget.source.text(), index)
    
    def telemetryListAppend(self, source: str, uiIndex: int):
        item = QListWidgetItem(source)
        item.setData(Qt.UserRole, uiIndex)
        self.telemetryList.addItem(item)
        self.telemetryList.show()

    def clearAll(self):
        widgets = [self.telemetryConfigs.widget(i) for i in range(self.telemetryConfigs.count())]
        for w in widgets: self.telemetryConfigs.removeWidget(w)
        self.telemetryList.clear()
        self.telemetryConfigs.hide()
        self.telemetryList.hide()
            
    def generateConfig(self):
        telemetryList = [
            self.telemetryConfigs.widget(index).genConfig() 
            for index in range(self.telemetryConfigs.count())
        ]
        
        return {
            "session_policies": {
                "archive": os.getenv("DAODATA", os.getcwd()),
                "grouping": "on" if self.telemetryConfigs.count() > 1 else "off"
            },
            "telemetry_list": telemetryList
        }
            
    def exportConfig(self):
        # pick output location.
        outputFile, _ = QFileDialog.getSaveFileName(self, "Select Configuration File Path", os.getcwd())
        if not outputFile:
            return
            
        # generate and output file.
        with open(outputFile, "w") as file:
            yaml.dump(self.generateConfig(), file)
            
    def launchRecordSession(self):
        # connect to the telemetry tool.
        # ifce = daoCommandIfce(self.toolIP, self.toolPort, timeout=1)
        # status, _ = ifce.Ping()
        # if 0 != status:
        #     QMessageBox.critical(self, "Error", f"Connection to daoTelemetry tool failed (status code: {status})")
        #     return
        
        # # check the tool is ready to record.
        # status, state = ifce.State(None)
        # if 0 != status:
        #     QMessageBox.critical(self, "Error", f"Connection to daoTelemetry tool failed (status code: {status})")
        #     return

        # if not state in ("Off", "Idle"):
        #     QMessageBox.critical(self, "Error", f"daoTelemetry tool is not available to record currently (current state: {state})")
        #     return
            
        # generate the config and determine if the session will need manual stoppage.
        sessionAutoEnds = True
        config = self.generateConfig()
        for telemetry in config["telemetry_list"]:
            if (telemetry["source"].split("//:")[0] == "shm") and (not "frame-count" in telemetry): 
                sessionAutoEnds = False
                break
        
        # issue recording session.
        # try:
        #     ifce.Exec("Disable")
        #     ifce.Exec("Stop")
        #     ifce.Other(yaml.dump(config))
        #     ifce.Exec("Enable")
        #     ifce.Exec("Run")
        # except:
        #     QMessageBox.critical(self, "Error", f"failed to configure telemetry tool")
            
        # await session end.
        if sessionAutoEnds:
            self.recordBtn.setText("Recording ..")
            self.recordBtn.setEnabled(False)
            time.sleep(15)
            self.recordBtn.setText("Record")
            self.recordBtn.setEnabled(True)
        else:
            self.recordBtn.setText("End Recording")

