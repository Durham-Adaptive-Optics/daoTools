# daoDAQ - recording shared memories to disk

daoDAQ records dao shared-memory (SHM) streams to FITS files. You tell it
which SHMs to watch, press start, and it writes every new frame of each SHM
until you stop it or until each SHM has recorded its requested number of
frames.

It runs as a server (a daoBase component) that you control over the network
with:

- **`daoDAQCtrl.py`**: a GUI with full control (see [GUI](#gui-daodaqctrlpy)).
- **`daoDAQCli.py`**: a command-line client, for scripts.
- **`daoDAQClient`**: the Python library that both of them use.
- **`daoShmViewer.py`**, DAQ tab: a quick form to build a configuration and
  start or finish a recording.

The rest of this page covers:
- [Quick start](#quick-start)
- [Configuration file](#configuration-file)
- [States](#states)
- [GUI: daoDAQCtrl.py](#gui-daodaqctrlpy)
- [Command line: daoDAQCli.py](#command-line-daodaqclipy)
- [Python: daoDAQClient](#python-daodaqclient)
- [Output](#output)
- [Extracting telemetry](#extracting-telemetry)
- [Performance and tuning](#performance-and-tuning)
- [Limitations and known issues](#limitations-and-known-issues)
- [Troubleshooting](#troubleshooting)

---

## Quick start

```bash
export DAODATA=/data/dao          # required: daoDAQ refuses to start without it
daoDAQCtrl.py                     # GUI: Launch daoDAQ, pick a configuration, Apply, START
```

Or from a terminal:

```bash
daoDAQ &                                     # server, logs to $DAODATA/daoDAQ.logs
daoDAQCli.py upload session.yaml             # send the configuration
daoDAQCli.py apply                           # parse it and attach the SHMs -> ready
daoDAQCli.py acquire --wait                  # record, block until done (needs `samples`)
```

A minimal `session.yaml`:

```yaml
root_storage: ${DAOROOT}/telemetry # the default; created when daoTools is installed
sources:
  - uri: smem:///tmp/wfs.im.shm
    samples: 10000
```

This records the next 10 000 frames of `/tmp/wfs.im.shm` into
`$DAOROOT/telemetry/<date>_<time>/wfs.fits`.

---

## Running the server

```
daoDAQ [-c FILE] [-p PORT] [-s] [-v | -vv] [--version]

  -c, --daq-configuration FILE   configuration to upload at start-up
  -s, --stdout-logging           log to the terminal instead of $DAODATA/daoDAQ.logs
  -v / -vv                       DEBUG / TRACE logging (default INFO)
  -p, --port PORT                TCP port of the control server (default 62000)
```

- **Required:** the `DAODATA` environment variable must be set.
- **Port:** the server listens on TCP port **62000** unless `-p` says
  otherwise. Clients then need the same port: `daoDAQCli.py --port`,
  `daoDAQCtrl.py --port`. The DAQ tab of `daoShmViewer.py` only uses 62000.
- **Stopping:** press Ctrl-C or send SIGINT. This stops any acquisition in
  progress, releases the SHMs and exits.
- **Start-up configuration:** `-c` only uploads the file. The server still
  starts in `Off`, and you apply the configuration the usual way.

---

## Configuration file

A YAML document with a storage root and a list of sources:

```yaml
root_storage: ${DAOROOT}/telemetry # required, directory must exist; $VAR / ${VAR} expanded

sources:                           # required, at least one
  - uri: smem:///tmp/wfs.im.shm    # required, smem://<absolute path of the SHM>
    samples: 10000                 # stop this source after N frames
    file_rollover: 1000            # start a new FITS file every N frames
    buffer_limit: 500              # max frames queued in RAM before dropping
    daq_affinity: 4                # CPU core for the capture thread
    sink_affinity: 5               # CPU core for the disk-writer thread
    eager_start: true              # also record the frame already in the SHM at START

  - uri: smem:///tmp/dm.im.shm     # sources are independent of each other

  - uri: file:///home/ao/calib/im.fits   # copied into the session directory at START
```

**The default `root_storage` is `${DAOROOT}/telemetry`.** The installed
`default.yaml` and *New* in the GUI both use it, and installing daoTools
creates the folder.

`root_storage` may use environment variables (`$NAME` or `${NAME}`). They
are expanded from **daoDAQ's** environment. An unset variable puts the server
in Error when the configuration is applied.

daoDAQ itself never creates `root_storage`: if the folder is missing, START
fails. When daoDAQ runs on the same machine, `daoDAQCtrl.py` creates it on
*Apply*.

There are two kinds of source:
- **`smem://`** records the frames of a shared memory. All the parameters
  below apply to it.
- **`file://`** copies a file (a calibration, the loop configuration, …)
  into the session directory once, at START. It takes no other parameter,
  and it counts as done as soon as the copy succeeds.

| Parameter | Default | Meaning |
|---|---|---|
| `uri` | (required) | `smem://` or `file://` followed by an absolute path, so there are three slashes. Each path may appear only once. |
| `samples` | unlimited | The source stops after writing this many frames. When **every** `smem://` source has `samples`, the session ends by itself; otherwise it runs until STOP. |
| `file_rollover` | one file | Frames per FITS file. With rollover, files go to a subdirectory: `<session>/<name>/<name>_0.fits`, `_1.fits`, and so on. |
| `buffer_limit` | unlimited | Maximum number of frames waiting in RAM between capture and disk. When the queue is full, new frames are **dropped** (with a warning in the log). **Always set it**: without a limit, a disk that can't keep up makes RAM use grow until the machine runs out of memory. |
| `daq_affinity`, `sink_affinity` | unpinned | Pin the two threads of this source to CPU cores (Linux only). |
| `eager_start` | `true` | `true`: the frame already in the SHM when START is pressed is recorded first. `false`: only frames written after START are recorded. |
| `format` | `fits` | Output format; `fits` is the only one. |
| `metadata_only` | `false` | Accepted but **not implemented**: full frames are always written. |

**Unknown keys are silently ignored**, so a misspelled parameter name has no
effect. `daoDAQCtrl.py` warns about them. A wrong value type or a missing SHM
only shows up when the configuration is applied, and a missing `file://` file
only shows up at START (see [States](#states)).

---

## States

daoDAQ uses the standard daoBase component state machine. The GUI and the
client library use friendlier names for the same states:

| daoBase state | Client name | Meaning |
|---|---|---|
| `Off` | Unconfigured | No configuration applied. **A configuration can be uploaded here.** |
| `Standby` | Configured | Configuration parsed. |
| `Idle` | Ready | SHMs attached, threads waiting. Press START here. |
| `Running` | Acquiring | Recording. |
| `Error` | Error | Something failed. **A configuration can also be uploaded here.** |

```
           Init              Enable              Run
   Off ──────────▶ Standby ──────────▶ Idle ──────────▶ Running
       ◀──────────         ◀──────────      ◀──────────
           Stop              Disable            Idle (STOP, or all `samples` reached)

   Idle / Running ──(failure)──▶ Error ──Recover──▶ Idle
```

| Transition | What daoDAQ does |
|---|---|
| Init | Parses the uploaded YAML. A bad file puts the server in Error. |
| Enable | Opens every SHM and starts two threads per source. A missing SHM puts the server in Error. |
| Run | Creates `<root_storage>/<YYYY-mm-dd_HH-MM-SS>/`, copies the `file://` sources into it and starts recording. |
| Idle | Stops recording. The files are complete and closed. |
| Disable | Stops the threads and releases the SHMs. |
| Stop | Unloads the configuration. |
| Recover | Parses the uploaded configuration **again** and re-attaches the SHMs. So, to get out of Error: upload a corrected configuration, then Recover. |

Two consequences:
- To **change the configuration**, the server first has to go back to `Off`.
  `daoDAQCli.py upload` and the GUI's *Apply* do this for you.
- **The server replies "success" to every command,** even one that was
  refused or failed. The clients check the state after each command. The
  reason for an Error is only written to `$DAODATA/daoDAQ.logs`.
  `daoDAQCtrl.py` shows it when daoDAQ runs on the same machine.

---

## GUI: daoDAQCtrl.py

```bash
daoDAQCtrl.py [--host HOST] [--port 62000] [--config FILE] [--config-dir DIR] [--light]
```

It has two tabs, **Acquire** (below) and **Extract**, with an activity log
under them. The Extract tab is described in
[Extracting telemetry](#extracting-telemetry).

The Acquire tab has three panels:

**Server**
- Shows the live state, colour-coded, and the server's PID.
- **Launch daoDAQ** starts a local server, which keeps running after the GUI
  closes.
- **Shut down** sends it SIGINT, after asking for confirmation if a recording
  is in progress.
- Launch and Shut down only work when the host is this machine.

**Configuration**
- **File row:** the drop-down lists the YAML files in `--config-dir`
  (default `$DAOROOT/config/daq`, where `default.yaml` is installed).
  - *New* starts an empty configuration, with `root_storage: ${DAOROOT}/telemetry`.
  - *Load…* opens any other file, and *Reload* re-reads the current one.
  - *Save* and *Save as…* write the configuration.
- **root_storage:** a field with a folder picker.
- **Sources tab:**
  - On the left, the SHMs found in `/tmp` (`*.im.shm`, as in
    `daoShmViewer`), with a search box. **Double-click an SHM** (or select
    several and press *Add selected →*) to record it. It is added with
    `buffer_limit: 1000`. SHMs already recorded are shown in bold green.
  - On the right, the recorded sources with their parameters and a status:
    SHM missing, bad value, unknown key, and so on. Select one to edit it in
    the form (`samples`, `file_rollover`, `buffer_limit`, affinities,
    `eager_start`). **Double-click a row, or press *Remove*, to drop it.**
    *Add file…* adds a `file://` source.
- **YAML tab:** the same configuration as text, for hand editing. Both tabs
  stay in sync. A form edit rewrites the YAML, so comments in a hand-written
  file are lost from that point on. While the YAML doesn't parse, the Sources
  tab is disabled.
- **Checks:** every change is checked the way daoDAQ reads it. When daoDAQ is
  local, the checks also cover whether the SHMs and the `file://` files
  exist. A missing `root_storage` is only a warning, because *Apply* creates
  it.
- **Apply to daoDAQ** uploads the configuration, including unsaved edits,
  and takes daoDAQ to *Ready*. From *Error*, it uploads and then runs Recover.

**Acquisition**
- **START** and **STOP** record.
- **Release** unloads the configuration and frees the SHMs.
- **Recover** leaves *Error* using the configuration already uploaded.
- While recording, the panel shows the session directory, the elapsed time,
  and the number of FITS files and MB written. **Open folder** opens the
  directory. Sessions started by another client are followed too, because the
  state is polled.

Buttons are enabled only in the states where they can succeed.

The DAQ tab of `daoShmViewer.py` is lighter: it builds a configuration from a
form, then uses one Start/Finish button against the local server.

---

## Command line: daoDAQCli.py

```bash
daoDAQCli.py [--host 127.0.0.1] [--port 62000] COMMAND
```

| Command | Effect |
|---|---|
| `ping` | Check that the server answers. |
| `state` | Print the state. |
| `upload FILE` | Send a configuration. If needed, first takes the server back to Off. |
| `apply` | Off → Ready: parse the configuration and attach the SHMs. |
| `acquire [--wait]` | START. `--wait` blocks until the session ends by itself. |
| `wait` | Block until the current session ends by itself. |
| `finish` | STOP. |
| `recover` | Error → Ready. |

A typical script:

```bash
daoDAQCli.py upload calib.yaml && daoDAQCli.py apply && daoDAQCli.py acquire --wait
```

## Python: daoDAQClient

```python
from daoDAQClient import DAQClient, DAQState

daq = DAQClient("127.0.0.1", 62000, timeout_s=5)
daq.daq_session_configure_upload(open("session.yaml").read())
daq.daq_session_configure_apply()         # -> DAQState.Ready
daq.daq_session_begin()
daq.daq_session_await_finish(timeout=60)  # sessions where every source has `samples`
# or: daq.daq_session_finish()            # STOP
print(daq.state())
```

Every method raises `RuntimeError` when the server doesn't reach the
expected state.

---

## Output

```
<root_storage>/
└── 2026-09-23_17-26-17/          one directory per START (local time)
    ├── wfs.fits                  source without file_rollover
    ├── im.fits                   file:// source, copied as is
    └── dm/                       source with file_rollover
        ├── dm_0.fits
        ├── dm_1.fits
        └── ...
```

The file name is the SHM's name without its extension: `/tmp/wfs.im.shm`
becomes `wfs`.

**Each frame is its own FITS image (HDU)**: the first frame is the primary
HDU, and each following frame is an extension. Every HDU carries:

| Keyword | Content |
|---|---|
| `ATYPE` | dao data type of the SHM |
| `ATIME` | the frame's write timestamp, ns since the Unix epoch |
| `CNT0` | frame counter of the SHM (increases by 1 for each frame written) |
| `CNT1`, `CNT2` | the SHM's other counters |

The axes are reversed, so FITS readers see the array with dao's row-major
shape: `(rows, cols)` → `NAXIS1 = cols`, `NAXIS2 = rows`. `astropy` returns the
original shape.

Reading a session back:

```python
import glob
import os
import numpy as np
from astropy.io import fits

frames, cnt0, t_ns = [], [], []
for path in sorted(glob.glob(os.path.expandvars("$DAOROOT/telemetry/2026-09-23_17-26-17/dm/dm_*.fits")),
                   key=lambda p: int(p.rsplit("_", 1)[1].split(".")[0])):
    with fits.open(path) as hdus:
        for hdu in hdus:
            frames.append(hdu.data)
            cnt0.append(hdu.header["CNT0"])
            t_ns.append(hdu.header["ATIME"])

frames = np.stack(frames)                 # (n_frames, rows, cols)
missed = np.diff(cnt0) - 1                # > 0 where frames were not recorded
print(f"{len(frames)} frames, {missed.clip(0).sum()} missed")
```

Sort rollover files numerically: an alphabetical sort puts `dm_10` before `dm_2`.

Gaps in `CNT0` show which frames were not recorded. Frames go missing in
three cases:
- the SHM was written faster than daoDAQ could copy,
- a frame was overwritten while being copied,
- the queue reached `buffer_limit`.

The last two are logged as warnings in `$DAODATA/daoDAQ.logs`.

---

## Extracting telemetry

To get the frames recorded between two times, as one array per source:

**GUI:** the *Extract* tab of `daoDAQCtrl.py`.
1. `root_storage` is filled from the current configuration. Opening the tab
   scans it, and *Scan* rescans. It is rescanned automatically after each
   recording.
2. The tree lists each session and its sources, with frame counts, first and
   last frame times, and the frame shape and type. Tick the sources to
   extract; the newest session is ticked by default.
3. Set the range in *from* / *to* (local time, inclusive, to the
   millisecond). Double-click a session or a source to use its time span, or
   press *Fit to ticked*. The panel shows how many frames and MB the range
   holds.
4. Choose the output file and format, then press *EXTRACT*. The extraction
   runs in the background with a progress bar and can be cancelled.

**Command line:** `daoDAQExtract.py`.

```bash
daoDAQExtract.py $DAOROOT/telemetry --list                              # sessions, sources, time spans
daoDAQExtract.py $DAOROOT/telemetry --start 17:40 --end 17:45 # what that range holds
daoDAQExtract.py $DAOROOT/telemetry --start 17:40 --end 17:45 -o wfs.h5 --source wfs
daoDAQExtract.py $DAOROOT/telemetry --start "2026-09-23 17:40:05.250" --end "2026-09-23 17:40:06" \
                 --session 2026-09-23_17-39-50 -o slice.fits
```

- **Times:** `2026-09-23 17:40:05.250`, `17:40` (today), ISO 8601 with an
  offset, or epoch seconds.
- **Time zone:** local time, or UTC with `--utc`.
- **The range is inclusive,** and an end time written to the millisecond
  includes that whole millisecond. So times copied from `--list` include the
  frames they show.
- **Without `-o`,** it only prints how many frames the range holds.

**Python:** `daoDAQExtractor`.

```python
import os
from daoDAQExtractor import scan, extract, parse_time, write

catalog = scan(os.path.expandvars("$DAOROOT/telemetry"))
t0, t1 = parse_time("17:40"), parse_time("17:45", end=True)
result = extract(catalog, t0, t1, sources=["wfs"])
result["wfs"].data        # (n_frames, rows, cols), original dtype
result["wfs"].atime       # int64 ns since the epoch; also cnt0, cnt1, cnt2
write(result, "wfs.h5")
```

**Output formats**

| Extension | Content |
|---|---|
| `.h5` (HDF5, needs `h5py`) | One group per source: `data`, `atime`, `cnt0`, `cnt1`, `cnt2`. |
| `.fits` | For each source, an image cube `<SOURCE>` and a table `<SOURCE>_TIME` (ATIME, CNT0–2). |
| `.npz` | Arrays `<source>.data`, `<source>.atime`, `<source>.cnt0`, … |

Across the output:
- Frames keep their original data type.
- Frames are in time order.
- A source recorded in several sessions is merged into one array. If its
  shape or type changed between sessions, each session's part is kept
  separate, as `<source>@<session>`.
- `file://` copies in the session folders are ignored, even FITS ones.

**Speed**

The extractor reads only a few headers per file and the frames in the
range. It relies on every frame of a daoDAQ file having the same size, and
binary-searches on `ATIME`. For example, a 1 s slice of a 288 MB,
50 000-frame file took **8 ms**, against 9.3 s for astropy reading every
frame.

`daoDAQParser.py` loads whole sessions into memory. Use it for small
sessions only.

**Memory**

The extracted frames are held in memory before being written. The GUI
warns above 4 GB.

---

## Performance and tuning

- Each source has **two threads**:
  - the **capture thread** waits for `cnt0` to change, then copies the frame
    into RAM;
  - the **writer thread** writes the queued frames to disk.

  Both threads **poll continuously** instead of sleeping, so **each source
  keeps two CPU cores fully busy while recording**. Pin them with
  `daq_affinity` and `sink_affinity` to cores that the real-time pipeline
  doesn't use.
- A slow disk doesn't make the capture miss frames: frames wait in RAM
  instead. Use `buffer_limit` to cap RAM use, at the cost of dropping frames
  once the queue is full.
- FITS files have one HDU per frame, so very long runs make very large
  files. `file_rollover` keeps each file to a manageable size.

---

## Limitations and known issues

| Issue | Effect |
|---|---|
| `metadata_only` isn't implemented | Full frames are written. |
| Complex data types | SHMs of type complex float or complex double are rejected when applying the configuration. |
| Session directory name has one-second resolution | Two STARTs within the same second fail: the directory already exists, so the server goes to Error. |
| Error during Enable | If an SHM is missing, the sources opened before it stay allocated. Recover after fixing the configuration works (tested), but restarting daoDAQ is the cleanest recovery if it behaves oddly afterwards. |

---

## Troubleshooting

| Symptom | Check |
|---|---|
| `ERROR: DAODATA directory must be present!` | `export DAODATA=...` before starting daoDAQ. |
| Clients time out | Is daoDAQ running? `ss -ltnp \| grep 62000` shows it: its process is listed as `ZMQ_server`, so `pgrep daoDAQ` doesn't find it. Do the client and server use the same port? Is the port reachable (firewall)? |
| Apply puts the server in Error | The reason is in `tail $DAODATA/daoDAQ.logs`. Usually one of: an SHM path that doesn't exist, a typo in a parameter value, `root_storage` missing, or YAML syntax. |
| START puts the server in Error | Is `root_storage` writable? Does every `file://` file exist? Was there a second START within the same second? |
| Files have fewer frames than expected | Look for "dropped sample" warnings in the log. Set `buffer_limit`, pin the threads, or write to a faster disk. |
| Session never ends by itself | Every `smem://` source needs `samples`. Otherwise press STOP. |

---

Source code (`src/`):

| File | Contents |
|---|---|
| `main.cpp` | Command-line options |
| `server.cpp` | State machine and sessions |
| `configuration.cpp` | YAML parsing |
| `daqs/smem.cpp` | SHM capture |
| `sinks/fits.cpp` | FITS writer |

Clients: `daoTools/src/python/daoDAQClient.py`, `daoTools/apps/daoDAQCli.py`,
`daoTools/gui/daoDAQCtrl.py`. Extraction: `daoTools/src/python/daoDAQExtractor.py`,
`daoTools/apps/daoDAQExtract.py`.
