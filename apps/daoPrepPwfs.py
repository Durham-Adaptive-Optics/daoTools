#!/usr/bin/env python3

import sys, getopt
import numpy as np
import astropy.io.fits as pf
import time
import os
import dao
import daoTools

detSizeX=128
detSizeY=128

bgShm=dao.shm('/tmp/wsBg.im.shm', np.zeros((detSizeX,detSizeY)).astype(np.float32))
ffShm=dao.shm('/tmp/wsFf.im.shm', np.ones((detSizeX,detSizeY)).astype(np.float32))
fluxShm=dao.shm('/tmp/wsFlux.im.shm', np.zeros((1,1)).astype(np.float32))

quadPosX = np.zeros([4,1])
quadPosX[0,0]=13
quadPosX[1,0]=13
quadPosX[2,0]=71
quadPosX[3,0]=71
#quadPosX[0,0]=7
#quadPosX[1,0]=6
#quadPosX[2,0]=73
#quadPosX[3,0]=72
quadxshm=dao.shm('/tmp/quadx.im.shm', quadPosX)
quadPosY = np.zeros([4,1])
quadPosY[0,0]=13
quadPosY[1,0]=71
quadPosY[2,0]=13
quadPosY[3,0]=71
#quadPosY[0,0]=9
#quadPosY[1,0]=75
#quadPosY[2,0]=10
#quadPosY[3,0]=76

quadyshm=dao.shm('/tmp/quady.im.shm', quadPosY)
syShm = dao.shm('/tmp/wsThreshold.im.shm', np.zeros((1,1)).astype(np.float32))

pupSize = 44
fluxShm=dao.shm('/tmp/fluxCal.im.shm', np.zeros((2*pupSize,2*pupSize)).astype(np.float32))
imageSize = 128
# Pupil
wfsMapArray = daoTools.pupil(pupSize)
pixid = np.copy(wfsMapArray)
pixid[pixid==0] = -1
pixid[pixid!=-1] = np.linspace(0,wfsMapArray.sum().astype(np.int32)-1,wfsMapArray.sum().astype(np.int32)).astype(np.int32)
pixidShm = dao.shm('/tmp/pixid.im.shm', pixid.astype(np.int32))

pixidMap=np.linspace(0, pupSize*pupSize-1, pupSize*pupSize).reshape(pupSize, pupSize).astype(np.int32)
pixidMap[pixid==-1] = -1
pixidMapShm = dao.shm('/tmp/pixidMap.im.shm', pixidMap)


pup=-1*np.ones([imageSize,imageSize]).astype(np.int32)
pup2=np.zeros([imageSize,imageSize]).astype(np.int32)
quadx=quadxshm.get_data().astype(np.int32)
quady=quadyshm.get_data().astype(np.int32)

quadId = np.zeros([2*pupSize ,2*pupSize]).astype(np.int32)
idIm=0
for j in range(0,2*pupSize):
    for k in range(0,2*pupSize):
        quadId[j,k] = idIm
        idIm = idIm +1

pup[quadx[0,0]:quadx[0,0]+pupSize ,quady[0,0]:quady[0,0]+pupSize ] = quadId[0:pupSize ,0:pupSize ]
pup[quadx[1,0]:quadx[1,0]+pupSize ,quady[1,0]:quady[1,0]+pupSize ] = quadId[0:pupSize ,pupSize :2*pupSize ]
pup[quadx[2,0]:quadx[2,0]+pupSize ,quady[2,0]:quady[2,0]+pupSize ] = quadId[pupSize :2*pupSize ,0:pupSize ]
pup[quadx[3,0]:quadx[3,0]+pupSize ,quady[3,0]:quady[3,0]+pupSize ] = quadId[pupSize :2*pupSize ,pupSize :2*pupSize ]

pup2[quadx[0,0]:quadx[0,0]+pupSize ,quady[0,0]:quady[0,0]+pupSize ] = wfsMapArray
pup2[quadx[1,0]:quadx[1,0]+pupSize ,quady[1,0]:quady[1,0]+pupSize ] = wfsMapArray
pup2[quadx[2,0]:quadx[2,0]+pupSize ,quady[2,0]:quady[2,0]+pupSize ] = wfsMapArray
pup2[quadx[3,0]:quadx[3,0]+pupSize ,quady[3,0]:quady[3,0]+pupSize ] = wfsMapArray

#dao.createFits(pup, os.getenv('DAODATA')+'/pupilMask.fits')
#dao.createFits(pup2, os.getenv('DAODATA')+'/pupilMask2.fits')

pupshm=dao.shm('/tmp/pupMask.im.shm', pup.astype(np.int32))
pup2shm=dao.shm('/tmp/pupMaskCircular.im.shm', pup2.astype(np.int32))

sxShm = dao.shm('/tmp/slopes.im.shm', np.zeros((wfsMapArray.sum().astype(np.int32),2)).astype(np.float32))
srefShm = dao.shm('/tmp/slopesRef.im.shm', np.zeros((wfsMapArray.sum().astype(np.int32),2)).astype(np.float32))


recShm = dao.shm('/tmp/reconstructor.im.shm', np.zeros((wfsMapArray.sum().astype(np.int32)*2, 349)).astype(np.float32))