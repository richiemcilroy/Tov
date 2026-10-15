/* js.c — Tov's bridge to JavaScriptCore (see js.h): the engine, values, calls, bytes and
 * promises across the boundary. Linked only by programs that import npm packages, with
 * runtime/node.c (the Node.js natives) and the system JavaScriptCore framework. */

#ifndef __APPLE__
#define _GNU_SOURCE 1   /* (glibc's posix_spawn extensions: POSIX_SPAWN_SETSID, addclosefrom_np) */
#endif
#include "js.h"

/* The engine: Tov's own JavaScriptCore (TV_JSC_OWN: scripts/jsc, linked into the program), or
 * the system's (TV_JSC_SYSTEM: macOS's framework), or WebKitGTK's elsewhere. */
#if defined(__APPLE__) && !defined(TV_JSC_OWN)
#define TV_JSC_SYSTEM 1
#include <JavaScriptCore/JavaScriptCore.h>
#else
#include <JavaScriptCore/JavaScript.h>
#endif
#ifdef TV_JSC_OWN
#include <JavaScriptCore/TovAPI.h>
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <dlfcn.h>
#include <errno.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* A call Tov makes most (a fused expression, a method, a record's maker): Tov's own engine has a
 * leaner one (TVCall) than the C API's. */
#ifdef TV_JSC_OWN
#define TV_JS_CALL TVCall
#else
#define TV_JS_CALL JSObjectCallAsFunction
#endif

JSGlobalContextRef tv_js_ctx;
bool tv_js_encoded;
/* runtime/napi.c (native addons), in programs whose bundle holds one: it sets this */
void (*tv_js_napi_install)(JSContextRef ctx);
static JSStringRef tv_js_bundle_url;    /* the prelude's URL */
static JSObjectRef tv_js_npm;           /* globalThis.__tov_npm (protected) */
static JSObjectRef tv_js_noop;          /* calling it runs pending microtasks */

/* Set by the program: a Tov JsError object (owned) holding a thrown JavaScript value. */
void *(*tv_js_make_error)(JSValueRef exc);

/* Until runtime/node.c defines it (a program links node.c as an object, so its definition wins):
 * no natives, so the shims fall back to what the engine has (internal/bootstrap/host_fallback). */
__attribute__((weak)) void tv_node_install(JSContextRef ctx, JSObjectRef native) {
    (void)native;
    JSStringRef k = JSStringCreateWithUTF8CString("__tov_native");
    JSObjectDeleteProperty(ctx, JSContextGetGlobalObject(ctx), k, NULL);
    JSStringRelease(k);
}

static JSValueRef tv_js_noop_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self; (void)n; (void)a; (void)exc;
    return JSValueMakeUndefined(ctx);
}

/* The blob (see Bundle::blob in crates/tov/src/npm/bundle.rs): a u32 index — the module count,
 * the prelude's offset, then per module its name's offset, its source's offset and length — and
 * NUL-terminated strings. */
static uint32_t tv_js_u32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* The blob is in the program (tv_js_blob, assembled in) or, when the program says so (tv_js_blob
 * is "TOV_APP"), appended to its executable file, mapped from there: a build whose JavaScript
 * changed then makes the program by appending it to the binary it linked before, without linking
 * again (crates/tov/src/build.rs, with_blob). The file ends with the blob's offset and length
 * (u64s) and "TOV_BLOB". */
static const unsigned char *tv_js_blob_p;

static bool tv_js_exe_path(char *out, size_t cap);

static const unsigned char *tv_js_blob_data(void) {
    if (tv_js_blob_p) return tv_js_blob_p;
    if (memcmp(tv_js_blob, "TOV_APP", 8) != 0) return tv_js_blob_p = tv_js_blob;
    char exe[1024];
    int fd = tv_js_exe_path(exe, sizeof exe) ? open(exe, O_RDONLY | O_CLOEXEC) : -1;
    struct stat st;
    unsigned char tail[24];
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 24 || pread(fd, tail, 24, st.st_size - 24) != 24 || memcmp(tail + 16, "TOV_BLOB", 8) != 0) {
        const char *m = "tov: the program's JavaScript is missing from its file (was it changed or stripped?)\n";
        tv_write_fd(2, m, strlen(m));
        _exit(1);
    }
    uint64_t at = 0, len = 0;
    for (int k = 7; k >= 0; k--) {
        at = at << 8 | tail[k];
        len = len << 8 | tail[8 + k];
    }
    void *p = mmap(NULL, (size_t)len, PROT_READ, MAP_PRIVATE, fd, (off_t)at);
    close(fd);
    if (p == MAP_FAILED) {
        const char *m = "tov: can't map the program's JavaScript\n";
        tv_write_fd(2, m, strlen(m));
        _exit(1);
    }
    return tv_js_blob_p = p;
}

#define tv_js_blob tv_js_blob_data()
static uint32_t tv_js_nmodules(void) { return tv_js_u32(tv_js_blob); }
static const char *tv_js_prelude(void) { return (const char *)tv_js_blob + tv_js_u32(tv_js_blob + 4); }
static const char *tv_js_module_name(uint32_t i) { return (const char *)tv_js_blob + tv_js_u32(tv_js_blob + 8 + 16 * i); }
static const char *tv_js_module_src(uint32_t i, size_t *len) {
    *len = tv_js_u32(tv_js_blob + 16 + 16 * i);
    return (const char *)tv_js_blob + tv_js_u32(tv_js_blob + 12 + 16 * i);
}
static const char *tv_js_module_map(uint32_t i) { return (const char *)tv_js_blob + tv_js_u32(tv_js_blob + 20 + 16 * i); }

static JSStringRef tv_js_string_ref(const char *s, size_t n, bool nul_terminated);

/* globalThis.__tov_name(id) / __tov_map(id): a module's name, and its requires (encoded, see
 * Bundle::maps), read from the blob when the module is first loaded. */
static JSValueRef tv_js_blob_string(JSContextRef ctx, size_t n, const JSValueRef a[], const char *(*get)(uint32_t)) {
    double d = n > 0 ? JSValueToNumber(ctx, a[0], NULL) : -1;
    if (!(d >= 0 && d < tv_js_nmodules())) return JSValueMakeUndefined(ctx);
    const char *s = get((uint32_t)d);
    JSStringRef t = tv_js_string_ref(s, strlen(s), true);
    JSValueRef v = JSValueMakeString(ctx, t);
    JSStringRelease(t);
    return v;
}
static JSValueRef tv_js_name_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    return tv_js_blob_string(ctx, n, a, tv_js_module_name);
}
static JSValueRef tv_js_map_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    return tv_js_blob_string(ctx, n, a, tv_js_module_map);
}

static JSStringRef tv_js_string_ref(const char *s, size_t n, bool nul_terminated);

/* A module's source as a JavaScript string. */
static JSStringRef tv_js_module_text(uint32_t i) {
    size_t len;
    const char *src = tv_js_module_src(i, &len);
    /* (the bundler's sources are valid UTF-8; a NUL byte inside needs the slow path) */
    if (strlen(src) == len) return JSStringCreateWithUTF8CString(src);
    return tv_js_string_ref(src, len, false);
}

#if defined(TV_JSC_OWN) || defined(__APPLE__)
/* The bytecode cache. A module's bytecode (its functions' too, all of them) is written to a file
 * the first time the program runs, and the next run reads it back instead of parsing: the media
 * server Cap runs (654 modules, 5 MB) spends ~35 ms parsing modules at start, and more compiling
 * the functions it calls. With Tov's own JavaScriptCore (TV_JSC_OWN, scripts/jsc) it's
 * TovAPI.h's TVEvaluateScript and TVWriteBytecode, the text read where it is in the program;
 * with the system's on macOS, JSScript (the Objective-C API), whose cache the engine only reads
 * from a "data vault" directory (one only Apple's own software can make), so the cache's file is
 * set on the JSScript directly (its m_cachePath, checked to be what's expected: if
 * JavaScriptCore changes, nothing is cached). The engine checks a cache before it uses one (its
 * text, the engine's build; the system's, this boot too): one that doesn't match is written
 * again. The files are written by a process of their own (the program, run again to write them:
 * TOV_JS_CACHE_WRITE), started when the program first idles or exits, at a low priority, each
 * to a temporary file renamed into place: a file is whole or isn't there. They're in the user's
 * temporary directory on macOS (which the system empties of what isn't used), ~/.cache/tov-js
 * elsewhere, named for the module's text, so programs share them; or in TOV_JS_CACHE_DIR.
 * TOV_JS_CACHE=0 turns it off. */
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/qos.h>
/* (<spawn.h>'s, hidden under strict standards) */
extern int posix_spawnattr_set_qos_class_np(posix_spawnattr_t *attr, qos_class_t qos);
#endif
#ifdef TV_JSC_SYSTEM
#include <objc/message.h>
#include <objc/runtime.h>
/* (the Objective-C runtime's, not in its headers) */
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);
#endif

extern char **environ;

/* (smaller modules parse faster than a cache is read: a program of only small modules, such as
 * the 7 KB the runtime's own bootstrap is, starts ~1 ms sooner without one) */
#define TV_JSC_MIN_BYTES 8192

static struct {
    int state;              /* 0 not yet looked, 1 on, -1 off */
#ifdef TV_JSC_SYSTEM
    Class script;           /* JSScript */
    ptrdiff_t cache_path;   /* its m_cachePath */
    id context;             /* a JSContext over tv_js_ctx */
    id vm;                  /* its JSVirtualMachine */
#endif
    char dir[1024];
    uint32_t *queue;        /* modules to write */
    size_t nqueue, capqueue;
} tv_jsc;

/* This program's executable, to start again (the cache's writer). */
static bool tv_jsc_exe(char *out, size_t cap) { return tv_js_exe_path(out, cap); }

static void tv_jsc_low_priority(posix_spawnattr_t *attr) {
#ifdef __APPLE__
    posix_spawnattr_set_qos_class_np(attr, QOS_CLASS_UTILITY);
#else
    (void)attr; /* (the writer lowers its own: tv_jsc_writer) */
#endif
}

#ifdef TV_JSC_SYSTEM
static id tv_jsc_send(id self, const char *sel) { return ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName(sel)); }

/* JSScript as expected, and a JSContext over the program's context. */
static bool tv_jsc_system(void) {
    Class script = objc_getClass("JSScript"), context = objc_getClass("JSContext");
    if (!script || !context) return false;
    Ivar path = class_getInstanceVariable(script, "m_cachePath");
    const char *type = path ? ivar_getTypeEncoding(path) : NULL;
    /* (a RetainPtr<NSURL>: a pointer the JSScript releases) */
    if (!type || strcmp(type, "{RetainPtr<NSURL>=\"m_ptr\"@\"NSURL\"}") != 0) return false;
    if (!class_getInstanceMethod(script, sel_registerName("readCache")) || !class_getInstanceMethod(script, sel_registerName("cacheBytecodeWithError:"))
        || !class_getInstanceMethod(script, sel_registerName("isUsingBytecodeCache"))
        || !class_getClassMethod(script, sel_registerName("scriptOfType:withSource:andSourceURL:andBytecodeCache:inVirtualMachine:error:"))
        || !class_getClassMethod(context, sel_registerName("contextWithJSGlobalContextRef:"))
        || !class_getInstanceMethod(context, sel_registerName("evaluateJSScript:")))
        return false;
    if (tv_js_ctx) {
        id ctx = ((id (*)(Class, SEL, JSGlobalContextRef))objc_msgSend)(context, sel_registerName("contextWithJSGlobalContextRef:"), tv_js_ctx);
        if (!ctx) return false;
        tv_jsc.context = (id)CFRetain(ctx);
        tv_jsc.vm = (id)CFRetain(tv_jsc_send(ctx, "virtualMachine"));
    }
    tv_jsc.script = script;
    tv_jsc.cache_path = ivar_getOffset(path);
    return true;
}
#endif

static bool tv_jsc_on(void) {
    if (tv_jsc.state) return tv_jsc.state > 0;
    tv_jsc.state = -1;
    const char *off = getenv("TOV_JS_CACHE");
    if (off && strcmp(off, "0") == 0) return false;
#ifdef TV_JSC_SYSTEM
    if (!tv_jsc_system()) return false;
#endif
    const char *dir = getenv("TOV_JS_CACHE_DIR");
    if (dir && *dir) {
        if (strlen(dir) >= sizeof tv_jsc.dir - 32) return false;
        strcpy(tv_jsc.dir, dir);
    } else {
#ifdef __APPLE__
        size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, tv_jsc.dir, sizeof tv_jsc.dir);
        if (n == 0 || n > sizeof tv_jsc.dir - 32) return false;
        strcat(tv_jsc.dir, "tov-js");
#else
        const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
        int n = xdg && *xdg ? snprintf(tv_jsc.dir, sizeof tv_jsc.dir, "%s/tov-js", xdg)
              : home && *home ? snprintf(tv_jsc.dir, sizeof tv_jsc.dir, "%s/.cache/tov-js", home)
              : snprintf(tv_jsc.dir, sizeof tv_jsc.dir, "/tmp/tov-js-%d", (int)getuid());
        if (n < 0 || (size_t)n > sizeof tv_jsc.dir - 32) return false;
        if (!(xdg && *xdg) && home && *home) {
            /* (~/.cache, as it may not be there yet) */
            char parent[1024];
            snprintf(parent, sizeof parent, "%s/.cache", home);
            mkdir(parent, 0700);
        }
#endif
    }
    if (mkdir(tv_jsc.dir, 0700) != 0 && errno != EEXIST) return false;
#ifdef TV_JSC_CACHE_FORMAT
    /* (Tov's own engine's caches, in a directory for its build's format: the system's engine
     * names its files the same) */
    size_t at = strlen(tv_jsc.dir);
    snprintf(tv_jsc.dir + at, sizeof tv_jsc.dir - at, "/%s", TV_JSC_CACHE_FORMAT);
    if (mkdir(tv_jsc.dir, 0700) != 0 && errno != EEXIST) return false;
#endif
    tv_jsc.state = 1;
    return true;
}

/* The cache's files for module i, named for its name and text: `out` + ".jsc" (the bytecode),
 * and with the system's engine + ".js" (the text, when it's ASCII: read from the file, mapped,
 * it's memory the system can drop and read again, where a copy of it would be the program's own,
 * ~5 MB for that server; Tov's own engine reads the text where it is in the program). */
static void tv_jsc_file(uint32_t i, char *out, size_t cap) {
    size_t len;
    const unsigned char *s = (const unsigned char *)tv_js_module_src(i, &len);
    uint64_t h = 0x9e3779b97f4a7c15ull ^ len;
    size_t k = 0;
    for (; k + 8 <= len; k += 8) {
        uint64_t w;
        memcpy(&w, s + k, 8);
        h = (h ^ w) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    for (; k < len; k++) h = (h ^ s[k]) * 0x100000001b3ull;
    for (const unsigned char *p = (const unsigned char *)tv_js_module_name(i); *p; p++) h = (h ^ *p) * 0x100000001b3ull;
    h ^= h >> 29;
    snprintf(out, cap, "%s/%016llx", tv_jsc.dir, (unsigned long long)h);
}

static void tv_jsc_at_exit(void);

static void tv_jsc_queue(uint32_t i) {
    static bool registered;
    if (!registered) {
        registered = true;
        atexit(tv_jsc_at_exit);
    }
    if (tv_jsc.nqueue == tv_jsc.capqueue) {
        size_t cap = tv_jsc.capqueue ? tv_jsc.capqueue * 2 : 64;
        uint32_t *q = realloc(tv_jsc.queue, cap * sizeof *q);
        if (!q) return;
        tv_jsc.queue = q;
        tv_jsc.capqueue = cap;
    }
    tv_jsc.queue[tv_jsc.nqueue++] = i;
}

#ifdef TV_JSC_SYSTEM
/* Module i's name as a URL (owned), or NULL if it isn't one as it is: stack traces name a
 * module as its JSScript's URL reads (and those modules aren't cached). */
static CFURLRef tv_jsc_url(uint32_t i) {
    const char *name = tv_js_module_name(i);
    CFURLRef url = CFURLCreateWithBytes(NULL, (const UInt8 *)name, (CFIndex)strlen(name), kCFStringEncodingUTF8, NULL);
    if (!url) return NULL;
    char back[1024];
    if (!CFStringGetCString(CFURLGetString(url), back, sizeof back, kCFStringEncodingUTF8) || strcmp(back, name) != 0) {
        CFRelease(url);
        return NULL;
    }
    return url;
}

/* A JSScript of module i in `vm`, autoreleased, its text mapped from file `mapped` if there's one
 * (NULL if the module's name isn't a URL). */
static id tv_jsc_script(uint32_t i, id vm, const char *mapped) {
    size_t len;
    const char *src = tv_js_module_src(i, &len);
    CFURLRef url = tv_jsc_url(i);
    if (!url) return NULL;
    id error = NULL, script = NULL;
    if (mapped) {
        CFURLRef file = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)mapped, (CFIndex)strlen(mapped), false);
        if (file) {
            script = ((id (*)(Class, SEL, long, id, id, id, id, id *))objc_msgSend)(tv_jsc.script,
                sel_registerName("scriptOfType:memoryMappedFromASCIIFile:withSourceURL:andBytecodeCache:inVirtualMachine:error:"), 0, (id)file, (id)url, NULL, vm, &error);
            CFRelease(file);
        }
    } else {
        CFStringRef text = CFStringCreateWithBytesNoCopy(NULL, (const UInt8 *)src, (CFIndex)len, kCFStringEncodingUTF8, false, kCFAllocatorNull);
        if (text) {
            script = ((id (*)(Class, SEL, long, id, id, id, id, id *))objc_msgSend)(tv_jsc.script,
                sel_registerName("scriptOfType:withSource:andSourceURL:andBytecodeCache:inVirtualMachine:error:"), 0, (id)text, (id)url, NULL, vm, &error);
            CFRelease(text);
        }
    }
    CFRelease(url);
    return script;
}

/* Gives the JSScript its cache's file (the JSScript owns the URL). */
static void tv_jsc_set_file(id script, const char *file) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)file, (CFIndex)strlen(file), false);
    CFURLRef *slot = (CFURLRef *)((char *)script + tv_jsc.cache_path);
    if (*slot) CFRelease(*slot);
    *slot = url;
}

/* Module i's function from its cache, if it has one; if it hasn't, it's queued to be written. */
static bool tv_jsc_load(JSContextRef ctx, uint32_t i, JSValueRef *out, JSValueRef *exc) {
    size_t len;
    tv_js_module_src(i, &len);
    if (len < TV_JSC_MIN_BYTES || !tv_jsc_on()) return false;
    char base[1100], file[1110], text[1110];
    tv_jsc_file(i, base, sizeof base);
    snprintf(file, sizeof file, "%s.jsc", base);
    snprintf(text, sizeof text, "%s.js", base);
    struct stat st;
    if (stat(file, &st) != 0 || st.st_size == 0) {
        CFURLRef url = tv_jsc_url(i);
        if (url) {
            CFRelease(url);
            tv_jsc_queue(i);
        }
        return false;
    }
    bool mapped = stat(text, &st) == 0 && (size_t)st.st_size == len;
    void *pool = objc_autoreleasePoolPush();
    bool done = false;
    id script = tv_jsc_script(i, tv_jsc.vm, mapped ? text : NULL);
    if (script) {
        tv_jsc_set_file(script, file);
        ((void (*)(id, SEL))objc_msgSend)(script, sel_registerName("readCache"));
        if (((BOOL (*)(id, SEL))objc_msgSend)(script, sel_registerName("isUsingBytecodeCache"))) {
            id value = ((id (*)(id, SEL, id))objc_msgSend)(tv_jsc.context, sel_registerName("evaluateJSScript:"), script);
            id thrown = tv_jsc_send(tv_jsc.context, "exception");
            if (thrown) {
                if (exc) *exc = ((JSValueRef (*)(id, SEL))objc_msgSend)(thrown, sel_registerName("JSValueRef"));
                ((void (*)(id, SEL, id))objc_msgSend)(tv_jsc.context, sel_registerName("setException:"), NULL);
                *out = NULL;
            } else {
                *out = value ? ((JSValueRef (*)(id, SEL))objc_msgSend)(value, sel_registerName("JSValueRef")) : JSValueMakeUndefined(ctx);
            }
            done = true;
        } else {
            /* (out of date: the engine emptied it) */
            tv_jsc_queue(i);
        }
    }
    objc_autoreleasePoolPop(pool);
    return done;
}

/* Writes modules' caches (in the writing process, in an engine of its own). */
static void tv_jsc_write(const uint32_t *modules, size_t n) {
    void *pool = objc_autoreleasePoolPush();
    id vm = tv_jsc_send(tv_jsc_send((id)objc_getClass("JSVirtualMachine"), "alloc"), "init");
    for (size_t k = 0; vm && k < n; k++) {
        void *inner = objc_autoreleasePoolPush();
        uint32_t i = modules[k];
        CFURLRef url = tv_jsc_url(i);
        if (!url) {
            objc_autoreleasePoolPop(inner);
            continue;
        }
        CFRelease(url);
        char base[1100], file[1110], text[1110], tmp[1200];
        tv_jsc_file(i, base, sizeof base);
        snprintf(file, sizeof file, "%s.jsc", base);
        snprintf(text, sizeof text, "%s.js", base);
        snprintf(tmp, sizeof tmp, "%s.%d.tmp", base, (int)getpid());
        /* the text first (when it's ASCII), then the bytecode of the text as mapped */
        size_t len;
        const unsigned char *src = (const unsigned char *)tv_js_module_src(i, &len);
        bool ascii = true;
        for (size_t j = 0; j < len && ascii; j++) ascii = src[j] < 0x80;
        const char *mapped = NULL;
        if (ascii) {
            FILE *f = fopen(tmp, "wb");
            if (f) {
                bool ok = fwrite(src, 1, len, f) == len;
                if (fclose(f) == 0 && ok && rename(tmp, text) == 0) mapped = text;
                else unlink(tmp);
            }
        }
        id script = tv_jsc_script(i, vm, mapped);
        if (script) {
            tv_jsc_set_file(script, tmp);
            id error = NULL;
            if (((BOOL (*)(id, SEL, id *))objc_msgSend)(script, sel_registerName("cacheBytecodeWithError:"), &error)) rename(tmp, file);
            else unlink(tmp);
        }
        objc_autoreleasePoolPop(inner);
    }
    if (vm) CFRelease(vm);
    objc_autoreleasePoolPop(pool);
}

#else
/* Module i's function: from its cache if it has one (if it hasn't, or it's out of date, it's
 * queued to be written); either way the text read where it is in the program. */
static JSValueRef tv_jsc_eval(JSContextRef ctx, uint32_t i, bool *cached, JSValueRef *exc) {
    size_t len;
    const char *src = tv_js_module_src(i, &len);
    int fd = -1;
    char base[1100], file[1110];
    if (len >= TV_JSC_MIN_BYTES && tv_jsc_on()) {
        tv_jsc_file(i, base, sizeof base);
        snprintf(file, sizeof file, "%s.jsc", base);
        fd = open(file, O_RDONLY | O_CLOEXEC);
    }
    JSValueRef r = TVEvaluateScript(ctx, src, len, tv_js_module_name(i), fd, cached, exc);
    if (fd >= 0) close(fd);
    if (len >= TV_JSC_MIN_BYTES && tv_jsc.state > 0 && !*cached) tv_jsc_queue(i);
    return r;
}

/* Writes modules' caches (in the writing process, in an engine of its own). */
static void tv_jsc_write(const uint32_t *modules, size_t n) {
    JSContextGroupRef group = JSContextGroupCreate();
    for (size_t k = 0; k < n; k++) {
        uint32_t i = modules[k];
        char base[1100], file[1110], tmp[1200];
        tv_jsc_file(i, base, sizeof base);
        snprintf(file, sizeof file, "%s.jsc", base);
        snprintf(tmp, sizeof tmp, "%s.%d.tmp", base, (int)getpid());
        int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) continue;
        size_t len;
        const char *src = tv_js_module_src(i, &len);
        bool ok = TVWriteBytecode(group, src, len, tv_js_module_name(i), fd);
        if (close(fd) == 0 && ok) rename(tmp, file);
        else unlink(tmp);
    }
    JSContextGroupRelease(group);
}
#endif

/* Starts the program again to write the queued modules' caches (TOV_JS_CACHE_WRITE=modules),
 * detached: by way of a process that starts it and exits at once (so it's never this program's
 * child to wait for), with nothing of this one's open (its sockets, its terminal). */
static void tv_jsc_flush(void) {
    if (tv_jsc.nqueue == 0) return;
    char exe[1024];
    size_t cap = 32 + tv_jsc.nqueue * 11;
    char *var = malloc(cap);
    if (!var || !tv_jsc_exe(exe, sizeof exe)) {
        free(var);
        return;
    }
    size_t at = (size_t)snprintf(var, cap, "TOV_JS_CACHE_WRITE=spawn");
    for (size_t k = 0; k < tv_jsc.nqueue; k++) at += (size_t)snprintf(var + at, cap - at, ",%u", tv_jsc.queue[k]);
    tv_jsc.nqueue = 0;
    size_t nenv = 0;
    while (environ[nenv]) nenv++;
    char **env = malloc((nenv + 2) * sizeof *env);
    if (!env) {
        free(var);
        return;
    }
    size_t m = 0;
    for (size_t k = 0; k < nenv; k++)
        if (strncmp(environ[k], "TOV_JS_CACHE_WRITE=", sizeof "TOV_JS_CACHE_WRITE=" - 1) != 0) env[m++] = environ[k];
    env[m++] = var;
    env[m] = NULL;
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    for (int fd = 0; fd < 3; fd++) posix_spawn_file_actions_addopen(&files, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef __APPLE__
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
#else
    posix_spawn_file_actions_addclosefrom_np(&files, 3);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
#endif
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    tv_jsc_low_priority(&attr);
    char *argv[] = {exe, NULL};
    pid_t pid;
    if (posix_spawn(&pid, exe, &files, &attr, argv, env) == 0)
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    free(env);
    free(var);
}

static void tv_jsc_at_exit(void) { tv_jsc_flush(); }

/* The program run to write caches (see tv_jsc_flush): before anything of the program runs. */
__attribute__((constructor)) static void tv_jsc_writer(void) {
    const char *what = getenv("TOV_JS_CACHE_WRITE");
    if (!what) return;
    if (strncmp(what, "spawn,", 6) == 0) {
        /* the go-between: starts the writer and exits */
        size_t n = strlen(what);
        char *var = malloc(n + 32);
        if (!var) _exit(0);
        snprintf(var, n + 32, "TOV_JS_CACHE_WRITE=write,%s", what + 6);
        for (char **e = environ; *e; e++)
            if (strncmp(*e, "TOV_JS_CACHE_WRITE=", sizeof "TOV_JS_CACHE_WRITE=" - 1) == 0) *e = var;
        char exe[1024];
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        tv_jsc_low_priority(&attr);
        char *argv[] = {exe, NULL};
        pid_t pid;
        if (tv_jsc_exe(exe, sizeof exe)) posix_spawn(&pid, exe, NULL, &attr, argv, environ);
        _exit(0);
    }
    if (strncmp(what, "write,", 6) != 0 || !tv_jsc_on()) _exit(0);
#ifndef __APPLE__
    /* (the cache is written in the background: at the lowest priority) */
    nice(19);
#endif
    size_t n = 0, cap = 0;
    uint32_t *modules = NULL;
    for (const char *p = what + 6; *p;) {
        char *end;
        unsigned long i = strtoul(p, &end, 10);
        if (end == p) break;
        if (i < tv_js_nmodules()) {
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                uint32_t *q = realloc(modules, cap * sizeof *q);
                if (!q) break;
                modules = q;
            }
            modules[n++] = (uint32_t)i;
        }
        p = *end == ',' ? end + 1 : end;
    }
    tv_jsc_write(modules, n);
    _exit(0);
}
#endif

/* This program's executable file. */
static bool tv_js_exe_path(char *out, size_t cap) {
#ifdef __APPLE__
    uint32_t size = (uint32_t)cap;
    return _NSGetExecutablePath(out, &size) == 0;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return false;
    out[n] = 0;
    return true;
#endif
}

/* globalThis.__tov_compile(id): module `id`'s function, compiled on first require with the
 * module's name as its URL (stack traces name the file; line numbers are the file's). */
static JSValueRef tv_js_compile_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self;
    double d = n > 0 ? JSValueToNumber(ctx, a[0], NULL) : -1;
    if (!(d >= 0 && d < tv_js_nmodules())) return JSValueMakeUndefined(ctx);
    uint32_t i = (uint32_t)d;
    /* TOV_JS_TRACE=1 lists the modules as they're compiled (what a program loads at start) */
    static int trace = -1;
    if (trace < 0) trace = getenv("TOV_JS_TRACE") != NULL;
    double t0 = trace ? tv_performance_now() : 0;
    JSValueRef r;
    bool cached = false;
#ifdef TV_JSC_OWN
    r = tv_jsc_eval(ctx, i, &cached, exc);
#else
#ifdef TV_JSC_SYSTEM
    cached = tv_jsc_load(ctx, i, &r, exc);
#endif
    if (!cached) {
        JSStringRef src = tv_js_module_text(i);
        JSStringRef url = JSStringCreateWithUTF8CString(tv_js_module_name(i));
        r = JSEvaluateScript(ctx, src, NULL, url, 1, exc);
        JSStringRelease(src);
        JSStringRelease(url);
    }
#endif
    if (trace) {
        char line[512];
        size_t len;
        tv_js_module_src(i, &len);
        int k = snprintf(line, sizeof line, "tov: %s %s (%zu bytes) in %.3f ms\n", cached ? "loaded" : "compiled", tv_js_module_name(i), len, tv_performance_now() - t0);
        tv_write_fd(2, line, (size_t)k);
    }
    return r;
}

/* globalThis.__tov_read_file(path[, exists]): a file's text (UTF-8), or undefined. For requires of files
 * the bundle doesn't hold (`require(dir + "/package.json")`): read where the program runs, as
 * Node.js does. */
static JSValueRef tv_js_read_file_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self; (void)exc;
    if (n == 0 || !JSValueIsString(ctx, a[0])) return JSValueMakeUndefined(ctx);
    tv_str path = tv_js_to_str(a[0]);
    FILE *fp = fopen(path.p->data, "rb");
    tv_str_release(path);
    if (!fp) return JSValueMakeUndefined(ctx);
    /* (a second argument: whether it's there, without reading it) */
    if (n > 1 && JSValueToBoolean(ctx, a[1])) {
        fclose(fp);
        return JSValueMakeBoolean(ctx, true);
    }
    tv_sb sb = {0};
    char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, fp)) > 0) tv_sb_push(&sb, buf, got);
    fclose(fp);
    JSStringRef t = tv_js_string_ref(sb.data ? sb.data : "", sb.len, false);
    tv_sb_free(&sb);
    JSValueRef v = JSValueMakeString(ctx, t);
    JSStringRelease(t);
    return v;
}

/* globalThis.__tov_compile_source(code, url): a CommonJS module function compiled from source
 * (a file read from disk, see above). */
static JSValueRef tv_js_compile_source_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self;
    if (n < 2) return JSValueMakeUndefined(ctx);
    JSStringRef code = JSValueToStringCopy(ctx, a[0], exc);
    JSStringRef url = JSValueToStringCopy(ctx, a[1], exc);
    if (!code || !url) return NULL;
    /* (function (module, exports, require, __filename, __dirname) {<code>\n}) */
    JSStringRef params[5] = {JSStringCreateWithUTF8CString("module"), JSStringCreateWithUTF8CString("exports"), JSStringCreateWithUTF8CString("require"), JSStringCreateWithUTF8CString("__filename"), JSStringCreateWithUTF8CString("__dirname")};
    JSObjectRef fn = JSObjectMakeFunction(ctx, NULL, 5, params, code, url, 1, exc);
    for (int i = 0; i < 5; i++) JSStringRelease(params[i]);
    JSStringRelease(code);
    JSStringRelease(url);
    return fn;
}

/* globalThis.__tov_source(url): the code of the module at `url` (assert reads the failing
 * expression from it), else undefined. */
static JSValueRef tv_js_source_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self;
    if (n == 0 || !JSValueIsString(ctx, a[0])) return JSValueMakeUndefined(ctx);
    tv_str url = tv_js_to_str(a[0]);
    JSValueRef r = JSValueMakeUndefined(ctx);
    if (strcmp(url.p->data, "tov:npm") == 0) {
        JSStringRef t = JSStringCreateWithUTF8CString(tv_js_prelude());
        r = JSValueMakeString(ctx, t);
        JSStringRelease(t);
    } else {
        for (uint32_t i = 0, n = tv_js_nmodules(); i < n; i++) {
            if (strcmp(tv_js_module_name(i), url.p->data) == 0) {
                JSStringRef t = tv_js_module_text(i);
                r = JSValueMakeString(ctx, t);
                JSStringRelease(t);
                break;
            }
        }
    }
    tv_str_release(url);
    (void)exc;
    return r;
}

void tv_js_def(JSContextRef ctx, JSObjectRef obj, const char *name, JSObjectCallAsFunctionCallback fn) {
    JSStringRef k = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx, obj, k, JSObjectMakeFunctionWithCallback(ctx, k, fn), kJSPropertyAttributeDontEnum, NULL);
    JSStringRelease(k);
}

static _Noreturn void tv_js_fatal(const char *what, JSValueRef exc) {
    tv_str text = tv_js_error_text(exc);
    tv_err_cstr(what);
    tv_err_cstr(text.p->data);
    tv_err_cstr("\n");
    exit(1);
}

static void tv_js_trace_phase(const char *what, double since) {
    static int trace = -1;
    if (trace < 0) trace = getenv("TOV_JS_TRACE") != NULL;
    if (!trace) return;
    char line[160];
    int k = snprintf(line, sizeof line, "tov: %s in %.3f ms (%.3f ms since start)\n", what, tv_performance_now() - since, tv_performance_now());
    tv_write_fd(2, line, (size_t)k);
}

#if defined(TV_JSC_OWN)
/* JavaScriptCore's timers (the incremental sweeper, the next full collection, FinalizationRegistry
 * callbacks) and what its threads hand back (WebAssembly compiled off the thread) are on this
 * thread's run loop (WTF's own, TovAPI.h), which runs only when the event loop runs it. */
static uint64_t tv_js_host_due(uint64_t now_ms) {
    double wait = TVRunLoopSecondsUntilWork();
    if (wait < 0) return 0;
    if (wait == 0) return now_ms ? now_ms : 1;
    /* (+1: rounding down would wake it a little early) */
    return now_ms + (uint64_t)(wait < 3600 ? wait * 1000 : 3600000) + 1;
}

static void tv_js_host_run(void) {
    TVRunLoopCycle();
}
#elif defined(__APPLE__)
/* JavaScriptCore's timers (the incremental sweeper, the next full collection, FinalizationRegistry
 * callbacks) are on this thread's CFRunLoop, which runs only when the event loop runs it: without
 * them, garbage swept and collected only as memory runs out stays held. */
static uint64_t tv_js_host_due(uint64_t now_ms) {
    CFAbsoluteTime at = CFRunLoopGetNextTimerFireDate(CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    if (at == 0) return 0;
    double wait = (at - CFAbsoluteTimeGetCurrent()) * 1000;
    if (wait <= 0) return now_ms ? now_ms : 1;
    /* (+1: rounding down would wake it a little early) */
    return now_ms + (uint64_t)(wait < 3600000 ? wait : 3600000) + 1;
}

static void tv_js_host_run(void) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);
}
#else
/* WebKitGTK's JavaScriptCore runs its timers and the work its threads hand back (the same:
 * collections, FinalizationRegistry callbacks, WebAssembly compiled off the main thread) on a
 * GLib main context, which runs only when the event loop runs it: the thread-default context
 * when the engine starts, so one of the program's own (tv_js_glib, made then; without one the
 * engine makes one no one runs, the thread not being GLib's main thread to it). */
#include <glib.h>

static GMainContext *tv_js_glib;

static uint64_t tv_js_host_due(uint64_t now_ms) {
    GMainContext *c = tv_js_glib;
    if (!g_main_context_acquire(c)) return 0;
    gint priority, timeout;
    gboolean ready = g_main_context_prepare(c, &priority);
    GPollFD fds[16];
    gint n = g_main_context_query(c, priority, &timeout, fds, 16);
    if (n > 16) n = 16;
    /* (a cycle is finished before the next starts; what's ready is dispatched by tv_js_host_run) */
    for (gint i = 0; i < n; i++) fds[i].revents = 0;
    g_main_context_check(c, priority, fds, n);
    g_main_context_release(c);
    if (ready || timeout == 0) return now_ms ? now_ms : 1;
    if (timeout < 0) return 0;
    return now_ms + (uint64_t)timeout;
}

static void tv_js_host_run(void) {
    /* (what's ready now, not what running it makes ready: that's the next turn's) */
    for (int i = 0; i < 64 && g_main_context_iteration(tv_js_glib, FALSE); i++) {}
}
#endif

#ifdef TV_JSC_OWN
static void tv_js_will_block(void) {
    if (tv_js_ctx) TVReleaseStickyLock(tv_js_ctx);
}
#endif

/* (JavaScriptCore's, exported though not in its headers) */
extern void JSSynchronousGarbageCollectForDebugging(JSContextRef ctx) __attribute__((weak_import));

/* The engine's own functions by name: with Tov's own engine linked in, referred to directly (a
 * program exports no symbols to look up); otherwise looked up in the engine's library. */
#ifdef TV_JSC_OWN
#ifdef __APPLE__
#define TV_JSC_CXX(name) "_" name
#else
#define TV_JSC_CXX(name) name
#endif
extern void tv_jsc_delete_all_code(void *vm, int effort) __asm__(TV_JSC_CXX("_ZN3JSC2VM13deleteAllCodeENS_19DeleteAllCodeEffortE"));
extern void tv_jsc_release_free_memory(void) __asm__(TV_JSC_CXX("_ZN3WTF27releaseFastMallocFreeMemoryEv"));
#define tv_js_engine_fn(name, own) ((void *)(own))
#else
#define tv_js_engine_fn(name, own) dlsym(RTLD_DEFAULT, name)
#endif

/* VM::deleteAllCode(DeleteAllCodeIfNotCollecting), JavaScriptCore's (exported, not in its headers;
 * the context group is the VM). Called from a native function, as it needs the engine's lock and
 * runs once JavaScript returns. */
static JSValueRef tv_js_drop_code_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)f; (void)self; (void)n; (void)a; (void)exc;
    static void (*drop)(void *vm, int effort);
    static bool looked;
    if (!looked) {
        looked = true;
        drop = (void (*)(void *, int))tv_js_engine_fn("_ZN3JSC2VM13deleteAllCodeENS_19DeleteAllCodeEffortE", tv_jsc_delete_all_code);
    }
    if (drop) drop((void *)JSContextGetGroup(ctx), 1);
    return JSValueMakeUndefined(ctx);
}

/* The loop is idle after work (tv_loop_idle): a full collection, and the memory it frees back to
 * the system; deep, the compiled code first (recompiled when it runs again, at a few ms' cost to
 * the next burst: after one, an http server holds ~32 MB without either, ~24 MB with the
 * collection, ~14 MB with the code dropped too). */
static void tv_js_idle(bool deep) {
    static void (*release)(void);
    static JSObjectRef drop_code;
    if (!drop_code) {
        release = (void (*)(void))tv_js_engine_fn("_ZN3WTF27releaseFastMallocFreeMemoryEv", tv_jsc_release_free_memory);
        drop_code = JSObjectMakeFunctionWithCallback(tv_js_ctx, NULL, tv_js_drop_code_fn);
        JSValueProtect(tv_js_ctx, drop_code);
    }
#ifdef TV_JSC_OWN
    /* (TovAPI: and the blocks the collection emptied are freed at once, so their memory goes back
     * now rather than as the engine's sweeper and scavenger get to it, seconds later) */
    (void)release;
    TVReleaseMemory(tv_js_ctx, deep);
#else
    if (deep) JSObjectCallAsFunction(tv_js_ctx, drop_code, NULL, 0, NULL, NULL);
    if (JSSynchronousGarbageCollectForDebugging) JSSynchronousGarbageCollectForDebugging(tv_js_ctx);
    if (release) release();
#endif
    /* (and what the system allocator holds freed: native addons' and the runtime's own) */
#ifdef __APPLE__
    malloc_zone_pressure_relief(NULL, 0);
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
#if defined(TV_JSC_OWN) || defined(__APPLE__)
    tv_jsc_flush();
#endif
}

/* TOV_JS_PROFILE=<file>: JavaScriptCore's sampling profiler runs from the start, and its stack
 * traces (JSON) go to the file at exit, for scripts/js-profile.py (exported, not in headers) */
extern bool JSContextGroupEnableSamplingProfiler(JSContextGroupRef group) __attribute__((weak_import));
extern JSStringRef JSContextGroupTakeSamplesFromSamplingProfiler(JSContextGroupRef group) __attribute__((weak_import));

static void tv_js_profile_write(void) {
    const char *path = getenv("TOV_JS_PROFILE");
    if (!path || !tv_js_ctx || !JSContextGroupTakeSamplesFromSamplingProfiler) return;
    JSStringRef json = JSContextGroupTakeSamplesFromSamplingProfiler(JSContextGetGroup(tv_js_ctx));
    if (!json) return;
    size_t cap = JSStringGetMaximumUTF8CStringSize(json);
    char *buf = malloc(cap);
    size_t n = buf ? JSStringGetUTF8CString(json, buf, cap) : 0;
    FILE *f = fopen(path, "w");
    if (f && n) fwrite(buf, 1, n - 1, f);
    if (f) fclose(f);
    free(buf);
    JSStringRelease(json);
}

JSContextRef tv_js_start(void) {
    if (tv_js_ctx) return tv_js_ctx;
    double t0 = tv_performance_now();
    /* JavaScriptCore's options come from the environment when the first VM starts, and only then
     * (so the program and its children never see the variables; one set already wins):
     * SharedArrayBuffer, which it leaves out of API contexts unless asked; and one compiler
     * thread per optimizing tier, as what a compiler thread allocates stays held until the
     * allocator scavenges it (an http server's hot paths tier up at ~70 MB with the default
     * threads, ~52 MB with one each, at the same speed); and a heap let grow to about twice what
     * survives a full collection, not four times (the engine's choice on a machine with 16 GB or
     * more, for a browser), marked by two threads, not one per core (more spin more than they
     * mark on a server's small heap): Cap's media server probes at 86 MB, not 130, peaking at 126
     * MB, not 164, on 10% more CPU, still a quarter less than Bun's */
    static const char *const options[][2] = {
        {"JSC_useSharedArrayBuffer", "1"},
        {"JSC_numberOfDFGCompilerThreads", "1"},
        {"JSC_numberOfFTLCompilerThreads", "1"},
        {"JSC_heapGrowthMaxIncrease", "1"},
        {"JSC_numberOfGCMarkers", "2"},
    };
    enum { noptions = sizeof options / sizeof options[0] };
    bool set[noptions];
    for (int i = 0; i < noptions; i++) {
        set[i] = !getenv(options[i][0]);
        if (set[i]) setenv(options[i][0], options[i][1], 0);
    }
#if !defined(__APPLE__) && !defined(TV_JSC_OWN)
    tv_js_glib = g_main_context_new();
    g_main_context_push_thread_default(tv_js_glib);
#endif
#ifdef TV_JSC_OWN
    TVInitialize();
#endif
    JSGlobalContextRef ctx = JSGlobalContextCreate(NULL);
    for (int i = 0; i < noptions; i++)
        if (set[i]) unsetenv(options[i][0]);
    tv_js_trace_phase("created the JavaScript context", t0);
    tv_js_ctx = ctx;
    if (getenv("TOV_JS_PROFILE") && JSContextGroupEnableSamplingProfiler && JSContextGroupEnableSamplingProfiler(JSContextGetGroup(ctx)))
        atexit(tv_js_profile_write);
#ifdef TV_JSC_OWN
    /* the engine's lock kept between calls (a call into the engine otherwise takes it and gives
     * it back: ~25% of a million small calls into a package), given back when the thread blocks */
    TVSetStickyLock(ctx);
    tv_loop_will_block = tv_js_will_block;
#endif
    tv_loop_host_due = tv_js_host_due;
    tv_loop_host_run = tv_js_host_run;
    tv_loop_idle = tv_js_idle;
    /* values made without the engine (js.h): only if it encodes them as expected */
    tv_js_encoded = tv_js_bits(JSValueMakeNumber(ctx, 1.5)) == 0x3ffa000000000000ull && tv_js_bits(JSValueMakeNumber(ctx, -1)) == 0xfffe0000ffffffffull
        && tv_js_bits(JSValueMakeBoolean(ctx, true)) == TV_JS_TRUE && tv_js_bits(JSValueMakeBoolean(ctx, false)) == TV_JS_FALSE
        && tv_js_bits(JSValueMakeUndefined(ctx)) == TV_JS_UNDEFINED && tv_js_bits(JSValueMakeNull(ctx)) == TV_JS_NULL;
    JSObjectRef global = JSContextGetGlobalObject(ctx);
    JSObjectRef native = JSObjectMake(ctx, NULL, NULL);
    JSStringRef k = JSStringCreateWithUTF8CString("__tov_native");
    JSObjectSetProperty(ctx, global, k, native, kJSPropertyAttributeDontEnum, NULL);
    JSStringRelease(k);
    tv_node_install(ctx, native);
    tv_js_def(ctx, global, "__tov_source", tv_js_source_fn);
    tv_js_def(ctx, global, "__tov_compile", tv_js_compile_fn);
    tv_js_def(ctx, global, "__tov_name", tv_js_name_fn);
    tv_js_def(ctx, global, "__tov_read_file", tv_js_read_file_fn);
    if (tv_js_napi_install) tv_js_napi_install(ctx);
    tv_js_def(ctx, global, "__tov_compile_source", tv_js_compile_source_fn);
    tv_js_def(ctx, global, "__tov_map", tv_js_map_fn);
    {
        JSStringRef k = JSStringCreateWithUTF8CString("__tov_count");
        JSObjectSetProperty(ctx, global, k, JSValueMakeNumber(ctx, tv_js_nmodules()), kJSPropertyAttributeDontEnum, NULL);
        JSStringRelease(k);
    }
    tv_js_noop = JSObjectMakeFunctionWithCallback(ctx, NULL, tv_js_noop_fn);
    JSValueProtect(ctx, tv_js_noop);
    JSStringRef prelude = JSStringCreateWithUTF8CString(tv_js_prelude());
    tv_js_bundle_url = JSStringCreateWithUTF8CString("tov:npm");
    JSValueRef exc = NULL;
    double t1 = tv_performance_now();
    JSEvaluateScript(ctx, prelude, NULL, tv_js_bundle_url, 1, &exc);
    tv_js_trace_phase("ran the bundle's prelude (and the globals)", t1);
    JSStringRelease(prelude);
    if (exc) tv_js_fatal("npm packages failed to load: ", exc);
    k = JSStringCreateWithUTF8CString("__tov_npm");
    JSValueRef npm = JSObjectGetProperty(ctx, global, k, NULL);
    JSStringRelease(k);
    tv_js_npm = (JSObjectRef)npm;
    JSValueProtect(ctx, npm);
    return ctx;
}

void tv_js_drain(void) {
    if (tv_js_ctx) JSObjectCallAsFunction(tv_js_ctx, tv_js_noop, NULL, 0, NULL, NULL);
}

/* ------------------------------------------------------------------ values */


/* UTF-8 → UTF-16, invalid sequences as U+FFFD (WHATWG decoding). Returns the length. */
static size_t tv_utf8_to_utf16(const unsigned char *s, size_t n, JSChar *out) {
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c < 0x80) { out[o++] = (JSChar)c; i++; continue; }
        unsigned need, cp, lo = 0x80, hi = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) { need = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; cp = c & 0x0f; if (c == 0xe0) lo = 0xa0; if (c == 0xed) hi = 0x9f; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; cp = c & 0x07; if (c == 0xf0) lo = 0x90; if (c == 0xf4) hi = 0x8f; }
        else { out[o++] = 0xfffd; i++; continue; }
        i++;
        bool bad = false;
        for (unsigned k = 0; k < need; k++) {
            if (i >= n || s[i] < lo || s[i] > hi) { bad = true; break; }
            cp = (cp << 6) | (s[i] & 0x3f);
            lo = 0x80; hi = 0xbf;
            i++;
        }
        if (bad) { out[o++] = 0xfffd; continue; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[o++] = (JSChar)(0xd800 + (cp >> 10));
            out[o++] = (JSChar)(0xdc00 + (cp & 0x3ff));
        } else {
            out[o++] = (JSChar)cp;
        }
    }
    return o;
}

/* Valid UTF-8 without NUL bytes (JavaScriptCore reads it as a C string, 8-bit when it can). */
static bool tv_utf8_plain(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c >= 0x01 && c < 0x80) { i++; continue; }
        if (c < 0xc2 || c > 0xf4) return false;   /* (NUL included) */
        size_t need = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
        if (n - i <= need) return false;
        unsigned lo = 0x80, hi = 0xbf;
        if (c == 0xe0) lo = 0xa0; else if (c == 0xed) hi = 0x9f; else if (c == 0xf0) lo = 0x90; else if (c == 0xf4) hi = 0x8f;
        if (s[i + 1] < lo || s[i + 1] > hi) return false;
        for (size_t k = 2; k <= need; k++) if ((s[i + k] & 0xc0) != 0x80) return false;
        i += need + 1;
    }
    return true;
}

static JSStringRef tv_js_string_ref(const char *s, size_t n, bool nul_terminated) {
    if (nul_terminated && tv_utf8_plain((const unsigned char *)s, n)) return JSStringCreateWithUTF8CString(s);
    JSChar small[256];
    JSChar *buf = n <= 256 ? small : tv_alloc(n * sizeof(JSChar));
    size_t len = tv_utf8_to_utf16((const unsigned char *)s, n, buf);
    JSStringRef r = JSStringCreateWithCharacters(buf, len);
    if (buf != small) tv_free(buf);
    return r;
}

JSValueRef tv_js_from_str(tv_str s) {
    JSContextRef ctx = tv_js();
    /* literals (immortal buffers): a small direct-mapped cache of their JavaScript strings */
    enum { LIT_CACHE = 256 };
    static struct { const tv_strbuf *p; JSValueRef v; } lits[LIT_CACHE];
    uint32_t slot = 0;
    if (s.p->rc < 0) {
        slot = (uint32_t)(((uintptr_t)s.p >> 4) * 0x9e3779b97f4a7c15ull >> 56);
        if (lits[slot].p == s.p) return lits[slot].v;
    }
    JSStringRef r = tv_js_string_ref(s.p->data, (size_t)s.p->len, true);
    JSValueRef v = JSValueMakeString(ctx, r);
    JSStringRelease(r);
    if (s.p->rc < 0) {
        if (lits[slot].v) JSValueUnprotect(ctx, lits[slot].v);
        JSValueProtect(ctx, v);
        lits[slot].p = s.p;
        lits[slot].v = v;
    }
    return v;
}

JSValueRef tv_js_str(const char *s, size_t n) {
    JSContextRef ctx = tv_js();
    JSStringRef r = tv_js_string_ref(s, n, false);
    JSValueRef v = JSValueMakeString(ctx, r);
    JSStringRelease(r);
    return v;
}

static tv_str tv_js_string_to_str(JSStringRef r) {
    size_t len = JSStringGetLength(r);
    const JSChar *u = JSStringGetCharactersPtr(r);
    /* UTF-16 → UTF-8 with lone surrogates as U+FFFD (as TextEncoder does) */
    tv_sb sb = {0};
    tv_sb_grow(&sb, len + 1);
    for (size_t i = 0; i < len; i++) {
        unsigned c = u[i];
        if (c < 0x80) { tv_sb_add_char(&sb, (char)c); continue; }
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < len && u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (u[i + 1] - 0xdc00);
            i++;
        } else if (c >= 0xd800 && c <= 0xdfff) {
            c = 0xfffd;
        }
        char b[4];
        size_t k;
        if (c < 0x800) { b[0] = (char)(0xc0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3f)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xe0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3f)); b[2] = (char)(0x80 | (c & 0x3f)); k = 3; }
        else { b[0] = (char)(0xf0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3f)); b[2] = (char)(0x80 | ((c >> 6) & 0x3f)); b[3] = (char)(0x80 | (c & 0x3f)); k = 4; }
        tv_sb_add(&sb, b, k);
    }
    return tv_str_from_sb(&sb);
}

tv_str tv_js_to_str(JSValueRef v) {
    JSContextRef ctx = tv_js();
    JSValueRef exc = NULL;
    JSStringRef r = JSValueToStringCopy(ctx, v, &exc);
    if (!r) return tv_str_from("[object]", 8);
    tv_str s = tv_js_string_to_str(r);
    JSStringRelease(r);
    return s;
}

const char *tv_js_typeof(JSValueRef v) {
    JSContextRef ctx = tv_js();
    switch (JSValueGetType(ctx, v)) {
    case kJSTypeUndefined: return "undefined";
    case kJSTypeNull: return "object";
    case kJSTypeBoolean: return "boolean";
    case kJSTypeNumber: return "number";
    case kJSTypeString: return "string";
    case kJSTypeSymbol: return "symbol";
#if defined(MAC_OS_VERSION_15_0) || defined(kJSTypeBigInt)
    case kJSTypeBigInt: return "bigint";
#endif
    case kJSTypeObject: return JSObjectIsFunction(ctx, (JSObjectRef)v) ? "function" : "object";
    }
    return "object";
}

JSValueRef tv_js_num_slow(double d) { return JSValueMakeNumber(tv_js(), d); }
JSValueRef tv_js_simple_slow(uint64_t bits) {
    JSContextRef ctx = tv_js();
    switch (bits) {
    case TV_JS_TRUE: return JSValueMakeBoolean(ctx, true);
    case TV_JS_FALSE: return JSValueMakeBoolean(ctx, false);
    case TV_JS_NULL: return JSValueMakeNull(ctx);
    default: return JSValueMakeUndefined(ctx);
    }
}

bool tv_js_as_num_slow(JSValueRef v, double *out) {
    JSContextRef ctx = tv_js();
    if (!JSValueIsNumber(ctx, v)) return false;
    *out = JSValueToNumber(ctx, v, NULL);
    return true;
}

bool tv_js_as_bool_slow(JSValueRef v, bool *out) {
    JSContextRef ctx = tv_js();
    if (!JSValueIsBoolean(ctx, v)) return false;
    *out = JSValueToBoolean(ctx, v);
    return true;
}

bool tv_js_as_str(JSValueRef v, tv_str *out) {
    JSContextRef ctx = tv_js();
    if (!JSValueIsString(ctx, v)) return false;
    JSStringRef r = JSValueToStringCopy(ctx, v, NULL);
    *out = tv_js_string_to_str(r);
    JSStringRelease(r);
    return true;
}

/* tv_type_js: Js values in Tov containers */
static void tv_js_t_retain(void *p) { tv_js_retain(*(JSValueRef *)p); }
static void tv_js_t_release(void *p) { tv_js_release(*(JSValueRef *)p); }
static bool tv_js_t_eq(const void *a, const void *b) { return JSValueIsStrictEqual(tv_js_ctx, *(JSValueRef *)a, *(JSValueRef *)b); }
static uint64_t tv_js_t_hash(const void *p) {
    JSValueRef v = *(JSValueRef *)p;
    JSContextRef ctx = tv_js_ctx;
    if (JSValueIsString(ctx, v)) {
        tv_str s = tv_js_to_str(v);
        uint64_t h = tv_str_hash(s);
        tv_str_release(s);
        return h;
    }
    if (JSValueIsNumber(ctx, v)) {
        double d = JSValueToNumber(ctx, v, NULL);
        if (d == 0) d = 0; /* -0 === 0 */
        uint64_t u;
        memcpy(&u, &d, 8);
        return u * 0x9e3779b97f4a7c15ull;
    }
    return (uint64_t)(uintptr_t)v * 0x9e3779b97f4a7c15ull;
}
static void tv_js_t_to_str(tv_sb *sb, const void *p) {
    tv_str s = tv_js_to_str(*(JSValueRef *)p);
    tv_sb_push_str(sb, s);
    tv_str_release(s);
}
static void tv_js_t_inspect(tv_sb *sb, const void *p, int depth) {
    JSValueRef v = *(JSValueRef *)p;
    JSContextRef ctx = tv_js();
    /* Node.js's util.inspect when the bundle has node:util (programs that print JavaScript values
     * do), else JSON-ish */
    static JSValueRef inspect;
    static bool looked;
    if (!looked) {
        looked = true;
        JSValueRef exc = NULL;
        JSValueRef util = tv_js_import("node:util", &exc);
        static JSStringRef k;
        if (util && JSValueIsObject(ctx, util)) inspect = JSObjectGetProperty(ctx, (JSObjectRef)util, tv_js_key(&k, "inspect"), NULL);
        if (inspect && JSValueIsObject(ctx, inspect)) JSValueProtect(ctx, inspect); else inspect = NULL;
    }
    if (depth == 0 && JSValueIsString(ctx, v)) {
        tv_js_t_to_str(sb, p);
        return;
    }
    if (inspect) {
        JSValueRef r = JSObjectCallAsFunction(ctx, (JSObjectRef)inspect, NULL, 1, &v, NULL);
        if (r && JSValueIsString(ctx, r)) {
            tv_str s = tv_js_to_str(r);
            tv_sb_push_str(sb, s);
            tv_str_release(s);
            return;
        }
    }
    if (JSValueIsString(ctx, v)) {
        tv_str s = tv_js_to_str(v);
        tv_inspect_str(sb, s, depth);
        tv_str_release(s);
        return;
    }
    if (JSValueIsObject(ctx, v) && !JSObjectIsFunction(ctx, (JSObjectRef)v)) {
        JSStringRef json = JSValueCreateJSONString(ctx, v, 0, NULL);
        if (json) {
            tv_str s = tv_js_string_to_str(json);
            JSStringRelease(json);
            tv_sb_push_str(sb, s);
            tv_str_release(s);
            return;
        }
    }
    tv_js_t_to_str(sb, p);
}
const tv_type tv_type_js = {sizeof(JSValueRef), tv_js_t_retain, tv_js_t_release, tv_js_t_eq, tv_js_t_hash, tv_js_t_to_str, tv_js_t_inspect};

/* ------------------------------------------------------------------ calls and properties */

JSStringRef tv_js_key_slow(JSStringRef *slot, const char *name) {
    *slot = JSStringCreateWithUTF8CString(name);
    return *slot;
}

static JSValueRef tv_js_type_error(JSContextRef ctx, const char *msg, JSValueRef *exc) {
    JSValueRef m = tv_js_str(msg, strlen(msg));
    JSObjectRef e = JSObjectMakeError(ctx, 1, &m, NULL);
    static JSStringRef kname;
    JSObjectSetProperty(ctx, e, tv_js_key(&kname, "name"), tv_js_str("TypeError", 9), kJSPropertyAttributeDontEnum, NULL);
    *exc = e;
    return NULL;
}

JSValueRef tv_js_get(JSValueRef obj, JSStringRef key, JSValueRef *exc) {
    JSContextRef ctx = tv_js();
    JSObjectRef o = JSValueToObject(ctx, obj, exc);
    if (!o) return NULL;
    JSValueRef r = JSObjectGetProperty(ctx, o, key, exc);
    return *exc ? NULL : r;
}

bool tv_js_set(JSValueRef obj, JSStringRef key, JSValueRef value, JSValueRef *exc) {
    JSContextRef ctx = tv_js();
    JSObjectRef o = JSValueToObject(ctx, obj, exc);
    if (!o) return false;
    JSObjectSetProperty(ctx, o, key, value, kJSPropertyAttributeNone, exc);
    return !*exc;
}

JSValueRef tv_js_index(JSValueRef obj, uint32_t i, JSValueRef *exc) {
    JSContextRef ctx = tv_js();
    JSObjectRef o = JSValueToObject(ctx, obj, exc);
    if (!o) return NULL;
    JSValueRef r = JSObjectGetPropertyAtIndex(ctx, o, i, exc);
    return *exc ? NULL : r;
}

JSValueRef tv_js_call(JSValueRef fn, JSValueRef self, size_t n, const JSValueRef *args, JSValueRef *exc) {
    JSContextRef ctx = tv_js();
    if (!JSValueIsObject(ctx, fn) || !JSObjectIsFunction(ctx, (JSObjectRef)fn)) return tv_js_type_error(ctx, "not a function", exc);
    JSObjectRef this_obj = NULL;
    if (self && !JSValueIsUndefined(ctx, self)) {
        /* (a primitive `this` is boxed, as for sloppy-mode callees) */
        this_obj = JSValueToObject(ctx, self, NULL);
    }
    JSValueRef r = TV_JS_CALL(ctx, (JSObjectRef)fn, this_obj, n, args, exc);
    return *exc ? NULL : r;
}

JSValueRef tv_js_invoke(JSValueRef obj, JSStringRef key, size_t n, const JSValueRef *args, JSValueRef *exc) {
    JSValueRef fn = tv_js_get(obj, key, exc);
    if (!fn) return NULL;
    JSContextRef ctx = tv_js_ctx;
    if (!JSValueIsObject(ctx, fn) || !JSObjectIsFunction(ctx, (JSObjectRef)fn)) {
        char msg[200];
        tv_str k = tv_js_string_to_str(key);
        snprintf(msg, sizeof msg, "%.150s is not a function", k.p->data);
        tv_str_release(k);
        return tv_js_type_error(ctx, msg, exc);
    }
    JSValueRef r = TV_JS_CALL(ctx, (JSObjectRef)fn, JSValueToObject(ctx, obj, NULL), n, args, exc);
    return *exc ? NULL : r;
}

JSValueRef tv_js_new(JSValueRef fn, size_t n, const JSValueRef *args, JSValueRef *exc) {
    JSContextRef ctx = tv_js();
    if (!JSValueIsObject(ctx, fn) || !JSObjectIsConstructor(ctx, (JSObjectRef)fn)) return tv_js_type_error(ctx, "not a constructor", exc);
    JSObjectRef r = JSObjectCallAsConstructor(ctx, (JSObjectRef)fn, n, args, exc);
    return *exc ? NULL : r;
}

JSValueRef tv_js_import(const char *spec, JSValueRef *exc) {
    JSContextRef ctx = tv_js();
    JSValueRef arg = tv_js_str(spec, strlen(spec));
    JSValueRef r = JSObjectCallAsFunction(ctx, tv_js_npm, NULL, 1, &arg, exc);
    return *exc ? NULL : r;
}

tv_str tv_js_error_text(JSValueRef exc) {
    JSContextRef ctx = tv_js_ctx;
    tv_sb sb = {0};
    tv_str s = tv_js_to_str(exc);
    tv_sb_push_str(&sb, s);
    tv_str_release(s);
    if (JSValueIsObject(ctx, exc)) {
        static JSStringRef kstack;
        JSValueRef st = JSObjectGetProperty(ctx, (JSObjectRef)exc, tv_js_key(&kstack, "stack"), NULL);
        if (st && JSValueIsString(ctx, st)) {
            tv_str t = tv_js_to_str(st);
            /* JavaScriptCore's frames are "fn@url:line:col" lines */
            const char *p = t.p->data;
            while (*p) {
                const char *e = strchr(p, '\n');
                size_t n = e ? (size_t)(e - p) : strlen(p);
                if (n) {
                    tv_sb_push(&sb, "\n    at ", 8);
                    tv_sb_push(&sb, p, n);
                }
                p += n + (e ? 1 : 0);
            }
            tv_str_release(t);
        }
    }
    return tv_str_from_sb(&sb);
}

/* ------------------------------------------------------------------ bytes */

JSObjectRef tv_js_bytes(void *ptr, size_t len, void (*dealloc)(void *bytes, void *ctx), void *dctx) {
    return JSObjectMakeTypedArrayWithBytesNoCopy(tv_js(), kJSTypedArrayTypeUint8Array, ptr, len, dealloc, dctx, NULL);
}

static void tv_js_free_bytes(void *bytes, void *ctx) { (void)ctx; free(bytes); }

/* (in the engine's own heap, which it counts towards collections and gives back as it
 * scavenges: a malloc'd copy freed only when collected would fragment the system allocator) */
JSObjectRef tv_js_bytes_copy(const void *ptr, size_t len) {
    JSContextRef ctx = tv_js();
#ifdef TV_JSC_OWN
    /* (TovAPI's: the C API's pins the array's buffer once its bytes are asked for, so it can't
     * be transferred) */
    void *bytes;
    JSObjectRef own = TVMakeUint8Array(ctx, len, &bytes);
    if (!own) tv_trap("out of memory", "js");
    if (len) memcpy(bytes, ptr, len);
    return own;
#endif
    JSObjectRef a = JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array, len, NULL);
    if (!a) tv_trap("out of memory", "js");
    if (len) memcpy(JSObjectGetTypedArrayBytesPtr(ctx, a, NULL), ptr, len);
    return a;
}

bool tv_js_bytes_view(JSValueRef v, uint8_t **ptr, size_t *len) {
    JSContextRef ctx = tv_js();
    if (!JSValueIsObject(ctx, v)) return false;
    JSObjectRef o = (JSObjectRef)v;
    JSTypedArrayType t = JSValueGetTypedArrayType(ctx, v, NULL);
    if (t == kJSTypedArrayTypeNone) return false;
    if (t == kJSTypedArrayTypeArrayBuffer) {
        *ptr = JSObjectGetArrayBufferBytesPtr(ctx, o, NULL);
        *len = JSObjectGetArrayBufferByteLength(ctx, o, NULL);
        return true;
    }
    /* JSObjectGetTypedArrayBytesPtr points at the start of the backing ArrayBuffer: add the
     * view's byte offset (a Buffer is usually a slice of a shared pool) */
    *ptr = (uint8_t *)JSObjectGetTypedArrayBytesPtr(ctx, o, NULL) + JSObjectGetTypedArrayByteOffset(ctx, o, NULL);
    *len = JSObjectGetTypedArrayByteLength(ctx, o, NULL);
    return true;
}

bool tv_js_as_u8s(JSValueRef v, tv_arr *out) {
    JSContextRef ctx = tv_js();
    if (!JSValueIsObject(ctx, v)) return false;
    JSTypedArrayType t = JSValueGetTypedArrayType(ctx, v, NULL);
    if (t != kJSTypedArrayTypeUint8Array && t != kJSTypedArrayTypeUint8ClampedArray && t != kJSTypedArrayTypeArrayBuffer) return false;
    uint8_t *p;
    size_t len;
    if (!tv_js_bytes_view(v, &p, &len)) return false;
    tv_arr a = TV_EMPTY_ARR;
    if (len) {
        memcpy(tv_arr_reserve_tail(&a, &tv_type_u8, (tv_int)len), p, len);
        a.len = (tv_int)len;
    }
    *out = a;
    return true;
}

/* ------------------------------------------------------------------ promises */

struct tv_js_deferred {
    JSObjectRef resolve, reject;
};

JSObjectRef tv_js_deferred_new(tv_js_deferred **out) {
    JSContextRef ctx = tv_js();
    tv_js_deferred *d = tv_alloc(sizeof *d);
    JSObjectRef p = JSObjectMakeDeferredPromise(ctx, &d->resolve, &d->reject, NULL);
    JSValueProtect(ctx, d->resolve);
    JSValueProtect(ctx, d->reject);
    *out = d;
    return p;
}

void tv_js_settle(tv_js_deferred *d, JSValueRef value, bool ok) {
    JSContextRef ctx = tv_js_ctx;
    JSObjectCallAsFunction(ctx, ok ? d->resolve : d->reject, NULL, 1, &value, NULL);
    JSValueUnprotect(ctx, d->resolve);
    JSValueUnprotect(ctx, d->reject);
    tv_free(d);
}

/* tv_js_await: the two reactions passed to `then` share one record. */
typedef struct {
    tv_promise *p;      /* settled by the first reaction to run (held) */
    int refs;           /* reactions not yet finalized */
} tv_js_waiter;

static JSClassRef tv_js_fulfill_class, tv_js_reject_class;

static void tv_js_waiter_settle(JSContextRef ctx, JSObjectRef f, size_t n, const JSValueRef a[], bool ok) {
    tv_js_waiter *w = JSObjectGetPrivate(f);
    if (!w || !w->p) return;
    JSValueRef v = n > 0 ? a[0] : JSValueMakeUndefined(ctx);
    tv_promise *p = w->p;
    w->p = NULL;
    if (ok) {
        tv_promise_resolve(p, &v);
    } else {
        if (!tv_js_make_error) tv_js_fatal("uncaught ", v);
        tv_promise_reject(p, tv_js_make_error(v));
    }
    tv_promise_release(p);
}
static JSValueRef tv_js_fulfill_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)self; (void)exc;
    tv_js_waiter_settle(ctx, f, n, a, true);
    return JSValueMakeUndefined(ctx);
}
static JSValueRef tv_js_reject_fn(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)self; (void)exc;
    tv_js_waiter_settle(ctx, f, n, a, false);
    return JSValueMakeUndefined(ctx);
}
static void tv_js_waiter_finalize(JSObjectRef f) {
    tv_js_waiter *w = JSObjectGetPrivate(f);
    if (w && --w->refs == 0) {
        if (w->p) tv_promise_release(w->p);
        free(w);
    }
}

tv_promise *tv_js_await(JSValueRef v) {
    JSContextRef ctx = tv_js();
    tv_promise *p = tv_promise_new(&tv_type_js);
    static JSStringRef kthen;
    JSValueRef then = JSValueIsObject(ctx, v) ? JSObjectGetProperty(ctx, (JSObjectRef)v, tv_js_key(&kthen, "then"), NULL) : NULL;
    if (!then || !JSValueIsObject(ctx, then) || !JSObjectIsFunction(ctx, (JSObjectRef)then)) {
        tv_promise_resolve(p, &v);
        return p;
    }
    if (!tv_js_fulfill_class) {
        JSClassDefinition d = kJSClassDefinitionEmpty;
        d.callAsFunction = tv_js_fulfill_fn;
        d.finalize = tv_js_waiter_finalize;
        tv_js_fulfill_class = JSClassCreate(&d);
        d.callAsFunction = tv_js_reject_fn;
        tv_js_reject_class = JSClassCreate(&d);
    }
    tv_js_waiter *w = malloc(sizeof *w);
    if (!w) tv_trap("out of memory", "js");
    w->p = p;
    w->refs = 2;
    tv_promise_retain(p);
    JSValueRef fns[2] = {JSObjectMake(ctx, tv_js_fulfill_class, w), JSObjectMake(ctx, tv_js_reject_class, w)};
    JSValueRef exc = NULL;
    JSObjectCallAsFunction(ctx, (JSObjectRef)then, (JSObjectRef)v, 2, fns, &exc);
    if (exc && w->p) {
        /* `then` threw: the await rejects with that */
        tv_js_waiter_settle(ctx, (JSObjectRef)fns[1], 1, &exc, false);
    }
    return p;
}

/* ------------------------------------------------------------------ for generated code */

void tv_js_error_parts(JSValueRef exc, tv_str *name, tv_str *message) {
    JSContextRef ctx = tv_js();
    if (JSValueIsObject(ctx, exc)) {
        static JSStringRef kname, kmessage;
        JSValueRef n = JSObjectGetProperty(ctx, (JSObjectRef)exc, tv_js_key(&kname, "name"), NULL);
        JSValueRef m = JSObjectGetProperty(ctx, (JSObjectRef)exc, tv_js_key(&kmessage, "message"), NULL);
        if (n && JSValueIsString(ctx, n) && m && !JSValueIsUndefined(ctx, m)) {
            *name = tv_js_to_str(n);
            *message = tv_js_to_str(m);
            return;
        }
    }
    *name = tv_str_from("JsError", 7);
    *message = tv_js_to_str(exc);
}

static _Noreturn void tv_js_trap_exc(JSValueRef exc, const char *loc) {
    tv_str text = tv_js_error_text(exc);
    tv_sb sb = {0};
    tv_sb_push_cstr(&sb, "uncaught JavaScript exception: ");
    tv_sb_push_str(&sb, text);
    tv_sb_push_char(&sb, 0);
    tv_trap(sb.data, loc);
}

JSValueRef tv_js_import_as(const char *spec, int kind, const char *loc) {
    JSContextRef ctx = tv_js();
    JSValueRef exc = NULL;
    JSValueRef m = tv_js_import(spec, &exc);
    if (!m) tv_js_trap_exc(exc, loc);
    if (kind == 0) {
        /* as in Node.js: a CommonJS module's namespace has `default` (module.exports) and its keys */
        static JSStringRef kns;
        JSValueRef ns = JSObjectGetProperty(ctx, JSContextGetGlobalObject(ctx), tv_js_key(&kns, "__tov_ns"), NULL);
        JSValueRef r = JSObjectCallAsFunction(ctx, (JSObjectRef)ns, NULL, 1, &m, &exc);
        if (!r) tv_js_trap_exc(exc, loc);
        return r;
    }
    if (kind == 1 && JSValueIsObject(ctx, m)) {
        /* an ES module's default export; a CommonJS module's is module.exports */
        static JSStringRef kesm, kdefault;
        JSValueRef esm = JSObjectGetProperty(ctx, (JSObjectRef)m, tv_js_key(&kesm, "__esModule"), NULL);
        if (esm && JSValueToBoolean(ctx, esm)) {
            JSValueRef r = JSObjectGetProperty(ctx, (JSObjectRef)m, tv_js_key(&kdefault, "default"), &exc);
            if (!r || exc) tv_js_trap_exc(exc, loc);
            return r;
        }
    }
    return m;
}

JSValueRef tv_js_get_or_trap(JSValueRef obj, tv_js_name *key, const char *loc) {
    JSValueRef exc = NULL;
    JSValueRef r = tv_js_name_get(obj, key, &exc);
    if (!r) tv_js_trap_exc(exc, loc);
    return r;
}

JSValueRef tv_js_at_or_trap(JSValueRef obj, double i, const char *loc) {
    JSContextRef ctx = tv_js();
    JSValueRef exc = NULL;
    JSObjectRef o = JSValueToObject(ctx, obj, &exc);
    if (!o) tv_js_trap_exc(exc, loc);
    JSValueRef r;
    if (i >= 0 && i < 4294967295.0 && i == (double)(uint32_t)i) {
        r = JSObjectGetPropertyAtIndex(ctx, o, (unsigned)i, &exc);
    } else {
        r = JSObjectGetPropertyForKey(ctx, o, JSValueMakeNumber(ctx, i), &exc);
    }
    if (exc) tv_js_trap_exc(exc, loc);
    return r;
}

JSValueRef tv_js_key_or_trap(JSValueRef obj, tv_str key, const char *loc) {
    JSContextRef ctx = tv_js();
    JSValueRef exc = NULL;
    JSObjectRef o = JSValueToObject(ctx, obj, &exc);
    if (!o) tv_js_trap_exc(exc, loc);
    JSValueRef r = JSObjectGetPropertyForKey(ctx, o, tv_js_from_str(key), &exc);
    if (exc) tv_js_trap_exc(exc, loc);
    return r;
}

static JSStringRef tv_js_name_str(tv_js_name *k) {
    if (!k->str) k->str = JSStringCreateWithUTF8CString(k->text);
    return k->str;
}

void tv_js_put_or_trap(JSValueRef obj, tv_js_name *key, JSValueRef value, const char *loc) {
    JSValueRef exc = NULL;
    if (!tv_js_set(obj, tv_js_name_str(key), value, &exc)) tv_js_trap_exc(exc, loc);
}

_Noreturn void tv_js_type_trap(JSValueRef v, const char *want, const char *loc) {
    tv_sb sb = {0};
    tv_sb_push_cstr(&sb, "a JavaScript ");
    tv_sb_push_cstr(&sb, JSValueIsNull(tv_js(), v) ? "null" : tv_js_typeof(v));
    tv_sb_push_cstr(&sb, " isn't a `");
    tv_sb_push_cstr(&sb, want);
    tv_sb_push_cstr(&sb, "`");
    tv_sb_push_char(&sb, 0);
    tv_trap(sb.data, loc);
}

bool tv_js_truthy_slow(JSValueRef v) { return JSValueToBoolean(tv_js(), v); }
bool tv_js_is_array(JSValueRef v) { return JSValueIsArray(tv_js(), v); }

uint32_t tv_js_length(JSValueRef v) {
    JSContextRef ctx = tv_js();
    static JSStringRef klength;
    if (!JSValueIsObject(ctx, v)) return 0;
    JSValueRef n = JSObjectGetProperty(ctx, (JSObjectRef)v, tv_js_key(&klength, "length"), NULL);
    double d = n ? JSValueToNumber(ctx, n, NULL) : 0;
    return d >= 0 && d < 4294967296.0 ? (uint32_t)d : 0;
}

JSValueRef tv_js_array(size_t n, const JSValueRef *items) { return JSObjectMakeArray(tv_js(), n, items, NULL); }
JSValueRef tv_js_object(void) { return JSObjectMake(tv_js(), NULL, NULL); }

JSValueRef tv_js_lit(JSValueRef *slot, const char *s, size_t n) {
    if (!*slot) *slot = tv_js_retain(tv_js_str(s, n));
    return *slot;
}

/* Tov closures as JavaScript functions: the closure and its trampoline as private data.
 * Environments of collected functions are released later, outside the collector. */
typedef struct { tv_fn fn; tv_js_tramp tramp; } tv_js_closure;
static JSClassRef tv_js_closure_class;
static tv_env **tv_js_dead_envs;
static size_t tv_js_ndead, tv_js_capdead;

static void tv_js_release_dead(void) {
    while (tv_js_ndead) tv_env_release(tv_js_dead_envs[--tv_js_ndead]);
}

static JSValueRef tv_js_closure_call(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t n, const JSValueRef a[], JSValueRef *exc) {
    (void)ctx; (void)self; (void)exc;
    tv_js_closure *c = JSObjectGetPrivate(f);
    return c->tramp(&c->fn, n, a);
}

static void tv_js_closure_finalize(JSObjectRef f) {
    tv_js_closure *c = JSObjectGetPrivate(f);
    if (!c) return;
    if (c->fn.env) {
        if (tv_js_ndead == tv_js_capdead) {
            tv_js_capdead = tv_js_capdead ? tv_js_capdead * 2 : 16;
            tv_js_dead_envs = realloc(tv_js_dead_envs, tv_js_capdead * sizeof *tv_js_dead_envs);
            if (!tv_js_dead_envs) abort();
        }
        tv_js_dead_envs[tv_js_ndead++] = c->fn.env;
    }
    free(c);
}

JSValueRef tv_js_function(tv_fn fn, tv_js_tramp tramp) {
    JSContextRef ctx = tv_js();
    tv_js_release_dead();
    if (!tv_js_closure_class) {
        JSClassDefinition d = kJSClassDefinitionEmpty;
        d.className = "Function";
        d.callAsFunction = tv_js_closure_call;
        d.finalize = tv_js_closure_finalize;
        tv_js_closure_class = JSClassCreate(&d);
    }
    tv_js_closure *c = malloc(sizeof *c);
    if (!c) abort();
    c->fn = fn;
    c->tramp = tramp;
    tv_env_retain(fn.env);
    return JSObjectMake(ctx, tv_js_closure_class, c);
}

/* ------------------------------------------------------------------ roots
 *
 * Cells Tov holds, with counts: open addressing on the key (the cell pointer). The keys live in
 * the program's main frame (tv_js_roots_init), where the collector's conservative stack scan sees
 * them; the counts on the heap. Past 3/4 full, new cells go to JSValueProtect instead (a release
 * not found here is the protect's). Tombstones are swept out when they pile up. */

static JSValueRef *tv_js_keys;
static uint32_t *tv_js_counts;
static uint32_t tv_js_cap, tv_js_live;

void tv_js_roots_init(JSValueRef *keys, uint32_t cap) {
    memset(keys, 0, (size_t)cap * sizeof *keys);
    tv_js_counts = calloc(cap, sizeof *tv_js_counts);
    if (!tv_js_counts) return;
    tv_js_keys = keys;
    tv_js_cap = cap;
}

static uint32_t tv_js_slot_of(JSValueRef v) {
    uint64_t h = (uint64_t)(uintptr_t)v >> 4;
    h *= 0x9e3779b97f4a7c15ull;
    return (uint32_t)(h >> 32) & (tv_js_cap - 1);
}

/* Open addressing with linear probing; a removal shifts the entries after it back (no
 * tombstones: a value retained and released a million times in a loop left the table full of
 * them, and every insertion walking past them was a sixth of the loop's time). */
void tv_js_root_add(JSValueRef v) {
    if (tv_js_cap) {
        uint32_t i = tv_js_slot_of(v);
        for (JSValueRef k; (k = tv_js_keys[i]); i = (i + 1) & (tv_js_cap - 1)) {
            if (k == v) {
                tv_js_counts[i]++;
                return;
            }
        }
        if (tv_js_live + 1 <= tv_js_cap / 4 * 3) {
            tv_js_counts[i] = 1;
            tv_js_keys[i] = v;
            tv_js_live++;
            return;
        }
    }
    JSValueProtect(tv_js(), v);
}

void tv_js_root_remove(JSValueRef v) {
    if (tv_js_cap) {
        uint32_t mask = tv_js_cap - 1, i = tv_js_slot_of(v);
        for (JSValueRef k; (k = tv_js_keys[i]); i = (i + 1) & mask) {
            if (k != v) continue;
            if (--tv_js_counts[i]) return;
            /* the gap at i: an entry after it whose home is at or before the gap moves into it
             * (its copy is made before it's cleared: a scan of the table never misses it) */
            uint32_t j = i;
            for (;;) {
                j = (j + 1) & mask;
                JSValueRef kj = tv_js_keys[j];
                if (!kj) break;
                uint32_t home = tv_js_slot_of(kj);
                /* (does home lie cyclically in (i, j]? then it stays) */
                bool stays = i <= j ? (i < home && home <= j) : (i < home || home <= j);
                if (stays) continue;
                tv_js_keys[i] = kj;
                tv_js_counts[i] = tv_js_counts[j];
                i = j;
            }
            tv_js_keys[i] = NULL;
            tv_js_live--;
            return;
        }
    }
    if (tv_js_ctx) JSValueUnprotect(tv_js_ctx, v);
}

/* ------------------------------------------------------------------ names and shapes */

static bool tv_js_is_ident(const char *s) {
    if (!*s || (*s >= '0' && *s <= '9')) return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$')) return false;
    }
    return true;
}

/* `.name` or `["name"]` (JSON-quoted) */
static void tv_js_access(tv_sb *sb, const char *name) {
    if (tv_js_is_ident(name)) {
        tv_sb_push_char(sb, '.');
        tv_sb_push_cstr(sb, name);
        return;
    }
    tv_sb_push_char(sb, '[');
    tv_json_quote(sb, tv_str_from(name, strlen(name)));
    tv_sb_push_char(sb, ']');
}

/* A function compiled from `body` with parameters o, a0, a1, ... (n of the a's). */
static JSObjectRef tv_js_make_fn(const char *params_prefix, uint32_t n, tv_sb *body) {
    JSContextRef ctx = tv_js();
    JSStringRef *names = tv_alloc((size_t)(n + 1) * sizeof *names);
    uint32_t np = 0;
    if (params_prefix) names[np++] = JSStringCreateWithUTF8CString(params_prefix);
    for (uint32_t i = 0; i < n; i++) {
        char b[8];
        snprintf(b, sizeof b, "a%u", i);
        names[np++] = JSStringCreateWithUTF8CString(b);
    }
    tv_sb_push_char(body, 0);
    JSStringRef src = JSStringCreateWithUTF8CString(body->data);
    JSObjectRef f = JSObjectMakeFunction(ctx, NULL, np, names, src, NULL, 1, NULL);
    JSStringRelease(src);
    for (uint32_t i = 0; i < np; i++) JSStringRelease(names[i]);
    tv_free(names);
    tv_sb_free(body);
    if (f) JSValueProtect(ctx, f);
    return f;
}

JSValueRef tv_js_name_get(JSValueRef obj, tv_js_name *k, JSValueRef *exc) {
    JSContextRef ctx = tv_js();
#ifdef TV_JSC_OWN
    /* read where it is, without a call into the engine (null and undefined throw as `o.name`
     * words it, by way of the compiled reader below) */
    if (!JSValueIsUndefined(ctx, obj) && !JSValueIsNull(ctx, obj)) {
        if (!k->id) k->id = TVPropertyName(ctx, k->text);
        return TVGetProperty(ctx, obj, k->id, exc);
    }
#endif
    if (!k->get) {
        tv_sb b = {0};
        tv_sb_push_cstr(&b, "return o");
        tv_js_access(&b, k->text);
        k->get = tv_js_make_fn("o", 0, &b);
        if (!k->get) return tv_js_get(obj, tv_js_name_str(k), exc);
    }
    JSValueRef r = TV_JS_CALL(ctx, k->get, NULL, 1, &obj, exc);
    return *exc ? NULL : r;
}

JSValueRef tv_js_name_call(JSValueRef obj, tv_js_name *k, size_t n, const JSValueRef *args, JSValueRef *exc) {
    if (n >= 5) return tv_js_invoke(obj, tv_js_name_str(k), n, args, exc);
    JSContextRef ctx = tv_js();
    if (!k->call[n]) {
        tv_sb b = {0};
        tv_sb_push_cstr(&b, "return o");
        tv_js_access(&b, k->text);
        tv_sb_push_char(&b, '(');
        for (size_t i = 0; i < n; i++) {
            char a[16];
            snprintf(a, sizeof a, "%sa%zu", i ? ", " : "", i);
            tv_sb_push_cstr(&b, a);
        }
        tv_sb_push_char(&b, ')');
        k->call[n] = tv_js_make_fn("o", (uint32_t)n, &b);
        if (!k->call[n]) return tv_js_invoke(obj, tv_js_name_str(k), n, args, exc);
    }
    JSValueRef all[5];
    all[0] = obj;
    for (size_t i = 0; i < n; i++) all[i + 1] = args[i];
    JSValueRef r = TV_JS_CALL(ctx, k->call[n], NULL, n + 1, all, exc);
    return *exc ? NULL : r;
}

JSValueRef tv_js_shape_make(tv_js_shape *s, const JSValueRef *values) {
    JSContextRef ctx = tv_js();
    if (!s->make) {
        tv_sb b = {0};
        tv_sb_push_cstr(&b, "var o = {");
        bool first = true;
        for (uint32_t i = 0; i < s->n; i++) {
            if (s->optional[i]) continue;
            char a[16];
            snprintf(a, sizeof a, "a%u", i);
            if (!first) tv_sb_push_cstr(&b, ", ");
            first = false;
            tv_json_quote(&b, tv_str_from(s->keys[i], strlen(s->keys[i])));
            tv_sb_push_cstr(&b, ": ");
            tv_sb_push_cstr(&b, a);
        }
        tv_sb_push_cstr(&b, "};");
        for (uint32_t i = 0; i < s->n; i++) {
            if (!s->optional[i]) continue;
            char a[64];
            snprintf(a, sizeof a, " if (a%u !== undefined) o", i);
            tv_sb_push_cstr(&b, a);
            tv_js_access(&b, s->keys[i]);
            snprintf(a, sizeof a, " = a%u;", i);
            tv_sb_push_cstr(&b, a);
        }
        tv_sb_push_cstr(&b, " return o;");
        s->make = tv_js_make_fn(NULL, s->n, &b);
    }
    return TV_JS_CALL(ctx, s->make, NULL, s->n, values, NULL);
}

JSValueRef tv_js_thunk(JSObjectRef *slot, const char *body, size_t n, const JSValueRef *args, JSValueRef *exc) {
    JSContextRef ctx = tv_js();
    if (!*slot) {
        tv_sb b = {0};
        tv_sb_push_cstr(&b, body);
        *slot = tv_js_make_fn(NULL, (uint32_t)n, &b);
        if (!*slot) tv_trap("internal error: a fused JavaScript expression didn't compile", body);
    }
    JSValueRef r = TV_JS_CALL(ctx, *slot, NULL, n, args, exc);
    return *exc ? NULL : r;
}

_Noreturn void tv_js_throw_trap(JSValueRef exc, const char *loc) { tv_js_trap_exc(exc, loc); }
