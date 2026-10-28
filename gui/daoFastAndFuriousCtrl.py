#!/usr/bin/env python3
"""
Fast & Furious Control - runs daoFastAndFurious.py's steps and shows the loop

Drives the Fast & Furious focal-plane wavefront sensing (daoFastAndFurious.py): builds
the modal basis, calibrates (PSF centre, camera orientation, basis sign and scale),
runs and stops the loop, with the gain, the leak and the hold applied live through
control SHMs. A test aberration can be put on a spare DM channel through M2A. Shows the
PSF around the calibrated centre, the Strehl proxy of each iteration and the pupil phase
of the correction.

Each step runs daoFastAndFurious.py as a subprocess, its output in the log. The files
(basis, calibration, final theta) are kept in the work directory. The settings are
remembered between sessions.

Usage: daoFastAndFuriousCtrl.py [options]

Options:
  -i  PSF camera shm (default: /tmp/psf.im.shm)
  -o  output shm: modal channel, DM channel (with M2A) or WFS reference (with IM)
  -z  modes-to-actuator (M2A) matrix shm (default: /tmp/dmM2A.im.shm)
  -m  DM actuator map shm (default: /tmp/dmMap.im.shm)
  -w  work directory (default: $DAODATA/fastAndFurious)
  --light  Light mode (default: dark)

Options given override the remembered settings.

Examples:
  daoFastAndFuriousCtrl.py
  daoFastAndFuriousCtrl.py -i /tmp/sciIm.im.shm -o /tmp/dmCmd06.im.shm -z /tmp/dmM2A.im.shm
"""

import os
import re
import sys
import getopt
import shutil
import signal
import numpy as np
from PyQt5.QtCore import QProcess, QSettings, QTimer, Qt
from PyQt5.QtWidgets import (QApplication, QMainWindow, QWidget, QGroupBox, QFormLayout,
                             QHBoxLayout, QVBoxLayout, QGridLayout, QLineEdit, QPushButton,
                             QDoubleSpinBox, QSpinBox, QCheckBox, QLabel, QPlainTextEdit,
                             QMessageBox, QScrollArea, QSplitter)
import pyqtgraph as pg
import dao
import daoFastAndFuriousLib as ff

ITERATION = re.compile(r'iteration (\d+): Strehl ~ ([-\d.eE+]+)')
CALIB_RESULT = re.compile(r'orientation (\d+), scale (\S+), correlation (\S+), Strehl ~ (\S+)')


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color: #f0f0f0; color: #000000; }
            QGroupBox { border: 1px solid #aaa; margin-top: 8px; padding-top: 4px; }
            QGroupBox::title { subcontrol-origin: margin; left: 6px; }
            QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit {
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
        QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit {
            background-color: #2a2a2a; color: #cccccc; border: 1px solid #555; }
        QPushButton {
            background-color: #3a3a3a; color: #cccccc;
            border: 1px solid #555; padding: 3px 6px; border-radius: 3px;
        }
        QPushButton:hover   { background-color: #4a4a4a; }
        QPushButton:pressed { background-color: #555; }
        QPushButton:disabled { color: #666; }
    """


def findApp():
    '''daoFastAndFurious.py: on the PATH, else in $DAOROOT/bin.'''
    app = shutil.which('daoFastAndFurious.py')
    if app is None and os.getenv('DAOROOT'):
        app = os.path.join(os.getenv('DAOROOT'), 'bin', 'daoFastAndFurious.py')
    return app


def defaultWorkDir():
    root = os.getenv('DAODATA') or os.path.expanduser('~/.dao')
    return os.path.join(root, 'fastAndFurious')


# the fields remembered: key -> (default, kind)
FIELDS = {
    'camera':       ('/tmp/psf.im.shm', str),
    'background':   ('', str),
    'output':       ('/tmp/dmCmd06.im.shm', str),
    'm2a':          ('/tmp/dmM2A.im.shm', str),
    'map':          ('/tmp/dmMap.im.shm', str),
    'im':           ('', str),
    'workDir':      (defaultWorkDir(), str),
    'ctrlPrefix':   ('/tmp/ff', str),
    'samp':         (2.0, float),
    'size':         (63, int),
    'frames':       (1, int),
    'delay':        (2, int),
    'modes':        (50, int),
    'res':          (128, int),
    'diameter':     (0.0, float),
    'coupling':     (0.3, float),
    'amp':          (0.05, float),
    'calModes':     (20, int),
    'fitSamp':      (False, bool),
    'gain':         (0.3, float),
    'leak':         (0.97, float),
    'iterations':   (0, int),
    'restore':      (False, bool),
    'abChannel':    ('/tmp/dmCmd05.im.shm', str),
    'abAmp':        (0.02, float),
    'abSeed':       (0, int),
}


class Main(QMainWindow):
    def __init__(self, overrides, light):
        super().__init__()
        self.setWindowTitle('Fast & Furious Control')
        self.light = light
        self.settings = QSettings('dao', 'daoFastAndFuriousCtrl')
        self.app = findApp()
        self.proc = None
        self.procKind = None
        self.outBuf = ''
        self.warning = None
        self.calib = None
        self.strehl = []
        self.camShm = None
        self.camName = None
        self.phaseShm = None
        self.phaseName = None
        self.center = None
        self.ctrl = {}

        self.w = {}
        self.buildUi()
        self.loadSettings(overrides)
        self.createControlShms()
        self.refreshFiles()

        self.timer = QTimer(self)
        self.timer.timeout.connect(self.updateDisplays)
        self.timer.start(200)

    # ------------------------------------------------------------------ UI
    def field(self, key, minimum=None, maximum=None, decimals=None, step=None):
        default, kind = FIELDS[key]
        if kind is str:
            wdg = QLineEdit()
        elif kind is bool:
            wdg = QCheckBox()
        elif kind is int:
            wdg = QSpinBox()
            wdg.setRange(minimum if minimum is not None else 0,
                         maximum if maximum is not None else 100000)
        else:
            wdg = QDoubleSpinBox()
            wdg.setDecimals(decimals or 3)
            wdg.setRange(minimum if minimum is not None else 0.0,
                         maximum if maximum is not None else 1e6)
            wdg.setSingleStep(step or 0.01)
        self.w[key] = wdg
        return wdg

    def value(self, key):
        wdg, kind = self.w[key], FIELDS[key][1]
        if kind is str:
            return wdg.text().strip()
        if kind is bool:
            return wdg.isChecked()
        return wdg.value()

    def setValue(self, key, v):
        wdg, kind = self.w[key], FIELDS[key][1]
        if kind is str:
            wdg.setText(str(v))
        elif kind is bool:
            wdg.setChecked(v in (True, 'true', '1', 1))
        else:
            wdg.setValue(kind(v))

    def buildUi(self):
        left = QWidget()
        lv = QVBoxLayout(left)

        g = QGroupBox('SHMs')
        f = QFormLayout(g)
        f.addRow('PSF camera', self.field('camera'))
        f.addRow('Background', self.field('background'))
        f.addRow('Output', self.field('output'))
        f.addRow('M2A', self.field('m2a'))
        f.addRow('DM map', self.field('map'))
        f.addRow('IM (WFS ref.)', self.field('im'))
        f.addRow('Control prefix', self.field('ctrlPrefix'))
        f.addRow('Work directory', self.field('workDir'))
        lv.addWidget(g)

        g = QGroupBox('Optics and timing')
        f = QFormLayout(g)
        f.addRow('Pixels per lambda/D', self.field('samp', 0.5, 100, 3, 0.1))
        f.addRow('Crop (odd), px', self.field('size', 9, 1001))
        f.addRow('Frames averaged', self.field('frames', 1, 1000))
        f.addRow('Frames skipped', self.field('delay', 0, 1000))
        f.addRow('Modes', self.field('modes', 1, 10000))
        lv.addWidget(g)

        g = QGroupBox('Basis')
        f = QFormLayout(g)
        f.addRow('Pupil pixels', self.field('res', 16, 1024))
        f.addRow('Diameter, pitches (0: auto)', self.field('diameter', 0, 1000, 2, 0.5))
        f.addRow('Coupling', self.field('coupling', 0.01, 0.99, 2, 0.05))
        self.btnBasis = QPushButton('Build basis')
        self.btnBasis.clicked.connect(self.buildBasis)
        self.lblBasis = QLabel()
        f.addRow(self.btnBasis, self.lblBasis)
        lv.addWidget(g)

        g = QGroupBox('Calibration')
        f = QFormLayout(g)
        f.addRow('Poke amplitude', self.field('amp', 0.0001, 100, 4, 0.01))
        f.addRow('Modes poked', self.field('calModes', 1, 10000))
        f.addRow('Fit the sampling', self.field('fitSamp'))
        self.btnCalib = QPushButton('Calibrate')
        self.btnCalib.clicked.connect(self.calibrate)
        self.lblCalib = QLabel()
        self.lblCalib.setWordWrap(True)
        f.addRow(self.btnCalib, self.lblCalib)
        lv.addWidget(g)

        g = QGroupBox('Loop')
        f = QFormLayout(g)
        f.addRow('Gain', self.field('gain', 0, 2, 3, 0.05))
        f.addRow('Leak', self.field('leak', 0, 1, 3, 0.01))
        f.addRow('Iterations (0: until stop)', self.field('iterations', 0, 1000000))
        f.addRow('Restore output at stop', self.field('restore'))
        self.w['gain'].valueChanged.connect(lambda v: self.writeCtrl('gain', v))
        self.w['leak'].valueChanged.connect(lambda v: self.writeCtrl('leak', v))
        row = QHBoxLayout()
        self.btnStart = QPushButton('Start')
        self.btnStart.clicked.connect(self.startLoop)
        self.btnStop = QPushButton('Stop')
        self.btnStop.clicked.connect(self.stopProcess)
        self.btnHold = QPushButton('Hold')
        self.btnHold.setCheckable(True)
        self.btnHold.toggled.connect(self.hold)
        self.btnZero = QPushButton('Zero output')
        self.btnZero.clicked.connect(self.zeroOutput)
        for b in (self.btnStart, self.btnStop, self.btnHold, self.btnZero):
            row.addWidget(b)
        f.addRow(row)
        lv.addWidget(g)

        g = QGroupBox('Test aberration (through M2A)')
        f = QFormLayout(g)
        f.addRow('DM channel', self.field('abChannel'))
        f.addRow('Amplitude per mode', self.field('abAmp', 0, 100, 4, 0.005))
        f.addRow('Seed', self.field('abSeed', 0, 1000000))
        row = QHBoxLayout()
        b = QPushButton('Apply')
        b.clicked.connect(self.applyAberration)
        row.addWidget(b)
        b = QPushButton('Clear')
        b.clicked.connect(self.clearAberration)
        row.addWidget(b)
        f.addRow(row)
        lv.addWidget(g)
        lv.addStretch(1)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(left)
        scroll.setMinimumWidth(420)

        # displays
        right = QWidget()
        rv = QVBoxLayout(right)
        self.lblStatus = QLabel('idle')
        rv.addWidget(self.lblStatus)
        grid = QGridLayout()
        self.psfView = pg.GraphicsLayoutWidget()
        p = self.psfView.addPlot(title='PSF (log)')
        p.setAspectLocked(True)
        p.hideAxis('left'); p.hideAxis('bottom')
        self.psfImg = pg.ImageItem()
        self.psfImg.setColorMap(pg.colormap.get('inferno'))
        p.addItem(self.psfImg)
        self.phaseView = pg.GraphicsLayoutWidget()
        p = self.phaseView.addPlot(title='Correction phase (rad)')
        p.setAspectLocked(True)
        p.hideAxis('left'); p.hideAxis('bottom')
        self.phaseImg = pg.ImageItem()
        self.phaseImg.setColorMap(pg.colormap.get('viridis'))
        p.addItem(self.phaseImg)
        grid.addWidget(self.psfView, 0, 0)
        grid.addWidget(self.phaseView, 0, 1)
        self.strehlPlot = pg.PlotWidget(title='Strehl proxy')
        self.strehlPlot.setLabel('bottom', 'iteration')
        self.strehlPlot.showGrid(x=True, y=True, alpha=0.3)
        self.strehlCurve = self.strehlPlot.plot(pen=pg.mkPen('#4a9eff', width=2), symbol='o',
                                                symbolSize=4)
        grid.addWidget(self.strehlPlot, 1, 0, 1, 2)
        rv.addLayout(grid, 3)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(5000)
        rv.addWidget(self.log, 1)

        split = QSplitter(Qt.Horizontal)
        split.addWidget(scroll)
        split.addWidget(right)
        split.setStretchFactor(1, 1)
        self.setCentralWidget(split)
        self.resize(1400, 900)
        self.setBusy(None)

    # ------------------------------------------------------------ settings
    def loadSettings(self, overrides):
        for key, (default, _) in FIELDS.items():
            self.setValue(key, self.settings.value(key, default))
        for key, v in overrides.items():
            self.setValue(key, v)

    def saveSettings(self):
        for key in FIELDS:
            self.settings.setValue(key, self.value(key))

    def path(self, name):
        return os.path.join(self.value('workDir'), name)

    def refreshFiles(self):
        basis, calib = self.path('ffBasis.npz'), self.path('ffCalib.npz')
        if os.path.exists(basis):
            with np.load(basis) as d:
                self.lblBasis.setText(f'{d["basis"].shape[0]} modes, {d["pupil"].shape[0]} px pupil')
        else:
            self.lblBasis.setText('no basis yet')
        self.center = None
        self.calib = None
        if os.path.exists(calib):
            with np.load(calib) as d:
                self.calib = {k: d[k].tolist() for k in d}
            c = self.calib
            self.center = tuple(int(v) for v in c['center'])
            self.lblCalib.setText(f'centre {self.center}, orientation {int(c["orientation"])}, '
                                  f'scale {c["scale"]:.4g}, correlation {c["correlation"]:.3f}, '
                                  f'{c["samp"]:.3g} px per lambda/D, crop {int(c["size"])}')
            bad = c['correlation'] < 0.3
            self.lblCalib.setStyleSheet('color: #ff5050;' if bad else '')
        else:
            self.lblCalib.setText('not calibrated')
            self.lblCalib.setStyleSheet('')

    # -------------------------------------------------------- control SHMs
    def createControlShms(self):
        prefix = self.value('ctrlPrefix')
        values = {'gain': self.value('gain'), 'leak': self.value('leak'), 'enable': 1.0}
        for key, v in values.items():
            name = f'{prefix}{key.capitalize()}.im.shm'
            try:
                self.ctrl[key] = (name, dao.shm(name, np.array([[v]], dtype=np.float32)))
            except Exception as e:
                self.append(f'cannot create {name}: {e}')

    def writeCtrl(self, key, v):
        if key in self.ctrl:
            self.ctrl[key][1].set_data(np.array([[v]], dtype=np.float32))

    def hold(self, on):
        self.writeCtrl('enable', 0.0 if on else 1.0)
        self.btnHold.setText('Resume' if on else 'Hold')

    # ----------------------------------------------------------- processes
    def common(self):
        args = ['-i', self.value('camera'), '-B', self.path('ffBasis.npz'),
                '-M', str(self.value('modes')), '-q', str(self.value('samp')),
                '-N', str(self.value('size')), '-n', str(self.value('frames')),
                '-d', str(self.value('delay')), '-o', self.value('output')]
        for opt, key in (('-b', 'background'), ('-z', 'm2a'), ('-I', 'im')):
            if self.value(key):
                args += [opt, self.value(key)]
        return args

    def missingShms(self, keys):
        '''The SHM fields among keys whose file does not exist; warns about them.'''
        missing = [f'{key}: {self.value(key)}' for key in keys
                   if self.value(key) and not os.path.exists(self.value(key))]
        if missing:
            QMessageBox.warning(self, 'F&F', 'SHM not found (set it in the SHMs box):\n'
                                + '\n'.join(missing))
        return bool(missing)

    def launch(self, kind, args):
        if self.proc is not None:
            return
        if self.app is None or not os.path.exists(self.app):
            QMessageBox.warning(self, 'F&F', 'daoFastAndFurious.py not found (PATH, $DAOROOT/bin)')
            return
        self.saveSettings()
        os.makedirs(self.value('workDir'), exist_ok=True)
        self.proc = QProcess(self)
        self.proc.setProcessChannelMode(QProcess.MergedChannels)
        self.proc.setWorkingDirectory(self.value('workDir'))
        self.proc.readyReadStandardOutput.connect(self.readOutput)
        self.proc.finished.connect(self.finished)
        self.procKind = kind
        self.outBuf = ''
        cmd = [sys.executable, '-u', self.app] + args
        self.append('$ ' + ' '.join(cmd[2:]))
        self.proc.start(cmd[0], cmd[1:])
        self.setBusy(kind)

    def readOutput(self):
        self.outBuf += bytes(self.proc.readAllStandardOutput()).decode(errors='replace')
        parts = re.split(r'[\r\n]', self.outBuf)
        self.outBuf = parts.pop()
        for line in parts:
            self.handleLine(line)

    def handleLine(self, line):
        if not line.strip() or '[INFO]' in line:
            return
        m = ITERATION.search(line)
        if m:
            self.strehl.append(float(m.group(2)))
            self.strehlCurve.setData(np.arange(1, len(self.strehl) + 1), self.strehl)
            self.lblStatus.setText(f'running: {line.strip()}')
            return
        m = CALIB_RESULT.search(line)
        if m:
            corr, strehl = float(m.group(3)), float(m.group(4))
            if corr < 0.3:
                self.warning = (f'Calibration correlation {corr:.3f} (good: > 0.5). Check the '
                                f'pixels per lambda/D, the crop and the poke amplitude.')
            elif strehl < 0.5:
                self.warning = (f'Calibrated on an aberrated PSF (Strehl ~ {strehl:.2f}): '
                                f'calibrate with the DM flat, the test aberration cleared.')
        self.append(line)

    def finished(self, code, _status):
        if self.outBuf:
            self.handleLine(self.outBuf)
        self.append(f'[{self.procKind} finished, exit code {code}]')
        self.proc = None
        self.procKind = None
        self.setBusy(None)
        self.refreshFiles()
        if self.warning:
            QMessageBox.warning(self, 'F&F', self.warning)
            self.warning = None

    def stopProcess(self):
        if self.proc is not None:
            # SIGINT: the loop stops cleanly, a calibration restores its output
            os.kill(int(self.proc.processId()), signal.SIGINT)

    def setBusy(self, kind):
        idle = kind is None
        for b in (self.btnBasis, self.btnCalib, self.btnStart, self.btnZero):
            b.setEnabled(idle)
        self.btnStop.setEnabled(not idle)
        self.btnHold.setEnabled(kind == 'run')
        if idle:
            self.btnHold.setChecked(False)
        self.lblStatus.setText('idle' if idle else f'{kind}...')

    # --------------------------------------------------------------- steps
    def buildBasis(self):
        if self.missingShms(('map', 'm2a')):
            return
        args =['basis', self.path('ffBasis.npz'), '-m', self.value('map'), '-z', self.value('m2a'),
                '-M', str(self.value('modes')), '-r', str(self.value('res')),
                '-c', str(self.value('coupling'))]
        if self.value('diameter') > 0:
            args += ['-D', str(self.value('diameter'))]
        self.launch('basis', args)

    def calibrate(self):
        if not os.path.exists(self.path('ffBasis.npz')):
            QMessageBox.warning(self, 'F&F', 'Build the basis first')
            return
        if self.missingShms(('camera', 'background', 'output', 'm2a', 'im')):
            return
        args = ['calibrate', self.path('ffCalib.npz')] + self.common() + [
            '-a', str(self.value('amp')), '-k', str(self.value('calModes'))]
        if self.value('fitSamp'):
            args.append('-f')
        self.launch('calibrate', args)

    def startLoop(self):
        for name, what in (('ffBasis.npz', 'Build the basis'), ('ffCalib.npz', 'Calibrate')):
            if not os.path.exists(self.path(name)):
                QMessageBox.warning(self, 'F&F', f'{what} first')
                return
        if self.missingShms(('camera', 'background', 'output', 'm2a', 'im')):
            return
        self.refreshFiles()
        c = self.calib
        if int(c['size']) != self.value('size'):
            QMessageBox.warning(self, 'F&F', f'The calibration was done with a crop of '
                                f'{int(c["size"])} px: calibrate again, or set it back')
            return
        if not self.value('fitSamp') and abs(c['samp'] - self.value('samp')) > 1e-3 * c['samp']:
            QMessageBox.warning(self, 'F&F', f'The calibration was done with {c["samp"]:.4g} px '
                                f'per lambda/D, the field says {self.value("samp"):.4g}: the loop '
                                f'uses the calibration\'s. Calibrate again, or set it back')
            return
        if c['correlation'] < 0.3 and QMessageBox.question(
                self, 'F&F', f'The calibration\'s correlation is {c["correlation"]:.3f} (good: '
                f'> 0.5): the sampling, the crop or the pokes are likely wrong, the loop will '
                f'not converge. Start anyway?') != QMessageBox.Yes:
            return
        self.createControlShms()
        phaseName = f'{self.value("ctrlPrefix")}Phase.im.shm'
        args = ['run'] + self.common() + [
            '-C', self.path('ffCalib.npz'), '-g', str(self.value('gain')),
            '-l', str(self.value('leak')), '-k', str(self.value('iterations')),
            '-G', self.ctrl['gain'][0], '-K', self.ctrl['leak'][0], '-e', self.ctrl['enable'][0],
            '-p', phaseName, '-s', self.path('ffTheta.npy')]
        if self.value('restore'):
            args.append('-R')
        self.strehl = []
        self.strehlCurve.setData([], [])
        self.phaseShm = None
        self.phaseName = phaseName
        self.launch('run', args)

    def zeroOutput(self):
        name = self.value('output')
        if QMessageBox.question(self, 'F&F', f'Write zeros to {name}?\n(not for a WFS reference)') \
                != QMessageBox.Yes:
            return
        self.writeZeros(name)

    def writeZeros(self, name):
        try:
            shm = dao.shm(name)
            d = np.asarray(shm.get_data())
            shm.set_data(np.zeros(d.shape, dtype=d.dtype))
            self.append(f'{name} zeroed')
        except Exception as e:
            self.append(f'{name}: {e}')

    def applyAberration(self):
        if self.missingShms(('m2a', 'abChannel')):
            return
        try:
            m2a = np.asarray(dao.shm(self.value('m2a')).get_data(), dtype=np.float64)
            n = min(self.value('modes'), m2a.shape[1])
            c = np.random.default_rng(self.value('abSeed')).normal(size=n) * self.value('abAmp')
            shm = dao.shm(self.value('abChannel'))
            d = np.asarray(shm.get_data())
            shm.set_data((m2a[:, :n] @ c).reshape(d.shape).astype(d.dtype))
            self.append(f'test aberration on {self.value("abChannel")}: {n} modes x '
                        f'{self.value("abAmp")}, seed {self.value("abSeed")}')
        except Exception as e:
            self.append(f'test aberration: {e}')

    def clearAberration(self):
        self.writeZeros(self.value('abChannel'))

    # ------------------------------------------------------------ displays
    def updateDisplays(self):
        try:
            if self.camShm is None or self.camName != self.value('camera'):
                self.camName = self.value('camera')
                self.camShm = dao.shm(self.camName)
            frame = np.asarray(self.camShm.get_data(), dtype=np.float64)
            center = self.center or ff.findCenter(frame)
            img = ff.crop(frame, center, self.value('size'))
            self.psfImg.setImage(np.log10(np.clip(img, 0, None) + 1).T, autoLevels=True)
        except Exception:
            self.camShm = None
        if self.procKind == 'run':
            try:
                if self.phaseShm is None:
                    self.phaseShm = dao.shm(self.phaseName)
                self.phaseImg.setImage(np.asarray(self.phaseShm.get_data()).T, autoLevels=True)
            except Exception:
                self.phaseShm = None

    def append(self, text):
        self.log.appendPlainText(text)

    def closeEvent(self, event):
        self.saveSettings()
        if self.proc is not None:
            self.stopProcess()
            self.proc.waitForFinished(5000)
        event.accept()


def main():
    try:
        opts, _ = getopt.getopt(sys.argv[1:], 'hi:o:z:m:w:', ['light', 'help'])
    except getopt.GetoptError as e:
        print(e)
        print(__doc__)
        sys.exit(2)
    keys = {'-i': 'camera', '-o': 'output', '-z': 'm2a', '-m': 'map', '-w': 'workDir'}
    overrides, light = {}, False
    for opt, arg in opts:
        if opt in ('-h', '--help'):
            print(__doc__)
            sys.exit(0)
        elif opt == '--light':
            light = True
        else:
            overrides[keys[opt]] = arg
    app = QApplication(sys.argv)
    app.setStyleSheet(make_stylesheet(light))
    pg.setConfigOption('background', '#f0f0f0' if light else '#1e1e1e')
    pg.setConfigOption('foreground', '#000000' if light else '#cccccc')
    win = Main(overrides, light)
    win.show()
    sys.exit(app.exec_())


if __name__ == '__main__':
    main()
