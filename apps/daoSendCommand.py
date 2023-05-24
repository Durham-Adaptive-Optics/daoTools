#!/usr/bin/env python3

import sys
import time
from daoLog import daoLog
import logging
from daoCommandIfce import daoCommandIfce
import argparse


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
    logger = daoLog(__name__)
    log = logging.getLogger(__name__)
    log.setLevel(logging.TRACE)

    parser = argparse.ArgumentParser(description='daoSendCommand')
    # ip = sys.argv[1]
    # port = int(sys.argv[2])
    # Command = sys.argv[3]
    # payload = sys.argv[4]

    parser.add_argument('-i', '--ip',
                        dest='ip',
                        default='127.0.0.1',
                        help='the ip to send command') 
    parser.add_argument('-p', '--port',
                        dest='port',
                        default='5555',
                        help='Port to listen on')
    parser.add_argument('-c', '--command',
                        dest='command',
                        default='PING',
                        help='Command to send')
    parser.add_argument('-a','--args',  dest = 'args', help='Additional arguments')

    # Parse the arguments
    args = parser.parse_args()
    
    # Access the argument values
    ip = args.ip
    port = args.port
    command = args.command
    additional_args = args.args
    log.debug(f"{ip}:{port} {command} {additional_args}")
    ifce = daoCommandIfce(ip,port)


    # check command is a valid one
    if command == "EXEC":
        ret = ifce.Exec(additional_args)
    elif(command == "SETUP"):
        ret = ifce.Setup(additional_args)
    elif(command == "UPDATE"):
        ifce.Update(additional_args)
    elif(command == "PING"):
        ret = ifce.Ping(additional_args)
    elif(command == "STATE"):
        ifce.State(additional_args)
    elif(command == "SET_LOG_LEVEL"):
        ret = ifce.SetLogLevel(additional_args)
    elif(command == "QUERY"):
        ret = ifce.Query(additional_args)
    elif(command == "DUMP"):
        ret = ifce.Dump(additional_args)
    elif(command == "OTHER"):
        ret = ifce.Other(additional_args)
    elif(command == "UNKNOWN"):
        log.error("Unkown command")

    if(ret[0] == 0):
        log.info(f"Success: {ret[1]}")
    else:
        log.error(f"Error: {ret[0]} : {ret[1]}")
