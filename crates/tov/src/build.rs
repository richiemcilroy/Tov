//! `tov build` / `run` / `test`: check, generate C, compile with the system C compiler.
//!
//! Binaries are cached by a hash of the generated C and the compiler flags, in one per-user
//! directory shared by every checkout and worktree (`$TOV_CACHE_DIR`, else
//! `$XDG_CACHE_HOME/tov`, else `~/.cache/tov`).

use crate::check;
use crate::codegen::{self, Mode};
use crate::diag::Diagnostic;
use crate::driver;
use crate::source::SourceMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant, SystemTime};

/// runtime/tls.c and the vendored BoringSSL, embedded by the Cargo build script (../build.rs).
use crate::tls_flags;

mod tls_files {
    include!(concat!(env!("OUT_DIR"), "/tls_files.rs"));
}

/// The JavaScript bridge and the Node.js natives: linked by programs that import npm packages.
const JS_C: &str = include_str!("../../../runtime/js.c");
const NODE_C: &str = include_str!("../../../runtime/node.c");
/// Node-API (native addons): linked when the bundle holds one.
const NAPI_C: &str = include_str!("../../../runtime/napi.c");
/// Lets JavaScriptCore compile JavaScript to machine code (without it, it only interprets: an
/// order of magnitude slower). Programs that link it are signed with this.
const JIT_ENTITLEMENTS: &str = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict><key>com.apple.security.cs.allow-jit</key><true/></dict></plist>\n";

pub enum BuildError {
    Diagnostics(SourceMap, Vec<Diagnostic>),
    Message(String),
}

pub struct Built {
    pub binary: PathBuf,
    /// It links TLS (the program calls fetch()).
    pub tls: bool,
    /// It links Tov's own JavaScriptCore rather than the system engine.
    pub jsc_own: bool,
    pub cached: bool,
    /// The program's source files on disk (the entry and everything it imports).
    pub sources: Vec<PathBuf>,
    /// (check, generate C, C compile)
    pub timings: (Duration, Duration, Duration),
}

pub struct Options {
    pub mode: Mode,
    /// Integer overflow wraps instead of trapping (like Rust release builds).
    pub unchecked: bool,
    /// C optimization flag.
    pub opt: String,
    /// Also write the generated C here.
    pub emit_c: Option<PathBuf>,
    /// Keep function names and C debug info (`tov build -g`) for debuggers and profilers;
    /// otherwise local symbols are stripped (smaller binaries).
    pub symbols: bool,
}

/// The files on disk behind a source map (not the standard library's built-in modules).
pub fn source_paths(sm: &SourceMap) -> Vec<PathBuf> {
    sm.files.iter().map(|f| f.path.clone()).filter(|p| p.is_file()).collect()
}

/// Whether a cached file is there; if so it's marked as just used (its modification time), which
/// is what pruning goes by.
fn cached(p: &Path) -> bool {
    match std::fs::File::options().append(true).open(p) {
        Ok(f) => {
            let _ = f.set_modified(SystemTime::now());
            true
        }
        Err(_) => p.is_file(),
    }
}

const GIB: u64 = 1 << 30;

/// Keeps the cache in bounds, at most once a day: what interrupted builds left goes, then what
/// hasn't been used for 30 days, then the least recently used while it's over 2 GiB (down to 1.5).
pub fn prune_cache() {
    let dir = cache_dir();
    let stamp = dir.join(".pruned");
    let now = SystemTime::now();
    let age = |t: SystemTime| now.duration_since(t).unwrap_or(Duration::ZERO);
    if std::fs::metadata(&stamp).and_then(|m| m.modified()).is_ok_and(|t| age(t) < Duration::from_secs(86400)) {
        return;
    }
    if std::fs::create_dir_all(&dir).is_err() || std::fs::write(&stamp, b"").is_err() {
        return;
    }
    let mut entries = Vec::new();
    for sub in ["bin", "c"] {
        let Ok(rd) = std::fs::read_dir(dir.join(sub)) else { continue };
        for e in rd.flatten() {
            let Ok(meta) = e.metadata() else { continue };
            let size = if meta.is_dir() { dir_size(&e.path()) } else { meta.len() };
            entries.push((meta.modified().unwrap_or(now), size, e.path(), meta.is_dir()));
        }
    }
    entries.sort_by_key(|e| e.0);
    let mut total: u64 = entries.iter().map(|e| e.1).sum();
    let over = total > 2 * GIB;
    for (modified, size, path, is_dir) in entries {
        let leftover = path.file_name().is_some_and(|n| n.to_string_lossy().contains(".tmp")) && age(modified) > Duration::from_secs(3600);
        if leftover || age(modified) > Duration::from_secs(30 * 86400) || (over && total > 3 * GIB / 2) {
            let removed = if is_dir { std::fs::remove_dir_all(&path) } else { std::fs::remove_file(&path) };
            if removed.is_ok() {
                total = total.saturating_sub(size);
            }
        }
    }
}

fn dir_size(p: &Path) -> u64 {
    std::fs::read_dir(p).map_or(0, |rd| {
        rd.flatten().map(|e| e.metadata().map_or(0, |m| if m.is_dir() { dir_size(&e.path()) } else { m.len() })).sum()
    })
}

/// Empties the cache (what `tov clean` does): the bytes it freed.
pub fn clean_cache() -> u64 {
    let dir = cache_dir();
    let mut freed = 0;
    for sub in ["bin", "c"] {
        let p = dir.join(sub);
        let size = dir_size(&p);
        if std::fs::remove_dir_all(&p).is_ok() {
            freed += size;
        }
    }
    let _ = std::fs::remove_file(dir.join(".pruned"));
    freed
}

pub fn cache_dir() -> PathBuf {
    if let Ok(d) = std::env::var("TOV_CACHE_DIR") {
        return PathBuf::from(d);
    }
    if let Ok(d) = std::env::var("XDG_CACHE_HOME") {
        return PathBuf::from(d).join("tov");
    }
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
    PathBuf::from(home).join(".cache").join("tov")
}

/// The C compiler: `$CC`, else the newest clang we can find (Homebrew LLVM is usually newer
/// than the system compiler, and generates faster code), else `cc`. On macOS, with the SDK to
/// pass as `-isysroot`.
fn c_compiler() -> (String, Option<String>) {
    if let Ok(cc) = std::env::var("CC") {
        return (cc, None);
    }
    // macOS: the system clang generates code as fast as Homebrew's LLVM (bench/run.py) and
    // starts faster, and it links in the same invocation. Called directly rather than through
    // /usr/bin/cc, an xcrun trampoline that adds 10–15 ms to every build.
    if cfg!(target_vendor = "apple") {
        if let Some((clang, sdk)) = apple_toolchain() {
            return (clang, Some(sdk));
        }
        if Path::new("/usr/bin/cc").is_file() {
            return ("/usr/bin/cc".into(), None);
        }
    }
    for candidate in ["/opt/homebrew/opt/llvm/bin/clang", "/usr/local/opt/llvm/bin/clang", "/usr/lib/llvm-21/bin/clang", "/usr/lib/llvm-20/bin/clang"] {
        if Path::new(candidate).is_file() {
            return (candidate.to_string(), None);
        }
    }
    // Any clang before `cc`, which on Linux is usually GCC: Tov's C runs faster from clang
    // (x86-64, Ubuntu's clang 18 against GCC 13: fib 449 ms against 511, nbody 308 against 382)
    if let Some(path) = std::env::var_os("PATH")
        && let Some(clang) = std::env::split_paths(&path).map(|d| d.join("clang")).find(|p| p.is_file())
    {
        return (clang.to_string_lossy().into_owned(), None);
    }
    ("cc".into(), None)
}

/// Xcode's clang and the macOS SDK, found with xcrun once and remembered in the cache.
fn apple_toolchain() -> Option<(String, String)> {
    let file = cache_dir().join("toolchain");
    if let Ok(text) = std::fs::read_to_string(&file) {
        let mut lines = text.lines();
        if let (Some(clang), Some(sdk)) = (lines.next(), lines.next())
            && Path::new(clang).is_file()
            && Path::new(sdk).is_dir()
        {
            return Some((clang.to_string(), sdk.to_string()));
        }
    }
    let run = |args: &[&str]| -> Option<String> {
        let out = Command::new("xcrun").args(args).output().ok()?;
        let s = String::from_utf8(out.stdout).ok()?.trim().to_string();
        (out.status.success() && !s.is_empty()).then_some(s)
    };
    let clang = run(&["--find", "clang"])?;
    let sdk = run(&["--show-sdk-path"])?;
    if !Path::new(&clang).is_file() || !Path::new(&sdk).is_dir() {
        return None;
    }
    let _ = std::fs::create_dir_all(cache_dir());
    let tmp = file.with_extension(format!("tmp{}", std::process::id()));
    if std::fs::write(&tmp, format!("{clang}\n{sdk}\n")).is_ok() {
        let _ = std::fs::rename(&tmp, &file);
    }
    Some((clang, sdk))
}

/// The linker driver. Homebrew clang loads its LTO library on every link (~40 ms), so when we
/// picked it ourselves on macOS, the system driver links instead (same `ld`, same objects).
fn linker(cc: &str) -> String {
    if std::env::var_os("CC").is_none() && cfg!(target_vendor = "apple") && cc.contains("/opt/llvm/") && Path::new("/usr/bin/cc").is_file() {
        return "/usr/bin/cc".into();
    }
    cc.to_string()
}

/// Linux: lld links, when it's installed. It's faster than the system's ld, and `--as-needed`
/// leaves out a library only the runtime functions it drops would call (GNU ld counts their calls
/// too, so every program would load libm).
fn use_lld(cc: &str) -> bool {
    if cfg!(target_vendor = "apple") || cfg!(windows) {
        return false;
    }
    let beside = Path::new(cc).parent().filter(|d| !d.as_os_str().is_empty()).is_some_and(|d| d.join("ld.lld").is_file());
    beside || std::env::var_os("PATH").is_some_and(|path| std::env::split_paths(&path).any(|d| d.join("ld.lld").is_file()))
}

pub fn build(paths: &[PathBuf], base: &Path, opts: &Options) -> Result<Built, BuildError> {
    prune_cache();
    let t0 = Instant::now();
    let loaded = driver::load(paths, base).map_err(BuildError::Message)?;
    let driver::Loaded { sm, mut interner, modules, diags } = loaded;
    let sources = source_paths(&sm);
    if !diags.is_empty() {
        return Err(BuildError::Diagnostics(sm, diags));
    }
    // npm packages resolve from the entry file's directory
    let npm_root = modules.iter().find(|m| m.entry).and_then(|m| m.path.parent()).map(Path::to_path_buf).unwrap_or_else(|| base.to_path_buf());
    let (diags, mut checker) = check::check_for_build(&modules, &mut interner, &sm);
    if !diags.is_empty() {
        drop(checker);
        return Err(BuildError::Diagnostics(sm, diags));
    }
    let t1 = Instant::now();
    let program = match codegen::generate(&mut checker, opts.mode) {
        Ok(p) => p,
        Err(d) => {
            drop(checker);
            return Err(BuildError::Diagnostics(sm, d));
        }
    };
    drop(checker);
    let codegen::Program { c: mut c_src, mut uses_tls, npm } = program;
    let mut native = false;
    // The blob, when it's appended to the binary rather than assembled into it (see with_blob)
    let mut appended_blob: Option<PathBuf> = None;
    // npm packages: bundled into a blob the program embeds (see runtime/js.c)
    if !npm.is_empty() {
        let b = crate::npm::bundle(&npm_root, &npm).map_err(|e| BuildError::Message(format!("can't bundle the npm packages: {e}")))?;
        if std::env::var_os("TOV_TRACE").is_some() {
            for w in &b.warnings {
                eprintln!("npm: {w}");
            }
        }
        // Node.js's crypto and zlib, and fetch(), run on the TLS library's BoringSSL, codecs and TLS
        // client (runtime/crypto.c, compress.c, tls.c): a bundle with them links the library, and
        // installs them before main
        for (module, install) in [("node:crypto", "tv_crypto_install"), ("node:zlib", "tv_zlib_install"), ("node:internal/tov/fetch", "tv_tls_install")] {
            if b.sources.iter().any(|(name, _)| name == module) {
                uses_tls = true;
                c_src.push_str(&format!("__attribute__((constructor)) static void tvg_npm_{}(void) {{ {install}(); }}\n", module[5..].replace('/', "_")));
            }
        }
        let blob = b.blob();
        let c_dir = cache_dir().join("c");
        std::fs::create_dir_all(&c_dir).map_err(|e| BuildError::Message(format!("can't create {}: {e}", c_dir.display())))?;
        let path = c_dir.join(format!("npm-{}.blob", hash_hex(&[&blob])));
        if !cached(&path) {
            let tmp = path.with_extension(format!("tmp{}", std::process::id()));
            std::fs::write(&tmp, &blob).and_then(|_| std::fs::rename(&tmp, &path)).map_err(|e| BuildError::Message(format!("can't write {}: {e}", path.display())))?;
        }
        native = b.native;
        let p = path.to_string_lossy().replace('\\', "\\\\").replace('"', "\\\"");
        if own_jsc_dir().is_some() {
            // (with Tov's own engine, appended to the binary: a build whose JavaScript changed
            // doesn't link the program again)
            c_src.push_str("const unsigned char tv_js_blob[8] = \"TOV_APP\";\n");
            appended_blob = Some(path.clone());
        } else if cfg!(target_vendor = "apple") {
            c_src.push_str(&format!("__asm__(\".section __TEXT,__const\\n.globl _tv_js_blob\\n.p2align 3\\n_tv_js_blob:\\n.incbin \\\"{p}\\\"\\n.byte 0\\n.text\\n\");\n"));
        } else {
            c_src.push_str(&format!("__asm__(\".section .rodata.tv_js_blob,\\\"a\\\"\\n.globl tv_js_blob\\n.p2align 3\\ntv_js_blob:\\n.incbin \\\"{p}\\\"\\n.byte 0\\n.text\\n\");\n"));
        }
    }
    // Linux: BoringSSL's classes with virtual destructors name C++'s sized operator delete, which
    // nothing calls (it allocates and frees with functions of its own). Defined here as libstdc++
    // defines it, a program using TLS doesn't load libstdc++ (~1 MB of its memory) for it. (The
    // JavaScript engine is C++ through and through: a program with npm packages loads it anyway.)
    if uses_tls && npm.is_empty() && !cfg!(target_vendor = "apple") && !cfg!(windows) {
        c_src.push_str("void _ZdlPvm(void *p, unsigned long n) { (void)n; free(p); }\n");
    }
    let t2 = Instant::now();
    if let Some(p) = &opts.emit_c {
        std::fs::write(p, codegen::standalone(&c_src)).map_err(|e| BuildError::Message(format!("can't write {}: {e}", p.display())))?;
    }
    let (cc, sysroot) = c_compiler();
    // TOV_CFLAGS adds C compiler flags (e.g. "-fsanitize=address,undefined -g" to audit memory safety).
    let extra = std::env::var("TOV_CFLAGS").unwrap_or_default();
    // No FMA contraction: `a * b + c` rounds twice, exactly as in JavaScript (and it's faster on
    // latency-bound loops, where a fused multiply-add lengthens the dependency chain).
    let mut flags: Vec<&str> = vec![opts.opt.as_str(), "-std=gnu11", "-w", "-ffp-contract=off"];
    // Tov code is bounds-checked, so C's stack canaries and fortified memcpy only add checks
    // (and imports: each costs about 50 bytes of binary).
    flags.extend(["-fno-stack-protector", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0"]);
    if opts.unchecked {
        flags.push("-DTOV_UNCHECKED=1");
    }
    if opts.symbols {
        flags.push("-g");
    }
    // The program links with a big main stack (below), so main needn't check for one.
    if cfg!(target_vendor = "apple") && !extra.contains("-fsanitize") {
        flags.push("-DTVG_MAIN_STACK=1");
    }
    if let Some(sdk) = &sysroot {
        flags.push("-isysroot");
        flags.push(sdk);
    }
    flags.extend(extra.split_whitespace());
    let flag_text = flags.join(" ");
    let rt_key = hash_hex(&[codegen::RUNTIME_H.as_bytes(), codegen::RUNTIME_C.as_bytes(), codegen::RUNTIME_BIG.as_bytes(), codegen::RUNTIME_DEC.as_bytes(), codegen::RUNTIME_NET.as_bytes(), codegen::RUNTIME_WS.as_bytes(), cc.as_bytes(), flag_text.as_bytes(), env!("CARGO_PKG_VERSION").as_bytes()]);
    // A program that fetches links TLS (its code calls tv_tls_install).
    let tls = uses_tls.then(|| TlsArchive::new(&cc, sysroot.as_deref(), &extra));
    // A program importing npm packages links the JavaScript bridge (compiled once per compiler).
    let jsc = if npm.is_empty() { None } else { Some(jsc_flags()?) };
    let jsc_own = jsc.as_ref().is_some_and(|j| j.own);
    let js_key = match &jsc {
        None => String::new(),
        Some(j) => hash_hex(&[JS_C.as_bytes(), NODE_C.as_bytes(), NAPI_C.as_bytes(), codegen::JS_H.as_bytes(), codegen::RUNTIME_H.as_bytes(), cc.as_bytes(), flag_text.as_bytes(), JIT_ENTITLEMENTS.as_bytes(), j.cflags.join(" ").as_bytes(), j.libs.join(" ").as_bytes()]),
    };
    // Link flags (see below) are part of what a cached binary was built with.
    let tls_key = tls.as_ref().map(|t| t.key.clone()).unwrap_or_default();
    let lld = use_lld(&cc);
    let key = hash_hex(&[c_src.as_bytes(), rt_key.as_bytes(), LINK_FLAGS.as_bytes(), if lld { b"lld" } else { b"ld" }, tls_key.as_bytes(), js_key.as_bytes()]);
    let dir = cache_dir();
    let bin_dir = dir.join("bin");
    let binary = bin_dir.join(&key);
    if cached(&binary) {
        let binary = match &appended_blob {
            Some(blob) => with_blob(&binary, blob)?,
            None => binary,
        };
        return Ok(Built { binary, tls: uses_tls, jsc_own, cached: true, sources, timings: (t1 - t0, t2 - t1, Duration::ZERO) });
    }
    let c_dir = dir.join("c");
    for d in [&bin_dir, &c_dir] {
        std::fs::create_dir_all(d).map_err(|e| BuildError::Message(format!("can't create {}: {e}", d.display())))?;
    }
    // The runtime is compiled once per compiler and flags; programs link against the object. It,
    // the JavaScript bridge and the TLS archives (each cached apart) compile on threads of their
    // own while the program's own code compiles here.
    let rt_obj = c_dir.join(format!("rt-{rt_key}.o"));
    let message = |e: BuildError| match e {
        BuildError::Message(m) => m,
        BuildError::Diagnostics(..) => "internal error: diagnostics from compiling C".into(),
    };
    let rt_job = || -> Result<(), String> {
        if cached(&rt_obj) {
            return Ok(());
        }
        let rt_c = c_dir.join(format!("rt-{rt_key}.c"));
        std::fs::write(&rt_c, codegen::runtime_source()).map_err(|e| format!("can't write {}: {e}", rt_c.display()))?;
        let tmp = c_dir.join(format!("rt-{rt_key}.tmp{}.o", std::process::id()));
        let mut cmd = Command::new(&cc);
        cmd.args(&flags).args(["-ffunction-sections", "-fdata-sections", "-c", "-o"]).arg(&tmp).arg(&rt_c);
        run_cc(cmd, &cc, &rt_c).map_err(message)?;
        std::fs::rename(&tmp, &rt_obj).map_err(|e| format!("can't move the runtime into the cache: {e}"))
    };
    let js_flags: Option<Vec<String>> = jsc.as_ref().map(|j| flags.iter().map(|f| f.to_string()).chain(j.cflags.iter().cloned()).collect());
    let js_job = || -> Result<Option<Vec<PathBuf>>, String> {
        match &js_flags {
            Some(f) => js_objects(&cc, f, &c_dir, &js_key).map(Some).map_err(message),
            None => Ok(None),
        }
    };
    let tls_job = || -> Result<Option<[PathBuf; 2]>, String> { tls.as_ref().map(|t| t.build(&cc, &c_dir)).transpose().map_err(message) };
    let parallel = std::thread::scope(|s| {
        let rt_thread = s.spawn(rt_job);
        let js_thread = s.spawn(js_job);
        let tls_thread = s.spawn(tls_job);
        // Every program starts with the same runtime interface and prelude: precompile it once
        // (per compiler and flags) and compile only the program's own code against it.
        let prefix = codegen::fixed_prefix();
        let pch = match c_src.strip_prefix(prefix.as_str()) {
            Some(_) => precompiled_header(&cc, &flags, &c_dir, &prefix, &rt_key),
            None => None,
        };
        let c_path = c_dir.join(format!("{key}.c"));
        let c_text: &str = match &pch {
            Some(_) => &c_src[prefix.len()..],
            None => &c_src,
        };
        let written = std::fs::write(&c_path, c_text).map_err(|e| format!("can't write {}: {e}", c_path.display()));
        let rt = rt_thread.join().unwrap_or_else(|_| Err("internal error: compiling the runtime panicked".into()));
        let js = js_thread.join().unwrap_or_else(|_| Err("internal error: compiling the JavaScript bridge panicked".into()));
        let tls = tls_thread.join().unwrap_or_else(|_| Err("internal error: compiling the TLS library panicked".into()));
        (pch, c_path, written, rt, js, tls)
    });
    let (pch, c_path, written, rt_done, js_objs, tls_archives) = parallel;
    written.map_err(BuildError::Message)?;
    rt_done.map_err(BuildError::Message)?;
    let js_objs = js_objs.map_err(BuildError::Message)?;
    let tls_archives = tls_archives.map_err(BuildError::Message)?;
    let mut flags = flags.clone();
    let include;
    if let Some(h) = &pch {
        include = h.to_string_lossy().into_owned();
        flags.push("-include");
        flags.push(&include);
    }
    // Compile to a temporary name, then rename: concurrent builds never see a partial binary.
    let tmp = bin_dir.join(format!("{key}.tmp{}", std::process::id()));
    let ld = linker(&cc);
    let mut cmd = Command::new(&ld);
    let obj = c_dir.join(format!("{key}.tmp{}.o", std::process::id()));
    if ld == cc {
        cmd.args(&flags).arg("-o").arg(&tmp).arg(&c_path);
    } else {
        let mut compile = Command::new(&cc);
        compile.args(&flags).arg("-c").arg("-o").arg(&obj).arg(&c_path);
        run_cc(compile, &cc, &c_path)?;
        cmd.args(extra.split_whitespace()).arg("-o").arg(&tmp).arg(&obj);
    }
    cmd.arg(&rt_obj);
    if lld {
        cmd.arg("-fuse-ld=lld");
    }
    if !cfg!(target_vendor = "apple") && !cfg!(windows) {
        // (a library only if the program calls into it: libm, libstdc++ (see _ZdlPvm above) and
        // libpthread before glibc 2.34 often aren't, and loading libm alone makes ~300 KB of a
        // program's memory)
        cmd.arg("-Wl,--as-needed");
    }
    if let (Some(jsc), Some(objs)) = (&jsc, &js_objs) {
        cmd.arg(&objs[0]).arg(&objs[1]);
        if native {
            // native addons find Node-API in the program (they're linked against no library)
            cmd.arg(&objs[2]);
            if cfg!(target_vendor = "apple") {
                cmd.args(["-Wl,-exported_symbol,_napi_*", "-Wl,-exported_symbol,_node_api_*"]);
            } else {
                cmd.arg("-rdynamic");
            }
        }
        cmd.args(&jsc.libs);
    }
    if let Some(archives) = &tls_archives {
        cmd.args(archives);
        // BoringSSL is C++ (no exceptions or RTTI): a few libc++/libstdc++ helpers
        cmd.arg(if cfg!(target_vendor = "apple") { "-lc++" } else { "-lstdc++" });
    }
    cmd.arg("-lm");
    if !cfg!(target_vendor = "apple") && !cfg!(windows) {
        cmd.arg("-lpthread");
    }
    // Drop the runtime functions the program doesn't use. On macOS the program runs on the main
    // thread with a 512 MiB stack (the most arm64 allows) instead of a thread of its own.
    if cfg!(target_vendor = "apple") {
        // Only `main` is exported: the runtime's functions needn't be in the export table.
        cmd.arg("-Wl,-dead_strip").arg("-Wl,-exported_symbol,_main");
        if !opts.symbols {
            cmd.arg("-Wl,-x"); // local symbols (function names) only serve debuggers and profilers
        }
        // (Sanitizers can't lay out their shadow memory around a large main stack.)
        if !extra.contains("-fsanitize") {
            cmd.arg(LINK_STACK);
        }
    } else if !cfg!(windows) {
        cmd.arg("-Wl,--gc-sections");
        if !opts.symbols {
            cmd.arg("-Wl,-x");
        }
    }
    let linked = run_cc(cmd, &ld, &c_path);
    let _ = std::fs::remove_file(&obj);
    linked?;
    if jsc.as_ref().is_some_and(|j| !j.own) && cfg!(target_vendor = "apple") {
        sign_for_jit(&tmp, &c_dir)?;
    }
    std::fs::rename(&tmp, &binary).map_err(|e| BuildError::Message(format!("can't move the binary into the cache: {e}")))?;
    let binary = match &appended_blob {
        Some(blob) => with_blob(&binary, blob)?,
        None => binary,
    };
    let t3 = Instant::now();
    Ok(Built { binary, tls: uses_tls, jsc_own, cached: false, sources, timings: (t1 - t0, t2 - t1, t3 - t2) })
}

/// The compiler flags without the precompiled header's `-include` (for other C files).
/// runtime/js.c and runtime/node.c compiled once per compiler and flags: (js.o, node.o).
fn js_objects(cc: &str, flags: &[String], c_dir: &Path, key: &str) -> Result<Vec<PathBuf>, BuildError> {
    let objs = vec![c_dir.join(format!("js-{key}.o")), c_dir.join(format!("node-{key}.o")), c_dir.join(format!("napi-{key}.o"))];
    if objs.iter().all(|o| cached(o)) {
        return Ok(objs);
    }
    let fail = |what: String| BuildError::Message(format!("can't build the JavaScript bridge: {what}"));
    let dir = c_dir.join(format!("js-{key}.tmp{}", std::process::id()));
    std::fs::create_dir_all(&dir).map_err(|e| fail(e.to_string()))?;
    for (name, text) in [("tov.h", codegen::RUNTIME_H), ("js.h", codegen::JS_H), ("js.c", JS_C), ("node.c", NODE_C), ("napi.c", NAPI_C)] {
        std::fs::write(dir.join(name), text).map_err(|e| fail(e.to_string()))?;
    }
    // (the three at once)
    let results: Vec<Result<(), BuildError>> = std::thread::scope(|s| {
        let jobs: Vec<_> = ["js.c", "node.c", "napi.c"]
            .iter()
            .zip(&objs)
            .map(|(src, obj)| {
                let dir = &dir;
                s.spawn(move || {
                    let tmp = obj.with_extension(format!("tmp{}.o", std::process::id()));
                    let mut cmd = Command::new(cc);
                    cmd.args(flags).args(["-ffunction-sections", "-fdata-sections", "-c", "-o"]).arg(&tmp).arg(dir.join(src));
                    run_cc(cmd, cc, &dir.join(src))?;
                    std::fs::rename(&tmp, obj).map_err(|e| BuildError::Message(format!("can't build the JavaScript bridge: {e}")))
                })
            })
            .collect();
        jobs.into_iter().map(|j| j.join().unwrap_or_else(|_| Err(BuildError::Message("internal error: compiling the JavaScript bridge panicked".into())))).collect()
    });
    for r in results {
        r?;
    }
    for name in ["tov.h", "js.h", "js.c", "node.c", "napi.c"] {
        let _ = std::fs::remove_file(dir.join(name));
    }
    let _ = std::fs::remove_dir(&dir);
    Ok(objs)
}

/// How programs importing npm packages compile and link against JavaScriptCore: Tov's own build
/// of it (scripts/jsc/build.sh: a static library in ~/.cache/tov/jsc/<os>-<arch>, or
/// TOV_JSC_DIR) when there's one, unless TOV_JSC=system; otherwise on macOS the system's
/// framework, elsewhere WebKitGTK's library (pkg-config's javascriptcoregtk-4.1, or 6.0).
struct JscFlags {
    cflags: Vec<String>,
    libs: Vec<String>,
    /// Tov's own engine: on macOS it compiles JavaScript to machine code without the JIT
    /// entitlement (the linker's ad-hoc signature is enough), so the binary isn't re-signed
    /// (codesign hashes the whole binary again: 155 ms of a 34 MB one's build)
    own: bool,
}

fn own_jsc_dir() -> Option<PathBuf> {
    if std::env::var("TOV_JSC").is_ok_and(|v| v == "system") {
        return None;
    }
    let dir = match std::env::var_os("TOV_JSC_DIR") {
        Some(d) => PathBuf::from(d),
        None => {
            let os = if cfg!(target_vendor = "apple") { "darwin" } else { std::env::consts::OS };
            let arch = match std::env::consts::ARCH {
                "aarch64" => "arm64",
                a => a,
            };
            cache_dir().join("jsc").join(format!("{os}-{arch}"))
        }
    };
    dir.join("lib/libtovjsc.a").is_file().then_some(dir)
}

fn jsc_flags() -> Result<JscFlags, BuildError> {
    if let Some(dir) = own_jsc_dir() {
        // (the build's version: a rebuilt engine relinks programs)
        let version = std::fs::read_to_string(dir.join("VERSION")).unwrap_or_default();
        // (and the library as built: its size and time, so programs relink when it's rebuilt)
        let lib = std::fs::metadata(dir.join("lib/libtovjsc.a")).ok();
        let stamp = lib.map(|m| format!("{}-{:?}", m.len(), m.modified().ok())).unwrap_or_default();
        // (the cache's format: its files go in a directory of its own, TV_JSC_CACHE_FORMAT)
        let format = version.split_whitespace().last().unwrap_or("0").to_string();
        let cflags = vec![format!("-I{}", dir.join("include").display()), "-DTV_JSC_OWN=1".to_string(), format!("-DTV_JSC_CACHE_FORMAT=\"{format}\""), format!("-DTV_JSC_BUILD=\"{} {}\"", version.trim(), stamp)];
        let mut libs = vec![dir.join("lib/libtovjsc.a").to_string_lossy().into_owned()];
        if cfg!(target_vendor = "apple") {
            libs.extend(["-licucore", "-lc++", "-framework", "CoreFoundation", "-framework", "Foundation"].map(String::from));
        } else {
            libs.extend(["-licui18n", "-licuuc", "-licudata", "-lstdc++", "-ldl", "-latomic"].map(String::from));
        }
        return Ok(JscFlags { cflags, libs, own: true });
    }
    if cfg!(target_vendor = "apple") {
        return Ok(JscFlags { cflags: Vec::new(), libs: ["-framework", "JavaScriptCore", "-framework", "CoreFoundation", "-lobjc"].map(String::from).to_vec(), own: false });
    }
    let pkg = |package: &str, what: &str| -> Option<Vec<String>> {
        let out = Command::new("pkg-config").arg(what).arg(package).output().ok()?;
        out.status.success().then(|| String::from_utf8_lossy(&out.stdout).split_whitespace().map(String::from).collect())
    };
    for package in ["javascriptcoregtk-4.1", "javascriptcoregtk-6.0"] {
        if let (Some(cflags), Some(mut libs)) = (pkg(package, "--cflags"), pkg(package, "--libs")) {
            libs.push("-ldl".into());
            return Ok(JscFlags { cflags, libs, own: false });
        }
    }
    Err(BuildError::Message("npm packages need JavaScriptCore: install WebKitGTK's (on Debian and Ubuntu, libjavascriptcoregtk-4.1-dev; pkg-config finds it)".into()))
}

/// The program: its binary as linked (`base`, which holds no JavaScript) with the blob appended
/// (runtime/js.c maps it from there): `base` + "-" + the blob's name, made once per blob (on
/// APFS the copy shares the base's blocks). The binary is page-aligned, then the blob, a NUL,
/// the blob's offset and length (u64s, little-endian) and "TOV_BLOB". Older programs made from
/// the same base are removed (each would otherwise keep a copy of the engine).
fn with_blob(base: &Path, blob: &Path) -> Result<PathBuf, BuildError> {
    let fail = |e: std::io::Error| BuildError::Message(format!("can't append the program's JavaScript: {e}"));
    let name = base.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let blob_name = blob.file_stem().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let out = base.with_file_name(format!("{name}-{blob_name}"));
    if !cached(&out) {
        use std::io::Write;
        let tmp = out.with_file_name(format!("{name}-{blob_name}.tmp{}", std::process::id()));
        std::fs::copy(base, &tmp).map_err(fail)?;
        let js = std::fs::read(blob).map_err(fail)?;
        let mut f = std::fs::OpenOptions::new().append(true).open(&tmp).map_err(fail)?;
        let len = f.metadata().map_err(fail)?.len();
        const PAGE: u64 = 16384;
        let at = len.div_ceil(PAGE) * PAGE;
        f.write_all(&vec![0u8; (at - len) as usize]).map_err(fail)?;
        f.write_all(&js).map_err(fail)?;
        f.write_all(&[0]).map_err(fail)?;
        f.write_all(&at.to_le_bytes()).map_err(fail)?;
        f.write_all(&(js.len() as u64 + 1).to_le_bytes()).map_err(fail)?;
        f.write_all(b"TOV_BLOB").map_err(fail)?;
        drop(f);
        std::fs::rename(&tmp, &out).map_err(fail)?;
        // (the same program's earlier ones)
        if let Some(dir) = base.parent()
            && let Ok(entries) = std::fs::read_dir(dir)
        {
            let prefix = format!("{name}-");
            for e in entries.flatten() {
                let n = e.file_name().to_string_lossy().into_owned();
                if n.starts_with(&prefix) && !n.contains(".tmp") && e.path() != out {
                    let _ = std::fs::remove_file(e.path());
                }
            }
        }
    }
    Ok(out)
}

/// Ad-hoc signs a binary with the JIT entitlement (see JIT_ENTITLEMENTS).
fn sign_for_jit(binary: &Path, c_dir: &Path) -> Result<(), BuildError> {
    let plist = c_dir.join(format!("jit-{}.plist", hash_hex(&[JIT_ENTITLEMENTS.as_bytes()])));
    if !plist.is_file() {
        std::fs::write(&plist, JIT_ENTITLEMENTS).map_err(|e| BuildError::Message(format!("can't write {}: {e}", plist.display())))?;
    }
    let out = Command::new("/usr/bin/codesign").args(["-s", "-", "-f", "--entitlements"]).arg(&plist).arg(binary).output().map_err(|e| BuildError::Message(format!("can't run codesign: {e}")))?;
    if !out.status.success() {
        return Err(BuildError::Message(format!("codesign failed: {}", String::from_utf8_lossy(&out.stderr).trim())));
    }
    Ok(())
}

/// macOS: the main thread's stack size (the most arm64 allows).
const LINK_STACK: &str = "-Wl,-stack_size,0x20000000";
/// Everything that changes how programs are linked, for the binary cache key.
const LINK_FLAGS: &str = "dead-strip; exported: _main; -x unless -g; sanitizers link without -stack_size; -Wl,-stack_size,0x20000000; Linux: --as-needed after the runtime, lld when installed";

/// The precompiled fixed prefix (`pre-<key>.h`, with its `.pch`/`.gch` beside it) to pass as
/// `-include`, built on first use; `None` if the compiler can't precompile it (then programs
/// are compiled whole, which is only slower).
fn precompiled_header(cc: &str, flags: &[&str], c_dir: &Path, prefix: &str, rt_key: &str) -> Option<PathBuf> {
    let clang = cfg!(target_vendor = "apple") || Path::new(cc).file_name().is_some_and(|n| n.to_string_lossy().contains("clang"));
    let key = hash_hex(&[prefix.as_bytes(), rt_key.as_bytes()]);
    let header = c_dir.join(format!("pre-{key}.h"));
    let pch = c_dir.join(format!("pre-{key}.h.{}", if clang { "pch" } else { "gch" }));
    // (not the header's time: clang checks the precompiled header against it)
    if cached(&pch) && header.is_file() {
        return Some(header);
    }
    let pid = std::process::id();
    let tmp_h = c_dir.join(format!("pre-{key}.tmp{pid}.h"));
    let tmp_p = c_dir.join(format!("pre-{key}.tmp{pid}.pch"));
    // The header goes to its final path first (the .pch records it; its content is fixed by the
    // key, so a concurrent build writes the same bytes), then the .pch appears atomically.
    if !header.is_file() {
        std::fs::write(&tmp_h, prefix).ok()?;
        std::fs::rename(&tmp_h, &header).ok()?;
    }
    let ok = Command::new(cc).args(flags).args(["-x", "c-header", "-o"]).arg(&tmp_p).arg(&header).output().is_ok_and(|o| o.status.success());
    if !ok {
        let _ = std::fs::remove_file(&tmp_p);
        return None;
    }
    std::fs::rename(&tmp_p, &pch).ok()?;
    Some(header)
}

fn run_cc(mut cmd: Command, cc: &str, c_path: &Path) -> Result<(), BuildError> {
    let out = cmd.output().map_err(|e| BuildError::Message(format!("can't run the C compiler `{cc}`: {e} (set CC to choose one)")))?;
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr);
        let first: String = stderr.lines().take(30).collect::<Vec<_>>().join("\n");
        return Err(BuildError::Message(format!(
            "internal error: the generated C failed to compile (this is a bug in tov)\n  C file: {}\n{first}",
            c_path.display()
        )));
    }
    Ok(())
}

/// The TLS archives: the vendored libraries (BoringSSL, brotli, libdeflate, zlib, zstd), and the
/// runtime's files that use them (runtime/tls.c, codecs.c, crypto.c, compress.c). Their flags are
/// fixed (tls_flags: -O3 whatever the program's -O, and no --unchecked), so every build shares
/// them; only fetch() programs link them. The vendored archive is keyed by its sources and flags
/// alone, and the compiler carries it precompiled (see build.rs), so it's compiled here only for
/// sanitizer builds or when the compiler was built without it; the runtime's, a few files, is
/// compiled once per runtime, compiler and SDK.
struct TlsArchive {
    key: String,
    vendor_key: String,
    /// flags for C, assembly and C++ alike
    flags: Vec<String>,
}

impl TlsArchive {
    fn new(cc: &str, sysroot: Option<&str>, extra: &str) -> TlsArchive {
        let sanitizers: Vec<&str> = if extra.contains("-fsanitize") { extra.split_whitespace().collect() } else { Vec::new() };
        let mut flags: Vec<String> = tls_flags::FLAGS.iter().map(|f| f.to_string()).collect();
        if let Some(sdk) = sysroot {
            flags.push("-isysroot".into());
            flags.push(sdk.into());
        }
        flags.extend(sanitizers.iter().map(|f| f.to_string()));
        let vendor_key = tls_flags::vendor_key(tls_files::VENDOR_KEY, &sanitizers);
        let key = hash_hex(&[tls_files::RUNTIME_KEY.as_bytes(), vendor_key.as_bytes(), codegen::RUNTIME_H.as_bytes(), cc.as_bytes(), flags.join(" ").as_bytes()]);
        TlsArchive { key, vendor_key, flags }
    }

    /// The C++ compiler that goes with `cc` (CXX overrides it).
    fn cxx(cc: &str) -> String {
        if let Ok(cxx) = std::env::var("CXX") {
            return cxx;
        }
        let path = Path::new(cc);
        let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let sibling = |n: String| path.with_file_name(n).to_string_lossy().into_owned();
        if let Some(rest) = name.strip_prefix("clang") {
            return sibling(format!("clang++{rest}"));
        }
        if let Some(rest) = name.strip_prefix("gcc") {
            return sibling(format!("g++{rest}"));
        }
        if name == "cc" {
            return sibling("c++".into());
        }
        "c++".into()
    }

    /// The archives to link (the runtime's, then the vendored libraries it uses), compiling them
    /// first if they aren't cached.
    fn build(&self, cc: &str, c_dir: &Path) -> Result<[PathBuf; 2], BuildError> {
        let fail = |what: String| BuildError::Message(format!("can't build the TLS library: {what}"));
        let vendor = c_dir.join(format!("vendor-{}.a", self.vendor_key));
        if !cached(&vendor) {
            if self.vendor_key == tls_files::VENDOR_ARCHIVE_KEY && !tls_files::VENDOR_ARCHIVE.is_empty() {
                let tmp = c_dir.join(format!("vendor-{}.tmp{}.a", self.vendor_key, std::process::id()));
                std::fs::write(&tmp, tls_files::VENDOR_ARCHIVE).map_err(|e| fail(format!("{e}")))?;
                std::fs::rename(&tmp, &vendor).map_err(|e| fail(format!("can't move it into the cache: {e}")))?;
            } else {
                eprintln!("tov: compiling the TLS library (BoringSSL, brotli, libdeflate, zlib, zstd); this happens once");
                self.compile(cc, c_dir, &vendor, |p| p.starts_with("vendor/"))?;
            }
        }
        let runtime = c_dir.join(format!("tls-{}.a", self.key));
        if !cached(&runtime) {
            self.compile(cc, c_dir, &runtime, |p| p.starts_with("runtime/"))?;
        }
        Ok([runtime, vendor])
    }

    /// Compiles the TLS files `pick` chooses into `archive`, in parallel.
    fn compile(&self, cc: &str, c_dir: &Path, archive: &Path, pick: impl Fn(&str) -> bool) -> Result<(), BuildError> {
        let fail = |what: String| BuildError::Message(format!("can't build the TLS library: {what}"));
        let name = archive.file_stem().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let dir = c_dir.join(format!("{name}.tmp{}", std::process::id()));
        for (rel, bytes) in tls_files::TLS_FILES.iter().copied().chain([("runtime/tov.h", codegen::RUNTIME_H.as_bytes())]) {
            let path = dir.join(rel);
            std::fs::create_dir_all(path.parent().unwrap()).map_err(|e| fail(format!("{e}")))?;
            std::fs::write(&path, bytes).map_err(|e| fail(format!("{e}")))?;
        }
        let sources: Vec<(&str, &[&str])> = tls_files::TLS_FILES.iter().filter(|f| pick(f.0)).filter_map(|f| tls_flags::lang(f.0).map(|l| (f.0, l))).collect();
        let cxx = Self::cxx(cc);
        let include: Vec<String> = tls_flags::INCLUDES.iter().map(|p| format!("-I{}", dir.join(p).display())).collect();
        let threads = std::thread::available_parallelism().map_or(4, |n| n.get()).min(sources.len().max(1));
        let next = std::sync::atomic::AtomicUsize::new(0);
        let errors = std::sync::Mutex::new(Vec::new());
        std::thread::scope(|s| {
            for _ in 0..threads {
                s.spawn(|| loop {
                    let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    let Some((src, lang)) = sources.get(i) else { break };
                    let compiler: &str = if src.ends_with(".cc") { &cxx } else { cc };
                    let out = Command::new(compiler).args(&self.flags).args(*lang).args(&include).arg("-c").arg(dir.join(src)).arg("-o").arg(dir.join(format!("{i}.o"))).output();
                    match out {
                        Ok(o) if o.status.success() => {}
                        Ok(o) => errors.lock().unwrap().push(format!("{src}: {}", String::from_utf8_lossy(&o.stderr).lines().take(5).collect::<Vec<_>>().join("\n"))),
                        Err(e) => errors.lock().unwrap().push(format!("{src}: can't run `{compiler}`: {e}")),
                    }
                });
            }
        });
        if let Some(e) = errors.into_inner().unwrap().first() {
            let _ = std::fs::remove_dir_all(&dir);
            return Err(fail(e.clone()));
        }
        // the toolchain's own archiver (next to the compiler we picked), else the one on PATH
        let ar = Path::new(cc).parent().map(|d| d.join("ar")).filter(|p| p.is_file()).map_or("ar".to_string(), |p| p.to_string_lossy().into_owned());
        let tmp = c_dir.join(format!("{name}.tmp{}.a", std::process::id()));
        let objs: Vec<PathBuf> = (0..sources.len()).map(|i| dir.join(format!("{i}.o"))).collect();
        let out = Command::new(&ar).arg("rcs").arg(&tmp).args(&objs).output().map_err(|e| fail(format!("can't run `{ar}`: {e}")))?;
        let _ = std::fs::remove_dir_all(&dir);
        if !out.status.success() {
            let _ = std::fs::remove_file(&tmp);
            return Err(fail(format!("`{ar}` failed: {}", String::from_utf8_lossy(&out.stderr))));
        }
        std::fs::rename(&tmp, archive).map_err(|e| {
            let _ = std::fs::remove_file(&tmp);
            fail(format!("can't move it into the cache: {e}"))
        })?;
        Ok(())
    }
}

/// A 128-bit content hash (two independent 64-bit lanes), hex-encoded. For cache keys, not security.
fn hash_hex(parts: &[&[u8]]) -> String {
    tls_flags::hash_hex(parts)
}
