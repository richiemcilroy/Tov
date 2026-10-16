#!/usr/bin/env python3
"""npm packages from Tov against Bun: startup, a call-heavy loop (each way), an HTTP service, binary
size and build time. Run `bun install` here first (zod). Interleaves Tov and Bun per repetition and
reports medians.

  bench/npm/run.py [--repeat N]
"""
import argparse, json, os, socket, statistics, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import rusage  # noqa: E402
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "target", "bench-npm")
TOV = os.path.join(ROOT, "target", "release", "tov")
LOAD = os.path.join(OUT, "load")
BODY = '{"name":"Ada Lovelace","email":"ada@example.com","age":36}'


def sh(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)}: exit {r.returncode}: {(r.stderr or r.stdout).strip()[-500:]}")
    return r.stdout


def wall(cmd, n=20):
    xs = []
    for _ in range(n):
        t = time.perf_counter()
        subprocess.run(cmd, stdout=subprocess.DEVNULL, cwd=HERE)
        xs.append((time.perf_counter() - t) * 1000)
    return statistics.median(xs)


def rss_mb(cmd):
    """The program's peak resident set, in MiB."""
    argv, report = rusage.wrap(cmd)
    p = subprocess.Popen(argv, stdout=subprocess.DEVNULL, cwd=HERE, start_new_session=report is not None)
    _, status, ru = os.wait4(p.pid, 0)
    p.returncode = os.waitstatus_to_exitcode(status)  # (reaped here: Popen mustn't wait again)
    rss, _ = rusage.read(report, ru)
    return (rss or 0) / 1024 / 1024


def service(cmd, port):
    p = subprocess.Popen(cmd, cwd=HERE, env={**os.environ, "PORT": str(port)}, stdout=subprocess.DEVNULL)
    # (listening: a first run, compiling its JavaScript, can take more than a second)
    end = time.time() + 20
    while True:
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            break
        except OSError:
            if p.poll() is not None or time.time() > end:
                p.kill()
                raise RuntimeError(f"{cmd[0]} didn't start serving on port {port}")
            time.sleep(0.05)
    c0 = rusage.cpu(p.pid)
    out = sh([LOAD, "-j", "-c", "64", "-t", "4", "-d", "5", "-w", "1", "-m", "POST", "-b", BODY, f"http://127.0.0.1:{port}/"])
    c1 = rusage.cpu(p.pid)
    rss = int(sh(["ps", "-o", "rss=", "-p", str(p.pid)]).strip()) / 1024
    p.kill()
    p.wait()
    j = json.loads(out.strip().splitlines()[-1])
    # (cpu over the 5 s measured plus the 1 s warmup, per request measured: an estimate)
    return j["rps"], (c1 - c0) / (j["rps"] * 6) * 1e6, rss, j["p99_us"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeat", type=int, default=3)
    a = ap.parse_args()
    if not os.path.isdir(os.path.join(HERE, "node_modules", "zod")):
        sys.exit("run `bun install` in bench/npm first")
    os.makedirs(OUT, exist_ok=True)
    subprocess.run(["cargo", "build", "-q", "--release"], cwd=ROOT, check=True)
    sh(["cc", "-O2", "-o", LOAD, os.path.join(ROOT, "bench", "http", "load.c"), "-lpthread"])
    builds = {}
    for name in ("startup", "loop", "callback", "service"):
        src = os.path.join(HERE, f"{name}.tov")
        exe = os.path.join(OUT, name)
        sh([TOV, "build", src, "-o", exe])
        # a rebuild after an edit: generate C, compile, link and sign again
        text = open(src).read()
        times = []
        for i in range(5):
            with open(src, "w") as f:
                f.write(text.replace('"Ada"', f'"Ada {i}"', 1))
            t = time.perf_counter()
            sh([TOV, "build", src, "-o", exe])
            times.append((time.perf_counter() - t) * 1000)
        with open(src, "w") as f:
            f.write(text)
        sh([TOV, "build", src, "-o", exe])
        builds[name] = statistics.median(times)
    t = time.perf_counter()
    sh(["bun", "build", "--compile", os.path.join(HERE, "startup.js"), "--outfile", os.path.join(OUT, "startup-bun")], cwd=HERE)
    bun_build = (time.perf_counter() - t) * 1000
    rows = {}
    rows["startup (ms)"] = (wall([os.path.join(OUT, "startup")]), wall(["bun", "--no-install", "startup.js"]))
    rows["startup rss (MB)"] = (rss_mb([os.path.join(OUT, "startup")]), rss_mb(["bun", "--no-install", "startup.js"]))
    loops = ([], [])
    for _ in range(a.repeat):
        loops[0].append(int(sh([os.path.join(OUT, "loop")], cwd=HERE).split()[1]))
        loops[1].append(int(sh(["bun", "--no-install", "loop.js"], cwd=HERE).split()[1]))
    rows["1M zod calls (ms)"] = (statistics.median(loops[0]), statistics.median(loops[1]))
    calls = ([], [])
    for _ in range(a.repeat):
        calls[0].append(int(sh([os.path.join(OUT, "callback")], cwd=HERE).split()[1]))
        calls[1].append(int(sh(["bun", "--no-install", "callback.js"], cwd=HERE).split()[1]))
    rows["1M calls into Tov (ms)"] = (statistics.median(calls[0]), statistics.median(calls[1]))
    svc = ([], [])
    for _ in range(a.repeat):
        svc[0].append(service([os.path.join(OUT, "service")], 39181))
        svc[1].append(service(["bun", "--no-install", "service.js"], 39182))
    med = lambda xs, k: statistics.median(x[k] for x in xs)
    rows["service req/s"] = (med(svc[0], 0), med(svc[1], 0))
    rows["service cpu/req (us)"] = (med(svc[0], 1), med(svc[1], 1))
    rows["service p99 (us)"] = (med(svc[0], 3), med(svc[1], 3))
    rows["service rss (MB)"] = (med(svc[0], 2), med(svc[1], 2))
    rows["binary (MB)"] = (os.path.getsize(os.path.join(OUT, "startup")) / 1e6, os.path.getsize(os.path.join(OUT, "startup-bun")) / 1e6)
    rows["rebuild after an edit (ms)"] = (builds["startup"], bun_build)
    print(f"{'':28} {'Tov':>10} {'Bun':>10}")
    for k, (b, u) in rows.items():
        print(f"{k:28} {b:10.1f} {u:10.1f}")
    with open(os.path.join(ROOT, "bench", "results", time.strftime("npm-%Y%m%d-%H%M%S.json")), "w") as f:
        json.dump({k: {"tov": b, "bun": u} for k, (b, u) in rows.items()}, f, indent=1)


if __name__ == "__main__":
    main()
