//! Async programs: each tests/async/<name>.tov is compiled and run; stdout must equal
//! tests/async/<name>.stdout, which is the same program's output under Node (scripts/tov2js.sh).
//! When `node` is installed the expectation is re-checked against it, so an expectation can't
//! drift from what JavaScript does. Timer deadlines in these programs are at least 10 ms apart,
//! so Node's output doesn't depend on scheduling jitter.

use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap();
    let dir = root.join("tests/async");
    let mut cases: Vec<PathBuf> = std::fs::read_dir(&dir).unwrap().flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|e| e == "tov")).collect();
    cases.sort();
    let node = Command::new("node").arg("--version").output().is_ok_and(|o| o.status.success());
    let mut failed = Vec::new();
    for case in &cases {
        let name = case.file_stem().unwrap().to_string_lossy().into_owned();
        let expected = std::fs::read_to_string(case.with_extension("stdout")).unwrap_or_default();
        if node {
            let out = Command::new("sh").arg(root.join("scripts/tov2js.sh")).arg(case).output().expect("run node");
            let js = String::from_utf8_lossy(&out.stdout);
            if js != expected {
                failed.push(format!("{name}: the expectation differs from Node's output\n--- expected\n{expected}--- node\n{js}{}", String::from_utf8_lossy(&out.stderr)));
                continue;
            }
        }
        let opts = tov::build::Options { mode: tov::codegen::Mode::Run, unchecked: false, opt: "-O1".into(), emit_c: None, symbols: false, install: false };
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
        let out = Command::new(&built.binary).output().expect("run binary");
        let actual = String::from_utf8_lossy(&out.stdout).into_owned();
        if actual != expected || !out.status.success() {
            failed.push(format!("{name}: output differs (exit {:?})\n--- expected\n{expected}--- actual\n{actual}{}", out.status.code(), String::from_utf8_lossy(&out.stderr)));
        }
    }
    for f in &failed {
        eprintln!("{f}\n");
    }
    println!("async: {} programs{}, {} failed", cases.len(), if node { " (checked against Node)" } else { "" }, failed.len());
    if !failed.is_empty() {
        std::process::exit(1);
    }
}
