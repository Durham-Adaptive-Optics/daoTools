import numpy as np
from threading import Thread, Event
from os import path
import queue

import datetime
import logging

import daoShm 
import daoLog

from astropy.table import Table, Column
from astropy.io import fits

# limitations:
# All require to run at same frame rate. This can be adjusted but currently not viable
#  
class daoShmRecord:
    def __init__(self, shmFiles, name=__name__):
        """
        Initializes a daoShmRecord object.

        Parameters
        ----------
        shmFiles : list of str
            A list of file paths of the shared memory files to be recorded.
        name : str, optional
            The name of the logger to be used for this object.
        """
        self.logger = logging.getLogger(name)
        self.shmFiles = shmFiles
        self.shmList = []

        # Check that each shared memory file exists
        for file in self.shmFiles:
            if not path.exists(file):
                raise FileNotFoundError(f"shmFile: {file} - does not exist")

        # Open each shared memory file and add it to a list
        for file in self.shmFiles:
            self.shmList.append(daoShm.shm(file))
    
    def record(self, nFrames, fitsfile=None):
        """
        Records data from the shared memory files, and saves it to a FITS file.

        Parameters
        ----------
        nFrames : int
            The number of frames to be recorded from each shared memory file.
        fitsfile : str, optional
            The name of the output FITS file to save the recorded data to.

        Returns
        -------
        dict
            A dictionary containing the recorded data.
        """
        self.threads = []
        self.que = queue.Queue()

        # Start a new thread to record data from each shared memory file
        for i in range(len(self.shmFiles)):
            self.logger.info(f"Launching thread[{i}] for shm: {self.shmFiles[i]}")
            t = Thread(target=self.ReadSHM, args=(self.shmList[i], nFrames, self.que, self.logger))
            t.start()
            self.threads.append(t)

        # Wait for all threads to complete
        for t in self.threads:
            self.logger.info(f"Joining thread: {i}")
            t.join()

        # Combine the recorded data from all threads into a single dictionary
        self.dic = {}
        while not self.que.empty():
            p = self.que.get()
            self.dic.update(p)

        # Save the recorded data to a FITS file if a filename is provided
        if (fitsfile):
            self.save2Fits(self.dic, fitsfile)
        return self.dic
            
    def ReadSHM(self, shm, nFrames, que, logger):
        """
        Records data from a shared memory file.

        Parameters
        ----------
        shm : daoShm object
            The shared memory object to be recorded.
        nFrames : int
            The number of frames to be recorded.
        que : queue.Queue
            The queue to store the recorded data.
        logger : logging.Logger
            The logger to be used for this function.

        Returns
        -------
        None
        """
        d = shm.get_data()
        [x,y] = d.shape
        logger.info(f"{shm.mtdata['imname']} - {nFrames} of [{x} x {y}] with data type {d.dtype.name}")

        # create a np array for each data point
        data        = np.zeros([nFrames, x,y]).astype(np.float64)
        counters    = np.zeros([nFrames]).astype(np.int32)
        timestamp   = np.zeros([nFrames]).astype(np.uint64)

        i = 0
        update=False
        first = True
        while (i < nFrames):
            d = shm.get_data(check=True)
            if(first):
                last_counter = shm.get_counter() - 1
                first = False
            new_counter = shm.get_counter()

            diff = new_counter - last_counter
            logger.debug(f"{shm.mtdata['imname']} : frame({i}) - diff({diff})[{new_counter} - {last_counter}]")
            # error checking the counter
            if(diff != 1):
                if(diff > 1):
                    # if greater then 1 it means we missed a frame so we calc the diff so we can skip those locations in the array and zero them out
                    i+=diff
                    update=True
                elif (diff == 0):
                    logger.warning(f"{shm.mtdata['imname']} : Unacknowledge semaphore detected")
                    update=False
                elif diff < 0:
                    #if less then zero we raise an exception
                    raise Exception(f"shm read failed for {shm.mtdata['imname']} ")
                    update=False
            else:
                update=True

            if update == True:
                data[i] = d
                counters[i] = new_counter
                timestamp[i] = shm.get_timestamp().timestamp()
                last_counter = new_counter
                i+=1
                update=False
            # else:
            #     last_counter = new_counter
        # create dictionary:
        partDic = {shm.mtdata['imname'] : {'counter' : counters, 'timestamp' : timestamp, 'data' : data}}
        que.put(partDic)
        return 

    def save2Fits(self, data_dict, filename, optional_header=None):
        """
        Save a dictionary of data as a multi-extension FITS file.

        Parameters
        ----------
        data_dict : dict
            A dictionary where each key is a file name and each value is a
            dictionary containing the 'counter', 'timestamp', and 'data'
            arrays to be saved as a binary table.
        filename : str
            The name of the output FITS file.
        optional_header : dict, optional
            A dictionary containing any optional header keywords and values to
            be added to the primary HDU.

        Raises
        ------
        IOError
            If the output file already exists and `overwrite` is False.

        Returns
        -------
        None
        """
        self.logger.debug(f"Save2Fits: {filename}")
        hdulist = fits.HDUList()
        i = 0
        d = {}

        # Loop over each file in the dictionary
        for key in data_dict:
            counter = data_dict[key]['counter']
            timestamp = data_dict[key]['timestamp']
            data = data_dict[key]['data']

            # Convert nested dictionary to structured numpy array
            dtype = np.dtype([
                ('counter', counter.dtype),
                ('timestamp', timestamp.dtype),
                ('data', data.dtype, data.shape[1:])
            ])
            value = np.empty(len(counter), dtype=dtype)
            value['counter'] = counter
            value['timestamp'] = timestamp
            value['data'] = data

            # Create FITS binary table and append to HDUList
            hdulist.append(fits.BinTableHDU(value, name=f"file{i}"))
            d.update({f"file{i}": key})
            i += 1

        # Add optional header keywords to the primary HDU
        if optional_header:
            d.update(optional_header)

        header_items = [(key, str(val), '') for key, val in d.items()]

        # Create a new Header object and add the keywords
        new_header = fits.Header()
        for item in header_items:
            new_header.append(item)
        hdulist[0].header.extend(new_header, update=True)

        # Write to file
        hdulist.writeto(filename, overwrite=True)


# free function to read in data from fits.
def daoShmRecReadFromFits(filename):
    """
    Read data and header information from a FITS file.

    Parameters
    ----------
    filename : str
        Name of the FITS file to read.

    Returns
    -------
    data_dict : dict
        Dictionary containing the data from each HDU.
    optional_headers : dict
        Dictionary containing non-default header items from primary HDU.

    """
    # Reopen the FITS file with case sensitivity enabled
    hdulist = fits.open(filename, case_sensitive=True)

    # Create an empty dictionary to hold the data
    data_dict = {}
    optional_headers = {}

    # Loop over each HDU (excluding the primary HDU)
    for hdu in hdulist[1:]:
        # Get the name of the HDU from the primary HDU header
        key = hdulist[0].header[hdu.name]

        # Get the data from the HDU
        data = hdu.data

        # Convert the structured numpy array to a nested dictionary
        counter = data['counter']
        timestamp = data['timestamp']
        value = data['data']
        # Verify the shape of the data and transpose if necessary
        if len(value.shape) == 3:
            value = np.transpose(value, (2, 0, 1))
        data_dict[key] = {'counter': counter, 'timestamp': timestamp, 'data': value}

    # Extract non-default header items from primary HDU
    if len(hdulist) > 0:
        header = hdulist[0].header
        for key in header:
            if key not in ['SIMPLE', 'BITPIX', 'NAXIS', 'EXTEND']:
                optional_headers[key] = header[key]

    hdulist.close()

    return data_dict, optional_headers

if __name__=="__main__":
     ## using dao logger but no requiement without these lines will use default logger.
    log = daoLog.daoLog(__name__,level=logging.TRACE)
    logger = logging.getLogger(__name__)
    logger.setLevel(logging.TRACE)

    file1 = 'test.im.shm'
    file2 = 'other.im.shm'
    ffile = 'test.fits'
    list = [file1, file2] #, 'Test2.im.shm']

    # create record object and check shm files exsit and open them
    daoRec = daoShmRecord(list)

    # record 10 frames and save to fits file
    a   = daoRec.record(10, fitsfile=ffile)

    # load from fits file
    b,c = daoShmRecReadFromFits('Test.fits')

    print(a)
    print(b)
    print(c)