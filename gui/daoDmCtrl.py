#!/usr/bin/env python3
"""
DM Channel Control - modal control of one DM command channel through its M2A matrix

Drives one DM command channel (-s) through the M2A (modes-to-actuator) modal
basis: each column of M2A is one mode, expressed as an actuator command
vector. The DM map is used to scatter that vector back into a 2D image, so
no analytic (Zernike/poppy) basis is needed -- whatever basis M2A actually
holds (Zernike, KL, Fourier, a custom IF set, ...) is what the sliders drive
and what the previews show. The 10 sliders set the amplitude (volt) of the
first 10 M2A modes; Reset zeroes them.

Opening the GUI shows the channel's current content without touching it;
the channel is only written once a slider is moved.

Usage: daoDmCtrl.py [options]

Options:
  -i  DM identifier, window title only (default: 1)
  -s  DM command channel shm to drive, 468x1-style float32 (default: /tmp/dm.im.shm)
  -m  DM actuator map shm: binary mask of the controlled actuators (default: /tmp/dmMap.im.shm)
  -z  Modes-to-actuator (M2A) matrix shm, nAct x nModes float32 (default: /tmp/dmM2A.im.shm)
  --light  Light mode (default: dark)

Examples:
  daoDmCtrl.py
  daoDmCtrl.py -s /tmp/dmCmd01.im.shm -m /tmp/dmMap.im.shm -z /tmp/dmM2A.im.shm
  daoDmCtrl.py --light
"""

from PyQt5.uic import loadUiType
import sys
import getopt
import os
import numpy as np
from PyQt5.QtWidgets import QApplication
from pyqtgraph.Qt import QtCore
import pyqtgraph as pg
import dao

path = os.getenv('DAOROOT') + '/data/'
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(path, 'daoDmCtrl.ui'))

MODE_CMAP = 'viridis'


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color: #f0f0f0; color: #000000; }
            QLabel, QCheckBox { color: #000000; }
            QPushButton {
                background-color: #e0e0e0; color: #000000;
                border: 1px solid #aaa; padding: 3px 6px; border-radius: 3px;
            }
            QPushButton:hover   { background-color: #d0d0d0; }
            QPushButton:pressed { background-color: #bbb; }
            QSlider::groove:horizontal { background: #d0d0d0; height: 6px; border-radius: 3px; }
            QSlider::handle:horizontal {
                background: #4a9eff; width: 14px; height: 14px; margin: -4px 0; border-radius: 7px;
            }
            QSlider::sub-page:horizontal { background: #4a9eff; border-radius: 3px; }
        """
    else:
        return """
            QMainWindow, QWidget { background-color: #1e1e1e; color: #cccccc; }
            QLabel, QCheckBox { color: #cccccc; }
            QPushButton {
                background-color: #3a3a3a; color: #cccccc;
                border: 1px solid #555; padding: 3px 6px; border-radius: 3px;
            }
            QPushButton:hover   { background-color: #4a4a4a; }
            QPushButton:pressed { background-color: #555; }
            QSlider::groove:horizontal { background: #3a3a3a; height: 6px; border-radius: 3px; }
            QSlider::handle:horizontal {
                background: #4a9eff; width: 14px; height: 14px; margin: -4px 0; border-radius: 7px;
            }
            QSlider::sub-page:horizontal { background: #4a9eff; border-radius: 3px; }
        """


NMODES = 10  # number of sliders / modes exposed by the GUI


class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, dmId, shmDmName, shmMapName, shmM2aName):
        super(Main, self).__init__()
        self.setupUi(self)

        self.dmId = dmId
        self.scale = 100.0

        self.shmdm  = dao.shm(shmDmName)
        self.shmMap = dao.shm(shmMapName)
        self.shmM2a = dao.shm(shmM2aName)

        self.dmMap = self.shmMap.get_data()
        self.mapH, self.mapW = self.dmMap.shape
        self.nAct = int((self.dmMap == 1).sum())

        # M2A: shape (nAct, nModes). Column k is mode k as an actuator command,
        # whatever basis it was built from (Zernike, KL, Fourier, custom IFs, ...).
        self.m2a = np.asarray(self.shmM2a.get_data(), dtype=np.float32)
        if self.m2a.shape[0] != self.nAct:
            # tolerate a transposed matrix
            self.m2a = self.m2a.T
        self.nAvail = min(NMODES, self.m2a.shape[1])

        # per-mode amplitude (volt), driven by the sliders
        self.z = np.zeros(NMODES, dtype=np.float32)

        self.vbMain = pg.ViewBox()
        self.graphicsView.setCentralItem(self.vbMain)
        self.vbMain.setAspectLocked(True)
        self.imgMain = pg.ImageItem()
        self.imgMain.setColorMap(MODE_CMAP)
        self.vbMain.addItem(self.imgMain)
        self.vbMain.setRange(QtCore.QRectF(0, 0, self.mapW, self.mapH))

        self.hist = pg.HistogramLUTItem()
        self.hist.setImageItem(self.imgMain)
        self.hist.gradient.loadPreset(MODE_CMAP)
        self.hist.setLevels(-0.1, 0.1)
        self.graphicsView_11.setCentralItem(self.hist)

        # one small preview per mode, built from the M2A column + DM map
        self.modeViews = []
        self.modeImgs = []
        for k in range(NMODES):
            gv = getattr(self, 'graphicsView_%d' % (k + 1))
            vb = pg.ViewBox()
            gv.setCentralItem(vb)
            vb.setAspectLocked(True)
            img = pg.ImageItem()
            img.setColorMap(MODE_CMAP)
            vb.addItem(img)
            self.modeViews.append(vb)
            self.modeImgs.append(img)
            img.setImage(self._modeImage(k))

        # accumulated command shape shown in the main view
        self.modeShape = np.full((self.mapH, self.mapW), np.nan, dtype=np.float32)
        self.imgMain.setImage(np.nan_to_num(self.modeShape), autoLevels=False)

        for k in range(NMODES):
            slider = getattr(self, 'horizontalSlider_%d' % (k + 1))
            slider.valueChanged.connect(self._makeSliderCB(k))
            if k >= self.nAvail:
                slider.setEnabled(False)
                getattr(self, 'label_%d' % (k + 1)).setText('n/a')

        self.pushButton.clicked.connect(self.ResetZ)
        self.autoScaleCheckBox.toggled.connect(lambda _: self._setMainImage(self.modeShape))

        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(500)
        self.timer.timeout.connect(self.Update)

        # Show the channel's current content without touching the DM: opening
        # the GUI must not write (reset) the channel.
        self._showCurrentChannel()

    def _modeImage(self, k):
        """2D image of mode k: scatter its M2A column through the DM map."""
        img = np.full((self.mapH, self.mapW), np.nan, dtype=np.float32)
        if k < self.m2a.shape[1]:
            img[self.dmMap == 1] = self.m2a[:, k]
        return img

    def _makeSliderCB(self, k):
        def cb():
            amplitude = float(getattr(self, 'horizontalSlider_%d' % (k + 1)).value())
            volt = amplitude / self.scale
            getattr(self, 'label_%d' % (k + 1)).setText('%.2f volt' % volt)
            self.z[k] = np.float32(volt)
            self.SendToDM()
        return cb

    def ResetZ(self):
        for k in range(NMODES):
            getattr(self, 'horizontalSlider_%d' % (k + 1)).setValue(0)
        self.z[:] = 0
        self.SendToDM()

    def _setMainImage(self, shape2d):
        """Push a 2D shape to the main view, honouring the auto-scale toggle.

        Auto-scale on : stretch the colour map to the actuator values only
                        (NaN background excluded), so even a small pure tilt
                        fills the full range.
        Auto-scale off: leave the levels wherever the histogram is set, so the
                        user can pin a fixed scale and read absolute amplitude.
        """
        self.imgMain.setImage(np.nan_to_num(shape2d), autoLevels=False)
        if self.autoScaleCheckBox.isChecked():
            finite = shape2d[np.isfinite(shape2d)]
            if finite.size and float(np.ptp(finite)) > 0:
                lo, hi = float(np.min(finite)), float(np.max(finite))
            else:
                lo, hi = -1e-6, 1e-6
            self.imgMain.setLevels((lo, hi))
            self.hist.setLevels(lo, hi)

    def _showCurrentChannel(self):
        """Render the channel's current command in the main view (read only)."""
        cur = np.nan_to_num(self.shmdm.get_data())
        self.modeShape[:] = np.nan
        self.modeShape[self.dmMap == 1] = cur[:, 0]
        self._setMainImage(self.modeShape)

    def SendToDM(self):
        """Combine the active modes into an actuator command vector and push it."""
        cmd = np.zeros((self.nAct, 1), dtype=np.float32)
        for k in range(self.nAvail):
            cmd[:, 0] += self.z[k] * self.m2a[:, k]
        cmd = np.nan_to_num(cmd)
        self.shmdm.set_data(cmd.astype(np.float32))

        self.modeShape[:] = np.nan
        self.modeShape[self.dmMap == 1] = cmd[:, 0]
        self._setMainImage(self.modeShape)

    def Start(self):
        self.timer.start()

    def Stop(self):
        self.timer.stop()

    @QtCore.pyqtSlot()
    def Update(self):
        # As long as no mode is dialed in, track the channel's live content;
        # once the sliders are used, show what this GUI is applying.
        if not np.any(self.z):
            self._showCurrentChannel()
        else:
            self._setMainImage(self.modeShape)


if __name__ == '__main__':
    dmId       = '1'
    shmDmName  = '/tmp/dm.im.shm'
    shmMapName = '/tmp/dmMap.im.shm'
    shmM2aName = '/tmp/dmM2A.im.shm'
    light      = False
    try:
        opts, args = getopt.getopt(sys.argv[1:], "hi:s:m:z:",
                                    ["help", "dmId=", "shmDmName=", "shmMapArray=", "shmM2aName=", "light"])
    except getopt.GetoptError:
        print('err, usage: daoDmCtrl.py -i <dmId> -s <shmDmName> -m <shmMapArray> -z <shmM2aName> [--light]')
        sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoDmCtrl.py -i <dmId> -s <shmDmName> -m <shmMapArray> -z <shmM2aName> [--light]')
            sys.exit()
        elif opt in ("-i", "--dmId"):
            dmId = str(arg)
        elif opt in ("-s", "--shmDmName"):
            shmDmName = str(arg)
        elif opt in ("-m", "--shmMapArray"):
            shmMapName = str(arg)
        elif opt in ("-z", "--shmM2aName"):
            shmM2aName = str(arg)
        elif opt == '--light':
            light = True

    if light:
        pg.setConfigOption('background', '#f0f0f0')
        pg.setConfigOption('foreground', '#000000')
    else:
        pg.setConfigOption('background', '#1e1e1e')
        pg.setConfigOption('foreground', '#cccccc')

    app = QApplication([])
    app.setStyleSheet(make_stylesheet(light))

    main = Main(dmId, shmDmName, shmMapName, shmM2aName)
    main.setWindowTitle('DM%s Channel Control  [%s]' % (dmId, os.path.basename(shmDmName)))
    main.show()
    main.Start()
    sys.exit(app.exec_())
