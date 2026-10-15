# Tov

A programming language for coding agents. Tov reads like TypeScript, so your agent can already write it, and when the agent gets something wrong the compiler hands back the edit that fixes it. What comes out is one native binary, faster than Rust's on our benchmarks and in a fraction of the memory Bun uses ([the numbers](#performance)).

To start, paste the prompt on [tov.sh](https://tov.sh) into your agent. It installs Tov and reads [tov.sh/llms.txt](https://tov.sh/llms.txt), which is the language written up for agents. To install it yourself, run `curl -fsSL https://tov.sh/install.sh | sh`. That builds Tov from source, so it needs git and a C compiler.

Code that looks like TypeScript behaves like TypeScript, or doesn't compile. Bun's and Node's APIs are there (`Bun.serve`, `fetch`, `node:fs`, `async`/`await`), and so are npm packages: an app written for Bun runs on Tov unchanged. What Tov leaves out is what makes programs hard to follow. There's no `any`, `null` or `==`, a call that can fail is marked `try`, integer overflow stops the program, and arrays and records are values, so nothing changes behind the agent's back. Errors have a code, and where the fix is clear, `tov check --json` returns the exact edit and says whether it's safe to apply as it is.

A program compiles in about 42 ms (Rust: 98). A web server serves 242,000 requests a second on one core in 1.6 MB of memory, against 216,000 in 4.4 MB for Rust's axum and 111,000 in 17 MB for Bun (macOS, M4 Max).

The landing page, [site/](site/), is a Tov program: `cd site && tov dev`.

## Status

Tov is early, and it changes every day. It's at version 0.0.1.

| Works today | Not yet |
|---|---|
| Command-line programs and web servers | Windows |
| Records, unions, generics, classes, interfaces, checked errors | A package manager of its own (install packages with npm or Bun) |
| `async`/`await`, promises, timers | Tuples, enums and some other TypeScript features ([spec](docs/spec.md)) |
| `fetch()` over HTTP and HTTPS, with streaming, compression and proxies | TLS sockets (`tls.connect`) and worker threads for npm packages |
| Bun's server API (`Bun.serve`, routes, `Request`, `Response`) | `Promise.allSettled`/`any`, `.then`/`.catch` |
| A Node-style standard library (`fs`, `path`, `process`) | |
| npm packages, including Node-API native addons, and Bun apps | |
| macOS (arm64) and Linux | |

## How an agent works with Tov

An agent writes TypeScript:

```ts
Bun.serve({
  port: 3000,
  fetch(req) {
    const url = new URL(req.url)
    return new Response(`Hello from ${url.pathname}`)
  },
})
```

Tov says what's wrong and how to fix it, for people:

```
error[T0831] hello.tov:4:17: `new URL` can throw `TypeError`; mark the call `try` to pass the error on, or catch it
    4 |     const url = new URL(req.url)
  why: errors are values in Tov: every call that can fail is marked, so control flow is visible
  or: wrap it in `try { ... } catch (e) { ... }`
  fix[safe]: pass it on: `try new URL(req.url)`
```

and for tools (`tov check --json`, trimmed):

```json
{
  "code": "T0831",
  "file": "hello.tov",
  "start": { "line": 4, "col": 17 },
  "message": "`new URL` can throw `TypeError`; mark the call `try` to pass the error on, or catch it",
  "fixes": [{
    "label": "pass it on: `try new URL(req.url)`",
    "applicability": "safe",
    "edits": [{ "start": { "line": 4, "col": 17 }, "end": { "line": 4, "col": 17 }, "text": "try " }]
  }]
}
```

A `safe` fix can be applied as it is, and `tov explain T0831` describes any code. With the fix applied, `tov build hello.tov` writes a 152 KB binary.

To get an agent started, give it [tov.sh/llms.txt](https://tov.sh/llms.txt). It's [docs/spec.md](docs/spec.md), which lists only what differs from TypeScript, with a short guide to the commands in front.

## Example

```ts
type Shape =
  | { kind: "circle", r: f64 }
  | { kind: "rect", w: f64, h: f64 }

function area(s: Shape): f64 {
  switch (s.kind) {
    case "circle":
      return Math.PI * s.r * s.r
    case "rect":
      return s.w * s.h
  }
}

function main() {
  const shapes: Shape[] = [{ kind: "circle", r: 1 }, { kind: "rect", w: 2, h: 3 }]
  for (const s of shapes) {
    console.log(`${s.kind}: ${area(s).toFixed(2)}`)
  }
}

test("rect area", () => {
  expect(area({ kind: "rect", w: 2, h: 3 })).toBe(6)
})
```

## Getting started

You need Rust (to build the compiler) and a C compiler (`clang` or `gcc`).

```sh
git clone https://github.com/richiemcilroy/Tov
cd Tov && cargo install --path crates/tov   # puts `tov` in ~/.cargo/bin

tov run examples/shapes          # build and run main()
tov --watch server.tov          # rebuild and restart when a source file changes
tov dev                          # run package.json's "dev" script, as `bun dev` does
tov test examples                # run every test(...)
tov build examples/shapes -o app # write a native binary
tov check src/                   # type-check only (--json for tools)
tov explain T0001                # what an error code means
```

The command line works like Bun's: `tov <file>` runs a program and passes it the arguments after the file name, `tov <script>` runs a `package.json` script (with `pre`/`post` scripts, the `npm_*` environment and Bun's output), and `--watch` restarts the program when the entry file or anything it imports changes.

## How it differs from TypeScript

Tov keeps TypeScript's syntax and drops the parts that make programs hard to reason about or slow to run:

- No `any`, `null`, `==` or implicit type conversions.
- `int` and `f64` are separate types. Integer overflow traps instead of silently losing precision.
- Arrays, records and maps are values. Assigning one makes a copy, and the compiler flags code where TypeScript would have shared the value instead.
- To modify a caller's value, a function declares an `inout` parameter and the caller passes `&x`.
- Every call that can fail is marked `try` (or caught), and a function's errors are part of its type.
- Unions of object types must be narrowed before you use their fields, and a `switch` must cover every case.
- Classes are reference-counted, not garbage-collected. If instances of a class could reference each other in a cycle, the compiler asks you to mark the back-reference `weak`, make it `readonly`, or declare a `cyclic class` (which adds cycle collection for that class only).

The full list is in [docs/spec.md](docs/spec.md).

## npm packages and Bun apps

Install packages with npm or Bun, then import them:

```ts
import { z } from "zod"

const User = try z.object({ name: z.string(), age: z.number() })
const r = try User.safeParse({ name: "Ada", age: 36 })
console.log(r.success as bool)
```

Tov code compiles to native code. The packages are bundled when the program is built (resolved as Node.js resolves them; ES modules, TypeScript and JSX converted), embedded in the binary, and run on JavaScriptCore, the engine Bun uses. Values that come from JavaScript have the type `Js` and are checked when converted (`as bool`), and JavaScript calls that can throw are marked `try` like any other.

Node's built-in modules are there for packages that need them (`fs`, `http`, `net`, `child_process`, `crypto`, `zlib`, `dns`, streams and the rest), and so is Bun's (`Bun.serve`, `Bun.spawn`, `Bun.file`). Native addons load through Node-API; an addon a package requires by name (`require("./build/addon.node")`) is loaded from the program's own directory, and `tov build` copies it there. An app written for Bun runs as it is: this is the whole port of [Cap's media server](https://github.com/CapSoftware/Cap/tree/main/apps/media-server) (Hono, zod, FFmpeg through a native addon, `Bun.spawn`):

```ts
import server from "cap-media-server"
import { serve } from "bun"

function main() {
  // Cap's media server, as Bun runs it: its default export served
  try serve(server)
}
```

Programs that import npm packages link Tov's own build of JavaScriptCore when it's built (`scripts/jsc/build.sh`, about 10 minutes), or else the system's (macOS) or WebKitGTK's (Linux). The engine makes such a binary about 34 MB; programs that don't import npm packages don't include it.

## Performance

All numbers are from an Apple M4 Max. Each benchmark's README has its method, every result, its caveats and, for most, the same benchmark on Linux (x86-64).

### Programs

Median wall time and peak memory. Every language prints identical output ([bench/README.md](bench/README.md)).

| benchmark | C | Rust | Node | Bun | **Tov** |
|---|---:|---:|---:|---:|---:|
| binary_trees | 686 ms · 18.7 MB | 793 ms · 19.0 MB | 461 ms · 212 MB | 381 ms · 195 MB | **260 ms · 18.7 MB** |
| class_trees | 690 ms · 18.7 MB | 793 ms · 19.0 MB | 467 ms · 210 MB | 371 ms · 223 MB | **259 ms · 18.7 MB** |
| dispatch (virtual calls) | 76 ms · 1.9 MB | 76 ms · 2.1 MB | 201 ms · 82 MB | 109 ms · 27 MB | **39 ms · 1.9 MB** |
| map_insert | 524 ms · 235 MB | 534 ms · 311 MB | 945 ms · 295 MB | 764 ms · 592 MB | **276 ms · 157 MB** |
| json (stringify and parse) | — | 260 ms · 241 MB | 444 ms · 491 MB | 267 ms · 327 MB | **226 ms · 143 MB** |
| nbody | 310 ms · 1.8 MB | 221 ms · 2.0 MB | 556 ms · 81 MB | 505 ms · 23 MB | **206 ms** · 1.9 MB |
| sort | 268 ms · 26 MB | 73 ms · 40 MB | 1185 ms · 279 MB | 711 ms · 121 MB | **47 ms** · 26 MB |
| strings | 254 ms · 216 MB | 230 ms · 263 MB | 517 ms · 701 MB | 208 ms · 592 MB | **155 ms · 192 MB** |
| array_push | 51 ms · 244 MB | 59 ms · 244 MB | 347 ms · 780 MB | 207 ms · 814 MB | **48 ms · 242 MB** |
| fib | 492 ms · 1.9 MB | 439 ms · 2.0 MB | 1500 ms · 78 MB | 948 ms · 18.8 MB | **427 ms · 1.9 MB** |
| points | **288 ms** · 1.9 MB | 299 ms · 2.0 MB | 1891 ms · 87 MB | 815 ms · 32 MB | **288 ms · 1.8 MB** |
| async_calls (awaited calls) | — | **14 ms** · 2.0 MB | 643 ms · 81 MB | 525 ms · 30 MB | 17 ms · 1.9 MB |
| async_tasks (1,000 tasks) | — | **136 ms** · 2.3 MB | 353 ms · 97 MB | 179 ms · 31 MB | 137 ms · 2.5 MB |

Tov checks for integer overflow by default; Rust's release builds don't. `points` is bound by floating-point add latency in all three compiled languages. The async benchmarks' Rust runs on tokio's single-threaded runtime, one core like the others. Some differences are the libraries': Rust's `HashMap` hashes with SipHash (DoS-resistant and slower), and C's `qsort` calls its comparator through a function pointer.

Binaries are 33 KB for most of these, the same as C (83 KB at most; Rust's are 330–370 KB stripped), and a build takes 0.04–0.11 s (C 0.04–0.06 s, Rust 0.08–0.23 s, or about 2 s with tokio or serde). Run them yourself with `python3 bench/run.py`.

### HTTP server

Tov speaks Bun's server API, so a Bun server ports with only Tov's usual edits (a `try` on calls that can throw). `tests/parity` runs a Bun server under Bun and its port under Tov and checks that every response matches.

```ts
type Todo = { id: number; title: string }
const todos: Todo[] = []

const server = Bun.serve({
  port: 3000,
  routes: {
    "/todos/:id": (req) => {
      const todo = todos.find((t) => t.id === Number(req.params.id))
      return todo === undefined ? new Response("Not Found", { status: 404 }) : Response.json(todo)
    },
    "/todos": {
      GET: () => Response.json(todos),
      POST: async (req) => {
        const todo = try (await req.json()) as Todo
        todos.push(todo)
        return Response.json(todo, { status: 201 })
      },
    },
  },
  fetch(req) {
    return new Response("Not Found", { status: 404 })
  },
})
console.log(`Listening on ${server.url}`)
```

Requests per second on Linux (Docker), 128 keep-alive connections, JSON route, with Tov running the Bun benchmark's own code ([bench/http/README.md](bench/http/README.md)):

| | Rust (axum) | Bun | **Tov** |
|---|---:|---:|---:|
| 1 core | 294k | 257k | **371k** |
| 4 cores | 1.30M\* | 958k | **1.61M** |
| p99 at a fixed 600k req/s, 4 cores | 1.91 ms\* | — | **1.30 ms** |
| memory, 4 cores (PSS) | 5.1 MB\* | 97 MB | **1.6 MB** |

\* Thread-per-core Rust; tokio's default multi-thread runtime reaches 720k. With pipelining, Tov serves 8.1M JSON requests/s on 4 cores (Rust 2.4M).

### HTTP client

`fetch()` against a local server, macOS ([bench/fetch/README.md](bench/fetch/README.md)):

| | **Tov** | Bun | Node (undici) | Rust (reqwest) |
|---|---:|---:|---:|---:|
| small requests, one at a time | **45.7k req/s** | 24.9k | 15.2k | 34.8k |
| small requests, 64 at once | **179k req/s** | 128k | 31.3k | 145k\* |
| CPU per request, 64 at once | **5.5 µs** | 13.0 µs | 47.1 µs | 7.6 µs |
| memory, 64 at once | **3.1 MB** | 41.5 MB | 251 MB | 8.3 MB |
| 8 MB downloads, one at a time | **8.6 GB/s** | 7.7 GB/s | 2.1 GB/s | 1.7 GB/s |
| HTTPS, 64 at once | **162k req/s** | 116k | 28.3k | 128k\* |

\* Rust's multi-threaded runtime; its single-threaded one is in the README.

### npm packages

[Cap's media server](bench/cap-media/README.md) on Bun and, unchanged, on Tov (macOS, Tov's own JavaScriptCore, medians of 3 runs of 10 s each):

| | Bun 1.4 | **Tov** |
|---|---:|---:|
| startup (exec to first `/health`) | 128 ms | **107 ms** |
| idle memory | 50 MB | 50 MB |
| `/health` (64 connections) | 33.1k req/s | **35.5k req/s** |
| `/video/probe` (4 in flight) | **1,468/s** | 1,353/s |
| CPU per probe | 1,248 µs | **881 µs** |
| `/audio/extract` (an ffmpeg subprocess, streamed) | 19.6/s | 19.0/s |
| peak memory under load | 136 MB | **126 MB** |
| memory 3 s after the load | 65 MB | 66 MB |
| failed requests | 0 | 0 |

Tov does the media work on less CPU than Bun (29% less per probe), but with 4 probes in flight Bun finishes 8% more of them each second: its `fetch()` client runs on a thread of its own, and Tov's runs on the event loop. Tov's binary is 36 MB with its engine and builds in 1.3 s with nothing cached.

Where Tov is behind: a million calls from Tov into a small npm function (zod's `safeParse`) take 146 ms, against 104 ms when Bun runs the same program in JavaScript, because every call crosses from native code into the engine; and a program using npm packages starts in 11.5 ms, against Bun's 10.1 ([bench/npm/README.md](bench/npm/README.md)).

## Development

```sh
cargo test                        # checker snapshots, fuzzing, native run tests, HTTP and npm tests
scripts/linux.sh                  # the npm tests in a Linux container (any command: scripts/linux.sh cargo test)
scripts/jsc/build.sh              # Tov's own JavaScriptCore (`scripts/jsc/build.sh linux` for Linux's)
python3 bench/run.py              # cross-language benchmarks
python3 bench/http/run.py         # HTTP server benchmark (bench/http/linux.sh for Linux)
scripts/sanitize.sh run file.tov # run under ASan/UBSan in a Linux container
```

## Project layout

- `crates/tov/`: the compiler and the npm bundler (Rust, no dependencies)
- `runtime/`: the C runtime linked into every program (the event loop, HTTP server and client), the JavaScriptCore bridge (`js.c`, `napi.c`) and Node's built-ins for npm packages (`node.c`, `node/`)
- `scripts/jsc/`: Tov's build of JavaScriptCore and its patches
- `vendor/boringssl/`, `vendor/brotli/`, `vendor/zstd/`: BoringSSL (Apache-2.0) for TLS, and the brotli (MIT) and zstd (BSD) codecs. They're compiled once, and only programs that use them link them (see each `TOV.md`).
- `examples/`: sample programs
- `tests/`: checker snapshots, run tests, fuzzing, and npm, HTTP and parity tests
- `bench/`: benchmarks against C, Rust, Node and Bun, and Cap's media server on Bun and Tov
- `site/`: the landing page, a Tov program (`cd site && tov dev`)
- `docs/`: the [language spec](docs/spec.md)

## License

MIT, see [LICENSE](LICENSE). The vendored libraries in `vendor/` and Node's built-ins in `runtime/node/` keep their own licenses.
