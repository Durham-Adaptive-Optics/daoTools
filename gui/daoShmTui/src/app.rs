//! State and keys.

use std::collections::VecDeque;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};

use crate::cmap::{Cmap, Scale};
use crate::tabs::{Action, Prompt, Tab, ToolState};
use crate::shm::{now_epoch, npy_bytes, Frame, Part, Registry};

#[derive(Clone, Copy, PartialEq)]
pub enum Focus {
    List,
    View,
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Mode {
    Auto,
    Image,
    Line,
    Bars,
    Table,
    Value,
}

impl Mode {
    pub fn next(self) -> Mode {
        match self {
            Mode::Auto => Mode::Image,
            Mode::Image => Mode::Line,
            Mode::Line => Mode::Bars,
            Mode::Bars => Mode::Table,
            Mode::Table => Mode::Value,
            Mode::Value => Mode::Auto,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Mode::Auto => "auto",
            Mode::Image => "image",
            Mode::Line => "line",
            Mode::Bars => "bars",
            Mode::Table => "table",
            Mode::Value => "value",
        }
    }
}

#[derive(Clone, Copy, PartialEq)]
pub enum RangeMode {
    MinMax,
    P999,
    P995,
    P99,
    Sym,
    Locked,
}

impl RangeMode {
    pub fn next(self) -> RangeMode {
        match self {
            RangeMode::MinMax => RangeMode::P999,
            RangeMode::P999 => RangeMode::P995,
            RangeMode::P995 => RangeMode::P99,
            RangeMode::P99 => RangeMode::Sym,
            RangeMode::Sym => RangeMode::MinMax,
            RangeMode::Locked => RangeMode::MinMax,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            RangeMode::MinMax => "min-max",
            RangeMode::P999 => "99.9%",
            RangeMode::P995 => "99.5%",
            RangeMode::P99 => "99%",
            RangeMode::Sym => "±max",
            RangeMode::Locked => "locked",
        }
    }
}

#[derive(Clone, Copy, PartialEq)]
pub enum Strip {
    Off,
    Trend,
    Hist,
}

#[derive(Clone, Copy, PartialEq)]
pub enum Trend {
    Mean,
    Std,
    MinMax,
    Pixel,
    Frames,
}

impl Trend {
    pub fn next(self) -> Trend {
        match self {
            Trend::Mean => Trend::Std,
            Trend::Std => Trend::MinMax,
            Trend::MinMax => Trend::Pixel,
            Trend::Pixel => Trend::Frames,
            Trend::Frames => Trend::Mean,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Trend::Mean => "mean",
            Trend::Std => "std",
            Trend::MinMax => "min / mean / max",
            Trend::Pixel => "cursor pixel",
            Trend::Frames => "frames per update (Δcnt0)",
        }
    }
}

#[derive(Clone, Copy, PartialEq)]
pub enum Sort {
    Name,
    Rate,
    Size,
}

#[derive(Clone, Copy, PartialEq)]
pub enum Popup {
    None,
    Help,
    Info,
}

#[derive(Clone, Copy, Default)]
pub struct Stats {
    pub min: f64,
    pub max: f64,
    pub mean: f64,
    pub std: f64,
    pub nonfinite: usize,
    pub n: usize,
}

pub fn stats(d: &[f64]) -> Stats {
    let (mut mn, mut mx, mut s, mut s2, mut n, mut bad) = (f64::INFINITY, f64::NEG_INFINITY, 0.0, 0.0, 0usize, 0usize);
    for &v in d {
        if !v.is_finite() {
            bad += 1;
            continue;
        }
        mn = mn.min(v);
        mx = mx.max(v);
        s += v;
        s2 += v * v;
        n += 1;
    }
    if n == 0 {
        return Stats { nonfinite: bad, ..Default::default() };
    }
    let mean = s / n as f64;
    Stats { min: mn, max: mx, mean, std: (s2 / n as f64 - mean * mean).max(0.0).sqrt(), nonfinite: bad, n }
}

fn percentiles(d: &[f64], a: f64, b: f64) -> Option<(f64, f64)> {
    let step = (d.len() / 50_000).max(1);
    let mut s: Vec<f64> = d.iter().step_by(step).copied().filter(|v| v.is_finite()).collect();
    if s.is_empty() {
        return None;
    }
    s.sort_by(|x, y| x.partial_cmp(y).unwrap());
    let m = s.len() - 1;
    Some((s[(a * m as f64).floor() as usize], s[(b * m as f64).ceil() as usize]))
}

pub struct HistPoint {
    pub t: f64,
    pub a: f64,
    pub b: f64,
    pub c: f64,
}

pub struct App {
    pub reg: Registry,
    pub host: String,
    pub focus: Focus,
    pub filter: String,
    pub filtering: bool,
    pub sort: Sort,
    pub list_sel: usize,
    pub selected: Option<PathBuf>,

    pub frame: Option<Frame>,
    pub view: Vec<f64>,
    pub stats: Stats,
    pub notice: Option<String>,

    pub mode: Mode,
    pub cmap: Cmap,
    pub rev: bool,
    pub scale: Scale,
    pub range: RangeMode,
    pub locked: Option<(f64, f64)>,
    pub last_range: (f64, f64),
    pub rot: u8,
    pub flipx: bool,
    pub flipy: bool,
    pub slice: usize,
    pub part: Part,
    pub avg: usize,
    avg_buf: VecDeque<Vec<f64>>,
    avg_sum: Vec<f64>,
    pub reference: Option<Vec<f64>>,

    pub zoom: f64,
    pub cursor: (usize, usize), // displayed (row, col)
    pub table_off: (usize, usize),

    pub strip: Strip,
    pub trend: Trend,
    pub hist: VecDeque<HistPoint>,

    pub paused: bool,
    pub fps: f64,
    last_read: Instant,
    last_cnt0: Option<u64>,
    force: bool,
    pub shown: VecDeque<Instant>,

    pub popup: Popup,
    pub info: Vec<(String, String)>,
    pub sems: Option<Vec<(usize, Vec<(i32, String)>)>>,
    pub nsem: usize,
    last_info: Instant,

    pub message: Option<(String, Instant)>,
    last_scan: Instant,
    pub truecolor: bool,

    pub tab: Tab,
    pub prompt: Option<Prompt>,
    pub confirm: Option<(String, Action)>,
    pub t: ToolState,
}

const HIST_LEN: usize = 600;

impl App {
    pub fn new(reg: Registry, fps: f64, truecolor: bool, open: Option<String>, write: bool, snapdir: PathBuf) -> App {
        let host = std::fs::read_to_string("/proc/sys/kernel/hostname").map(|s| s.trim().to_string()).unwrap_or_default();
        let mut app = App {
            reg,
            host,
            focus: Focus::List,
            filter: String::new(),
            filtering: false,
            sort: Sort::Name,
            list_sel: 0,
            selected: None,
            frame: None,
            view: Vec::new(),
            stats: Stats::default(),
            notice: None,
            mode: Mode::Auto,
            cmap: Cmap::Viridis,
            rev: false,
            scale: Scale::Lin,
            range: RangeMode::MinMax,
            locked: None,
            last_range: (0.0, 1.0),
            rot: 0,
            flipx: false,
            flipy: false,
            slice: 0,
            part: Part::Abs,
            avg: 1,
            avg_buf: VecDeque::new(),
            avg_sum: Vec::new(),
            reference: None,
            zoom: 1.0,
            cursor: (0, 0),
            table_off: (0, 0),
            strip: Strip::Off,
            trend: Trend::Mean,
            hist: VecDeque::new(),
            paused: false,
            fps,
            last_read: Instant::now() - Duration::from_secs(10),
            last_cnt0: None,
            force: true,
            shown: VecDeque::new(),
            popup: Popup::None,
            info: Vec::new(),
            sems: None,
            nsem: 0,
            last_info: Instant::now() - Duration::from_secs(10),
            message: None,
            last_scan: Instant::now(),
            truecolor,
            tab: Tab::View,
            prompt: None,
            confirm: None,
            t: ToolState::new(write, snapdir),
        };
        if let Some(name) = open {
            let names = app.visible();
            if let Some(i) = names.iter().position(|p| {
                p.to_string_lossy() == name || base(p) == name.trim_end_matches(".im.shm")
            }) {
                app.list_sel = i;
                app.select_current();
                app.focus = Focus::View;
            } else {
                app.say(format!("no SHM {}", name));
            }
        }
        if app.selected.is_none() && !app.visible().is_empty() {
            app.select_current();
        }
        app
    }

    pub fn say(&mut self, m: String) {
        self.message = Some((m, Instant::now()));
    }

    /// The SHM paths shown in the list, filtered and sorted
    pub fn visible(&self) -> Vec<PathBuf> {
        let terms: Vec<String> = self.filter.to_lowercase().split_whitespace().map(|s| s.to_string()).collect();
        let mut v: Vec<&crate::shm::ShmFile> = self
            .reg
            .files
            .iter()
            .filter(|f| {
                let p = f.path.to_string_lossy().to_lowercase();
                terms.iter().all(|t| p.contains(t.as_str()))
            })
            .collect();
        match self.sort {
            Sort::Name => {}
            Sort::Rate => v.sort_by(|a, b| b.rate.partial_cmp(&a.rate).unwrap_or(std::cmp::Ordering::Equal)),
            Sort::Size => v.sort_by(|a, b| b.nelement.cmp(&a.nelement)),
        }
        v.into_iter().map(|f| f.path.clone()).collect()
    }

    pub fn select_index(&mut self) {
        self.select_current();
    }

    fn select_current(&mut self) {
        let vis = self.visible();
        if vis.is_empty() {
            return;
        }
        self.list_sel = self.list_sel.min(vis.len() - 1);
        let p = vis[self.list_sel].clone();
        if self.selected.as_ref() != Some(&p) {
            self.selected = Some(p);
            self.reset_view();
        }
    }

    fn reset_view(&mut self) {
        self.frame = None;
        self.view.clear();
        self.avg_buf.clear();
        self.avg_sum.clear();
        self.reference = None;
        self.hist.clear();
        self.slice = 0;
        self.zoom = 1.0;
        self.cursor = (0, 0);
        self.table_off = (0, 0);
        self.last_cnt0 = None;
        self.force = true;
        self.mode = Mode::Auto; // each SHM in the view that suits it
        if self.range == RangeMode::Locked {
            self.range = RangeMode::MinMax;
        }
        self.last_info = Instant::now() - Duration::from_secs(10);
    }

    /// Called often: rescans, reads a new frame when due, refreshes the info popup.
    pub fn tick(&mut self) {
        self.tools_tick();
        if self.last_scan.elapsed() >= Duration::from_secs(1) {
            self.reg.scan();
            self.last_scan = Instant::now();
            let vis = self.visible();
            if let Some(sel) = &self.selected {
                if let Some(i) = vis.iter().position(|p| p == sel) {
                    self.list_sel = i;
                }
            }
        }
        if let Some((_, t)) = &self.message {
            if t.elapsed() > Duration::from_secs(4) {
                self.message = None;
            }
        }
        let path = match &self.selected {
            Some(p) => p.clone(),
            None => return,
        };
        let f = match self.reg.get(&path) {
            Some(f) => f,
            None => {
                self.notice = Some("the SHM is gone: waiting for it to come back".into());
                self.force = true;
                return;
            }
        };
        if (self.popup == Popup::Info || self.tab == Tab::Sems) && self.last_info.elapsed() >= Duration::from_secs(1) {
            self.info = f.meta();
            self.sems = f.sem_users();
            self.nsem = f.nsem;
            self.last_info = Instant::now();
        }
        if !f.readable {
            self.notice = Some(if f.gpu {
                "GPU SHM without a host copy (DAO_GPU_MIRROR): its data cannot be read from the file".into()
            } else {
                "this SHM cannot be read".into()
            });
            return;
        }
        if self.paused && !self.force {
            return;
        }
        if !self.force && self.last_read.elapsed().as_secs_f64() < 1.0 / self.fps {
            return;
        }
        let c = f.cnt0();
        if !self.force && Some(c) == self.last_cnt0 {
            return;
        }
        if let Some(fr) = f.read_frame(self.slice, self.part) {
            let dcnt = self.last_cnt0.map(|l| fr.cnt0.saturating_sub(l) as f64).unwrap_or(0.0);
            self.last_cnt0 = Some(fr.cnt0);
            self.notice = None;
            let reshaped = self.frame.as_ref().map(|o| o.rows != fr.rows || o.cols != fr.cols || o.dtype != fr.dtype).unwrap_or(true);
            if reshaped {
                self.avg_buf.clear();
                self.avg_sum.clear();
                if self.reference.as_ref().map(|r| r.len() != fr.data.len()).unwrap_or(false) {
                    self.reference = None;
                }
            }
            self.frame = Some(fr);
            self.process();
            self.push_hist(dcnt);
            let now = Instant::now();
            self.shown.push_back(now);
            while self.shown.front().map(|t| now.duration_since(*t) > Duration::from_secs(2)).unwrap_or(false) {
                self.shown.pop_front();
            }
            self.clamp_cursor();
        }
        self.last_read = Instant::now();
        self.force = false;
    }

    fn process(&mut self) {
        let fr = match &self.frame {
            Some(f) => f,
            None => return,
        };
        let n = fr.data.len();
        let mut cur: Vec<f64> = if self.avg > 1 {
            if self.avg_sum.len() != n {
                self.avg_sum = vec![0.0; n];
                self.avg_buf.clear();
            }
            for (s, v) in self.avg_sum.iter_mut().zip(&fr.data) {
                *s += v;
            }
            self.avg_buf.push_back(fr.data.clone());
            while self.avg_buf.len() > self.avg {
                if let Some(old) = self.avg_buf.pop_front() {
                    for (s, v) in self.avg_sum.iter_mut().zip(&old) {
                        *s -= v;
                    }
                }
            }
            let k = 1.0 / self.avg_buf.len() as f64;
            self.avg_sum.iter().map(|s| s * k).collect()
        } else {
            fr.data.clone()
        };
        if let Some(r) = &self.reference {
            if r.len() == n {
                for (c, rv) in cur.iter_mut().zip(r) {
                    *c -= rv;
                }
            }
        }
        self.stats = stats(&cur);
        self.view = cur;
    }

    fn push_hist(&mut self, dcnt: f64) {
        let st = self.stats;
        let (a, b, c) = match self.trend {
            Trend::Mean => (st.mean, f64::NAN, f64::NAN),
            Trend::Std => (st.std, f64::NAN, f64::NAN),
            Trend::MinMax => (st.min, st.mean, st.max),
            Trend::Pixel => (self.cursor_value().unwrap_or(f64::NAN), f64::NAN, f64::NAN),
            Trend::Frames => (dcnt, f64::NAN, f64::NAN),
        };
        let a = if self.effective_mode() == Mode::Value && self.trend != Trend::Frames {
            self.view.first().copied().unwrap_or(f64::NAN)
        } else {
            a
        };
        self.hist.push_back(HistPoint { t: now_epoch(), a, b, c });
        while self.hist.len() > HIST_LEN {
            self.hist.pop_front();
        }
    }

    /* ---------------- geometry */

    pub fn src_dims(&self) -> (usize, usize) {
        self.frame.as_ref().map(|f| (f.rows, f.cols)).unwrap_or((0, 0))
    }

    /// displayed (rows, cols), after rotation
    pub fn disp_dims(&self) -> (usize, usize) {
        let (r, c) = self.src_dims();
        if self.rot % 2 == 1 {
            (c, r)
        } else {
            (r, c)
        }
    }

    /// displayed (y, x) -> source (r, c)
    pub fn to_src(&self, y: usize, x: usize) -> (usize, usize) {
        let (rows, cols) = self.src_dims();
        let (h, w) = self.disp_dims();
        let x = if self.flipx { w - 1 - x } else { x };
        let y = if self.flipy { h - 1 - y } else { y };
        match self.rot % 4 {
            0 => (y, x),
            1 => (rows - 1 - x, y),
            2 => (rows - 1 - y, cols - 1 - x),
            _ => (x, cols - 1 - y),
        }
    }

    pub fn is_1d(&self) -> bool {
        let (r, c) = self.src_dims();
        r == 1 || c == 1
    }

    pub fn effective_mode(&self) -> Mode {
        if self.mode != Mode::Auto {
            return self.mode;
        }
        if self.view.len() == 1 {
            Mode::Value
        } else if self.is_1d() {
            Mode::Line
        } else {
            Mode::Image
        }
    }

    pub fn value_at_src(&self, r: usize, c: usize) -> Option<f64> {
        let (rows, cols) = self.src_dims();
        if r < rows && c < cols {
            self.view.get(r * cols + c).copied()
        } else {
            None
        }
    }

    pub fn cursor_src(&self) -> Option<(usize, usize)> {
        let (h, w) = self.disp_dims();
        if h == 0 || w == 0 {
            return None;
        }
        if self.effective_mode() == Mode::Image {
            Some(self.to_src(self.cursor.0.min(h - 1), self.cursor.1.min(w - 1)))
        } else {
            // vectors and tables: the cursor is (row, col) of the source
            let (rows, cols) = self.src_dims();
            Some((self.cursor.0.min(rows - 1), self.cursor.1.min(cols - 1)))
        }
    }

    pub fn cursor_value(&self) -> Option<f64> {
        let (r, c) = self.cursor_src()?;
        self.value_at_src(r, c)
    }

    fn clamp_cursor(&mut self) {
        let (h, w) = if self.effective_mode() == Mode::Image { self.disp_dims() } else { self.src_dims() };
        if h > 0 && w > 0 {
            self.cursor.0 = self.cursor.0.min(h - 1);
            self.cursor.1 = self.cursor.1.min(w - 1);
        }
    }

    /// the display range
    pub fn range(&mut self) -> (f64, f64) {
        let st = self.stats;
        let (mut lo, mut hi) = match self.range {
            RangeMode::Locked => self.locked.unwrap_or((st.min, st.max)),
            RangeMode::P999 => percentiles(&self.view, 0.0005, 0.9995).unwrap_or((st.min, st.max)),
            RangeMode::P995 => percentiles(&self.view, 0.0025, 0.9975).unwrap_or((st.min, st.max)),
            RangeMode::P99 => percentiles(&self.view, 0.005, 0.995).unwrap_or((st.min, st.max)),
            RangeMode::Sym => {
                let m = st.min.abs().max(st.max.abs());
                (-m, m)
            }
            RangeMode::MinMax => (st.min, st.max),
        };
        if !lo.is_finite() || !hi.is_finite() {
            lo = 0.0;
            hi = 1.0;
        }
        if hi - lo <= 1e-5 * lo.abs().max(hi.abs()) {
            // flat frame (or float rounding only): one colour, not its noise stretched
            let c = (lo + hi) / 2.0;
            let w = if c != 0.0 { c.abs() * 0.01 } else { 0.5 };
            lo = c - w;
            hi = c + w;
        }
        self.last_range = (lo, hi);
        (lo, hi)
    }

    /* ---------------- keys */

    /// false: quit
    pub fn key(&mut self, k: KeyEvent) -> bool {
        if self.confirm.is_some() {
            self.confirm_key(k);
            return true;
        }
        if self.prompt.is_some() {
            self.prompt_key(k);
            return true;
        }
        if self.filtering {
            match k.code {
                KeyCode::Esc => {
                    self.filtering = false;
                    self.filter.clear();
                }
                KeyCode::Enter => self.filtering = false,
                KeyCode::Backspace => {
                    self.filter.pop();
                }
                KeyCode::Char(c) => self.filter.push(c),
                _ => {}
            }
            self.list_sel = 0;
            self.select_current();
            return true;
        }
        if self.popup != Popup::None {
            match k.code {
                KeyCode::Char('q') | KeyCode::Esc | KeyCode::Char('?') | KeyCode::Char('i') | KeyCode::Enter => {
                    self.popup = Popup::None
                }
                _ => {}
            }
            return true;
        }
        let ctrl = k.modifiers.contains(KeyModifiers::CONTROL);
        match k.code {
            KeyCode::Char('q') => return false,
            KeyCode::Char('c') if ctrl => return false,
            KeyCode::Char('?') => self.popup = Popup::Help,
            KeyCode::Char('i') => {
                self.popup = Popup::Info;
                self.last_info = Instant::now() - Duration::from_secs(10);
            }
            KeyCode::Tab | KeyCode::BackTab => {
                self.focus = if self.focus == Focus::List { Focus::View } else { Focus::List }
            }
            KeyCode::Char('/') => {
                self.filtering = true;
                self.focus = Focus::List;
            }
            KeyCode::Char(' ') => {
                self.paused = !self.paused;
                self.force = !self.paused;
            }
            KeyCode::Char('f') => {
                self.fps = match self.fps as u32 {
                    0..=2 => 5.0,
                    3..=5 => 10.0,
                    6..=10 => 20.0,
                    11..=20 => 30.0,
                    21..=30 => 60.0,
                    _ => 2.0,
                };
                self.say(format!("{} frames per second at most", self.fps));
            }
            KeyCode::Char('w') => self.save(),
            KeyCode::Char(d @ '1'..='7') => {
                self.tab = crate::tabs::TABS[(d as u8 - b'1') as usize].0;
                self.last_info = Instant::now() - Duration::from_secs(10);
            }
            KeyCode::Char('N') => {
                self.write_key('N');
            }
            _ => {
                if self.focus == Focus::List {
                    self.list_key(k);
                } else if self.tab != Tab::View {
                    self.tab_key(k);
                } else if let KeyCode::Char(c) = k.code {
                    if !self.write_key(c) {
                        self.view_key(k);
                    }
                } else {
                    self.view_key(k);
                }
            }
        }
        true
    }

    fn list_key(&mut self, k: KeyEvent) {
        let n = self.visible().len();
        if n == 0 {
            return;
        }
        match k.code {
            KeyCode::Up | KeyCode::Char('k') => self.list_sel = self.list_sel.saturating_sub(1),
            KeyCode::Down | KeyCode::Char('j') => self.list_sel = (self.list_sel + 1).min(n - 1),
            KeyCode::PageUp => self.list_sel = self.list_sel.saturating_sub(15),
            KeyCode::PageDown => self.list_sel = (self.list_sel + 15).min(n - 1),
            KeyCode::Home | KeyCode::Char('g') => self.list_sel = 0,
            KeyCode::End | KeyCode::Char('G') => self.list_sel = n - 1,
            KeyCode::Enter | KeyCode::Right => self.focus = Focus::View,
            KeyCode::Char('s') => {
                self.sort = match self.sort {
                    Sort::Name => Sort::Rate,
                    Sort::Rate => Sort::Size,
                    Sort::Size => Sort::Name,
                };
                let sel = self.selected.clone();
                if let Some(i) = sel.and_then(|s| self.visible().iter().position(|p| *p == s)) {
                    self.list_sel = i;
                }
                return;
            }
            _ => return,
        }
        self.select_current();
    }

    fn view_key(&mut self, k: KeyEvent) {
        let shift = k.modifiers.contains(KeyModifiers::SHIFT);
        let step = if shift { 10 } else { 1 };
        let refresh = |a: &mut App| {
            a.force = true;
        };
        match k.code {
            KeyCode::Left => self.cursor.1 = self.cursor.1.saturating_sub(step),
            KeyCode::Right => self.cursor.1 += step,
            KeyCode::Up => self.cursor.0 = self.cursor.0.saturating_sub(step),
            KeyCode::Down => self.cursor.0 += step,
            KeyCode::Home => self.cursor = (0, 0),
            KeyCode::Esc => self.focus = Focus::List,
            KeyCode::Char('+') | KeyCode::Char('=') => self.zoom = (self.zoom * 1.5).min(256.0),
            KeyCode::Char('-') => self.zoom = (self.zoom / 1.5).max(1.0),
            KeyCode::Char('0') => self.zoom = 1.0,
            KeyCode::Char('m') => self.mode = self.mode.next(),
            KeyCode::Char('c') => self.cmap = self.cmap.next(),
            KeyCode::Char('C') => self.rev = !self.rev,
            KeyCode::Char('s') => self.scale = self.scale.next(),
            KeyCode::Char('r') => self.range = self.range.next(),
            KeyCode::Char('l') => {
                if self.range == RangeMode::Locked {
                    self.range = RangeMode::MinMax;
                } else {
                    self.locked = Some(self.last_range);
                    self.range = RangeMode::Locked;
                }
            }
            KeyCode::Char('o') => {
                self.rot = (self.rot + 1) % 4;
                self.cursor = (0, 0);
            }
            KeyCode::Char('x') => self.flipx = !self.flipx,
            KeyCode::Char('y') => self.flipy = !self.flipy,
            KeyCode::Char(']') => {
                let d = self.frame.as_ref().map(|f| f.depth).unwrap_or(0);
                if d > 0 && self.slice + 1 < d {
                    self.slice += 1;
                    self.avg_buf.clear();
                    self.avg_sum.clear();
                    refresh(self);
                }
            }
            KeyCode::Char('[') => {
                if self.slice > 0 {
                    self.slice -= 1;
                    self.avg_buf.clear();
                    self.avg_sum.clear();
                    refresh(self);
                }
            }
            KeyCode::Char('p') => {
                self.part = self.part.next();
                self.avg_buf.clear();
                self.avg_sum.clear();
                refresh(self);
            }
            KeyCode::Char('a') => {
                self.avg = match self.avg {
                    1 => 4,
                    4 => 16,
                    16 => 64,
                    _ => 1,
                };
                self.avg_buf.clear();
                self.avg_sum.clear();
                self.say(format!("average of the last {} frames shown", self.avg));
            }
            KeyCode::Char('b') => {
                if self.reference.is_some() {
                    self.reference = None;
                    self.say("reference cleared".into());
                } else if let Some(f) = &self.frame {
                    self.reference = Some(f.data.clone());
                    self.say("frame − reference shown (b again: clear)".into());
                }
                self.process();
            }
            KeyCode::Char('t') => {
                self.strip = match self.strip {
                    Strip::Off => Strip::Trend,
                    Strip::Trend => Strip::Hist,
                    Strip::Hist => Strip::Off,
                }
            }
            KeyCode::Char('T') => {
                self.trend = self.trend.next();
                self.hist.clear();
                if self.strip == Strip::Off {
                    self.strip = Strip::Trend;
                }
            }
            _ => {}
        }
        self.clamp_cursor();
    }

    fn save(&mut self) {
        let f = match &self.frame {
            Some(f) => f,
            None => return self.say("no frame to save".into()),
        };
        let name = self.selected.as_ref().map(|p| base(p)).unwrap_or_else(|| "shm".into());
        let file = format!("{}_{}.npy", name, f.cnt0);
        match std::fs::write(&file, npy_bytes(f)) {
            Ok(_) => {
                let cwd = std::env::current_dir().map(|d| d.display().to_string()).unwrap_or_default();
                self.say(format!("saved {}/{}", cwd, file))
            }
            Err(e) => self.say(format!("cannot save {}: {}", file, e)),
        }
    }
}

pub fn base(p: &std::path::Path) -> String {
    let n = p.file_name().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    n.trim_end_matches(".im.shm").to_string()
}
