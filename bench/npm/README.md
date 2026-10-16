# npm packages from Tov

A Tov program using an npm package (zod), against the same program in Bun. Run `bun install`
here first, then `bench/npm/run.py`.

| | what it measures |
|---|---|
| `startup` | start, load zod, validate one object, exit (wall time, median of 20) |
| `loop` | a million `safeParse` calls, each with an object built for it: every call crosses from Tov into JavaScript, and each does little work (the bridge's worst case) |
| `callback` | a million calls from a JavaScript loop into a Tov function (`loop`'s other direction: a framework calling a handler) |
| `service` | an HTTP server validating each request's JSON body with zod (Tov's server against `Bun.serve`), driven by `bench/http`'s load generator (64 connections, 4 threads, 5 s) |

How Tov runs npm code: the packages are bundled when the program is built (resolved like
Node.js; ES modules, TypeScript and JSX converted), embedded in the binary, and run on Tov's own
build of JavaScriptCore (`scripts/jsc`; the engine Bun uses, linked into the program), each
module compiled when it's first required.
Calls go through runtime/js.c: numbers, booleans and `undefined` cross without engine calls, and
a JavaScript expression in Tov code (`User.safeParse({ name, age })`) compiles to one engine call.

Measure on Tov's own engine (`scripts/jsc/build.sh`). Without it, Linux programs fall back to
WebKitGTK's shared JavaScriptCore, where every crossing is slower and a JavaScript exception that
reaches Tov costs about 95 µs (resolving its native stack against that library's symbol
table), against about 1.4 µs for one thrown and caught inside JavaScript: error paths measured
there look far worse than they are.

## Results

Medians of 3 runs, Bun 1.4.0, from `bench/results/npm-20261011-013402.json` (macOS) and a run on Linux.

| | macOS (M4 Max): Tov | Bun | Linux (x86-64, Ryzen 9950X): Tov | Bun |
|---|---:|---:|---:|---:|
| startup | 11.5 ms | **10.1 ms** | 11.3 ms | **8.4 ms** |
| startup memory (RSS) | 22.5 MB | **16.5 MB** | 26.5 MB | **20.3 MB** |
| 1M zod calls | 146 ms | **104 ms** | 280 ms | **211 ms** |
| service req/s | **141k** | 116k | **233k** | 154k |
| service CPU per request | **7.1 µs** | 8.9 µs | **4.4 µs** | 6.9 µs |
| service p99 | **810 µs** | 1,071 µs | **517 µs** | 914 µs |
| service memory (RSS) | 50.6 MB | **47.4 MB** | **47.1 MB** | 52.3 MB |
| binary size | **30 MB** | 64 MB | **38 MB** | 83 MB (`bun build --compile`) |
| rebuild after an edit | **132 ms** | 144 ms | **391 ms** | 1,261 ms (`bun build --compile`) |

On macOS the load generator and loopback, not the servers, set the ceiling on req/s; CPU per
request is the measure that tells the servers apart there.

Where Tov is behind:
- The call-heavy loop. Each call into JavaScript costs about 40 ns of engine entry and exit,
  and Bun has no such boundary. In a service, a request crosses once or twice, and Tov's
  server more than makes up for it.
- Startup and its memory. The engine is linked into the program, so its code is paged in from
  the binary as it starts (the system's JavaScriptCore, `TOV_JSC=system`, is already in memory:
  10.4 ms and 15 MB on macOS).
