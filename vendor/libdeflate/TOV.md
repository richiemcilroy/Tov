# libdeflate decompressor, vendored

The decompressor from [libdeflate](https://github.com/ebiggers/libdeflate) 1.26 (MIT, see `COPYING`) decodes `Content-Encoding: gzip` and `deflate` bodies that `fetch()` reads whole (`text()`, `json()`, `bytes()` and the like), at once, when they have all arrived: about twice as fast as zlib. It can't decode a stream in pieces, so bodies read in chunks (`res.body`) go through zlib (`vendor/zlib`).

The files are `libdeflate.h`, `common_defs.h`, and from `lib/`: the decompressors (`deflate_decompress.c` with `decompress_template.h`, `gzip_decompress.c`, `zlib_decompress.c`), the checksums (`adler32.c`, `crc32.c`) and what they include, `utils.c`, and the CPU feature detection and SIMD code in `lib/arm/` and `lib/x86/`. There's no compressor.

They come from the release tag `v1.26` (`libdeflate-1.26.tar.gz` from GitHub, SHA-256 `bba03fffc5538576213675ce6968fcff6ce2e67d82e4d5febea2d05f9f13cf85`), byte for byte.

The build compiles them into the archive that fetch programs link, with BoringSSL (see `vendor/boringssl/TOV.md`). `runtime/codecs.c` calls them.
