/* tov.c — the Tov runtime (C11, libc + libm only).
 *
 * Generated programs #include "tov.h" and this file into a single translation unit,
 * so everything that is not part of the tov.h contract is `static` and prefixed `tv_`.
 * See tov.h for ownership conventions.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   /* POSIX and BSD interfaces (sigaction, MAP_ANON, nanosleep) under -std=c11 */
#endif
#include "tov.h"

#include <float.h>
#include <math.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#define TV_HAVE_ISATTY 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define TV_LIKELY(x) __builtin_expect(!!(x), 1)
#define TV_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define TV_LIKELY(x) (x)
#define TV_UNLIKELY(x) (x)
#endif

/* ================================================================== globals */

int tv_argc;
char **tv_argv;

static jmp_buf *tv_test_jmp;       /* non-NULL while a test is running */
/* Unwinds to the running test (set by tv_test_run, so programs without tests don't link longjmp). */
void (*tv_test_unwind)(void); /* not static: the compiler would call its only value directly */
static tv_sb tv_test_msg;          /* failure message of the running test */
static const char *tv_test_loc;
static int64_t tv_tests_passed, tv_tests_failed;

/* ================================================================== traps and memory */

/* Output goes straight to the file descriptors (the runtime buffers stdout itself, and stderr
 * is unbuffered anyway): no stdio in the paths every program links. */
__attribute__((noinline)) void tv_write_fd(int fd, const char *s, size_t n) {
#if defined(__unix__) || defined(__APPLE__)
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
#else
    FILE *f = fd == 2 ? stderr : stdout;
    fwrite(s, 1, n, f);
    fflush(f);
#endif
}

__attribute__((noinline)) void tv_err_cstr(const char *s) { tv_write_fd(2, s, strlen(s)); }

static size_t tv_fmt_i64(char *buf, int64_t v);

static _Noreturn void tv_oom(size_t size) {
    tv_out_flush();
    char num[24];
    size_t n = tv_fmt_i64(num, (int64_t)size);
    tv_err_cstr("trap: out of memory (allocating ");
    tv_write_fd(2, num, n);
    tv_err_cstr(" bytes)\n");
    exit(101);
}

/* `size` bytes of zeros straight from the kernel: its pages are touched (and counted) only as
 * they're written, unlike malloc's, which zeroing would touch at once. Never freed: for tables
 * made at first use. */
static void *tv_zero_pages(size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (TV_UNLIKELY(p == MAP_FAILED)) tv_oom(size);
    return p;
}

_Noreturn void tv_trap(const char *msg, const char *loc) {
    if (tv_test_jmp) {
        tv_test_msg.len = 0;
        tv_sb_push_cstr(&tv_test_msg, "trap: ");
        tv_sb_push_cstr(&tv_test_msg, msg);
        tv_test_loc = loc;
        tv_test_unwind();
    }
    tv_out_flush();
    tv_err_cstr("trap: ");
    tv_err_cstr(msg);
    tv_err_cstr("\n");
    if (loc && *loc) {
        tv_err_cstr("  at ");
        tv_err_cstr(loc);
        tv_err_cstr("\n");
    }
    exit(101);
}

void *tv_alloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (TV_UNLIKELY(!p)) tv_oom(size);
    return p;
}

void *tv_realloc(void *p, size_t size) {
    void *q = realloc(p, size ? size : 1);
    if (TV_UNLIKELY(!q)) tv_oom(size);
    return q;
}

void tv_free(void *p) { free(p); }

/* a * b + c, trapping on overflow (allocation sizes). */
static size_t tv_size_mul_add(size_t a, size_t b, size_t c) {
    if (b && a > (SIZE_MAX - c) / b) tv_trap("allocation size overflow", NULL);
    return a * b + c;
}

/* ================================================================== string builder */

/* A builder's buffer is laid out as a tv_strbuf (header, bytes, room for a NUL) so that
 * tv_str_from_sb can adopt it without copying. sb->data points at the bytes. */
#define TV_STR_HDR offsetof(tv_strbuf, data)

/* Short builders (most: a number, a header line, a small JSON body) live in the small-object
 * heap; a buffer's size (so where it lives) follows from its capacity. */
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
#define TV_SB_SMALL(cap) false
#else
#define TV_SB_SMALL(cap) (TV_STR_HDR + (cap) + 1 <= (TV_SMALL_CLASSES - 1) * 8)
#endif
/* Big builders (a response body, a large JSON text) on macOS get their pages straight from the
 * kernel, and give them straight back: its allocator keeps freed large blocks dirty for reuse,
 * so a burst of big buffers would stay in the program's footprint. (glibc maps blocks this
 * big itself.) */
#if defined(__APPLE__) && !(defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer)))
#include <malloc/malloc.h>
#define TV_SB_MAPS 1
#define TV_SB_MAPPED(cap) ((cap) >= ((size_t)128 << 10))
/* A mapped builder holding this much becomes its string as it is (tv_str_from_sb); the string is
 * unmapped when released (tv_str_release_slow: malloc doesn't own it). Less is copied out. */
#define TV_STR_ADOPT_MAPPED ((size_t)64 << 10)
#define TV_STR_MAYBE_MAPPED(size) ((size) >= TV_STR_ADOPT_MAPPED)
static size_t tv_sb_map_size(size_t cap) {
    size_t page = (size_t)getpagesize();
    return (TV_STR_HDR + cap + 1 + page - 1) & ~(page - 1);
}
/* Freed mapped buffers are kept (up to TV_SB_CACHE_MAX bytes) for the next to reuse, warm: a
 * fresh mapping's pages are zeroed and faulted in one by one, ~350 µs of an 8 MB body's ~1.1 ms.
 * The event loop unmaps them once the program has been idle a second (tv_sb_cache_flush). */
#include <os/lock.h>
#define TV_SB_CACHE_MAX ((size_t)32 << 20)
#define TV_SB_CACHE_SLOTS 8
static struct { char *p; size_t size; uint64_t age; } tv_sb_cache[TV_SB_CACHE_SLOTS];
static size_t tv_sb_cache_bytes;
static os_unfair_lock tv_sb_cache_lock = OS_UNFAIR_LOCK_INIT;

/* A cached mapping of `size` to `most` bytes, cut down to `size`, or NULL. (The smallest that
 * fits; what's past `size` goes back.) */
static char *tv_sb_cache_take(size_t size, size_t most) {
    if (!tv_sb_cache_bytes) return NULL;
    char *p = NULL;
    size_t have = 0;
    os_unfair_lock_lock(&tv_sb_cache_lock);
    int best = -1;
    for (int i = 0; i < TV_SB_CACHE_SLOTS; i++)
        if (tv_sb_cache[i].p && tv_sb_cache[i].size >= size && tv_sb_cache[i].size <= most && (best < 0 || tv_sb_cache[i].size < tv_sb_cache[best].size)) best = i;
    if (best >= 0) {
        p = tv_sb_cache[best].p;
        have = tv_sb_cache[best].size;
        tv_sb_cache[best].p = NULL;
        tv_sb_cache_bytes -= have;
    }
    os_unfair_lock_unlock(&tv_sb_cache_lock);
    if (p && have > size) munmap(p + size, have - size);
    return p;
}

static char *tv_sb_map(size_t cap) {
    size_t size = tv_sb_map_size(cap);
    /* (up to twice the size: a buffer growing by doubling finds each of its sizes again, rather
     * than cutting down the biggest) */
    char *c = tv_sb_cache_take(size, 2 * size);
    if (c) return c;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (TV_UNLIKELY(p == MAP_FAILED)) tv_oom(cap);
    return (char *)p;
}

/* Unmaps `size` bytes at p (page-rounded, from tv_sb_map), or keeps them for reuse. A full
 * cache makes room by unmapping its oldest: the sizes just freed are the ones about to be
 * asked for again, and one that no longer fits anything would otherwise hold its place. */
static void tv_sb_unmap(char *p, size_t size) {
    char *drop[TV_SB_CACHE_SLOTS];
    size_t drop_size[TV_SB_CACHE_SLOTS];
    int ndrop = 0;
    if (size <= TV_SB_CACHE_MAX / 2) {
        static uint64_t clock;
        os_unfair_lock_lock(&tv_sb_cache_lock);
        for (;;) {
            int free_slot = -1, oldest = -1;
            for (int i = 0; i < TV_SB_CACHE_SLOTS; i++) {
                if (!tv_sb_cache[i].p) { if (free_slot < 0) free_slot = i; }
                else if (oldest < 0 || tv_sb_cache[i].age < tv_sb_cache[oldest].age) oldest = i;
            }
            if (free_slot >= 0 && tv_sb_cache_bytes + size <= TV_SB_CACHE_MAX) {
                tv_sb_cache[free_slot].p = p;
                tv_sb_cache[free_slot].size = size;
                tv_sb_cache[free_slot].age = ++clock;
                tv_sb_cache_bytes += size;
                p = NULL;
                break;
            }
            drop[ndrop] = tv_sb_cache[oldest].p;
            drop_size[ndrop++] = tv_sb_cache[oldest].size;
            tv_sb_cache_bytes -= tv_sb_cache[oldest].size;
            tv_sb_cache[oldest].p = NULL;
        }
        os_unfair_lock_unlock(&tv_sb_cache_lock);
    }
    while (ndrop--) munmap(drop[ndrop], drop_size[ndrop]);
    if (p) munmap(p, size);
}

/* Grows the mapping at p from `have` bytes to `size` in place, without copying its pages, when
 * the address space after it is free (a hint, which the kernel takes when it can); NULL if not.
 * (No other imports: each costs every program's binary its stub, fixup and symbol.) */
static char *tv_sb_remap(char *p, size_t have, size_t size) {
    char *end = p + have;
    void *q = mmap(end, size - have, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (q == (void *)end) return p;
    if (q != MAP_FAILED) munmap(q, size - have);
    return NULL;
}

static void tv_sb_cache_flush(void) {
    os_unfair_lock_lock(&tv_sb_cache_lock);
    for (int i = 0; i < TV_SB_CACHE_SLOTS; i++)
        if (tv_sb_cache[i].p) {
            munmap(tv_sb_cache[i].p, tv_sb_cache[i].size);
            tv_sb_cache[i].p = NULL;
        }
    tv_sb_cache_bytes = 0;
    os_unfair_lock_unlock(&tv_sb_cache_lock);
}
#else
#define TV_SB_MAPPED(cap) false
#define TV_STR_ADOPT_MAPPED SIZE_MAX
#define TV_STR_MAYBE_MAPPED(size) false
static const size_t tv_sb_cache_bytes = 0;
static void tv_sb_cache_flush(void) {}
static void tv_sb_unmap(char *p, size_t size) { (void)size; free(p); }  /* (never mapped here) */
static size_t tv_sb_map_size(size_t cap) { return cap; }
static char *tv_sb_map(size_t cap) { return (char *)tv_alloc(cap); }
static char *tv_sb_cache_take(size_t size, size_t most) { (void)size; (void)most; return NULL; }
static char *tv_sb_remap(char *p, size_t have, size_t size) { (void)p; (void)have; (void)size; return NULL; }
#endif

static inline void tv_sb_release_buf(char *base, size_t cap) {
    if (TV_SB_SMALL(cap)) {
        size_t c = (TV_STR_HDR + cap + 1 + 7) >> 3;
        *(void **)(void *)base = tv_small_bins[c];
        tv_small_bins[c] = base;
    } else if (TV_SB_MAPPED(cap)) {
        tv_sb_unmap(base, tv_sb_map_size(cap));
    } else {
        free(base);
    }
}

void tv_sb_grow(tv_sb *sb, size_t need) {
    size_t cap = sb->cap ? sb->cap * 2 : 32;
    if (cap < need) cap = need;
    if (cap > SIZE_MAX - TV_STR_HDR - 1) tv_trap("string too long", NULL);
    /* (a builder on the stack: its bytes are copied to a block of its own, the stack left be) */
    char *stack = tv_sb_is_stack(sb) ? sb->data : NULL;
    char *old = sb->data && !stack ? sb->data - TV_STR_HDR : NULL;
    char *base;
    if (TV_SB_SMALL(cap)) {
        size_t c = (TV_STR_HDR + cap + 1 + 7) >> 3;
        void **f = tv_small_bins[c];
        if (f) tv_small_bins[c] = *f;
        else f = tv_small_refill(c);
        base = (char *)f;
        if (old) memcpy(base + TV_STR_HDR, sb->data, sb->len);
        if (old) tv_sb_release_buf(old, sb->cap);
    } else if (TV_SB_MAPPED(cap)) {
        /* (one just its size that's cached, rather than growing in place, as a body that began
         * arriving before it was read grows to its length: copying what's written costs less
         * than faulting in fresh pages, and the cached one would otherwise sit unused) */
        size_t size = tv_sb_map_size(cap);
        bool was_mapped = old && TV_SB_MAPPED(sb->cap);
        base = was_mapped ? tv_sb_cache_take(size, size + size / 8) : NULL;
        if (!base && was_mapped && (base = tv_sb_remap(old, tv_sb_map_size(sb->cap), size))) old = NULL;
        if (!base) base = tv_sb_map(cap);
        if (old) {
            memcpy(base + TV_STR_HDR, sb->data, sb->len);
            /* (an outgrown mapped buffer goes back, not into the cache: a builder doubling to
             * 8 MB would keep its every smaller size) */
            if (TV_SB_MAPPED(sb->cap)) munmap(old, tv_sb_map_size(sb->cap));
            else tv_sb_release_buf(old, sb->cap);
        }
        cap = tv_sb_map_size(cap) - TV_STR_HDR - 1;   /* (the whole of its pages) */
    } else if (old && TV_SB_SMALL(sb->cap)) {
        base = (char *)tv_alloc(TV_STR_HDR + cap + 1);
        memcpy(base + TV_STR_HDR, sb->data, sb->len);
        tv_sb_release_buf(old, sb->cap);
    } else {
        base = (char *)tv_realloc(old, TV_STR_HDR + cap + 1);
    }
    if (stack && sb->len) memcpy(base + TV_STR_HDR, stack, sb->len);
    /* (a block's count isn't the stack mark, whatever was in it: see tv_sb_is_stack) */
    ((tv_strbuf *)(void *)base)->rc = 0;
    sb->data = base + TV_STR_HDR;
    sb->cap = cap;
}

static inline char *tv_sb_reserve(tv_sb *sb, size_t extra) {
    if (TV_UNLIKELY(sb->cap - sb->len < extra)) {
        if (extra > SIZE_MAX - sb->len) tv_trap("string too long", NULL);
        tv_sb_grow(sb, sb->len + extra);
    }
    return sb->data + sb->len;
}

void tv_sb_push_slow(tv_sb *sb, const char *s, size_t n) {
    if (!n) return;
    tv_copy(tv_sb_reserve(sb, n), s, n);
    sb->len += n;
}

void tv_sb_push_cstr(tv_sb *sb, const char *s) { tv_sb_push(sb, s, strlen(s)); }

void tv_sb_push_char(tv_sb *sb, char c) {
    *tv_sb_reserve(sb, 1) = c;
    sb->len++;
}

void tv_sb_free(tv_sb *sb) {
    if (sb->data && !tv_sb_is_stack(sb)) tv_sb_release_buf(sb->data - TV_STR_HDR, sb->cap);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

void tv_sb_push_str(tv_sb *sb, tv_str s) { tv_sb_push(sb, s.p->data, (size_t)s.p->len); }

/* ================================================================== hashing */

static inline uint64_t tv_mix64(uint64_t x) { /* splitmix64 finalizer */
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}


static inline uint64_t tv_mum(uint64_t a, uint64_t b) {
#ifdef __SIZEOF_INT128__
    __extension__ typedef unsigned __int128 tv_u128;
    tv_u128 r = (tv_u128)a * b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
#else
    uint64_t ha = a >> 32, la = (uint32_t)a, hb = b >> 32, lb = (uint32_t)b;
    uint64_t rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb;
    uint64_t t = rl + (rm0 << 32), c = t < rl;
    uint64_t lo = t + (rm1 << 32);
    c += lo < t;
    uint64_t hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
    return lo ^ hi;
#endif
}

static inline uint64_t tv_r64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint64_t tv_r32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* wyhash-style byte hash. */
static uint64_t tv_hash_bytes(const void *data, size_t len) {
    const uint64_t s0 = 0xa0761d6478bd642fULL, s1 = 0xe7037ed1a0b428dbULL, s2 = 0x8ebc6af09c88c6e3ULL;
    const unsigned char *p = (const unsigned char *)data;
    uint64_t seed = 0x243f6a8885a308d3ULL ^ tv_mum(len ^ s0, s1);
    uint64_t a, b;
    if (len <= 16) {
        if (len >= 8) {
            a = tv_r64(p);
            b = tv_r64(p + len - 8);
        } else if (len >= 4) {
            a = tv_r32(p);
            b = tv_r32(p + len - 4);
        } else if (len > 0) {
            a = ((uint64_t)p[0] << 16) | ((uint64_t)p[len >> 1] << 8) | p[len - 1];
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        size_t n = len;
        while (n > 16) {
            seed = tv_mum(tv_r64(p) ^ s1, tv_r64(p + 8) ^ seed);
            p += 16;
            n -= 16;
        }
        a = tv_r64(p + n - 16);
        b = tv_r64(p + n - 8);
    }
    return tv_mum(s1 ^ len, tv_mum(a ^ s2, b ^ seed));
}

/* ================================================================== sorting */

/* LSD radix sort of 64-bit keys, 11-bit digits, skipping digits every key shares.
 * `idx` (optional) is permuted alongside the keys. Stable. */
void tv_radix64(uint64_t *keys, int64_t *idx, tv_int n) {
    if (n < 2) return;
    enum { BITS = 11, B = 1 << BITS, PASSES = 6 };
    size_t *hist = calloc((size_t)PASSES * B, sizeof(size_t));
    if (!hist) tv_trap("out of memory", "sort");
    for (tv_int i = 0; i < n; i++) {
        uint64_t k = keys[i];
        for (int p = 0; p < PASSES; p++) hist[p * B + ((k >> (p * BITS)) & (B - 1))]++;
    }
    uint64_t *k2 = tv_alloc((size_t)n * sizeof(uint64_t));
    int64_t *i2 = idx ? tv_alloc((size_t)n * sizeof(int64_t)) : NULL;
    uint64_t *ks = keys, *kd = k2;
    int64_t *is = idx, *id = i2;
    for (int p = 0; p < PASSES; p++) {
        size_t *h = hist + p * B;
        int shift = p * BITS;
        if (h[(ks[0] >> shift) & (B - 1)] == (size_t)n) continue;
        size_t sum = 0;
        for (int b = 0; b < B; b++) { size_t c = h[b]; h[b] = sum; sum += c; }
        if (idx) {
            for (tv_int i = 0; i < n; i++) { uint64_t k = ks[i]; size_t pos = h[(k >> shift) & (B - 1)]++; kd[pos] = k; id[pos] = is[i]; }
            int64_t *ti = is; is = id; id = ti;
        } else {
            for (tv_int i = 0; i < n; i++) { uint64_t k = ks[i]; kd[h[(k >> shift) & (B - 1)]++] = k; }
        }
        uint64_t *tk = ks; ks = kd; kd = tk;
    }
    if (ks != keys) memcpy(keys, ks, (size_t)n * sizeof(uint64_t));
    if (idx && is != idx) memcpy(idx, is, (size_t)n * sizeof(int64_t));
    tv_free(k2);
    if (i2) tv_free(i2);
    free(hist);
}
/* Order-preserving 64-bit key for a double under the comparator `a - b`:
 * both zeros share a key (they compare equal), NaN sorts last. */
static inline uint64_t tv_f64_key(double x) {
    uint64_t u;
    if (x == 0) u = 0;
    else if (x != x) u = 0x7FF8000000000000ull;
    else memcpy(&u, &x, 8);
    return (u >> 63) ? ~u : (u ^ 0x8000000000000000ull);
}
/* In-place radix sort of 64-bit keys, not stable (callers use it only where equal keys are
 * indistinguishable). One in-place pass (American flag) splits the keys by the 11 highest bits
 * on which they differ; buckets then finish with LSD radix sort through a scratch buffer the
 * size of the bucket (small, in cache) — or, if a bucket is still large, recursively in place.
 * Extra memory: the largest small bucket, not a copy of the array. */
enum { TV_RX_SMALL = 1 << 15 };
static void tv_lsd_bucket(uint64_t *k, size_t n, uint64_t *tmp, uint64_t diff) {
    uint64_t *src = k, *dst = tmp;
    for (int shift = 0; shift < 64; shift += 8) {
        if (!((diff >> shift) & 255)) continue; /* a byte every key shares */
        size_t cnt[256] = {0};
        for (size_t i = 0; i < n; i++) cnt[(src[i] >> shift) & 255]++;
        size_t pos = 0;
        for (int b = 0; b < 256; b++) { size_t c = cnt[b]; cnt[b] = pos; pos += c; }
        for (size_t i = 0; i < n; i++) { uint64_t v = src[i]; dst[cnt[(v >> shift) & 255]++] = v; }
        uint64_t *t = src; src = dst; dst = t;
    }
    if (src != k) memcpy(k, src, n * sizeof *k);
}
/* Scratch for bucket sorts, grown to the largest bucket met (often a few KiB). */
typedef struct tv_rx_tmp { uint64_t *p; size_t cap; } tv_rx_tmp;
/* One American-flag pass on the digit at `shift` (counts `cnt`, bucket cursors `head`/`tail`
 * of type T), then each bucket recursively. */
#define TV_RX_PARTITION(T) \
    for (size_t i = 0; i < n; i++) cnt[(k[i] >> shift) & (B - 1)]++; \
    { T pos = 0; for (int b = 0; b < B; b++) { head[b] = pos; pos += cnt[b]; tail[b] = pos; } } \
    for (int b = 0; b < B; b++) { \
        while (head[b] < tail[b]) { \
            uint64_t v = k[head[b]]; \
            size_t d = (v >> shift) & (B - 1); \
            while (d != (size_t)b) { /* cycle: place v, pick up what was there */ \
                uint64_t t = k[head[d]]; \
                k[head[d]++] = v; \
                v = t; \
                d = (v >> shift) & (B - 1); \
            } \
            k[head[b]++] = v; \
        } \
    } \
    { size_t start = 0; \
      for (int b = 0; b < B; b++) { \
          if (shift > 0 && cnt[b] > 1) tv_radix_inplace(k + start, cnt[b], tmp); \
          start += cnt[b]; \
      } }
static void tv_radix_inplace(uint64_t *k, size_t n, tv_rx_tmp *tmp) {
    if (n < 64) {
        for (size_t i = 1; i < n; i++) {
            uint64_t v = k[i];
            size_t j = i;
            while (j > 0 && k[j - 1] > v) { k[j] = k[j - 1]; j--; }
            k[j] = v;
        }
        return;
    }
    uint64_t diff = 0, k0 = k[0];
    for (size_t i = 1; i < n; i++) diff |= k[i] ^ k0;
    if (!diff) return;
    if (n <= TV_RX_SMALL) {
        if (tmp->cap < n) {
            size_t cap = tmp->cap * 2 > n ? tmp->cap * 2 : n;
            if (cap > TV_RX_SMALL) cap = TV_RX_SMALL;
            tmp->p = tv_realloc(tmp->p, cap * sizeof(uint64_t));
            tmp->cap = cap;
        }
        tv_lsd_bucket(k, n, tmp->p, diff);
        return;
    }
    int top = 63 - __builtin_clzll(diff);
    int shift = top >= 10 ? top - 10 : 0;
    enum { B = 2048 };
    /* 32-bit counts on the stack while they fit (24 KiB, no allocation), else 64-bit on the heap */
    if (n <= UINT32_MAX) {
        uint32_t cnt[B] = {0}, head[B], tail[B];
        TV_RX_PARTITION(uint32_t)
        return;
    }
    size_t *cnt = calloc(B * 3, sizeof(size_t));
    if (!cnt) tv_trap("out of memory", "sort");
    size_t *head = cnt + B, *tail = cnt + 2 * B;
    TV_RX_PARTITION(size_t)
    free(cnt);
}
/* xs.sort((a, b) => a - b) / (b - a) on f64[]: the doubles become order-preserving keys in
 * place, are radix-sorted in place, and are decoded back. Equal keys are equal values (NaNs
 * share one key; both zeros share a key and keep their original order: their signs are
 * replayed in encounter order), so the unstable in-place sort is unobservable. No copy of the
 * array is made. */
void tv_sort_f64(double *a, tv_int n, bool desc) {
    if (n < 2) return;
    uint64_t *k = (uint64_t *)(void *)a;
    uint8_t *zs = NULL;
    tv_int nz = 0;
    for (tv_int i = 0; i < n; i++) {
        double x = a[i];
        if (x == 0) {
            if (!zs) zs = tv_alloc((size_t)n);
            zs[nz++] = signbit(x) ? 1 : 0;
        }
        uint64_t key = tv_f64_key(x);
        k[i] = desc ? ~key : key;
    }
    tv_rx_tmp tmp = { NULL, 0 };
    tv_radix_inplace(k, (size_t)n, &tmp);
    free(tmp.p);
    uint64_t kz = desc ? ~0x8000000000000000ull : 0x8000000000000000ull;
    tv_int zi = 0;
    for (tv_int i = 0; i < n; i++) {
        uint64_t key = desc ? ~k[i] : k[i];
        if (k[i] == kz) { a[i] = zs[zi++] ? -0.0 : 0.0; continue; }
        uint64_t u = (key >> 63) ? (key ^ 0x8000000000000000ull) : ~key;
        memcpy(&a[i], &u, 8);
    }
    if (zs) tv_free(zs);
}

/* A typed array's `sort()` with no comparator: by number (as JavaScript's: -0 before +0, NaN
 * last). kind: 0 i8, 1 u8, 2 i16, 3 u16, 4 i32, 5 u32, 6 f32, 7 f64. */
void tv_sort_typed(void *data, tv_int n, int kind) {
    if (n < 2) return;
    uint64_t *k = tv_alloc((size_t)n * sizeof(uint64_t));
    for (tv_int i = 0; i < n; i++) {
        uint64_t key;
        switch (kind) {
        case 0: key = (uint64_t)(int64_t)((int8_t *)data)[i] ^ 0x8000000000000000ull; break;
        case 1: key = ((uint8_t *)data)[i]; break;
        case 2: key = (uint64_t)(int64_t)((int16_t *)data)[i] ^ 0x8000000000000000ull; break;
        case 3: key = ((uint16_t *)data)[i]; break;
        case 4: key = (uint64_t)(int64_t)((int32_t *)data)[i] ^ 0x8000000000000000ull; break;
        case 5: key = ((uint32_t *)data)[i]; break;
        default: {
            double x = kind == 6 ? (double)((float *)data)[i] : ((double *)data)[i];
            key = x != x ? UINT64_MAX : x == 0 && signbit(x) ? 0x7FFFFFFFFFFFFFFFull : tv_f64_key(x);
        }
        }
        k[i] = key;
    }
    tv_rx_tmp tmp = { NULL, 0 };
    tv_radix_inplace(k, (size_t)n, &tmp);
    free(tmp.p);
    for (tv_int i = 0; i < n; i++) {
        uint64_t key = k[i];
        switch (kind) {
        case 0: ((int8_t *)data)[i] = (int8_t)(int64_t)(key ^ 0x8000000000000000ull); break;
        case 1: ((uint8_t *)data)[i] = (uint8_t)key; break;
        case 2: ((int16_t *)data)[i] = (int16_t)(int64_t)(key ^ 0x8000000000000000ull); break;
        case 3: ((uint16_t *)data)[i] = (uint16_t)key; break;
        case 4: ((int32_t *)data)[i] = (int32_t)(int64_t)(key ^ 0x8000000000000000ull); break;
        case 5: ((uint32_t *)data)[i] = (uint32_t)key; break;
        default: {
            double x;
            if (key == UINT64_MAX) x = NAN;
            else {
                uint64_t u = (key >> 63) ? (key ^ 0x8000000000000000ull) : ~key;
                memcpy(&x, &u, 8);
            }
            if (kind == 6) ((float *)data)[i] = (float)x;
            else ((double *)data)[i] = x;
        }
        }
    }
    tv_free(k);
}

void tv_f64_index_trap(double d, const char *loc) {
    char msg[96];
    snprintf(msg, sizeof msg, "array index %.17g is not a whole number", d);
    tv_trap(msg, loc);
}

int32_t tv_f64_to_i32_slow(double d) {
    if (!isfinite(d)) return 0;
    double m = fmod(trunc(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (int32_t)(uint32_t)m;
}

/* ================================================================== strings */

const tv_strbuf tv_empty_strbuf = {TV_STR_IMMORTAL | TV_STR_ASCII, 0, {0}}; /* flexible-array initializer: GNU C (gcc, clang) */

#define TV_A1(c) {TV_STR_IMMORTAL | TV_STR_ASCII, 1, {(char)(c), 0}}
#define TV_A4(c) TV_A1(c), TV_A1((c) + 1), TV_A1((c) + 2), TV_A1((c) + 3)
#define TV_A16(c) TV_A4(c), TV_A4((c) + 4), TV_A4((c) + 8), TV_A4((c) + 12)
/* Immortal one-character strings for printable ASCII: chars()/split("") and friends don't allocate for them. */
const tv_small_strbuf tv_ascii_strs[96] = { /* printable ASCII, 32..127 */
    TV_A16(32), TV_A16(48), TV_A16(64), TV_A16(80), TV_A16(96), TV_A16(112),
};
#undef TV_A1
#undef TV_A4
#undef TV_A16

TV_STR_LIT(tv_lit_true, "true");
TV_STR_LIT(tv_lit_false, "false");


/* Small strings (header + bytes + NUL <= TV_SMALL_MAX) come from the small-object free lists;
 * larger ones from malloc. The class follows from the length alone, so freeing needs no flag.
 * (Plain malloc under AddressSanitizer, so it sees every string.) */
enum { TV_SMALL_MAX = (TV_SMALL_CLASSES - 1) * 8 };
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
#define TV_PLAIN_ALLOC 1
#endif
void *tv_small_bins[TV_SMALL_CLASSES];

/* A size class's free list is empty: hand out its next never-used block. Blocks come from a
 * chunk per class (1 KiB, doubling with each chunk to 16 KiB) and are handed out one by one, so
 * memory is only touched once an object uses it. The first 64 KiB of chunks come from malloc
 * one by one (mostly pages already in use: a small program adds none of its own); later ones
 * are cut from 16 MiB malloc blocks, mapped lazily (untouched pages cost nothing) and able to
 * reuse memory the program freed, such as a map's outgrown table. */
typedef struct tv_class_space { char *cur, *lim; uint8_t grow; } tv_class_space;
enum { TV_ARENA_BLOCK = 16 << 20 };
static char *tv_bump, *tv_bump_end;
static size_t tv_carved;
static void *tv_carve(tv_class_space *k, size_t sz) {
    if (TV_LIKELY((size_t)(k->lim - k->cur) >= sz)) {
        char *p = k->cur;
        k->cur += sz;
        return p;
    }
    size_t chunk = (size_t)1024 << k->grow;
    if (chunk >= 16384) chunk = 16384;
    else k->grow++;
    size_t n = chunk / sz;
    if (n == 0) n = 1;
    size_t bytes = n * sz;
    char *p;
    if (tv_carved + bytes <= 65536) {
        p = tv_alloc(bytes);
        tv_carved += bytes;
    } else {
        if ((size_t)(tv_bump_end - tv_bump) < bytes) {
            tv_bump = tv_alloc(TV_ARENA_BLOCK);
            tv_bump_end = tv_bump + TV_ARENA_BLOCK;
        }
        p = tv_bump;
        /* (rounded to 16, so the next chunk is as aligned as malloc's: tasks and promises need it) */
        tv_bump += (bytes + 15) & ~(size_t)15;
    }
    k->cur = p + sz;
    k->lim = p + bytes;
    return p;
}

static tv_class_space tv_small_space[TV_SMALL_CLASSES];
__attribute__((noinline)) void *tv_small_refill(size_t c) {
    return tv_carve(&tv_small_space[c], c * 8);
}
static inline void *tv_small_alloc(size_t size) {
#ifdef TV_PLAIN_ALLOC
    /* (the size class it would come from: code may use the whole block, see tv_str_append_bytes) */
    return tv_alloc((size + 7) & ~(size_t)7);
#endif
    size_t c = (size + 7) >> 3;
    void **f = tv_small_bins[c];
    if (TV_LIKELY(f != NULL)) { tv_small_bins[c] = *f; return f; }
    return tv_small_refill(c);
}
static inline void tv_small_free(void *p, size_t size) {
#ifdef TV_PLAIN_ALLOC
    free(p); return;
#endif
    size_t c = (size + 7) >> 3;
    *(void **)p = tv_small_bins[c];
    tv_small_bins[c] = p;
}
static inline size_t tv_strbuf_size(size_t n) { return TV_STR_HDR + n + 1; }

/* (the last few medium strings' storage, freed: a string made and dropped again and again,
 * `JSON.parse(text)` of a long one in a loop, reuses it rather than the system allocator handing
 * the pages back each time; Tov's values live on one thread) */
#define TV_STR_KEEP_MAX ((size_t)1 << 20)
static struct { void *p; size_t size; } tv_str_kept[4];
static unsigned tv_str_kept_next;

static void *tv_str_take(size_t size) {
    for (int i = 0; i < 4; i++) {
        size_t k = tv_str_kept[i].size;
        if (tv_str_kept[i].p && k >= size && k - size <= size / 4) {
            void *p = tv_str_kept[i].p;
            tv_str_kept[i].p = NULL;
            return p;
        }
    }
    return tv_alloc(size);
}

static void tv_str_keep(void *p, size_t size) {
    if (size > TV_STR_KEEP_MAX) { free(p); return; }
    unsigned i = tv_str_kept_next++ & 3;
    if (tv_str_kept[i].p) free(tv_str_kept[i].p);
    tv_str_kept[i].p = p;
    tv_str_kept[i].size = size;
}

static tv_strbuf *tv_strbuf_new(size_t n) {
    size_t size = tv_size_mul_add(n, 1, TV_STR_HDR + 1);
    if (n > INT32_MAX) tv_trap("string too long", NULL);
    tv_strbuf *b = (tv_strbuf *)(size <= TV_SMALL_MAX ? tv_small_alloc(size) : size <= TV_STR_KEEP_MAX ? tv_str_take(size) : tv_alloc(size));
    b->rc = 1;
    b->len = (int32_t)n;
    b->data[n] = 0;
    return b;
}

static void tv_uview_drop(const tv_strbuf *p);
void tv_str_release_slow(tv_str s) {
    if (s.p->rc & TV_STR_WIDE) tv_uview_drop(s.p);
    size_t size = tv_strbuf_size((size_t)s.p->len);
    if (size <= TV_SMALL_MAX) tv_small_free(s.p, size);
#ifdef TV_SB_MAPS
    else if (TV_STR_MAYBE_MAPPED(size) && malloc_size(s.p) == 0) {
        size_t page = (size_t)getpagesize();
        tv_sb_unmap((char *)s.p, (size + page - 1) & ~(page - 1));
    }
#endif
    else tv_str_keep(s.p, size);
}

tv_str tv_str_alloc(size_t n, char **data) {
    tv_strbuf *b = tv_strbuf_new(n);
    *data = b->data;
    return (tv_str){b};
}

tv_str tv_str_from(const char *bytes, size_t n) {
    if (n == 0) return TV_EMPTY_STR;
    if (n == 1 && (unsigned char)bytes[0] - 32u < 96u) return tv_ascii_str((unsigned char)bytes[0]);
    tv_strbuf *b = tv_strbuf_new(n);
    tv_copy(b->data, bytes, n);
    return (tv_str){b};
}

/* Text known to be ASCII (number formatting): its UTF-16 length is its byte length. The bytes
 * were just written one at a time: copied one at a time too (a wider load of bytes still being
 * stored waits for all of them on x86, a store-forwarding stall that was most of `String(x)`). */
static tv_str tv_str_from_fresh(const char *bytes, size_t n);
static tv_str tv_str_from_ascii(const char *bytes, size_t n) {
    tv_str r = n <= 1 || n > 40 ? tv_str_from(bytes, n) : tv_str_from_fresh(bytes, n);
    if (r.p->rc > 0) r.p->rc |= TV_STR_ASCII;
    return r;
}

/* A short string of bytes just written (2 to 40 of them): see tv_str_from_ascii. */
static tv_str tv_str_from_fresh(const char *bytes, size_t n) {
    tv_strbuf *b = tv_strbuf_new(n);
#if defined(__aarch64__)
    /* (Arm forwards the bytes' stores to a copy's wider loads: a plain copy, ~10% faster for
     * `"v" + i` and ~17% for `String(x)` than the gather below) */
    memcpy(b->data, bytes, n);
    b->data[n] = 0;
#else
    /* (gathered a byte at a time, written a word at a time, the NUL and the block's padding
     * included: later word reads of the string, a compare's, forward from those stores) */
    const volatile unsigned char *src = (const volatile unsigned char *)bytes;
    for (size_t w = 0; w * 8 <= n; w++) {
        uint64_t word = 0;
        for (size_t k = 0; k < 8; k++) {
            size_t i = w * 8 + k;
            if (i < n) word |= (uint64_t)src[i] << (8 * k);
        }
        memcpy(b->data + w * 8, &word, 8);
    }
#endif
    return (tv_str){b};
}

tv_str tv_str_from_sb(tv_sb *sb) {
    size_t n = sb->len;
    if (tv_sb_is_stack(sb)) {
        tv_str r = n >= 2 && n <= 40 ? tv_str_from_fresh(sb->data, n) : tv_str_from(sb->data, n);
        sb->data = NULL;
        sb->len = sb->cap = 0;
        return r;
    }
    if (n <= 1) { /* empty or one byte: tv_str_from may return an immortal string */
        tv_str r = tv_str_from(sb->data, n);
        tv_sb_free(sb);
        return r;
    }
    if (tv_strbuf_size(n) <= TV_SMALL_MAX) { /* small strings live in the small-block allocator */
        tv_str r = n <= 40 ? tv_str_from_fresh(sb->data, n) : tv_str_from(sb->data, n);
        tv_sb_free(sb);
        return r;
    }
    if (TV_SB_MAPPED(sb->cap) && TV_STR_MAYBE_MAPPED(tv_strbuf_size(n))) {
        /* a big mapped builder (a response body, say) becomes the string, its spare pages given
         * back: no copy of it, nor a second of its size at once */
        char *base = sb->data - TV_STR_HDR;
        size_t page = (size_t)getpagesize();
        size_t keep = (tv_strbuf_size(n) + page - 1) & ~(page - 1), mapped = tv_sb_map_size(sb->cap);
        if (n > INT32_MAX) tv_trap("string too long", NULL);
        if (keep < mapped) munmap(base + keep, mapped - keep);
        tv_strbuf *b = (tv_strbuf *)base;
        b->rc = 1;
        b->len = (int32_t)n;
        b->data[n] = 0;
        sb->data = NULL;
        sb->len = sb->cap = 0;
        return (tv_str){b};
    }
    if (TV_SB_SMALL(sb->cap) || TV_SB_MAPPED(sb->cap)) { /* the builder's block is in the small-object heap, or mapped: copy out */
        tv_strbuf *b = (tv_strbuf *)tv_alloc(TV_STR_HDR + n + 1);
        memcpy(b->data, sb->data, n);
        tv_sb_free(sb);
        b->rc = 1;
        b->len = (int32_t)n;
        b->data[n] = 0;
        return (tv_str){b};
    }
    tv_strbuf *b = (tv_strbuf *)(sb->data - TV_STR_HDR);
    if (sb->cap - n > 64) b = (tv_strbuf *)tv_realloc(b, TV_STR_HDR + n + 1);
    b->rc = 1;
    if (n > INT32_MAX) tv_trap("string too long", NULL);
    b->len = (int32_t)n;
    b->data[n] = 0;
    sb->data = NULL;
    sb->len = sb->cap = 0;
    return (tv_str){b};
}

#if defined(__GLIBC__)
#include <malloc.h>
#endif
/* What a string's malloc'd block can hold, header and NUL included (0: unknown, or the block
 * isn't malloc's: a mapped one). */
static size_t tv_strbuf_block(const tv_strbuf *p) {
#if defined(__APPLE__)
    return malloc_size(p);
#elif defined(__GLIBC__)
    return malloc_usable_size((void *)p);
#else
    (void)p;
    return 0;
#endif
}

/* s += bytes[0, n) (`kind`: what's known of them, TV_STR_ASCII / TV_STR_WIDE). A string only `s`
 * holds grows in place, its block doubling when full (so a loop of `+=` is linear); a shared
 * one is copied into a block with room to grow. Small strings stay in their size class. */
void tv_str_append_bytes(tv_str *s, const char *bytes, size_t n, int32_t kind) {
    if (n == 0) return;
    tv_strbuf *p = s->p;
    size_t len = (size_t)p->len, nl = tv_size_mul_add(len, 1, n);
    if (nl > INT32_MAX) tv_trap("string too long", NULL);
    int32_t rc = p->rc;
    int32_t flags = (rc & kind & TV_STR_ASCII) | ((rc | kind) & TV_STR_WIDE);
    size_t size = tv_strbuf_size(len), nsize = tv_strbuf_size(nl);
    tv_strbuf *q = NULL;
    if (rc > 0 && (rc & TV_STR_COUNT) == 1 && (bytes < p->data || bytes > p->data + len)) {
        if (rc & TV_STR_WIDE) tv_uview_drop(p); /* (its UTF-16 view describes the old text) */
        if (size <= TV_SMALL_MAX) {
            if (((size + 7) >> 3) == ((nsize + 7) >> 3)) q = p;
            else {
                q = (tv_strbuf *)(nsize <= TV_SMALL_MAX ? tv_small_alloc(nsize) : tv_alloc(nsize < 1024 ? 1024 : nsize));
                memcpy(q->data, p->data, len);
                tv_small_free(p, size);
            }
        } else {
            size_t block = tv_strbuf_block(p);
            if (block >= nsize) q = p;
            else if (block) q = (tv_strbuf *)tv_realloc(p, nsize > 2 * block ? nsize : 2 * block);
        }
        if (q) {
            tv_copy(q->data + len, bytes, n);
            q->data[nl] = 0;
            q->len = (int32_t)nl;
            q->rc = 1 | flags;
            s->p = q;
            return;
        }
    }
    /* (shared, immortal or mapped: a copy, with room for more) */
    q = (tv_strbuf *)(nsize <= TV_SMALL_MAX ? tv_small_alloc(nsize) : tv_alloc(nsize + nsize / 2));
    tv_copy(q->data, p->data, len);
    tv_copy(q->data + len, bytes, n);
    q->data[nl] = 0;
    q->len = (int32_t)nl;
    q->rc = 1 | flags;
    tv_str_release(*s);
    s->p = q;
}

void tv_str_append(tv_str *s, tv_str t) {
    tv_str_append_bytes(s, t.p->data, (size_t)t.p->len, t.p->rc & (TV_STR_ASCII | TV_STR_WIDE));
}

tv_str tv_str_concat(tv_str a, tv_str b) {
    size_t na = (size_t)a.p->len, nb = (size_t)b.p->len;
    if (nb == 0) { tv_str_retain(a); return a; }
    if (na == 0) { tv_str_retain(b); return b; }
    tv_strbuf *r = tv_strbuf_new(tv_size_mul_add(na, 1, nb));
    memcpy(r->data, a.p->data, na);
    memcpy(r->data + na, b.p->data, nb);
    r->rc |= (a.p->rc & b.p->rc & TV_STR_ASCII) | ((a.p->rc | b.p->rc) & TV_STR_WIDE);
    return (tv_str){r};
}

tv_str tv_str_concatn(const tv_str *parts, int n) {
    size_t total = 0;
    uint64_t ascii = TV_STR_ASCII, wide = 0;
    for (int i = 0; i < n; i++) {
        total = tv_size_mul_add((size_t)parts[i].p->len, 1, total);
        ascii &= parts[i].p->rc;
        wide |= parts[i].p->rc;
    }
    tv_strbuf *r = tv_strbuf_new(total);
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        size_t k = (size_t)parts[i].p->len;
        memcpy(r->data + at, parts[i].p->data, k);
        at += k;
    }
    r->rc |= (ascii & TV_STR_ASCII) | (wide & TV_STR_WIDE);
    return (tv_str){r};
}

tv_gen *tv_gen_new(void *F, bool (*resume)(void *F), void (*drop)(void *F), void *yv) {
    tv_gen *g = (tv_gen *)tv_alloc(sizeof(tv_gen));
    g->rc = 1; g->F = F; g->resume = resume; g->drop = drop; g->yv = yv; g->done = false;
    return g;
}

void tv_gen_release(tv_gen *g) {
    if (!g || --g->rc > 0) return;
    g->drop(g->F);
    free(g);
}

tv_arr tv_gen_collect(tv_gen *g, const tv_type *t) {
    tv_arr out = tv_arr_fresh();
    unsigned char tmp[64];
    while (!tv_gen_next(g)) {
        void *v = g->yv;
        if (t->size <= sizeof tmp) {
            memcpy(tmp, v, t->size);
            if (t->retain) t->retain(tmp);
            tv_arr_push(&out, t, tmp);
        } else {
            void *c = malloc(t->size);
            if (!c) tv_trap("out of memory", NULL);
            memcpy(c, v, t->size);
            if (t->retain) t->retain(c);
            tv_arr_push(&out, t, c);
            free(c);
        }
    }
    return out;
}

/* A generator over an array: its frame is the array, the type and the next index. */
typedef struct { tv_arr a; const tv_type *t; int64_t i; void *yv; } tv_arr_gen;
static tv_gen *tv_arr_gen_self(void *F);
static bool tv_arr_gen_resume(void *F) {
    tv_arr_gen *g = F;
    if (g->i >= tv_arr_len(g->a)) return true;
    tv_arr_gen_self(F)->yv = tv_arr_data(g->a) + (size_t)g->i * g->t->size;
    g->i++;
    return false;
}
static void tv_arr_gen_drop(void *F) {
    tv_arr_gen *g = F;
    tv_arr_release(g->a, g->t);
    free(g);
}
/* (the generator's own `yv` points into the array: kept in the frame's last field) */
static tv_gen *tv_arr_gen_self(void *F) { return (tv_gen *)((tv_arr_gen *)F)->yv; }

tv_gen *tv_gen_of_arr(tv_arr a, const tv_type *t) {
    tv_arr_gen *F = malloc(sizeof *F);
    if (!F) tv_trap("out of memory", NULL);
    F->a = a; F->t = t; F->i = 0;
    tv_gen *g = tv_gen_new(F, tv_arr_gen_resume, tv_arr_gen_drop, NULL);
    F->yv = g;
    return g;
}

static void tv_gen_retain_v(void *p) { tv_gen_retain(*(tv_gen **)p); }
static void tv_gen_release_v(void *p) { tv_gen_release(*(tv_gen **)p); }
static bool tv_gen_eq_v(const void *a, const void *b) { return *(tv_gen *const *)a == *(tv_gen *const *)b; }
static uint64_t tv_gen_hash_v(const void *p) { return (uint64_t)(uintptr_t)*(tv_gen *const *)p * 0x9E3779B97F4A7C15ull; }
static void tv_gen_str_v(tv_sb *sb, const void *p) { (void)p; tv_sb_push_cstr(sb, "[object Generator]"); }
static void tv_gen_inspect_v(tv_sb *sb, const void *p, int depth) { (void)p; (void)depth; tv_sb_push_cstr(sb, "Object [Generator] {}"); }
const tv_type tv_type_gen = {sizeof(tv_gen *), tv_gen_retain_v, tv_gen_release_v, tv_gen_eq_v, tv_gen_hash_v, tv_gen_str_v, tv_gen_inspect_v};

int tv_str_cmp_long(tv_str a, tv_str b) {
    int64_t na = a.p->len, nb = b.p->len;
    int c = memcmp(a.p->data, b.p->data, (size_t)(na < nb ? na : nb));
    if (c) return c < 0 ? -1 : 1;
    return na < nb ? -1 : na > nb;
}

uint64_t tv_str_hash(tv_str s) { return tv_hash_bytes(s.p->data, (size_t)s.p->len); }

/* ------------------------------------------------------------------ integer / bool formatting */

/* Writes the decimal digits of v ending at `end`; returns the start. */
static char *tv_fmt_u64_rev(char *end, uint64_t v) {
    do {
        *--end = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    return end;
}

static size_t tv_fmt_i64(char *buf, int64_t v) { /* buf >= 21 bytes */
    char tmp[24], *end = tmp + sizeof tmp;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    char *p = tv_fmt_u64_rev(end, u);
    if (v < 0) *--p = '-';
    size_t n = (size_t)(end - p);
    memcpy(buf, p, n);
    return n;
}

tv_str tv_str_from_int(tv_int v) {
    /* (0 to 999, `String(i % 50)`, a key or a label: shared immortal strings, made on first use;
     * their table and the cache below too, so a program's zeroed data stays one page) */
    static tv_small_strbuf *small;
    static struct tv_int_cached { tv_int v; tv_str s; } *cache;
    if (TV_UNLIKELY(!small)) {
        small = tv_zero_pages(1000 * sizeof *small + 256 * sizeof *cache);
        cache = (struct tv_int_cached *)(void *)(small + 1000);
    }
    if ((uint64_t)v < 1000) {
        tv_small_strbuf *b = &small[v];
        if (!b->len) {
            int n = v >= 100 ? 3 : v >= 10 ? 2 : 1;
            for (int i = n - 1, x = (int)v; i >= 0; i--, x /= 10) b->data[i] = (char)('0' + x % 10);
            b->rc = TV_STR_IMMORTAL | TV_STR_ASCII;
            b->len = n;
        }
        return (tv_str){(tv_strbuf *)(void *)b};
    }
    /* (others made text again and again, `String(key)` in a hash or `String(id)` as a key:
     * the last ones' text kept, by value; Tov's values live on one thread) */
    size_t h = (size_t)(((uint64_t)v * 0x9E3779B97F4A7C15ull) >> 56);
    if (cache[h].s.p && cache[h].v == v) {
        tv_str_retain(cache[h].s);
        return cache[h].s;
    }
    char buf[24];
    tv_str s = tv_str_from_ascii(buf, tv_fmt_i64(buf, v));
    if (cache[h].s.p) tv_str_release(cache[h].s);
    tv_str_retain(s);
    cache[h].v = v;
    cache[h].s = s;
    return s;
}

void tv_sb_push_int(tv_sb *sb, tv_int v) {
    /* written into the builder from the end of its room for 20 digits and a sign */
    char tmp[24], *end = tmp + sizeof tmp;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    char *p = end;
    while (u >= 100) {
        static const char pairs[] = "00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";
        unsigned r = (unsigned)(u % 100);
        u /= 100;
        p -= 2;
        p[0] = pairs[2 * r];
        p[1] = pairs[2 * r + 1];
    }
    if (u >= 10) {
        p -= 2;
        p[0] = (char)('0' + u / 10);
        p[1] = (char)('0' + u % 10);
    } else {
        *--p = (char)('0' + u);
    }
    if (v < 0) *--p = '-';
    tv_sb_push(sb, p, (size_t)(end - p));
}

static void tv_sb_push_u64(tv_sb *sb, uint64_t v) {
    char tmp[24], *end = tmp + sizeof tmp;
    char *p = tv_fmt_u64_rev(end, v);
    tv_sb_push(sb, p, (size_t)(end - p));
}

tv_str tv_str_from_bool(bool v) { return v ? TV_LIT(tv_lit_true) : TV_LIT(tv_lit_false); }

/* ------------------------------------------------------------------ JS number formatting */

/* Ryu's multipliers: 5^i and 5^-i to 125 bits, from every 26th power and 2-bit corrections
 * (generated and checked against exact values). */
static const uint64_t tv_ryu_pow5[26] = { 1u, 5u, 25u, 125u, 625u, 3125u, 15625u, 78125u, 390625u, 1953125u, 9765625u, 48828125u, 244140625u, 1220703125u, 6103515625u, 30517578125u, 152587890625u, 762939453125u, 3814697265625u, 19073486328125u, 95367431640625u, 476837158203125u, 2384185791015625u, 11920928955078125u, 59604644775390625u, 298023223876953125u };
static const uint64_t tv_ryu_split[13][2] = { { 0u, 1152921504606846976u }, { 0u, 1490116119384765625u }, { 1032610780636961552u, 1925929944387235853u }, { 7910200175544436838u, 1244603055572228341u }, { 16941905809032713930u, 1608611746708759036u }, { 13024893955298202172u, 2079081953128979843u }, { 6607496772837067824u, 1343575221513417750u }, { 17332926989895652603u, 1736530273035216783u }, { 13037379183483547984u, 2244412773384604712u }, { 1605989338741628675u, 1450417759929778918u }, { 9630225068416591280u, 1874621017369538693u }, { 665883850346957067u, 1211445438634777304u }, { 14931890668723713708u, 1565756531257009982u } };
static const uint64_t tv_ryu_inv_split[15][2] = { { 1u, 2305843009213693952u }, { 5955668970331000884u, 1784059615882449851u }, { 8982663654677661702u, 1380349269358112757u }, { 7286864317269821294u, 2135987035920910082u }, { 7005857020398200553u, 1652639921975621497u }, { 17965325103354776697u, 1278668206209430417u }, { 8928596168509315048u, 1978643211784836272u }, { 10075671573058298858u, 1530901034580419511u }, { 597001226353042382u, 1184477304306571148u }, { 1527430471115325346u, 1832889850782397517u }, { 12533209867169019542u, 1418129833677084982u }, { 5577825024675947042u, 2194449627517475473u }, { 11006974540203867551u, 1697873161311732311u }, { 10313493231639821582u, 1313665730009899186u }, { 12701016819766672773u, 2032799256770390445u } };
static const uint32_t tv_ryu_offsets[21] = { 0u, 0u, 0u, 0u, 1073741824u, 1500076437u, 1431590229u, 1448432917u, 1091896580u, 1079333904u, 1146442053u, 1146111296u, 1163220304u, 1073758208u, 2521039936u, 1431721317u, 1413824581u, 1075134801u, 1431671125u, 1363170645u, 261u };
static const uint32_t tv_ryu_inv_offsets[22] = { 1414808916u, 67458373u, 268701696u, 4195348u, 1073807360u, 1091917141u, 1108u, 65604u, 1073741824u, 1140850753u, 1346716752u, 1431634004u, 1365595476u, 1073758208u, 16777217u, 66816u, 1364284433u, 89478484u, 1346442496u, 1074003968u, 84148496u, 0u };

/* Shortest round-trip digits of a double (Ryu, Ulf Adams 2018), small-table form. */
static const char tv_digit_pairs[] = "00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";
static inline uint32_t tv_ryu_pow5bits(int32_t e) { return (uint32_t)(((uint32_t)e * 1217359) >> 19) + 1; }
static inline uint32_t tv_ryu_log10_pow2(int32_t e) { return ((uint32_t)e * 78913) >> 18; }
static inline uint32_t tv_ryu_log10_pow5(int32_t e) { return ((uint32_t)e * 732923) >> 20; }
static inline uint32_t tv_ryu_pow5_factor(uint64_t v) {
    uint32_t n = 0;
    for (;;) { uint64_t q = v / 5; if (v - 5 * q != 0) break; v = q; n++; }
    return n;
}
static inline bool tv_ryu_mul_pow5(uint64_t v, uint32_t p) { return tv_ryu_pow5_factor(v) >= p; }
static inline bool tv_ryu_mul_pow2(uint64_t v, uint32_t p) { return (v & ((1ull << p) - 1)) == 0; }
static inline void tv_ryu_compute_pow5(uint32_t i, uint64_t r[2]) {
    uint32_t base = i / 26, base2 = base * 26, off = i - base2;
    const uint64_t *mul = tv_ryu_split[base];
    if (off == 0) { r[0] = mul[0]; r[1] = mul[1]; return; }
    uint64_t m = tv_ryu_pow5[off];
    unsigned __int128 b0 = (unsigned __int128)m * mul[0], b2 = (unsigned __int128)m * mul[1];
    uint32_t delta = tv_ryu_pow5bits((int32_t)i) - tv_ryu_pow5bits((int32_t)base2);
    unsigned __int128 s = (b0 >> delta) + (b2 << (64 - delta)) + ((tv_ryu_offsets[i / 16] >> ((i % 16) << 1)) & 3);
    r[0] = (uint64_t)s; r[1] = (uint64_t)(s >> 64);
}
static inline void tv_ryu_compute_inv_pow5(uint32_t i, uint64_t r[2]) {
    uint32_t base = (i + 25) / 26, base2 = base * 26, off = base2 - i;
    const uint64_t *mul = tv_ryu_inv_split[base];
    if (off == 0) { r[0] = mul[0]; r[1] = mul[1]; return; }
    uint64_t m = tv_ryu_pow5[off];
    unsigned __int128 b0 = (unsigned __int128)m * (mul[0] - 1), b2 = (unsigned __int128)m * mul[1];
    uint32_t delta = tv_ryu_pow5bits((int32_t)base2) - tv_ryu_pow5bits((int32_t)i);
    unsigned __int128 s = (b0 >> delta) + (b2 << (64 - delta)) + 1 + ((tv_ryu_inv_offsets[i / 16] >> ((i % 16) << 1)) & 3);
    r[0] = (uint64_t)s; r[1] = (uint64_t)(s >> 64);
}
static inline uint64_t tv_ryu_mul_shift(uint64_t m, const uint64_t mul[2], int32_t j) {
    unsigned __int128 b0 = (unsigned __int128)m * mul[0], b2 = (unsigned __int128)m * mul[1];
    return (uint64_t)(((b0 >> 64) + b2) >> (j - 64));
}
/* v > 0, finite: digits (no trailing zeros) and n with v == 0.d1..dk × 10^n. */
static int tv_ryu_digits(double v, char *digits, int *n_out) {
    uint64_t bits; memcpy(&bits, &v, 8);
    uint64_t ieee_m = bits & ((1ull << 52) - 1);
    uint32_t ieee_e = (uint32_t)((bits >> 52) & 0x7ff);
    int32_t e2; uint64_t m2;
    if (ieee_e == 0) { e2 = 1 - 1023 - 52 - 2; m2 = ieee_m; }
    else { e2 = (int32_t)ieee_e - 1023 - 52 - 2; m2 = (1ull << 52) | ieee_m; }
    bool even = (m2 & 1) == 0, accept = even;
    uint64_t mv = 4 * m2;
    uint32_t mm_shift = ieee_m != 0 || ieee_e <= 1;
    uint64_t vr, vp, vm, mul[2];
    int32_t e10;
    bool vm_tz = false, vr_tz = false;
    if (e2 >= 0) {
        uint32_t q = tv_ryu_log10_pow2(e2) - (e2 > 3);
        e10 = (int32_t)q;
        int32_t k = 125 + (int32_t)tv_ryu_pow5bits((int32_t)q) - 1;
        int32_t i = -e2 + (int32_t)q + k;
        tv_ryu_compute_inv_pow5(q, mul);
        vr = tv_ryu_mul_shift(4 * m2, mul, i);
        vp = tv_ryu_mul_shift(4 * m2 + 2, mul, i);
        vm = tv_ryu_mul_shift(4 * m2 - 1 - mm_shift, mul, i);
        if (q <= 21) {
            if (mv % 5 == 0) vr_tz = tv_ryu_mul_pow5(mv, q);
            else if (accept) vm_tz = tv_ryu_mul_pow5(mv - 1 - mm_shift, q);
            else vp -= tv_ryu_mul_pow5(mv + 2, q);
        }
    } else {
        uint32_t q = tv_ryu_log10_pow5(-e2) - (-e2 > 1);
        e10 = (int32_t)q + e2;
        int32_t i = -e2 - (int32_t)q;
        int32_t k = (int32_t)tv_ryu_pow5bits(i) - 125;
        int32_t j = (int32_t)q - k;
        tv_ryu_compute_pow5((uint32_t)i, mul);
        vr = tv_ryu_mul_shift(4 * m2, mul, j);
        vp = tv_ryu_mul_shift(4 * m2 + 2, mul, j);
        vm = tv_ryu_mul_shift(4 * m2 - 1 - mm_shift, mul, j);
        if (q <= 1) {
            vr_tz = true;
            if (accept) vm_tz = mm_shift == 1;
            else --vp;
        } else if (q < 63) {
            vr_tz = tv_ryu_mul_pow2(mv, q);
        }
    }
    int32_t removed = 0;
    uint8_t last = 0;
    uint64_t out;
    if (vm_tz || vr_tz) {
        while (vp / 10 > vm / 10) {
            vm_tz &= vm % 10 == 0;
            vr_tz &= last == 0;
            last = (uint8_t)(vr % 10);
            vr /= 10; vp /= 10; vm /= 10; removed++;
        }
        if (vm_tz) {
            while (vm % 10 == 0) {
                vr_tz &= last == 0;
                last = (uint8_t)(vr % 10);
                vr /= 10; vp /= 10; vm /= 10; removed++;
            }
        }
        if (vr_tz && last == 5 && vr % 2 == 0) last = 4;
        out = vr + ((vr == vm && (!accept || !vm_tz)) || last >= 5);
    } else {
        /* (only the most significant digit removed decides the rounding: remove them in
         * chunks while the bounds still differ above the chunk) */
        bool up = false;
        if (vp / 100000000 > vm / 100000000) {
            up = vr % 100000000 >= 50000000;
            vr /= 100000000; vp /= 100000000; vm /= 100000000; removed += 8;
        }
        if (vp / 10000 > vm / 10000) {
            up = vr % 10000 >= 5000;
            vr /= 10000; vp /= 10000; vm /= 10000; removed += 4;
        }
        if (vp / 100 > vm / 100) {
            up = vr % 100 >= 50;
            vr /= 100; vp /= 100; vm /= 100; removed += 2;
        }
        if (vp / 10 > vm / 10) {
            up = vr % 10 >= 5;
            vr /= 10; vp /= 10; vm /= 10; removed++;
        }
        out = vr + (vr == vm || up);
    }
    while (out % 10 == 0 && out >= 10) { out /= 10; removed++; }
    int len = 1;
    for (uint64_t p = 10; len < 20 && out >= p; p *= 10) len++;
    char *q = digits + len;
    while (out >= 100) { uint32_t r = (uint32_t)(out % 100); out /= 100; q -= 2; memcpy(q, tv_digit_pairs + 2 * r, 2); }
    if (out >= 10) { q -= 2; memcpy(q, tv_digit_pairs + 2 * out, 2); }
    else *--q = (char)('0' + out);
    *n_out = e10 + removed + len;
    return len;
}

/* Shortest decimal digits d1..dk (no trailing zeros) and exponent n such that
 * v == 0.d1..dk × 10^n round-trips (v > 0, finite), the nearest v of those (JavaScript's
 * Number::toString). For f32, round-trips as a float. */
static int tv_shortest_digits(double v, bool is_f32, char *digits, int *n_out) {
    if (!is_f32) return tv_ryu_digits(v, digits, n_out);
    char tmp[48];
    for (int prec = 0; prec < 9; prec++) {
        snprintf(tmp, sizeof tmp, "%.*e", prec, v);
        if (strtof(tmp, NULL) == (float)v) break;
    }
    int k = 0;
    const char *s = tmp;
    for (; *s && *s != 'e'; s++)
        if (*s >= '0' && *s <= '9') digits[k++] = *s;
    int e = *s == 'e' ? atoi(s + 1) : 0;
    while (k > 1 && digits[k - 1] == '0') k--;
    *n_out = e + 1;
    return k;
}

/* JS Number::toString(v) into buf (>= 32 bytes); returns the length. */
static size_t tv_fmt_number(char *buf, double v, bool is_f32) {
    if (v != v) { memcpy(buf, "NaN", 3); return 3; }
    if (v == 0) { buf[0] = '0'; return 1; }
    char *p = buf;
    if (v < 0) { *p++ = '-'; v = -v; }
    if (isinf(v)) { memcpy(p, "Infinity", 8); return (size_t)(p - buf) + 8; }
    if (v < (is_f32 ? 16777216.0 : 9007199254740992.0) && v == (double)(int64_t)v)
        return (size_t)(p - buf) + tv_fmt_i64(p, (int64_t)v);
    char d[24] = {0};
    int n, k = 0;
    /* (a number of a few decimals, a price: the fewest that give it back exactly, m / 10^j with m
     * below 10^15 (one such m: the double's spacing is finer than 10^-j there) are its shortest
     * digits, without the general search) */
    static const double p10[7] = {1, 10, 100, 1000, 10000, 100000, 1000000};
    double t6 = v * 1e6;
    /* (v * 10^6 a whole number but for its rounding: else it has more decimals, past this path) */
    if (!is_f32 && v < 1e9 && v >= 1e-6 && __builtin_fabs(t6 - __builtin_round(t6)) <= t6 * 1e-15) {
        for (int j = 1; j <= 6; j++) {
            double t = v * p10[j];
            if (t >= 1e15) break;
            int64_t m = (int64_t)(t + 0.5);
            if ((double)m / p10[j] != v) continue;
            char tmp[20];
            int nd = 0;
            for (int64_t x = m; x; x /= 10) tmp[nd++] = (char)('0' + x % 10);
            for (int i = 0; i < nd; i++) d[i] = tmp[nd - 1 - i];
            k = nd;
            n = nd - j;
            goto laid_out;
        }
    }
    k = tv_shortest_digits(v, is_f32, d, &n);
laid_out:
    if (k <= n && n <= 21) {
        memcpy(p, d, (size_t)k); p += k;
        for (int i = k; i < n; i++) *p++ = '0';
    } else if (0 < n && n <= 21) {
        memcpy(p, d, (size_t)n); p += n;
        *p++ = '.';
        memcpy(p, d + n, (size_t)(k - n)); p += k - n;
    } else if (-6 < n && n <= 0) {
        *p++ = '0'; *p++ = '.';
        for (int i = n; i < 0; i++) *p++ = '0';
        memcpy(p, d, (size_t)k); p += k;
    } else {
        *p++ = d[0];
        if (k > 1) { *p++ = '.'; memcpy(p, d + 1, (size_t)(k - 1)); p += k - 1; }
        *p++ = 'e';
        int e = n - 1;
        *p++ = e < 0 ? '-' : '+';
        p += tv_fmt_i64(p, e < 0 ? -e : e);
    }
    return (size_t)(p - buf);
}

tv_str tv_str_from_f64(double v) {
    /* (a number made text again and again, by a sort comparing them as strings or `String(id)`:
     * the last ones' text kept, by the number's bits; Tov's values live on one thread. Made at
     * the first call: 4 KB of zeros in the program's data would put its globals on a second
     * page, for every program that can print a number, however few it prints) */
    static struct tv_f64_cached { uint64_t bits; tv_str s; } *cache;
    if (TV_UNLIKELY(!cache)) cache = tv_zero_pages(256 * sizeof *cache);
    uint64_t bits;
    memcpy(&bits, &v, 8);
    size_t h = (size_t)((bits * 0x9E3779B97F4A7C15ull) >> 56);
    if (cache[h].s.p && cache[h].bits == bits) {
        tv_str_retain(cache[h].s);
        return cache[h].s;
    }
    char buf[40];
    tv_str s = tv_str_from_ascii(buf, tv_fmt_number(buf, v, false));
    if (cache[h].s.p) tv_str_release(cache[h].s);
    tv_str_retain(s);
    cache[h].bits = bits;
    cache[h].s = s;
    return s;
}

static const char tv_radix_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

tv_str tv_int_to_radix(tv_int v, tv_int radix, const char *loc) {
    if (radix < 2 || radix > 36) tv_trap("toString() radix must be between 2 and 36", loc);
    char buf[72], *end = buf + sizeof buf, *p = end;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        *--p = tv_radix_digits[u % (uint64_t)radix];
        u /= (uint64_t)radix;
    } while (u);
    if (v < 0) *--p = '-';
    return tv_str_from_ascii(p, (size_t)(end - p));
}

/* V8's DoubleToRadixCString: the shortest digits that read back as v, fractions
   included, so the result matches JavaScript's digit for digit. */
tv_str tv_f64_to_radix(double v, tv_int radix, const char *loc) {
    if (radix < 2 || radix > 36) tv_trap("toString() radix must be between 2 and 36", loc);
    if (radix == 10 || isnan(v) || isinf(v)) return tv_str_from_f64(v);
    enum { SIZE = 2200 };
    char buf[SIZE];
    int icur = SIZE / 2, fcur = icur;
    bool neg = v < 0;
    if (neg) v = -v;
    double integer = floor(v), fraction = v - integer;
    /* half the distance to the next double: digits below it don't change the value */
    double delta = 0.5 * (nextafter(v, INFINITY) - v);
    if (delta < nextafter(0.0, 1.0)) delta = nextafter(0.0, 1.0);
    int r = (int)radix;
    if (fraction >= delta) {
        buf[fcur++] = '.';
        do {
            fraction *= r;
            delta *= r;
            int digit = (int)fraction;
            buf[fcur++] = tv_radix_digits[digit];
            fraction -= digit;
            if (fraction > 0.5 || (fraction == 0.5 && (digit & 1))) {
                if (fraction + delta > 1) {
                    /* round up, carrying into the integer part if every digit overflows */
                    for (;;) {
                        fcur--;
                        if (fcur == SIZE / 2) {
                            integer += 1;
                            break;
                        }
                        char c = buf[fcur];
                        int d = c > '9' ? c - 'a' + 10 : c - '0';
                        if (d + 1 < r) {
                            buf[fcur++] = tv_radix_digits[d + 1];
                            break;
                        }
                    }
                    break;
                }
            }
        } while (fraction >= delta);
    }
    /* integer digits; past 2^53 the low digits aren't represented and are zeros */
    while (integer / r >= 9007199254740992.0) {
        integer /= r;
        buf[--icur] = '0';
    }
    do {
        double rem = fmod(integer, r);
        buf[--icur] = tv_radix_digits[(int)rem];
        integer = (integer - rem) / r;
    } while (integer > 0);
    if (neg) buf[--icur] = '-';
    return tv_str_from_ascii(buf + icur, (size_t)(fcur - icur));
}

void tv_sb_push_f64(tv_sb *sb, double v) {
    char *p = tv_sb_reserve(sb, 40);
    sb->len += tv_fmt_number(p, v, false);
}

static void tv_toFixed_check(tv_int digits, const char *loc) {
    if (digits < 0 || digits > 100) tv_trap("toFixed() digits argument must be between 0 and 100", loc);
}

static int tv_fixed_into(double v, int f, char *buf);

tv_str tv_f64_to_fixed(double v, tv_int digits, const char *loc) {
    tv_toFixed_check(digits, loc);
    if (v != v || fabs(v) >= 1e21) return tv_str_from_f64(v);
    if (v == 0) v = 0.0; /* (-0).toFixed() is "0" */
    char buf[160];
    int len = tv_fixed_into(v, (int)digits, buf);
    return tv_str_from_ascii(buf, (size_t)len);
}

/* "1234567.5" (an optional '-') as "1,234,567.5", trailing fraction zeros past min_frac dropped */
static tv_str tv_group_digits(const char *s, int len, int min_frac) {
    const char *dot = memchr(s, '.', (size_t)len);
    int end = len;
    if (dot) {
        int keep = (int)(dot - s) + 1 + min_frac;
        while (end > keep && s[end - 1] == '0') end--;
        if (end == (int)(dot - s) + 1) end--;
    } else if (min_frac > 0) {
        dot = s + len;
    }
    int neg = s[0] == '-';
    int ilen = (int)((dot ? dot : s + len) - s) - neg;
    char out[800];
    int o = 0;
    if (neg) out[o++] = '-';
    for (int i = 0; i < ilen; i++) {
        if (i > 0 && (ilen - i) % 3 == 0) out[o++] = ',';
        out[o++] = s[neg + i];
    }
    if (dot && dot < s + len) {
        memcpy(out + o, dot, (size_t)(s + end - dot));
        o += (int)(s + end - dot);
    } else if (min_frac > 0) {
        out[o++] = '.';
        for (int i = 0; i < min_frac; i++) out[o++] = '0';
    }
    return tv_str_from_ascii(out, (size_t)o);
}

/* Intl rounds the shortest decimal form of v (1.0005 → "1.001"), halves away from zero */
tv_str tv_f64_to_locale(double v, int min_frac, int max_frac) {
    if (v != v) return tv_str_from_ascii("NaN", 3);
    if (isinf(v)) return v > 0 ? tv_str_from("\xe2\x88\x9e", 3) : tv_str_from("-\xe2\x88\x9e", 4);
    char d[24] = {'0'};
    int k = 1, n = 1;
    if (v != 0) k = tv_shortest_digits(fabs(v), false, d, &n);
    /* |v| = 0.d1..dk × 10^n; digit j of the result (ilen integer digits, then max_frac) is d[j + n - ilen] */
    int ilen = n > 1 ? n : 1, total = ilen + max_frac;
    char buf[512];
    int len = 0;
    if (signbit(v)) buf[len++] = '-';
    char *digits = buf + len + 1; /* (room for a carry) */
    for (int j = 0; j < total; j++) {
        int i = j + n - ilen;
        digits[j] = i >= 0 && i < k ? d[i] : '0';
    }
    int r = total + n - ilen;
    if (r >= 0 && r < k && d[r] >= '5') {
        int j = total - 1;
        while (j >= 0 && digits[j] == '9') digits[j--] = '0';
        if (j >= 0) digits[j]++;
        else { digits--; digits[0] = '1'; ilen++; total++; }
    }
    if (digits != buf + len) memmove(buf + len, digits, (size_t)total);
    memmove(buf + len + ilen + 1, buf + len + ilen, (size_t)max_frac);
    buf[len + ilen] = '.';
    len += total + 1;
    if (max_frac == 0) len--;
    return tv_group_digits(buf, len, min_frac);
}

tv_str tv_int_to_locale(tv_int v, int min_frac) {
    char buf[32];
    int len = snprintf(buf, sizeof buf, "%lld", (long long)v);
    return tv_group_digits(buf, len, min_frac);
}

/* v (finite, |v| < 1e21) with f fraction digits, as toFixed: into buf, returning its length */
static int tv_fixed_into(double v, int f, char *buf) {
    int len;
    /* printf rounds exact ties to even; JS rounds them away from zero. A tie happens exactly
     * when v = m × 2^-(f+1) with m odd; then "%.{f+1}f" is exact and ends in '5'. */
    /* |v| = mant × 2^e, from its bits */
    double a = fabs(v);
    uint64_t bits;
    memcpy(&bits, &a, sizeof bits);
    int be = (int)(bits >> 52);
    uint64_t mant = bits & ((UINT64_C(1) << 52) - 1);
    int e = be ? be - 1075 : -1074;
    if (be) mant |= UINT64_C(1) << 52;
    while (mant && !(mant & 1)) { mant >>= 1; e++; }
    /* (exact in 128-bit integers, the common case: n = |v| × 10^f, rounded half up as JS's
     * toFixed picks the larger of two nearest; then its digits with the point put in) */
    if (f <= 19 && (mant == 0 || (e < 0 && e >= -100) || (e >= 0 && e <= 10))) {
        static const uint64_t p10[20] = {1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull, 100000000ull, 1000000000ull,
            10000000000ull, 100000000000ull, 1000000000000ull, 10000000000000ull, 100000000000000ull, 1000000000000000ull,
            10000000000000000ull, 100000000000000000ull, 1000000000000000000ull, 10000000000000000000ull};
        unsigned __int128 n;
        if (mant == 0) {
            n = 0;
        } else if (e >= 0) {
            n = (unsigned __int128)(mant << e) * p10[f];
        } else {
            unsigned __int128 p = (unsigned __int128)mant * p10[f];
            int sh = -e;
            if (sh >= 128) {
                n = 0;
            } else {
                n = p >> sh;
                unsigned __int128 rem = p - (n << sh), half = (unsigned __int128)1 << (sh - 1);
                if (rem >= half) n++;
            }
        }
        char dig[48];
        int nd = 0;
        /* (128-bit division is a library call: the digits past 64 bits only that way) */
        while (n >> 64) { dig[nd++] = (char)('0' + (int)(n % 10)); n /= 10; }
        uint64_t n64 = (uint64_t)n;
        do { dig[nd++] = (char)('0' + (int)(n64 % 10)); n64 /= 10; } while (n64);
        while (nd < f + 1) dig[nd++] = '0';
        len = 0;
        if (v < 0) buf[len++] = '-';
        for (int i = nd - 1; i >= 0; i--) {
            buf[len++] = dig[i];
            if (i == f && f > 0) buf[len++] = '.';
        }
        buf[len] = 0;
        return len;
    }
    if (mant && e == -(f + 1)) {
        len = snprintf(buf, 160, "%.*f", f + 1, v);
        buf[--len] = 0;                 /* drop the '5' */
        if (f == 0) buf[--len] = 0;     /* drop the '.' */
        int i = len - 1;
        for (;;) {                      /* increment the magnitude */
            if (i < 0 || buf[i] == '-') {
                memmove(buf + (i + 2), buf + (i + 1), (size_t)(len - i)); /* shift incl. NUL */
                buf[i + 1] = '1';
                len++;
                break;
            }
            if (buf[i] == '.') { i--; continue; }
            if (buf[i] == '9') { buf[i--] = '0'; continue; }
            buf[i]++;
            break;
        }
    } else {
        len = snprintf(buf, 160, "%.*f", f, v);
    }
    return len;
}

/* |v|'s first p significant digits into d, rounded as JavaScript does (exact ties away from
 * zero; printf would take them to even), and the power of ten of the first (v finite). */
static void tv_round_digits(double v, int p, char *d, int *e_out) {
    char sci[160], wide[200];
    snprintf(sci, sizeof sci, "%.*e", p - 1, fabs(v));
    snprintf(wide, sizeof wide, "%.*e", p + 40, fabs(v));
    int nd = 0;
    const char *q = sci;
    for (; *q && *q != 'e'; q++)
        if (*q >= '0' && *q <= '9') d[nd++] = *q;
    int e = atoi(q + 1);
    /* a tie: the wide form's digits past p are 5 then only zeros */
    const char *w = wide;
    int k = 0, tie = 1, seen = 0;
    for (; *w && *w != 'e'; w++) {
        if (*w < '0' || *w > '9') continue;
        if (k == p) tie &= *w == '5', seen = 1;
        else if (k > p) tie &= *w == '0';
        k++;
    }
    if (tie && seen) {
        /* round up from the truncated digits */
        nd = 0;
        for (w = wide; *w && *w != 'e' && nd < p; w++)
            if (*w >= '0' && *w <= '9') d[nd++] = *w;
        e = atoi(strchr(wide, 'e') + 1);
        int i = nd - 1;
        while (i >= 0 && d[i] == '9') d[i--] = '0';
        if (i >= 0) d[i]++;
        else { memmove(d + 1, d, (size_t)nd - 1); d[0] = '1'; e++; }
    }
    *e_out = e;
}

tv_str tv_f64_to_exponential(double v, tv_int digits, bool has_digits, const char *loc) {
    if (v != v || isinf(v)) return tv_str_from_f64(v);
    if (has_digits && (digits < 0 || digits > 100)) tv_trap("RangeError: toExponential() argument must be between 0 and 100", loc);
    char d[120], out[200];
    int k, e, n = 0;
    if (v == 0) {
        k = has_digits ? (int)digits + 1 : 1;
        memset(d, '0', (size_t)k);
        e = 0;
    } else if (has_digits) {
        k = (int)digits + 1;
        tv_round_digits(v, k, d, &e);
    } else {
        int pos;
        k = tv_shortest_digits(fabs(v), false, d, &pos);
        e = pos - 1;
    }
    if (v < 0) out[n++] = '-';
    out[n++] = d[0];
    if (k > 1) { out[n++] = '.'; memcpy(out + n, d + 1, (size_t)k - 1); n += k - 1; }
    n += snprintf(out + n, sizeof out - (size_t)n, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    return tv_str_from_ascii(out, (size_t)n);
}

tv_str tv_f64_to_precision(double v, tv_int digits, const char *loc) {
    if (digits < 1 || digits > 100) tv_trap("RangeError: toPrecision() argument must be between 1 and 100", loc);
    if (v != v || isinf(v)) return tv_str_from_f64(v);
    if (v == 0) v = 0.0;
    int p = (int)digits;
    char d[120];
    int e, neg = v < 0;
    if (v == 0) { memset(d, '0', (size_t)p); e = 0; }
    else tv_round_digits(v, p, d, &e);
    char out[200];
    int n = 0;
    if (neg) out[n++] = '-';
    if (e < -6 || e >= p) {
        out[n++] = d[0];
        if (p > 1) { out[n++] = '.'; memcpy(out + n, d + 1, (size_t)p - 1); n += p - 1; }
        n += snprintf(out + n, sizeof out - (size_t)n, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    } else if (e >= 0) {
        memcpy(out + n, d, (size_t)e + 1); n += e + 1;
        if (p > e + 1) { out[n++] = '.'; memcpy(out + n, d + e + 1, (size_t)(p - e - 1)); n += p - e - 1; }
    } else {
        out[n++] = '0'; out[n++] = '.';
        for (int z = 0; z < -e - 1; z++) out[n++] = '0';
        memcpy(out + n, d, (size_t)p); n += p;
    }
    return tv_str_from_ascii(out, (size_t)n);
}

tv_str tv_int_to_fixed(tv_int v, tv_int digits, const char *loc) {
    tv_toFixed_check(digits, loc);
    tv_sb sb = {0};
    tv_sb_push_int(&sb, v);
    if (digits > 0) {
        tv_sb_push_char(&sb, '.');
        memset(tv_sb_reserve(&sb, (size_t)digits), '0', (size_t)digits);
        sb.len += (size_t)digits;
    }
    return tv_str_from_sb(&sb);
}

/* ------------------------------------------------------------------ UTF-8 helpers */

static inline size_t tv_utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1; /* stray continuation byte (invalid UTF-8): treat as one unit */
}

/* Decodes the character at p (n bytes available); returns its byte length. */
static size_t tv_utf8_decode(const unsigned char *p, size_t n, uint32_t *cp) {
    size_t l = tv_utf8_len(p[0]);
    if (l > n) l = n;
    uint32_t c = l == 1 ? p[0] : l == 2 ? p[0] & 0x1F : l == 3 ? p[0] & 0x0F : p[0] & 0x07;
    for (size_t i = 1; i < l; i++) c = (c << 6) | (p[i] & 0x3F);
    *cp = c;
    return l;
}

static inline bool tv_is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

/* Generated by scripts/collation.js from ICU's root collation (Node's Intl): each covered code
 * point's collation elements (primary, secondary, tertiary). A slot is an offset into
 * tv_coll_ce | its count << 24 (0 elements: ignorable); 0xffffffff: not covered (ordered by code
 * point after every covered primary, Han last). */
#define TV_COLL_MAXP 6501
#define TV_COLL_SEC0 1
#define TV_COLL_TER0 1
static const struct { uint32_t lo, hi, off; } tv_coll_ranges[6] = {{0x0, 0x700, 0}, {0x900, 0x980, 1792}, {0x1100, 0x1200, 1920}, {0x1e00, 0x2c00, 2176}, {0x3000, 0x3100, 5760}, {0x1f000, 0x1fb00, 6016}};
static const uint32_t tv_coll_idx[8832] = {
    0,0,0,0,0,0,0,0,0,16777216,16777217,16777218,
    16777219,16777220,5,5,5,5,5,5,5,5,5,5,
    5,5,5,5,5,5,5,5,16777221,16777222,16777223,16777224,
    16777225,16777226,16777227,16777228,16777229,16777230,16777231,16777232,16777233,16777234,16777235,16777236,
    16777237,16777238,16777239,16777240,16777241,16777242,16777243,16777244,16777245,16777246,16777247,16777248,
    16777249,16777250,16777251,16777252,16777253,16777254,16777255,16777256,16777257,16777258,16777259,16777260,
    16777261,16777262,16777263,16777264,16777265,16777266,16777267,16777268,16777269,16777270,16777271,16777272,
    16777273,16777274,16777275,16777276,16777277,16777278,16777279,16777280,16777281,16777282,16777283,16777284,
    16777285,16777286,16777287,16777288,16777289,16777290,16777291,16777292,16777293,16777294,16777295,16777296,
    16777297,16777298,16777299,16777300,16777301,16777302,16777303,16777304,16777305,16777306,16777307,16777308,
    16777309,16777310,16777311,16777312,16777313,16777314,16777315,100,100,100,100,100,
    100,16777316,101,101,101,101,101,101,101,101,101,101,
    101,101,101,101,101,101,101,101,101,101,101,101,
    101,101,101,101,16777317,16777318,16777319,16777320,16777321,16777322,16777323,16777324,
    16777325,16777326,16777327,16777328,16777329,114,16777330,16777331,16777332,16777333,16777334,16777335,
    16777336,16777337,16777338,16777339,16777340,16777341,16777342,16777343,50331776,50331779,50331782,16777353,
    33554570,33554572,33554574,33554576,33554578,33554580,33554582,33554584,33554586,33554588,33554590,33554592,
    33554594,33554596,33554598,33554600,33554602,33554604,33554606,33554608,33554610,33554612,33554614,16777400,
    33554617,33554619,33554621,33554623,33554625,33554627,16777413,33554630,33554632,33554634,33554636,33554638,
    33554640,33554642,33554644,33554646,33554648,33554650,33554652,33554654,33554656,33554658,33554660,33554662,
    33554664,33554666,33554668,33554670,33554672,33554674,33554676,16777462,33554679,33554681,33554683,33554685,
    33554687,33554689,16777475,33554692,33554694,33554696,33554698,33554700,33554702,33554704,33554706,33554708,
    33554710,33554712,33554714,33554716,33554718,33554720,33554722,33554724,33554726,33554728,33554730,33554732,
    33554734,33554736,33554738,33554740,33554742,33554744,33554746,33554748,33554750,33554752,33554754,33554756,
    33554758,33554760,33554762,33554764,33554766,33554768,33554770,33554772,33554774,33554776,33554778,33554780,
    33554782,33554784,33554786,33554788,33554790,16777576,33554793,33554795,33554797,33554799,33554801,33554803,
    16777589,33554806,33554808,33554810,33554812,33554814,33554816,33554818,33554820,33554822,33554824,33554826,
    33554828,33554830,33554832,33554834,33554836,33554838,16777624,16777625,33554842,33554844,33554846,33554848,
    33554850,33554852,33554854,33554856,33554858,33554860,33554862,33554864,33554866,33554868,33554870,33554872,
    33554874,33554876,33554878,33554880,33554882,33554884,33554886,33554888,33554890,33554892,16777678,16777679,
    33554896,33554898,33554900,33554902,33554904,33554906,33554908,33554910,33554912,33554914,33554916,33554918,
    33554920,33554922,33554924,33554926,33554928,33554930,33554932,33554934,33554936,33554938,33554940,33554942,
    16777728,16777729,16777730,16777731,16777732,16777733,16777734,16777735,16777736,16777737,16777738,16777739,
    16777740,33554957,16777743,16777744,16777745,16777746,16777747,16777748,16777749,16777750,16777751,16777752,
    16777753,16777754,16777755,16777756,16777757,16777758,16777759,16777760,33554977,33554979,16777765,16777766,
    16777767,16777768,16777769,16777770,16777771,16777772,16777773,16777774,16777775,16777776,16777777,33554994,
    33554996,16777782,16777783,16777784,16777785,16777786,16777787,16777788,16777789,16777790,16777791,16777792,
    16777793,16777794,33555011,16777797,16777798,16777799,16777800,16777801,50332234,50332237,50332240,33555027,
    33555029,33555031,33555033,33555035,33555037,33555039,33555041,33555043,33555045,33555047,33555049,33555051,
    33555053,50332271,50332274,50332277,50332280,50332283,50332286,50332289,50332292,16777863,50332296,50332299,
    50332302,50332305,50332308,50332311,16777882,16777883,33555100,33555102,33555104,33555106,33555108,33555110,
    50332328,50332331,33555118,33555120,33555122,33555124,33555126,33555128,33555130,33555132,16777918,16777919,
    33555136,33555138,50332356,50332359,50332362,50332365,50332368,50332371,33555158,33555160,33555162,33555164,
    33555166,33555168,33555170,33555172,33555174,33555176,33555178,33555180,33555182,33555184,33555186,33555188,
    33555190,33555192,33555194,33555196,33555198,33555200,33555202,33555204,33555206,33555208,33555210,33555212,
    16777998,16777999,33555216,33555218,16778004,16778005,16778006,16778007,16778008,16778009,33555226,33555228,
    33555230,33555232,50332450,50332453,50332456,50332459,33555246,33555248,50332466,50332469,33555256,33555258,
    16778044,16778045,16778046,16778047,33555264,33555266,16778052,16778053,16778054,16778055,16778056,16778057,
    16778058,16778059,16778060,16778061,16778062,16778063,16778064,16778065,16778066,16778067,16778068,16778069,
    16778070,16778071,16778072,16778073,16778074,16778075,16778076,16778077,16778078,16778079,16778080,16778081,
    16778082,16778083,16778084,16778085,16778086,16778087,16778088,16778089,16778090,16778091,16778092,16778093,
    16778094,16778095,16778096,16778097,16778098,16778099,16778100,16778101,16778102,16778103,16778104,16778105,
    16778106,16778107,16778108,16778109,16778110,16778111,16778112,16778113,16778114,16778115,16778116,16778117,
    16778118,16778119,16778120,16778121,16778122,16778123,16778124,16778125,16778126,16778127,16778128,16778129,
    16778130,16778131,16778132,16778133,16778134,16778135,16778136,16778137,16778138,16778139,16778140,16778141,
    16778142,16778143,16778144,16778145,16778146,16778147,16778148,16778149,16778150,16778151,16778152,16778153,
    16778154,16778155,16778156,33555373,16778159,16778160,33555377,16778163,16778164,16778165,33555382,33555384,
    16778170,16778171,16778172,16778173,16778174,16778175,16778176,16778177,16778178,16778179,16778180,16778181,
    16778182,16778183,16778184,16778185,16778186,16778187,16778188,16778189,16778190,16778191,16778192,16778193,
    16778194,16778195,16778196,16778197,16778198,16778199,16778200,16778201,16778202,16778203,16778204,16778205,
    16778206,16778207,16778208,16778209,16778210,16778211,16778212,16778213,16778214,16778215,16778216,16778217,
    16778218,16778219,16778220,16778221,16778222,16778223,16778224,16778225,16778226,16778227,16778228,16778229,
    16778230,16778231,16778232,16778233,16778234,16778235,16778236,16778237,16778238,16778239,16778240,16778241,
    16778242,16778243,16778244,16778245,16778246,16778247,16778248,16778249,16778250,16778251,16778252,16778253,
    16778254,16778255,16778256,16778257,16778258,16778259,16778260,16778261,16778262,16778263,16778264,16778265,
    16778266,16778267,16778268,16778269,16778270,16778271,16778272,16778273,16778274,16778275,16778276,16778277,
    16778278,16778279,16778280,16778281,16778282,16778283,16778284,16778285,16778286,16778287,16778288,16778289,
    16778290,16778291,16778292,16778293,16778294,16778295,16778296,16778297,16778298,16778299,16778300,16778301,
    16778302,16778303,16778304,16778305,16778306,16778307,16778308,16778309,16778310,16778311,16778312,16778313,
    16778314,16778315,16778316,16778317,16778318,16778319,16778320,16778321,33555538,16778324,16778325,16778326,
    16778327,16778328,16778329,16778330,16778331,16778332,16778333,16778334,16778335,16778336,16778337,16778338,
    16778339,16778340,16778341,16778342,16778343,16778344,16778345,16778346,16778347,16778348,16778349,16778350,
    16778351,16778352,16778353,16778354,16778355,16778356,16778357,16778358,16778359,16778360,16778361,16778362,
    16778363,16778364,16778365,16778366,16778367,16778368,16778369,16778370,16778371,16778372,16778373,16778374,
    16778375,16778376,16778377,16778378,16778379,16778380,16778381,16778382,16778383,16778384,16778385,16778386,
    16778387,33555604,33555606,16778392,33555609,33555611,33555613,16778399,33555616,16778402,33555619,33555621,
    50332839,16778410,16778411,16778412,16778413,16778414,16778415,16778416,16778417,16778418,16778419,16778420,
    16778421,16778422,16778423,16778424,16778425,16778426,16778427,16778428,16778429,16778430,16778431,16778432,
    16778433,16778434,33555651,33555653,33555655,33555657,33555659,33555661,50332879,16778450,16778451,16778452,
    16778453,16778454,16778455,16778456,16778457,16778458,16778459,16778460,16778461,16778462,16778463,16778464,
    16778465,16778466,16778467,16778468,16778469,16778470,16778471,16778472,16778473,16778474,33555691,33555693,
    33555695,33555697,33555699,16778485,16778486,16778487,16778488,33555705,33555707,16778493,16778494,16778495,
    16778496,16778497,16778498,16778499,16778500,16778501,16778502,16778503,16778504,16778505,16778506,16778507,
    16778508,16778509,16778510,16778511,16778512,16778513,16778514,16778515,16778516,16778517,16778518,16778519,
    16778520,16778521,16778522,16778523,16778524,16778525,16778526,16778527,16778528,16778529,16778530,16778531,
    16778532,16778533,16778534,16778535,33555752,33555754,16778540,33555757,16778543,16778544,16778545,33555762,
    16778548,16778549,16778550,16778551,33555768,33555770,33555772,16778558,16778559,16778560,16778561,16778562,
    16778563,16778564,16778565,16778566,16778567,33555784,16778570,16778571,16778572,16778573,16778574,16778575,
    16778576,16778577,16778578,16778579,16778580,16778581,16778582,16778583,16778584,16778585,16778586,16778587,
    16778588,16778589,16778590,16778591,16778592,16778593,16778594,16778595,16778596,16778597,16778598,16778599,
    16778600,33555817,16778603,16778604,16778605,16778606,16778607,16778608,16778609,16778610,16778611,16778612,
    16778613,16778614,16778615,16778616,16778617,16778618,16778619,16778620,16778621,16778622,16778623,16778624,
    33555841,33555843,16778629,33555846,16778632,16778633,16778634,33555851,16778637,16778638,16778639,16778640,
    33555857,33555859,33555861,16778647,16778648,16778649,16778650,16778651,16778652,16778653,16778654,16778655,
    16778656,16778657,16778658,16778659,16778660,16778661,16778662,16778663,16778664,16778665,16778666,16778667,
    16778668,16778669,33555886,33555888,16778674,16778675,16778676,16778677,16778678,16778679,16778680,16778681,
    16778682,16778683,16778684,33555901,33555903,33555905,16778691,33555908,1478,1478,16778694,16778695,
    16778696,16778697,16778698,16778699,33555916,33555918,16778704,16778705,16778706,16778707,16778708,16778709,
    16778710,16778711,16778712,16778713,16778714,16778715,16778716,16778717,16778718,16778719,16778720,16778721,
    16778722,16778723,16778724,16778725,16778726,16778727,16778728,16778729,16778730,16778731,16778732,16778733,
    16778734,16778735,16778736,16778737,16778738,16778739,16778740,16778741,16778742,16778743,16778744,16778745,
    16778746,16778747,16778748,16778749,16778750,33555967,33555969,16778755,16778756,16778757,16778758,16778759,
    16778760,16778761,16778762,16778763,16778764,16778765,16778766,16778767,33555984,33555986,33555988,33555990,
    16778776,16778777,33555994,33555996,16778782,16778783,33556000,33556002,33556004,33556006,33556008,33556010,
    16778796,16778797,33556014,33556016,33556018,33556020,33556022,33556024,16778810,16778811,33556028,33556030,
    33556032,33556034,33556036,33556038,33556040,33556042,33556044,33556046,33556048,33556050,16778836,16778837,
    33556054,33556056,16778842,16778843,16778844,16778845,16778846,16778847,16778848,16778849,16778850,16778851,
    16778852,16778853,16778854,16778855,16778856,16778857,16778858,16778859,16778860,16778861,16778862,16778863,
    16778864,16778865,16778866,16778867,16778868,16778869,16778870,16778871,16778872,16778873,16778874,16778875,
    16778876,16778877,16778878,16778879,16778880,16778881,16778882,16778883,16778884,16778885,16778886,16778887,
    16778888,16778889,16778890,16778891,16778892,16778893,16778894,16778895,16778896,16778897,16778898,16778899,
    16778900,16778901,16778902,16778903,16778904,16778905,16778906,16778907,16778908,16778909,16778910,16778911,
    16778912,16778913,16778914,16778915,16778916,16778917,16778918,16778919,16778920,16778921,16778922,16778923,
    16778924,16778925,16778926,16778927,16778928,16778929,16778930,16778931,16778932,16778933,16778934,16778935,
    16778936,16778937,16778938,16778939,16778940,16778941,16778942,16778943,16778944,16778945,16778946,16778947,
    16778948,16778949,16778950,16778951,16778952,16778953,16778954,16778955,16778956,16778957,16778958,16778959,
    16778960,16778961,16778962,16778963,16778964,16778965,16778966,16778967,16778968,16778969,16778970,16778971,
    16778972,16778973,16778974,16778975,16778976,16778977,16778978,16778979,16778980,16778981,16778982,33556199,
    16778985,16778986,16778987,16778988,16778989,16778990,16778991,16778992,16778993,1778,1778,1778,
    1778,1778,1778,1778,1778,1778,1778,1778,1778,1778,1778,1778,
    1778,1778,1778,1778,1778,1778,1778,1778,1778,1778,1778,1778,
    1778,1778,1778,1778,33556210,33556212,33556214,33556216,33556218,33556220,33556222,33556224,
    33556226,33556228,33556230,33556232,33556234,1804,16779020,33556237,16779023,33556240,33556242,16779028,
    1813,1813,16779029,33556246,16779032,16779033,16779034,16779035,16779036,16779037,16779038,16779039,
    16779040,16779041,16779042,16779043,16779044,16779045,16779046,16779047,16779048,16779049,16779050,16779051,
    16779052,16779053,16779054,16779055,16779056,16779057,16779058,16779059,16779060,16779061,16779062,16779063,
    16779064,16779065,16779066,16779067,16779068,16779069,16779070,16779071,16779072,16779073,16779074,33556291,
    33556293,16779079,16779080,16779081,16779082,16779083,16779084,16779085,16779086,16779087,16779088,16779089,
    1874,1874,1874,1874,1874,1874,16779090,16779091,16779092,16779093,16779094,16779095,
    16779096,16779097,16779098,16779099,1884,1884,1884,1884,1884,1884,1884,1884,
    1884,1884,1884,16779100,1885,16779101,16779102,16779103,16779104,16779105,50333538,50333541,
    50333544,50333547,50333550,16779121,16779122,16779123,16779124,16779125,16779126,16779127,16779128,16779129,
    16779130,16779131,16779132,16779133,16779134,16779135,16779136,16779137,16779138,16779139,16779140,16779141,
    16779142,16779143,16779144,16779145,1930,16779146,16779147,16779148,16779149,16779150,16779151,16779152,
    16779153,16779154,16779155,33556372,33556374,33556376,33556378,33556380,33556382,33556384,33556386,33556388,
    33556390,33556392,33556394,33556396,33556398,33556400,33556402,33556404,33556406,33556408,33556410,33556412,
    16779198,16779199,16779200,16779201,16779202,16779203,16779204,16779205,16779206,16779207,16779208,16779209,
    16779210,16779211,16779212,16779213,33556430,16779216,16779217,16779218,16779219,16779220,16779221,16779222,
    16779223,16779224,16779225,16779226,16779227,16779228,16779229,16779230,16779231,16779232,16779233,16779234,
    16779235,16779236,16779237,16779238,16779239,16779240,16779241,16779242,16779243,16779244,16779245,16779246,
    16779247,16779248,16779249,16779250,16779251,16779252,16779253,16779254,16779255,16779256,16779257,16779258,
    16779259,16779260,16779261,16779262,16779263,16779264,16779265,16779266,16779267,16779268,16779269,16779270,
    16779271,16779272,16779273,16779274,16779275,16779276,16779277,16779278,16779279,16779280,16779281,16779282,
    16779283,16779284,16779285,16779286,16779287,16779288,16779289,16779290,16779291,16779292,16779293,16779294,
    50333727,16779298,50333731,16779302,16779303,16779304,16779305,16779306,16779307,16779308,16779309,16779310,
    16779311,16779312,16779313,16779314,16779315,16779316,16779317,50333750,16779321,16779322,2107,2107,
    2107,2107,2107,2107,2107,2107,16779323,2108,2108,2108,2108,2108,
    2108,16779324,16779325,2110,2110,16779326,2111,2111,2111,2111,16779327,16779328,
    16779329,16779330,16779331,16779332,16779333,16779334,16779335,16779336,16779337,16779338,16779339,16779340,
    16779341,33556558,33556560,16779346,33556563,33556565,33556567,33556569,16779355,16779356,16779357,16779358,
    16779359,16779360,16779361,16779362,16779363,16779364,16779365,16779366,16779367,16779368,16779369,16779370,
    16779371,16779372,16779373,16779374,16779375,16779376,16779377,16779378,16779379,16779380,16779381,16779382,
    16779383,16779384,16779385,16779386,16779387,16779388,16779389,16779390,16779391,50333824,16779395,16779396,
    16779397,16779398,16779399,16779400,16779401,50333834,16779405,16779406,50333839,16779410,16779411,16779412,
    16779413,16779414,16779415,16779416,33556633,16779419,16779420,16779421,16779422,16779423,16779424,16779425,
    16779426,16779427,16779428,16779429,16779430,16779431,16779432,16779433,16779434,16779435,16779436,16779437,
    16779438,2223,2223,33556655,33556657,16779443,16779444,16779445,50333878,50333881,50333884,50333887,
    50333890,50333893,50333896,50333899,16779470,16779471,16779472,16779473,16779474,16779475,16779476,16779477,
    16779478,16779479,16779480,16779481,16779482,16779483,16779484,16779485,16779486,16779487,16779488,16779489,
    16779490,16779491,16779492,16779493,16779494,16779495,16779496,16779497,16779498,16779499,16779500,16779501,
    16779502,16779503,16779504,16779505,16779506,16779507,16779508,16779509,16779510,16779511,16779512,16779513,
    16779514,16779515,16779516,16779517,16779518,16779519,16779520,16779521,16779522,16779523,16779524,16779525,
    16779526,16779527,16779528,16779529,16779530,16779531,16779532,16779533,16779534,16779535,16779536,16779537,
    16779538,16779539,16779540,16779541,16779542,16779543,16779544,16779545,16779546,16779547,16779548,16779549,
    16779550,16779551,16779552,16779553,16779554,16779555,16779556,16779557,16779558,16779559,16779560,16779561,
    16779562,16779563,16779564,16779565,16779566,16779567,16779568,16779569,16779570,16779571,16779572,16779573,
    16779574,16779575,16779576,16779577,16779578,16779579,16779580,16779581,16779582,16779583,16779584,16779585,
    16779586,16779587,16779588,16779589,16779590,16779591,16779592,16779593,16779594,16779595,16779596,16779597,
    16779598,16779599,16779600,16779601,16779602,16779603,16779604,16779605,16779606,16779607,16779608,16779609,
    16779610,16779611,16779612,16779613,16779614,16779615,16779616,16779617,16779618,16779619,16779620,16779621,
    16779622,16779623,16779624,16779625,16779626,16779627,16779628,16779629,16779630,16779631,16779632,16779633,
    16779634,16779635,16779636,16779637,16779638,16779639,16779640,16779641,16779642,16779643,16779644,16779645,
    16779646,16779647,16779648,16779649,16779650,16779651,16779652,16779653,16779654,16779655,16779656,16779657,
    16779658,16779659,16779660,16779661,16779662,16779663,16779664,16779665,16779666,16779667,16779668,16779669,
    16779670,16779671,16779672,16779673,16779674,16779675,16779676,16779677,16779678,16779679,16779680,16779681,
    16779682,16779683,16779684,16779685,16779686,16779687,16779688,16779689,16779690,16779691,16779692,16779693,
    16779694,16779695,16779696,16779697,16779698,16779699,16779700,16779701,16779702,16779703,16779704,16779705,
    16779706,16779707,16779708,16779709,16779710,16779711,16779712,16779713,16779714,16779715,16779716,16779717,
    16779718,16779719,16779720,16779721,16779722,16779723,16779724,16779725,16779726,16779727,16779728,16779729,
    16779730,16779731,16779732,16779733,16779734,16779735,16779736,16779737,16779738,16779739,16779740,16779741,
    16779742,16779743,16779744,16779745,16779746,16779747,16779748,16779749,16779750,16779751,16779752,16779753,
    16779754,16779755,16779756,16779757,33556974,33556976,33556978,33556980,33556982,33556984,33556986,33556988,
    50334206,50334209,33556996,33556998,33557000,33557002,33557004,33557006,33557008,33557010,33557012,33557014,
    50334232,50334235,50334238,50334241,33557028,33557030,33557032,33557034,50334252,50334255,33557042,33557044,
    33557046,33557048,33557050,33557052,33557054,33557056,33557058,33557060,33557062,33557064,33557066,33557068,
    33557070,33557072,50334290,50334293,33557080,33557082,33557084,33557086,33557088,33557090,33557092,33557094,
    50334312,50334315,33557102,33557104,33557106,33557108,33557110,33557112,33557114,33557116,33557118,33557120,
    33557122,33557124,33557126,33557128,33557130,33557132,33557134,33557136,50334354,50334357,50334360,50334363,
    50334366,50334369,50334372,50334375,33557162,33557164,33557166,33557168,33557170,33557172,33557174,33557176,
    50334394,50334397,33557184,33557186,33557188,33557190,33557192,33557194,50334412,50334415,50334418,50334421,
    50334424,50334427,33557214,33557216,33557218,33557220,33557222,33557224,33557226,33557228,33557230,33557232,
    33557234,33557236,33557238,33557240,50334458,50334461,50334464,50334467,33557254,33557256,33557258,33557260,
    33557262,33557264,33557266,33557268,33557270,33557272,33557274,33557276,33557278,33557280,33557282,33557284,
    33557286,33557288,33557290,33557292,33557294,33557296,33557298,33557300,33557302,33557304,33557306,33557308,
    33557310,33557312,33557314,50334532,16780103,16780104,33557321,16780107,33557324,33557326,33557328,33557330,
    50334548,50334551,50334554,50334557,50334560,50334563,50334566,50334569,50334572,50334575,50334578,50334581,
    50334584,50334587,50334590,50334593,50334596,50334599,50334602,50334605,33557392,33557394,33557396,33557398,
    33557400,33557402,50334620,50334623,50334626,50334629,50334632,50334635,50334638,50334641,50334644,50334647,
    33557434,33557436,33557438,33557440,33557442,33557444,33557446,33557448,50334666,50334669,50334672,50334675,
    50334678,50334681,50334684,50334687,50334690,50334693,50334696,50334699,50334702,50334705,50334708,50334711,
    50334714,50334717,50334720,50334723,33557510,33557512,33557514,33557516,50334734,50334737,50334740,50334743,
    50334746,50334749,50334752,50334755,50334758,50334761,33557548,33557550,33557552,33557554,33557556,33557558,
    33557560,33557562,33557564,33557566,16780352,16780353,16780354,16780355,33557572,33557574,50334792,50334795,
    50334798,50334801,50334804,50334807,33557594,33557596,50334814,50334817,50334820,50334823,50334826,50334829,
    33557616,33557618,50334836,50334839,50334842,50334845,16780416,16780417,33557634,33557636,50334854,50334857,
    50334860,50334863,16780434,16780435,33557652,33557654,50334872,50334875,50334878,50334881,50334884,50334887,
    33557674,33557676,50334894,50334897,50334900,50334903,50334906,50334909,33557696,33557698,50334916,50334919,
    50334922,50334925,50334928,50334931,33557718,33557720,50334938,50334941,50334944,50334947,50334950,50334953,
    33557740,33557742,50334960,50334963,50334966,50334969,16780540,16780541,33557758,33557760,50334978,50334981,
    50334984,50334987,16780558,16780559,33557776,33557778,50334996,50334999,50335002,50335005,50335008,50335011,
    16780582,33557799,16780585,50335018,16780589,50335022,16780593,50335026,33557813,33557815,50335033,50335036,
    50335039,50335042,50335045,50335048,33557835,33557837,50335055,50335058,50335061,50335064,50335067,50335070,
    33557857,33557859,33557861,33557863,33557865,33557867,33557869,33557871,33557873,33557875,33557877,33557879,
    33557881,33557883,16780669,16780670,50335103,50335106,67112325,67112329,67112333,67112337,67112341,67112345,
    50335133,50335136,67112355,67112359,67112363,67112367,67112371,67112375,50335163,50335166,67112385,67112389,
    67112393,67112397,67112401,67112405,50335193,50335196,67112415,67112419,67112423,67112427,67112431,67112435,
    50335223,50335226,67112445,67112449,67112453,67112457,67112461,67112465,50335253,50335256,67112475,67112479,
    67112483,67112487,67112491,67112495,33558067,33558069,50335287,33558074,50335292,16780863,33558080,50335298,
    33558085,33558087,33558089,33558091,33558093,16780879,16780880,16780881,16780882,33558099,50335317,33558104,
    50335322,16780893,33558110,50335328,33558115,33558117,33558119,33558121,33558123,33558125,33558127,33558129,
    33558131,33558133,50335351,50335354,16780925,16780926,33558143,50335361,33558148,33558150,33558152,33558154,
    16780940,33558157,33558159,33558161,33558163,33558165,50335383,50335386,33558173,33558175,33558177,50335395,
    33558182,33558184,33558186,33558188,33558190,33558192,33558194,16780980,16780981,16780982,50335415,33558202,
    50335420,16780991,33558208,50335426,33558213,33558215,33558217,33558219,33558221,16781007,16781008,16781009,
    16781010,16781011,16781012,16781013,16781014,16781015,16781016,16781017,16781018,16781019,16781020,3805,
    3805,3805,3805,3805,16781021,16781022,16781023,16781024,16781025,16781026,16781027,16781028,
    33558245,33558247,33558249,33558251,33558253,33558255,33558257,33558259,16781045,16781046,16781047,16781048,
    16781049,33558266,50335484,16781055,16781056,16781057,3842,3842,3842,3842,3842,16781058,
    16781059,16781060,16781061,33558278,50335496,16781067,33558284,50335502,16781073,16781074,16781075,16781076,
    33558293,16781079,16781080,16781081,16781082,16781083,16781084,16781085,16781086,16781087,16781088,33558305,
    33558307,33558309,16781095,16781096,16781097,16781098,16781099,16781100,16781101,16781102,16781103,16781104,
    16781105,16781106,16781107,67112756,16781112,16781113,16781114,16781115,16781116,16781117,16781118,16781119,
    3904,3904,3904,3904,3904,16781120,3905,3905,3905,3905,3905,3905,
    3905,3905,3905,3905,16781121,16781122,16781123,16781124,16781125,16781126,16781127,16781128,
    16781129,16781130,16781131,16781132,16781133,16781134,16781135,16781136,16781137,16781138,16781139,16781140,
    16781141,16781142,16781143,16781144,16781145,16781146,16781147,16781148,16781149,16781150,16781151,16781152,
    16781153,16781154,16781155,16781156,16781157,16781158,16781159,16781160,16781161,16781162,16781163,16781164,
    16781165,16781166,16781167,16781168,16781169,16781170,16781171,16781172,16781173,16781174,16781175,16781176,
    33558393,16781179,16781180,16781181,16781182,16781183,16781184,16781185,16781186,16781187,16781188,16781189,
    16781190,16781191,16781192,16781193,16781194,16781195,16781196,16781197,16781198,16781199,16781200,16781201,
    16781202,16781203,16781204,16781205,16781206,16781207,16781208,16781209,16781210,16781211,16781212,16781213,
    16781214,16781215,16781216,16781217,16781218,16781219,16781220,16781221,16781222,16781223,16781224,16781225,
    16781226,16781227,16781228,16781229,16781230,16781231,16781232,16781233,16781234,16781235,16781236,16781237,
    16781238,16781239,16781240,16781241,16781242,16781243,16781244,16781245,16781246,16781247,16781248,16781249,
    16781250,16781251,16781252,16781253,16781254,16781255,16781256,16781257,16781258,16781259,16781260,16781261,
    16781262,16781263,16781264,16781265,50335698,50335701,16781272,33558489,16781275,50335708,50335711,16781282,
    16781283,33558500,16781286,16781287,16781288,16781289,16781290,33558507,16781293,16781294,16781295,16781296,
    16781297,16781298,33558515,16781301,16781302,16781303,16781304,16781305,16781306,16781307,16781308,16781309,
    33558526,50335744,33558531,16781317,16781318,16781319,16781320,16781321,16781322,16781323,16781324,33558541,
    16781327,16781328,16781329,16781330,16781331,16781332,16781333,16781334,16781335,16781336,16781337,16781338,
    16781339,16781340,16781341,50335774,16781345,16781346,16781347,16781348,16781349,16781350,16781351,16781352,
    16781353,16781354,16781355,16781356,16781357,16781358,16781359,16781360,16781361,16781362,16781363,16781364,
    50335797,50335800,67113019,50335807,50335810,50335813,50335816,50335819,50335822,50335825,50335828,50335831,
    50335834,50335837,50335840,33558627,16781413,33558630,50335848,33558635,16781421,33558638,50335856,67113075,
    33558647,16781433,33558650,50335868,16781439,16781440,16781441,16781442,16781443,33558660,50335878,33558665,
    16781451,33558668,50335886,67113105,33558677,16781463,33558680,50335898,16781469,16781470,16781471,16781472,
    16781473,16781474,16781475,16781476,16781477,16781478,16781479,16781480,16781481,50335914,16781485,16781486,
    16781487,16781488,16781489,16781490,16781491,16781492,16781493,16781494,16781495,16781496,16781497,16781498,
    16781499,16781500,33558717,33558719,16781505,16781506,16781507,16781508,16781509,16781510,16781511,16781512,
    16781513,16781514,16781515,16781516,16781517,16781518,16781519,16781520,16781521,16781522,33558739,16781525,
    16781526,16781527,16781528,16781529,16781530,16781531,16781532,16781533,16781534,16781535,16781536,16781537,
    16781538,16781539,16781540,16781541,16781542,16781543,16781544,16781545,16781546,16781547,16781548,16781549,
    16781550,16781551,16781552,16781553,16781554,33558771,33558773,33558775,16781561,16781562,16781563,16781564,
    16781565,16781566,16781567,16781568,16781569,16781570,16781571,16781572,16781573,16781574,16781575,16781576,
    16781577,16781578,16781579,16781580,16781581,16781582,16781583,16781584,16781585,16781586,16781587,16781588,
    16781589,16781590,16781591,16781592,16781593,16781594,16781595,16781596,16781597,16781598,16781599,16781600,
    16781601,16781602,16781603,16781604,16781605,16781606,16781607,16781608,16781609,16781610,16781611,16781612,
    33558829,16781615,16781616,16781617,16781618,33558835,16781621,16781622,33558839,16781625,16781626,16781627,
    16781628,16781629,16781630,16781631,16781632,16781633,16781634,16781635,16781636,16781637,16781638,16781639,
    16781640,16781641,16781642,16781643,16781644,16781645,16781646,16781647,33558864,16781650,33558867,16781653,
    16781654,16781655,16781656,16781657,33558874,50336092,16781663,33558880,50336098,16781669,16781670,16781671,
    16781672,16781673,16781674,16781675,16781676,16781677,16781678,16781679,16781680,16781681,16781682,16781683,
    16781684,33558901,16781687,16781688,33558905,16781691,16781692,33558909,16781695,33558912,16781698,16781699,
    16781700,16781701,16781702,16781703,16781704,16781705,16781706,16781707,16781708,16781709,16781710,16781711,
    16781712,16781713,16781714,16781715,16781716,16781717,16781718,16781719,33558936,16781722,33558939,16781725,
    16781726,16781727,16781728,16781729,16781730,16781731,16781732,16781733,16781734,33558951,33558953,33558955,
    33558957,33558959,16781745,16781746,33558963,33558965,16781751,16781752,33558969,33558971,16781757,16781758,
    16781759,16781760,16781761,16781762,33558979,33558981,16781767,16781768,33558985,33558987,16781773,16781774,
    33558991,33558993,16781779,16781780,16781781,16781782,16781783,16781784,16781785,16781786,16781787,16781788,
    16781789,16781790,16781791,16781792,16781793,16781794,16781795,16781796,16781797,16781798,16781799,16781800,
    16781801,16781802,16781803,16781804,16781805,16781806,16781807,16781808,16781809,16781810,16781811,16781812,
    33559029,33559031,33559033,33559035,16781821,16781822,16781823,16781824,16781825,16781826,16781827,16781828,
    16781829,16781830,16781831,16781832,16781833,16781834,16781835,16781836,16781837,16781838,16781839,16781840,
    16781841,16781842,16781843,16781844,16781845,16781846,16781847,16781848,16781849,16781850,16781851,16781852,
    16781853,16781854,16781855,16781856,16781857,16781858,16781859,16781860,16781861,16781862,16781863,16781864,
    16781865,16781866,16781867,16781868,33559085,33559087,33559089,33559091,16781877,16781878,16781879,16781880,
    16781881,16781882,33559099,33559101,33559103,33559105,16781891,16781892,16781893,16781894,16781895,16781896,
    16781897,16781898,16781899,16781900,16781901,16781902,16781903,16781904,16781905,16781906,16781907,16781908,
    16781909,16781910,16781911,16781912,16781913,16781914,16781915,16781916,16781917,16781918,16781919,16781920,
    16781921,16781922,16781923,16781924,16781925,16781926,16781927,16781928,16781929,16781930,16781931,16781932,
    16781933,16781934,16781935,16781936,16781937,16781938,16781939,16781940,16781941,16781942,16781943,16781944,
    16781945,16781946,16781947,16781948,16781949,16781950,16781951,16781952,16781953,16781954,16781955,16781956,
    16781957,16781958,16781959,16781960,16781961,16781962,16781963,16781964,16781965,16781966,16781967,16781968,
    16781969,16781970,16781971,16781972,16781973,16781974,16781975,16781976,16781977,16781978,16781979,16781980,
    16781981,16781982,16781983,16781984,16781985,16781986,16781987,16781988,16781989,16781990,16781991,16781992,
    16781993,16781994,16781995,16781996,16781997,16781998,16781999,16782000,16782001,16782002,16782003,16782004,
    16782005,16782006,16782007,16782008,16782009,16782010,16782011,16782012,16782013,16782014,16782015,16782016,
    16782017,16782018,16782019,16782020,16782021,16782022,16782023,16782024,16782025,16782026,16782027,16782028,
    16782029,16782030,16782031,16782032,16782033,16782034,16782035,16782036,16782037,16782038,16782039,16782040,
    16782041,16782042,16782043,16782044,16782045,16782046,16782047,16782048,16782049,16782050,16782051,16782052,
    16782053,16782054,16782055,16782056,16782057,16782058,16782059,16782060,16782061,16782062,16782063,16782064,
    16782065,16782066,16782067,16782068,16782069,16782070,16782071,16782072,16782073,16782074,16782075,16782076,
    16782077,16782078,16782079,16782080,16782081,16782082,16782083,16782084,16782085,16782086,16782087,16782088,
    16782089,16782090,16782091,16782092,16782093,16782094,16782095,16782096,16782097,16782098,16782099,16782100,
    16782101,16782102,16782103,16782104,16782105,16782106,16782107,16782108,16782109,16782110,16782111,16782112,
    16782113,16782114,16782115,16782116,16782117,16782118,16782119,16782120,16782121,16782122,16782123,16782124,
    16782125,16782126,16782127,16782128,16782129,16782130,16782131,16782132,16782133,16782134,16782135,16782136,
    16782137,16782138,16782139,16782140,16782141,16782142,16782143,16782144,16782145,16782146,16782147,16782148,
    16782149,16782150,16782151,16782152,16782153,16782154,16782155,16782156,16782157,16782158,16782159,16782160,
    16782161,16782162,16782163,16782164,16782165,16782166,16782167,16782168,16782169,16782170,16782171,16782172,
    16782173,16782174,16782175,16782176,16782177,16782178,16782179,16782180,16782181,16782182,16782183,16782184,
    16782185,16782186,16782187,16782188,16782189,16782190,16782191,16782192,16782193,16782194,16782195,16782196,
    16782197,16782198,16782199,16782200,16782201,16782202,16782203,16782204,16782205,16782206,16782207,16782208,
    16782209,16782210,16782211,16782212,16782213,16782214,16782215,16782216,16782217,16782218,16782219,16782220,
    16782221,16782222,16782223,16782224,16782225,16782226,16782227,16782228,16782229,16782230,16782231,16782232,
    16782233,16782234,16782235,16782236,16782237,16782238,16782239,16782240,16782241,16782242,16782243,16782244,
    16782245,16782246,16782247,16782248,16782249,16782250,16782251,16782252,16782253,16782254,16782255,16782256,
    16782257,16782258,16782259,16782260,16782261,16782262,16782263,16782264,16782265,16782266,16782267,16782268,
    16782269,33559486,33559488,33559490,33559492,33559494,33559496,33559498,33559500,33559502,33559504,33559506,
    50336724,50336727,50336730,50336733,50336736,50336739,50336742,50336745,50336748,67113967,67113971,67113975,
    67113979,67113983,67113987,67113991,67113995,67113999,67114003,67114007,33559579,33559581,33559583,33559585,
    33559587,33559589,33559591,33559593,33559595,50336813,50336816,50336819,50336822,50336825,50336828,50336831,
    50336834,50336837,50336840,50336843,50336846,50336849,50336852,50336855,50336858,50336861,50336864,50336867,
    50336870,50336873,50336876,50336879,50336882,50336885,50336888,50336891,50336894,50336897,50336900,50336903,
    50336906,50336909,50336912,50336915,50336918,50336921,16782492,16782493,16782494,16782495,16782496,16782497,
    16782498,16782499,16782500,16782501,16782502,16782503,16782504,16782505,16782506,16782507,16782508,16782509,
    16782510,16782511,16782512,16782513,16782514,16782515,16782516,16782517,16782518,16782519,16782520,16782521,
    16782522,16782523,16782524,16782525,16782526,16782527,16782528,16782529,16782530,16782531,16782532,16782533,
    16782534,16782535,16782536,16782537,16782538,16782539,16782540,16782541,16782542,16782543,16782544,16782545,
    16782546,16782547,16782548,16782549,16782550,16782551,16782552,16782553,16782554,16782555,16782556,16782557,
    16782558,16782559,16782560,16782561,16782562,16782563,16782564,16782565,16782566,16782567,16782568,16782569,
    16782570,16782571,16782572,16782573,16782574,16782575,16782576,16782577,16782578,16782579,16782580,16782581,
    16782582,16782583,16782584,16782585,16782586,16782587,16782588,16782589,16782590,16782591,16782592,16782593,
    16782594,16782595,16782596,16782597,16782598,16782599,16782600,16782601,16782602,16782603,16782604,16782605,
    16782606,16782607,16782608,16782609,16782610,16782611,16782612,16782613,16782614,16782615,16782616,16782617,
    16782618,16782619,16782620,16782621,16782622,16782623,16782624,16782625,16782626,16782627,16782628,16782629,
    16782630,16782631,16782632,16782633,16782634,16782635,16782636,16782637,16782638,16782639,16782640,16782641,
    16782642,16782643,16782644,16782645,16782646,16782647,16782648,16782649,16782650,16782651,16782652,16782653,
    16782654,16782655,16782656,16782657,16782658,16782659,16782660,16782661,16782662,16782663,16782664,16782665,
    16782666,16782667,16782668,16782669,16782670,16782671,16782672,16782673,16782674,16782675,16782676,16782677,
    16782678,16782679,16782680,16782681,16782682,16782683,16782684,16782685,16782686,16782687,16782688,16782689,
    16782690,16782691,16782692,16782693,16782694,16782695,16782696,16782697,16782698,16782699,16782700,16782701,
    16782702,16782703,16782704,16782705,16782706,16782707,16782708,16782709,16782710,16782711,16782712,16782713,
    16782714,16782715,16782716,16782717,16782718,16782719,16782720,16782721,16782722,16782723,16782724,16782725,
    16782726,16782727,16782728,16782729,16782730,16782731,16782732,16782733,16782734,16782735,16782736,16782737,
    16782738,16782739,16782740,16782741,16782742,16782743,16782744,16782745,16782746,16782747,16782748,16782749,
    16782750,16782751,16782752,16782753,16782754,16782755,16782756,16782757,16782758,16782759,16782760,16782761,
    16782762,16782763,16782764,16782765,16782766,16782767,16782768,16782769,16782770,16782771,16782772,16782773,
    16782774,16782775,16782776,16782777,16782778,16782779,16782780,16782781,16782782,16782783,16782784,16782785,
    16782786,16782787,16782788,16782789,16782790,16782791,16782792,16782793,16782794,16782795,16782796,16782797,
    16782798,16782799,16782800,16782801,16782802,16782803,16782804,16782805,16782806,16782807,16782808,16782809,
    16782810,16782811,16782812,16782813,16782814,16782815,16782816,16782817,16782818,16782819,16782820,16782821,
    16782822,16782823,16782824,16782825,16782826,16782827,16782828,16782829,16782830,16782831,16782832,16782833,
    16782834,16782835,16782836,16782837,16782838,16782839,16782840,16782841,16782842,16782843,16782844,16782845,
    16782846,16782847,16782848,16782849,16782850,16782851,16782852,16782853,16782854,16782855,16782856,16782857,
    16782858,16782859,16782860,16782861,16782862,16782863,16782864,16782865,16782866,16782867,16782868,16782869,
    16782870,16782871,16782872,16782873,16782874,16782875,16782876,16782877,16782878,16782879,16782880,16782881,
    16782882,16782883,16782884,16782885,16782886,16782887,16782888,16782889,16782890,16782891,16782892,16782893,
    16782894,16782895,16782896,16782897,16782898,16782899,16782900,16782901,16782902,16782903,16782904,16782905,
    16782906,16782907,16782908,16782909,16782910,16782911,16782912,16782913,16782914,16782915,16782916,16782917,
    16782918,16782919,16782920,16782921,16782922,16782923,16782924,16782925,16782926,16782927,16782928,16782929,
    16782930,16782931,16782932,16782933,16782934,16782935,16782936,16782937,16782938,16782939,16782940,16782941,
    16782942,16782943,16782944,16782945,16782946,16782947,16782948,16782949,16782950,16782951,16782952,16782953,
    16782954,16782955,16782956,16782957,16782958,16782959,16782960,16782961,16782962,16782963,16782964,16782965,
    16782966,16782967,16782968,16782969,16782970,16782971,16782972,16782973,16782974,16782975,16782976,16782977,
    16782978,16782979,16782980,16782981,16782982,16782983,16782984,16782985,16782986,16782987,16782988,16782989,
    16782990,16782991,16782992,16782993,16782994,16782995,16782996,16782997,16782998,16782999,16783000,16783001,
    16783002,16783003,16783004,16783005,16783006,16783007,16783008,16783009,16783010,16783011,16783012,16783013,
    16783014,16783015,16783016,16783017,16783018,16783019,16783020,16783021,16783022,16783023,16783024,16783025,
    16783026,16783027,16783028,16783029,16783030,16783031,16783032,16783033,16783034,16783035,16783036,16783037,
    16783038,16783039,16783040,16783041,16783042,16783043,16783044,16783045,16783046,16783047,16783048,16783049,
    16783050,16783051,16783052,16783053,16783054,16783055,16783056,16783057,16783058,16783059,16783060,16783061,
    16783062,16783063,16783064,16783065,16783066,16783067,16783068,16783069,16783070,16783071,16783072,16783073,
    16783074,16783075,16783076,16783077,16783078,16783079,16783080,16783081,16783082,16783083,16783084,16783085,
    16783086,16783087,16783088,16783089,16783090,16783091,16783092,16783093,16783094,16783095,16783096,16783097,
    16783098,16783099,16783100,16783101,16783102,16783103,16783104,16783105,16783106,16783107,16783108,16783109,
    16783110,16783111,16783112,16783113,16783114,16783115,16783116,16783117,16783118,16783119,16783120,16783121,
    16783122,16783123,16783124,16783125,16783126,16783127,16783128,16783129,16783130,16783131,16783132,16783133,
    16783134,16783135,16783136,16783137,16783138,16783139,16783140,16783141,16783142,16783143,16783144,16783145,
    16783146,16783147,16783148,16783149,16783150,16783151,16783152,16783153,16783154,16783155,16783156,16783157,
    16783158,16783159,16783160,16783161,16783162,16783163,16783164,16783165,16783166,16783167,16783168,16783169,
    16783170,16783171,16783172,16783173,16783174,16783175,16783176,16783177,16783178,16783179,16783180,16783181,
    16783182,16783183,16783184,16783185,16783186,16783187,16783188,16783189,16783190,16783191,16783192,16783193,
    16783194,16783195,16783196,16783197,16783198,16783199,16783200,16783201,16783202,16783203,16783204,16783205,
    16783206,16783207,16783208,16783209,16783210,16783211,16783212,16783213,16783214,16783215,16783216,16783217,
    16783218,16783219,16783220,16783221,16783222,16783223,16783224,16783225,16783226,16783227,16783228,16783229,
    16783230,16783231,16783232,16783233,16783234,16783235,16783236,16783237,16783238,16783239,16783240,16783241,
    16783242,16783243,16783244,16783245,16783246,16783247,16783248,16783249,16783250,16783251,16783252,16783253,
    16783254,16783255,16783256,16783257,16783258,16783259,16783260,16783261,16783262,16783263,16783264,16783265,
    16783266,16783267,16783268,16783269,16783270,16783271,16783272,16783273,16783274,16783275,16783276,16783277,
    16783278,16783279,16783280,16783281,16783282,16783283,16783284,16783285,16783286,16783287,16783288,16783289,
    16783290,16783291,16783292,16783293,16783294,16783295,16783296,16783297,16783298,16783299,16783300,16783301,
    16783302,16783303,16783304,16783305,16783306,16783307,16783308,16783309,16783310,16783311,16783312,16783313,
    16783314,16783315,16783316,16783317,16783318,16783319,16783320,16783321,16783322,16783323,16783324,16783325,
    16783326,16783327,16783328,16783329,16783330,16783331,16783332,16783333,16783334,16783335,16783336,16783337,
    16783338,16783339,16783340,16783341,16783342,16783343,16783344,16783345,16783346,16783347,16783348,16783349,
    16783350,16783351,16783352,16783353,16783354,16783355,16783356,16783357,16783358,16783359,16783360,16783361,
    16783362,16783363,16783364,16783365,16783366,16783367,16783368,16783369,16783370,16783371,16783372,16783373,
    16783374,16783375,16783376,16783377,16783378,16783379,16783380,16783381,16783382,16783383,16783384,16783385,
    16783386,16783387,16783388,16783389,16783390,16783391,16783392,16783393,16783394,16783395,16783396,16783397,
    16783398,16783399,16783400,16783401,16783402,16783403,16783404,16783405,16783406,16783407,16783408,16783409,
    16783410,16783411,16783412,16783413,16783414,16783415,16783416,16783417,16783418,16783419,16783420,16783421,
    16783422,16783423,16783424,16783425,16783426,16783427,16783428,16783429,16783430,16783431,16783432,16783433,
    16783434,16783435,16783436,16783437,16783438,16783439,16783440,16783441,16783442,16783443,16783444,16783445,
    16783446,16783447,16783448,16783449,16783450,16783451,16783452,16783453,16783454,16783455,16783456,16783457,
    16783458,16783459,16783460,16783461,16783462,16783463,16783464,16783465,16783466,16783467,16783468,16783469,
    16783470,16783471,16783472,16783473,16783474,16783475,16783476,16783477,16783478,16783479,16783480,16783481,
    16783482,16783483,16783484,16783485,16783486,16783487,16783488,16783489,16783490,16783491,16783492,16783493,
    16783494,16783495,16783496,16783497,16783498,16783499,16783500,16783501,16783502,16783503,16783504,16783505,
    16783506,16783507,16783508,16783509,16783510,16783511,16783512,16783513,16783514,16783515,16783516,16783517,
    16783518,16783519,16783520,16783521,16783522,16783523,16783524,16783525,16783526,16783527,16783528,16783529,
    16783530,16783531,16783532,16783533,16783534,16783535,16783536,16783537,16783538,16783539,16783540,16783541,
    16783542,16783543,16783544,16783545,16783546,16783547,16783548,16783549,16783550,16783551,16783552,16783553,
    16783554,16783555,16783556,16783557,16783558,16783559,16783560,16783561,16783562,16783563,16783564,16783565,
    16783566,16783567,16783568,16783569,16783570,16783571,16783572,16783573,16783574,16783575,16783576,16783577,
    16783578,16783579,16783580,16783581,16783582,16783583,16783584,16783585,16783586,16783587,16783588,16783589,
    16783590,16783591,16783592,16783593,16783594,16783595,16783596,16783597,16783598,16783599,16783600,16783601,
    16783602,16783603,16783604,16783605,16783606,16783607,16783608,16783609,16783610,16783611,16783612,16783613,
    16783614,16783615,16783616,16783617,16783618,16783619,16783620,16783621,16783622,16783623,16783624,16783625,
    16783626,16783627,16783628,16783629,16783630,16783631,16783632,16783633,16783634,16783635,16783636,16783637,
    16783638,16783639,16783640,16783641,16783642,16783643,16783644,16783645,16783646,16783647,16783648,16783649,
    16783650,16783651,16783652,16783653,16783654,16783655,16783656,16783657,16783658,16783659,16783660,16783661,
    16783662,16783663,16783664,16783665,16783666,16783667,16783668,16783669,16783670,16783671,16783672,16783673,
    16783674,16783675,16783676,16783677,16783678,16783679,16783680,16783681,16783682,16783683,16783684,16783685,
    16783686,16783687,16783688,16783689,16783690,16783691,16783692,16783693,16783694,16783695,16783696,16783697,
    16783698,16783699,16783700,16783701,16783702,16783703,16783704,16783705,16783706,16783707,16783708,16783709,
    16783710,16783711,16783712,16783713,16783714,16783715,16783716,16783717,16783718,16783719,16783720,16783721,
    16783722,16783723,16783724,16783725,16783726,16783727,16783728,16783729,16783730,16783731,16783732,16783733,
    16783734,16783735,16783736,16783737,16783738,16783739,16783740,16783741,16783742,16783743,16783744,16783745,
    16783746,16783747,16783748,16783749,16783750,16783751,16783752,16783753,16783754,16783755,16783756,16783757,
    16783758,16783759,16783760,16783761,16783762,16783763,16783764,16783765,16783766,16783767,16783768,16783769,
    16783770,16783771,16783772,16783773,16783774,16783775,16783776,16783777,16783778,16783779,16783780,16783781,
    16783782,16783783,16783784,16783785,16783786,16783787,16783788,16783789,16783790,16783791,16783792,16783793,
    16783794,16783795,16783796,16783797,16783798,16783799,16783800,16783801,16783802,16783803,16783804,16783805,
    16783806,16783807,16783808,16783809,16783810,16783811,16783812,16783813,16783814,16783815,16783816,16783817,
    16783818,16783819,16783820,16783821,16783822,16783823,16783824,16783825,16783826,16783827,16783828,16783829,
    16783830,16783831,16783832,16783833,16783834,16783835,16783836,16783837,16783838,16783839,16783840,16783841,
    16783842,16783843,16783844,16783845,16783846,16783847,16783848,16783849,16783850,16783851,16783852,16783853,
    16783854,16783855,16783856,16783857,67115506,16783862,16783863,16783864,16783865,16783866,16783867,16783868,
    16783869,16783870,16783871,16783872,16783873,16783874,16783875,16783876,16783877,16783878,16783879,16783880,
    16783881,16783882,16783883,16783884,16783885,16783886,16783887,16783888,16783889,16783890,16783891,16783892,
    16783893,16783894,16783895,16783896,16783897,16783898,16783899,16783900,16783901,16783902,16783903,16783904,
    16783905,16783906,16783907,16783908,16783909,16783910,16783911,16783912,16783913,16783914,16783915,16783916,
    16783917,16783918,16783919,16783920,16783921,16783922,16783923,16783924,16783925,16783926,16783927,16783928,
    16783929,16783930,16783931,16783932,16783933,16783934,16783935,16783936,16783937,16783938,16783939,16783940,
    16783941,16783942,16783943,16783944,16783945,16783946,16783947,16783948,16783949,16783950,16783951,16783952,
    16783953,16783954,16783955,16783956,16783957,16783958,16783959,16783960,16783961,16783962,16783963,16783964,
    50338397,33561184,50338402,16783973,16783974,16783975,16783976,16783977,16783978,16783979,16783980,16783981,
    16783982,16783983,16783984,16783985,16783986,16783987,16783988,16783989,16783990,16783991,16783992,16783993,
    16783994,16783995,16783996,16783997,16783998,16783999,16784000,16784001,16784002,16784003,16784004,16784005,
    16784006,16784007,16784008,16784009,16784010,16784011,16784012,16784013,16784014,16784015,16784016,16784017,
    16784018,16784019,16784020,16784021,16784022,16784023,16784024,16784025,16784026,16784027,16784028,16784029,
    16784030,16784031,16784032,16784033,16784034,16784035,16784036,16784037,16784038,16784039,16784040,16784041,
    16784042,16784043,16784044,16784045,16784046,16784047,16784048,16784049,16784050,16784051,16784052,16784053,
    16784054,16784055,16784056,16784057,16784058,16784059,16784060,16784061,16784062,16784063,16784064,16784065,
    16784066,16784067,16784068,16784069,16784070,16784071,16784072,16784073,33561290,16784076,16784077,16784078,
    16784079,16784080,16784081,16784082,16784083,16784084,16784085,16784086,16784087,16784088,16784089,16784090,
    16784091,16784092,16784093,16784094,16784095,16784096,16784097,16784098,16784099,16784100,16784101,16784102,
    16784103,16784104,16784105,16784106,16784107,16784108,16784109,16784110,16784111,16784112,16784113,16784114,
    16784115,16784116,16784117,16784118,16784119,16784120,16784121,16784122,16784123,16784124,16784125,16784126,
    16784127,16784128,16784129,16784130,16784131,16784132,16784133,16784134,16784135,16784136,16784137,16784138,
    16784139,16784140,16784141,16784142,16784143,16784144,16784145,16784146,16784147,16784148,16784149,16784150,
    16784151,16784152,16784153,16784154,16784155,16784156,16784157,16784158,16784159,16784160,16784161,16784162,
    16784163,16784164,16784165,16784166,16784167,16784168,16784169,16784170,16784171,16784172,16784173,16784174,
    16784175,16784176,16784177,16784178,16784179,16784180,16784181,16784182,16784183,16784184,16784185,16784186,
    16784187,16784188,16784189,16784190,16784191,16784192,16784193,16784194,16784195,16784196,16784197,16784198,
    16784199,16784200,16784201,16784202,16784203,16784204,16784205,16784206,16784207,16784208,16784209,16784210,
    16784211,16784212,16784213,16784214,16784215,16784216,16784217,16784218,16784219,16784220,16784221,16784222,
    16784223,16784224,16784225,16784226,16784227,16784228,16784229,16784230,16784231,16784232,16784233,16784234,
    16784235,16784236,16784237,16784238,16784239,16784240,16784241,16784242,16784243,16784244,16784245,16784246,
    16784247,16784248,16784249,16784250,16784251,16784252,16784253,16784254,16784255,16784256,16784257,16784258,
    16784259,16784260,16784261,16784262,16784263,16784264,16784265,16784266,16784267,16784268,16784269,16784270,
    16784271,16784272,16784273,16784274,16784275,16784276,16784277,16784278,16784279,16784280,16784281,16784282,
    16784283,16784284,16784285,16784286,16784287,16784288,16784289,16784290,16784291,16784292,16784293,16784294,
    16784295,16784296,16784297,16784298,16784299,16784300,16784301,16784302,16784303,16784304,16784305,16784306,
    16784307,16784308,16784309,16784310,16784311,16784312,16784313,16784314,16784315,16784316,16784317,16784318,
    16784319,16784320,16784321,16784322,16784323,16784324,16784325,16784326,16784327,16784328,16784329,16784330,
    16784331,16784332,16784333,16784334,16784335,16784336,16784337,16784338,16784339,16784340,16784341,16784342,
    16784343,16784344,16784345,16784346,16784347,16784348,16784349,16784350,16784351,16784352,16784353,16784354,
    16784355,16784356,16784357,16784358,16784359,16784360,16784361,16784362,16784363,16784364,16784365,16784366,
    16784367,16784368,16784369,16784370,16784371,16784372,16784373,16784374,16784375,16784376,16784377,16784378,
    16784379,16784380,16784381,16784382,16784383,16784384,16784385,16784386,16784387,16784388,16784389,16784390,
    16784391,16784392,16784393,16784394,16784395,33561612,33561614,33561616,16784402,16784403,16784404,16784405,
    16784406,16784407,16784408,16784409,16784410,16784411,33561628,33561630,33561632,33561634,33561636,33561638,
    16784424,16784425,33561642,16784428,33561645,16784431,16784432,16784433,16784434,16784435,16784436,16784437,
    16784438,16784439,16784440,16784441,16784442,16784443,16784444,16784445,16784446,16784447,16784448,16784449,
    16784450,16784451,16784452,16784453,50338886,16784457,50338890,16784461,50338894,16784465,50338898,16784469,
    50338902,16784473,50338906,16784477,50338910,16784481,50338914,16784485,50338918,16784489,50338922,16784493,
    50338926,16784497,50338930,16784501,16784502,50338935,16784506,50338939,16784510,50338943,16784514,16784515,
    16784516,16784517,16784518,16784519,50338952,50338955,16784526,50338959,50338962,16784533,50338966,50338969,
    16784540,50338973,50338976,16784547,50338980,50338983,16784554,16784555,16784556,16784557,16784558,16784559,
    16784560,16784561,16784562,16784563,16784564,16784565,16784566,16784567,16784568,16784569,16784570,16784571,
    16784572,16784573,16784574,16784575,50339008,16784579,16784580,16784581,16784582,33561799,33561801,16784587,
    16784588,16784589,50339022,33561809,16784595,16784596,16784597,16784598,16784599,16784600,16784601,16784602,
    16784603,16784604,16784605,16784606,50339039,16784610,50339043,16784614,50339047,16784618,50339051,16784622,
    50339055,16784626,50339059,16784630,50339063,16784634,50339067,16784638,50339071,16784642,50339075,16784646,
    50339079,16784650,50339083,16784654,16784655,50339088,16784659,50339092,16784663,50339096,16784667,16784668,
    16784669,16784670,16784671,16784672,50339105,50339108,16784679,50339112,50339115,16784686,50339119,50339122,
    16784693,50339126,50339129,16784700,50339133,50339136,16784707,16784708,16784709,16784710,16784711,16784712,
    16784713,16784714,16784715,16784716,16784717,16784718,16784719,16784720,16784721,16784722,16784723,16784724,
    16784725,16784726,16784727,16784728,50339161,16784732,16784733,50339166,50339169,50339172,50339175,16784746,
    16784747,16784748,50339181,33561968,16784754,16784755,16784756,16784757,16784758,16784759,16784760,16784761,
    16784762,16784763,16784764,16784765,16784766,16784767,16784768,16784769,16784770,16784771,16784772,16784773,
    16784774,16784775,16784776,16784777,16784778,16784779,16784780,16784781,16784782,16784783,16784784,16784785,
    16784786,16784787,16784788,16784789,16784790,16784791,16784792,16784793,16784794,16784795,16784796,16784797,
    16784798,16784799,16784800,16784801,16784802,16784803,16784804,16784805,16784806,16784807,16784808,16784809,
    16784810,16784811,16784812,16784813,16784814,16784815,16784816,16784817,16784818,16784819,16784820,16784821,
    16784822,16784823,16784824,16784825,16784826,16784827,16784828,16784829,16784830,16784831,16784832,16784833,
    16784834,16784835,16784836,16784837,16784838,16784839,16784840,16784841,16784842,16784843,16784844,16784845,
    16784846,16784847,16784848,16784849,16784850,16784851,16784852,16784853,16784854,16784855,16784856,16784857,
    16784858,16784859,16784860,16784861,16784862,16784863,16784864,16784865,16784866,16784867,16784868,16784869,
    16784870,16784871,16784872,16784873,16784874,16784875,16784876,16784877,16784878,16784879,16784880,16784881,
    16784882,16784883,16784884,16784885,16784886,16784887,16784888,16784889,16784890,16784891,16784892,16784893,
    16784894,16784895,16784896,16784897,16784898,16784899,16784900,16784901,16784902,16784903,16784904,16784905,
    16784906,16784907,16784908,16784909,16784910,16784911,16784912,16784913,16784914,16784915,16784916,16784917,
    16784918,16784919,16784920,16784921,16784922,16784923,16784924,16784925,16784926,16784927,16784928,16784929,
    16784930,16784931,16784932,16784933,16784934,16784935,16784936,16784937,16784938,16784939,16784940,16784941,
    16784942,16784943,16784944,16784945,16784946,16784947,16784948,16784949,16784950,16784951,16784952,16784953,
    16784954,16784955,16784956,16784957,16784958,16784959,16784960,16784961,16784962,16784963,16784964,16784965,
    16784966,16784967,16784968,16784969,16784970,16784971,16784972,16784973,16784974,16784975,16784976,16784977,
    16784978,16784979,16784980,16784981,16784982,16784983,16784984,16784985,16784986,16784987,16784988,16784989,
    16784990,16784991,16784992,16784993,16784994,16784995,16784996,16784997,16784998,16784999,16785000,16785001,
    16785002,16785003,16785004,16785005,16785006,16785007,16785008,16785009,33562226,33562228,33562230,33562232,
    33562234,33562236,33562238,33562240,33562242,33562244,33562246,16785032,16785033,16785034,16785035,16785036,
    50339469,50339472,50339475,50339478,50339481,50339484,50339487,50339490,50339493,50339496,50339499,50339502,
    50339505,50339508,50339511,50339514,50339517,50339520,50339523,50339526,50339529,50339532,50339535,50339538,
    50339541,50339544,50339547,16785118,16785119,33562336,33562338,16785124,16785125,16785126,16785127,16785128,
    16785129,16785130,16785131,16785132,16785133,16785134,16785135,16785136,16785137,16785138,16785139,16785140,
    16785141,16785142,16785143,16785144,16785145,16785146,16785147,16785148,16785149,16785150,33562367,33562369,
    33562371,33562373,50339591,33562378,16785164,16785165,16785166,16785167,16785168,16785169,16785170,16785171,
    16785172,16785173,16785174,16785175,16785176,16785177,16785178,16785179,16785180,16785181,16785182,16785183,
    16785184,16785185,16785186,16785187,16785188,16785189,33562406,33562408,33562410,16785196,16785197,16785198,
    16785199,16785200,16785201,16785202,16785203,16785204,16785205,16785206,16785207,16785208,16785209,16785210,
    16785211,16785212,16785213,16785214,16785215,16785216,16785217,16785218,16785219,16785220,16785221,16785222,
    16785223,16785224,16785225,33562442,33562444,33562446,33562448,33562450,33562452,33562454,16785240,16785241,
    33562458,16785244,33562461,33562463,16785249,16785250,33562467,16785253,16785254,16785255,16785256,16785257,
    16785258,16785259,16785260,16785261,16785262,16785263,33562480,16785266,16785267,16785268,16785269,16785270,
    16785271,16785272,16785273,16785274,16785275,16785276,16785277,16785278,16785279,16785280,16785281,16785282,
    16785283,16785284,16785285,16785286,16785287,16785288,16785289,16785290,16785291,16785292,16785293,16785294,
    16785295,16785296,16785297,16785298,16785299,16785300,16785301,16785302,16785303,16785304,16785305,16785306,
    16785307,16785308,16785309,16785310,16785311,16785312,16785313,16785314,16785315,16785316,16785317,16785318,
    16785319,16785320,16785321,16785322,16785323,16785324,16785325,16785326,16785327,16785328,16785329,16785330,
    16785331,16785332,16785333,16785334,16785335,16785336,16785337,16785338,16785339,16785340,16785341,16785342,
    16785343,16785344,16785345,16785346,16785347,16785348,16785349,16785350,16785351,16785352,16785353,16785354,
    33562571,33562573,16785359,16785360,16785361,16785362,16785363,16785364,16785365,16785366,16785367,16785368,
    16785369,16785370,16785371,16785372,16785373,16785374,16785375,50339808,16785379,16785380,16785381,16785382,
    16785383,16785384,16785385,16785386,16785387,16785388,16785389,16785390,16785391,16785392,16785393,16785394,
    16785395,16785396,16785397,16785398,16785399,16785400,16785401,16785402,16785403,16785404,16785405,16785406,
    16785407,16785408,16785409,16785410,16785411,16785412,16785413,16785414,16785415,16785416,16785417,16785418,
    16785419,16785420,16785421,16785422,~0u,~0u,~0u,~0u,~0u,~0u,~0u,~0u,
    ~0u,16785423,16785424,16785425,16785426,16785427,16785428,16785429,16785430,16785431,16785432,16785433,
    16785434,16785435,16785436,16785437,16785438,16785439,16785440,16785441,16785442,16785443,16785444,16785445,
    16785446,16785447,16785448,16785449,16785450,16785451,16785452,16785453,16785454,16785455,16785456,16785457,
    16785458,16785459,16785460,16785461,16785462,16785463,16785464,16785465,16785466,16785467,16785468,16785469,
    16785470,16785471,16785472,16785473,16785474,16785475,16785476,16785477,16785478,16785479,16785480,16785481,
    16785482,16785483,16785484,16785485,16785486,16785487,16785488,16785489,16785490,16785491,16785492,16785493,
    16785494,16785495,16785496,16785497,16785498,16785499,16785500,16785501,16785502,16785503,16785504,16785505,
    16785506,16785507,16785508,16785509,16785510,16785511,16785512,16785513,16785514,16785515,16785516,16785517,
    16785518,16785519,16785520,16785521,16785522,16785523,16785524,16785525,16785526,16785527,16785528,16785529,
    16785530,16785531,16785532,16785533,16785534,16785535,16785536,16785537,16785538,16785539,16785540,16785541,
    16785542,16785543,16785544,16785545,16785546,16785547,16785548,16785549,16785550,16785551,16785552,16785553,
    16785554,16785555,16785556,16785557,16785558,16785559,16785560,16785561,16785562,16785563,16785564,16785565,
    16785566,16785567,16785568,16785569,16785570,16785571,16785572,16785573,16785574,16785575,16785576,16785577,
    16785578,16785579,16785580,16785581,16785582,16785583,16785584,16785585,16785586,16785587,16785588,16785589,
    16785590,16785591,16785592,16785593,16785594,16785595,16785596,16785597,16785598,16785599,16785600,16785601,
    16785602,16785603,16785604,16785605,16785606,16785607,16785608,16785609,16785610,16785611,16785612,16785613,
    16785614,16785615,16785616,16785617,16785618,16785619,16785620,16785621,16785622,16785623,16785624,16785625,
    16785626,16785627,16785628,16785629,16785630,16785631,16785632,16785633,16785634,16785635,16785636,16785637,
    16785638,16785639,16785640,16785641,16785642,16785643,16785644,16785645,16785646,16785647,16785648,16785649,
    16785650,16785651,16785652,16785653,16785654,16785655,16785656,16785657,16785658,16785659,16785660,16785661,
    16785662,16785663,16785664,16785665,16785666,16785667,16785668,16785669,16785670,16785671,16785672,16785673,
    16785674,16785675,16785676,16785677,16785678,16785679,16785680,16785681,16785682,16785683,16785684,16785685,
    16785686,16785687,16785688,16785689,16785690,16785691,16785692,16785693,16785694,16785695,16785696,16785697,
    16785698,16785699,16785700,16785701,16785702,16785703,16785704,16785705,16785706,16785707,16785708,16785709,
    16785710,16785711,16785712,16785713,16785714,16785715,16785716,16785717,16785718,16785719,16785720,16785721,
    16785722,16785723,16785724,16785725,16785726,16785727,16785728,16785729,16785730,16785731,16785732,16785733,
    16785734,16785735,16785736,16785737,16785738,16785739,16785740,16785741,16785742,16785743,16785744,16785745,
    16785746,16785747,16785748,16785749,16785750,16785751,16785752,16785753,16785754,16785755,16785756,16785757,
    16785758,16785759,16785760,16785761,16785762,16785763,16785764,16785765,16785766,16785767,16785768,16785769,
    16785770,16785771,16785772,16785773,16785774,16785775,16785776,16785777,16785778,16785779,16785780,16785781,
    16785782,16785783,16785784,16785785,16785786,16785787,16785788,16785789,16785790,16785791,16785792,16785793,
    16785794,16785795,16785796,16785797,16785798,16785799,16785800,16785801,16785802,16785803,16785804,16785805,
    16785806,16785807,16785808,16785809,16785810,16785811,16785812,16785813,16785814,16785815,16785816,16785817,
    16785818,16785819,16785820,16785821,16785822,16785823,16785824,16785825,16785826,16785827,16785828,16785829,
    16785830,16785831,16785832,16785833,16785834,16785835,16785836,16785837,16785838,16785839,16785840,16785841,
    16785842,16785843,16785844,16785845,16785846,16785847,16785848,16785849,16785850,16785851,16785852,16785853,
    16785854,16785855,16785856,16785857,16785858,16785859,16785860,16785861,16785862,16785863,16785864,16785865,
    16785866,16785867,16785868,16785869,16785870,16785871,16785872,16785873,16785874,16785875,16785876,16785877,
    16785878,16785879,16785880,16785881,16785882,16785883,16785884,16785885,16785886,16785887,16785888,16785889,
    16785890,16785891,16785892,16785893,16785894,16785895,16785896,16785897,16785898,16785899,16785900,16785901,
    16785902,16785903,16785904,16785905,16785906,16785907,16785908,16785909,16785910,16785911,16785912,16785913,
    16785914,16785915,16785916,16785917,16785918,16785919,16785920,16785921,16785922,16785923,16785924,16785925,
    16785926,16785927,16785928,16785929,16785930,16785931,16785932,16785933,16785934,16785935,16785936,16785937,
    16785938,16785939,16785940,16785941,16785942,16785943,16785944,16785945,16785946,16785947,16785948,16785949,
    16785950,16785951,16785952,16785953,16785954,16785955,16785956,16785957,16785958,16785959,16785960,16785961,
    16785962,16785963,16785964,16785965,16785966,16785967,16785968,16785969,16785970,16785971,16785972,16785973,
    16785974,16785975,16785976,16785977,16785978,16785979,16785980,16785981,16785982,16785983,16785984,16785985,
    16785986,16785987,16785988,16785989,16785990,16785991,16785992,16785993,16785994,16785995,16785996,16785997,
    16785998,16785999,16786000,16786001,16786002,16786003,16786004,16786005,16786006,16786007,16786008,16786009,
    16786010,16786011,16786012,16786013,16786014,16786015,16786016,16786017,16786018,16786019,16786020,16786021,
    16786022,16786023,16786024,16786025,16786026,16786027,16786028,16786029,16786030,16786031,16786032,16786033,
    16786034,16786035,16786036,16786037,16786038,16786039,16786040,16786041,16786042,16786043,16786044,16786045,
    16786046,16786047,16786048,16786049,16786050,16786051,16786052,16786053,16786054,16786055,16786056,16786057,
    16786058,16786059,16786060,16786061,16786062,16786063,16786064,16786065,16786066,16786067,16786068,16786069,
    16786070,16786071,16786072,16786073,16786074,16786075,16786076,16786077,16786078,16786079,16786080,16786081,
    16786082,16786083,16786084,16786085,16786086,16786087,16786088,16786089,16786090,16786091,16786092,16786093,
    16786094,16786095,16786096,16786097,16786098,16786099,16786100,16786101,16786102,16786103,16786104,16786105,
    16786106,16786107,16786108,16786109,16786110,16786111,16786112,16786113,16786114,16786115,16786116,16786117,
    16786118,16786119,16786120,16786121,16786122,16786123,16786124,16786125,16786126,16786127,16786128,16786129,
    16786130,16786131,16786132,16786133,16786134,16786135,16786136,16786137,16786138,16786139,16786140,16786141,
    16786142,16786143,16786144,16786145,16786146,16786147,16786148,16786149,16786150,16786151,16786152,16786153,
    16786154,16786155,16786156,16786157,16786158,16786159,16786160,16786161,16786162,16786163,16786164,16786165,
    16786166,16786167,16786168,16786169,16786170,16786171,16786172,16786173,16786174,16786175,16786176,16786177,
    16786178,16786179,16786180,16786181,16786182,16786183,16786184,16786185,16786186,16786187,16786188,16786189,
    16786190,16786191,16786192,16786193,16786194,16786195,16786196,16786197,16786198,16786199,16786200,16786201,
    16786202,16786203,16786204,16786205,16786206,16786207,16786208,16786209,16786210,16786211,16786212,16786213,
    16786214,16786215,16786216,16786217,16786218,16786219,16786220,16786221,16786222,16786223,16786224,16786225,
    16786226,16786227,16786228,16786229,16786230,16786231,16786232,16786233,16786234,16786235,16786236,16786237,
    16786238,16786239,16786240,16786241,16786242,16786243,16786244,16786245,16786246,16786247,16786248,16786249,
    16786250,16786251,16786252,16786253,16786254,16786255,16786256,16786257,16786258,16786259,16786260,16786261,
    16786262,16786263,16786264,16786265,16786266,16786267,16786268,16786269,16786270,16786271,16786272,16786273,
    16786274,16786275,16786276,16786277,16786278,16786279,16786280,16786281,16786282,16786283,16786284,16786285,
    16786286,16786287,16786288,16786289,16786290,16786291,16786292,16786293,16786294,16786295,16786296,16786297,
    16786298,16786299,16786300,16786301,16786302,16786303,16786304,16786305,16786306,16786307,16786308,16786309,
    16786310,16786311,16786312,16786313,16786314,16786315,16786316,16786317,16786318,16786319,16786320,16786321,
    16786322,16786323,16786324,16786325,16786326,16786327,16786328,16786329,16786330,16786331,16786332,16786333,
    16786334,16786335,16786336,16786337,16786338,16786339,16786340,16786341,16786342,16786343,16786344,16786345,
    16786346,16786347,16786348,16786349,16786350,16786351,16786352,16786353,16786354,16786355,16786356,16786357,
    16786358,16786359,16786360,16786361,16786362,16786363,16786364,16786365,16786366,16786367,16786368,16786369,
    16786370,16786371,16786372,16786373,16786374,16786375,16786376,16786377,16786378,16786379,16786380,16786381,
    16786382,16786383,16786384,16786385,16786386,16786387,16786388,16786389,16786390,16786391,16786392,16786393,
    16786394,16786395,16786396,16786397,16786398,16786399,16786400,16786401,16786402,16786403,16786404,16786405,
    16786406,16786407,16786408,16786409,16786410,16786411,16786412,16786413,16786414,16786415,16786416,16786417,
    16786418,16786419,16786420,16786421,16786422,16786423,16786424,16786425,16786426,16786427,16786428,16786429,
    16786430,16786431,16786432,16786433,16786434,16786435,16786436,16786437,16786438,16786439,16786440,16786441,
    16786442,16786443,16786444,16786445,16786446,16786447,16786448,16786449,16786450,16786451,16786452,16786453,
    16786454,16786455,16786456,16786457,16786458,16786459,16786460,16786461,16786462,16786463,16786464,16786465,
    16786466,16786467,16786468,16786469,16786470,16786471,16786472,16786473,16786474,16786475,16786476,16786477,
    16786478,16786479,16786480,16786481,16786482,16786483,16786484,16786485,16786486,16786487,16786488,16786489,
    16786490,16786491,16786492,16786493,16786494,16786495,16786496,16786497,16786498,16786499,16786500,16786501,
    16786502,16786503,16786504,16786505,16786506,16786507,16786508,16786509,16786510,16786511,16786512,16786513,
    16786514,16786515,16786516,16786517,16786518,16786519,16786520,16786521,16786522,16786523,16786524,16786525,
    16786526,16786527,16786528,16786529,16786530,16786531,16786532,16786533,16786534,16786535,16786536,16786537,
    16786538,16786539,16786540,16786541,16786542,16786543,16786544,16786545,16786546,16786547,16786548,16786549,
    16786550,16786551,16786552,16786553,16786554,16786555,16786556,16786557,16786558,16786559,16786560,16786561,
    16786562,16786563,16786564,16786565,16786566,16786567,16786568,16786569,16786570,16786571,16786572,16786573,
    16786574,16786575,16786576,16786577,16786578,16786579,16786580,16786581,16786582,16786583,16786584,16786585,
    16786586,16786587,16786588,16786589,16786590,16786591,16786592,16786593,16786594,16786595,16786596,16786597,
    16786598,16786599,16786600,16786601,16786602,16786603,16786604,16786605,16786606,16786607,16786608,16786609,
    16786610,16786611,16786612,16786613,16786614,16786615,16786616,16786617,16786618,16786619,16786620,16786621,
    16786622,16786623,16786624,16786625,16786626,16786627,16786628,16786629,16786630,16786631,16786632,16786633,
    16786634,16786635,16786636,16786637,16786638,16786639,16786640,16786641,16786642,16786643,16786644,16786645,
    16786646,16786647,16786648,16786649,16786650,16786651,16786652,16786653,16786654,16786655,16786656,16786657,
    16786658,16786659,16786660,16786661,16786662,16786663,16786664,16786665,16786666,16786667,16786668,16786669,
    16786670,16786671,16786672,16786673,16786674,16786675,16786676,16786677,16786678,16786679,16786680,16786681,
    16786682,16786683,16786684,16786685,16786686,16786687,16786688,16786689,16786690,16786691,16786692,16786693,
    16786694,16786695,16786696,16786697,16786698,16786699,16786700,16786701,16786702,16786703,16786704,16786705,
    16786706,16786707,16786708,16786709,16786710,16786711,16786712,16786713,16786714,16786715,16786716,16786717,
    16786718,16786719,16786720,16786721,16786722,16786723,16786724,16786725,16786726,16786727,16786728,16786729,
    16786730,16786731,16786732,16786733,16786734,16786735,16786736,16786737,16786738,16786739,16786740,16786741,
    16786742,16786743,16786744,16786745,16786746,16786747,16786748,16786749,16786750,16786751,16786752,16786753,
    16786754,16786755,16786756,16786757,16786758,16786759,16786760,16786761,16786762,16786763,16786764,16786765,
    16786766,16786767,16786768,16786769,16786770,16786771,16786772,16786773,16786774,16786775,16786776,16786777,
    16786778,16786779,16786780,16786781,16786782,16786783,16786784,16786785,16786786,16786787,16786788,16786789,
    16786790,16786791,16786792,16786793,16786794,16786795,16786796,16786797,16786798,16786799,16786800,16786801,
    16786802,16786803,16786804,16786805,16786806,16786807,16786808,16786809,16786810,16786811,16786812,16786813,
    16786814,16786815,16786816,16786817,16786818,16786819,16786820,16786821,16786822,16786823,16786824,16786825,
    16786826,16786827,16786828,16786829,16786830,16786831,16786832,16786833,16786834,16786835,16786836,16786837,
    16786838,16786839,16786840,16786841,16786842,16786843,16786844,16786845,16786846,16786847,16786848,16786849,
    16786850,16786851,16786852,16786853,16786854,16786855,16786856,16786857,16786858,16786859,16786860,16786861,
    16786862,16786863,16786864,16786865,16786866,16786867,16786868,16786869,16786870,16786871,16786872,16786873,
    16786874,16786875,16786876,16786877,16786878,16786879,16786880,16786881,16786882,16786883,16786884,16786885,
    16786886,16786887,16786888,16786889,16786890,16786891,16786892,16786893,16786894,16786895,16786896,16786897,
    16786898,16786899,16786900,16786901,16786902,16786903,16786904,16786905,16786906,16786907,16786908,16786909,
    16786910,16786911,16786912,16786913,16786914,16786915,16786916,16786917,16786918,16786919,16786920,16786921,
    16786922,16786923,16786924,16786925,16786926,16786927,16786928,16786929,16786930,16786931,16786932,16786933,
    16786934,16786935,16786936,16786937,16786938,16786939,16786940,16786941,16786942,16786943,16786944,16786945,
    16786946,16786947,16786948,16786949,16786950,16786951,16786952,16786953,16786954,16786955,16786956,16786957,
    16786958,16786959,16786960,16786961,16786962,16786963,16786964,16786965,16786966,16786967,16786968,16786969,
    16786970,16786971,16786972,16786973,16786974,16786975,16786976,16786977,16786978,16786979,16786980,16786981,
    16786982,16786983,16786984,16786985,16786986,16786987,16786988,16786989,16786990,16786991,16786992,16786993,
    16786994,16786995,16786996,16786997,16786998,16786999,16787000,16787001,16787002,16787003,16787004,16787005,
    16787006,16787007,16787008,16787009,16787010,16787011,16787012,16787013,16787014,16787015,16787016,16787017,
    16787018,16787019,16787020,16787021,16787022,16787023,16787024,16787025,16787026,16787027,16787028,16787029,
    16787030,16787031,16787032,16787033,16787034,16787035,16787036,16787037,16787038,16787039,16787040,16787041,
    16787042,16787043,16787044,16787045,16787046,16787047,16787048,16787049,16787050,16787051,16787052,16787053,
    16787054,16787055,16787056,16787057,16787058,16787059,16787060,16787061,16787062,16787063,16787064,16787065,
    16787066,16787067,16787068,16787069,16787070,16787071,16787072,16787073,16787074,16787075,16787076,16787077,
    16787078,16787079,16787080,16787081,16787082,16787083,16787084,16787085,16787086,16787087,16787088,16787089,
    16787090,16787091,16787092,16787093,16787094,16787095,16787096,16787097,16787098,16787099,16787100,16787101,
    16787102,16787103,16787104,16787105,16787106,16787107,16787108,16787109,16787110,16787111,16787112,16787113,
    16787114,16787115,16787116,16787117,16787118,16787119,16787120,16787121,16787122,16787123,16787124,16787125,
    16787126,16787127,16787128,16787129,16787130,16787131,16787132,16787133,16787134,16787135,16787136,16787137,
    16787138,16787139,16787140,16787141,16787142,16787143,16787144,16787145,16787146,16787147,16787148,16787149,
    16787150,16787151,16787152,16787153,16787154,16787155,16787156,16787157,16787158,16787159,16787160,16787161,
    16787162,16787163,16787164,16787165,16787166,16787167,16787168,16787169,16787170,16787171,16787172,16787173,
    16787174,16787175,16787176,16787177,16787178,16787179,16787180,16787181,16787182,16787183,16787184,16787185,
    16787186,16787187,16787188,16787189,16787190,16787191,16787192,16787193,16787194,16787195,16787196,16787197,
    16787198,16787199,16787200,16787201,16787202,16787203,16787204,16787205,16787206,16787207,16787208,16787209,
    16787210,16787211,16787212,16787213,16787214,16787215,16787216,16787217,16787218,16787219,16787220,16787221,
    16787222,16787223,16787224,16787225,16787226,16787227,16787228,16787229,16787230,16787231,16787232,16787233,
    16787234,16787235,16787236,16787237,16787238,16787239,16787240,16787241,16787242,16787243,16787244,16787245,
    16787246,16787247,16787248,16787249,16787250,16787251,16787252,16787253,16787254,16787255,16787256,16787257,
    16787258,16787259,16787260,16787261,16787262,16787263,16787264,16787265,16787266,16787267,16787268,16787269,
    16787270,16787271,16787272,16787273,16787274,16787275,16787276,16787277,16787278,16787279,16787280,16787281,
    16787282,16787283,16787284,16787285,16787286,16787287,16787288,16787289,16787290,16787291,16787292,16787293,
    16787294,16787295,16787296,16787297,16787298,16787299,16787300,16787301,16787302,16787303,16787304,16787305,
    16787306,16787307,16787308,16787309,16787310,16787311,16787312,16787313,16787314,16787315,16787316,16787317,
    16787318,16787319,16787320,16787321,16787322,16787323,16787324,16787325,16787326,16787327,16787328,16787329,
    16787330,16787331,16787332,16787333,16787334,16787335,16787336,16787337,16787338,16787339,16787340,16787341,
    16787342,16787343,16787344,16787345,16787346,16787347,16787348,16787349,16787350,16787351,16787352,16787353,
    16787354,16787355,16787356,16787357,16787358,16787359,16787360,16787361,16787362,16787363,16787364,16787365,
    16787366,16787367,16787368,16787369,16787370,16787371,16787372,16787373,16787374,16787375,16787376,16787377,
    16787378,16787379,16787380,16787381,16787382,16787383,16787384,16787385,16787386,16787387,16787388,16787389,
    16787390,16787391,16787392,16787393,16787394,16787395,16787396,16787397,16787398,16787399,16787400,16787401,
    16787402,16787403,16787404,16787405,16787406,16787407,16787408,16787409,16787410,16787411,16787412,16787413,
    16787414,16787415,16787416,16787417,16787418,16787419,16787420,16787421,16787422,16787423,16787424,16787425,
    16787426,16787427,16787428,16787429,16787430,16787431,16787432,16787433,16787434,16787435,16787436,16787437,
    16787438,16787439,16787440,16787441,16787442,16787443,16787444,16787445,16787446,16787447,16787448,16787449,
    16787450,16787451,16787452,16787453,16787454,16787455,16787456,16787457,16787458,16787459,16787460,16787461,
    16787462,16787463,16787464,16787465,16787466,16787467,16787468,16787469,16787470,16787471,16787472,16787473,
    16787474,16787475,16787476,16787477,16787478,16787479,16787480,16787481,16787482,16787483,16787484,16787485,
    16787486,16787487,16787488,16787489,16787490,16787491,16787492,16787493,16787494,16787495,16787496,16787497,
    16787498,16787499,16787500,16787501,16787502,16787503,16787504,16787505,16787506,16787507,16787508,16787509,
    16787510,16787511,16787512,16787513,16787514,16787515,16787516,16787517,16787518,16787519,16787520,16787521,
    16787522,16787523,16787524,16787525,16787526,16787527,16787528,16787529,16787530,16787531,16787532,16787533,
    16787534,16787535,16787536,16787537,16787538,16787539,16787540,16787541,16787542,16787543,16787544,16787545,
    16787546,16787547,16787548,16787549,16787550,16787551,16787552,16787553,16787554,16787555,16787556,16787557,
    16787558,16787559,16787560,16787561,16787562,16787563,16787564,16787565,16787566,16787567,16787568,16787569,
    16787570,16787571,16787572,16787573,16787574,16787575,16787576,16787577,16787578,16787579,16787580,16787581,
    16787582,16787583,16787584,16787585,16787586,16787587,16787588,16787589,16787590,16787591,16787592,16787593,
    16787594,16787595,16787596,16787597,16787598,16787599,16787600,16787601,16787602,16787603,16787604,16787605,
    16787606,16787607,16787608,16787609,16787610,16787611,16787612,16787613,16787614,16787615,16787616,16787617,
    16787618,16787619,16787620,16787621,16787622,16787623,16787624,16787625,16787626,16787627,16787628,16787629,
    16787630,16787631,16787632,16787633,16787634,16787635,16787636,16787637,16787638,16787639,16787640,16787641,
    16787642,16787643,16787644,16787645,16787646,16787647,16787648,16787649,16787650,16787651,16787652,16787653,
};
static const uint16_t tv_coll_ce[10438][3] = {
    {2, 1, 1}, {3, 1, 1}, {4, 1, 1}, {5, 1, 1}, {6, 1, 1}, {10, 1, 1}, {39, 1, 1}, {66, 1, 1},
    {166, 1, 1}, {4824, 1, 1}, {167, 1, 1}, {164, 1, 1}, {63, 1, 1}, {69, 1, 1}, {70, 1, 1}, {158, 1, 1},
    {426, 1, 1}, {26, 1, 1}, {14, 1, 1}, {47, 1, 1}, {162, 1, 1}, {4861, 1, 1}, {4862, 1, 1}, {4874, 1, 1},
    {4879, 1, 1}, {4881, 1, 1}, {4883, 1, 1}, {4885, 1, 1}, {4887, 1, 1}, {4889, 1, 1}, {4891, 1, 1}, {36, 1, 1},
    {33, 1, 1}, {430, 1, 1}, {431, 1, 1}, {432, 1, 1}, {42, 1, 1}, {157, 1, 1}, {4898, 1, 7}, {4904, 1, 5},
    {4909, 1, 7}, {4915, 1, 9}, {4923, 1, 9}, {4935, 1, 5}, {4940, 1, 7}, {4948, 1, 9}, {4957, 1, 9}, {4962, 1, 7},
    {4968, 1, 5}, {4971, 1, 9}, {4982, 1, 7}, {4984, 1, 5}, {4992, 1, 9}, {4998, 1, 5}, {5002, 1, 5}, {5006, 1, 7},
    {5017, 1, 5}, {5028, 1, 7}, {5038, 1, 7}, {5048, 1, 7}, {5053, 1, 5}, {5055, 1, 7}, {5056, 1, 5}, {5062, 1, 5},
    {71, 1, 1}, {163, 1, 1}, {72, 1, 1}, {204, 1, 1}, {12, 1, 1}, {201, 1, 1}, {4898, 1, 1}, {4904, 1, 1},
    {4909, 1, 1}, {4915, 1, 1}, {4923, 1, 1}, {4935, 1, 1}, {4940, 1, 1}, {4948, 1, 1}, {4957, 1, 1}, {4962, 1, 1},
    {4968, 1, 1}, {4971, 1, 1}, {4982, 1, 1}, {4984, 1, 1}, {4992, 1, 1}, {4998, 1, 1}, {5002, 1, 1}, {5006, 1, 1},
    {5017, 1, 1}, {5028, 1, 1}, {5038, 1, 1}, {5048, 1, 1}, {5053, 1, 1}, {5055, 1, 1}, {5056, 1, 1}, {5062, 1, 1},
    {73, 1, 1}, {434, 1, 1}, {74, 1, 1}, {436, 1, 1}, {7, 1, 1}, {10, 1, 7}, {40, 1, 1}, {4823, 1, 1},
    {4825, 1, 1}, {4822, 1, 1}, {4826, 1, 1}, {435, 1, 1}, {154, 1, 1}, {208, 1, 1}, {278, 1, 1}, {4898, 1, 11},
    {67, 1, 1}, {433, 1, 1}, {280, 1, 1}, {205, 1, 1}, {269, 1, 1}, {427, 1, 1}, {4874, 1, 5}, {4879, 1, 5},
    {202, 1, 1}, {5115, 1, 3}, {155, 1, 1}, {50, 1, 1}, {211, 1, 1}, {4862, 1, 5}, {4992, 1, 12}, {68, 1, 1},
    {4862, 1, 2}, {442, 1, 2}, {4881, 1, 2}, {4862, 1, 2}, {442, 1, 2}, {4874, 1, 2}, {4879, 1, 2}, {442, 1, 2},
    {4881, 1, 2}, {43, 1, 1}, {4898, 1, 7}, {0, 7, 1}, {4898, 1, 7}, {0, 6, 1}, {4898, 1, 7}, {0, 9, 1},
    {4898, 1, 7}, {0, 15, 1}, {4898, 1, 7}, {0, 13, 1}, {4898, 1, 7}, {0, 11, 1}, {4898, 1, 2}, {4923, 1, 2},
    {4909, 1, 7}, {0, 19, 1}, {4923, 1, 9}, {0, 7, 1}, {4923, 1, 9}, {0, 6, 1}, {4923, 1, 9}, {0, 9, 1},
    {4923, 1, 9}, {0, 13, 1}, {4957, 1, 9}, {0, 7, 1}, {4957, 1, 9}, {0, 6, 1}, {4957, 1, 9}, {0, 9, 1},
    {4957, 1, 9}, {0, 13, 1}, {4915, 1, 3}, {0, 66, 1}, {4984, 1, 5}, {0, 15, 1}, {4992, 1, 9}, {0, 7, 1},
    {4992, 1, 9}, {0, 6, 1}, {4992, 1, 9}, {0, 9, 1}, {4992, 1, 9}, {0, 15, 1}, {4992, 1, 9}, {0, 13, 1},
    {429, 1, 1}, {4992, 1, 3}, {0, 18, 1}, {5038, 1, 7}, {0, 7, 1}, {5038, 1, 7}, {0, 6, 1}, {5038, 1, 7},
    {0, 9, 1}, {5038, 1, 7}, {0, 13, 1}, {5056, 1, 5}, {0, 6, 1}, {5072, 1, 3}, {5017, 1, 2}, {5017, 1, 2},
    {4898, 1, 1}, {0, 7, 1}, {4898, 1, 1}, {0, 6, 1}, {4898, 1, 1}, {0, 9, 1}, {4898, 1, 1}, {0, 15, 1},
    {4898, 1, 1}, {0, 13, 1}, {4898, 1, 1}, {0, 11, 1}, {4898, 1, 2}, {4923, 1, 2}, {4909, 1, 1}, {0, 19, 1},
    {4923, 1, 1}, {0, 7, 1}, {4923, 1, 1}, {0, 6, 1}, {4923, 1, 1}, {0, 9, 1}, {4923, 1, 1}, {0, 13, 1},
    {4957, 1, 1}, {0, 7, 1}, {4957, 1, 1}, {0, 6, 1}, {4957, 1, 1}, {0, 9, 1}, {4957, 1, 1}, {0, 13, 1},
    {4915, 1, 1}, {0, 66, 1}, {4984, 1, 1}, {0, 15, 1}, {4992, 1, 1}, {0, 7, 1}, {4992, 1, 1}, {0, 6, 1},
    {4992, 1, 1}, {0, 9, 1}, {4992, 1, 1}, {0, 15, 1}, {4992, 1, 1}, {0, 13, 1}, {428, 1, 1}, {4992, 1, 1},
    {0, 18, 1}, {5038, 1, 1}, {0, 7, 1}, {5038, 1, 1}, {0, 6, 1}, {5038, 1, 1}, {0, 9, 1}, {5038, 1, 1},
    {0, 13, 1}, {5056, 1, 1}, {0, 6, 1}, {5072, 1, 1}, {5056, 1, 1}, {0, 13, 1}, {4898, 1, 7}, {0, 21, 1},
    {4898, 1, 1}, {0, 21, 1}, {4898, 1, 7}, {0, 8, 1}, {4898, 1, 1}, {0, 8, 1}, {4898, 1, 7}, {0, 20, 1},
    {4898, 1, 1}, {0, 20, 1}, {4909, 1, 7}, {0, 6, 1}, {4909, 1, 1}, {0, 6, 1}, {4909, 1, 7}, {0, 9, 1},
    {4909, 1, 1}, {0, 9, 1}, {4909, 1, 7}, {0, 16, 1}, {4909, 1, 1}, {0, 16, 1}, {4909, 1, 7}, {0, 10, 1},
    {4909, 1, 1}, {0, 10, 1}, {4915, 1, 9}, {0, 10, 1}, {4915, 1, 1}, {0, 10, 1}, {4915, 1, 3}, {0, 28, 1},
    {4915, 1, 1}, {0, 28, 1}, {4923, 1, 9}, {0, 21, 1}, {4923, 1, 1}, {0, 21, 1}, {4923, 1, 9}, {0, 8, 1},
    {4923, 1, 1}, {0, 8, 1}, {4923, 1, 9}, {0, 16, 1}, {4923, 1, 1}, {0, 16, 1}, {4923, 1, 9}, {0, 20, 1},
    {4923, 1, 1}, {0, 20, 1}, {4923, 1, 9}, {0, 10, 1}, {4923, 1, 1}, {0, 10, 1}, {4940, 1, 7}, {0, 9, 1},
    {4940, 1, 1}, {0, 9, 1}, {4940, 1, 7}, {0, 8, 1}, {4940, 1, 1}, {0, 8, 1}, {4940, 1, 7}, {0, 16, 1},
    {4940, 1, 1}, {0, 16, 1}, {4940, 1, 7}, {0, 19, 1}, {4940, 1, 1}, {0, 19, 1}, {4948, 1, 9}, {0, 9, 1},
    {4948, 1, 1}, {0, 9, 1}, {4948, 1, 3}, {0, 28, 1}, {4948, 1, 1}, {0, 28, 1}, {4957, 1, 9}, {0, 15, 1},
    {4957, 1, 1}, {0, 15, 1}, {4957, 1, 9}, {0, 21, 1}, {4957, 1, 1}, {0, 21, 1}, {4957, 1, 9}, {0, 8, 1},
    {4957, 1, 1}, {0, 8, 1}, {4957, 1, 9}, {0, 20, 1}, {4957, 1, 1}, {0, 20, 1}, {4957, 1, 9}, {0, 16, 1},
    {4958, 1, 1}, {4957, 1, 10}, {4962, 1, 8}, {4957, 1, 2}, {4962, 1, 2}, {4962, 1, 7}, {0, 9, 1}, {4962, 1, 1},
    {0, 9, 1}, {4968, 1, 5}, {0, 19, 1}, {4968, 1, 1}, {0, 19, 1}, {5005, 1, 1}, {4971, 1, 9}, {0, 6, 1},
    {4971, 1, 1}, {0, 6, 1}, {4971, 1, 9}, {0, 19, 1}, {4971, 1, 1}, {0, 19, 1}, {4971, 1, 9}, {0, 10, 1},
    {4971, 1, 1}, {0, 10, 1}, {4971, 1, 10}, {50, 1, 2}, {4971, 1, 2}, {50, 1, 2}, {4971, 1, 3}, {0, 28, 1},
    {4971, 1, 1}, {0, 28, 1}, {4984, 1, 5}, {0, 6, 1}, {4984, 1, 1}, {0, 6, 1}, {4984, 1, 5}, {0, 19, 1},
    {4984, 1, 1}, {0, 19, 1}, {4984, 1, 5}, {0, 10, 1}, {4984, 1, 1}, {0, 10, 1}, {5081, 1, 2}, {4984, 1, 2},
    {4991, 1, 3}, {4991, 1, 1}, {4992, 1, 9}, {0, 21, 1}, {4992, 1, 1}, {0, 21, 1}, {4992, 1, 9}, {0, 8, 1},
    {4992, 1, 1}, {0, 8, 1}, {4992, 1, 9}, {0, 14, 1}, {4992, 1, 1}, {0, 14, 1}, {4992, 1, 2}, {4923, 1, 2},
    {4992, 1, 2}, {4923, 1, 2}, {5006, 1, 7}, {0, 6, 1}, {5006, 1, 1}, {0, 6, 1}, {5006, 1, 7}, {0, 19, 1},
    {5006, 1, 1}, {0, 19, 1}, {5006, 1, 7}, {0, 10, 1}, {5006, 1, 1}, {0, 10, 1}, {5017, 1, 5}, {0, 6, 1},
    {5017, 1, 1}, {0, 6, 1}, {5017, 1, 5}, {0, 9, 1}, {5017, 1, 1}, {0, 9, 1}, {5017, 1, 5}, {0, 19, 1},
    {5017, 1, 1}, {0, 19, 1}, {5017, 1, 5}, {0, 10, 1}, {5017, 1, 1}, {0, 10, 1}, {5028, 1, 7}, {0, 19, 1},
    {5028, 1, 1}, {0, 19, 1}, {5028, 1, 7}, {0, 10, 1}, {5028, 1, 1}, {0, 10, 1}, {5031, 1, 3}, {5031, 1, 1},
    {5038, 1, 7}, {0, 15, 1}, {5038, 1, 1}, {0, 15, 1}, {5038, 1, 7}, {0, 21, 1}, {5038, 1, 1}, {0, 21, 1},
    {5038, 1, 7}, {0, 8, 1}, {5038, 1, 1}, {0, 8, 1}, {5038, 1, 7}, {0, 11, 1}, {5038, 1, 1}, {0, 11, 1},
    {5038, 1, 7}, {0, 14, 1}, {5038, 1, 1}, {0, 14, 1}, {5038, 1, 7}, {0, 20, 1}, {5038, 1, 1}, {0, 20, 1},
    {5053, 1, 5}, {0, 9, 1}, {5053, 1, 1}, {0, 9, 1}, {5056, 1, 5}, {0, 9, 1}, {5056, 1, 1}, {0, 9, 1},
    {5056, 1, 5}, {0, 13, 1}, {5062, 1, 5}, {0, 6, 1}, {5062, 1, 1}, {0, 6, 1}, {5062, 1, 5}, {0, 16, 1},
    {5062, 1, 1}, {0, 16, 1}, {5062, 1, 5}, {0, 10, 1}, {5062, 1, 1}, {0, 10, 1}, {5017, 1, 1}, {0, 66, 1},
    {4906, 1, 1}, {4907, 1, 3}, {4908, 1, 3}, {4908, 1, 1}, {5077, 1, 3}, {5077, 1, 1}, {4994, 1, 3}, {4912, 1, 3},
    {4912, 1, 1}, {4918, 1, 3}, {4919, 1, 3}, {4920, 1, 3}, {4920, 1, 1}, {5062, 1, 2}, {5053, 1, 2}, {4925, 1, 3},
    {4926, 1, 3}, {4927, 1, 3}, {4938, 1, 3}, {4938, 1, 1}, {4944, 1, 3}, {4946, 1, 3}, {4952, 1, 1}, {4961, 1, 3},
    {4960, 1, 3}, {4969, 1, 3}, {4969, 1, 1}, {4974, 1, 1}, {4980, 1, 1}, {5045, 1, 3}, {4987, 1, 3}, {4988, 1, 1},
    {4995, 1, 3}, {4992, 1, 9}, {0, 34, 1}, {4992, 1, 1}, {0, 34, 1}, {4947, 1, 3}, {4947, 1, 1}, {5000, 1, 3},
    {5000, 1, 1}, {5007, 1, 3}, {5075, 1, 3}, {5075, 1, 1}, {5024, 1, 3}, {5025, 1, 1}, {5033, 1, 1}, {5034, 1, 3},
    {5034, 1, 1}, {5035, 1, 3}, {5038, 1, 7}, {0, 34, 1}, {5038, 1, 1}, {0, 34, 1}, {5047, 1, 3}, {5050, 1, 3},
    {5059, 1, 3}, {5059, 1, 1}, {5063, 1, 3}, {5063, 1, 1}, {5068, 1, 3}, {5069, 1, 3}, {5069, 1, 1}, {5070, 1, 1},
    {5074, 1, 1}, {5076, 1, 3}, {5076, 1, 1}, {5028, 1, 2}, {5017, 1, 2}, {5073, 1, 1}, {5090, 1, 1}, {5091, 1, 1},
    {5092, 1, 1}, {5093, 1, 1}, {4915, 1, 10}, {5062, 1, 6}, {0, 10, 2}, {4915, 1, 10}, {5062, 1, 2}, {0, 10, 2},
    {4915, 1, 2}, {5062, 1, 2}, {0, 10, 2}, {4971, 1, 10}, {4962, 1, 8}, {4971, 1, 10}, {4962, 1, 2}, {4971, 1, 2},
    {4962, 1, 2}, {4984, 1, 6}, {4962, 1, 8}, {4984, 1, 6}, {4962, 1, 2}, {4984, 1, 2}, {4962, 1, 2}, {4898, 1, 7},
    {0, 10, 1}, {4898, 1, 1}, {0, 10, 1}, {4957, 1, 9}, {0, 10, 1}, {4957, 1, 1}, {0, 10, 1}, {4992, 1, 9},
    {0, 10, 1}, {4992, 1, 1}, {0, 10, 1}, {5038, 1, 7}, {0, 10, 1}, {5038, 1, 1}, {0, 10, 1}, {5038, 1, 7},
    {0, 13, 1}, {0, 21, 1}, {5038, 1, 1}, {0, 13, 1}, {0, 21, 1}, {5038, 1, 7}, {0, 13, 1}, {0, 6, 1},
    {5038, 1, 1}, {0, 13, 1}, {0, 6, 1}, {5038, 1, 7}, {0, 13, 1}, {0, 10, 1}, {5038, 1, 1}, {0, 13, 1},
    {0, 10, 1}, {5038, 1, 7}, {0, 13, 1}, {0, 7, 1}, {5038, 1, 1}, {0, 13, 1}, {0, 7, 1}, {4925, 1, 1},
    {4898, 1, 7}, {0, 13, 1}, {0, 21, 1}, {4898, 1, 1}, {0, 13, 1}, {0, 21, 1}, {4898, 1, 7}, {0, 16, 1},
    {0, 21, 1}, {4898, 1, 1}, {0, 16, 1}, {0, 21, 1}, {4898, 1, 2}, {4923, 1, 2}, {0, 21, 1}, {4898, 1, 2},
    {4923, 1, 2}, {0, 21, 1}, {4943, 1, 3}, {4943, 1, 1}, {4940, 1, 7}, {0, 10, 1}, {4940, 1, 1}, {0, 10, 1},
    {4968, 1, 5}, {0, 10, 1}, {4968, 1, 1}, {0, 10, 1}, {4992, 1, 9}, {0, 20, 1}, {4992, 1, 1}, {0, 20, 1},
    {4992, 1, 9}, {0, 20, 1}, {0, 21, 1}, {4992, 1, 1}, {0, 20, 1}, {0, 21, 1}, {5068, 1, 3}, {0, 10, 1},
    {5068, 1, 1}, {0, 10, 1}, {4962, 1, 1}, {0, 10, 1}, {4915, 1, 10}, {5062, 1, 6}, {4915, 1, 10}, {5062, 1, 2},
    {4915, 1, 2}, {5062, 1, 2}, {4940, 1, 7}, {0, 6, 1}, {4940, 1, 1}, {0, 6, 1}, {4952, 1, 3}, {5073, 1, 3},
    {4984, 1, 5}, {0, 7, 1}, {4984, 1, 1}, {0, 7, 1}, {4898, 1, 7}, {0, 11, 1}, {0, 6, 1}, {4898, 1, 1},
    {0, 11, 1}, {0, 6, 1}, {4898, 1, 2}, {4923, 1, 2}, {0, 6, 1}, {4898, 1, 2}, {4923, 1, 2}, {0, 6, 1},
    {4992, 1, 3}, {0, 18, 1}, {0, 6, 1}, {4992, 1, 1}, {0, 18, 1}, {0, 6, 1}, {4898, 1, 7}, {0, 31, 1},
    {4898, 1, 1}, {0, 31, 1}, {4898, 1, 7}, {0, 33, 1}, {4898, 1, 1}, {0, 33, 1}, {4923, 1, 9}, {0, 31, 1},
    {4923, 1, 1}, {0, 31, 1}, {4923, 1, 9}, {0, 33, 1}, {4923, 1, 1}, {0, 33, 1}, {4957, 1, 9}, {0, 31, 1},
    {4957, 1, 1}, {0, 31, 1}, {4957, 1, 9}, {0, 33, 1}, {4957, 1, 1}, {0, 33, 1}, {4992, 1, 9}, {0, 31, 1},
    {4992, 1, 1}, {0, 31, 1}, {4992, 1, 9}, {0, 33, 1}, {4992, 1, 1}, {0, 33, 1}, {5006, 1, 7}, {0, 31, 1},
    {5006, 1, 1}, {0, 31, 1}, {5006, 1, 7}, {0, 33, 1}, {5006, 1, 1}, {0, 33, 1}, {5038, 1, 7}, {0, 31, 1},
    {5038, 1, 1}, {0, 31, 1}, {5038, 1, 7}, {0, 33, 1}, {5038, 1, 1}, {0, 33, 1}, {5017, 1, 5}, {0, 40, 1},
    {5017, 1, 1}, {0, 40, 1}, {5028, 1, 7}, {0, 40, 1}, {5028, 1, 1}, {0, 40, 1}, {5061, 1, 3}, {5061, 1, 1},
    {4948, 1, 9}, {0, 10, 1}, {4948, 1, 1}, {0, 10, 1}, {4988, 1, 3}, {4921, 1, 1}, {4997, 1, 3}, {4997, 1, 1},
    {5064, 1, 3}, {5064, 1, 1}, {4898, 1, 7}, {0, 16, 1}, {4898, 1, 1}, {0, 16, 1}, {4923, 1, 9}, {0, 19, 1},
    {4923, 1, 1}, {0, 19, 1}, {4992, 1, 9}, {0, 13, 1}, {0, 21, 1}, {4992, 1, 1}, {0, 13, 1}, {0, 21, 1},
    {4992, 1, 9}, {0, 15, 1}, {0, 21, 1}, {4992, 1, 1}, {0, 15, 1}, {0, 21, 1}, {4992, 1, 9}, {0, 16, 1},
    {4992, 1, 1}, {0, 16, 1}, {4992, 1, 9}, {0, 16, 1}, {0, 21, 1}, {4992, 1, 1}, {0, 16, 1}, {0, 21, 1},
    {5056, 1, 5}, {0, 21, 1}, {5056, 1, 1}, {0, 21, 1}, {4978, 1, 1}, {4990, 1, 1}, {5036, 1, 1}, {4963, 1, 1},
    {4915, 1, 2}, {4904, 1, 2}, {5002, 1, 2}, {4998, 1, 2}, {4900, 1, 1}, {4911, 1, 3}, {4911, 1, 1}, {4974, 1, 3},
    {5032, 1, 1}, {5021, 1, 1}, {5067, 1, 1}, {5079, 1, 3}, {5079, 1, 1}, {4906, 1, 3}, {5041, 1, 3}, {5052, 1, 3},
    {4924, 1, 3}, {4924, 1, 1}, {4964, 1, 3}, {4964, 1, 1}, {5004, 1, 3}, {5004, 1, 1}, {5008, 1, 3}, {5008, 1, 1},
    {5058, 1, 3}, {5058, 1, 1}, {4901, 1, 1}, {4902, 1, 1}, {4903, 1, 1}, {4907, 1, 1}, {4994, 1, 1}, {4913, 1, 1},
    {4918, 1, 1}, {4919, 1, 1}, {4928, 1, 1}, {4926, 1, 1}, {4929, 1, 1}, {4927, 1, 1}, {4930, 1, 1}, {4931, 1, 1},
    {4932, 1, 1}, {4966, 1, 1}, {4944, 1, 1}, {4941, 1, 1}, {4942, 1, 1}, {4946, 1, 1}, {4934, 1, 1}, {5042, 1, 1},
    {4953, 1, 1}, {4954, 1, 1}, {4960, 1, 1}, {4961, 1, 1}, {4959, 1, 1}, {4975, 1, 1}, {4976, 1, 1}, {4977, 1, 1},
    {4979, 1, 1}, {5045, 1, 1}, {5046, 1, 1}, {4983, 1, 1}, {4987, 1, 1}, {4989, 1, 1}, {4986, 1, 1}, {4995, 1, 1},
    {4993, 1, 1}, {4996, 1, 1}, {5001, 1, 1}, {5009, 1, 1}, {5010, 1, 1}, {5011, 1, 1}, {5012, 1, 1}, {5013, 1, 1},
    {5014, 1, 1}, {5015, 1, 1}, {5007, 1, 1}, {5016, 1, 1}, {5020, 1, 1}, {5024, 1, 1}, {4967, 1, 1}, {5026, 1, 1},
    {5027, 1, 1}, {5037, 1, 1}, {5035, 1, 1}, {5041, 1, 1}, {5047, 1, 1}, {5050, 1, 1}, {5052, 1, 1}, {5054, 1, 1},
    {4981, 1, 1}, {5057, 1, 1}, {5065, 1, 1}, {5066, 1, 1}, {5068, 1, 1}, {5071, 1, 1}, {5078, 1, 1}, {5084, 1, 1},
    {5089, 1, 1}, {5094, 1, 1}, {5095, 1, 1}, {4905, 1, 1}, {4933, 1, 1}, {4945, 1, 1}, {4951, 1, 1}, {4965, 1, 1},
    {4970, 1, 1}, {4973, 1, 1}, {5003, 1, 1}, {5087, 1, 1}, {5088, 1, 1}, {4915, 1, 2}, {5062, 1, 2}, {4917, 1, 1},
    {4916, 1, 1}, {5028, 1, 2}, {5017, 1, 2}, {5030, 1, 1}, {5029, 1, 1}, {4936, 1, 1}, {4971, 1, 2}, {5017, 1, 2},
    {4971, 1, 2}, {5062, 1, 2}, {5096, 1, 1}, {5097, 1, 1}, {5043, 1, 1}, {5044, 1, 1}, {4948, 1, 13}, {4953, 1, 3},
    {4962, 1, 11}, {5006, 1, 12}, {5009, 1, 3}, {5011, 1, 3}, {5016, 1, 3}, {5053, 1, 9}, {5056, 1, 9}, {218, 1, 1},
    {220, 1, 1}, {4955, 1, 1}, {5081, 1, 1}, {4956, 1, 1}, {5083, 1, 1}, {5085, 1, 1}, {5080, 1, 1}, {5086, 1, 1},
    {221, 1, 1}, {222, 1, 1}, {223, 1, 1}, {224, 1, 1}, {225, 1, 1}, {226, 1, 1}, {227, 1, 1}, {228, 1, 1},
    {229, 1, 1}, {230, 1, 1}, {231, 1, 1}, {232, 1, 1}, {233, 1, 1}, {234, 1, 1}, {4811, 1, 1}, {4812, 1, 1},
    {235, 1, 1}, {236, 1, 1}, {237, 1, 1}, {238, 1, 1}, {239, 1, 1}, {240, 1, 1}, {206, 1, 1}, {207, 1, 1},
    {209, 1, 1}, {212, 1, 1}, {203, 1, 1}, {210, 1, 1}, {241, 1, 1}, {242, 1, 1}, {4946, 1, 5}, {4971, 1, 14},
    {5017, 1, 9}, {5055, 1, 12}, {5084, 1, 3}, {243, 1, 1}, {244, 1, 1}, {245, 1, 1}, {246, 1, 1}, {247, 1, 1},
    {248, 1, 1}, {249, 1, 1}, {250, 1, 1}, {251, 1, 1}, {5082, 1, 1}, {252, 1, 1}, {253, 1, 1}, {254, 1, 1},
    {255, 1, 1}, {256, 1, 1}, {257, 1, 1}, {258, 1, 1}, {259, 1, 1}, {260, 1, 1}, {261, 1, 1}, {262, 1, 1},
    {263, 1, 1}, {264, 1, 1}, {265, 1, 1}, {266, 1, 1}, {267, 1, 1}, {268, 1, 1}, {0, 7, 1}, {0, 6, 1},
    {0, 9, 1}, {0, 15, 1}, {0, 21, 1}, {0, 29, 1}, {0, 8, 1}, {0, 16, 1}, {0, 13, 1}, {0, 30, 1},
    {0, 11, 1}, {0, 14, 1}, {0, 10, 1}, {0, 22, 1}, {0, 22, 1}, {0, 31, 1}, {0, 32, 1}, {0, 33, 1},
    {0, 22, 1}, {0, 4, 1}, {0, 5, 1}, {0, 22, 1}, {0, 24, 1}, {0, 24, 1}, {0, 24, 1}, {0, 24, 1},
    {0, 22, 1}, {0, 34, 1}, {0, 24, 1}, {0, 24, 1}, {0, 24, 1}, {0, 24, 1}, {0, 24, 1}, {0, 35, 1},
    {0, 36, 1}, {0, 37, 1}, {0, 38, 1}, {0, 39, 1}, {0, 40, 1}, {0, 19, 1}, {0, 20, 1}, {0, 24, 1},
    {0, 24, 1}, {0, 24, 1}, {0, 24, 1}, {0, 41, 1}, {0, 42, 1}, {0, 24, 1}, {0, 43, 1}, {0, 44, 1},
    {0, 3, 1}, {0, 24, 1}, {0, 45, 1}, {0, 28, 1}, {0, 25, 1}, {0, 25, 1}, {0, 18, 1}, {0, 46, 1},
    {0, 24, 1}, {0, 24, 1}, {0, 24, 1}, {0, 22, 1}, {0, 22, 1}, {0, 22, 1}, {0, 7, 1}, {0, 6, 1},
    {0, 12, 1}, {0, 4, 1}, {0, 13, 1}, {0, 6, 1}, {0, 47, 1}, {0, 22, 1}, {0, 24, 1}, {0, 24, 1},
    {0, 24, 1}, {0, 22, 1}, {0, 22, 1}, {0, 22, 1}, {0, 24, 1}, {0, 24, 1}, {0, 2, 1}, {0, 22, 1},
    {0, 22, 1}, {0, 22, 1}, {0, 24, 1}, {0, 24, 1}, {0, 24, 1}, {0, 24, 1}, {0, 22, 1}, {0, 48, 1},
    {0, 24, 1}, {0, 24, 1}, {0, 22, 1}, {0, 24, 1}, {0, 22, 1}, {0, 22, 1}, {0, 24, 1}, {0, 49, 1},
    {0, 50, 1}, {0, 24, 1}, {4898, 1, 3}, {4923, 1, 3}, {4957, 1, 3}, {4992, 1, 3}, {5038, 1, 3}, {4909, 1, 3},
    {4915, 1, 3}, {4948, 1, 3}, {4982, 1, 3}, {5006, 1, 3}, {5028, 1, 3}, {5048, 1, 3}, {5055, 1, 3}, {5107, 1, 3},
    {5107, 1, 1}, {5136, 1, 3}, {5136, 1, 1}, {218, 1, 1}, {219, 1, 1}, {5104, 1, 3}, {5104, 1, 1}, {5922, 1, 1},
    {5923, 1, 1}, {5110, 1, 3}, {5127, 1, 1}, {5126, 1, 1}, {5128, 1, 1}, {33, 1, 1}, {5111, 1, 3}, {5924, 1, 1},
    {5925, 1, 1}, {5926, 1, 1}, {5927, 1, 1}, {202, 1, 1}, {208, 1, 1}, {0, 6, 1}, {5098, 1, 3}, {0, 6, 1},
    {50, 1, 1}, {5102, 1, 5}, {0, 6, 1}, {5108, 1, 3}, {0, 6, 1}, {5110, 1, 5}, {0, 6, 1}, {5928, 1, 1},
    {5118, 1, 3}, {0, 6, 1}, {5929, 1, 1}, {5130, 1, 3}, {0, 6, 1}, {5134, 1, 3}, {0, 6, 1}, {5110, 1, 1},
    {0, 13, 1}, {0, 6, 1}, {5098, 1, 3}, {5099, 1, 5}, {5100, 1, 5}, {5101, 1, 3}, {5102, 1, 5}, {5106, 1, 3},
    {5108, 1, 3}, {5109, 1, 5}, {5110, 1, 5}, {5112, 1, 5}, {5114, 1, 3}, {5115, 1, 5}, {5116, 1, 3}, {5117, 1, 3},
    {5118, 1, 3}, {5119, 1, 7}, {5123, 1, 5}, {5930, 1, 1}, {5125, 1, 5}, {5129, 1, 3}, {5130, 1, 3}, {5131, 1, 5},
    {5132, 1, 3}, {5133, 1, 3}, {5134, 1, 3}, {5110, 1, 5}, {0, 13, 1}, {5130, 1, 3}, {0, 13, 1}, {5098, 1, 1},
    {0, 6, 1}, {5102, 1, 1}, {0, 6, 1}, {5108, 1, 1}, {0, 6, 1}, {5110, 1, 1}, {0, 6, 1}, {5130, 1, 1},
    {0, 13, 1}, {0, 6, 1}, {5098, 1, 1}, {5099, 1, 1}, {5100, 1, 1}, {5101, 1, 1}, {5102, 1, 1}, {5106, 1, 1},
    {5108, 1, 1}, {5109, 1, 1}, {5110, 1, 1}, {5112, 1, 1}, {5114, 1, 1}, {5115, 1, 1}, {5116, 1, 1}, {5117, 1, 1},
    {5118, 1, 1}, {5119, 1, 1}, {5123, 1, 1}, {5125, 1, 9}, {5125, 1, 1}, {5129, 1, 1}, {5130, 1, 1}, {5131, 1, 1},
    {5132, 1, 1}, {5133, 1, 1}, {5134, 1, 1}, {5110, 1, 1}, {0, 13, 1}, {5130, 1, 1}, {0, 13, 1}, {5118, 1, 1},
    {0, 6, 1}, {5130, 1, 1}, {0, 6, 1}, {5134, 1, 1}, {0, 6, 1}, {5113, 1, 3}, {5099, 1, 3}, {5109, 1, 3},
    {5130, 1, 5}, {5130, 1, 5}, {0, 6, 1}, {5130, 1, 5}, {0, 13, 1}, {5131, 1, 3}, {5119, 1, 3}, {5113, 1, 1},
    {5122, 1, 3}, {5122, 1, 1}, {5105, 1, 3}, {5105, 1, 1}, {5103, 1, 3}, {5103, 1, 1}, {5121, 1, 3}, {5121, 1, 1},
    {5135, 1, 3}, {5135, 1, 1}, {5138, 1, 3}, {5138, 1, 1}, {5139, 1, 3}, {5139, 1, 1}, {5140, 1, 3}, {5140, 1, 1},
    {5141, 1, 3}, {5141, 1, 1}, {5142, 1, 3}, {5142, 1, 1}, {5143, 1, 3}, {5143, 1, 1}, {5144, 1, 3}, {5144, 1, 1},
    {5112, 1, 3}, {5123, 1, 3}, {5125, 1, 3}, {5111, 1, 1}, {5109, 1, 7}, {5102, 1, 3}, {421, 1, 1}, {5137, 1, 3},
    {5137, 1, 1}, {5125, 1, 7}, {5120, 1, 3}, {5120, 1, 1}, {5124, 1, 1}, {5127, 1, 3}, {5126, 1, 3}, {5128, 1, 3},
    {5160, 1, 3}, {0, 7, 1}, {5160, 1, 3}, {0, 13, 1}, {5157, 1, 3}, {5150, 1, 3}, {0, 6, 1}, {5161, 1, 3},
    {5168, 1, 3}, {5173, 1, 3}, {5173, 1, 3}, {0, 13, 1}, {5174, 1, 3}, {5188, 1, 3}, {5200, 1, 3}, {5217, 1, 3},
    {5175, 1, 3}, {0, 6, 1}, {5171, 1, 3}, {0, 7, 1}, {5218, 1, 3}, {0, 8, 1}, {5242, 1, 3}, {5145, 1, 3},
    {5148, 1, 3}, {5149, 1, 3}, {5150, 1, 3}, {5155, 1, 3}, {5160, 1, 3}, {5162, 1, 3}, {5165, 1, 3}, {5171, 1, 3},
    {5171, 1, 3}, {0, 8, 1}, {5175, 1, 3}, {5183, 1, 3}, {5191, 1, 3}, {5193, 1, 3}, {5202, 1, 3}, {5204, 1, 3},
    {5208, 1, 3}, {5211, 1, 3}, {5214, 1, 3}, {5218, 1, 3}, {5222, 1, 3}, {5223, 1, 3}, {5233, 1, 3}, {5235, 1, 3},
    {5243, 1, 3}, {5244, 1, 3}, {5245, 1, 3}, {5246, 1, 3}, {5247, 1, 3}, {5250, 1, 3}, {5251, 1, 3}, {5252, 1, 3},
    {5145, 1, 1}, {5148, 1, 1}, {5149, 1, 1}, {5150, 1, 1}, {5155, 1, 1}, {5160, 1, 1}, {5162, 1, 1}, {5165, 1, 1},
    {5171, 1, 1}, {5171, 1, 1}, {0, 8, 1}, {5175, 1, 1}, {5183, 1, 1}, {5191, 1, 1}, {5193, 1, 1}, {5202, 1, 1},
    {5204, 1, 1}, {5208, 1, 1}, {5211, 1, 1}, {5214, 1, 1}, {5218, 1, 1}, {5222, 1, 1}, {5223, 1, 1}, {5233, 1, 1},
    {5235, 1, 1}, {5243, 1, 1}, {5244, 1, 1}, {5245, 1, 1}, {5246, 1, 1}, {5247, 1, 1}, {5250, 1, 1}, {5251, 1, 1},
    {5252, 1, 1}, {5160, 1, 1}, {0, 7, 1}, {5160, 1, 1}, {0, 13, 1}, {5157, 1, 1}, {5150, 1, 1}, {0, 6, 1},
    {5161, 1, 1}, {5168, 1, 1}, {5173, 1, 1}, {5173, 1, 1}, {0, 13, 1}, {5174, 1, 1}, {5188, 1, 1}, {5200, 1, 1},
    {5217, 1, 1}, {5175, 1, 1}, {0, 6, 1}, {5171, 1, 1}, {0, 7, 1}, {5218, 1, 1}, {0, 8, 1}, {5242, 1, 1},
    {5229, 1, 3}, {5229, 1, 1}, {5249, 1, 3}, {5249, 1, 1}, {5254, 1, 3}, {5254, 1, 1}, {5255, 1, 3}, {5255, 1, 1},
    {5257, 1, 3}, {5257, 1, 1}, {5256, 1, 3}, {5256, 1, 1}, {5258, 1, 3}, {5258, 1, 1}, {5259, 1, 3}, {5259, 1, 1},
    {5260, 1, 3}, {5260, 1, 1}, {5261, 1, 3}, {5261, 1, 1}, {5262, 1, 3}, {5262, 1, 1}, {5262, 1, 3}, {0, 31, 1},
    {5262, 1, 1}, {0, 31, 1}, {5221, 1, 3}, {5221, 1, 1}, {5232, 1, 3}, {5232, 1, 1}, {5231, 1, 3}, {5231, 1, 1},
    {5230, 1, 3}, {5230, 1, 1}, {5207, 1, 3}, {5207, 1, 1}, {270, 1, 1}, {1, 1, 12}, {0, 53, 1}, {1, 1, 7},
    {0, 23, 1}, {1, 1, 1}, {0, 17, 1}, {1, 1, 1}, {1, 1, 7}, {0, 23, 1}, {5172, 1, 3}, {5172, 1, 1},
    {5248, 1, 3}, {5248, 1, 1}, {5209, 1, 3}, {5209, 1, 1}, {5150, 1, 3}, {0, 66, 1}, {5150, 1, 1}, {0, 66, 1},
    {5151, 1, 3}, {5151, 1, 1}, {5153, 1, 3}, {5153, 1, 1}, {5164, 1, 3}, {5164, 1, 1}, {5159, 1, 3}, {5159, 1, 1},
    {5176, 1, 3}, {5176, 1, 1}, {5180, 1, 3}, {5180, 1, 1}, {5179, 1, 3}, {5179, 1, 1}, {5178, 1, 3}, {5178, 1, 1},
    {5196, 1, 3}, {5196, 1, 1}, {5199, 1, 3}, {5199, 1, 1}, {5206, 1, 3}, {5206, 1, 1}, {5263, 1, 3}, {5263, 1, 1},
    {5213, 1, 3}, {5213, 1, 1}, {5216, 1, 3}, {5216, 1, 1}, {5219, 1, 3}, {5219, 1, 1}, {5220, 1, 3}, {5220, 1, 1},
    {5226, 1, 3}, {5226, 1, 1}, {5234, 1, 3}, {5234, 1, 1}, {5237, 1, 3}, {5237, 1, 1}, {5239, 1, 3}, {5239, 1, 1},
    {5227, 1, 3}, {5227, 1, 1}, {5240, 1, 3}, {5240, 1, 1}, {5241, 1, 3}, {5241, 1, 1}, {5265, 1, 3}, {5162, 1, 3},
    {0, 8, 1}, {5162, 1, 1}, {0, 8, 1}, {5177, 1, 3}, {5177, 1, 1}, {5184, 1, 3}, {5184, 1, 1}, {5197, 1, 3},
    {5197, 1, 1}, {5195, 1, 3}, {5195, 1, 1}, {5238, 1, 3}, {5238, 1, 1}, {5192, 1, 3}, {5192, 1, 1}, {5265, 1, 1},
    {5145, 1, 3}, {0, 8, 1}, {5145, 1, 1}, {0, 8, 1}, {5145, 1, 3}, {0, 13, 1}, {5145, 1, 1}, {0, 13, 1},
    {5147, 1, 3}, {5147, 1, 1}, {5160, 1, 3}, {0, 8, 1}, {5160, 1, 1}, {0, 8, 1}, {5146, 1, 3}, {5146, 1, 1},
    {5146, 1, 3}, {0, 13, 1}, {5146, 1, 1}, {0, 13, 1}, {5162, 1, 3}, {0, 13, 1}, {5162, 1, 1}, {0, 13, 1},
    {5165, 1, 3}, {0, 13, 1}, {5165, 1, 1}, {0, 13, 1}, {5169, 1, 3}, {5169, 1, 1}, {5171, 1, 3}, {0, 21, 1},
    {5171, 1, 1}, {0, 21, 1}, {5171, 1, 3}, {0, 13, 1}, {5171, 1, 1}, {0, 13, 1}, {5202, 1, 3}, {0, 13, 1},
    {5202, 1, 1}, {0, 13, 1}, {5203, 1, 3}, {5203, 1, 1}, {5203, 1, 3}, {0, 13, 1}, {5203, 1, 1}, {0, 13, 1},
    {5250, 1, 3}, {0, 13, 1}, {5250, 1, 1}, {0, 13, 1}, {5218, 1, 3}, {0, 21, 1}, {5218, 1, 1}, {0, 21, 1},
    {5218, 1, 3}, {0, 13, 1}, {5218, 1, 1}, {0, 13, 1}, {5218, 1, 3}, {0, 14, 1}, {5218, 1, 1}, {0, 14, 1},
    {5235, 1, 3}, {0, 13, 1}, {5235, 1, 1}, {0, 13, 1}, {5154, 1, 3}, {5154, 1, 1}, {5246, 1, 3}, {0, 13, 1},
    {5246, 1, 1}, {0, 13, 1}, {5152, 1, 3}, {5152, 1, 1}, {5224, 1, 3}, {5224, 1, 1}, {5225, 1, 3}, {5225, 1, 1},
    {5156, 1, 3}, {5156, 1, 1}, {5158, 1, 3}, {5158, 1, 1}, {5166, 1, 3}, {5166, 1, 1}, {5170, 1, 3}, {5170, 1, 1},
    {5189, 1, 3}, {5189, 1, 1}, {5201, 1, 3}, {5201, 1, 1}, {5212, 1, 3}, {5212, 1, 1}, {5215, 1, 3}, {5215, 1, 1},
    {5167, 1, 3}, {5167, 1, 1}, {5186, 1, 3}, {5186, 1, 1}, {5190, 1, 3}, {5190, 1, 1}, {5210, 1, 3}, {5210, 1, 1},
    {5253, 1, 3}, {5253, 1, 1}, {5182, 1, 3}, {5182, 1, 1}, {5264, 1, 3}, {5264, 1, 1}, {5181, 1, 3}, {5181, 1, 1},
    {5187, 1, 3}, {5187, 1, 1}, {5198, 1, 3}, {5198, 1, 1}, {5205, 1, 3}, {5205, 1, 1}, {5228, 1, 3}, {5228, 1, 1},
    {5194, 1, 3}, {5194, 1, 1}, {5163, 1, 3}, {5163, 1, 1}, {5236, 1, 3}, {5236, 1, 1}, {5185, 1, 3}, {5185, 1, 1},
    {5931, 1, 1}, {5266, 1, 3}, {5268, 1, 3}, {5269, 1, 3}, {5270, 1, 3}, {5271, 1, 3}, {5272, 1, 3}, {5273, 1, 3},
    {5274, 1, 3}, {5275, 1, 3}, {5276, 1, 3}, {5277, 1, 3}, {5278, 1, 3}, {5279, 1, 3}, {5280, 1, 3}, {5281, 1, 3},
    {5282, 1, 3}, {5283, 1, 3}, {5284, 1, 3}, {5285, 1, 3}, {5286, 1, 3}, {5287, 1, 3}, {5289, 1, 3}, {5290, 1, 3},
    {5291, 1, 3}, {5292, 1, 3}, {5293, 1, 3}, {5294, 1, 3}, {5295, 1, 3}, {5296, 1, 3}, {5297, 1, 3}, {5298, 1, 3},
    {5299, 1, 3}, {5300, 1, 3}, {5301, 1, 3}, {5302, 1, 3}, {5303, 1, 3}, {5304, 1, 3}, {5305, 1, 3}, {5932, 1, 1},
    {5933, 1, 1}, {5306, 1, 1}, {193, 1, 1}, {194, 1, 1}, {41, 1, 1}, {27, 1, 1}, {44, 1, 1}, {195, 1, 1},
    {5267, 1, 1}, {5266, 1, 1}, {5268, 1, 1}, {5269, 1, 1}, {5270, 1, 1}, {5271, 1, 1}, {5272, 1, 1}, {5273, 1, 1},
    {5274, 1, 1}, {5275, 1, 1}, {5276, 1, 1}, {5277, 1, 1}, {5278, 1, 1}, {5279, 1, 1}, {5280, 1, 1}, {5281, 1, 1},
    {5282, 1, 1}, {5283, 1, 1}, {5284, 1, 1}, {5285, 1, 1}, {5286, 1, 1}, {5287, 1, 1}, {5289, 1, 1}, {5290, 1, 1},
    {5291, 1, 1}, {5292, 1, 1}, {5293, 1, 1}, {5294, 1, 1}, {5295, 1, 1}, {5296, 1, 1}, {5297, 1, 1}, {5298, 1, 1},
    {5299, 1, 1}, {5300, 1, 1}, {5301, 1, 1}, {5302, 1, 1}, {5303, 1, 1}, {5304, 1, 1}, {5305, 1, 1}, {5271, 1, 2},
    {5301, 1, 2}, {5288, 1, 1}, {37, 1, 1}, {15, 1, 1}, {5934, 1, 1}, {5935, 1, 1}, {271, 1, 1}, {272, 1, 1},
    {4827, 1, 1}, {5936, 1, 1}, {1, 1, 13}, {0, 53, 1}, {1, 1, 14}, {0, 53, 1}, {1, 1, 15}, {0, 53, 1},
    {1, 1, 16}, {0, 53, 1}, {1, 1, 17}, {0, 53, 1}, {1, 1, 18}, {0, 53, 1}, {1, 1, 19}, {0, 53, 1},
    {1, 1, 20}, {0, 53, 1}, {1, 1, 21}, {0, 53, 1}, {1, 1, 22}, {0, 53, 1}, {1, 1, 22}, {0, 53, 1},
    {1, 1, 23}, {0, 53, 1}, {1, 1, 26}, {0, 53, 1}, {196, 1, 1}, {1, 1, 27}, {0, 53, 1}, {197, 1, 1},
    {1, 1, 25}, {0, 53, 1}, {1, 1, 24}, {0, 53, 1}, {198, 1, 1}, {199, 1, 1}, {1, 1, 21}, {0, 53, 1},
    {5937, 1, 1}, {5938, 1, 1}, {5939, 1, 1}, {5940, 1, 1}, {5941, 1, 1}, {5942, 1, 1}, {5943, 1, 1}, {5944, 1, 1},
    {5307, 1, 1}, {5308, 1, 1}, {5309, 1, 1}, {5310, 1, 1}, {5311, 1, 1}, {5312, 1, 1}, {5315, 1, 1}, {5316, 1, 1},
    {5317, 1, 1}, {5318, 1, 1}, {5321, 1, 3}, {5321, 1, 1}, {5322, 1, 1}, {5323, 1, 3}, {5323, 1, 1}, {5324, 1, 3},
    {5324, 1, 1}, {5325, 1, 1}, {5326, 1, 1}, {5327, 1, 3}, {5327, 1, 1}, {5328, 1, 3}, {5328, 1, 1}, {5329, 1, 1},
    {5330, 1, 1}, {5331, 1, 1}, {5332, 1, 1}, {5945, 1, 1}, {5946, 1, 1}, {5947, 1, 1}, {5948, 1, 1}, {5319, 1, 1},
    {5313, 1, 1}, {5314, 1, 1}, {5320, 1, 1}, {63, 1, 9}, {0, 66, 1}, {66, 1, 13}, {0, 66, 1}, {5949, 1, 1},
    {5950, 1, 1}, {5951, 1, 1}, {5952, 1, 1}, {5953, 1, 1}, {5954, 1, 1}, {5955, 1, 1}, {5956, 1, 1}, {5957, 1, 1},
    {5958, 1, 1}, {5959, 1, 1}, {449, 1, 1}, {451, 1, 1}, {273, 1, 1}, {170, 1, 1}, {172, 1, 1}, {4828, 1, 1},
    {28, 1, 1}, {29, 1, 1}, {274, 1, 1}, {275, 1, 1}, {34, 1, 1}, {53, 1, 1}, {38, 1, 1}, {45, 1, 1},
    {5472, 1, 1}, {5333, 1, 1}, {5341, 1, 1}, {1, 1, 36}, {0, 53, 1}, {5341, 1, 1}, {1, 1, 37}, {0, 53, 1},
    {5452, 1, 1}, {1, 1, 37}, {0, 53, 1}, {5341, 1, 1}, {1, 1, 38}, {0, 53, 1}, {5463, 1, 1}, {1, 1, 37},
    {0, 53, 1}, {5341, 1, 1}, {5343, 1, 1}, {5347, 1, 1}, {5348, 1, 1}, {5349, 1, 1}, {5355, 1, 1}, {5361, 1, 1},
    {5362, 1, 1}, {5366, 1, 1}, {5367, 1, 1}, {5378, 1, 1}, {5379, 1, 1}, {5390, 1, 1}, {5391, 1, 1}, {5396, 1, 1},
    {5397, 1, 1}, {5401, 1, 1}, {5402, 1, 1}, {5404, 1, 1}, {5405, 1, 1}, {5432, 1, 1}, {5433, 1, 1}, {5469, 1, 1},
    {5470, 1, 1}, {5471, 1, 1}, {5408, 1, 1}, {5416, 1, 1}, {5419, 1, 1}, {5434, 1, 1}, {5439, 1, 1}, {5440, 1, 1},
    {5446, 1, 1}, {5452, 1, 1}, {5462, 1, 1}, {5463, 1, 1}, {1, 1, 28}, {0, 53, 1}, {1, 1, 29}, {0, 53, 1},
    {1, 1, 30}, {0, 53, 1}, {1, 1, 31}, {0, 53, 1}, {1, 1, 32}, {0, 53, 1}, {1, 1, 33}, {0, 53, 1},
    {1, 1, 34}, {0, 53, 1}, {1, 1, 35}, {0, 53, 1}, {1, 1, 36}, {0, 53, 1}, {1, 1, 37}, {0, 53, 1},
    {1, 1, 38}, {0, 53, 1}, {1, 1, 40}, {0, 53, 1}, {1, 1, 41}, {0, 53, 1}, {1, 1, 42}, {0, 53, 1},
    {1, 1, 43}, {0, 53, 1}, {1, 1, 44}, {0, 53, 1}, {1, 1, 45}, {0, 53, 1}, {1, 1, 46}, {0, 53, 1},
    {1, 1, 47}, {0, 53, 1}, {1, 1, 48}, {0, 53, 1}, {1, 1, 39}, {0, 53, 1}, {4861, 1, 1}, {4862, 1, 1},
    {4874, 1, 1}, {4879, 1, 1}, {4881, 1, 1}, {4883, 1, 1}, {4885, 1, 1}, {4887, 1, 1}, {4889, 1, 1}, {4891, 1, 1},
    {168, 1, 1}, {30, 1, 1}, {31, 1, 1}, {161, 1, 1}, {5342, 1, 1}, {5415, 1, 1}, {1, 1, 49}, {0, 53, 1},
    {5339, 1, 1}, {5338, 1, 1}, {5340, 1, 1}, {5333, 1, 3}, {5334, 1, 1}, {5335, 1, 1}, {5336, 1, 1}, {5337, 1, 1},
    {5350, 1, 1}, {5351, 1, 1}, {5344, 1, 1}, {5352, 1, 1}, {5353, 1, 1}, {5345, 1, 1}, {5354, 1, 1}, {5346, 1, 1},
    {5363, 1, 1}, {5364, 1, 1}, {5356, 1, 1}, {5357, 1, 1}, {5365, 1, 1}, {5358, 1, 1}, {5360, 1, 1}, {5368, 1, 1},
    {5369, 1, 1}, {5370, 1, 1}, {5371, 1, 1}, {5372, 1, 1}, {5373, 1, 1}, {5374, 1, 1}, {5375, 1, 1}, {5376, 1, 1},
    {5380, 1, 1}, {5381, 1, 1}, {5382, 1, 1}, {5383, 1, 1}, {5384, 1, 1}, {5385, 1, 1}, {5386, 1, 1}, {5387, 1, 1},
    {5388, 1, 1}, {5392, 1, 1}, {5393, 1, 1}, {5394, 1, 1}, {5398, 1, 1}, {5399, 1, 1}, {5403, 1, 1}, {5406, 1, 1},
    {5409, 1, 1}, {5410, 1, 1}, {5411, 1, 1}, {5412, 1, 1}, {5413, 1, 1}, {5414, 1, 1}, {5417, 1, 1}, {5418, 1, 1},
    {5420, 1, 1}, {5421, 1, 1}, {5422, 1, 1}, {5423, 1, 1}, {5424, 1, 1}, {5425, 1, 1}, {5426, 1, 1}, {5427, 1, 1},
    {5428, 1, 1}, {5429, 1, 1}, {5430, 1, 1}, {5431, 1, 1}, {5435, 1, 1}, {5436, 1, 1}, {5437, 1, 1}, {5438, 1, 1},
    {5445, 1, 1}, {5441, 1, 1}, {5442, 1, 1}, {5443, 1, 1}, {5444, 1, 1}, {5447, 1, 1}, {5359, 1, 1}, {5451, 1, 1},
    {1, 1, 37}, {0, 53, 1}, {5448, 1, 1}, {5448, 1, 1}, {1, 1, 37}, {0, 53, 1}, {5449, 1, 1}, {5453, 1, 1},
    {5454, 1, 1}, {5455, 1, 1}, {5456, 1, 1}, {5457, 1, 1}, {5458, 1, 1}, {5459, 1, 1}, {5460, 1, 1}, {5464, 1, 1},
    {5465, 1, 1}, {5466, 1, 1}, {5461, 1, 1}, {5467, 1, 1}, {5468, 1, 1}, {5473, 1, 1}, {5473, 1, 1}, {1, 1, 37},
    {0, 53, 1}, {48, 1, 1}, {5451, 1, 1}, {276, 1, 1}, {5452, 1, 3}, {5463, 1, 3}, {277, 1, 1}, {5377, 1, 1},
    {5389, 1, 1}, {4861, 1, 1}, {4862, 1, 1}, {4874, 1, 1}, {4879, 1, 1}, {4881, 1, 1}, {4883, 1, 1}, {4885, 1, 1},
    {4887, 1, 1}, {4889, 1, 1}, {4891, 1, 1}, {5395, 1, 1}, {5400, 1, 1}, {5407, 1, 1}, {5333, 1, 1}, {0, 66, 1},
    {5439, 1, 1}, {0, 66, 1}, {5450, 1, 1}, {1, 1, 51}, {0, 53, 1}, {1, 1, 51}, {0, 53, 1}, {1, 1, 52},
    {0, 53, 1}, {1, 1, 53}, {0, 53, 1}, {5476, 1, 1}, {5477, 1, 1}, {5478, 1, 1}, {5484, 1, 1}, {5485, 1, 1},
    {5486, 1, 1}, {5487, 1, 1}, {5488, 1, 1}, {5490, 1, 1}, {5492, 1, 1}, {5493, 1, 1}, {5494, 1, 1}, {5495, 1, 1},
    {5496, 1, 1}, {5497, 1, 1}, {5498, 1, 1}, {5499, 1, 1}, {5500, 1, 1}, {5501, 1, 1}, {5502, 1, 1}, {5504, 1, 1},
    {5505, 1, 1}, {5506, 1, 1}, {5507, 1, 1}, {5508, 1, 1}, {5511, 1, 1}, {5512, 1, 1}, {5513, 1, 1}, {5514, 1, 1},
    {5516, 1, 1}, {5518, 1, 1}, {5519, 1, 1}, {5520, 1, 1}, {5521, 1, 1}, {5522, 1, 1}, {5523, 1, 1}, {5524, 1, 1},
    {5524, 1, 1}, {1, 1, 50}, {0, 53, 1}, {5525, 1, 1}, {5526, 1, 1}, {5527, 1, 1}, {5529, 1, 1}, {5530, 1, 1},
    {5531, 1, 1}, {5533, 1, 1}, {5533, 1, 1}, {1, 1, 50}, {0, 53, 1}, {5534, 1, 1}, {5535, 1, 1}, {5535, 1, 1},
    {1, 1, 50}, {0, 53, 1}, {5536, 1, 1}, {5537, 1, 1}, {5538, 1, 1}, {5539, 1, 1}, {5540, 1, 1}, {5544, 1, 1},
    {5545, 1, 1}, {1, 1, 50}, {0, 53, 1}, {5541, 1, 1}, {5543, 1, 1}, {5549, 1, 1}, {5550, 1, 1}, {5551, 1, 1},
    {5552, 1, 1}, {5553, 1, 1}, {5554, 1, 1}, {5557, 1, 1}, {5559, 1, 1}, {5560, 1, 1}, {5562, 1, 1}, {5563, 1, 1},
    {5564, 1, 1}, {5565, 1, 1}, {5566, 1, 1}, {5567, 1, 1}, {5561, 1, 1}, {5546, 1, 1}, {5474, 1, 1}, {1, 1, 5},
    {0, 17, 1}, {1, 1, 3}, {0, 17, 1}, {5558, 1, 1}, {5547, 1, 1}, {5548, 1, 1}, {5500, 1, 1}, {1, 1, 50},
    {0, 53, 1}, {5501, 1, 1}, {1, 1, 50}, {0, 53, 1}, {5502, 1, 1}, {1, 1, 50}, {0, 53, 1}, {5508, 1, 1},
    {1, 1, 50}, {0, 53, 1}, {5516, 1, 1}, {1, 1, 50}, {0, 53, 1}, {5518, 1, 1}, {1, 1, 50}, {0, 53, 1},
    {5526, 1, 1}, {1, 1, 50}, {0, 53, 1}, {5531, 1, 1}, {1, 1, 50}, {0, 53, 1}, {5489, 1, 1}, {5491, 1, 1},
    {5555, 1, 1}, {5556, 1, 1}, {51, 1, 1}, {52, 1, 1}, {4861, 1, 1}, {4862, 1, 1}, {4874, 1, 1}, {4879, 1, 1},
    {4881, 1, 1}, {4883, 1, 1}, {4885, 1, 1}, {4887, 1, 1}, {4889, 1, 1}, {4891, 1, 1}, {200, 1, 1}, {4813, 1, 1},
    {5475, 1, 1}, {5479, 1, 1}, {5480, 1, 1}, {5481, 1, 1}, {5482, 1, 1}, {5483, 1, 1}, {5515, 1, 1}, {5509, 1, 1},
    {5532, 1, 1}, {5503, 1, 1}, {5510, 1, 1}, {5542, 1, 1}, {5517, 1, 1}, {5528, 1, 1}, {5568, 1, 1}, {5569, 1, 1},
    {5570, 1, 1}, {5571, 1, 1}, {5572, 1, 1}, {5573, 1, 1}, {5574, 1, 1}, {5575, 1, 1}, {5576, 1, 1}, {5577, 1, 1},
    {5578, 1, 1}, {5579, 1, 1}, {5580, 1, 1}, {5581, 1, 1}, {5582, 1, 1}, {5583, 1, 1}, {5584, 1, 1}, {5585, 1, 1},
    {5586, 1, 1}, {5587, 1, 1}, {5588, 1, 1}, {5589, 1, 1}, {5590, 1, 1}, {5591, 1, 1}, {5592, 1, 1}, {5593, 1, 1},
    {5594, 1, 1}, {5595, 1, 1}, {5596, 1, 1}, {5597, 1, 1}, {5598, 1, 1}, {5599, 1, 1}, {5600, 1, 1}, {5601, 1, 1},
    {5602, 1, 1}, {5603, 1, 1}, {5604, 1, 1}, {5605, 1, 1}, {5606, 1, 1}, {5607, 1, 1}, {5608, 1, 1}, {5609, 1, 1},
    {5610, 1, 1}, {5611, 1, 1}, {5612, 1, 1}, {5613, 1, 1}, {5614, 1, 1}, {5615, 1, 1}, {5616, 1, 1}, {5617, 1, 1},
    {5618, 1, 1}, {5619, 1, 1}, {5620, 1, 1}, {5621, 1, 1}, {5622, 1, 1}, {5623, 1, 1}, {5624, 1, 1}, {5625, 1, 1},
    {5626, 1, 1}, {5627, 1, 1}, {5628, 1, 1}, {5629, 1, 1}, {5630, 1, 1}, {5631, 1, 1}, {5632, 1, 1}, {5633, 1, 1},
    {5634, 1, 1}, {5635, 1, 1}, {5636, 1, 1}, {5637, 1, 1}, {5638, 1, 1}, {5639, 1, 1}, {5640, 1, 1}, {5641, 1, 1},
    {5642, 1, 1}, {5643, 1, 1}, {5644, 1, 1}, {5645, 1, 1}, {5646, 1, 1}, {5647, 1, 1}, {5648, 1, 1}, {5649, 1, 1},
    {5650, 1, 1}, {5651, 1, 1}, {5652, 1, 1}, {5653, 1, 1}, {5654, 1, 1}, {5655, 1, 1}, {5656, 1, 1}, {5657, 1, 1},
    {5658, 1, 1}, {5659, 1, 1}, {5660, 1, 1}, {5661, 1, 1}, {5662, 1, 1}, {5663, 1, 1}, {5664, 1, 1}, {5665, 1, 1},
    {5666, 1, 1}, {5667, 1, 1}, {5668, 1, 1}, {5669, 1, 1}, {5670, 1, 1}, {5671, 1, 1}, {5672, 1, 1}, {5673, 1, 1},
    {5674, 1, 1}, {5675, 1, 1}, {5676, 1, 1}, {5677, 1, 1}, {5678, 1, 1}, {5679, 1, 1}, {5680, 1, 1}, {5681, 1, 1},
    {5682, 1, 1}, {5683, 1, 1}, {5684, 1, 1}, {5685, 1, 1}, {5686, 1, 1}, {5687, 1, 1}, {5688, 1, 1}, {5689, 1, 1},
    {5690, 1, 1}, {5691, 1, 1}, {5692, 1, 1}, {5693, 1, 1}, {5694, 1, 1}, {5695, 1, 1}, {5696, 1, 1}, {5697, 1, 1},
    {5698, 1, 1}, {5699, 1, 1}, {5700, 1, 1}, {5701, 1, 1}, {5702, 1, 1}, {5703, 1, 1}, {5704, 1, 1}, {5705, 1, 1},
    {5706, 1, 1}, {5707, 1, 1}, {5708, 1, 1}, {5709, 1, 1}, {5710, 1, 1}, {5711, 1, 1}, {5712, 1, 1}, {5713, 1, 1},
    {5714, 1, 1}, {5715, 1, 1}, {5716, 1, 1}, {5717, 1, 1}, {5718, 1, 1}, {5719, 1, 1}, {5720, 1, 1}, {5721, 1, 1},
    {5722, 1, 1}, {5723, 1, 1}, {5724, 1, 1}, {5725, 1, 1}, {5726, 1, 1}, {5727, 1, 1}, {5728, 1, 1}, {5729, 1, 1},
    {5730, 1, 1}, {5731, 1, 1}, {5732, 1, 1}, {5733, 1, 1}, {5734, 1, 1}, {5735, 1, 1}, {5736, 1, 1}, {5737, 1, 1},
    {5738, 1, 1}, {5739, 1, 1}, {5740, 1, 1}, {5741, 1, 1}, {5742, 1, 1}, {5743, 1, 1}, {5744, 1, 1}, {5745, 1, 1},
    {5746, 1, 1}, {5747, 1, 1}, {5748, 1, 1}, {5749, 1, 1}, {5750, 1, 1}, {5751, 1, 1}, {5752, 1, 1}, {5753, 1, 1},
    {5754, 1, 1}, {5755, 1, 1}, {5756, 1, 1}, {5757, 1, 1}, {5758, 1, 1}, {5759, 1, 1}, {5760, 1, 1}, {5761, 1, 1},
    {5762, 1, 1}, {5763, 1, 1}, {5764, 1, 1}, {5765, 1, 1}, {5766, 1, 1}, {5767, 1, 1}, {5768, 1, 1}, {5769, 1, 1},
    {5770, 1, 1}, {5771, 1, 1}, {5772, 1, 1}, {5773, 1, 1}, {5774, 1, 1}, {5775, 1, 1}, {5776, 1, 1}, {5777, 1, 1},
    {5778, 1, 1}, {5779, 1, 1}, {5780, 1, 1}, {5781, 1, 1}, {5782, 1, 1}, {5783, 1, 1}, {5784, 1, 1}, {5785, 1, 1},
    {5786, 1, 1}, {5787, 1, 1}, {5788, 1, 1}, {5789, 1, 1}, {5790, 1, 1}, {5791, 1, 1}, {5792, 1, 1}, {5793, 1, 1},
    {5794, 1, 1}, {5795, 1, 1}, {5796, 1, 1}, {5797, 1, 1}, {5798, 1, 1}, {5799, 1, 1}, {5800, 1, 1}, {5801, 1, 1},
    {5802, 1, 1}, {5803, 1, 1}, {5804, 1, 1}, {5805, 1, 1}, {5806, 1, 1}, {5807, 1, 1}, {5808, 1, 1}, {5809, 1, 1},
    {5810, 1, 1}, {5811, 1, 1}, {5812, 1, 1}, {5813, 1, 1}, {5814, 1, 1}, {5815, 1, 1}, {5816, 1, 1}, {5817, 1, 1},
    {5818, 1, 1}, {5819, 1, 1}, {5820, 1, 1}, {5821, 1, 1}, {5822, 1, 1}, {5823, 1, 1}, {4898, 1, 7}, {0, 39, 1},
    {4898, 1, 1}, {0, 39, 1}, {4904, 1, 5}, {0, 16, 1}, {4904, 1, 1}, {0, 16, 1}, {4904, 1, 5}, {0, 37, 1},
    {4904, 1, 1}, {0, 37, 1}, {4904, 1, 5}, {0, 44, 1}, {4904, 1, 1}, {0, 44, 1}, {4909, 1, 7}, {0, 19, 1},
    {0, 6, 1}, {4909, 1, 1}, {0, 19, 1}, {0, 6, 1}, {4915, 1, 9}, {0, 16, 1}, {4915, 1, 1}, {0, 16, 1},
    {4915, 1, 9}, {0, 37, 1}, {4915, 1, 1}, {0, 37, 1}, {4915, 1, 9}, {0, 44, 1}, {4915, 1, 1}, {0, 44, 1},
    {4915, 1, 9}, {0, 19, 1}, {4915, 1, 1}, {0, 19, 1}, {4915, 1, 9}, {0, 41, 1}, {4915, 1, 1}, {0, 41, 1},
    {4923, 1, 9}, {0, 21, 1}, {0, 7, 1}, {4923, 1, 1}, {0, 21, 1}, {0, 7, 1}, {4923, 1, 9}, {0, 21, 1},
    {0, 6, 1}, {4923, 1, 1}, {0, 21, 1}, {0, 6, 1}, {4923, 1, 9}, {0, 41, 1}, {4923, 1, 1}, {0, 41, 1},
    {4923, 1, 9}, {0, 43, 1}, {4923, 1, 1}, {0, 43, 1}, {4923, 1, 9}, {0, 19, 1}, {0, 8, 1}, {4923, 1, 1},
    {0, 19, 1}, {0, 8, 1}, {4935, 1, 5}, {0, 16, 1}, {4935, 1, 1}, {0, 16, 1}, {4940, 1, 7}, {0, 21, 1},
    {4940, 1, 1}, {0, 21, 1}, {4948, 1, 9}, {0, 16, 1}, {4948, 1, 1}, {0, 16, 1}, {4948, 1, 9}, {0, 37, 1},
    {4948, 1, 1}, {0, 37, 1}, {4948, 1, 9}, {0, 13, 1}, {4948, 1, 1}, {0, 13, 1}, {4948, 1, 9}, {0, 19, 1},
    {4948, 1, 1}, {0, 19, 1}, {4948, 1, 9}, {0, 42, 1}, {4948, 1, 1}, {0, 42, 1}, {4957, 1, 9}, {0, 43, 1},
    {4957, 1, 1}, {0, 43, 1}, {4957, 1, 9}, {0, 13, 1}, {0, 6, 1}, {4957, 1, 1}, {0, 13, 1}, {0, 6, 1},
    {4968, 1, 5}, {0, 6, 1}, {4968, 1, 1}, {0, 6, 1}, {4968, 1, 5}, {0, 37, 1}, {4968, 1, 1}, {0, 37, 1},
    {4968, 1, 5}, {0, 44, 1}, {4968, 1, 1}, {0, 44, 1}, {4971, 1, 9}, {0, 37, 1}, {4971, 1, 1}, {0, 37, 1},
    {4971, 1, 9}, {0, 37, 1}, {0, 21, 1}, {4971, 1, 1}, {0, 37, 1}, {0, 21, 1}, {4971, 1, 9}, {0, 44, 1},
    {4971, 1, 1}, {0, 44, 1}, {4971, 1, 9}, {0, 41, 1}, {4971, 1, 1}, {0, 41, 1}, {4982, 1, 7}, {0, 6, 1},
    {4982, 1, 1}, {0, 6, 1}, {4982, 1, 7}, {0, 16, 1}, {4982, 1, 1}, {0, 16, 1}, {4982, 1, 7}, {0, 37, 1},
    {4982, 1, 1}, {0, 37, 1}, {4984, 1, 5}, {0, 16, 1}, {4984, 1, 1}, {0, 16, 1}, {4984, 1, 5}, {0, 37, 1},
    {4984, 1, 1}, {0, 37, 1}, {4984, 1, 5}, {0, 44, 1}, {4984, 1, 1}, {0, 44, 1}, {4984, 1, 5}, {0, 41, 1},
    {4984, 1, 1}, {0, 41, 1}, {4992, 1, 9}, {0, 15, 1}, {0, 6, 1}, {4992, 1, 1}, {0, 15, 1}, {0, 6, 1},
    {4992, 1, 9}, {0, 15, 1}, {0, 13, 1}, {4992, 1, 1}, {0, 15, 1}, {0, 13, 1}, {4992, 1, 9}, {0, 21, 1},
    {0, 7, 1}, {4992, 1, 1}, {0, 21, 1}, {0, 7, 1}, {4992, 1, 9}, {0, 21, 1}, {0, 6, 1}, {4992, 1, 1},
    {0, 21, 1}, {0, 6, 1}, {4998, 1, 5}, {0, 6, 1}, {4998, 1, 1}, {0, 6, 1}, {4998, 1, 5}, {0, 16, 1},
    {4998, 1, 1}, {0, 16, 1}, {5006, 1, 7}, {0, 16, 1}, {5006, 1, 1}, {0, 16, 1}, {5006, 1, 7}, {0, 37, 1},
    {5006, 1, 1}, {0, 37, 1}, {5006, 1, 7}, {0, 37, 1}, {0, 21, 1}, {5006, 1, 1}, {0, 37, 1}, {0, 21, 1},
    {5006, 1, 7}, {0, 44, 1}, {5006, 1, 1}, {0, 44, 1}, {5017, 1, 5}, {0, 16, 1}, {5017, 1, 1}, {0, 16, 1},
    {5017, 1, 5}, {0, 37, 1}, {5017, 1, 1}, {0, 37, 1}, {5017, 1, 5}, {0, 6, 1}, {0, 16, 1}, {5017, 1, 1},
    {0, 6, 1}, {0, 16, 1}, {5017, 1, 5}, {0, 10, 1}, {0, 16, 1}, {5017, 1, 1}, {0, 10, 1}, {0, 16, 1},
    {5017, 1, 5}, {0, 37, 1}, {0, 16, 1}, {5017, 1, 1}, {0, 37, 1}, {0, 16, 1}, {5028, 1, 7}, {0, 16, 1},
    {5028, 1, 1}, {0, 16, 1}, {5028, 1, 7}, {0, 37, 1}, {5028, 1, 1}, {0, 37, 1}, {5028, 1, 7}, {0, 44, 1},
    {5028, 1, 1}, {0, 44, 1}, {5028, 1, 7}, {0, 41, 1}, {5028, 1, 1}, {0, 41, 1}, {5038, 1, 7}, {0, 38, 1},
    {5038, 1, 1}, {0, 38, 1}, {5038, 1, 7}, {0, 43, 1}, {5038, 1, 1}, {0, 43, 1}, {5038, 1, 7}, {0, 41, 1},
    {5038, 1, 1}, {0, 41, 1}, {5038, 1, 7}, {0, 15, 1}, {0, 6, 1}, {5038, 1, 1}, {0, 15, 1}, {0, 6, 1},
    {5038, 1, 7}, {0, 21, 1}, {0, 13, 1}, {5038, 1, 1}, {0, 21, 1}, {0, 13, 1}, {5048, 1, 7}, {0, 15, 1},
    {5048, 1, 1}, {0, 15, 1}, {5048, 1, 7}, {0, 37, 1}, {5048, 1, 1}, {0, 37, 1}, {5053, 1, 5}, {0, 7, 1},
    {5053, 1, 1}, {0, 7, 1}, {5053, 1, 5}, {0, 6, 1}, {5053, 1, 1}, {0, 6, 1}, {5053, 1, 5}, {0, 13, 1},
    {5053, 1, 1}, {0, 13, 1}, {5053, 1, 5}, {0, 16, 1}, {5053, 1, 1}, {0, 16, 1}, {5053, 1, 5}, {0, 37, 1},
    {5053, 1, 1}, {0, 37, 1}, {5055, 1, 7}, {0, 16, 1}, {5055, 1, 1}, {0, 16, 1}, {5055, 1, 7}, {0, 13, 1},
    {5055, 1, 1}, {0, 13, 1}, {5056, 1, 5}, {0, 16, 1}, {5056, 1, 1}, {0, 16, 1}, {5062, 1, 5}, {0, 9, 1},
    {5062, 1, 1}, {0, 9, 1}, {5062, 1, 5}, {0, 37, 1}, {5062, 1, 1}, {0, 37, 1}, {5062, 1, 5}, {0, 44, 1},
    {5062, 1, 1}, {0, 44, 1}, {4948, 1, 1}, {0, 44, 1}, {5028, 1, 1}, {0, 13, 1}, {5053, 1, 1}, {0, 11, 1},
    {5056, 1, 1}, {0, 11, 1}, {4898, 1, 2}, {5083, 1, 2}, {5017, 1, 1}, {0, 66, 1}, {0, 16, 1}, {5022, 1, 1},
    {5023, 1, 1}, {5017, 1, 2}, {5017, 1, 2}, {4922, 1, 1}, {4898, 1, 7}, {0, 37, 1}, {4898, 1, 1}, {0, 37, 1},
    {4898, 1, 7}, {0, 30, 1}, {4898, 1, 1}, {0, 30, 1}, {4898, 1, 7}, {0, 9, 1}, {0, 6, 1}, {4898, 1, 1},
    {0, 9, 1}, {0, 6, 1}, {4898, 1, 7}, {0, 9, 1}, {0, 7, 1}, {4898, 1, 1}, {0, 9, 1}, {0, 7, 1},
    {4898, 1, 7}, {0, 9, 1}, {0, 30, 1}, {4898, 1, 1}, {0, 9, 1}, {0, 30, 1}, {4898, 1, 7}, {0, 9, 1},
    {0, 15, 1}, {4898, 1, 1}, {0, 9, 1}, {0, 15, 1}, {4898, 1, 7}, {0, 37, 1}, {0, 9, 1}, {4898, 1, 1},
    {0, 37, 1}, {0, 9, 1}, {4898, 1, 7}, {0, 8, 1}, {0, 6, 1}, {4898, 1, 1}, {0, 8, 1}, {0, 6, 1},
    {4898, 1, 7}, {0, 8, 1}, {0, 7, 1}, {4898, 1, 1}, {0, 8, 1}, {0, 7, 1}, {4898, 1, 7}, {0, 8, 1},
    {0, 30, 1}, {4898, 1, 1}, {0, 8, 1}, {0, 30, 1}, {4898, 1, 7}, {0, 8, 1}, {0, 15, 1}, {4898, 1, 1},
    {0, 8, 1}, {0, 15, 1}, {4898, 1, 7}, {0, 37, 1}, {0, 8, 1}, {4898, 1, 1}, {0, 37, 1}, {0, 8, 1},
    {4923, 1, 9}, {0, 37, 1}, {4923, 1, 1}, {0, 37, 1}, {4923, 1, 9}, {0, 30, 1}, {4923, 1, 1}, {0, 30, 1},
    {4923, 1, 9}, {0, 15, 1}, {4923, 1, 1}, {0, 15, 1}, {4923, 1, 9}, {0, 9, 1}, {0, 6, 1}, {4923, 1, 1},
    {0, 9, 1}, {0, 6, 1}, {4923, 1, 9}, {0, 9, 1}, {0, 7, 1}, {4923, 1, 1}, {0, 9, 1}, {0, 7, 1},
    {4923, 1, 9}, {0, 9, 1}, {0, 30, 1}, {4923, 1, 1}, {0, 9, 1}, {0, 30, 1}, {4923, 1, 9}, {0, 9, 1},
    {0, 15, 1}, {4923, 1, 1}, {0, 9, 1}, {0, 15, 1}, {4923, 1, 9}, {0, 37, 1}, {0, 9, 1}, {4923, 1, 1},
    {0, 37, 1}, {0, 9, 1}, {4957, 1, 9}, {0, 30, 1}, {4957, 1, 1}, {0, 30, 1}, {4957, 1, 9}, {0, 37, 1},
    {4957, 1, 1}, {0, 37, 1}, {4992, 1, 9}, {0, 37, 1}, {4992, 1, 1}, {0, 37, 1}, {4992, 1, 9}, {0, 30, 1},
    {4992, 1, 1}, {0, 30, 1}, {4992, 1, 9}, {0, 9, 1}, {0, 6, 1}, {4992, 1, 1}, {0, 9, 1}, {0, 6, 1},
    {4992, 1, 9}, {0, 9, 1}, {0, 7, 1}, {4992, 1, 1}, {0, 9, 1}, {0, 7, 1}, {4992, 1, 9}, {0, 9, 1},
    {0, 30, 1}, {4992, 1, 1}, {0, 9, 1}, {0, 30, 1}, {4992, 1, 9}, {0, 9, 1}, {0, 15, 1}, {4992, 1, 1},
    {0, 9, 1}, {0, 15, 1}, {4992, 1, 9}, {0, 37, 1}, {0, 9, 1}, {4992, 1, 1}, {0, 37, 1}, {0, 9, 1},
    {4992, 1, 9}, {0, 34, 1}, {0, 6, 1}, {4992, 1, 1}, {0, 34, 1}, {0, 6, 1}, {4992, 1, 9}, {0, 34, 1},
    {0, 7, 1}, {4992, 1, 1}, {0, 34, 1}, {0, 7, 1}, {4992, 1, 9}, {0, 34, 1}, {0, 30, 1}, {4992, 1, 1},
    {0, 34, 1}, {0, 30, 1}, {4992, 1, 9}, {0, 34, 1}, {0, 15, 1}, {4992, 1, 1}, {0, 34, 1}, {0, 15, 1},
    {4992, 1, 9}, {0, 34, 1}, {0, 37, 1}, {4992, 1, 1}, {0, 34, 1}, {0, 37, 1}, {5038, 1, 7}, {0, 37, 1},
    {5038, 1, 1}, {0, 37, 1}, {5038, 1, 7}, {0, 30, 1}, {5038, 1, 1}, {0, 30, 1}, {5038, 1, 7}, {0, 34, 1},
    {0, 6, 1}, {5038, 1, 1}, {0, 34, 1}, {0, 6, 1}, {5038, 1, 7}, {0, 34, 1}, {0, 7, 1}, {5038, 1, 1},
    {0, 34, 1}, {0, 7, 1}, {5038, 1, 7}, {0, 34, 1}, {0, 30, 1}, {5038, 1, 1}, {0, 34, 1}, {0, 30, 1},
    {5038, 1, 7}, {0, 34, 1}, {0, 15, 1}, {5038, 1, 1}, {0, 34, 1}, {0, 15, 1}, {5038, 1, 7}, {0, 34, 1},
    {0, 37, 1}, {5038, 1, 1}, {0, 34, 1}, {0, 37, 1}, {5056, 1, 5}, {0, 7, 1}, {5056, 1, 1}, {0, 7, 1},
    {5056, 1, 5}, {0, 37, 1}, {5056, 1, 1}, {0, 37, 1}, {5056, 1, 5}, {0, 30, 1}, {5056, 1, 1}, {0, 30, 1},
    {5056, 1, 5}, {0, 15, 1}, {5056, 1, 1}, {0, 15, 1}, {4971, 1, 2}, {4971, 1, 2}, {4971, 1, 2}, {4971, 1, 2},
    {5051, 1, 3}, {5051, 1, 1}, {5060, 1, 3}, {5060, 1, 1}, {5098, 1, 1}, {0, 4, 1}, {5098, 1, 1}, {0, 5, 1},
    {5098, 1, 1}, {0, 4, 1}, {0, 7, 1}, {5098, 1, 1}, {0, 5, 1}, {0, 7, 1}, {5098, 1, 1}, {0, 4, 1},
    {0, 6, 1}, {5098, 1, 1}, {0, 5, 1}, {0, 6, 1}, {5098, 1, 1}, {0, 4, 1}, {0, 12, 1}, {5098, 1, 1},
    {0, 5, 1}, {0, 12, 1}, {5098, 1, 3}, {0, 4, 1}, {5098, 1, 3}, {0, 5, 1}, {5098, 1, 3}, {0, 4, 1},
    {0, 7, 1}, {5098, 1, 3}, {0, 5, 1}, {0, 7, 1}, {5098, 1, 3}, {0, 4, 1}, {0, 6, 1}, {5098, 1, 3},
    {0, 5, 1}, {0, 6, 1}, {5098, 1, 3}, {0, 4, 1}, {0, 12, 1}, {5098, 1, 3}, {0, 5, 1}, {0, 12, 1},
    {5102, 1, 1}, {0, 4, 1}, {5102, 1, 1}, {0, 5, 1}, {5102, 1, 1}, {0, 4, 1}, {0, 7, 1}, {5102, 1, 1},
    {0, 5, 1}, {0, 7, 1}, {5102, 1, 1}, {0, 4, 1}, {0, 6, 1}, {5102, 1, 1}, {0, 5, 1}, {0, 6, 1},
    {5960, 1, 1}, {5961, 1, 1}, {5102, 1, 5}, {0, 4, 1}, {5102, 1, 5}, {0, 5, 1}, {5102, 1, 5}, {0, 4, 1},
    {0, 7, 1}, {5102, 1, 5}, {0, 5, 1}, {0, 7, 1}, {5102, 1, 5}, {0, 4, 1}, {0, 6, 1}, {5102, 1, 5},
    {0, 5, 1}, {0, 6, 1}, {5962, 1, 1}, {5963, 1, 1}, {5108, 1, 1}, {0, 4, 1}, {5108, 1, 1}, {0, 5, 1},
    {5108, 1, 1}, {0, 4, 1}, {0, 7, 1}, {5108, 1, 1}, {0, 5, 1}, {0, 7, 1}, {5108, 1, 1}, {0, 4, 1},
    {0, 6, 1}, {5108, 1, 1}, {0, 5, 1}, {0, 6, 1}, {5108, 1, 1}, {0, 4, 1}, {0, 12, 1}, {5108, 1, 1},
    {0, 5, 1}, {0, 12, 1}, {5108, 1, 3}, {0, 4, 1}, {5108, 1, 3}, {0, 5, 1}, {5108, 1, 3}, {0, 4, 1},
    {0, 7, 1}, {5108, 1, 3}, {0, 5, 1}, {0, 7, 1}, {5108, 1, 3}, {0, 4, 1}, {0, 6, 1}, {5108, 1, 3},
    {0, 5, 1}, {0, 6, 1}, {5108, 1, 3}, {0, 4, 1}, {0, 12, 1}, {5108, 1, 3}, {0, 5, 1}, {0, 12, 1},
    {5110, 1, 1}, {0, 4, 1}, {5110, 1, 1}, {0, 5, 1}, {5110, 1, 1}, {0, 4, 1}, {0, 7, 1}, {5110, 1, 1},
    {0, 5, 1}, {0, 7, 1}, {5110, 1, 1}, {0, 4, 1}, {0, 6, 1}, {5110, 1, 1}, {0, 5, 1}, {0, 6, 1},
    {5110, 1, 1}, {0, 4, 1}, {0, 12, 1}, {5110, 1, 1}, {0, 5, 1}, {0, 12, 1}, {5110, 1, 5}, {0, 4, 1},
    {5110, 1, 5}, {0, 5, 1}, {5110, 1, 5}, {0, 4, 1}, {0, 7, 1}, {5110, 1, 5}, {0, 5, 1}, {0, 7, 1},
    {5110, 1, 5}, {0, 4, 1}, {0, 6, 1}, {5110, 1, 5}, {0, 5, 1}, {0, 6, 1}, {5110, 1, 5}, {0, 4, 1},
    {0, 12, 1}, {5110, 1, 5}, {0, 5, 1}, {0, 12, 1}, {5118, 1, 1}, {0, 4, 1}, {5118, 1, 1}, {0, 5, 1},
    {5118, 1, 1}, {0, 4, 1}, {0, 7, 1}, {5118, 1, 1}, {0, 5, 1}, {0, 7, 1}, {5118, 1, 1}, {0, 4, 1},
    {0, 6, 1}, {5118, 1, 1}, {0, 5, 1}, {0, 6, 1}, {5964, 1, 1}, {5965, 1, 1}, {5118, 1, 3}, {0, 4, 1},
    {5118, 1, 3}, {0, 5, 1}, {5118, 1, 3}, {0, 4, 1}, {0, 7, 1}, {5118, 1, 3}, {0, 5, 1}, {0, 7, 1},
    {5118, 1, 3}, {0, 4, 1}, {0, 6, 1}, {5118, 1, 3}, {0, 5, 1}, {0, 6, 1}, {5966, 1, 1}, {5967, 1, 1},
    {5130, 1, 1}, {0, 4, 1}, {5130, 1, 1}, {0, 5, 1}, {5130, 1, 1}, {0, 4, 1}, {0, 7, 1}, {5130, 1, 1},
    {0, 5, 1}, {0, 7, 1}, {5130, 1, 1}, {0, 4, 1}, {0, 6, 1}, {5130, 1, 1}, {0, 5, 1}, {0, 6, 1},
    {5130, 1, 1}, {0, 4, 1}, {0, 12, 1}, {5130, 1, 1}, {0, 5, 1}, {0, 12, 1}, {5968, 1, 1}, {5130, 1, 3},
    {0, 5, 1}, {5969, 1, 1}, {5130, 1, 3}, {0, 5, 1}, {0, 7, 1}, {5970, 1, 1}, {5130, 1, 3}, {0, 5, 1},
    {0, 6, 1}, {5971, 1, 1}, {5130, 1, 3}, {0, 5, 1}, {0, 12, 1}, {5134, 1, 1}, {0, 4, 1}, {5134, 1, 1},
    {0, 5, 1}, {5134, 1, 1}, {0, 4, 1}, {0, 7, 1}, {5134, 1, 1}, {0, 5, 1}, {0, 7, 1}, {5134, 1, 1},
    {0, 4, 1}, {0, 6, 1}, {5134, 1, 1}, {0, 5, 1}, {0, 6, 1}, {5134, 1, 1}, {0, 4, 1}, {0, 12, 1},
    {5134, 1, 1}, {0, 5, 1}, {0, 12, 1}, {5134, 1, 3}, {0, 4, 1}, {5134, 1, 3}, {0, 5, 1}, {5134, 1, 3},
    {0, 4, 1}, {0, 7, 1}, {5134, 1, 3}, {0, 5, 1}, {0, 7, 1}, {5134, 1, 3}, {0, 4, 1}, {0, 6, 1},
    {5134, 1, 3}, {0, 5, 1}, {0, 6, 1}, {5134, 1, 3}, {0, 4, 1}, {0, 12, 1}, {5134, 1, 3}, {0, 5, 1},
    {0, 12, 1}, {5098, 1, 1}, {0, 7, 1}, {5098, 1, 1}, {0, 6, 1}, {5102, 1, 1}, {0, 7, 1}, {5102, 1, 1},
    {0, 6, 1}, {5108, 1, 1}, {0, 7, 1}, {5108, 1, 1}, {0, 6, 1}, {5110, 1, 1}, {0, 7, 1}, {5110, 1, 1},
    {0, 6, 1}, {5118, 1, 1}, {0, 7, 1}, {5118, 1, 1}, {0, 6, 1}, {5130, 1, 1}, {0, 7, 1}, {5130, 1, 1},
    {0, 6, 1}, {5134, 1, 1}, {0, 7, 1}, {5134, 1, 1}, {0, 6, 1}, {5972, 1, 1}, {5973, 1, 1}, {5098, 1, 1},
    {0, 4, 1}, {0, 47, 1}, {5098, 1, 1}, {0, 5, 1}, {0, 47, 1}, {5098, 1, 1}, {0, 4, 1}, {0, 7, 1},
    {0, 47, 1}, {5098, 1, 1}, {0, 5, 1}, {0, 7, 1}, {0, 47, 1}, {5098, 1, 1}, {0, 4, 1}, {0, 6, 1},
    {0, 47, 1}, {5098, 1, 1}, {0, 5, 1}, {0, 6, 1}, {0, 47, 1}, {5098, 1, 1}, {0, 4, 1}, {0, 12, 1},
    {0, 47, 1}, {5098, 1, 1}, {0, 5, 1}, {0, 12, 1}, {0, 47, 1}, {5098, 1, 3}, {0, 4, 1}, {0, 47, 1},
    {5098, 1, 3}, {0, 5, 1}, {0, 47, 1}, {5098, 1, 3}, {0, 4, 1}, {0, 7, 1}, {0, 47, 1}, {5098, 1, 3},
    {0, 5, 1}, {0, 7, 1}, {0, 47, 1}, {5098, 1, 3}, {0, 4, 1}, {0, 6, 1}, {0, 47, 1}, {5098, 1, 3},
    {0, 5, 1}, {0, 6, 1}, {0, 47, 1}, {5098, 1, 3}, {0, 4, 1}, {0, 12, 1}, {0, 47, 1}, {5098, 1, 3},
    {0, 5, 1}, {0, 12, 1}, {0, 47, 1}, {5108, 1, 1}, {0, 4, 1}, {0, 47, 1}, {5108, 1, 1}, {0, 5, 1},
    {0, 47, 1}, {5108, 1, 1}, {0, 4, 1}, {0, 7, 1}, {0, 47, 1}, {5108, 1, 1}, {0, 5, 1}, {0, 7, 1},
    {0, 47, 1}, {5108, 1, 1}, {0, 4, 1}, {0, 6, 1}, {0, 47, 1}, {5108, 1, 1}, {0, 5, 1}, {0, 6, 1},
    {0, 47, 1}, {5108, 1, 1}, {0, 4, 1}, {0, 12, 1}, {0, 47, 1}, {5108, 1, 1}, {0, 5, 1}, {0, 12, 1},
    {0, 47, 1}, {5108, 1, 3}, {0, 4, 1}, {0, 47, 1}, {5108, 1, 3}, {0, 5, 1}, {0, 47, 1}, {5108, 1, 3},
    {0, 4, 1}, {0, 7, 1}, {0, 47, 1}, {5108, 1, 3}, {0, 5, 1}, {0, 7, 1}, {0, 47, 1}, {5108, 1, 3},
    {0, 4, 1}, {0, 6, 1}, {0, 47, 1}, {5108, 1, 3}, {0, 5, 1}, {0, 6, 1}, {0, 47, 1}, {5108, 1, 3},
    {0, 4, 1}, {0, 12, 1}, {0, 47, 1}, {5108, 1, 3}, {0, 5, 1}, {0, 12, 1}, {0, 47, 1}, {5134, 1, 1},
    {0, 4, 1}, {0, 47, 1}, {5134, 1, 1}, {0, 5, 1}, {0, 47, 1}, {5134, 1, 1}, {0, 4, 1}, {0, 7, 1},
    {0, 47, 1}, {5134, 1, 1}, {0, 5, 1}, {0, 7, 1}, {0, 47, 1}, {5134, 1, 1}, {0, 4, 1}, {0, 6, 1},
    {0, 47, 1}, {5134, 1, 1}, {0, 5, 1}, {0, 6, 1}, {0, 47, 1}, {5134, 1, 1}, {0, 4, 1}, {0, 12, 1},
    {0, 47, 1}, {5134, 1, 1}, {0, 5, 1}, {0, 12, 1}, {0, 47, 1}, {5134, 1, 3}, {0, 4, 1}, {0, 47, 1},
    {5134, 1, 3}, {0, 5, 1}, {0, 47, 1}, {5134, 1, 3}, {0, 4, 1}, {0, 7, 1}, {0, 47, 1}, {5134, 1, 3},
    {0, 5, 1}, {0, 7, 1}, {0, 47, 1}, {5134, 1, 3}, {0, 4, 1}, {0, 6, 1}, {0, 47, 1}, {5134, 1, 3},
    {0, 5, 1}, {0, 6, 1}, {0, 47, 1}, {5134, 1, 3}, {0, 4, 1}, {0, 12, 1}, {0, 47, 1}, {5134, 1, 3},
    {0, 5, 1}, {0, 12, 1}, {0, 47, 1}, {5098, 1, 1}, {0, 8, 1}, {5098, 1, 1}, {0, 21, 1}, {5098, 1, 1},
    {0, 7, 1}, {0, 47, 1}, {5098, 1, 1}, {0, 47, 1}, {5098, 1, 1}, {0, 6, 1}, {0, 47, 1}, {5974, 1, 1},
    {5098, 1, 1}, {0, 12, 1}, {5098, 1, 1}, {0, 12, 1}, {0, 47, 1}, {5098, 1, 3}, {0, 8, 1}, {5098, 1, 3},
    {0, 21, 1}, {5098, 1, 3}, {0, 7, 1}, {5098, 1, 3}, {0, 6, 1}, {5098, 1, 3}, {0, 47, 1}, {213, 1, 1},
    {5110, 1, 1}, {213, 1, 1}, {215, 1, 1}, {208, 1, 1}, {0, 12, 1}, {5108, 1, 1}, {0, 7, 1}, {0, 47, 1},
    {5108, 1, 1}, {0, 47, 1}, {5108, 1, 1}, {0, 6, 1}, {0, 47, 1}, {5975, 1, 1}, {5108, 1, 1}, {0, 12, 1},
    {5108, 1, 1}, {0, 12, 1}, {0, 47, 1}, {5102, 1, 5}, {0, 7, 1}, {5102, 1, 5}, {0, 6, 1}, {5108, 1, 3},
    {0, 7, 1}, {5108, 1, 3}, {0, 6, 1}, {5108, 1, 3}, {0, 47, 1}, {213, 1, 1}, {0, 7, 1}, {213, 1, 1},
    {0, 6, 1}, {213, 1, 1}, {0, 12, 1}, {5110, 1, 1}, {0, 8, 1}, {5110, 1, 1}, {0, 21, 1}, {5110, 1, 1},
    {0, 13, 1}, {0, 7, 1}, {5110, 1, 1}, {0, 13, 1}, {0, 6, 1}, {5976, 1, 1}, {5977, 1, 1}, {5110, 1, 1},
    {0, 12, 1}, {5110, 1, 1}, {0, 13, 1}, {0, 12, 1}, {5110, 1, 5}, {0, 8, 1}, {5110, 1, 5}, {0, 21, 1},
    {5110, 1, 5}, {0, 7, 1}, {5110, 1, 5}, {0, 6, 1}, {5978, 1, 1}, {214, 1, 1}, {0, 7, 1}, {214, 1, 1},
    {0, 6, 1}, {214, 1, 1}, {0, 12, 1}, {5130, 1, 1}, {0, 8, 1}, {5130, 1, 1}, {0, 21, 1}, {5130, 1, 1},
    {0, 13, 1}, {0, 7, 1}, {5130, 1, 1}, {0, 13, 1}, {0, 6, 1}, {5123, 1, 1}, {0, 4, 1}, {5123, 1, 1},
    {0, 5, 1}, {5130, 1, 1}, {0, 12, 1}, {5130, 1, 1}, {0, 13, 1}, {0, 12, 1}, {5130, 1, 3}, {0, 8, 1},
    {5130, 1, 3}, {0, 21, 1}, {5130, 1, 3}, {0, 7, 1}, {5130, 1, 3}, {0, 6, 1}, {5123, 1, 5}, {0, 5, 1},
    {208, 1, 1}, {0, 7, 1}, {208, 1, 1}, {0, 6, 1}, {201, 1, 1}, {5979, 1, 1}, {5980, 1, 1}, {5134, 1, 1},
    {0, 7, 1}, {0, 47, 1}, {5134, 1, 1}, {0, 47, 1}, {5134, 1, 1}, {0, 6, 1}, {0, 47, 1}, {5981, 1, 1},
    {5134, 1, 1}, {0, 12, 1}, {5134, 1, 1}, {0, 12, 1}, {0, 47, 1}, {5118, 1, 3}, {0, 7, 1}, {5118, 1, 3},
    {0, 6, 1}, {5134, 1, 3}, {0, 7, 1}, {5134, 1, 3}, {0, 6, 1}, {5134, 1, 3}, {0, 47, 1}, {202, 1, 1},
    {214, 1, 1}, {5982, 1, 1}, {10, 1, 5}, {10, 1, 5}, {10, 1, 5}, {10, 1, 5}, {10, 1, 5}, {10, 1, 5},
    {10, 1, 5}, {10, 1, 7}, {10, 1, 5}, {10, 1, 5}, {10, 1, 5}, {16, 1, 1}, {16, 1, 3}, {17, 1, 1},
    {18, 1, 1}, {19, 1, 1}, {20, 1, 1}, {149, 1, 1}, {13, 1, 1}, {63, 1, 1}, {0, 66, 1}, {63, 1, 3},
    {0, 66, 1}, {63, 1, 5}, {0, 66, 1}, {63, 1, 7}, {0, 66, 1}, {66, 1, 1}, {0, 66, 1}, {66, 1, 3},
    {0, 66, 1}, {66, 1, 5}, {0, 66, 1}, {66, 1, 7}, {0, 66, 1}, {173, 1, 1}, {174, 1, 1}, {175, 1, 1},
    {176, 1, 1}, {47, 1, 3}, {47, 1, 2}, {47, 1, 2}, {47, 1, 2}, {47, 1, 2}, {47, 1, 2}, {177, 1, 1},
    {8, 1, 1}, {9, 1, 1}, {10, 1, 7}, {169, 1, 1}, {171, 1, 1}, {181, 1, 1}, {181, 1, 2}, {181, 1, 2},
    {181, 1, 2}, {181, 1, 2}, {181, 1, 2}, {182, 1, 1}, {182, 1, 2}, {182, 1, 2}, {182, 1, 2}, {182, 1, 2},
    {182, 1, 2}, {185, 1, 1}, {64, 1, 1}, {65, 1, 1}, {186, 1, 1}, {39, 1, 2}, {39, 1, 2}, {46, 1, 1},
    {11, 1, 1}, {187, 1, 1}, {189, 1, 1}, {191, 1, 1}, {192, 1, 1}, {178, 1, 1}, {442, 1, 1}, {75, 1, 1},
    {76, 1, 1}, {42, 1, 2}, {42, 1, 2}, {42, 1, 2}, {39, 1, 2}, {39, 1, 2}, {42, 1, 2}, {165, 1, 1},
    {156, 1, 1}, {179, 1, 1}, {180, 1, 1}, {159, 1, 1}, {35, 1, 1}, {190, 1, 1}, {160, 1, 1}, {438, 1, 1},
    {21, 1, 1}, {188, 1, 1}, {54, 1, 1}, {55, 1, 1}, {181, 1, 2}, {181, 1, 2}, {181, 1, 2}, {181, 1, 2},
    {56, 1, 1}, {57, 1, 1}, {58, 1, 1}, {59, 1, 1}, {60, 1, 1}, {61, 1, 1}, {62, 1, 1}, {10, 1, 5},
    {5983, 1, 1}, {4861, 1, 5}, {4957, 1, 14}, {5984, 1, 1}, {5985, 1, 1}, {4881, 1, 5}, {4883, 1, 5}, {4885, 1, 5},
    {4887, 1, 5}, {4889, 1, 5}, {4891, 1, 5}, {426, 1, 3}, {437, 1, 3}, {431, 1, 3}, {69, 1, 3}, {70, 1, 3},
    {4984, 1, 11}, {4861, 1, 7}, {4862, 1, 7}, {4874, 1, 7}, {4879, 1, 7}, {4881, 1, 7}, {4883, 1, 7}, {4885, 1, 7},
    {4887, 1, 7}, {4889, 1, 7}, {4891, 1, 7}, {426, 1, 5}, {437, 1, 5}, {431, 1, 5}, {69, 1, 5}, {70, 1, 5},
    {5986, 1, 1}, {4898, 1, 12}, {4923, 1, 13}, {4992, 1, 13}, {5055, 1, 13}, {4926, 1, 5}, {4948, 1, 14}, {4968, 1, 9},
    {4971, 1, 15}, {4982, 1, 13}, {4984, 1, 12}, {4998, 1, 11}, {5017, 1, 11}, {5028, 1, 11}, {5987, 1, 1}, {5988, 1, 1},
    {5989, 1, 1}, {4829, 1, 1}, {4830, 1, 1}, {4831, 1, 1}, {4832, 1, 1}, {4833, 1, 1}, {4834, 1, 1}, {4835, 1, 1},
    {4999, 1, 1}, {5006, 1, 8}, {5017, 1, 2}, {4836, 1, 1}, {4837, 1, 1}, {4838, 1, 1}, {4839, 1, 1}, {4840, 1, 1},
    {4841, 1, 1}, {4842, 1, 1}, {4843, 1, 1}, {4844, 1, 1}, {4845, 1, 1}, {4846, 1, 1}, {4847, 1, 1}, {4848, 1, 1},
    {4849, 1, 1}, {4850, 1, 1}, {4851, 1, 1}, {4852, 1, 1}, {4853, 1, 1}, {4854, 1, 1}, {4855, 1, 1}, {4856, 1, 1},
    {4857, 1, 1}, {4858, 1, 1}, {4859, 1, 1}, {4860, 1, 1}, {5990, 1, 1}, {5991, 1, 1}, {5992, 1, 1}, {5993, 1, 1},
    {5994, 1, 1}, {5995, 1, 1}, {5996, 1, 1}, {5997, 1, 1}, {5998, 1, 1}, {5999, 1, 1}, {6000, 1, 1}, {6001, 1, 1},
    {6002, 1, 1}, {6003, 1, 1}, {0, 51, 1}, {0, 52, 1}, {0, 54, 1}, {0, 54, 1}, {0, 55, 1}, {0, 56, 1},
    {0, 57, 1}, {0, 58, 1}, {0, 25, 1}, {0, 25, 1}, {0, 25, 1}, {0, 59, 1}, {0, 60, 1}, {0, 26, 1},
    {0, 26, 1}, {0, 26, 1}, {0, 26, 1}, {0, 61, 1}, {0, 26, 1}, {0, 26, 1}, {0, 26, 1}, {0, 25, 1},
    {0, 62, 1}, {0, 63, 1}, {0, 64, 1}, {0, 65, 1}, {0, 25, 1}, {0, 25, 1}, {0, 24, 1}, {0, 24, 1},
    {0, 24, 1}, {0, 24, 1}, {0, 22, 1}, {6004, 1, 1}, {6005, 1, 1}, {6006, 1, 1}, {6007, 1, 1}, {6008, 1, 1},
    {6009, 1, 1}, {6010, 1, 1}, {6011, 1, 1}, {6012, 1, 1}, {6013, 1, 1}, {6014, 1, 1}, {6015, 1, 1}, {6016, 1, 1},
    {6017, 1, 1}, {6018, 1, 1}, {4898, 1, 2}, {162, 1, 2}, {4909, 1, 2}, {4898, 1, 2}, {162, 1, 2}, {5017, 1, 2},
    {4909, 1, 11}, {269, 1, 2}, {4909, 1, 8}, {282, 1, 1}, {4909, 1, 2}, {162, 1, 2}, {4992, 1, 2}, {4909, 1, 2},
    {162, 1, 2}, {5038, 1, 2}, {4927, 1, 5}, {283, 1, 1}, {269, 1, 2}, {4935, 1, 6}, {4940, 1, 3}, {4948, 1, 11},
    {4948, 1, 11}, {4948, 1, 11}, {4948, 1, 5}, {4948, 1, 1}, {0, 28, 1}, {4957, 1, 12}, {4957, 1, 12}, {4971, 1, 12},
    {4971, 1, 5}, {284, 1, 1}, {4984, 1, 7}, {4984, 1, 6}, {4992, 1, 2}, {285, 1, 1}, {286, 1, 1}, {4998, 1, 7},
    {5002, 1, 7}, {5006, 1, 9}, {5006, 1, 9}, {5006, 1, 9}, {287, 1, 1}, {288, 1, 1}, {5017, 1, 6}, {4982, 1, 8},
    {5028, 1, 8}, {4923, 1, 10}, {4971, 1, 10}, {5028, 1, 8}, {4982, 1, 8}, {289, 1, 1}, {5062, 1, 7}, {290, 1, 1},
    {5134, 1, 3}, {291, 1, 1}, {5062, 1, 7}, {292, 1, 1}, {4968, 1, 5}, {4898, 1, 7}, {0, 11, 1}, {4904, 1, 7},
    {4909, 1, 11}, {293, 1, 1}, {4923, 1, 5}, {4923, 1, 11}, {4935, 1, 7}, {4939, 1, 3}, {4982, 1, 11}, {4992, 1, 5},
    {5307, 1, 3}, {5308, 1, 3}, {5309, 1, 3}, {5310, 1, 3}, {4957, 1, 5}, {294, 1, 1}, {4935, 1, 6}, {4898, 1, 8},
    {5055, 1, 8}, {5119, 1, 5}, {5100, 1, 3}, {5100, 1, 7}, {5119, 1, 9}, {425, 1, 3}, {295, 1, 1}, {296, 1, 1},
    {297, 1, 1}, {298, 1, 1}, {4915, 1, 12}, {4915, 1, 5}, {4923, 1, 5}, {4957, 1, 5}, {4962, 1, 3}, {299, 1, 1},
    {581, 1, 1}, {300, 1, 1}, {4899, 1, 1}, {4939, 1, 1}, {301, 1, 1}, {4862, 1, 2}, {442, 1, 2}, {4887, 1, 2},
    {4862, 1, 2}, {442, 1, 2}, {4891, 1, 2}, {4862, 1, 2}, {442, 1, 2}, {4862, 1, 2}, {4861, 1, 2}, {4862, 1, 2},
    {442, 1, 2}, {4879, 1, 2}, {4874, 1, 2}, {442, 1, 2}, {4879, 1, 2}, {4862, 1, 2}, {442, 1, 2}, {4883, 1, 2},
    {4874, 1, 2}, {442, 1, 2}, {4883, 1, 2}, {4879, 1, 2}, {442, 1, 2}, {4883, 1, 2}, {4881, 1, 2}, {442, 1, 2},
    {4883, 1, 2}, {4862, 1, 2}, {442, 1, 2}, {4885, 1, 2}, {4883, 1, 2}, {442, 1, 2}, {4885, 1, 2}, {4862, 1, 2},
    {442, 1, 2}, {4889, 1, 2}, {4879, 1, 2}, {442, 1, 2}, {4889, 1, 2}, {4883, 1, 2}, {442, 1, 2}, {4889, 1, 2},
    {4887, 1, 2}, {442, 1, 2}, {4889, 1, 2}, {4862, 1, 2}, {442, 1, 2}, {4957, 1, 11}, {4957, 1, 10}, {4957, 1, 10},
    {4957, 1, 10}, {4957, 1, 10}, {4957, 1, 10}, {4957, 1, 10}, {5048, 1, 8}, {5048, 1, 9}, {5048, 1, 8}, {4957, 1, 10},
    {5048, 1, 8}, {4957, 1, 10}, {4957, 1, 10}, {5048, 1, 8}, {4957, 1, 10}, {4957, 1, 10}, {4957, 1, 10}, {4957, 1, 10},
    {5055, 1, 8}, {5055, 1, 9}, {5055, 1, 8}, {4957, 1, 10}, {5055, 1, 8}, {4957, 1, 10}, {4957, 1, 10}, {4971, 1, 11},
    {4909, 1, 9}, {4915, 1, 11}, {4982, 1, 9}, {4957, 1, 3}, {4957, 1, 2}, {4957, 1, 2}, {4957, 1, 2}, {4957, 1, 2},
    {4957, 1, 2}, {4957, 1, 2}, {5048, 1, 2}, {5048, 1, 3}, {5048, 1, 2}, {4957, 1, 2}, {5048, 1, 2}, {4957, 1, 2},
    {4957, 1, 2}, {5048, 1, 2}, {4957, 1, 2}, {4957, 1, 2}, {4957, 1, 2}, {4957, 1, 2}, {5055, 1, 2}, {5055, 1, 3},
    {5055, 1, 2}, {4957, 1, 2}, {5055, 1, 2}, {4957, 1, 2}, {4957, 1, 2}, {4971, 1, 3}, {4909, 1, 3}, {4915, 1, 3},
    {4982, 1, 3}, {4892, 1, 1}, {4893, 1, 1}, {4894, 1, 1}, {4914, 1, 3}, {4914, 1, 1}, {4885, 1, 1}, {4895, 1, 1},
    {4896, 1, 1}, {4897, 1, 1}, {4861, 1, 2}, {442, 1, 2}, {4879, 1, 2}, {302, 1, 1}, {303, 1, 1}, {6019, 1, 1},
    {6020, 1, 1}, {6021, 1, 1}, {6022, 1, 1}, {304, 1, 1}, {306, 1, 1}, {305, 1, 1}, {307, 1, 1}, {308, 1, 1},
    {309, 1, 1}, {310, 1, 1}, {311, 1, 1}, {312, 1, 1}, {313, 1, 1}, {304, 1, 1}, {0, 18, 1}, {305, 1, 1},
    {0, 18, 1}, {314, 1, 1}, {315, 1, 1}, {316, 1, 1}, {317, 1, 1}, {318, 1, 1}, {319, 1, 1}, {320, 1, 1},
    {321, 1, 1}, {322, 1, 1}, {323, 1, 1}, {324, 1, 1}, {325, 1, 1}, {326, 1, 1}, {327, 1, 1}, {328, 1, 1},
    {329, 1, 1}, {330, 1, 1}, {331, 1, 1}, {308, 1, 1}, {0, 18, 1}, {332, 1, 1}, {333, 1, 1}, {334, 1, 1},
    {335, 1, 1}, {336, 1, 1}, {337, 1, 1}, {338, 1, 1}, {339, 1, 1}, {340, 1, 1}, {341, 1, 1}, {342, 1, 1},
    {343, 1, 1}, {344, 1, 1}, {345, 1, 1}, {346, 1, 1}, {347, 1, 1}, {348, 1, 1}, {349, 1, 1}, {350, 1, 1},
    {351, 1, 1}, {352, 1, 1}, {353, 1, 1}, {354, 1, 1}, {355, 1, 1}, {356, 1, 1}, {357, 1, 1}, {358, 1, 1},
    {359, 1, 1}, {360, 1, 1}, {361, 1, 1}, {362, 1, 1}, {0, 18, 1}, {366, 1, 1}, {0, 18, 1}, {364, 1, 1},
    {0, 18, 1}, {362, 1, 1}, {363, 1, 1}, {364, 1, 1}, {365, 1, 1}, {366, 1, 1}, {367, 1, 1}, {368, 1, 1},
    {369, 1, 1}, {370, 1, 1}, {371, 1, 1}, {372, 1, 1}, {373, 1, 1}, {374, 1, 1}, {375, 1, 1}, {376, 1, 1},
    {377, 1, 1}, {378, 1, 1}, {379, 1, 1}, {380, 1, 1}, {381, 1, 1}, {382, 1, 1}, {383, 1, 1}, {384, 1, 1},
    {385, 1, 1}, {386, 1, 1}, {387, 1, 1}, {388, 1, 1}, {389, 1, 1}, {390, 1, 1}, {391, 1, 1}, {392, 1, 1},
    {393, 1, 1}, {394, 1, 1}, {395, 1, 1}, {396, 1, 1}, {397, 1, 1}, {398, 1, 1}, {399, 1, 1}, {400, 1, 1},
    {401, 1, 1}, {402, 1, 1}, {403, 1, 1}, {404, 1, 1}, {405, 1, 1}, {406, 1, 1}, {407, 1, 1}, {408, 1, 1},
    {409, 1, 1}, {410, 1, 1}, {411, 1, 1}, {412, 1, 1}, {413, 1, 1}, {413, 1, 1}, {0, 18, 1}, {414, 1, 1},
    {415, 1, 1}, {416, 1, 1}, {417, 1, 1}, {417, 1, 1}, {0, 18, 1}, {418, 1, 1}, {419, 1, 1}, {419, 1, 1},
    {0, 18, 1}, {420, 1, 1}, {422, 1, 1}, {423, 1, 1}, {424, 1, 1}, {425, 1, 1}, {437, 1, 1}, {439, 1, 1},
    {440, 1, 1}, {441, 1, 1}, {443, 1, 1}, {444, 1, 1}, {445, 1, 1}, {446, 1, 1}, {447, 1, 1}, {448, 1, 1},
    {450, 1, 1}, {452, 1, 1}, {453, 1, 1}, {454, 1, 1}, {455, 1, 1}, {456, 1, 1}, {457, 1, 1}, {458, 1, 1},
    {458, 1, 1}, {0, 18, 1}, {459, 1, 1}, {459, 1, 1}, {0, 18, 1}, {460, 1, 1}, {461, 1, 1}, {462, 1, 1},
    {463, 1, 1}, {464, 1, 1}, {464, 1, 2}, {464, 1, 2}, {464, 1, 2}, {464, 1, 2}, {464, 1, 2}, {465, 1, 1},
    {465, 1, 2}, {465, 1, 2}, {465, 1, 2}, {465, 1, 2}, {465, 1, 2}, {466, 1, 1}, {467, 1, 1}, {468, 1, 1},
    {469, 1, 1}, {470, 1, 1}, {471, 1, 1}, {472, 1, 1}, {473, 1, 1}, {474, 1, 1}, {475, 1, 1}, {476, 1, 1},
    {477, 1, 1}, {478, 1, 1}, {479, 1, 1}, {480, 1, 1}, {481, 1, 1}, {477, 1, 1}, {0, 18, 1}, {482, 1, 1},
    {483, 1, 1}, {483, 1, 1}, {0, 18, 1}, {484, 1, 1}, {485, 1, 1}, {484, 1, 1}, {0, 18, 1}, {486, 1, 1},
    {486, 1, 1}, {0, 18, 1}, {487, 1, 1}, {488, 1, 1}, {489, 1, 1}, {490, 1, 1}, {491, 1, 1}, {492, 1, 1},
    {493, 1, 1}, {494, 1, 1}, {495, 1, 1}, {496, 1, 1}, {497, 1, 1}, {498, 1, 1}, {499, 1, 1}, {500, 1, 1},
    {501, 1, 1}, {502, 1, 1}, {503, 1, 1}, {504, 1, 1}, {505, 1, 1}, {506, 1, 1}, {507, 1, 1}, {508, 1, 1},
    {431, 1, 1}, {0, 18, 1}, {509, 1, 1}, {509, 1, 1}, {0, 18, 1}, {510, 1, 1}, {511, 1, 1}, {512, 1, 1},
    {513, 1, 1}, {514, 1, 1}, {515, 1, 1}, {516, 1, 1}, {517, 1, 1}, {518, 1, 1}, {519, 1, 1}, {490, 1, 1},
    {0, 18, 1}, {430, 1, 1}, {0, 18, 1}, {432, 1, 1}, {0, 18, 1}, {511, 1, 1}, {0, 18, 1}, {512, 1, 1},
    {0, 18, 1}, {520, 1, 1}, {521, 1, 1}, {520, 1, 1}, {0, 18, 1}, {521, 1, 1}, {0, 18, 1}, {522, 1, 1},
    {523, 1, 1}, {522, 1, 1}, {0, 18, 1}, {523, 1, 1}, {0, 18, 1}, {524, 1, 1}, {525, 1, 1}, {526, 1, 1},
    {527, 1, 1}, {528, 1, 1}, {529, 1, 1}, {524, 1, 1}, {0, 18, 1}, {525, 1, 1}, {0, 18, 1}, {530, 1, 1},
    {531, 1, 1}, {530, 1, 1}, {0, 18, 1}, {531, 1, 1}, {0, 18, 1}, {532, 1, 1}, {533, 1, 1}, {532, 1, 1},
    {0, 18, 1}, {533, 1, 1}, {0, 18, 1}, {534, 1, 1}, {535, 1, 1}, {536, 1, 1}, {537, 1, 1}, {538, 1, 1},
    {539, 1, 1}, {540, 1, 1}, {541, 1, 1}, {542, 1, 1}, {543, 1, 1}, {544, 1, 1}, {545, 1, 1}, {546, 1, 1},
    {547, 1, 1}, {548, 1, 1}, {549, 1, 1}, {550, 1, 1}, {551, 1, 1}, {552, 1, 1}, {553, 1, 1}, {554, 1, 1},
    {555, 1, 1}, {556, 1, 1}, {557, 1, 1}, {558, 1, 1}, {559, 1, 1}, {560, 1, 1}, {561, 1, 1}, {562, 1, 1},
    {563, 1, 1}, {564, 1, 1}, {565, 1, 1}, {566, 1, 1}, {567, 1, 1}, {558, 1, 1}, {0, 18, 1}, {564, 1, 1},
    {0, 18, 1}, {565, 1, 1}, {0, 18, 1}, {567, 1, 1}, {0, 18, 1}, {568, 1, 1}, {569, 1, 1}, {570, 1, 1},
    {571, 1, 1}, {572, 1, 1}, {573, 1, 1}, {574, 1, 1}, {575, 1, 1}, {576, 1, 1}, {577, 1, 1}, {578, 1, 1},
    {579, 1, 1}, {580, 1, 1}, {582, 1, 1}, {583, 1, 1}, {584, 1, 1}, {585, 1, 1}, {586, 1, 1}, {587, 1, 1},
    {588, 1, 1}, {589, 1, 1}, {590, 1, 1}, {591, 1, 1}, {592, 1, 1}, {593, 1, 1}, {594, 1, 1}, {595, 1, 1},
    {596, 1, 1}, {597, 1, 1}, {598, 1, 1}, {599, 1, 1}, {600, 1, 1}, {601, 1, 1}, {602, 1, 1}, {603, 1, 1},
    {604, 1, 1}, {605, 1, 1}, {606, 1, 1}, {607, 1, 1}, {608, 1, 1}, {609, 1, 1}, {610, 1, 1}, {611, 1, 1},
    {612, 1, 1}, {613, 1, 1}, {614, 1, 1}, {615, 1, 1}, {616, 1, 1}, {526, 1, 1}, {0, 18, 1}, {527, 1, 1},
    {0, 18, 1}, {541, 1, 1}, {0, 18, 1}, {542, 1, 1}, {0, 18, 1}, {617, 1, 1}, {618, 1, 1}, {619, 1, 1},
    {620, 1, 1}, {621, 1, 1}, {622, 1, 1}, {570, 1, 1}, {0, 18, 1}, {571, 1, 1}, {0, 18, 1}, {572, 1, 1},
    {0, 18, 1}, {573, 1, 1}, {0, 18, 1}, {623, 1, 1}, {624, 1, 1}, {625, 1, 1}, {626, 1, 1}, {627, 1, 1},
    {628, 1, 1}, {629, 1, 1}, {630, 1, 1}, {631, 1, 1}, {632, 1, 1}, {633, 1, 1}, {634, 1, 1}, {635, 1, 1},
    {636, 1, 1}, {637, 1, 1}, {638, 1, 1}, {639, 1, 1}, {640, 1, 1}, {641, 1, 1}, {642, 1, 1}, {643, 1, 1},
    {644, 1, 1}, {645, 1, 1}, {646, 1, 1}, {647, 1, 1}, {648, 1, 1}, {77, 1, 1}, {78, 1, 1}, {79, 1, 1},
    {80, 1, 1}, {649, 1, 1}, {650, 1, 1}, {651, 1, 1}, {652, 1, 1}, {653, 1, 1}, {654, 1, 1}, {655, 1, 1},
    {656, 1, 1}, {657, 1, 1}, {658, 1, 1}, {659, 1, 1}, {660, 1, 1}, {661, 1, 1}, {662, 1, 1}, {663, 1, 1},
    {664, 1, 1}, {665, 1, 1}, {666, 1, 1}, {667, 1, 1}, {668, 1, 1}, {669, 1, 1}, {670, 1, 1}, {671, 1, 1},
    {672, 1, 1}, {673, 1, 1}, {674, 1, 1}, {675, 1, 1}, {676, 1, 1}, {677, 1, 1}, {131, 1, 1}, {132, 1, 1},
    {678, 1, 1}, {679, 1, 1}, {680, 1, 1}, {681, 1, 1}, {682, 1, 1}, {683, 1, 1}, {684, 1, 1}, {685, 1, 1},
    {686, 1, 1}, {687, 1, 1}, {688, 1, 1}, {689, 1, 1}, {690, 1, 1}, {691, 1, 1}, {692, 1, 1}, {693, 1, 1},
    {694, 1, 1}, {695, 1, 1}, {696, 1, 1}, {697, 1, 1}, {698, 1, 1}, {699, 1, 1}, {700, 1, 1}, {701, 1, 1},
    {702, 1, 1}, {703, 1, 1}, {704, 1, 1}, {705, 1, 1}, {706, 1, 1}, {707, 1, 1}, {708, 1, 1}, {709, 1, 1},
    {710, 1, 1}, {711, 1, 1}, {712, 1, 1}, {713, 1, 1}, {714, 1, 1}, {715, 1, 1}, {716, 1, 1}, {717, 1, 1},
    {718, 1, 1}, {719, 1, 1}, {720, 1, 1}, {721, 1, 1}, {722, 1, 1}, {723, 1, 1}, {724, 1, 1}, {725, 1, 1},
    {726, 1, 1}, {727, 1, 1}, {728, 1, 1}, {729, 1, 1}, {730, 1, 1}, {731, 1, 1}, {732, 1, 1}, {733, 1, 1},
    {734, 1, 1}, {735, 1, 1}, {736, 1, 1}, {737, 1, 1}, {738, 1, 1}, {739, 1, 1}, {740, 1, 1}, {741, 1, 1},
    {742, 1, 1}, {743, 1, 1}, {744, 1, 1}, {745, 1, 1}, {746, 1, 1}, {747, 1, 1}, {748, 1, 1}, {749, 1, 1},
    {750, 1, 1}, {751, 1, 1}, {752, 1, 1}, {753, 1, 1}, {754, 1, 1}, {755, 1, 1}, {756, 1, 1}, {757, 1, 1},
    {758, 1, 1}, {759, 1, 1}, {760, 1, 1}, {761, 1, 1}, {762, 1, 1}, {763, 1, 1}, {764, 1, 1}, {765, 1, 1},
    {766, 1, 1}, {767, 1, 1}, {768, 1, 1}, {769, 1, 1}, {770, 1, 1}, {771, 1, 1}, {772, 1, 1}, {773, 1, 1},
    {774, 1, 1}, {775, 1, 1}, {776, 1, 1}, {777, 1, 1}, {778, 1, 1}, {779, 1, 1}, {780, 1, 1}, {781, 1, 1},
    {782, 1, 1}, {783, 1, 1}, {784, 1, 1}, {785, 1, 1}, {786, 1, 1}, {787, 1, 1}, {788, 1, 1}, {789, 1, 1},
    {790, 1, 1}, {791, 1, 1}, {792, 1, 1}, {793, 1, 1}, {794, 1, 1}, {795, 1, 1}, {796, 1, 1}, {797, 1, 1},
    {798, 1, 1}, {799, 1, 1}, {800, 1, 1}, {801, 1, 1}, {802, 1, 1}, {803, 1, 1}, {804, 1, 1}, {805, 1, 1},
    {806, 1, 1}, {807, 1, 1}, {808, 1, 1}, {809, 1, 1}, {810, 1, 1}, {811, 1, 1}, {812, 1, 1}, {813, 1, 1},
    {814, 1, 1}, {815, 1, 1}, {816, 1, 1}, {817, 1, 1}, {818, 1, 1}, {819, 1, 1}, {820, 1, 1}, {821, 1, 1},
    {822, 1, 1}, {823, 1, 1}, {824, 1, 1}, {825, 1, 1}, {826, 1, 1}, {827, 1, 1}, {828, 1, 1}, {829, 1, 1},
    {830, 1, 1}, {831, 1, 1}, {832, 1, 1}, {833, 1, 1}, {834, 1, 1}, {835, 1, 1}, {836, 1, 1}, {837, 1, 1},
    {838, 1, 1}, {839, 1, 1}, {840, 1, 1}, {841, 1, 1}, {842, 1, 1}, {843, 1, 1}, {844, 1, 1}, {845, 1, 1},
    {846, 1, 1}, {847, 1, 1}, {848, 1, 1}, {849, 1, 1}, {850, 1, 1}, {851, 1, 1}, {852, 1, 1}, {853, 1, 1},
    {854, 1, 1}, {855, 1, 1}, {856, 1, 1}, {857, 1, 1}, {858, 1, 1}, {859, 1, 1}, {860, 1, 1}, {861, 1, 1},
    {862, 1, 1}, {863, 1, 1}, {864, 1, 1}, {865, 1, 1}, {866, 1, 1}, {867, 1, 1}, {868, 1, 1}, {869, 1, 1},
    {870, 1, 1}, {871, 1, 1}, {872, 1, 1}, {873, 1, 1}, {874, 1, 1}, {875, 1, 1}, {876, 1, 1}, {877, 1, 1},
    {878, 1, 1}, {879, 1, 1}, {880, 1, 1}, {881, 1, 1}, {882, 1, 1}, {883, 1, 1}, {884, 1, 1}, {885, 1, 1},
    {886, 1, 1}, {887, 1, 1}, {888, 1, 1}, {889, 1, 1}, {890, 1, 1}, {891, 1, 1}, {892, 1, 1}, {893, 1, 1},
    {894, 1, 1}, {895, 1, 1}, {896, 1, 1}, {897, 1, 1}, {898, 1, 1}, {899, 1, 1}, {900, 1, 1}, {901, 1, 1},
    {902, 1, 1}, {903, 1, 1}, {904, 1, 1}, {905, 1, 1}, {906, 1, 1}, {907, 1, 1}, {908, 1, 1}, {909, 1, 1},
    {910, 1, 1}, {911, 1, 1}, {912, 1, 1}, {913, 1, 1}, {914, 1, 1}, {915, 1, 1}, {916, 1, 1}, {917, 1, 1},
    {918, 1, 1}, {919, 1, 1}, {920, 1, 1}, {921, 1, 1}, {922, 1, 1}, {923, 1, 1}, {924, 1, 1}, {925, 1, 1},
    {926, 1, 1}, {927, 1, 1}, {928, 1, 1}, {929, 1, 1}, {930, 1, 1}, {931, 1, 1}, {932, 1, 1}, {6023, 1, 1},
    {6024, 1, 1}, {6025, 1, 1}, {6026, 1, 1}, {6027, 1, 1}, {6028, 1, 1}, {6029, 1, 1}, {6030, 1, 1}, {6031, 1, 1},
    {6032, 1, 1}, {6033, 1, 1}, {6034, 1, 1}, {6035, 1, 1}, {6036, 1, 1}, {6037, 1, 1}, {6038, 1, 1}, {6039, 1, 1},
    {6040, 1, 1}, {6041, 1, 1}, {6042, 1, 1}, {6043, 1, 1}, {6044, 1, 1}, {933, 1, 1}, {934, 1, 1}, {935, 1, 1},
    {936, 1, 1}, {937, 1, 1}, {938, 1, 1}, {939, 1, 1}, {940, 1, 1}, {941, 1, 1}, {942, 1, 1}, {943, 1, 1},
    {6045, 1, 1}, {6046, 1, 1}, {6047, 1, 1}, {6048, 1, 1}, {6049, 1, 1}, {6050, 1, 1}, {6051, 1, 1}, {6052, 1, 1},
    {6053, 1, 1}, {6054, 1, 1}, {6055, 1, 1}, {6056, 1, 1}, {6057, 1, 1}, {6058, 1, 1}, {6059, 1, 1}, {6060, 1, 1},
    {6061, 1, 1}, {6062, 1, 1}, {6063, 1, 1}, {6064, 1, 1}, {6065, 1, 1}, {4862, 1, 3}, {4874, 1, 3}, {4879, 1, 3},
    {4881, 1, 3}, {4883, 1, 3}, {4885, 1, 3}, {4887, 1, 3}, {4889, 1, 3}, {4891, 1, 3}, {4862, 1, 2}, {4861, 1, 2},
    {4862, 1, 2}, {4862, 1, 2}, {4862, 1, 2}, {4874, 1, 2}, {4862, 1, 2}, {4879, 1, 2}, {4862, 1, 2}, {4881, 1, 2},
    {4862, 1, 2}, {4883, 1, 2}, {4862, 1, 2}, {4885, 1, 2}, {4862, 1, 2}, {4887, 1, 2}, {4862, 1, 2}, {4889, 1, 2},
    {4862, 1, 2}, {4891, 1, 2}, {4874, 1, 2}, {4861, 1, 2}, {69, 1, 2}, {4862, 1, 2}, {70, 1, 2}, {69, 1, 2},
    {4874, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4879, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4881, 1, 2}, {70, 1, 2},
    {69, 1, 2}, {4883, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4885, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4887, 1, 2},
    {70, 1, 2}, {69, 1, 2}, {4889, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4891, 1, 2}, {70, 1, 2}, {69, 1, 2},
    {4862, 1, 2}, {4861, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4862, 1, 2}, {4862, 1, 2}, {70, 1, 2}, {69, 1, 2},
    {4862, 1, 2}, {4874, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4862, 1, 2}, {4879, 1, 2}, {70, 1, 2}, {69, 1, 2},
    {4862, 1, 2}, {4881, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4862, 1, 2}, {4883, 1, 2}, {70, 1, 2}, {69, 1, 2},
    {4862, 1, 2}, {4885, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4862, 1, 2}, {4887, 1, 2}, {70, 1, 2}, {69, 1, 2},
    {4862, 1, 2}, {4889, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4862, 1, 2}, {4891, 1, 2}, {70, 1, 2}, {69, 1, 2},
    {4874, 1, 2}, {4861, 1, 2}, {70, 1, 2}, {4862, 1, 2}, {47, 1, 2}, {4874, 1, 2}, {47, 1, 2}, {4879, 1, 2},
    {47, 1, 2}, {4881, 1, 2}, {47, 1, 2}, {4883, 1, 2}, {47, 1, 2}, {4885, 1, 2}, {47, 1, 2}, {4887, 1, 2},
    {47, 1, 2}, {4889, 1, 2}, {47, 1, 2}, {4891, 1, 2}, {47, 1, 2}, {4862, 1, 2}, {4861, 1, 2}, {47, 1, 2},
    {4862, 1, 2}, {4862, 1, 2}, {47, 1, 2}, {4862, 1, 2}, {4874, 1, 2}, {47, 1, 2}, {4862, 1, 2}, {4879, 1, 2},
    {47, 1, 2}, {4862, 1, 2}, {4881, 1, 2}, {47, 1, 2}, {4862, 1, 2}, {4883, 1, 2}, {47, 1, 2}, {4862, 1, 2},
    {4885, 1, 2}, {47, 1, 2}, {4862, 1, 2}, {4887, 1, 2}, {47, 1, 2}, {4862, 1, 2}, {4889, 1, 2}, {47, 1, 2},
    {4862, 1, 2}, {4891, 1, 2}, {47, 1, 2}, {4874, 1, 2}, {4861, 1, 2}, {47, 1, 2}, {69, 1, 2}, {4898, 1, 2},
    {70, 1, 2}, {69, 1, 2}, {4904, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4909, 1, 2}, {70, 1, 2}, {69, 1, 2},
    {4915, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4923, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4935, 1, 2}, {70, 1, 2},
    {69, 1, 2}, {4940, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4948, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4957, 1, 2},
    {70, 1, 2}, {69, 1, 2}, {4962, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4968, 1, 2}, {70, 1, 2}, {69, 1, 2},
    {4971, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4982, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4984, 1, 2}, {70, 1, 2},
    {69, 1, 2}, {4992, 1, 2}, {70, 1, 2}, {69, 1, 2}, {4998, 1, 2}, {70, 1, 2}, {69, 1, 2}, {5002, 1, 2},
    {70, 1, 2}, {69, 1, 2}, {5006, 1, 2}, {70, 1, 2}, {69, 1, 2}, {5017, 1, 2}, {70, 1, 2}, {69, 1, 2},
    {5028, 1, 2}, {70, 1, 2}, {69, 1, 2}, {5038, 1, 2}, {70, 1, 2}, {69, 1, 2}, {5048, 1, 2}, {70, 1, 2},
    {69, 1, 2}, {5053, 1, 2}, {70, 1, 2}, {69, 1, 2}, {5055, 1, 2}, {70, 1, 2}, {69, 1, 2}, {5056, 1, 2},
    {70, 1, 2}, {69, 1, 2}, {5062, 1, 2}, {70, 1, 2}, {4898, 1, 9}, {4904, 1, 9}, {4909, 1, 12}, {4915, 1, 13},
    {4923, 1, 12}, {4935, 1, 9}, {4940, 1, 9}, {4948, 1, 12}, {4957, 1, 13}, {4962, 1, 9}, {4968, 1, 7}, {4971, 1, 13},
    {4982, 1, 12}, {4984, 1, 9}, {4992, 1, 11}, {4998, 1, 9}, {5002, 1, 9}, {5006, 1, 11}, {5017, 1, 7}, {5028, 1, 9},
    {5038, 1, 9}, {5048, 1, 11}, {5053, 1, 7}, {5055, 1, 11}, {5056, 1, 7}, {5062, 1, 9}, {4898, 1, 5}, {4904, 1, 3},
    {4909, 1, 5}, {4915, 1, 7}, {4923, 1, 7}, {4935, 1, 3}, {4940, 1, 5}, {4948, 1, 7}, {4957, 1, 7}, {4962, 1, 5},
    {4968, 1, 3}, {4971, 1, 7}, {4982, 1, 5}, {4984, 1, 3}, {4992, 1, 7}, {4998, 1, 3}, {5002, 1, 3}, {5006, 1, 5},
    {5017, 1, 3}, {5028, 1, 5}, {5038, 1, 5}, {5048, 1, 5}, {5053, 1, 3}, {5055, 1, 5}, {5056, 1, 3}, {5062, 1, 3},
    {4861, 1, 3}, {4864, 1, 1}, {4865, 1, 1}, {4867, 1, 1}, {4868, 1, 1}, {4869, 1, 1}, {4870, 1, 1}, {4871, 1, 1},
    {4872, 1, 1}, {4873, 1, 1}, {4875, 1, 1}, {4862, 1, 3}, {4874, 1, 3}, {4879, 1, 3}, {4881, 1, 3}, {4883, 1, 3},
    {4885, 1, 3}, {4887, 1, 3}, {4889, 1, 3}, {4891, 1, 3}, {4863, 1, 1}, {4861, 1, 3}, {944, 1, 1}, {945, 1, 1},
    {946, 1, 1}, {947, 1, 1}, {948, 1, 1}, {949, 1, 1}, {950, 1, 1}, {951, 1, 1}, {952, 1, 1}, {953, 1, 1},
    {954, 1, 1}, {955, 1, 1}, {956, 1, 1}, {957, 1, 1}, {958, 1, 1}, {959, 1, 1}, {960, 1, 1}, {961, 1, 1},
    {962, 1, 1}, {963, 1, 1}, {964, 1, 1}, {965, 1, 1}, {966, 1, 1}, {967, 1, 1}, {968, 1, 1}, {969, 1, 1},
    {970, 1, 1}, {971, 1, 1}, {972, 1, 1}, {973, 1, 1}, {974, 1, 1}, {975, 1, 1}, {976, 1, 1}, {977, 1, 1},
    {978, 1, 1}, {979, 1, 1}, {980, 1, 1}, {981, 1, 1}, {982, 1, 1}, {983, 1, 1}, {984, 1, 1}, {985, 1, 1},
    {986, 1, 1}, {987, 1, 1}, {988, 1, 1}, {989, 1, 1}, {990, 1, 1}, {991, 1, 1}, {992, 1, 1}, {993, 1, 1},
    {994, 1, 1}, {995, 1, 1}, {996, 1, 1}, {997, 1, 1}, {998, 1, 1}, {999, 1, 1}, {1000, 1, 1}, {1001, 1, 1},
    {1002, 1, 1}, {1003, 1, 1}, {1004, 1, 1}, {1005, 1, 1}, {1006, 1, 1}, {1007, 1, 1}, {1008, 1, 1}, {1009, 1, 1},
    {1010, 1, 1}, {1011, 1, 1}, {1012, 1, 1}, {1013, 1, 1}, {1014, 1, 1}, {1015, 1, 1}, {1016, 1, 1}, {1017, 1, 1},
    {1018, 1, 1}, {1019, 1, 1}, {1020, 1, 1}, {1021, 1, 1}, {1022, 1, 1}, {1023, 1, 1}, {1024, 1, 1}, {1025, 1, 1},
    {1026, 1, 1}, {1027, 1, 1}, {1028, 1, 1}, {1029, 1, 1}, {1030, 1, 1}, {1031, 1, 1}, {1032, 1, 1}, {1033, 1, 1},
    {1034, 1, 1}, {1035, 1, 1}, {1036, 1, 1}, {1037, 1, 1}, {1038, 1, 1}, {1039, 1, 1}, {1040, 1, 1}, {1041, 1, 1},
    {1042, 1, 1}, {1043, 1, 1}, {1044, 1, 1}, {1045, 1, 1}, {1046, 1, 1}, {1047, 1, 1}, {1048, 1, 1}, {1049, 1, 1},
    {1050, 1, 1}, {1051, 1, 1}, {1052, 1, 1}, {1053, 1, 1}, {1054, 1, 1}, {1055, 1, 1}, {1056, 1, 1}, {1057, 1, 1},
    {1058, 1, 1}, {1059, 1, 1}, {1060, 1, 1}, {1061, 1, 1}, {1062, 1, 1}, {1063, 1, 1}, {1064, 1, 1}, {1065, 1, 1},
    {1066, 1, 1}, {1067, 1, 1}, {1068, 1, 1}, {1069, 1, 1}, {1070, 1, 1}, {1071, 1, 1}, {1072, 1, 1}, {1073, 1, 1},
    {1074, 1, 1}, {1075, 1, 1}, {1076, 1, 1}, {1077, 1, 1}, {1078, 1, 1}, {1079, 1, 1}, {1080, 1, 1}, {1081, 1, 1},
    {1082, 1, 1}, {1083, 1, 1}, {1084, 1, 1}, {1085, 1, 1}, {1086, 1, 1}, {1087, 1, 1}, {1088, 1, 1}, {1089, 1, 1},
    {1090, 1, 1}, {1091, 1, 1}, {1092, 1, 1}, {1093, 1, 1}, {1094, 1, 1}, {1095, 1, 1}, {1096, 1, 1}, {1097, 1, 1},
    {1098, 1, 1}, {1099, 1, 1}, {1100, 1, 1}, {1101, 1, 1}, {1102, 1, 1}, {1103, 1, 1}, {1104, 1, 1}, {1105, 1, 1},
    {1106, 1, 1}, {1107, 1, 1}, {1108, 1, 1}, {1109, 1, 1}, {1110, 1, 1}, {1111, 1, 1}, {1112, 1, 1}, {1113, 1, 1},
    {1114, 1, 1}, {1115, 1, 1}, {1116, 1, 1}, {1117, 1, 1}, {1118, 1, 1}, {1119, 1, 1}, {1120, 1, 1}, {1121, 1, 1},
    {1122, 1, 1}, {1123, 1, 1}, {1124, 1, 1}, {1125, 1, 1}, {1126, 1, 1}, {1127, 1, 1}, {1128, 1, 1}, {1129, 1, 1},
    {1130, 1, 1}, {1131, 1, 1}, {1132, 1, 1}, {1133, 1, 1}, {1134, 1, 1}, {1135, 1, 1}, {1136, 1, 1}, {1137, 1, 1},
    {1138, 1, 1}, {1139, 1, 1}, {1140, 1, 1}, {1141, 1, 1}, {1142, 1, 1}, {1143, 1, 1}, {1144, 1, 1}, {1145, 1, 1},
    {1146, 1, 1}, {1147, 1, 1}, {1148, 1, 1}, {1149, 1, 1}, {1150, 1, 1}, {1151, 1, 1}, {1152, 1, 1}, {1153, 1, 1},
    {1154, 1, 1}, {1155, 1, 1}, {1156, 1, 1}, {1157, 1, 1}, {1158, 1, 1}, {1159, 1, 1}, {1160, 1, 1}, {1161, 1, 1},
    {1162, 1, 1}, {1163, 1, 1}, {1164, 1, 1}, {1165, 1, 1}, {1166, 1, 1}, {1167, 1, 1}, {1168, 1, 1}, {1169, 1, 1},
    {1170, 1, 1}, {1171, 1, 1}, {1172, 1, 1}, {1173, 1, 1}, {1174, 1, 1}, {1175, 1, 1}, {1176, 1, 1}, {1177, 1, 1},
    {1178, 1, 1}, {1179, 1, 1}, {1180, 1, 1}, {1181, 1, 1}, {1182, 1, 1}, {1183, 1, 1}, {1184, 1, 1}, {1185, 1, 1},
    {1186, 1, 1}, {1187, 1, 1}, {1188, 1, 1}, {1189, 1, 1}, {1190, 1, 1}, {1191, 1, 1}, {1192, 1, 1}, {1193, 1, 1},
    {1194, 1, 1}, {1195, 1, 1}, {1196, 1, 1}, {1197, 1, 1}, {1198, 1, 1}, {1199, 1, 1}, {1200, 1, 1}, {1201, 1, 1},
    {1202, 1, 1}, {1203, 1, 1}, {1204, 1, 1}, {1205, 1, 1}, {1206, 1, 1}, {1207, 1, 1}, {1208, 1, 1}, {1209, 1, 1},
    {1210, 1, 1}, {1211, 1, 1}, {1212, 1, 1}, {1213, 1, 1}, {1214, 1, 1}, {1215, 1, 1}, {1216, 1, 1}, {1217, 1, 1},
    {1218, 1, 1}, {1219, 1, 1}, {1220, 1, 1}, {1221, 1, 1}, {1222, 1, 1}, {1223, 1, 1}, {1224, 1, 1}, {1225, 1, 1},
    {1226, 1, 1}, {1227, 1, 1}, {1228, 1, 1}, {1229, 1, 1}, {1230, 1, 1}, {1231, 1, 1}, {1232, 1, 1}, {1233, 1, 1},
    {1234, 1, 1}, {1235, 1, 1}, {1236, 1, 1}, {1237, 1, 1}, {1238, 1, 1}, {1239, 1, 1}, {1240, 1, 1}, {1241, 1, 1},
    {1242, 1, 1}, {1243, 1, 1}, {1244, 1, 1}, {1245, 1, 1}, {1246, 1, 1}, {1247, 1, 1}, {2660, 1, 1}, {2661, 1, 1},
    {2662, 1, 1}, {2663, 1, 1}, {2664, 1, 1}, {2665, 1, 1}, {2666, 1, 1}, {2667, 1, 1}, {1248, 1, 1}, {1249, 1, 1},
    {1250, 1, 1}, {1251, 1, 1}, {1252, 1, 1}, {1253, 1, 1}, {1254, 1, 1}, {1255, 1, 1}, {1256, 1, 1}, {1257, 1, 1},
    {1258, 1, 1}, {1259, 1, 1}, {1260, 1, 1}, {1261, 1, 1}, {1262, 1, 1}, {1263, 1, 1}, {1264, 1, 1}, {1265, 1, 1},
    {1266, 1, 1}, {1267, 1, 1}, {1268, 1, 1}, {1269, 1, 1}, {1270, 1, 1}, {1271, 1, 1}, {1272, 1, 1}, {1273, 1, 1},
    {1274, 1, 1}, {1275, 1, 1}, {1276, 1, 1}, {1277, 1, 1}, {1278, 1, 1}, {1279, 1, 1}, {1280, 1, 1}, {1281, 1, 1},
    {1282, 1, 1}, {1283, 1, 1}, {1284, 1, 1}, {1285, 1, 1}, {1286, 1, 1}, {1287, 1, 1}, {1288, 1, 1}, {1289, 1, 1},
    {1290, 1, 1}, {1291, 1, 1}, {1292, 1, 1}, {1293, 1, 1}, {1294, 1, 1}, {1295, 1, 1}, {1296, 1, 1}, {1297, 1, 1},
    {1298, 1, 1}, {1299, 1, 1}, {1300, 1, 1}, {2668, 1, 1}, {2669, 1, 1}, {2670, 1, 1}, {1301, 1, 1}, {1302, 1, 1},
    {1303, 1, 1}, {1304, 1, 1}, {1305, 1, 1}, {1306, 1, 1}, {1307, 1, 1}, {1308, 1, 1}, {1309, 1, 1}, {1310, 1, 1},
    {1311, 1, 1}, {1312, 1, 1}, {1313, 1, 1}, {1314, 1, 1}, {1315, 1, 1}, {1316, 1, 1}, {1317, 1, 1}, {1318, 1, 1},
    {1319, 1, 1}, {1320, 1, 1}, {1321, 1, 1}, {1322, 1, 1}, {1323, 1, 1}, {1324, 1, 1}, {1325, 1, 1}, {1326, 1, 1},
    {2654, 1, 1}, {2655, 1, 1}, {2656, 1, 1}, {2657, 1, 1}, {2658, 1, 1}, {2659, 1, 1}, {1327, 1, 1}, {1328, 1, 1},
    {1329, 1, 1}, {1330, 1, 1}, {1331, 1, 1}, {1332, 1, 1}, {1333, 1, 1}, {1334, 1, 1}, {1335, 1, 1}, {1336, 1, 1},
    {1337, 1, 1}, {1338, 1, 1}, {1339, 1, 1}, {1340, 1, 1}, {1341, 1, 1}, {1342, 1, 1}, {1343, 1, 1}, {1344, 1, 1},
    {1345, 1, 1}, {1346, 1, 1}, {1347, 1, 1}, {1348, 1, 1}, {1349, 1, 1}, {1350, 1, 1}, {1351, 1, 1}, {1352, 1, 1},
    {1353, 1, 1}, {1354, 1, 1}, {1355, 1, 1}, {1356, 1, 1}, {1357, 1, 1}, {1358, 1, 1}, {1359, 1, 1}, {1360, 1, 1},
    {1361, 1, 1}, {1362, 1, 1}, {1363, 1, 1}, {1364, 1, 1}, {1365, 1, 1}, {1366, 1, 1}, {1367, 1, 1}, {1368, 1, 1},
    {1369, 1, 1}, {1370, 1, 1}, {1371, 1, 1}, {1372, 1, 1}, {1373, 1, 1}, {1374, 1, 1}, {1375, 1, 1}, {1376, 1, 1},
    {1377, 1, 1}, {1378, 1, 1}, {1379, 1, 1}, {1380, 1, 1}, {1381, 1, 1}, {1382, 1, 1}, {1383, 1, 1}, {1384, 1, 1},
    {1385, 1, 1}, {1386, 1, 1}, {1387, 1, 1}, {1388, 1, 1}, {1389, 1, 1}, {1390, 1, 1}, {1391, 1, 1}, {1392, 1, 1},
    {1393, 1, 1}, {1394, 1, 1}, {1395, 1, 1}, {1396, 1, 1}, {1397, 1, 1}, {1398, 1, 1}, {1399, 1, 1}, {1400, 1, 1},
    {1401, 1, 1}, {1402, 1, 1}, {1403, 1, 1}, {1404, 1, 1}, {1405, 1, 1}, {1406, 1, 1}, {1407, 1, 1}, {1408, 1, 1},
    {1409, 1, 1}, {1410, 1, 1}, {1411, 1, 1}, {1412, 1, 1}, {1413, 1, 1}, {1414, 1, 1}, {1415, 1, 1}, {1416, 1, 1},
    {1417, 1, 1}, {1418, 1, 1}, {1419, 1, 1}, {1420, 1, 1}, {1421, 1, 1}, {1422, 1, 1}, {1423, 1, 1}, {1424, 1, 1},
    {1425, 1, 1}, {1426, 1, 1}, {1427, 1, 1}, {1428, 1, 1}, {1429, 1, 1}, {1430, 1, 1}, {1431, 1, 1}, {1432, 1, 1},
    {1433, 1, 1}, {1434, 1, 1}, {1435, 1, 1}, {1436, 1, 1}, {1437, 1, 1}, {1438, 1, 1}, {1465, 1, 1}, {1466, 1, 1},
    {1467, 1, 1}, {1468, 1, 1}, {1469, 1, 1}, {1470, 1, 1}, {1471, 1, 1}, {1472, 1, 1}, {1473, 1, 1}, {1474, 1, 1},
    {1475, 1, 1}, {1476, 1, 1}, {1477, 1, 1}, {1478, 1, 1}, {1479, 1, 1}, {1480, 1, 1}, {1481, 1, 1}, {1482, 1, 1},
    {1483, 1, 1}, {1484, 1, 1}, {1485, 1, 1}, {1486, 1, 1}, {1487, 1, 1}, {1488, 1, 1}, {1489, 1, 1}, {1490, 1, 1},
    {1491, 1, 1}, {1492, 1, 1}, {1493, 1, 1}, {1494, 1, 1}, {1495, 1, 1}, {1496, 1, 1}, {1497, 1, 1}, {1498, 1, 1},
    {1499, 1, 1}, {1500, 1, 1}, {1501, 1, 1}, {1502, 1, 1}, {1503, 1, 1}, {1504, 1, 1}, {1505, 1, 1}, {1506, 1, 1},
    {1507, 1, 1}, {1508, 1, 1}, {1509, 1, 1}, {1510, 1, 1}, {1511, 1, 1}, {1512, 1, 1}, {1513, 1, 1}, {1514, 1, 1},
    {1515, 1, 1}, {1516, 1, 1}, {1517, 1, 1}, {1518, 1, 1}, {1519, 1, 1}, {1520, 1, 1}, {1521, 1, 1}, {1522, 1, 1},
    {1523, 1, 1}, {1524, 1, 1}, {1525, 1, 1}, {1526, 1, 1}, {1527, 1, 1}, {1528, 1, 1}, {1529, 1, 1}, {1530, 1, 1},
    {1531, 1, 1}, {1532, 1, 1}, {1533, 1, 1}, {1534, 1, 1}, {1535, 1, 1}, {1536, 1, 1}, {1537, 1, 1}, {1538, 1, 1},
    {1539, 1, 1}, {1540, 1, 1}, {1541, 1, 1}, {1542, 1, 1}, {1543, 1, 1}, {1544, 1, 1}, {1545, 1, 1}, {1546, 1, 1},
    {1547, 1, 1}, {1548, 1, 1}, {1549, 1, 1}, {1550, 1, 1}, {1551, 1, 1}, {1552, 1, 1}, {1553, 1, 1}, {1554, 1, 1},
    {1555, 1, 1}, {1556, 1, 1}, {1557, 1, 1}, {1558, 1, 1}, {1559, 1, 1}, {1560, 1, 1}, {1561, 1, 1}, {1562, 1, 1},
    {1563, 1, 1}, {1564, 1, 1}, {1565, 1, 1}, {1566, 1, 1}, {1567, 1, 1}, {1568, 1, 1}, {117, 1, 1}, {118, 1, 1},
    {119, 1, 1}, {120, 1, 1}, {121, 1, 1}, {122, 1, 1}, {123, 1, 1}, {124, 1, 1}, {125, 1, 1}, {126, 1, 1},
    {127, 1, 1}, {128, 1, 1}, {129, 1, 1}, {130, 1, 1}, {4862, 1, 3}, {4874, 1, 3}, {4879, 1, 3}, {4881, 1, 3},
    {4883, 1, 3}, {4885, 1, 3}, {4887, 1, 3}, {4889, 1, 3}, {4891, 1, 3}, {4863, 1, 1}, {4862, 1, 3}, {4874, 1, 3},
    {4879, 1, 3}, {4881, 1, 3}, {4883, 1, 3}, {4885, 1, 3}, {4887, 1, 3}, {4889, 1, 3}, {4891, 1, 3}, {4863, 1, 1},
    {4862, 1, 3}, {4874, 1, 3}, {4879, 1, 3}, {4881, 1, 3}, {4883, 1, 3}, {4885, 1, 3}, {4887, 1, 3}, {4889, 1, 3},
    {4891, 1, 3}, {4863, 1, 1}, {1569, 1, 1}, {1570, 1, 1}, {1571, 1, 1}, {1572, 1, 1}, {1573, 1, 1}, {1574, 1, 1},
    {1575, 1, 1}, {1576, 1, 1}, {1577, 1, 1}, {1578, 1, 1}, {1579, 1, 1}, {1580, 1, 1}, {1581, 1, 1}, {1582, 1, 1},
    {1583, 1, 1}, {1584, 1, 1}, {1585, 1, 1}, {1586, 1, 1}, {1587, 1, 1}, {1588, 1, 1}, {1589, 1, 1}, {1590, 1, 1},
    {1591, 1, 1}, {1592, 1, 1}, {1593, 1, 1}, {1594, 1, 1}, {1595, 1, 1}, {1596, 1, 1}, {1597, 1, 1}, {1598, 1, 1},
    {1599, 1, 1}, {1600, 1, 1}, {1601, 1, 1}, {1602, 1, 1}, {1603, 1, 1}, {1604, 1, 1}, {1605, 1, 1}, {1606, 1, 1},
    {1607, 1, 1}, {1608, 1, 1}, {1609, 1, 1}, {1610, 1, 1}, {1611, 1, 1}, {1612, 1, 1}, {1613, 1, 1}, {1614, 1, 1},
    {1615, 1, 1}, {1616, 1, 1}, {1617, 1, 1}, {105, 1, 1}, {106, 1, 1}, {1618, 1, 1}, {1619, 1, 1}, {1620, 1, 1},
    {1621, 1, 1}, {1622, 1, 1}, {1623, 1, 1}, {1624, 1, 1}, {1625, 1, 1}, {1626, 1, 1}, {1627, 1, 1}, {1628, 1, 1},
    {1629, 1, 1}, {1630, 1, 1}, {1631, 1, 1}, {1632, 1, 1}, {1633, 1, 1}, {1634, 1, 1}, {1635, 1, 1}, {1636, 1, 1},
    {1637, 1, 1}, {1638, 1, 1}, {1639, 1, 1}, {1640, 1, 1}, {1641, 1, 1}, {1642, 1, 1}, {1643, 1, 1}, {1644, 1, 1},
    {1645, 1, 1}, {1646, 1, 1}, {1647, 1, 1}, {1648, 1, 1}, {107, 1, 1}, {108, 1, 1}, {109, 1, 1}, {110, 1, 1},
    {111, 1, 1}, {112, 1, 1}, {113, 1, 1}, {114, 1, 1}, {115, 1, 1}, {116, 1, 1}, {1649, 1, 1}, {1650, 1, 1},
    {1651, 1, 1}, {1652, 1, 1}, {1653, 1, 1}, {1654, 1, 1}, {1655, 1, 1}, {1656, 1, 1}, {1657, 1, 1}, {1658, 1, 1},
    {1659, 1, 1}, {1660, 1, 1}, {1661, 1, 1}, {1662, 1, 1}, {1663, 1, 1}, {1664, 1, 1}, {2398, 1, 1}, {2399, 1, 1},
    {2400, 1, 1}, {2401, 1, 1}, {2402, 1, 1}, {2403, 1, 1}, {2404, 1, 1}, {2405, 1, 1}, {2406, 1, 1}, {2407, 1, 1},
    {2408, 1, 1}, {2409, 1, 1}, {2410, 1, 1}, {2411, 1, 1}, {2412, 1, 1}, {2413, 1, 1}, {2414, 1, 1}, {2415, 1, 1},
    {2416, 1, 1}, {2417, 1, 1}, {2418, 1, 1}, {2419, 1, 1}, {2420, 1, 1}, {2421, 1, 1}, {2422, 1, 1}, {2423, 1, 1},
    {2424, 1, 1}, {2425, 1, 1}, {2426, 1, 1}, {2427, 1, 1}, {2428, 1, 1}, {2429, 1, 1}, {2430, 1, 1}, {2431, 1, 1},
    {2432, 1, 1}, {2433, 1, 1}, {2434, 1, 1}, {2435, 1, 1}, {2436, 1, 1}, {2437, 1, 1}, {2438, 1, 1}, {2439, 1, 1},
    {2440, 1, 1}, {2441, 1, 1}, {2442, 1, 1}, {2443, 1, 1}, {2444, 1, 1}, {2445, 1, 1}, {2446, 1, 1}, {2447, 1, 1},
    {2448, 1, 1}, {2449, 1, 1}, {2450, 1, 1}, {2451, 1, 1}, {2452, 1, 1}, {2453, 1, 1}, {2454, 1, 1}, {2455, 1, 1},
    {2456, 1, 1}, {2457, 1, 1}, {2458, 1, 1}, {2459, 1, 1}, {2460, 1, 1}, {2461, 1, 1}, {2462, 1, 1}, {2463, 1, 1},
    {2464, 1, 1}, {2465, 1, 1}, {2466, 1, 1}, {2467, 1, 1}, {2468, 1, 1}, {2469, 1, 1}, {2470, 1, 1}, {2471, 1, 1},
    {2472, 1, 1}, {2473, 1, 1}, {2474, 1, 1}, {2475, 1, 1}, {2476, 1, 1}, {2477, 1, 1}, {2478, 1, 1}, {2479, 1, 1},
    {2480, 1, 1}, {2481, 1, 1}, {2482, 1, 1}, {2483, 1, 1}, {2484, 1, 1}, {2485, 1, 1}, {2486, 1, 1}, {2487, 1, 1},
    {2488, 1, 1}, {2489, 1, 1}, {2490, 1, 1}, {2491, 1, 1}, {2492, 1, 1}, {2493, 1, 1}, {2494, 1, 1}, {2495, 1, 1},
    {2496, 1, 1}, {2497, 1, 1}, {2498, 1, 1}, {2499, 1, 1}, {2500, 1, 1}, {2501, 1, 1}, {2502, 1, 1}, {2503, 1, 1},
    {2504, 1, 1}, {2505, 1, 1}, {2506, 1, 1}, {2507, 1, 1}, {2508, 1, 1}, {2509, 1, 1}, {2510, 1, 1}, {2511, 1, 1},
    {2512, 1, 1}, {2513, 1, 1}, {2514, 1, 1}, {2515, 1, 1}, {2516, 1, 1}, {2517, 1, 1}, {2518, 1, 1}, {2519, 1, 1},
    {2520, 1, 1}, {2521, 1, 1}, {2522, 1, 1}, {2523, 1, 1}, {2524, 1, 1}, {2525, 1, 1}, {2526, 1, 1}, {2527, 1, 1},
    {2528, 1, 1}, {2529, 1, 1}, {2530, 1, 1}, {2531, 1, 1}, {2532, 1, 1}, {2533, 1, 1}, {2534, 1, 1}, {2535, 1, 1},
    {2536, 1, 1}, {2537, 1, 1}, {2538, 1, 1}, {2539, 1, 1}, {2540, 1, 1}, {2541, 1, 1}, {2542, 1, 1}, {2543, 1, 1},
    {2544, 1, 1}, {2545, 1, 1}, {2546, 1, 1}, {2547, 1, 1}, {2548, 1, 1}, {2549, 1, 1}, {2550, 1, 1}, {2551, 1, 1},
    {2552, 1, 1}, {2553, 1, 1}, {2554, 1, 1}, {2555, 1, 1}, {2556, 1, 1}, {2557, 1, 1}, {2558, 1, 1}, {2559, 1, 1},
    {2560, 1, 1}, {2561, 1, 1}, {2562, 1, 1}, {2563, 1, 1}, {2564, 1, 1}, {2565, 1, 1}, {2566, 1, 1}, {2567, 1, 1},
    {2568, 1, 1}, {2569, 1, 1}, {2570, 1, 1}, {2571, 1, 1}, {2572, 1, 1}, {2573, 1, 1}, {2574, 1, 1}, {2575, 1, 1},
    {2576, 1, 1}, {2577, 1, 1}, {2578, 1, 1}, {2579, 1, 1}, {2580, 1, 1}, {2581, 1, 1}, {2582, 1, 1}, {2583, 1, 1},
    {2584, 1, 1}, {2585, 1, 1}, {2586, 1, 1}, {2587, 1, 1}, {2588, 1, 1}, {2589, 1, 1}, {2590, 1, 1}, {2591, 1, 1},
    {2592, 1, 1}, {2593, 1, 1}, {2594, 1, 1}, {2595, 1, 1}, {2596, 1, 1}, {2597, 1, 1}, {2598, 1, 1}, {2599, 1, 1},
    {2600, 1, 1}, {2601, 1, 1}, {2602, 1, 1}, {2603, 1, 1}, {2604, 1, 1}, {2605, 1, 1}, {2606, 1, 1}, {2607, 1, 1},
    {2608, 1, 1}, {2609, 1, 1}, {2610, 1, 1}, {2611, 1, 1}, {2612, 1, 1}, {2613, 1, 1}, {2614, 1, 1}, {2615, 1, 1},
    {2616, 1, 1}, {2617, 1, 1}, {2618, 1, 1}, {2619, 1, 1}, {2620, 1, 1}, {2621, 1, 1}, {2622, 1, 1}, {2623, 1, 1},
    {2624, 1, 1}, {2625, 1, 1}, {2626, 1, 1}, {2627, 1, 1}, {2628, 1, 1}, {2629, 1, 1}, {2630, 1, 1}, {2631, 1, 1},
    {2632, 1, 1}, {2633, 1, 1}, {2634, 1, 1}, {2635, 1, 1}, {2636, 1, 1}, {2637, 1, 1}, {2638, 1, 1}, {2639, 1, 1},
    {2640, 1, 1}, {2641, 1, 1}, {2642, 1, 1}, {2643, 1, 1}, {2644, 1, 1}, {2645, 1, 1}, {2646, 1, 1}, {2647, 1, 1},
    {2648, 1, 1}, {2649, 1, 1}, {2650, 1, 1}, {2651, 1, 1}, {2652, 1, 1}, {2653, 1, 1}, {1665, 1, 1}, {1666, 1, 1},
    {1667, 1, 1}, {1668, 1, 1}, {1669, 1, 1}, {1670, 1, 1}, {1671, 1, 1}, {1672, 1, 1}, {1673, 1, 1}, {1674, 1, 1},
    {1675, 1, 1}, {1676, 1, 1}, {1677, 1, 1}, {1678, 1, 1}, {1679, 1, 1}, {1680, 1, 1}, {1681, 1, 1}, {1682, 1, 1},
    {1683, 1, 1}, {1684, 1, 1}, {1685, 1, 1}, {1686, 1, 1}, {1687, 1, 1}, {1688, 1, 1}, {1689, 1, 1}, {1690, 1, 1},
    {1691, 1, 1}, {1692, 1, 1}, {1693, 1, 1}, {1694, 1, 1}, {1695, 1, 1}, {1696, 1, 1}, {1697, 1, 1}, {1698, 1, 1},
    {1699, 1, 1}, {1700, 1, 1}, {1701, 1, 1}, {1702, 1, 1}, {1703, 1, 1}, {1704, 1, 1}, {1705, 1, 1}, {1706, 1, 1},
    {1707, 1, 1}, {1708, 1, 1}, {1709, 1, 1}, {1710, 1, 1}, {1711, 1, 1}, {1712, 1, 1}, {1713, 1, 1}, {1714, 1, 1},
    {1715, 1, 1}, {1716, 1, 1}, {1717, 1, 1}, {1718, 1, 1}, {1719, 1, 1}, {1720, 1, 1}, {1721, 1, 1}, {1722, 1, 1},
    {1723, 1, 1}, {1724, 1, 1}, {1725, 1, 1}, {1726, 1, 1}, {1727, 1, 1}, {1728, 1, 1}, {1729, 1, 1}, {1730, 1, 1},
    {1731, 1, 1}, {1732, 1, 1}, {1733, 1, 1}, {1734, 1, 1}, {1735, 1, 1}, {1736, 1, 1}, {1737, 1, 1}, {1738, 1, 1},
    {1739, 1, 1}, {1740, 1, 1}, {1741, 1, 1}, {1742, 1, 1}, {1743, 1, 1}, {1744, 1, 1}, {1745, 1, 1}, {1746, 1, 1},
    {1747, 1, 1}, {1748, 1, 1}, {1749, 1, 1}, {1750, 1, 1}, {1751, 1, 1}, {1752, 1, 1}, {1753, 1, 1}, {1754, 1, 1},
    {1755, 1, 1}, {1756, 1, 1}, {1757, 1, 1}, {1758, 1, 1}, {1759, 1, 1}, {1760, 1, 1}, {1761, 1, 1}, {1762, 1, 1},
    {1763, 1, 1}, {1764, 1, 1}, {1765, 1, 1}, {1766, 1, 1}, {1767, 1, 1}, {1768, 1, 1}, {1769, 1, 1}, {1770, 1, 1},
    {1771, 1, 1}, {1772, 1, 1}, {1773, 1, 1}, {1774, 1, 1}, {1775, 1, 1}, {1776, 1, 1}, {1777, 1, 1}, {1778, 1, 1},
    {1779, 1, 1}, {1780, 1, 1}, {1781, 1, 1}, {1782, 1, 1}, {1783, 1, 1}, {1784, 1, 1}, {1785, 1, 1}, {1786, 1, 1},
    {1787, 1, 1}, {1788, 1, 1}, {1789, 1, 1}, {1790, 1, 1}, {1791, 1, 1}, {1792, 1, 1}, {1793, 1, 1}, {1794, 1, 1},
    {1795, 1, 1}, {83, 1, 1}, {84, 1, 1}, {85, 1, 1}, {86, 1, 1}, {87, 1, 1}, {88, 1, 1}, {89, 1, 1},
    {90, 1, 1}, {91, 1, 1}, {92, 1, 1}, {93, 1, 1}, {94, 1, 1}, {95, 1, 1}, {96, 1, 1}, {97, 1, 1},
    {98, 1, 1}, {99, 1, 1}, {100, 1, 1}, {101, 1, 1}, {102, 1, 1}, {103, 1, 1}, {104, 1, 1}, {1796, 1, 1},
    {1797, 1, 1}, {1798, 1, 1}, {1799, 1, 1}, {1800, 1, 1}, {1801, 1, 1}, {1802, 1, 1}, {1803, 1, 1}, {1804, 1, 1},
    {1805, 1, 1}, {1806, 1, 1}, {1807, 1, 1}, {1808, 1, 1}, {1809, 1, 1}, {1810, 1, 1}, {1811, 1, 1}, {1812, 1, 1},
    {1813, 1, 1}, {1814, 1, 1}, {1815, 1, 1}, {1816, 1, 1}, {1817, 1, 1}, {1818, 1, 1}, {1819, 1, 1}, {1820, 1, 1},
    {1821, 1, 1}, {1822, 1, 1}, {1823, 1, 1}, {1824, 1, 1}, {1825, 1, 1}, {1826, 1, 1}, {1827, 1, 1}, {1828, 1, 1},
    {1829, 1, 1}, {1830, 1, 1}, {1831, 1, 1}, {1832, 1, 1}, {1833, 1, 1}, {1834, 1, 1}, {1835, 1, 1}, {1836, 1, 1},
    {1837, 1, 1}, {1838, 1, 1}, {1839, 1, 1}, {1840, 1, 1}, {1841, 1, 1}, {1842, 1, 1}, {1843, 1, 1}, {1844, 1, 1},
    {1845, 1, 1}, {1846, 1, 1}, {1847, 1, 1}, {1848, 1, 1}, {1849, 1, 1}, {1850, 1, 1}, {1851, 1, 1}, {1852, 1, 1},
    {1853, 1, 1}, {1854, 1, 1}, {1855, 1, 1}, {1856, 1, 1}, {1857, 1, 1}, {1858, 1, 1}, {150, 1, 1}, {151, 1, 1},
    {152, 1, 1}, {153, 1, 1}, {1859, 1, 1}, {1860, 1, 1}, {1861, 1, 1}, {1862, 1, 1}, {1863, 1, 1}, {1864, 1, 1},
    {1865, 1, 1}, {1866, 1, 1}, {1867, 1, 1}, {1868, 1, 1}, {1869, 1, 1}, {1870, 1, 1}, {1871, 1, 1}, {1872, 1, 1},
    {1873, 1, 1}, {1874, 1, 1}, {1875, 1, 1}, {1876, 1, 1}, {1877, 1, 1}, {1878, 1, 1}, {1879, 1, 1}, {1880, 1, 1},
    {1881, 1, 1}, {1882, 1, 1}, {1883, 1, 1}, {1884, 1, 1}, {1885, 1, 1}, {1886, 1, 1}, {1887, 1, 1}, {1888, 1, 1},
    {1889, 1, 1}, {1890, 1, 1}, {81, 1, 1}, {82, 1, 1}, {1891, 1, 1}, {1892, 1, 1}, {1893, 1, 1}, {1894, 1, 1},
    {1895, 1, 1}, {1896, 1, 1}, {1897, 1, 1}, {1898, 1, 1}, {1899, 1, 1}, {1900, 1, 1}, {1901, 1, 1}, {1902, 1, 1},
    {1903, 1, 1}, {1904, 1, 1}, {464, 1, 2}, {464, 1, 2}, {464, 1, 2}, {464, 1, 2}, {1905, 1, 1}, {1906, 1, 1},
    {1907, 1, 1}, {1908, 1, 1}, {1909, 1, 1}, {1910, 1, 1}, {1911, 1, 1}, {1912, 1, 1}, {1913, 1, 1}, {1914, 1, 1},
    {1915, 1, 1}, {1916, 1, 1}, {1917, 1, 1}, {1918, 1, 1}, {1919, 1, 1}, {1920, 1, 1}, {1921, 1, 1}, {1922, 1, 1},
    {1923, 1, 1}, {1924, 1, 1}, {1925, 1, 1}, {1926, 1, 1}, {1927, 1, 1}, {1928, 1, 1}, {1929, 1, 1}, {1930, 1, 1},
    {1931, 1, 1}, {1932, 1, 1}, {1933, 1, 1}, {1934, 1, 1}, {1935, 1, 1}, {1936, 1, 1}, {1937, 1, 1}, {1938, 1, 1},
    {1939, 1, 1}, {1940, 1, 1}, {1941, 1, 1}, {1942, 1, 1}, {1943, 1, 1}, {1944, 1, 1}, {1945, 1, 1}, {1946, 1, 1},
    {1947, 1, 1}, {1948, 1, 1}, {1949, 1, 1}, {1950, 1, 1}, {1951, 1, 1}, {1952, 1, 1}, {1953, 1, 1}, {1954, 1, 1},
    {1955, 1, 1}, {1956, 1, 1}, {1957, 1, 1}, {1958, 1, 1}, {1959, 1, 1}, {1960, 1, 1}, {1961, 1, 1}, {1962, 1, 1},
    {1963, 1, 1}, {1964, 1, 1}, {1965, 1, 1}, {1966, 1, 1}, {1967, 1, 1}, {1968, 1, 1}, {1969, 1, 1}, {1970, 1, 1},
    {1971, 1, 1}, {1972, 1, 1}, {1973, 1, 1}, {1974, 1, 1}, {1975, 1, 1}, {1976, 1, 1}, {1977, 1, 1}, {1978, 1, 1},
    {1979, 1, 1}, {1980, 1, 1}, {1981, 1, 1}, {1982, 1, 1}, {1983, 1, 1}, {1984, 1, 1}, {1985, 1, 1}, {1986, 1, 1},
    {1987, 1, 1}, {1988, 1, 1}, {1989, 1, 1}, {1990, 1, 1}, {1991, 1, 1}, {1992, 1, 1}, {1993, 1, 1}, {1994, 1, 1},
    {1995, 1, 1}, {1996, 1, 1}, {1997, 1, 1}, {1998, 1, 1}, {1999, 1, 1}, {2000, 1, 1}, {2001, 1, 1}, {2002, 1, 1},
    {2003, 1, 1}, {2004, 1, 1}, {2005, 1, 1}, {2006, 1, 1}, {2007, 1, 1}, {36, 1, 2}, {36, 1, 2}, {431, 1, 2},
    {431, 1, 2}, {431, 1, 2}, {431, 1, 2}, {431, 1, 2}, {431, 1, 2}, {2008, 1, 1}, {2009, 1, 1}, {2010, 1, 1},
    {2011, 1, 1}, {2012, 1, 1}, {2013, 1, 1}, {2014, 1, 1}, {2015, 1, 1}, {2016, 1, 1}, {2017, 1, 1}, {2018, 1, 1},
    {2019, 1, 1}, {2020, 1, 1}, {2021, 1, 1}, {2022, 1, 1}, {2023, 1, 1}, {2024, 1, 1}, {2025, 1, 1}, {2026, 1, 1},
    {2027, 1, 1}, {2028, 1, 1}, {2029, 1, 1}, {2030, 1, 1}, {2031, 1, 1}, {2032, 1, 1}, {2033, 1, 1}, {2034, 1, 1},
    {2035, 1, 1}, {2036, 1, 1}, {2037, 1, 1}, {2038, 1, 1}, {2039, 1, 1}, {2040, 1, 1}, {2041, 1, 1}, {2042, 1, 1},
    {2043, 1, 1}, {2044, 1, 1}, {2045, 1, 1}, {2046, 1, 1}, {2047, 1, 1}, {2048, 1, 1}, {2049, 1, 1}, {2050, 1, 1},
    {2051, 1, 1}, {2052, 1, 1}, {2053, 1, 1}, {2054, 1, 1}, {2055, 1, 1}, {2056, 1, 1}, {2057, 1, 1}, {2058, 1, 1},
    {2059, 1, 1}, {2060, 1, 1}, {2061, 1, 1}, {2062, 1, 1}, {2063, 1, 1}, {2064, 1, 1}, {2065, 1, 1}, {2066, 1, 1},
    {2067, 1, 1}, {2068, 1, 1}, {2069, 1, 1}, {2070, 1, 1}, {2071, 1, 1}, {2072, 1, 1}, {2073, 1, 1}, {2074, 1, 1},
    {2075, 1, 1}, {2076, 1, 1}, {2077, 1, 1}, {2078, 1, 1}, {2079, 1, 1}, {2080, 1, 1}, {2081, 1, 1}, {2082, 1, 1},
    {2083, 1, 1}, {2084, 1, 1}, {2085, 1, 1}, {2086, 1, 1}, {2087, 1, 1}, {2088, 1, 1}, {2089, 1, 1}, {2090, 1, 1},
    {2091, 1, 1}, {2092, 1, 1}, {2093, 1, 1}, {2094, 1, 1}, {2095, 1, 1}, {2096, 1, 1}, {2097, 1, 1}, {2098, 1, 1},
    {2099, 1, 1}, {2100, 1, 1}, {2101, 1, 1}, {2102, 1, 1}, {2103, 1, 1}, {2104, 1, 1}, {2105, 1, 1}, {2106, 1, 1},
    {2107, 1, 1}, {2108, 1, 1}, {2109, 1, 1}, {0, 18, 1}, {2109, 1, 1}, {2110, 1, 1}, {2111, 1, 1}, {2112, 1, 1},
    {2113, 1, 1}, {2114, 1, 1}, {2115, 1, 1}, {2116, 1, 1}, {2117, 1, 1}, {2118, 1, 1}, {2119, 1, 1}, {2120, 1, 1},
    {2121, 1, 1}, {2122, 1, 1}, {2123, 1, 1}, {2124, 1, 1}, {2125, 1, 1}, {2126, 1, 1}, {2127, 1, 1}, {2128, 1, 1},
    {2129, 1, 1}, {2130, 1, 1}, {2131, 1, 1}, {2132, 1, 1}, {2133, 1, 1}, {2134, 1, 1}, {2135, 1, 1}, {2136, 1, 1},
    {2137, 1, 1}, {2138, 1, 1}, {2139, 1, 1}, {2140, 1, 1}, {2141, 1, 1}, {2142, 1, 1}, {2143, 1, 1}, {2144, 1, 1},
    {2145, 1, 1}, {2146, 1, 1}, {2147, 1, 1}, {2148, 1, 1}, {2149, 1, 1}, {2150, 1, 1}, {2151, 1, 1}, {2152, 1, 1},
    {2153, 1, 1}, {2154, 1, 1}, {2155, 1, 1}, {2156, 1, 1}, {2157, 1, 1}, {2158, 1, 1}, {2159, 1, 1}, {2160, 1, 1},
    {2161, 1, 1}, {2162, 1, 1}, {2163, 1, 1}, {2164, 1, 1}, {2165, 1, 1}, {2166, 1, 1}, {2167, 1, 1}, {2168, 1, 1},
    {2169, 1, 1}, {2170, 1, 1}, {2171, 1, 1}, {2172, 1, 1}, {2173, 1, 1}, {2174, 1, 1}, {2175, 1, 1}, {2176, 1, 1},
    {2177, 1, 1}, {2178, 1, 1}, {2179, 1, 1}, {2180, 1, 1}, {2181, 1, 1}, {2182, 1, 1}, {2183, 1, 1}, {2184, 1, 1},
    {2185, 1, 1}, {2186, 1, 1}, {2187, 1, 1}, {2188, 1, 1}, {2189, 1, 1}, {2190, 1, 1}, {2191, 1, 1}, {2192, 1, 1},
    {2193, 1, 1}, {2194, 1, 1}, {2195, 1, 1}, {2196, 1, 1}, {2197, 1, 1}, {2198, 1, 1}, {2199, 1, 1}, {2200, 1, 1},
    {2201, 1, 1}, {2202, 1, 1}, {2203, 1, 1}, {2204, 1, 1}, {2205, 1, 1}, {2206, 1, 1}, {2207, 1, 1}, {2208, 1, 1},
    {2209, 1, 1}, {2210, 1, 1}, {2211, 1, 1}, {2212, 1, 1}, {2213, 1, 1}, {2214, 1, 1}, {2215, 1, 1}, {2216, 1, 1},
    {2217, 1, 1}, {2218, 1, 1}, {2219, 1, 1}, {2220, 1, 1}, {2221, 1, 1}, {2222, 1, 1}, {2223, 1, 1}, {2224, 1, 1},
    {2225, 1, 1}, {2226, 1, 1}, {2227, 1, 1}, {2228, 1, 1}, {2229, 1, 1}, {2230, 1, 1}, {2231, 1, 1}, {2232, 1, 1},
    {2233, 1, 1}, {2234, 1, 1}, {2235, 1, 1}, {2236, 1, 1}, {2237, 1, 1}, {2238, 1, 1}, {2239, 1, 1}, {2240, 1, 1},
    {2241, 1, 1}, {2242, 1, 1}, {2243, 1, 1}, {2244, 1, 1}, {2245, 1, 1}, {2246, 1, 1}, {2247, 1, 1}, {2248, 1, 1},
    {2249, 1, 1}, {2250, 1, 1}, {2251, 1, 1}, {2252, 1, 1}, {2253, 1, 1}, {2254, 1, 1}, {2255, 1, 1}, {2256, 1, 1},
    {2257, 1, 1}, {2258, 1, 1}, {2259, 1, 1}, {6066, 1, 1}, {6067, 1, 1}, {2260, 1, 1}, {2261, 1, 1}, {2262, 1, 1},
    {2263, 1, 1}, {2264, 1, 1}, {2265, 1, 1}, {2266, 1, 1}, {2267, 1, 1}, {2268, 1, 1}, {2269, 1, 1}, {2270, 1, 1},
    {2271, 1, 1}, {2272, 1, 1}, {2273, 1, 1}, {2274, 1, 1}, {2275, 1, 1}, {2276, 1, 1}, {2277, 1, 1}, {2278, 1, 1},
    {2279, 1, 1}, {2280, 1, 1}, {2281, 1, 1}, {2282, 1, 1}, {2283, 1, 1}, {2284, 1, 1}, {2285, 1, 1}, {2286, 1, 1},
    {2287, 1, 1}, {2288, 1, 1}, {2289, 1, 1}, {2290, 1, 1}, {2291, 1, 1}, {2292, 1, 1}, {2293, 1, 1}, {2294, 1, 1},
    {2295, 1, 1}, {2296, 1, 1}, {2297, 1, 1}, {2298, 1, 1}, {2299, 1, 1}, {2300, 1, 1}, {2301, 1, 1}, {2302, 1, 1},
    {2303, 1, 1}, {2304, 1, 1}, {2305, 1, 1}, {2306, 1, 1}, {2307, 1, 1}, {2308, 1, 1}, {2309, 1, 1}, {2310, 1, 1},
    {2311, 1, 1}, {2312, 1, 1}, {2313, 1, 1}, {2314, 1, 1}, {2315, 1, 1}, {2316, 1, 1}, {2317, 1, 1}, {2318, 1, 1},
    {2319, 1, 1}, {2320, 1, 1}, {2321, 1, 1}, {2322, 1, 1}, {2323, 1, 1}, {2324, 1, 1}, {2325, 1, 1}, {2326, 1, 1},
    {2327, 1, 1}, {2328, 1, 1}, {2329, 1, 1}, {2330, 1, 1}, {2331, 1, 1}, {2332, 1, 1}, {2333, 1, 1}, {2334, 1, 1},
    {2335, 1, 1}, {2336, 1, 1}, {2337, 1, 1}, {2338, 1, 1}, {2339, 1, 1}, {2340, 1, 1}, {2341, 1, 1}, {2342, 1, 1},
    {2343, 1, 1}, {2344, 1, 1}, {2345, 1, 1}, {2346, 1, 1}, {2347, 1, 1}, {2348, 1, 1}, {2349, 1, 1}, {2350, 1, 1},
    {2351, 1, 1}, {2352, 1, 1}, {2353, 1, 1}, {2354, 1, 1}, {2355, 1, 1}, {2356, 1, 1}, {2357, 1, 1}, {2358, 1, 1},
    {2359, 1, 1}, {2360, 1, 1}, {2361, 1, 1}, {2362, 1, 1}, {2363, 1, 1}, {2364, 1, 1}, {2365, 1, 1}, {2366, 1, 1},
    {2367, 1, 1}, {2368, 1, 1}, {2369, 1, 1}, {2370, 1, 1}, {2371, 1, 1}, {2372, 1, 1}, {2373, 1, 1}, {2374, 1, 1},
    {2375, 1, 1}, {2376, 1, 1}, {2377, 1, 1}, {2378, 1, 1}, {2379, 1, 1}, {2380, 1, 1}, {2381, 1, 1}, {2382, 1, 1},
    {2383, 1, 1}, {2384, 1, 1}, {2385, 1, 1}, {2386, 1, 1}, {2387, 1, 1}, {2388, 1, 1}, {2389, 1, 1}, {2390, 1, 1},
    {2391, 1, 1}, {2392, 1, 1}, {2393, 1, 1}, {2394, 1, 1}, {2395, 1, 1}, {2396, 1, 1}, {2397, 1, 1}, {10, 1, 3},
    {32, 1, 1}, {49, 1, 1}, {183, 1, 1}, {4804, 1, 1}, {4814, 1, 1}, {5836, 1, 1}, {4861, 1, 1}, {131, 1, 1},
    {132, 1, 1}, {133, 1, 1}, {134, 1, 1}, {135, 1, 1}, {136, 1, 1}, {137, 1, 1}, {138, 1, 1}, {139, 1, 1},
    {140, 1, 1}, {4805, 1, 1}, {4806, 1, 1}, {141, 1, 1}, {142, 1, 1}, {143, 1, 1}, {144, 1, 1}, {145, 1, 1},
    {146, 1, 1}, {147, 1, 1}, {148, 1, 1}, {22, 1, 1}, {66, 1, 9}, {0, 66, 1}, {66, 1, 11}, {0, 66, 1},
    {66, 1, 12}, {0, 66, 1}, {4807, 1, 1}, {4862, 1, 1}, {4874, 1, 1}, {4879, 1, 1}, {4881, 1, 1}, {4883, 1, 1},
    {4885, 1, 1}, {4887, 1, 1}, {4889, 1, 1}, {4891, 1, 1}, {1, 1, 54}, {0, 53, 1}, {1, 1, 55}, {0, 53, 1},
    {1, 1, 56}, {0, 53, 1}, {1, 1, 57}, {0, 53, 1}, {1, 1, 58}, {0, 53, 1}, {1, 1, 59}, {0, 53, 1},
    {23, 1, 1}, {4816, 1, 1}, {4816, 1, 1}, {0, 27, 1}, {4817, 1, 1}, {4817, 1, 1}, {0, 27, 1}, {4818, 1, 1},
    {4805, 1, 3}, {4808, 1, 1}, {5884, 1, 1}, {5876, 1, 1}, {5885, 1, 1}, {4815, 1, 1}, {5856, 1, 1}, {184, 1, 1},
    {4809, 1, 1}, {4810, 1, 1}, {6068, 1, 1}, {5824, 1, 1}, {5824, 1, 3}, {5825, 1, 1}, {5825, 1, 3}, {5826, 1, 1},
    {5826, 1, 3}, {5827, 1, 1}, {5827, 1, 3}, {5828, 1, 1}, {5828, 1, 3}, {5829, 1, 3}, {5829, 1, 3}, {1, 1, 9},
    {0, 27, 1}, {5830, 1, 1}, {5830, 1, 1}, {1, 1, 9}, {0, 27, 1}, {5831, 1, 1}, {5831, 1, 1}, {1, 1, 9},
    {0, 27, 1}, {5832, 1, 3}, {5832, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5833, 1, 1}, {5833, 1, 1}, {1, 1, 9},
    {0, 27, 1}, {5834, 1, 1}, {5834, 1, 1}, {1, 1, 9}, {0, 27, 1}, {5835, 1, 1}, {5835, 1, 1}, {1, 1, 9},
    {0, 27, 1}, {5837, 1, 1}, {5837, 1, 1}, {1, 1, 9}, {0, 27, 1}, {5838, 1, 1}, {5838, 1, 1}, {1, 1, 9},
    {0, 27, 1}, {5839, 1, 1}, {5839, 1, 1}, {1, 1, 9}, {0, 27, 1}, {5840, 1, 1}, {5840, 1, 1}, {1, 1, 9},
    {0, 27, 1}, {5841, 1, 1}, {5841, 1, 1}, {1, 1, 9}, {0, 27, 1}, {5842, 1, 1}, {5842, 1, 3}, {5842, 1, 3},
    {1, 1, 9}, {0, 27, 1}, {5843, 1, 1}, {5843, 1, 1}, {1, 1, 9}, {0, 27, 1}, {5844, 1, 1}, {5844, 1, 1},
    {1, 1, 9}, {0, 27, 1}, {5845, 1, 1}, {5846, 1, 1}, {5847, 1, 1}, {5848, 1, 1}, {5849, 1, 1}, {5850, 1, 1},
    {5850, 1, 1}, {1, 1, 9}, {0, 27, 1}, {5850, 1, 1}, {1, 1, 11}, {0, 27, 1}, {5851, 1, 1}, {5851, 1, 1},
    {1, 1, 9}, {0, 27, 1}, {5851, 1, 1}, {1, 1, 11}, {0, 27, 1}, {5852, 1, 1}, {5852, 1, 1}, {1, 1, 9},
    {0, 27, 1}, {5852, 1, 1}, {1, 1, 11}, {0, 27, 1}, {5853, 1, 1}, {5853, 1, 1}, {1, 1, 9}, {0, 27, 1},
    {5853, 1, 1}, {1, 1, 11}, {0, 27, 1}, {5854, 1, 1}, {5854, 1, 1}, {1, 1, 9}, {0, 27, 1}, {5854, 1, 1},
    {1, 1, 11}, {0, 27, 1}, {5855, 1, 1}, {5857, 1, 1}, {5858, 1, 1}, {5859, 1, 1}, {5860, 1, 1}, {5861, 1, 1},
    {5861, 1, 3}, {5862, 1, 1}, {5862, 1, 3}, {5863, 1, 1}, {5863, 1, 3}, {5864, 1, 1}, {5865, 1, 1}, {5866, 1, 1},
    {5867, 1, 1}, {5868, 1, 1}, {5869, 1, 1}, {5869, 1, 3}, {5870, 1, 1}, {5871, 1, 1}, {5872, 1, 1}, {5873, 1, 1},
    {5826, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5829, 1, 1}, {5832, 1, 1}, {6069, 1, 1}, {6070, 1, 1}, {1, 1, 9},
    {0, 27, 1}, {1, 1, 11}, {0, 27, 1}, {216, 1, 1}, {217, 1, 1}, {4819, 1, 1}, {4819, 1, 1}, {1, 1, 9},
    {0, 27, 1}, {5863, 1, 4}, {5865, 1, 2}, {24, 1, 1}, {5824, 1, 5}, {5824, 1, 7}, {5825, 1, 5}, {5825, 1, 7},
    {5826, 1, 5}, {5826, 1, 7}, {5827, 1, 5}, {5827, 1, 7}, {5828, 1, 5}, {5828, 1, 7}, {5829, 1, 7}, {5829, 1, 7},
    {1, 1, 9}, {0, 27, 1}, {5830, 1, 3}, {5830, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5831, 1, 3}, {5831, 1, 3},
    {1, 1, 9}, {0, 27, 1}, {5832, 1, 7}, {5832, 1, 7}, {1, 1, 9}, {0, 27, 1}, {5833, 1, 3}, {5833, 1, 3},
    {1, 1, 9}, {0, 27, 1}, {5834, 1, 3}, {5834, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5835, 1, 3}, {5835, 1, 3},
    {1, 1, 9}, {0, 27, 1}, {5837, 1, 3}, {5837, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5838, 1, 3}, {5838, 1, 3},
    {1, 1, 9}, {0, 27, 1}, {5839, 1, 3}, {5839, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5840, 1, 3}, {5840, 1, 3},
    {1, 1, 9}, {0, 27, 1}, {5841, 1, 3}, {5841, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5842, 1, 5}, {5842, 1, 7},
    {5842, 1, 7}, {1, 1, 9}, {0, 27, 1}, {5843, 1, 3}, {5843, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5844, 1, 3},
    {5844, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5845, 1, 3}, {5846, 1, 3}, {5847, 1, 3}, {5848, 1, 3}, {5849, 1, 3},
    {5850, 1, 3}, {5850, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5850, 1, 3}, {1, 1, 11}, {0, 27, 1}, {5851, 1, 3},
    {5851, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5851, 1, 3}, {1, 1, 11}, {0, 27, 1}, {5852, 1, 3}, {5852, 1, 3},
    {1, 1, 9}, {0, 27, 1}, {5852, 1, 3}, {1, 1, 11}, {0, 27, 1}, {5853, 1, 3}, {5853, 1, 3}, {1, 1, 9},
    {0, 27, 1}, {5853, 1, 3}, {1, 1, 11}, {0, 27, 1}, {5854, 1, 3}, {5854, 1, 3}, {1, 1, 9}, {0, 27, 1},
    {5854, 1, 3}, {1, 1, 11}, {0, 27, 1}, {5855, 1, 3}, {5857, 1, 3}, {5858, 1, 3}, {5859, 1, 3}, {5860, 1, 3},
    {5861, 1, 5}, {5861, 1, 7}, {5862, 1, 5}, {5862, 1, 7}, {5863, 1, 5}, {5863, 1, 7}, {5864, 1, 3}, {5865, 1, 3},
    {5866, 1, 3}, {5867, 1, 3}, {5868, 1, 3}, {5869, 1, 5}, {5869, 1, 7}, {5870, 1, 3}, {5871, 1, 3}, {5872, 1, 3},
    {5873, 1, 3}, {5826, 1, 7}, {1, 1, 9}, {0, 27, 1}, {5829, 1, 5}, {5832, 1, 5}, {5869, 1, 7}, {1, 1, 9},
    {0, 27, 1}, {5870, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5871, 1, 3}, {1, 1, 9}, {0, 27, 1}, {5872, 1, 3},
    {1, 1, 9}, {0, 27, 1}, {25, 1, 1}, {4820, 1, 1}, {4821, 1, 1}, {4821, 1, 1}, {1, 1, 9}, {0, 27, 1},
    {5833, 1, 4}, {5844, 1, 4}, {2671, 1, 1}, {2672, 1, 1}, {2673, 1, 1}, {2674, 1, 1}, {2675, 1, 1}, {2676, 1, 1},
    {2677, 1, 1}, {2678, 1, 1}, {2679, 1, 1}, {2680, 1, 1}, {2681, 1, 1}, {2682, 1, 1}, {2683, 1, 1}, {2684, 1, 1},
    {2685, 1, 1}, {2686, 1, 1}, {2687, 1, 1}, {2688, 1, 1}, {2689, 1, 1}, {2690, 1, 1}, {2691, 1, 1}, {2692, 1, 1},
    {2693, 1, 1}, {2694, 1, 1}, {2695, 1, 1}, {2696, 1, 1}, {2697, 1, 1}, {2698, 1, 1}, {2699, 1, 1}, {2700, 1, 1},
    {2701, 1, 1}, {2702, 1, 1}, {2703, 1, 1}, {2704, 1, 1}, {2705, 1, 1}, {2706, 1, 1}, {2707, 1, 1}, {2708, 1, 1},
    {2709, 1, 1}, {2710, 1, 1}, {2711, 1, 1}, {2712, 1, 1}, {2713, 1, 1}, {2714, 1, 1}, {6071, 1, 1}, {6072, 1, 1},
    {6073, 1, 1}, {6074, 1, 1}, {2715, 1, 1}, {2716, 1, 1}, {2717, 1, 1}, {2718, 1, 1}, {2719, 1, 1}, {2720, 1, 1},
    {2721, 1, 1}, {2722, 1, 1}, {2723, 1, 1}, {2724, 1, 1}, {2725, 1, 1}, {2726, 1, 1}, {2727, 1, 1}, {2728, 1, 1},
    {2729, 1, 1}, {2730, 1, 1}, {2731, 1, 1}, {2732, 1, 1}, {2733, 1, 1}, {2734, 1, 1}, {2735, 1, 1}, {2736, 1, 1},
    {2737, 1, 1}, {2738, 1, 1}, {2739, 1, 1}, {2740, 1, 1}, {2741, 1, 1}, {2742, 1, 1}, {2743, 1, 1}, {2744, 1, 1},
    {2745, 1, 1}, {2746, 1, 1}, {2747, 1, 1}, {2748, 1, 1}, {2749, 1, 1}, {2750, 1, 1}, {2751, 1, 1}, {2752, 1, 1},
    {2753, 1, 1}, {2754, 1, 1}, {2755, 1, 1}, {2756, 1, 1}, {2757, 1, 1}, {2758, 1, 1}, {2759, 1, 1}, {2760, 1, 1},
    {2761, 1, 1}, {2762, 1, 1}, {2763, 1, 1}, {2764, 1, 1}, {2765, 1, 1}, {2766, 1, 1}, {2767, 1, 1}, {2768, 1, 1},
    {2769, 1, 1}, {2770, 1, 1}, {2771, 1, 1}, {2772, 1, 1}, {2773, 1, 1}, {2774, 1, 1}, {2775, 1, 1}, {2776, 1, 1},
    {2777, 1, 1}, {2778, 1, 1}, {2779, 1, 1}, {2780, 1, 1}, {2781, 1, 1}, {2782, 1, 1}, {2783, 1, 1}, {2784, 1, 1},
    {2785, 1, 1}, {2786, 1, 1}, {2787, 1, 1}, {2788, 1, 1}, {2789, 1, 1}, {2790, 1, 1}, {2791, 1, 1}, {2792, 1, 1},
    {2793, 1, 1}, {2794, 1, 1}, {2795, 1, 1}, {2796, 1, 1}, {2797, 1, 1}, {2798, 1, 1}, {2799, 1, 1}, {2800, 1, 1},
    {2801, 1, 1}, {2802, 1, 1}, {2803, 1, 1}, {2804, 1, 1}, {2805, 1, 1}, {2806, 1, 1}, {2807, 1, 1}, {2808, 1, 1},
    {2809, 1, 1}, {2810, 1, 1}, {2811, 1, 1}, {2812, 1, 1}, {2813, 1, 1}, {2814, 1, 1}, {6075, 1, 1}, {6076, 1, 1},
    {6077, 1, 1}, {6078, 1, 1}, {6079, 1, 1}, {6080, 1, 1}, {6081, 1, 1}, {6082, 1, 1}, {6083, 1, 1}, {6084, 1, 1},
    {6085, 1, 1}, {6086, 1, 1}, {2815, 1, 1}, {2816, 1, 1}, {2817, 1, 1}, {2818, 1, 1}, {2819, 1, 1}, {2820, 1, 1},
    {2821, 1, 1}, {2822, 1, 1}, {2823, 1, 1}, {2824, 1, 1}, {2825, 1, 1}, {2826, 1, 1}, {2827, 1, 1}, {2828, 1, 1},
    {2829, 1, 1}, {6087, 1, 1}, {6088, 1, 1}, {2830, 1, 1}, {2831, 1, 1}, {2832, 1, 1}, {2833, 1, 1}, {2834, 1, 1},
    {2835, 1, 1}, {2836, 1, 1}, {2837, 1, 1}, {2838, 1, 1}, {2839, 1, 1}, {2840, 1, 1}, {2841, 1, 1}, {2842, 1, 1},
    {2843, 1, 1}, {2844, 1, 1}, {6089, 1, 1}, {2845, 1, 1}, {2846, 1, 1}, {2847, 1, 1}, {2848, 1, 1}, {2849, 1, 1},
    {2850, 1, 1}, {2851, 1, 1}, {2852, 1, 1}, {2853, 1, 1}, {2854, 1, 1}, {2855, 1, 1}, {2856, 1, 1}, {2857, 1, 1},
    {2858, 1, 1}, {2859, 1, 1}, {6090, 1, 1}, {2860, 1, 1}, {2861, 1, 1}, {2862, 1, 1}, {2863, 1, 1}, {2864, 1, 1},
    {2865, 1, 1}, {2866, 1, 1}, {2867, 1, 1}, {2868, 1, 1}, {2869, 1, 1}, {2870, 1, 1}, {2871, 1, 1}, {2872, 1, 1},
    {2873, 1, 1}, {2874, 1, 1}, {2875, 1, 1}, {2876, 1, 1}, {2877, 1, 1}, {2878, 1, 1}, {2879, 1, 1}, {2880, 1, 1},
    {2881, 1, 1}, {2882, 1, 1}, {2883, 1, 1}, {2884, 1, 1}, {2885, 1, 1}, {2886, 1, 1}, {2887, 1, 1}, {2888, 1, 1},
    {2889, 1, 1}, {2890, 1, 1}, {2891, 1, 1}, {2892, 1, 1}, {2893, 1, 1}, {2894, 1, 1}, {2895, 1, 1}, {2896, 1, 1},
    {6091, 1, 1}, {6092, 1, 1}, {6093, 1, 1}, {6094, 1, 1}, {6095, 1, 1}, {6096, 1, 1}, {6097, 1, 1}, {6098, 1, 1},
    {6099, 1, 1}, {6100, 1, 1}, {4861, 1, 2}, {47, 1, 2}, {4861, 1, 2}, {26, 1, 2}, {4862, 1, 2}, {26, 1, 2},
    {4874, 1, 2}, {26, 1, 2}, {4879, 1, 2}, {26, 1, 2}, {4881, 1, 2}, {26, 1, 2}, {4883, 1, 2}, {26, 1, 2},
    {4885, 1, 2}, {26, 1, 2}, {4887, 1, 2}, {26, 1, 2}, {4889, 1, 2}, {26, 1, 2}, {4891, 1, 2}, {26, 1, 2},
    {4861, 1, 3}, {4861, 1, 3}, {2999, 1, 1}, {3000, 1, 1}, {3001, 1, 1}, {69, 1, 2}, {4898, 1, 8}, {70, 1, 2},
    {69, 1, 2}, {4904, 1, 6}, {70, 1, 2}, {69, 1, 2}, {4909, 1, 8}, {70, 1, 2}, {69, 1, 2}, {4915, 1, 10},
    {70, 1, 2}, {69, 1, 2}, {4923, 1, 10}, {70, 1, 2}, {69, 1, 2}, {4935, 1, 6}, {70, 1, 2}, {69, 1, 2},
    {4940, 1, 8}, {70, 1, 2}, {69, 1, 2}, {4948, 1, 10}, {70, 1, 2}, {69, 1, 2}, {4957, 1, 10}, {70, 1, 2},
    {69, 1, 2}, {4962, 1, 8}, {70, 1, 2}, {69, 1, 2}, {4968, 1, 6}, {70, 1, 2}, {69, 1, 2}, {4971, 1, 10},
    {70, 1, 2}, {69, 1, 2}, {4982, 1, 8}, {70, 1, 2}, {69, 1, 2}, {4984, 1, 6}, {70, 1, 2}, {69, 1, 2},
    {4992, 1, 10}, {70, 1, 2}, {69, 1, 2}, {4998, 1, 6}, {70, 1, 2}, {69, 1, 2}, {5002, 1, 6}, {70, 1, 2},
    {69, 1, 2}, {5006, 1, 8}, {70, 1, 2}, {69, 1, 2}, {5017, 1, 6}, {70, 1, 2}, {69, 1, 2}, {5028, 1, 8},
    {70, 1, 2}, {69, 1, 2}, {5038, 1, 8}, {70, 1, 2}, {69, 1, 2}, {5048, 1, 8}, {70, 1, 2}, {69, 1, 2},
    {5053, 1, 6}, {70, 1, 2}, {69, 1, 2}, {5055, 1, 8}, {70, 1, 2}, {69, 1, 2}, {5056, 1, 6}, {70, 1, 2},
    {69, 1, 2}, {5062, 1, 6}, {70, 1, 2}, {141, 1, 2}, {5017, 1, 6}, {142, 1, 2}, {4909, 1, 12}, {5006, 1, 11},
    {4909, 1, 8}, {4915, 1, 10}, {5053, 1, 6}, {5062, 1, 6}, {279, 1, 1}, {4898, 1, 13}, {4904, 1, 11}, {4909, 1, 13},
    {4915, 1, 15}, {4923, 1, 14}, {4935, 1, 11}, {4940, 1, 11}, {4948, 1, 15}, {4957, 1, 15}, {4962, 1, 12}, {4968, 1, 11},
    {4971, 1, 16}, {4982, 1, 14}, {4984, 1, 13}, {4992, 1, 14}, {4998, 1, 12}, {5002, 1, 11}, {5006, 1, 13}, {5017, 1, 12},
    {5028, 1, 12}, {5038, 1, 11}, {5048, 1, 12}, {5053, 1, 11}, {5055, 1, 14}, {5056, 1, 11}, {5062, 1, 11}, {4948, 1, 10},
    {5048, 1, 8}, {4982, 1, 8}, {5048, 1, 8}, {5017, 1, 6}, {4915, 1, 10}, {5017, 1, 6}, {5017, 1, 6}, {4998, 1, 6},
    {4998, 1, 6}, {5048, 1, 8}, {5053, 1, 6}, {4909, 1, 8}, {4898, 1, 9}, {4904, 1, 9}, {4909, 1, 12}, {4915, 1, 13},
    {4923, 1, 12}, {4935, 1, 9}, {4940, 1, 9}, {4948, 1, 12}, {4957, 1, 13}, {4962, 1, 9}, {4968, 1, 7}, {4971, 1, 13},
    {4982, 1, 12}, {4984, 1, 9}, {4992, 1, 11}, {4998, 1, 9}, {5002, 1, 9}, {5006, 1, 11}, {5017, 1, 7}, {5028, 1, 9},
    {5038, 1, 9}, {5048, 1, 11}, {5053, 1, 7}, {5055, 1, 11}, {5056, 1, 7}, {5062, 1, 9}, {4982, 1, 8}, {4909, 1, 8},
    {4982, 1, 8}, {4915, 1, 10}, {4982, 1, 8}, {5006, 1, 8}, {3002, 1, 1}, {3003, 1, 1}, {3004, 1, 1}, {4898, 1, 13},
    {4904, 1, 11}, {4909, 1, 13}, {4915, 1, 15}, {4923, 1, 14}, {4935, 1, 11}, {4940, 1, 11}, {4948, 1, 15}, {4957, 1, 15},
    {4962, 1, 12}, {4968, 1, 11}, {4971, 1, 16}, {4982, 1, 14}, {4984, 1, 13}, {4992, 1, 14}, {4998, 1, 12}, {5002, 1, 11},
    {5006, 1, 13}, {5017, 1, 12}, {5028, 1, 12}, {5038, 1, 11}, {5048, 1, 12}, {5053, 1, 11}, {5055, 1, 14}, {5056, 1, 11},
    {5062, 1, 11}, {4998, 1, 12}, {4957, 1, 2}, {4909, 1, 2}, {4998, 1, 2}, {4898, 1, 2}, {5017, 1, 2}, {4898, 1, 2},
    {4898, 1, 2}, {4904, 1, 2}, {5053, 1, 2}, {4909, 1, 2}, {4915, 1, 10}, {4962, 1, 8}, {4909, 1, 2}, {4971, 1, 2},
    {4910, 1, 1}, {4937, 1, 1}, {4957, 1, 2}, {4915, 1, 2}, {4985, 1, 1}, {4984, 1, 2}, {4940, 1, 2}, {4992, 1, 2},
    {4968, 1, 2}, {5019, 1, 1}, {5040, 1, 1}, {5048, 1, 2}, {5017, 1, 2}, {4880, 1, 1}, {4878, 1, 1}, {4877, 1, 1},
    {4882, 1, 1}, {4890, 1, 1}, {4884, 1, 1}, {4888, 1, 1}, {4876, 1, 1}, {4886, 1, 1}, {4866, 1, 1}, {4915, 1, 14},
    {4948, 1, 2}, {4909, 1, 2}, {4949, 1, 1}, {4950, 1, 1}, {4972, 1, 1}, {5018, 1, 1}, {5039, 1, 1}, {5049, 1, 1},
    {281, 1, 1}, {6101, 1, 1}, {6102, 1, 1}, {6103, 1, 1}, {6104, 1, 1}, {6105, 1, 1}, {6106, 1, 1}, {6107, 1, 1},
    {6108, 1, 1}, {6109, 1, 1}, {6110, 1, 1}, {6111, 1, 1}, {6112, 1, 1}, {6113, 1, 1}, {6114, 1, 1}, {6115, 1, 1},
    {6116, 1, 1}, {6117, 1, 1}, {6118, 1, 1}, {6119, 1, 1}, {6120, 1, 1}, {6121, 1, 1}, {6122, 1, 1}, {6123, 1, 1},
    {6124, 1, 1}, {6125, 1, 1}, {6126, 1, 1}, {6127, 1, 1}, {6128, 1, 1}, {6129, 1, 1}, {6130, 1, 1}, {6131, 1, 1},
    {6132, 1, 1}, {6133, 1, 1}, {6134, 1, 1}, {6135, 1, 1}, {6136, 1, 1}, {6137, 1, 1}, {6138, 1, 1}, {6139, 1, 1},
    {6140, 1, 1}, {6141, 1, 1}, {6142, 1, 1}, {6143, 1, 1}, {6144, 1, 1}, {6145, 1, 1}, {6146, 1, 1}, {6147, 1, 1},
    {6148, 1, 1}, {6149, 1, 1}, {6150, 1, 1}, {6151, 1, 1}, {6152, 1, 1}, {6153, 1, 1}, {6154, 1, 1}, {6155, 1, 1},
    {6156, 1, 1}, {1439, 1, 1}, {1440, 1, 1}, {1441, 1, 1}, {1442, 1, 1}, {1443, 1, 1}, {1444, 1, 1}, {1445, 1, 1},
    {1446, 1, 1}, {1447, 1, 1}, {1448, 1, 1}, {1449, 1, 1}, {1450, 1, 1}, {1451, 1, 1}, {1452, 1, 1}, {1453, 1, 1},
    {1454, 1, 1}, {1455, 1, 1}, {1456, 1, 1}, {1457, 1, 1}, {1458, 1, 1}, {1459, 1, 1}, {1460, 1, 1}, {1461, 1, 1},
    {1462, 1, 1}, {1463, 1, 1}, {1464, 1, 1}, {5854, 1, 2}, {5829, 1, 4}, {5833, 1, 4}, {5833, 1, 4}, {5834, 1, 5},
    {6157, 1, 1}, {6158, 1, 1}, {6159, 1, 1}, {6160, 1, 1}, {6161, 1, 1}, {6162, 1, 1}, {6163, 1, 1}, {6164, 1, 1},
    {6165, 1, 1}, {6166, 1, 1}, {6167, 1, 1}, {6168, 1, 1}, {6169, 1, 1}, {5899, 1, 1}, {5895, 1, 1}, {5886, 1, 1},
    {5843, 1, 4}, {1, 1, 10}, {0, 27, 2}, {5878, 1, 1}, {5893, 1, 1}, {5917, 1, 1}, {5894, 1, 1}, {5879, 1, 1},
    {5906, 1, 1}, {5911, 1, 1}, {5904, 1, 1}, {5882, 1, 1}, {5897, 1, 1}, {5880, 1, 1}, {5905, 1, 1}, {5881, 1, 1},
    {5916, 1, 1}, {5912, 1, 1}, {5918, 1, 1}, {5892, 1, 1}, {5890, 1, 1}, {5910, 1, 1}, {5901, 1, 1}, {5903, 1, 1},
    {5874, 1, 1}, {5875, 1, 1}, {5920, 1, 1}, {5896, 1, 1}, {5877, 1, 1}, {5888, 1, 1}, {5902, 1, 1}, {5919, 1, 1},
    {5900, 1, 1}, {5914, 1, 1}, {5915, 1, 1}, {5889, 1, 1}, {5909, 1, 1}, {5908, 1, 1}, {5907, 1, 1}, {5913, 1, 1},
    {5883, 1, 1}, {5891, 1, 1}, {5921, 1, 1}, {6170, 1, 1}, {6171, 1, 1}, {6172, 1, 1}, {6173, 1, 1}, {6174, 1, 1},
    {6175, 1, 1}, {6176, 1, 1}, {6177, 1, 1}, {6178, 1, 1}, {6179, 1, 1}, {6180, 1, 1}, {5898, 1, 1}, {5887, 1, 1},
    {6181, 1, 1}, {6182, 1, 1}, {6183, 1, 1}, {6184, 1, 1}, {6185, 1, 1}, {6186, 1, 1}, {6187, 1, 1}, {6188, 1, 1},
    {6189, 1, 1}, {6190, 1, 1}, {6191, 1, 1}, {6192, 1, 1}, {6193, 1, 1}, {6194, 1, 1}, {3005, 1, 1}, {3006, 1, 1},
    {3007, 1, 1}, {3008, 1, 1}, {3009, 1, 1}, {3010, 1, 1}, {6195, 1, 1}, {6196, 1, 1}, {6197, 1, 1}, {6198, 1, 1},
    {6199, 1, 1}, {6200, 1, 1}, {6201, 1, 1}, {6202, 1, 1}, {6203, 1, 1}, {6204, 1, 1}, {6205, 1, 1}, {6206, 1, 1},
    {6207, 1, 1}, {6208, 1, 1}, {6209, 1, 1}, {6210, 1, 1}, {6211, 1, 1}, {6212, 1, 1}, {6213, 1, 1}, {6214, 1, 1},
    {6215, 1, 1}, {6216, 1, 1}, {6217, 1, 1}, {6218, 1, 1}, {6219, 1, 1}, {6220, 1, 1}, {6221, 1, 1}, {6222, 1, 1},
    {6223, 1, 1}, {6224, 1, 1}, {6225, 1, 1}, {6226, 1, 1}, {6227, 1, 1}, {6228, 1, 1}, {6229, 1, 1}, {6230, 1, 1},
    {6231, 1, 1}, {6232, 1, 1}, {6233, 1, 1}, {6234, 1, 1}, {6235, 1, 1}, {6236, 1, 1}, {6237, 1, 1}, {6238, 1, 1},
    {6239, 1, 1}, {6240, 1, 1}, {6241, 1, 1}, {6242, 1, 1}, {6243, 1, 1}, {6244, 1, 1}, {6245, 1, 1}, {6246, 1, 1},
    {6247, 1, 1}, {6248, 1, 1}, {6249, 1, 1}, {6250, 1, 1}, {6251, 1, 1}, {6252, 1, 1}, {6253, 1, 1}, {6254, 1, 1},
    {6255, 1, 1}, {6256, 1, 1}, {6257, 1, 1}, {6258, 1, 1}, {6259, 1, 1}, {6260, 1, 1}, {6261, 1, 1}, {6262, 1, 1},
    {6263, 1, 1}, {6264, 1, 1}, {6265, 1, 1}, {6266, 1, 1}, {6267, 1, 1}, {6268, 1, 1}, {6269, 1, 1}, {6270, 1, 1},
    {6271, 1, 1}, {6272, 1, 1}, {6273, 1, 1}, {6274, 1, 1}, {6275, 1, 1}, {6276, 1, 1}, {6277, 1, 1}, {6278, 1, 1},
    {6279, 1, 1}, {6280, 1, 1}, {6281, 1, 1}, {6282, 1, 1}, {6283, 1, 1}, {6284, 1, 1}, {6285, 1, 1}, {6286, 1, 1},
    {6287, 1, 1}, {6288, 1, 1}, {6289, 1, 1}, {6290, 1, 1}, {6291, 1, 1}, {6292, 1, 1}, {6293, 1, 1}, {6294, 1, 1},
    {6295, 1, 1}, {6296, 1, 1}, {6297, 1, 1}, {6298, 1, 1}, {6299, 1, 1}, {6300, 1, 1}, {6301, 1, 1}, {6302, 1, 1},
    {6303, 1, 1}, {6304, 1, 1}, {6305, 1, 1}, {6306, 1, 1}, {6307, 1, 1}, {6308, 1, 1}, {6309, 1, 1}, {6310, 1, 1},
    {6311, 1, 1}, {6312, 1, 1}, {6313, 1, 1}, {6314, 1, 1}, {6315, 1, 1}, {6316, 1, 1}, {6317, 1, 1}, {6318, 1, 1},
    {6319, 1, 1}, {6320, 1, 1}, {6321, 1, 1}, {6322, 1, 1}, {6323, 1, 1}, {6324, 1, 1}, {6325, 1, 1}, {6326, 1, 1},
    {6327, 1, 1}, {6328, 1, 1}, {6329, 1, 1}, {6330, 1, 1}, {6331, 1, 1}, {6332, 1, 1}, {6333, 1, 1}, {6334, 1, 1},
    {6335, 1, 1}, {6336, 1, 1}, {6337, 1, 1}, {6338, 1, 1}, {6339, 1, 1}, {6340, 1, 1}, {6341, 1, 1}, {6342, 1, 1},
    {6343, 1, 1}, {6344, 1, 1}, {6345, 1, 1}, {6346, 1, 1}, {6347, 1, 1}, {6348, 1, 1}, {3011, 1, 1}, {3012, 1, 1},
    {3013, 1, 1}, {3014, 1, 1}, {3015, 1, 1}, {3016, 1, 1}, {3017, 1, 1}, {3018, 1, 1}, {3019, 1, 1}, {3020, 1, 1},
    {3021, 1, 1}, {3022, 1, 1}, {3023, 1, 1}, {3024, 1, 1}, {3025, 1, 1}, {3026, 1, 1}, {3027, 1, 1}, {3028, 1, 1},
    {3029, 1, 1}, {3030, 1, 1}, {3031, 1, 1}, {3032, 1, 1}, {3033, 1, 1}, {3034, 1, 1}, {3035, 1, 1}, {3036, 1, 1},
    {3037, 1, 1}, {3038, 1, 1}, {3039, 1, 1}, {3040, 1, 1}, {3041, 1, 1}, {3042, 1, 1}, {3043, 1, 1}, {3044, 1, 1},
    {3045, 1, 1}, {3046, 1, 1}, {3047, 1, 1}, {3048, 1, 1}, {3049, 1, 1}, {3050, 1, 1}, {3051, 1, 1}, {3052, 1, 1},
    {3053, 1, 1}, {3054, 1, 1}, {3055, 1, 1}, {3056, 1, 1}, {3057, 1, 1}, {3058, 1, 1}, {3059, 1, 1}, {3060, 1, 1},
    {3061, 1, 1}, {3062, 1, 1}, {3063, 1, 1}, {3064, 1, 1}, {3065, 1, 1}, {3066, 1, 1}, {3067, 1, 1}, {3068, 1, 1},
    {3069, 1, 1}, {3070, 1, 1}, {3071, 1, 1}, {3072, 1, 1}, {3073, 1, 1}, {3074, 1, 1}, {3075, 1, 1}, {3076, 1, 1},
    {3077, 1, 1}, {3078, 1, 1}, {3079, 1, 1}, {3080, 1, 1}, {3081, 1, 1}, {3082, 1, 1}, {3083, 1, 1}, {3084, 1, 1},
    {3085, 1, 1}, {3086, 1, 1}, {3087, 1, 1}, {3088, 1, 1}, {3089, 1, 1}, {3090, 1, 1}, {3091, 1, 1}, {3092, 1, 1},
    {3093, 1, 1}, {3094, 1, 1}, {3095, 1, 1}, {3096, 1, 1}, {3097, 1, 1}, {3098, 1, 1}, {3099, 1, 1}, {3100, 1, 1},
    {3101, 1, 1}, {3102, 1, 1}, {3103, 1, 1}, {3104, 1, 1}, {3105, 1, 1}, {3106, 1, 1}, {3107, 1, 1}, {3108, 1, 1},
    {3109, 1, 1}, {3110, 1, 1}, {3111, 1, 1}, {3112, 1, 1}, {3113, 1, 1}, {3114, 1, 1}, {3115, 1, 1}, {3116, 1, 1},
    {3117, 1, 1}, {3118, 1, 1}, {3119, 1, 1}, {3120, 1, 1}, {3121, 1, 1}, {3122, 1, 1}, {3123, 1, 1}, {3124, 1, 1},
    {3125, 1, 1}, {3126, 1, 1}, {3127, 1, 1}, {3128, 1, 1}, {3129, 1, 1}, {3130, 1, 1}, {3131, 1, 1}, {3132, 1, 1},
    {3133, 1, 1}, {3134, 1, 1}, {3135, 1, 1}, {3136, 1, 1}, {3137, 1, 1}, {3138, 1, 1}, {3139, 1, 1}, {3140, 1, 1},
    {3141, 1, 1}, {3142, 1, 1}, {3143, 1, 1}, {3144, 1, 1}, {3145, 1, 1}, {3146, 1, 1}, {3147, 1, 1}, {3148, 1, 1},
    {3149, 1, 1}, {3150, 1, 1}, {3151, 1, 1}, {3152, 1, 1}, {3153, 1, 1}, {3154, 1, 1}, {3155, 1, 1}, {3156, 1, 1},
    {3157, 1, 1}, {3158, 1, 1}, {3159, 1, 1}, {3160, 1, 1}, {3161, 1, 1}, {3162, 1, 1}, {3163, 1, 1}, {3164, 1, 1},
    {3165, 1, 1}, {3166, 1, 1}, {3167, 1, 1}, {3168, 1, 1}, {3169, 1, 1}, {3170, 1, 1}, {3171, 1, 1}, {3172, 1, 1},
    {3173, 1, 1}, {3174, 1, 1}, {3175, 1, 1}, {3176, 1, 1}, {3177, 1, 1}, {3178, 1, 1}, {3179, 1, 1}, {3180, 1, 1},
    {3181, 1, 1}, {3182, 1, 1}, {3183, 1, 1}, {3184, 1, 1}, {3185, 1, 1}, {3186, 1, 1}, {3187, 1, 1}, {3188, 1, 1},
    {3189, 1, 1}, {3190, 1, 1}, {3191, 1, 1}, {3192, 1, 1}, {3193, 1, 1}, {3194, 1, 1}, {3195, 1, 1}, {3196, 1, 1},
    {3197, 1, 1}, {3198, 1, 1}, {3199, 1, 1}, {3200, 1, 1}, {3201, 1, 1}, {3202, 1, 1}, {3203, 1, 1}, {3204, 1, 1},
    {3205, 1, 1}, {3206, 1, 1}, {3207, 1, 1}, {3208, 1, 1}, {3209, 1, 1}, {3210, 1, 1}, {3211, 1, 1}, {3212, 1, 1},
    {3213, 1, 1}, {3214, 1, 1}, {3215, 1, 1}, {3216, 1, 1}, {3217, 1, 1}, {3218, 1, 1}, {3219, 1, 1}, {3220, 1, 1},
    {3221, 1, 1}, {3222, 1, 1}, {3223, 1, 1}, {3224, 1, 1}, {3225, 1, 1}, {3226, 1, 1}, {3227, 1, 1}, {3228, 1, 1},
    {3229, 1, 1}, {3230, 1, 1}, {3231, 1, 1}, {3232, 1, 1}, {3233, 1, 1}, {3234, 1, 1}, {3235, 1, 1}, {3236, 1, 1},
    {3237, 1, 1}, {3238, 1, 1}, {3239, 1, 1}, {3240, 1, 1}, {3241, 1, 1}, {3242, 1, 1}, {3243, 1, 1}, {3244, 1, 1},
    {3245, 1, 1}, {3246, 1, 1}, {3247, 1, 1}, {3248, 1, 1}, {3249, 1, 1}, {3250, 1, 1}, {3251, 1, 1}, {3252, 1, 1},
    {3253, 1, 1}, {3254, 1, 1}, {3255, 1, 1}, {3256, 1, 1}, {3257, 1, 1}, {3258, 1, 1}, {3259, 1, 1}, {3260, 1, 1},
    {3261, 1, 1}, {3262, 1, 1}, {3263, 1, 1}, {3264, 1, 1}, {3265, 1, 1}, {3266, 1, 1}, {3267, 1, 1}, {3268, 1, 1},
    {3269, 1, 1}, {3270, 1, 1}, {3271, 1, 1}, {3272, 1, 1}, {3273, 1, 1}, {3274, 1, 1}, {3275, 1, 1}, {3276, 1, 1},
    {3277, 1, 1}, {3278, 1, 1}, {3279, 1, 1}, {3280, 1, 1}, {3281, 1, 1}, {3282, 1, 1}, {3283, 1, 1}, {3284, 1, 1},
    {3285, 1, 1}, {3286, 1, 1}, {3287, 1, 1}, {3288, 1, 1}, {3289, 1, 1}, {3290, 1, 1}, {3291, 1, 1}, {3292, 1, 1},
    {3293, 1, 1}, {3294, 1, 1}, {3295, 1, 1}, {3296, 1, 1}, {3297, 1, 1}, {3298, 1, 1}, {3299, 1, 1}, {3300, 1, 1},
    {3301, 1, 1}, {3302, 1, 1}, {3303, 1, 1}, {3304, 1, 1}, {3305, 1, 1}, {3306, 1, 1}, {3307, 1, 1}, {3308, 1, 1},
    {3309, 1, 1}, {3310, 1, 1}, {3311, 1, 1}, {3312, 1, 1}, {3313, 1, 1}, {3314, 1, 1}, {3315, 1, 1}, {3316, 1, 1},
    {3317, 1, 1}, {3318, 1, 1}, {3319, 1, 1}, {3320, 1, 1}, {3321, 1, 1}, {3322, 1, 1}, {3323, 1, 1}, {3324, 1, 1},
    {3325, 1, 1}, {3326, 1, 1}, {3327, 1, 1}, {3328, 1, 1}, {3329, 1, 1}, {3330, 1, 1}, {3331, 1, 1}, {3332, 1, 1},
    {3333, 1, 1}, {3334, 1, 1}, {3335, 1, 1}, {3336, 1, 1}, {3337, 1, 1}, {3338, 1, 1}, {3339, 1, 1}, {3340, 1, 1},
    {3341, 1, 1}, {3342, 1, 1}, {3343, 1, 1}, {3344, 1, 1}, {3345, 1, 1}, {3346, 1, 1}, {3347, 1, 1}, {3348, 1, 1},
    {3349, 1, 1}, {3350, 1, 1}, {3351, 1, 1}, {3352, 1, 1}, {3353, 1, 1}, {3354, 1, 1}, {3355, 1, 1}, {3356, 1, 1},
    {3357, 1, 1}, {3358, 1, 1}, {3359, 1, 1}, {3360, 1, 1}, {3361, 1, 1}, {3362, 1, 1}, {3363, 1, 1}, {3364, 1, 1},
    {3365, 1, 1}, {3366, 1, 1}, {3367, 1, 1}, {3368, 1, 1}, {3369, 1, 1}, {3370, 1, 1}, {3371, 1, 1}, {3372, 1, 1},
    {3373, 1, 1}, {3374, 1, 1}, {3375, 1, 1}, {3376, 1, 1}, {3377, 1, 1}, {3378, 1, 1}, {3379, 1, 1}, {3380, 1, 1},
    {3381, 1, 1}, {3382, 1, 1}, {3383, 1, 1}, {3384, 1, 1}, {3385, 1, 1}, {3386, 1, 1}, {3387, 1, 1}, {3388, 1, 1},
    {3389, 1, 1}, {3390, 1, 1}, {3391, 1, 1}, {3392, 1, 1}, {3393, 1, 1}, {3394, 1, 1}, {3395, 1, 1}, {3396, 1, 1},
    {3397, 1, 1}, {3398, 1, 1}, {3399, 1, 1}, {3400, 1, 1}, {3401, 1, 1}, {3402, 1, 1}, {3403, 1, 1}, {3404, 1, 1},
    {3405, 1, 1}, {3406, 1, 1}, {3407, 1, 1}, {3408, 1, 1}, {3409, 1, 1}, {3410, 1, 1}, {3411, 1, 1}, {3412, 1, 1},
    {3413, 1, 1}, {3414, 1, 1}, {3415, 1, 1}, {3416, 1, 1}, {3417, 1, 1}, {3418, 1, 1}, {3419, 1, 1}, {3420, 1, 1},
    {3421, 1, 1}, {3422, 1, 1}, {3423, 1, 1}, {3424, 1, 1}, {3425, 1, 1}, {3426, 1, 1}, {3427, 1, 1}, {3428, 1, 1},
    {3429, 1, 1}, {3430, 1, 1}, {3431, 1, 1}, {3432, 1, 1}, {3433, 1, 1}, {3434, 1, 1}, {3435, 1, 1}, {3436, 1, 1},
    {3437, 1, 1}, {3438, 1, 1}, {3439, 1, 1}, {3440, 1, 1}, {3441, 1, 1}, {3442, 1, 1}, {3443, 1, 1}, {3444, 1, 1},
    {3445, 1, 1}, {3446, 1, 1}, {3447, 1, 1}, {3448, 1, 1}, {3449, 1, 1}, {3450, 1, 1}, {3451, 1, 1}, {3452, 1, 1},
    {3453, 1, 1}, {3454, 1, 1}, {3455, 1, 1}, {3456, 1, 1}, {3457, 1, 1}, {3458, 1, 1}, {3459, 1, 1}, {3460, 1, 1},
    {3461, 1, 1}, {3462, 1, 1}, {3463, 1, 1}, {3464, 1, 1}, {3465, 1, 1}, {3466, 1, 1}, {3467, 1, 1}, {3468, 1, 1},
    {3469, 1, 1}, {3470, 1, 1}, {3471, 1, 1}, {3472, 1, 1}, {3473, 1, 1}, {3474, 1, 1}, {3475, 1, 1}, {3476, 1, 1},
    {3477, 1, 1}, {3478, 1, 1}, {3479, 1, 1}, {3480, 1, 1}, {3481, 1, 1}, {3482, 1, 1}, {3483, 1, 1}, {3484, 1, 1},
    {3485, 1, 1}, {3486, 1, 1}, {3487, 1, 1}, {3488, 1, 1}, {3489, 1, 1}, {3490, 1, 1}, {3491, 1, 1}, {3492, 1, 1},
    {3493, 1, 1}, {3494, 1, 1}, {3495, 1, 1}, {3496, 1, 1}, {3497, 1, 1}, {3498, 1, 1}, {3499, 1, 1}, {3500, 1, 1},
    {3501, 1, 1}, {3502, 1, 1}, {3503, 1, 1}, {3504, 1, 1}, {3505, 1, 1}, {3506, 1, 1}, {3507, 1, 1}, {3508, 1, 1},
    {3509, 1, 1}, {3510, 1, 1}, {3511, 1, 1}, {3512, 1, 1}, {3513, 1, 1}, {3514, 1, 1}, {3515, 1, 1}, {3516, 1, 1},
    {3517, 1, 1}, {3518, 1, 1}, {3519, 1, 1}, {3520, 1, 1}, {3521, 1, 1}, {3522, 1, 1}, {3523, 1, 1}, {3524, 1, 1},
    {3525, 1, 1}, {3526, 1, 1}, {3527, 1, 1}, {3528, 1, 1}, {3529, 1, 1}, {3530, 1, 1}, {3531, 1, 1}, {3532, 1, 1},
    {3533, 1, 1}, {3534, 1, 1}, {3535, 1, 1}, {3536, 1, 1}, {3537, 1, 1}, {3538, 1, 1}, {3539, 1, 1}, {3540, 1, 1},
    {3541, 1, 1}, {3542, 1, 1}, {3543, 1, 1}, {3544, 1, 1}, {3545, 1, 1}, {3546, 1, 1}, {3547, 1, 1}, {3548, 1, 1},
    {3549, 1, 1}, {3550, 1, 1}, {3551, 1, 1}, {3552, 1, 1}, {3553, 1, 1}, {3554, 1, 1}, {3555, 1, 1}, {3556, 1, 1},
    {3557, 1, 1}, {3558, 1, 1}, {3559, 1, 1}, {3560, 1, 1}, {3561, 1, 1}, {3562, 1, 1}, {3563, 1, 1}, {3564, 1, 1},
    {3565, 1, 1}, {3566, 1, 1}, {3567, 1, 1}, {3568, 1, 1}, {3569, 1, 1}, {3570, 1, 1}, {3571, 1, 1}, {3572, 1, 1},
    {3573, 1, 1}, {3574, 1, 1}, {3575, 1, 1}, {3576, 1, 1}, {3577, 1, 1}, {3578, 1, 1}, {3579, 1, 1}, {3580, 1, 1},
    {3581, 1, 1}, {3582, 1, 1}, {3583, 1, 1}, {3584, 1, 1}, {3585, 1, 1}, {3586, 1, 1}, {3587, 1, 1}, {3588, 1, 1},
    {3589, 1, 1}, {3590, 1, 1}, {3591, 1, 1}, {3592, 1, 1}, {3593, 1, 1}, {3594, 1, 1}, {3595, 1, 1}, {3596, 1, 1},
    {3597, 1, 1}, {3598, 1, 1}, {3599, 1, 1}, {3600, 1, 1}, {3601, 1, 1}, {3602, 1, 1}, {3603, 1, 1}, {3604, 1, 1},
    {3605, 1, 1}, {3606, 1, 1}, {3607, 1, 1}, {3608, 1, 1}, {3609, 1, 1}, {3610, 1, 1}, {3611, 1, 1}, {3612, 1, 1},
    {3613, 1, 1}, {3614, 1, 1}, {3615, 1, 1}, {3616, 1, 1}, {3617, 1, 1}, {3618, 1, 1}, {3619, 1, 1}, {3620, 1, 1},
    {3621, 1, 1}, {3622, 1, 1}, {3623, 1, 1}, {3624, 1, 1}, {3625, 1, 1}, {3626, 1, 1}, {3627, 1, 1}, {3628, 1, 1},
    {3629, 1, 1}, {3630, 1, 1}, {3631, 1, 1}, {3632, 1, 1}, {3633, 1, 1}, {3634, 1, 1}, {3635, 1, 1}, {3636, 1, 1},
    {3637, 1, 1}, {3638, 1, 1}, {3639, 1, 1}, {3640, 1, 1}, {3641, 1, 1}, {3642, 1, 1}, {3643, 1, 1}, {3644, 1, 1},
    {3645, 1, 1}, {3646, 1, 1}, {3647, 1, 1}, {3648, 1, 1}, {3649, 1, 1}, {3650, 1, 1}, {3651, 1, 1}, {3652, 1, 1},
    {3653, 1, 1}, {3654, 1, 1}, {3655, 1, 1}, {3656, 1, 1}, {3657, 1, 1}, {3658, 1, 1}, {3659, 1, 1}, {3660, 1, 1},
    {3661, 1, 1}, {3662, 1, 1}, {3663, 1, 1}, {3664, 1, 1}, {3665, 1, 1}, {3666, 1, 1}, {3667, 1, 1}, {3668, 1, 1},
    {3669, 1, 1}, {3670, 1, 1}, {3671, 1, 1}, {3672, 1, 1}, {3673, 1, 1}, {3674, 1, 1}, {3675, 1, 1}, {3676, 1, 1},
    {3677, 1, 1}, {3678, 1, 1}, {3679, 1, 1}, {3680, 1, 1}, {3681, 1, 1}, {3682, 1, 1}, {3683, 1, 1}, {3684, 1, 1},
    {3685, 1, 1}, {3686, 1, 1}, {3687, 1, 1}, {3688, 1, 1}, {3689, 1, 1}, {3690, 1, 1}, {3691, 1, 1}, {3692, 1, 1},
    {3693, 1, 1}, {3694, 1, 1}, {3695, 1, 1}, {3696, 1, 1}, {3697, 1, 1}, {3698, 1, 1}, {3699, 1, 1}, {3700, 1, 1},
    {3701, 1, 1}, {3702, 1, 1}, {3703, 1, 1}, {3704, 1, 1}, {3705, 1, 1}, {3706, 1, 1}, {3707, 1, 1}, {3708, 1, 1},
    {3709, 1, 1}, {3710, 1, 1}, {3711, 1, 1}, {3712, 1, 1}, {3713, 1, 1}, {3714, 1, 1}, {3715, 1, 1}, {3716, 1, 1},
    {3717, 1, 1}, {3718, 1, 1}, {3719, 1, 1}, {3720, 1, 1}, {3721, 1, 1}, {3722, 1, 1}, {3723, 1, 1}, {3724, 1, 1},
    {3725, 1, 1}, {3726, 1, 1}, {3727, 1, 1}, {3728, 1, 1}, {3729, 1, 1}, {3730, 1, 1}, {3731, 1, 1}, {3732, 1, 1},
    {3733, 1, 1}, {3734, 1, 1}, {3735, 1, 1}, {3736, 1, 1}, {3737, 1, 1}, {3738, 1, 1}, {3739, 1, 1}, {3740, 1, 1},
    {3741, 1, 1}, {3742, 1, 1}, {3743, 1, 1}, {3744, 1, 1}, {3745, 1, 1}, {3746, 1, 1}, {3747, 1, 1}, {3748, 1, 1},
    {3749, 1, 1}, {3750, 1, 1}, {3751, 1, 1}, {3752, 1, 1}, {3753, 1, 1}, {3754, 1, 1}, {3755, 1, 1}, {3756, 1, 1},
    {3757, 1, 1}, {3758, 1, 1}, {3759, 1, 1}, {3760, 1, 1}, {3761, 1, 1}, {3762, 1, 1}, {3763, 1, 1}, {3764, 1, 1},
    {3765, 1, 1}, {3766, 1, 1}, {3767, 1, 1}, {3768, 1, 1}, {3769, 1, 1}, {3770, 1, 1}, {3771, 1, 1}, {3772, 1, 1},
    {3773, 1, 1}, {3774, 1, 1}, {3775, 1, 1}, {3776, 1, 1}, {3777, 1, 1}, {3778, 1, 1}, {4155, 1, 1}, {4156, 1, 1},
    {4157, 1, 1}, {4158, 1, 1}, {4159, 1, 1}, {4160, 1, 1}, {4161, 1, 1}, {4162, 1, 1}, {4163, 1, 1}, {4164, 1, 1},
    {4165, 1, 1}, {4166, 1, 1}, {4167, 1, 1}, {4168, 1, 1}, {4169, 1, 1}, {4170, 1, 1}, {4171, 1, 1}, {4172, 1, 1},
    {4173, 1, 1}, {4174, 1, 1}, {4175, 1, 1}, {4176, 1, 1}, {4177, 1, 1}, {4178, 1, 1}, {4179, 1, 1}, {4180, 1, 1},
    {4181, 1, 1}, {4182, 1, 1}, {4183, 1, 1}, {4184, 1, 1}, {4185, 1, 1}, {4186, 1, 1}, {4187, 1, 1}, {4188, 1, 1},
    {4189, 1, 1}, {4190, 1, 1}, {4191, 1, 1}, {4192, 1, 1}, {4193, 1, 1}, {4194, 1, 1}, {4195, 1, 1}, {4196, 1, 1},
    {4197, 1, 1}, {4198, 1, 1}, {4199, 1, 1}, {4200, 1, 1}, {4201, 1, 1}, {4202, 1, 1}, {4203, 1, 1}, {4204, 1, 1},
    {4205, 1, 1}, {4206, 1, 1}, {4207, 1, 1}, {4208, 1, 1}, {4209, 1, 1}, {4210, 1, 1}, {4211, 1, 1}, {4212, 1, 1},
    {4213, 1, 1}, {4214, 1, 1}, {4215, 1, 1}, {4216, 1, 1}, {4217, 1, 1}, {4218, 1, 1}, {4219, 1, 1}, {4220, 1, 1},
    {4221, 1, 1}, {4222, 1, 1}, {4223, 1, 1}, {4224, 1, 1}, {4225, 1, 1}, {4226, 1, 1}, {4227, 1, 1}, {4228, 1, 1},
    {4229, 1, 1}, {4230, 1, 1}, {4231, 1, 1}, {4232, 1, 1}, {4233, 1, 1}, {4234, 1, 1}, {4235, 1, 1}, {4236, 1, 1},
    {4237, 1, 1}, {4238, 1, 1}, {4239, 1, 1}, {4240, 1, 1}, {4241, 1, 1}, {4242, 1, 1}, {4243, 1, 1}, {4244, 1, 1},
    {4245, 1, 1}, {4246, 1, 1}, {4247, 1, 1}, {4248, 1, 1}, {4249, 1, 1}, {4250, 1, 1}, {4251, 1, 1}, {4252, 1, 1},
    {4253, 1, 1}, {4254, 1, 1}, {4255, 1, 1}, {4256, 1, 1}, {4257, 1, 1}, {4258, 1, 1}, {4259, 1, 1}, {4260, 1, 1},
    {4261, 1, 1}, {4262, 1, 1}, {4263, 1, 1}, {4264, 1, 1}, {4265, 1, 1}, {4266, 1, 1}, {4267, 1, 1}, {4268, 1, 1},
    {4269, 1, 1}, {4270, 1, 1}, {4271, 1, 1}, {4272, 1, 1}, {4273, 1, 1}, {4274, 1, 1}, {4275, 1, 1}, {4276, 1, 1},
    {4277, 1, 1}, {4278, 1, 1}, {4279, 1, 1}, {4280, 1, 1}, {4281, 1, 1}, {4282, 1, 1}, {4283, 1, 1}, {4284, 1, 1},
    {4285, 1, 1}, {4286, 1, 1}, {4287, 1, 1}, {4288, 1, 1}, {4289, 1, 1}, {4290, 1, 1}, {4291, 1, 1}, {4292, 1, 1},
    {4293, 1, 1}, {4294, 1, 1}, {4295, 1, 1}, {4296, 1, 1}, {4297, 1, 1}, {4298, 1, 1}, {4299, 1, 1}, {4300, 1, 1},
    {4301, 1, 1}, {4302, 1, 1}, {4303, 1, 1}, {4304, 1, 1}, {4305, 1, 1}, {4306, 1, 1}, {4307, 1, 1}, {4308, 1, 1},
    {4309, 1, 1}, {4310, 1, 1}, {4311, 1, 1}, {4312, 1, 1}, {4313, 1, 1}, {4314, 1, 1}, {4315, 1, 1}, {4316, 1, 1},
    {4317, 1, 1}, {4318, 1, 1}, {4319, 1, 1}, {4320, 1, 1}, {4321, 1, 1}, {4322, 1, 1}, {4323, 1, 1}, {4324, 1, 1},
    {4325, 1, 1}, {4326, 1, 1}, {4327, 1, 1}, {4328, 1, 1}, {4329, 1, 1}, {4330, 1, 1}, {4331, 1, 1}, {4332, 1, 1},
    {4333, 1, 1}, {4334, 1, 1}, {4335, 1, 1}, {4336, 1, 1}, {4337, 1, 1}, {4338, 1, 1}, {4339, 1, 1}, {4340, 1, 1},
    {4341, 1, 1}, {4342, 1, 1}, {4343, 1, 1}, {4344, 1, 1}, {4345, 1, 1}, {4346, 1, 1}, {4347, 1, 1}, {4348, 1, 1},
    {4349, 1, 1}, {4350, 1, 1}, {4351, 1, 1}, {4352, 1, 1}, {4353, 1, 1}, {4354, 1, 1}, {4355, 1, 1}, {4356, 1, 1},
    {4357, 1, 1}, {4358, 1, 1}, {4359, 1, 1}, {4360, 1, 1}, {4361, 1, 1}, {4362, 1, 1}, {4363, 1, 1}, {4364, 1, 1},
    {4365, 1, 1}, {4366, 1, 1}, {4367, 1, 1}, {4368, 1, 1}, {4369, 1, 1}, {4370, 1, 1}, {4371, 1, 1}, {6349, 1, 1},
    {6350, 1, 1}, {6351, 1, 1}, {4372, 1, 1}, {4373, 1, 1}, {4374, 1, 1}, {4375, 1, 1}, {4376, 1, 1}, {4377, 1, 1},
    {4378, 1, 1}, {4379, 1, 1}, {4380, 1, 1}, {4381, 1, 1}, {4382, 1, 1}, {4383, 1, 1}, {4384, 1, 1}, {4385, 1, 1},
    {4386, 1, 1}, {4387, 1, 1}, {4388, 1, 1}, {6352, 1, 1}, {6353, 1, 1}, {6354, 1, 1}, {4389, 1, 1}, {4390, 1, 1},
    {4391, 1, 1}, {4392, 1, 1}, {4393, 1, 1}, {4394, 1, 1}, {4395, 1, 1}, {4396, 1, 1}, {4397, 1, 1}, {4398, 1, 1},
    {4399, 1, 1}, {4400, 1, 1}, {4401, 1, 1}, {6355, 1, 1}, {6356, 1, 1}, {6357, 1, 1}, {4402, 1, 1}, {4403, 1, 1},
    {4404, 1, 1}, {4405, 1, 1}, {4406, 1, 1}, {4407, 1, 1}, {4408, 1, 1}, {4409, 1, 1}, {4410, 1, 1}, {4411, 1, 1},
    {4412, 1, 1}, {4413, 1, 1}, {4414, 1, 1}, {4415, 1, 1}, {4416, 1, 1}, {4417, 1, 1}, {4418, 1, 1}, {4419, 1, 1},
    {4420, 1, 1}, {4421, 1, 1}, {4422, 1, 1}, {4423, 1, 1}, {4424, 1, 1}, {4425, 1, 1}, {4426, 1, 1}, {4427, 1, 1},
    {4428, 1, 1}, {4429, 1, 1}, {4430, 1, 1}, {4431, 1, 1}, {4432, 1, 1}, {4433, 1, 1}, {4434, 1, 1}, {4435, 1, 1},
    {4436, 1, 1}, {4437, 1, 1}, {4438, 1, 1}, {4439, 1, 1}, {4440, 1, 1}, {4441, 1, 1}, {4442, 1, 1}, {4443, 1, 1},
    {4444, 1, 1}, {4445, 1, 1}, {4446, 1, 1}, {4447, 1, 1}, {4448, 1, 1}, {4449, 1, 1}, {4450, 1, 1}, {4451, 1, 1},
    {4452, 1, 1}, {4453, 1, 1}, {4454, 1, 1}, {4455, 1, 1}, {4456, 1, 1}, {4457, 1, 1}, {4458, 1, 1}, {4459, 1, 1},
    {4460, 1, 1}, {4461, 1, 1}, {4462, 1, 1}, {4463, 1, 1}, {4464, 1, 1}, {4465, 1, 1}, {4466, 1, 1}, {4467, 1, 1},
    {4468, 1, 1}, {4469, 1, 1}, {4470, 1, 1}, {4471, 1, 1}, {4472, 1, 1}, {4473, 1, 1}, {4474, 1, 1}, {4475, 1, 1},
    {4476, 1, 1}, {4477, 1, 1}, {4478, 1, 1}, {4479, 1, 1}, {4480, 1, 1}, {4481, 1, 1}, {4482, 1, 1}, {4483, 1, 1},
    {4484, 1, 1}, {4485, 1, 1}, {4486, 1, 1}, {4487, 1, 1}, {4488, 1, 1}, {4489, 1, 1}, {4490, 1, 1}, {4491, 1, 1},
    {4492, 1, 1}, {4493, 1, 1}, {4494, 1, 1}, {4495, 1, 1}, {4496, 1, 1}, {4497, 1, 1}, {4498, 1, 1}, {4499, 1, 1},
    {4500, 1, 1}, {4501, 1, 1}, {4502, 1, 1}, {4503, 1, 1}, {4504, 1, 1}, {4505, 1, 1}, {4506, 1, 1}, {4507, 1, 1},
    {4508, 1, 1}, {4509, 1, 1}, {4510, 1, 1}, {4511, 1, 1}, {4512, 1, 1}, {4513, 1, 1}, {4514, 1, 1}, {4515, 1, 1},
    {4516, 1, 1}, {4517, 1, 1}, {4518, 1, 1}, {4519, 1, 1}, {4520, 1, 1}, {4521, 1, 1}, {4522, 1, 1}, {4523, 1, 1},
    {4524, 1, 1}, {4525, 1, 1}, {4526, 1, 1}, {4527, 1, 1}, {4528, 1, 1}, {4529, 1, 1}, {4530, 1, 1}, {4531, 1, 1},
    {4532, 1, 1}, {4533, 1, 1}, {4534, 1, 1}, {4535, 1, 1}, {4536, 1, 1}, {4537, 1, 1}, {4538, 1, 1}, {4539, 1, 1},
    {4540, 1, 1}, {4541, 1, 1}, {4542, 1, 1}, {4543, 1, 1}, {4544, 1, 1}, {4545, 1, 1}, {4546, 1, 1}, {4547, 1, 1},
    {4548, 1, 1}, {4549, 1, 1}, {4550, 1, 1}, {4551, 1, 1}, {4552, 1, 1}, {4553, 1, 1}, {4554, 1, 1}, {4555, 1, 1},
    {4556, 1, 1}, {4557, 1, 1}, {4558, 1, 1}, {4559, 1, 1}, {4560, 1, 1}, {4561, 1, 1}, {4562, 1, 1}, {4563, 1, 1},
    {4564, 1, 1}, {4565, 1, 1}, {4566, 1, 1}, {4567, 1, 1}, {4568, 1, 1}, {4569, 1, 1}, {4570, 1, 1}, {4571, 1, 1},
    {4572, 1, 1}, {4573, 1, 1}, {4574, 1, 1}, {4575, 1, 1}, {4576, 1, 1}, {4577, 1, 1}, {4578, 1, 1}, {4579, 1, 1},
    {4580, 1, 1}, {4581, 1, 1}, {4582, 1, 1}, {4583, 1, 1}, {4584, 1, 1}, {4585, 1, 1}, {4586, 1, 1}, {4587, 1, 1},
    {4588, 1, 1}, {4589, 1, 1}, {4590, 1, 1}, {4591, 1, 1}, {4592, 1, 1}, {4593, 1, 1}, {4594, 1, 1}, {4595, 1, 1},
    {4596, 1, 1}, {4597, 1, 1}, {4598, 1, 1}, {4599, 1, 1}, {4600, 1, 1}, {4601, 1, 1}, {4602, 1, 1}, {4603, 1, 1},
    {4604, 1, 1}, {4605, 1, 1}, {4606, 1, 1}, {4607, 1, 1}, {4608, 1, 1}, {4609, 1, 1}, {4610, 1, 1}, {4611, 1, 1},
    {4612, 1, 1}, {4613, 1, 1}, {4614, 1, 1}, {4615, 1, 1}, {4616, 1, 1}, {4617, 1, 1}, {4618, 1, 1}, {4619, 1, 1},
    {6358, 1, 1}, {6359, 1, 1}, {6360, 1, 1}, {6361, 1, 1}, {6362, 1, 1}, {6363, 1, 1}, {4620, 1, 1}, {4621, 1, 1},
    {4622, 1, 1}, {4623, 1, 1}, {4624, 1, 1}, {4625, 1, 1}, {4626, 1, 1}, {4627, 1, 1}, {4628, 1, 1}, {4629, 1, 1},
    {4630, 1, 1}, {4631, 1, 1}, {6364, 1, 1}, {6365, 1, 1}, {6366, 1, 1}, {6367, 1, 1}, {4632, 1, 1}, {6368, 1, 1},
    {6369, 1, 1}, {6370, 1, 1}, {6371, 1, 1}, {6372, 1, 1}, {6373, 1, 1}, {6374, 1, 1}, {6375, 1, 1}, {6376, 1, 1},
    {6377, 1, 1}, {6378, 1, 1}, {6379, 1, 1}, {6380, 1, 1}, {6381, 1, 1}, {6382, 1, 1}, {4633, 1, 1}, {4634, 1, 1},
    {4635, 1, 1}, {4636, 1, 1}, {4637, 1, 1}, {4638, 1, 1}, {4639, 1, 1}, {4640, 1, 1}, {4641, 1, 1}, {4642, 1, 1},
    {4643, 1, 1}, {4644, 1, 1}, {6383, 1, 1}, {6384, 1, 1}, {6385, 1, 1}, {6386, 1, 1}, {4645, 1, 1}, {4646, 1, 1},
    {4647, 1, 1}, {4648, 1, 1}, {4649, 1, 1}, {4650, 1, 1}, {4651, 1, 1}, {4652, 1, 1}, {4653, 1, 1}, {4654, 1, 1},
    {4655, 1, 1}, {4656, 1, 1}, {4657, 1, 1}, {4658, 1, 1}, {4659, 1, 1}, {4660, 1, 1}, {4661, 1, 1}, {4662, 1, 1},
    {4663, 1, 1}, {4664, 1, 1}, {4665, 1, 1}, {4666, 1, 1}, {4667, 1, 1}, {4668, 1, 1}, {4669, 1, 1}, {4670, 1, 1},
    {4671, 1, 1}, {4672, 1, 1}, {4673, 1, 1}, {4674, 1, 1}, {4675, 1, 1}, {4676, 1, 1}, {4677, 1, 1}, {4678, 1, 1},
    {4679, 1, 1}, {4680, 1, 1}, {4681, 1, 1}, {4682, 1, 1}, {4683, 1, 1}, {4684, 1, 1}, {4685, 1, 1}, {4686, 1, 1},
    {4687, 1, 1}, {4688, 1, 1}, {4689, 1, 1}, {4690, 1, 1}, {4691, 1, 1}, {4692, 1, 1}, {4693, 1, 1}, {4694, 1, 1},
    {4695, 1, 1}, {4696, 1, 1}, {4697, 1, 1}, {4698, 1, 1}, {4699, 1, 1}, {4700, 1, 1}, {6387, 1, 1}, {6388, 1, 1},
    {6389, 1, 1}, {6390, 1, 1}, {6391, 1, 1}, {6392, 1, 1}, {6393, 1, 1}, {6394, 1, 1}, {4701, 1, 1}, {4702, 1, 1},
    {4703, 1, 1}, {4704, 1, 1}, {4705, 1, 1}, {4706, 1, 1}, {4707, 1, 1}, {4708, 1, 1}, {4709, 1, 1}, {4710, 1, 1},
    {6395, 1, 1}, {6396, 1, 1}, {6397, 1, 1}, {6398, 1, 1}, {6399, 1, 1}, {6400, 1, 1}, {4711, 1, 1}, {4712, 1, 1},
    {4713, 1, 1}, {4714, 1, 1}, {4715, 1, 1}, {4716, 1, 1}, {4717, 1, 1}, {4718, 1, 1}, {4719, 1, 1}, {4720, 1, 1},
    {4721, 1, 1}, {4722, 1, 1}, {4723, 1, 1}, {4724, 1, 1}, {4725, 1, 1}, {4726, 1, 1}, {4727, 1, 1}, {4728, 1, 1},
    {4729, 1, 1}, {4730, 1, 1}, {4731, 1, 1}, {4732, 1, 1}, {4733, 1, 1}, {4734, 1, 1}, {4735, 1, 1}, {4736, 1, 1},
    {4737, 1, 1}, {4738, 1, 1}, {4739, 1, 1}, {4740, 1, 1}, {4741, 1, 1}, {4742, 1, 1}, {4743, 1, 1}, {4744, 1, 1},
    {4745, 1, 1}, {4746, 1, 1}, {4747, 1, 1}, {4748, 1, 1}, {4749, 1, 1}, {4750, 1, 1}, {6401, 1, 1}, {6402, 1, 1},
    {6403, 1, 1}, {6404, 1, 1}, {6405, 1, 1}, {6406, 1, 1}, {6407, 1, 1}, {6408, 1, 1}, {4751, 1, 1}, {4752, 1, 1},
    {4753, 1, 1}, {4754, 1, 1}, {4755, 1, 1}, {4756, 1, 1}, {4757, 1, 1}, {4758, 1, 1}, {4759, 1, 1}, {4760, 1, 1},
    {4761, 1, 1}, {4762, 1, 1}, {4763, 1, 1}, {4764, 1, 1}, {4765, 1, 1}, {4766, 1, 1}, {4767, 1, 1}, {4768, 1, 1},
    {4769, 1, 1}, {4770, 1, 1}, {4771, 1, 1}, {4772, 1, 1}, {4773, 1, 1}, {4774, 1, 1}, {4775, 1, 1}, {4776, 1, 1},
    {4777, 1, 1}, {4778, 1, 1}, {4779, 1, 1}, {4780, 1, 1}, {6409, 1, 1}, {6410, 1, 1}, {4781, 1, 1}, {4782, 1, 1},
    {4783, 1, 1}, {4784, 1, 1}, {4785, 1, 1}, {4786, 1, 1}, {4787, 1, 1}, {4788, 1, 1}, {4789, 1, 1}, {4790, 1, 1},
    {4791, 1, 1}, {4792, 1, 1}, {6411, 1, 1}, {6412, 1, 1}, {6413, 1, 1}, {6414, 1, 1}, {4793, 1, 1}, {4794, 1, 1},
    {6415, 1, 1}, {6416, 1, 1}, {6417, 1, 1}, {6418, 1, 1}, {6419, 1, 1}, {6420, 1, 1}, {6421, 1, 1}, {6422, 1, 1},
    {6423, 1, 1}, {6424, 1, 1}, {6425, 1, 1}, {6426, 1, 1}, {6427, 1, 1}, {6428, 1, 1}, {4795, 1, 1}, {4796, 1, 1},
    {4797, 1, 1}, {4798, 1, 1}, {4799, 1, 1}, {4800, 1, 1}, {4801, 1, 1}, {4802, 1, 1}, {4803, 1, 1}, {6429, 1, 1},
    {6430, 1, 1}, {6431, 1, 1}, {6432, 1, 1}, {6433, 1, 1}, {6434, 1, 1}, {6435, 1, 1}, {6436, 1, 1}, {6437, 1, 1},
    {6438, 1, 1}, {6439, 1, 1}, {6440, 1, 1}, {6441, 1, 1}, {6442, 1, 1}, {6443, 1, 1}, {6444, 1, 1}, {6445, 1, 1},
    {6446, 1, 1}, {6447, 1, 1}, {6448, 1, 1}, {6449, 1, 1}, {6450, 1, 1}, {6451, 1, 1}, {6452, 1, 1}, {6453, 1, 1},
    {6454, 1, 1}, {6455, 1, 1}, {6456, 1, 1}, {6457, 1, 1}, {6458, 1, 1}, {6459, 1, 1}, {6460, 1, 1}, {6461, 1, 1},
    {6462, 1, 1}, {6463, 1, 1}, {6464, 1, 1}, {6465, 1, 1}, {6466, 1, 1}, {6467, 1, 1}, {3779, 1, 1}, {3780, 1, 1},
    {3781, 1, 1}, {3782, 1, 1}, {3783, 1, 1}, {3784, 1, 1}, {3785, 1, 1}, {3786, 1, 1}, {3787, 1, 1}, {3788, 1, 1},
    {3789, 1, 1}, {3790, 1, 1}, {3791, 1, 1}, {3792, 1, 1}, {3793, 1, 1}, {3794, 1, 1}, {3795, 1, 1}, {3796, 1, 1},
    {3797, 1, 1}, {3798, 1, 1}, {3799, 1, 1}, {3800, 1, 1}, {3801, 1, 1}, {3802, 1, 1}, {3803, 1, 1}, {3804, 1, 1},
    {3805, 1, 1}, {3806, 1, 1}, {3807, 1, 1}, {3808, 1, 1}, {3809, 1, 1}, {3810, 1, 1}, {3811, 1, 1}, {3812, 1, 1},
    {3813, 1, 1}, {3814, 1, 1}, {3815, 1, 1}, {3816, 1, 1}, {3817, 1, 1}, {3818, 1, 1}, {3819, 1, 1}, {3820, 1, 1},
    {3821, 1, 1}, {3822, 1, 1}, {3823, 1, 1}, {3824, 1, 1}, {3825, 1, 1}, {3826, 1, 1}, {3827, 1, 1}, {3828, 1, 1},
    {3829, 1, 1}, {3830, 1, 1}, {3831, 1, 1}, {3832, 1, 1}, {3833, 1, 1}, {3834, 1, 1}, {3835, 1, 1}, {3836, 1, 1},
    {3837, 1, 1}, {3838, 1, 1}, {3839, 1, 1}, {3840, 1, 1}, {3841, 1, 1}, {3842, 1, 1}, {3843, 1, 1}, {3844, 1, 1},
    {3845, 1, 1}, {3846, 1, 1}, {3847, 1, 1}, {3848, 1, 1}, {3849, 1, 1}, {3850, 1, 1}, {3851, 1, 1}, {3852, 1, 1},
    {3853, 1, 1}, {3854, 1, 1}, {3855, 1, 1}, {3856, 1, 1}, {3857, 1, 1}, {3858, 1, 1}, {3859, 1, 1}, {3860, 1, 1},
    {3861, 1, 1}, {3862, 1, 1}, {3863, 1, 1}, {3864, 1, 1}, {3865, 1, 1}, {3866, 1, 1}, {3867, 1, 1}, {3868, 1, 1},
    {3869, 1, 1}, {3870, 1, 1}, {3871, 1, 1}, {3872, 1, 1}, {3873, 1, 1}, {3874, 1, 1}, {3875, 1, 1}, {3876, 1, 1},
    {3877, 1, 1}, {3878, 1, 1}, {3879, 1, 1}, {3880, 1, 1}, {3881, 1, 1}, {3882, 1, 1}, {3883, 1, 1}, {3884, 1, 1},
    {3885, 1, 1}, {3886, 1, 1}, {3887, 1, 1}, {3888, 1, 1}, {3889, 1, 1}, {3890, 1, 1}, {3891, 1, 1}, {3892, 1, 1},
    {3893, 1, 1}, {3894, 1, 1}, {3895, 1, 1}, {3896, 1, 1}, {3897, 1, 1}, {3898, 1, 1}, {3899, 1, 1}, {3900, 1, 1},
    {3901, 1, 1}, {3902, 1, 1}, {3903, 1, 1}, {3904, 1, 1}, {3905, 1, 1}, {3906, 1, 1}, {3907, 1, 1}, {3908, 1, 1},
    {3909, 1, 1}, {3910, 1, 1}, {3911, 1, 1}, {3912, 1, 1}, {3913, 1, 1}, {3914, 1, 1}, {3915, 1, 1}, {3916, 1, 1},
    {3917, 1, 1}, {3918, 1, 1}, {3919, 1, 1}, {3920, 1, 1}, {3921, 1, 1}, {3922, 1, 1}, {3923, 1, 1}, {3924, 1, 1},
    {3925, 1, 1}, {3926, 1, 1}, {3927, 1, 1}, {3928, 1, 1}, {3929, 1, 1}, {3930, 1, 1}, {3931, 1, 1}, {3932, 1, 1},
    {3933, 1, 1}, {3934, 1, 1}, {3935, 1, 1}, {3936, 1, 1}, {3937, 1, 1}, {3938, 1, 1}, {3939, 1, 1}, {3940, 1, 1},
    {3941, 1, 1}, {3942, 1, 1}, {3943, 1, 1}, {3944, 1, 1}, {3945, 1, 1}, {3946, 1, 1}, {3947, 1, 1}, {3948, 1, 1},
    {3949, 1, 1}, {3950, 1, 1}, {3951, 1, 1}, {3952, 1, 1}, {3953, 1, 1}, {3954, 1, 1}, {3955, 1, 1}, {3956, 1, 1},
    {3957, 1, 1}, {3958, 1, 1}, {3959, 1, 1}, {3960, 1, 1}, {3961, 1, 1}, {3962, 1, 1}, {3963, 1, 1}, {3964, 1, 1},
    {3965, 1, 1}, {3966, 1, 1}, {3967, 1, 1}, {3968, 1, 1}, {3969, 1, 1}, {3970, 1, 1}, {3971, 1, 1}, {3972, 1, 1},
    {3973, 1, 1}, {3974, 1, 1}, {3975, 1, 1}, {3976, 1, 1}, {3977, 1, 1}, {3978, 1, 1}, {3979, 1, 1}, {3980, 1, 1},
    {3981, 1, 1}, {3982, 1, 1}, {3983, 1, 1}, {3984, 1, 1}, {3985, 1, 1}, {3986, 1, 1}, {3987, 1, 1}, {3988, 1, 1},
    {3989, 1, 1}, {3990, 1, 1}, {3991, 1, 1}, {3992, 1, 1}, {3993, 1, 1}, {3994, 1, 1}, {3995, 1, 1}, {3996, 1, 1},
    {3997, 1, 1}, {3998, 1, 1}, {3999, 1, 1}, {4000, 1, 1}, {4001, 1, 1}, {4002, 1, 1}, {4003, 1, 1}, {4004, 1, 1},
    {4005, 1, 1}, {4006, 1, 1}, {4007, 1, 1}, {4008, 1, 1}, {4009, 1, 1}, {4010, 1, 1}, {4011, 1, 1}, {4012, 1, 1},
    {4013, 1, 1}, {4014, 1, 1}, {4015, 1, 1}, {4016, 1, 1}, {4017, 1, 1}, {4018, 1, 1}, {4019, 1, 1}, {4020, 1, 1},
    {4021, 1, 1}, {4022, 1, 1}, {4023, 1, 1}, {4024, 1, 1}, {4025, 1, 1}, {4026, 1, 1}, {4027, 1, 1}, {4028, 1, 1},
    {4029, 1, 1}, {4030, 1, 1}, {4031, 1, 1}, {4032, 1, 1}, {4033, 1, 1}, {4034, 1, 1}, {2897, 1, 1}, {2898, 1, 1},
    {2899, 1, 1}, {2900, 1, 1}, {2901, 1, 1}, {2902, 1, 1}, {2903, 1, 1}, {2904, 1, 1}, {2905, 1, 1}, {2906, 1, 1},
    {2907, 1, 1}, {2908, 1, 1}, {2909, 1, 1}, {2910, 1, 1}, {2911, 1, 1}, {2912, 1, 1}, {2913, 1, 1}, {2914, 1, 1},
    {2915, 1, 1}, {2916, 1, 1}, {2917, 1, 1}, {2918, 1, 1}, {2919, 1, 1}, {2920, 1, 1}, {2921, 1, 1}, {2922, 1, 1},
    {2923, 1, 1}, {2924, 1, 1}, {2925, 1, 1}, {2926, 1, 1}, {2927, 1, 1}, {2928, 1, 1}, {2929, 1, 1}, {2930, 1, 1},
    {2931, 1, 1}, {2932, 1, 1}, {2933, 1, 1}, {2934, 1, 1}, {2935, 1, 1}, {2936, 1, 1}, {2937, 1, 1}, {2938, 1, 1},
    {2939, 1, 1}, {2940, 1, 1}, {2941, 1, 1}, {2942, 1, 1}, {2943, 1, 1}, {2944, 1, 1}, {2945, 1, 1}, {2946, 1, 1},
    {2947, 1, 1}, {2948, 1, 1}, {2949, 1, 1}, {2950, 1, 1}, {2951, 1, 1}, {2952, 1, 1}, {2953, 1, 1}, {2954, 1, 1},
    {2955, 1, 1}, {2956, 1, 1}, {2957, 1, 1}, {2958, 1, 1}, {2959, 1, 1}, {2960, 1, 1}, {2961, 1, 1}, {2962, 1, 1},
    {2963, 1, 1}, {2964, 1, 1}, {2965, 1, 1}, {2966, 1, 1}, {2967, 1, 1}, {2968, 1, 1}, {2969, 1, 1}, {2970, 1, 1},
    {2971, 1, 1}, {2972, 1, 1}, {2973, 1, 1}, {2974, 1, 1}, {2975, 1, 1}, {2976, 1, 1}, {2977, 1, 1}, {2978, 1, 1},
    {2979, 1, 1}, {2980, 1, 1}, {2981, 1, 1}, {2982, 1, 1}, {2983, 1, 1}, {2984, 1, 1}, {6468, 1, 1}, {6469, 1, 1},
    {6470, 1, 1}, {6471, 1, 1}, {6472, 1, 1}, {6473, 1, 1}, {6474, 1, 1}, {6475, 1, 1}, {2985, 1, 1}, {2986, 1, 1},
    {2987, 1, 1}, {2988, 1, 1}, {2989, 1, 1}, {2990, 1, 1}, {2991, 1, 1}, {2992, 1, 1}, {2993, 1, 1}, {2994, 1, 1},
    {2995, 1, 1}, {2996, 1, 1}, {2997, 1, 1}, {2998, 1, 1}, {6476, 1, 1}, {6477, 1, 1}, {4035, 1, 1}, {4036, 1, 1},
    {4037, 1, 1}, {4038, 1, 1}, {4039, 1, 1}, {4040, 1, 1}, {4041, 1, 1}, {4042, 1, 1}, {4043, 1, 1}, {4044, 1, 1},
    {4045, 1, 1}, {4046, 1, 1}, {4047, 1, 1}, {6478, 1, 1}, {6479, 1, 1}, {6480, 1, 1}, {4048, 1, 1}, {4049, 1, 1},
    {4050, 1, 1}, {4051, 1, 1}, {4052, 1, 1}, {4053, 1, 1}, {4054, 1, 1}, {4055, 1, 1}, {4056, 1, 1}, {4057, 1, 1},
    {4058, 1, 1}, {6481, 1, 1}, {6482, 1, 1}, {6483, 1, 1}, {4059, 1, 1}, {4060, 1, 1}, {4061, 1, 1}, {4062, 1, 1},
    {4063, 1, 1}, {4064, 1, 1}, {4065, 1, 1}, {4066, 1, 1}, {4067, 1, 1}, {4068, 1, 1}, {4069, 1, 1}, {4070, 1, 1},
    {4071, 1, 1}, {4072, 1, 1}, {4073, 1, 1}, {4074, 1, 1}, {4075, 1, 1}, {4076, 1, 1}, {4077, 1, 1}, {4078, 1, 1},
    {4079, 1, 1}, {4080, 1, 1}, {4081, 1, 1}, {4082, 1, 1}, {4083, 1, 1}, {4084, 1, 1}, {4085, 1, 1}, {4086, 1, 1},
    {4087, 1, 1}, {4088, 1, 1}, {4089, 1, 1}, {4090, 1, 1}, {4091, 1, 1}, {4092, 1, 1}, {4093, 1, 1}, {4094, 1, 1},
    {4095, 1, 1}, {4096, 1, 1}, {4097, 1, 1}, {4098, 1, 1}, {4099, 1, 1}, {4100, 1, 1}, {4101, 1, 1}, {4102, 1, 1},
    {4103, 1, 1}, {4104, 1, 1}, {4105, 1, 1}, {4106, 1, 1}, {4107, 1, 1}, {4108, 1, 1}, {4109, 1, 1}, {4110, 1, 1},
    {4111, 1, 1}, {4112, 1, 1}, {4113, 1, 1}, {4114, 1, 1}, {4115, 1, 1}, {6484, 1, 1}, {4116, 1, 1}, {6485, 1, 1},
    {6486, 1, 1}, {6487, 1, 1}, {6488, 1, 1}, {4117, 1, 1}, {4118, 1, 1}, {4119, 1, 1}, {4120, 1, 1}, {4121, 1, 1},
    {4122, 1, 1}, {4123, 1, 1}, {4124, 1, 1}, {4125, 1, 1}, {4126, 1, 1}, {4127, 1, 1}, {4128, 1, 1}, {4129, 1, 1},
    {4130, 1, 1}, {4131, 1, 1}, {4132, 1, 1}, {6489, 1, 1}, {6490, 1, 1}, {4133, 1, 1}, {4134, 1, 1}, {4135, 1, 1},
    {4136, 1, 1}, {4137, 1, 1}, {4138, 1, 1}, {4139, 1, 1}, {4140, 1, 1}, {4141, 1, 1}, {4142, 1, 1}, {4143, 1, 1},
    {4144, 1, 1}, {6491, 1, 1}, {6492, 1, 1}, {6493, 1, 1}, {6494, 1, 1}, {4145, 1, 1}, {4146, 1, 1}, {4147, 1, 1},
    {4148, 1, 1}, {4149, 1, 1}, {4150, 1, 1}, {4151, 1, 1}, {4152, 1, 1}, {4153, 1, 1}, {4154, 1, 1}, {6495, 1, 1},
    {6496, 1, 1}, {6497, 1, 1}, {6498, 1, 1}, {6499, 1, 1}, {6500, 1, 1}, {6501, 1, 1},
};

/* A string's collation elements at one level, one weight at a time (0 at the end): a code point
 * the tables don't cover is its own primary, after every covered one. */
typedef struct { const unsigned char *p, *e; const uint16_t (*ce)[3]; uint32_t n, unk, jamo[2], nj; } tv_coll_it;

static uint32_t tv_coll_next(tv_coll_it *it, int level) {
    for (;;) {
        while (it->n) {
            uint32_t w = (*it->ce)[level];
            it->ce++, it->n--;
            if (w) return w;
        }
        if (it->unk) {
            uint32_t cp = it->unk;
            it->unk = 0;
            /* (Han last: after every other script) */
            bool han = (cp >= 0x3400 && cp <= 0x4dbf) || (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0x20000 && cp <= 0x3ffff);
            return level == 0 ? TV_COLL_MAXP + 1 + cp + (han ? 0x110000u : 0) : level == 1 ? TV_COLL_SEC0 : TV_COLL_TER0;
        }
        uint32_t cp;
        if (it->nj) cp = it->jamo[--it->nj];
        else {
            if (it->p >= it->e) return 0;
            it->p += tv_utf8_decode(it->p, (size_t)(it->e - it->p), &cp);
            /* (a Hangul syllable: its jamo, as its canonical decomposition) */
            if (cp >= 0xac00 && cp <= 0xd7a3) {
                uint32_t sy = cp - 0xac00, t = sy % 28;
                it->nj = 0;
                if (t) it->jamo[it->nj++] = 0x11a7 + t;
                it->jamo[it->nj++] = 0x1161 + sy % 588 / 28;
                cp = 0x1100 + sy / 588;
            }
        }
        uint32_t slot = ~0u;
        for (size_t r = 0; r < sizeof tv_coll_ranges / sizeof tv_coll_ranges[0]; r++)
            if (cp < tv_coll_ranges[r].hi) {
                if (cp >= tv_coll_ranges[r].lo) slot = tv_coll_idx[tv_coll_ranges[r].off + cp - tv_coll_ranges[r].lo];
                break;
            }
        if (slot == ~0u) it->unk = cp;
        else it->ce = &tv_coll_ce[slot & 0xffffff], it->n = slot >> 24;
    }
}

static int tv_coll_level(const unsigned char *x, size_t nx, const unsigned char *y, size_t ny, int level) {
    tv_coll_it a = {x, x + nx, NULL, 0, 0, {0, 0}, 0}, b = {y, y + ny, NULL, 0, 0, {0, 0}, 0};
    for (;;) {
        uint32_t u = tv_coll_next(&a, level), v = tv_coll_next(&b, level);
        if (u != v) return u < v ? -1 : 1;
        if (!u) return 0;
    }
}

static __attribute__((noinline)) tv_int tv_coll_compare(const unsigned char *x, size_t nx, const unsigned char *y, size_t ny) {
    for (int level = 0; level < 3; level++) {
        int r = tv_coll_level(x, nx, y, ny, level);
        if (r) return r;
    }
    return 0;
}

/* The primary weight a character starts with, when it has one and is in the tables (0: not
 * known so simply). */
static inline uint32_t tv_coll_first_primary(const unsigned char *p, size_t n) {
    uint32_t cp;
    if (p[0] < 0x80) cp = p[0];
    else tv_utf8_decode(p, n, &cp);
    if (cp >= tv_coll_ranges[0].hi) return 0;
    uint32_t slot = tv_coll_idx[cp];
    return slot != ~0u && slot >> 24 ? tv_coll_ce[slot & 0xffffff][0] : 0;
}

tv_int tv_str_locale_compare(tv_str a, tv_str b) {
    /* (ICU's root collation, as JavaScript's localeCompare without a locale: letters by their base
     * letter first, then accents, then case, lowercase first; see the tables) */
    /* (the same string: equal, as sorting by a field of few values asks again and again) */
    if (a.p == b.p) return 0;
    const unsigned char *x = (const unsigned char *)a.p->data, *y = (const unsigned char *)b.p->data;
    size_t na = (size_t)a.p->len, nb = (size_t)b.p->len, n = na < nb ? na : nb;
    /* (past the common prefix, eight bytes at a time: it weighs the same in both) */
    size_t k = 0;
    while (k + 8 <= n) {
        uint64_t u, v;
        memcpy(&u, x + k, 8);
        memcpy(&v, y + k, 8);
        if (u != v) break;
        k += 8;
    }
    while (k < n && x[k] == y[k]) k++;
    if (k == na && k == nb) return 0;
    while (k > 0 && tv_is_cont(x[k])) k--; /* (from the start of the character that differs) */
    /* (two characters starting with different primary weights: that's the order) */
    if (k < n) {
        uint32_t u = tv_coll_first_primary(x + k, na - k), v = tv_coll_first_primary(y + k, nb - k);
        if (u && v && u != v) return u < v ? -1 : 1;
    }
    return tv_coll_compare(x + k, na - k, y + k, nb - k);
}


static bool tv_is_js_space(uint32_t c) {
    if (c < 0x80) return c == ' ' || (c >= 0x09 && c <= 0x0D);
    return c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

static void tv_trim_range(tv_str s, bool left, bool right, size_t *a_out, size_t *b_out) {
    const unsigned char *d = (const unsigned char *)s.p->data;
    size_t a = 0, b = (size_t)s.p->len;
    uint32_t cp;
    /* (an ASCII byte is space only as ' ' or \t..\r: no decoding) */
    if (left)
        while (a < b) {
            if (d[a] < 0x80) {
                if (d[a] == ' ' || (d[a] >= 9 && d[a] <= 13)) { a++; continue; }
                break;
            }
            size_t l = tv_utf8_decode(d + a, b - a, &cp);
            if (!tv_is_js_space(cp)) break;
            a += l;
        }
    if (right)
        while (b > a) {
            if (d[b - 1] < 0x80) {
                if (d[b - 1] == ' ' || (d[b - 1] >= 9 && d[b - 1] <= 13)) { b--; continue; }
                break;
            }
            size_t st = b - 1;
            while (st > a && tv_is_cont(d[st]) && b - st < 4) st--;
            tv_utf8_decode(d + st, b - st, &cp);
            if (!tv_is_js_space(cp)) break;
            b = st;
        }
    *a_out = a;
    *b_out = b;
}

tv_int tv_str_char_count(tv_str s) {
    const unsigned char *d = (const unsigned char *)s.p->data;
    size_t n = (size_t)s.p->len, count = 0, i = 0;
#if defined(__GNUC__) || defined(__clang__)
    for (; i + 8 <= n; i += 8) { /* count non-continuation bytes, 8 at a time */
        uint64_t w = tv_r64(d + i);
        uint64_t cont = (w & ~(w << 1)) & 0x8080808080808080ULL; /* bit7=1, bit6=0 */
        count += 8 - (size_t)__builtin_popcountll(cont);
    }
#endif
    for (; i < n; i++) count += !tv_is_cont(d[i]);
    return (tv_int)count;
}

/* ------------------------------------------------------------------ UTF-16 view */

static const tv_strbuf tv_fffd_strbuf = {TV_STR_IMMORTAL | TV_STR_WIDE, 3, {(char)0xEF, (char)0xBF, (char)0xBD, 0}};

static inline bool tv_ascii8(const unsigned char *p) { return (tv_r64(p) & 0x8080808080808080ULL) == 0; }

static inline void tv_str_mark(tv_str s, bool ascii) {
    if (s.p->rc > 0) s.p->rc |= ascii ? TV_STR_ASCII : TV_STR_WIDE;
}

__attribute__((noinline, cold)) bool tv_str_classify(tv_strbuf *p) {
    const unsigned char *d = (const unsigned char *)p->data;
    size_t n = (size_t)p->len, i = 0;
    uint64_t any = 0;
    for (; i + 8 <= n; i += 8) any |= tv_r64(d + i);
    for (; i < n; i++) any |= d[i];
    bool ascii = (any & 0x8080808080808080ULL) == 0;
    if (p->rc >= 0) p->rc |= ascii ? TV_STR_ASCII : TV_STR_WIDE;
    return ascii;
}

/* The character starting at d[i] (n bytes in all): its byte length, *units its UTF-16 length. */
static inline size_t tv_char_units(const unsigned char *d, size_t n, size_t i, int64_t *units) {
    size_t l = tv_utf8_len(d[i]);
    if (l > n - i) l = n - i;
    *units = l == 4 ? 2 : 1;
    return l;
}

/* UTF-16 units in d[from, to), both character boundaries. */
static int64_t tv_units_between(const unsigned char *d, size_t n, size_t from, size_t to) {
    size_t i = from;
    int64_t u = 0, k;
    while (i < to) {
        if (i + 8 <= to && tv_ascii8(d + i)) { i += 8; u += 8; continue; }
        i += tv_char_units(d, n, i, &k);
        u += k;
    }
    return u;
}

/* What's known of a few non-ASCII strings: the length (-1 until needed) and the last position
 * converted (a character boundary: its unit and byte index), so walking one index by index,
 * either way, converts each index in a step; and a loop over two strings (an edit distance)
 * keeps both. Freeing a string drops its entry. */
enum { TV_UVIEWS = 4 };
static tv_uview tv_uviews[TV_UVIEWS];
static unsigned tv_uview_next;

tv_uview *tv_uview_last = tv_uviews;

static inline tv_uview *tv_uview_of(const tv_strbuf *p) {
    if (TV_LIKELY(tv_uview_last->p == p)) return tv_uview_last;
    tv_uview *v = NULL;
    for (int i = 0; i < TV_UVIEWS; i++)
        if (tv_uviews[i].p == p) v = &tv_uviews[i];
    if (!v) {
        v = &tv_uviews[tv_uview_next++ % TV_UVIEWS];
        free(v->units);
        *v = (tv_uview){p, -1, 0, 0, NULL, 0};
    }
    tv_uview_last = v;
    return v;
}

static void tv_uview_drop(const tv_strbuf *p) {
    for (int i = 0; i < TV_UVIEWS; i++)
        if (tv_uviews[i].p == p) {
            free(tv_uviews[i].units);
            tv_uviews[i] = (tv_uview){NULL, -1, 0, 0, NULL, 0};
        }
}

tv_int tv_str_wide_length(tv_strbuf *p) {
    tv_uview *v = tv_uview_of(p);
    if (v->len < 0) v->len = tv_units_between((const unsigned char *)p->data, (size_t)p->len, 0, (size_t)p->len);
    return v->len;
}

/* Byte offset of the character holding unit u (0 <= u) of a non-ASCII string (past the end:
 * its end); *mid when u is the second half of a surrogate pair (that character's low half). */
static size_t tv_str_unit_byte(const tv_strbuf *p, int64_t u, bool *mid) {
    const unsigned char *d = (const unsigned char *)p->data;
    size_t n = (size_t)p->len;
    tv_uview *v = tv_uview_of(p);
    int64_t cu = v->u, k;
    size_t b = (size_t)v->b;
    *mid = false;
    if (u < cu && u < cu - u) { cu = 0; b = 0; } /* nearer the start than the cache */
    if (u < cu) {                                /* walk back */
        while (cu > u && b > 0) {
            size_t st = b - 1;
            while (st > 0 && tv_is_cont(d[st]) && b - st < 4) st--;
            tv_char_units(d, n, st, &k);
            b = st;
            cu -= k;
        }
        *mid = cu < u;
    } else {
        while (cu < u && b < n) {
            if (u - cu >= 8 && b + 8 <= n && tv_ascii8(d + b)) { b += 8; cu += 8; continue; }
            size_t l = tv_char_units(d, n, b, &k);
            if (cu + k > u) { *mid = true; break; }
            cu += k;
            b += l;
        }
    }
    v->u = cu;
    v->b = (int64_t)b;
    return b;
}

/* Unit index of byte offset b (a character boundary) of a non-ASCII string. */
static int64_t tv_str_byte_unit(const tv_strbuf *p, size_t b) {
    tv_uview *v = tv_uview_of(p);
    size_t from = 0;
    int64_t u = 0;
    if ((size_t)v->b <= b) { from = (size_t)v->b; u = v->u; }
    u += tv_units_between((const unsigned char *)p->data, (size_t)p->len, from, b);
    v->u = u;
    v->b = (int64_t)b;
    return u;
}

static inline uint32_t tv_decode_at(const tv_strbuf *p, size_t b) {
    uint32_t cp;
    tv_utf8_decode((const unsigned char *)p->data + b, (size_t)p->len - b, &cp);
    return cp;
}

/* A string indexed again and again gets its UTF-16 units (up to 16M of them, 32 MiB), so each
 * index after that is a load, as in a JavaScript engine's two-byte string. */
static bool tv_uview_units(tv_uview *v) {
    if (++v->looks < 64 || v->len > (16 << 20)) return false;
    uint16_t *w = (uint16_t *)tv_alloc((size_t)v->len * 2 + 2), *o = w;
    const unsigned char *d = (const unsigned char *)v->p->data;
    size_t n = (size_t)v->p->len, i = 0;
    while (i < n) {
        uint32_t cp;
        size_t l = tv_utf8_decode(d + i, n - i, &cp);
        if (l == 4) {
            *o++ = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
            *o++ = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            *o++ = (uint16_t)cp;
        }
        i += l;
    }
    v->units = w;
    return true;
}

tv_int tv_str_char_code_at_slow(tv_str s, tv_int i) {
    if (tv_str_is_ascii(s)) return (uint64_t)i < (uint64_t)s.p->len ? (unsigned char)s.p->data[i] : -1;
    tv_uview *v = tv_uview_of(s.p);
    const unsigned char *d = (const unsigned char *)s.p->data;
    if (v->len < 0) v->len = tv_units_between(d, (size_t)s.p->len, 0, (size_t)s.p->len);
    if ((uint64_t)i >= (uint64_t)v->len) return -1;
    if (v->units || tv_uview_units(v)) return v->units[i];
    if (i == v->u + 1 && d[v->b] < 0x80) { /* the next unit after a one-byte character */
        v->u = i;
        v->b++;
        if (d[v->b] < 0x80) return d[v->b];
    }
    bool mid;
    uint32_t cp = tv_decode_at(s.p, tv_str_unit_byte(s.p, i, &mid));
    if (cp < 0x10000) return cp;
    return mid ? 0xDC00 + ((cp - 0x10000) & 0x3FF) : 0xD800 + ((cp - 0x10000) >> 10);
}

tv_int tv_str_code_point_at(tv_str s, tv_int i) {
    if (tv_str_is_ascii(s)) return (uint64_t)i < (uint64_t)s.p->len ? (unsigned char)s.p->data[i] : -1;
    if ((uint64_t)i >= (uint64_t)tv_str_wide_length(s.p)) return -1;
    bool mid;
    uint32_t cp = tv_decode_at(s.p, tv_str_unit_byte(s.p, i, &mid));
    return mid ? 0xDC00 + ((cp - 0x10000) & 0x3FF) : cp;
}

/* Bytes [*ba, *bb) for units [a, b) (0 <= a < b <= length) of s; *pre / *post when the range
 * starts with a pair's low half / ends with a pair's high half (each becomes U+FFFD). */
static void tv_unit_range(tv_str s, int64_t a, int64_t b, size_t *ba, size_t *bb, bool *pre, bool *post) {
    if (tv_str_is_ascii(s)) { *ba = (size_t)a; *bb = (size_t)b; *pre = *post = false; return; }
    bool mid;
    size_t x = tv_str_unit_byte(s.p, a, &mid);
    *pre = mid;
    if (mid) x += 4;
    size_t y = tv_str_unit_byte(s.p, b, &mid);
    *post = mid;
    if (y < x) y = x; /* (units a, a + 1 of one pair: its low half only) */
    *ba = x;
    *bb = y;
}

/* Units [a, b) of s (clamped already) as a string. */
static tv_str tv_str_units(tv_str s, int64_t a, int64_t b) {
    if (a >= b) return TV_EMPTY_STR;
    size_t x, y;
    bool pre, post;
    tv_unit_range(s, a, b, &x, &y, &pre, &post);
    if (!pre && !post) {
        if (x == 0 && y == (size_t)s.p->len) { tv_str_retain(s); return s; }
        tv_str r = tv_str_from(s.p->data + x, y - x);
        if (s.p->rc & TV_STR_ASCII) tv_str_mark(r, true);
        return r;
    }
    if (b - a == 1) return (tv_str){(tv_strbuf *)&tv_fffd_strbuf};
    tv_sb sb = {0};
    if (pre) tv_sb_push(&sb, "\xEF\xBF\xBD", 3);
    tv_sb_push(&sb, s.p->data + x, y - x);
    if (post) tv_sb_push(&sb, "\xEF\xBF\xBD", 3);
    return tv_str_from_sb(&sb);
}

tv_str tv_str_unit_at_slow(tv_str s, tv_int i) {
    if (tv_str_is_ascii(s)) {
        unsigned char c = (unsigned char)s.p->data[i];
        return c - 32u < 96u ? tv_ascii_str(c) : tv_str_from(s.p->data + i, 1);
    }
    return tv_str_units(s, i, i + 1);
}

tv_str tv_str_char_at(tv_str s, tv_int i) {
    if ((uint64_t)i >= (uint64_t)tv_str_length(s)) return TV_EMPTY_STR;
    return tv_str_unit_at(s, i);
}

static void tv_sb_push_utf8(tv_sb *sb, uint32_t cp);

/* Appends UTF-16 unit c, pairing it with a pending high surrogate *hi. */
static void tv_sb_push_unit(tv_sb *sb, uint32_t c, uint32_t *hi) {
    if (*hi) {
        uint32_t h = *hi;
        *hi = 0;
        if (c >= 0xDC00 && c <= 0xDFFF) {
            tv_sb_push_utf8(sb, 0x10000 + ((h - 0xD800) << 10) + (c - 0xDC00));
            return;
        }
        tv_sb_push(sb, "\xEF\xBF\xBD", 3);
    }
    if (c >= 0xD800 && c <= 0xDBFF) { *hi = c; return; }
    if (c >= 0xDC00 && c <= 0xDFFF) c = 0xFFFD;
    tv_sb_push_utf8(sb, c);
}

tv_str tv_str_from_char_codes(const tv_int *c, size_t n) {
    tv_sb sb = {0};
    uint32_t hi = 0;
    for (size_t i = 0; i < n; i++) tv_sb_push_unit(&sb, (uint32_t)c[i] & 0xFFFF, &hi);
    if (hi) tv_sb_push(&sb, "\xEF\xBF\xBD", 3);
    return tv_str_from_sb(&sb);
}

tv_str tv_str_from_char_code(tv_int c) {
    uint32_t u = (uint32_t)c & 0xFFFF;
    if (u - 32u < 96u) return tv_ascii_str((unsigned char)u);
    return tv_str_from_char_codes(&c, 1);
}

tv_str tv_str_from_code_points(const tv_int *c, size_t n, const char *loc) {
    tv_sb sb = {0};
    for (size_t i = 0; i < n; i++) {
        if (c[i] < 0 || c[i] > 0x10FFFF) {
            char msg[80];
            snprintf(msg, sizeof msg, "invalid code point %lld", (long long)c[i]);
            tv_trap(msg, loc);
        }
        uint32_t cp = (uint32_t)c[i];
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        tv_sb_push_utf8(&sb, cp);
    }
    return tv_str_from_sb(&sb);
}

/* ------------------------------------------------------------------ string methods */

static inline int64_t tv_clamp_index(int64_t i, int64_t len) {
    if (i < 0) { i += len; return i < 0 ? 0 : i; }
    return i > len ? len : i;
}

static inline int64_t tv_clamp0(int64_t i, int64_t len) { return i < 0 ? 0 : i > len ? len : i; }

/* UTF-16 units a..b of s onto sb (its bytes when it's ASCII). */
void tv_sb_push_units(tv_sb *sb, tv_str s, tv_int a, tv_int b) {
    if (b <= a) return;
    if (tv_str_is_ascii(s)) {
        tv_sb_push(sb, s.p->data + a, (size_t)(b - a));
        return;
    }
    tv_str piece = tv_str_units(s, a, b);
    tv_sb_push_str(sb, piece);
    tv_str_release(piece);
}

/* s += t.slice(start, end): the units appended without making the slice. */
void tv_str_append_slice(tv_str *s, tv_str t, tv_int start, tv_int end, bool has_end) {
    if (tv_str_is_ascii(t)) {
        int64_t len = tv_str_byte_len(t);
        int64_t a = tv_clamp_index(start, len), b = has_end ? tv_clamp_index(end, len) : len;
        if (b > a) tv_str_append_bytes(s, t.p->data + a, (size_t)(b - a), TV_STR_ASCII);
        return;
    }
    tv_str piece = tv_str_slice(t, start, end, has_end, NULL);
    tv_str_append(s, piece);
    tv_str_release(piece);
}

tv_str tv_str_slice(tv_str s, tv_int start, tv_int end, bool has_end, const char *loc) {
    (void)loc;
    int64_t len = tv_str_length(s);
    int64_t a = tv_clamp_index(start, len);
    int64_t b = has_end ? tv_clamp_index(end, len) : len;
    return tv_str_units(s, a, b);
}

tv_str tv_str_ascii_slice(tv_str s, int64_t a, int64_t b) {
    if (a >= b) return TV_EMPTY_STR;
    if (a == 0 && b == s.p->len) { tv_str_retain(s); return s; }
    tv_str r = tv_str_from(s.p->data + a, (size_t)(b - a));
    tv_str_mark(r, true);
    return r;
}

tv_str tv_str_substring_slow(tv_str s, tv_int start, tv_int end, bool has_end) {
    int64_t len = tv_str_length(s);
    int64_t a = tv_clamp0(start, len), b = has_end ? tv_clamp0(end, len) : len;
    return a <= b ? tv_str_units(s, a, b) : tv_str_units(s, b, a);
}

/* The length of s.slice (kind 0), s.substring (1) or s.substr (2) of these arguments, nothing made:
 * the clamped unit positions' difference (a cut surrogate pair's half is still one unit). */
tv_int tv_str_sub_len(tv_str s, int kind, tv_int start, tv_int end, bool has_end) {
    int64_t len = tv_str_length(s), a, b;
    switch (kind) {
    case 0:
        a = tv_clamp_index(start, len), b = has_end ? tv_clamp_index(end, len) : len;
        return b > a ? b - a : 0;
    case 1:
        a = tv_clamp0(start, len), b = has_end ? tv_clamp0(end, len) : len;
        return a <= b ? b - a : a - b;
    default:
        a = tv_clamp_index(start, len), b = !has_end ? len : end <= 0 ? a : end > len - a ? len : a + end;
        return b - a;
    }
}

tv_str tv_str_substr(tv_str s, tv_int start, tv_int count, bool has_count) {
    int64_t len = tv_str_length(s);
    int64_t a = tv_clamp_index(start, len);
    int64_t b = !has_count ? len : count <= 0 ? a : count > len - a ? len : a + count;
    return tv_str_units(s, a, b);
}

/* Byte index of needle in h at or after `from`, or -1. */
static int64_t tv_find(const char *h, size_t hn, const char *nd, size_t nn, size_t from) {
    if (from > hn || nn > hn - from) return -1;
    if (nn == 0) return (int64_t)from;
    const char *p = h + from, *last = h + hn - nn; /* last possible start */
    char first = nd[0];
    while (p <= last) {
        p = (const char *)memchr(p, first, (size_t)(last - p) + 1);
        if (!p) return -1;
        if (tv_mem_eq(p + 1, nd + 1, nn - 1)) return p - h;
        p++;
    }
    return -1;
}

bool tv_str_includes(tv_str s, tv_str needle) {
    return tv_find(s.p->data, (size_t)s.p->len, needle.p->data, (size_t)needle.p->len, 0) >= 0;
}

/* Byte offset of unit u (>= 0; past the end: the end). *mid as for tv_str_unit_byte. */
static inline size_t tv_unit_to_byte(tv_str s, int64_t u, bool *mid) {
    *mid = false;
    if (tv_str_is_ascii(s)) return u > s.p->len ? (size_t)s.p->len : (size_t)u;
    return tv_str_unit_byte(s.p, u, mid);
}

/* Unit index of byte offset b (a character boundary). (A string whose length isn't known yet
 * just counts up to b, which costs no more than finding b did.) */
static inline int64_t tv_byte_to_unit(tv_str s, size_t b) {
    if (s.p->rc & TV_STR_ASCII) return (int64_t)b;
    return tv_str_byte_unit(s.p, b);
}

tv_int tv_str_index_of_from(tv_str s, tv_str needle, tv_int from) {
    bool mid = false;
    size_t fb = from <= 0 ? 0 : tv_unit_to_byte(s, from, &mid);
    if (mid) fb += 4; /* (no match starts at a pair's second half) */
    if (needle.p->len == 0) {
        int64_t len = tv_str_length(s);
        return from <= 0 ? 0 : from > len ? len : from;
    }
    int64_t pos = tv_find(s.p->data, (size_t)s.p->len, needle.p->data, (size_t)needle.p->len, fb);
    return pos < 0 ? -1 : tv_byte_to_unit(s, (size_t)pos);
}

tv_int tv_str_last_index_of(tv_str s, tv_str needle, tv_int from, bool has_from) {
    if (!has_from && tv_str_is_ascii(s)) {
        /* (ASCII text, from the end: byte positions are its unit positions) */
        size_t n = (size_t)s.p->len, nn = (size_t)needle.p->len;
        if (nn > n) return -1;
        if (nn == 0) return (tv_int)n;
        const char *h = s.p->data, *nd = needle.p->data;
        char c0 = nd[0];
        for (size_t k = n - nn + 1; k-- > 0;)
            if (h[k] == c0 && (nn == 1 || tv_mem_eq(h + k, nd, nn))) return (tv_int)k;
        return -1;
    }
    int64_t len = tv_str_length(s);
    int64_t start = !has_from || from > len ? len : from < 0 ? 0 : from;
    if (needle.p->len == 0) return start;
    size_t n = (size_t)s.p->len, nn = (size_t)needle.p->len;
    if (nn > n) return -1;
    bool mid;
    size_t k = tv_unit_to_byte(s, start, &mid);
    if (k > n - nn) k = n - nn;
    const char *h = s.p->data, *nd = needle.p->data;
    for (;;) {
        if (h[k] == nd[0] && tv_mem_eq(h + k, nd, nn)) return tv_byte_to_unit(s, k);
        if (k == 0) return -1;
        k--;
    }
}

bool tv_str_includes_at(tv_str s, tv_str needle, tv_int from) { return tv_str_index_of_from(s, needle, from) >= 0; }

bool tv_str_starts_with_at(tv_str s, tv_str prefix, tv_int pos) {
    bool mid = false;
    size_t b = pos <= 0 ? 0 : tv_unit_to_byte(s, pos, &mid);
    if (mid) return prefix.p->len == 0;
    return (size_t)prefix.p->len <= (size_t)s.p->len - b && tv_mem_eq(s.p->data + b, prefix.p->data, (size_t)prefix.p->len);
}

bool tv_str_ends_with_at(tv_str s, tv_str suffix, tv_int end) {
    bool mid = false;
    size_t e = end <= 0 ? 0 : tv_unit_to_byte(s, end, &mid);
    if (mid) return suffix.p->len == 0;
    size_t nn = (size_t)suffix.p->len;
    return nn <= e && tv_mem_eq(s.p->data + e - nn, suffix.p->data, nn);
}


tv_int tv_str_index_of(tv_str s, tv_str needle) {
    int64_t pos = tv_find(s.p->data, (size_t)s.p->len, needle.p->data, (size_t)needle.p->len, 0);
    return pos <= 0 ? pos : tv_byte_to_unit(s, (size_t)pos);
}

static tv_arrbuf *tv_arrbuf_new(size_t esize, int64_t cap);

/* `for (const ch of s)`: the code point at byte *pos as a string, *pos moved past it; false at
 * the end. (No array of them: printable ASCII characters are immortal strings.) */
bool tv_str_next_char_slow(tv_str s, tv_int *pos, tv_str *out) {
    size_t n = (size_t)s.p->len, i = (size_t)*pos;
    if (i >= n) return false;
    const char *d = s.p->data;
    unsigned char c = (unsigned char)d[i];
    if (c - 32u < 96u) {
        *out = tv_ascii_str(c);
        *pos = (tv_int)i + 1;
        return true;
    }
    size_t l = tv_utf8_len(c);
    if (l > n - i) l = n - i;
    *out = tv_str_from(d + i, l);
    *pos = (tv_int)(i + l);
    return true;
}

tv_arr tv_str_chars(tv_str s) {
    int64_t count = tv_str_char_count(s);
    if (count == 0) return tv_arr_fresh();
    tv_arrbuf *b = tv_arrbuf_new(sizeof(tv_str), count);
    tv_str *out = (tv_str *)(void *)tv_arrbuf_data(b);
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len, i = 0;
    int64_t k = 0;
    while (i < n && k < count) {
        size_t l = tv_utf8_len((unsigned char)d[i]);
        if (l > n - i) l = n - i;
        out[k++] = tv_str_from(d + i, l);
        i += l;
    }
    return tv_arr_of(b, k);
}

/* The bytes of w equal to the byte repeated in pat: 0x80 in each (exact, no carries between
 * bytes). */
static inline uint64_t tv_bytes_eq(uint64_t w, uint64_t pat) {
    uint64_t x = w ^ pat;
    return ~(((x & 0x7F7F7F7F7F7F7F7FULL) + 0x7F7F7F7F7F7F7F7FULL) | x | 0x7F7F7F7F7F7F7F7FULL);
}

/* s.split(c) for one byte c: the pieces counted first (8 bytes at a time), so the array is
 * made once at its size, then found the same way. */
static tv_arr tv_str_split_byte(tv_str s, unsigned char c) {
    const unsigned char *d = (const unsigned char *)s.p->data;
    size_t n = (size_t)s.p->len, i = 0, count = 1;
    uint64_t pat = 0x0101010101010101ULL * c;
    for (; i + 8 <= n; i += 8) count += (size_t)__builtin_popcountll(tv_bytes_eq(tv_r64(d + i), pat));
    for (; i < n; i++) count += d[i] == c;
    if (count == 1) {
        tv_arr r = tv_arr_fresh();
        tv_str_retain(s);
        tv_arr_push(&r, &tv_type_str, &s);
        return r;
    }
    bool ascii = tv_str_is_ascii(s);
    tv_arrbuf *b = tv_arrbuf_new(sizeof(tv_str), (int64_t)count);
    tv_str *out = (tv_str *)(void *)tv_arrbuf_data(b);
    size_t k = 0, from = 0;
    i = 0;
    while (k + 1 < count) {
        uint64_t z;
        while (i + 8 <= n && !(z = tv_bytes_eq(tv_r64(d + i), pat))) i += 8;
        if (i + 8 <= n) i += (size_t)__builtin_ctzll(z) >> 3;
        else while (d[i] != c) i++;
        tv_str piece = tv_str_from((const char *)d + from, i - from);
        if (ascii) tv_str_mark(piece, true);
        out[k++] = piece;
        from = ++i;
    }
    tv_str piece = tv_str_from((const char *)d + from, n - from);
    if (ascii) tv_str_mark(piece, true);
    out[k++] = piece;
    return tv_arr_of(b, (int64_t)k);
}

/* s.split(sep, limit): at most `limit` pieces (ToUint32 of it, as JavaScript reads it) */
tv_arr tv_str_split_limit(tv_str s, tv_str sep, double limit) {
    uint32_t n = (uint32_t)(int64_t)(limit == limit ? limit : 0);
    if (n == 0) return tv_arr_fresh();
    if (sep.p->len == 0) {
        tv_arr a = tv_str_split(s, sep);
        if ((uint64_t)tv_arr_len(a) > n) tv_arr_set_length(&a, &tv_type_str, (tv_int)n, "split");
        return a;
    }
    /* (the first n pieces only: `src.split("\n", 1)` of a long text looks at its first line) */
    tv_arr a = tv_arr_fresh();
    const char *d = s.p->data, *sd = sep.p->data;
    size_t len = (size_t)s.p->len, sl = (size_t)sep.p->len, i = 0;
    while ((uint64_t)tv_arr_len(a) < n) {
        const char *hit = len - i >= sl ? memmem(d + i, len - i, sd, sl) : NULL;
        size_t end = hit ? (size_t)(hit - d) : len;
        tv_str piece = tv_str_from(d + i, end - i);
        tv_arr_push(&a, &tv_type_str, &piece);
        if (!hit) break;
        i = end + sl;
    }
    return a;
}

tv_arr tv_str_split(tv_str s, tv_str sep) {
    if (sep.p->len == 0) {
        if (tv_str_is_ascii(s)) return tv_str_chars(s);
        /* into UTF-16 units: a pair's halves can't stand alone in UTF-8, each is U+FFFD */
        tv_arrbuf *b = tv_arrbuf_new(sizeof(tv_str), tv_str_length(s));
        tv_str *out = (tv_str *)(void *)tv_arrbuf_data(b);
        const unsigned char *d = (const unsigned char *)s.p->data;
        size_t n = (size_t)s.p->len, i = 0;
        int64_t k = 0, units;
        while (i < n) {
            size_t l = tv_char_units(d, n, i, &units);
            if (units == 2) {
                out[k++] = (tv_str){(tv_strbuf *)&tv_fffd_strbuf};
                out[k++] = (tv_str){(tv_strbuf *)&tv_fffd_strbuf};
            } else {
                out[k++] = d[i] - 32u < 96u ? tv_ascii_str(d[i]) : tv_str_from((const char *)d + i, l);
            }
            i += l;
        }
        return tv_arr_of(b, k);
    }
    if (sep.p->len == 1 && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) return tv_str_split_byte(s, (unsigned char)sep.p->data[0]);
    tv_arr r = tv_arr_fresh();
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len, sn = (size_t)sep.p->len, from = 0;
    int64_t pos = tv_find(d, n, sep.p->data, sn, 0);
    if (pos < 0) {
        tv_str_retain(s);
        tv_arr_push(&r, &tv_type_str, &s);
        return r;
    }
    bool ascii = tv_str_is_ascii(s); /* (then so are the pieces: none needs a look of its own) */
    for (;;) {
        size_t end = pos < 0 ? n : (size_t)pos;
        tv_str piece = tv_str_from(d + from, end - from);
        if (ascii) tv_str_mark(piece, true);
        tv_arr_push_fast(&r, &tv_type_str, &piece);
        if (pos < 0) break;
        from = end + sn;
        pos = tv_find(d, n, sep.p->data, sn, from);
    }
    return r;
}

static tv_str tv_str_trim_impl(tv_str s, bool left, bool right) {
    size_t a, b;
    tv_trim_range(s, left, right, &a, &b);
    if (a == 0 && b == (size_t)s.p->len) { tv_str_retain(s); return s; }
    tv_str r = tv_str_from(s.p->data + a, b - a);
    if (s.p->rc & TV_STR_ASCII) tv_str_mark(r, true);
    return r;
}

tv_str tv_str_trim(tv_str s) { return tv_str_trim_impl(s, true, true); }
tv_str tv_str_trim_start(tv_str s) { return tv_str_trim_impl(s, true, false); }
tv_str tv_str_trim_end(tv_str s) { return tv_str_trim_impl(s, false, true); }

static tv_str tv_str_map_ascii(tv_str s, char lo, char hi, int delta) {
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len, i = 0;
    while (i < n && !(d[i] >= lo && d[i] <= hi)) i++;
    if (i == n) { tv_str_retain(s); return s; }
    tv_strbuf *b = tv_strbuf_new(n);
    memcpy(b->data, d, n);
    for (; i < n; i++)
        if (b->data[i] >= lo && b->data[i] <= hi) b->data[i] = (char)(b->data[i] + delta);
    b->rc |= s.p->rc & (TV_STR_ASCII | TV_STR_WIDE);
    return (tv_str){b};
}

tv_str tv_str_to_upper(tv_str s) { return tv_str_map_ascii(s, 'a', 'z', 'A' - 'a'); }
tv_str tv_str_to_lower(tv_str s) { return tv_str_map_ascii(s, 'A', 'Z', 'a' - 'A'); }

/* Appends a JS replacement string, expanding $$, $&, $` and $'. */
static void tv_push_replacement(tv_sb *sb, tv_str repl, const char *s, size_t slen, size_t pos, size_t mlen) {
    const char *r = repl.p->data;
    size_t rn = (size_t)repl.p->len;
    if (!memchr(r, '$', rn)) { tv_sb_push(sb, r, rn); return; }
    for (size_t i = 0; i < rn; i++) {
        if (r[i] == '$' && i + 1 < rn) {
            char c = r[i + 1];
            if (c == '$') { tv_sb_push_char(sb, '$'); i++; continue; }
            if (c == '&') { tv_sb_push(sb, s + pos, mlen); i++; continue; }
            if (c == '`') { tv_sb_push(sb, s, pos); i++; continue; }
            if (c == '\'') { tv_sb_push(sb, s + pos + mlen, slen - pos - mlen); i++; continue; }
        }
        tv_sb_push_char(sb, r[i]);
    }
}

tv_str tv_str_replace(tv_str s, tv_str search, tv_str replacement) {
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len, mn = (size_t)search.p->len;
    int64_t pos = tv_find(d, n, search.p->data, mn, 0);
    if (pos < 0) { tv_str_retain(s); return s; }
    tv_sb sb = {0};
    tv_sb_push(&sb, d, (size_t)pos);
    tv_push_replacement(&sb, replacement, d, n, (size_t)pos, mn);
    tv_sb_push(&sb, d + pos + mn, n - (size_t)pos - mn);
    tv_str r = tv_str_from_sb(&sb);
    if (s.p->rc & replacement.p->rc & TV_STR_ASCII) tv_str_mark(r, true);
    return r;
}

tv_str tv_str_replace_all(tv_str s, tv_str search, tv_str replacement) {
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len, mn = (size_t)search.p->len;
    tv_sb sb = {0};
    if (mn == 0) { /* insert at every character boundary, including both ends */
        size_t i = 0;
        for (;;) {
            tv_push_replacement(&sb, replacement, d, n, i, 0);
            if (i >= n) break;
            size_t l = tv_utf8_len((unsigned char)d[i]);
            if (l > n - i) l = n - i;
            tv_sb_push(&sb, d + i, l);
            i += l;
        }
        return tv_str_from_sb(&sb);
    }
    int64_t pos = tv_find(d, n, search.p->data, mn, 0);
    if (pos < 0) { tv_str_retain(s); return s; }
    size_t last = 0;
    while (pos >= 0) {
        tv_sb_push(&sb, d + last, (size_t)pos - last);
        tv_push_replacement(&sb, replacement, d, n, (size_t)pos, mn);
        last = (size_t)pos + mn;
        pos = tv_find(d, n, search.p->data, mn, last);
    }
    tv_sb_push(&sb, d + last, n - last);
    tv_str r = tv_str_from_sb(&sb);
    if (s.p->rc & replacement.p->rc & TV_STR_ASCII) tv_str_mark(r, true);
    return r;
}

tv_str tv_str_repeat(tv_str s, tv_int count, const char *loc) {
    if (count < 0) {
        char msg[64];
        snprintf(msg, sizeof msg, "Invalid count value: %lld", (long long)count);
        tv_trap(msg, loc);
    }
    size_t n = (size_t)s.p->len;
    if (count == 0 || n == 0) return TV_EMPTY_STR;
    if (count == 1) { tv_str_retain(s); return s; }
    if ((uint64_t)count > (uint64_t)(INT64_MAX / 2) / n) tv_trap("Invalid string length", loc);
    size_t total = n * (size_t)count;
    tv_strbuf *b = tv_strbuf_new(total);
    memcpy(b->data, s.p->data, n);
    size_t have = n;
    while (have < total) { /* doubling copies */
        size_t c = have <= total - have ? have : total - have;
        memcpy(b->data + have, b->data, c);
        have += c;
    }
    b->rc |= s.p->rc & (TV_STR_ASCII | TV_STR_WIDE);
    return (tv_str){b};
}

/* Appends `chars` characters of fill (cycled). fill is non-empty. */
static void tv_push_fill(tv_sb *sb, tv_str fill, int64_t units) {
    int64_t fc = tv_str_length(fill);
    int64_t full = units / fc, rem = units % fc;
    for (int64_t i = 0; i < full; i++) tv_sb_push_str(sb, fill);
    if (rem) {
        tv_str part = tv_str_units(fill, 0, rem);
        tv_sb_push_str(sb, part);
        tv_str_release(part);
    }
}

static tv_str tv_str_pad(tv_str s, tv_int len, tv_str fill, bool at_start) {
    int64_t cur = tv_str_length(s);
    if (len <= cur || fill.p->len == 0) { tv_str_retain(s); return s; }
    tv_sb sb = {0};
    if (!at_start) tv_sb_push_str(&sb, s);
    tv_push_fill(&sb, fill, len - cur);
    if (at_start) tv_sb_push_str(&sb, s);
    return tv_str_from_sb(&sb);
}

tv_str tv_str_pad_start(tv_str s, tv_int len, tv_str fill) { return tv_str_pad(s, len, fill, true); }
tv_str tv_str_pad_end(tv_str s, tv_int len, tv_str fill) { return tv_str_pad(s, len, fill, false); }

/* ------------------------------------------------------------------ parsing */

static inline bool tv_is_digit(char c) { return c >= '0' && c <= '9'; }

/* Eisel-Lemire's multipliers: 5^q to 128 bits, from every 28th power and 2-bit corrections
 * (generated and checked against exact values). */
static const uint64_t tv_el_pow5[24][2] = { { 17218479456385750618ull, 1242899115359157055ull }, { 17386221171832138870ull, 13836871056604407053ull }, { 17555597020139803786ull, 7729112049988473103ull }, { 17726622920963562283ull, 9091170749936331336ull }, { 17899314949046850752ull, 14051622066030463842ull }, { 18073689335732591980ull, 8749227812785250177ull }, { 18249762470488780874ull, 10416625923311642211ull }, { 18427550902448932383ull, 18298537904747540562ull }, { 9303535670983768199ull, 578208414664970847ull }, { 9394170331095332911ull, 10291851488884697288ull }, { 9485687950320942729ull, 1825030320404309164ull }, { 9578097130411805364ull, 13644483260788183358ull }, { 9671406556917033397ull, 11979463175419572496ull }, { 9765625000000000000ull, 0ull }, { 9860761315262647567ull, 11927795063396681728ull }, { 9956824444577826731ull, 7941369183226839863ull }, { 10053823416929743980ull, 13653190937906703988ull }, { 10151767349262596893ull, 1772699331562333708ull }, { 10250665447337476733ull, 1466078993672598279ull }, { 10350527006597618960ull, 17973304801030866876ull }, { 10451361413042082976ull, 2133748077373825698ull }, { 10553178144107943212ull, 3674159897003727796ull }, { 10655986769561074501ull, 14608429132904838403ull }, { 10759796952395615435ull, 1096485900831157192ull } };
static const uint32_t tv_el_offsets[41] = { 353719360u, 4526096u, 327680u, 0u, 4097u, 1074790400u, 72368385u, 0u, 1073807440u, 272634112u, 17055749u, 1048640u, 1343488000u, 88425536u, 1430607188u, 2421249385u, 1704286565u, 89167189u, 335877396u, 2861917249u, 2811931370u, 64u, 0u, 0u, 0u, 0u, 1314816u, 20975696u, 71566592u, 1346377045u, 341071121u, 1070421u, 1114368u, 2505466896u, 2505397669u, 1142228228u, 1343575316u, 1415926101u, 4216149u, 1073741829u, 1118273u };

/* Decimal to double (Eisel-Lemire, as in Lemire's fast_float), small-table form. */
static const uint64_t tv_el_pow5_small[28] = { 1ull, 5ull, 25ull, 125ull, 625ull, 3125ull, 15625ull, 78125ull, 390625ull, 1953125ull, 9765625ull, 48828125ull, 244140625ull, 1220703125ull, 6103515625ull, 30517578125ull, 152587890625ull, 762939453125ull, 3814697265625ull, 19073486328125ull, 95367431640625ull, 476837158203125ull, 2384185791015625ull, 11920928955078125ull, 59604644775390625ull, 298023223876953125ull, 1490116119384765625ull, 7450580596923828125ull };
/* 5^q to 128 bits (top bit set), for q in [-342, 308] */
static inline void tv_el_pow5_128(int q, uint64_t *hi, uint64_t *lo) {
    int i = q + 342, b = i / 28, off = i - b * 28;
    uint64_t h = tv_el_pow5[b][0], l = tv_el_pow5[b][1];
    if (off) {
        uint64_t m = tv_el_pow5_small[off];
        unsigned __int128 pl = (unsigned __int128)l * m, ph = (unsigned __int128)h * m;
        uint64_t w0 = (uint64_t)pl;
        unsigned __int128 mid = (pl >> 64) + (uint64_t)ph;
        uint64_t w1 = (uint64_t)mid, w2 = (uint64_t)(ph >> 64) + (uint64_t)(mid >> 64);
        int s = __builtin_clzll(w2);
        if (s) { h = (w2 << s) | (w1 >> (64 - s)); l = (w1 << s) | (w0 >> (64 - s)); }
        else { h = w2; l = w1; }
        uint64_t c = (tv_el_offsets[i / 16] >> ((i % 16) * 2)) & 3, nl = l + c;
        h += nl < l; l = nl;
    }
    *hi = h; *lo = l;
}
/* w × 10^q correctly rounded (w != 0, exact); false when it can't be sure, or the result
 * isn't a normal double (then strtod decides). */
static bool tv_el_parse(uint64_t w, int q, double *out) {
    /* (a significand and a power of ten both exact as doubles: one correctly rounded operation) */
    static const double pow10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    if (w < (1ull << 53) && q >= -22 && q <= 22) {
        *out = q < 0 ? (double)w / pow10[-q] : (double)w * pow10[q];
        return true;
    }
    if (q < -342 || q > 308) return false;
    int lz = __builtin_clzll(w);
    w <<= lz;
    uint64_t ph, pl;
    tv_el_pow5_128(q, &ph, &pl);
    unsigned __int128 first = (unsigned __int128)w * ph;
    uint64_t hi = (uint64_t)(first >> 64), lo = (uint64_t)first;
    const uint64_t mask = 0xFFFFFFFFFFFFFFFFull >> 55;
    if ((hi & mask) == mask) {
        uint64_t sh = (uint64_t)(((unsigned __int128)w * pl) >> 64), nlo = lo + sh;
        hi += nlo < lo;
        lo = nlo;
        if (lo == 0xFFFFFFFFFFFFFFFFull && (q < -27 || q > 55)) return false;
    }
    int upper = (int)(hi >> 63);
    uint64_t mant = hi >> (upper + 64 - 52 - 3);
    int32_t p2 = (int32_t)(((152170 + 65536) * q) >> 16) + 63 + upper - lz + 1023;
    if (p2 <= 0) return false;
    if (lo <= 1 && q >= -4 && q <= 23 && (mant & 3) == 1 && (mant << (upper + 64 - 52 - 3)) == hi) mant &= ~1ull;
    mant += mant & 1;
    mant >>= 1;
    if (mant >= (2ull << 52)) { mant = 1ull << 52; p2++; }
    mant &= ~(1ull << 52);
    if (p2 >= 0x7FF) return false;
    uint64_t bits = mant | ((uint64_t)p2 << 52);
    memcpy(out, &bits, 8);
    return true;
}
/* A JSON-shaped number's text [s, e) (validated): its value, by Eisel-Lemire when it has at
 * most 19 significant digits. */
static bool tv_el_decimal(const unsigned char *s, const unsigned char *e, double *out) {
    bool neg = false;
    if (s < e && *s == '-') { neg = true; s++; }
    uint64_t w = 0;
    int digits = 0, q = 0;
    for (; s < e && *s >= '0' && *s <= '9'; s++) {
        if (w || *s != '0') { if (++digits > 19) return false; w = w * 10 + (uint64_t)(*s - '0'); }
    }
    if (s < e && *s == '.') {
        for (s++; s < e && *s >= '0' && *s <= '9'; s++) {
            if (w || *s != '0') { if (++digits > 19) return false; w = w * 10 + (uint64_t)(*s - '0'); }
            q--;
        }
    }
    if (s < e && (*s == 'e' || *s == 'E')) {
        s++;
        bool eneg = false;
        if (s < e && (*s == '+' || *s == '-')) eneg = *s++ == '-';
        int x = 0;
        for (; s < e && *s >= '0' && *s <= '9'; s++) if (x < 100000) x = x * 10 + (*s - '0');
        q += eneg ? -x : x;
    }
    if (s != e) return false;
    double d;
    if (w == 0) d = 0;
    else if (!tv_el_parse(w, q, &d)) return false;
    *out = neg ? -d : d;
    return true;
}

bool tv_parse_float(tv_str s, double *out) {
    size_t a, b;
    tv_trim_range(s, true, true, &a, &b);
    const char *p = s.p->data + a, *e = s.p->data + b, *q = p;
    if (p == e) return false;
    bool neg = *q == '-';
    if (*q == '+' || *q == '-') q++;
    if (e - q == 8 && memcmp(q, "Infinity", 8) == 0) {
        *out = neg ? -INFINITY : INFINITY;
        return true;
    }
    size_t nd = 0;
    while (q < e && tv_is_digit(*q)) q++, nd++;
    if (q < e && *q == '.') {
        q++;
        while (q < e && tv_is_digit(*q)) q++, nd++;
    }
    if (nd == 0) return false;
    if (q < e && (*q == 'e' || *q == 'E')) {
        q++;
        if (q < e && (*q == '+' || *q == '-')) q++;
        size_t ne = 0;
        while (q < e && tv_is_digit(*q)) q++, ne++;
        if (ne == 0) return false;
    }
    if (q != e) return false;
    if (tv_el_decimal((const unsigned char *)p, (const unsigned char *)e, out)) return true;
    /* The span is validated; strtod stops at e (whitespace, NUL or a non-ASCII byte). */
    char *end;
    double d = strtod(p, &end);
    if (end != e) return false;
    *out = d;
    return true;
}

double tv_js_number(tv_str s) {
    size_t a, b;
    tv_trim_range(s, true, true, &a, &b);
    const char *p = s.p->data + a, *e = s.p->data + b;
    if (p == e) return 0; /* Number("") and Number("  ") are 0 */
    if (e - p > 2 && p[0] == '0') {
        int base = (p[1] == 'x' || p[1] == 'X') ? 16 : (p[1] == 'o' || p[1] == 'O') ? 8 : (p[1] == 'b' || p[1] == 'B') ? 2 : 0;
        if (base) {
            double v = 0;
            for (const char *q = p + 2; q < e; q++) {
                int d = (*q >= '0' && *q <= '9') ? *q - '0' : (*q >= 'a' && *q <= 'f') ? *q - 'a' + 10 : (*q >= 'A' && *q <= 'F') ? *q - 'A' + 10 : 99;
                if (d >= base) return NAN;
                v = v * base + d;
            }
            return v;
        }
    }
    double d;
    return tv_parse_float(s, &d) ? d : NAN;
}

/* parseFloat of the bytes p..e (leading whitespace already skipped) */
static double tv_parse_float_bytes(const char *p, const char *e) {
    const char *q = p;
    bool neg = q < e && *q == '-';
    if (q < e && (*q == '+' || *q == '-')) q++;
    if (e - q >= 8 && memcmp(q, "Infinity", 8) == 0) return neg ? -INFINITY : INFINITY;
    size_t nd = 0;
    while (q < e && tv_is_digit(*q)) q++, nd++;
    if (q < e && *q == '.') {
        const char *r = q + 1;
        size_t nf = 0;
        while (r < e && tv_is_digit(*r)) r++, nf++;
        if (nd + nf > 0) q = r, nd += nf;
    }
    if (nd == 0) return NAN;
    if (q < e && (*q == 'e' || *q == 'E')) {
        const char *r = q + 1;
        if (r < e && (*r == '+' || *r == '-')) r++;
        size_t ne = 0;
        while (r < e && tv_is_digit(*r)) r++, ne++;
        if (ne) q = r;
    }
    double d;
    if (tv_el_decimal((const unsigned char *)p, (const unsigned char *)q, &d)) return d;
    char buf[400];
    size_t n = (size_t)(q - p);
    if (n >= sizeof buf) {
        char *big = tv_alloc(n + 1);
        memcpy(big, p, n);
        big[n] = 0;
        double d = strtod(big, NULL);
        tv_free(big);
        return d;
    }
    memcpy(buf, p, n);
    buf[n] = 0;
    return strtod(buf, NULL);
}

double tv_js_parse_float(tv_str s) {
    /* the longest prefix that's a decimal number (after leading whitespace), else NaN */
    size_t a, b;
    tv_trim_range(s, true, false, &a, &b);
    return tv_parse_float_bytes(s.p->data + a, s.p->data + b);
}

/* parseFloat(s.slice(start, end)) without making the slice (of ASCII text: its units are bytes). */
double tv_js_parse_float_slice(tv_str s, tv_int start, tv_int end, bool has_end) {
    if (!tv_str_is_ascii(s)) {
        tv_str piece = tv_str_slice(s, start, end, has_end, NULL);
        double d = tv_js_parse_float(piece);
        tv_str_release(piece);
        return d;
    }
    int64_t len = tv_str_byte_len(s);
    int64_t a = tv_clamp_index(start, len), b = has_end ? tv_clamp_index(end, len) : len;
    if (b < a) b = a;
    const char *p = s.p->data + a, *e = s.p->data + b;
    while (p < e && (*p == ' ' || (*p >= 9 && *p <= 13))) p++;
    return tv_parse_float_bytes(p, e);
}

bool tv_parse_int(tv_str s, tv_int radix, tv_int *out) {
    size_t a, b;
    tv_trim_range(s, true, false, &a, &b);
    const char *p = s.p->data + a, *e = s.p->data + b;
    bool neg = false;
    if (p < e && (*p == '+' || *p == '-')) neg = *p++ == '-';
    bool hex_prefix = e - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
    if (radix == 0) {
        radix = 10;
        if (hex_prefix) { radix = 16; p += 2; }
    } else if (radix == 16) {
        if (hex_prefix) p += 2;
    } else if (radix < 2 || radix > 36) {
        return false;
    }
    uint64_t v = 0, limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    size_t nd = 0;
    for (; p < e; p++) {
        char c = *p;
        int dv = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
        if (dv >= radix) break;
        if (v > (limit - (uint64_t)dv) / (uint64_t)radix) return false; /* doesn't fit in int */
        v = v * (uint64_t)radix + (uint64_t)dv;
        nd++;
    }
    if (nd == 0) return false;
    *out = neg ? (tv_int)(0 - v) : (tv_int)v;
    return true;
}

/* ================================================================== arrays */

static int64_t tv_grow_cap(int64_t cur, int64_t min_cap) {
    int64_t c = cur > INT64_MAX / 2 ? INT64_MAX : cur * 2; /* geometric, at least 4 */
    if (c < 4) c = 4;
    return c < min_cap ? min_cap : c;
}

tv_arrbuf tv_empty_arrbuf = {-1, 0, 0, NULL};

/* An elements' block of its own (`ext`): 16 bytes, then room left free at the front for
 * `unshift` (none at first; `shift` leaves its slot there), then the elements. Its size in
 * bytes, front included, is the word before the elements: what `ext` points to after the
 * front is all the block knows of where it starts. The block's first word is its size (the
 * front's word is never before the second): a small block comes from the small-object free
 * lists and goes back to them, as freeing a malloc block of that size costs several times as
 * much (macOS's allocator reads the clock on every free). */
enum { TV_EXT_PRE = 16 };
static inline size_t tv_ext_front(const unsigned char *ext) { size_t f; memcpy(&f, ext - sizeof f, sizeof f); return f; }
static inline void tv_ext_set_front(unsigned char *ext, size_t f) { memcpy(ext - sizeof f, &f, sizeof f); }
static inline unsigned char *tv_ext_base(unsigned char *ext) { return ext - TV_EXT_PRE - tv_ext_front(ext); }
static inline size_t tv_ext_size(const unsigned char *base) { size_t n; memcpy(&n, base, sizeof n); return n; }
/* (16-byte multiples: the elements stay as aligned as malloc's) */
static unsigned char *tv_ext_block(size_t size) {
    size = (size + 15) & ~(size_t)15;
    unsigned char *b = (unsigned char *)(size <= TV_SMALL_MAX ? tv_small_alloc(size) : tv_alloc(size));
    memcpy(b, &size, sizeof size);
    return b;
}
static void tv_ext_free(unsigned char *ext) {
    unsigned char *b = tv_ext_base(ext);
    size_t size = tv_ext_size(b);
    if (size <= TV_SMALL_MAX) tv_small_free(b, size);
    else free(b);
}
/* A block for `bytes` of elements after `front` free bytes: where the elements start. */
static unsigned char *tv_ext_new(size_t front, size_t bytes) {
    unsigned char *b = tv_ext_block(TV_EXT_PRE + front + (bytes ? bytes : 1));
    unsigned char *ext = b + TV_EXT_PRE + front;
    tv_ext_set_front(ext, front);
    return ext;
}

/* A header with room for cap elements right after it (len 0). */
static tv_arrbuf *tv_arrbuf_new(size_t esize, int64_t cap) {
    tv_arrbuf *b = (tv_arrbuf *)tv_arrbuf_alloc(tv_size_mul_add((size_t)cap, esize, 0));
    b->rc = 1;
    b->len = 0;
    b->cap = cap;
    b->ext = NULL;
    return b;
}

/* tv_mem_eq for n >= 16: up to 64 bytes by blocks overlapping the end, longer 64 at a time. */
typedef uint8_t tv_v16 __attribute__((vector_size(16), aligned(1)));
static inline tv_v16 tv_v16_at(const char *p) { tv_v16 x; memcpy(&x, p, 16); return x; }
static inline uint64_t tv_v16_any(tv_v16 d) { uint64_t w[2]; memcpy(w, &d, 16); return w[0] | w[1]; }
#define TV_V16X(a, b, k) (tv_v16_at((a) + (k)) ^ tv_v16_at((b) + (k)))
bool tv_mem_eq_long(const char *a, const char *b, size_t n) {
    if (n <= 32) return !tv_v16_any(TV_V16X(a, b, 0) | TV_V16X(a, b, n - 16));
    if (n <= 64) return !tv_v16_any(TV_V16X(a, b, 0) | TV_V16X(a, b, 16) | TV_V16X(a, b, n - 32) | TV_V16X(a, b, n - 16));
    /* (differences or-ed together, looked at once every 256 bytes: the look is a long chain) */
    tv_v16 acc = TV_V16X(a, b, n - 16);
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        acc |= TV_V16X(a, b, i) | TV_V16X(a, b, i + 16) | TV_V16X(a, b, i + 32) | TV_V16X(a, b, i + 48);
        if ((i & 255) == 192 && tv_v16_any(acc)) return false;
    }
    for (; i + 16 <= n; i += 16) acc |= TV_V16X(a, b, i);
    return !tv_v16_any(acc);
}

/* The retains of the descriptors of objects (a class's instances, a boxed record): a pointer
 * to a count first, 32 or 64 bits, or NULL. Copies of them retain inline, not by calls. */
void tv_retain_rc32(void *p) {
    int32_t *o = *(int32_t **)p;
    if (o) (*o)++;
}
void tv_retain_rc64(void *p) {
    int64_t *o = *(int64_t **)p;
    if (o) (*o)++;
}

/* Retains n elements of type t at d, t's retain known to be set. */
static void tv_retain_n(unsigned char *d, int64_t n, const tv_type *t) {
    if (t == &tv_type_str) {
        /* (strings retained here, not through calls: an immortal literal's is a test) */
        const tv_str *xs = (const tv_str *)(const void *)d;
        for (int64_t i = 0; i < n; i++) tv_str_retain(xs[i]);
    } else if (t->retain == tv_retain_rc32) {
        int32_t **xs = (int32_t **)(void *)d;
        for (int64_t i = 0; i < n; i++) if (xs[i]) (*xs[i])++;
    } else if (t->retain == tv_retain_rc64) {
        int64_t **xs = (int64_t **)(void *)d;
        for (int64_t i = 0; i < n; i++) if (xs[i]) (*xs[i])++;
    } else {
        size_t es = t->size;
        for (int64_t i = 0; i < n; i++) t->retain(d + (size_t)i * es);
    }
}

/* Copies n elements and retains the copies. */
static void tv_copy_retain(unsigned char *dst, const unsigned char *src, int64_t n, const tv_type *t) {
    size_t es = t->size;
    if (!es || n <= 0) return;
    memcpy(dst, src, (size_t)n * es);
    if (t->retain) tv_retain_n(dst, n, t);
}

void tv_arrbuf_release_slow(tv_arrbuf *TV_NOESCAPE p, const tv_type *t) {
    if (t && t->release && t->size) {
        size_t es = t->size;
        unsigned char *d = tv_arrbuf_data(p);
        if (t == &tv_type_str) {
            tv_str *xs = (tv_str *)(void *)d;
            for (int64_t i = 0; i < p->len; i++)
                if (xs[i].p) tv_str_release(xs[i]);
        } else if (es == sizeof(void *)) {
            /* (a word of zeros, `null`/`undefined` of a reference type: nothing to release) */
            for (int64_t i = 0; i < p->len; i++)
                if (((void **)(void *)d)[i]) t->release(d + (size_t)i * es);
        } else {
            for (int64_t i = 0; i < p->len; i++) t->release(d + (size_t)i * es);
        }
    }
    if (t) {
        tv_arrbuf_free_block(p, t->size);
        return;
    }
    if (__builtin_expect(tv_arr_props_n != 0, 0)) tv_arr_props_forget(p);
    if (p->ext) tv_ext_free(p->ext);
    free(p);
}

/* ---------------------------------------------------------------- arrays' own properties */

/* An array with properties of its own (`T[] & { links: L }`, as JavaScript's arrays are objects):
 * kept here by its header (which never moves), an object of them; dropped when it's freed. */
static tv_map tv_arr_props;
int64_t tv_arr_props_n;
/* (which headers may have some, by a hash of their address: most arrays freed have none) */
static uint64_t tv_arr_props_bits[1024];
static inline uint32_t tv_arr_props_bit(const tv_arrbuf *p) { return (uint32_t)(((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull >> 48); }

static void tv_arr_props_forget_now(tv_arrbuf *p) {
    uint32_t b = tv_arr_props_bit(p);
    if (!(tv_arr_props_bits[b >> 6] >> (b & 63) & 1)) return;
    tv_int k = (tv_int)(uintptr_t)p;
    if (tv_map_delete(&tv_arr_props, &tv_type_int, &tv_type_dyn, &k)) tv_arr_props_n--;
}

tv_dyn tv_arr_prop_get(tv_arr a, tv_str key) {
    if (!tv_arr_props_n) return (tv_dyn){&tv_type_undefined, NULL};
    tv_int k = (tv_int)(uintptr_t)a.p;
    tv_dyn *o = tv_map_get(tv_arr_props, &tv_type_int, &tv_type_dyn, &k);
    if (!o) return (tv_dyn){&tv_type_undefined, NULL};
    tv_dyn *v = tv_map_get(*(tv_map *)tv_dyn_data(*o), &tv_type_str, &tv_type_dyn, &key);
    if (!v) return (tv_dyn){&tv_type_undefined, NULL};
    tv_dyn_retain(*v);
    return *v;
}

void (*tv_arr_props_forget)(tv_arrbuf *p);

void tv_arr_prop_set(tv_arr *a, tv_str key, tv_dyn v) {
    tv_arr_props_forget = tv_arr_props_forget_now;
    /* (an immortal array, the empty one every `[]` shares: an array of its own first) */
    if (a->p->rc < 0) *a = tv_arr_fresh();
    if (!tv_arr_props.p) tv_arr_props = tv_map_fresh();
    tv_int k = (tv_int)(uintptr_t)a->p;
    tv_dyn *o = tv_map_get(tv_arr_props, &tv_type_int, &tv_type_dyn, &k);
    if (!o) {
        tv_map m = tv_map_fresh_object();
        tv_dyn d = tv_dyn_box(&tv_type_dynobj, &m);
        tv_map_set(&tv_arr_props, &tv_type_int, &tv_type_dyn, &k, &d);
        tv_arr_props_n++;
        uint32_t b = tv_arr_props_bit(a->p);
        tv_arr_props_bits[b >> 6] |= 1ull << (b & 63);
        o = tv_map_get(tv_arr_props, &tv_type_int, &tv_type_dyn, &k);
    }
    tv_str_retain(key);
    tv_map_set((tv_map *)tv_dyn_data(*o), &tv_type_str, &tv_type_dyn, &key, &v);
}

/* Frees an array's header and elements' blocks (its elements released already). */
void tv_arrbuf_free_block(tv_arrbuf *TV_NOESCAPE p, size_t es) {
    if (__builtin_expect(tv_arr_props_n != 0, 0)) tv_arr_props_forget(p);
    /* (the header's block: the size it was made with, from the count of elements it held) */
    int64_t inl = p->cap;
    if (p->ext) {
        memcpy(&inl, p + 1, sizeof inl);
        tv_ext_free(p->ext);
    }
    size_t s = tv_arrbuf_block((size_t)inl * es);
#ifndef TV_ARRBUF_PLAIN
    if (s <= TV_ARRBUF_SMALL) {
        size_t c = s >> 3;
        *(void **)(void *)p = tv_small_bins[c];
        tv_small_bins[c] = p;
        return;
    }
#endif
    free(p);
}

void tv_arr_release_slow(tv_arr a, const tv_type *t) { tv_arrbuf_release_slow(a.p, t); }

void tv_arr_make_unique(tv_arr *a, const tv_type *t, tv_int min_cap) {
    tv_arrbuf *p = a->p;
    size_t es = t->size;
    if (!p) {
        a->p = tv_arrbuf_new(es, min_cap > 0 ? tv_grow_cap(0, min_cap) : 0);
        return;
    }
    if (p->rc > 0) {
        if (p->cap >= min_cap) return;
        /* grow: the elements move to a block of their own (the header stays put) */
        int64_t nc = tv_grow_cap(p->cap, min_cap);
        size_t bytes = tv_size_mul_add((size_t)nc, es, 0);
        if (!p->ext) {
            unsigned char *d = tv_ext_new(0, bytes);
            if (es && p->len) memcpy(d, tv_arrbuf_data(p), (size_t)p->len * es);
            /* (the count the header's block was made for, where the elements were: see
             * tv_arrbuf_release_slow) */
            int64_t inl = p->cap;
            memcpy(p + 1, &inl, sizeof inl);
            p->ext = d;
        } else if (tv_ext_front(p->ext) == 0 && tv_ext_size(p->ext - TV_EXT_PRE) > TV_SMALL_MAX) {
            /* (a malloc block, growing: in place when the allocator can) */
            size_t size = (TV_EXT_PRE + (bytes ? bytes : 1) + 15) & ~(size_t)15;
            unsigned char *b = (unsigned char *)tv_realloc(p->ext - TV_EXT_PRE, size);
            memcpy(b, &size, sizeof size);
            p->ext = b + TV_EXT_PRE;
        } else {
            /* (room left at the front, or a small block: the elements move to the start of a new one) */
            unsigned char *d = tv_ext_new(0, bytes);
            if (es && p->len) memcpy(d, p->ext, (size_t)p->len * es);
            tv_ext_free(p->ext);
            p->ext = d;
        }
        p->cap = nc;
        return;
    }
    /* immortal (rc < 0): an array of its own, with the same elements */
    int64_t len = p->len;
    int64_t nc = min_cap > len ? tv_grow_cap(len, min_cap) : len;
    tv_arrbuf *q = tv_arrbuf_new(es, nc);
    tv_copy_retain(tv_arrbuf_data(q), tv_arrbuf_data(p), len, t);
    q->len = len;
    a->p = q;
}

tv_arr tv_arr_with_capacity(const tv_type *t, tv_int cap) {
    return tv_arr_of(tv_arrbuf_new(t->size, cap > 0 ? cap : 0), 0);
}

void *tv_arr_reserve_tail(tv_arr *a, const tv_type *t, tv_int n) {
    int64_t len = a->p ? a->p->len : 0;
    if (n < 0 || n > INT64_MAX - len) tv_trap("invalid array reserve count", NULL);
    tv_arr_make_unique(a, t, len + n);
    return tv_arrbuf_data(a->p) + (size_t)len * t->size;
}

_Noreturn void tv_arr_oob(tv_int i, tv_int len, const char *loc) {
    char msg[96];
    snprintf(msg, sizeof msg, "index %lld out of bounds for length %lld", (long long)i, (long long)len);
    tv_trap(msg, loc);
}

void *tv_arr_at_mut(tv_arr *a, const tv_type *t, tv_int i, const char *loc) {
    tv_int n = tv_arr_len(*a);
    if ((uint64_t)i >= (uint64_t)n) tv_arr_oob(i, n, loc);
    tv_arr_make_unique(a, t, 0);
    return tv_arrbuf_data(a->p) + (size_t)i * t->size;
}

void tv_arr_push(tv_arr *a, const tv_type *t, void *elem) {
    int64_t len = tv_arr_len(*a);
    tv_arr_make_unique(a, t, len + 1);
    if (t->size) memcpy(tv_arrbuf_data(a->p) + (size_t)len * t->size, elem, t->size);
    a->p->len = len + 1;
}

bool tv_arr_pop(tv_arr *a, const tv_type *t, void *out) {
    int64_t len = tv_arr_len(*a);
    if (len == 0) return false;
    tv_arr_make_unique(a, t, 0);
    size_t es = t->size;
    if (es) {
        unsigned char *src = tv_arrbuf_data(a->p) + (size_t)(len - 1) * es;
        if (out) memcpy(out, src, es);
        else if (t->release) t->release(src);
    }
    a->p->len = len - 1;
    return true;
}

void tv_arr_set_length(tv_arr *a, const tv_type *t, tv_int n, const char *loc) {
    int64_t len = tv_arr_len(*a);
    if (n == len) return;
    if (n < 0 || n > len) {
        if (n < 0) tv_trap("invalid array length", loc);
        /* (plain data, numbers and booleans: the new elements are zeros, as `new Array<number>(n)`
         * makes them) */
        if (!t->retain && !t->release) {
            if (n > INT32_MAX) tv_trap("Invalid array length", loc);
            void *w = tv_arr_reserve_tail(a, t, n - len);
            memset(w, 0, (size_t)(n - len) * t->size);
            a->p->len = n;
            return;
        }
        tv_trap("setting an array's length past its end (JavaScript would leave holes)", loc);
    }
    tv_arr_make_unique(a, t, 0);
    size_t es = t->size;
    if (es && t->release) {
        unsigned char *data = tv_arrbuf_data(a->p);
        for (int64_t i = n; i < len; i++) t->release(data + (size_t)i * es);
    }
    a->p->len = n;
}

/* `xs.length = n` past the end of an array whose elements may be `undefined`: the holes are
   `fill` (that `undefined`, which holds nothing to retain). */
void tv_arr_set_length_fill(tv_arr *a, const tv_type *t, tv_int n, const void *fill, const char *loc) {
    int64_t len = tv_arr_len(*a);
    if (n <= len) {
        tv_arr_set_length(a, t, n, loc);
        return;
    }
    if (n > INT32_MAX) tv_trap("invalid array length", loc);
    size_t es = t->size;
    /* (room for all of them at once, then copies of `fill`, which holds nothing to retain) */
    tv_arr_make_unique(a, t, n);
    if (es) {
        unsigned char *d = tv_arrbuf_data(a->p);
        for (int64_t i = len; i < n; i++) memcpy(d + (size_t)i * es, fill, es);
    }
    a->p->len = n;
}

/* (a queue taken from the front: once it's longer than this, its elements get a block of their
 * own, where taking the first one moves the start along instead of every other element down) */
enum { TV_SHIFT_INLINE = 32 };

bool tv_arr_shift(tv_arr *a, const tv_type *t, void *out) {
    int64_t len = tv_arr_len(*a);
    if (len == 0) return false;
    tv_arr_make_unique(a, t, 0);
    size_t es = t->size;
    tv_arrbuf *p = a->p;
    if (es && !p->ext && len > TV_SHIFT_INLINE) {
        tv_arr_make_unique(a, t, p->cap + 1);
        p = a->p;
    }
    if (es) {
        unsigned char *d = tv_arrbuf_data(p);
        if (out) memcpy(out, d, es);
        else if (t->release) t->release(d);
        if (p->ext) {
            p->ext = d + es;
            tv_ext_set_front(p->ext, tv_ext_front(d) + es);
            p->cap--;
        } else {
            memmove(d, d + es, (size_t)(len - 1) * es);
        }
    }
    p->len = len - 1;
    return true;
}

void tv_arr_unshift(tv_arr *a, const tv_type *t, void *elem) {
    int64_t len = tv_arr_len(*a);
    size_t es = t->size;
    tv_arr_make_unique(a, t, 0);
    tv_arrbuf *p = a->p;
    if (es && p->ext && tv_ext_front(p->ext) >= es) {
        /* room at the front */
        unsigned char *d = p->ext - es;
        tv_ext_set_front(d, tv_ext_front(p->ext) - es);
        p->ext = d;
        p->cap++;
        memcpy(d, elem, es);
        p->len = len + 1;
        return;
    }
    if (es && len >= TV_SHIFT_INLINE) {
        /* a block with as much room at the front as there are elements (and the room after them
         * there was): adding to the front again and again moves each element a few times, not
         * once an addition */
        size_t front = (size_t)len * es, back = (size_t)(p->cap - len) * es;
        unsigned char *d = tv_ext_new(front, (size_t)(len + 1) * es + back);
        memcpy(d + es, tv_arrbuf_data(p), (size_t)len * es);
        if (p->ext) {
            tv_ext_free(p->ext);
        } else {
            int64_t inl = p->cap;
            memcpy(p + 1, &inl, sizeof inl);
        }
        p->ext = d;
        p->cap = len + 1 + (int64_t)(back / es);
        memcpy(d, elem, es);
        p->len = len + 1;
        return;
    }
    tv_arr_make_unique(a, t, len + 1);
    p = a->p;
    if (es) {
        unsigned char *d = tv_arrbuf_data(p);
        memmove(d + es, d, (size_t)len * es);
        memcpy(d, elem, es);
    }
    p->len = len + 1;
}

tv_arr tv_arr_splice(tv_arr *a, const tv_type *t, tv_int start, tv_int del, bool has_del, tv_arr items) {
    int64_t len = tv_arr_len(*a);
    int64_t s = tv_clamp_index(start, len);
    int64_t d = has_del ? (del < 0 ? 0 : (del > len - s ? len - s : del)) : len - s;
    int64_t n = tv_arr_len(items);
    if (d == 0 && n == 0) return tv_arr_fresh();
    tv_arr_make_unique(a, t, len - d + n > len ? len - d + n : len);
    size_t es = t->size;
    tv_arr removed;
    if (d > 0) {
        tv_arrbuf *b = tv_arrbuf_new(es, d);
        if (es) memcpy(tv_arrbuf_data(b), tv_arrbuf_data(a->p) + (size_t)s * es, (size_t)d * es); /* moved out */
        removed = tv_arr_of(b, d);
    } else {
        removed = tv_arr_fresh();
    }
    if (es) {
        unsigned char *data = tv_arrbuf_data(a->p);
        memmove(data + (size_t)(s + n) * es, data + (size_t)(s + d) * es, (size_t)(len - s - d) * es);
        if (n) tv_copy_retain(data + (size_t)s * es, tv_arrbuf_data(items.p), n, t);
    }
    a->p->len = len - d + n;
    return removed;
}

tv_arr tv_arr_slice(tv_arr a, const tv_type *t, tv_int start, tv_int end, bool has_start, bool has_end) {
    int64_t len = tv_arr_len(a);
    int64_t s = has_start ? tv_clamp_index(start, len) : 0;
    int64_t e = has_end ? tv_clamp_index(end, len) : len;
    if (e <= s) return tv_arr_fresh();
    tv_arrbuf *b = tv_arrbuf_new(t->size, e - s);
    tv_copy_retain(tv_arrbuf_data(b), tv_arrbuf_data(a.p) + (size_t)s * t->size, e - s, t);
    return tv_arr_of(b, e - s);
}

tv_arr tv_arr_concat(tv_arr a, tv_arr b, const tv_type *t) {
    int64_t na = tv_arr_len(a), nb = tv_arr_len(b);
    if (na > INT64_MAX - nb) tv_trap("array too long", NULL);
    tv_arrbuf *r = tv_arrbuf_new(t->size, na + nb);
    tv_copy_retain(tv_arrbuf_data(r), tv_arrbuf_data(a.p), na, t);
    tv_copy_retain(tv_arrbuf_data(r) + (size_t)na * t->size, tv_arrbuf_data(b.p), nb, t);
    return tv_arr_of(r, na + nb);
}

tv_arr tv_arr_make(const tv_type *t, tv_int n, const void *elem, const char *loc) {
    if (n < 0) tv_trap("Invalid array length", loc);
    if (n == 0) return tv_arr_fresh();
    tv_arrbuf *b = tv_arrbuf_new(t->size, n);
    unsigned char *d = tv_arrbuf_data(b);
    size_t es = t->size;
    bool zero = true;
    for (size_t k = 0; k < es; k++) zero &= ((const unsigned char *)elem)[k] == 0;
    /* (a word of zeros, `null`/`undefined` of a reference type, holds nothing to retain) */
    if (zero && (!t->retain || es == sizeof(void *))) {
        memset(d, 0, (size_t)n * es);
    } else if (!t->retain && es == 1) {
        memset(d, *(const unsigned char *)elem, (size_t)n);
    } else if (!t->retain) {
        /* (values that hold nothing: the first copied in, then the filled part doubled) */
        size_t total = (size_t)n * es, done = es;
        memcpy(d, elem, es);
        while (done < total) {
            size_t c = done < total - done ? done : total - done;
            memcpy(d + done, d, c);
            done += c;
        }
    } else if (es == 8) {
        uint64_t w;
        memcpy(&w, elem, 8);
        for (tv_int i = 0; i < n; i++) memcpy(d + (size_t)i * 8, &w, 8);
        if (t->retain) tv_retain_n(d, n, t);
    } else {
        for (tv_int i = 0; i < n; i++) tv_copy_retain(d + (size_t)i * es, elem, 1, t);
    }
    return tv_arr_of(b, n);
}

bool tv_arr_set_from(tv_arr *a, const tv_type *t, tv_arr src, tv_int offset) {
    int64_t len = tv_arr_len(*a), n = tv_arr_len(src);
    if (offset < 0 || n > len - offset) return false;
    if (n == 0) return true;
    tv_arr_make_unique(a, t, len);
    memmove((unsigned char *)tv_arrbuf_data(a->p) + (size_t)offset * t->size, tv_arrbuf_data(src.p), (size_t)n * t->size);
    return true;
}

void tv_arr_fill(tv_arr *a, const tv_type *t, const void *elem, tv_int start, bool has_start, tv_int end, bool has_end) {
    int64_t len = tv_arr_len(*a);
    int64_t s = has_start ? tv_clamp_index(start, len) : 0, e = has_end ? tv_clamp_index(end, len) : len;
    if (s >= e) return;
    tv_arr_make_unique(a, t, len);
    unsigned char *d = tv_arrbuf_data(a->p);
    if (!t->release && !t->retain) {
        /* plain data: zeros and bytes by memset, the rest by whole-element stores */
        size_t n = (size_t)(e - s), es = t->size;
        unsigned char *p = d + (size_t)s * es;
        const unsigned char *b = elem;
        bool zero = true;
        for (size_t k = 0; k < es; k++) zero &= b[k] == 0;
        if (zero || es == 1) {
            memset(p, es ? b[0] : 0, n * es);
        } else if (es == 8) {
            uint64_t v;
            memcpy(&v, elem, 8);
            for (size_t i = 0; i < n; i++) memcpy(p + i * 8, &v, 8);
        } else if (es == 4) {
            uint32_t v;
            memcpy(&v, elem, 4);
            for (size_t i = 0; i < n; i++) memcpy(p + i * 4, &v, 4);
        } else {
            for (size_t i = 0; i < n; i++) memcpy(p + i * es, elem, es);
        }
        return;
    }
    for (int64_t i = s; i < e; i++) {
        if (t->release) t->release(d + (size_t)i * t->size);
        tv_copy_retain(d + (size_t)i * t->size, elem, 1, t);
    }
}

void tv_arr_extend(tv_arr *a, const tv_type *t, tv_arr b) {
    int64_t nb = tv_arr_len(b);
    if (nb == 0) return;
    unsigned char *w = tv_arr_reserve_tail(a, t, nb);
    tv_copy_retain(w, tv_arrbuf_data(b.p), nb, t);
    a->p->len += nb;
}

static void tv_swap_bytes(unsigned char *x, unsigned char *y, size_t n) {
    unsigned char tmp[64];
    while (n) {
        size_t c = n < sizeof tmp ? n : sizeof tmp;
        memcpy(tmp, x, c);
        memcpy(x, y, c);
        memcpy(y, tmp, c);
        x += c, y += c, n -= c;
    }
}

void tv_arr_reverse(tv_arr *a, const tv_type *t) {
    int64_t len = tv_arr_len(*a);
    size_t es = t->size;
    if (len < 2 || !es) return;
    tv_arr_make_unique(a, t, 0);
    unsigned char *d = tv_arrbuf_data(a->p);
    for (int64_t i = 0, j = len - 1; i < j; i++, j--) {
        if (es == 8) {
            uint64_t x, y;
            memcpy(&x, d + (size_t)i * 8, 8);
            memcpy(&y, d + (size_t)j * 8, 8);
            memcpy(d + (size_t)i * 8, &y, 8);
            memcpy(d + (size_t)j * 8, &x, 8);
        } else {
            tv_swap_bytes(d + (size_t)i * es, d + (size_t)j * es, es);
        }
    }
}

/* ------------------------------------------------------------------ stable merge sort */

typedef int (*tv_cmp3)(void *ctx, const void *a, const void *b); /* > 0 when a sorts after b */

static void tv_insertion_sort(unsigned char *base, size_t n, size_t es, tv_cmp3 cmp, void *ctx, unsigned char *tmp) {
    for (size_t i = 1; i < n; i++) {
        unsigned char *x = base + i * es;
        if (cmp(ctx, x - es, x) <= 0) continue;
        memcpy(tmp, x, es);
        size_t j = i - 1;
        while (j > 0 && cmp(ctx, base + (j - 1) * es, tmp) > 0) j--;
        memmove(base + (j + 1) * es, base + j * es, (i - j) * es);
        memcpy(base + j * es, tmp, es);
    }
}

/* tmp must hold at least max(1, n/2) elements. */
static void tv_merge_sort(unsigned char *base, size_t n, size_t es, tv_cmp3 cmp, void *ctx, unsigned char *tmp) {
    if (n <= 12) {
        tv_insertion_sort(base, n, es, cmp, ctx, tmp);
        return;
    }
    size_t h = n / 2;
    tv_merge_sort(base, h, es, cmp, ctx, tmp);
    tv_merge_sort(base + h * es, n - h, es, cmp, ctx, tmp);
    if (cmp(ctx, base + (h - 1) * es, base + h * es) <= 0) return; /* already in order */
    memcpy(tmp, base, h * es);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) {
        if (cmp(ctx, tmp + i * es, base + j * es) <= 0) memcpy(base + k++ * es, tmp + i++ * es, es);
        else memcpy(base + k++ * es, base + j++ * es, es);
    }
    if (i < h) memcpy(base + k * es, tmp + i * es, (h - i) * es);
}

typedef struct { double (*cmp)(void *ctx, const void *x, const void *y); void *ctx; } tv_user_cmp;

static int tv_user_cmp_fn(void *c, const void *a, const void *b) {
    tv_user_cmp *u = (tv_user_cmp *)c;
    double r = u->cmp(u->ctx, a, b);
    return r > 0 ? 1 : r < 0 ? -1 : 0; /* NaN counts as 0 (JS) */
}

static int tv_str_cmp_fn(void *c, const void *a, const void *b) {
    (void)c;
    return tv_str_cmp(*(const tv_str *)a, *(const tv_str *)b);
}

typedef struct { tv_str key; int64_t idx; } tv_sort_key;

void tv_arr_sort(tv_arr *a, const tv_type *t, double (*cmp)(void *ctx, const void *x, const void *y), void *ctx) {
    int64_t n = tv_arr_len(*a);
    size_t es = t->size;
    if (n < 2 || !es) return;
    tv_arr_make_unique(a, t, 0);
    unsigned char *d = tv_arrbuf_data(a->p);
    if (cmp || t == &tv_type_str) {
        unsigned char *tmp = (unsigned char *)tv_alloc(tv_size_mul_add((size_t)n / 2 + 1, es, 0));
        tv_user_cmp u = {cmp, ctx};
        if (cmp) tv_merge_sort(d, (size_t)n, es, tv_user_cmp_fn, &u, tmp);
        else tv_merge_sort(d, (size_t)n, es, tv_str_cmp_fn, NULL, tmp);
        free(tmp);
        return;
    }
    /* JS default order: compare String(x). */
    tv_sort_key *keys = (tv_sort_key *)tv_alloc(tv_size_mul_add((size_t)n, sizeof(tv_sort_key), 0));
    for (int64_t i = 0; i < n; i++) {
        tv_sb sb = {0};
        if (t != &tv_type_undefined && t != &tv_type_null) t->to_str(&sb, d + (size_t)i * es);
        keys[i].key = tv_str_from_sb(&sb);
        keys[i].idx = i;
    }
    tv_sort_key *tmpk = (tv_sort_key *)tv_alloc(((size_t)n / 2 + 1) * sizeof(tv_sort_key));
    tv_merge_sort((unsigned char *)keys, (size_t)n, sizeof(tv_sort_key), tv_str_cmp_fn, NULL, (unsigned char *)tmpk);
    free(tmpk);
    unsigned char *out = (unsigned char *)tv_alloc((size_t)n * es);
    for (int64_t i = 0; i < n; i++) {
        memcpy(out + (size_t)i * es, d + (size_t)keys[i].idx * es, es);
        tv_str_release(keys[i].key);
    }
    memcpy(d, out, (size_t)n * es);
    free(out);
    free(keys);
}

/* indexOf / lastIndexOf: the common elements compared here, not through the descriptor (a loop
 * the C compiler can unroll and vectorize; a string by its length before its bytes); integers
 * of every width by value, floats by == (NaN is never found, -0 finds 0). */
#define TV_FIND(T, EQ) do { \
        T x_; memcpy(&x_, elem, sizeof x_); const T *p_ = (const T *)(const void *)d; \
        if (last) { for (int64_t i = n - 1; i >= 0; i--) if (EQ(p_[i], x_)) return i; } \
        else { for (int64_t i = 0; i < n; i++) if (EQ(p_[i], x_)) return i; } \
        return -1; } while (0)
/* (numbers: eight at a time, the block's compares or-ed (vector compares), then the one found) */
#define TV_FIND_NUM(T) do { \
        T x_; memcpy(&x_, elem, sizeof x_); const T *p_ = (const T *)(const void *)d; \
        if (last) { \
            int64_t i = n; \
            for (; i >= 8; i -= 8) { \
                const T *b_ = p_ + i - 8; \
                if ((b_[0] == x_) | (b_[1] == x_) | (b_[2] == x_) | (b_[3] == x_) | (b_[4] == x_) | (b_[5] == x_) | (b_[6] == x_) | (b_[7] == x_)) break; \
            } \
            for (int64_t j = i - 1; j >= 0; j--) if (p_[j] == x_) return j; \
        } else { \
            int64_t i = 0; \
            for (; i + 8 <= n; i += 8) { \
                const T *b_ = p_ + i; \
                if ((b_[0] == x_) | (b_[1] == x_) | (b_[2] == x_) | (b_[3] == x_) | (b_[4] == x_) | (b_[5] == x_) | (b_[6] == x_) | (b_[7] == x_)) break; \
            } \
            for (; i < n; i++) if (p_[i] == x_) return i; \
        } \
        return -1; } while (0)
#define TV_EQ_PLAIN(a, b) ((a) == (b))
#define TV_EQ_STR(a, b) tv_str_eq(a, b)
static tv_int tv_arr_find(tv_arr a, const tv_type *t, const void *elem, bool last) {
    int64_t n = tv_arr_len(a);
    size_t es = t->size;
    const unsigned char *d = n ? tv_arrbuf_data(a.p) : NULL;
    if (!n) return -1;
    if (t == &tv_type_int || t == &tv_type_u64) TV_FIND_NUM(int64_t);
    if (t == &tv_type_f64) TV_FIND_NUM(double);
    if (t == &tv_type_str) TV_FIND(tv_str, TV_EQ_STR);
    if (t == &tv_type_i32) TV_FIND_NUM(int32_t);
    if (t == &tv_type_u32) TV_FIND_NUM(uint32_t);
    if (t == &tv_type_f32) TV_FIND_NUM(float);
    if (t == &tv_type_i16) TV_FIND_NUM(int16_t);
    if (t == &tv_type_u16) TV_FIND_NUM(uint16_t);
    if (t == &tv_type_i8) TV_FIND_NUM(int8_t);
    if (t == &tv_type_u8 || t == &tv_type_bool) TV_FIND_NUM(uint8_t);
    if (last) {
        for (int64_t i = n - 1; i >= 0; i--)
            if (t->eq(d + (size_t)i * es, elem)) return i;
    } else {
        for (int64_t i = 0; i < n; i++)
            if (t->eq(d + (size_t)i * es, elem)) return i;
    }
    return -1;
}

tv_int tv_arr_index_of(tv_arr a, const tv_type *t, const void *elem) { return tv_arr_find(a, t, elem, false); }

tv_int tv_arr_last_index_of(tv_arr a, const tv_type *t, const void *elem) { return tv_arr_find(a, t, elem, true); }

bool tv_arr_includes(tv_arr a, const tv_type *t, const void *elem) {
    /* (SameValueZero: NaN is found, as it isn't by indexOf) */
    if (t == &tv_type_f64 || t == &tv_type_f32) {
        double x = t == &tv_type_f64 ? *(const double *)elem : (double)*(const float *)elem;
        if (x != x) {
            int64_t n = tv_arr_len(a);
            for (int64_t i = 0; i < n; i++) {
                const unsigned char *e = tv_arrbuf_data(a.p) + (size_t)i * t->size;
                double y = t == &tv_type_f64 ? *(const double *)(const void *)e : (double)*(const float *)(const void *)e;
                if (y != y) return true;
            }
            return false;
        }
    }
    return tv_arr_index_of(a, t, elem) >= 0;
}


static void tv_join_into(tv_sb *sb, tv_arr a, const tv_type *t, const char *sep, size_t sep_len) {
    int64_t n = tv_arr_len(a);
    size_t es = t->size;
    for (int64_t i = 0; i < n; i++) {
        if (i) tv_sb_push(sb, sep, sep_len);
        if (t != &tv_type_undefined && t != &tv_type_null) t->to_str(sb, tv_arrbuf_data(a.p) + (size_t)i * es);
    }
}

tv_str tv_arr_join(tv_arr a, const tv_type *t, tv_str sep) {
    if (tv_arr_len(a) == 1 && t == &tv_type_str) {
        tv_str s = *(tv_str *)(void *)tv_arrbuf_data(a.p);
        tv_str_retain(s);
        return s;
    }
    if (t == &tv_type_str && tv_arr_len(a) > 1) {
        /* strings: the length first, then one buffer of it */
        int64_t n = tv_arr_len(a);
        const tv_str *xs = (const tv_str *)(void *)tv_arrbuf_data(a.p);
        size_t sl = (size_t)sep.p->len, total = sl * (size_t)(n - 1);
        int32_t all = sep.p->rc, any = sep.p->rc; /* what's known of the parts */
        for (int64_t i = 0; i < n; i++) {
            if (__builtin_add_overflow(total, (size_t)xs[i].p->len, &total)) tv_trap("string too long", NULL);
            all &= xs[i].p->rc;
            any |= xs[i].p->rc;
        }
        tv_strbuf *b = tv_strbuf_new(total);
        char *w = b->data;
        for (int64_t i = 0; i < n; i++) {
            if (i && sl) {
                memcpy(w, sep.p->data, sl);
                w += sl;
            }
            memcpy(w, xs[i].p->data, (size_t)xs[i].p->len);
            w += xs[i].p->len;
        }
        b->rc |= (all & TV_STR_ASCII) | (any & TV_STR_WIDE);
        return (tv_str){b};
    }
    tv_sb sb = {0};
    tv_join_into(&sb, a, t, sep.p->data, (size_t)sep.p->len);
    return tv_str_from_sb(&sb);
}

void tv_to_str_arr(tv_sb *sb, tv_arr a, const tv_type *t) { tv_join_into(sb, a, t, ",", 1); }

/* ================================================================== maps and sets */

/* One allocation: header, `cap` entries in insertion order, then 2*cap index slots.
 * Entry: uint64 hash (top bit set; 0 = deleted), key (padded to 8), value (padded to 8).
 * Index slot: entry number + 1 in the low bits (emask), bits of the hash above them (so a probe
 * passes most other keys without reading their entries); 0 = empty. Open addressing, linear
 * probing, backward-shift
 * deletion (so the index never holds tombstones); deleted entries are compacted away when
 * the entry array fills up. */
/* The store of a map's entries: replaced by a bigger one as the map grows (the header that
 * every reference holds stays). */
typedef struct tv_mapbuf tv_mapbuf;
struct tv_maphdr {
    tv_hcount rc;
    tv_mapbuf *b;     /* NULL while empty */
    int64_t object;   /* made as an object (`{ a: 1 }` for a Record<string, V>, JSON): printed as one */
};

struct tv_mapbuf {
    int64_t rc;       /* (unused: 1) */
    int64_t count;    /* live entries */
    int64_t used;     /* entries used, live or deleted */
    int64_t cap;      /* entry capacity (power of two) */
    uint64_t mask;    /* index slots - 1 */
    uint32_t *index;
    int64_t first;    /* entries before it are all deleted: iteration starts there */
    uint64_t emask;   /* an index slot's entry-number bits (header = 64 bytes: entries stay aligned) */
    unsigned char entries[];
};

#define TV_LIVE_BIT ((uint64_t)1 << 63)
#define TV_MAP_MIN_CAP 2

typedef struct { size_t voff, stride; } tv_mlayout;

/* An element's bytes copied: 8 of them (most keys and values) without a call to memcpy. */
static inline void tv_elem_copy(void *d, const void *s, size_t n) {
    if (n == 8) { uint64_t x; memcpy(&x, s, 8); memcpy(d, &x, 8); }
    else if (n) memcpy(d, s, n);
}

static inline tv_mlayout tv_mlay(const tv_type *kt, const tv_type *vt) {
    tv_mlayout L;
    L.voff = 8 + ((kt->size + 7) & ~(size_t)7);
    L.stride = L.voff + ((vt->size + 7) & ~(size_t)7);
    return L;
}

static inline unsigned char *tv_entry(const tv_mapbuf *m, tv_mlayout L, int64_t i) {
    return (unsigned char *)m->entries + (size_t)i * L.stride;
}
/* An index slot's hash bits, its entry number, and the slot for entry i of a key hashed h. */
static inline uint32_t tv_slot_tag(const tv_mapbuf *m, uint64_t h) { return (uint32_t)(h >> 31) & ~(uint32_t)m->emask; }
static inline int64_t tv_slot_entry(const tv_mapbuf *m, uint32_t s) { return (int64_t)(s & (uint32_t)m->emask) - 1; }
static inline uint32_t tv_slot_of(const tv_mapbuf *m, uint64_t h, int64_t i) { return tv_slot_tag(m, h) | (uint32_t)(i + 1); }
static inline uint64_t tv_entry_hash(const unsigned char *e) { uint64_t h; memcpy(&h, e, 8); return h; }
static inline void tv_entry_set_hash(unsigned char *e, uint64_t h) { memcpy(e, &h, 8); }

static inline double tv_canon_f64(double x) {
    if (x == 0) return 0.0;  /* -0 → 0 */
    if (x != x) return NAN;  /* one NaN */
    return x;
}

static inline __attribute__((always_inline)) uint64_t tv_key_hash(const tv_type *kt, const void *key) {
    if (kt == &tv_type_int) return tv_mix64(*(const uint64_t *)key) | TV_LIVE_BIT;
    if (kt == &tv_type_str) { /* (the common key: hashed here, not through the table) */
        const tv_strbuf *b = ((const tv_str *)key)->p;
        /* (a one-character key, `counts.get(ch)`: its byte mixed, without the general hash) */
        if (b->len == 1) return tv_mix64((unsigned char)b->data[0] ^ UINT64_C(0x9e3779b97f4a7c15)) | TV_LIVE_BIT;
        return tv_hash_bytes(b->data, (size_t)b->len) | TV_LIVE_BIT;
    }
    /* (the high bits folded down: an object key's hash is its address times a constant, whose
     * low bits are the same for every object of a size) */
    uint64_t h = kt->hash(key);
    return (h ^ (h >> 32)) | TV_LIVE_BIT;
}

/* Map keys use SameValueZero: like === except NaN equals NaN. */
static inline bool tv_key_eq(const tv_type *kt, const void *a, const void *b) {
    if (kt == &tv_type_str) {
        /* (keys are mostly short: compared inline rather than by a call to memcmp) */
        return tv_str_eq(*(const tv_str *)a, *(const tv_str *)b);
    }
    if (kt == &tv_type_int) return *(const int64_t *)a == *(const int64_t *)b;
    if (kt == &tv_type_f64) {
        double x = *(const double *)a, y = *(const double *)b;
        return x == y || (x != x && y != y);
    }
    if (kt == &tv_type_f32) {
        float x = *(const float *)a, y = *(const float *)b;
        return x == y || (x != x && y != y);
    }
    return kt->eq(a, b);
}

/* A store's size: small ones (most maps: 8 entries) come from the small-object free lists, in
 * sizes of 16 bytes so their entries stay aligned. */
enum { TV_MAPBUF_SMALL = 512 };
static size_t tv_mapbuf_bytes(int64_t cap, tv_mlayout L) {
    size_t bytes = tv_size_mul_add((size_t)cap, L.stride, sizeof(tv_mapbuf));
    bytes = tv_size_mul_add((size_t)cap * 2, sizeof(uint32_t), bytes);
    return bytes <= TV_MAPBUF_SMALL ? (bytes + 15) & ~(size_t)15 : bytes;
}
/* Big stores on macOS map their entries (after the header) and their index apart, not from
 * malloc: a store the map outgrows goes straight back (its allocator keeps freed large blocks
 * dirty, a k-mer count's every size), and the entries grow in place when they can, only the
 * index built again. */
#ifdef TV_SB_MAPS
#define TV_MAPBUF_MAPPED(bytes) ((bytes) >= ((size_t)128 << 10))
#else
#define TV_MAPBUF_MAPPED(bytes) false
#endif
static size_t tv_mapbuf_entry_bytes(int64_t cap, tv_mlayout L) {
    size_t page = (size_t)getpagesize();
    return (sizeof(tv_mapbuf) + (size_t)cap * L.stride + page - 1) & ~(page - 1);
}
static size_t tv_mapbuf_index_bytes(int64_t cap) {
    size_t page = (size_t)getpagesize();
    return ((size_t)cap * 2 * sizeof(uint32_t) + page - 1) & ~(page - 1);
}
static void *tv_map_pages(size_t size) { return tv_zero_pages(size); }

static void tv_mapbuf_free(tv_mapbuf *m, tv_mlayout L) {
    size_t bytes = tv_mapbuf_bytes(m->cap, L);
    if (bytes <= TV_MAPBUF_SMALL) tv_small_free(m, bytes);
    else if (TV_MAPBUF_MAPPED(bytes)) {
        munmap(m->index, tv_mapbuf_index_bytes(m->cap));
        munmap(m, tv_mapbuf_entry_bytes(m->cap, L));
    } else free(m);
}

static void tv_mapbuf_size(tv_mapbuf *m, int64_t cap) {
    m->cap = cap;
    m->mask = (uint64_t)cap * 2 - 1;
    m->emask = ((uint64_t)1 << (64 - __builtin_clzll((uint64_t)cap))) - 1;
}

static tv_mapbuf *tv_mapbuf_new(int64_t cap, tv_mlayout L) {
    if (cap > ((int64_t)1 << 31)) tv_trap("map too large", NULL);
    size_t islots = (size_t)cap * 2;
    size_t bytes = tv_mapbuf_bytes(cap, L);
    tv_mapbuf *m;
    if (TV_MAPBUF_MAPPED(bytes)) { /* (fresh pages: the index is zeros already) */
        m = (tv_mapbuf *)tv_map_pages(tv_mapbuf_entry_bytes(cap, L));
        m->index = (uint32_t *)tv_map_pages(tv_mapbuf_index_bytes(cap));
    } else {
        m = (tv_mapbuf *)(bytes <= TV_MAPBUF_SMALL ? tv_small_alloc(bytes) : tv_alloc(bytes));
        m->index = (uint32_t *)(void *)(m->entries + (size_t)cap * L.stride);
        memset(m->index, 0, islots * sizeof(uint32_t));
    }
    m->rc = 1;
    m->count = m->used = 0;
    m->first = 0;
    tv_mapbuf_size(m, cap);
    return m;
}

/* A mapped store grown to `cap` entries: its pages extended in place if the address space after
 * them is free, else moved (one copy, the old pages unmapped), and its index built again; NULL if
 * it isn't mapped. */
static tv_mapbuf *tv_mapbuf_grow(tv_mapbuf *m, tv_mlayout L, int64_t cap) {
    if (!TV_MAPBUF_MAPPED(tv_mapbuf_bytes(m->cap, L)) || cap > ((int64_t)1 << 31)) return NULL;
    size_t have = tv_mapbuf_entry_bytes(m->cap, L), want = tv_mapbuf_entry_bytes(cap, L);
    /* (the index first: it's often what lies just after the entries, and never both at once) */
    munmap(m->index, tv_mapbuf_index_bytes(m->cap));
    tv_mapbuf *n = (tv_mapbuf *)(void *)tv_sb_remap((char *)m, have, want);
    if (!n) {
        n = (tv_mapbuf *)tv_map_pages(want);
        memcpy(n, m, sizeof(tv_mapbuf) + (size_t)m->used * L.stride);
        munmap(m, have);
    }
    tv_mapbuf_size(n, cap);
    n->index = (uint32_t *)tv_map_pages(tv_mapbuf_index_bytes(cap));
    for (int64_t i = n->first; i < n->used; i++) {
        uint64_t h = tv_entry_hash(tv_entry(n, L, i));
        if (!h) continue;
        uint64_t pos = h & n->mask;
        while (n->index[pos]) pos = (pos + 1) & n->mask;
        n->index[pos] = tv_slot_of(n, h, i);
    }
    return n;
}

/* New buffer with the live entries of `old` (compacted). retain: clone (retain keys and
 * values) rather than move. */
static tv_mapbuf *tv_map_rebuild(const tv_mapbuf *old, tv_mlayout L, int64_t cap, bool retain,
                                 const tv_type *kt, const tv_type *vt) {
    tv_mapbuf *m = tv_mapbuf_new(cap, L);
    int64_t j = 0;
    for (int64_t i = 0; i < old->used; i++) {
        const unsigned char *src = tv_entry(old, L, i);
        uint64_t h = tv_entry_hash(src);
        if (!h) continue;
        unsigned char *dst = tv_entry(m, L, j);
        memcpy(dst, src, L.stride);
        if (retain) {
            if (kt->retain && kt->size) kt->retain(dst + 8);
            if (vt->retain && vt->size) vt->retain(dst + L.voff);
        }
        uint64_t pos = h & m->mask;
        while (m->index[pos]) pos = (pos + 1) & m->mask;
        m->index[pos] = tv_slot_of(m, h, j);
        j++;
    }
    m->count = m->used = j;
    return m;
}

static inline tv_mapbuf *tv_map_buf(tv_map m) { return m.p ? m.p->b : NULL; }

tv_map tv_map_fresh(void) {
    tv_maphdr *h = (tv_maphdr *)tv_small_alloc(sizeof(tv_maphdr));
    h->rc = 1;
    h->b = NULL;
    h->object = 0;
    return (tv_map){h};
}

tv_map tv_map_fresh_object(void) {
    tv_map m = tv_map_fresh();
    m.p->object = 1;
    return m;
}

tv_map tv_map_copy(tv_map m, const tv_type *kt, const tv_type *vt) {
    tv_map c = tv_map_fresh();
    tv_mapbuf *b = tv_map_buf(m);
    if (b && b->count) c.p->b = tv_map_rebuild(b, tv_mlay(kt, vt), b->cap, true, kt, vt);
    return c;
}

/* The store to change: the map's own (a place without one gets a new map first). */
static tv_mapbuf *tv_map_unique(tv_map *mp, const tv_type *kt, const tv_type *vt, tv_mlayout L) {
    (void)kt, (void)vt;
    if (!mp->p) *mp = tv_map_fresh();
    if (!mp->p->b) mp->p->b = tv_mapbuf_new(TV_MAP_MIN_CAP, L);
    return mp->p->b;
}

/* Index slot holding `key` (*found = true), or the empty slot where it would go. */
static uint64_t tv_map_find(const tv_mapbuf *m, tv_mlayout L, const tv_type *kt, const void *key, uint64_t h,
                            bool *found) {
    uint64_t i = h & m->mask;
    uint32_t tag = tv_slot_tag(m, h), emask = (uint32_t)m->emask;
    for (;;) {
        uint32_t e = m->index[i];
        if (!e) { *found = false; return i; }
        if ((e & ~emask) == tag) {
            const unsigned char *ent = tv_entry(m, L, (int64_t)(e & emask) - 1);
            if (tv_entry_hash(ent) == h && tv_key_eq(kt, ent + 8, key)) { *found = true; return i; }
        }
        i = (i + 1) & m->mask;
    }
}

void tv_map_retain(tv_map m) {
    if (m.p && m.p->rc >= 0) m.p->rc++;
}

static void tv_map_release_entries(tv_mapbuf *m, const tv_type *kt, const tv_type *vt, tv_mlayout L) {
    bool rk = kt->release && kt->size, rv = vt->release && vt->size;
    if (!rk && !rv) return;
    /* (string keys and values released here, not through the descriptor: an immortal literal's is
     * a check) */
    bool sk = kt == &tv_type_str, sv = vt == &tv_type_str;
    for (int64_t i = 0; i < m->used; i++) {
        unsigned char *e = tv_entry(m, L, i);
        if (!tv_entry_hash(e)) continue;
        if (sk) { tv_str x; memcpy(&x, e + 8, sizeof x); tv_str_release(x); }
        else if (rk) kt->release(e + 8);
        if (sv) { tv_str x; memcpy(&x, e + L.voff, sizeof x); tv_str_release(x); }
        else if (rv) vt->release(e + L.voff);
    }
}

void tv_map_release(tv_map m, const tv_type *kt, const tv_type *vt) {
    if (!m.p || m.p->rc <= 0 || --m.p->rc > 0) return;
    if (m.p->b) {
        tv_mlayout L = tv_mlay(kt, vt);
        tv_map_release_entries(m.p->b, kt, vt, L);
        tv_mapbuf_free(m.p->b, L);
    }
    tv_small_free(m.p, sizeof(tv_maphdr));
}

bool tv_map_is_object(tv_map m) { return m.p && m.p->object; }

tv_int tv_map_size(tv_map m) { tv_mapbuf *b = tv_map_buf(m); return b ? b->count : 0; }

void *tv_map_get(tv_map mm, const tv_type *kt, const tv_type *vt, const void *key) {
    tv_mapbuf *m = tv_map_buf(mm);
    if (!m || !m->count) return NULL;
    tv_mlayout L = tv_mlay(kt, vt);
    /* (a few string keys, a record's: the same string as the key, a literal passed along, found
     * by its address before any hashing) */
    if (kt == &tv_type_str && m->used <= 8) {
        const void *kp = *(void *const *)key;
        for (int64_t i = 0; i < m->used; i++) {
            unsigned char *e = tv_entry(m, L, i);
            if (*(void *const *)(e + 8) == kp && tv_entry_hash(e)) return e + L.voff;
        }
    }
    bool found;
    uint64_t pos = tv_map_find(m, L, kt, key, tv_key_hash(kt, key), &found);
    return found ? tv_entry(m, L, tv_slot_entry(m, m->index[pos])) + L.voff : NULL;
}

bool tv_map_has(tv_map m, const tv_type *kt, const tv_type *vt, const void *key) {
    return tv_map_get(m, kt, vt, key) != NULL;
}

/* A new entry for a key not in the (unique) map, whose empty index slot is `pos`: grows the map
 * when it's full. The entry's key and value are the caller's to fill in. */
static unsigned char *tv_map_add(tv_map *mp, tv_mapbuf *m, tv_mlayout L, const tv_type *kt, const tv_type *vt, uint64_t h, uint64_t pos) {
    if (m->used == m->cap) {
        /* full: compact if at least half the entries are deleted, else double */
        int64_t ncap = m->count * 2 <= m->cap ? m->cap : m->cap * 2;
        tv_mapbuf *n = ncap > m->cap ? tv_mapbuf_grow(m, L, ncap) : NULL;
        if (!n) {
            n = tv_map_rebuild(m, L, ncap, false, kt, vt);
            tv_mapbuf_free(m, L);
        }
        mp->p->b = m = n;
        pos = h & m->mask;
        while (m->index[pos]) pos = (pos + 1) & m->mask;
    }
    int64_t idx = m->used++;
    unsigned char *e = tv_entry(m, L, idx);
    tv_entry_set_hash(e, h);
    m->index[pos] = tv_slot_of(m, h, idx);
    m->count++;
    return e;
}

tv_map tv_map_from_entries(const tv_type *kt, const tv_type *vt, tv_int n, const void *keys, const void *vals, bool object, uint64_t *hashes) {
    tv_map mm = tv_map_fresh();
    mm.p->object = object;
    if (n <= 0) return mm;
    tv_mlayout L = tv_mlay(kt, vt);
    int64_t cap = TV_MAP_MIN_CAP;
    while (cap < n) cap *= 2;
    tv_mapbuf *m = tv_mapbuf_new(cap, L);
    const unsigned char *k = keys, *v = vals;
    /* (the keys' hashes, kept by the literal's site from its first time: its keys are constants) */
    if (hashes && !hashes[0])
        for (int64_t i = 0; i < n; i++) hashes[i] = tv_key_hash(kt, k + (size_t)i * kt->size);
    for (int64_t i = 0; i < n; i++, k += kt->size, v += vt->size) {
        uint64_t h = hashes ? hashes[i] : tv_key_hash(kt, k);
        uint64_t pos = h & m->mask;
        while (m->index[pos]) pos = (pos + 1) & m->mask;
        unsigned char *e = tv_entry(m, L, i);
        tv_entry_set_hash(e, h);
        tv_elem_copy(e + 8, k, kt->size);
        tv_elem_copy(e + L.voff, v, vt->size);
        m->index[pos] = tv_slot_of(m, h, i);
    }
    m->count = m->used = n;
    mm.p->b = m;
    return mm;
}

void tv_map_set(tv_map *mp, const tv_type *kt, const tv_type *vt, void *key, void *value) {
    tv_mlayout L = tv_mlay(kt, vt);
    tv_mapbuf *m = tv_map_unique(mp, kt, vt, L);
    uint64_t h = tv_key_hash(kt, key);
    bool found;
    uint64_t pos = tv_map_find(m, L, kt, key, h, &found);
    if (found) { /* replace the value in place; keep the original key */
        unsigned char *e = tv_entry(m, L, tv_slot_entry(m, m->index[pos]));
        if (vt->size) {
            if (vt->release) vt->release(e + L.voff);
            tv_elem_copy(e + L.voff, value, vt->size);
        }
        if (kt->release && kt->size) kt->release(key);
        return;
    }
    unsigned char *e = tv_map_add(mp, m, L, kt, vt, h, pos);
    tv_elem_copy(e + 8, key, kt->size);
    tv_elem_copy(e + L.voff, value, vt->size);
}

/* Find-or-insert, for `m.set(k, (m.get(k) ?? d) op e)`: one probe. */
void *tv_map_get_mut(tv_map *mp, const tv_type *kt, const tv_type *vt, const void *key) {
    if (!tv_map_buf(*mp)) return NULL;
    tv_mlayout L = tv_mlay(kt, vt);
    tv_mapbuf *m = tv_map_unique(mp, kt, vt, L);
    bool found;
    uint64_t pos = tv_map_find(m, L, kt, key, tv_key_hash(kt, key), &found);
    return found ? tv_entry(m, L, tv_slot_entry(m, m->index[pos])) + L.voff : NULL;
}

void *tv_map_slot(tv_map *mp, const tv_type *kt, const tv_type *vt, void *key, bool *inserted) {
    tv_mlayout L = tv_mlay(kt, vt);
    tv_mapbuf *m = tv_map_unique(mp, kt, vt, L);
    uint64_t h = tv_key_hash(kt, key);
    bool found;
    uint64_t pos = tv_map_find(m, L, kt, key, h, &found);
    if (found) {
        if (kt->release && kt->size) kt->release(key);
        *inserted = false;
        return tv_entry(m, L, tv_slot_entry(m, m->index[pos])) + L.voff;
    }
    unsigned char *e = tv_map_add(mp, m, L, kt, vt, h, pos);
    tv_elem_copy(e + 8, key, kt->size);
    memset(e + L.voff, 0, vt->size);
    *inserted = true;
    return e + L.voff;
}

/* The slot for the key with bytes p[0, n); a new key is `key` when given (owned), else a copy. */
static inline __attribute__((always_inline)) void *tv_map_slot_bytes(tv_map *mp, const tv_type *vt, const char *p, size_t n, tv_str key, bool given, bool ascii, bool *inserted) {
    const tv_type *kt = &tv_type_str;
    tv_mlayout L = tv_mlay(kt, vt);
    tv_mapbuf *m = tv_map_unique(mp, kt, vt, L);
    uint64_t h = tv_hash_bytes(p, n) | TV_LIVE_BIT;
    uint64_t pos = h & m->mask;
    uint32_t tag = tv_slot_tag(m, h), emask = (uint32_t)m->emask;
    for (;;) {
        uint32_t i = m->index[pos];
        if (!i) break;
        unsigned char *e = tv_entry(m, L, (int64_t)(i & emask) - 1);
        if ((i & ~emask) == tag && tv_entry_hash(e) == h) {
            tv_str k;
            memcpy(&k, e + 8, sizeof k);
            if ((size_t)k.p->len == n && tv_mem_eq(k.p->data, p, n)) {
                if (given) tv_str_release(key);
                *inserted = false;
                return e + L.voff;
            }
        }
        pos = (pos + 1) & m->mask;
    }
    tv_str k = given ? key : tv_str_from(p, n);
    if (ascii) tv_str_mark(k, true);
    unsigned char *e = tv_map_add(mp, m, L, kt, vt, h, pos);
    memcpy(e + 8, &k, sizeof k);
    memset(e + L.voff, 0, vt->size);
    *inserted = true;
    return e + L.voff;
}

/* (a non-ASCII string: its unit indexes convert, and a slice that cuts a surrogate pair isn't
 * bytes of s) */
__attribute__((noinline)) static void *tv_map_slot_slice_wide(tv_map *mp, const tv_type *vt, tv_str s, tv_int start, tv_int end, bool has_end, bool *inserted) {
    int64_t len = tv_str_wide_length(s.p);
    int64_t a = tv_clamp_index(start, len);
    int64_t b = has_end ? tv_clamp_index(end, len) : len;
    if (a >= b) return tv_map_slot_bytes(mp, vt, s.p->data, 0, TV_EMPTY_STR, false, true, inserted);
    size_t x, y;
    bool pre, post;
    tv_unit_range(s, a, b, &x, &y, &pre, &post);
    if (!pre && !post) return tv_map_slot_bytes(mp, vt, s.p->data + x, y - x, TV_EMPTY_STR, false, false, inserted);
    tv_str cut = tv_str_units(s, a, b);
    return tv_map_slot_bytes(mp, vt, cut.p->data, (size_t)cut.p->len, cut, true, false, inserted);
}

void *tv_map_slot_slice(tv_map *mp, const tv_type *vt, tv_str s, tv_int start, tv_int end, bool has_end, const char *loc, bool *inserted) {
    (void)loc;
    if (TV_UNLIKELY(!tv_str_is_ascii(s))) return tv_map_slot_slice_wide(mp, vt, s, start, end, has_end, inserted);
    int64_t len = s.p->len;
    int64_t a = tv_clamp_index(start, len);
    int64_t b = has_end ? tv_clamp_index(end, len) : len;
    if (b < a) b = a;
    return tv_map_slot_bytes(mp, vt, s.p->data + a, (size_t)(b - a), TV_EMPTY_STR, false, true, inserted);
}

bool tv_map_delete(tv_map *mp, const tv_type *kt, const tv_type *vt, const void *key) {
    tv_mapbuf *m = tv_map_buf(*mp);
    if (!m || !m->count) return false;
    tv_mlayout L = tv_mlay(kt, vt);
    uint64_t h = tv_key_hash(kt, key);
    bool found;
    uint64_t pos = tv_map_find(m, L, kt, key, h, &found);
    if (!found) return false;
    int64_t at = tv_slot_entry(m, m->index[pos]);
    unsigned char *e = tv_entry(m, L, at);
    tv_entry_set_hash(e, 0);
    m->count--;
    /* (deleting the oldest, as an LRU cache does: later reads of the first skip it at once) */
    if (at == m->first) {
        while (m->first < m->used && !tv_entry_hash(tv_entry(m, L, m->first))) m->first++;
    }
    /* backward-shift deletion in the index */
    uint64_t mask = m->mask, i = pos, j = pos;
    for (;;) {
        j = (j + 1) & mask;
        uint32_t s = m->index[j];
        if (!s) break;
        uint64_t home = tv_entry_hash(tv_entry(m, L, tv_slot_entry(m, s))) & mask;
        if (((j - home) & mask) >= ((j - i) & mask)) {
            m->index[i] = s;
            i = j;
        }
    }
    m->index[i] = 0;
    if (m->count == 0) m->used = m->first = 0; /* index is empty too */
    /* release last: a release callback must see a consistent map */
    if (kt->release && kt->size) kt->release(e + 8);
    if (vt->release && vt->size) vt->release(e + L.voff);
    return true;
}

/* Empties the map (for every reference to it). */
void tv_map_clear(tv_map *m, const tv_type *kt, const tv_type *vt) {
    tv_mapbuf *b = tv_map_buf(*m);
    if (!b) return;
    m->p->b = NULL; /* (a release callback sees the map empty) */
    tv_mlayout L = tv_mlay(kt, vt);
    tv_map_release_entries(b, kt, vt, L);
    tv_mapbuf_free(b, L);
}

static tv_arr tv_map_column(tv_map mm, const tv_type *kt, const tv_type *vt, bool values) {
    tv_mapbuf *m = tv_map_buf(mm);
    if (!m || !m->count) return tv_arr_fresh();
    tv_mlayout L = tv_mlay(kt, vt);
    const tv_type *t = values ? vt : kt;
    size_t off = values ? L.voff : 8, es = t->size;
    tv_arrbuf *b = tv_arrbuf_new(es, m->count);
    int64_t j = 0;
    /* (a word each, a string's, an object's or a number's: copied here, then retained together,
     * not through calls) */
    if (es == 8 && (t == &tv_type_str || !t->retain || t->retain == tv_retain_rc32 || t->retain == tv_retain_rc64)) {
        uint64_t *out = (uint64_t *)(void *)tv_arrbuf_data(b);
        for (int64_t i = 0; i < m->used; i++) {
            unsigned char *e = tv_entry(m, L, i);
            if (!tv_entry_hash(e)) continue;
            uint64_t w;
            memcpy(&w, e + off, 8);
            out[j++] = w;
        }
        if (t->retain) tv_retain_n((unsigned char *)out, j, t);
        return tv_arr_of(b, j);
    }
    for (int64_t i = 0; i < m->used; i++) {
        unsigned char *e = tv_entry(m, L, i);
        if (!tv_entry_hash(e)) continue;
        if (es) {
            memcpy(tv_arrbuf_data(b) + (size_t)j * es, e + off, es);
            if (t->retain) t->retain(tv_arrbuf_data(b) + (size_t)j * es);
        }
        j++;
    }
    return tv_arr_of(b, j);
}

tv_arr tv_map_keys(tv_map m, const tv_type *kt, const tv_type *vt) { return tv_map_column(m, kt, vt, false); }
tv_arr tv_map_values(tv_map m, const tv_type *kt, const tv_type *vt) { return tv_map_column(m, kt, vt, true); }

bool tv_map_next(tv_map mm, const tv_type *kt, const tv_type *vt, tv_int *cursor, void **key, void **value) {
    tv_mapbuf *m = tv_map_buf(mm);
    if (!m) return false;
    tv_mlayout L = tv_mlay(kt, vt);
    for (int64_t i = *cursor < m->first ? m->first : *cursor; i < m->used; i++) {
        unsigned char *e = tv_entry(m, L, i);
        if (!tv_entry_hash(e)) continue;
        if (key) *key = e + 8;
        if (value) *value = e + L.voff;
        *cursor = i + 1;
        return true;
    }
    *cursor = m->used;
    return false;
}

bool tv_map_eq(tv_map a, tv_map b, const tv_type *kt, const tv_type *vt) {
    if (a.p == b.p) return true;
    if (tv_map_size(a) != tv_map_size(b)) return false;
    void *k, *v;
    for (tv_int i = 0; tv_map_next(a, kt, vt, &i, &k, &v);) {
        void *w = tv_map_get(b, kt, vt, k);
        if (!w) return false;
        if (vt->size && !vt->eq(v, w)) return false;
    }
    return true;
}

/* ================================================================== console formatting */

/* A port of the layout rules of Node's util.inspect (lib/internal/util/inspect.js) with the
 * options console.log uses: breakLength 80, compact 3, depth 2, maxArrayLength 100,
 * maxStringLength 10000. A container formats each child into a list of pieces (Node's
 * `output` array), then tv_reduce_to_single_string lays them out on one line or several.
 * The `depth` argument of the inspect functions is Node's `recurseTimes`. */
#define TV_BREAK_LENGTH 80
#define TV_COMPACT 3
#define TV_INSPECT_DEPTH 2
#define TV_MAX_ARRAY_LENGTH 100
#define TV_MAX_STRING_LENGTH 10000
#define TV_MIN_LINE_LENGTH 16

/* Node's ctx.indentationLvl and ctx.currentDepth (single-threaded, reset at depth 0). */
static struct { int64_t indent, current_depth; } tv_ictx;

typedef struct {
    tv_sb text;    /* all pieces, back to back */
    size_t *ends;  /* end offset of each piece */
    size_t n, cap;
} tv_pieces;

static void tv_pieces_mark(tv_pieces *p) { /* ends the current piece */
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 8;
        p->ends = (size_t *)tv_realloc(p->ends, p->cap * sizeof(size_t));
    }
    p->ends[p->n++] = p->text.len;
}

static const char *tv_piece(const tv_pieces *p, size_t i, size_t *len) {
    size_t start = i ? p->ends[i - 1] : 0;
    *len = p->ends[i] - start;
    return p->text.data ? p->text.data + start : "";
}

static void tv_pieces_free(tv_pieces *p) {
    tv_sb_free(&p->text);
    free(p->ends);
}

static void tv_push_spaces(tv_sb *sb, int64_t n) {
    if (n <= 0) return;
    memset(tv_sb_reserve(sb, (size_t)n), ' ', (size_t)n);
    sb->len += (size_t)n;
}

/* JavaScript string length (UTF-16 code units) of UTF-8 text. */
static size_t tv_utf16_len(const char *s, size_t n) {
    size_t u = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        u += (c & 0xC0) != 0x80;
        u += c >= 0xF0; /* surrogate pair */
    }
    return u;
}

/* Code points Node's getStringWidth (ICU) reports as 2 columns wide, generated from Node. */
static const uint32_t tv_wide_ranges[][2] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0},
    {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2630, 0x2637}, {0x2648, 0x2653},
    {0x267F, 0x267F}, {0x268A, 0x268F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB},
    {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705},
    {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C},
    {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x2E99}, {0x2E9B, 0x2EF3}, {0x2F00, 0x2FD5},
    {0x2FF0, 0x303E}, {0x3041, 0x3096}, {0x3099, 0x30FF}, {0x3105, 0x312F}, {0x3131, 0x318E},
    {0x3190, 0x31E5}, {0x31EF, 0x321E}, {0x3220, 0x3247}, {0x3250, 0xA48C}, {0xA490, 0xA4C6},
    {0xA960, 0xA97C}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE52},
    {0xFE54, 0xFE66}, {0xFE68, 0xFE6B}, {0xFF01, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4},
    {0x16FF0, 0x16FF6}, {0x17000, 0x18CD5}, {0x18CFF, 0x18D1E}, {0x18D80, 0x18DF2}, {0x1AFF0, 0x1AFF3},
    {0x1AFF5, 0x1AFFB}, {0x1AFFD, 0x1AFFE}, {0x1B000, 0x1B122}, {0x1B132, 0x1B132}, {0x1B150, 0x1B152},
    {0x1B155, 0x1B155}, {0x1B164, 0x1B167}, {0x1B170, 0x1B2FB}, {0x1D15E, 0x1D164}, {0x1D1BB, 0x1D1C0},
    {0x1D300, 0x1D356}, {0x1D360, 0x1D376}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F1E6, 0x1F202}, {0x1F210, 0x1F23B}, {0x1F240, 0x1F248}, {0x1F250, 0x1F251},
    {0x1F260, 0x1F265}, {0x1F300, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E},
    {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567},
    {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D8}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FA7C}, {0x1FA80, 0x1FA8A}, {0x1FA8E, 0x1FAC6}, {0x1FAC8, 0x1FAC8},
    {0x1FACD, 0x1FADC}, {0x1FADF, 0x1FAEA}, {0x1FAEF, 0x1FAF8}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

static bool tv_is_wide(uint32_t c) {
    size_t lo = 0, hi = sizeof tv_wide_ranges / sizeof *tv_wide_ranges;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < tv_wide_ranges[mid][0]) hi = mid;
        else if (c > tv_wide_ranges[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}

static bool tv_is_zero_width(uint32_t c) { /* Node's isZeroWidthCodePoint */
    return c <= 0x1F || (c >= 0x7F && c <= 0x9F) || (c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) ||
           (c >= 0xE0100 && c <= 0xE01EF);
}

/* Terminal columns (Node's getStringWidth). */
static size_t tv_str_width(const char *s, size_t n) {
    size_t w = 0, i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            w += c >= 32 && c != 0x7F;
            i++;
            continue;
        }
        uint32_t cp;
        i += tv_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        w += tv_is_wide(cp) ? 2 : tv_is_zero_width(cp) ? 0 : 1;
    }
    return w;
}

static bool tv_has_newline(const char *s, size_t n) { return n && memchr(s, '\n', n) != NULL; }

/* Node's strEscape: quote with ' unless the text contains ' (then " if possible, else `
 * if the text has neither ` nor "${"); escape control characters and the backslash. */
static void tv_push_quoted(tv_sb *sb, const char *d, size_t n) {
    char q = '\'';
    if (n && memchr(d, '\'', n)) {
        if (!memchr(d, '"', n)) q = '"';
        else if (!memchr(d, '`', n) && tv_find(d, n, "${", 2, 0) < 0) q = '`';
    }
    tv_sb_push_char(sb, q);
    size_t run = 0; /* start of the pending unescaped run */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)d[i];
        bool c1 = c == 0xC2 && i + 1 < n && (unsigned char)d[i + 1] <= 0x9F; /* U+0080..U+009F */
        if (c >= 0x20 && c != 0x7F && c != '\\' && !(c == '\'' && q == '\'') && !c1) continue;
        tv_sb_push(sb, d + run, i - run);
        char esc[8];
        switch (c) {
        case '\n': tv_sb_push(sb, "\\n", 2); break;
        case '\t': tv_sb_push(sb, "\\t", 2); break;
        case '\r': tv_sb_push(sb, "\\r", 2); break;
        case '\b': tv_sb_push(sb, "\\b", 2); break;
        case '\f': tv_sb_push(sb, "\\f", 2); break;
        case '\\': tv_sb_push(sb, "\\\\", 2); break;
        case '\'': tv_sb_push(sb, "\\'", 2); break;
        default:
            snprintf(esc, sizeof esc, "\\x%02X", c1 ? (unsigned char)d[i + 1] : c);
            tv_sb_push(sb, esc, 4);
            if (c1) i++;
        }
        run = i + 1;
    }
    tv_sb_push(sb, d + run, n - run);
    tv_sb_push_char(sb, q);
}

void tv_inspect_str(tv_sb *sb, tv_str s, int depth) {
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len;
    if (depth == 0) { tv_sb_push(sb, d, n); return; }
    /* formatPrimitive: truncate after maxStringLength code units ... */
    size_t units = tv_utf16_len(d, n), remaining = 0;
    if (units > TV_MAX_STRING_LENGTH) {
        size_t i = 0, u = 0;
        while (i < n) {
            size_t l = tv_utf8_len((unsigned char)d[i]), w = l == 4 ? 2 : 1;
            if (u + w > TV_MAX_STRING_LENGTH) break;
            u += w;
            i += l;
        }
        remaining = units - TV_MAX_STRING_LENGTH;
        units = u;
        n = i;
    }
    /* ... and split long strings after each newline into 'a\n' + 'b' */
    if (units > TV_MIN_LINE_LENGTH && (int64_t)units > TV_BREAK_LENGTH - tv_ictx.indent - 4 && tv_has_newline(d, n)) {
        size_t start = 0;
        while (start < n) {
            const char *nl = (const char *)memchr(d + start, '\n', n - start);
            size_t end = nl ? (size_t)(nl - d) + 1 : n;
            if (start) {
                tv_sb_push(sb, " +\n", 3);
                tv_push_spaces(sb, tv_ictx.indent + 2);
            }
            tv_push_quoted(sb, d + start, end - start);
            start = end;
        }
    } else {
        tv_push_quoted(sb, d, n);
    }
    if (remaining) {
        tv_sb_push_cstr(sb, "... ");
        tv_sb_push_int(sb, (tv_int)remaining);
        tv_sb_push_cstr(sb, remaining > 1 ? " more characters" : " more character");
    }
}

static void tv_push_more_items(tv_pieces *out, int64_t more) {
    tv_sb_push_cstr(&out->text, "... ");
    tv_sb_push_int(&out->text, more);
    tv_sb_push_cstr(&out->text, more > 1 ? " more items" : " more item");
    tv_pieces_mark(out);
}

/* Node's groupArrayElements: lays out > 6 array entries in aligned columns. Returns false
 * (leaving `res` empty) when the entries should not be grouped. */
static bool tv_group_array_elements(const tv_pieces *out, tv_pieces *res, bool numbers) {
    size_t total = 0, max_len = 0, n = out->n, out_len = n, len;
    if (n > TV_MAX_ARRAY_LENGTH) out_len--; /* leave "... more items" out */
    size_t *width = (size_t *)tv_alloc(out_len * 2 * sizeof(size_t)), *units = width + out_len;
    for (size_t i = 0; i < out_len; i++) {
        const char *s = tv_piece(out, i, &len);
        width[i] = tv_str_width(s, len);
        units[i] = tv_utf16_len(s, len);
        total += width[i] + 2;
        if (max_len < width[i]) max_len = width[i];
    }
    int64_t actual_max = (int64_t)max_len + 2; /* + ", " */
    bool grouped = false;
    if (actual_max * 3 + tv_ictx.indent < TV_BREAK_LENGTH &&
        ((double)total / (double)actual_max > 5 || max_len <= 6)) {
        double average_bias = sqrt((double)actual_max - (double)total / (double)n);
        double biased_max = fmax((double)actual_max - 3 - average_bias, 1);
        double c = floor(sqrt(2.5 * biased_max * (double)out_len) / biased_max + 0.5); /* Math.round */
        int64_t columns = (int64_t)c;
        int64_t fit = (TV_BREAK_LENGTH - tv_ictx.indent) / actual_max;
        if (fit < columns) columns = fit;
        if (TV_COMPACT * 4 < columns) columns = TV_COMPACT * 4;
        if (15 < columns) columns = 15;
        if (columns > 1) {
            grouped = true;
            size_t cols = (size_t)columns;
            size_t *line_max = (size_t *)tv_alloc(cols * sizeof(size_t));
            for (size_t i = 0; i < cols; i++) {
                size_t m = 0;
                for (size_t j = i; j < out_len; j += cols)
                    if (width[j] > m) m = width[j];
                line_max[i] = m + 2;
            }
            for (size_t i = 0; i < out_len; i += cols) {
                size_t max = i + cols < out_len ? i + cols : out_len, j = i;
                for (; j < max - 1; j++) { /* "entry, " padded to the column width */
                    const char *s = tv_piece(out, j, &len);
                    int64_t pad = (int64_t)line_max[j - i] - (int64_t)width[j] - 2;
                    if (numbers) tv_push_spaces(&res->text, pad);
                    tv_sb_push(&res->text, s, len);
                    tv_sb_push(&res->text, ", ", 2);
                    if (!numbers) tv_push_spaces(&res->text, pad);
                }
                const char *s = tv_piece(out, j, &len);
                if (numbers) tv_push_spaces(&res->text, (int64_t)line_max[j - i] - (int64_t)width[j] - 2);
                tv_sb_push(&res->text, s, len);
                tv_pieces_mark(res);
            }
            if (out_len < n) {
                const char *s = tv_piece(out, out_len, &len);
                tv_sb_push(&res->text, s, len);
                tv_pieces_mark(res);
            }
            free(line_max);
        }
    }
    free(width);
    return grouped;
}

/* Node's isBelowBreakLength (lengths in UTF-16 code units, like JS .length). */
static bool tv_is_below_break_length(const tv_pieces *out, size_t start) {
    size_t total = out->n + start, len;
    if (total + out->n > TV_BREAK_LENGTH) return false;
    for (size_t i = 0; i < out->n; i++) {
        const char *s = tv_piece(out, i, &len);
        total += tv_utf16_len(s, len);
        if (total > TV_BREAK_LENGTH) return false;
    }
    return true;
}

/* Node's reduceToSingleString (compact = 3, base = ""). Appends to sb and frees out.
 * recurse_times is the container's depth + 1. */
static void tv_reduce_to_single_string(tv_sb *sb, tv_pieces *out, const char *brace0, const char *brace1,
                                       bool is_array, int64_t recurse_times, bool numbers) {
    size_t entries = out->n, len;
    tv_pieces grouped = {0};
    const tv_pieces *o = out;
    if (is_array && entries > 6 && tv_group_array_elements(out, &grouped, numbers)) o = &grouped;
    size_t b0 = strlen(brace0);
    if (tv_ictx.current_depth - recurse_times < TV_COMPACT && entries == o->n) {
        size_t start = o->n + (size_t)tv_ictx.indent + b0 + 10;
        if (tv_is_below_break_length(o, start)) {
            bool newline = false;
            for (size_t i = 0; i < o->n && !newline; i++) {
                const char *s = tv_piece(o, i, &len);
                newline = tv_has_newline(s, len);
            }
            if (!newline) { /* "{ a, b }" */
                tv_sb_push(sb, brace0, b0);
                tv_sb_push_char(sb, ' ');
                for (size_t i = 0; i < o->n; i++) {
                    if (i) tv_sb_push(sb, ", ", 2);
                    const char *s = tv_piece(o, i, &len);
                    tv_sb_push(sb, s, len);
                }
                tv_sb_push_char(sb, ' ');
                tv_sb_push_cstr(sb, brace1);
                goto done;
            }
        }
    }
    /* one entry per line */
    tv_sb_push(sb, brace0, b0);
    for (size_t i = 0; i < o->n; i++) {
        tv_sb_push(sb, i ? ",\n" : "\n", i ? 2 : 1);
        tv_push_spaces(sb, tv_ictx.indent + 2);
        const char *s = tv_piece(o, i, &len);
        tv_sb_push(sb, s, len);
    }
    tv_sb_push_char(sb, '\n');
    tv_push_spaces(sb, tv_ictx.indent);
    tv_sb_push_cstr(sb, brace1);
done:
    tv_pieces_free(&grouped);
    tv_pieces_free(out);
}

/* Common prologue of formatRaw. Returns false when the container was printed as
 * "[Array]"-style because it is deeper than the depth limit. */
static bool tv_inspect_enter(tv_sb *sb, int depth, const char *too_deep) {
    if (depth == 0) tv_ictx.indent = 0; /* top level: recover from an interrupted inspect */
    if (depth > TV_INSPECT_DEPTH) {
        tv_sb_push_cstr(sb, too_deep);
        return false;
    }
    tv_ictx.current_depth = depth + 1;
    return true;
}

static bool tv_is_number_type(const tv_type *t) {
    return t == &tv_type_int || t == &tv_type_f64 || t == &tv_type_f32 || t == &tv_type_i8 || t == &tv_type_i16 ||
           t == &tv_type_i32 || t == &tv_type_u8 || t == &tv_type_u16 || t == &tv_type_u32 || t == &tv_type_u64;
}

void tv_inspect_arr(tv_sb *sb, tv_arr a, const tv_type *t, int depth) { tv_inspect_typed_arr(sb, a, t, depth, NULL); }

/* A regular expression's match as Node shows it: its groups, then `index`, `input` and `groups`
 * (`[ 'ab', 'b', index: 0, input: 'abc', groups: undefined ]`; one entry a line when long). A
 * global match's (index < 0) is its strings only. */
void tv_inspect_match(tv_sb *sb, tv_arr items, const tv_type *it, tv_int index, tv_str input, const void *groups, const tv_type *gt, int depth) {
    if (index < 0) { tv_inspect_arr(sb, items, it, depth); return; }
    int64_t n = tv_arr_len(items);
    tv_sb *parts = tv_alloc((size_t)(n + 3) * sizeof(tv_sb));
    memset(parts, 0, (size_t)(n + 3) * sizeof(tv_sb));
    for (int64_t i = 0; i < n; i++) tv_inspect_value(&parts[i], it, tv_arrbuf_data(items.p) + (size_t)i * it->size, depth + 1);
    tv_sb_push_cstr(&parts[n], "index: ");
    tv_sb_push_int(&parts[n], index);
    tv_sb_push_cstr(&parts[n + 1], "input: ");
    tv_inspect_value(&parts[n + 1], &tv_type_str, &input, depth + 1);
    tv_sb_push_cstr(&parts[n + 2], "groups: ");
    {
        /* (named groups: an object of no prototype, as Node says) */
        tv_sb g = {0};
        tv_inspect_value(&g, gt, groups, depth + 1);
        if (g.len && g.data[0] == '{') tv_sb_push_cstr(&parts[n + 2], "[Object: null prototype] ");
        tv_sb_push(&parts[n + 2], g.data, g.len);
        tv_sb_free(&g);
    }
    size_t total = 4;
    bool nl = false;
    for (int64_t i = 0; i < n + 3; i++) {
        total += parts[i].len + 2;
        nl |= memchr(parts[i].data, '\n', parts[i].len) != NULL;
    }
    bool one_line = !nl && total <= 76;
    tv_sb_push_cstr(sb, one_line ? "[ " : "[\n");
    for (int64_t i = 0; i < n + 3; i++) {
        if (!one_line) tv_sb_push_cstr(sb, "  ");
        tv_sb_push(sb, parts[i].data, parts[i].len);
        if (i + 1 < n + 3) tv_sb_push_cstr(sb, one_line ? ", " : ",\n");
        tv_sb_free(&parts[i]);
    }
    tv_sb_push_cstr(sb, one_line ? " ]" : "\n]");
    tv_free(parts);
}

void tv_inspect_typed_arr(tv_sb *sb, tv_arr a, const tv_type *t, int depth, const char *name) {
    int64_t n = tv_arr_len(a);
    char open[48] = "[";
    /* (a typed array: `Uint8Array(3) [ 1, 2, 3 ]`, as Node shows it) */
    if (name) snprintf(open, sizeof open, "%s(%lld) [", name, (long long)n);
    if (n == 0) { tv_sb_push_cstr(sb, open); tv_sb_push_char(sb, ']'); return; }
    if (!tv_inspect_enter(sb, depth, name ? "[Object]" : "[Array]")) return;
    tv_pieces out = {0};
    int64_t shown = n < TV_MAX_ARRAY_LENGTH ? n : TV_MAX_ARRAY_LENGTH;
    for (int64_t i = 0; i < shown; i++) { /* formatProperty: indentation + 2 per entry */
        tv_ictx.indent += 2;
        tv_inspect_value(&out.text, t, tv_arrbuf_data(a.p) + (size_t)i * t->size, depth + 1);
        tv_ictx.indent -= 2;
        tv_pieces_mark(&out);
    }
    if (shown < n) tv_push_more_items(&out, n - shown);
    tv_reduce_to_single_string(sb, &out, open, "]", true, depth + 1, tv_is_number_type(t));
}

static void tv_inspect_map_impl(tv_sb *sb, tv_map m, const tv_type *kt, const tv_type *vt, int depth, bool is_set) {
    int64_t n = tv_map_size(m);
    char brace0[40];
    snprintf(brace0, sizeof brace0, "%s(%lld) {", is_set ? "Set" : "Map", (long long)n);
    if (n == 0) {
        tv_sb_push(sb, brace0, strlen(brace0));
        tv_sb_push_char(sb, '}');
        return;
    }
    if (!tv_inspect_enter(sb, depth, is_set ? "[Set]" : "[Map]")) return;
    tv_pieces out = {0};
    void *k, *v;
    int64_t shown = 0;
    tv_ictx.indent += 2; /* formatSet / formatMap */
    for (tv_int i = 0; shown < TV_MAX_ARRAY_LENGTH && tv_map_next(m, kt, vt, &i, &k, &v); shown++) {
        tv_inspect_value(&out.text, kt, k, depth + 1);
        if (!is_set) {
            tv_sb_push(&out.text, " => ", 4);
            tv_inspect_value(&out.text, vt, v, depth + 1);
        }
        tv_pieces_mark(&out);
    }
    tv_ictx.indent -= 2;
    if (shown < n) tv_push_more_items(&out, n - shown);
    tv_reduce_to_single_string(sb, &out, brace0, "}", false, depth + 1, false);
}

void tv_inspect_record(tv_sb *sb, int depth, size_t n, const char *const *names, const tv_type *const *types,
                       const void *const *fields);

void tv_map_inspect(tv_sb *sb, tv_map m, const tv_type *kt, const tv_type *vt, int depth) {
    /* (made as an object: printed as one, `{ a: 1 }`) */
    if (m.p && m.p->object && kt == &tv_type_str) {
        size_t n = (size_t)tv_map_size(m), k = 0;
        const char **names = (const char **)tv_alloc((n ? n : 1) * sizeof *names);
        const tv_type **types = (const tv_type **)tv_alloc((n ? n : 1) * sizeof *types);
        const void **fields = (const void **)tv_alloc((n ? n : 1) * sizeof *fields);
        void *kp, *vp;
        for (tv_int i = 0; tv_map_next(m, kt, vt, &i, &kp, &vp);) {
            names[k] = ((tv_str *)kp)->p->data;
            types[k] = vt;
            fields[k] = vp;
            k++;
        }
        tv_inspect_record(sb, depth, k, names, types, fields);
        tv_free(names);
        tv_free((void *)types);
        tv_free((void *)fields);
        return;
    }
    tv_inspect_map_impl(sb, m, kt, vt, depth, false);
}

void tv_set_inspect(tv_sb *sb, tv_map m, const tv_type *kt, int depth) {
    tv_inspect_map_impl(sb, m, kt, &tv_type_undefined, depth, true);
}

static bool tv_is_plain_key(const char *s) { /* /^[a-zA-Z_][a-zA-Z_0-9]*$/ */
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_')) return false;
    for (s++; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_' || (*s >= '0' && *s <= '9')))
            return false;
    return true;
}

void tv_inspect_record(tv_sb *sb, int depth, size_t n, const char *const *names, const tv_type *const *types,
                       const void *const *fields) {
    if (n == 0) { tv_sb_push(sb, "{}", 2); return; }
    if (!tv_inspect_enter(sb, depth, "[Object]")) return;
    tv_pieces out = {0};
    for (size_t i = 0; i < n; i++) { /* formatProperty */
        if (tv_is_plain_key(names[i])) tv_sb_push_cstr(&out.text, names[i]);
        else tv_push_quoted(&out.text, names[i], strlen(names[i]));
        tv_sb_push(&out.text, ": ", 2);
        tv_ictx.indent += 2;
        tv_inspect_value(&out.text, types[i], fields[i], depth + 1);
        tv_ictx.indent -= 2;
        tv_pieces_mark(&out);
    }
    tv_reduce_to_single_string(sb, &out, "{", "}", false, depth + 1, false);
}

void tv_inspect_tuple(tv_sb *sb, int depth, size_t n, const tv_type *const *types, const void *const *fields) {
    if (!tv_inspect_enter(sb, depth, "[Array]")) return;
    tv_pieces out = {0};
    bool numbers = true;
    for (size_t i = 0; i < n; i++) {
        tv_ictx.indent += 2;
        tv_inspect_value(&out.text, types[i], fields[i], depth + 1);
        tv_ictx.indent -= 2;
        tv_pieces_mark(&out);
        numbers &= tv_is_number_type(types[i]);
    }
    tv_reduce_to_single_string(sb, &out, "[", "]", true, depth + 1, numbers);
}

int tv_inspect_anon;

void tv_inspect_object(tv_sb *sb, int depth, const char *cls, size_t n, const char *const *names, const tv_type *const *types,
                       const void *const *fields) {
    if (tv_inspect_anon) cls = "";
    size_t cl = strlen(cls);
    if (n == 0) {
        tv_sb_push(sb, cls, cl);
        tv_sb_push_cstr(sb, cl ? " {}" : "{}");
        return;
    }
    char buf[160];
    snprintf(buf, sizeof buf, "[%s]", cls);
    if (!tv_inspect_enter(sb, depth, buf)) return;
    tv_pieces out = {0};
    for (size_t i = 0; i < n; i++) {
        if (tv_is_plain_key(names[i])) tv_sb_push_cstr(&out.text, names[i]);
        else tv_push_quoted(&out.text, names[i], strlen(names[i]));
        tv_sb_push(&out.text, ": ", 2);
        tv_ictx.indent += 2;
        tv_inspect_value(&out.text, types[i], fields[i], depth + 1);
        tv_ictx.indent -= 2;
        tv_pieces_mark(&out);
    }
    snprintf(buf, sizeof buf, "%s%s{", cls, cl ? " " : "");
    tv_reduce_to_single_string(sb, &out, buf, "}", false, depth + 1, false);
}

/* ================================================================== primitive type descriptors */

#define TV_DEFINE_INT_TYPE(name, ctype, is_unsigned)                                              \
    static bool tv_##name##_eq(const void *a, const void *b) { return *(const ctype *)a == *(const ctype *)b; } \
    static uint64_t tv_##name##_hash(const void *p) { return tv_mix64((uint64_t)*(const ctype *)p); } \
    static void tv_##name##_to_str(tv_sb *sb, const void *p) {                                    \
        if (is_unsigned) tv_sb_push_u64(sb, (uint64_t)*(const ctype *)p);                          \
        else tv_sb_push_int(sb, (tv_int)*(const ctype *)p);                                        \
    }                                                                                             \
    const tv_type tv_type_##name = {sizeof(ctype), NULL, NULL, tv_##name##_eq, tv_##name##_hash,   \
                                    tv_##name##_to_str, NULL};

TV_DEFINE_INT_TYPE(int, int64_t, 0)
TV_DEFINE_INT_TYPE(i8, int8_t, 0)
TV_DEFINE_INT_TYPE(i16, int16_t, 0)
TV_DEFINE_INT_TYPE(i32, int32_t, 0)
TV_DEFINE_INT_TYPE(u8, uint8_t, 1)
TV_DEFINE_INT_TYPE(u16, uint16_t, 1)
TV_DEFINE_INT_TYPE(u32, uint32_t, 1)
TV_DEFINE_INT_TYPE(u64, uint64_t, 1)
#undef TV_DEFINE_INT_TYPE

static bool tv_f64_eq(const void *a, const void *b) { return *(const double *)a == *(const double *)b; }
static uint64_t tv_f64_hash(const void *p) {
    double x = tv_canon_f64(*(const double *)p);
    uint64_t bits;
    memcpy(&bits, &x, 8);
    return tv_mix64(bits);
}
static void tv_f64_to_str(tv_sb *sb, const void *p) { tv_sb_push_f64(sb, *(const double *)p); }
static void tv_f64_inspect(tv_sb *sb, const void *p, int depth) {
    (void)depth;
    double x = *(const double *)p;
    if (x == 0 && signbit(x)) tv_sb_push(sb, "-0", 2); /* Node shows -0 */
    else tv_sb_push_f64(sb, x);
}
const tv_type tv_type_f64 = {sizeof(double), NULL, NULL, tv_f64_eq, tv_f64_hash, tv_f64_to_str, NULL};

static bool tv_f32_eq(const void *a, const void *b) { return *(const float *)a == *(const float *)b; }
static uint64_t tv_f32_hash(const void *p) { return tv_f64_hash(&(double){(double)*(const float *)p}); }
static void tv_f32_to_str(tv_sb *sb, const void *p) {
    char buf[40];
    /* (read as a number, as a Float32Array's element is in JavaScript: 0.1f is 0.10000000149011612) */
    tv_sb_push(sb, buf, tv_fmt_number(buf, (double)*(const float *)p, false));
}
static void tv_f32_inspect(tv_sb *sb, const void *p, int depth) {
    (void)depth;
    float x = *(const float *)p;
    if (x == 0 && signbit(x)) tv_sb_push(sb, "-0", 2);
    else tv_f32_to_str(sb, p);
}
const tv_type tv_type_f32 = {sizeof(float), NULL, NULL, tv_f32_eq, tv_f32_hash, tv_f32_to_str, NULL};

static bool tv_bool_eq(const void *a, const void *b) { return *(const bool *)a == *(const bool *)b; }
static uint64_t tv_bool_hash(const void *p) { return *(const bool *)p ? 0x9e3779b97f4a7c15ULL : 0x2545f4914f6cdd1dULL; }
static void tv_bool_to_str(tv_sb *sb, const void *p) { tv_sb_push_cstr(sb, *(const bool *)p ? "true" : "false"); }
const tv_type tv_type_bool = {sizeof(bool), NULL, NULL, tv_bool_eq, tv_bool_hash, tv_bool_to_str, NULL};

static void tv_strp_retain(void *p) { tv_str_retain(*(tv_str *)p); }
static void tv_strp_release(void *p) { tv_str_release(*(tv_str *)p); }
static bool tv_strp_eq(const void *a, const void *b) { return tv_str_eq(*(const tv_str *)a, *(const tv_str *)b); }
static uint64_t tv_strp_hash(const void *p) { return tv_str_hash(*(const tv_str *)p); }
static void tv_strp_to_str(tv_sb *sb, const void *p) { tv_sb_push_str(sb, *(const tv_str *)p); }
const tv_type tv_type_str = {sizeof(tv_str), tv_strp_retain, tv_strp_release, tv_strp_eq, tv_strp_hash,
                             tv_strp_to_str, NULL};

static bool tv_undef_eq(const void *a, const void *b) { (void)a; (void)b; return true; }
static uint64_t tv_undef_hash(const void *p) { (void)p; return 0x6a09e667f3bcc909ULL; }
static void tv_undef_to_str(tv_sb *sb, const void *p) { (void)p; tv_sb_push(sb, "undefined", 9); }
const tv_type tv_type_undefined = {0, NULL, NULL, tv_undef_eq, tv_undef_hash, tv_undef_to_str, NULL};
static uint64_t tv_null_hash(const void *p) { (void)p; return 0xbb67ae8584caa73bULL; }
static void tv_null_to_str(tv_sb *sb, const void *p) { (void)p; tv_sb_push(sb, "null", 4); }
/* (one byte, a tv_unit: arrays of nulls hold their elements; undefined's 0 is for sets' values) */
const tv_type tv_type_null = {1, NULL, NULL, tv_undef_eq, tv_null_hash, tv_null_to_str, NULL};

/* ================================================================== unknown */

/* (a box is small and its size known from its type: from the small-object free lists, as an
 * array's, without malloc) */
static inline size_t tv_dynbox_size(const tv_type *t) { return (sizeof(tv_dynbox) + t->size + 15) & ~(size_t)15; }

tv_dyn tv_dyn_box(const tv_type *t, const void *v) {
    if (t->size == 0 || t == &tv_type_null) return (tv_dyn){t, NULL};
    size_t s = tv_dynbox_size(t);
    tv_dynbox *b;
#ifndef TV_ARRBUF_PLAIN
    if (s <= TV_ARRBUF_SMALL) {
        size_t c = s >> 3;
        void **f = (void **)tv_small_bins[c];
        if (__builtin_expect(f != NULL, 1)) { tv_small_bins[c] = *f; b = (tv_dynbox *)(void *)f; }
        else b = (tv_dynbox *)tv_small_refill(c);
    } else
#endif
        b = (tv_dynbox *)tv_alloc(s);
    b->rc = 1;
    memcpy(b->data, v, t->size);
    return (tv_dyn){t, b};
}

void tv_dyn_release(tv_dyn d) {
    if (!d.b || --d.b->rc > 0) return;
    if (d.t->release) d.t->release(d.b->data);
#ifndef TV_ARRBUF_PLAIN
    size_t s = tv_dynbox_size(d.t);
    if (s <= TV_ARRBUF_SMALL) {
        size_t c = s >> 3;
        *(void **)(void *)d.b = tv_small_bins[c];
        tv_small_bins[c] = d.b;
        return;
    }
#endif
    free(d.b);
}

static void tv_dynp_retain(void *p) { tv_dyn_retain(*(tv_dyn *)p); }
static void tv_dynp_release(void *p) { tv_dyn_release(*(tv_dyn *)p); }
bool tv_dyn_is_num(const tv_type *t) {
    return t == &tv_type_f64 || t == &tv_type_int || t == &tv_type_f32 || t == &tv_type_i32 || t == &tv_type_u32 || t == &tv_type_i16 || t == &tv_type_u16 || t == &tv_type_i8 || t == &tv_type_u8 || t == &tv_type_u64;
}
static double tv_num_of(const tv_type *t, const void *p);
double tv_dyn_number(tv_dyn d) { return tv_num_of(d.t, tv_dyn_data(d)); }
static double tv_num_of(const tv_type *t, const void *p) {
    tv_dyn d = {t, NULL};
    if (d.t == &tv_type_f64) return *(const double *)p;
    if (d.t == &tv_type_int) return (double)*(const tv_int *)p;
    if (d.t == &tv_type_f32) return (double)*(const float *)p;
    if (d.t == &tv_type_i32) return (double)*(const int32_t *)p;
    if (d.t == &tv_type_u32) return (double)*(const uint32_t *)p;
    if (d.t == &tv_type_i16) return (double)*(const int16_t *)p;
    if (d.t == &tv_type_u16) return (double)*(const uint16_t *)p;
    if (d.t == &tv_type_i8) return (double)*(const int8_t *)p;
    if (d.t == &tv_type_u8) return (double)*(const uint8_t *)p;
    if (d.t == &tv_type_u64) return (double)*(const uint64_t *)p;
    return NAN;
}

/* (===: numbers by value, whatever width holds them) */
bool tv_dyn_eq(tv_dyn x, tv_dyn y) {
    if (tv_dyn_is_num(x.t) && tv_dyn_is_num(y.t)) return tv_dyn_number(x) == tv_dyn_number(y);
    if (x.t != y.t) return false;
    if (!x.b || !y.b) return x.b == y.b;
    return x.t->eq ? x.t->eq(x.b->data, y.b->data) : memcmp(x.b->data, y.b->data, x.t->size) == 0;
}
static bool tv_dynp_eq(const void *a, const void *b) { return tv_dyn_eq(*(const tv_dyn *)a, *(const tv_dyn *)b); }
static uint64_t tv_dynp_hash(const void *p) {
    const tv_dyn *d = p;
    if (tv_dyn_is_num(d->t)) {
        double x = tv_dyn_number(*d);
        return tv_type_f64.hash(&x);
    }
    return d->t->hash ? d->t->hash(tv_dyn_data(*d)) : (uint64_t)(uintptr_t)d->t;
}
static void tv_dynp_to_str(tv_sb *sb, const void *p) {
    const tv_dyn *d = p;
    d->t->to_str(sb, tv_dyn_data(*d));
}
static void tv_dynp_inspect(tv_sb *sb, const void *p, int depth) {
    const tv_dyn *d = p;
    tv_inspect_value(sb, d->t, tv_dyn_data(*d), depth);
}
const tv_type tv_type_dyn = {sizeof(tv_dyn), tv_dynp_retain, tv_dynp_release, tv_dynp_eq, tv_dynp_hash, tv_dynp_to_str, tv_dynp_inspect};

bool tv_dyn_truthy(tv_dyn d) {
    if (d.t == &tv_type_undefined || d.t == &tv_type_null) return false;
    if (d.t == &tv_type_bool) return *(const bool *)tv_dyn_data(d);
    if (d.t == &tv_type_str) return ((const tv_str *)tv_dyn_data(d))->p->len != 0;
    if (tv_dyn_is_num(d.t)) {
        double x = tv_dyn_number(d);
        return x == x && x != 0;
    }
    return true;
}

/* ================================================================== output */

/* Output is buffered in 8 KiB inside the runtime's globals (a program that prints a little
 * touches one page for all of them), moving to 64 KiB once a program fills that. */
enum { TV_OUT_SMALL = 8192, TV_OUT_BIG = 65536 };
static char tv_out_small[TV_OUT_SMALL];
static char *tv_out_big; /* (a pointer initialized to tv_out_small would put a page of data in every binary) */
static size_t tv_out_len;
#define tv_out_buf (tv_out_big ? tv_out_big : tv_out_small)
#define tv_out_cap ((size_t)(tv_out_big ? TV_OUT_BIG : TV_OUT_SMALL))
static bool tv_out_tty; /* stdout is a terminal: flush after every line */

void tv_out_flush(void) {
    if (tv_out_len) {
        tv_write_fd(1, tv_out_buf, tv_out_len);
        tv_out_len = 0;
    }
}

void tv_out_write(const char *s, size_t n) {
    if (!n) return;
    if (n > tv_out_cap - tv_out_len) {
        tv_out_flush();
        if (!tv_out_big) tv_out_big = tv_alloc(TV_OUT_BIG);
        if (n >= tv_out_cap) {
            tv_write_fd(1, s, n);
            return;
        }
    }
    memcpy(tv_out_buf + tv_out_len, s, n);
    tv_out_len += n;
}

void tv_out_sb_line(tv_sb *sb) {
    tv_sb_push_char(sb, '\n');
    tv_out_write(sb->data, sb->len);
    tv_sb_free(sb);
    if (tv_out_tty) tv_out_flush();
}

void tv_err_sb_line(tv_sb *sb) {
    tv_out_flush(); /* keep stdout/stderr ordering */
    tv_sb_push_char(sb, '\n');
    tv_write_fd(2, sb->data, sb->len);
    tv_sb_free(sb);
}

/* ================================================================== math */

double tv_math_round(double x) {
    if (!isfinite(x)) return x;
    double r = floor(x);
    if (x - r >= 0.5) r += 1.0;
    return r == 0 ? copysign(0.0, x) : r; /* Math.round(-0.4) is -0 */
}

tv_int tv_f64_to_int(double x, const char *what, const char *loc) {
    if (TV_LIKELY(x >= -9223372036854775808.0 && x < 9223372036854775808.0)) return (tv_int)x;
    char num[40], msg[160];
    num[tv_fmt_number(num, x, false)] = 0;
    snprintf(msg, sizeof msg, "%s(%s) is %s", what ? what : "int", num,
             x != x ? "not an integer" : "out of the int range");
    tv_trap(msg, loc);
}

/* Stored xor the default seed, so the state is zero-initialized (no __data page in the binary). */
#define TV_RNG_DEFAULT 0x9e3779b97f4a7c15ULL
uint64_t tv_rng_state;

static void tv_random_seed(uint64_t seed) {
    uint64_t x = tv_mix64(seed + TV_RNG_DEFAULT);
    tv_rng_state = (x ? x : TV_RNG_DEFAULT) ^ TV_RNG_DEFAULT;
}

/* Seeded as a program that draws random numbers starts (TOV_SEED: a number, or any other value
 * for a random seed), so programs that never draw one don't link getenv, time and clock. */
__attribute__((noinline, cold)) void tv_random_init(void) {
    const char *seed = getenv("TOV_SEED");
    if (seed) {
        char *end;
        unsigned long long v = strtoull(seed, &end, 0);
        if (end != seed && *end == 0) tv_random_seed(v);
        else tv_random_seed((uint64_t)time(NULL) ^ ((uint64_t)clock() << 32) ^ (uint64_t)(uintptr_t)&seed);
    } else {
        tv_random_seed(0);
    }
}


/* ================================================================== closures */

void tv_env_release_slow(tv_env *e) {
    if (e->drop) e->drop(e);
    free(e);
}

/* ================================================================== tests */

bool tv_test_active(void) { return tv_test_jmp != NULL; }

static void tv_test_longjmp(void) { longjmp(*tv_test_jmp, 1); }

void tv_test_call(void *fn) { ((void (*)(void))fn)(); }

void tv_test_run(const char *name, void (*fn)(void)) { tv_test_run_ctx(name, strlen(name), tv_test_call, (void *)fn, false); }

void tv_test_run_ctx(const char *name, size_t n, void (*fn)(void *), void *ctx, bool suite) {
    jmp_buf jb;
    jmp_buf *outer = tv_test_jmp;
    tv_test_jmp = &jb;
    tv_test_unwind = tv_test_longjmp;
    tv_sb sb = {0};
    if (setjmp(jb) == 0) {
        fn(ctx);
        tv_test_jmp = outer;
        if (suite) return;
        tv_tests_passed++;
        tv_sb_push_cstr(&sb, "ok   ");
        tv_sb_push(&sb, name, n);
    } else {
        tv_test_jmp = outer;
        tv_tests_failed++;
        tv_sb_push_cstr(&sb, "FAIL ");
        tv_sb_push(&sb, name, n);
        tv_sb_push_cstr(&sb, "\n  ");
        tv_sb_push(&sb, tv_test_msg.data, tv_test_msg.len);
        if (tv_test_loc && *tv_test_loc) {
            tv_sb_push_cstr(&sb, "\n  at ");
            tv_sb_push_cstr(&sb, tv_test_loc);
        }
        tv_sb_free(&tv_test_msg);
        tv_test_loc = NULL;
    }
    tv_out_sb_line(&sb);
}

_Noreturn void tv_expect_fail(tv_sb *message, const char *loc) {
    tv_sb msg = {0};
    if (message) {
        msg = *message;
        *message = (tv_sb){0};
    }
    if (!msg.len) tv_sb_push_cstr(&msg, "expectation failed");
    if (tv_test_jmp) {
        tv_sb_free(&tv_test_msg);
        tv_test_msg = msg;
        tv_test_loc = loc;
        tv_test_unwind();
    }
    tv_sb_push_char(&msg, 0);
    tv_trap(msg.data, loc);
}

int tv_test_summary(void) {
    tv_sb sb = {0};
    tv_sb_push_int(&sb, tv_tests_passed);
    tv_sb_push_cstr(&sb, " passed, ");
    tv_sb_push_int(&sb, tv_tests_failed);
    tv_sb_push_cstr(&sb, " failed");
    tv_out_sb_line(&sb);
    return tv_tests_failed ? 1 : 0;
}

/* ================================================================== init */

void tv_init(int argc, char **argv) {
    static bool initialized;
    tv_argc = argc;
    tv_argv = argv;
    if (initialized) return;
    initialized = true;
    atexit(tv_out_flush);
#ifdef TV_HAVE_ISATTY
    tv_out_tty = isatty(1) != 0;
#endif
}

/* ================================================================== system */

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

static tv_sb tv_native_err;

static const char *tv_errno_name(int e) {
    switch (e) {
    case ENOENT: return "ENOENT";
    case EACCES: return "EACCES";
    case EEXIST: return "EEXIST";
    case ENOTDIR: return "ENOTDIR";
    case EISDIR: return "EISDIR";
    case ENOTEMPTY: return "ENOTEMPTY";
    case EPERM: return "EPERM";
    case EBUSY: return "EBUSY";
    case EMFILE: return "EMFILE";
    case ENOSPC: return "ENOSPC";
    case EROFS: return "EROFS";
    default: return "EIO";
    }
}

static const char *tv_errno_text(int e) {
    switch (e) {
    case ENOENT: return "no such file or directory";
    case EACCES: return "permission denied";
    case EEXIST: return "file already exists";
    case ENOTDIR: return "not a directory";
    case EISDIR: return "illegal operation on a directory";
    case ENOTEMPTY: return "directory not empty";
    case EPERM: return "operation not permitted";
    default: return strerror(e);
    }
}

/* "ENOENT: no such file or directory, open 'path'" */
static void tv_native_fail(int e, const char *op, tv_str path) {
    if (tv_native_err.len) return;
    tv_sb_push_cstr(&tv_native_err, tv_errno_name(e));
    tv_sb_push_cstr(&tv_native_err, ": ");
    tv_sb_push_cstr(&tv_native_err, tv_errno_text(e));
    tv_sb_push_cstr(&tv_native_err, ", ");
    tv_sb_push_cstr(&tv_native_err, op);
    tv_sb_push_cstr(&tv_native_err, " '");
    tv_sb_push(&tv_native_err, path.p->data, (size_t)path.p->len);
    tv_sb_push_char(&tv_native_err, '\'');
}

void tv_native_error(const char *msg) {
    if (tv_native_err.len) return;
    tv_sb_push_cstr(&tv_native_err, msg);
}

__attribute__((noreturn)) unsigned char tv_native_trap(tv_str msg) {
    tv_trap(msg.p ? msg.p->data : "", NULL);
}

tv_str tv_native_takeError(void) {
    if (!tv_native_err.len) return TV_EMPTY_STR;
    return tv_str_from_sb(&tv_native_err);
}

tv_str tv_native_readFile(tv_str path) {
    FILE *f = fopen(path.p->data, "rb");
    if (!f) { tv_native_fail(errno, "open", path); return TV_EMPTY_STR; }
    struct stat st;
    if (fstat(fileno(f), &st) == 0 && S_ISDIR(st.st_mode)) { fclose(f); tv_native_fail(EISDIR, "read", path); return TV_EMPTY_STR; }
    tv_sb sb = {0};
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) tv_sb_push(&sb, buf, n);
    int err = ferror(f) ? errno : 0;
    fclose(f);
    if (err) { tv_sb_free(&sb); tv_native_fail(err, "read", path); return TV_EMPTY_STR; }
    return tv_str_from_sb(&sb);
}

void tv_native_writeFile(tv_str path, tv_str data, bool append) {
    FILE *f = fopen(path.p->data, append ? "ab" : "wb");
    if (!f) { tv_native_fail(errno, "open", path); return; }
    if (data.p->len && fwrite(data.p->data, 1, (size_t)data.p->len, f) != (size_t)data.p->len) tv_native_fail(errno, "write", path);
    if (fclose(f) != 0) tv_native_fail(errno, "close", path);
}

bool tv_native_exists(tv_str path) {
    struct stat st;
    return stat(path.p->data, &st) == 0;
}

static int tv_cmp_cstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

tv_arr tv_native_readDir(tv_str path) {
    DIR *d = opendir(path.p->data);
    if (!d) { tv_native_fail(errno, "scandir", path); return tv_arr_fresh(); }
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; names = tv_realloc(names, cap * sizeof(char *)); }
        size_t len = strlen(e->d_name);
        names[n] = tv_alloc(len + 1);
        memcpy(names[n], e->d_name, len + 1);
        n++;
    }
    closedir(d);
    qsort(names, n, sizeof(char *), tv_cmp_cstr);
    tv_arr out = tv_arr_with_capacity(&tv_type_str, (tv_int)n);
    for (size_t i = 0; i < n; i++) {
        tv_str s = tv_str_from(names[i], strlen(names[i]));
        tv_arr_push(&out, &tv_type_str, &s);
        tv_free(names[i]);
    }
    tv_free(names);
    return out;
}

static int tv_mkdir_p(char *p) {
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(p, 0777) != 0 && errno != EEXIST) { *s = '/'; return -1; }
        *s = '/';
    }
    if (mkdir(p, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

void tv_native_mkdir(tv_str path, bool recursive) {
    if (recursive) {
        char *p = tv_alloc((size_t)path.p->len + 1);
        memcpy(p, path.p->data, (size_t)path.p->len + 1);
        if (tv_mkdir_p(p) != 0) tv_native_fail(errno, "mkdir", path);
        tv_free(p);
        return;
    }
    if (mkdir(path.p->data, 0777) != 0) tv_native_fail(errno, "mkdir", path);
}

void tv_native_unlink(tv_str path) {
    if (unlink(path.p->data) != 0) tv_native_fail(errno, "unlink", path);
}

static int tv_rm_rf(const char *p) {
    struct stat st;
    if (lstat(p, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        if (!d) return -1;
        struct dirent *e;
        size_t pl = strlen(p);
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            size_t nl = strlen(e->d_name);
            char *child = tv_alloc(pl + nl + 2);
            memcpy(child, p, pl);
            child[pl] = '/';
            memcpy(child + pl + 1, e->d_name, nl + 1);
            int r = tv_rm_rf(child);
            tv_free(child);
            if (r != 0) { closedir(d); return -1; }
        }
        closedir(d);
        return rmdir(p);
    }
    return unlink(p);
}

void tv_native_rm(tv_str path, bool recursive, bool force) {
    struct stat st;
    if (lstat(path.p->data, &st) != 0) {
        if (!(force && errno == ENOENT)) tv_native_fail(errno, "rm", path);
        return;
    }
    if (S_ISDIR(st.st_mode) && !recursive) { tv_native_fail(EISDIR, "rm", path); return; }
    if (tv_rm_rf(path.p->data) != 0) tv_native_fail(errno, "rm", path);
}

tv_str tv_native_cwd(void) {
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) return tv_str_from(".", 1);
    return tv_str_from(buf, strlen(buf));
}

tv_arr tv_process_argv(void) {
    tv_arr out = tv_arr_with_capacity(&tv_type_str, tv_argc + 1);
    /* Node: [node binary, script, args...]; a Tov program is both, so argv[0] appears twice. */
    for (int i = -1; i < tv_argc; i++) {
        const char *a = tv_argv[i < 0 ? 0 : i];
        tv_str s = tv_str_from(a, strlen(a));
        tv_arr_push(&out, &tv_type_str, &s);
    }
    return out;
}

bool tv_process_env(tv_str name, tv_str *out) {
    const char *v = getenv(name.p->data);
    if (!v) return false;
    *out = tv_str_from(v, strlen(v));
    return true;
}

tv_int tv_process_pid(void) {
#if defined(_WIN32)
    return (tv_int)_getpid();
#else
    return (tv_int)getpid();
#endif
}

/* statSync's payload: "size;mtimeMs;isdir" — parsed by std/fs. */
tv_str tv_native_statInfo(tv_str path) {
    struct stat st;
    if (stat(path.p->data, &st) != 0) { tv_native_fail(errno, "stat", path); return TV_EMPTY_STR; }
#if defined(__APPLE__)
    double ms = (double)st.st_mtimespec.tv_sec * 1000.0 + (double)st.st_mtimespec.tv_nsec / 1e6;
#elif defined(_WIN32)
    double ms = (double)st.st_mtime * 1000.0;
#else
    double ms = (double)st.st_mtim.tv_sec * 1000.0 + (double)st.st_mtim.tv_nsec / 1e6;
#endif
    char buf[96];
    int n = snprintf(buf, sizeof buf, "%lld;%.0f;%d", (long long)st.st_size, ms, S_ISDIR(st.st_mode) ? 1 : 0);
    return tv_str_from(buf, (size_t)n);
}

void tv_native_access(tv_str path, tv_int mode) {
    if (access(path.p->data, (int)mode) != 0) tv_native_fail(errno, "access", path);
}

_Noreturn void tv_process_exit(tv_int code) {
    tv_out_flush();
    exit((int)code);
}

double tv_date_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)(tv.tv_usec / 1000);
}

/* ---- Date's time zone (the system's: TZ, else /etc/localtime) */
double tv_native_dateNow(void) { return tv_date_now(); }

/* The seconds east of UTC at UTC second `s`. localtime_r reads the time zone again each call
 * (most of a local date's field reads), so the offset is kept for spans it holds for: the same at
 * a second and two weeks either side of it, it holds for those four weeks (no time zone changes
 * its offset twice within two weeks: Morocco's Ramadan pause, the shortest, is a month).
 * Forgotten when TZ changes (see tv_tz_changed). */
static unsigned tv_tz_generation = 1;
void tv_tz_changed(void) { tv_tz_generation++; }
static long tv_gmtoff_at(double s) {
    time_t tt = (time_t)s;
    struct tm tm;
    if (!localtime_r(&tt, &tm)) return 0;
    return tm.tm_gmtoff;
}
static long tv_date_gmtoff(double s) {
    static struct { double lo, hi; long off; unsigned gen; } spans[4];
    static unsigned next, utc_gen;
    static bool utc;
    /* (TZ naming UTC: no offset ever, nothing to ask) */
    if (utc_gen != tv_tz_generation) {
        utc_gen = tv_tz_generation;
        const char *tz = getenv("TZ");
        utc = tz && (!strcmp(tz, "UTC") || !strcmp(tz, "Etc/UTC") || !strcmp(tz, "UTC0") || !strcmp(tz, "GMT") || !strcmp(tz, "Etc/GMT") || !strcmp(tz, "Universal") || !strcmp(tz, "Zulu") || !strcmp(tz, ":UTC"));
    }
    if (utc) return 0;
    for (int i = 0; i < 4; i++)
        if (spans[i].gen == tv_tz_generation && s >= spans[i].lo && s < spans[i].hi) return spans[i].off;
    long off = tv_gmtoff_at(s);
    const double fortnight = 14 * 86400.0;
    if (tv_gmtoff_at(s - fortnight) == off && tv_gmtoff_at(s + fortnight) == off) {
        unsigned i = next++ & 3;
        spans[i].lo = s - fortnight;
        spans[i].hi = s + fortnight;
        spans[i].off = off;
        spans[i].gen = tv_tz_generation;
    }
    return off;
}

static double tv_floor_div(double a, double b) { return floor(a / b); }

/* getTimezoneOffset(): minutes from local time to UTC (west of UTC positive), at time t (ms). */
/* ---- node:crypto's hashes, HMACs and random bytes for Tov code (std/crypto.tov), on the
 * TLS library's BoringSSL (runtime/crypto.c, installed before main when a program uses them) */
typedef struct { void *ctx; bool hmac; } tv_cry_slot;
static tv_cry_slot *tv_cry_slots;
static tv_int tv_cry_n, tv_cry_cap, tv_cry_free = -1;

static tv_int tv_cry_put(void *ctx, bool hmac) {
    tv_int i;
    if (tv_cry_free >= 0) {
        i = tv_cry_free;
        tv_cry_free = (tv_int)(intptr_t)tv_cry_slots[i].ctx;
    } else {
        if (tv_cry_n == tv_cry_cap) {
            tv_cry_cap = tv_cry_cap ? tv_cry_cap * 2 : 16;
            tv_cry_slots = tv_realloc(tv_cry_slots, (size_t)tv_cry_cap * sizeof *tv_cry_slots);
        }
        i = tv_cry_n++;
    }
    tv_cry_slots[i] = (tv_cry_slot){ctx, hmac};
    return i;
}

/* A hash (or, with `hmac`, an HMAC keyed by key's bytes) of the algorithm: a handle, or -1 when
 * BoringSSL doesn't have it. */
tv_int tv_native_cryptoNew(tv_str alg, tv_arr key, bool hmac) {
    if (!tv_crypto) return -1;
    char name[64];
    size_t n = (size_t)tv_str_byte_len(alg) < sizeof name - 1 ? (size_t)tv_str_byte_len(alg) : sizeof name - 1;
    memcpy(name, alg.p->data, n);
    name[n] = 0;
    void *c = hmac ? tv_crypto->hmac_new(name, tv_arr_len(key) ? (const uint8_t *)tv_arr_data(key) : (const uint8_t *)"", (size_t)tv_arr_len(key)) : tv_crypto->hash_new(name);
    return c ? tv_cry_put(c, hmac) : -1;
}

static void tv_cry_update(tv_int h, const uint8_t *p, size_t n) {
    tv_cry_slot *s = &tv_cry_slots[h];
    if (!s->ctx || !n) return;
    (s->hmac ? tv_crypto->hmac_update : tv_crypto->hash_update)(s->ctx, p, n);
}
void tv_native_cryptoUpdate(tv_int h, tv_str data) { tv_cry_update(h, (const uint8_t *)data.p->data, (size_t)tv_str_byte_len(data)); }
void tv_native_cryptoUpdateBytes(tv_int h, tv_arr data) { tv_cry_update(h, (const uint8_t *)tv_arr_data(data), (size_t)tv_arr_len(data)); }

/* The digest, and the handle freed: its length. */
static size_t tv_cry_final(tv_int h, uint8_t out[64]) {
    tv_cry_slot *s = &tv_cry_slots[h];
    if (!s->ctx) return 0;
    size_t len = (s->hmac ? tv_crypto->hmac_final : tv_crypto->hash_final)(s->ctx, out);
    (s->hmac ? tv_crypto->hmac_free : tv_crypto->hash_free)(s->ctx);
    s->ctx = (void *)(intptr_t)tv_cry_free;
    tv_cry_free = h;
    return len;
}

/* Bytes as Node's Buffer.toString(encoding) writes them: hex, base64, base64url, latin1. */
static tv_str tv_bytes_encode(const uint8_t *p, size_t n, tv_str enc) {
    const char *e = enc.p->data;
    size_t el = (size_t)tv_str_byte_len(enc);
    char buf[200];
    char *w = n * 2 + 4 <= sizeof buf ? buf : tv_alloc(n * 2 + 4);
    size_t k = 0;
    if (el == 3 && !memcmp(e, "hex", 3)) {
        static const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < n; i++) {
            w[k++] = digits[p[i] >> 4];
            w[k++] = digits[p[i] & 15];
        }
    } else if ((el == 6 && !memcmp(e, "base64", 6)) || (el == 9 && !memcmp(e, "base64url", 9))) {
        bool url = el == 9;
        const char *tab = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t i = 0;
        for (; i + 2 < n; i += 3) {
            uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
            w[k++] = tab[v >> 18];
            w[k++] = tab[(v >> 12) & 63];
            w[k++] = tab[(v >> 6) & 63];
            w[k++] = tab[v & 63];
        }
        if (i < n) {
            uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0);
            w[k++] = tab[v >> 18];
            w[k++] = tab[(v >> 12) & 63];
            if (i + 1 < n) w[k++] = tab[(v >> 6) & 63];
            else if (!url) w[k++] = '=';
            if (!url) w[k++] = '=';
        }
    } else {
        /* latin1 / binary: each byte a character */
        tv_sb sb = {0};
        for (size_t i = 0; i < n; i++) {
            if (p[i] < 128) tv_sb_push(&sb, (const char *)p + i, 1);
            else {
                char two[2] = {(char)(0xC0 | (p[i] >> 6)), (char)(0x80 | (p[i] & 63))};
                tv_sb_push(&sb, two, 2);
            }
        }
        if (w != buf) tv_free(w);
        return tv_str_from_sb(&sb);
    }
    tv_str out = tv_str_from(w, k);
    if (w != buf) tv_free(w);
    return out;
}

tv_str tv_native_cryptoDigest(tv_int h, tv_str enc) {
    uint8_t out[64];
    size_t len = tv_cry_final(h, out);
    return tv_bytes_encode(out, len, enc);
}

tv_arr tv_native_cryptoDigestBytes(tv_int h) {
    uint8_t out[64];
    size_t len = tv_cry_final(h, out);
    tv_arr a = tv_arr_with_capacity(&tv_type_u8, (tv_int)len);
    memcpy(tv_arr_data(a), out, len);
    a.p->len = (tv_int)len;
    return a;
}

tv_str tv_native_bytesEncode(tv_arr bytes, tv_str enc) { return tv_bytes_encode((const uint8_t *)tv_arr_data(bytes), (size_t)tv_arr_len(bytes), enc); }

tv_arr tv_native_cryptoRandomBytes(tv_int n) {
    if (n < 0) n = 0;
    tv_arr a = tv_arr_with_capacity(&tv_type_u8, n);
    if (tv_crypto) tv_crypto->random((uint8_t *)tv_arr_data(a), (size_t)n);
    else arc4random_buf(tv_arr_data(a), (size_t)n);
    a.p->len = n;
    return a;
}

tv_str tv_native_cryptoRandomUUID(void) {
    uint8_t b[16];
    if (tv_crypto) tv_crypto->random(b, 16);
    else arc4random_buf(b, 16);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    static const char digits[] = "0123456789abcdef";
    char out[36];
    size_t k = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[k++] = '-';
        out[k++] = digits[b[i] >> 4];
        out[k++] = digits[b[i] & 15];
    }
    return tv_str_from(out, 36);
}

bool tv_native_cryptoEqual(tv_arr a, tv_arr b) {
    size_t n = (size_t)tv_arr_len(a);
    if (tv_crypto) return tv_crypto->equal((const uint8_t *)tv_arr_data(a), (const uint8_t *)tv_arr_data(b), n);
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= ((const uint8_t *)tv_arr_data(a))[i] ^ ((const uint8_t *)tv_arr_data(b))[i];
    return d == 0;
}

/* Days since 1970-01-01 to (year, month 1-12, day 1-31) (Hinnant's civil_from_days). */
static void tv_civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

static int64_t tv_days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static char *tv_put_digits(char *w, int64_t v, int width) {
    for (int i = width - 1; i >= 0; i--) {
        w[i] = (char)('0' + v % 10);
        v /= 10;
    }
    return w + width;
}

/* Date's toISOString of a valid time t (ms since the epoch). */
tv_str tv_native_dateIso(double t) {
    int64_t ms = (int64_t)t;
    int64_t day = ms >= 0 ? ms / 86400000 : -((-ms + 86399999) / 86400000);
    int64_t in_day = ms - day * 86400000;
    int64_t y;
    int m, d;
    tv_civil_from_days(day, &y, &m, &d);
    char buf[40], *w = buf;
    if (y >= 0 && y <= 9999) {
        w = tv_put_digits(w, y, 4);
    } else {
        *w++ = y < 0 ? '-' : '+';
        w = tv_put_digits(w, y < 0 ? -y : y, 6);
    }
    *w++ = '-';
    w = tv_put_digits(w, m, 2);
    *w++ = '-';
    w = tv_put_digits(w, d, 2);
    *w++ = 'T';
    w = tv_put_digits(w, in_day / 3600000, 2);
    *w++ = ':';
    w = tv_put_digits(w, in_day / 60000 % 60, 2);
    *w++ = ':';
    w = tv_put_digits(w, in_day / 1000 % 60, 2);
    *w++ = '.';
    w = tv_put_digits(w, in_day % 1000, 3);
    *w++ = 'Z';
    return tv_str_from(buf, (size_t)(w - buf));
}

/* `YYYY-MM-DDTHH:MM:SS.sssZ` exactly (what toISOString makes) as a time, else NaN (for the
 * full parser to read). */
double tv_native_dateParseIso(tv_str s) {
    const char *p = s.p->data;
    if (s.p->len != 24 || p[4] != '-' || p[7] != '-' || p[10] != 'T' || p[13] != ':' || p[16] != ':' || p[19] != '.' || p[23] != 'Z') return NAN;
    static const int at[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18, 20, 21, 22};
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) if (p[at[i]] < '0' || p[at[i]] > '9') return NAN;
#define TV_D2(i) ((p[i] - '0') * 10 + (p[(i) + 1] - '0'))
    int64_t y = TV_D2(0) * 100 + TV_D2(2);
    int mo = TV_D2(5), d = TV_D2(8), h = TV_D2(11), mi = TV_D2(14), sec = TV_D2(17);
    int ms = (p[20] - '0') * 100 + TV_D2(21);
#undef TV_D2
    static const int mdays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mo < 1 || mo > 12 || d < 1 || d > mdays[mo - 1] || h > 24 || mi > 59 || sec > 59) return NAN;
    if (h == 24 && (mi || sec || ms)) return NAN;
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (mo == 2 && d == 29 && !leap) return NAN;
    return (double)(tv_days_from_civil(y, mo, d) * 86400000 + (int64_t)h * 3600000 + mi * 60000 + sec * 1000 + ms);
}

double tv_native_dateOffset(double t) {
    if (t != t) return t;
    return (double)(-tv_date_gmtoff(tv_floor_div(t, 1000.0))) / 60.0;
}

/* The UTC time (ms) of local wall-clock time `local` (ms, as if it were UTC): the offset in
 * effect then (in a gap, the one before it; in an overlap, the earlier time, as JavaScript's). */
double tv_native_dateLocalToUtc(double local) {
    if (local != local) return local;
    double s = tv_floor_div(local, 1000.0);
    long off1 = tv_date_gmtoff(s);
    long off2 = tv_date_gmtoff(s - (double)off1);
    long off = off1;
    if (off1 != off2) {
        /* (near a change: the offset that maps back to this wall-clock time, else the earlier) */
        long a = tv_date_gmtoff(s - (double)off2);
        off = a == off2 ? off2 : (off1 > off2 ? off1 : off2);
    }
    /* (an overlap: two UTC times show this wall-clock time; JavaScript takes the earlier) */
    long before = tv_date_gmtoff(s - (double)off - 3600.0);
    if (before > off && tv_date_gmtoff(s - (double)before) == before) off = before;
    return local - (double)off * 1000.0;
}

/* The zone's name at time t, as Node writes it in Date's toString ("Coordinated Universal Time",
 * "Pacific Daylight Time"): ICU's long names for common zones, else the abbreviation. */
tv_str tv_native_dateZoneName(double t) {
    time_t tt = (time_t)tv_floor_div(t, 1000.0);
    struct tm tm;
    const char *abbr = localtime_r(&tt, &tm) && tm.tm_zone ? tm.tm_zone : "UTC";
    /* (abbreviations several zones share, told apart by their offset) */
    static const struct { const char *abbr; long off; const char *name; } shared[] = {
        {"IST", 19800, "India Standard Time"}, {"IST", 3600, "Irish Standard Time"}, {"IST", 7200, "Israel Standard Time"},
        {"CST", 28800, "China Standard Time"}, {"CST", -18000, "Cuba Standard Time"}, {"BST", 21600, "Bangladesh Standard Time"},
    };
    for (size_t i = 0; i < sizeof shared / sizeof shared[0]; i++)
        if (strcmp(abbr, shared[i].abbr) == 0 && tm.tm_gmtoff == shared[i].off) return tv_str_from(shared[i].name, strlen(shared[i].name));
    static const char *const names[][2] = {
        {"UTC", "Coordinated Universal Time"}, {"GMT", "Greenwich Mean Time"}, {"BST", "British Summer Time"},
        {"WET", "Western European Standard Time"}, {"WEST", "Western European Summer Time"},
        {"IDT", "Israel Daylight Time"}, {"SAST", "South Africa Standard Time"}, {"WIB", "Western Indonesia Time"},
        {"HKT", "Hong Kong Standard Time"}, {"PKT", "Pakistan Standard Time"}, {"MSK", "Moscow Standard Time"},
        {"AWST", "Australian Western Standard Time"}, {"ACST", "Australian Central Standard Time"}, {"ACDT", "Australian Central Daylight Time"},
        {"CET", "Central European Standard Time"}, {"CEST", "Central European Summer Time"},
        {"EET", "Eastern European Standard Time"}, {"EEST", "Eastern European Summer Time"},
        {"EST", "Eastern Standard Time"}, {"EDT", "Eastern Daylight Time"}, {"CST", "Central Standard Time"},
        {"CDT", "Central Daylight Time"}, {"MST", "Mountain Standard Time"}, {"MDT", "Mountain Daylight Time"},
        {"PST", "Pacific Standard Time"}, {"PDT", "Pacific Daylight Time"}, {"AKST", "Alaska Standard Time"},
        {"AKDT", "Alaska Daylight Time"}, {"HST", "Hawaii-Aleutian Standard Time"}, {"JST", "Japan Standard Time"},
        {"KST", "Korean Standard Time"}, {"AEST", "Australian Eastern Standard Time"}, {"AEDT", "Australian Eastern Daylight Time"},
        {"NZST", "New Zealand Standard Time"}, {"NZDT", "New Zealand Daylight Time"},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(abbr, names[i][0]) == 0) return tv_str_from(names[i][1], strlen(names[i][1]));
    /* (an offset name, "+0530": GMT and the offset, as ICU writes zones without names) */
    if (abbr[0] == '+' || abbr[0] == '-') {
        char buf[32];
        long off = tm.tm_gmtoff;
        long a = off < 0 ? -off : off;
        snprintf(buf, sizeof buf, "GMT%c%02ld:%02ld", off < 0 ? '-' : '+', a / 3600, (a / 60) % 60);
        return tv_str_from(buf, strlen(buf));
    }
    return tv_str_from(abbr, strlen(abbr));
}

#ifndef __APPLE__
static struct timespec tv_start_time;
#endif
#if defined(__APPLE__) && defined(__aarch64__)
uint64_t tv_perf_start;
double tv_perf_ms_per_tick;
void tv_perf_init(uint64_t t) {
    uint64_t f;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(f));
    tv_perf_start = t;
    tv_perf_ms_per_tick = 1e3 / (double)(f ? f : 24000000);
}
#elif defined(__APPLE__)
#include <mach/mach_time.h>
/* (mach_absolute_time reads the commpage, ~10 ns; CLOCK_MONOTONIC adds the boot time to it) */
double tv_performance_now(void) {
    static uint64_t start;
    static double ms_per_tick;
    uint64_t t = mach_absolute_time();
    if (TV_UNLIKELY(ms_per_tick == 0)) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        ms_per_tick = (double)tb.numer / (double)tb.denom / 1e6;
        start = t;
    }
    return (double)(t - start) * ms_per_tick;
}
#else
double tv_performance_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (tv_start_time.tv_sec == 0 && tv_start_time.tv_nsec == 0) tv_start_time = ts;
    return (double)(ts.tv_sec - tv_start_time.tv_sec) * 1e3 + (double)(ts.tv_nsec - tv_start_time.tv_nsec) / 1e6;
}
#endif

void tv_write_stdout(tv_str s) { tv_out_write(s.p->data, (size_t)s.p->len); }

void tv_write_stderr(tv_str s) {
    tv_out_flush();
    tv_write_fd(2, s.p->data, (size_t)s.p->len);
}

/* ================================================================== JSON */

/* Bytes that need escaping in a JSON string: control characters, '"' and '\\'. */
static const uint8_t tv_json_esc[256] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,
};

void tv_json_quote(tv_sb *sb, tv_str s) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *d = (const unsigned char *)s.p->data;
    size_t n = (size_t)s.p->len, i = 0;
    tv_sb_reserve(sb, n + 2);
    sb->data[sb->len++] = '"';
    while (i < n) {
        size_t start = i;
        /* (eight bytes at a time while none is below 0x20, a quote or a backslash) */
        while (i + 8 <= n) {
            uint64_t w;
            memcpy(&w, d + i, 8);
            const uint64_t ones = 0x0101010101010101ULL, highs = 0x8080808080808080ULL;
            uint64_t q = w ^ (ones * '"'), b = w ^ (ones * '\\');
            uint64_t hit = ((w - ones * 0x20) & ~w) | ((q - ones) & ~q) | ((b - ones) & ~b);
            if (hit & highs) break;
            i += 8;
        }
        while (i < n && !tv_json_esc[d[i]]) i++;
        if (i > start) tv_sb_push(sb, (const char *)d + start, i - start);
        if (i == n) break;
        unsigned char c = d[i++];
        char buf[6];
        switch (c) {
        case '"': tv_sb_push(sb, "\\\"", 2); break;
        case '\\': tv_sb_push(sb, "\\\\", 2); break;
        case '\b': tv_sb_push(sb, "\\b", 2); break;
        case '\f': tv_sb_push(sb, "\\f", 2); break;
        case '\n': tv_sb_push(sb, "\\n", 2); break;
        case '\r': tv_sb_push(sb, "\\r", 2); break;
        case '\t': tv_sb_push(sb, "\\t", 2); break;
        default:
            buf[0] = '\\'; buf[1] = 'u'; buf[2] = '0'; buf[3] = '0'; buf[4] = hex[c >> 4]; buf[5] = hex[c & 15];
            tv_sb_push(sb, buf, 6);
        }
    }
    tv_sb_push_char(sb, '"');
}

void tv_json_number(tv_sb *sb, double x) {
    if (x != x || x == INFINITY || x == -INFINITY) { tv_sb_push_cstr(sb, "null"); return; }
    if (x == 0) { tv_sb_push_char(sb, '0'); return; } /* -0 prints as 0 */
    tv_sb_push_f64(sb, x);
}

void tv_jp_init(tv_jp *p, tv_str text) {
    p->s = (const unsigned char *)text.p->data;
    p->n = (size_t)text.p->len;
    p->i = 0;
    p->err[0] = 0;
}

static void tv_jp_ws(tv_jp *p) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++;
}

char tv_jp_peek_slow(tv_jp *p) {
    tv_jp_ws(p);
    return p->i < p->n ? (char)p->s[p->i] : 0;
}

bool tv_jp_fail(tv_jp *p, const char *expected) {
    if (p->err[0]) return false;
    if (p->i >= p->n) snprintf(p->err, sizeof p->err, "Unexpected end of JSON input (expected %s)", expected);
    else if (p->s[p->i] < 0x20 || p->s[p->i] >= 0x7f) snprintf(p->err, sizeof p->err, "Expected %s at position %zu of the JSON input", expected, p->i);
    else snprintf(p->err, sizeof p->err, "Expected %s but found '%c' at position %zu of the JSON input", expected, p->s[p->i], p->i);
    return false;
}

bool tv_jp_char_slow(tv_jp *p, char c) {
    if (tv_jp_peek(p) == c) { p->i++; return true; }
    char what[4] = { '\'', c, '\'', 0 };
    return tv_jp_fail(p, what);
}

bool tv_jp_word(tv_jp *p, const char *w) {
    tv_jp_ws(p);
    size_t n = strlen(w);
    if (p->n - p->i >= n && memcmp(p->s + p->i, w, n) == 0) { p->i += n; return true; }
    return tv_jp_fail(p, w);
}

static int tv_jp_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void tv_sb_push_utf8(tv_sb *sb, uint32_t cp) {
    char b[4];
    if (cp < 0x80) { b[0] = (char)cp; tv_sb_push(sb, b, 1); }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); tv_sb_push(sb, b, 2); }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 63)); b[2] = (char)(0x80 | (cp & 63)); tv_sb_push(sb, b, 3); }
    else { b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 63)); b[2] = (char)(0x80 | (cp >> 6 & 63)); b[3] = (char)(0x80 | (cp & 63)); tv_sb_push(sb, b, 4); }
}

static bool tv_jp_u4(tv_jp *p, uint32_t *out) {
    if (p->n - p->i < 4) return tv_jp_fail(p, "4 hex digits");
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        int h = tv_jp_hex(p->s[p->i + k]);
        if (h < 0) return tv_jp_fail(p, "a hex digit");
        v = v * 16 + (uint32_t)h;
    }
    p->i += 4;
    *out = v;
    return true;
}

/* Past a string's characters up to a quote, a backslash or a control character (16 bytes at a
 * time while there's none). */
static inline void tv_jp_plain(tv_jp *p) {
    const unsigned char *s = p->s;
    size_t i = p->i, n = p->n;
    typedef unsigned char tv_v16 __attribute__((vector_size(16)));
    while (n - i >= 16) {
        tv_v16 v;
        memcpy(&v, s + i, 16);
        tv_v16 hit = (tv_v16)((v == (unsigned char)'"') | (v == (unsigned char)'\\') | (v < (unsigned char)0x20));
        uint64_t lo, hi;
        memcpy(&lo, &hit, 8);
        memcpy(&hi, (const char *)&hit + 8, 8);
        if (lo | hi) break;
        i += 16;
    }
    while (i < n && s[i] != '"' && s[i] != '\\' && s[i] >= 0x20) i++;
    p->i = i;
}

bool tv_jp_string(tv_jp *p, tv_str *out) {
    if (tv_jp_peek(p) != '"') return tv_jp_fail(p, "a string");
    p->i++;
    size_t start = p->i;
    /* Fast path: no escapes */
    tv_jp_plain(p);
    if (p->i < p->n && p->s[p->i] == '"') {
        *out = tv_str_from((const char *)p->s + start, p->i - start);
        p->i++;
        return true;
    }
    tv_sb sb = {0};
    tv_sb_push(&sb, (const char *)p->s + start, p->i - start);
    while (p->i < p->n) {
        unsigned char c = p->s[p->i];
        if (c == '"') { p->i++; *out = tv_str_from_sb(&sb); return true; }
        if (c < 0x20) { tv_sb_free(&sb); return tv_jp_fail(p, "a string character"); }
        if (c != '\\') { tv_sb_push_char(&sb, (char)c); p->i++; continue; }
        p->i++;
        if (p->i >= p->n) break;
        char e = (char)p->s[p->i++];
        switch (e) {
        case '"': tv_sb_push_char(&sb, '"'); break;
        case '\\': tv_sb_push_char(&sb, '\\'); break;
        case '/': tv_sb_push_char(&sb, '/'); break;
        case 'b': tv_sb_push_char(&sb, '\b'); break;
        case 'f': tv_sb_push_char(&sb, '\f'); break;
        case 'n': tv_sb_push_char(&sb, '\n'); break;
        case 'r': tv_sb_push_char(&sb, '\r'); break;
        case 't': tv_sb_push_char(&sb, '\t'); break;
        case 'u': {
            uint32_t cp;
            if (!tv_jp_u4(p, &cp)) { tv_sb_free(&sb); return false; }
            if (cp >= 0xD800 && cp < 0xDC00 && p->n - p->i >= 6 && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                p->i += 2;
                uint32_t lo;
                if (!tv_jp_u4(p, &lo)) { tv_sb_free(&sb); return false; }
                if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else { tv_sb_push_utf8(&sb, 0xFFFD); cp = lo; }
            }
            if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
            tv_sb_push_utf8(&sb, cp);
            break;
        }
        default: tv_sb_free(&sb); p->i--; return tv_jp_fail(p, "a valid escape");
        }
    }
    tv_sb_free(&sb);
    return tv_jp_fail(p, "'\"'");
}

bool tv_jp_key(tv_jp *p, tv_jp_k *k) {
    k->owned = false;
    if (tv_jp_peek(p) != '"') return tv_jp_fail(p, "a string");
    size_t start = ++p->i;
    tv_jp_plain(p);
    if (p->i < p->n && p->s[p->i] == '"') {
        k->s = (const char *)p->s + start;
        k->n = p->i++ - start;
        return true;
    }
    /* (escapes: decoded) */
    p->i = start - 1;
    if (!tv_jp_string(p, &k->own)) return false;
    k->owned = true;
    k->s = k->own.p->data;
    k->n = (size_t)k->own.p->len;
    return true;
}

bool tv_jp_number(tv_jp *p, double *out) {
    tv_jp_ws(p);
    const unsigned char *s = p->s;
    size_t start = p->i, i = p->i, n = p->n;
    /* (validated as it's read: its significant digits w, up to 19, and exponent q) */
    bool neg = false;
    uint64_t w = 0;
    int digits = 0, q = 0;
    if (i < n && s[i] == '-') { neg = true; i++; }
    if (i < n && s[i] == '0') i++;
    else if (i < n && s[i] >= '1' && s[i] <= '9') {
        for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) { w = w * 10 + (uint64_t)(s[i] - '0'); digits++; }
    } else return tv_jp_fail(p, "a number");
    if (i < n && s[i] == '.') {
        i++;
        if (!(i < n && s[i] >= '0' && s[i] <= '9')) { p->i = i; return tv_jp_fail(p, "a digit"); }
        for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) {
            if (w || s[i] != '0') { w = w * 10 + (uint64_t)(s[i] - '0'); digits++; }
            q--;
        }
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        bool eneg = false;
        if (i < n && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
        if (!(i < n && s[i] >= '0' && s[i] <= '9')) { p->i = i; return tv_jp_fail(p, "a digit"); }
        int x = 0;
        for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) if (x < 100000) x = x * 10 + (s[i] - '0');
        q += eneg ? -x : x;
    }
    double d;
    if (digits <= 19 && (w == 0 ? (d = 0, true) : tv_el_parse(w, q, &d))) {
        *out = neg ? -d : d;
        p->i = i;
        return true;
    }
    char buf[64];
    size_t len = i - start;
    char *tmp = len < sizeof buf ? buf : tv_alloc(len + 1);
    memcpy(tmp, s + start, len);
    tmp[len] = 0;
    *out = strtod(tmp, NULL);
    if (tmp != buf) tv_free(tmp);
    p->i = i;
    return true;
}

/* A JSON number read exactly into a decimal (no detour through a double). */
bool tv_jp_decimal(tv_jp *p, tv_dec *out) {
    tv_jp_ws(p);
    size_t start = p->i, i = p->i;
    if (i < p->n && p->s[i] == '-') i++;
    while (i < p->n && ((p->s[i] >= '0' && p->s[i] <= '9') || p->s[i] == '.' || p->s[i] == 'e' || p->s[i] == 'E' || p->s[i] == '+' || p->s[i] == '-')) i++;
    if (i == start || tv_dec_parse_n((const char *)p->s + start, i - start, out) != 0) return tv_jp_fail(p, "a decimal number");
    p->i = i;
    return true;
}

bool tv_jp_skip(tv_jp *p) {
    char c = tv_jp_peek(p);
    switch (c) {
    case '"': { tv_str s; if (!tv_jp_string(p, &s)) return false; tv_str_release(s); return true; }
    case '{':
        p->i++;
        if (tv_jp_try_char(p, '}')) return true;
        do {
            tv_str k;
            if (!tv_jp_string(p, &k)) return false;
            tv_str_release(k);
            if (!tv_jp_char(p, ':') || !tv_jp_skip(p)) return false;
        } while (tv_jp_try_char(p, ','));
        return tv_jp_char(p, '}');
    case '[':
        p->i++;
        if (tv_jp_try_char(p, ']')) return true;
        do {
            if (!tv_jp_skip(p)) return false;
        } while (tv_jp_try_char(p, ','));
        return tv_jp_char(p, ']');
    case 't': return tv_jp_word(p, "true");
    case 'f': return tv_jp_word(p, "false");
    case 'n': return tv_jp_word(p, "null");
    default: { double d; return tv_jp_number(p, &d); }
    }
}

/* ---------------------------------------------------------------- JSON as an `unknown` */

/* JSON.parse into an `unknown`: an object as a map of its keys to unknowns (made as an object, so
 * it prints and stringifies as one), an array of unknowns, numbers as f64s, strings, booleans and
 * null as themselves. */
static void tv_dynarrp_retain(void *p) { tv_arr_retain(*(tv_arr *)p); }
static void tv_dynarrp_release(void *p) { tv_arr_release(*(tv_arr *)p, &tv_type_dyn); }
static bool tv_dynrefp_eq(const void *a, const void *b) { return *(void *const *)a == *(void *const *)b; }
static uint64_t tv_dynrefp_hash(const void *p) { return (uint64_t)(uintptr_t)*(void *const *)p * 0x9E3779B97F4A7C15ull; }
static void tv_dynarrp_to_str(tv_sb *sb, const void *p) { tv_to_str_arr(sb, *(const tv_arr *)p, &tv_type_dyn); }
static void tv_dynarrp_inspect(tv_sb *sb, const void *p, int depth) { tv_inspect_arr(sb, *(const tv_arr *)p, &tv_type_dyn, depth); }
const tv_type tv_type_dynarr = {sizeof(tv_arr), tv_dynarrp_retain, tv_dynarrp_release, tv_dynrefp_eq, tv_dynrefp_hash, tv_dynarrp_to_str, tv_dynarrp_inspect};
static void tv_dynobjp_retain(void *p) { tv_map_retain(*(tv_map *)p); }
static void tv_dynobjp_release(void *p) { tv_map_release(*(tv_map *)p, &tv_type_str, &tv_type_dyn); }
static void tv_dynobjp_to_str(tv_sb *sb, const void *p) { (void)p; tv_sb_push_cstr(sb, "[object Object]"); }
static void tv_dynobjp_inspect(tv_sb *sb, const void *p, int depth) { tv_map_inspect(sb, *(const tv_map *)p, &tv_type_str, &tv_type_dyn, depth); }
const tv_type tv_type_dynobj = {sizeof(tv_map), tv_dynobjp_retain, tv_dynobjp_release, tv_dynrefp_eq, tv_dynrefp_hash, tv_dynobjp_to_str, tv_dynobjp_inspect};

bool tv_jp_dyn(tv_jp *p, tv_dyn *out) {
    char c = tv_jp_peek(p);
    switch (c) {
    case '"': {
        tv_str s;
        if (!tv_jp_string(p, &s)) return false;
        *out = tv_dyn_box(&tv_type_str, &s);
        return true;
    }
    case '{': {
        p->i++;
        tv_map m = tv_map_fresh();
        m.p->object = 1;
        if (!tv_jp_try_char(p, '}')) {
            do {
                tv_str k;
                if (!tv_jp_string(p, &k)) { tv_map_release(m, &tv_type_str, &tv_type_dyn); return false; }
                tv_dyn v = {&tv_type_undefined, NULL};
                if (!tv_jp_char(p, ':') || !tv_jp_dyn(p, &v)) {
                    tv_str_release(k);
                    tv_map_release(m, &tv_type_str, &tv_type_dyn);
                    return false;
                }
                tv_map_set(&m, &tv_type_str, &tv_type_dyn, &k, &v);
            } while (tv_jp_try_char(p, ','));
            if (!tv_jp_char(p, '}')) { tv_map_release(m, &tv_type_str, &tv_type_dyn); return false; }
        }
        *out = tv_dyn_box(&tv_type_dynobj, &m);
        return true;
    }
    case '[': {
        p->i++;
        tv_arr a = tv_arr_fresh();
        if (!tv_jp_try_char(p, ']')) {
            do {
                tv_dyn v = {&tv_type_undefined, NULL};
                if (!tv_jp_dyn(p, &v)) { tv_arr_release(a, &tv_type_dyn); return false; }
                tv_arr_push(&a, &tv_type_dyn, &v);
            } while (tv_jp_try_char(p, ','));
            if (!tv_jp_char(p, ']')) { tv_arr_release(a, &tv_type_dyn); return false; }
        }
        *out = tv_dyn_box(&tv_type_dynarr, &a);
        return true;
    }
    case 't': case 'f': {
        bool b = c == 't';
        if (!tv_jp_word(p, b ? "true" : "false")) return false;
        *out = tv_dyn_box(&tv_type_bool, &b);
        return true;
    }
    case 'n':
        if (!tv_jp_word(p, "null")) return false;
        *out = (tv_dyn){&tv_type_null, NULL};
        return true;
    default: {
        double d;
        if (!tv_jp_number(p, &d)) return false;
        *out = tv_dyn_box(&tv_type_f64, &d);
        return true;
    }
    }
}

/* ================================================================== any */

/* *p (of type t) === x */
bool tv_any_eq_raw(const tv_type *t, const void *p, tv_dyn x) {
    if (tv_dyn_is_num(t) && tv_dyn_is_num(x.t)) return tv_num_of(t, p) == tv_dyn_number(x);
    if (t != x.t) return false;
    if (!x.b) return t->size == 0 || t == &tv_type_null;
    return t->eq ? t->eq(p, x.b->data) : memcmp(p, x.b->data, t->size) == 0;
}

/* TypeScript's `any` is an `unknown` (a tv_dyn) that every operation is allowed on; what the
 * runtime knows of (strings, numbers, JSON's arrays and objects) is handled here, and the
 * program's own types by generated code (see codegen/anyval.rs). */

static inline bool tv_any_nullish(tv_dyn d) { return d.t == &tv_type_undefined || d.t == &tv_type_null; }
static inline tv_dyn tv_any_undef(void) { return (tv_dyn){&tv_type_undefined, NULL}; }

/* ToNumber */
double tv_any_num(tv_dyn d) {
    if (tv_dyn_is_num(d.t)) return tv_dyn_number(d);
    if (d.t == &tv_type_str) return tv_js_number(*(const tv_str *)tv_dyn_data(d));
    if (d.t == &tv_type_bool) return *(const bool *)tv_dyn_data(d) ? 1 : 0;
    if (d.t == &tv_type_null) return 0;
    if (d.t == &tv_type_dynarr) {
        tv_arr a = *(const tv_arr *)tv_dyn_data(d);
        if (tv_arr_len(a) == 0) return 0;
        if (tv_arr_len(a) == 1) return tv_any_num(((const tv_dyn *)tv_arr_data(a))[0]);
    }
    return NAN;
}

/* String(d), owned */
tv_str tv_any_str(tv_dyn d) {
    if (d.t == &tv_type_str) {
        tv_str s = *(const tv_str *)tv_dyn_data(d);
        tv_str_retain(s);
        return s;
    }
    tv_sb sb = {0};
    d.t->to_str(&sb, tv_dyn_data(d));
    return tv_str_from_sb(&sb);
}

/* A property key: String(k) */
tv_str tv_any_key(tv_dyn k) { return tv_any_str(k); }

/* (a primitive, to `+`: not an object, which becomes its string) */
static bool tv_any_prim_num(tv_dyn d) {
    return tv_dyn_is_num(d.t) || d.t == &tv_type_bool || tv_any_nullish(d);
}

tv_dyn tv_any_add(tv_dyn a, tv_dyn b) {
    if (tv_any_prim_num(a) && tv_any_prim_num(b)) {
        if (a.t == &tv_type_int && b.t == &tv_type_int) {
            tv_int x = *(const tv_int *)tv_dyn_data(a), y = *(const tv_int *)tv_dyn_data(b), r;
            if (!__builtin_add_overflow(x, y, &r) && (uint64_t)r + (UINT64_C(1) << 53) <= (UINT64_C(1) << 54)) return tv_dyn_box(&tv_type_int, &r);
        }
        double r = tv_any_num(a) + tv_any_num(b);
        return tv_dyn_box(&tv_type_f64, &r);
    }
    tv_str x = tv_any_str(a), y = tv_any_str(b);
    tv_str r = tv_str_concat(x, y);
    tv_str_release(x);
    tv_str_release(y);
    return tv_dyn_box(&tv_type_str, &r);
}

/* a < b (`le`: a <= b), JavaScript's: strings by their units, anything else as numbers */
bool tv_any_lt(tv_dyn a, tv_dyn b, bool le) {
    if (a.t == &tv_type_str && b.t == &tv_type_str) {
        int c = tv_str_cmp(*(const tv_str *)tv_dyn_data(a), *(const tv_str *)tv_dyn_data(b));
        return le ? c <= 0 : c < 0;
    }
    double x = tv_any_num(a), y = tv_any_num(b);
    return le ? x <= y : x < y;
}

/* a == b */
bool tv_any_loose_eq(tv_dyn a, tv_dyn b) {
    if (tv_any_nullish(a) || tv_any_nullish(b)) return tv_any_nullish(a) && tv_any_nullish(b);
    bool an = tv_dyn_is_num(a.t) || a.t == &tv_type_bool, bn = tv_dyn_is_num(b.t) || b.t == &tv_type_bool;
    if ((an && (bn || b.t == &tv_type_str)) || (bn && a.t == &tv_type_str)) return tv_any_num(a) == tv_any_num(b);
    return tv_dyn_eq(a, b);
}

_Noreturn void tv_any_nullish_trap(tv_dyn d, tv_str key, const char *loc) {
    tv_sb sb = {0};
    tv_sb_push_cstr(&sb, "TypeError: Cannot read properties of ");
    tv_sb_push_cstr(&sb, d.t == &tv_type_null ? "null" : "undefined");
    tv_sb_push_cstr(&sb, " (reading '");
    tv_sb_push_str(&sb, key);
    tv_sb_push_cstr(&sb, "')");
    tv_sb_add_char(&sb, 0);
    tv_trap(sb.data, loc);
}

_Noreturn void tv_any_type_trap(const char *want, tv_dyn d, const char *loc) {
    tv_sb sb = {0};
    tv_sb_push_cstr(&sb, "TypeError: an `any` holding ");
    if (tv_any_nullish(d)) tv_sb_push_cstr(&sb, d.t == &tv_type_null ? "null" : "undefined");
    else {
        tv_sb_push_cstr(&sb, "a value");
        if (d.t == &tv_type_str) tv_sb_push_cstr(&sb, " of type string");
        else if (tv_dyn_is_num(d.t)) tv_sb_push_cstr(&sb, " of type number");
        else if (d.t == &tv_type_bool) tv_sb_push_cstr(&sb, " of type boolean");
    }
    tv_sb_push_cstr(&sb, " was used as ");
    tv_sb_push_cstr(&sb, want);
    tv_sb_add_char(&sb, 0);
    tv_trap(sb.data, loc);
}

static tv_dyn tv_any_box_str(tv_str s) { return tv_dyn_box(&tv_type_str, &s); }
static tv_dyn tv_any_box_int(tv_int i) { return tv_dyn_box(&tv_type_int, &i); }
static tv_dyn tv_any_box_bool(bool b) { return tv_dyn_box(&tv_type_bool, &b); }

/* o.k of what the runtime knows of; false: not one of those (the program's types are next) */
bool tv_any_get_rt(tv_dyn d, tv_str k, tv_dyn *out, const char *loc) {
    if (tv_any_nullish(d)) tv_any_nullish_trap(d, k, loc);
    static const char len_s[] = "length";
    bool is_len = k.p->len == 6 && memcmp(k.p->data, len_s, 6) == 0;
    if (d.t == &tv_type_str) {
        if (is_len) { *out = tv_any_box_int(tv_str_length(*(const tv_str *)tv_dyn_data(d))); return true; }
        *out = tv_any_undef();
        return true;
    }
    if (d.t == &tv_type_dynarr) {
        if (is_len) { *out = tv_any_box_int(tv_arr_len(*(const tv_arr *)tv_dyn_data(d))); return true; }
        *out = tv_any_undef();
        return true;
    }
    if (d.t == &tv_type_dynobj) {
        tv_dyn *v = tv_map_get(*(const tv_map *)tv_dyn_data(d), &tv_type_str, &tv_type_dyn, &k);
        if (v) { tv_dyn_retain(*v); *out = *v; } else *out = tv_any_undef();
        return true;
    }
    return false;
}

/* o[i] for an integer i of what the runtime knows of */
bool tv_any_at_rt(tv_dyn d, tv_int i, tv_dyn *out) {
    if (d.t == &tv_type_str) {
        tv_str s = *(const tv_str *)tv_dyn_data(d);
        if (i < 0 || i >= tv_str_length(s)) { *out = tv_any_undef(); return true; }
        tv_str c = tv_str_char_at(s, i);
        *out = tv_any_box_str(c);
        return true;
    }
    if (d.t == &tv_type_dynarr) {
        tv_arr a = *(const tv_arr *)tv_dyn_data(d);
        if (i < 0 || i >= tv_arr_len(a)) { *out = tv_any_undef(); return true; }
        tv_dyn v = ((const tv_dyn *)tv_arr_data(a))[i];
        tv_dyn_retain(v);
        *out = v;
        return true;
    }
    return false;
}

/* o.k = v (v moved) of what the runtime knows of */
bool tv_any_set_rt(tv_dyn d, tv_str k, tv_dyn v, const char *loc) {
    if (tv_any_nullish(d)) {
        tv_dyn_release(v);
        tv_sb sb = {0};
        tv_sb_push_cstr(&sb, "TypeError: Cannot set properties of ");
        tv_sb_push_cstr(&sb, d.t == &tv_type_null ? "null" : "undefined");
        tv_sb_push_cstr(&sb, " (setting '");
        tv_sb_push_str(&sb, k);
        tv_sb_push_cstr(&sb, "')");
        tv_sb_add_char(&sb, 0);
        tv_trap(sb.data, loc);
    }
    if (d.t == &tv_type_dynobj) {
        tv_str_retain(k);
        tv_map_set((tv_map *)tv_dyn_data(d), &tv_type_str, &tv_type_dyn, &k, &v);
        return true;
    }
    if (d.t == &tv_type_str || d.t == &tv_type_dynarr || tv_dyn_is_num(d.t) || d.t == &tv_type_bool) {
        /* (a primitive's property, or an array's own: dropped, as sloppy JavaScript does) */
        tv_dyn_release(v);
        return true;
    }
    return false;
}

/* o[i] = v (v moved) */
bool tv_any_set_at_rt(tv_dyn d, tv_int i, tv_dyn v) {
    if (d.t == &tv_type_dynarr) {
        tv_arr *a = (tv_arr *)tv_dyn_data(d);
        if (i < 0) { tv_dyn_release(v); return true; }
        while (tv_arr_len(*a) < i) {
            tv_dyn u = tv_any_undef();
            tv_arr_push(a, &tv_type_dyn, &u);
        }
        if (i == tv_arr_len(*a)) tv_arr_push(a, &tv_type_dyn, &v);
        else {
            tv_arr_make_unique(a, &tv_type_dyn, tv_arr_len(*a));
            tv_dyn *slot = &((tv_dyn *)tv_arr_data(*a))[i];
            tv_dyn_release(*slot);
            *slot = v;
        }
        return true;
    }
    return false;
}

/* (the argument `i` of a call on an `any`, `undefined` past the end) */
static inline tv_dyn tv_any_arg(int n, const tv_dyn *a, int i) { return i < n ? a[i] : tv_any_undef(); }
static inline bool tv_any_has(int n, const tv_dyn *a, int i) { return i < n && a[i].t != &tv_type_undefined; }
static tv_int tv_any_int_arg(int n, const tv_dyn *a, int i, tv_int dflt) {
    if (!tv_any_has(n, a, i)) return dflt;
    double x = tv_any_num(a[i]);
    if (x != x) return 0;
    if (x >= 9.0e15) return (tv_int)9.0e15;
    if (x <= -9.0e15) return -(tv_int)9.0e15;
    return (tv_int)x;
}
static tv_str tv_any_str_arg(int n, const tv_dyn *a, int i) { return tv_any_str(tv_any_arg(n, a, i)); }

/* o.k(...args) of a string or one of JSON's arrays; false: no such method here */
bool tv_any_invoke_rt(tv_dyn d, tv_str k, int n, const tv_dyn *a, tv_dyn *out, const char *loc) {
    if (tv_any_nullish(d)) tv_any_nullish_trap(d, k, loc);
    char name[24];
    if (k.p->len >= (int32_t)sizeof name) return false;
    memcpy(name, k.p->data, (size_t)k.p->len);
    name[k.p->len] = 0;
    if (!strcmp(name, "toString") && n == 0) { *out = tv_any_box_str(tv_any_str(d)); return true; }
    if (d.t == &tv_type_str) {
        tv_str s = *(const tv_str *)tv_dyn_data(d);
        tv_int len = tv_str_length(s);
#define TV_SARG(i) tv_str x##i = tv_any_str_arg(n, a, i)
#define TV_SREL(i) tv_str_release(x##i)
        if (!strcmp(name, "toUpperCase")) { *out = tv_any_box_str(tv_str_to_upper(s)); return true; }
        if (!strcmp(name, "toLowerCase")) { *out = tv_any_box_str(tv_str_to_lower(s)); return true; }
        if (!strcmp(name, "trim")) { *out = tv_any_box_str(tv_str_trim(s)); return true; }
        if (!strcmp(name, "trimStart")) { *out = tv_any_box_str(tv_str_trim_start(s)); return true; }
        if (!strcmp(name, "trimEnd")) { *out = tv_any_box_str(tv_str_trim_end(s)); return true; }
        if (!strcmp(name, "charAt")) { *out = tv_any_box_str(tv_str_char_at(s, tv_any_int_arg(n, a, 0, 0))); return true; }
        if (!strcmp(name, "charCodeAt")) {
            tv_int c = tv_str_char_code_at(s, tv_any_int_arg(n, a, 0, 0));
            if (c < 0) { double nan = NAN; *out = tv_dyn_box(&tv_type_f64, &nan); } else *out = tv_any_box_int(c);
            return true;
        }
        if (!strcmp(name, "slice")) { *out = tv_any_box_str(tv_str_slice(s, tv_any_int_arg(n, a, 0, 0), tv_any_int_arg(n, a, 1, len), true, loc)); return true; }
        if (!strcmp(name, "substring")) { *out = tv_any_box_str(tv_str_substring(s, tv_any_int_arg(n, a, 0, 0), tv_any_int_arg(n, a, 1, len), true)); return true; }
        if (!strcmp(name, "substr")) { *out = tv_any_box_str(tv_str_substr(s, tv_any_int_arg(n, a, 0, 0), tv_any_int_arg(n, a, 1, 0), tv_any_has(n, a, 1))); return true; }
        if (!strcmp(name, "indexOf")) { TV_SARG(0); *out = tv_any_box_int(tv_str_index_of_from(s, x0, tv_any_int_arg(n, a, 1, 0))); TV_SREL(0); return true; }
        if (!strcmp(name, "lastIndexOf")) { TV_SARG(0); *out = tv_any_box_int(tv_str_last_index_of(s, x0, tv_any_int_arg(n, a, 1, 0), tv_any_has(n, a, 1))); TV_SREL(0); return true; }
        if (!strcmp(name, "includes")) { TV_SARG(0); *out = tv_any_box_bool(tv_str_includes_at(s, x0, tv_any_int_arg(n, a, 1, 0))); TV_SREL(0); return true; }
        if (!strcmp(name, "startsWith")) { TV_SARG(0); *out = tv_any_box_bool(tv_str_starts_with_at(s, x0, tv_any_int_arg(n, a, 1, 0))); TV_SREL(0); return true; }
        if (!strcmp(name, "endsWith")) { TV_SARG(0); *out = tv_any_box_bool(tv_str_ends_with_at(s, x0, tv_any_int_arg(n, a, 1, len))); TV_SREL(0); return true; }
        if (!strcmp(name, "repeat")) { *out = tv_any_box_str(tv_str_repeat(s, tv_any_int_arg(n, a, 0, 0), loc)); return true; }
        if (!strcmp(name, "padStart") || !strcmp(name, "padEnd")) {
            tv_str fill = tv_any_has(n, a, 1) ? tv_any_str(a[1]) : tv_str_from(" ", 1);
            tv_int w = tv_any_int_arg(n, a, 0, 0);
            *out = tv_any_box_str(name[3] == 'S' ? tv_str_pad_start(s, w, fill) : tv_str_pad_end(s, w, fill));
            tv_str_release(fill);
            return true;
        }
        if ((!strcmp(name, "replace") || !strcmp(name, "replaceAll")) && n >= 2 && a[0].t == &tv_type_str && a[1].t == &tv_type_str) {
            tv_str x = *(const tv_str *)tv_dyn_data(a[0]), y = *(const tv_str *)tv_dyn_data(a[1]);
            *out = tv_any_box_str(name[7] == 'A' ? tv_str_replace_all(s, x, y) : tv_str_replace(s, x, y));
            return true;
        }
        if (!strcmp(name, "concat")) {
            tv_str r = s;
            tv_str_retain(r);
            for (int i = 0; i < n; i++) {
                tv_str x = tv_any_str(a[i]), t = tv_str_concat(r, x);
                tv_str_release(x);
                tv_str_release(r);
                r = t;
            }
            *out = tv_any_box_str(r);
            return true;
        }
        if (!strcmp(name, "split") && n >= 1 && a[0].t == &tv_type_str) {
            tv_arr parts = tv_str_split(s, *(const tv_str *)tv_dyn_data(a[0]));
            tv_int m = tv_arr_len(parts);
            tv_arr r = tv_arr_fresh();
            for (tv_int i = 0; i < m; i++) {
                tv_str p = ((const tv_str *)tv_arr_data(parts))[i];
                tv_str_retain(p);
                tv_dyn v = tv_any_box_str(p);
                tv_arr_push(&r, &tv_type_dyn, &v);
            }
            tv_arr_release(parts, &tv_type_str);
            *out = tv_dyn_box(&tv_type_dynarr, &r);
            return true;
        }
        if (!strcmp(name, "at")) {
            tv_int i = tv_any_int_arg(n, a, 0, 0);
            if (i < 0) i += len;
            *out = i < 0 || i >= len ? tv_any_undef() : tv_any_box_str(tv_str_char_at(s, i));
            return true;
        }
        if (!strcmp(name, "localeCompare")) {
            TV_SARG(0);
            int c = tv_str_cmp(s, x0);
            TV_SREL(0);
            *out = tv_any_box_int(c < 0 ? -1 : c > 0);
            return true;
        }
#undef TV_SARG
#undef TV_SREL
        return false;
    }
    if (d.t == &tv_type_dynarr) {
        tv_arr *ar = (tv_arr *)tv_dyn_data(d);
        if (!strcmp(name, "push")) {
            for (int i = 0; i < n; i++) {
                tv_dyn v = a[i];
                tv_dyn_retain(v);
                tv_arr_push(ar, &tv_type_dyn, &v);
            }
            *out = tv_any_box_int(tv_arr_len(*ar));
            return true;
        }
        if (!strcmp(name, "pop")) {
            tv_dyn v;
            *out = tv_arr_pop(ar, &tv_type_dyn, &v) ? v : tv_any_undef();
            return true;
        }
        if (!strcmp(name, "join")) {
            tv_str sep = tv_any_has(n, a, 0) ? tv_any_str(a[0]) : tv_str_from(",", 1);
            *out = tv_any_box_str(tv_arr_join(*ar, &tv_type_dyn, sep));
            tv_str_release(sep);
            return true;
        }
        if (!strcmp(name, "indexOf") || !strcmp(name, "includes")) {
            tv_int r = -1, m = tv_arr_len(*ar);
            tv_dyn x = tv_any_arg(n, a, 0);
            for (tv_int i = 0; i < m; i++)
                if (tv_dyn_eq(((const tv_dyn *)tv_arr_data(*ar))[i], x)) { r = i; break; }
            *out = name[1] == 'n' && name[2] == 'c' ? tv_any_box_bool(r >= 0) : tv_any_box_int(r);
            return true;
        }
        if (!strcmp(name, "slice")) {
            tv_arr r = tv_arr_slice(*ar, &tv_type_dyn, tv_any_int_arg(n, a, 0, 0), tv_any_int_arg(n, a, 1, 0), tv_any_has(n, a, 0), tv_any_has(n, a, 1));
            *out = tv_dyn_box(&tv_type_dynarr, &r);
            return true;
        }
        return false;
    }
    if (d.t == &tv_type_dynobj && !strcmp(name, "hasOwnProperty") && n >= 1) {
        tv_str key = tv_any_str(a[0]);
        *out = tv_any_box_bool(tv_map_has(*(const tv_map *)tv_dyn_data(d), &tv_type_str, &tv_type_dyn, &key));
        tv_str_release(key);
        return true;
    }
    return false;
}

/* The length of what for...of over an `any` iterates; -1: not a string or JSON's array */
tv_int tv_any_len_rt(tv_dyn d) {
    if (d.t == &tv_type_dynarr) return tv_arr_len(*(const tv_arr *)tv_dyn_data(d));
    if (d.t == &tv_type_str) return tv_str_length(*(const tv_str *)tv_dyn_data(d));
    return -1;
}


bool tv_jp_end(tv_jp *p) {
    if (tv_jp_peek(p) == 0) return true;
    return tv_jp_fail(p, "the end of the input");
}

bool tv_jp_find_key(tv_jp *p, const char *key, tv_str *val) {
    size_t saved = p->i;
    char saved_err[sizeof p->err];
    memcpy(saved_err, p->err, sizeof p->err);
    bool found = false;
    if (tv_jp_try_char(p, '{') && !tv_jp_try_char(p, '}')) {
        do {
            tv_str k;
            if (!tv_jp_string(p, &k) || !tv_jp_char(p, ':')) break;
            bool match = strcmp(k.p->data, key) == 0;
            tv_str_release(k);
            if (match && tv_jp_peek(p) == '"') { found = tv_jp_string(p, val); break; }
            if (!tv_jp_skip(p)) break;
        } while (tv_jp_try_char(p, ','));
    }
    p->i = saved;
    memcpy(p->err, saved_err, sizeof p->err);
    return found;
}

tv_str tv_jp_error(tv_jp *p) {
    const char *e = p->err[0] ? p->err : "Invalid JSON";
    return tv_str_from(e, strlen(e));
}

/* ================================================================== async
 * Tasks, promises, the microtask queue and timers (see tov.h). */

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#endif

tv_task *tv_cur_task;
size_t tv_cur_sync = 1;
static void (*tv_err_retain)(void *);
static void (*tv_err_release)(void *);

/* Tasks, promises and pending HTTP requests come and go per request: per-size free lists
 * (32-byte classes up to 2 KiB) refilled from 64 KiB slabs; bigger ones from malloc. */
enum { TV_ASYNC_STEP = 32, TV_ASYNC_MAX = 2048, TV_ASYNC_CLASSES = TV_ASYNC_MAX / TV_ASYNC_STEP + 1 };
static void *tv_async_bins[TV_ASYNC_CLASSES];
static tv_class_space tv_async_space[TV_ASYNC_CLASSES];

static void *tv_async_alloc(size_t size) {
#ifdef TV_PLAIN_ALLOC
    return tv_alloc(size);
#endif
    if (size > TV_ASYNC_MAX) return tv_alloc(size);
    size_t c = (size + TV_ASYNC_STEP - 1) / TV_ASYNC_STEP;
    void **f = tv_async_bins[c];
    if (TV_LIKELY(f != NULL)) {
        tv_async_bins[c] = *f;
        return f;
    }
    return tv_carve(&tv_async_space[c], c * TV_ASYNC_STEP);
}

static void tv_async_free(void *p, size_t size) {
#ifdef TV_PLAIN_ALLOC
    free(p);
    return;
#endif
    if (size > TV_ASYNC_MAX) {
        free(p);
        return;
    }
    size_t c = (size + TV_ASYNC_STEP - 1) / TV_ASYNC_STEP;
    *(void **)p = tv_async_bins[c];
    tv_async_bins[c] = p;
}
static void (*tv_err_report)(void *);

void tv_async_init(void (*err_retain)(void *), void (*err_release)(void *), void (*err_report)(void *)) {
    tv_err_retain = err_retain;
    tv_err_release = err_release;
    tv_err_report = err_report;
}

/* ---------------------------------------------------------------- microtask queue (a ring) */

typedef struct { void (*fn)(void *, void *); void *a, *b; } tv_job;
static tv_job *tv_mq;
static size_t tv_mq_head, tv_mq_cap;
size_t tv_mq_len;

static void tv_mq_push(void (*fn)(void *, void *), void *a, void *b) {
    if (tv_mq_len == tv_mq_cap) {
        size_t cap = tv_mq_cap ? tv_mq_cap * 2 : 64;
        tv_job *q = tv_alloc(cap * sizeof *q);
        for (size_t i = 0; i < tv_mq_len; i++) q[i] = tv_mq[(tv_mq_head + i) & (tv_mq_cap - 1)];
        tv_free(tv_mq);
        tv_mq = q;
        tv_mq_head = 0;
        tv_mq_cap = cap;
    }
    tv_mq[(tv_mq_head + tv_mq_len) & (tv_mq_cap - 1)] = (tv_job){fn, a, b};
    tv_mq_len++;
}

/* Rejections nobody awaited by the time the microtask queue drained (reported, then exit 1). */
static tv_promise **tv_unhandled;
static size_t tv_nunhandled, tv_unhandled_cap;

static void tv_check_unhandled(void) {
    for (size_t i = 0; i < tv_nunhandled; i++) {
        tv_promise *p = tv_unhandled[i];
        if (!p->handled) {
            tv_out_flush();
            if (tv_err_report) tv_err_report(p->err);
            exit(1);
        }
        p->reported = false;
        tv_promise_release(p);
    }
    tv_nunhandled = 0;
}

static void tv_run_microtasks(void) {
    while (tv_mq_len) {
        tv_job j = tv_mq[tv_mq_head];
        tv_mq_head = (tv_mq_head + 1) & (tv_mq_cap - 1);
        tv_mq_len--;
        j.fn(j.a, j.b);
    }
    if (tv_nunhandled) tv_check_unhandled();
}

static void tv_callback_job(void *fn, void *env) {
    ((void (*)(tv_env *))fn)((tv_env *)env);
    tv_env_release((tv_env *)env);
}

void tv_queue_microtask(tv_fn cb) {
    tv_env_retain(cb.env);
    tv_mq_push(tv_callback_job, cb.fn, cb.env);
}

/* ---------------------------------------------------------------- tasks */

static void tv_resume_job(void *a, void *b);

tv_task *tv_task_new(size_t frame_size, tv_task_run run, const tv_type *vt) {
    tv_task *t = tv_async_alloc(sizeof(tv_task) + frame_size);
    memset(t, 0, sizeof(tv_task) + frame_size);
    t->run = run;
    t->size = (uint32_t)frame_size;
    t->promise = tv_promise_new(vt);
    return t;
}

/* Runs the task as the current one; frees it once it has finished. */
static void tv_task_step(tv_task *t) {
    tv_task *prev = tv_cur_task;
    size_t prev_sync = tv_cur_sync;
    tv_cur_task = t;
    tv_cur_sync = t->flags & TV_TASK_SYNC;
    bool done = t->run(t);
    tv_cur_task = prev;
    tv_cur_sync = prev_sync;
    if (done) {
        tv_promise_release(t->promise);
        tv_async_free(t, sizeof(tv_task) + t->size);
    }
}

static void tv_resume_job(void *a, void *b) {
    (void)b;
    tv_task *t = a;
    t->flags &= ~TV_TASK_SYNC;   /* run from the queue: nothing of its starter is left to run */
    tv_task_step(t);
}

/* Set by the standard library right before starting a task in tail position: nothing observable
 * runs between the start and the next microtask checkpoint, so it may continue eagerly. */
static bool tv_spawn_tail;

void tv_native_spawnTail(void) { tv_spawn_tail = true; }

tv_promise *tv_task_spawn(tv_task *t) {
    tv_promise *p = t->promise;
    tv_promise_retain(p);
    if (tv_spawn_tail) tv_spawn_tail = false;
    else t->flags |= TV_TASK_SYNC;
    tv_task_step(t);
    return p;
}

void tv_task_start(tv_task *t) { tv_task_step(t); }

void tv_task_yield(void) {
    if (!tv_cur_task) tv_trap("internal error: await outside a task", NULL);
    tv_mq_push(tv_resume_job, tv_cur_task, NULL);
}

/* ---------------------------------------------------------------- promises */

tv_promise *tv_promise_new(const tv_type *vt) {
    size_t size = vt ? vt->size : 0;
    tv_promise *p = tv_async_alloc(sizeof(tv_promise) + size);
    memset(p, 0, sizeof(tv_promise));
    p->rc = 1;
    p->vt = vt;
    return p;
}

void tv_promise_release_slow(tv_promise *p) {
    if (p->state == TV_FULFILLED && p->vt && p->vt->release) p->vt->release(p->value);
    if (p->state == TV_REJECTED && p->err && tv_err_release) tv_err_release(p->err);
    tv_free(p->more);
    tv_async_free(p, sizeof(tv_promise) + (p->vt ? p->vt->size : 0));
}

static void tv_promise_wake(tv_promise *p) {
    if (p->waiter) {
        tv_mq_push(tv_resume_job, p->waiter, NULL);
        p->waiter = NULL;
    }
    for (int32_t i = 0; i < p->nmore; i++) tv_mq_push(p->more[i].fn, p->more[i].a, p->more[i].b);
    p->nmore = 0;
}

/* Runs fn(a, b) as a microtask once p settles (at once if it has), after earlier reactions. */
static void tv_promise_react(tv_promise *p, void (*fn)(void *, void *), void *a, void *b) {
    p->handled = true;
    if (p->state != TV_PENDING) {
        tv_mq_push(fn, a, b);
        return;
    }
    if (p->nmore == p->capmore) {
        p->capmore = p->capmore ? p->capmore * 2 : 4;
        p->more = tv_realloc(p->more, (size_t)p->capmore * sizeof *p->more);
    }
    p->more[p->nmore++] = (tv_reaction){fn, a, b};
}

void tv_promise_on(tv_promise *p, void (*fn)(void *, void *), void *a, void *b) { tv_promise_react(p, fn, a, b); }

void tv_promise_resolve(tv_promise *p, const void *value) {
    if (p->state != TV_PENDING) return;
    size_t size = p->vt ? p->vt->size : 0;
    if (size) {
        memcpy(p->value, value, size);
        if (p->vt->retain) p->vt->retain(p->value);
    }
    p->state = TV_FULFILLED;
    tv_promise_wake(p);
}

void tv_promise_resolve_move(tv_promise *p, void *value) {
    if (p->state != TV_PENDING) {
        if (p->vt && p->vt->release) p->vt->release(value);
        return;
    }
    size_t size = p->vt ? p->vt->size : 0;
    if (size) memcpy(p->value, value, size);
    p->state = TV_FULFILLED;
    tv_promise_wake(p);
}

void tv_promise_reject(tv_promise *p, void *err) {
    if (p->state != TV_PENDING) {
        if (tv_err_release) tv_err_release(err);
        return;
    }
    p->err = err;
    p->state = TV_REJECTED;
    if (!p->handled && !p->waiter && !p->reported) {
        if (tv_nunhandled == tv_unhandled_cap) {
            /* (the ones awaited since go first: a loop that rejects and catches without ever
             * yielding would otherwise keep every one, error and all, until it ends) */
            size_t k = 0;
            for (size_t i = 0; i < tv_nunhandled; i++) {
                tv_promise *q = tv_unhandled[i];
                if (q->handled) {
                    q->reported = false;
                    tv_promise_release(q);
                } else {
                    tv_unhandled[k++] = q;
                }
            }
            tv_nunhandled = k;
        }
        if (tv_nunhandled == tv_unhandled_cap) {
            tv_unhandled_cap = tv_unhandled_cap ? tv_unhandled_cap * 2 : 8;
            tv_unhandled = tv_realloc(tv_unhandled, tv_unhandled_cap * sizeof *tv_unhandled);
        }
        p->reported = true;
        tv_promise_retain(p);
        tv_unhandled[tv_nunhandled++] = p;
    }
    tv_promise_wake(p);
}

void tv_await_suspend(tv_promise *p) {
    tv_task *t = tv_cur_task;
    if (!t) tv_trap("internal error: await outside a task", NULL);
    p->handled = true;
    if (p->state != TV_PENDING) {
        tv_mq_push(tv_resume_job, t, NULL);
        return;
    }
    if (!p->waiter && !p->nmore) {
        p->waiter = t;
        return;
    }
    tv_promise_react(p, tv_resume_job, t, NULL);
}

/* One descriptor for every promise type: the value's type travels with the promise. */
static void tv_promise_ty_retain(void *p) { tv_promise_retain(*(tv_promise **)p); }
static void tv_promise_ty_release(void *p) { tv_promise_release(*(tv_promise **)p); }
static bool tv_promise_ty_eq(const void *a, const void *b) { return *(tv_promise *const *)a == *(tv_promise *const *)b; }
static uint64_t tv_promise_ty_hash(const void *p) { return tv_mix64((uint64_t)(uintptr_t)*(tv_promise *const *)p); }
static void tv_promise_ty_str(tv_sb *sb, const void *p) { (void)p; tv_sb_push_cstr(sb, "[object Promise]"); }
static void tv_promise_ty_inspect(tv_sb *sb, const void *pp, int depth) {
    tv_promise *p = *(tv_promise *const *)pp;
    tv_sb_push_cstr(sb, "Promise { ");
    if (p->state == TV_PENDING) tv_sb_push_cstr(sb, "<pending>");
    else if (p->state == TV_REJECTED) tv_sb_push_cstr(sb, "<rejected>");
    else if (p->vt) tv_inspect_value(sb, p->vt, p->value, depth + 1);
    else tv_sb_push_cstr(sb, "undefined");
    tv_sb_push_cstr(sb, " }");
}
const tv_type tv_type_promise = {sizeof(tv_promise *), tv_promise_ty_retain, tv_promise_ty_release, tv_promise_ty_eq, tv_promise_ty_hash, tv_promise_ty_str, NULL};

void tv_inspect_value(tv_sb *sb, const tv_type *t, const void *p, int depth) {
    if (t->inspect) t->inspect(sb, p, depth);
    else if (t == &tv_type_f64) tv_f64_inspect(sb, p, depth);
    else if (t == &tv_type_f32) tv_f32_inspect(sb, p, depth);
    else if (t == &tv_type_str) tv_inspect_str(sb, *(const tv_str *)p, depth);
    else if (t == &tv_type_promise) tv_promise_ty_inspect(sb, p, depth);
    else t->to_str(sb, p); /* integers, booleans, undefined: as String(x) */
}

/* ---------------------------------------------------------------- Promise.all, Promise.race */

typedef struct {
    tv_promise *result;
    tv_promise **elems;       /* retained until every reaction has run */
    int64_t n, remaining, pending_jobs;
    const tv_type *et, *arr_t;
    unsigned char *values;    /* all: the values so far (n * et->size), with `filled` flags */
    bool *filled;
} tv_combine;

static void tv_combine_done(tv_combine *s) {
    if (--s->pending_jobs) return;
    size_t size = s->et ? s->et->size : 0;
    for (int64_t i = 0; i < s->n; i++) {
        if (s->filled && s->filled[i] && s->et->release) s->et->release(s->values + (size_t)i * size);
        tv_promise_release(s->elems[i]);
    }
    tv_promise_release(s->result);
    tv_free(s->values);
    tv_free(s->filled);
    tv_free(s->elems);
    tv_free(s);
}

static tv_combine *tv_combine_new(tv_arr ps, const tv_type *et, const tv_type *rt, const tv_type *arr_t) {
    tv_combine *s = tv_alloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->n = s->remaining = s->pending_jobs = tv_arr_len(ps);
    s->et = et;
    s->arr_t = arr_t;
    s->result = tv_promise_new(rt);
    tv_promise_retain(s->result);   /* the state's reference */
    s->elems = tv_alloc((size_t)(s->n ? s->n : 1) * sizeof *s->elems);
    for (int64_t i = 0; i < s->n; i++) {
        s->elems[i] = ((tv_promise **)tv_arr_data(ps))[i];
        tv_promise_retain(s->elems[i]);
    }
    return s;
}

static void tv_all_step(void *a, void *b) {
    tv_combine *s = a;
    int64_t i = (int64_t)(intptr_t)b;
    tv_promise *p = s->elems[i];
    if (s->result->state == TV_PENDING) {
        if (p->state == TV_REJECTED) {
            if (tv_err_retain) tv_err_retain(p->err);
            tv_promise_reject(s->result, p->err);
        } else {
            size_t size = s->et->size;
            if (size) {
                memcpy(s->values + (size_t)i * size, p->value, size);
                if (s->et->retain) s->et->retain(s->values + (size_t)i * size);
            }
            s->filled[i] = true;
            if (--s->remaining == 0) {
                /* every value is in: move them into the result array */
                tv_arr arr = tv_arr_with_capacity(s->et, s->n);
                void *dst = tv_arr_reserve_tail(&arr, s->et, s->n);
                if (size) memcpy(dst, s->values, (size_t)s->n * size);
                arr.p->len = s->n;
                memset(s->filled, 0, (size_t)s->n * sizeof *s->filled);
                tv_promise_resolve_move(s->result, &arr);
            }
        }
    }
    tv_combine_done(s);
}

tv_promise *tv_promise_all(tv_arr ps, const tv_type *et, const tv_type *arr_t) {
    tv_combine *s = tv_combine_new(ps, et, arr_t, arr_t);
    tv_promise *result = s->result;
    if (s->n == 0) {
        /* nothing to wait for: fulfilled at once, with [] */
        tv_arr empty = tv_arr_fresh();
        tv_promise_resolve_move(result, &empty);
        s->pending_jobs = 1;
        tv_combine_done(s);
        return result;
    }
    s->values = tv_alloc((size_t)s->n * (et->size ? et->size : 1));
    s->filled = tv_alloc((size_t)s->n * sizeof *s->filled);
    memset(s->filled, 0, (size_t)s->n * sizeof *s->filled);
    for (int64_t i = 0; i < s->n; i++) tv_promise_react(s->elems[i], tv_all_step, s, (void *)(intptr_t)i);
    return result;
}

static void tv_race_step(void *a, void *b) {
    tv_combine *s = a;
    tv_promise *p = s->elems[(int64_t)(intptr_t)b];
    if (s->result->state == TV_PENDING) {
        if (p->state == TV_REJECTED) {
            if (tv_err_retain) tv_err_retain(p->err);
            tv_promise_reject(s->result, p->err);
        } else {
            tv_promise_resolve(s->result, p->value);
        }
    }
    tv_combine_done(s);
}

tv_promise *tv_promise_race(tv_arr ps, const tv_type *et) {
    tv_combine *s = tv_combine_new(ps, et, et, NULL);
    tv_promise *result = s->result;
    if (s->n == 0) {   /* never settles, as in JavaScript */
        s->pending_jobs = 1;
        tv_combine_done(s);
        return result;
    }
    for (int64_t i = 0; i < s->n; i++) tv_promise_react(s->elems[i], tv_race_step, s, (void *)(intptr_t)i);
    return result;
}

typedef struct { tv_env h; tv_promise *p; } tv_resolver_env;

static void tv_resolver_drop(tv_env *e) { tv_promise_release(((tv_resolver_env *)e)->p); }

tv_env *tv_promise_resolver(tv_promise *p) {
    tv_resolver_env *e = tv_alloc(sizeof *e);
    e->h.rc = 1;
    e->h.drop = tv_resolver_drop;
    e->p = p;
    tv_promise_retain(p);
    return &e->h;
}

/* ---------------------------------------------------------------- timers (a min-heap by due time, then creation) */

/* As libuv (so as Node): the loop's clock is in whole milliseconds, read when the loop wakes up;
 * a timer is due at that time plus its delay, and due timers fire in the order they were set. */
typedef struct {
    uint64_t when;    /* due, in ms of the loop's clock */
    uint64_t seq;
    uint64_t every;   /* setInterval: period in ms (0: once) */
    tv_int id;
    tv_fn cb;         /* fn NULL: cleared */
    bool weak;        /* unref'd (AbortSignal.timeout): doesn't keep the program running */
} tv_timer;

static tv_timer *tv_timers;
static size_t tv_ntimers, tv_timers_cap, tv_live_timers;
static uint64_t tv_timer_seq;
static tv_int tv_timer_ids;

static uint64_t tv_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t tv_loop_ms;   /* the loop's clock (0: not read yet) */
static uint64_t tv_loop_worked_at;   /* when the loop last did work (0: it hasn't) */
static int tv_loop_idle_stage;       /* idle since then: 1 collected, 2 deep (see tv_loop_idle) */

/* The deep idle (compiled code dropped) comes this long after the last work. It adapts, as a
 * JIT's code flushing does: a program idle for good gives its memory back soon, and one whose
 * bursts come back within seconds of each drop (and recompile on each, ~25% more CPU for their
 * first second) waits longer, doubling to 20 s; long quiet halves it back. */
#define TV_LOOP_DEEP_IDLE_MIN_MS 2500
#define TV_LOOP_DEEP_IDLE_MAX_MS 20000
#define TV_LOOP_DEEP_SOON_MS 10000    /* work this soon after a drop: the drop cost more than it saved */
#define TV_LOOP_DEEP_QUIET_MS 60000   /* idle this long after a drop: the program is quiet again */
static uint64_t tv_loop_deep_ms = TV_LOOP_DEEP_IDLE_MIN_MS;
static uint64_t tv_loop_dropped_at;  /* when a deep idle after work last dropped code (0: none since) */

/* the loop did work (I/O, timers) */
static void tv_loop_work(uint64_t now_ms) {
    if (tv_loop_dropped_at) {
        uint64_t since = now_ms - tv_loop_dropped_at;
        if (since < TV_LOOP_DEEP_SOON_MS) {
            tv_loop_deep_ms = tv_loop_deep_ms * 2 < TV_LOOP_DEEP_IDLE_MAX_MS ? tv_loop_deep_ms * 2 : TV_LOOP_DEEP_IDLE_MAX_MS;
        } else if (since > TV_LOOP_DEEP_QUIET_MS) {
            tv_loop_deep_ms = tv_loop_deep_ms / 2 > TV_LOOP_DEEP_IDLE_MIN_MS ? tv_loop_deep_ms / 2 : TV_LOOP_DEEP_IDLE_MIN_MS;
        }
        tv_loop_dropped_at = 0;
    }
    tv_loop_worked_at = now_ms ? now_ms : 1;
    tv_loop_idle_stage = 0;
}

static uint64_t tv_loop_update(void) { return tv_loop_ms = tv_mono_ns() / 1000000u; }
static uint64_t tv_loop_now(void) { return tv_loop_ms ? tv_loop_ms : tv_loop_update(); }

/* Sleeps until the loop's clock reads `ms`, precisely: a plain nanosleep wakes ~2 ms late on
 * macOS; a kqueue timer marked critical, ~0.1 ms. */
static void tv_sleep_until(uint64_t ms) {
    uint64_t deadline = ms * 1000000u;
    uint64_t now = tv_mono_ns();
    if (deadline <= now) return;
    if (tv_loop_will_block) tv_loop_will_block();
#if defined(__APPLE__) || defined(__FreeBSD__)
    static int kq = -1;
    if (kq < 0) kq = kqueue();
    if (kq >= 0) {
        struct kevent ch, ev;
        EV_SET(&ch, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, (int64_t)(deadline - now), 0);
        while (kevent(kq, &ch, 1, &ev, 1, NULL) < 0 && errno == EINTR) {}
        return;
    }
    struct timespec ts = {(time_t)((deadline - now) / 1000000000u), (long)((deadline - now) % 1000000000u)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
#else
    struct timespec ts = {(time_t)(deadline / 1000000000u), (long)(deadline % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
#endif
}

static bool tv_timer_before(const tv_timer *a, const tv_timer *b) { return a->when < b->when || (a->when == b->when && a->seq < b->seq); }

/* Where each timer is in the heap, by id (open addressing; ids are never 0): clearing or
 * unref'ing a timer finds it at once, and a cleared one leaves the heap. (Node's timer lists,
 * runtime/node.c, re-arm their one native timer for each setTimeout that changes the next due
 * time: left in the heap until due, a server's cleared timers piled up in their tens of
 * thousands, and each clear searched them all.) */
typedef struct { tv_int id; size_t at; } tv_timer_slot;
static tv_timer_slot *tv_timer_pos;
static size_t tv_timer_pos_cap;   /* a power of 2, at least twice the timers */

static inline size_t tv_timer_hash(tv_int id) { return (size_t)(((uint64_t)id * 0x9E3779B97F4A7C15ull) >> 32) & (tv_timer_pos_cap - 1); }

static tv_timer_slot *tv_timer_find(tv_int id) {
    if (!tv_timer_pos_cap) return NULL;
    for (size_t i = tv_timer_hash(id);; i = (i + 1) & (tv_timer_pos_cap - 1)) {
        if (tv_timer_pos[i].id == id) return &tv_timer_pos[i];
        if (!tv_timer_pos[i].id) return NULL;
    }
}

static void tv_timer_note(tv_int id, size_t at) {
    size_t i = tv_timer_hash(id);
    while (tv_timer_pos[i].id && tv_timer_pos[i].id != id) i = (i + 1) & (tv_timer_pos_cap - 1);
    tv_timer_pos[i] = (tv_timer_slot){ id, at };
}

static void tv_timer_unnote(tv_int id) {
    tv_timer_slot *s = tv_timer_find(id);
    if (!s) return;
    /* (backward shift: the entries after it that probed past it move up) */
    size_t mask = tv_timer_pos_cap - 1, i = (size_t)(s - tv_timer_pos);
    for (size_t j = (i + 1) & mask; tv_timer_pos[j].id; j = (j + 1) & mask) {
        size_t home = tv_timer_hash(tv_timer_pos[j].id);
        if (((j - home) & mask) >= ((j - i) & mask)) {
            tv_timer_pos[i] = tv_timer_pos[j];
            i = j;
        }
    }
    tv_timer_pos[i].id = 0;
}

static inline void tv_timer_put(size_t i, tv_timer t) {
    tv_timers[i] = t;
    tv_timer_note(t.id, i);
}

static void tv_timer_sift_up(size_t i, tv_timer t) {
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!tv_timer_before(&t, &tv_timers[parent])) break;
        tv_timer_put(i, tv_timers[parent]);
        i = parent;
    }
    tv_timer_put(i, t);
}

static void tv_timer_sift_down(size_t i, tv_timer t) {
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        const tv_timer *best = &t;
        if (l < tv_ntimers && tv_timer_before(&tv_timers[l], best)) { m = l; best = &tv_timers[l]; }
        if (r < tv_ntimers && tv_timer_before(&tv_timers[r], best)) m = r;
        if (m == i) break;
        tv_timer_put(i, tv_timers[m]);
        i = m;
    }
    tv_timer_put(i, t);
}

static void tv_timer_push(tv_timer t) {
    if (tv_ntimers == tv_timers_cap) {
        tv_timers_cap = tv_timers_cap ? tv_timers_cap * 2 : 16;
        tv_timers = tv_realloc(tv_timers, tv_timers_cap * sizeof *tv_timers);
    }
    if ((tv_ntimers + 1) * 2 > tv_timer_pos_cap) {
        tv_timer_slot *old = tv_timer_pos;
        size_t old_cap = tv_timer_pos_cap;
        tv_timer_pos_cap = old_cap ? old_cap * 2 : 32;
        tv_timer_pos = tv_alloc(tv_timer_pos_cap * sizeof *tv_timer_pos);
        memset(tv_timer_pos, 0, tv_timer_pos_cap * sizeof *tv_timer_pos);
        for (size_t i = 0; i < old_cap; i++)
            if (old[i].id) tv_timer_note(old[i].id, old[i].at);
        tv_free(old);
    }
    tv_timer_sift_up(tv_ntimers++, t);
}

/* Takes the timer at heap index k out. */
static tv_timer tv_timer_take(size_t k) {
    tv_timer t = tv_timers[k];
    tv_timer_unnote(t.id);
    tv_timer last = tv_timers[--tv_ntimers];
    if (k < tv_ntimers) {
        if (k > 0 && tv_timer_before(&last, &tv_timers[(k - 1) / 2])) tv_timer_sift_up(k, last);
        else tv_timer_sift_down(k, last);
    }
    return t;
}

static tv_timer tv_timer_pop(void) { return tv_timer_take(0); }

tv_int tv_set_timer(tv_fn cb, double ms, bool repeat) {
    /* As Node: delays below 1 ms (or not a number, or too large) are 1 ms; fractions are dropped. */
    if (!(ms >= 1 && ms <= 2147483647.0)) ms = 1;
    uint64_t delay = (uint64_t)ms;
    tv_env_retain(cb.env);
    tv_timer t = {tv_loop_now() + delay, ++tv_timer_seq, repeat ? delay : 0, ++tv_timer_ids, cb, false};
    tv_timer_push(t);
    tv_live_timers++;
    return t.id;
}

void tv_clear_timer(tv_int id) {
    tv_timer_slot *at = tv_timer_find(id);
    if (!at) return;
    tv_timer t = tv_timer_take(at->at);
    tv_env_release(t.cb.env);
    if (!t.weak) tv_live_timers--;
}

/* Node's timer.unref(): the timer still fires while other work keeps the program running,
 * but doesn't keep it running by itself. */
void tv_native_timerUnref(tv_int id) {
    tv_timer_slot *at = tv_timer_find(id);
    if (!at) return;
    tv_timer *t = &tv_timers[at->at];
    if (t->weak) return;
    t->weak = true;
    tv_live_timers--;
}

/* A timer that ran for less than this (with its microtasks) isn't the program at work either: it
 * doesn't put off giving memory back. Programs leave such timers behind (a race's spare timeout,
 * resolving a promise long settled: Cap's media server leaves one per ffmpeg it runs, firing 5 s
 * later), and npm code's take tens of µs each (Node.js's timer lists run in JavaScript). */
#define TV_LOOP_TIMER_WORK_NS 1000000

/* Fires every timer that is due, each followed by the microtasks it queued (as Node). */
static void tv_fire_timers(void) {
    uint64_t now = tv_loop_update();
    while (tv_ntimers && tv_timers[0].when <= now) {
        tv_timer t = tv_timer_pop();
        if (!t.cb.fn) continue;
        uint64_t t0 = tv_mono_ns();
        void (*fn)(tv_env *) = (void (*)(tv_env *))t.cb.fn;
        if (t.every) {
            tv_env_retain(t.cb.env);   /* the call's own reference: clearInterval may run inside it */
            t.when = now + t.every;
            t.seq = ++tv_timer_seq;
            tv_timer_push(t);
            fn(t.cb.env);
            tv_env_release(t.cb.env);
        } else {
            if (!t.weak) tv_live_timers--;
            fn(t.cb.env);
            tv_env_release(t.cb.env);
        }
        tv_run_microtasks();
        /* (an unref'd timer firing, like AbortSignal.timeout's, isn't the program at work) */
        if (!t.weak && tv_mono_ns() - t0 >= TV_LOOP_TIMER_WORK_NS) tv_loop_work(now);
    }
}

static bool tv_http_busy(void);

/* Node.js's check phase (setImmediate), for npm code (runtime/node.c): while pending, the loop
 * runs tv_loop_check once per turn, after timers and I/O, and doesn't block; a ref'd one keeps the
 * program running. */
void (*tv_loop_check)(void);
bool tv_loop_check_pending, tv_loop_check_ref;

static void tv_loop_run_check(void) {
    if (!tv_loop_check_pending || !tv_loop_check) return;
    tv_loop_check_pending = false;
    tv_loop_check();
}

uint64_t (*tv_loop_host_due)(uint64_t now_ms);
void (*tv_loop_host_run)(void);
bool (*tv_loop_host_busy)(void);
static uint64_t tv_loop_host_at, tv_loop_host_asked;

void (*tv_loop_idle)(bool deep);
void (*tv_loop_will_block)(void);
static bool tv_loop_idled;   /* the loop has been idle since the program started */
/* Set by a tv_io handler whose event was housekeeping, not the program at work (an idle pooled
 * connection closing): it doesn't put off tv_loop_idle. */
bool tv_io_quiet;
#define TV_LOOP_IDLE_MS 1000

/* when tv_loop_idle is next due (0: it isn't) */
static uint64_t tv_loop_idle_due(void) {
    if ((!tv_loop_idle && !tv_sb_cache_bytes) || !tv_loop_worked_at || tv_loop_idle_stage >= 2) return 0;
    return tv_loop_worked_at + (tv_loop_idle_stage == 0 ? TV_LOOP_IDLE_MS : tv_loop_deep_ms);
}

static void tv_fc_rbufs_flush(void);

static void tv_loop_maybe_idle(uint64_t now_ms) {
    uint64_t due = tv_loop_idle_due();
    if (!due || now_ms < due) return;
    /* (the first time, what's compiled is mostly startup code that won't run again) */
    bool deep = tv_loop_idle_stage == 1 || !tv_loop_idled;
    tv_loop_idle_stage = deep ? 2 : 1;
    /* (the first idle's drop, of startup code, says nothing about the program's bursts) */
    if (deep && tv_loop_idled) tv_loop_dropped_at = now_ms;
    tv_loop_idled = true;
    /* (the buffers kept for reuse go back: the burst is over) */
    tv_sb_cache_flush();
    tv_fc_rbufs_flush();
    if (tv_loop_idle) tv_loop_idle(deep);
}

/* Runs the engine's timers that are due; returns when the next is (0: none). Asked at most once
 * a millisecond (the answer costs ~0.1 µs), or now when `fresh` (the loop is about to wait). */
static uint64_t tv_loop_host(uint64_t now_ms, bool fresh) {
    if (!tv_loop_host_due) return 0;
    if (fresh || now_ms != tv_loop_host_asked) {
        tv_loop_host_asked = now_ms;
        tv_loop_host_at = tv_loop_host_due(now_ms);
    }
    if (tv_loop_host_at && tv_loop_host_at <= now_ms) {
        tv_loop_host_run();
        tv_loop_host_at = tv_loop_host_due(now_ms);
        /* (one still due once run: not again this millisecond) */
        if (tv_loop_host_at && tv_loop_host_at <= now_ms) tv_loop_host_at = now_ms + 1;
    }
    return tv_loop_host_at;
}

static bool tv_loop_for_timers;   /* tv_http_loop runs while timers are live, too */
static int tv_http_q;
static void tv_http_loop(void);
static void tv_http_refresh_date(void);

void tv_async_run(void) {
    for (;;) {
        tv_run_microtasks();
        /* while servers run, their loop runs the timers too */
        if (tv_http_busy()) {
            tv_http_run();
            continue;
        }
        while (tv_ntimers && !tv_timers[0].cb.fn) (void)tv_timer_pop();
        bool check = tv_loop_check_pending;
        /* (the engine's threads at work for the program keep it running; asked before the
         * descriptors below: the first time, it starts watching the one they wake the loop with) */
        bool host_busy = tv_loop_host_busy && tv_loop_host_busy();
        if (!tv_live_timers && !(check && tv_loop_check_ref) && !host_busy) return;
        /* descriptors are watched that don't keep the program running (a signal's pipe, an
         * unref'd socket): the timers wait on them too */
        if (tv_http_q >= 0) {
            tv_loop_for_timers = true;
            tv_http_refresh_date();
            tv_http_loop();
            tv_loop_for_timers = false;
            continue;
        }
        if (tv_ntimers && !check && tv_timers[0].when > tv_loop_update()) {
            uint64_t wake = tv_timers[0].when, host = tv_loop_host(tv_loop_ms, true), idle = tv_loop_idle_due();
            if (host && host < wake) wake = host;
            if (idle && idle < wake) wake = idle;
            tv_sleep_until(wake);
            tv_loop_host(tv_loop_update(), false);
            tv_loop_maybe_idle(tv_loop_ms);
        }
        if (tv_ntimers) tv_fire_timers();
        tv_run_microtasks();
        tv_loop_run_check();
    }
}

/* ================================================================== HTTP server */

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <signal.h>
#include <stdatomic.h>
#include <strings.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define TV_KQUEUE 1
#else
#include <sys/epoll.h>
#endif

typedef struct tv_http_conn {
    int fd;
    struct tv_http_server *srv;
    struct tv_http_conn *prev, *next;
    char *in; size_t in_len, in_cap;
    char *out; size_t out_len, out_cap, out_off;
    bool close_after, want_write, want_read, stalled, continued;
    /* in/out currently point at the loop's shared buffers (see tv_http_attach) */
    bool in_shared, out_shared;
    bool eof;             /* the client closed its side */
    bool dirty;           /* on tv_http_dirty: a late response is ready to write */
    bool taken;           /* handed to its handler as a raw socket (an upgrade): leave the fd open */
    struct tv_http_conn *dirty_next;
    /* Requests answered later (async handlers), in request order; see tv_http_req. */
    struct tv_http_req *pend_head, *pend_tail;
    int npending;
} tv_http_conn;

/* A request whose response isn't written yet: its handler returned a promise (it has an id for
 * httpRespondTo), or it was answered while an earlier request on the same connection is still
 * waiting (responses go out in request order). */
typedef struct tv_http_req {
    struct tv_http_req *next;
    tv_http_conn *c;      /* NULL once the connection has closed */
    tv_int id;            /* 0: answered already, waiting for the ones ahead of it */
    bool keep, head, ready;
    bool streaming;       /* raw output so far goes out as soon as it's this request's turn */
    bool gone;            /* its client has gone (told tv_http_on_gone) */
    tv_sb out;            /* the response, once ready */
} tv_http_req;

/* Deferred requests whose clients went away (the connection closed or reset, or the client ended
 * its side) before they were answered: tv_http_on_gone hears each id once, after the event batch.
 * Their responses can still be given; they go nowhere. */
void (*tv_http_on_gone)(tv_int id);
static tv_int *tv_http_gone;
static size_t tv_http_ngone, tv_http_gone_cap;

static void tv_http_note_gone(tv_http_req *r) {
    if (!r->id || r->gone || !tv_http_on_gone) return;
    r->gone = true;
    if (tv_http_ngone == tv_http_gone_cap) {
        tv_http_gone_cap = tv_http_gone_cap ? tv_http_gone_cap * 2 : 16;
        tv_http_gone = tv_realloc(tv_http_gone, tv_http_gone_cap * sizeof *tv_http_gone);
    }
    tv_http_gone[tv_http_ngone++] = r->id;
}

static void tv_http_flush_gone(void) {
    for (size_t i = 0; i < tv_http_ngone; i++) tv_http_on_gone(tv_http_gone[i]);
    tv_http_ngone = 0;
}

/* Pipelined requests handled ahead of a slow one, per connection, before reading pauses. */
#define TV_HTTP_MAX_PENDING 64

/* One read buffer and one write buffer per event loop: a connection borrows them while it is
 * handled, and keeps heap buffers only for leftovers (a partial request, or output the socket
 * didn't take). An idle keep-alive connection holds no buffers. */
#define TV_HTTP_RBUF (64 * 1024)
static char *tv_http_rbuf;
static char *tv_http_wbuf;
static size_t tv_http_wbuf_cap;

typedef struct tv_http_server {
    int fd;              /* -1 once stopped */
    tv_int port;
    tv_fn handler;
    bool node_errors;    /* malformed requests are answered as Node.js's http answers them */
} tv_http_server;

#define TV_HTTP_MAX_SERVERS 64
static tv_http_server tv_http_servers[TV_HTTP_MAX_SERVERS];
static int tv_http_nservers, tv_http_active;
static tv_int tv_http_want_workers = 1;
static int tv_http_q = -1;               /* the event loop's kqueue/epoll (created on first use) */
static tv_http_conn *tv_http_conns;      /* open connections (for stop) */
static tv_http_conn *tv_http_cur;       /* the connection whose request is being handled */
static bool tv_http_keep;               /* the current request allows keep-alive */
static bool tv_http_head;               /* the current request is HEAD: send headers only */
bool tv_http_v10;                       /* the current request is HTTP/1.0 */
static size_t tv_http_rest_off;         /* the current request's end in its connection's input */
static char tv_http_date[64];           /* "date: ...\r\n", refreshed once a second */
static time_t tv_http_date_at;
/* With several workers, each process accepts only while it holds no more connections than
 * the least-loaded worker (+1), so persistent connections spread evenly; counts live in a
 * shared mapping made before fork. */
static _Atomic int64_t *tv_http_loads;
static tv_int tv_http_nworkers = 1, tv_http_worker;
static pid_t tv_http_parent;   /* the supervisor, in worker processes */

static bool tv_http_may_accept(void) {
    if (!tv_http_loads) return true;
    int64_t mine = atomic_load_explicit(&tv_http_loads[tv_http_worker], memory_order_relaxed);
    for (tv_int i = 0; i < tv_http_nworkers; i++)
        if (atomic_load_explicit(&tv_http_loads[i], memory_order_relaxed) + 1 < mine) return false;
    return true;
}

static void tv_http_load_add(int64_t d) {
    if (tv_http_loads) atomic_fetch_add_explicit(&tv_http_loads[tv_http_worker], d, memory_order_relaxed);
}

static void tv_http_out(tv_http_conn *c, const char *s, size_t n) {
    if (c->out_cap - c->out_len < n) {
        size_t cap = c->out_cap ? c->out_cap * 2 : 4096;
        while (cap - c->out_len < n) cap *= 2;
        c->out = tv_realloc(c->out, cap);
        c->out_cap = cap;
        if (c->out_shared) { tv_http_wbuf = c->out; tv_http_wbuf_cap = cap; }
    }
    memcpy(c->out + c->out_len, s, n);
    c->out_len += n;
}

static const char *tv_http_reason(tv_int s) {
    switch (s) {
    case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted"; case 204: return "No Content";
    case 301: return "Moved Permanently"; case 302: return "Found"; case 304: return "Not Modified"; case 307: return "Temporary Redirect"; case 308: return "Permanent Redirect";
    case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
    case 409: return "Conflict"; case 413: return "Payload Too Large"; case 415: return "Unsupported Media Type"; case 422: return "Unprocessable Entity"; case 429: return "Too Many Requests";
    case 500: return "Internal Server Error"; case 501: return "Not Implemented"; case 502: return "Bad Gateway"; case 503: return "Service Unavailable";
    default: return "";
    }
}

static void tv_http_refresh_date(void) {
    time_t now = time(NULL);
    if (now == tv_http_date_at) return;
    tv_http_date_at = now;
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(tv_http_date, sizeof tv_http_date, "date: %a, %d %b %Y %H:%M:%S GMT\r\n", &tm);
}

static bool tv_has_header(tv_str block, const char *name) {
    size_t n = strlen(name);
    const char *s = block.p->data, *end = s + block.p->len;
    while (s < end) {
        if ((size_t)(end - s) > n && strncasecmp(s, name, n) == 0 && s[n] == ':') return true;
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        if (!nl) break;
        s = nl + 1;
    }
    return false;
}

/* Output goes to the connection (c) or to a response kept for later (sb). */
static inline void tv_http_put(tv_http_conn *c, tv_sb *sb, const char *s, size_t n) {
    if (c) tv_http_out(c, s, n);
    else tv_sb_push(sb, s, n);
}

static void tv_http_response(tv_http_conn *c, tv_sb *sb, bool keep, bool head, tv_int status, tv_str headers, tv_str body, bool typed) {
    char line[160], *p = line;
    memcpy(p, "HTTP/1.1 ", 9);
    p += 9;
    if (status < 100 || status > 999) status = 500;
    *p++ = (char)('0' + status / 100);
    *p++ = (char)('0' + status / 10 % 10);
    *p++ = (char)('0' + status % 10);
    *p++ = ' ';
    const char *reason = tv_http_reason(status);
    size_t rl = strlen(reason);
    memcpy(p, reason, rl);
    p += rl;
    *p++ = '\r';
    *p++ = '\n';
    /* 1xx, 204 and 304 responses carry no body (RFC 9110 §6.4.1) */
    bool bodiless = status < 200 || status == 204 || status == 304;
    if (!bodiless) {
        memcpy(p, "content-length: ", 16);
        p += 16;
        char digits[24];
        int nd = 0;
        uint64_t len = (uint64_t)body.p->len;
        do { digits[nd++] = (char)('0' + len % 10); len /= 10; } while (len);
        while (nd) *p++ = digits[--nd];
        *p++ = '\r';
        *p++ = '\n';
    }
    tv_http_put(c, sb, line, (size_t)(p - line));
    if (!bodiless && typed && !tv_has_header(headers, "content-type")) {
        static const char ct[] = "content-type: text/plain;charset=utf-8\r\n";
        tv_http_put(c, sb, ct, sizeof ct - 1);
    }
    tv_http_put(c, sb, tv_http_date, strlen(tv_http_date));
    if (!keep) {
        static const char cl[] = "connection: close\r\n";
        tv_http_put(c, sb, cl, sizeof cl - 1);
    }
    tv_http_put(c, sb, headers.p->data, (size_t)headers.p->len);
    tv_http_put(c, sb, "\r\n", 2);
    if (!bodiless && !head) tv_http_put(c, sb, body.p->data, (size_t)body.p->len);
}

static tv_http_conn *tv_http_dirty;   /* connections with a late response to write */

static void tv_http_mark_dirty(tv_http_conn *c) {
    if (c->dirty) return;
    c->dirty = true;
    c->dirty_next = tv_http_dirty;
    tv_http_dirty = c;
}

static void tv_http_enqueue(tv_http_conn *c, tv_http_req *r) {
    r->next = NULL;
    if (c->pend_tail) c->pend_tail->next = r; else c->pend_head = r;
    c->pend_tail = r;
    c->npending++;
}

/* Writes the ready responses at the front of c's queue, in order (and what a streaming one
 * has so far, once it's at the front). */
static void tv_http_drain(tv_http_conn *c) {
    if (c->pend_head && !c->pend_head->ready && c->pend_head->streaming && c->pend_head->out.len) {
        tv_http_req *r = c->pend_head;
        tv_http_out(c, r->out.data, r->out.len);
        r->out.len = 0;
    }
    while (c->pend_head && c->pend_head->ready) {
        tv_http_req *r = c->pend_head;
        if (r->out.len) tv_http_out(c, r->out.data, r->out.len);
        if (!r->keep) c->close_after = true;
        c->pend_head = r->next;
        if (!c->pend_head) c->pend_tail = NULL;
        c->npending--;
        tv_sb_free(&r->out);
        tv_async_free(r, sizeof *r);
    }
}

void tv_native_httpRespond(tv_int status, tv_str headers, tv_str body, bool typed) {
    tv_http_conn *c = tv_http_cur;
    if (!c) return;
    tv_http_cur = NULL; /* one response per request */
    if (!c->npending) {
        tv_http_response(c, NULL, tv_http_keep, tv_http_head, status, headers, body, typed);
        return;
    }
    /* an earlier request on this connection is still waiting: this response waits behind it */
    tv_http_req *r = tv_async_alloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->c = c;
    r->keep = tv_http_keep;
    r->head = tv_http_head;
    r->ready = true;
    tv_http_response(NULL, &r->out, r->keep, r->head, status, headers, body, typed);
    tv_http_enqueue(c, r);
}

/* Requests waiting for httpRespondTo, by id (slot 0 unused; free slots are reused). */
static tv_http_req **tv_http_deferred;
static tv_int tv_http_ndeferred, tv_http_deferred_cap, tv_http_deferred_free;

tv_int tv_native_httpDefer(void) {
    tv_http_conn *c = tv_http_cur;
    if (!c) return 0;
    tv_http_cur = NULL;
    tv_http_req *r = tv_async_alloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->c = c;
    r->keep = tv_http_keep;
    r->head = tv_http_head;
    tv_int id;
    if (tv_http_deferred_free) {
        id = tv_http_deferred_free;
        tv_http_deferred_free = (tv_int)(intptr_t)tv_http_deferred[id];
    } else {
        if (tv_http_ndeferred + 1 >= tv_http_deferred_cap) {
            tv_http_deferred_cap = tv_http_deferred_cap ? tv_http_deferred_cap * 2 : 64;
            tv_http_deferred = tv_realloc(tv_http_deferred, (size_t)tv_http_deferred_cap * sizeof *tv_http_deferred);
        }
        id = ++tv_http_ndeferred;
    }
    tv_http_deferred[id] = r;
    r->id = id;
    tv_http_enqueue(c, r);
    return id;
}

void tv_native_httpRespondTo(tv_int id, tv_int status, tv_str headers, tv_str body, bool typed) {
    if (id <= 0 || id > tv_http_ndeferred) return;
    tv_http_req *r = tv_http_deferred[id];
    if (!r || (uintptr_t)r <= (uintptr_t)tv_http_ndeferred) return;   /* not waiting (a free slot) */
    tv_http_deferred[id] = (tv_http_req *)(intptr_t)tv_http_deferred_free;
    tv_http_deferred_free = id;
    r->id = 0;
    if (!r->c) { tv_async_free(r, sizeof *r); return; }   /* the client went away */
    tv_http_refresh_date();
    tv_http_conn *c = r->c;
    /* next in line: straight into the connection's output; otherwise it waits its turn */
    if (c->pend_head == r) tv_http_response(c, NULL, r->keep, r->head, status, headers, body, typed);
    else tv_http_response(NULL, &r->out, r->keep, r->head, status, headers, body, typed);
    r->ready = true;
    tv_http_drain(c);   /* writes (in order) and frees what's ready, r included */
    tv_http_mark_dirty(c);
}

/* Raw output for deferred request id: bytes already formatted as an HTTP response (Node.js's
 * http writes its own). They go out as soon as it's this request's turn; end: 0 more is coming,
 * 1 that's all (the connection stays open), 2 that's all and the connection closes after it. */
void tv_native_httpWriteRaw(tv_int id, const char *p, size_t n, int end) {
    if (id <= 0 || id > tv_http_ndeferred) return;
    tv_http_req *r = tv_http_deferred[id];
    if (!r || (uintptr_t)r <= (uintptr_t)tv_http_ndeferred) return;
    if (end) {
        tv_http_deferred[id] = (tv_http_req *)(intptr_t)tv_http_deferred_free;
        tv_http_deferred_free = id;
        r->id = 0;
    }
    if (!r->c) {   /* the client went away */
        if (end) {
            tv_sb_free(&r->out);
            tv_async_free(r, sizeof *r);
        }
        return;
    }
    tv_http_conn *c = r->c;
    r->streaming = true;
    if (c->pend_head == r && !r->out.len) {
        if (n) tv_http_out(c, p, n);
    } else if (n) {
        tv_sb_push(&r->out, p, n);
    }
    if (end) {
        if (end == 2) r->keep = false;
        r->ready = true;
    }
    tv_http_drain(c);
    tv_http_mark_dirty(c);
}

int64_t tv_native_httpBuffered(tv_int id) {
    if (id <= 0 || id > tv_http_ndeferred) return -1;
    tv_http_req *r = tv_http_deferred[id];
    if (!r || (uintptr_t)r <= (uintptr_t)tv_http_ndeferred || !r->c) return -1;
    return (int64_t)(r->c->out_len - r->c->out_off + r->out.len);
}

/* The fd of deferred request id's connection (-1 if it's gone), for its addresses. */
int tv_native_httpFd(tv_int id) {
    if (id <= 0 || id > tv_http_ndeferred) return -1;
    tv_http_req *r = tv_http_deferred[id];
    if (!r || (uintptr_t)r <= (uintptr_t)tv_http_ndeferred || !r->c) return -1;
    return r->c->fd;
}

/* Hands deferred request id's connection to the caller as a raw socket (an upgrade): returns its
 * fd, and the bytes that followed the request in *rest; the server forgets the connection once
 * the handler returns. Only while the request is being handled, and the only one waiting. */
int tv_native_httpTakeover(tv_int id, tv_sb *rest) {
    if (id <= 0 || id > tv_http_ndeferred) return -1;
    tv_http_req *r = tv_http_deferred[id];
    if (!r || (uintptr_t)r <= (uintptr_t)tv_http_ndeferred || !r->c) return -1;
    tv_http_conn *c = r->c;
    if (c->npending != 1 || c->pend_head != r || c->taken) return -1;
    if (tv_http_rest_off < c->in_len) tv_sb_push(rest, c->in + tv_http_rest_off, c->in_len - tv_http_rest_off);
    c->in_len = 0;
    c->taken = true;
    /* the server stops watching the fd now, before its new owner watches it (the same fd and
     * filters: a later delete would undo the new owner's) */
#ifdef TV_KQUEUE
    struct kevent ev[2];
    EV_SET(&ev[0], c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ev[1], c->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(tv_http_q, ev, 1 + (c->want_write ? 1 : 0), NULL, 0, NULL);
#else
    epoll_ctl(tv_http_q, EPOLL_CTL_DEL, c->fd, NULL);
#endif
    c->want_read = c->want_write = false;
    tv_http_deferred[id] = (tv_http_req *)(intptr_t)tv_http_deferred_free;
    tv_http_deferred_free = id;
    c->pend_head = c->pend_tail = NULL;
    c->npending = 0;
    tv_sb_free(&r->out);
    tv_async_free(r, sizeof *r);
    return c->fd;
}

tv_int tv_native_headerIndex(tv_str block, tv_str name) {
    size_t n = (size_t)name.p->len;
    const char *base = block.p->data, *s = base, *end = s + block.p->len;
    while (s < end) {
        if ((size_t)(end - s) > n && strncasecmp(s, name.p->data, n) == 0 && s[n] == ':') {
            const char *v = s + n + 1;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            return (tv_int)(v - base);
        }
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        if (!nl) break;
        s = nl + 1;
    }
    return -1;
}

tv_str tv_native_headerValue(tv_str block, tv_int at) {
    const char *base = block.p->data, *s = base + at, *end = base + block.p->len;
    const char *e = s;
    while (e < end && *e != '\r' && *e != '\n') e++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    return tv_str_from(s, (size_t)(e - s));
}

tv_str tv_native_headerRemove(tv_str block, tv_str name) {
    size_t n = (size_t)name.p->len;
    const char *s = block.p->data, *end = s + block.p->len;
    tv_sb sb = {0};
    while (s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        const char *next = nl ? nl + 1 : end;
        if (!((size_t)(end - s) > n && strncasecmp(s, name.p->data, n) == 0 && s[n] == ':')) tv_sb_push(&sb, s, (size_t)(next - s));
        s = next;
    }
    return tv_str_from_sb(&sb);
}

/* block + "name: value\r\n"; drops bytes that would let a name or value end the line early
 * (CR, LF, NUL; and ':' or whitespace in the name), so user headers can't split a response. */
tv_str tv_native_headerAppend(tv_str block, tv_str name, tv_str value) {
    tv_sb sb = {0};
    tv_sb_push(&sb, block.p->data, (size_t)block.p->len);
    for (int64_t i = 0; i < name.p->len; i++) {
        char ch = name.p->data[i];
        if (ch != '\r' && ch != '\n' && ch != '\0' && ch != ':' && ch != ' ' && ch != '\t') tv_sb_push_char(&sb, ch);
    }
    tv_sb_push(&sb, ": ", 2);
    for (int64_t i = 0; i < value.p->len; i++) {
        char ch = value.p->data[i];
        if (ch != '\r' && ch != '\n' && ch != '\0') tv_sb_push_char(&sb, ch);
    }
    tv_sb_push(&sb, "\r\n", 2);
    return tv_str_from_sb(&sb);
}

TV_STR_LIT(tv_lit_host, "host");

/* A request's full URL: "http://" + Host header (or localhost) + target. */
tv_str tv_native_requestUrl(tv_str headers, tv_str target) {
    tv_int at = tv_native_headerIndex(headers, (tv_str){(tv_strbuf *)&tv_lit_host});
    const char *host = "localhost";
    size_t hl = 9;
    if (at >= 0) {
        const char *base = headers.p->data, *e = base + at, *end = base + headers.p->len;
        while (e < end && *e != '\r' && *e != '\n') e++;
        while (e > base + at && (e[-1] == ' ' || e[-1] == '\t')) e--;
        host = base + at;
        hl = (size_t)(e - host);
    }
    tv_sb sb = {0};
    tv_sb_grow(&sb, 7 + hl + (size_t)target.p->len + 1);
    tv_sb_push(&sb, "http://", 7);
    tv_sb_push(&sb, host, hl);
    tv_sb_push(&sb, target.p->data, (size_t)target.p->len);
    return tv_str_from_sb(&sb);
}

TV_STR_LIT(tv_lit_get, "GET");
TV_STR_LIT(tv_lit_post, "POST");

#define TV_HTTP_MAX_HEAD (64 * 1024)
#define TV_HTTP_MAX_BODY (64 * 1024 * 1024)
#define TV_HTTP_OUT_HIGH (1024 * 1024)   /* stop handling pipelined requests until flushed */

/* A canned error response; the connection closes after it is written. */
static void tv_http_fail(tv_http_conn *c, const char *status) {
    char buf[160];
    int n = c->srv->node_errors ? snprintf(buf, sizeof buf, "HTTP/1.1 %s\r\nConnection: close\r\n\r\n", status)
                                : snprintf(buf, sizeof buf, "HTTP/1.1 %s\r\ncontent-length: 0\r\nconnection: close\r\n\r\n", status);
    tv_http_out(c, buf, (size_t)n);
    c->close_after = true;
}

static bool tv_http_token_eq(const char *v, const char *end, const char *word) {
    size_t n = strlen(word);
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    while (end > v && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    return (size_t)(end - v) == n && strncasecmp(v, word, n) == 0;
}

/* Decodes a chunked body starting at `p`; returns the bytes consumed (through the trailers),
 * 0 if incomplete, or -1 if malformed / too large. */
static long long tv_http_dechunk(const char *p, const char *end, tv_sb *out) {
    const char *s = p;
    for (;;) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        if (!nl) return 0;
        uint64_t size = 0;
        const char *d = s;
        int digits = 0;
        for (; d < nl; d++, digits++) {
            int v = *d >= '0' && *d <= '9' ? *d - '0' : (*d | 32) >= 'a' && (*d | 32) <= 'f' ? (*d | 32) - 'a' + 10 : -1;
            if (v < 0) break;
            size = size * 16 + (uint64_t)v;
            if (size > TV_HTTP_MAX_BODY) return -1;
        }
        if (!digits || (*d != '\r' && *d != ';')) return -1;
        s = nl + 1;
        if (size == 0) {
            /* trailers: lines until an empty one */
            for (;;) {
                const char *t = memchr(s, '\n', (size_t)(end - s));
                if (!t) return 0;
                bool empty = t == s || (t == s + 1 && *s == '\r');
                s = t + 1;
                if (empty) return (long long)(s - p);
            }
        }
        if ((uint64_t)(end - s) < size + 2) return 0;
        if (out->len + size > TV_HTTP_MAX_BODY) return -1;
        tv_sb_push(out, s, (size_t)size);
        s += size;
        if (s[0] != '\r' || s[1] != '\n') return -1;
        s += 2;
    }
}

/* Handles every complete request in c->in (stopping early when the output backs up, which
 * sets c->stalled); returns false if the connection must close at once. */
static bool tv_http_process(tv_http_conn *c) {
    tv_fn h = c->srv->handler;
    size_t pos = 0;
    c->stalled = false;
    while (pos < c->in_len && !c->close_after && c->npending < TV_HTTP_MAX_PENDING) {
        if (c->out_len >= TV_HTTP_OUT_HIGH) { c->stalled = true; break; }
        char *start = c->in + pos;
        size_t avail = c->in_len - pos;
        /* tolerate blank lines between requests (RFC 9112 §2.2) */
        if (start[0] == '\r' || start[0] == '\n') { pos++; continue; }
        char *hdr_end = NULL;
        for (char *q = start; (q = memchr(q, '\n', avail - (size_t)(q - start))) != NULL; q++) {
            if (q + 2 < start + avail && q[1] == '\r' && q[2] == '\n') { hdr_end = q + 3; break; }
            if (q + 1 < start + avail && q[1] == '\n') { hdr_end = q + 2; break; }
        }
        if (!hdr_end) {
            if (avail > TV_HTTP_MAX_HEAD) tv_http_fail(c, "431 Request Header Fields Too Large");
            break;
        }
        if ((size_t)(hdr_end - start) > TV_HTTP_MAX_HEAD) { tv_http_fail(c, "431 Request Header Fields Too Large"); break; }
        /* Request line: METHOD SP TARGET SP HTTP/1.x CRLF */
        char *line_end = memchr(start, '\n', (size_t)(hdr_end - start));
        char *eol = line_end > start && line_end[-1] == '\r' ? line_end - 1 : line_end;
        char *sp1 = memchr(start, ' ', (size_t)(eol - start));
        char *sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1)) : NULL;
        if (!sp1 || sp1 == start || !sp2 || sp2 == sp1 + 1 || eol - sp2 != 9 || memcmp(sp2 + 1, "HTTP/1.", 7) != 0 || (sp2[8] != '0' && sp2[8] != '1')) {
            tv_http_fail(c, "400 Bad Request");
            break;
        }
        bool http10 = sp2[8] == '0';
        char *headers = line_end + 1;
        char *headers_end = hdr_end - (hdr_end[-2] == '\r' ? 2 : 1);
        long long clen = -1;
        bool keep = !http10, chunked = false, expect = false, bad = false;
        for (char *s = headers; s < headers_end;) {
            char *nl = memchr(s, '\n', (size_t)(hdr_end - s));
            size_t len = (size_t)(nl - s);
            if (len > 15 && strncasecmp(s, "content-length:", 15) == 0) {
                const char *v = s + 15, *e = nl;
                while (v < e && (*v == ' ' || *v == '\t')) v++;
                while (e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
                long long n = 0;
                if (v == e) bad = true;
                for (; v < e; v++) {
                    if (*v < '0' || *v > '9') { bad = true; break; }
                    if (n <= TV_HTTP_MAX_BODY) n = n * 10 + (*v - '0'); /* saturates: over the cap → 413 */
                }
                if (clen >= 0 && clen != n) bad = true;
                clen = n;
            } else if (len > 18 && strncasecmp(s, "transfer-encoding:", 18) == 0) {
                if (tv_http_token_eq(s + 18, nl, "chunked")) chunked = true;
                else bad = true; /* other codings are not supported */
            } else if (len > 11 && strncasecmp(s, "connection:", 11) == 0) {
                if (tv_http_token_eq(s + 11, nl, "close")) keep = false;
                else if (tv_http_token_eq(s + 11, nl, "keep-alive")) keep = true;
            } else if (len > 7 && strncasecmp(s, "expect:", 7) == 0) {
                expect = tv_http_token_eq(s + 7, nl, "100-continue");
            }
            s = nl + 1;
        }
        if (bad || (chunked && clen >= 0)) { tv_http_fail(c, "400 Bad Request"); break; }
        if (clen > TV_HTTP_MAX_BODY) { tv_http_fail(c, "413 Payload Too Large"); break; }
        char *in_end = c->in + c->in_len;
        size_t consumed;
        tv_str body;
        if (chunked) {
            tv_sb sb = {0};
            long long n = tv_http_dechunk(hdr_end, in_end, &sb);
            if (n < 0) { tv_sb_free(&sb); tv_http_fail(c, "400 Bad Request"); break; }
            if (n == 0) {
                tv_sb_free(&sb);
                if (expect && !c->continued) { tv_http_out(c, "HTTP/1.1 100 Continue\r\n\r\n", 25); c->continued = true; }
                break;
            }
            consumed = (size_t)(hdr_end - start) + (size_t)n;
            body = tv_str_from_sb(&sb);
        } else {
            if (clen < 0) clen = 0;
            if ((size_t)(in_end - hdr_end) < (size_t)clen) {
                if (expect && !c->continued) { tv_http_out(c, "HTTP/1.1 100 Continue\r\n\r\n", 25); c->continued = true; }
                break; /* body incomplete */
            }
            consumed = (size_t)(hdr_end - start) + (size_t)clen;
            body = clen ? tv_str_from(hdr_end, (size_t)clen) : TV_EMPTY_STR;
        }
        c->continued = false;
        size_t mlen = (size_t)(sp1 - start);
        tv_str method = mlen == 3 && memcmp(start, "GET", 3) == 0 ? TV_LIT(tv_lit_get) : mlen == 4 && memcmp(start, "POST", 4) == 0 ? TV_LIT(tv_lit_post) : tv_str_from(start, mlen);
        tv_str target = tv_str_from(sp1 + 1, (size_t)(sp2 - sp1 - 1));
        tv_str hdrs = tv_str_from(headers, (size_t)(headers_end - headers));
        tv_http_cur = c;
        tv_http_keep = keep;
        tv_http_v10 = http10;
        tv_http_rest_off = pos + consumed;
        tv_http_head = mlen == 4 && memcmp(start, "HEAD", 4) == 0;
        ((void (*)(tv_env *, tv_str, tv_str, tv_str, tv_str))h.fn)(h.env, method, target, hdrs, body);
        if (tv_http_cur) tv_native_httpRespond(500, TV_EMPTY_STR, TV_EMPTY_STR, false); /* no response */
        tv_run_microtasks();   /* each request is a macrotask, as in JavaScript */
        tv_str_release(method);
        tv_str_release(target);
        tv_str_release(hdrs);
        tv_str_release(body);
        pos += consumed;
        /* taken over (an upgrade): what followed is the handler's now */
        if (c->taken) return true;
        if (!keep) c->close_after = true;
    }
    if (pos) {
        memmove(c->in, c->in + pos, c->in_len - pos);
        c->in_len -= pos;
    }
    return true;
}

static bool tv_http_flush(tv_http_conn *c) {
    while (c->out_off < c->out_len) {
        ssize_t w = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            return false;
        }
        c->out_off += (size_t)w;
    }
    c->out_off = c->out_len = 0;
    return true;
}

static tv_http_conn tv_http_dead;   /* stands in for connections closed earlier in an event batch */

static void tv_http_release(tv_http_conn *c, bool close_fd);

static void tv_http_close(tv_http_conn *c) { tv_http_release(c, true); }

/* Forgets connection c (closing its fd unless it was taken over). */
static void tv_http_release(tv_http_conn *c, bool close_fd) {
    tv_http_load_add(-1);
    if (c->dirty) {
        for (tv_http_conn **pp = &tv_http_dirty; *pp; pp = &(*pp)->dirty_next)
            if (*pp == c) { *pp = c->dirty_next; break; }
    }
    /* waiting requests outlive the connection until they're answered (then dropped) */
    for (tv_http_req *r = c->pend_head, *next; r; r = next) {
        next = r->next;
        if (r->id) {
            tv_http_note_gone(r);
            r->c = NULL;
        } else {
            tv_sb_free(&r->out);
            tv_async_free(r, sizeof *r);
        }
    }
    if (c->prev) c->prev->next = c->next; else tv_http_conns = c->next;
    if (c->next) c->next->prev = c->prev;
    /* (a connection taken over left the loop's watch when it was taken: see httpTakeover) */
    if (close_fd) close(c->fd);
    if (!c->in_shared) tv_free(c->in);
    if (!c->out_shared) tv_free(c->out);
    tv_free(c);
}

/* Lends the shared buffers to c (when it has no leftovers of its own). */
static void tv_http_attach(tv_http_conn *c) {
    if (c->in_len == 0 && !c->in_shared) {
        tv_free(c->in);
        if (!tv_http_rbuf) tv_http_rbuf = tv_alloc(TV_HTTP_RBUF); /* only the pages reads touch are resident */
        c->in = tv_http_rbuf;
        c->in_cap = TV_HTTP_RBUF;
        c->in_shared = true;
    }
    if (c->out_len == 0 && !c->out_shared) {
        tv_free(c->out);
        if (!tv_http_wbuf) { tv_http_wbuf_cap = 64 * 1024; tv_http_wbuf = tv_alloc(tv_http_wbuf_cap); }
        c->out = tv_http_wbuf;
        c->out_cap = tv_http_wbuf_cap;
        c->out_off = 0;
        c->out_shared = true;
    }
}

/* Gives the shared buffers back, copying any leftovers to c's own (exact-size) buffers. */
static void tv_http_detach(tv_http_conn *c) {
    if (c->in_shared) {
        c->in_shared = false;
        if (c->in_len) {
            size_t cap = c->in_len < 4096 ? 4096 : c->in_len * 2;
            char *p = tv_alloc(cap);
            memcpy(p, c->in, c->in_len);
            c->in = p;
            c->in_cap = cap;
        } else {
            c->in = NULL;
            c->in_cap = 0;
        }
    }
    if (c->out_shared) {
        c->out_shared = false;
        size_t left = c->out_len - c->out_off;
        if (left) {
            char *p = tv_alloc(left);
            memcpy(p, c->out + c->out_off, left);
            c->out = p;
            c->out_cap = c->out_len = left;
        } else {
            c->out = NULL;
            c->out_cap = c->out_len = 0;
        }
        c->out_off = 0;
        /* a huge response grew the shared buffer: don't keep it */
        if (tv_http_wbuf_cap > (size_t)1 << 20) {
            tv_free(tv_http_wbuf);
            tv_http_wbuf = NULL;
            tv_http_wbuf_cap = 0;
        }
    }
    if (c->in && c->in_len == 0) { tv_free(c->in); c->in = NULL; c->in_cap = 0; }
    if (c->out && c->out_len == 0) { tv_free(c->out); c->out = NULL; c->out_cap = 0; c->out_off = 0; }
}

/* ---- servers: registered by listen (at any time), served by tv_http_run after the program */


/* Other I/O on the loop (the fetch client, runtime/node.c; tv_io in tov.h): its events' udata
 * is a tv_io pointer with bit 0 set. tv_io_refs counts what keeps the loop running (requests in
 * flight, DNS lookups); idle pooled connections don't. After each batch of events the loop calls
 * tv_io_after_batch (which frees handles closed during the batch). Not static: nothing binds the
 * loop to the client's code, so programs that never fetch don't link it. */
int tv_io_refs;
void (*tv_io_after_batch)(void);
/* Before the loop waits for events (runtime/net.c writes out what sockets were given). */
static void (*tv_loop_before_wait_net)(void);
void (*tv_io_after_fork)(void);

static int tv_loop_queue(void) {
    if (tv_http_q < 0) {
#ifdef TV_KQUEUE
        tv_http_q = kqueue();
#else
        tv_http_q = epoll_create1(EPOLL_CLOEXEC);
#endif
        if (tv_http_q < 0) tv_trap("can't create the event loop", NULL);
        fcntl(tv_http_q, F_SETFD, FD_CLOEXEC);
    }
    return tv_http_q;
}

static bool tv_http_is_server(void *p) {
    return (char *)p >= (char *)tv_http_servers && (char *)p < (char *)(tv_http_servers + TV_HTTP_MAX_SERVERS);
}

static void tv_http_watch_listener(tv_http_server *sv) {
#ifdef TV_KQUEUE
    struct kevent ev;
    EV_SET(&ev, sv->fd, EVFILT_READ, EV_ADD, 0, 0, sv);
    kevent(tv_http_q, &ev, 1, NULL, 0, NULL);
#else
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = sv };
    epoll_ctl(tv_http_q, EPOLL_CTL_ADD, sv->fd, &ev);
#endif
}

static void tv_http_accept(tv_http_server *sv) {
    while (sv->fd >= 0 && tv_http_may_accept()) {
        /* (close-on-exec: a child the program starts, ffmpeg say, mustn't keep its connections:
         * on Linux, epoll keeps reporting a socket closed here while a child holds a copy) */
#ifdef __linux__
        int fd = accept4(sv->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) break;
#else
        int fd = accept(sv->fd, NULL, NULL);
        if (fd < 0) break;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#endif
        tv_http_load_add(1);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        tv_http_conn *nc = tv_alloc(sizeof *nc);
        memset(nc, 0, sizeof *nc);
        nc->fd = fd;
        nc->srv = sv;
        nc->want_read = true;
        nc->next = tv_http_conns;
        if (tv_http_conns) tv_http_conns->prev = nc;
        tv_http_conns = nc;
#ifdef TV_KQUEUE
        struct kevent cev;
        EV_SET(&cev, fd, EVFILT_READ, EV_ADD, 0, 0, nc);
        kevent(tv_http_q, &cev, 1, NULL, 0, NULL);
#else
        struct epoll_event cev = { .events = EPOLLIN | EPOLLRDHUP, .data.ptr = nc };
        epoll_ctl(tv_http_q, EPOLL_CTL_ADD, fd, &cev);
#endif
    }
}

/* Reads (when readable), handles the complete requests, writes; closes the connection once it's
 * finished. Returns false if it was closed. */
static bool tv_http_service(tv_http_conn *c, bool readable, bool broken) {
    bool ok = !broken;
    tv_http_attach(c);
    if (readable && c->want_read) {
        for (;;) {
            if (c->in_cap - c->in_len < 4096) {
                size_t cap = c->in_cap ? c->in_cap * 2 : 8192;
                if (c->in_shared) { /* more than the shared buffer holds: move to its own */
                    char *p = tv_alloc(cap);
                    memcpy(p, c->in, c->in_len);
                    c->in = p;
                    c->in_shared = false;
                } else {
                    c->in = tv_realloc(c->in, cap);
                }
                c->in_cap = cap;
            }
            size_t room = c->in_cap - c->in_len;
            ssize_t r = read(c->fd, c->in + c->in_len, room);
            /* Level-triggered: a short read means the socket is drained, so skip the read that
             * would only return EAGAIN (one syscall per request saved). */
            if (r > 0) { c->in_len += (size_t)r; if ((size_t)r == room && c->in_len < TV_HTTP_MAX_BODY + TV_HTTP_MAX_HEAD) continue; break; }
            if (r == 0) {
                c->eof = true;
                /* (as Node.js's http and Bun do: a client that ends its side has gone) */
                for (tv_http_req *q = c->pend_head; q; q = q->next) tv_http_note_gone(q);
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
    }
    /* handle, write, and handle again while flushing unblocks pipelined requests */
    while (ok) {
        if (c->in_len && !c->close_after && c->npending < TV_HTTP_MAX_PENDING) ok = tv_http_process(c);
        if (c->taken) {
            tv_http_detach(c);
            tv_http_release(c, false);
            return false;
        }
        if (ok && c->out_len) ok = tv_http_flush(c);
        if (!(ok && c->stalled && c->out_len == 0)) break;
    }
    /* a stopped server finishes the requests in hand, then closes its connections */
    if (c->srv->fd < 0 && c->in_len == 0) c->close_after = true;
    if (ok && c->out_len == 0 && c->npending == 0 && (c->close_after || c->eof)) ok = false;
    if (!ok) {
        tv_http_detach(c);
        tv_http_close(c);
        return false;
    }
    tv_http_detach(c);
    /* interest: write while output is pending; read unless output is backed up, or too many
     * requests are waiting for their responses */
    bool want_write = c->out_len > 0;
    bool want_read = !(c->stalled || c->eof || (c->close_after && c->out_len) || c->npending >= TV_HTTP_MAX_PENDING);
#ifdef TV_KQUEUE
    struct kevent mods[2];
    int nm = 0;
    if (want_write != c->want_write) EV_SET(&mods[nm++], c->fd, EVFILT_WRITE, want_write ? EV_ADD : EV_DELETE, 0, 0, c);
    if (want_read != c->want_read) EV_SET(&mods[nm++], c->fd, EVFILT_READ, want_read ? EV_ENABLE : EV_DISABLE, 0, 0, c);
    if (nm) kevent(tv_http_q, mods, nm, NULL, 0, NULL);
#else
    if (want_write != c->want_write || want_read != c->want_read) {
        struct epoll_event wev = { .events = (want_read ? EPOLLIN | EPOLLRDHUP : 0) | (want_write ? EPOLLOUT : 0), .data.ptr = c };
        epoll_ctl(tv_http_q, EPOLL_CTL_MOD, c->fd, &wev);
    }
#endif
    c->want_write = want_write;
    c->want_read = want_read;
    return true;
}

static char tv_http_timer_tag;   /* the loop's timer event (the next setTimeout) */

/* whether the loop goes on: servers, connections or ref'd I/O, a ref'd check phase, (run for
 * timers, see tv_async_run) timers, or the engine's threads at work for the program */
static bool tv_http_alive(void) {
    return tv_http_active > 0 || tv_http_conns || tv_io_refs > 0 || (tv_loop_check_pending && tv_loop_check_ref)
        || (tv_loop_for_timers && tv_live_timers > 0) || (tv_loop_host_busy && tv_loop_host_busy());
}

/* Runs the event loop until every server has stopped and its connections have closed; timers
 * and microtasks run in it too. */
static void tv_http_loop(void) {
    tv_loop_queue();
    tv_loop_work(tv_loop_update());   /* (starting up is work: its garbage goes once idle) */
#ifdef TV_KQUEUE
    struct kevent events[256];
#else
    struct epoll_event events[256];
#endif
    for (int i = 0; i < tv_http_nservers; i++)
        if (tv_http_servers[i].fd >= 0) tv_http_watch_listener(&tv_http_servers[i]);
#ifdef TV_KQUEUE
    if (tv_http_parent) { /* a worker exits with its supervisor (Linux uses PR_SET_PDEATHSIG) */
        struct kevent ev;
        EV_SET(&ev, tv_http_parent, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, &tv_http_dead);
        if (kevent(tv_http_q, &ev, 1, NULL, 0, NULL) != 0) _exit(0);
    }
#endif
    while (tv_http_alive()) {
        tv_run_microtasks();
        /* connections whose late responses are ready */
        while (tv_http_dirty) {
            tv_http_conn *c = tv_http_dirty;
            tv_http_dirty = c->dirty_next;
            c->dirty = false;
            tv_http_service(c, false, false);
            tv_run_microtasks();
        }
        if (tv_loop_before_wait_net) tv_loop_before_wait_net();
        if (!tv_http_alive()) break;
        if (tv_out_len) tv_out_flush(); /* handler logs reach pipes and files promptly */
        while (tv_ntimers && !tv_timers[0].cb.fn) (void)tv_timer_pop();
        /* wait for I/O, or until the next timer is due */
        bool timed = tv_ntimers > 0; /* unref'd timers fire too while the loop runs */
        uint64_t now_ms = tv_loop_update();
        uint64_t due = timed ? tv_timers[0].when : 0;
        /* (the engine's timers wake it too, without keeping it running) */
        bool waits = !(timed && due <= now_ms) && !tv_loop_check_pending;
        uint64_t host = tv_loop_host(now_ms, waits), idle = tv_loop_idle_due();
        if (host && (!due || host < due)) due = host;
        if (idle && (!due || idle < due)) due = idle;
#ifdef TV_KQUEUE
        struct kevent tch;
        int nch = 0;
        struct timespec zero = {0, 0};
        if (due && due > now_ms) {
            /* a kqueue timer marked critical wakes within ~0.1 ms (a plain timeout: ~1 ms late) */
            uint64_t wait = due * 1000000u > tv_mono_ns() ? due * 1000000u - tv_mono_ns() : 0;
            EV_SET(&tch, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, (int64_t)wait, &tv_http_timer_tag);
            nch = 1;
        }
        /* (a pending check phase doesn't wait) */
        bool blocks = !((due && due <= now_ms) || tv_loop_check_pending);
        if (blocks && tv_loop_will_block) tv_loop_will_block();
        int n = kevent(tv_http_q, &tch, nch, events, 256, blocks ? NULL : &zero);
#else
        int timeout = -1;
        if (due) timeout = due > now_ms ? (int)(due - now_ms) : 0;
        if (tv_loop_check_pending) timeout = 0; /* (a pending check phase doesn't wait) */
        if (timeout != 0 && tv_loop_will_block) tv_loop_will_block();
        int n = epoll_wait(tv_http_q, events, 256, timeout);
#endif
        if (n < 0) { if (errno == EINTR) continue; break; }
        tv_http_refresh_date();
        if (timed) tv_fire_timers();
        bool worked = false;
        for (int i = 0; i < n; i++) {
#ifdef TV_KQUEUE
            void *tag = events[i].udata;
            bool readable = events[i].filter == EVFILT_READ, broken = (events[i].flags & EV_ERROR) != 0;
#else
            void *tag = events[i].data.ptr;
            bool readable = events[i].events & (EPOLLIN | EPOLLRDHUP), broken = events[i].events & (EPOLLHUP | EPOLLERR);
#endif
            if (tag == &tv_http_timer_tag) continue;
            if ((uintptr_t)tag & 1) {
                tv_io *h = (tv_io *)((uintptr_t)tag - 1);
                tv_io_quiet = false;
#ifdef TV_KQUEUE
                h->ready(h, events[i].filter == EVFILT_READ, events[i].filter == EVFILT_WRITE, broken);
#else
                h->ready(h, (events[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0, (events[i].events & (EPOLLOUT | EPOLLERR)) != 0, false);
#endif
                if (!tv_io_quiet) worked = true;
                continue;
            }
            worked = true;
            if (tv_http_is_server(tag)) {
                tv_http_accept(tag);
                continue;
            }
            tv_http_conn *c = tag;
            if (c == &tv_http_dead) {
#ifdef TV_KQUEUE
                if (events[i].filter == EVFILT_PROC) _exit(0);
#endif
                continue;
            }
            if (!tv_http_service(c, readable, broken)) {
                for (int j = i + 1; j < n; j++) {
#ifdef TV_KQUEUE
                    if (events[j].udata == c) events[j].udata = &tv_http_dead;
#else
                    if (events[j].data.ptr == c) events[j].data.ptr = &tv_http_dead;
#endif
                }
            }
        }
        if (tv_io_after_batch) tv_io_after_batch();
        if (tv_http_ngone) tv_http_flush_gone();
        tv_run_microtasks();
        tv_loop_run_check();
        if (worked) tv_loop_work(tv_loop_update());
        else tv_loop_maybe_idle(tv_loop_update());
    }
    /* the queue stays: pooled client connections are still registered with it */
}

/* ---- workers: the parent supervises; each child runs the event loop */

static volatile sig_atomic_t tv_http_stop_sig;

static pid_t *volatile tv_http_kids;
static tv_int tv_http_nkids;

/* May run on any thread (the program runs off the main thread), so it stops the workers
 * itself; their exit wakes the supervisor's waitpid. */
static void tv_http_on_stop(int sig) {
    tv_http_stop_sig = sig;
    pid_t *kids = tv_http_kids;
    if (kids)
        for (tv_int w = 0; w < tv_http_nkids; w++)
            if (kids[w] > 0) kill(kids[w], SIGTERM);
}

static pid_t tv_http_spawn(tv_int w) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    /* child */
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    tv_http_worker = w;
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    if (getppid() != tv_http_parent) _exit(0); /* the supervisor died before prctl */
    /* the parent's event queue (epoll's is shared after fork) and client state aren't ours */
    if (tv_http_q >= 0) { close(tv_http_q); tv_http_q = -1; }
    if (tv_io_after_fork) tv_io_after_fork();
    /* Give back the free malloc pages inherited from the program's setup. */
#if defined(__APPLE__)
    malloc_zone_pressure_relief(NULL, 0);
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
    tv_http_refresh_date();
    tv_http_loop();
    tv_out_flush();
    _exit(0);
}

/* Forks `workers` children and keeps them running: a worker that dies (a trap in a handler)
 * is replaced, after a second's pause if it died within a second of starting. SIGTERM and
 * SIGINT stop the workers and then the supervisor. */
static void tv_http_supervise(tv_int workers) {
    tv_http_parent = getpid();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = tv_http_on_stop; /* no SA_RESTART: waitpid returns EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    pid_t *kids = tv_alloc((size_t)workers * sizeof *kids);
    time_t *born = tv_alloc((size_t)workers * sizeof *born);
    for (tv_int w = 0; w < workers; w++) kids[w] = 0;
    tv_http_nkids = workers;
    tv_http_kids = kids;
    for (tv_int w = 0; w < workers; w++) {
        kids[w] = tv_http_spawn(w);
        born[w] = time(NULL);
    }
    tv_int alive = workers;
    while (!tv_http_stop_sig && alive > 0) {
        int st;
        pid_t pid = waitpid(-1, &st, 0);
        if (pid < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (tv_int w = 0; w < workers; w++) {
            if (kids[w] != pid) continue;
            if (tv_http_stop_sig) break;
            /* a worker whose servers all stopped exits cleanly; one that crashed is replaced */
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                kids[w] = 0;
                alive--;
                break;
            }
            /* Killed by a stop signal (the process group was signalled, and this thread saw the
             * worker exit before the handler ran): that's a stop, not a crash. */
            if (WIFSIGNALED(st) && (WTERMSIG(st) == SIGTERM || WTERMSIG(st) == SIGINT || WTERMSIG(st) == SIGKILL)) {
                kids[w] = 0;
                tv_http_stop_sig = WTERMSIG(st) == SIGKILL ? SIGTERM : WTERMSIG(st);
                break;
            }
            if (time(NULL) - born[w] < 1) sleep(1); /* crash loop: don't spin */
            if (tv_http_loads) atomic_store(&tv_http_loads[w], 0);
            kids[w] = tv_http_spawn(w);
            born[w] = time(NULL);
        }
    }
    if (!tv_http_stop_sig) return; /* every worker finished */
    int sig = tv_http_stop_sig ? tv_http_stop_sig : SIGTERM;
    for (tv_int w = 0; w < workers; w++) if (kids[w] > 0) kill(kids[w], SIGTERM);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {}
    tv_out_flush();
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

static bool tv_http_busy(void) { return tv_http_active > 0 || tv_http_conns || tv_io_refs > 0; }

void tv_http_run(void) {
    if (!tv_http_busy()) return;
    tv_out_flush();
    tv_int workers = tv_http_want_workers;
    if (workers <= 1 || tv_http_active == 0) {
        tv_http_refresh_date();
        tv_http_loop();
        return;
    }
    void *m = mmap(NULL, (size_t)workers * sizeof(int64_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (m != MAP_FAILED) { tv_http_loads = m; tv_http_nworkers = workers; }
    tv_http_supervise(workers);
    /* every worker has finished: so have this process's servers */
    for (int i = 0; i < tv_http_nservers; i++)
        if (tv_http_servers[i].fd >= 0) { close(tv_http_servers[i].fd); tv_http_servers[i].fd = -1; }
    tv_http_active = 0;
}

tv_int tv_native_httpListen(tv_int port, tv_str host, tv_fn handler) {
    signal(SIGPIPE, SIG_IGN);
    if (tv_http_nservers == TV_HTTP_MAX_SERVERS) { tv_sb_push_cstr(&tv_native_err, "too many servers"); return -1; }
    const char *h = host.p->len ? host.p->data : "0.0.0.0";
    if (strcmp(h, "localhost") == 0) h = "127.0.0.1";
    /* an IPv6 address (with "::" both IPv6 and IPv4, as Node.js listens by default) */
    bool v6 = strchr(h, ':') != NULL;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    socklen_t slen;
    if (v6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons((uint16_t)port);
        if (inet_pton(AF_INET6, h, &a6->sin6_addr) != 1) { tv_native_fail(EINVAL, "listen", host); return -1; }
        slen = sizeof *a6;
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&ss;
        a4->sin_family = AF_INET;
        a4->sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, h, &a4->sin_addr) != 1) { tv_native_fail(EINVAL, "listen", host); return -1; }
        slen = sizeof *a4;
    }
    int lfd = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { tv_native_fail(errno, "socket", host); return -1; }
    fcntl(lfd, F_SETFD, FD_CLOEXEC);   /* (a child the program starts doesn't hold the port) */
    int one = 1, zero = 0;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (v6) setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    if (bind(lfd, (struct sockaddr *)&ss, slen) != 0) {
        tv_sb_push_cstr(&tv_native_err, errno == EADDRINUSE ? "Failed to start server. Is port " : "Failed to start server on port ");
        tv_sb_push_int(&tv_native_err, port);
        tv_sb_push_cstr(&tv_native_err, errno == EADDRINUSE ? " in use?" : "");
        close(lfd);
        return -1;
    }
    if (listen(lfd, 4096) != 0) { tv_native_fail(errno, "listen", host); close(lfd); return -1; }
    fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL) | O_NONBLOCK);
    socklen_t len = sizeof ss;
    getsockname(lfd, (struct sockaddr *)&ss, &len);
    uint16_t bound = v6 ? ((struct sockaddr_in6 *)&ss)->sin6_port : ((struct sockaddr_in *)&ss)->sin_port;
    int id = tv_http_nservers++;
    tv_env_retain(handler.env); /* kept for the life of the program */
    tv_http_servers[id] = (tv_http_server){ .fd = lfd, .port = ntohs(bound), .handler = handler };
    tv_http_active++;
    if (tv_http_q >= 0) tv_http_watch_listener(&tv_http_servers[id]); /* started from a handler */
    return id;
}

void tv_native_httpNodeErrors(tv_int id) { if (id >= 0 && id < tv_http_nservers) tv_http_servers[id].node_errors = true; }

tv_int tv_native_httpPort(tv_int id) { return id >= 0 && id < tv_http_nservers ? tv_http_servers[id].port : 0; }

/* Stops accepting; open connections finish the request in hand and close. */
void tv_native_httpStop(tv_int id, bool force) {
    if (id < 0 || id >= tv_http_nservers || tv_http_servers[id].fd < 0) return;
    close(tv_http_servers[id].fd);
    tv_http_servers[id].fd = -1;
    tv_http_active--;
    /* Connections close from the loop, not here: stop can run mid-batch (from a handler or a
     * callback), with events for them still to come. An idle one (or every one, when forced)
     * is shut down, and closes at its next event; the rest close after the request in hand. */
    for (tv_http_conn *c = tv_http_conns; c; c = c->next) {
        if (c->srv != &tv_http_servers[id]) continue;
        c->close_after = true;
        if (force || (c->in_len == 0 && c->out_len == 0 && c->npending == 0 && c != tv_http_cur)) shutdown(c->fd, SHUT_RDWR);
    }
}

void tv_native_httpWorkers(tv_int n) {
    if (n > tv_http_want_workers) tv_http_want_workers = n > 1024 ? 1024 : n;
}

/* ================================================================== URLs */

static bool tv_url_special(const char *scheme, size_t n) {
    return (n == 4 && memcmp(scheme, "http", 4) == 0) || (n == 5 && memcmp(scheme, "https", 5) == 0) || (n == 2 && memcmp(scheme, "ws", 2) == 0) ||
           (n == 3 && memcmp(scheme, "wss", 3) == 0) || (n == 3 && memcmp(scheme, "ftp", 3) == 0) || (n == 4 && memcmp(scheme, "file", 4) == 0);
}

static const char *tv_url_default_port(const char *scheme, size_t n) {
    if (n == 4 && memcmp(scheme, "http", 4) == 0) return "80";
    if (n == 5 && memcmp(scheme, "https", 5) == 0) return "443";
    if (n == 2 && memcmp(scheme, "ws", 2) == 0) return "80";
    if (n == 3 && memcmp(scheme, "wss", 3) == 0) return "443";
    if (n == 3 && memcmp(scheme, "ftp", 3) == 0) return "21";
    return NULL;
}

/* Length of a scheme at the start of s (letters, then letters/digits/+-.), then ':'; 0 if none. */
static size_t tv_url_scheme_len(const char *s, size_t n) {
    if (n == 0 || !((s[0] | 32) >= 'a' && (s[0] | 32) <= 'z')) return 0;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (c == ':') return i;
        if (!(((c | 32) >= 'a' && (c | 32) <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) return 0;
    }
    return 0;
}

/* Appends `s` percent-encoding bytes a URL can't hold as-is in this part. */
static void tv_url_push_encoded(tv_sb *sb, const char *s, size_t n, bool query) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        bool enc = c <= 0x20 || c >= 0x7f || c == '"' || c == '<' || c == '>' || (!query && (c == '`' || c == '{' || c == '}')) || (query && c == '\'');
        if (enc) {
            char e[3] = { '%', hex[c >> 4], hex[c & 15] };
            tv_sb_push(sb, e, 3);
        } else {
            tv_sb_push_char(sb, (char)c);
        }
    }
}

/* Removes "." and ".." segments from an absolute path (in place in sb). */
static void tv_url_normalize_path(tv_sb *out, const char *p, size_t n) {
    /* segments stack as offsets into out */
    size_t starts[256];
    int depth = 0;
    out->len = 0;
    size_t i = 0;
    if (n == 0 || p[0] != '/') { tv_sb_push_char(out, '/'); }
    while (i < n) {
        if (p[i] == '/') i++;
        size_t j = i;
        while (j < n && p[j] != '/') j++;
        size_t len = j - i;
        bool last = j >= n;
        bool dot = (len == 1 && p[i] == '.') || (len == 3 && (memcmp(p + i, "%2e", 3) == 0 || memcmp(p + i, "%2E", 3) == 0));
        bool dotdot = (len == 2 && p[i] == '.' && p[i + 1] == '.');
        if (dotdot) {
            if (depth > 0) out->len = starts[--depth];
            if (last) tv_sb_push_char(out, '/');
        } else if (dot) {
            if (last) tv_sb_push_char(out, '/');
        } else {
            if (depth < 256) starts[depth++] = (size_t)out->len;
            tv_sb_push_char(out, '/');
            tv_url_push_encoded(out, p + i, len, false);
        }
        i = j;
        if (i < n && p[i] == '/' && i + 1 == n) { /* trailing slash */
            tv_sb_push_char(out, '/');
            break;
        }
    }
    if (out->len == 0) tv_sb_push_char(out, '/');
}

typedef struct tv_url_parts {
    tv_sb proto, host, port, path, search, hash, userinfo;
    bool authority, has_query, has_hash;
} tv_url_parts;

static void tv_url_free(tv_url_parts *u) {
    tv_sb_free(&u->proto); tv_sb_free(&u->host); tv_sb_free(&u->port);
    tv_sb_free(&u->path); tv_sb_free(&u->search); tv_sb_free(&u->hash); tv_sb_free(&u->userinfo);
}

/* Parses an absolute URL; false if it isn't one. */
static bool tv_url_parse_abs(const char *s, size_t n, tv_url_parts *u) {
    size_t sl = tv_url_scheme_len(s, n);
    if (sl == 0) return false;
    for (size_t i = 0; i < sl; i++) tv_sb_push_char(&u->proto, (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]));
    tv_sb_push_char(&u->proto, ':');
    bool special = tv_url_special(u->proto.data, sl);
    size_t i = sl + 1;
    /* hash and query end the rest */
    size_t hash_at = n, query_at = n;
    for (size_t k = i; k < n; k++) if (s[k] == '#') { hash_at = k; break; }
    for (size_t k = i; k < hash_at; k++) if (s[k] == '?') { query_at = k; break; }
    size_t path_end = query_at;
    bool has_auth = (i + 1 < n && (s[i] == '/' || (special && s[i] == '\\')) && (s[i + 1] == '/' || (special && s[i + 1] == '\\')));
    if (has_auth) {
        u->authority = true;
        i += 2;
        size_t a = i;
        while (i < path_end && s[i] != '/' && !(special && s[i] == '\\')) i++;
        /* authority s[a..i): userinfo (kept for href), host, port */
        size_t at = a;
        for (size_t k = a; k < i; k++) if (s[k] == '@') at = k + 1;
        if (at > a) tv_url_push_encoded(&u->userinfo, s + a, at - 1 - a, false);
        size_t colon = i;
        if (at < i && s[at] == '[') {
            size_t rb = at;
            while (rb < i && s[rb] != ']') rb++;
            for (size_t k = rb; k < i; k++) if (s[k] == ':') { colon = k; break; }
        } else {
            for (size_t k = at; k < i; k++) if (s[k] == ':') { colon = k; break; }
        }
        for (size_t k = at; k < colon; k++) tv_sb_push_char(&u->host, (char)(s[k] >= 'A' && s[k] <= 'Z' ? s[k] + 32 : s[k]));
        if (special && u->host.len == 0) return false;
        if (colon < i) {
            size_t ps = colon + 1;
            for (size_t k = ps; k < i; k++) if (s[k] < '0' || s[k] > '9') return false;
            while (ps + 1 < i && s[ps] == '0') ps++; /* "080" → "80" */
            if (ps < i) {
                long port = strtol(s + ps, NULL, 10);
                if (port > 65535) return false;
                const char *def = tv_url_default_port(u->proto.data, sl);
                if (!def || (size_t)(i - ps) != strlen(def) || memcmp(s + ps, def, strlen(def)) != 0) tv_sb_push(&u->port, s + ps, i - ps);
            }
        }
    }
    if (special) {
        tv_url_normalize_path(&u->path, s + i, path_end - i);
    } else {
        tv_url_push_encoded(&u->path, s + i, path_end - i, false);
    }
    u->has_query = query_at < hash_at;
    u->has_hash = hash_at < n;
    if (query_at < hash_at && hash_at - query_at > 1) tv_url_push_encoded(&u->search, s + query_at, hash_at - query_at, true);
    if (hash_at < n && n - hash_at > 1) tv_url_push_encoded(&u->hash, s + hash_at, n - hash_at, false);
    return true;
}

/* The common case — `http(s)://host[:port]/path[?query][#hash]` with nothing to encode or
 * normalize: its part boundaries (no allocation). False: take the full parser. */
typedef struct tv_url_bounds { size_t scheme, host, host_end, port, port_end, path, query, hash; bool default_port; } tv_url_bounds;

static bool tv_url_scan(const char *s, size_t n, tv_url_bounds *b) {
    size_t sl = n >= 7 && memcmp(s, "http://", 7) == 0 ? 4 : n >= 8 && memcmp(s, "https://", 8) == 0 ? 5 : 0;
    if (!sl) return false;
    size_t h = sl + 3, i = h, colon = 0;
    for (; i < n && s[i] != '/' && s[i] != '?' && s[i] != '#'; i++) {
        char c = s[i];
        if (c == ':') colon = i;
        else if (c == '@' || c == '[' || c == '\\' || (c >= 'A' && c <= 'Z') || (unsigned char)c <= ' ' || (unsigned char)c >= 0x7f) return false;
    }
    size_t host_end = colon ? colon : i;
    if (host_end == h) return false;
    size_t port_at = colon ? colon + 1 : i;
    for (size_t k = port_at; k < i; k++) if (s[k] < '0' || s[k] > '9') return false;
    if (colon && (i - port_at == 0 || s[port_at] == '0' || i - port_at > 5)) return false;
    size_t path_at = i, q = n, hash = n;
    if (path_at == n || s[path_at] != '/') return false; /* "http://host" → path "/" */
    for (size_t k = path_at; k < n; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c <= ' ' || c >= 0x7f || c == '"' || c == '<' || c == '>' || c == '`' || c == '{' || c == '}' || c == '\\' || c == '\'') return false;
        if (c == '#') { hash = k; break; }
        if (c == '?' && q == n) q = k;
        if (c == '.' && q == n && s[k - 1] == '/') return false; /* dot segments: normalize */
        if (c == '%' && q == n) return false;                    /* %2e etc. */
    }
    b->scheme = sl;
    b->host = h;
    b->host_end = host_end;
    b->port = port_at;
    b->port_end = i;
    b->path = path_at;
    b->query = q < hash ? q : hash;
    b->hash = hash;
    b->default_port = colon && ((sl == 4 && i - port_at == 2 && memcmp(s + port_at, "80", 2) == 0) || (sl == 5 && i - port_at == 3 && memcmp(s + port_at, "443", 3) == 0));
    return true;
}

static bool tv_url_fast(const char *s, size_t n, tv_arr *out) {
    tv_url_bounds b;
    if (!tv_url_scan(s, n, &b)) return false;
    tv_str parts[6];
    parts[0] = tv_str_from(s, b.scheme + 1);
    parts[1] = tv_str_from(s + b.host, b.host_end - b.host);
    parts[2] = b.port < b.port_end && !b.default_port ? tv_str_from(s + b.port, b.port_end - b.port) : TV_EMPTY_STR;
    parts[3] = tv_str_from(s + b.path, b.query - b.path);
    parts[4] = b.hash - b.query > 1 ? tv_str_from(s + b.query, b.hash - b.query) : TV_EMPTY_STR;
    parts[5] = n - b.hash > 1 ? tv_str_from(s + b.hash, n - b.hash) : TV_EMPTY_STR;
    *out = tv_arr_with_capacity(&tv_type_str, 6);
    for (int k = 0; k < 6; k++) tv_arr_push(out, &tv_type_str, &parts[k]);
    return true;
}

/* Parses `input` (resolved against `base` when relative) into u; false if it isn't a URL. */
static bool tv_url_parse_full(tv_str input, tv_str base, tv_url_parts *u) {
    const char *s = input.p->data;
    size_t n = (size_t)input.p->len;
    while (n && (unsigned char)*s <= ' ') { s++; n--; }
    while (n && (unsigned char)s[n - 1] <= ' ') n--;
    memset(u, 0, sizeof *u);
    bool ok = tv_url_parse_abs(s, n, u);
    if (!ok && base.p->len) {
        tv_url_free(u);
        memset(u, 0, sizeof *u);
        tv_url_parts b = {0};
        if (tv_url_parse_abs(base.p->data, (size_t)base.p->len, &b)) {
            tv_sb abs = {0};
            tv_sb_push(&abs, b.proto.data, (size_t)b.proto.len);
            if (n >= 2 && s[0] == '/' && s[1] == '/') {
                tv_sb_push(&abs, s, n);
            } else {
                tv_sb_push(&abs, "//", 2);
                if (b.userinfo.len) { tv_sb_push(&abs, b.userinfo.data, (size_t)b.userinfo.len); tv_sb_push_char(&abs, '@'); }
                tv_sb_push(&abs, b.host.data, (size_t)b.host.len);
                if (b.port.len) { tv_sb_push_char(&abs, ':'); tv_sb_push(&abs, b.port.data, (size_t)b.port.len); }
                if (n && s[0] == '/') {
                    tv_sb_push(&abs, s, n);
                } else if (n && s[0] == '?') {
                    tv_sb_push(&abs, b.path.data, (size_t)b.path.len);
                    tv_sb_push(&abs, s, n);
                } else if (n && s[0] == '#') {
                    tv_sb_push(&abs, b.path.data, (size_t)b.path.len);
                    if (b.has_query) tv_sb_push_char(&abs, '?');
                    if (b.search.len > 1) tv_sb_push(&abs, b.search.data + 1, (size_t)b.search.len - 1);
                    tv_sb_push(&abs, s, n);
                } else if (n == 0) {
                    tv_sb_push(&abs, b.path.data, (size_t)b.path.len);
                    if (b.has_query) tv_sb_push_char(&abs, '?');
                    if (b.search.len > 1) tv_sb_push(&abs, b.search.data + 1, (size_t)b.search.len - 1);
                } else {
                    size_t dir = (size_t)b.path.len;
                    while (dir && b.path.data[dir - 1] != '/') dir--;
                    tv_sb_push(&abs, b.path.data, dir);
                    tv_sb_push(&abs, s, n);
                }
            }
            ok = tv_url_parse_abs(abs.data, (size_t)abs.len, u);
            tv_sb_free(&abs);
        }
        tv_url_free(&b);
    }
    if (!ok) tv_url_free(u);
    return ok;
}

tv_arr tv_native_urlParse(tv_str input, tv_str base) {
    tv_arr fast;
    if (tv_url_fast(input.p->data, (size_t)input.p->len, &fast)) return fast;
    tv_url_parts u;
    if (!tv_url_parse_full(input, base, &u)) return tv_arr_fresh();
    tv_arr out = tv_arr_with_capacity(&tv_type_str, 6);
    tv_sb *parts[6] = { &u.proto, &u.host, &u.port, &u.path, &u.search, &u.hash };
    for (int k = 0; k < 6; k++) {
        tv_str v = tv_str_from_sb(parts[k]);
        tv_arr_push(&out, &tv_type_str, &v);
    }
    tv_url_free(&u);
    return out;
}

tv_str tv_native_urlDecode(tv_str s, bool plus) {
    const char *p = s.p->data;
    size_t n = (size_t)s.p->len;
    bool any = false;
    for (size_t i = 0; i < n; i++) if (p[i] == '%' || (plus && p[i] == '+')) { any = true; break; }
    if (!any) { tv_str_retain(s); return s; }
    tv_sb sb = {0};
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '+' && plus) { tv_sb_push_char(&sb, ' '); continue; }
        if (c == '%' && i + 2 < n) {
            int h, l;
            char a = p[i + 1], b = p[i + 2];
            h = a >= '0' && a <= '9' ? a - '0' : (a | 32) >= 'a' && (a | 32) <= 'f' ? (a | 32) - 'a' + 10 : -1;
            l = b >= '0' && b <= '9' ? b - '0' : (b | 32) >= 'a' && (b | 32) <= 'f' ? (b | 32) - 'a' + 10 : -1;
            if (h >= 0 && l >= 0) { tv_sb_push_char(&sb, (char)(h * 16 + l)); i += 2; continue; }
        }
        tv_sb_push_char(&sb, c);
    }
    return tv_str_from_sb(&sb);
}

tv_str tv_native_urlEncode(tv_str s) {
    static const char hex[] = "0123456789ABCDEF";
    tv_sb sb = {0};
    for (int64_t i = 0; i < s.p->len; i++) {
        unsigned char c = (unsigned char)s.p->data[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_') tv_sb_push_char(&sb, (char)c);
        else if (c == ' ') tv_sb_push_char(&sb, '+');
        else { char e[3] = { '%', hex[c >> 4], hex[c & 15] }; tv_sb_push(&sb, e, 3); }
    }
    return tv_str_from_sb(&sb);
}

/* Normalized href of a URL ("" if it isn't one). An already-normal URL is returned as is. */
tv_str tv_native_urlNormalize(tv_str input, tv_str base) {
    const char *s = input.p->data;
    size_t n = (size_t)input.p->len;
    size_t lead = 0;
    while (lead < n && (unsigned char)s[lead] <= ' ') lead++;
    size_t trail = n;
    while (trail > lead && (unsigned char)s[trail - 1] <= ' ') trail--;
    tv_url_bounds b;
    /* already normal: no default port, no empty "?" or "#" */
    if (lead == 0 && trail == n && tv_url_scan(s, n, &b) && !b.default_port && b.hash - b.query != 1 && n - b.hash != 1) {
        tv_str_retain(input);
        return input;
    }
    tv_url_parts u;
    if (!tv_url_parse_full(input, base, &u)) return TV_EMPTY_STR;
    tv_sb sb = {0};
    tv_sb_push(&sb, u.proto.data, (size_t)u.proto.len);
    if (u.authority) {
        tv_sb_push(&sb, "//", 2);
        if (u.userinfo.len) { tv_sb_push(&sb, u.userinfo.data, (size_t)u.userinfo.len); tv_sb_push_char(&sb, '@'); }
        tv_sb_push(&sb, u.host.data, (size_t)u.host.len);
        if (u.port.len) { tv_sb_push_char(&sb, ':'); tv_sb_push(&sb, u.port.data, (size_t)u.port.len); }
    }
    tv_sb_push(&sb, u.path.data, (size_t)u.path.len);
    if (u.has_query) tv_sb_push_char(&sb, '?');
    if (u.search.len > 1) tv_sb_push(&sb, u.search.data + 1, (size_t)u.search.len - 1);
    if (u.has_hash) tv_sb_push_char(&sb, '#');
    if (u.hash.len > 1) tv_sb_push(&sb, u.hash.data + 1, (size_t)u.hash.len - 1);
    tv_url_free(&u);
    return tv_str_from_sb(&sb);
}

/* Part k of a normalized href: 0 protocol ("http:"), 1 hostname, 2 port, 3 pathname,
 * 4 search ("?q" or ""), 5 hash ("#h" or ""). */
tv_str tv_native_urlPart(tv_str href, tv_int k) {
    const char *s = href.p->data, *end = s + href.p->len;
    const char *colon = memchr(s, ':', (size_t)href.p->len);
    if (!colon) return TV_EMPTY_STR;
    if (k == 0) return tv_str_from(s, (size_t)(colon + 1 - s));
    const char *p = colon + 1, *host = p, *host_end = p, *port = p, *port_end = p;
    if (end - p >= 2 && p[0] == '/' && p[1] == '/') {
        host = p + 2;
        const char *a = host;
        while (a < end && *a != '/' && *a != '?' && *a != '#') a++;
        for (const char *x = host; x < a; x++) if (*x == '@') host = x + 1; /* userinfo */
        const char *c = host;
        if (c < a && *c == '[') { while (c < a && *c != ']') c++; }
        while (c < a && *c != ':') c++;
        host_end = c;
        port = c < a ? c + 1 : a;
        port_end = a;
        p = a;
    }
    if (k == 1) return tv_str_from(host, (size_t)(host_end - host));
    if (k == 2) return tv_str_from(port, (size_t)(port_end - port));
    const char *hash = memchr(p, '#', (size_t)(end - p));
    if (!hash) hash = end;
    const char *q = memchr(p, '?', (size_t)(hash - p));
    if (!q) q = hash;
    if (k == 3) return tv_str_from(p, (size_t)(q - p));
    if (k == 4) return hash - q > 1 ? tv_str_from(q, (size_t)(hash - q)) : TV_EMPTY_STR;
    return end - hash > 1 ? tv_str_from(hash, (size_t)(end - hash)) : TV_EMPTY_STR;
}

/* ================================================================== HTTP client (fetch) */

/* fetch() runs natively on the event loop: DNS lookups on a small thread pool (cached), pooled
 * keep-alive connections per origin, a request writer and an incremental HTTP/1.1 response
 * parser, redirects, and gzip/deflate decoding. The standard library (std/http) wraps it:
 * fetchStart begins a request and returns its id, fetchWait gives a Promise<int> that settles
 * once the whole response has arrived (0) or the request failed (< 0), and the other natives
 * read the result. Nothing here is referenced unless a program calls fetch. */

#include <limits.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/uio.h>
#include <sys/un.h>
#ifndef TV_KQUEUE
#include <sys/eventfd.h>
#endif

/* ---- DNS: getaddrinfo on a few lazily started threads (64 KiB stacks), results cached */

#define TV_DNS_MAX_ADDRS 8
#define TV_DNS_TTL_MS 30000
typedef struct tv_addrs {
    int n;
    struct sockaddr_storage a[TV_DNS_MAX_ADDRS];
    socklen_t len[TV_DNS_MAX_ADDRS];
} tv_addrs;

struct tv_fr;
typedef struct tv_dns {
    struct tv_dns *next;       /* cache chain */
    struct tv_dns *job_next;   /* job or done queue */
    char *host;
    bool resolving;
    int err;                   /* getaddrinfo's error (0: found) */
    tv_addrs addrs;
    uint64_t expires;
    struct tv_fr *waiters;     /* requests waiting for this lookup */
    void *net_waiters;         /* sockets waiting for it (runtime/net.c) */
} tv_dns;
static void (*tv_dns_net_done)(tv_dns *d);

#define TV_DNS_BUCKETS 64
static tv_dns *tv_dns_cache[TV_DNS_BUCKETS];
static pthread_mutex_t tv_dns_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tv_dns_cv = PTHREAD_COND_INITIALIZER;
static tv_dns *tv_dns_jobs, *tv_dns_done;
static int tv_dns_threads, tv_dns_idle;
static int tv_dns_pipe[2] = {-1, -1};

static uint32_t tv_hash_cstr(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void tv_addrs_from(tv_addrs *out, struct addrinfo *ai) {
    out->n = 0;
    /* IPv4 first: a server bound to 0.0.0.0 doesn't answer on ::1 */
    for (int pass = 0; pass < 2; pass++)
        for (struct addrinfo *a = ai; a && out->n < TV_DNS_MAX_ADDRS; a = a->ai_next) {
            if ((a->ai_family == AF_INET) != (pass == 0)) continue;
            if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
            memcpy(&out->a[out->n], a->ai_addr, a->ai_addrlen);
            out->len[out->n++] = a->ai_addrlen;
        }
}

static void *tv_dns_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&tv_dns_mu);
    for (;;) {
        while (!tv_dns_jobs) {
            tv_dns_idle++;
            pthread_cond_wait(&tv_dns_cv, &tv_dns_mu);
            tv_dns_idle--;
        }
        tv_dns *d = tv_dns_jobs;
        tv_dns_jobs = d->job_next;
        pthread_mutex_unlock(&tv_dns_mu);
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_ADDRCONFIG;
        const char *h = d->host;
        char bare[256];
        size_t hl = strlen(h);
        if (hl >= 2 && h[0] == '[' && h[hl - 1] == ']' && hl - 2 < sizeof bare) { memcpy(bare, h + 1, hl - 2); bare[hl - 2] = 0; h = bare; }
        int err = getaddrinfo(h, NULL, &hints, &res);
        if (!err) {
            tv_addrs_from(&d->addrs, res);
            if (d->addrs.n == 0) err = EAI_NONAME;
            freeaddrinfo(res);
        }
        d->err = err;
        pthread_mutex_lock(&tv_dns_mu);
        d->job_next = tv_dns_done;
        tv_dns_done = d;
        pthread_mutex_unlock(&tv_dns_mu);
        char one = 1;
        ssize_t w = write(tv_dns_pipe[1], &one, 1);
        (void)w;
        pthread_mutex_lock(&tv_dns_mu);
    }
    return NULL;
}

static void tv_dns_ready(tv_io *h, bool readable, bool writable, bool broken);
/* `ready` is set when the first lookup starts: a statically initialized code pointer here would
 * land in clang's merged globals next to the event loop's, and keep all of this linked. */
static tv_io tv_dns_io;

void tv_io_add(int fd, tv_io *h, bool read, bool write) {
    int q = tv_loop_queue();
    void *tag = (void *)((uintptr_t)h | 1);
#ifdef TV_KQUEUE
    struct kevent ev[2];
    int n = 0;
    if (read) EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD, 0, 0, tag);
    if (write) EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, tag);
    kevent(q, ev, n, NULL, 0, NULL);
#else
    struct epoll_event ev = { .events = (read ? EPOLLIN | EPOLLRDHUP : 0) | (write ? EPOLLOUT : 0), .data.ptr = tag };
    epoll_ctl(q, EPOLL_CTL_ADD, fd, &ev);
#endif
}

void tv_io_set(int fd, tv_io *h, bool read, bool write) {
    int q = tv_loop_queue();
    void *tag = (void *)((uintptr_t)h | 1);
#ifdef TV_KQUEUE
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_ADD | (read ? EV_ENABLE : EV_DISABLE), 0, 0, tag);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_ADD | (write ? EV_ENABLE : EV_DISABLE), 0, 0, tag);
    kevent(q, ev, 2, NULL, 0, NULL);
#else
    struct epoll_event ev = { .events = (read ? EPOLLIN | EPOLLRDHUP : 0) | (write ? EPOLLOUT : 0), .data.ptr = tag };
    if (epoll_ctl(q, EPOLL_CTL_MOD, fd, &ev) != 0 && errno == ENOENT) epoll_ctl(q, EPOLL_CTL_ADD, fd, &ev);
#endif
}

#ifndef TV_KQUEUE
#include <sys/syscall.h>
#endif

bool tv_io_proc(int pid, tv_io *h, int *fd_out) {
    int q = tv_loop_queue();
    *fd_out = -1;
    void *tag = (void *)((uintptr_t)h | 1);
#ifdef TV_KQUEUE
    struct kevent ev;
    EV_SET(&ev, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, tag);
    return kevent(q, &ev, 1, NULL, 0, NULL) == 0;
#else
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) return false;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    *fd_out = fd;
    struct epoll_event ev = { .events = EPOLLIN | EPOLLONESHOT, .data.ptr = tag };
    return epoll_ctl(q, EPOLL_CTL_ADD, fd, &ev) == 0;
#endif
}

static void tv_dns_start_thread(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024 > PTHREAD_STACK_MIN ? 64 * 1024 : PTHREAD_STACK_MIN);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &attr, tv_dns_worker, NULL) == 0) tv_dns_threads++;
    pthread_attr_destroy(&attr);
}

static void tv_dns_queue(tv_dns *d) {
    if (tv_dns_pipe[0] < 0) {
        tv_dns_io.ready = tv_dns_ready;
        if (pipe(tv_dns_pipe) != 0) tv_trap("can't create a pipe for DNS lookups", NULL);
        for (int i = 0; i < 2; i++) {
            fcntl(tv_dns_pipe[i], F_SETFL, fcntl(tv_dns_pipe[i], F_GETFL) | O_NONBLOCK);
            fcntl(tv_dns_pipe[i], F_SETFD, FD_CLOEXEC);
        }
        tv_io_add(tv_dns_pipe[0], &tv_dns_io, true, false);
    }
    tv_io_refs++;
    pthread_mutex_lock(&tv_dns_mu);
    d->job_next = tv_dns_jobs;
    tv_dns_jobs = d;
    if (tv_dns_idle == 0 && tv_dns_threads < 4) tv_dns_start_thread();
    else pthread_cond_signal(&tv_dns_cv);
    pthread_mutex_unlock(&tv_dns_mu);
}

/* ---- UTF-8 validation */

/* Validates UTF-8 from *pos up to n: false at the first invalid sequence; otherwise *pos stops at
 * the end, or at a sequence cut off by the end (more bytes may complete it). ASCII runs go 16
 * bytes at a time. */
static bool tv_utf8_scan(const uint8_t *p, size_t n, size_t *pos) {
    size_t i = *pos;
    while (i < n) {
        /* (ASCII 64 bytes at a time, one test for all of them: a body of text is mostly that) */
        while (n - i >= 64) {
            uint64_t w[8], acc = 0;
            memcpy(w, p + i, 64);
            for (int k = 0; k < 8; k++) acc |= w[k];
            if (acc & 0x8080808080808080ull) break;
            i += 64;
        }
        if (i == n) break;
        if (n - i >= 16) {
            uint64_t a, b;
            memcpy(&a, p + i, 8);
            memcpy(&b, p + i + 8, 8);
            if (!((a | b) & 0x8080808080808080ull)) { i += 16; continue; }
        }
        uint8_t c = p[i];
        if (c < 0x80) { i++; continue; }
        size_t need = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
        if (!need) { *pos = i; return false; }
        if (n - i <= need) break;
        uint8_t lo = c == 0xE0 ? 0xA0 : c == 0xF0 ? 0x90 : 0x80, hi = c == 0xED ? 0x9F : c == 0xF4 ? 0x8F : 0xBF;
        if (p[i + 1] < lo || p[i + 1] > hi) { *pos = i; return false; }
        for (size_t k = 2; k <= need; k++)
            if ((p[i + k] & 0xC0) != 0x80) { *pos = i; return false; }
        i += need + 1;
    }
    *pos = i;
    return true;
}

/* ---- requests and connections */

typedef struct tv_fc tv_fc;
typedef struct tv_origin {
    struct tv_origin *next;
    char *key;                 /* "host:port" */
    tv_fc *idle;               /* idle connections, most recently used first */
    int nidle;
    int sparing;               /* spare connections being opened (tv_fc_spare) */
} tv_origin;

enum { TV_FR_HEAD, TV_FR_FIXED, TV_FR_CHUNK_SIZE, TV_FR_CHUNK_DATA, TV_FR_CHUNK_END, TV_FR_TRAILERS, TV_FR_UNTIL_CLOSE, TV_FR_DONE };
enum { TV_FETCH_FOLLOW, TV_FETCH_MANUAL, TV_FETCH_ERROR };
enum { TV_FETCH_DECOMPRESS = 1, TV_FETCH_INSECURE = 2 };

/* Set by tv_tls_install() in programs linked with TLS (runtime/tls.c, runtime/codecs.c). */
const tv_tls_ops *tv_tls_impl;
const tv_codec_ops *tv_codec;
const tv_crypto_ops *tv_crypto;
const tv_zs_ops *tv_zs;

typedef struct tv_fr {
    tv_int id;
    int result;                /* 1 running, 0 the whole response is in, < 0 failed */
    int head_state;            /* 1 waiting for the response head, 0 it arrived, < 0 failed first */
    tv_promise *done;          /* fetchWait: the head arrived (0) or the request failed */
    tv_promise *body_p;        /* fetchBodyWait: the body is in (0) or failed */
    tv_promise *read_p;        /* fetchRead: body bytes to take (> 0), the end (0), or failed */
    bool streaming;            /* the body is read in chunks (res.body) */
    bool paused;               /* ... and the reader is behind: the socket isn't read */
    int shares;                /* Response.clone()s that will take the body too */
    struct tv_fr *dns_next;    /* waiting on a lookup */
    /* the request */
    tv_str method, headers, body, url;
    int redirect_mode, redirects;
    bool decompress;
    bool https, insecure;      /* TLS; without certificate checks (tls.rejectUnauthorized: false) */
    tv_str ca;                 /* extra trusted certificates (PEM), for tls.ca */
    tv_str unix_path;          /* Bun's `unix`: connect to this socket instead ("" for TCP) */
    tv_str proxy;              /* the `proxy` option ("": HTTP_PROXY/HTTPS_PROXY, unless NO_PROXY) */
    /* where to connect: the host itself, or the proxy (then `via_proxy`) */
    char *dial_host;
    int dial_port;
    bool via_proxy;
    char *proxy_auth;          /* "Basic ..." for the proxy, or NULL */
    char *host;                /* hostname (IPv6 in brackets) */
    char *key;                 /* the pool key: scheme, host, port and TLS options */
    int port;
    tv_sb head;                /* the serialized request head */
    bool merged;               /* the body was appended to head (one TLS record for both) */
    size_t sent;               /* bytes of head + body written */
    tv_fc *c;
    bool retried;
    /* the response */
    int state, status;
    bool keep, discard, head_only, got_any;
    int enc;                   /* 1 gzip, 2 deflate, 3 br, 4 zstd */
    /* An encoded body (unless decompress: false) arrives in raw and is decoded onto rbody as it
     * does; a reader that's behind leaves some in raw (from raw_off), undecoded. */
    tv_decoder *dec;
    tv_sb raw;
    size_t raw_off;
    /* A body read in chunks (streaming) is decoded as it arrives, at most TV_STREAM_HIGH ahead of
     * the reader: a small body that decodes to a huge one is decoded as it's read. A body read
     * whole is decoded at once when it's all in (faster). Until the program reads it, nothing
     * is decoded; a body that has all arrived by then is draining (the request settles once
     * it's decoded). */
    bool whole, draining;
    int64_t remaining;
    struct tv_bk *bulk;        /* the rest of a big body being received on a thread (tv_bk_start) */
    /* The body is checked as UTF-8 while it arrives (still in cache): text() then needn't read
     * it again. A second pass over a big body right after it arrived also slows the next
     * transfer (it evicts what the kernel's copy needs). */
    size_t utf8_pos;
    bool utf8_bad;
    tv_sb status_text, rheaders, rbody, location;
    bool redirected;
    const char *code;          /* failure: Bun's error code (points into code_buf) */
    char code_buf[64];
    tv_sb message;
} tv_fr;

struct tv_fc {
    tv_io io;
    int fd;
    bool reading, writing, registered;
    bool connecting, reused, dead;
    tv_origin *origin;
    tv_fc *next;               /* idle list, or the list of closed handles */
    tv_fr *r;                  /* the request in progress (NULL when idle) */
    tv_addrs addrs;            /* where to connect */
    int ai;                    /* the address being tried */
    int err;                   /* the last connect error */
    int port;
    tv_tls *tls;               /* https: the TLS session (NULL for http) */
    int early_err;             /* the ClientHello, sent while connecting, failed with this */
    bool handshaking;
    bool tunneling;            /* waiting for a proxy's answer to CONNECT */
    bool no_reuse;             /* a proxy refused the tunnel: its connection isn't ours to keep */
    tv_sb ctl;                 /* the CONNECT request, while it's being sent */
    size_t ctl_off;
    bool dialing;              /* a spare the socket helper is opening (tv_fc_spare) */
    bool bulk;                 /* a thread is receiving a body on it (tv_bk_start) */
    bool spare;                /* ... and, still connecting, one waiting to join the pool */
    int dial_rc, dial_err;     /* ... connect's result */
    struct tv_fc *dial_next;
    char *in;
    size_t in_off, in_len, in_cap;
};

#define TV_ORIGIN_BUCKETS 64
static tv_origin *tv_origins[TV_ORIGIN_BUCKETS];
static tv_fc *tv_fc_graveyard;
/* Requests by id: slot + 1 in the low 32 bits, a generation above (stale ids find nothing). */
static tv_fr **tv_fr_table;
static tv_int *tv_fr_free;          /* free slots */
static tv_int tv_fr_cap, tv_fr_nfree;
static uint32_t tv_fr_gen;

static void tv_fr_step(tv_fr *r);
static void tv_fc_ready(tv_io *h, bool readable, bool writable, bool broken);

/* A response's read buffer (64 KB): the last few freed are kept for the next responses, as the
 * system's allocator may hand a freed one's pages back and fault them in again for the next
 * (with MallocSpaceEfficient set, ~15% of a small fetch's CPU); freed once the loop idles. */
#define TV_FC_RBUF ((size_t)64 << 10)
#define TV_FC_RBUFS 8
static __thread char *tv_fc_rbufs[TV_FC_RBUFS];
static __thread int tv_fc_nrbufs;

static char *tv_fc_rbuf(void) {
    return tv_fc_nrbufs ? tv_fc_rbufs[--tv_fc_nrbufs] : tv_alloc(TV_FC_RBUF);
}

static void tv_fc_rbuf_free(char *p, size_t cap) {
    if (p && cap == TV_FC_RBUF && tv_fc_nrbufs < TV_FC_RBUFS) tv_fc_rbufs[tv_fc_nrbufs++] = p;
    else tv_free(p);
}

static void tv_fc_rbufs_flush(void) {
    while (tv_fc_nrbufs) tv_free(tv_fc_rbufs[--tv_fc_nrbufs]);
}

/* ---- the socket helper

   A JavaScript host (runtime/node.c) sets tv_fetch_offload: its thread is busy running
   JavaScript, so a thread of its own closes finished connections (close() frees what the socket
   still held: ~10 us for a body read partway, as media readers leave theirs) and opens spare ones
   (tv_fc_spare). The loop doesn't wait for either. */
bool tv_fetch_offload;
static pthread_mutex_t tv_sk_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tv_sk_cv = PTHREAD_COND_INITIALIZER;
static bool tv_sk_started, tv_sk_idle, tv_sk_poked;
static int *tv_sk_closes;                    /* sockets to close */
static size_t tv_sk_nclose, tv_sk_close_cap;
static tv_fc *tv_sk_dials, *tv_sk_dialed;    /* spares to open, and opened (or not) */
static tv_io tv_sk_io;                       /* the loop's: woken when spares are open */
static int tv_sk_wake_q = -1;
#ifndef TV_KQUEUE
static int tv_sk_efd = -1;
#endif

static void tv_sk_wake_loop(void) {
#ifdef TV_KQUEUE
    struct kevent ev;
    /* (a trigger sets the event's udata too) */
    EV_SET(&ev, (uintptr_t)&tv_sk_io, EVFILT_USER, 0, NOTE_TRIGGER, 0, (void *)((uintptr_t)&tv_sk_io | 1));
    kevent(tv_sk_wake_q, &ev, 1, NULL, 0, NULL);
#else
    uint64_t one = 1;
    ssize_t w = write(tv_sk_efd, &one, sizeof one);
    (void)w;
#endif
}

/* (on the helper: connects c to its address; the loop goes on in tv_sk_ready) */
static void tv_sk_dial_now(tv_fc *c) {
    struct sockaddr_storage sa = c->addrs.a[c->ai];
    if (sa.ss_family == AF_INET) ((struct sockaddr_in *)&sa)->sin_port = htons((uint16_t)c->port);
    else if (sa.ss_family == AF_INET6) ((struct sockaddr_in6 *)&sa)->sin6_port = htons((uint16_t)c->port);
    int fd = socket(sa.ss_family, SOCK_STREAM, 0);
    c->dial_rc = -1;
    if (fd < 0) { c->dial_err = errno; c->fd = -1; return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    c->dial_rc = connect(fd, (struct sockaddr *)&sa, c->addrs.len[c->ai]);
    c->dial_err = c->dial_rc == 0 ? 0 : errno;
    c->fd = fd;
}

static void *tv_sk_run(void *arg) {
    (void)arg;
    pthread_mutex_lock(&tv_sk_mu);
    for (;;) {
        while (!tv_sk_nclose && !tv_sk_dials) {
            tv_sk_idle = true;
            pthread_cond_wait(&tv_sk_cv, &tv_sk_mu);
            tv_sk_idle = false;
        }
        tv_fc *dials = tv_sk_dials;
        tv_sk_dials = NULL;
        int closes[64];
        size_t nc = tv_sk_nclose < 64 ? tv_sk_nclose : 64;
        tv_sk_nclose -= nc;
        memcpy(closes, tv_sk_closes + tv_sk_nclose, nc * sizeof *closes);
        pthread_mutex_unlock(&tv_sk_mu);
        /* (spares first: a request may be about to want one) */
        tv_fc *done = NULL, *last = NULL;
        for (tv_fc *c = dials, *next; c; c = next) {
            next = c->dial_next;
            tv_sk_dial_now(c);
            c->dial_next = NULL;
            if (last) last->dial_next = c;
            else done = c;
            last = c;
        }
        bool poke = false;
        if (done) {
            pthread_mutex_lock(&tv_sk_mu);
            last->dial_next = tv_sk_dialed;
            tv_sk_dialed = done;
            poke = !tv_sk_poked;
            tv_sk_poked = true;
            pthread_mutex_unlock(&tv_sk_mu);
        }
        if (poke) tv_sk_wake_loop();
        for (size_t i = 0; i < nc; i++) close(closes[i]);
        pthread_mutex_lock(&tv_sk_mu);
    }
    return NULL;
}

static void tv_sk_ready(tv_io *h, bool readable, bool writable, bool broken);

static bool tv_sk_start(void) {
    if (tv_sk_started) return true;
    tv_sk_io.ready = tv_sk_ready;
    tv_sk_wake_q = tv_loop_queue();
#ifdef TV_KQUEUE
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)&tv_sk_io, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, (void *)((uintptr_t)&tv_sk_io | 1));
    if (kevent(tv_sk_wake_q, &ev, 1, NULL, 0, NULL) != 0) return false;
#else
    tv_sk_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (tv_sk_efd < 0) return false;
    tv_io_add(tv_sk_efd, &tv_sk_io, true, false);
#endif
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024 > PTHREAD_STACK_MIN ? 64 * 1024 : PTHREAD_STACK_MIN);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    tv_sk_started = pthread_create(&t, &attr, tv_sk_run, NULL) == 0;
    pthread_attr_destroy(&attr);
    return tv_sk_started;
}

static void tv_sk_wake(void) {
    bool wake = tv_sk_idle;
    pthread_mutex_unlock(&tv_sk_mu);
    if (wake) pthread_cond_signal(&tv_sk_cv);
}

/* Closes a connection's socket on the helper (it leaves the loop's queue now). */
static bool tv_sk_close(tv_fc *c) {
    if (!tv_fetch_offload || !tv_sk_start()) return false;
    if (c->registered) {
#ifdef TV_KQUEUE
        struct kevent ev[2];
        int n = 0;
        EV_SET(&ev[n++], c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        if (c->writing) EV_SET(&ev[n++], c->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(tv_loop_queue(), ev, n, NULL, 0, NULL);
#else
        epoll_ctl(tv_loop_queue(), EPOLL_CTL_DEL, c->fd, NULL);
#endif
    }
    pthread_mutex_lock(&tv_sk_mu);
    if (tv_sk_nclose == tv_sk_close_cap) {
        tv_sk_close_cap = tv_sk_close_cap ? tv_sk_close_cap * 2 : 64;
        tv_sk_closes = tv_realloc(tv_sk_closes, tv_sk_close_cap * sizeof *tv_sk_closes);
    }
    tv_sk_closes[tv_sk_nclose++] = c->fd;
    tv_sk_wake();
    return true;
}

static void tv_fc_reap(void) {
    while (tv_fc_graveyard) {
        tv_fc *c = tv_fc_graveyard;
        tv_fc_graveyard = c->next;
        tv_sb_free(&c->ctl);
        tv_fc_rbuf_free(c->in, c->in_cap);
        tv_free(c);
    }
}

static void tv_fc_interest(tv_fc *c, bool read, bool write) {
    if (c->registered && read == c->reading && write == c->writing) return;
    int q = tv_loop_queue();
    void *tag = (void *)((uintptr_t)&c->io | 1);
#ifdef TV_KQUEUE
    struct kevent ev[2];
    int n = 0;
    if (!c->registered || read != c->reading) EV_SET(&ev[n++], c->fd, EVFILT_READ, !c->registered ? (read ? EV_ADD : EV_ADD | EV_DISABLE) : read ? EV_ENABLE : EV_DISABLE, 0, 0, tag);
    if (write != c->writing) EV_SET(&ev[n++], c->fd, EVFILT_WRITE, write ? EV_ADD : EV_DELETE, 0, 0, tag);
    if (n) kevent(q, ev, n, NULL, 0, NULL);
#else
    struct epoll_event ev = { .events = (read ? EPOLLIN | EPOLLRDHUP : 0) | (write ? EPOLLOUT : 0), .data.ptr = tag };
    epoll_ctl(q, c->registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, c->fd, &ev);
#endif
    c->registered = true;
    c->reading = read;
    c->writing = write;
}

static void tv_fc_close(tv_fc *c) {
    if (c->dead) return;
    c->dead = true;
    if (c->dialing) return; /* (the helper has it: tv_sk_ready frees it) */
    if (c->tls) { tv_tls_impl->close(c->tls, false); c->tls = NULL; }
    if (c->fd >= 0 && !tv_sk_close(c)) close(c->fd); /* also leaves the event queue */
    c->fd = -1;
    c->next = tv_fc_graveyard;
    tv_fc_graveyard = c;
}

static tv_origin *tv_origin_get(const char *key) {
    uint32_t b = tv_hash_cstr(key) % TV_ORIGIN_BUCKETS;
    for (tv_origin *o = tv_origins[b]; o; o = o->next)
        if (strcmp(o->key, key) == 0) return o;
    tv_origin *o = tv_alloc(sizeof *o);
    memset(o, 0, sizeof *o);
    size_t n = strlen(key);
    o->key = tv_alloc(n + 1);
    memcpy(o->key, key, n + 1);
    o->next = tv_origins[b];
    tv_origins[b] = o;
    return o;
}

static void tv_origin_unlink(tv_fc *c) {
    tv_origin *o = c->origin;
    for (tv_fc **pp = &o->idle; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; o->nidle--; return; }
}

/* ---- failure */

static void tv_fr_resolve(tv_promise **pp, tv_int v) {
    tv_promise *p = *pp;
    if (!p) return;
    *pp = NULL;
    tv_promise_resolve(p, &v);
    tv_promise_release(p);
}

/* The response is complete (0), or the request failed (< 0) — before the head arrived, or while
 * the body did. */
static void tv_fr_settle(tv_fr *r, int result) {
    if (r->result != 1) return;
    r->result = result;
    tv_io_refs--;
    if (r->head_state == 1) {
        r->head_state = result;
        tv_fr_resolve(&r->done, result);
    }
    tv_fr_resolve(&r->body_p, result);
    tv_fr_resolve(&r->read_p, result == 0 && r->rbody.len ? (tv_int)r->rbody.len : result);
}

/* The final response's head is in: fetch() resolves (the body follows). */
static void tv_fr_head_ready(tv_fr *r) {
    if (r->head_state != 1) return;
    r->head_state = 0;
    tv_fr_resolve(&r->done, 0);
}

/* A body streamed as it arrives: at most this much waits for the reader before the socket pauses
 * (a reader that stops partway and drops the body leaves no more than this held until it's
 * collected). */
#define TV_STREAM_HIGH (256 << 10)

/* Set by the host (runtime/node.c): bytes of response bodies as they arrive, which the
 * JavaScript objects reading them hold until they're collected. */
void (*tv_fetch_on_bytes)(size_t n);

/* An encoded body that's decoded (not `decompress: false`). */
static inline bool tv_fr_decoding(tv_fr *r) { return r->decompress && r->enc; }

/* Where arriving body bytes go: an encoded body's are decoded from raw onto rbody. */
static inline tv_sb *tv_fr_sink(tv_fr *r) { return tv_fr_decoding(r) ? &r->raw : &r->rbody; }

static bool tv_fr_decode(tv_fr *r, bool final);

/* Body bytes arrived: decode them, and a waiting reader gets them. True when the reader is far
 * enough behind that the socket should pause. */
static bool tv_fr_data(tv_fr *r) {
    /* (nothing's reading it yet: at most TV_STREAM_HIGH arrives until the program reads it, whole
     * or as a stream; a body read partway or not at all holds no more than that) */
    if (!r->streaming) return !r->whole && tv_fr_sink(r)->len >= TV_STREAM_HIGH;
    if (tv_fr_decoding(r) && !tv_fr_decode(r, false)) return false;
    if (r->read_p && r->rbody.len) tv_fr_resolve(&r->read_p, (tv_int)r->rbody.len);
    return r->rbody.len >= TV_STREAM_HIGH || r->raw_off < r->raw.len;
}

static void tv_fr_fail(tv_fr *r, const char *code, const char *fmt, ...) {
    if (r->result != 1) return;
    snprintf(r->code_buf, sizeof r->code_buf, "%s", code); /* the TLS layer's codes don't outlive the connection */
    r->code = r->code_buf;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    tv_sb_push(&r->message, buf, n < 0 ? 0 : (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    if (r->c) {
        tv_fc *c = r->c;
        r->c = NULL;
        c->r = NULL;
        tv_fc_close(c);
    }
    tv_fr_settle(r, -1);
}

static const char TV_FETCH_VERBOSE[] = "For more information, pass `verbose: true` in the second argument to fetch()";

/* ---- the request line and headers */

static bool tv_hdr_has(tv_str block, const char *name) { return tv_has_header(block, name); }

static void tv_base64(tv_sb *sb, const char *s, size_t n) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(uint8_t)s[i] << 16 | (i + 1 < n ? (uint32_t)(uint8_t)s[i + 1] << 8 : 0) | (i + 2 < n ? (uint8_t)s[i + 2] : 0);
        tv_sb_push_char(sb, tbl[v >> 18 & 63]);
        tv_sb_push_char(sb, tbl[v >> 12 & 63]);
        tv_sb_push_char(sb, i + 1 < n ? tbl[v >> 6 & 63] : '=');
        tv_sb_push_char(sb, i + 2 < n ? tbl[v & 63] : '=');
    }
}

/* The proxy variables, read once (each fetch read four: a scan of the environment each; the
 * program's own process.env doesn't change the C environment they're in). */
static struct { bool read; const char *no_proxy, *http, *https; } tv_proxy_env;

static void tv_proxy_env_read(void) {
    if (tv_proxy_env.read) return;
    tv_proxy_env.read = true;
    const char *list = getenv("no_proxy");
    if (!list || !*list) list = getenv("NO_PROXY");
    tv_proxy_env.no_proxy = list;
    for (int https = 0; https < 2; https++) {
        const char *spec = https ? getenv("https_proxy") : getenv("http_proxy");
        if (!spec || !*spec) spec = https ? getenv("HTTPS_PROXY") : getenv("HTTP_PROXY");
        if (spec && !*spec) spec = NULL;
        if (https) tv_proxy_env.https = spec;
        else tv_proxy_env.http = spec;
    }
}

/* NO_PROXY: "*", or comma-separated hosts, domains ("example.com" also covers its subdomains,
 * as does ".example.com") and host:port pairs. */
static bool tv_no_proxy(const char *host, int port) {
    tv_proxy_env_read();
    const char *list = tv_proxy_env.no_proxy;
    if (!list) return false;
    size_t hl = strlen(host);
    for (const char *p = list; *p;) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        const char *te = e;
        while (te > p && te[-1] == ' ') te--;
        size_t n = (size_t)(te - p);
        if (n == 1 && *p == '*') return true;
        const char *colon = memchr(p, ':', n);
        if (colon && !memchr(p, '[', n)) {
            if (atoi(colon + 1) != port) { p = e; continue; }
            n = (size_t)(colon - p);
        }
        const char *d = p;
        if (n && *d == '.') { d++; n--; }
        if (n && (hl == n || (hl > n && host[hl - n - 1] == '.')) && strncasecmp(host + hl - n, d, n) == 0) return true;
        p = e;
    }
    return false;
}

/* Where r connects: its host, or (the `proxy` option, else HTTP_PROXY/HTTPS_PROXY unless
 * NO_PROXY) a proxy. Unix sockets never go through a proxy. */
static bool tv_fr_choose_proxy(tv_fr *r, bool https) {
    tv_free(r->dial_host);
    tv_free(r->proxy_auth);
    r->dial_host = NULL;
    r->proxy_auth = NULL;
    r->via_proxy = false;
    const char *spec = r->proxy.p->len ? r->proxy.p->data : NULL;
    if (!spec && !r->unix_path.p->len && !tv_no_proxy(r->host, r->port)) spec = https ? tv_proxy_env.https : tv_proxy_env.http;
    if (!spec || r->unix_path.p->len) {
        size_t hl = strlen(r->host);
        r->dial_host = tv_alloc(hl + 1);
        memcpy(r->dial_host, r->host, hl + 1);
        r->dial_port = r->port;
        return true;
    }
    /* [http://][user:pass@]host[:port] */
    const char *p = spec;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strstr(p, "://")) { tv_fr_fail(r, "UnsupportedProxyProtocol", "Only http: proxies are supported: %s", spec); return false; }
    const char *end = p + strcspn(p, "/?#");
    const char *at = NULL;
    for (const char *x = p; x < end; x++) if (*x == '@') at = x;
    if (at) {
        tv_str ui = tv_native_urlDecode(tv_str_from(p, (size_t)(at - p)), false);
        tv_sb auth = {0};
        tv_sb_push(&auth, "Basic ", 6);
        tv_base64(&auth, ui.p->data, (size_t)ui.p->len);
        tv_sb_push_char(&auth, 0);
        r->proxy_auth = tv_alloc(auth.len);
        memcpy(r->proxy_auth, auth.data, auth.len);
        tv_sb_free(&auth);
        tv_str_release(ui);
        p = at + 1;
    }
    const char *hend = p;
    if (*hend == '[') { while (hend < end && *hend != ']') hend++; if (hend < end) hend++; }
    while (hend < end && *hend != ':') hend++;
    size_t hl = (size_t)(hend - p);
    if (hl == 0) { tv_fr_fail(r, "InvalidProxyURL", "The proxy URL is invalid: %s", spec); return false; }
    r->dial_host = tv_alloc(hl + 1);
    memcpy(r->dial_host, p, hl);
    r->dial_host[hl] = 0;
    r->dial_port = hend < end ? atoi(hend + 1) : 80;
    r->via_proxy = true;
    return true;
}

/* Splits r->url (a normalized href) into host, port and target and writes the request head.
 * False (after failing r) if it isn't an http URL. */
static bool tv_fr_prepare(tv_fr *r) {
    const char *s = r->url.p->data, *end = s + r->url.p->len;
    bool https = r->url.p->len >= 8 && memcmp(s, "https://", 8) == 0;
    if (!https && !(r->url.p->len >= 7 && memcmp(s, "http://", 7) == 0)) {
        tv_fr_fail(r, "ERR_INVALID_ARG_VALUE", "protocol must be http: or https:");
        return false;
    }
    if (https && !tv_tls_impl) {
        tv_fr_fail(r, "ERR_TLS_UNSUPPORTED", "https: isn't available: this program was built without TLS");
        return false;
    }
    r->https = https;
    const char *a = s + (https ? 8 : 7), *auth_end = a;
    while (auth_end < end && *auth_end != '/' && *auth_end != '?' && *auth_end != '#') auth_end++;
    const char *host = a, *userinfo = NULL, *userinfo_end = NULL;
    for (const char *x = a; x < auth_end; x++) if (*x == '@') { userinfo = a; userinfo_end = x; host = x + 1; }
    const char *hend = host;
    if (hend < auth_end && *hend == '[') { while (hend < auth_end && *hend != ']') hend++; if (hend < auth_end) hend++; }
    while (hend < auth_end && *hend != ':') hend++;
    int port = https ? 443 : 80;
    if (hend < auth_end) port = atoi(hend + 1);
    tv_free(r->host);
    tv_free(r->key);
    size_t hl = (size_t)(hend - host);
    r->host = tv_alloc(hl + 1);
    memcpy(r->host, host, hl);
    r->host[hl] = 0;
    r->port = port;
    /* connections are pooled per scheme, host, port and TLS options */
    size_t kl = hl + 64;
    r->key = tv_alloc(kl);
    if (https) snprintf(r->key, kl, "https://%s:%d%s|%08x", r->host, port, r->insecure ? "|insecure" : "", r->ca.p->len ? tv_hash_cstr(r->ca.p->data) : 0);
    else snprintf(r->key, kl, "http://%s:%d", r->host, port);
    if (r->unix_path.p->len) { /* pooled per socket too */
        size_t used = strlen(r->key);
        snprintf(r->key + used, kl - used, "|unix:%08x", tv_hash_cstr(r->unix_path.p->data));
    }
    const char *target = auth_end, *hash = memchr(target, '#', (size_t)(end - target));
    if (!hash) hash = end;
    if (!tv_fr_choose_proxy(r, https)) return false;
    tv_sb *h = &r->head;
    h->len = 0;
    r->merged = false;
    tv_sb_push(h, r->method.p->data, (size_t)r->method.p->len);
    tv_sb_push_char(h, ' ');
    if (r->via_proxy && !https) {
        /* through a proxy, plain HTTP names the whole URL (absolute form) */
        tv_sb_push(h, s, (size_t)(auth_end - s));
    }
    if (target == hash || *target != '/') tv_sb_push_char(h, '/');
    tv_sb_push(h, target, (size_t)(hash - target));
    tv_sb_push(h, " HTTP/1.1\r\n", 11);
    tv_sb_push(h, r->headers.p->data, (size_t)r->headers.p->len);
    /* Bun's defaults, after the caller's headers */
    if (!tv_hdr_has(r->headers, "connection")) tv_sb_push(h, "Connection: keep-alive\r\n", 24);
    if (!tv_hdr_has(r->headers, "user-agent")) tv_sb_push(h, "User-Agent: Tov/0.0.1\r\n", sizeof "User-Agent: Tov/0.0.1\r\n" - 1);
    if (!tv_hdr_has(r->headers, "accept")) tv_sb_push(h, "Accept: */*\r\n", 13);
    if (!tv_hdr_has(r->headers, "host")) {
        tv_sb_push(h, "Host: ", 6);
        tv_sb_push(h, host, (size_t)(auth_end - host));
        tv_sb_push(h, "\r\n", 2);
    }
    /* what Bun asks for (brotli and zstd come with the TLS archive, which every fetch program links) */
    if (r->decompress && !tv_hdr_has(r->headers, "accept-encoding")) {
        if (tv_codec) tv_sb_push(h, "Accept-Encoding: gzip, deflate, br, zstd\r\n", 42);
    }
    if (r->via_proxy && !https && r->proxy_auth && !tv_hdr_has(r->headers, "proxy-authorization")) {
        tv_sb_push(h, "Proxy-Authorization: ", 21);
        tv_sb_push(h, r->proxy_auth, strlen(r->proxy_auth));
        tv_sb_push(h, "\r\n", 2);
    }
    if (userinfo && !tv_hdr_has(r->headers, "authorization")) {
        tv_str ui = tv_native_urlDecode(tv_str_from(userinfo, (size_t)(userinfo_end - userinfo)), false);
        tv_sb_push(h, "Authorization: Basic ", 21);
        tv_base64(h, ui.p->data, (size_t)ui.p->len);
        tv_sb_push(h, "\r\n", 2);
        tv_str_release(ui);
    }
    const char *m = r->method.p->data;
    /* as Bun: every method but GET and HEAD says its length, even when it's 0 */
    bool wants_length = r->body.p->len > 0 || (strcmp(m, "GET") != 0 && strcmp(m, "HEAD") != 0);
    if (wants_length && !tv_hdr_has(r->headers, "content-length") && !tv_hdr_has(r->headers, "transfer-encoding")) {
        char cl[48];
        int n = snprintf(cl, sizeof cl, "Content-Length: %d\r\n", (int)r->body.p->len);
        tv_sb_push(h, cl, (size_t)n);
    }
    tv_sb_push(h, "\r\n", 2);
    r->head_only = strcmp(m, "HEAD") == 0;
    if (r->via_proxy) { /* pooled per proxy too */
        size_t used = strlen(r->key), kl = used + strlen(r->dial_host) + 32;
        r->key = tv_realloc(r->key, kl);
        snprintf(r->key + used, kl - used, "|proxy:%s:%d", r->dial_host, r->dial_port);
    }
    return true;
}

/* ---- writing */

/* Bytes of request still to write: the head, then the body (unless merged into the head). */
static size_t tv_fr_out_len(const tv_fr *r) { return r->head.len + (r->merged ? 0 : (size_t)r->body.p->len); }

static bool tv_fc_write(tv_fc *c) {
    tv_fr *r = c->r;
    size_t hn = r->head.len, bn = r->merged ? 0 : (size_t)r->body.p->len;
    if (c->tls) {
        while (r->sent < hn + bn) {
            const char *p = r->sent < hn ? r->head.data + r->sent : r->body.p->data + (r->sent - hn);
            size_t n = r->sent < hn ? hn - r->sent : bn - (r->sent - hn);
            long w = tv_tls_impl->write(c->tls, p, n);
            if (w > 0) { r->sent += (size_t)w; continue; }
            if (w == -1) { tv_fc_interest(c, true, false); return true; }
            if (w == -2) { tv_fc_interest(c, true, true); return true; }
            return false;
        }
        tv_fc_interest(c, true, false);
        return true;
    }
    while (r->sent < hn + bn) {
        struct iovec iov[2];
        int n = 0;
        if (r->sent < hn) iov[n++] = (struct iovec){ r->head.data + r->sent, hn - r->sent };
        size_t bo = r->sent > hn ? r->sent - hn : 0;
        if (bn > bo) iov[n++] = (struct iovec){ r->body.p->data + bo, bn - bo };
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = iov;
        mh.msg_iovlen = n;
#ifdef MSG_NOSIGNAL
        ssize_t w = sendmsg(c->fd, &mh, MSG_NOSIGNAL);
#else
        ssize_t w = sendmsg(c->fd, &mh, 0);
#endif
        if (w > 0) { r->sent += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { tv_fc_interest(c, true, true); return true; }
        return false;
    }
    tv_fc_interest(c, true, false);
    return true;
}

/* ---- the response */

/* No one's reading what's arrived: stop reading until they do. What's held meanwhile is told to
 * the host's collector (a response read partway and dropped holds it, and its connection, until
 * the response is collected); bytes a reader takes it counts itself, as they reach it. */
static void tv_fr_pause(tv_fr *r, tv_fc *c) {
    r->paused = true;
    tv_fc_interest(c, false, false);
    if (tv_fetch_on_bytes) tv_fetch_on_bytes(r->raw.len + r->rbody.len);
}

static int tv_hex(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }

static bool tv_ieq(const char *a, size_t n, const char *lit) {
    size_t m = strlen(lit);
    if (n != m) return false;
    for (size_t i = 0; i < n; i++) if ((a[i] | 0x20) != lit[i]) return false;
    return true;
}

/* A token list header ("Connection: keep-alive, Upgrade") holds `word`. */
static bool tv_has_token(const char *v, size_t n, const char *word) {
    size_t wl = strlen(word);
    for (size_t i = 0; i < n;) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t s = i;
        while (i < n && v[i] != ',') i++;
        size_t e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
        if (e - s == wl) {
            size_t k = 0;
            while (k < wl && (v[s + k] | 0x20) == word[k]) k++;
            if (k == wl) return true;
        }
    }
    return false;
}

static bool tv_redirect_status(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

/* Parses a response head (status line and header lines, `n` bytes up to the blank line). */
static bool tv_fr_head(tv_fr *r, const char *p, size_t n) {
    const char *end = p + n, *eol = memchr(p, '\n', n);
    if (!eol || eol - p < 12 || memcmp(p, "HTTP/1.", 7) != 0 || (p[7] != '0' && p[7] != '1') || p[8] != ' ') return false;
    bool http10 = p[7] == '0';
    if (p[9] < '1' || p[9] > '9' || p[10] < '0' || p[10] > '9' || p[11] < '0' || p[11] > '9') return false;
    r->status = (p[9] - '0') * 100 + (p[10] - '0') * 10 + (p[11] - '0');
    const char *reason = p + 12, *reason_end = eol;
    if (reason_end > reason && reason_end[-1] == '\r') reason_end--;
    if (reason < reason_end && *reason == ' ') reason++;
    r->status_text.len = 0;
    tv_sb_push(&r->status_text, reason, (size_t)(reason_end - reason));
    r->rheaders.len = 0;
    r->location.len = 0;
    r->enc = 0;
    int64_t length = -1;
    bool chunked = false, te = false, close = http10, keepalive_seen = false;
    for (const char *l = eol + 1; l < end;) {
        const char *le = memchr(l, '\n', (size_t)(end - l));
        if (!le) le = end;
        const char *lend = le > l && le[-1] == '\r' ? le - 1 : le;
        if (lend == l) break;
        const char *colon = memchr(l, ':', (size_t)(lend - l));
        if (!colon || colon == l) return false;
        for (const char *x = l; x < colon; x++) if ((unsigned char)*x <= ' ' || *x == 0x7f) return false;
        const char *v = colon + 1, *ve = lend;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t nl = (size_t)(colon - l), vl = (size_t)(ve - v);
        tv_sb_push(&r->rheaders, l, nl);
        tv_sb_push(&r->rheaders, ": ", 2);
        tv_sb_push(&r->rheaders, v, vl);
        tv_sb_push(&r->rheaders, "\r\n", 2);
        if (tv_ieq(l, nl, "content-length")) {
            int64_t cl = 0;
            if (vl == 0) return false;
            for (size_t i = 0; i < vl; i++) {
                if (v[i] < '0' || v[i] > '9' || cl > (INT64_MAX - 9) / 10) return false;
                cl = cl * 10 + (v[i] - '0');
            }
            if (length >= 0 && length != cl) return false;
            length = cl;
        } else if (tv_ieq(l, nl, "transfer-encoding")) {
            te = true;
            /* chunked must be the last coding */
            size_t k = vl;
            while (k > 0 && v[k - 1] != ',') k--;
            const char *last = v + k;
            while (last < ve && (*last == ' ' || *last == '\t')) last++;
            chunked = tv_ieq(last, (size_t)(ve - last), "chunked");
        } else if (tv_ieq(l, nl, "connection")) {
            if (tv_has_token(v, vl, "close")) close = true;
            if (tv_has_token(v, vl, "keep-alive")) keepalive_seen = true;
        } else if (tv_ieq(l, nl, "content-encoding")) {
            r->enc = tv_ieq(v, vl, "gzip") || tv_ieq(v, vl, "x-gzip") ? 1 : tv_ieq(v, vl, "deflate") ? 2 : tv_ieq(v, vl, "br") ? 3 : tv_ieq(v, vl, "zstd") ? 4 : 0;
        } else if (tv_ieq(l, nl, "location")) {
            r->location.len = 0;
            tv_sb_push(&r->location, v, vl);
        }
        l = le + 1;
    }
    if (http10 && keepalive_seen) close = false;
    r->keep = !close;
    int s = r->status;
    bool no_body = r->head_only || s == 204 || s == 304 || (s >= 100 && s < 200);
    if (no_body) {
        r->state = TV_FR_DONE;
    } else if (te) {
        if (chunked) r->state = TV_FR_CHUNK_SIZE;
        else { r->state = TV_FR_UNTIL_CLOSE; r->keep = false; }
    } else if (length >= 0) {
        r->remaining = length;
        r->state = length ? TV_FR_FIXED : TV_FR_DONE;
    } else {
        r->state = TV_FR_UNTIL_CLOSE;
        r->keep = false;
    }
    r->discard = r->redirect_mode != TV_FETCH_MANUAL && tv_redirect_status(s) && r->location.len > 0;
    if (!r->discard && length > 0 && r->state == TV_FR_FIXED) {
        if (length > INT32_MAX) return false;
        /* (sized for the whole body once it's known to be read whole; read as a stream, or
         * not read yet, it grows as bytes come) */
        if (r->whole) tv_sb_grow(tv_fr_sink(r), (size_t)length < ((size_t)64 << 20) ? (size_t)length : ((size_t)64 << 20));
    }
    return true;
}

/* (an encoded body that isn't decoded is checked when text() reads it) */
static void tv_fr_check_utf8(tv_fr *r) {
    if (!r->utf8_bad && (!r->enc || tv_fr_decoding(r))) r->utf8_bad = !tv_utf8_scan((const uint8_t *)r->rbody.data, r->rbody.len, &r->utf8_pos);
}

/* Decodes what has arrived of an encoded body onto rbody: all of it at the end (final) or for a
 * whole-body read, else as much as a reader may have waiting. False (the request failed) if it's
 * corrupt, or at the end, cut short. */
static bool tv_fr_decode(tv_fr *r, bool final) {
    const char *why = r->enc == 3 ? "BrotliDecompressionError" : r->enc == 4 ? "ZstdDecompressionError" : "ZlibError"; /* Bun's codes */
    if (!r->dec && !(tv_codec && (r->dec = tv_codec->open(r->enc)))) {
        tv_fr_fail(r, why, "%s fetching \"%s\". %s", why, r->url.p->data, TV_FETCH_VERBOSE);
        return false;
    }
    size_t limit = 0;
    if (!final) {
        if (r->rbody.len >= TV_STREAM_HIGH) return true;
        limit = TV_STREAM_HIGH - r->rbody.len;
    }
    if (r->rbody.cap == 0) {
        /* one buffer for the output, reserved once: 8 times the encoded size when it's known
         * (pages never written cost nothing), at most what may wait for a reader. Growing it
         * step by step instead leaves a freed block per step (macOS keeps them cached). */
        size_t est = r->state == TV_FR_FIXED || r->state == TV_FR_DONE ? (r->raw.len + (size_t)r->remaining) * 8 : 1 << 20;
        if (limit && est > TV_STREAM_HIGH + (64 << 10)) est = TV_STREAM_HIGH + (64 << 10);
        if (est < (64 << 10)) est = 64 << 10;
        if (est > (1 << 30)) est = 1 << 30;
        tv_sb_grow(&r->rbody, est);
    }
    int st = tv_codec->step(r->dec, (const uint8_t *)r->raw.data, r->raw.len, &r->raw_off, &r->rbody, limit);
    if (st < 0 || (final && st == 0) || r->rbody.len > INT32_MAX) {
        /* corrupt, or (at the end) cut short */
        tv_fr_fail(r, why, "%s fetching \"%s\". %s", why, r->url.p->data, TV_FETCH_VERBOSE);
        return false;
    }
    if (r->raw_off == r->raw.len) {
        r->raw.len = r->raw_off = 0;
    } else if (r->raw_off >= (64 << 10)) {
        memmove(r->raw.data, r->raw.data + r->raw_off, r->raw.len - r->raw_off);
        r->raw.len -= r->raw_off;
        r->raw_off = 0;
    }
    tv_fr_check_utf8(r);
    return true;
}

/* Decodes an encoded body that has all arrived, at once. False (the request failed) if it's
 * corrupt or cut short. */
static bool tv_fr_decode_whole(tv_fr *r) {
    if (r->dec) return tv_fr_decode(r, true); /* a reader started it in chunks */
    if (!tv_codec || !tv_codec->whole(r->enc, (const uint8_t *)r->raw.data + r->raw_off, r->raw.len - r->raw_off, &r->rbody) || r->rbody.len > INT32_MAX) {
        const char *why = r->enc == 3 ? "BrotliDecompressionError" : r->enc == 4 ? "ZstdDecompressionError" : "ZlibError";
        tv_fr_fail(r, why, "%s fetching \"%s\". %s", why, r->url.p->data, TV_FETCH_VERBOSE);
        return false;
    }
    r->raw.len = r->raw_off = 0;
    tv_fr_check_utf8(r);
    return true;
}

static void tv_fr_take(tv_fr *r, const char *p, size_t n) {
    if (r->discard || r->result != 1) return;
    tv_sb_push(tv_fr_sink(r), p, n);
    if (!tv_fr_decoding(r)) tv_fr_check_utf8(r);
    tv_fr_data(r);
}

/* Consumes what it can of the connection's input. 1: the response is complete, 0: needs more,
 * -1: malformed. */
static int tv_fr_parse(tv_fr *r, tv_fc *c) {
    for (;;) {
        char *p = c->in + c->in_off;
        size_t n = c->in_len - c->in_off;
        switch (r->state) {
        case TV_FR_HEAD: {
            if (n == 0) return 0;
            /* the blank line ends the head */
            const char *e = NULL;
            for (const char *x = p; (x = memchr(x, '\n', (size_t)(p + n - x))) != NULL; x++) {
                if (x + 1 < p + n && x[1] == '\n') { e = x + 2; break; }
                if (x + 2 < p + n && x[1] == '\r' && x[2] == '\n') { e = x + 3; break; }
            }
            if (!e) return n > TV_HTTP_MAX_HEAD ? -1 : 0;
            if (!tv_fr_head(r, p, (size_t)(e - p))) return -1;
            c->in_off += (size_t)(e - p);
            if (r->status >= 100 && r->status < 200) {
                if (r->status == 101) return -1;
                r->state = TV_FR_HEAD; /* an interim response: the real one follows */
                continue;
            }
            if (!r->discard) tv_fr_head_ready(r);
            if (r->state == TV_FR_DONE) return 1;
            continue;
        }
        case TV_FR_FIXED: {
            if (n == 0) return 0;
            size_t take = (int64_t)n < r->remaining ? n : (size_t)r->remaining;
            tv_fr_take(r, p, take);
            if (r->result != 1) return 0; /* the body couldn't be decoded */
            c->in_off += take;
            r->remaining -= (int64_t)take;
            if (r->remaining == 0) { r->state = TV_FR_DONE; return 1; }
            continue;
        }
        case TV_FR_CHUNK_SIZE: {
            char *nl = memchr(p, '\n', n);
            if (!nl) return n > 4096 ? -1 : 0;
            int64_t size = 0;
            const char *x = p;
            int digits = 0;
            for (int d; (d = tv_hex(*x)) >= 0; x++, digits++) {
                if (size > (INT64_MAX >> 4)) return -1;
                size = size * 16 + d;
            }
            if (!digits) return -1;
            c->in_off += (size_t)(nl + 1 - p);
            if (size == 0) { r->state = TV_FR_TRAILERS; continue; }
            if (!r->discard && (int64_t)tv_fr_sink(r)->len + size > INT32_MAX) return -1;
            r->remaining = size;
            r->state = TV_FR_CHUNK_DATA;
            continue;
        }
        case TV_FR_CHUNK_DATA: {
            if (n == 0) return 0;
            size_t take = (int64_t)n < r->remaining ? n : (size_t)r->remaining;
            tv_fr_take(r, p, take);
            if (r->result != 1) return 0; /* the body couldn't be decoded */
            c->in_off += take;
            r->remaining -= (int64_t)take;
            if (r->remaining == 0) r->state = TV_FR_CHUNK_END;
            continue;
        }
        case TV_FR_CHUNK_END: {
            if (n == 0) return 0;
            if (p[0] == '\n') { c->in_off += 1; r->state = TV_FR_CHUNK_SIZE; continue; }
            if (n < 2) return 0;
            if (p[0] != '\r' || p[1] != '\n') return -1;
            c->in_off += 2;
            r->state = TV_FR_CHUNK_SIZE;
            continue;
        }
        case TV_FR_TRAILERS: {
            char *nl = memchr(p, '\n', n);
            if (!nl) return n > TV_HTTP_MAX_HEAD ? -1 : 0;
            bool blank = nl == p || (nl == p + 1 && p[0] == '\r');
            c->in_off += (size_t)(nl + 1 - p);
            if (blank) { r->state = TV_FR_DONE; return 1; }
            continue;
        }
        case TV_FR_UNTIL_CLOSE: {
            if (n) { tv_fr_take(r, p, n); c->in_off += n; }
            if (r->result != 1) return 0;
            if (!r->discard && tv_fr_sink(r)->len > INT32_MAX) return -1;
            return 0;
        }
        default:
            return 1;
        }
    }
}

/* ---- connections */

static void tv_fc_release(tv_fc *c, bool reusable) {
    c->r = NULL;
    if (!reusable || c->dead || c->no_reuse) { tv_fc_close(c); return; }
    tv_origin *o = c->origin;
    if (o->nidle >= 256) { tv_fc_close(c); return; }
    c->reused = true;
    c->in_off = c->in_len = 0;
    if (c->in_cap > 16384) { tv_fc_rbuf_free(c->in, c->in_cap); c->in = NULL; c->in_cap = 0; }
    /* idle: watch for the server closing it */
    tv_fc_interest(c, true, false);
    c->next = o->idle;
    o->idle = c;
    o->nidle++;
}

/* > 0 bytes; 0 the connection ended; -1 nothing more for now; -2 TLS wants to write; -3 failed. */
static long tv_fc_recv(tv_fc *c, char *buf, size_t n) {
    if (c->tls) return tv_tls_impl->read(c->tls, buf, n);
    for (;;) {
        ssize_t r = read(c->fd, buf, n);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : -3;
    }
}

static bool tv_fc_open(tv_fc *c) {
    for (; c->ai < c->addrs.n; c->ai++) {
        struct sockaddr_storage *sa = &c->addrs.a[c->ai];
        if (sa->ss_family == AF_INET) ((struct sockaddr_in *)sa)->sin_port = htons((uint16_t)c->port);
        else if (sa->ss_family == AF_INET6) ((struct sockaddr_in6 *)sa)->sin6_port = htons((uint16_t)c->port);
        int fd = socket(sa->ss_family, SOCK_STREAM, 0);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        c->fd = fd;
        c->registered = c->reading = c->writing = false;
        int rc = connect(fd, (struct sockaddr *)sa, c->addrs.len[c->ai]);
        if (rc == 0) { c->connecting = false; return true; }
        if (errno == EINPROGRESS || errno == EINTR) {
            c->connecting = true;
            tv_fc_interest(c, false, true);
            return true;
        }
        c->err = errno;
        close(fd);
        c->fd = -1;
    }
    return false;
}

static void tv_fr_connect_failed(tv_fr *r, int err) {
    if (r->unix_path.p->len) { tv_fr_fail(r, "FailedToOpenSocket", "Was there a typo in the url or port?"); return; }
    if (err == ECONNREFUSED) tv_fr_fail(r, "ConnectionRefused", "Unable to connect. Is the computer able to access the url?");
    else if (err == ETIMEDOUT) tv_fr_fail(r, "ConnectionTimeout", "Unable to connect. Is the computer able to access the url?");
    else tv_fr_fail(r, "ConnectionRefused", "Unable to connect. Is the computer able to access the url?");
}

static void tv_fr_send(tv_fr *r, tv_fc *c) {
    r->c = c;
    c->r = r;
    r->sent = 0;
    if (c->tls && !r->merged && r->body.p->len && r->body.p->len <= 8192) {
        /* one TLS record (and one write) for a small request */
        tv_sb_push(&r->head, r->body.p->data, (size_t)r->body.p->len);
        r->merged = true;
    }
    r->state = TV_FR_HEAD;
    r->got_any = false;
    if (!tv_fc_write(c)) {
        /* a pooled connection the server closed: try once more on a new one */
        bool retry = c->reused && !r->retried;
        c->r = NULL;
        r->c = NULL;
        tv_fc_close(c);
        if (retry) { r->retried = true; tv_fr_step(r); }
        else tv_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", TV_FETCH_VERBOSE);
    }
}

/* Continues a TLS handshake; sends the request once it's done. */
static void tv_fc_handshake(tv_fc *c) {
    tv_fr *r = c->r;
    int rc = tv_tls_impl->handshake(c->tls);
    if (rc == 0) { c->handshaking = false; tv_fr_send(r, c); return; }
    /* still connecting (the ClientHello went early): wait for the connection, and report a
     * refused one as such, not as a TLS error */
    if (c->connecting) {
        /* the write took the connect's error (a loopback refusal is instant on Linux): keep it,
         * as SO_ERROR won't say it again */
        if (rc < 0) c->early_err = errno ? errno : ECONNREFUSED;
        tv_fc_interest(c, true, true);
        return;
    }
    if (rc == 1) { tv_fc_interest(c, true, false); return; }
    if (rc == 2) { tv_fc_interest(c, true, true); return; }
    const char *code = "ERR_SSL";
    char msg[512];
    tv_tls_impl->why(c->tls, r->url.p->data, &code, msg, sizeof msg);
    tv_fr_fail(r, code, "%s", msg);
}

static void tv_fc_start_tls(tv_fc *c);
static void tv_fr_complete(tv_fr *r);

/* Sends what's left of the CONNECT request; true once it's all out. */
static bool tv_fc_send_ctl(tv_fc *c) {
    while (c->ctl_off < c->ctl.len) {
#ifdef MSG_NOSIGNAL
        ssize_t w = send(c->fd, c->ctl.data + c->ctl_off, c->ctl.len - c->ctl_off, MSG_NOSIGNAL);
#else
        ssize_t w = send(c->fd, c->ctl.data + c->ctl_off, c->ctl.len - c->ctl_off, 0);
#endif
        if (w > 0) { c->ctl_off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { tv_fc_interest(c, true, true); return false; }
        return false;
    }
    tv_fc_interest(c, true, false);
    return true;
}

/* The proxy's answer to CONNECT: 200 starts TLS through the tunnel; anything else is the
 * response (a 407, say), as Bun gives it. */
static void tv_fc_tunnel(tv_fc *c, bool readable, bool writable) {
    tv_fr *r = c->r;
    if (writable && c->ctl_off < c->ctl.len && !tv_fc_send_ctl(c) && c->ctl_off < c->ctl.len) return;
    if (!readable) return;
    for (;;) {
        if (c->in_cap - c->in_len < 4096) {
            size_t cap = c->in_cap ? c->in_cap * 2 : 16384;
            c->in = tv_realloc(c->in, cap);
            c->in_cap = cap;
        }
        ssize_t n = read(c->fd, c->in + c->in_len, c->in_cap - c->in_len);
        if (n > 0) { c->in_len += (size_t)n; continue; }
        if (n == 0) { tv_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", TV_FETCH_VERBOSE); return; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        tv_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", TV_FETCH_VERBOSE);
        return;
    }
    const char *e = NULL;
    for (size_t i = 3; i < c->in_len; i++) if (memcmp(c->in + i - 3, "\r\n\r\n", 4) == 0) { e = c->in + i + 1; break; }
    if (!e) return; /* more to come */
    c->tunneling = false;
    if (c->in_len >= 12 && memcmp(c->in, "HTTP/1.", 7) == 0 && memcmp(c->in + 9, "200", 3) == 0) {
        c->in_off = c->in_len = 0; /* the tunnel is open */
        tv_fc_start_tls(c);
        return;
    }
    /* refused: parse the proxy's response as the answer */
    c->no_reuse = true;
    r->https = false;
    r->state = TV_FR_HEAD;
    r->got_any = true;
    int st = tv_fr_parse(r, c);
    if (r->result != 1) return;
    if (st < 0) { tv_fr_fail(r, "Malformed_HTTP_Response", "Malformed_HTTP_Response fetching \"%s\". %s", r->url.p->data, TV_FETCH_VERBOSE); return; }
    r->keep = false;
    if (st > 0) {
        r->c = NULL;
        tv_fc_release(c, false);
        tv_fr_complete(r);
    }
}

/* The TCP connection is up: tunnel through the proxy (https), start TLS, or send the request. */
static void tv_fc_connected(tv_fc *c) {
    tv_fr *r = c->r;
    if (!r->https) { tv_fr_send(r, c); return; }
    if (c->tls) { tv_fc_handshake(c); return; } /* the ClientHello, made while connecting, goes out */
    if (r->via_proxy) {
        tv_sb *h = &c->ctl;
        h->len = 0;
        c->ctl_off = 0;
        char line[600];
        int n = snprintf(line, sizeof line, "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\n", r->host, r->port, r->host, r->port);
        tv_sb_push(h, line, (size_t)n);
        if (r->proxy_auth) {
            tv_sb_push(h, "Proxy-Authorization: ", 21);
            tv_sb_push(h, r->proxy_auth, strlen(r->proxy_auth));
            tv_sb_push(h, "\r\n", 2);
        }
        tv_sb_push(h, "\r\n", 2);
        c->tunneling = true;
        tv_fc_send_ctl(c);
        return;
    }
    tv_fc_start_tls(c);
}

static void tv_fc_start_tls(tv_fc *c) {
    tv_fr *r = c->r;
    c->tls = tv_tls_impl->open(c->fd, r->host, r->key, !r->insecure, r->ca.p->data, (size_t)r->ca.p->len);
    if (!c->tls) { tv_fr_fail(r, "ERR_SSL", "TLS setup failed"); return; }
    c->handshaking = true;
    tv_fc_handshake(c);
}

/* idle and opening connections the pool is kept topped up to (tv_fc_spare) */
#define TV_FC_SPARES 3
static void tv_fc_spare(tv_fc *old, tv_fr *r);

/* Connects to the resolved addresses (DNS done). */
static void tv_fr_connect(tv_fr *r, const tv_addrs *addrs) {
    tv_fc *c = tv_alloc(sizeof *c);
    memset(c, 0, sizeof *c);
    c->io.ready = tv_fc_ready;
    c->fd = -1;
    c->addrs = *addrs;
    c->port = r->dial_port;
    c->origin = tv_origin_get(r->key);
    c->err = ECONNREFUSED;
    if (!tv_fc_open(c)) {
        int err = c->err;
        tv_free(c);
        tv_fr_connect_failed(r, err);
        return;
    }
    r->c = c;
    c->r = r;
    /* (connecting here, the pool was empty: the next request finds one waiting) */
    if (!c->connecting) tv_fc_spare(c, r);
    if (!c->connecting) { tv_fc_connected(c); return; }
    /* make the ClientHello (the key shares are the costly part) while TCP connects: it waits
     * in the TLS write buffer until the socket is writable */
    if (r->https && !r->via_proxy) tv_fc_start_tls(c);
}

/* A spare connection to the origin c was connected to, opened by the helper: a request that
 * abandons its response (a media reader that has read what it wants of a range) ends its
 * connection, and its program's next request to that origin, soon after, would connect again;
 * now one is waiting in the pool, connected while the program carried on. Plain HTTP only (not
 * through a proxy), and while the origin has nothing idle. */
static void tv_fc_spare(tv_fc *old, tv_fr *r) {
    tv_origin *o = old->origin;
    if (!tv_fetch_offload || old->tls || r->https || r->via_proxy || r->unix_path.p->len || old->connecting || old->fd < 0) return;
    if (o->nidle + o->sparing >= TV_FC_SPARES) return;
    if (!tv_sk_start()) return;
    tv_fc *c = tv_alloc(sizeof *c);
    memset(c, 0, sizeof *c);
    c->io.ready = tv_fc_ready;
    c->fd = -1;
    c->addrs = old->addrs;
    c->ai = old->ai;
    c->port = old->port;
    c->origin = o;
    c->dialing = true;
    o->sparing++;
    pthread_mutex_lock(&tv_sk_mu);
    c->dial_next = tv_sk_dials;
    tv_sk_dials = c;
    tv_sk_wake();
}

/* The loop: spares the helper opened, into their origins' pools (or let go). */
static bool tv_bk_finish_all(void);

static void tv_sk_ready(tv_io *h, bool readable, bool writable, bool broken) {
    (void)h; (void)readable; (void)writable; (void)broken;
#ifndef TV_KQUEUE
    uint64_t v;
    ssize_t rd = read(tv_sk_efd, &v, sizeof v);
    (void)rd;
#endif
    /* (bodies a thread received are the program's work; spares opened, housekeeping) */
    if (!tv_bk_finish_all()) tv_io_quiet = true;
    pthread_mutex_lock(&tv_sk_mu);
    tv_fc *done = tv_sk_dialed;
    tv_sk_dialed = NULL;
    tv_sk_poked = false;
    pthread_mutex_unlock(&tv_sk_mu);
    for (tv_fc *c = done, *next; c; c = next) {
        next = c->dial_next;
        c->dial_next = NULL;
        c->dialing = false;
        c->origin->sparing--;
        if (!c->dead && c->fd >= 0 && c->dial_rc != 0 && (c->dial_err == EINPROGRESS || c->dial_err == EINTR)) {
            /* (it joins the pool once it's connected: tv_fc_ready) */
            c->spare = true;
            c->connecting = true;
            tv_fc_interest(c, false, true);
            continue;
        }
        if (c->dead || c->fd < 0 || c->dial_rc != 0) {
            if (c->fd >= 0) close(c->fd);
            c->fd = -1;
            c->dead = true;
            c->next = tv_fc_graveyard;
            tv_fc_graveyard = c;
            continue;
        }
        tv_fc_release(c, true);
    }
}

static void tv_dns_resolved(tv_dns *d) {
    if (d->net_waiters && tv_dns_net_done) tv_dns_net_done(d);
    tv_fr *w = d->waiters;
    d->waiters = NULL;
    while (w) {
        tv_fr *next = w->dns_next;
        w->dns_next = NULL;
        if (w->result == 1) {
            if (d->err) tv_fr_fail(w, "ENOTFOUND", "getaddrinfo ENOTFOUND %s", w->dial_host);
            else tv_fr_connect(w, &d->addrs);
        }
        w = next;
    }
}

static void tv_dns_ready(tv_io *h, bool readable, bool writable, bool broken) {
    (void)h; (void)writable; (void)broken;
    if (!readable) return;
    char buf[64];
    while (read(tv_dns_pipe[0], buf, sizeof buf) > 0) {}
    pthread_mutex_lock(&tv_dns_mu);
    tv_dns *done = tv_dns_done;
    tv_dns_done = NULL;
    pthread_mutex_unlock(&tv_dns_mu);
    for (tv_dns *d = done, *next; d; d = next) {
        next = d->job_next;
        d->resolving = false;
        d->expires = tv_loop_update() + (d->err ? 1000 : TV_DNS_TTL_MS);
        tv_io_refs--;
        tv_dns_resolved(d);
    }
}

static bool tv_numeric_host(const char *host, tv_addrs *out) {
    out->n = 0;
    struct sockaddr_in *v4 = (struct sockaddr_in *)&out->a[0];
    memset(v4, 0, sizeof *v4);
    if (inet_pton(AF_INET, host, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        out->len[0] = sizeof *v4;
        out->n = 1;
        return true;
    }
    size_t hl = strlen(host);
    if (hl >= 2 && host[0] == '[' && host[hl - 1] == ']' && hl < 64) {
        char bare[64];
        memcpy(bare, host + 1, hl - 2);
        bare[hl - 2] = 0;
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&out->a[0];
        memset(v6, 0, sizeof *v6);
        if (inet_pton(AF_INET6, bare, &v6->sin6_addr) == 1) {
            v6->sin6_family = AF_INET6;
            out->len[0] = sizeof *v6;
            out->n = 1;
            return true;
        }
    }
    if (strcmp(host, "localhost") == 0) {
        memset(&out->a[0], 0, sizeof out->a[0]);
        memset(&out->a[1], 0, sizeof out->a[1]);
        struct sockaddr_in *a = (struct sockaddr_in *)&out->a[0];
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        out->len[0] = sizeof *a;
        struct sockaddr_in6 *b = (struct sockaddr_in6 *)&out->a[1];
        b->sin6_family = AF_INET6;
        b->sin6_addr = in6addr_loopback;
        out->len[1] = sizeof *b;
        out->n = 2;
        return true;
    }
    return false;
}

/* Sends r on a pooled connection, or looks up its host and connects. */
static void tv_fr_step(tv_fr *r) {
    tv_origin *o = tv_origin_get(r->key);
    while (o->idle) {
        tv_fc *c = o->idle;
        o->idle = c->next;
        o->nidle--;
        if (c->dead) continue;
        /* (the last idle one taken: the next request finds one waiting) */
        tv_fc_spare(c, r);
        tv_fr_send(r, c);
        return;
    }
    tv_addrs addrs;
    if (r->unix_path.p->len) {
        /* Bun's `unix`: the socket, whatever the URL's host */
        struct sockaddr_un *un = (struct sockaddr_un *)&addrs.a[0];
        memset(un, 0, sizeof *un);
        un->sun_family = AF_UNIX;
        if ((size_t)r->unix_path.p->len >= sizeof un->sun_path) { tv_fr_fail(r, "ENAMETOOLONG", "The socket path is too long: %s", r->unix_path.p->data); return; }
        memcpy(un->sun_path, r->unix_path.p->data, (size_t)r->unix_path.p->len);
        addrs.len[0] = (socklen_t)sizeof *un;
        addrs.n = 1;
        tv_fr_connect(r, &addrs);
        return;
    }
    if (tv_numeric_host(r->dial_host, &addrs)) { tv_fr_connect(r, &addrs); return; }
    uint32_t b = tv_hash_cstr(r->dial_host) % TV_DNS_BUCKETS;
    tv_dns *d = tv_dns_cache[b];
    while (d && strcmp(d->host, r->dial_host) != 0) d = d->next;
    if (d && !d->resolving && d->expires > tv_loop_update()) {
        if (d->err) tv_fr_fail(r, "ENOTFOUND", "getaddrinfo ENOTFOUND %s", r->dial_host);
        else tv_fr_connect(r, &d->addrs);
        return;
    }
    if (!d) {
        d = tv_alloc(sizeof *d);
        memset(d, 0, sizeof *d);
        size_t hl = strlen(r->dial_host);
        d->host = tv_alloc(hl + 1);
        memcpy(d->host, r->dial_host, hl + 1);
        d->next = tv_dns_cache[b];
        tv_dns_cache[b] = d;
    }
    r->dns_next = d->waiters;
    d->waiters = r;
    if (!d->resolving) {
        d->resolving = true;
        tv_dns_queue(d);
    }
}

static void tv_fr_reset_response(tv_fr *r) {
    r->state = TV_FR_HEAD;
    r->status = 0;
    r->rbody.len = 0;
    if (r->dec) { tv_codec->close(r->dec); r->dec = NULL; }
    r->raw.len = r->raw_off = 0;
    r->draining = false;
    r->discard = false;
    r->utf8_pos = 0;
    r->utf8_bad = false;
}

/* The whole response is in: follow a redirect, or decode the body and settle. */
static void tv_fr_complete(tv_fr *r) {
    int s = r->status;
    if (r->redirect_mode != TV_FETCH_MANUAL && tv_redirect_status(s) && r->location.len > 0) {
        if (r->redirect_mode == TV_FETCH_ERROR) {
            tv_fr_fail(r, "UnexpectedRedirect", "UnexpectedRedirect fetching \"%s\". %s", r->url.p->data, TV_FETCH_VERBOSE);
            return;
        }
        if (++r->redirects > 20) {
            tv_fr_fail(r, "TooManyRedirects", "The response redirected too many times. %s", TV_FETCH_VERBOSE);
            return;
        }
        tv_str loc = tv_str_from(r->location.data, r->location.len);
        tv_str next = tv_native_urlNormalize(loc, r->url);
        tv_str_release(loc);
        if (next.p->len == 0) {
            tv_fr_fail(r, "InvalidRedirectURL", "The redirect URL \"%.*s\" is invalid", (int)r->location.len, r->location.data);
            return;
        }
        const char *m = r->method.p->data;
        if ((s == 303 && strcmp(m, "GET") != 0 && strcmp(m, "HEAD") != 0) || ((s == 301 || s == 302) && strcmp(m, "POST") == 0)) {
            tv_str_release(r->method);
            r->method = tv_str_from("GET", 3);
            tv_str_release(r->body);
            r->body = TV_EMPTY_STR;
            static const char *const body_headers[] = {"content-type", "content-length", "content-encoding", "content-language", "content-location", "transfer-encoding"};
            for (size_t i = 0; i < sizeof body_headers / sizeof *body_headers; i++) {
                tv_str name = tv_str_from(body_headers[i], strlen(body_headers[i]));
                tv_str h2 = tv_native_headerRemove(r->headers, name);
                tv_str_release(r->headers);
                tv_str_release(name);
                r->headers = h2;
            }
        }
        /* another origin doesn't get the credentials */
        char *old_key = r->key;
        r->key = NULL;
        tv_str_release(r->url);
        r->url = next;
        if (!tv_fr_prepare(r)) { tv_free(old_key); return; }
        if (strcmp(old_key, r->key) != 0) {
            static const char *const creds[] = {"authorization", "proxy-authorization", "cookie"};
            bool removed = false;
            for (size_t i = 0; i < 3; i++) {
                if (!tv_hdr_has(r->headers, creds[i])) continue;
                tv_str name = tv_str_from(creds[i], strlen(creds[i]));
                tv_str h2 = tv_native_headerRemove(r->headers, name);
                tv_str_release(r->headers);
                tv_str_release(name);
                r->headers = h2;
                removed = true;
            }
            if (removed) tv_fr_prepare(r);
        }
        tv_free(old_key);
        r->redirected = true;
        r->retried = false;
        tv_fr_reset_response(r);
        tv_fr_step(r);
        return;
    }
    if (tv_fr_decoding(r) && (r->dec || r->raw.len)) {
        /* the rest of the body (checked as UTF-8 as it's decoded) */
        if (r->whole) {
            if (!tv_fr_decode_whole(r)) return;
        } else {
            if (r->streaming && !tv_fr_decode(r, false)) return;
            if (!r->streaming || r->raw_off < r->raw.len) {
                r->draining = true; /* decoded as it's read */
                if (r->read_p && r->rbody.len) tv_fr_resolve(&r->read_p, (tv_int)r->rbody.len);
                return;
            }
            if (!tv_fr_decode(r, true)) return;
        }
    } else if (r->enc && !tv_fr_decoding(r)) {
        r->utf8_bad = true; /* not decoded (decompress: false): text() checks it */
    }
    tv_fr_settle(r, 0);
}

/* ---- big bodies, received on threads of their own

   The rest of a big body read whole, its length known (`(await fetch(url)).text()` of
   megabytes), is received on a thread of its own, straight into the buffer it becomes: several
   downloads copy out of the kernel at once (on Linux the copy is the whole cost), and the loop
   goes on meanwhile. The job holds the connection and the buffer while it runs: the request has
   neither (an abort only shuts the socket, ending the read sooner) and gets them back when
   it's done (tv_bk_finish, on the loop). Plain HTTP: a TLS connection's state stays on the
   loop. On Linux: 8 downloads of 8 MB at once, 0.7k/s to 1.5k/s (Bun 1.1k); macOS's loopback
   copies cheaply enough that the hand-off costs more than it saves (one download, 92 ms per
   100 to 130). */
#define TV_BK_MIN ((size_t)1 << 20)
#define TV_BK_THREADS 8
typedef struct tv_bk {
    tv_int id;            /* the request's; 0 once it was aborted (or freed) meanwhile */
    tv_fc *c;
    int fd;
    tv_sb buf;            /* the request's sink, moved out of it */
    bool raw;             /* that sink was r->raw (an encoded body), not r->rbody */
    size_t want, got;     /* bytes to receive, received (fewer: the peer closed, or err) */
    int err;
    bool check, bad;      /* checked as UTF-8 as it arrives (see tv_fr_check_utf8): from pos */
    size_t pos;
    struct tv_bk *next;
} tv_bk;
static pthread_mutex_t tv_bk_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tv_bk_cv = PTHREAD_COND_INITIALIZER;
static tv_bk *tv_bk_jobs, *tv_bk_done;
static int tv_bk_threads, tv_bk_idle;

#ifdef __linux__
static void *tv_bk_run(void *arg) {
    (void)arg;
    pthread_mutex_lock(&tv_bk_mu);
    for (;;) {
        while (!tv_bk_jobs) {
            tv_bk_idle++;
            pthread_cond_wait(&tv_bk_cv, &tv_bk_mu);
            tv_bk_idle--;
        }
        tv_bk *j = tv_bk_jobs;
        tv_bk_jobs = j->next;
        pthread_mutex_unlock(&tv_bk_mu);
        /* (blocking for the job, the kernel filling the buffer in one call; non-blocking again
         * for the loop, after) */
        char *d = j->buf.data + j->buf.len;
        int fl = fcntl(j->fd, F_GETFL);
        fcntl(j->fd, F_SETFL, fl & ~O_NONBLOCK);
        while (j->got < j->want) {
            /* (a piece at a time, checked as UTF-8 while it's still in cache) */
            size_t ask = j->want - j->got < ((size_t)256 << 10) ? j->want - j->got : (size_t)256 << 10;
            ssize_t n = recv(j->fd, d + j->got, ask, MSG_WAITALL);
            if (n > 0) {
                j->got += (size_t)n;
                if (j->check && !j->bad) j->bad = !tv_utf8_scan((const uint8_t *)j->buf.data, j->buf.len + j->got, &j->pos);
                continue;
            }
            if (n == 0) break;
            if (errno == EINTR) continue;
            j->err = errno;
            break;
        }
        fcntl(j->fd, F_SETFL, fl);
        pthread_mutex_lock(&tv_bk_mu);
        bool poke = !tv_bk_done;
        j->next = tv_bk_done;
        tv_bk_done = j;
        pthread_mutex_unlock(&tv_bk_mu);
        if (poke) tv_sk_wake_loop();
        pthread_mutex_lock(&tv_bk_mu);
    }
    return NULL;
}

/* Hands the rest of r's body (c's to receive, room for all of it in its sink) to a thread:
   false if it can't start one (the loop receives it, as before). */
static bool tv_bk_start(tv_fr *r, tv_fc *c) {
    if (!tv_sk_start()) return false;
    pthread_mutex_lock(&tv_bk_mu);
    if (tv_bk_idle == 0 && tv_bk_threads < TV_BK_THREADS) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64 * 1024 > PTHREAD_STACK_MIN ? 64 * 1024 : PTHREAD_STACK_MIN);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t t;
        bool ok = pthread_create(&t, &attr, tv_bk_run, NULL) == 0;
        pthread_attr_destroy(&attr);
        if (ok) tv_bk_threads++;
        else if (tv_bk_threads == 0) {
            pthread_mutex_unlock(&tv_bk_mu);
            return false;
        }
    }
    tv_bk *j = tv_alloc(sizeof *j);
    memset(j, 0, sizeof *j);
    j->id = r->id;
    j->c = c;
    j->fd = c->fd;
    j->raw = tv_fr_decoding(r);
    j->check = !j->raw && !r->enc && !r->utf8_bad;
    j->pos = r->utf8_pos;
    tv_sb *sink = tv_fr_sink(r);
    j->buf = *sink;
    sink->data = NULL;
    sink->len = sink->cap = 0;
    j->want = (size_t)r->remaining;
    r->bulk = j;
    r->c = NULL;
    c->r = NULL;
    /* (out of the loop's queue meanwhile: epoll reports a hang-up even with no interest) */
    c->bulk = true;
#ifdef TV_KQUEUE
    tv_fc_interest(c, false, false);
#else
    if (c->registered) epoll_ctl(tv_loop_queue(), EPOLL_CTL_DEL, c->fd, NULL);
    c->registered = c->reading = c->writing = false;
#endif
    /* (the loop waits for it, even past an abort: it hands back the connection and buffer) */
    tv_io_refs++;
    j->next = tv_bk_jobs;
    tv_bk_jobs = j;
    pthread_cond_signal(&tv_bk_cv);
    pthread_mutex_unlock(&tv_bk_mu);
    return true;
}
#endif

/* An abort while a thread receives r's body: the socket is shut so its read ends; the job finds
   the request gone when it's done. */
static void tv_bk_abandon(tv_fr *r) {
    if (!r->bulk) return;
    r->bulk->id = 0;
    shutdown(r->bulk->fd, SHUT_RDWR);
    r->bulk = NULL;
}

static tv_fr *tv_fr_get(tv_int id);

/* (on the loop) A thread is done with its body: back into the request, which completes (or
   fails, cut short), as if the loop had received it. */
static void tv_bk_finish(tv_bk *j) {
    tv_io_refs--;
    tv_fr *r = j->id ? tv_fr_get(j->id) : NULL;
    tv_fc *c = j->c;
    c->bulk = false;
    if (!r || r->result != 1 || r->bulk != j) {
        tv_sb_free(&j->buf);
        tv_fc_close(c);
        tv_free(j);
        return;
    }
    r->bulk = NULL;
    tv_sb *sink = j->raw ? &r->raw : &r->rbody;
    *sink = j->buf;
    sink->len += j->got;
    r->remaining -= (int64_t)j->got;
    if (j->got) r->got_any = true;
    if (j->check) {
        r->utf8_bad = j->bad;
        r->utf8_pos = j->pos;
    }
    bool complete = j->got == j->want;
    tv_free(j);
    if (!tv_fr_decoding(r)) tv_fr_check_utf8(r);
    if (!complete) {
        tv_fc_close(c);
        tv_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", TV_FETCH_VERBOSE);
        return;
    }
    r->state = TV_FR_DONE;
    tv_fc_release(c, r->keep);
    tv_fr_complete(r);
}

/* (false if there were none) */
static bool tv_bk_finish_all(void) {
    if (!tv_bk_threads) return false;
    pthread_mutex_lock(&tv_bk_mu);
    tv_bk *done = tv_bk_done;
    tv_bk_done = NULL;
    pthread_mutex_unlock(&tv_bk_mu);
    if (!done) return false;
    while (done) {
        tv_bk *j = done;
        done = j->next;
        tv_bk_finish(j);
    }
    return true;
}

static void tv_fc_ready(tv_io *h, bool readable, bool writable, bool broken) {
    tv_fc *c = (tv_fc *)h;
    if (c->dead || c->bulk) return;
    tv_fr *r = c->r;
    if (!r) {
        tv_io_quiet = true;
        if (c->spare) {
            /* a spare connection, connected (or not) */
            if (!writable && !broken) return;
            int err = 0;
            socklen_t el = sizeof err;
            if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0) err = errno;
            c->spare = false;
            c->connecting = false;
            if (err || broken) { tv_fc_close(c); return; }
            tv_fc_release(c, true);
            return;
        }
        /* idle: the server closed it (or sent something unasked for) */
        if (readable || broken) {
            if (c->tls && !broken) {
                /* a late TLS 1.3 session ticket arrives this way: that's all right */
                char b;
                if (tv_tls_impl->read(c->tls, &b, 1) == -1) return;
            }
            tv_origin_unlink(c);
            tv_fc_close(c);
        }
        return;
    }
    if (c->connecting) {
        if (!writable && !readable && !broken) return;
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0) err = errno;
        if (!err) err = c->early_err;
        if (!err && c->ai > 0) {
            /* an event left over from an address given up on: this one may still be connecting */
            struct sockaddr_storage peer;
            socklen_t pl = sizeof peer;
            if (getpeername(c->fd, (struct sockaddr *)&peer, &pl) != 0) {
                if (errno == ENOTCONN) return;
                err = errno;
            }
        }
        if (err) {
            if (c->tls) { tv_tls_impl->close(c->tls, false); c->tls = NULL; c->handshaking = false; c->early_err = 0; }
            close(c->fd);
            c->fd = -1;
            c->ai++;
            if (tv_fc_open(c)) {
                if (!c->connecting) tv_fc_connected(c);
                return;
            }
            c->r = NULL;
            r->c = NULL;
            tv_fc_close(c);
            tv_fr_connect_failed(r, c->err ? c->err : err);
            return;
        }
        c->connecting = false;
        tv_fc_connected(c);
        return;
    }
    if (c->tunneling) {
        if (broken) { tv_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", TV_FETCH_VERBOSE); return; }
        tv_fc_tunnel(c, readable, writable);
        return;
    }
    if (c->handshaking) {
        if (readable || writable || broken) tv_fc_handshake(c);
        return;
    }
    if (writable && r->sent < tv_fr_out_len(r)) {
        if (!tv_fc_write(c)) {
            bool retry = c->reused && !r->retried && !r->got_any;
            c->r = NULL;
            r->c = NULL;
            tv_fc_close(c);
            if (retry) { r->retried = true; tv_fr_step(r); }
            else tv_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", TV_FETCH_VERBOSE);
            return;
        }
    }
    if (!readable && !broken) return;
    bool eof = false, error = false;
    for (;;) {
        /* a known-length body goes straight into its buffer */
        if (r->state == TV_FR_FIXED && c->in_off == c->in_len && !r->discard) {
            size_t step = r->whole ? (size_t)1 << 20 : r->streaming ? (size_t)64 << 10 : TV_STREAM_HIGH;
            size_t want = (size_t)r->remaining < step ? (size_t)r->remaining : step;
            tv_sb *sink = tv_fr_sink(r);
            if (sink->cap - sink->len < want) {
                /* read whole, its length known: room for the rest at once (a buffer grown by
                 * doubling copies itself each time, ends up to twice the size, and doesn't fit
                 * the string it becomes, or the next body's, which tv_sb_map would otherwise
                 * reuse) */
                size_t rest = (size_t)r->remaining;
                bool all = r->whole && rest <= ((size_t)256 << 20);
                tv_sb_grow(sink, sink->len + (all ? rest : want));
            }
#ifdef __linux__
            /* (a big rest, room for all of it: received on a thread, see tv_bk_start) */
            if (r->whole && !c->tls && (size_t)r->remaining >= TV_BK_MIN && sink->cap - sink->len >= (size_t)r->remaining && tv_bk_start(r, c)) return;
#endif
            long n = tv_fc_recv(c, sink->data + sink->len, want);
            if (n > 0) {
                r->got_any = true;
                sink->len += (size_t)n;
                r->remaining -= n;
                if (!tv_fr_decoding(r)) tv_fr_check_utf8(r);
                bool full = tv_fr_data(r);
                if (r->result != 1) return; /* the body couldn't be decoded */
                if (r->remaining == 0) { r->state = TV_FR_DONE; break; }
                if (full) { tv_fr_pause(r, c); break; }
                /* a stream's reader gets each 64 KB as it arrives, before more is read (the socket
                 * raises its next event for the rest; TLS's buffered records wouldn't) */
                if (r->streaming && !c->tls && r->rbody.len >= ((size_t)64 << 10)) break;
                /* TLS returns a record at a time: read until it has no more */
                if ((size_t)n == want || c->tls) continue;
                break;
            }
            if (n == 0) { eof = true; break; }
            if (n == -2) { tv_fc_interest(c, true, true); break; }
            if (n == -3) error = true;
            break;
        }
        if (c->in_off == c->in_len) c->in_off = c->in_len = 0;
        if (c->in_cap - c->in_len < 16384) {
            if (c->in_off) {
                memmove(c->in, c->in + c->in_off, c->in_len - c->in_off);
                c->in_len -= c->in_off;
                c->in_off = 0;
            }
            if (c->in_cap - c->in_len < 16384) {
                size_t cap = c->in_cap ? c->in_cap * 2 : TV_FC_RBUF;
                c->in = c->in ? tv_realloc(c->in, cap) : tv_fc_rbuf();
                c->in_cap = cap;
            }
        }
        size_t room = c->in_cap - c->in_len;
        long n = tv_fc_recv(c, c->in + c->in_len, room);
        if (n > 0) {
            r->got_any = true;
            c->in_len += (size_t)n;
            int st = tv_fr_parse(r, c);
            if (r->result != 1) return;
            if (st < 0) {
                tv_fr_fail(r, "Malformed_HTTP_Response", "Malformed_HTTP_Response fetching \"%s\". %s", r->url.p->data, TV_FETCH_VERBOSE);
                return;
            }
            if (st > 0) break;
            if (r->streaming ? r->rbody.len >= TV_STREAM_HIGH || r->raw_off < r->raw.len : !r->whole && tv_fr_sink(r)->len >= TV_STREAM_HIGH) {
                tv_fr_pause(r, c);
                break;
            }
            if ((size_t)n == room || c->tls) continue;
            break;
        }
        if (n == 0) { eof = true; break; }
        if (n == -2) { tv_fc_interest(c, true, true); break; }
        if (n == -3) error = true;
        break;
    }
    if (r->state == TV_FR_DONE) {
        bool leftover = c->in_off < c->in_len;
        r->c = NULL;
        tv_fc_release(c, r->keep && !leftover && !eof && !error);
        tv_fr_complete(r);
        return;
    }
    if (eof || error) {
        if (r->state == TV_FR_UNTIL_CLOSE && eof) {
            r->state = TV_FR_DONE;
            r->c = NULL;
            tv_fc_release(c, false);
            tv_fr_complete(r);
            return;
        }
        bool retry = c->reused && !r->retried && !r->got_any;
        c->r = NULL;
        r->c = NULL;
        tv_fc_close(c);
        if (retry) { r->retried = true; tv_fr_reset_response(r); tv_fr_step(r); return; }
        tv_fr_fail(r, "ECONNRESET", "The socket connection was closed unexpectedly. %s", TV_FETCH_VERBOSE);
    }
}

/* ---- after fork (a server worker): the parent's connections, lookups and threads aren't ours */

static void tv_fetch_after_fork(void) {
    if (tv_sk_started) {
        /* the helper isn't the child's: the next one starts it again */
        pthread_mutex_init(&tv_sk_mu, NULL);
        pthread_cond_init(&tv_sk_cv, NULL);
        tv_sk_started = tv_sk_idle = tv_sk_poked = false;
        tv_sk_nclose = 0;
        tv_sk_dials = tv_sk_dialed = NULL;
#ifndef TV_KQUEUE
        if (tv_sk_efd >= 0) close(tv_sk_efd);
        tv_sk_efd = -1;
#endif
    }
    /* (the parent's body threads aren't the child's; nor are their jobs) */
    pthread_mutex_init(&tv_bk_mu, NULL);
    pthread_cond_init(&tv_bk_cv, NULL);
    tv_bk_threads = tv_bk_idle = 0;
    tv_bk_jobs = tv_bk_done = NULL;
    pthread_mutex_init(&tv_dns_mu, NULL);
    pthread_cond_init(&tv_dns_cv, NULL);
    tv_dns_threads = tv_dns_idle = 0;
    tv_dns_jobs = tv_dns_done = NULL;
    if (tv_dns_pipe[0] >= 0) { close(tv_dns_pipe[0]); close(tv_dns_pipe[1]); tv_dns_pipe[0] = tv_dns_pipe[1] = -1; }
    for (int b = 0; b < TV_DNS_BUCKETS; b++) tv_dns_cache[b] = NULL;
    for (int b = 0; b < TV_ORIGIN_BUCKETS; b++) {
        for (tv_origin *o = tv_origins[b]; o; o = o->next) {
            for (tv_fc *c = o->idle; c; c = c->next) if (c->fd >= 0) close(c->fd);
            o->idle = NULL;
            o->nidle = 0;
        }
    }
    for (tv_int i = 0; i < tv_fr_cap; i++) {
        tv_fr *r = tv_fr_table[i];
        if (r && r->result == 1) {
            if (r->c && r->c->fd >= 0) { close(r->c->fd); r->c->fd = -1; }
            r->c = NULL;
            r->result = -1;
            r->code = "ECONNRESET";
        }
    }
    tv_io_refs = 0;
}

/* ---- natives */

static tv_fr *tv_fr_get(tv_int id) {
    tv_int i = (id & 0xffffffff) - 1;
    if (i < 0 || i >= tv_fr_cap) return NULL;
    tv_fr *r = tv_fr_table[i];
    return r && r->id == id ? r : NULL;
}

tv_int tv_native_fetchStart(tv_str method, tv_str url, tv_str headers, tv_str body, tv_int redirect, tv_int flags, tv_str ca, tv_str unix_path, tv_str proxy) {
    tv_io_after_batch = tv_fc_reap;
    tv_io_after_fork = tv_fetch_after_fork;
    static bool sigpipe_ignored;   /* (once: it's a syscall) */
    if (!sigpipe_ignored) {
        signal(SIGPIPE, SIG_IGN);
        sigpipe_ignored = true;
    }
    if (tv_fr_nfree == 0) {
        tv_int cap = tv_fr_cap ? tv_fr_cap * 2 : 16;
        tv_fr_table = tv_realloc(tv_fr_table, (size_t)cap * sizeof *tv_fr_table);
        tv_fr_free = tv_realloc(tv_fr_free, (size_t)cap * sizeof *tv_fr_free);
        for (tv_int i = cap - 1; i >= tv_fr_cap; i--) { tv_fr_table[i] = NULL; tv_fr_free[tv_fr_nfree++] = i; }
        tv_fr_cap = cap;
    }
    tv_int slot = tv_fr_free[--tv_fr_nfree];
    tv_fr *r = tv_alloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->id = (tv_int)(((uint64_t)++tv_fr_gen << 32) | (uint64_t)(slot + 1));
    tv_fr_table[slot] = r;
    r->result = 1;
    r->head_state = 1;
    tv_io_refs++;
    tv_str_retain(method);
    tv_str_retain(headers);
    tv_str_retain(body);
    r->method = method;
    r->headers = headers;
    r->body = body;
    r->redirect_mode = (int)redirect;
    r->decompress = (flags & TV_FETCH_DECOMPRESS) != 0;
    r->insecure = (flags & TV_FETCH_INSECURE) != 0;
    tv_str_retain(ca);
    r->ca = ca;
    tv_str_retain(unix_path);
    r->unix_path = unix_path;
    tv_str_retain(proxy);
    r->proxy = proxy;
    r->url = tv_native_urlNormalize(url, TV_EMPTY_STR);
    if (r->url.p->len == 0) {
        tv_fr_fail(r, "ERR_INVALID_URL", "fetch() URL is invalid");
        return r->id;
    }
    /* the fragment isn't sent, and isn't part of the response URL */
    const char *hash = memchr(r->url.p->data, '#', (size_t)r->url.p->len);
    if (hash) {
        tv_str cut = tv_str_from(r->url.p->data, (size_t)(hash - r->url.p->data));
        tv_str_release(r->url);
        r->url = cut;
    }
    if (tv_fr_prepare(r)) tv_fr_step(r);
    return r->id;
}

/* A Promise<int>: settled with `now` unless `pending`, else kept in *slot until it settles. */
static tv_promise *tv_fr_promise(tv_promise **slot, bool pending, tv_int now) {
    tv_promise *p = tv_promise_new(&tv_type_int);
    if (!pending || !slot) {
        tv_promise_resolve(p, &now);
        return p;
    }
    if (*slot) tv_promise_release(*slot); /* a reader asking again: the new promise replaces it */
    tv_promise_retain(p);
    *slot = p;
    return p;
}

/* A body that has all arrived is still being decoded: decode more (all of it for a whole-body
 * read), and settle once it's done. */
static void tv_fr_drain(tv_fr *r, bool all) {
    if (!r->draining || r->result != 1) return;
    if (all) {
        if (!tv_fr_decode_whole(r)) return;
    } else {
        if (!tv_fr_decode(r, false)) return;
        if (r->raw_off < r->raw.len) return;
        if (!tv_fr_decode(r, true)) return; /* complete? */
    }
    r->draining = false;
    tv_fr_settle(r, 0);
}

/* The response head arrived (0), or the request failed (< 0). */
tv_promise *tv_native_fetchWait(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    return tv_fr_promise(r ? &r->done : NULL, r && r->head_state == 1, r ? r->head_state : -1);
}

/* The whole body is in (0), or it failed (< 0). */
tv_promise *tv_native_fetchBodyWait(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    if (r && !r->whole) {
        /* read whole: decoded at once when it's all in (now, if it is) */
        r->whole = true;
        tv_fr_drain(r, true);
        if (r->paused && r->c && !r->streaming) {
            /* it waited, unread, for this: the rest comes now */
            r->paused = false;
            tv_fc *c = r->c;
            tv_fc_interest(c, true, false);
            tv_fc_ready(&c->io, true, false, false);
        }
    }
    return tv_fr_promise(r ? &r->body_p : NULL, r && r->result == 1, r ? r->result : -1);
}

/* Streaming (res.body): bytes to take (> 0), the end (0), or a failure (< 0). */
tv_promise *tv_native_fetchRead(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    if (!r) return tv_fr_promise(NULL, false, -1);
    r->streaming = true;
    /* what arrived before reading began */
    if (!r->rbody.len && r->draining) tv_fr_drain(r, false);
    else if (!r->rbody.len && r->raw_off < r->raw.len && r->result == 1) tv_fr_decode(r, false);
    if (r->rbody.len) return tv_fr_promise(NULL, false, (tv_int)r->rbody.len);
    return tv_fr_promise(&r->read_p, r->result == 1, r->result);
}

/* The body bytes that have arrived and haven't been taken (*len of them; NULL if none): read
 * them, then tv_native_fetchTaken. */
const char *tv_native_fetchData(tv_int id, size_t *len) {
    tv_fr *r = tv_fr_get(id);
    *len = r ? r->rbody.len : 0;
    return r && r->rbody.len ? r->rbody.data : NULL;
}

/* The body bytes that have arrived (and haven't been taken). */
tv_arr tv_native_fetchTake(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    if (!r) return tv_arr_fresh();
    tv_arr a = tv_arr_with_capacity(&tv_type_u8, (tv_int)r->rbody.len);
    if (r->rbody.len) memcpy(tv_arr_data(a), r->rbody.data, r->rbody.len);
    a.p->len = (tv_int)r->rbody.len;
    tv_native_fetchTaken(id);
    return a;
}

/* What tv_native_fetchData gave has been read: room for more. */
void tv_native_fetchTaken(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    if (!r) return;
    r->rbody.len = 0;
    /* (the whole body has been taken: its buffer goes now) */
    if (r->result != 1 && !r->draining && r->raw_off >= r->raw.len) {
        tv_sb_free(&r->rbody);
        return;
    }
    /* decode what waited for room (while it's still more than the reader wants, stay paused) */
    if (r->draining) { tv_fr_drain(r, false); return; }
    if (r->raw_off < r->raw.len && r->result == 1 && (!tv_fr_decode(r, false) || r->raw_off < r->raw.len)) return;
    if (r->paused && r->c) {
        /* the reader caught up: read on (what's buffered in TLS or unparsed won't raise an event) */
        r->paused = false;
        tv_fc *c = r->c;
        tv_fc_interest(c, true, false);
        tv_fc_ready(&c->io, true, false, false);
    }
}

/* Response.clone(): one more reader will take the whole body. */
void tv_native_fetchShare(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    if (r) r->shares++;
}

static void tv_fetch_handle_release(void *p) { tv_native_fetchFree(*(tv_int *)p); }
static const tv_type tv_type_fetch_handle = { sizeof(tv_int), NULL, tv_fetch_handle_release, NULL, NULL, NULL, NULL };

/* A Response's hold on request `id`: a settled promise whose release (the Response going away)
 * frees the request, and aborts it if the body is still arriving. */
tv_promise *tv_native_fetchHandle(tv_int id) {
    tv_promise *p = tv_promise_new(&tv_type_fetch_handle);
    tv_promise_resolve(p, &id);
    return p;
}

tv_int tv_native_fetchStatus(tv_int id) { tv_fr *r = tv_fr_get(id); return r ? r->status : 0; }
tv_str tv_native_fetchStatusText(tv_int id) { tv_fr *r = tv_fr_get(id); return r ? tv_str_from(r->status_text.data, r->status_text.len) : TV_EMPTY_STR; }
tv_str tv_native_fetchHeaders(tv_int id) { tv_fr *r = tv_fr_get(id); return r ? tv_str_from(r->rheaders.data, r->rheaders.len) : TV_EMPTY_STR; }
tv_str tv_native_fetchUrl(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    if (!r) return TV_EMPTY_STR;
    tv_str_retain(r->url);
    return r->url;
}
bool tv_native_fetchRedirected(tv_int id) { tv_fr *r = tv_fr_get(id); return r && r->redirected; }
/* The body is valid UTF-8 without a BOM: text() can return it as it is. */
bool tv_native_fetchBodyClean(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    if (!r || r->utf8_bad || r->utf8_pos != r->rbody.len) return false;
    const uint8_t *p = (const uint8_t *)r->rbody.data;
    return !(r->rbody.len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF);
}

tv_str tv_native_fetchBody(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    if (!r) return TV_EMPTY_STR;
    if (r->shares > 0) { /* a clone reads it too */
        r->shares--;
        return tv_str_from(r->rbody.data, r->rbody.len);
    }
    return tv_str_from_sb(&r->rbody);
}
tv_str tv_native_fetchErrorCode(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    return r && r->code ? tv_str_from(r->code, strlen(r->code)) : TV_EMPTY_STR;
}
tv_str tv_native_fetchErrorMessage(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    return r ? tv_str_from(r->message.data, r->message.len) : TV_EMPTY_STR;
}

/* Abandons a request in flight (AbortSignal): its connection is closed, and fetchWait's promise
 * settles with -2. */
void tv_native_fetchAbort(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    if (!r || r->result != 1) return;
    tv_bk_abandon(r);
    if (r->c) {
        tv_fc *c = r->c;
        r->c = NULL;
        c->r = NULL;
        tv_fc_spare(c, r);
        tv_fc_close(c);
    }
    /* a lookup keeps going (others may want it); this request stops waiting for it */
    for (int b = 0; b < TV_DNS_BUCKETS; b++)
        for (tv_dns *d = tv_dns_cache[b]; d; d = d->next)
            for (tv_fr **pp = &d->waiters; *pp; pp = &(*pp)->dns_next)
                if (*pp == r) { *pp = r->dns_next; goto unlinked; }
unlinked:
    r->code = "AbortError";
    tv_fr_settle(r, -2);
    /* (nothing will read what arrived: its buffers go now, not when the request is freed) */
    tv_sb_free(&r->rbody);
    tv_sb_free(&r->raw);
    r->raw_off = 0;
}

void tv_native_fetchFree(tv_int id) {
    tv_fr *r = tv_fr_get(id);
    if (!r) return;
    if (r->result == 1) tv_native_fetchAbort(id);
    tv_int slot = (id & 0xffffffff) - 1;
    tv_fr_table[slot] = NULL;
    tv_fr_free[tv_fr_nfree++] = slot;
    if (r->done) tv_promise_release(r->done);
    if (r->body_p) tv_promise_release(r->body_p);
    if (r->read_p) tv_promise_release(r->read_p);
    tv_str_release(r->method);
    tv_str_release(r->headers);
    tv_str_release(r->body);
    tv_str_release(r->url);
    tv_str_release(r->ca);
    tv_str_release(r->unix_path);
    tv_str_release(r->proxy);
    tv_free(r->dial_host);
    tv_free(r->proxy_auth);
    tv_free(r->host);
    tv_free(r->key);
    tv_sb_free(&r->head);
    tv_sb_free(&r->status_text);
    tv_sb_free(&r->rheaders);
    tv_sb_free(&r->rbody);
    if (r->dec) tv_codec->close(r->dec);
    tv_sb_free(&r->raw);
    tv_sb_free(&r->location);
    tv_sb_free(&r->message);
    tv_free(r);
}

/* ---- bodies: bytes <-> strings, and text() decoding */

tv_str tv_native_bytesToString(tv_arr bytes) {
    return tv_str_from((const char *)tv_arr_data(bytes), (size_t)bytes.p->len);
}

tv_arr tv_native_stringToBytes(tv_str s) {
    tv_arr a = tv_arr_with_capacity(&tv_type_u8, s.p->len);
    if (s.p->len) memcpy(tv_arr_data(a), s.p->data, (size_t)s.p->len);
    a.p->len = s.p->len;
    return a;
}

/* WHATWG "UTF-8 decode": drops a leading BOM and replaces each maximal invalid subsequence
 * with U+FFFD. Returns s itself when it is already clean. */
tv_str tv_native_utf8Clean(tv_str s) {
    const uint8_t *p = (const uint8_t *)s.p->data;
    size_t n = (size_t)s.p->len, i = 0;
    bool bom = n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF;
    if (bom) i = 3;
    /* validate first: most bodies are fine */
    size_t j = i;
    bool valid = tv_utf8_scan(p, n, &j) && j == n;
    if (valid) {
        if (!bom) { tv_str_retain(s); return s; }
        return tv_str_from((const char *)p + 3, n - 3);
    }
    tv_sb out = {0};
    tv_sb_grow(&out, n + 16);
    tv_sb_push(&out, (const char *)p + i, j - i);
    static const char fffd[3] = {(char)0xEF, (char)0xBF, (char)0xBD};
    i = j;
    while (i < n) {
        uint8_t b = p[i];
        if (b < 0x80) { tv_sb_push_char(&out, (char)b); i++; continue; }
        size_t need = b >= 0xC2 && b <= 0xDF ? 1 : b >= 0xE0 && b <= 0xEF ? 2 : b >= 0xF0 && b <= 0xF4 ? 3 : 0;
        if (!need) { tv_sb_push(&out, fffd, 3); i++; continue; }
        uint8_t lo = b == 0xE0 ? 0xA0 : b == 0xF0 ? 0x90 : 0x80, hi = b == 0xED ? 0x9F : b == 0xF4 ? 0x8F : 0xBF;
        size_t k = 1;
        while (k <= need && i + k < n) {
            uint8_t c = p[i + k];
            if (c < lo || c > hi) break;
            lo = 0x80;
            hi = 0xBF;
            k++;
        }
        if (k == need + 1) { tv_sb_push(&out, (const char *)p + i, k); i += k; }
        else { tv_sb_push(&out, fffd, 3); i += k; } /* the maximal subpart becomes one U+FFFD */
    }
    return tv_str_from_sb(&out);
}

/* ---- Headers: combined values and sorted entries, as the Fetch standard defines them */

/* Every `name` line's value, joined with ", " ("" if none). */
tv_str tv_native_headerGet(tv_str block, tv_str name) {
    const char *s = block.p->data, *end = s + block.p->len;
    size_t n = (size_t)name.p->len;
    tv_sb out = {0};
    int found = 0;
    const char *first = NULL;
    size_t first_len = 0;
    while (s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        const char *le = nl ? nl : end;
        if ((size_t)(le - s) > n && strncasecmp(s, name.p->data, n) == 0 && s[n] == ':') {
            const char *v = s + n + 1, *ve = le;
            if (ve > v && ve[-1] == '\r') ve--;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            if (found == 0) { first = v; first_len = (size_t)(ve - v); }
            else {
                if (found == 1) tv_sb_push(&out, first, first_len);
                tv_sb_push(&out, ", ", 2);
                tv_sb_push(&out, v, (size_t)(ve - v));
            }
            found++;
        }
        if (!nl) break;
        s = nl + 1;
    }
    if (found == 1) return tv_str_from(first, first_len);
    return tv_str_from_sb(&out);
}

typedef struct { const char *name, *value; size_t nlen, vlen, order; } tv_hdr_line;

static int tv_hdr_cmp(const void *a, const void *b) {
    const tv_hdr_line *x = a, *y = b;
    size_t n = x->nlen < y->nlen ? x->nlen : y->nlen;
    int c = strncasecmp(x->name, y->name, n);
    if (c) return c;
    if (x->nlen != y->nlen) return x->nlen < y->nlen ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

/* [name, value, name, value, ...]: names lower-cased and sorted, repeated names combined
 * (except set-cookie, one entry per cookie). */
tv_arr tv_native_headerEntries(tv_str block) {
    const char *s = block.p->data, *end = s + block.p->len;
    size_t cap = 16, n = 0;
    tv_hdr_line *lines = tv_alloc(cap * sizeof *lines);
    while (s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        const char *le = nl ? nl : end;
        const char *colon = memchr(s, ':', (size_t)(le - s));
        if (colon && colon > s) {
            const char *v = colon + 1, *ve = le;
            if (ve > v && ve[-1] == '\r') ve--;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            if (n == cap) { cap *= 2; lines = tv_realloc(lines, cap * sizeof *lines); }
            lines[n] = (tv_hdr_line){ s, v, (size_t)(colon - s), (size_t)(ve - v), n };
            n++;
        }
        if (!nl) break;
        s = nl + 1;
    }
    qsort(lines, n, sizeof *lines, tv_hdr_cmp);
    tv_arr out = tv_arr_with_capacity(&tv_type_str, (tv_int)(n * 2));
    tv_str *o = tv_arr_data(out);
    size_t k = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        bool cookie = lines[i].nlen == 10 && strncasecmp(lines[i].name, "set-cookie", 10) == 0;
        if (!cookie)
            while (j < n && lines[j].nlen == lines[i].nlen && strncasecmp(lines[j].name, lines[i].name, lines[i].nlen) == 0) j++;
        char *lower = tv_alloc(lines[i].nlen);
        for (size_t c = 0; c < lines[i].nlen; c++) { char ch = lines[i].name[c]; lower[c] = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); }
        o[k++] = tv_str_from(lower, lines[i].nlen);
        tv_free(lower);
        if (j == i + 1) {
            o[k++] = tv_str_from(lines[i].value, lines[i].vlen);
        } else {
            tv_sb v = {0};
            for (size_t x = i; x < j; x++) {
                if (x > i) tv_sb_push(&v, ", ", 2);
                tv_sb_push(&v, lines[x].value, lines[x].vlen);
            }
            o[k++] = tv_str_from_sb(&v);
        }
        i = j;
    }
    out.p->len = (tv_int)k;
    tv_free(lines);
    return out;
}

/* Each `name` line's value, in order (getSetCookie). */
tv_arr tv_native_headerValues(tv_str block, tv_str name) {
    const char *s = block.p->data, *end = s + block.p->len;
    size_t n = (size_t)name.p->len;
    tv_arr out = tv_arr_fresh();
    while (s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        const char *le = nl ? nl : end;
        if ((size_t)(le - s) > n && strncasecmp(s, name.p->data, n) == 0 && s[n] == ':') {
            const char *v = s + n + 1, *ve = le;
            if (ve > v && ve[-1] == '\r') ve--;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            tv_str *slot = tv_arr_reserve_tail(&out, &tv_type_str, 1);
            *slot = tv_str_from(v, (size_t)(ve - v));
            out.p->len++;
        }
        if (!nl) break;
        s = nl + 1;
    }
    return out;
}

/* TextDecoder's stream mode: how many of the bytes form whole characters, leaving out a character
 * cut off at the end (still valid so far: more bytes may complete it). */
tv_int tv_native_utf8Complete(tv_arr bytes) {
    const uint8_t *p = tv_arr_data(bytes);
    size_t n = (size_t)bytes.p->len;
    /* look back at most 3 bytes for the start of the last character */
    for (size_t back = 1; back <= 3 && back <= n; back++) {
        uint8_t c = p[n - back];
        if ((c & 0xC0) == 0x80) continue; /* a continuation byte: keep looking */
        size_t need = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 1;
        return (tv_int)(need > back ? n - back : n);
    }
    return (tv_int)n;
}

/* ---- Blob and FormData helpers (bytes kept in strings) */

/* Bytes [start, end) of s, with negative offsets from the end and clamping, as Blob.slice. */
tv_str tv_native_byteSlice(tv_str s, tv_int start, tv_int end) {
    tv_int n = s.p->len;
    if (start < 0) start = start + n < 0 ? 0 : start + n;
    if (end < 0) end = end + n < 0 ? 0 : end + n;
    if (start > n) start = n;
    if (end > n) end = n;
    if (end <= start) return TV_EMPTY_STR;
    return tv_str_from(s.p->data + start, (size_t)(end - start));
}

/* A Blob's type: lower-cased, or "" if it has a byte outside printable ASCII. */
tv_str tv_native_mimeLower(tv_str s) {
    tv_int n = s.p->len;
    for (tv_int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s.p->data[i];
        if (c < 0x20 || c > 0x7e) return TV_EMPTY_STR;
    }
    for (tv_int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s.p->data[i];
        if (c >= 'A' && c <= 'Z') {
            tv_str out = tv_str_from(s.p->data, (size_t)n);
            for (tv_int j = i; j < n; j++) {
                char d = out.p->data[j];
                if (d >= 'A' && d <= 'Z') out.p->data[j] = (char)(d + 32);
            }
            return out;
        }
    }
    tv_str_retain(s);
    return s;
}

/* A multipart/form-data body split into its parts, flattened: [name, filename, content type,
 * "file" or "", value, ...] (a part without a name is skipped), or on a malformed body, one
 * element: what's wrong, as Bun says it. */
tv_arr tv_native_multipartParse(tv_str body, tv_str boundary) {
    tv_arr out = tv_arr_fresh();
    const char *why = NULL;
    size_t bl = (size_t)boundary.p->len + 2;
    char *delim = tv_alloc(bl + 1);
    delim[0] = delim[1] = '-';
    memcpy(delim + 2, boundary.p->data, (size_t)boundary.p->len);
    const char *p = body.p->data, *end = p + body.p->len;
    const char *at = boundary.p->len ? memmem(p, (size_t)(end - p), delim, bl) : NULL;
    for (;;) {
        if (!at) { why = "missing final boundary"; break; }
        const char *part = at + bl;
        if (end - part >= 2 && part[0] == '-' && part[1] == '-') break; /* the closing delimiter */
        if (end - part >= 2 && part[0] == '\r' && part[1] == '\n') part += 2;
        else if (part < end && part[0] == '\n') part += 1;
        const char *next = memmem(part, (size_t)(end - part), delim, bl);
        if (!next) { why = "missing final boundary"; break; }
        const char *stop = next;
        if (stop - part >= 2 && stop[-2] == '\r' && stop[-1] == '\n') stop -= 2;
        else if (stop > part && stop[-1] == '\n') stop -= 1;
        /* headers, a blank line, the value */
        const char *hend = memmem(part, (size_t)(stop - part), "\r\n\r\n", 4);
        size_t skip = 4;
        if (!hend) { hend = memmem(part, (size_t)(stop - part), "\n\n", 2); skip = 2; }
        if (!hend) { why = "is missing header colon separator"; break; }
        const char *name = NULL, *filename = NULL, *type = NULL;
        size_t nl = 0, fl = 0, tl = 0;
        for (const char *l = part; l < hend;) {
            const char *le = memchr(l, '\n', (size_t)(hend - l));
            if (!le) le = hend;
            const char *lend = le > l && le[-1] == '\r' ? le - 1 : le;
            if (lend > l && !memchr(l, ':', (size_t)(lend - l))) { why = "is missing header colon separator"; break; }
            if (lend - l >= 20 && strncasecmp(l, "content-disposition:", 20) == 0) {
                for (const char *x = l + 20; x < lend; x++) {
                    bool isname = lend - x > 6 && strncasecmp(x, "name=\"", 6) == 0 && (x[-1] == ' ' || x[-1] == ';');
                    bool isfile = lend - x > 10 && strncasecmp(x, "filename=\"", 10) == 0;
                    if (!isname && !isfile) continue;
                    const char *v = x + (isfile ? 10 : 6), *ve = memchr(v, '"', (size_t)(lend - v));
                    if (!ve) break;
                    if (isfile) { filename = v; fl = (size_t)(ve - v); } else { name = v; nl = (size_t)(ve - v); }
                    x = ve;
                }
            } else if (lend - l >= 13 && strncasecmp(l, "content-type:", 13) == 0) {
                type = l + 13;
                while (type < lend && *type == ' ') type++;
                tl = (size_t)(lend - type);
            }
            l = le + 1;
        }
        if (why) break;
        if (name) {
            const char *v = hend + skip;
            tv_str fields[5] = {
                tv_str_from(name, nl), filename ? tv_str_from(filename, fl) : TV_EMPTY_STR, type ? tv_str_from(type, tl) : TV_EMPTY_STR,
                filename ? tv_str_from("file", 4) : TV_EMPTY_STR, v < stop ? tv_str_from(v, (size_t)(stop - v)) : TV_EMPTY_STR,
            };
            tv_str *slot = tv_arr_reserve_tail(&out, &tv_type_str, 5);
            memcpy(slot, fields, sizeof fields);
            out.p->len += 5;
        }
        at = next;
    }
    tv_free(delim);
    if (why) {
        tv_arr_release(out, &tv_type_str);
        out = tv_arr_fresh();
        tv_str *slot = tv_arr_reserve_tail(&out, &tv_type_str, 1);
        *slot = tv_str_from(why, strlen(why));
        out.p->len = 1;
    }
    return out;
}
