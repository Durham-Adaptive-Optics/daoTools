#!/usr/bin/env python3

import sys
from PyQt5 import QtCore
from PyQt5.QtWidgets import (
    QApplication, QComboBox, QGridLayout, QHBoxLayout, QLabel, QMainWindow,
    QPushButton, QScrollArea, QVBoxLayout, QWidget,
)
import numpy as np
import pyqtgraph as pg
import time
import dao

COLORMAPS = {
    'grey':    [(0,0,0),(255,255,255)],
    'viridis': [(68,1,84),(59,82,139),(33,145,140),(94,201,98),(253,231,37)],
    'inferno': [(0,0,4),(120,28,109),(238,71,41),(252,177,57),(252,255,164)],
    'plasma':  [(13,8,135),(126,3,168),(204,71,120),(248,149,64),(240,249,33)],
    'bwr':     [(0,60,200),(255,255,255),(200,30,30)],
}
DIVERGING_CMAPS = {'bwr'}

def make_colormap(name):
    colors = COLORMAPS.get(name, COLORMAPS['grey'])
    return pg.ColorMap(np.linspace(0, 1, len(colors)), colors)


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color: #f0f0f0; color: #000000; }
            QPushButton {
                background-color: #e0e0e0; color: #000000;
                border: 1px solid #aaa; padding: 4px 8px; border-radius: 3px;
            }
            QPushButton:hover   { background-color: #d0d0d0; }
            QPushButton:pressed { background-color: #bbb; }
            QCheckBox, QLabel   { color: #000000; }
            QDoubleSpinBox, QComboBox {
                background-color: #ffffff; color: #000000; border: 1px solid #aaa;
            }
        """
    else:
        return """
            QMainWindow, QWidget { background-color: #1e1e1e; color: #cccccc; }
            QPushButton {
                background-color: #3a3a3a; color: #cccccc;
                border: 1px solid #555; padding: 4px 8px; border-radius: 3px;
            }
            QPushButton:hover   { background-color: #4a4a4a; }
            QPushButton:pressed { background-color: #555; }
            QCheckBox, QLabel   { color: #cccccc; }
            QCheckBox::indicator { width: 14px; height: 14px; }
            QCheckBox::indicator:unchecked { background-color: #aaaaaa; border: 1px solid #ccc; }
            QDoubleSpinBox, QComboBox {
                background-color: #3a3a3a; color: #cccccc; border: 1px solid #555;
            }
        """


DEFAULT_LABELS = ('Flat', 'Loop', 'Turbulence', 'Pokes')  # channels 00, 01, 02, 03


class Main(QMainWindow):
    def __init__(self, name, mapName, labels=None, channels=4):
        super(Main, self).__init__()
        if channels < 1:
            raise ValueError('channels must be a positive integer')
        if labels is None:
            labels = [
                DEFAULT_LABELS[i] if i < len(DEFAULT_LABELS) else f'Channel {i:02d}'
                for i in range(channels)
            ]
        if len(labels) != channels:
            raise ValueError(f'expected {channels} channel labels')

        self.shmdm = dao.shm(f'/tmp/{name}.im.shm')
        self.channel_shms = [
            dao.shm(f'/tmp/{name}{i:02d}.im.shm') for i in range(channels)
        ]
        self.map = dao.shm(f'/tmp/{mapName}.im.shm')
        self.dmMask = self.map.get_data()
        self.active_actuators = self.dmMask == 1
        # Keep the combined stream first in the data lists, but show it last.
        self.streams = [self.shmdm] + self.channel_shms
        self.dm_arrays = [self.dmMask.astype(np.float32) for _ in self.streams]
        self.dmM = self.dm_arrays[0]

        central = QWidget()
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)
        self.titleLabel = QLabel(f'DM Display — {name}  (map: {mapName})')
        self.titleLabel.setAlignment(QtCore.Qt.AlignCenter)
        layout.addWidget(self.titleLabel)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        panels = QWidget()
        self.grid = QGridLayout(panels)
        scroll.setWidget(panels)
        layout.addWidget(scroll)
        self.images = [None] * len(self.streams)
        titles = [f'{name} — Combined'] + [
            f'{name}{i:02d} — {label}' for i, label in enumerate(labels)
        ]
        for position, index in enumerate(list(range(1, channels + 1)) + [0]):
            panel = QWidget()
            panel_layout = QVBoxLayout(panel)
            label = QLabel(titles[index])
            label.setAlignment(QtCore.Qt.AlignCenter)
            panel_layout.addWidget(label)
            view = pg.GraphicsView()
            view.setMinimumSize(150, 150)
            vb = pg.ViewBox()
            vb.setAspectLocked(True)
            vb.setDefaultPadding(0)
            vb.setBorder(None)
            view.setCentralItem(vb)
            img = pg.ImageItem()
            vb.addItem(img)
            self.images[index] = img
            panel_layout.addWidget(view)
            row, column = divmod(position, 3)
            self.grid.addWidget(panel, row, column)
            self.grid.setRowStretch(row, 1)
        for column in range(3):
            self.grid.setColumnStretch(column, 1)

        controls = QHBoxLayout()
        controls.addWidget(QLabel('Colormap:'))
        self.cmapCombo = QComboBox()
        controls.addWidget(self.cmapCombo)
        self.pushButton = QPushButton('Reset All')
        controls.addWidget(self.pushButton)
        controls.addStretch()
        self.minLabel = QLabel('0.0')
        self.maxLabel = QLabel('0.0')
        controls.addWidget(QLabel('Min:'))
        controls.addWidget(self.minLabel)
        controls.addWidget(QLabel('Max:'))
        controls.addWidget(self.maxLabel)
        layout.addLayout(controls)
        rows = (channels + 3) // 3
        self.resize(660, min(180 * rows + 100, 850))

        self.cmapName = 'grey'
        for cmapName in COLORMAPS:
            self.cmapCombo.addItem(cmapName)
        self.cmapCombo.setCurrentText(self.cmapName)
        self.cmapCombo.currentTextChanged.connect(self.ChangeColormap)
        for img in self.images:
            img.setColorMap(make_colormap(self.cmapName))

        self.pushButton.clicked.connect(self.ResetAll)

        self.imCnt1 = self.shmdm.get_counter()
        self.timer  = QtCore.QTimer(self)
        self.timer.setInterval(100)
        self.timer.timeout.connect(self.Update)
        self.t1 = time.time()
        self.Update()

    def Start(self):
        self.timer.start()

    def Stop(self):
        self.timer.stop()

    def ResetAll(self):
        zeroCmd = self.shmdm.get_data() * 0
        for s in self.streams:
            s.set_data(zeroCmd)

    def ChangeColormap(self, name):
        self.cmapName = name
        cm = make_colormap(name)
        for img in self.images:
            img.setColorMap(cm)
        self.Update()

    def SetImage(self, imgItem, data):
        if self.cmapName in DIVERGING_CMAPS:
            # Center a diverging colormap on zero so push/pull actuator strokes are symmetric.
            vmax = float(max(abs(data.min()), abs(data.max()), 1e-12))
            imgItem.setImage(data, levels=(-vmax, vmax), autoLevels=False)
        else:
            imgItem.setImage(data)

    @QtCore.pyqtSlot()
    def Update(self):
        for stream, data, img in zip(self.streams, self.dm_arrays, self.images):
            data[self.active_actuators] = stream.get_data()[:, 0]
            self.SetImage(img, data)

        self.minLabel.setText(str(np.min(self.dmM)))
        self.maxLabel.setText(str(np.max(self.dmM)))

        self.imCnt2 = self.shmdm.get_counter()
        self.t2 = time.time()
        self.t1 = self.t2
        self.imCnt1 = self.imCnt2


def parse_args(argv=None):
    import argparse

    parser = argparse.ArgumentParser(
        description='Display DM channels and the combined command in a three-column grid.')
    parser.add_argument('-s', '--name', default='dmCmd', help='DM stream name')
    parser.add_argument('-m', '--map', default='dmMap', help='actuator map stream name')
    parser.add_argument('-n', '--channels', type=int, default=4,
                        help='number of channels, starting at 00 (default: 4)')
    parser.add_argument('-l', '--labels',
                        help='comma-separated labels, one per channel; defaults to '
                             'Flat, Loop, Turbulence, Pokes, then numbered labels')
    parser.add_argument('--light', action='store_true', help='use the light theme')
    args = parser.parse_args(argv)
    if args.channels < 1:
        parser.error('-n/--channels must be a positive integer')
    if args.labels is not None:
        args.labels = [label.strip() for label in args.labels.split(',')]
        if len(args.labels) != args.channels:
            parser.error(f'-l/--labels needs exactly {args.channels} comma-separated names')
    return args


if __name__ == '__main__':
    args = parse_args()
    name, mapName, light = args.name, args.map, args.light

    if light:
        pg.setConfigOption('background', '#f0f0f0')
        pg.setConfigOption('foreground', '#000000')
    else:
        pg.setConfigOption('background', '#1e1e1e')
        pg.setConfigOption('foreground', '#cccccc')

    app = QApplication([])
    app.setStyleSheet(make_stylesheet(light))

    main = Main(name, mapName, args.labels, args.channels)
    main.setWindowTitle(f'DM Display — {name}')
    main.show()
    main.Start()
    sys.exit(app.exec_())
