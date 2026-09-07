#!/usr/bin/env python3
"""
DM Channels Control - overview of every DM command channel, with a button per
raw channel to open daoDmCtrl.py (M2A-modal poke sliders) on that channel.

Shows the DM command map for each numbered channel (<name>00, <name>01,
<name>02, <name>03 -- default labels Flat/Loop/Turbulence/Pokes, see -l) plus
the combined command (<name>), each drawn in 2D through the DM actuator map --
same channel scheme as daoDmDisp.py. One button per raw channel spawns
daoDmCtrl.py on that channel's shm for modal poking, so you can poke a
channel while the loop runs.

Usage: daoDmChannelsCtrl.py [options]

Options:
  -s  DM command base name              (default: dmCmd)
  -m  DM actuator map name              (default: dmMap)
  -z  Modes-to-command (M2A) matrix name, forwarded to daoDmCtrl.py -z (default: dmM2A)
  -l  Channel labels for 00,01,02,03, comma-separated (default: Flat,Loop,Turbulence,Pokes)
  --light  Light mode (default: dark)

Examples:
  daoDmChannelsCtrl.py
  daoDmChannelsCtrl.py -s dmCmd -m dmMap -z dmM2A
  daoDmChannelsCtrl.py -l Flat,ClosedLoop,Kolmogorov,Pokes
  daoDmChannelsCtrl.py --light
"""

from PyQt5.uic import loadUiType
import sys
import getopt
import os
import subprocess
import numpy as np
import pyqtgraph as pg
from PyQt5.QtWidgets import QApplication
from PyQt5 import QtCore
import dao

path = os.getenv('DAOROOT') + '/data/'
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(path, 'daoDmChannelsCtrl.ui'))

COLORMAPS = {
    'grey':    [(0,0,0),(255,255,255)],
    'viridis': [(68,1,84),(59,82,139),(33,145,140),(94,201,98),(253,231,37)],
    'inferno': [(0,0,4),(120,28,109),(238,71,41),(252,177,57),(252,255,164)],
}

def make_colormap(name):
    colors = COLORMAPS.get(name, COLORMAPS['viridis'])
    return pg.ColorMap(np.linspace(0, 1, len(colors)), colors)


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color: #f0f0f0; color: #000000; }
            QLabel { color: #000000; }
            QPushButton {
                background-color: #e0e0e0; color: #000000;
                border: 1px solid #aaa; padding: 3px 6px; border-radius: 3px;
            }
            QPushButton:hover   { background-color: #d0d0d0; }
            QPushButton:pressed { background-color: #bbb; }
        """
    else:
        return """
            QMainWindow, QWidget { background-color: #1e1e1e; color: #cccccc; }
            QLabel { color: #cccccc; }
            QPushButton {
                background-color: #3a3a3a; color: #cccccc;
                border: 1px solid #555; padding: 3px 6px; border-radius: 3px;
            }
            QPushButton:hover   { background-color: #4a4a4a; }
            QPushButton:pressed { background-color: #555; }
        """


class DmShapeView:
    """Renders a DM command vector, masked into the 2D actuator map, in a GraphicsView."""
    def __init__(self, graphicsView, mask, colormap='viridis'):
        self.vb = pg.ViewBox()
        self.vb.setAspectLocked(True)
        self.vb.setDefaultPadding(0)
        self.vb.setBorder(None)
        graphicsView.setCentralItem(self.vb)
        self.img = pg.ImageItem()
        self.img.setColorMap(make_colormap(colormap))
        self.vb.addItem(self.img)
        self._buf = np.zeros_like(mask, dtype=np.float32)

    def update_shape(self, cmd, mask):
        self._buf[mask == 1] = cmd[:, 0]
        self.img.setImage(self._buf)


DEFAULT_LABELS = ('Flat', 'Loop', 'Turbulence', 'Pokes')  # channels 00, 01, 02, 03


class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, name, mapName, m2aName, labels=DEFAULT_LABELS):
        super(Main, self).__init__()
        self.setupUi(self)

        self.name       = name
        self.mapShmPath = f'/tmp/{mapName}.im.shm'
        self.m2aShmPath = f'/tmp/{m2aName}.im.shm'
        self._children  = []

        self.shmMap = dao.shm(self.mapShmPath)
        self.dmMask = self.shmMap.get_data()

        # suffix -> (title, shm path, has "open channel ctrl" button)
        channels = [
            ('0',     f'{name}00 — {labels[0]}', f'/tmp/{name}00.im.shm', True),
            ('1',     f'{name}01 — {labels[1]}', f'/tmp/{name}01.im.shm', True),
            ('total', f'{name} — Combined',      f'/tmp/{name}.im.shm',   False),
            ('2',     f'{name}02 — {labels[2]}', f'/tmp/{name}02.im.shm', True),
            ('3',     f'{name}03 — {labels[3]}', f'/tmp/{name}03.im.shm', True),
        ]

        self.panels = {}
        for suffix, title, shmPath, hasBtn in channels:
            getattr(self, f'titleLabel_{suffix}').setText(title)
            gv = getattr(self, f'graphicsView_{suffix}')
            view = DmShapeView(gv, self.dmMask)
            self.panels[suffix] = {
                'shm':    dao.shm(shmPath),
                'view':   view,
                'minmax': getattr(self, f'minmaxLabel_{suffix}'),
            }
            if hasBtn:
                btn = getattr(self, f'openButton_{suffix}')
                btn.clicked.connect(self._makeOpenCB(shmPath))

        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(200)
        self.timer.timeout.connect(self.Update)

    def _makeOpenCB(self, shmPath):
        def cb():
            self._openChannelCtrl(shmPath)
        return cb

    def _openChannelCtrl(self, shmPath):
        here = os.path.dirname(os.path.abspath(__file__))
        script = os.path.join(here, 'daoDmCtrl.py')
        cmd = [sys.executable, script,
               '-i', self.name,
               '-s', shmPath,
               '-m', self.mapShmPath,
               '-z', self.m2aShmPath]
        try:
            self._children.append(subprocess.Popen(cmd))
        except Exception as exc:
            print(f'failed to open {script}: {exc}')

    def Start(self):
        self.timer.start()
        self.Update()

    def Stop(self):
        self.timer.stop()

    @QtCore.pyqtSlot()
    def Update(self):
        mask = self.shmMap.get_data()
        for panel in self.panels.values():
            cmd = panel['shm'].get_data()
            panel['view'].update_shape(cmd, mask)
            panel['minmax'].setText('min %.3f   max %.3f' % (np.min(cmd[:, 0]), np.max(cmd[:, 0])))


if __name__ == '__main__':
    name    = 'dmCmd'
    mapName = 'dmMap'
    m2aName = 'dmM2A'
    labels  = list(DEFAULT_LABELS)
    light   = False
    usage = ('daoDmChannelsCtrl.py -s <name> -m <map> -z <m2aShm> [-l <chan00,chan01,chan02,chan03>] [--light]'
              '  (default labels: %s)' % ','.join(DEFAULT_LABELS))
    try:
        opts, args = getopt.getopt(sys.argv[1:], "hs:m:z:l:",
                                    ["help", "name=", "map=", "m2a=", "labels=", "light"])
    except getopt.GetoptError:
        print('err, usage: ' + usage)
        sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print(usage)
            sys.exit()
        elif opt in ("-s", "--name"):
            name = arg
        elif opt in ("-m", "--map"):
            mapName = arg
        elif opt in ("-z", "--m2a"):
            m2aName = arg
        elif opt in ("-l", "--labels"):
            parts = [p.strip() for p in arg.split(',')]
            if len(parts) != 4:
                print('err, -l/--labels needs exactly 4 comma-separated names (00,01,02,03)')
                sys.exit(2)
            labels = parts
        elif opt == '--light':
            light = True

    if light:
        pg.setConfigOption('background', '#f0f0f0')
        pg.setConfigOption('foreground', '#000000')
    else:
        pg.setConfigOption('background', '#1e1e1e')
        pg.setConfigOption('foreground', '#cccccc')

    app = QApplication([])
    app.setStyleSheet(make_stylesheet(light))

    main = Main(name, mapName, m2aName, labels)
    main.setWindowTitle(f'DM Channels — {name}')
    main.show()
    main.Start()
    sys.exit(app.exec_())
