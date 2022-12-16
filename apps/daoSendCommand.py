import sys
import daoCommand_pb2
import zmq
import time



# Commands:
# EXEC state changes Init,Stop,Enable,Disable,Run,Idle,Recover
# SETUP - not implimented
# UPDATE -= not implimented
# PING - return PID if alive
# STATE - returns state
# SET_LOG_LEVEL accepts log level: TRACE, DEBUG,INFO,WARNING,ERROR ,FATAL
# QUERY - query a variables value
# DUMP - call user_dump user can either dump through log or with reply string
# OTHER - calls def user_process_other(self, string): to be overloaded if required

if __name__=="__main__":

    ip = sys.argv[1]
    port = int(sys.argv[2])
    Command = sys.argv[3]
    payload = sys.argv[4]

    command = daoCommand_pb2.Commands()
    com = command.message.add()

    Reply = daoCommand_pb2.Reply()

    com.component = "sendCommand.py"
    com.function = daoCommand_pb2.CommandMessage.COMMAND.Value(Command)
    com.payload = payload
    
    context = zmq.Context()
    socket = context.socket(zmq.REQ)

    socket.connect("tcp://" + ip + ":" + str(port) )
    socket.setsockopt(zmq.RCVTIMEO, 200)
    mess = command.SerializeToString()
    print(f"Sending {Command} to {ip}:{port} with args: {com.payload}")
    socket.send(mess)
    start = time.time()
    print('Waiting for reply...')

    while True:
        try: 
            message = socket.recv()
            Reply.ParseFromString(message)
            for i in Reply.reply:
                if(i.status == 0):
                    print("Success")
                else:
                    print("Failure")
                print(i.payload)
            break
        except zmq.ZMQError as e:
            if e.errno == zmq.EAGAIN:
                pass # no message was ready (yet!)
        now = time.time()
        if(now-start >= 5):
            print("Command Timed out")
            exit()
