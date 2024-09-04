#!/usr/bin/env python3
#
import dao
import numpy as np
import os
import astropy.io.fits as fits
import time
import matplotlib.pyplot as plt
import sys

if __name__ == '__main__':
    shm = sys.argv[1]
    nFrame = int(sys.argv[2])
    desc = sys.argv[3]
    baseName = shm.split('/')[-1].split('.')[0] 

    shm=dao.shm(shm)
    shmp = dao.shm(f"/tmp/{baseName}Percent.im.shm", np.zeros((1,1)).astype(np.uint32))
    dataSize = shm.get_data().shape
    dataType = shm.get_data().dtype

    dataCube = np.zeros((nFrame, dataSize[0], dataSize[1])).astype(dataType)

    for k in range(nFrame):
        sys.stdout.write(f"\racquiring... {k+1}/{nFrame}")
        sys.stdout.flush()
        dataCube[k,:,:] = shm.get_data(check=True, semNb=3)
        shmp.set_data(shmp.get_data()*0+np.uint32(100*(k+1)/nFrame))
    print('\n')

    fileName = f"{os.getenv("DAODATA")}/data/{baseName}Cube{desc}.npy"
    print(f"saving in {fileName}")
    np.save(f"{fileName}", dataCube)