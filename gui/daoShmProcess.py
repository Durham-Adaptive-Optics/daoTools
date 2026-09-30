#!/usr/bin/env python3
"""Monitor only processes currently connected to local dao SHM segments.

Run in the terminal by default, add --qt for a GUI table, or --once for a single
terminal snapshot.
"""
import argparse
import shutil
import sys
import time

import psutil
from daoShmPipeline import PipelineModel, SHM_DIR, _process_name


class ShmProcessMonitor:
    def __init__(self, directory=SHM_DIR):
        self.model = PipelineModel(directory)
        self.scan_ms = 0.0
        self.processes = {}

    def scan(self):
        started = time.perf_counter()
        records = self.model.scan()
        processes = {}
        for record in records:
            for process in record["processes"]:
                entry = processes.setdefault(process["pid"], {
                    "pid": process["pid"],
                    "name": _process_name(process["command"]),
                    "command": process["command"],
                    "roles": set(),
                    "connections": {},
                })
                if process["writer"]:
                    entry["roles"].add("writer")
                    entry["connections"][record["name"]] = "write"
                if process["reader"]:
                    entry["roles"].add("reader")
                    current = entry["connections"].get(record["name"])
                    entry["connections"][record["name"]] = "read/write" if current == "write" else "read"
                if not process["writer"] and not process["reader"]:
                    entry["roles"].add("attached")
                    entry["connections"].setdefault(record["name"], "attached")

        stats = self._process_stats(processes)
        rows = []
        for pid, entry in processes.items():
            entry.update(stats.get(pid, {}))
            entry["role"] = "+".join(role for role in ("writer", "reader", "attached")
                                      if role in entry["roles"]) or "attached"
            entry["shms"] = sorted(entry["connections"])
            rows.append(entry)
        rows.sort(key=lambda row: (row.get("cpu", 0.0), row["pid"]), reverse=True)
        self.scan_ms = (time.perf_counter() - started) * 1000
        return rows, len(records)

    def _process_stats(self, processes):
        stats = {}
        current_processes = {}
        for pid in processes:
            process = self.processes.get(pid)
            if process is None:
                try:
                    process = psutil.Process(pid)
                except psutil.Error:
                    continue
            try:
                cpu = process.cpu_percent(interval=None)
                stats[pid] = {
                    "cpu": cpu,
                    "cores": cpu / 100.0,
                    "thread_count": process.num_threads(),
                    "memory_percent": process.memory_percent(),
                    "rss_mb": process.memory_info().rss / (1024 * 1024),
                    "state": process.status(),
                }
                current_processes[pid] = process
            except psutil.Error:
                continue
        self.processes = current_processes
        return stats

    def close(self):
        self.model.close()


def _terminal_table(rows, shm_count):
    terminal_width = shutil.get_terminal_size(fallback=(100, 24)).columns
    fixed_widths = (7, 6, 7, 5, 6, 9, 13, 6)
    process_width = max(16, terminal_width - sum(fixed_widths) - len(fixed_widths))
    widths = (*fixed_widths, process_width)
    headers = ("PID", "CPU%", "CORES", "THR", "MEM%", "RSS MiB", "ROLE", "SHMs", "PROCESS")
    line_format = " ".join(f"{{:<{width}}}" for width in widths)
    output = [line_format.format(*headers)]
    output.append("-" * min(terminal_width, sum(widths) + len(widths) - 1))
    for row in rows:
        values = (
            str(row["pid"]),
            f"{row.get('cpu', 0.0):.1f}",
            f"{row.get('cores', 0.0):.2f}",
            str(row.get("thread_count", 0)),
            f"{row.get('memory_percent', 0.0):.1f}",
            f"{row.get('rss_mb', 0.0):.1f}",
            row["role"],
            str(len(row["shms"])),
            row["name"],
        )
        output.append(line_format.format(*(value[:width] for value, width in zip(values, widths))))
    if not rows:
        output.append("No connected SHM processes")
    return output, shm_count


def run_terminal(monitor, interval, once):
    try:
        while True:
            rows, shm_count = monitor.scan()
            output, _ = _terminal_table(rows, shm_count)
            if not once and sys.stdout.isatty():
                print("\033[2J\033[H", end="")
            print(f"dao SHM processes | {len(rows)} PIDs | {shm_count} connected SHMs | "
                  f"scan {monitor.scan_ms:.0f} ms | {time.strftime('%H:%M:%S')}")
            print("\n".join(output), flush=True)
            if once:
                break
            time.sleep(interval)
    except KeyboardInterrupt:
        pass
    finally:
        monitor.close()


def run_qt(monitor, interval):
    from PyQt5.QtCore import Qt, QTimer
    from PyQt5.QtWidgets import (
        QApplication,
        QHeaderView,
        QLabel,
        QMainWindow,
        QTableWidget,
        QTableWidgetItem,
        QVBoxLayout,
        QWidget,
    )

    class ProcessWindow(QMainWindow):
        headers = ("PID", "Process", "CPU %", "CPU cores", "Threads", "MEM %", "RSS MiB", "State", "Role", "SHM connections", "Command")

        def __init__(self):
            super().__init__()
            self.setWindowTitle("dao SHM Processes")
            self.resize(1200, 680)
            central = QWidget()
            layout = QVBoxLayout(central)
            self.status = QLabel("Scanning connected SHM processes")
            self.table = QTableWidget(0, len(self.headers))
            self.table.setHorizontalHeaderLabels(self.headers)
            self.table.setEditTriggers(QTableWidget.NoEditTriggers)
            self.table.setSelectionBehavior(QTableWidget.SelectRows)
            self.table.setAlternatingRowColors(True)
            self.table.verticalHeader().setVisible(False)
            self.table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents)
            self.table.horizontalHeader().setSectionResizeMode(len(self.headers) - 1, QHeaderView.Stretch)
            layout.addWidget(self.status)
            layout.addWidget(self.table)
            self.setCentralWidget(central)

            self.timer = QTimer(self)
            self.timer.timeout.connect(self.refresh)
            self.timer.start(max(250, int(interval * 1000)))
            self.refresh()

        def refresh(self):
            rows, shm_count = monitor.scan()
            self.table.setRowCount(len(rows))
            for row_index, row in enumerate(rows):
                values = (
                    str(row["pid"]),
                    row["name"],
                    f"{row.get('cpu', 0.0):.1f}",
                    f"{row.get('cores', 0.0):.2f}",
                    str(row.get("thread_count", 0)),
                    f"{row.get('memory_percent', 0.0):.1f}",
                    f"{row.get('rss_mb', 0.0):.1f}",
                    row.get("state", "?"),
                    row["role"],
                    ", ".join(f"{name} ({mode})" for name, mode in sorted(row["connections"].items())),
                    row["command"],
                )
                for column, value in enumerate(values):
                    item = QTableWidgetItem(value)
                    item.setFlags(item.flags() & ~Qt.ItemIsEditable)
                    self.table.setItem(row_index, column, item)
            self.status.setText(
                f"{len(rows)} connected PIDs  |  {shm_count} active SHMs  |  "
                f"scan {monitor.scan_ms:.0f} ms  |  updated {time.strftime('%H:%M:%S')}"
            )

        def closeEvent(self, event):
            self.timer.stop()
            monitor.close()
            event.accept()

    app = QApplication.instance() or QApplication(sys.argv[:1])
    window = ProcessWindow()
    window.show()
    return app.exec_()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt", action="store_true", help="show the process table in a Qt window")
    parser.add_argument("--once", action="store_true", help="print one terminal snapshot and exit")
    parser.add_argument("--interval", type=float, default=1.0, help="refresh period in seconds")
    parser.add_argument("--directory", default=SHM_DIR, help="SHM directory to scan")
    args = parser.parse_args()
    if args.interval <= 0:
        parser.error("--interval must be greater than zero")

    monitor = ShmProcessMonitor(args.directory)
    if args.qt:
        return run_qt(monitor, args.interval)
    run_terminal(monitor, args.interval, args.once)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())