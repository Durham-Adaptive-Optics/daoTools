import sys
from PyQt5.QtWidgets import QApplication, QTableWidget, QTableWidgetItem, QTextEdit, QHBoxLayout, QVBoxLayout, QWidget, QPushButton, QLineEdit, QFormLayout, QLabel, QHeaderView
from PyQt5.QtCore import QTimer
from collections import deque
from daoRecvLogs import network_log

class TableWidget(QWidget):
    def __init__(self):
        super().__init__()

        self.log = network_log()
        self.table = QTableWidget()
        self.table.setColumnCount(5)
        self.table.verticalHeader().setDefaultSectionSize(15)
        self.table.verticalHeader().setSectionResizeMode(QHeaderView.Fixed)

        self.table.setHorizontalHeaderLabels(["Name", "Timestamp", "Machine", "Log Level", "Message"])

        self.filter_name = QTextEdit()
        self.filter_name.setFixedHeight(30)
        self.filter_name_label = QLabel("Filter Name:")
        self.filter_timestamp = QTextEdit()
        self.filter_timestamp.setFixedHeight(30)
        self.filter_timestamp_label = QLabel("Filter Timestamp:")
        self.filter_machine = QTextEdit()
        self.filter_machine.setFixedHeight(30)
        self.filter_machine_label = QLabel("Filter Machine:")
        self.filter_log_level = QTextEdit()
        self.filter_log_level.setFixedHeight(30)
        self.filter_log_level_label = QLabel("Filter Log Level:")
        self.filter_message = QTextEdit()
        self.filter_message.setFixedHeight(30)
        self.filter_message_label = QLabel("Filter Message:")

        self.button_start = QPushButton("Start")
        self.button_stop  = QPushButton("Pause")
        self.button_clear = QPushButton("Clear")
        self.log_ip     = QLineEdit("127.0.0.1")
        self.log_port   = QLineEdit("5555")

        filter_label_layout = QHBoxLayout()
        filter_label_layout.addWidget(self.filter_name_label)
        filter_label_layout.addWidget(self.filter_timestamp_label)
        filter_label_layout.addWidget(self.filter_machine_label)
        filter_label_layout.addWidget(self.filter_log_level_label)
        filter_label_layout.addWidget(self.filter_message_label)

        filter_layout = QHBoxLayout()
        filter_layout.addWidget(self.filter_name)
        filter_layout.addWidget(self.filter_timestamp)
        filter_layout.addWidget(self.filter_machine)
        filter_layout.addWidget(self.filter_log_level)
        filter_layout.addWidget(self.filter_message)

        button_layout = QHBoxLayout()
        button_layout.addWidget(self.button_start)
        button_layout.addWidget(self.button_stop)
        button_layout.addWidget(self.button_clear)

        config_layout = QFormLayout()
        config_layout.addRow("IP:   ", self.log_ip)
        config_layout.addRow("Port: ", self.log_port)

        main_layout = QVBoxLayout()
        main_layout.addWidget(self.table)
        main_layout.addLayout(filter_label_layout)
        main_layout.addLayout(filter_layout)
        main_layout.addLayout(button_layout)
        main_layout.addLayout(config_layout)

        self.setLayout(main_layout)

        # Connect the textChanged signal from each input text box to the filter_table function
        self.filter_name.textChanged.connect(self.filter_table)
        self.filter_timestamp.textChanged.connect(self.filter_table)
        self.filter_machine.textChanged.connect(self.filter_table)
        self.filter_log_level.textChanged.connect(self.filter_table)
        self.filter_message.textChanged.connect(self.filter_table)

        # connect the buttons

        self.timer = QTimer(interval=100, timeout=self.updating)
        self.button_start.clicked.connect(self.button_start_clicked)
        self.button_stop.clicked.connect(self.button_stop_clicked)
        self.button_clear.clicked.connect(self.button_clear_clicked)

    def filter_table(self):
        filter_name_text = self.filter_name.toPlainText()
        filter_timestamp_text = self.filter_timestamp.toPlainText()
        filter_machine_text = self.filter_machine.toPlainText()
        filter_log_level_text = self.filter_log_level.toPlainText()
        filter_message_text = self.filter_message.toPlainText()

        if all(v is None for v in [filter_name_text, filter_timestamp_text, filter_machine_text, filter_log_level_text, filter_message_text]):
            return

        for i in range(self.table.rowCount()):
            name = self.table.item(i, 0).text()
            timestamp = self.table.item(i, 1).text()
            machine = self.table.item(i, 2).text()
            log_level = self.table.item(i, 3).text()
            message = self.table.item(i, 4).text()

            if filter_name_text not in name or filter_timestamp_text not in timestamp or filter_machine_text not in machine or filter_log_level_text not in log_level or filter_message_text not in message:
                self.table.setRowHidden(i, True)
            else:
                self.table.setRowHidden(i, False)

    def updating(self):
        while len(self.log.buffer) > 0:
            A = self.log.buffer.popleft()
            self.add_data([(A.component_name,A.time_stamp,A.machine, self.log.level2Text(A.log_level), A.log_message)]) 

    def clear_table(self):
        self.table.setRowCount(0)
                
    def add_data(self, data):
        """Add data to the table.
        
        data : List[Tuple[str, str, str, str, str]]
            List of tuples containing name, timestamp, machine, log_level, message.
        """
        for i, row in enumerate(data):
            self.table.insertRow(i)
            # self.table.setRowHeight(i,15)
            for j, val in enumerate(row):
                self.table.setItem(i, j, QTableWidgetItem(val))
        self.table.resizeColumnToContents(0)
        self.table.resizeColumnToContents(1)
        self.table.resizeColumnToContents(2)
        self.table.resizeColumnToContents(3)
        self.table.resizeColumnToContents(4)
        self.filter_table()

    def button_start_clicked(self):
        # update text
        ip = self.log_ip.text()
        port = self.log_port.text()
        if not self.log.connected:
            self.button_start.setText("Stop")
            self.log.connect(ip, port)
            self.log.StartLogging()
            self.timer.start()
        else:
            self.button_start.setText("Start")
            self.log.disconnect()
            self.log.StopLogging()
            self.timer.stop()

    def button_stop_clicked(self):
        if self.log.connected:
            if self.timer.isActive():
                self.timer.stop()
                self.button_stop.setText("Resume")
            else:
                self.timer.start()
                self.button_stop.setText("Pause")

    def button_clear_clicked(self):
        # clear the screen
        self.clear_table()

if __name__=="__main__":
    app = QApplication(sys.argv)
    table_widget = TableWidget()
    table_widget.show()
    sys.exit(app.exec_())

    