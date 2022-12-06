#!/usr/bin/env python3

import daoShm
import magicplot
import time
import sys
from os.path import exists
import numpy as np
import argparse

from PyQt5 import QtCore


class UpdateThread(QtCore.QThread):
    """
    QThread to update plot. Taken from AOplot.
    """
    # Create signals:
    # (a) for updating the plot when new data arrives:
    updateSignal       = QtCore.pyqtSignal(object)

    def __init__(self, shm_filename, sempahore_number, display=1):
        super(UpdateThread, self).__init__()
        self.shm_filename = shm_filename
        self.shm = daoShm.shm(self.shm_filename)
        self.start_time = time.time()
        self.end_time = time.time()
        self.semaphore_number = sempahore_number
        self.display= display
        self.work_time = 0
        

    # This function is called when you say "updateThread.start()":
    def run(self):
        while self.isRunning:
            frame = self.shm.get_data(check=True, semNb=self.semaphore_number)
            self.end_time = time.time()
            self.frame_number = self.shm.get_counter()
            self.work_time+=(self.end_time - self.start_time)
            if(self.frame_number % self.display == 0):
                self.emitUpdateSignal(frame)
                avg_frame_time = self.work_time/self.display
                frame_rate = 1/avg_frame_time
                print(f"setting frame: {self.frame_number:08},  avg_frame_time: {avg_frame_time*1000:.2f} ms frame rate: {frame_rate:.2f} Hz", end="\r")
                self.work_time = 0
            self.start_time = time.time()
            

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
    parser.add_argument('-s', '--semaphore',
                        dest='semaphore',
                        default='3',
                        help='The semaphore to read 0 - 9')   
    parser.add_argument('-d', '--display',
                        dest='display',
                        default='100',
                        help='How often to display to cmd line info')                      
    args = parser.parse_args()

    shm_filename = str(args.shm_filename).strip()
    print(f"SHM_image:{shm_filename}")

    # check if it exists
    if not exists(shm_filename):
        print(f"Error: File does not exist, unable to read from.")
        print(f"       Could not open {shm_filename}")
        exit()

    app = magicplot.pyqtgraph.mkQApp()
    plt = magicplot.MagicPlot()
    im = plt.getImageItem()
    plt.show()
    t = UpdateThread(shm_filename, sempahore_number=int(args.semaphore), display=int(args.display))
    t.updateSignal.connect(im.setData)
    t.start()
    app.exec_()
    t.exit()




