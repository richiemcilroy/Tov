/* js.h — Tov's bridge to JavaScriptCore, for npm packages and the Node.js built-ins under them.
 *
 * A program that imports npm packages embeds their bundle (see crates/tov/src/npm) and links
 * runtime/js.c and runtime/node.c with JavaScriptCore. One context per process, created on the
 * first use of a package. Single-threaded, like the rest of the runtime.
 *
 * Values: a Tov `Js` value is a JSValueRef. JavaScriptCore's 64-bit JSValueRef is the encoded
 * value (as Bun relies on too), so numbers, booleans, undefined and null are made and read here
 * without calling the engine (tv_js() checks the encoding once; if it ever differs, everything
 * goes through the API). While Tov holds a cell (an object or a string) in a variable, field
 * or container, it's in a root table kept on the program's main stack, which the collector
 * scans conservatively: no engine call either (JSValueProtect takes over if the table fills).
 *
 * Microtasks: JavaScriptCore runs its microtask queue whenever the outermost call into
 * JavaScript returns (an evaluation, a call, a resolve), so promise reactions run right after
 * the native code that triggered them, as in Node.js. tv_js_drain is for the rare case of work
 * queued with no call in progress.
 *
 * This header declares the few JavaScriptCore types it needs instead of including its headers
 * (generated code compiles faster); js.c and node.c include the real ones. */
#ifndef TOV_JS_H
#define TOV_JS_H

#include "tov.h"

/* The same typedefs as <JavaScriptCore/JSBase.h> (repeating a typedef is allowed in C11). */
typedef const struct OpaqueJSContextGroup *JSContextGroupRef;
typedef const struct OpaqueJSContext *JSContextRef;
typedef struct OpaqueJSContext *JSGlobalContextRef;
typedef struct OpaqueJSString *JSStringRef;
typedef struct OpaqueJSClass *JSClassRef;
typedef const struct OpaqueJSValue *JSValueRef;
typedef struct OpaqueJSValue *JSObjectRef;
typedef JSValueRef (*JSObjectCallAsFunctionCallback)(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                                     size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception);

/* ------------------------------------------------------------------ the engine */

/* The program's bundle (generated code embeds it): the npm packages it imports and the Node.js
 * built-ins they use — a prelude that defines globalThis.__tov_npm, and each module's code,
 * compiled when it's first required (layout: Bundle::blob in crates/tov/src/npm/bundle.rs). */
extern const unsigned char tv_js_blob[];

/* The context: created on first use (tv_js_start), with globalThis.__tov_native (filled by
 * tv_node_install), globalThis.__tov_source, and the bundle evaluated. */
JSContextRef tv_js_start(void);
extern JSGlobalContextRef tv_js_ctx;
static inline JSContextRef tv_js(void) { return tv_js_ctx ? tv_js_ctx : tv_js_start(); }

/* Filled by runtime/node.c: the natives Node.js's built-ins run on (process, timers, fs, ...),
 * as functions on `native` (globalThis.__tov_native). */
void tv_node_install(JSContextRef ctx, JSObjectRef native);
/* obj[name] = a function calling fn. */
void tv_js_def(JSContextRef ctx, JSObjectRef obj, const char *name, JSObjectCallAsFunctionCallback fn);

/* Set by runtime/napi.c when the program links it (native addons). */
extern void (*tv_js_napi_install)(JSContextRef ctx);

/* Runs JavaScript's pending microtasks (see above). */
void tv_js_drain(void);

/* ------------------------------------------------------------------ values */

/* (tv_js_ctx: NULL until tv_js()) */
extern bool tv_js_encoded;             /* the engine encodes values as below (checked by tv_js()) */

#define TV_JS_NUMBER_TAG 0xfffe000000000000ull
#define TV_JS_DOUBLE_OFFSET 0x0002000000000000ull
#define TV_JS_NOT_CELL 0xfffe000000000002ull
#define TV_JS_FALSE 0x06ull
#define TV_JS_TRUE 0x07ull
#define TV_JS_UNDEFINED 0x0aull
#define TV_JS_NULL 0x02ull

static inline uint64_t tv_js_bits(JSValueRef v) { uint64_t u; memcpy(&u, &v, 8); return u; }
static inline JSValueRef tv_js_of_bits(uint64_t u) { JSValueRef v; memcpy(&v, &u, 8); return v; }
/* An object, string, symbol or bigint: what the collector must be told Tov holds. */
static inline bool tv_js_is_cell(JSValueRef v) {
    uint64_t u = tv_js_bits(v);
    return tv_js_encoded ? u != 0 && (u & TV_JS_NOT_CELL) == 0 : u != 0;
}

void tv_js_root_add(JSValueRef v);
void tv_js_root_remove(JSValueRef v);
/* The program's main frame gives the root table its storage (see above). */
void tv_js_roots_init(JSValueRef *keys, uint32_t cap);
static inline JSValueRef tv_js_retain(JSValueRef v) { if (tv_js_is_cell(v)) tv_js_root_add(v); return v; }
static inline void tv_js_release(JSValueRef v) { if (tv_js_is_cell(v)) tv_js_root_remove(v); }

extern const tv_type tv_type_js;       /* Tov's `Js` (retain/release as above; String(x); inspect) */

JSValueRef tv_js_num_slow(double d);
JSValueRef tv_js_simple_slow(uint64_t bits);   /* booleans, undefined, null through the API */
static inline JSValueRef tv_js_undefined(void) { return tv_js_encoded ? tv_js_of_bits(TV_JS_UNDEFINED) : tv_js_simple_slow(TV_JS_UNDEFINED); }
static inline JSValueRef tv_js_null(void) { return tv_js_encoded ? tv_js_of_bits(TV_JS_NULL) : tv_js_simple_slow(TV_JS_NULL); }
static inline JSValueRef tv_js_bool(bool b) { return tv_js_encoded ? tv_js_of_bits(b ? TV_JS_TRUE : TV_JS_FALSE) : tv_js_simple_slow(b ? TV_JS_TRUE : TV_JS_FALSE); }
static inline JSValueRef tv_js_num(double d) {
    if (!tv_js_encoded) return tv_js_num_slow(d);
    uint64_t u;
    memcpy(&u, &d, 8);
    if (d >= -2147483648.0 && d <= 2147483647.0) {
        int32_t i = (int32_t)d;
        if ((double)i == d && u != 0x8000000000000000ull) return tv_js_of_bits(TV_JS_NUMBER_TAG | (uint32_t)i);
    }
    if (d != d) u = 0x7ff8000000000000ull;   /* the canonical NaN */
    return tv_js_of_bits(u + TV_JS_DOUBLE_OFFSET);
}
static inline bool tv_js_is_nullish(JSValueRef v) {
    uint64_t u = tv_js_bits(v);
    return u == TV_JS_UNDEFINED || u == TV_JS_NULL;
}
/* UTF-8 → a JavaScript string (invalid UTF-8 becomes U+FFFD). */
JSValueRef tv_js_str(const char *s, size_t n);
/* A Tov string (NUL-terminated; a literal's JavaScript string is made once and kept). */
JSValueRef tv_js_from_str(tv_str s);
/* String(v) as UTF-8 (lone surrogates become U+FFFD). Exceptions from toString are swallowed
 * into "[object]"-style text; use tv_js_call for code that may throw. */
tv_str tv_js_to_str(JSValueRef v);

/* typeof, and checked conversions: false if `v` isn't one (no coercion). */
const char *tv_js_typeof(JSValueRef v);    /* "undefined", "object", "function", "string", ... */
bool tv_js_as_num_slow(JSValueRef v, double *out);
bool tv_js_as_bool_slow(JSValueRef v, bool *out);
static inline bool tv_js_as_num(JSValueRef v, double *out) {
    if (!tv_js_encoded) return tv_js_as_num_slow(v, out);
    uint64_t u = tv_js_bits(v);
    if ((u & TV_JS_NUMBER_TAG) == TV_JS_NUMBER_TAG) {
        *out = (int32_t)(uint32_t)u;
        return true;
    }
    if ((u & TV_JS_NUMBER_TAG) == 0) return false;
    u -= TV_JS_DOUBLE_OFFSET;
    memcpy(out, &u, 8);
    return true;
}
static inline bool tv_js_as_bool(JSValueRef v, bool *out) {
    if (!tv_js_encoded) return tv_js_as_bool_slow(v, out);
    uint64_t u = tv_js_bits(v);
    if (u != TV_JS_TRUE && u != TV_JS_FALSE) return false;
    *out = u == TV_JS_TRUE;
    return true;
}
bool tv_js_as_str(JSValueRef v, tv_str *out);

/* ------------------------------------------------------------------ calls and properties
 *
 * Each returns the result, or NULL with *exc set to the thrown value (a JS exception). */

/* A property name as generated code uses it (`static tv_js_name k = {"name"};`): its string, and
 * small compiled functions reading the property and calling the method (one engine call each,
 * with the engine's own property caches). */
typedef struct tv_js_name {
    const char *text;
    JSStringRef str;
    JSValueRef value;          /* the name as a JavaScript string */
    JSObjectRef get;           /* (o) => o.name */
    JSObjectRef call[5];       /* (o, a0, ...) => o.name(a0, ...), by argument count */
    void *id;                  /* the name as Tov's own engine keeps it (TVPropertyName) */
} tv_js_name;
JSValueRef tv_js_name_get(JSValueRef obj, tv_js_name *k, JSValueRef *exc);
JSValueRef tv_js_name_call(JSValueRef obj, tv_js_name *k, size_t n, const JSValueRef *args, JSValueRef *exc);
/* A record as a JavaScript object: `make` is (a0, ...) => ({k0: a0, ...}), compiled on first use. */
typedef struct tv_js_shape {
    uint32_t n;
    const char *const *keys;
    const bool *optional;     /* (absent when undefined) */
    JSObjectRef make;
} tv_js_shape;
JSValueRef tv_js_shape_make(tv_js_shape *s, const JSValueRef *values);
/* A fused JavaScript expression (see codegen/js.rs): `return <expr>` over parameters a0, a1, ...,
 * compiled into *slot on first use; NULL with *exc set if it throws. */
JSValueRef tv_js_thunk(JSObjectRef *slot, const char *body, size_t n, const JSValueRef *args, JSValueRef *exc);
/* An exception where Tov code can't pass it on (a property read): traps. */
_Noreturn void tv_js_throw_trap(JSValueRef exc, const char *loc);

/* A property name, created once: `static JSStringRef k; tv_js_key(&k, "name")`. */
JSStringRef tv_js_key_slow(JSStringRef *slot, const char *name);
static inline JSStringRef tv_js_key(JSStringRef *slot, const char *name) { return *slot ? *slot : tv_js_key_slow(slot, name); }

JSValueRef tv_js_get(JSValueRef obj, JSStringRef key, JSValueRef *exc);
bool tv_js_set(JSValueRef obj, JSStringRef key, JSValueRef value, JSValueRef *exc);
JSValueRef tv_js_index(JSValueRef obj, uint32_t i, JSValueRef *exc);
/* fn(...args) with `this` (NULL: undefined). */
JSValueRef tv_js_call(JSValueRef fn, JSValueRef self, size_t n, const JSValueRef *args, JSValueRef *exc);
/* obj.key(...args) */
JSValueRef tv_js_invoke(JSValueRef obj, JSStringRef key, size_t n, const JSValueRef *args, JSValueRef *exc);
/* new fn(...args) */
JSValueRef tv_js_new(JSValueRef fn, size_t n, const JSValueRef *args, JSValueRef *exc);

/* A package's exports: require(spec) inside the bundle (spec as the program imports it). */
JSValueRef tv_js_import(const char *spec, JSValueRef *exc);

/* Set by the program: a Tov JsError object (owned) holding a thrown JavaScript value. */
extern void *(*tv_js_make_error)(JSValueRef exc);

/* A thrown value as text: "Name: message" and, for errors, the stack. */
tv_str tv_js_error_text(JSValueRef exc);
/* For a Tov JsError: the thrown value's name and message (an Error's own; else "JsError" and
 * String(value)). */
void tv_js_error_parts(JSValueRef exc, tv_str *name, tv_str *message);

/* ------------------------------------------------------------------ for generated code */

/* `import ... from "spec"`: kind 0 the namespace (`import * as`), 1 the default export, else
 * the module's exports; traps if the package fails to load. */
JSValueRef tv_js_import_as(const char *spec, int kind, const char *loc);
/* obj.key / obj[i] / obj[k]; a JavaScript exception (a throwing getter, a property of
 * undefined) traps. */
JSValueRef tv_js_get_or_trap(JSValueRef obj, tv_js_name *key, const char *loc);
JSValueRef tv_js_at_or_trap(JSValueRef obj, double i, const char *loc);
JSValueRef tv_js_key_or_trap(JSValueRef obj, tv_str key, const char *loc);
void tv_js_put_or_trap(JSValueRef obj, tv_js_name *key, JSValueRef value, const char *loc);
/* A value from JavaScript that doesn't convert to the type `want` names: traps. */
_Noreturn void tv_js_type_trap(JSValueRef v, const char *want, const char *loc);
bool tv_js_truthy_slow(JSValueRef v);
static inline bool tv_js_truthy(JSValueRef v) {
    if (!tv_js_encoded || tv_js_is_cell(v)) return tv_js_truthy_slow(v);
    uint64_t u = tv_js_bits(v);
    if ((u & TV_JS_NUMBER_TAG) == TV_JS_NUMBER_TAG) return (uint32_t)u != 0;
    if (u & TV_JS_NUMBER_TAG) {
        double d;
        u -= TV_JS_DOUBLE_OFFSET;
        memcpy(&d, &u, 8);
        return d == d && d != 0;
    }
    return u == TV_JS_TRUE;
}
bool tv_js_is_array(JSValueRef v);
uint32_t tv_js_length(JSValueRef v);    /* v.length as an array index */
JSValueRef tv_js_array(size_t n, const JSValueRef *items);
JSValueRef tv_js_object(void);
JSValueRef tv_js_lit(JSValueRef *slot, const char *s, size_t n);   /* a string literal, created once */
/* A JavaScript function calling a Tov closure through `tramp` (generated per signature); the
 * closure's environment is held until the function is collected. */
typedef JSValueRef (*tv_js_tramp)(const tv_fn *fn, size_t n, const JSValueRef *args);
JSValueRef tv_js_function(tv_fn fn, tv_js_tramp tramp);

/* ------------------------------------------------------------------ bytes */

/* A Uint8Array over ptr[0, len) without copying; dealloc(ptr, ctx) runs when it's collected
 * (NULL: never). */
JSObjectRef tv_js_bytes(void *ptr, size_t len, void (*dealloc)(void *bytes, void *ctx), void *ctx);
/* A copy of bytes in a new Uint8Array. */
JSObjectRef tv_js_bytes_copy(const void *ptr, size_t len);
/* The bytes of a typed array, ArrayBuffer or DataView (valid until JavaScript next runs). */
bool tv_js_bytes_view(JSValueRef v, uint8_t **ptr, size_t *len);

/* ------------------------------------------------------------------ promises */

/* A pending JavaScript promise that native code settles later (once). */
typedef struct tv_js_deferred tv_js_deferred;
JSObjectRef tv_js_deferred_new(tv_js_deferred **out);
void tv_js_settle(tv_js_deferred *d, JSValueRef value, bool ok);   /* resolves (ok) or rejects; frees d */

/* Tov awaiting a JavaScript value: a Tov promise (of Js) settled when `v` settles (now, if `v`
 * isn't a thenable). A rejection becomes a JsError holding the reason. */
/* (set by runtime/node.c: told about each promise Tov awaits, which handles its rejection) */
extern JSObjectRef tv_js_handled_hook;
tv_promise *tv_js_await(JSValueRef v);

#endif /* TOV_JS_H */
