#!/usr/bin/env python3

import sys, getopt
import numpy as np

cupyAvail = False

try:
    import cupy as cp
    print("CuPy is available and imported.")
    cupyAvail = True
except ImportError:
    cp = None
    print("CuPy is not installed. Falling back to an alternative.")

import time
import datetime
import dao

import os
os.nice(10)

if __name__ == '__main__':
    print('daoMVM.py')
    mShmName = '/tmp/matrix.im.shm'
    vShmName = '/tmp/vector.im.shm'
    oShmName = '/tmp/output.im.shm'
    semNb=9
    gpu = False
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hm:v:o:g",["help", "mat=", "vec=", "out=", "gpu"])
    except getopt.GetoptError:
      print('err, usage: daoMvM.py -m<matrix shm> -v <vector shm> -o <output vector shm> -g')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoMvM.py -m<matrix shm> -v <vector shm> -o <output vector shm>')
            sys.exit()
        elif opt in ("-m", "--matrix"):
            mShmName = str(arg)
        elif opt in ("-v", "--vector"):
            vShmName = str(arg)
        elif opt in ("-o", "--output"):
            oShmName = str(arg)
        elif opt in ("-g", "--gpu"):
            if cupyAvail == False:
                print("Cupy not install, using numpy instead")
                gpu=False
            else:
                gpu=True

    mShm = dao.shm(mShmName)
    mCounter = mShm.get_counter()
    if gpu:
        # copy the matrix to the GPU
        m = cp.array(mShm.get_data())
    else:
        m = mShm.get_data()
    vShm = dao.shm(vShmName)
    oShm = dao.shm(oShmName)
    
    nV = int(m.shape[1])
    while 1:
        v=vShm.get_data(check=True, semNb=semNb)
        v=v[:nV]
        if gpu:
            v = cp.array(v)
            t0 = time.time()
            oShm.set_data(cp.asnumpy(cp.matmul(m, v[:nV])))
        else:
            t0 = time.time()
            oShm.set_data(np.matmul(m, v))
        t1 = time.time()
        sys.stdout.write(f"\rCM({mCounter}) MvM in {(t1 - t0) * 1000:.3f} ms")
        sys.stdout.flush()
        # update cm if needed
        if mCounter != mShm.get_counter():
            mCounter = mShm.get_counter()
            if gpu:
                m = cp.array(mShm.get_data())
            else:
                m = mShm.get_data()
            print("\nMatrix updated")
