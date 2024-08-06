#!/usr/bin/env python3

from PyQt5.uic import loadUiType
import sys, getopt
from PyQt5 import QtGui
from PyQt5 import QtCore
from PyQt5.QtWidgets import QApplication

import numpy as np
import pyqtgraph as pg
import time
import dao
import os
from matplotlib import cm

path = os.getenv('DAOROOT')+'/data/'
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(path,'daoDmDisp.ui'))

class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, name, mapName):
        super(Main, self).__init__()
        self.setupUi(self)
        self.scale=1000
        # Create SHM object
        self.shmdm = dao.shm('/tmp/'+name+'.im.shm')
        self.shmdm1 = dao.shm('/tmp/'+name+'00.im.shm')
        self.shmdm2 = dao.shm('/tmp/'+name+'01.im.shm')
        self.shmdm3 = dao.shm('/tmp/'+name+'02.im.shm')
        self.shmdm4 = dao.shm('/tmp/'+name+'03.im.shm')
        self.map = dao.shm('/tmp/'+mapName+'.im.shm')
        self.dmMask=self.map.get_data();
        self.dmM = np.copy(self.map.get_data()).astype(np.float32)
        self.dm1M = np.copy(self.map.get_data()).astype(np.float32)
        self.dm2M = np.copy(self.map.get_data()).astype(np.float32)
        self.dm3M = np.copy(self.map.get_data()).astype(np.float32)
        self.dm4M = np.copy(self.map.get_data()).astype(np.float32)
        # Get first image
        print(self.dmMask)
        self.dmM[self.dmMask == 1] = self.shmdm.get_data()[:,0]
        self.dm1M[self.dmMask == 1] = self.shmdm1.get_data()[:,0]
        self.dm2M[self.dmMask == 1] = self.shmdm2.get_data()[:,0]
        self.dm3M[self.dmMask == 1] = self.shmdm3.get_data()[:,0]
        self.dm4M[self.dmMask == 1] = self.shmdm4.get_data()[:,0]

        self.vb = pg.ViewBox()
        self.graphicsView.setCentralItem(self.vb)
        self.img = pg.ImageItem()
        self.vb.addItem(self.img)
        self.vb.setAspectLocked(True)
        
        self.vb1 = pg.ViewBox()
        self.graphicsView_1.setCentralItem(self.vb1)
        self.img1 = pg.ImageItem()
        self.vb1.addItem(self.img1)
        self.vb1.setAspectLocked(True)

        self.vb2 = pg.ViewBox()
        self.graphicsView_2.setCentralItem(self.vb2)
        self.img2 = pg.ImageItem()
        self.vb2.addItem(self.img2)
        self.vb2.setAspectLocked(True)

        self.vb3 = pg.ViewBox()
        self.graphicsView_3.setCentralItem(self.vb3)
        self.img3 = pg.ImageItem()
        self.vb3.addItem(self.img3)
        self.vb3.setAspectLocked(True)

        self.vb4 = pg.ViewBox()
        self.graphicsView_4.setCentralItem(self.vb4)
        self.img4 = pg.ImageItem()
        self.vb4.addItem(self.img4)
        self.vb4.setAspectLocked(True)

        self.img.setImage(self.dmM)
        self.img1.setImage(self.dm1M)
        self.img2.setImage(self.dm2M)
        self.img3.setImage(self.dm3M)
        self.img4.setImage(self.dm4M)

        self.pushButton.toggle()
        self.pushButton.clicked.connect(self.ResetAll)
        # save first counter
        self.imCnt1 = self.shmdm.get_counter()
        # Create QT timer to update display
        self.timer  = QtCore.QTimer(self)
        # Throw event timeout with an interval of 50 milliseconds
        self.timer.setInterval(100) 
        # each time timer counts done call self.Update
        self.timer.timeout.connect(self.Update) 
        # save initial time for frequency disply purpose
        self.t1=time.time()

    def Start(self):
        """ Start timer """
        self.timer.start()

    def Stop(self):
        """ Stop timer """
        self.timer.stop()

    def ResetAll(self):
        zeroCmd = self.shmdm.get_data()*0;
        self.shmdm.set_data(zeroCmd)
        self.shmdm1.set_data(zeroCmd)
        self.shmdm2.set_data(zeroCmd)
        self.shmdm3.set_data(zeroCmd)
        self.shmdm4.set_data(zeroCmd)

    @QtCore.pyqtSlot()
    def Update(self):
        """ Update the GUI """
        # Update the displayed image with the data of the SHM
        self.dmM[self.dmMask == 1] = self.shmdm.get_data()[:,0]
        self.dm1M[self.dmMask == 1] = self.shmdm1.get_data()[:,0]
        self.dm2M[self.dmMask == 1] = self.shmdm2.get_data()[:,0]
        self.dm3M[self.dmMask == 1] = self.shmdm3.get_data()[:,0]
        self.dm4M[self.dmMask == 1] = self.shmdm4.get_data()[:,0]
        self.img.setImage(self.dmM)
        self.img1.setImage(self.dm1M)
        self.img2.setImage(self.dm2M)
        self.img3.setImage(self.dm3M)
        self.img4.setImage(self.dm4M)
        self.minLabel.setText(str(np.min(self.dmM)))
        self.maxLabel.setText(str(np.max(self.dmM)))

        # get the counter
        self.imCnt2 = self.shmdm.get_counter()
        self.t2=time.time()
        ellapsedTime = self.t2-self.t1
        nbImage = self.imCnt2-self.imCnt1
        self.t1=self.t2
        self.imCnt1=self.imCnt2

if __name__ == '__main__':
    name = 'dmCmd'
    mapName = 'dmMap'
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hs:m:",["help", "name=", "map="])
    except getopt.GetoptError:
      print('err, usage: dmDisp.py -s <name> -m <map>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('dmDisp.py -s <name> -m <map>')
            sys.exit()
        elif opt in ("-s", "--name"):
            name = arg
        elif opt in ("-m", "--map"):
            mapName = arg

    app = QApplication([])
    main = Main(name, mapName)
    main.setWindowTitle(''+name+'')
    main.show()
    main.Start()
    sys.exit(app.exec_())
