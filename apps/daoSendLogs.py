import sys, signal
import daoLogging_pb2
import zmq
import time
import numpy as np
import argparse
from datetime import datetime

LEVEL = [daoLogging_pb2.LogMessage.NOSET,
            daoLogging_pb2.LogMessage.TRACE, 
            daoLogging_pb2.LogMessage.DEBUG,
            daoLogging_pb2.LogMessage.INFO,
            daoLogging_pb2.LogMessage.WARNING,
            daoLogging_pb2.LogMessage.ERROR,
            daoLogging_pb2.LogMessage.CRITICAL]

def generate_random_log(frame):
    log_message = daoLogging_pb2.LogMessage()
    log_message.time_stamp = datetime.now().strftime("%d-%b-%Y (%H:%M:%S.%f)")
    log_message.component_name = 'Test_logger'
    log_message.machine = 'hrtc3'
    log_message.log_message = "Some log message"
    log_message.level=LEVEL[np.random.randint(len(LEVEL))]
    return log_message



if __name__=="__main__":
    parser = argparse.ArgumentParser(description='Send random network logs to network for testing')
    parser.add_argument('-i', '--ip',
                        dest='ip',
                        default='127.0.0.1',
                        help='the ip to listen on')     
    parser.add_argument('-p', '--port',
                        dest='port',
                        default='5555',
                        help='Port to listen on')                     
    args = parser.parse_args()
    # configure the zmq for Pub
    ip = args.ip
    port=args.port

    connect_string = f'tcp://{ip}:{str(port)}'
    print(f"Connecting to : {connect_string}")
    context = zmq.Context()
    socket = context.socket(zmq.PUB)
    socket.bind(connect_string)

    i = 0
    try:
        while(True):
            # configure some sort log messages
            log_message = generate_random_log(i)
            print(f"Sending message: {i}")

            sendString = log_message.SerializeToString()
            socket.send(sendString)
            time.sleep(1)
            i+=1
    except KeyboardInterrupt:
        print("")
        print("Keyboard Interupt caught, exiting program")

    print("Starting clean up")
