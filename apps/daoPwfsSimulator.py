#!/usr/bin/env python3
'''
Script used to simulate the PWFS image from the commands applied to the DM
'''
import sys, getopt
import numpy as np
#import pyfits as pf
import astropy.io.fits as pf
import matplotlib.pyplot as plt
import time
import shmlib
import os
from threading import Thread
import datetime
import daoShm 
###########

if __name__ == '__main__':
    # WFS parameters
    mod = 5
    wvlngth = 1.65e-6
    v2m = 0.6e-6
    dmShmName = '/tmp/dm.im.shm'
    wsShmName = '/tmp/ws.im.shm'
    infFitsName = 'inf.fits'
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hd:w:",["help", "dm=", "ws="])
    except getopt.GetoptError:
      print('err, usage: pwfsSimulator.py -d <dmName> -w <wsName> -f <infFitsName>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoPwfsSimulator.py -d <dmName> -w <wsName> -f <infFitsName>')
            sys.exit()
        elif opt in ("-d", "--dm"):
            dmShmName = str(arg)
        elif opt in ("-w", "--ws"):
            wsShmName=str(arg)
        elif opt in ("-f", "--file"):
            infFitsName=str(arg)
    sys.stdout.write("daoPwfsSimulator for %s and %s"%(dmShmName, wsShmName))

    nPx = int(ws.pupSizeX)
    inf = pf.getdata(infFitsName)
    
    # DM and image shared memories
    dmShm=shmlib.shm(dmShmName)
    imShm=shmlib.shm(wsShmName)
    # Pupil positions
    pxShm=shmlib.shm('/tmp/'+dmShmName+'Quadx.im.shm')
    pyShm=shmlib.shm('/tmp/'+dmShmName+'Quady.im.shm')
    px = pxShm.get_data()
    py = pyShm.get_data()

    # Pupil
    pup = daoTools.pupil(nPx)

    # First relisation (don't wait for first frame)
    # Get DM commands
    cmds = v2m*dmShm.get_data()
    Z_in = np.squeeze(np.matmul(np.transpose(inf),np.reshape(cmds, [dm.sizeX * dm.sizeY,1])))

    # Compute PWS image
    pwfsFrame = (daoTools.pwfsImage(Z_in, wvlngth, mod, pup, ws.cam.sizeX))
    # Put simulated pupils in location set in shared memory
    spx = np.copy(px)
    spx[:,:] = [[13],[13],[71],[71]]     # Simulated positions
    spy = np.copy(py)
    spy[:,:] = [[13],[71],[13],[71]]
    frame = np.zeros(np.shape(pwfsFrame))
    for n in range(0,4):
        frame[px[n,0]:px[n,0]+nPx,py[n,0]:py[n,0]+nPx] = pwfsFrame[spx[n,0]:spx[n,0]+nPx,spy[n,0]:spy[n,0]+nPx]
    # Remove flat fielding factor
    # frame[:,0:32] = frame[:,0:32]/1.5
    imShm.set_data(frame.astype('uint16'))

    t0=time.time()
    cnt=11
    # Loop when DM changes
    while 1:
        # Get DM commands
        cmds = v2m*dmShm.get_data()
        Z_in = np.squeeze(np.matmul(np.transpose(inf),np.reshape(cmds,[dm.sizeX * dm.sizeY, 1])))

        # Compute PWS image
        pwfsFrame = (daoTools.pwfsImage(Z_in, wvlngth, mod, pup, ws.cam.sizeX))
        sys.stdout.write("\r%d "%(cnt))
        cnt=cnt+1
        # Put simulated pupils in location set in shared memory
        for n in range(0,4):
            pupN = pwfsFrame[spx[n,0]:spx[n,0]+nPx,spy[n,0]:spy[n,0]+nPx]
            # Add constant factor to get valid pixels
            #pupN[pup==1] = pupN[pup==1]
            frame[px[n,0]:px[n,0]+nPx,py[n,0]:py[n,0]+nPx] = pupN

        # Remove flat fielding factor
        # KECK detector simulation....
        #frame[:,0:32] = frame[:,0:32]/1.5
        #frame[0,0:6] = 0xDEAD
        #frame[0,6:13] = 0xBEEF
        #frame[0,13:20] = 0xDEAD
        #frame[0,20:26] = 0xBEEF
        #frame[0,26:32] = 0xDEAD
        imShm.set_data(frame.astype('uint16'))
        t1=time.time()
        sys.stdout.write("%.3f Hz"%(1/(t1-t0)))
        sys.stdout.flush()
        t0=t1

