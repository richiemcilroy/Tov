/* napi.c — Node-API (N-API) on JavaScriptCore, so npm packages' native addons (`.node` files built
 * with napi-rs, node-addon-api, plain C) load in Tov programs. Linked, with the napi_* symbols
 * exported, only by programs whose bundle holds a native module.
 *
 * - A napi_value is a JSValueRef. Values made during a native call are kept alive (Tov's root
 *   table, see js.h) until its handle scope closes or the call returns, as Node.js's handle scopes do.
 * - An exception a native function leaves pending is thrown when it returns to JavaScript.
 * - napi_wrap, externals and finalizers hang a private object on the target; its finalizer runs
 *   when the collector takes it, and the addon's callback runs later, outside the collector.
 * - Async work runs on a pool of threads; it and thread-safe functions come back to the main
 *   thread through a pipe the event loop watches.
 *
 * The types and values below are Node-API's ABI (js_native_api_types.h, node_api_types.h). */

#include "js.h"

#if defined(__APPLE__) && !defined(TV_JSC_OWN)
#include <JavaScriptCore/JavaScriptCore.h>
#else
#include <JavaScriptCore/JavaScript.h>
#endif
#ifdef TV_JSC_OWN
#include <JavaScriptCore/TovAPI.h>
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* (char16_t: macOS SDKs have no <uchar.h> for C) */
typedef uint16_t char16_t;
#include <unistd.h>

#define NAPI_EXTERN __attribute__((visibility("default"), used))
#define NAPI_AUTO_LENGTH SIZE_MAX

typedef struct napi_env__ *napi_env;
typedef struct napi_value__ *napi_value;
typedef struct napi_ref__ *napi_ref;
typedef struct napi_handle_scope__ *napi_handle_scope;
typedef struct napi_escapable_handle_scope__ *napi_escapable_handle_scope;
typedef struct napi_callback_info__ *napi_callback_info;
typedef struct napi_deferred__ *napi_deferred;
typedef struct napi_async_work__ *napi_async_work;
typedef struct napi_threadsafe_function__ *napi_threadsafe_function;
typedef struct napi_async_context__ *napi_async_context;
typedef struct napi_callback_scope__ *napi_callback_scope;
typedef struct napi_async_cleanup_hook_handle__ *napi_async_cleanup_hook_handle;

typedef enum {
    napi_default = 0,
    napi_writable = 1 << 0,
    napi_enumerable = 1 << 1,
    napi_configurable = 1 << 2,
    napi_static = 1 << 10,
} napi_property_attributes;

typedef enum { napi_undefined, napi_null, napi_boolean, napi_number, napi_string, napi_symbol, napi_object, napi_function, napi_external, napi_bigint } napi_valuetype;

typedef enum {
    napi_int8_array,
    napi_uint8_array,
    napi_uint8_clamped_array,
    napi_int16_array,
    napi_uint16_array,
    napi_int32_array,
    napi_uint32_array,
    napi_float32_array,
    napi_float64_array,
    napi_bigint64_array,
    napi_biguint64_array,
    napi_float16_array,
} napi_typedarray_type;

typedef enum {
    napi_ok,
    napi_invalid_arg,
    napi_object_expected,
    napi_string_expected,
    napi_name_expected,
    napi_function_expected,
    napi_number_expected,
    napi_boolean_expected,
    napi_array_expected,
    napi_generic_failure,
    napi_pending_exception,
    napi_cancelled,
    napi_escape_called_twice,
    napi_handle_scope_mismatch,
    napi_callback_scope_mismatch,
    napi_queue_full,
    napi_closing,
    napi_bigint_expected,
    napi_date_expected,
    napi_arraybuffer_expected,
    napi_detachable_arraybuffer_expected,
    napi_would_deadlock,
    napi_no_external_buffers_allowed,
    napi_cannot_run_js,
} napi_status;

typedef napi_value (*napi_callback)(napi_env env, napi_callback_info info);
typedef void (*napi_finalize)(napi_env env, void *finalize_data, void *finalize_hint);

typedef struct {
    const char *utf8name;
    napi_value name;
    napi_callback method;
    napi_callback getter;
    napi_callback setter;
    napi_value value;
    napi_property_attributes attributes;
    void *data;
} napi_property_descriptor;

typedef struct {
    const char *error_message;
    void *engine_reserved;
    uint32_t engine_error_code;
    napi_status error_code;
} napi_extended_error_info;

typedef enum { napi_key_include_prototypes, napi_key_own_only } napi_key_collection_mode;
typedef enum { napi_key_all_properties = 0, napi_key_writable = 1, napi_key_enumerable = 1 << 1, napi_key_configurable = 1 << 2, napi_key_skip_strings = 1 << 3, napi_key_skip_symbols = 1 << 4 } napi_key_filter;
typedef enum { napi_key_keep_numbers, napi_key_numbers_to_strings } napi_key_conversion;

typedef struct {
    uint64_t lower;
    uint64_t upper;
} napi_type_tag;

typedef enum { napi_tsfn_release, napi_tsfn_abort } napi_threadsafe_function_release_mode;
typedef enum { napi_tsfn_nonblocking, napi_tsfn_blocking } napi_threadsafe_function_call_mode;
typedef void (*napi_async_execute_callback)(napi_env env, void *data);
typedef void (*napi_async_complete_callback)(napi_env env, napi_status status, void *data);
typedef void (*napi_threadsafe_function_call_js)(napi_env env, napi_value js_callback, void *context, void *data);
typedef void (*napi_cleanup_hook)(void *arg);
typedef void (*napi_async_cleanup_hook)(napi_async_cleanup_hook_handle handle, void *data);

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    const char *release;
} napi_node_version;

typedef napi_value (*napi_addon_register_func)(napi_env env, napi_value exports);
typedef struct napi_module {
    int nm_version;
    unsigned int nm_flags;
    const char *nm_filename;
    napi_addon_register_func nm_register_func;
    const char *nm_modname;
    void *nm_priv;
    void *reserved[4];
} napi_module;

/* ------------------------------------------------------------------ the environment */

typedef struct tv_napi_cleanup {
    napi_cleanup_hook fn;
    void *arg;
    struct tv_napi_cleanup *next;
} tv_napi_cleanup;

struct napi_env__ {
    JSContextRef ctx;
    JSValueRef pending;               /* a pending exception (rooted) */
    napi_extended_error_info error;
    void *instance_data;
    napi_finalize instance_finalize;
    void *instance_hint;
    char *filename;                   /* file:// URL of the addon */
    tv_napi_cleanup *cleanups;
    int32_t api_version;
};

static const char *const tv_napi_messages[] = {
    NULL,
    "Invalid argument",
    "An object was expected",
    "A string was expected",
    "A string or symbol was expected",
    "A function was expected",
    "A number was expected",
    "A boolean was expected",
    "An array was expected",
    "Unknown failure",
    "An exception is pending",
    "The async work item was cancelled",
    "napi_escape_handle already called on scope",
    "Invalid handle scope usage",
    "Invalid callback scope usage",
    "Thread-safe function queue is full",
    "Thread-safe function handle is closing",
    "A bigint was expected",
    "A date was expected",
    "An arraybuffer was expected",
    "A detachable arraybuffer was expected",
    "Main thread would deadlock",
    "External buffers are not allowed",
    "Cannot run JavaScript",
};

static napi_status tv_napi_set(napi_env env, napi_status s) {
    if (env) {
        env->error.error_code = s;
        env->error.error_message = (unsigned)s < sizeof tv_napi_messages / sizeof *tv_napi_messages ? tv_napi_messages[s] : NULL;
    }
    return s;
}
#define OK(env) tv_napi_set((env), napi_ok)
#define FAIL(env, s) tv_napi_set((env), (s))
#define ARG(env, x) do { if (!(x)) return FAIL((env), napi_invalid_arg); } while (0)

/* ------------------------------------------------------------------ handles
 *
 * Values handed to native code: rooted from creation until their scope closes. Scopes nest; a
 * native call from JavaScript opens one implicitly. */

static JSValueRef *tv_napi_handles;
static size_t tv_napi_nhandles, tv_napi_caphandles;

static napi_value tv_napi_hold(JSValueRef v) {
    if (v && tv_js_is_cell(v)) {
        if (tv_napi_nhandles == tv_napi_caphandles) {
            tv_napi_caphandles = tv_napi_caphandles ? tv_napi_caphandles * 2 : 256;
            tv_napi_handles = realloc(tv_napi_handles, tv_napi_caphandles * sizeof *tv_napi_handles);
            if (!tv_napi_handles) abort();
        }
        tv_napi_handles[tv_napi_nhandles++] = tv_js_retain(v);
    }
    return (napi_value)v;
}

static void tv_napi_release_to(size_t mark) {
    while (tv_napi_nhandles > mark) tv_js_release(tv_napi_handles[--tv_napi_nhandles]);
}

#define V(x) ((JSValueRef)(x))
#define O(x) ((JSObjectRef)(x))

/* An exception from an engine call: pending, for the native code to see. */
static napi_status tv_napi_caught(napi_env env, JSValueRef exc) {
    if (env->pending) tv_js_release(env->pending);
    env->pending = tv_js_retain(exc);
    return FAIL(env, napi_pending_exception);
}
#define CHECK_EXC(env, exc) do { if (exc) return tv_napi_caught((env), (exc)); } while (0)

/* ------------------------------------------------------------------ deferred work
 *
 * Finalizers (from the collector) and completions (from other threads) run on the main thread,
 * outside the collector: queued, and the event loop woken through a pipe. */

typedef struct tv_napi_job {
    void (*fn)(void *a, void *b, void *c, void *d);
    void *a, *b, *c, *d;
    struct tv_napi_job *next;
} tv_napi_job;

static pthread_mutex_t tv_napi_lock = PTHREAD_MUTEX_INITIALIZER;
static tv_napi_job *tv_napi_jobs, *tv_napi_jobs_tail;
static int tv_napi_pipe[2] = {-1, -1};
static tv_io tv_napi_io;
static int tv_napi_keepalive;           /* async work in flight and ref'd thread-safe functions */

static void tv_napi_run_jobs(void) {
    for (;;) {
        pthread_mutex_lock(&tv_napi_lock);
        tv_napi_job *j = tv_napi_jobs;
        tv_napi_jobs = tv_napi_jobs_tail = NULL;
        pthread_mutex_unlock(&tv_napi_lock);
        if (!j) return;
        while (j) {
            tv_napi_job *next = j->next;
            size_t mark = tv_napi_nhandles;
            j->fn(j->a, j->b, j->c, j->d);
            tv_napi_release_to(mark);
            free(j);
            j = next;
        }
    }
}

static void tv_napi_ready(tv_io *h, bool readable, bool writable, bool broken) {
    (void)h; (void)writable; (void)broken;
    if (readable) {
        char buf[256];
        while (read(tv_napi_pipe[0], buf, sizeof buf) > 0) {
        }
    }
    tv_napi_run_jobs();
    tv_js_drain();
}

static void tv_napi_loop_init(void) {
    if (tv_napi_pipe[0] >= 0) return;
    if (pipe(tv_napi_pipe) != 0) abort();
    fcntl(tv_napi_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(tv_napi_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(tv_napi_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(tv_napi_pipe[1], F_SETFD, FD_CLOEXEC);
    tv_napi_io.ready = tv_napi_ready;
    tv_io_add(tv_napi_pipe[0], &tv_napi_io, true, false);
}

/* (any thread) */
static void tv_napi_post(void (*fn)(void *, void *, void *, void *), void *a, void *b, void *c, void *d) {
    tv_napi_job *j = malloc(sizeof *j);
    if (!j) abort();
    *j = (tv_napi_job){fn, a, b, c, d, NULL};
    pthread_mutex_lock(&tv_napi_lock);
    if (tv_napi_jobs_tail) tv_napi_jobs_tail->next = j; else tv_napi_jobs = j;
    tv_napi_jobs_tail = j;
    pthread_mutex_unlock(&tv_napi_lock);
    char c1 = 1;
    ssize_t w = write(tv_napi_pipe[1], &c1, 1);
    (void)w;
}

/* the event loop keeps running while there's work it'll be woken for */
static void tv_napi_ref(int d) {
    int before = tv_napi_keepalive;
    tv_napi_keepalive += d;
    if (before == 0 && tv_napi_keepalive > 0) tv_io_refs++;
    else if (before > 0 && tv_napi_keepalive == 0) tv_io_refs--;
}

/* ------------------------------------------------------------------ private objects
 *
 * Native data on a JavaScript object: a private holder object (class tv_napi_box) stored under a
 * symbol; collected with its owner, when its finalizer queues the addon's. */

typedef struct {
    napi_env env;
    void *data;
    napi_finalize finalize;
    void *hint;
    bool is_wrap;
    napi_type_tag tag;
    bool tagged;
} tv_napi_box;

static JSClassRef tv_napi_box_class, tv_napi_external_class, tv_napi_function_class;
static JSValueRef tv_napi_function_proto;   /* Function.prototype (rooted) */
static JSValueRef tv_napi_wrap_key, tv_napi_tag_key, tv_napi_finalizers_key;   /* symbols (rooted) */

static void tv_napi_finalize_job(void *a, void *b, void *c, void *d) {
    napi_finalize fin = (napi_finalize)a;
    fin((napi_env)b, c, d);
}

static void tv_napi_box_finalize(JSObjectRef o) {
    tv_napi_box *box = JSObjectGetPrivate(o);
    if (!box) return;
    if (box->finalize) tv_napi_post(tv_napi_finalize_job, (void *)box->finalize, box->env, box->data, box->hint);
    free(box);
}

static JSValueRef tv_napi_eval(JSContextRef ctx, const char *src) {
    JSStringRef s = JSStringCreateWithUTF8CString(src);
    JSValueRef v = JSEvaluateScript(ctx, s, NULL, NULL, 1, NULL);
    JSStringRelease(s);
    return v;
}

/* JavaScript helpers the API is easier to write with (compiled once) */
static JSObjectRef tv_napi_js;

static JSObjectRef tv_napi_helper(JSContextRef ctx, const char *name) {
    JSStringRef k = JSStringCreateWithUTF8CString(name);
    JSValueRef f = JSObjectGetProperty(ctx, tv_napi_js, k, NULL);
    JSStringRelease(k);
    return (JSObjectRef)f;
}

static JSValueRef tv_napi_call_helper(napi_env env, const char *name, size_t n, const JSValueRef *args, JSValueRef *exc) {
    return JSObjectCallAsFunction(env->ctx, tv_napi_helper(env->ctx, name), NULL, n, args, exc);
}

static JSValueRef tv_napi_function_call(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc);

static void tv_napi_global_init(JSContextRef ctx) {
    if (tv_napi_js) return;
    JSClassDefinition d = kJSClassDefinitionEmpty;
    d.className = "NapiBox";
    d.finalize = tv_napi_box_finalize;
    tv_napi_box_class = JSClassCreate(&d);
    d.className = "External";
    tv_napi_external_class = JSClassCreate(&d);
    JSClassDefinition fd = kJSClassDefinitionEmpty;
    fd.className = "Function";
    fd.callAsFunction = tv_napi_function_call;
    fd.finalize = tv_napi_box_finalize;
    tv_napi_function_class = JSClassCreate(&fd);
    tv_napi_js = (JSObjectRef)tv_napi_eval(ctx,
        "({"
        " wrapKey: Symbol('napi.wrap'), tagKey: Symbol('napi.typeTag'), finKey: Symbol('napi.finalizers'),"
        " define: function (o, k, desc) { Object.defineProperty(o, k, desc); },"
        /* (as V8's DefineOwnProperty under Node.js: a property that can't be redefined stays, no throw) */
        " accessor: function (o, k, get, set, e, c) { Reflect.defineProperty(o, k, { get: get, set: set, enumerable: e, configurable: c }); },"
        " value: function (o, k, v, w, e, c) { Reflect.defineProperty(o, k, { value: v, writable: w, enumerable: e, configurable: c }); },"
        " setHidden: function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true, configurable: true, enumerable: false }); },"
        " getHidden: function (o, k) { return Object.prototype.hasOwnProperty.call(o, k) ? o[k] : undefined; },"
        " delHidden: function (o, k) { delete o[k]; },"
        " addFinalizer: function (o, k, box) { var a = Object.prototype.hasOwnProperty.call(o, k) ? o[k] : undefined; if (!a) { a = []; Object.defineProperty(o, k, { value: a, enumerable: false, configurable: true }); } a.push(box); },"
        " makeClass: function (impl, name) {"
        "   var C = function () { if (!new.target) throw new TypeError(\"Class constructor \" + name + \" cannot be invoked without 'new'\"); var r = impl.apply(this, [new.target].concat(Array.prototype.slice.call(arguments))); return (r !== null && (typeof r === 'object' || typeof r === 'function')) ? r : this; };"
        "   Object.defineProperty(C, 'name', { value: name }); return C; },"
        " keys: function (o, own, enumOnly, skipStrings, skipSymbols, numsToStrings) {"
        "   var out = [], seen = new Set();"
        "   for (var p = o; p !== null && p !== undefined; p = own ? null : Object.getPrototypeOf(p)) {"
        "     var ks = Reflect.ownKeys(p);"
        "     for (var i = 0; i < ks.length; i++) { var k = ks[i]; if (seen.has(k)) continue; var d = Object.getOwnPropertyDescriptor(p, k);"
        "       if (enumOnly && !d.enumerable) continue; if (typeof k === 'symbol' ? skipSymbols : skipStrings) continue; seen.add(k);"
        "       out.push(typeof k === 'string' && !numsToStrings && /^(0|[1-9][0-9]*)$/.test(k) && +k < 4294967295 ? +k : k); }"
        "   } return out; },"
        " freeze: Object.freeze, seal: Object.seal, isArray: Array.isArray,"
        " isPromise: function (v) { return v instanceof Promise; },"
        " isDate: function (v) { return v instanceof Date; },"
        " date: function (t) { return new Date(t); },"
        " dateValue: function (d) { return d.valueOf(); },"
        " isError: function (v) { return v instanceof Error; },"
        " isDataView: function (v) { return v instanceof DataView; },"
        " dataView: function (b, off, len) { return new DataView(b, off, len); },"
        " dataViewInfo: function (v) { return [v.byteLength, v.buffer, v.byteOffset]; },"
        " typedArray: function (type, b, off, len) { var T = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float32Array, Float64Array, BigInt64Array, BigUint64Array, typeof Float16Array === 'function' ? Float16Array : Float32Array][type]; return new T(b, off, len); },"
        " typedArrayBuffer: function (v) { return [v.buffer, v.byteOffset]; },"
        " toBuffer: function (u8) { var B = globalThis.Buffer; if (B) Object.setPrototypeOf(u8, B.prototype); return u8; },"
        " isBuffer: function (v) { return v instanceof Uint8Array || (typeof DataView === 'function' && v instanceof DataView); },"
        " symbol: function (d) { return d === undefined ? Symbol() : Symbol(d); },"
        " symbolFor: function (d) { return Symbol.for(d); },"
        " bigint: function (s) { return BigInt(s); },"
        " bigintWords: function (neg, words) { var r = 0n; for (var i = words.length - 1; i >= 0; i--) r = (r << 64n) + BigInt.asUintN(64, BigInt(words[i])); return neg ? -r : r; },"
        " bigintInfo: function (b) { var neg = b < 0n; if (neg) b = -b; var w = []; while (b > 0n) { w.push((b & 0xffffffffffffffffn).toString()); b >>= 64n; } return [neg, w]; },"
        " bigintToString: function (b, signed) { return (signed ? BigInt.asIntN(64, b) : BigInt.asUintN(64, b)).toString() + '|' + ((signed ? BigInt.asIntN(64, b) : BigInt.asUintN(64, b)) === b ? 1 : 0); },"
        " isBigint: function (v) { return typeof v === 'bigint'; },"
        " error: function (C, code, msg) { var e = new C(msg); if (code !== undefined) e.code = code; return e; },"
        " weakRef: function (v) { return new WeakRef(v); },"
        " deref: function (r) { return r.deref(); },"
        " detach: function (b) { if (typeof b.transfer === 'function') b.transfer(); else if (typeof structuredClone === 'function') structuredClone(b, { transfer: [b] }); },"
        " detached: function (b) { return b.detached === true || b.byteLength === 0 && b.detached !== false; },"
        " coerceBool: function (v) { return !!v; }, coerceNumber: function (v) { return +v; }, coerceString: function (v) { return '' + v; }, coerceObject: function (v) { return Object(v); },"
        " instanceOf: function (o, c) { return o instanceof c; },"
        " newInstance: function (C, args) { return Reflect.construct(C, args); },"
        " promise: function () { var r = {}; r.p = new Promise(function (ok, no) { r.ok = ok; r.no = no; }); return r; },"
        " runScript: function (s) { return (0, eval)(s); }"
        "})");
    JSValueProtect(ctx, tv_napi_js);
    JSStringRef k = JSStringCreateWithUTF8CString("wrapKey");
    tv_napi_wrap_key = JSObjectGetProperty(ctx, tv_napi_js, k, NULL);
    JSStringRelease(k);
    k = JSStringCreateWithUTF8CString("tagKey");
    tv_napi_tag_key = JSObjectGetProperty(ctx, tv_napi_js, k, NULL);
    JSStringRelease(k);
    k = JSStringCreateWithUTF8CString("finKey");
    tv_napi_finalizers_key = JSObjectGetProperty(ctx, tv_napi_js, k, NULL);
    JSStringRelease(k);
    JSValueProtect(ctx, tv_napi_wrap_key);
    JSValueProtect(ctx, tv_napi_tag_key);
    JSValueProtect(ctx, tv_napi_finalizers_key);
    tv_napi_function_proto = tv_napi_eval(ctx, "Function.prototype");
    JSValueProtect(ctx, tv_napi_function_proto);
    tv_napi_loop_init();
}

static JSObjectRef tv_napi_new_box(napi_env env, void *data, napi_finalize fin, void *hint) {
    tv_napi_box *b = calloc(1, sizeof *b);
    if (!b) abort();
    b->env = env;
    b->data = data;
    b->finalize = fin;
    b->hint = hint;
    return JSObjectMake(env->ctx, tv_napi_box_class, b);
}

/* ------------------------------------------------------------------ functions */

struct napi_callback_info__ {
    JSValueRef self;
    JSValueRef new_target;
    size_t argc;
    const JSValueRef *argv;
    void *data;
};

typedef struct {
    tv_napi_box box;          /* (first: the finalizer frees it as a box) */
    napi_callback cb;
    void *data;
} tv_napi_fn;

/* Calls a native callback from JavaScript: one implicit handle scope; a pending exception is thrown. */
static JSValueRef tv_napi_invoke(napi_env env, napi_callback cb, void *data, JSValueRef self, JSValueRef new_target, size_t n, const JSValueRef a[], JSValueRef *exc) {
    struct napi_callback_info__ info = {self, new_target, n, a, data};
    size_t mark = tv_napi_nhandles;
    napi_value r = cb(env, &info);
    JSValueRef out = r ? V(r) : JSValueMakeUndefined(env->ctx);
    if (env->pending) {
        *exc = env->pending;
        env->pending = NULL;
        /* (the exception stays rooted by the handle list until the scope closes) */
        tv_napi_hold(*exc);
        tv_js_release(*exc);
        out = NULL;
    }
    tv_napi_release_to(mark);
    return out;
}

static JSValueRef tv_napi_function_call(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    tv_napi_fn *fn = JSObjectGetPrivate(f);
    (void)ctx;
    return tv_napi_invoke(fn->box.env, fn->cb, fn->data, self ? self : JSValueMakeUndefined(fn->box.env->ctx), NULL, n, a, exc);
}

/* a class constructor's implementation: called with (new.target, ...args) and `this` */
static JSValueRef tv_napi_ctor_call(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    tv_napi_fn *fn = JSObjectGetPrivate(f);
    (void)ctx;
    return tv_napi_invoke(fn->box.env, fn->cb, fn->data, self, n > 0 ? a[0] : NULL, n > 0 ? n - 1 : 0, n > 0 ? a + 1 : a, exc);
}

static JSClassRef tv_napi_ctor_class;

static JSObjectRef tv_napi_make_fn(napi_env env, const char *name, size_t len, napi_callback cb, void *data, bool ctor) {
    tv_napi_fn *fn = calloc(1, sizeof *fn);
    if (!fn) abort();
    fn->box.env = env;
    fn->cb = cb;
    fn->data = data;
    if (ctor && !tv_napi_ctor_class) {
        JSClassDefinition d = kJSClassDefinitionEmpty;
        d.className = "Function";
        d.callAsFunction = tv_napi_ctor_call;
        d.finalize = tv_napi_box_finalize;
        tv_napi_ctor_class = JSClassCreate(&d);
    }
    JSObjectRef f = JSObjectMake(env->ctx, ctor ? tv_napi_ctor_class : tv_napi_function_class, fn);
    /* (callable objects made from a class get Object.prototype: give them Function's, for
     * call/apply/bind) */
    JSObjectSetPrototype(env->ctx, f, tv_napi_function_proto);
    if (name && !ctor) {
        size_t n = len == NAPI_AUTO_LENGTH ? strlen(name) : len;
        JSValueRef args[6] = {f, tv_js_str("name", 4), tv_js_str(name, n), JSValueMakeBoolean(env->ctx, false), JSValueMakeBoolean(env->ctx, false), JSValueMakeBoolean(env->ctx, true)};
        tv_napi_call_helper(env, "value", 6, args, NULL);
    }
    return f;
}

NAPI_EXTERN napi_status napi_create_function(napi_env env, const char *utf8name, size_t length, napi_callback cb, void *data, napi_value *result) {
    ARG(env, cb && result);
    *result = tv_napi_hold(tv_napi_make_fn(env, utf8name, length, cb, data, false));
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_cb_info(napi_env env, napi_callback_info info, size_t *argc, napi_value *argv, napi_value *this_arg, void **data) {
    ARG(env, info);
    if (argv && argc) {
        for (size_t i = 0; i < *argc; i++) argv[i] = (napi_value)(i < info->argc ? info->argv[i] : JSValueMakeUndefined(env->ctx));
    }
    if (argc) *argc = info->argc;
    if (this_arg) *this_arg = (napi_value)info->self;
    if (data) *data = info->data;
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_new_target(napi_env env, napi_callback_info info, napi_value *result) {
    ARG(env, info && result);
    *result = (napi_value)info->new_target;
    return OK(env);
}

/* ------------------------------------------------------------------ simple values */

NAPI_EXTERN napi_status napi_get_undefined(napi_env env, napi_value *r) { ARG(env, r); *r = (napi_value)JSValueMakeUndefined(env->ctx); return OK(env); }
NAPI_EXTERN napi_status napi_get_null(napi_env env, napi_value *r) { ARG(env, r); *r = (napi_value)JSValueMakeNull(env->ctx); return OK(env); }
NAPI_EXTERN napi_status napi_get_boolean(napi_env env, bool b, napi_value *r) { ARG(env, r); *r = (napi_value)JSValueMakeBoolean(env->ctx, b); return OK(env); }
NAPI_EXTERN napi_status napi_get_global(napi_env env, napi_value *r) { ARG(env, r); *r = tv_napi_hold(JSContextGetGlobalObject(env->ctx)); return OK(env); }
NAPI_EXTERN napi_status napi_create_double(napi_env env, double v, napi_value *r) { ARG(env, r); *r = (napi_value)JSValueMakeNumber(env->ctx, v); return OK(env); }
NAPI_EXTERN napi_status napi_create_int32(napi_env env, int32_t v, napi_value *r) { ARG(env, r); *r = (napi_value)JSValueMakeNumber(env->ctx, v); return OK(env); }
NAPI_EXTERN napi_status napi_create_uint32(napi_env env, uint32_t v, napi_value *r) { ARG(env, r); *r = (napi_value)JSValueMakeNumber(env->ctx, v); return OK(env); }
NAPI_EXTERN napi_status napi_create_int64(napi_env env, int64_t v, napi_value *r) { ARG(env, r); *r = (napi_value)JSValueMakeNumber(env->ctx, (double)v); return OK(env); }

NAPI_EXTERN napi_status napi_typeof(napi_env env, napi_value value, napi_valuetype *r) {
    ARG(env, value && r);
    JSContextRef ctx = env->ctx;
    JSValueRef v = V(value);
    switch (JSValueGetType(ctx, v)) {
    case kJSTypeUndefined: *r = napi_undefined; break;
    case kJSTypeNull: *r = napi_null; break;
    case kJSTypeBoolean: *r = napi_boolean; break;
    case kJSTypeNumber: *r = napi_number; break;
    case kJSTypeString: *r = napi_string; break;
    case kJSTypeSymbol: *r = napi_symbol; break;
    case kJSTypeObject:
        if (JSValueIsObjectOfClass(ctx, v, tv_napi_external_class)) *r = napi_external;
        else if (JSObjectIsFunction(ctx, O(v))) *r = napi_function;
        else *r = napi_object;
        break;
    default: {
        JSValueRef args[1] = {v};
        JSValueRef b = tv_napi_call_helper(env, "isBigint", 1, args, NULL);
        *r = b && JSValueToBoolean(ctx, b) ? napi_bigint : napi_object;
    }
    }
    return OK(env);
}

static napi_status tv_napi_number(napi_env env, napi_value value, double *d) {
    ARG(env, value);
    if (!JSValueIsNumber(env->ctx, V(value))) return FAIL(env, napi_number_expected);
    *d = JSValueToNumber(env->ctx, V(value), NULL);
    return napi_ok;
}

NAPI_EXTERN napi_status napi_get_value_double(napi_env env, napi_value value, double *r) {
    ARG(env, r);
    napi_status s = tv_napi_number(env, value, r);
    return s == napi_ok ? OK(env) : s;
}

/* JavaScript's ToInt32 / ToUint32 of a number */
static uint32_t tv_napi_to_u32(double d) {
    if (!isfinite(d)) return 0;
    d = trunc(d);
    d = fmod(d, 4294967296.0);
    if (d < 0) d += 4294967296.0;
    return (uint32_t)d;
}

NAPI_EXTERN napi_status napi_get_value_int32(napi_env env, napi_value value, int32_t *r) {
    ARG(env, r);
    double d;
    napi_status s = tv_napi_number(env, value, &d);
    if (s != napi_ok) return s;
    *r = (int32_t)tv_napi_to_u32(d);
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_value_uint32(napi_env env, napi_value value, uint32_t *r) {
    ARG(env, r);
    double d;
    napi_status s = tv_napi_number(env, value, &d);
    if (s != napi_ok) return s;
    *r = tv_napi_to_u32(d);
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_value_int64(napi_env env, napi_value value, int64_t *r) {
    ARG(env, r);
    double d;
    napi_status s = tv_napi_number(env, value, &d);
    if (s != napi_ok) return s;
    if (!isfinite(d)) *r = 0;
    else if (d >= 9223372036854775807.0) *r = INT64_MAX;
    else if (d <= -9223372036854775808.0) *r = INT64_MIN;
    else *r = (int64_t)d;
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_value_bool(napi_env env, napi_value value, bool *r) {
    ARG(env, value && r);
    if (!JSValueIsBoolean(env->ctx, V(value))) return FAIL(env, napi_boolean_expected);
    *r = JSValueToBoolean(env->ctx, V(value));
    return OK(env);
}

/* ------------------------------------------------------------------ strings */

static napi_status tv_napi_make_string(napi_env env, JSStringRef s, napi_value *r) {
    *r = tv_napi_hold(JSValueMakeString(env->ctx, s));
    JSStringRelease(s);
    return OK(env);
}

NAPI_EXTERN napi_status napi_create_string_utf8(napi_env env, const char *str, size_t length, napi_value *r) {
    ARG(env, r && (str || length == 0));
    size_t n = length == NAPI_AUTO_LENGTH ? strlen(str) : length;
    *r = tv_napi_hold(tv_js_str(str ? str : "", n));
    return OK(env);
}

NAPI_EXTERN napi_status napi_create_string_latin1(napi_env env, const char *str, size_t length, napi_value *r) {
    ARG(env, r && (str || length == 0));
    size_t n = length == NAPI_AUTO_LENGTH ? strlen(str) : length;
#ifdef TOV_API_STRINGS
    *r = tv_napi_hold(TVMakeLatin1String(env->ctx, str ? str : "", n));
    return OK(env);
#else
    JSChar *u = malloc((n ? n : 1) * sizeof *u);
    if (!u) abort();
    for (size_t i = 0; i < n; i++) u[i] = (unsigned char)str[i];
    JSStringRef s = JSStringCreateWithCharacters(u, n);
    free(u);
    return tv_napi_make_string(env, s, r);
#endif
}

NAPI_EXTERN napi_status napi_create_string_utf16(napi_env env, const char16_t *str, size_t length, napi_value *r) {
    ARG(env, r && (str || length == 0));
    size_t n = length;
    if (n == NAPI_AUTO_LENGTH) {
        n = 0;
        while (str[n]) n++;
    }
    return tv_napi_make_string(env, JSStringCreateWithCharacters((const JSChar *)str, n), r);
}

NAPI_EXTERN napi_status node_api_create_external_string_latin1(napi_env env, char *str, size_t length, napi_finalize fin, void *hint, napi_value *r, bool *copied) {
    napi_status s = napi_create_string_latin1(env, str, length, r);
    if (copied) *copied = true;
    (void)fin; (void)hint;
    return s;
}

NAPI_EXTERN napi_status node_api_create_external_string_utf16(napi_env env, char16_t *str, size_t length, napi_finalize fin, void *hint, napi_value *r, bool *copied) {
    napi_status s = napi_create_string_utf16(env, str, length, r);
    if (copied) *copied = true;
    (void)fin; (void)hint;
    return s;
}

NAPI_EXTERN napi_status node_api_create_property_key_utf16(napi_env env, const char16_t *str, size_t length, napi_value *r) { return napi_create_string_utf16(env, str, length, r); }
NAPI_EXTERN napi_status node_api_create_property_key_latin1(napi_env env, const char *str, size_t length, napi_value *r) { return napi_create_string_latin1(env, str, length, r); }
NAPI_EXTERN napi_status node_api_create_property_key_utf8(napi_env env, const char *str, size_t length, napi_value *r) { return napi_create_string_utf8(env, str, length, r); }

/* UTF-16 of a string value (caller frees) */
static napi_status tv_napi_utf16(napi_env env, napi_value value, JSStringRef *out) {
    ARG(env, value);
    if (!JSValueIsString(env->ctx, V(value))) return FAIL(env, napi_string_expected);
    *out = JSValueToStringCopy(env->ctx, V(value), NULL);
    return napi_ok;
}

NAPI_EXTERN napi_status napi_get_value_string_utf8(napi_env env, napi_value value, char *buf, size_t bufsize, size_t *result) {
#ifdef TOV_API_STRINGS
    /* (straight from the engine when it all fits, or only its length is asked) */
    if (value) {
        size_t k = TVGetStringUTF8(env->ctx, V(value), buf, buf ? bufsize : 0);
        if (k != SIZE_MAX && (!buf || k < bufsize)) {
            if (buf) buf[k] = 0;
            if (result) *result = k;
            return OK(env);
        }
    }
#endif
    JSStringRef s;
    napi_status st = tv_napi_utf16(env, value, &s);
    if (st != napi_ok) return st;
    size_t len = JSStringGetLength(s);
    const JSChar *u = JSStringGetCharactersPtr(s);
    /* UTF-16 → UTF-8 (lone surrogates as U+FFFD), stopping before a character that doesn't fit */
    size_t o = 0, cap = buf ? (bufsize ? bufsize - 1 : 0) : SIZE_MAX;
    for (size_t i = 0; i < len; i++) {
        unsigned c = u[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < len && u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (u[i + 1] - 0xdc00);
            i++;
        } else if (c >= 0xd800 && c <= 0xdfff) {
            c = 0xfffd;
        }
        char b[4];
        size_t k;
        if (c < 0x80) { b[0] = (char)c; k = 1; }
        else if (c < 0x800) { b[0] = (char)(0xc0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3f)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xe0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3f)); b[2] = (char)(0x80 | (c & 0x3f)); k = 3; }
        else { b[0] = (char)(0xf0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3f)); b[2] = (char)(0x80 | ((c >> 6) & 0x3f)); b[3] = (char)(0x80 | (c & 0x3f)); k = 4; }
        if (o + k > cap) break;
        if (buf) memcpy(buf + o, b, k);
        o += k;
    }
    JSStringRelease(s);
    if (buf && bufsize) buf[o] = 0;
    if (result) *result = o;
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_value_string_latin1(napi_env env, napi_value value, char *buf, size_t bufsize, size_t *result) {
#ifdef TOV_API_STRINGS
    if (value) {
        size_t k = TVGetStringLatin1(env->ctx, V(value), buf, buf ? bufsize : 0);
        if (k != SIZE_MAX && (!buf || k < bufsize)) {
            if (buf) buf[k] = 0;
            if (result) *result = k;
            return OK(env);
        }
    }
#endif
    JSStringRef s;
    napi_status st = tv_napi_utf16(env, value, &s);
    if (st != napi_ok) return st;
    size_t len = JSStringGetLength(s);
    const JSChar *u = JSStringGetCharactersPtr(s);
    size_t n = buf ? (bufsize ? (len < bufsize - 1 ? len : bufsize - 1) : 0) : len;
    if (buf) {
        for (size_t i = 0; i < n; i++) buf[i] = (char)(u[i] & 0xff);
        if (bufsize) buf[n] = 0;
    }
    JSStringRelease(s);
    if (result) *result = n;
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_value_string_utf16(napi_env env, napi_value value, char16_t *buf, size_t bufsize, size_t *result) {
    JSStringRef s;
    napi_status st = tv_napi_utf16(env, value, &s);
    if (st != napi_ok) return st;
    size_t len = JSStringGetLength(s);
    size_t n = buf ? (bufsize ? (len < bufsize - 1 ? len : bufsize - 1) : 0) : len;
    if (buf) {
        memcpy(buf, JSStringGetCharactersPtr(s), n * sizeof(JSChar));
        if (bufsize) buf[n] = 0;
    }
    JSStringRelease(s);
    if (result) *result = n;
    return OK(env);
}

/* ------------------------------------------------------------------ symbols, bigints, dates */

NAPI_EXTERN napi_status napi_create_symbol(napi_env env, napi_value description, napi_value *r) {
    ARG(env, r);
    JSValueRef args[1] = {description ? V(description) : JSValueMakeUndefined(env->ctx)};
    *r = tv_napi_hold(tv_napi_call_helper(env, "symbol", 1, args, NULL));
    return OK(env);
}

NAPI_EXTERN napi_status node_api_symbol_for(napi_env env, const char *utf8description, size_t length, napi_value *r) {
    ARG(env, r);
    size_t n = length == NAPI_AUTO_LENGTH ? strlen(utf8description) : length;
    JSValueRef args[1] = {tv_js_str(utf8description, n)};
    *r = tv_napi_hold(tv_napi_call_helper(env, "symbolFor", 1, args, NULL));
    return OK(env);
}

static napi_status tv_napi_bigint_from(napi_env env, const char *digits, napi_value *r) {
    JSValueRef args[1] = {tv_js_str(digits, strlen(digits))};
    JSValueRef exc = NULL;
    JSValueRef v = tv_napi_call_helper(env, "bigint", 1, args, &exc);
    CHECK_EXC(env, exc);
    *r = tv_napi_hold(v);
    return OK(env);
}

NAPI_EXTERN napi_status napi_create_bigint_int64(napi_env env, int64_t value, napi_value *r) {
    ARG(env, r);
    char b[32];
    snprintf(b, sizeof b, "%lld", (long long)value);
    return tv_napi_bigint_from(env, b, r);
}

NAPI_EXTERN napi_status napi_create_bigint_uint64(napi_env env, uint64_t value, napi_value *r) {
    ARG(env, r);
    char b[32];
    snprintf(b, sizeof b, "%llu", (unsigned long long)value);
    return tv_napi_bigint_from(env, b, r);
}

NAPI_EXTERN napi_status napi_create_bigint_words(napi_env env, int sign_bit, size_t word_count, const uint64_t *words, napi_value *r) {
    ARG(env, r && (words || word_count == 0));
    JSValueRef *ws = malloc((word_count ? word_count : 1) * sizeof *ws);
    if (!ws) abort();
    for (size_t i = 0; i < word_count; i++) {
        char b[32];
        snprintf(b, sizeof b, "%llu", (unsigned long long)words[i]);
        ws[i] = tv_js_str(b, strlen(b));
    }
    JSValueRef args[2] = {JSValueMakeBoolean(env->ctx, sign_bit != 0), JSObjectMakeArray(env->ctx, word_count, ws, NULL)};
    free(ws);
    JSValueRef exc = NULL;
    JSValueRef v = tv_napi_call_helper(env, "bigintWords", 2, args, &exc);
    CHECK_EXC(env, exc);
    *r = tv_napi_hold(v);
    return OK(env);
}

static napi_status tv_napi_bigint_64(napi_env env, napi_value value, bool is_signed, char *out, size_t cap, bool *lossless) {
    ARG(env, value);
    JSValueRef args[2] = {V(value), JSValueMakeBoolean(env->ctx, is_signed)};
    JSValueRef isb = tv_napi_call_helper(env, "isBigint", 1, args, NULL);
    if (!isb || !JSValueToBoolean(env->ctx, isb)) return FAIL(env, napi_bigint_expected);
    JSValueRef s = tv_napi_call_helper(env, "bigintToString", 2, args, NULL);
    tv_str t = tv_js_to_str(s);
    char *bar = strchr(t.p->data, '|');
    if (lossless) *lossless = bar && bar[1] == '1';
    if (bar) *bar = 0;
    snprintf(out, cap, "%s", t.p->data);
    tv_str_release(t);
    return napi_ok;
}

NAPI_EXTERN napi_status napi_get_value_bigint_int64(napi_env env, napi_value value, int64_t *result, bool *lossless) {
    ARG(env, result);
    char b[64];
    napi_status s = tv_napi_bigint_64(env, value, true, b, sizeof b, lossless);
    if (s != napi_ok) return s;
    *result = strtoll(b, NULL, 10);
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_value_bigint_uint64(napi_env env, napi_value value, uint64_t *result, bool *lossless) {
    ARG(env, result);
    char b[64];
    napi_status s = tv_napi_bigint_64(env, value, false, b, sizeof b, lossless);
    if (s != napi_ok) return s;
    *result = strtoull(b, NULL, 10);
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_value_bigint_words(napi_env env, napi_value value, int *sign_bit, size_t *word_count, uint64_t *words) {
    ARG(env, value && word_count);
    JSValueRef args[1] = {V(value)};
    JSValueRef isb = tv_napi_call_helper(env, "isBigint", 1, args, NULL);
    if (!isb || !JSValueToBoolean(env->ctx, isb)) return FAIL(env, napi_bigint_expected);
    JSObjectRef info = (JSObjectRef)tv_napi_call_helper(env, "bigintInfo", 1, args, NULL);
    bool neg = JSValueToBoolean(env->ctx, JSObjectGetPropertyAtIndex(env->ctx, info, 0, NULL));
    JSObjectRef ws = (JSObjectRef)JSObjectGetPropertyAtIndex(env->ctx, info, 1, NULL);
    size_t n = tv_js_length(ws);
    if (!words) {
        *word_count = n;
        if (sign_bit) *sign_bit = neg;
        return OK(env);
    }
    size_t m = n < *word_count ? n : *word_count;
    for (size_t i = 0; i < m; i++) {
        tv_str t = tv_js_to_str(JSObjectGetPropertyAtIndex(env->ctx, ws, (unsigned)i, NULL));
        words[i] = strtoull(t.p->data, NULL, 10);
        tv_str_release(t);
    }
    *word_count = m;
    if (sign_bit) *sign_bit = neg;
    return OK(env);
}

NAPI_EXTERN napi_status napi_create_date(napi_env env, double time, napi_value *r) {
    ARG(env, r);
    JSValueRef args[1] = {JSValueMakeNumber(env->ctx, time)};
    *r = tv_napi_hold(tv_napi_call_helper(env, "date", 1, args, NULL));
    return OK(env);
}

NAPI_EXTERN napi_status napi_is_date(napi_env env, napi_value value, bool *r) {
    ARG(env, value && r);
    JSValueRef args[1] = {V(value)};
    *r = JSValueToBoolean(env->ctx, tv_napi_call_helper(env, "isDate", 1, args, NULL));
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_date_value(napi_env env, napi_value value, double *r) {
    bool is;
    napi_status s = napi_is_date(env, value, &is);
    if (s != napi_ok) return s;
    if (!is) return FAIL(env, napi_date_expected);
    JSValueRef args[1] = {V(value)};
    *r = JSValueToNumber(env->ctx, tv_napi_call_helper(env, "dateValue", 1, args, NULL), NULL);
    return OK(env);
}

/* ------------------------------------------------------------------ objects and properties */

NAPI_EXTERN napi_status napi_create_object(napi_env env, napi_value *r) { ARG(env, r); *r = tv_napi_hold(JSObjectMake(env->ctx, NULL, NULL)); return OK(env); }
NAPI_EXTERN napi_status napi_create_array(napi_env env, napi_value *r) { ARG(env, r); *r = tv_napi_hold(JSObjectMakeArray(env->ctx, 0, NULL, NULL)); return OK(env); }

NAPI_EXTERN napi_status napi_create_array_with_length(napi_env env, size_t length, napi_value *r) {
    ARG(env, r);
    JSObjectRef a = JSObjectMakeArray(env->ctx, 0, NULL, NULL);
    JSStringRef k = JSStringCreateWithUTF8CString("length");
    JSObjectSetProperty(env->ctx, a, k, JSValueMakeNumber(env->ctx, (double)length), 0, NULL);
    JSStringRelease(k);
    *r = tv_napi_hold(a);
    return OK(env);
}

static napi_status tv_napi_object(napi_env env, napi_value v, JSObjectRef *o) {
    ARG(env, v);
    JSValueRef exc = NULL;
    JSValueRef val = V(v);
    if (!JSValueIsObject(env->ctx, val)) {
        if (JSValueIsUndefined(env->ctx, val) || JSValueIsNull(env->ctx, val)) return FAIL(env, napi_object_expected);
        *o = JSValueToObject(env->ctx, val, &exc);
        CHECK_EXC(env, exc);
        return napi_ok;
    }
    *o = O(val);
    return napi_ok;
}

NAPI_EXTERN napi_status napi_set_property(napi_env env, napi_value object, napi_value key, napi_value value) {
    ARG(env, key && value);
    JSObjectRef o;
    napi_status s = tv_napi_object(env, object, &o);
    if (s != napi_ok) return s;
    JSValueRef exc = NULL;
    JSObjectSetPropertyForKey(env->ctx, o, V(key), V(value), kJSPropertyAttributeNone, &exc);
    CHECK_EXC(env, exc);
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_property(napi_env env, napi_value object, napi_value key, napi_value *r) {
    ARG(env, key && r);
    JSObjectRef o;
    napi_status s = tv_napi_object(env, object, &o);
    if (s != napi_ok) return s;
    JSValueRef exc = NULL;
    JSValueRef v = JSObjectGetPropertyForKey(env->ctx, o, V(key), &exc);
    CHECK_EXC(env, exc);
    *r = tv_napi_hold(v);
    return OK(env);
}

NAPI_EXTERN napi_status napi_has_property(napi_env env, napi_value object, napi_value key, bool *r) {
    ARG(env, key && r);
    JSObjectRef o;
    napi_status s = tv_napi_object(env, object, &o);
    if (s != napi_ok) return s;
    JSValueRef exc = NULL;
    *r = JSObjectHasPropertyForKey(env->ctx, o, V(key), &exc);
    CHECK_EXC(env, exc);
    return OK(env);
}

NAPI_EXTERN napi_status napi_delete_property(napi_env env, napi_value object, napi_value key, bool *r) {
    ARG(env, key);
    JSObjectRef o;
    napi_status s = tv_napi_object(env, object, &o);
    if (s != napi_ok) return s;
    JSValueRef exc = NULL;
    bool ok = JSObjectDeletePropertyForKey(env->ctx, o, V(key), &exc);
    CHECK_EXC(env, exc);
    if (r) *r = ok;
    return OK(env);
}

NAPI_EXTERN napi_status napi_has_own_property(napi_env env, napi_value object, napi_value key, bool *r) {
    ARG(env, key && r);
    JSObjectRef o;
    napi_status s = tv_napi_object(env, object, &o);
    if (s != napi_ok) return s;
    JSValueRef args[2] = {o, V(key)};
    JSValueRef exc = NULL;
    JSValueRef v = tv_napi_eval(env->ctx, "(function (o, k) { return Object.prototype.hasOwnProperty.call(o, k); })");
    v = JSObjectCallAsFunction(env->ctx, O(v), NULL, 2, args, &exc);
    CHECK_EXC(env, exc);
    *r = JSValueToBoolean(env->ctx, v);
    return OK(env);
}

static JSValueRef tv_napi_name(napi_env env, const char *utf8name) { return tv_js_str(utf8name, strlen(utf8name)); }

NAPI_EXTERN napi_status napi_set_named_property(napi_env env, napi_value object, const char *utf8name, napi_value value) {
    ARG(env, utf8name);
    return napi_set_property(env, object, (napi_value)tv_napi_name(env, utf8name), value);
}

NAPI_EXTERN napi_status napi_get_named_property(napi_env env, napi_value object, const char *utf8name, napi_value *r) {
    ARG(env, utf8name);
    return napi_get_property(env, object, (napi_value)tv_napi_name(env, utf8name), r);
}

NAPI_EXTERN napi_status napi_has_named_property(napi_env env, napi_value object, const char *utf8name, bool *r) {
    ARG(env, utf8name);
    return napi_has_property(env, object, (napi_value)tv_napi_name(env, utf8name), r);
}

NAPI_EXTERN napi_status napi_set_element(napi_env env, napi_value object, uint32_t index, napi_value value) {
    ARG(env, value);
    JSObjectRef o;
    napi_status s = tv_napi_object(env, object, &o);
    if (s != napi_ok) return s;
    JSValueRef exc = NULL;
    JSObjectSetPropertyAtIndex(env->ctx, o, index, V(value), &exc);
    CHECK_EXC(env, exc);
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_element(napi_env env, napi_value object, uint32_t index, napi_value *r) {
    ARG(env, r);
    JSObjectRef o;
    napi_status s = tv_napi_object(env, object, &o);
    if (s != napi_ok) return s;
    JSValueRef exc = NULL;
    JSValueRef v = JSObjectGetPropertyAtIndex(env->ctx, o, index, &exc);
    CHECK_EXC(env, exc);
    *r = tv_napi_hold(v);
    return OK(env);
}

NAPI_EXTERN napi_status napi_has_element(napi_env env, napi_value object, uint32_t index, bool *r) {
    return napi_has_property(env, object, (napi_value)JSValueMakeNumber(env->ctx, index), r);
}

NAPI_EXTERN napi_status napi_delete_element(napi_env env, napi_value object, uint32_t index, bool *r) {
    return napi_delete_property(env, object, (napi_value)JSValueMakeNumber(env->ctx, index), r);
}

NAPI_EXTERN napi_status napi_is_array(napi_env env, napi_value value, bool *r) {
    ARG(env, value && r);
    *r = JSValueIsArray(env->ctx, V(value));
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_array_length(napi_env env, napi_value value, uint32_t *r) {
    ARG(env, value && r);
    if (!JSValueIsArray(env->ctx, V(value))) return FAIL(env, napi_array_expected);
    *r = tv_js_length(V(value));
    return OK(env);
}

static napi_status tv_napi_keys(napi_env env, napi_value object, bool own, bool enum_only, bool skip_strings, bool skip_symbols, bool nums_to_strings, napi_value *r) {
    ARG(env, r);
    JSObjectRef o;
    napi_status s = tv_napi_object(env, object, &o);
    if (s != napi_ok) return s;
    JSContextRef ctx = env->ctx;
    JSValueRef args[6] = {o, JSValueMakeBoolean(ctx, own), JSValueMakeBoolean(ctx, enum_only), JSValueMakeBoolean(ctx, skip_strings), JSValueMakeBoolean(ctx, skip_symbols), JSValueMakeBoolean(ctx, nums_to_strings)};
    JSValueRef exc = NULL;
    JSValueRef v = tv_napi_call_helper(env, "keys", 6, args, &exc);
    CHECK_EXC(env, exc);
    *r = tv_napi_hold(v);
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_property_names(napi_env env, napi_value object, napi_value *r) {
    /* (for...in: enumerable string keys, the prototype chain's too, numbers as strings) */
    return tv_napi_keys(env, object, false, true, false, true, true, r);
}

NAPI_EXTERN napi_status napi_get_all_property_names(napi_env env, napi_value object, napi_key_collection_mode mode, napi_key_filter filter, napi_key_conversion conversion, napi_value *r) {
    return tv_napi_keys(env, object, mode == napi_key_own_only, (filter & napi_key_enumerable) != 0, (filter & napi_key_skip_strings) != 0, (filter & napi_key_skip_symbols) != 0, conversion == napi_key_numbers_to_strings, r);
}

NAPI_EXTERN napi_status napi_get_prototype(napi_env env, napi_value object, napi_value *r) {
    ARG(env, r);
    JSObjectRef o;
    napi_status s = tv_napi_object(env, object, &o);
    if (s != napi_ok) return s;
    *r = tv_napi_hold(JSObjectGetPrototype(env->ctx, o));
    return OK(env);
}

NAPI_EXTERN napi_status napi_object_freeze(napi_env env, napi_value object) {
    JSValueRef args[1] = {V(object)};
    JSValueRef exc = NULL;
    tv_napi_call_helper(env, "freeze", 1, args, &exc);
    CHECK_EXC(env, exc);
    return OK(env);
}

NAPI_EXTERN napi_status napi_object_seal(napi_env env, napi_value object) {
    JSValueRef args[1] = {V(object)};
    JSValueRef exc = NULL;
    tv_napi_call_helper(env, "seal", 1, args, &exc);
    CHECK_EXC(env, exc);
    return OK(env);
}

static napi_status tv_napi_define(napi_env env, JSObjectRef o, const napi_property_descriptor *p) {
    JSContextRef ctx = env->ctx;
    JSValueRef key = p->name ? V(p->name) : tv_js_str(p->utf8name, strlen(p->utf8name));
    bool e = (p->attributes & napi_enumerable) != 0, c = (p->attributes & napi_configurable) != 0, w = (p->attributes & napi_writable) != 0;
    JSValueRef exc = NULL;
    if (p->getter || p->setter) {
        JSValueRef get = p->getter ? tv_napi_make_fn(env, NULL, 0, p->getter, p->data, false) : JSValueMakeUndefined(ctx);
        JSValueRef set = p->setter ? tv_napi_make_fn(env, NULL, 0, p->setter, p->data, false) : JSValueMakeUndefined(ctx);
        JSValueRef args[6] = {o, key, get, set, JSValueMakeBoolean(ctx, e), JSValueMakeBoolean(ctx, c)};
        tv_napi_call_helper(env, "accessor", 6, args, &exc);
    } else {
        JSValueRef value;
        if (p->method) {
            char name[256] = "";
            if (p->utf8name) snprintf(name, sizeof name, "%s", p->utf8name);
            value = tv_napi_make_fn(env, p->utf8name ? name : NULL, NAPI_AUTO_LENGTH, p->method, p->data, false);
        } else {
            value = p->value ? V(p->value) : JSValueMakeUndefined(ctx);
        }
        JSValueRef args[6] = {o, key, value, JSValueMakeBoolean(ctx, w), JSValueMakeBoolean(ctx, e), JSValueMakeBoolean(ctx, c)};
        tv_napi_call_helper(env, "value", 6, args, &exc);
    }
    CHECK_EXC(env, exc);
    return napi_ok;
}

NAPI_EXTERN napi_status napi_define_properties(napi_env env, napi_value object, size_t property_count, const napi_property_descriptor *properties) {
    ARG(env, property_count == 0 || properties);
    JSObjectRef o;
    napi_status s = tv_napi_object(env, object, &o);
    if (s != napi_ok) return s;
    for (size_t i = 0; i < property_count; i++) {
        s = tv_napi_define(env, o, &properties[i]);
        if (s != napi_ok) return s;
    }
    return OK(env);
}

NAPI_EXTERN napi_status napi_define_class(napi_env env, const char *utf8name, size_t length, napi_callback constructor, void *data, size_t property_count, const napi_property_descriptor *properties, napi_value *r) {
    ARG(env, utf8name && constructor && r && (property_count == 0 || properties));
    JSContextRef ctx = env->ctx;
    size_t n = length == NAPI_AUTO_LENGTH ? strlen(utf8name) : length;
    JSObjectRef impl = tv_napi_make_fn(env, NULL, 0, constructor, data, true);
    JSValueRef args[2] = {impl, tv_js_str(utf8name, n)};
    JSValueRef exc = NULL;
    JSValueRef cls = tv_napi_call_helper(env, "makeClass", 2, args, &exc);
    CHECK_EXC(env, exc);
    tv_js_retain(cls);
    /* (the implementation lives as long as the class) */
    JSValueRef hidden[3] = {cls, tv_js_str("__napi_impl", 11), impl};
    tv_napi_call_helper(env, "setHidden", 3, hidden, NULL);
    JSStringRef kp = JSStringCreateWithUTF8CString("prototype");
    JSObjectRef proto = (JSObjectRef)JSObjectGetProperty(ctx, (JSObjectRef)cls, kp, NULL);
    JSStringRelease(kp);
    for (size_t i = 0; i < property_count; i++) {
        const napi_property_descriptor *p = &properties[i];
        napi_status s = tv_napi_define(env, (p->attributes & napi_static) ? (JSObjectRef)cls : proto, p);
        if (s != napi_ok) {
            tv_js_release(cls);
            return s;
        }
    }
    *r = tv_napi_hold(cls);
    tv_js_release(cls);
    return OK(env);
}

NAPI_EXTERN napi_status napi_strict_equals(napi_env env, napi_value a, napi_value b, bool *r) {
    ARG(env, a && b && r);
    *r = JSValueIsStrictEqual(env->ctx, V(a), V(b));
    return OK(env);
}

NAPI_EXTERN napi_status napi_instanceof(napi_env env, napi_value object, napi_value constructor, bool *r) {
    ARG(env, object && constructor && r);
    if (!JSValueIsObject(env->ctx, V(constructor)) || !JSObjectIsFunction(env->ctx, O(constructor))) return FAIL(env, napi_function_expected);
    JSValueRef exc = NULL;
    *r = JSValueIsInstanceOfConstructor(env->ctx, V(object), O(constructor), &exc);
    CHECK_EXC(env, exc);
    return OK(env);
}

/* ------------------------------------------------------------------ calls */

NAPI_EXTERN napi_status napi_call_function(napi_env env, napi_value recv, napi_value func, size_t argc, const napi_value *argv, napi_value *r) {
    ARG(env, func && (argc == 0 || argv));
    if (env->pending) return FAIL(env, napi_pending_exception);
    if (!JSValueIsObject(env->ctx, V(func)) || !JSObjectIsFunction(env->ctx, O(func))) return FAIL(env, napi_function_expected);
    JSObjectRef self = recv && JSValueIsObject(env->ctx, V(recv)) ? O(recv) : NULL;
    JSValueRef exc = NULL;
    JSValueRef v = JSObjectCallAsFunction(env->ctx, O(func), self, argc, (const JSValueRef *)argv, &exc);
    CHECK_EXC(env, exc);
    if (r) *r = tv_napi_hold(v);
    return OK(env);
}

NAPI_EXTERN napi_status napi_new_instance(napi_env env, napi_value constructor, size_t argc, const napi_value *argv, napi_value *r) {
    ARG(env, constructor && r && (argc == 0 || argv));
    if (env->pending) return FAIL(env, napi_pending_exception);
    JSValueRef arr = JSObjectMakeArray(env->ctx, argc, (const JSValueRef *)argv, NULL);
    JSValueRef args[2] = {V(constructor), arr};
    JSValueRef exc = NULL;
    JSValueRef v = tv_napi_call_helper(env, "newInstance", 2, args, &exc);
    CHECK_EXC(env, exc);
    *r = tv_napi_hold(v);
    return OK(env);
}

NAPI_EXTERN napi_status napi_make_callback(napi_env env, napi_async_context ctx_, napi_value recv, napi_value func, size_t argc, const napi_value *argv, napi_value *r) {
    (void)ctx_;
    napi_status s = napi_call_function(env, recv, func, argc, argv, r);
    tv_js_drain();
    return s;
}

/* ------------------------------------------------------------------ errors */

NAPI_EXTERN napi_status napi_get_last_error_info(napi_env env, const napi_extended_error_info **r) {
    if (!env || !r) return napi_invalid_arg;
    *r = &env->error;
    return napi_ok;
}

NAPI_EXTERN napi_status napi_throw(napi_env env, napi_value error) {
    ARG(env, error);
    if (env->pending) tv_js_release(env->pending);
    env->pending = tv_js_retain(V(error));
    return OK(env);
}

static napi_status tv_napi_make_error(napi_env env, const char *ctor, napi_value code, napi_value msg, napi_value *r) {
    ARG(env, msg && r);
    if (!JSValueIsString(env->ctx, V(msg))) return FAIL(env, napi_string_expected);
    JSValueRef c = tv_napi_eval(env->ctx, ctor);
    JSValueRef args[3] = {c, code ? V(code) : JSValueMakeUndefined(env->ctx), V(msg)};
    *r = tv_napi_hold(tv_napi_call_helper(env, "error", 3, args, NULL));
    return OK(env);
}

NAPI_EXTERN napi_status napi_create_error(napi_env env, napi_value code, napi_value msg, napi_value *r) { return tv_napi_make_error(env, "Error", code, msg, r); }
NAPI_EXTERN napi_status napi_create_type_error(napi_env env, napi_value code, napi_value msg, napi_value *r) { return tv_napi_make_error(env, "TypeError", code, msg, r); }
NAPI_EXTERN napi_status napi_create_range_error(napi_env env, napi_value code, napi_value msg, napi_value *r) { return tv_napi_make_error(env, "RangeError", code, msg, r); }
NAPI_EXTERN napi_status node_api_create_syntax_error(napi_env env, napi_value code, napi_value msg, napi_value *r) { return tv_napi_make_error(env, "SyntaxError", code, msg, r); }

static napi_status tv_napi_throw_new(napi_env env, const char *ctor, const char *code, const char *msg) {
    napi_value c = code ? (napi_value)tv_js_str(code, strlen(code)) : NULL;
    napi_value m = (napi_value)tv_js_str(msg ? msg : "", msg ? strlen(msg) : 0);
    napi_value e;
    napi_status s = tv_napi_make_error(env, ctor, c, m, &e);
    if (s != napi_ok) return s;
    return napi_throw(env, e);
}

NAPI_EXTERN napi_status napi_throw_error(napi_env env, const char *code, const char *msg) { return tv_napi_throw_new(env, "Error", code, msg); }
NAPI_EXTERN napi_status napi_throw_type_error(napi_env env, const char *code, const char *msg) { return tv_napi_throw_new(env, "TypeError", code, msg); }
NAPI_EXTERN napi_status napi_throw_range_error(napi_env env, const char *code, const char *msg) { return tv_napi_throw_new(env, "RangeError", code, msg); }
NAPI_EXTERN napi_status node_api_throw_syntax_error(napi_env env, const char *code, const char *msg) { return tv_napi_throw_new(env, "SyntaxError", code, msg); }

NAPI_EXTERN napi_status napi_is_error(napi_env env, napi_value value, bool *r) {
    ARG(env, value && r);
    JSValueRef args[1] = {V(value)};
    *r = JSValueToBoolean(env->ctx, tv_napi_call_helper(env, "isError", 1, args, NULL));
    return OK(env);
}

NAPI_EXTERN napi_status napi_is_exception_pending(napi_env env, bool *r) {
    ARG(env, r);
    *r = env->pending != NULL;
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_and_clear_last_exception(napi_env env, napi_value *r) {
    ARG(env, r);
    if (!env->pending) {
        *r = (napi_value)JSValueMakeUndefined(env->ctx);
        return OK(env);
    }
    *r = tv_napi_hold(env->pending);
    tv_js_release(env->pending);
    env->pending = NULL;
    return OK(env);
}

NAPI_EXTERN void napi_fatal_error(const char *location, size_t location_len, const char *message, size_t message_len) {
    fprintf(stderr, "FATAL ERROR: %.*s %.*s\n", (int)(location ? (location_len == NAPI_AUTO_LENGTH ? strlen(location) : location_len) : 0), location ? location : "",
            (int)(message ? (message_len == NAPI_AUTO_LENGTH ? strlen(message) : message_len) : 0), message ? message : "");
    abort();
}

NAPI_EXTERN napi_status napi_fatal_exception(napi_env env, napi_value err) {
    tv_str t = tv_js_error_text(V(err));
    fprintf(stderr, "Uncaught %s\n", t.p->data);
    (void)env;
    exit(7);
}

/* ------------------------------------------------------------------ coercion */

static napi_status tv_napi_coerce(napi_env env, const char *helper, napi_value v, napi_value *r) {
    ARG(env, v && r);
    JSValueRef args[1] = {V(v)};
    JSValueRef exc = NULL;
    JSValueRef out = tv_napi_call_helper(env, helper, 1, args, &exc);
    CHECK_EXC(env, exc);
    *r = tv_napi_hold(out);
    return OK(env);
}

NAPI_EXTERN napi_status napi_coerce_to_bool(napi_env env, napi_value v, napi_value *r) { return tv_napi_coerce(env, "coerceBool", v, r); }
NAPI_EXTERN napi_status napi_coerce_to_number(napi_env env, napi_value v, napi_value *r) { return tv_napi_coerce(env, "coerceNumber", v, r); }
NAPI_EXTERN napi_status napi_coerce_to_string(napi_env env, napi_value v, napi_value *r) { return tv_napi_coerce(env, "coerceString", v, r); }
NAPI_EXTERN napi_status napi_coerce_to_object(napi_env env, napi_value v, napi_value *r) { return tv_napi_coerce(env, "coerceObject", v, r); }

/* ------------------------------------------------------------------ scopes */

NAPI_EXTERN napi_status napi_open_handle_scope(napi_env env, napi_handle_scope *r) {
    ARG(env, r);
    *r = (napi_handle_scope)(uintptr_t)(tv_napi_nhandles + 1);
    return OK(env);
}

NAPI_EXTERN napi_status napi_close_handle_scope(napi_env env, napi_handle_scope scope) {
    ARG(env, scope);
    size_t mark = (size_t)(uintptr_t)scope - 1;
    if (mark > tv_napi_nhandles) return FAIL(env, napi_handle_scope_mismatch);
    tv_napi_release_to(mark);
    return OK(env);
}

/* an escapable scope: escaping keeps the value rooted past the scope (in the parent's list) */
typedef struct {
    size_t mark;
    bool escaped;
} tv_napi_escapable;

NAPI_EXTERN napi_status napi_open_escapable_handle_scope(napi_env env, napi_escapable_handle_scope *r) {
    ARG(env, r);
    tv_napi_escapable *s = malloc(sizeof *s);
    if (!s) abort();
    s->mark = tv_napi_nhandles;
    s->escaped = false;
    *r = (napi_escapable_handle_scope)s;
    return OK(env);
}

NAPI_EXTERN napi_status napi_close_escapable_handle_scope(napi_env env, napi_escapable_handle_scope scope) {
    ARG(env, scope);
    tv_napi_escapable *s = (tv_napi_escapable *)scope;
    if (s->mark > tv_napi_nhandles) return FAIL(env, napi_handle_scope_mismatch);
    /* (escaped values were moved below the mark) */
    tv_napi_release_to(s->mark + (s->escaped ? 1 : 0));
    free(s);
    return OK(env);
}

NAPI_EXTERN napi_status napi_escape_handle(napi_env env, napi_escapable_handle_scope scope, napi_value escapee, napi_value *r) {
    ARG(env, scope && escapee && r);
    tv_napi_escapable *s = (tv_napi_escapable *)scope;
    if (s->escaped) return FAIL(env, napi_escape_called_twice);
    s->escaped = true;
    /* root it at the scope's mark (shift the scope's handles up by one) */
    JSValueRef v = V(escapee);
    if (tv_js_is_cell(v)) {
        tv_napi_hold(v);
        memmove(tv_napi_handles + s->mark + 1, tv_napi_handles + s->mark, (tv_napi_nhandles - 1 - s->mark) * sizeof *tv_napi_handles);
        tv_napi_handles[s->mark] = v;
    } else {
        s->escaped = false;
    }
    *r = escapee;
    return OK(env);
}

NAPI_EXTERN napi_status napi_open_callback_scope(napi_env env, napi_value resource, napi_async_context ctx_, napi_callback_scope *r) {
    (void)resource; (void)ctx_;
    ARG(env, r);
    *r = (napi_callback_scope)(uintptr_t)1;
    return OK(env);
}

NAPI_EXTERN napi_status napi_close_callback_scope(napi_env env, napi_callback_scope scope) {
    (void)scope;
    tv_js_drain();
    return OK(env);
}

NAPI_EXTERN napi_status napi_async_init(napi_env env, napi_value resource, napi_value name, napi_async_context *r) {
    (void)resource; (void)name;
    ARG(env, r);
    *r = (napi_async_context)(uintptr_t)1;
    return OK(env);
}

NAPI_EXTERN napi_status napi_async_destroy(napi_env env, napi_async_context ctx_) {
    (void)ctx_;
    return OK(env);
}

/* ------------------------------------------------------------------ references */

struct napi_ref__ {
    napi_env env;
    JSValueRef value;          /* strong (rooted) while count > 0; else a WeakRef for objects */
    JSValueRef weak;
    uint32_t count;
};

NAPI_EXTERN napi_status napi_create_reference(napi_env env, napi_value value, uint32_t initial, napi_ref *r) {
    ARG(env, value && r);
    struct napi_ref__ *ref = calloc(1, sizeof *ref);
    if (!ref) abort();
    ref->env = env;
    ref->count = initial;
    JSValueRef v = V(value);
    if (initial > 0 || !tv_js_is_cell(v)) {
        ref->value = tv_js_retain(v);
    } else {
        JSValueRef args[1] = {v};
        JSValueRef w = JSValueIsObject(env->ctx, v) ? tv_napi_call_helper(env, "weakRef", 1, args, NULL) : NULL;
        ref->weak = w ? tv_js_retain(w) : NULL;
        if (!w) ref->value = tv_js_retain(v);
    }
    *r = ref;
    return OK(env);
}

NAPI_EXTERN napi_status napi_delete_reference(napi_env env, napi_ref ref) {
    ARG(env, ref);
    if (ref->value) tv_js_release(ref->value);
    if (ref->weak) tv_js_release(ref->weak);
    free(ref);
    return OK(env);
}

NAPI_EXTERN napi_status napi_reference_ref(napi_env env, napi_ref ref, uint32_t *r) {
    ARG(env, ref);
    if (ref->count == 0 && !ref->value && ref->weak) {
        JSValueRef args[1] = {ref->weak};
        JSValueRef v = tv_napi_call_helper(env, "deref", 1, args, NULL);
        if (v && !JSValueIsUndefined(env->ctx, v)) ref->value = tv_js_retain(v);
    }
    ref->count++;
    if (r) *r = ref->count;
    return OK(env);
}

NAPI_EXTERN napi_status napi_reference_unref(napi_env env, napi_ref ref, uint32_t *r) {
    ARG(env, ref);
    if (ref->count == 0) return FAIL(env, napi_generic_failure);
    ref->count--;
    if (ref->count == 0 && ref->value && tv_js_is_cell(ref->value) && JSValueIsObject(env->ctx, ref->value)) {
        if (!ref->weak) {
            JSValueRef args[1] = {ref->value};
            JSValueRef w = tv_napi_call_helper(env, "weakRef", 1, args, NULL);
            if (w) ref->weak = tv_js_retain(w);
        }
        if (ref->weak) {
            tv_js_release(ref->value);
            ref->value = NULL;
        }
    }
    if (r) *r = ref->count;
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_reference_value(napi_env env, napi_ref ref, napi_value *r) {
    ARG(env, ref && r);
    if (ref->value) {
        *r = tv_napi_hold(ref->value);
    } else if (ref->weak) {
        JSValueRef args[1] = {ref->weak};
        JSValueRef v = tv_napi_call_helper(env, "deref", 1, args, NULL);
        *r = v && !JSValueIsUndefined(env->ctx, v) ? tv_napi_hold(v) : NULL;
    } else {
        *r = NULL;
    }
    return OK(env);
}

/* ------------------------------------------------------------------ wrap, external, finalizers, tags */

static tv_napi_box *tv_napi_hidden_box(napi_env env, JSValueRef obj, JSValueRef key) {
    JSValueRef args[2] = {obj, key};
    JSValueRef h = tv_napi_call_helper(env, "getHidden", 2, args, NULL);
    if (!h || !JSValueIsObjectOfClass(env->ctx, h, tv_napi_box_class)) return NULL;
    return JSObjectGetPrivate((JSObjectRef)h);
}

NAPI_EXTERN napi_status napi_wrap(napi_env env, napi_value js_object, void *native, napi_finalize fin, void *hint, napi_ref *result) {
    ARG(env, js_object);
    if (!JSValueIsObject(env->ctx, V(js_object))) return FAIL(env, napi_object_expected);
    if (tv_napi_hidden_box(env, V(js_object), tv_napi_wrap_key)) return FAIL(env, napi_invalid_arg);
    JSObjectRef box = tv_napi_new_box(env, native, fin, hint);
    ((tv_napi_box *)JSObjectGetPrivate(box))->is_wrap = true;
    JSValueRef args[3] = {V(js_object), tv_napi_wrap_key, box};
    tv_napi_call_helper(env, "setHidden", 3, args, NULL);
    if (result) return napi_create_reference(env, js_object, 0, result);
    return OK(env);
}

NAPI_EXTERN napi_status napi_unwrap(napi_env env, napi_value js_object, void **r) {
    ARG(env, js_object && r);
    if (!JSValueIsObject(env->ctx, V(js_object))) return FAIL(env, napi_object_expected);
    tv_napi_box *b = tv_napi_hidden_box(env, V(js_object), tv_napi_wrap_key);
    if (!b) return FAIL(env, napi_invalid_arg);
    *r = b->data;
    return OK(env);
}

NAPI_EXTERN napi_status napi_remove_wrap(napi_env env, napi_value js_object, void **r) {
    ARG(env, js_object);
    tv_napi_box *b = tv_napi_hidden_box(env, V(js_object), tv_napi_wrap_key);
    if (!b) return FAIL(env, napi_invalid_arg);
    if (r) *r = b->data;
    b->finalize = NULL;   /* removed: its finalizer won't run */
    JSValueRef args[2] = {V(js_object), tv_napi_wrap_key};
    tv_napi_call_helper(env, "delHidden", 2, args, NULL);
    return OK(env);
}

NAPI_EXTERN napi_status napi_create_external(napi_env env, void *data, napi_finalize fin, void *hint, napi_value *r) {
    ARG(env, r);
    tv_napi_box *b = calloc(1, sizeof *b);
    if (!b) abort();
    b->env = env;
    b->data = data;
    b->finalize = fin;
    b->hint = hint;
    *r = tv_napi_hold(JSObjectMake(env->ctx, tv_napi_external_class, b));
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_value_external(napi_env env, napi_value value, void **r) {
    ARG(env, value && r);
    if (!JSValueIsObjectOfClass(env->ctx, V(value), tv_napi_external_class)) return FAIL(env, napi_invalid_arg);
    tv_napi_box *b = JSObjectGetPrivate(O(value));
    *r = b ? b->data : NULL;
    return OK(env);
}

NAPI_EXTERN napi_status napi_add_finalizer(napi_env env, napi_value js_object, void *data, napi_finalize fin, void *hint, napi_ref *result) {
    ARG(env, js_object && fin);
    if (!JSValueIsObject(env->ctx, V(js_object))) return FAIL(env, napi_object_expected);
    JSValueRef args[3] = {V(js_object), tv_napi_finalizers_key, tv_napi_new_box(env, data, fin, hint)};
    tv_napi_call_helper(env, "addFinalizer", 3, args, NULL);
    if (result) return napi_create_reference(env, js_object, 0, result);
    return OK(env);
}

NAPI_EXTERN napi_status node_api_post_finalizer(napi_env env, napi_finalize fin, void *data, void *hint) {
    ARG(env, fin);
    tv_napi_post(tv_napi_finalize_job, (void *)fin, env, data, hint);
    return OK(env);
}

NAPI_EXTERN napi_status napi_type_tag_object(napi_env env, napi_value value, const napi_type_tag *tag) {
    ARG(env, value && tag);
    if (!JSValueIsObject(env->ctx, V(value))) return FAIL(env, napi_object_expected);
    if (tv_napi_hidden_box(env, V(value), tv_napi_tag_key)) return FAIL(env, napi_invalid_arg);
    JSObjectRef box = tv_napi_new_box(env, NULL, NULL, NULL);
    tv_napi_box *b = JSObjectGetPrivate(box);
    b->tag = *tag;
    b->tagged = true;
    JSValueRef args[3] = {V(value), tv_napi_tag_key, box};
    tv_napi_call_helper(env, "setHidden", 3, args, NULL);
    return OK(env);
}

NAPI_EXTERN napi_status napi_check_object_type_tag(napi_env env, napi_value value, const napi_type_tag *tag, bool *r) {
    ARG(env, value && tag && r);
    tv_napi_box *b = JSValueIsObject(env->ctx, V(value)) ? tv_napi_hidden_box(env, V(value), tv_napi_tag_key) : NULL;
    *r = b && b->tagged && b->tag.lower == tag->lower && b->tag.upper == tag->upper;
    return OK(env);
}

/* ------------------------------------------------------------------ instance data, cleanup, versions */

NAPI_EXTERN napi_status napi_set_instance_data(napi_env env, void *data, napi_finalize fin, void *hint) {
    ARG(env, true);
    env->instance_data = data;
    env->instance_finalize = fin;
    env->instance_hint = hint;
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_instance_data(napi_env env, void **r) {
    ARG(env, r);
    *r = env->instance_data;
    return OK(env);
}

NAPI_EXTERN napi_status napi_add_env_cleanup_hook(napi_env env, napi_cleanup_hook fun, void *arg) {
    ARG(env, fun);
    tv_napi_cleanup *c = malloc(sizeof *c);
    if (!c) abort();
    *c = (tv_napi_cleanup){fun, arg, env->cleanups};
    env->cleanups = c;
    return OK(env);
}

NAPI_EXTERN napi_status napi_remove_env_cleanup_hook(napi_env env, napi_cleanup_hook fun, void *arg) {
    ARG(env, fun);
    for (tv_napi_cleanup **p = &env->cleanups; *p; p = &(*p)->next) {
        if ((*p)->fn == fun && (*p)->arg == arg) {
            tv_napi_cleanup *c = *p;
            *p = c->next;
            free(c);
            break;
        }
    }
    return OK(env);
}

NAPI_EXTERN napi_status napi_add_async_cleanup_hook(napi_env env, napi_async_cleanup_hook hook, void *arg, napi_async_cleanup_hook_handle *r) {
    (void)hook; (void)arg;
    if (r) *r = (napi_async_cleanup_hook_handle)(uintptr_t)1;
    return OK(env);
}

NAPI_EXTERN napi_status napi_remove_async_cleanup_hook(napi_async_cleanup_hook_handle h) {
    (void)h;
    return napi_ok;
}

NAPI_EXTERN napi_status napi_get_version(napi_env env, uint32_t *r) {
    ARG(env, r);
    *r = 10;
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_node_version(napi_env env, const napi_node_version **r) {
    static const napi_node_version v = {26, 3, 0, "node"};
    ARG(env, r);
    *r = &v;
    return OK(env);
}

NAPI_EXTERN napi_status node_api_get_module_file_name(napi_env env, const char **r) {
    ARG(env, r);
    *r = env->filename ? env->filename : "";
    return OK(env);
}

NAPI_EXTERN napi_status napi_adjust_external_memory(napi_env env, int64_t change, int64_t *r) {
    static int64_t total;
    total += change;
    if (r) *r = total;
    return OK(env);
}

NAPI_EXTERN napi_status napi_run_script(napi_env env, napi_value script, napi_value *r) {
    ARG(env, script && r);
    if (!JSValueIsString(env->ctx, V(script))) return FAIL(env, napi_string_expected);
    JSValueRef args[1] = {V(script)};
    JSValueRef exc = NULL;
    JSValueRef v = tv_napi_call_helper(env, "runScript", 1, args, &exc);
    CHECK_EXC(env, exc);
    *r = tv_napi_hold(v);
    return OK(env);
}

/* (a libuv loop isn't there: addons that drive one directly don't run) */
NAPI_EXTERN napi_status napi_get_uv_event_loop(napi_env env, void **loop) {
    static char not_a_loop[64];
    ARG(env, loop);
    *loop = not_a_loop;
    return OK(env);
}

/* ------------------------------------------------------------------ buffers and typed arrays */

static void tv_napi_free_bytes(void *bytes, void *ctx) { (void)ctx; free(bytes); }

/* an external buffer's finalizer, from the collector (deferred) */
typedef struct {
    napi_env env;
    napi_finalize fin;
    void *hint;
} tv_napi_extbuf;

static void tv_napi_extbuf_free(void *bytes, void *ctx) {
    tv_napi_extbuf *e = ctx;
    if (e->fin) tv_napi_post(tv_napi_finalize_job, (void *)e->fin, e->env, bytes, e->hint);
    free(e);
}

static JSValueRef tv_napi_to_buffer(napi_env env, JSValueRef u8) {
    JSValueRef args[1] = {u8};
    return tv_napi_call_helper(env, "toBuffer", 1, args, NULL);
}

NAPI_EXTERN napi_status napi_create_arraybuffer(napi_env env, size_t byte_length, void **data, napi_value *r) {
    ARG(env, r);
    void *p = calloc(1, byte_length ? byte_length : 1);
    if (!p) return FAIL(env, napi_generic_failure);
    JSValueRef exc = NULL;
    JSObjectRef b = JSObjectMakeArrayBufferWithBytesNoCopy(env->ctx, p, byte_length, tv_napi_free_bytes, NULL, &exc);
    CHECK_EXC(env, exc);
    if (data) *data = p;
    *r = tv_napi_hold(b);
    return OK(env);
}

NAPI_EXTERN napi_status napi_create_external_arraybuffer(napi_env env, void *external_data, size_t byte_length, napi_finalize fin, void *hint, napi_value *r) {
    ARG(env, r);
    tv_napi_extbuf *e = malloc(sizeof *e);
    if (!e) abort();
    *e = (tv_napi_extbuf){env, fin, hint};
    JSValueRef exc = NULL;
    JSObjectRef b = JSObjectMakeArrayBufferWithBytesNoCopy(env->ctx, external_data, byte_length, tv_napi_extbuf_free, e, &exc);
    CHECK_EXC(env, exc);
    *r = tv_napi_hold(b);
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_arraybuffer_info(napi_env env, napi_value value, void **data, size_t *len) {
    ARG(env, value);
    if (JSValueGetTypedArrayType(env->ctx, V(value), NULL) != kJSTypedArrayTypeArrayBuffer) return FAIL(env, napi_arraybuffer_expected);
    if (data) *data = JSObjectGetArrayBufferBytesPtr(env->ctx, O(value), NULL);
    if (len) *len = JSObjectGetArrayBufferByteLength(env->ctx, O(value), NULL);
    return OK(env);
}

NAPI_EXTERN napi_status napi_is_arraybuffer(napi_env env, napi_value value, bool *r) {
    ARG(env, value && r);
    *r = JSValueGetTypedArrayType(env->ctx, V(value), NULL) == kJSTypedArrayTypeArrayBuffer;
    return OK(env);
}

NAPI_EXTERN napi_status napi_detach_arraybuffer(napi_env env, napi_value value) {
    JSValueRef args[1] = {V(value)};
    JSValueRef exc = NULL;
    tv_napi_call_helper(env, "detach", 1, args, &exc);
    CHECK_EXC(env, exc);
    return OK(env);
}

NAPI_EXTERN napi_status napi_is_detached_arraybuffer(napi_env env, napi_value value, bool *r) {
    ARG(env, value && r);
    JSValueRef args[1] = {V(value)};
    *r = JSValueToBoolean(env->ctx, tv_napi_call_helper(env, "detached", 1, args, NULL));
    return OK(env);
}

static const JSTypedArrayType tv_napi_ta_types[] = {
    kJSTypedArrayTypeInt8Array, kJSTypedArrayTypeUint8Array, kJSTypedArrayTypeUint8ClampedArray, kJSTypedArrayTypeInt16Array,
    kJSTypedArrayTypeUint16Array, kJSTypedArrayTypeInt32Array, kJSTypedArrayTypeUint32Array, kJSTypedArrayTypeFloat32Array,
    kJSTypedArrayTypeFloat64Array, kJSTypedArrayTypeBigInt64Array, kJSTypedArrayTypeBigUint64Array,
};

static int tv_napi_ta_size(napi_typedarray_type t) {
    switch (t) {
    case napi_int8_array: case napi_uint8_array: case napi_uint8_clamped_array: return 1;
    case napi_int16_array: case napi_uint16_array: case napi_float16_array: return 2;
    case napi_int32_array: case napi_uint32_array: case napi_float32_array: return 4;
    default: return 8;
    }
}

NAPI_EXTERN napi_status napi_create_typedarray(napi_env env, napi_typedarray_type type, size_t length, napi_value arraybuffer, size_t byte_offset, napi_value *r) {
    ARG(env, arraybuffer && r);
    int size = tv_napi_ta_size(type);
    if (byte_offset % (size_t)size) {
        return napi_throw_range_error(env, "ERR_NAPI_INVALID_TYPEDARRAY_ALIGNMENT", "start offset of the typed array should be a multiple of its element size"), FAIL(env, napi_pending_exception);
    }
    JSValueRef args[4] = {JSValueMakeNumber(env->ctx, type), V(arraybuffer), JSValueMakeNumber(env->ctx, (double)byte_offset), JSValueMakeNumber(env->ctx, (double)length)};
    JSValueRef exc = NULL;
    JSValueRef v = tv_napi_call_helper(env, "typedArray", 4, args, &exc);
    CHECK_EXC(env, exc);
    *r = tv_napi_hold(v);
    return OK(env);
}

NAPI_EXTERN napi_status napi_is_typedarray(napi_env env, napi_value value, bool *r) {
    ARG(env, value && r);
    JSTypedArrayType t = JSValueGetTypedArrayType(env->ctx, V(value), NULL);
    *r = t != kJSTypedArrayTypeNone && t != kJSTypedArrayTypeArrayBuffer;
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_typedarray_info(napi_env env, napi_value value, napi_typedarray_type *type, size_t *length, void **data, napi_value *arraybuffer, size_t *byte_offset) {
    ARG(env, value);
    JSContextRef ctx = env->ctx;
    JSTypedArrayType t = JSValueGetTypedArrayType(ctx, V(value), NULL);
    if (t == kJSTypedArrayTypeNone || t == kJSTypedArrayTypeArrayBuffer) return FAIL(env, napi_invalid_arg);
    napi_typedarray_type nt = napi_uint8_array;
    for (size_t i = 0; i < sizeof tv_napi_ta_types / sizeof *tv_napi_ta_types; i++) {
        if (tv_napi_ta_types[i] == t) nt = (napi_typedarray_type)i;
    }
    if (type) *type = nt;
    if (length) *length = JSObjectGetTypedArrayLength(ctx, O(value), NULL);
    size_t off = JSObjectGetTypedArrayByteOffset(ctx, O(value), NULL);
    if (data) *data = (uint8_t *)JSObjectGetTypedArrayBytesPtr(ctx, O(value), NULL) + off;
    if (arraybuffer) *arraybuffer = tv_napi_hold(JSObjectGetTypedArrayBuffer(ctx, O(value), NULL));
    if (byte_offset) *byte_offset = off;
    return OK(env);
}

NAPI_EXTERN napi_status napi_create_dataview(napi_env env, size_t length, napi_value arraybuffer, size_t byte_offset, napi_value *r) {
    ARG(env, arraybuffer && r);
    JSValueRef args[3] = {V(arraybuffer), JSValueMakeNumber(env->ctx, (double)byte_offset), JSValueMakeNumber(env->ctx, (double)length)};
    JSValueRef exc = NULL;
    JSValueRef v = tv_napi_call_helper(env, "dataView", 3, args, &exc);
    CHECK_EXC(env, exc);
    *r = tv_napi_hold(v);
    return OK(env);
}

NAPI_EXTERN napi_status napi_is_dataview(napi_env env, napi_value value, bool *r) {
    ARG(env, value && r);
    JSValueRef args[1] = {V(value)};
    *r = JSValueToBoolean(env->ctx, tv_napi_call_helper(env, "isDataView", 1, args, NULL));
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_dataview_info(napi_env env, napi_value value, size_t *bytelength, void **data, napi_value *arraybuffer, size_t *byte_offset) {
    ARG(env, value);
    JSValueRef args[1] = {V(value)};
    JSObjectRef info = (JSObjectRef)tv_napi_call_helper(env, "dataViewInfo", 1, args, NULL);
    if (!info) return FAIL(env, napi_invalid_arg);
    JSContextRef ctx = env->ctx;
    size_t len = (size_t)JSValueToNumber(ctx, JSObjectGetPropertyAtIndex(ctx, info, 0, NULL), NULL);
    JSValueRef buf = JSObjectGetPropertyAtIndex(ctx, info, 1, NULL);
    size_t off = (size_t)JSValueToNumber(ctx, JSObjectGetPropertyAtIndex(ctx, info, 2, NULL), NULL);
    if (bytelength) *bytelength = len;
    if (data) *data = (uint8_t *)JSObjectGetArrayBufferBytesPtr(ctx, O(buf), NULL) + off;
    if (arraybuffer) *arraybuffer = tv_napi_hold(buf);
    if (byte_offset) *byte_offset = off;
    return OK(env);
}

NAPI_EXTERN napi_status napi_create_buffer(napi_env env, size_t size, void **data, napi_value *r) {
    ARG(env, r);
    void *p = calloc(1, size ? size : 1);
    if (!p) return FAIL(env, napi_generic_failure);
    JSValueRef exc = NULL;
    JSObjectRef u8 = JSObjectMakeTypedArrayWithBytesNoCopy(env->ctx, kJSTypedArrayTypeUint8Array, p, size, tv_napi_free_bytes, NULL, &exc);
    CHECK_EXC(env, exc);
    if (data) *data = p;
    *r = tv_napi_hold(tv_napi_to_buffer(env, u8));
    return OK(env);
}

NAPI_EXTERN napi_status napi_create_buffer_copy(napi_env env, size_t length, const void *src, void **data, napi_value *r) {
    void *p;
    napi_status s = napi_create_buffer(env, length, &p, r);
    if (s != napi_ok) return s;
    if (length) memcpy(p, src, length);
    if (data) *data = p;
    return OK(env);
}

NAPI_EXTERN napi_status napi_create_external_buffer(napi_env env, size_t length, void *data, napi_finalize fin, void *hint, napi_value *r) {
    ARG(env, r);
    tv_napi_extbuf *e = malloc(sizeof *e);
    if (!e) abort();
    *e = (tv_napi_extbuf){env, fin, hint};
    JSValueRef exc = NULL;
    JSObjectRef u8 = JSObjectMakeTypedArrayWithBytesNoCopy(env->ctx, kJSTypedArrayTypeUint8Array, data, length, tv_napi_extbuf_free, e, &exc);
    CHECK_EXC(env, exc);
    *r = tv_napi_hold(tv_napi_to_buffer(env, u8));
    return OK(env);
}

NAPI_EXTERN napi_status node_api_create_buffer_from_arraybuffer(napi_env env, napi_value arraybuffer, size_t byte_offset, size_t byte_length, napi_value *r) {
    napi_value u8;
    napi_status s = napi_create_typedarray(env, napi_uint8_array, byte_length, arraybuffer, byte_offset, &u8);
    if (s != napi_ok) return s;
    *r = tv_napi_hold(tv_napi_to_buffer(env, V(u8)));
    return OK(env);
}

NAPI_EXTERN napi_status napi_is_buffer(napi_env env, napi_value value, bool *r) {
    ARG(env, value && r);
    JSValueRef args[1] = {V(value)};
    *r = JSValueToBoolean(env->ctx, tv_napi_call_helper(env, "isBuffer", 1, args, NULL));
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_buffer_info(napi_env env, napi_value value, void **data, size_t *length) {
    ARG(env, value);
    uint8_t *p;
    size_t n;
    if (!tv_js_bytes_view(V(value), &p, &n)) return FAIL(env, napi_invalid_arg);
    if (data) *data = p;
    if (length) *length = n;
    return OK(env);
}

/* ------------------------------------------------------------------ promises */

struct napi_deferred__ {
    JSValueRef ok, no;   /* rooted */
};

NAPI_EXTERN napi_status napi_create_promise(napi_env env, napi_deferred *deferred, napi_value *promise) {
    ARG(env, deferred && promise);
    JSValueRef exc = NULL;
    JSObjectRef r = (JSObjectRef)tv_napi_call_helper(env, "promise", 0, NULL, &exc);
    CHECK_EXC(env, exc);
    JSStringRef kp = JSStringCreateWithUTF8CString("p"), ko = JSStringCreateWithUTF8CString("ok"), kn = JSStringCreateWithUTF8CString("no");
    struct napi_deferred__ *d = malloc(sizeof *d);
    if (!d) abort();
    d->ok = tv_js_retain(JSObjectGetProperty(env->ctx, r, ko, NULL));
    d->no = tv_js_retain(JSObjectGetProperty(env->ctx, r, kn, NULL));
    *promise = tv_napi_hold(JSObjectGetProperty(env->ctx, r, kp, NULL));
    JSStringRelease(kp);
    JSStringRelease(ko);
    JSStringRelease(kn);
    *deferred = d;
    return OK(env);
}

static napi_status tv_napi_settle(napi_env env, napi_deferred d, napi_value v, bool ok) {
    ARG(env, d && v);
    JSValueRef args[1] = {V(v)};
    JSObjectCallAsFunction(env->ctx, O(ok ? d->ok : d->no), NULL, 1, args, NULL);
    tv_js_release(d->ok);
    tv_js_release(d->no);
    free(d);
    return OK(env);
}

NAPI_EXTERN napi_status napi_resolve_deferred(napi_env env, napi_deferred d, napi_value v) { return tv_napi_settle(env, d, v, true); }
NAPI_EXTERN napi_status napi_reject_deferred(napi_env env, napi_deferred d, napi_value v) { return tv_napi_settle(env, d, v, false); }

NAPI_EXTERN napi_status napi_is_promise(napi_env env, napi_value value, bool *r) {
    ARG(env, value && r);
    JSValueRef args[1] = {V(value)};
    *r = JSValueToBoolean(env->ctx, tv_napi_call_helper(env, "isPromise", 1, args, NULL));
    return OK(env);
}

/* ------------------------------------------------------------------ async work
 *
 * `execute` runs on a pool thread; `complete` back on the main thread (through the pipe). */

struct napi_async_work__ {
    napi_env env;
    napi_async_execute_callback execute;
    napi_async_complete_callback complete;
    void *data;
    struct napi_async_work__ *next;
    int state;   /* 0 idle, 1 queued, 2 running, 3 done */
    bool cancelled;
};

static pthread_mutex_t tv_napi_work_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tv_napi_work_cond = PTHREAD_COND_INITIALIZER;
static struct napi_async_work__ *tv_napi_work_head, *tv_napi_work_tail;
static int tv_napi_nworkers;

static void tv_napi_complete_job(void *a, void *b, void *c, void *d) {
    (void)b; (void)c; (void)d;
    napi_async_work w = a;
    tv_napi_ref(-1);
    w->state = 3;
    /* (the completion usually deletes the work: nothing of it is read after) */
    napi_env env = w->env;
    if (w->complete) w->complete(env, w->cancelled ? napi_cancelled : napi_ok, w->data);
    if (env->pending) {
        /* an exception escaping a completion: uncaught, as in Node.js */
        JSValueRef e = env->pending;
        env->pending = NULL;
        tv_str t = tv_js_error_text(e);
        fprintf(stderr, "Uncaught %s\n", t.p->data);
        exit(7);
    }
}

static void *tv_napi_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&tv_napi_work_lock);
        while (!tv_napi_work_head) pthread_cond_wait(&tv_napi_work_cond, &tv_napi_work_lock);
        napi_async_work w = tv_napi_work_head;
        tv_napi_work_head = w->next;
        if (!tv_napi_work_head) tv_napi_work_tail = NULL;
        w->state = 2;
        pthread_mutex_unlock(&tv_napi_work_lock);
        w->execute(w->env, w->data);
        tv_napi_post(tv_napi_complete_job, w, NULL, NULL, NULL);
    }
    return NULL;
}

NAPI_EXTERN napi_status napi_create_async_work(napi_env env, napi_value resource, napi_value resource_name, napi_async_execute_callback execute, napi_async_complete_callback complete, void *data, napi_async_work *r) {
    (void)resource; (void)resource_name;
    ARG(env, execute && r);
    napi_async_work w = calloc(1, sizeof *w);
    if (!w) abort();
    w->env = env;
    w->execute = execute;
    w->complete = complete;
    w->data = data;
    *r = w;
    return OK(env);
}

NAPI_EXTERN napi_status napi_delete_async_work(napi_env env, napi_async_work w) {
    ARG(env, w);
    free(w);
    return OK(env);
}

NAPI_EXTERN napi_status napi_queue_async_work(napi_env env, napi_async_work w) {
    ARG(env, w);
    pthread_mutex_lock(&tv_napi_work_lock);
    if (tv_napi_nworkers == 0) {
        /* as libuv's default pool */
        for (int i = 0; i < 4; i++) {
            pthread_t t;
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setstacksize(&at, 8 << 20);
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&t, &at, tv_napi_worker, NULL) == 0) tv_napi_nworkers++;
            pthread_attr_destroy(&at);
        }
    }
    w->state = 1;
    w->next = NULL;
    if (tv_napi_work_tail) tv_napi_work_tail->next = w; else tv_napi_work_head = w;
    tv_napi_work_tail = w;
    pthread_cond_signal(&tv_napi_work_cond);
    pthread_mutex_unlock(&tv_napi_work_lock);
    tv_napi_ref(1);
    return OK(env);
}

NAPI_EXTERN napi_status napi_cancel_async_work(napi_env env, napi_async_work w) {
    ARG(env, w);
    pthread_mutex_lock(&tv_napi_work_lock);
    bool removed = false;
    for (napi_async_work *p = &tv_napi_work_head; *p; p = &(*p)->next) {
        if (*p == w) {
            *p = w->next;
            removed = true;
            break;
        }
    }
    if (removed) {
        tv_napi_work_tail = NULL;
        for (napi_async_work q = tv_napi_work_head; q; q = q->next) tv_napi_work_tail = q;
    }
    pthread_mutex_unlock(&tv_napi_work_lock);
    if (!removed) return FAIL(env, napi_generic_failure);
    w->cancelled = true;
    tv_napi_post(tv_napi_complete_job, w, NULL, NULL, NULL);
    return OK(env);
}

/* ------------------------------------------------------------------ thread-safe functions */

typedef struct tv_napi_tsfn_call {
    void *data;
    struct tv_napi_tsfn_call *next;
} tv_napi_tsfn_call;

struct napi_threadsafe_function__ {
    napi_env env;
    JSValueRef func;                        /* rooted (may be NULL) */
    void *context;
    napi_threadsafe_function_call_js call_js;
    napi_finalize finalize;
    void *finalize_data;
    size_t max_queue;
    size_t threads;
    bool closing, aborted, refd, scheduled, finalized;
    pthread_mutex_t lock;
    pthread_cond_t space;
    tv_napi_tsfn_call *head, *tail;
    size_t queued;
};

static void tv_napi_tsfn_finalize(napi_threadsafe_function f) {
    if (f->finalized) return;
    f->finalized = true;
    if (f->refd) tv_napi_ref(-1);
    f->refd = false;
    if (f->finalize) f->finalize(f->env, f->finalize_data, f->context);
    if (f->func) tv_js_release(f->func);
    f->func = NULL;
}

static void tv_napi_tsfn_drain(void *a, void *b, void *c, void *d) {
    (void)b; (void)c; (void)d;
    napi_threadsafe_function f = a;
    for (;;) {
        pthread_mutex_lock(&f->lock);
        f->scheduled = false;
        tv_napi_tsfn_call *call = f->head;
        if (call) {
            f->head = call->next;
            if (!f->head) f->tail = NULL;
            f->queued--;
            pthread_cond_signal(&f->space);
        }
        bool done = !call && (f->threads == 0 || f->aborted);
        pthread_mutex_unlock(&f->lock);
        if (!call) {
            if (done) tv_napi_tsfn_finalize(f);
            return;
        }
        size_t mark = tv_napi_nhandles;
        if (!f->aborted) {
            if (f->call_js) {
                f->call_js(f->env, (napi_value)f->func, f->context, call->data);
            } else if (f->func) {
                JSObjectCallAsFunction(f->env->ctx, O(f->func), NULL, 0, NULL, NULL);
            }
        }
        tv_napi_release_to(mark);
        free(call);
        if (f->env->pending) {
            JSValueRef e = f->env->pending;
            f->env->pending = NULL;
            tv_str t = tv_js_error_text(e);
            fprintf(stderr, "Uncaught %s\n", t.p->data);
            exit(7);
        }
        tv_js_drain();
    }
}

NAPI_EXTERN napi_status napi_create_threadsafe_function(napi_env env, napi_value func, napi_value async_resource, napi_value async_resource_name, size_t max_queue_size, size_t initial_thread_count, void *thread_finalize_data, napi_finalize thread_finalize_cb, void *context, napi_threadsafe_function_call_js call_js_cb, napi_threadsafe_function *r) {
    (void)async_resource; (void)async_resource_name;
    ARG(env, r && initial_thread_count > 0 && (func || call_js_cb));
    napi_threadsafe_function f = calloc(1, sizeof *f);
    if (!f) abort();
    f->env = env;
    f->func = func ? tv_js_retain(V(func)) : NULL;
    f->context = context;
    f->call_js = call_js_cb;
    f->finalize = thread_finalize_cb;
    f->finalize_data = thread_finalize_data;
    f->max_queue = max_queue_size;
    f->threads = initial_thread_count;
    f->refd = true;
    pthread_mutex_init(&f->lock, NULL);
    pthread_cond_init(&f->space, NULL);
    tv_napi_ref(1);
    *r = f;
    return OK(env);
}

NAPI_EXTERN napi_status napi_get_threadsafe_function_context(napi_threadsafe_function f, void **r) {
    if (!f || !r) return napi_invalid_arg;
    *r = f->context;
    return napi_ok;
}

NAPI_EXTERN napi_status napi_call_threadsafe_function(napi_threadsafe_function f, void *data, napi_threadsafe_function_call_mode mode) {
    if (!f) return napi_invalid_arg;
    pthread_mutex_lock(&f->lock);
    while (f->max_queue && f->queued >= f->max_queue && !f->closing && !f->aborted) {
        if (mode == napi_tsfn_nonblocking) {
            pthread_mutex_unlock(&f->lock);
            return napi_queue_full;
        }
        pthread_cond_wait(&f->space, &f->lock);
    }
    if (f->aborted || f->closing) {
        pthread_mutex_unlock(&f->lock);
        return napi_closing;
    }
    tv_napi_tsfn_call *call = malloc(sizeof *call);
    if (!call) abort();
    call->data = data;
    call->next = NULL;
    if (f->tail) f->tail->next = call; else f->head = call;
    f->tail = call;
    f->queued++;
    bool post = !f->scheduled;
    f->scheduled = true;
    pthread_mutex_unlock(&f->lock);
    if (post) tv_napi_post(tv_napi_tsfn_drain, f, NULL, NULL, NULL);
    return napi_ok;
}

NAPI_EXTERN napi_status napi_acquire_threadsafe_function(napi_threadsafe_function f) {
    if (!f) return napi_invalid_arg;
    pthread_mutex_lock(&f->lock);
    if (f->closing) {
        pthread_mutex_unlock(&f->lock);
        return napi_closing;
    }
    f->threads++;
    pthread_mutex_unlock(&f->lock);
    return napi_ok;
}

NAPI_EXTERN napi_status napi_release_threadsafe_function(napi_threadsafe_function f, napi_threadsafe_function_release_mode mode) {
    if (!f) return napi_invalid_arg;
    pthread_mutex_lock(&f->lock);
    if (f->threads == 0) {
        pthread_mutex_unlock(&f->lock);
        return napi_invalid_arg;
    }
    f->threads--;
    if (mode == napi_tsfn_abort) {
        f->aborted = true;
        f->closing = true;
    }
    if (f->threads == 0) f->closing = true;
    bool post = (f->threads == 0 || f->aborted) && !f->scheduled;
    if (post) f->scheduled = true;
    pthread_cond_broadcast(&f->space);
    pthread_mutex_unlock(&f->lock);
    if (post) tv_napi_post(tv_napi_tsfn_drain, f, NULL, NULL, NULL);
    return napi_ok;
}

NAPI_EXTERN napi_status napi_ref_threadsafe_function(napi_env env, napi_threadsafe_function f) {
    ARG(env, f);
    if (!f->refd && !f->finalized) {
        f->refd = true;
        tv_napi_ref(1);
    }
    return OK(env);
}

NAPI_EXTERN napi_status napi_unref_threadsafe_function(napi_env env, napi_threadsafe_function f) {
    ARG(env, f);
    if (f->refd) {
        f->refd = false;
        tv_napi_ref(-1);
    }
    return OK(env);
}

/* ------------------------------------------------------------------ loading addons */

static napi_module *tv_napi_registered;   /* set by a legacy addon's constructor during dlopen */

NAPI_EXTERN void napi_module_register(napi_module *mod) { tv_napi_registered = mod; }

/* globalThis.__tov_dlopen(path, exports): loads a `.node` addon and returns its exports. */
static JSValueRef tv_napi_dlopen_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self;
    if (n < 2) return JSValueMakeUndefined(ctx);
    tv_napi_global_init(ctx);
    tv_str path = tv_js_to_str(a[0]);
    tv_napi_registered = NULL;
    /* (lazy, as Node.js loads addons: a symbol the addon never calls needn't be there) */
    void *h = dlopen(path.p->data, RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        const char *why = dlerror();
        tv_sb sb = {0};
        tv_sb_push_cstr(&sb, why ? why : "dlopen failed");
        JSValueRef m = tv_js_str(sb.data, sb.len);
        tv_sb_free(&sb);
        tv_str_release(path);
        JSObjectRef e = JSObjectMakeError(ctx, 1, &m, NULL);
        JSStringRef kc = JSStringCreateWithUTF8CString("code");
        JSObjectSetProperty(ctx, e, kc, tv_js_str("ERR_DLOPEN_FAILED", 17), 0, NULL);
        JSStringRelease(kc);
        *exc = e;
        return NULL;
    }
    napi_addon_register_func init = (napi_addon_register_func)dlsym(h, "napi_register_module_v1");
    int32_t (*version)(void) = (int32_t (*)(void))dlsym(h, "node_api_module_get_api_version_v1");
    if (!init && tv_napi_registered) init = tv_napi_registered->nm_register_func;
    if (!init) {
        JSValueRef m = tv_js_str("Module did not self-register (not a Node-API addon)", 51);
        tv_str_release(path);
        *exc = JSObjectMakeError(ctx, 1, &m, NULL);
        return NULL;
    }
    napi_env env = calloc(1, sizeof *env);
    if (!env) abort();
    env->ctx = ctx;
    env->api_version = version ? version() : 8;
    size_t plen = (size_t)path.p->len;
    env->filename = malloc(plen + 8);
    if (env->filename) {
        memcpy(env->filename, "file://", 7);
        memcpy(env->filename + 7, path.p->data, plen + 1);
    }
    tv_str_release(path);
    size_t mark = tv_napi_nhandles;
    napi_value exports = (napi_value)a[1];
    napi_value r = init(env, exports);
    JSValueRef out = r ? V(r) : a[1];
    if (env->pending) {
        *exc = env->pending;
        env->pending = NULL;
        out = NULL;
    }
    if (out) tv_js_retain(out);
    tv_napi_release_to(mark);
    if (out) tv_js_release(out);
    return out;
}

/* Called by js.c when the context starts (in programs that link this file: this file's
 * constructor tells it so). */
static void tv_napi_install(JSContextRef ctx) {
    tv_js_def(ctx, JSContextGetGlobalObject(ctx), "__tov_dlopen", tv_napi_dlopen_fn);
}

__attribute__((constructor)) static void tv_napi_register(void) { tv_js_napi_install = tv_napi_install; }
