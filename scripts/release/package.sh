#!/bin/sh
# Packages a release's files for this machine's platform into target/dist:
#   tov-<os>-<arch>.tar.gz        the compiler (`tov`), built here
#   tov-jsc-<os>-<arch>.tar.gz    Tov's JavaScript engine, for npm packages (scripts/jsc/build.sh;
#                                 on Linux the release's, from `build.sh linux-release`)
# each with a .sha256 beside it. site/public/install.sh downloads them from the GitHub release.
#
#   scripts/release/package.sh [engine-dir]
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
os=$(uname -s | tr A-Z a-z)
arch=$(uname -m)
[ "$arch" = aarch64 ] && arch=arm64
plat="$os-$arch"
cache=${XDG_CACHE_HOME:-$HOME/.cache}/tov/jsc
case "$os" in
    darwin) engine=${1:-$cache/$plat} ;;
    *) engine=${1:-$cache/release/$plat} ;;
esac
[ -f "$engine/lib/libtovjsc.a" ] || { echo "no engine in $engine (scripts/jsc/build.sh$( [ "$os" = linux ] && echo ' linux-release'))" >&2; exit 1; }
if [ "$os" = linux ] && [ ! -f "$engine/lib/libicuuc.a" ]; then
    echo "$engine has no ICU of its own: build the release's engine (scripts/jsc/build.sh linux-release)" >&2
    exit 1
fi

out="$root/target/dist"
mkdir -p "$out"
cd "$root"
# (macOS 11 and up, as the engine)
MACOSX_DEPLOYMENT_TARGET=11.0 cargo build --release --locked -q -p tov

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT INT TERM
mkdir -p "$stage/tov" "$stage/jsc"
cp target/release/tov "$stage/tov/tov"
strip "$stage/tov/tov"
# (macOS: stripping voids the linker's ad-hoc signature, without which arm64 won't run it)
[ "$os" = darwin ] && codesign -s - -f "$stage/tov/tov" 2> /dev/null
cp LICENSE "$stage/tov/"
tar -C "$stage/tov" -czf "$out/tov-$plat.tar.gz" .
cp -R "$engine/include" "$engine/lib" "$engine/VERSION" "$stage/jsc/"
[ -f "$engine/order.txt" ] && cp "$engine/order.txt" "$stage/jsc/"
tar -C "$stage/jsc" -czf "$out/tov-jsc-$plat.tar.gz" .
for f in "tov-$plat.tar.gz" "tov-jsc-$plat.tar.gz"; do
    (cd "$out" && { command -v sha256sum > /dev/null && sha256sum "$f" || shasum -a 256 "$f"; } > "$f.sha256")
done
ls -la "$out"
