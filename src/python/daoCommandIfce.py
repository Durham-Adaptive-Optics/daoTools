import sys
import daoCommand_pb2
import zmq
import time
from daoLog import daoLog
import logging


class daoCommandIfce:
    def __init__(self, ip, port, name='sendCommands', timeout=.5):
        self.ip = ip
        self.port = port
        self.local_timeout = timeout
        self.log = logging.getLogger(__name__)

        self.network_context = zmq.Context()
        self.network_socket = self.network_context.socket(zmq.REQ)
        self.connect_string = f'tcp://{self.ip}:{self.port}'
        self.log.trace(f"Connecting to: {self.connect_string}")
        self.network_socket.connect(self.connect_string)
        self.network_socket.setsockopt(zmq.RCVTIMEO, 200)


    def sendCommand(self,Command):
        Reply = daoCommand_pb2.ReplyMessage()
        message = Command.SerializeToString()
        self.log.info(f"Sending {self.CommandToString(Command.function)} to {self.ip}:{self.port} with args: {Command.payload}")
        mess = Command.SerializeToString()
        self.network_socket.send(mess)
        start = time.time()
        # print('Waiting for reply...')

        while True:
            try: 
                rep = self.network_socket.recv()
                Reply.ParseFromString(rep)
                return Reply.status, Reply.payload
            except zmq.ZMQError as e:
                if e.errno == zmq.EAGAIN:
                    pass # no message was ready (yet!)
            now = time.time()
            if(now-start >= self.local_timeout):
                self.log.error("timeout resetting socket")
                self.network_socket.close()
                self.network_socket = self.network_context.socket(zmq.REQ)
                self.network_socket.connect(self.connect_string)
                return 2, "Timeout"
    
    def check_status(self, status, payload):
        if(status == 0):
            self.log.info("Command Successful")
        elif(status == 1):
          self.log.error("Command failed")
        elif(status == 2):
            self.log.error("Timeout")
        else:
            self.log.error(f"Unknow respose: {status} - {payload}")


    def Exec(self, args):
        command = daoCommand_pb2.CommandMessage()
        command.function = daoCommand_pb2.CommandMessage.COMMAND.Value("EXEC")
        command.payload = args
        command.component = ""

        status, payload = self.sendCommand(command)
        self.check_status(status,payload)
        return status,payload

    def Setup(self, args):
        self.log.error("Setup command not implimented")
        return 

    def Update(self, args):
        self.log.error("Setup command not implimented")
        pass

    def Ping(self, args=None):
        command = daoCommand_pb2.CommandMessage()
        command.function = daoCommand_pb2.CommandMessage.COMMAND.Value("PING")
        command.payload = ""
        command.component = ""

        status, payload = self.sendCommand(command)
        self.check_status(status,payload)
        return status, payload

    def State(self, args):
        command = daoCommand_pb2.CommandMessage()
        command.function = daoCommand_pb2.CommandMessage.COMMAND.Value("STATE")
        command.payload = ""
        command.component = ""

        status, payload = self.sendCommand(command)
        self.check_status(status,payload)
        return status, payload


    def SetLogLevel(self, args):
        command = daoCommand_pb2.CommandMessage()
        command.function = daoCommand_pb2.CommandMessage.COMMAND.Value("SET_LOG_LEVEL")
        command.payload = command
        command.component = ''

        status, payload = self.sendCommand(command)
        self.check_status(status,payload)
        return status, payload

    def Query(self, args):
        command = daoCommand_pb2.CommandMessage()
        command.function = daoCommand_pb2.CommandMessage.COMMAND.Value("QUERY")
        command.payload = args
        command.component = ''

        status, payload = self.sendCommand(command)
        self.check_status(status,payload)
        return status, payload

    def Dump(self, args):
        command = daoCommand_pb2.CommandMessage()
        command.function = daoCommand_pb2.CommandMessage.COMMAND.Value("DUMP")
        command.payload = ""
        command.component = ""

        status, payload = self.sendCommand(command)
        self.check_status(status,payload)
        return status, payload

    def Other(self, args):
        command = daoCommand_pb2.CommandMessage()
        command.function = daoCommand_pb2.CommandMessage.COMMAND.Value("OTHER")
        command.payload = args
        command.component = ""

        status, payload = self.sendCommand(command)
        self.check_status(status,payload)
        return status, payload

    def CommandToString(self, command):
        if(command == 0):
            return "Exec"
        elif(command==1):
            return "Setup"
        elif(command==2):
            return "Update"            
        elif(command==3):
            return "Ping"
        elif(command==4):
            return "State"
        elif(command==5):
            return "SetLogLevel"
        elif(command==6):
            return "Dump"
        elif(command==7):
            return "Query"
        elif(command==8):
            return "Other"
        else:
            return f"Unknown command: {command}"


def getCommandList():
    return ["EXEC","SETUP","UPDATE", "PING","STATE", "SET_LOG_LEVEL", "DUMP", "QUERY", "OTHER"]

# Commands:
# EXEC state changes Init,Stop,Enable,Disable,Run,Idle,Recover
# SETUP - not implimented
# UPDATE -= not implimented
# PING - return PID if alive
# STATE - returns state
# SET_LOG_LEVEL accepts log level: TRACE, DEBUG,INFO,WARNING,ERROR,CRITICAL
# QUERY - query a variables value
# DUMP - call user_dump user can either dump through log or with reply string
# OTHER - calls def user_process_other(self, string): to be overloaded if required

if __name__=="__main__":

    logger = daoLog(__name__)
    ip ='127.0.0.1'
    # sys.argv[1]
    port = 5556 
    # int(sys.argv[2])
    # Command = sys.argv[3]
    # payload = sys.argv[4]


    A = daoCommandIfce(ip,port)
    print(A.Ping(""))
    print(A.State(""))
    print(A.Exec("Init"))
    # A.Exec("Init")








    # command = daoCommand_pb2.CommandMessage()
    # com = command.message.add()

    # Reply = daoCommand_pb2.Reply()

    # command.component = "sendCommand.py"
    # command.function = daoCommand_pb2.CommandMessage.COMMAND.Value(Command)
    # command.payload = payload
    
    # context = zmq.Context()
    # socket = context.socket(zmq.REQ)

    # socket.connect("tcp://" + ip + ":" + str(port) )
    # socket.setsockopt(zmq.RCVTIMEO, 200)
    # mess = command.SerializeToString()
    # print(f"Sending {Command} to {ip}:{port} with args: {command.payload}")
    # socket.send(mess)
    # start = time.time()
    # print('Waiting for reply...')

    # while True:
    #     try: 
    #         message = socket.recv()
    #         Reply.ParseFromString(message)
    #         for i in Reply.reply:
    #             if(i.status == 0):
    #                 print("Success")
    #             else:
    #                 print("Failure")
    #             print(i.payload)
    #         break
    #     except zmq.ZMQError as e:
    #         if e.errno == zmq.EAGAIN:
    #             pass # no message was ready (yet!)
    #     now = time.time()
    #     if(now-start >= 5):
    #         print("Command Timed out")
    #         exit()
