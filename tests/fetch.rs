//! fetch(): each tests/fetch/<name>.tov runs against a scripted HTTP/1.1 server (below) that covers
//! the protocol's corners: chunked and close-delimited bodies, gzip/deflate, interim 1xx
//! responses, redirects of every kind, keep-alive reuse, servers that drop connections, slow
//! responses (aborts and timeouts), malformed responses. Its output must equal
//! tests/fetch/<name>.stdout, which is the same program's output under Bun; when `bun` is
//! installed the expectation is re-checked against it (except for native_* programs, which cover
//! what Tov does differently or Bun can't run).

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

struct Req {
    method: String,
    path: String,
    query: String,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
}

impl Req {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }
    fn param(&self, name: &str) -> Option<String> {
        self.query.split('&').find_map(|kv| kv.strip_prefix(name).and_then(|r| r.strip_prefix('=')).map(|v| v.to_string()))
    }
}

fn read_req(s: &mut impl Read, buf: &mut Vec<u8>) -> Option<Req> {
    loop {
        if let Some(end) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            let head = String::from_utf8_lossy(&buf[..end]).into_owned();
            let mut lines = head.split("\r\n");
            let first = lines.next()?;
            let mut parts = first.split(' ');
            let method = parts.next()?.to_string();
            let target = parts.next()?.to_string();
            let (path, query) = match target.split_once('?') {
                Some((p, q)) => (p.to_string(), q.to_string()),
                None => (target.clone(), String::new()),
            };
            let headers: Vec<(String, String)> = lines.filter_map(|l| l.split_once(':').map(|(k, v)| (k.trim().to_string(), v.trim().to_string()))).collect();
            let len: usize = headers.iter().find(|(k, _)| k.eq_ignore_ascii_case("content-length")).and_then(|(_, v)| v.parse().ok()).unwrap_or(0);
            while buf.len() < end + 4 + len {
                let mut chunk = [0u8; 65536];
                let n = s.read(&mut chunk).ok()?;
                if n == 0 {
                    return None;
                }
                buf.extend_from_slice(&chunk[..n]);
            }
            let body = buf[end + 4..end + 4 + len].to_vec();
            buf.drain(..end + 4 + len);
            return Some(Req { method, path, query, headers, body });
        }
        let mut chunk = [0u8; 65536];
        let n = s.read(&mut chunk).ok()?;
        if n == 0 {
            return None;
        }
        buf.extend_from_slice(&chunk[..n]);
    }
}

/// A deflate stored (uncompressed) block.
fn stored_block(data: &[u8], last: bool) -> Vec<u8> {
    let n = data.len() as u16;
    let mut v = vec![last as u8];
    v.extend(n.to_le_bytes());
    v.extend((!n).to_le_bytes());
    v.extend_from_slice(data);
    v
}

fn crc32(data: &[u8]) -> u32 {
    let mut c = !0u32;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0xEDB8_8320 } else { c >> 1 };
        }
    }
    !c
}

fn respond(s: &mut impl Write, status: &str, headers: &[(&str, String)], body: &[u8]) {
    let mut out = format!("HTTP/1.1 {status}\r\nContent-Length: {}\r\n", body.len()).into_bytes();
    for (k, v) in headers {
        out.extend_from_slice(format!("{k}: {v}\r\n").as_bytes());
    }
    out.extend_from_slice(b"\r\n");
    out.extend_from_slice(body);
    let _ = s.write_all(&out);
}

/// Serves one connection; returns false when it should be closed.
fn handle(s: &mut (impl Read + Write), r: &Req, served: usize, fixtures: &Path, alt: &str) -> bool {
    let fixture = |name: &str| std::fs::read(fixtures.join(name)).unwrap();
    match r.path.as_str() {
        "/text" => respond(s, "200 OK", &[("Content-Type", "text/plain".into())], b"hello"),
        "/json" => respond(s, "200 OK", &[("Content-Type", "application/json".into())], br#"{"a":1,"b":[1,2],"c":"x"}"#),
        "/echo" => {
            let mut out = format!("{} {}", r.method, if r.query.is_empty() { String::new() } else { format!("?{}", r.query) });
            for h in ["content-type", "content-length", "authorization", "cookie", "x-custom", "x-multi"] {
                if let Some(v) = r.header(h) {
                    out.push_str(&format!("|{h}={v}"));
                }
            }
            out.push_str(&format!("|body={}", String::from_utf8_lossy(&r.body)));
            respond(s, "200 OK", &[], out.as_bytes());
        }
        // the body back, under the request's Content-Type (for form data: its boundary is random)
        "/mirror" => respond(s, "200 OK", &[("Content-Type", r.header("content-type").unwrap_or_default().to_string())], &r.body),
        "/chunked" => {
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3;ext=1\r\nhel\r\n2\r\nlo\r\nA\r\n, chunked!\r\n0\r\nX-Trailer: 1\r\n\r\n");
        }
        "/chunked-big" => {
            let mut out = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n".to_vec();
            for i in 0..500 {
                let chunk = format!("{:0>1000}", i);
                out.extend_from_slice(format!("{:x}\r\n{chunk}\r\n", chunk.len()).as_bytes());
            }
            out.extend_from_slice(b"0\r\n\r\n");
            let _ = s.write_all(&out);
        }
        "/close" => {
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nread until the server closes");
            return false;
        }
        "/http10" => {
            let _ = s.write_all(b"HTTP/1.0 200 OK\r\n\r\nan HTTP/1.0 body");
            return false;
        }
        "/lf" => {
            let _ = s.write_all(b"HTTP/1.1 200 OK\ncontent-length: 7\nx-lf: yes\n\nlf only");
        }
        "/continue" => {
            let _ = s.write_all(b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\n");
            respond(s, "200 OK", &[], b"after interim responses");
        }
        "/status" => {
            let code = r.param("code").unwrap_or_else(|| "200".into());
            let reason = r.param("reason").unwrap_or_default().replace("%20", " ");
            respond(s, &format!("{code} {reason}"), &[], format!("status {code}").as_bytes());
        }
        "/nobody" => {
            let code = r.param("code").unwrap();
            let _ = s.write_all(format!("HTTP/1.1 {code} X\r\nContent-Length: 0\r\n\r\n").as_bytes());
        }
        "/head" => respond(s, "200 OK", &[("X-Head", "1".into())], b"body not sent for HEAD"),
        "/redirect" => {
            let code = r.param("code").unwrap_or_else(|| "302".into());
            let to = r.param("to").unwrap_or_else(|| "/echo".into()).replace("%3A", ":").replace("%2F", "/").replace("ALT", alt);
            respond(s, &format!("{code} Redirect"), &[("Location", to)], b"redirecting");
        }
        "/chain" => {
            let n: u32 = r.param("n").and_then(|v| v.parse().ok()).unwrap_or(0);
            if n == 0 {
                respond(s, "200 OK", &[], b"end of chain");
            } else {
                respond(s, "302 Found", &[("Location", format!("/chain?n={}", n - 1))], b"");
            }
        }
        "/loop" => respond(s, "302 Found", &[("Location", "/loop".into())], b""),
        "/headers" => {
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-A: 1\r\nx-a: 2\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\nX-Spaces:   padded value  \r\n\r\nok");
        }
        "/gzip" | "/deflate" | "/br" | "/zstd" => {
            let name = r.param("f").unwrap();
            let enc = &r.path[1..];
            respond(s, "200 OK", &[("Content-Encoding", enc.into())], &fixture(&name));
        }
        "/badgzip" => respond(s, "200 OK", &[("Content-Encoding", "gzip".into())], b"\x1f\x8b\x08\x00not really gzip at all"),
        "/badbr" => respond(s, "200 OK", &[("Content-Encoding", "br".into())], b"not brotli at all, really not"),
        "/badzstd" => respond(s, "200 OK", &[("Content-Encoding", "zstd".into())], b"\x28\xb5\x2f\xfd not zstd after the magic"),
        "/utf8" => respond(s, "200 OK", &[], b"a\xffb\xe2\x82 c\xf0\x9f\x98\x80d"),
        "/bom" => respond(s, "200 OK", &[], b"\xef\xbb\xbfbom"),
        "/big" => {
            let n: usize = r.param("n").and_then(|v| v.parse().ok()).unwrap_or(1000);
            let body: Vec<u8> = (0..n).map(|i| b'a' + (i % 26) as u8).collect();
            respond(s, "200 OK", &[], &body);
        }
        "/slow" => {
            let ms: u64 = r.param("ms").and_then(|v| v.parse().ok()).unwrap_or(1000);
            std::thread::sleep(Duration::from_millis(ms));
            respond(s, "200 OK", &[], b"slow");
        }
        "/conn" => respond(s, "200 OK", &[], format!("request {} on this connection", served + 1).as_bytes()),
        "/closing" => {
            respond(s, "200 OK", &[("Connection", "close".into())], b"closing after this");
            return false;
        }
        "/drop" => return false,
        "/stream" => {
            // server-sent events: the head at once, then events over ~200 ms
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n");
            for (i, ev) in ["data: one\n\n", "data: two\n\n", "data: three\n\n"].iter().enumerate() {
                std::thread::sleep(Duration::from_millis(if i == 0 { 0 } else { 100 }));
                let _ = s.write_all(format!("{:x}\r\n{ev}\r\n", ev.len()).as_bytes());
            }
            let _ = s.write_all(b"0\r\n\r\n");
        }
        "/stream-gzip" | "/stream-gzip-cut" => {
            // the same events gzipped (stored blocks), each sent as it's made; the cut one ends
            // (cleanly, for HTTP) before the gzip stream does
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n");
            let mut all = Vec::new();
            for (i, ev) in ["data: one\n\n", "data: two\n\n", "data: three\n\n"].iter().enumerate() {
                std::thread::sleep(Duration::from_millis(if i == 0 { 0 } else { 100 }));
                let mut part = if i == 0 { vec![0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff] } else { Vec::new() };
                part.extend(stored_block(ev.as_bytes(), false));
                all.extend_from_slice(ev.as_bytes());
                let _ = s.write_all(format!("{:x}\r\n", part.len()).as_bytes());
                let _ = s.write_all(&part);
                let _ = s.write_all(b"\r\n");
            }
            if r.path == "/stream-gzip" {
                let mut end = stored_block(&[], true);
                end.extend(crc32(&all).to_le_bytes());
                end.extend((all.len() as u32).to_le_bytes());
                let _ = s.write_all(format!("{:x}\r\n", end.len()).as_bytes());
                let _ = s.write_all(&end);
                let _ = s.write_all(b"\r\n");
            }
            let _ = s.write_all(b"0\r\n\r\n");
        }
        "/stream-cut" => {
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n");
            std::thread::sleep(Duration::from_millis(50));
            return false;
        }
        "/slowbody" => {
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n");
            std::thread::sleep(Duration::from_millis(2000));
            let _ = s.write_all(b"late");
        }
        "/malformed" => {
            let _ = s.write_all(b"this is not HTTP\r\n\r\n");
            return false;
        }
        "/short" => {
            let _ = s.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly part");
            return false;
        }
        _ => respond(s, "404 Not Found", &[], b"no such route"),
    }
    true
}

/// The HTTPS test servers (python3 tests/fetch/tls/server.py): ports for the good, expired and
/// self-signed certificates.
struct TlsServer {
    child: std::process::Child,
    ports: [u16; 3],
}

impl TlsServer {
    fn start(script: &Path) -> Option<TlsServer> {
        let mut child = Command::new("python3").arg(script).stdout(std::process::Stdio::piped()).stderr(std::process::Stdio::null()).spawn().ok()?;
        let mut line = String::new();
        std::io::BufRead::read_line(&mut std::io::BufReader::new(child.stdout.take()?), &mut line).ok()?;
        let ports: Vec<u16> = line.split_whitespace().skip(1).filter_map(|p| p.parse().ok()).collect();
        let ports: [u16; 3] = ports.try_into().ok()?;
        Some(TlsServer { child, ports })
    }
}

impl Drop for TlsServer {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// An HTTP proxy: absolute-form requests (`GET http://host/path`) are answered here with what
/// the proxy saw; CONNECT opens a tunnel to the target. With `auth`, requests without
/// `Proxy-Authorization: Basic dXNlcjpwYXNz` (user:pass) get 407.
fn serve_proxy(listener: TcpListener, auth: bool) {
    for conn in listener.incoming() {
        let Ok(mut s) = conn else { continue };
        std::thread::spawn(move || {
            let mut buf = Vec::new();
            while let Some(r) = read_req(&mut s, &mut buf) {
                let authorized = r.header("proxy-authorization") == Some("Basic dXNlcjpwYXNz");
                if auth && !authorized {
                    respond(&mut s, "407 Proxy Authentication Required", &[("Proxy-Authenticate", "Basic realm=\"test\"".into())], b"proxy says no");
                    continue;
                }
                if r.method == "CONNECT" {
                    let Ok(up) = TcpStream::connect(r.path.as_str()) else {
                        respond(&mut s, "502 Bad Gateway", &[], b"no route");
                        return;
                    };
                    let _ = s.write_all(b"HTTP/1.1 200 Connection Established\r\n\r\n");
                    let (mut a, mut b) = (s.try_clone().unwrap(), up.try_clone().unwrap());
                    let mut up2 = up;
                    std::thread::spawn(move || {
                        let _ = std::io::copy(&mut b, &mut a);
                        let _ = a.shutdown(std::net::Shutdown::Write);
                    });
                    let _ = std::io::copy(&mut s, &mut up2);
                    let _ = up2.shutdown(std::net::Shutdown::Write);
                    return;
                }
                let target = if r.query.is_empty() { r.path.clone() } else { format!("{}?{}", r.path, r.query) };
                let body = format!("proxied {} {} host={} auth={}", r.method, target, r.header("host").unwrap_or(""), r.header("proxy-authorization").is_some());
                respond(&mut s, "200 OK", &[], body.as_bytes());
            }
        });
    }
}

/// The same routes on a Unix socket (Bun's `unix` option).
fn serve_unix(listener: std::os::unix::net::UnixListener, fixtures: PathBuf) {
    for conn in listener.incoming() {
        let Ok(mut s) = conn else { continue };
        let fixtures = fixtures.clone();
        std::thread::spawn(move || {
            let mut buf = Vec::new();
            let mut served = 0;
            while let Some(r) = read_req(&mut s, &mut buf) {
                if !handle(&mut s, &r, served, &fixtures, "") {
                    break;
                }
                served += 1;
            }
        });
    }
}

fn serve(listener: TcpListener, fixtures: PathBuf, alt: String, stop: Arc<AtomicBool>) {
    for conn in listener.incoming() {
        if stop.load(Ordering::Relaxed) {
            break;
        }
        let Ok(mut s) = conn else { continue };
        let fixtures = fixtures.clone();
        let alt = alt.clone();
        std::thread::spawn(move || {
            let mut buf = Vec::new();
            let mut served = 0;
            while let Some(r) = read_req(&mut s, &mut buf) {
                if !handle(&mut s, &r, served, &fixtures, &alt) {
                    break;
                }
                served += 1;
            }
        });
    }
}

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap();
    let dir = root.join("tests/fetch");
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let base = format!("http://127.0.0.1:{port}");
    let alt = format!("http://localhost:{port}");
    let stop = Arc::new(AtomicBool::new(false));
    // TOV_FETCH_SERVE=<alt origin>: only serve (for running the clients elsewhere, e.g. under
    // sanitizers in a container); prints the port.
    if let Ok(alt) = std::env::var("TOV_FETCH_SERVE") {
        let alt = alt.replace("PORT", &port.to_string());
        let proxy = TcpListener::bind("127.0.0.1:0").unwrap();
        let proxy_auth = TcpListener::bind("127.0.0.1:0").unwrap();
        println!("{port} {} {}", proxy.local_addr().unwrap().port(), proxy_auth.local_addr().unwrap().port());
        std::thread::spawn(move || serve_proxy(proxy, false));
        std::thread::spawn(move || serve_proxy(proxy_auth, true));
        serve(listener, dir, alt, stop);
        return;
    }
    {
        let (dir, alt, stop) = (dir.clone(), alt.clone(), stop.clone());
        std::thread::spawn(move || serve(listener, dir, alt, stop));
    }
    let proxy = TcpListener::bind("127.0.0.1:0").unwrap();
    let proxy_port = proxy.local_addr().unwrap().port();
    std::thread::spawn(move || serve_proxy(proxy, false));
    let proxy_auth = TcpListener::bind("127.0.0.1:0").unwrap();
    let proxy_auth_port = proxy_auth.local_addr().unwrap().port();
    std::thread::spawn(move || serve_proxy(proxy_auth, true));
    let unix_path = std::env::temp_dir().join(format!("tov-fetch-{}.sock", std::process::id()));
    {
        let listener = std::os::unix::net::UnixListener::bind(&unix_path).expect("bind a unix socket");
        let dir = dir.clone();
        std::thread::spawn(move || serve_unix(listener, dir));
    }
    // Every tests/fetch/<name>.tov; the ones not named native_* are also checked against Bun.
    let mut cases: Vec<PathBuf> = std::fs::read_dir(&dir).unwrap().flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|e| e == "tov")).collect();
    cases.sort();
    let mut failed = Vec::new();
    let bun_version = Command::new("bun").arg("--version").output().ok().filter(|o| o.status.success()).map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string());
    let bun = bun_version.is_some();
    // (the expectations are Bun 1.4.0's output: another version's differences are reported, not failed)
    let bun_strict = bun_version.as_deref() == Some("1.4.0");
    let mut bun_notes = Vec::new();
    // HTTPS: tests/fetch/tls/server.py (tls.tov and proxy.tov need it; skipped without python3)
    let tls = TlsServer::start(&dir.join("tls/server.py"));
    let mut skipped = 0;
    for case in &cases {
        let name = case.file_stem().unwrap().to_string_lossy().into_owned();
        let mut env: Vec<(&str, String)> = vec![
            ("BASE", base.clone()),
            ("ALT", alt.clone()),
            ("UNIX", unix_path.to_string_lossy().into_owned()),
            ("PROXY", format!("http://127.0.0.1:{proxy_port}")),
            ("PROXY_AUTH", format!("127.0.0.1:{proxy_auth_port}")),
        ];
        match &tls {
            Some(t) => env.extend([("TLS_GOOD", t.ports[0].to_string()), ("TLS_EXPIRED", t.ports[1].to_string()), ("TLS_SELF", t.ports[2].to_string())]),
            None if name == "tls" || name == "proxy" => {
                skipped += 1;
                continue;
            }
            None => {}
        }
        if name == "proxy_env" {
            env.extend([("HTTP_PROXY", format!("http://127.0.0.1:{proxy_port}")), ("NO_PROXY", "localhost,127.0.0.1".to_string())]);
        }
        let expected = std::fs::read_to_string(case.with_extension("stdout")).unwrap_or_default();
        if bun && !name.starts_with("native_") {
            let out = Command::new("sh").arg(root.join("scripts/tov2js.sh")).arg(case).current_dir(&root).env("TOV2JS_RUNTIME", "bun").envs(env.iter().map(|(k, v)| (*k, v.as_str()))).output().expect("run bun");
            let js = String::from_utf8_lossy(&out.stdout);
            if js != expected {
                let msg = format!("{name}: the expectation differs from Bun's output\n--- expected\n{expected}--- bun\n{js}{}", String::from_utf8_lossy(&out.stderr));
                if bun_strict { failed.push(msg) } else { bun_notes.push(name.clone()) }
            }
        }
        let opts = tov::build::Options { mode: tov::codegen::Mode::Run, unchecked: false, opt: "-O1".into(), emit_c: None, symbols: false, install: false };
        match tov::build::build(std::slice::from_ref(case), &root, &opts) {
            Ok(built) => {
                if !built.tls {
                    failed.push(format!("{name}: fetches, but doesn't link TLS"));
                }
                let started = std::time::Instant::now();
                let out = Command::new(&built.binary).current_dir(&root).envs(env.iter().map(|(k, v)| (*k, v.as_str()))).output().expect("run binary");
                let actual = String::from_utf8_lossy(&out.stdout).into_owned();
                // nothing here waits long: a slow exit means something kept the loop alive
                if started.elapsed() > Duration::from_secs(20) {
                    failed.push(format!("{name}: took {:?}", started.elapsed()));
                }
                if actual != expected || !out.status.success() {
                    failed.push(format!("{name}: output differs (exit {:?})\n--- expected\n{expected}--- actual\n{actual}{}", out.status.code(), String::from_utf8_lossy(&out.stderr)));
                }
            }
            Err(tov::build::BuildError::Diagnostics(sm, d)) => failed.push(format!("{name}: build failed\n{}", tov::diag::render_text(&d, &sm))),
            Err(tov::build::BuildError::Message(m)) => failed.push(format!("{name}: {m}")),
        }
    }
    // Only programs that fetch link TLS.
    let opts = tov::build::Options { mode: tov::codegen::Mode::Run, unchecked: false, opt: "-O1".into(), emit_c: None, symbols: false, install: false };
    let hello = std::env::temp_dir().join(format!("tov-no-tls-{}.tov", std::process::id()));
    std::fs::write(&hello, "console.log(\"hi\")\n").unwrap();
    match tov::build::build(std::slice::from_ref(&hello), &root, &opts) {
        Ok(b) if b.tls => failed.push("a program that doesn't fetch links TLS".into()),
        Ok(_) => {}
        Err(_) => failed.push("hello-world didn't build".into()),
    }
    let _ = std::fs::remove_file(&hello);
    let _ = std::fs::remove_file(&unix_path);
    stop.store(true, Ordering::Relaxed);
    let _ = TcpStream::connect(("127.0.0.1", port));
    for f in &failed {
        eprintln!("{f}\n");
    }
    drop(tls);
    if !bun_notes.is_empty() {
        println!("fetch: Bun {} prints differently from the recorded Bun 1.4.0 for {} (not a Tov failure)", bun_version.as_deref().unwrap_or(""), bun_notes.join(", "));
    }
    let skip_note = if skipped > 0 { format!(", {skipped} skipped (no python3 for the HTTPS server)") } else { String::new() };
    println!("fetch: {} programs{}{skip_note}, {} failed", cases.len() - skipped, if bun { " (checked against Bun)" } else { "" }, failed.len());
    if !failed.is_empty() {
        std::process::exit(1);
    }
}
