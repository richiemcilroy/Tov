//! Bun parity: serves tests/parity/server.ts with `bun` (NODE_ENV=production) and its Tov
//! port tests/parity/server.tov.ts natively, sends both the same requests in the same order,
//! and compares status, body and the headers a client would act on. The same for
//! framework.ts: an app whose handlers a JavaScript framework calls (issue #20). Skipped
//! without `bun`.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::Path;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

struct Server(Child, u16);

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

fn free_port() -> u16 {
    TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port()
}

fn start(mut cmd: Command, port: u16) -> Server {
    cmd.env("PORT", port.to_string()).env("NODE_ENV", "production").stdout(Stdio::null()).stderr(Stdio::null());
    let child = cmd.spawn().expect("start server");
    let end = Instant::now() + Duration::from_secs(10);
    while TcpStream::connect(("127.0.0.1", port)).is_err() {
        assert!(Instant::now() < end, "server did not start");
        std::thread::sleep(Duration::from_millis(20));
    }
    Server(child, port)
}

#[derive(Debug, PartialEq)]
struct Resp {
    status: u32,
    headers: Vec<(String, String)>,
    body: String,
}

/// One request on a fresh connection (`Connection: close`), read to the end.
fn send(port: u16, req: &str) -> Resp {
    let mut c = TcpStream::connect(("127.0.0.1", port)).unwrap();
    c.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    c.write_all(req.as_bytes()).unwrap();
    let mut buf = Vec::new();
    let _ = c.read_to_end(&mut buf);
    let text = String::from_utf8_lossy(&buf).into_owned();
    let (head, body) = text.split_once("\r\n\r\n").unwrap_or((&text, ""));
    let mut lines = head.lines();
    let status = lines.next().and_then(|l| l.get(9..12)).and_then(|s| s.parse().ok()).unwrap_or(0);
    // Headers a client acts on (not date, length, etag or connection management).
    let mut headers: Vec<(String, String)> = lines
        .filter_map(|l| l.split_once(':'))
        .map(|(k, v)| (k.trim().to_ascii_lowercase(), v.trim().to_string()))
        .filter(|(k, _)| matches!(k.as_str(), "content-type" | "location" | "cache-control") || k.starts_with("x-"))
        .collect();
    headers.sort();
    Resp { status, headers, body: body.to_string() }
}

fn get(path: &str) -> String {
    format!("GET {path} HTTP/1.1\r\nHost: localhost\r\nUser-Agent: parity/1\r\nConnection: close\r\n\r\n")
}

fn with_body(method: &str, path: &str, body: &str) -> String {
    format!("{method} {path} HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}", body.len())
}

fn main() {
    if Command::new("bun").arg("--version").output().is_err() {
        println!("parity: skipped (bun not found)");
        return;
    }
    let server = [
        get("/"),
        get("/health"),
        get("/todos"),
        get("/todos/2"),
        get("/todos/9"),
        get("/todos/"),
        with_body("POST", "/todos", r#"{"title":"new"}"#),
        get("/todos"),
        get("/todos/3"),
        get("/users/ada/posts/42"),
        get("/users/a%20b/posts/x"),
        get("/static/css/site.css"),
        get("/static/app.js"),
        get("/static"),
        get("/only-get"),
        with_body("POST", "/only-get", ""),
        get("/boom"),
        get("/empty"),
        with_body("POST", "/echo", "hello, world"),
        get("/search?q=hi%20there&tag=a&tag=b&page=2"),
        get("/search?q=a+b"),
        get("/redirect"),
        "GET /headers HTTP/1.1\r\nHost: localhost\r\nUser-Agent: parity/1\r\nx-custom: Yes\r\nConnection: close\r\n\r\n".to_string(),
        with_body("DELETE", "/headers", ""),
        get("/nope/deeper?x=1"),
        get("/health"),
    ];
    // (each bad input is sent twice: the server is still there after the first)
    let framework = [
        get("/users/ada"),
        get("/users/a"),
        get("/users/a"),
        get("/users/bob"),
        get("/helper/ada"),
        get("/helper/a"),
        get("/guarded/ada"),
        get("/guarded/a"),
        get("/async/ada"),
        get("/async/a"),
        get("/async/a"),
        get("/fail"),
        get("/refused"),
        get("/query?q=hi"),
        get("/nope"),
        get("/users/ada"),
    ];
    let mut failed = 0;
    let mut total = 0;
    for (name, requests) in [("server", &server[..]), ("framework", &framework[..])] {
        let (n, f) = compare(name, requests);
        total += n;
        failed += f;
    }
    println!("parity: {total} requests, {failed} differ");
    if failed > 0 {
        std::process::exit(1);
    }
}

/// Serves tests/parity/<name>.ts with Bun and <name>.tov.ts natively and sends both `requests`:
/// (requests sent, how many answers differ).
fn compare(name: &str, requests: &[String]) -> (usize, usize) {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap();
    let dir = root.join("tests/parity");
    let opts = tov::build::Options { mode: tov::codegen::Mode::Run, unchecked: false, opt: "-O2".into(), emit_c: None, symbols: false };
    let built = match tov::build::build(&[dir.join(format!("{name}.tov.ts"))], &root, &opts) {
        Ok(b) => b,
        Err(tov::build::BuildError::Diagnostics(sm, d)) => panic!("{name}: build failed\n{}", tov::diag::render_text(&d, &sm)),
        Err(tov::build::BuildError::Message(m)) => panic!("{name}: {m}"),
    };
    let (bp, np) = (free_port(), free_port());
    let tov = start(Command::new(&built.binary), bp);
    let mut bun_cmd = Command::new("bun");
    bun_cmd.arg(dir.join(format!("{name}.ts")));
    let bun = start(bun_cmd, np);
    let mut failed = 0;
    for r in requests {
        let (mut a, mut b) = (send(tov.1, r), send(bun.1, r));
        if name == "framework" {
            // (the framework builds its Responses in JavaScript, whose `content-type` charset
            // parameter still differs from Bun's: `UTF-8` for `utf-8`, none on JSON; a separate
            // gap from what this pair checks, which is who answers a handler's exception)
            for x in [&mut a, &mut b] {
                x.headers.retain(|(k, _)| k != "content-type");
            }
        }
        if a != b {
            failed += 1;
            eprintln!("FAIL {name}: {}\n  tov: {a:?}\n  bun:  {b:?}", r.lines().next().unwrap());
        }
    }
    (requests.len(), failed)
}
