#!/usr/bin/env python3

import sys, getopt
import numpy as np
from astropy.io import fits

import time
import datetime
import dao

import os
os.nice(10)

if __name__ == '__main__':
    print('Super basic MVM script.')

    shmName = '/tmp/test.im.shm'
    pauseTime = 0.1

    try:
        opts, args = getopt.getopt(sys.argv[1:],"hi:r:o:",["help", "input=", "recon=", "output="])
    except getopt.GetoptError:
      print('err, usage: daoReconstructor.py -i <input shm> -r <recon shm> -o <output shm>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoReconstructor.py -i <input shm> -r <recon shm> -o <output shm>')
            sys.exit()
        elif opt in ("-i", "--input"):
            inShmName = str(arg)
        elif opt in ("-r", "--recon"):
            reconShmName = str(arg)
        elif opt in ("-o", "--output"):
            outShmName = str(arg)

    inputShm = dao.shm(inShmName)       
    reconShm = dao.shm(reconShmName)       
    outputShm = dao.shm(outShmName)       

    while 1:
        sys.stdout.write(f"\rprocessing frame {outputShm.get_counter()}")
        sys.stdout.flush()
        outputShm.set_data(np.matmul(reconShm.get_data(), inputShm.get_data(check=True)))