//! The daoShmViewer tools: quick record, SHM-to-SHM latency (daoTimeDiff in tmux), snapshots
//! (daoSnapshot.py), tmux sessions, DAQ (daoDAQCli.py), and the value conversions the writes
//! need.

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crate::shm::{now_epoch, DType, Part, ShmFile};

/* ------------------------------------------------------------------ commands */

/// (exit code, stdout, stderr); Err: could not start
pub fn run(prog: &str, args: &[&str]) -> Result<(i32, String, String), String> {
    let out = Command::new(prog).args(args).output().map_err(|e| format!("{}: {}", prog, e))?;
    Ok((out.status.code().unwrap_or(-1), String::from_utf8_lossy(&out.stdout).into_owned(), String::from_utf8_lossy(&out.stderr).into_owned()))
}

pub fn which(prog: &str) -> bool {
    std::env::var_os("PATH")
        .map(|p| std::env::split_paths(&p).any(|d| d.join(prog).is_file()))
        .unwrap_or(false)
}

/* ------------------------------------------------------------------ tmux */

#[derive(Clone)]
pub struct Session {
    pub name: String,
    pub windows: u32,
    pub attached: bool,
    pub created: f64,
}

pub fn tmux_sessions() -> Vec<Session> {
    match run("tmux", &["list-sessions", "-F", "#{session_name}\t#{session_windows}\t#{session_attached}\t#{session_created}"]) {
        Ok((0, out, _)) => out
            .lines()
            .filter_map(|l| {
                let p: Vec<&str> = l.split('\t').collect();
                if p.len() == 4 {
                    Some(Session {
                        name: p[0].into(),
                        windows: p[1].parse().unwrap_or(0),
                        attached: p[2] != "0",
                        created: p[3].parse().unwrap_or(0.0),
                    })
                } else {
                    None
                }
            })
            .collect(),
        _ => Vec::new(),
    }
}

/// the last lines of a session's active pane
pub fn tmux_capture(name: &str, lines: u32) -> String {
    // a pane target: "=session:" (exact session name, its active window and pane)
    match run("tmux", &["capture-pane", "-p", "-J", "-t", &format!("={}:", name), "-S", &format!("-{}", lines)]) {
        Ok((0, out, _)) => out.trim_end().to_string(),
        Ok((_, _, err)) => err.trim().to_string(),
        Err(e) => e,
    }
}

/* ------------------------------------------------------------------ latency */

pub const LAT_PREFIX: &str = "daoLatency_";

fn shm_base(p: &Path) -> String {
    let n = p.file_name().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    let n = n.trim_end_matches(".im.shm").trim_end_matches(".shm");
    n.chars().map(|c| if c.is_alphanumeric() { c } else { '_' }).collect()
}

/// daoShmViewer's session name for a pair (daoLatency_a_b): the same tool, the same session
pub fn latency_name(a: &Path, b: &Path) -> String {
    format!("{}{}_{}", LAT_PREFIX, shm_base(a), shm_base(b))
}

pub fn latency_start(dir: &Path, a: &Path, b: &Path, n: u32) -> Result<String, String> {
    if !which("daoTimeDiff") || !which("tmux") {
        return Err("needs daoTimeDiff and tmux in the PATH".into());
    }
    let name = latency_name(a, b);
    let meas = dir.join(format!("{}.im.shm", name));
    let cmd = format!(
        "daoTimeDiff -S '{}' '{}' '{}' -n {} -L",
        a.display(),
        b.display(),
        meas.display(),
        n.max(2)
    );
    let _ = run("tmux", &["kill-session", "-t", &format!("={}", name)]);
    match run("tmux", &["new-session", "-d", "-s", &name, &cmd])? {
        (0, _, _) => Ok(name),
        (_, _, err) => Err(format!("tmux: {}", err.trim())),
    }
}

pub fn latency_stop(name: &str) -> Result<(), String> {
    match run("tmux", &["kill-session", "-t", &format!("={}", name)])? {
        (0, _, _) => Ok(()),
        (_, _, err) => Err(err.trim().to_string()),
    }
}

/// the measurement SHM of a latency session: in a watched directory
pub fn latency_meas(dirs: &[PathBuf], name: &str) -> PathBuf {
    for d in dirs {
        let p = d.join(format!("{}.im.shm", name));
        if p.exists() {
            return p;
        }
    }
    dirs[0].join(format!("{}.im.shm", name))
}

pub fn with_suffix(meas: &Path, suffix: &str) -> PathBuf {
    let s = meas.to_string_lossy();
    PathBuf::from(format!("{}{}.im.shm", s.trim_end_matches(".im.shm"), suffix))
}

/* ------------------------------------------------------------------ snapshots */

pub struct SnapFile {
    pub name: String,
    pub bytes: u64,
    pub mtime: f64,
}

pub fn snapshots(dir: &Path) -> Vec<SnapFile> {
    let mut v: Vec<SnapFile> = std::fs::read_dir(dir)
        .map(|rd| {
            rd.flatten()
                .filter(|e| e.path().extension().map(|x| x == "dao").unwrap_or(false))
                .filter_map(|e| {
                    let m = e.metadata().ok()?;
                    let mtime = m.modified().ok()?.duration_since(std::time::UNIX_EPOCH).ok()?.as_secs_f64();
                    Some(SnapFile { name: e.file_name().to_string_lossy().into_owned(), bytes: m.len(), mtime })
                })
                .collect()
        })
        .unwrap_or_default();
    v.sort_by(|a, b| b.mtime.partial_cmp(&a.mtime).unwrap_or(std::cmp::Ordering::Equal));
    v
}

pub fn valid_name(n: &str) -> bool {
    !n.is_empty() && !n.starts_with('.') && n.chars().all(|c| c.is_alphanumeric() || "_-.".contains(c))
}

/// daoSnapshot.py create|load in a thread; the result goes to `out`
pub fn snapshot_run(what: &'static str, dir: PathBuf, name: String, out: Arc<Mutex<Option<String>>>) {
    std::thread::spawn(move || {
        let path = dir.join(if name.ends_with(".dao") { name.clone() } else { format!("{}.dao", name) });
        let msg = (|| -> Result<String, String> {
            if !which("daoSnapshot.py") {
                return Err("daoSnapshot.py is not in the PATH".into());
            }
            std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
            let p = path.to_string_lossy().into_owned();
            let (code, so, se) = run("daoSnapshot.py", &[what, &p])?;
            let tail: Vec<&str> = so.lines().chain(se.lines()).collect();
            let tail = tail[tail.len().saturating_sub(3)..].join(" | ");
            if code == 0 {
                Ok(format!("daoSnapshot.py {} {}: done", what, p))
            } else {
                Err(format!("daoSnapshot.py {} failed ({}): {}", what, code, tail))
            }
        })();
        *out.lock().unwrap() = Some(msg.unwrap_or_else(|e| e));
    });
}

/* ------------------------------------------------------------------ quick record */

#[derive(Clone, PartialEq)]
pub enum RecState {
    Recording,
    Done,
    Failed(String),
}

pub struct Recording {
    pub path: PathBuf,
    pub n: usize,
    pub done: usize,
    pub state: RecState,
    pub t0: Instant,
    pub secs: f64,
    pub gaps: u64,
    pub cancel: bool,
    pub shape: Vec<usize>,
    pub dtype: Option<DType>,
    pub raw: Vec<u8>, // n frames, the SHM's type
    pub cnt0: Vec<u64>,
    pub cnt2: Vec<u64>,
    pub ts: Vec<f64>,
}

/// N consecutive frames, polled from a read-only map of its own (cnt0 every 50 µs): every
/// frame with its cnt0, cnt2 and timestamp, missed ones show as gaps in cnt0.
pub fn record_start(path: PathBuf, n: usize, timeout: f64) -> Arc<Mutex<Recording>> {
    let rec = Arc::new(Mutex::new(Recording {
        path: path.clone(),
        n,
        done: 0,
        state: RecState::Recording,
        t0: Instant::now(),
        secs: 0.0,
        gaps: 0,
        cancel: false,
        shape: Vec::new(),
        dtype: None,
        raw: Vec::new(),
        cnt0: Vec::new(),
        cnt2: Vec::new(),
        ts: Vec::new(),
    }));
    let r = rec.clone();
    std::thread::spawn(move || {
        let res = (|| -> Result<(), String> {
            let f = ShmFile::open(&path)?;
            if !f.readable {
                return Err("this SHM cannot be read".into());
            }
            let mut last = f.cnt0();
            let mut t_frame = Instant::now();
            let mut raw = Vec::new();
            let (mut c0, mut c2, mut ts) = (Vec::with_capacity(n), Vec::with_capacity(n), Vec::with_capacity(n));
            {
                let mut g = r.lock().unwrap();
                g.shape = f.shape.clone();
                g.dtype = f.dtype;
            }
            while c0.len() < n {
                if r.lock().unwrap().cancel {
                    return Err(format!("cancelled after {} frames", c0.len()));
                }
                if f.cnt0() == last {
                    if t_frame.elapsed().as_secs_f64() > timeout {
                        return Err(format!("no new frame for {} s (after {} frames)", timeout, c0.len()));
                    }
                    std::thread::sleep(Duration::from_micros(50));
                    continue;
                }
                let fr = f.read_frame(0, Part::Abs).ok_or("read failed")?;
                raw.extend_from_slice(&fr.raw);
                c0.push(fr.cnt0);
                c2.push(fr.cnt2);
                ts.push(fr.ts);
                last = fr.cnt0;
                t_frame = Instant::now();
                r.lock().unwrap().done = c0.len();
            }
            let mut g = r.lock().unwrap();
            g.gaps = c0.windows(2).map(|w| w[1].saturating_sub(w[0]).saturating_sub(1)).sum();
            g.raw = raw;
            g.cnt0 = c0;
            g.cnt2 = c2;
            g.ts = ts;
            Ok(())
        })();
        let mut g = r.lock().unwrap();
        g.secs = g.t0.elapsed().as_secs_f64();
        g.state = match res {
            Ok(()) => RecState::Done,
            Err(e) => RecState::Failed(e),
        };
    });
    rec
}

fn npy(descr: &str, shape: &[usize], data: &[u8]) -> Vec<u8> {
    let shape = if shape.len() == 1 {
        format!("({},)", shape[0])
    } else {
        format!("({})", shape.iter().map(|s| s.to_string()).collect::<Vec<_>>().join(", "))
    };
    let mut header = format!("{{'descr': '{}', 'fortran_order': False, 'shape': {}, }}", descr, shape);
    let pad = (64 - (10 + header.len() + 1) % 64) % 64;
    header.push_str(&" ".repeat(pad));
    header.push('\n');
    let mut out = b"\x93NUMPY\x01\x00".to_vec();
    out.extend_from_slice(&(header.len() as u16).to_le_bytes());
    out.extend_from_slice(header.as_bytes());
    out.extend_from_slice(data);
    out
}

/// .npy: the frames (n, ...shape); .npz: frames, cnt0, cnt2, timestamp
pub fn record_save(r: &Recording, file: &Path) -> Result<(), String> {
    let dtype = r.dtype.ok_or("no type")?;
    let mut shape = vec![r.cnt0.len()];
    shape.extend_from_slice(&r.shape);
    let frames = npy(dtype.descr(), &shape, &r.raw);
    let bytes = if file.extension().map(|e| e == "npz").unwrap_or(false) {
        let u64s = |v: &[u64]| v.iter().flat_map(|x| x.to_le_bytes()).collect::<Vec<u8>>();
        let f64s: Vec<u8> = r.ts.iter().flat_map(|x| x.to_le_bytes()).collect();
        zip_store(&[
            ("frames.npy", frames),
            ("cnt0.npy", npy("<u8", &[r.cnt0.len()], &u64s(&r.cnt0))),
            ("cnt2.npy", npy("<u8", &[r.cnt2.len()], &u64s(&r.cnt2))),
            ("timestamp.npy", npy("<f8", &[r.ts.len()], &f64s)),
        ])
    } else {
        frames
    };
    if let Some(d) = file.parent() {
        if !d.as_os_str().is_empty() {
            std::fs::create_dir_all(d).map_err(|e| e.to_string())?;
        }
    }
    std::fs::write(file, bytes).map_err(|e| format!("{}: {}", file.display(), e))
}

fn crc32(data: &[u8]) -> u32 {
    let mut table = [0u32; 256];
    for (i, t) in table.iter_mut().enumerate() {
        let mut c = i as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
        }
        *t = c;
    }
    let mut c = 0xFFFF_FFFFu32;
    for &b in data {
        c = table[((c ^ b as u32) & 0xFF) as usize] ^ (c >> 8);
    }
    c ^ 0xFFFF_FFFF
}

/// a zip of stored (uncompressed) files, as numpy reads .npz (ZIP64 not handled: < 4 GB)
fn zip_store(files: &[(&str, Vec<u8>)]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut central = Vec::new();
    for (name, data) in files {
        let crc = crc32(data);
        let off = out.len() as u32;
        let (n, sz) = (name.len() as u16, data.len() as u32);
        out.extend_from_slice(&0x0403_4b50u32.to_le_bytes());
        out.extend_from_slice(&[20, 0, 0, 0, 0, 0, 0, 0, 0x21, 0]); // version, flags, method 0, time, date
        out.extend_from_slice(&crc.to_le_bytes());
        out.extend_from_slice(&sz.to_le_bytes());
        out.extend_from_slice(&sz.to_le_bytes());
        out.extend_from_slice(&n.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(name.as_bytes());
        out.extend_from_slice(data);
        central.extend_from_slice(&0x0201_4b50u32.to_le_bytes());
        central.extend_from_slice(&[20, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0x21, 0]);
        central.extend_from_slice(&crc.to_le_bytes());
        central.extend_from_slice(&sz.to_le_bytes());
        central.extend_from_slice(&sz.to_le_bytes());
        central.extend_from_slice(&n.to_le_bytes());
        central.extend_from_slice(&[0u8; 12]); // extra, comment, disk, attributes
        central.extend_from_slice(&off.to_le_bytes());
        central.extend_from_slice(name.as_bytes());
    }
    let cd_off = out.len() as u32;
    let cd_len = central.len() as u32;
    out.extend_from_slice(&central);
    out.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
    out.extend_from_slice(&[0, 0, 0, 0]);
    out.extend_from_slice(&(files.len() as u16).to_le_bytes());
    out.extend_from_slice(&(files.len() as u16).to_le_bytes());
    out.extend_from_slice(&cd_len.to_le_bytes());
    out.extend_from_slice(&cd_off.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out
}

/* ------------------------------------------------------------------ values */

/// one value as the SHM type's bytes (complex: real part, imaginary 0)
pub fn encode(d: DType, v: f64, out: &mut Vec<u8>) {
    match d {
        DType::U8 => out.push(v.round().clamp(0.0, 255.0) as u8),
        DType::I8 => out.push(v.round().clamp(-128.0, 127.0) as i8 as u8),
        DType::U16 => out.extend_from_slice(&(v.round().clamp(0.0, 65535.0) as u16).to_le_bytes()),
        DType::I16 => out.extend_from_slice(&(v.round().clamp(-32768.0, 32767.0) as i16).to_le_bytes()),
        DType::U32 => out.extend_from_slice(&(v.round().clamp(0.0, u32::MAX as f64) as u32).to_le_bytes()),
        DType::I32 => out.extend_from_slice(&(v.round().clamp(i32::MIN as f64, i32::MAX as f64) as i32).to_le_bytes()),
        DType::U64 => out.extend_from_slice(&(v.round().max(0.0) as u64).to_le_bytes()),
        DType::I64 => out.extend_from_slice(&(v.round() as i64).to_le_bytes()),
        DType::F32 => out.extend_from_slice(&(v as f32).to_le_bytes()),
        DType::F64 => out.extend_from_slice(&v.to_le_bytes()),
        DType::C64 => {
            out.extend_from_slice(&(v as f32).to_le_bytes());
            out.extend_from_slice(&0f32.to_le_bytes());
        }
        DType::C128 => {
            out.extend_from_slice(&v.to_le_bytes());
            out.extend_from_slice(&0f64.to_le_bytes());
        }
    }
}

/// a small, fast random generator (xorshift64*), seeded from the clock
pub struct Rng(u64);

impl Rng {
    pub fn new() -> Rng {
        Rng((now_epoch() * 1e9) as u64 | 1)
    }
    pub fn uniform(&mut self) -> f64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        (self.0.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 11) as f64 / (1u64 << 53) as f64
    }
}

/// a .npy file: (descr, shape, data bytes)
pub fn read_npy(path: &Path) -> Result<(String, Vec<usize>, Vec<u8>), String> {
    let b = std::fs::read(path).map_err(|e| format!("{}: {}", path.display(), e))?;
    if b.len() < 10 || &b[..6] != b"\x93NUMPY" {
        return Err("not a .npy file".into());
    }
    let (hl, start) = if b[6] == 1 {
        (u16::from_le_bytes([b[8], b[9]]) as usize, 10)
    } else {
        (u32::from_le_bytes([b[8], b[9], b[10], b[11]]) as usize, 12)
    };
    let h = String::from_utf8_lossy(&b[start..start + hl]).into_owned();
    let field = |k: &str| -> Option<String> {
        let i = h.find(&format!("'{}'", k))? + k.len() + 2;
        let rest = h[i..].trim_start().trim_start_matches(':').trim_start();
        Some(rest.to_string())
    };
    let descr = field("descr").and_then(|r| r.split('\'').nth(1).map(|s| s.to_string())).ok_or("no descr")?;
    if field("fortran_order").map(|r| r.starts_with("True")).unwrap_or(false) {
        return Err("Fortran-order arrays are not read".into());
    }
    let shape_s = field("shape").ok_or("no shape")?;
    let inner = &shape_s[shape_s.find('(').ok_or("bad shape")? + 1..shape_s.find(')').ok_or("bad shape")?];
    let shape: Vec<usize> = inner.split(',').filter(|s| !s.trim().is_empty()).map(|s| s.trim().parse().unwrap_or(0)).collect();
    Ok((descr, shape, b[start + hl..].to_vec()))
}

/// values of a .npy data block as f64 (numeric types; complex: real part)
pub fn npy_values(descr: &str, data: &[u8]) -> Result<Vec<f64>, String> {
    let le = !descr.starts_with('>');
    let t = descr.trim_start_matches(['<', '>', '|', '=']);
    let rd = |chunk: &[u8]| -> Vec<u8> {
        let mut c = chunk.to_vec();
        if !le {
            c.reverse();
        }
        c
    };
    let conv: Box<dyn Fn(&[u8]) -> f64> = match t {
        "u1" => Box::new(|c: &[u8]| c[0] as f64),
        "i1" => Box::new(|c: &[u8]| c[0] as i8 as f64),
        "u2" => Box::new(move |c: &[u8]| u16::from_le_bytes(rd(c).try_into().unwrap()) as f64),
        "i2" => Box::new(move |c: &[u8]| i16::from_le_bytes(rd(c).try_into().unwrap()) as f64),
        "u4" => Box::new(move |c: &[u8]| u32::from_le_bytes(rd(c).try_into().unwrap()) as f64),
        "i4" => Box::new(move |c: &[u8]| i32::from_le_bytes(rd(c).try_into().unwrap()) as f64),
        "u8" => Box::new(move |c: &[u8]| u64::from_le_bytes(rd(c).try_into().unwrap()) as f64),
        "i8" => Box::new(move |c: &[u8]| i64::from_le_bytes(rd(c).try_into().unwrap()) as f64),
        "f4" => Box::new(move |c: &[u8]| f32::from_le_bytes(rd(c).try_into().unwrap()) as f64),
        "f8" => Box::new(move |c: &[u8]| f64::from_le_bytes(rd(c).try_into().unwrap())),
        "c8" => Box::new(move |c: &[u8]| f32::from_le_bytes(rd(&c[..4]).try_into().unwrap()) as f64),
        "c16" => Box::new(move |c: &[u8]| f64::from_le_bytes(rd(&c[..8]).try_into().unwrap())),
        _ => return Err(format!("type {} is not read", descr)),
    };
    let size: usize = match t {
        "u1" | "i1" => 1,
        "u2" | "i2" => 2,
        "u4" | "i4" | "f4" => 4,
        "c16" => 16,
        _ => 8,
    };
    Ok(data.chunks_exact(size).map(|c| conv(c)).collect())
}

/* ------------------------------------------------------------------ DAQ */

#[derive(Clone)]
pub struct DaqSource {
    pub uri: String,
    pub params: Vec<(String, String)>, // in order; booleans as true/false
}

pub const SMEM_KEYS: [&str; 8] =
    ["format", "samples", "file_rollover", "daq_affinity", "sink_affinity", "buffer_limit", "metadata_only", "eager_start"];

impl DaqSource {
    pub fn smem(path: &Path) -> DaqSource {
        DaqSource {
            uri: format!("smem://{}", path.display()),
            params: vec![
                ("format".into(), "fits".into()),
                ("metadata_only".into(), "false".into()),
                ("eager_start".into(), "true".into()),
            ],
        }
    }
    pub fn file(path: &str) -> DaqSource {
        DaqSource { uri: format!("file://{}", path), params: Vec::new() }
    }
    pub fn is_smem(&self) -> bool {
        self.uri.starts_with("smem://")
    }
    pub fn get(&self, k: &str) -> Option<&str> {
        self.params.iter().find(|(a, _)| a == k).map(|(_, v)| v.as_str())
    }
    pub fn set(&mut self, k: &str, v: Option<String>) {
        self.params.retain(|(a, _)| a != k);
        if let Some(v) = v {
            self.params.push((k.into(), v));
        }
    }
}

#[derive(Clone, Default)]
pub struct DaqConfig {
    pub root: String,
    pub sources: Vec<DaqSource>,
}

impl DaqConfig {
    /// the YAML daoShmViewer exports
    pub fn to_yaml(&self) -> String {
        let mut s = format!("root_storage: {}\nsources:\n", self.root);
        if self.sources.is_empty() {
            s = format!("root_storage: {}\nsources: []\n", self.root);
        }
        for src in &self.sources {
            s.push_str(&format!("- uri: {}\n", src.uri));
            for (k, v) in &src.params {
                s.push_str(&format!("  {}: {}\n", k, v));
            }
        }
        s
    }

    /// the same subset of YAML back (what daoShmViewer and daoShmWebViewer export)
    pub fn from_yaml(text: &str) -> Result<DaqConfig, String> {
        let mut c = DaqConfig::default();
        for line in text.lines() {
            let l = line.split(" #").next().unwrap_or("").trim_end();
            if l.trim().is_empty() || l.trim_start().starts_with('#') {
                continue;
            }
            let t = l.trim_start();
            let unq = |v: &str| v.trim().trim_matches('\'').trim_matches('"').to_string();
            if let Some(rest) = t.strip_prefix("- ") {
                let (k, v) = rest.split_once(':').ok_or(format!("cannot read: {}", line))?;
                if k.trim() != "uri" {
                    return Err(format!("a source must start with uri: {}", line));
                }
                c.sources.push(DaqSource { uri: unq(v), params: Vec::new() });
            } else if let Some((k, v)) = t.split_once(':') {
                let (k, v) = (k.trim(), unq(v));
                if l.starts_with(' ') {
                    let s = c.sources.last_mut().ok_or(format!("a field outside a source: {}", line))?;
                    s.params.push((k.into(), v));
                } else if k == "root_storage" {
                    c.root = v;
                }
            }
        }
        for s in &c.sources {
            if !(s.uri.starts_with("smem://") || s.uri.starts_with("file://")) {
                return Err(format!("source uri must be smem://... or file://...: {}", s.uri));
            }
        }
        Ok(c)
    }
}

/// daoDAQCli.py <args>: Ok(stdout) or Err(why)
pub fn daq_cli(args: &[&str]) -> Result<String, String> {
    if !which("daoDAQCli.py") {
        return Err("daoDAQCli.py is not in the PATH".into());
    }
    let (code, out, err) = run("daoDAQCli.py", args)?;
    let strip = |s: &str| {
        // no colours
        let mut o = String::new();
        let mut esc = false;
        for ch in s.chars() {
            if ch == '\x1b' {
                esc = true;
            } else if esc {
                if ch == 'm' {
                    esc = false;
                }
            } else {
                o.push(ch);
            }
        }
        o.trim().to_string()
    };
    if code == 0 {
        Ok(strip(&out))
    } else {
        let e = strip(&format!("{}\n{}", out, err));
        Err(e.lines().last().unwrap_or("failed").to_string())
    }
}

/// start a session: upload the configuration, apply, acquire
pub fn daq_start(cfg: &DaqConfig) -> Result<String, String> {
    let file = std::env::temp_dir().join(format!("daoShmTui_daq_{}.yaml", std::process::id()));
    std::fs::write(&file, cfg.to_yaml()).map_err(|e| e.to_string())?;
    let f = file.to_string_lossy().into_owned();
    let r = daq_cli(&["upload", &f]).and_then(|_| daq_cli(&["apply"])).and_then(|_| daq_cli(&["acquire"]));
    let _ = std::fs::remove_file(&file);
    r.map(|_| "DAQ session started".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn daq_yaml_round_trip() {
        let mut c = DaqConfig { root: "/data".into(), sources: vec![DaqSource::smem(Path::new("/tmp/a.im.shm")), DaqSource::file("/etc/x")] };
        c.sources[0].set("samples", Some("100".into()));
        let back = DaqConfig::from_yaml(&c.to_yaml()).unwrap();
        assert_eq!(back.root, "/data");
        assert_eq!(back.sources.len(), 2);
        assert_eq!(back.sources[0].get("samples"), Some("100"));
        assert_eq!(back.sources[1].uri, "file:///etc/x");
    }

    #[test]
    fn crc32_known() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn latency_session_name() {
        assert_eq!(latency_name(Path::new("/tmp/shIm.im.shm"), Path::new("/tmp/dm-Cmd.im.shm")), "daoLatency_shIm_dm_Cmd");
    }
}
