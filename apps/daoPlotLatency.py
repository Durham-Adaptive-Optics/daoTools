#!/usr/bin/env python3

import numpy as np
import sys
import dao
import matplotlib.pyplot as plt
from tqdm import tqdm 

nData = int(sys.argv[1])

latencyShm = dao.shm(sys.argv[2])
latency = np.zeros(nData)

# Use tqdm for the progress bar
for k in tqdm(range(nData), desc="Reading Latency"):
    latency[k] = latencyShm.get_data(check=True, semNb=5)[0, 0]

# Calculate statistics
mean_latency = np.mean(latency)
std_latency = np.std(latency)

plt.ion()
# --- Histogram Plot ---
plt.figure(figsize=(10, 6))
n, bins, patches = plt.hist(latency, bins=1000, edgecolor='black', alpha=0.7)

# Add average and std lines
plt.axvline(mean_latency, color='red', linestyle='dashed', linewidth=2, label=f'Mean = {mean_latency:.2f} us')
plt.axvline(mean_latency + std_latency, color='green', linestyle='dotted', linewidth=2, label=f'+1 STD = {mean_latency + std_latency:.2f} us')
plt.axvline(mean_latency - std_latency, color='green', linestyle='dotted', linewidth=2, label=f'-1 STD = {mean_latency - std_latency:.2f} us')

# Add jitter text
plt.text(0.95, 0.95, f'Jitter = {std_latency:.2f} us', 
         horizontalalignment='right', verticalalignment='top', 
         transform=plt.gca().transAxes, 
         fontsize=12, bbox=dict(facecolor='white', alpha=0.7, edgecolor='black'))

# Titles and labels
plt.title(f'Latency Histogram for {nData} frames')
plt.xlabel('Latency (us)')
plt.ylabel('Frequency')
plt.legend()
plt.grid(True, linestyle='--', alpha=0.6)

# --- Time Series Scatter Plot ---
plt.figure(figsize=(12, 6))
plt.plot(latency, marker='o', linestyle='None', markersize=2, alpha=0.7)

plt.axhline(mean_latency, color='red', linestyle='dashed', linewidth=2, label=f'Mean = {mean_latency:.2f} us')
plt.axhline(mean_latency + std_latency, color='green', linestyle='dotted', linewidth=2, label=f'+1 STD = {mean_latency + std_latency:.2f} us')
plt.axhline(mean_latency - std_latency, color='green', linestyle='dotted', linewidth=2, label=f'-1 STD = {mean_latency - std_latency:.2f} us')

plt.title(f'Latency Over Time for {nData} frames')
plt.xlabel('Frame Index')
plt.ylabel('Latency (us)')
plt.legend()
plt.grid(True, linestyle='--', alpha=0.6)

