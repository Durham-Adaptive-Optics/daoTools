#!/usr/bin/env python3
"""
daoTurbulenceVonKarmanSimulator.py -- a simple turbulence simulator on a DM.

Writes a frozen-flow von Karman phase screen, sampled at the DM actuators, into a
DM command channel SHM (one value per actuator of the map, row-major over the map):
summed with the other channels by daoShmCombiner, the wavefront sensor sees it as
turbulence and the loop corrects it on its own channel.

The screen is periodic, NPIX x NPIX samples at a quarter of the actuator pitch
(NPIX/4 pitches across: about 6 D for an 11 x 11 DM), moving with the wind at
wall-clock speed: the rate (--rate) only sets how often the channel is written, not
how fast the turbulence evolves. Outer scale L0, piston removed.

Amplitude: physical by default. The phase at 500 nm (r0) becomes a DM surface of
half the optical path (reflection), divided by --stroke, the metres of surface of a
command of 1 (e.g. 1e-5 for an Alpao DM97-15 driven in normalised units). Or --rms
gives the RMS command directly, whatever the r0. Then x --gain, and clipped to +-cap.

On Ctrl-C / SIGTERM the channel is set back to zero.

Compared with daoTurbulenceSimulator.py (Kolmogorov screen of one sample per
actuator, periodic over D, stepped by dt, a new screen every period): an outer
scale, a screen several D long, sub-actuator sampling, wall-clock evolution,
normalised-command amplitude (stroke or RMS) and a clip.

    daoTurbulenceVonKarmanSimulator.py --shm /tmp/dmCmd04.im.shm --map /tmp/dmMap.im.shm --diameter 4
    daoTurbulenceVonKarmanSimulator.py --shm ... --map ... --diameter 4 --r0 0.05 --wind 10 --direction 45
    daoTurbulenceVonKarmanSimulator.py --shm ... --map ... --diameter 4 --rms 0.05
"""
import argparse
import signal
import sys
import time

import dao
import numpy as np

NPIX = 256              # screen samples across (periodic)
OVERSAMPLE = 4          # screen samples per actuator pitch
LAMBDA = 500e-9         # r0's wavelength [m]


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--shm', required=True, help='DM command channel SHM written (one value per actuator)')
    p.add_argument('--map', required=True, help='DM actuator map SHM (2-D, nonzero = actuator)')
    p.add_argument('--diameter', type=float, required=True, help='pupil diameter across the DM [m]')
    p.add_argument('--r0', type=float, default=0.1, help='Fried parameter at 500 nm [m] (default 0.1)')
    p.add_argument('--L0', type=float, default=25.0, help='outer scale [m] (default 25)')
    p.add_argument('--wind', type=float, default=5.0, help='wind speed [m/s] (default 5)')
    p.add_argument('--direction', type=float, default=0.0, help='wind direction [deg], 0 = +x (default 0)')
    p.add_argument('--rate', type=float, default=100.0, help='channel updates per second (default 100)')
    p.add_argument('--stroke', type=float, default=1e-5,
                   help='DM surface [m] of a command of 1 (default 1e-5)')
    p.add_argument('--rms', type=float, default=0.0,
                   help='RMS command instead of the physical amplitude (0: physical, default)')
    p.add_argument('--gain', type=float, default=1.0, help='amplitude factor (default 1)')
    p.add_argument('--cap', type=float, default=0.5, help='clip the channel to +-cap (default 0.5; 0: no clip)')
    p.add_argument('--seed', type=int, default=None, help='random seed of the screen')
    return p.parse_args()


def von_karman_screen(n, dx, r0, L0, rng):
    """periodic phase screen [rad at 500 nm], n x n samples of dx metres"""
    fx = np.fft.fftfreq(n, dx)
    fxx, fyy = np.meshgrid(fx, fx)
    f2 = fxx ** 2 + fyy ** 2 + 1.0 / L0 ** 2
    psd = 0.023 * r0 ** (-5.0 / 3.0) * f2 ** (-11.0 / 6.0)     # phase PSD [rad^2 m^2]
    psd[0, 0] = 0.0                                              # no piston
    df = 1.0 / (n * dx)
    noise = rng.standard_normal((n, n)) + 1j * rng.standard_normal((n, n))
    return np.real(np.fft.ifft2(noise * np.sqrt(psd) * df)) * n * n


def sample(screen, x, y):
    """bilinear interpolation of the periodic screen at (x, y) in samples"""
    n = screen.shape[0]
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    x0, y0 = x0 % n, y0 % n
    x1, y1 = (x0 + 1) % n, (y0 + 1) % n
    return ((1 - fx) * (1 - fy) * screen[y0, x0] + fx * (1 - fy) * screen[y0, x1]
            + (1 - fx) * fy * screen[y1, x0] + fx * fy * screen[y1, x1])


def main():
    a = parse_args()
    shm = dao.shm(a.shm)
    dm_map = np.asarray(dao.shm(a.map).get_data()) != 0
    n_act = int(dm_map.sum())
    if shm.get_data().size != n_act:
        raise SystemExit(f"{a.shm} has {shm.get_data().size} values, {a.map} {n_act} actuators")

    # actuator positions in screen samples (row-major over the map, as the DM channels)
    n_across = dm_map.shape[0]
    pitch = a.diameter / (n_across - 1)
    dx = pitch / OVERSAMPLE
    iy, ix = np.nonzero(dm_map)
    ax, ay = ix * OVERSAMPLE + 0.0, iy * OVERSAMPLE + 0.0

    rng = np.random.default_rng(a.seed)
    screen = von_karman_screen(NPIX, dx, a.r0, a.L0, rng)
    to_cmd = LAMBDA / (2 * np.pi) / 2.0 / a.stroke             # rad -> OPD -> surface -> command
    theta = np.deg2rad(a.direction)
    vx, vy = a.wind * np.cos(theta) / dx, a.wind * np.sin(theta) / dx   # samples per second

    zero = np.zeros((n_act, 1), np.float32)
    stop = []
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    print(f"turbulence on {a.shm}: D {a.diameter} m, r0 {a.r0} m, L0 {a.L0} m, wind {a.wind} m/s "
          f"at {a.direction} deg, {a.rate} Hz, screen {NPIX * dx:.1f} m across, "
          + (f"RMS command {a.rms}" if a.rms > 0 else f"stroke {a.stroke} m per command")
          + f", gain {a.gain}, cap {a.cap}")

    t0 = time.monotonic()
    period = 1.0 / a.rate
    next_t, last_print = t0, t0
    try:
        while not stop:
            t = time.monotonic() - t0
            cmd = sample(screen, ax + vx * t, ay + vy * t) * to_cmd
            cmd -= cmd.mean()
            if a.rms > 0:
                rms = cmd.std()
                cmd *= a.rms / rms if rms > 0 else 0.0
            cmd *= a.gain
            if a.cap > 0:
                cmd = np.clip(cmd, -a.cap, a.cap)
            shm.set_data(cmd.reshape(-1, 1).astype(np.float32))
            now = time.monotonic()
            if now - last_print >= 1.0:
                last_print = now
                sys.stdout.write(f"\r  t {t:7.1f} s  RMS {cmd.std():.4f}  peak {np.abs(cmd).max():.4f}  ")
                sys.stdout.flush()
            next_t += period
            delay = next_t - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                next_t = time.monotonic()                 # late: no catch-up burst
    finally:
        shm.set_data(zero)
        print(f"\n{a.shm} set back to zero")


if __name__ == '__main__':
    main()
