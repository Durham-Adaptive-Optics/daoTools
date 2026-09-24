"""Numerical regression tests for daoBase's primary API migration.

See tests/README.md for the isolated baseline/current build setup.
All SHM names are private to each test; no installed executables are launched.
"""
import ctypes as C
import os
from pathlib import Path
import subprocess
import shutil
import time

import numpy as np
import pytest
import daoShm

if not hasattr(daoShm.daoLib, 'daoShmSetData'):
    pytest.skip('Requires daoBase primary API; see tests/README.md', allow_module_level=True)

P = C.POINTER(daoShm.IMAGE)
SUCCESS = 0
REAL_TYPES = [np.uint8, np.int8, np.uint16, np.int16, np.uint32,
              np.int32, np.uint64, np.int64, np.float32, np.float64]


def bind(lib, name, args):
    function = getattr(lib, name)
    function.argtypes = args
    function.restype = C.c_int8
    return function


def ptr(stream):
    return C.byref(stream.image)


@pytest.fixture
def streams(tmp_path):
    handles = []

    def make(data, depth=1):
        path = tmp_path / f's{len(handles)}.im.shm'
        stream = daoShm.shm(str(path), np.ascontiguousarray(data), depth=depth)
        handles.append(stream)
        return stream
    yield make
    for stream in reversed(handles):
        stream.close()


@pytest.fixture
def libraries(tmp_path):
    if not all(os.getenv(name) for name in ('DAO_TOOLS_BASELINE_LIB', 'DAO_TOOLS_LIB')):
        pytest.skip('Set baseline/current library paths; see tests/README.md')
    paths = [os.environ['DAO_TOOLS_BASELINE_LIB'], os.environ['DAO_TOOLS_LIB']]
    assert Path(paths[0]).resolve() != Path(paths[1]).resolve()
    # These libraries cache mask pointers/counters in C static variables.
    # Give each test fresh library instances, as separate CLI processes have.
    # A reused mapping address with cnt0=1 must not leak across test cases.
    loaded = []
    for index, path in enumerate(paths):
        copy = tmp_path / f'libdaoTools-{index}{Path(path).suffix}'
        shutil.copyfile(path, copy)
        loaded.append(C.CDLL(str(copy)))
    return loaded


def published(stream, before, expected, exact=False):
    actual = stream.get_data().copy()
    if exact:
        np.testing.assert_array_equal(actual, expected)
    else:
        np.testing.assert_allclose(actual, expected, rtol=3e-6, atol=1e-7)
    assert stream.get_counter() == before + 1
    assert stream.image.md[0].write == 0
    return actual


@pytest.mark.parametrize('dtype', REAL_TYPES + [np.complex64, np.complex128])
@pytest.mark.parametrize('legacy', [True, False])
def test_create_open_write_finalize_wait(tmp_path, dtype, legacy):
    """Exercise all six migrated create/open/write/finalize/wait contracts."""
    lib = daoShm.daoLib
    create = bind(lib, 'daoShmImageCreate' if legacy else 'daoShmCreate',
                  [P, C.c_char_p, C.c_long, C.POINTER(C.c_uint32),
                   C.c_uint8, C.c_int, C.c_int])
    open_ = bind(lib, 'daoShmShm2Img' if legacy else 'daoShmOpen', [C.c_char_p, P])
    finalize = bind(lib, 'daoShmImagePart2ShmFinalize' if legacy
                    else 'daoShmSetDataPartFinalize', [P])
    wait = bind(lib, 'daoShmWaitForSemaphore' if legacy else 'daoShmWaitSem',
                [P, C.c_int32])
    timed = bind(lib, 'daoShmWaitForSemaphoreTimeout' if legacy
                 else 'daoShmWaitSemTimeout',
                 [P, C.c_int32, C.POINTER(daoShm.timespec)])
    write = bind(lib, 'daoShmImage2Shm' if legacy else 'daoShmSetData',
                 [C.c_void_p, C.c_uint32, P] if legacy else
                 [P, C.c_void_p, C.c_uint32])
    close = bind(lib, 'daoShmClose', [P])
    data = np.arange(12).reshape(3, 4).astype(dtype)
    if np.issubdtype(dtype, np.integer):
        limits = np.iinfo(dtype)
        data.flat[:4] = [limits.min, limits.max, 0, 1]
    else:
        data = (data - 6) / 4
        if np.issubdtype(dtype, np.complexfloating):
            data += 1j * data
    filename = str(tmp_path / 'api.im.shm').encode()
    writer, reader = daoShm.IMAGE(), daoShm.IMAGE()
    shape = (C.c_uint32 * 2)(3, 4)
    # Type codes in dao.h are ordered like this list.
    type_code = (REAL_TYPES + [np.complex64, np.complex128]).index(dtype) + 1
    assert create(C.byref(writer), filename, 2, shape, type_code, 1, 0) == SUCCESS
    try:
        assert open_(filename, C.byref(reader)) == SUCCESS
        try:
            before = reader.md[0].cnt0
            for iteration in range(3):
                frame = np.ascontiguousarray(data + iteration, dtype=dtype)
                if legacy:
                    result = write(frame.ctypes.data, frame.size, C.byref(writer))
                else:
                    result = write(C.byref(writer), frame.ctypes.data, frame.size)
                assert result == SUCCESS
                assert wait(C.byref(reader), 0) == SUCCESS
                deadline = daoShm.make_timespec_from_now(0.2)
                assert timed(C.byref(reader), 1, C.byref(deadline)) == SUCCESS
                raw = C.string_at(reader.array, frame.nbytes)
                np.testing.assert_array_equal(np.frombuffer(raw, dtype=dtype).reshape(3, 4), frame)
                assert reader.md[0].cnt0 == before + iteration + 1
            frame = np.ascontiguousarray(data * 2, dtype=dtype)
            C.memmove(writer.array, frame.ctypes.data, frame.nbytes)
            assert finalize(C.byref(writer)) == SUCCESS
            assert wait(C.byref(reader), 0) == SUCCESS
            assert reader.md[0].cnt0 == before + 4
            np.testing.assert_array_equal(
                np.frombuffer(C.string_at(reader.array, frame.nbytes), dtype=dtype),
                frame.ravel())
            # Semaphore 0 was consumed; a past deadline must time out.
            expired = daoShm.timespec(tv_sec=0, tv_nsec=0)
            assert timed(C.byref(reader), 0, C.byref(expired)) == -1
        finally:
            close(C.byref(reader))
    finally:
        close(C.byref(writer))


@pytest.mark.parametrize('dtype', REAL_TYPES)
@pytest.mark.parametrize('channels', [1, 4, 6, 8])
@pytest.mark.parametrize('depth', [1, 3])
def test_combine_api(streams, dtype, channels, depth):
    arrays = [np.full((2, 3), i + 1, dtype=dtype) for i in range(channels)]
    inputs = [streams(a, depth) for a in arrays]
    pointers = (P * channels)(*[C.pointer(s.image) for s in inputs])
    results = []
    for name in ('daoShmCombineShm2Shm', 'daoShmCombine'):
        out = streams(np.zeros((2, 3), dtype=dtype), depth)
        combine = bind(daoShm.daoLib, name, [C.POINTER(P), P, C.c_int, C.c_int])
        for frame in range(5):  # includes FIFO wrap-around
            for s, a in zip(inputs, arrays):
                s.set_data(a + frame)
            before = out.get_counter()
            assert combine(pointers, ptr(out), channels, 6) == SUCCESS
            expected = np.sum([a + frame for a in arrays], axis=0).astype(dtype)
            results.append(published(out, before, expected, exact=True))
    for left, right in zip(results[:5], results[5:]):
        np.testing.assert_array_equal(left, right)


@pytest.mark.parametrize('dtype', REAL_TYPES)
@pytest.mark.parametrize('precision', [np.float32, np.float64])
def test_calibration(streams, libraries, dtype, precision):
    raw = np.arange(12).reshape(3, 4).astype(dtype)
    ff = np.linspace(0.25, 2, 12).reshape(3, 4).astype(precision)
    bg = np.full((3, 4), 1.5, dtype=precision)
    inputs = [streams(a) for a in (raw, ff, bg)]
    inputs[0].image.md[0].cnt2 = 73
    name = 'daoToolsShmCalibrate' + ('64' if precision == np.float64 else '')
    actual = []
    for lib in libraries:
        out = streams(np.zeros((3, 4), dtype=precision))
        before = out.get_counter()
        assert bind(lib, name, [P] * 4)(*[ptr(s) for s in inputs], ptr(out)) == SUCCESS
        actual.append(published(out, before, (raw.astype(precision) - bg) * ff))
        assert out.image.md[0].cnt2 == 73
    np.testing.assert_array_equal(*actual)


@pytest.mark.parametrize('dtype', REAL_TYPES)
def test_pws_calibration(streams, libraries, dtype):
    raw = np.arange(12).reshape(3, 4).astype(dtype)
    ff = np.full((3, 4), 0.5, dtype=np.float32)
    bg = np.full((3, 4), 0.25, dtype=np.float32)
    mask = np.array([[2, -1, 0, -1], [-1, 1, -1, 3], [-1, -1, -1, -1]], np.int32)
    inputs = [streams(a) for a in (raw, ff, bg, mask)]
    actual = []
    expected = np.zeros(4, np.float32)
    valid = mask >= 0
    expected[mask[valid]] = (raw.astype(np.float32) * ff - bg)[valid]
    for lib in libraries:
        out, flux = streams(np.zeros((4, 1), np.float32)), streams(np.zeros((1, 1), np.float32))
        before = out.get_counter()
        assert bind(lib, 'daoToolsShmCalibratePws', [P] * 6)(
            *[ptr(s) for s in inputs], ptr(out), ptr(flux)) == SUCCESS
        actual.append(published(out, before, expected.reshape(4, 1)))
        np.testing.assert_allclose(flux.get_data(), expected.sum())
    np.testing.assert_array_equal(*actual)


@pytest.mark.parametrize('dtype', REAL_TYPES)
def test_extract(streams, libraries, dtype):
    data = np.arange(12).reshape(3, 4).astype(dtype)
    mask = np.array([[1, 0, 1, 0], [0, 1, 0, 1], [1, 0, 0, 1]], np.uint32)
    inp, m = streams(data), streams(mask)
    actual = []
    for lib in libraries:
        out = streams(np.zeros((6, 1), dtype))
        before = out.get_counter()
        assert bind(lib, 'daoToolsShmExtract', [P] * 3)(ptr(inp), ptr(m), ptr(out)) == SUCCESS
        actual.append(published(out, before, data[mask == 1].reshape(6, 1), exact=True))
    np.testing.assert_array_equal(*actual)


@pytest.mark.parametrize('dtype', [np.float32, np.float64])
@pytest.mark.parametrize('mode', ['plain', 'dual', 'norm_a', 'flux_diff', 'flux_a'])
@pytest.mark.parametrize('normalize', [0, 1])
def test_subtract_extract(streams, libraries, dtype, mode, normalize):
    a = np.arange(12).reshape(3, 4).astype(dtype) + 1
    b = np.full((3, 4), 2, dtype)
    mask = np.array([[1, 0, 1, 0], [0, 1, 0, 1], [1, 0, 0, 1]], np.uint32)
    inputs = [streams(x) for x in (a, b, mask)]
    norm_a, norm_b = streams(np.full((1, 1), 4, dtype)), streams(np.full((1, 1), 2, dtype))
    x, y = a[mask == 1], b[mask == 1]
    if mode == 'plain':
        name = 'daoToolsShmSubstractExtractFinalize'
        extra = [ptr(norm_a), normalize]
        expected = (x - y) / (4 if normalize else 1)
    elif mode == 'dual':
        name = 'daoToolsShmSubstractExtractDualNormFinalize'
        extra = [ptr(norm_a), ptr(norm_b), normalize]
        expected = x / 4 - y / 2 if normalize else x - y
    elif mode == 'norm_a':
        name = 'daoToolsShmSubstractExtractNormAFinalize'
        extra = [ptr(norm_a), normalize]
        expected = x / 4 - y if normalize else x - y
    elif mode == 'flux_diff':
        name, extra = 'daoToolsShmSubstractExtractNorm', []
        expected = (x - y) / np.maximum(x - y, 1).sum()
    else:
        name, extra = 'daoToolsShmSubstractExtractNormImage', []
        expected = x / x.sum() - y
    actual = []
    signature = [P] * (4 + max(len(extra) - 1, 0)) + ([C.c_int] if extra else [])
    for lib in libraries:
        out = streams(np.zeros((6, 1), dtype))
        before = out.get_counter()
        assert bind(lib, name, signature)(*[ptr(s) for s in inputs], ptr(out), *extra) == SUCCESS
        actual.append(published(out, before, expected.reshape(6, 1)))
    np.testing.assert_array_equal(*actual)


@pytest.mark.parametrize('dtype', [np.float32, np.float64])
@pytest.mark.parametrize('finalize', [0, 1])
def test_copy_to_position(streams, libraries, dtype, finalize):
    data = np.array([[-2.5], [0], [3.25]], dtype)
    source = streams(data)
    actual = []
    for lib in libraries:
        out = streams(np.full((8, 1), 7, dtype))
        before = out.get_counter()
        assert bind(lib, 'daoShmCopyToPosition', [P, P, C.c_int, C.c_int, C.c_int])(
            ptr(source), ptr(out), 3, 2, finalize) == SUCCESS
        expected = np.full((8, 1), 7, dtype)
        expected[2:5] = data
        np.testing.assert_array_equal(out.get_data(), expected)
        assert out.get_counter() == before + finalize
        actual.append(out.get_data().copy())
    np.testing.assert_array_equal(*actual)


@pytest.mark.parametrize('dtype', [np.float32, np.float64])
@pytest.mark.parametrize('channels', [1, 4, 6, 8])
@pytest.mark.parametrize('remove_piston', [0, 1])
def test_dm_combine_unchanged(streams, libraries, dtype, channels, remove_piston):
    """Preserve current behavior, including existing clipping/piston semantics."""
    arrays = [np.linspace(-3, 4, 12).reshape(3, 4).astype(dtype) + i / 4
              for i in range(channels)]
    inputs = [streams(a) for a in arrays]
    pointers = (P * channels)(*[C.pointer(s.image) for s in inputs])
    actual = []
    for lib in libraries:
        out = streams(np.zeros((3, 4), dtype))
        before = out.get_counter()
        assert bind(lib, 'daoDmCombine', [C.POINTER(P), P, C.c_int, C.c_int, C.c_int, C.c_double])(
            pointers, ptr(out), channels, 12, remove_piston, 2.0) == SUCCESS
        assert out.get_counter() == before + 1
        actual.append(out.get_data().copy())
    np.testing.assert_array_equal(*actual)


@pytest.mark.parametrize('case', ['gain', 'modal_gain', 'mvm', 'calibrate', 'extract',
                                 'downsample_mean', 'downsample_sum'])
@pytest.mark.parametrize('precision', [np.float32, np.float64])
def test_executable_pipeline(tmp_path, libraries, case, precision):
    """Run actual original/new processes and compare three deterministic frames."""
    import tempfile
    roots = [Path(os.environ['DAO_TOOLS_BASELINE_LIB']).resolve().parent.parent,
             Path(os.environ['DAO_TOOLS_LIB']).resolve().parent.parent]
    results = []
    for root in roots:
        # Several legacy command-line tools have 32-byte filename buffers.
        with tempfile.TemporaryDirectory(prefix='dt', dir='/tmp') as directory:
            handles, paths = [], []

            def make(data):
                path = str(Path(directory) / f'{len(handles)}.im.shm')
                assert len(path) < 32
                s = daoShm.shm(path, np.ascontiguousarray(data))
                handles.append(s)
                paths.append(path)
                return s

            data = np.arange(1, 13).reshape(3, 4).astype(precision)
            if case in ('gain', 'modal_gain'):
                inp = make(data)
                gain = np.linspace(0.25, 2, 12).reshape(3, 4).astype(precision)
                make(gain if case == 'modal_gain' else np.array([[0.5]], precision))
                out = make(np.zeros_like(data))
                executable = 'daoApplyGain'
                args = (['-m'] if case == 'modal_gain' else []) + ['-S', *paths, '-s', '0', '-L']
                reference = lambda frame: frame * (gain if case == 'modal_gain' else 0.5)
            elif case == 'mvm':
                data = np.arange(1, 5).reshape(4, 1).astype(precision)
                inp = make(data)
                matrix = np.array([[1, -2, 3, 0.5], [0, 0.25, -1, 2],
                                   [2, 1, 0, -0.25]], precision)
                make(matrix)
                out = make(np.zeros((3, 1), precision))
                executable, args = 'daoMvM', ['-S', *paths, '-s', '0', '-N', '1', '-L']
                reference = lambda frame: matrix @ frame
            elif case == 'calibrate':
                inp = make(data)
                make(np.full(data.shape, 0.5, np.float32))
                make(np.full(data.shape, 2, np.float32))
                out = make(np.zeros(data.shape, np.float32))
                executable, args = 'daoPixelCalibrate', ['-S', *paths, '-s', '0', '-L']
                reference = lambda frame: (frame - 2) * 0.5
            elif case == 'extract':
                inp = make(data)
                mask = np.array([[1, 0, 1, 0], [0, 1, 0, 1], [1, 0, 0, 1]], np.uint32)
                make(mask)
                out = make(np.zeros((6, 1), precision))
                executable, args = 'daoPixelExtract', ['-S', *paths, '-s', '0', '-L']
                reference = lambda frame: frame[mask == 1].reshape(6, 1)
            else:
                data = np.arange(1, 25).reshape(4, 6).astype(np.uint16)
                inp = make(data)
                out = make(np.zeros((2, 2), np.uint16))
                executable, args = 'daoDownsample', paths + (['-s'] if case.endswith('sum') else [])

                def reference(frame):
                    total = frame.reshape(2, 2, 2, 3).sum(axis=(1, 3))
                    return (total if case.endswith('sum') else np.floor(total / 6 + 0.5)).astype(np.uint16)

            env = dict(os.environ)
            env['LD_LIBRARY_PATH'] = str(root / 'src') + ':' + env.get('LD_LIBRARY_PATH', '')
            env['OPENBLAS_NUM_THREADS'] = '1'
            log_path = tmp_path / f'{root.name}-{case}.log'
            try:
                with log_path.open('w') as log:
                    process = subprocess.Popen([str(root / 'apps' / executable), *args],
                                               env=env, stdout=log, stderr=log)
                    try:
                        def await_frame(frame):
                            previous = out.get_counter()
                            deadline = time.monotonic() + 5
                            while time.monotonic() < deadline:
                                assert process.poll() is None, log_path.read_text()
                                inp.set_data(frame)
                                time.sleep(0.01)
                                if out.get_counter() > previous:
                                    value = out.get_data().copy()
                                    if np.allclose(value, reference(frame), rtol=3e-6, atol=1e-7):
                                        return value
                            pytest.fail('No expected output within 5 seconds: ' + log_path.read_text())

                        # Synchronize startup before testing distinct input frames.
                        await_frame(data)
                        values = [await_frame(np.ascontiguousarray(data + k)) for k in (1, 4, 2)]
                        results.append(values)
                    finally:
                        process.terminate()
                        try:
                            process.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=2)
            finally:
                for s in reversed(handles):
                    s.close()
    for before, after in zip(*results):
        np.testing.assert_array_equal(before, after)


@pytest.mark.parametrize('dtype', [np.float32, np.float64])
@pytest.mark.parametrize('empty_mask', [False, True])
@pytest.mark.parametrize('name', ['daoToolsShmSubstractExtractNorm',
                                  'daoToolsShmSubstractExtractNormImage'])
def test_zero_flux_and_empty_mask(streams, libraries, dtype, empty_mask, name):
    a, b = streams(np.zeros((2, 3), dtype)), streams(np.zeros((2, 3), dtype))
    mask = streams(np.full((2, 3), 0 if empty_mask else 1, np.uint32))
    size = 1 if empty_mask else 6
    expected = np.full((size, 1), 7, dtype)
    if not empty_mask and name == 'daoToolsShmSubstractExtractNorm':
        expected.fill(0)
    results = []
    for lib in libraries:
        out = streams(np.full((size, 1), 7, dtype))
        before = out.get_counter()
        assert bind(lib, name, [P] * 4)(ptr(a), ptr(b), ptr(mask), ptr(out)) == SUCCESS
        results.append(published(out, before, expected, exact=True))
    np.testing.assert_array_equal(*results)


@pytest.mark.parametrize('name', ['daoToolsShmSubstractExtractFinalize',
                                  'daoToolsShmSubstractExtractNormAFinalize',
                                  'daoToolsShmSubstractExtractDualNormFinalize'])
def test_invalid_mask_does_not_publish(streams, libraries, name):
    a, b = streams(np.ones((2, 3), np.float32)), streams(np.ones((2, 3), np.float32))
    mask = streams(np.ones((2, 3), np.float32))  # required type is uint32
    norm = streams(np.ones((1, 1), np.float32))
    extra = [ptr(norm)] * (2 if 'DualNorm' in name else 1) + [1]
    for lib in libraries:
        out = streams(np.full((6, 1), 7, np.float32))
        before = out.get_counter()
        assert bind(lib, name, [P] * (3 + len(extra)) + [C.c_int])(
            ptr(a), ptr(b), ptr(mask), ptr(out), *extra) == 1
        assert out.get_counter() == before
        np.testing.assert_array_equal(out.get_data(), 7)
