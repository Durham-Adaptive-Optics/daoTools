#!/usr/bin/env python3

'''
 # @ Author: Thomas N. Davies
 # @ Company: Centre for Advanced Instrumentation, Durham University
 # @ Contact: thomas.n.davies@durham.ac.uk
 # @ Create Time: 2026-02-09 08:15:03
 # @ Description: Monitors and prints update rate of a DAO shared memory.
'''

import argparse as ap
import time
import dao

def busy_wait(delay: float):
    if delay <= 0.0:
        return
    
    t0 = time.perf_counter()
    while (time.perf_counter() - t0) < delay:
        pass

def log(freq: float):
    print(f"{args.SHM} @ {freq:.1f} Hz", end="\r")

if __name__ == "__main__":
    app = ap.ArgumentParser()
    app.add_argument("SHM", help="Dao Shared Memory to Monitor")
    app.add_argument("--interval, -i", dest="interval", default=1.5, type=float, help="Time duration to average shm updates over")
    args = app.parse_args()
    
    shm = dao.shm(args.SHM)
    
    log(0.0)
    try:
        initialCnt = None    
        while 1:
            initialCnt = shm.get_counter()
            busy_wait(args.interval)
            shmUpdateFrequency = (shm.get_counter() - initialCnt) / args.interval
            log(shmUpdateFrequency)
    except:
        print("")
