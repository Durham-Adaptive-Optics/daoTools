#!/usr/bin/env python3

import numpy as np
import shmlib
import getopt
import sys
import time
import matplotlib.pyplot as plt

if __name__ == '__main__':
    shmimName = 'WFSim.im.sh'
    axisFlip = False 
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hs:a",["help", "shmimName=", "axisFlip"])
    except getopt.GetoptError:
      print('err, usage: doaImageRTD.py -s <shmimName> -a')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoImageRTD.py -s <shmimName>')
            sys.exit()
        elif opt in ("-s", "--shm"):
            shmimName = str(arg)
        elif opt in ("-a", "--axisFlip"):
            axisFlip = True
    shm = shmlib.shm(shmimName)
    imageSizeX = shm.get_data().shape[0]
    imageSizeY = shm.get_data().shape[1]
    if axisFlip == True:
        imageSizeX = shm.get_data().shape[1]
        imageSizeY = shm.get_data().shape[0]

    plt.ion()
    fig = plt.figure()
    ax = fig.add_subplot(111)
    a=plt.imshow(shm.get_data().flatten().reshape((imageSizeX,imageSizeY)))
    plt.colorbar()
    plt.show()
    while True:
        a.set_data(shm.get_data().flatten().reshape((imageSizeX,imageSizeY)))
        fig.canvas.draw()
        fig.canvas.flush_events()
        time.sleep(0.01)
