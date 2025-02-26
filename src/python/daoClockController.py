import dao 
import logging
import os
import daoLaunch
import numpy as np
import subprocess


class clockController:

    def __init__(self, clockName, frequnecy, frequnecyName=None, controller="daoClock", tmuxname=None):

        self.log = logging.getLogger(__name__)
        self.shm_file = clockName
        self.path, fullname = os.path.split(self.shm_file)
        filenameparts = fullname.split(".")
        self.filename = filenameparts[0]
        self.ext ="." + ".".join(filenameparts[1:])
        self.controller= controller
        self.frequnecy = frequnecy

        if tmuxname is None:
            self.tmuxname = self.filename
        else:
            self.tmuxname = tmuxname
        
        if frequnecyName is None:
            self.freqName = "Freq"
        else:
            self.freqName = frequnecyName

        self.log.debug(f"self.filename: {self.shm_file}")
        self.log.debug(f"path: {self.path}")
        self.log.debug(f"filename: {self.filename}")
        self.log.debug(f"ext: {self.ext}")
        self.log.debug(f"controller: {self.controller}")
        self.log.debug(f"tmuxname: {self.tmuxname}")
        self.log.debug(f"frequnecy")

    def createShm(self,):
        """
        Creates shared memory segments for various camera parameters.
        """
        print(self.filename)
        self.clockShmName = self.path + '/' + self.filename + self.ext
        self.freqShmName  = self.path + '/' + self.freqName + self.ext
        self.clockShm 	= dao.shm(self.clockShmName, np.zeros((1,1)).astype(np.uint32))
        self.freqShm    = dao.shm(self.freqShmName,  np.zeros((1,1)).astype(np.float32))
        
   
    def loadShm(self,):
        """
        Loads existing shared memory segments for camera parameters.
        """
        self.clockShmName = self.path + '/' + self.filename + self.ext
        self.freqShmName  = self.path + '/' + self.freqName + self.ext
        self.clockShm 	= dao.shm(self.clockShmName)
        self.freqShm    = dao.shm(self.freqShmName)

    def launch(self):
        """
        Launches the camera controller process using TMUX.
        """
        args = f'-L {self.clockShmName} {self.freqShmName} {self.frequnecy}'
        daoLaunch.manage_process(action='launch', tmuxname=self.tmuxname, processExe=self.controller, processArgs=args)

    def kill(self):
        """
        Terminates the camera controller process associated with the TMUX session.
        """
        daoLaunch.manage_process(action='kill', tmuxname=self.tmuxname)

    def setFrequnecy(self, frequnecy):
        self.frequnecy = frequnecy
        self.freqShm.set_data((self.freqShm.get_data()*0+frequnecy).astype(np.float32))

    def getFrequnecy(self):
        return self.freqShm.get_data()
            
if __name__=="__main__":
    log = logging.getLogger(__name__)
    log.setLevel(logging.DEBUG)
    shm = "/tmp/wfsImage.im.shm"
    a = "First Light Imaging-C-BLUE ONE 1.7 MP-01-00001cdab9df"
    # device ="\'First Light Imaging-C-BLUE ONE 1.7 MP-01-00001ee7a6e1\'"
    device = f"\'{a}\'"

    cblue = daoCblue(shm, device)

    # cblue.createShm()
    # cblue.launch()
    # cblue.setFPS(10)

