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
    nbSuba = 1
    subaSize = 10
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hi:s:n:c:",["help", "imShm=", "subaSize=", "nbSUba=","centroidShmName"])
    except getopt.GetoptError:
      print('err, usage: daoComputeCentroids.py -i <imShm> -s <subaSize> -n <nbSuba> -c <centroidShm>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoComputeCentroids.py -i <imShm>  -s <subaSize> -n <nbSuba> -c <centroidShm>')
            sys.exit()
        elif opt in ("-i", "--imShm"):
            imShmName = str(arg)
        elif opt in ("-s", "--subaSize"):
            subaSize = int(arg)
        elif opt in ("-n", "--nbSuba"):
            nbSuba = int(arg)
        elif opt in ("-c", "--centroidShm"):
            centroidShmName = str(arg)

    imShm = daoShm.shm(imShmName)
    centroid = np.zeros((nbSuba**2, 2))
    # create centroid SHM
    centroidShm = daoShm.shm(centroidShmName, centroid)

    # Create SH WFS
    shWfs = daoTools.ShackHartmannWFS(nbSuba, nbSuba, subaSize)

    t1 = time.time()
    while 1:
        im = imShm.get_data(check=True)
        t0=t1
        centroid =  shWfs.compute_centroids(im)
        t1 = time.time()
        sys.stdout.write("\rcomp Time = %.6f"%(t1-t0))
        sys.stdout.flush()
        centroidShm.set_data(centroid)

     