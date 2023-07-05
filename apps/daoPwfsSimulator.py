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
import os
from threading import Thread
import datetime
import dao 
import daoTools
###########

#DM
mapArray=np.array([
[0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0],
[0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0],
[0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0],
[0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0],
[0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0],
[0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0],
[0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0],
[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],
[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],
[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],
[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],
[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],
[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],
[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],
[0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0],
[0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0],
[0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0],
[0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0],
[0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0],
[0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0],
[0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0]])

nbAct = mapArray.sum()
sizeX = mapArray.shape[0]
sizeY = mapArray.shape[1]

# Define the Pyramid geometry
quadrantSize = 7744
pupSizeX = 44
pupSizeY = 44

detSizeX=128
detSizeY=128

# define position of the quadrant into the detector
quadPosX = np.zeros([4,1])
quadPosX[0,0]=7
quadPosX[1,0]=6
quadPosX[2,0]=73
quadPosX[3,0]=72

quadPosY = np.zeros([4,1])
quadPosY[0,0]=9
quadPosY[1,0]=75
quadPosY[2,0]=10
quadPosY[3,0]=76
# WFS parameters
mod = 5
wvlngth = 1.65e-6
v2m = 0.6e-6
dmShmName = '/tmp/dm.im.shm'
dmMapShmName = '/tmp/dmMap.im.shm'
wsShmName = '/tmp/ws.im.shm'
infFitsName = os.getenv('DAODATA')+'/dmInf.fits'

if __name__ == '__main__':

    sys.stdout.write("daoPwfsSimulator for %s and %s"%(dmShmName, wsShmName))
    # DM and image shared memories
    dmShm=dao.shm(dmShmName, np.zeros((nbAct, 1)).astype(np.float32))
    dmMapShm=dao.shm(dmMapShmName, mapArray)
    imShm=dao.shm(wsShmName, np.zeros((detSizeX,detSizeY)).astype(np.uint16))
    # Load influence funciton
    nPx = int(pupSizeX)
    inf = pf.getdata(infFitsName)
    
    # Pupil positions
    px = quadPosX
    py = quadPosY

    # Pupil
    pup = daoTools.pupil(nPx)

    # First relisation (don't wait for first frame)
    # Get DM commands
    cmds = dmMapShm.get_data().flatten().astype(np.float32)
    cmds[dmMapShm.get_data().flatten() == 1]=v2m*dmShm.get_data()[:,0]
    #cmds = v2m*dmShm.get_data()
    Z_in = np.squeeze(np.matmul(np.transpose(inf),np.reshape(cmds, [sizeX * sizeY,1])))

    # Compute PWS image
    pwfsFrame = (daoTools.pwfsImage(Z_in, wvlngth, mod, pup, detSizeX))
    imShm.set_data(pwfsFrame.astype(np.uint16))
#    # Put simulated pupils in location set in shared memory
#    spx = np.copy(px)
#    spx[:,:] = [[13],[13],[71],[71]]     # Simulated positions
#    spy = np.copy(py)
#    spy[:,:] = [[13],[71],[13],[71]]
#    frame = np.zeros(np.shape(pwfsFrame))
#    for n in range(0,4):
#        frame[px[n,0]:px[n,0]+nPx,py[n,0]:py[n,0]+nPx] = pwfsFrame[spx[n,0]:spx[n,0]+nPx,spy[n,0]:spy[n,0]+nPx]
#    # Remove flat fielding factor
#    # frame[:,0:32] = frame[:,0:32]/1.5
#    imShm.set_data(frame.astype('uint16'))
#
    t0=time.time()
    cnt=0
    # Loop when DM changes
    while 1:
        # Get DM commands
        cmds = dmMapShm.get_data().flatten().astype(np.float32)
        cmds[dmMapShm.get_data().flatten() == 1]=v2m*dmShm.get_data(check=True)[:,0]
        Z_in = np.squeeze(np.matmul(np.transpose(inf),np.reshape(cmds,[sizeX * sizeY, 1])))

        # Compute PWS image
        pwfsFrame = (daoTools.pwfsImage(Z_in, wvlngth, mod, pup, detSizeX))
        sys.stdout.write("\r%d "%(cnt))
        cnt=cnt+1
#        # Put simulated pupils in location set in shared memory
#        for n in range(0,4):
#            pupN = pwfsFrame[spx[n,0]:spx[n,0]+nPx,spy[n,0]:spy[n,0]+nPx]
#            # Add constant factor to get valid pixels
#            #pupN[pup==1] = pupN[pup==1]
#            frame[px[n,0]:px[n,0]+nPx,py[n,0]:py[n,0]+nPx] = pupN
#
#        # Remove flat fielding factor
#        # KECK detector simulation....
#        #frame[:,0:32] = frame[:,0:32]/1.5
#        #frame[0,0:6] = 0xDEAD
#        #frame[0,6:13] = 0xBEEF
#        #frame[0,13:20] = 0xDEAD
#        #frame[0,20:26] = 0xBEEF
#        #frame[0,26:32] = 0xDEAD
        imShm.set_data(pwfsFrame.astype('uint16'))
        t1=time.time()
        sys.stdout.write("%.3f Hz"%(1/(t1-t0)))
        sys.stdout.flush()
#        t0=t1
#
#