#!/usr/bin/env python3
"""
ETF Display - measured error transfer function of an AO loop vs the analytical model

Load a saved ETF measurement (or acquire one with daoEtfMeasure.py), then tune the loop
gain, the frame rate and the delays by hand: the model curve is redrawn as they move.
Shows the latency, the 0 dB crossings (rejection bandwidth) and the model - measured rms.

The acquisition runs daoEtfMeasure.py on the SHMs of the "Shared memories" group: a random
disturbance on one mode through a DM channel, loop closed then open. The loop is left OPEN
at the end; Stop zeroes the disturbance channel.

The SHM names and the telemetry folder are remembered between sessions.

Usage: daoEtfDisp.py [options] [file]

Options:
  --loop  PATH   loop switch SHM, 0 open / 1 closed (default: /tmp/lpCmd.im.shm)
  --gain  PATH   loop gain SHM, read only (default: /tmp/lpGain.im.shm)
  --modes PATH   modes measured by the WFS (default: /tmp/modes.im.shm)
  --dist  PATH   DM channel taking the disturbance (default: /tmp/dmCmd03.im.shm)
  --m2a   PATH   modes-to-actuators matrix, n_act x n_modes (default: /tmp/dmM2A.im.shm)
  --fps   PATH   frame rate SHM [Hz] (default: none, the model's frame rate is used)
  -d PATH        telemetry folder (default: $DAODATA/telemetry)
  --tau  US      model RTC delay [us]       (default: 36)
  --tdm  US      model DM delay [us]        (default: 375)
  --tcam US      model camera delay [us]    (default: 572)
  --mode N       acquisition mode           --amp A   acquisition amplitude
  --light        light mode (default: dark)

  file           an ETF file to load (.npy/.npz/.pkl with the data_rtf dict)

Options given override the remembered settings.

Examples:
  daoEtfDisp.py
  daoEtfDisp.py --loop /tmp/aoLpCmd.im.shm --gain /tmp/aoLpGain.im.shm \\
      --modes /tmp/wfsModes.im.shm --dist /tmp/dmCmd05.im.shm --m2a /tmp/dmM2C.im.shm \\
      --fps /tmp/camFps.im.shm
"""

import argparse
import os
import pickle
import re
import shutil
import sys

import numpy as np
from PyQt5 import QtCore
from PyQt5.QtCore import QProcess, QSettings, pyqtSignal
from PyQt5.QtGui import QIcon
from PyQt5.QtWidgets import QApplication, QFileDialog, QVBoxLayout
from PyQt5.uic import loadUiType
import pyqtgraph as pg

import daoEtfLib as etf

_UI_DIR = os.getenv("DAOROOT", ".") + "/data/"
_UI_FILE = os.path.join(_UI_DIR, "daoEtfDisp.ui")
if not os.path.isfile(_UI_FILE):        # run from the source tree
    _UI_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "daoEtfDisp.ui")
Ui_MainWindow, QMainWindow = loadUiType(_UI_FILE)

# the remembered fields: key -> (line edit, default)
SHM_FIELDS = {
    "loop":  ("loopShmEdit", "/tmp/lpCmd.im.shm"),
    "gain":  ("gainShmEdit", "/tmp/lpGain.im.shm"),
    "modes": ("modesShmEdit", "/tmp/modes.im.shm"),
    "dist":  ("distShmEdit", "/tmp/dmCmd03.im.shm"),
    "m2a":   ("m2aShmEdit", "/tmp/dmM2A.im.shm"),
    "fps":   ("fpsShmEdit", ""),
    "telemetry": ("telemetryEdit", etf.telemetry_dir()),
}
SAVED = re.compile(r"Saving in (.+)")


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color: #f0f0f0; color: #000000; }
            QGroupBox { border: 1px solid #aaa; margin-top: 8px; padding-top: 4px; }
            QGroupBox::title { subcontrol-origin: margin; left: 6px; }
            QLineEdit, QSpinBox, QDoubleSpinBox {
                background-color: #ffffff; color: #000000; border: 1px solid #aaa; }
            QPushButton {
                background-color: #e0e0e0; color: #000000;
                border: 1px solid #aaa; padding: 3px 6px; border-radius: 3px;
            }
            QPushButton:hover   { background-color: #d0d0d0; }
            QPushButton:pressed { background-color: #bbb; }
            QPushButton:disabled { color: #999; }
        """
    return """
        QMainWindow, QWidget { background-color: #1e1e1e; color: #cccccc; }
        QGroupBox { border: 1px solid #555; margin-top: 8px; padding-top: 4px; }
        QGroupBox::title { subcontrol-origin: margin; left: 6px; }
        QLineEdit, QSpinBox, QDoubleSpinBox {
            background-color: #2a2a2a; color: #cccccc; border: 1px solid #555; }
        QPushButton {
            background-color: #3a3a3a; color: #cccccc;
            border: 1px solid #555; padding: 3px 6px; border-radius: 3px;
        }
        QPushButton:hover   { background-color: #4a4a4a; }
        QPushButton:pressed { background-color: #2a2a2a; }
        QPushButton:disabled { color: #666; }
    """


def find_measure_app():
    """daoEtfMeasure.py: on the PATH, else in $DAOROOT/bin, else in the source tree's apps/."""
    app = shutil.which("daoEtfMeasure.py")
    if app is None and os.getenv("DAOROOT"):
        app = os.path.join(os.getenv("DAOROOT"), "bin", "daoEtfMeasure.py")
    if app is None or not os.path.isfile(app):
        here = os.path.dirname(os.path.abspath(__file__))
        app = os.path.join(here, "..", "apps", "daoEtfMeasure.py")
    return app


# ------------------------------------------------------------------------------ controls
class ParamControl(QtCore.QObject):
    """Ties a .ui slider to a .ui spin box: the spin box holds the value, its range and its
    step (set in the .ui); the slider spans the same range."""

    changed = pyqtSignal(float)
    STEPS = 1000

    def __init__(self, slider, spin):
        super().__init__(spin)
        self.slider, self.spin = slider, spin
        slider.setRange(0, self.STEPS)
        self._sync_slider(spin.value())
        slider.valueChanged.connect(self._from_slider)
        spin.valueChanged.connect(self._from_spin)

    def value(self):
        return self.spin.value()

    def set_value(self, value, emit=True):
        self.blockSignals(not emit)
        self.spin.setValue(value)
        self._sync_slider(self.spin.value())
        self.blockSignals(False)

    def _sync_slider(self, value):
        low, high = self.spin.minimum(), self.spin.maximum()
        pos = round((value - low) / (high - low) * self.STEPS) if high > low else 0
        self.slider.blockSignals(True)
        self.slider.setValue(int(pos))
        self.slider.blockSignals(False)

    def _from_slider(self, pos):
        low, high = self.spin.minimum(), self.spin.maximum()
        self.spin.setValue(low + (high - low) * pos / self.STEPS)

    def _from_spin(self, value):
        self._sync_slider(value)
        self.changed.emit(value)


# ------------------------------------------------------------------------------ window
class EtfDisp(QMainWindow, Ui_MainWindow):
    def __init__(self, overrides=None, light=False, parent=None):
        super().__init__(parent)
        self.parent = parent
        self.setupUi(self)
        self.setWindowFlags(QtCore.Qt.Window)
        self.settings = QSettings("dao", "daoEtfDisp")

        self.proc = None
        self.proc_out = ""
        self.stopped = False
        self.data = None
        self.data_path = ""
        self.freq = None       # experimental frequencies (DC removed)
        self.etf_exp = None    # experimental ETF [dB]

        overrides = dict(overrides or {})
        for key, (edit, default) in SHM_FIELDS.items():
            getattr(self, edit).setText(str(self.settings.value(key, default)))
        for key in SHM_FIELDS:
            if key in overrides:
                getattr(self, SHM_FIELDS[key][0]).setText(overrides.pop(key))

        # model parameters: slider + spin box pairs from the .ui
        self.gain = ParamControl(self.gainSlider, self.gainBox)
        self.fs = ParamControl(self.fsSlider, self.fsBox)
        self.tau = ParamControl(self.tauSlider, self.tauBox)
        self.tdm = ParamControl(self.tdmSlider, self.tdmBox)
        self.tcam = ParamControl(self.tcamSlider, self.tcamBox)
        for key, control in (("tau", self.tau), ("tdm", self.tdm), ("tcam", self.tcam)):
            if overrides.get(key) is not None:
                control.set_value(float(overrides[key]), emit=False)
        if overrides.get("mode") is not None:
            self.modeSpin.setValue(int(overrides["mode"]))
        if overrides.get("amp") is not None:
            self.ampSpin.setValue(float(overrides["amp"]))
        for control in (self.gain, self.fs, self.tau, self.tdm, self.tcam):
            control.changed.connect(self._update_model)
        # "Reset delays" goes back to the delays at start (options or .ui)
        self.default_delays = {c: c.value() for c in (self.tau, self.tdm, self.tcam)}

        # the plot, in the .ui placeholder
        self.plotWidget = pg.PlotWidget()
        layout = QVBoxLayout(self.plotView)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.plotWidget)
        self.plot = self.plotWidget.getPlotItem()
        self.plot.setLogMode(x=True, y=False)
        self.plot.showGrid(x=True, y=True, alpha=0.3)
        self.plot.setLabels(left="ETF [dB]", bottom="Frequency [Hz]")
        self.plot.addLegend()
        self.plot.addItem(pg.InfiniteLine(pos=0, angle=0, pen=pg.mkPen("#888888", width=1)))
        measured, model = ("#0072b2", "#d55e00") if light else ("c", "y")
        self.expCurve = self.plot.plot(pen=pg.mkPen(measured, width=2), name="Measured")
        self.modelCurve = self.plot.plot(pen=pg.mkPen(model, width=2), name="Model")

        # the experimental ETF recomputed once typing stops
        self.exp_timer = QtCore.QTimer(self)
        self.exp_timer.setSingleShot(True)
        self.exp_timer.setInterval(300)
        self.exp_timer.timeout.connect(self._recompute_experimental)
        for spin in (self.startSpin, self.endSpin, self.nSpin):
            spin.valueChanged.connect(self.exp_timer.start)

        self.loadButton.clicked.connect(self.load_file)
        self.loadLastButton.clicked.connect(self.load_last)
        self.acquireButton.clicked.connect(self.acquire)
        self.stopButton.clicked.connect(self.stop_acquisition)
        self.resetDelaysButton.clicked.connect(self.reset_delays)
        self.fromDataButton.clicked.connect(self.gain_rate_from_data)

        self._update_model()

    def shm(self, key):
        return getattr(self, SHM_FIELDS[key][0]).text().strip()

    def save_settings(self):
        for key in SHM_FIELDS:
            self.settings.setValue(key, self.shm(key))

    def set_acquisition(self, mode, amplitude):
        """Pre-fill the acquisition mode and amplitude (e.g. from another window)."""
        self.modeSpin.setValue(int(mode))
        self.ampSpin.setValue(float(amplitude))

    def _set_status(self, text):
        self.statusLabel.setText(text)

    # -------------------------------------------------------------------- loading
    def load_file(self):
        path, _ = QFileDialog.getOpenFileName(self, "Select ETF data", self.shm("telemetry"),
                                              etf.FILE_FILTER)
        if path:
            self.load(path)

    def load_last(self, *_):
        try:
            self.load(etf.find_last_etf_file(self.shm("telemetry")))
        except (OSError, ValueError) as error:
            self._set_status(f"Could not load the last ETF: {error}")

    def load(self, path):
        try:
            data = etf.load_etf_data(path)
        except (OSError, ValueError, pickle.UnpicklingError) as error:
            self._set_status(f"Could not load {os.path.basename(path)}: {error}")
            return
        self.data, self.data_path = data, path
        for spin, key in ((self.startSpin, "start"), (self.endSpin, "end"), (self.nSpin, "N")):
            if key in data:
                spin.blockSignals(True)
                spin.setValue(int(data[key]))
                spin.blockSignals(False)
        self.modeSpin.setValue(int(data["mode"]))
        if "amp_mode" in data:
            self.ampSpin.setValue(float(data["amp_mode"]))
        gain = data.get("loop_gain")
        self.dataLabel.setText(
            f"{os.path.basename(path)}\nmode {int(data['mode'])}, fs {float(data['fs']):.0f} Hz"
            + (f", gain {float(gain):.3f}" if gain is not None else ""))
        self.gain_rate_from_data()
        self._recompute_experimental()
        self.plot.enableAutoRange()
        self._set_status(f"Loaded {path}")

    def gain_rate_from_data(self, *_):
        """Set the model gain and frame rate to the measured ones."""
        if self.data is None:
            self._set_status("Load data first")
            return
        if self.data.get("loop_gain") is not None:
            self.gain.set_value(float(self.data["loop_gain"]), emit=False)
        self.fs.set_value(float(self.data["fs"]), emit=False)
        self._update_model()

    def reset_delays(self, *_):
        for control, value in self.default_delays.items():
            control.set_value(value, emit=False)
        self._update_model()

    # --------------------------------------------------------------- computations
    def _recompute_experimental(self):
        if self.data is None:
            return
        try:
            self.freq, self.etf_exp = etf.etf_from_data(
                self.data, self.startSpin.value(), self.endSpin.value(), self.nSpin.value())
        except ValueError as error:
            self._set_status(str(error))
            return
        self.expCurve.setData(self.freq, self.etf_exp)
        self._update_model()

    def _model_parameters(self):
        """(gain, Ti, Tau, Tdm, Tcam) in SI units."""
        return (self.gain.value(), 1.0 / self.fs.value(), self.tau.value() * 1e-6,
                self.tdm.value() * 1e-6, self.tcam.value() * 1e-6)

    def _update_model(self, *_):
        gain, Ti, Tau, Tdm, Tcam = self._model_parameters()
        freq = self.freq if self.freq is not None else np.logspace(0, np.log10(0.5 / Ti), 500)
        model_db = etf.model_db(freq, gain, Ti, Tau, Tdm, Tcam)
        self.modelCurve.setData(freq, model_db)

        latency = Ti + Tau + Tdm + Tcam
        self.latencyLabel.setText(
            f"Latency {latency * 1e3:.3f} ms  (Ti {Ti * 1e3:.3f} + Tau {Tau * 1e3:.3f}"
            f" + Tdm {Tdm * 1e3:.3f} + Tcam {Tcam * 1e3:.3f})")
        text = f"0 dB crossing: model {etf.first_crossing(freq, model_db):.1f} Hz"
        if self.etf_exp is not None:
            text += f", measured {etf.first_crossing(self.freq, self.etf_exp):.1f} Hz"
            diff = model_db - self.etf_exp
            self.errorLabel.setText(f"Model - measured: rms {np.sqrt(np.mean(diff ** 2)):.2f} dB")
        self.bandwidthLabel.setText(text)
        self.plot.setTitle(f"Gain {gain:.3f} | latency {latency * 1e3:.3f} ms")

    # ---------------------------------------------------------------- acquisition
    def acquisition_command(self):
        """[python, daoEtfMeasure.py, args...] from the window's fields."""
        args = [str(self.modeSpin.value()), str(self.ampSpin.value())]
        if self.shm("fps"):
            args += ["--fs-shm", self.shm("fps")]
        else:
            args += ["--fs", str(self.fs.value())]
        args += ["-t", str(self.durationSpin.value()),
                 "--start", str(self.startSpin.value()), "--end", str(self.endSpin.value()),
                 "-N", str(self.nSpin.value()), "--no-plot", "--telemetry", self.shm("telemetry")]
        for key in ("loop", "gain", "modes", "dist", "m2a"):
            args += [f"--{key}", self.shm(key)]
        return [sys.executable, find_measure_app()] + args

    def acquire(self, *_):
        if self.proc is not None:
            return
        missing = [k for k in ("loop", "gain", "modes", "dist", "m2a", "telemetry") if not self.shm(k)]
        if missing:
            self._set_status("Give the " + ", ".join(missing) + " first")
            return
        self.save_settings()
        command = self.acquisition_command()
        self.proc_out, self.stopped = "", False
        self.proc = QProcess(self)
        self.proc.setProcessChannelMode(QProcess.MergedChannels)
        self.proc.readyReadStandardOutput.connect(self._read_output)
        self.proc.finished.connect(self._finished)
        self.proc.start(command[0], command[1:])
        self.acquireButton.setEnabled(False)
        self.stopButton.setEnabled(True)
        rate = f"frame rate from {self.shm('fps')}" if self.shm("fps") else f"{self.fs.value():.0f} Hz"
        self._set_status(f"Acquiring mode {self.modeSpin.value()} ({rate}): "
                         f"about {2 * self.durationSpin.value()} s, closed then open loop")

    def _read_output(self):
        text = bytes(self.proc.readAllStandardOutput()).decode(errors="replace")
        self.proc_out += text
        sys.stdout.write(text)
        sys.stdout.flush()
        lines = [line for line in re.split(r"[\r\n]", text) if line.strip()]
        if lines and re.match(r"(CL|OL) ", lines[-1]):
            self._set_status(("Closed" if lines[-1].startswith("CL") else "Open")
                             + " loop: " + lines[-1][3:].strip())

    def _finished(self, code, _status):
        self.proc = None
        self.acquireButton.setEnabled(True)
        self.stopButton.setEnabled(False)
        if self.stopped:
            return
        if code != 0:
            lines = [line for line in self.proc_out.strip().splitlines() if line.strip()]
            self._set_status("Acquisition failed: " + (lines[-1] if lines else f"exit code {code}"))
            return
        saved = SAVED.search(self.proc_out)
        if saved:
            self.load(saved.group(1).strip())
        else:
            self.load_last()

    def stop_acquisition(self, *_):
        if self.proc is None:
            return
        self.stopped = True
        self.proc.terminate()                    # daoEtfMeasure.py zeroes the disturbance on SIGTERM
        if not self.proc.waitForFinished(3000):
            self.proc.kill()
            self.proc.waitForFinished(1000)
        try:                                     # and again, in case it was killed
            import dao
            shm = dao.shm(self.shm("dist"))
            d = np.asarray(shm.get_data())
            shm.set_data(np.zeros(d.shape, dtype=d.dtype))
        except Exception as error:
            print(f"Could not reset {self.shm('dist')}: {error}", file=sys.stderr)
        self._set_status("Acquisition stopped (disturbance removed; check the loop state)")

    def closeEvent(self, event):
        self.save_settings()
        self.stop_acquisition()
        if self.parent and getattr(self.parent, "etfWindow", None) is self:
            self.parent.etfWindow = None
        super().closeEvent(event)


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="ETF display: measured vs model",
                                usage="daoEtfDisp.py [options] [file]  (see the header of daoEtfDisp.py)")
    for key in ("loop", "gain", "modes", "dist", "m2a", "fps"):
        p.add_argument(f"--{key}")
    p.add_argument("-d", "--telemetry")
    for key in ("tau", "tdm", "tcam", "amp"):
        p.add_argument(f"--{key}", type=float)
    p.add_argument("--mode", type=int)
    p.add_argument("--light", action="store_true")
    p.add_argument("file", nargs="?")
    return p.parse_args(argv)


def main():
    if any(a in ("-h", "--help") for a in sys.argv[1:]):
        print(__doc__)
        sys.exit(0)
    args = parse_args()
    overrides = {k: v for k, v in vars(args).items() if v is not None and k not in ("light", "file")}
    app = QApplication(sys.argv)
    app.setApplicationDisplayName("ETF")
    # dao logo in the window's title bar
    logo = os.path.join(os.getenv("DAOROOT", ""), "data", "daoLogo.png")
    if os.path.exists(logo):
        app.setWindowIcon(QIcon(logo))
    app.setStyleSheet(make_stylesheet(args.light))
    pg.setConfigOption("background", "#f0f0f0" if args.light else "#1e1e1e")
    pg.setConfigOption("foreground", "#000000" if args.light else "#cccccc")
    window = EtfDisp(overrides, args.light)
    window.show()
    if args.file:
        window.load(args.file)
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
