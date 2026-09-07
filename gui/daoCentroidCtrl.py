#!/usr/bin/env python3
"""
Centroid Threshold Control - live control of the relative-centroider's threshold SHM

Threshold is a fraction of each subaperture's local max (relativeThreshold =
threshold * localMax), as used by daoComputeCentroidRelative / RelativeRef, so
it is a value in [0, 1] rather than a raw ADU count.

Usage: daoCentroidCtrl.py [options]

Options:
  -t  Threshold shm   (default: /tmp/shThreshold.im.shm)
  -i  WFS image shm, for the max/mean readout only (default: /tmp/shIm.im.shm)
  --light  Light mode (default: dark)

Examples:
  daoCentroidCtrl.py
  daoCentroidCtrl.py -t /tmp/shThreshold.im.shm -i /tmp/shIm.im.shm
  daoCentroidCtrl.py --light
"""

from PyQt5.uic import loadUiType
import sys
import getopt
import os
import numpy as np
from PyQt5 import QtCore
from PyQt5.QtWidgets import QApplication
import dao

path = os.getenv('DAOROOT') + '/data/'
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(path, 'daoCentroidCtrl.ui'))


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color: #f0f0f0; color: #000000; }
            QLabel { color: #000000; }
            QDoubleSpinBox {
                background-color: #ffffff; color: #000000; border: 1px solid #aaa;
            }
            QSlider::groove:horizontal { background: #d0d0d0; height: 6px; border-radius: 3px; }
            QSlider::handle:horizontal {
                background: #4a9eff; width: 14px; height: 14px; margin: -4px 0; border-radius: 7px;
            }
            QSlider::sub-page:horizontal { background: #4a9eff; border-radius: 3px; }
        """
    else:
        return """
            QMainWindow, QWidget { background-color: #1e1e1e; color: #cccccc; }
            QLabel { color: #cccccc; }
            QDoubleSpinBox {
                background-color: #3a3a3a; color: #cccccc; border: 1px solid #555;
            }
            QSlider::groove:horizontal { background: #3a3a3a; height: 6px; border-radius: 3px; }
            QSlider::handle:horizontal {
                background: #4a9eff; width: 14px; height: 14px; margin: -4px 0; border-radius: 7px;
            }
            QSlider::sub-page:horizontal { background: #4a9eff; border-radius: 3px; }
        """


# Threshold is a fraction of each subaperture's local max (relativeThreshold = threshold * localMax,
# see daoCentroidSpotsRelative in src/c/daoTools.c), so it lives in [0, 1]. The slider is an int, so
# it works in thousandths.
SLIDER_SCALE = 1000
SLIDER_MAX   = 1 * SLIDER_SCALE


class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, thresholdShmName, imgShmName):
        super(Main, self).__init__()
        self.setupUi(self)

        self.shmThreshold = dao.shm(thresholdShmName)

        self.shmImg = None
        if imgShmName:
            try:
                self.shmImg = dao.shm(imgShmName)
            except Exception:
                self.shmImg = None
                self.imgStatsLabel.setText(f'image shm unavailable: {imgShmName}')

        self.thresholdSpin.setRange(0.0, 1.0)
        self.thresholdSpin.setDecimals(3)
        self.thresholdSpin.setSingleStep(0.01)
        self.thresholdSpin.setSuffix(' × peak')

        self.thresholdSlider.setRange(0, SLIDER_MAX)

        current = float(self.shmThreshold.get_data()[0, 0])
        self._setControls(current)

        self.thresholdSpin.valueChanged.connect(self.OnSpinChanged)
        self.thresholdSlider.valueChanged.connect(self.OnSliderChanged)

        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(200)
        self.timer.timeout.connect(self.Update)

    def _setControls(self, value):
        """Set spin+slider to value without re-triggering the SHM write."""
        self.thresholdSpin.blockSignals(True)
        self.thresholdSlider.blockSignals(True)
        self.thresholdSpin.setValue(value)
        self.thresholdSlider.setValue(int(round(value * SLIDER_SCALE)))
        self.thresholdSpin.blockSignals(False)
        self.thresholdSlider.blockSignals(False)

    def ApplyThreshold(self, value):
        self.shmThreshold.set_data(np.array([[value]], dtype=np.float32))

    def OnSpinChanged(self, value):
        self.thresholdSlider.blockSignals(True)
        self.thresholdSlider.setValue(int(round(value * SLIDER_SCALE)))
        self.thresholdSlider.blockSignals(False)
        self.ApplyThreshold(value)

    def OnSliderChanged(self, value):
        fvalue = value / SLIDER_SCALE
        self.thresholdSpin.blockSignals(True)
        self.thresholdSpin.setValue(fvalue)
        self.thresholdSpin.blockSignals(False)
        self.ApplyThreshold(fvalue)

    def Start(self):
        self.timer.start()

    def Stop(self):
        self.timer.stop()

    @QtCore.pyqtSlot()
    def Update(self):
        # Pick up changes made by another instance of this GUI, or by a script.
        current = float(self.shmThreshold.get_data()[0, 0])
        if abs(current - self.thresholdSpin.value()) > 1e-6:
            self._setControls(current)

        if self.shmImg is not None:
            try:
                im = self.shmImg.get_data()
                self.imgStatsLabel.setText(f'image max: {float(im.max()):.1f}   mean: {float(im.mean()):.1f}')
            except Exception:
                pass


if __name__ == '__main__':
    thresholdShmName = '/tmp/shThreshold.im.shm'
    imgShmName       = '/tmp/shIm.im.shm'
    light            = False
    try:
        opts, args = getopt.getopt(sys.argv[1:], "ht:i:", ["help", "threshold=", "img=", "light"])
    except getopt.GetoptError:
        print('err, usage: daoCentroidCtrl.py -t <thresholdShm> -i <imgShm> [--light]')
        sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoCentroidCtrl.py -t <thresholdShm> -i <imgShm> [--light]')
            sys.exit()
        elif opt in ("-t", "--threshold"):
            thresholdShmName = arg
        elif opt in ("-i", "--img"):
            imgShmName = arg
        elif opt == '--light':
            light = True

    app = QApplication([])
    app.setStyleSheet(make_stylesheet(light))

    main = Main(thresholdShmName, imgShmName)
    main.setWindowTitle(f'Centroid Threshold — {thresholdShmName}')
    main.show()
    main.Start()
    sys.exit(app.exec_())
