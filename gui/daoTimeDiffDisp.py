#!/usr/bin/env python3
"""
Time Diff Display - SHM-to-SHM latency measurement and plot.

Standalone version of daoShmViewer's "SHM Latency" tab. Start launches the
real `daoTimeDiff` binary (see apps/daoTimeDiff.c) inside its own, named tmux
session, and this GUI only monitors the measurement / Avg / Rms / Array SHMs
that tool publishes: all the measurement math (the sliding window, AVG, RMS)
lives in the C tool, nothing is recomputed or buffered here.

Usage: daoTimeDiffDisp.py <SHM1> <SHM2> [options]

  SHM1   start of the interval (e.g. the camera/WFS image SHM)
  SHM2   end of the interval   (e.g. the DM command SHM)

Options:
  --sem1 N      semaphore to wait on in SHM1 (default: 5)
  --sem2 N      semaphore to wait on in SHM2 (default: 5)
  -n, --window N
                sliding-window size for AVG/RMS, daoTimeDiff's -n (default: 100)
  -m, --more    pass -m to daoTimeDiff (frame IDs / diff / negTs in its tmux
                session)
  --meas PATH   measurement SHM to publish to (default: derived from the two
                SHM names, /tmp/daoLatency_<shm1>_<shm2>.im.shm)
  --keep        leave daoTimeDiff running when this window is closed (default:
                closing the window kills the session, like pressing Stop)
  --light       light mode (default: dark)

Semaphores default to 5 rather than 0 on purpose: same convention as
daoPlotLatency.py, so a monitoring tap does not consume the semaphore posts
the real pipeline consumer needs.

The two SHM paths stay editable while the GUI runs - Stop, change them, Start
again to measure a different pair.

Examples:
  daoTimeDiffDisp.py /tmp/wfsIm.im.shm /tmp/dmCmd.im.shm
  daoTimeDiffDisp.py /tmp/wfsIm.im.shm /tmp/dmCmd.im.shm -n 500
  daoTimeDiffDisp.py /tmp/wfsIm.im.shm /tmp/dmCmd.im.shm --sem1 6 --sem2 6 --light
"""

import os

# Bind pyqtgraph to PyQt5 -- PyQt6 is also installed and pyqtgraph would pick
# it by default, which then clashes with the PyQt5 widgets uic builds.
os.environ.setdefault("PYQTGRAPH_QT_LIB", "PyQt5")

import argparse
import subprocess
import sys

from PyQt5.uic import loadUiType
from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtWidgets import QApplication, QFileDialog, QMessageBox

import numpy as np
import pyqtgraph as pg
import dao

_UI_DIR = os.getenv("DAOROOT", ".") + "/data/"
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(_UI_DIR, "daoTimeDiffDisp.ui"))

SAMPLE_BRUSH = "#4a9eff"
STAT_PEN = "#55dd55"
POLL_MS = 100          # 10 Hz readout - plenty for a GUI, and well below the
                       # rate at which daoTimeDiff republishes its window.


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color:#f0f0f0; color:#000; }
            QLabel, QCheckBox { color:#000; }
            QPushButton { background:#e0e0e0; color:#000; border:1px solid #aaa;
                          padding:3px 8px; border-radius:3px; }
            QPushButton:hover { background:#d0d0d0; }
            QPushButton:disabled { color:#888; background:#ececec; }
            QSpinBox, QLineEdit { background:#fff; color:#000; border:1px solid #aaa; }
            QGroupBox { border:1px solid #aaa; border-radius:4px; margin-top:8px; color:#000; }
            QGroupBox::title { subcontrol-origin:margin; left:8px; color:#555; }
        """
    return """
        QMainWindow, QWidget { background-color:#1e1e1e; color:#ccc; }
        QLabel, QCheckBox { color:#ccc; }
        QPushButton { background:#3a3a3a; color:#ccc; border:1px solid #555;
                      padding:3px 8px; border-radius:3px; }
        QPushButton:hover { background:#4a4a4a; }
        QPushButton:disabled { color:#777; background:#2a2a2a; border-color:#3a3a3a; }
        QSpinBox, QLineEdit { background:#3a3a3a; color:#ccc; border:1px solid #555; }
        QGroupBox { border:1px solid #555; border-radius:4px; margin-top:8px; color:#ccc; }
        QGroupBox::title { subcontrol-origin:margin; left:8px; color:#aaa; }
    """


def _shm_base(path):
    """Basename with the shm suffix stripped and anything non-alphanumeric
    folded to '_', so it is safe in a tmux session / SHM name."""
    name = os.path.basename(path)
    for suffix in (".im.shm", ".shm"):
        if name.endswith(suffix):
            name = name[: -len(suffix)]
            break
    return "".join(c if c.isalnum() else "_" for c in name)


def tmux_name(path1, path2):
    """Deterministic, identifiable tmux session name for a given SHM pair,
    e.g. daoLatency_wfsIm_dmCmd - stable across Start/Stop, so restarting on
    the same pair reuses (and cleanly replaces) the same session instead of
    piling up orphans. Same scheme as daoShmViewer's latency tab, so the two
    front-ends recognise each other's sessions."""
    return f"daoLatency_{_shm_base(path1)}_{_shm_base(path2)}"


def sibling_shm_name(measName, prefix):
    """<name>.im.shm -> <name><prefix>.im.shm, the same convention the C-side
    daoToolsInsertShmNamePrefix() uses for daoTimeDiff's Avg/Rms/Array SHMs."""
    suffix = ".im.shm"
    if measName.endswith(suffix):
        return measName[: -len(suffix)] + prefix + suffix
    return measName + prefix


class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, shm1, shm2, sem1, sem2, window, moreInfo, measName, keepOnExit):
        super(Main, self).__init__()
        self.setupUi(self)

        self.shm1Edit.setText(shm1 or "")
        self.shm2Edit.setText(shm2 or "")
        self.sem1Spin.setValue(sem1)
        self.sem2Spin.setValue(sem2)
        self.popSizeSpin.setValue(window)
        self.moreInfoCheck.setChecked(moreInfo)

        # Explicit --meas pins the measurement SHM name; otherwise it is
        # re-derived from whatever pair is in the edits at each Start.
        self.measNameOverride = measName
        self.keepOnExit = keepOnExit

        self.session = None
        self.measShm = self.avgShm = self.rmsShm = self.arrayShm = None
        self.measShmName = self.avgShmName = self.rmsShmName = self.arrayShmName = None
        self.lastCounter = None

        self._setup_plots()

        self.startButton.clicked.connect(self.start_measurement)
        self.stopButton.clicked.connect(self.stop_measurement)
        self.shm1BrowseButton.clicked.connect(lambda: self._browse(self.shm1Edit))
        self.shm2BrowseButton.clicked.connect(lambda: self._browse(self.shm2Edit))

        self.timer = QTimer(self)
        self.timer.setInterval(POLL_MS)
        self.timer.timeout.connect(self.update_measurement)

    # ------------------------------------------------------------------
    def _setup_plots(self):
        """Two views of the same rolling buffer: a scatter trend with AVG/RMS
        overlaid, and a histogram of its distribution. Deliberately plain -
        no toolbar buttons, no context menu, no autoscale fighting."""
        self.plot = pg.PlotItem()
        self.plotWidget.setCentralItem(self.plot)
        self.plot.showGrid(x=False, y=True, alpha=0.15)
        self.plot.setLabel("left", "latency (us)")
        self.plot.setLabel("bottom", "sample")
        self.plot.hideButtons()
        self.plot.setMenuEnabled(False)
        self.curve = self.plot.plot(pen=None, symbol="o", symbolSize=4,
                                    symbolPen=None, symbolBrush=SAMPLE_BRUSH)
        self.avgLine = pg.InfiniteLine(angle=0, pen=pg.mkPen(STAT_PEN, width=1))
        self.avgPlusLine = pg.InfiniteLine(angle=0, pen=pg.mkPen(STAT_PEN, width=1, style=Qt.DashLine))
        self.avgMinusLine = pg.InfiniteLine(angle=0, pen=pg.mkPen(STAT_PEN, width=1, style=Qt.DashLine))
        for ln in (self.avgLine, self.avgPlusLine, self.avgMinusLine):
            ln.hide()
            self.plot.addItem(ln)

        self.histPlot = pg.PlotItem()
        self.histWidget.setCentralItem(self.histPlot)
        self.histPlot.showGrid(x=False, y=True, alpha=0.15)
        self.histPlot.setLabel("left", "count")
        self.histPlot.setLabel("bottom", "latency (us)")
        self.histPlot.hideButtons()
        self.histPlot.setMenuEnabled(False)
        self.histCurve = self.histPlot.plot(stepMode="center", fillLevel=0,
                                            brush=SAMPLE_BRUSH + "80",
                                            pen=pg.mkPen(SAMPLE_BRUSH, width=1))

    def _browse(self, edit):
        path, _ = QFileDialog.getOpenFileName(self, "Select a SHM", "/tmp", "SHM files (*.im.shm);;All files (*)")
        if path:
            edit.setText(path)

    def _error(self, msg):
        QMessageBox.critical(self, "daoTimeDiffDisp", msg)

    # ------------------------------------------------------------------
    def start_measurement(self):
        """Launch daoTimeDiff in its own tmux session, then start polling the
        SHMs it publishes."""
        path1 = self.shm1Edit.text().strip()
        path2 = self.shm2Edit.text().strip()
        if not path1 or not path2:
            self._error("Set both SHM 1 and SHM 2 first")
            return
        # daoTimeDiff would fail inside tmux where nothing is watching, so
        # catch a missing input here, where the message is visible.
        missing = [p for p in (path1, path2) if not os.path.exists(p)]
        if missing:
            self._error("SHM does not exist:\n" + "\n".join(missing))
            return

        session = tmux_name(path1, path2)
        measName = self.measNameOverride or f"/tmp/{session}.im.shm"

        cmd = (f"daoTimeDiff -S {path1} {path2} "
               f"{self.sem1Spin.value()} {self.sem2Spin.value()} {measName} "
               f"-n {self.popSizeSpin.value()}"
               f"{' -m' if self.moreInfoCheck.isChecked() else ''} -L")
        try:
            # Replace any stale session with the same (deterministic) name first.
            subprocess.run(f"tmux kill-session -t {session}", shell=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            result = subprocess.run(f'tmux new-session -d -s {session} "{cmd}"',
                                    shell=True, capture_output=True, text=True)
            if result.returncode != 0:
                self._error(f"Could not start tmux session: {result.stderr}")
                return
        except Exception as e:
            self._error(f"Could not start daoTimeDiff: {e}")
            return

        self.session = session
        self.measShmName = measName
        self.avgShmName = sibling_shm_name(measName, "Avg")
        self.rmsShmName = sibling_shm_name(measName, "Rms")
        self.arrayShmName = sibling_shm_name(measName, "Array")
        self.measShm = self.avgShm = self.rmsShm = self.arrayShm = None
        self.lastCounter = None

        self.curve.setData([], [])
        self.histCurve.setData([], [])
        for ln in (self.avgLine, self.avgPlusLine, self.avgMinusLine):
            ln.hide()
        self.statsLabel.setText("AVG: --   RMS: --   n: 0")
        self.sessionLabel.setText(f"tmux session: {session}  (cmd: {cmd})")
        self.startButton.setEnabled(False)
        self.stopButton.setEnabled(True)
        self._set_args_enabled(False)
        self.timer.start()

    def stop_measurement(self):
        """Kill the tmux session (and with it, daoTimeDiff running inside),
        stop polling."""
        self.timer.stop()
        if self.session:
            subprocess.run(f"tmux kill-session -t {self.session}", shell=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.sessionLabel.setText(f"tmux session: {self.session} (stopped)")
        else:
            self.sessionLabel.setText("tmux session: --")
        self.session = None
        # Drop the SHM handles: a later Start may well point at a different
        # measurement SHM, and daoTimeDiff recreates these on launch.
        self.measShm = self.avgShm = self.rmsShm = self.arrayShm = None
        self.startButton.setEnabled(True)
        self.stopButton.setEnabled(False)
        self._set_args_enabled(True)

    def _set_args_enabled(self, on):
        """The args are baked into daoTimeDiff's command line at launch, so
        editing them while it runs would just be misleading."""
        for w in (self.shm1Edit, self.shm2Edit, self.shm1BrowseButton, self.shm2BrowseButton,
                  self.sem1Spin, self.sem2Spin, self.popSizeSpin, self.moreInfoCheck):
            w.setEnabled(on)

    # ------------------------------------------------------------------
    def update_measurement(self):
        """Timer tick: attach to daoTimeDiff's measurement/Avg/Rms/Array SHMs
        (it needs a moment to create them after tmux launches it, so this
        retries silently until they appear), then, once a new sample is
        published, read and display it. daoTimeDiff itself owns the rolling
        window (Array, chronologically ordered, exactly -n elements once
        full) and the AVG/RMS - this is a pure reader."""
        if self.measShm is None:
            try:
                self.measShm = dao.shm(self.measShmName)
                self.avgShm = dao.shm(self.avgShmName)
                self.rmsShm = dao.shm(self.rmsShmName)
                self.arrayShm = dao.shm(self.arrayShmName)
            except Exception:
                return  # daoTimeDiff hasn't created the SHMs yet - retry next tick

        try:
            counter = self.measShm.get_counter()
        except Exception as e:
            self.stop_measurement()
            self._error(f"Lost the measurement SHM ({e})")
            return
        if self.lastCounter is not None and counter == self.lastCounter:
            return
        self.lastCounter = counter

        try:
            avg = float(np.ravel(self.avgShm.get_data())[0])
            rms = float(np.ravel(self.rmsShm.get_data())[0])
            # The Array SHM's own write counter tracks exactly how many of its
            # popSize slots are valid so far (same growth as daoTimeDiff's
            # circCount) - avoids plotting the zero-filled tail before the
            # window first fills.
            popSize = self.arrayShm.get_data().size
            n = min(self.arrayShm.get_counter(), popSize)
            arr = np.ravel(self.arrayShm.get_data())[:n]
        except Exception:
            return  # transient read race - wait for the next tick

        self.statsLabel.setText(f"AVG: {avg:9.3f} us   RMS: {rms:9.3f} us   n: {n}/{popSize}")

        self.curve.setData(np.arange(n), arr)
        self.avgLine.setPos(avg)
        self.avgPlusLine.setPos(avg + rms)
        self.avgMinusLine.setPos(avg - rms)
        for ln in (self.avgLine, self.avgPlusLine, self.avgMinusLine):
            ln.show()

        # Bin count capped and tied to n so the histogram stays meaningful
        # (and cheap) from the very first few samples.
        if n > 0:
            nbins = int(np.clip(n // 2, 5, 40))
            counts, edges = np.histogram(arr, bins=nbins)
            self.histCurve.setData(edges, counts)

    # ------------------------------------------------------------------
    def closeEvent(self, event):
        """Don't leave an orphaned SCHED_FIFO daoTimeDiff behind when the
        window goes away - unless --keep was asked for, in which case the
        tmux session is left running and can be attached to by name."""
        if self.session and not self.keepOnExit:
            self.stop_measurement()
        else:
            self.timer.stop()
        super(Main, self).closeEvent(event)


def main():
    parser = argparse.ArgumentParser(
        description="SHM-to-SHM latency measurement and plot (front-end for daoTimeDiff)",
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("shm1", nargs="?", default="", help="start-of-interval SHM")
    parser.add_argument("shm2", nargs="?", default="", help="end-of-interval SHM")
    parser.add_argument("--sem1", type=int, default=5, help="semaphore to wait on in SHM1 (default: 5)")
    parser.add_argument("--sem2", type=int, default=5, help="semaphore to wait on in SHM2 (default: 5)")
    parser.add_argument("-n", "--window", type=int, default=100,
                        help="AVG/RMS sliding-window size, daoTimeDiff's -n (default: 100)")
    parser.add_argument("-m", "--more", action="store_true", help="pass -m to daoTimeDiff")
    parser.add_argument("--meas", default="", help="measurement SHM (default: derived from the SHM pair)")
    parser.add_argument("--keep", action="store_true",
                        help="leave daoTimeDiff running when the window is closed")
    parser.add_argument("--light", action="store_true", help="light mode (default: dark)")
    args = parser.parse_args()

    if args.light:
        pg.setConfigOption("background", "#f0f0f0")
        pg.setConfigOption("foreground", "#000000")
    else:
        pg.setConfigOption("background", "#1e1e1e")
        pg.setConfigOption("foreground", "#cccccc")

    app = QApplication(sys.argv)
    app.setStyleSheet(make_stylesheet(args.light))

    win = Main(args.shm1, args.shm2, args.sem1, args.sem2,
               args.window, args.more, args.meas, args.keep)
    if args.shm1 and args.shm2:
        win.setWindowTitle("SHM Latency  [%s -> %s]"
                           % (os.path.basename(args.shm1), os.path.basename(args.shm2)))
    else:
        win.setWindowTitle("SHM Latency")
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
