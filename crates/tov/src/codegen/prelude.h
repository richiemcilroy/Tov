
typedef uint8_t tv_unit;
/* String literals are real tv_strbuf objects (GNU static flexible-array initializer), so the
 * program can be compiled with strict aliasing. */
#define TVG_STR_LIT(name, text, kind) static const tv_strbuf name = { TV_STR_IMMORTAL | kind, sizeof(text) - 1, text }
#define TVG_LIT(name) ((tv_str){(tv_strbuf *)&(name)})
/* Boxes, cells and class instances come from the runtime's small-object free lists (sizes
 * known at every call site, 8-byte classes, see tv_small_bins). */
enum { TVG_CLASSES = TV_SMALL_CLASSES };
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
#define TVG_PLAIN_ALLOC 1 /* let AddressSanitizer see every allocation */
#endif
static inline void *tvg_alloc_small(size_t size) {
#ifdef TVG_PLAIN_ALLOC
    return tv_alloc((size + 7) & ~(size_t)7);
#endif
    size_t c = (size + 7) >> 3;
    if (__builtin_expect(c >= TVG_CLASSES, 0)) return tv_alloc(size);
    void **f = tv_small_bins[c];
    if (__builtin_expect(f != NULL, 1)) { tv_small_bins[c] = *f; return f; }
    return tv_small_refill(c);
}
/* JSON.stringify indentation: a newline and `lvl` copies of `ind` (nothing when compact). */
static __attribute__((noinline)) void tvg_js_nl_slow(tv_sb *sb, tv_str ind, int lvl) {
    tv_sb_push_char(sb, '\n');
    for (int i = 0; i < lvl; i++) tv_sb_push(sb, ind.p->data, (size_t)ind.p->len);
}
static inline void tvg_js_nl(tv_sb *sb, tv_str ind, int lvl) {
    if (__builtin_expect(ind.p->len != 0, 0)) tvg_js_nl_slow(sb, ind, lvl);
}
static tv_str tvg_js_spaces(tv_int n) {
    static const char sp[] = "          ";
    if (n <= 0) return TV_EMPTY_STR;
    return tv_str_from(sp, (size_t)(n > 10 ? 10 : n));
}
#if defined(__APPLE__)
#define TVG_PLATFORM "darwin"
#elif defined(_WIN32)
#define TVG_PLATFORM "win32"
#else
#define TVG_PLATFORM "linux"
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#define TVG_ARCH "arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#define TVG_ARCH "x64"
#else
#define TVG_ARCH "unknown"
#endif
/* The error being thrown (a class instance extending Error), or NULL. Calls that can throw are
 * followed by a check; `try` blocks jump to their handler, other code returns early. */
static void *tvg_err;
/* An async function waiting at an `await`: it returns "not finished" (see asyncfn.rs). */
#define TV_SUSPEND return false
/* Interface values: data (object, boxed record copy, or cell) + itab (retain/release/print, then
 * one thunk per member). */
typedef struct tvg_itab {
    void (*retain)(void *p);
    void (*release)(void *p);
    void (*inspect)(tv_sb *sb, void *p, int depth);
    void (*to_str)(tv_sb *sb, void *p);
} tvg_itab;
typedef struct tv_iface { void *p; const tvg_itab *t; } tv_iface;
static inline void tv_iface_retain(tv_iface v) { if (v.t) v.t->retain(v.p); }
static inline void tv_iface_release(tv_iface v) { if (v.t) v.t->release(v.p); }
/* Class instances: a 16-byte header, then fields (a subclass embeds its base first). */
/* Every class instance starts with an 8-byte header. Classes that weak references (or the cycle
 * collector) can reach also count weak references, in a slot right after the header (the root
 * of the hierarchy has it, so its offset is the same in every subclass). */
typedef struct tvg_obj { int32_t rc; uint16_t cid; uint16_t flags; } tvg_obj;
typedef struct tvg_objw { tvg_obj h; int32_t weak; int32_t pad_; } tvg_objw;
#define TVG_WEAK(p) (((tvg_objw *)(void *)(p))->weak)
static void tvg_obj_drop(void *p);
static void tvg_obj_free(void *p);
static void tvg_obj_inspect(tv_sb *sb, void *p, int depth);
#define tvg_obj_init(o, id) (*(tvg_obj *)(void *)(o) = (tvg_obj){1, (uint16_t)(id), 0})
static inline void tvg_obj_retain(void *p) { if (p) ((tvg_obj *)p)->rc++; }
/* A function value adapted to another function type: the environment holds the original. */
typedef struct tvg_fnbox { tv_env h; tv_fn f; } tvg_fnbox;
static void tvg_fnbox_drop(tv_env *e) { tv_env_release(((tvg_fnbox *)e)->f.env); }
/* `===` on functions: the same function, seen through the adapters that fit it to other types
 * (a function stored where one of fewer parameters is wanted is still itself). */
static inline tv_fn tvg_fn_unwrap(tv_fn f) { while (f.env && f.env->drop == tvg_fnbox_drop) f = ((tvg_fnbox *)f.env)->f; return f; }
static inline bool tvg_fn_eq(tv_fn a, tv_fn b) { a = tvg_fn_unwrap(a); b = tvg_fn_unwrap(b); return a.fn == b.fn && a.env == b.env; }
static inline void tvg_obj_release(void *p) { if (p && --((tvg_obj *)p)->rc == 0) tvg_obj_drop(p); }
static inline void tvg_weak_retain(void *p) { if (p) TVG_WEAK(p)++; }
static inline void tvg_weak_release(void *p) { if (p && --TVG_WEAK(p) == 0 && ((tvg_obj *)p)->rc <= 0) tvg_obj_free(p); }
/* A closure environment's `size` (see tv_env): its block's, when tvg_alloc_small took it from the
 * small-object free lists; 0 when from malloc (too big, or plain allocation) */
#ifdef TVG_PLAIN_ALLOC
#define TVG_ENV_SIZE(n) 0u
#else
#define TVG_ENV_SIZE(n) ((((n) + 7) >> 3) < TVG_CLASSES ? (uint32_t)(n) : 0u)
#endif
static inline void tvg_free_small(void *p, size_t size) {
#ifdef TVG_PLAIN_ALLOC
    tv_free(p); return;
#endif
    size_t c = (size + 7) >> 3;
    if (__builtin_expect(c >= TVG_CLASSES, 0)) { tv_free(p); return; }
    *(void **)p = tv_small_bins[c];
    tv_small_bins[c] = p;
}
/* ---- Cycle collector for `cyclic class` objects (only they pay for it) ----
 * Cyclic objects live in a list, with a 24-byte prefix before the header. A collection counts,
 * for each cyclic object, the references it gets from other cyclic objects; any object with more
 * references than that is referenced from outside (a variable, a temporary, an acyclic object)
 * and is live, as is everything it reaches. The rest is garbage: unreachable cycles.
 * Copy-on-write buffers (arrays, maps, boxes) reachable from cyclic objects are graph nodes too,
 * so a buffer shared by two objects isn't counted twice. */
typedef struct tvg_cyc { struct tvg_cyc *prev, *next; int64_t gc; } tvg_cyc;
enum { TVG_CYCLIC = 1, TVG_COLLECTING = 2, TVG_REACHED = 4 };
static tvg_cyc tvg_cyc_head = { &tvg_cyc_head, &tvg_cyc_head, 0 };
static int64_t tvg_cyc_count, tvg_cyc_since, tvg_cyc_limit = 10000, tvg_cyc_factor = 2;
static int tvg_gc_phase; /* 1: count internal references; 2: mark what's reachable */
#define TVG_CYC(p) ((tvg_cyc *)(void *)((char *)(p) - sizeof(tvg_cyc)))
#define TVG_OBJ(c) ((tvg_obj *)(void *)((char *)(c) + sizeof(tvg_cyc)))
static void tvg_cyc_collect(void);
static void tvg_obj_traverse(void *p);
static void tvg_obj_dropfields(void *p);
static inline void *tvg_cyc_alloc(size_t size) {
    if (__builtin_expect(++tvg_cyc_since > tvg_cyc_limit, 0)) tvg_cyc_collect();
    tvg_cyc *c = tvg_alloc_small(size + sizeof(tvg_cyc));
    c->gc = 0;
    c->next = tvg_cyc_head.next; c->prev = &tvg_cyc_head; tvg_cyc_head.next->prev = c; tvg_cyc_head.next = c;
    tvg_cyc_count++;
    return c + 1;
}
static inline void tvg_cyc_unlink(void *p) { tvg_cyc *c = TVG_CYC(p); c->prev->next = c->next; c->next->prev = c->prev; tvg_cyc_count--; }
static inline void tvg_cyc_free(void *p, size_t size) { tvg_free_small(TVG_CYC(p), size + sizeof(tvg_cyc)); }
typedef void (*tvg_gv_fn)(void *buf, int64_t len);
typedef struct { void *buf; int64_t internal; int64_t len; tvg_gv_fn fn; int reached; } tvg_gcb;
static tvg_gcb *tvg_gcb_tab; static size_t tvg_gcb_cap, tvg_gcb_n;
static void **tvg_gc_stack; static size_t tvg_gc_sn, tvg_gc_scap;
static struct tvg_cyc **tvg_gc_all; static size_t tvg_gc_allcap;
/* (the product's high bits, folded down: an address's low bits are its alignment's, the same
 * for every buffer of a size) */
static inline size_t tvg_gcb_hash(void *buf) { uint64_t z = (uint64_t)(uintptr_t)buf * 0x9E3779B97F4A7C15ull; return (size_t)(z ^ (z >> 32)); }
static tvg_gcb *tvg_gcb_find(void *buf, int *fresh) {
    if ((tvg_gcb_n + 1) * 2 > tvg_gcb_cap) {
        size_t oc = tvg_gcb_cap, nc = oc ? oc * 2 : 1024;
        tvg_gcb *old = tvg_gcb_tab;
        tvg_gcb_tab = calloc(nc, sizeof(tvg_gcb));
        if (!tvg_gcb_tab) tv_trap("out of memory", "cycle collector");
        tvg_gcb_cap = nc;
        for (size_t i = 0; i < oc; i++) if (old[i].buf) {
            size_t h = tvg_gcb_hash(old[i].buf) & (nc - 1);
            while (tvg_gcb_tab[h].buf) h = (h + 1) & (nc - 1);
            tvg_gcb_tab[h] = old[i];
        }
        free(old);
    }
    size_t h = tvg_gcb_hash(buf) & (tvg_gcb_cap - 1);
    while (tvg_gcb_tab[h].buf && tvg_gcb_tab[h].buf != buf) h = (h + 1) & (tvg_gcb_cap - 1);
    *fresh = tvg_gcb_tab[h].buf == NULL;
    if (*fresh) { tvg_gcb_tab[h].buf = buf; tvg_gcb_n++; }
    return &tvg_gcb_tab[h];
}
static void tvg_gc_push(void *p) {
    if (tvg_gc_sn == tvg_gc_scap) {
        tvg_gc_scap = tvg_gc_scap ? tvg_gc_scap * 2 : 1024;
        tvg_gc_stack = realloc(tvg_gc_stack, tvg_gc_scap * sizeof(void *));
        if (!tvg_gc_stack) tv_trap("out of memory", "cycle collector");
    }
    tvg_gc_stack[tvg_gc_sn++] = p;
}
/* A strong reference from a cyclic object (or one of its buffers) to a class instance. */
static inline void tvg_gc_obj(void *p) {
    if (!p) return;
    tvg_obj *h = p;
    if (!(h->flags & TVG_CYCLIC)) return;
    if (tvg_gc_phase == 1) TVG_CYC(p)->gc--;
    else if (!(h->flags & TVG_REACHED)) { h->flags |= TVG_REACHED; tvg_gc_push(p); }
}
/* A buffer reachable from a cyclic object: returns true when its elements should be visited now. */
static bool tvg_gc_buf(void *buf, int64_t len, tvg_gv_fn fn) {
    if (!buf || *(int64_t *)buf < 0) return false;
    /* (held only by what reached it, which is visited once a phase: no need to count it) */
    if (*(int64_t *)buf == 1) return true;
    int fresh;
    tvg_gcb *b = tvg_gcb_find(buf, &fresh);
    if (fresh) { b->len = len; b->fn = fn; }
    if (tvg_gc_phase == 1) { b->internal++; return fresh; }
    if (b->reached) return false;
    b->reached = 1;
    return true;
}
static void tvg_cyc_collect(void) {
    tvg_cyc_since = 0;
    if (tvg_gcb_n) memset(tvg_gcb_tab, 0, tvg_gcb_cap * sizeof(tvg_gcb));
    tvg_gcb_n = 0;
    /* (each object's count less the references from others: its own added as it's reached in
     * the walk, theirs taken off as they're walked, in any order; it's 0 between collections) */
    /* (the objects are listed in an array as the first pass walks them: the passes after it
     * read ahead of where they are, not one cache miss after another down the list) */
    int64_t walked = tvg_cyc_count, found = 0;
    if ((size_t)walked > tvg_gc_allcap) {
        free(tvg_gc_all);
        tvg_gc_allcap = (size_t)walked * 2;
        tvg_gc_all = malloc(tvg_gc_allcap * sizeof(tvg_cyc *));
        if (!tvg_gc_all) tv_trap("out of memory", "cycle collector");
    }
    tvg_cyc **all = tvg_gc_all;
    size_t n = 0;
    tvg_gc_phase = 1;
    for (tvg_cyc *c = tvg_cyc_head.next; c != &tvg_cyc_head; c = c->next) {
        __builtin_prefetch(c->next);
        all[n++] = c;
        c->gc += TVG_OBJ(c)->rc;
        tvg_obj_traverse(TVG_OBJ(c));
    }
    tvg_gc_phase = 2;
    tvg_gc_sn = 0;
    for (size_t i = 0; i < n; i++) {
        if (i + 8 < n) __builtin_prefetch(all[i + 8]);
        tvg_cyc *c = all[i];
        if (c->gc > 0) { TVG_OBJ(c)->flags |= TVG_REACHED; tvg_gc_push(TVG_OBJ(c)); }
    }
    for (size_t i = 0; tvg_gcb_n && i < tvg_gcb_cap; i++) {
        tvg_gcb *b = &tvg_gcb_tab[i];
        if (b->buf && !b->reached && *(int64_t *)b->buf > b->internal) { b->reached = 1; b->fn(b->buf, b->len); }
    }
    while (tvg_gc_sn) {
        void *p = tvg_gc_stack[--tvg_gc_sn];
        tvg_obj_traverse(p);
    }
    tvg_gc_phase = 0;
    /* Garbage: not reached. Taken out of the list (what's reached is linked again, in order),
     * pinned while fields are released (freeing what only it referenced; its own count reaching
     * zero is ignored while it's being collected), then freed. */
    tvg_cyc *prev = &tvg_cyc_head;
    size_t g = 0;
    for (size_t i = 0; i < n; i++) {
        if (i + 8 < n) __builtin_prefetch(all[i + 8], 1);
        tvg_cyc *c = all[i];
        tvg_obj *h = TVG_OBJ(c);
        if (h->flags & TVG_REACHED) { h->flags &= ~TVG_REACHED; c->gc = 0; prev->next = c; c->prev = prev; prev = c; continue; }
        h->flags |= TVG_COLLECTING;
        TVG_WEAK(h)++;
        all[g++] = c;
    }
    prev->next = &tvg_cyc_head; tvg_cyc_head.prev = prev;
    found = (int64_t)g;
    tvg_cyc_count -= found;
    for (size_t i = 0; i < g; i++) {
        if (i + 8 < g) __builtin_prefetch(all[i + 8]);
        tvg_obj_dropfields(TVG_OBJ(all[i]));
    }
    for (size_t i = 0; i < g; i++) {
        if (i + 8 < g) __builtin_prefetch(all[i + 8], 1);
        tvg_obj *h = TVG_OBJ(all[i]);
        h->rc = 0;
        h->flags &= ~TVG_COLLECTING;
        if (--TVG_WEAK(h) == 0) tvg_obj_free(h);
    }
    /* (the next one after as many allocations as are live, times a factor that grows fourfold,
     * to 64, each time a collection finds little (less than half of what it walked): a
     * structure that mostly lives on isn't walked again and again; but never more than 2M
     * allocations past twice the live objects, so the garbage that a later phase makes is still
     * found) */
    tvg_cyc_factor = found * 2 >= walked && found ? 8 : (tvg_cyc_factor < 64 ? tvg_cyc_factor * 4 : 64);
    int64_t live = tvg_cyc_count * tvg_cyc_factor, most = tvg_cyc_count * 8 + ((int64_t)1 << 22);
    if (live > most) live = most;
    tvg_cyc_limit = live > 10000 ? live : 10000;
}
/* Slow paths take arrays by value and return the updated value, so the array itself never
 * escapes: the C compiler keeps the pointer to its header in a register. */
static __attribute__((noinline)) tv_arr tvg_push_slow(tv_arr a, const tv_type *t, void *elem) { tv_arr_push(&a, t, elem); return a; }
static __attribute__((noinline)) tv_arrbuf *tvg_clone_buf(tv_arr a, const tv_type *t) { tv_arr_make_unique(&a, t, 0); return a.p; }
/* Capacity for `n` elements, by value (taking the array's address would keep it in memory). */
static __attribute__((noinline)) tv_arr tvg_reserve(tv_arr a, const tv_type *t, tv_int n) { tv_arr_make_unique(&a, t, n); return a; }
/* Makes the place's array one that can be changed (its own, in place of the immortal empty
 * array a place starts with when no value was given) without changing the length. */
static inline void tvg_make_unique(tv_arr *a, const tv_type *t) {
    /* (in a program the only such array is the empty one: a new empty array replaces it, so the
     * old pointer is never passed anywhere, and the C compiler can keep tracking what it knows
     * about the array) */
    (void)t;
    if (__builtin_expect(!a->p || a->p->rc < 0, 0)) a->p = tv_arr_fresh().p;
}
/* Typed push: inline fast path when there's spare capacity. */
#define TVG_PUSH(T, arr, desc, val) do { tv_arr *bmp_a = (arr); T bmp_v = (val); tv_arrbuf *bmp_p = bmp_a->p; \
    if (__builtin_expect(bmp_p->rc > 0 && bmp_p->len < bmp_p->cap, 1)) ((T *)(void *)tv_arrbuf_data(bmp_p))[bmp_p->len++] = bmp_v; \
    else { T bmp_s = bmp_v; /* addressable only on the slow path: no spill per push */ \
        *bmp_a = tvg_push_slow(*bmp_a, (desc), &bmp_s); } } while (0)
/* Push onto an array known to be uniquely owned (made unique before the loop). */
#define TVG_PUSH_U(T, arr, desc, val) do { tv_arr *bmp_a = (arr); T bmp_v = (val); tv_arrbuf *bmp_p = bmp_a->p; \
    if (__builtin_expect(bmp_p->len < bmp_p->cap, 1)) ((T *)(void *)tv_arrbuf_data(bmp_p))[bmp_p->len++] = bmp_v; \
    else { T bmp_s = bmp_v; *bmp_a = tvg_push_slow(*bmp_a, (desc), &bmp_s); } } while (0)
/* Push onto a unique array whose capacity is cached in `cap` (only pushes reallocate it). */
#define TVG_PUSH_C(T, arr, capv, desc, val) do { tv_arr *bmp_a = (arr); T bmp_v = (val); \
    if (__builtin_expect(bmp_a->p->len < (capv), 1)) ((T *)(void *)tv_arrbuf_data(bmp_a->p))[bmp_a->p->len++] = bmp_v; \
    else { T bmp_s = bmp_v; *bmp_a = tvg_push_slow(*bmp_a, (desc), &bmp_s); (capv) = bmp_a->p->cap; } } while (0)
/* Push with capacity reserved for it before the loop (reserve_for_pushes). */
#define TVG_PUSH_X(T, arr, val) do { tv_arr *bmp_a = (arr); ((T *)(void *)tv_arrbuf_data(bmp_a->p))[bmp_a->p->len++] = (val); } while (0)
/* Mutable element access: a bounds-checked pointer (a placeholder empty array made real first). */
static inline void *tvg_at_mut(tv_arr *a, const tv_type *t, size_t size, tv_int i, const char *loc) {
    tvg_make_unique(a, t);
    return tv_arr_at(*a, size, i, loc);
}
/* `xs[i] = v` of plain data when i isn't below the length: the array grows to i + 1, as in
 * JavaScript, the slots between holding zeros (JavaScript's holes, read as `undefined`, are the
 * element type's zero); a negative or absurdly far index traps. The array comes back by value,
 * so the caller's never has its address taken (and can stay in registers). */
static __attribute__((noinline, cold)) tv_arr tvg_put_grow(tv_arr a, const tv_type *t, size_t size, tv_int i, const char *loc) {
    tv_int n = a.p->len;
    if (i < n || i - n > ((tv_int)1 << 28)) tv_arr_oob(i, n, loc);
    tv_arr_make_unique(&a, t, i + 1);
    memset(tv_arrbuf_data(a.p) + (size_t)n * size, 0, (size_t)(i + 1 - n) * size);
    a.p->len = i + 1;
    return a;
}
/* Sorting (radix sorts of 64-bit keys) lives in the runtime, compiled once (runtime/tov.c). */
#define tvg_radix64 tv_radix64
#define tvg_sort_f64 tv_sort_f64
/* Order-preserving 64-bit key for a double under the comparator `a - b`:
 * both zeros share a key (they compare equal), NaN sorts last. */
static inline uint64_t tvg_f64_key(double x) {
    uint64_t u;
    if (x == 0) u = 0;
    else if (x != x) u = 0x7FF8000000000000ull;
    else memcpy(&u, &x, 8);
    return (u >> 63) ? ~u : (u ^ 0x8000000000000000ull);
}
/* The program thread keeps the main thread's scheduling class (macOS would otherwise demote it). */
#if defined(__APPLE__)
#include <pthread/qos.h>
#define TVG_THREAD_QOS(a) (pthread_attr_set_qos_class_np((a), qos_class_self(), 0) == 0 || 1)
#else
#define TVG_THREAD_QOS(a) 1
#endif
/* Accumulator recursion elimination (tre_candidate). The recursion computes suffix sums
 * (the total minus a prefix sum), so each must fit. While every term is non-negative the
 * prefix sums rise from 0 to the running sum `tre_acc`, and every suffix sum lies between the
 * base value and the total: checking the final `acc + base` covers them all, in one register.
 * The first negative term or 64-bit overflow moves the sum to exact 128-bit state, which
 * remembers the smallest and largest prefix sum, and sets `tre_acc` to INT64_MIN to say so.
 * One sign test then covers all three: with `acc` >= 0 and `t` >= 0 the wrapped `acc + t` is
 * negative exactly on overflow, and INT64_MIN + t is negative for every `t` >= 0. */
typedef struct { __int128 acc, mn, mx; } tvg_tre_wide;
static __attribute__((noinline)) tv_int tvg_tre_add_slow(tvg_tre_wide *w, tv_int acc, tv_int t) {
    if (acc >= 0) { w->acc = acc; w->mn = 0; w->mx = acc; }
    else {
        if (w->acc < w->mn) w->mn = w->acc;
        if (w->acc > w->mx) w->mx = w->acc;
    }
    w->acc += t;
    return INT64_MIN;
}
static __attribute__((noinline, cold)) tv_int tvg_tre_end_slow(tvg_tre_wide *w, tv_int acc, tv_int b, const char *loc) {
    if (acc >= 0) { w->acc = acc; w->mn = 0; w->mx = acc; }
    __int128 T = w->acc + b;
    if (T - w->mx < INT64_MIN || T - w->mn > INT64_MAX) tv_trap("integer overflow in +", loc);
    return (tv_int)T;
}
#ifdef TOV_UNCHECKED
#define TVG_TRE_ADD(t) (tre_acc = (tv_int)((uint64_t)tre_acc + (uint64_t)(t)))
#define TVG_TRE_END(b, loc) ((void)(loc), (tv_int)((uint64_t)tre_acc + (uint64_t)(b)))
#else
#define TVG_TRE_ADD(t) do { tv_int tre_t_ = (t), tre_n_ = (tv_int)((uint64_t)tre_acc + (uint64_t)tre_t_); \
    if (__builtin_expect((tre_t_ | tre_n_) < 0, 0)) tre_acc = tvg_tre_add_slow(&tre_ws, tre_acc, tre_t_); \
    else tre_acc = tre_n_; } while (0)
#define TVG_TRE_END(b, loc) ({ tv_int tre_b_ = (b), tre_r_; \
    if (__builtin_expect(tre_acc < 0, 0)) tre_r_ = tvg_tre_end_slow(&tre_ws, tre_acc, tre_b_, (loc)); \
    else if (__builtin_expect(__builtin_add_overflow(tre_acc, tre_b_, &tre_r_), 0)) tre_r_ = tvg_tre_end_slow(&tre_ws, tre_acc, tre_b_, (loc)); \
    tre_r_; })
#endif
/* TypeScript's numbers, computed as integers: a result past ±2^53 is the double nearest it, as
 * JavaScript holds it (each operand is one exactly, so the double operation rounds the exact
 * result), and one past the integer range traps. */
static __attribute__((noinline, cold)) tv_int tvg_jround(double d, const char *loc) {
    if (!(d > -9223372036854775808.0 && d < 9223372036854775808.0)) tv_trap("a number computed as an integer passed the integer range", loc);
    return (tv_int)d;
}
#define TVG_PAST53(r) ((uint64_t)(r) + (UINT64_C(1) << 53) > (UINT64_C(1) << 54))
/* `a + k` / `a - k`, k a constant 0 < k < 2^53: one test of a (it's within ±2^53, and the
 * result is too), so no overflow test besides. */
#define TV_JADDK(a, k, loc) ({ tv_int tv__a = (a); __builtin_expect((uint64_t)tv__a + (UINT64_C(1) << 53) > (UINT64_C(1) << 54) - (uint64_t)(k), 0) ? tvg_jround((double)tv__a + (double)(k), loc) : tv__a + (k); })
#define TV_JSUBK(a, k, loc) ({ tv_int tv__a = (a); __builtin_expect((uint64_t)tv__a + (UINT64_C(1) << 53) - (uint64_t)(k) > (UINT64_C(1) << 54) - (uint64_t)(k), 0) ? tvg_jround((double)tv__a - (double)(k), loc) : tv__a - (k); })
#ifdef TOV_UNCHECKED
#define TV_JADD(a, b, loc) TV_ADD(tv_int, a, b, loc)
#define TV_JSUB(a, b, loc) TV_SUB(tv_int, a, b, loc)
#define TV_JMUL(a, b, loc) TV_MUL(tv_int, a, b, loc)
#else
#define TV_JADD(a, b, loc) ({ tv_int tv__a = (a), tv__b = (b), tv__r; if (__builtin_expect(__builtin_add_overflow(tv__a, tv__b, &tv__r) || TVG_PAST53(tv__r), 0)) tv__r = tvg_jround((double)tv__a + (double)tv__b, loc); tv__r; })
#define TV_JSUB(a, b, loc) ({ tv_int tv__a = (a), tv__b = (b), tv__r; if (__builtin_expect(__builtin_sub_overflow(tv__a, tv__b, &tv__r) || TVG_PAST53(tv__r), 0)) tv__r = tvg_jround((double)tv__a - (double)tv__b, loc); tv__r; })
#define TV_JMUL(a, b, loc) ({ tv_int tv__a = (a), tv__b = (b), tv__r; if (__builtin_expect(__builtin_mul_overflow(tv__a, tv__b, &tv__r) || TVG_PAST53(tv__r), 0)) tv__r = tvg_jround((double)tv__a * (double)tv__b, loc); tv__r; })
#endif
#ifdef TOV_UNCHECKED
/* --unchecked: two's-complement wrapping (defined behaviour, like Rust release builds). */
#define TV_ADD(T, a, b, loc) ({ T tv__r; (void)__builtin_add_overflow((a), (b), &tv__r); tv__r; })
#define TV_SUB(T, a, b, loc) ({ T tv__r; (void)__builtin_sub_overflow((a), (b), &tv__r); tv__r; })
#define TV_MUL(T, a, b, loc) ({ T tv__r; (void)__builtin_mul_overflow((a), (b), &tv__r); tv__r; })
#else
#define TV_ADD(T, a, b, loc) ({ T tv__r; if (__builtin_expect(__builtin_add_overflow((a), (b), &tv__r), 0)) tv_trap("integer overflow in +", loc); tv__r; })
#define TV_SUB(T, a, b, loc) ({ T tv__r; if (__builtin_expect(__builtin_sub_overflow((a), (b), &tv__r), 0)) tv_trap("integer overflow in -", loc); tv__r; })
#define TV_MUL(T, a, b, loc) ({ T tv__r; if (__builtin_expect(__builtin_mul_overflow((a), (b), &tv__r), 0)) tv_trap("integer overflow in *", loc); tv__r; })
#endif
/* `x % d` by a d a loop nest doesn't change (see Gen::hoist_divisors): d's reciprocal, worked out
 * once before the loop (Granlund and Montgomery's round-up method), turns each remainder of
 * x >= 0 by d > 0 into a multiply and shifts instead of a division. */
typedef struct { uint64_t m, d; uint8_t s1, s2; } tvg_udiv;
static inline tvg_udiv tvg_udiv_new(tv_int d) {
    tvg_udiv r = {0, 0, 0, 0};
    if (d <= 0) return r;
    int l = d == 1 ? 0 : 64 - __builtin_clzll((uint64_t)d - 1);
    r.m = (uint64_t)((((unsigned __int128)1 << 64) * (((unsigned __int128)1 << l) - (uint64_t)d)) / (uint64_t)d) + 1;
    r.d = (uint64_t)d;
    r.s1 = l < 1 ? l : 1;
    r.s2 = l > 1 ? l - 1 : 0;
    return r;
}
static inline uint64_t tvg_udiv_q(uint64_t x, tvg_udiv v) {
    uint64_t t = (uint64_t)(((unsigned __int128)x * v.m) >> 64);
    return (t + ((x - t) >> v.s1)) >> v.s2;
}
#define TVG_REM_BY(a, b, dv, loc) ({ tv_int tv__x = (a); (tv__x >= 0 && (dv).d) ? (tv_int)((uint64_t)tv__x - tvg_udiv_q((uint64_t)tv__x, (dv)) * (dv).d) : TV_REM(tv_int, tv__x, (b), loc); })
#define TV_REM(T, a, b, loc) ({ T tv__b = (b); if (tv__b == 0) tv_trap("integer remainder by zero", loc); T tv__a = (a); (tv__b == -1) ? (T)0 : (T)(tv__a % tv__b); })
#define TV_UREM(T, a, b, loc) ({ T tv__b = (b); if (tv__b == 0) tv_trap("integer remainder by zero", loc); (T)((a) % tv__b); })
#define TV_NEG(T, a, loc) ({ T tv__r; if (__builtin_sub_overflow((T)0, (a), &tv__r)) tv_trap("integer overflow in unary -", loc); tv__r; })
#define TV_CONV(T, v, lo, hi, loc) ({ __typeof__(v) tv__v = (v); if (tv__v < (lo) || tv__v > (hi)) tv_trap("integer conversion out of range", loc); (T)tv__v; })
static inline tv_int tv_pow_int(tv_int b, tv_int e, const char *loc) {
    if (e < 0) tv_trap("negative integer exponent (use f64 for fractional powers)", loc);
    tv_int r = 1;
    while (e > 0) {
        if (e & 1) { if (__builtin_mul_overflow(r, b, &r)) tv_trap("integer overflow in **", loc); }
        e >>= 1;
        if (e > 0 && __builtin_mul_overflow(b, b, &b)) tv_trap("integer overflow in **", loc);
    }
    return r;
}
static inline double tv_min_f64(double a, double b) { return (a != a || b != b) ? (0.0 / 0.0) : (a < b ? a : (a > b ? b : (a == 0 && b == 0 ? (signbit(a) ? a : b) : a))); }
static inline double tv_max_f64(double a, double b) { return (a != a || b != b) ? (0.0 / 0.0) : (a > b ? a : (a < b ? b : (a == 0 && b == 0 ? (signbit(a) ? b : a) : a))); }
