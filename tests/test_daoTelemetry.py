# @author: Thomas Davies
# @date: 19/07/2025
# @brief: unit tests for daoTelemetry tool.

from daoCommandIfce import daoCommandIfce
import astropy.io.fits as fits
from time import sleep
import numpy as np
import subprocess
import pytest
import shutil
import dao
import os

BOUNDS_UINT8 = (np.iinfo(np.uint8).min, np.iinfo(np.uint8).max)
BOUNDS_UINT16 = (np.iinfo(np.uint16).min, np.iinfo(np.uint16).max)
BOUNDS_UINT32 = (np.iinfo(np.uint32).min, np.iinfo(np.uint32).max)
BOUNDS_UINT64 = (np.iinfo(np.uint64).min, np.iinfo(np.uint64).max)

BOUNDS_INT8 = (np.iinfo(np.int8).min, np.iinfo(np.int8).max)
BOUNDS_INT16 = (np.iinfo(np.int16).min, np.iinfo(np.int16).max)
BOUNDS_INT32 = (np.iinfo(np.int32).min, np.iinfo(np.int32).max)
BOUNDS_INT64 = (np.iinfo(np.int64).min, np.iinfo(np.int64).max)

DELAY = 0.1
PORT = 9000
NFRAMES = 10
LOGFILE = f"{os.getcwd()}/daoTelemetry.logs"
ROOT = "/tmp/daoTelemetryTests"
SHM = "/tmp/test_daoTelemetry.im.shm"
CONFIG = f"telemetry_root: {ROOT}\ntelemetry:\n - {{target: {SHM}, limit: {NFRAMES}}}"

# ========================================================================================== #

@pytest.fixture(scope='module')
def ifce():
    print(f"logs: {LOGFILE}")
    os.mkdir(ROOT)
    try:
        daoTelemetry = subprocess.Popen(['daoTelemetry', f"{PORT}", f"-l {LOGFILE}"])
        sleep(0.5)
        ifce = daoCommandIfce("127.0.0.1", PORT)
        assert(ifce.Other(CONFIG)[0] == 0)
        yield ifce
        daoTelemetry.kill()
    finally:
        shutil.rmtree(ROOT)

# ========================================================================================== #

@pytest.mark.parametrize("specs", [
    # _DATATYPE_UINT64
    (np.uint64, (1,1), BOUNDS_UINT64),
    (np.uint64, (2,3), BOUNDS_UINT64),
    (np.uint64, (2,3,4), BOUNDS_UINT64),
    
    # _DATATYPE_UINT32
    (np.uint32, (1,1), BOUNDS_UINT32),
    (np.uint32, (2,3), BOUNDS_UINT32),
    (np.uint32, (2,3,4), BOUNDS_UINT32),
    
    # _DATATYPE_UINT16
    (np.uint16, (1,1), BOUNDS_UINT16),
    (np.uint16, (2,3), BOUNDS_UINT16),
    (np.uint16, (2,3,4), BOUNDS_UINT16),
    
    # _DATATYPE_UINT8
    (np.uint8, (1,1), BOUNDS_UINT8),
    (np.uint8, (2,3), BOUNDS_UINT8),
    (np.uint8, (2,3,4), BOUNDS_UINT8),
    
    # _DATATYPE_INT8
    (np.int8, (1,1), BOUNDS_INT8),
    (np.int8, (2,3), BOUNDS_INT8),
    (np.int8, (2,3,4), BOUNDS_INT8),
    
    # _DATATYPE_INT16
    (np.int16, (1,1), BOUNDS_INT16),
    (np.int16, (2,3), BOUNDS_INT16),
    (np.int16, (2,3,4), BOUNDS_INT16),
    
    # _DATATYPE_INT32
    (np.int32, (1,1), BOUNDS_INT32),
    (np.int32, (2,3), BOUNDS_INT32),
    (np.int32, (2,3,4), BOUNDS_INT32),
        
    # _DATATYPE_INT64
    (np.int64, (1,1), BOUNDS_INT64),
    (np.int64, (2,3), BOUNDS_INT64),
    (np.int64, (2,3,4), BOUNDS_INT64)
])
def test_integer_types(ifce, specs):
    # get test specs
    dtype,shape,bounds = specs
    
    # create shm
    shm = dao.shm(SHM, np.zeros(shape, dtype=dtype))
    
    # start telemetry session
    assert(ifce.Exec("Init")[0] == 0)
    assert(ifce.Exec("Enable")[0] == 0)
    assert(ifce.Exec("Run")[0] == 0)

    status, state = ifce.State(None)
    assert(status == 0)
    assert(state == "Running")
    
    # write frames in to shared memory
    reference_frames = []
    for i in range(NFRAMES):
        data = np.random.randint(bounds[0], bounds[-1], size=shape, dtype=dtype)
        reference_frames.append(data)
        shm.set_data(data)
        sleep(DELAY)
    
    # wait for telemetry-session to finish
    while True:
        status, state = ifce.State(None)
        assert(status == 0)
        if state == "Off":
            break
        
    # validate datafile
    session_folder_name = os.listdir(ROOT)[0]
    session_folder = f"{ROOT}/{session_folder_name}"
    print(f"session folder: {session_folder}")
    
    datafile_name = os.listdir(session_folder)[0]
    datafile = f"{session_folder}/{datafile_name}"
    print(f"datafile: {datafile}")
    
    with fits.open(datafile) as data:
        assert(len(data) == NFRAMES) # check all frames were recorded.
        
        for frame_id, hdu in enumerate(data): # for each frame, ensure metadata is present & data was recorded correctly.
            assert("atype" in hdu.header)
            assert("cnt0" in hdu.header)
            assert("cnt1" in hdu.header)
            assert("cnt2" in hdu.header)
            print(hdu.data)
            assert(np.array_equal(hdu.data, reference_frames[frame_id]))
    
    shutil.rmtree(session_folder)
    
# ========================================================================================== #

@pytest.mark.parametrize("specs", [
    # _DATATYPE_FLOAT
    (np.float32, (1,1)),
    (np.float32, (2,3)),
    (np.float32, (2,3,4)),
    
    # _DATATYPE_DOUBLE
    (np.float64, (1,1)),
    (np.float64, (2,3)),
    (np.float64, (2,3,4)),
])
def test_real_types(ifce, specs):
    # get test specs
    dtype,shape = specs
    
    # create shm
    shm = dao.shm(SHM, np.zeros(shape, dtype=dtype))
    
    # start telemetry session
    assert(ifce.Exec("Init")[0] == 0)
    assert(ifce.Exec("Enable")[0] == 0)
    assert(ifce.Exec("Run")[0] == 0)

    status, state = ifce.State(None)
    assert(status == 0)
    assert(state == "Running")
    
    # write frames in to shared memory
    reference_frames = []
    for i in range(NFRAMES):
        data = np.random.random(size=shape).astype(dtype)
        reference_frames.append(data)
        shm.set_data(data)
        sleep(DELAY)
    
    # wait for telemetry-session to finish
    while True:
        status, state = ifce.State(None)
        assert(status == 0)
        if state == "Off":
            break
        
    # validate datafile
    session_folder_name = os.listdir(ROOT)[0]
    session_folder = f"{ROOT}/{session_folder_name}"
    print(f"session folder: {session_folder}")
    
    datafile_name = os.listdir(session_folder)[0]
    datafile = f"{session_folder}/{datafile_name}"
    print(f"datafile: {datafile}")
    
    with fits.open(datafile) as data:
        assert(len(data) == NFRAMES) # check all frames were recorded.
        
        for frame_id, hdu in enumerate(data): # for each frame, ensure metadata is present & data was recorded correctly.
            assert("atype" in hdu.header)
            assert("cnt0" in hdu.header)
            assert("cnt1" in hdu.header)
            assert("cnt2" in hdu.header)
            print(hdu.data)
            assert(np.array_equal(hdu.data, reference_frames[frame_id]))
    
    shutil.rmtree(session_folder)