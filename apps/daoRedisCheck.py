#!/usr/bin/env python

from daoComponent import daoComponent
import daoShm
from daoLog import daoLog
from daoDoubleBuffer import DoubleBuffer
import time
import sys
import numpy as np
from daoDb import *


class redisCheck(daoComponent):
    def __init__(self, name=__name__, config=None, port=5557, redisIp='127.0.0.1', redisPort=6379, input_file='/tmp/rec.im.shm', key='test'):
        self.redisIp=redisIp
        self.redisPort=redisPort
        self.input_file=input_file
        self.redis_key = key
        super().__init__(name, config, port)
        pass
    
        return

    def load_static_config(self):
        # put any config load.
        # here we decifer what ever config was provided.
        # this can be a dictionary yaml, json. We have no requirements.
        self.log.trace("load_static_config()")

        # opening connection to Redit:
        self.redis = redis.Redis(host=self.redisIp, port=self.redisPort)
        self.input_shm = daoShm.shm(self.input_file)

        # add to dictionary for querying
        # we can probably be clever and use the actual variable names to self populate this but for now this will work
        self.variable_dictionary.update({'input_file' : self.input_file})

        return
    
    def load_dynamic_config(self):
        self.log.trace("load_dynamic_config")
    
        # take current Redis data and put into SHM
        self.val, self.meta = get_numpy_array_from_redis(self.redis, self.redis_key)
        self.input_shm.set_data(self.val.astype(np.float32))
        self.input_shm.set_counter(int(self.meta['uid']))

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
        # expect a message
        p = self.redis.pubsub()
        p.subscribe(f'{self.redis_key}:message')

        while Running:
            # if update on message
            message = p.get_message()
            if message:
                self.log.debug("new data in the db")
                # get data
                self.val, self.meta = get_numpy_array_from_redis(self.redis, self.redis_key)
                # save to SHM
                self.input_shm.set_data(self.val)
                self.input_shm.set_counter(int(self.meta['uid']))
            time.sleep(0.1)
            if self.procThreadStop.is_set():
                Running = False
            
        return

if __name__=="__main__":
    scriptName = sys.argv[0]
    shmName = sys.argv[1]
    redisIp = sys.argv[2]
    redisPort = int(sys.argv[3])
    key = sys.argv[4]
    topicName = f'redisCheck_{key}'
    logger = daoLog(name=topicName, filename=f"/tmp/redisCheck_{key}.log")

    A = redisCheck(topicName, redisIp=redisIp, redisPort=redisPort, input_file=shmName, key=key)

    while True:
        try:
            time.sleep(1)
        except KeyboardInterrupt:
            print('interrupted!')
            A.stop()
            break
