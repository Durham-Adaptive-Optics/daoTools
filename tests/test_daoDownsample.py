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
# UTILITY FUNCTIONS
# ========================================================================================== #

def roundHalfUp(arr: np.ndarray) -> np.ndarray:
    return np.floor(arr + 0.5)

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

@pytest.mark.parametrize("grid", [(2,2), (2,3), (3,2)])
@pytest.mark.parametrize("sumMode", [True, False])
def test_Downsample(grid, sumMode, src = (6,6)):
    # create source frame & input/output shms.
    srcHeight, srcWidth = src
    gridHeight, gridWidth = grid
    outHeight, outWidth = srcHeight // gridHeight, srcWidth // gridWidth
    
    frame = np.random.randint(low=0,high=np.iinfo(np.uint8).max, size=(srcHeight, srcWidth), dtype=np.uint16)
    outShm = dao.shm(OUTIMG_SHM, np.zeros((outHeight, outWidth), dtype=np.uint16))
    srcShm = dao.shm(SRCIMG_SHM, np.zeros((srcHeight, srcWidth), dtype=np.uint16))

    # launch daoDownsample.
    summationFlag = ["-s"] if sumMode else []
    daoDownsample = subprocess.Popen(["daoDownsample", SRCIMG_SHM, OUTIMG_SHM] + summationFlag)
    time.sleep(1)
    
    # write frame into source shm and await downsampled frame.
    cnt0 = outShm.get_counter()
    srcShm.set_data(frame)
    while outShm.get_counter() == cnt0: pass
    daoDownsample.kill()

    # verify downsampled frame against reference algorithm.
    reference = np.reshape(
        frame, (outHeight, gridHeight, outWidth, gridWidth)
    ).sum(axis=(1,3), dtype=np.uint16)

    if not sumMode:
        nGrid = gridWidth * gridHeight
        scaledArray = reference / nGrid
        reference = roundHalfUp(scaledArray).astype(np.uint16)
    
    if not np.array_equal(outShm.get_data(), reference):
        print(f"Frame: {frame}")
        print(f"Reference: {reference}")
        print(f"Output: {outShm.get_data()}")
        assert False