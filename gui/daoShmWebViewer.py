#!/usr/bin/env python3
"""
daoShmWebViewer: the daoShmViewer in a web browser.

    daoShmWebViewer.py                      # http://localhost:8090, /tmp, read-only
    daoShmWebViewer.py --write              # also set / load / create SHMs from the page
    daoShmWebViewer.py --dir /tmp --dir /dev/shm/loop --port 8091
    daoShmWebViewer.py --host 0.0.0.0 --token secret    # other machines: http://<host>:8090/?token=secret

From another machine without opening a port: ssh -L 8090:localhost:8090 <rtc>, then
http://localhost:8090 there.

The server maps the SHM files read-only (mmap of the /tmp file): it never takes a semaphore,
never writes a byte of an SHM it only shows, and never attaches a GPU SHM (only its /tmp
host copy is read, when the SHM keeps one: DAO_GPU_MIRROR). Frames are sent when cnt0
changes, at most at the rate each view asks for, and only once the browser took the
previous one: a slow link drops frames, it never slows the loop.

Writes (--write only) go through libdao (daoShm.shm.set_data), as the Qt viewer's. --write
also allows the tools that start processes or write files: latency (daoTimeDiff in tmux),
snapshots (daoSnapshot.py), DAQ sessions (DAQClient), a terminal on a tmux session, recordings
saved on the server. Read-only, the page still records (to the browser), shows the tmux output
and the DAQ state.

The page (gui/web/daoShmViewer/) is plain HTML / JavaScript, with nothing loaded from the
internet: it works on an isolated network.
"""
import argparse
import ctypes
import fcntl
import glob
import io
import json
import math
import mmap
import os
import secrets
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor

import numpy as np
import tornado.ioloop
import tornado.web
import tornado.websocket

import daoShm

EXEC = ThreadPoolExecutor(6)                          # blocking calls: tmux, daoSnapshot, DAQ, ...

################################################
#           SHM files, read-only
################################################

MD = daoShm.IMAGE_METADATA
MD_SIZE = ctypes.sizeof(MD)
DAO_SHM_MAGIC = 0x4D485344
DAO_GPU_MAGIC = 0x55504744
DAO_GPU_MIRROR = 0x1
SEM_LOCK_BASE = 1 << 62
F_OFD_GETLK = getattr(fcntl, "F_OFD_GETLK", 36)

# dao atype: numpy dtype
ATYPES = {1: "u1", 2: "i1", 3: "u2", 4: "i2", 5: "u4", 6: "i4", 7: "u8", 8: "i8",
          9: "f4", 10: "f8", 11: "c8", 12: "c16"}


def _field(name, fmt):
    return (getattr(MD, name).offset, struct.Struct("<" + fmt))


FIELDS = {
    "magic": _field("magic", "I"), "layout": _field("layout", "I"), "naxis": _field("naxis", "B"),
    "size": _field("size", "3I"), "nelement": _field("nelement", "Q"), "atype": _field("atype", "B"),
    "creation_time": _field("creation_time", "d"), "last_access": _field("last_access", "d"),
    "atime": _field("atime", "qq"), "shared": _field("shared", "B"), "status": _field("status", "B"),
    "logflag": _field("logflag", "B"), "sem": _field("sem", "H"), "cnt0": _field("cnt0", "Q"),
    "cnt1": _field("cnt1", "Q"), "cnt2": _field("cnt2", "Q"), "write": _field("write", "B"),
    "NBkw": _field("NBkw", "H"), "fifo_size": _field("fifo_size", "I"),
    "fifo_last_written": _field("fifo_last_written", "I"), "gpu_magic": _field("gpu_magic", "I"),
    "gpu_device": _field("gpu_device", "i"), "gpu_flags": _field("gpu_flags", "I"),
    "gpu_size": _field("gpu_size", "Q"),
}
NAME_OFF = MD.name.offset
NAME_LEN = MD.name.size


class ShmFile:
    """One SHM file, mapped read-only."""

    def __init__(self, path):
        self.path = path
        fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
        try:
            st = os.fstat(fd)
            if st.st_size < MD_SIZE:
                raise ValueError("too small for an SHM")
            self.mm = mmap.mmap(fd, 0, access=mmap.ACCESS_READ)
        except Exception:
            os.close(fd)
            raise
        self.fd = fd                                  # kept: semaphore lock tests
        self.ino, self.dev, self.fsize = st.st_ino, st.st_dev, st.st_size
        if self.f("magic") != DAO_SHM_MAGIC:
            self.close()
            raise ValueError("not a dao SHM (or an older layout)")
        self.layout = self.f("layout")
        self.naxis = max(1, min(3, self.f("naxis")))
        self.shape = tuple(int(s) for s in self.f("size")[:self.naxis])
        self.nelement = int(self.f("nelement"))
        self.atype = self.f("atype")
        self.dtype = np.dtype(ATYPES.get(self.atype, "u1"))
        self.fifo = max(1, int(self.f("fifo_size")))
        self.nsem = int(self.f("sem"))
        self.gpu = self.f("gpu_magic") == DAO_GPU_MAGIC
        self.mirror = bool(self.f("gpu_flags") & DAO_GPU_MIRROR)
        self.data_off = self.fifo * MD_SIZE
        self.seg_bytes = self.nelement * self.dtype.itemsize
        self.readable = (not self.gpu or self.mirror) and self.atype in ATYPES \
            and self.data_off + self.fifo * self.seg_bytes <= self.fsize \
            and int(np.prod(self.shape)) == self.nelement
        # list rate
        self.rate = 0.0
        self.rate_cnt = self.cnt0()
        self.rate_t = time.monotonic()

    def close(self):
        try:
            self.mm.close()
        except Exception:
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass

    def f(self, name, seg=0):
        off, s = FIELDS[name]
        v = s.unpack_from(self.mm, seg * MD_SIZE + off)
        return v if len(v) > 1 else v[0]

    def last(self):
        last = self.f("fifo_last_written")
        return last if last < self.fifo else 0

    def cnt0(self):
        return self.f("cnt0", self.last())

    def ts(self, seg=None):
        sec, nsec = self.f("atime", self.last() if seg is None else seg)
        return sec + nsec * 1e-9

    def name(self):
        raw = bytes(self.mm[NAME_OFF:NAME_OFF + NAME_LEN])
        return raw.split(b"\0", 1)[0].decode(errors="replace")

    def stale(self):
        """True when the file was removed or replaced (new inode or size: recreated)."""
        try:
            st = os.stat(self.path)
        except OSError:
            return True
        return st.st_ino != self.ino or st.st_dev != self.dev or st.st_size != self.fsize

    def read(self):
        """(array in the SHM's shape, segment): a copy of the newest frame, retried when a
        write was going on during the copy."""
        arr, seg = None, 0
        for _ in range(4):
            seg = self.last()
            c = self.f("cnt0", seg)
            arr = np.frombuffer(self.mm, self.dtype, self.nelement, self.data_off + seg * self.seg_bytes).copy()
            if self.f("cnt0", seg) == c and self.last() == seg and not self.f("write"):
                break
        return arr.reshape(self.shape), seg

    def summary(self, now):
        cnt = self.cnt0()
        dt = now - self.rate_t
        if dt >= 0.5:
            self.rate = max(0.0, (cnt - self.rate_cnt) / dt)
            self.rate_cnt, self.rate_t = cnt, now
        ts = self.ts()
        age = time.time() - ts if ts > 0 else None
        return {"path": self.path, "shape": self.shape, "dtype": self.dtype.name, "cnt0": cnt,
                "rate": round(self.rate, 2), "age": age if age is not None and -1 < age < 1e8 else None,
                "gpu": self.gpu, "readable": self.readable, "fifo": self.fifo, "nsem": self.nsem,
                "bytes": self.seg_bytes}

    def meta(self):
        seg = self.last()
        md = {k: self.f(k, seg) for k in ("cnt0", "cnt1", "cnt2", "write", "status", "shared", "logflag")}
        sec, nsec = self.f("atime", seg)
        md.update({
            "path": self.path, "name": self.name(), "layout": self.layout, "naxis": self.naxis,
            "shape": self.shape, "nelement": self.nelement, "atype": self.atype, "dtype": self.dtype.name,
            "bytes": self.seg_bytes, "file_bytes": self.fsize, "creation_time": self.f("creation_time"),
            "last_access": self.f("last_access"), "atime": sec + nsec * 1e-9, "sem": self.nsem,
            "NBkw": self.f("NBkw"), "fifo_size": self.fifo, "fifo_last_written": seg,
            "gpu": self.gpu, "readable": self.readable,
        })
        if self.gpu:
            md.update({"gpu_device": self.f("gpu_device"), "gpu_mirror": self.mirror,
                       "gpu_bytes": self.f("gpu_size")})
        return md

    def sems(self):
        """{sem: [{pid, cmd}]} for the semaphores a reader holds; [] = PID not visible."""
        users = {}
        for s in range(self.nsem):
            fl = struct.pack("hhqqi4x", fcntl.F_WRLCK, os.SEEK_SET, SEM_LOCK_BASE + s, 1, 0)
            try:
                res = fcntl.fcntl(self.fd, F_OFD_GETLK, fl)
            except OSError:
                return None                           # no OFD locks here
            if struct.unpack("hhqqi4x", res)[0] != fcntl.F_UNLCK:
                users[s] = []
        if users and os.path.isdir("/proc/self/fdinfo"):
            dev = "%02x:%02x:%d" % (os.major(self.dev), os.minor(self.dev), self.ino)
            for pid in os.listdir("/proc"):
                if not pid.isdigit():
                    continue
                try:
                    fds = os.listdir("/proc/%s/fdinfo" % pid)
                except OSError:
                    continue
                for fd in fds:
                    try:
                        with open("/proc/%s/fdinfo/%s" % (pid, fd)) as fh:
                            for line in fh:
                                if line.startswith("lock:") and "OFDLCK" in line and " " + dev + " " in line:
                                    s = int(line.split()[-2]) - SEM_LOCK_BASE
                                    if s in users and all(u["pid"] != int(pid) for u in users[s]):
                                        users[s].append({"pid": int(pid), "cmd": process_command(pid)})
                    except (OSError, ValueError):
                        continue
        return users


def process_command(pid):
    try:
        with open("/proc/%s/cmdline" % pid, "rb") as fh:
            return fh.read().replace(b"\0", b" ").decode(errors="replace").strip()
    except OSError:
        return ""


class Registry:
    """The SHM files of the watched directories, rescanned every second."""

    def __init__(self, dirs):
        self.dirs = [os.path.realpath(d) for d in dirs]
        self.files = {}
        self.errors = {}
        self.scan()

    def allowed(self, path):
        real = os.path.realpath(path)
        return path.endswith(".im.shm") and any(os.path.dirname(real) == d for d in self.dirs)

    def scan(self):
        seen = set()
        for d in self.dirs:
            for path in glob.glob(os.path.join(d, "*.im.shm")):
                seen.add(path)
                f = self.files.get(path)
                if f is not None and not f.stale():
                    continue
                if f is not None:
                    f.close()
                    del self.files[path]
                try:
                    self.files[path] = ShmFile(path)
                    self.errors.pop(path, None)
                except Exception as e:
                    self.errors[path] = str(e)
        for path in list(self.files):
            if path not in seen:
                self.files.pop(path).close()
        for path in list(self.errors):
            if path not in seen:
                del self.errors[path]

    def get(self, path):
        f = self.files.get(path)
        if f is not None and f.stale():
            self.scan()
            f = self.files.get(path)
        return f

    def listing(self):
        now = time.monotonic()
        items = [f.summary(now) for f in self.files.values()]
        items += [{"path": p, "error": e} for p, e in self.errors.items()]
        items.sort(key=lambda i: i["path"])
        return items


################################################
#           Frames
################################################

def to_wire(a, part):
    """The array as sent: types JavaScript has (int64 as float64), complex as one real part."""
    if np.iscomplexobj(a):
        a = {"real": a.real, "imag": a.imag, "phase": np.angle(a)}.get(part, np.abs(a)).astype(np.float32)
    elif a.dtype in (np.int64, np.uint64):
        a = a.astype(np.float64)
    return np.ascontiguousarray(a)


def stats(a):
    if a.size == 0:
        return {}
    if a.dtype.kind == "f":
        finite = np.isfinite(a)
        bad = int(a.size - np.count_nonzero(finite))
        v = a[finite] if bad else a
        if v.size == 0:
            return {"nonfinite": bad}
    else:
        bad, v = 0, a
    v64 = v.astype(np.float64, copy=False)
    return {"min": float(v.min()), "max": float(v.max()), "mean": float(v64.mean()),
            "std": float(v64.std()), "sum": float(v64.sum()), "nonfinite": bad}


def frame_message(sub_id, f, opts):
    a, seg = f.read()
    full_shape = a.shape
    depth = 0
    if a.ndim == 3:
        depth = a.shape[0]
        k = min(max(int(opts.get("slice", 0)), 0), depth - 1)
        a = a[k]
    w = to_wire(a, opts.get("part", "abs"))
    st = stats(w if np.iscomplexobj(a) else a)       # complex: of the part shown
    stride = 1
    max_el = int(opts.get("maxEl", 4 << 20))
    if w.size > max_el:
        if w.ndim == 2:
            stride = int(math.ceil(math.sqrt(w.size / max_el)))
            w = np.ascontiguousarray(w[::stride, ::stride])
        else:
            stride = int(math.ceil(w.size / max_el))
            w = np.ascontiguousarray(w[::stride])
    sec, nsec = f.f("atime", seg)
    ts = sec + nsec * 1e-9
    header = {
        "type": "frame", "id": sub_id, "path": f.path, "cnt0": f.f("cnt0", seg), "cnt1": f.f("cnt1", seg),
        "cnt2": f.f("cnt2", seg), "ts": ts, "now": time.time(), "shape": full_shape, "view": w.shape,
        "dtype": w.dtype.name, "src": f.dtype.name, "slice": int(opts.get("slice", 0)) if depth else None,
        "depth": depth, "stride": stride, "stats": st,
    }
    hb = json.dumps(header).encode()
    pad = (-(4 + len(hb))) % 8                        # data 8-byte aligned: typed arrays view it as is
    return struct.pack("<I", len(hb) + pad) + hb + b" " * pad + w.tobytes()


################################################
#           Quick record
################################################

class Recorder:
    """Records N consecutive frames of an SHM in a thread. A CPU SHM: woken by a semaphore of
    its own (as the Qt viewer's get_data(check=True)), released at the end; a GPU SHM (its host
    copy): by polling cnt0. Every frame comes with its cnt0, cnt2 and timestamp: the frames
    missed (a writer faster than the copy) show as gaps in cnt0."""

    KEEP = 4

    def __init__(self, max_bytes):
        self.max_bytes = max_bytes
        self.recs = {}

    def start(self, f, n, timeout):
        n = int(n)
        if n < 1:
            raise ValueError("at least 1 frame")
        if n * f.seg_bytes > self.max_bytes:
            raise ValueError("%d frames of %d bytes: more than --record-max (%d MB)"
                             % (n, f.seg_bytes, self.max_bytes >> 20))
        done = [r for r in self.recs.values() if r["state"] != "recording"]
        for r in sorted(done, key=lambda r: r["t0"])[:max(0, len(done) - self.KEEP + 1)]:
            del self.recs[r["id"]]
        rec = {"id": uuid.uuid4().hex[:12], "path": f.path, "n": n, "done": 0, "state": "recording",
               "error": None, "t0": time.time(), "t1": None, "cancel": False, "gaps": 0,
               "mode": "poll" if f.gpu else "semaphore", "bytes": n * f.seg_bytes}
        self.recs[rec["id"]] = rec
        threading.Thread(target=self._run, args=(rec, f, float(timeout)), daemon=True).start()
        return rec["id"]

    def _run(self, rec, f, timeout):
        n = rec["n"]
        shm = None
        try:
            frames = np.empty((n,) + f.shape, f.dtype)
            cnt0 = np.zeros(n, np.uint64)
            cnt2 = np.zeros(n, np.uint64)
            ts = np.zeros(n, np.float64)
            if rec["mode"] == "semaphore":
                shm = daoShm.shm(f.path, logLevel=0)
            last = f.cnt0()
            i, t_frame = 0, time.monotonic()
            while i < n:
                if rec["cancel"]:
                    raise RuntimeError("cancelled after %d frames" % i)
                if shm is not None:
                    tsp = daoShm.make_timespec_from_now(0.25)
                    shm.daoShmWaitSemTimeout(ctypes.byref(shm.image), -1, ctypes.byref(tsp))  # -1: its own
                elif f.cnt0() == last:
                    time.sleep(5e-5)
                if f.cnt0() == last:
                    if time.monotonic() - t_frame > timeout:
                        raise RuntimeError("no new frame for %g s (after %d frames)" % (timeout, i))
                    continue
                a, seg = f.read()
                frames[i] = a
                cnt0[i], cnt2[i] = f.f("cnt0", seg), f.f("cnt2", seg)
                ts[i] = f.ts(seg)
                last = cnt0[i]
                i += 1
                rec["done"] = i
                t_frame = time.monotonic()
            rec.update(frames=frames, cnt0=cnt0, cnt2=cnt2, ts=ts, state="done", t1=time.time(),
                       gaps=int(np.sum(np.diff(cnt0.astype(np.int64)) - 1)) if n > 1 else 0)
        except Exception as e:
            rec.update(state="error", error=str(e), t1=time.time())
        finally:
            if shm is not None:
                shm.close()                           # gives its semaphore back

    @staticmethod
    def status(rec):
        return {k: rec[k] for k in ("id", "path", "n", "done", "state", "error", "t0", "t1", "gaps", "mode", "bytes")}

    def save(self, rec, out, fmt):
        if rec.get("state") != "done":
            raise ValueError("recording not finished")
        if fmt == "npz":
            np.savez(out, frames=rec["frames"], cnt0=rec["cnt0"], cnt2=rec["cnt2"], timestamp=rec["ts"])
        elif fmt == "fits":
            from astropy.io import fits
            hdu = fits.PrimaryHDU(rec["frames"])
            hdu.header["SHM"] = rec["path"]
            hdu.header["GAPS"] = rec["gaps"]
            tab = fits.BinTableHDU.from_columns([
                fits.Column(name="cnt0", format="K", array=rec["cnt0"].astype(np.int64)),
                fits.Column(name="cnt2", format="K", array=rec["cnt2"].astype(np.int64)),
                fits.Column(name="timestamp", format="D", array=rec["ts"])])
            fits.HDUList([hdu, tab]).writeto(out, overwrite=True)
        else:
            np.save(out, rec["frames"])


################################################
#           Tools: latency, snapshots, tmux, DAQ
################################################

LAT_PREFIX = "daoLatency_"


def run(cmd, timeout=10, cwd=None):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=cwd)
    return p.returncode, p.stdout, p.stderr


def tmux(*args, timeout=5):
    return run(["tmux", *args], timeout=timeout)


def tmux_sessions():
    rc, out, err = tmux("list-sessions", "-F",
                        "#{session_name}\t#{session_windows}\t#{session_attached}\t#{session_created}\t#{session_activity}")
    if rc != 0:
        if "no server running" in err or "No such file" in err or "error connecting" in err:
            return []
        raise RuntimeError(err.strip() or "tmux list-sessions failed")
    sessions = []
    for line in out.splitlines():
        p = line.split("\t")
        if len(p) == 5:
            sessions.append({"name": p[0], "windows": int(p[1]), "attached": int(p[2]),
                             "created": int(p[3]), "activity": int(p[4])})
    return sessions


def latency_name(path1, path2):
    """daoShmViewer's session name for a pair of SHMs (daoLatency_a_b): the same tool, the same session."""
    def b(p):
        n = os.path.basename(p)
        for suffix in (".im.shm", ".shm"):
            if n.endswith(suffix):
                n = n[:-len(suffix)]
                break
        return "".join(c if c.isalnum() else "_" for c in n)
    return "%s%s_%s" % (LAT_PREFIX, b(path1), b(path2))


def insert_suffix(path, suffix):
    return path[:-len(".im.shm")] + suffix + ".im.shm"


class Tools:
    def __init__(self, srv):
        self.srv = srv
        a = srv.args
        self.snapdir = os.path.realpath(os.path.expanduser(a.snapshot_dir))
        self.bins = {b: shutil.which(b) for b in ("tmux", "daoTimeDiff", "daoSnapshot.py")}
        try:
            import yaml  # noqa: F401
            self.yaml = True
        except ImportError:
            self.yaml = False

    def info(self):
        return {"tmux": bool(self.bins["tmux"]), "daoTimeDiff": bool(self.bins["daoTimeDiff"]),
                "daoSnapshot": bool(self.bins["daoSnapshot.py"]), "snapdir": self.snapdir,
                "daodata": os.environ.get("DAODATA", ""), "display": bool(os.environ.get("DISPLAY")),
                "yaml": self.yaml}

    # --- latency (daoTimeDiff in tmux, as daoShmViewer's tab)
    def meas_path(self, name):
        for d in self.srv.reg.dirs:
            p = os.path.join(d, name + ".im.shm")
            if os.path.exists(p):
                return p
        return os.path.join(self.srv.reg.dirs[0], name + ".im.shm")

    def latency_start(self, a, b, n):
        reg = self.srv.reg
        if not self.bins["daoTimeDiff"] or not self.bins["tmux"]:
            raise RuntimeError("needs daoTimeDiff and tmux in the PATH of the server")
        if not (reg.allowed(a) and reg.allowed(b)):
            raise PermissionError("not watched SHMs")
        n = max(2, min(100000, int(n)))
        name = latency_name(a, b)
        meas = os.path.join(reg.dirs[0], name + ".im.shm")
        cmd = shlex.join(["daoTimeDiff", "-S", a, b, meas, "-n", str(n), "-L"])
        tmux("kill-session", "-t", "=" + name)
        rc, out, err = tmux("new-session", "-d", "-s", name, cmd)
        if rc != 0:
            raise RuntimeError("tmux: " + err.strip())
        return {"name": name, "cmd": cmd, "meas": meas}

    def latency_list(self):
        out = []
        for s in tmux_sessions():
            if not s["name"].startswith(LAT_PREFIX):
                continue
            meas = self.meas_path(s["name"])
            rc, cmd, _ = tmux("list-panes", "-t", "=" + s["name"], "-F", "#{pane_start_command}")
            item = {"name": s["name"], "meas": meas, "cmd": cmd.strip().splitlines()[0] if rc == 0 and cmd.strip() else "",
                    "created": s["created"]}
            item.update(self.latency_values(meas, with_array=False))
            out.append(item)
        return out

    def latency_values(self, meas, with_array=True):
        reg = self.srv.reg
        fa, fr, fx = (reg.get(insert_suffix(meas, s)) for s in ("Avg", "Rms", "Array"))
        if not (fa and fr and fx):
            return {"ready": False}
        avg = float(fa.read()[0].ravel()[0])
        rms = float(fr.read()[0].ravel()[0])
        arr = fx.read()[0].ravel()
        n = int(min(fx.cnt0(), arr.size))
        v = {"ready": True, "avg": avg, "rms": rms, "n": n, "pop": int(arr.size), "cnt0": fx.cnt0(),
             "array": insert_suffix(meas, "Array")}
        if with_array:
            v["values"] = [float(x) for x in arr[:n]]
        return v

    def latency_stop(self, name):
        if not name.startswith(LAT_PREFIX):
            raise ValueError("not a latency session")
        rc, out, err = tmux("kill-session", "-t", "=" + name)
        if rc != 0:
            raise RuntimeError(err.strip())
        return "stopped " + name

    # --- snapshots (daoSnapshot.py)
    def snap_path(self, name):
        name = str(name)
        if not name or name.startswith(".") or not all(c.isalnum() or c in "_-." for c in name):
            raise ValueError("snapshot name: letters, digits, _ - . only")
        if not name.endswith(".dao"):
            name += ".dao"
        return os.path.join(self.snapdir, name)

    def snapshot_list(self):
        if not os.path.isdir(self.snapdir):
            return []
        out = []
        for p in sorted(glob.glob(os.path.join(self.snapdir, "*.dao")), key=os.path.getmtime, reverse=True):
            st = os.stat(p)
            out.append({"name": os.path.basename(p), "bytes": st.st_size, "mtime": st.st_mtime})
        return out

    def snapshot_run(self, what, name):
        if not self.bins["daoSnapshot.py"]:
            raise RuntimeError("daoSnapshot.py not in the PATH of the server")
        path = self.snap_path(name)
        if what == "create":
            os.makedirs(self.snapdir, exist_ok=True)
            if os.path.exists(path):
                raise ValueError("%s exists" % path)
        elif not os.path.isfile(path):
            raise ValueError("no snapshot %s" % path)
        rc, out, err = run([self.bins["daoSnapshot.py"], what, path], timeout=600, cwd=self.snapdir)
        tail = "\n".join((out + err).strip().splitlines()[-40:])
        if rc != 0:
            raise RuntimeError("daoSnapshot.py %s failed (exit %d)\n%s" % (what, rc, tail))
        self.srv.reg.scan()
        return {"path": path, "output": tail}

    # --- tmux
    def tmux_panes(self, name):
        rc, out, err = tmux("list-panes", "-s", "-t", "=" + name, "-F",
                            "#{window_index}.#{pane_index}\t#{window_name}\t#{pane_current_command}\t#{window_active}#{pane_active}")
        if rc != 0:
            raise RuntimeError(err.strip())
        panes = []
        for line in out.splitlines():
            p = line.split("\t")
            if len(p) == 4:
                panes.append({"id": p[0], "window": p[1], "cmd": p[2], "active": p[3] == "11"})
        return panes

    def tmux_capture(self, name, pane, lines):
        if name not in {s["name"] for s in tmux_sessions()}:
            raise ValueError("no tmux session %s" % name)
        panes = self.tmux_panes(name)
        if pane not in {p["id"] for p in panes}:
            pane = next((p["id"] for p in panes if p["active"]), panes[0]["id"] if panes else "0.0")
        lines = max(10, min(5000, int(lines)))
        rc, out, err = tmux("capture-pane", "-p", "-J", "-t", "=%s:%s" % (name, pane), "-S", str(-lines))
        if rc != 0:
            raise RuntimeError(err.strip())
        return {"name": name, "pane": pane, "panes": panes, "text": out.rstrip("\n")}

    def tmux_terminal(self, name):
        if name not in {s["name"] for s in tmux_sessions()}:
            raise ValueError("no tmux session %s" % name)
        if not os.environ.get("DISPLAY"):
            raise RuntimeError("the server has no display (DISPLAY): attach from a terminal instead")
        for term, pre in (("x-terminal-emulator", ["-e"]), ("gnome-terminal", ["--"]), ("konsole", ["-e"]),
                          ("xterm", ["-e"])):
            exe = shutil.which(term)
            if exe:
                subprocess.Popen([exe, *pre, "tmux", "attach", "-t", "=" + name], start_new_session=True,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return "terminal opened on the server's display (%s)" % term
        raise RuntimeError("no terminal emulator found on the server")

    # --- DAQ (daoDAQ through DAQClient, as daoShmViewer's tab)
    @staticmethod
    def daq_client():
        from daoDAQClient import DAQClient
        return DAQClient()

    def daq_state(self):
        try:
            return {"state": self.daq_client().state().name}
        except Exception as e:
            return {"state": None, "error": str(e)}

    @staticmethod
    def daq_check(config):
        if not isinstance(config, dict) or not isinstance(config.get("sources", []), list):
            raise ValueError("a DAQ configuration: {root_storage, sources: [...]}")
        for s in config.get("sources", []):
            uri = str(s.get("uri", ""))
            if not (uri.startswith("smem://") or uri.startswith("file://")):
                raise ValueError("source uri must be smem://... or file://...: %r" % uri)
        return config

    def daq_yaml(self, config):
        import yaml
        return yaml.safe_dump(self.daq_check(config), sort_keys=False)

    def daq_parse(self, text):
        import yaml
        return self.daq_check(yaml.safe_load(text) or {})

    def daq_start(self, config):
        c = self.daq_client()
        c.daq_session_configure_upload(self.daq_yaml(config))
        c.daq_session_configure_apply()
        c.daq_session_begin()
        return "DAQ session started"

    def daq_finish(self):
        self.daq_client().daq_session_finish()
        return "DAQ session finished"

    def daq_recover(self):
        self.daq_client().recover()
        return "DAQ recovered"


################################################
#           Web
################################################

class Server:
    def __init__(self, args):
        self.args = args
        self.reg = Registry(args.dir)
        self.clients = set()
        self.hostname = socket.gethostname()
        self.recorder = Recorder(args.record_max << 20)
        self.tools = Tools(self)

    def hello(self):
        return {"type": "hello", "host": self.hostname, "write": self.args.write, "dirs": self.reg.dirs,
                "pid": os.getpid(), "version": 2, "tools": self.tools.info()}

    def scan(self):
        self.reg.scan()
        if not self.clients:
            return
        msg = json.dumps({"type": "list", "items": self.reg.listing(), "now": time.time()})
        for c in list(self.clients):
            c.send_text(msg)

    def tick(self):
        now = time.monotonic()
        for c in list(self.clients):
            c.pump(now)


class Auth:
    def check_token(self, remember=False):
        token = self.application.settings.get("token")
        if not token:
            return True
        given = self.get_argument("token", None) or self.get_cookie("daoShmWebViewer")
        if given and secrets.compare_digest(given, token):
            if remember and self.get_argument("token", None):
                self.set_cookie("daoShmWebViewer", token, httponly=True, samesite="Strict")
            return True
        return False


class Page(Auth, tornado.web.RequestHandler):
    def get(self):
        if not self.check_token(remember=True):
            raise tornado.web.HTTPError(403, "token needed: ?token=...")
        self.set_header("Cache-Control", "no-cache")
        with open(os.path.join(self.application.settings["static_path"], "index.html"), "rb") as fh:
            self.write(fh.read())


class Api(Auth, tornado.web.RequestHandler):
    def prepare(self):
        if not self.check_token():
            raise tornado.web.HTTPError(403)
        self.srv = self.application.settings["srv"]

    def shm_path(self):
        path = self.get_argument("path")
        if not self.srv.reg.allowed(path):
            raise tornado.web.HTTPError(403, "not a watched SHM")
        return path


class Download(Api):
    def get(self):
        path = self.shm_path()
        f = self.srv.reg.get(path)
        if f is None or not f.readable:
            raise tornado.web.HTTPError(404)
        a, seg = f.read()
        fmt = self.get_argument("fmt", "npy")
        base = os.path.basename(path)[:-len(".im.shm")]
        stamp = time.strftime("%Y%m%dT%H%M%S")
        buf = io.BytesIO()
        if fmt == "fits":
            from astropy.io import fits
            hdu = fits.PrimaryHDU(a)
            hdu.header["SHM"] = path
            hdu.header["CNT0"] = f.f("cnt0", seg)
            hdu.header["CNT2"] = f.f("cnt2", seg)
            hdu.header["TS"] = f.ts(seg)
            hdu.writeto(buf)
        elif fmt == "csv":
            np.savetxt(buf, a.reshape(a.shape[0], -1) if a.ndim > 1 else a[:, None], delimiter=",", fmt="%.9g")
        else:
            fmt = "npy"
            np.save(buf, a)
        self.set_header("Content-Type", "application/octet-stream")
        self.set_header("Content-Disposition", 'attachment; filename="%s_%s_%d.%s"'
                        % (base, stamp, f.f("cnt0", seg), fmt))
        self.write(buf.getvalue())


class RecordDownload(Api):
    def get(self):
        r = self.srv.recorder.recs.get(self.get_argument("id"))
        if r is None or r["state"] != "done":
            raise tornado.web.HTTPError(404, "no such finished recording")
        fmt = self.get_argument("fmt", "npy")
        fmt = fmt if fmt in ("npy", "npz", "fits") else "npy"
        buf = io.BytesIO()
        self.srv.recorder.save(r, buf, fmt)
        name = "%s_%s_%dframes.%s" % (os.path.basename(r["path"])[:-len(".im.shm")],
                                       time.strftime("%Y%m%dT%H%M%S", time.localtime(r["t0"])), r["n"], fmt)
        self.set_header("Content-Type", "application/octet-stream")
        self.set_header("Content-Disposition", 'attachment; filename="%s"' % name)
        self.write(buf.getvalue())


class SnapshotDownload(Api):
    def get(self):
        path = self.srv.tools.snap_path(self.get_argument("name"))
        if not os.path.isfile(path):
            raise tornado.web.HTTPError(404)
        self.set_header("Content-Type", "application/octet-stream")
        self.set_header("Content-Disposition", 'attachment; filename="%s"' % os.path.basename(path))
        with open(path, "rb") as fh:
            self.write(fh.read())


class Load(Api):
    """POST the bytes of a .npy or .fits file: written to the SHM if shape and type match."""

    def post(self):
        if not self.srv.args.write:
            raise tornado.web.HTTPError(403, "read-only server (--write)")
        path = self.shm_path()
        name = self.get_argument("name", "file.npy")
        body = self.request.body
        shm = open_writable(path)
        try:
            cur = shm.get_data()
            if name.lower().endswith((".fits", ".fit", ".fts")):
                from astropy.io import fits
                data = None
                with fits.open(io.BytesIO(body)) as hdul:
                    found = []
                    for i, hdu in enumerate(hdul):
                        if getattr(hdu, "data", None) is not None:
                            found.append("HDU %d: %s %s" % (i, hdu.data.shape, hdu.data.dtype))
                            if data is None and hdu.data.shape == cur.shape:
                                data = np.array(hdu.data)
                if data is None:
                    raise ValueError("no HDU of shape %s; found %s" % (cur.shape, ", ".join(found) or "none"))
            else:
                data = np.load(io.BytesIO(body), allow_pickle=False)
            if data.size == cur.size and data.shape != cur.shape:
                data = data.reshape(cur.shape)
            if data.shape != cur.shape:
                raise ValueError("shape %s, the SHM has %s" % (data.shape, cur.shape))
            if data.dtype.kind != cur.dtype.kind and not self.get_argument("cast", None):
                raise ValueError("type %s, the SHM has %s (cast=1 to convert)" % (data.dtype, cur.dtype))
            shm.set_data(np.ascontiguousarray(data.astype(cur.dtype)))
            self.write({"ok": True, "info": "%s written from %s" % (path, name)})
        except Exception as e:
            self.set_status(400)
            self.write({"ok": False, "error": str(e)})
        finally:
            shm.close()


def open_writable(path):
    daoShm.setLogLevel(0)
    return daoShm.shm(path, logLevel=0)


class Socket(Auth, tornado.websocket.WebSocketHandler):
    """One page. Text messages: JSON both ways; binary: frames (server to page)."""

    def open(self):
        if not self.check_token():
            self.close(4003, "token")
            return
        self.srv = self.application.settings["srv"]
        self.subs = {}
        self.pending = 0
        self.srv.clients.add(self)
        self.send_text(json.dumps(self.srv.hello()))
        self.send_text(json.dumps({"type": "list", "items": self.srv.reg.listing(), "now": time.time()}))

    def on_close(self):
        if hasattr(self, "srv"):
            self.srv.clients.discard(self)

    def send_text(self, text):
        try:
            self.write_message(text)
        except tornado.websocket.WebSocketClosedError:
            self.srv.clients.discard(self)

    def reply(self, req, ok=True, **kw):
        self.send_text(json.dumps(dict(type="reply", req=req, ok=ok, **kw)))

    WRITE_OPS = {"set", "create", "record_save", "latency_start", "latency_stop", "snapshot_create",
                 "snapshot_load", "tmux_terminal", "daq_start", "daq_finish", "daq_recover"}

    async def blocking(self, fn, *a):
        return await tornado.ioloop.IOLoop.current().run_in_executor(EXEC, fn, *a)

    async def on_message(self, message):
        try:
            m = json.loads(message)
        except ValueError:
            return
        op, req = m.get("op"), m.get("req")
        if op in self.WRITE_OPS and not self.srv.args.write:
            self.reply(req, ok=False, error="read-only server: start it with --write")
            return
        tools, rec = self.srv.tools, self.srv.recorder
        try:
            if op == "record_start":
                f = self.srv.reg.get(m.get("path", ""))
                if f is None or not f.readable:
                    raise ValueError("no readable SHM %s" % m.get("path"))
                self.reply(req, id=rec.start(f, m.get("n", 10), m.get("timeout", 5)))
                return
            if op == "record_status":
                r = rec.recs.get(m.get("id"))
                if r is None:
                    raise ValueError("no such recording (expired?)")
                self.reply(req, rec=rec.status(r))
                return
            if op == "record_cancel":
                r = rec.recs.get(m.get("id"))
                if r is not None:
                    r["cancel"] = True
                self.reply(req)
                return
            if op == "record_save":
                r = rec.recs.get(m.get("id"))
                if r is None:
                    raise ValueError("no such recording (expired?)")
                out = os.path.expanduser(os.path.expandvars(str(m.get("file", ""))))
                if not os.path.isabs(out):
                    raise ValueError("an absolute path on the server")
                fmt = "npz" if out.endswith(".npz") else "fits" if out.endswith((".fits", ".fit")) else "npy"
                if fmt == "npy" and not out.endswith(".npy"):
                    out += ".npy"
                os.makedirs(os.path.dirname(out), exist_ok=True)
                await self.blocking(rec.save, r, out, fmt)
                self.reply(req, info="saved " + out)
                return
            simple = {
                "latency_list": lambda: {"sessions": tools.latency_list()},
                "latency_data": lambda: tools.latency_values(tools.meas_path(m["name"])),
                "latency_start": lambda: tools.latency_start(m["a"], m["b"], m.get("n", 100)),
                "latency_stop": lambda: {"info": tools.latency_stop(m["name"])},
                "snapshot_list": lambda: {"dir": tools.snapdir, "files": tools.snapshot_list()},
                "snapshot_create": lambda: tools.snapshot_run("create", m["name"]),
                "snapshot_load": lambda: tools.snapshot_run("load", m["name"]),
                "tmux_list": lambda: {"sessions": tmux_sessions()},
                "tmux_capture": lambda: tools.tmux_capture(m["name"], str(m.get("pane", "")), m.get("lines", 300)),
                "tmux_terminal": lambda: {"info": tools.tmux_terminal(m["name"])},
                "daq_state": tools.daq_state,
                "daq_yaml": lambda: {"text": tools.daq_yaml(m["config"])},
                "daq_parse": lambda: {"config": tools.daq_parse(m["text"])},
                "daq_start": lambda: {"info": tools.daq_start(tools.daq_check(m["config"]))},
                "daq_finish": lambda: {"info": tools.daq_finish()},
                "daq_recover": lambda: {"info": tools.daq_recover()},
            }
            if op in simple:
                self.reply(req, **(await self.blocking(simple[op])))
                return
        except Exception as e:
            self.reply(req, ok=False, error=str(e))
            return
        try:
            if op == "sub":
                s = self.subs.get(m["id"])
                opts = {k: m[k] for k in ("fps", "slice", "part", "maxEl", "paused") if k in m}
                if s is None or s["path"] != m["path"]:
                    self.subs[m["id"]] = {"path": m["path"], "opts": opts, "cnt": None, "t": 0.0, "force": True,
                                          "gone": False}
                else:
                    s["opts"].update(opts)
                    s["force"] = True
            elif op == "unsub":
                self.subs.pop(m.get("id"), None)
            elif op == "once":
                if m.get("id") in self.subs:
                    self.subs[m["id"]]["force"] = True
            elif op == "meta":
                f = self.srv.reg.get(m["path"])
                if f is None:
                    raise ValueError("no such SHM")
                self.reply(req, meta=f.meta())
            elif op == "sems":
                f = self.srv.reg.get(m["path"])
                if f is None:
                    raise ValueError("no such SHM")
                users = f.sems()
                self.reply(req, nsem=f.nsem, users=None if users is None else {str(k): v for k, v in users.items()})
            elif op in ("set", "create"):
                if not self.srv.args.write:
                    raise PermissionError("read-only server: start it with --write")
                self.reply(req, info=self.write_op(op, m))
            else:
                raise ValueError("unknown op %r" % (op,))
        except Exception as e:
            if req is not None:
                self.reply(req, ok=False, error=str(e))

    def write_op(self, op, m):
        reg = self.srv.reg
        if op == "create":
            name = str(m.get("name", ""))
            if not name or not all(c.isalnum() or c in "_-." for c in name):
                raise ValueError("name: letters, digits, _ - . only")
            d = m.get("dir") or reg.dirs[0]
            if os.path.realpath(d) not in reg.dirs:
                raise ValueError("not a watched directory")
            shape = tuple(int(s) for s in m["shape"])
            if not 1 <= len(shape) <= 3 or min(shape) < 1:
                raise ValueError("shape: 1 to 3 sizes >= 1")
            if len(shape) == 1:
                shape = (shape[0], 1)
            path = os.path.join(d, name + ".im.shm")
            if os.path.exists(path) and not m.get("overwrite"):
                raise ValueError("%s exists" % path)
            daoShm.setLogLevel(0)
            daoShm.shm(path, np.zeros(shape, np.dtype(m.get("dtype", "float32"))), logLevel=0).close()
            reg.scan()
            return "created %s %s %s" % (path, shape, m.get("dtype", "float32"))
        path = m["path"]
        if not reg.allowed(path):
            raise PermissionError("not a watched SHM")
        shm = open_writable(path)
        try:
            data = shm.get_data()
            mode = m.get("mode")
            if mode == "value":
                data = np.full_like(data, m["value"])
            elif mode == "random":
                lo, hi = float(m.get("min", 0)), float(m.get("max", 1))
                if np.issubdtype(data.dtype, np.integer):
                    data = np.random.randint(int(lo), int(hi) + 1, size=data.shape).astype(data.dtype)
                else:
                    data = np.random.uniform(lo, hi, size=data.shape).astype(data.dtype)
            elif mode == "elements":
                flat = data.reshape(-1)
                for i, v in zip(m["index"], m["values"]):
                    flat[int(i)] = v
            else:
                raise ValueError("mode: value, random or elements")
            shm.set_data(np.ascontiguousarray(data))
        finally:
            shm.close()
        return "%s: %s" % (path, mode)

    def pump(self, now):
        """Send each view its newest frame, when it changed and its rate allows."""
        if not self.subs or self.pending >= 2:
            return
        order = sorted(self.subs.items(), key=lambda kv: kv[1]["t"])
        for sid, s in order:
            if self.pending >= 2:
                return
            opts = s["opts"]
            f = self.srv.reg.files.get(s["path"])
            if f is None:
                if not s["gone"]:
                    s["gone"] = True
                    self.send_text(json.dumps({"type": "gone", "id": sid, "path": s["path"]}))
                continue
            if s["gone"]:
                s["gone"], s["force"] = False, True
            if not f.readable:
                if s["force"]:
                    s["force"] = False
                    self.send_text(json.dumps({"type": "unreadable", "id": sid, "path": s["path"],
                                               "gpu": f.gpu}))
                continue
            if opts.get("paused") and not s["force"]:
                continue
            fps = float(opts.get("fps", 20) or 20)
            if not s["force"] and now - s["t"] < 1.0 / fps:
                continue
            cnt = f.cnt0()
            if cnt == s["cnt"] and not s["force"]:
                continue
            try:
                msg = frame_message(sid, f, opts)
            except Exception as e:
                s["force"] = False
                self.send_text(json.dumps({"type": "error", "id": sid, "error": str(e)}))
                continue
            s["cnt"], s["t"], s["force"] = cnt, now, False
            self.pending += 1
            try:
                fut = self.write_message(msg, binary=True)
            except tornado.websocket.WebSocketClosedError:
                self.srv.clients.discard(self)
                return
            fut.add_done_callback(self._sent)

    def _sent(self, fut):
        self.pending = max(0, self.pending - 1)


def static_dir():
    here = os.path.dirname(os.path.realpath(__file__))
    for d in (os.path.join(here, "web", "daoShmViewer"),
              os.path.join(os.environ.get("DAOROOT", ""), "data", "web", "daoShmViewer"),
              os.path.join(here, "..", "data", "web", "daoShmViewer")):
        if os.path.isfile(os.path.join(d, "index.html")):
            return os.path.realpath(d)
    sys.exit("daoShmWebViewer: the page (web/daoShmViewer/index.html) was not found next to this script "
             "nor in $DAOROOT/data/web/daoShmViewer")


def main():
    p = argparse.ArgumentParser(description="daoShmViewer in a web browser")
    p.add_argument("--port", type=int, default=8090)
    p.add_argument("--host", default="127.0.0.1", help="address to listen on (0.0.0.0: every interface)")
    p.add_argument("--dir", action="append", help="directory of *.im.shm files (repeatable; default /tmp)")
    p.add_argument("--write", action="store_true",
                   help="allow changes from the page: SHMs (set / load / create), processes (latency, "
                        "snapshot load, DAQ sessions, terminals) and files (snapshots, saved recordings)")
    p.add_argument("--record-max", type=int, default=2048, help="largest quick record, MB (default 2048)")
    p.add_argument("--snapshot-dir", default=os.path.join(os.environ.get("DAODATA") or "~", "daoSnapshots"),
                   help="daoSnapshot.py files (default $DAODATA/daoSnapshots)")
    p.add_argument("--token", help="required as ?token=... (first visit) for every page and socket")
    p.add_argument("--open", action="store_true", help="open the page in a browser")
    args = p.parse_args()
    args.dir = args.dir or ["/tmp"]
    loopback = args.host in ("127.0.0.1", "localhost", "::1")
    if not loopback and not args.token:
        print("daoShmWebViewer: listening on %s without --token: anyone reaching this port sees the SHMs%s"
              % (args.host, " and can write them" if args.write else ""), file=sys.stderr)

    srv = Server(args)
    static = static_dir()
    app = tornado.web.Application([
        (r"/", Page),
        (r"/ws", Socket),
        (r"/api/download", Download),
        (r"/api/load", Load),
        (r"/api/record", RecordDownload),
        (r"/api/snapshot", SnapshotDownload),
    ], static_path=static, srv=srv, token=args.token, websocket_max_message_size=1 << 20,
        max_body_size=1 << 30)
    app.listen(args.port, address=args.host, max_body_size=1 << 30)
    tornado.ioloop.PeriodicCallback(srv.scan, 1000).start()
    tornado.ioloop.PeriodicCallback(srv.tick, 5).start()
    url = "http://%s:%d/%s" % ("localhost" if loopback or args.host == "0.0.0.0" else args.host, args.port,
                               "?token=" + args.token if args.token else "")
    print("daoShmWebViewer: %s  (%s, %s)" % (url, ", ".join(srv.reg.dirs), "read-write" if args.write else "read-only"))
    if args.open:
        import webbrowser
        webbrowser.open(url)
    try:
        tornado.ioloop.IOLoop.current().start()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
