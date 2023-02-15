#!/usr/bin/env python

from daoComponent import Component
import daoShm
from daoLog import daoLog
from daoDoubleBuffer import DoubleBuffer
import time

import numpy as np


class redisCheck(Component):
    def __init__(self, name=__name__, config=None, port=5557):
        super().__init__(name, config, port)
        pass
    
        return

    def load_static_config(self):
        # put any config load.
        # here we decifer what ever config was provided.
        # this can be a dictionary yaml, json. We have no requirements.
        self.log.trace("load_static_config()")

        # opening connection to Redit:
        self.redis = [] 
        self.redis_key = ''


        self.input_file = "/tmp/cal.im.shm"
        self.input_shm       = daoShm.shm(self.input_file)



        # self.subApMap   = DoubleBuffer()
        # self.add_update_map(self.subApMap_shm, self.subApMap)

        # add to dictionary for querying
        # we can probably be clever and use the actual variable names to self populate this but for now this will work
        self.variable_dictionary.update({'input_file' : self.input_file})

        return
    
    def load_dynamic_config(self):
        self.log.trace("load_dynamic_config")
    
        # take current Redis data and put into SHM

        return

    def user_dump(self):
        try:
            payload = "\n"
            payload += f"input shm file   {self.input_file}\n"
            payload += f"output shm file: {self.output_file}\n"
            payload += f"subApMap shm file: {self.subApMap_file}\n"
            self.log.info(payload)
        except Exception as e:
            payload =f"\n{e}"
        return payload


    # here we can allow the user to do processing nothing special
    # this should be over
    def processingThread(self):
        self.log.trace("Entering running thread")
        Running = True
        while Running:
            # if update on Redis

            # get data

            # save to SHM

            if self.procThreadStop.is_set():
                Running = False
            
        return

if __name__=="__main__":
    name = "redisCheck"
    ip = "127.0.0.1"
    logger = daoLog(name=name, ip=ip, port=5555)

    A = redisCheck(name)

    while True:
        try:
            time.sleep(1)
        except KeyboardInterrupt:
            print('interrupted!')
            A.stop()
            break
