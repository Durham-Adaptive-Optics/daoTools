#!/usr/bin/env python3
"""
Shack-Hartmann Display - Real-time WFS image with centroid/reference overlays
and optional intensity map

Usage: daoShDisp.py [options]

Options:
  -i  WFS image shm        (default: /tmp/shIm.im.shm)
  -c  Centroids shm        (default: /tmp/shCentroids.im.shm)
  -r  References shm       (default: /tmp/shReferences.im.shm)
  -p  Pupil mask shm       (default: /tmp/shPupMask.im.shm)
  -w  Window table shm     (window centroider, rows of 9: centre x, y, width, height,
                            search x, y, ...; default: none)
  --search N  search range of the correlation centroider: a +-N px box around each
              reference (default: 0, none)
  --fps    Update rate in Hz (default: 10)
  --cmap   Colormap: grey (default), viridis, inferno, plasma, jet; the overlays take
           colours that stand out on it (the checkboxes in their colours are the key)
  --size   Display size in pixels (default: 600)
  --light  Light mode (default: dark)

Overlays: Ref, Centroids, Arrows; Sub-ap grid (the cells of the valid sub-apertures,
from the pupil mask and the image size: the search area of a centre-of-gravity
centroider); Search (with -w: each window, solid, and its search area, dashed; with
--search N: a +-N px box around each reference).

Examples:
  daoShDisp.py
  daoShDisp.py --search 5                    # correlation centroider, search +-5 px
  daoShDisp.py -w /tmp/shWinTable.im.shm     # window centroider
  daoShDisp.py -i /tmp/wfs1.im.shm -c /tmp/cent1.im.shm -r /tmp/ref1.im.shm -p /tmp/pup1.im.shm
  daoShDisp.py --light
"""

import os
import sys
import argparse
import time
import numpy as np
import dao
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtWidgets
from PyQt5.QtGui import QIcon

COLORMAPS = {
    'grey':    [(0,0,0),(255,255,255)],
    'viridis': [(68,1,84),(59,82,139),(33,145,140),(94,201,98),(253,231,37)],
    'inferno': [(0,0,4),(120,28,109),(238,71,41),(252,177,57),(252,255,164)],
    'plasma':  [(13,8,135),(126,3,168),(204,71,120),(248,149,64),(240,249,33)],
    'jet':     [(0,0,143),(0,0,255),(0,127,255),(0,255,255),(127,255,127),
                (255,255,0),(255,127,0),(255,0,0),(127,0,0)],
}

# the overlays' colours, per colormap: hues the colormap does not hold, bright enough on
# its low end (most of an SH image). One per overlay, in this order:
# centroids, sub-aperture grid, search (windows), references, arrows
PALETTES = {
    'grey':    ['#ff0000', '#00c8ff', '#ffcc00', '#00ff00', '#ffa500'],   # the original r / g / orange
    'viridis': ['#ffcc00', '#44ddff', '#ff6666', '#77ee77', '#ffffff'],
    'inferno': ['#00ffff', '#7fff00', '#ffffff', '#3399ff', '#c0c0ff'],
    'plasma':  ['#00ffff', '#7fff00', '#ffffff', '#3399ff', '#c0c0ff'],
    'jet':     ['#ffffff', '#ff00ff', '#ff8c00', '#ffa0d8', '#bbbbbb'],
}
CENT, GRID, SEARCH, REF, ARROW = range(5)


def overlay_colour(cmap, role):
    return PALETTES.get(cmap, PALETTES['grey'])[role]


def label_style(col):
    """a checkbox label in its overlay's colour (a light patch behind a dark one)"""
    return f'color: {col}; background-color: #cccccc;' if pg.mkColor(col).lightness() < 80 else f'color: {col};'

def make_colormap(name):
    colors = COLORMAPS.get(name, COLORMAPS['grey'])
    return pg.ColorMap(np.linspace(0, 1, len(colors)), colors)

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
            QPushButton:checked { background-color: #4a90d9; color: #fff; border-color: #2a70b9; }
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
            QPushButton:checked { background-color: #1a5a8a; border-color: #2a8aba; }
            QCheckBox, QLabel   { color: #cccccc; }
            QCheckBox::indicator { width: 14px; height: 14px; }
            QCheckBox::indicator:unchecked { background-color: #aaaaaa; border: 1px solid #ccc; }
            QDoubleSpinBox, QComboBox {
                background-color: #3a3a3a; color: #cccccc; border: 1px solid #555;
            }
        """


def parse_args():
    p = argparse.ArgumentParser(description='Shack-Hartmann WFS display')
    p.add_argument('-i', '--shm',  default='/tmp/shIm.im.shm',          help='WFS image shm')
    p.add_argument('-c', '--cent', default='/tmp/shCentroids.im.shm',   help='Centroids shm')
    p.add_argument('-r', '--ref',  default='/tmp/shReferences.im.shm',  help='References shm')
    p.add_argument('-p', '--pup',  default='/tmp/shPupMask.im.shm',     help='Pupil mask shm')
    p.add_argument('-w', '--win',  default=None,
                   help='window table shm (window centroider: centre x, y, width, height, search x, y, ...)')
    p.add_argument('--search', type=int, default=0,
                   help='search range [px] around each reference (correlation centroider; default 0: none)')
    p.add_argument('--fps',   type=float, default=10.0, help='Update rate in Hz (default: 10)')
    p.add_argument('--cmap',  default='grey', choices=COLORMAPS.keys())
    p.add_argument('--size',  type=int, default=600,    help='Display size in pixels (default: 600)')
    p.add_argument('--light', action='store_true',      help='Light mode (default: dark)')
    return p.parse_args()


def try_shm(path, label):
    try:
        s = dao.shm(path)
        print(f"{label}: {path}")
        return s
    except Exception:
        print(f"{label}: NOT FOUND ({path})")
        return None


def c_round(v):
    """roundf() of the C centroiders (half away from zero), on an array"""
    v = np.asarray(v, float)
    return np.where(v >= 0, np.floor(v + 0.5), -np.floor(-v + 0.5)).astype(int)


def rect_path(x0, y0, w, h):
    """one path through all the rectangles (NaN between them), for a PlotDataItem"""
    xs, ys = [], []
    for a, b, c, e in zip(x0, y0, w, h):
        xs += [a, a + c, a + c, a, a, np.nan]
        ys += [b, b, b + e, b + e, b, np.nan]
    return np.array(xs, float), np.array(ys, float)


def main():
    args = parse_args()

    if args.light:
        pg.setConfigOption('background', '#f0f0f0')
        pg.setConfigOption('foreground', '#000000')
    else:
        pg.setConfigOption('background', '#1e1e1e')
        pg.setConfigOption('foreground', '#cccccc')

    shm_im   = try_shm(args.shm,  "Image    ")
    shm_cent = try_shm(args.cent, "Centroids")
    shm_ref  = try_shm(args.ref,  "Reference")
    shm_pup  = try_shm(args.pup,  "Pupil    ")
    shm_win  = try_shm(args.win,  "Windows  ") if args.win else None

    if shm_im is None:
        print("ERROR: image shm required"); sys.exit(1)

    data0 = np.squeeze(shm_im.get_data()).astype(float)
    if data0.ndim != 2:
        print(f"ERROR: expected 2D image, got {data0.shape}"); sys.exit(1)
    nx, ny = data0.shape
    print(f"Image: {nx}x{ny}")

    nCent = 0
    if shm_cent is not None:
        try:
            nCent = shm_cent.get_data().shape[0] // 4
            print(f"nCent: {nCent}")
        except Exception:
            pass

    pup = None
    pup_shape = None
    if shm_pup is not None:
        try:
            pup = np.squeeze(shm_pup.get_data()).astype(float)
            pup_shape = pup.shape
            print(f"Pupil mask: {pup_shape}")
        except Exception:
            pass

    app = QtWidgets.QApplication(sys.argv)
    # dao logo in the window's title bar
    logo = os.path.join(os.getenv('DAOROOT', ''), 'data', 'daoLogo.png')
    if os.path.exists(logo):
        app.setWindowIcon(QIcon(logo))

    win = QtWidgets.QMainWindow()
    win.setWindowTitle(f"ShDisp  {args.shm}  [{nx}x{ny}]")
    win.setStyleSheet(make_stylesheet(args.light))

    central = QtWidgets.QWidget()
    win.setCentralWidget(central)
    layout = QtWidgets.QVBoxLayout(central)
    layout.setContentsMargins(4, 4, 4, 4)
    layout.setSpacing(2)

    # --- Line 1: stats + freq ---
    info_color = '#000000' if args.light else '#cccccc'
    info_label = QtWidgets.QLabel("x=--  y=--  val=--    max=--  mean=--  std=--    0.00 Hz")
    info_label.setStyleSheet(f"font-family: monospace; font-size: 11px; color: {info_color};")
    layout.addWidget(info_label)

    # --- Line 2: autoscale + min/max ---
    line2 = QtWidgets.QHBoxLayout()
    autoscale_cb = QtWidgets.QCheckBox("Autoscale")
    autoscale_cb.setChecked(True)
    line2.addWidget(autoscale_cb)
    line2.addWidget(QtWidgets.QLabel("Min:"))
    min_spin = QtWidgets.QDoubleSpinBox()
    min_spin.setRange(-1e9, 1e9); min_spin.setDecimals(1)
    min_spin.setValue(data0.min()); min_spin.setEnabled(False)
    line2.addWidget(min_spin)
    line2.addWidget(QtWidgets.QLabel("Max:"))
    max_spin = QtWidgets.QDoubleSpinBox()
    max_spin.setRange(-1e9, 1e9); max_spin.setDecimals(1)
    max_spin.setValue(data0.max()); max_spin.setEnabled(False)
    line2.addWidget(max_spin)
    line2.addStretch()
    layout.addLayout(line2)

    # --- Line 3: cmap + overlays + log + intmap + freeze ---
    line3 = QtWidgets.QHBoxLayout()
    line3.addWidget(QtWidgets.QLabel("Cmap:"))
    cmap_combo = QtWidgets.QComboBox()
    for name in COLORMAPS:
        cmap_combo.addItem(name)
    cmap_combo.setCurrentText(args.cmap)
    line3.addWidget(cmap_combo)

    ref_cb = QtWidgets.QCheckBox("Ref")
    ref_cb.setChecked(True)
    ref_cb.setEnabled(shm_ref is not None and nCent > 0)
    line3.addWidget(ref_cb)

    cent_cb = QtWidgets.QCheckBox("Centroids")
    cent_cb.setChecked(True)
    cent_cb.setEnabled(shm_cent is not None and nCent > 0)
    line3.addWidget(cent_cb)

    arrow_cb = QtWidgets.QCheckBox("Arrows")
    arrow_cb.setChecked(False)
    arrow_cb.setEnabled(shm_cent is not None and shm_ref is not None and nCent > 0)
    line3.addWidget(arrow_cb)

    grid_cb = QtWidgets.QCheckBox("Sub-ap grid")
    grid_cb.setChecked(False)
    grid_cb.setToolTip("the cells of the valid sub-apertures (pupil mask and image size)")
    line3.addWidget(grid_cb)

    search_cb = QtWidgets.QCheckBox("Search")
    search_cb.setChecked(shm_win is not None or args.search > 0)
    search_cb.setEnabled(shm_win is not None or (args.search > 0 and shm_ref is not None and nCent > 0))
    search_cb.setToolTip("-w: each window (solid) and its search area (dashed)\n"
                         "--search N: a +-N px box around each reference")
    line3.addWidget(search_cb)

    log_cb = QtWidgets.QCheckBox("Log")
    log_cb.setChecked(False)
    line3.addWidget(log_cb)

    intmap_btn = QtWidgets.QPushButton("Int Map")
    intmap_btn.setCheckable(True)
    intmap_btn.setChecked(False)
    intmap_btn.setEnabled(shm_cent is not None and pup is not None and nCent > 0)
    line3.addWidget(intmap_btn)

    freeze_btn = QtWidgets.QPushButton("Freeze")
    freeze_btn.setCheckable(True)
    line3.addWidget(freeze_btn)

    line3.addStretch()
    layout.addLayout(line3)

    # --- Panels: WFS image + intensity map side by side ---
    panels = QtWidgets.QWidget()
    panels_layout = QtWidgets.QHBoxLayout(panels)
    panels_layout.setContentsMargins(0, 0, 0, 0)
    panels_layout.setSpacing(4)
    layout.addWidget(panels)

    gw_bg = '#f0f0f0' if args.light else '#1e1e1e'

    # -- WFS image panel --
    gw = pg.GraphicsLayoutWidget()
    gw.setBackground(gw_bg)
    gw.setFixedSize(args.size, args.size)
    panels_layout.addWidget(gw)

    plot = gw.addPlot()
    plot.showAxes(True)
    plot.setDefaultPadding(0)
    plot.vb.disableAutoRange()
    plot.vb.setRange(xRange=(-0.5, nx - 0.5), yRange=(-0.5, ny - 0.5), padding=0)
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

    ref_item  = pg.ScatterPlotItem(pen=pg.mkPen(overlay_colour(args.cmap, REF), width=1),
                                   brush=pg.mkBrush(None), symbol='+', size=10)
    cent_item = pg.ScatterPlotItem(pen=pg.mkPen(overlay_colour(args.cmap, CENT), width=1),
                                   brush=pg.mkBrush(None), symbol='o', size=6)
    plot.addItem(ref_item)
    plot.addItem(cent_item)

    # sub-aperture grid: fixed, from the pupil mask and the image size
    grid_item = pg.PlotDataItem(pen=pg.mkPen(overlay_colour(args.cmap, GRID), width=1), connect='finite')
    grid_item.setVisible(False)
    plot.addItem(grid_item)
    if pup is not None and pup.ndim == 2:
        rows, cols = data0.shape
        my_, mx_ = pup.shape
        if rows % my_ == 0 and cols % mx_ == 0:
            csx, csy = cols // mx_, rows // my_
            r, c = np.nonzero(pup)
            grid_item.setData(*rect_path(c * csx - 0.5, r * csy - 0.5, [csx] * len(c), [csy] * len(r)))
            print(f"Sub-aperture grid: {len(r)} cells of {csx}x{csy} px")
        else:
            grid_cb.setEnabled(False)
            grid_cb.setToolTip(f"image {cols}x{rows} not a multiple of the pupil mask {mx_}x{my_}")
    else:
        grid_cb.setEnabled(False)
    grid_cb.stateChanged.connect(lambda v: grid_item.setVisible(bool(v)))

    # search areas: windows (solid) and their search (dashed), or +-N around the references
    win_item = pg.PlotDataItem(pen=pg.mkPen(overlay_colour(args.cmap, SEARCH), width=2), connect='finite')
    search_item = pg.PlotDataItem(pen=pg.mkPen(overlay_colour(args.cmap, SEARCH), width=1,
                                               style=QtCore.Qt.PenStyle.DashLine), connect='finite')
    plot.addItem(win_item)
    plot.addItem(search_item)
    for it in (win_item, search_item):
        it.setVisible(search_cb.isChecked())
    search_cb.stateChanged.connect(lambda v: [it.setVisible(bool(v)) for it in (win_item, search_item)])

    arrow_items = []

    def clear_arrows():
        for a in arrow_items:
            plot.removeItem(a)
        arrow_items.clear()

    hline = pg.InfiniteLine(angle=0,  movable=False,
                            pen=pg.mkPen('y', width=1, style=QtCore.Qt.PenStyle.DashLine))
    vline = pg.InfiniteLine(angle=90, movable=False,
                            pen=pg.mkPen('y', width=1, style=QtCore.Qt.PenStyle.DashLine))
    plot.addItem(hline); plot.addItem(vline)

    cursor_pos = {'x': 0, 'y': 0}

    def mouse_moved(evt):
        pos = evt[0]
        if plot.sceneBoundingRect().contains(pos):
            mp = plot.vb.mapSceneToView(pos)
            cursor_pos['x'] = int(round(mp.x()))
            cursor_pos['y'] = int(round(mp.y()))
            vline.setPos(mp.x()); hline.setPos(mp.y())

    proxy = pg.SignalProxy(plot.scene().sigMouseMoved, rateLimit=60, slot=mouse_moved)

    # -- Intensity map panel (hidden by default) --
    gw_int = pg.GraphicsLayoutWidget()
    gw_int.setBackground(gw_bg)
    gw_int.setFixedSize(args.size, args.size)
    gw_int.setVisible(False)
    panels_layout.addWidget(gw_int)

    int_map_data = np.zeros(pup_shape if pup_shape else (1, 1), dtype=float)
    mx, my = int_map_data.shape

    plot_int = gw_int.addPlot()
    plot_int.showAxes(True)
    plot_int.setDefaultPadding(0)
    plot_int.vb.disableAutoRange()
    plot_int.vb.setRange(xRange=(0, mx), yRange=(0, my), padding=0)
    plot_int.vb.setAspectLocked(True)
    plot_int.setTitle("Intensity Map", color=info_color, size='10pt')

    int_item = pg.ImageItem()
    plot_int.addItem(int_item)
    int_item.setColorMap(make_colormap('viridis'))

    int_colorbar = pg.ColorBarItem(
        values=(0, 1), colorMap=make_colormap('viridis'),
        label='Flux', interactive=True,
    )
    int_colorbar.setImageItem(int_item, insert_in=plot_int)

    # --- Signals ---
    state   = {'frozen': False}
    t_ref   = [time.time()]
    cnt_ref = [shm_im.get_counter() if hasattr(shm_im, 'get_counter') else 0]

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
        set_overlay_colours(name)

    def set_overlay_colours(name):
        """the overlays in the colormap's palette; the checkboxes in their colours (the key)"""
        ref_item.setPen(pg.mkPen(overlay_colour(name, REF), width=1))
        cent_item.setPen(pg.mkPen(overlay_colour(name, CENT), width=1))
        grid_item.setPen(pg.mkPen(overlay_colour(name, GRID), width=1))
        win_item.setPen(pg.mkPen(overlay_colour(name, SEARCH), width=2))
        search_item.setPen(pg.mkPen(overlay_colour(name, SEARCH), width=1, style=QtCore.Qt.PenStyle.DashLine))
        for a in arrow_items:
            a.setPen(pg.mkPen(overlay_colour(name, ARROW), width=1))
        for cb, role in ((ref_cb, REF), (cent_cb, CENT), (arrow_cb, ARROW), (grid_cb, GRID), (search_cb, SEARCH)):
            cb.setStyleSheet(label_style(overlay_colour(name, role)))

    def toggle_intmap(checked):
        gw_int.setVisible(checked)
        win.adjustSize()

    set_overlay_colours(args.cmap)
    autoscale_cb.stateChanged.connect(toggle_autoscale)
    freeze_btn.toggled.connect(toggle_freeze)
    cmap_combo.currentTextChanged.connect(change_cmap)
    ref_cb.stateChanged.connect(lambda v: ref_item.setVisible(bool(v)))
    cent_cb.stateChanged.connect(lambda v: cent_item.setVisible(bool(v)))
    arrow_cb.stateChanged.connect(lambda v: clear_arrows() if not v else None)
    intmap_btn.toggled.connect(toggle_intmap)

    # --- Update ---
    def update():
        if state['frozen']:
            return
        try:
            data = np.squeeze(shm_im.get_data()).astype(float)
        except Exception:
            return
        if data.ndim != 2:
            return

        disp = data.copy()
        if log_cb.isChecked():
            disp[disp <= 0] = 1e-12
            disp = np.log(disp)

        vmin = disp.min() if autoscale_cb.isChecked() else min_spin.value()
        vmax = disp.max() if autoscale_cb.isChecked() else max_spin.value()

        if autoscale_cb.isChecked():
            for spin, val in ((min_spin, vmin), (max_spin, vmax)):
                spin.blockSignals(True); spin.setValue(val); spin.blockSignals(False)

        img_item.setImage(disp.T, levels=(vmin, vmax), autoLevels=False)
        # Centre pixel i on integer coordinate i (default ImageItem centres it on
        # i+0.5). This matches daoComputeCentroidRelative / ShackHartmannWFS, whose
        # centroids and references are in the "pixel index = coordinate" frame, so
        # the centroid / reference overlays land on the spots instead of half a
        # pixel to the lower-left.
        h, w = disp.shape
        img_item.setRect(QtCore.QRectF(-0.5, -0.5, w, h))
        colorbar.setLevels((vmin, vmax))

        xref = yref = xcur = ycur = None
        if shm_ref is not None and shm_cent is not None and nCent > 0:
            try:
                cref = shm_ref.get_data()
                cent = shm_cent.get_data()
                xref = np.asarray(cref[:nCent,        0], dtype=float)
                yref = np.asarray(cref[nCent:2*nCent, 0], dtype=float)
                dx   = np.asarray(cent[:nCent,        0], dtype=float)
                dy   = np.asarray(cent[nCent:2*nCent, 0], dtype=float)
                xcur = xref + dx
                ycur = yref + dy
            except Exception:
                pass

        if xref is not None:
            if ref_cb.isChecked():
                ref_item.setData(x=xref, y=yref)
            if cent_cb.isChecked():
                cent_item.setData(x=xcur, y=ycur)
            if arrow_cb.isChecked():
                clear_arrows()
                for i in range(len(xref)):
                    line = pg.PlotDataItem(
                        x=[xref[i], xcur[i]], y=[yref[i], ycur[i]],
                        pen=pg.mkPen(overlay_colour(cmap_combo.currentText(), ARROW), width=1)
                    )
                    plot.addItem(line)
                    arrow_items.append(line)

        if search_cb.isChecked():
            try:
                if shm_win is not None:
                    t = np.asarray(shm_win.get_data(), float).reshape(-1, 9)
                    w, h = c_round(t[:, 2]), c_round(t[:, 3])
                    x0, y0 = c_round(t[:, 0]) - w // 2, c_round(t[:, 1]) - h // 2
                    rx, ry = c_round(t[:, 4]), c_round(t[:, 5])
                    win_item.setData(*rect_path(x0 - 0.5, y0 - 0.5, w, h))
                    search_item.setData(*rect_path(x0 - rx - 0.5, y0 - ry - 0.5, w + 2 * rx, h + 2 * ry))
                elif args.search > 0 and xref is not None:
                    n = args.search
                    size = [2 * n + 1] * len(xref)
                    search_item.setData(*rect_path(c_round(xref) - n - 0.5, c_round(yref) - n - 0.5, size, size))
            except Exception:
                pass

        if intmap_btn.isChecked() and shm_cent is not None and pup is not None and nCent > 0:
            try:
                cent = shm_cent.get_data()
                flux = np.asarray(cent[2*nCent:3*nCent, 0], dtype=float)
                int_map = np.zeros(pup_shape, dtype=float)
                int_map[pup == 1] = flux
                if log_cb.isChecked():
                    int_map[int_map <= 0] = 1e-12
                    int_map = np.log(int_map)
                ivmin, ivmax = int_map.min(), int_map.max()
                int_item.setImage(int_map.T, levels=(ivmin, ivmax), autoLevels=False)
                int_colorbar.setLevels((ivmin, ivmax))
            except Exception:
                pass

        try:
            cnt2 = shm_im.get_counter()
            t2   = time.time()
            dt   = t2 - t_ref[0]
            freq = (cnt2 - cnt_ref[0]) / dt if dt > 0 else 0.0
            t_ref[0] = t2; cnt_ref[0] = cnt2
            freq_str = f"{freq:.1f} Hz"
        except Exception:
            freq_str = "--"

        cx, cy = cursor_pos['x'], cursor_pos['y']
        val_str = f"{data[cx, cy]:.1f}" if 0 <= cx < nx and 0 <= cy < ny else "--"

        info_label.setText(
            f"x={cx:4d}  y={cy:4d}  val={val_str}    "
            f"max={data.max():.1f}  mean={data.mean():.2f}  std={data.std():.2f}    {freq_str}"
        )

    timer = QtCore.QTimer()
    timer.timeout.connect(update)
    timer.start(int(1000 / args.fps))

    win.adjustSize()
    win.show()
    sys.exit(app.exec())


if __name__ == '__main__':
    main()