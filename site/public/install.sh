#!/bin/sh
# Installs Tov (https://tov.sh).
#
#   curl -fsSL https://tov.sh/install.sh | sh
#
# On macOS (Apple silicon) and Linux (x86-64) it downloads the latest release from
# https://github.com/richiemcilroy/Tov/releases: the compiler, into ~/.tov/bin, and Tov's
# JavaScript engine, for programs that import npm packages, into Tov's cache. Elsewhere, or with
# TOV_FROM_SOURCE=1, it builds the compiler from source (needs git; installs Rust with rustup if
# it's missing) and puts it in ~/.cargo/bin.
#
# Either way Tov needs a C compiler (clang or gcc), which it builds programs with. Run it again
# to update. TOV_VERSION=v0.1.0 picks a release; TOV_NPM=0 leaves out the engine (it can be
# built later with scripts/jsc/build.sh); TOV_DIR sets where the source goes (default
# ~/.tov/src).
set -eu

say() { printf '%s\n' "$*"; }
start_path=":$PATH:"
fail() { printf 'tov install: %s\n' "$*" >&2; exit 1; }

if ! command -v cc > /dev/null 2>&1 && ! command -v clang > /dev/null 2>&1 && ! command -v gcc > /dev/null 2>&1; then
    case "$(uname -s)" in
        Darwin) fail "a C compiler is needed: run \`xcode-select --install\`, then run this again" ;;
        *) fail "a C compiler is needed: install gcc or clang (e.g. \`sudo apt install build-essential\`), then run this again" ;;
    esac
fi

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) plat=darwin-arm64 ;;
    Linux-x86_64) plat=linux-x86_64 ;;
    *) plat="" ;;
esac

finish() { # where tov is, and how to start
    say ""
    say "Tov is installed: $1"
    case "$start_path" in
        *":$2:"*) ;;
        *) say "Add $2 to your PATH so your shell finds tov, e.g. in ~/.zshrc or ~/.bashrc:"
           say "  export PATH=\"$2:\$PATH\"" ;;
    esac
    say ""
    say "  tov run hello.tov        build and run"
    say "  tov check --json src/    errors and their fixes, for your agent"
    say "  tov build app.tov -o app one native binary"
    say ""
    say "The language, for you and your agent: https://tov.sh/llms.txt"
}

if [ -n "$plat" ] && [ "${TOV_FROM_SOURCE:-}" != 1 ]; then
    base=${TOV_DOWNLOAD:-https://github.com/richiemcilroy/Tov/releases}
    case "${TOV_VERSION:-latest}" in
        latest) url="$base/latest/download" ;;
        *) url="$base/download/$TOV_VERSION" ;;
    esac
    command -v curl > /dev/null 2>&1 || fail "curl is needed: install it, then run this again"
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT INT TERM
    sha() { if command -v sha256sum > /dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
    fetch() { # an archive and its checksum, checked
        curl -fsSL "$url/$1" -o "$tmp/$1" || fail "couldn't download $url/$1"
        curl -fsSL "$url/$1.sha256" -o "$tmp/$1.sha256" || fail "couldn't download $url/$1.sha256"
        [ "$(sha "$tmp/$1")" = "$(cut -d' ' -f1 < "$tmp/$1.sha256")" ] || fail "$1 doesn't match its checksum: run this again"
    }
    say "Downloading tov ($plat)..."
    fetch "tov-$plat.tar.gz"
    bin="${TOV_HOME:-$HOME/.tov}/bin"
    mkdir -p "$bin" "$tmp/tov"
    tar -xzf "$tmp/tov-$plat.tar.gz" -C "$tmp/tov"
    mv -f "$tmp/tov/tov" "$bin/tov"
    if [ "${TOV_NPM:-1}" != 0 ]; then
        say "Downloading Tov's JavaScript engine, for npm packages..."
        fetch "tov-jsc-$plat.tar.gz"
        jsc="${XDG_CACHE_HOME:-$HOME/.cache}/tov/jsc/$plat"
        rm -rf "$jsc.tmp"
        mkdir -p "$jsc.tmp"
        tar -xzf "$tmp/tov-jsc-$plat.tar.gz" -C "$jsc.tmp"
        rm -rf "$jsc"
        mv "$jsc.tmp" "$jsc"
    fi
    # (an earlier install built from source: this one goes first only if its directory does)
    if [ -x "$HOME/.cargo/bin/tov" ]; then
        say ""
        say "Note: $HOME/.cargo/bin/tov is an earlier install, built from source; remove it (cargo uninstall tov)"
        say "or put $bin ahead of $HOME/.cargo/bin in your PATH."
    fi
    finish "$bin/tov" "$bin"
    exit 0
fi

command -v git > /dev/null 2>&1 || fail "git is needed: install it, then run this again"
if ! command -v cargo > /dev/null 2>&1; then
    if [ -x "$HOME/.cargo/bin/cargo" ]; then
        PATH="$HOME/.cargo/bin:$PATH"
    else
        say "Installing Rust (rustup), which builds Tov's compiler..."
        curl -fsSL https://sh.rustup.rs | sh -s -- -y -q --profile minimal > /dev/null
        PATH="$HOME/.cargo/bin:$PATH"
    fi
fi

# (the compiler needs Rust 1.95 or newer: a rustup install updates, any other says what to do)
rust_minor=$(rustc --version 2>/dev/null | sed -n 's/^rustc 1\.\([0-9]*\).*/\1/p')
if [ -n "$rust_minor" ] && [ "$rust_minor" -lt 95 ]; then
    if command -v rustup > /dev/null 2>&1; then
        say "Updating Rust (Tov needs 1.95 or newer)..."
        rustup update stable -q > /dev/null
    else
        fail "Tov needs Rust 1.95 or newer, and this rustc is 1.$rust_minor: update it (or install rustup from https://rustup.rs), then run this again"
    fi
fi

dir="${TOV_DIR:-$HOME/.tov/src}"
if [ -d "$dir/.git" ]; then
    say "Updating $dir..."
    git -C "$dir" pull -q --ff-only
else
    say "Downloading Tov into $dir..."
    mkdir -p "$(dirname "$dir")"
    git clone -q --depth 1 https://github.com/richiemcilroy/Tov "$dir"
fi

say "Building tov (a minute or two)..."
cargo install -q --locked --path "$dir/crates/tov"

if [ "${TOV_NPM:-}" = 1 ]; then
    say "Building Tov's JavaScript engine, for npm packages (about 10 minutes)..."
    sh "$dir/scripts/jsc/build.sh"
fi

finish "$(command -v tov 2>/dev/null || echo "$HOME/.cargo/bin/tov")" "$HOME/.cargo/bin"
