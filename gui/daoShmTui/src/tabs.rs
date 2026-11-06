//! The tool tabs (semaphores, quick record, latency, snapshots, tmux, DAQ), the input prompt,
//! confirmations, and the writes (--write: set, random, edit a value, load .npy, create).

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use ratatui::crossterm::event::{KeyCode, KeyEvent};

use crate::app::{base, App, Focus};
use crate::dao::Dao;
use crate::shm::{now_epoch, DType};
use crate::tools::{self, DaqConfig, DaqSource, RecState, Recording, Session, SnapFile};

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Tab {
    View,
    Sems,
    Record,
    Latency,
    Snaps,
    Tmux,
    Daq,
}

pub const TABS: [(Tab, &str); 7] = [
    (Tab::View, "View"),
    (Tab::Sems, "Semaphores"),
    (Tab::Record, "Record"),
    (Tab::Latency, "Latency"),
    (Tab::Snaps, "Snapshots"),
    (Tab::Tmux, "tmux"),
    (Tab::Daq, "DAQ"),
];

#[derive(Clone, PartialEq)]
pub enum PromptKind {
    EditValue,
    SetAll,
    Random,
    LoadNpy,
    Create,
    RecordN,
    RecordSave,
    LatencyN,
    SnapCreate,
    DaqRoot,
    DaqFile,
    DaqField(String),
    DaqExport,
    DaqImport,
}

pub struct Prompt {
    pub label: String,
    pub buf: String,
    pub kind: PromptKind,
}

pub enum Action {
    Write { path: PathBuf, raw: Vec<u8>, n: usize, what: String },
    SnapLoad(String),
    LatencyStop(String),
    DaqStart,
    DaqFinish,
    DaqRecover,
}

pub struct LatData {
    pub avg: f64,
    pub rms: f64,
    pub values: Vec<f64>,
    pub pop: usize,
    pub array: PathBuf,
}

pub struct ToolState {
    pub write: bool,
    dao: Option<Result<Dao, String>>,
    pub snapdir: PathBuf,
    // record
    pub rec: Option<Arc<Mutex<Recording>>>,
    pub rec_n: usize,
    pub rec_timeout: f64,
    pub rec_saved: Option<String>,
    // latency
    pub lat_a: Option<PathBuf>,
    pub lat_b: Option<PathBuf>,
    pub lat_n: u32,
    pub lat_sessions: Vec<String>,
    pub lat_sel: usize,
    pub lat_data: Option<LatData>,
    last_lat: Instant,
    // snapshots
    pub snaps: Vec<SnapFile>,
    pub snap_sel: usize,
    pub snap_result: Arc<Mutex<Option<String>>>,
    pub snap_busy: Option<String>,
    last_snaps: Instant,
    // tmux
    pub sessions: Vec<Session>,
    pub tmux_sel: usize,
    pub tmux_text: String,
    pub tmux_scroll: u16,
    last_tmux: Instant,
    pub attach: Option<String>,
    // DAQ
    pub daq: DaqConfig,
    pub daq_sel: usize,
    pub daq_field: usize,
    pub daq_state: Arc<Mutex<(Option<String>, String)>>,
    daq_polling: bool,
}

impl ToolState {
    pub fn new(write: bool, snapdir: PathBuf) -> ToolState {
        ToolState {
            write,
            dao: None,
            snapdir,
            rec: None,
            rec_n: 100,
            rec_timeout: 5.0,
            rec_saved: None,
            lat_a: None,
            lat_b: None,
            lat_n: 100,
            lat_sessions: Vec::new(),
            lat_sel: 0,
            lat_data: None,
            last_lat: Instant::now() - Duration::from_secs(10),
            snaps: Vec::new(),
            snap_sel: 0,
            snap_result: Arc::new(Mutex::new(None)),
            snap_busy: None,
            last_snaps: Instant::now() - Duration::from_secs(10),
            sessions: Vec::new(),
            tmux_sel: 0,
            tmux_text: String::new(),
            tmux_scroll: 0,
            last_tmux: Instant::now() - Duration::from_secs(10),
            attach: None,
            daq: DaqConfig { root: std::env::var("DAODATA").unwrap_or_default(), sources: Vec::new() },
            daq_sel: 0,
            daq_field: 0,
            daq_state: Arc::new(Mutex::new((None, String::new()))),
            daq_polling: false,
        }
    }
}

fn stamp() -> String {
    let t = now_epoch() as libc::time_t;
    // SAFETY: localtime_r with valid pointers
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe { libc::localtime_r(&t, &mut tm) };
    format!("{:04}{:02}{:02}_{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec)
}

fn dtype_from(name: &str) -> Option<(DType, u8)> {
    Some(match name {
        "uint8" | "u8" => (DType::U8, 1),
        "int8" | "i8" => (DType::I8, 2),
        "uint16" | "u16" => (DType::U16, 3),
        "int16" | "i16" => (DType::I16, 4),
        "uint32" | "u32" => (DType::U32, 5),
        "int32" | "i32" => (DType::I32, 6),
        "uint64" | "u64" => (DType::U64, 7),
        "int64" | "i64" => (DType::I64, 8),
        "float32" | "f32" | "float" => (DType::F32, 9),
        "float64" | "f64" | "double" => (DType::F64, 10),
        "complex64" | "c64" => (DType::C64, 11),
        "complex128" | "c128" => (DType::C128, 12),
        _ => return None,
    })
}

impl App {
    pub fn ask(&mut self, label: &str, init: String, kind: PromptKind) {
        self.prompt = Some(crate::tabs::Prompt { label: label.into(), buf: init, kind });
    }

    pub fn confirm_action(&mut self, msg: String, a: Action) {
        self.confirm = Some((msg, a));
    }

    fn need_write(&mut self) -> bool {
        if !self.t.write {
            self.say("read-only: start daoShmTui with --write for this".into());
            return false;
        }
        true
    }

    fn dao(&mut self) -> Result<&Dao, String> {
        if !self.t.write {
            return Err("read-only: start daoShmTui with --write".into());
        }
        if self.t.dao.is_none() {
            self.t.dao = Some(Dao::load());
        }
        match self.t.dao.as_ref().unwrap() {
            Ok(d) => Ok(d),
            Err(e) => Err(e.clone()),
        }
    }

    /* ------------------------------------------------------------ periodic */

    pub fn tools_tick(&mut self) {
        let done = self.t.snap_result.lock().unwrap().take();
        if let Some(m) = done {
            self.t.snap_busy = None;
            self.say(m);
            self.t.last_snaps = Instant::now() - Duration::from_secs(10);
            self.reg.scan();
        }
        match self.tab {
            Tab::Latency if self.t.last_lat.elapsed() >= Duration::from_millis(500) => {
                self.t.last_lat = Instant::now();
                self.t.lat_sessions =
                    tools::tmux_sessions().into_iter().map(|s| s.name).filter(|n| n.starts_with(tools::LAT_PREFIX)).collect();
                self.t.lat_sel = self.t.lat_sel.min(self.t.lat_sessions.len().saturating_sub(1));
                self.t.lat_data = self.t.lat_sessions.get(self.t.lat_sel).cloned().and_then(|n| self.lat_values(&n));
            }
            Tab::Snaps if self.t.last_snaps.elapsed() >= Duration::from_secs(2) => {
                self.t.last_snaps = Instant::now();
                self.t.snaps = tools::snapshots(&self.t.snapdir);
                self.t.snap_sel = self.t.snap_sel.min(self.t.snaps.len().saturating_sub(1));
            }
            Tab::Tmux if self.t.last_tmux.elapsed() >= Duration::from_secs(1) => {
                self.t.last_tmux = Instant::now();
                self.t.sessions = tools::tmux_sessions();
                self.t.tmux_sel = self.t.tmux_sel.min(self.t.sessions.len().saturating_sub(1));
                self.t.tmux_text = match self.t.sessions.get(self.t.tmux_sel) {
                    Some(s) => tools::tmux_capture(&s.name, 500),
                    None => String::new(),
                };
            }
            Tab::Daq if !self.t.daq_polling => {
                // the state, every 2 s, without blocking the screen (daoDAQCli.py is slow to start)
                self.t.daq_polling = true;
                let st = self.t.daq_state.clone();
                std::thread::spawn(move || loop {
                    let r = tools::daq_cli(&["state"]);
                    *st.lock().unwrap() = match r {
                        Ok(out) => (out.rsplit(':').next().map(|s| s.trim().to_string()), String::new()),
                        Err(e) => (None, e),
                    };
                    std::thread::sleep(Duration::from_secs(2));
                });
            }
            _ => {}
        }
    }

    fn lat_values(&self, name: &str) -> Option<LatData> {
        let meas = tools::latency_meas(&self.reg.dirs, name);
        let (fa, fr, fx) = (
            self.reg.get(&tools::with_suffix(&meas, "Avg"))?,
            self.reg.get(&tools::with_suffix(&meas, "Rms"))?,
            self.reg.get(&tools::with_suffix(&meas, "Array"))?,
        );
        let avg = fa.read_frame(0, crate::shm::Part::Abs)?.data.first().copied()?;
        let rms = fr.read_frame(0, crate::shm::Part::Abs)?.data.first().copied()?;
        let arr = fx.read_frame(0, crate::shm::Part::Abs)?;
        let n = (fx.cnt0() as usize).min(arr.data.len());
        Some(LatData { avg, rms, values: arr.data[..n].to_vec(), pop: arr.data.len(), array: fx.path.clone() })
    }

    /* ------------------------------------------------------------ prompt */

    pub fn prompt_key(&mut self, k: KeyEvent) {
        let p = match self.prompt.as_mut() {
            Some(p) => p,
            None => return,
        };
        match k.code {
            KeyCode::Esc => self.prompt = None,
            KeyCode::Backspace => {
                p.buf.pop();
            }
            KeyCode::Char(c) => p.buf.push(c),
            KeyCode::Enter => {
                let p = self.prompt.take().unwrap();
                let r = self.prompt_done(p.kind, p.buf.trim().to_string());
                if let Err(e) = r {
                    self.say(e);
                }
            }
            _ => {}
        }
    }

    fn prompt_done(&mut self, kind: PromptKind, v: String) -> Result<(), String> {
        let num = |s: &str| s.parse::<f64>().map_err(|_| format!("not a number: {}", s));
        match kind {
            PromptKind::EditValue => {
                let x = num(&v)?;
                let (fr, path) = (self.frame.as_ref().ok_or("no frame")?, self.selected.clone().ok_or("no SHM")?);
                let (r, c) = self.cursor_src().ok_or("no cursor")?;
                let idx = fr.slice * fr.rows * fr.cols + r * fr.cols + c;
                let es = fr.dtype.size();
                let mut raw = fr.raw.clone();
                let mut b = Vec::new();
                tools::encode(fr.dtype, x, &mut b);
                raw[idx * es..idx * es + es].copy_from_slice(&b);
                let n = raw.len() / es;
                self.dao()?.write(&path, &raw, n)?;
                self.say(format!("{}[{}, {}] = {}", base(&path), r, c, x));
            }
            PromptKind::SetAll => {
                let x = num(&v)?;
                let fr = self.frame.as_ref().ok_or("no frame")?;
                let (d, n) = (fr.dtype, fr.raw.len() / fr.dtype.size());
                let mut raw = Vec::with_capacity(fr.raw.len());
                for _ in 0..n {
                    tools::encode(d, x, &mut raw);
                }
                let path = self.selected.clone().ok_or("no SHM")?;
                self.confirm_action(format!("set every element of {} to {}?", base(&path), x), Action::Write { path, raw, n, what: format!("set to {}", x) });
            }
            PromptKind::Random => {
                let parts: Vec<&str> = v.split_whitespace().collect();
                if parts.len() != 2 {
                    return Err("two numbers: min max".into());
                }
                let (lo, hi) = (num(parts[0])?, num(parts[1])?);
                let fr = self.frame.as_ref().ok_or("no frame")?;
                let (d, n) = (fr.dtype, fr.raw.len() / fr.dtype.size());
                let mut rng = tools::Rng::new();
                let mut raw = Vec::with_capacity(fr.raw.len());
                for _ in 0..n {
                    let u = rng.uniform();
                    tools::encode(d, if d.is_int() { (lo + u * (hi - lo + 1.0)).floor().min(hi) } else { lo + u * (hi - lo) }, &mut raw);
                }
                let path = self.selected.clone().ok_or("no SHM")?;
                self.confirm_action(
                    format!("fill {} with random values in [{}, {}]?", base(&path), lo, hi),
                    Action::Write { path, raw, n, what: format!("random in [{}, {}]", lo, hi) },
                );
            }
            PromptKind::LoadNpy => {
                let file = PathBuf::from(shellexpand(&v));
                let (descr, shape, data) = tools::read_npy(&file)?;
                let fr = self.frame.as_ref().ok_or("no frame")?;
                let n = fr.raw.len() / fr.dtype.size();
                let count: usize = shape.iter().product();
                if count != n {
                    return Err(format!("{} has {} values {:?}, the SHM {} {:?}", file.display(), count, shape, n, fr.shape));
                }
                let raw = if descr == fr.dtype.descr() {
                    data[..fr.raw.len()].to_vec()
                } else {
                    let vals = tools::npy_values(&descr, &data)?;
                    let mut raw = Vec::with_capacity(fr.raw.len());
                    for x in vals.iter().take(n) {
                        tools::encode(fr.dtype, *x, &mut raw);
                    }
                    raw
                };
                let path = self.selected.clone().ok_or("no SHM")?;
                let conv = if descr == fr.dtype.descr() { String::new() } else { format!(" (converted {} -> {})", descr, fr.dtype.name()) };
                self.confirm_action(
                    format!("write {} into {}{}?", file.display(), base(&path), conv),
                    Action::Write { path, raw, n, what: format!("loaded {}", file.display()) },
                );
            }
            PromptKind::Create => {
                let parts: Vec<&str> = v.split_whitespace().collect();
                if parts.len() < 2 || !tools::valid_name(parts[0]) {
                    return Err("name shape [type]: e.g. myShm 128,128 float32".into());
                }
                let mut size: Vec<u32> = parts[1].split(',').map(|s| s.trim().parse().unwrap_or(0)).collect();
                if size.is_empty() || size.len() > 3 || size.contains(&0) {
                    return Err("shape: 1 to 3 sizes > 0, e.g. 128,128".into());
                }
                if size.len() == 1 {
                    size.push(1);
                }
                let (_, atype) = dtype_from(parts.get(2).copied().unwrap_or("float32")).ok_or("type: float32, int16, uint16, ...")?;
                let path = self.reg.dirs[0].join(format!("{}.im.shm", parts[0]));
                if path.exists() {
                    return Err(format!("{} exists", path.display()));
                }
                self.dao()?.create(&path, &size, atype)?;
                self.reg.scan();
                self.say(format!("created {}", path.display()));
            }
            PromptKind::RecordN => self.t.rec_n = v.parse::<usize>().ok().filter(|n| *n > 0).ok_or("frames: a number > 0")?,
            PromptKind::RecordSave => {
                let file = PathBuf::from(shellexpand(&v));
                let rec = self.t.rec.clone().ok_or("nothing recorded")?;
                let g = rec.lock().unwrap();
                tools::record_save(&g, &file)?;
                self.t.rec_saved = Some(file.display().to_string());
                drop(g);
                self.say(format!("saved {}", file.display()));
            }
            PromptKind::LatencyN => self.t.lat_n = v.parse::<u32>().ok().filter(|n| *n >= 2).ok_or("window: a number >= 2")?,
            PromptKind::SnapCreate => {
                let name = if v.is_empty() { format!("snapshot_{}", stamp()) } else { v };
                if !tools::valid_name(&name) {
                    return Err("snapshot name: letters, digits, _ - . only".into());
                }
                self.t.snap_busy = Some(format!("daoSnapshot.py create {}…", name));
                tools::snapshot_run("create", self.t.snapdir.clone(), name, self.t.snap_result.clone());
            }
            PromptKind::DaqRoot => self.t.daq.root = v,
            PromptKind::DaqFile => {
                if !v.starts_with('/') {
                    return Err("an absolute path".into());
                }
                let s = DaqSource::file(&v);
                if !self.t.daq.sources.iter().any(|x| x.uri == s.uri) {
                    self.t.daq.sources.push(s);
                }
            }
            PromptKind::DaqField(key) => {
                let s = self.t.daq.sources.get_mut(self.t.daq_sel).ok_or("no source")?;
                if v.is_empty() {
                    s.set(&key, None);
                } else if key != "format" && v.parse::<i64>().is_err() {
                    return Err(format!("{}: a whole number (empty: unset)", key));
                } else {
                    s.set(&key, Some(v));
                }
            }
            PromptKind::DaqExport => {
                let f = PathBuf::from(shellexpand(&v));
                std::fs::write(&f, self.t.daq.to_yaml()).map_err(|e| format!("{}: {}", f.display(), e))?;
                self.say(format!("exported {}", f.display()));
            }
            PromptKind::DaqImport => {
                let f = PathBuf::from(shellexpand(&v));
                let text = std::fs::read_to_string(&f).map_err(|e| format!("{}: {}", f.display(), e))?;
                self.t.daq = DaqConfig::from_yaml(&text)?;
                self.t.daq_sel = 0;
                self.say(format!("imported {}: {} sources", f.display(), self.t.daq.sources.len()));
            }
        }
        Ok(())
    }

    pub fn confirm_key(&mut self, k: KeyEvent) {
        let (_, a) = match self.confirm.take() {
            Some(c) => c,
            None => return,
        };
        if !matches!(k.code, KeyCode::Char('y') | KeyCode::Char('Y')) {
            self.say("cancelled".into());
            return;
        }
        let r: Result<String, String> = match a {
            Action::Write { path, raw, n, what } => self.dao().and_then(|d| d.write(&path, &raw, n)).map(|_| format!("{}: {}", base(&path), what)),
            Action::SnapLoad(name) => {
                self.t.snap_busy = Some(format!("daoSnapshot.py load {}…", name));
                tools::snapshot_run("load", self.t.snapdir.clone(), name, self.t.snap_result.clone());
                Ok("loading the snapshot…".into())
            }
            Action::LatencyStop(name) => tools::latency_stop(&name).map(|_| format!("stopped {}", name)),
            Action::DaqStart => tools::daq_start(&self.t.daq),
            Action::DaqFinish => tools::daq_cli(&["finish"]).map(|_| "DAQ session finished".into()),
            Action::DaqRecover => tools::daq_cli(&["recover"]).map(|_| "DAQ recovered".into()),
        };
        match r {
            Ok(m) => self.say(m),
            Err(e) => self.say(e),
        }
    }

    /* ------------------------------------------------------------ writes from the view */

    pub fn write_key(&mut self, c: char) -> bool {
        if !"eVRLN".contains(c) {
            return false;
        }
        if !self.need_write() {
            return true;
        }
        if c != 'N' && self.frame.is_none() {
            self.say("no frame".into());
            return true;
        }
        match c {
            'e' => {
                let cur = self.cursor_value().map(|v| format!("{}", v)).unwrap_or_default();
                let (r, cc) = self.cursor_src().unwrap_or((0, 0));
                self.ask(&format!("new value of [{}, {}]", r, cc), cur, PromptKind::EditValue)
            }
            'V' => self.ask("set every element to", "0".into(), PromptKind::SetAll),
            'R' => self.ask("random values, min max", "0 1".into(), PromptKind::Random),
            'L' => self.ask("load a .npy file", String::new(), PromptKind::LoadNpy),
            'N' => self.ask(&format!("new SHM in {}: name shape [type]", self.reg.dirs[0].display()), String::new(), PromptKind::Create),
            _ => {}
        }
        true
    }

    /* ------------------------------------------------------------ tab keys */

    pub fn tab_key(&mut self, k: KeyEvent) {
        if k.code == KeyCode::Esc {
            self.focus = Focus::List;
            return;
        }
        match self.tab {
            Tab::View => {}
            Tab::Sems => {}
            Tab::Record => self.record_key(k),
            Tab::Latency => self.latency_key(k),
            Tab::Snaps => self.snaps_key(k),
            Tab::Tmux => self.tmux_key(k),
            Tab::Daq => self.daq_key(k),
        }
    }

    fn record_key(&mut self, k: KeyEvent) {
        let busy = self.t.rec.as_ref().map(|r| r.lock().unwrap().state == RecState::Recording).unwrap_or(false);
        match k.code {
            KeyCode::Char('n') => self.ask("frames to record", self.t.rec_n.to_string(), PromptKind::RecordN),
            KeyCode::Enter | KeyCode::Char('r') if !busy => {
                if let Some(p) = self.selected.clone() {
                    self.t.rec = Some(tools::record_start(p, self.t.rec_n, self.t.rec_timeout));
                    self.t.rec_saved = None;
                }
            }
            KeyCode::Char('c') if busy => {
                if let Some(r) = &self.t.rec {
                    r.lock().unwrap().cancel = true;
                }
            }
            KeyCode::Char('s') => {
                let done = self.t.rec.as_ref().map(|r| r.lock().unwrap().state == RecState::Done).unwrap_or(false);
                if done {
                    let r = self.t.rec.clone().unwrap();
                    let g = r.lock().unwrap();
                    let dir = std::env::var("DAODATA").unwrap_or_else(|_| std::env::current_dir().map(|d| d.display().to_string()).unwrap_or(".".into()));
                    let init = format!("{}/{}_{}_{}frames.npy", dir, base(&g.path), stamp(), g.cnt0.len());
                    drop(g);
                    self.ask("save as (.npy: frames, .npz: frames + cnt0, cnt2, timestamp)", init, PromptKind::RecordSave);
                } else {
                    self.say("nothing recorded yet".into());
                }
            }
            _ => {}
        }
    }

    fn latency_key(&mut self, k: KeyEvent) {
        match k.code {
            KeyCode::Char('a') => self.t.lat_a = self.selected.clone(),
            KeyCode::Char('b') => self.t.lat_b = self.selected.clone(),
            KeyCode::Char('n') => self.ask("daoTimeDiff window (-n)", self.t.lat_n.to_string(), PromptKind::LatencyN),
            KeyCode::Up => self.t.lat_sel = self.t.lat_sel.saturating_sub(1),
            KeyCode::Down => self.t.lat_sel = (self.t.lat_sel + 1).min(self.t.lat_sessions.len().saturating_sub(1)),
            KeyCode::Enter => {
                if !self.need_write() {
                    return;
                }
                match (self.t.lat_a.clone(), self.t.lat_b.clone()) {
                    (Some(a), Some(b)) => match tools::latency_start(&self.reg.dirs[0], &a, &b, self.t.lat_n) {
                        Ok(n) => self.say(format!("started {}", n)),
                        Err(e) => self.say(e),
                    },
                    _ => self.say("set the start (a) and end (b) SHMs: select them in the list".into()),
                }
                self.t.last_lat = Instant::now() - Duration::from_secs(10);
            }
            KeyCode::Char('x') => {
                if !self.need_write() {
                    return;
                }
                if let Some(n) = self.t.lat_sessions.get(self.t.lat_sel).cloned() {
                    self.confirm_action(format!("stop {}?", n), Action::LatencyStop(n));
                }
            }
            KeyCode::Char('v') => {
                if let Some(d) = &self.t.lat_data {
                    let p = d.array.clone();
                    self.show_path(&p);
                }
            }
            _ => {}
        }
        self.t.last_lat = Instant::now() - Duration::from_secs(10);
    }

    fn snaps_key(&mut self, k: KeyEvent) {
        match k.code {
            KeyCode::Up => self.t.snap_sel = self.t.snap_sel.saturating_sub(1),
            KeyCode::Down => self.t.snap_sel = (self.t.snap_sel + 1).min(self.t.snaps.len().saturating_sub(1)),
            KeyCode::Char('c') => {
                if self.need_write() {
                    self.ask("snapshot name (empty: a time stamp)", String::new(), PromptKind::SnapCreate)
                }
            }
            KeyCode::Enter => {
                if !self.need_write() {
                    return;
                }
                if let Some(s) = self.t.snaps.get(self.t.snap_sel) {
                    let n = s.name.clone();
                    self.confirm_action(
                        format!("load {}? it recreates every SHM saved in it (overwriting the current ones) and restarts the tmux sessions' commands", n),
                        Action::SnapLoad(n),
                    );
                }
            }
            _ => {}
        }
    }

    fn tmux_key(&mut self, k: KeyEvent) {
        match k.code {
            KeyCode::Up => {
                self.t.tmux_sel = self.t.tmux_sel.saturating_sub(1);
                self.t.tmux_scroll = 0;
            }
            KeyCode::Down => {
                self.t.tmux_sel = (self.t.tmux_sel + 1).min(self.t.sessions.len().saturating_sub(1));
                self.t.tmux_scroll = 0;
            }
            KeyCode::PageUp => self.t.tmux_scroll = self.t.tmux_scroll.saturating_add(10),
            KeyCode::PageDown => self.t.tmux_scroll = self.t.tmux_scroll.saturating_sub(10),
            KeyCode::End => self.t.tmux_scroll = 0,
            KeyCode::Enter => {
                if let Some(s) = self.t.sessions.get(self.t.tmux_sel) {
                    self.t.attach = Some(s.name.clone());
                }
            }
            _ => {}
        }
        self.t.last_tmux = Instant::now() - Duration::from_secs(10);
    }

    fn daq_key(&mut self, k: KeyEvent) {
        let nf = |s: Option<&DaqSource>| s.map(|s| if s.is_smem() { tools::SMEM_KEYS.len() } else { 0 }).unwrap_or(0);
        match k.code {
            KeyCode::Up => {
                self.t.daq_sel = self.t.daq_sel.saturating_sub(1);
                self.t.daq_field = 0;
            }
            KeyCode::Down => {
                self.t.daq_sel = (self.t.daq_sel + 1).min(self.t.daq.sources.len().saturating_sub(1));
                self.t.daq_field = 0;
            }
            KeyCode::Left => self.t.daq_field = self.t.daq_field.saturating_sub(1),
            KeyCode::Right => {
                let n = nf(self.t.daq.sources.get(self.t.daq_sel));
                if n > 0 {
                    self.t.daq_field = (self.t.daq_field + 1).min(n - 1);
                }
            }
            KeyCode::Char('a') => {
                if let Some(p) = self.selected.clone() {
                    let s = DaqSource::smem(&p);
                    if !self.t.daq.sources.iter().any(|x| x.uri == s.uri) {
                        self.t.daq.sources.push(s);
                        self.t.daq_sel = self.t.daq.sources.len() - 1;
                    }
                }
            }
            KeyCode::Char('f') => self.ask("file source (absolute path)", String::new(), PromptKind::DaqFile),
            KeyCode::Char('d') | KeyCode::Delete => {
                if self.t.daq_sel < self.t.daq.sources.len() {
                    self.t.daq.sources.remove(self.t.daq_sel);
                    self.t.daq_sel = self.t.daq_sel.min(self.t.daq.sources.len().saturating_sub(1));
                }
            }
            KeyCode::Char('o') => self.ask("root storage", self.t.daq.root.clone(), PromptKind::DaqRoot),
            KeyCode::Char('E') => self.ask("export the configuration to", format!("{}/daq.yaml", std::env::current_dir().map(|d| d.display().to_string()).unwrap_or(".".into())), PromptKind::DaqExport),
            KeyCode::Char('I') => self.ask("import a configuration (YAML)", String::new(), PromptKind::DaqImport),
            KeyCode::Enter | KeyCode::Char(' ') => {
                let field = self.t.daq_field;
                if let Some(s) = self.t.daq.sources.get_mut(self.t.daq_sel) {
                    if !s.is_smem() {
                        return;
                    }
                    let key = tools::SMEM_KEYS[field];
                    if key == "metadata_only" || key == "eager_start" {
                        let v = s.get(key).map(|v| v == "true").unwrap_or(key == "eager_start");
                        s.set(key, Some((!v).to_string()));
                    } else {
                        let cur = s.get(key).unwrap_or("").to_string();
                        self.ask(&format!("{} (empty: unset)", key), cur, PromptKind::DaqField(key.to_string()));
                    }
                }
            }
            KeyCode::Char('S') => {
                if !self.need_write() {
                    return;
                }
                let state = self.t.daq_state.lock().unwrap().0.clone();
                if state.as_deref() == Some("Acquiring") {
                    self.confirm_action("finish the DAQ session?".into(), Action::DaqFinish);
                } else if self.t.daq.sources.is_empty() {
                    self.say("no source: a adds the selected SHM, f a file".into());
                } else {
                    self.confirm_action(format!("start a DAQ session with {} sources?", self.t.daq.sources.len()), Action::DaqStart);
                }
            }
            KeyCode::Char('R') => {
                if self.need_write() {
                    self.confirm_action("recover the DAQ server?".into(), Action::DaqRecover);
                }
            }
            _ => {}
        }
    }

    /// select an SHM in the list (and the View tab)
    pub fn show_path(&mut self, p: &Path) {
        self.reg.scan();
        let vis = self.visible();
        if let Some(i) = vis.iter().position(|q| q == p) {
            self.list_sel = i;
            self.select_index();
            self.tab = Tab::View;
            self.focus = Focus::View;
        } else {
            self.say(format!("{} is not in the list (filter?)", p.display()));
        }
    }
}

fn shellexpand(s: &str) -> String {
    let s = s.trim();
    if let Some(rest) = s.strip_prefix("~/") {
        if let Ok(h) = std::env::var("HOME") {
            return format!("{}/{}", h, rest);
        }
    }
    let mut out = String::new();
    let mut it = s.chars().peekable();
    while let Some(c) = it.next() {
        if c == '$' {
            let mut name = String::new();
            while let Some(&n) = it.peek() {
                if n.is_alphanumeric() || n == '_' {
                    name.push(n);
                    it.next();
                } else {
                    break;
                }
            }
            out.push_str(&std::env::var(&name).unwrap_or_default());
        } else {
            out.push(c);
        }
    }
    out
}
