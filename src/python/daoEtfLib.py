"""Error transfer function (ETF) of an AO loop: analytical model, measurement and files.

No GUI: used by daoEtfDisp.py (the ETF window) and daoEtfMeasure.py (the acquisition).

The measurement puts a random disturbance on one mode through a DM channel (the mode's
column of the M2A, times a uniform random amplitude each frame), records the modes measured
by the WFS with the loop closed, then the same disturbance with the loop open, and takes

    ETF = CSD(closed, open) / PSD(open)

The model is the classical integrator loop with pure delays:

    H_OL = H_WFS H_cam H_RTC H_DAC H_DM g / (1 - exp(-s Ti)),   H_ER = 1 / (1 + H_OL)

where only the sum of the delays Ti/2 + Tau + Tdm + Tcam changes H_ER.
"""

import glob
import os
import pickle
import time

import numpy as np

FILE_FILTER = "ETF data (*.npy *.npz *.pkl *.pickle);;All Files (*)"
REQUIRED_KEYS = ("out", "inp", "fs", "mode")


def telemetry_dir():
    """$DAODATA/telemetry (or ~/.dao/telemetry without DAODATA)."""
    root = os.getenv("DAODATA") or os.path.expanduser("~/.dao")
    return os.path.join(root, "telemetry")


def pointer_file(folder=None):
    """The one-line file naming the last telemetry folder written: <folder>/tmp/current_telemetry_folder.txt."""
    return os.path.join(folder or telemetry_dir(), "tmp", "current_telemetry_folder.txt")


# ------------------------------------------------------------------------------ model
def etf_model(fp, loop_gain, Ti, Tau, Tdm, Tcam):
    """Analytical loop transfer functions at the frequencies ``fp`` [Hz].

    Ti: sampling period, Tau: RTC delay, Tdm: DM delay, Tcam: camera readout + transfer [s].
    Returns (H_ER, H_CL, H_OL, H_N).
    """
    S = 1j * 2.0 * np.pi * np.asarray(fp, dtype=float)
    H_WFS = np.exp(-Ti / 2 * S)
    H_RTC = np.exp(-Tau * S)
    H_DM = np.exp(-Tdm * S)
    H_READOUT = np.exp(-Tcam * S)
    H_DAC = (1.0 - np.exp(-S * Ti)) / (S * Ti)
    CC = loop_gain / (1 - np.exp(-S * Ti))
    H_OL = H_WFS * H_READOUT * H_RTC * H_DAC * H_DM * CC
    H_CL = H_OL / (1 + H_OL)
    H_ER = 1.0 / (1.0 + H_OL)
    H_N = H_CL / H_WFS
    return H_ER, H_CL, H_OL, H_N


def model_db(fp, loop_gain, Ti, Tau, Tdm, Tcam):
    """|H_ER| in dB."""
    return 20.0 * np.log10(np.abs(etf_model(fp, loop_gain, Ti, Tau, Tdm, Tcam)[0]))


def csd(x, y, fs, nperseg):
    """Welch cross spectral density of x and y, as scipy.signal.csd with its defaults (periodic
    Hann window, half overlap, mean removed per segment, one-sided, density), without scipy."""
    x, y = np.asarray(x, dtype=float), np.asarray(y, dtype=float)
    nperseg = min(int(nperseg), x.size)
    step = nperseg - nperseg // 2
    window = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(nperseg) / nperseg)
    starts = np.arange(0, x.size - nperseg + 1, step)
    idx = starts[:, None] + np.arange(nperseg)[None, :]
    xs, ys = x[idx], y[idx]
    xs = (xs - xs.mean(axis=1, keepdims=True)) * window
    ys = (ys - ys.mean(axis=1, keepdims=True)) * window
    p = np.conj(np.fft.rfft(xs, axis=1)) * np.fft.rfft(ys, axis=1)
    p = p.mean(axis=0) / (fs * np.sum(window ** 2))
    p[1:-1 if nperseg % 2 == 0 else None] *= 2.0       # one-sided: all but DC (and Nyquist)
    return np.fft.rfftfreq(nperseg, 1.0 / fs), p


def measured_etf(out, inp, fs, nperseg):
    """Experimental ETF from the closed-loop (``out``) and open-loop (``inp``) series of a mode.

    Returns (f, ETF [dB], open-loop PSD [dB]).
    """
    f, Poi = csd(out, inp, fs, nperseg)
    _, Pii = csd(inp, inp, fs, nperseg)
    return f, 20.0 * np.log10(np.abs(Poi / Pii)), 20.0 * np.log10(np.abs(Pii))


def first_crossing(freq, db):
    """First frequency where the ETF reaches 0 dB (the rejection bandwidth), NaN if none."""
    above = np.nonzero(np.asarray(db) >= 0)[0]
    return freq[above[0]] if above.size else np.nan


def mode_series(data, key):
    """The measured mode's series of ``data[key]`` (all modes x frames, or one series)."""
    series = np.asarray(data[key], dtype=float)
    return series[int(data["mode"])] if series.ndim == 2 else series.ravel()


def etf_from_data(data, start, end, n):
    """(f, ETF dB) of a dataset between the samples start and end, nperseg = n x 64, DC removed."""
    out, inp = mode_series(data, "out"), mode_series(data, "inp")
    end = min(end, out.size)
    if start >= end - 1:
        raise ValueError(f"start ({start}) must be smaller than end ({end})")
    f, etf, _ = measured_etf(out[start:end], inp[start:end], float(data["fs"]), min(n * 64, end - start))
    return f[1:], etf[1:]


# ------------------------------------------------------------------------------ files
def load_etf_data(path):
    """A saved ETF dataset (the data_rtf dict) from .npy/.npz/.pkl."""
    if path.endswith(".npz"):
        with np.load(path, allow_pickle=True) as npz:
            data = {key: npz[key] for key in npz.files}
    elif path.endswith((".pkl", ".pickle")):
        with open(path, "rb") as handle:
            data = pickle.load(handle)
    else:
        data = np.load(path, allow_pickle=True)
        if isinstance(data, np.ndarray) and data.dtype == object and data.shape == ():
            data = data.item()
    if isinstance(data, dict) and "data_rtf" in data:
        data = data["data_rtf"]
    if isinstance(data, np.ndarray) and data.dtype == object and data.shape == ():
        data = data.item()
    if not isinstance(data, dict):
        raise ValueError("file does not contain an ETF dictionary")
    data = {k: (v.item() if isinstance(v, np.ndarray) and v.shape == () else v) for k, v in data.items()}
    missing = [k for k in REQUIRED_KEYS if k not in data]
    if missing:
        raise ValueError(f"missing keys: {', '.join(missing)}")
    return data


def find_last_etf_file(folder=None):
    """The newest ETF file in the folder named by <folder>/tmp/current_telemetry_folder.txt."""
    pointer = pointer_file(folder)
    try:
        with open(pointer) as handle:
            last = handle.read().strip()
    except OSError:
        last = ""
    if not last:
        raise FileNotFoundError(f"no folder in {pointer}")
    if os.path.isfile(last):
        return last
    candidates = [p for ext in ("npy", "npz", "pkl", "pickle")
                  for p in glob.glob(os.path.join(last, "**", f"*.{ext}"), recursive=True)]
    rtf = [p for p in candidates if "data_rtf" in os.path.basename(p)]
    candidates = rtf or candidates
    if not candidates:
        raise FileNotFoundError(f"no ETF file in {last}")
    return max(candidates, key=os.path.getmtime)


def save_etf_data(data, folder=None, tracking_number=None):
    """Save ``data`` as <folder>/<YYYY_MM_DD>/<tn>/<tn>_data_rtf.npy (tn = YYYYmmdd_HHMMSS) and
    point <folder>/tmp/current_telemetry_folder.txt at it. Returns the file's path."""
    folder = folder or telemetry_dir()
    tracking_number = tracking_number or time.strftime("%Y%m%d_%H%M%S")
    save_loc = os.path.join(folder, time.strftime("%Y_%m_%d"), tracking_number)
    os.makedirs(save_loc, exist_ok=True)
    path = os.path.join(save_loc, tracking_number + "_data_rtf.npy")
    np.save(path, data, allow_pickle=True)
    pointer = pointer_file(folder)
    os.makedirs(os.path.dirname(pointer), exist_ok=True)
    with open(pointer, "w") as handle:
        handle.write(save_loc)
    return path


# ------------------------------------------------------------------------------ measurement
def _first(shm):
    return np.asarray(shm.get_data()).flat[0]


def _fill(shm, value):
    """Write the scalar ``value`` everywhere in ``shm``, keeping its shape and type."""
    d = np.asarray(shm.get_data())
    shm.set_data(np.full(d.shape, value, dtype=d.dtype))


def disturbance(m2a, mode, amplitude, n_frames, rng=None):
    """Actuator commands (n_act x n_frames): the mode's M2A column times amplitude x U(-1, 1)."""
    rng = rng or np.random.default_rng()
    column = np.asarray(m2a, dtype=float)[:, mode]
    return (amplitude * column[:, None] * (1.0 - 2.0 * rng.random(n_frames))[None, :]).astype(np.float32)


def _record(modes, dist, commands, label, progress):
    n_frames = commands.shape[1]
    shape, dtype = np.asarray(dist.get_data()).shape, np.asarray(dist.get_data()).dtype
    cube = np.zeros((np.asarray(modes.get_data()).size, n_frames))
    modes.get_data(check=True)                     # in step with the WFS
    step = max(1, n_frames // 20)
    for i in range(n_frames):
        dist.set_data(commands[:, i].reshape(shape).astype(dtype))
        cube[:, i] = np.asarray(modes.get_data(check=True)).ravel()
        if progress and i % step == 0:
            progress(label, i, n_frames)
    if progress:
        progress(label, n_frames, n_frames)
    return cube


def measure_etf(loop, gain, modes, dist, m2a, mode, amplitude, fs, duration,
                start=50, end=15000, n=6, rng=None, progress=None, settle=0.1):
    """Acquire an ETF on one mode: the disturbance with the loop closed, then open.

    loop, gain, modes, dist, m2a: open SHMs (dao.shm, or anything with get_data / set_data):
    the loop switch (0 open, 1 closed), the integrator gain, the modes measured by the WFS,
    the DM channel taking the disturbance (n_act values) and the modes-to-actuators matrix
    (n_act x n_modes). The disturbance channel is zeroed at the end, even on an error, and the
    loop is left OPEN. progress(label, i, n) is called about 20 times per phase.
    Returns the data_rtf dict (see save_etf_data).
    """
    m2a_data = np.asarray(m2a.get_data())
    n_modes = np.asarray(modes.get_data()).size
    n_act = np.asarray(dist.get_data()).size
    if not 0 <= mode < min(n_modes, m2a_data.shape[1]):
        raise ValueError(f"mode {mode} outside 0..{min(n_modes, m2a_data.shape[1]) - 1}")
    if m2a_data.shape[0] != n_act:
        raise ValueError(f"the M2A has {m2a_data.shape[0]} rows, the disturbance channel {n_act} values")
    n_frames = int(fs * duration)
    if n_frames < 2:
        raise ValueError(f"{n_frames} frames: check the frame rate ({fs} Hz) and the duration ({duration} s)")
    end = min(end, n_frames)
    if start >= end - 1:
        raise ValueError(f"start ({start}) must be smaller than end ({end}, at most {n_frames} frames)")
    loop_gain = float(_first(gain))
    commands = disturbance(m2a_data, mode, amplitude, n_frames, rng)
    try:
        _fill(loop, 1)
        time.sleep(settle)
        out = _record(modes, dist, commands, "CL", progress)
        _fill(dist, 0)
        _fill(loop, 0)
        time.sleep(settle)
        inp = _record(modes, dist, commands, "OL", progress)
    finally:
        _fill(dist, 0)
    f, ftr, _ = measured_etf(out[mode, start:end], inp[mode, start:end], fs, min(n * 64, end - start))
    return {
        "out": out, "inp": inp, "fs": fs, "mode": mode, "amp_mode": amplitude,
        "random+cmd": commands, "N": n, "end": end, "start": start, "mod_test": [mode],
        "loop_gain": loop_gain, "FTR": ftr, "frequency_vector": f,
    }
