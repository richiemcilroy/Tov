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
/// The API Tov's JavaScriptCore adds (scripts/jsc/patches), as this compiler's bridge expects it.
const TOV_API_H: &str = include_str!("../../../scripts/jsc/patches/TovAPI.h");
const NODE_TTY_H: &str = include_str!("../../../runtime/node_tty.h");
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
    /// The program; with `blob`, the program as linked, which is the program with it appended
    pub binary: PathBuf,
    /// The program's JavaScript for `install` to append to `binary` (see with_blob), when it was
    /// built only to be installed (`Options::install`)
    pub blob: Option<PathBuf>,
    /// The Node-API addons it loads from its own directory, beside `binary` (see `install`).
    pub addons: Vec<PathBuf>,
    /// It links TLS (the program calls fetch()).
    pub tls: bool,
    /// It links Tov's own JavaScriptCore rather than the system engine.
    pub jsc_own: bool,
    pub cached: bool,
    /// The program's source files on disk (the entry and everything it imports).
    pub sources: Vec<PathBuf>,
    /// It bundles npm packages (whose files `sources` doesn't list).
    pub npm: bool,
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
    /// The program is only installed (`install`), not run from the cache: a program whose
    /// JavaScript goes after the binary is then put together where it's installed, not in the
    /// cache as well (`Built::blob`).
    pub install: bool,
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

/// Keeps the cache in bounds, once a day or, while builds pile up (a program with npm packages is
/// tens of MB), once a minute: what interrupted builds left goes, then what hasn't been used for
/// 30 days, then the least recently used while it's over 2 GiB (down to 1.5).
pub fn prune_cache() {
    let dir = cache_dir();
    let stamp = dir.join(".pruned");
    let now = SystemTime::now();
    let age = |t: SystemTime| now.duration_since(t).unwrap_or(Duration::ZERO);
    if let Ok(t) = std::fs::metadata(&stamp).and_then(|m| m.modified()) {
        let many = || std::fs::read_dir(dir.join("bin")).is_ok_and(|rd| rd.count() > 64);
        if age(t) < Duration::from_secs(60) || (age(t) < Duration::from_secs(86400) && !many()) {
            return;
        }
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
    if cfg!(target_vendor = "apple")
        && let Some((clang, sdk)) = apple_toolchain()
    {
        return (clang, Some(sdk));
    }
    (crate::rt_flags::other_compiler(), None)
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
    let tmp = file.with_extension(format!("tmp{}", tmp_tag()));
    if std::fs::write(&tmp, format!("{clang}\n{sdk}\n")).is_ok() {
        let _ = std::fs::rename(&tmp, &file);
    }
    Some((clang, sdk))
}

/// The C compiler (`$CC` or the one found), its flags and what's derived from them alone: the
/// runtime's key and the precompiled header every program's code is compiled against.
struct Toolchain {
    cc: String,
    sysroot: Option<String>,
    /// TOV_CFLAGS: C compiler flags added (e.g. "-fsanitize=address,undefined -g" to audit memory safety)
    extra: String,
    base: Vec<String>,
    rt_key: String,
    pch: Option<PathBuf>,
}

impl Toolchain {
    fn new(opt: &str, unchecked: bool, symbols: bool) -> Toolchain {
        let (cc, sysroot) = c_compiler();
        let extra = std::env::var("TOV_CFLAGS").unwrap_or_default();
        let base = crate::rt_flags::base_flags(opt, unchecked, symbols, extra.contains("-fsanitize"), sysroot.as_deref());
        let mut flags: Vec<&str> = base.iter().map(|f| f.as_str()).collect();
        flags.extend(extra.split_whitespace());
        let flag_text = flags.join(" ");
        let rt_key = hash_hex(&[codegen::RUNTIME_H.as_bytes(), codegen::RUNTIME_C.as_bytes(), codegen::RUNTIME_BIG.as_bytes(), codegen::RUNTIME_DEC.as_bytes(), codegen::RUNTIME_NET.as_bytes(), codegen::RUNTIME_WS.as_bytes(), cc.as_bytes(), flag_text.as_bytes(), env!("CARGO_PKG_VERSION").as_bytes()]);
        let c_dir = cache_dir().join("c");
        let pch = std::fs::create_dir_all(&c_dir).ok().and_then(|_| precompiled_header(&cc, &flags, &c_dir, &codegen::fixed_prefix(), &rt_key));
        Toolchain { cc, sysroot, extra, base, rt_key, pch }
    }
}

/// A thread's result, waited for when it's taken or dropped (a build that stops early doesn't
/// leave the compiler writing into the cache as the program exits).
struct Background<T>(Option<std::thread::JoinHandle<T>>);

impl<T> Background<T> {
    fn join(mut self) -> T {
        self.0.take().and_then(|h| h.join().ok()).expect("internal error: finding the C compiler panicked")
    }
}

impl<T> Drop for Background<T> {
    fn drop(&mut self) {
        if let Some(h) = self.0.take() {
            let _ = h.join();
        }
    }
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

/// Linux without lld: whether the system's ld can lay out functions in an order of ours
/// (`--section-ordering-file`, GNU ld 2.43 and later).
fn gnu_ld_orders() -> bool {
    if cfg!(target_vendor = "apple") || cfg!(windows) {
        return false;
    }
    Command::new("ld").arg("--help").output().is_ok_and(|o| String::from_utf8_lossy(&o.stdout).contains("--section-ordering-file"))
}

/// The engine's function order (a symbol per line, as lld takes it) as GNU ld's section ordering
/// script: each function is its own section (`-ffunction-sections`), `.text.<symbol>`. Written
/// once beside the cache's other files, by the order's hash.
fn gnu_order_script(order: &Path, hash: &str) -> Option<PathBuf> {
    let path = cache_dir().join("c").join(format!("order-{hash}.ld"));
    if cached(&path) {
        return Some(path);
    }
    let text = std::fs::read_to_string(order).ok()?;
    let mut script = String::from(".text : {\n");
    for sym in text.lines().map(str::trim).filter(|l| !l.is_empty()) {
        script.push_str(&format!("  *(.text.{sym})\n"));
    }
    script.push_str("}\n");
    write_new(&path, |t| std::fs::write(t, &script)).ok()?;
    Some(path)
}

pub fn build(paths: &[PathBuf], base: &Path, opts: &Options) -> Result<Built, BuildError> {
    prune_cache();
    let t0 = Instant::now();
    if !cfg!(target_vendor = "apple") && !cfg!(windows) && own_jsc_dir().is_some() {
        std::thread::spawn(static_icu);
    }
    let loaded = driver::load_with(paths, base, opts.mode == crate::codegen::Mode::Test).map_err(BuildError::Message)?;
    let driver::Loaded { sm, mut interner, mut modules, diags } = loaded;
    let sources = source_paths(&sm);
    if !diags.is_empty() {
        return Err(BuildError::Diagnostics(sm, diags));
    }
    // npm packages resolve from the entry file's directory
    let npm_root = modules.iter().find(|m| m.entry).and_then(|m| m.path.parent()).map(Path::to_path_buf).unwrap_or_else(|| base.to_path_buf());
    let mut expect_rounds = 0;
    let (diags, mut checker) = loop {
        let first = check::check_for_build(&modules, &mut interner, &sm);
        let (diags, checker) = if first.1.rewrites.is_empty() {
            first
        } else {
            // TypeScript that changes parameters in place, or returns promises from async functions:
            // rewritten (`inout`, `await`) and checked again
            let rw = first.1.rewrites.clone();
            drop(first);
            driver::rewrite_rounds(&mut modules, &mut interner, &sm, rw);
            check::check_for_build(&modules, &mut interner, &sm)
        };
        // (a test's `// @ts-expect-error` statement: one that doesn't check throws a TypeError,
        // as the input checks it gets past would)
        if opts.mode == crate::codegen::Mode::Test && !diags.is_empty() && expect_rounds < 3 {
            let targets = expected_errors(&modules, &sm, &diags);
            if !targets.is_empty() {
                drop(checker);
                throw_instead(&mut modules, &mut interner, &targets);
                expect_rounds += 1;
                continue;
            }
        }
        // (elsewhere, one that only clears a slot, `// @ts-expect-error` then `xs[i] = undefined`
        // so what it held can be freed: a store the program never reads, left out)
        if opts.mode != crate::codegen::Mode::Test && !diags.is_empty() && expect_rounds < 3 {
            let targets: Vec<(usize, crate::ast::StmtId)> = expected_errors(&modules, &sm, &diags).into_iter().filter(|&(mi, st)| clears_a_slot(&modules[mi].ast, st)).collect();
            if !targets.is_empty() {
                drop(checker);
                for &(mi, st) in &targets {
                    modules[mi].ast.stmts[st as usize].kind = crate::ast::StmtKind::Block(Vec::new());
                }
                expect_rounds += 1;
                continue;
            }
        }
        break (diags, checker);
    };
    if !diags.is_empty() {
        drop(checker);
        return Err(BuildError::Diagnostics(sm, diags));
    }
    let t1 = Instant::now();
    // (the compiler, its flags and the precompiled header, found and built on a thread while the
    // program is generated and its npm packages bundled: none of it depends on the program)
    let toolchain = Background(Some(std::thread::spawn({
        let (opt, unchecked, symbols) = (opts.opt.clone(), opts.unchecked, opts.symbols);
        move || Toolchain::new(&opt, unchecked, symbols)
    })));
    let program = match codegen::generate(&mut checker, opts.mode) {
        Ok(p) => p,
        Err(d) => {
            drop(checker);
            return Err(BuildError::Diagnostics(sm, d));
        }
    };
    drop(checker);
    let codegen::Program { c: mut c_src, mut uses_tls, uses_regexp, npm } = program;
    let mut native = false;
    let mut addons: Vec<PathBuf> = Vec::new();
    // npm packages with Tov's own engine: the JavaScript goes after the binary, which therefore
    // doesn't depend on it, so the packages are bundled on a thread while the program compiles
    // and links. The binary has what any bundle could ask of it: the TLS library (Node.js's
    // crypto and fetch) and zlib installed before main, and Node-API for native addons.
    let bundling = (!npm.is_empty() && own_jsc_dir().is_some()).then(|| {
        let (root, npm) = (npm_root.clone(), npm.clone());
        std::thread::spawn(move || crate::npm::bundle(&root, &npm))
    });
    if bundling.is_some() {
        uses_tls = true;
        native = true;
        c_src.push_str("__attribute__((constructor)) static void tvg_npm_tls(void) { tv_tls_install(); tv_zlib_install(); }\n");
        c_src.push_str("const unsigned char tv_js_blob[8] = \"TOV_APP\";\n");
    } else if !npm.is_empty() {
        // npm packages: bundled into a blob the program embeds (see runtime/js.c)
        let b = crate::npm::bundle(&npm_root, &npm).map_err(|e| BuildError::Message(format!("can't bundle the npm packages: {e}")))?;
        // Node.js's crypto and zlib, and fetch(), run on the TLS library's BoringSSL, codecs and TLS
        // client (runtime/crypto.c, compress.c, tls.c): a bundle with them links the library, and
        // installs them before main
        for (module, install) in [("node:crypto", "tv_crypto_install"), ("node:zlib", "tv_zlib_install"), ("node:internal/tov/fetch", "tv_tls_install")] {
            if b.sources.iter().any(|(name, _)| name == module) {
                uses_tls = true;
                c_src.push_str(&format!("__attribute__((constructor)) static void tvg_npm_{}(void) {{ {install}(); }}\n", module[5..].replace('/', "_")));
            }
        }
        let path = write_blob(&b)?;
        native = b.native;
        addons = b.addons;
        let p = path.to_string_lossy().replace('\\', "\\\\").replace('"', "\\\"");
        if cfg!(target_vendor = "apple") {
            c_src.push_str(&format!("__asm__(\".section __TEXT,__const\\n.globl _tv_js_blob\\n.p2align 3\\n_tv_js_blob:\\n.incbin \\\"{p}\\\"\\n.byte 0\\n.text\\n\");\n"));
        } else {
            c_src.push_str(&format!("__asm__(\".section .rodata.tv_js_blob,\\\"a\\\"\\n.globl tv_js_blob\\n.p2align 3\\ntv_js_blob:\\n.incbin \\\"{p}\\\"\\n.byte 0\\n.text\\n\");\n"));
        }
    }
    // Linux: BoringSSL's classes with virtual destructors name C++'s sized operator delete, which
    // nothing calls (it allocates and frees with functions of its own), and a bounds-checked
    // accessor from the C++ headers names `std::__throw_out_of_range_fmt`. Defined here as
    // libstdc++ defines them (without exceptions, a throw is an abort), a program using TLS
    // doesn't load libstdc++ (~1.4 MB of its memory) for them: GNU ld keeps a library a
    // reference names even when the code with it is dropped. (The JavaScript engine is C++
    // through and through: a program with npm packages loads it anyway.)
    if uses_tls && npm.is_empty() && !cfg!(target_vendor = "apple") && !cfg!(windows) {
        c_src.push_str("void _ZdlPvm(void *p, unsigned long n) { (void)n; free(p); }\n");
        c_src.push_str("__attribute__((noreturn)) void _ZSt24__throw_out_of_range_fmtPKcz(const char *fmt, ...) { (void)fmt; abort(); }\n");
    }
    let t2 = Instant::now();
    if let Some(p) = &opts.emit_c {
        // (how to build it: the runtime's C is in it, and it needs only libm, unless it calls fetch()
        // or imports npm packages, whose libraries `tov build` links)
        let how = if uses_tls || !npm.is_empty() {
            "/* It also links TLS or the JavaScript engine: build it with `tov build`. */\n"
        } else {
            "/* To build it: cc -O3 -std=gnu11 -ffp-contract=off FILE.c -o program -lm */\n"
        };
        let c = codegen::standalone(&c_src);
        let first = c.find('\n').map_or(0, |i| i + 1);
        std::fs::write(p, format!("{}{how}{}", &c[..first], &c[first..])).map_err(|e| BuildError::Message(format!("can't write {}: {e}", p.display())))?;
    }
    let Toolchain { cc, sysroot, extra, base, rt_key, pch: prebuilt_pch } = toolchain.join();
    let mut flags: Vec<&str> = base.iter().map(|f| f.as_str()).collect();
    flags.extend(extra.split_whitespace());
    let flag_text = flags.join(" ");
    // A program that fetches links TLS (its code calls tv_tls_install).
    let tls = uses_tls.then(|| TlsArchive::new(&cc, sysroot.as_deref(), &extra));
    // A program importing npm packages links the JavaScript bridge (compiled once per compiler).
    let jsc = if npm.is_empty() { None } else { Some(jsc_flags()?) };
    let jsc_own = jsc.as_ref().is_some_and(|j| j.own);
    let js_key = match &jsc {
        None => String::new(),
        Some(j) => hash_hex(&[JS_C.as_bytes(), NODE_C.as_bytes(), NODE_TTY_H.as_bytes(), NAPI_C.as_bytes(), codegen::JS_H.as_bytes(), codegen::RUNTIME_H.as_bytes(), cc.as_bytes(), flag_text.as_bytes(), JIT_ENTITLEMENTS.as_bytes(), j.cflags.join(" ").as_bytes(), j.libs.join(" ").as_bytes()]),
    };
    // Link flags (see below) are part of what a cached binary was built with.
    let tls_key = tls.as_ref().map(|t| t.key.clone()).unwrap_or_default();
    let lld = use_lld(&cc);
    // (the addons' bytes too: a rebuilt addon gives a new key, so the one beside a cached binary is current)
    let mut addon_bytes = Vec::new();
    for a in &addons {
        addon_bytes.push(std::fs::read(a).map_err(|e| BuildError::Message(format!("can't read {}: {e}", a.display())))?);
    }
    let addon_key = hash_hex(&addon_bytes.iter().map(|b| b.as_slice()).collect::<Vec<_>>());
    let re_key = if uses_regexp {
        let mut parts: Vec<&[u8]> = codegen::REGEXP_FILES.iter().map(|f| f.1.as_bytes()).collect();
        parts.extend([codegen::RUNTIME_H.as_bytes(), cc.as_bytes(), flag_text.as_bytes()]);
        hash_hex(&parts)
    } else {
        String::new()
    };
    // (the engine's function order, so the code a program runs is on as few pages as can be: lld
    // follows the list as it is, GNU ld 2.43+ as a script made from it, ~7 MB less of an npm
    // service's memory)
    let order = jsc.as_ref().and_then(|j| j.order.as_ref()).filter(|_| lld);
    let gnu_order = jsc.as_ref().and_then(|j| j.order.as_ref()).filter(|_| !lld && gnu_ld_orders()).and_then(|(p, h)| gnu_order_script(p, h).map(|s| (s, h.clone())));
    let order_key = order.map(|o| o.1.as_str()).or(gnu_order.as_ref().map(|o| o.1.as_str())).unwrap_or_default();
    let key = hash_hex(&[c_src.as_bytes(), rt_key.as_bytes(), LINK_FLAGS.as_bytes(), if lld { b"lld" } else { b"ld" }, tls_key.as_bytes(), js_key.as_bytes(), addon_key.as_bytes(), re_key.as_bytes(), order_key.as_bytes()]);
    let dir = cache_dir();
    // A program with addons gets a directory of its own, the addons beside it.
    let bin_dir = if addons.is_empty() { dir.join("bin") } else { dir.join("bin").join(&key) };
    let binary = bin_dir.join(&key);
    let addons: Vec<PathBuf> = addons.iter().map(|a| bin_dir.join(a.file_name().unwrap_or_default())).collect();
    if cached(&binary) {
        if !addons.is_empty() {
            // (the cache is pruned by modification time)
            let _ = std::fs::File::open(&bin_dir).and_then(|f| f.set_modified(SystemTime::now()));
        }
        let (binary, addons, appended_blob) = match bundling {
            Some(h) => with_bundle(binary, h)?,
            None => (binary, addons, None),
        };
        let (binary, blob) = installable(binary, appended_blob, opts)?;
        return Ok(Built { binary, blob, addons, tls: uses_tls, jsc_own, cached: true, sources, npm: !npm.is_empty(), timings: (t1 - t0, t2 - t1, Duration::ZERO) });
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
        // (compiled when Tov was, for these flags and this compiler: written out as it is)
        if rt_key == tls_files::PREBUILT_RT_KEY && !tls_files::PREBUILT_RT.is_empty() {
            let tmp = c_dir.join(format!("rt-{rt_key}.tmp{}.o", tmp_tag()));
            std::fs::write(&tmp, tls_files::PREBUILT_RT).map_err(|e| format!("can't write the runtime into the cache: {e}"))?;
            return std::fs::rename(&tmp, &rt_obj).map_err(|e| format!("can't move the runtime into the cache: {e}"));
        }
        let rt_c = c_dir.join(format!("rt-{rt_key}.c"));
        std::fs::write(&rt_c, codegen::runtime_source()).map_err(|e| format!("can't write {}: {e}", rt_c.display()))?;
        let tmp = c_dir.join(format!("rt-{rt_key}.tmp{}.o", tmp_tag()));
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
    let re_job = || -> Result<Vec<PathBuf>, String> { if uses_regexp { regexp_objects(&cc, &flags, &c_dir, &re_key).map_err(message) } else { Ok(Vec::new()) } };
    // Every program starts with the same runtime interface and prelude: precompile it once
    // (per compiler and flags) and compile only the program's own code against it.
    let prefix = codegen::fixed_prefix();
    let pch = if c_src.starts_with(prefix.as_str()) { prebuilt_pch } else { None };
    let c_path = c_dir.join(format!("{key}.c"));
    let c_text: &str = match &pch {
        Some(_) => &c_src[prefix.len()..],
        None => &c_src,
    };
    let mut program_flags = flags.clone();
    let include;
    if let Some(h) = &pch {
        // (clang: the precompiled header itself, unchecked against the header it was made from,
        // which may be elsewhere: it's named by its contents' hash, and made by Tov's build)
        if is_clang(&cc) {
            include = format!("{}.pch", h.display());
            program_flags.extend(["-include-pch", include.as_str(), "-Xclang", "-fno-validate-pch"]);
        } else {
            include = h.to_string_lossy().into_owned();
            program_flags.extend(["-include", include.as_str()]);
        }
    }
    // (the program's own code to an object, then linked, when a library it links still has to
    // be compiled: the program compiles meanwhile rather than after; otherwise one command
    // compiles and links it)
    let ld = linker(&cc);
    let busy = !(cached(&rt_obj) || (rt_key == tls_files::PREBUILT_RT_KEY && !tls_files::PREBUILT_RT.is_empty()))
        || (js_flags.is_some() && !js_objects_ready(&c_dir, &js_key))
        || tls.as_ref().is_some_and(|t| !t.ready(&c_dir))
        || (uses_regexp && !regexp_units(&c_dir, &re_key).1.iter().all(|o| cached(o)));
    let separate = ld != cc || busy;
    let obj = c_dir.join(format!("{key}.tmp{}.o", tmp_tag()));
    let compile_job = || -> Result<(), BuildError> {
        std::fs::write(&c_path, c_text).map_err(|e| BuildError::Message(format!("can't write {}: {e}", c_path.display())))?;
        if !separate {
            return Ok(());
        }
        let mut compile = Command::new(&cc);
        compile.args(&program_flags).arg("-c").arg("-o").arg(&obj).arg(&c_path);
        run_cc(compile, &cc, &c_path)
    };
    let parallel = std::thread::scope(|s| {
        let rt_thread = s.spawn(rt_job);
        let js_thread = s.spawn(js_job);
        let tls_thread = s.spawn(tls_job);
        let re_thread = s.spawn(re_job);
        let compiled = compile_job();
        let rt = rt_thread.join().unwrap_or_else(|_| Err("internal error: compiling the runtime panicked".into()));
        let js = js_thread.join().unwrap_or_else(|_| Err("internal error: compiling the JavaScript bridge panicked".into()));
        let tls = tls_thread.join().unwrap_or_else(|_| Err("internal error: compiling the TLS library panicked".into()));
        let re = re_thread.join().unwrap_or_else(|_| Err("internal error: compiling the regular expression library panicked".into()));
        (compiled, rt, js, tls, re)
    });
    let (compiled, rt_done, js_objs, tls_archives, re_objs) = parallel;
    if compiled.is_err() {
        let _ = std::fs::remove_file(&obj);
    }
    compiled?;
    let re_objs = re_objs.map_err(BuildError::Message)?;
    rt_done.map_err(BuildError::Message)?;
    let js_objs = js_objs.map_err(BuildError::Message)?;
    let tls_archives = tls_archives.map_err(BuildError::Message)?;
    // Link to a temporary name, then rename: concurrent builds never see a partial binary.
    let tmp = bin_dir.join(format!("{key}.tmp{}", tmp_tag()));
    let mut cmd = Command::new(&ld);
    if ld != cc {
        cmd.args(extra.split_whitespace()).arg("-o").arg(&tmp).arg(&obj);
    } else if separate {
        // (the compiler's flags, as one command would have them, for what they mean to a link)
        cmd.args(&flags).arg("-o").arg(&tmp).arg(&obj);
    } else {
        cmd.args(&program_flags).arg("-o").arg(&tmp).arg(&c_path);
    }
    cmd.arg(&rt_obj);
    cmd.args(&re_objs);
    if lld {
        cmd.arg("-fuse-ld=lld");
    }
    if let Some((path, _)) = &gnu_order {
        cmd.arg(format!("-Wl,--section-ordering-file={}", path.display()));
    }
    if let Some((path, _)) = order {
        cmd.arg(format!("-Wl,--symbol-ordering-file={}", path.display())).arg("-Wl,--no-warn-symbol-ordering");
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
                // (Node-API's functions only, not every symbol of the program and the engine)
                let list = c_dir.join("napi-exports.list");
                if !list.is_file() {
                    let _ = write_new(&list, |t| std::fs::write(t, "{ napi_*; node_api_*; };\n"));
                }
                cmd.arg(format!("-Wl,--dynamic-list={}", list.display()));
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
        // Only `main` is exported: the runtime's functions needn't be in the export table. And a
        // library nothing left calls isn't loaded (Tov's JavaScriptCore no longer calls Foundation
        // or CoreFoundation, which loading costs a program 0.9 ms and 4 MB as it starts; an engine
        // built before still links them).
        cmd.arg("-Wl,-dead_strip").arg("-Wl,-dead_strip_dylibs").arg("-Wl,-exported_symbol,_main");
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
    // (the addons first: a binary in the cache has them beside it)
    for (bytes, to) in addon_bytes.iter().zip(&addons) {
        write_new(to, |t| std::fs::write(t, bytes)).map_err(|e| BuildError::Message(format!("can't write {}: {e}", to.display())))?;
    }
    std::fs::rename(&tmp, &binary).map_err(|e| BuildError::Message(format!("can't move the binary into the cache: {e}")))?;
    let (binary, addons, appended_blob) = match bundling {
        Some(h) => with_bundle(binary, h)?,
        None => (binary, addons, None),
    };
    let (binary, blob) = installable(binary, appended_blob, opts)?;
    let t3 = Instant::now();
    Ok(Built { binary, blob, addons, tls: uses_tls, jsc_own, cached: false, sources, npm: !npm.is_empty(), timings: (t1 - t0, t2 - t1, t3 - t2) })
}

fn js_object_paths(c_dir: &Path, key: &str) -> Vec<PathBuf> {
    vec![c_dir.join(format!("js-{key}.o")), c_dir.join(format!("node-{key}.o")), c_dir.join(format!("napi-{key}.o"))]
}

/// The bridge's objects as kept beside Tov's engine: compiled once for it, as they are part of
/// what programs link with it, so a cache of its own (a fresh TOV_CACHE_DIR, say) takes them
/// from there.
fn js_kept_paths(objs: &[PathBuf]) -> Option<Vec<PathBuf>> {
    own_jsc_dir().map(|d| objs.iter().map(|o| d.join("bridge").join(o.file_name().unwrap())).collect())
}

/// Whether js_objects has its objects without compiling them.
fn js_objects_ready(c_dir: &Path, key: &str) -> bool {
    let objs = js_object_paths(c_dir, key);
    objs.iter().all(|o| cached(o)) || js_kept_paths(&objs).is_some_and(|k| k.iter().all(|k| cached(k)))
}

/// The compiler flags without the precompiled header's `-include` (for other C files).
/// runtime/js.c and runtime/node.c compiled once per compiler and flags: (js.o, node.o).
fn js_objects(cc: &str, flags: &[String], c_dir: &Path, key: &str) -> Result<Vec<PathBuf>, BuildError> {
    let objs = js_object_paths(c_dir, key);
    if objs.iter().all(|o| cached(o)) {
        return Ok(objs);
    }
    let kept = js_kept_paths(&objs);
    if let Some(kept) = &kept
        && kept.iter().all(|k| cached(k))
    {
        for (k, o) in kept.iter().zip(&objs) {
            let tmp = o.with_extension(format!("tmp{}.o", tmp_tag()));
            if std::fs::copy(k, &tmp).is_err() || std::fs::rename(&tmp, o).is_err() {
                let _ = std::fs::remove_file(&tmp);
                return Err(BuildError::Message("can't copy the JavaScript bridge into the cache".into()));
            }
        }
        return Ok(objs);
    }
    let fail = |what: String| BuildError::Message(format!("can't build the JavaScript bridge: {what}"));
    let dir = c_dir.join(format!("js-{key}.tmp{}", tmp_tag()));
    std::fs::create_dir_all(&dir).map_err(|e| fail(e.to_string()))?;
    for (name, text) in [("tov.h", codegen::RUNTIME_H), ("js.h", codegen::JS_H), ("js.c", JS_C), ("node.c", NODE_C), ("node_tty.h", NODE_TTY_H), ("napi.c", NAPI_C)] {
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
                    let tmp = obj.with_extension(format!("tmp{}.o", tmp_tag()));
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
    for name in ["tov.h", "js.h", "js.c", "node.c", "node_tty.h", "napi.c"] {
        let _ = std::fs::remove_file(dir.join(name));
    }
    let _ = std::fs::remove_dir(&dir);
    if let Some(kept) = &kept
        && let Some(parent) = kept[0].parent()
        && std::fs::create_dir_all(parent).is_ok()
    {
        for (k, o) in kept.iter().zip(&objs) {
            let tmp = k.with_extension(format!("tmp{}.o", tmp_tag()));
            if std::fs::copy(o, &tmp).is_ok() {
                let _ = std::fs::rename(&tmp, k);
            }
        }
    }
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
    /// Linux: the order to lay out the engine's and runtime's functions in (scripts/jsc/order-linux.txt,
    /// kept beside the engine), and a hash of it, for lld
    order: Option<(PathBuf, String)>,
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

/// Linux: whether ICU's static libraries are where the compiler looks for libraries (Debian's
/// libicu-dev has them). Asked of the compiler once (`-print-search-dirs`, its library
/// directories); `build` asks on a thread as it starts, so the answer is in by the time a
/// program with npm packages links (three `-print-file-name`s in a row were ~75 ms of a cold
/// build).
fn static_icu() -> bool {
    static ICU: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ICU.get_or_init(|| {
        let Ok(out) = Command::new(c_compiler().0).arg("-print-search-dirs").output() else { return false };
        let text = String::from_utf8_lossy(&out.stdout);
        let dirs: Vec<PathBuf> = text.lines().filter_map(|l| l.strip_prefix("libraries: ")).flat_map(|l| std::env::split_paths(l.trim_start_matches('=')).collect::<Vec<_>>()).collect();
        ["libicuuc.a", "libicui18n.a", "libicudata.a"].iter().all(|name| dirs.iter().any(|d| d.join(name).is_file()))
    })
}

fn jsc_flags() -> Result<JscFlags, BuildError> {
    if let Some(dir) = own_jsc_dir() {
        // (an engine built from older patches lacks API the bridge calls: say so before C does)
        let api = std::fs::read_to_string(dir.join("include/JavaScriptCore/TovAPI.h")).unwrap_or_default();
        if api != TOV_API_H {
            eprintln!("warning: Tov's JavaScriptCore in {} is older than this compiler (its TovAPI.h differs): rebuild it with scripts/jsc/build.sh, or set TOV_JSC=system", dir.display());
        }
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
            // ICU and the C++ library linked in, as Bun links them, when the static libraries are
            // there (Debian's libicu-dev has them): a program then binds no symbols of theirs as it
            // starts (the dynamic loader was a sixth of an npm program's start); and the program
            // isn't position independent (`-no-pie`: no relocations to apply at start either)
            if static_icu() {
                libs.extend(["-Wl,-Bstatic", "-licui18n", "-licuuc", "-licudata", "-lstdc++", "-latomic", "-Wl,-Bdynamic", "-ldl", "-no-pie"].map(String::from));
            } else {
                libs.extend(["-licui18n", "-licuuc", "-licudata", "-lstdc++", "-ldl", "-latomic"].map(String::from));
            }
        }
        // (the functions that run as programs start and serve, together: what starting touches
        // of the engine's 27 MB of code is a few MB of pages, not most of it in 64 KB windows)
        let order = (!cfg!(target_vendor = "apple")).then(|| dir.join("order.txt")).and_then(|p| std::fs::read(&p).ok().map(|b| (p, hash_hex(&[&b]))));
        return Ok(JscFlags { cflags, libs, own: true, order });
    }
    if cfg!(target_vendor = "apple") {
        return Ok(JscFlags { cflags: Vec::new(), libs: ["-framework", "JavaScriptCore", "-framework", "CoreFoundation", "-lobjc"].map(String::from).to_vec(), own: false, order: None });
    }
    let pkg = |package: &str, what: &str| -> Option<Vec<String>> {
        let out = Command::new("pkg-config").arg(what).arg(package).output().ok()?;
        out.status.success().then(|| String::from_utf8_lossy(&out.stdout).split_whitespace().map(String::from).collect())
    };
    for package in ["javascriptcoregtk-4.1", "javascriptcoregtk-6.0"] {
        if let (Some(cflags), Some(mut libs)) = (pkg(package, "--cflags"), pkg(package, "--libs")) {
            libs.push("-ldl".into());
            return Ok(JscFlags { cflags, libs, own: false, order: None });
        }
    }
    Err(BuildError::Message("npm packages need JavaScriptCore: install WebKitGTK's (on Debian and Ubuntu, libjavascriptcoregtk-4.1-dev; pkg-config finds it)".into()))
}

/// The program: its binary as linked (`base`, which holds no JavaScript) with the blob appended
/// (runtime/js.c maps it from there): `base` + "-" + the blob's name, made once per blob (on
/// APFS the copy shares the base's blocks). The binary is page-aligned, then the blob, a NUL,
/// the blob's offset and length (u64s, little-endian) and "TOV_BLOB". Older programs made from
/// the same base are removed (each would otherwise keep a copy of the engine).
/// Copies `from` to `to` (a new file): on Linux as a clone of its blocks where the file system
/// can (btrfs, XFS), which a binary with the engine and ICU in it (~70 MB) otherwise spends ~35
/// ms writing; elsewhere `std::fs::copy` (macOS clones on APFS itself).
fn copy_file(from: &Path, to: &Path) -> std::io::Result<()> {
    #[cfg(target_os = "linux")]
    {
        use std::os::fd::AsRawFd;
        use std::os::unix::fs::PermissionsExt;
        unsafe extern "C" {
            fn ioctl(fd: i32, request: u64, ...) -> i32;
        }
        const FICLONE: u64 = 0x4004_9409;
        let src = std::fs::File::open(from)?;
        let mode = src.metadata()?.permissions().mode();
        let dst = std::fs::File::create(to)?;
        // SAFETY: both descriptors are open files; FICLONE takes the source's descriptor
        if unsafe { ioctl(dst.as_raw_fd(), FICLONE, src.as_raw_fd()) } == 0 {
            return dst.set_permissions(std::fs::Permissions::from_mode(mode));
        }
        drop(dst);
    }
    std::fs::copy(from, to).map(|_| ())
}

fn with_blob(base: &Path, blob: &Path) -> Result<PathBuf, BuildError> {
    let fail = |e: std::io::Error| BuildError::Message(format!("can't append the program's JavaScript: {e}"));
    let name = base.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let blob_name = blob.file_stem().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let out = base.with_file_name(format!("{name}-{blob_name}"));
    if !cached(&out) {
        let tmp = out.with_file_name(format!("{name}-{blob_name}.tmp{}", tmp_tag()));
        if let Err(e) = append_blob(base, blob, &tmp) {
            let _ = std::fs::remove_file(&tmp);
            return Err(fail(e));
        }
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

/// Writes `to` (a new file): `base` and then `blob`, as with_blob describes.
fn append_blob(base: &Path, blob: &Path, to: &Path) -> std::io::Result<()> {
    use std::io::Write;
    copy_file(base, to)?;
    let js = std::fs::read(blob)?;
    let mut f = std::fs::OpenOptions::new().append(true).open(to)?;
    let len = f.metadata()?.len();
    const PAGE: u64 = 16384;
    let at = len.div_ceil(PAGE) * PAGE;
    f.write_all(&vec![0u8; (at - len) as usize])?;
    f.write_all(&js)?;
    f.write_all(&[0])?;
    f.write_all(&at.to_le_bytes())?;
    f.write_all(&(js.len() as u64 + 1).to_le_bytes())?;
    f.write_all(b"TOV_BLOB")
}

/// npm packages bundled on a thread (see `bundling` in `build`): the bundle's blob in the cache,
/// and its native addons put beside a copy of the binary in a directory of their own (a hard
/// link: the cache's files aren't changed in place), as `tov run` loads them from beside the
/// program. (binary, addons, blob)
fn with_bundle(binary: PathBuf, bundling: std::thread::JoinHandle<Result<crate::npm::Bundle, String>>) -> Result<(PathBuf, Vec<PathBuf>, Option<PathBuf>), BuildError> {
    let b = bundling.join().map_err(|_| BuildError::Message("internal error: bundling the npm packages panicked".into()))?;
    let b = b.map_err(|e| BuildError::Message(format!("can't bundle the npm packages: {e}")))?;
    let blob = write_blob(&b)?;
    if b.addons.is_empty() {
        return Ok((binary, Vec::new(), Some(blob)));
    }
    let fail = |e: std::io::Error| BuildError::Message(format!("can't put the program's addons in the cache: {e}"));
    let mut bytes = Vec::new();
    for a in &b.addons {
        bytes.push(std::fs::read(a).map_err(|e| BuildError::Message(format!("can't read {}: {e}", a.display())))?);
    }
    let name = binary.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let dir = binary.with_file_name(format!("{name}-{}", hash_hex(&bytes.iter().map(|b| b.as_slice()).collect::<Vec<_>>())));
    let placed = dir.join(&name);
    let addons: Vec<PathBuf> = b.addons.iter().map(|a| dir.join(a.file_name().unwrap_or_default())).collect();
    if cached(&placed) {
        let _ = std::fs::File::open(&dir).and_then(|f| f.set_modified(SystemTime::now()));
    } else {
        std::fs::create_dir_all(&dir).map_err(fail)?;
        for (bytes, to) in bytes.iter().zip(&addons) {
            write_new(to, |t| std::fs::write(t, bytes)).map_err(fail)?;
        }
        let tmp = dir.join(format!("{name}.tmp{}", tmp_tag()));
        std::fs::hard_link(&binary, &tmp).or_else(|_| copy_file(&binary, &tmp)).and_then(|_| std::fs::rename(&tmp, &placed)).map_err(fail)?;
    }
    Ok((placed, addons, Some(blob)))
}

/// The bundle's blob in the cache (`npm-<hash>.blob`), written there the first time.
fn write_blob(b: &crate::npm::Bundle) -> Result<PathBuf, BuildError> {
    if std::env::var_os("TOV_TRACE").is_some() {
        for w in &b.warnings {
            eprintln!("npm: {w}");
        }
    }
    let blob = b.blob();
    let c_dir = cache_dir().join("c");
    std::fs::create_dir_all(&c_dir).map_err(|e| BuildError::Message(format!("can't create {}: {e}", c_dir.display())))?;
    let path = c_dir.join(format!("npm-{}.blob", hash_hex(&[&blob])));
    if !cached(&path) {
        let tmp = path.with_extension(format!("tmp{}", tmp_tag()));
        std::fs::write(&tmp, &blob).and_then(|_| std::fs::rename(&tmp, &path)).map_err(|e| BuildError::Message(format!("can't write {}: {e}", path.display())))?;
    }
    Ok(path)
}

/// A linked binary and the JavaScript to go after it: the program in the cache (with_blob), or
/// for a build only installed, the two for `install` to put together.
fn installable(binary: PathBuf, blob: Option<PathBuf>, opts: &Options) -> Result<(PathBuf, Option<PathBuf>), BuildError> {
    match blob {
        Some(blob) if !opts.install => Ok((with_blob(&binary, &blob)?, None)),
        blob => Ok((binary, blob)),
    }
}

/// Writes `to` through a new file renamed over it: macOS kills a signed binary (or one whose
/// addon is) rewritten in place, since it keeps the old file's signature.
fn write_new(to: &Path, write: impl FnOnce(&Path) -> std::io::Result<()>) -> std::io::Result<()> {
    let name = to.file_name().map_or("tov".into(), |n| n.to_string_lossy().into_owned());
    let tmp = to.with_file_name(format!(".{name}.tmp{}", tmp_tag()));
    let done = write(&tmp).and_then(|_| std::fs::rename(&tmp, to));
    if done.is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
    done
}

/// Puts a built program at `dest`, with the Node-API addons it loads beside it.
pub fn install(built: &Built, dest: &Path) -> std::io::Result<()> {
    let dir = dest.parent().unwrap_or(Path::new("."));
    if let Some(a) = built.addons.iter().find(|a| a.file_name() == dest.file_name()) {
        return Err(std::io::Error::new(std::io::ErrorKind::InvalidInput, format!("the program would replace its addon {}, which it loads from beside it", a.file_name().unwrap_or_default().to_string_lossy())));
    }
    for a in &built.addons {
        write_new(&dir.join(a.file_name().unwrap_or_default()), |t| std::fs::copy(a, t).map(|_| ()))?;
    }
    // (a program whose JavaScript is appended is written here from its parts: on a file system
    // without clones a copy of the program from the cache would write its ~80 MB twice)
    match &built.blob {
        Some(blob) => write_new(dest, |t| append_blob(&built.binary, blob, t)),
        None => write_new(dest, |t| copy_file(&built.binary, t)),
    }
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
const LINK_FLAGS: &str = "dead-strip; dead-strip-dylibs; exported: _main; -x unless -g; sanitizers link without -stack_size; -Wl,-stack_size,0x20000000; Linux: --as-needed after the runtime, lld when installed";

/// The precompiled fixed prefix (`pre-<key>.h`, with its `.pch`/`.gch` beside it) to pass as
/// `-include`, built on first use; `None` if the compiler can't precompile it (then programs
/// are compiled whole, which is only slower).
fn is_clang(cc: &str) -> bool {
    cfg!(target_vendor = "apple") || Path::new(cc).file_name().is_some_and(|n| n.to_string_lossy().contains("clang"))
}

fn precompiled_header(cc: &str, flags: &[&str], c_dir: &Path, prefix: &str, rt_key: &str) -> Option<PathBuf> {
    let clang = is_clang(cc);
    let key = hash_hex(&[prefix.as_bytes(), rt_key.as_bytes()]);
    let header = c_dir.join(format!("pre-{key}.h"));
    let pch = c_dir.join(format!("pre-{key}.h.{}", if clang { "pch" } else { "gch" }));
    // (not the header's time: clang checks the precompiled header against it)
    if cached(&pch) && header.is_file() {
        return Some(header);
    }
    // (compiled with Tov, for its default flags and compiler: written out as it is)
    if clang && key == tls_files::PREBUILT_PCH_KEY && !tls_files::PREBUILT_PCH.is_empty() {
        let tmp = c_dir.join(format!("pre-{key}.tmp{}.pch", tmp_tag()));
        if (header.is_file() || write_new(&header, |t| std::fs::write(t, prefix)).is_ok()) && std::fs::write(&tmp, tls_files::PREBUILT_PCH).and_then(|_| std::fs::rename(&tmp, &pch)).is_ok() {
            return Some(header);
        }
        let _ = std::fs::remove_file(&tmp);
    }
    let pid = tmp_tag();
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

    /// Whether `build` has the archives without compiling them (cached, or carried by the compiler).
    fn ready(&self, c_dir: &Path) -> bool {
        let vendor = cached(&c_dir.join(format!("vendor-{}.a", self.vendor_key))) || (self.vendor_key == tls_files::VENDOR_ARCHIVE_KEY && !tls_files::VENDOR_ARCHIVE.is_empty());
        let runtime = cached(&c_dir.join(format!("tls-{}.a", self.key))) || (self.key == tls_files::RUNTIME_ARCHIVE_KEY && !tls_files::RUNTIME_ARCHIVE.is_empty());
        vendor && runtime
    }

    /// The archives to link (the runtime's, then the vendored libraries it uses), compiling them
    /// first if they aren't cached.
    fn build(&self, cc: &str, c_dir: &Path) -> Result<[PathBuf; 2], BuildError> {
        let fail = |what: String| BuildError::Message(format!("can't build the TLS library: {what}"));
        let vendor = c_dir.join(format!("vendor-{}.a", self.vendor_key));
        if !cached(&vendor) {
            if self.vendor_key == tls_files::VENDOR_ARCHIVE_KEY && !tls_files::VENDOR_ARCHIVE.is_empty() {
                let tmp = c_dir.join(format!("vendor-{}.tmp{}.a", self.vendor_key, tmp_tag()));
                std::fs::write(&tmp, tls_files::VENDOR_ARCHIVE).map_err(|e| fail(format!("{e}")))?;
                std::fs::rename(&tmp, &vendor).map_err(|e| fail(format!("can't move it into the cache: {e}")))?;
            } else {
                eprintln!("tov: compiling the TLS library (BoringSSL, brotli, libdeflate, zlib, zstd); this happens once");
                self.compile(cc, c_dir, &vendor, |p| p.starts_with("vendor/"))?;
            }
        }
        let runtime = c_dir.join(format!("tls-{}.a", self.key));
        if !cached(&runtime) {
            if self.key == tls_files::RUNTIME_ARCHIVE_KEY && !tls_files::RUNTIME_ARCHIVE.is_empty() {
                let tmp = c_dir.join(format!("tls-{}.tmp{}.a", self.key, tmp_tag()));
                std::fs::write(&tmp, tls_files::RUNTIME_ARCHIVE).map_err(|e| fail(format!("{e}")))?;
                std::fs::rename(&tmp, &runtime).map_err(|e| fail(format!("can't move it into the cache: {e}")))?;
            } else {
                self.compile(cc, c_dir, &runtime, |p| p.starts_with("runtime/"))?;
            }
        }
        Ok([runtime, vendor])
    }

    /// Compiles the TLS files `pick` chooses into `archive`, in parallel.
    fn compile(&self, cc: &str, c_dir: &Path, archive: &Path, pick: impl Fn(&str) -> bool) -> Result<(), BuildError> {
        let fail = |what: String| BuildError::Message(format!("can't build the TLS library: {what}"));
        let name = archive.file_stem().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let dir = c_dir.join(format!("{name}.tmp{}", tmp_tag()));
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
        let tmp = c_dir.join(format!("{name}.tmp{}.a", tmp_tag()));
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
/// The regular expression library's objects (runtime/regexp.c, QuickJS's libregexp and PCRE2),
/// compiled once per compiler and flags, in parallel.
fn regexp_units(c_dir: &Path, key: &str) -> (Vec<&'static str>, Vec<PathBuf>) {
    let mut units: Vec<&str> = vec!["regexp.c", "libregexp.c", "libunicode.c", "cutils.c"];
    units.extend(codegen::REGEXP_FILES.iter().map(|f| f.0).filter(|n| n.starts_with("pcre2/src/") && n.ends_with(".c")));
    let stem = |u: &str| Path::new(u).file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    let objs: Vec<PathBuf> = units.iter().map(|u| c_dir.join(format!("re-{key}-{}.o", stem(u)))).collect();
    (units, objs)
}

fn regexp_objects(cc: &str, flags: &[&str], c_dir: &Path, key: &str) -> Result<Vec<PathBuf>, BuildError> {
    let (units, objs) = regexp_units(c_dir, key);
    if objs.iter().all(|o| cached(o)) {
        return Ok(objs);
    }
    let fail = |what: String| BuildError::Message(format!("can't build the regular expression library: {what}"));
    let dir = c_dir.join(format!("re-{key}.tmp{}", tmp_tag()));
    for (name, text) in codegen::REGEXP_FILES.iter().copied().chain([("tov.h", codegen::RUNTIME_H)]) {
        let path = dir.join(name);
        std::fs::create_dir_all(path.parent().unwrap_or(&dir)).map_err(|e| fail(format!("{e}")))?;
        std::fs::write(&path, text).map_err(|e| fail(format!("{e}")))?;
    }
    let pcre = dir.join("pcre2/src");
    let result = std::thread::scope(|scope| {
        let jobs: Vec<_> = units
            .iter()
            .zip(&objs)
            .filter(|(_, obj)| !cached(obj))
            .map(|(unit, obj)| {
                let (dir, pcre) = (&dir, &pcre);
                scope.spawn(move || -> Result<(), BuildError> {
                    let tmp = obj.with_extension(format!("tmp{}.o", tmp_tag()));
                    let mut cmd = Command::new(cc);
                    cmd.args(flags).args(["-w", "-ffunction-sections", "-fdata-sections", "-DHAVE_CONFIG_H", "-c", "-o"]).arg(&tmp);
                    cmd.arg(format!("-I{}", dir.display())).arg(format!("-I{}", pcre.display())).arg(dir.join(unit));
                    run_cc(cmd, cc, &dir.join(unit))?;
                    std::fs::rename(&tmp, obj).map_err(|e| BuildError::Message(format!("can't build the regular expression library: can't move it into the cache: {e}")))
                })
            })
            .collect();
        for j in jobs {
            j.join().map_err(|_| fail("a compiler thread panicked".into()))??;
        }
        Ok(objs.clone())
    });
    let _ = std::fs::remove_dir_all(&dir);
    result
}

/// A name for a temporary file no other build makes: the process, and a count within it (the
/// threads of one process, `cargo test`'s say, build at once).
pub(crate) fn tmp_tag() -> String {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    format!("{}-{}", std::process::id(), NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed))
}

pub fn hash_hex(parts: &[&[u8]]) -> String {
    tls_flags::hash_hex(parts)
}

/// Statements (module, statement) a `// @ts-expect-error` covers that have errors.
fn expected_errors(modules: &[crate::check::Module], sm: &crate::source::SourceMap, diags: &[crate::diag::Diagnostic]) -> Vec<(usize, crate::ast::StmtId)> {
    let mut out = Vec::new();
    for d in diags {
        let Some(mi) = modules.iter().position(|m| m.file == d.span.file && !m.ast.ts_expect.is_empty()) else { continue };
        let ast = &modules[mi].ast;
        let file = sm.get(d.span.file);
        let line = file.line_col(d.span.start).0;
        if !ast.ts_expect.iter().any(|&o| file.line_col(o).0 == line) {
            continue;
        }
        // (the smallest statement holding the error that starts on that line)
        let best = ast
            .stmts
            .iter()
            .enumerate()
            .filter(|(_, st)| st.span.file == d.span.file && st.span.start <= d.span.start && d.span.end <= st.span.end && file.line_col(st.span.start).0 == line)
            .filter(|(_, st)| !matches!(st.kind, crate::ast::StmtKind::Block(_)))
            .min_by_key(|(_, st)| st.span.end - st.span.start)
            .map(|(i, _)| i as crate::ast::StmtId);
        if let Some(s) = best
            && !out.contains(&(mi, s))
        {
            out.push((mi, s));
        }
    }
    out
}

/// Each statement becomes `throw new TypeError(...)`.
/// Statement `s` is `xs[i] = undefined` or `o.f = null`: a store of nothing into an element or
/// a field.
fn clears_a_slot(ast: &crate::ast::Ast, s: crate::ast::StmtId) -> bool {
    use crate::ast::{AssignOp, ExprKind, StmtKind};
    let StmtKind::Expr(e) = ast.stmt(s).kind else { return false };
    let ExprKind::Assign(AssignOp::Assign, t, v) = ast.expr(e).kind else { return false };
    matches!(ast.expr(t).kind, ExprKind::Index { .. } | ExprKind::Member { .. }) && matches!(ast.expr(v).kind, ExprKind::Undefined | ExprKind::Null)
}

fn throw_instead(modules: &mut [crate::check::Module], interner: &mut crate::intern::Interner, targets: &[(usize, crate::ast::StmtId)]) {
    use crate::ast::{Arg, Expr, ExprKind, StmtKind};
    let te = interner.intern("TypeError");
    let msg = interner.intern("a value of the wrong type (`@ts-expect-error`)");
    for &(mi, s) in targets {
        let ast = &mut modules[mi].ast;
        let span = ast.stmts[s as usize].span;
        ast.exprs.push(Expr { kind: ExprKind::Ident(te), span });
        let callee = ast.exprs.len() as crate::ast::ExprId - 1;
        ast.exprs.push(Expr { kind: ExprKind::Str(msg), span });
        let m = ast.exprs.len() as crate::ast::ExprId - 1;
        ast.exprs.push(Expr { kind: ExprKind::New { callee, type_args: Vec::new(), args: vec![Arg { expr: m, by_ref: None }] }, span });
        let n = ast.exprs.len() as crate::ast::ExprId - 1;
        ast.stmts[s as usize].kind = StmtKind::Throw(n);
    }
}
