#!/usr/bin/env python3

from PyQt5.uic import loadUiType
import sys, getopt
from PyQt5 import QtCore
from PyQt5.QtWidgets import QApplication
import numpy as np
import pyqtgraph as pg
import time
import os

import dao

pg.setConfigOptions(imageAxisOrder='row-major')

# get the directory of ui files
path = os.getenv('DAOROOT') + '/data/'
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(path, 'daoShDisp.ui'))


class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, shmimName, shmcentName, shmrefName, shmpupName):
        super(Main, self).__init__()
        self.setupUi(self)

        self.log = False

        # ---------------------------------------------------------------------
        # Left image view
        # ---------------------------------------------------------------------
        self.imPlot = pg.PlotItem()
        self.graphicsView_image.setCentralItem(self.imPlot)
        self.imPlot.setAspectLocked()
        self.imPlot.invertY(True)

        self.imItem = pg.ImageItem()
        self.imPlot.addItem(self.imItem)

        self.refPlot = pg.PlotDataItem(
            pen=None,
            symbol='+',
            symbolPen=pg.mkPen('g'),
            symbolBrush=None,
            symbolSize=10
        )
        self.imPlot.addItem(self.refPlot)

        self.centPlot = pg.PlotDataItem(
            pen=None,
            symbol='o',
            symbolPen=pg.mkPen('r'),
            symbolBrush=None,
            symbolSize=6
        )
        self.imPlot.addItem(self.centPlot)

        self.imHist = pg.HistogramLUTItem()
        self.imHist.setImageItem(self.imItem)
        self.graphicsView_imageLUT.setCentralItem(self.imHist)
        self.imHist.vb.setMouseEnabled(y=False)

        # ---------------------------------------------------------------------
        # Right intensity map view
        # ---------------------------------------------------------------------
        self.intPlot = pg.PlotItem()
        self.graphicsView_intensity.setCentralItem(self.intPlot)
        self.intPlot.setAspectLocked()
        self.intPlot.invertY(True)

        self.intItem = pg.ImageItem()
        self.intPlot.addItem(self.intItem)

        self.intHist = pg.HistogramLUTItem()
        self.intHist.setImageItem(self.intItem)
        self.graphicsView_intensityLUT.setCentralItem(self.intHist)
        self.intHist.vb.setMouseEnabled(y=False)

        # ---------------------------------------------------------------------
        # Connect checkbox
        # ---------------------------------------------------------------------
        self.checkBoxLog.setCheckState(False)
        self.checkBoxLog.clicked.connect(self.CheckBoxLogCB)

        # ---------------------------------------------------------------------
        # SHM objects
        # ---------------------------------------------------------------------
        print("Image      :", shmimName)
        print("Centroids  :", shmcentName)
        print("References :", shmrefName)
        print("Pupil mask :", shmpupName)

        self.shmim = dao.shm(shmimName)
        self.shmcent = dao.shm(shmcentName)
        self.shmref = dao.shm(shmrefName)
        self.shmpup = dao.shm(shmpupName)

        self.pup = self.shmpup.get_data()
        self.intMap = np.zeros(self.pup.shape, dtype=np.float32)

        cent = self.shmcent.get_data()
        self.nCent = cent.shape[0] // 4

        # first image / first counters
        self.initializeDisplay()

        self.imCnt1 = self.shmim.get_counter()
        self.t1 = time.time()

        # timer
        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(100)
        self.timer.timeout.connect(self.Update)

    def CheckBoxLogCB(self):
        """Check box callback."""
        if self.checkBoxLog.isChecked():
            self.log = True
        else:
            self.log = False

    def Start(self):
        """Start timer."""
        self.timer.start()

    def Stop(self):
        """Stop timer."""
        self.timer.stop()

    def initializeDisplay(self):
        """Initialize display."""
        im = self.shmim.get_data()
        cref = self.shmref.get_data()
        cent = self.shmcent.get_data()

        ny, nx = im.shape

        self.imItem.setImage(im, autoLevels=True)
        self.imPlot.setRange(QtCore.QRectF(0, 0, nx, ny))

        self.updateSpotOverlay(cref, cent)
        self.updateIntensityMap(cent)

        self.intItem.setImage(self.intMap, autoLevels=True)
        self.intPlot.setRange(QtCore.QRectF(0, 0, self.intMap.shape[1], self.intMap.shape[0]))

        self.maxLabel.setText("max: %.3f" % np.max(im))

    def updateSpotOverlay(self, cref, cent):
        """Update reference and centroid overlays."""
        nCent = cent.shape[0] // 4

        xref = np.asarray(cref[:nCent, 0], dtype=np.float32)
        yref = np.asarray(cref[nCent:2 * nCent, 0], dtype=np.float32)

        xcur = xref + np.asarray(cent[:nCent, 0], dtype=np.float32)
        ycur = yref + np.asarray(cent[nCent:2 * nCent, 0], dtype=np.float32)

        self.refPlot.setData(x=xref, y=yref)
        self.centPlot.setData(x=xcur, y=ycur)

    def updateIntensityMap(self, cent):
        """Rebuild intensity map from centroid SHM and pupil mask."""
        nCent = cent.shape[0] // 4
        intVect = np.asarray(cent[2 * nCent:3 * nCent, 0], dtype=np.float32)

        self.intMap.fill(0.0)
        self.intMap[self.pup == 1] = intVect

        if self.log:
            disp = np.array(self.intMap, dtype=np.float32, copy=True)
            disp[disp <= 0] = 1e-12
            disp = np.log(disp)
        else:
            disp = self.intMap

        self.intItem.setImage(disp, autoLevels=False)

    @QtCore.pyqtSlot()
    def Update(self):
        """Update the GUI."""
        try:
            cref = self.shmref.get_data()
            cent = self.shmcent.get_data()
            im = self.shmim.get_data()

            self.nCent = cent.shape[0] // 4

            if self.log:
                imDisp = np.array(im, dtype=np.float32, copy=True)
                imDisp[imDisp <= 0] = 1e-12
                imDisp = np.log(imDisp)
            else:
                imDisp = im

            self.imItem.setImage(imDisp, autoLevels=False)
            self.maxLabel.setText("max: %.3f" % np.max(im))

            self.updateSpotOverlay(cref, cent)
            self.updateIntensityMap(cent)

            self.imCnt2 = self.shmim.get_counter()
            self.t2 = time.time()
            elapsedTime = self.t2 - self.t1
            nbImage = self.imCnt2 - self.imCnt1

            if elapsedTime > 0:
                self.freqLabel.setText('%.2f Hz' % (nbImage / elapsedTime))
            else:
                self.freqLabel.setText('0.00 Hz')

            self.t1 = self.t2
            self.imCnt1 = self.imCnt2

        except Exception as err:
            self.freqLabel.setText("update error")
            print("Update error:", err)


if __name__ == '__main__':
    shmimName = '/tmp/image.im.shm'
    shmcentName = '/tmp/centroids.im.shm'
    shmrefName = '/tmp/references.im.shm'
    shmpupName = '/tmp/pupMask1.im.shm'

    try:
        opts, args = getopt.getopt(
            sys.argv[1:],
            "hi:c:r:p:",
            ["help", "shmimName=", "shmcentName=", "shmrefName=", "shmpupName="]
        )
    except getopt.GetoptError:
        print('err, usage: daoShDisp.py -i <shmimName> -c <shmcentName> -r <shmrefName> -p <shmpupName>')
        sys.exit(2)

    for opt, arg in opts:
        if opt == '-h':
            print('daoShDisp.py -i <shmimName> -c <shmcentName> -r <shmrefName> -p <shmpupName>')
            sys.exit()
        elif opt in ("-i", "--shmimName"):
            shmimName = str(arg)
        elif opt in ("-c", "--shmcentName"):
            shmcentName = str(arg)
        elif opt in ("-r", "--shmrefName"):
            shmrefName = str(arg)
        elif opt in ("-p", "--shmpupName"):
            shmpupName = str(arg)

    app = QApplication([])
    main = Main(shmimName, shmcentName, shmrefName, shmpupName)
    main.setWindowTitle(shmimName)
    main.show()
    main.Start()
    sys.exit(app.exec_())