#!/usr/bin/env python3

import sys, getopt
import numpy as np
import astropy.io.fits as pf
import time
import datetime
from scipy import signal 
import matplotlib.pyplot as plt
from math import gamma

import dao

import os
os.nice(10)

# Function to provide simulated turbulence to apply to the
# DM.  Output is in the form of DM commands.
# 
# Charlotte Bond    16th October 2018
# The applyTub function sets up the initial parameters of the
# turbulence and computes the initial phase screen.  This is
# then propagated through the function updatePhaseScreen()
#
# inputs: nAct - number of actuators (across pupil).
#                Sets the maximum resolution of the phase
#                screen.  Should be 21 (Keck).
#         D -    Diamater of telescope (10.95m for Keck) [m]
#         r0 -   Fried parameter [m] at 500nm
#         wS -   Wind speed [m/s]
#         wD -   Wind direction [deg].  0 = purely x direction,
#                90 = purely y direction.
#         dt -   Time step [s] between updates of the commands
#                (i.e. 1/freq where freq is the frequency of
#                the AO loop)
#         T -    Total time period [s]
#
# For live use adapt applyTub() function to continuously update DM
# or save a series of images phase screens for later application.
#
# Charlotte Bond    16th October 2018
#

def applyTub(shm, shmMap, nAct, D, r0, wS, wD, dt, T):

    # Compute the initial phase screen in Fourier space
    [fft_Z,Kx,Ky] = fourierPhaseScreen(nAct,D,r0)

    # Time
    t = np.linspace(0,T,int(np.round(T/dt+1)))
    nTimes = (np.round(T/dt+1)).astype(np.int32)

    # Update phase screen/commands as turbulence moves aross pupil
    wD_rad = np.pi*wD/180

    Z_DM = np.zeros([nAct,nAct])
    pup = pupil(nAct)==1
    
    #plt.ion()
    for n in range(0,nTimes):
        # Shift due to wind
        shift = np.exp(2*1j*np.pi*(Kx*np.cos(wD_rad)+Ky*np.sin(wD_rad))*wS*t[n])

        # New phase screen
        Z = np.real(np.fft.ifft2(np.fft.ifftshift(fft_Z*shift)))/D*pow(nAct,2)

        # Project onto DM actuators (phase computed at 500nm)
        Z_coefs = projectOntoDM(Z,nAct,500e-9)

        #----------------------------------------------------------------------
        # Look at DM coefficients in 2D map (just for plotting
        # purposes, remove for live use)
        Z_DM[pup] = Z_coefs 

        shm.set_data(Z_DM.astype(np.float32).flatten()[shmMap.get_data().flatten()==1])
        time.sleep(dt)

        #plt.imshow(Z_DM)
        #plt.colorbar()
        #plt.show()
        #plt.pause(0.01)
        #plt.close()
        #----------------------------------------------------------------------


    
# Function to compute phase screen in Fourier space.  Uses
# a Kolmogorov turbulence profile and produces a random
# realisation of the phase (in Fourier space).  Can be
# used to feed turbulence simulator.
#
# Inputs: nAct - number of actuators across the pupil
#         D -    telescope diamater [m]
#         r0 -   Fried parameter [m] at 500nm
    
    
def fourierPhaseScreen(nAct, D, r0):
    
    # Initiate spatial frequency parameters (maximum frequency
    # given by resolution)
    d = D/(nAct)
    kmax = 1/(2*d)
    if (nAct % 2):
        k = np.linspace(-kmax,kmax,nAct)
    else:
        k = np.linspace(-kmax,kmax-1/D,nAct)

    # Compute PSD for given r0 and frequency vector
    [Kx,Ky] = np.meshgrid(k,k)
    Kr = np.sqrt(pow(Kx,2)+pow(Ky,2))
    asd = np.sqrt(turbulencePSD(Kr,r0))

    # Compute random realisation of phase (first iteration)
    noise = np.random.randn(nAct,nAct)+1j*np.random.randn(nAct,nAct)
    fft_Z = asd*noise

    return fft_Z, Kx, Ky

# Function to compute the power sepctral density for a given
# turbulence profile.
#
# inputs: k -  spatial frequency vector [1/m]
#         r0 - Fried parameter [m] at 500nm

def turbulencePSD(k, r0):

    [N,M] = np.shape(k)
    
    # Define amplitude with r0
    amp = pow((24*gamma(6/5)/5),5/6) * pow(gamma(11/6),2) / (2*pow(np.pi,11/3)) * pow(r0,-5/3)

    # Compute PSD
    psd = amp*pow(pow(k,2),-11/6)

    # Remove piston
    n0 = (np.ceil((N+1)/2)-1).astype(np.int32)
    psd[n0,n0] = 0

    return psd

# Function to project a phase onto DM actuators.  Currently uses
# straight projection but should in future consider shape of
# DM influence functions.
#
# Function uses actuator calibration for Keck DM, i.e. 0.6 u/V for
# multiple actuators.
#
# inputs: Z -    phase [rad]
#         nAct - number of actuators acroos pupil (Z should have
#                dimensions of nAct x nAct)
#         wL -   wavelength associated with phase [m]

def projectOntoDM(Z, nAct, wL):

    # Convert phase to optical path length and halve to
    # compensate for double pass of DM
    Z_DM = Z*wL/(2*np.pi)/2

    # Set commands only for valid actuators
    p = pupil(nAct)==1

    # Scale commands for Keck DM (1V = 0.6u)
    scale = 0.6e-6
    Z_coefs = Z_DM[p]/scale

    # Remove piston (average)
    Z_coefs = Z_coefs - np.mean(Z_coefs)

    return Z_coefs


# Compute pupil mask for given diameter
# 
# input: N - size of array and diameter of pupil

def pupil(N):

    p = np.zeros([N,N])

    radius = N/2
    [X,Y] = np.meshgrid(np.linspace(-(N-1)/2,(N-1)/2,N),np.linspace(-(N-1)/2,(N-1)/2,N))
    R = np.sqrt(pow(X,2)+pow(Y,2))
    p[R<=radius] = 1
    
    return p



if __name__ == '__main__':
    print('Turbulence Simulator Tool')
    
    shmName = '/tmp/dm.im.shm'
    shmMapName = '/tmp/dm241Map.im.shm'
    diameter = 10 # m
    r0 = 0.1 # m
    wSpeed = 10 # m/s
    wDirection = -45 # deg
    dt = 0.005 # s
    period = 100 # total time period

    try:
        opts, args = getopt.getopt(sys.argv[1:],"hf:d:r:s:o:t:p",["help", "fileShm=", "diameter=", "r0=", "speed=", "orientation=", "timeDelta=", "period="])
    except getopt.GetoptError:
      print('err, usage: daoTurbulenceSimulator.py -f <file Shm> -d <diameter> -r <r0> -s <speed> -o <orientation> -t <dt> -p <period>')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoTurbulenceSimulator.py -f <file Shm> -d <diameter> -r <r0> -s <speed> -o <orientation> -t <dt> -p <period>')
            sys.exit()
        elif opt in ("-f", "--fileShm"):
            shmName = str(arg)
        elif opt in ("-d", "--diameter"):
            diameter = float(arg)
        elif opt in ("-r", "--r0"):
            r0 = float(arg)
        elif opt in ("-s", "--speed"):
            wSpeed = float(arg)
        elif opt in ("-o", "--orientation"):
            wDirection = float(arg)
        elif opt in ("-t", "--timeDelta"):
            dt = float(arg)
        elif opt in ("-p", "--period"):
            period = float(arg)

    shm = dao.shm(shmName)       
    shmMap = dao.shm(shmMapName)       
    # Get number of actuator
    nAct = shmMap.get_data().shape[0]

    try:
        while 1:
            applyTub(shm, shmMap, nAct, diameter, r0, wSpeed, wDirection, dt, period)
    except KeyboardInterrupt:
        shm.set_data(0*shm.get_data())

