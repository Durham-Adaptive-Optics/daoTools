#!/usr/bin/env python3
"""Live view of local dao SHM segments and their attached processes.

Semaphore registration identifies readers; other processes with the SHM open
are shown as attached because dao does not record publisher PIDs in metadata.
"""
import argparse
import glob
import math
import os
import subprocess
import sys
import time
from collections import deque
import shlex

import daoShm
import numpy as np
import psutil
from PyQt5.QtCore import Qt, QRect, QTimer
from PyQt5.QtGui import QColor, QCursor, QFont, QPainter, QPen
from PyQt5.QtWidgets import (
    QApplication,
    QDialog,
    QDialogButtonBox,
    QLabel,
    QMainWindow,
    QPlainTextEdit,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)


SHM_DIR = "." if os.name == "nt" else "/tmp"
REFRESH_MS = 1000


def process_command(pid):
    """Return a process command line using the cross-platform process API."""
    try:
        command = psutil.Process(pid).cmdline()
        if os.name == "nt":
            return subprocess.list2cmdline(command)
        return shlex.join(command)
    except (psutil.Error, OSError):
        return ""


def _processes_by_file(paths):
    result = {path: set() for path in paths}
    lookup = {os.path.normcase(os.path.realpath(path)): path for path in paths}
    if not lookup:
        return result

    for process in psutil.process_iter(["pid"]):
        pid = process.info["pid"]
        if pid == os.getpid():
            continue
        try:
            opened_files = process.open_files()
        except (psutil.Error, OSError):
            opened_files = []

        def record_matches(files):
            matched = False
            for opened_file in files:
                path = getattr(opened_file, "path", None)
                if path is None:
                    continue
                requested_path = lookup.get(os.path.normcase(os.path.realpath(path)))
                if requested_path is not None:
                    result[requested_path].add(pid)
                    matched = True
            return matched

        found_shm = record_matches(opened_files)
        if os.name == "nt" and not found_shm:
            try:
                record_matches(process.memory_maps())
            except (psutil.Error, OSError, NotImplementedError):
                continue
    return result


def _format_size(size):
    if size >= 1024 * 1024:
        return f"{size / (1024 * 1024):.2f} MiB"
    if size >= 1024:
        return f"{size / 1024:.1f} KiB"
    return f"{size} B"


def _process_name(command):
    try:
        parts = shlex.split(command, posix=os.name != "nt")
    except ValueError:
        parts = command.split()
    if os.name == "nt":
        parts = [part.strip('"') for part in parts]
    if not parts:
        return "unknown process"

    label = None
    for index, part in enumerate(parts):
        if part in ("--label", "--name") and index + 1 < len(parts):
            label = parts[index + 1]
        elif part.startswith("--label="):
            label = part.split("=", 1)[1]

    executable = os.path.basename(parts[0])
    if executable.lower().startswith(("python", "pypy")):
        if "-m" in parts:
            module_index = parts.index("-m") + 1
            name = parts[module_index] if module_index < len(parts) else executable
        elif "-c" in parts:
            name = f"{executable} -c"
        else:
            script = next((part for part in parts[1:] if not part.startswith("-")), None)
            name = os.path.basename(script) if script else executable
    else:
        name = executable
    return f"{label} ({name})" if label else name


class PipelineModel:
    """Keep SHM handles open and sample their frame counters over time."""

    def __init__(self, directory=SHM_DIR):
        self.directory = directory
        self.handles = {}
        self.samples = {}
        self.process_commands = {}

    def scan(self):
        now = time.monotonic()
        paths = sorted(glob.glob(os.path.join(self.directory, "*.im.shm")))
        pending = []
        for path in paths:
            if path not in self.handles:
                try:
                    self.handles[path] = daoShm.shm(path)
                except (OSError, ValueError, RuntimeError):
                    continue
            handle = self.handles[path]
            try:
                metadata = handle.image.md.contents
                shape = tuple(int(metadata.size[i]) for i in range(metadata.naxis))
                dtype = np.dtype(daoShm.daoType2NpType(metadata.atype))
                frames = int(metadata.cnt0)
                previous = self.samples.get(path)
                rate = None
                if previous is not None and now > previous[1]:
                    rate = max(0, frames - previous[0]) / (now - previous[1])
                self.samples[path] = (frames, now)

                semaphore_users = handle.sem_users()
                registered_readers = set()
                for marker_path in glob.glob(path + ".readerpid.*"):
                    try:
                        registered_readers.add(int(marker_path.rsplit(".", 1)[-1]))
                    except ValueError:
                        continue
                writer_pid = None
                try:
                    with open(path + ".writerpid") as writer_file:
                        candidate = int(writer_file.read().strip())
                except (OSError, ValueError):
                    candidate = None

                frame_bytes = int(metadata.nelement) * dtype.itemsize
                fifo_size = max(1, int(metadata.fifo_size))
                pending.append({
                    "shm": handle,
                    "name": os.path.basename(path)[:-len(".im.shm")],
                    "path": path,
                    "shape": shape,
                    "dtype": dtype.name,
                    "itemsize": dtype.itemsize,
                    "frame_bytes": frame_bytes,
                    "total_bytes": frame_bytes * fifo_size,
                    "fifo_size": fifo_size,
                    "frames": frames,
                    "rate": rate,
                    "semaphore_users": semaphore_users,
                    "registered_readers": registered_readers,
                    "writer_pid": candidate,
                    "semaphores_in_use": len(semaphore_users),
                    "semaphores_total": int(metadata.sem),
                })
            except (OSError, ValueError, RuntimeError, AttributeError):
                continue

        pids_by_file = _processes_by_file([item["path"] for item in pending])

        active_pids = set()
        for item in pending:
            attached = set(pids_by_file.get(item["path"], ()))
            readers = {pid for pids in item["semaphore_users"].values() for pid in pids}
            readers.update(item["registered_readers"])
            readers.intersection_update(attached)
            attached.update(readers)
            attached.discard(os.getpid())
            writer_pid = item.pop("writer_pid")
            if writer_pid in attached:
                item["writer_pid"] = writer_pid
            else:
                item["writer_pid"] = None
            item["readers"] = len(readers)
            item["processes"] = [
                {"pid": pid, "reader": pid in readers, "writer": pid == item["writer_pid"]}
                for pid in sorted(attached)
            ]
            active_pids.update(attached)
            item.pop("semaphore_users")
            item.pop("registered_readers")
            item.pop("shm")

        commands = {pid: command for pid, command in self.process_commands.items() if pid in active_pids}
        commands.update({pid: process_command(pid) for pid in active_pids - commands.keys()})
        self.process_commands = {
            pid: commands.get(pid) or "process unavailable" for pid in active_pids
        }
        records = []
        for item in pending:
            visible_processes = []
            for process in item["processes"]:
                process["command"] = self.process_commands[process["pid"]]
                if "daoShmPipeline.py" not in process["command"]:
                    visible_processes.append(process)
            item["processes"] = visible_processes
            if visible_processes:
                records.append(item)

        for path in set(self.handles) - set(paths):
            self.handles.pop(path).close()
            self.samples.pop(path, None)
        return records

    def close(self):
        for handle in self.handles.values():
            handle.close()
        self.handles.clear()


class PipelineCanvas(QWidget):
    NODE_WIDTH = 270
    NODE_GAP_X = 100
    NODE_GAP_Y = 42
    MARGIN = 28

    def __init__(self):
        super().__init__()
        self.records = []
        self.nodes = {}
        self.edges = []
        self.positions = {}
        self.rows = {}
        self.setMouseTracking(True)

    def set_records(self, records, available_width=None):
        self.records = records
        self.nodes, self.edges = _pipeline_graph(records)
        self.relayout(available_width)

    def relayout(self, available_width=None):
        if available_width is None or available_width <= 0:
            available_width = self.window().width() - 80
        self.positions, self.rows, width, height = _layout_pipeline_graph(
            self.nodes, self.edges, available_width
        )
        self.setMinimumSize(width, height)
        self.resize(width, height)
        self.update()

    def mouseMoveEvent(self, event):
        for node_id, node in self.nodes.items():
            x, y, width, height = self.positions[node_id]
            if x <= event.x() <= x + width and y <= event.y() <= y + height:
                self.setCursor(QCursor(Qt.PointingHandCursor))
                tooltip = node["command"] if node["kind"] == "process" else node["path"]
                self.setToolTip(tooltip)
                return
        self.setCursor(QCursor(Qt.ArrowCursor))
        self.setToolTip("")
        super().mouseMoveEvent(event)

    def mousePressEvent(self, event):
        if event.button() == Qt.LeftButton:
            for node_id, node in self.nodes.items():
                if QRect(*self.positions[node_id]).contains(event.pos()):
                    self._show_node_details(node_id, node)
                    event.accept()
                    return
        super().mousePressEvent(event)

    def _show_node_details(self, node_id, node):
        dialog = QDialog(self)
        if node["kind"] == "process":
            dialog.setWindowTitle(f"Process: {node['display_name']} (PID {node['pid']})")
            role = " + ".join(role.title() for role in ("writer", "reader", "attached")
                               if role in node["roles"])
            lines = [
                f"Process: {node['display_name']}",
                f"PID: {node['pid']}",
                f"Role: {role}",
                "",
                "Command:",
                f"  {node['command']}",
                "",
                "Shared-memory connections:",
            ]
            for source, target, kind in self.edges:
                if source == node_id and target[0] == "shm":
                    lines.append(f"  Writes to: {self.nodes[target]['path']}")
                elif target == node_id and source[0] == "shm":
                    label = "Reads from" if kind == "read" else "Attached to"
                    lines.append(f"  {label}: {self.nodes[source]['path']}")
        else:
            dialog.setWindowTitle(f"Shared memory: {node['name']}")
            rate = "warming up" if node["rate"] is None else f"{node['rate']:.2f} Hz"
            lines = [
                f"Name: {node['name']}",
                f"Path: {node['path']}",
                f"Data type: {node['dtype']} ({node['itemsize']} bytes per item)",
                f"Shape: {node['shape']}",
                f"Frame size: {_format_size(node['frame_bytes'])}",
                f"Allocated data: {_format_size(node['total_bytes'])}",
                f"FIFO depth: {node['fifo_size']}",
                f"Frames published: {node['frames']}",
                f"Update frequency: {rate}",
                f"Readers: {node['readers']}",
                f"Semaphores in use: {node['semaphores_in_use']}/{node['semaphores_total']}",
                "",
                "Connected processes:",
            ]
            for process in sorted(node["processes"], key=lambda item: item["pid"]):
                roles = []
                if process["writer"]:
                    roles.append("writer")
                if process["reader"]:
                    roles.append("reader")
                if not roles:
                    roles.append("attached")
                lines.append(
                    f"  PID {process['pid']} [{', '.join(roles)}] "
                    f"{_process_name(process['command'])}"
                )
                lines.append(f"    {process['command']}")

        layout = QVBoxLayout(dialog)
        details = QPlainTextEdit()
        details.setReadOnly(True)
        details.setLineWrapMode(QPlainTextEdit.NoWrap)
        details.setFont(QFont("Menlo", 10))
        details.setPlainText("\n".join(lines))
        buttons = QDialogButtonBox(QDialogButtonBox.Close)
        buttons.rejected.connect(dialog.reject)
        buttons.accepted.connect(dialog.accept)
        layout.addWidget(details)
        layout.addWidget(buttons)
        dialog.resize(720, 440)
        dialog.exec_()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        painter.fillRect(self.rect(), QColor("#171c20"))
        if not self.nodes:
            painter.setPen(QColor("#9aa7ad"))
            painter.drawText(self.rect(), Qt.AlignCenter, "No connected SHM pipeline found")
            return

        for source, target, kind in self.edges:
            self._draw_edge(painter, source, target, kind)
        for node_id, node in self.nodes.items():
            self._draw_node(painter, node_id, node, self.positions[node_id])
        painter.end()

    def _draw_edge(self, painter, source, target, kind):
        source_rect = self.positions[source]
        target_rect = self.positions[target]
        source_x, source_y, source_width, source_height = source_rect
        target_x, target_y, target_width, target_height = target_rect
        if self.rows[source] < self.rows[target]:
            start = (source_x + source_width // 2, source_y + source_height)
            end = (target_x + target_width // 2, target_y)
        elif self.rows[source] > self.rows[target]:
            start = (source_x + source_width // 2, source_y)
            end = (target_x + target_width // 2, target_y + target_height)
        else:
            forward = source_x < target_x
            start = (source_x + source_width, source_y + source_height // 2) if forward else (
                source_x, source_y + source_height // 2
            )
            end = (target_x, target_y + target_height // 2) if forward else (
                target_x + target_width, target_y + target_height // 2
            )
        color = QColor("#43a99a" if kind == "write" else "#d58b45" if kind == "read" else "#607d8b")
        style = Qt.DashLine if kind == "attached" else Qt.SolidLine
        painter.setPen(QPen(color, 2 if kind != "attached" else 1, style))
        painter.drawLine(*start, *end)
        if kind != "attached":
            angle = math.atan2(end[1] - start[1], end[0] - start[0])
            back_x = end[0] - math.cos(angle) * 10
            back_y = end[1] - math.sin(angle) * 10
            offset_x = math.sin(angle) * 4
            offset_y = math.cos(angle) * 4
            painter.drawLine(int(end[0]), int(end[1]), int(back_x + offset_x), int(back_y - offset_y))
            painter.drawLine(int(end[0]), int(end[1]), int(back_x - offset_x), int(back_y + offset_y))

    @staticmethod
    def _draw_node(painter, node_id, node, rect):
        x, y, width, height = rect
        if node["kind"] == "shm":
            painter.setPen(QPen(QColor("#3a9b8d"), 2))
            painter.setBrush(QColor("#202c2c"))
            painter.drawRoundedRect(x, y, width, height, 5, 5)
            painter.setPen(QColor("#e2ece9"))
            painter.setFont(QFont("Menlo", 10, QFont.Bold))
            painter.drawText(x + 12, y + 21, node["name"])
            painter.setFont(QFont("Menlo", 8))
            painter.setPen(QColor("#b6c4c2"))
            painter.drawText(x + 12, y + 40, f"{node['dtype']} {node['shape']}  |  {node['itemsize']} B/item")
            rate = "warming up" if node["rate"] is None else f"{node['rate']:.1f} Hz"
            painter.drawText(x + 12, y + 59, f"frame {node['frames']}  |  {rate}")
            painter.drawText(x + 12, y + 78,
                             f"{_format_size(node['frame_bytes'])}/frame  |  "
                             f"{_format_size(node['total_bytes'])} allocated")
            painter.drawText(x + 12, y + 97,
                             f"{node['readers']} readers  |  "
                             f"{node['semaphores_in_use']}/{node['semaphores_total']} sems in use")
            return

        roles = node["roles"]
        is_writer = "writer" in roles
        is_reader = "reader" in roles
        color = QColor("#3a9b8d" if is_writer and not is_reader else
                       "#6c9b8e" if is_writer and is_reader else
                       "#d58b45" if is_reader else "#607d8b")
        painter.setPen(QPen(color, 2))
        painter.setBrush(QColor("#292d2f"))
        painter.drawRoundedRect(x, y, width, height, 5, 5)
        if is_writer and is_reader:
            label = "RELAY  READ + WRITE"
        elif is_writer:
            label = "WRITER"
        elif is_reader:
            label = "READER"
        else:
            label = "ATTACHED"
        painter.setPen(QColor("#e2e6e7"))
        painter.setFont(QFont("Menlo", 9, QFont.Bold))
        painter.drawText(x + 12, y + 23, f"{label}  PID {node['pid']}")
        painter.setFont(QFont("Menlo", 9, QFont.Bold))
        painter.setPen(QColor("#e2e6e7"))
        painter.drawText(x + 12, y + 45, node["display_name"][:30])
        painter.setFont(QFont("Menlo", 7))
        painter.setPen(QColor("#b6c4c2"))
        painter.drawText(x + 12, y + 64, node["command"][:38])


def _pipeline_graph(records):
    nodes = {}
    edges = set()
    for record in records:
        shm_id = ("shm", record["path"])
        nodes[shm_id] = {"kind": "shm", **record}
        for process in record["processes"]:
            process_id = ("process", process["pid"])
            node = nodes.setdefault(process_id, {
                "kind": "process",
                "pid": process["pid"],
                "command": process["command"],
                "display_name": _process_name(process["command"]),
                "roles": set(),
            })
            is_writer = process["writer"]
            is_reader = process["reader"]
            if is_writer:
                node["roles"].add("writer")
                edges.add((process_id, shm_id, "write"))
            if is_reader:
                node["roles"].add("reader")
                edges.add((shm_id, process_id, "read"))
            if not is_writer and not is_reader:
                node["roles"].add("attached")
                edges.add((shm_id, process_id, "attached"))
    return nodes, sorted(edges, key=lambda edge: (str(edge[0]), str(edge[1]), edge[2]))


def _layout_pipeline_graph(nodes, edges, available_width=1000):
    def sort_key(node_id):
        node = nodes[node_id]
        return (node["kind"], str(node.get("name", node.get("pid", ""))))

    directed = [(source, target) for source, target, kind in edges if kind != "attached"]
    indegree = {node_id: 0 for node_id in nodes}
    children = {node_id: [] for node_id in nodes}
    for source, target in directed:
        indegree[target] += 1
        children[source].append(target)
    queue = deque(sorted((node_id for node_id, degree in indegree.items() if degree == 0), key=sort_key))
    layers = {node_id: 0 for node_id in nodes}
    visited = set()
    while queue:
        node_id = queue.popleft()
        visited.add(node_id)
        for child in children[node_id]:
            layers[child] = max(layers[child], layers[node_id] + 1)
            indegree[child] -= 1
            if indegree[child] == 0:
                queue.append(child)
    fallback_layer = max(layers.values(), default=0) + 1
    for node_id in nodes.keys() - visited:
        layers[node_id] = fallback_layer
    for source, target, kind in edges:
        if kind == "attached":
            layers[target] = max(layers[target], layers[source] + 1)

    level_nodes = {}
    for node_id, layer in layers.items():
        level_nodes.setdefault(layer, []).append(node_id)

    incoming = {node_id: [] for node_id in nodes}
    outgoing = {node_id: [] for node_id in nodes}
    for source, target in directed:
        outgoing[source].append(target)
        incoming[target].append(source)

    ordered_index = {}
    for layer in sorted(level_nodes):
        def branch_order(node_id):
            neighbors = incoming[node_id] or outgoing[node_id]
            average = sum(ordered_index.get(neighbor, 0) for neighbor in neighbors) / len(neighbors) if neighbors else 0
            return average, sort_key(node_id)

        level_nodes[layer].sort(key=branch_order)
        for index, node_id in enumerate(level_nodes[layer]):
            ordered_index[node_id] = index

    available_width = max(PipelineCanvas.NODE_WIDTH + 2 * PipelineCanvas.MARGIN, available_width)
    columns = max(1, (available_width - 2 * PipelineCanvas.MARGIN + PipelineCanvas.NODE_GAP_X) //
                  (PipelineCanvas.NODE_WIDTH + PipelineCanvas.NODE_GAP_X))
    columns = int(columns)
    column_gap = PipelineCanvas.NODE_GAP_X
    if columns > 1:
        column_gap = min(
            180,
            (available_width - 2 * PipelineCanvas.MARGIN - columns * PipelineCanvas.NODE_WIDTH) // (columns - 1),
        )

    rank_order = {layer: index for index, layer in enumerate(sorted(level_nodes))}
    layers_by_row = {}
    for layer, node_ids in level_nodes.items():
        row = rank_order[layer] // columns
        layers_by_row.setdefault(row, []).append((layer, node_ids))

    positions = {}
    rows = {}
    row_y = PipelineCanvas.MARGIN
    for row, grouped_layers in sorted(layers_by_row.items()):
        stack_heights = {
            layer: sum(116 if nodes[node_id]["kind"] == "shm" else 90 for node_id in node_ids)
            + PipelineCanvas.NODE_GAP_Y * max(0, len(node_ids) - 1)
            for layer, node_ids in grouped_layers
        }
        row_content_height = max(stack_heights.values(), default=0)
        for layer, node_ids in grouped_layers:
            slot = rank_order[layer] % columns
            column = columns - 1 - slot if row % 2 else slot
            y = row_y + (row_content_height - stack_heights[layer]) // 2
            for node_id in node_ids:
                node_height = 116 if nodes[node_id]["kind"] == "shm" else 90
                positions[node_id] = (
                    PipelineCanvas.MARGIN + column * (PipelineCanvas.NODE_WIDTH + column_gap),
                    y,
                    PipelineCanvas.NODE_WIDTH,
                    node_height,
                )
                rows[node_id] = row
                y += node_height + PipelineCanvas.NODE_GAP_Y
        row_y += row_content_height + PipelineCanvas.NODE_GAP_Y

    columns_used = min(columns, len(level_nodes))
    width = (2 * PipelineCanvas.MARGIN + columns_used * PipelineCanvas.NODE_WIDTH +
             max(0, columns_used - 1) * column_gap)
    height = max((rect[1] + rect[3] for rect in positions.values()), default=0) + PipelineCanvas.MARGIN
    return positions, rows, max(width, 500), max(height, 180)


class PipelineWindow(QMainWindow):
    def __init__(self, directory=SHM_DIR):
        super().__init__()
        self.model = PipelineModel(directory)
        self.setWindowTitle("dao SHM Pipeline")
        self.resize(980, 720)

        central = QWidget()
        layout = QVBoxLayout(central)
        self.status = QLabel("Scanning /tmp for dao shared memory")
        self.status.setStyleSheet("padding: 7px 10px; color: #e0e8e7; background: #263235;")
        self.canvas = PipelineCanvas()
        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setWidget(self.canvas)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(self.status)
        layout.addWidget(self.scroll)
        self.setCentralWidget(central)
        self.setStyleSheet("QMainWindow { background: #171c20; } QScrollArea { border: 0; }")

        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(REFRESH_MS)
        self.refresh()

    def refresh(self):
        started = time.perf_counter()
        records = self.model.scan()
        elapsed_ms = (time.perf_counter() - started) * 1000
        self.canvas.set_records(records, self.scroll.viewport().width())
        process_count = len({
            process["pid"] for record in records for process in record["processes"]
        })
        self.status.setText(
            f"{len(records)} shared segments   |   {process_count} processes   "
            f"|   scan {elapsed_ms:.0f} ms   |   {time.strftime('%H:%M:%S')}"
        )

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if hasattr(self, "scroll") and self.scroll.viewport().width() > 0:
            self.canvas.relayout(self.scroll.viewport().width())

    def closeEvent(self, event):
        self.timer.stop()
        self.model.close()
        event.accept()


def show_shm_pipeline(directory=SHM_DIR):
    """Show discovered dao SHMs and connected process handles in a Qt window."""
    app = QApplication.instance() or QApplication(sys.argv[:1])
    window = PipelineWindow(directory)
    window.show()
    return app.exec_()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", default=SHM_DIR, help="directory to scan (default: /tmp)")
    args = parser.parse_args()
    return show_shm_pipeline(args.directory)


if __name__ == "__main__":
    raise SystemExit(main())