'''
 # @ Author: Thomas N. Davies
 # @ Company: Centre for Advanced Instrumentation, Durham University
 # @ Contact: thomas.n.davies@durham.ac.uk
 # @ Create Time: 2026-05-18 10:01:47
 # @ Description: Data Acquisition (DAQ) Tool - System Test Suite
 '''

# ================================================================ #

from daoDAQClient import DAQClient, DAQState
from daoDAQParser import DAODAQParser
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

''' Defines the period to wait for an event to finalise before a test is failed. '''
DEFAULT_TIMEOUT: int = 5

# ================================================================ #

@pytest.fixture
def tmp_directory():
    ''' Fixture: creates a fresh temporary directory for each test and cleans it up on teardown. '''
    path = f"/tmp/dao_daq_test"
    os.makedirs(path)
    yield path
    shutil.rmtree(path)

@pytest.fixture
def tool_inst():
    ''' Fixture: spawns a fresh daoDAQ tool process for each test and kills it on teardown. '''
    inst = sp.Popen(["daoDAQ", "-s", "-vvv"])
    yield inst
    if inst.poll() is None:
        inst.kill()
    inst.wait()
            
@pytest.fixture
def client():
    ''' Fixture: creates a fresh DAQClient instance for each test. '''
    yield DAQClient(timeout_s=DEFAULT_TIMEOUT)

def expect_error_state(client, timeout=1):
    ''' Utility: polls the client state until the tool enters the Error state, or raises if it times out. '''
    t0 = time.perf_counter()
    while 1:
        if time.perf_counter() - t0 >= timeout:
            raise RuntimeError("tool never went into Error state")
        
        if DAQState.Error == client.state():
            break

# ================================================================ #
    
@pytest.mark.parametrize("shape", [(2, 3), (2, 3, 4)])
@pytest.mark.parametrize("dtype", [
    np.uint8, np.int8, 
    np.uint16, np.int16, 
    np.uint32, np.int32, 
    np.uint64, np.int64, 
    np.float32, np.float64
])
def test_SharedMemoryAcquisition(tmp_directory, client, tool_inst, shape, dtype):
    '''
    Verifies that a single sample can be correctly acquired from a shared memory resource and saved to disk.
    Parametrised across all supported dtypes and array shapes to ensure correct data and metadata
    are preserved end-to-end through the DAQ pipeline into the output FITS file.
    '''
    # Generate test sample..
    limits = np.iinfo(dtype) if np.issubdtype(dtype, np.integer) else np.finfo(dtype)
    test_sample = np.linspace(limits.min / 2, limits.max / 2, np.prod(shape), dtype=dtype).reshape(shape)

    # Setup shared-memory..
    shmLocalName = "test_DAQ"
    smemPath = f"/tmp/{shmLocalName}.im.shm"
    smem = dao.shm(smemPath, test_sample, depth=1)
    
    # Retrieve sample metadata from shm for verification..
    reference_md = {
        # shm metadata ..
        "atime": smem.get_meta_data()["atime"]["tsfixed"]["secondlong"],
        "atype": smem.get_meta_data()["atype"],
        "cnt0": smem.get_meta_data()["cnt0"],
        "cnt1": smem.get_meta_data()["cnt1"],
        "cnt2": smem.get_meta_data()["cnt2"],
    }
    
    # Setup DAQ..
    daqConfig = {
        "root_storage": tmp_directory,
        "eager_start": True,
        "sources": [{
            "uri": f"smem://{smemPath}",
            "samples": 1
        }]
    }
        
    # Acquire sample from shm..
    client.daq_session_configure_upload(yaml.dump(daqConfig))
    client.daq_session_configure_apply()
    client.daq_session_begin()
    client.daq_session_await_finish(timeout=DEFAULT_TIMEOUT)
   
    # Verify acquisition..
    session_parser = DAODAQParser(daqConfig["root_storage"])
    session_contents = session_parser.parse_latest_session()

    assert session_contents is not None, "No session directory"
    assert session_contents.num_resources == 1, "Session directory content number incorrect"
    assert shmLocalName in session_contents.resources, "Session directory content incorrect"
    
    resource = session_contents.resources.get(shmLocalName)
    assert len(resource.file_paths) == 1, "Incorrect number of FITS files"
    assert resource.num_samples == 1, "Incorrect sample count"
    assert resource.dtype.type == dtype, "Incorrect sample dtype"
    assert np.array_equal(resource.samples[0].data, test_sample), "Incorrect sample array"
    assert reference_md == {
            "atime": resource.samples[0].metadata.atime,
            "atype": resource.samples[0].metadata.atype,
            "cnt0": resource.samples[0].metadata.cnt0,
            "cnt1": resource.samples[0].metadata.cnt1,
            "cnt2": resource.samples[0].metadata.cnt2
    }, "Incorrect sample metadata"
    
def test_SharedMemoryRollover(tmp_directory, client, tool_inst):
    '''
    Verifies that FITS file rollover works correctly when acquiring multiple samples from shared memory.
    Writes a series of samples to shared memory and checks that the DAQ tool splits them across the
    expected number of FITS files, and that all sample data and metadata are correctly preserved.
    '''
    SAMPLES_TO_COLLECT: int = 10
    SAMPLES_PER_FILE: int = 3
    WRITE_PERIOD: int = 30

    ref_data, ref_md = {}, {}
    
    # Generate test sample..
    dtype, shape = np.float32, (1,1)
    limits = np.finfo(dtype)
    new_sample_func = lambda: np.random.uniform(limits.min / 2, limits.max / 2, np.prod(shape)).astype(dtype).reshape(shape)
    
    # Setup shared-memory..
    shmLocalName = "test_DAQ"
    smemPath = f"/tmp/{shmLocalName}.im.shm"
    smem = dao.shm(smemPath, np.zeros(shape, dtype=dtype), depth=1)
    
    # Setup DAQ..
    daqConfig = {
        "root_storage": tmp_directory,
        "sources": [{
            "uri": f"smem://{smemPath}",
            "samples": SAMPLES_TO_COLLECT,
            "file_rollover": SAMPLES_PER_FILE,
            "eager_start": False
        }]
    }
        
    # Acquire samples from shm..
    client.daq_session_configure_upload(yaml.dump(daqConfig))
    client.daq_session_configure_apply()
    client.daq_session_begin()
    
    # Write samples to shm..
    t0 = time.perf_counter()
    while 1:
        new_sample = new_sample_func()
        smem.set_data(new_sample)
        
        cnt0 = smem.get_meta_data()["cnt0"]
        ref_data[cnt0] = new_sample
        ref_md[cnt0] = {
            "atime": smem.get_meta_data()["atime"]["tsfixed"]["secondlong"],
            "atype": smem.get_meta_data()["atype"],
            "cnt1": smem.get_meta_data()["cnt1"],
            "cnt2": smem.get_meta_data()["cnt2"],
        }
        
        if client.state() is DAQState.Ready:
            break

        if time.perf_counter() - t0 >= WRITE_PERIOD:
            raise RuntimeError(f"daq tool failed to capture {SAMPLES_TO_COLLECT} samples within {WRITE_PERIOD}s test-period")
        
    # Verify acquisition..
    session_parser = DAODAQParser(daqConfig["root_storage"])
    session_contents = session_parser.parse_latest_session()

    assert session_contents is not None, "No session directory"
    assert session_contents.num_resources == 1, "Session directory content number incorrect"
    assert shmLocalName in session_contents.resources, "Session directory content incorrect"
    
    resource = session_contents.resources.get(shmLocalName)
    assert len(resource.file_paths) == math.ceil(SAMPLES_TO_COLLECT / SAMPLES_PER_FILE), "Incorrect number of FITS files"
    assert resource.num_samples == SAMPLES_TO_COLLECT, "Incorrect sample count"
    assert resource.dtype.type == dtype, "Incorrect sample dtype"
    
    for acquired_sample in resource.samples:
        assert acquired_sample.metadata.cnt0 in ref_data, "No reference array found"
        assert acquired_sample.metadata.cnt0 in ref_md, "No reference metadata found"
        assert np.array_equal(acquired_sample.data, ref_data.get(acquired_sample.metadata.cnt0)), "Incorrect sample array"
        assert ref_md.get(acquired_sample.metadata.cnt0) == {
            "atime": acquired_sample.metadata.atime,
            "atype": acquired_sample.metadata.atype,
            "cnt1": acquired_sample.metadata.cnt1,
            "cnt2": acquired_sample.metadata.cnt2
        }, "Incorrect sample metadata"
  
def test_Smem_Missing(tmp_directory, client, tool_inst):
    '''
    Verifies that the tool transitions to the Error state when configured with a shared memory
    path that does not exist on the system.
    '''
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

def test_File_DAQ(tmp_directory, client, request, tool_inst):
    '''
    Verifies that a file source can be acquired and saved correctly to the session directory.
    Uses this test file itself as the source, then checks it appears in the output session directory.
    '''
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
    client.daq_session_await_finish(timeout=DEFAULT_TIMEOUT)

    # Validate
    listing = os.listdir(tmp_directory)
    sessionDir = os.path.join(tmp_directory, listing[0])
    datafiles = [file for file in os.listdir(sessionDir)]
    assert os.path.basename(request.fspath) in datafiles

def test_ManualSessionControl(tmp_directory, client, tool_inst, request):
    '''
    Verifies that multiple DAQ sessions can be started and stopped manually in sequence.
    Configures both a shared memory and file source, then begins and finishes two back-to-back
    sessions to confirm the tool handles repeated session lifecycle correctly.
    '''
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
    
    client.daq_session_begin()
    client.daq_session_finish()

def test_AlertAndRecovery(tmp_directory, client, tool_inst):
    '''
    Verifies that the tool can recover from an Error state after a bad configuration is applied.
    Configures a non-existent file source to trigger an error, confirms the Error state is reached,
    then calls recover() to check the tool can return to a healthy state.
    '''
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
            
    client.recover()
    
def test_Process_Terminate(tmp_directory, client, tool_inst):
    '''
    Verifies that the daoDAQ tool exits cleanly with the expected exit code (166) when sent a SIGINT
    while a DAQ session is active.
    '''
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
    exit_code = tool_inst.wait(timeout=DEFAULT_TIMEOUT)
    assert 166 == exit_code