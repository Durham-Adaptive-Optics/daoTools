#!/usr/bin/env python3
"""
daoTurbulenceVonKarmanCtrl.py -- a mini GUI for daoTurbulenceVonKarmanSimulator.py.

Set the turbulence (r0, outer scale, wind, rate, amplitude: physical with the DM
stroke, or an RMS command; gain, clip) and START: the simulator runs in its own tmux
session (daoTmux; START again restarts it with the new values), so closing this window
leaves it running. STOP sends it Ctrl-C: it sets its channel back to zero.

Live: the channel scattered into the DM map, and its RMS and peak over the last minute.

Usage:
  daoTurbulenceVonKarmanCtrl.py [--shm <channel SHM>] [--map <map SHM>] [--diameter <m>]
                                [--session <tmux>] [--light]
  --shm       DM command channel the turbulence is written to (default /tmp/dmCmd04.im.shm)
  --map       DM actuator map (default /tmp/dmMap.im.shm)
  --diameter  pupil diameter across the DM [m] (default 4)
  --session   tmux session of the simulator (default DAO_TURB)
  --light     light mode (default: dark)
"""
import collections
import getopt
import os
import shlex
import shutil
import subprocess
import sys
import time

import numpy as np
from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtWidgets import (QApplication, QComboBox, QDoubleSpinBox, QFormLayout, QGroupBox,
                             QHBoxLayout, QLabel, QLineEdit, QMainWindow, QPushButton, QSpinBox,
                             QVBoxLayout, QWidget)
import pyqtgraph as pg

try:
    import dao
except ImportError:          # the GUI still starts and stops the simulator
    dao = None

SIMULATOR = 'daoTurbulenceVonKarmanSimulator.py'
HISTORY_S = 60.0


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color: #f0f0f0; color: #000000; }
            QGroupBox { border: 1px solid #aaa; margin-top: 8px; padding-top: 4px; }
            QGroupBox::title { subcontrol-origin: margin; left: 6px; }
            QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
                background-color: #ffffff; color: #000000; border: 1px solid #aaa; }
            QPushButton {
                background-color: #e0e0e0; color: #000000;
                border: 1px solid #aaa; padding: 3px 6px; border-radius: 3px;
            }
            QPushButton:hover   { background-color: #d0d0d0; }
            QPushButton:pressed { background-color: #bbb; }
        """
    return """
        QMainWindow, QWidget { background-color: #1e1e1e; color: #cccccc; }
        QGroupBox { border: 1px solid #555; margin-top: 8px; padding-top: 4px; }
        QGroupBox::title { subcontrol-origin: margin; left: 6px; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background-color: #2a2a2a; color: #cccccc; border: 1px solid #555; }
        QPushButton {
            background-color: #3a3a3a; color: #cccccc;
            border: 1px solid #555; padding: 3px 6px; border-radius: 3px;
        }
        QPushButton:hover   { background-color: #4a4a4a; }
        QPushButton:pressed { background-color: #555; }
    """


def find_simulator():
    """on the PATH, else next to this GUI's repo (apps/), else $DAOROOT/bin"""
    app = shutil.which(SIMULATOR)
    if app:
        return app
    here = os.path.dirname(os.path.abspath(__file__))
    for d in (os.path.join(here, os.pardir, 'apps'), os.path.join(os.getenv('DAOROOT', ''), 'bin')):
        cand = os.path.abspath(os.path.join(d, SIMULATOR))
        if os.path.isfile(cand):
            return cand
    return SIMULATOR


def session_running(session):
    return subprocess.run(['tmux', 'has-session', '-t', f'={session}'],
                          capture_output=True).returncode == 0


class TurbulenceCtrl(QMainWindow):
    def __init__(self, shm, dm_map, diameter, session, light):
        super().__init__()
        self.session = session
        self.setStyleSheet(make_stylesheet(light))
        pg.setConfigOption('background', '#f0f0f0' if light else '#1e1e1e')
        pg.setConfigOption('foreground', '#000000' if light else '#cccccc')

        central = QWidget()
        self.setCentralWidget(central)
        row = QHBoxLayout(central)
        left = QVBoxLayout()
        row.addLayout(left)

        # --- parameters ---
        box = QGroupBox("Turbulence")
        form = QFormLayout(box)
        form.setVerticalSpacing(4)
        self.shmEdit = QLineEdit(shm)
        form.addRow("channel SHM", self.shmEdit)
        self.mapEdit = QLineEdit(dm_map)
        form.addRow("DM map SHM", self.mapEdit)
        self.diameter = self._dspin(0.01, 100, diameter, 2, "pupil diameter across the DM")
        form.addRow("diameter [m]", self.diameter)
        self.r0 = self._dspin(0.005, 10, 0.1, 3, "Fried parameter at 500 nm")
        form.addRow("r0 at 500 nm [m]", self.r0)
        self.L0 = self._dspin(0.1, 1000, 25, 1, "outer scale")
        form.addRow("L0 [m]", self.L0)
        self.wind = self._dspin(0, 200, 5, 2, "wind speed")
        form.addRow("wind [m/s]", self.wind)
        self.direction = self._dspin(-360, 360, 0, 1, "wind direction, 0 = +x")
        form.addRow("direction [deg]", self.direction)
        self.rate = self._dspin(1, 10000, 100, 0, "channel updates per second (not the evolution speed)")
        form.addRow("rate [Hz]", self.rate)
        self.ampMode = QComboBox()
        self.ampMode.addItems(["physical (stroke)", "RMS command"])
        self.ampMode.currentIndexChanged.connect(self._sync_amp)
        form.addRow("amplitude", self.ampMode)
        self.stroke = QLineEdit("1e-5")
        self.stroke.setToolTip("DM surface [m] of a command of 1")
        self.strokeLabel = QLabel("stroke [m / command]")
        form.addRow(self.strokeLabel, self.stroke)
        self.rms = self._dspin(0.0001, 1, 0.05, 4, "RMS command, whatever the r0")
        self.rmsLabel = QLabel("RMS command")
        form.addRow(self.rmsLabel, self.rms)
        self.gain = self._dspin(0, 100, 1, 3, "amplitude factor")
        form.addRow("gain", self.gain)
        self.cap = self._dspin(0, 10, 0.5, 3, "clip the channel to +-cap (0: no clip)")
        form.addRow("clip +-", self.cap)
        self.seed = QSpinBox()
        self.seed.setRange(0, 2 ** 31 - 1)
        self.seed.setToolTip("random seed of the screen (0: a new screen each start)")
        form.addRow("seed", self.seed)
        left.addWidget(box)

        buttons = QHBoxLayout()
        self.startButton = QPushButton("START / RESTART")
        self.startButton.clicked.connect(self.start)
        self.stopButton = QPushButton("STOP")
        self.stopButton.clicked.connect(self.stop)
        buttons.addWidget(self.startButton)
        buttons.addWidget(self.stopButton)
        left.addLayout(buttons)
        self.statusLabel = QLabel("")
        self.statusLabel.setWordWrap(True)
        left.addWidget(self.statusLabel)
        left.addStretch()

        # --- live view ---
        right = QVBoxLayout()
        row.addLayout(right)
        self.mapView = pg.GraphicsLayoutWidget()
        self.mapView.setFixedSize(260, 260)
        vb = self.mapView.addViewBox()
        vb.setAspectLocked(True)
        vb.invertY(True)
        vb.setMouseEnabled(False, False)
        self.mapImg = pg.ImageItem()
        self.cmap = pg.colormap.get('viridis')
        vb.addItem(self.mapImg)
        right.addWidget(self.mapView)
        self.plot = pg.PlotWidget()
        self.plot.setFixedSize(260, 180)
        self.plot.showGrid(x=True, y=True, alpha=0.3)
        self.plot.setLabel('bottom', 'time [s]')
        self.plot.addLegend(offset=(5, 5))
        self.rmsCurve = self.plot.plot([], [], pen=pg.mkPen('#4a9eff', width=2), name='RMS')
        self.peakCurve = self.plot.plot([], [], pen=pg.mkPen('#ff9f40', width=1), name='peak')
        right.addWidget(self.plot)
        self.liveLabel = QLabel("")
        right.addWidget(self.liveLabel)
        right.addStretch()

        self.history = collections.deque()
        self._shm, self._map, self._paths = None, None, None
        self._sync_amp()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(200)

    @staticmethod
    def _dspin(lo, hi, val, decimals, tip):
        s = QDoubleSpinBox()
        s.setRange(lo, hi)
        s.setDecimals(decimals)
        s.setSingleStep(10 ** (-decimals) if decimals else 1)
        s.setValue(val)
        s.setToolTip(tip)
        return s

    def _sync_amp(self):
        physical = self.ampMode.currentIndex() == 0
        for w in (self.stroke, self.strokeLabel):
            w.setVisible(physical)
        for w in (self.rms, self.rmsLabel):
            w.setVisible(not physical)

    # ---- simulator ------------------------------------------------------------
    def command(self):
        args = [find_simulator(), '--shm', self.shmEdit.text().strip(), '--map', self.mapEdit.text().strip(),
                '--diameter', f'{self.diameter.value():g}', '--r0', f'{self.r0.value():g}',
                '--L0', f'{self.L0.value():g}', '--wind', f'{self.wind.value():g}',
                '--direction', f'{self.direction.value():g}', '--rate', f'{self.rate.value():g}',
                '--gain', f'{self.gain.value():g}', '--cap', f'{self.cap.value():g}']
        if self.ampMode.currentIndex() == 0:
            args += ['--stroke', self.stroke.text().strip()]
        else:
            args += ['--rms', f'{self.rms.value():g}']
        if self.seed.value():
            args += ['--seed', str(self.seed.value())]
        return args

    def start(self):
        try:
            float(self.stroke.text())
        except ValueError:
            self.statusLabel.setText(f"stroke '{self.stroke.text()}' is not a number")
            return
        cmd = shlex.join(self.command())
        r = subprocess.run(['daoTmux', self.session, cmd], capture_output=True, text=True)
        if r.returncode != 0:
            self.statusLabel.setText(f"daoTmux failed: {(r.stdout + r.stderr).strip()[-300:]}")
            return
        self.history.clear()
        self.statusLabel.setText(f"started in tmux session {self.session}")
        self.statusLabel.setToolTip(cmd)

    def stop(self):
        subprocess.run(['tmux', 'send-keys', '-t', f'={self.session}', 'C-c', ''], capture_output=True)
        deadline = time.time() + 2.0
        while session_running(self.session) and time.time() < deadline:
            # the simulator exits on Ctrl-C (zeroing its channel); its shell stays
            time.sleep(0.1)
            if not self._simulator_alive():
                break
        subprocess.run(['tmux', 'kill-session', '-t', f'={self.session}'], capture_output=True)
        if self._connect() and self._shm is not None:           # killed before it could clean up
            self._shm.set_data(np.zeros_like(self._shm.get_data()))
        self.statusLabel.setText(f"stopped ({self.session}); channel set to zero")

    def _simulator_alive(self):
        r = subprocess.run(['tmux', 'list-panes', '-t', f'={self.session}', '-F', '#{pane_current_command}'],
                           capture_output=True, text=True)
        return 'python' in r.stdout

    # ---- live view ------------------------------------------------------------
    def _connect(self):
        paths = (self.shmEdit.text().strip(), self.mapEdit.text().strip())
        if dao is None or not all(os.path.exists(p) for p in paths):
            self._shm = self._map = None
            return False
        if paths != self._paths or self._shm is None:
            try:
                self._shm, self._map = dao.shm(paths[0]), np.asarray(dao.shm(paths[1]).get_data()) != 0
                self._paths = paths
            except Exception:                                    # noqa: BLE001
                self._shm = self._map = None
                return False
        return True

    def refresh(self):
        running = session_running(self.session)
        self.setWindowTitle(f"Turbulence ({'running' if running else 'stopped'})")
        if not self._connect():
            self.liveLabel.setText("channel / map SHM not found")
            return
        try:
            cmd = np.ravel(self._shm.get_data()).astype(float)
        except Exception:                                        # noqa: BLE001
            self._shm = None
            return
        if cmd.size != int(self._map.sum()):
            self.liveLabel.setText(f"{cmd.size} values, the map has {int(self._map.sum())} actuators")
            return
        peak = float(np.abs(cmd).max()) if cmd.size else 0.0
        lim = max(peak, 1e-6)
        rgba = np.zeros(self._map.shape + (4,), np.ubyte)
        rgba[...] = (90, 90, 90, 255)                    # outside the actuator map
        rgba[self._map] = self.cmap.map((cmd / lim + 1) / 2, mode='byte')
        self.mapImg.setImage(rgba.swapaxes(0, 1), levels=(0, 255))
        now = time.time()
        self.history.append((now, float(cmd.std()), peak))
        while self.history and now - self.history[0][0] > HISTORY_S:
            self.history.popleft()
        t = np.array([h[0] for h in self.history]) - now
        self.rmsCurve.setData(t, [h[1] for h in self.history])
        self.peakCurve.setData(t, [h[2] for h in self.history])
        self.liveLabel.setText(f"RMS {cmd.std():.4f}   peak {peak:.4f}   "
                               f"({'running' if running else 'stopped'})")


def main():
    shm, dm_map, diameter, session, light = '/tmp/dmCmd04.im.shm', '/tmp/dmMap.im.shm', 4.0, 'DAO_TURB', False
    try:
        opts, _ = getopt.getopt(sys.argv[1:], 'h', ['help', 'shm=', 'map=', 'diameter=', 'session=', 'light'])
    except getopt.GetoptError as e:
        print(f'err: {e}\n{__doc__}')
        sys.exit(2)
    for opt, arg in opts:
        if opt in ('-h', '--help'):
            print(__doc__)
            sys.exit(0)
        elif opt == '--shm':
            shm = arg
        elif opt == '--map':
            dm_map = arg
        elif opt == '--diameter':
            diameter = float(arg)
        elif opt == '--session':
            session = arg
        elif opt == '--light':
            light = True
    app = QApplication([])
    win = TurbulenceCtrl(shm, dm_map, diameter, session, light)
    win.show()
    sys.exit(app.exec_())


if __name__ == '__main__':
    main()
