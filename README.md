# Tov

**Tov is a TypeScript-shaped language that runs faster than Rust.** It compiles to a small native binary that, on most of our benchmarks, beats Rust and C, and runs several times faster than Bun or Node in a fraction of the memory.

```sh
tov run app.ts             # build and run
tov build app.ts -o app    # write a native binary
tov test src/              # run every *.test.ts (Jest's API: describe, it, expect)
```

## The goal

1. **Run any TypeScript as it is.** Code written for TypeScript, Bun or Node should compile unchanged and behave exactly as it does there: the same semantics, Bun's and Node's APIs, npm packages.
2. **Be the fastest way to run it.** Faster than Bun and Node, and at least as fast as C and Rust, using less memory, on every benchmark.

Tov is early (version 0.1.0) and changes every day. Where it doesn't match TypeScript yet, the aim is that it tells you at compile time, with an error code and usually the fix, rather than quietly doing something different.

## Benchmarks

The same `main.ts`, run by Node and Bun and compiled by Tov, next to C and Rust versions of the same program. Every version prints the same output. These are medians of 5 runs on an Apple M4 Max, 22 October 2026 ([all 25 benchmarks, and how they're run](bench/README.md)).

| benchmark | C | Rust | Node | Bun | **Tov** |
|---|---:|---:|---:|---:|---:|
| binary_trees (allocation) | 746 ms | 849 ms | 488 ms | 408 ms | **268 ms** |
| dispatch (virtual calls) | 84 ms | 85 ms | 222 ms | 119 ms | **44 ms** |
| map_insert | 549 ms | 548 ms | 937 ms | 746 ms | **204 ms** |
| sort | 278 ms | 74 ms | 1,197 ms | 710 ms | **46 ms** |
| strings | 274 ms | 228 ms | 520 ms | 204 ms | **108 ms** |
| json (stringify + parse) | — | 274 ms | 485 ms | 280 ms | **246 ms** |
| fib (recursion) | 505 ms | **457 ms** | 1,522 ms | 959 ms | 467 ms |
| nbody (floating point) | 318 ms | 230 ms | 570 ms | 516 ms | **227 ms** |
| levenshtein | **486 ms** | 564 ms | 1,007 ms | 896 ms | 537 ms |
| mandelbrot | **523 ms** | 546 ms | 690 ms | 572 ms | 564 ms |
| async_calls (awaits) | — | **15 ms** | 653 ms | 547 ms | 19 ms |
| decimal_ledger | — | **139 ms** | 1,997 ms | 1,974 ms | 310 ms |

Across all 25, Tov beats Node and Bun on every one and is the fastest of the five on 16. It's furthest behind on exact decimals (Rust's `rust_decimal` is 2–4× faster) and on awaiting tiny async calls (Rust is 1.2× faster).

Memory is where the gap is widest. A small program peaks at 1.7 MB under Tov, against 20–30 MB under Bun. `binary_trees` peaks at 18.5 MB, against Bun's 185 MB.

**Web servers.** Running the Bun HTTP benchmark's own code on Linux, Tov serves 371k requests/s on one core (Bun 257k, Rust's axum 294k), and 1.61M on four in 1.6 MB of memory (Bun: 97 MB). See [bench/http](bench/http/README.md).

**`fetch()`.** Tov makes 45.7k requests/s one at a time (Bun 24.9k), and 179k/s with 64 in flight (Bun 128k). See [bench/fetch](bench/fetch/README.md).

## Where it stands

**Works today:** command-line programs and web servers; classes, generics, unions, async/await; `Bun.serve`, `fetch()`, `node:fs`/`path`/`process` and the rest of Node's built-ins; npm packages (run on JavaScriptCore, the engine Bun uses), including native addons; Jest-style tests; macOS (arm64) and Linux.

**Not yet:** `any` (beyond where the type is known anyway), classes kept in variables or arrays (they can be passed as arguments), Windows.

We measure TypeScript parity on real code. [TheAlgorithms/TypeScript](https://github.com/TheAlgorithms/TypeScript) is a library of 99 test files written for Jest and `tsc`; 72 of them build and pass unchanged under `tov test`, and most of the rest stop at a compile error naming the missing feature. A known case still behaves differently from JavaScript instead of failing to compile: reading past the end of an array outside a comparison or condition traps (JavaScript gives `undefined`). Each gap we close is tested in `tests/run/`. The full list of differences is in [docs/spec.md](docs/spec.md).

## Install

```sh
curl -fsSL https://tov.sh/install.sh | sh
```

This builds Tov from source. It needs git and a C compiler, and installs Rust if it's missing. Using a coding agent? Paste the prompt on [tov.sh](https://tov.sh): it installs Tov and has the agent read [tov.sh/llms.txt](https://tov.sh/llms.txt).

To build it yourself (Rust 1.95+): `git clone https://github.com/richiemcilroy/Tov && cd Tov && cargo install --path crates/tov`.

## Beyond TypeScript

Plain TypeScript is the main thing. Files ending in `.tov` can also use a few extras when you want more control: `int` and sized integers that trap on overflow, an exact `decimal` type, errors as part of a function's type (`try`), and `inout` parameters. See [docs/spec.md](docs/spec.md).

## Development

```sh
cargo test                  # checker, runtime, HTTP and npm tests
python3 bench/run.py        # the benchmarks above
```

`crates/tov/` is the compiler (Rust, no dependencies). It generates C, which is linked with the runtime in `runtime/` (C). Benchmarks are in `bench/`, tests in `tests/`, and the landing page in `site/` is itself a Tov program.

## License

MIT, see [LICENSE](LICENSE). The libraries in `vendor/` and Node's built-ins in `runtime/node/` keep their own licenses.
