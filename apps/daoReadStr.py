#!/usr/bin/env python3
#
import dao
import numpy as np
import sys

shm = sys.argv[1]

replyShm = dao.shm(shm)

# Read uint8 array from SHM
arr = replyShm.get_data()

# Convert back to command string
reply = bytes(arr).split(b'\0', 1)[0].decode('utf8')
print(f"command = [{reply}]")