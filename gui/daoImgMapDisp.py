#!/usr/bin/env python3
"""
Image Display - Real-time image display from shared memory

Usage: daoImgDisp.py -s <shmName> [options]

Options:
  -s, --shm      Shared memory path
  -m, --mask     Mask shared memory path (optional)
  --fps          Update rate in Hz (default: 10)
  --cmap         Colormap (default: viridis)
  --attenuation  Attenuation outside mask (default: 0.6)
  --size         Display size in pixels (default: 600)
  --pup          Pupil shm path (default: /tmp/wfs1pup.im.shm)

Examples:
  daoImgDisp.py -s /tmp/cblue1.im.shm
  daoImgDisp.py -s /tmp/cblue1.im.shm -m /tmp/wfs1Mask.im.shm
  daoImgDisp.py -s /tmp/cblue1.im.shm -m /tmp/wfs1Mask.im.shm --attenuation 0.3
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
    parser = argparse.ArgumentParser(
        description='Image Display - Real-time image display from shared memory',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s -s /tmp/cblue1.im.shm
  %(prog)s -s /tmp/cblue1.im.shm -m /tmp/wfs1Mask.im.shm
  %(prog)s -s /tmp/cblue1.im.shm -m /tmp/wfs1Mask.im.shm --attenuation 0.3 --size 700
        """
    )
    parser.add_argument('shmName', nargs='?')
    parser.add_argument('-s', '--shm',  dest='shmFlag')
    parser.add_argument('-m', '--mask', dest='maskName', default=None,
                        help='Mask shm path (e.g. /tmp/wfs1Mask.im.shm)')
    parser.add_argument('--pup',  default='/tmp/wfs1pup.im.shm',
                        help='Pupil shm path (default: /tmp/wfs1pup.im.shm)')
    parser.add_argument('--fps',         type=float, default=10.0)
    parser.add_argument('--cmap',        default='viridis')
    parser.add_argument('--attenuation', type=float, default=0.6)
    parser.add_argument('--size',        type=int,   default=600,
                        help='Display size in pixels (default: 600)')
    args = parser.parse_args()
    if args.shmFlag:
        args.shmName = args.shmFlag
    if not args.shmName:
        parser.error("shmName required")
    if not 0.0 <= args.attenuation <= 1.0:
        parser.error("--attenuation must be between 0.0 and 1.0")
    return args


COLORMAPS = {
    'viridis': [(68,1,84),(59,82,139),(33,145,140),(94,201,98),(253,231,37)],
    'inferno': [(0,0,4),(120,28,109),(238,71,41),(252,177,57),(252,255,164)],
    'plasma':  [(13,8,135),(126,3,168),(204,71,120),(248,149,64),(240,249,33)],
    'grey':    [(0,0,0),(255,255,255)],
}


def make_colormap(name):
    colors = COLORMAPS.get(name, COLORMAPS['viridis'])
    return pg.ColorMap(np.linspace(0, 1, len(colors)), colors)


def apply_mask(data, mask, attenuation):
    result = data.copy()
    outside = mask < 0.5
    result[outside] = data.min() + (data[outside] - data.min()) * (1.0 - attenuation)
    return result


def main():
    args = parse_args()

    # Read shm at startup
    shm = dao.shm(args.shmName)
    data0 = np.squeeze(shm.get_data()).astype(float)
    if data0.ndim != 2:
        print(f"ERROR: expected 2D image, got shape {data0.shape}")
        sys.exit(1)
    nx, ny = data0.shape
    print(f"Image size: {nx} x {ny}")

    # Mask shm
    mask_shm = None
    mask0 = np.ones((nx, ny), dtype=float)
    if args.maskName:
        mask_shm = dao.shm(args.maskName)
        mask0 = np.squeeze(mask_shm.get_data()).astype(float)
        if mask0.shape != (nx, ny):
            print(f"ERROR: mask shape {mask0.shape} != image shape {(nx, ny)}")
            sys.exit(1)
        print(f"Mask loaded: {args.maskName}")

    # Pupil shm
    pup_shm = None
    try:
        pup_shm = dao.shm(args.pup)
        pup0 = np.squeeze(pup_shm.get_data()).astype(float)
        print(f"Pupil loaded: {args.pup}")
    except Exception:
        print(f"Pupil shm not found: {args.pup}")

    app = QtWidgets.QApplication(sys.argv)
    pg.setConfigOptions(antialias=False)

    win = QtWidgets.QMainWindow()
    win.setWindowTitle(f"Image Display - {args.shmName} [{nx}x{ny}]")
    win.setStyleSheet("""
        QMainWindow, QWidget {
            background-color: #1e1e1e;
            color: #cccccc;
        }
        QPushButton {
            background-color: #3a3a3a;
            color: #cccccc;
            border: 1px solid #555;
            padding: 4px 8px;
            border-radius: 3px;
        }
        QPushButton:hover   { background-color: #4a4a4a; }
        QPushButton:pressed { background-color: #555; }
        QCheckBox, QLabel   { color: #cccccc; }
        QDoubleSpinBox {
            background-color: #3a3a3a;
            color: #cccccc;
            border: 1px solid #555;
        }
        QComboBox {
            background-color: #3a3a3a;
            color: #cccccc;
            border: 1px solid #555;
        }
    """)

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

    mask_cb = QtWidgets.QCheckBox("Mask")
    mask_cb.setChecked(False)
    mask_cb.setEnabled(mask_shm is not None)
    ctrl_layout.addWidget(mask_cb)

    ctrl_layout.addWidget(QtWidgets.QLabel("Att:"))
    att_spin = QtWidgets.QDoubleSpinBox()
    att_spin.setRange(0.0, 1.0)
    att_spin.setValue(args.attenuation)
    att_spin.setSingleStep(0.05)
    att_spin.setDecimals(2)
    att_spin.setEnabled(mask_shm is not None)
    ctrl_layout.addWidget(att_spin)

    pup_cb = QtWidgets.QCheckBox("Pupil")
    pup_cb.setChecked(False)
    pup_cb.setEnabled(pup_shm is not None)
    ctrl_layout.addWidget(pup_cb)

    freeze_btn = QtWidgets.QPushButton("Freeze")
    freeze_btn.setCheckable(True)
    ctrl_layout.addWidget(freeze_btn)

    ctrl_layout.addStretch()
    stats_label = QtWidgets.QLabel("")
    ctrl_layout.addWidget(stats_label)

    layout.addLayout(ctrl_layout)

    # --- Square image view ---
    gw = pg.GraphicsLayoutWidget()
    gw.setBackground('#1e1e1e')
    gw.setFixedSize(args.size, args.size)  # fixed square — no reshaping
    layout.addWidget(gw)

    plot = gw.addPlot()
    plot.showAxes(True)
    plot.setDefaultPadding(0)
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

    # Pupil contour overlay
    pup_contour = pg.IsocurveItem(level=0.5, pen=pg.mkPen('r', width=1))
    pup_contour.setVisible(False)
    plot.addItem(pup_contour)

    # Crosshair
    hline = pg.InfiniteLine(angle=0,  movable=False,
                            pen=pg.mkPen('y', width=1,
                            style=QtCore.Qt.PenStyle.DashLine))
    vline = pg.InfiniteLine(angle=90, movable=False,
                            pen=pg.mkPen('y', width=1,
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

    # --- Signals ---
    state = {'frozen': False}

    def toggle_autoscale(checked):
        min_spin.setEnabled(not checked)
        max_spin.setEnabled(not checked)

    def toggle_freeze(checked):
        state['frozen'] = checked
        freeze_btn.setText("Unfreeze" if checked else "Freeze")

    def change_cmap(name):
        cm = make_colormap(name)
        img_item.setColorMap(cm)
        colorbar.setColorMap(cm)

    def toggle_pup(checked):
        pup_contour.setVisible(checked)
        if checked and pup_shm is not None:
            try:
                pup = np.squeeze(pup_shm.get_data()).astype(float)
                pup_contour.setData(pup.T)
            except Exception:
                pass

    autoscale_cb.stateChanged.connect(toggle_autoscale)
    freeze_btn.toggled.connect(toggle_freeze)
    cmap_combo.currentTextChanged.connect(change_cmap)
    pup_cb.stateChanged.connect(toggle_pup)

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

        if mask_cb.isChecked() and mask_shm is not None:
            try:
                mask = np.squeeze(mask_shm.get_data()).astype(float)
            except Exception:
                mask = mask0
            data = apply_mask(data, mask, att_spin.value())

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

        # Update pupil contour if visible
        if pup_cb.isChecked() and pup_shm is not None:
            try:
                pup = np.squeeze(pup_shm.get_data()).astype(float)
                pup_contour.setData(pup.T)
            except Exception:
                pass

        stats_label.setText(
            f"min={data.min():.4f}  max={data.max():.4f}  "
            f"mean={data.mean():.4f}  std={data.std():.4f}"
        )

    timer = QtCore.QTimer()
    timer.timeout.connect(update)
    timer.start(int(1000 / args.fps))

    win.adjustSize()
    win.show()
    sys.exit(app.exec())


if __name__ == '__main__':
    main()