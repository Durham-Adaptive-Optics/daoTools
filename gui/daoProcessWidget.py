import sys
from PyQt5.QtWidgets import QApplication, QWidget, QHBoxLayout, QLabel, QComboBox, QVBoxLayout, QLineEdit, QPushButton
import subprocess
from daoCommandIfce import daoCommandIfce
import daoLaunch
import datetime
from dataclasses import dataclass
import threading
import logging
from daoLog import daoLog
from typing import Union


@dataclass
class Process:
    name:       str
    exe:        str
    tmuxname:   str
    port:       int
    machine:    Union [str, None] = None
    user:       Union [str, None] = None
    args:       Union [str, None] = None
    state:      str = "Unknown"
    pid:        int = 0

class ProcessWidget(QWidget):
    def __init__(self, process, parent=None, name=__name__):
        super().__init__(parent)
        self.log = logging.getLogger(name)
        self.process = process
        if process.machine is None:
            process.machine = '127.0.0.1'
        self.connect_string = f'{self.process.machine}:{self.process.port}'
        self.commandIfce = daoCommandIfce(self.process.machine, self.process.port)

        if self.process.user is None:
            self.user_dispay = ""
        self.layout = QHBoxLayout()

        self.label_name    = QLabel(f"{self.process.name}")
        self.label_exe     = QLabel(f"{self.process.exe}")
        self.label_machine = QLabel(f"{self.user_dispay} - {self.process.machine}:{self.process.port}")
        self.label_tmux    = QLabel(f"{self.process.tmuxname}")
        self.label_state   = QLabel(f"{self.process.state}")
        self.label_pid     = QLabel(f"{self.process.pid}")
        self.button_xterm = QPushButton("Open xterm")
        self.button_xterm.clicked.connect(self.open_xterm)

        self.dropdown_command = QComboBox()
        self.dropdown_command.addItems(["EXEC","SETUP","UPDATE", "PING","STATE", "SET_LOG_LEVEL", "DUMP", "QUERY", "OTHER"])
        self.dropdown_command.activated[str].connect(self.command_on_activated)

        self.dropdown_state = QComboBox()
        self.dropdown_state.addItems(["Init", "Stop", "Enable", "Disable", "Run", "Idle", "Recover"])
        self.dropdown_state.activated[str].connect(self.state_on_activated)

        self.dropdown_launch = QComboBox()
        self.dropdown_launch.addItems(["launch", "kill", "xterm"])
        self.dropdown_launch.activated[str].connect(self.launch_on_activated)

        self.ArgumentInput  = QLineEdit("args")

        self.layout.addWidget(self.dropdown_launch)
        self.layout.addWidget(self.label_name)
        self.layout.addWidget(self.label_exe)
        self.layout.addWidget(self.label_machine)
        self.layout.addWidget(self.label_tmux)
        self.layout.addWidget(self.label_state)
        self.layout.addWidget(self.label_pid)
        self.layout.addWidget(self.dropdown_state)
        self.layout.addWidget(self.dropdown_command)
        self.layout.addWidget(self.ArgumentInput)
        self.layout.addWidget(self.button_xterm)
        self.setLayout(self.layout)

        threading.Thread(target = self.check_update).start()

    # function to see if alive and what state and to update labels accordingly.
    def check_update(self):
        #  check tmux session alive...
        print(f"Checking if tmux {self.process.tmuxname} is alive")
        if(daoLaunch.check_tmux_session(tmuxname=self.process.tmuxname, user=self.process.user, machine=self.process.machine)):
            # tmux session alive send ping command to confirm
            print(f"pining process")
            status, payload = self.commandIfce.Ping("")
            if(status == 0):
                print("process alive: ")
                self.process.pid = payload
                print("checking process State")
                status, payload = self.commandIfce.State("")
                if(status == 0):
                    self.process.state = payload
            elif(status == 1):
                print("Process failed")
            else:
                print("Process timed out")
        else:
            print("tmux session not running:")
            self.process.state="Unknown"
            self.process.pid = 0

        self.update_labels()

    def open_xterm(self):
        daoLaunch.launch_xterm(machine=self.process.machine,user=self.process.user,tmux=self.process.tmuxname)

    def update_labels(self):
        self.label_state.setText(f"{self.process.state}")
        self.label_pid.setText(f"{self.process.pid}")

    def command_on_activated(self, text):
        args = self.ArgumentInput.text()
        if text == "EXEC":
            status, payload = self.commandIfce.Exec(args)
            if(status == 0):
                self.log.info("Successful")
        elif text == "SETUP":
            status, payload = self.commandIfce.Setup(args)
            if(status == 0):
                self.log.info("Successful")
        elif text == "UPDATE":
            status, payload = self.commandIfce.Update(args)
        elif text == "PING":
            status, payload = self.commandIfce.Ping(args)
            if(status == 0):
                self.log.info("Successful")
                self.process.pid = payload
                self.log.info(f"PID: {payload}")
        elif text == "STATE":
            status, payload = self.commandIfce.State(args)
            if(status == 0):
                self.process.state = payload
        elif text == "SET_LOG_LEVEL":
            status, payload = self.commandIfce.SetLogLevel(args)
        elif text == "DUMP":
            status, payload = self.commandIfce.Dump(args)
            if(status == 0):
                print(payload)
        elif text == "QUERY":
            status, payload = self.commandIfce.Query(args)
            if(status == 0):
                print(payload)
        elif text == "OTHER":
            status, payload = self.commandIfce.Other(args)
        
        self.check_update()


    def state_on_activated(self, text):
        status, payload = self.commandIfce.Exec(text)
        if(status == 0):
            self.log.info("Sucessful")
            self.check_update()
        else:
            self.log.warning(payload)

    def launch_on_activated(self, text):
        self.log.trace(f"mangae_process: {text}")
        daoLaunch.manage_process(text, 
                                tmuxname = self.process.tmuxname,
                                user=self.process.user,
                                machine=self.process.machine,
                                processExe=self.process.exe,
                                processArgs=self.process.args)
        self.check_update()


class SupervisorWidget(QWidget):
    def __init__(self, process, parent=None, name=__name__):
        super().__init__(parent)
        self.process_list = process
        # Create a vertical layout
        self.layout = QVBoxLayout()
        # Add items to the layout
        for proc in self.process_list:
            self.layout.addWidget(ProcessWidget(proc, name=name))
        # Set the layout for the widget
        self.setLayout(self.layout)



if __name__ == '__main__':
    app = QApplication(sys.argv)
    logger = daoLog(__name__)

    process1 = Process( "Process 1",
                        "daoPixCal.py",
                        "pixCal",
                        5556)
    process2 = Process( "Process 2",
                        "daoCentroidSH.py",
                        "CentroidSH",
                        5557)
    process_list = [process2]
    supervisor_widget = SupervisorWidget(process_list, name=__name__)
    supervisor_widget.show()
    sys.exit(app.exec_())