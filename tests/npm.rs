//! npm packages: each tests/npm/<name>.tov imports packages from tests/npm/node_modules (small
//! CommonJS, ES module and TypeScript packages) and runs; stdout must equal <name>.stdout.
//! Tov runs JavaScript on JavaScriptCore: the system's on macOS, WebKitGTK's elsewhere
//! (scripts/linux.sh runs these in Linux).

use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap();
    let dir = root.join("tests/npm");
    // the Node-API fixture: a C addon, compiled here (binaries aren't checked in)
    let addon = dir.join("node_modules/napi-addon");
    let built = addon.join("build/addon.node");
    // (built for this system: a Mach-O or an ELF file, as the checkout may be shared with Linux)
    let elf = std::fs::read(&built).map(|b| b.starts_with(b"\x7fELF")).unwrap_or(false);
    let other = elf == cfg!(target_vendor = "apple");
    let stale = other || std::fs::metadata(&built).and_then(|b| Ok(b.modified()? < std::fs::metadata(addon.join("addon.c"))?.modified()?)).unwrap_or(true);
    if stale {
        std::fs::create_dir_all(addon.join("build")).unwrap();
        let link: &[&str] = if cfg!(target_vendor = "apple") { &["-bundle", "-undefined", "dynamic_lookup"] } else { &["-shared", "-fPIC"] };
        let ok = Command::new("cc").arg("-O1").args(link).arg("-o").arg(&built).arg(addon.join("addon.c")).status().map(|s| s.success()).unwrap_or(false);
        assert!(ok, "can't compile tests/npm/node_modules/napi-addon/addon.c");
    }
    let mut cases: Vec<PathBuf> = std::fs::read_dir(&dir).unwrap().flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|e| e == "tov")).collect();
    cases.sort();
    let mut failed = Vec::new();
    for case in &cases {
        let name = case.file_stem().unwrap().to_string_lossy().into_owned();
        let opts = tov::build::Options { mode: tov::codegen::Mode::Run, unchecked: false, opt: "-O1".into(), emit_c: None, symbols: false };
        let built = match tov::build::build(std::slice::from_ref(case), &root, &opts) {
            Ok(b) => b,
            Err(tov::build::BuildError::Diagnostics(sm, d)) => {
                failed.push(format!("{name}: build failed\n{}", tov::diag::render_text(&d, &sm)));
                continue;
            }
            Err(tov::build::BuildError::Message(m)) => {
                failed.push(format!("{name}: {m}"));
                continue;
            }
        };
        // (<name>.linux.stdout, when a program's output differs there: process.platform, say)
        let os_expected = case.with_extension(format!("{}.stdout", std::env::consts::OS));
        let expected = std::fs::read_to_string(&os_expected).or_else(|_| std::fs::read_to_string(case.with_extension("stdout"))).unwrap_or_default();
        // `cache`: runs twice, with a cache of its own: compiled (writing the bytecode cache as it
        // exits), then from the cache (on macOS, or with Tov's own engine: TOV_JSC_DIR)
        let cache = (name == "cache" && (cfg!(target_vendor = "apple") || std::env::var_os("TOV_JSC_DIR").is_some())).then(|| std::env::temp_dir().join(format!("tov-npm-cache-{}", std::process::id())));
        let runs = if cache.is_some() { 2 } else { 1 };
        for run in 0..runs {
            let mut cmd = Command::new(&built.binary);
            if let Some(dir) = &cache {
                if run == 0 {
                    let _ = std::fs::remove_dir_all(dir);
                    std::fs::create_dir_all(dir).unwrap();
                } else {
                    wait_for_cache(dir);
                    cmd.env("TOV_JS_TRACE", "1");
                }
                cmd.env("TOV_JS_CACHE_DIR", dir);
            }
            let out = cmd.output().expect("run binary");
            let actual = String::from_utf8_lossy(&out.stdout).into_owned();
            let stderr = String::from_utf8_lossy(&out.stderr);
            if actual != expected || !out.status.success() {
                failed.push(format!("{name} (run {}): output differs (exit {:?})\n--- expected\n{expected}--- actual\n{actual}{stderr}", run + 1, out.status.code()));
            } else if run == 1 && !stderr.contains("node_modules/cache-lib/index.js") || run == 1 && !stderr.lines().any(|l| l.starts_with("tov: loaded") && l.contains("cache-lib")) {
                failed.push(format!("{name}: the second run didn't load cache-lib from the bytecode cache\n{stderr}"));
            }
        }
        if let Some(dir) = &cache {
            let _ = std::fs::remove_dir_all(dir);
        }
        if name == "napi" {
            failed.extend(installed_addon(&built, &expected).err());
        }
    }
    for f in &failed {
        eprintln!("{f}\n");
    }
    println!("npm: {} programs, {} failed", cases.len(), failed.len());
    if !failed.is_empty() {
        std::process::exit(1);
    }
}

/// A program installed elsewhere loads its addon from beside it, never from its build path: it
/// runs with the copy `install` put there, and fails once that copy is gone.
fn installed_addon(built: &tov::build::Built, expected: &str) -> Result<(), String> {
    let dir = std::env::temp_dir().join(format!("tov-npm-install-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    if tov::build::install(built, &dir.join("addon.node")).is_ok() {
        return Err("napi: installed as addon.node, it replaced its own addon".into());
    }
    let dest = dir.join("napi");
    tov::build::install(built, &dest).map_err(|e| format!("napi: can't install: {e}"))?;
    let addon = dir.join("addon.node");
    let first = Command::new(&dest).output().expect("run the installed program");
    std::fs::remove_file(&addon).map_err(|e| format!("napi: the addon wasn't installed beside the program: {e}"))?;
    let second = Command::new(&dest).output().expect("run the installed program");
    let _ = std::fs::remove_dir_all(&dir);
    let actual = String::from_utf8_lossy(&first.stdout);
    if actual != expected || !first.status.success() {
        return Err(format!("napi installed: output differs (exit {:?})\n{actual}{}", first.status.code(), String::from_utf8_lossy(&first.stderr)));
    }
    let stderr = String::from_utf8_lossy(&second.stderr);
    if second.status.success() || !stderr.contains(&*addon.to_string_lossy()) {
        return Err(format!("napi installed: without its addon beside it, it should fail naming {} (exit {:?})\n{stderr}", addon.display(), second.status.code()));
    }
    Ok(())
}

/// Waits for the process a program starts as it exits to finish writing its bytecode cache (the
/// directory's files stop changing, none of them temporary).
fn wait_for_cache(dir: &Path) {
    let mut last = usize::MAX;
    for _ in 0..100 {
        std::thread::sleep(std::time::Duration::from_millis(100));
        let names: Vec<String> = std::fs::read_dir(dir).map(|d| d.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect()).unwrap_or_default();
        let done = names.iter().any(|n| n.ends_with(".jsc")) && !names.iter().any(|n| n.ends_with(".tmp"));
        if done && names.len() == last {
            return;
        }
        last = names.len();
    }
}
