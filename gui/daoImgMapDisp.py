#!/usr/bin/env python3
"""
Image Display - Real-time image display from shared memory

Usage: daoImgMapDisp.py <shmName> [options]

Options:
  --pup   Pupil shm path  (default: /tmp/wfs1pup.im.shm)
  --mask  Mask shm path   (default: /tmp/wfs1Mask.im.shm)
  --fps   Update rate in Hz (default: 10)
  --cmap  Colormap: viridis|inferno|plasma|grey (default: grey)
  --size  Display size in pixels (default: 600)
  --att   Mask attenuation 0..1 (default: 0.6)
  --light Use light mode (default: dark)

Examples:
  daoImgMapDisp.py /tmp/cblue1.im.shm
  daoImgMapDisp.py /tmp/cblue1.im.shm --pup /tmp/wfs1pup.im.shm --mask /tmp/wfs1Mask.im.shm
  daoImgMapDisp.py /tmp/cblue1.im.shm --light
"""

import sys
import argparse
import numpy as np
import dao
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtWidgets

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

def mask_bbox(mask):
    rows = np.any(mask > 0.5, axis=1)
    cols = np.any(mask > 0.5, axis=0)
    x0, x1 = np.where(rows)[0][[0, -1]]
    y0, y1 = np.where(cols)[0][[0, -1]]
    return int(x0), int(x1)+1, int(y0), int(y1)+1


def parse_args():
    p = argparse.ArgumentParser(description='Real-time image display from shared memory')
    p.add_argument('shmName',                                 help='Shared memory path')
    p.add_argument('--pup',   default='/tmp/wfs1pup.im.shm',  help='Pupil shm path')
    p.add_argument('--mask',  default='/tmp/wfs1Mask.im.shm', help='Mask shm path')
    p.add_argument('--att',   type=float, default=0.6,        help='Mask attenuation 0..1')
    p.add_argument('--fps',   type=float, default=10.0,       help='Update rate in Hz')
    p.add_argument('--cmap',  default='grey', choices=COLORMAPS.keys())
    p.add_argument('--size',  type=int,   default=600,        help='Display size in pixels')
    p.add_argument('--light', action='store_true',            help='Light mode (default: dark)')
    return p.parse_args()


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color: #f0f0f0; color: #000000; }
            QPushButton {
                background-color: #e0e0e0; color: #000000;
                border: 1px solid #aaa; padding: 4px 8px; border-radius: 3px;
            }
            QPushButton:hover    { background-color: #d0d0d0; }
            QPushButton:pressed  { background-color: #bbb; }
            QPushButton:checked  { background-color: #4a90d9; color: #fff; border-color: #2a70b9; }
            QCheckBox, QLabel    { color: #000000; }
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
            QPushButton:hover    { background-color: #4a4a4a; }
            QPushButton:pressed  { background-color: #555; }
            QPushButton:checked  { background-color: #1a6a3a; border-color: #2a9a5a; }
            QCheckBox, QLabel    { color: #cccccc; }
            QCheckBox::indicator { width: 14px; height: 14px; }
            QCheckBox::indicator:unchecked { background-color: #aaaaaa; border: 1px solid #ccc; }
            QDoubleSpinBox, QComboBox {
                background-color: #3a3a3a; color: #cccccc; border: 1px solid #555;
            }
        """


def main():
    args = parse_args()

    # Configure pyqtgraph theme before app creation
    if args.light:
        pg.setConfigOption('background', '#f0f0f0')
        pg.setConfigOption('foreground', '#000000')
    else:
        pg.setConfigOption('background', '#1e1e1e')
        pg.setConfigOption('foreground', '#cccccc')

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
    mask_data = np.ones((nx, ny), dtype=float)
    try:
        mask_shm = dao.shm(args.mask)
        m0 = np.squeeze(mask_shm.get_data()).astype(float)
        if m0.shape != (nx, ny):
            print(f"WARNING: mask shape {m0.shape} != image shape {(nx,ny)}, ignoring")
            mask_shm = None
        else:
            mask_data = m0
            print(f"Mask:  {args.mask}")
    except Exception:
        print(f"Mask shm not found: {args.mask}")

    bbox = {'x0': 0, 'x1': nx, 'y0': 0, 'y1': ny}
    if mask_shm is not None:
        try:
            x0, x1, y0, y1 = mask_bbox(mask_data)
            bbox.update({'x0': x0, 'x1': x1, 'y0': y0, 'y1': y1})
        except Exception:
            pass

    app = QtWidgets.QApplication(sys.argv)

    win = QtWidgets.QMainWindow()
    win.setWindowTitle(f"ImgDisp  {args.shmName}  [{nx}x{ny}]")
    win.setStyleSheet(make_stylesheet(args.light))

    central = QtWidgets.QWidget()
    win.setCentralWidget(central)
    layout = QtWidgets.QVBoxLayout(central)
    layout.setContentsMargins(4, 4, 4, 4)
    layout.setSpacing(2)

    # --- Line 1: crosshair coords + image stats ---
    info_color = '#000000' if args.light else '#cccccc'
    info_label = QtWidgets.QLabel("x=--  y=--  val=--    min=--  max=--  mean=--  std=--")
    info_label.setStyleSheet(f"font-family: monospace; font-size: 11px; color: {info_color};")
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

    # --- Line 3: cmap + mask + att + pupil + freeze + crop ---
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

    crop_btn = QtWidgets.QPushButton("Crop")
    crop_btn.setCheckable(True)
    crop_btn.setEnabled(mask_shm is not None)
    line3.addWidget(crop_btn)

    line3.addStretch()
    layout.addLayout(line3)

    # --- Square image view ---
    gw_bg = '#f0f0f0' if args.light else '#1e1e1e'
    gw = pg.GraphicsLayoutWidget()
    gw.setBackground(gw_bg)
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

    def toggle_crop(checked):
        if checked and mask_shm is not None:
            try:
                m = np.squeeze(mask_shm.get_data()).astype(float)
                x0, x1, y0, y1 = mask_bbox(m)
                bbox.update({'x0': x0, 'x1': x1, 'y0': y0, 'y1': y1})
            except Exception:
                pass
            plot.vb.setRange(
                xRange=(bbox['x0'], bbox['x1']),
                yRange=(bbox['y0'], bbox['y1']),
                padding=0.02
            )
            crop_btn.setText("Full")
        else:
            plot.vb.setRange(xRange=(0, nx), yRange=(0, ny), padding=0)
            crop_btn.setText("Crop")

    autoscale_cb.stateChanged.connect(toggle_autoscale)
    freeze_btn.toggled.connect(toggle_freeze)
    cmap_combo.currentTextChanged.connect(change_cmap)
    pup_cb.stateChanged.connect(toggle_pup)
    crop_btn.toggled.connect(toggle_crop)

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