/* tov.h — the runtime interface used by C code generated from Tov.
 *
 * Conventions
 * - Single-threaded; reference counts are non-atomic.
 * - Heap values (strings, arrays, maps, boxes, closure environments) start with an
 *   `int64_t rc`. rc < 0 means immortal (static literals): retain/release ignore it.
 * - Values are *owned* (+1) or *borrowed* (+0). Unless a comment says otherwise:
 *   arguments are borrowed, return values are owned, and `void *elem`/`key`/`value`
 *   inputs to container "insert" operations are MOVED (the container takes ownership).
 * - Containers are copy-on-write: mutating operations call *_make_unique first, which
 *   clones the buffer when rc > 1 (retaining the elements it copies).
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

/* ------------------------------------------------------------------ traps */

_Noreturn void tv_trap(const char *msg, const char *loc);
void *tv_alloc(size_t size);          /* traps on out-of-memory */
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

void tv_sb_push(tv_sb *sb, const char *s, size_t n);
void tv_sb_grow(tv_sb *sb, size_t need);          /* capacity >= need bytes */
/* Inline fast paths for generated code. */
static inline void tv_sb_add(tv_sb *sb, const char *s, size_t n) {
    if (__builtin_expect(sb->cap - sb->len < n, 0)) tv_sb_grow(sb, sb->len + n);
    memcpy(sb->data + sb->len, s, n);
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

/* Descriptors for primitives. */
extern const tv_type tv_type_int, tv_type_f64, tv_type_f32, tv_type_bool, tv_type_str, tv_type_undefined;
extern const tv_type tv_type_promise;   /* every Promise<T> (a tv_promise *): Node's `Promise { ... }` */
extern const tv_type tv_type_i8, tv_type_i16, tv_type_i32, tv_type_u8, tv_type_u16, tv_type_u32, tv_type_u64;

/* ------------------------------------------------------------------ strings (immutable UTF-8) */

typedef struct tv_strbuf {
    int32_t rc;        /* < 0: immortal (literals) */
    int32_t len;       /* bytes, excluding the trailing NUL (strings are < 2 GiB) */
    char data[];       /* NUL-terminated */
} tv_strbuf;

typedef struct tv_str {
    tv_strbuf *p;      /* never NULL; the empty string is tv_empty_str */
} tv_str;

extern const tv_strbuf tv_empty_strbuf;
#define TV_EMPTY_STR ((tv_str){(tv_strbuf *)&tv_empty_strbuf})

/* A static literal: TV_STR_LIT(name, "text") declares `name` usable as TV_LIT(name). Immortal
 * buffers are never written, so they're const (read-only pages, nothing in __data). */
#define TV_STR_LIT(name, text) \
    static const struct { int32_t rc; int32_t len; char data[sizeof(text)]; } name = { -1, sizeof(text) - 1, text }
#define TV_LIT(name) ((tv_str){(tv_strbuf *)&(name)})

static inline void tv_str_retain(tv_str s) { if (s.p->rc >= 0) s.p->rc++; }
void tv_str_release_slow(tv_str s);
static inline void tv_str_release(tv_str s) { if (s.p->rc > 0 && --s.p->rc == 0) tv_str_release_slow(s); }

tv_str tv_str_from(const char *bytes, size_t n);
tv_str tv_str_from_sb(tv_sb *sb);               /* takes the builder's contents; sb is reset */
tv_str tv_str_concat(tv_str a, tv_str b);
bool tv_str_eq(tv_str a, tv_str b);
int tv_str_cmp(tv_str a, tv_str b);             /* byte order == code point order */
uint64_t tv_str_hash(tv_str s);
static inline tv_int tv_str_byte_len(tv_str s) { return s.p->len; }

/* JavaScript-compatible conversions */
tv_str tv_str_from_int(tv_int v);
tv_str tv_str_from_f64(double v);
tv_str tv_int_to_radix(tv_int v, tv_int radix, const char *loc);   /* (255).toString(16) → "ff"; traps outside 2..36 */
tv_str tv_f64_to_radix(double v, tv_int radix, const char *loc);   /* as Number.prototype.toString(radix), fractions included */               /* Number.prototype.toString(): 1 → "1", 0.1+0.2 → "0.30000000000000004", 1e21 → "1e+21" */
tv_str tv_str_from_bool(bool v);
tv_str tv_f64_to_fixed(double v, tv_int digits, const char *loc); /* JS toFixed (round half up on exact ties); traps if digits ∉ 0..100 */
tv_str tv_int_to_fixed(tv_int v, tv_int digits, const char *loc);
void tv_sb_push_int(tv_sb *sb, tv_int v);
void tv_sb_push_f64(tv_sb *sb, double v);       /* same format as tv_str_from_f64 */
void tv_sb_push_str(tv_sb *sb, tv_str s);

/* Parsing: returns false when the whole string (after trimming whitespace) isn't a number. */
bool tv_parse_float(tv_str s, double *out);     /* JS Number(s)/parseFloat subset: decimal, exponent, Infinity */
bool tv_parse_int(tv_str s, tv_int radix, tv_int *out);  /* radix 0 = 10 (or 16 with 0x); JS parseInt accepts a numeric prefix */

/* String methods (JavaScript semantics, with indices in BYTES; slice positions that split
 * a UTF-8 character trap). Negative indices count from the end, as in JS slice. */
struct tv_arr;
struct tv_arr tv_str_chars(tv_str s);           /* one string per Unicode scalar value */
tv_int tv_str_char_count(tv_str s);
tv_str tv_str_slice(tv_str s, tv_int start, tv_int end, bool has_end, const char *loc);
bool tv_str_includes(tv_str s, tv_str needle);
bool tv_str_starts_with(tv_str s, tv_str prefix);
bool tv_str_ends_with(tv_str s, tv_str suffix);
tv_int tv_str_index_of(tv_str s, tv_str needle);            /* byte index or -1 */
struct tv_arr tv_str_split(tv_str s, tv_str sep);           /* "" separator splits into characters */
tv_str tv_str_trim(tv_str s);
tv_str tv_str_trim_start(tv_str s);
tv_str tv_str_trim_end(tv_str s);
tv_str tv_str_to_upper(tv_str s);                           /* ASCII letters only in M1 */
tv_str tv_str_to_lower(tv_str s);
tv_str tv_str_replace(tv_str s, tv_str search, tv_str replacement);      /* first occurrence */
tv_str tv_str_replace_all(tv_str s, tv_str search, tv_str replacement);
tv_str tv_str_repeat(tv_str s, tv_int count, const char *loc);         /* traps on count < 0 */
tv_str tv_str_pad_start(tv_str s, tv_int len, tv_str fill);            /* len in characters */
tv_str tv_str_pad_end(tv_str s, tv_int len, tv_str fill);

/* ------------------------------------------------------------------ arrays (copy-on-write) */

/* Representation: the length lives in the value (so the C compiler can see it, e.g. a
 * constant length after inlining); the heap buffer holds rc, capacity and the elements.
 *
 * Sharing invariant: a buffer is shared (rc > 1) only through tv_arr_retain, i.e. by exact
 * copies of one value, and every mutation goes through tv_arr_make_unique (or a function that
 * calls it) before it changes elements or len. Hence all values sharing a buffer have the same
 * len, and the buffer owns exactly the elements [0, len) of any of them: tv_arr_release_slow
 * releases a.len elements. Elements at index >= len of a unique buffer are not owned (e.g.
 * after pop). A buffer with rc < 0 is immortal (a static literal) and is never mutated or
 * freed. Functions returning a tv_arr return either a fresh buffer (rc 1) or a retained copy
 * of an argument value (same len), which preserves the invariant. */
typedef struct tv_arrbuf {
    int64_t rc;
    int64_t cap;
    int64_t _pad[2];            /* keeps data 16-byte aligned (header stays 32 bytes) */
    unsigned char data[];
} tv_arrbuf;

typedef struct tv_arr {
    tv_arrbuf *p;               /* NULL = no allocation (len must be 0) */
    int64_t len;
} tv_arr;

#define TV_EMPTY_ARR ((tv_arr){NULL, 0})

static inline tv_int tv_arr_len(tv_arr a) { return a.len; }
static inline void *tv_arr_data(tv_arr a) { return a.p ? (void *)a.p->data : NULL; }
static inline void tv_arr_retain(tv_arr a) { if (a.p && a.p->rc >= 0) a.p->rc++; }
void tv_arr_release_slow(tv_arr a, const tv_type *t);
static inline void tv_arr_release(tv_arr a, const tv_type *t) { if (a.p && a.p->rc > 0 && --a.p->rc == 0) tv_arr_release_slow(a, t); }

/* Ensures *a is uniquely owned (rc == 1) with capacity >= min_cap (clones the value's len
 * elements, retaining them, when the buffer is shared or immortal). */
void tv_arr_make_unique(tv_arr *a, const tv_type *t, tv_int min_cap);
tv_arr tv_arr_with_capacity(const tv_type *t, tv_int cap);   /* len 0 */
/* Reserve capacity for exactly n more elements and return a pointer to where element a->len goes
   (buffer made unique; caller writes elements and then adds to a->len). */
void *tv_arr_reserve_tail(tv_arr *a, const tv_type *t, tv_int n);

/* Index checks: trap with "index <i> out of bounds for length <n>". */
_Noreturn void tv_arr_oob(tv_int i, tv_int len, const char *loc);
static inline void *tv_arr_at(tv_arr a, size_t elem_size, tv_int i, const char *loc) {
    if ((uint64_t)i >= (uint64_t)a.len) tv_arr_oob(i, a.len, loc);
    return a.p->data + (size_t)i * elem_size;
}
/* Mutable element access: makes the array unique first. */
void *tv_arr_at_mut(tv_arr *a, const tv_type *t, tv_int i, const char *loc);

void tv_arr_push(tv_arr *a, const tv_type *t, void *elem);         /* moves elem */
static inline void tv_arr_push_fast(tv_arr *a, const tv_type *t, void *elem) {
    if (a->p && a->p->rc == 1 && a->len < a->p->cap) {
        memcpy(a->p->data + (size_t)a->len * t->size, elem, t->size);
        a->len++;
    } else {
        tv_arr_push(a, t, elem);
    }
}
bool tv_arr_pop(tv_arr *a, const tv_type *t, void *out);          /* false if empty; out receives ownership */
bool tv_arr_shift(tv_arr *a, const tv_type *t, void *out);
void tv_arr_unshift(tv_arr *a, const tv_type *t, void *elem);      /* moves elem */
tv_arr tv_arr_slice(tv_arr a, const tv_type *t, tv_int start, tv_int end, bool has_start, bool has_end); /* JS semantics incl. negatives */
tv_arr tv_arr_concat(tv_arr a, tv_arr b, const tv_type *t);
void tv_arr_reverse(tv_arr *a, const tv_type *t);
/* Stable sort. cmp returns <0, 0, >0 like a JS comparator (a double). NULL cmp = JS default
 * (compare String(x) of each element). */
void tv_arr_sort(tv_arr *a, const tv_type *t, double (*cmp)(void *ctx, const void *x, const void *y), void *ctx);
tv_int tv_arr_index_of(tv_arr a, const tv_type *t, const void *elem);
tv_int tv_arr_last_index_of(tv_arr a, const tv_type *t, const void *elem);
tv_str tv_arr_join(tv_arr a, const tv_type *t, tv_str sep);       /* elements via t->to_str; "undefined" members print as "" (JS) */

/* ------------------------------------------------------------------ maps and sets (insertion-ordered, copy-on-write) */

typedef struct tv_mapbuf tv_mapbuf;   /* private layout */
typedef struct tv_map {
    tv_mapbuf *p;               /* NULL = empty map */
} tv_map;

#define TV_EMPTY_MAP ((tv_map){NULL})

void tv_map_retain(tv_map m);
void tv_map_release(tv_map m, const tv_type *kt, const tv_type *vt);
tv_int tv_map_size(tv_map m);
/* Borrowed pointer to the value for key, or NULL. Valid until the map is next mutated. */
void *tv_map_get(tv_map m, const tv_type *kt, const tv_type *vt, const void *key);
void tv_map_set(tv_map *m, const tv_type *kt, const tv_type *vt, void *key, void *value);   /* moves key and value; replaces (and releases) an existing value, keeps insertion position */
bool tv_map_has(tv_map m, const tv_type *kt, const tv_type *vt, const void *key);
bool tv_map_delete(tv_map *m, const tv_type *kt, const tv_type *vt, const void *key);
void tv_map_clear(tv_map *m, const tv_type *kt, const tv_type *vt);
tv_arr tv_map_keys(tv_map m, const tv_type *kt, const tv_type *vt);     /* insertion order, retained copies */
tv_arr tv_map_values(tv_map m, const tv_type *kt, const tv_type *vt);
/* Iteration in insertion order: for (i = 0; tv_map_next(m, kt, vt, &i, &k, &v);) — borrowed k/v. */
bool tv_map_next(tv_map m, const tv_type *kt, const tv_type *vt, tv_int *cursor, void **key, void **value);
/* Descriptor helpers for maps used as values inside other containers. */
bool tv_map_eq(tv_map a, tv_map b, const tv_type *kt, const tv_type *vt);   /* identity-free value equality */
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
/* console.log formatting of a record: fields in declaration order. */
void tv_inspect_record(tv_sb *sb, int depth, size_t n, const char *const *names, const tv_type *const *types, const void *const *fields);
/* A class instance: `Name { a: 1 }` (`[Name]` past the depth limit). */
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
double tv_random(void);                   /* xorshift, deterministic seed unless TOV_SEED is set */

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
struct tv_arr tv_process_argv(void);                   /* ["tov-binary-path", "program", args...] like Node's */
bool tv_process_env(tv_str name, tv_str *out);
tv_int tv_process_pid(void);
_Noreturn void tv_process_exit(tv_int code);
double tv_date_now(void);                              /* ms since the epoch */
double tv_performance_now(void);                       /* ms since the program started */
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
char tv_jp_peek(tv_jp *p);                             /* next non-space byte, or 0 at the end */
bool tv_jp_char(tv_jp *p, char c);                     /* consumes c (after spaces) or fails */
bool tv_jp_try_char(tv_jp *p, char c);                 /* consumes c if it's next */
bool tv_jp_word(tv_jp *p, const char *w);              /* null / true / false */
bool tv_jp_string(tv_jp *p, tv_str *out);
bool tv_jp_number(tv_jp *p, double *out);
bool tv_jp_decimal(tv_jp *p, tv_dec *out);   /* a JSON number, exactly */
bool tv_jp_skip(tv_jp *p);                             /* any value */
bool tv_jp_end(tv_jp *p);                              /* only spaces left */
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
extern void (*tv_fetch_on_bytes)(size_t n);   /* response bytes as they arrive (the host's GC accounting) */
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
