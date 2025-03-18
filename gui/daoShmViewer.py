import sys
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QSplitter, QFileSystemModel, QTreeView,
    QLabel, QVBoxLayout, QWidget, QTextBrowser
)
from PyQt5.QtCore import Qt, QDir, QTimer
from PyQt5.QtGui import QPixmap
import os
from datetime import datetime

import dao
import magicplot
import numpy as np


class FileViewerApp(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("File Viewer and Metadata")
        self.timer = QTimer()
        self.timer.timeout.connect(self.on_timer_triggered)

        # Create the main splitter
        splitter = QSplitter(Qt.Horizontal)
        

        # File Viewer on the left
        self.file_model = QFileSystemModel()
        self.file_model.setNameFilters(["*.im.shm"])  # File types
        self.file_model.setFilter(QDir.Files)  # Only display files, not folders
        self.file_model.setNameFilterDisables(False)
        self.file_model.setRootPath(".")

        self.file_view = QTreeView()
        self.file_view.setModel(self.file_model)
        self.file_view.setRootIndex(self.file_model.index("/tmp/"))
        self.file_view.clicked.connect(self.on_file_selected)

        splitter.addWidget(self.file_view)

        # Image and Metadata Viewer on the right
        right_widget = QWidget()
        self.right_layout = QVBoxLayout()
        self.image_label = magicplot.MagicPlot()
        self.metadata_browser = QTextBrowser()


        self.right_layout.addWidget(self.image_label)
        self.right_layout.addWidget(self.metadata_browser)
        right_widget.setLayout(self.right_layout)

        splitter.addWidget(right_widget)
        
          # Configure splitter
        splitter.setStretchFactor(0, 1)  # Left widget gets 1x weight
        splitter.setStretchFactor(1, 2)  # Right widget gets 2x weight
        splitter.setSizes([300, 700])    # Initial sizes for the widgets

        # Set main widget
        self.setCentralWidget(splitter)

    def on_file_selected(self, index):
        # Get the file path
        file_path = self.file_model.filePath(index)
        self.shm = dao.shm(file_path)
        data = self.shm.get_data()
        if(np.min(data.shape) == 1):
            self.FLAT = True
        else:
            self.FLAT = False
        # Save the position of the current widget in the layout
        index = self.right_layout.indexOf(self.image_label)

        if self.image_label:
            self.right_layout.removeWidget(self.image_label)
            self.image_label.deleteLater()
            self.image_label = magicplot.MagicPlot()
            self.right_layout.insertWidget(index, self.image_label)

        if self.FLAT:
            # self.image_label.plot(data.flatten())
            self.im = self.image_label.getDataItem()
            self.im.setData(self.shm.get_data().flatten())
        else:
            # self.image_label.plot(data)
            self.im = self.image_label.getImageItem()
            self.im.setData(self.shm.get_data())
           
        self.image_label.updatePanBounds()
        self.image_label.viewBox.autoRange()
        # Display metadata
        self.show_metadata(file_path)
        
        # Start or restart the timer
        self.timer.start(100)  # Set the timer to 5 seconds (5000 milliseconds)

    def on_timer_triggered(self):
        if(self.FLAT):
            self.im.setData(self.shm.get_data().flatten())
        else:
            self.im.setData(self.shm.get_data())      

    def show_metadata(self, file_path):
        try:
            stat_info = os.stat(file_path)
            creation_time = datetime.fromtimestamp(stat_info.st_ctime).strftime("%Y-%m-%d %H:%M:%S")
            modification_time = datetime.fromtimestamp(stat_info.st_mtime).strftime("%Y-%m-%d %H:%M:%S")
            size = stat_info.st_size  # File size in bytes
            a = self.shm.get_data()
            shape = a.shape
            dtype = str(a.dtype)

            metadata = (
                f"File: {os.path.basename(file_path)}\n"
                f"Size: {size} bytes\n"
                f"Created: {creation_time}\n"
                f"Last Modified: {modification_time} \n"
                f"shape: {shape} \n"
                f"dtype: {dtype} \n"
            )
            self.metadata_browser.setText(metadata)
        except Exception as e:
            self.metadata_browser.setText(f"Error reading metadata: {e}")


if __name__ == "__main__":
    app = QApplication(sys.argv)
    viewer = FileViewerApp()
    viewer.show()
    sys.exit(app.exec_())
