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

# ========================================================================================== #

PORT = 19000
ROOT = "/tmp/daoTelemetryTests"
SHM = "/tmp/test_daoTelemetry.im.shm"

# ========================================================================================== #

@pytest.fixture
def tool():
    os.mkdir(ROOT)
    try:
        daoTelemetry = subprocess.Popen(['daoTelemetry', f"{PORT}"])
        sleep(0.5)
        ifce = daoCommandIfce("127.0.0.1", PORT, timeout=5)
        yield ifce
    finally:
        daoTelemetry.kill()
        shutil.rmtree(ROOT)

# ========================================================================================== #

def test_Manual_Session(tool: daoCommandIfce):
    # configure tool
    assert(tool.Other(
        f"telemetry_root: {ROOT}\ntelemetry:\n - {{target: {SHM}}}"    
    )[0] == 0)

    # create shm
    _ = dao.shm(SHM, np.zeros((1,1)))
    
    # start session
    assert(tool.Exec("Init")[0] == 0)
    assert(tool.Exec("Enable")[0] == 0)
    assert(tool.Exec("Run")[0] == 0)

    status, state = tool.State(None)
    assert((status == 0) and (state == "Running"))

    # end session
    assert(tool.Exec("Idle")[0] == 0)
    assert(tool.Exec("Disable")[0] == 0)
    assert(tool.Exec("Stop")[0] == 0)

    status, state = tool.State(None)
    assert((status == 0) and (state == "Off"))

# ========================================================================================== #

def test_Consecutive_Sessions(tool: daoCommandIfce):
    NSESSIONS = 3
    
    # configure tool
    assert(tool.Other(
        f"telemetry_root: {ROOT}\ntelemetry:\n - {{target: {SHM}}}"    
    )[0] == 0)

    # create shm
    _ = dao.shm(SHM, np.zeros((1,1)))
    
    # prep session
    assert(tool.Exec("Init")[0] == 0)
    assert(tool.Exec("Enable")[0] == 0)
    
    for _ in range(NSESSIONS):
        # start session
        assert(tool.Exec("Run")[0] == 0)

        status, state = tool.State(None)
        assert((status == 0) and (state == "Running"))

        # end session
        assert(tool.Exec("Idle")[0] == 0)

        status, state = tool.State(None)
        assert((status == 0) and (state == "Idle"))
        
        # delay so the session dir timestamps are different
        sleep(3)
        
    assert(tool.Exec("Disable")[0] == 0)
    assert(tool.Exec("Stop")[0] == 0)

    status, state = tool.State(None)
    assert((status == 0) and (state == "Off"))

# ========================================================================================== #

def test_Automated_Session(tool: daoCommandIfce):
    # configure tool
    NFRAMES = 10
    DELAY = 0.1
    
    assert(tool.Other(
        f"telemetry_root: {ROOT}\ntelemetry:\n - {{target: {SHM}, limit: {NFRAMES}}}"    
    )[0] == 0)

    # create shm
    shm = dao.shm(SHM, np.zeros((1,1)))
    
    # start session
    assert(tool.Exec("Init")[0] == 0)
    assert(tool.Exec("Enable")[0] == 0)
    assert(tool.Exec("Run")[0] == 0)

    status, state = tool.State(None)
    assert((status == 0) and (state == "Running"))

    # write frames to shm
    for _ in range(NFRAMES-1):
        data = np.random.rand(1,1)
        shm.set_data(data)
        sleep(DELAY)
    
    # await session end
    sleep(1)
    status, state = tool.State(None)
    assert((status == 0) and (state == "Off"))

# ========================================================================================== #

def test_Configuration_Error(tool: daoCommandIfce):
    # configure tool
    assert(tool.Other("")[0] == 0)

    # create shm
    _ = dao.shm(SHM, np.zeros((1,1)))
    
    # start session
    assert(tool.Exec("Init")[0] == 0)
    
    # check we went to error state
    status, state = tool.State(None)
    assert((status == 0) and (state == "Error"))
    
    # update to a valid config
    assert(tool.Other(
        f"telemetry_root: {ROOT}\ntelemetry:\n - {{target: {SHM}}}"    
    )[0] == 0)

    # recover from error state
    assert(tool.Exec("Recover")[0] == 0)

    # check we recovered to Idle
    sleep(1)
    status, state = tool.State(None)
    assert((status == 0) and (state == "Idle"))
    
# ========================================================================================== #

def test_Allocation_Error(tool: daoCommandIfce):
    # configure tool
    assert(tool.Other(
        f"telemetry_root: {ROOT}\ntelemetry:\n - {{target: /tmp/__foobar__.im.shm}}"    
    )[0] == 0)

    # create shm
    _ = dao.shm(SHM, np.zeros((1,1)))
    
    # start session
    assert(tool.Exec("Init")[0] == 0)
    assert(tool.Exec("Enable")[0] == 0)
    
    # check we went to error state
    status, state = tool.State(None)
    assert((status == 0) and (state == "Error"))
    
    # update to a valid config
    assert(tool.Other(
        f"telemetry_root: {ROOT}\ntelemetry:\n - {{target: {SHM}}}"    
    )[0] == 0)

    # recover from error state
    assert(tool.Exec("Recover")[0] == 0)

    # check we recovered to Idle
    sleep(1)
    status, state = tool.State(None)
    assert((status == 0) and (state == "Idle"))

# # ========================================================================================== #

# def test_Recording_Error(tool: daoCommandIfce):
#     ENV = f"{ROOT}/tmp"
#     os.mkdir(ENV)

#     # configure tool
#     assert(tool.Other(
#         f"telemetry_root: {ENV}\ntelemetry:\n - {{target: {SHM}}}"    
#     )[0] == 0)

#     # create shm
#     data = np.zeros((1,1))
#     shm = dao.shm(SHM, data)
    
#     # start session
#     assert(tool.Exec("Init")[0] == 0)
#     assert(tool.Exec("Enable")[0] == 0)
#     assert(tool.Exec("Run")[0] == 0)
    
#     # delete session directory and push new frame 
#     # to trigger a recording error.
#     shutil.rmtree(ENV)
#     shm.set_data(data)
    
#     # check we went to error state
#     sleep(0.5)
#     status, state = tool.State(None)
#     assert((status == 0) and (state == "Error"))

#     # recover from error state
#     assert(tool.Exec("Recover")[0] == 0)

#     # check we recovered to Idle
#     sleep(0.5)
#     status, state = tool.State(None)
#     assert((status == 0) and (state == "Idle"))

# # ========================================================================================== #

# @pytest.mark.parametrize("dtype", numpy_dtypes = [
#     np.int8, np.int16, np.int32, np.int64,
#     np.uint8, np.uint16, np.uint32, np.uint64,
#     np.float32, np.float64,
# ])
# @pytest.mark.parametrize("shape", [(2,3), (2,3,4)])
# @pytest.mark.parametrize("unity", [True, False])
# def test_Data_Integrity(tool: daoCommandIfce, unity, dtype, shape):
#     ENV = f"{ROOT}/test_Data_Integrity"
#     os.mkdir(ENV)
    
#     #
#     NFRAMES = 100
#     DELAY = 0.1
#     CAP = 0 if unity else (NFRAMES // 3)
    
#     # create shm
#     shm = dao.shm(SHM, np.zeros(shape, dtype=dtype))

#     # configure session
#     assert(tool.Other(
#         f"telemetry_root: {ROOT}\ntelemetry:\n - {{target: {SHM}, capacity: {CAP}, limit: {NFRAMES}}}"    
#     )[0] == 0)

#     # start telemetry session
#     assert(tool.Exec("Init")[0] == 0)
#     assert(tool.Exec("Enable")[0] == 0)
#     assert(tool.Exec("Run")[0] == 0)

#     status, state = tool.State(None)
#     assert((status == 0) and (state == "Running"))
    
#     # write frames in to shared memory
#     gold_data = []
#     min, max = np.iinfo(dtype).min, np.iinfo(dtype).max
#     gold_data.append(shm.get_data())
#     for _ in range(NFRAMES-1):
#         data = np.random.randint(min, max, size=shape, dtype=dtype)
#         gold_data.append(data)
#         shm.set_data(data)
#         sleep(DELAY)
    
#     # end session
#     sleep(1)
#     status, state = tool.State(None)
#     assert((status == 0) and (state == "Off"))
        
#     # gather recorded data into a list for checking
#     disk_data = []
#     sessiondir = os.listdir(ENV)[0]
#     for datafile in os.listdir(sessiondir):
#         with fits.open(datafile) as data:
#             disk_data.append(data)
    
#     # check recorded data is accurate
#     assert(len(gold_data) == len(disk_data))
#     for frame_id in range(len(gold_data)):
#         disk_frame = disk_data[frame_id]
#         gold_frame = gold_data[frame_id]
#         assert(np.array_equal(disk_data.data, gold_frame))
#         assert("atype" in disk_frame.header)
#         assert("atime" in disk_frame.header)
#         assert("cnt0" in disk_frame.header)
#         assert("cnt1" in disk_frame.header)
#         assert("cnt2" in disk_frame.header)
   
#     # cleanup
#     shutil.rmtree(ENV)

# # ========================================================================================== #
