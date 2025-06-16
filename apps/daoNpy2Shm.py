#!/usr/bin/env python3

import sys, getopt
import numpy as np

import time
import datetime
import dao

import os
os.nice(10)

if __name__ == '__main__':
    print('Npy to shm tool.')

    npyName = 'test.npy'    
    shmName = '/tmp/test.im.shm'
    pauseTime = 0.1

    try:
        opts, args = getopt.getopt(sys.argv[1:],"hn:s:p:",["help", "npy=", "shm=", "pause="])
    except getopt.GetoptError:
      print('err, usage: daoNpy2Shm.py -n <npy file> -s <shm name> -p <pause>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoNpy2Shm.py -n <npy name> -s <shm name> =p <pause>')
            sys.exit()
        elif opt in ("-n", "--npy"):
            npyName = str(arg)
        elif opt in ("-s", "--shm"):
            shmName = str(arg)
        elif opt in ("-p", "--pause"):
            pauseTime = float(arg)

    data = np.load(npyName)
    shm = dao.shm(shmName, data[0,:,:])       

    while 1:
        for k in range(data.shape[0]):
            sys.stdout.write(f"\rwriting image {k}/{data.shape[0]}")
            sys.stdout.flush()
            shm.set_data(np.copy(data[k,:,:]))
            time.sleep(pauseTime)
