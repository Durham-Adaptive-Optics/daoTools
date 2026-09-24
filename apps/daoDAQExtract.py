#!/usr/bin/env python3
"""
daoDAQExtract - extract daoDAQ telemetry between two times.

Looks through the sessions recorded by daoDAQ under a root_storage, and
writes the frames whose timestamp (ATIME) falls in [start, end] to one file:
one array per source, with its timestamps and counters.

Usage:
  daoDAQExtract.py ROOT --list
  daoDAQExtract.py ROOT --start T [--end T] [--source a,b] [-o OUT.h5|.fits|.npz]

Times: "2026-09-23 17:40:05.250", "17:40" (today), ISO 8601 with offset, or
epoch seconds. Local time unless --utc. The range is inclusive, and an end
time given to the millisecond includes that whole millisecond, so times
copied from --list work as shown. Without -o, only prints what would be
extracted.

Examples:
  daoDAQExtract.py $DAODATA --list
  daoDAQExtract.py $DAODATA --start 17:40 --end 17:45 -o wfs_1740.h5
  daoDAQExtract.py $DAODATA --start "2026-09-23 17:40" --end "2026-09-23 17:40:10" \\
                   --source wfs,dm -o /tmp/slice.fits
"""

import argparse
import os
import sys

from daoDAQExtractor import (close, estimate, extract, format_time, parse_time,
                             scan, write, FORMATS)


def human_bytes(n):
    for unit in ("B", "kB", "MB", "GB", "TB"):
        if n < 1000 or unit == "TB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1000


def print_catalog(catalog, utc):
    if not catalog:
        print("no daoDAQ session found")
        return
    print(f"{'session':21} {'source':20} {'frames':>8}  {'first frame':23}  {'last frame':23}  shape / type")
    for session in catalog:
        if not session.sources:
            print(f"{session.name:21} (no frames)")
        for name, s in session.sources.items():
            print(f"{session.name:21} {name:20} {s.n_frames:8d}  {format_time(s.first_atime, utc)}  "
                  f"{format_time(s.last_atime, utc)}  {'x'.join(map(str, s.shape))} {s.dtype}")


def main():
    p = argparse.ArgumentParser(description="Extract daoDAQ telemetry between two times.",
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog=__doc__.split("Examples:")[1].join(["Examples:", ""]))
    p.add_argument("root", help="daoDAQ root_storage (folder holding the session folders)")
    p.add_argument("--list", action="store_true", help="list sessions, sources and time spans")
    p.add_argument("--start", help="start time (default: first recorded frame)")
    p.add_argument("--end", help="end time (default: last recorded frame)")
    p.add_argument("--source", help="comma-separated source names (default: all)")
    p.add_argument("--session", help="comma-separated session folders to look in (default: all)")
    p.add_argument("--utc", action="store_true", help="times given and shown in UTC")
    p.add_argument("-o", "--output", help=f"output file ({', '.join(sorted(FORMATS))})")
    args = p.parse_args()

    catalog = scan(args.root, args.session.split(",") if args.session else None)
    try:
        if args.list:
            print_catalog(catalog, args.utc)
            return 0

        firsts = [s.first_atime for s in catalog if s.first_atime is not None]
        lasts = [s.last_atime for s in catalog if s.last_atime is not None]
        if not firsts:
            print("no recorded frames found", file=sys.stderr)
            return 1
        try:
            t0 = parse_time(args.start, args.utc) if args.start else min(firsts)
            t1 = parse_time(args.end, args.utc, end=True) if args.end else max(lasts)
        except ValueError as exc:
            print(exc, file=sys.stderr)
            return 2
        if t1 < t0:
            print("end is before start", file=sys.stderr)
            return 1
        sources = args.source.split(",") if args.source else None

        frames, size = estimate(catalog, t0, t1, sources)
        print(f"range {format_time(t0, args.utc)} -> {format_time(t1, args.utc)}"
              f"{' UTC' if args.utc else ''}: {frames} frames, {human_bytes(size)}")
        if not frames:
            return 1
        if not args.output:
            return 0

        def progress(done, total):
            print(f"\r  {done}/{total} frames", end="", file=sys.stderr, flush=True)

        result = extract(catalog, t0, t1, sources, progress=progress)
        print(file=sys.stderr)
        for name, r in result.items():
            print(f"  {name:24} {len(r.data):8d} frames  {format_time(int(r.atime[0]), args.utc)} -> "
                  f"{format_time(int(r.atime[-1]), args.utc)}  from {', '.join(r.sessions)}")
        write(result, args.output, t0, t1)
        print(f"wrote {os.path.abspath(args.output)}")
        return 0
    finally:
        close(catalog)


if __name__ == "__main__":
    sys.exit(main())
