#!/usr/bin/env python3
"""
Loop Display - generic AO loop control panel

Close/open a loop, set its leaky-integrator gain and leak, and enable/disable
the integrator itself. Talks only to plain scalar control SHMs (lpCmd/lpGain/
lpLeak by default) - nothing here is tied to a specific pipeline; point it at
any loop's SHMs with -c/-g/-l/-e.

Usage: daoLoopDisp.py [options]

Options:
  -c  Loop state shm: 0 = open, 1 = closed (default: /tmp/lpCmd.im.shm)
  -g  Loop gain shm (default: /tmp/lpGain.im.shm)
  -l  Loop leak shm (default: /tmp/lpLeak.im.shm)
  -e  Enable shm: 0 = integrator disabled, 1 = enabled (default: derived from
      -c as <base>Enable.im.shm, e.g. lpCmd.im.shm -> lpCmdEnable.im.shm -
      matches daoLeakyIntegrator(Map)'s own -e default)
  --light  Light mode (default: dark)

Any SHM that doesn't exist yet is skipped (greyed out / shown as "--") and
retried automatically once it appears, so this can be started before or
after the processes that create those SHMs.

Examples:
  daoLoopDisp.py
  daoLoopDisp.py -c /tmp/lpCmd.im.shm -g /tmp/lpGain.im.shm -l /tmp/lpLeak.im.shm
  daoLoopDisp.py -e /tmp/lpCmdEnable.im.shm
  daoLoopDisp.py --light
"""

import os
import sys
import getopt

import numpy as np
import pyqtgraph as pg
from PyQt5.uic import loadUiType
from PyQt5.QtWidgets import QApplication
from pyqtgraph.Qt import QtCore
import dao

path = os.getenv('DAOROOT') + '/data/'
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(path, 'daoLoopDisp.ui'))


def _derive_enable_shm_name(base):
    """<name>.im.shm -> <name>Enable.im.shm, same convention as the C-side
    daoToolsInsertShmNamePrefix() helper used by daoLeakyIntegrator(Map)."""
    suffix = ".im.shm"
    if base.endswith(suffix):
        return base[:-len(suffix)] + "Enable" + suffix
    return base + "Enable"


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color: #f0f0f0; color: #000000; }
            QLabel { color: #000000; }
            QPushButton {
                background-color: #e0e0e0; color: #000000;
                border: 1px solid #aaa; padding: 4px 8px; border-radius: 3px;
            }
            QPushButton:hover   { background-color: #d0d0d0; }
            QPushButton:pressed { background-color: #bbb; }
            QDoubleSpinBox, QSpinBox {
                background-color: #ffffff; color: #000000; border: 1px solid #aaa;
            }
            QGroupBox {
                border: 1px solid #aaa; border-radius: 4px;
                margin-top: 8px; color: #000000;
            }
            QGroupBox::title { subcontrol-origin: margin; left: 8px; color: #555555; }
        """
    else:
        return """
            QMainWindow, QWidget { background-color: #1e1e1e; color: #cccccc; }
            QLabel { color: #cccccc; }
            QPushButton {
                background-color: #3a3a3a; color: #cccccc;
                border: 1px solid #555555; padding: 4px 8px; border-radius: 3px;
            }
            QPushButton:hover   { background-color: #4a4a4a; }
            QPushButton:pressed { background-color: #555555; }
            QDoubleSpinBox, QSpinBox {
                background-color: #3a3a3a; color: #cccccc; border: 1px solid #555555;
            }
            QGroupBox {
                border: 1px solid #555555; border-radius: 4px;
                margin-top: 8px; color: #cccccc;
            }
            QGroupBox::title { subcontrol-origin: margin; left: 8px; color: #aaaaaa; }
        """


def _open(path):
    """Best-effort attach: None (rather than an exception) if the SHM doesn't
    exist yet - the update loop keeps retrying so start order doesn't matter."""
    try:
        return dao.shm(path) if path and os.path.exists(path) else None
    except Exception:
        return None


class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, shmCmdName, shmGainName, shmLeakName, shmEnableName):
        super(Main, self).__init__()
        self.setupUi(self)

        self.shmCmdName, self.shmGainName, self.shmLeakName = shmCmdName, shmGainName, shmLeakName
        self.shmEnableName = shmEnableName
        self.shmCmd = self.shmGain = self.shmLeak = self.shmEnable = None

        self.closeLoopButton.clicked.connect(lambda: self.setLoop(1))
        self.openLoopButton.clicked.connect(lambda: self.setLoop(0))
        self.gainButton.clicked.connect(self.setGain)
        self.leakButton.clicked.connect(self.setLeak)
        self.enableCheck.toggled.connect(self.setEnable)

        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(200)
        self.timer.timeout.connect(self.Update)

    def Start(self):
        self.timer.start()

    def Stop(self):
        self.timer.stop()

    # ------------------------------------------------------------------
    def _connect(self):
        if self.shmCmd is None:
            self.shmCmd = _open(self.shmCmdName)
        if self.shmGain is None:
            self.shmGain = _open(self.shmGainName)
        if self.shmLeak is None:
            self.shmLeak = _open(self.shmLeakName)
        if self.shmEnable is None:
            self.shmEnable = _open(self.shmEnableName)
        ok = self.shmCmd is not None
        for w in (self.closeLoopButton, self.openLoopButton, self.gainButton, self.leakButton):
            w.setEnabled(ok)
        self.enableCheck.setEnabled(self.shmEnable is not None)

    def setLoop(self, state):
        if self.shmCmd is not None:
            self.shmCmd.set_data(self.shmCmd.get_data() * 0 + int(state))

    def setGain(self):
        if self.shmGain is not None:
            self.shmGain.set_data(self.shmGain.get_data() * 0 + self.gainSpin.value())

    def setLeak(self):
        if self.shmLeak is not None:
            self.shmLeak.set_data(self.shmLeak.get_data() * 0 + self.leakSpin.value())

    def setEnable(self, checked):
        if self.shmEnable is not None:
            self.shmEnable.set_data(self.shmEnable.get_data() * 0 + int(checked))

    # ------------------------------------------------------------------
    @QtCore.pyqtSlot()
    def Update(self):
        self._connect()

        closed = False
        if self.shmCmd is not None:
            try:
                closed = bool(np.ravel(self.shmCmd.get_data())[0])
            except Exception:
                self.shmCmd = None
        self.loopStateLabel.setText("CLOSED" if closed else "OPEN")
        self.loopStateLabel.setStyleSheet(
            "color:#ff5555;font-weight:bold;" if closed else "color:#55dd55;font-weight:bold;")
        self.closeLoopButton.setStyleSheet("background-color:#2e7d32;" if closed else "")
        self.openLoopButton.setStyleSheet("" if closed else "background-color:#2e7d32;")

        if self.shmGain is not None:
            try:
                self.gainStateLabel.setText("current: %.3f" % float(np.ravel(self.shmGain.get_data())[0]))
            except Exception:
                self.shmGain = None
        if self.shmLeak is not None:
            try:
                self.leakStateLabel.setText("current: %.4f" % float(np.ravel(self.shmLeak.get_data())[0]))
            except Exception:
                self.shmLeak = None

        if self.shmEnable is not None:
            try:
                enabled = bool(np.ravel(self.shmEnable.get_data())[0])
                if self.enableCheck.isChecked() != enabled:
                    # Reflect external changes (another GUI, the process
                    # itself) without re-triggering setEnable() -> a write
                    # back to the shm on every poll.
                    self.enableCheck.blockSignals(True)
                    self.enableCheck.setChecked(enabled)
                    self.enableCheck.blockSignals(False)
            except Exception:
                self.shmEnable = None


if __name__ == '__main__':
    shmCmdName    = '/tmp/lpCmd.im.shm'
    shmGainName   = '/tmp/lpGain.im.shm'
    shmLeakName   = '/tmp/lpLeak.im.shm'
    shmEnableName = ''   # empty means "derive from shmCmdName below"
    light = False
    try:
        opts, args = getopt.getopt(sys.argv[1:], "hc:g:l:e:",
                                    ["help", "shmCmdName=", "shmGainName=", "shmLeakName=",
                                     "shmEnableName=", "light"])
    except getopt.GetoptError:
        print('err, usage: daoLoopDisp.py -c <shmCmdName> -g <shmGainName> -l <shmLeakName> '
              '[-e <shmEnableName>] [--light]')
        sys.exit(2)
    for opt, arg in opts:
        if opt in ('-h', '--help'):
            print(__doc__)
            sys.exit()
        elif opt in ("-c", "--shmCmdName"):
            shmCmdName = str(arg)
        elif opt in ("-g", "--shmGainName"):
            shmGainName = str(arg)
        elif opt in ("-l", "--shmLeakName"):
            shmLeakName = str(arg)
        elif opt in ("-e", "--shmEnableName"):
            shmEnableName = str(arg)
        elif opt == '--light':
            light = True

    if not shmEnableName:
        shmEnableName = _derive_enable_shm_name(shmCmdName)

    if light:
        pg.setConfigOption('background', '#f0f0f0')
        pg.setConfigOption('foreground', '#000000')
    else:
        pg.setConfigOption('background', '#1e1e1e')
        pg.setConfigOption('foreground', '#cccccc')

    app = QApplication([])
    app.setStyleSheet(make_stylesheet(light))

    main = Main(shmCmdName, shmGainName, shmLeakName, shmEnableName)
    main.setWindowTitle('Loop  [%s]' % os.path.basename(shmCmdName))
    main.show()
    main.Start()
    sys.exit(app.exec_())
