#!/usr/bin/env python3
"""
Image Display - Real-time image display from shared memory

Usage: imgDisplay.py <shmName> [options]

Options:
  --fps        Update rate in Hz (default: 10)
  --cmap       Colormap (default: viridis)
  --help       Show this help message

Examples:
  imgDisplay.py /tmp/img.im.shm
  imgDisplay.py /tmp/img.im.shm --fps 20 --cmap inferno
"""

import dao
import numpy as np
import argparse
import sys
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtWidgets


def parse_args():
    parser = argparse.ArgumentParser(
        description='Image Display - Real-time image display from shared memory',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s /tmp/img.im.shm
  %(prog)s /tmp/img.im.shm --fps 20
  %(prog)s /tmp/img.im.shm --fps 20 --cmap inferno
        """
    )
    parser.add_argument('shmName',
                        help='Shared memory path (e.g. /tmp/img.im.shm)')
    parser.add_argument('--fps',  type=float, default=10.0,
                        help='Update rate in Hz (default: 10)')
    parser.add_argument('--cmap', default='viridis',
                        help='Colormap: viridis, inferno, plasma, grey (default: viridis)')
    return parser.parse_args()


COLORMAPS = {
    'viridis': [
        ( 68,   1,  84),
        ( 59,  82, 139),
        ( 33, 145, 140),
        ( 94, 201,  98),
        (253, 231,  37),
    ],
    'inferno': [
        (  0,   0,   4),
        (120,  28, 109),
        (238,  71,  41),
        (252, 177,  57),
        (252, 255, 164),
    ],
    'plasma': [
        ( 13,   8, 135),
        (126,   3, 168),
        (204,  71, 120),
        (248, 149,  64),
        (240, 249,  33),
    ],
    'grey': [
        (  0,   0,   0),
        (255, 255, 255),
    ],
}


def make_colormap(name):
    colors = COLORMAPS.get(name, COLORMAPS['viridis'])
    positions = np.linspace(0, 1, len(colors))
    return pg.ColorMap(positions, colors)


def main():
    args = parse_args()

    # Read shm and get image shape at startup
    shm = dao.shm(args.shmName)
    data0 = np.squeeze(shm.get_data()).astype(float)
    if data0.ndim != 2:
        print(f"ERROR: expected 2D image, got shape {data0.shape}")
        sys.exit(1)
    nx, ny = data0.shape
    print(f"Image size: {nx} x {ny}")

    app = QtWidgets.QApplication(sys.argv)
    pg.setConfigOptions(antialias=False)

    # Main window
    win = QtWidgets.QMainWindow()
    win.setWindowTitle(f"Image Display - {args.shmName} [{nx}x{ny}]")
    win.resize(800, 750)

    # Central widget
    central = QtWidgets.QWidget()
    win.setCentralWidget(central)
    layout = QtWidgets.QVBoxLayout(central)
    layout.setContentsMargins(4, 4, 4, 4)
    layout.setSpacing(4)

    # --- Controls bar ---
    ctrl_layout = QtWidgets.QHBoxLayout()

    autoscale_cb = QtWidgets.QCheckBox("Autoscale")
    autoscale_cb.setChecked(True)
    ctrl_layout.addWidget(autoscale_cb)

    ctrl_layout.addWidget(QtWidgets.QLabel("Min:"))
    min_spin = QtWidgets.QDoubleSpinBox()
    min_spin.setRange(-1e9, 1e9)
    min_spin.setValue(data0.min())
    min_spin.setDecimals(4)
    min_spin.setEnabled(False)
    ctrl_layout.addWidget(min_spin)

    ctrl_layout.addWidget(QtWidgets.QLabel("Max:"))
    max_spin = QtWidgets.QDoubleSpinBox()
    max_spin.setRange(-1e9, 1e9)
    max_spin.setValue(data0.max())
    max_spin.setDecimals(4)
    max_spin.setEnabled(False)
    ctrl_layout.addWidget(max_spin)

    ctrl_layout.addWidget(QtWidgets.QLabel("Colormap:"))
    cmap_combo = QtWidgets.QComboBox()
    for name in COLORMAPS:
        cmap_combo.addItem(name)
    cmap_combo.setCurrentText(args.cmap)
    ctrl_layout.addWidget(cmap_combo)

    freeze_btn = QtWidgets.QPushButton("Freeze")
    freeze_btn.setCheckable(True)
    ctrl_layout.addWidget(freeze_btn)

    ctrl_layout.addStretch()

    stats_label = QtWidgets.QLabel("")
    ctrl_layout.addWidget(stats_label)

    layout.addLayout(ctrl_layout)

    # --- Image view ---
    gw = pg.GraphicsLayoutWidget()
    gw.setBackground('k')
    layout.addWidget(gw)

    plot = gw.addPlot()
    plot.setAspectLocked(True)
    plot.showAxes(True)
    plot.vb.disableAutoRange()
    plot.vb.setRange(xRange=(0, nx), yRange=(0, ny), padding=0)

    img_item = pg.ImageItem()
    plot.addItem(img_item)

    cmap = make_colormap(args.cmap)
    img_item.setColorMap(cmap)

    colorbar = pg.ColorBarItem(
        values=(data0.min(), data0.max()),
        colorMap=cmap,
        label='Intensity',
        interactive=True,
    )
    colorbar.setImageItem(img_item, insert_in=plot)

    # Crosshair
    hline = pg.InfiniteLine(angle=0, movable=False,
                            pen=pg.mkPen('r', width=1,
                                         style=QtCore.Qt.PenStyle.DashLine))
    vline = pg.InfiniteLine(angle=90, movable=False,
                            pen=pg.mkPen('r', width=1,
                                         style=QtCore.Qt.PenStyle.DashLine))
    plot.addItem(hline)
    plot.addItem(vline)

    def mouse_moved(evt):
        pos = evt[0]
        if plot.sceneBoundingRect().contains(pos):
            mp = plot.vb.mapSceneToView(pos)
            vline.setPos(mp.x())
            hline.setPos(mp.y())

    proxy = pg.SignalProxy(plot.scene().sigMouseMoved, rateLimit=60,
                           slot=mouse_moved)

    # --- State ---
    state = {'frozen': False, 'cmap': args.cmap}

    def toggle_freeze(checked):
        state['frozen'] = checked
        freeze_btn.setText("Unfreeze" if checked else "Freeze")

    def toggle_autoscale(checked):
        min_spin.setEnabled(not checked)
        max_spin.setEnabled(not checked)

    def change_cmap(name):
        cm = make_colormap(name)
        img_item.setColorMap(cm)
        colorbar.setColorMap(cm)

    autoscale_cb.stateChanged.connect(toggle_autoscale)
    freeze_btn.toggled.connect(toggle_freeze)
    cmap_combo.currentTextChanged.connect(change_cmap)

    # --- Update loop ---
    def update():
        if state['frozen']:
            return
        try:
            data = np.squeeze(shm.get_data()).astype(float)
        except Exception:
            return
        if data.ndim != 2:
            return

        if autoscale_cb.isChecked():
            vmin, vmax = data.min(), data.max()
            min_spin.blockSignals(True)
            max_spin.blockSignals(True)
            min_spin.setValue(vmin)
            max_spin.setValue(vmax)
            min_spin.blockSignals(False)
            max_spin.blockSignals(False)
        else:
            vmin = min_spin.value()
            vmax = max_spin.value()

        img_item.setImage(data.T, levels=(vmin, vmax), autoLevels=False)
        colorbar.setLevels((vmin, vmax))

        stats_label.setText(
            f"min={data.min():.4f}  max={data.max():.4f}  "
            f"mean={data.mean():.4f}  std={data.std():.4f}"
        )

    timer = QtCore.QTimer()
    timer.timeout.connect(update)
    timer.start(int(1000 / args.fps))

    win.show()
    sys.exit(app.exec())


if __name__ == '__main__':
    main()