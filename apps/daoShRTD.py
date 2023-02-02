#!/usr/bin/env python3

import numpy as np
import daoShm
import getopt
import sys
import time
import matplotlib.pyplot as plt

if __name__ == '__main__':
    shmimName = '/tmp/image.im.sh'
    shmcentName = '/tmp/centroids.im.sh'
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hi:c",["help", "shmimName=", "shmcentName"])
    except getopt.GetoptError:
      print('err, usage: doaShRTD.py -i <shmimName> -c <shmcentName>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoShRTD.py -s <shmimName>')
            sys.exit()
        elif opt in ("-i", "--shmimName"):
            shmimName = str(arg)
        elif opt in ("-c", "--shmcentName"):
            shmcentName = str(arg)
    shmim = daoShm.shm(shmimName)

    plt.ion()
    fig = plt.figure()
    ax = fig.add_subplot(111)
    a=plt.imshow(shmim.get_data())
    plt.colorbar()
    plt.show()
    while True:
        a.set_data(shmim.get_data())
        fig.canvas.draw()
        fig.canvas.flush_events()
        time.sleep(0.01)
