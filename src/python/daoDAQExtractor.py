"""
daoDAQ telemetry extraction.

Finds the frames recorded by daoDAQ between two times and extracts them into
one array per source, with their timestamps and counters. Used by
daoDAQExtract.py (command line) and the Extract tab of daoDAQCtrl.py.

daoDAQ writes one FITS HDU per frame, and every HDU of a file has the same
header and data size. A file is therefore indexed from its first two HDUs
(position of frame k = offset of HDU 1 + (k - 1) * stride), and a time range
is found by binary search on the ATIME header keyword. Only a few headers are
read per file, plus the frames asked for; nothing is loaded up front.

    from daoDAQExtractor import scan, extract, parse_time, write

    catalog = scan("/data/dao/daq")                 # sessions and their sources
    t0, t1 = parse_time("2026-09-23 17:40"), parse_time("17:45")
    result = extract(catalog, t0, t1, sources=["wfs"])
    result["wfs"].data, result["wfs"].atime         # (n, rows, cols), ns since epoch
    write(result, "wfs_1740.h5")

Times are int nanoseconds since the Unix epoch (ATIME). The range is
inclusive: start <= ATIME <= end.
"""

import datetime as _dt
import os
import re
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional

import numpy as np

FITS_BLOCK = 2880
SESSION_RE = re.compile(r"^\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}$")
ROLLOVER_RE = re.compile(r"^(.+)_(\d+)\.fits$")

# FITS BITPIX -> stored (big-endian) dtype
_BITPIX = {8: "u1", 16: ">i2", 32: ">i4", 64: ">i8", -32: ">f4", -64: ">f8"}


class NotDaqFile(Exception):
    """A FITS file that was not written by daoDAQ (e.g. a file:// source copy)."""


# ----------------------------------------------------------------------------
# time helpers
def parse_time(text, utc=False, today=None, end=False):
    """Parse a user time into ns since the epoch.

    Accepts "2026-09-23 17:40:05.250", ISO 8601 (with or without offset),
    "17:40" / "17:40:05.5" (today), or a number of seconds since the epoch.
    Naive times are local time, or UTC with utc=True.

    With end=True, a time given to the millisecond or coarser covers its whole
    last millisecond, so an end time copied from format_time() (or the GUI)
    includes the frame it shows."""
    text = str(text).strip()
    fraction = re.search(r"[.,](\d+)", text)
    pad = 999_999 if end and (fraction is None or len(fraction.group(1)) <= 3) else 0
    return _parse_time(text, utc, today) + pad


def _parse_time(text, utc, today):
    try:
        seconds = float(text)
    except ValueError:
        pass
    else:
        whole = int(seconds)
        return whole * 10**9 + int(round((seconds - whole) * 1e9))

    if re.fullmatch(r"\d{1,2}:\d{2}(:\d{2}(\.\d+)?)?", text):
        day = today or _dt.date.today()
        text = f"{day.isoformat()} {text}"
    try:
        stamp = _dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        raise ValueError(f"cannot understand time '{text}' "
                         "(use e.g. '2026-09-23 17:40:05', '17:40' or epoch seconds)")
    if stamp.tzinfo is None:
        stamp = stamp.replace(tzinfo=_dt.timezone.utc) if utc else stamp.astimezone()
    whole = stamp.replace(microsecond=0)
    return int(whole.timestamp()) * 10**9 + stamp.microsecond * 1000


def format_time(ns, utc=False, ms=True):
    """ns since the epoch -> 'YYYY-mm-dd HH:MM:SS.mmm' (local time unless utc)."""
    tz = _dt.timezone.utc if utc else None
    stamp = _dt.datetime.fromtimestamp(ns // 10**9, tz).replace(microsecond=(ns % 10**9) // 1000)
    return stamp.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3 if ms else -7]


# ----------------------------------------------------------------------------
# FITS reading (only what cfitsio writes for daoDAQ)
def _value(raw):
    raw = raw.strip()
    if raw.startswith("'"):
        return raw.strip("'").strip()
    if raw in ("T", "F"):
        return raw == "T"
    try:
        return int(raw)
    except ValueError:
        return float(raw)


def _read_header(f, offset):
    """Header cards at `offset` -> (dict, header size in bytes), or None if truncated."""
    f.seek(offset)
    cards, size = {}, 0
    while True:
        block = f.read(FITS_BLOCK)
        if len(block) < FITS_BLOCK:
            return None
        size += FITS_BLOCK
        for i in range(0, FITS_BLOCK, 80):
            card = block[i:i + 80].decode("ascii", "replace")
            key = card[:8].strip()
            if key == "END":
                return cards, size
            if card[8:10] == "= ":
                raw = card[10:]
                if not raw.lstrip().startswith("'"):
                    raw = raw.split("/", 1)[0]
                cards[key] = _value(raw)


@dataclass
class _Hdu:
    offset: int          # header start
    data_offset: int
    size: int            # header + padded data
    shape: tuple         # numpy order (slowest axis first)
    bitpix: int
    bzero: float
    bscale: float
    atime: int
    cnt0: int
    cnt1: int
    cnt2: int


def _hdu_at(f, offset):
    parsed = _read_header(f, offset)
    if parsed is None:
        return None
    cards, header_size = parsed
    naxis = cards.get("NAXIS", 0)
    shape = tuple(cards[f"NAXIS{i}"] for i in range(naxis, 0, -1))
    nbytes = abs(cards["BITPIX"]) // 8 * int(np.prod(shape)) if naxis else 0
    padded = -(-nbytes // FITS_BLOCK) * FITS_BLOCK
    return _Hdu(offset, offset + header_size, header_size + padded, shape, cards["BITPIX"],
                cards.get("BZERO", 0), cards.get("BSCALE", 1),
                cards.get("ATIME"), cards.get("CNT0", 0), cards.get("CNT1", 0), cards.get("CNT2", 0))


def _decode(raw, hdu):
    """Stored FITS values -> native numpy array (unsigned types via BZERO)."""
    bits = abs(hdu.bitpix)
    if hdu.bscale == 1 and hdu.bzero == 0:
        return raw.astype(raw.dtype.newbyteorder("="))
    if hdu.bscale == 1 and hdu.bitpix == 8 and hdu.bzero == -128:
        return (raw ^ np.uint8(0x80)).view(np.int8)
    if hdu.bscale == 1 and hdu.bitpix in (16, 32, 64) and hdu.bzero == 2 ** (bits - 1):
        unsigned = raw.view(f">u{bits // 8}") ^ np.array(1 << (bits - 1), dtype=f">u{bits // 8}")
        return unsigned.astype(f"u{bits // 8}")
    return raw.astype(np.float64) * hdu.bscale + hdu.bzero


class FitsFile:
    """Random access to the frames of one daoDAQ FITS file."""

    def __init__(self, path):
        self.path = str(path)
        self._f = open(self.path, "rb")
        size = os.fstat(self._f.fileno()).st_size
        first = _hdu_at(self._f, 0)
        if first is None or first.atime is None or not first.shape:
            self._f.close()
            raise NotDaqFile(self.path)
        self.shape, self.bitpix = first.shape, first.bitpix
        self._first = first
        self._offsets = None    # explicit offsets when the layout is irregular
        self._stride = 0
        n_rest = 0
        second = _hdu_at(self._f, first.size) if size > first.size else None
        if second is not None:
            self._stride = second.size
            n_rest = (size - first.size) // self._stride   # a truncated last frame is ignored
            last = _hdu_at(self._f, first.size + (n_rest - 1) * self._stride) if n_rest else None
            if last is None or last.shape != self.shape or last.atime is None:
                self._scan_all(size)
                n_rest = len(self._offsets) - 1
        self.n = 1 + n_rest
        self.first_atime = first.atime
        self.last_atime = self.hdu(self.n - 1).atime
        self.dtype = self.read(0).dtype

    def _scan_all(self, size):
        """Fallback for files whose HDUs do not all have the same size."""
        offsets, offset = [], 0
        while offset < size:
            hdu = _hdu_at(self._f, offset)
            if hdu is None or hdu.atime is None or offset + hdu.size > size:
                break
            offsets.append(offset)
            offset += hdu.size
        self._offsets = offsets

    def _offset(self, k):
        if self._offsets is not None:
            return self._offsets[k]
        return 0 if k == 0 else self._first.size + (k - 1) * self._stride

    def hdu(self, k):
        return self._first if k == 0 else _hdu_at(self._f, self._offset(k))

    def atime(self, k):
        return self.hdu(k).atime

    def bisect(self, t, right=False):
        """First frame with ATIME >= t (or > t with right=True)."""
        lo, hi = 0, self.n
        while lo < hi:
            mid = (lo + hi) // 2
            a = self.atime(mid)
            if a < t or (right and a == t):
                lo = mid + 1
            else:
                hi = mid
        return lo

    def read(self, k, hdu=None):
        hdu = hdu or self.hdu(k)
        self._f.seek(hdu.data_offset)
        count = int(np.prod(hdu.shape))
        raw = np.fromfile(self._f, dtype=_BITPIX[hdu.bitpix], count=count)
        if raw.size != count:
            raise IOError(f"{self.path}: frame {k} is truncated")
        return _decode(raw.reshape(hdu.shape), hdu)

    def close(self):
        self._f.close()


# ----------------------------------------------------------------------------
# catalog of sessions and sources
@dataclass
class Source:
    name: str
    session: str
    files: List[FitsFile] = field(default_factory=list)

    @property
    def n_frames(self):
        return sum(f.n for f in self.files)

    @property
    def first_atime(self):
        return self.files[0].first_atime if self.files else None

    @property
    def last_atime(self):
        return self.files[-1].last_atime if self.files else None

    @property
    def shape(self):
        return self.files[0].shape if self.files else None

    @property
    def dtype(self):
        return self.files[0].dtype if self.files else None

    def frames_between(self, t0, t1):
        """[(file, k0, k1)] of the frames with t0 <= ATIME <= t1 (k1 exclusive)."""
        spans = []
        for f in self.files:
            if f.last_atime < t0 or f.first_atime > t1:
                continue
            k0, k1 = f.bisect(t0), f.bisect(t1, right=True)
            if k1 > k0:
                spans.append((f, k0, k1))
        return spans

    def count_between(self, t0, t1):
        return sum(k1 - k0 for _, k0, k1 in self.frames_between(t0, t1))


@dataclass
class Session:
    name: str                       # YYYY-mm-dd_HH-MM-SS (local time of START)
    path: str
    sources: Dict[str, Source] = field(default_factory=dict)
    skipped: List[str] = field(default_factory=list)   # FITS files that are not daoDAQ frames

    @property
    def first_atime(self):
        times = [s.first_atime for s in self.sources.values() if s.files]
        return min(times) if times else None

    @property
    def last_atime(self):
        times = [s.last_atime for s in self.sources.values() if s.files]
        return max(times) if times else None


def scan_session(path):
    """Index one session directory."""
    path = str(path)
    session = Session(os.path.basename(os.path.normpath(path)), path)
    for entry in sorted(os.listdir(path)):
        full = os.path.join(path, entry)
        if os.path.isfile(full) and entry.endswith(".fits"):
            files = [full]
            name = entry[:-len(".fits")]
        elif os.path.isdir(full):
            files = [os.path.join(full, f) for f in os.listdir(full)
                     if (m := ROLLOVER_RE.match(f)) and m.group(1) == entry]
            files.sort(key=lambda p: int(ROLLOVER_RE.match(os.path.basename(p)).group(2)))
            name = entry
        else:
            continue
        source = Source(name, session.name)
        for f in files:
            try:
                fits_file = FitsFile(f)
            except (NotDaqFile, KeyError, ValueError, OSError):
                session.skipped.append(f)   # e.g. a file:// source that happens to be FITS
                continue
            if fits_file.n:
                source.files.append(fits_file)
        if source.files:
            session.sources[name] = source
    return session


def scan(root, sessions=None):
    """Index every session under a daoDAQ root_storage (or only `sessions`)."""
    root = str(root)
    if not os.path.isdir(root):
        raise FileNotFoundError(f"{root} is not a directory")
    names = sorted(d for d in os.listdir(root)
                   if SESSION_RE.match(d) and os.path.isdir(os.path.join(root, d)))
    if sessions is not None:
        wanted = set(sessions)
        names = [n for n in names if n in wanted]
    return [scan_session(os.path.join(root, n)) for n in names]


def close(catalog):
    for session in catalog:
        for source in session.sources.values():
            for f in source.files:
                f.close()


# ----------------------------------------------------------------------------
# extraction
@dataclass
class Extracted:
    name: str
    data: np.ndarray                # (n_frames, *frame_shape)
    atime: np.ndarray               # int64 ns since epoch
    cnt0: np.ndarray
    cnt1: np.ndarray
    cnt2: np.ndarray
    sessions: List[str]


def plan(catalog, t0, t1, sources=None):
    """{output name: [Source, ...]} of the sources with frames in [t0, t1].

    A source recorded in several sessions is merged when its frame shape and
    type match; otherwise each session's part is named '<source>@<session>'."""
    wanted = set(sources) if sources else None
    groups: Dict[str, List[Source]] = {}
    for session in catalog:
        for name, source in session.sources.items():
            if wanted is not None and name not in wanted:
                continue
            if source.last_atime < t0 or source.first_atime > t1 or not source.count_between(t0, t1):
                continue
            groups.setdefault(name, []).append(source)
    result = {}
    for name, parts in groups.items():
        if len({(p.shape, str(p.dtype)) for p in parts}) == 1:
            result[name] = parts
        else:
            for p in parts:
                result[f"{name}@{p.session}"] = [p]
    return result


def estimate(catalog, t0, t1, sources=None):
    """(frames, bytes) that extract() would return."""
    frames = size = 0
    for parts in plan(catalog, t0, t1, sources).values():
        for p in parts:
            n = p.count_between(t0, t1)
            frames += n
            size += n * int(np.prod(p.shape)) * np.dtype(p.dtype).itemsize
    return frames, size


def extract(catalog, t0, t1, sources=None,
            progress: Optional[Callable[[int, int], None]] = None,
            cancelled: Optional[Callable[[], bool]] = None) -> Dict[str, Extracted]:
    """Frames with t0 <= ATIME <= t1, one Extracted per source, in time order.

    progress(done, total) is called while reading; cancelled() returning True
    stops early (InterruptedError)."""
    groups = plan(catalog, t0, t1, sources)
    spans = {name: [s for p in parts for s in p.frames_between(t0, t1)] for name, parts in groups.items()}
    total = sum(k1 - k0 for sp in spans.values() for _, k0, k1 in sp)
    done = 0
    result = {}
    for name, parts in groups.items():
        n = sum(k1 - k0 for _, k0, k1 in spans[name])
        data = np.empty((n,) + tuple(parts[0].shape), dtype=parts[0].dtype)
        meta = np.empty((4, n), dtype=np.int64)
        i = 0
        for f, k0, k1 in spans[name]:
            for k in range(k0, k1):
                hdu = f.hdu(k)
                data[i] = f.read(k, hdu)
                meta[:, i] = (hdu.atime, hdu.cnt0, hdu.cnt1, hdu.cnt2)
                i += 1
                done += 1
                if progress and done % 256 == 0:
                    progress(done, total)
                if cancelled and done % 256 == 0 and cancelled():
                    raise InterruptedError("extraction cancelled")
        order = np.argsort(meta[0], kind="stable")
        result[name] = Extracted(name, data[order], meta[0][order], meta[1][order].astype(np.uint64),
                                 meta[2][order].astype(np.uint64), meta[3][order].astype(np.uint64),
                                 [p.session for p in parts])
    if progress:
        progress(done, total)
    return result


# ----------------------------------------------------------------------------
# output
FORMATS = {".h5": "HDF5", ".hdf5": "HDF5", ".fits": "FITS", ".npz": "NumPy"}


def write(result, path, t0=None, t1=None):
    """Write an extract() result; the format follows the extension (.h5, .fits, .npz).

    - .h5:   one group per source: data, atime, cnt0, cnt1, cnt2 (+ attributes)
    - .fits: per source an image cube '<source>' and a table '<source>_TIME'
             (ATIME, CNT0, CNT1, CNT2)
    - .npz:  arrays '<source>.data', '<source>.atime', '<source>.cnt0', ...
    """
    ext = os.path.splitext(path)[1].lower()
    if ext not in FORMATS:
        raise ValueError(f"unknown output format '{ext}' (use {', '.join(sorted(FORMATS))})")
    info = {"t_start_ns": -1 if t0 is None else int(t0), "t_end_ns": -1 if t1 is None else int(t1),
            "created": _dt.datetime.now().astimezone().isoformat(timespec="seconds"),
            "creator": "daoDAQExtract"}

    if FORMATS[ext] == "HDF5":
        import h5py
        with h5py.File(path, "w") as h5:
            h5.attrs.update(info)
            for name, r in result.items():
                g = h5.create_group(name)
                chunks = (1,) + r.data.shape[1:] if len(r.data) else None
                g.create_dataset("data", data=r.data, chunks=chunks)
                for key in ("atime", "cnt0", "cnt1", "cnt2"):
                    g.create_dataset(key, data=getattr(r, key))
                g["atime"].attrs["unit"] = "ns since 1970-01-01 UTC"
                g.attrs["sessions"] = ",".join(r.sessions)
    elif FORMATS[ext] == "FITS":
        from astropy.io import fits
        primary = fits.PrimaryHDU()
        primary.header["TSTART"] = (info["t_start_ns"], "range start, ns since epoch")
        primary.header["TEND"] = (info["t_end_ns"], "range end, ns since epoch")
        primary.header["CREATOR"] = info["creator"]
        hdus = [primary]
        for name, r in result.items():
            hdus.append(fits.ImageHDU(r.data, name=name[:68]))
            hdus.append(fits.BinTableHDU.from_columns([
                fits.Column("ATIME", "K", array=r.atime),
                fits.Column("CNT0", "K", array=r.cnt0.astype(np.int64)),
                fits.Column("CNT1", "K", array=r.cnt1.astype(np.int64)),
                fits.Column("CNT2", "K", array=r.cnt2.astype(np.int64))], name=f"{name[:63]}_TIME"))
        fits.HDUList(hdus).writeto(path, overwrite=True)
    else:
        arrays = {}
        for name, r in result.items():
            for key in ("data", "atime", "cnt0", "cnt1", "cnt2"):
                arrays[f"{name}.{key}"] = getattr(r, key)
        arrays["_info"] = np.array([f"{k}={v}" for k, v in info.items()])
        np.savez(path, **arrays)


def available_formats():
    """Output extensions usable on this machine."""
    out = [".npz"]
    try:
        import h5py  # noqa: F401
        out.insert(0, ".h5")
    except ImportError:
        pass
    try:
        from astropy.io import fits  # noqa: F401
        out.insert(1 if out[0] == ".h5" else 0, ".fits")
    except ImportError:
        pass
    return out
