#!/usr/bin/env python3

import numpy as np
import daoShm
import daoTools
import getopt
import sys
import time

if __name__ == '__main__':
    imShmName = '/tmp/image.im.sh'
    lutShmName = '/tmp/lut.im.sh'
    centroidShmName = '/tmp/centroids.im.sh'
    nbSuba = 1
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hi:l:c:",["help", "imShm=", "lutShm", "centroidShmName"])
    except getopt.GetoptError:
      print('err, usage: daoComputeCentroids.py -i <imShm> -l <lutShm> -c <centroidShm>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoComputeCentroids.py -i <imShm> -l <lutShm> -c <centroidShm>')
            sys.exit()
        elif opt in ("-i", "--imShm"):
            imShmName = str(arg)
        elif opt in ("-l", "--lutShm"):
            lutShmName = str(arg)
        elif opt in ("-c", "--centroidShm"):
            centroidShmName = str(arg)

    imShm = daoShm.shm(imShmName)
    lutShm = daoShm.shm(lutShmName)
    nbSuba = int(np.sqrt(lutShm.get_data().shape[0]))
    subaSize = int(np.sqrt(lutShm.get_data().shape[1]))
    centroid=np.zeros((2,nbSuba**2) )
    # create centroid SHM
    centroidShm = daoShm.shm(centroidShmName, centroid)
    t1 = time.time()
    while 1:
        imFlat = imShm.get_data(check=True).flatten()
        t0=t1
        for k in range(nbSuba**2):
            centroid[0,k],centroid[1,k] = daoTools.cog(imFlat[lutShm.get_data()[k,:]].reshape(subaSize,subaSize))
        t1 = time.time()
        sys.stdout.write("\rcomp Time = %.6f"%(t1-t0))
        sys.stdout.flush()
        centroidShm.set_data(centroid)

     