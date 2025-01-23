# /******************************************************************************
#  * Project:        Writes to shared-memory at the desired rate.
#  Author:           Thomas Davies Created:        23/01/2025
#  ******************************************************************************/
 
from time import perf_counter, clock_gettime, CLOCK_REALTIME, sleep
import daoShm
import numpy as np
import sys

# ========================== #

# python throughput.py <nbytes> <period (ms)>
nbytes = int(sys.argv[1])
period = int(sys.argv[2]) / 1000 # convert to seconds.

# ========================== #

ts = daoShm.shm('ts.im.shm', data=np.zeros((1,1)).astype(np.float32)) # Done by John's code.

def stamp_time():
    t = clock_gettime(CLOCK_REALTIME)
    ts.set_data(np.array([t]))

# ========================== #

shm = daoShm.shm('test.im.shm', data=np.zeros((nbytes,1)).astype(np.uint8))

def update_data():
    data = np.random.random((nbytes,1))
    shm.set_data(data)

# ========================== #

# @TODO: Maybe make this optional, and we can write a C++
# latency estimator too, as John will be using dao C++
# and this may have different latency as it doesn't go through
# python bindings etc?

print("Estimating write latency..")
n_writes = 10
t0 = perf_counter()
for k in range(n_writes):
    update_data()
t1 = perf_counter()

total_write_time = t1 - t0
avg_write_time = total_write_time / n_writes
print(f"Estimated Latency: {avg_write_time*1000}ms")

run_limited = avg_write_time < period
if not run_limited:
    print(f"Warning: Writing to shared-memory takes {1000*(avg_write_time-period)}ms longer \nthan your desired time between writes.")
    print(f"Write spacing of {period*1000}ms cannot be achieved.")
    print(f"Proceeding to write flat-out to get as close to the target rate as possible.")

# ========================== #

if not run_limited:
    while True:
        update_data()
else:
    t0 = perf_counter()
    wait_time = period - avg_write_time
    while True:
        now = perf_counter()
        if now - t0 > wait_time:
            update_data()
            t0 = perf_counter() # time when data has been written to shm.