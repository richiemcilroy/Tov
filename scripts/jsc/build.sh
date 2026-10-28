#!/bin/sh
# Builds Tov's JavaScriptCore: WebKit's JSCOnly port at a pinned release, with Tov's patches
# (scripts/jsc/patches), as one static library and its headers in
#   ~/.cache/tov/jsc/<os>-<arch>/{lib/libtovjsc.a, include/JavaScriptCore/*.h, VERSION, order.txt (Linux)}
# which `tov build` links programs that import npm packages with (TOV_JSC=system uses the
# system's instead). On macOS it builds here; `scripts/jsc/build.sh linux` builds Linux's in a
# container (scripts/jsc/Dockerfile.linux).
set -eu
TAG=webkitgtk-2.54.1
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cache=${TOV_JSC_CACHE:-$HOME/.cache/tov/jsc}
mkdir -p "$cache"

if [ "${1:-}" = linux ]; then
    docker build -q -t tov-jsc-linux -f "$here/Dockerfile.linux" "$here" > /dev/null
    exec docker run --rm -v "$root:/tov" -v "$cache:/jsc" -e TOV_JSC_CACHE=/jsc tov-jsc-linux sh /tov/scripts/jsc/build.sh
fi

src="$cache/src-$TAG"
if [ ! -d "$src/Source/JavaScriptCore" ]; then
    rm -rf "$src.tmp"
    git clone --quiet --filter=blob:none --no-checkout --depth 1 --branch "$TAG" https://github.com/WebKit/WebKit.git "$src.tmp"
    git -C "$src.tmp" sparse-checkout init --cone
    git -C "$src.tmp" sparse-checkout set Source/JavaScriptCore Source/WTF Source/bmalloc Source/cmake \
        Source/ThirdParty/capstone Source/ThirdParty/unifdef Source/ThirdParty/gtest Tools/Scripts
    git -C "$src.tmp" checkout --quiet
    mv "$src.tmp" "$src"
fi
# the patches, on a clean tree
git -C "$src" checkout --quiet -- .
for p in "$here"/patches/*.patch; do
    [ -f "$p" ] && git -C "$src" apply "$p"
done
cp "$here"/patches/*.cpp "$here"/patches/*.h "$src/Source/JavaScriptCore/API/" 2>/dev/null || true

# what the bytecode cache's format is versioned by (the build's, not the program binary's)
version=$( (echo "$TAG"; cat "$here"/patches/* "$here"/include/*/*) | cksum | cut -d' ' -f1)
os=$(uname -s | tr A-Z a-z)
arch=$(uname -m)
[ "$arch" = aarch64 ] && arch=arm64
# TOV_JSC_PGO=gen builds an engine that records a profile of where it spends its time
# (programs linked with TOV_JSC_DIR=<cache>/<os>-<arch>-gen and TOV_CFLAGS=-fprofile-instr-generate
# write it as they run); TOV_JSC_PGO=<merged .profdata> builds the engine optimized for it.
pgo=${TOV_JSC_PGO:-}
suffix=""
[ "$pgo" = gen ] && suffix="-gen"
build="$cache/build-$os-$arch$suffix"
# (macOS: libpthread's private header, for the engine's fast thread-local slots: include/)
flags="-ffunction-sections -fdata-sections"
[ "$os" = darwin ] && flags="$flags -I$here/include"
if [ "$pgo" = gen ]; then
    flags="$flags -fprofile-instr-generate"
elif [ -n "$pgo" ]; then
    flags="$flags -fprofile-instr-use=$pgo -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin"
fi
out="$cache/$os-$arch$suffix"
# (ThinLTO on macOS, whose packaging below merges the bitcode into machine code; Linux's doesn't
# yet, so its objects stay machine code)
lto="-DLTO_MODE="
[ "$os" = darwin ] && lto="-DLTO_MODE=thin"
# (ThinLTO: the objects are bitcode, optimized across each other when they're merged below, so
# a program's own link stays an ordinary one)
cmake -S "$src" -B "$build" -G Ninja -DPORT=JSCOnly -DCMAKE_BUILD_TYPE=Release -DENABLE_STATIC_JSC=ON \
    -DUSE_THIN_ARCHIVES=OFF -DENABLE_FTL_JIT=ON -DDEVELOPER_MODE=OFF -DENABLE_API_TESTS=OFF -DENABLE_TOOLS=OFF \
    -DUSE_CXX_STDLIB_ASSERTIONS=OFF $lto -DUSE_LD_LLD=OFF \
    -DCMAKE_C_FLAGS="$flags" \
    -DCMAKE_CXX_FLAGS="$flags -DTOV_JSC_CACHE_VERSION=${version}u" > "$cache/configure-$os-$arch.log"
nice -n 10 cmake --build "$build" --target JavaScriptCore JavaScriptCoreJIT > "$cache/build-$os-$arch.log"

# one archive: JavaScriptCore, its JIT's objects (an object library), WTF and bmalloc
rm -rf "$out.tmp"
mkdir -p "$out.tmp/lib" "$out.tmp/include/JavaScriptCore"
jit=$(find "$build/Source/JavaScriptCore/CMakeFiles/JavaScriptCoreJIT.dir" -name '*.o')
if [ "$os" = darwin ]; then
    # The interpreters (LLInt, IPInt) are assembly, compiled to machine code on their own, outside
    # the link-time optimization.
    llint="$out.tmp/LowLevelInterpreter.o"
    clang -c -O3 -x ir "$build/Source/JavaScriptCore/CMakeFiles/LowLevelInterpreterLib.dir/llint/LowLevelInterpreter.cpp.o" -o "$llint"
    cp "$build/lib/libJavaScriptCore.a" "$out.tmp/libJavaScriptCore.a"
    ar -d "$out.tmp/libJavaScriptCore.a" LowLevelInterpreter.cpp.o
    # the rest, optimized as one program (ThinLTO) into one object of machine code: all of the
    # engine, and what it uses of WTF and bmalloc (which hold alternatives for other platforms),
    # keeping what the interpreters, outside it, call (else they're the engine's own, and unused,
    # to the optimizer)
    nm -u "$llint" | sed 's/^/-Wl,-u,/' > "$out.tmp/llint-uses"
    # (laid out in the order of the functions programs run as they start and serve: Linux's
    # profile, with the leading underscore of macOS's symbols, as the engine's C++ names are
    # otherwise the same. A program's link keeps an object's order, so starting, it pages in less
    # of the engine's code, at no cost to each link that an order file there would add.)
    sed 's/^/_/' "$here/order-linux.txt" > "$out.tmp/order"
    clang -r -nostdlib -flto=thin -O3 -Wl,-mllvm,-import-instr-limit=20 -Wl,-keep_private_externs @"$out.tmp/llint-uses" -Wl,-force_load,"$out.tmp/libJavaScriptCore.a" $jit "$build/lib/libWTF.a" "$build/lib/libbmalloc.a" \
        -Wl,-order_file,"$out.tmp/order" -Wl,-cache_path_lto,"$cache/lto-cache-$os-$arch$suffix" -o "$out.tmp/optimized.o"
    # Then the interpreters merged in, and only what's called from outside the engine left
    # global: its C API, Tov's (TovAPI), and the functions the runtime calls by name
    # (runtime/js.c). The rest is local to the object, so a program's link resolves a few hundred
    # global symbols, not ~120,000.
    nm -gUm "$out.tmp/optimized.o" | grep -v 'private external' | awk '{print $NF}' | grep -E '^_(JS|kJS|TV)' > "$out.tmp/exports"
    grep -ho '"_Z[A-Za-z0-9_]*"' "$root"/runtime/*.c | tr -d '"' | sed 's/^/_/' | sort -u >> "$out.tmp/exports"
    ld -r "$out.tmp/optimized.o" "$llint" -exported_symbols_list "$out.tmp/exports" -o "$out.tmp/engine.o"
    # The interpreters' handlers are found at offsets from each other, in a section of their own
    # (__jsc_int): a program linked with -dead_strip mustn't split it at its symbols and drop those
    # no symbol names. The object comes out without MH_SUBSECTIONS_VIA_SYMBOLS (the interpreters'
    # didn't have it), so nothing at all would be dropped: it's set again, and the section's symbols
    # but its first made alternate entries (N_ALT_ENTRY), which don't start an atom of their own,
    # so the section is one.
    python3 - "$out.tmp/engine.o" <<'PY'
import struct, sys
b = bytearray(open(sys.argv[1], "rb").read())
magic, _, _, filetype, ncmds, _, flags = struct.unpack_from("<IiiIIII", b, 0)
assert magic == 0xfeedfacf and filetype == 1
struct.pack_into("<I", b, 24, flags | 0x2000)
off, sect, n, interp, symtab = 32, 0, 0, None, None
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", b, off)
    if cmd == 0x19:  # LC_SEGMENT_64
        for i in range(struct.unpack_from("<I", b, off + 64)[0]):
            s = off + 72 + i * 80
            sect += 1
            if b[s:s + 16].rstrip(b"\0") == b"__jsc_int":
                interp = (sect, struct.unpack_from("<Q", b, s + 32)[0])
    elif cmd == 0x2:  # LC_SYMTAB
        symtab = struct.unpack_from("<II", b, off + 8)
    off += size
assert interp and symtab
for i in range(symtab[1]):
    s = symtab[0] + i * 16
    ntype, nsect, ndesc, value = struct.unpack_from("<BBHQ", b, s + 4)
    if nsect == interp[0] and not ntype & 0xe0 and value > interp[1]:
        struct.pack_into("<H", b, s + 6, ndesc | 0x200)
        n += 1
assert n > 100
open(sys.argv[1], "wb").write(b)
PY
    libtool -static -o "$out.tmp/lib/libtovjsc.a" "$out.tmp/engine.o" 2> /dev/null
    rm "$llint" "$out.tmp/llint-uses" "$out.tmp/order" "$out.tmp/exports" "$out.tmp/libJavaScriptCore.a" "$out.tmp/optimized.o" "$out.tmp/engine.o"
    headers="$build/JavaScriptCore.framework/Headers"
else
    {
        echo "CREATE $out.tmp/lib/libtovjsc.a"
        for a in libJavaScriptCore.a libWTF.a libbmalloc.a; do echo "ADDLIB $build/lib/$a"; done
        for o in $jit; do echo "ADDMOD $o"; done
        echo SAVE
        echo END
    } | ar -M
    headers="$build/JavaScriptCore/Headers/JavaScriptCore"
fi
cp "$headers"/*.h "$out.tmp/include/JavaScriptCore/"
echo "$TAG $version" > "$out.tmp/VERSION"
# (Linux: the order lld lays out the functions that run as programs start and serve in; macOS's
# engine object is laid out in it above)
[ "$os" = linux ] && cp "$here/order-linux.txt" "$out.tmp/order.txt"
rm -rf "$out"
mv "$out.tmp" "$out"
echo "built $out ($(du -sh "$out/lib/libtovjsc.a" | cut -f1))"
