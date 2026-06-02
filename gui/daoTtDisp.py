#!/usr/bin/env python3
"""
Tip-Tilt Display - Real-time X,Y position display

Usage: daoTtDisp.py <shmName> [options]

Options:
  --range  Half-range in controller units, display spans -range..+range (default: 1.0)
  --fps    Update rate in Hz (default: 100)

Examples:
  daoTtDisp.py /tmp/ttm1Cmd.im.shm
  daoTtDisp.py /tmp/ttm1Cmd.im.shm --range 0.5
"""

import dao
import numpy as np
import argparse
import sys
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtWidgets

pg.setConfigOption('background', '#1e1e1e')
pg.setConfigOption('foreground', '#cccccc')


def parse_args():
    p = argparse.ArgumentParser(description='Tip-Tilt Display - Real-time X,Y position display')
    p.add_argument('shmName', help='Shared memory path (e.g. /tmp/ttm1Cmd.im.shm)')
    p.add_argument('--range',  type=float, default=1.0,
                   help='Display range (default: 1.0)')
    p.add_argument('--center', action='store_true',
                   help='Center at 0: spans -range..+range (default: 0..range)')
    p.add_argument('--fps',    type=float, default=100.0, help='Update rate in Hz (default: 100)')
    return p.parse_args()


def main():
    args = parse_args()
    if args.center:
        xmin, xmax = -args.range, args.range
        origin = 0.0
        half = args.range
    else:
        xmin, xmax = 0.0, args.range
        origin = args.range / 2.0
        half = args.range / 2.0

    ttm = dao.shm(args.shmName)

    app = QtWidgets.QApplication(sys.argv)

    win = QtWidgets.QMainWindow()
    win.setWindowTitle(f"TT Display  {args.shmName}")
    win.setStyleSheet("""
        QMainWindow, QWidget { background-color: #1e1e1e; color: #cccccc; }
        QLabel { color: #cccccc; font-family: monospace; font-size: 12px; }
    """)

    central = QtWidgets.QWidget()
    win.setCentralWidget(central)
    layout = QtWidgets.QVBoxLayout(central)
    layout.setContentsMargins(4, 4, 4, 4)
    layout.setSpacing(2)

    # --- Line 1: status readout ---
    info_label = QtWidgets.QLabel("A=--  B=--    ΔA=--  ΔB=--")
    info_label.setStyleSheet("font-family: monospace; font-size: 12px; color: #00ff64;")
    layout.addWidget(info_label)

    # --- Square plot ---
    gw = pg.GraphicsLayoutWidget()
    gw.setBackground('#1e1e1e')
    gw.setFixedSize(500, 500)
    layout.addWidget(gw, alignment=QtCore.Qt.AlignmentFlag.AlignLeft)

    plot = gw.addPlot()
    plot.setXRange(xmin, xmax, padding=0)
    plot.setYRange(xmin, xmax, padding=0)
    plot.setAspectLocked(True)
    plot.showGrid(x=True, y=True, alpha=0.2)
    plot.setDefaultPadding(0)
    plot.getAxis('bottom').setLabel('A')
    plot.getAxis('left').setLabel('B')

    # Concentric reference circles at 25/50/75/100% of half-range
    for frac in [0.25, 0.5, 0.75, 1.0]:
        r = half * frac
        circle = QtWidgets.QGraphicsEllipseItem(origin - r, origin - r, r * 2, r * 2)
        circle.setPen(pg.mkPen(color=(80, 80, 80), width=1,
                               style=QtCore.Qt.PenStyle.DashLine))
        circle.setBrush(pg.mkBrush(None))
        plot.addItem(circle)

    # Center crosshair
    plot.addItem(pg.InfiniteLine(pos=origin, angle=0,
                                 pen=pg.mkPen(color=(80, 80, 80), width=1,
                                              style=QtCore.Qt.PenStyle.DashLine)))
    plot.addItem(pg.InfiniteLine(pos=origin, angle=90,
                                 pen=pg.mkPen(color=(80, 80, 80), width=1,
                                              style=QtCore.Qt.PenStyle.DashLine)))

    # Trail
    TRAIL_LEN = 200
    trail_x = np.zeros(TRAIL_LEN)
    trail_y = np.zeros(TRAIL_LEN)
    trail_plot = plot.plot(trail_x, trail_y,
                           pen=pg.mkPen(color=(0, 150, 255, 80), width=1))

    # Current position dot
    dot = plot.plot([0], [0],
                    pen=None, symbol='o', symbolSize=12,
                    symbolBrush=pg.mkBrush(0, 255, 100),
                    symbolPen=pg.mkPen('w', width=1))

    idx = [0]

    def update():
        try:
            cmd = ttm.get_data()
            a = float(cmd[0, 0])
            b = float(cmd[1, 0])
        except Exception:
            return

        trail_x[idx[0] % TRAIL_LEN] = a
        trail_y[idx[0] % TRAIL_LEN] = b
        idx[0] += 1

        i = idx[0] % TRAIL_LEN
        trail_plot.setData(np.roll(trail_x, -i), np.roll(trail_y, -i))
        dot.setData([a], [b])

        info_label.setText(
            f"A={a:+.4f}  B={b:+.4f}    ΔA={a:+.4f}  ΔB={b:+.4f}"
        )

    timer = QtCore.QTimer()
    timer.timeout.connect(update)
    timer.start(int(1000 / args.fps))

    win.adjustSize()
    win.show()
    sys.exit(app.exec())


if __name__ == '__main__':
    main()