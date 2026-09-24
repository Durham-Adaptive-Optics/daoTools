#!/usr/bin/env python3
"""
DAQ Control - control panel for a daoDAQ server.

Connects to a running daoDAQ (see daoTools/apps/daoDAQ/README.md) through
daoDAQClient, the same library daoDAQCli.py uses, and gives full control
over it:

  - launch / shut down a local daoDAQ process
  - pick, edit, validate and save a YAML session configuration
  - apply it (upload + Init + Enable -> Ready), or re-apply it after an Error
  - START / STOP an acquisition, release the SHMs, recover from Error
  - follow the session directory being written (FITS files, size, time)
  - Extract tab: pick sessions/sources and a time range, and extract that
    telemetry to one HDF5 / FITS / NumPy file (daoDAQExtractor)

The server state is polled, so sessions finished on their own (samples
reached) or driven by another client (daoDAQCli.py) are shown live.

Usage: daoDAQCtrl.py [options]

Options:
  --host HOST        daoDAQ host (default: 127.0.0.1)
  --port PORT        daoDAQ port (default: 62000)
  --config FILE      configuration to load in the editor at start-up
  --config-dir DIR   directory listed in the configuration drop-down
                     (default: $DAOROOT/config/daq)
  --light            light mode (default: dark)

Examples:
  daoDAQCtrl.py
  daoDAQCtrl.py --config ~/daq/wfs_10k.yaml
  daoDAQCtrl.py --host rtc01 --port 62000
"""

import argparse
import glob
import logging
import os
import re
import signal
import socket
import subprocess
import sys
import time

import yaml
from PyQt5.uic import loadUiType
from PyQt5.QtCore import QDateTime, QThread, Qt, QTimer, pyqtSignal
from PyQt5.QtGui import QColor, QFont, QPalette, QTextCursor
from PyQt5.QtWidgets import (QApplication, QFileDialog, QHeaderView, QListWidgetItem,
                             QMessageBox, QStyleFactory, QTableWidgetItem, QTreeWidgetItem)

import daoDAQExtractor as X
from daoDAQClient import DAQClient, DAQState

_UI_DIR = os.getenv("DAOROOT", ".") + "/data/"
Ui_MainWindow, QMainWindow = loadUiType(os.path.join(_UI_DIR, "daoDAQCtrl.ui"))

POLL_MS = 500               # state poll period while connected
POLL_DISCONNECTED_MS = 2000  # state poll period while nobody answers
POLL_TIMEOUT_S = 0.3        # a state query must answer quickly
PROBE_TIMEOUT_S = 0.05      # TCP check of the port while disconnected
CMD_TIMEOUT_S = 5.0         # transitions open/close SHMs and join threads
SHM_REFRESH_MS = 2000       # rescan of the available SHMs

DEFAULT_ROOT_STORAGE = "${DAOROOT}/telemetry"   # where daoDAQ writes, for New configurations
SHM_DIR = "/tmp"            # where SHMs live (same convention as daoShmViewer)
DEFAULT_BUFFER_LIMIT = 1000  # given to every SHM added from the list

# Parameters daoDAQ reads for each smem source (anything else is ignored
# silently by the server, so it is flagged here).
SOURCE_KEYS = {
    "uri": str,
    "samples": int,
    "file_rollover": int,
    "buffer_limit": int,
    "daq_affinity": int,
    "sink_affinity": int,
    "eager_start": bool,
    "metadata_only": bool,
    "format": str,
}

# (text, background) of the state banner
STATE_STYLE = {
    None:                   ("NOT CONNECTED - is daoDAQ running?", "#3a3a3a"),
    DAQState.Unconfigured:  ("OFF - no configuration applied", "#555555"),
    DAQState.Configured:    ("STANDBY - configuration parsed", "#3a5f8a"),
    DAQState.Ready:         ("READY - SHMs attached, press START", "#2a6f9a"),
    DAQState.Acquiring:     ("ACQUIRING", "#1e7a3c"),
    DAQState.Error:         ("ERROR - fix the configuration, then Apply or Recover", "#a02828"),
}

# activity-log wording of each state, with the next step where there is one
STATE_LOG = {
    None:                   "not connected",
    DAQState.Unconfigured:  "OFF (no configuration) - press 'Apply to daoDAQ'",
    DAQState.Configured:    "STANDBY (configuration parsed)",
    DAQState.Ready:         "READY - press START",
    DAQState.Acquiring:     "ACQUIRING",
    DAQState.Error:         "ERROR",
}

SOURCE_COLUMNS = ["source", "samples", "rollover", "buffer", "affinity", "eager", "status"]


def make_stylesheet(light):
    base = """
        QPushButton { border:1px solid %(bd)s; padding:4px 10px; border-radius:3px;
                      background:%(btn)s; color:%(fg)s; }
        QPushButton:hover { background:%(btnh)s; border-color:#4a9eff; }
        QPushButton:disabled { color:%(dis)s; background:%(bg)s; border-color:%(bgb)s; }
        QPushButton#startButton:enabled { background:#1e7a3c; color:#fff; border-color:#2a9a4c;
                                          font-weight:bold; }
        QPushButton#stopButton:enabled  { background:#8a6d1f; color:#fff; border-color:#b8902a;
                                          font-weight:bold; }
        QLabel, QGroupBox { color:%(fg)s; }
        QGroupBox { border:1px solid %(bd)s; border-radius:4px; margin-top:8px; padding-top:6px; }
        QGroupBox::title { subcontrol-origin:margin; left:8px; padding:0 3px; }
        QLineEdit, QSpinBox, QComboBox, QPlainTextEdit, QTableWidget, QTreeWidget, QDateTimeEdit {
            background:%(fld)s; color:%(fg)s; border:1px solid %(bd)s; }
        QSpinBox:disabled, QCheckBox:disabled, QLabel:disabled, QLineEdit:disabled,
        QTableWidget:disabled, QListWidget:disabled { color:%(dis)s; }
        QHeaderView::section { background:%(btn)s; color:%(fg)s; border:0; padding:2px 4px; }
        QScrollBar:vertical, QScrollBar:horizontal { background:%(fld)s; width:10px; height:10px; }
        QScrollBar::handle { background:%(bd)s; border-radius:4px; min-height:20px; min-width:20px; }
        QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }
    """
    if light:
        return base % dict(bg="#f0f0f0", bgb="#d8d8d8", fg="#000", btn="#e0e0e0",
                           btnh="#d0d0d0", bd="#aaa", fld="#ffffff", dis="#999")
    return ("QWidget { background:#1e1e1e; color:#cccccc; }"
            "QToolTip { background:#2a2a2a; color:#cccccc; border:1px solid #555; }"
            "QTabBar::tab { background:#2a2a2a; color:#cccccc; border:1px solid #555;"
            "               padding:4px 12px; }"
            "QTabBar::tab:selected { background:#3a3a3a; color:#ffffff; }"
            "QTabWidget::pane { border:1px solid #555; }"
            "QListWidget { background:#2a2a2a; color:#cccccc; border:1px solid #555; }"
            "QTreeWidget { alternate-background-color:#262626; }"
            "QTreeView::indicator { width:12px; height:12px; border:1px solid #999; background:#1e1e1e; }"
            "QTreeView::indicator:checked { background:#4a9eff; border-color:#4a9eff; }"
            "QTreeView::indicator:indeterminate { background:#1e4f7a; border-color:#4a9eff; }"
            "QProgressBar { background:#2a2a2a; color:#cccccc; border:1px solid #555; text-align:center; }"
            "QProgressBar::chunk { background:#2a6f9a; }"
            "QListWidget::item:selected, QTableWidget::item:selected, QTreeWidget::item:selected"
            "  { background:#2a6f9a; color:#ffffff; }"
            "QComboBox QAbstractItemView { background:#2a2a2a; color:#cccccc;"
            "  selection-background-color:#2a6f9a; selection-color:#ffffff; }"
            "QTableCornerButton::section { background:#3a3a3a; border:0; }"
            + base) % dict(
        bg="#1e1e1e", bgb="#333", fg="#cccccc", btn="#3a3a3a", btnh="#4a4a4a",
        bd="#555", fld="#2a2a2a", dis="#666")


def apply_dark_palette(app):
    """Dark palette for every widget, dialogs and item views included.

    The stylesheet alone leaves text colours to the desktop theme, and a light
    system theme then draws black text on the dark backgrounds."""
    app.setStyle(QStyleFactory.create("Fusion"))
    pal = QPalette()
    colors = {
        QPalette.Window: "#1e1e1e", QPalette.WindowText: "#cccccc",
        QPalette.Base: "#2a2a2a", QPalette.AlternateBase: "#262626",
        QPalette.Text: "#cccccc", QPalette.BrightText: "#ffffff",
        QPalette.Button: "#3a3a3a", QPalette.ButtonText: "#cccccc",
        QPalette.Highlight: "#2a6f9a", QPalette.HighlightedText: "#ffffff",
        QPalette.ToolTipBase: "#2a2a2a", QPalette.ToolTipText: "#cccccc",
        QPalette.PlaceholderText: "#777777", QPalette.Link: "#4a9eff",
        QPalette.Light: "#4a4a4a", QPalette.Midlight: "#3a3a3a",
        QPalette.Mid: "#2a2a2a", QPalette.Dark: "#151515", QPalette.Shadow: "#000000",
    }
    for role, color in colors.items():
        pal.setColor(role, QColor(color))
    for role in (QPalette.WindowText, QPalette.Text, QPalette.ButtonText):
        pal.setColor(QPalette.Disabled, role, QColor("#666666"))
    app.setPalette(pal)


def is_local_host(host):
    if host in ("127.0.0.1", "localhost", "::1", socket.gethostname()):
        return True
    try:
        return socket.gethostbyname(host) in ("127.0.0.1",
                                              socket.gethostbyname(socket.gethostname()))
    except OSError:
        return False


_ENV_RE = re.compile(r"\$(?:\{([^}]*)\}|([A-Za-z0-9_]+))")


def expand_env(path):
    """Expand $NAME / ${NAME} like daoDAQ does for root_storage.

    Raises KeyError(name) for a variable that is not set."""
    def sub(match):
        name = match.group(1) if match.group(1) is not None else match.group(2)
        if name not in os.environ:
            raise KeyError(name)
        return os.environ[name]
    return _ENV_RE.sub(sub, path)


def validate_config(text, local):
    """Check a daoDAQ YAML configuration the way the server will read it.

    Returns (config dict or None, rows for the sources table, errors, warnings).
    `local` enables checks against this machine's filesystem (SHMs exist,
    root_storage exists) - pointless when daoDAQ runs on another host."""
    errors, warnings, rows = [], [], []
    try:
        cfg = yaml.safe_load(text)
    except yaml.YAMLError as exc:
        return None, rows, [f"YAML syntax: {exc}"], warnings

    if not isinstance(cfg, dict):
        return None, rows, ["the document must be a mapping (root_storage, sources)"], warnings

    root = cfg.get("root_storage")
    if not isinstance(root, str) or not root:
        errors.append("missing required 'root_storage'")
    elif local:
        try:
            expanded = expand_env(root)
        except KeyError as exc:
            errors.append(f"root_storage {root}: environment variable {exc} is not set")
        else:
            if not os.path.isdir(expanded):
                shown = root if expanded == root else f"{root} (= {expanded})"
                warnings.append(f"root_storage {shown} does not exist yet, it is created on Apply")

    sources = cfg.get("sources")
    if not isinstance(sources, list) or not sources:
        errors.append("'sources' must be a non-empty list")
        sources = []

    seen = set()
    for i, src in enumerate(sources):
        if not isinstance(src, dict) or "uri" not in src:
            errors.append(f"source #{i + 1}: missing 'uri'")
            rows.append([f"source #{i + 1}", "-", "-", "-", "-", "-", "missing uri"])   # one row per source
            continue
        uri = str(src["uri"])
        status = "ok"
        scheme, sep, location = uri.partition("://")
        if not sep or scheme not in ("smem", "file"):
            errors.append(f"{uri}: URI must start with smem:// or file://")
            status = "bad URI"
        elif location in seen:
            errors.append(f"{uri}: duplicate source")
            status = "duplicate"
        seen.add(location)

        if scheme == "file" and local and status == "ok":
            if not os.path.isfile(location):
                errors.append(f"{uri}: file does not exist (START would put daoDAQ in Error)")
                status = "file missing"
        elif scheme == "smem" and local and status == "ok":
            if not os.path.exists(location):
                errors.append(f"{uri}: SHM does not exist (Apply would put daoDAQ in Error)")
                status = "SHM missing"

        for key, value in src.items():
            if key not in SOURCE_KEYS:
                warnings.append(f"{uri}: unknown parameter '{key}' is ignored by daoDAQ")
            elif not isinstance(value, SOURCE_KEYS[key]) or (SOURCE_KEYS[key] is int and isinstance(value, bool)):
                errors.append(f"{uri}: '{key}' must be {SOURCE_KEYS[key].__name__}")
                status = f"bad {key}"
        if src.get("format", "fits") != "fits":
            errors.append(f"{uri}: only format 'fits' is supported")
        if src.get("metadata_only"):
            warnings.append(f"{uri}: metadata_only is read but not implemented, full frames are written")

        def opt(key, default="-"):
            return str(src[key]) if key in src else default

        if scheme == "file":   # copied once into the session directory at START
            rows.append([uri, "copy at START", "-", "-", "-", "-", status])
            continue
        rows.append([uri, opt("samples", "until STOP"), opt("file_rollover", "one file"),
                     opt("buffer_limit", "unlimited"),
                     f"{opt('daq_affinity')}/{opt('sink_affinity')}",
                     opt("eager_start", "True"), status])

    return (cfg if not errors else None), rows, errors, warnings


def expand_env_or_empty(path):
    try:
        return expand_env(path.strip())
    except KeyError:
        return ""


class ExtractWorker(QThread):
    """Runs daoDAQExtractor.extract() + write() off the GUI thread."""
    progress = pyqtSignal(int, int)
    done = pyqtSignal(str, str)
    failed = pyqtSignal(str)

    def __init__(self, catalog, t0, t1, path):
        super().__init__()
        self.catalog, self.t0, self.t1, self.path = catalog, t0, t1, path
        self.cancel = False

    def run(self):
        try:
            result = X.extract(self.catalog, self.t0, self.t1,
                               progress=self.progress.emit, cancelled=lambda: self.cancel)
            X.write(result, self.path, self.t0, self.t1)
        except InterruptedError:
            self.failed.emit("cancelled")
        except Exception as exc:   # noqa: BLE001
            self.failed.emit(f"failed: {exc}")
        else:
            self.done.emit(self.path, ", ".join(f"{n} {len(r.data)} frames" for n, r in result.items()))


class Main(QMainWindow, Ui_MainWindow):
    def __init__(self, host, port, config_file=None, config_dir=None, light=False):
        super().__init__()
        if not light:
            apply_dark_palette(QApplication.instance())
        self.setupUi(self)
        self.setStyleSheet(make_stylesheet(light))
        self.configEdit.setFont(QFont("Monospace", 9))
        self.logEdit.setFont(QFont("Monospace", 8))

        self.hostEdit.setText(host)
        self.portSpin.setValue(port)
        self.config_dir = config_dir
        self.local = is_local_host(host)
        self.config_path = None     # file shown in the editor
        self.config_dirty = False
        self.config_valid = None    # parsed dict when the editor content is valid
        self.model = None           # parsed document edited by the Sources tab
        self.applied_config = None  # last config this GUI applied (for root_storage)

        self.poll_client = None     # short timeout, used for STATE polling
        self.cmd_client = None      # long timeout, used for transitions
        self.state = None           # DAQState, None = not connected
        self.pid = None
        self.last_ping = 0.0
        self.local_proc = None      # daoDAQ launched from this GUI

        self.session_t0 = None      # time the current/last acquisition started
        self.session_dir = None
        self.session_end = None

        self.sourcesTable.setColumnCount(len(SOURCE_COLUMNS))
        self.sourcesTable.setHorizontalHeaderLabels(SOURCE_COLUMNS)
        self.sourcesTable.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents)
        self.sourcesTable.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        self.sourcesTable.horizontalHeaderItem(4).setToolTip("daq_affinity / sink_affinity")
        self.sourcesTable.verticalHeader().setVisible(False)

        # -- wire up
        self.connectButton.clicked.connect(self._connect)
        self.hostEdit.returnPressed.connect(self._connect)
        self.launchButton.clicked.connect(self._launch)
        self.shutdownButton.clicked.connect(self._shutdown)
        self.configCombo.activated.connect(lambda _: self._load_config(self.configCombo.currentData()))
        self.newButton.clicked.connect(self._new_config)
        self.browseButton.clicked.connect(self._browse)
        self.reloadButton.clicked.connect(lambda: self._load_config(self.config_path, force=True))
        self.saveButton.clicked.connect(self._save)
        self.saveAsButton.clicked.connect(self._save_as)
        self.applyButton.clicked.connect(self._apply)
        self.rootEdit.editingFinished.connect(self._root_edited)
        self.rootBrowseButton.clicked.connect(self._browse_root)
        self.shmFilterEdit.textChanged.connect(self._filter_shms)
        self.shmList.itemDoubleClicked.connect(lambda item: self._add_selected_shms([item]))
        self.addShmButton.clicked.connect(lambda: self._add_selected_shms())
        self.addFileButton.clicked.connect(self._add_file)
        self.removeSourceButton.clicked.connect(lambda: self._remove_source())
        self.sourcesTable.itemSelectionChanged.connect(self._form_load)
        self.sourcesTable.cellDoubleClicked.connect(lambda row, _: self._remove_source(row))
        for widget, _, _ in self._form_fields():
            widget.valueChanged.connect(self._form_changed)
        self.eagerCheck.toggled.connect(self._form_changed)
        self.sourcesSplitter.setSizes([300, 600])
        self.startButton.clicked.connect(self._start)
        self.stopButton.clicked.connect(self._stop)
        self.releaseButton.clicked.connect(self._release)
        self.recoverButton.clicked.connect(self._recover)
        self.openDirButton.clicked.connect(self._open_dir)

        self.validate_timer = QTimer(self, singleShot=True, interval=300)
        self.validate_timer.timeout.connect(self._validate)
        self.configEdit.textChanged.connect(self._config_edited)

        self._refresh_shm_list()
        self.shm_timer = QTimer(self)
        self.shm_timer.timeout.connect(self._refresh_shm_list)
        self.shm_timer.start(SHM_REFRESH_MS)

        self._fill_config_combo()
        if config_file:
            self._load_config(config_file)
        elif self.configCombo.count():
            self._load_config(self.configCombo.itemData(0))
        else:
            self._new_config()

        self._setup_extract_tab()

        self.poll_timer = QTimer(self)
        self.poll_timer.timeout.connect(self._poll)
        self._connect()

    # ------------------------------------------------------------------
    # activity log
    def _log(self, msg, level="info"):
        color = {"info": "", "ok": "#4caf50", "warn": "#e0a030", "error": "#e05050"}[level]
        stamp = time.strftime("%H:%M:%S")
        html = f"<span style='color:{color}'>{stamp}  {msg}</span>" if color else f"{stamp}  {msg}"
        self.logEdit.appendHtml(html)
        self.logEdit.moveCursor(QTextCursor.End)

    # ------------------------------------------------------------------
    # connection
    def _connect(self):
        host, port = self.hostEdit.text().strip(), self.portSpin.value()
        self.poll_client = DAQClient(host, port, timeout_s=POLL_TIMEOUT_S)
        self.cmd_client = DAQClient(host, port, timeout_s=CMD_TIMEOUT_S)
        # daoCommandIfce logs every request (and each timeout while daoDAQ is
        # down); the GUI reports what matters itself.
        logging.getLogger("daoCommandIfce").setLevel(logging.CRITICAL)
        self.local = is_local_host(host)
        self.state, self.pid, self.last_ping = None, None, 0.0
        self._log(f"connecting to daoDAQ at {host}:{port}")
        self._validate()
        self._poll()

    def _port_open(self):
        """Quick TCP check, so that polling a daoDAQ that is not running does
        not block the GUI for the whole ZMQ timeout."""
        try:
            with socket.create_connection((self.hostEdit.text().strip(), self.portSpin.value()),
                                          timeout=PROBE_TIMEOUT_S):
                return True
        except OSError:
            return False

    def _poll(self):
        previous = self.state
        try:
            if self.state is None and not self._port_open():
                raise ConnectionError("nobody listening")
            self.state = self.poll_client.state()
        except Exception:   # noqa: BLE001 - nobody answering
            self.state = None
            self.pid = None

        if self.state is not None and (self.pid is None or time.time() - self.last_ping > 5):
            self._ping()

        if self.state != previous:
            self._state_changed(previous)
        if self.session_t0 is not None and self.session_end is None:
            self._update_session()

        self.poll_timer.start(POLL_MS if self.state is not None else POLL_DISCONNECTED_MS)
        self._refresh_widgets()

    def _ping(self):
        self.last_ping = time.time()
        try:
            status, payload = self.poll_client.api.Ping()
            if status == 0:
                self.pid = int(str(payload).split(":")[-1])
        except Exception:   # noqa: BLE001 - timeout or unexpected payload
            self.pid = None

    def _state_changed(self, previous):
        level = {DAQState.Error: "error", None: "warn"}.get(self.state, "info")
        self._log(f"daoDAQ state: {STATE_LOG[self.state]}", level)
        self._validate()   # SHMs may have appeared/vanished since the last edit
        if self.state is DAQState.Error:
            cause = self._last_server_error()
            if cause:
                self._log(f"daoDAQ says: {cause}", "error")

        if self.state is DAQState.Acquiring and previous is not DAQState.Acquiring:
            # started here, by daoDAQCli.py, or by anything else
            self.session_t0 = time.time()
            self.session_dir, self.session_end = None, None
        if previous is DAQState.Acquiring and self.state is not DAQState.Acquiring:
            self.session_end = time.time()
            self.catalog_stale = True
            self._update_session()
            if self.state is DAQState.Ready:
                self._log("acquisition finished", "ok")

    def _last_server_error(self):
        """Last ERROR/CRITICAL line of a local daoDAQ's log file, if readable.

        The server replies SUCCESS to every command, so its log is the only
        place the reason for an Error is reported."""
        if not self.local or not os.getenv("DAODATA"):
            return None
        try:
            with open(os.path.join(os.getenv("DAODATA"), "daoDAQ.logs"), "rb") as f:
                f.seek(0, os.SEEK_END)
                f.seek(max(0, f.tell() - 16384))
                lines = f.read().decode(errors="replace").splitlines()
        except OSError:
            return None
        for line in reversed(lines):
            if "[ERROR]" in line or "[CRIT" in line:
                msg = line.split(" - ", 1)[-1].strip()
                if msg != "Could not change state.":   # generic, the cause is logged before it
                    return msg
        return None

    def _refresh_widgets(self):
        text, bg = STATE_STYLE[self.state]
        self.stateLabel.setText(text)
        self.stateLabel.setStyleSheet(
            f"background:{bg}; color:#fff; font-weight:bold; border-radius:4px; padding:4px;")
        if self.state is None:
            self.pidLabel.setText("")
        else:
            where = "local" if self.local else self.hostEdit.text()
            self.pidLabel.setText(f"daoDAQ pid {self.pid if self.pid else '?'} ({where})"
                                  + ("   - launched from this GUI" if self._own_proc_alive() else ""))

        s = self.state
        connected = s is not None
        self.launchButton.setEnabled(not connected and self.local)
        self.shutdownButton.setEnabled(connected and self.local and self.pid is not None)
        self.applyButton.setEnabled(connected and s is not DAQState.Acquiring
                                    and self.config_valid is not None)
        self.startButton.setEnabled(s is DAQState.Ready)
        self.stopButton.setEnabled(s is DAQState.Acquiring)
        self.releaseButton.setEnabled(s in (DAQState.Configured, DAQState.Ready))
        self.recoverButton.setEnabled(s is DAQState.Error)
        self.openDirButton.setEnabled(self.session_dir is not None and self.local)
        self.saveButton.setEnabled(self.config_dirty or self.config_path is None)
        self.reloadButton.setEnabled(self.config_path is not None)

        title = "daoDAQCtrl"
        name = os.path.basename(self.config_path) if self.config_path else "untitled"
        title += f"  [{name}{' *' if self.config_dirty else ''}]"
        self.setWindowTitle(title)

    # ------------------------------------------------------------------
    # daoDAQ process
    def _own_proc_alive(self):
        return self.local_proc is not None and self.local_proc.poll() is None

    def _launch(self):
        if not os.getenv("DAODATA"):
            QMessageBox.critical(self, "daoDAQCtrl", "DAODATA is not set; daoDAQ refuses to start without it.")
            return
        port = self.portSpin.value()
        try:
            self.local_proc = subprocess.Popen(
                ["daoDAQ", "-p", str(port)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True)   # survives the GUI being closed
        except OSError as exc:
            self._log(f"failed to launch daoDAQ: {exc}", "error")
            return
        self._log(f"launched daoDAQ (pid {self.local_proc.pid}), logs in "
                  f"{os.path.join(os.getenv('DAODATA'), 'daoDAQ.logs')}", "ok")
        QTimer.singleShot(700, self._poll)

    def _shutdown(self):
        if self.state is DAQState.Acquiring:
            if QMessageBox.question(self, "daoDAQCtrl",
                                    "An acquisition is running. Stop it and shut daoDAQ down?") \
                    != QMessageBox.Yes:
                return
        try:
            os.kill(self.pid, signal.SIGINT)
            self._log(f"sent SIGINT to daoDAQ (pid {self.pid})")
        except OSError as exc:
            self._log(f"failed to signal pid {self.pid}: {exc}", "error")
        QTimer.singleShot(700, self._poll)

    # ------------------------------------------------------------------
    # configuration file
    def _fill_config_combo(self):
        self.configCombo.clear()
        if self.config_dir and os.path.isdir(self.config_dir):
            for path in sorted(glob.glob(os.path.join(self.config_dir, "*.y*ml"))):
                self.configCombo.addItem(os.path.basename(path), path)

    def _select_in_combo(self, path):
        if path is None:
            self.configCombo.setCurrentIndex(-1)
            return
        idx = self.configCombo.findData(path)
        if idx < 0:
            self.configCombo.addItem(os.path.basename(path), path)
            idx = self.configCombo.count() - 1
        self.configCombo.setCurrentIndex(idx)

    def _confirm_discard(self):
        if not self.config_dirty:
            return True
        return QMessageBox.question(self, "daoDAQCtrl", "Discard the unsaved changes to the configuration?") \
            == QMessageBox.Yes

    def _set_text(self, text):
        self.configEdit.blockSignals(True)
        self.configEdit.setPlainText(text)
        self.configEdit.blockSignals(False)

    def _load_config(self, path, force=False):
        if not path or (path == self.config_path and not force):
            return
        if not self._confirm_discard():
            self._select_in_combo(self.config_path)
            return
        path = os.path.abspath(os.path.expanduser(path))
        try:
            with open(path) as f:
                text = f.read()
        except OSError as exc:
            self._log(f"cannot read {path}: {exc}", "error")
            return
        self._set_text(text)
        self.config_path, self.config_dirty = path, False
        self._select_in_combo(path)
        self._log(f"loaded {path}")
        self._validate()

    def _new_config(self):
        if not self._confirm_discard():
            return
        self.model = {"root_storage": DEFAULT_ROOT_STORAGE, "sources": []}
        self.config_path = None
        self._select_in_combo(None)
        self._push_model()
        self._log("new configuration: double-click SHMs to record them, then Save as...")

    def _browse(self):
        start = os.path.dirname(self.config_path) if self.config_path else (self.config_dir or os.getcwd())
        path, _ = QFileDialog.getOpenFileName(self, "daoDAQ configuration", start,
                                              "YAML (*.yaml *.yml);;All files (*)")
        if path:
            self._load_config(path)

    def _write(self, path):
        try:
            with open(path, "w") as f:
                f.write(self.configEdit.toPlainText())
        except OSError as exc:
            self._log(f"cannot write {path}: {exc}", "error")
            return False
        self.config_path, self.config_dirty = path, False
        self._select_in_combo(path)
        self._log(f"saved {path}", "ok")
        self._refresh_widgets()
        return True

    def _save(self):
        if self.config_path:
            self._write(self.config_path)
        else:
            self._save_as()

    def _save_as(self):
        start = self.config_path or os.path.join(self.config_dir or os.getcwd(), "daq.yaml")
        path, _ = QFileDialog.getSaveFileName(self, "Save daoDAQ configuration", start,
                                              "YAML (*.yaml *.yml)")
        if path:
            if not os.path.splitext(path)[1]:
                path += ".yaml"
            self._write(path)

    def _config_edited(self):
        """The YAML tab was edited by hand."""
        self.config_dirty = True
        self.validate_timer.start()
        self._refresh_widgets()

    def _validate(self):
        """Parse the YAML text, check it, and refresh everything shown from it."""
        text = self.configEdit.toPlainText()
        cfg, rows, errors, warnings = validate_config(text, self.local)
        self.config_valid = cfg
        try:
            parsed = yaml.safe_load(text)
        except yaml.YAMLError:
            parsed = None
        if isinstance(parsed, dict):
            parsed.setdefault("sources", [])
        # The structured editor works on the parsed document; while the YAML
        # does not parse into a mapping with a list of sources it is disabled.
        self.model = parsed if isinstance(parsed, dict) and isinstance(parsed["sources"], list) else None
        self.sourcesTab.setEnabled(self.model is not None)
        self.rootEdit.setEnabled(self.model is not None)
        self.rootBrowseButton.setEnabled(self.model is not None)

        if self.model is not None and not self.rootEdit.hasFocus():
            self.rootEdit.setText(str(self.model.get("root_storage") or ""))

        selected = self._selected_source()
        self.sourcesTable.blockSignals(True)
        self.sourcesTable.setRowCount(len(rows))
        for r, row in enumerate(rows):
            for c, value in enumerate(row):
                item = QTableWidgetItem(os.path.basename(value) if c == 0 else value)
                if c == 0:
                    item.setToolTip(value)
                if c == len(row) - 1 and value != "ok":
                    item.setForeground(QColor("#e05050"))
                self.sourcesTable.setItem(r, c, item)
        if selected is not None and selected < len(rows):
            self.sourcesTable.selectRow(selected)
        self.sourcesTable.blockSignals(False)
        self._form_load()
        self._mark_recorded_shms()

        if errors:
            text = "<span style='color:#e05050'>✗ " + "<br>✗ ".join(errors) + "</span>"
        else:
            n = len(rows)
            finite = all(row[1] != "until STOP" for row in rows)
            text = (f"<span style='color:#4caf50'>✓ {n} source{'s' * (n != 1)}, "
                    + ("stops by itself when every source has its samples" if finite
                       else "runs until STOP") + "</span>")
        if warnings:
            text += "<br><span style='color:#e0a030'>⚠ " + "<br>⚠ ".join(warnings) + "</span>"
        self.validLabel.setText(text)
        self._refresh_widgets()

    # ------------------------------------------------------------------
    # structured editor (Sources tab): edits self.model, then rewrites the YAML
    def _push_model(self, select=None):
        """Write self.model back into the YAML tab and refresh from it.

        Comments in a hand-written file are lost from the first structured edit
        on; the YAML tab stays available for hand editing."""
        self._set_text(yaml.safe_dump(self.model, sort_keys=False, default_flow_style=False))
        self.config_dirty = True
        if select is not None:
            self.sourcesTable.blockSignals(True)
            self.sourcesTable.setRowCount(max(self.sourcesTable.rowCount(), select + 1))
            self.sourcesTable.selectRow(select)
            self.sourcesTable.blockSignals(False)
        self._validate()

    def _selected_source(self):
        rows = self.sourcesTable.selectionModel().selectedRows()
        return rows[0].row() if rows else None

    def _source_index(self, uri):
        for i, src in enumerate(self.model["sources"]):
            if isinstance(src, dict) and src.get("uri") == uri:
                return i
        return None

    def _add_source(self, src):
        if self.model is None:
            self._log("fix the YAML first: the Sources tab needs a valid document", "warn")
            return
        idx = self._source_index(src["uri"])
        if idx is not None:
            self._log(f"{src['uri']} is already recorded")
        else:
            self.model["sources"].append(src)
            idx = len(self.model["sources"]) - 1
            self._log(f"added {src['uri']}")
        self._push_model(select=idx)

    def _add_selected_shms(self, items=None):
        items = items or self.shmList.selectedItems()
        for item in items:
            self._add_source({"uri": "smem://" + os.path.join(SHM_DIR, item.text()),
                              "buffer_limit": DEFAULT_BUFFER_LIMIT})

    def _add_file(self):
        path, _ = QFileDialog.getOpenFileName(self, "File copied into each session", os.getcwd())
        if path:
            self._add_source({"uri": "file://" + os.path.abspath(path)})

    def _remove_source(self, row=None):
        row = self._selected_source() if row is None else row
        if self.model is None or row is None or row >= len(self.model["sources"]):
            return
        src = self.model["sources"].pop(row)
        self._log(f"removed {src.get('uri', src) if isinstance(src, dict) else src}")
        n = len(self.model["sources"])
        self._push_model(select=min(row, n - 1) if n else None)

    def _root_edited(self):
        if self.model is None:
            return
        root = self.rootEdit.text().strip()
        if root == (self.model.get("root_storage") or ""):
            return
        self.model["root_storage"] = root
        self._push_model(select=self._selected_source())

    def _browse_root(self):
        try:
            start = expand_env(self.rootEdit.text()) or os.getenv("DAODATA", os.getcwd())
        except KeyError:
            start = os.getcwd()
        path = QFileDialog.getExistingDirectory(self, "root_storage", start)
        if path:
            self.rootEdit.setText(path)
            self._root_edited()

    # form widget -> (key, value meaning "not set")
    def _form_fields(self):
        return [(self.samplesSpin, "samples", 0), (self.rolloverSpin, "file_rollover", 0),
                (self.bufferSpin, "buffer_limit", 0), (self.daqAffSpin, "daq_affinity", -1),
                (self.sinkAffSpin, "sink_affinity", -1)]

    def _form_load(self):
        """Show the selected source's parameters in the form."""
        row = self._selected_source()
        src = None
        if self.model is not None and row is not None and row < len(self.model["sources"]):
            src = self.model["sources"][row]
        is_smem = isinstance(src, dict) and str(src.get("uri", "")).startswith("smem://")
        self.sourceForm.setEnabled(is_smem)
        self.removeSourceButton.setEnabled(src is not None)
        self.sourceForm.setTitle(f"Selected source: {os.path.basename(str(src.get('uri', '')))}"
                                 if isinstance(src, dict) else "Selected source")
        widgets = [w for w, _, _ in self._form_fields()] + [self.eagerCheck]
        for w in widgets:
            w.blockSignals(True)
        for widget, key, unset in self._form_fields():
            value = src.get(key) if is_smem else None
            widget.setValue(value if isinstance(value, int) and not isinstance(value, bool) else unset)
        self.eagerCheck.setChecked(src.get("eager_start", True) is not False if is_smem else True)
        for w in widgets:
            w.blockSignals(False)

    def _form_changed(self):
        row = self._selected_source()
        if self.model is None or row is None or row >= len(self.model["sources"]):
            return
        src = self.model["sources"][row]
        for widget, key, unset in self._form_fields():
            if widget.value() == unset:
                src.pop(key, None)
            else:
                src[key] = widget.value()
        if self.eagerCheck.isChecked():
            src.pop("eager_start", None)   # default
        else:
            src["eager_start"] = False
        self._push_model(select=row)

    # ------------------------------------------------------------------
    # available SHMs (same convention as daoShmViewer: /tmp/*.im.shm)
    def _refresh_shm_list(self):
        names = sorted(os.path.basename(p) for p in glob.glob(os.path.join(SHM_DIR, "*.im.shm")))
        current = [self.shmList.item(i).text() for i in range(self.shmList.count())]
        if names != current:
            selected = {item.text() for item in self.shmList.selectedItems()}
            self.shmList.clear()
            for name in names:
                item = QListWidgetItem(name)
                self.shmList.addItem(item)
                item.setSelected(name in selected)
            self._filter_shms()
            self._mark_recorded_shms()

    def _filter_shms(self):
        needle = self.shmFilterEdit.text().lower()
        for i in range(self.shmList.count()):
            item = self.shmList.item(i)
            item.setHidden(needle not in item.text().lower())

    def _mark_recorded_shms(self):
        recorded = set()
        if self.model is not None:
            recorded = {str(s.get("uri", "")) for s in self.model["sources"] if isinstance(s, dict)}
        for i in range(self.shmList.count()):
            item = self.shmList.item(i)
            on = "smem://" + os.path.join(SHM_DIR, item.text()) in recorded
            font = item.font()
            font.setBold(on)
            item.setFont(font)
            item.setForeground(QColor("#4caf50") if on else self.shmList.palette().text().color())
            item.setToolTip("recorded (double-click in the right table to remove)" if on
                            else "double-click to record")

    # ------------------------------------------------------------------
    # daoDAQ commands
    def _run(self, what, func):
        """Run a daoDAQClient call, report the outcome, refresh the state."""
        QApplication.setOverrideCursor(Qt.WaitCursor)
        try:
            func()
            ok = True
        except Exception as exc:   # noqa: BLE001
            self._log(f"{what} failed: {exc}", "error")
            ok = False
        finally:
            QApplication.restoreOverrideCursor()
        self._poll()
        return ok

    def _apply(self):
        text = self.configEdit.toPlainText()
        cfg = self.config_valid
        if cfg is None:
            return
        if self.local:
            root = expand_env(cfg["root_storage"])
            if not os.path.isdir(root):
                try:
                    os.makedirs(root)
                except OSError as exc:
                    self._log(f"cannot create root_storage {root}: {exc}", "error")
                    return
                self._log(f"created root_storage {root}")
        if self.local and "$" in cfg["root_storage"]:
            # Expanded here as well, so that a daoDAQ built before it learned
            # to expand environment variables gets a real path.
            upload = dict(cfg, root_storage=expand_env(cfg["root_storage"]))
            text = yaml.safe_dump(upload, sort_keys=False, default_flow_style=False)

        def do():
            if self.cmd_client.state() is DAQState.Error:
                # daoDAQ accepts a new config in Error; Recover re-applies it.
                self.cmd_client.daq_session_configure_upload(text)
                self.cmd_client.recover()
            else:
                self.cmd_client.daq_session_configure_upload(text)
                self.cmd_client.daq_session_configure_apply()

        if self._run("apply configuration", do):
            self.applied_config = cfg
            src = os.path.basename(self.config_path) if self.config_path else "editor"
            self._log(f"configuration applied ({src}{', unsaved edits' if self.config_dirty else ''}), "
                      "daoDAQ is ready", "ok")

    def _start(self):
        if self._run("start", self.cmd_client.daq_session_begin):
            self._log("acquisition started", "ok")

    def _stop(self):
        self._run("stop", self.cmd_client.daq_session_finish)

    def _release(self):
        def do():
            api = self.cmd_client.api
            if self.cmd_client.state() is DAQState.Ready:
                self.cmd_client.dao_invoke_(api.Exec, "Disable")
            self.cmd_client.dao_invoke_(api.Exec, "Stop")

        if self._run("release", do):
            self._log("SHMs released, configuration unloaded")

    def _recover(self):
        if self._run("recover", self.cmd_client.recover):
            self._log("recovered, daoDAQ is ready", "ok")

    # ------------------------------------------------------------------
    # session output
    def _root_storage(self):
        cfg = self.applied_config or self.config_valid
        try:
            return expand_env(cfg["root_storage"]) if cfg else None
        except KeyError:
            return None

    def _update_session(self):
        if self.session_t0 is None:
            return
        elapsed = (self.session_end or time.time()) - self.session_t0
        running = self.session_end is None

        root = self._root_storage()
        if self.session_dir is None and self.local and root and os.path.isdir(root):
            # daoDAQ names the session directory after the local time it started
            candidates = [os.path.join(root, d) for d in os.listdir(root)]
            candidates = [d for d in candidates
                          if os.path.isdir(d) and os.path.getmtime(d) >= self.session_t0 - 5]
            if candidates:
                self.session_dir = max(candidates, key=os.path.getmtime)

        if self.session_dir:
            n_files, n_bytes = 0, 0
            for dirpath, _, files in os.walk(self.session_dir):
                for name in files:
                    if name.endswith(".fits"):
                        n_files += 1
                        try:
                            n_bytes += os.path.getsize(os.path.join(dirpath, name))
                        except OSError:
                            pass
            self.sessionLabel.setText(f"Session: {self.session_dir}")
            self.progressLabel.setText(
                f"{'running' if running else 'finished'}  {elapsed:7.1f} s   "
                f"{n_files} FITS file{'s' * (n_files != 1)}   {n_bytes / 1e6:.1f} MB")
        else:
            self.sessionLabel.setText("Session: directory not visible from this machine"
                                      if not self.local else "Session: waiting for directory...")
            self.progressLabel.setText(f"{'running' if running else 'finished'}  {elapsed:7.1f} s")

    def _open_dir(self):
        if self.session_dir:
            subprocess.Popen(["xdg-open", self.session_dir],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # ------------------------------------------------------------------
    # Extract tab: telemetry between two times (daoDAQExtractor)
    def _setup_extract_tab(self):
        self.catalog = []             # [daoDAQExtractor.Session] of the last scan
        self.catalog_stale = True     # an acquisition ended since the last scan
        self.output_auto = True       # output name follows the range until edited
        self.extract_worker = None

        for ext in X.available_formats():
            self.formatCombo.addItem(f"{X.FORMATS[ext]} ({ext})", ext)
        header = self.catalogTree.header()
        header.setSectionResizeMode(QHeaderView.ResizeToContents)
        header.setStretchLastSection(True)
        self.extractProgress.setVisible(False)
        self.cancelExtractButton.setVisible(False)

        self.estimate_timer = QTimer(self, singleShot=True, interval=250)
        self.estimate_timer.timeout.connect(self._estimate)
        self.mainTabs.currentChanged.connect(self._tab_changed)
        self.scanButton.clicked.connect(self._scan)
        self.extractRootEdit.returnPressed.connect(self._scan)
        self.extractRootBrowseButton.clicked.connect(self._browse_extract_root)
        self.catalogTree.itemChanged.connect(lambda *_: self.estimate_timer.start())
        self.catalogTree.itemDoubleClicked.connect(self._use_item_span)
        self.startEdit.dateTimeChanged.connect(lambda *_: self._range_changed())
        self.endEdit.dateTimeChanged.connect(lambda *_: self._range_changed())
        self.fitRangeButton.clicked.connect(self._fit_to_ticked)
        self.formatCombo.activated.connect(lambda _: self._set_output_ext(self.formatCombo.currentData()))
        self.outputEdit.textEdited.connect(self._output_edited)
        self.outputBrowseButton.clicked.connect(self._browse_output)
        self.extractButton.clicked.connect(self._extract)
        self.cancelExtractButton.clicked.connect(self._cancel_extract)
        self._estimate()

    def _tab_changed(self, index):
        if self.mainTabs.widget(index) is not self.extractTab:
            return
        if not self.extractRootEdit.text():
            self.extractRootEdit.setText(self._root_storage() or os.getenv("DAODATA", ""))
        if self.catalog_stale:
            self._scan()

    def _browse_extract_root(self):
        path = QFileDialog.getExistingDirectory(self, "daoDAQ root_storage",
                                                self.extractRootEdit.text() or os.getcwd())
        if path:
            self.extractRootEdit.setText(path)
            self._scan()

    def _scan(self):
        if self._extracting():
            return
        X.close(self.catalog)
        self.catalog, self.catalog_stale = [], False
        self.catalogTree.clear()
        try:
            root = expand_env(self.extractRootEdit.text().strip())
        except KeyError as exc:
            self._log(f"environment variable {exc} is not set", "error")
            return
        if not root:
            return
        QApplication.setOverrideCursor(Qt.WaitCursor)
        try:
            self.catalog = X.scan(root)
        except Exception as exc:   # noqa: BLE001
            self._log(f"cannot scan {root}: {exc}", "error")
            self.catalog = []
        finally:
            QApplication.restoreOverrideCursor()

        self.catalogTree.blockSignals(True)
        flags = Qt.ItemIsEnabled | Qt.ItemIsSelectable | Qt.ItemIsUserCheckable
        newest = len(self.catalog) - 1
        for i, session in enumerate(self.catalog):
            top = QTreeWidgetItem(self.catalogTree)
            top.setFlags(flags | Qt.ItemIsAutoTristate)
            top.setData(0, Qt.UserRole, (i, None))
            n = sum(s.n_frames for s in session.sources.values())
            self._fill_row(top, session.name, n, session.first_atime, session.last_atime, "")
            for name, source in session.sources.items():
                child = QTreeWidgetItem(top)
                child.setFlags(flags)
                child.setData(0, Qt.UserRole, (i, name))
                shape = "x".join(map(str, source.shape))
                self._fill_row(child, name, source.n_frames, source.first_atime, source.last_atime,
                               f"{shape} {source.dtype}")
                # the newest session is ticked by default
                child.setCheckState(0, Qt.Checked if i == newest else Qt.Unchecked)
            top.setExpanded(i == newest)
        self.catalogTree.blockSignals(False)

        n_sources = sum(len(s.sources) for s in self.catalog)
        self._log(f"scanned {root}: {len(self.catalog)} session{'s' * (len(self.catalog) != 1)}, "
                  f"{n_sources} recorded source{'s' * (n_sources != 1)}")
        if self.catalog:
            self._set_range(self.catalog[-1].first_atime, self.catalog[-1].last_atime)
        self._estimate()

    @staticmethod
    def _fill_row(item, name, frames, first, last, frame):
        item.setText(0, name)
        item.setText(1, str(frames))
        item.setText(2, X.format_time(first) if first is not None else "-")
        item.setText(3, X.format_time(last) if last is not None else "-")
        item.setText(4, frame)
        item.setTextAlignment(1, Qt.AlignRight | Qt.AlignVCenter)

    def _ticked(self):
        """Catalog restricted to the ticked sources."""
        picked = []
        for i in range(self.catalogTree.topLevelItemCount()):
            top = self.catalogTree.topLevelItem(i)
            session = self.catalog[top.data(0, Qt.UserRole)[0]]
            names = [top.child(j).data(0, Qt.UserRole)[1] for j in range(top.childCount())
                     if top.child(j).checkState(0) == Qt.Checked]
            if names:
                picked.append(X.Session(session.name, session.path,
                                        {n: session.sources[n] for n in names}))
        return picked

    # time range: QDateTimeEdit has ms resolution; the end keeps its whole ms
    def _range(self):
        t0 = self.startEdit.dateTime().toMSecsSinceEpoch() * 10**6
        t1 = self.endEdit.dateTime().toMSecsSinceEpoch() * 10**6 + 999_999
        return t0, t1

    def _set_range(self, t0, t1):
        if t0 is None or t1 is None:
            return
        for edit, ns in ((self.startEdit, t0), (self.endEdit, t1)):
            edit.blockSignals(True)
            edit.setDateTime(QDateTime.fromMSecsSinceEpoch(ns // 10**6))
            edit.blockSignals(False)
        self._range_changed()

    def _range_changed(self):
        if self.output_auto:
            self._auto_output()
        self.estimate_timer.start()

    def _use_item_span(self, item, _column):
        i, name = item.data(0, Qt.UserRole)
        span = self.catalog[i].sources[name] if name else self.catalog[i]
        self._set_range(span.first_atime, span.last_atime)

    def _fit_to_ticked(self):
        ticked = [s for session in self._ticked() for s in session.sources.values()]
        if ticked:
            self._set_range(min(s.first_atime for s in ticked), max(s.last_atime for s in ticked))

    def _estimate(self):
        if self._extracting():
            return
        t0, t1 = self._range()
        ticked = self._ticked()
        if not self.catalog:
            text, ok = "Nothing to extract: scan a root_storage holding daoDAQ sessions.", False
        elif t1 < t0:
            text, ok = "<span style='color:#e05050'>the end is before the start</span>", False
        elif not ticked:
            text, ok = "Tick at least one source.", False
        else:
            frames, size = X.estimate(ticked, t0, t1)
            n = len(X.plan(ticked, t0, t1))
            text = (f"{frames} frame{'s' * (frames != 1)} from {n} source{'s' * (n != 1)}, "
                    f"{size / 1e6:.1f} MB")
            if size > 4e9:
                text = f"<span style='color:#e0a030'>{text} - held in memory while extracting</span>"
            ok = frames > 0
        self.summaryLabel.setText(text)
        self.extractButton.setEnabled(ok and bool(self.outputEdit.text().strip()))

    # output file
    def _auto_output(self):
        t0, _ = self._range()
        folder = expand_env_or_empty(self.extractRootEdit.text()) or os.getcwd()
        stamp = X.format_time(t0).replace("-", "").replace(":", "").replace(" ", "_")[:15]
        self.outputEdit.setText(os.path.join(folder, f"extract_{stamp}{self.formatCombo.currentData()}"))

    def _output_edited(self, text):
        self.output_auto = False
        ext = os.path.splitext(text)[1].lower()
        idx = self.formatCombo.findData(ext)
        if idx >= 0:
            self.formatCombo.setCurrentIndex(idx)
        self.estimate_timer.start()

    def _set_output_ext(self, ext):
        path = self.outputEdit.text().strip()
        if path:
            self.outputEdit.setText(os.path.splitext(path)[0] + ext)

    def _browse_output(self):
        ext = self.formatCombo.currentData()
        path, _ = QFileDialog.getSaveFileName(self, "Extract to", self.outputEdit.text() or os.getcwd(),
                                              ";;".join(f"{X.FORMATS[e]} (*{e})" for e in X.available_formats()))
        if path:
            if os.path.splitext(path)[1].lower() not in X.FORMATS:
                path += ext
            self.outputEdit.setText(path)
            self._output_edited(path)

    # extraction (worker thread)
    def _extracting(self):
        return self.extract_worker is not None and self.extract_worker.isRunning()

    def _extract(self):
        path = os.path.abspath(os.path.expanduser(self.outputEdit.text().strip()))
        ext = os.path.splitext(path)[1].lower()
        if ext not in X.available_formats():
            self._log(f"cannot write '{ext}' files here (available: {', '.join(X.available_formats())})", "error")
            return
        if os.path.exists(path) and QMessageBox.question(
                self, "daoDAQCtrl", f"{path} exists. Overwrite it?") != QMessageBox.Yes:
            return
        t0, t1 = self._range()
        self.extract_worker = ExtractWorker(self._ticked(), t0, t1, path)
        self.extract_worker.progress.connect(self._extract_progress)
        self.extract_worker.done.connect(self._extract_done)
        self.extract_worker.failed.connect(self._extract_failed)
        for w in self._extract_inputs():
            w.setEnabled(False)
        self.extractProgress.setValue(0)
        self.extractProgress.setVisible(True)
        self.cancelExtractButton.setVisible(True)
        self._log(f"extracting {X.format_time(t0)} -> {X.format_time(t1)} to {path}")
        self.extract_worker.start()

    def _extract_inputs(self):
        return [self.extractRootEdit, self.extractRootBrowseButton, self.scanButton, self.catalogTree,
                self.rangeGroup, self.outputEdit, self.formatCombo, self.outputBrowseButton,
                self.extractButton]

    def _extract_progress(self, done, total):
        self.extractProgress.setMaximum(max(total, 1))
        self.extractProgress.setValue(done)

    def _extract_finished(self):
        for w in self._extract_inputs():
            w.setEnabled(True)
        self.extractProgress.setVisible(False)
        self.cancelExtractButton.setVisible(False)
        self._estimate()

    def _extract_done(self, path, summary):
        self._log(f"extracted to {path}: {summary}", "ok")
        self._extract_finished()

    def _extract_failed(self, message):
        self._log(f"extraction {message}", "warn" if message == "cancelled" else "error")
        self._extract_finished()

    def _cancel_extract(self):
        if self._extracting():
            self.extract_worker.cancel = True

    # ------------------------------------------------------------------
    def closeEvent(self, event):
        if not self._confirm_discard():
            event.ignore()
            return
        if self._extracting():
            if QMessageBox.question(self, "daoDAQCtrl", "An extraction is running. Cancel it and quit?") \
                    != QMessageBox.Yes:
                event.ignore()
                return
            self.extract_worker.cancel = True
            self.extract_worker.wait()
        X.close(self.catalog)
        if self._own_proc_alive():
            self._log("daoDAQ launched from this GUI keeps running")
        event.accept()


def parse_args():
    p = argparse.ArgumentParser(description="daoDAQ control panel")
    p.add_argument("--host", default="127.0.0.1", help="daoDAQ host")
    p.add_argument("--port", type=int, default=DAQClient.DEFAULT_TCP_PORT, help="daoDAQ port")
    p.add_argument("--config", help="configuration file to open at start-up")
    p.add_argument("--config-dir", default=os.path.join(os.getenv("DAOROOT", "."), "config", "daq"),
                   help="directory listed in the configuration drop-down")
    p.add_argument("--light", action="store_true", help="light mode")
    return p.parse_args()


def main():
    args = parse_args()
    app = QApplication(sys.argv)
    try:
        gui = Main(args.host, args.port, config_file=args.config,
                   config_dir=args.config_dir, light=args.light)
    except Exception as exc:   # noqa: BLE001
        print(f"daoDAQCtrl: {exc}", file=sys.stderr)
        QMessageBox.critical(None, "daoDAQCtrl", str(exc))
        return 1
    gui.show()
    return app.exec_()


if __name__ == "__main__":
    sys.exit(main())
