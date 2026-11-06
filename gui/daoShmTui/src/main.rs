//! daoShmTui: the DAO shared memory viewer in a terminal.
//!
//!     daoShmTui                         # the SHMs of /tmp
//!     daoShmTui -d /tmp -d /dev/shm/loop shIm
//!
//! Read-only: the SHM files are mapped read-only, no semaphore is taken, nothing is written to
//! them; a GPU SHM is read through its /tmp host copy when it keeps one. Frames are read when
//! cnt0 changes, at most --fps times a second.

#![allow(non_snake_case)] // daoShmTui, as the other dao tools

mod app;
mod cmap;
mod dao;
mod shm;
mod tabs;
mod tools;
mod ui;

use std::io::{self, stdout};
use std::path::PathBuf;
use std::time::Duration;

use ratatui::backend::CrosstermBackend;
use ratatui::crossterm::event::{self, Event, KeyEventKind};
use ratatui::crossterm::execute;
use ratatui::crossterm::terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen};
use ratatui::Terminal;

const USAGE: &str = "\
daoShmTui: the DAO shared memory viewer in a terminal

usage: daoShmTui [options] [SHM]
  SHM               open this SHM (its name, e.g. shIm, or its path)
  -d, --dir DIR     a directory of *.im.shm files (repeatable; default /tmp)
  --fps N           frames per second at most (default 20; f in the viewer)
  --cmap NAME       viridis inferno magma plasma turbo gray hot coolwarm
  --truecolor       24-bit colour (default when COLORTERM is truecolor or 24bit)
  --256             256 colours (default otherwise)
  --write           allow changes: SHMs (set, random, edit, load .npy, create, through libdao),
                    processes (latency measurements, snapshot loads, DAQ sessions), snapshots
  --snapshot-dir D  daoSnapshot.py files (default $DAODATA/daoSnapshots)
  -h, --help        this help

Without --write: read-only, the files are mapped read-only, no semaphore is taken.
? in the viewer: the keys. Tabs 1-7: view, semaphores, record, latency, snapshots, tmux, DAQ.";

struct Args {
    dirs: Vec<PathBuf>,
    fps: f64,
    cmap: Option<cmap::Cmap>,
    truecolor: bool,
    open: Option<String>,
    write: bool,
    snapdir: PathBuf,
}

fn parse_args() -> Result<Args, String> {
    let ct = std::env::var("COLORTERM").unwrap_or_default();
    let snapdir = PathBuf::from(std::env::var("DAODATA").unwrap_or_else(|_| std::env::var("HOME").unwrap_or_else(|_| ".".into()))).join("daoSnapshots");
    let mut a = Args { dirs: Vec::new(), fps: 20.0, cmap: None, truecolor: ct.contains("truecolor") || ct.contains("24bit"), open: None, write: false, snapdir };
    let mut it = std::env::args().skip(1);
    while let Some(s) = it.next() {
        match s.as_str() {
            "-h" | "--help" => {
                println!("{}", USAGE);
                std::process::exit(0);
            }
            "-d" | "--dir" => a.dirs.push(PathBuf::from(it.next().ok_or("-d needs a directory")?)),
            "--fps" => {
                a.fps = it.next().and_then(|v| v.parse().ok()).filter(|v: &f64| *v > 0.0).ok_or("--fps needs a number > 0")?
            }
            "--cmap" => {
                let n = it.next().ok_or("--cmap needs a name")?;
                a.cmap = Some(cmap::Cmap::from_name(&n).ok_or(format!("no colour map {}", n))?);
            }
            "--256" => a.truecolor = false,
            "--write" => a.write = true,
            "--snapshot-dir" => a.snapdir = PathBuf::from(it.next().ok_or("--snapshot-dir needs a directory")?),
            "--truecolor" => a.truecolor = true,
            x if x.starts_with('-') => return Err(format!("unknown option {}\n\n{}", x, USAGE)),
            x => a.open = Some(x.to_string()),
        }
    }
    if a.dirs.is_empty() {
        a.dirs.push(PathBuf::from("/tmp"));
    }
    Ok(a)
}

/// tmux attach to a session: the terminal is handed over and taken back on detach (inside
/// tmux: switch this client to it instead)
fn attach(term: &mut Terminal<CrosstermBackend<io::Stdout>>, name: &str) -> io::Result<()> {
    let target = format!("={}", name);
    if std::env::var_os("TMUX").is_some() {
        let _ = std::process::Command::new("tmux").args(["switch-client", "-t", &target]).status();
        return Ok(());
    }
    restore();
    let _ = std::process::Command::new("tmux").args(["attach", "-t", &target]).status();
    enable_raw_mode()?;
    execute!(stdout(), EnterAlternateScreen)?;
    term.clear()
}

fn restore() {
    let _ = disable_raw_mode();
    let _ = execute!(stdout(), LeaveAlternateScreen);
}

fn main() -> io::Result<()> {
    let args = match parse_args() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("daoShmTui: {}", e);
            std::process::exit(2);
        }
    };
    let reg = shm::Registry::new(args.dirs.clone());
    let mut app = app::App::new(reg, args.fps, args.truecolor, args.open.clone(), args.write, args.snapdir.clone());
    if let Some(c) = args.cmap {
        app.cmap = c;
    }

    // the terminal is given back on exit and on a panic
    let hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        restore();
        hook(info);
    }));
    enable_raw_mode()?;
    execute!(stdout(), EnterAlternateScreen)?;
    let mut term = Terminal::new(CrosstermBackend::new(stdout()))?;
    term.clear()?;

    let result = (|| -> io::Result<()> {
        loop {
            if let Some(name) = app.t.attach.take() {
                attach(&mut term, &name)?;
            }
            app.tick();
            term.draw(|f| ui::draw(f, &mut app))?;
            // wake for keys, else at the frame rate to look for a new frame
            let wait = Duration::from_secs_f64((1.0 / app.fps).clamp(0.005, 0.25));
            if event::poll(wait)? {
                match event::read()? {
                    Event::Key(k) if k.kind == KeyEventKind::Press => {
                        if !app.key(k) {
                            return Ok(());
                        }
                        // more keys queued (held arrow): handle them before drawing again
                        while event::poll(Duration::ZERO)? {
                            if let Event::Key(k) = event::read()? {
                                if k.kind == KeyEventKind::Press && !app.key(k) {
                                    return Ok(());
                                }
                            }
                        }
                    }
                    Event::Resize(_, _) => {}
                    _ => {}
                }
            }
        }
    })();
    restore();
    result
}
