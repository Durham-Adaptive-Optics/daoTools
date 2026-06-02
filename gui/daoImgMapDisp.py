#!/usr/bin/env python3
"""
Image Display - Real-time image display from shared memory

Usage: daoImgDisp.py <shmName> [options]

Options:
  --pup   Pupil shm path  (default: /tmp/wfs1pup.im.shm)
  --mask  Mask shm path   (default: /tmp/wfs1Mask.im.shm)
  --fps   Update rate in Hz (default: 10)
  --cmap  Colormap: viridis|inferno|plasma|grey (default: grey)
  --size  Display size in pixels (default: 600)
  --att   Mask attenuation 0..1 (default: 0.6)

Examples:
  daoImgDisp.py /tmp/cblue1.im.shm
  daoImgDisp.py /tmp/cblue1.im.shm --pup /tmp/wfs1pup.im.shm --mask /tmp/wfs1Mask.im.shm
"""

import sys
import argparse
import numpy as np
import dao
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtWidgets

pg.setConfigOption('background', '#1e1e1e')
pg.setConfigOption('foreground', '#cccccc')

COLORMAPS = {
    'viridis': [(68,1,84),(59,82,139),(33,145,140),(94,201,98),(253,231,37)],
    'inferno': [(0,0,4),(120,28,109),(238,71,41),(252,177,57),(252,255,164)],
    'plasma':  [(13,8,135),(126,3,168),(204,71,120),(248,149,64),(240,249,33)],
    'grey':    [(0,0,0),(255,255,255)],
}

def make_colormap(name):
    colors = COLORMAPS.get(name, COLORMAPS['grey'])
    return pg.ColorMap(np.linspace(0, 1, len(colors)), colors)

def apply_mask(data, mask, attenuation):
    result = data.copy()
    outside = mask < 0.5
    result[outside] = data.min() + (data[outside] - data.min()) * (1.0 - attenuation)
    return result


def parse_args():
    p = argparse.ArgumentParser(description='Real-time image display from shared memory')
    p.add_argument('shmName',                                 help='Shared memory path')
    p.add_argument('--pup',  default='/tmp/wfs1pup.im.shm',  help='Pupil shm path')
    p.add_argument('--mask', default='/tmp/wfs1Mask.im.shm', help='Mask shm path')
    p.add_argument('--att',  type=float, default=0.6,        help='Mask attenuation 0..1')
    p.add_argument('--fps',  type=float, default=10.0,       help='Update rate in Hz')
    p.add_argument('--cmap', default='grey', choices=COLORMAPS.keys())
    p.add_argument('--size', type=int,   default=600,        help='Display size in pixels')
    return p.parse_args()


def main():
    args = parse_args()

    shm = dao.shm(args.shmName)
    data0 = np.squeeze(shm.get_data()).astype(float)
    if data0.ndim != 2:
        print(f"ERROR: expected 2D image, got shape {data0.shape}")
        sys.exit(1)
    nx, ny = data0.shape
    print(f"Image: {nx}x{ny}  shm: {args.shmName}")

    pup_shm = None
    try:
        pup_shm = dao.shm(args.pup)
        print(f"Pupil: {args.pup}")
    except Exception:
        print(f"Pupil shm not found: {args.pup}")

    mask_shm = None
    try:
        mask_shm = dao.shm(args.mask)
        m0 = np.squeeze(mask_shm.get_data()).astype(float)
        if m0.shape != (nx, ny):
            print(f"WARNING: mask shape {m0.shape} != image shape {(nx,ny)}, ignoring")
            mask_shm = None
        else:
            print(f"Mask:  {args.mask}")
    except Exception:
        print(f"Mask shm not found: {args.mask}")

    app = QtWidgets.QApplication(sys.argv)

    win = QtWidgets.QMainWindow()
    win.setWindowTitle(f"ImgDisp  {args.shmName}  [{nx}x{ny}]")
    win.setStyleSheet("""
        QMainWindow, QWidget { background-color: #1e1e1e; color: #cccccc; }
        QPushButton {
            background-color: #3a3a3a; color: #cccccc;
            border: 1px solid #555; padding: 4px 8px; border-radius: 3px;
        }
        QPushButton:hover   { background-color: #4a4a4a; }
        QPushButton:pressed { background-color: #555; }
        QCheckBox, QLabel   { color: #cccccc; }
        QDoubleSpinBox, QComboBox {
            background-color: #3a3a3a; color: #cccccc; border: 1px solid #555;
        }
    """)

    central = QtWidgets.QWidget()
    win.setCentralWidget(central)
    layout = QtWidgets.QVBoxLayout(central)
    layout.setContentsMargins(4, 4, 4, 4)
    layout.setSpacing(2)

    # --- Line 1: crosshair coords + image stats ---
    info_label = QtWidgets.QLabel("x=--  y=--  val=--    min=--  max=--  mean=--  std=--")
    info_label.setStyleSheet("font-family: monospace; font-size: 11px; color: #cccccc;")
    layout.addWidget(info_label)

    # --- Line 2: autoscale + min/max ---
    line2 = QtWidgets.QHBoxLayout()

    autoscale_cb = QtWidgets.QCheckBox("Autoscale")
    autoscale_cb.setChecked(True)
    line2.addWidget(autoscale_cb)

    line2.addWidget(QtWidgets.QLabel("Min:"))
    min_spin = QtWidgets.QDoubleSpinBox()
    min_spin.setRange(-1e9, 1e9); min_spin.setDecimals(4)
    min_spin.setValue(data0.min()); min_spin.setEnabled(False)
    line2.addWidget(min_spin)

    line2.addWidget(QtWidgets.QLabel("Max:"))
    max_spin = QtWidgets.QDoubleSpinBox()
    max_spin.setRange(-1e9, 1e9); max_spin.setDecimals(4)
    max_spin.setValue(data0.max()); max_spin.setEnabled(False)
    line2.addWidget(max_spin)

    line2.addStretch()
    layout.addLayout(line2)

    # --- Line 3: cmap + mask + pupil + freeze ---
    line3 = QtWidgets.QHBoxLayout()

    line3.addWidget(QtWidgets.QLabel("Cmap:"))
    cmap_combo = QtWidgets.QComboBox()
    for name in COLORMAPS:
        cmap_combo.addItem(name)
    cmap_combo.setCurrentText(args.cmap)
    line3.addWidget(cmap_combo)

    mask_cb = QtWidgets.QCheckBox("Mask")
    mask_cb.setChecked(False)
    mask_cb.setEnabled(mask_shm is not None)
    line3.addWidget(mask_cb)

    line3.addWidget(QtWidgets.QLabel("Att:"))
    att_spin = QtWidgets.QDoubleSpinBox()
    att_spin.setRange(0.0, 1.0); att_spin.setSingleStep(0.05)
    att_spin.setDecimals(2); att_spin.setValue(args.att)
    att_spin.setEnabled(mask_shm is not None)
    line3.addWidget(att_spin)

    pup_cb = QtWidgets.QCheckBox("Pupil")
    pup_cb.setChecked(False)
    pup_cb.setEnabled(pup_shm is not None)
    line3.addWidget(pup_cb)

    freeze_btn = QtWidgets.QPushButton("Freeze")
    freeze_btn.setCheckable(True)
    line3.addWidget(freeze_btn)

    line3.addStretch()
    layout.addLayout(line3)

    # --- Image view (square, fills remaining space) ---
    gw = pg.GraphicsLayoutWidget()
    gw.setBackground('#1e1e1e')
    gw.setFixedSize(args.size, args.size)
    layout.addWidget(gw, alignment=QtCore.Qt.AlignmentFlag.AlignLeft)

    plot = gw.addPlot()
    plot.showAxes(True)
    plot.setDefaultPadding(0)
    plot.vb.disableAutoRange()
    plot.vb.setRange(xRange=(0, nx), yRange=(0, ny), padding=0)
    plot.vb.setAspectLocked(True)

    img_item = pg.ImageItem()
    plot.addItem(img_item)

    cmap = make_colormap(args.cmap)
    img_item.setColorMap(cmap)

    colorbar = pg.ColorBarItem(
        values=(data0.min(), data0.max()),
        colorMap=cmap, label='Intensity', interactive=True,
    )
    colorbar.setImageItem(img_item, insert_in=plot)

    pup_contour = pg.IsocurveItem(level=0.5, pen=pg.mkPen('r', width=1))
    pup_contour.setVisible(False)
    plot.addItem(pup_contour)

    hline = pg.InfiniteLine(angle=0,  movable=False,
                            pen=pg.mkPen('r', width=1, style=QtCore.Qt.PenStyle.DashLine))
    vline = pg.InfiniteLine(angle=90, movable=False,
                            pen=pg.mkPen('r', width=1, style=QtCore.Qt.PenStyle.DashLine))
    plot.addItem(hline)
    plot.addItem(vline)

    cursor_pos = {'x': 0, 'y': 0}

    def mouse_moved(evt):
        pos = evt[0]
        if plot.sceneBoundingRect().contains(pos):
            mp = plot.vb.mapSceneToView(pos)
            cursor_pos['x'] = int(mp.x())
            cursor_pos['y'] = int(mp.y())
            vline.setPos(mp.x())
            hline.setPos(mp.y())

    proxy = pg.SignalProxy(plot.scene().sigMouseMoved, rateLimit=60, slot=mouse_moved)

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
                pup_contour.setData(np.squeeze(pup_shm.get_data()).astype(float).T)
            except Exception:
                pass

    autoscale_cb.stateChanged.connect(toggle_autoscale)
    freeze_btn.toggled.connect(toggle_freeze)
    cmap_combo.currentTextChanged.connect(change_cmap)
    pup_cb.stateChanged.connect(toggle_pup)

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
                data = apply_mask(data, mask, att_spin.value())
            except Exception:
                pass

        vmin = data.min() if autoscale_cb.isChecked() else min_spin.value()
        vmax = data.max() if autoscale_cb.isChecked() else max_spin.value()

        if autoscale_cb.isChecked():
            for spin, val in ((min_spin, vmin), (max_spin, vmax)):
                spin.blockSignals(True); spin.setValue(val); spin.blockSignals(False)

        img_item.setImage(data.T, levels=(vmin, vmax), autoLevels=False)
        colorbar.setLevels((vmin, vmax))

        if pup_cb.isChecked() and pup_shm is not None:
            try:
                pup_contour.setData(np.squeeze(pup_shm.get_data()).astype(float).T)
            except Exception:
                pass

        cx, cy = cursor_pos['x'], cursor_pos['y']
        val_str = f"{data[cx, cy]:.4f}" if 0 <= cx < nx and 0 <= cy < ny else "--"

        info_label.setText(
            f"x={cx:4d}  y={cy:4d}  val={val_str}    "
            f"min={vmin:.4f}  max={vmax:.4f}  "
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