#!/usr/bin/env python3
#
import dao
import numpy as np
import sys

shm=sys.argv[1]
command=sys.argv[2]
print(f"{command} to {shm}")
cmdShm=dao.shm(shm)
# set_data needs the full SHM size: zero-pad the null-terminated string
buf = np.zeros(cmdShm.get_data().size, dtype=np.uint8)
raw = command.encode('utf8')[:buf.size - 1]
buf[:len(raw)] = np.frombuffer(raw, dtype=np.uint8)
cmdShm.set_data(buf)