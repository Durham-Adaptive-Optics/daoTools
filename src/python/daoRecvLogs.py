import sys, signal
import daoLogging_pb2
import zmq
import random
import collections
from dataclasses import dataclass
from threading import Thread
from threading import Event

import time
#recv logs
@dataclass
class LogHolder:
    component_name: str
    time_stamp: str
    machine: str
    log_message: str
    log_level: str

class network_log:
    def __init__(self, buffer_length=2000, filename='/tmp/daoLog.logs'):
        self.buffer_length = buffer_length
        self.buffer = collections.deque(maxlen=buffer_length)

        self.recv_thread = Thread(target = self.fill_log_queue)
        self.recv_stopEvent = Event()
        self.filename = filename
        self.connected = False

    def level2Text(self, level):
        if level == 0:
            return "Trace  "
        elif level == 1:
            return "Debug  "
        elif level == 2:
            return "INFO   "
        elif level == 3:
            return "WARNING"
        elif level == 4:
            return "ERROR  "
        elif level == 5:
            return "FATAL  "
        else:
            return "?????"
    
    def connect(self, ip, port, filename=None):
        if self.connected:
            # stop recv

            # close log file
            self.file.close()
            pass
    
        if filename is not None:
            self.filename = filename
        
        #connect to file
        self.file = open(self.filename, 'a')
        self.connect_string = f'tcp://{ip}:{port}'
        self.network_context = zmq.Context()
        self.socket = self.network_context.socket(zmq.SUB)

        self.socket.bind (self.connect_string)
        self.socket.subscribe('')
        print(f"Conneted to logs: {self.connect_string}")
        self.connected = True
        return

    def StartLogging(self):
        if(self.recv_thread.is_alive()):
            self.StopLogging()
        self.recv_thread = Thread(target = self.fill_log_queue)
        self.recv_stopEvent = Event()
        self.recv_thread.start()
    
    def StopLogging(self):
        if(self.recv_thread.is_alive()):
            self.recv_thread.join()
            self.recv_stopEvent.clear()

    
    def disconnect(self):
        if not self.connected:
            print("Error: Not connected")
        
        self.file.close()
        self.socket.setsockopt( zmq.LINGER, 0 )  #  to avoid hanging infinitely
        self.socket.close()                      #  for all sockets & devices
        self.network_context.term() 
        self.connected = False

    def process_log(self,log_message):
        log_message.logs[0].log_level
        A = LogHolder(  log_message.logs[0].component_name,
                        log_message.logs[0].time_stamp,
                        log_message.logs[0].machine,
                        log_message.logs[0].log_message,
                        log_message.logs[0].level)
        return A


    def getString(self, A):
        return f"[{A.time_stamp}] - {A.machine}({A.component_name}) - [{self.level2Text(A.log_level)}] {A.log_message}"

    def fill_log_queue(self):
        if not self.connected:
            print("Error: not connected")

        logs = daoLogging_pb2.Logs()

        running = True
        while  running:
            recv_message = self.socket.recv()
            logs.ParseFromString(recv_message)
            A = self.process_log(logs)
            self.buffer.append(A)
            string = self.getString(A)
            #string = f"[{A.time_stamp}] - {A.machine}({A.component_name}) - [{self.level2Text(A.log_level)}] {A.log_message} \n"
            self.file.write(f"{string}\n")
            if self.recv_stopEvent.is_set():
                running = False

if __name__=="__main__":
    d = network_log(filename = "logs.log")
    d.connect("127.0.0.1",5555)

    d.recv_thread.start()

    try:
        while(True):
            if len(d.buffer) > 0:
                A = d.buffer.popleft()
            else:
                time.sleep(0.1)
    except KeyboardInterrupt:
        print("")
        print("Keyboard Interupt caught, exiting program")
        d.recv_stopEvent.set()
        print("Waiting for thread to end to join")
        d.recv_thread.join()
    


