#!/usr/bin/env python3

'''---------------------------------------------------------------------------
Several useful tools for AO
'''

import os, sys, mmap, struct
import numpy as np
import astropy.io.fits as pf
import time
from threading import Thread
from threading import Event
import zmq

# FIFO (First In First Out) class
class Fifo:
    ''' First In First Out Class. aka circular buffer.
        The size of the buffer is given in the constructor
        >> myFifo = Fifo(100)
        >> myFifo.Append(something)
        >> myFifo.Get() # Get all the data in the fifo
        >> myFifo.GetLast() # Get the most recent data of the fifo
	'''
    def __init__(self, nelem):
        self.data = [0 for i in range(nelem)]
        self.nelem = nelem

    def Append(self, x):
        self.data.pop(0)
        self.data.append(x)
    
    def GetLast(self):
        return self.data[self.nelem-1]

    def Get(self):
        return self.data

#
# The following fits handling function are extremely basic, use at your own
# risks
#
def createFits(data, fname):
    ''' Create fits file from data
    '''
    hdu = pf.PrimaryHDU(data)
#    hdu.writeto(fname, clobber=True)
    hdu.writeto(fname, overwrite=True)
    logging.info("creating "+fname)

def createCubeFits(data, fname):
    ''' Create fitsfile from data
    '''
    hdul = pf.HDUList()
    hdul.append(pf.PrimaryHDU())

    for img in data:
        hdul.append(pf.ImageHDU(data=img))

    hdu = pf.PrimaryHDU(data)
    hdu.writeto(fname, overwrite=True)

def readFits(fname):
    ''' Read Fits data from file
    '''
    hdulist = pf.open(fname)
    return hdulist[0].data

# Create Circular Mask
def createCircularMask(h, w, center=None, radius=None):
    ''' Circular Mask creation function
    '''
    if center is None: # use the middle of the image
        center = [int(w/2), int(h/2)]
    if radius is None: # use the smallest distance between the center and image walls
        radius = min(center[0], center[1], w-center[0], h-center[1])

    Y, X = np.ogrid[:h, :w]
    dist_from_center = np.sqrt((X - center[0] + 0.5)**2 + (Y-center[1] + 0.5)**2)

    mask = dist_from_center <= radius
    return mask

# Function to find centre of gravity of an image
def cog(im):
    ''' Return center of gravity coordinates of an image
    '''
    # Set x/y vector
    [ny,nx] = np.shape(im)
    [X,Y] = np.meshgrid(np.linspace(0,nx-1,nx),np.linspace(0,ny-1,ny))

    # Compute centre of gravity
    x0 = np.sum(X*im)/np.sum(im)
    y0 = np.sum(Y*im)/np.sum(im)

    return x0, y0

def pupil(N):
    '''Compute pupil mask for given diameter
       input: N - size of array and diameter of pupil
    '''
    p = np.zeros([N,N])

    radius = N/2
    [X,Y] = np.meshgrid(np.linspace(-(N-1)/2,(N-1)/2,N),np.linspace(-(N-1)/2,(N-1)/2,N))
    R = np.sqrt(pow(X,2)+pow(Y,2))
    p[R<=radius] = 1
    
    return p

# Function to compute the rms of an array along a given dimension
def rms(data,dim):
    '''Function to compute the rms of an array along a given dimension
       input: data - input array
              dim - dimension along which to compute the rms (0 or 1)
    '''
    r = np.sqrt(np.mean(pow(data,2),axis=dim))
    return r

class Hadamard():
    ''' Create Hadamard Matrix
    '''
    def __init__(self, n, dmMask):
        # Create the matrix.
        self.Hmat = np.ones((n,n), np.bool)
        self.dmMask = dmMask
        # Initialize Hadamard matrix of order n.
        i1 = 1
        while i1 < n:
            for i2 in range(i1):
                for i3 in range(i1):
                    self.Hmat[i2+i1][i3]    = self.Hmat[i2][i3]
                    self.Hmat[i2][i3+i1]    = self.Hmat[i2][i3]
                    self.Hmat[i2+i1][i3+i1] = not self.Hmat[i2][i3]
            i1 += i1
        self.idxArray = -np.ones(n)
        self.dmId = np.where(self.dmMask.reshape(1,self.dmMask.size) == 1)
        self.idxArray[0:sum(sum(self.dmMask))] = self.dmId[1]
        self.Hpoke=np.zeros((self.dmMask.size,n))
        for k in range(0,n):
            for index in range(0,n):
                ii = int(self.idxArray[index])
                if ii >0:
                    self.Hpoke[ii,k] = self.Hmat[index,k]

# Equivalent of Matlab conv2 function
def conv2(x,y,mode):
    '''Equivalent of Matlab conv2 function
      inputs:   x,y  - arrays to be convolved
                mode - option.
                       'same'
      return:   z    - result of convolution
    '''
    z = np.rot90(convolve2d(np.rot90(x,2),np.rot90(y,2),mode),2)
    return z

# Function to compute 2D PSD
# If window = 1, apply hanning window
def computePSD(dataCube,window):

    [nMeas,N,M] = np.shape(dataCube)

    psd = np.zeros([N,M])
    if window==1:
        w = fftWindow(N)
    else:
        w = np.ones([N,N])

    # Compute 2D PSD
    for n in range(0,nMeas):
        psd = psd + pow(abs(np.fft.fftshift(np.fft.fft2(w*dataCube[n,:,:]))),2)/nMeas

    # Take radial average
    n0 = np.ceil((N+1)/2)-1
    psd1d = radialMean(psd,[n0,n0])

    return psd, psd1d

# Function to compute window for PSD (Hanning window)
def fftWindow(N):
    [X,Y] = np.meshgrid(np.linspace(-N/2,N/2,N),np.linspace(-N/2,N/2,N))
    R = np.sqrt(pow(X,2)+pow(Y,2))

    w = pow(np.cos(np.pi*R/(N)),2)
    w[np.abs(R)>N/2] = 0

    return w
    
def radialMean(image, center=None):
    """
    Calculate the azimuthally averaged radial profile.
    image - The 2D image
    center - The [x,y] pixel coordinates used as the center. The default is 
             None, which then uses the center of the image (including 
             fracitonal pixels).
    
    """
    # Calculate the indices from the image
    y, x = np.indices(image.shape)

    if not center:
        center = np.array([(x.max()-x.min())/2.0, (y.max()-y.min())/2.0])

    r = np.hypot(x - center[0], y - center[1])

    # Get sorted radii
    ind = np.argsort(r.flat)
    r_sorted = r.flat[ind]
    i_sorted = image.flat[ind]

    # Get the integer part of the radii (bin size = 1)
    r_int = r_sorted.astype(int)

    # Find all pixels that fall within each radial bin.
    deltar = r_int[1:] - r_int[:-1]  # Assumes all radii represented
    rind = np.where(deltar)[0]       # location of changed radius
    nr = rind[1:] - rind[:-1]        # number of radius bin
    
    # Cumulative sum to figure out sums for each radius bin
    csim = np.cumsum(i_sorted, dtype=float)
    tbin = csim[rind[1:]] - csim[rind[:-1]]

    radial_prof = tbin / nr

    return radial_prof

def computePsf(wf,wavelength,pupil,psfRef,res):
    # Wavenumber
    k = 2*np.pi/wavelength

    # Number of wavefronts in cube
    if np.size(np.shape(wf))==2:
        nWFs = 1
    else:
        nWFs = np.shape(wf)[0]

    # Compute reference PSF
    #psfRef = np.abs(np.fft.fftshift(np.fft.fft2(pupil,s=[res,res])))**2

    # Compute psf
    psf = 0
    for n in range(0,nWFs):

        if np.size(np.shape(wf))==2:
            # Phase
            phi = wf*k
        else:
            # Phase
            phi = wf[n,:,:]*k

        # Pupil function
        P = pupil*np.exp(1j*phi)
        
        # Instantaneous psf
        psf_tmp = np.abs(np.fft.fftshift(np.fft.fft2(P,s=[res,res])))**2

        # Combined psf
        psf = psf+psf_tmp/nWFs

    # Compute strehl
    strehl = np.max(psf)/np.max(psfRef)
    sys.stdout.write('\rStrehl ratio = '+str(strehl*100)+'%')
    sys.stdout.flush()

    return psf, strehl

def computePsfRef(wavelength,pupil,res):
    # Wavenumber
    k = 2*np.pi/wavelength

    # Compute reference PSF
    psfRef = np.abs(np.fft.fftshift(np.fft.fft2(pupil,s=[res,res])))**2

    return psfRef

class ShackHartmannWFS:
    """
    A class to simulate a Shack-Hartmann wavefront sensor (WFS) and compute the centroid of the spots.
    """
    def __init__(self, num_subapertures: int, subaperture_size: int):
        """
        Initialize the Shack-Hartmann WFS object.

        Parameters:
        - num_subapertures (int): The number of subapertures in the Shack-Hartmann WFS.
        - subaperture_size (int): The size of each subaperture.
        """
        self.num_subapertures = num_subapertures
        self.subaperture_size = subaperture_size
        self.subapertures = np.zeros((num_subapertures, subaperture_size, subaperture_size))

    def measure_wavefront(self, wavefront: np.ndarray):
        """
        Measure the wavefront by dividing it into subapertures.

        Parameters:
        - wavefront (np.ndarray): The wavefront to be measured.

        """
        for i in range(self.num_subapertures):
            x, y = np.meshgrid(np.linspace(-1, 1, self.subaperture_size), np.linspace(-1, 1, self.subaperture_size))
            subaperture = wavefront[i * self.subaperture_size:(i + 1) * self.subaperture_size, i * self.subaperture_size:(i + 1) * self.subaperture_size]
            self.subapertures[i] = subaperture

    def compute_centroids(self) -> np.ndarray:
        """
        Compute the centroid of the spots.

        Returns:
        - centroids (np.ndarray): The centroids of the spots. The shape of the array is (num_subapertures, 2) where the first column represents the x-coordinate and the second column represents the y-coordinate.
        """
        centroids = np.zeros((self.num_subapertures, 2))
        for i in range(self.num_subapertures):
            subaperture = self.subapertures[i]
            x, y = np.meshgrid(np.linspace(-1, 1, self.subaperture_size), np.linspace(-1, 1, self.subaperture_size))
            centroids[i, 0] = np.sum(x * subaperture) / np.sum(subaperture)
            centroids[i, 1] = np.sum(y * subaperture) / np.sum(subaperture)
        return centroids

class PyramidWFS:
    def __init__(self, pyramid_size, subaperture_size):
        """
        Initialize the Pyramid WFS object with pyramid size and subaperture size.

        Parameters:
        pyramid_size (int): size of the pyramid (number of subapertures along one side)
        subaperture_size (float): size of the subapertures in meters
        """
        self.pyramid_size = pyramid_size
        self.subaperture_size = subaperture_size
        self.pyramid = np.zeros((pyramid_size, pyramid_size))

    def measure_wavefront(self, wavefront):
        """
        Measure the wavefront and store the values in the pyramid.

        Parameters:
        wavefront (np.ndarray): 2D array with wavefront values
        """
        self.pyramid = wavefront

    def compute_slopes(self):
        """
        Compute the slopes (derivatives) of the wavefront.

        Returns:
        slopes (np.ndarray): 3D array with slopes along x and y axes
        """
        slopes = np.zeros((2, self.pyramid_size, self.pyramid_size))
        for i in range(self.pyramid_size):
            for j in range(self.pyramid_size):
                slopes[0, i, j] = (self.pyramid[i, j+1] - self.pyramid[i, j-1])/(2*self.subaperture_size)
                slopes[1, i, j] = (self.pyramid[i+1, j] - self.pyramid[i-1, j])/(2*self.subaperture_size)
        # Normalize slopes by the flux
        slopes /= np.sum(self.pyramid)
        return slopes

    def compute_intensity(self):
        """
        Compute the intensity of the wavefront.

        Returns:
        intensity (np.ndarray): 2D array with intensity values
        """
        intensity = np.zeros((self.pyramid_size, self.pyramid_size))
        for i in range(self.pyramid_size):
            for j in range(self.pyramid_size):
                intensity[i, j] = (self.pyramid[i, j] + self.pyramid[i+1, j] + self.pyramid[i, j+1] + self.pyramid[i+1, j+1])/4
        return intensity
