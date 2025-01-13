import dao
import logging
import os
import daoLaunch
import numpy as np
from daoProcessController import ProcessController 
from daoTools import ShackHartmannWFS



class CentroidController(ProcessController):
    def __init__(self, input_shm, output_shm, process, tmuxname=None, input_shape=(1, 1), output_shape=(1, 1), input_datatype=np.float32, output_datatype=np.float32, subApRef_shm_name=None, threshold_shm=None, subApSize=1, nValidSubAps=1):
        """
        Initialize a processController instance.

        Args:
            input_shm (str): The name of the input shared memory.
            output_shm (str): The name of the output shared memory.
            process (str): The name of the process.
            tmuxname (str, optional): The name of the tmux session. Defaults to None.
            input_shape (tuple, optional): The shape of the input data. Defaults to (1, 1).
            output_shape (tuple, optional): The shape of the output data. Defaults to (1, 1).
            input_datatype (type, optional): The datatype of the input data. Defaults to np.float32.
            output_datatype (type, optional): The datatype of the output data. Defaults to np.float32.
        """
        super().__init__(input_shm, output_shm, process, tmuxname, input_shape, output_shape, input_datatype, output_datatype)
        
        if subApRef_shm_name is None:
            self.subApRef_shm_name = self.path + '/' + self.input_name + 'subApRef' + self.ext
        else:
            self.subApRef_shm_name = subApRef_shm_name
        if threshold_shm is None:
            self.threshold_shm_name = self.path + '/' + self.input_name + 'threshold' + self.ext
        else:
            self.threshold_shm_name = threshold_shm
        
        self.subApSize = subApSize
        self.nValidSubAps = nValidSubAps
        print(f"self.subApRef_shm_name: {self.subApRef_shm_name}")
        print(f"self.threshold_shm_name: {self.threshold_shm_name}")
        print(f"self.subApSize: {self.subApSize}")
        print(f"self.nValidSubAps: {self.nValidSubAps}")
    
    def createShm(self,):
        """
        Creates shared memory segments for various camera parameters.
        """
        super().createShm()
        self.subApRef_shm_name = dao.shm(self.subApRef_shm_name, np.zeros(self.nValidSubAps*2).astype(self.output_datatype))
        self.threshold_shm_name = dao.shm(self.threshold_shm_name, np.zeros((1,1)).astype(self.output_datatype))

    def loadShm(self,):
        """
        Loads existing shared memory segments for camera parameters.
        """
        super().loadShm()
        self.subApRef_shm_name = dao.shm(self.subApRef_shm_name)
        self.threshold_shm_name = dao.shm(self.threshold_shm_name)

        
    def launch(self):
        """
        Launches the camera controller process using TMUX.
        """
        # daoComputeCentroidRelative -L /tmp/wfs1CalIm.im.shm  /tmp/centroids1.im.shm /tmp/references1.im.shm /tmp/threshold1.im.shm 68 208
        args = f'-L {self.input_shm_name} {self.output_shm_name} {self.subApRef_shm_name} {self.threshold_shm_name} {self.subApSize} {self.nValidSubAps}'
        daoLaunch.manage_process(action='launch', tmuxname=self.tmuxname, processExe=self.process, processArgs=args)

    def generateSubApertureReference(self, nSubAps = 1, offset=None):
        """
        Generates a reference sub-aperture map for the WFS.
        """
        if offset is None:
            offset = (self.input_shape[0] - nSubAps*self.subApSize)/2
        else:
            offset = offset
        offset = 0
        
        pup=np.zeros((nSubAps,nSubAps))
        radius=nSubAps/2
        [X,Y]=np.meshgrid(np.linspace(-(nSubAps-1)/2,(nSubAps-1)/2,nSubAps), np.linspace(-(nSubAps-1)/2,(nSubAps-1)/2,nSubAps))
        R=np.sqrt(pow(X,2)+pow(Y,2))
        pup[R<=radius] = 1
        validSuba = int(pup.sum())

        # SH object which compute the default position
        sh=ShackHartmannWFS(nSubAps, nSubAps, self.subApSize)

        sxref=sh.subaperture_center_coordinates[0][pup==1]
        syref=sh.subaperture_center_coordinates[1][pup==1]
        #ref origine
        ro=np.array([np.array([sxref,syref]).astype(np.float32).T.flatten()])
        return ro.T+offset
    
    def setReference(self, ref):
        """
        Sets the reference sub-aperture map for the WFS.
        """
        self.subApRef_shm_name.set_data(ref)

if __name__=="__main__":
    log = dao.daoLog.daoLog(__name__)
    # log.logger.setLevel(logging.DEBUG)
    input_shm_name = "/tmp/calImage.im.shm"
    output_shm_name = "/tmp/centroids.im.shm"
    process = 'daoComputeCentroidRelative'
    nValidSubAps = 208
    subApSize = 68
    
    test = PixCalController(input_shm_name, output_shm_name, process,input_shape=(1104,1104), output_shape=(4*nValidSubAps, 1), subApSize=subApSize, nValidSubAps=nValidSubAps)
    
    test.createShm()
    test.launch()