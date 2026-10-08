#!/usr/bin/env python3
#
import dao
import numpy as np
import sys
import time

dao.setLogLevel(0)
shmName=sys.argv[1]
command=sys.argv[2]
cmdShm=dao.shm(f"/tmp/{shmName}SCmd.im.shm")
replyShm=dao.shm(f"/tmp/{shmName}SRsp.im.shm")
print(f"Writing {command}")
# set_data needs the full SHM size: zero-pad the null-terminated string
buf = np.zeros(cmdShm.get_data().size, dtype=np.uint8)
raw = command.encode('utf8')[:buf.size - 1]
buf[:len(raw)] = np.frombuffer(raw, dtype=np.uint8)
cmdShm.set_data(buf)
time.sleep(0.5)
# Read uint8 array from SHM
arr = replyShm.get_data()
# Convert back to command string
reply = bytes(arr).split(b'\0', 1)[0].decode('utf8')
print(f"{reply}")
