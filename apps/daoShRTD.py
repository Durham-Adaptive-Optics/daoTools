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
    shmcent = daoShm.shm(shmcentName)

    offset = 19
    plt.ion()
    fig, ax = plt.subplots()
    iref=ax.imshow(shmim.get_data(), cmap='gray')
    #cref=ax.plot(a.subaperture_center_coordinates[0].flatten()+offset,a.subaperture_center_coordinates[1].flatten()+offset,'+b')
    cent = shmcent.get_data()
    centh=ax.plot(cent[:,0]+offset, cent[:,1]+offset,'.r')
    
    # draw gridlines
    ax.grid(which='major', axis='both', linestyle='-', color='k', linewidth=2)
    ax.set_xticks(np.arange(offset-0.5, 450+offset-0.5+15, 15));
    ax.set_yticks(np.arange(offset-0.5, 450+offset-0.5+15, 15));

    #fig = plt.figure()
    #ax = fig.add_subplot(111)
    #a=plt.imshow(shmim.get_data())
    #plt.colorbar()
    #plt.show()
    while True:
        iref.set_data(shmim.get_data())
        cent = shmcent.get_data()
        iref.set_data(shmim.get_data())
        #centh.set_data(cent[:,0]+offset, cent[:,1]+offset)
        centh.set_data(cent[:,0]+offset, cent[:,1]+offset)
        fig.canvas.draw()
        fig.canvas.flush_events()
        time.sleep(0.1)
