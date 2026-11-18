#!/usr/bin/env python3
"""daoEtfMeasure.py -- measure the error transfer function (ETF) of an AO loop on one mode.

A random disturbance on the mode (its M2A column times amplitude x U(-1, 1), one value per
frame) is written to a DM channel in step with the WFS modes, first with the loop closed,
then with the loop open. ETF = CSD(closed, open) / PSD(open), compared with the integrator
model (daoEtfLib.py). The loop is left OPEN at the end; the disturbance channel is zeroed,
also when the measurement is stopped (Ctrl-C, SIGTERM) or fails.

The data (the data_rtf dict: the modes closed and open, the disturbance, fs, the gain...)
is saved as <telemetry>/<YYYY_MM_DD>/<tn>/<tn>_data_rtf.npy, and
<telemetry>/tmp/current_telemetry_folder.txt points at it. daoEtfDisp.py shows it.

Examples:
    daoEtfMeasure.py 3 0.01 --fs 1000
    daoEtfMeasure.py 3 0.01 --fs-shm /tmp/camFps.im.shm -t 30 \\
        --loop /tmp/aoLpCmd.im.shm --gain /tmp/aoLpGain.im.shm --modes /tmp/wfsModes.im.shm \\
        --dist /tmp/dmCmd05.im.shm --m2a /tmp/dmM2C.im.shm
"""

import argparse
import signal
import sys

import numpy as np

import daoEtfLib as etf

DEFAULTS = {
    "loop": "/tmp/lpCmd.im.shm",
    "gain": "/tmp/lpGain.im.shm",
    "modes": "/tmp/modes.im.shm",
    "dist": "/tmp/dmCmd03.im.shm",
    "m2a": "/tmp/dmM2A.im.shm",
}


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                epilog=__doc__.split("\n\n", 1)[1],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", type=int, help="index of the mode (0-based)")
    p.add_argument("amp_mode", type=float, help="amplitude of the disturbance (M2A units)")
    rate = p.add_mutually_exclusive_group(required=True)
    rate.add_argument("--fs", type=float, help="frame rate [Hz]")
    rate.add_argument("--fs-shm", help="SHM holding the frame rate [Hz] (first value)")
    p.add_argument("-t", "--duration", type=float, default=30.0,
                   help="duration of each acquisition (closed, open) [s] (default 30)")
    p.add_argument("--start", type=int, default=50, help="first sample of the ETF (default 50)")
    p.add_argument("--end", type=int, default=15000, help="last sample of the ETF (default 15000)")
    p.add_argument("-N", type=int, default=6, help="Welch segment nperseg = N x 64 (default 6)")
    p.add_argument("--loop", default=DEFAULTS["loop"],
                   help=f"loop switch SHM, 0 open / 1 closed (default {DEFAULTS['loop']})")
    p.add_argument("--gain", default=DEFAULTS["gain"],
                   help=f"loop gain SHM, read only (default {DEFAULTS['gain']})")
    p.add_argument("--modes", default=DEFAULTS["modes"],
                   help=f"modes measured by the WFS (default {DEFAULTS['modes']})")
    p.add_argument("--dist", default=DEFAULTS["dist"],
                   help=f"DM channel taking the disturbance (default {DEFAULTS['dist']})")
    p.add_argument("--m2a", default=DEFAULTS["m2a"],
                   help=f"modes-to-actuators matrix, n_act x n_modes (default {DEFAULTS['m2a']})")
    p.add_argument("--tau", type=float, default=36e-6, help="model: RTC delay [s] (default 36e-6)")
    p.add_argument("--tdm", type=float, default=375e-6, help="model: DM delay [s] (default 375e-6)")
    p.add_argument("--tcam", type=float, default=572e-6,
                   help="model: camera readout + transfer [s] (default 572e-6)")
    p.add_argument("--telemetry", default=None,
                   help="telemetry folder (default $DAODATA/telemetry)")
    p.add_argument("--seed", type=int, default=None, help="random seed")
    p.add_argument("--no-plot", action="store_true", help="no matplotlib plot at the end")
    p.add_argument("--no-save", action="store_true", help="do not save the data")
    return p.parse_args(argv)


def progress(label, i, n):
    print(f"{label} {100 * i / n:5.1f}% ({i}/{n})", end="\r" if i < n else "\n", flush=True)


def main(argv=None):
    args = parse_args(argv)
    import dao

    # SIGTERM (the GUI's Stop) as an exception, so the disturbance channel is zeroed
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))

    try:
        shms = {k: dao.shm(getattr(args, k)) for k in DEFAULTS}
        fs = args.fs if args.fs is not None else float(np.asarray(dao.shm(args.fs_shm).get_data()).flat[0])
    except Exception as e:
        print(f"ERROR: cannot open the SHMs: {e}", file=sys.stderr)
        return 1
    if not fs > 0:
        print(f"ERROR: frame rate {fs} Hz", file=sys.stderr)
        return 1

    print("\n" + "=" * 54 + "\nETF MEASUREMENT\n" + "=" * 54)
    for k, v in (("mode", args.mode), ("amplitude", args.amp_mode), ("fs", f"{fs} Hz"),
                 ("duration", f"{args.duration} s"), ("frames", int(fs * args.duration)),
                 ("loop gain", float(np.asarray(shms["gain"].get_data()).flat[0])),
                 ("start, end", f"{args.start}, {args.end}"), ("nperseg", args.N * 64),
                 *((k, getattr(args, k)) for k in DEFAULTS)):
        print(f"{k:12s}: {v}")
    print("=" * 54 + "\n")

    rng = np.random.default_rng(args.seed)
    try:
        data = etf.measure_etf(shms["loop"], shms["gain"], shms["modes"], shms["dist"], shms["m2a"],
                               args.mode, args.amp_mode, fs, args.duration, args.start, args.end,
                               args.N, rng=rng, progress=progress)
    except ValueError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    data["shms"] = {k: getattr(args, k) for k in DEFAULTS}
    print("Loop left OPEN.")

    mode, s, e = args.mode, data["start"], data["end"]
    print(f"\nclosed rms : {np.std(data['out'][mode, s:e]):.4g}")
    print(f"open rms   : {np.std(data['inp'][mode, s:e]):.4g}")
    print(f"ETF        : {np.min(data['FTR']):.2f} .. {np.max(data['FTR']):.2f} dB")

    if not args.no_save:
        path = etf.save_etf_data({"data_rtf": data}, args.telemetry)
        print(f"Saving in {path}")
    else:
        print("Not saved (--no-save).")

    if not args.no_plot:
        import matplotlib.pyplot as plt
        Ti = 1.0 / fs
        f = data["frequency_vector"]
        model = etf.model_db(f[1:], data["loop_gain"], Ti, args.tau, args.tdm, args.tcam)
        latency = Ti + args.tau + args.tdm + args.tcam
        plt.figure()
        plt.semilogx(f[1:], model, label="model")
        plt.semilogx(f[1:], data["FTR"][1:], label="measured")
        plt.title(f"Mode {mode} - gain {data['loop_gain']:.3f} - latency {latency * 1e3:.3f} ms")
        plt.grid(which="both")
        plt.xlabel("Frequency [Hz]")
        plt.ylabel("ETF [dB]")
        plt.legend()
        plt.show()
    return 0


if __name__ == "__main__":
    sys.exit(main())
