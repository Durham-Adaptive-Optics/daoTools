#!/usr/bin/env python3
"""
Fourier Magnitude Display - Real-time display from two shared memories

Usage:
  daoFFTDisp.py <shmRe> <shmIm> [options]

Examples:
  daoFFTDisp.py /tmp/imgRe.im.shm /tmp/imgIm.im.shm
  daoFFTDisp.py /tmp/imgRe.im.shm /tmp/imgIm.im.shm --fps 20 --cmap inferno
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


def strip_border(data):
    d = data.copy()
    d[0, :] = 0
    d[-1, :] = 0
    d[:, 0] = 0
    d[:, -1] = 0
    return d


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color: #f0f0f0; color: #000000; }
            QPushButton { background-color: #e0e0e0; color: #000000;
                          border: 1px solid #aaa; padding: 4px 8px; border-radius: 3px; }
            QPushButton:hover { background-color: #d0d0d0; }
            QPushButton:pressed { background-color: #bbb; }
            QCheckBox, QLabel { color: #000000; }
            QDoubleSpinBox, QComboBox {
                background-color: #ffffff; color: #000000; border: 1px solid #aaa;
            }
        """
    return """
        QMainWindow, QWidget { background-color: #1e1e1e; color: #cccccc; }
        QPushButton { background-color: #3a3a3a; color: #cccccc;
                      border: 1px solid #555; padding: 4px 8px; border-radius: 3px; }
        QPushButton:hover { background-color: #4a4a4a; }
        QPushButton:pressed { background-color: #555; }
        QCheckBox, QLabel { color: #cccccc; }
        QDoubleSpinBox, QComboBox {
            background-color: #3a3a3a; color: #cccccc; border: 1px solid #555;
        }
    """


def parse_args():
    p = argparse.ArgumentParser(description="Real-time Fourier magnitude display from shmRe/shmIm")
    p.add_argument("shmRe", help="Shared memory path for real part")
    p.add_argument("shmIm", help="Shared memory path for imaginary part")
    p.add_argument("--fps", type=float, default=10.0, help="Update rate in Hz")
    p.add_argument("--cmap", default="grey", choices=COLORMAPS.keys())
    p.add_argument("--size", type=int, default=600, help="Display size in pixels")
    p.add_argument("--light", action="store_true", help="Light mode")
    p.add_argument("--log", action="store_true", help="Display log10(1 + magnitude)")
    return p.parse_args()


def get_magnitude(shm_re, shm_im, use_log=False):
    img_re = np.squeeze(shm_re.get_data()).astype(float)
    img_im = np.squeeze(shm_im.get_data()).astype(float)

    if img_re.shape != img_im.shape:
        raise ValueError(f"Shape mismatch: Re {img_re.shape}, Im {img_im.shape}")

    img = np.sqrt(img_re * img_re + img_im * img_im)

    if use_log:
        img = np.log10(1.0 + img)

    return img


def main():
    args = parse_args()

    pg.setConfigOption("background", "#f0f0f0" if args.light else "#1e1e1e")
    pg.setConfigOption("foreground", "#000000" if args.light else "#cccccc")

    shm_re = dao.shm(args.shmRe)
    shm_im = dao.shm(args.shmIm)

    try:
        data0 = get_magnitude(shm_re, shm_im, args.log)
    except Exception as e:
        print(f"ERROR: cannot read input shared memories: {e}")
        sys.exit(1)

    if data0.ndim != 2:
        print(f"ERROR: expected 2D images, got shape {data0.shape}")
        sys.exit(1)

    nx, ny = data0.shape
    img_shape = [nx, ny]

    print(f"Fourier magnitude image: {nx}x{ny}")
    print(f"  Re: {args.shmRe}")
    print(f"  Im: {args.shmIm}")

    app = QtWidgets.QApplication(sys.argv)

    win = QtWidgets.QMainWindow()
    win.setWindowTitle(f"FFT Magnitude [{nx}x{ny}]")
    win.setStyleSheet(make_stylesheet(args.light))

    central = QtWidgets.QWidget()
    win.setCentralWidget(central)

    layout = QtWidgets.QVBoxLayout(central)
    layout.setContentsMargins(4, 4, 4, 4)
    layout.setSpacing(2)

    info_color = "#000000" if args.light else "#cccccc"
    info_label = QtWidgets.QLabel("x=--  y=--  val=--    min=--  max=--  mean=--  std=--")
    info_label.setStyleSheet(
        f"font-family: monospace; font-size: 11px; color: {info_color};"
    )
    layout.addWidget(info_label)

    line2 = QtWidgets.QHBoxLayout()

    autoscale_cb = QtWidgets.QCheckBox("Autoscale")
    autoscale_cb.setChecked(True)
    line2.addWidget(autoscale_cb)

    line2.addWidget(QtWidgets.QLabel("Min:"))
    min_spin = QtWidgets.QDoubleSpinBox()
    min_spin.setRange(-1e30, 1e30)
    min_spin.setDecimals(6)
    min_spin.setValue(float(data0.min()))
    min_spin.setEnabled(False)
    line2.addWidget(min_spin)

    line2.addWidget(QtWidgets.QLabel("Max:"))
    max_spin = QtWidgets.QDoubleSpinBox()
    max_spin.setRange(-1e30, 1e30)
    max_spin.setDecimals(6)
    max_spin.setValue(float(data0.max()))
    max_spin.setEnabled(False)
    line2.addWidget(max_spin)

    line2.addStretch()
    layout.addLayout(line2)

    line3 = QtWidgets.QHBoxLayout()

    line3.addWidget(QtWidgets.QLabel("Cmap:"))
    cmap_combo = QtWidgets.QComboBox()
    for name in COLORMAPS:
        cmap_combo.addItem(name)
    cmap_combo.setCurrentText(args.cmap)
    line3.addWidget(cmap_combo)

    border_cb = QtWidgets.QCheckBox("Strip border")
    border_cb.setChecked(False)
    line3.addWidget(border_cb)

    log_cb = QtWidgets.QCheckBox("log10(1+mag)")
    log_cb.setChecked(args.log)
    line3.addWidget(log_cb)

    freeze_btn = QtWidgets.QPushButton("Freeze")
    freeze_btn.setCheckable(True)
    line3.addWidget(freeze_btn)

    line3.addStretch()
    layout.addLayout(line3)

    gw = pg.GraphicsLayoutWidget()
    gw.setBackground("#f0f0f0" if args.light else "#1e1e1e")
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
        values=(float(data0.min()), float(data0.max())),
        colorMap=cmap,
        label="Magnitude",
        interactive=True,
    )
    colorbar.setImageItem(img_item, insert_in=plot)

    hline = pg.InfiniteLine(
        angle=0,
        movable=False,
        pen=pg.mkPen("r", width=1, style=QtCore.Qt.PenStyle.DashLine),
    )
    vline = pg.InfiniteLine(
        angle=90,
        movable=False,
        pen=pg.mkPen("r", width=1, style=QtCore.Qt.PenStyle.DashLine),
    )
    plot.addItem(hline)
    plot.addItem(vline)

    cursor_pos = {"x": 0, "y": 0}
    state = {"frozen": False}

    def mouse_moved(evt):
        pos = evt[0]
        if plot.sceneBoundingRect().contains(pos):
            mp = plot.vb.mapSceneToView(pos)
            cursor_pos["x"] = int(mp.x())
            cursor_pos["y"] = int(mp.y())
            vline.setPos(mp.x())
            hline.setPos(mp.y())

    pg.SignalProxy(plot.scene().sigMouseMoved, rateLimit=60, slot=mouse_moved)

    def toggle_autoscale(checked):
        min_spin.setEnabled(not checked)
        max_spin.setEnabled(not checked)

    def toggle_freeze(checked):
        state["frozen"] = checked
        freeze_btn.setText("Unfreeze" if checked else "Freeze")

    def change_cmap(name):
        cm = make_colormap(name)
        img_item.setColorMap(cm)
        colorbar.setColorMap(cm)

    autoscale_cb.stateChanged.connect(toggle_autoscale)
    freeze_btn.toggled.connect(toggle_freeze)
    cmap_combo.currentTextChanged.connect(change_cmap)

    def update():
        if state["frozen"]:
            return

        try:
            data = get_magnitude(shm_re, shm_im, log_cb.isChecked())
        except Exception as e:
            info_label.setText(f"ERROR: {e}")
            return

        if data.ndim != 2:
            return

        if border_cb.isChecked():
            data = strip_border(data)

        if data.shape != (img_shape[0], img_shape[1]):
            img_shape[0], img_shape[1] = data.shape
            plot.vb.setRange(
                xRange=(0, img_shape[0]),
                yRange=(0, img_shape[1]),
                padding=0,
            )

        vmin = float(data.min()) if autoscale_cb.isChecked() else min_spin.value()
        vmax = float(data.max()) if autoscale_cb.isChecked() else max_spin.value()

        if vmax <= vmin:
            vmax = vmin + 1.0

        if autoscale_cb.isChecked():
            min_spin.blockSignals(True)
            max_spin.blockSignals(True)
            min_spin.setValue(vmin)
            max_spin.setValue(vmax)
            min_spin.blockSignals(False)
            max_spin.blockSignals(False)

        img_item.setImage(data.T, levels=(vmin, vmax), autoLevels=False)
        colorbar.setLevels((vmin, vmax))

        cx, cy = cursor_pos["x"], cursor_pos["y"]
        h, w = data.shape

        val_str = f"{data[cx, cy]:.6g}" if 0 <= cx < h and 0 <= cy < w else "--"

        info_label.setText(
            f"x={cx:4d}  y={cy:4d}  val={val_str}    "
            f"min={vmin:.6g}  max={vmax:.6g}  "
            f"mean={data.mean():.6g}  std={data.std():.6g}"
        )

    timer = QtCore.QTimer()
    timer.timeout.connect(update)
    timer.start(max(1, int(1000 / args.fps)))

    update()
    win.adjustSize()
    win.show()

    sys.exit(app.exec())


if __name__ == "__main__":
    main()