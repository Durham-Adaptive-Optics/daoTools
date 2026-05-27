#!/usr/bin/env python3
"""
TTM Display - Real-time X,Y position display for Tip-Tilt Mirror

Usage: ttmDisplay.py <shmName> [options]

Options:
  --range  Display range in controller units (default: 100.0)
  --help   Show this help message

Example:
  ttmDisplay.py /tmp/ttm1Cmd.im.shm
  ttmDisplay.py /tmp/ttm1Cmd.im.shm --range 50
"""

import dao
import numpy as np
import argparse
import sys
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtWidgets


def parse_args():
    parser = argparse.ArgumentParser(
        description='TTM Display - Real-time X,Y position display',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s /tmp/ttm1Cmd.im.shm
  %(prog)s /tmp/ttm1Cmd.im.shm --range 50
        """
    )
    parser.add_argument('shmName',
                        help='Shared memory path (e.g. /tmp/ttm1Cmd.im.shm)')
    parser.add_argument('--range', type=float, default=100.0,
                        help='Display range in controller units (default: 100.0)')
    return parser.parse_args()


def main():
    args = parse_args()

    ttm = dao.shm(args.shmName)
    half = args.range / 2.0
    center = args.range / 2.0

    app = QtWidgets.QApplication(sys.argv)

    # Main window
    win = pg.GraphicsLayoutWidget(title=f"TTM Display - {args.shmName}")
    win.resize(600, 620)
    win.setBackground('k')

    # Plot area
    plot = win.addPlot(title="Tip-Tilt Position")
    plot.setXRange(0, args.range)
    plot.setYRange(0, args.range)
    plot.setAspectLocked(True)
    plot.showGrid(x=True, y=True, alpha=0.2)
    plot.getAxis('bottom').setLabel('Axis A (units)')
    plot.getAxis('left').setLabel('Axis B (units)')

    # Target circles
    for r in [half * 0.25, half * 0.5, half * 0.75, half]:
        circle = pg.QtWidgets.QGraphicsEllipseItem(
            center - r, center - r, r * 2, r * 2
        )
        circle.setPen(pg.mkPen(color=(80, 80, 80), width=1, style=QtCore.Qt.PenStyle.DashLine))
        circle.setBrush(pg.mkBrush(None))
        plot.addItem(circle)

    # Crosshair lines at center
    hline = pg.InfiniteLine(pos=center, angle=0,
                            pen=pg.mkPen(color=(80, 80, 80), width=1,
                                         style=QtCore.Qt.PenStyle.DashLine))
    vline = pg.InfiniteLine(pos=center, angle=90,
                            pen=pg.mkPen(color=(80, 80, 80), width=1,
                                         style=QtCore.Qt.PenStyle.DashLine))
    plot.addItem(hline)
    plot.addItem(vline)

    # Trail (last N positions)
    TRAIL_LEN = 200
    trail_x = np.full(TRAIL_LEN, center)
    trail_y = np.full(TRAIL_LEN, center)
    trail_plot = plot.plot(trail_x, trail_y,
                           pen=pg.mkPen(color=(0, 150, 255, 80), width=1))

    # Current position dot
    dot = plot.plot([center], [center],
                    pen=None,
                    symbol='o',
                    symbolSize=12,
                    symbolBrush=pg.mkBrush(0, 255, 100),
                    symbolPen=pg.mkPen('w', width=1))

    # Status label
    label = pg.LabelItem(justify='left')
    win.nextRow()
    win.addItem(label)

    idx = [0]  # mutable counter for trail

    def update():
        try:
            cmd = ttm.get_data()
            a = float(cmd[0, 0])
            b = float(cmd[1, 0])
        except Exception:
            return

        # Update trail
        trail_x[idx[0] % TRAIL_LEN] = a
        trail_y[idx[0] % TRAIL_LEN] = b
        idx[0] += 1

        # Reorder trail so it draws oldest -> newest
        i = idx[0] % TRAIL_LEN
        tx = np.roll(trail_x, -i)
        ty = np.roll(trail_y, -i)
        trail_plot.setData(tx, ty)

        # Update dot
        dot.setData([a], [b])

        # Update label
        da = a - center
        db = b - center
        label.setText(
            f"<span style='color:#00ff64; font-size:14px'>"
            f"A={a:.2f}  B={b:.2f} &nbsp;&nbsp; "
            f"ΔA={da:+.2f}  ΔB={db:+.2f}"
            f"</span>"
        )

    timer = QtCore.QTimer()
    timer.timeout.connect(update)
    timer.start(10)  # 100 Hz

    win.show()
    sys.exit(app.exec())


if __name__ == '__main__':
    main()