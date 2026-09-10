#!/usr/bin/env python3
"""
Bar Display - real-time bar chart of a 1D shared-memory vector.

One bar per value in the shm. The X extent defaults to the whole vector; the
"Index from / to" spin boxes restrict it to any sub-range. "Y autoscale"
fits the Y axis to the visible bars each frame; unchecked, the Y min / Y max
spin boxes pin a fixed scale.

Usage: daoBarDisp.py <shmName> [options]

Options:
  --from N     first index to show   (default: 0)
  --to N       last index to show, inclusive (default: last)
  --fps HZ     update rate           (default: 15)
  --light      light mode            (default: dark)

Examples:
  daoBarDisp.py /tmp/dmCmd00.im.shm
  daoBarDisp.py /tmp/shCentroids.im.shm --from 0 --to 199
  daoBarDisp.py /tmp/modes.im.shm --fps 30 --light
"""

import os

# Bind pyqtgraph to PyQt5 -- PyQt6 is also installed and pyqtgraph would pick
# it by default, which then clashes with the PyQt5 widgets uic builds.
os.environ.setdefault("PYQTGRAPH_QT_LIB", "PyQt5")

import argparse
import sys
import time

from PyQt5.uic import loadUiType
from PyQt5.QtCore import QTimer
from PyQt5.QtWidgets import QApplication, QMessageBox

import numpy as np
import pyqtgraph as pg
import dao

_UI_DIR = os.getenv("DAOROOT", ".") + "/data/"
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(_UI_DIR, "daoBarDisp.ui"))

BAR_BRUSH = "#4a9eff"


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color:#f0f0f0; color:#000; }
            QLabel, QCheckBox { color:#000; }
            QPushButton { background:#e0e0e0; color:#000; border:1px solid #aaa;
                          padding:3px 8px; border-radius:3px; }
            QPushButton:hover { background:#d0d0d0; }
            QPushButton:checked { background:#4a90d9; color:#fff; border-color:#2a70b9; }
            QSpinBox, QDoubleSpinBox { background:#fff; color:#000; border:1px solid #aaa; }
        """
    return """
        QMainWindow, QWidget { background-color:#1e1e1e; color:#ccc; }
        QLabel, QCheckBox { color:#ccc; }
        QPushButton { background:#3a3a3a; color:#ccc; border:1px solid #555;
                      padding:3px 8px; border-radius:3px; }
        QPushButton:hover { background:#4a4a4a; }
        QPushButton:checked { background:#1a5a8a; border-color:#2a8aba; color:#fff; }
        QSpinBox, QDoubleSpinBox { background:#3a3a3a; color:#ccc; border:1px solid #555; }
        QCheckBox::indicator { width:14px; height:14px; }
        QCheckBox::indicator:unchecked { background:#aaa; border:1px solid #ccc; }
    """


def read_vector(shm):
    """Latest shm content as a flat float64 vector."""
    return np.asarray(shm.get_data(), dtype=np.float64).ravel()


class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, shm_name, i0=None, i1=None, fps=15.0, light=False):
        super().__init__()
        self.setupUi(self)
        self.setStyleSheet(make_stylesheet(light))

        self.shm = dao.shm(shm_name)
        self.shm_name = shm_name
        self.n = read_vector(self.shm).size

        self.idxFromSpin.setMaximum(max(self.n - 1, 0))
        self.idxToSpin.setMaximum(max(self.n - 1, 0))
        self.idxFromSpin.setValue(0 if i0 is None else int(np.clip(i0, 0, self.n - 1)))
        self.idxToSpin.setValue(self.n - 1 if i1 is None else int(np.clip(i1, 0, self.n - 1)))

        self.plot = pg.PlotItem()
        self.plotWidget.setCentralItem(self.plot)
        self.plot.showGrid(x=False, y=True, alpha=0.3)
        self.plot.setLabel("bottom", "index")
        self.plot.setMouseEnabled(x=True, y=False)
        self.plot.setMenuEnabled(False)
        self.bar = pg.BarGraphItem(x=[0], height=[0], width=0.9,
                                   brush=BAR_BRUSH, pen=pg.mkPen(None))
        self.plot.addItem(self.bar)

        self._t = time.time()
        self._c = self.shm.get_counter()

        self.idxFromSpin.valueChanged.connect(self._range_changed)
        self.idxToSpin.valueChanged.connect(self._range_changed)
        self.fullRangeButton.clicked.connect(self._full_range)
        self.autoscaleCheck.toggled.connect(self._autoscale_toggled)
        self.yMinSpin.valueChanged.connect(self._refresh)
        self.yMaxSpin.valueChanged.connect(self._refresh)
        self.fpsSpin.valueChanged.connect(self._set_fps)

        self.timer = QTimer(self)
        self.timer.timeout.connect(self._refresh)

        self._autoscale_toggled(self.autoscaleCheck.isChecked())
        self._set_fps(fps)
        self._range_changed()

    # ------------------------------------------------------------------
    def _index_range(self):
        a, b = self.idxFromSpin.value(), self.idxToSpin.value()
        return (a, b) if a <= b else (b, a)

    def _range_changed(self, *_):
        a, b = self._index_range()
        self.plot.setXRange(a - 0.5, b + 0.5, padding=0)
        self._refresh()

    def _full_range(self):
        self.idxFromSpin.blockSignals(True)
        self.idxToSpin.blockSignals(True)
        self.idxFromSpin.setValue(0)
        self.idxToSpin.setValue(max(self.n - 1, 0))
        self.idxFromSpin.blockSignals(False)
        self.idxToSpin.blockSignals(False)
        self._range_changed()

    def _autoscale_toggled(self, on):
        self.yMinSpin.setEnabled(not on)
        self.yMaxSpin.setEnabled(not on)
        self._refresh()

    def _set_fps(self, hz):
        hz = max(0.5, float(hz))
        self.fpsSpin.blockSignals(True)
        self.fpsSpin.setValue(hz)
        self.fpsSpin.blockSignals(False)
        self.timer.start(int(1000.0 / hz))

    # ------------------------------------------------------------------
    def _refresh(self, *_):
        if self.freezeButton.isChecked():
            return
        v = read_vector(self.shm)

        if v.size != self.n:                      # shm was resized under us
            self.n = v.size
            for spin in (self.idxFromSpin, self.idxToSpin):
                spin.blockSignals(True)
                spin.setMaximum(max(self.n - 1, 0))
                spin.blockSignals(False)

        a, b = self._index_range()
        b = min(b, v.size - 1)
        a = min(a, b)
        idx = np.arange(a, b + 1)
        seg = v[a:b + 1]

        if self.autoscaleCheck.isChecked() and seg.size:
            lo, hi = float(np.min(seg)), float(np.max(seg))
            if lo == hi:
                lo, hi = lo - 1.0, hi + 1.0
            pad = 0.05 * (hi - lo)
            ylo, yhi = lo - pad, hi + pad
            for spin, val in ((self.yMinSpin, lo), (self.yMaxSpin, hi)):
                spin.blockSignals(True)
                spin.setValue(val)
                spin.blockSignals(False)
        else:
            ylo, yhi = self.yMinSpin.value(), self.yMaxSpin.value()
            if yhi <= ylo:
                yhi = ylo + 1.0

        # draw bars from the bottom of the visible window, not from y=0, so the
        # shape stays readable even when the values sit far from zero
        self.bar.setOpts(x=idx, y0=ylo, height=seg - ylo, width=0.9)
        self.plot.setYRange(ylo, yhi, padding=0)

        cnt = self.shm.get_counter()
        now = time.time()
        dt = now - self._t
        freq = (cnt - self._c) / dt if dt > 0 else 0.0
        self._t, self._c = now, cnt

        if seg.size:
            self.infoLabel.setText(
                f"{os.path.basename(self.shm_name)}  n={self.n}   "
                f"showing [{a}:{b}] ({seg.size})   "
                f"min={seg.min():.4g}  max={seg.max():.4g}  "
                f"mean={seg.mean():.4g}  std={seg.std():.4g}   |   {freq:5.1f} Hz")
        else:
            self.infoLabel.setText(f"{os.path.basename(self.shm_name)}  n={self.n}  "
                                   f"(empty selection)")


def parse_args():
    p = argparse.ArgumentParser(description="bar-chart display of a 1D shm vector")
    p.add_argument("shmName", help="shared memory path")
    p.add_argument("--from", dest="i0", type=int, default=None,
                   help="first index to display (default: 0)")
    p.add_argument("--to", dest="i1", type=int, default=None,
                   help="last index to display, inclusive (default: last)")
    p.add_argument("--fps", type=float, default=15.0, help="update rate [Hz]")
    p.add_argument("--light", action="store_true", help="light mode")
    return p.parse_args()


def main():
    args = parse_args()
    if args.light:
        pg.setConfigOption("background", "#f0f0f0")
        pg.setConfigOption("foreground", "#000000")
    else:
        pg.setConfigOption("background", "#1e1e1e")
        pg.setConfigOption("foreground", "#cccccc")

    app = QApplication(sys.argv)
    try:
        gui = Main(args.shmName, i0=args.i0, i1=args.i1, fps=args.fps, light=args.light)
    except Exception as exc:                       # noqa: BLE001
        print(f"daoBarDisp: cannot open {args.shmName}: {exc}", file=sys.stderr)
        QMessageBox.critical(None, "daoBarDisp", f"{args.shmName}\n\n{exc}")
        return 1
    gui.setWindowTitle(f"daoBarDisp  [{os.path.basename(args.shmName)}]")
    gui.show()
    return app.exec_()


if __name__ == "__main__":
    sys.exit(main())
