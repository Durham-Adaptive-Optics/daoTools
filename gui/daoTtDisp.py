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
        QCheckBox { color: #cccccc; font-family: monospace; font-size: 12px; }
        QCheckBox::indicator { width: 14px; height: 14px; }
        QCheckBox::indicator:unchecked { background-color: #aaaaaa; border: 1px solid #ccc; }
        QSpinBox {
            background-color: #2d2d2d; color: #cccccc;
            border: 1px solid #555555; border-radius: 3px;
            font-family: monospace; font-size: 12px;
            padding: 1px 4px;
        }
        QSpinBox::up-button, QSpinBox::down-button { width: 16px; }
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

    # --- Line 2: rolling average controls ---
    ravg_layout = QtWidgets.QHBoxLayout()
    ravg_layout.setSpacing(6)

    ravg_check = QtWidgets.QCheckBox("Rolling average")
    ravg_check.setChecked(False)

    ravg_spin = QtWidgets.QSpinBox()
    ravg_spin.setMinimum(2)
    ravg_spin.setMaximum(10000)
    ravg_spin.setValue(100)
    ravg_spin.setFixedWidth(70)
    ravg_spin.setToolTip("Number of samples for rolling average")

    ravg_unit = QtWidgets.QLabel("samples")
    ravg_unit.setStyleSheet("font-family: monospace; font-size: 12px; color: #888888;")

    ravg_layout.addWidget(ravg_check)
    ravg_layout.addWidget(ravg_spin)
    ravg_layout.addWidget(ravg_unit)
    ravg_layout.addStretch()
    layout.addLayout(ravg_layout)

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

    # Current position dot (raw)
    dot = plot.plot([0], [0],
                    pen=None, symbol='o', symbolSize=12,
                    symbolBrush=pg.mkBrush(0, 255, 100),
                    symbolPen=pg.mkPen('w', width=1))

    # Rolling average dot (orange, hidden by default)
    dot_avg = plot.plot([0], [0],
                        pen=None, symbol='o', symbolSize=10,
                        symbolBrush=pg.mkBrush(255, 160, 0),
                        symbolPen=pg.mkPen('w', width=1))
    dot_avg.setVisible(False)

    # Rolling average buffers (pre-allocated to max spinbox size)
    BUF_MAX = 10000
    ravg_buf_a = np.zeros(BUF_MAX)
    ravg_buf_b = np.zeros(BUF_MAX)
    ravg_idx = [0]
    ravg_count = [0]

    idx = [0]

    def update():
        try:
            cmd = ttm.get_data()
            a = float(cmd[0, 0])
            b = float(cmd[1, 0])
        except Exception:
            return

        # --- Trail ---
        trail_x[idx[0] % TRAIL_LEN] = a
        trail_y[idx[0] % TRAIL_LEN] = b
        idx[0] += 1
        i = idx[0] % TRAIL_LEN
        trail_plot.setData(np.roll(trail_x, -i), np.roll(trail_y, -i))

        # --- Raw dot ---
        dot.setData([a], [b])

        # --- Rolling average ---
        use_ravg = ravg_check.isChecked()
        dot_avg.setVisible(use_ravg)

        n = ravg_spin.value()
        ri = ravg_idx[0] % BUF_MAX
        ravg_buf_a[ri] = a
        ravg_buf_b[ri] = b
        ravg_idx[0] += 1
        ravg_count[0] = min(ravg_count[0] + 1, BUF_MAX)

        if use_ravg:
            # Use only the last n samples, capped to what we have
            k = min(n, ravg_count[0])
            # indices of the last k samples in circular buffer
            end = ravg_idx[0]          # one past last written (in unbounded space)
            start = end - k
            indices = np.arange(start, end) % BUF_MAX
            avg_a = float(np.mean(ravg_buf_a[indices]))
            avg_b = float(np.mean(ravg_buf_b[indices]))
            dot_avg.setData([avg_a], [avg_b])
            info_label.setText(
                f"A={a:+.4f}  B={b:+.4f}    "
                f"Ā={avg_a:+.4f}  B̄={avg_b:+.4f}  (n={k})"
            )
        else:
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