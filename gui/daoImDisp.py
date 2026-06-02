#!/usr/bin/env python3
"""
Image Display - Real-time image display from shared memory (no .ui file)

Usage: daoImDisp.py <shmName> [options]

Options:
  --fps   Update rate in Hz (default: 10)
  --size  Display size in pixels (default: 600)
  --light Use light mode (default: dark)

Examples:
  daoImDisp.py /tmp/ws00image.im.shm
  daoImDisp.py /tmp/ws00image.im.shm --fps 20 --light
"""

import sys
import argparse
import numpy as np
import time
import dao
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtWidgets


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


def parse_args():
    p = argparse.ArgumentParser(description='Real-time image display from shared memory')
    p.add_argument('shmName',            help='Shared memory path')
    p.add_argument('--fps',  type=float, default=10.0, help='Update rate in Hz (default: 10)')
    p.add_argument('--size', type=int,   default=600,  help='Display size in pixels (default: 600)')
    p.add_argument('--light', action='store_true',     help='Light mode (default: dark)')
    return p.parse_args()


def main():
    args = parse_args()

    if args.light:
        pg.setConfigOption('background', '#f0f0f0')
        pg.setConfigOption('foreground', '#000000')
    else:
        pg.setConfigOption('background', '#1e1e1e')
        pg.setConfigOption('foreground', '#cccccc')

    shm = dao.shm(args.shmName)
    data0 = shm.get_data()
    nx, ny = data0.shape[0], data0.shape[1]
    print(f"Image: {nx}x{ny}  shm: {args.shmName}")

    app = QtWidgets.QApplication(sys.argv)

    win = QtWidgets.QMainWindow()
    win.setWindowTitle(f"{args.shmName}  [{nx}x{ny}]")
    win.setStyleSheet(make_stylesheet(args.light))

    central = QtWidgets.QWidget()
    win.setCentralWidget(central)
    layout = QtWidgets.QVBoxLayout(central)
    layout.setContentsMargins(4, 4, 4, 4)
    layout.setSpacing(2)

    # --- Line 1: freq + max ---
    info_color = '#000000' if args.light else '#cccccc'
    info_label = QtWidgets.QLabel("0.00 Hz    max=--")
    info_label.setStyleSheet(f"font-family: monospace; font-size: 11px; color: {info_color};")
    layout.addWidget(info_label)

    # --- Line 2: log checkbox ---
    line2 = QtWidgets.QHBoxLayout()
    log_cb = QtWidgets.QCheckBox("Log")
    log_cb.setChecked(False)
    line2.addWidget(log_cb)
    line2.addStretch()
    layout.addLayout(line2)

    # --- Image + histogram side by side ---
    panels = QtWidgets.QHBoxLayout()
    layout.addLayout(panels)

    gw_bg = '#f0f0f0' if args.light else '#1e1e1e'

    # Image view
    gw = pg.GraphicsLayoutWidget()
    gw.setBackground(gw_bg)
    gw.setFixedSize(args.size, args.size)
    panels.addWidget(gw)

    vb = pg.ViewBox()
    gw.setCentralItem(vb)
    vb.setAspectLocked()
    vb.setRange(QtCore.QRectF(0, 0, nx, ny))

    img_item = pg.ImageItem()
    vb.addItem(img_item)

    # Histogram / LUT
    gw_hist = pg.GraphicsLayoutWidget()
    gw_hist.setBackground(gw_bg)
    gw_hist.setFixedWidth(120)
    gw_hist.setFixedHeight(args.size)
    panels.addWidget(gw_hist)

    hist = pg.HistogramLUTItem()
    hist.setImageItem(img_item)
    gw_hist.setCentralItem(hist)
    hist.vb.setMouseEnabled(y=False)

    # State
    t_ref  = [time.time()]
    cnt_ref = [shm.get_counter()]

    def update():
        im = shm.get_data().astype(float)
        disp = im.reshape(nx, ny)
        if log_cb.isChecked():
            disp = disp.copy()
            disp[disp <= 0] = 1e-12
            disp = np.log(disp)
        img_item.setImage(np.rot90(disp, 3))

        cnt2 = shm.get_counter()
        t2   = time.time()
        dt   = t2 - t_ref[0]
        freq = (cnt2 - cnt_ref[0]) / dt if dt > 0 else 0.0
        t_ref[0]   = t2
        cnt_ref[0] = cnt2

        info_label.setText(f"{freq:.2f} Hz    max={im.max():.1f}")

    timer = QtCore.QTimer()
    timer.timeout.connect(update)
    timer.start(int(1000 / args.fps))

    win.adjustSize()
    win.show()
    sys.exit(app.exec())


if __name__ == '__main__':
    main()