//! Writes, through libdao (loaded at run time with dlopen, only for --write): daoShmSetData
//! publishes a frame as every writer does (cnt0, timestamp, semaphores). Without libdao the
//! viewer still runs, read-only.

use std::ffi::{c_void, CStr, CString};
use std::path::Path;

type OpenFn = unsafe extern "C" fn(*const libc::c_char, *mut c_void) -> i8;
type CloseFn = unsafe extern "C" fn(*mut c_void) -> i8;
type SetDataFn = unsafe extern "C" fn(*mut c_void, *mut c_void, u32) -> i8;
type CreateFn = unsafe extern "C" fn(*mut c_void, *const libc::c_char, libc::c_long, *mut u32, u8, libc::c_int, libc::c_int) -> i8;

pub struct Dao {
    open: OpenFn,
    close: CloseFn,
    set_data: SetDataFn,
    create: CreateFn,
}

/// IMAGE (dao.h) is 360 bytes today; room to spare for a later, bigger one
const IMAGE_ROOM: usize = 8192;

impl Dao {
    pub fn load() -> Result<Dao, String> {
        let mut tried = vec!["libdao.so".to_string()];
        if let Ok(root) = std::env::var("DAOROOT") {
            tried.push(format!("{}/lib/libdao.so", root));
            tried.push(format!("{}/lib64/libdao.so", root));
        }
        for lib in &tried {
            let c = CString::new(lib.as_str()).unwrap();
            // SAFETY: dlopen / dlsym with valid strings; the symbols' types are dao.h's
            let h = unsafe { libc::dlopen(c.as_ptr(), libc::RTLD_NOW) };
            if h.is_null() {
                continue;
            }
            let sym = |n: &str| -> Result<*mut c_void, String> {
                let cn = CString::new(n).unwrap();
                let p = unsafe { libc::dlsym(h, cn.as_ptr()) };
                if p.is_null() {
                    Err(format!("{} has no {}", lib, n))
                } else {
                    Ok(p)
                }
            };
            unsafe {
                return Ok(Dao {
                    open: std::mem::transmute::<*mut c_void, OpenFn>(sym("daoShmOpen")?),
                    close: std::mem::transmute::<*mut c_void, CloseFn>(sym("daoShmClose")?),
                    set_data: std::mem::transmute::<*mut c_void, SetDataFn>(sym("daoShmSetData")?),
                    create: std::mem::transmute::<*mut c_void, CreateFn>(sym("daoShmCreate")?),
                });
            }
        }
        let err = unsafe { libc::dlerror() };
        let why = if err.is_null() { String::new() } else { unsafe { CStr::from_ptr(err) }.to_string_lossy().into_owned() };
        Err(format!("libdao not found ({}): no writes", why))
    }

    /// Writes a whole frame (raw bytes in the SHM's type, nelement values) as a new frame.
    pub fn write(&self, path: &Path, raw: &[u8], nelement: usize) -> Result<(), String> {
        let c = CString::new(path.to_string_lossy().as_bytes()).map_err(|e| e.to_string())?;
        let mut img = vec![0u64; IMAGE_ROOM / 8];
        let p = img.as_mut_ptr() as *mut c_void;
        // SAFETY: img is zeroed and larger than IMAGE; raw holds nelement values
        unsafe {
            if (self.open)(c.as_ptr(), p) != 0 {
                return Err(format!("daoShmOpen({}) failed", path.display()));
            }
            let r = (self.set_data)(p, raw.as_ptr() as *mut c_void, nelement as u32);
            (self.close)(p);
            if r != 0 {
                return Err(format!("daoShmSetData({}) failed", path.display()));
            }
        }
        Ok(())
    }

    /// Creates (or recreates) an SHM: naxis sizes, dao atype; zero-filled.
    pub fn create(&self, path: &Path, size: &[u32], atype: u8) -> Result<(), String> {
        let c = CString::new(path.to_string_lossy().as_bytes()).map_err(|e| e.to_string())?;
        let mut img = vec![0u64; IMAGE_ROOM / 8];
        let p = img.as_mut_ptr() as *mut c_void;
        let mut sz = size.to_vec();
        // SAFETY: as above
        unsafe {
            if (self.create)(p, c.as_ptr(), sz.len() as libc::c_long, sz.as_mut_ptr(), atype, 1, 0) != 0 {
                return Err(format!("daoShmCreate({}) failed", path.display()));
            }
            (self.close)(p);
        }
        Ok(())
    }
}
