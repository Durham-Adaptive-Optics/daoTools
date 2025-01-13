#!/usr/bin/env python3

from PyQt5.uic import loadUiType
import sys, getopt
from PyQt5 import QtGui
from PyQt5 import QtCore
from PyQt5.QtWidgets import QApplication
import numpy as np
import pyqtgraph as pg
import time
import os

import dao

# get the directory of ui files
path = os.getenv('DAOROOT')+'/data/'
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(path,'daoImDisp.ui'))

class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, shmimName):
        super(Main, self).__init__()
        self.setupUi(self)
        self.vb = pg.ViewBox()
        self.graphicsView.setCentralItem(self.vb)
        self.vb.setAspectLocked()
        self.img = pg.ImageItem()
        #self.img.setLevels([0, 100])
        self.vb.addItem(self.img)
        # Contrast/color control
        self.hist = pg.HistogramLUTItem()
        self.hist.setImageItem(self.img)
        self.graphicsView_2.setCentralItem(self.hist)        

        self.hist.vb.setMouseEnabled(y=False)  # Disable auto-scaling

        # Create SHM object
        print(shmimName)
        self.shmim = dao.shm(shmimName)
        # set display according to SHM size
        print(str(self.shmim.get_meta_data()['size'][0])+','+str(self.shmim.get_meta_data()['size'][1]))
        self.vb.setRange(QtCore.QRectF(0, 0, self.shmim.get_meta_data()['size'][0], self.shmim.get_meta_data()['size'][1]))
        # Get first image
        im = self.shmim.get_data()
        self.img.setImage(np.rot90(im.reshape(im.shape[1], im.shape[0]),3))
        # save first counter
        self.imCnt1 = self.shmim.get_meta_data()['cnt0']
        # Create QT timer to update display
        self.timer  = QtCore.QTimer(self)
        # Throw event timeout with an interval of 50 milliseconds
        self.timer.setInterval(100) 
        # each time timer counts done call self.Update
        self.timer.timeout.connect(self.Update) 
        # Create callback to on/off checkbox
        self.checkBox1.setCheckState(False)
        self.checkBox1.clicked.connect(self.CheckBox1CB)
        # save initial time for frequency disply purpose
        self.t1=time.time()
        self.log=False

    def CheckBox1CB(self):
        """ Check Box callback """
        if self.checkBox1.isChecked():
            self.log=True
        else:
            self.log=False

    def Start(self):
        """ Start timer """
        self.timer.start()

    def Stop(self):
        """ Stop timer """
        self.timer.stop()

    @QtCore.pyqtSlot()
    def Update(self):
        """ Update the GUI """
        # Update the displayed image with the data of the SHM
        im=self.shmim.get_data()
        if self.log==True:
            im=np.log(im)
        self.img.setImage(np.rot90(im.reshape(im.shape[1], im.shape[0]),3))
        # get the counter
        self.imCnt2 = self.shmim.get_counter()
#        sys.stdout.write("\r%d"%self.imCnt2)
#        sys.stdout.flush()
        self.t2=time.time()
        ellapsedTime = self.t2-self.t1
        nbImage = self.imCnt2-self.imCnt1
        # display frequency
        self.freqLabel.setText(str('%.2f Hz' %(nbImage/ellapsedTime)))
        self.t1=self.t2
        self.imCnt1=self.imCnt2

if __name__ == '__main__':
    shmimName = '/tmp/ws00image.im.shm'
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hs:",["help", "shmimName="])
    except getopt.GetoptError:
      print('err, usage: imDisp.py -s <shmimName>')
      sys.exit(2)
    print(opts)
    print(args)
    for opt, arg in opts:
        if opt == '-h':
            print('imDisp.py -s <shmimName>')
            sys.exit()
        elif opt in ("-s", "--shm"):
            shmimName = str(arg)
    app = QApplication([])
    main = Main(shmimName)
    main.setWindowTitle(shmimName)
    main.show()
    main.Start()
    sys.exit(app.exec_())

