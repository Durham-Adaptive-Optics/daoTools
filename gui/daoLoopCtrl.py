#!/usr/bin/env python3
"""
Loop Control - control panel for a classical (leaky) integrator loop.

Drives the three scalar shared memories the integrator watches:

  lpCmd   uint32   0 = open loop, 1 = closed loop   (default /tmp/lpCmd.im.shm)
  lpGain  float32  integrator gain, [0 .. 2]        (default /tmp/lpGain.im.shm)
  lpLeak  float32  leak factor,     [0 .. 1]        (default /tmp/lpLeak.im.shm)

Opening the GUI does not write anything - it shows the current values. A shm
that does not exist is created (open loop, gain 0, leak 1). Values are polled
so external changes (setLoopGain.py, another GUI, ...) are reflected live.

Usage: daoLoopCtrl.py [options]

Options:
  --cmd  PATH   lpCmd  shm (default: /tmp/lpCmd.im.shm)
  --gain PATH   lpGain shm (default: /tmp/lpGain.im.shm)
  --leak PATH   lpLeak shm (default: /tmp/lpLeak.im.shm)
  --gain-max V  gain slider/spin upper bound (default: 2.0)
  --light       light mode (default: dark)

Examples:
  daoLoopCtrl.py
  daoLoopCtrl.py --cmd /tmp/lpCmdpo4ao.im.shm --gain-max 1.0
"""

import argparse
import os
import sys

import numpy as np
from PyQt5.uic import loadUiType
from PyQt5.QtCore import QTimer
from PyQt5.QtWidgets import QApplication, QMessageBox

import dao

_UI_DIR = os.getenv("DAOROOT", ".") + "/data/"
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(_UI_DIR, "daoLoopCtrl.ui"))

GAIN_STEPS = 1000   # slider units per 1.0 of gain
LEAK_STEPS = 1000   # slider units per 1.0 of leak


def make_stylesheet(light):
    base = """
        QPushButton { border:1px solid %(bd)s; padding:4px 8px; border-radius:3px;
                      background:%(btn)s; color:%(fg)s; }
        QPushButton:hover { background:%(btnh)s; }
        QLabel { color:%(fg)s; }
        QSlider::groove:horizontal { background:%(grv)s; height:6px; border-radius:3px; }
        QSlider::handle:horizontal { background:#4a9eff; width:16px; margin:-6px 0;
                                     border-radius:8px; }
        QSlider::sub-page:horizontal { background:#4a9eff; border-radius:3px; }
        QDoubleSpinBox { background:%(fld)s; color:%(fg)s; border:1px solid %(bd)s; }
        QPushButton#loopButton:!checked { background:#8a6d1f; color:#fff; border-color:#b8902a; }
        QPushButton#loopButton:checked  { background:#1e7a3c; color:#fff; border-color:#2a9a4c; }
        QPushButton#loopButton:hover { border-color:#4a9eff; }
    """
    if light:
        return base % dict(bg="#f0f0f0", fg="#000", btn="#e0e0e0", btnh="#d0d0d0",
                           bd="#aaa", grv="#c8c8c8", fld="#ffffff")
    return ("QMainWindow, QWidget { background:#1e1e1e; }" + base) % dict(
        bg="#1e1e1e", fg="#cccccc", btn="#3a3a3a", btnh="#4a4a4a",
        bd="#555", grv="#3a3a3a", fld="#2a2a2a")


def open_scalar_shm(path, dtype, default):
    """Attach the shm at `path`, or create it (shape (1,1), `dtype`, `default`)."""
    if os.path.exists(path):
        return dao.shm(path)
    return dao.shm(path, np.full((1, 1), default, dtype=dtype))


def scalar(shm):
    return float(np.asarray(shm.get_data()).ravel()[0])


def write_scalar(shm, value):
    """Write `value` keeping the shm's existing dtype/shape."""
    shm.set_data(shm.get_data() * 0 + value)


class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, cmd_path, gain_path, leak_path, gain_max=2.0, light=False):
        super().__init__()
        self.setupUi(self)
        self.setStyleSheet(make_stylesheet(light))
        self.gain_max = float(gain_max)

        self.cmd_shm = open_scalar_shm(cmd_path, np.uint32, 0)
        self.gain_shm = open_scalar_shm(gain_path, np.float32, 0.0)
        self.leak_shm = open_scalar_shm(leak_path, np.float32, 1.0)
        self.paths = (cmd_path, gain_path, leak_path)

        self.gainSlider.setMaximum(int(round(self.gain_max * GAIN_STEPS)))
        self.gainSpin.setMaximum(self.gain_max)
        self.gainLabel.setText(f"Gain [0 - {self.gain_max:g}]")

        # -- wire up
        self.loopButton.toggled.connect(self._toggle_loop)
        self.gainSlider.valueChanged.connect(
            lambda v: self._set_gain(v / GAIN_STEPS, src="slider"))
        self.gainSpin.valueChanged.connect(
            lambda v: self._set_gain(v, src="spin"))
        self.leakSlider.valueChanged.connect(
            lambda v: self._set_leak(v / LEAK_STEPS, src="slider"))
        self.leakSpin.valueChanged.connect(
            lambda v: self._set_leak(v, src="spin"))
        self.zeroGainButton.clicked.connect(lambda: self._set_gain(0.0))
        self.fullLeakButton.clicked.connect(lambda: self._set_leak(1.0))

        self._pull()   # populate widgets from the shms without writing

        self.timer = QTimer(self)
        self.timer.setInterval(300)
        self.timer.timeout.connect(self._pull)
        self.timer.start()

    # ------------------------------------------------------------------
    def _closed(self):
        return scalar(self.cmd_shm) >= 0.5

    def _toggle_loop(self, checked):
        write_scalar(self.cmd_shm, 1 if checked else 0)
        self._refresh_loop_label(checked)

    def _refresh_loop_label(self, closed):
        self.loopButton.setText("CLOSED LOOP  (click to open)" if closed
                                else "OPEN LOOP  (click to close)")
        self.statusLabel.setText(
            f"loop {'CLOSED' if closed else 'OPEN'}   "
            f"gain={scalar(self.gain_shm):.3f}   leak={scalar(self.leak_shm):.3f}")

    def _set_gain(self, value, src=None):
        value = float(np.clip(value, 0.0, self.gain_max))
        write_scalar(self.gain_shm, value)
        self._sync_pair(self.gainSlider, self.gainSpin, value, GAIN_STEPS, src)
        self.gainNowLabel.setText(f"now: {scalar(self.gain_shm):.3f}")
        self._refresh_loop_label(self._closed())

    def _set_leak(self, value, src=None):
        value = float(np.clip(value, 0.0, 1.0))
        write_scalar(self.leak_shm, value)
        self._sync_pair(self.leakSlider, self.leakSpin, value, LEAK_STEPS, src)
        self.leakNowLabel.setText(f"now: {scalar(self.leak_shm):.3f}")
        self._refresh_loop_label(self._closed())

    @staticmethod
    def _sync_pair(slider, spin, value, steps, src):
        if src != "slider":
            slider.blockSignals(True)
            slider.setValue(int(round(value * steps)))
            slider.blockSignals(False)
        if src != "spin":
            spin.blockSignals(True)
            spin.setValue(value)
            spin.blockSignals(False)

    # ------------------------------------------------------------------
    def _pull(self):
        """Refresh widgets from the shms (external changes), without writing.

        Widgets the user is actively touching (slider held down, spin box
        focused) are left alone so the poll never fights the input."""
        try:
            gain = scalar(self.gain_shm)
            leak = scalar(self.leak_shm)
            closed = self._closed()
        except Exception:   # noqa: BLE001 - shm briefly unavailable
            return

        def push(widget, setter):
            busy = (widget.isSliderDown() if hasattr(widget, "isSliderDown")
                    else widget.hasFocus())
            if busy:
                return
            widget.blockSignals(True)
            setter()
            widget.blockSignals(False)

        push(self.loopButton, lambda: self.loopButton.setChecked(closed))
        push(self.gainSlider, lambda: self.gainSlider.setValue(int(round(gain * GAIN_STEPS))))
        push(self.gainSpin, lambda: self.gainSpin.setValue(min(gain, self.gain_max)))
        push(self.leakSlider, lambda: self.leakSlider.setValue(int(round(leak * LEAK_STEPS))))
        push(self.leakSpin, lambda: self.leakSpin.setValue(min(leak, 1.0)))

        self.gainNowLabel.setText(f"now: {gain:.3f}")
        self.leakNowLabel.setText(f"now: {leak:.3f}")
        self._refresh_loop_label(closed)


def parse_args():
    p = argparse.ArgumentParser(description="classical integrator loop control panel")
    p.add_argument("--cmd", default="/tmp/lpCmd.im.shm", help="lpCmd shm (uint32 0/1)")
    p.add_argument("--gain", default="/tmp/lpGain.im.shm", help="lpGain shm (float32)")
    p.add_argument("--leak", default="/tmp/lpLeak.im.shm", help="lpLeak shm (float32)")
    p.add_argument("--gain-max", type=float, default=2.0, help="gain upper bound")
    p.add_argument("--light", action="store_true", help="light mode")
    return p.parse_args()


def main():
    args = parse_args()
    app = QApplication(sys.argv)
    try:
        gui = Main(args.cmd, args.gain, args.leak,
                   gain_max=args.gain_max, light=args.light)
    except Exception as exc:   # noqa: BLE001
        print(f"daoLoopCtrl: {exc}", file=sys.stderr)
        QMessageBox.critical(None, "daoLoopCtrl", str(exc))
        return 1
    gui.setWindowTitle("daoLoopCtrl  ["
                       + os.path.basename(args.cmd).split(".")[0] + "]")
    gui.show()
    return app.exec_()


if __name__ == "__main__":
    sys.exit(main())
