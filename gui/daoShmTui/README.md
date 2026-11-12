# daoShmTui

The DAO shared memory viewer in a terminal (Rust, [ratatui](https://ratatui.rs)): over ssh, in
tmux, on a machine without a display.

```
daoShmTui                          # the SHMs of /tmp
daoShmTui -d /tmp -d /dev/shm/loop shIm
daoShmTui --fps 30 --cmap inferno
```

**Read-only by default.** The SHM files are mapped read-only: no semaphore is taken, nothing is
written to them, a GPU SHM is never attached (its `/tmp` host copy is read when it keeps one,
`DAO_GPU_MIRROR`). A frame is read when `cnt0` changes, at most `--fps` times a second (20 by
default). No libdao is needed to view: the metadata layout (`IMAGE_METADATA`, dao.h) is mirrored
and checked by `cargo test`.

**`--write`** allows changes: SHMs (set every element, random values, edit the cursor's value,
load a `.npy`, create an SHM), published through libdao (`daoShmSetData`: cnt0, timestamp,
semaphores, as every writer), which is then loaded at run time; and the tools that start
processes (latency measurements, snapshot loads, DAQ sessions) or write snapshots. Every change
to an SHM asks for a confirmation.

## Build and install

`waf configure build install` (daoTools) builds and installs it when `cargo` and `rustc` >= 1.75
are found (`waf configure --without-rust`: never). A failed cargo build (offline, crates never
fetched) only warns: the rest of daoTools still builds. By hand:

```
cd gui/daoShmTui
cargo build --release --locked                             # target/release/daoShmTui
cargo install --path . --root $DAOROOT --locked            # $DAOROOT/bin/daoShmTui
```

`Cargo.lock` pins the dependency versions that build with Rust 1.75 (ratatui 0.28); keep
`--locked`. Images are drawn in true colour when `COLORTERM` says so, else with the 256-colour
palette (`--256` forces it).

## What it shows

- **The SHMs** of the watched directories, rescanned every second: shape, type, update rate;
  filter (`/`), sort by name, rate or size (`s`).
- **Image**: two pixels per character cell (half blocks), colour maps (viridis, inferno, magma,
  plasma, turbo, gray, hot, coolwarm; reversed), linear / sqrt / log / asinh, range min–max,
  99.9 / 99.5 / 99 %, ±max or locked, rotation and flips, zoom on a cursor, the values drawn in
  the pixels once they are wide enough, colour bar.
- **Vectors**: a line (braille) or bars. **Scalar**: a large number. **Table**: the values, the
  cursor's cell highlighted. **Cubes**: one slice (`[` `]`). **Complex**: |z|, real, imag, phase.
- **Average** of the last N frames, **difference** to a reference frame.
- **Strip**: the trend of the mean, std, min / mean / max, the cursor pixel or the frames
  skipped (Δcnt0); or the histogram of the frame.
- **Status**: cnt0, cnt2, the update rate, frames shown per second, time since the last write,
  min / max / mean / std, the cursor's value.
- **Info** (`i`): every metadata field, the semaphores and the PID and command of the reader
  holding each.
- **Save** (`w`): the frame as `.npy`, in the SHM's own type and shape.
- **Write** (`--write`, view): `e` the cursor's value, `V` every element, `R` random values in
  [min, max], `L` a `.npy` file (converted to the SHM's type), `N` a new SHM (name shape type).

The tabs (`1`–`7`), as daoShmViewer's:

| Tab | |
|---|---|
| 1 View | the selected SHM, as above |
| 2 Semaphores | the semaphores of the selected SHM, in columns of 8: free, or the PID and command of the reader holding it |
| 3 Record | N consecutive frames (polled read-only) with their cnt0, cnt2 and timestamps, missed frames counted; saved as `.npy` (frames) or `.npz` (frames, cnt0, cnt2, timestamp) |
| 4 Latency | `daoTimeDiff -S` between two SHMs (`a`, `b`: the list's selection) in its own tmux session, the same session name as daoShmViewer's; AVG / RMS, scatter, histogram; `v` shows its Array SHM |
| 5 Snapshots | `daoSnapshot.py create` / `load` (`--snapshot-dir`, default `$DAODATA/daoSnapshots`) |
| 6 tmux | the sessions and their output (read-only); `Enter` attaches (Ctrl-b d comes back; inside tmux: switch-client) |
| 7 DAQ | daoShmViewer's DAQ tab: root storage, SHM and file sources with their options, YAML export / import, start / finish / recover through `daoDAQCli.py` |

`?` in the viewer lists the keys.

## Code

| File | |
|---|---|
| `src/shm.rs` | SHM files: read-only map, metadata, newest frame (torn reads retried), directory scan, semaphore users (open-file-description locks + `/proc`), `.npy` |
| `src/cmap.rs` | colour maps, scales |
| `src/app.rs` | state, frame processing (average, reference, statistics), keys |
| `src/ui.rs` | drawing |
| `src/tabs.rs` | the tabs, prompts, confirmations, the writes |
| `src/tools.rs` | record, latency, snapshots, tmux, DAQ (YAML, daoDAQCli.py), value conversions, `.npy` / `.npz` |
| `src/dao.rs` | libdao, loaded at run time for `--write` |
| `src/main.rs` | arguments, terminal, event loop, tmux attach |
