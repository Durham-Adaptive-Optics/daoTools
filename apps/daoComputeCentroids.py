#!/usr/bin/env python3

import numpy as np
import daoShm
import daoTools
import getopt
import sys
import time

if __name__ == '__main__':
    imShmName = '/tmp/image.im.sh'
    centroidShmName = '/tmp/centroids.im.sh'
    thresholdShmName = '/tmp/threshold.im.shm' 
    nbSuba = 1
    subaSize = 10
    offset = 10
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hi:t:o:s:n:c:",["help", "imShm=", "thresholdShm=", "offset=", "subaSize=", "nbSuba=","centroidShmName"])
    except getopt.GetoptError:
      print('err, usage: daoComputeCentroids.py -i <imShm> -t <thresholdShm> -o <offset> -s <subaSize> -n <nbSuba> -c <centroidShm>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoComputeCentroids.py -i <imShm> -t <thresholdShm> -o <offset> -s <subaSize> -n <nbSuba> -c <centroidShm>')
            sys.exit()
        elif opt in ("-i", "--imShm"):
            imShmName = str(arg)
        elif opt in ("-i", "--thresholdShm"):
            thresholdShmName = str(arg)
        elif opt in ("-o", "--offset"):
            offset = int(arg)
        elif opt in ("-s", "--subaSize"):
            subaSize = int(arg)
        elif opt in ("-n", "--nbSuba"):
            nbSuba = int(arg)
        elif opt in ("-c", "--centroidShm"):
            centroidShmName = str(arg)

    imSize = nbSuba * subaSize
    imShm = daoShm.shm(imShmName)
    thresholdShm = daoShm.shm(thresholdShmName)

    centroid = np.zeros((nbSuba**2, 2))
    # create centroid SHM
    centroidShm = daoShm.shm(centroidShmName, centroid)

    # Create SH WFS
    shWfs = daoTools.ShackHartmannWFS(nbSuba, nbSuba, subaSize)

    t1 = time.time()
    while 1:
        im = imShm.get_data(check=True)
        im[im < thresholdShm.get_data()[0,0]] = 0
        t0=t1
        centroid =  shWfs.compute_centroids(im[offset:offset+imSize, offset:offset+imSize])
        #centroid =  shWfs.compute_centroids(im)
        t1 = time.time()
        sys.stdout.write("\rcomp Time = %.6f"%(t1-t0))
        sys.stdout.flush()
        centroidShm.set_data(centroid)

     