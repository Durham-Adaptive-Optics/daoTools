'''
 # @ Author: Thomas N. Davies
 # @ Company: Centre for Advanced Instrumentation, Durham University
 # @ Contact: thomas.n.davies@durham.ac.uk
 # @ Create Time: 2025-09-01 13:06:33
 # @ Description: Test suite for daoTelemetry.
 '''

# ========================================================================================== #
# IMPORTS
# ========================================================================================== #

from daoCommandIfce import daoCommandIfce
import astropy.io.fits as fits
from time import perf_counter, sleep
import numpy as np
import subprocess
import inspect
import pytest
import shutil
import dao
import os
import yaml
import pathlib

# ========================================================================================== #
# DEFINES
# ========================================================================================== #

TEST_DIRECTORY = "/tmp/daoTelemetry_"
TEST_CONFIGFILE = "daoTelemetry/config.yaml"

# ========================================================================================== #
# UTIL ROUTINES
# ========================================================================================== #


def QueryState(i: daoCommandIfce) -> str:
    """
        Queries daoTelemetry tool for its current state.
    """
    sleep(0.5)
    status, state = i.State(None)
    assert((status == 0))
    return state

def AssertState(i: daoCommandIfce, target_state: str):
    """
        Queries daoTelemetry tool for its current state
        and asserts it is the desired state.
    """
    state = QueryState(i)
    assert(state == target_state)

def StateTransition(i: daoCommandIfce, transition: str, target_state: str = None):
    """
        Sends command to daoTelemetry tool to carry
        out the desired state transition.
    """
    assert(i.Exec(transition)[0] == 0)
    
    if target_state:
        AssertState(i, target_state)
    
def SetConfig(i: daoCommandIfce, config: str):
    """
        Sends command to daoTelemetry tool to set the
        active configuration string.
    """
    assert(i.Other(config)[0] == 0)
     
def InitTestRoutine():
    """
        Creates subdirectory for test routine inside root
        test directory.
    """
    caller_name = inspect.stack()[1].function
    subdir_name = f"{TEST_DIRECTORY}/{caller_name}"
    os.mkdir(subdir_name)
    return subdir_name

@pytest.fixture
def i():
    """ 
        Creates dedicated testing directory and launches daoTelemetry process,
        providing a command interface, and kills the process and removes
        testing directory after each test has ended. 
    """
    port = 15000
    os.mkdir(TEST_DIRECTORY)
    try:
        daoTelemetry = subprocess.Popen(['daoTelemetry', f"{port}"])
        sleep(0.5)
        ifce = daoCommandIfce("127.0.0.1", port)
        yield ifce
    finally:
        daoTelemetry.kill()
        shutil.rmtree(TEST_DIRECTORY)
        
@pytest.fixture
def i2():
    """ 
        Same as i() but passed a valid configuration file on the cli
        when the daoTelemetry process is launched. 
    """
    port = 15000
    os.mkdir(TEST_DIRECTORY)
    try:
        daoTelemetry = subprocess.Popen(['daoTelemetry', f"{port}", f"-c{TEST_CONFIGFILE}"])
        sleep(0.5)
        ifce = daoCommandIfce("127.0.0.1", port)
        yield ifce
    finally:
        daoTelemetry.kill()
        shutil.rmtree(TEST_DIRECTORY)

# ========================================================================================== #
# TEST ROUTINES
# ========================================================================================== #

def test_CliConfig(i2: daoCommandIfce):
    InitTestRoutine()
    """ 
       Checks config can be set via CLI flag.
    """
    StateTransition(i2, "Init", "Standby")
    
def test_NetworkConfig(i: daoCommandIfce):
    InitTestRoutine()
    """ 
       Checks config can be set via network.
    """
    with open(TEST_CONFIGFILE) as config_file:
        config = config_file.read()
    
    SetConfig(i, config)
    StateTransition(i, "Init", "Standby")
    
def test_NoConfig(i: daoCommandIfce):
    InitTestRoutine()
    """ 
        Checks no config triggers an error state.
    """
    StateTransition(i, "Init", "Error")

def test_MalformedConfig(i: daoCommandIfce):
    InitTestRoutine()
    """ 
        Checks an invalid config triggers an error state.
    """
    with open(TEST_CONFIGFILE) as config_file:
        cfg_yml = yaml.safe_load(config_file)

    # remove required field to invalidate config.
    cfg_yml.pop("telemetry")
    
    # generate config string
    config = yaml.dump(cfg_yml)
    
    SetConfig(i, config)
    StateTransition(i, "Init", "Error")

def test_InvalidSharedMemory(i: daoCommandIfce):
    InitTestRoutine()
    """ 
       Checks an error is raised if a shared memory
       target fails to be opened.
    """
    with open(TEST_CONFIGFILE) as config_file:
        config = config_file.read()

    SetConfig(i, config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Error")

@pytest.mark.parametrize("T", [np.complex64, np.complex128])
def test_UnsupportedType(i: daoCommandIfce, T):
    root = InitTestRoutine()
    """ 
       Checks an error is raised if a shared memory target
       uses an unsupported data type.
    """
    # create shm
    shmpath = f"/tmp/shm.im.shm"
    _ = dao.shm(shmpath, np.zeros((1,1), dtype=T))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"] = [{
        "target": shmpath
    }]
    config = yaml.dump(yml)

    SetConfig(i, config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Error")

#! @test_ThreadAffinity only works on Linux currently.
def test_ThreadAffinity(i: daoCommandIfce):
    root = InitTestRoutine()
    """ 
       Checks that a target's collector thread
       has the configured core affinity.
    """
    cores = np.arange(os.cpu_count())
    core = int(cores[cores.size // 2])
    
    # create shm
    shmpath = f"/tmp/shm.im.shm"
    _ = dao.shm(shmpath, np.zeros((1,1)))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"] = [{
        "target": shmpath,
        "core": core
    }]
    config = yaml.dump(yml)

    SetConfig(i, config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Idle")
    
    # check affinity of collector thread
    PID = None
    for pid in os.listdir("/proc"):
        if pid.isdigit():
            try:
                exePath = os.readlink(f"/proc/{pid}/exe")
            except:
                continue
            
            if exePath.endswith("daoTelemetry"):
                PID = pid
                break
    assert PID, "Could not find daoTelemetry PID"    
    
    TID = None
    for tid in os.listdir(f"/proc/{PID}/task"):
        with open(f"/proc/{PID}/task/{tid}/comm") as comm:
            threadName = comm.read().strip()
            if threadName == shmpath:
                TID = tid
                break
    assert TID, "Could not find collector thread TID"

    with open(f"/proc/{PID}/task/{TID}/status") as status:
        threadMetadata = status.read().split("\n")
        for entry in threadMetadata:
            kv = entry.split(":")
            if kv[0] == "Cpus_allowed":
                cpuMask = int(kv[1].strip(), 16)
                assert cpuMask & (1 << core) != 0, "Thread not running on specified core!"
                assert cpuMask & ~(1 << core) == 0, "Thread is running on unspecified cores!"

def test_FileCopy(i: daoCommandIfce):
    root = InitTestRoutine()
    """ 
       Checks configured files are copied to the
       session directory upon session start.
    """
    COPY_FILE = "test_daoTelemetry.py" # file to copy
    
    # create shm
    shmpath = f"/tmp/shm.im.shm"
    _ = dao.shm(shmpath, np.zeros((1,1)))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["files"] = [
        os.path.abspath(f"daoTelemetry/{COPY_FILE}")
    ]
    yml["telemetry"] = [{
        "target": shmpath       
    }]
    config = yaml.dump(yml)

    SetConfig(i, config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Idle")
    StateTransition(i, "Run", "Running")

    fileCopied = False
    sessionDir = os.listdir(root)[0]
    for file in os.listdir(f"{root}/{sessionDir}"):
        print(file)
        if file == COPY_FILE:
            fileCopied = True
            break
        
    assert(fileCopied)

def test_ErrorRecovery(i: daoCommandIfce):
    root = InitTestRoutine()
    """ 
       Checks that the error state can be recovered from.
    """
    # create shm
    shmpath = f"/tmp/shm.im.shm"
    _ = dao.shm(shmpath, np.zeros((1,1)))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"] = [{
        "target": shmpath       
    }]
    config = yaml.dump(yml)

    SetConfig(i, config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Idle")
    StateTransition(i, "Run", "Running")
    StateTransition(i, "OnFailue", "Error")
    StateTransition(i, "Recover", "Idle")

def test_RecordingLimit(i: daoCommandIfce):
    root = InitTestRoutine()
    """ 
       Checks that the recording session automatically
       stops once the configured limit is reached.
    """
    NUM_FRAMES = 10 # number of frames to record to disk.
    
    # create shm
    shmpath = f"/tmp/shm.im.shm"
    shm = dao.shm(shmpath, np.zeros((1,1)))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"] = [{
        "target": shmpath,
        "limit": NUM_FRAMES  
    }]
    config = yaml.dump(yml)

    SetConfig(i, config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Idle")
    StateTransition(i, "Run", "Running")
    
    # write frames to shm
    for _ in range(NUM_FRAMES - 1):
        data = np.random.rand(1,1)
        shm.set_data(data)
        sleep(0.5)

    AssertState(i, "Off")

def test_DatafileCapacity(i: daoCommandIfce):
    root = InitTestRoutine()
    """ 
       Checks that frames are recorded into multiple
       data files to ensure that each file's size does not
       exceed its configured capacity.
    """
    FILE_CAPACITY = 10 # number of frames per datafile.
    
    # create shm
    shmName = "shm"
    shmPath = f"/tmp/{shmName}.im.shm"
    shm = dao.shm(shmPath, np.zeros((1,1)))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"] = [{
        "target": shmPath,
        "capacity": FILE_CAPACITY,
        "limit": FILE_CAPACITY + 1
    }]
    config = yaml.dump(yml)

    SetConfig(i, config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Idle")
    StateTransition(i, "Run")
    
    # write (FILE_CAPACITY - 1) frames for 1st datafile.
    # and another frame for the 2nd datafile.
    for _ in range(FILE_CAPACITY):
        data = np.random.rand(1,1)
        shm.set_data(data)
        sleep(1)
        
    # check two datafiles were created
    sessionDir = os.listdir(root)[0]
    for file in os.listdir(f"{root}/{sessionDir}"):
        fileID = int(file[file.index("_")+1 : file.index(".")])
        fileName = file.split("_")[0]

        assert(fileName == shmName)
        assert(fileID == 1 or fileID == 2)

        with fits.open(f"{root}/{sessionDir}/{file}") as data:
            expectedFrames = FILE_CAPACITY if fileID == 1 else 1
            assert(len(data) == expectedFrames)

def test_RecordingError(i: daoCommandIfce):
    root = InitTestRoutine()
    """ 
       Checks an error is raised when an issue occurs
       within the main recording loop of a collector thread.
    """
    FILE_CAPACITY = 10 # number of frames per datafile.
    
    # create shm
    shmName = "shm"
    shmPath = f"/tmp/{shmName}.im.shm"
    shm = dao.shm(shmPath, np.zeros((1,1)))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"] = [{
        "target": shmPath,
        "capacity": FILE_CAPACITY
    }]
    config = yaml.dump(yml)

    SetConfig(i, config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Idle")
    StateTransition(i, "Run", "Running")
    
    # create file w/ name of upcoming datafile-2
    sessionDir = os.listdir(root)[0]
    file2 = pathlib.Path(f"{root}/{sessionDir}/{shmName}_2.fits")
    file2.touch(exist_ok=False)
    
    # write frames
    for _ in range(FILE_CAPACITY):
        data = np.random.rand(1,1)
        shm.set_data(data)
        sleep(0.5)

    AssertState(i, "Error")

@pytest.mark.parametrize("dtype", [
    np.int8, np.int16, np.int32, np.int64,
    np.uint8, np.uint16, np.uint32, np.uint64,
    np.float32, np.float64
])
@pytest.mark.parametrize("shape", [(2,3), (2,3,4)])
def test_RecordingAccuracy(i: daoCommandIfce, dtype, shape):
    root = InitTestRoutine()
    """ 
       Checks that data across all supported types and array dimensions
       is correctly recorded.
    """
    
    #
    NUM_FRAMES = 10
    type = np.dtype(dtype)
    dtype_limits = np.iinfo(type) if type.kind in "iu" else np.finfo(type)
    min, max = dtype_limits.min, dtype_limits.max
    
    # create shm
    shmPath = f"/tmp/shm.im.shm"
    shm = dao.shm(shmPath, np.zeros(shape, dtype=dtype))

    # track golden frames
    gold_data = []
    gold_data.append(shm.get_data())

    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"] = [{
        "target": shmPath,
        "limit": NUM_FRAMES
    }]
    config = yaml.dump(yml)

    SetConfig(i, config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Idle")
    StateTransition(i, "Run", "Running")

    # write golden frames
    for _ in range(NUM_FRAMES - 1):
        raw_data = np.random.random(size=shape) * (max - min) + min # array of random floats between min & max
        data = raw_data.astype(dtype) # cast vals into desired type.
        gold_data.append(data)
        shm.set_data(data)
        sleep(0.5)
    
    AssertState(i, "Off")
        
    # check recorded data is accurate
    sessionDir = os.listdir(root)[0]
    datafile = os.listdir(f"{root}/{sessionDir}")[0]
    with fits.open(f"{root}/{sessionDir}/{datafile}") as data:
        assert(len(gold_data) == len(data))
    
        for frame_id in range(len(gold_data)):
            # fetch recorded frame and its reference.
            hdu = data[frame_id]
            gold_frame = gold_data[frame_id]

            # check all metadata is present
            assert("atype" in hdu.header)
            assert("atime" in hdu.header)
            assert("cnt0" in hdu.header)
            assert("cnt1" in hdu.header)
            assert("cnt2" in hdu.header)

            # check data accuracy
            assert(np.array_equal(hdu.data, gold_frame))
            assert(hdu.header["atype"] == shm.get_meta_data()["atype"])
            assert(hdu.header["cnt0"] == (frame_id + 1))

# def test_StateMachine(i: daoCommandIfce):
#     root = InitTestRoutine()
#     """ 
#        Stress tests the state transitions by 
#        performing a random walk through the state space.
#     """
#     # create shm
#     shmPath = f"/tmp/shm.im.shm"
#     _ = dao.shm(shmPath, np.zeros((1,1)))

#     # create config
#     yml = {}
#     yml["telemetry_root"] = root
#     yml["telemetry"] = [{
#         "target": shmPath
#     }]
#     config = yaml.dump(yml)
#     SetConfig(i, config)
    
#     # cycle states
#     tThreshold = 30 # number seconds to run test for
#     pThreshold = 0.5 # probability of taking forward transition.

#     t0 = perf_counter()
#     while (perf_counter() - t0) < tThreshold:
#         state = QueryState(i)

#         if state == "Off":
#             StateTransition(i, "Init", "Standby")
#         elif state == "Running":
#             StateTransition(i, "Idle", "Idle")
#         else:
#             transition_lookup = {
#                 "Standby": [
#                     ("Enable", "Idle"), # forward transition
#                     ("Stop", "Off") # back transition
#                 ],
                
#                 "Idle": [
#                     ("Run", "Running"), # forward transition
#                     ("Idle", "Idle") # back transition
#                 ]   
#             }
            
#             assert(state in transition_lookup)
#             k = 0 if np.random.random() <= pThreshold else 1
#             transition, goto_state = transition_lookup[state][k]
#             StateTransition(i, transition, goto_state)
#             sleep(0.5)
        
