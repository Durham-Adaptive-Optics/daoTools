#!/usr/bin/env python3
"""
Tip-Tilt Display - Real-time X,Y position display

Usage: daoTtDisp.py <shmName> [options]

Options:
  --range        Half-range in controller units, display spans -range..+range (default: 1.0)
  --center       Center at 0: spans -range..+range (default: 0..range)
  --fps          Update rate in Hz (default: 100)
  --centre-file  where the target centres are remembered
                 (default $DAODATA/config/daoTtDispCentre.json)

Controls:
  AUTO           toggle: rescale both axes (kept square) to 1.5 x the largest
                 excursion from the target centre. Released, the last scale is kept.
  red cross      always visible, under the trail and dots, on the current
                 centre or on the point being proposed for it
  click in plot  move the cross there; the target does not move yet, so a
                 stray click costs nothing
  SET CENTRE     move the target (circles + crosshair) to the red cross;
                 enabled only once a point is marked
  CENTRE 0       put target and cross back at the default centre (0 with
                 --center, range/2 otherwise)

The centre is saved as soon as it moves, keyed by SHM name, and restored at the
next start. Delete the file to forget the centres.

Examples:
  daoTtDisp.py /tmp/ttm1Cmd.im.shm
  daoTtDisp.py /tmp/ttm1Cmd.im.shm --range 0.5
"""

import dao
import numpy as np
import argparse
import json
import os
import sys
from pathlib import Path
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtWidgets

pg.setConfigOption('background', '#1e1e1e')
pg.setConfigOption('foreground', '#cccccc')

TRAIL_LEN = 200
BUF_MAX = 10000
# Autoscale: half-range = AUTO_MARGIN x the largest excursion from the centre,
# never below AUTO_MIN_HALF, and only re-applied on a change bigger than
# AUTO_HYST so the axes do not jitter at the refresh rate.
AUTO_MARGIN = 1.5
AUTO_MIN_HALF = 1e-3
AUTO_HYST = 0.05


def parse_args():
    p = argparse.ArgumentParser(description='Tip-Tilt Display - Real-time X,Y position display')
    p.add_argument('shmName', help='Shared memory path (e.g. /tmp/ttm1Cmd.im.shm)')
    p.add_argument('--range',  type=float, default=1.0,
                   help='Display range (default: 1.0)')
    p.add_argument('--center', action='store_true',
                   help='Center at 0: spans -range..+range (default: 0..range)')
    p.add_argument('--fps',    type=float, default=100.0, help='Update rate in Hz (default: 100)')
    p.add_argument('--centre-file', default=None,
                   help='where target centres are remembered '
                        '(default $DAODATA/config/daoTtDispCentre.json)')
    return p.parse_args()


def centre_file_path(override=None):
    if override:
        return Path(override).expanduser()
    return Path(os.environ.get("DAODATA", "/tmp")) / "config" / "daoTtDispCentre.json"


def load_centres(path):
    """Saved centres as {key: [cx, cy]}. A missing or unreadable file is not an
    error: the display just starts on its default centre."""
    try:
        data = json.loads(Path(path).read_text())
    except FileNotFoundError:
        return {}
    except Exception as exc:                                    # noqa: BLE001
        print(f"daoTtDisp: ignoring {path}: {exc}", file=sys.stderr)
        return {}
    return data if isinstance(data, dict) else {}


def save_centre(path, key, cx, cy):
    """Read-modify-write, so displays of other SHMs do not drop each other's
    entry, and tmp+replace so a crash mid-write cannot leave a truncated file."""
    data = load_centres(path)
    data[key] = [cx, cy]
    path = Path(path)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + ".tmp")
        tmp.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        tmp.replace(path)
    except Exception as exc:                                    # noqa: BLE001
        print(f"daoTtDisp: cannot save centre to {path}: {exc}", file=sys.stderr)


class TtDisplay:
    """The plot and its controls: trail, current and rolling-average positions,
    reference circles and crosshair around a movable target centre."""

    def __init__(self, args):
        # (origin, origin) is the default centre: 0 with --center, range/2 otherwise
        self.origin = 0.0 if args.center else args.range / 2.0
        # half-range of the view: --range at start, then whatever AUTO last set
        self.half = args.range if args.center else args.range / 2.0
        self.cx = self.cy = self.origin
        self.centrePath = centre_file_path(args.centre_file)
        self.centreKey = args.shmName
        self.ttm = dao.shm(args.shmName)

        self.win = QtWidgets.QMainWindow()
        self.win.setWindowTitle(f"TT Display  {args.shmName}")
        self.win.setStyleSheet("""
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
            QPushButton {
                background-color: #2d2d2d; color: #cccccc;
                border: 1px solid #555555; border-radius: 3px;
                font-family: monospace; font-size: 12px;
                padding: 1px 6px;
            }
            QPushButton:checked { background-color: #005a2d; color: #00ff64; border-color: #00ff64; }
            QPushButton:disabled { color: #666666; border-color: #3a3a3a; }
        """)

        central = QtWidgets.QWidget()
        self.win.setCentralWidget(central)
        layout = QtWidgets.QVBoxLayout(central)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(2)

        # --- Line 1: status readout ---
        self.info_label = QtWidgets.QLabel("A=--  B=--    ΔA=--  ΔB=--")
        self.info_label.setStyleSheet("font-family: monospace; font-size: 12px; color: #00ff64;")
        layout.addWidget(self.info_label)

        # --- Line 2: rolling average controls ---
        ravg_layout = QtWidgets.QHBoxLayout()
        ravg_layout.setSpacing(6)
        self.ravg_check = QtWidgets.QCheckBox("Rolling average")
        self.ravg_check.setChecked(False)
        self.ravg_spin = QtWidgets.QSpinBox()
        self.ravg_spin.setMinimum(2)
        self.ravg_spin.setMaximum(BUF_MAX)
        self.ravg_spin.setValue(100)
        self.ravg_spin.setFixedWidth(70)
        self.ravg_spin.setToolTip("Number of samples for rolling average")
        ravg_unit = QtWidgets.QLabel("samples")
        ravg_unit.setStyleSheet("font-family: monospace; font-size: 12px; color: #888888;")
        ravg_layout.addWidget(self.ravg_check)
        ravg_layout.addWidget(self.ravg_spin)
        ravg_layout.addWidget(ravg_unit)
        ravg_layout.addStretch()
        layout.addLayout(ravg_layout)

        # --- Line 3: centre / autoscale controls ---
        self.setCentreButton = QtWidgets.QPushButton("SET CENTRE")
        self.setCentreButton.setFixedWidth(100)
        self.setCentreButton.setEnabled(False)
        self.setCentreButton.setToolTip(
            "Move the target to the red cross.\n"
            "Click in the plot first to put the cross where you want the centre; "
            "enabled only once a point is marked.")
        self.autoButton = QtWidgets.QPushButton("AUTO")
        self.autoButton.setCheckable(True)
        self.autoButton.setFixedWidth(60)
        self.autoButton.setToolTip(
            "Autoscale both axes (kept square) to 1.5 x the largest excursion "
            "from the target centre. Released, the last scale is kept.")
        self.centreButton = QtWidgets.QPushButton("CENTRE 0")
        self.centreButton.setFixedWidth(80)
        self.centreButton.setToolTip(
            "Put the target back at the default centre (0 with --center, range/2 otherwise).\n"
            "The centre is saved as soon as it moves and restored at the next start.")
        ctrl_layout = QtWidgets.QHBoxLayout()
        ctrl_layout.setSpacing(6)
        for w in (self.setCentreButton, self.autoButton, self.centreButton):
            ctrl_layout.addWidget(w)
        ctrl_layout.addStretch()
        layout.addLayout(ctrl_layout)

        # --- Square plot ---
        gw = pg.GraphicsLayoutWidget()
        gw.setBackground('#1e1e1e')
        gw.setFixedSize(500, 500)
        layout.addWidget(gw, alignment=QtCore.Qt.AlignmentFlag.AlignLeft)

        self.plot = gw.addPlot()
        self.vb = self.plot.getViewBox()
        self.plot.setAspectLocked(True)
        self.plot.showGrid(x=True, y=True, alpha=0.2)
        self.plot.setDefaultPadding(0)
        self.plot.getAxis('bottom').setLabel('A')
        self.plot.getAxis('left').setLabel('B')

        # Concentric reference circles at 25/50/75/100% of half-range, centre
        # crosshair. Kept as attributes: both follow the centre and the range.
        dash = pg.mkPen(color=(80, 80, 80), width=1, style=QtCore.Qt.PenStyle.DashLine)
        self.circles = []
        for frac in [0.25, 0.5, 0.75, 1.0]:
            circle = QtWidgets.QGraphicsEllipseItem(0, 0, 0, 0)
            circle.setPen(dash)
            circle.setBrush(pg.mkBrush(None))
            self.plot.addItem(circle)
            self.circles.append((circle, frac))
        self.hLine = pg.InfiniteLine(pos=self.cy, angle=0, pen=dash)
        self.vLine = pg.InfiniteLine(pos=self.cx, angle=90, pen=dash)
        self.plot.addItem(self.hLine)
        self.plot.addItem(self.vLine)

        # The red cross marks the centre. A click moves it to the clicked point
        # (self.pending) without moving the target; SET CENTRE then makes the
        # target follow it. Either way the cross stays where it is, so it always
        # shows either the current centre or the one being proposed. Negative z
        # keeps it under the trail and the dots, which must stay readable.
        self.pending = None
        self.marker = pg.ScatterPlotItem(symbol='+', size=12,
                                         pen=pg.mkPen((255, 40, 40), width=1),
                                         brush=pg.mkBrush(255, 40, 40))
        self.marker.setZValue(-1)
        self.plot.addItem(self.marker)
        self.marker.setData([self.cx], [self.cy])

        # Trail
        self.trail_x = np.zeros(TRAIL_LEN)
        self.trail_y = np.zeros(TRAIL_LEN)
        self.trail_plot = self.plot.plot(self.trail_x, self.trail_y,
                                         pen=pg.mkPen(color=(0, 150, 255, 80), width=1))

        # Current position dot (raw)
        self.dot = self.plot.plot([0], [0],
                                  pen=None, symbol='o', symbolSize=12,
                                  symbolBrush=pg.mkBrush(0, 255, 100),
                                  symbolPen=pg.mkPen('w', width=1))

        # Rolling average dot (orange, hidden by default)
        self.dot_avg = self.plot.plot([0], [0],
                                      pen=None, symbol='o', symbolSize=10,
                                      symbolBrush=pg.mkBrush(255, 160, 0),
                                      symbolPen=pg.mkPen('w', width=1))
        self.dot_avg.setVisible(False)

        # Rolling average buffers (pre-allocated to max spinbox size)
        self.ravg_buf_a = np.zeros(BUF_MAX)
        self.ravg_buf_b = np.zeros(BUF_MAX)
        self.ravg_idx = 0
        self.ravg_count = 0
        self.idx = 0

        self._applyRange()
        saved = load_centres(self.centrePath).get(self.centreKey)
        if saved and len(saved) == 2:
            try:
                self._setCentre(float(saved[0]), float(saved[1]), save=False)
            except (TypeError, ValueError):
                pass

        self.setCentreButton.clicked.connect(self._commitPending)
        self.centreButton.clicked.connect(self._resetCentre)
        self.plot.scene().sigMouseClicked.connect(self._onClick)

    def _applyRange(self):
        """Square view of +/-half around the target centre, circles to match."""
        self.plot.setXRange(self.cx - self.half, self.cx + self.half, padding=0)
        self.plot.setYRange(self.cy - self.half, self.cy + self.half, padding=0)
        for circle, frac in self.circles:
            r = self.half * frac
            circle.setRect(self.cx - r, self.cy - r, r * 2, r * 2)

    def _setCentre(self, x, y, save=True):
        self.cx, self.cy = float(x), float(y)
        self.hLine.setPos(self.cy)
        self.vLine.setPos(self.cx)
        self.marker.setData([self.cx], [self.cy])
        self._applyRange()
        if save:
            save_centre(self.centrePath, self.centreKey, self.cx, self.cy)

    def _clearPending(self):
        """Forget the proposal, but leave the cross where it is."""
        self.pending = None
        self.setCentreButton.setEnabled(False)

    def _onClick(self, ev):
        """A left click only *marks* where the centre would go, with a red
        cross. Nothing moves until SET CENTRE is pressed, so an ordinary click
        on the plot can never shift the target by accident."""
        if ev.button() != QtCore.Qt.LeftButton:
            return
        if not self.vb.sceneBoundingRect().contains(ev.scenePos()):
            return
        pos = self.vb.mapSceneToView(ev.scenePos())
        self.pending = (pos.x(), pos.y())
        self.marker.setData([pos.x()], [pos.y()])
        self.setCentreButton.setEnabled(True)
        ev.accept()

    def _commitPending(self):
        """SET CENTRE: move the target to the red cross, which stays put."""
        if self.pending is None:
            return
        self._setCentre(*self.pending)
        self._clearPending()

    def _resetCentre(self):
        """CENTRE 0: target and cross both back to the default centre."""
        self._setCentre(self.origin, self.origin)
        self._clearPending()

    def _autoscale(self):
        """Fit the trail: half-range = AUTO_MARGIN x the largest excursion from
        the centre, taken over both axes so the view stays square."""
        n = min(self.idx, TRAIL_LEN)
        if n == 0:
            return
        x = self.trail_x[:n]
        y = self.trail_y[:n]
        ok = np.isfinite(x) & np.isfinite(y)
        if not ok.any():
            return
        d = max(np.max(np.abs(x[ok] - self.cx)), np.max(np.abs(y[ok] - self.cy)))
        half = max(AUTO_MARGIN * float(d), AUTO_MIN_HALF)
        if abs(half - self.half) > AUTO_HYST * self.half:
            self.half = half
            self._applyRange()

    def update(self):
        try:
            cmd = self.ttm.get_data()
            a = float(cmd[0, 0])
            b = float(cmd[1, 0])
        except Exception:
            return

        # --- Trail ---
        self.trail_x[self.idx % TRAIL_LEN] = a
        self.trail_y[self.idx % TRAIL_LEN] = b
        self.idx += 1
        i = self.idx % TRAIL_LEN
        self.trail_plot.setData(np.roll(self.trail_x, -i), np.roll(self.trail_y, -i))

        if self.autoButton.isChecked():
            self._autoscale()

        # --- Raw dot ---
        self.dot.setData([a], [b])

        # --- Rolling average ---
        use_ravg = self.ravg_check.isChecked()
        self.dot_avg.setVisible(use_ravg)

        n = self.ravg_spin.value()
        ri = self.ravg_idx % BUF_MAX
        self.ravg_buf_a[ri] = a
        self.ravg_buf_b[ri] = b
        self.ravg_idx += 1
        self.ravg_count = min(self.ravg_count + 1, BUF_MAX)

        centre = ""
        if (self.cx, self.cy) != (self.origin, self.origin):
            centre = f"    centre=({self.cx:+.4f}, {self.cy:+.4f})"
        if self.pending is not None:
            centre += f"    pick=({self.pending[0]:+.4f}, {self.pending[1]:+.4f})"

        if use_ravg:
            # Use only the last n samples, capped to what we have
            k = min(n, self.ravg_count)
            # indices of the last k samples in circular buffer
            end = self.ravg_idx          # one past last written (in unbounded space)
            start = end - k
            indices = np.arange(start, end) % BUF_MAX
            avg_a = float(np.mean(self.ravg_buf_a[indices]))
            avg_b = float(np.mean(self.ravg_buf_b[indices]))
            self.dot_avg.setData([avg_a], [avg_b])
            self.info_label.setText(
                f"A={a:+.4f}  B={b:+.4f}    "
                f"Ā={avg_a:+.4f}  B̄={avg_b:+.4f}  (n={k}){centre}"
            )
        else:
            self.info_label.setText(
                f"A={a:+.4f}  B={b:+.4f}    ΔA={a:+.4f}  ΔB={b:+.4f}{centre}"
            )


def main():
    args = parse_args()
    app = QtWidgets.QApplication(sys.argv)
    display = TtDisplay(args)

    timer = QtCore.QTimer()
    timer.timeout.connect(display.update)
    timer.start(int(1000 / args.fps))

    display.win.adjustSize()
    display.win.show()
    sys.exit(app.exec())


if __name__ == '__main__':
    main()
