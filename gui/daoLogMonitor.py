from PyQt5.QtWidgets import QApplication,QWidget,QTextEdit,QVBoxLayout,QPushButton, QHBoxLayout, QFormLayout, QLineEdit
from PyQt5.QtCore import QTimer
import sys
import time
from collections import deque

from daoRecvLogs import network_log

class LogMonitor(QWidget):
        def __init__(self,parent=None):
            super().__init__(parent)

            self.log = network_log()

            self.setWindowTitle("LogMonitor")
            # self.resize(300,270)

            self.textEdit = QTextEdit()
            self.textEdit.setReadOnly(True)

            self.start_button = QPushButton("Start")
            self.pause_button = QPushButton("Pause")
            self.clear_button = QPushButton("Clear")
            self.log_ip     = QLineEdit("127.0.0.1")
            self.log_port   = QLineEdit("5555")


            # handle the layout of the UI
            OverAllLayout = QVBoxLayout()
            OverAllLayout.addWidget(self.textEdit)

            mLayout = QHBoxLayout()
            mLayout.addWidget(self.start_button)
            mLayout.addWidget(self.pause_button)
            mLayout.addWidget(self.clear_button)
            OverAllLayout.addLayout(mLayout)

            configLayout    = QFormLayout()

            configLayout.addRow("IP:   ", self.log_ip)
            configLayout.addRow("Port: ", self.log_port)

            OverAllLayout.addLayout(configLayout)
            self.setLayout(OverAllLayout)

            self.timer = QTimer(interval=100, timeout=self.updating)
            self.start_button.clicked.connect(self.start_button_Clicked)
            self.pause_button.clicked.connect(self.pause_button_Clicked)
            self.clear_button.clicked.connect(self.clear_button_Clicked)

        def updating(self):
            while len(self.log.buffer) > 0:
                A = self.log.buffer.popleft()
                self.textEdit.append(self.log.getString(A))
        
        def start_button_Clicked(self):
            # update text
            ip = self.log_ip.text()
            port = self.log_port.text()
            if not self.log.connected:
                self.start_button.setText("Stop")
                self.log.connect(ip, port)
                self.log.StartLogging()
                self.timer.start()
            else:
                self.start_button.setText("Start")
                self.log.disconnect()
                self.log.StopLogging()
                self.timer.stop()

        def pause_button_Clicked(self):
            if self.log.connected:
                if self.timer.isActive():
                    self.timer.stop()
                    self.pause_button.setText("Resume")
                else:
                    self.timer.start()
                    self.pause_button.setText("Pause")

        def clear_button_Clicked(self):
            # clear the screen
            self.textEdit.clear()

if __name__ == '__main__':

    
    app = QApplication(sys.argv)
    win = LogMonitor()
    win.show()
    sys.exit(app.exec_())