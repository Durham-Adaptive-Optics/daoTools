#!/usr/bin/env python3

from PyQt5.uic import loadUiType
import sys
from PyQt5 import QtCore
from PyQt5.QtWidgets import QApplication
import numpy as np
import pyqtgraph as pg
import time
import dao
import os

path = os.getenv('DAOROOT') + '/data/'
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(path, 'daoDmDisp6.ui'))


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color: #f0f0f0; color: #000000; }
            QPushButton {
                background-color: #e0e0e0; color: #000000;
                border: 1px solid #aaa; padding: 4px 8px; border-radius: 3px;
            }
            QPushButton:hover   { background-color: #d0d0d0; }
            QPushButton:pressed { background-color: #bbb; }
            QCheckBox, QLabel   { color: #000000; }
            QDoubleSpinBox, QComboBox {
                background-color: #ffffff; color: #000000; border: 1px solid #aaa;
            }
        """
    else:
        return """
            QMainWindow, QWidget { background-color: #1e1e1e; color: #cccccc; }
            QPushButton {
                background-color: #3a3a3a; color: #cccccc;
                border: 1px solid #555; padding: 4px 8px; border-radius: 3px;
            }
            QPushButton:hover   { background-color: #4a4a4a; }
            QPushButton:pressed { background-color: #555; }
            QCheckBox, QLabel   { color: #cccccc; }
            QCheckBox::indicator { width: 14px; height: 14px; }
            QCheckBox::indicator:unchecked { background-color: #aaaaaa; border: 1px solid #ccc; }
            QDoubleSpinBox, QComboBox {
                background-color: #3a3a3a; color: #cccccc; border: 1px solid #555;
            }
        """


class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, name, mapName):
        super(Main, self).__init__()
        self.setupUi(self)
        self.scale = 1000

        self.shmdm  = dao.shm(f'/tmp/{name}.im.shm')
        self.shmdm1 = dao.shm(f'/tmp/{name}00.im.shm')
        self.shmdm2 = dao.shm(f'/tmp/{name}01.im.shm')
        self.shmdm3 = dao.shm(f'/tmp/{name}02.im.shm')
        self.shmdm4 = dao.shm(f'/tmp/{name}03.im.shm')
        self.shmdm5 = dao.shm(f'/tmp/{name}04.im.shm')
        self.shmdm6 = dao.shm(f'/tmp/{name}05.im.shm')
        self.map    = dao.shm(f'/tmp/{mapName}.im.shm')

        self.dmMask = self.map.get_data()
        self.dmM  = np.copy(self.dmMask).astype(np.float32)
        self.dm1M = np.copy(self.dmMask).astype(np.float32)
        self.dm2M = np.copy(self.dmMask).astype(np.float32)
        self.dm3M = np.copy(self.dmMask).astype(np.float32)
        self.dm4M = np.copy(self.dmMask).astype(np.float32)
        self.dm5M = np.copy(self.dmMask).astype(np.float32)
        self.dm6M = np.copy(self.dmMask).astype(np.float32)

        self.dmM [self.dmMask == 1] = self.shmdm.get_data()[:,0]
        self.dm1M[self.dmMask == 1] = self.shmdm1.get_data()[:,0]
        self.dm2M[self.dmMask == 1] = self.shmdm2.get_data()[:,0]
        self.dm3M[self.dmMask == 1] = self.shmdm3.get_data()[:,0]
        self.dm4M[self.dmMask == 1] = self.shmdm4.get_data()[:,0]
        self.dm5M[self.dmMask == 1] = self.shmdm5.get_data()[:,0]
        self.dm6M[self.dmMask == 1] = self.shmdm6.get_data()[:,0]

        for vb_name, gv_name in [('vb',  'graphicsView'),
                                  ('vb1', 'graphicsView_1'),
                                  ('vb2', 'graphicsView_2'),
                                  ('vb3', 'graphicsView_3'),
                                  ('vb4', 'graphicsView_4'),
                                  ('vb5', 'graphicsView_5'),
                                  ('vb6', 'graphicsView_6')]:
            gv = getattr(self, gv_name)
            vb = pg.ViewBox()
            vb.setAspectLocked(True)
            vb.setDefaultPadding(0)
            vb.setBorder(None)
            gv.setCentralItem(vb)
            setattr(self, vb_name, vb)

        for img_name in ('img', 'img1', 'img2', 'img3', 'img4', 'img5', 'img6'):
            img = pg.ImageItem()
            getattr(self, 'vb' + img_name[3:]).addItem(img)
            setattr(self, img_name, img)

        self.img.setImage(self.dmM)
        self.img1.setImage(self.dm1M)
        self.img2.setImage(self.dm2M)
        self.img3.setImage(self.dm3M)
        self.img4.setImage(self.dm4M)
        self.img5.setImage(self.dm5M)
        self.img6.setImage(self.dm6M)

        self.pushButton.toggle()
        self.pushButton.clicked.connect(self.ResetAll)

        self.imCnt1 = self.shmdm.get_counter()
        self.timer  = QtCore.QTimer(self)
        self.timer.setInterval(100)
        self.timer.timeout.connect(self.Update)
        self.t1 = time.time()

    def Start(self):
        self.timer.start()

    def Stop(self):
        self.timer.stop()

    def ResetAll(self):
        zeroCmd = self.shmdm.get_data() * 0
        for s in (self.shmdm, self.shmdm1, self.shmdm2, self.shmdm3, self.shmdm4, self.shmdm5, self.shmdm6):
            s.set_data(zeroCmd)

    @QtCore.pyqtSlot()
    def Update(self):
        self.dmM [self.dmMask == 1] = self.shmdm.get_data()[:,0]
        self.dm1M[self.dmMask == 1] = self.shmdm1.get_data()[:,0]
        self.dm2M[self.dmMask == 1] = self.shmdm2.get_data()[:,0]
        self.dm3M[self.dmMask == 1] = self.shmdm3.get_data()[:,0]
        self.dm4M[self.dmMask == 1] = self.shmdm4.get_data()[:,0]
        self.dm5M[self.dmMask == 1] = self.shmdm5.get_data()[:,0]
        self.dm6M[self.dmMask == 1] = self.shmdm6.get_data()[:,0]

        self.img.setImage(self.dmM)
        self.img1.setImage(self.dm1M)
        self.img2.setImage(self.dm2M)
        self.img3.setImage(self.dm3M)
        self.img4.setImage(self.dm4M)
        self.img5.setImage(self.dm5M)
        self.img6.setImage(self.dm6M)

        self.minLabel.setText(str(np.min(self.dmM)))
        self.maxLabel.setText(str(np.max(self.dmM)))

        self.imCnt2 = self.shmdm.get_counter()
        self.t2 = time.time()
        self.t1 = self.t2
        self.imCnt1 = self.imCnt2


if __name__ == '__main__':
    import getopt
    name     = 'dmCmd'
    mapName  = 'dmMap'
    light    = False
    try:
        opts, args = getopt.getopt(sys.argv[1:], "hs:m:", ["help", "name=", "map=", "light"])
    except getopt.GetoptError:
        print('err, usage: daoDmDisp.py -s <name> -m <map> [--light]')
        sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoDmDisp.py -s <name> -m <map> [--light]')
            sys.exit()
        elif opt in ("-s", "--name"):
            name = arg
        elif opt in ("-m", "--map"):
            mapName = arg
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

    main = Main(name, mapName)
    main.setWindowTitle(name)
    main.show()
    main.Start()
    sys.exit(app.exec_())