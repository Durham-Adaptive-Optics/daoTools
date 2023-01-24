from daoRecvLogs import network_log
import argparse
from time import sleep


if __name__=="__main__":
    parser = argparse.ArgumentParser(description='Take network logs from network and print to screen and file')
    parser.add_argument('-f', '--filename',
                        dest='log_file',
                        default='logs.log',
                        help='file to print logs to')  
    parser.add_argument('-i', '--ip',
                        dest='ip',
                        default='127.0.0.1',
                        help='the ip to listen on')     
    parser.add_argument('-p', '--port',
                        dest='port',
                        default='5555',
                        help='Port to listen on')                     
    args = parser.parse_args()

    d = network_log(filename = args.log_file)
    d.connect(args.ip,int(args.port))

    d.recv_thread.start()

    try:
        while(True):
            if len(d.buffer) > 0:
                A = d.buffer.popleft()
                print(d.getString(A))
            else:
                sleep(0.1)
    except KeyboardInterrupt:
        print("")
        print("Keyboard Interupt caught, exiting program")
        d.recv_stopEvent.set()
        print("Waiting for thread to end to join")
        d.recv_thread.join()