#!/usr/bin/env python3

from PyQt5.uic import loadUiType
import sys, getopt
from PyQt5.QtWidgets import QApplication, QVBoxLayout, QHBoxLayout, QWidget
from PyQt5.QtCore import QRunnable, QThreadPool, pyqtSlot, QObject, pyqtSignal
from pyqtgraph.Qt import QtGui, QtCore
import numpy as np
import pyqtgraph as pg

from scipy import ndimage
import time
import os
import poppy
import scipy.io
import dao

pg.setConfigOption('background', '#1e1e1e')
pg.setConfigOption('foreground', '#cccccc')

'''
npix=dmActMap.shape[0]
x = (np.arange(npix, dtype=np.float32) - (npix - 1) / 2.) / ((npix - 1) / 2.)
y=x
xx, yy = np.meshgrid(x, y)
rho = np.sqrt(xx ** 2 + yy ** 2)
theta = np.arctan2(yy, xx)
a=poppy.zernike.zernike(0,0,rho,theta,outside=0.0)
'''
# get the directory of this script
path = os.getenv('DAOROOT') + '/data/'
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(path, 'daoDmCtrl.ui'))

DARK_STYLESHEET = """
    QMainWindow, QWidget {
        background-color: #1e1e1e;
        color: #cccccc;
    }
    QPushButton {
        background-color: #3a3a3a;
        color: #cccccc;
        border: 1px solid #555555;
        padding: 4px 8px;
        border-radius: 3px;
    }
    QPushButton:hover   { background-color: #4a4a4a; }
    QPushButton:pressed { background-color: #555555; }
    QPushButton:checked { background-color: #555555; }
    QCheckBox, QLabel   { color: #cccccc; }
    QDoubleSpinBox, QSpinBox {
        background-color: #3a3a3a;
        color: #cccccc;
        border: 1px solid #555555;
    }
    QComboBox {
        background-color: #3a3a3a;
        color: #cccccc;
        border: 1px solid #555555;
    }
    QSlider::groove:horizontal {
        background: #3a3a3a;
        height: 6px;
        border-radius: 3px;
    }
    QSlider::handle:horizontal {
        background: #4a9eff;
        width: 14px;
        height: 14px;
        margin: -4px 0;
        border-radius: 7px;
    }
    QSlider::sub-page:horizontal {
        background: #4a9eff;
        border-radius: 3px;
    }
    QGroupBox {
        border: 1px solid #555555;
        border-radius: 4px;
        margin-top: 8px;
        color: #cccccc;
    }
    QGroupBox::title {
        subcontrol-origin: margin;
        left: 8px;
        color: #aaaaaa;
    }
    QProgressBar {
        background-color: #3a3a3a;
        color: #cccccc;
        border: 1px solid #555555;
        border-radius: 3px;
    }
    QProgressBar::chunk { background-color: #4a9eff; }
"""

class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, dmId, shmDmName, shmMapName, shmZ2aName):
        super(Main, self).__init__()
        self.setupUi(self)
        self.setStyleSheet(DARK_STYLESHEET)

        self.dmId = dmId
        self.scale = 100
        self.vb   = pg.ViewBox()
        self.vb1  = pg.ViewBox()
        self.vb2  = pg.ViewBox()
        self.vb3  = pg.ViewBox()
        self.vb4  = pg.ViewBox()
        self.vb5  = pg.ViewBox()
        self.vb6  = pg.ViewBox()
        self.vb7  = pg.ViewBox()
        self.vb8  = pg.ViewBox()
        self.vb9  = pg.ViewBox()
        self.vb10 = pg.ViewBox()
        self.graphicsView.setCentralItem(self.vb)
        self.graphicsView_1.setCentralItem(self.vb1)
        self.graphicsView_2.setCentralItem(self.vb2)
        self.graphicsView_3.setCentralItem(self.vb3)
        self.graphicsView_4.setCentralItem(self.vb4)
        self.graphicsView_5.setCentralItem(self.vb5)
        self.graphicsView_6.setCentralItem(self.vb6)
        self.graphicsView_7.setCentralItem(self.vb7)
        self.graphicsView_8.setCentralItem(self.vb8)
        self.graphicsView_9.setCentralItem(self.vb9)
        self.graphicsView_10.setCentralItem(self.vb10)
        self.vb.setAspectLocked()
        self.img   = pg.ImageItem()
        self.img1  = pg.ImageItem()
        self.img2  = pg.ImageItem()
        self.img3  = pg.ImageItem()
        self.img4  = pg.ImageItem()
        self.img5  = pg.ImageItem()
        self.img6  = pg.ImageItem()
        self.img7  = pg.ImageItem()
        self.img8  = pg.ImageItem()
        self.img9  = pg.ImageItem()
        self.img10 = pg.ImageItem()
        self.hist = pg.HistogramLUTItem()
        self.hist.setImageItem(self.img)
        self.hist.setLevels(-0.1, 0.1)
        self.graphicsView_11.setCentralItem(self.hist)
        self.vb.addItem(self.img)
        self.vb1.addItem(self.img1)
        self.vb2.addItem(self.img2)
        self.vb3.addItem(self.img3)
        self.vb4.addItem(self.img4)
        self.vb5.addItem(self.img5)
        self.vb6.addItem(self.img6)
        self.vb7.addItem(self.img7)
        self.vb8.addItem(self.img8)
        self.vb9.addItem(self.img9)
        self.vb10.addItem(self.img10)

        # Create SHM object
        print(shmDmName)
        self.shmdm   = dao.shm(shmDmName)
        self.shmMap  = dao.shm(shmMapName)
        self.shmZ2a  = dao.shm(shmZ2aName)
        self.dmSize  = self.shmMap.get_meta_data()['size'][0]
        print(str(self.shmMap.get_meta_data()['size'][0]) + ',' +
              str(self.shmMap.get_meta_data()['size'][1]))
        self.vb.setRange(QtCore.QRectF(
            0, 0,
            self.shmMap.get_meta_data()['size'][0],
            self.shmMap.get_meta_data()['size'][1]))
        self.dmMaskCirc = poppy.zernike.zernike(0, 0, self.dmSize, outside=0.0)
        self.dmMaskCirc = self.shmMap.get_data()
        self.dmMaskHex  = np.rot90(poppy.zernike.hex_aperture(self.dmSize))
        self.dmMask     = self.dmMaskCirc  # default circular mask

        # Get first image
        cmds = 0 * self.shmMap.get_data().astype(np.float32)
        cmds[self.shmMap.get_data() == 1] = self.shmdm.get_data()[:, 0]
        self.img.setImage(cmds, autoLevels=False)

        self.zernikeBasis = poppy.zernike.zernike_basis(11, self.dmSize + 2, outside=0.0)
        if hasattr(self.zernikeBasis, 'get'):
            # poppy returns cupy arrays when a GPU is available; pyqtgraph/numpy need plain ndarrays
            self.zernikeBasis = self.zernikeBasis.get()
        self.img1.setImage(self.zernikeBasis[1, :, :])
        self.img2.setImage(self.zernikeBasis[2, :, :])
        self.img3.setImage(self.zernikeBasis[3, :, :])
        self.img4.setImage(self.zernikeBasis[4, :, :])
        self.img5.setImage(self.zernikeBasis[5, :, :])
        self.img6.setImage(self.zernikeBasis[6, :, :])
        self.img7.setImage(self.zernikeBasis[7, :, :])
        self.img8.setImage(self.zernikeBasis[8, :, :])
        self.img9.setImage(self.zernikeBasis[9, :, :])
        self.img10.setImage(self.zernikeBasis[10, :, :])

        # save first counter
        self.imCnt1 = self.shmdm.get_meta_data()['cnt0']

        # Create QT timer to update display
        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(500)
        self.timer.timeout.connect(self.Update)

        # sliders
        self.horizontalSlider_1.valueChanged.connect(self.Hs1CB)
        self.horizontalSlider_2.valueChanged.connect(self.Hs2CB)
        self.horizontalSlider_3.valueChanged.connect(self.Hs3CB)
        self.horizontalSlider_4.valueChanged.connect(self.Hs4CB)
        self.horizontalSlider_5.valueChanged.connect(self.Hs5CB)
        self.horizontalSlider_6.valueChanged.connect(self.Hs6CB)
        self.horizontalSlider_7.valueChanged.connect(self.Hs7CB)
        self.horizontalSlider_8.valueChanged.connect(self.Hs8CB)
        self.horizontalSlider_9.valueChanged.connect(self.Hs9CB)
        self.horizontalSlider_10.valueChanged.connect(self.Hs10CB)

        self.t1 = time.time()
        self.zernike       = np.zeros((self.dmSize, self.dmSize, 10))
        self.zernikeTotal  = np.zeros((self.dmSize, self.dmSize))
        self.zernikeVector = np.zeros((self.dmSize * self.dmSize, 1))
        self.z    = np.zeros(10).astype(np.float32)
        self.z2c  = self.shmZ2a.get_data()
        self.SendToDM()

        self.pushButton.toggle()
        self.pushButton.clicked.connect(self.ResetZ)
        self.dmMask = self.dmMaskCirc

    def Hs1CB(self):
        ''' Tip '''
        amplitude = float(self.horizontalSlider_1.value())
        self.label_1.setText(str(amplitude / 100) + ' volt')
        self.zernike[0:self.dmSize, 0:self.dmSize, 0] = \
            amplitude * self.zernikeBasis[1, 1:self.dmSize+1, 1:self.dmSize+1] * self.dmMask / self.scale
        self.z[0] = float(amplitude / 100)
        self.SendToDM()

    def Hs2CB(self):
        ''' Tilt '''
        amplitude = float(self.horizontalSlider_2.value())
        self.label_2.setText(str(amplitude / 100) + ' volt')
        self.zernike[0:self.dmSize, 0:self.dmSize, 1] = \
            amplitude * self.zernikeBasis[2, 1:self.dmSize+1, 1:self.dmSize+1] * self.dmMask / self.scale
        self.z[1] = float(amplitude / 100)
        self.SendToDM()

    def Hs3CB(self):
        amplitude = float(self.horizontalSlider_3.value())
        self.label_3.setText(str(amplitude / 100) + ' volt')
        self.zernike[0:self.dmSize, 0:self.dmSize, 2] = \
            amplitude * self.zernikeBasis[3, 1:self.dmSize+1, 1:self.dmSize+1] * self.dmMask / self.scale
        self.z[2] = float(amplitude / 100)
        self.SendToDM()

    def Hs4CB(self):
        amplitude = float(self.horizontalSlider_4.value())
        self.label_4.setText(str(amplitude / 100) + ' volt')
        self.zernike[0:self.dmSize, 0:self.dmSize, 3] = \
            amplitude * self.zernikeBasis[4, 1:self.dmSize+1, 1:self.dmSize+1] * self.dmMask / self.scale
        self.z[3] = float(amplitude / 100)
        self.SendToDM()

    def Hs5CB(self):
        amplitude = float(self.horizontalSlider_5.value())
        self.label_5.setText(str(amplitude / 100) + ' volt')
        self.zernike[0:self.dmSize, 0:self.dmSize, 4] = \
            amplitude * self.zernikeBasis[5, 1:self.dmSize+1, 1:self.dmSize+1] * self.dmMask / self.scale
        self.z[4] = float(amplitude / 100)
        self.SendToDM()

    def Hs6CB(self):
        amplitude = float(self.horizontalSlider_6.value())
        self.label_6.setText(str(amplitude / 100) + ' volt')
        self.zernike[0:self.dmSize, 0:self.dmSize, 5] = \
            amplitude * self.zernikeBasis[6, 1:self.dmSize+1, 1:self.dmSize+1] * self.dmMask / self.scale
        self.z[5] = float(amplitude / 100)
        self.SendToDM()

    def Hs7CB(self):
        amplitude = float(self.horizontalSlider_7.value())
        self.label_7.setText(str(amplitude / 100) + ' volt')
        self.zernike[0:self.dmSize, 0:self.dmSize, 6] = \
            amplitude * self.zernikeBasis[7, 1:self.dmSize+1, 1:self.dmSize+1] * self.dmMask / self.scale
        self.z[6] = float(amplitude / 100)
        self.SendToDM()

    def Hs8CB(self):
        amplitude = float(self.horizontalSlider_8.value())
        self.label_8.setText(str(amplitude / 100) + ' volt')
        self.zernike[0:self.dmSize, 0:self.dmSize, 7] = \
            amplitude * self.zernikeBasis[8, 1:self.dmSize+1, 1:self.dmSize+1] * self.dmMask / self.scale
        self.z[7] = float(amplitude / 100)
        self.SendToDM()

    def Hs9CB(self):
        amplitude = float(self.horizontalSlider_9.value())
        self.label_9.setText(str(amplitude / 100) + ' volt')
        self.zernike[0:self.dmSize, 0:self.dmSize, 8] = \
            amplitude * self.zernikeBasis[9, 1:self.dmSize+1, 1:self.dmSize+1] * self.dmMask / self.scale
        self.z[8] = float(amplitude / 100)
        self.SendToDM()

    def Hs10CB(self):
        amplitude = float(self.horizontalSlider_10.value())
        self.label_10.setText(str(amplitude / 100) + ' volt')
        self.zernike[0:self.dmSize, 0:self.dmSize, 9] = \
            amplitude * self.zernikeBasis[10, 1:self.dmSize+1, 1:self.dmSize+1] * self.dmMask / self.scale
        self.z[9] = float(amplitude / 100)
        self.SendToDM()

    def ResetZ(self):
        self.horizontalSlider_1.setValue(0)
        self.horizontalSlider_2.setValue(0)
        self.horizontalSlider_3.setValue(0)
        self.horizontalSlider_4.setValue(0)
        self.horizontalSlider_5.setValue(0)
        self.horizontalSlider_6.setValue(0)
        self.horizontalSlider_7.setValue(0)
        self.horizontalSlider_8.setValue(0)
        self.horizontalSlider_9.setValue(0)
        self.horizontalSlider_10.setValue(0)
        self.ReApply()

    def ReApply(self):
        self.Hs1CB()
        self.Hs2CB()
        self.Hs3CB()
        self.Hs4CB()
        self.Hs5CB()
        self.Hs6CB()
        self.Hs7CB()
        self.Hs8CB()
        self.Hs9CB()
        self.Hs10CB()

    def SendToDM(self):
        self.zernikeTotal = np.zeros((self.dmSize, self.dmSize))
        for k in range(0, 10):
            self.zernikeTotal = \
                self.zernikeTotal + self.zernike[0:self.dmSize, 0:self.dmSize, k]
        self.zernikeVector = self.zernikeTotal.reshape(self.dmSize * self.dmSize, 1)
        self.zernikeVector = np.nan_to_num(self.zernikeVector)

        cmd = self.shmdm.get_data() * 0
        for k in range(10):
            cmd[:, 0] += (self.z[k] * self.z2c[:, k]).astype(np.float32)
        self.shmdm.set_data(cmd.astype(np.float32))
        self.Update()

    def Start(self):
        """ Start timer """
        self.timer.start()

    def Stop(self):
        """ Stop timer """
        self.timer.stop()

    @QtCore.pyqtSlot()
    def Update(self):
        """ Update the GUI """
        self.img.setImage(self.zernikeTotal, autoLevels=False)
        self.imCnt2 = self.shmdm.get_meta_data()['cnt0']
        self.t2 = time.time()
        ellapsedTime = self.t2 - self.t1
        nbImage = self.imCnt2 - self.imCnt1
        self.t1 = self.t2
        self.imCnt1 = self.imCnt2


if __name__ == '__main__':
    shmZ2aName = '/tmp/dmM2A.im.shm'
    shmDmName  = '/tmp/dm.im.shm'
    shmMapName = '/tmp/dmMap.im.shm'
    try:
        opts, args = getopt.getopt(sys.argv[1:], "hi:s:m:z:",
                                   ["help", "dmId=", "shmDmName=",
                                    "shmMapArray", "shmZ2aName"])
    except getopt.GetoptError:
        print('err, usage: mattoDmCtrl.py -i <dmId> -s <shmDmName> -m <shmMapArray> -z <z2aShm>')
        sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoDmCtrl.py -i <dmId> -s <shmDmName> -m <shmMapArray> -z <shmZ2aName>')
            sys.exit()
        elif opt in ("-i", "--dmId"):
            dmId = str(arg)
        elif opt in ("-s", "--dmshm"):
            shmDmName = str(arg)
        elif opt in ("-m", "--mapShm"):
            shmMapName = str(arg)
        elif opt in ("-z", "--z2aShm"):
            shmZ2aName = str(arg)
    app = QApplication([])
    main = Main(dmId, shmDmName, shmMapName, shmZ2aName)
    main.setWindowTitle(shmDmName)
    main.show()
    main.Start()
    sys.exit(app.exec_())
