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
static struct { char *p; size_t size; } tv_sb_cache[TV_SB_CACHE_SLOTS];
static size_t tv_sb_cache_bytes;
static os_unfair_lock tv_sb_cache_lock = OS_UNFAIR_LOCK_INIT;

static char *tv_sb_map(size_t cap) {
    size_t size = tv_sb_map_size(cap);
    if (tv_sb_cache_bytes) {
        /* (the smallest that's big enough, and not more than twice it: a buffer growing by
         * doubling finds each of its sizes again, rather than cutting down the biggest; what's
         * past `size` goes back) */
        char *p = NULL;
        size_t have = 0;
        os_unfair_lock_lock(&tv_sb_cache_lock);
        int best = -1;
        for (int i = 0; i < TV_SB_CACHE_SLOTS; i++)
            if (tv_sb_cache[i].p && tv_sb_cache[i].size >= size && tv_sb_cache[i].size <= 2 * size && (best < 0 || tv_sb_cache[i].size < tv_sb_cache[best].size)) best = i;
        if (best >= 0) {
            p = tv_sb_cache[best].p;
            have = tv_sb_cache[best].size;
            tv_sb_cache[best].p = NULL;
            tv_sb_cache_bytes -= have;
        }
        os_unfair_lock_unlock(&tv_sb_cache_lock);
        if (p) {
            if (have > size) munmap(p + size, have - size);
            return p;
        }
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (TV_UNLIKELY(p == MAP_FAILED)) tv_oom(cap);
    return (char *)p;
}

/* Unmaps `size` bytes at p (page-rounded, from tv_sb_map), or keeps them for reuse. */
static void tv_sb_unmap(char *p, size_t size) {
    if (size <= TV_SB_CACHE_MAX / 2) {
        os_unfair_lock_lock(&tv_sb_cache_lock);
        if (tv_sb_cache_bytes + size <= TV_SB_CACHE_MAX)
            for (int i = 0; i < TV_SB_CACHE_SLOTS; i++)
                if (!tv_sb_cache[i].p) {
                    tv_sb_cache[i].p = p;
                    tv_sb_cache[i].size = size;
                    tv_sb_cache_bytes += size;
                    p = NULL;
                    break;
                }
        os_unfair_lock_unlock(&tv_sb_cache_lock);
    }
    if (p) munmap(p, size);
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
    char *old = sb->data ? sb->data - TV_STR_HDR : NULL;
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
        base = tv_sb_map(cap);
        if (old) {
            memcpy(base + TV_STR_HDR, sb->data, sb->len);
            tv_sb_release_buf(old, sb->cap);
        }
        cap = tv_sb_map_size(cap) - TV_STR_HDR - 1;   /* (the whole of its pages) */
    } else if (old && TV_SB_SMALL(sb->cap)) {
        base = (char *)tv_alloc(TV_STR_HDR + cap + 1);
        memcpy(base + TV_STR_HDR, sb->data, sb->len);
        tv_sb_release_buf(old, sb->cap);
    } else {
        base = (char *)tv_realloc(old, TV_STR_HDR + cap + 1);
    }
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

/* memcpy for the short pieces strings are mostly built from: two overlapping word copies
 * instead of a call. */
static inline void tv_copy(char *d, const char *s, size_t n) {
    if (n <= 16) {
        if (n >= 8) {
            uint64_t a, b;
            memcpy(&a, s, 8);
            memcpy(&b, s + n - 8, 8);
            memcpy(d, &a, 8);
            memcpy(d + n - 8, &b, 8);
        } else if (n >= 4) {
            uint32_t a, b;
            memcpy(&a, s, 4);
            memcpy(&b, s + n - 4, 4);
            memcpy(d, &a, 4);
            memcpy(d + n - 4, &b, 4);
        } else if (n) {
            d[0] = s[0];
            d[n / 2] = s[n / 2];
            d[n - 1] = s[n - 1];
        }
        return;
    }
    memcpy(d, s, n);
}

void tv_sb_push(tv_sb *sb, const char *s, size_t n) {
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
    if (sb->data) tv_sb_release_buf(sb->data - TV_STR_HDR, sb->cap);
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

/* ================================================================== strings */

const tv_strbuf tv_empty_strbuf = {-1, 0, {0}}; /* flexible-array initializer: GNU C (gcc, clang) */

typedef struct { int32_t rc; int32_t len; char data[4]; } tv_small_strbuf;
#define TV_A1(c) {-1, 1, {(char)(c), 0}}
#define TV_A4(c) TV_A1(c), TV_A1((c) + 1), TV_A1((c) + 2), TV_A1((c) + 3)
#define TV_A16(c) TV_A4(c), TV_A4((c) + 4), TV_A4((c) + 8), TV_A4((c) + 12)
/* Immortal one-character strings for printable ASCII: chars()/split("") and friends don't allocate for them. */
static const tv_small_strbuf tv_ascii_strs[96] = { /* printable ASCII, 32..127 */
    TV_A16(32), TV_A16(48), TV_A16(64), TV_A16(80), TV_A16(96), TV_A16(112),
};
#undef TV_A1
#undef TV_A4
#undef TV_A16

TV_STR_LIT(tv_lit_true, "true");
TV_STR_LIT(tv_lit_false, "false");

static inline tv_str tv_ascii_str(unsigned char c) { return (tv_str){(tv_strbuf *)&tv_ascii_strs[c - 32]}; }

/* Small strings (header + bytes + NUL <= TV_SMALL_MAX) come from the small-object free lists;
 * larger ones from malloc. The class follows from the length alone, so freeing needs no flag.
 * (Plain malloc under AddressSanitizer, so it sees every string.) */
enum { TV_SMALL_MAX = 256 };
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
        tv_bump += bytes;
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
    return tv_alloc(size);
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

static tv_strbuf *tv_strbuf_new(size_t n) {
    size_t size = tv_size_mul_add(n, 1, TV_STR_HDR + 1);
    if (n > INT32_MAX) tv_trap("string too long", NULL);
    tv_strbuf *b = (tv_strbuf *)(size <= TV_SMALL_MAX ? tv_small_alloc(size) : tv_alloc(size));
    b->rc = 1;
    b->len = (int32_t)n;
    b->data[n] = 0;
    return b;
}

void tv_str_release_slow(tv_str s) {
    size_t size = tv_strbuf_size((size_t)s.p->len);
    if (size <= TV_SMALL_MAX) tv_small_free(s.p, size);
#ifdef TV_SB_MAPS
    else if (TV_STR_MAYBE_MAPPED(size) && malloc_size(s.p) == 0) {
        size_t page = (size_t)getpagesize();
        tv_sb_unmap((char *)s.p, (size + page - 1) & ~(page - 1));
    }
#endif
    else free(s.p);
}

tv_str tv_str_from(const char *bytes, size_t n) {
    if (n == 0) return TV_EMPTY_STR;
    if (n == 1 && (unsigned char)bytes[0] - 32u < 96u) return tv_ascii_str((unsigned char)bytes[0]);
    tv_strbuf *b = tv_strbuf_new(n);
    tv_copy(b->data, bytes, n);
    return (tv_str){b};
}

tv_str tv_str_from_sb(tv_sb *sb) {
    size_t n = sb->len;
    if (n <= 1) { /* empty or one byte: tv_str_from may return an immortal string */
        tv_str r = tv_str_from(sb->data, n);
        tv_sb_free(sb);
        return r;
    }
    if (tv_strbuf_size(n) <= TV_SMALL_MAX) { /* small strings live in the small-block allocator */
        tv_str r = tv_str_from(sb->data, n);
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

tv_str tv_str_concat(tv_str a, tv_str b) {
    size_t na = (size_t)a.p->len, nb = (size_t)b.p->len;
    if (nb == 0) { tv_str_retain(a); return a; }
    if (na == 0) { tv_str_retain(b); return b; }
    tv_strbuf *r = tv_strbuf_new(tv_size_mul_add(na, 1, nb));
    memcpy(r->data, a.p->data, na);
    memcpy(r->data + na, b.p->data, nb);
    return (tv_str){r};
}

bool tv_str_eq(tv_str a, tv_str b) {
    return a.p == b.p || (a.p->len == b.p->len && memcmp(a.p->data, b.p->data, (size_t)a.p->len) == 0);
}

int tv_str_cmp(tv_str a, tv_str b) {
    if (a.p == b.p) return 0;
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
    char buf[24];
    return tv_str_from(buf, tv_fmt_i64(buf, v));
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

/* Shortest decimal digits d1..dk (no trailing zeros) and exponent n such that
 * v == 0.d1..dk × 10^n round-trips (v > 0, finite). For f32, round-trips as a float. */
static int tv_shortest_digits(double v, bool is_f32, char *digits, int *n_out) {
    char tmp[48];
    if (is_f32) {
        for (int prec = 0; prec < 9; prec++) {
            snprintf(tmp, sizeof tmp, "%.*e", prec, v);
            if (strtof(tmp, NULL) == (float)v) break;
        }
    } else if (v >= DBL_MIN) {
        /* For normal doubles, if any <=15-digit decimal round-trips then the correctly rounded
         * 15-digit one does too, and stripping its trailing zeros gives the shortest. */
        snprintf(tmp, sizeof tmp, "%.14e", v);
        if (strtod(tmp, NULL) != v) {
            snprintf(tmp, sizeof tmp, "%.15e", v);
            if (strtod(tmp, NULL) != v) snprintf(tmp, sizeof tmp, "%.16e", v);
        }
    } else { /* subnormal: fewer significant bits, search all precisions */
        for (int prec = 0; prec < 17; prec++) {
            snprintf(tmp, sizeof tmp, "%.*e", prec, v);
            if (strtod(tmp, NULL) == v) break;
        }
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
    /* Fast path: the fewest decimals f such that round(v * 10^f) / 10^f is exactly v (both
     * exact in a double: the division is correctly rounded, like parsing the decimal). The
     * smallest f gives the fewest significant digits. Typical JSON numbers (prices, coordinates)
     * take this path; others use the general shortest search. */
    static const double pow10_tab[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16 };
    if (!is_f32 && v >= 1e-7 && v < 1e15) {
        for (int f = 1; f <= 16 && !k; f++) {
            double s = v * pow10_tab[f];
            if (s >= 9007199254740992.0) break;
            double r = (double)(int64_t)(s + 0.5);
            if (r / pow10_tab[f] != v) continue;
            /* digits of r, with the decimal point f places from the right */
            char tmp[24];
            int m = (int)tv_fmt_i64(tmp, (int64_t)r);
            int lead = 0;
            while (lead < m - 1 && tmp[lead] == '0') lead++;
            k = m - lead;
            memcpy(d, tmp + lead, (size_t)k);
            n = m - f;
            while (k > 1 && d[k - 1] == '0') k--;
        }
    }
    if (!k) k = tv_shortest_digits(v, is_f32, d, &n);
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
    char buf[40];
    return tv_str_from(buf, tv_fmt_number(buf, v, false));
}

void tv_sb_push_f64(tv_sb *sb, double v) {
    char buf[40];
    tv_sb_push(sb, buf, tv_fmt_number(buf, v, false));
}

static void tv_toFixed_check(tv_int digits, const char *loc) {
    if (digits < 0 || digits > 100) tv_trap("toFixed() digits argument must be between 0 and 100", loc);
}

tv_str tv_f64_to_fixed(double v, tv_int digits, const char *loc) {
    tv_toFixed_check(digits, loc);
    if (v != v || fabs(v) >= 1e21) return tv_str_from_f64(v);
    if (v == 0) v = 0.0; /* (-0).toFixed() is "0" */
    int f = (int)digits;
    char buf[160];
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
    if (mant && e == -(f + 1)) {
        len = snprintf(buf, sizeof buf, "%.*f", f + 1, v);
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
        len = snprintf(buf, sizeof buf, "%.*f", f, v);
    }
    return tv_str_from(buf, (size_t)len);
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

static bool tv_is_js_space(uint32_t c) {
    if (c < 0x80) return c == ' ' || (c >= 0x09 && c <= 0x0D);
    return c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

static void tv_trim_range(tv_str s, bool left, bool right, size_t *a_out, size_t *b_out) {
    const unsigned char *d = (const unsigned char *)s.p->data;
    size_t a = 0, b = (size_t)s.p->len;
    uint32_t cp;
    if (left)
        while (a < b) {
            size_t l = tv_utf8_decode(d + a, b - a, &cp);
            if (!tv_is_js_space(cp)) break;
            a += l;
        }
    if (right)
        while (b > a) {
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

/* Byte offset after `chars` characters starting at d (bounded by n). */
static size_t tv_utf8_advance(const char *d, size_t n, size_t chars) {
    size_t i = 0;
    while (chars-- && i < n) i += tv_utf8_len((unsigned char)d[i]);
    return i < n ? i : n;
}

/* ------------------------------------------------------------------ string methods */

static inline int64_t tv_clamp_index(int64_t i, int64_t len) {
    if (i < 0) { i += len; return i < 0 ? 0 : i; }
    return i > len ? len : i;
}

static void tv_check_boundary(tv_str s, int64_t i, const char *loc) {
    if (i > 0 && i < s.p->len && tv_is_cont((unsigned char)s.p->data[i])) {
        char msg[96];
        snprintf(msg, sizeof msg, "string index %lld is not on a UTF-8 character boundary", (long long)i);
        tv_trap(msg, loc);
    }
}

tv_str tv_str_slice(tv_str s, tv_int start, tv_int end, bool has_end, const char *loc) {
    int64_t len = s.p->len;
    int64_t a = tv_clamp_index(start, len);
    int64_t b = has_end ? tv_clamp_index(end, len) : len;
    if (b < a) b = a;
    tv_check_boundary(s, a, loc);
    tv_check_boundary(s, b, loc);
    if (a == 0 && b == len) { tv_str_retain(s); return s; }
    return tv_str_from(s.p->data + a, (size_t)(b - a));
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
        if (memcmp(p + 1, nd + 1, nn - 1) == 0) return p - h;
        p++;
    }
    return -1;
}

bool tv_str_includes(tv_str s, tv_str needle) { return tv_str_index_of(s, needle) >= 0; }

bool tv_str_starts_with(tv_str s, tv_str prefix) {
    return prefix.p->len <= s.p->len && memcmp(s.p->data, prefix.p->data, (size_t)prefix.p->len) == 0;
}

bool tv_str_ends_with(tv_str s, tv_str suffix) {
    return suffix.p->len <= s.p->len &&
           memcmp(s.p->data + s.p->len - suffix.p->len, suffix.p->data, (size_t)suffix.p->len) == 0;
}

tv_int tv_str_index_of(tv_str s, tv_str needle) {
    return tv_find(s.p->data, (size_t)s.p->len, needle.p->data, (size_t)needle.p->len, 0);
}

static tv_arrbuf *tv_arrbuf_new(size_t esize, int64_t cap);

tv_arr tv_str_chars(tv_str s) {
    int64_t count = tv_str_char_count(s);
    if (count == 0) return TV_EMPTY_ARR;
    tv_arrbuf *b = tv_arrbuf_new(sizeof(tv_str), count);
    tv_str *out = (tv_str *)(void *)b->data;
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len, i = 0;
    int64_t k = 0;
    while (i < n && k < count) {
        size_t l = tv_utf8_len((unsigned char)d[i]);
        if (l > n - i) l = n - i;
        out[k++] = tv_str_from(d + i, l);
        i += l;
    }
    return (tv_arr){b, k};
}

tv_arr tv_str_split(tv_str s, tv_str sep) {
    if (sep.p->len == 0) return tv_str_chars(s);
    tv_arr r = TV_EMPTY_ARR;
    const char *d = s.p->data;
    size_t n = (size_t)s.p->len, sn = (size_t)sep.p->len, from = 0;
    int64_t pos = tv_find(d, n, sep.p->data, sn, 0);
    if (pos < 0) {
        tv_str_retain(s);
        tv_arr_push(&r, &tv_type_str, &s);
        return r;
    }
    for (;;) {
        size_t end = pos < 0 ? n : (size_t)pos;
        tv_str piece = tv_str_from(d + from, end - from);
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
    return tv_str_from(s.p->data + a, b - a);
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
    return tv_str_from_sb(&sb);
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
    return tv_str_from_sb(&sb);
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
    return (tv_str){b};
}

/* Appends `chars` characters of fill (cycled). fill is non-empty. */
static void tv_push_fill(tv_sb *sb, tv_str fill, int64_t chars) {
    int64_t fc = tv_str_char_count(fill);
    int64_t full = chars / fc, rem = chars % fc;
    for (int64_t i = 0; i < full; i++) tv_sb_push_str(sb, fill);
    tv_sb_push(sb, fill.p->data, tv_utf8_advance(fill.p->data, (size_t)fill.p->len, (size_t)rem));
}

static tv_str tv_str_pad(tv_str s, tv_int len, tv_str fill, bool at_start) {
    int64_t cur = tv_str_char_count(s);
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
    /* The span is validated; strtod stops at e (whitespace, NUL or a non-ASCII byte). */
    char *end;
    double d = strtod(p, &end);
    if (end != e) return false;
    *out = d;
    return true;
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

static tv_arrbuf *tv_arrbuf_new(size_t esize, int64_t cap) {
    tv_arrbuf *b = (tv_arrbuf *)tv_alloc(tv_size_mul_add((size_t)cap, esize, sizeof(tv_arrbuf)));
    b->rc = 1;
    b->cap = cap;
    b->_pad[0] = b->_pad[1] = 0;
    return b;
}

/* Copies n elements and retains the copies. */
static void tv_copy_retain(unsigned char *dst, const unsigned char *src, int64_t n, const tv_type *t) {
    size_t es = t->size;
    if (!es || n <= 0) return;
    memcpy(dst, src, (size_t)n * es);
    if (t->retain)
        for (int64_t i = 0; i < n; i++) t->retain(dst + (size_t)i * es);
}

void tv_arr_release_slow(tv_arr a, const tv_type *t) {
    if (t && t->release && t->size) {
        size_t es = t->size;
        for (int64_t i = 0; i < a.len; i++) t->release(a.p->data + (size_t)i * es);
    }
    free(a.p);
}

void tv_arr_make_unique(tv_arr *a, const tv_type *t, tv_int min_cap) {
    tv_arrbuf *p = a->p;
    size_t es = t->size;
    if (p && p->rc == 1) {
        if (p->cap >= min_cap) return;
        int64_t nc = tv_grow_cap(p->cap, min_cap);
        p = (tv_arrbuf *)tv_realloc(p, tv_size_mul_add((size_t)nc, es, sizeof(tv_arrbuf)));
        p->cap = nc;
        a->p = p;
        return;
    }
    if (!p) {
        if (min_cap > 0) a->p = tv_arrbuf_new(es, tv_grow_cap(0, min_cap));
        return;
    }
    /* shared (rc > 1) or immortal (rc < 0): clone the value's elements */
    int64_t len = a->len;
    int64_t nc = min_cap > len ? tv_grow_cap(len, min_cap) : len;
    tv_arrbuf *q = NULL;
    if (nc > 0) {
        q = tv_arrbuf_new(es, nc);
        tv_copy_retain(q->data, p->data, len, t);
    }
    if (p->rc > 0) p->rc--; /* was > 1, so this never frees */
    a->p = q;
}

tv_arr tv_arr_with_capacity(const tv_type *t, tv_int cap) {
    if (cap <= 0) return TV_EMPTY_ARR;
    return (tv_arr){tv_arrbuf_new(t->size, cap), 0};
}

void *tv_arr_reserve_tail(tv_arr *a, const tv_type *t, tv_int n) {
    if (n < 0 || n > INT64_MAX - a->len) tv_trap("invalid array reserve count", NULL);
    tv_arr_make_unique(a, t, a->len + n);
    return a->p ? a->p->data + (size_t)a->len * t->size : NULL;
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
    return a->p->data + (size_t)i * t->size;
}

void tv_arr_push(tv_arr *a, const tv_type *t, void *elem) {
    int64_t len = tv_arr_len(*a);
    tv_arr_make_unique(a, t, len + 1);
    if (t->size) memcpy(a->p->data + (size_t)len * t->size, elem, t->size);
    a->len = len + 1;
}

bool tv_arr_pop(tv_arr *a, const tv_type *t, void *out) {
    int64_t len = tv_arr_len(*a);
    if (len == 0) return false;
    tv_arr_make_unique(a, t, 0);
    size_t es = t->size;
    if (es) {
        unsigned char *src = a->p->data + (size_t)(len - 1) * es;
        if (out) memcpy(out, src, es);
        else if (t->release) t->release(src);
    }
    a->len = len - 1;
    return true;
}

bool tv_arr_shift(tv_arr *a, const tv_type *t, void *out) {
    int64_t len = tv_arr_len(*a);
    if (len == 0) return false;
    tv_arr_make_unique(a, t, 0);
    size_t es = t->size;
    if (es) {
        unsigned char *d = a->p->data;
        if (out) memcpy(out, d, es);
        else if (t->release) t->release(d);
        memmove(d, d + es, (size_t)(len - 1) * es);
    }
    a->len = len - 1;
    return true;
}

void tv_arr_unshift(tv_arr *a, const tv_type *t, void *elem) {
    int64_t len = tv_arr_len(*a);
    tv_arr_make_unique(a, t, len + 1);
    size_t es = t->size;
    if (es) {
        unsigned char *d = a->p->data;
        memmove(d + es, d, (size_t)len * es);
        memcpy(d, elem, es);
    }
    a->len = len + 1;
}

tv_arr tv_arr_slice(tv_arr a, const tv_type *t, tv_int start, tv_int end, bool has_start, bool has_end) {
    int64_t len = tv_arr_len(a);
    int64_t s = has_start ? tv_clamp_index(start, len) : 0;
    int64_t e = has_end ? tv_clamp_index(end, len) : len;
    if (e <= s) return TV_EMPTY_ARR;
    if (s == 0 && e == len) { tv_arr_retain(a); return a; } /* whole array: share (copy-on-write) */
    tv_arrbuf *b = tv_arrbuf_new(t->size, e - s);
    tv_copy_retain(b->data, a.p->data + (size_t)s * t->size, e - s, t);
    return (tv_arr){b, e - s};
}

tv_arr tv_arr_concat(tv_arr a, tv_arr b, const tv_type *t) {
    int64_t na = tv_arr_len(a), nb = tv_arr_len(b);
    if (nb == 0) { tv_arr_retain(a); return a; }
    if (na == 0) { tv_arr_retain(b); return b; }
    if (na > INT64_MAX - nb) tv_trap("array too long", NULL);
    tv_arrbuf *r = tv_arrbuf_new(t->size, na + nb);
    tv_copy_retain(r->data, a.p->data, na, t);
    tv_copy_retain(r->data + (size_t)na * t->size, b.p->data, nb, t);
    return (tv_arr){r, na + nb};
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
    unsigned char *d = a->p->data;
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
    unsigned char *d = a->p->data;
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
        if (t != &tv_type_undefined) t->to_str(&sb, d + (size_t)i * es);
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

tv_int tv_arr_index_of(tv_arr a, const tv_type *t, const void *elem) {
    int64_t n = tv_arr_len(a);
    size_t es = t->size;
    for (int64_t i = 0; i < n; i++)
        if (t->eq(a.p->data + (size_t)i * es, elem)) return i;
    return -1;
}

tv_int tv_arr_last_index_of(tv_arr a, const tv_type *t, const void *elem) {
    size_t es = t->size;
    for (int64_t i = tv_arr_len(a) - 1; i >= 0; i--)
        if (t->eq(a.p->data + (size_t)i * es, elem)) return i;
    return -1;
}

static void tv_join_into(tv_sb *sb, tv_arr a, const tv_type *t, const char *sep, size_t sep_len) {
    int64_t n = tv_arr_len(a);
    size_t es = t->size;
    for (int64_t i = 0; i < n; i++) {
        if (i) tv_sb_push(sb, sep, sep_len);
        if (t != &tv_type_undefined) t->to_str(sb, a.p->data + (size_t)i * es);
    }
}

tv_str tv_arr_join(tv_arr a, const tv_type *t, tv_str sep) {
    if (tv_arr_len(a) == 1 && t == &tv_type_str) {
        tv_str s = *(tv_str *)(void *)a.p->data;
        tv_str_retain(s);
        return s;
    }
    tv_sb sb = {0};
    tv_join_into(&sb, a, t, sep.p->data, (size_t)sep.p->len);
    return tv_str_from_sb(&sb);
}

void tv_to_str_arr(tv_sb *sb, tv_arr a, const tv_type *t) { tv_join_into(sb, a, t, ",", 1); }

/* ================================================================== maps and sets */

/* One allocation: header, `cap` entries in insertion order, then 2*cap index slots.
 * Entry: uint64 hash (top bit set; 0 = deleted), key (padded to 8), value (padded to 8).
 * Index slot: entry number + 1, 0 = empty. Open addressing, linear probing, backward-shift
 * deletion (so the index never holds tombstones); deleted entries are compacted away when
 * the entry array fills up. */
struct tv_mapbuf {
    int64_t rc;
    int64_t count;    /* live entries */
    int64_t used;     /* entries used, live or deleted */
    int64_t cap;      /* entry capacity (power of two) */
    uint64_t mask;    /* index slots - 1 */
    uint32_t *index;
    int64_t _pad[2];  /* header = 64 bytes, entries stay 16-byte aligned */
    unsigned char entries[];
};

#define TV_LIVE_BIT ((uint64_t)1 << 63)
#define TV_MAP_MIN_CAP 8

typedef struct { size_t voff, stride; } tv_mlayout;

static inline tv_mlayout tv_mlay(const tv_type *kt, const tv_type *vt) {
    tv_mlayout L;
    L.voff = 8 + ((kt->size + 7) & ~(size_t)7);
    L.stride = L.voff + ((vt->size + 7) & ~(size_t)7);
    return L;
}

static inline unsigned char *tv_entry(const tv_mapbuf *m, tv_mlayout L, int64_t i) {
    return (unsigned char *)m->entries + (size_t)i * L.stride;
}
static inline uint64_t tv_entry_hash(const unsigned char *e) { uint64_t h; memcpy(&h, e, 8); return h; }
static inline void tv_entry_set_hash(unsigned char *e, uint64_t h) { memcpy(e, &h, 8); }

static inline double tv_canon_f64(double x) {
    if (x == 0) return 0.0;  /* -0 → 0 */
    if (x != x) return NAN;  /* one NaN */
    return x;
}

static inline uint64_t tv_key_hash(const tv_type *kt, const void *key) { return kt->hash(key) | TV_LIVE_BIT; }

/* Map keys use SameValueZero: like === except NaN equals NaN. */
static inline bool tv_key_eq(const tv_type *kt, const void *a, const void *b) {
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

static tv_mapbuf *tv_mapbuf_new(int64_t cap, tv_mlayout L) {
    if (cap > ((int64_t)1 << 31)) tv_trap("map too large", NULL);
    size_t islots = (size_t)cap * 2;
    size_t bytes = tv_size_mul_add((size_t)cap, L.stride, sizeof(tv_mapbuf));
    bytes = tv_size_mul_add(islots, sizeof(uint32_t), bytes);
    tv_mapbuf *m = (tv_mapbuf *)tv_alloc(bytes);
    m->rc = 1;
    m->count = m->used = 0;
    m->cap = cap;
    m->mask = islots - 1;
    m->index = (uint32_t *)(void *)(m->entries + (size_t)cap * L.stride);
    m->_pad[0] = m->_pad[1] = 0;
    memset(m->index, 0, islots * sizeof(uint32_t));
    return m;
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
        m->index[pos] = (uint32_t)(j + 1);
        j++;
    }
    m->count = m->used = j;
    return m;
}

static tv_mapbuf *tv_map_unique(tv_map *mp, const tv_type *kt, const tv_type *vt, tv_mlayout L) {
    tv_mapbuf *m = mp->p;
    if (!m) return mp->p = tv_mapbuf_new(TV_MAP_MIN_CAP, L);
    if (m->rc == 1) return m;
    tv_mapbuf *c = tv_map_rebuild(m, L, m->cap, true, kt, vt);
    if (m->rc > 0) m->rc--;
    return mp->p = c;
}

/* Index slot holding `key` (*found = true), or the empty slot where it would go. */
static uint64_t tv_map_find(const tv_mapbuf *m, tv_mlayout L, const tv_type *kt, const void *key, uint64_t h,
                            bool *found) {
    uint64_t i = h & m->mask;
    for (;;) {
        uint32_t e = m->index[i];
        if (!e) { *found = false; return i; }
        const unsigned char *ent = tv_entry(m, L, e - 1);
        if (tv_entry_hash(ent) == h && tv_key_eq(kt, ent + 8, key)) { *found = true; return i; }
        i = (i + 1) & m->mask;
    }
}

void tv_map_retain(tv_map m) {
    if (m.p && m.p->rc >= 0) m.p->rc++;
}

static void tv_map_release_entries(tv_mapbuf *m, const tv_type *kt, const tv_type *vt, tv_mlayout L) {
    bool rk = kt->release && kt->size, rv = vt->release && vt->size;
    if (!rk && !rv) return;
    for (int64_t i = 0; i < m->used; i++) {
        unsigned char *e = tv_entry(m, L, i);
        if (!tv_entry_hash(e)) continue;
        if (rk) kt->release(e + 8);
        if (rv) vt->release(e + L.voff);
    }
}

void tv_map_release(tv_map m, const tv_type *kt, const tv_type *vt) {
    if (!m.p || m.p->rc <= 0 || --m.p->rc > 0) return;
    tv_map_release_entries(m.p, kt, vt, tv_mlay(kt, vt));
    free(m.p);
}

tv_int tv_map_size(tv_map m) { return m.p ? m.p->count : 0; }

void *tv_map_get(tv_map mm, const tv_type *kt, const tv_type *vt, const void *key) {
    tv_mapbuf *m = mm.p;
    if (!m || !m->count) return NULL;
    tv_mlayout L = tv_mlay(kt, vt);
    bool found;
    uint64_t pos = tv_map_find(m, L, kt, key, tv_key_hash(kt, key), &found);
    return found ? tv_entry(m, L, m->index[pos] - 1) + L.voff : NULL;
}

bool tv_map_has(tv_map m, const tv_type *kt, const tv_type *vt, const void *key) {
    return tv_map_get(m, kt, vt, key) != NULL;
}

void tv_map_set(tv_map *mp, const tv_type *kt, const tv_type *vt, void *key, void *value) {
    tv_mlayout L = tv_mlay(kt, vt);
    tv_mapbuf *m = tv_map_unique(mp, kt, vt, L);
    uint64_t h = tv_key_hash(kt, key);
    bool found;
    uint64_t pos = tv_map_find(m, L, kt, key, h, &found);
    if (found) { /* replace the value in place; keep the original key */
        unsigned char *e = tv_entry(m, L, m->index[pos] - 1);
        if (vt->size) {
            if (vt->release) vt->release(e + L.voff);
            memcpy(e + L.voff, value, vt->size);
        }
        if (kt->release && kt->size) kt->release(key);
        return;
    }
    if (m->used == m->cap) {
        /* full: compact if at least half the entries are deleted, else double */
        int64_t ncap = m->count * 2 <= m->cap ? m->cap : m->cap * 2;
        tv_mapbuf *n = tv_map_rebuild(m, L, ncap, false, kt, vt);
        free(m);
        mp->p = m = n;
        pos = h & m->mask;
        while (m->index[pos]) pos = (pos + 1) & m->mask;
    }
    int64_t idx = m->used++;
    unsigned char *e = tv_entry(m, L, idx);
    tv_entry_set_hash(e, h);
    if (kt->size) memcpy(e + 8, key, kt->size);
    if (vt->size) memcpy(e + L.voff, value, vt->size);
    m->index[pos] = (uint32_t)(idx + 1);
    m->count++;
}

bool tv_map_delete(tv_map *mp, const tv_type *kt, const tv_type *vt, const void *key) {
    tv_mapbuf *m = mp->p;
    if (!m || !m->count) return false;
    tv_mlayout L = tv_mlay(kt, vt);
    uint64_t h = tv_key_hash(kt, key);
    bool found;
    uint64_t pos = tv_map_find(m, L, kt, key, h, &found);
    if (!found) return false;
    if (m->rc != 1) {
        m = tv_map_unique(mp, kt, vt, L);
        pos = tv_map_find(m, L, kt, key, h, &found);
    }
    unsigned char *e = tv_entry(m, L, m->index[pos] - 1);
    tv_entry_set_hash(e, 0);
    m->count--;
    /* backward-shift deletion in the index */
    uint64_t mask = m->mask, i = pos, j = pos;
    for (;;) {
        j = (j + 1) & mask;
        uint32_t s = m->index[j];
        if (!s) break;
        uint64_t home = tv_entry_hash(tv_entry(m, L, s - 1)) & mask;
        if (((j - home) & mask) >= ((j - i) & mask)) {
            m->index[i] = s;
            i = j;
        }
    }
    m->index[i] = 0;
    if (m->count == 0) m->used = 0; /* index is empty too */
    /* release last: a release callback must see a consistent map */
    if (kt->release && kt->size) kt->release(e + 8);
    if (vt->release && vt->size) vt->release(e + L.voff);
    return true;
}

void tv_map_clear(tv_map *m, const tv_type *kt, const tv_type *vt) {
    tv_map_release(*m, kt, vt);
    m->p = NULL;
}

static tv_arr tv_map_column(tv_map mm, const tv_type *kt, const tv_type *vt, bool values) {
    tv_mapbuf *m = mm.p;
    if (!m || !m->count) return TV_EMPTY_ARR;
    tv_mlayout L = tv_mlay(kt, vt);
    const tv_type *t = values ? vt : kt;
    size_t off = values ? L.voff : 8, es = t->size;
    tv_arrbuf *b = tv_arrbuf_new(es, m->count);
    int64_t j = 0;
    for (int64_t i = 0; i < m->used; i++) {
        unsigned char *e = tv_entry(m, L, i);
        if (!tv_entry_hash(e)) continue;
        if (es) {
            memcpy(b->data + (size_t)j * es, e + off, es);
            if (t->retain) t->retain(b->data + (size_t)j * es);
        }
        j++;
    }
    return (tv_arr){b, j};
}

tv_arr tv_map_keys(tv_map m, const tv_type *kt, const tv_type *vt) { return tv_map_column(m, kt, vt, false); }
tv_arr tv_map_values(tv_map m, const tv_type *kt, const tv_type *vt) { return tv_map_column(m, kt, vt, true); }

bool tv_map_next(tv_map mm, const tv_type *kt, const tv_type *vt, tv_int *cursor, void **key, void **value) {
    tv_mapbuf *m = mm.p;
    if (!m) return false;
    tv_mlayout L = tv_mlay(kt, vt);
    for (int64_t i = *cursor; i < m->used; i++) {
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

void tv_inspect_arr(tv_sb *sb, tv_arr a, const tv_type *t, int depth) {
    int64_t n = tv_arr_len(a);
    if (n == 0) { tv_sb_push(sb, "[]", 2); return; }
    if (!tv_inspect_enter(sb, depth, "[Array]")) return;
    tv_pieces out = {0};
    int64_t shown = n < TV_MAX_ARRAY_LENGTH ? n : TV_MAX_ARRAY_LENGTH;
    for (int64_t i = 0; i < shown; i++) { /* formatProperty: indentation + 2 per entry */
        tv_ictx.indent += 2;
        tv_inspect_value(&out.text, t, a.p->data + (size_t)i * t->size, depth + 1);
        tv_ictx.indent -= 2;
        tv_pieces_mark(&out);
    }
    if (shown < n) tv_push_more_items(&out, n - shown);
    tv_reduce_to_single_string(sb, &out, "[", "]", true, depth + 1, tv_is_number_type(t));
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

void tv_map_inspect(tv_sb *sb, tv_map m, const tv_type *kt, const tv_type *vt, int depth) {
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

void tv_inspect_object(tv_sb *sb, int depth, const char *cls, size_t n, const char *const *names, const tv_type *const *types,
                       const void *const *fields) {
    size_t cl = strlen(cls);
    if (n == 0) {
        tv_sb_push(sb, cls, cl);
        tv_sb_push(sb, " {}", 3);
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
    snprintf(buf, sizeof buf, "%s {", cls);
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
    tv_sb_push(sb, buf, tv_fmt_number(buf, (double)*(const float *)p, true));
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
static uint64_t tv_rng_state;

static void tv_random_seed(uint64_t seed) {
    uint64_t x = tv_mix64(seed + TV_RNG_DEFAULT);
    tv_rng_state = (x ? x : TV_RNG_DEFAULT) ^ TV_RNG_DEFAULT;
}

/* Seeded on first use (TOV_SEED: a number, or any other value for a random seed), so programs
 * that never draw a random number don't link getenv, time and clock. */
static bool tv_rng_ready;
static __attribute__((noinline, cold)) void tv_random_init(void) {
    tv_rng_ready = true;
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

double tv_random(void) { /* xorshift64* */
    if (TV_UNLIKELY(!tv_rng_ready)) tv_random_init();
    uint64_t x = tv_rng_state ^ TV_RNG_DEFAULT;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    tv_rng_state = x ^ TV_RNG_DEFAULT;
    return (double)((x * 0x2545f4914f6cdd1dULL) >> 11) * 0x1.0p-53;
}

/* ================================================================== closures */

void tv_env_release_slow(tv_env *e) {
    if (e->drop) e->drop(e);
    free(e);
}

/* ================================================================== tests */

bool tv_test_active(void) { return tv_test_jmp != NULL; }

static void tv_test_longjmp(void) { longjmp(*tv_test_jmp, 1); }

void tv_test_run(const char *name, void (*fn)(void)) {
    jmp_buf jb;
    tv_test_jmp = &jb;
    tv_test_unwind = tv_test_longjmp;
    tv_sb sb = {0};
    if (setjmp(jb) == 0) {
        fn();
        tv_test_jmp = NULL;
        tv_tests_passed++;
        tv_sb_push_cstr(&sb, "ok   ");
        tv_sb_push_cstr(&sb, name);
    } else {
        tv_test_jmp = NULL;
        tv_tests_failed++;
        tv_sb_push_cstr(&sb, "FAIL ");
        tv_sb_push_cstr(&sb, name);
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
    if (!d) { tv_native_fail(errno, "scandir", path); return TV_EMPTY_ARR; }
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

#ifndef __APPLE__
static struct timespec tv_start_time;
#endif
#ifdef __APPLE__
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

char tv_jp_peek(tv_jp *p) {
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

bool tv_jp_char(tv_jp *p, char c) {
    if (tv_jp_peek(p) == c) { p->i++; return true; }
    char what[4] = { '\'', c, '\'', 0 };
    return tv_jp_fail(p, what);
}

bool tv_jp_try_char(tv_jp *p, char c) {
    if (tv_jp_peek(p) == c) { p->i++; return true; }
    return false;
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

bool tv_jp_string(tv_jp *p, tv_str *out) {
    if (tv_jp_peek(p) != '"') return tv_jp_fail(p, "a string");
    p->i++;
    size_t start = p->i;
    /* Fast path: no escapes. */
    while (p->i < p->n && p->s[p->i] != '"' && p->s[p->i] != '\\' && p->s[p->i] >= 0x20) p->i++;
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

bool tv_jp_number(tv_jp *p, double *out) {
    tv_jp_ws(p);
    size_t start = p->i, i = p->i;
    if (i < p->n && p->s[i] == '-') i++;
    if (i < p->n && p->s[i] == '0') i++;
    else if (i < p->n && p->s[i] >= '1' && p->s[i] <= '9') { while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') i++; }
    else return tv_jp_fail(p, "a number");
    if (i < p->n && p->s[i] == '.') {
        i++;
        if (!(i < p->n && p->s[i] >= '0' && p->s[i] <= '9')) { p->i = i; return tv_jp_fail(p, "a digit"); }
        while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') i++;
    }
    if (i < p->n && (p->s[i] == 'e' || p->s[i] == 'E')) {
        i++;
        if (i < p->n && (p->s[i] == '+' || p->s[i] == '-')) i++;
        if (!(i < p->n && p->s[i] >= '0' && p->s[i] <= '9')) { p->i = i; return tv_jp_fail(p, "a digit"); }
        while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') i++;
    }
    /* Fast path (exact): up to 15 significant digits, no exponent part, few decimals:
     * mantissa / 10^decimals is correctly rounded because both are exact doubles. */
    {
        uint64_t mant = 0;
        int digits = 0, decimals = 0;
        bool neg = false, simple = true, frac = false;
        for (size_t j = start; j < i; j++) {
            unsigned char c = p->s[j];
            if (c == '-') neg = true;
            else if (c == '.') frac = true;
            else if (c >= '0' && c <= '9') {
                if (mant || c != '0') digits++;
                mant = mant * 10 + (uint64_t)(c - '0');
                if (frac) decimals++;
            } else { simple = false; break; }
        }
        static const double p10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
        if (simple && digits <= 15 && decimals <= 22) {
            double x = (double)mant / p10[decimals];
            *out = neg ? -x : x;
            p->i = i;
            return true;
        }
    }
    char buf[64];
    size_t len = i - start;
    char *tmp = len < sizeof buf ? buf : tv_alloc(len + 1);
    memcpy(tmp, p->s + start, len);
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
    if (i == start || tv_dec_parse_n(p->s + start, i - start, out) != 0) return tv_jp_fail(p, "a decimal number");
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
                arr.len = s->n;
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
        tv_arr empty = TV_EMPTY_ARR;
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

static void tv_timer_push(tv_timer t) {
    if (tv_ntimers == tv_timers_cap) {
        tv_timers_cap = tv_timers_cap ? tv_timers_cap * 2 : 16;
        tv_timers = tv_realloc(tv_timers, tv_timers_cap * sizeof *tv_timers);
    }
    size_t i = tv_ntimers++;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!tv_timer_before(&t, &tv_timers[parent])) break;
        tv_timers[i] = tv_timers[parent];
        i = parent;
    }
    tv_timers[i] = t;
}

static tv_timer tv_timer_pop(void) {
    tv_timer top = tv_timers[0];
    tv_timer last = tv_timers[--tv_ntimers];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        const tv_timer *best = &last;
        if (l < tv_ntimers && tv_timer_before(&tv_timers[l], best)) { m = l; best = &tv_timers[l]; }
        if (r < tv_ntimers && tv_timer_before(&tv_timers[r], best)) m = r;
        if (m == i) break;
        tv_timers[i] = tv_timers[m];
        i = m;
    }
    if (tv_ntimers) tv_timers[i] = last;
    return top;
}

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
    for (size_t i = 0; i < tv_ntimers; i++) {
        if (tv_timers[i].id == id && tv_timers[i].cb.fn) {
            tv_env_release(tv_timers[i].cb.env);
            tv_timers[i].cb.fn = NULL;
            if (!tv_timers[i].weak) tv_live_timers--;
            return;
        }
    }
}

/* Node's timer.unref(): the timer still fires while other work keeps the program running,
 * but doesn't keep it running by itself. */
void tv_native_timerUnref(tv_int id) {
    for (size_t i = 0; i < tv_ntimers; i++) {
        if (tv_timers[i].id == id && tv_timers[i].cb.fn && !tv_timers[i].weak) {
            tv_timers[i].weak = true;
            tv_live_timers--;
            return;
        }
    }
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
        ((void (*)(void *, tv_str, tv_str, tv_str, tv_str))h.fn)(h.env, method, target, hdrs, body);
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
    if (!tv_url_parse_full(input, base, &u)) return TV_EMPTY_ARR;
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

static void tv_fc_reap(void) {
    while (tv_fc_graveyard) {
        tv_fc *c = tv_fc_graveyard;
        tv_fc_graveyard = c->next;
        tv_sb_free(&c->ctl);
        tv_free(c->in);
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
    if (c->tls) { tv_tls_impl->close(c->tls, false); c->tls = NULL; }
    if (c->fd >= 0) close(c->fd); /* also leaves the event queue */
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

/* NO_PROXY: "*", or comma-separated hosts, domains ("example.com" also covers its subdomains,
 * as does ".example.com") and host:port pairs. */
static bool tv_no_proxy(const char *host, int port) {
    const char *list = getenv("no_proxy");
    if (!list || !*list) list = getenv("NO_PROXY");
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
    if (!spec && !r->unix_path.p->len && !tv_no_proxy(r->host, r->port)) {
        spec = https ? getenv("https_proxy") : getenv("http_proxy");
        if (!spec || !*spec) spec = https ? getenv("HTTPS_PROXY") : getenv("HTTP_PROXY");
        if (spec && !*spec) spec = NULL;
    }
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
    if (c->in_cap > 16384) { tv_free(c->in); c->in = NULL; c->in_cap = 0; }
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
    if (!c->connecting) { tv_fc_connected(c); return; }
    /* make the ClientHello (the key shares are the costly part) while TCP connects: it waits
     * in the TLS write buffer until the socket is writable */
    if (r->https && !r->via_proxy) tv_fc_start_tls(c);
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

static void tv_fc_ready(tv_io *h, bool readable, bool writable, bool broken) {
    tv_fc *c = (tv_fc *)h;
    if (c->dead) return;
    tv_fr *r = c->r;
    if (!r) {
        tv_io_quiet = true;
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
                 * the string it becomes, which tv_sb_map would otherwise reuse) */
                size_t rest = (size_t)r->remaining;
                bool all = r->whole && !tv_fr_decoding(r) && rest <= ((size_t)256 << 20);
                tv_sb_grow(sink, sink->len + (all ? rest : want));
            }
            long n = tv_fc_recv(c, sink->data + sink->len, want);
            if (n > 0) {
                r->got_any = true;
                sink->len += (size_t)n;
                r->remaining -= n;
                if (!tv_fr_decoding(r)) tv_fr_check_utf8(r);
                bool full = tv_fr_data(r);
                if (r->result != 1) return; /* the body couldn't be decoded */
                if (r->remaining == 0) { r->state = TV_FR_DONE; break; }
                if (full) { r->paused = true; tv_fc_interest(c, false, false); break; }
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
                size_t cap = c->in_cap ? c->in_cap * 2 : 65536;
                c->in = tv_realloc(c->in, cap);
                c->in_cap = cap;
            }
        }
        size_t room = c->in_cap - c->in_len;
        long n = tv_fc_recv(c, c->in + c->in_len, room);
        if (n > 0) {
            r->got_any = true;
            c->in_len += (size_t)n;
            if (tv_fetch_on_bytes) tv_fetch_on_bytes((size_t)n);
            int st = tv_fr_parse(r, c);
            if (r->result != 1) return;
            if (st < 0) {
                tv_fr_fail(r, "Malformed_HTTP_Response", "Malformed_HTTP_Response fetching \"%s\". %s", r->url.p->data, TV_FETCH_VERBOSE);
                return;
            }
            if (st > 0) break;
            if (r->streaming ? r->rbody.len >= TV_STREAM_HIGH || r->raw_off < r->raw.len : !r->whole && tv_fr_sink(r)->len >= TV_STREAM_HIGH) {
                r->paused = true;
                tv_fc_interest(c, false, false);
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
    if (!r) return TV_EMPTY_ARR;
    tv_arr a = tv_arr_with_capacity(&tv_type_u8, (tv_int)r->rbody.len);
    if (r->rbody.len) memcpy(tv_arr_data(a), r->rbody.data, r->rbody.len);
    a.len = (tv_int)r->rbody.len;
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
    if (r->c) {
        tv_fc *c = r->c;
        r->c = NULL;
        c->r = NULL;
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
    return tv_str_from((const char *)tv_arr_data(bytes), (size_t)bytes.len);
}

tv_arr tv_native_stringToBytes(tv_str s) {
    tv_arr a = tv_arr_with_capacity(&tv_type_u8, s.p->len);
    if (s.p->len) memcpy(tv_arr_data(a), s.p->data, (size_t)s.p->len);
    a.len = s.p->len;
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
    out.len = (tv_int)k;
    tv_free(lines);
    return out;
}

/* Each `name` line's value, in order (getSetCookie). */
tv_arr tv_native_headerValues(tv_str block, tv_str name) {
    const char *s = block.p->data, *end = s + block.p->len;
    size_t n = (size_t)name.p->len;
    tv_arr out = TV_EMPTY_ARR;
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
            out.len++;
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
    size_t n = (size_t)bytes.len;
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
    tv_arr out = TV_EMPTY_ARR;
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
            out.len += 5;
        }
        at = next;
    }
    tv_free(delim);
    if (why) {
        tv_arr_release(out, &tv_type_str);
        out = TV_EMPTY_ARR;
        tv_str *slot = tv_arr_reserve_tail(&out, &tv_type_str, 1);
        *slot = tv_str_from(why, strlen(why));
        out.len = 1;
    }
    return out;
}
