import dao
import logging
import os
import daoLaunch
import numpy as np
from daoProcessController import ProcessController 
from daoTools import ShackHartmannWFS



class CentroidController(ProcessController):
    def __init__(self, input_shm, output_shm, process, tmuxname=None, input_shape=(1, 1), output_shape=None, input_datatype=np.float32, output_datatype=np.float32, subApRef_shm_name=None, threshold_shm=None, nPix=1, nSubs=1, threshold=0):
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
        
        self.nPix = nPix
        self.nSubs = nSubs
        self.threshold = threshold
        
        if output_shape is not None:
            self.nValidSubs=output_shape[0]
        else:
            self.nValidSubs = None
            
        print(f"self.subApRef_shm_name: {self.subApRef_shm_name}")
        print(f"self.threshold_shm_name: {self.threshold_shm_name}")
        print(f"self.nPix: {self.nPix}")
        print(f"self.nSubs: {self.nSubs}")
        print(f"self.threshold: {self.threshold}")
    
    def createShm(self,):
        """
        Creates shared memory segments for various camera parameters.
        """
        if self.nValidSubs is not None:
            super().createShm()
            self.subApRef_shm = dao.shm(self.subApRef_shm_name, (np.ones(self.nValidSubs*2)*self.threshold).astype(self.output_datatype))
            self.threshold_shm = dao.shm(self.threshold_shm_name, np.zeros((1,1)).astype(self.output_datatype))
        else:
            ref = self.generateSubApertureReference()
            self.output_shape = (self.nValidSubs*4, 1)
            super().createShm()
            self.subApRef_shm = dao.shm(self.subApRef_shm_name, ref.astype(np.float32))
            self.threshold_shm = dao.shm(self.threshold_shm_name, (np.ones((1,1))*self.threshold).astype(self.output_datatype))

    def loadShm(self,):
        """
        Loads existing shared memory segments for camera parameters.
        """
        super().loadShm()
        self.subApRef_shm = dao.shm(self.subApRef_shm_name)
        self.threshold_shm = dao.shm(self.threshold_shm_name)

        
    def launch(self):
        """
        Launches the camera controller process using TMUX.
        """
        # daoComputeCentroidRelative -L /tmp/wfs1CalIm.im.shm  /tmp/centroids1.im.shm /tmp/references1.im.shm /tmp/threshold1.im.shm 68 208
        args = f'-L {self.input_shm_name} {self.output_shm_name} {self.subApRef_shm_name} {self.threshold_shm_name} {self.nPix} {self.nValidSubs}'
        daoLaunch.manage_process(action='launch', tmuxname=self.tmuxname, processExe=self.process, processArgs=args)

    def generateSubApertureReference(self, offset=None):
        """
        Generates a reference sub-aperture map for the WFS.
        """
        if offset is None:
            offset = (self.input_shape[0] - self.nSubs*self.nPix)/2
        else:
            offset = offset
        offset = 0
        
        pup=np.zeros((self.nSubs,self.nSubs))
        radius=self.nSubs/2
        [X,Y]=np.meshgrid(np.linspace(-(self.nSubs-1)/2,(self.nSubs-1)/2,self.nSubs), np.linspace(-(self.nSubs-1)/2,(self.nSubs-1)/2,self.nSubs))
        R=np.sqrt(pow(X,2)+pow(Y,2))
        pup[R<=radius] = 1
        self.nValidSubs = int(pup.sum())

        # SH object which compute the default position
        sh=ShackHartmannWFS(self.nSubs, self.nSubs, self.nPix)

        sxref=sh.subaperture_center_coordinates[0][pup==1]
        syref=sh.subaperture_center_coordinates[1][pup==1]
        #ref origine
        ro=np.array([np.array([sxref,syref]).astype(np.float32).T.flatten()])
        return ro.T+offset
    
    def setReference(self, ref):
        """
        Sets the reference sub-aperture map for the WFS.
        """
        self.nValidSubs = ref.shape[0]//2
        self.output_shape = (self.nValidSubs*4,1)
        self.subApRef_shm.set_data(ref)
    
    def setThreshold(self, threshold):
        self.threshold_shm.set_data(self.threshold_shm.get_data()*0 + threshold)

if __name__=="__main__":
    log = dao.daoLog.daoLog(__name__)
    # log.logger.setLevel(logging.DEBUG)
    input_shm_name = "/tmp/calImage.im.shm"
    output_shm_name = "/tmp/centroids.im.shm"
    process = 'daoComputeCentroidRelative'
    nSubs = 208
    nPix = 68
    
    test = CentroidController(input_shm_name, output_shm_name, process,input_shape=(1104,1104), output_shape=(4*nValidSubAps, 1), nPix=nPix, nSubs=nSubs)
    
    test.createShm()
    test.launch()