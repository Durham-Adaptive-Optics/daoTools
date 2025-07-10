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

    width = int(sys.argv[4])
    cx = int(sys.argv[5])
    cy = int(sys.argv[6])
    
#    fileName = f"{os.getenv('DAODATA')}/data/{baseName}Cube{desc}.npy"    
#    np.save(f"{fileName}", dataCube[:,cx-int(width/2):cx+int(width/2),cy-int(width/2):cy+int(width/2)])
#    np.save(f"{fileName}", dataCube)

#%% Modified July 10, 2025 - Saving data to FITS file 
    
    data = dataCube[:,cx-int(width/2):cx+int(width/2),cy-int(width/2):cy+int(width/2)]
    DIT = dao.shm(f"/tmp/{self.baseName}Dit.im.shm", np.zeros((1,1)).astype(np.uint32))
    FPS = dao.shm(f"/tmp/{self.baseName}Fps.im.shm", , np.zeros((1,1)).astype(np.uint32))

    time_stamp = time.strftime("%Y-%m-%d%T%H_%M_%S", time.localtime())
    
    hdu = fits.PrimaryHDU(data)
    
    header = hdu.header
    header['FPS'] = FPS
    header['DIT'] = DIT
    header['Nframes'] = nFrame
    header['ROI'] = width
    header['CX'] = cx
    header['CY'] = cy 
    
    
    hdul = fits.HDUList([hdu])
    
    filename = f"{os.getenv('DAODATA')}/data/{baseName}{time_stamp}Cube{desc}.fits"
    print(f"saving in {fileName}")
    hdul.writeto{filename}
    
    
