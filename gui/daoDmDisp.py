#!/usr/bin/env python3

from PyQt5.uic import loadUiType
import sys, getopt
from PyQt5 import QtGui
from PyQt5 import QtCore
from PyQt5.QtWidgets import QApplication

import numpy as np
import pyqtgraph as pg
from scipy import ndimage
import time
import dao
import os
import poppy
from matplotlib import cm
#from scipy.misc.pilutil import toimage
import scipy.ndimage
from PIL import Image
from PIL.ImageQt import ImageQt


def nparrayToQPixmap(arrayImage):
    pilImage = Image.fromarray(np.uint8(cm.gray(arrayImage)*255))
    qtImage = ImageQt(pilImage)
    qImage = QtGui.QImage(qtImage)
    qPixmap = QtGui.QPixmap(qImage)
    return qPixmap

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
#        self.dmMaskCirc=poppy.zernike.zernike(0,0,dm.mapArray.shape[0],outside=0.0)
        self.dmMaskCirc=self.map.get_data();
        self.dmMaskHex=np.rot90(poppy.zernike.hex_aperture(self.map.get_data().shape[0]))
        self.dmMask = self.dmMaskCirc # default circular mask
        self.dmM = np.copy(self.map.get_data()).astype(np.float32)
        self.dm1M = np.copy(self.map.get_data()).astype(np.float32)
        self.dm2M = np.copy(self.map.get_data()).astype(np.float32)
        self.dm3M = np.copy(self.map.get_data()).astype(np.float32)
        self.dm4M = np.copy(self.map.get_data()).astype(np.float32)
        # Get first image
        self.dmM[self.dmMask == 1] = self.shmdm.get_data()
        self.dm1M[self.dmMask == 1] = self.shmdm1.get_data()
        self.dm2M[self.dmMask == 1] = self.shmdm2.get_data()
        self.dm3M[self.dmMask == 1] = self.shmdm3.get_data()
        self.dm4M[self.dmMask == 1] = self.shmdm4.get_data()
        self.label.setPixmap(nparrayToQPixmap(scipy.ndimage.zoom(self.dmM,10, order=0)))
        self.label1.setPixmap(nparrayToQPixmap(scipy.ndimage.zoom(self.dm1M),10, order=0)))
        self.label2.setPixmap(nparrayToQPixmap(scipy.ndimage.zoom(self.dm2M),10, order=0)))
        self.label3.setPixmap(nparrayToQPixmap(scipy.ndimage.zoom(self.dm3M),10, order=0)))
        self.label4.setPixmap(nparrayToQPixmap(scipy.ndimage.zoom(self.dm4M),10, order=0)))
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
        self.dmM[self.dmMask == 1] = self.shmdm.get_data()
        self.dm1M[self.dmMask == 1] = self.shmdm1.get_data()
        self.dm2M[self.dmMask == 1] = self.shmdm2.get_data()
        self.dm3M[self.dmMask == 1] = self.shmdm3.get_data()
        self.dm4M[self.dmMask == 1] = self.shmdm4.get_data()
        self.label.setPixmap(nparrayToQPixmap(scipy.ndimage.zoom(self.dmM,10, order=0)))
        self.label1.setPixmap(nparrayToQPixmap(scipy.ndimage.zoom(self.dm1M,10, order=0)))
        self.label2.setPixmap(nparrayToQPixmap(scipy.ndimage.zoom(self.dm2M,10, order=0)))
        self.label3.setPixmap(nparrayToQPixmap(scipy.ndimage.zoom(self.dm3M,10, order=0)))
        self.label4.setPixmap(nparrayToQPixmap(scipy.ndimage.zoom(self.dm4M,10, order=0)))
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

    app = QtGui.QApplication([])
    main = Main(name, mapName)
    main.setWindowTitle(''+name+'')
    main.show()
    main.Start()
    sys.exit(app.exec_())
