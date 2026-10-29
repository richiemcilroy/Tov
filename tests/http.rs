//! std/http protocol tests: builds tests/http/app.tov, starts it on a free port, and checks raw
//! HTTP/1.1 conversations (keep-alive, pipelining, chunked bodies, 100-continue, HEAD, errors,
//! header sanitizing, large responses, several workers).

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::Path;
use std::process::{Child, Command};
use std::time::{Duration, Instant};

struct Server {
    child: Child,
    port: u16,
}

impl Drop for Server {
    fn drop(&mut self) {
        // Workers are forked children in the same process group.
        kill_group(self.child.id());
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn kill_group(pid: u32) {
    // (`--`: procps's kill, Linux's, reads a negative pid after the signal as an option)
    let _ = Command::new("kill").args(["-TERM", "--"]).arg(format!("-{pid}")).stderr(std::process::Stdio::null()).status();
}

fn start(binary: &Path, workers: u32) -> Server {
    start_with(binary, workers, std::process::Stdio::inherit())
}

fn start_with(binary: &Path, workers: u32, stdout: std::process::Stdio) -> Server {
    let port = TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port();
    let mut cmd = Command::new(binary);
    cmd.env("PORT", port.to_string()).env("WORKERS", workers.to_string()).stdout(stdout).stderr(std::process::Stdio::null());
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        cmd.process_group(0);
    }
    let child = cmd.spawn().expect("start server");
    let end = Instant::now() + Duration::from_secs(5);
    while TcpStream::connect(("127.0.0.1", port)).is_err() {
        assert!(Instant::now() < end, "server did not start");
        std::thread::sleep(Duration::from_millis(20));
    }
    Server { child, port }
}

fn connect(s: &Server) -> TcpStream {
    let c = TcpStream::connect(("127.0.0.1", s.port)).unwrap();
    c.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    c
}

#[derive(Debug)]
struct Resp {
    status: u32,
    head: String,
    body: Vec<u8>,
}

impl Resp {
    fn header(&self, name: &str) -> Option<String> {
        self.head.lines().skip(1).find_map(|l| {
            let (k, v) = l.split_once(':')?;
            k.eq_ignore_ascii_case(name).then(|| v.trim().to_string())
        })
    }
    fn headers(&self, name: &str) -> Vec<String> {
        self.head.lines().skip(1).filter_map(|l| l.split_once(':')).filter(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.trim().to_string()).collect()
    }
    fn text(&self) -> String {
        String::from_utf8_lossy(&self.body).into_owned()
    }
}

/// Reads one response (Content-Length framed; no body for HEAD/204/304/1xx).
fn read_resp(c: &mut TcpStream, buf: &mut Vec<u8>, head_only: bool) -> Resp {
    loop {
        if let Some(i) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            let head = String::from_utf8_lossy(&buf[..i]).into_owned();
            let status: u32 = head[9..12].parse().unwrap();
            let len: usize = head
                .lines()
                .find_map(|l| l.split_once(':').filter(|(k, _)| k.eq_ignore_ascii_case("content-length")).map(|(_, v)| v.trim().parse().unwrap()))
                .unwrap_or(0);
            let len = if head_only || status < 200 || status == 204 || status == 304 { 0 } else { len };
            while buf.len() < i + 4 + len {
                fill(c, buf);
            }
            let body = buf[i + 4..i + 4 + len].to_vec();
            buf.drain(..i + 4 + len);
            return Resp { status, head, body };
        }
        fill(c, buf);
    }
}

fn fill(c: &mut TcpStream, buf: &mut Vec<u8>) {
    let mut tmp = [0u8; 65536];
    let n = c.read(&mut tmp).expect("read");
    assert!(n > 0, "connection closed early; have {:?}", String::from_utf8_lossy(buf));
    buf.extend_from_slice(&tmp[..n]);
}

fn closed(c: &mut TcpStream) -> bool {
    let mut tmp = [0u8; 16];
    matches!(c.read(&mut tmp), Ok(0) | Err(_))
}

fn roundtrip(s: &Server, req: &str) -> Resp {
    let mut c = connect(s);
    c.write_all(req.as_bytes()).unwrap();
    read_resp(&mut c, &mut Vec::new(), req.starts_with("HEAD"))
}

fn check(failed: &mut Vec<String>, name: &str, ok: bool, detail: impl std::fmt::Debug) {
    if !ok {
        failed.push(format!("{name}: {detail:?}"));
    }
}

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap();
    let app = root.join("tests/http/app.tov");
    let opts = tov::build::Options { mode: tov::codegen::Mode::Run, unchecked: false, opt: "-O2".into(), emit_c: None, symbols: false, install: false };
    let built = match tov::build::build(std::slice::from_ref(&app), &root, &opts) {
        Ok(b) => b,
        Err(tov::build::BuildError::Diagnostics(sm, d)) => panic!("build failed\n{}", tov::diag::render_text(&d, &sm)),
        Err(tov::build::BuildError::Message(m)) => panic!("{m}"),
    };
    let mut failed = Vec::new();
    let f = &mut failed;
    let s = start(&built.binary, 1);

    // basics and defaults
    let r = roundtrip(&s, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    check(f, "get", r.status == 200 && r.text() == "hello", &r);
    check(f, "default content-type", r.header("content-type").as_deref() == Some("text/plain;charset=utf-8"), &r);
    check(f, "date header", r.header("date").is_some_and(|d| d.ends_with("GMT")), &r);
    check(f, "keep-alive by default", r.header("connection").is_none(), &r);
    let r = roundtrip(&s, "GET /nope HTTP/1.1\r\n\r\n");
    check(f, "404", r.status == 404 && r.head.starts_with("HTTP/1.1 404 Not Found"), &r);
    let r = roundtrip(&s, "GET /json HTTP/1.1\r\n\r\n");
    check(f, "json", r.status == 201 && r.header("content-type").as_deref() == Some("application/json;charset=utf-8") && r.text() == r#"[{"id":1,"name":"a"},{"id":2,"name":"b"}]"#, &r);
    let r = roundtrip(&s, "GET /info?a=1&b=2 HTTP/1.1\r\nHost: example.test:8080\r\nx-name:   Ada  \r\nUser-Agent: t/1\r\n\r\n");
    check(f, "request fields", r.text() == "GET /info ?a=1&b=2 Ada t/1 http://example.test:8080/info?a=1&b=2", r.text());
    let r = roundtrip(&s, "GET /empty HTTP/1.1\r\n\r\n");
    check(f, "204 has no length or type", r.status == 204 && r.header("content-length").is_none() && r.header("content-type").is_none(), &r);

    // keep-alive and pipelining on one connection
    let mut c = connect(&s);
    let mut buf = Vec::new();
    c.write_all(b"GET / HTTP/1.1\r\n\r\n").unwrap();
    let a = read_resp(&mut c, &mut buf, false);
    c.write_all(b"GET /nope HTTP/1.1\r\n\r\n").unwrap();
    let b = read_resp(&mut c, &mut buf, false);
    check(f, "keep-alive", a.status == 200 && b.status == 404, (&a, &b));
    c.write_all(b"GET / HTTP/1.1\r\n\r\nPOST /echo HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcGET /nope HTTP/1.1\r\n\r\n").unwrap();
    let rs: Vec<Resp> = (0..3).map(|_| read_resp(&mut c, &mut buf, false)).collect();
    check(f, "pipelined order", rs[0].text() == "hello" && rs[1].text() == "abc" && rs[2].status == 404, &rs);

    // bodies: split across writes, chunked, 100-continue
    let mut c = connect(&s);
    let mut buf = Vec::new();
    c.write_all(b"POST /echo HTTP/1.1\r\nContent-Length: 10\r\n\r\nhello").unwrap();
    std::thread::sleep(Duration::from_millis(30));
    c.write_all(b"world").unwrap();
    let r = read_resp(&mut c, &mut buf, false);
    check(f, "split body", r.text() == "helloworld" && r.header("content-type").as_deref() == Some("application/octet-stream"), &r);
    c.write_all(b"POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n1;ext=1\r\n,\r\n").unwrap();
    std::thread::sleep(Duration::from_millis(30));
    c.write_all(b"6\r\n world\r\n0\r\nx-trailer: 1\r\n\r\n").unwrap();
    let r = read_resp(&mut c, &mut buf, false);
    check(f, "chunked body", r.text() == "hello, world", &r);
    c.write_all(b"POST /echo HTTP/1.1\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n").unwrap();
    let r = read_resp(&mut c, &mut buf, false);
    check(f, "100 continue", r.status == 100, &r);
    c.write_all(b"data").unwrap();
    let r = read_resp(&mut c, &mut buf, false);
    check(f, "after continue", r.status == 200 && r.text() == "data", &r);

    // HEAD, HTTP/1.0, Connection: close
    let r = roundtrip(&s, "HEAD / HTTP/1.1\r\n\r\n");
    check(f, "head", r.status == 200 && r.header("content-length").as_deref() == Some("5") && r.body.is_empty(), &r);
    let mut c = connect(&s);
    c.write_all(b"GET / HTTP/1.0\r\n\r\n").unwrap();
    let r = read_resp(&mut c, &mut Vec::new(), false);
    check(f, "http/1.0 closes", r.header("connection").as_deref() == Some("close") && closed(&mut c), &r);
    let mut c = connect(&s);
    c.write_all(b"GET / HTTP/1.1\r\nConnection: close\r\n\r\nGET / HTTP/1.1\r\n\r\n").unwrap();
    let mut buf = Vec::new();
    let r = read_resp(&mut c, &mut buf, false);
    check(f, "connection: close", r.text() == "hello" && buf.is_empty() && closed(&mut c), (&r, &buf));

    // malformed and oversized requests
    for (name, req, status) in [
        ("bad request line", "GARBAGE\r\n\r\n".to_string(), 400),
        ("bad version", "GET / HTTP/2.0\r\n\r\n".to_string(), 400),
        ("cl + te", "POST /echo HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n".to_string(), 400),
        ("conflicting lengths", "POST /echo HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd".to_string(), 400),
        ("bad length", "POST /echo HTTP/1.1\r\nContent-Length: 3x\r\n\r\nabc".to_string(), 400),
        ("unknown coding", "POST /echo HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n".to_string(), 400),
        ("bad chunk", "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n".to_string(), 400),
        ("huge body", "POST /echo HTTP/1.1\r\nContent-Length: 999999999999\r\n\r\n".to_string(), 413),
        ("huge headers", format!("GET / HTTP/1.1\r\nx-big: {}\r\n\r\n", "a".repeat(70_000)), 431),
    ] {
        let mut c = connect(&s);
        let _ = c.write_all(req.as_bytes());
        let r = read_resp(&mut c, &mut Vec::new(), false);
        check(f, name, r.status == status && closed(&mut c), &r);
    }

    // user headers can't inject lines; set replaces, append adds
    let r = roundtrip(&s, "GET /inject?a%0D%0Aset-cookie:%20evil=1 HTTP/1.1\r\n\r\n");
    check(f, "no header injection", r.header("set-cookie").is_none() && r.header("x-value").as_deref() == Some("aset-cookie:%20evil=1"), &r);
    check(f, "set replaces", r.headers("x-other") == ["2"], &r);
    check(f, "append adds", r.headers("x-multi") == ["a", "b"], &r);

    // a response larger than the socket buffer, read slowly
    let mut c = connect(&s);
    c.write_all(b"GET /big HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n").unwrap();
    std::thread::sleep(Duration::from_millis(100));
    let mut buf = Vec::new();
    let r = read_resp(&mut c, &mut buf, false);
    let r2 = read_resp(&mut c, &mut buf, false);
    check(f, "big response", r.body.len() == 4 * 1024 * 1024 && r.body.iter().all(|&b| b == b'x') && r2.text() == "hello", r.body.len());

    // many pipelined requests from a client that doesn't read until the end (backpressure)
    let mut c = connect(&s);
    let reqs = "GET /big HTTP/1.1\r\n\r\n".repeat(8);
    let writer = {
        let mut w = c.try_clone().unwrap();
        std::thread::spawn(move || w.write_all(reqs.as_bytes()).unwrap())
    };
    std::thread::sleep(Duration::from_millis(200));
    let mut buf = Vec::new();
    let total: usize = (0..8).map(|_| read_resp(&mut c, &mut buf, false).body.len()).sum();
    writer.join().unwrap();
    check(f, "backpressure", total == 8 * 4 * 1024 * 1024, total);
    drop(s);

    // handler logs reach a pipe while the server runs
    let mut s = start_with(&built.binary, 1, std::process::Stdio::piped());
    let r = roundtrip(&s, "GET /log?x1 HTTP/1.1\r\n\r\n");
    let mut out = s.child.stdout.take().unwrap();
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let mut buf = [0u8; 256];
        let n = out.read(&mut buf).unwrap_or(0);
        let _ = tx.send(String::from_utf8_lossy(&buf[..n]).into_owned());
    });
    let line = rx.recv_timeout(Duration::from_secs(3)).unwrap_or_default();
    check(f, "logs flushed", r.text() == "ok" && line == "logged ?x1\n", line);
    drop(s);

    // several workers; a worker killed by a trap is replaced
    let s = start(&built.binary, 3);
    let oks = (0..30).filter(|_| roundtrip(&s, "GET / HTTP/1.1\r\n\r\n").text() == "hello").count();
    check(f, "workers", oks == 30, oks);
    for _ in 0..3 {
        let mut c = connect(&s);
        c.write_all(b"GET /crash HTTP/1.1\r\n\r\n").unwrap();
        check(f, "trap closes the connection", closed(&mut c), "");
    }
    std::thread::sleep(Duration::from_millis(1500));
    let oks = (0..30).filter(|_| roundtrip(&s, "GET / HTTP/1.1\r\n\r\n").text() == "hello").count();
    check(f, "workers respawned", oks == 30, oks);
    // SIGTERM to the supervisor alone stops the workers too
    let _ = Command::new("kill").arg("-TERM").arg(s.child.id().to_string()).status();
    let end = Instant::now() + Duration::from_secs(3);
    while TcpStream::connect(("127.0.0.1", s.port)).is_ok() && Instant::now() < end {
        std::thread::sleep(Duration::from_millis(50));
    }
    check(f, "supervisor stops workers", TcpStream::connect(("127.0.0.1", s.port)).is_err(), "port still open");
    drop(s);

    // SIGTERM to the whole process group (as a shell or a process manager sends it): the
    // workers die first, and the supervisor must not mistake that for crashes and respawn.
    for round in 0..5 {
        let mut s = start(&built.binary, 3);
        let _ = roundtrip(&s, "GET / HTTP/1.1\r\n\r\n");
        kill_group(s.child.id());
        let end = Instant::now() + Duration::from_secs(3);
        let mut exited = false;
        while Instant::now() < end {
            if let Ok(Some(_)) = s.child.try_wait() {
                exited = true;
                break;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        std::thread::sleep(Duration::from_millis(100));
        let open = TcpStream::connect(("127.0.0.1", s.port)).is_ok();
        check(f, "group SIGTERM stops everything", exited && !open, (round, exited, open));
        let _ = s.child.kill();
    }

    for x in &failed {
        eprintln!("FAIL {x}");
    }
    println!("http: {} failed", failed.len());
    if !failed.is_empty() {
        std::process::exit(1);
    }
}
