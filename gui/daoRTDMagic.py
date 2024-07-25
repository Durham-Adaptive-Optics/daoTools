#!/usr/bin/env python3

import daoShm
import magicplot
import time
import sys
from os.path import exists
import numpy as np
import argparse
# from threading import Thread, Event

from PyQt5 import QtCore


class UpdateThread(QtCore.QThread):
    """
    QThread to update plot. Taken from AOplot.
    """
    # Create signals:
    # (a) for updating the plot when new data arrives:
    updateSignal       = QtCore.pyqtSignal(object)

    def __init__(self, shm_filename, refresh=10):
        super(UpdateThread, self).__init__()
        self.shm_filename = shm_filename
        self.shm = daoShm.shm(self.shm_filename)
        self.refresh =refresh
        self.updateTime = 1/refresh
        self.counter = self.shm.get_counter()
        self.newCounter = 0
        self.cycles = 0
        self.maxCycles = refresh
        self.emitUpdateSignal(self.shm.get_data())
        

    # This function is called when you say "updateThread.start()":
    def run(self):
        while self.isRunning:
            self.emitUpdateSignal(self.shm.get_data())
            time.sleep(self.updateTime)
            self.newCounter= self.shm.get_counter()
            diff = self.newCounter - self.counter
            if(self.cycles >= self.maxCycles):
                frameRate = diff/ (self.updateTime*self.cycles)
                print(f"display@ {self.refresh:0.2f} Hz reception@ {frameRate:0.2f} Hz", end="\r")
                self.counter = self.newCounter
                self.cycles = 0
            else:
                self.cycles+=1

    # This function is called by the RTC everytime new data is ready:
    def emitUpdateSignal(self, data ):
        """
        Emit the signal saying that new data is ready, and pass the data to the
        slot that is connected to this signal.

        Args:
            data : ["data", streamname, (data, frame time, frame number)]
                   this is the structure of a data package coming from darc

        Returns:
            nothing
        """
        data[0,0]=0
        self.updateSignal.emit(np.rot90(data, k=3))
 
if __name__=="__main__":
    parser = argparse.ArgumentParser(description='Script to shmPublisher')
    parser.add_argument('-f', '--filename',
                        dest='shm_filename',
                        default='/tmp/image.im.shm',
                        help='Shared memory filename to be published into')  
    parser.add_argument('-r', '--refresh',
                        dest='refresh',
                        default='60',
                        help='How often to update the display in (Hz)')                      
    args = parser.parse_args()

    shm_filename = str(args.shm_filename).strip()
    print(f"SHM_image:{shm_filename}@{args.refresh} Hz")

    # check if it exists
    if not exists(shm_filename):
        print(f"Error: File does not exist, unable to read from.")
        print(f"       Could not open {shm_filename}")
        exit()

    app = magicplot.pyqtgraph.mkQApp()
    plt = magicplot.MagicPlot()
    im = plt.getImageItem()
    plt.show()
    t = UpdateThread(shm_filename,refresh=int(args.refresh))
    t.updateSignal.connect(im.setData)
    t.start()
    app.exec_()
    t.exit()




