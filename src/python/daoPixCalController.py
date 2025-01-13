import dao
import logging
import os
import daoLaunch
import numpy as np
from daoProcessController import ProcessController 


class PixCalController(ProcessController):
    def __init__(self, input_shm, output_shm, process, tmuxname=None, input_shape=(1, 1), output_shape=(1, 1), input_datatype=np.float32, output_datatype=np.float32, flatField_shm=None, background_shm=None):
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
        
        if flatField_shm is None:
            self.flatField_shm_name = self.path + '/' + self.input_name + 'flatField' + self.ext
        else:
            self.flatField_shm_name = flatField_shm
        if background_shm is None:
            self.background_shm_name = self.path + '/' + self.input_name + 'background' + self.ext
        else:

            self.background_shm_name = background_shm
        print(f"self.flatField_shm_name: {self.flatField_shm_name}")
        print(f"self.background_shm_name: {self.background_shm_name}")
    
    def createShm(self,):
        """
        Creates shared memory segments for various camera parameters.
        """
        super().createShm()
        self.flatField_shm = dao.shm(self.flatField_shm_name, np.zeros(self.input_shape).astype(self.output_datatype))
        self.background_shm = dao.shm(self.background_shm_name, np.zeros(self.input_shape).astype(self.output_datatype))

    def loadShm(self,):
        """
        Loads existing shared memory segments for camera parameters.
        """
        super().loadShm()
        self.flatField_shm = dao.shm(self.flatField_shm_name)
        self.background_shm = dao.shm(self.background_shm_name)
        
    def launch(self):
        """
        Launches the camera controller process using TMUX.
        """
        args = f'-L {self.input_shm_name} {self.flatField_shm_name} {self.background_shm_name} {self.output_shm_name}'
        daoLaunch.manage_process(action='launch', tmuxname=self.tmuxname, processExe=self.process, processArgs=args)

if __name__=="__main__":
    log = dao.daoLog.daoLog(__name__)
    # log.logger.setLevel(logging.DEBUG)
    input_shm_name = "/tmp/rawImage.im.shm"
    output_shm_name = "/tmp/calImage.im.shm"
    process = 'daoPixelCalibrate'
    test = PixCalController(input_shm_name, output_shm_name, process)
    
    test.createShm()
    test.launch()