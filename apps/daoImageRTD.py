#!/usr/bin/env python3

import numpy as np
import dao
import getopt
import sys
import time
import matplotlib
import matplotlib.pyplot as plt

matplotlib.rcParams['image.interpolation'] = 'None'


def press(event):
    '''
    handles keyboard events, in case 'q' key is pressed, the program is terminated
    '''
    print('pressed: ', event.key)
    sys.stdout.flush()
    if event.key == 'q':
        print('terminating the program')
        sys.exit(0)

def winClose(ha):
    '''
    handles window close event, the program is terminated
    '''
    #print('window closed: ', ha)
    sys.stdout.flush()
    print('terminating the program')
    sys.exit(0)

if __name__ == '__main__':
    shmimName = 'WFSim.im.sh'
    axisFlip = False
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hs:a",["help", "shmimName=", "axisFlip"])
    except getopt.GetoptError:
      print('err, usage: doaImageRTD.py -s <shmimName> -a')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoImageRTD.py -s <shmimName>')
            sys.exit()
        elif opt in ("-s", "--shm"):
            shmimName = str(arg)
        elif opt in ("-a", "--axisFlip"):
            axisFlip = True
    shm = dao.shm(shmimName)
    imageSizeX = shm.get_data().shape[0]
    imageSizeY = shm.get_data().shape[1]
    if axisFlip == True:
        imageSizeX = shm.get_data().shape[1]
        imageSizeY = shm.get_data().shape[0]

    plt.ion()
    fig = plt.figure(shmimName[5:])
    #fig.canvas.mpl_connect('key_press_event', press)
    fig.canvas.mpl_connect('close_event', winClose)
    ax = fig.add_subplot(111)
    im=shm.get_data()
    im[0:2,0]=0
    im=im.flatten().reshape((imageSizeX,imageSizeY))
    a=plt.imshow(im, cmap='turbo')
    plt.title('press \'q\' to close')
    plt.colorbar()
    plt.tight_layout()
    plt.show()
    while True:
        im=shm.get_data()
        im[0:2,0]=0
        im=im.flatten().reshape((imageSizeX,imageSizeY))
        #data=shm.get_data().flatten().reshape((imageSizeX,imageSizeY))
        data=im
        dmax=data.max()
        a.set_data(data)
        #a.set_clim(0,dmax)
        fig.canvas.draw()
        fig.canvas.flush_events()
        time.sleep(1/60.)
