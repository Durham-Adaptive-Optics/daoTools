#!/usr/bin/env python3

"""
# @ Author: Thomas N. Davies
# @ Company: Centre for Advanced Instrumentation, Durham University
# @ Contact: thomas.n.davies@durham.ac.uk
# @ Create Time: 2026-01-12 05:05:37
# @ Description: Client script to communicate and instruct a daoTelemetry service. 
"""

from daoCommandIfce import daoCommandIfce as df
from daoLog import daoLog
import argparse as ap
import time

def start_recording(interface):
    interface.Exec("Init")
    interface.Exec("Enable")
    interface.Exec("Run")
    time.sleep(1)

    status, state = interface.State(None)
    setupGood = (status == 0) and (state == "Running")
    return not setupGood

def stop_recording(interface):
    interface.Exec("Idle")
    interface.Exec("Disable")
    interface.Exec("Stop")
    time.sleep(1)

    status, state = interface.State(None)
    setupGood = (status == 0) and (state == "Off")
    return not setupGood

if __name__ == "__main__":
    daoLog("daoTelemetryClient")

    app = ap.ArgumentParser(prog="daoTelemetryClient", description="Client to interact with a daoTelemetry service")
    app.add_argument("PORT", type=int, help="daoTelemetry service port")
    app.add_argument(
        "--start", dest="start", action="store_true", help="Start recording session"
    )
    app.add_argument(
        "--stop", dest="stop", action="store_true", help="Stop recording session"
    )
    args = app.parse_args()

    interface = None
    try:
        interface = df("127.0.0.1", args.PORT)
    except Exception as e:
        print(f"failed to create interface to daoTelemetry: {e}")
        exit(1)

    error = False
    if args.start:
        error = start_recording(interface)
    elif args.stop:
        error = stop_recording(interface)

    exit(1 if error else 0)
