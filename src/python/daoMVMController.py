import dao
import logging
import os
import daoLaunch
import numpy as np
from daoProcessController import ProcessController 


class MVMController(ProcessController):
    def __init__(self, input_shm, output_shm, process, tmuxname=None, input_shape=(1, 1), output_shape=(1, 1), input_datatype=np.float32, output_datatype=np.float32, CM_shm=None):
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
        
        if CM_shm is None:
            self.CM_shm_name = self.path + '/' + self.input_name + 'CM' + self.ext
        else:
            self.CM_shm_name = CM_shm
        print(f"self.CM_shm_name: {self.CM_shm_name}")
    
    def createShm(self,):
        """
        Creates shared memory segments for various camera parameters.
        """
        super().createShm()
        acts = self.output_shape[0]
        nSubs = self.input_shape[0]//2
        
        self.CM_shm = dao.shm(self.CM_shm_name, np.zeros((acts, nSubs)).astype(self.output_datatype))

    def loadShm(self,):
        """
        Loads existing shared memory segments for camera parameters.
        """
        super().loadShm()
        self.CM_shm = dao.shm(self.CM_shm_name)
        
    def launch(self):
        """
        Launches the camera controller process using TMUX.
        """
        args = f'-m {self.CM_shm_name} -v {self.input_shm_name} -o {self.output_shm_name}'
        daoLaunch.manage_process(action='launch', tmuxname=self.tmuxname, processExe=self.process, processArgs=args)
    
    def setCM(self, CM):
        self.CM_shm.set_data(CM)
        
    def getCM(self):
        return self.CM_shm.get_data()
    
if __name__=="__main__":
    log = dao.daoLog.daoLog(__name__)
    # log.logger.setLevel(logging.DEBUG)
    input_shm_name = "/tmp/rawImage.im.shm"
    output_shm_name = "/tmp/calImage.im.shm"
    process = ''
    test = MVMController(input_shm_name, output_shm_name, process)
    
    test.createShm()
    test.launch()