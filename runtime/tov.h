/* tov.h — the runtime interface used by C code generated from Tov.
 *
 * Conventions
 * - Single-threaded; reference counts are non-atomic.
 * - Heap values (strings, arrays, maps, boxes, closure environments) start with an
 *   `int64_t rc`. rc < 0 means immortal (static literals): retain/release ignore it.
 * - Values are *owned* (+1) or *borrowed* (+0). Unless a comment says otherwise:
 *   arguments are borrowed, return values are owned, and `void *elem`/`key`/`value`
 *   inputs to container "insert" operations are MOVED (the container takes ownership).
 * - Arrays, maps and sets are shared, as JavaScript's (every reference sees every change);
 *   copies are made only by operations that make new ones (slice, concat, tv_map_copy...).
 * - Element/key/value types are described by `tv_type` descriptors emitted by the compiler.
 * - Out-of-range indices, integer overflow, failed `!` assertions etc. call tv_trap(),
 *   which prints "trap: <msg>\n  at <loc>" to stderr and exits with status 101 —
 *   or, while a test is running, fails that test and continues with the next one.
 */
#ifndef TOV_H
#define TOV_H

/* (for compilers without it: GCC before 14 rejects __has_feature in #if even behind defined()) */
#ifndef __has_feature
#define __has_feature(x) 0
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int64_t tv_int;

/* JavaScript's ToInt32 of a number (for bitwise operators): truncated, wrapped to 32 bits;
 * NaN and the infinities give 0. */
static inline bool tv_f64_truthy(double d) { return d != 0 && d == d; }
/* JavaScript's `%` on numbers: integer division when both are whole (as most are, `n % i`
 * after `n = n / i`), fmod otherwise. The result has the dividend's sign, -0 included. */
static inline double tv_fmod(double a, double b) {
    if (__builtin_expect(__builtin_fabs(a) < 9007199254740992.0 && __builtin_fabs(b) < 9007199254740992.0 && b != 0, 1)) {
        int64_t x = (int64_t)a, y = (int64_t)b;
        if ((double)x == a && (double)y == b) {
            int64_t r = x % y;
            return r == 0 ? __builtin_copysign(0.0, a) : (double)r;
        }
    }
    return __builtin_fmod(a, b);
}
/* JavaScript's ToIntegerOrInfinity, clamped to an int: truncated toward zero, NaN is 0. */
static inline tv_int tv_f64_to_int_js(double d) {
    if (__builtin_expect(d > -9.2e18 && d < 9.2e18, 1)) return (tv_int)d;
    return d != d ? 0 : d < 0 ? INT64_MIN : INT64_MAX;
}
_Noreturn void tv_f64_index_trap(double d, const char *loc);
/* A float used as an array index: it must be a whole number. */
static inline tv_int tv_f64_index(double d, const char *loc) {
    tv_int i = (tv_int)d;
    if (__builtin_expect((double)i == d && d > -9.2e18 && d < 9.2e18, 1)) return i;
    tv_f64_index_trap(d, loc);
}
static inline bool tv_f64_is_integer(double d) { return d == __builtin_trunc(d) && d - d == 0; }
static inline bool tv_f64_is_safe_integer(double d) { return tv_f64_is_integer(d) && __builtin_fabs(d) <= 9007199254740991.0; }
static inline bool tv_int_is_safe(tv_int v) { return v >= -INT64_C(9007199254740991) && v <= INT64_C(9007199254740991); }
int32_t tv_f64_to_i32_slow(double d);
static inline int32_t tv_f64_to_i32(double d) {
    if (__builtin_expect(d > -9.2e18 && d < 9.2e18, 1)) return (int32_t)(uint32_t)(uint64_t)(int64_t)d;
    return tv_f64_to_i32_slow(d);
}
/* a number stored into a Uint8ClampedArray: clamped to 0..255, rounded half to even, NaN 0 */
static inline uint8_t tv_f64_to_u8c(double d) { return d > 0 ? (d < 255 ? (uint8_t)__builtin_rint(d) : 255) : 0; }
static inline uint8_t tv_int_to_u8c(tv_int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* ------------------------------------------------------------------ traps */

_Noreturn void tv_trap(const char *msg, const char *loc);
/* traps on out-of-memory; fresh memory nothing else points to (the C compiler may assume so) */
__attribute__((malloc, alloc_size(1), returns_nonnull)) void *tv_alloc(size_t size);
void tv_write_fd(int fd, const char *s, size_t n); /* all of s to fd 1 or 2, unbuffered */
void tv_err_cstr(const char *s);                     /* to stderr */

/* Small objects (strings, boxes, class instances, closures' cells: up to 512 bytes) come from
 * per-size-class free lists shared by the runtime and generated code (8-byte classes).
 * tv_small_refill carves more blocks when a list is empty. Memory is reused, never returned. */
enum { TV_SMALL_CLASSES = 65 };
extern void *tv_small_bins[TV_SMALL_CLASSES];
void *tv_small_refill(size_t c);
void *tv_realloc(void *p, size_t size);
void tv_free(void *p);

/* ------------------------------------------------------------------ string builder */

/* Zero-initialize ({0}) before use. `data` is owned by the runtime (it sits inside a
 * larger allocation so tv_str_from_sb can adopt it without copying): read it freely, but
 * release it only with tv_sb_free / tv_str_from_sb / tv_out_sb_line, never free(). */
typedef struct tv_sb {
    char *data;
    size_t len, cap;
} tv_sb;

void tv_sb_push_slow(tv_sb *sb, const char *s, size_t n);
void tv_sb_grow(tv_sb *sb, size_t need);          /* capacity >= need bytes */
/* memcpy for the short pieces strings are mostly built from: overlapping word copies instead
 * of a call into the C library (which costs more than copying a few bytes). */
static inline void tv_copy(char *d, const char *s, size_t n) {
    if (n <= 32) {
        if (n >= 16) {
            uint64_t a, b, c, e;
            memcpy(&a, s, 8);
            memcpy(&b, s + 8, 8);
            memcpy(&c, s + n - 16, 8);
            memcpy(&e, s + n - 8, 8);
            memcpy(d, &a, 8);
            memcpy(d + 8, &b, 8);
            memcpy(d + n - 16, &c, 8);
            memcpy(d + n - 8, &e, 8);
        } else if (n >= 8) {
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
/* Appends n bytes: copied inline when they fit (a template's pieces, mostly), else the builder
 * grows first. */
static inline void tv_sb_push(tv_sb *sb, const char *s, size_t n) {
    if (__builtin_expect(sb->cap - sb->len >= n && sb->data != NULL, 1)) {
        tv_copy(sb->data + sb->len, s, n);
        sb->len += n;
        return;
    }
    tv_sb_push_slow(sb, s, n);
}
/* Inline fast paths for generated code. */
static inline void tv_sb_add(tv_sb *sb, const char *s, size_t n) {
    if (__builtin_expect(sb->cap - sb->len < n, 0)) tv_sb_grow(sb, sb->len + n);
    tv_copy(sb->data + sb->len, s, n);
    sb->len += n;
}
static inline void tv_sb_add_char(tv_sb *sb, char c) {
    if (__builtin_expect(sb->cap == sb->len, 0)) tv_sb_grow(sb, sb->len + 1);
    sb->data[sb->len++] = c;
}
void tv_sb_push_cstr(tv_sb *sb, const char *s);
void tv_sb_push_char(tv_sb *sb, char c);
void tv_sb_free(tv_sb *sb);

/* Radix sorts: 64-bit keys (with an optional index permuted alongside, stable), and
 * xs.sort((a, b) => a - b) (or b - a) on f64[], in place. */
void tv_radix64(uint64_t *keys, int64_t *idx, tv_int n);
void tv_sort_f64(double *a, tv_int n, bool desc);
void tv_sort_typed(void *data, tv_int n, int kind);   /* a typed array's sort(): by number */

/* ------------------------------------------------------------------ type descriptors */

struct tv_type;
typedef struct tv_type {
    size_t size;                                  /* sizeof the C representation (0 allowed) */
    void (*retain)(void *p);                      /* NULL: plain data, nothing to retain */
    void (*release)(void *p);                     /* NULL: plain data, nothing to release */
    bool (*eq)(const void *a, const void *b);     /* value equality (===) */
    uint64_t (*hash)(const void *p);              /* consistent with eq */
    void (*to_str)(tv_sb *sb, const void *p);     /* JavaScript String(x) */
    void (*inspect)(tv_sb *sb, const void *p, int depth); /* console.log formatting (Node style) */
} tv_type;

/* console.log formatting of any value. The runtime's own descriptors leave `inspect` NULL (so a
 * program that never inspects doesn't link the formatter); this handles them. */
void tv_inspect_value(tv_sb *sb, const tv_type *t, const void *p, int depth);

/* `unknown`: a value of any type with its type's descriptor, `b` a counted box holding a copy of
 * it (NULL for undefined and null, which have no data). */
typedef struct tv_dynbox { int64_t rc; _Alignas(16) unsigned char data[]; } tv_dynbox;
typedef struct tv_dyn { const struct tv_type *t; tv_dynbox *b; } tv_dyn;
tv_dyn tv_dyn_box(const struct tv_type *t, const void *v);   /* moves *v into a new box */
static inline void tv_dyn_retain(tv_dyn d) { if (d.b) d.b->rc++; }
void tv_dyn_release(tv_dyn d);
static inline void *tv_dyn_data(tv_dyn d) { return d.b ? (void *)d.b->data : NULL; }
extern const struct tv_type tv_type_dyn;    /* an `unknown` value itself (a tv_dyn) */
double tv_dyn_number(tv_dyn d);    /* the number an `unknown` holds (as tested) */
bool tv_dyn_truthy(tv_dyn d);
bool tv_dyn_eq(tv_dyn a, tv_dyn b);
bool tv_dyn_is_num(const struct tv_type *t);

/* Descriptors for primitives. */
extern const tv_type tv_type_int, tv_type_f64, tv_type_f32, tv_type_bool, tv_type_str, tv_type_undefined, tv_type_null;
extern const tv_type tv_type_promise;   /* every Promise<T> (a tv_promise *): Node's `Promise { ... }` */
extern const tv_type tv_type_i8, tv_type_i16, tv_type_i32, tv_type_u8, tv_type_u16, tv_type_u32, tv_type_u64;

/* ------------------------------------------------------------------ strings (immutable UTF-8) */

/* A function with no effect but its result, read from its arguments and memory: a call whose
 * result goes unused is dropped, one of the same arguments made once (strings never change). */
#define TV_PURE __attribute__((pure))

typedef struct tv_strbuf {
    int32_t rc;        /* the count (TV_STR_COUNT bits) and what's known of the text (TV_STR_ASCII
                        * or TV_STR_WIDE, set once it's first looked at); < 0: immortal (literals) */
    int32_t len;       /* bytes, excluding the trailing NUL (strings are < 2 GiB) */
    char data[];       /* NUL-terminated */
} tv_strbuf;

#define TV_STR_COUNT 0x1FFFFFFF
#define TV_STR_ASCII 0x20000000   /* every byte is ASCII: UTF-16 indexes are byte indexes */
#define TV_STR_WIDE  0x40000000   /* some byte isn't */
#define TV_STR_IMMORTAL ((int32_t)0x80000000)

typedef struct tv_str {
    tv_strbuf *p;      /* never NULL; the empty string is tv_empty_str */
} tv_str;

extern const tv_strbuf tv_empty_strbuf;
#define TV_EMPTY_STR ((tv_str){(tv_strbuf *)&tv_empty_strbuf})

/* A static literal of ASCII text: TV_STR_LIT(name, "text") declares `name` usable as
 * TV_LIT(name). Immortal buffers are never written, so they're const (read-only pages,
 * nothing in __data). */
#define TV_STR_LIT(name, text) \
    static const struct { int32_t rc; int32_t len; char data[sizeof(text)]; } name = { TV_STR_IMMORTAL | TV_STR_ASCII, sizeof(text) - 1, text }
#define TV_LIT(name) ((tv_str){(tv_strbuf *)&(name)})

static inline void tv_str_retain(tv_str s) { if (s.p->rc >= 0) s.p->rc++; }
void tv_str_release_slow(tv_str s);
static inline void tv_str_release(tv_str s) { if (s.p->rc > 0 && (--s.p->rc & TV_STR_COUNT) == 0) tv_str_release_slow(s); }

tv_str tv_str_from(const char *bytes, size_t n);
tv_str tv_str_alloc(size_t n, char **data); /* a new string of n bytes, *data to fill (n > 1: a one-byte string is shared) */
tv_str tv_str_from_sb(tv_sb *sb);               /* takes the builder's contents; sb is reset */
tv_str tv_str_concat(tv_str a, tv_str b);
void tv_str_append(tv_str *s, tv_str t);                     /* s += t (in place when s is the only holder) */
void tv_str_append_bytes(tv_str *s, const char *bytes, size_t n, int32_t kind);
void tv_str_append_slice(tv_str *s, tv_str t, tv_int start, tv_int end, bool has_end);   /* s += t.slice(start, end) */
tv_str tv_str_concatn(const tv_str *parts, int n);   /* a + b + c ...: one new string */
/* A generator: its frame, resumed by `next` until it finishes (resume returns true). `yv` points
 * at the frame's last yielded value; drop releases what the frame holds and frees it. */
typedef struct tv_gen { int64_t rc; void *F; bool (*resume)(void *F); void (*drop)(void *F); void *yv; bool done; } tv_gen;
tv_gen *tv_gen_new(void *F, bool (*resume)(void *F), void (*drop)(void *F), void *yv);
static inline bool tv_gen_next(tv_gen *g) {   /* true once it's finished (nothing yielded) */
    if (g->done) return true;
    if (g->resume(g->F)) { g->done = true; return true; }
    return false;
}
static inline void tv_gen_retain(tv_gen *g) { if (g) g->rc++; }
void tv_gen_release(tv_gen *g);
extern const tv_type tv_type_gen;
/* Whether n bytes at a and b are equal: under 16 inline (two overlapping words, or a few bytes),
 * longer by 16-byte vectors (tv.c). Faster than memcmp's call for the lengths strings have. */
TV_PURE bool tv_mem_eq_long(const char *a, const char *b, size_t n);
static inline bool tv_mem_eq(const char *a, const char *b, size_t n) {
    if (n >= 16) return tv_mem_eq_long(a, b, n);
    if (n >= 8) {
        uint64_t x, y, u, v;
        memcpy(&x, a, 8), memcpy(&y, b, 8), memcpy(&u, a + n - 8, 8), memcpy(&v, b + n - 8, 8);
        return !((x ^ y) | (u ^ v));
    }
    if (n >= 4) {
        uint32_t x, y, u, v;
        memcpy(&x, a, 4), memcpy(&y, b, 4), memcpy(&u, a + n - 4, 4), memcpy(&v, b + n - 4, 4);
        return !((x ^ y) | (u ^ v));
    }
    return n == 0 || (a[0] == b[0] && a[n >> 1] == b[n >> 1] && a[n - 1] == b[n - 1]);
}
/* (inline: against a literal, whose length and bytes are constants, it's a length test and a
 * few word compares) */
static inline bool tv_str_eq(tv_str a, tv_str b) {
    return a.p == b.p || (a.p->len == b.p->len && tv_mem_eq(a.p->data, b.p->data, (size_t)b.p->len));
}
TV_PURE int tv_str_cmp_long(tv_str a, tv_str b);
/* byte order == code point order (inline: short strings, `a < b` of keys or numbers' text, mostly
 * differ in their first bytes) */
static inline int tv_str_cmp(tv_str a, tv_str b) {
    if (a.p == b.p) return 0;
    int32_t na = a.p->len, nb = b.p->len, n = na < nb ? na : nb;
    if (n > 16) return tv_str_cmp_long(a, b);
    for (int32_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a.p->data[i], y = (unsigned char)b.p->data[i];
        if (x != y) return x < y ? -1 : 1;
    }
    return na < nb ? -1 : na > nb;
}
/* tv_str_cmp(s, the one-character string c), c ASCII: s's first byte decides (a lead byte of a
 * longer character is above every ASCII one), then whether there's more of it. */
static inline int tv_str_cmp1(tv_str s, unsigned char c) {
    if (s.p->len == 0) return -1;
    unsigned char b = (unsigned char)s.p->data[0];
    if (b != c) return b < c ? -1 : 1;
    return s.p->len > 1;
}
TV_PURE uint64_t tv_str_hash(tv_str s);
static inline tv_int tv_str_byte_len(tv_str s) { return s.p->len; }

/* JavaScript's view of a string: UTF-16 code units. An ASCII string's unit indexes are its byte
 * indexes; others' lengths and positions come from a few cached ones (so a loop over
 * `s.charCodeAt(i)` walks the string once). A unit that is half of a surrogate pair, cut out on
 * its own (by `slice`, `s[i]`, `split("")`), becomes U+FFFD: UTF-8 can't hold a lone surrogate. */
bool tv_str_classify(tv_strbuf *p);             /* sets TV_STR_ASCII or TV_STR_WIDE; true if ASCII */
static inline bool tv_str_is_ascii(tv_str s) {
    int32_t rc = s.p->rc;
    return (rc & TV_STR_ASCII) || (!(rc & TV_STR_WIDE) && tv_str_classify(s.p));
}
/* What's cached of a non-ASCII string (the last one looked at is tv_uview_last): its UTF-16
 * length (-1 until needed), a position (unit u starts at byte b), and once it's indexed again
 * and again, its UTF-16 units. */
typedef struct tv_uview { const tv_strbuf *p; int64_t len, u, b; uint16_t *units; int64_t looks; } tv_uview;
extern tv_uview *tv_uview_last;
tv_int tv_str_wide_length(tv_strbuf *p);
static inline tv_int tv_str_length(tv_str s) {
    if (tv_str_is_ascii(s)) return s.p->len;
    const tv_uview *v = tv_uview_last;
    return v->p == s.p && v->len >= 0 ? v->len : tv_str_wide_length(s.p);
}
TV_PURE tv_int tv_str_char_code_at_slow(tv_str s, tv_int i);
/* s.charCodeAt(i); out of range -1 (JavaScript's NaN: it fails every comparison with a code) */
static inline tv_int tv_str_char_code_at(tv_str s, tv_int i) {
    const tv_strbuf *p = s.p;
    if (__builtin_expect((p->rc & TV_STR_ASCII) && (uint64_t)i < (uint64_t)p->len, 1)) return (unsigned char)p->data[i];
    tv_uview *v = tv_uview_last;
    if (v->p == p && v->units && (uint64_t)i < (uint64_t)v->len) return v->units[i];
    /* the unit after the cached one, both ASCII characters: a step */
    if (v->p == p && i == v->u + 1 && i < v->len) {
        const unsigned char *d = (const unsigned char *)p->data + v->b;
        if ((d[0] | d[1]) < 0x80) {
            v->u = i;
            v->b++;
            return d[1];
        }
    }
    return tv_str_char_code_at_slow(s, i);
}
tv_int tv_str_code_point_at(tv_str s, tv_int i);            /* -1 out of range */
bool tv_str_next_char_slow(tv_str s, tv_int *pos, tv_str *out); /* for...of: the next code point, owned (see tv_str_next_char) */
tv_str tv_str_unit_at_slow(tv_str s, tv_int i);
tv_str tv_str_char_at(tv_str s, tv_int i);                  /* charAt: "" out of range */
tv_str tv_str_from_char_code(tv_int c);                     /* String.fromCharCode(c) */
tv_str tv_str_from_char_codes(const tv_int *c, size_t n);   /* pairs surrogates given together */
tv_str tv_str_from_code_points(const tv_int *c, size_t n, const char *loc);   /* traps on an invalid code point */

/* JavaScript-compatible conversions */
tv_str tv_str_from_int(tv_int v);
tv_str tv_str_from_f64(double v);               /* Number.prototype.toString(): 1 → "1", 0.1+0.2 → "0.30000000000000004", 1e21 → "1e+21" */
tv_str tv_int_to_radix(tv_int v, tv_int radix, const char *loc);   /* (255).toString(16) → "ff"; traps outside 2..36 */
tv_str tv_f64_to_radix(double v, tv_int radix, const char *loc);   /* as Number.prototype.toString(radix), fractions included */
tv_str tv_str_from_bool(bool v);
tv_str tv_f64_to_fixed(double v, tv_int digits, const char *loc); /* JS toFixed (round half up on exact ties); traps if digits ∉ 0..100 */
tv_str tv_int_to_fixed(tv_int v, tv_int digits, const char *loc);
tv_str tv_f64_to_locale(double v, int min_frac, int max_frac); /* Intl.NumberFormat("en-US"): grouped, fraction digits rounded as toFixed */
tv_str tv_int_to_locale(tv_int v, int min_frac);
tv_str tv_f64_to_precision(double v, tv_int digits, const char *loc); /* JS toPrecision; traps if digits ∉ 1..100 */
tv_str tv_f64_to_exponential(double v, tv_int digits, bool has_digits, const char *loc); /* JS toExponential */
tv_int tv_str_locale_compare(tv_str a, tv_str b); /* -1, 0, 1: ICU's root collation (base letters, then accents, then case) */
void tv_sb_push_int(tv_sb *sb, tv_int v);
void tv_sb_push_f64(tv_sb *sb, double v);       /* same format as tv_str_from_f64 */
/* A TypeScript number held as an integer: past 2^53 written as JavaScript writes that double. */
static inline void tv_sb_push_jint(tv_sb *sb, tv_int v) { if ((uint64_t)v + (UINT64_C(1) << 53) > (UINT64_C(1) << 54)) tv_sb_push_f64(sb, (double)v); else tv_sb_push_int(sb, v); }
static inline tv_str tv_str_from_jint(tv_int v) { return (uint64_t)v + (UINT64_C(1) << 53) > (UINT64_C(1) << 54) ? tv_str_from_f64((double)v) : tv_str_from_int(v); }
void tv_sb_push_str(tv_sb *sb, tv_str s);

/* Parsing: returns false when the whole string (after trimming whitespace) isn't a number. */
bool tv_parse_float(tv_str s, double *out);     /* JS Number(s)/parseFloat subset: decimal, exponent, Infinity */
bool tv_parse_int(tv_str s, tv_int radix, tv_int *out);  /* radix 0 = 10 (or 16 with 0x); JS parseInt accepts a numeric prefix */
/* Regular expressions (runtime/regexp.c, linked when a program uses them): a compiled
 * expression's id (-1 and an error when it doesn't compile); a match's group positions. */
tv_int tv_native_reCompile(tv_str source, tv_str flags);
/* The matchers `tov build` compiled from the program's patterns (runtime/regexp.c's tv_rejit). */
void tv_rejit_table(const void *table, size_t n);
tv_str tv_native_reFlags(tv_int h);
struct tv_arr tv_native_reExec(tv_int h, tv_str s, tv_int from);
tv_int tv_native_reTest(tv_int h, tv_str s, tv_int from);
/* A regex's matches in a string one by one (runtime/regexp.c; see tv_re_iter_init). */
typedef struct tv_re_iter {
    tv_int h, n, from;
    const uint8_t *buf;
    uint16_t *units;
    bool ascii;
    int type, shift;
    uint8_t **cap;
    uint8_t *on_stack[16];
} tv_re_iter;
void tv_re_iter_init(tv_re_iter *it, tv_int h, tv_str s);
bool tv_re_iter_next(tv_re_iter *it, tv_int pos[2]);
tv_str tv_re_iter_group(const tv_re_iter *it, tv_str s, int k); /* group k of the last match ("" when it took no part; past them its offset as text, then s) */
void tv_re_iter_done(tv_re_iter *it);
struct tv_arr tv_native_reMatchAll(tv_int h, tv_str s);           /* a global match's strings */
void tv_sb_push_units(tv_sb *sb, tv_str s, tv_int a, tv_int b);   /* units a..b of s */
struct tv_arr tv_native_reExecInto(tv_int h, tv_str s, tv_int from, struct tv_arr p);
struct tv_arr tv_native_reNames(tv_int h);
struct tv_arr tv_native_reSplit(tv_int h, tv_str s, tv_int max);   /* s.split(re, max) */
tv_str tv_native_reReplace(tv_int h, tv_str s, tv_str rep);        /* s.replace(re, rep): rep without `$`, re not sticky */
void tv_native_error(const char *msg);         /* the message __native.takeError() returns next */
double tv_js_number(tv_str s);                  /* Number(s): NaN when it isn't one; "" is 0; 0x/0o/0b */
double tv_js_parse_float(tv_str s);             /* parseFloat(s): the longest decimal prefix, else NaN */
double tv_js_parse_float_slice(tv_str s, tv_int start, tv_int end, bool has_end);   /* parseFloat(s.slice(start, end)) */

/* String methods (JavaScript semantics: indexes count UTF-16 code units). Negative indices
 * count from the end, as in JS slice. */
struct tv_arr;
struct tv_arr tv_str_chars(tv_str s);           /* one string per Unicode scalar value */
tv_int tv_str_char_count(tv_str s);
tv_str tv_str_slice(tv_str s, tv_int start, tv_int end, bool has_end, const char *loc);
TV_PURE bool tv_str_includes(tv_str s, tv_str needle);
tv_str tv_str_substring_slow(tv_str s, tv_int start, tv_int end, bool has_end);
/* Immortal one-character strings for printable ASCII (32..127). */
typedef struct { int32_t rc; int32_t len; char data[4]; } tv_small_strbuf;
extern const tv_small_strbuf tv_ascii_strs[96];
static inline tv_str tv_ascii_str(unsigned char c) { return (tv_str){(tv_strbuf *)(void *)&tv_ascii_strs[c - 32]}; }
/* One unit as a string (0 <= i < length): a printable ASCII one is the shared one-character
 * string, inline (a literal's characters become table lookups). */
static inline tv_str tv_str_unit_at(tv_str s, tv_int i) {
    if (tv_str_is_ascii(s)) {
        unsigned char c = (unsigned char)s.p->data[i];
        if (c - 32u < 96u) return tv_ascii_str(c);
    }
    return tv_str_unit_at_slow(s, i);
}
/* for...of: the next code point, owned; a printable ASCII one inline (the shared string). */
static inline bool tv_str_next_char(tv_str s, tv_int *pos, tv_str *out) {
    tv_int i = *pos;
    if (__builtin_expect(i < (tv_int)s.p->len, 1)) {
        unsigned char c = (unsigned char)s.p->data[i];
        if (__builtin_expect(c - 32u < 96u, 1)) {
            *out = tv_ascii_str(c);
            *pos = i + 1;
            return true;
        }
    }
    return tv_str_next_char_slow(s, pos, out);
}
tv_str tv_str_ascii_slice(tv_str s, int64_t a, int64_t b);  /* bytes a..b of an ASCII string */
static inline tv_str tv_str_substring(tv_str s, tv_int start, tv_int end, bool has_end) {
    /* (one ASCII character, `s.substring(i, i + 1)`: the shared one-character string) */
    if (has_end && end == start + 1 && (uint64_t)start < (uint64_t)s.p->len && tv_str_is_ascii(s)) {
        unsigned char c = (unsigned char)s.p->data[start];
        if (c - 32u < 96u) return tv_ascii_str(c);
    }
    /* (an ASCII string: its unit indexes are byte indexes) */
    if (tv_str_is_ascii(s)) {
        int64_t len = s.p->len;
        int64_t a = start < 0 ? 0 : start > len ? len : start;
        int64_t b = !has_end ? len : end < 0 ? 0 : end > len ? len : end;
        return a <= b ? tv_str_ascii_slice(s, a, b) : tv_str_ascii_slice(s, b, a);
    }
    return tv_str_substring_slow(s, start, end, has_end);
}
/* (inline: against a literal, whose length and bytes are constants, a length test and a word
 * compare) */
static inline bool tv_str_starts_with(tv_str s, tv_str prefix) {
    return prefix.p->len <= s.p->len && tv_mem_eq(s.p->data, prefix.p->data, (size_t)prefix.p->len);
}
static inline bool tv_str_ends_with(tv_str s, tv_str suffix) {
    return suffix.p->len <= s.p->len && tv_mem_eq(s.p->data + s.p->len - suffix.p->len, suffix.p->data, (size_t)suffix.p->len);
}
tv_str tv_str_substr(tv_str s, tv_int start, tv_int count, bool has_count);
TV_PURE tv_int tv_str_index_of(tv_str s, tv_str needle);            /* unit index or -1 */
TV_PURE tv_int tv_str_index_of_from(tv_str s, tv_str needle, tv_int from);
TV_PURE tv_int tv_str_sub_len(tv_str s, int kind, tv_int start, tv_int end, bool has_end);
TV_PURE tv_int tv_str_last_index_of(tv_str s, tv_str needle, tv_int from, bool has_from);
TV_PURE bool tv_str_includes_at(tv_str s, tv_str needle, tv_int from);
TV_PURE bool tv_str_starts_with_at(tv_str s, tv_str prefix, tv_int pos);
TV_PURE bool tv_str_ends_with_at(tv_str s, tv_str suffix, tv_int end);
struct tv_arr tv_str_split(tv_str s, tv_str sep);
struct tv_arr tv_str_split_limit(tv_str s, tv_str sep, double limit); /* s.split(sep, limit) */           /* "" separator splits into UTF-16 units */
tv_str tv_str_trim(tv_str s);
tv_str tv_str_trim_start(tv_str s);
tv_str tv_str_trim_end(tv_str s);
tv_str tv_str_to_upper(tv_str s);                           /* ASCII letters only in M1 */
tv_str tv_str_to_lower(tv_str s);
tv_str tv_str_replace(tv_str s, tv_str search, tv_str replacement);      /* first occurrence */
tv_str tv_str_replace_all(tv_str s, tv_str search, tv_str replacement);
tv_str tv_str_repeat(tv_str s, tv_int count, const char *loc);         /* traps on count < 0 */
tv_str tv_str_pad_start(tv_str s, tv_int len, tv_str fill);            /* len in UTF-16 units */
tv_str tv_str_pad_end(tv_str s, tv_int len, tv_str fill);

/* ------------------------------------------------------------------ arrays (shared) */

/* Representation: an array is a pointer to its header, which holds the length, the capacity and
 * where the elements are: right after the header when it was allocated with room for them, or
 * a block of their own once it has grown (the header never moves).
 *
 * Arrays are shared, as in JavaScript: every reference to one sees its length and elements, and
 * changes made through any of them. The header owns exactly the elements [0, len); elements at
 * index >= len are not owned (e.g. after pop). A header with rc < 0 is immortal (tv_empty_arrbuf:
 * TV_EMPTY_ARR stands in where no array has been given yet) and is never changed or freed:
 * tv_arr_make_unique gives a place holding it an array of its own first. A NULL header (zeroed
 * memory) is only released, never read. */
/* The header's counts have a 64-bit type that no element has (tv_int is int64_t; this is the
 * other one of long and long long): a store into an element then can't change the length or
 * the count as far as the C compiler knows, so it keeps them in registers around such stores. */
#if defined(__APPLE__)
typedef long tv_hcount;
#else
typedef long long tv_hcount;
#endif
typedef struct tv_arrbuf {
    tv_hcount rc;
    tv_hcount len;
    tv_hcount cap;
    /* The elements' block once they've grown out of the header; NULL while they follow it (the
     * header holds no pointer to itself: such a pointer would count as the header escaping, and
     * the C compiler would stop tracking its length across calls). Read through tv_arrbuf_data. */
    unsigned char *ext;
} tv_arrbuf;                    /* 32 bytes: elements that follow it are 16-byte aligned */

static inline unsigned char *tv_arrbuf_data(const tv_arrbuf *b) { return b->ext ? b->ext : (unsigned char *)(b + 1); }
/* A header and `data` bytes of elements after it: a block of a multiple of 16 bytes (so the
 * elements stay 16-byte aligned), with room for at least one word (where the elements' count
 * goes once they move out, see tv_arr_make_unique). Small ones come from the small-object free
 * lists: a tree of little arrays is made and dropped without malloc. */
enum { TV_ARRBUF_SMALL = (TV_SMALL_CLASSES - 1) * 8 };
static inline size_t tv_arrbuf_block(size_t data) {
    size_t s = sizeof(tv_arrbuf) + (data < 8 ? 8 : data);
    return s < data ? SIZE_MAX : (s + 15) & ~(size_t)15;
}
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
#define TV_ARRBUF_PLAIN 1
#endif
static inline void *tv_arrbuf_alloc(size_t data) {
    size_t s = tv_arrbuf_block(data);
#ifndef TV_ARRBUF_PLAIN
    if (s <= TV_ARRBUF_SMALL) {
        size_t c = s >> 3;
        void **f = (void **)tv_small_bins[c];
        if (__builtin_expect(f != NULL, 1)) { tv_small_bins[c] = *f; return f; }
        return tv_small_refill(c);
    }
#endif
    return tv_alloc(s);
}

typedef struct tv_arr {
    tv_arrbuf *p;
} tv_arr;

extern tv_arrbuf tv_empty_arrbuf;
#define TV_EMPTY_ARR ((tv_arr){&tv_empty_arrbuf})

/* (an array's length is under 2^48: its elements fill memory, and no machine has that much.
 * Saying so lets the C compiler see that `i + j` of indexes below two lengths can't overflow.) */
static inline tv_int tv_arr_len(tv_arr a) {
    tv_int n = (tv_int)a.p->len;
    if ((uint64_t)n >= ((uint64_t)1 << 48)) __builtin_unreachable();
    return n;
}
static inline void *tv_arr_data(tv_arr a) { return (void *)tv_arrbuf_data(a.p); }
/* A fresh header for len elements of `es` bytes (len set; elements not written yet). */
static inline tv_arr tv_arr_of(tv_arrbuf *b, int64_t len) { b->len = len; return (tv_arr){b}; }
/* A fresh array of n elements of `es` bytes each, inline (elements not written yet). Inline, so
 * the C compiler sees the length it stores, and that nothing else can change it until the
 * array is passed somewhere: a literal's length stays a constant it can unroll loops by. */
static inline tv_arr tv_arr_new_len(size_t es, tv_int n) {
    tv_arrbuf *b = (tv_arrbuf *)tv_arrbuf_alloc((size_t)n * es);
    b->rc = 1;
    b->len = n;
    b->cap = n;
    b->ext = NULL;
    return (tv_arr){b};
}
/* `new Array(n)` of zeros (0, false, null): made and cleared here, the length a constant the
 * C compiler can see. */
static inline tv_arr tv_arr_zeros(size_t es, tv_int n, const char *loc) {
    if (__builtin_expect((uint64_t)n > (uint64_t)INT32_MAX, 0)) tv_trap("Invalid array length", loc);
    tv_arr a = tv_arr_new_len(es, n);
    if (n) memset(tv_arrbuf_data(a.p), 0, (size_t)n * es);
    return a;
}
/* `new Array(n)` whose elements the caller writes next (all n of them). */
static inline tv_arr tv_arr_uninit(size_t es, tv_int n, const char *loc) {
    if (__builtin_expect((uint64_t)n > (uint64_t)INT32_MAX, 0)) tv_trap("Invalid array length", loc);
    return tv_arr_new_len(es, n);
}
/* A new empty array of its own (what every array a program sees is: TV_EMPTY_ARR, shared,
 * only stands in where no value has been given yet). */
static inline tv_arr tv_arr_fresh(void) { return tv_arr_new_len(0, 0); }
static inline void tv_arr_retain(tv_arr a) { if (a.p && a.p->rc >= 0) a.p->rc++; }
void tv_arr_release_slow(tv_arr a, const tv_type *t);
/* (the header's pointer goes nowhere but free(): saying so lets the C compiler keep what it knows
 * about an array, its length, across calls made after a release that could have freed it) */
#if defined(__clang__)
#define TV_NOESCAPE __attribute__((noescape))
#else
#define TV_NOESCAPE
#endif
/* The retains of object descriptors (a count first in the object, 32 or 64 bits): copies of
 * arrays of them retain inline when the descriptor's retain is one of these. */
void tv_retain_rc32(void *p);
void tv_retain_rc64(void *p);
void tv_arrbuf_release_slow(tv_arrbuf *TV_NOESCAPE p, const tv_type *t);
void tv_arrbuf_free_block(tv_arrbuf *TV_NOESCAPE p, size_t es);
/* Frees an array's blocks, its elements released: inline when they follow the header and the
 * block goes back on a small-object list. */
/* Arrays' own properties (`T[] & { links: L }`), by header: see tv_arr_prop_get. */
extern int64_t tv_arr_props_n;
/* (set by tv_arr_prop_set, the first time any array gets a property: freeing an array calls it
 * only then, and a program that never sets one doesn't link the map of them, nor the dynamic
 * values and their formatting that come with it) */
extern void (*tv_arr_props_forget)(tv_arrbuf *p);
tv_dyn tv_arr_prop_get(tv_arr a, tv_str key);              /* owned; undefined if not set */
void tv_arr_prop_set(tv_arr *a, tv_str key, tv_dyn v);     /* v moved */
static inline void tv_arrbuf_free(tv_arrbuf *p, size_t es) {
    if (__builtin_expect(tv_arr_props_n != 0, 0)) tv_arr_props_forget(p);
#ifndef TV_ARRBUF_PLAIN
    if (!p->ext) {
        size_t s = tv_arrbuf_block((size_t)p->cap * es);
        if (s <= TV_ARRBUF_SMALL) {
            size_t c = s >> 3;
            *(void **)(void *)p = tv_small_bins[c];
            tv_small_bins[c] = p;
            return;
        }
    }
#endif
    tv_arrbuf_free_block(p, es);
}
/* Release with the array type's own free function (generated code: see arr_free_fn). */
static inline void tv_arr_release_with(tv_arr a, void (*fr)(tv_arrbuf *)) { if (a.p && a.p->rc > 0 && --a.p->rc == 0) fr(a.p); }
static inline void tv_arr_release(tv_arr a, const tv_type *t) { if (a.p && a.p->rc > 0 && --a.p->rc == 0) tv_arrbuf_release_slow(a.p, t); }

/* Ensures *a can be changed (an array of its own in place of the immortal empty one) with
 * capacity >= min_cap. A shared array is changed where it is, for every reference to it. */
void tv_arr_make_unique(tv_arr *a, const tv_type *t, tv_int min_cap);
tv_arr tv_arr_with_capacity(const tv_type *t, tv_int cap);   /* len 0 */
/* Reserve capacity for exactly n more elements and return a pointer to where element a->p->len
   goes (header made unique; caller writes elements and then adds to a->p->len). */
void *tv_arr_reserve_tail(tv_arr *a, const tv_type *t, tv_int n);

/* Index checks: trap with "index <i> out of bounds for length <n>". */
_Noreturn void tv_arr_oob(tv_int i, tv_int len, const char *loc);
static inline void *tv_arr_at(tv_arr a, size_t elem_size, tv_int i, const char *loc) {
    if ((uint64_t)i >= (uint64_t)a.p->len) tv_arr_oob(i, a.p->len, loc);
    return tv_arrbuf_data(a.p) + (size_t)i * elem_size;
}
/* Bytes stored into arrays go through these: through a character type (uint8_t) a store could
 * change any object as far as the C compiler knows, so it reloads around it what it kept in
 * registers. _BitInt(8) is the same size and isn't a character type; every other access stays
 * uint8_t, which may read and write anything. */
#if defined(__BITINT_MAXWIDTH__)
typedef unsigned _BitInt(8) TV_U8S;
typedef signed _BitInt(8) TV_I8S;
#else
typedef uint8_t TV_U8S;
typedef int8_t TV_I8S;
#endif
/* An index checked against a length held apart (an array's, read before a loop that can't change
 * it). A macro: as an inline function it keeps clang from pairing up the loop's float work. */
#define TV_IDX(i, len, loc) ({ tv_int i_ = (i), n_ = (len); if ((uint64_t)i_ >= (uint64_t)n_) tv_arr_oob(i_, n_, loc); i_; })
/* Mutable element access: makes the array unique first. */
void *tv_arr_at_mut(tv_arr *a, const tv_type *t, tv_int i, const char *loc);

void tv_arr_push(tv_arr *a, const tv_type *t, void *elem);         /* moves elem */
void tv_arr_set_length_fill(tv_arr *a, const tv_type *t, tv_int n, const void *fill, const char *loc);
tv_arr tv_gen_collect(tv_gen *g, const tv_type *t);   /* every value it yields (retained), in an array */
tv_gen *tv_gen_of_arr(tv_arr a, const tv_type *t);    /* a generator over an array's elements (takes a)  */
static inline void tv_arr_push_fast(tv_arr *a, const tv_type *t, void *elem) {
    if (a->p && a->p->rc > 0 && a->p->len < a->p->cap) {
        memcpy(tv_arrbuf_data(a->p) + (size_t)a->p->len * t->size, elem, t->size);
        a->p->len++;
    } else {
        tv_arr_push(a, t, elem);
    }
}
bool tv_arr_pop(tv_arr *a, const tv_type *t, void *out);          /* false if empty; out receives ownership */
/* `xs.length = n`: drops the elements from n on (n past the length traps: JavaScript's holes
 * have no value of the element type). */
void tv_arr_set_length(tv_arr *a, const tv_type *t, tv_int n, const char *loc);
bool tv_arr_shift(tv_arr *a, const tv_type *t, void *out);
void tv_arr_unshift(tv_arr *a, const tv_type *t, void *elem);      /* moves elem */
/* a.splice(start, del, ...items): the removed elements (JavaScript's clamping; del omitted: to the end) */
tv_arr tv_arr_splice(tv_arr *a, const tv_type *t, tv_int start, tv_int del, bool has_del, tv_arr items);
tv_arr tv_arr_slice(tv_arr a, const tv_type *t, tv_int start, tv_int end, bool has_start, bool has_end); /* JS semantics incl. negatives */
tv_arr tv_arr_concat(tv_arr a, tv_arr b, const tv_type *t);
void tv_arr_extend(tv_arr *a, const tv_type *t, tv_arr b);         /* appends b's elements (retained) */
tv_arr tv_arr_make(const tv_type *t, tv_int n, const void *elem, const char *loc);   /* n copies of elem (retained); traps if n < 0 */
void tv_arr_fill(tv_arr *a, const tv_type *t, const void *elem, tv_int start, bool has_start, tv_int end, bool has_end);   /* JS fill: copies of elem */
bool tv_arr_set_from(tv_arr *a, const tv_type *t, tv_arr src, tv_int offset); /* a typed array's set: src's plain elements copied in at offset (false: past the end) */
void tv_arr_reverse(tv_arr *a, const tv_type *t);
/* Stable sort. cmp returns <0, 0, >0 like a JS comparator (a double). NULL cmp = JS default
 * (compare String(x) of each element). */
void tv_arr_sort(tv_arr *a, const tv_type *t, double (*cmp)(void *ctx, const void *x, const void *y), void *ctx);
tv_int tv_arr_index_of(tv_arr a, const tv_type *t, const void *elem);
bool tv_arr_includes(tv_arr a, const tv_type *t, const void *elem); /* SameValueZero: NaN is found */
tv_int tv_arr_last_index_of(tv_arr a, const tv_type *t, const void *elem);
tv_str tv_arr_join(tv_arr a, const tv_type *t, tv_str sep);       /* elements via t->to_str; "undefined" members print as "" (JS) */

/* ------------------------------------------------------------------ maps and sets (insertion-ordered, shared) */

/* A map (or set) is a pointer to its header, which every reference shares (as JavaScript's
 * objects): changes through any reference are seen through all. NULL stands in where no map has
 * been given yet (a change gives the place a map of its own first). */
typedef struct tv_maphdr tv_maphdr;   /* private layout */
typedef struct tv_map {
    tv_maphdr *p;
} tv_map;

#define TV_EMPTY_MAP ((tv_map){NULL})

tv_map tv_map_fresh(void);                                           /* a new empty map */
tv_map tv_map_fresh_object(void);                                    /* the same, made as an object (printed as one) */
tv_map tv_map_copy(tv_map m, const tv_type *kt, const tv_type *vt);  /* new Map(m): a new map, same entries */
/* A map of n entries with distinct keys, keys[i] to vals[i] (moved in), made at its size: an object
 * literal. hashes (or NULL): n slots kept for constant keys, filled the first time (0: not yet). */
tv_map tv_map_from_entries(const tv_type *kt, const tv_type *vt, tv_int n, const void *keys, const void *vals, bool object, uint64_t *hashes);

void tv_map_retain(tv_map m);
void tv_map_release(tv_map m, const tv_type *kt, const tv_type *vt);
tv_int tv_map_size(tv_map m);
/* Borrowed pointer to the value for key, or NULL. Valid until the map is next mutated. */
void *tv_map_get(tv_map m, const tv_type *kt, const tv_type *vt, const void *key);
/* The same, to change in place (`m.get(k)!.push(x)`): the map is made unique first. */
void *tv_map_get_mut(tv_map *mp, const tv_type *kt, const tv_type *vt, const void *key);
void tv_map_set(tv_map *m, const tv_type *kt, const tv_type *vt, void *key, void *value);   /* moves key and value; replaces (and releases) an existing value, keeps insertion position */
bool tv_map_has(tv_map m, const tv_type *kt, const tv_type *vt, const void *key);
/* `m.set(k, (m.get(k) ?? d) op e)` in one probe: k's value slot, added zeroed (*inserted) when
 * missing. Moves key in when it's added, releases it when it's found. */
void *tv_map_slot(tv_map *mp, const tv_type *kt, const tv_type *vt, void *key, bool *inserted);
/* The same for the string key s.slice(start, end) (with tv_str_slice's checks), whose string is
 * only made when it's added. */
void *tv_map_slot_slice(tv_map *mp, const tv_type *vt, tv_str s, tv_int start, tv_int end, bool has_end, const char *loc, bool *inserted);
bool tv_map_delete(tv_map *m, const tv_type *kt, const tv_type *vt, const void *key);
void tv_map_clear(tv_map *m, const tv_type *kt, const tv_type *vt);
tv_arr tv_map_keys(tv_map m, const tv_type *kt, const tv_type *vt);     /* insertion order, retained copies */
tv_arr tv_map_values(tv_map m, const tv_type *kt, const tv_type *vt);
/* Iteration in insertion order: for (i = 0; tv_map_next(m, kt, vt, &i, &k, &v);) — borrowed k/v. */
bool tv_map_next(tv_map m, const tv_type *kt, const tv_type *vt, tv_int *cursor, void **key, void **value);
/* Descriptor helpers for maps used as values inside other containers. */
bool tv_map_eq(tv_map a, tv_map b, const tv_type *kt, const tv_type *vt);   /* identity-free value equality */
bool tv_map_is_object(tv_map m);   /* made as an object (`{ a: 1 }` for a Record<string, V>) */
void tv_map_inspect(tv_sb *sb, tv_map m, const tv_type *kt, const tv_type *vt, int depth); /* Map(2) { 'a' => 1, 'b' => 2 } */
/* Sets are maps whose value type has size 0: pass &tv_type_undefined as vt and NULL as value. */
void tv_set_inspect(tv_sb *sb, tv_map m, const tv_type *kt, int depth);                    /* Set(2) { 1, 2 } */

/* ------------------------------------------------------------------ console formatting */

/* Node's util.inspect as used by console.log (breakLength 80, compact 3, depth 2): numbers via
 * tv_sb_push_f64/int (-0 shown as -0), strings quoted inside containers (Node's quote choice
 * and escapes), arrays "[ 1, 2, 3 ]" ("[]" when empty), records "{ x: 1, y: 2 }" ("{}" when
 * empty), undefined, true/false. Containers switch to Node's multi-line layout (including
 * column-grouped arrays of more than 6 entries) when a line would exceed 80 columns, and print
 * [Array] / [Object] / [Map] / [Set] below depth 2. `depth` is 0 for a top-level console.log
 * argument; containers pass depth + 1 to their elements' inspect callbacks. */
void tv_inspect_str(tv_sb *sb, tv_str s, int depth);   /* depth 0 = top level: unquoted */
void tv_inspect_arr(tv_sb *sb, tv_arr a, const tv_type *t, int depth);
void tv_inspect_match(tv_sb *sb, tv_arr items, const tv_type *it, tv_int index, tv_str input, const void *groups, const tv_type *gt, int depth);
void tv_inspect_typed_arr(tv_sb *sb, tv_arr a, const tv_type *t, int depth, const char *name); /* name: a typed array's, NULL for an array */
/* console.log formatting of a record: fields in declaration order. */
void tv_inspect_record(tv_sb *sb, int depth, size_t n, const char *const *names, const tv_type *const *types, const void *const *fields);
void tv_inspect_tuple(tv_sb *sb, int depth, size_t n, const tv_type *const *types, const void *const *fields);   /* [A, B]: as an array */
/* A class instance: `Name { a: 1 }` (`[Name]` past the depth limit). */
/* While nonzero, class instances print without their class's name (`{ x: 1 }`): `toEqual`
 * comparing them with objects, as Jest does. */
extern int tv_inspect_anon;
void tv_inspect_object(tv_sb *sb, int depth, const char *cls, size_t n, const char *const *names, const tv_type *const *types, const void *const *fields);
void tv_to_str_arr(tv_sb *sb, tv_arr a, const tv_type *t);   /* JS String([1,2]) == "1,2" */

/* ------------------------------------------------------------------ output */

/* Buffered stdout; flushed at exit and before traps. */
void tv_out_write(const char *s, size_t n);
void tv_out_sb_line(tv_sb *sb);          /* writes sb + "\n" to stdout, then resets sb */
void tv_err_sb_line(tv_sb *sb);          /* same to stderr (unbuffered) */
void tv_out_flush(void);

/* ------------------------------------------------------------------ bigint (runtime/big.c) */

/* One word: odd is a small integer (word >> 1, -2^62 .. 2^62-1), even points to a buffer (sign
 * and magnitude, 64-bit limbs, least significant first). A value has one form: one that fits a
 * small integer is always small, so `==` on two small words is equality. */
typedef struct { int64_t v; } tv_big;
typedef struct tv_bigbuf {
    int64_t rc;
    uint32_t len, cap;     /* limbs in use (no leading zero limb), limbs allocated */
    uint32_t neg, pad_;
    uint64_t d[];
} tv_bigbuf;
#define TV_BIG_SMALL_MIN (-(INT64_C(1) << 62))
#define TV_BIG_SMALL_MAX ((INT64_C(1) << 62) - 1)
#define TV_BIG_UNLIKELY(x) __builtin_expect(!!(x), 0)
extern const tv_type tv_type_big;

static inline bool tv_big_is_small(tv_big a) { return (a.v & 1) != 0; }
static inline tv_big tv_big_small(int64_t x) { return (tv_big){ (int64_t)((uint64_t)x << 1) | 1 }; }
tv_big tv_big_from_i64_slow(int64_t x);
static inline tv_big tv_big_from_i64(int64_t x) {
    return (x >= TV_BIG_SMALL_MIN && x <= TV_BIG_SMALL_MAX) ? tv_big_small(x) : tv_big_from_i64_slow(x);
}
void tv_big_free(tv_big a);
static inline void tv_big_retain(tv_big a) { if (!(a.v & 1)) ((tv_bigbuf *)(intptr_t)a.v)->rc++; }
static inline void tv_big_release(tv_big a) {
    if (!(a.v & 1) && --((tv_bigbuf *)(intptr_t)a.v)->rc == 0) tv_big_free(a);
}

tv_big tv_big_add_slow(tv_big a, tv_big b);
tv_big tv_big_sub_slow(tv_big a, tv_big b);
tv_big tv_big_mul_slow(tv_big a, tv_big b);
static inline tv_big tv_big_add(tv_big a, tv_big b) {
    int64_t r;
    if (__builtin_expect(a.v & b.v & 1, 1) && !__builtin_add_overflow(a.v, b.v - 1, &r)) return (tv_big){r};
    return tv_big_add_slow(a, b);
}
static inline tv_big tv_big_sub(tv_big a, tv_big b) {
    int64_t r;
    if (__builtin_expect(a.v & b.v & 1, 1) && !__builtin_sub_overflow(a.v, b.v - 1, &r)) return (tv_big){r};
    return tv_big_sub_slow(a, b);
}
static inline tv_big tv_big_mul(tv_big a, tv_big b) {
    int64_t r;
    if (__builtin_expect(a.v & b.v & 1, 1) && !__builtin_mul_overflow(a.v >> 1, b.v - 1, &r)) return (tv_big){r | 1};
    return tv_big_mul_slow(a, b);
}
tv_big tv_big_div(tv_big a, tv_big b, const char *loc);   /* truncates; traps on 0n */
tv_big tv_big_rem(tv_big a, tv_big b, const char *loc);   /* the dividend's sign */
tv_big tv_big_pow(tv_big a, tv_big b, const char *loc);   /* traps on a negative exponent */
tv_big tv_big_neg(tv_big a);
tv_big tv_big_not(tv_big a);
tv_big tv_big_bitop(tv_big a, tv_big b, int op);           /* 0 &, 1 |, 2 ^ (two's complement) */
tv_big tv_big_shift(tv_big a, tv_big b, bool left, const char *loc);
tv_big tv_big_as_n(tv_int bits, tv_big a, bool is_signed, const char *loc);

/* x op= y: in place when x holds its buffer alone. */
void tv_big_add_to_slow(tv_big *x, tv_big y, bool sub);
void tv_big_mul_to_slow(tv_big *x, tv_big y);
static inline void tv_big_add_to(tv_big *x, tv_big y) {
    int64_t r;
    if (__builtin_expect(x->v & y.v & 1, 1) && !__builtin_add_overflow(x->v, y.v - 1, &r)) { x->v = r; return; }
    tv_big_add_to_slow(x, y, false);
}
static inline void tv_big_sub_to(tv_big *x, tv_big y) {
    int64_t r;
    if (__builtin_expect(x->v & y.v & 1, 1) && !__builtin_sub_overflow(x->v, y.v - 1, &r)) { x->v = r; return; }
    tv_big_add_to_slow(x, y, true);
}
static inline void tv_big_mul_to(tv_big *x, tv_big y) {
    int64_t r;
    if (__builtin_expect(x->v & y.v & 1, 1) && !__builtin_mul_overflow(x->v >> 1, y.v - 1, &r)) { x->v = r | 1; return; }
    tv_big_mul_to_slow(x, y);
}
/* a op b where a is owned and consumed (its buffer is reused when it's the only reference). */
static inline tv_big tv_big_add_take(tv_big a, tv_big b) { tv_big_add_to(&a, b); return a; }
static inline tv_big tv_big_sub_take(tv_big a, tv_big b) { tv_big_sub_to(&a, b); return a; }
static inline tv_big tv_big_mul_take(tv_big a, tv_big b) { tv_big_mul_to(&a, b); return a; }
tv_big tv_big_neg_take(tv_big a);
/* z + sign * x * y (sign ±1) in one pass when a factor fits a limb; _to updates z in place. */
tv_big tv_big_muladd(tv_big x, tv_big y, tv_big z, int sign);
void tv_big_muladd_to(tv_big *z, tv_big x, tv_big y, int sign);
tv_big tv_big_from_i128_(__int128 x);
/* a - b where b is owned and consumed: -(b - a). */
static inline tv_big tv_big_rsub_take(tv_big b, tv_big a) { tv_big_sub_to(&b, a); return tv_big_neg_take(b); }

int tv_big_cmp_slow(tv_big a, tv_big b);
bool tv_big_eq_slow(tv_big a, tv_big b);
static inline int tv_big_cmp(tv_big a, tv_big b) {
    if (__builtin_expect(a.v & b.v & 1, 1)) return (a.v > b.v) - (a.v < b.v);
    return tv_big_cmp_slow(a, b);
}
static inline bool tv_big_eq(tv_big a, tv_big b) { return a.v == b.v || (!((a.v | b.v) & 1) && tv_big_eq_slow(a, b)); }
int tv_big_cmp_int(tv_big a, int64_t b);
int tv_big_cmp_f64(tv_big a, double b);
uint64_t tv_big_hash(tv_big a);

tv_big tv_big_lit(const char *text);                       /* a literal's value (immortal) */
bool tv_big_parse(tv_str s, tv_big *out);                   /* BigInt(string); false: SyntaxError */
bool tv_big_from_f64(double x, tv_big *out);                /* BigInt(number); false: not an integer */
double tv_big_to_f64(tv_big a);                             /* Number(x): nearest, ties to even */
bool tv_big_fits_int(tv_big a);
tv_big tv_big_from_u64_(uint64_t m);
uint64_t tv_big_to_u64(tv_big a, const char *loc);          /* traps outside 0 .. 2^64-1 */
tv_int tv_big_to_int(tv_big a, const char *loc);            /* traps beyond 64 bits */
void tv_sb_push_big(tv_sb *sb, tv_big a);
void tv_sb_push_big_radix(tv_sb *sb, tv_big a, int radix);
tv_str tv_big_to_string(tv_big a, tv_int radix, const char *loc);

/* ------------------------------------------------------------------ decimal (runtime/dec.c) */

/* coefficient × 10^-scale: a signed coefficient under 10^34 shifted left 8 bits, the scale
 * (0..34) in the low byte. Two words, so it's 8-byte aligned wherever it's stored. */
typedef struct { uint64_t lo, hi; } tv_dec;
extern const tv_type tv_type_dec;
#define TV_DEC_LIM ((__int128)10000000000000000ll * 1000000000000000000ll)
static inline __int128 tv_dec_coef(tv_dec d) { return (__int128)(((unsigned __int128)d.hi << 64) | d.lo) >> 8; }
static inline int tv_dec_scale(tv_dec d) { return (int)(d.lo & 0xFF); }
static inline tv_dec tv_dec_make(__int128 c, int s) {
    unsigned __int128 v = ((unsigned __int128)c << 8) | (unsigned)s;
    return (tv_dec){ (uint64_t)v, (uint64_t)(v >> 64) };
}
static inline tv_dec tv_dec_from_int(int64_t x) { return tv_dec_make(x, 0); }
tv_dec tv_dec_add_slow(tv_dec a, tv_dec b, int sign);
tv_dec tv_dec_mul_slow(tv_dec a, tv_dec b);
int tv_dec_cmp_slow(tv_dec a, tv_dec b);
static inline tv_dec tv_dec_add(tv_dec a, tv_dec b) {
    if (__builtin_expect(((a.lo ^ b.lo) & 0xFF) == 0, 1)) {
        __int128 c = tv_dec_coef(a) + tv_dec_coef(b);
        if (__builtin_expect(c < TV_DEC_LIM && c > -TV_DEC_LIM, 1)) return tv_dec_make(c, (int)(a.lo & 0xFF));
    }
    return tv_dec_add_slow(a, b, 1);
}
static inline tv_dec tv_dec_sub(tv_dec a, tv_dec b) {
    if (__builtin_expect(((a.lo ^ b.lo) & 0xFF) == 0, 1)) {
        __int128 c = tv_dec_coef(a) - tv_dec_coef(b);
        if (__builtin_expect(c < TV_DEC_LIM && c > -TV_DEC_LIM, 1)) return tv_dec_make(c, (int)(a.lo & 0xFF));
    }
    return tv_dec_add_slow(a, b, -1);
}
static inline tv_dec tv_dec_mul(tv_dec a, tv_dec b) {
    int s = (int)(a.lo & 0xFF) + (int)(b.lo & 0xFF);
    __int128 x = tv_dec_coef(a), y = tv_dec_coef(b);
    if (__builtin_expect(s <= 34 && x == (int64_t)x && y == (int64_t)y, 1)) {
        __int128 c = (__int128)(int64_t)x * (int64_t)y;
        if (__builtin_expect(c < TV_DEC_LIM && c > -TV_DEC_LIM, 1)) return tv_dec_make(c, s);
    }
    return tv_dec_mul_slow(a, b);
}
static inline int tv_dec_cmp(tv_dec a, tv_dec b) {
    if (__builtin_expect(((a.lo ^ b.lo) & 0xFF) == 0, 1)) {
        __int128 x = tv_dec_coef(a), y = tv_dec_coef(b);
        return (x > y) - (x < y);
    }
    return tv_dec_cmp_slow(a, b);
}
static inline bool tv_dec_eq(tv_dec a, tv_dec b) {
    if (((a.lo ^ b.lo) & 0xFF) == 0) return a.lo == b.lo && a.hi == b.hi;
    return tv_dec_cmp_slow(a, b) == 0;
}
static inline tv_dec tv_dec_neg(tv_dec a) { return tv_dec_make(-tv_dec_coef(a), tv_dec_scale(a)); }
static inline tv_dec tv_dec_abs(tv_dec a) { __int128 c = tv_dec_coef(a); return c < 0 ? tv_dec_make(-c, tv_dec_scale(a)) : a; }
tv_dec tv_dec_div(tv_dec a, tv_dec b, const char *loc);      /* 34 significant digits, half even */
tv_dec tv_dec_rem(tv_dec a, tv_dec b, const char *loc);
uint64_t tv_dec_hash(tv_dec a);
tv_dec tv_dec_round(tv_dec a, tv_int places, tv_str mode, const char *loc);
tv_dec tv_dec_round_slow(tv_dec a, tv_int places, int mode, const char *loc);
tv_dec tv_dec_div_round(tv_dec a, tv_dec b, tv_int places, int mode, const char *loc);  /* (a / b).round(places, mode) */
extern const uint64_t tv_p10[20];
/* round(places, mode): mode 0 halfEven, 1 halfExpand, 2 halfTrunc, 3 halfCeil, 4 halfFloor,
 * 5 ceil, 6 floor, 7 trunc, 8 expand. Inline for a coefficient in 64 bits and a constant mode. */
static inline tv_dec tv_dec_round_m(tv_dec a, tv_int places, int mode, const char *loc) {
    int s = tv_dec_scale(a);
    if (places >= s && places <= 34) return a;
    __int128 c = tv_dec_coef(a);
    if (__builtin_expect(places >= 0 && s - places <= 18 && c == (int64_t)c, 1)) {
        bool neg = c < 0;
        uint64_t ax = neg ? (uint64_t)0 - (uint64_t)(int64_t)c : (uint64_t)(int64_t)c, p = tv_p10[s - places];
        uint64_t q = ax / p, r = ax - q * p, r2 = 2 * r;
        bool up;
        switch (mode) {
        case 0: up = r2 > p || (r2 == p && (q & 1)); break;
        case 1: up = r2 >= p && r != 0; break;
        case 2: up = r2 > p; break;
        case 3: up = r2 > p || (r2 == p && !neg); break;
        case 4: up = r2 > p || (r2 == p && neg); break;
        case 5: up = r && !neg; break;
        case 6: up = r && neg; break;
        case 7: up = false; break;
        default: up = r != 0; break;
        }
        q += up;
        return tv_dec_make(neg ? -(__int128)q : (__int128)q, (int)places);
    }
    return tv_dec_round_slow(a, places, mode, loc);
}
tv_dec tv_dec_floor(tv_dec a);
tv_dec tv_dec_ceil(tv_dec a);
tv_dec tv_dec_trunc(tv_dec a);
void tv_sb_push_dec(tv_sb *sb, tv_dec a);
tv_str tv_dec_to_string(tv_dec a);
tv_str tv_dec_to_fixed(tv_dec a, tv_int digits, const char *loc);
int tv_dec_parse_n(const char *s, size_t n, tv_dec *out);   /* 0 ok, 1 not a number, 2 too large */
int tv_dec_parse(tv_str s, tv_dec *out);
tv_dec tv_dec_lit(const char *text);
bool tv_dec_from_f64(double x, tv_dec *out);
double tv_dec_to_f64(tv_dec a);
tv_dec tv_dec_from_big(tv_big b, const char *loc);
tv_int tv_dec_to_int(tv_dec a, const char *loc);

/* ------------------------------------------------------------------ math */

double tv_math_round(double x);           /* JS Math.round: half toward +infinity */
tv_int tv_f64_to_int(double x, const char *what, const char *loc);   /* traps on NaN/±inf/out of i64 range */
/* Math.random: xorshift64*, deterministic seed unless TOV_SEED is set. A program that draws
 * numbers calls tv_random_init as it starts (so programs that don't never read TOV_SEED);
 * tv_random is inline and checks nothing, so a loop drawing numbers keeps the state in a
 * register. The state is stored xor 0x9e3779b97f4a7c15 (zero-initialized: no data page). */
extern uint64_t tv_rng_state;
void tv_random_init(void);
static inline double tv_random(void) {
    uint64_t x = tv_rng_state ^ 0x9e3779b97f4a7c15ULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    tv_rng_state = x ^ 0x9e3779b97f4a7c15ULL;
    return (double)((x * 0x2545f4914f6cdd1dULL) >> 11) * 0x1.0p-53;
}

/* ------------------------------------------------------------------ closures and boxes */

typedef struct tv_env {
    int64_t rc;
    void (*drop)(struct tv_env *env);     /* releases captured values; runtime frees env after */
} tv_env;

typedef struct tv_fn {
    void *fn;          /* C function taking (tv_env *env, args...) */
    tv_env *env;       /* NULL when nothing is captured */
} tv_fn;

static inline void tv_env_retain(tv_env *e) { if (e && e->rc >= 0) e->rc++; }
void tv_env_release_slow(tv_env *e);
static inline void tv_env_release(tv_env *e) { if (e && e->rc > 0 && --e->rc == 0) tv_env_release_slow(e); }

/* ------------------------------------------------------------------ async (tasks, promises, timers)
 *
 * An async function compiles to a frame and a resume function that returns true once the frame
 * has finished. A task runs one root frame; awaited async calls are embedded in their caller's
 * frame, so resuming a task resumes its whole chain. Scheduling follows JavaScript exactly:
 * microtasks run in FIFO order after each macrotask (a timer callback, a request), and every
 * `await` costs one microtask tick. The one shortcut, `tv_async_eager`, is taken only when
 * running on immediately is indistinguishable from waiting a tick. Single-threaded. */

enum { TV_PENDING = 0, TV_FULFILLED = 1, TV_REJECTED = 2 };

typedef struct tv_task tv_task;

/* Something to run (as a microtask) when a promise settles. */
typedef struct tv_reaction { void (*fn)(void *a, void *b); void *a, *b; } tv_reaction;

typedef struct tv_promise {
    int64_t rc;
    uint8_t state;            /* TV_PENDING, TV_FULFILLED or TV_REJECTED */
    bool handled;             /* awaited: a rejection isn't reported as unhandled */
    bool reported;            /* on the unhandled-rejection list */
    const tv_type *vt;        /* the value's type */
    void *err;                /* rejection: an Error object (owned) */
    tv_task *waiter;          /* the task waiting for it, when that's the first and only reaction */
    tv_reaction *more;        /* other reactions, in the order they were added */
    int32_t nmore, capmore;
    _Alignas(16) unsigned char value[];
} tv_promise;

/* A task's `run` resumes its root frame; true once it has finished (and settled `promise`). */
typedef bool (*tv_task_run)(tv_task *t);

struct tv_task {
    tv_task *next;            /* microtask queue link */
    tv_task_run run;
    tv_promise *promise;      /* the task's result (the task holds one reference) */
    uint32_t flags;           /* TV_TASK_SYNC: its starter is still running (see tv_async_eager) */
    uint32_t size;            /* frame size */
    _Alignas(16) unsigned char frame[];
};
enum { TV_TASK_SYNC = 1u };

extern tv_task *tv_cur_task;  /* the task running now (NULL outside tasks) */

/* The program's error objects: released and printed through hooks the program installs. */
void tv_async_init(void (*err_retain)(void *), void (*err_release)(void *), void (*err_report)(void *));

tv_promise *tv_promise_new(const tv_type *vt);                 /* pending, rc 1 */
static inline void tv_promise_retain(tv_promise *p) { if (p) p->rc++; }
void tv_promise_release_slow(tv_promise *p);
static inline void tv_promise_release(tv_promise *p) { if (p && --p->rc == 0) tv_promise_release_slow(p); }
void tv_promise_resolve(tv_promise *p, const void *value);     /* copies and retains the value */
void tv_promise_resolve_move(tv_promise *p, void *value);      /* takes the value (no retain) */
void tv_promise_reject(tv_promise *p, void *err);              /* takes the error */
void tv_promise_on(tv_promise *p, void (*fn)(void *, void *), void *a, void *b);  /* fn(a, b) as a microtask once p settles */
static inline void *tv_promise_value(tv_promise *p) { return p->value; }
tv_env *tv_promise_resolver(tv_promise *p);                     /* env of a `resolve`/`reject` closure: holds p */
static inline tv_promise *tv_resolver_promise(tv_env *env) { return *(tv_promise **)(env + 1); }

tv_task *tv_task_new(size_t frame_size, tv_task_run run, const tv_type *vt);   /* zeroed frame */
static inline void *tv_task_frame(tv_task *t) { return t->frame; }
/* Starts a task (an async call that isn't awaited): runs it until it first waits, then returns
 * its promise (a new reference). */
tv_promise *tv_task_spawn(tv_task *t);
/* Starts a task from the event loop (the program's top level): it may continue eagerly. */
void tv_task_start(tv_task *t);

/* Continuing now is indistinguishable from waiting a tick: no microtask is queued, and the task
 * isn't being run synchronously by a starter that still has code to run (tv_cur_sync: nonzero
 * then, and outside tasks). Every `await` asks, so it's one test. */
extern size_t tv_mq_len;
extern size_t tv_cur_sync;
static inline bool tv_async_eager(void) { return (tv_mq_len | tv_cur_sync) == 0; }
/* `await p`: true if the value can be taken now — settled, and nothing else could run first.
 * Otherwise call tv_await_suspend and suspend: the task resumes once the value is ready. */
static inline bool tv_await_now(tv_promise *p) { p->handled = true; return p->state != TV_PENDING && tv_async_eager(); }
void tv_await_suspend(tv_promise *p);
/* After an embedded async call finished: suspend for the tick its promise would have taken. */
void tv_task_yield(void);

void tv_queue_microtask(tv_fn callback);        /* queueMicrotask(cb) (retains cb) */
/* Promise.all / Promise.race over an array of promises (borrowed): `et` describes their values;
 * `arr_t`, an array of them (all's result). A new promise (rc 1). */
tv_promise *tv_promise_all(struct tv_arr ps, const tv_type *et, const tv_type *arr_t);
tv_promise *tv_promise_race(struct tv_arr ps, const tv_type *et);
tv_int tv_set_timer(tv_fn callback, double ms, bool repeat);   /* setTimeout/setInterval (retains cb) */
/* I/O on the event loop: tv_io_add watches fd (level-triggered) and calls h->ready from the loop
 * when it's readable or writable; while tv_io_refs > 0 the loop keeps running. */
typedef struct tv_io tv_io;
struct tv_io { void (*ready)(tv_io *h, bool readable, bool writable, bool broken); };
extern int tv_io_refs;
void tv_io_add(int fd, tv_io *h, bool read, bool write);
/* changes what a watched fd waits for; neither stops watching it (closing the fd ends it too) */
void tv_io_set(int fd, tv_io *h, bool read, bool write);
/* calls h->ready once when process pid exits (then reap it with waitpid, and close *fd_out if
 * it isn't -1: the descriptor watched for it); false if it can't */
bool tv_io_proc(int pid, tv_io *h, int *fd_out);
/* Node.js's check phase (setImmediate) for npm code: set tv_loop_check_pending and the loop runs
 * tv_loop_check once, after timers and I/O, without blocking; tv_loop_check_ref: it keeps the
 * program running. */
extern void (*tv_loop_check)(void);
extern bool tv_loop_check_pending, tv_loop_check_ref;
/* The engine's own timers (JavaScriptCore sweeps and collects on its thread's run loop, which the
 * event loop stands in for): tv_loop_host_due(now) says when the next is due, in the loop's
 * milliseconds (0: none), and tv_loop_host_run runs those due. They never keep the program
 * running. */
extern uint64_t (*tv_loop_host_due)(uint64_t now_ms);
extern void (*tv_loop_host_run)(void);
/* Whether the engine has work underway on its threads that it hands back soon (WebAssembly
 * compiled off the thread, for a promise the program awaits): unlike its timers, it keeps the
 * program running. While it does, a descriptor is watched (tv_io) that its threads wake the loop
 * with when they hand the work back. */
extern bool (*tv_loop_host_busy)(void);
/* Called once the loop has been idle (no I/O, no timers firing) for a second after doing work,
 * and again (deep) after 2.5 s to 20 s (adapting to how soon work comes back after a drop: see
 * runtime/tov.c): the engine collects its garbage then and gives the memory back, and deep,
 * drops its compiled code too. The first idle after starting is deep. */
extern void (*tv_loop_idle)(bool deep);
/* Called just before the thread blocks (the loop waits, or sleeps): the engine gives back the
 * lock it keeps between calls (runtime/js.c). */
extern void (*tv_loop_will_block)(void);
void tv_clear_timer(tv_int id);
/* Runs the event loop until nothing is left: microtasks, timers and servers. */
void tv_async_run(void);

/* ------------------------------------------------------------------ tests */

/* Runs one test; a failing expect or a trap inside it marks it failed and returns.
 * Prints "ok   <name>" or "FAIL <name>\n  <message>\n  at <loc>". */
void tv_test_run(const char *name, void (*fn)(void));
/* A test named name[0, n) running fn(ctx) (it may run inside another: a `describe` suite's); a
 * suite reports only a failure outside its tests. */
void tv_test_run_ctx(const char *name, size_t n, void (*fn)(void *), void *ctx, bool suite);
void tv_test_call(void *fn);  /* calls fn, a `void (*)(void)` */
bool tv_test_active(void);   /* inside tv_test_run (a trap fails the test instead of exiting) */
_Noreturn void tv_expect_fail(tv_sb *message, const char *loc);   /* message: e.g. "expected 3, received 4"; takes (frees) the builder */
int tv_test_summary(void);   /* prints "<n> passed, <m> failed"; returns exit status (0 if all passed) */

/* Called by the generated main() before anything else. Stores argc/argv below. */
void tv_init(int argc, char **argv);
extern int tv_argc;
extern char **tv_argv;

/* ------------------------------------------------------------------ system (used by the standard library) */

/* Errors are reported through a per-process slot read by tv_native_takeError (Node-style messages:
 * "ENOENT: no such file or directory, open 'x'"). */
tv_str tv_native_takeError(void);
__attribute__((noreturn)) unsigned char tv_native_trap(tv_str msg); /* returns `never` (tv_unit) */
tv_str tv_native_readFile(tv_str path);
tv_str tv_native_statInfo(tv_str path);
void tv_native_access(tv_str path, tv_int mode);
void tv_native_writeFile(tv_str path, tv_str data, bool append);
bool tv_native_exists(tv_str path);
struct tv_arr tv_native_readDir(tv_str path);          /* names, sorted */
void tv_native_mkdir(tv_str path, bool recursive);
void tv_native_unlink(tv_str path);
void tv_native_rm(tv_str path, bool recursive, bool force);
tv_str tv_native_cwd(void);
double tv_native_dateNow(void);
double tv_native_dateOffset(double t);
tv_int tv_native_cryptoNew(tv_str alg, tv_arr key, bool hmac);     /* std/crypto: a hash or HMAC handle, -1: unknown */
void tv_native_cryptoUpdate(tv_int h, tv_str data);
void tv_native_cryptoUpdateBytes(tv_int h, tv_arr data);
tv_str tv_native_cryptoDigest(tv_int h, tv_str encoding);          /* frees the handle */
tv_arr tv_native_cryptoDigestBytes(tv_int h);
tv_str tv_native_bytesEncode(tv_arr bytes, tv_str encoding);       /* hex, base64, base64url, latin1 */
tv_arr tv_native_cryptoRandomBytes(tv_int n);
tv_str tv_native_cryptoRandomUUID(void);
bool tv_native_cryptoEqual(tv_arr a, tv_arr b);
tv_str tv_native_dateIso(double t);              /* toISOString of a valid time */
double tv_native_dateParseIso(tv_str s);         /* exactly toISOString's form, else NaN */
double tv_native_dateLocalToUtc(double local);
tv_str tv_native_dateZoneName(double t);
struct tv_arr tv_process_argv(void);                   /* ["tov-binary-path", "program", args...] like Node's */
bool tv_process_env(tv_str name, tv_str *out);
tv_int tv_process_pid(void);
_Noreturn void tv_process_exit(tv_int code);
double tv_date_now(void);                              /* ms since the epoch */
void tv_tz_changed(void);                               /* TZ was set: Date's offsets asked again */
#if defined(__APPLE__) && defined(__aarch64__)
/* ms since the program started: the system counter read here (what mach_absolute_time reads,
 * without the call), scaled by its frequency */
extern uint64_t tv_perf_start;
extern double tv_perf_ms_per_tick;
void tv_perf_init(uint64_t t);
static inline double tv_performance_now(void) {
    uint64_t t;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(t));
    if (__builtin_expect(tv_perf_ms_per_tick == 0, 0)) tv_perf_init(t);
    return (double)(t - tv_perf_start) * tv_perf_ms_per_tick;
}
#else
double tv_performance_now(void);                       /* ms since the program started */
#endif
void tv_write_stdout(tv_str s);
void tv_write_stderr(tv_str s);

/* ------------------------------------------------------------------ JSON */

void tv_json_quote(tv_sb *sb, tv_str s);               /* JSON.stringify of a string */
void tv_json_number(tv_sb *sb, double x);              /* JS number, or null for NaN/Infinity */
typedef struct tv_jp {
    const unsigned char *s;
    size_t n, i;
    char err[160];                                     /* first error ("" if none) */
} tv_jp;
void tv_jp_init(tv_jp *p, tv_str text);
char tv_jp_peek_slow(tv_jp *p);
bool tv_jp_char_slow(tv_jp *p, char c);
/* (inline for the byte that's next, as in JSON.stringify's output, the rest out of line: spaces) */
static inline char tv_jp_peek(tv_jp *p) {                /* next non-space byte, or 0 at the end */
    if (__builtin_expect(p->i < p->n && p->s[p->i] > ' ', 1)) return (char)p->s[p->i];
    return tv_jp_peek_slow(p);
}
static inline bool tv_jp_char(tv_jp *p, char c) {       /* consumes c (after spaces) or fails */
    if (__builtin_expect(p->i < p->n && p->s[p->i] == (unsigned char)c, 1)) { p->i++; return true; }
    return tv_jp_char_slow(p, c);
}
static inline bool tv_jp_try_char(tv_jp *p, char c) {   /* consumes c if it's next */
    if (tv_jp_peek(p) == c) { p->i++; return true; }
    return false;
}
bool tv_jp_word(tv_jp *p, const char *w);              /* null / true / false */
bool tv_jp_string(tv_jp *p, tv_str *out);
/* An object's key: the bytes in the input when it has no escapes (nothing allocated), else decoded
 * into `own`; tv_jp_key_done when it's been compared, or tv_jp_key_str for it as a string. */
typedef struct tv_jp_k { const char *s; size_t n; bool owned; tv_str own; } tv_jp_k;
bool tv_jp_key(tv_jp *p, tv_jp_k *k);
static inline void tv_jp_key_done(tv_jp_k *k) { if (k->owned) tv_str_release(k->own); }
static inline tv_str tv_jp_key_str(tv_jp_k *k) { return k->owned ? k->own : tv_str_from(k->s, k->n); }
bool tv_jp_number(tv_jp *p, double *out);
bool tv_jp_decimal(tv_jp *p, tv_dec *out);   /* a JSON number, exactly */
bool tv_jp_skip(tv_jp *p);                             /* any value */
bool tv_jp_end(tv_jp *p);                              /* only spaces left */
bool tv_jp_dyn(tv_jp *p, tv_dyn *out);                  /* any JSON value, as an `unknown` */
extern const struct tv_type tv_type_dynarr, tv_type_dynobj; /* (its arrays and objects: unknown[], Record<string, unknown>) */

/* `any` (see tov.c): conversions and operators, and what the runtime knows of strings and JSON's
 * arrays and objects (false/-1: not one of those; the program's own types are generated). */
double tv_any_num(tv_dyn d);                                   /* ToNumber */
tv_str tv_any_str(tv_dyn d);                                   /* String(d), owned */
tv_str tv_any_key(tv_dyn k);                                   /* a property key, owned */
tv_dyn tv_any_add(tv_dyn a, tv_dyn b);                         /* a + b, owned */
bool tv_any_lt(tv_dyn a, tv_dyn b, bool le);                   /* a < b, or a <= b */
bool tv_any_loose_eq(tv_dyn a, tv_dyn b);                      /* a == b */
_Noreturn void tv_any_nullish_trap(tv_dyn d, tv_str key, const char *loc);
_Noreturn void tv_any_type_trap(const char *want, tv_dyn d, const char *loc);
bool tv_any_get_rt(tv_dyn d, tv_str k, tv_dyn *out, const char *loc);
bool tv_any_at_rt(tv_dyn d, tv_int i, tv_dyn *out);
bool tv_any_set_rt(tv_dyn d, tv_str k, tv_dyn v, const char *loc);   /* v moved */
bool tv_any_set_at_rt(tv_dyn d, tv_int i, tv_dyn v);                  /* v moved */
bool tv_any_invoke_rt(tv_dyn d, tv_str k, int n, const tv_dyn *a, tv_dyn *out, const char *loc);
tv_int tv_any_len_rt(tv_dyn d);
bool tv_any_eq_raw(const tv_type *t, const void *p, tv_dyn x);    /* *p (a t) === x */
static inline bool tv_any_int_key(tv_dyn k, tv_int *i) {
    if (k.t == &tv_type_int) { *i = *(const tv_int *)tv_dyn_data(k); return true; }
    if (tv_dyn_is_num(k.t)) {
        double x = tv_dyn_number(k);
        if (x >= 0 && x < 9.0e15 && x == (double)(tv_int)x) { *i = (tv_int)x; return true; }
    }
    return false;
}
bool tv_jp_fail(tv_jp *p, const char *expected);       /* records "expected X at position N"; returns false */
bool tv_jp_find_key(tv_jp *p, const char *key, tv_str *val);  /* string value of key in the object at the cursor (not consumed) */
tv_str tv_jp_error(tv_jp *p);

/* ------------------------------------------------------------------ HTTP server (std/http) */

/* HTTP servers (std/http): httpListen binds and registers a server (returns its id, or -1 with
 * the error in tv_native_takeError); tv_http_run, called after the program, serves every
 * registered server — in httpWorkers processes — until all are stopped. For each request the
 * handler(env, method, target, headers, body) answers with tv_native_httpRespond. */
tv_int tv_native_httpListen(tv_int port, tv_str host, tv_fn handler);
tv_int tv_native_httpPort(tv_int id);
void tv_native_httpStop(tv_int id, bool force);
void tv_native_httpWorkers(tv_int n);
/* URLs: parse → [protocol, hostname, port, pathname, search, hash] (empty if invalid; `base`
 * resolves a relative input, "" for none); percent-decoding (`plus`: '+' is a space) and
 * application/x-www-form-urlencoded encoding. */
struct tv_arr tv_native_urlParse(tv_str input, tv_str base);
tv_str tv_native_requestUrl(tv_str headers, tv_str target);   /* "http://" + Host + target */
tv_str tv_native_urlNormalize(tv_str input, tv_str base);      /* normalized href, "" if invalid */
tv_str tv_native_urlPart(tv_str href, tv_int k);               /* 0 protocol .. 5 hash */
tv_str tv_native_urlDecode(tv_str s, bool plus);
tv_str tv_native_urlEncode(tv_str s);
void tv_native_httpRespond(tv_int status, tv_str headers, tv_str body, bool typed);  /* typed: add the default content-type */
/* An async handler: httpDefer (while the request is being handled) returns an id; the response
 * comes later through httpRespondTo. Responses on a connection go out in request order. */
tv_int tv_native_httpDefer(void);
/* The next task started (an async call not awaited) is in tail position: the caller does nothing
 * observable before the next microtask checkpoint, so the task may continue eagerly. */
void tv_native_spawnTail(void);
void tv_native_httpRespondTo(tv_int id, tv_int status, tv_str headers, tv_str body, bool typed);
/* For Node.js's http on these servers (runtime/node.c): raw response bytes for a deferred request
 * (end: 0 more to come, 1 done, 2 done and close), its connection's fd, the current request's
 * HTTP version, and taking its connection over as a raw socket (an upgrade). */
void tv_native_httpWriteRaw(tv_int id, const char *p, size_t n, int end);
int tv_native_httpFd(tv_int id);
extern void (*tv_http_on_gone)(tv_int id);   /* a deferred request's client went away (see tov.c) */
/* bytes of deferred request id's response not yet taken by the socket (its own, and what's
 * queued ahead of it on the connection), or -1 if the client has gone */
int64_t tv_native_httpBuffered(tv_int id);
int tv_native_httpTakeover(tv_int id, tv_sb *rest);
void tv_native_httpNodeErrors(tv_int id);   /* malformed requests answered in Node.js's form */
extern bool tv_http_v10;
tv_int tv_native_headerIndex(tv_str block, tv_str name);   /* offset of the value, or -1 */
tv_str tv_native_headerValue(tv_str block, tv_int at);
void tv_http_run(void);   /* after the program: serves registered servers until stopped (no-op if none) */
tv_str tv_native_headerRemove(tv_str block, tv_str name);  /* the block without `name` lines */
tv_str tv_native_headerAppend(tv_str block, tv_str name, tv_str value);
/* fetch(): see the HTTP client section of tov.c */
tv_int tv_native_fetchStart(tv_str method, tv_str url, tv_str headers, tv_str body, tv_int redirect, tv_int flags, tv_str ca, tv_str unix_path, tv_str proxy);
tv_promise *tv_native_fetchWait(tv_int id);    /* Promise<int>: 0 the head arrived, -1 failed, -2 aborted */
tv_promise *tv_native_fetchBodyWait(tv_int id);   /* ... 0 the whole body is in */
tv_promise *tv_native_fetchRead(tv_int id);    /* ... > 0 bytes to take, 0 the end */
tv_arr tv_native_fetchTake(tv_int id);
void tv_native_fetchShare(tv_int id);
tv_promise *tv_native_fetchHandle(tv_int id);  /* a Response's hold on the request */
tv_int tv_native_fetchStatus(tv_int id);
tv_str tv_native_fetchStatusText(tv_int id);
tv_str tv_native_fetchHeaders(tv_int id);
tv_str tv_native_fetchUrl(tv_int id);
bool tv_native_fetchRedirected(tv_int id);
tv_str tv_native_fetchBody(tv_int id);         /* moves the body out */
bool tv_native_fetchBodyClean(tv_int id);      /* valid UTF-8, no BOM (call before fetchBody) */
tv_str tv_native_fetchErrorCode(tv_int id);
tv_str tv_native_fetchErrorMessage(tv_int id);
void tv_native_fetchAbort(tv_int id);
void tv_native_fetchFree(tv_int id);
const char *tv_native_fetchData(tv_int id, size_t *len);   /* the bytes that have arrived (NULL: none), then: */
void tv_native_fetchTaken(tv_int id);                      /* they've been read */
extern void (*tv_fetch_on_bytes)(size_t n);   /* response bytes held unread (the host's GC accounting) */
extern bool tv_fetch_offload;   /* set by a busy host: connections are closed, and spares opened, on a thread of their own */
void tv_native_timerUnref(tv_int id);

/* TLS for fetch(): runtime/tls.c, compiled with the vendored mbedTLS into an archive that only
 * programs calling fetch() link. Their code calls tv_tls_install(), which sets tv_tls_impl;
 * until then (or in other programs) it is NULL. */
typedef struct tv_tls tv_tls;
typedef struct tv_tls_ops {
    /* A client for `host` on connected socket fd; `key` names the origin (sessions resume per key). */
    tv_tls *(*open)(int fd, const char *host, const char *key, bool verify, const char *ca, size_t ca_len);
    int (*handshake)(tv_tls *t);                          /* 0 done, 1 wants read, 2 wants write, -1 failed */
    long (*read)(tv_tls *t, void *buf, size_t n);         /* > 0 bytes, 0 ended, -1 wants read, -2 write, -3 failed */
    long (*write)(tv_tls *t, const void *buf, size_t n);  /* as read */
    size_t (*pending)(tv_tls *t);                         /* decrypted bytes waiting to be read */
    void (*why)(tv_tls *t, const char *url, const char **code, char *msg, size_t n);  /* (url NULL: not a fetch) */
    void (*close)(tv_tls *t, bool notify);
    /* A server context from PEM certificate chain and key (NULL and a message on failure), and a
     * server-side session on an accepted socket. */
    void *(*server)(const char *cert, size_t cert_len, const char *key, size_t key_len, char *err, size_t err_n);
    tv_tls *(*accept)(int fd, void *server);
    /* the peer's certificate checked out (a client with verification on) */
    bool (*verified)(tv_tls *t);
} tv_tls_ops;
extern const tv_tls_ops *tv_tls_impl;
/* Streaming body decoders (runtime/codecs.c, in the same archive): set by tv_tls_install too.
 * enc: 1 gzip, 2 deflate, 3 br, 4 zstd. step decodes in[*used, n) onto out, at most `limit`
 * new bytes (0: no limit), and advances *used: 1 the input so far is a complete stream, 0 more
 * is needed (input, or room), -1 corrupt. */
typedef struct tv_decoder tv_decoder;
typedef struct tv_codec_ops {
    tv_decoder *(*open)(int enc);
    int (*step)(tv_decoder *d, const uint8_t *in, size_t n, size_t *used, tv_sb *out, size_t limit);
    void (*close)(tv_decoder *d);
    bool (*whole)(int enc, const uint8_t *in, size_t n, tv_sb *out); /* all at once; false if corrupt */
} tv_codec_ops;
extern const tv_codec_ops *tv_codec;
extern const tv_codec_ops tv_codecs;
/* Hashes, HMAC, random bytes and key derivation (runtime/crypto.c, in the same archive), for npm
 * packages' `crypto`: set by tv_crypto_install (which links only BoringSSL's libcrypto, not the
 * TLS client), and by tv_tls_install too. Digests go by Node.js's names ("sha256",
 * "SHA-256", "RSA-SHA256"); *_new return NULL for an unknown one. *_final write up to 64 bytes. */
typedef struct tv_crypto_ops {
    int (*digest_size)(const char *name);   /* -1: unknown */
    void *(*hash_new)(const char *name);
    void *(*hash_copy)(void *h);
    void (*hash_update)(void *h, const uint8_t *p, size_t n);
    size_t (*hash_final)(void *h, uint8_t *out);
    void (*hash_free)(void *h);
    void *(*hmac_new)(const char *name, const uint8_t *key, size_t n);
    void (*hmac_update)(void *h, const uint8_t *p, size_t n);
    size_t (*hmac_final)(void *h, uint8_t *out);
    void (*hmac_free)(void *h);
    void (*random)(uint8_t *out, size_t n);
    bool (*pbkdf2)(const char *digest, const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen, uint32_t iter, uint8_t *out, size_t outlen);
    bool (*scrypt)(const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen, uint64_t N, uint64_t r, uint64_t p, size_t maxmem, uint8_t *out, size_t outlen);
    bool (*hkdf)(const char *digest, const uint8_t *key, size_t klen, const uint8_t *salt, size_t slen, const uint8_t *info, size_t ilen, uint8_t *out, size_t outlen);
    bool (*equal)(const uint8_t *a, const uint8_t *b, size_t n);   /* constant time */
} tv_crypto_ops;
extern const tv_crypto_ops *tv_crypto;
extern const tv_crypto_ops tv_crypto_table;
void tv_crypto_install(void);
/* Compression streams for npm packages' `zlib` (runtime/compress.c, in the same archive): set by
 * tv_zlib_install. A stream is opened in one of Node.js's modes (1 deflate, 2 inflate, 3 gzip,
 * 4 gunzip, 5 deflate raw, 6 inflate raw, 7 unzip, 8 brotli decode, 9 brotli encode, 10 zstd
 * compress, 11 zstd decompress), initialized for its kind, then written: each write runs the
 * codec over in into out with the flush value, reports what's left of each, and check() then
 * says whether it failed, with Node.js's message and code. A failed init, params or reset
 * returns the error (message NULL: none). */
typedef struct tv_zstream tv_zstream;
typedef struct tv_zs_error { const char *message; const char *code; int err; } tv_zs_error;
typedef struct tv_zs_ops {
    tv_zstream *(*open)(int mode);
    void (*init_zlib)(tv_zstream *z, int window_bits, int level, int mem_level, int strategy, const uint8_t *dict, size_t n);
    tv_zs_error (*init_brotli)(tv_zstream *z, const uint32_t *params, size_t n, const uint8_t *dict, size_t dict_len);
    tv_zs_error (*init_zstd)(tv_zstream *z, const uint32_t *params, size_t n, uint64_t pledged, const uint8_t *dict, size_t dict_len);
    void (*write)(tv_zstream *z, int flush, const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len, uint32_t *avail_in, uint32_t *avail_out);
    tv_zs_error (*check)(tv_zstream *z);
    tv_zs_error (*params)(tv_zstream *z, int level, int strategy);
    tv_zs_error (*reset)(tv_zstream *z);
    void (*close)(tv_zstream *z);
    uint32_t (*crc32)(uint32_t crc, const uint8_t *p, size_t n);
} tv_zs_ops;
extern const tv_zs_ops *tv_zs;
void tv_zlib_install(void);
void tv_tls_install(void);
tv_str tv_native_bytesToString(tv_arr bytes);
tv_arr tv_native_stringToBytes(tv_str s);
tv_str tv_native_utf8Clean(tv_str s);
tv_int tv_native_utf8Complete(tv_arr bytes);
tv_str tv_native_byteSlice(tv_str s, tv_int start, tv_int end);
tv_str tv_native_mimeLower(tv_str s);
tv_arr tv_native_multipartParse(tv_str body, tv_str boundary);
tv_str tv_native_headerGet(tv_str block, tv_str name);        /* values joined with ", " */
tv_arr tv_native_headerEntries(tv_str block);                  /* sorted [name, value, ...] */
tv_arr tv_native_headerValues(tv_str block, tv_str name);
/* sockets (runtime/net.c): Bun.connect, Bun.listen */
tv_int tv_native_netConnect(tv_str host, tv_int port, tv_int tls, bool verify, tv_str ca, tv_str unix_path);
void tv_native_netBind(tv_int id, tv_fn events, tv_str host, tv_str unix_path);
tv_int tv_native_netWrite(tv_int id, tv_arr bytes);
tv_int tv_native_netWriteText(tv_int id, tv_str text);
void tv_native_netEnd(tv_int id);
void tv_native_netFlush(tv_int id);
void tv_native_netClose(tv_int id);
void tv_native_netRef(tv_int id, bool ref);
bool tv_native_netNoDelay(tv_int id, bool on);
void tv_native_netHalfOpen(tv_int id, bool on);
tv_str tv_native_netAddress(tv_int id, tv_int which);
tv_int tv_native_netBuffered(tv_int id);
tv_int tv_native_netListen(tv_str host, tv_int port, tv_str cert, tv_str key, tv_str unix_path, tv_fn on_accept);
tv_int tv_native_netListenPort(tv_int id);
void tv_native_netStop(tv_int id);
void tv_native_netListenRef(tv_int id, bool ref);
tv_str tv_native_netError(void);
/* WebSockets (runtime/ws.c): Bun.serve's websocket and the WebSocket client */
tv_int tv_native_wsUpgrade(tv_str key, tv_str headers, tv_int max_payload);
tv_int tv_native_wsConnect(tv_str host, tv_int port, tv_str path, bool tls, bool verify, tv_str ca, tv_str headers, tv_str protocols, tv_int max_payload);
tv_int tv_native_wsSendText(tv_int id, tv_str text, tv_int op);
tv_int tv_native_wsSendBytes(tv_int id, tv_arr bytes, tv_int op);
void tv_native_wsClose(tv_int id, tv_int code, tv_str reason, bool now);
void tv_native_wsSubscribe(tv_int id, tv_str topic, bool on);
bool tv_native_wsSubscribed(tv_int id, tv_str topic);
tv_arr tv_native_wsTopics(tv_int id);
tv_int tv_native_wsPublishText(tv_str topic, tv_str text, tv_int except);
tv_int tv_native_wsPublishBytes(tv_str topic, tv_arr bytes, tv_int except);
tv_int tv_native_wsSubscribers(tv_str topic);
tv_str tv_native_wsProtocol(tv_int id);          /* WHATWG UTF-8 decode (BOM dropped, U+FFFD for bad bytes) */

#endif /* TOV_H */
