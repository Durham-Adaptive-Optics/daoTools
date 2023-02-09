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
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtGui


class UpdateThread_Image(QtCore.QThread):
    """
    QThread to update plot. Taken from AOplot.
    """
    # Create signals:
    # (a) for updating the plot when new data arrives:
    updateSignal       = QtCore.pyqtSignal(object)

    def __init__(self, shm_filename, refresh=10):
        super(UpdateThread_Image, self).__init__()
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
        self.updateSignal.emit( data)

class UpdateThread_subApGrid(QtCore.QThread):
    """
    QThread to update plot. Taken from AOplot.
    """
    # Create signals:
    # (a) for updating the plot when new data arrives:
    updateSignal       = QtCore.pyqtSignal(object)

    def __init__(self, shm_filename):
        super(UpdateThread_subApGrid, self).__init__()
        self.shm_filename = shm_filename
        self.shm = daoShm.shm(self.shm_filename)

        subApMap = self.shm.get_data()

        self.A = []
        for i in subApMap:
            self.A.append(plt.addRect(i[0],i[1],i[2], i[3]))
        # print(A)
        # self.emitUpdateSignal(A)
        

    # This function is called when you say "updateThread.start()":
    def run(self):
        while self.isRunning:
            subApMap = self.shm.get_data(check=True)

            t = 0
            for i in subApMap:
                print("New data")
                self.A[t].setRect(i[0], i[1], i[2], i[3])
                t+=1
            pass

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
        self.updateSignal.emit( data)

if __name__=="__main__":
    parser = argparse.ArgumentParser(description='Script to shmPublisher')
    parser.add_argument('-f', '--filename',
                        dest='shm_filename',
                        default='/tmp/image.im.shm',
                        help='Shared memory filename to be published into')  
    parser.add_argument('-s', '--filename_sub_ap',
                        dest='shm_filename_subs',
                        default='/tmp/subs.im.shm',
                        help='Shared memory filename to be published into')  
    parser.add_argument('-r', '--refresh',
                        dest='refresh',
                        default='60',
                        help='How often to update the display in (Hz)')                      
    args = parser.parse_args()

    shm_filename = str(args.shm_filename).strip()
    shm_filename_subs = args.shm_filename_subs
    print(f"SHM_image:{shm_filename}@{args.refresh} Hz")
    if(shm_filename_subs is not None):
        print(f"SubApMap:{shm_filename_subs}")

    # check if it exists
    if not exists(shm_filename):
        print(f"Error: File does not exist, unable to read from.")
        print(f"       Could not open {shm_filename}")
        exit()

    if(shm_filename_subs is not None):
        if not exists(shm_filename_subs):
            print(f"Error: File does not exist, unable to read from.")
            print(f"       Could not open {shm_filename_subs}")
            exit()

    app = magicplot.pyqtgraph.mkQApp()
    plt = magicplot.MagicPlot()
    im = plt.getImageItem()
    plt.show()
    t = UpdateThread_Image(shm_filename,refresh=int(args.refresh))
    t.updateSignal.connect(im.setData)

    if shm_filename_subs is not None:
        t2 = UpdateThread_subApGrid(shm_filename_subs)
        # t2.updateSignal.connect(im.setData)


    t.start()
    t2.start()
    app.exec_()
    t.exit()




