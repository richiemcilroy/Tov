/* big.c — `bigint`: arbitrary-precision integers, as JavaScript's BigInt.
 *
 * A tv_big is one machine word. An odd word is a small integer (the word shifted right by one:
 * -2^62 .. 2^62-1), so most values never touch the heap and add, subtract, multiply and compare
 * them inline (tov.h). An even word points to a tv_bigbuf: a reference-counted sign and magnitude
 * (64-bit limbs, least significant first, no leading zero limbs). Every value has one form: a
 * value that fits a small integer is always one, so equality and hashing can compare words.
 *
 * Buffers are immutable once shared. One held only by its owner (rc == 1) is updated in place by
 * compound assignment (`x += y`, `x *= y`) and by operations that consume their left operand,
 * so a loop that grows a number reuses its buffer.
 *
 * Multiplication is schoolbook below 32 limbs and Karatsuba above; division is Knuth's algorithm
 * D, with quotient digits from a precomputed reciprocal of the divisor's top limb
 * (Möller–Granlund), which keeps hardware division out of the inner loop. Decimal strings come
 * from division by 10^19, a limb at a time. */

#include "tov.h"

#include <stdlib.h>
#include <string.h>

typedef uint64_t tv_limb;
typedef unsigned __int128 tv_dlimb;

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif
#if !__has_builtin(__builtin_addcll)
/* (GCC before 14) */
static inline unsigned long long tv_addc(unsigned long long a, unsigned long long b, unsigned long long c, unsigned long long *out) {
    tv_dlimb s = (tv_dlimb)a + b + c;
    *out = (unsigned long long)(s >> 64);
    return (unsigned long long)s;
}
static inline unsigned long long tv_subc(unsigned long long a, unsigned long long b, unsigned long long c, unsigned long long *out) {
    unsigned long long d = a - b - c;
    *out = (a < b) | ((a == b) & c);
    return d;
}
#define __builtin_addcll tv_addc
#define __builtin_subcll tv_subc
#endif

#define TV_BIG_KARATSUBA 32

static inline tv_bigbuf *tv_bb(tv_big a) { return (tv_bigbuf *)(intptr_t)a.v; }
static inline tv_big tv_big_of(tv_bigbuf *b) { return (tv_big){ (int64_t)(intptr_t)b }; }

/* ---------------------------------------------------------------- buffers */

static inline size_t tv_bb_size(uint32_t cap) { return sizeof(tv_bigbuf) + (size_t)cap * sizeof(tv_limb); }

/* Buffers past the small allocator's sizes come in classes a quarter apart (64 limbs, 80, 96,
 * 112, 128, 160, ...), and freed ones wait on a list per class for the next number of that size:
 * a loop over numbers of thousands of digits reuses a few buffers instead of asking malloc for
 * new ones (whose large path is slow, on macOS a system call). The lists hold 32 MiB at most. */
#define TV_BB_CLASSES 96
static void *tv_bb_free_list[TV_BB_CLASSES];
static size_t tv_bb_cached;

static inline int tv_bb_class(uint32_t cap, uint32_t *rounded) {
    /* the smallest class >= cap: 2^e, 1.25 * 2^e, 1.5 * 2^e, 1.75 * 2^e */
    int e = 31 - __builtin_clz(cap);
    uint32_t base = (uint32_t)1 << e, step = base >> 2;
    uint32_t q = (cap - base + step - 1) / (step ? step : 1);
    if (q == 4) {
        e++;
        q = 0;
        base <<= 1;
        step = base >> 2;
    }
    *rounded = base + q * step;
    return (e - 6) * 4 + (int)q;
}

static tv_bigbuf *tv_bb_new(uint32_t cap) {
    if (cap < 2) cap = 2;
    tv_bigbuf *b;
    size_t size = tv_bb_size(cap);
    if (size <= 512) {
        b = tv_small_alloc(size);
    } else {
        if (cap < 64) cap = 64;
        uint32_t rounded;
        int c = tv_bb_class(cap, &rounded);
        if (c < TV_BB_CLASSES && tv_bb_free_list[c]) {
            b = tv_bb_free_list[c];
            tv_bb_free_list[c] = *(void **)b;
            tv_bb_cached -= tv_bb_size(rounded);
        } else {
            b = tv_alloc(tv_bb_size(rounded));
        }
        cap = rounded;
    }
    b->rc = 1;
    b->len = 0;
    b->cap = cap;
    b->neg = 0;
    return b;
}

void tv_big_free(tv_big a) {
    tv_bigbuf *b = tv_bb(a);
    size_t size = tv_bb_size(b->cap);
    if (size <= 512) {
        tv_small_free(b, size);
        return;
    }
    uint32_t rounded;
    int c = tv_bb_class(b->cap, &rounded);
    if (c < TV_BB_CLASSES && rounded == b->cap && tv_bb_cached + size <= ((size_t)32 << 20)) {
        *(void **)b = tv_bb_free_list[c];
        tv_bb_free_list[c] = b;
        tv_bb_cached += size;
        return;
    }
    free(b);
}

/* Scratch limbs for one operation (division's working copy): grown, never shrunk. */
static tv_limb *tv_big_scratch;
static size_t tv_big_scratch_n;
/* n limbs from s to d: a short copy inline (most numbers are a few limbs, and a call into the C
 * library's memcpy costs more than the copy), a long one through memcpy. (The empty asm keeps
 * the compiler from turning the loop back into a memcpy call.) */
static inline void tv_limbs_copy(tv_limb *d, const tv_limb *s, size_t n) {
    if (n > 16) {
        memcpy(d, s, n * sizeof(tv_limb));
        return;
    }
    size_t i = 0;
    for (; i + 2 <= n; i += 2) {
        tv_limb a = s[i], b = s[i + 1];
        __asm__("" : "+r"(a), "+r"(b));
        d[i] = a;
        d[i + 1] = b;
    }
    if (i < n) d[i] = s[i];
}

static tv_limb *tv_scratch(size_t n) {
    if (n > tv_big_scratch_n) {
        free(tv_big_scratch);
        tv_big_scratch_n = n + n / 2 + 16;
        tv_big_scratch = tv_alloc(tv_big_scratch_n * sizeof(tv_limb));
    }
    return tv_big_scratch;
}

/* Room for `cap` limbs in a buffer its owner holds alone (the contents are kept). */
static tv_bigbuf *tv_bb_reserve(tv_bigbuf *b, uint32_t cap) {
    if (b->cap >= cap) return b;
    uint32_t ncap = cap + cap / 4 + 2;
    tv_bigbuf *n = tv_bb_new(ncap);
    n->len = b->len;
    n->neg = b->neg;
    tv_limbs_copy(n->d, b->d, b->len);
    tv_big_free(tv_big_of(b));
    return n;
}

/* The canonical value of a buffer just computed (its owner's): leading zero limbs dropped, and
 * a magnitude that fits a small integer becomes one. */
static tv_big tv_bb_finish(tv_bigbuf *b) {
    uint32_t n = b->len;
    while (n > 0 && b->d[n - 1] == 0) n--;
    b->len = n;
    if (n <= 1) {
        tv_limb m = n ? b->d[0] : 0;
        if (m < ((tv_limb)1 << 62) || (b->neg && m == ((tv_limb)1 << 62))) {
            int64_t v = b->neg ? -(int64_t)m : (int64_t)m;
            tv_big_free(tv_big_of(b));
            return tv_big_small(v);
        }
    }
    if (n == 0) b->neg = 0;
    return tv_big_of(b);
}

tv_big tv_big_from_i64_slow(int64_t x) {
    tv_bigbuf *b = tv_bb_new(2);
    b->neg = x < 0;
    b->d[0] = x < 0 ? (tv_limb)0 - (tv_limb)x : (tv_limb)x;
    b->len = 1;
    return tv_big_of(b);
}

static tv_big tv_big_from_u64(uint64_t m, bool neg) {
    if (m < ((uint64_t)1 << 62) || (neg && m == ((uint64_t)1 << 62))) return tv_big_small(neg ? -(int64_t)m : (int64_t)m);
    tv_bigbuf *b = tv_bb_new(2);
    b->neg = neg;
    b->d[0] = m;
    b->len = 1;
    return tv_big_of(b);
}

static tv_big tv_big_from_i128(__int128 x);
tv_big tv_big_from_i128_(__int128 x) { return tv_big_from_i128(x); }

static tv_big tv_big_from_i128(__int128 x) {
    if (x >= TV_BIG_SMALL_MIN && x <= TV_BIG_SMALL_MAX) return tv_big_small((int64_t)x);
    tv_bigbuf *b = tv_bb_new(2);
    b->neg = x < 0;
    unsigned __int128 m = x < 0 ? (unsigned __int128)0 - (unsigned __int128)x : (unsigned __int128)x;
    b->d[0] = (tv_limb)m;
    b->d[1] = (tv_limb)(m >> 64);
    b->len = 2;
    return tv_bb_finish(b);
}

/* A value's sign and magnitude, borrowed (a small one's limb lives in the view). */
typedef struct {
    const tv_limb *d;
    uint32_t n;
    bool neg;
    tv_limb one;
} tv_bview;

static inline void tv_view(tv_big a, tv_bview *v) {
    if (tv_big_is_small(a)) {
        int64_t x = a.v >> 1;
        v->neg = x < 0;
        v->one = x < 0 ? (tv_limb)0 - (tv_limb)x : (tv_limb)x;
        v->d = &v->one;
        v->n = x != 0;
    } else {
        tv_bigbuf *b = tv_bb(a);
        v->d = b->d;
        v->n = b->len;
        v->neg = b->neg != 0;
    }
}

/* ---------------------------------------------------------------- magnitudes */

static int tv_mag_cmp(const tv_limb *a, uint32_t an, const tv_limb *b, uint32_t bn) {
    if (an != bn) return an < bn ? -1 : 1;
    for (uint32_t i = an; i-- > 0;) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

/* r = a + b (an >= bn; r has an limbs and may be a); returns the carry. */
static tv_limb tv_mag_add(tv_limb *r, const tv_limb *a, uint32_t an, const tv_limb *b, uint32_t bn) {
    unsigned long long c = 0;
    uint32_t i = 0;
    for (; i + 8 <= bn; i += 8) {
        tv_limb x[8], y[8];
        for (int j = 0; j < 8; j++) {
            x[j] = a[i + j];
            y[j] = b[i + j];
        }
        for (int j = 0; j < 8; j++) r[i + j] = __builtin_addcll(x[j], y[j], c, &c);
    }
    for (; i < bn; i++) r[i] = __builtin_addcll(a[i], b[i], c, &c);
    for (; i < an; i++) {
        tv_limb s = a[i] + c;
        c = s < c;
        r[i] = s;
    }
    return c;
}

/* r = a - b (a >= b, an >= bn; r has an limbs and may be a). */
static void tv_mag_sub(tv_limb *r, const tv_limb *a, uint32_t an, const tv_limb *b, uint32_t bn) {
    unsigned long long borrow = 0;
    uint32_t i = 0;
    for (; i + 8 <= bn; i += 8) {
        tv_limb x[8], y[8];
        for (int j = 0; j < 8; j++) {
            x[j] = a[i + j];
            y[j] = b[i + j];
        }
        for (int j = 0; j < 8; j++) r[i + j] = __builtin_subcll(x[j], y[j], borrow, &borrow);
    }
    for (; i < bn; i++) r[i] = __builtin_subcll(a[i], b[i], borrow, &borrow);
    for (; i < an; i++) {
        tv_limb x = a[i];
        r[i] = x - borrow;
        borrow = x < borrow;
    }
}

/* x = y - x (y >= x, yn >= xn; x has room for yn limbs). */
static void tv_mag_rsub(tv_limb *x, uint32_t xn, const tv_limb *y, uint32_t yn) {
    unsigned long long borrow = 0;
    uint32_t i = 0;
    for (; i < xn; i++) x[i] = __builtin_subcll(y[i], x[i], borrow, &borrow);
    for (; i < yn; i++) {
        tv_limb v = y[i];
        x[i] = v - borrow;
        borrow = v < borrow;
    }
}

/* The limb loops below take four limbs a step: four independent products, then their low and
 * high halves summed along one add-with-carry chain (one cycle a link), and the second operand
 * added (or subtracted) along a chain of its own. About twice as fast as a limb at a time, whose
 * carry goes through a 128-bit add at every step. */

#if defined(__clang__)
/* The products of a[0..4) and m as four limbs plus a high carry: p[0..4) + (c_in) */
#define TV_MUL4(a, m, c, s0, s1, s2, s3)                                                  \
    do {                                                                                  \
        tv_dlimb p0_ = (tv_dlimb)(a)[0] * (m), p1_ = (tv_dlimb)(a)[1] * (m);              \
        tv_dlimb p2_ = (tv_dlimb)(a)[2] * (m), p3_ = (tv_dlimb)(a)[3] * (m);              \
        unsigned long long k_;                                                            \
        s0 = __builtin_addcll((tv_limb)p0_, (c), 0, &k_);                                 \
        s1 = __builtin_addcll((tv_limb)p1_, (tv_limb)(p0_ >> 64), k_, &k_);               \
        s2 = __builtin_addcll((tv_limb)p2_, (tv_limb)(p1_ >> 64), k_, &k_);               \
        s3 = __builtin_addcll((tv_limb)p3_, (tv_limb)(p2_ >> 64), k_, &k_);               \
        (c) = (tv_limb)(p3_ >> 64) + k_;                                                  \
    } while (0)

/* Eight products summed along one carry chain: o[0..8) = a[0..8) * m + c, c = the high limb. */
#define TV_MUL8(a, m, c, o)                                                                   \
    do {                                                                                      \
        tv_dlimb p_[8];                                                                       \
        for (int j_ = 0; j_ < 8; j_++) p_[j_] = (tv_dlimb)(a)[j_] * (m);                      \
        unsigned long long k_;                                                                \
        (o)[0] = __builtin_addcll((tv_limb)p_[0], (c), 0, &k_);                               \
        for (int j_ = 1; j_ < 8; j_++) (o)[j_] = __builtin_addcll((tv_limb)p_[j_], (tv_limb)(p_[j_ - 1] >> 64), k_, &k_); \
        (c) = (tv_limb)(p_[7] >> 64) + k_;                                                    \
    } while (0)
#else
/* GCC: each limb summed in 128 bits instead (low half, the previous product's high half, the
 * carry: < 2^66), which it compiles to straight adds; through __builtin_addcll's chain it was
 * 1.5x slower. (clang is ~1.6x slower this way.) */
#define TV_MUL4(a, m, c, s0, s1, s2, s3)                                                  \
    do {                                                                                  \
        tv_dlimb p0_ = (tv_dlimb)(a)[0] * (m), p1_ = (tv_dlimb)(a)[1] * (m);              \
        tv_dlimb p2_ = (tv_dlimb)(a)[2] * (m), p3_ = (tv_dlimb)(a)[3] * (m);              \
        tv_dlimb s_ = (tv_dlimb)(tv_limb)p0_ + (c);                                       \
        s0 = (tv_limb)s_;                                                                 \
        s_ = (tv_dlimb)(tv_limb)p1_ + (tv_limb)(p0_ >> 64) + (tv_limb)(s_ >> 64);         \
        s1 = (tv_limb)s_;                                                                 \
        s_ = (tv_dlimb)(tv_limb)p2_ + (tv_limb)(p1_ >> 64) + (tv_limb)(s_ >> 64);         \
        s2 = (tv_limb)s_;                                                                 \
        s_ = (tv_dlimb)(tv_limb)p3_ + (tv_limb)(p2_ >> 64) + (tv_limb)(s_ >> 64);         \
        s3 = (tv_limb)s_;                                                                 \
        (c) = (tv_limb)(p3_ >> 64) + (tv_limb)(s_ >> 64);                                 \
    } while (0)

/* Eight products summed the same way: o[0..8) = a[0..8) * m + c, c = the high limb. */
#define TV_MUL8(a, m, c, o)                                                                   \
    do {                                                                                      \
        tv_dlimb p_[8];                                                                       \
        for (int j_ = 0; j_ < 8; j_++) p_[j_] = (tv_dlimb)(a)[j_] * (m);                      \
        tv_dlimb s_ = (tv_dlimb)(tv_limb)p_[0] + (c);                                         \
        (o)[0] = (tv_limb)s_;                                                                 \
        for (int j_ = 1; j_ < 8; j_++) {                                                      \
            s_ = (tv_dlimb)(tv_limb)p_[j_] + (tv_limb)(p_[j_ - 1] >> 64) + (tv_limb)(s_ >> 64); \
            (o)[j_] = (tv_limb)s_;                                                            \
        }                                                                                     \
        (c) = (tv_limb)(p_[7] >> 64) + (tv_limb)(s_ >> 64);                                   \
    } while (0)
#endif

/* r = a * m (r has n limbs, may be a); returns the high limb. */
static tv_limb tv_mag_mul1(tv_limb *r, const tv_limb *a, uint32_t n, tv_limb m) {
    tv_limb c = 0;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        tv_limb o[8];
        TV_MUL8(a + i, m, c, o);
        for (int j = 0; j < 8; j++) r[i + j] = o[j];
    }
    for (; i + 4 <= n; i += 4) {
        tv_limb s0, s1, s2, s3;
        TV_MUL4(a + i, m, c, s0, s1, s2, s3);
        r[i] = s0;
        r[i + 1] = s1;
        r[i + 2] = s2;
        r[i + 3] = s3;
    }
    for (; i < n; i++) {
        tv_dlimb p = (tv_dlimb)a[i] * m + c;
        r[i] = (tv_limb)p;
        c = (tv_limb)(p >> 64);
    }
    return c;
}

/* r = z + a * m over n limbs (r may be z or a); returns the carry out of r[n-1]. */
static tv_limb tv_mag_addmul_n(tv_limb *r, const tv_limb *z, const tv_limb *a, uint32_t n, tv_limb m) {
    tv_limb c = 0;
    unsigned long long cz = 0;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        tv_limb o[8], zz[8];
        TV_MUL8(a + i, m, c, o);
        for (int j = 0; j < 8; j++) zz[j] = z[i + j];
        for (int j = 0; j < 8; j++) r[i + j] = __builtin_addcll(o[j], zz[j], cz, &cz);
    }
    for (; i + 4 <= n; i += 4) {
        tv_limb s0, s1, s2, s3;
        TV_MUL4(a + i, m, c, s0, s1, s2, s3);
        tv_limb z0 = z[i], z1 = z[i + 1], z2 = z[i + 2], z3 = z[i + 3];
        r[i] = __builtin_addcll(s0, z0, cz, &cz);
        r[i + 1] = __builtin_addcll(s1, z1, cz, &cz);
        r[i + 2] = __builtin_addcll(s2, z2, cz, &cz);
        r[i + 3] = __builtin_addcll(s3, z3, cz, &cz);
    }
    c += cz;
    for (; i < n; i++) {
        tv_dlimb p = (tv_dlimb)a[i] * m + z[i] + c;
        r[i] = (tv_limb)p;
        c = (tv_limb)(p >> 64);
    }
    return c;
}

/* r = z - a * m over n limbs (r may be z or a); returns the borrow out of r[n-1] (the high limb
 * of the product beyond n, plus one if the subtraction borrowed). */
static tv_limb tv_mag_submul_n(tv_limb *r, const tv_limb *z, const tv_limb *a, uint32_t n, tv_limb m) {
    tv_limb c = 0;
    unsigned long long bz = 0;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        tv_limb o[8], zz[8];
        TV_MUL8(a + i, m, c, o);
        for (int j = 0; j < 8; j++) zz[j] = z[i + j];
        for (int j = 0; j < 8; j++) r[i + j] = __builtin_subcll(zz[j], o[j], bz, &bz);
    }
    for (; i + 4 <= n; i += 4) {
        tv_limb s0, s1, s2, s3;
        TV_MUL4(a + i, m, c, s0, s1, s2, s3);
        tv_limb z0 = z[i], z1 = z[i + 1], z2 = z[i + 2], z3 = z[i + 3];
        r[i] = __builtin_subcll(z0, s0, bz, &bz);
        r[i + 1] = __builtin_subcll(z1, s1, bz, &bz);
        r[i + 2] = __builtin_subcll(z2, s2, bz, &bz);
        r[i + 3] = __builtin_subcll(z3, s3, bz, &bz);
    }
    c += bz;
    for (; i < n; i++) {
        tv_dlimb p = (tv_dlimb)a[i] * m + c;
        tv_limb lo = (tv_limb)p;
        c = (tv_limb)(p >> 64);
        tv_limb x = z[i];
        r[i] = x - lo;
        c += x < lo;
    }
    return c;
}

/* r += a * m over n limbs; returns the carry out of r[n-1]. */
static tv_limb tv_mag_addmul1(tv_limb *r, const tv_limb *a, uint32_t n, tv_limb m) { return tv_mag_addmul_n(r, r, a, n, m); }

/* r -= a * m over n limbs; returns the borrow out of r[n-1]. */
static tv_limb tv_mag_submul1(tv_limb *r, const tv_limb *a, uint32_t n, tv_limb m) { return tv_mag_submul_n(r, r, a, n, m); }

/* r = a * b, schoolbook (r has an + bn limbs, distinct from a and b; bn >= 1). */
static void tv_mag_mul_school(tv_limb *r, const tv_limb *a, uint32_t an, const tv_limb *b, uint32_t bn) {
    r[an] = tv_mag_mul1(r, a, an, b[0]);
    for (uint32_t j = 1; j < bn; j++) r[an + j] = tv_mag_addmul1(r + j, a, an, b[j]);
}

static void tv_mag_mul(tv_limb *r, const tv_limb *a, uint32_t an, const tv_limb *b, uint32_t bn);

/* r[0..n) += a[0..an) (an <= n); the carry runs through r. */
static void tv_mag_add_into(tv_limb *r, uint32_t n, const tv_limb *a, uint32_t an) {
    tv_limb c = tv_mag_add(r, r, an, a, an);
    for (uint32_t i = an; c && i < n; i++) {
        r[i] += 1;
        c = r[i] == 0;
    }
}

static inline uint32_t tv_mag_norm(const tv_limb *a, uint32_t n) {
    while (n > 0 && a[n - 1] == 0) n--;
    return n;
}

/* Karatsuba: an >= bn >= TV_BIG_KARATSUBA, an < 2 * bn. */
static void tv_mag_mul_kara(tv_limb *r, const tv_limb *a, uint32_t an, const tv_limb *b, uint32_t bn) {
    uint32_t h = an / 2;
    const tv_limb *a0 = a, *a1 = a + h, *b0 = b, *b1 = b + h;
    uint32_t a1n = an - h, b1n = bn - h;
    uint32_t a0n = tv_mag_norm(a0, h), b0n = tv_mag_norm(b0, h);
    /* z0 = a0*b0 in r[0, 2h), z2 = a1*b1 in r[2h, an + bn) */
    memset(r, 0, (size_t)(an + bn) * sizeof(tv_limb));
    if (a0n && b0n) tv_mag_mul(r, a0, a0n, b0, b0n);
    tv_mag_mul(r + 2 * h, a1, a1n, b1, b1n);
    /* z1 = (a0 + a1)(b0 + b1) - z0 - z2 */
    uint32_t sbn = (b1n > h ? b1n : h) + 1, san = a1n + 1;
    tv_limb *tmp = tv_alloc((size_t)(san + sbn + san + sbn) * sizeof(tv_limb));
    tv_limb *sa = tmp, *sb = tmp + san, *z1 = tmp + san + sbn;
    sa[a1n] = tv_mag_add(sa, a1, a1n, a0, a0n);
    if (b1n >= b0n) {
        memset(sb, 0, (size_t)sbn * sizeof(tv_limb));
        sb[b1n] = tv_mag_add(sb, b1, b1n, b0, b0n);
    } else {
        memset(sb, 0, (size_t)sbn * sizeof(tv_limb));
        sb[b0n] = tv_mag_add(sb, b0, b0n, b1, b1n);
    }
    uint32_t sn = tv_mag_norm(sa, san), tn = tv_mag_norm(sb, sbn);
    uint32_t zn = sn + tn;
    if (sn && tn) {
        if (sn >= tn) tv_mag_mul(z1, sa, sn, sb, tn);
        else tv_mag_mul(z1, sb, tn, sa, sn);
    } else {
        zn = 0;
    }
    uint32_t z0n = tv_mag_norm(r, 2 * h), z2n = tv_mag_norm(r + 2 * h, an + bn - 2 * h);
    zn = tv_mag_norm(z1, zn);
    if (z0n) tv_mag_sub(z1, z1, zn, r, z0n);
    zn = tv_mag_norm(z1, zn);
    if (z2n) tv_mag_sub(z1, z1, zn, r + 2 * h, z2n);
    zn = tv_mag_norm(z1, zn);
    if (zn) tv_mag_add_into(r + h, an + bn - h, z1, zn);
    free(tmp);
}

/* r = a * b (r has an + bn limbs, distinct from a and b; an, bn >= 1). */
static void tv_mag_mul(tv_limb *r, const tv_limb *a, uint32_t an, const tv_limb *b, uint32_t bn) {
    if (an < bn) {
        const tv_limb *t = a; a = b; b = t;
        uint32_t tn = an; an = bn; bn = tn;
    }
    if (bn < TV_BIG_KARATSUBA) {
        tv_mag_mul_school(r, a, an, b, bn);
        return;
    }
    if (an < 2 * bn) {
        tv_mag_mul_kara(r, a, an, b, bn);
        return;
    }
    /* unbalanced: a in pieces of bn limbs, each multiplied by b and added in */
    memset(r, 0, (size_t)(an + bn) * sizeof(tv_limb));
    tv_limb *t = tv_alloc((size_t)(2 * bn) * sizeof(tv_limb));
    for (uint32_t off = 0; off < an; off += bn) {
        uint32_t pn = an - off < bn ? an - off : bn;
        uint32_t pnn = tv_mag_norm(a + off, pn);
        if (!pnn) continue;
        tv_mag_mul(t, a + off, pnn, b, bn);
        tv_mag_add_into(r + off, an + bn - off, t, pnn + bn);
    }
    free(t);
}

/* (u1, u0) / v for a normalized v (top bit set) and u1 < v, in 32-bit halves with the
 * hardware's 64-bit divide (Hacker's Delight's divlu): far cheaper than a general 128-bit
 * division, which compilers leave to a library routine. */
static inline tv_limb tv_divlu(tv_limb u1, tv_limb u0, tv_limb v) {
    const tv_limb b = (tv_limb)1 << 32;
    tv_limb vn1 = v >> 32, vn0 = v & 0xffffffffu, un1 = u0 >> 32, un0 = u0 & 0xffffffffu;
    tv_limb q1 = u1 / vn1, rhat = u1 - q1 * vn1;
    while (q1 >= b || q1 * vn0 > ((rhat << 32) | un1)) {
        q1--;
        rhat += vn1;
        if (rhat >= b) break;
    }
    tv_limb un21 = (u1 << 32) + un1 - q1 * v;
    tv_limb q0 = un21 / vn1;
    rhat = un21 - q0 * vn1;
    while (q0 >= b || q0 * vn0 > ((rhat << 32) | un0)) {
        q0--;
        rhat += vn1;
        if (rhat >= b) break;
    }
    return (q1 << 32) | q0;
}

/* The reciprocal of a normalized divisor (top bit set): floor((B^2 - 1) / d) - B. */
static inline tv_limb tv_recip(tv_limb d) { return tv_divlu(~d, ~(tv_limb)0, d); }

/* (nh, nl) / d for a normalized d with reciprocal di, nh < d (Möller–Granlund). */
static inline tv_limb tv_div_preinv(tv_limb nh, tv_limb nl, tv_limb d, tv_limb di, tv_limb *rem) {
    tv_dlimb p = (tv_dlimb)nh * di + (((tv_dlimb)(nh + 1) << 64) | nl);
    tv_limb q = (tv_limb)(p >> 64), q0 = (tv_limb)p;
    tv_limb r = nl - q * d;
    if (r > q0) {
        q--;
        r += d;
    }
    if (TV_BIG_UNLIKELY(r >= d)) {
        q++;
        r -= d;
    }
    *rem = r;
    return q;
}

/* q = a / d (q has n limbs, may be a); returns a % d. */
static tv_limb tv_mag_divrem1(tv_limb *q, const tv_limb *a, uint32_t n, tv_limb d) {
    int s = __builtin_clzll(d);
    tv_limb dn = d << s, di = tv_recip(dn), r = 0;
    if (s == 0) {
        for (uint32_t i = n; i-- > 0;) q[i] = tv_div_preinv(r, a[i], dn, di, &r);
        return r;
    }
    r = a[n - 1] >> (64 - s);
    for (uint32_t i = n; i-- > 0;) {
        tv_limb nl = (a[i] << s) | (i ? a[i - 1] >> (64 - s) : 0);
        q[i] = tv_div_preinv(r, nl, dn, di, &r);
    }
    return r >> s;
}

/* Knuth's algorithm D: q = a / b (n - m + 1 limbs, or NULL), r = a % b (m limbs, or NULL);
 * n >= m >= 2, b[m-1] != 0. Neither operand is shifted: each quotient digit is estimated from
 * the top limbs as the normalized algorithm would see them, and the remainder is worked out on a
 * copy of the dividend. A quotient of one limb with no remainder wanted (pidigits' `x / y` < 10)
 * isn't worked out at all: one read-only pass checks the estimate. */
static void tv_mag_divrem(tv_limb *q, tv_limb *r, const tv_limb *a, uint32_t n, const tv_limb *b, uint32_t m) {
    int s = __builtin_clzll(b[m - 1]);
    /* the divisor's top two limbs, normalized */
    tv_limb v1 = b[m - 1], v0 = b[m - 2];
    if (s) {
        v1 = (v1 << s) | (v0 >> (64 - s));
        v0 = (v0 << s) | (m >= 3 ? b[m - 3] >> (64 - s) : 0);
    }
    tv_limb di = tv_recip(v1);
    /* the window's top three limbs, normalized: u[k] for k = j + m, j + m - 1, j + m - 2 */
#define TV_UN(u, k) (s ? (((u)[k] << s) | ((k) > 0 ? (u)[(k) - 1] >> (64 - s) : 0)) : (u)[k])
    if (!r && n == m) {
        /* the quotient is one limb: estimate it, then check it against a without changing a */
        tv_limb u2 = s ? a[n - 1] >> (64 - s) : 0;
        tv_limb u1 = s ? (a[n - 1] << s) | (a[n - 2] >> (64 - s)) : a[n - 1];
        tv_limb u0 = s ? (a[n - 2] << s) | (n >= 3 ? a[n - 3] >> (64 - s) : 0) : a[n - 2];
        tv_limb qhat, rhat;
        bool big_r = false;
        if (u2 >= v1) {
            qhat = ~(tv_limb)0;
            rhat = u1 + v1;
            big_r = rhat < u1;
        } else {
            qhat = tv_div_preinv(u2, u1, v1, di, &rhat);
        }
        while (!big_r && (tv_dlimb)qhat * v0 > (((tv_dlimb)rhat << 64) | u0)) {
            qhat--;
            tv_limb old = rhat;
            rhat += v1;
            big_r = rhat < old;
        }
        /* a - qhat * b < 0? (qhat is at most one too large) */
        /* a - qhat*b = E * B^(m-2) + (a's lower limbs) - qhat * (b's lower limbs), where E is what
         * the top limbs leave: (rhat*B + u0) - qhat*v0. The last term is under qhat * B^(m-2), so
         * E >= qhat means qhat is right; only otherwise is the whole product checked. */
        bool sure = big_r || ((((tv_dlimb)rhat << 64) | u0) - (tv_dlimb)qhat * v0) >= qhat;
        if (qhat && !sure && tv_mag_submul_n(tv_scratch(m), a, b, m, qhat) != 0) qhat--;   /* (a has no limb above m - 1: anything left over is negative) */
        q[0] = qhat;
        return;
    }
    tv_limb *u = tv_scratch((size_t)n + 1);
    tv_limbs_copy(u, a, n);
    u[n] = 0;
    for (uint32_t j = n - m + 1; j-- > 0;) {
        uint32_t k = j + m;
        tv_limb u2 = TV_UN(u, k), u1 = TV_UN(u, k - 1), u0 = TV_UN(u, k - 2);
        tv_limb qhat, rhat;
        bool big_r = false;
        if (u2 >= v1) {
            qhat = ~(tv_limb)0;
            rhat = u1 + v1;
            big_r = rhat < u1;
        } else {
            qhat = tv_div_preinv(u2, u1, v1, di, &rhat);
        }
        while (!big_r && (tv_dlimb)qhat * v0 > (((tv_dlimb)rhat << 64) | u0)) {
            qhat--;
            tv_limb old = rhat;
            rhat += v1;
            big_r = rhat < old;
        }
        tv_limb top = u[k];
        tv_limb borrow;
        if (m <= 4) {
            /* (a divisor of up to four limbs: the product subtracted inline) */
            tv_limb c = 0;
            for (uint32_t i = 0; i < 4; i++) {
                if (i >= m) break;
                tv_dlimb p = (tv_dlimb)b[i] * qhat + c;
                tv_limb lo = (tv_limb)p, x = u[j + i];
                c = (tv_limb)(p >> 64) + (x < lo);
                u[j + i] = x - lo;
            }
            borrow = c;
        } else {
            borrow = tv_mag_submul1(u + j, b, m, qhat);
        }
        if (top < borrow) {
            /* one too many: add the divisor back */
            qhat--;
            u[k] = top - borrow + tv_mag_add(u + j, u + j, m, b, m);
        } else {
            u[k] = top - borrow;
        }
        if (q) q[j] = qhat;
    }
#undef TV_UN
    if (r) tv_limbs_copy(r, u, m);
}

/* ---------------------------------------------------------------- add, subtract, multiply */

/* The sum of two views with signs (the result's buffer is new). */
static tv_big tv_big_addv(const tv_bview *x, const tv_bview *y, bool yneg) {
    const tv_bview *a = x, *b = y;
    bool aneg = x->neg, bneg = yneg;
    if (aneg == bneg) {
        if (a->n < b->n) { const tv_bview *t = a; a = b; b = t; }
        tv_bigbuf *r = tv_bb_new(a->n + 1);
        r->d[a->n] = tv_mag_add(r->d, a->d, a->n, b->d, b->n);
        r->len = a->n + 1;
        r->neg = aneg;
        return tv_bb_finish(r);
    }
    int c = tv_mag_cmp(a->d, a->n, b->d, b->n);
    if (c == 0) return tv_big_small(0);
    if (c < 0) {
        const tv_bview *t = a; a = b; b = t;
        aneg = bneg;
    }
    tv_bigbuf *r = tv_bb_new(a->n);
    tv_mag_sub(r->d, a->d, a->n, b->d, b->n);
    r->len = a->n;
    r->neg = aneg;
    return tv_bb_finish(r);
}

tv_big tv_big_add_slow(tv_big a, tv_big b) {
    if (tv_big_is_small(a) && tv_big_is_small(b)) return tv_big_from_i64((a.v >> 1) + (b.v >> 1));
    tv_bview x, y;
    tv_view(a, &x);
    tv_view(b, &y);
    return tv_big_addv(&x, &y, y.neg);
}

tv_big tv_big_sub_slow(tv_big a, tv_big b) {
    if (tv_big_is_small(a) && tv_big_is_small(b)) return tv_big_from_i64((a.v >> 1) - (b.v >> 1));
    tv_bview x, y;
    tv_view(a, &x);
    tv_view(b, &y);
    return tv_big_addv(&x, &y, y.n ? !y.neg : false);
}

/* r[0..an+bn) = a * b for numbers of at most four limbs each (256 bits: elliptic curves, hashes),
 * in one inlined loop nest the compiler unrolls, with no calls or size dispatch. */
static inline void tv_mag_mul_small(tv_limb *restrict r, const tv_limb *a, uint32_t an, const tv_limb *b, uint32_t bn) {
    /* (the first row written, the rest added in: no buffer to clear or copy, which the compiler
     * would make calls to memset and memcpy) */
    tv_limb c = 0, b0 = b[0];
    for (uint32_t i = 0; i < 4; i++) {
        if (i >= an) break;
        tv_dlimb p = (tv_dlimb)a[i] * b0 + c;
        r[i] = (tv_limb)p;
        c = (tv_limb)(p >> 64);
    }
    r[an] = c;
    for (uint32_t j = 1; j < 4; j++) {
        if (j >= bn) break;
        tv_limb bj = b[j];
        c = 0;
        for (uint32_t i = 0; i < 4; i++) {
            if (i >= an) break;
            tv_dlimb p = (tv_dlimb)a[i] * bj + r[i + j] + c;
            r[i + j] = (tv_limb)p;
            c = (tv_limb)(p >> 64);
        }
        r[an + j] = c;
    }
}

/* r (8 limbs) = a^2 for a of 4 limbs: each cross product once, doubled, and the squares added
 * (10 limb products where multiplying would take 16). */
static inline void tv_mag_sqr4(tv_limb *restrict r, const tv_limb *a) {
    tv_limb t[8] = {0};
    for (int i = 0; i < 3; i++) {
        tv_limb c = 0;
        for (int j = i + 1; j < 4; j++) {
            tv_dlimb p = (tv_dlimb)a[i] * a[j] + t[i + j] + c;
            t[i + j] = (tv_limb)p;
            c = (tv_limb)(p >> 64);
        }
        t[i + 4] = c;
    }
    for (int k = 7; k > 0; k--) t[k] = (t[k] << 1) | (t[k - 1] >> 63);
    t[0] <<= 1;
    unsigned long long c = 0;
    for (int i = 0; i < 4; i++) {
        tv_dlimb p = (tv_dlimb)a[i] * a[i];
        r[2 * i] = __builtin_addcll(t[2 * i], (tv_limb)p, c, &c);
        r[2 * i + 1] = __builtin_addcll(t[2 * i + 1], (tv_limb)(p >> 64), c, &c);
    }
}

tv_big tv_big_mul_slow(tv_big a, tv_big b) {
    if (tv_big_is_small(a) && tv_big_is_small(b)) return tv_big_from_i128((__int128)(a.v >> 1) * (b.v >> 1));
    tv_bview x, y;
    tv_view(a, &x);
    tv_view(b, &y);
    if (!x.n || !y.n) return tv_big_small(0);
    tv_bigbuf *r = tv_bb_new(x.n + y.n);
    if (x.d == y.d && x.n == 4) tv_mag_sqr4(r->d, x.d);
    else if (x.n <= 4 && y.n <= 4) tv_mag_mul_small(r->d, x.d, x.n, y.d, y.n);
    else if (y.n == 1) r->d[x.n] = tv_mag_mul1(r->d, x.d, x.n, y.d[0]);
    else if (x.n == 1) r->d[y.n] = tv_mag_mul1(r->d, y.d, y.n, x.d[0]);
    else tv_mag_mul(r->d, x.d, x.n, y.d, y.n);
    r->len = x.n + y.n;
    r->neg = x.neg != y.neg;
    return tv_bb_finish(r);
}

tv_big tv_big_neg(tv_big a) {
    if (tv_big_is_small(a)) return tv_big_from_i64(-(a.v >> 1));
    tv_bigbuf *s = tv_bb(a);
    tv_bigbuf *r = tv_bb_new(s->len);
    tv_limbs_copy(r->d, s->d, s->len);
    r->len = s->len;
    r->neg = !s->neg;
    return tv_bb_finish(r);
}

/* -a, consuming a (its buffer flips sign when it's the only reference). */
tv_big tv_big_neg_take(tv_big a) {
    if (!tv_big_is_small(a) && tv_bb(a)->rc == 1) {
        tv_bigbuf *b = tv_bb(a);
        b->neg = !b->neg;
        return tv_bb_finish(b);
    }
    tv_big r = tv_big_neg(a);
    tv_big_release(a);
    return r;
}

/* x op= y in place when x's buffer is its own; otherwise as `x = x op y`. */
static bool tv_big_unique(tv_big a) { return !tv_big_is_small(a) && tv_bb(a)->rc == 1; }

void tv_big_add_to_slow(tv_big *x, tv_big y, bool sub) {
    tv_bview v;
    tv_view(y, &v);
    bool yneg = sub ? (v.n ? !v.neg : false) : v.neg;
    if (tv_big_unique(*x) && x->v != y.v) {
        tv_bigbuf *b = tv_bb(*x);
        if ((b->neg != 0) == yneg || !v.n) {
            /* same signs: the magnitudes add, in place */
            uint32_t n = b->len > v.n ? b->len : v.n;
            if (b->cap < n + 1) b = tv_bb_reserve(b, n + 1);
            if (b->len < n) memset(b->d + b->len, 0, (size_t)(n - b->len) * sizeof(tv_limb));
            b->d[n] = tv_mag_add(b->d, b->d, n, v.d, v.n);
            b->len = n + 1;
            *x = tv_bb_finish(b);
            return;
        }
        int c = tv_mag_cmp(b->d, b->len, v.d, v.n);
        if (c >= 0) {
            /* opposite signs, |x| >= |y|: the sign stays, the magnitude shrinks in place */
            tv_mag_sub(b->d, b->d, b->len, v.d, v.n);
            *x = tv_bb_finish(b);
            return;
        }
        if (b->cap >= v.n) {
            /* opposite signs, |x| < |y|: y's sign, |y| - |x|, in place */
            tv_mag_rsub(b->d, b->len, v.d, v.n);
            b->len = v.n;
            b->neg = yneg;
            *x = tv_bb_finish(b);
            return;
        }
    }
    tv_bview u;
    tv_view(*x, &u);
    tv_big r = tv_big_addv(&u, &v, yneg);
    tv_big_release(*x);
    *x = r;
}

void tv_big_mul_to_slow(tv_big *x, tv_big y) {
    if (tv_big_unique(*x) && tv_big_is_small(y)) {
        int64_t m = y.v >> 1;
        tv_bigbuf *b = tv_bb(*x);
        if (m == 0) {
            tv_big_release(*x);
            *x = tv_big_small(0);
            return;
        }
        if (b->cap < b->len + 1) b = tv_bb_reserve(b, b->len + 1);
        tv_limb mm = m < 0 ? (tv_limb)0 - (tv_limb)m : (tv_limb)m;
        b->d[b->len] = tv_mag_mul1(b->d, b->d, b->len, mm);
        b->len++;
        if (m < 0) b->neg = !b->neg;
        *x = tv_bb_finish(b);
        return;
    }
    tv_big r = tv_big_mul_slow(*x, y);
    tv_big_release(*x);
    *x = r;
}

/* ---------------------------------------------------------------- fused multiply-add */

/* |r| = |z| + |x| * m over rn = max(zn, xn) + 1 limbs (r may be z, or x at the same index). */
static void tv_mag_addmul_into(tv_limb *r, const tv_limb *z, uint32_t zn, const tv_limb *x, uint32_t xn, tv_limb m, uint32_t rn) {
    uint32_t i = zn < xn ? zn : xn;
    tv_limb c = tv_mag_addmul_n(r, z, x, i, m);
    for (; i < xn; i++) {
        tv_dlimb p = (tv_dlimb)x[i] * m + c;
        r[i] = (tv_limb)p;
        c = (tv_limb)(p >> 64);
    }
    for (; i < zn; i++) {
        tv_limb v = z[i] + c;
        c = v < c;
        r[i] = v;
    }
    for (; i < rn; i++) {
        r[i] = c;
        c = 0;
    }
}

/* r = z - x * m over rn = max(zn, xn) + 1 limbs; true if it went negative (r then holds the two's
 * complement, which tv_mag_negate turns into the magnitude). */
static bool tv_mag_submul_into(tv_limb *r, const tv_limb *z, uint32_t zn, const tv_limb *x, uint32_t xn, tv_limb m, uint32_t rn) {
    /* the limbs both have, in one pass; what it leaves to subtract (c) carries on into the rest */
    uint32_t both = zn < xn ? zn : xn;
    tv_limb c = tv_mag_submul_n(r, z, x, both, m);
    unsigned long long borrow = 0;
    for (uint32_t i = both; i < rn; i++) {
        tv_limb lo = 0;
        if (i < xn) {
            tv_dlimb p = (tv_dlimb)x[i] * m + c;
            lo = (tv_limb)p;
            c = (tv_limb)(p >> 64);
        } else {
            lo = c;
            c = 0;
        }
        r[i] = __builtin_subcll(i < zn ? z[i] : 0, lo, borrow, &borrow);
    }
    return borrow != 0;
}

static void tv_mag_negate(tv_limb *r, uint32_t n) {
    tv_limb c = 1;
    for (uint32_t i = 0; i < n; i++) {
        tv_limb v = ~r[i] + c;
        c = c && v == 0;
        r[i] = v;
    }
}

/* z + sign * x * y in `dst` (z's own buffer when dst is z's, else new): the one-pass case, a
 * factor that fits one limb. Returns false if it doesn't apply. */
static bool tv_big_muladd_fast(tv_big x, tv_big y, tv_big z, int sign, tv_big *out, bool in_place) {
    if (!tv_big_is_small(y)) {
        if (!tv_big_is_small(x)) return false;
        tv_big t = x; x = y; y = t;
    }
    int64_t ys = y.v >> 1;
    tv_bview X, Z;
    tv_view(x, &X);
    tv_view(z, &Z);
    tv_limb m = ys < 0 ? (tv_limb)0 - (tv_limb)ys : (tv_limb)ys;
    if (m == 0 || X.n == 0) {
        if (!in_place) tv_big_retain(z);
        *out = z;
        return true;
    }
    bool pneg = (X.neg != (ys < 0)) != (sign < 0);
    uint32_t rn = (Z.n > X.n ? Z.n : X.n) + 1;
    tv_bigbuf *r;
    if (in_place) {
        r = tv_bb(z);
        if (r->cap < rn) {
            r = tv_bb_reserve(r, rn);
            Z.d = r->d;
        }
        if (r->len < rn) memset(r->d + r->len, 0, (size_t)(rn - r->len) * sizeof(tv_limb));
        Z.n = r->len;
    } else {
        r = tv_bb_new(rn);
    }
    bool zneg = Z.n ? Z.neg : pneg;
    if (pneg == zneg) {
        tv_mag_addmul_into(r->d, Z.d, Z.n, X.d, X.n, m, rn);
        r->neg = zneg;
    } else if (tv_mag_submul_into(r->d, Z.d, Z.n, X.d, X.n, m, rn)) {
        tv_mag_negate(r->d, rn);
        r->neg = pneg;
    } else {
        r->neg = zneg;
    }
    r->len = rn;
    *out = tv_bb_finish(r);
    return true;
}

tv_big tv_big_muladd(tv_big x, tv_big y, tv_big z, int sign) {
    tv_big r;
    if (tv_big_muladd_fast(x, y, z, sign, &r, false)) return r;
    tv_big p = tv_big_mul(x, y);
    r = sign > 0 ? tv_big_add(z, p) : tv_big_sub(z, p);
    tv_big_release(p);
    return r;
}

void tv_big_muladd_to(tv_big *z, tv_big x, tv_big y, int sign) {
    tv_big r;
    if (tv_big_is_small(*z) && tv_big_is_small(x) && tv_big_is_small(y)) {
        __int128 v = (__int128)(z->v >> 1) + (__int128)sign * ((__int128)(x.v >> 1) * (y.v >> 1));
        *z = tv_big_from_i128_(v);
        return;
    }
    bool own = !tv_big_is_small(*z) && tv_bb(*z)->rc == 1 && z->v != x.v && z->v != y.v;
    if (tv_big_muladd_fast(x, y, *z, sign, &r, own)) {
        if (!own) tv_big_release(*z);
        *z = r;
        return;
    }
    tv_big p = tv_big_mul(x, y);
    if (sign > 0) tv_big_add_to(z, p);
    else tv_big_sub_to(z, p);
    tv_big_release(p);
}

/* ---------------------------------------------------------------- divide */

/* ---- Remainders by a divisor seen again (modular arithmetic: x % P with the same P over and
 * over), by Barrett's method: with mu = floor(B^2k / d) kept, x mod d (x under B^2k) is two
 * multiplications that don't wait on each other, where long division is a chain of k + 1
 * dependent quotient digits. (Handbook of Applied Cryptography, 14.42.) */
typedef unsigned __int128 tv_u128;
#define TV_BARRETT(K)                                                                                       \
    static void tv_barrett_rem##K(tv_limb *restrict r, const tv_limb *restrict x, const tv_limb *restrict d, \
                                  const tv_limb *restrict mu) {                                             \
        const tv_limb *q1 = x + (K - 1);                                                                    \
        tv_limb p[2 * K + 2];                                                                               \
        for (int i = 0; i < 2 * K + 2; i++) p[i] = 0;                                                       \
        for (int i = 0; i <= K; i++) {                                                                      \
            tv_limb c = 0;                                                                                  \
            for (int j = 0; j <= K; j++) {                                                                  \
                tv_u128 t = (tv_u128)q1[i] * mu[j] + p[i + j] + c;                                          \
                p[i + j] = (tv_limb)t;                                                                      \
                c = (tv_limb)(t >> 64);                                                                     \
            }                                                                                               \
            p[i + K + 1] = c;                                                                               \
        }                                                                                                   \
        const tv_limb *q3 = p + K + 1;                                                                      \
        tv_limb s[K + 1];                                                                                   \
        for (int i = 0; i <= K; i++) s[i] = 0;                                                              \
        for (int i = 0; i <= K; i++) {                                                                      \
            tv_limb c = 0;                                                                                  \
            for (int j = 0; j < K && i + j <= K; j++) {                                                     \
                tv_u128 t = (tv_u128)q3[i] * d[j] + s[i + j] + c;                                           \
                s[i + j] = (tv_limb)t;                                                                      \
                c = (tv_limb)(t >> 64);                                                                     \
            }                                                                                               \
            if (i == 0) s[K] += c;                                                                          \
        }                                                                                                   \
        tv_limb w[K + 1];                                                                                   \
        unsigned long long br = 0;                                                                          \
        for (int i = 0; i <= K; i++) w[i] = __builtin_subcll(x[i], s[i], br, &br);                         \
        for (int k = 0; k < 2; k++) {                                                                       \
            tv_limb t[K + 1];                                                                               \
            unsigned long long b2 = 0;                                                                      \
            for (int i = 0; i < K; i++) t[i] = __builtin_subcll(w[i], d[i], b2, &b2);                       \
            t[K] = __builtin_subcll(w[K], 0, b2, &b2);                                                      \
            if (b2) break;                                                                                  \
            for (int i = 0; i <= K; i++) w[i] = t[i];                                                       \
        }                                                                                                   \
        for (int i = 0; i < K; i++) r[i] = w[i];                                                            \
    }
TV_BARRETT(2)
TV_BARRETT(3)
TV_BARRETT(4)
TV_BARRETT(5)
TV_BARRETT(6)
TV_BARRETT(7)
TV_BARRETT(8)

typedef struct { tv_limb d[8], mu[9]; uint32_t k; bool ready; } tv_barrett;
static tv_barrett tv_barretts[4];
static unsigned tv_barrett_next;

/* The kept mu of the k-limb divisor d: NULL the first time it's seen (it's kept, its mu made
 * the second time). */
static const tv_barrett *tv_barrett_of(const tv_limb *d, uint32_t k) {
    for (int i = 0; i < 4; i++) {
        tv_barrett *b = &tv_barretts[i];
        if (b->k != k || memcmp(b->d, d, k * sizeof(tv_limb))) continue;
        if (b->ready) return b;
        /* (seen twice: mu = floor(B^2k / d), when it fits k + 1 limbs) */
        tv_limb num[17] = {0}, q[17] = {0};
        num[2 * k] = 1;
        tv_mag_divrem(q, NULL, num, 2 * k + 1, d, k);
        if (q[k + 1]) return NULL;
        for (uint32_t j = 0; j <= k; j++) b->mu[j] = q[j];
        b->ready = true;
        return b;
    }
    /* (a divisor seen once takes the place of another seen once, not of a kept mu: the changing
     * divisors of a gcd don't push out the modulus) */
    tv_barrett *b = NULL;
    for (int i = 0; i < 4 && !b; i++) if (!tv_barretts[(tv_barrett_next + i) & 3].ready) b = &tv_barretts[(tv_barrett_next + i) & 3];
    if (!b) b = &tv_barretts[tv_barrett_next & 3];
    tv_barrett_next++;
    b->k = k;
    b->ready = false;
    memcpy(b->d, d, k * sizeof(tv_limb));
    return NULL;
}

/* r (k limbs) = x mod d by Barrett, for x of at most 2k limbs. */
static void tv_barrett_rem2k(const tv_barrett *b, tv_limb *r, const tv_limb *x, uint32_t n) {
    tv_limb xx[16];
    for (uint32_t i = 0; i < 2 * b->k; i++) xx[i] = i < n ? x[i] : 0;
    switch (b->k) {
    case 2: tv_barrett_rem2(r, xx, b->d, b->mu); break;
    case 3: tv_barrett_rem3(r, xx, b->d, b->mu); break;
    case 4: tv_barrett_rem4(r, xx, b->d, b->mu); break;
    case 5: tv_barrett_rem5(r, xx, b->d, b->mu); break;
    case 6: tv_barrett_rem6(r, xx, b->d, b->mu); break;
    case 7: tv_barrett_rem7(r, xx, b->d, b->mu); break;
    default: tv_barrett_rem8(r, xx, b->d, b->mu); break;
    }
}

/* r (k limbs) = x mod d, for x of any length: its top 2k limbs reduced, then the remainder with
 * the next k limbs below it, and so on down (each window under d * B^k <= B^2k). */
static void tv_barrett_rem(const tv_barrett *b, tv_limb *r, const tv_limb *x, uint32_t n) {
    uint32_t k = b->k;
    if (n <= 2 * k) {
        tv_barrett_rem2k(b, r, x, n);
        return;
    }
    tv_limb w[16];
    uint32_t pos = n - 2 * k;
    tv_barrett_rem2k(b, r, x + pos, 2 * k);
    while (pos > 0) {
        uint32_t take = pos >= k ? k : pos;
        pos -= take;
        for (uint32_t i = 0; i < take; i++) w[i] = x[pos + i];
        for (uint32_t i = 0; i < k; i++) w[take + i] = r[i];
        tv_barrett_rem2k(b, r, w, take + k);
    }
}

/* Truncated division (JavaScript's): q = a / b, r = a % b (either may be NULL). */
static void tv_big_divmod(tv_big a, tv_big b, tv_big *q, tv_big *r, const char *loc) {
    if (tv_big_is_small(b) && b.v == 1) tv_trap("bigint division by zero", loc);
    if (tv_big_is_small(a) && tv_big_is_small(b)) {
        int64_t x = a.v >> 1, y = b.v >> 1;
        /* (-2^62 / -1 is 2^62: one past the small range) */
        if (q) *q = tv_big_from_i64(x / y);
        if (r) *r = tv_big_small(x % y);
        return;
    }
    tv_bview x, y;
    tv_view(a, &x);
    tv_view(b, &y);
    if (tv_mag_cmp(x.d, x.n, y.d, y.n) < 0) {
        if (q) *q = tv_big_small(0);
        if (r) {
            tv_big_retain(a);
            *r = a;
        }
        return;
    }
    if (y.n == 1) {
        tv_bigbuf *qb = q ? tv_bb_new(x.n) : NULL;
        tv_limb *qd = qb ? qb->d : tv_alloc((size_t)x.n * sizeof(tv_limb));
        tv_limb rem = tv_mag_divrem1(qd, x.d, x.n, y.d[0]);
        if (qb) {
            qb->len = x.n;
            qb->neg = x.neg != y.neg;
            *q = tv_bb_finish(qb);
        } else {
            free(qd);
        }
        if (r) *r = tv_big_from_u64(rem, x.neg);
        return;
    }
    tv_bigbuf *qb = q ? tv_bb_new(x.n - y.n + 1) : NULL;
    tv_bigbuf *rb = r ? tv_bb_new(y.n) : NULL;
    const tv_barrett *bar = !q && y.n >= 2 && y.n <= 8 && x.n <= 8 * y.n ? tv_barrett_of(y.d, y.n) : NULL;
    if (bar) tv_barrett_rem(bar, rb->d, x.d, x.n);
    else tv_mag_divrem(qb ? qb->d : NULL, rb ? rb->d : NULL, x.d, x.n, y.d, y.n);
    if (qb) {
        qb->len = x.n - y.n + 1;
        qb->neg = x.neg != y.neg;
        *q = tv_bb_finish(qb);
    }
    if (rb) {
        rb->len = y.n;
        rb->neg = x.neg;
        *r = tv_bb_finish(rb);
    }
}

tv_big tv_big_div(tv_big a, tv_big b, const char *loc) {
    tv_big q;
    tv_big_divmod(a, b, &q, NULL, loc);
    return q;
}

tv_big tv_big_rem(tv_big a, tv_big b, const char *loc) {
    tv_big r;
    tv_big_divmod(a, b, NULL, &r, loc);
    return r;
}

tv_big tv_big_pow(tv_big a, tv_big b, const char *loc) {
    if (!tv_big_is_small(b) || b.v < 0) {
        if (!tv_big_is_small(b) && !tv_bb(b)->neg) tv_trap("bigint exponent too large", loc);
        tv_trap("bigint exponent must be non-negative", loc);
    }
    int64_t e = b.v >> 1;
    if (tv_big_is_small(a)) {
        int64_t x = a.v >> 1;
        if (x == 0) return tv_big_small(e == 0);
        if (x == 1) return tv_big_small(1);
        if (x == -1) return tv_big_small(e & 1 ? -1 : 1);
    }
    if (e > ((int64_t)1 << 32)) tv_trap("bigint exponent too large", loc);
    if (e == 0) return tv_big_small(1);
    if (e == 2) return tv_big_mul(a, a);
    /* (the result starts as the first power taken, not as 1 times it) */
    tv_big base = a, acc = tv_big_small(1);
    bool have = false;
    tv_big_retain(base);
    while (e > 0) {
        if (e & 1) {
            if (have) {
                tv_big t = tv_big_mul(acc, base);
                tv_big_release(acc);
                acc = t;
            } else {
                tv_big_retain(base);
                acc = base;
                have = true;
            }
        }
        e >>= 1;
        if (e) {
            tv_big t = tv_big_mul(base, base);
            tv_big_release(base);
            base = t;
        }
    }
    tv_big_release(base);
    return acc;
}

/* ---------------------------------------------------------------- compare, hash */

int tv_big_cmp_slow(tv_big a, tv_big b) {
    tv_bview x, y;
    tv_view(a, &x);
    tv_view(b, &y);
    if (x.neg != y.neg) return x.neg ? -1 : 1;
    int c = tv_mag_cmp(x.d, x.n, y.d, y.n);
    return x.neg ? -c : c;
}

bool tv_big_eq_slow(tv_big a, tv_big b) {
    if (tv_big_is_small(a) || tv_big_is_small(b)) return false;
    tv_bigbuf *x = tv_bb(a), *y = tv_bb(b);
    return x->neg == y->neg && x->len == y->len && memcmp(x->d, y->d, (size_t)x->len * sizeof(tv_limb)) == 0;
}

uint64_t tv_big_hash(tv_big a) {
    if (tv_big_is_small(a)) {
        uint64_t x = (uint64_t)a.v * 0x9E3779B97F4A7C15ull;
        return x ^ (x >> 29);
    }
    tv_bigbuf *b = tv_bb(a);
    uint64_t h = 0x6a09e667f3bcc909ull ^ b->neg;
    for (uint32_t i = 0; i < b->len; i++) {
        h = (h ^ b->d[i]) * 0x100000001B3ull;
        h ^= h >> 31;
    }
    return h;
}

int tv_big_cmp_int(tv_big a, int64_t b) {
    if (tv_big_is_small(a)) {
        int64_t x = a.v >> 1;
        return (x > b) - (x < b);
    }
    return tv_bb(a)->neg ? -1 : 1;
}

/* a against a number, exactly (JavaScript's 5n < 5.5): -1, 0 or 1, or 2 when b is NaN (every
 * comparison is false). */
int tv_big_cmp_f64(tv_big a, double b) {
    if (b != b) return 2;
    if (b == __builtin_inf()) return -1;
    if (b == -__builtin_inf()) return 1;
    double fb = __builtin_floor(b);
    if (tv_big_is_small(a) && fb >= -0x1p62 && fb < 0x1p62) {
        int64_t x = a.v >> 1, y = (int64_t)fb;
        if (x != y) return (x > y) - (x < y);
    } else {
        tv_big f;
        tv_big_from_f64(fb, &f);
        int c = tv_big_cmp(a, f);
        tv_big_release(f);
        if (c != 0) return c;
    }
    return b > fb ? -1 : 0;
}

/* ---------------------------------------------------------------- conversions */

tv_big tv_big_from_u64_(uint64_t m) { return tv_big_from_u64(m, false); }

uint64_t tv_big_to_u64(tv_big a, const char *loc) {
    if (tv_big_is_small(a) && a.v >= 0) return (uint64_t)(a.v >> 1);
    if (tv_big_is_small(a) || tv_bb(a)->neg || tv_bb(a)->len != 1) tv_trap("bigint out of range for an integer", loc);
    return tv_bb(a)->d[0];
}

bool tv_big_fits_int(tv_big a) {
    if (tv_big_is_small(a)) return true;
    tv_bigbuf *b = tv_bb(a);
    return b->len == 1 && (b->d[0] <= (tv_limb)INT64_MAX || (b->neg && b->d[0] == (tv_limb)1 << 63));
}

tv_int tv_big_to_int(tv_big a, const char *loc) {
    if (tv_big_is_small(a)) return a.v >> 1;
    if (!tv_big_fits_int(a)) tv_trap("bigint out of range for an integer", loc);
    tv_bigbuf *b = tv_bb(a);
    return b->neg ? (int64_t)((tv_limb)0 - b->d[0]) : (int64_t)b->d[0];
}

/* Number(x): the nearest double (ties to even), Infinity beyond the range. */
double tv_big_to_f64(tv_big a) {
    if (tv_big_is_small(a)) return (double)(a.v >> 1);
    tv_bigbuf *b = tv_bb(a);
    uint32_t n = b->len;
    int lz = __builtin_clzll(b->d[n - 1]);
    uint64_t bits = (uint64_t)n * 64 - (uint64_t)lz;   /* the magnitude's bit length */
    /* the top 64 bits, and whether anything below them is set */
    tv_limb top = b->d[n - 1] << lz;
    bool sticky = false;
    if (n >= 2) {
        if (lz) top |= b->d[n - 2] >> (64 - lz);
        sticky = lz ? (b->d[n - 2] << lz) != 0 : false;
        for (uint32_t i = 0; i + 2 < n && !sticky; i++) sticky = b->d[i] != 0;
    }
    uint64_t mant = top >> 11;           /* 53 bits */
    uint64_t rest = top & 0x7FF;
    if (rest > 0x400 || (rest == 0x400 && (sticky || (mant & 1)))) mant++;
    uint64_t e = bits - 1;
    if (mant == (uint64_t)1 << 53) {
        mant >>= 1;
        e++;
    }
    union { double d; uint64_t u; } r;
    if (e > 1023) r.u = 0x7FF0000000000000ull;
    else r.u = ((e + 1023) << 52) | (mant & (((uint64_t)1 << 52) - 1));
    if (b->neg) r.u |= (uint64_t)1 << 63;
    return r.d;
}

/* BigInt(x) for a double: false unless it's an integer. */
bool tv_big_from_f64(double x, tv_big *out) {
    union { double d; uint64_t u; } v = { x };
    int e = (int)((v.u >> 52) & 0x7FF);
    if (e == 0x7FF) return false;
    if (x > -4611686018427387904.0 && x < 4611686018427387904.0) {
        int64_t i = (int64_t)x;
        if ((double)i != x) return false;
        *out = tv_big_small(i);
        return true;
    }
    /* |x| >= 2^62: an integer (the exponent puts every bit above the point) */
    uint64_t mant = (v.u & (((uint64_t)1 << 52) - 1)) | ((uint64_t)1 << 52);
    int shift = e - 1075;                /* x = mant * 2^shift, shift > 0 here */
    uint32_t limbs = (uint32_t)(shift / 64) + 2;
    tv_bigbuf *b = tv_bb_new(limbs);
    memset(b->d, 0, (size_t)limbs * sizeof(tv_limb));
    int w = shift / 64, s = shift % 64;
    b->d[w] = mant << s;
    if (s) b->d[w + 1] = mant >> (64 - s);
    b->len = limbs;
    b->neg = (v.u >> 63) != 0;
    *out = tv_bb_finish(b);
    return true;
}

/* The digits of |a| in a radix (2..36), least significant chunk first, through `emit`. */
static const char tv_digits36[] = "0123456789abcdefghijklmnopqrstuvwxyz";

/* Decimal digits of x[0..n) into out[0..width), right-aligned with leading zeros: 19 digits at a
 * time, by division by 10^19 (x is overwritten). */
static void tv_dec_simple(tv_limb *x, uint32_t n, char *out, size_t width) {
    const tv_limb chunk = 10000000000000000000ull;
    size_t pos = width;
    n = tv_mag_norm(x, n);
    while (n > 0 && pos > 0) {
        tv_limb r = tv_mag_divrem1(x, x, n, chunk);
        n = tv_mag_norm(x, n);
        for (int i = 0; i < 19 && pos > 0; i++) {
            out[--pos] = (char)('0' + r % 10);
            r /= 10;
        }
    }
    memset(out, '0', pos);
}

/* 10^(19 * 2^k), k = 0, 1, ...: squared from the one before, kept for the program's life. */
static tv_limb *tv_dec_pow[32];
static uint32_t tv_dec_pown[32];
static void tv_dec_pow_ensure(int k) {
    if (!tv_dec_pow[0]) {
        tv_dec_pow[0] = tv_alloc(sizeof(tv_limb));
        tv_dec_pow[0][0] = 10000000000000000000ull;
        tv_dec_pown[0] = 1;
    }
    for (int i = 1; i <= k; i++) {
        if (tv_dec_pow[i]) continue;
        uint32_t pn = tv_dec_pown[i - 1];
        tv_limb *p = tv_alloc((size_t)pn * 2 * sizeof(tv_limb));
        tv_mag_mul(p, tv_dec_pow[i - 1], pn, tv_dec_pow[i - 1], pn);
        tv_dec_pow[i] = p;
        tv_dec_pown[i] = tv_mag_norm(p, pn * 2);
    }
}

/* Divide and conquer: x = q * 10^d + r with 10^d about half x's size, each half converted on
 * its own. The divisions run on the multiply-subtract loop, much faster per limb than dividing
 * by 10^19 limb after limb (the simple method, used below 40 limbs). x[0..n) is overwritten. */
static void tv_dec_rec(tv_limb *x, uint32_t n, char *out, size_t width) {
    n = tv_mag_norm(x, n);
    if (n < 40) {
        tv_dec_simple(x, n, out, width);
        return;
    }
    int k = 0;
    tv_dec_pow_ensure(1);
    while (1) {
        tv_dec_pow_ensure(k + 1);
        if ((uint64_t)tv_dec_pown[k + 1] * 2 > (uint64_t)n + 1) break;
        k++;
    }
    const tv_limb *p = tv_dec_pow[k];
    uint32_t pn = tv_dec_pown[k];
    size_t pd = (size_t)19 << k;
    tv_limb *q = tv_alloc((size_t)(n - pn + 1 + pn) * sizeof(tv_limb));
    tv_limb *r = q + (n - pn + 1);
    if (pn == 1) {
        r[0] = tv_mag_divrem1(q, x, n, p[0]);
    } else {
        tv_mag_divrem(q, r, x, n, p, pn);
    }
    tv_dec_rec(q, n - pn + 1, out, width - pd);
    tv_dec_rec(r, pn, out + width - pd, pd);
    free(q);
}

void tv_sb_push_big_radix(tv_sb *sb, tv_big a, int radix) {
    if (tv_big_is_small(a) && radix == 10) {
        tv_sb_push_int(sb, a.v >> 1);
        return;
    }
    tv_bview x;
    tv_view(a, &x);
    if (!x.n) {
        tv_sb_push_char(sb, '0');
        return;
    }
    if (radix == 10 && x.n >= 40) {
        /* at most 19.27 digits a limb */
        size_t width = (size_t)x.n * 20 + 1;
        char *buf = tv_alloc(width + 1);
        tv_limb *t = tv_alloc((size_t)x.n * sizeof(tv_limb));
        memcpy(t, x.d, (size_t)x.n * sizeof(tv_limb));
        tv_dec_rec(t, x.n, buf + 1, width);
        free(t);
        char *d = buf + 1;
        while (*d == '0') d++;
        if (x.neg) *--d = '-';
        tv_sb_push(sb, d, (size_t)(buf + 1 + width - d));
        free(buf);
        return;
    }
    /* a chunk is the largest power of the radix in a limb: k digits */
    tv_limb chunk = (tv_limb)radix;
    int k = 1;
    while (chunk <= ~(tv_limb)0 / (tv_limb)radix) {
        chunk *= (tv_limb)radix;
        k++;
    }
    tv_limb *t = tv_alloc((size_t)x.n * sizeof(tv_limb));
    memcpy(t, x.d, (size_t)x.n * sizeof(tv_limb));
    uint32_t n = x.n;
    size_t cap = (size_t)n * 64 + 2, len = 0;
    char *buf = tv_alloc(cap);
    while (n > 0) {
        tv_limb r = tv_mag_divrem1(t, t, n, chunk);
        while (n > 0 && t[n - 1] == 0) n--;
        for (int i = 0; i < k; i++) {
            if (n == 0 && r == 0) break;
            buf[len++] = tv_digits36[r % (tv_limb)radix];
            r /= (tv_limb)radix;
        }
    }
    if (x.neg) buf[len++] = '-';
    for (size_t i = 0; i < len / 2; i++) {
        char c = buf[i];
        buf[i] = buf[len - 1 - i];
        buf[len - 1 - i] = c;
    }
    tv_sb_push(sb, buf, len);
    free(buf);
    free(t);
}

void tv_sb_push_big(tv_sb *sb, tv_big a) { tv_sb_push_big_radix(sb, a, 10); }

tv_str tv_big_to_string(tv_big a, tv_int radix, const char *loc) {
    if (radix < 2 || radix > 36) tv_trap("toString() radix must be between 2 and 36", loc);
    tv_sb sb = {0};
    tv_sb_push_big_radix(&sb, a, (int)radix);
    return tv_str_from_sb(&sb);
}

/* The value of the digits s[0, n) in a radix (every one valid). */
static tv_big tv_big_from_digits(const char *s, size_t n, int radix, bool neg) {
    tv_limb chunk = (tv_limb)radix;
    int k = 1;
    while (chunk <= ~(tv_limb)0 / (tv_limb)radix) {
        chunk *= (tv_limb)radix;
        k++;
    }
    /* bits per digit, rounded up */
    int bpd = 1;
    while ((1 << bpd) < radix) bpd++;
    uint32_t cap = (uint32_t)((n * (size_t)bpd) / 64 + 2);
    tv_bigbuf *b = tv_bb_new(cap);
    b->len = 0;
    size_t first = n % (size_t)k;
    if (first == 0) first = (size_t)k;
    for (size_t i = 0; i < n;) {
        size_t take = i == 0 ? first : (size_t)k;
        tv_limb v = 0, mul = 1;
        for (size_t j = 0; j < take; j++) {
            char c = s[i + j];
            int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
            v = v * (tv_limb)radix + (tv_limb)d;
            mul *= (tv_limb)radix;
        }
        i += take;
        tv_limb c = b->len ? tv_mag_mul1(b->d, b->d, b->len, take == (size_t)k ? chunk : mul) : 0;
        if (c) b->d[b->len++] = c;
        if (b->len == 0) {
            if (v) {
                b->d[0] = v;
                b->len = 1;
            }
        } else {
            tv_limb carry = tv_mag_add(b->d, b->d, b->len, &v, 1);
            if (carry) b->d[b->len++] = carry;
        }
    }
    b->neg = neg;
    return tv_bb_finish(b);
}

static bool tv_big_space(unsigned char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

/* BigInt(string), as JavaScript reads it: surrounding whitespace, an optional sign (decimal
 * only), or 0x / 0o / 0b; "" is 0n. */
bool tv_big_parse(tv_str s, tv_big *out) {
    const char *p = s.p->data, *e = p + s.p->len;
    while (p < e && tv_big_space((unsigned char)*p)) p++;
    while (e > p && tv_big_space((unsigned char)e[-1])) e--;
    if (p == e) {
        *out = tv_big_small(0);
        return true;
    }
    int radix = 10;
    bool neg = false;
    if (e - p > 2 && p[0] == '0' && (p[1] | 0x20) == 'x') { radix = 16; p += 2; }
    else if (e - p > 2 && p[0] == '0' && (p[1] | 0x20) == 'o') { radix = 8; p += 2; }
    else if (e - p > 2 && p[0] == '0' && (p[1] | 0x20) == 'b') { radix = 2; p += 2; }
    else if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    if (p == e) return false;
    for (const char *q = p; q < e; q++) {
        char c = *q;
        int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'z') ? c - 'a' + 10 : (c >= 'A' && c <= 'Z') ? c - 'A' + 10 : 99;
        if (d >= radix) return false;
    }
    while (p < e - 1 && *p == '0') p++;
    *out = tv_big_from_digits(p, (size_t)(e - p), radix, neg);
    return true;
}

/* A literal's value (digits as the compiler wrote them: decimal, or 0x / 0o / 0b), immortal. */
tv_big tv_big_lit(const char *text) {
    tv_big v;
    size_t n = strlen(text);
    int radix = 10;
    if (n > 2 && text[0] == '0' && (text[1] | 0x20) == 'x') radix = 16;
    else if (n > 2 && text[0] == '0' && (text[1] | 0x20) == 'o') radix = 8;
    else if (n > 2 && text[0] == '0' && (text[1] | 0x20) == 'b') radix = 2;
    if (radix != 10) {
        text += 2;
        n -= 2;
    }
    v = tv_big_from_digits(text, n, radix, false);
    if (!tv_big_is_small(v)) tv_bb(v)->rc = INT64_MAX / 2;
    return v;
}

/* ---------------------------------------------------------------- bits */

/* Two's complement in `n` limbs (n > the magnitude's length), into t. */
static void tv_twos(const tv_bview *x, tv_limb *t, uint32_t n) {
    memcpy(t, x->d, (size_t)x->n * sizeof(tv_limb));
    memset(t + x->n, 0, (size_t)(n - x->n) * sizeof(tv_limb));
    if (x->neg) {
        tv_limb c = 1;
        for (uint32_t i = 0; i < n; i++) {
            tv_limb v = ~t[i] + c;
            c = c && v == 0;
            t[i] = v;
        }
    }
}

/* The value of a two's complement number in n limbs (t is reused). */
static tv_big tv_from_twos(tv_limb *t, uint32_t n) {
    bool neg = (t[n - 1] >> 63) != 0;
    if (neg) {
        tv_limb c = 1;
        for (uint32_t i = 0; i < n; i++) {
            tv_limb v = ~t[i] + c;
            c = c && v == 0;
            t[i] = v;
        }
    }
    tv_bigbuf *b = tv_bb_new(n);
    memcpy(b->d, t, (size_t)n * sizeof(tv_limb));
    b->len = n;
    b->neg = neg;
    return tv_bb_finish(b);
}

tv_big tv_big_bitop(tv_big a, tv_big b, int op) {
    if (tv_big_is_small(a) && tv_big_is_small(b)) {
        int64_t x = a.v >> 1, y = b.v >> 1;
        return tv_big_small(op == 0 ? (x & y) : op == 1 ? (x | y) : (x ^ y));
    }
    tv_bview x, y;
    tv_view(a, &x);
    tv_view(b, &y);
    uint32_t n = (x.n > y.n ? x.n : y.n) + 1;
    tv_limb *t = tv_alloc((size_t)n * 2 * sizeof(tv_limb));
    tv_twos(&x, t, n);
    tv_twos(&y, t + n, n);
    for (uint32_t i = 0; i < n; i++) t[i] = op == 0 ? (t[i] & t[n + i]) : op == 1 ? (t[i] | t[n + i]) : (t[i] ^ t[n + i]);
    tv_big r = tv_from_twos(t, n);
    free(t);
    return r;
}

tv_big tv_big_not(tv_big a) {
    /* ~x is -x - 1 */
    tv_big n = tv_big_neg(a);
    tv_big r = tv_big_sub(n, tv_big_small(1));
    tv_big_release(n);
    return r;
}

/* x << s for s >= 0 (magnitude shift; the sign stays). */
static tv_big tv_big_shl_mag(const tv_bview *x, uint64_t s) {
    uint32_t w = (uint32_t)(s / 64), bits = (uint32_t)(s % 64);
    tv_bigbuf *r = tv_bb_new(x->n + w + 1);
    memset(r->d, 0, (size_t)w * sizeof(tv_limb));
    if (bits == 0) {
        memcpy(r->d + w, x->d, (size_t)x->n * sizeof(tv_limb));
        r->d[w + x->n] = 0;
    } else {
        tv_limb c = 0;
        for (uint32_t i = 0; i < x->n; i++) {
            r->d[w + i] = (x->d[i] << bits) | c;
            c = x->d[i] >> (64 - bits);
        }
        r->d[w + x->n] = c;
    }
    r->len = x->n + w + 1;
    r->neg = x->neg;
    return tv_bb_finish(r);
}

/* x >> s for s >= 0: floor(x / 2^s). */
static tv_big tv_big_shr_mag(const tv_bview *x, uint64_t s) {
    uint64_t w = s / 64;
    uint32_t bits = (uint32_t)(s % 64);
    if (w >= x->n) return tv_big_small(x->neg && x->n ? -1 : 0);
    uint32_t n = x->n - (uint32_t)w;
    tv_bigbuf *r = tv_bb_new(n);
    bool lost = false;
    for (uint64_t i = 0; i < w && !lost; i++) lost = x->d[i] != 0;
    if (bits == 0) {
        memcpy(r->d, x->d + w, (size_t)n * sizeof(tv_limb));
    } else {
        lost = lost || (x->d[w] << (64 - bits)) != 0;
        for (uint32_t i = 0; i < n; i++) {
            tv_limb hi = i + 1 < n ? x->d[w + i + 1] << (64 - bits) : 0;
            r->d[i] = (x->d[w + i] >> bits) | hi;
        }
    }
    r->len = n;
    r->neg = x->neg;
    if (x->neg && lost) {
        /* rounds toward negative infinity: one more in magnitude */
        if (r->cap < n + 1) r = tv_bb_reserve(r, n + 1);
        tv_limb one = 1;
        r->d[n] = tv_mag_add(r->d, r->d, n, &one, 1);
        r->len = n + 1;
    }
    return tv_bb_finish(r);
}

tv_big tv_big_shift(tv_big a, tv_big b, bool left, const char *loc) {
    if (!tv_big_is_small(b)) {
        /* a shift past any memory: left traps, right leaves the sign */
        bool bneg = tv_bb(b)->neg;
        if (left != bneg) tv_trap("bigint too large to shift", loc);
        tv_bview x;
        tv_view(a, &x);
        return tv_big_small(x.neg && x.n ? -1 : 0);
    }
    int64_t s = b.v >> 1;
    if (s < 0) {
        left = !left;
        s = -s;
    }
    tv_bview x;
    tv_view(a, &x);
    if (!x.n) return tv_big_small(0);
    if (left) {
        if (s > ((int64_t)1 << 32)) tv_trap("bigint too large to shift", loc);
        if (tv_big_is_small(a) && s < 62) {
            int64_t v = a.v >> 1;
            int64_t r = (int64_t)((uint64_t)v << s);
            if ((r >> s) == v && r >= TV_BIG_SMALL_MIN && r <= TV_BIG_SMALL_MAX) return tv_big_small(r);
        }
        return tv_big_shl_mag(&x, (uint64_t)s);
    }
    if (tv_big_is_small(a)) return tv_big_small(s >= 63 ? ((a.v >> 1) < 0 ? -1 : 0) : (a.v >> 1) >> s);
    return tv_big_shr_mag(&x, (uint64_t)s);
}

/* BigInt.asUintN(bits, x) / BigInt.asIntN(bits, x): x modulo 2^bits, unsigned or signed. */
tv_big tv_big_as_n(tv_int bits, tv_big a, bool is_signed, const char *loc) {
    if (bits < 0 || bits > ((tv_int)1 << 32)) tv_trap("BigInt.asIntN/asUintN: bits out of range", loc);
    if (bits == 0) return tv_big_small(0);
    tv_bview x;
    tv_view(a, &x);
    uint32_t n = (uint32_t)((bits + 63) / 64) + 1;
    if (n < x.n + 1) n = x.n + 1;
    tv_limb *t = tv_alloc((size_t)n * sizeof(tv_limb));
    tv_twos(&x, t, n);
    /* keep the low `bits` bits */
    uint32_t keep = (uint32_t)((bits + 63) / 64);
    for (uint32_t i = keep; i < n; i++) t[i] = 0;
    uint32_t top = (uint32_t)(bits % 64);
    if (top) t[keep - 1] &= ((tv_limb)1 << top) - 1;
    tv_bigbuf *r = tv_bb_new(n);
    memcpy(r->d, t, (size_t)n * sizeof(tv_limb));
    r->len = n;
    r->neg = 0;
    free(t);
    tv_big u = tv_bb_finish(r);
    if (!is_signed) return u;
    /* signed: subtract 2^bits when bit (bits - 1) is set */
    tv_bview v;
    tv_view(u, &v);
    uint64_t bit = (uint64_t)bits - 1;
    bool set = bit / 64 < v.n && ((v.d[bit / 64] >> (bit % 64)) & 1);
    if (!set) return u;
    tv_bview one = { NULL, 1, false, 1 };
    one.d = &one.one;
    tv_big p = tv_big_shl_mag(&one, (uint64_t)bits);
    tv_big s = tv_big_sub(u, p);
    tv_big_release(u);
    tv_big_release(p);
    return s;
}

/* ---------------------------------------------------------------- the descriptor */

static void tv_bigp_retain(void *p) { tv_big_retain(*(tv_big *)p); }
static void tv_bigp_release(void *p) { tv_big_release(*(tv_big *)p); }
static bool tv_bigp_eq(const void *a, const void *b) { return tv_big_eq(*(const tv_big *)a, *(const tv_big *)b); }
static uint64_t tv_bigp_hash(const void *p) { return tv_big_hash(*(const tv_big *)p); }
static void tv_bigp_to_str(tv_sb *sb, const void *p) { tv_sb_push_big(sb, *(const tv_big *)p); }
static void tv_bigp_inspect(tv_sb *sb, const void *p, int depth) {
    (void)depth;
    tv_sb_push_big(sb, *(const tv_big *)p);
    tv_sb_push_char(sb, 'n');
}
const tv_type tv_type_big = { sizeof(tv_big), tv_bigp_retain, tv_bigp_release, tv_bigp_eq, tv_bigp_hash, tv_bigp_to_str, tv_bigp_inspect };
