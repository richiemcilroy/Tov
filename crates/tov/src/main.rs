mod scripts;
mod watch;

use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::Instant;

const HELP: &str = "\
tov — the Tov toolchain

usage:
  tov <file> [args...]                     build (cached) and run a program; `tov run <file>` too
  tov <script> [args...]                   run a package.json script (`tov dev` runs \"dev\")
  tov check [paths...] [--json] [--time]   type-check .tov files (default: current directory)
  tov build [path] [-o out] [--emit-c f]   build a native binary
  tov test [path]                          build and run every `test(...)`
  tov explain <CODE>                       explain a diagnostic code
  tov clean                                empty the build cache (it's also pruned as it grows)
  tov version

A script wins over a file of the same name; `check`, `build`, `test` and `run` are always
tov's own (`tov run test` runs a \"test\" script). There are no packages yet, so `install`
and `add` only explain that.

build options: -O0 | -O1 | -O2 | -O3 (default) | -Os, --time,
               --unchecked (integer overflow wraps instead of trapping, like Rust release builds),
               -g (keep function names and debug info, for debuggers and profilers)
watch:         --watch (with run, test or build: rebuild when a source file changes, and
               restart the program), --no-clear-screen, --watch-kill-signal=SIGTERM
scripts:       --silent (don't print each command before running it)
";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    // Options may come before the command, as in Bun: `tov --watch server.tov`.
    let mut lead = 0;
    while let Some(a) = args.get(lead) {
        if !a.starts_with("--") || matches!(a.as_str(), "--" | "--help" | "--version") {
            break;
        }
        lead += if a == "--watch-kill-signal" { 2 } else { 1 };
    }
    let lead = lead.min(args.len());
    let (flags, rest) = args.split_at(lead);
    let with = |tail: &[String]| -> Vec<String> { flags.iter().chain(tail).cloned().collect() };
    // `tov clean --help`: help, not the command (a program's own `--help` comes after its file)
    if matches!(rest.first().map(|s| s.as_str()), Some("check" | "build" | "run" | "test" | "explain" | "clean")) && matches!(rest.get(1).map(|s| s.as_str()), Some("--help" | "-h")) {
        print!("{HELP}");
        return ExitCode::SUCCESS;
    }
    match rest.first().map(|s| s.as_str()) {
        Some("check") => check(&rest[1..]),
        Some("build") => build_cmd(&with(&rest[1..]), BuildCmd::Build),
        Some("run") => run_cmd(&with(&rest[1..]), false),
        Some("test") => build_cmd(&with(&rest[1..]), BuildCmd::Test),
        Some("explain") => explain(rest.get(1).map(|s| s.as_str())),
        Some("clean") if rest.len() > 1 => {
            eprintln!("error: `tov clean` takes no arguments (it empties the whole build cache)");
            ExitCode::from(2)
        }
        Some("clean") => {
            let freed = tov::build::clean_cache();
            println!("clean: freed {:.1} MB of cached builds ({})", freed as f64 / 1e6, tov::build::cache_dir().display());
            ExitCode::SUCCESS
        }
        // Internal: bundle npm packages (for testing the bundler): `tov __bundle <dir> <spec>... [-o out.js]`.
        Some("__bundle") => bundle_cmd(&rest[1..]),
        // Internal: the npm bundler's tokens of a file between two byte offsets: `tov __lex <file> [from] [to]`.
        Some("__lex") => {
            let a = &rest[1..];
            let Some(src) = a.first().and_then(|f| std::fs::read_to_string(f).ok()) else {
                eprintln!("usage: tov __lex <file> [from] [to]");
                return ExitCode::from(2);
            };
            let from: u32 = a.get(1).and_then(|x| x.parse().ok()).unwrap_or(0);
            let to: u32 = a.get(2).and_then(|x| x.parse().ok()).unwrap_or(u32::MAX);
            match tov::npm::lex::tokenize(&src) {
                Ok(toks) => {
                    for t in toks.iter().filter(|t| t.start >= from && t.start < to) {
                        println!("{} {:?} {:?}", t.start, t.kind, t.text(&src));
                    }
                    ExitCode::SUCCESS
                }
                Err(e) => {
                    eprintln!("error at byte {}: {}", e.pos, e.message);
                    ExitCode::from(1)
                }
            }
        }
        // Bun's package commands: there's nothing for them to do yet, so say so.
        Some(cmd @ ("install" | "i" | "add" | "remove" | "update")) => {
            eprintln!("error: `tov {cmd}` isn't available yet");
            eprintln!("  instead: install npm packages with bun, npm, pnpm or yarn: Tov imports them from node_modules (`import {{ z }} from \"zod\"`)");
            ExitCode::from(1)
        }
        Some("version" | "--version" | "-V") => {
            println!("tov {}", env!("CARGO_PKG_VERSION"));
            ExitCode::SUCCESS
        }
        Some("help" | "--help" | "-h") => {
            print!("{HELP}");
            ExitCode::SUCCESS
        }
        None if flags.is_empty() => {
            print!("{HELP}");
            ExitCode::SUCCESS
        }
        None => {
            eprintln!("error: {} needs a file to run, e.g. `tov {} server.tov`", flags[0], flags.join(" "));
            ExitCode::from(2)
        }
        Some(_) => run_cmd(&with(rest), true),
    }
}

fn check(args: &[String]) -> ExitCode {
    let json = args.iter().any(|a| a == "--json");
    let time = args.iter().any(|a| a == "--time");
    let mut paths: Vec<PathBuf> = args.iter().filter(|a| !a.starts_with("--")).map(PathBuf::from).collect();
    if paths.is_empty() {
        paths.push(PathBuf::from("."));
    }
    let base = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    let start = Instant::now();
    let result = match tov::driver::check_paths(&paths, &base) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("error: {e}");
            return ExitCode::from(2);
        }
    };
    let elapsed = start.elapsed();
    if json {
        println!("{}", tov::diag::render_json(&result.diags, &result.sm, result.files));
    } else {
        print!("{}", tov::diag::render_text(&result.diags, &result.sm));
        let files = if result.files == 1 { "1 file".to_string() } else { format!("{} files", result.files) };
        if result.diags.is_empty() {
            println!("check: ok ({files})");
        } else {
            // Program files only (errors in the standard library are counted, but it isn't a file of yours).
            let mut with_errors: Vec<_> = result.diags.iter().map(|d| d.span.file).filter(|&f| {
                let name = &result.sm.get(f).name;
                !(name.starts_with("std/") || name.starts_with("node:") || name.starts_with('<'))
            }).collect();
            with_errors.sort();
            with_errors.dedup();
            let n = result.diags.len();
            println!("check: {n} error{} in {} of {files}", if n == 1 { "" } else { "s" }, with_errors.len());
        }
    }
    if time {
        let secs = elapsed.as_secs_f64();
        eprintln!("checked {} lines in {:.2} ms ({:.0} lines/s)", result.lines, secs * 1000.0, result.lines as f64 / secs.max(1e-9));
        eprintln!("  load+parse {:.2} ms, check {:.2} ms", result.phases.0.as_secs_f64() * 1000.0, result.phases.1.as_secs_f64() * 1000.0);
    }
    // Exit without dropping the program's data structures; the OS reclaims the memory faster.
    use std::io::Write;
    let _ = std::io::stdout().flush();
    std::process::exit(if result.diags.is_empty() { 0 } else { 1 });
}

fn explain(code: Option<&str>) -> ExitCode {
    let Some(code) = code else {
        eprintln!("usage: tov explain <CODE>");
        return ExitCode::from(2);
    };
    match tov::codes::lookup(code) {
        Some(text) => {
            println!("{}: {text}", code.to_ascii_uppercase());
            ExitCode::SUCCESS
        }
        None => {
            eprintln!("unknown diagnostic code `{code}`");
            ExitCode::from(2)
        }
    }
}

#[derive(PartialEq)]
enum BuildCmd {
    Build,
    Run,
    Test,
}

struct Flags {
    paths: Vec<PathBuf>,
    program_args: Vec<String>,
    out: Option<PathBuf>,
    emit_c: Option<PathBuf>,
    opt: String,
    unchecked: bool,
    symbols: bool,
    time: bool,
    watch: bool,
    clear_screen: bool,
    kill_signal: i32,
    silent: bool,
}

/// For `run`, the first path is the program and everything after it is the program's (as in
/// Bun); otherwise program arguments follow `--`.
fn parse_flags(args: &[String], cmd: &BuildCmd) -> Result<Flags, ExitCode> {
    let mut f = Flags {
        paths: Vec::new(),
        program_args: Vec::new(),
        out: None,
        emit_c: None,
        opt: "-O3".to_string(),
        unchecked: false,
        symbols: false,
        time: false,
        watch: false,
        clear_screen: true,
        kill_signal: watch::SIGTERM,
        silent: false,
    };
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        match a {
            "--" if *cmd != BuildCmd::Run => {
                f.program_args = args[i + 1..].to_vec();
                break;
            }
            "-o" => {
                i += 1;
                f.out = args.get(i).map(PathBuf::from);
            }
            "--emit-c" => {
                i += 1;
                f.emit_c = args.get(i).map(PathBuf::from);
            }
            "--time" => f.time = true,
            "--unchecked" => f.unchecked = true,
            "-g" => f.symbols = true,
            "--watch" => f.watch = true,
            "--no-clear-screen" => f.clear_screen = false,
            "--silent" => f.silent = true,
            "--hot" => {
                eprintln!("error: --hot isn't supported yet: Tov can't swap new code into a running program");
                eprintln!("  instead: --watch rebuilds and restarts the program when a source file changes");
                return Err(ExitCode::from(2));
            }
            o if o == "--watch-kill-signal" || o.starts_with("--watch-kill-signal=") => {
                let value = match o.strip_prefix("--watch-kill-signal=") {
                    Some(v) => v.to_string(),
                    None => {
                        i += 1;
                        args.get(i).cloned().unwrap_or_default()
                    }
                };
                let Some(sig) = watch::parse_signal(&value) else {
                    eprintln!("error: --watch-kill-signal: unknown signal \"{value}\"");
                    eprintln!("  valid: SIGTERM (default), SIGINT, SIGHUP, SIGQUIT, SIGKILL, SIGUSR1, SIGUSR2, or a number");
                    return Err(ExitCode::from(2));
                };
                f.kill_signal = sig;
            }
            o if o.starts_with("-O") => f.opt = o.to_string(),
            o if o.starts_with("--") => {
                eprintln!("error: unknown option `{o}`\n\n{HELP}");
                return Err(ExitCode::from(2));
            }
            p => {
                f.paths.push(PathBuf::from(p));
                if *cmd == BuildCmd::Run {
                    // a `--` right after the name is dropped, as Bun does
                    let rest = &args[i + 1..];
                    f.program_args = rest.strip_prefix(&["--".to_string()][..]).unwrap_or(rest).to_vec();
                    break;
                }
            }
        }
        i += 1;
    }
    Ok(f)
}

/// `tov run <name>` and `tov <name>`: a package.json script by that name wins, as in Bun;
/// otherwise `name` is a program to build and run.
fn run_cmd(args: &[String], bare: bool) -> ExitCode {
    let f = match parse_flags(args, &BuildCmd::Run) {
        Ok(f) => f,
        Err(code) => return code,
    };
    if let Some(name) = f.paths.first().and_then(|p| p.to_str()) {
        let pkg = match scripts::find() {
            Ok(p) => p,
            Err(e) => {
                eprintln!("error: {e}");
                return ExitCode::from(2);
            }
        };
        if let Some(pkg) = pkg.as_ref().filter(|p| p.script(name).is_some()) {
            if f.watch {
                eprintln!("error: --watch applies to a program, not the script \"{name}\"");
                eprintln!("  instead: put it in the script, e.g. \"{name}\": \"tov --watch server.tov\"");
                return ExitCode::from(2);
            }
            return scripts::run(pkg, name, &f.program_args, f.silent);
        }
        // A bare word that's neither a script nor a file is a missing script, as in Bun.
        let looks_like_file = name.contains('/') || name.ends_with(".tov") || name.ends_with(".ts");
        if !Path::new(name).exists() && (bare || !looks_like_file) {
            return scripts::not_found(pkg.as_ref(), name);
        }
    }
    execute(f, BuildCmd::Run)
}

fn build_cmd(args: &[String], cmd: BuildCmd) -> ExitCode {
    match parse_flags(args, &cmd) {
        Ok(f) => execute(f, cmd),
        Err(code) => code,
    }
}

fn execute(f: Flags, cmd: BuildCmd) -> ExitCode {
    let mut paths = f.paths;
    if paths.is_empty() {
        paths.push(PathBuf::from("."));
    }
    let base = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    let mode = if cmd == BuildCmd::Test { tov::codegen::Mode::Test } else { tov::codegen::Mode::Run };
    let opts = tov::build::Options { mode, unchecked: f.unchecked, opt: f.opt, emit_c: f.emit_c, symbols: f.symbols, install: cmd == BuildCmd::Build };
    let dest = f.out.unwrap_or_else(|| {
        let stem = paths[0].file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| "main".into());
        PathBuf::from(if stem == "." || stem.is_empty() { "main".to_string() } else { stem })
    });
    if f.watch {
        let then = match cmd {
            BuildCmd::Build => watch::Then::Build(dest),
            BuildCmd::Run => watch::Then::Run,
            BuildCmd::Test => watch::Then::Test,
        };
        let wopts = watch::Options { clear_screen: f.clear_screen, kill_signal: f.kill_signal };
        return watch::run(&paths, &base, &opts, &f.program_args, then, &wopts);
    }
    // `tov test dir` with TypeScript test files: each one a program of its own, as `bun test`
    // runs them (one that doesn't build doesn't stop the others)
    if cmd == BuildCmd::Test && opts.emit_c.is_none() {
        let dirs: Vec<PathBuf> = paths.iter().filter(|p| p.is_dir()).cloned().collect();
        let files = if dirs.is_empty() { Vec::new() } else { tov::driver::ts_test_files(&dirs) };
        if !files.is_empty() {
            let mut singles: Vec<PathBuf> = paths.iter().filter(|p| p.is_file()).cloned().collect();
            singles.extend(files);
            return test_files(&singles, &base, &opts);
        }
    }
    // `tov file.ts` again with nothing changed: the binary it built, run straight away
    let manifest = (cmd == BuildCmd::Run && opts.emit_c.is_none() && !f.time && paths.len() == 1 && paths[0].is_file()).then(|| run_manifest(&paths[0], &opts.opt, opts.unchecked, opts.symbols)).flatten();
    if let Some(m) = &manifest
        && let Some(bin) = fresh_binary(m)
    {
        return exec_program(&bin, &f.program_args);
    }
    let built = match tov::build::build(&paths, &base, &opts) {
        Ok(b) => b,
        Err(e) => return report(e),
    };
    if let Some(m) = &manifest
        && !built.npm
    {
        write_manifest(m, &built.binary, &built.sources);
    }
    if f.time {
        let (c, g, cc) = built.timings;
        eprintln!(
            "check {:.1} ms, generate {:.1} ms, C compile {:.1} ms{}",
            c.as_secs_f64() * 1000.0,
            g.as_secs_f64() * 1000.0,
            cc.as_secs_f64() * 1000.0,
            if built.cached { " (cached binary)" } else { "" }
        );
    }
    match cmd {
        BuildCmd::Build => {
            if let Err(e) = tov::build::install(&built, &dest) {
                eprintln!("error: can't write {}: {e}", dest.display());
                return ExitCode::from(2);
            }
            println!("build: {}", dest.display());
            ExitCode::SUCCESS
        }
        BuildCmd::Run | BuildCmd::Test => exec_program(&built.binary, &f.program_args),
    }
}

/// `tov test` on many TypeScript test files: each built (on every core; the build cache keeps
/// unchanged ones) and run on its own, their reports in order, then the totals.
fn test_files(files: &[PathBuf], base: &Path, opts: &tov::build::Options) -> ExitCode {
    use std::sync::atomic::{AtomicUsize, Ordering};
    // (the first builds the runtime others share)
    let next = AtomicUsize::new(1);
    let mut results: Vec<Option<Result<PathBuf, String>>> = (0..files.len()).map(|_| None).collect();
    let build = |f: &PathBuf| match tov::build::build(std::slice::from_ref(f), base, opts) {
        Ok(b) => Ok(b.binary),
        Err(tov::build::BuildError::Diagnostics(sm, diags)) => {
            let n = diags.len();
            Err(format!("{}build: {n} error{}\n", tov::diag::render_text(&diags, &sm), if n == 1 { "" } else { "s" }))
        }
        Err(tov::build::BuildError::Message(m)) => Err(format!("error: {m}\n")),
    };
    results[0] = Some(build(&files[0]));
    let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4).min(files.len());
    let done = std::sync::Mutex::new(Vec::new());
    std::thread::scope(|scope| {
        for _ in 0..threads {
            scope.spawn(|| {
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    if i >= files.len() {
                        break;
                    }
                    let r = build(&files[i]);
                    let out = match r {
                        Ok(bin) => match std::process::Command::new(&bin).output() {
                            Ok(o) => Ok((o.status.success(), format!("{}{}", String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr)))),
                            Err(e) => Err(format!("error: can't run {}: {e}\n", bin.display())),
                        },
                        Err(e) => Err(e),
                    };
                    done.lock().unwrap().push((i, out));
                }
            });
        }
    });
    let mut outs: Vec<Option<Result<(bool, String), String>>> = (0..files.len()).map(|_| None).collect();
    for (i, o) in done.into_inner().unwrap() {
        outs[i] = Some(o);
    }
    outs[0] = Some(match results[0].take().unwrap() {
        Ok(bin) => match std::process::Command::new(&bin).output() {
            Ok(o) => Ok((o.status.success(), format!("{}{}", String::from_utf8_lossy(&o.stdout), String::from_utf8_lossy(&o.stderr)))),
            Err(e) => Err(format!("error: can't run {}: {e}\n", bin.display())),
        },
        Err(e) => Err(e),
    });
    let (mut passed, mut failed, mut broken) = (0u64, 0u64, 0usize);
    for (f, o) in files.iter().zip(outs) {
        let shown = f.strip_prefix(base).unwrap_or(f).display().to_string();
        println!("{shown}:");
        match o.unwrap() {
            Ok((_, text)) => {
                // ("<n> passed, <m> failed" ends a test program's report)
                let mut lines: Vec<&str> = text.lines().collect();
                let counts = lines.last().and_then(|l| {
                    let (p, f) = l.split_once(" passed, ")?;
                    Some((p.parse::<u64>().ok()?, f.strip_suffix(" failed")?.parse::<u64>().ok()?))
                });
                match counts {
                    Some((p, f)) => {
                        lines.pop();
                        passed += p;
                        failed += f;
                    }
                    None => failed += 1,
                }
                for l in lines {
                    println!("{l}");
                }
            }
            Err(e) => {
                broken += 1;
                print!("{e}");
            }
        }
    }
    let n = files.len();
    let mut summary = format!("{passed} passed, {failed} failed across {n} file{}", if n == 1 { "" } else { "s" });
    if broken > 0 {
        summary.push_str(&format!(" ({broken} didn't build)"));
    }
    println!("{summary}");
    if failed > 0 || broken > 0 { ExitCode::from(1) } else { ExitCode::SUCCESS }
}

/// Runs the program in tov's place: signals reach it, `timeout tov run x` stops it, and its
/// exit status is the command's.
fn exec_program(binary: &Path, args: &[String]) -> ExitCode {
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        let e = std::process::Command::new(binary).args(args).exec();
        eprintln!("error: can't run {}: {e}", binary.display());
        return ExitCode::from(2);
    }
    #[allow(unreachable_code)]
    let status = std::process::Command::new(binary).args(args).status();
    match status {
        Ok(s) => match s.code() {
            Some(c) => ExitCode::from(c.clamp(0, 255) as u8),
            None => ExitCode::from(134),
        },
        Err(e) => {
            eprintln!("error: can't run {}: {e}", binary.display());
            ExitCode::from(2)
        }
    }
}

/// Where `tov run` notes what it last built from `entry` with these options (and this tov, and
/// these TOV_* and CC settings): the binary and every source file's size and modification time.
fn run_manifest(entry: &Path, opt: &str, unchecked: bool, symbols: bool) -> Option<PathBuf> {
    let abs = std::path::absolute(entry).ok()?;
    let exe = std::env::current_exe().ok()?;
    let em = std::fs::metadata(&exe).ok()?;
    let stamp = |m: &std::fs::Metadata| m.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map(|d| d.as_nanos()).unwrap_or(0);
    let mut env: Vec<(String, String)> = std::env::vars().filter(|(k, _)| k.starts_with("TOV_") || k == "CC").collect();
    env.sort();
    let desc = format!("{}\n{opt} {unchecked} {symbols}\n{} {} {}\n{env:?}", abs.display(), exe.display(), em.len(), stamp(&em));
    Some(tov::build::cache_dir().join("run").join(tov::build::hash_hex(&[desc.as_bytes()])))
}

/// The binary a manifest names, if it's there and no source changed since.
fn fresh_binary(manifest: &Path) -> Option<PathBuf> {
    let text = std::fs::read_to_string(manifest).ok()?;
    let mut lines = text.lines();
    let bin = PathBuf::from(lines.next()?);
    for line in lines {
        let mut parts = line.splitn(3, ' ');
        let (size, time, path) = (parts.next()?, parts.next()?, parts.next()?);
        let m = std::fs::metadata(path).ok()?;
        let t = m.modified().ok()?.duration_since(std::time::UNIX_EPOCH).ok()?.as_nanos();
        if m.len().to_string() != size || t.to_string() != time {
            return None;
        }
    }
    bin.is_file().then_some(bin)
}

fn write_manifest(manifest: &Path, binary: &Path, sources: &[PathBuf]) {
    let mut text = format!("{}\n", binary.display());
    for p in sources {
        let Ok(m) = std::fs::metadata(p) else { return };
        let Some(t) = m.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()) else { return };
        let Ok(abs) = std::path::absolute(p) else { return };
        text.push_str(&format!("{} {} {}\n", m.len(), t.as_nanos(), abs.display()));
    }
    if let Some(dir) = manifest.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    // (written whole, then renamed: a run reading it never sees half)
    let tmp = manifest.with_extension(format!("tmp{}", std::process::id()));
    if std::fs::write(&tmp, text).is_ok() {
        let _ = std::fs::rename(&tmp, manifest);
    }
}

/// Prints a failed build's diagnostics or message; the exit code for it.
fn report(e: tov::build::BuildError) -> ExitCode {
    match e {
        tov::build::BuildError::Diagnostics(sm, diags) => {
            print!("{}", tov::diag::render_text(&diags, &sm));
            let n = diags.len();
            println!("build: {n} error{}", if n == 1 { "" } else { "s" });
            ExitCode::from(1)
        }
        tov::build::BuildError::Message(m) => {
            eprintln!("error: {m}");
            ExitCode::from(2)
        }
    }
}

fn bundle_cmd(args: &[String]) -> ExitCode {
    let mut out = None;
    let mut rest = Vec::new();
    let mut i = 0;
    while i < args.len() {
        if args[i] == "-o" {
            out = args.get(i + 1).cloned();
            i += 2;
            continue;
        }
        rest.push(args[i].clone());
        i += 1;
    }
    let Some((dir, specs)) = rest.split_first() else {
        eprintln!("usage: tov __bundle <dir> <package>... [--blob] [-o out.js]");
        return ExitCode::from(2);
    };
    // --blob: the form programs embed (see runtime/js.c), instead of one script
    let blob = specs.iter().any(|s| s == "--blob");
    let specs: Vec<String> = specs.iter().filter(|s| *s != "--blob").cloned().collect();
    let t0 = std::time::Instant::now();
    match tov::npm::bundle(std::path::Path::new(dir), &specs) {
        Ok(b) => {
            for w in &b.warnings {
                eprintln!("warning: {w}");
            }
            let bytes = if blob { b.blob() } else { b.script().into_bytes() };
            eprintln!("bundled {} modules, {} bytes in {:.1} ms", b.modules, bytes.len(), t0.elapsed().as_secs_f64() * 1000.0);
            match out {
                Some(p) => {
                    if let Err(e) = std::fs::write(&p, &bytes) {
                        eprintln!("error: can't write {p}: {e}");
                        return ExitCode::from(1);
                    }
                }
                None => {
                    use std::io::Write;
                    let _ = std::io::stdout().write_all(&bytes);
                }
            }
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::from(1)
        }
    }
}
