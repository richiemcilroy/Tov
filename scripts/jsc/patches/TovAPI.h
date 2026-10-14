/* TovAPI.h: what Tov's runtime (runtime/js.c) needs from JavaScriptCore beyond its C API, in
 * Tov's own build of it (scripts/jsc). */

#ifndef TovAPI_h
#define TovAPI_h

#include <JavaScriptCore/JSBase.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Evaluates `len` bytes of UTF-8 at `source` as a script named `url` (NUL-terminated UTF-8).
 * ASCII text isn't copied: the caller keeps it alive and unchanged for as long as the engine
 * may read it (the program's lifetime, for text in its binary). With `cache_fd` >= 0, the
 * bytecode TVWriteBytecode wrote to that file for the same text is used if it's valid for this
 * text and this build of the engine (*used_cache says whether it was); the engine maps the
 * file, so the caller may close the descriptor once this returns. */
JS_EXPORT JSValueRef TVEvaluateScript(JSContextRef ctx, const char *source, size_t len, const char *url, int cache_fd, bool *used_cache, JSValueRef *exception);

/* Writes the bytecode of a script as TVEvaluateScript takes it, its functions' included, to
 * `fd` (from its start); false if it can't (the script doesn't parse, or the write failed). */
JS_EXPORT bool TVWriteBytecode(JSContextGroupRef group, const char *source, size_t len, const char *url, int fd);

/* Starts the engine on the calling thread, which is then the one its contexts are used on, with
 * the options in the environment (JSC_name=value): its run loop (below) is the engine's main one,
 * where it schedules what's meant for the main thread. Before the first context is made. */
JS_EXPORT void TVInitialize(void);

/* The calling thread's run loop, where the engine runs its timers (collections, finalizers) and
 * what its threads hand back (WebAssembly compiled off the thread), for an event loop of the
 * embedder's own to run: seconds until it has work (0: it has now; < 0: none scheduled); run
 * what's due; and a function called (from any thread, with a lock held: it should only wake
 * the event loop) when work is scheduled. */
JS_EXPORT double TVRunLoopSecondsUntilWork(void);
JS_EXPORT void TVRunLoopCycle(void);
JS_EXPORT void TVRunLoopSetWakeUp(void (*wake)(void));

/* Whether the engine's threads have work underway they'll hand back soon (WebAssembly being
 * compiled off the thread for a promise): the program isn't done while they do. */
JS_EXPORT bool TVHasImminentWork(JSContextRef ctx);

/* A new Uint8Array of `len` bytes, uninitialized, its bytes at *bytes for the caller to fill
 * before anything else runs; NULL if out of memory. Unlike JSObjectGetTypedArrayBytesPtr's, its
 * buffer isn't pinned (a stream can transfer it), and a small one has none until asked for. */
JS_EXPORT JSObjectRef TVMakeUint8Array(JSContextRef ctx, size_t len, void **bytes);

/* Sets the engine's options ("name=value name=value ...", as JSC_name=value in the
 * environment), before the first context is made; false if one isn't an option. */
JS_EXPORT bool TVSetOptions(const char *options);

/* The host's time zone changed (process.env.TZ was set, say): the date and Intl caches of ctx's
 * engine are cleared now (JavaScript running reads the new zone at once, as Node.js's does), and
 * those of any other engine as it's next entered. (macOS's own notification says so for the
 * system's time zone.) */
JS_EXPORT void TVTimeZoneDidChange(JSContextRef ctx);

/* Gives memory back now (the program is idle): optionally drops compiled code, collects in
 * full, frees the blocks that emptied (rather than as the incremental sweeper reaches them), and
 * returns the allocator's free pages to the system. */
JS_EXPORT void TVReleaseMemory(JSContextRef ctx, bool dropCode);

/* The engine's lock stays this thread's between calls into it (a "sticky" lock): each call no
 * longer takes it and gives it back (setting the stack's limits, the heap's access, and back),
 * while what giving it back does that a program sees happens as before (microtasks run when the
 * outermost call returns). TVReleaseStickyLock gives it back for real: before the thread blocks,
 * so a collection on another thread needn't wait for it; the next call takes it again. */
JS_EXPORT void TVSetStickyLock(JSContextRef ctx);

/* JSObjectCallAsFunction, for the calls Tov makes most (a fused expression, a method): `function`
 * must be callable; `thisObject` NULL is the global object; NULL with *exception set if it
 * throws. (Without the C API's generality: no profiling hook, its arguments' buffer kept inline.) */
JS_EXPORT JSValueRef TVCall(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount, const JSValueRef arguments[], JSValueRef *exception);

/* A property name, made once and kept (for TVGetProperty). */
JS_EXPORT void *TVPropertyName(JSContextRef ctx, const char *utf8);
/* `value.name`, as JavaScript reads it (from a primitive's prototype, through getters and
 * proxies), without entering the engine when it's a plain property; NULL with *exception set
 * if it throws. */
JS_EXPORT JSValueRef TVGetProperty(JSContextRef ctx, JSValueRef value, void *name, JSValueRef *exception);
JS_EXPORT void TVReleaseStickyLock(JSContextRef ctx);

#ifdef __cplusplus
}
#endif

#endif /* TovAPI_h */
