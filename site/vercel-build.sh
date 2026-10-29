#!/bin/sh
# Builds tov.sh on Vercel: the Tov compiler from this repository, then the site with it
# (site/build.tov writes site/dist, which Vercel serves). Vercel's build image has neither Rust nor,
# always, a C compiler: this installs what's missing.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)

if ! command -v cc > /dev/null 2>&1; then
    dnf install -y -q gcc > /dev/null
fi
# (the compiler needs Rust 1.95 or newer, and the build image's may be older or missing)
rust_minor=$(rustc --version 2>/dev/null | sed -n 's/^rustc 1\.\([0-9]*\).*/\1/p')
if [ -z "$rust_minor" ] || [ "$rust_minor" -lt 95 ]; then
    if command -v rustup > /dev/null 2>&1; then
        rustup -q toolchain install stable --profile minimal > /dev/null
        rustup -q default stable > /dev/null
    else
        curl -sSf https://sh.rustup.rs | sh -s -- -y -q --profile minimal > /dev/null
    fi
    if [ -f "$HOME/.cargo/env" ]; then . "$HOME/.cargo/env"; fi
fi

# (the site makes no HTTPS requests: the TLS libraries the compiler would carry aren't needed)
TOV_NO_PREBUILT_VENDOR=1 cargo build --release -q --manifest-path "$root/Cargo.toml" -p tov
cd "$here"
"$root/target/release/tov" build.tov
