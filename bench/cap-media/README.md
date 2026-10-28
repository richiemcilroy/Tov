# Cap's media server: Bun vs Tov

[Cap's media server](https://github.com/CapSoftware/Cap/tree/main/apps/media-server) is a Bun app: Hono routes, zod, mediabunny with node-av (an N-API addon wrapping FFmpeg), `Bun.spawn` for ffmpeg subprocesses, `Bun.file`, and Bun's convention of serving an entry point's default export. Cap runs it as `bun src/index.ts`.

Tov runs the same source, unchanged. `out/ms/main.tov` is the whole port:

```ts
import server from "cap-media-server"
import { serve } from "bun"

function main() {
  // Cap's media server, as Bun runs it: its default export served
  try serve(server)
}
```

`cap-media-server` is a one-line package re-exporting `src/index.ts`. `bun` is Tov's `bun` module (`runtime/node/bun.js`): `serve` on Tov's native HTTP server, `spawn` over `child_process`, and `file` as a Blob read from its file.

## What's measured

`run.py` copies the media server out of a Cap checkout, installs its packages with `bun install --production`, and builds the Tov binary. It then serves a generated test video (10 s, 1280×720 H.264 with AAC) over local HTTP. Each runtime is started in turn, interleaved across repetitions, and for each:

| metric | how |
|---|---|
| `startup_ms` | from exec to the first `200` from `/health` |
| `idle_mb` | physical footprint 3 s after startup |
| `health_rps`, `health_p50_us`, `health_p99_us` | `GET /health` with 64 connections (`bench/http/load.c`) |
| `probe_ops`, `probe_p50_ms`, `probe_p99_ms` | `POST /video/probe` (node-av reads the video over HTTP), 4 in flight (the server takes 6 at once) |
| `extract_ops`, `extract_p50_ms`, `extract_p99_ms` | `POST /audio/extract` (an ffmpeg subprocess, its output streamed back), 2 in flight |
| `health_cpu_us`, `probe_cpu_us`, `extract_cpu_us` | the server's own CPU time (user + system, not its children's) per completed request |
| `after_mb`, `peak_mb` | physical footprint 3 s after the load, and its peak |
| `shutdown_ms` | from SIGTERM to exit (the server's own shutdown handler runs) |

The media routes are driven by `client.mjs` under Node, so the client is the same whichever runtime serves. `/video/thumbnail` isn't measured: with FFmpeg 8 its `image2` pipe output fails (exit 234) under both runtimes.

```
bench/cap-media/run.py --cap ~/github/Cap --reps 3 --secs 10
```

## Results

Tov on its own JavaScriptCore (`scripts/jsc`). The origin and the load generator run on the same machine, so differences of a few percent are noise. Memory is the physical footprint on macOS and the resident set on Linux.

macOS (M4 Max), Bun 1.4.4: medians of 5 repetitions with 10 s per load, from `bench/results/cap-media-20261028-142535.json`.

| | Bun | Tov |
|---|---:|---:|
| startup (exec to first `/health`) | 128 ms | **101 ms** |
| idle memory | 47.9 MB | **43.0 MB** |
| `/health` | 31.8k req/s (p99 3.9 ms) | **40.8k req/s** (p99 2.4 ms) |
| CPU per `/health` | 35 µs | **27 µs** |
| `/video/probe` | 2,708/s (p50 1.37 ms, p99 3.0 ms) | **2,732/s** (p50 1.37 ms, p99 3.1 ms) |
| CPU per probe | 563 µs | **405 µs** |
| `/audio/extract` | 26.2/s (p50 76.1 ms) | 26.0/s (p50 76.5 ms) |
| CPU per extract (the server's, not ffmpeg's) | 6.8 ms | **2.4 ms** |
| memory 3 s after the load | 60.3 MB | **52.7 MB** |
| peak memory | 162 MB | **90 MB** |
| shutdown (SIGTERM to exit, as `Popen.wait` sees it) | 8.8 ms | 8.8 ms |
| failed requests | 0 | 0 |

Linux (x86-64, Ryzen 9950X), an earlier run (Bun 1.4.0, the engine and runtime of 2026-10-11, 3 repetitions):

| | Bun | Tov |
|---|---:|---:|
| startup (exec to first `/health`) | **193 ms** | 354 ms |
| idle memory | **91 MB** | 125 MB |
| `/health` | 21.4k req/s (p99 6.3 ms) | **22.5k req/s** (p99 4.6 ms) |
| CPU per `/health` | 53 µs | **51 µs** |
| `/video/probe` | 517/s (p99 66 ms) | **899/s** (p99 12 ms) |
| CPU per probe | 2,708 µs | **1,349 µs** |
| `/audio/extract` | 17.9/s (p99 249 ms) | 16.9/s (p99 167 ms) |
| CPU per extract (the server's, not ffmpeg's) | **8.6 ms** | 13.2 ms |
| memory 3 s after the load | **137 MB** | 148 MB |
| peak memory | **162 MB** | 205 MB |
| failed requests | 0 | 0 |

The Tov binary builds in 0.5 s on macOS with its bytecode cached and is 36 MB with its engine; on Linux, 47 MB. The packages' native addon (node-av, 61 MB) loads from `node_modules`, as under Bun.

- **CPU**: on macOS Tov serves `/health` on 22% less CPU than Bun, probes a video on 28% less, and extracts audio on 64% less.
- **Latency**: probes and extracts spend most of their time waiting on the origin and ffmpeg, and come out level. Bun's `fetch()` client reads and writes its sockets on a thread of its own; Tov's does so on the event loop, which with 4 probes in flight is idle about half the time.
- **Startup**: Tov loads the server's 654 modules from its bytecode cache, written the first time the program idles. On Linux it started slower than Bun, and held more memory throughout: the engine's Linux build wasn't tuned as the macOS one was.
- **Memory**: Tov peaks lower under load: its engine lets the heap grow to about three times what survives a collection, and counts the 64 KB arrays a probe streams through as freed when a young collection frees them (JavaScriptCore otherwise counts them until a full collection, which they bring on). Once the server has been idle a second it collects and returns memory (a timer that does next to nothing, like the 5 s timeout the server leaves per ffmpeg it runs, doesn't count as work), and drops compiled code soon after, all but the runtime's own bytecode.
- **Shutdown**: measured exactly (a blocking `waitpid`), Tov exits ~4.5 ms after SIGTERM and Bun ~3.7 ms; the harness's `Popen.wait` polls, so both read 8.8 ms. Most of the difference is the server's shutdown code running for the first time, its bytecode dropped while it idled.
- **Reliability**: when the origin refuses a connection under load, mediabunny's retried fetch rejects. Bun 1.4.0 exited on it in earlier runs; Tov reports the request's failure and carries on.
