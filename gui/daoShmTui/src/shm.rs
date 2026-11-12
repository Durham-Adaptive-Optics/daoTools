//! DAO SHM files, read-only: the metadata (dao.h IMAGE_METADATA, Linux layout) and the newest
//! frame, through a read-only memory map of the /tmp file. Nothing is written, no semaphore is
//! taken, and a GPU SHM is never attached (its /tmp host copy is read when it keeps one).

use std::collections::HashMap;
use std::fs::File;
use std::os::unix::fs::MetadataExt;
use std::os::unix::io::AsRawFd;
use std::path::{Path, PathBuf};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use memmap2::Mmap;

/* IMAGE_METADATA field offsets (dao.h, DAO_SHM_LAYOUT_VERSION 2, without DATA_PACKED, Linux:
 * no semCounter). Checked against a #[repr(C)] mirror of the struct in the tests below. */
pub const MD_SIZE: usize = 16632;
const OFF_MAGIC: usize = 0;
const OFF_LAYOUT: usize = 4;
const OFF_NAME: usize = 8;
const NAME_LEN: usize = 256;
const OFF_NAXIS: usize = 264;
const OFF_SIZE: usize = 268;
const OFF_NELEMENT: usize = 280;
const OFF_ATYPE: usize = 288;
const OFF_CREATION: usize = 296;
const OFF_ATIME: usize = 312;
const OFF_SEM: usize = 332;
const OFF_CNT0: usize = 336;
const OFF_CNT1: usize = 344;
const OFF_CNT2: usize = 352;
const OFF_WRITE: usize = 360;
const OFF_NBKW: usize = 362;
const OFF_FIFO_SIZE: usize = 16576;
const OFF_FIFO_LAST: usize = 16580;
const OFF_GPU_MAGIC: usize = 16584;
const OFF_GPU_DEVICE: usize = 16588;
const OFF_GPU_FLAGS: usize = 16608;
const OFF_GPU_SIZE: usize = 16616;

pub const DAO_SHM_MAGIC: u32 = 0x4D48_5344;
const DAO_GPU_MAGIC: u32 = 0x5550_4744;
const DAO_GPU_MIRROR: u32 = 0x1;
const SEM_LOCK_BASE: i64 = 1 << 62;

/// dao atype
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum DType {
    U8,
    I8,
    U16,
    I16,
    U32,
    I32,
    U64,
    I64,
    F32,
    F64,
    C64,
    C128,
}

impl DType {
    fn from_atype(a: u8) -> Option<DType> {
        Some(match a {
            1 => DType::U8,
            2 => DType::I8,
            3 => DType::U16,
            4 => DType::I16,
            5 => DType::U32,
            6 => DType::I32,
            7 => DType::U64,
            8 => DType::I64,
            9 => DType::F32,
            10 => DType::F64,
            11 => DType::C64,
            12 => DType::C128,
            _ => return None,
        })
    }
    pub fn size(self) -> usize {
        match self {
            DType::U8 | DType::I8 => 1,
            DType::U16 | DType::I16 => 2,
            DType::U32 | DType::I32 | DType::F32 => 4,
            DType::U64 | DType::I64 | DType::F64 | DType::C64 => 8,
            DType::C128 => 16,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            DType::U8 => "uint8",
            DType::I8 => "int8",
            DType::U16 => "uint16",
            DType::I16 => "int16",
            DType::U32 => "uint32",
            DType::I32 => "int32",
            DType::U64 => "uint64",
            DType::I64 => "int64",
            DType::F32 => "float32",
            DType::F64 => "float64",
            DType::C64 => "complex64",
            DType::C128 => "complex128",
        }
    }
    /// numpy descr, for .npy files
    pub fn descr(self) -> &'static str {
        match self {
            DType::U8 => "|u1",
            DType::I8 => "|i1",
            DType::U16 => "<u2",
            DType::I16 => "<i2",
            DType::U32 => "<u4",
            DType::I32 => "<i4",
            DType::U64 => "<u8",
            DType::I64 => "<i8",
            DType::F32 => "<f4",
            DType::F64 => "<f8",
            DType::C64 => "<c8",
            DType::C128 => "<c16",
        }
    }
    pub fn is_int(self) -> bool {
        !matches!(self, DType::F32 | DType::F64 | DType::C64 | DType::C128)
    }
    pub fn is_complex(self) -> bool {
        matches!(self, DType::C64 | DType::C128)
    }
}

/// Which part of a complex value is shown
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Part {
    Abs,
    Real,
    Imag,
    Phase,
}

impl Part {
    pub fn next(self) -> Part {
        match self {
            Part::Abs => Part::Real,
            Part::Real => Part::Imag,
            Part::Imag => Part::Phase,
            Part::Phase => Part::Abs,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Part::Abs => "|z|",
            Part::Real => "real",
            Part::Imag => "imag",
            Part::Phase => "phase",
        }
    }
}

/// The newest frame of an SHM, as shown: one slice of a cube, one part of a complex value.
pub struct Frame {
    pub shape: Vec<usize>, // the SHM's
    pub rows: usize,       // the view: rows x cols (a vector: 1 x n)
    pub cols: usize,
    pub data: Vec<f64>,
    pub raw: Vec<u8>, // the whole frame as in the SHM (for .npy)
    pub dtype: DType,
    pub cnt0: u64,
    pub cnt1: u64,
    pub cnt2: u64,
    pub ts: f64, // write time, s since the epoch (0: none)
    pub depth: usize, // cube: number of slices, else 0
    pub slice: usize,
}

pub struct ShmFile {
    pub path: PathBuf,
    mm: Mmap,
    file: File,
    ino: u64,
    dev: u64,
    fsize: u64,
    pub naxis: usize,
    pub shape: Vec<usize>,
    pub nelement: usize,
    pub dtype: Option<DType>,
    pub fifo: usize,
    pub nsem: usize,
    pub gpu: bool,
    pub mirror: bool,
    pub readable: bool,
    data_off: usize,
    seg_bytes: usize,
    // update rate, measured by the scan
    pub rate: f64,
    rate_cnt: u64,
    rate_t: Instant,
}

fn rd<const N: usize>(b: &[u8], off: usize) -> [u8; N] {
    let mut a = [0u8; N];
    a.copy_from_slice(&b[off..off + N]);
    a
}

impl ShmFile {
    pub fn open(path: &Path) -> Result<ShmFile, String> {
        let file = File::open(path).map_err(|e| e.to_string())?;
        let meta = file.metadata().map_err(|e| e.to_string())?;
        if (meta.len() as usize) < MD_SIZE {
            return Err("too small for an SHM".into());
        }
        // SAFETY: read-only map of a file other processes write; values are copied out
        // before use and a torn frame is retried (read_frame)
        let mm = unsafe { Mmap::map(&file) }.map_err(|e| e.to_string())?;
        let b = &mm[..];
        if u32::from_le_bytes(rd(b, OFF_MAGIC)) != DAO_SHM_MAGIC {
            return Err("not a dao SHM (or an older layout)".into());
        }
        let naxis = (b[OFF_NAXIS] as usize).clamp(1, 3);
        let shape: Vec<usize> =
            (0..naxis).map(|i| u32::from_le_bytes(rd(b, OFF_SIZE + 4 * i)) as usize).collect();
        let nelement = u64::from_le_bytes(rd(b, OFF_NELEMENT)) as usize;
        let dtype = DType::from_atype(b[OFF_ATYPE]);
        let fifo = (u32::from_le_bytes(rd(b, OFF_FIFO_SIZE)) as usize).max(1);
        let nsem = u16::from_le_bytes(rd(b, OFF_SEM)) as usize;
        let gpu = u32::from_le_bytes(rd(b, OFF_GPU_MAGIC)) == DAO_GPU_MAGIC;
        let mirror = u32::from_le_bytes(rd(b, OFF_GPU_FLAGS)) & DAO_GPU_MIRROR != 0;
        let esize = dtype.map(|d| d.size()).unwrap_or(1);
        let data_off = fifo * MD_SIZE;
        let seg_bytes = nelement * esize;
        let readable = (!gpu || mirror)
            && dtype.is_some()
            && data_off + fifo * seg_bytes <= meta.len() as usize
            && shape.iter().product::<usize>() == nelement;
        let mut f = ShmFile {
            path: path.to_path_buf(),
            mm,
            file,
            ino: meta.ino(),
            dev: meta.dev(),
            fsize: meta.len(),
            naxis,
            shape,
            nelement,
            dtype,
            fifo,
            nsem,
            gpu,
            mirror,
            readable,
            data_off,
            seg_bytes,
            rate: 0.0,
            rate_cnt: 0,
            rate_t: Instant::now(),
        };
        f.rate_cnt = f.cnt0();
        Ok(f)
    }

    fn base(&self) -> *const u8 {
        self.mm.as_ptr()
    }

    /// a field another process may change, read once from the shared memory
    fn vol<T: Copy>(&self, off: usize) -> T {
        // SAFETY: off + size_of::<T>() is within the map (checked at open: MD_SIZE * fifo)
        unsafe { std::ptr::read_volatile(self.base().add(off) as *const T) }
    }

    fn last(&self) -> usize {
        let l = self.vol::<u32>(OFF_FIFO_LAST) as usize;
        if l < self.fifo {
            l
        } else {
            0
        }
    }

    pub fn cnt0(&self) -> u64 {
        self.vol::<u64>(self.last() * MD_SIZE + OFF_CNT0)
    }

    fn ts_of(&self, seg: usize) -> f64 {
        let sec = self.vol::<i64>(seg * MD_SIZE + OFF_ATIME);
        let nsec = self.vol::<i64>(seg * MD_SIZE + OFF_ATIME + 8);
        sec as f64 + nsec as f64 * 1e-9
    }

    /// file removed or recreated (new inode or size)
    pub fn stale(&self) -> bool {
        match std::fs::metadata(&self.path) {
            Ok(m) => m.ino() != self.ino || m.dev() != self.dev || m.len() != self.fsize,
            Err(_) => true,
        }
    }

    pub fn update_rate(&mut self) {
        let now = Instant::now();
        let dt = now.duration_since(self.rate_t).as_secs_f64();
        if dt >= 0.5 {
            let c = self.cnt0();
            self.rate = if c >= self.rate_cnt { (c - self.rate_cnt) as f64 / dt } else { 0.0 };
            self.rate_cnt = c;
            self.rate_t = now;
        }
    }

    /// The newest frame (slice `slice` of a cube, `part` of complex values), copied out and
    /// retried when a write was going on during the copy.
    pub fn read_frame(&self, slice: usize, part: Part) -> Option<Frame> {
        let dtype = self.dtype?;
        if !self.readable {
            return None;
        }
        let mut raw = vec![0u8; self.seg_bytes];
        let mut seg = 0;
        for _ in 0..4 {
            seg = self.last();
            let c = self.vol::<u64>(seg * MD_SIZE + OFF_CNT0);
            let off = self.data_off + seg * self.seg_bytes;
            raw.copy_from_slice(&self.mm[off..off + self.seg_bytes]);
            let c2 = self.vol::<u64>(seg * MD_SIZE + OFF_CNT0);
            if c == c2 && self.last() == seg && self.vol::<u8>(OFF_WRITE) == 0 {
                break;
            }
        }
        let (depth, rows, cols) = match self.shape.len() {
            1 => (0, 1, self.shape[0]),
            2 if self.shape[1] == 1 => (0, 1, self.shape[0]), // (n, 1): a vector
            2 => (0, self.shape[0], self.shape[1]),
            _ => (self.shape[0], self.shape[1], self.shape[2]),
        };
        let slice = if depth > 0 { slice.min(depth - 1) } else { 0 };
        let n = rows * cols;
        let first = slice * n;
        let es = dtype.size();
        let mut data = Vec::with_capacity(n);
        for i in first..first + n {
            let o = i * es;
            let b = &raw[o..o + es];
            let v = match dtype {
                DType::U8 => b[0] as f64,
                DType::I8 => b[0] as i8 as f64,
                DType::U16 => u16::from_le_bytes([b[0], b[1]]) as f64,
                DType::I16 => i16::from_le_bytes([b[0], b[1]]) as f64,
                DType::U32 => u32::from_le_bytes(rd(b, 0)) as f64,
                DType::I32 => i32::from_le_bytes(rd(b, 0)) as f64,
                DType::U64 => u64::from_le_bytes(rd(b, 0)) as f64,
                DType::I64 => i64::from_le_bytes(rd(b, 0)) as f64,
                DType::F32 => f32::from_le_bytes(rd(b, 0)) as f64,
                DType::F64 => f64::from_le_bytes(rd(b, 0)),
                DType::C64 => cpart(f32::from_le_bytes(rd(b, 0)) as f64, f32::from_le_bytes(rd(b, 4)) as f64, part),
                DType::C128 => cpart(f64::from_le_bytes(rd(b, 0)), f64::from_le_bytes(rd(b, 8)), part),
            };
            data.push(v);
        }
        Some(Frame {
            shape: self.shape.clone(),
            rows,
            cols,
            data,
            raw,
            dtype,
            cnt0: self.vol::<u64>(seg * MD_SIZE + OFF_CNT0),
            cnt1: self.vol::<u64>(seg * MD_SIZE + OFF_CNT1),
            cnt2: self.vol::<u64>(seg * MD_SIZE + OFF_CNT2),
            ts: self.ts_of(seg),
            depth,
            slice,
        })
    }

    /// Every metadata field worth showing, as (name, value)
    pub fn meta(&self) -> Vec<(String, String)> {
        let b = &self.mm[..];
        let seg = self.last();
        let name_raw = &b[OFF_NAME..OFF_NAME + NAME_LEN];
        let name = String::from_utf8_lossy(name_raw.split(|&c| c == 0).next().unwrap_or(&[])).into_owned();
        let shape = self.shape.iter().map(|s| s.to_string()).collect::<Vec<_>>().join("x");
        let mut v = vec![
            ("path".into(), self.path.display().to_string()),
            ("name".into(), name),
            ("shape".into(), format!("{} (naxis {})", shape, self.naxis)),
            ("type".into(), format!("{} (atype {})", self.dtype.map(|d| d.name()).unwrap_or("?"), b[OFF_ATYPE])),
            ("elements".into(), self.nelement.to_string()),
            ("bytes".into(), format!("{} per frame, file {}", self.seg_bytes, self.fsize)),
            ("cnt0".into(), self.vol::<u64>(seg * MD_SIZE + OFF_CNT0).to_string()),
            ("cnt1".into(), self.vol::<u64>(seg * MD_SIZE + OFF_CNT1).to_string()),
            ("cnt2".into(), self.vol::<u64>(seg * MD_SIZE + OFF_CNT2).to_string()),
            ("rate".into(), format!("{:.1} Hz", self.rate)),
            ("written".into(), fmt_time(self.ts_of(seg))),
            ("created".into(), fmt_time(f64::from_le_bytes(rd(b, OFF_CREATION)))),
            ("semaphores".into(), self.nsem.to_string()),
            (
                "FIFO".into(),
                if self.fifo > 1 { format!("{} frames, last {}", self.fifo, seg) } else { "no".into() },
            ),
            ("write flag".into(), self.vol::<u8>(OFF_WRITE).to_string()),
            ("keywords".into(), u16::from_le_bytes(rd(b, OFF_NBKW)).to_string()),
            ("layout".into(), u32::from_le_bytes(rd(b, OFF_LAYOUT)).to_string()),
        ];
        if self.gpu {
            v.push((
                "GPU".into(),
                format!(
                    "device {}, {} bytes, {}",
                    i32::from_le_bytes(rd(b, OFF_GPU_DEVICE)),
                    u64::from_le_bytes(rd(b, OFF_GPU_SIZE)),
                    if self.mirror { "host copy kept current" } else { "no host copy" }
                ),
            ));
        }
        v
    }

    /// The semaphores a reader holds: sem -> PIDs (empty: taken, process not visible).
    /// None when this system has no open-file-description locks.
    pub fn sem_users(&self) -> Option<Vec<(usize, Vec<(i32, String)>)>> {
        let fd = self.file.as_raw_fd();
        let mut used: Vec<(usize, Vec<(i32, String)>)> = Vec::new();
        for s in 0..self.nsem {
            // SAFETY: plain struct for fcntl
            let mut fl: libc::flock = unsafe { std::mem::zeroed() };
            fl.l_type = libc::F_WRLCK as libc::c_short;
            fl.l_whence = libc::SEEK_SET as libc::c_short;
            fl.l_start = (SEM_LOCK_BASE + s as i64) as libc::off_t;
            fl.l_len = 1;
            // F_OFD_GETLK (36): does any open file description hold this byte?
            let r = unsafe { libc::fcntl(fd, 36, &mut fl as *mut libc::flock) };
            if r != 0 {
                return None;
            }
            if fl.l_type != libc::F_UNLCK as libc::c_short {
                used.push((s, Vec::new()));
            }
        }
        if used.is_empty() {
            return Some(used);
        }
        let dev = format!("{:02x}:{:02x}:{}", libc::major(self.dev), libc::minor(self.dev), self.ino);
        let mut found: HashMap<usize, Vec<(i32, String)>> = HashMap::new();
        if let Ok(procs) = std::fs::read_dir("/proc") {
            for p in procs.flatten() {
                let pid: i32 = match p.file_name().to_string_lossy().parse() {
                    Ok(v) => v,
                    Err(_) => continue,
                };
                let fds = match std::fs::read_dir(p.path().join("fdinfo")) {
                    Ok(d) => d,
                    Err(_) => continue,
                };
                for fdi in fds.flatten() {
                    let txt = match std::fs::read_to_string(fdi.path()) {
                        Ok(t) => t,
                        Err(_) => continue,
                    };
                    for line in txt.lines() {
                        if line.starts_with("lock:") && line.contains("OFDLCK") && line.contains(&format!(" {} ", dev)) {
                            let start: i64 = line.split_whitespace().rev().nth(1).and_then(|x| x.parse().ok()).unwrap_or(-1);
                            let s = start - SEM_LOCK_BASE;
                            if s >= 0 {
                                let e = found.entry(s as usize).or_default();
                                if !e.iter().any(|(q, _)| *q == pid) {
                                    e.push((pid, process_command(pid)));
                                }
                            }
                        }
                    }
                }
            }
        }
        for (s, users) in used.iter_mut() {
            if let Some(v) = found.remove(s) {
                *users = v;
            }
        }
        Some(used)
    }
}

fn cpart(re: f64, im: f64, part: Part) -> f64 {
    match part {
        Part::Abs => re.hypot(im),
        Part::Real => re,
        Part::Imag => im,
        Part::Phase => im.atan2(re),
    }
}

pub fn process_command(pid: i32) -> String {
    std::fs::read(format!("/proc/{}/cmdline", pid))
        .map(|b| String::from_utf8_lossy(&b).replace('\0', " ").trim().to_string())
        .unwrap_or_default()
}

pub fn now_epoch() -> f64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0)
}

fn fmt_time(t: f64) -> String {
    if t <= 0.0 {
        return "-".into();
    }
    let secs = t as libc::time_t;
    // SAFETY: localtime_r with valid pointers
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe { libc::localtime_r(&secs, &mut tm) };
    format!(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:06}",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        ((t.fract()) * 1e6) as u64
    )
}

/// The SHM files of the watched directories, rescanned every second.
pub struct Registry {
    pub dirs: Vec<PathBuf>,
    pub files: Vec<ShmFile>,           // sorted by path
    pub errors: Vec<(PathBuf, String)>, // files that are not readable SHMs
}

impl Registry {
    pub fn new(dirs: Vec<PathBuf>) -> Registry {
        let mut r = Registry { dirs, files: Vec::new(), errors: Vec::new() };
        r.scan();
        r
    }

    pub fn scan(&mut self) {
        let mut seen: Vec<PathBuf> = Vec::new();
        for d in &self.dirs {
            if let Ok(rd) = std::fs::read_dir(d) {
                for e in rd.flatten() {
                    let p = e.path();
                    if p.to_string_lossy().ends_with(".im.shm") {
                        seen.push(p);
                    }
                }
            }
        }
        seen.sort();
        // drop the vanished and the recreated
        self.files.retain(|f| seen.contains(&f.path) && !f.stale());
        self.errors.retain(|(p, _)| seen.contains(p));
        for p in &seen {
            if self.files.iter().any(|f| &f.path == p) {
                continue;
            }
            match ShmFile::open(p) {
                Ok(f) => {
                    self.errors.retain(|(q, _)| q != p);
                    self.files.push(f);
                }
                Err(e) => {
                    if !self.errors.iter().any(|(q, _)| q == p) {
                        self.errors.push((p.clone(), e));
                    }
                }
            }
        }
        self.files.sort_by(|a, b| a.path.cmp(&b.path));
        for f in self.files.iter_mut() {
            f.update_rate();
        }
    }

    pub fn get(&self, path: &Path) -> Option<&ShmFile> {
        self.files.iter().find(|f| f.path == path)
    }
}

/// .npy (version 1.0) of a frame: the SHM's own type and shape
pub fn npy_bytes(f: &Frame) -> Vec<u8> {
    let shape = match f.shape.len() {
        1 => format!("({},)", f.shape[0]),
        _ => format!("({})", f.shape.iter().map(|s| s.to_string()).collect::<Vec<_>>().join(", ")),
    };
    let mut header = format!("{{'descr': '{}', 'fortran_order': False, 'shape': {}, }}", f.dtype.descr(), shape);
    let total = 10 + header.len() + 1;
    let pad = (64 - total % 64) % 64;
    header.push_str(&" ".repeat(pad));
    header.push('\n');
    let mut out = Vec::with_capacity(10 + header.len() + f.raw.len());
    out.extend_from_slice(b"\x93NUMPY\x01\x00");
    out.extend_from_slice(&(header.len() as u16).to_le_bytes());
    out.extend_from_slice(header.as_bytes());
    out.extend_from_slice(&f.raw);
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{size_of, MaybeUninit};
    use std::ptr::addr_of;

    /// dao.h IMAGE_METADATA (Linux, not packed), to check the offsets above
    #[repr(C)]
    #[allow(dead_code)]
    struct Md {
        magic: u32,
        layout: u32,
        name: [u8; 256],
        naxis: u8,
        size: [u32; 3],
        nelement: u64,
        atype: u8,
        creation_time: f64,
        last_access: f64,
        atime: [i64; 2],
        shared: u8,
        status: u8,
        logflag: u8,
        sem: u16,
        cnt0: u64,
        cnt1: u64,
        cnt2: u64,
        write: u8,
        nbkw: u16,
        last_pos: u32,
        last_nb: u32,
        packet_nb: u32,
        packet_total: u32,
        last_nb_array: [u64; 2024],
        fifo_size: u32,
        fifo_last_written: u32,
        gpu_magic: u32,
        gpu_device: i32,
        gpu_uuid: [u8; 16],
        gpu_flags: u32,
        gpu_size: u64,
        gpu_id: u64,
    }

    #[test]
    fn layout_matches_dao_h() {
        let m = MaybeUninit::<Md>::uninit();
        let p = m.as_ptr();
        let off = |q: *const u8| q as usize - p as usize;
        unsafe {
            assert_eq!(size_of::<Md>(), MD_SIZE);
            assert_eq!(off(addr_of!((*p).layout) as *const u8), OFF_LAYOUT);
            assert_eq!(off(addr_of!((*p).name) as *const u8), OFF_NAME);
            assert_eq!(off(addr_of!((*p).naxis) as *const u8), OFF_NAXIS);
            assert_eq!(off(addr_of!((*p).size) as *const u8), OFF_SIZE);
            assert_eq!(off(addr_of!((*p).nelement) as *const u8), OFF_NELEMENT);
            assert_eq!(off(addr_of!((*p).atype) as *const u8), OFF_ATYPE);
            assert_eq!(off(addr_of!((*p).creation_time) as *const u8), OFF_CREATION);
            assert_eq!(off(addr_of!((*p).atime) as *const u8), OFF_ATIME);
            assert_eq!(off(addr_of!((*p).sem) as *const u8), OFF_SEM);
            assert_eq!(off(addr_of!((*p).cnt0) as *const u8), OFF_CNT0);
            assert_eq!(off(addr_of!((*p).cnt1) as *const u8), OFF_CNT1);
            assert_eq!(off(addr_of!((*p).cnt2) as *const u8), OFF_CNT2);
            assert_eq!(off(addr_of!((*p).write) as *const u8), OFF_WRITE);
            assert_eq!(off(addr_of!((*p).nbkw) as *const u8), OFF_NBKW);
            assert_eq!(off(addr_of!((*p).fifo_size) as *const u8), OFF_FIFO_SIZE);
            assert_eq!(off(addr_of!((*p).fifo_last_written) as *const u8), OFF_FIFO_LAST);
            assert_eq!(off(addr_of!((*p).gpu_magic) as *const u8), OFF_GPU_MAGIC);
            assert_eq!(off(addr_of!((*p).gpu_device) as *const u8), OFF_GPU_DEVICE);
            assert_eq!(off(addr_of!((*p).gpu_flags) as *const u8), OFF_GPU_FLAGS);
            assert_eq!(off(addr_of!((*p).gpu_size) as *const u8), OFF_GPU_SIZE);
        }
    }

    #[test]
    fn npy_header_aligned() {
        let f = Frame {
            shape: vec![3, 4],
            rows: 3,
            cols: 4,
            data: vec![0.0; 12],
            raw: vec![0u8; 48],
            dtype: DType::F32,
            cnt0: 0,
            cnt1: 0,
            cnt2: 0,
            ts: 0.0,
            depth: 0,
            slice: 0,
        };
        let b = npy_bytes(&f);
        let hl = u16::from_le_bytes([b[8], b[9]]) as usize;
        assert_eq!((10 + hl) % 64, 0);
        assert_eq!(b.len(), 10 + hl + 48);
    }
}
