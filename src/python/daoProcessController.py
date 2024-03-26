import dao
import logging
import os
import daoLaunch
import numpy as np

class ProcessController:
    
    def __init__(self, input_shm, output_shm, process, tmuxname=None, input_shape=(1, 1), output_shape=(1, 1), input_datatype=np.float32, output_datatype=np.float32):
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
        self.log = logging.getLogger(__name__)
        self.input_shm_name = input_shm
        self.output_shm_name = output_shm

        self.path, fullname = os.path.split(self.input_shm_name)
        filenameparts = fullname.split(".")
        self.input_name = filenameparts[0]

        self.path, fullname = os.path.split(self.output_shm_name)
        filenameparts = fullname.split(".")
        self.output_name = filenameparts[0]

        self.ext = "." + ".".join(filenameparts[1:])

        self.process = process

        if tmuxname is None:
            self.tmuxname = f'{process}_{self.input_name}'
        else:
            self.tmuxname = tmuxname

        self.input_shape = input_shape
        self.output_shape = output_shape

        self.input_datatype = input_datatype
        self.output_datatype = output_datatype
        
        
        self.log.info(f"self.input_name: {self.input_name}")
        self.log.info(f"self.output_name: {self.output_name}")
        self.log.info(f"process: {self.process}")
        self.log.info(f"tmuxname: {self.tmuxname}")
        self.log.info(f"input_shape: {self.input_shape}")  
        self.log.info(f"output_shape: {self.output_shape}")
        self.log.info(f"input_datatype: {self.input_datatype}")
        self.log.info(f"output_datatype: {self.output_datatype}")
        self.log.info(f"ext: {self.ext}")
        self.log.info(f"path: {self.path}")

        
    def createShm(self,):
        """
        Creates shared memory segments for various camera parameters.
        """
        self.input_shm = dao.shm(self.input_shm_name, np.zeros(self.input_shape).astype(self.input_datatype))
        self.output_shm = dao.shm(self.output_shm_name, np.zeros(self.output_shape).astype(self.output_datatype))
        
    def loadShm(self,):
        """
        Loads existing shared memory segments for camera parameters.
        """
        self.input_shm = dao.shm(self.input_shm_name)
        self.output_shm = dao.shm(self.output_shm_name)
        
    def launch(self):
        """
        Launches the camera controller process using TMUX.
        """
        args = f'-L {self.input_shm_name} {self.output_shm_name}'
        daoLaunch.manage_process(action='launch', tmuxname=self.tmuxname, processExe=self.process, processArgs=args)
        
    def kill(self):
        """
        Kills the camera controller process using TMUX.
        """
        daoLaunch.manage_process(action='kill', tmuxname=self.tmuxname)
        
if __name__=="__main__":
    log = dao.daoLog.daoLog(__name__)
    # log.logger.setLevel(logging.DEBUG)
    input_shm_name = "/tmp/rawImage.im.shm"
    output_shm_name = "/tmp/calImage.im.shm"
    process = 'top'
    test = ProcessController(input_shm_name, output_shm_name, process)
    
    test.createShm()
    test.launch()