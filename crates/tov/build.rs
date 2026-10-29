//! Cargo build script: embeds the fetch() library sources (runtime/tls.c and codecs.c with the
//! vendored BoringSSL, brotli, libdeflate, zlib and zstd) in the compiler, so `tov build` can
//! compile them into the archives that fetch() programs link (see src/build.rs), and hashes them
//! once here rather than on every build. It also compiles the vendored libraries, once (cached in
//! OUT_DIR by their sources and flags), into the archive the compiler carries: a machine's first
//! fetch() program then doesn't spend ~12 s compiling them.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::process::Command;

#[allow(dead_code)]
#[path = "src/tls_flags.rs"]
mod tls_flags;

#[allow(dead_code)]
#[path = "src/rt_flags.rs"]
mod rt_flags;

/// The npm bundler's JavaScript tokenizer: shims are minified here, once.
#[allow(dead_code)]
#[path = "src/npm/lex.rs"]
mod lex;

fn files(dir: &Path, out: &mut Vec<PathBuf>) {
    let mut entries: Vec<PathBuf> = std::fs::read_dir(dir).unwrap().flatten().map(|e| e.path()).collect();
    entries.sort();
    for p in entries {
        if p.is_dir() {
            files(&p, out);
        } else if p.extension().is_some_and(|e| matches!(e.to_str(), Some("c" | "cc" | "h" | "inc" | "S"))) {
            out.push(p);
        }
    }
}

fn main() {
    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("../..").canonicalize().unwrap();
    let vendor = root.join("vendor");
    let mut list = Vec::new();
    for lib in ["boringssl", "brotli", "libdeflate", "zlib", "zstd"] {
        files(&vendor.join(lib), &mut list);
    }
    list.push(root.join("runtime/tls.c"));
    list.push(root.join("runtime/codecs.c"));
    list.push(root.join("runtime/crypto.c"));
    list.push(root.join("runtime/compress.c"));
    // (one hash for the vendored libraries, one for the runtime's files that use them)
    let mut vendor_parts: Vec<Vec<u8>> = Vec::new();
    let mut runtime_parts: Vec<Vec<u8>> = Vec::new();
    let mut code = String::from("/// (path relative to the repository, contents)\npub static TLS_FILES: &[(&str, &[u8])] = &[\n");
    for p in &list {
        let rel = p.strip_prefix(&root).unwrap().to_string_lossy().replace('\\', "/");
        let _ = writeln!(code, "    ({rel:?}, include_bytes!({:?})),", p.to_string_lossy());
        let parts = if rel.starts_with("vendor/") { &mut vendor_parts } else { &mut runtime_parts };
        parts.push(rel.into_bytes());
        parts.push(std::fs::read(p).unwrap());
    }
    code.push_str("];\n");
    let hash = |parts: &[Vec<u8>]| tls_flags::hash_hex(&parts.iter().map(|p| p.as_slice()).collect::<Vec<_>>());
    let vendor_hash = hash(&vendor_parts);
    let _ = writeln!(code, "/// A hash of TLS_FILES' vendored libraries.\npub const VENDOR_KEY: &str = \"{vendor_hash}\";");
    let _ = writeln!(code, "/// A hash of TLS_FILES' runtime files (runtime/tls.c, ...).\npub const RUNTIME_KEY: &str = \"{}\";", hash(&runtime_parts));
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    match vendor_archive(&root, &list, &out_dir, &vendor_hash) {
        Some((key, path)) => {
            let _ = writeln!(code, "/// The vendored libraries, compiled with the plain flags (tls_flags), and that archive's key.\npub const VENDOR_ARCHIVE_KEY: &str = \"{key}\";\npub static VENDOR_ARCHIVE: &[u8] = include_bytes!({:?});", path.to_string_lossy());
        }
        None => code.push_str("pub const VENDOR_ARCHIVE_KEY: &str = \"\";\npub static VENDOR_ARCHIVE: &[u8] = &[];\n"),
    }
    match runtime_tls_archive(&root, &list, &out_dir, &hash(&runtime_parts), &vendor_hash) {
        Some((key, path)) => {
            let _ = writeln!(code, "/// The runtime's TLS files compiled (TlsArchive's archive), and its key.\npub const RUNTIME_ARCHIVE_KEY: &str = \"{key}\";\npub static RUNTIME_ARCHIVE: &[u8] = include_bytes!({:?});", path.to_string_lossy());
        }
        None => code.push_str("pub const RUNTIME_ARCHIVE_KEY: &str = \"\";\npub static RUNTIME_ARCHIVE: &[u8] = &[];\n"),
    }
    match prebuilt_runtime(&root, &out_dir) {
        Some((key, path)) => {
            let _ = writeln!(code, "/// The runtime compiled with the default flags (rt_flags), and its cache key.\npub const PREBUILT_RT_KEY: &str = \"{key}\";\npub static PREBUILT_RT: &[u8] = include_bytes!({:?});", path.to_string_lossy());
        }
        None => code.push_str("pub const PREBUILT_RT_KEY: &str = \"\";\npub static PREBUILT_RT: &[u8] = &[];\n"),
    }
    let out = out_dir.join("tls_files.rs");
    std::fs::write(out, code).unwrap();
    println!("cargo:rerun-if-changed={}", vendor.display());
    println!("cargo:rerun-if-changed={}", root.join("runtime/tls.c").display());
    println!("cargo:rerun-if-changed={}", root.join("runtime/codecs.c").display());
    println!("cargo:rerun-if-changed={}", root.join("runtime/crypto.c").display());
    println!("cargo:rerun-if-changed={}", root.join("runtime/compress.c").display());
    for p in &list {
        println!("cargo:rerun-if-changed={}", p.display());
    }
    node_shims(&root);
    regexp_library(&root, &out_dir);
}

/// QuickJS's libregexp compiled into the compiler itself: `tov build` compiles a program's
/// patterns to bytecode, the very bytecode its runtime makes, and that into C (codegen/rejit.rs).
fn regexp_library(root: &Path, out_dir: &Path) {
    let dir = root.join("vendor/quickjs-regexp");
    let cc = if cfg!(target_vendor = "apple") { rt_flags::xcrun_toolchain().map(|(c, _)| c).unwrap_or_else(rt_flags::other_compiler) } else { rt_flags::other_compiler() };
    let sysroot = if cfg!(target_vendor = "apple") { rt_flags::xcrun_toolchain().map(|(_, s)| s) } else { None };
    let mut objs = Vec::new();
    for unit in ["libregexp.c", "libunicode.c", "cutils.c"] {
        let obj = out_dir.join(format!("tovre-{}.o", unit.trim_end_matches(".c")));
        let mut cmd = Command::new(&cc);
        if let Some(s) = &sysroot {
            cmd.args(["-isysroot", s]);
        }
        let ok = cmd.args(["-O2", "-w", "-fPIC", "-c", "-o"]).arg(&obj).arg(format!("-I{}", dir.display())).arg(dir.join(unit)).status().map(|s| s.success()).unwrap_or(false);
        assert!(ok, "couldn't compile vendor/quickjs-regexp/{unit}");
        objs.push(obj);
        println!("cargo:rerun-if-changed={}", dir.join(unit).display());
    }
    let lib = out_dir.join("libtovre.a");
    let _ = std::fs::remove_file(&lib);
    let ok = Command::new("ar").arg("rcs").arg(&lib).args(&objs).status().map(|s| s.success()).unwrap_or(false);
    assert!(ok, "couldn't archive libregexp");
    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=tovre");
    for h in ["libregexp.h", "libregexp-exec.h", "libregexp-opcode.h", "libunicode.h", "libunicode-table.h", "cutils.h"] {
        println!("cargo:rerun-if-changed={}", dir.join(h).display());
    }
}

/// The vendored libraries compiled into an archive in OUT_DIR, as `tov build` would compile them
/// with the plain flags (no sanitizers) and the compiler it picks (Xcode's clang on macOS): its key
/// and path. None when it can't be (another target, no compiler), and `tov build` compiles them.
fn vendor_archive(root: &Path, list: &[PathBuf], out_dir: &Path, vendor_hash: &str) -> Option<(String, PathBuf)> {
    let (cc, cxx, sdk) = archive_toolchain()?;
    let key = tls_flags::vendor_key(vendor_hash, &[]);
    let archive = out_dir.join(format!("vendor-{key}.a"));
    if archive.is_file() {
        return Some((key, archive));
    }
    if !compile_archive(root, list, out_dir, "vendor", &archive, &cc, &cxx, sdk.as_deref()) {
        println!("cargo:warning=couldn't precompile the vendored TLS libraries; `tov build` will compile them when a program needs them");
        return None;
    }
    Some((key, archive))
}

/// The runtime's TLS files (runtime/tls.c, codecs.c, crypto.c, compress.c) compiled into the
/// archive `tov build` makes of them (TlsArchive in src/build.rs, same key): (its key, path).
fn runtime_tls_archive(root: &Path, list: &[PathBuf], out_dir: &Path, runtime_hash: &str, vendor_hash: &str) -> Option<(String, PathBuf)> {
    let (cc, cxx, sdk) = archive_toolchain()?;
    let mut flags: Vec<String> = tls_flags::FLAGS.iter().map(|f| f.to_string()).collect();
    if let Some(sdk) = &sdk {
        flags.push("-isysroot".into());
        flags.push(sdk.clone());
    }
    let vendor_key = tls_flags::vendor_key(vendor_hash, &[]);
    let tov_h = std::fs::read(root.join("runtime/tov.h")).ok()?;
    let key = tls_flags::hash_hex(&[runtime_hash.as_bytes(), vendor_key.as_bytes(), &tov_h, cc.as_bytes(), flags.join(" ").as_bytes()]);
    let archive = out_dir.join(format!("tls-{key}.a"));
    if archive.is_file() {
        return Some((key, archive));
    }
    if !compile_archive(root, list, out_dir, "runtime", &archive, &cc, &cxx, sdk.as_deref()) {
        return None;
    }
    Some((key, archive))
}

/// The C and C++ compilers `tov build` picks (Xcode's on macOS), and the SDK; None when cross
/// compiling or asked not to.
fn archive_toolchain() -> Option<(String, String, Option<String>)> {
    if std::env::var("TARGET").ok() != std::env::var("HOST").ok() || std::env::var_os("TOV_NO_PREBUILT_VENDOR").is_some() {
        return None;
    }
    let xcrun = |args: &[&str]| -> Option<String> {
        let out = Command::new("xcrun").args(args).output().ok()?;
        let s = String::from_utf8(out.stdout).ok()?.trim().to_string();
        (out.status.success() && !s.is_empty()).then_some(s)
    };
    let apple = std::env::var("CARGO_CFG_TARGET_VENDOR").as_deref() == Ok("apple");
    if apple {
        return Some((xcrun(&["--find", "clang"])?, xcrun(&["--find", "clang++"])?, xcrun(&["--show-sdk-path"])));
    }
    // (the compiler `tov build` picks, by the same path, or the archive's key never matches its
    // and every first fetch() program compiles the runtime's TLS files again: ~180 ms)
    let cc = std::env::var("CC").unwrap_or_else(|_| rt_flags::other_compiler());
    let cxx = std::env::var("CXX").unwrap_or_else(|_| {
        let path = Path::new(&cc);
        let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        match name.strip_prefix("clang").map(|rest| format!("clang++{rest}")).or_else(|| name.strip_prefix("gcc").map(|rest| format!("g++{rest}"))) {
            Some(n) => path.with_file_name(n).to_string_lossy().into_owned(),
            None => "c++".into(),
        }
    });
    Some((cc, cxx, None))
}

/// The TLS files under `prefix/` (vendor or runtime) compiled with tls_flags into `archive`.
#[allow(clippy::too_many_arguments)]
fn compile_archive(root: &Path, list: &[PathBuf], out_dir: &Path, prefix: &str, archive: &Path, cc: &str, cxx: &str, sdk: Option<&str>) -> bool {
    let objs_dir = out_dir.join(format!("{prefix}-objs"));
    let _ = std::fs::remove_dir_all(&objs_dir);
    if std::fs::create_dir_all(&objs_dir).is_err() {
        return false;
    }
    let sources: Vec<(&PathBuf, &'static [&'static str])> =
        list.iter().filter(|p| p.starts_with(root.join(prefix))).filter_map(|p| tls_flags::lang(&p.to_string_lossy()).map(|l| (p, l))).collect();
    let includes: Vec<String> = tls_flags::INCLUDES.iter().map(|i| format!("-I{}", root.join(i).display())).collect();
    let next = std::sync::atomic::AtomicUsize::new(0);
    let failed = std::sync::atomic::AtomicBool::new(false);
    let threads = std::thread::available_parallelism().map_or(4, |n| n.get());
    std::thread::scope(|s| {
        for _ in 0..threads {
            s.spawn(|| loop {
                let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                let Some((src, lang)) = sources.get(i) else { break };
                let compiler = if src.extension().is_some_and(|e| e == "cc") { cxx } else { cc };
                let mut cmd = Command::new(compiler);
                cmd.args(tls_flags::FLAGS).args(*lang).args(&includes);
                if let Some(sdk) = sdk {
                    cmd.arg("-isysroot").arg(sdk);
                }
                let ok = cmd.arg("-c").arg(src).arg("-o").arg(objs_dir.join(format!("{i}.o"))).output().is_ok_and(|o| o.status.success());
                if !ok {
                    failed.store(true, std::sync::atomic::Ordering::Relaxed);
                }
            });
        }
    });
    if failed.into_inner() {
        return false;
    }
    let tmp = out_dir.join(format!("{prefix}.tmp.a"));
    let _ = std::fs::remove_file(&tmp);
    let objs: Vec<PathBuf> = (0..sources.len()).map(|i| objs_dir.join(format!("{i}.o"))).collect();
    let ok = Command::new("ar").arg("rcs").arg(&tmp).args(&objs).output().is_ok_and(|o| o.status.success());
    let _ = std::fs::remove_dir_all(&objs_dir);
    ok && std::fs::rename(&tmp, archive).is_ok()
}

/// runtime/node/**/*.js as a sorted table of (module id, source) for the npm bundler: "path",
/// "internal/util", ... (see src/npm/node_shims.rs).
fn node_shims(root: &Path) {
    fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
        for e in std::fs::read_dir(dir).unwrap().flatten() {
            let p = e.path();
            if p.is_dir() {
                walk(&p, out);
            } else if p.extension().is_some_and(|e| e == "js") {
                out.push(p);
            }
        }
    }
    let dir = root.join("runtime/node");
    let mut files = Vec::new();
    if dir.is_dir() {
        walk(&dir, &mut files);
    }
    let mut entries: Vec<(String, PathBuf)> =
        files.into_iter().map(|p| (p.strip_prefix(&dir).unwrap().with_extension("").to_string_lossy().replace('\\', "/"), p)).collect();
    entries.sort();
    // Each shim is minified (comments and whitespace go; line breaks stay for automatic
    // semicolons), and its static requires found, here rather than in every bundle.
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("node");
    let mut code = String::from("/// (module id, minified source), sorted by id\npub static NODE_SHIMS: &[(&str, &str)] = &[\n");
    let mut requires = String::from("/// The static `require(...)` specifiers of each of NODE_SHIMS\npub static NODE_SHIM_REQUIRES: &[&[&str]] = &[\n");
    for (id, p) in &entries {
        let src = std::fs::read_to_string(p).unwrap();
        let min = lex::minify(&src).unwrap_or_else(|e| panic!("{}: {e}", p.display()));
        let reqs = lex::static_requires(&min).unwrap_or_else(|e| panic!("{}: {e}", p.display()));
        let dest = out.join(format!("{id}.js"));
        std::fs::create_dir_all(dest.parent().unwrap()).unwrap();
        if std::fs::read_to_string(&dest).ok().as_deref() != Some(min.as_str()) {
            std::fs::write(&dest, &min).unwrap();
        }
        let _ = writeln!(code, "    ({id:?}, include_str!({:?})),", dest.to_string_lossy());
        let _ = writeln!(requires, "    &{reqs:?},");
        println!("cargo:rerun-if-changed={}", p.display());
    }
    code.push_str("];\n");
    code.push_str(&requires);
    code.push_str("];\n");
    println!("cargo:rerun-if-changed=src/npm/lex.rs");
    std::fs::write(PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("node_shims.rs"), code).unwrap();
    println!("cargo:rerun-if-changed={}", dir.display());
}

/// The runtime (runtime/tov.c and its parts, one translation unit) compiled as `tov build`
/// compiles it with the default flags and this machine's compiler: (its cache key, the object).
fn prebuilt_runtime(root: &Path, out_dir: &Path) -> Option<(String, PathBuf)> {
    let read = |f: &str| std::fs::read_to_string(root.join("runtime").join(f)).ok();
    let h = read("tov.h")?;
    let parts = [read("tov.c")?, read("big.c")?, read("dec.c")?, read("net.c")?, read("ws.c")?];
    for f in ["tov.h", "tov.c", "big.c", "dec.c", "net.c", "ws.c"] {
        println!("cargo:rerun-if-changed={}", root.join("runtime").join(f).display());
    }
    if std::env::var_os("CC").is_some() {
        return None;
    }
    let (cc, sysroot) = if cfg!(target_vendor = "apple") {
        match rt_flags::xcrun_toolchain() {
            Some((c, s)) => (c, Some(s)),
            None => (rt_flags::other_compiler(), None),
        }
    } else {
        (rt_flags::other_compiler(), None)
    };
    let flags = rt_flags::base_flags("-O3", false, false, false, sysroot.as_deref());
    let flag_text = flags.join(" ");
    let version = std::env::var("CARGO_PKG_VERSION").unwrap_or_default();
    let key = tls_flags::hash_hex(&[h.as_bytes(), parts[0].as_bytes(), parts[1].as_bytes(), parts[2].as_bytes(), parts[3].as_bytes(), parts[4].as_bytes(), cc.as_bytes(), flag_text.as_bytes(), version.as_bytes()]);
    let obj = out_dir.join(format!("rt-{key}.o"));
    if obj.is_file() {
        return Some((key, obj));
    }
    let src = out_dir.join(format!("rt-{key}.c"));
    let refs: Vec<&str> = parts.iter().map(|p| p.as_str()).collect();
    std::fs::write(&src, rt_flags::runtime_source(&h, &refs)).ok()?;
    let tmp = out_dir.join(format!("rt-{key}.tmp.o"));
    let ok = Command::new(&cc).args(&flags).args(["-ffunction-sections", "-fdata-sections", "-c", "-o"]).arg(&tmp).arg(&src).status().map(|s| s.success()).unwrap_or(false);
    if !ok {
        println!("cargo:warning=couldn't precompile the runtime; `tov build` will compile it on first use");
        return None;
    }
    std::fs::rename(&tmp, &obj).ok()?;
    Some((key, obj))
}
