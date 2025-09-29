'''
 # @ Author: Thomas N. Davies
 # @ Company: Centre for Advanced Instrumentation, Durham University
 # @ Contact: thomas.n.davies@durham.ac.uk
 # @ Create Time: 2025-09-29 14:07:07
 # @ Description: Unit tests for daoDownsample app.
 '''

# ========================================================================================== #
# IMPORTS
# ========================================================================================== #

import time
import numpy as np
import subprocess
import pytest
import dao

# ========================================================================================== #
# DEFINES
# ========================================================================================== #

SRCIMG_SHM = "/tmp/srcImage.im.shm"
OUTIMG_SHM = "/tmp/outImage.im.shm"

# ========================================================================================== #
# TEST ROUTINES
# ========================================================================================== #

def test_nonUint16Pixels():
    srcShm = dao.shm(SRCIMG_SHM, np.zeros((1,1), dtype=np.float32))
    outShm = dao.shm(OUTIMG_SHM, np.zeros((1,1), dtype=np.float32))
    daoDownsample = subprocess.run(["daoDownsample", SRCIMG_SHM, OUTIMG_SHM])
    if daoDownsample.returncode != 1:
        raise RuntimeError

def test_upsample():
    srcShm = dao.shm(SRCIMG_SHM, np.zeros((100,50), dtype=np.uint16))
    outShm = dao.shm(OUTIMG_SHM, np.zeros((200,150), dtype=np.uint16))
    daoDownsample = subprocess.run(["daoDownsample", SRCIMG_SHM, OUTIMG_SHM])
    if daoDownsample.returncode != 1:
        raise RuntimeError

def test_nonIntegralGrid():
    srcShm = dao.shm(SRCIMG_SHM, np.zeros((200,300), dtype=np.uint16))
    outShm = dao.shm(OUTIMG_SHM, np.zeros((150,70), dtype=np.uint16))
    daoDownsample = subprocess.run(["daoDownsample", SRCIMG_SHM, OUTIMG_SHM])
    if daoDownsample.returncode != 1:
        raise RuntimeError

@pytest.mark.parametrize("grid", [(5,5), (10,5), (5,10)])
@pytest.mark.parametrize("sumMode", [True, False])
def test_Downsample(grid, sumMode, srcResolution = (1000,1000)):
    frame = np.random.randint(low=0,high=np.iinfo(np.uint16).max, size=srcResolution, dtype=np.uint16)
    outShm = dao.shm(OUTIMG_SHM, np.zeros((srcResolution[0] // grid[0], srcResolution[1] // grid[1]), dtype=np.uint16))
    srcShm = dao.shm(SRCIMG_SHM, np.zeros(srcResolution, dtype=np.uint16))

    summationFlag = ["-s"] if sumMode else []
    daoDownsample = subprocess.Popen(["daoDownsample", SRCIMG_SHM, OUTIMG_SHM] + summationFlag)
    time.sleep(1)
    cnt0 = outShm.get_counter()
    srcShm.set_data(frame)
    while outShm.get_counter() == cnt0: pass
    daoDownsample.kill()

    reshapedSrcImage = frame.reshape(frame.shape[0] // grid[0], grid[0], frame.shape[1] // grid[1], grid[1]) # todo is this correct?
    outputReference = reshapedSrcImage.sum(axis=(1,3), dtype=np.uint16) if sumMode else reshapedSrcImage.mean(axis=(1,3), dtype=np.uint16) # todo is this correct?
    
    print(f"Frame: {frame}")
    print(f"Output: {outShm.get_data()}")
    print(f"Ref: {outputReference}")
    
    assert np.array_equal(outShm.get_data(), outputReference)