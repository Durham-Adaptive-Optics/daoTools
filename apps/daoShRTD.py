#!/usr/bin/env python3

import numpy as np
import daoShm
import dao
import getopt
import sys
import time
import matplotlib.pyplot as plt

if __name__ == '__main__':
    shmimName = '/tmp/image.im.shm'
    shmcentName = '/tmp/centroids.im.shm'
    shmrefName = '/tmp/references.im.shm'
    shmpupName = '/tmp/pupMask1.im.shm'
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hi:c:r:p:",["help", "shmimName=", "shmcentName=", "shmrefName=", "shmpupName="])
    except getopt.GetoptError:
      print('err, usage: doaShRTD.py -i <shmimName> -c <shmcentName> -r <referenceName> -p <shmpupName>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoShRTD.py -i <shmimName> -c <shmcentName> -r <shmrefName> -p <shmpupName>')
            sys.exit()
        elif opt in ("-i", "--shmimName"):
            shmimName = str(arg)
        elif opt in ("-c", "--shmcentName"):
            shmcentName = str(arg)
        elif opt in ("-r", "--shmrefName"):
            shmrefName = str(arg)
        elif opt in ("-p", "--shmpupName"):
            shmpupName = str(arg)
    
    
    print(shmimName)
    print(shmimName)
    print(shmrefName)
    shmim = dao.shm(shmimName)
    shmcent = dao.shm(shmcentName)
    shmref = dao.shm(shmrefName)
    shmpup = dao.shm(shmpupName)
    pup=    shmpup.get_data()
    intMap=np.zeros(pup.shape)
    cent = shmcent.get_data()
    nCent =cent.shape[0]//4

    plt.ion()
    fig, ax = plt.subplots(1,2)
    im=shmim.get_data()
    iref=ax[0].imshow(im, cmap='gray')
    tref=ax[0].set_title(str(im.max()))
    cref=shmref.get_data()
    crefh,=ax[0].plot(cref[:nCent,:],cref[nCent:2*nCent,:],'+g')
    centh,=ax[0].plot(cref[:nCent,:] + cent[:nCent,:], cref[nCent:2*nCent,:] + cent[nCent:2*nCent,:],'.r')
    intVect = cent[2*nCent:3*nCent,0]
    intMap[pup==1]=intVect
    intref=ax[1].imshow(intMap, cmap='gray')
    
    while True:
        cref=shmref.get_data()
        cent=shmcent.get_data()
        im=shmim.get_data()
        iref.set_data(im)
        tref.set_text(str(im.max()))
        crefh.set_data(cref[:nCent,:], cref[nCent:2*nCent,:])
        centh.set_data(cref[:nCent,:] + cent[:nCent,:], cref[nCent:2*nCent,:] + cent[nCent:2*nCent,:])
        intVect = cent[2*nCent:3*nCent,0]
        intMap[pup==1]=intVect
        intref.set_data(intMap)
        fig.canvas.draw()
        fig.canvas.flush_events()
        time.sleep(0.1)
    
