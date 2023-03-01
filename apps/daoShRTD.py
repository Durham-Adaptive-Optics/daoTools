#!/usr/bin/env python3

import numpy as np
import daoShm
import getopt
import sys
import time
import matplotlib.pyplot as plt

if __name__ == '__main__':
    shmimName = '/tmp/image.im.shm'
    shmcentName = '/tmp/centroids.im.shm'
    shmrefName = '/tmp/references.im.shm'
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hi:c:r",["help", "shmimName=", "shmcentName=", "shmrefName="])
    except getopt.GetoptError:
      print('err, usage: doaShRTD.py -i <shmimName> -c <shmcentName> -r <referenceName>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoShRTD.py -s <shmimName> -c<shmcentName> - r<shmrefName>')
            sys.exit()
        elif opt in ("-i", "--shmimName"):
            shmimName = str(arg)
        elif opt in ("-c", "--shmcentName"):
            shmcentName = str(arg)
        elif opt in ("-r", "--shmrefName"):
            shmrefName = str(arg)
    shmim = daoShm.shm(shmimName)
    shmcent = daoShm.shm(shmcentName)
    shmref = daoShm.shm(shmrefName)

    plt.ion()
    fig, ax = plt.subplots()
    iref=ax.imshow(shmim.get_data(), cmap='gray')
    cref=shmref.get_data()
    crefh,=ax.plot(cref[:,0],cref[:,1],'+g')
    #cent = shmcent.get_data()
    cent=cref+shmcent.get_data();
    centh,=ax.plot(cent[:,0], cent[:,1],'.r')
    
    while True:
        cref=shmref.get_data()
        cent=cref+shmcent.get_data();
        iref.set_data(shmim.get_data())
        crefh.set_data(cref[:,0], cref[:,1])
        centh.set_data(cent[:,0], cent[:,1])
        fig.canvas.draw()
        fig.canvas.flush_events()
        time.sleep(0.1)
    
