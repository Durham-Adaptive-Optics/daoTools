# @author: Thomas Davies
# @date: 15/08/2025
# @brief: daoTelemetry testing

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
    state = QueryState()
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
        daoTelemetry = subprocess.Popen(['daoTelemetry', f"{port} -c {TEST_CONFIGFILE}"])
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
       Checks config set via CLI was applied.
    """
    StateTransition(i2, "Init", "Standby")
    
def test_NetworkConfig(i: daoCommandIfce):
    InitTestRoutine()
    """ 
       Checks config set via network was applied.
    """
    with open(TEST_CONFIGFILE) as config_file:
        config = config_file.read()
    
    SetConfig(config)
    StateTransition(i, "Init", "Standby")
    
def test_NoConfig(i: daoCommandIfce):
    InitTestRoutine()
    """ 
        Inits telemetry tool without setting a config,
        and checks the Error state is raised.
    """
    StateTransition(i, "Init", "Error")

def test_MalformedConfig(i: daoCommandIfce):
    InitTestRoutine()
    """ 
        Inits telemetry tool without setting a config,
        and checks the Error state is raised.
    """
    with open(TEST_CONFIGFILE) as config_file:
        cfg_yml = yaml.load(config_file)

    # remove required field to invalidate config.
    cfg_yml.pop("telemetry")
    
    # generate config string
    config = yaml.dump(cfg_yml)
    
    SetConfig(config)
    StateTransition(i, "Init", "Error")

def test_InvalidSharedMemory(i: daoCommandIfce):
    InitTestRoutine()
    """ 
       Checks config set via network was applied.
    """
    with open(TEST_CONFIGFILE) as config_file:
        config = config_file.read()

    SetConfig(config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Error")

@pytest.mark.parametrize("T", [np.complex64, np.complex128])
def test_UnsupportedType(i: daoCommandIfce, T):
    root = InitTestRoutine()
    """ 
       Checks config set via network was applied.
    """
    # create shm
    shmpath = f"{root}/shm.im.shm"
    _ = dao.shm(shmpath, np.zeros((1,1), dtype=T))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"].append({
        "target": shmpath
    })
    config = yaml.dump(yml)

    SetConfig(config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Error")

def test_ThreadAffinity(i: daoCommandIfce):
    root = InitTestRoutine()
    """ 
       Checks config set via network was applied.
    """
    cores = np.arange(os.cpu_count())
    core = cores[cores.size // 2]
    
    # create shm
    shmpath = f"{root}/shm.im.shm"
    _ = dao.shm(shmpath, np.zeros((1,1)))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"].append({
        "target": shmpath,
        "core": core
    })
    config = yaml.dump(yml)

    SetConfig(config)
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
    assert(PID, "Could not find daoTelemetry PID")    
    
    TID = None
    for tid in os.listdir(f"/proc/{PID}/task"):
        with open(f"/proc/{PID}/task/{tid}/comm") as comm:
            threadName = comm.read().strip()
            if threadName == shmpath:
                TID = tid
                break
    assert(TID, "Could not find collector thread TID")    

    with open(f"/proc/{PID}/task/{TID}/status") as status:
        threadMetadata = status.read().split("\n")
        for entry in threadMetadata:
            kv = entry.split(":")
            if kv[0] == "Cpus_allowed":
                cpuMask = kv[1].strip()
                assert(cpuMask & (1 << core) != 0) # desired core is allowed
                assert(cpuMask & ~(1 << core) == 0) # no other cores are allowed

def test_FileCopy(i: daoCommandIfce):
    root = InitTestRoutine()
    """ 
       Checks config set via network was applied.
    """
    fileCpy = "test_daoTelemetry.py" # file to copy
    
    # create shm
    shmpath = f"{root}/shm.im.shm"
    _ = dao.shm(shmpath, np.zeros((1,1)))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["files"].append([
        os.path.abspath(f"daoTelemetry/{fileCpy}")
    ])
    yml["telemetry"].append({
        "target": shmpath       
    })
    config = yaml.dump(yml)

    SetConfig(config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Idle")
    StateTransition(i, "Run", "Running")
    
    fileCopied = False
    sessionDir = os.listdir(root)[0]
    for file in os.listdir(sessionDir):
        if file == fileCpy:
            fileCopied = True
            break
    assert(fileCopied)

def test_ErrorRecovery(i: daoCommandIfce):
    root = InitTestRoutine()
    """ 
       Checks config set via network was applied.
    """
    # create shm
    shmpath = f"{root}/shm.im.shm"
    _ = dao.shm(shmpath, np.zeros((1,1)))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"].append({
        "target": shmpath       
    })
    config = yaml.dump(yml)

    SetConfig(config)
    StateTransition(i, "OnFailue", "Error")
    StateTransition(i, "Recover", "Idle")

def test_RecordingLimit(i: daoCommandIfce):
    root = InitTestRoutine()
    """ 
       Checks config set via network was applied.
    """
    nFrames = 10 # number of frames to record to disk.
    
    # create shm
    shmpath = f"{root}/shm.im.shm"
    shm = dao.shm(shmpath, np.zeros((1,1)))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"].append({
        "target": shmpath,
        "limit": nFrames  
    })
    config = yaml.dump(yml)

    SetConfig(config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Idle")
    StateTransition(i, "Run", "Running")
    
    # write frames to shm
    for _ in range(nFrames - 1):
        data = np.random.rand(1,1)
        shm.set_data(data)
        sleep(0.5)

    AssertState(i, "Off")

def test_DatafileCapacity(i: daoCommandIfce):
    root = InitTestRoutine()
    """ 
       Checks config set via network was applied.
    """
    fileCap = 10 # number of frames per datafile.
    file2Size = fileCap // 3 # number of frames to write into 2nd datafile
    
    # create shm
    shmName = "shm"
    shmPath = f"{root}/{shmName}.im.shm"
    shm = dao.shm(shmPath, np.zeros((1,1)))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"].append({
        "target": shmPath,
        "capacity": fileCap
    })
    config = yaml.dump(yml)

    SetConfig(config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Idle")
    StateTransition(i, "Run", "Running")
    
    # write frames for 1st datafile.
    for _ in range(fileCap - 1):
        data = np.random.rand(1,1)
        shm.set_data(data)
        sleep(0.5)
        
    # write frame for 2nd datafile
    for _ in range(file2Size):
        data = np.random.rand(1,1)
        shm.set_data(data)
        sleep(0.5)

    # check two datafiles were created
    sessionDir = os.listdir(root)[0]
    for file in os.listdir(sessionDir):
        fileName = file.split("_")[0]
        fileID = int(file.split("_")[1])
        
        assert(fileName == shmName)
        assert(fileID == 1 or fileID == 2)
        with fits.open(f"{root}/{sessionDir}/{file}") as data:
            expectedFrames = fileCap if fileID == 1 else file2Size
            assert(len(data) == expectedFrames)

def test_RecordingError(i: daoCommandIfce):
    root = InitTestRoutine()
    """ 
       Checks config set via network was applied.
    """
    fileCap = 10 # number of frames per datafile.
    
    # create shm
    shmName = "shm"
    shmPath = f"{root}/{shmName}.im.shm"
    shm = dao.shm(shmPath, np.zeros((1,1)))
    
    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"].append({
        "target": shmPath,
        "capacity": fileCap
    })
    config = yaml.dump(yml)

    SetConfig(config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Idle")
    StateTransition(i, "Run", "Running")
    
    # create file w/ name of upcoming datafile-2
    sessionDir = os.listdir(root)[0]
    file2 = pathlib.Path(f"{root}/{sessionDir}/{shmName}_2.fits")
    file2.touch(exist_ok=False)
    
    # write frames
    for _ in range(fileCap):
        data = np.random.rand(1,1)
        shm.set_data(data)
        sleep(0.5)

    AssertState(i, "Error")

@pytest.mark.parametrize("dtype", [
    np.int8, np.int16, np.int32, np.int64,
    np.uint8, np.uint16, np.uint32, np.uint64,
    np.float32, np.float64,
])
@pytest.mark.parametrize("shape", [(2,3), (2,3,4)])
def test_RecordingAccuracy(i: daoCommandIfce, dtype, shape):
    root = InitTestRoutine()
    """ 
       Checks config set via network was applied.
    """
    
    #
    nFrames = 10
    min, max = np.iinfo(dtype).min, np.iinfo(dtype).max
    
    # create shm
    shmPath = f"{root}/shm.im.shm"
    shm = dao.shm(shmPath, np.zeros(shape, dtype=dtype))

    # track golden frames
    gold_data = []
    gold_data.append(shm.get_data())

    # create config
    yml = {}
    yml["telemetry_root"] = root
    yml["telemetry"].append({
        "target": shmPath,
        "limit": nFrames
    })
    config = yaml.dump(yml)

    SetConfig(config)
    StateTransition(i, "Init", "Standby")
    StateTransition(i, "Enable", "Idle")
    StateTransition(i, "Run", "Running")

    # write golden frames
    for _ in range(nFrames - 1):
        data = np.random.randint(min, max, size=shape, dtype=dtype)
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

def test_StateMachine(i: daoCommandIfce):
    InitTestRoutine()
    """ 
       Checks config set via network was applied.
    """
    tThreshold = 120 # number seconds to run test for
    pThreshold = 0.5 # probability of taking forward transition.
    t0 = perf_counter()
    
    while (perf_counter() - t0) < tThreshold:
        state = QueryState(i)

        if state == "Off":
            StateTransition(i, "Init", "Standby")
        elif state == "Running":
            StateTransition(i, "Idle", "Idle")
        else:
            transition_lookup = {
                "Standby": [
                    ("Enable", "Idle"), # forward transition
                    ("Stop", "Off") # back transition
                ],
                
                "Idle": [
                    ("Run", "Running"), # forward transition
                    ("Idle", "Idle") # back transition
                ]   
            }
            
            assert(state in transition_lookup)
            i = 0 if np.random.random() <= pThreshold else 1
            transition, goto_state = transition_lookup[state][i]
            StateTransition(i, transition, goto_state)
        