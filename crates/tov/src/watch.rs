//! `--watch`, as in Bun: build and run the program, then rebuild and restart it whenever one
//! of its source files changes (the entry and everything it imports; for a directory entry,
//! also a `.tov` file added or removed). Files the program reads at run time aren't watched.
//! A failed build prints its errors and waits for the next change. On a terminal the screen is
//! cleared before each restart unless `--no-clear-screen`. The old program gets
//! `--watch-kill-signal` (SIGTERM by default) and SIGKILL if it's still running 2 s later.

use std::io::{IsTerminal, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitCode};
use std::sync::atomic::{AtomicI32, Ordering};
use std::time::{Duration, Instant, SystemTime};

const POLL: Duration = Duration::from_millis(100);

pub enum Then {
    Run,
    Test,
    /// Copy the binary here.
    Build(PathBuf),
}

pub struct Options {
    pub clear_screen: bool,
    pub kill_signal: i32,
}

pub const SIGTERM: i32 = 15;

/// A signal number from `SIGTERM`, `TERM` or `15`.
pub fn parse_signal(s: &str) -> Option<i32> {
    let upper = s.to_ascii_uppercase();
    let name = upper.strip_prefix("SIG").unwrap_or(&upper);
    let usr = if cfg!(target_os = "linux") { (10, 12) } else { (30, 31) };
    let n = match name {
        "HUP" => 1,
        "INT" => 2,
        "QUIT" => 3,
        "KILL" => 9,
        "USR1" => usr.0,
        "USR2" => usr.1,
        "TERM" => 15,
        _ => name.parse().ok().filter(|n| (1..=64).contains(n))?,
    };
    Some(n)
}

/// The SIGHUP, SIGINT or SIGTERM `tov` got: stop the program, then exit by that signal.
static QUIT: AtomicI32 = AtomicI32::new(0);

#[cfg(unix)]
unsafe extern "C" {
    fn signal(sig: i32, handler: usize) -> usize;
    fn raise(sig: i32) -> i32;
}

#[cfg(unix)]
extern "C" fn on_quit(sig: i32) {
    QUIT.store(sig, Ordering::Relaxed);
}

/// Without this, stopping `tov` any way but Ctrl+C would leave the program running.
fn catch_quit() {
    #[cfg(unix)]
    for sig in [1, 2, SIGTERM] {
        unsafe {
            signal(sig, on_quit as extern "C" fn(i32) as usize);
        }
    }
}

/// Exits the way the signal would have without the handler (so a script runner sees it).
fn exit_by(sig: i32) -> ExitCode {
    #[cfg(unix)]
    unsafe {
        signal(sig, 0);
        raise(sig);
    }
    ExitCode::from((128 + sig).clamp(0, 255) as u8)
}

pub fn run(paths: &[PathBuf], base: &Path, build: &tov::build::Options, args: &[String], then: Then, opts: &Options) -> ExitCode {
    if let Some(p) = paths.iter().find(|p| !p.exists()) {
        eprintln!("error: no such file or directory: {}", p.display());
        return ExitCode::from(2);
    }
    catch_quit();
    let dirs: Vec<PathBuf> = paths.iter().filter(|p| p.is_dir()).cloned().collect();
    let mut first = true;
    loop {
        if !first && opts.clear_screen && std::io::stdout().is_terminal() {
            print!("\x1b[2J\x1b[3J\x1b[H");
            let _ = std::io::stdout().flush();
        }
        first = false;
        let (mut child, mut files) = start(paths, base, build, args, &then);
        files.extend(paths.iter().filter(|p| p.is_file()).cloned());
        let mut seen = snapshot(&files, &dirs);
        loop {
            std::thread::sleep(POLL);
            let sig = QUIT.load(Ordering::Relaxed);
            if sig != 0 {
                if let Some(c) = child.take() {
                    stop(c, sig);
                }
                return exit_by(sig);
            }
            if let Some(c) = &mut child {
                if let Ok(Some(_)) = c.try_wait() {
                    child = None;
                }
            }
            let mut now = snapshot(&files, &dirs);
            if now == seen {
                continue;
            }
            // Let a burst of writes (an editor saving several files, a formatter) settle.
            loop {
                std::thread::sleep(Duration::from_millis(30));
                let again = snapshot(&files, &dirs);
                if again == now {
                    break;
                }
                now = again;
            }
            seen = now;
            break;
        }
        if let Some(c) = child.take() {
            stop(c, opts.kill_signal);
        }
    }
}

/// Builds and starts the program; the source files to watch.
fn start(paths: &[PathBuf], base: &Path, build: &tov::build::Options, args: &[String], then: &Then) -> (Option<Child>, Vec<PathBuf>) {
    let built = match tov::build::build(paths, base, build) {
        Ok(b) => b,
        Err(e) => {
            let files = match &e {
                tov::build::BuildError::Diagnostics(sm, _) => tov::build::source_paths(sm),
                tov::build::BuildError::Message(_) => Vec::new(),
            };
            crate::report(e);
            return (None, files);
        }
    };
    if let Then::Build(dest) = then {
        match tov::build::install(&built, dest) {
            Ok(_) => println!("build: {}", dest.display()),
            Err(e) => eprintln!("error: can't write {}: {e}", dest.display()),
        }
        return (None, built.sources);
    }
    match Command::new(&built.binary).args(args).spawn() {
        Ok(c) => (Some(c), built.sources),
        Err(e) => {
            eprintln!("error: can't run {}: {e}", built.binary.display());
            (None, built.sources)
        }
    }
}

fn stop(mut child: Child, sig: i32) {
    #[cfg(unix)]
    {
        unsafe extern "C" {
            fn kill(pid: i32, sig: i32) -> i32;
        }
        unsafe {
            kill(child.id() as i32, sig);
        }
        let deadline = Instant::now() + Duration::from_secs(2);
        while Instant::now() < deadline {
            if let Ok(Some(_)) = child.try_wait() {
                return;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
    }
    let _ = child.kill();
    let _ = child.wait();
}

type Snapshot = (Vec<Option<(SystemTime, u64)>>, Vec<PathBuf>);

/// Each watched file's modification time and size, and the `.tov` files under each
/// directory entry (so adding or removing one counts as a change).
fn snapshot(files: &[PathBuf], dirs: &[PathBuf]) -> Snapshot {
    let stamps = files.iter().map(|f| std::fs::metadata(f).ok().map(|m| (m.modified().unwrap_or(SystemTime::UNIX_EPOCH), m.len()))).collect();
    let mut listed = Vec::new();
    let mut todo: Vec<PathBuf> = dirs.to_vec();
    while let Some(dir) = todo.pop() {
        let Ok(entries) = std::fs::read_dir(&dir) else { continue };
        for entry in entries.flatten() {
            let name = entry.file_name();
            let name = name.to_string_lossy();
            if name.starts_with('.') || name == "target" || name == "node_modules" {
                continue;
            }
            let path = entry.path();
            if path.is_dir() {
                todo.push(path);
            } else if name.ends_with(".tov") {
                listed.push(path);
            }
        }
    }
    listed.sort();
    (stamps, listed)
}
