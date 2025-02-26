import dao
import logging
import os
import daoLaunch
import numpy as np
from daoProcessController import ProcessController 


class DMCombinderController(ProcessController):
    def __init__(self, input_shm, output_shm, process, tmuxname=None, input_shape=(1, 1), output_shape=(1, 1), input_datatype=np.float32, output_datatype=np.float32, nChannels=4):
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
        print
        self.nChannels = nChannels
        
        path, fullname = os.path.split(self.output_shm_name)
        filenameparts = fullname.split(".")
        name = filenameparts[0]
        ext = ".im.shm"
        self.channel_shm_names  = [f"{path}/{name}{str(i).zfill(2)}{ext}" for i in range(nChannels)]
        print(self.channel_shm_names)


    def createShm(self,):
        """
        Creates shared memory segments for various camera parameters.
        """
        super().createShm()
        self.channel_shm = []
        for i in self.channel_shm_names:
            A =  np.zeros((self.output_shape)).astype(self.output_datatype)
            self.channel_shm.append(dao.shm(i, A))


        

    def loadShm(self,):
        """
        Loads existing shared memory segments for camera parameters.
        """
        super().loadShm()
        self.channel_shm = []
        for i in self.channel_shm_names:
            self.channel_shm.append(dao.shm(i))
        
    def launch(self):
        """
        Launches the camera controller process using TMUX.
        """
        args = f'-L {self.output_shm_name} {self.nChannels}'
        daoLaunch.manage_process(action='launch', tmuxname=self.tmuxname, processExe=self.process, processArgs=args)
    

    
if __name__=="__main__":
    log = dao.daoLog.daoLog(__name__)
    # log.logger.setLevel(logging.DEBUG)
    input_shm_name = "/tmp/rawImage.im.shm"
    output_shm_name = "/tmp/calImage.im.shm"
    process = ''
    test = MVMController(input_shm_name, output_shm_name, process)
    
    test.createShm()
    test.launch()