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
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(path, 'daoDmDisp.ui'))

COLORMAPS = {
    'grey':    [(0,0,0),(255,255,255)],
    'viridis': [(68,1,84),(59,82,139),(33,145,140),(94,201,98),(253,231,37)],
    'inferno': [(0,0,4),(120,28,109),(238,71,41),(252,177,57),(252,255,164)],
    'plasma':  [(13,8,135),(126,3,168),(204,71,120),(248,149,64),(240,249,33)],
    'bwr':     [(0,60,200),(255,255,255),(200,30,30)],
}
DIVERGING_CMAPS = {'bwr'}

def make_colormap(name):
    colors = COLORMAPS.get(name, COLORMAPS['grey'])
    return pg.ColorMap(np.linspace(0, 1, len(colors)), colors)


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
        self.map    = dao.shm(f'/tmp/{mapName}.im.shm')

        self.dmMask = self.map.get_data()
        self.dmM  = np.copy(self.dmMask).astype(np.float32)
        self.dm1M = np.copy(self.dmMask).astype(np.float32)
        self.dm2M = np.copy(self.dmMask).astype(np.float32)
        self.dm3M = np.copy(self.dmMask).astype(np.float32)
        self.dm4M = np.copy(self.dmMask).astype(np.float32)

        self.dmM [self.dmMask == 1] = self.shmdm.get_data()[:,0]
        self.dm1M[self.dmMask == 1] = self.shmdm1.get_data()[:,0]
        self.dm2M[self.dmMask == 1] = self.shmdm2.get_data()[:,0]
        self.dm3M[self.dmMask == 1] = self.shmdm3.get_data()[:,0]
        self.dm4M[self.dmMask == 1] = self.shmdm4.get_data()[:,0]

        for vb_name, gv_name in [('vb',  'graphicsView'),
                                  ('vb1', 'graphicsView_1'),
                                  ('vb2', 'graphicsView_2'),
                                  ('vb3', 'graphicsView_3'),
                                  ('vb4', 'graphicsView_4')]:
            gv = getattr(self, gv_name)
            vb = pg.ViewBox()
            vb.setAspectLocked(True)
            vb.setDefaultPadding(0)
            vb.setBorder(None)
            gv.setCentralItem(vb)
            setattr(self, vb_name, vb)

        for img_name in ('img', 'img1', 'img2', 'img3', 'img4'):
            img = pg.ImageItem()
            getattr(self, 'vb' + img_name[3:]).addItem(img)
            setattr(self, img_name, img)

        self.img.setImage(self.dmM)
        self.img1.setImage(self.dm1M)
        self.img2.setImage(self.dm2M)
        self.img3.setImage(self.dm3M)
        self.img4.setImage(self.dm4M)

        self.titleLabel.setText(f'DM Display — {name}  (map: {mapName})')
        self.chanLabel1.setText(f'{name}00 — Loop')
        self.chanLabel2.setText(f'{name}01 — Pokes')
        self.chanLabel3.setText(f'{name}02 — Flat')
        self.chanLabel4.setText(f'{name}03 — Disturbance')
        self.chanLabelMain.setText(f'{name} — Combined')

        self.cmapName = 'grey'
        for cmapName in COLORMAPS:
            self.cmapCombo.addItem(cmapName)
        self.cmapCombo.setCurrentText(self.cmapName)
        self.cmapCombo.currentTextChanged.connect(self.ChangeColormap)
        for img in (self.img, self.img1, self.img2, self.img3, self.img4):
            img.setColorMap(make_colormap(self.cmapName))

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
        for s in (self.shmdm, self.shmdm1, self.shmdm2, self.shmdm3, self.shmdm4):
            s.set_data(zeroCmd)

    def ChangeColormap(self, name):
        self.cmapName = name
        cm = make_colormap(name)
        for img in (self.img, self.img1, self.img2, self.img3, self.img4):
            img.setColorMap(cm)
        self.Update()

    def SetImage(self, imgItem, data):
        if self.cmapName in DIVERGING_CMAPS:
            # Center a diverging colormap on zero so push/pull actuator strokes are symmetric.
            vmax = float(max(abs(data.min()), abs(data.max()), 1e-12))
            imgItem.setImage(data, levels=(-vmax, vmax), autoLevels=False)
        else:
            imgItem.setImage(data)

    @QtCore.pyqtSlot()
    def Update(self):
        self.dmM [self.dmMask == 1] = self.shmdm.get_data()[:,0]
        self.dm1M[self.dmMask == 1] = self.shmdm1.get_data()[:,0]
        self.dm2M[self.dmMask == 1] = self.shmdm2.get_data()[:,0]
        self.dm3M[self.dmMask == 1] = self.shmdm3.get_data()[:,0]
        self.dm4M[self.dmMask == 1] = self.shmdm4.get_data()[:,0]

        self.SetImage(self.img,  self.dmM)
        self.SetImage(self.img1, self.dm1M)
        self.SetImage(self.img2, self.dm2M)
        self.SetImage(self.img3, self.dm3M)
        self.SetImage(self.img4, self.dm4M)

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
    main.setWindowTitle(f'DM Display — {name}')
    main.show()
    main.Start()
    sys.exit(app.exec_())