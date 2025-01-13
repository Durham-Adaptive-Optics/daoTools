#!/usr/bin/env python3
#
import dao
import numpy as np
import astropy.io.fits as fits
import time
import matplotlib.pyplot as plt
import sys

if __name__ == '__main__':
    camShm = sys.argv[1]
    baseName = camShm.split('/')[-1].split('.')[0] 
    
    cam=dao.shm(camShm)
    bg = dao.shm(f"/tmp/{baseName}Bg.im.shm")
    bgp = dao.shm(f"/tmp/{baseName}BgPercent.im.shm", np.zeros((1,1)).astype(np.uint32))
    bk = bg.get_data()*0
    bg.set_data(bk)
    nFrame = int(sys.argv[2])
    for k in range(nFrame):
        sys.stdout.write(f"\raveraging... {k+1}/{nFrame}")
        sys.stdout.flush()
        bk = bk+cam.get_data(check=True, semNb=3)
        bgp.set_data(bgp.get_data()*0+np.uint32(100*(k+1)/nFrame))
    print('\n')
    bk=bk/nFrame
    bg.set_data(bk)
    bgp.set_data(bgp.get_data()*0+np.uint32(100))