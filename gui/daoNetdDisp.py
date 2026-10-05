#!/usr/bin/env python3
"""
daoNetdDisp: the network SHMs, through this machine's daoShmNetd (daoBase daoNet).

    daoNetdDisp.py [--light] [--refresh 1.0]

  Service       start it (domain, peers, options) or stop it; its log
  Machines      the other services it sees (address, SHMs, last heard, how found)
  Network SHMs  every SHM of every machine: replicate one here (Replicate, Keep: even
                unused), Release it, or View it (replicated, then daoImDisp.py);
                double-click: View
  Replicas      the SHMs replicated here: link, frames, skipped, rate, age
  Served        this machine's SHMs other machines read, and their readers

The service is daoShmNetd (daoBase, network SHMs); it keeps running when this window
closes. $DAO_NET_CONTROL: its control port (default 7709).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import threading

from PyQt5.QtCore import QObject, Qt, QTimer, pyqtSignal
from PyQt5.QtGui import QColor
from PyQt5.QtWidgets import (QAbstractItemView, QApplication, QCheckBox, QDialog, QFormLayout,
                             QGroupBox, QHBoxLayout, QHeaderView, QLabel, QLineEdit, QMainWindow,
                             QMessageBox, QPlainTextEdit, QPushButton, QSpinBox, QSplitter,
                             QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

try:
    import daoNet
except ImportError:
    daoNet = None

OK_COLOR = "#55dd55"
BAD_COLOR = "#ff6060"
WAIT_COLOR = "#e0b040"


def make_stylesheet(light):
    if light:
        return """
            QMainWindow, QWidget { background-color:#f0f0f0; color:#000; }
            QLabel, QCheckBox { color:#000; }
            QPushButton { background:#e0e0e0; color:#000; border:1px solid #aaa;
                          padding:3px 8px; border-radius:3px; }
            QPushButton:hover { background:#d0d0d0; }
            QPushButton:disabled { color:#888; background:#ececec; }
            QSpinBox, QLineEdit { background:#fff; color:#000; border:1px solid #aaa; }
            QTableWidget { background:#fff; color:#000; gridline-color:#ccc; }
            QHeaderView::section { background:#e0e0e0; color:#000; border:1px solid #ccc; padding:2px; }
            QGroupBox { border:1px solid #aaa; border-radius:4px; margin-top:14px; padding-top:6px; color:#000; }
            QGroupBox::title { subcontrol-origin:margin; left:8px; color:#555; }
        """
    return """
        QMainWindow, QWidget { background-color:#1e1e1e; color:#ccc; }
        QLabel, QCheckBox { color:#ccc; }
        QPushButton { background:#3a3a3a; color:#ccc; border:1px solid #555;
                      padding:3px 8px; border-radius:3px; }
        QPushButton:hover { background:#4a4a4a; }
        QPushButton:disabled { color:#777; background:#2a2a2a; border-color:#3a3a3a; }
        QSpinBox, QLineEdit { background:#3a3a3a; color:#ccc; border:1px solid #555; }
        QTableWidget { background:#252525; color:#ccc; gridline-color:#3a3a3a;
                       selection-background-color:#2f5d8a; }
        QHeaderView::section { background:#333; color:#ccc; border:1px solid #444; padding:2px; }
        QGroupBox { border:1px solid #555; border-radius:4px; margin-top:14px; padding-top:6px; color:#ccc; }
        QGroupBox::title { subcontrol-origin:margin; left:8px; color:#aaa; }
    """


class Background(QObject):
    """Runs a slow call (start, replicate: up to seconds) off the GUI thread; its
    result, or the exception it raised, comes back to `done` on the GUI thread."""
    finished = pyqtSignal(object, object, object)       # done, result, error

    def __init__(self):
        super().__init__()
        self.finished.connect(lambda done, result, error: done(result, error))

    def run(self, fn, done):
        def work():
            try:
                self.finished.emit(done, fn(), None)
            except Exception as e:                       # shown to the user by `done`
                self.finished.emit(done, None, e)
        threading.Thread(target=work, daemon=True).start()


def make_table(headers, widths=()):
    t = QTableWidget(0, len(headers))
    t.setHorizontalHeaderLabels(headers)
    t.verticalHeader().setVisible(False)
    t.setEditTriggers(QAbstractItemView.NoEditTriggers)
    t.setSelectionBehavior(QAbstractItemView.SelectRows)
    t.setSelectionMode(QAbstractItemView.SingleSelection)
    t.horizontalHeader().setSectionResizeMode(QHeaderView.Interactive)   # not ResizeToContents:
    t.horizontalHeader().setStretchLastSection(True)                     # refreshed every second
    for j, width in enumerate(widths):
        t.setColumnWidth(j, width)
    return t


def fill_table(table, rows, key=None, colors=None):
    """Replace the rows, keeping the selected row (by key(row)) selected."""
    selected = None
    sel = table.selectionModel().selectedRows()
    if key and sel and sel[0].row() < len(getattr(table, 'rows', [])):
        selected = key(table.rows[sel[0].row()])
    table.setUpdatesEnabled(False)
    table.setRowCount(len(rows))
    for i, row in enumerate(rows):
        for j, value in enumerate(row['cells']):
            item = table.item(i, j)
            if item is None:
                item = QTableWidgetItem()
                table.setItem(i, j, item)
            item.setText(str(value))
            color = (colors or {}).get((i, j))
            item.setForeground(QColor(color) if color else table.palette().text().color())
    table.rows = rows
    if selected is not None:
        for i, row in enumerate(rows):
            if key(row) == selected:
                table.selectRow(i)
                break
    else:
        table.clearSelection()
    table.setUpdatesEnabled(True)


def viewer_command(path, light):
    """daoImDisp.py on `path`: next to this script (a source tree), or on the PATH."""
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'daoImDisp.py')
    cmd = [sys.executable, here] if os.path.isfile(here) else [shutil.which('daoImDisp.py') or 'daoImDisp.py']
    return cmd + [path] + (['--light'] if light else [])


class Main(QMainWindow):
    def __init__(self, light, refresh_s):
        super().__init__()
        self.light = light
        self.bg = Background()
        self.busy = False                                # a start / replicate in progress
        self.setWindowTitle("dao network SHMs")
        self.resize(1000, 800)

        central = QWidget()
        layout = QVBoxLayout(central)
        layout.addWidget(self._service_box())
        split = QSplitter(Qt.Vertical)
        split.addWidget(self._machines_box())
        split.addWidget(self._shms_box())
        split.addWidget(self._replicas_box())
        split.addWidget(self._served_box())
        split.setSizes([120, 320, 200, 120])
        layout.addWidget(split, 1)
        self.message = QLabel("")
        layout.addWidget(self.message)
        self.setCentralWidget(central)

        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(int(refresh_s * 1000))
        self.refresh()

    # ------------------------------------------------------------------ layout
    def _service_box(self):
        box = QGroupBox("Service (daoShmNetd)")
        v = QVBoxLayout(box)
        self.state = QLabel("")
        self.state.setTextInteractionFlags(Qt.TextSelectableByMouse)
        v.addWidget(self.state)

        form = QFormLayout()
        self.domainEdit = QLineEdit(os.environ.get('DAO_NET_DOMAIN', 'dao'))
        self.domainEdit.setToolTip("only services of the same domain see each other: one per instrument")
        self.peersEdit = QLineEdit("")
        self.peersEdit.setPlaceholderText("192.168.1.205, rtc2:7710   (reached directly, without multicast)")
        self.allowEdit = QLineEdit("")
        self.allowEdit.setPlaceholderText("192.168.1.   (accept only these addresses; empty: all)")
        form.addRow("domain", self.domainEdit)
        form.addRow("peers", self.peersEdit)
        form.addRow("allow", self.allowEdit)
        v.addLayout(form)

        opts = QHBoxLayout()
        self.noMulticastCheck = QCheckBox("no multicast")
        self.noMulticastCheck.setToolTip("listed peers only, no discovery beacons")
        self.readOnlyCheck = QCheckBox("read-only")
        self.readOnlyCheck.setToolTip("refuse writes coming from other machines")
        self.spinCheck = QCheckBox("spin")
        self.spinCheck.setToolTip("low latency: the streams poll instead of sleeping (a core each)")
        self.verboseCheck = QCheckBox("verbose")
        self.idleSpin = QSpinBox()
        self.idleSpin.setRange(0, 100000)
        self.idleSpin.setValue(30)
        self.idleSpin.setToolTip("drop a replica no process maps for so long (0: never)")
        for w in (self.noMulticastCheck, self.readOnlyCheck, self.spinCheck, self.verboseCheck):
            opts.addWidget(w)
        opts.addWidget(QLabel("idle (s)"))
        opts.addWidget(self.idleSpin)
        opts.addStretch()
        self.startButton = QPushButton("Start")
        self.startButton.clicked.connect(self.start_service)
        self.stopButton = QPushButton("Stop")
        self.stopButton.clicked.connect(self.stop_service)
        self.logButton = QPushButton("Log")
        self.logButton.clicked.connect(self.show_log)
        for b in (self.startButton, self.stopButton, self.logButton):
            opts.addWidget(b)
        v.addLayout(opts)
        self.serviceOptions = [self.domainEdit, self.peersEdit, self.allowEdit, self.noMulticastCheck,
                               self.readOnlyCheck, self.spinCheck, self.verboseCheck, self.idleSpin]
        return box

    def _machines_box(self):
        box = QGroupBox("Machines")
        v = QVBoxLayout(box)
        self.peersTable = make_table(["Machine", "Address", "SHMs", "Last heard (s)", "Found by"], (160, 200, 70, 120))
        v.addWidget(self.peersTable)
        return box

    def _shms_box(self):
        box = QGroupBox("Network SHMs")
        v = QVBoxLayout(box)
        top = QHBoxLayout()
        self.filterEdit = QLineEdit("")
        self.filterEdit.setPlaceholderText("filter: machine or SHM name")
        self.filterEdit.textChanged.connect(self.refresh)
        top.addWidget(self.filterEdit, 1)
        self.wantButton = QPushButton("Replicate")
        self.wantButton.setToolTip("replicate it here now (dropped once nothing uses it)")
        self.keepButton = QPushButton("Keep")
        self.keepButton.setToolTip("replicate it here, and keep it even when nothing uses it")
        self.releaseButton = QPushButton("Release")
        self.releaseButton.setToolTip("stop replicating it here (the local replica is removed)")
        self.viewButton = QPushButton("View")
        self.viewButton.setToolTip("replicate it here and open it in daoImDisp.py (double-click)")
        self.wantButton.clicked.connect(lambda: self.replicate(keep=False))
        self.keepButton.clicked.connect(lambda: self.replicate(keep=True))
        self.releaseButton.clicked.connect(self.release_selected)
        self.viewButton.clicked.connect(self.view_selected)
        for b in (self.wantButton, self.keepButton, self.releaseButton, self.viewButton):
            top.addWidget(b)
        v.addLayout(top)
        self.shmsTable = make_table(["Machine", "SHM", "Shape", "Type", "State"], (160, 260, 110, 90))
        self.shmsTable.doubleClicked.connect(lambda _: self.view_selected())
        self.shmsTable.itemSelectionChanged.connect(self.update_buttons)
        v.addWidget(self.shmsTable)
        return box

    def _replicas_box(self):
        box = QGroupBox("Replicas on this machine")
        v = QVBoxLayout(box)
        self.replicasTable = make_table(["SHM", "Link", "Frames", "Skipped", "Rate (Hz)", "Age (us)",
                                         "Kept", "Path"], (240, 90, 90, 80, 90, 90, 60))
        v.addWidget(self.replicasTable)
        return box

    def _served_box(self):
        box = QGroupBox("Served: this machine's SHMs read by other machines")
        v = QVBoxLayout(box)
        self.sourcesTable = make_table(["SHM", "Readers", "Counter"], (260, 90))
        v.addWidget(self.sourcesTable)
        return box

    # ------------------------------------------------------------------ refresh
    def client(self):
        return daoNet.Client(timeout=1.0)                # the refresh never waits long

    def refresh(self):
        try:
            c = self.client()
            info = c.ping()
            st = c.status()
            shms = c.ls()
        except daoNet.ServiceError:
            self.running = False
            self.state.setText(f'<span style="color:{BAD_COLOR}">&#9679;</span> not running on this machine '
                               f'(control port {daoNet.control_port()})')
            for t in (self.peersTable, self.shmsTable, self.replicasTable, self.sourcesTable):
                fill_table(t, [])
            self.update_buttons()
            return
        self.running = True
        self.state.setText(f'<span style="color:{OK_COLOR}">&#9679;</span> running: <b>{info["node"]}</b>, '
                           f'domain <b>{info["domain"]}</b>, version {info["version"]}, '
                           f'data port {st.get("port", "?")}, SHMs in {st.get("dir", "?")}')

        fill_table(self.peersTable,
                   [{'key': p['node'], 'cells': [p['node'], f"{p['address']}:{p['port']}", p['shms'],
                                                 f"{p['seen_ms_ago'] / 1000:.1f}", p['via']]}
                    for p in st['peers']],
                   key=lambda r: r['key'])

        flt = self.filterEdit.text().strip().lower()
        rows, colors = [], {}
        for s in shms:
            if flt and flt not in s['node'].lower() and flt not in s['name'].lower():
                continue
            i = len(rows)
            rows.append({'key': f"{s['node']}:{s['name']}", 'node': s['node'], 'name': s['name'],
                         'state': s['state'], 'cells': [s['node'], s['name'], s['shape'], s['type'], s['state']]})
            if s['state'].startswith('replica'):
                colors[(i, 4)] = OK_COLOR
            elif s['state'].startswith('local'):
                colors[(i, 4)] = "#888888"
        fill_table(self.shmsTable, rows, key=lambda r: r['key'], colors=colors)

        rows, colors = [], {}
        for i, r in enumerate(st['replicas']):
            rows.append({'key': r['shm'], 'cells': [r['shm'], r['link'], r['frames'], r['dropped'],
                                                    f"{r['rate_hz']:.1f}", r['age_us'],
                                                    "yes" if r['mode'] == 'keep' else "", r['path']]})
            colors[(i, 1)] = {'up': OK_COLOR, 'down': BAD_COLOR}.get(r['link'], WAIT_COLOR)
        fill_table(self.replicasTable, rows, key=lambda r: r['key'], colors=colors)

        fill_table(self.sourcesTable,
                   [{'key': s['shm'], 'cells': [s['shm'], s['readers'], s['counter']]} for s in st['sources']],
                   key=lambda r: r['key'])
        self.update_buttons()

    def selected_shm(self):
        sel = self.shmsTable.selectionModel().selectedRows()
        rows = getattr(self.shmsTable, 'rows', [])
        if not sel or sel[0].row() >= len(rows):
            return None
        return rows[sel[0].row()]

    def update_buttons(self):
        running = getattr(self, 'running', False)
        s = self.selected_shm() if running else None
        remote = s is not None and not s['state'].startswith('local')
        idle = not self.busy
        self.startButton.setEnabled(not running and idle)
        self.stopButton.setEnabled(running and idle)
        for w in self.serviceOptions:
            w.setEnabled(not running)
        self.wantButton.setEnabled(remote and idle)
        self.keepButton.setEnabled(remote and idle)
        self.releaseButton.setEnabled(remote and s['state'].startswith('replica') and idle)
        self.viewButton.setEnabled(s is not None and idle)

    # ------------------------------------------------------------------ actions
    def set_busy(self, text):
        self.busy = bool(text)
        self.message.setText(text)
        self.update_buttons()

    def failed(self, what, error):
        self.set_busy("")
        QMessageBox.warning(self, "dao network SHMs", f"{what}:\n{error}")

    def start_args(self):
        args = ['--domain', self.domainEdit.text().strip() or 'dao']
        for peer in self.peersEdit.text().replace(';', ',').split(','):
            if peer.strip():
                args += ['--peer', peer.strip()]
        for prefix in self.allowEdit.text().replace(';', ',').split(','):
            if prefix.strip():
                args += ['--allow', prefix.strip()]
        if self.noMulticastCheck.isChecked():
            args.append('--no-multicast')
        if self.readOnlyCheck.isChecked():
            args.append('--read-only')
        if self.spinCheck.isChecked():
            args.append('--spin')
        if self.verboseCheck.isChecked():
            args.append('-v')
        args += ['--idle', str(self.idleSpin.value())]
        if 'DAO_NET_CONTROL' in os.environ:              # the port this window watches
            args += ['--control-port', str(daoNet.control_port())]
        return args

    def start_service(self):
        args = self.start_args()
        self.set_busy("starting daoShmNetd " + " ".join(args) + " ...")

        def done(_, error):
            if error:
                self.failed("Cannot start daoShmNetd", error)
                return
            self.set_busy("")
            self.refresh()
        self.bg.run(lambda: daoNet.start(args), done)

    def stop_service(self):
        if QMessageBox.question(self, "dao network SHMs",
                                "Stop daoShmNetd? Every replica on this machine stops, and other "
                                "machines lose the SHMs of this one.") != QMessageBox.Yes:
            return
        try:
            self.client().stop()
        except daoNet.ServiceError as e:
            self.failed("Cannot stop daoShmNetd", e)
        QTimer.singleShot(300, self.refresh)

    def show_log(self):
        path = os.path.join(tempfile.gettempdir(), f'daoShmNetd-{daoNet.control_port()}.log')
        try:
            with open(path, errors='replace') as f:
                text = "".join(f.readlines()[-500:])
        except OSError:
            text = f"(no log: {path} - written when the service is started with daoShmNet.py or this window)"
        dlg = QDialog(self)
        dlg.setWindowTitle(path)
        dlg.resize(900, 500)
        box = QPlainTextEdit(text)
        box.setReadOnly(True)
        box.setLineWrapMode(QPlainTextEdit.NoWrap)
        QVBoxLayout(dlg).addWidget(box)
        box.moveCursor(box.textCursor().End)
        dlg.show()

    def replicate(self, keep, then=None):
        s = self.selected_shm()
        if s is None:
            return
        self.set_busy(f"replicating {s['key']} ...")

        def done(path, error):
            if error:
                self.failed(f"Cannot replicate {s['key']}", error)
                return
            self.set_busy("")
            self.message.setText(f"{s['key']} -> {path}")
            self.refresh()
            if then:
                then(path)
        self.bg.run(lambda: self.client_slow().want(s['key'], keep=keep), done)

    def client_slow(self):
        return daoNet.Client()                           # a first frame may take seconds

    def release_selected(self):
        s = self.selected_shm()
        if s is None:
            return
        try:
            self.client().release(s['key'])
        except daoNet.ServiceError as e:
            self.failed(f"Cannot release {s['key']}", e)
        self.refresh()

    def view_selected(self):
        s = self.selected_shm()
        if s is None or self.busy:
            return

        def view(path):
            try:
                subprocess.Popen(viewer_command(path, self.light), start_new_session=True)
            except OSError as e:
                self.failed("Cannot start daoImDisp.py", e)
        self.replicate(keep=False, then=view)


def main():
    parser = argparse.ArgumentParser(description="dao network SHMs: the daoShmNetd service of this machine",
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("--light", action="store_true", help="light mode (default: dark)")
    parser.add_argument("--refresh", type=float, default=1.0, help="refresh period, s (default: 1)")
    args = parser.parse_args()
    if daoNet is None:
        sys.exit("daoNetdDisp: no daoNet module - needs a daoBase with network SHMs (daoShmNetd)")
    app = QApplication(sys.argv)
    app.setStyleSheet(make_stylesheet(args.light))
    win = Main(args.light, max(args.refresh, 0.1))
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
