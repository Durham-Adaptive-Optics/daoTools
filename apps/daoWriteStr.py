#!/usr/bin/env python3
#
import dao
import numpy as np
import sys

shm=sys.argv[1]
command=sys.argv[2]
print(f"{command} to {shm}")
cmdShm=dao.shm(shm)
command+="\0" + "000000000000000000000000000000000000"
cmdShm.set_data(np.frombuffer(command.encode('utf8'), dtype=np.uint8))