import dao
import logging
import os
import daoLaunch
import numpy as np
from daoProcessController import ProcessController 


class LoopController(ProcessController):
    def __init__(self, input_shm, output_shm, process, tmuxname=None, input_shape=(1, 1), output_shape=(1, 1), input_datatype=np.float32, output_datatype=np.float32, offset_shm=None, loop_shm=None, loop_gain_shm=None, loop_leak_shm=None):
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
        
        if offset_shm is None:
            self.offset_shm_name = self.path + '/' + self.input_name + 'offset' + self.ext
        else:
            self.offset_shm_name = offset_shm
       
        if loop_shm is None:
            self.loop_shm_name = self.path + '/' + self.input_name + 'Loop' + self.ext
        else:
            self.loop_shm_name = loop_shm
        
        if loop_gain_shm is None:
            self.loop_gain_shm_name = self.path + '/' + self.input_name + 'LoopGain' + self.ext
        else:
            self.loop_gain_shm_name = loop_gain_shm    

        if loop_leak_shm is None:
            self.loop_leak_shm_name = self.path + '/' + self.input_name + 'LoopLeak' + self.ext
        else:
            self.loop_leak_shm_name = loop_leak_shm    
        
    def createShm(self,):
        """
        Creates shared memory segments for various camera parameters.
        """
        super().createShm()
        self.offset_shm = dao.shm(self.offset_shm_name, np.zeros(self.output_shape).astype(self.output_datatype))
        self.loop_shm = dao.shm(self.loop_shm_name, np.zeros((1,1)).astype(np.uint32))
        self.loop_gain_shm = dao.shm(self.loop_gain_shm_name, np.zeros((1,1)).astype(np.float32))
        self.loop_leak_shm = dao.shm(self.loop_leak_shm_name, np.ones((1,1)).astype(np.float32))

        

    def loadShm(self,):
        """
        Loads existing shared memory segments for camera parameters.
        """
        super().loadShm()
        self.offset_shm = dao.shm(self.offset_shm_name)
        self.loop_shm = dao.shm(self.loop_shm_name)
        self.loop_gain_shm = dao.shm(self.loop_gain_shm_name)
        self.loop_leak_shm = dao.shm(self.loop_leak_shm_name)
        
    def launch(self):
        """
        Launches the camera controller process using TMUX.
        """
        args = f'-L {self.input_shm_name} {self.offset_shm_name} {self.output_shm_name} {self.loop_shm_name} {self.loop_gain_shm_name} {self.loop_leak_shm_name}'
        daoLaunch.manage_process(action='launch', tmuxname=self.tmuxname, processExe=self.process, processArgs=args)
    
    def setOffset(self,offset):
        self.offset_shm.set_data(offset)
    
    def getOffset(self):
        return self.offset_shm.get_data()

    def setLoopStatus(self,loop):
        self.loop_shm_shm.set_data(loop)
    
    def getLoopStatus(self):
        return self.loop_shm.get_data()

    def setLoopGain(self,gain):
        self.loop_gain_shm_shm.set_data(gain)
    
    def getLoopGain(self):
        return self.loop_gain_shm.get_data()
    
    def setLoopLeak(self,gain):
        self.loop_leak_shm_shm.set_data(leak)
    
    def getLoopLeak(self):
        return self.loop_leak_shm.get_data()
    
if __name__=="__main__":
    log = dao.daoLog.daoLog(__name__)
    # log.logger.setLevel(logging.DEBUG)
    input_shm_name = "/tmp/rawImage.im.shm"
    output_shm_name = "/tmp/calImage.im.shm"
    process = ''
    test = MVMController(input_shm_name, output_shm_name, process)
    
    test.createShm()
    test.launch()