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
    print('Fits to shm tool.')

    fitsName = 'test.fits'    
    shmName = '/tmp/test.im.shm'
    pauseTime = 0.1

    try:
        opts, args = getopt.getopt(sys.argv[1:],"hf:s:p:",["help", "fits=", "shm=", "pause="])
    except getopt.GetoptError:
      print('err, usage: daoFits2Shm.py -f <fits file> -s <shm name> -p <pause>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoFits2Shm.py -f <fits name> -s <shm name> =p <pause>')
            sys.exit()
        elif opt in ("-f", "--fits"):
            fitsName = str(arg)
        elif opt in ("-s", "--shm"):
            shmName = str(arg)
        elif opt in ("-p", "--pause"):
            pauseTime = float(arg)

    data = fits.getdata(fitsName)
    shm = dao.shm(shmName, data[:,:,0])       

    while 1:
        for k in range(data.shape[2]):
            sys.stdout.write(f"\rwriting image {k}/{data.shape[2]}")
            sys.stdout.flush()
            shm.set_data(np.copy(data[:,:,k]))
            time.sleep(pauseTime)
