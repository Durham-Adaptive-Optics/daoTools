'''
 # @ Author: Thomas N. Davies
 # @ Company: Centre for Advanced Instrumentation, Durham University
 # @ Contact: thomas.n.davies@durham.ac.uk
 # @ Create Time: 2026-05-18 10:01:47
 # @ Description: Data Acquisition (DAQ) Tool - Test Suite
 '''

# ================================================================ #

from daoDAQClient import DAQClient, DAQState
import astropy.io.fits as fits
import subprocess as sp
import numpy as np
import pytest
import shutil
import signal
import math
import yaml
import time
import dao
import os

# ================================================================ #
    
''' Tests for ensuring shared memory samples are correctly captured '''

def test_Smem_Rollover(tmp_directory, client, tool_inst):
    smem_tester_(tmp_directory, client, tool_inst, (2,3), np.float32, rollover=True, eager_start=False)

def test_Smem_EagerStart(tmp_directory, client, tool_inst):
    smem_tester_(tmp_directory, client, tool_inst, (2,3), np.float32, rollover=False, eager_start=True)

def test_Smem_Missing(tmp_directory, client, tool_inst):
    daqConfig = {
        "root_storage": tmp_directory,
        "sources": [{
            "uri": f"smem:///tmp/__no-exist__.im.shm"
        }]
    }
    client.daq_session_configure_upload(yaml.dump(daqConfig))
    
    try: 
        client.daq_session_configure_apply()
    except Exception as e:
        pass

    expect_error_state(client)
    
@pytest.mark.parametrize("shape", [(2, 3), (2, 3, 4)])
@pytest.mark.parametrize("dtype", [
    np.uint8, np.int8, np.uint16, np.int16, np.uint32, 
    np.int32, np.uint64, np.int64, np.float32, np.float64
])
def test_Smem_Multidimensional(tmp_directory, client, tool_inst, shape, dtype):
    smem_tester_(tmp_directory, client, tool_inst, shape, dtype, rollover=False, eager_start=False)

''' Tests for ensuring Files are correctly captured '''

def test_File_DAQ(tmp_directory, client, request, tool_inst):
    # Record
    daqConfig = {
        "root_storage": tmp_directory,
        "sources": [{
            "uri": f"file://{request.fspath}"
        }]
    }
    client.daq_session_configure_upload(yaml.dump(daqConfig))
    client.daq_session_configure_apply()
    client.daq_session_begin()
    client.daq_session_await_finish(timeout=1)

    # Validate
    listing = os.listdir(tmp_directory)
    sessionDir = os.path.join(tmp_directory, listing[0])
    datafiles = [file for file in os.listdir(sessionDir)]
    assert os.path.basename(request.fspath) in datafiles

''' Tests for ensuring general tool operations function correctly '''

def test_Finish_Session(tmp_directory, tool_inst, client, request):
    session_tester_(tmp_directory, client, tool_inst, request, back2back=False)
    
def test_Consecutive_Sessions(tmp_directory, tool_inst, client, request):
    session_tester_(tmp_directory, client, tool_inst, request, back2back=True)

def test_Session_Alert(tmp_directory, client, tool_inst):
    error_tester_(tmp_directory, client, tool_inst, recover=False)

def test_Recovery(tmp_directory, client, tool_inst):
    error_tester_(tmp_directory, client, tool_inst, recover=True)

def test_Process_Terminate(tmp_directory, client, tool_inst):
    smemPath = f"/tmp/test_DAQ.im.shm"
    smem = dao.shm(smemPath, np.zeros((1,1)))

    daqConfig = {
        "root_storage": tmp_directory,
        "sources": [{
            "uri": f"smem://{smemPath}"
        }]
    }

    client.daq_session_configure_upload(yaml.dump(daqConfig))
    client.daq_session_configure_apply()
    client.daq_session_begin()
    
    tool_inst.send_signal(signal.SIGINT)
    exit_code = tool_inst.wait(timeout=3)
    assert 166 == exit_code

# ================================================================ #

def smem_tester_(tmp_directory, client, tool_inst, shape, dtype, rollover: bool, eager_start: bool):
    limits = np.iinfo(dtype) if np.issubdtype(dtype, np.integer) else np.finfo(dtype)
    new_sample = lambda: np.linspace(limits.min / 2, limits.max / 2, np.prod(shape), dtype=dtype).reshape(shape)
    nSamples, nRollover = 5, 2
    
    # Setup smem
    shmLocalName = "test_DAQ"
    smemPath = f"/tmp/{shmLocalName}.im.shm"
    smem = dao.shm(smemPath, new_sample())

    # Record
    daqConfig = {
        "root_storage": tmp_directory,
        "sources": [{
            "uri": f"smem://{smemPath}",
            "samples": nSamples,
            "eager_start": eager_start
        }]
    }
    if rollover:
        daqConfig["sources"][0]["file_rollover"] = nRollover
        
    client.daq_session_configure_upload(yaml.dump(daqConfig))
    client.daq_session_configure_apply()
    client.daq_session_begin()

    # Write samples to smem
    sample_history = []
    def sample_history_add():
        sample = smem.get_data()
        metadata = smem.get_meta_data()
        sample_history.append([sample, {
            "atype": metadata["atype"],
            "atime": metadata["atime"]["tsfixed"]["secondlong"],
            "cnt0": metadata["cnt0"],
            "cnt1": metadata["cnt1"],
            "cnt2": metadata["cnt2"]
        }])
        
    if eager_start: sample_history_add()
    
    N = (nSamples - 1) if eager_start else nSamples
    for _ in range(N):
        sample = new_sample()
        smem.set_data(sample)
        sample_history_add()
        time.sleep(0.1) # feed sample in slowly as we want to ensure correctness, not test performance.
    
    # Await record to finish
    client.daq_session_await_finish(timeout=1)
    
    # Validate
    listing = os.listdir(tmp_directory)
    sessionDir = os.path.join(tmp_directory, listing[0])
    
    recorded_samples = []
    def add_recorded_sample(hdu):
        sample = hdu.data
        metadata = hdu.header
        recorded_samples.append([sample, {
            "atype": metadata["atype"],
            "atime": metadata["atime"],
            "cnt0": metadata["cnt0"],
            "cnt1": metadata["cnt1"],
            "cnt2": metadata["cnt2"]
        }])
        
    if rollover:
        nDatafiles: int = math.ceil(nSamples / nRollover)
        dataDirPath = os.path.join(sessionDir, shmLocalName)
        for i in range(nDatafiles):
            datafilePath = os.path.join(dataDirPath, f"{shmLocalName}_{i}.fits")
            with fits.open(datafilePath, mode='readonly') as datafile:
                for hdu in datafile: add_recorded_sample(hdu)
    else:
        datafilePath = os.path.join(sessionDir, f"{shmLocalName}.fits")
        with fits.open(datafilePath, mode='readonly') as datafile:
            for hdu in datafile: add_recorded_sample(hdu)

    for i in range(nSamples):
        reference_sample = sample_history[i]
        recorded_sample = recorded_samples[i]
        assert recorded_sample[0].dtype.type == reference_sample[0].dtype.type, f"Mismatched datatype between reference and recorded sample ({i})"
        assert np.array_equal(recorded_sample[0], reference_sample[0]), f"Mismatched values between reference and recorded sample ({i})\nREF:{reference_sample[0]}\nREC:{recorded_sample[0]}"
        assert recorded_sample[1] == reference_sample[1], f"Mismatched metadata between reference and recorded sample ({i})\nREF:{reference_sample[1]}\nREC:{recorded_sample[1]}"

def session_tester_(tmp_directory, client, tool_inst, request, back2back: bool):
    smemPath = f"/tmp/test_DAQ.im.shm"
    smem = dao.shm(smemPath, np.zeros((1,1)))

    daqConfig = {
        "root_storage": tmp_directory,
        "sources": 
        [
            {"uri": f"smem://{smemPath}"},
            {"uri": f"file://{request.fspath}"}
        ]
    }

    client.daq_session_configure_upload(yaml.dump(daqConfig))
    client.daq_session_configure_apply()
    
    client.daq_session_begin()
    client.daq_session_finish()
    time.sleep(1) # required due to limitation of timestamp naming resolution.

    if back2back:
        client.daq_session_begin()
        client.daq_session_finish()

def error_tester_(tmp_directory, client, tool_inst, recover: bool):
    daqConfig = {
        "root_storage": tmp_directory,
        "sources": 
        [
            {"uri": f"file://non-existent-file.test"},
        ]
    }

    client.daq_session_configure_upload(yaml.dump(daqConfig))
    client.daq_session_configure_apply()
    client.daq_session_begin()
    
    expect_error_state(client)
            
    if recover:
        client.recover()

def expect_error_state(client, timeout=1):
    t0 = time.perf_counter()
    while 1:
        if time.perf_counter() - t0 >= timeout:
            raise RuntimeError("tool never went into Error state")
        
        if DAQState.Error == client.state():
            break

# ================================================================ #

''' Fixture to create a clean directory for each test '''
@pytest.fixture
def tmp_directory():
    path = f"/tmp/dao_daq_test"
    os.makedirs(path)
    yield path
    shutil.rmtree(path)

''' Fixture to create a fresh instance of daoDAQ tool '''
@pytest.fixture
def tool_inst():
    inst = sp.Popen(["daoDAQ", "-s", "-ll"])
    yield inst
    if inst.poll() is None:
        inst.kill()
    inst.wait()
            
''' Fixture to create a fresh instance of daoDAQ client '''
@pytest.fixture
def client():
    yield DAQClient()
