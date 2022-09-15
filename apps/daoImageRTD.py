#!/usr/bin/env python3

import numpy as np
import shmlib
import getopt
import sys
import time
import matplotlib.pyplot as plt

if __name__ == '__main__':
    shmimName = 'WFSim.im.sh'
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hs:",["help", "shmimName="])
    except getopt.GetoptError:
      print('err, usage: doaImageRTD.py -s <shmimName>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoImageRTD.py -s <shmimName>')
            sys.exit()
        elif opt in ("-s", "--shm"):
            shmimName = str(arg)
    shm = shmlib.shm(shmimName)
    plt.ion()
    fig = plt.figure()
    ax = fig.add_subplot(111)
    a=plt.imshow(shm.get_data())
    plt.colorbar()
    plt.show()
    while True:
        a.set_data(shm.get_data())
        fig.canvas.draw()
        fig.canvas.flush_events()
        time.sleep(0.05)
