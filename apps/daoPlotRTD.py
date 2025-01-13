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
    shmimName = 'test.im.sh'
    try:
        opts, args = getopt.getopt(sys.argv[1:],"hs:",["help", "shmimName="])
    except getopt.GetoptError:
      print('err, usage: daoPlotRTD.py -s <shmimName> -a')
      sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('daoPlotRTD.py -s <shmimName>')
            sys.exit()
        elif opt in ("-s", "--shm"):
            shmimName = str(arg)
    shm = dao.shm(shmimName)

    plt.ion()
    fig = plt.figure(shmimName[5:])
    fig.canvas.mpl_connect('key_press_event', press)
    fig.canvas.mpl_connect('close_event', winClose)
    ax = fig.add_subplot(111)

    data = shm.get_data().flatten()  # Assuming shm.get_data() returns a 1D array
    p = ax.plot(np.arange(len(data)), data, color='blue')
    ax.set_ylim(0,1)
    plt.title('press \'q\' to close')
    plt.xlabel('Index')
    plt.ylabel('Value')
    plt.tight_layout()
    plt.show()

    while True:
        data = shm.get_data().flatten()
        p[0].set_ydata(data)
        fig.canvas.draw()
        fig.canvas.flush_events()
        time.sleep(1/60.)
