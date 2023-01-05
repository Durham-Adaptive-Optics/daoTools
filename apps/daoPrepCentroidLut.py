#!/usr/bin/env python3

import numpy as np
import daoShm
import getopt
import sys
import time

if __name__ == '__main__':
    imShmName = '/tmp/image.im.sh'
    imShmName = '/tmp/lut.im.sh'
    nbSuba = 1
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hi:n:l:",["help", "imShm=", "nbSuba=", "lutShm"])
    except getopt.GetoptError:
      print('err, usage: daoPrepCentroidLut.py -i <imShm> -n <nbSuba> -l <lutShm>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoPrepCentroidLut.py -i <imShm> -n <nbSuba> -l <lutShm>')
            sys.exit()
        elif opt in ("-i", "--imShm"):
            imShmName = str(arg)
        elif opt in ("-n", "--nbSuba"):
            nbSuba = int(arg)
        elif opt in ("-l", "--lutShm"):
            lutShmName = str(arg)

    imShm = daoShm.shm(imShmName)

    detSize = imShm.get_data().shape[0]
    maxSubaSize = int(np.floor(detSize/nbSuba))

    lut = np.linspace(0, detSize**2-1, detSize**2).reshape((detSize,detSize))
    lutMat = np.zeros((nbSuba**2, maxSubaSize**2)).astype(np.uint32)

    k=1
    for i in np.linspace(0,detSize-maxSubaSize, nbSuba).astype(np.int32):
        for j in np.linspace(0,detSize-maxSubaSize, nbSuba).astype(np.int32):
            #print("suba %d = [%d:%d, %d:%d]" %(k, i,i+maxSubaSize,j,j+maxSubaSize))
            lutMat[k-1,:] = lut[i:i+maxSubaSize, j:j+maxSubaSize].flatten().astype(np.uint32)
            k+=1
    
    lutShm = daoShm.shm(lutShmName, lutMat)