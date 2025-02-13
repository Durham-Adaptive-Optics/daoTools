# /******************************************************************************
#  * Project:        Measured the rate at which shared-memory is updated.
#  Author:           Thomas Davies Created:        23/01/2025
#  ******************************************************************************/

from time import perf_counter
import daoShm
import sys

# ========================== #

# python shm_rate.py <shm>
shm_name = sys.argv[1]

# ========================== #

shm = daoShm.shm(shm_name)

# ========================== #

sample_period = 10.0 # seconds.
frames = 0
t0 = perf_counter()
fid = shm.get_counter()
while True:
    fid_now = shm.get_counter()
    if fid_now > fid:
        fid = fid_now
        frames += 1
    
    if perf_counter() - t0 >= sample_period:
        avg_frame_rate = frames / sample_period
        avg_frame_spacing = sample_period / frames if frames > 0 else '-'
        print(f'Updating @ {avg_frame_rate}hz ({avg_frame_spacing*1000} ms)')
        t0 = perf_counter()
        frames = 0