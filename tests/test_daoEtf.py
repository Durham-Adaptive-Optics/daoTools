"""daoEtfLib (model, measured ETF, files, measurement with in-memory SHMs) and daoEtfDisp
(offscreen). No live SHM is touched."""
import importlib.util
import os
from pathlib import Path
import sys
from types import SimpleNamespace
from unittest.mock import patch

os.environ.setdefault("PYQTGRAPH_QT_LIB", "PyQt5")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src" / "python"))
import daoEtfLib as etf  # noqa: E402


# ------------------------------------------------------------------------------ model
def test_model_zero_gain_is_0db():
    f = np.logspace(0, 2.5, 50)
    assert np.allclose(etf.model_db(f, 0.0, 1e-3, 36e-6, 375e-6, 572e-6), 0.0)


def test_model_rejects_low_frequencies_and_overshoots_near_nyquist():
    f = np.logspace(0, np.log10(499), 400)
    db = etf.model_db(f, 0.5, 1e-3, 36e-6, 375e-6, 572e-6)
    assert db[0] < -30                     # integrator: strong rejection at 1 Hz
    assert db.max() > 0                    # the delays give an overshoot
    bw = etf.first_crossing(f, db)
    assert 10 < bw < 200


def test_only_the_sum_of_the_delays_matters():
    f = np.logspace(0, 2.5, 50)
    a = etf.model_db(f, 0.4, 1e-3, 100e-6, 200e-6, 300e-6)
    b = etf.model_db(f, 0.4, 1e-3, 300e-6, 300e-6, 0.0)
    assert np.allclose(a, b)


def test_measured_etf_of_a_simulated_integrator_loop():
    """A discrete integrator loop with a 2-frame delay: the measured ETF follows the discrete
    rejection 1 / (1 + g z^-2 / (1 - z^-1))."""
    rng = np.random.default_rng(0)
    fs, g, n = 1000.0, 0.4, 40000
    d = rng.standard_normal(n)
    res, cmd = np.zeros(n), np.zeros(n)
    for i in range(n):
        res[i] = d[i] - (cmd[i - 2] if i >= 2 else 0.0)
        cmd[i] = (cmd[i - 1] if i else 0.0) + g * res[i]
    f, measured, _ = etf.measured_etf(res, d, fs, 512)
    z = np.exp(2j * np.pi * f[1:] / fs)
    theory = 20 * np.log10(np.abs(1 / (1 + g * z ** -2 / (1 - z ** -1))))
    assert np.max(np.abs(measured[1:] - theory)) < 1.5


# ------------------------------------------------------------------------------ files
def _dataset(n_modes=3, n=4000, mode=1):
    rng = np.random.default_rng(1)
    return {"out": rng.standard_normal((n_modes, n)), "inp": rng.standard_normal((n_modes, n)),
            "fs": 1000.0, "mode": mode, "amp_mode": 0.01, "N": 4, "start": 10, "end": 3000,
            "loop_gain": 0.3}


def test_save_then_load_last(tmp_path):
    data = _dataset()
    path = etf.save_etf_data({"data_rtf": data}, str(tmp_path), tracking_number="20260101_000000")
    assert path.endswith("20260101_000000_data_rtf.npy")
    assert etf.find_last_etf_file(str(tmp_path)) == path
    loaded = etf.load_etf_data(path)
    assert loaded["mode"] == 1 and loaded["fs"] == 1000.0
    assert np.array_equal(loaded["out"], data["out"])


@pytest.mark.parametrize("ext", ["npz", "pkl"])
def test_load_other_formats(tmp_path, ext):
    data = _dataset()
    path = str(tmp_path / f"etf.{ext}")
    if ext == "npz":
        np.savez(path, **data)
    else:
        import pickle
        with open(path, "wb") as h:
            pickle.dump({"data_rtf": data}, h)
    assert etf.load_etf_data(path)["mode"] == 1


def test_load_rejects_missing_keys(tmp_path):
    path = str(tmp_path / "bad.npy")
    np.save(path, {"out": np.zeros(3)}, allow_pickle=True)
    with pytest.raises(ValueError, match="missing keys"):
        etf.load_etf_data(path)


def test_find_last_without_pointer(tmp_path):
    with pytest.raises(FileNotFoundError):
        etf.find_last_etf_file(str(tmp_path))


def test_etf_from_data_window():
    f, db = etf.etf_from_data(_dataset(), 10, 3000, 4)
    assert f.size == db.size and f[0] > 0
    with pytest.raises(ValueError):
        etf.etf_from_data(_dataset(), 3000, 3000, 4)


# ------------------------------------------------------------------------------ measurement
class Shm:
    def __init__(self, data):
        self.data = np.array(data)
        self.writes = []

    def get_data(self, check=False):
        return self.data.copy()

    def set_data(self, data):
        assert data.shape == self.data.shape and data.dtype == self.data.dtype
        self.data = data.copy()
        self.writes.append(self.data.copy())


class Loop:
    """In-memory loop: the WFS modes = M2A^+ (disturbance - the integrator's command), with
    the integrator running only while the loop switch is 1. One frame per modes read."""

    def __init__(self, n_act=6, n_modes=4, gain=0.4, delay=2):
        rng = np.random.default_rng(2)
        self.m2a = Shm(rng.standard_normal((n_act, n_modes)))
        self.c2m = np.linalg.pinv(self.m2a.data)
        self.loop = Shm(np.zeros((1, 1), np.uint32))
        self.gain = Shm(np.full((1, 1), gain, np.float32))
        self.dist = Shm(np.zeros((n_act, 1), np.float32))
        self.cmds = [np.zeros(n_act)] * delay
        loop = self

        class Modes(Shm):
            def get_data(self, check=False):
                if check:
                    applied = loop.cmds[-len(loop.cmds)]
                    res = loop.c2m @ (loop.dist.data.ravel() - applied)
                    new = loop.cmds[-1] + (loop.gain.data[0, 0] * loop.m2a.data @ res
                                           if loop.loop.data[0, 0] else 0)
                    loop.cmds = loop.cmds[1:] + [new]
                    self.data = res.reshape(-1, 1)
                return self.data.copy()

        self.modes = Modes(np.zeros((n_modes, 1)))


def test_measure_etf_closed_then_open_and_leaves_the_channel_at_zero():
    L = Loop()
    calls = []
    data = etf.measure_etf(L.loop, L.gain, L.modes, L.dist, L.m2a, mode=2, amplitude=0.1,
                           fs=1000.0, duration=4, start=50, end=4000, n=4,
                           rng=np.random.default_rng(3), progress=lambda *a: calls.append(a),
                           settle=0)
    assert data["out"].shape == (4, 4000) and data["inp"].shape == (4, 4000)
    assert data["loop_gain"] == pytest.approx(0.4)
    assert L.loop.data[0, 0] == 0                       # left open
    assert not L.dist.data.any()                        # disturbance removed
    assert {c[0] for c in calls} == {"CL", "OL"}
    # the disturbance is white: closed, it is rejected at low frequency (and amplified near
    # Nyquist by the delayed integrator, so the rms is not a test)
    f, db = etf.etf_from_data(data, 50, 4000, 4)
    assert db[0] < -10 and db.max() > 0


def test_measure_etf_zeroes_the_channel_on_an_error():
    L = Loop()

    class Boom(Exception):
        pass

    def progress(label, i, n):
        if label == "CL" and i > 100:
            raise Boom

    with pytest.raises(Boom):
        etf.measure_etf(L.loop, L.gain, L.modes, L.dist, L.m2a, 1, 0.1, 1000.0, 2,
                        end=2000, progress=progress, settle=0)
    assert not L.dist.data.any()


@pytest.mark.parametrize("kwargs, match", [
    ({"mode": 9}, "mode 9 outside"),
    ({"duration": 0.001}, "frames"),
    ({"start": 5000}, "start"),
])
def test_measure_etf_checks_before_touching_the_loop(kwargs, match):
    L = Loop()
    args = dict(mode=1, amplitude=0.1, fs=1000.0, duration=2, end=2000, settle=0)
    args.update(kwargs)
    with pytest.raises(ValueError, match=match):
        etf.measure_etf(L.loop, L.gain, L.modes, L.dist, L.m2a, **args)
    assert not L.loop.writes and not L.dist.writes


# ------------------------------------------------------------------------------ window
@pytest.fixture(scope="module")
def disp():
    pytest.importorskip("PyQt5")
    pytest.importorskip("pyqtgraph")
    from PyQt5.QtCore import QCoreApplication, QSettings
    from PyQt5.QtWidgets import QApplication
    QCoreApplication.setOrganizationName("dao-test")
    spec = importlib.util.spec_from_file_location("daoEtfDisp", ROOT / "gui" / "daoEtfDisp.py")
    module = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {"dao": SimpleNamespace()}):
        spec.loader.exec_module(module)
    app = QApplication.instance() or QApplication([])
    return module, app


def test_window_shm_fields_and_command(disp, tmp_path):
    module, _ = disp
    with patch.object(module, "QSettings", lambda *a: _MemorySettings()):
        w = module.EtfDisp({"modes": "/tmp/wfsModes.im.shm", "dist": "/tmp/dmCmd05.im.shm",
                            "telemetry": str(tmp_path), "tau": 100.0, "mode": 5})
    assert w.shm("modes") == "/tmp/wfsModes.im.shm"
    assert w.shm("loop") == "/tmp/lpCmd.im.shm"          # default kept
    assert w.tau.value() == pytest.approx(100.0)
    cmd = w.acquisition_command()
    assert cmd[1].endswith("daoEtfMeasure.py")
    assert cmd[2:4] == ["5", str(w.ampSpin.value())]
    assert "--fs" in cmd and "--fs-shm" not in cmd       # no frame-rate SHM: the model's
    assert cmd[cmd.index("--modes") + 1] == "/tmp/wfsModes.im.shm"
    assert cmd[cmd.index("--telemetry") + 1] == str(tmp_path)
    w.fpsShmEdit.setText("/tmp/fps.im.shm")
    cmd = w.acquisition_command()
    assert cmd[cmd.index("--fs-shm") + 1] == "/tmp/fps.im.shm"


def test_window_loads_and_fits(disp, tmp_path):
    module, _ = disp
    path = etf.save_etf_data({"data_rtf": _dataset()}, str(tmp_path))
    with patch.object(module, "QSettings", lambda *a: _MemorySettings()):
        w = module.EtfDisp({"telemetry": str(tmp_path)})
    w.load_last()
    assert w.data_path == path
    assert w.gain.value() == pytest.approx(0.3) and w.fs.value() == pytest.approx(1000.0)
    assert w.etf_exp is not None and "measured" in w.bandwidthLabel.text()
    assert "rms" in w.errorLabel.text()


class _MemorySettings:
    def __init__(self):
        self.d = {}

    def value(self, key, default=None):
        return self.d.get(key, default)

    def setValue(self, key, value):
        self.d[key] = value
