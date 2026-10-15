/* node.c — the natives Node.js's built-ins run on (runtime/node), as functions on
 * globalThis.__tov_native: process, timers (on Tov's event loop), os, tty and fs. Linked with
 * js.c by programs that import npm packages. The JS side of each is documented where it's used
 * (runtime/node/internal/bootstrap/process.js, internal/bindings/timers.js, ...os.js, ...fs.js).
 *
 * Errors: a failing call throws what Node.js throws, an Error with errno (negative), code,
 * syscall and path, and the message libuv gives ("ENOENT: no such file or directory, open 'x'"). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   /* (glibc's POSIX_SPAWN_SETSID, for spawn's detached: without it a detached child stays in our process group) */
#endif

#include "js.h"

#if defined(__APPLE__) && !defined(TV_JSC_OWN)
#include <JavaScriptCore/JavaScriptCore.h>
#else
#include <JavaScriptCore/JavaScript.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <net/if_dl.h>
#include <sys/sysctl.h>
#else
#include <linux/if_packet.h>
#include <sys/sysinfo.h>
#endif

extern char **environ;

#define NATIVE(name) static JSValueRef name(JSContextRef ctx, JSObjectRef fn_, JSObjectRef self_, size_t n, const JSValueRef a[], JSValueRef *exc)
#define UNUSED (void)fn_; (void)self_; (void)n; (void)a; (void)exc

/* ------------------------------------------------------------------ values */

static JSValueRef undef(JSContextRef ctx) { return JSValueMakeUndefined(ctx); }
static JSValueRef num(JSContextRef ctx, double d) { return JSValueMakeNumber(ctx, d); }

static JSValueRef str(JSContextRef ctx, const char *s) {
    (void)ctx;
    return tv_js_str(s, strlen(s));
}

static double arg_num(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i, double dflt) {
    if (i >= n || JSValueIsUndefined(ctx, a[i])) return dflt;
    double d = JSValueToNumber(ctx, a[i], NULL);
    return d != d ? dflt : d;
}

/* argument i as a NUL-terminated UTF-8 string (malloc'd; NULL if it's not a string) */
static char *arg_cstr(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i) {
    if (i >= n || !JSValueIsString(ctx, a[i])) return NULL;
    JSStringRef s = JSValueToStringCopy(ctx, a[i], NULL);
    size_t cap = JSStringGetMaximumUTF8CStringSize(s);
    char *out = malloc(cap);
    JSStringGetUTF8CString(s, out, cap);
    JSStringRelease(s);
    return out;
}

static void set(JSContextRef ctx, JSObjectRef obj, const char *key, JSValueRef v) {
    JSStringRef k = JSStringCreateWithUTF8CString(key);
    JSObjectSetProperty(ctx, obj, k, v, kJSPropertyAttributeNone, NULL);
    JSStringRelease(k);
}

static JSObjectRef array(JSContextRef ctx, size_t count, const JSValueRef *items) {
    return JSObjectMakeArray(ctx, count, items, NULL);
}

/* ------------------------------------------------------------------ errors, as libuv names them */

static const struct { int err; const char *code, *message; } tv_uv_errors[] = {
    {E2BIG, "E2BIG", "argument list too long"},
    {EACCES, "EACCES", "permission denied"},
    {EADDRINUSE, "EADDRINUSE", "address already in use"},
    {EADDRNOTAVAIL, "EADDRNOTAVAIL", "address not available"},
    {EAGAIN, "EAGAIN", "resource temporarily unavailable"},
    {EBADF, "EBADF", "bad file descriptor"},
    {EBUSY, "EBUSY", "resource busy or locked"},
    {ECANCELED, "ECANCELED", "operation canceled"},
    {ECONNABORTED, "ECONNABORTED", "software caused connection abort"},
    {ECONNREFUSED, "ECONNREFUSED", "connection refused"},
    {ECONNRESET, "ECONNRESET", "connection reset by peer"},
    {EEXIST, "EEXIST", "file already exists"},
    {EFAULT, "EFAULT", "bad address in system call argument"},
    {EFBIG, "EFBIG", "file too large"},
    {EHOSTUNREACH, "EHOSTUNREACH", "host is unreachable"},
    {EINTR, "EINTR", "interrupted system call"},
    {EINVAL, "EINVAL", "invalid argument"},
    {EIO, "EIO", "i/o error"},
    {EISDIR, "EISDIR", "illegal operation on a directory"},
    {ELOOP, "ELOOP", "too many symbolic links encountered"},
    {EMFILE, "EMFILE", "too many open files"},
    {EMLINK, "EMLINK", "too many links"},
    {ENAMETOOLONG, "ENAMETOOLONG", "name too long"},
    {ENFILE, "ENFILE", "file table overflow"},
    {ENODEV, "ENODEV", "no such device"},
    {ENOENT, "ENOENT", "no such file or directory"},
    {ENOMEM, "ENOMEM", "not enough memory"},
    {ENOSPC, "ENOSPC", "no space left on device"},
    {ENOSYS, "ENOSYS", "function not implemented"},
    {ENOTCONN, "ENOTCONN", "socket is not connected"},
    {ENOTDIR, "ENOTDIR", "not a directory"},
    {ENOTEMPTY, "ENOTEMPTY", "directory not empty"},
    {ENOTSUP, "ENOTSUP", "operation not supported on socket"},
    {ENOTTY, "ENOTTY", "inappropriate ioctl for device"},
    {ENXIO, "ENXIO", "no such device or address"},
    {EPERM, "EPERM", "operation not permitted"},
    {EPIPE, "EPIPE", "broken pipe"},
    {ERANGE, "ERANGE", "result too large"},
    {EROFS, "EROFS", "read-only file system"},
    {ESPIPE, "ESPIPE", "invalid seek"},
    {ESRCH, "ESRCH", "no such process"},
    {ETIMEDOUT, "ETIMEDOUT", "connection timed out"},
    {ETXTBSY, "ETXTBSY", "text file is busy"},
    {EXDEV, "EXDEV", "cross-device link not permitted"},
};

static void tv_uv_name(int err, const char **code, const char **message) {
    for (size_t i = 0; i < sizeof tv_uv_errors / sizeof *tv_uv_errors; i++) {
        if (tv_uv_errors[i].err == err) {
            *code = tv_uv_errors[i].code;
            *message = tv_uv_errors[i].message;
            return;
        }
    }
    *code = "UNKNOWN";
    *message = "unknown error";
}

/* The Error Node.js throws for a failed system call (uvException): "CODE: message, syscall
 * 'path' -> 'dest'", with errno (negative), code, syscall, path and dest. */
static JSValueRef tv_node_errno(JSContextRef ctx, int err, const char *syscall, const char *path, const char *dest) {
    const char *code, *message;
    tv_uv_name(err, &code, &message);
    char text[4096];
    int len = snprintf(text, sizeof text, "%s: %s, %s", code, message, syscall);
    if (path && len < (int)sizeof text) len += snprintf(text + len, sizeof text - (size_t)len, " '%s'", path);
    if (dest && len < (int)sizeof text) snprintf(text + len, sizeof text - (size_t)len, " -> '%s'", dest);
    JSValueRef msg = str(ctx, text);
    JSObjectRef e = JSObjectMakeError(ctx, 1, &msg, NULL);
    set(ctx, e, "errno", num(ctx, -err));
    set(ctx, e, "code", str(ctx, code));
    set(ctx, e, "syscall", str(ctx, syscall));
    if (path) set(ctx, e, "path", str(ctx, path));
    if (dest) set(ctx, e, "dest", str(ctx, dest));
    return e;
}

/* throws the errno error; returns undefined for the native to return */
static JSValueRef tv_node_throw(JSContextRef ctx, JSValueRef *exc, int err, const char *syscall, const char *path, const char *dest) {
    *exc = tv_node_errno(ctx, err, syscall, path, dest);
    return undef(ctx);
}

/* For os.js: fills ctx (a JS object) with { errno, code, message, syscall } and returns undefined. */
static JSValueRef tv_node_ctx_error(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i, int err, const char *syscall) {
    if (i < n && JSValueIsObject(ctx, a[i])) {
        JSObjectRef c = (JSObjectRef)a[i];
        const char *code, *message;
        tv_uv_name(err, &code, &message);
        set(ctx, c, "errno", num(ctx, -err));
        set(ctx, c, "code", str(ctx, code));
        set(ctx, c, "message", str(ctx, message));
        set(ctx, c, "syscall", str(ctx, syscall));
    }
    return undef(ctx);
}

/* ------------------------------------------------------------------ process */

static const char *tv_node_platform(void) {
#if defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

static const char *tv_node_arch(void) {
#if defined(__aarch64__)
    return "arm64";
#elif defined(__x86_64__)
    return "x64";
#else
    return "unknown";
#endif
}

static void tv_node_exec_path(char *out, size_t cap) {
#if defined(__APPLE__)
    uint32_t size = (uint32_t)cap;
    char raw[4096];
    uint32_t rs = sizeof raw;
    if (_NSGetExecutablePath(raw, &rs) == 0 && realpath(raw, out)) return;
    (void)size;
#else
    ssize_t k = readlink("/proc/self/exe", out, cap - 1);
    if (k > 0) {
        out[k] = 0;
        return;
    }
#endif
    snprintf(out, cap, "%s", tv_argc > 0 ? tv_argv[0] : "tov");
}

NATIVE(n_info) {
    UNUSED;
    JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
    /* Node.js: [node, script, args...]; a Tov program is both */
    size_t count = (size_t)tv_argc + 1;
    JSValueRef *items = malloc(sizeof(JSValueRef) * count);
    char exe[4096];
    tv_node_exec_path(exe, sizeof exe);
    items[0] = str(ctx, exe);
    for (int i = 0; i < tv_argc; i++) items[i + 1] = str(ctx, i == 0 ? exe : tv_argv[i]);
    set(ctx, o, "argv", array(ctx, count, items));
    free(items);
    set(ctx, o, "execArgv", array(ctx, 0, NULL));
    set(ctx, o, "execPath", str(ctx, exe));
    set(ctx, o, "pid", num(ctx, getpid()));
    set(ctx, o, "ppid", num(ctx, getppid()));
    set(ctx, o, "platform", str(ctx, tv_node_platform()));
    set(ctx, o, "arch", str(ctx, tv_node_arch()));
    const char *base = strrchr(exe, '/');
    set(ctx, o, "title", str(ctx, base ? base + 1 : exe));
    return o;
}

/* env() -> the environment as an object (process.env, made on first use) */
NATIVE(n_env) {
    UNUSED;
    JSObjectRef env = JSObjectMake(ctx, NULL, NULL);
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        char key[1024];
        size_t kl = (size_t)(eq - *e) < sizeof key - 1 ? (size_t)(eq - *e) : sizeof key - 1;
        memcpy(key, *e, kl);
        key[kl] = 0;
        set(ctx, env, key, str(ctx, eq + 1));
    }
    return env;
}

/* setTZ(value | undefined): process.env.TZ assigned. The C library and the engine see the new
 * time zone at once, as under Node.js (Date's local time, Intl). */
NATIVE(n_set_tz) {
    UNUSED;
    char *tz = n > 0 && JSValueIsString(ctx, a[0]) ? arg_cstr(ctx, n, a, 0) : NULL;
    if (tz) setenv("TZ", tz, 1);
    else unsetenv("TZ");
    free(tz);
    tzset();
#ifdef TV_JSC_OWN
    extern void TVTimeZoneDidChange(JSContextRef);
    TVTimeZoneDidChange(ctx);
#endif
    return undef(ctx);
}

NATIVE(n_cwd) {
    UNUSED;
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) return tv_node_throw(ctx, exc, errno, "uv_cwd", NULL, NULL);
    return str(ctx, buf);
}

NATIVE(n_chdir) {
    UNUSED;
    char *path = arg_cstr(ctx, n, a, 0);
    if (!path) return undef(ctx);
    if (chdir(path) != 0) {
        char cwd[4096];
        JSValueRef r = tv_node_throw(ctx, exc, errno, "chdir", getcwd(cwd, sizeof cwd) ? cwd : "", path);
        free(path);
        return r;
    }
    free(path);
    return undef(ctx);
}

NATIVE(n_exit) {
    UNUSED;
    tv_out_flush();
    exit((int)arg_num(ctx, n, a, 0, 0));
}

NATIVE(n_umask) {
    UNUSED;
    double m = arg_num(ctx, n, a, 0, -1);
    mode_t old;
    if (m < 0) {
        old = umask(0);
        umask(old);
    } else {
        old = umask((mode_t)m);
    }
    return num(ctx, old);
}

/* nanoseconds since the program started */
NATIVE(n_hrtime) {
    UNUSED;
    return num(ctx, tv_performance_now() * 1e6);
}

/* nowMs() -> milliseconds since the program started (performance.now's clock) */
NATIVE(n_now_ms) {
    UNUSED;
    return num(ctx, tv_performance_now());
}

/* write(fd, string | bytes): stdout through Tov's buffer (in order with the program's own
 * output), other fds directly */
NATIVE(n_write) {
    UNUSED;
    int fd = (int)arg_num(ctx, n, a, 0, 1);
    if (n < 2) return num(ctx, 0);
    uint8_t *p = NULL;
    size_t len = 0;
    char *text = NULL;
    if (JSValueIsString(ctx, a[1])) {
        JSStringRef s = JSValueToStringCopy(ctx, a[1], NULL);
        size_t cap = JSStringGetMaximumUTF8CStringSize(s);
        text = malloc(cap);
        len = JSStringGetUTF8CString(s, text, cap) - 1;
        JSStringRelease(s);
        p = (uint8_t *)text;
    } else if (!tv_js_bytes_view(a[1], &p, &len)) {
        return num(ctx, 0);
    }
    if (fd == 1) {
        tv_out_write((const char *)p, len);
    } else if (fd == 2) {
        tv_out_flush();
        tv_write_fd(2, (const char *)p, len);
    } else {
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(fd, p + off, len - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                free(text);
                return tv_node_throw(ctx, exc, errno, "write", NULL, NULL);
            }
            off += (size_t)w;
        }
    }
    free(text);
    return num(ctx, (double)len);
}

NATIVE(n_isatty) {
    UNUSED;
    return JSValueMakeBoolean(ctx, isatty((int)arg_num(ctx, n, a, 0, -1)) == 1);
}

NATIVE(n_guess_handle_type) {
    UNUSED;
    int fd = (int)arg_num(ctx, n, a, 0, -1);
    const char *type = "UNKNOWN";
    struct stat st;
    if (isatty(fd)) type = "TTY";
    else if (fstat(fd, &st) == 0) {
        if (S_ISREG(st.st_mode) || S_ISCHR(st.st_mode)) type = "FILE";
        else if (S_ISFIFO(st.st_mode)) type = "PIPE";
        else if (S_ISSOCK(st.st_mode)) {
            struct sockaddr_storage addr;
            socklen_t len = sizeof addr;
            if (getsockname(fd, (struct sockaddr *)&addr, &len) == 0) {
                if (addr.ss_family == AF_UNIX) type = "PIPE";
                else if (addr.ss_family == AF_INET || addr.ss_family == AF_INET6) {
                    int kind;
                    len = sizeof kind;
                    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &kind, &len) == 0) {
                        if (kind == SOCK_STREAM) type = "TCP";
                        else if (kind == SOCK_DGRAM) type = "UDP";
                    }
                }
            }
        }
    }
    return str(ctx, type);
}

NATIVE(n_window_size) {
    UNUSED;
    struct winsize ws;
    if (ioctl((int)arg_num(ctx, n, a, 0, 1), TIOCGWINSZ, &ws) != 0) return undef(ctx);
    JSValueRef items[2] = {num(ctx, ws.ws_col), num(ctx, ws.ws_row)};
    return array(ctx, 2, items);
}

static double tv_node_rss(void) {
#if defined(__APPLE__)
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) return (double)info.resident_size;
    return 0;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, resident = 0;
    if (f) {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        fclose(f);
    }
    return (double)resident * (double)sysconf(_SC_PAGESIZE);
#endif
}

static double tv_node_free_mem(void);

NATIVE(n_memory_usage) {
    UNUSED;
    JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
    double rss = tv_node_rss();
    set(ctx, o, "rss", num(ctx, rss));
    /* JavaScriptCore doesn't report its heap through the public API */
    set(ctx, o, "heapTotal", num(ctx, 0));
    set(ctx, o, "heapUsed", num(ctx, 0));
    set(ctx, o, "external", num(ctx, 0));
    set(ctx, o, "arrayBuffers", num(ctx, 0));
    set(ctx, o, "available", num(ctx, tv_node_free_mem()));
    return o;
}

NATIVE(n_cpu_usage) {
    UNUSED;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    JSValueRef items[2] = {
        num(ctx, (double)ru.ru_utime.tv_sec * 1e6 + (double)ru.ru_utime.tv_usec),
        num(ctx, (double)ru.ru_stime.tv_sec * 1e6 + (double)ru.ru_stime.tv_usec),
    };
    return array(ctx, 2, items);
}

NATIVE(n_ids) {
    UNUSED;
    JSValueRef items[4] = {num(ctx, getuid()), num(ctx, getgid()), num(ctx, geteuid()), num(ctx, getegid())};
    return array(ctx, 4, items);
}

static const struct { const char *name; int sig; } tv_signals[] = {
    {"SIGHUP", SIGHUP}, {"SIGINT", SIGINT}, {"SIGQUIT", SIGQUIT}, {"SIGILL", SIGILL}, {"SIGTRAP", SIGTRAP},
    {"SIGABRT", SIGABRT}, {"SIGBUS", SIGBUS}, {"SIGFPE", SIGFPE}, {"SIGKILL", SIGKILL}, {"SIGUSR1", SIGUSR1},
    {"SIGSEGV", SIGSEGV}, {"SIGUSR2", SIGUSR2}, {"SIGPIPE", SIGPIPE}, {"SIGALRM", SIGALRM}, {"SIGTERM", SIGTERM},
    {"SIGCHLD", SIGCHLD}, {"SIGCONT", SIGCONT}, {"SIGSTOP", SIGSTOP}, {"SIGTSTP", SIGTSTP}, {"SIGTTIN", SIGTTIN},
    {"SIGTTOU", SIGTTOU}, {"SIGURG", SIGURG}, {"SIGXCPU", SIGXCPU}, {"SIGXFSZ", SIGXFSZ}, {"SIGVTALRM", SIGVTALRM},
    {"SIGPROF", SIGPROF}, {"SIGWINCH", SIGWINCH}, {"SIGIO", SIGIO}, {"SIGSYS", SIGSYS},
};

NATIVE(n_kill) {
    UNUSED;
    int pid = (int)arg_num(ctx, n, a, 0, 0);
    int sig = SIGTERM;
    if (n > 1 && JSValueIsNumber(ctx, a[1])) {
        sig = (int)JSValueToNumber(ctx, a[1], NULL);
    } else if (n > 1 && JSValueIsString(ctx, a[1])) {
        char *name = arg_cstr(ctx, n, a, 1);
        sig = -1;
        for (size_t i = 0; i < sizeof tv_signals / sizeof *tv_signals; i++)
            if (strcmp(tv_signals[i].name, name) == 0) sig = tv_signals[i].sig;
        free(name);
        if (sig < 0) return tv_node_throw(ctx, exc, EINVAL, "kill", NULL, NULL);
    }
    if (kill(pid, sig) != 0) return tv_node_throw(ctx, exc, errno, "kill", NULL, NULL);
    return JSValueMakeBoolean(ctx, true);
}

/* process's 'uncaughtException' (set by internal/bootstrap/process): handler(err) -> whether a
 * listener took it */
static JSObjectRef tv_node_fatal_handler;

/* typedArrayType(value) -> JavaScriptCore's JSTypedArrayType for it (0..11: Int8Array ..
 * Float64Array, then ArrayBuffer; 12 none; later kinds after), without throwing */
NATIVE(n_typed_array_type) {
    UNUSED;
    if (n < 1) return num(ctx, kJSTypedArrayTypeNone);
    return num(ctx, JSValueGetTypedArrayType(ctx, a[0], NULL));
}

/* setFatalHandler(fn); fatal(err): report an uncaught exception from JavaScript */
NATIVE(n_set_fatal_handler) {
    UNUSED;
    if (tv_node_fatal_handler) JSValueUnprotect(ctx, tv_node_fatal_handler);
    tv_node_fatal_handler = n > 0 && JSValueIsObject(ctx, a[0]) ? (JSObjectRef)a[0] : NULL;
    if (tv_node_fatal_handler) JSValueProtect(ctx, tv_node_fatal_handler);
    return undef(ctx);
}

/* setExitHandler(fn): fn(code) -> the status, at the program's natural end */
static JSObjectRef tv_node_exit_handler;

NATIVE(n_set_exit_handler) {
    UNUSED;
    if (tv_node_exit_handler) JSValueUnprotect(ctx, tv_node_exit_handler);
    tv_node_exit_handler = n > 0 && JSValueIsObject(ctx, a[0]) ? (JSObjectRef)a[0] : NULL;
    if (tv_node_exit_handler) JSValueProtect(ctx, tv_node_exit_handler);
    return undef(ctx);
}

static void tv_node_report(JSValueRef exc);

int tv_node_exit_code(int code) {
    if (!tv_js_ctx || !tv_node_exit_handler) return code;
    JSValueRef arg = JSValueMakeNumber(tv_js_ctx, code), exc = NULL;
    JSValueRef r = JSObjectCallAsFunction(tv_js_ctx, tv_node_exit_handler, NULL, 1, &arg, &exc);
    if (exc) tv_node_report(exc); /* (a throwing 'exit' listener: printed, status 1, as in Node.js) */
    tv_out_flush();
    double d = r ? JSValueToNumber(tv_js_ctx, r, NULL) : code;
    return d == d ? (int)d : code;
}

NATIVE(n_fatal) {
    UNUSED;
    tv_node_report(n > 0 ? a[0] : undef(ctx));
    return undef(ctx);
}

/* ------------------------------------------------------------------ timers, on Tov's loop
 *
 * One Tov timer stands for all of Node.js's (internal/timers keeps the lists): when it fires,
 * onTimer runs what's due and re-arms it. Immediates run in the loop's check phase (onCheck). */

static JSObjectRef tv_node_on_timer, tv_node_on_check;
static tv_int tv_node_timer_id;     /* the pending Tov timer (0: none) */
static double tv_node_timer_due;    /* when it fires (ms, tv_performance_now's clock) */
static bool tv_node_timer_refed = true;

/* an exception nothing caught: to 'uncaughtException' listeners if there are any, else printed,
 * and the program ends (exit code 1, as Node.js's) */
static void tv_node_report(JSValueRef exc) {
    static bool reporting;
    if (tv_node_fatal_handler && !reporting) {
        reporting = true;
        JSValueRef inner = NULL;
        JSValueRef handled = JSObjectCallAsFunction(tv_js_ctx, tv_node_fatal_handler, NULL, 1, &exc, &inner);
        reporting = false;
        if (inner) exc = inner;
        else if (handled && JSValueToBoolean(tv_js_ctx, handled)) return;
    }
    tv_str text = tv_js_error_text(exc);
    tv_out_flush();
    tv_err_cstr(text.p->data);
    tv_err_cstr("\n");
    exit(1);
}

static void tv_node_call(JSObjectRef fn) {
    JSValueRef exc = NULL;
    JSObjectCallAsFunction(tv_js_ctx, fn, NULL, 0, NULL, &exc);
    if (exc) tv_node_report(exc);
}

static void tv_node_timer_fire(tv_env *env) {
    (void)env;
    tv_node_timer_id = 0;
    if (tv_node_on_timer) tv_node_call(tv_node_on_timer);
}

static void tv_node_timer_arm(double ms) {
    if (tv_node_timer_id) tv_clear_timer(tv_node_timer_id);
    tv_fn cb = {(void *)tv_node_timer_fire, NULL};
    tv_node_timer_id = tv_set_timer(cb, ms, false);
    tv_node_timer_due = tv_performance_now() + ms;
    if (!tv_node_timer_refed) tv_native_timerUnref(tv_node_timer_id);
}

static void tv_node_check(void) {
    if (tv_node_on_check) tv_node_call(tv_node_on_check);
}

NATIVE(n_now) {
    UNUSED;
    return num(ctx, (double)(int64_t)tv_performance_now());
}

NATIVE(n_timer_setup) {
    UNUSED;
    if (n < 2) return undef(ctx);
    tv_node_on_timer = (JSObjectRef)a[0];
    tv_node_on_check = (JSObjectRef)a[1];
    JSValueProtect(ctx, a[0]);
    JSValueProtect(ctx, a[1]);
    tv_loop_check = tv_node_check;
    return undef(ctx);
}

NATIVE(n_timer_schedule) {
    UNUSED;
    tv_node_timer_arm(arg_num(ctx, n, a, 0, 1));
    return undef(ctx);
}

NATIVE(n_timer_ref) {
    UNUSED;
    bool ref = n > 0 && JSValueToBoolean(ctx, a[0]);
    if (ref == tv_node_timer_refed) return undef(ctx);
    tv_node_timer_refed = ref;
    if (!tv_node_timer_id) return undef(ctx);
    if (!ref) {
        tv_native_timerUnref(tv_node_timer_id);
    } else {
        /* (Tov's timers can't be ref'd again: arm a new one for the same moment) */
        double left = tv_node_timer_due - tv_performance_now();
        tv_node_timer_arm(left > 1 ? left : 1);
    }
    return undef(ctx);
}

NATIVE(n_request_check) {
    UNUSED;
    tv_loop_check_pending = true;
    tv_loop_check_ref = n > 0 && JSValueToBoolean(ctx, a[0]);
    return undef(ctx);
}

/* ------------------------------------------------------------------ os (Node.js's node_os.cc) */

NATIVE(n_os_info) {
    UNUSED;
    struct utsname u;
    if (uname(&u) != 0) memset(&u, 0, sizeof u);
    JSValueRef items[4] = {str(ctx, u.sysname), str(ctx, u.version), str(ctx, u.release), str(ctx, u.machine)};
    return array(ctx, 4, items);
}

NATIVE(n_os_hostname) {
    UNUSED;
    char buf[256];
    if (gethostname(buf, sizeof buf) != 0) return tv_node_ctx_error(ctx, n, a, 0, errno, "uv_os_gethostname");
    buf[sizeof buf - 1] = 0;
    return str(ctx, buf);
}

static const char *tv_node_home(void) {
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : NULL;
}

NATIVE(n_os_homedir) {
    UNUSED;
    const char *h = tv_node_home();
    if (!h) return tv_node_ctx_error(ctx, n, a, 0, ENOENT, "uv_os_homedir");
    return str(ctx, h);
}

NATIVE(n_os_uptime) {
    UNUSED;
#if defined(__APPLE__)
    struct timeval boot;
    size_t len = sizeof boot;
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boot, &len, NULL, 0) != 0) return tv_node_ctx_error(ctx, n, a, 0, errno, "uv_uptime");
    return num(ctx, (double)(time(NULL) - boot.tv_sec));
#else
    struct sysinfo si;
    if (sysinfo(&si) != 0) return tv_node_ctx_error(ctx, n, a, 0, errno, "uv_uptime");
    return num(ctx, (double)si.uptime);
#endif
}

NATIVE(n_os_totalmem) {
    UNUSED;
#if defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
    return num(ctx, (double)mem);
#else
    struct sysinfo si;
    return num(ctx, sysinfo(&si) == 0 ? (double)si.totalram * si.mem_unit : 0);
#endif
}

static double tv_node_free_mem(void) {
#if defined(__APPLE__)
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS) return 0;
    return (double)vm.free_count * (double)sysconf(_SC_PAGESIZE);
#else
    FILE *f = fopen("/proc/meminfo", "r");
    double kb = 0;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemAvailable: %lf kB", &kb) == 1) break;
        }
        fclose(f);
    }
    return kb * 1024;
#endif
}

NATIVE(n_os_freemem) {
    UNUSED;
    return num(ctx, tv_node_free_mem());
}

NATIVE(n_os_loadavg) {
    UNUSED;
    double avg[3] = {0, 0, 0};
    getloadavg(avg, 3);
    JSValueRef items[3] = {num(ctx, avg[0]), num(ctx, avg[1]), num(ctx, avg[2])};
    return array(ctx, 3, items);
}

/* [model, speed (MHz), user, nice, sys, idle, irq (ms), ...] */
NATIVE(n_os_cpus) {
    UNUSED;
#if defined(__APPLE__)
    char model[256] = "";
    size_t len = sizeof model;
    sysctlbyname("machdep.cpu.brand_string", model, &len, NULL, 0);
    uint64_t freq = 0;
    len = sizeof freq;
    if (sysctlbyname("hw.cpufrequency", &freq, &len, NULL, 0) != 0) freq = 0;
    natural_t count = 0;
    processor_info_array_t info;
    mach_msg_type_number_t info_count;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count, &info, &info_count) != KERN_SUCCESS) return array(ctx, 0, NULL);
    long tick = sysconf(_SC_CLK_TCK);
    double mult = tick > 0 ? 1000.0 / (double)tick : 10;
    JSValueRef *items = malloc(sizeof(JSValueRef) * count * 7);
    for (natural_t i = 0; i < count; i++) {
        processor_cpu_load_info_t l = (processor_cpu_load_info_t)info + i;
        items[i * 7 + 0] = str(ctx, model);
        items[i * 7 + 1] = num(ctx, (double)(freq / 1000000));
        items[i * 7 + 2] = num(ctx, l->cpu_ticks[CPU_STATE_USER] * mult);
        items[i * 7 + 3] = num(ctx, l->cpu_ticks[CPU_STATE_NICE] * mult);
        items[i * 7 + 4] = num(ctx, l->cpu_ticks[CPU_STATE_SYSTEM] * mult);
        items[i * 7 + 5] = num(ctx, l->cpu_ticks[CPU_STATE_IDLE] * mult);
        items[i * 7 + 6] = num(ctx, 0);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)info, info_count * sizeof(int));
    JSObjectRef out = array(ctx, count * 7, items);
    free(items);
    return out;
#else
    char model[256] = "unknown";
    double mhz = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "model name", 10)) {
                char *c = strchr(line, ':');
                if (c) {
                    snprintf(model, sizeof model, "%s", c + 2);
                    model[strcspn(model, "\n")] = 0;
                }
            } else if (!strncmp(line, "cpu MHz", 7)) {
                char *c = strchr(line, ':');
                if (c && mhz == 0) mhz = atof(c + 1);
            }
        }
        fclose(f);
    }
    f = fopen("/proc/stat", "r");
    if (!f) return array(ctx, 0, NULL);
    long tick = sysconf(_SC_CLK_TCK);
    double mult = tick > 0 ? 1000.0 / (double)tick : 10;
    JSValueRef items[1024 * 7];
    size_t k = 0;
    char line[512];
    while (fgets(line, sizeof line, f) && k + 7 <= sizeof items / sizeof *items) {
        unsigned long long user, nice, sys, idle, iowait, irq;
        int id;
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu", &id, &user, &nice, &sys, &idle, &iowait, &irq) != 7) continue;
        items[k++] = str(ctx, model);
        items[k++] = num(ctx, (double)(long)mhz);
        items[k++] = num(ctx, (double)user * mult);
        items[k++] = num(ctx, (double)nice * mult);
        items[k++] = num(ctx, (double)sys * mult);
        items[k++] = num(ctx, (double)idle * mult);
        items[k++] = num(ctx, (double)irq * mult);
    }
    fclose(f);
    return array(ctx, k, items);
#endif
}

/* [name, address, netmask, family, mac, internal, scopeid, ...] */
NATIVE(n_os_interfaces) {
    UNUSED;
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) return tv_node_ctx_error(ctx, n, a, 0, errno, "uv_interface_addresses");
    size_t cap = 64, k = 0;
    JSValueRef *items = malloc(sizeof(JSValueRef) * cap * 7);
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || !(i->ifa_flags & IFF_UP)) continue;
        int fam = i->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6) continue;
        char addr[INET6_ADDRSTRLEN] = "", mask[INET6_ADDRSTRLEN] = "", mac[18] = "00:00:00:00:00:00";
        double scope = -1;
        if (fam == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, addr, sizeof addr);
            if (i->ifa_netmask) inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_netmask)->sin_addr, mask, sizeof mask);
        } else {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)i->ifa_addr;
            inet_ntop(AF_INET6, &s6->sin6_addr, addr, sizeof addr);
            if (i->ifa_netmask) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)i->ifa_netmask)->sin6_addr, mask, sizeof mask);
            scope = s6->sin6_scope_id;
        }
        /* the interface's hardware address */
        for (struct ifaddrs *j = list; j; j = j->ifa_next) {
            if (!j->ifa_addr || strcmp(j->ifa_name, i->ifa_name) != 0) continue;
#if defined(__APPLE__)
            if (j->ifa_addr->sa_family == AF_LINK) {
                struct sockaddr_dl *dl = (struct sockaddr_dl *)j->ifa_addr;
                unsigned char *m = (unsigned char *)LLADDR(dl);
                if (dl->sdl_alen == 6) snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
            }
#else
            if (j->ifa_addr->sa_family == AF_PACKET) {
                struct sockaddr_ll *ll = (struct sockaddr_ll *)j->ifa_addr;
                unsigned char *m = ll->sll_addr;
                if (ll->sll_halen == 6) snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
            }
#endif
        }
        if (k + 7 > cap * 7) {
            cap *= 2;
            items = realloc(items, sizeof(JSValueRef) * cap * 7);
        }
        items[k++] = str(ctx, i->ifa_name);
        items[k++] = str(ctx, addr);
        items[k++] = str(ctx, mask);
        items[k++] = str(ctx, fam == AF_INET ? "IPv4" : "IPv6");
        items[k++] = str(ctx, mac);
        items[k++] = JSValueMakeBoolean(ctx, (i->ifa_flags & IFF_LOOPBACK) != 0);
        items[k++] = num(ctx, scope);
    }
    freeifaddrs(list);
    JSObjectRef out = array(ctx, k, items);
    free(items);
    return out;
}

NATIVE(n_os_userinfo) {
    UNUSED;
    struct passwd *pw = getpwuid(geteuid());
    if (!pw) return tv_node_ctx_error(ctx, n, a, 1, errno ? errno : ENOENT, "uv_os_get_passwd");
    JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
    set(ctx, o, "uid", num(ctx, pw->pw_uid));
    set(ctx, o, "gid", num(ctx, pw->pw_gid));
    set(ctx, o, "username", str(ctx, pw->pw_name));
    set(ctx, o, "homedir", str(ctx, pw->pw_dir));
    set(ctx, o, "shell", pw->pw_shell ? str(ctx, pw->pw_shell) : JSValueMakeNull(ctx));
    return o;
}

NATIVE(n_os_getpriority) {
    UNUSED;
    errno = 0;
    int p = getpriority(PRIO_PROCESS, (id_t)arg_num(ctx, n, a, 0, 0));
    if (p == -1 && errno) return tv_node_ctx_error(ctx, n, a, 1, errno, "uv_os_getpriority");
    return num(ctx, p);
}

NATIVE(n_os_setpriority) {
    UNUSED;
    if (setpriority(PRIO_PROCESS, (id_t)arg_num(ctx, n, a, 0, 0), (int)arg_num(ctx, n, a, 1, 0)) != 0)
        return tv_node_ctx_error(ctx, n, a, 2, errno, "uv_os_setpriority");
    return num(ctx, 0);
}

NATIVE(n_os_parallelism) {
    UNUSED;
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    return num(ctx, c > 0 ? (double)c : 1);
}

/* ------------------------------------------------------------------ tty */

#include "node_tty.h"

/* The bytes a program asked to be written to its terminal however it ends (say, leaving the
 * alternate screen): at exit after the last of stdout, on a trap before its message, and on a
 * fatal SIGINT/SIGTERM while raw mode is on. Set with setResetOnExit(fd, string) on a terminal
 * fd (a copy of it is kept, so closing the stream does not lose it), cleared with "". Written
 * once; the signal handler reads it, so updates block those signals. */
static int tv_node_reset_fd = -1;
static char *tv_node_reset;
static size_t tv_node_reset_len;

static void tv_node_reset_write(void) { /* async-signal-safe */
    int fd = tv_node_reset_fd;
    if (fd < 0) return;
    tv_node_reset_fd = -1;
    const char *s = tv_node_reset;
    size_t len = tv_node_reset_len;
    while (len) {
        ssize_t w = write(fd, s, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        len -= (size_t)w;
    }
}

static void tv_node_reset_at_exit(void) {
    tv_out_flush();
    tv_node_reset_write();
}

NATIVE(n_set_reset_on_exit) {
    UNUSED;
    int fd = (int)arg_num(ctx, n, a, 0, -1);
    char *text = arg_cstr(ctx, n, a, 1);
    if (!text) return num(ctx, -EINVAL);
    int copy = -1;
    if (*text) {
        if (fd < 0 || !isatty(fd)) { free(text); return num(ctx, -ENOTTY); }
        copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (copy < 0) { int err = errno; free(text); return num(ctx, -err); }
    } else {
        free(text);
        text = NULL;
    }
    sigset_t before;
    tv_node_tty_block_signals(&before);
    int old_fd = tv_node_reset_fd;
    char *old = tv_node_reset;
    tv_node_reset_fd = -1;
    tv_node_reset = text;
    tv_node_reset_len = text ? strlen(text) : 0;
    tv_node_reset_fd = copy;
    sigprocmask(SIG_SETMASK, &before, NULL);
    if (old_fd >= 0) close(old_fd);
    free(old);
    static bool registered;
    if (copy >= 0 && !registered) {
        atexit(tv_node_reset_at_exit);
        tv_trap_cleanup = tv_node_reset_write;
        registered = true;
    }
    return num(ctx, 0);
}


static bool tv_node_tty_master(int fd) {
#ifdef TIOCGPTN
    unsigned int index;
    if (ioctl(fd, TIOCGPTN, &index) == 0) return true;
#endif
#ifdef TIOCPTYGNAME
    char path[128];
    if (ioctl(fd, TIOCPTYGNAME, path) == 0) return true;
#endif
    return false;
}

/* ------------------------------------------------------------------ fs (synchronous; the JS binding
 * runs them for callbacks and promises too, completing on the next loop turn) */

#include <dirent.h>
#include <limits.h>
#if defined(__APPLE__)
#include <copyfile.h>
#include <sys/mount.h>
#else
#include <sys/statfs.h>
#include <sys/sendfile.h>
#endif

/* a path argument: a string, or bytes (a Buffer path); malloc'd and NUL-terminated */
static char *arg_path(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i) {
    char *s = arg_cstr(ctx, n, a, i);
    if (s) return s;
    uint8_t *p;
    size_t len;
    if (i < n && tv_js_bytes_view(a[i], &p, &len)) {
        s = malloc(len + 1);
        memcpy(s, p, len);
        s[len] = 0;
        return s;
    }
    return NULL;
}

#define PATH_ARG(var, i) char *var = arg_path(ctx, n, a, i); if (!var) return tv_node_throw(ctx, exc, EINVAL, "open", NULL, NULL)
#define FAIL(syscall, path) do { int e_ = errno; JSValueRef r_ = tv_node_throw(ctx, exc, e_, syscall, path, NULL); return r_; } while (0)

NATIVE(n_fs_open) {
    UNUSED;
    PATH_ARG(path, 0);
    int fd = open(path, (int)arg_num(ctx, n, a, 1, O_RDONLY) | O_CLOEXEC, (mode_t)arg_num(ctx, n, a, 2, 0666));
    if (fd < 0) { int e = errno; JSValueRef r = tv_node_throw(ctx, exc, e, "open", path, NULL); free(path); return r; }
    free(path);
    return num(ctx, fd);
}

NATIVE(n_fs_close) {
    UNUSED;
    if (close((int)arg_num(ctx, n, a, 0, -1)) != 0 && errno != EINTR) return tv_node_throw(ctx, exc, errno, "close", NULL, NULL);
    return undef(ctx);
}

/* read(fd, buffer, offset, length, position): position < 0 reads at the current offset */
NATIVE(n_fs_read) {
    UNUSED;
    int fd = (int)arg_num(ctx, n, a, 0, -1);
    uint8_t *p;
    size_t len;
    if (n < 2 || !tv_js_bytes_view(a[1], &p, &len)) return tv_node_throw(ctx, exc, EINVAL, "read", NULL, NULL);
    size_t off = (size_t)arg_num(ctx, n, a, 2, 0), want = (size_t)arg_num(ctx, n, a, 3, (double)len);
    double pos = arg_num(ctx, n, a, 4, -1);
    if (off > len) off = len;
    if (want > len - off) want = len - off;
    for (;;) {
        ssize_t r = pos >= 0 ? pread(fd, p + off, want, (off_t)pos) : read(fd, p + off, want);
        if (r >= 0) return num(ctx, (double)r);
        if (errno != EINTR) return tv_node_throw(ctx, exc, errno, "read", NULL, NULL);
    }
}

/* write(fd, buffer, offset, length, position) */
NATIVE(n_fs_write) {
    UNUSED;
    int fd = (int)arg_num(ctx, n, a, 0, -1);
    uint8_t *p;
    size_t len;
    if (n < 2 || !tv_js_bytes_view(a[1], &p, &len)) return tv_node_throw(ctx, exc, EINVAL, "write", NULL, NULL);
    size_t off = (size_t)arg_num(ctx, n, a, 2, 0), want = (size_t)arg_num(ctx, n, a, 3, (double)len);
    double pos = arg_num(ctx, n, a, 4, -1);
    if (off > len) off = len;
    if (want > len - off) want = len - off;
    for (;;) {
        ssize_t w = pos >= 0 ? pwrite(fd, p + off, want, (off_t)pos) : write(fd, p + off, want);
        if (w >= 0) return num(ctx, (double)w);
        if (errno != EINTR) return tv_node_throw(ctx, exc, errno, "write", NULL, NULL);
    }
}

/* Node.js's 18 stat fields: dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks, then
 * atime, mtime, ctime and birthtime as (seconds, nanoseconds) */
static JSValueRef tv_node_stat_array(JSContextRef ctx, const struct stat *st) {
#if defined(__APPLE__)
    struct timespec at = st->st_atimespec, mt = st->st_mtimespec, ct = st->st_ctimespec, bt = st->st_birthtimespec;
#else
    struct timespec at = st->st_atim, mt = st->st_mtim, ct = st->st_ctim, bt = st->st_ctim;
#endif
    JSValueRef v[18] = {
        num(ctx, (double)st->st_dev), num(ctx, st->st_mode), num(ctx, (double)st->st_nlink), num(ctx, st->st_uid),
        num(ctx, st->st_gid), num(ctx, (double)st->st_rdev), num(ctx, (double)st->st_blksize), num(ctx, (double)st->st_ino),
        num(ctx, (double)st->st_size), num(ctx, (double)st->st_blocks),
        num(ctx, (double)at.tv_sec), num(ctx, (double)at.tv_nsec), num(ctx, (double)mt.tv_sec), num(ctx, (double)mt.tv_nsec),
        num(ctx, (double)ct.tv_sec), num(ctx, (double)ct.tv_nsec), num(ctx, (double)bt.tv_sec), num(ctx, (double)bt.tv_nsec),
    };
    return array(ctx, 18, v);
}

/* stat(path, throwIfNoEntry) / lstat: the 18 fields, or undefined for a missing entry when
 * throwIfNoEntry is false */
static JSValueRef tv_node_stat(JSContextRef ctx, size_t n, const JSValueRef a[], JSValueRef *exc, bool link) {
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return tv_node_throw(ctx, exc, EINVAL, link ? "lstat" : "stat", NULL, NULL);
    struct stat st;
    int r = link ? lstat(path, &st) : stat(path, &st);
    if (r != 0) {
        int e = errno;
        bool quiet = n > 1 && JSValueIsBoolean(ctx, a[1]) && !JSValueToBoolean(ctx, a[1]) && (e == ENOENT || e == ENOTDIR);
        JSValueRef out = quiet ? undef(ctx) : tv_node_throw(ctx, exc, e, link ? "lstat" : "stat", path, NULL);
        free(path);
        return out;
    }
    free(path);
    return tv_node_stat_array(ctx, &st);
}

NATIVE(n_fs_stat) { UNUSED; return tv_node_stat(ctx, n, a, exc, false); }
NATIVE(n_fs_lstat) { UNUSED; return tv_node_stat(ctx, n, a, exc, true); }

NATIVE(n_fs_fstat) {
    UNUSED;
    struct stat st;
    if (fstat((int)arg_num(ctx, n, a, 0, -1), &st) != 0) return tv_node_throw(ctx, exc, errno, "fstat", NULL, NULL);
    return tv_node_stat_array(ctx, &st);
}

/* Node.js's dirent types (UV_DIRENT_*) */
static int tv_node_dirent_type(unsigned char t) {
    switch (t) {
    case DT_REG: return 1;
    case DT_DIR: return 2;
    case DT_LNK: return 3;
    case DT_FIFO: return 4;
    case DT_SOCK: return 5;
    case DT_CHR: return 6;
    case DT_BLK: return 7;
    default: return 0;
    }
}

/* readdir(path, withTypes): names, or [names, types] */
NATIVE(n_fs_readdir) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return tv_node_throw(ctx, exc, EINVAL, "scandir", NULL, NULL);
    bool types = n > 1 && JSValueToBoolean(ctx, a[1]);
    DIR *d = opendir(path);
    if (!d) { int e = errno; JSValueRef r = tv_node_throw(ctx, exc, e, "scandir", path, NULL); free(path); return r; }
    size_t cap = 64, k = 0;
    JSValueRef *names = malloc(sizeof(JSValueRef) * cap), *kinds = malloc(sizeof(JSValueRef) * cap);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (k == cap) {
            cap *= 2;
            names = realloc(names, sizeof(JSValueRef) * cap);
            kinds = realloc(kinds, sizeof(JSValueRef) * cap);
        }
        names[k] = str(ctx, e->d_name);
        kinds[k] = num(ctx, tv_node_dirent_type(e->d_type));
        k++;
    }
    closedir(d);
    free(path);
    JSObjectRef list = array(ctx, k, names);
    JSValueRef out = list;
    if (types) {
        JSValueRef pair[2] = {list, array(ctx, k, kinds)};
        out = array(ctx, 2, pair);
    }
    free(names);
    free(kinds);
    return out;
}

/* mkdir(path, mode, recursive): with recursive, the first directory it made (else undefined) */
NATIVE(n_fs_mkdir) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return tv_node_throw(ctx, exc, EINVAL, "mkdir", NULL, NULL);
    mode_t mode = (mode_t)arg_num(ctx, n, a, 1, 0777);
    bool recursive = n > 2 && JSValueToBoolean(ctx, a[2]);
    if (!recursive) {
        if (mkdir(path, mode) != 0) { int e = errno; JSValueRef r = tv_node_throw(ctx, exc, e, "mkdir", path, NULL); free(path); return r; }
        free(path);
        return undef(ctx);
    }
    char *first = NULL;
    size_t len = strlen(path);
    for (size_t i = 1; i <= len; i++) {
        if (i < len && path[i] != '/') continue;
        char c = path[i];
        path[i] = 0;
        if (mkdir(path, mode) == 0) {
            if (!first) first = strdup(path);
        } else if (errno != EEXIST) {
            int e = errno;
            path[i] = c;
            JSValueRef r = tv_node_throw(ctx, exc, e, "mkdir", path, NULL);
            free(path);
            free(first);
            return r;
        } else {
            struct stat st;
            if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
                path[i] = c;
                JSValueRef r = tv_node_throw(ctx, exc, i == len ? EEXIST : ENOTDIR, "mkdir", path, NULL);
                free(path);
                free(first);
                return r;
            }
        }
        path[i] = c;
    }
    free(path);
    JSValueRef out = first ? str(ctx, first) : undef(ctx);
    free(first);
    return out;
}

#define PATH_OP(name, syscall, call) \
    NATIVE(name) { \
        UNUSED; \
        char *path = arg_path(ctx, n, a, 0); \
        if (!path) return tv_node_throw(ctx, exc, EINVAL, syscall, NULL, NULL); \
        if ((call) != 0) { int e = errno; JSValueRef r = tv_node_throw(ctx, exc, e, syscall, path, NULL); free(path); return r; } \
        free(path); \
        return undef(ctx); \
    }

PATH_OP(n_fs_rmdir, "rmdir", rmdir(path))
PATH_OP(n_fs_unlink, "unlink", unlink(path))
PATH_OP(n_fs_chmod, "chmod", chmod(path, (mode_t)arg_num(ctx, n, a, 1, 0)))
PATH_OP(n_fs_chown, "chown", chown(path, (uid_t)arg_num(ctx, n, a, 1, -1), (gid_t)arg_num(ctx, n, a, 2, -1)))
PATH_OP(n_fs_lchown, "lchown", lchown(path, (uid_t)arg_num(ctx, n, a, 1, -1), (gid_t)arg_num(ctx, n, a, 2, -1)))
PATH_OP(n_fs_access, "access", access(path, (int)arg_num(ctx, n, a, 1, F_OK)))

static struct timespec tv_node_ts(double secs) {
    struct timespec t;
    t.tv_sec = (time_t)secs;
    t.tv_nsec = (long)((secs - (double)t.tv_sec) * 1e9);
    if (t.tv_nsec < 0) { t.tv_sec--; t.tv_nsec += 1000000000; }
    return t;
}

/* utimes(path, atime, mtime) / lutimes, in seconds */
static JSValueRef tv_node_utimes(JSContextRef ctx, size_t n, const JSValueRef a[], JSValueRef *exc, int flags, const char *syscall) {
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return tv_node_throw(ctx, exc, EINVAL, syscall, NULL, NULL);
    struct timespec t[2] = {tv_node_ts(arg_num(ctx, n, a, 1, 0)), tv_node_ts(arg_num(ctx, n, a, 2, 0))};
    if (utimensat(AT_FDCWD, path, t, flags) != 0) { int e = errno; JSValueRef r = tv_node_throw(ctx, exc, e, syscall, path, NULL); free(path); return r; }
    free(path);
    return undef(ctx);
}
NATIVE(n_fs_utimes) { UNUSED; return tv_node_utimes(ctx, n, a, exc, 0, "utime"); }
NATIVE(n_fs_lutimes) { UNUSED; return tv_node_utimes(ctx, n, a, exc, AT_SYMLINK_NOFOLLOW, "lutime"); }

NATIVE(n_fs_futimes) {
    UNUSED;
    struct timespec t[2] = {tv_node_ts(arg_num(ctx, n, a, 1, 0)), tv_node_ts(arg_num(ctx, n, a, 2, 0))};
    if (futimens((int)arg_num(ctx, n, a, 0, -1), t) != 0) return tv_node_throw(ctx, exc, errno, "futime", NULL, NULL);
    return undef(ctx);
}

#define FD_OP(name, syscall, call) \
    NATIVE(name) { \
        UNUSED; \
        int fd = (int)arg_num(ctx, n, a, 0, -1); \
        if ((call) != 0) return tv_node_throw(ctx, exc, errno, syscall, NULL, NULL); \
        return undef(ctx); \
    }

FD_OP(n_fs_fsync, "fsync", fsync(fd))
#if defined(__APPLE__)
FD_OP(n_fs_fdatasync, "fdatasync", fsync(fd))
#else
FD_OP(n_fs_fdatasync, "fdatasync", fdatasync(fd))
#endif
FD_OP(n_fs_ftruncate, "ftruncate", ftruncate(fd, (off_t)arg_num(ctx, n, a, 1, 0)))
FD_OP(n_fs_fchmod, "fchmod", fchmod(fd, (mode_t)arg_num(ctx, n, a, 1, 0)))
FD_OP(n_fs_fchown, "fchown", fchown(fd, (uid_t)arg_num(ctx, n, a, 1, -1), (gid_t)arg_num(ctx, n, a, 2, -1)))

/* two-path operations: rename(from, to), link, symlink(target, path) */
static JSValueRef tv_node_two_paths(JSContextRef ctx, size_t n, const JSValueRef a[], JSValueRef *exc, const char *syscall, int which) {
    char *from = arg_path(ctx, n, a, 0), *to = arg_path(ctx, n, a, 1);
    if (!from || !to) { free(from); free(to); return tv_node_throw(ctx, exc, EINVAL, syscall, NULL, NULL); }
    int r = which == 0 ? rename(from, to) : which == 1 ? link(from, to) : symlink(from, to);
    JSValueRef out = undef(ctx);
    if (r != 0) out = tv_node_throw(ctx, exc, errno, syscall, from, to);
    free(from);
    free(to);
    return out;
}
NATIVE(n_fs_rename) { UNUSED; return tv_node_two_paths(ctx, n, a, exc, "rename", 0); }
NATIVE(n_fs_link) { UNUSED; return tv_node_two_paths(ctx, n, a, exc, "link", 1); }
NATIVE(n_fs_symlink) { UNUSED; return tv_node_two_paths(ctx, n, a, exc, "symlink", 2); }

NATIVE(n_fs_readlink) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return tv_node_throw(ctx, exc, EINVAL, "readlink", NULL, NULL);
    char buf[PATH_MAX + 1];
    ssize_t k = readlink(path, buf, PATH_MAX);
    if (k < 0) { int e = errno; JSValueRef r = tv_node_throw(ctx, exc, e, "readlink", path, NULL); free(path); return r; }
    buf[k] = 0;
    free(path);
    return str(ctx, buf);
}

NATIVE(n_fs_realpath) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return tv_node_throw(ctx, exc, EINVAL, "realpath", NULL, NULL);
    char buf[PATH_MAX + 1];
    if (!realpath(path, buf)) { int e = errno; JSValueRef r = tv_node_throw(ctx, exc, e, "realpath", path, NULL); free(path); return r; }
    free(path);
    return str(ctx, buf);
}

/* copyFile(src, dest, mode): COPYFILE_EXCL (1) fails if dest exists; FICLONE modes clone
 * where the file system can, else copy */
NATIVE(n_fs_copyfile) {
    UNUSED;
    char *src = arg_path(ctx, n, a, 0), *dest = arg_path(ctx, n, a, 1);
    if (!src || !dest) { free(src); free(dest); return tv_node_throw(ctx, exc, EINVAL, "copyfile", NULL, NULL); }
    int mode = (int)arg_num(ctx, n, a, 2, 0);
    JSValueRef out = undef(ctx);
    if ((mode & 1) && access(dest, F_OK) == 0) {
        out = tv_node_throw(ctx, exc, EEXIST, "copyfile", src, dest);
    } else {
#if defined(__APPLE__)
        if (copyfile(src, dest, NULL, COPYFILE_ALL | ((mode & 2) ? COPYFILE_CLONE : 0)) != 0) out = tv_node_throw(ctx, exc, errno, "copyfile", src, dest);
#else
        int in = open(src, O_RDONLY | O_CLOEXEC);
        struct stat st;
        int o = in >= 0 && fstat(in, &st) == 0 ? open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777) : -1;
        if (in < 0 || o < 0) {
            out = tv_node_throw(ctx, exc, errno, "copyfile", src, dest);
        } else {
            off_t left = st.st_size;
            while (left > 0) {
                ssize_t k = sendfile(o, in, NULL, (size_t)left);
                if (k <= 0) { if (k < 0 && errno == EINTR) continue; if (k < 0) out = tv_node_throw(ctx, exc, errno, "copyfile", src, dest); break; }
                left -= k;
            }
        }
        if (in >= 0) close(in);
        if (o >= 0) close(o);
#endif
    }
    free(src);
    free(dest);
    return out;
}

NATIVE(n_fs_mkdtemp) {
    UNUSED;
    char *prefix = arg_path(ctx, n, a, 0);
    if (!prefix) return tv_node_throw(ctx, exc, EINVAL, "mkdtemp", NULL, NULL);
    size_t pl = strlen(prefix);
    char *tmpl = malloc(pl + 7);
    memcpy(tmpl, prefix, pl);
    memcpy(tmpl + pl, "XXXXXX", 7);
    JSValueRef out;
    if (!mkdtemp(tmpl)) {
        char shown[PATH_MAX + 8];
        snprintf(shown, sizeof shown, "%sXXXXXX", prefix);
        out = tv_node_throw(ctx, exc, errno, "mkdtemp", shown, NULL);
    } else {
        out = str(ctx, tmpl);
    }
    free(tmpl);
    free(prefix);
    return out;
}

/* statfs(path): [type, bsize, blocks, bfree, bavail, files, ffree] */
NATIVE(n_fs_statfs) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return tv_node_throw(ctx, exc, EINVAL, "statfs", NULL, NULL);
    struct statfs s;
    if (statfs(path, &s) != 0) { int e = errno; JSValueRef r = tv_node_throw(ctx, exc, e, "statfs", path, NULL); free(path); return r; }
    free(path);
    JSValueRef v[7] = {num(ctx, (double)s.f_type), num(ctx, (double)s.f_bsize), num(ctx, (double)s.f_blocks), num(ctx, (double)s.f_bfree),
                       num(ctx, (double)s.f_bavail), num(ctx, (double)s.f_files), num(ctx, (double)s.f_ffree)};
    return array(ctx, 7, v);
}

/* removes path and, if it's a directory, everything in it */
static int tv_node_rm_tree(char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    size_t pl = strlen(path);
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t nl = strlen(e->d_name);
        char *child = malloc(pl + nl + 2);
        memcpy(child, path, pl);
        child[pl] = '/';
        memcpy(child + pl + 1, e->d_name, nl + 1);
        if (tv_node_rm_tree(child) != 0) rc = -1;
        free(child);
        if (rc) break;
    }
    int keep = errno;
    closedir(d);
    errno = keep;
    if (rc) return rc;
    return rmdir(path);
}

/* rmSync(path, maxRetries, recursive, retryDelay) */
NATIVE(n_fs_rm) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return tv_node_throw(ctx, exc, EINVAL, "rm", NULL, NULL);
    bool recursive = n > 2 && JSValueToBoolean(ctx, a[2]);
    struct stat st;
    int rc;
    if (lstat(path, &st) != 0) rc = -1;
    else if (S_ISDIR(st.st_mode) && !recursive) { errno = EISDIR; rc = -1; }
    else rc = recursive ? tv_node_rm_tree(path) : unlink(path);
    JSValueRef out = undef(ctx);
    if (rc != 0) {
        int e = errno;
        if (e == EISDIR) {
            /* Node.js: ERR_FS_EISDIR "Path is a directory: rm returned EISDIR (is a directory) path" */
            char text[PATH_MAX + 128];
            snprintf(text, sizeof text, "Path is a directory: rm returned EISDIR (is a directory) %s", path);
            JSValueRef msg = str(ctx, text);
            JSObjectRef err = JSObjectMakeError(ctx, 1, &msg, NULL);
            set(ctx, err, "code", str(ctx, "ERR_FS_EISDIR"));
            set(ctx, err, "errno", num(ctx, EISDIR));
            set(ctx, err, "syscall", str(ctx, "rm"));
            set(ctx, err, "path", str(ctx, path));
            *exc = err;
        } else {
            out = tv_node_throw(ctx, exc, e, "rm", path, NULL);
        }
    }
    free(path);
    return out;
}

/* whole files: readFileUtf8(path | fd, flags) and writeFileUtf8(path | fd, data, flags, mode) */
static int tv_node_open_arg(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i, int flags, int mode, bool *owned, char **path) {
    *path = NULL;
    if (i < n && JSValueIsNumber(ctx, a[i])) {
        *owned = false;
        return (int)JSValueToNumber(ctx, a[i], NULL);
    }
    *owned = true;
    *path = arg_path(ctx, n, a, i);
    if (!*path) { errno = EINVAL; return -1; }
    return open(*path, flags | O_CLOEXEC, mode);
}

NATIVE(n_fs_read_utf8) {
    UNUSED;
    bool owned;
    char *path;
    int fd = tv_node_open_arg(ctx, n, a, 0, (int)arg_num(ctx, n, a, 1, O_RDONLY), 0666, &owned, &path);
    if (fd < 0) { int e = errno; JSValueRef r = tv_node_throw(ctx, exc, e, "open", path, NULL); free(path); return r; }
    tv_sb sb = {0};
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) tv_sb_grow(&sb, (size_t)st.st_size + 1);
    else if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (owned) close(fd);
        JSValueRef r = tv_node_throw(ctx, exc, EISDIR, "read", NULL, NULL);
        free(path);
        return r;
    }
    for (;;) {
        if (sb.cap - sb.len < 65536) tv_sb_grow(&sb, sb.len + 65536);
        ssize_t k = read(fd, sb.data + sb.len, sb.cap - sb.len);
        if (k > 0) { sb.len += (size_t)k; continue; }
        if (k == 0) break;
        if (errno == EINTR) continue;
        int e = errno;
        if (owned) close(fd);
        tv_sb_free(&sb);
        JSValueRef r = tv_node_throw(ctx, exc, e, "read", NULL, NULL);
        free(path);
        return r;
    }
    if (owned) close(fd);
    free(path);
    JSValueRef out = tv_js_str(sb.data ? sb.data : "", sb.len);
    tv_sb_free(&sb);
    return out;
}

NATIVE(n_fs_write_utf8) {
    UNUSED;
    if (n < 2) return undef(ctx);
    bool owned;
    char *path;
    int fd = tv_node_open_arg(ctx, n, a, 0, (int)arg_num(ctx, n, a, 2, O_WRONLY | O_CREAT | O_TRUNC), (int)arg_num(ctx, n, a, 3, 0666), &owned, &path);
    if (fd < 0) { int e = errno; JSValueRef r = tv_node_throw(ctx, exc, e, "open", path, NULL); free(path); return r; }
    JSStringRef s = JSValueToStringCopy(ctx, a[1], NULL);
    size_t cap = JSStringGetMaximumUTF8CStringSize(s);
    char *text = malloc(cap);
    size_t len = JSStringGetUTF8CString(s, text, cap) - 1;
    JSStringRelease(s);
    size_t off = 0;
    JSValueRef out = undef(ctx);
    while (off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            out = tv_node_throw(ctx, exc, errno, "write", NULL, NULL);
            break;
        }
        off += (size_t)w;
    }
    free(text);
    if (owned) close(fd);
    free(path);
    return out;
}

NATIVE(n_fs_exists) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return JSValueMakeBoolean(ctx, false);
    bool ok = access(path, F_OK) == 0;
    free(path);
    return JSValueMakeBoolean(ctx, ok);
}

/* internalModuleStat(path): 0 file, 1 directory, negative errno */
NATIVE(n_fs_module_stat) {
    UNUSED;
    char *path = arg_path(ctx, n, a, n > 1 ? 1 : 0);
    if (!path) return num(ctx, -ENOENT);
    struct stat st;
    int r = stat(path, &st) == 0 ? (S_ISDIR(st.st_mode) ? 1 : 0) : -errno;
    free(path);
    return num(ctx, r);
}

void tv_node_fs_install(JSContextRef ctx, JSObjectRef native) {
    JSObjectRef fs = JSObjectMake(ctx, NULL, NULL);
    tv_js_def(ctx, fs, "open", n_fs_open);
    tv_js_def(ctx, fs, "close", n_fs_close);
    tv_js_def(ctx, fs, "read", n_fs_read);
    tv_js_def(ctx, fs, "write", n_fs_write);
    tv_js_def(ctx, fs, "stat", n_fs_stat);
    tv_js_def(ctx, fs, "lstat", n_fs_lstat);
    tv_js_def(ctx, fs, "fstat", n_fs_fstat);
    tv_js_def(ctx, fs, "readdir", n_fs_readdir);
    tv_js_def(ctx, fs, "mkdir", n_fs_mkdir);
    tv_js_def(ctx, fs, "rmdir", n_fs_rmdir);
    tv_js_def(ctx, fs, "unlink", n_fs_unlink);
    tv_js_def(ctx, fs, "chmod", n_fs_chmod);
    tv_js_def(ctx, fs, "chown", n_fs_chown);
    tv_js_def(ctx, fs, "lchown", n_fs_lchown);
    tv_js_def(ctx, fs, "access", n_fs_access);
    tv_js_def(ctx, fs, "utimes", n_fs_utimes);
    tv_js_def(ctx, fs, "lutimes", n_fs_lutimes);
    tv_js_def(ctx, fs, "futimes", n_fs_futimes);
    tv_js_def(ctx, fs, "fsync", n_fs_fsync);
    tv_js_def(ctx, fs, "fdatasync", n_fs_fdatasync);
    tv_js_def(ctx, fs, "ftruncate", n_fs_ftruncate);
    tv_js_def(ctx, fs, "fchmod", n_fs_fchmod);
    tv_js_def(ctx, fs, "fchown", n_fs_fchown);
    tv_js_def(ctx, fs, "rename", n_fs_rename);
    tv_js_def(ctx, fs, "link", n_fs_link);
    tv_js_def(ctx, fs, "symlink", n_fs_symlink);
    tv_js_def(ctx, fs, "readlink", n_fs_readlink);
    tv_js_def(ctx, fs, "realpath", n_fs_realpath);
    tv_js_def(ctx, fs, "copyFile", n_fs_copyfile);
    tv_js_def(ctx, fs, "mkdtemp", n_fs_mkdtemp);
    tv_js_def(ctx, fs, "statfs", n_fs_statfs);
    tv_js_def(ctx, fs, "rm", n_fs_rm);
    tv_js_def(ctx, fs, "readFileUtf8", n_fs_read_utf8);
    tv_js_def(ctx, fs, "writeFileUtf8", n_fs_write_utf8);
    tv_js_def(ctx, fs, "exists", n_fs_exists);
    tv_js_def(ctx, fs, "internalModuleStat", n_fs_module_stat);
    set(ctx, native, "fs", fs);
}

/* ------------------------------------------------------------------ crypto (runtime/crypto.c, through
 * tv_crypto: there when the program links the TLS archive) */

typedef struct { void *ctx; bool hmac; } tv_node_mac;

static void tv_node_mac_finalize(JSObjectRef o) {
    tv_node_mac *m = JSObjectGetPrivate(o);
    if (!m) return;
    if (m->ctx && tv_crypto) (m->hmac ? tv_crypto->hmac_free : tv_crypto->hash_free)(m->ctx);
    free(m);
}

static JSClassRef tv_node_mac_class(void) {
    static JSClassRef cls;
    if (!cls) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = "CryptoHandle";
        def.finalize = tv_node_mac_finalize;
        cls = JSClassCreate(&def);
    }
    return cls;
}

static JSValueRef tv_node_crypto_missing(JSContextRef ctx, JSValueRef *exc) {
    JSValueRef msg = str(ctx, "crypto is not available in this program (it was built without the TLS library)");
    *exc = JSObjectMakeError(ctx, 1, &msg, NULL);
    return undef(ctx);
}

static tv_node_mac *tv_node_mac_of(JSContextRef ctx, size_t n, const JSValueRef a[]) {
    if (n < 1 || !JSValueIsObjectOfClass(ctx, a[0], tv_node_mac_class())) return NULL;
    return JSObjectGetPrivate((JSObjectRef)a[0]);
}

static JSValueRef tv_node_mac_new(JSContextRef ctx, void *c, bool hmac) {
    tv_node_mac *m = malloc(sizeof *m);
    m->ctx = c;
    m->hmac = hmac;
    return JSObjectMake(ctx, tv_node_mac_class(), m);
}

/* hashNew(name) -> handle, or null for a digest BoringSSL doesn't have */
NATIVE(n_crypto_hash_new) {
    UNUSED;
    if (!tv_crypto) return tv_node_crypto_missing(ctx, exc);
    char *name = arg_cstr(ctx, n, a, 0);
    void *c = name ? tv_crypto->hash_new(name) : NULL;
    free(name);
    return c ? tv_node_mac_new(ctx, c, false) : JSValueMakeNull(ctx);
}

/* hmacNew(name, key bytes) -> handle, or null */
NATIVE(n_crypto_hmac_new) {
    UNUSED;
    if (!tv_crypto) return tv_node_crypto_missing(ctx, exc);
    char *name = arg_cstr(ctx, n, a, 0);
    uint8_t *k = NULL;
    size_t kl = 0;
    if (n > 1) tv_js_bytes_view(a[1], &k, &kl);
    void *c = name ? tv_crypto->hmac_new(name, k ? k : (const uint8_t *)"", kl) : NULL;
    free(name);
    return c ? tv_node_mac_new(ctx, c, true) : JSValueMakeNull(ctx);
}

/* macUpdate(handle, bytes) */
NATIVE(n_crypto_update) {
    UNUSED;
    tv_node_mac *m = tv_node_mac_of(ctx, n, a);
    uint8_t *p;
    size_t len;
    if (!m || !m->ctx || n < 2 || !tv_js_bytes_view(a[1], &p, &len)) return undef(ctx);
    (m->hmac ? tv_crypto->hmac_update : tv_crypto->hash_update)(m->ctx, p, len);
    return undef(ctx);
}

/* macDigest(handle) -> Uint8Array (the handle is spent) */
NATIVE(n_crypto_digest) {
    UNUSED;
    tv_node_mac *m = tv_node_mac_of(ctx, n, a);
    if (!m || !m->ctx) return undef(ctx);
    uint8_t out[64];
    size_t len = (m->hmac ? tv_crypto->hmac_final : tv_crypto->hash_final)(m->ctx, out);
    (m->hmac ? tv_crypto->hmac_free : tv_crypto->hash_free)(m->ctx);
    m->ctx = NULL;
    return tv_js_bytes_copy(out, len);
}

/* hashCopy(handle) -> a handle with the same state */
NATIVE(n_crypto_hash_copy) {
    UNUSED;
    tv_node_mac *m = tv_node_mac_of(ctx, n, a);
    if (!m || !m->ctx || m->hmac) return JSValueMakeNull(ctx);
    void *c = tv_crypto->hash_copy(m->ctx);
    return c ? tv_node_mac_new(ctx, c, false) : JSValueMakeNull(ctx);
}

NATIVE(n_crypto_digest_size) {
    UNUSED;
    if (!tv_crypto) return num(ctx, -1);
    char *name = arg_cstr(ctx, n, a, 0);
    int size = name ? tv_crypto->digest_size(name) : -1;
    free(name);
    return num(ctx, size);
}

/* randomFill(view): fills it with cryptographically secure bytes */
NATIVE(n_crypto_random_fill) {
    UNUSED;
    uint8_t *p;
    size_t len;
    if (n < 1 || !tv_js_bytes_view(a[0], &p, &len)) return undef(ctx);
    if (tv_crypto) tv_crypto->random(p, len);
    else arc4random_buf(p, len);
    return undef(ctx);
}

static bool tv_node_bytes_arg(size_t n, const JSValueRef a[], size_t i, uint8_t **p, size_t *len) {
    static uint8_t empty;
    if (i < n && tv_js_bytes_view(a[i], p, len)) return true;
    *p = &empty;
    *len = 0;
    return false;
}

/* pbkdf2(digest, password, salt, iterations, keylen) -> Uint8Array, or null for an unknown digest */
NATIVE(n_crypto_pbkdf2) {
    UNUSED;
    if (!tv_crypto) return tv_node_crypto_missing(ctx, exc);
    char *digest = arg_cstr(ctx, n, a, 0);
    uint8_t *pass, *salt;
    size_t pl, sl;
    tv_node_bytes_arg(n, a, 1, &pass, &pl);
    tv_node_bytes_arg(n, a, 2, &salt, &sl);
    size_t keylen = (size_t)arg_num(ctx, n, a, 4, 0);
    uint8_t *out = malloc(keylen ? keylen : 1);
    bool ok = digest && tv_crypto->pbkdf2(digest, pass, pl, salt, sl, (uint32_t)arg_num(ctx, n, a, 3, 1), out, keylen);
    free(digest);
    JSValueRef r = ok ? tv_js_bytes_copy(out, keylen) : JSValueMakeNull(ctx);
    free(out);
    return r;
}

/* scrypt(password, salt, N, r, p, maxmem, keylen) -> Uint8Array, or null if the parameters fail */
NATIVE(n_crypto_scrypt) {
    UNUSED;
    if (!tv_crypto) return tv_node_crypto_missing(ctx, exc);
    uint8_t *pass, *salt;
    size_t pl, sl;
    tv_node_bytes_arg(n, a, 0, &pass, &pl);
    tv_node_bytes_arg(n, a, 1, &salt, &sl);
    size_t keylen = (size_t)arg_num(ctx, n, a, 6, 0);
    uint8_t *out = malloc(keylen ? keylen : 1);
    bool ok = tv_crypto->scrypt(pass, pl, salt, sl, (uint64_t)arg_num(ctx, n, a, 2, 16384), (uint64_t)arg_num(ctx, n, a, 3, 8),
                                (uint64_t)arg_num(ctx, n, a, 4, 1), (size_t)arg_num(ctx, n, a, 5, 32 << 20), out, keylen);
    JSValueRef r = ok ? tv_js_bytes_copy(out, keylen) : JSValueMakeNull(ctx);
    free(out);
    return r;
}

/* hkdf(digest, key, salt, info, keylen) -> Uint8Array, or null */
NATIVE(n_crypto_hkdf) {
    UNUSED;
    if (!tv_crypto) return tv_node_crypto_missing(ctx, exc);
    char *digest = arg_cstr(ctx, n, a, 0);
    uint8_t *key, *salt, *info;
    size_t kl, sl, il;
    tv_node_bytes_arg(n, a, 1, &key, &kl);
    tv_node_bytes_arg(n, a, 2, &salt, &sl);
    tv_node_bytes_arg(n, a, 3, &info, &il);
    size_t keylen = (size_t)arg_num(ctx, n, a, 4, 0);
    uint8_t *out = malloc(keylen ? keylen : 1);
    bool ok = digest && tv_crypto->hkdf(digest, key, kl, salt, sl, info, il, out, keylen);
    free(digest);
    JSValueRef r = ok ? tv_js_bytes_copy(out, keylen) : JSValueMakeNull(ctx);
    free(out);
    return r;
}

/* timingSafeEqual(a, b): same-length byte views, compared in constant time */
NATIVE(n_crypto_equal) {
    UNUSED;
    uint8_t *x, *y;
    size_t xl, yl;
    if (!tv_node_bytes_arg(n, a, 0, &x, &xl) || !tv_node_bytes_arg(n, a, 1, &y, &yl) || xl != yl) return JSValueMakeBoolean(ctx, false);
    if (tv_crypto) return JSValueMakeBoolean(ctx, tv_crypto->equal(x, y, xl));
    unsigned char d = 0;
    for (size_t i = 0; i < xl; i++) d |= x[i] ^ y[i];
    return JSValueMakeBoolean(ctx, d == 0);
}

/* ------------------------------------------------------------------ work off the loop (libuv's
 * uv_queue_work): a job runs on a worker thread, then its callback is called from the event loop
 * with what done() makes of it. A pending job keeps the program running. */

#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>

typedef struct tv_node_job {
    struct tv_node_job *next;
    void (*work)(struct tv_node_job *job);                          /* on a worker thread */
    size_t (*done)(JSContextRef ctx, struct tv_node_job *job, JSValueRef *args);  /* on the loop: the callback's arguments (up to 4) */
    void (*free)(struct tv_node_job *job);
    JSObjectRef callback;
} tv_node_job;

static pthread_mutex_t tv_node_work_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tv_node_work_cv = PTHREAD_COND_INITIALIZER;
static tv_node_job *tv_node_jobs, *tv_node_jobs_tail, *tv_node_done;
static int tv_node_workers, tv_node_idle;
static int tv_node_wake[2] = {-1, -1};
static tv_io tv_node_work_io;

static void *tv_node_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&tv_node_work_mu);
    for (;;) {
        while (!tv_node_jobs) {
            tv_node_idle++;
            pthread_cond_wait(&tv_node_work_cv, &tv_node_work_mu);
            tv_node_idle--;
        }
        tv_node_job *job = tv_node_jobs;
        tv_node_jobs = job->next;
        if (!tv_node_jobs) tv_node_jobs_tail = NULL;
        pthread_mutex_unlock(&tv_node_work_mu);
        job->work(job);
        pthread_mutex_lock(&tv_node_work_mu);
        job->next = tv_node_done;
        tv_node_done = job;
        char one = 1;
        ssize_t w = write(tv_node_wake[1], &one, 1);
        (void)w;
    }
    return NULL;
}

static void tv_node_work_ready(tv_io *h, bool readable, bool writable, bool broken) {
    (void)h; (void)readable; (void)writable; (void)broken;
    char buf[64];
    while (read(tv_node_wake[0], buf, sizeof buf) > 0) {}
    pthread_mutex_lock(&tv_node_work_mu);
    tv_node_job *done = tv_node_done;
    tv_node_done = NULL;
    pthread_mutex_unlock(&tv_node_work_mu);
    /* (oldest first) */
    tv_node_job *ordered = NULL;
    while (done) {
        tv_node_job *next = done->next;
        done->next = ordered;
        ordered = done;
        done = next;
    }
    while (ordered) {
        tv_node_job *job = ordered;
        ordered = job->next;
        tv_io_refs--;
        JSValueRef args[4];
        size_t nargs = job->done(tv_js_ctx, job, args);
        JSObjectRef cb = job->callback;
        if (job->free) job->free(job);
        free(job);
        if (!cb) continue;
        JSValueRef exc = NULL;
        JSObjectCallAsFunction(tv_js_ctx, cb, NULL, nargs, args, &exc);
        JSValueUnprotect(tv_js_ctx, cb);
        if (exc) tv_node_report(exc);
    }
}

static void tv_node_queue(JSContextRef ctx, tv_node_job *job, JSValueRef callback) {
    if (tv_node_wake[0] < 0) {
        if (pipe(tv_node_wake) != 0) tv_trap("can't create a pipe for the work queue", NULL);
        for (int i = 0; i < 2; i++) {
            fcntl(tv_node_wake[i], F_SETFL, fcntl(tv_node_wake[i], F_GETFL) | O_NONBLOCK);
            fcntl(tv_node_wake[i], F_SETFD, FD_CLOEXEC);
        }
        tv_node_work_io.ready = tv_node_work_ready;
        tv_io_add(tv_node_wake[0], &tv_node_work_io, true, false);
    }
    job->callback = (JSObjectRef)callback;
    JSValueProtect(ctx, callback);
    tv_io_refs++;
    job->next = NULL;
    pthread_mutex_lock(&tv_node_work_mu);
    if (tv_node_jobs_tail) tv_node_jobs_tail->next = job;
    else tv_node_jobs = job;
    tv_node_jobs_tail = job;
    if (tv_node_idle == 0 && tv_node_workers < 4) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t t;
        if (pthread_create(&t, &attr, tv_node_worker, NULL) == 0) tv_node_workers++;
        pthread_attr_destroy(&attr);
    }
    pthread_cond_signal(&tv_node_work_cv);
    pthread_mutex_unlock(&tv_node_work_mu);
}

/* ------------------------------------------------------------------ dns (Node.js's cares_wrap.cc):
 * getaddrinfo and getnameinfo on the work queue, and DNS queries (the resolve* family) sent to a
 * server over UDP, then TCP if the answer was truncated; internal/bindings/cares_wrap.js builds
 * the queries and reads the answers. */

/* libuv's error for a getaddrinfo failure */
static int tv_node_eai(int r) {
    switch (r) {
#ifdef EAI_ADDRFAMILY
    case EAI_ADDRFAMILY: return -3000;
#endif
    case EAI_AGAIN: return -3001;
    case EAI_BADFLAGS: return -3002;
    case EAI_FAIL: return -3004;
    case EAI_FAMILY: return -3005;
    case EAI_MEMORY: return -3006;
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
    case EAI_NODATA: return -3007;
#endif
    case EAI_NONAME: return -3008;
#ifdef EAI_OVERFLOW
    case EAI_OVERFLOW: return -3009;
#endif
    case EAI_SERVICE: return -3010;
    case EAI_SOCKTYPE: return -3011;
#ifdef EAI_BADHINTS
    case EAI_BADHINTS: return -3013;
#endif
#ifdef EAI_PROTOCOL
    case EAI_PROTOCOL: return -3014;
#endif
    case EAI_SYSTEM: return -errno;
    default: return -3004;
    }
}

typedef struct {
    tv_node_job job;
    char *host;
    int family, flags, order;
    int err;
    struct addrinfo *res;
} tv_node_gai;

static void tv_node_gai_work(tv_node_job *j) {
    tv_node_gai *g = (tv_node_gai *)j;
    struct addrinfo hints = {0};
    hints.ai_family = g->family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = g->flags;
    int r = getaddrinfo(g->host, NULL, &hints, &g->res);
    g->err = r ? tv_node_eai(r) : 0;
}

static size_t tv_node_gai_done(JSContextRef ctx, tv_node_job *j, JSValueRef *args) {
    tv_node_gai *g = (tv_node_gai *)j;
    args[0] = num(ctx, g->err);
    if (g->err) {
        args[1] = undef(ctx);
        return 2;
    }
    JSValueRef items[256];
    size_t count = 0;
    /* order: 0 as the resolver gave them, 1 IPv4 first, 2 IPv6 first */
    int passes[2] = { AF_INET, AF_INET6 };
    if (g->order == 2) { passes[0] = AF_INET6; passes[1] = AF_INET; }
    for (int pass = 0; pass < (g->order ? 2 : 1); pass++) {
        for (struct addrinfo *ai = g->res; ai && count < 256; ai = ai->ai_next) {
            if (g->order && ai->ai_family != passes[pass]) continue;
            char ip[INET6_ADDRSTRLEN];
            const void *addr = ai->ai_family == AF_INET ? (const void *)&((struct sockaddr_in *)ai->ai_addr)->sin_addr
                             : ai->ai_family == AF_INET6 ? (const void *)&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr : NULL;
            if (!addr || !inet_ntop(ai->ai_family, addr, ip, sizeof ip)) continue;
            items[count++] = str(ctx, ip);
        }
    }
    args[1] = array(ctx, count, items);
    return 2;
}

static void tv_node_gai_free(tv_node_job *j) {
    tv_node_gai *g = (tv_node_gai *)j;
    if (g->res) freeaddrinfo(g->res);
    free(g->host);
}

/* getaddrinfo(hostname, family, flags, order, callback(err, addresses)) */
NATIVE(n_dns_getaddrinfo) {
    UNUSED;
    if (n < 5) return undef(ctx);
    tv_node_gai *g = calloc(1, sizeof *g);
    g->host = arg_cstr(ctx, n, a, 0);
    int family = (int)arg_num(ctx, n, a, 1, 0);
    g->family = family == 4 ? AF_INET : family == 6 ? AF_INET6 : AF_UNSPEC;
    g->flags = (int)arg_num(ctx, n, a, 2, 0);
    g->order = (int)arg_num(ctx, n, a, 3, 0);
    g->job.work = tv_node_gai_work;
    g->job.done = tv_node_gai_done;
    g->job.free = tv_node_gai_free;
    tv_node_queue(ctx, &g->job, a[4]);
    return num(ctx, 0);
}

typedef struct {
    tv_node_job job;
    struct sockaddr_storage addr;
    socklen_t len;
    int err;
    char host[NI_MAXHOST], service[NI_MAXSERV];
} tv_node_gni;

static void tv_node_gni_work(tv_node_job *j) {
    tv_node_gni *g = (tv_node_gni *)j;
    int r = getnameinfo((struct sockaddr *)&g->addr, g->len, g->host, sizeof g->host, g->service, sizeof g->service, NI_NAMEREQD);
    g->err = r ? tv_node_eai(r) : 0;
}

static size_t tv_node_gni_done(JSContextRef ctx, tv_node_job *j, JSValueRef *args) {
    tv_node_gni *g = (tv_node_gni *)j;
    args[0] = num(ctx, g->err);
    if (g->err) return 1;
    args[1] = str(ctx, g->host);
    args[2] = str(ctx, g->service);
    return 3;
}

/* getnameinfo(ip, port, callback(err, hostname, service)) -> 0, or UV_EINVAL for a bad ip */
NATIVE(n_dns_getnameinfo) {
    UNUSED;
    if (n < 3) return undef(ctx);
    char *ip = arg_cstr(ctx, n, a, 0);
    int port = (int)arg_num(ctx, n, a, 1, 0);
    tv_node_gni *g = calloc(1, sizeof *g);
    struct sockaddr_in *v4 = (struct sockaddr_in *)&g->addr;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&g->addr;
    if (ip && inet_pton(AF_INET, ip, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons((uint16_t)port);
        g->len = sizeof *v4;
    } else if (ip && inet_pton(AF_INET6, ip, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons((uint16_t)port);
        g->len = sizeof *v6;
    } else {
        free(ip);
        free(g);
        return num(ctx, -EINVAL);
    }
#ifdef __APPLE__
    ((struct sockaddr *)&g->addr)->sa_len = (uint8_t)g->len;
#endif
    free(ip);
    g->job.work = tv_node_gni_work;
    g->job.done = tv_node_gni_done;
    tv_node_queue(ctx, &g->job, a[2]);
    return num(ctx, 0);
}

/* canonicalizeIP(ip) -> its canonical form, or undefined if it isn't one */
NATIVE(n_dns_canonicalize) {
    UNUSED;
    char *ip = arg_cstr(ctx, n, a, 0);
    unsigned char buf[sizeof(struct in6_addr)];
    char out[INET6_ADDRSTRLEN];
    JSValueRef r = undef(ctx);
    if (ip && inet_pton(AF_INET, ip, buf) == 1 && inet_ntop(AF_INET, buf, out, sizeof out)) r = str(ctx, out);
    else if (ip && inet_pton(AF_INET6, ip, buf) == 1 && inet_ntop(AF_INET6, buf, out, sizeof out)) r = str(ctx, out);
    free(ip);
    return r;
}

/* ipv6Bytes(ip) -> 16 bytes, or undefined */
NATIVE(n_dns_ipv6_bytes) {
    UNUSED;
    char *ip = arg_cstr(ctx, n, a, 0);
    unsigned char buf[16];
    JSValueRef r = undef(ctx);
    if (ip && inet_pton(AF_INET6, ip, buf) == 1) r = tv_js_bytes_copy(buf, 16);
    free(ip);
    return r;
}

typedef struct {
    tv_node_job job;
    struct sockaddr_storage servers[8];
    socklen_t lens[8];
    int nservers;
    uint8_t *query;
    size_t query_len;
    int timeout_ms, tries;
    const char *err;
    uint8_t *answer;
    size_t answer_len;
} tv_node_dnsq;

/* (an error counts as ready, for the read or write to report: Linux raises a refused UDP query's
 * as POLLERR alone, where macOS makes the socket readable) */
static bool tv_node_dns_wait(int fd, short ev, int ms) {
    struct pollfd p = { fd, ev, 0 };
    return poll(&p, 1, ms) == 1 && (p.revents & (ev | POLLERR | POLLHUP));
}

/* one query over TCP (for an answer too big for UDP) */
static bool tv_node_dns_tcp(tv_node_dnsq *q, int i) {
    int fd = socket(q->servers[i].ss_family, SOCK_STREAM, 0);
    if (fd < 0) return false;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    bool ok = false;
    if (connect(fd, (struct sockaddr *)&q->servers[i], q->lens[i]) != 0 && errno != EINPROGRESS) goto out;
    if (!tv_node_dns_wait(fd, POLLOUT, q->timeout_ms)) goto out;
    uint8_t len[2] = { (uint8_t)(q->query_len >> 8), (uint8_t)q->query_len };
    if (write(fd, len, 2) != 2 || write(fd, q->query, q->query_len) != (ssize_t)q->query_len) goto out;
    size_t want = 0, got = 0;
    uint8_t head[2];
    size_t head_got = 0;
    while (head_got < 2) {
        if (!tv_node_dns_wait(fd, POLLIN, q->timeout_ms)) goto out;
        ssize_t r = read(fd, head + head_got, 2 - head_got);
        if (r <= 0) goto out;
        head_got += (size_t)r;
    }
    want = (size_t)head[0] << 8 | head[1];
    uint8_t *buf = malloc(want ? want : 1);
    while (got < want) {
        if (!tv_node_dns_wait(fd, POLLIN, q->timeout_ms)) { free(buf); goto out; }
        ssize_t r = read(fd, buf + got, want - got);
        if (r <= 0) { free(buf); goto out; }
        got += (size_t)r;
    }
    free(q->answer);
    q->answer = buf;
    q->answer_len = want;
    ok = true;
out:
    close(fd);
    return ok;
}

static void tv_node_dnsq_work(tv_node_job *j) {
    tv_node_dnsq *q = (tv_node_dnsq *)j;
    q->err = "ETIMEOUT";
    bool refused = false;
    for (int t = 0; t < q->tries; t++) {
        for (int i = 0; i < q->nservers; i++) {
            int fd = socket(q->servers[i].ss_family, SOCK_DGRAM, 0);
            if (fd < 0) continue;
            if (connect(fd, (struct sockaddr *)&q->servers[i], q->lens[i]) != 0 || send(fd, q->query, q->query_len, 0) < 0) {
                close(fd);
                continue;
            }
            uint8_t buf[4096];
            ssize_t r = -1;
            int left = q->timeout_ms << (t < 4 ? t : 4);
            while (tv_node_dns_wait(fd, POLLIN, left)) {
                r = recv(fd, buf, sizeof buf, 0);
                if (r < 0 && errno == ECONNREFUSED) { refused = true; break; }
                /* an answer to this query (its id) */
                if (r >= 12 && buf[0] == q->query[0] && buf[1] == q->query[1]) break;
                r = -1;
            }
            close(fd);
            if (r < 12) continue;
            q->answer = malloc((size_t)r);
            memcpy(q->answer, buf, (size_t)r);
            q->answer_len = (size_t)r;
            q->err = NULL;
            /* truncated: ask again over TCP */
            if (buf[2] & 0x02) tv_node_dns_tcp(q, i);
            return;
        }
    }
    if (refused) q->err = "ECONNREFUSED";
}

static size_t tv_node_dnsq_done(JSContextRef ctx, tv_node_job *j, JSValueRef *args) {
    tv_node_dnsq *q = (tv_node_dnsq *)j;
    if (q->err) {
        args[0] = str(ctx, q->err);
        return 1;
    }
    args[0] = JSValueMakeNull(ctx);
    args[1] = tv_js_bytes_copy(q->answer, q->answer_len);
    return 2;
}

static void tv_node_dnsq_free(tv_node_job *j) {
    tv_node_dnsq *q = (tv_node_dnsq *)j;
    free(q->query);
    free(q->answer);
}

/* query(servers [[ip, port], ...], packet, timeout ms, tries, callback(err code, answer bytes)) */
NATIVE(n_dns_query) {
    UNUSED;
    if (n < 5 || !JSValueIsObject(ctx, a[0])) return undef(ctx);
    tv_node_dnsq *q = calloc(1, sizeof *q);
    JSObjectRef list = (JSObjectRef)a[0];
    JSStringRef len_key = JSStringCreateWithUTF8CString("length");
    int count = (int)JSValueToNumber(ctx, JSObjectGetProperty(ctx, list, len_key, NULL), NULL);
    JSStringRelease(len_key);
    for (int i = 0; i < count && q->nservers < 8; i++) {
        JSValueRef pair = JSObjectGetPropertyAtIndex(ctx, list, (unsigned)i, NULL);
        if (!JSValueIsObject(ctx, pair)) continue;
        JSValueRef ipv = JSObjectGetPropertyAtIndex(ctx, (JSObjectRef)pair, 0, NULL);
        int port = (int)JSValueToNumber(ctx, JSObjectGetPropertyAtIndex(ctx, (JSObjectRef)pair, 1, NULL), NULL);
        char *ip = arg_cstr(ctx, 1, &ipv, 0);
        struct sockaddr_storage *ss = &q->servers[q->nservers];
        memset(ss, 0, sizeof *ss);
        struct sockaddr_in *v4 = (struct sockaddr_in *)ss;
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ss;
        if (ip && inet_pton(AF_INET, ip, &v4->sin_addr) == 1) {
            v4->sin_family = AF_INET;
            v4->sin_port = htons((uint16_t)(port ? port : 53));
            q->lens[q->nservers++] = sizeof *v4;
        } else if (ip && inet_pton(AF_INET6, ip, &v6->sin6_addr) == 1) {
            v6->sin6_family = AF_INET6;
            v6->sin6_port = htons((uint16_t)(port ? port : 53));
            q->lens[q->nservers++] = sizeof *v6;
        }
        free(ip);
    }
    uint8_t *p;
    size_t len;
    if (!tv_js_bytes_view(a[1], &p, &len)) {
        free(q);
        return undef(ctx);
    }
    q->query = malloc(len);
    memcpy(q->query, p, len);
    q->query_len = len;
    q->timeout_ms = (int)arg_num(ctx, n, a, 2, 2000);
    if (q->timeout_ms <= 0) q->timeout_ms = 2000;
    q->tries = (int)arg_num(ctx, n, a, 3, 4);
    if (q->tries <= 0) q->tries = 4;
    q->job.work = tv_node_dnsq_work;
    q->job.done = tv_node_dnsq_done;
    q->job.free = tv_node_dnsq_free;
    tv_node_queue(ctx, &q->job, a[4]);
    return num(ctx, 0);
}

/* ------------------------------------------------------------------ the engine's heap (v8's
 * getHeapStatistics) */

#include <dlfcn.h>

/* heapStats() -> [live bytes, capacity, extra (external) bytes], from JavaScriptCore's
 * JSGetMemoryUsageStatistics (which walks the heap: not for frequent calls) */
NATIVE(n_heap_stats) {
    UNUSED;
    static JSObjectRef (*stats)(JSContextRef);
    static bool looked;
    if (!looked) {
        looked = true;
#ifdef TV_JSC_OWN
        /* (Tov's own engine is linked in: a program exports no symbols to look up) */
        extern JSObjectRef JSGetMemoryUsageStatistics(JSContextRef);
        stats = JSGetMemoryUsageStatistics;
#else
        stats = (JSObjectRef (*)(JSContextRef))dlsym(RTLD_DEFAULT, "JSGetMemoryUsageStatistics");
#endif
    }
    double v[3] = {0, 0, 0};
    if (stats) {
        JSObjectRef o = stats(ctx);
        const char *keys[3] = { "heapSize", "heapCapacity", "extraMemorySize" };
        for (int i = 0; i < 3; i++) {
            JSStringRef k = JSStringCreateWithUTF8CString(keys[i]);
            v[i] = JSValueToNumber(ctx, JSObjectGetProperty(ctx, o, k, NULL), NULL);
            JSStringRelease(k);
        }
    }
    JSValueRef items[3] = { num(ctx, v[0]), num(ctx, v[1]), num(ctx, v[2]) };
    return array(ctx, 3, items);
}

/* ------------------------------------------------------------------ child processes (libuv's uv_spawn)
 *
 * tv_node_spawn starts a program as libuv does: found on the child's PATH, its stdio set up from
 * a spec per fd (a socketpair for a pipe, /dev/null, an inherited or given fd), in cwd, in a
 * new session if detached. spawnSync runs one to the end, feeding input and collecting output
 * with a timeout and a size limit, as Node.js's spawn_sync.cc does. */

#include <spawn.h>
#include <sys/wait.h>

enum { TV_STDIO_IGNORE, TV_STDIO_PIPE, TV_STDIO_INHERIT, TV_STDIO_FD };

typedef struct {
    int type;
    int fd;          /* inherit/fd: the parent's fd; pipe: the parent's end, once made */
} tv_node_stdio;

/* the program's path on PATH (from the child's environment, else ours), or NULL */
static char *tv_node_which(const char *file, char **env) {
    if (strchr(file, '/')) return strdup(file);
    const char *path = NULL;
    for (char **e = env ? env : environ; e && *e; e++)
        if (strncmp(*e, "PATH=", 5) == 0) path = *e + 5;
    if (!path) path = "/usr/bin:/bin";
    size_t fl = strlen(file);
    for (const char *p = path;; ) {
        const char *end = strchr(p, ':');
        size_t dl = end ? (size_t)(end - p) : strlen(p);
        char *cand = malloc(dl + fl + 2);
        if (dl == 0) memcpy(cand, ".", 1), dl = 1;
        else memcpy(cand, p, dl);
        cand[dl] = '/';
        memcpy(cand + dl + 1, file, fl + 1);
        struct stat st;
        if (access(cand, X_OK) == 0 && stat(cand, &st) == 0 && S_ISREG(st.st_mode)) return cand;
        free(cand);
        if (!end) break;
        p = end + 1;
    }
    return NULL;
}

/* starts file with argv/env (NULL: ours); stdio[i] pipes get the parent's end in .fd. -> pid, or
 * -errno */
static pid_t tv_node_spawn(const char *file, char **argv, char **env, const char *cwd, tv_node_stdio *stdio, int nstdio, bool detached) {
    char *path = tv_node_which(file, env);
    if (!path) return -ENOENT;
    int child_fds[64];
    int nfds = nstdio < 64 ? nstdio : 64;
    for (int i = 0; i < nfds; i++) child_fds[i] = -1;
    pid_t pid = 0;
    int err = 0;
    for (int i = 0; i < nfds && !err; i++) {
        if (stdio[i].type == TV_STDIO_PIPE) {
            int sv[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { err = errno; break; }
            fcntl(sv[0], F_SETFD, FD_CLOEXEC);
            fcntl(sv[1], F_SETFD, FD_CLOEXEC);
            stdio[i].fd = sv[0];
            child_fds[i] = sv[1];
        } else if (stdio[i].type == TV_STDIO_IGNORE) {
            child_fds[i] = open("/dev/null", (i == 0 ? O_RDONLY : O_RDWR) | O_CLOEXEC);
        } else {
            child_fds[i] = stdio[i].fd;
        }
    }
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
#ifdef POSIX_SPAWN_SETSID
    if (detached) flags |= POSIX_SPAWN_SETSID;
#endif
    posix_spawnattr_setflags(&attr, flags);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    if (!err) {
        for (int i = 0; i < nfds; i++) {
            if (child_fds[i] >= 0) posix_spawn_file_actions_adddup2(&fa, child_fds[i], i);
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (cwd) posix_spawn_file_actions_addchdir_np(&fa, cwd);
#pragma clang diagnostic pop
        /* (what the program wrote before comes before what the child writes to the same fds) */
        tv_out_flush();
        err = posix_spawn(&pid, path, &fa, &attr, argv, env ? env : environ);
    }
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    free(path);
    for (int i = 0; i < nfds; i++) {
        if (stdio[i].type == TV_STDIO_PIPE || stdio[i].type == TV_STDIO_IGNORE) {
            if (child_fds[i] >= 0) close(child_fds[i]);
        }
        if (err && stdio[i].type == TV_STDIO_PIPE && stdio[i].fd >= 0) {
            close(stdio[i].fd);
            stdio[i].fd = -1;
        }
    }
    /* (a missing cwd shows up as ENOENT, as libuv reports it) */
    return err ? -err : pid;
}

/* a JS array of strings as a NULL-terminated C array */
static char **tv_node_strings(JSContextRef ctx, JSValueRef v) {
    if (!v || !JSValueIsObject(ctx, v)) return NULL;
    JSObjectRef arr = (JSObjectRef)v;
    JSStringRef len_key = JSStringCreateWithUTF8CString("length");
    size_t len = (size_t)JSValueToNumber(ctx, JSObjectGetProperty(ctx, arr, len_key, NULL), NULL);
    JSStringRelease(len_key);
    char **out = calloc(len + 1, sizeof *out);
    for (size_t i = 0; i < len; i++) {
        JSValueRef e = JSObjectGetPropertyAtIndex(ctx, arr, (unsigned)i, NULL);
        out[i] = arg_cstr(ctx, 1, &e, 0);
        if (!out[i]) out[i] = strdup("");
    }
    return out;
}

static void tv_node_strings_free(char **s) {
    if (!s) return;
    for (char **p = s; *p; p++) free(*p);
    free(s);
}

/* stdio specs from [[type, fd], ...] */
static int tv_node_stdio_specs(JSContextRef ctx, JSValueRef v, tv_node_stdio *out, int max) {
    if (!JSValueIsObject(ctx, v)) return 0;
    JSObjectRef arr = (JSObjectRef)v;
    int n = 0;
    for (; n < max; n++) {
        JSValueRef e = JSObjectGetPropertyAtIndex(ctx, arr, (unsigned)n, NULL);
        if (!JSValueIsObject(ctx, e)) break;
        out[n].type = (int)JSValueToNumber(ctx, JSObjectGetPropertyAtIndex(ctx, (JSObjectRef)e, 0, NULL), NULL);
        out[n].fd = (int)JSValueToNumber(ctx, JSObjectGetPropertyAtIndex(ctx, (JSObjectRef)e, 1, NULL), NULL);
    }
    return n;
}

static const char *tv_node_signal_name(int sig) {
    for (size_t i = 0; i < sizeof tv_signals / sizeof *tv_signals; i++)
        if (tv_signals[i].sig == sig) return tv_signals[i].name;
    return NULL;
}

/* ------------------------------------------------------------------ signals (Node.js's signal_wrap):
 * signal(name, on) starts or stops delivering a signal to the handler signalHandler(fn) set, as
 * fn(name, number). The signal handler writes the signal's number to a pipe the loop watches, so
 * fn runs on the loop; as in Node.js, a watched signal doesn't keep the program running. */
static int tv_node_sig_pipe[2] = {-1, -1};
static tv_io tv_node_sig_io;
static JSObjectRef tv_node_sig_fn;
static volatile sig_atomic_t tv_node_sig_on[NSIG];
static volatile sig_atomic_t tv_node_sig_default[NSIG];
static volatile sig_atomic_t tv_node_sig_terminate[NSIG];
static pthread_t tv_node_sig_thread;
static struct sigaction tv_node_sig_default_before[NSIG];
static struct sigaction tv_node_sig_before[NSIG];   /* what each watched signal did before */

static void tv_node_sig_caught(int sig) {
    int saved = errno;
    /* Default termination cannot wait for a busy JS loop or a full self-pipe.
       Capture it at delivery and route it to the runtime thread, whose registry
       updates block INT/TERM. tcsetattr, sigaction and raise are signal-safe. */
    if (!tv_node_sig_on[sig] && tv_node_sig_default[sig]) tv_node_sig_terminate[sig] = 1;
    if (tv_node_sig_terminate[sig]) {
        if (!pthread_equal(pthread_self(), tv_node_sig_thread)) {
            pthread_kill(tv_node_sig_thread, sig);
        } else {
            tv_node_reset_write();
            tv_node_tty_reset();
            struct sigaction sa = {0};
            sa.sa_handler = SIG_DFL;
            sigemptyset(&sa.sa_mask);
            sigaction(sig, &sa, NULL);
            raise(sig);
        }
        errno = saved;
        return;
    }
    unsigned char b = (unsigned char)sig;
    (void)!write(tv_node_sig_pipe[1], &b, 1);
    errno = saved;
}

static struct sigaction tv_node_signal_action(void) {
    struct sigaction sa = {0};
    sa.sa_handler = tv_node_sig_caught;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGTERM);
    return sa;
}

static void tv_node_sig_ready(tv_io *h, bool readable, bool writable, bool broken) {
    (void)h; (void)readable; (void)writable; (void)broken;
    unsigned char buf[64];
    ssize_t got;
    while ((got = read(tv_node_sig_pipe[0], buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < got; i++) {
            int sig = buf[i];
            if (!tv_node_sig_on[sig] || !tv_node_sig_fn) continue;
            const char *name = tv_node_signal_name(sig);
            JSValueRef args[2] = { name ? str(tv_js_ctx, name) : num(tv_js_ctx, sig), num(tv_js_ctx, sig) };
            JSValueRef exc = NULL;
            JSObjectCallAsFunction(tv_js_ctx, tv_node_sig_fn, NULL, 2, args, &exc);
            if (exc) tv_node_report(exc);
        }
    }
}

static int tv_node_signal_setup(void) {
    if (tv_node_sig_pipe[0] >= 0) return 0;
    if (pipe(tv_node_sig_pipe) != 0) return -errno;
    for (int i = 0; i < 2; i++) {
        fcntl(tv_node_sig_pipe[i], F_SETFL, fcntl(tv_node_sig_pipe[i], F_GETFL) | O_NONBLOCK);
        fcntl(tv_node_sig_pipe[i], F_SETFD, FD_CLOEXEC);
    }
    tv_node_sig_io.ready = tv_node_sig_ready;
    tv_io_add(tv_node_sig_pipe[0], &tv_node_sig_io, true, false);
    return 0;
}

static int tv_node_signal_defaults(void) {
    if (!tv_node_tty_modes) return 0;
    int err = tv_node_signal_setup();
    if (err) return err;
    const int signals[] = { SIGINT, SIGTERM };
    for (size_t i = 0; i < sizeof signals / sizeof *signals; i++) {
        int sig = signals[i];
        if (tv_node_sig_default[sig]) continue;
        if (tv_node_sig_on[sig]) {
            if (tv_node_sig_before[sig].sa_handler == SIG_DFL) {
                tv_node_sig_default_before[sig] = tv_node_sig_before[sig];
                tv_node_sig_before[sig] = tv_node_signal_action();
                tv_node_sig_default[sig] = true;
            }
            continue;
        }
        struct sigaction before;
        if (sigaction(sig, NULL, &before) != 0) return -errno;
        /* Keep inherited ignored/custom dispositions and explicit listeners. */
        if (before.sa_handler != SIG_DFL) continue;
        struct sigaction sa = tv_node_signal_action();
        if (sigaction(sig, &sa, NULL) != 0) return -errno;
        tv_node_sig_default_before[sig] = before;
        tv_node_sig_default[sig] = true;
    }
    return 0;
}

static void tv_node_signal_defaults_stop(void) {
    const int signals[] = { SIGINT, SIGTERM };
    for (size_t i = 0; i < sizeof signals / sizeof *signals; i++) {
        int sig = signals[i];
        if (!tv_node_sig_default[sig]) continue;
        if (tv_node_sig_on[sig]) {
            tv_node_sig_before[sig] = tv_node_sig_default_before[sig];
        } else {
            struct sigaction current;
            if (sigaction(sig, NULL, &current) == 0 && current.sa_handler == tv_node_sig_caught) {
                sigaction(sig, &tv_node_sig_default_before[sig], NULL);
            }
        }
        tv_node_sig_default[sig] = false;
    }
}

/* signalHandler(fn) */
NATIVE(n_signal_handler) {
    UNUSED;
    if (tv_node_sig_fn) JSValueUnprotect(ctx, tv_node_sig_fn);
    tv_node_sig_fn = n > 0 && JSValueIsObject(ctx, a[0]) ? (JSObjectRef)a[0] : NULL;
    if (tv_node_sig_fn) JSValueProtect(ctx, tv_node_sig_fn);
    return undef(ctx);
}

/* signal(name, on) -> 0, or -errno (EINVAL: not a signal, or one that can't be caught) */
NATIVE(n_signal) {
    UNUSED;
    char *name = arg_cstr(ctx, n, a, 0);
    bool on = n > 1 && JSValueToBoolean(ctx, a[1]);
    int sig = -1;
    for (size_t i = 0; i < sizeof tv_signals / sizeof *tv_signals; i++)
        if (strcmp(tv_signals[i].name, name) == 0) sig = tv_signals[i].sig;
    free(name);
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP) return num(ctx, -EINVAL);
    if (on == tv_node_sig_on[sig]) return num(ctx, 0);
    sigset_t before;
    tv_node_tty_block_signals(&before);
    if (on) {
        int err = tv_node_signal_setup();
        if (err) { sigprocmask(SIG_SETMASK, &before, NULL); return num(ctx, err); }
        struct sigaction sa = tv_node_signal_action();
        if (sigaction(sig, &sa, &tv_node_sig_before[sig]) != 0) {
            int err = errno;
            sigprocmask(SIG_SETMASK, &before, NULL);
            return num(ctx, -err);
        }
    } else {
        struct sigaction current;
        if (sigaction(sig, NULL, &current) == 0 && current.sa_handler == tv_node_sig_caught)
            sigaction(sig, &tv_node_sig_before[sig], NULL);
    }
    tv_node_sig_on[sig] = on;
    sigprocmask(SIG_SETMASK, &before, NULL);
    return num(ctx, 0);
}

/* spawnSync(file, args, env|null, cwd|null, stdio [[type, fd]], inputs [bytes|null], timeout ms,
 * maxBuffer, killSignal, detached) -> [pid, status|null, signal name|null, errno (0: none),
 * [output per fd: bytes|null]] */
NATIVE(n_spawn_sync) {
    UNUSED;
    if (n < 10) return undef(ctx);
    char *file = arg_cstr(ctx, n, a, 0);
    char **argv = tv_node_strings(ctx, a[1]);
    char **env = JSValueIsNull(ctx, a[2]) ? NULL : tv_node_strings(ctx, a[2]);
    char *cwd = JSValueIsNull(ctx, a[3]) || JSValueIsUndefined(ctx, a[3]) ? NULL : arg_cstr(ctx, n, a, 3);
    tv_node_stdio stdio[16];
    int nstdio = tv_node_stdio_specs(ctx, a[4], stdio, 16);
    double timeout = arg_num(ctx, n, a, 6, 0);
    double max_buffer = arg_num(ctx, n, a, 7, 1024 * 1024);
    int kill_signal = (int)arg_num(ctx, n, a, 8, SIGTERM);
    bool detached = JSValueToBoolean(ctx, a[9]);
    uint8_t *in[16] = {0};
    size_t in_len[16] = {0}, in_off[16] = {0};
    for (int i = 0; i < nstdio; i++) {
        JSValueRef v = JSObjectGetPropertyAtIndex(ctx, (JSObjectRef)a[5], (unsigned)i, NULL);
        if (v && JSValueIsObject(ctx, v)) tv_js_bytes_view(v, &in[i], &in_len[i]);
    }
    if (!argv) argv = calloc(2, sizeof *argv), argv[0] = strdup(file ? file : "");
    pid_t pid = file ? tv_node_spawn(file, argv, env, cwd, stdio, nstdio, detached) : -EINVAL;
    tv_sb out[16];
    memset(out, 0, sizeof out);
    int err = 0;
    JSValueRef status = JSValueMakeNull(ctx), signal = JSValueMakeNull(ctx);
    if (pid < 0) {
        err = (int)-pid;
        pid = 0;
    } else {
        for (int i = 0; i < nstdio; i++) {
            if (stdio[i].type != TV_STDIO_PIPE) continue;
            fcntl(stdio[i].fd, F_SETFL, fcntl(stdio[i].fd, F_GETFL) | O_NONBLOCK);
            /* (stdin with nothing to write is closed now: the child sees its end) */
            if (i == 0 && !in[0]) shutdown(stdio[i].fd, SHUT_WR);
        }
        double deadline = timeout > 0 ? tv_performance_now() + timeout : 0;
        double total = 0;
        bool killed = false;
        int child_status = 0;
        bool exited = false;
        for (;;) {
            struct pollfd pf[16];
            int map[16], np = 0;
            for (int i = 0; i < nstdio; i++) {
                if (stdio[i].type != TV_STDIO_PIPE || stdio[i].fd < 0) continue;
                short ev = 0;
                if (i == 0 && in[0] && in_off[0] < in_len[0]) ev |= POLLOUT;
                if (i != 0) ev |= POLLIN;
                if (!ev) continue;
                pf[np].fd = stdio[i].fd;
                pf[np].events = ev;
                pf[np].revents = 0;
                map[np++] = i;
            }
            if (np == 0) break;
            int wait_ms = -1;
            if (deadline) {
                double left = deadline - tv_performance_now();
                if (left <= 0) {
                    kill(pid, kill_signal);
                    killed = true;
                    err = ETIMEDOUT;
                    break;
                }
                wait_ms = (int)left + 1;
            }
            int r = poll(pf, (nfds_t)np, wait_ms);
            if (r < 0 && errno == EINTR) continue;
            for (int k = 0; k < np; k++) {
                int i = map[k];
                if (pf[k].revents & POLLOUT) {
                    ssize_t w = write(stdio[i].fd, in[i] + in_off[i], in_len[i] - in_off[i]);
                    if (w > 0) in_off[i] += (size_t)w;
                    if (w < 0 && errno != EAGAIN) in_off[i] = in_len[i];
                    if (in_off[i] >= in_len[i]) shutdown(stdio[i].fd, SHUT_WR);
                } else if (pf[k].revents & (POLLIN | POLLHUP | POLLERR)) {
                    char buf[65536];
                    ssize_t got = read(stdio[i].fd, buf, sizeof buf);
                    if (got > 0) {
                        tv_sb_add(&out[i], buf, (size_t)got);
                        total += (double)got;
                        if (max_buffer > 0 && total > max_buffer) {
                            kill(pid, kill_signal);
                            killed = true;
                            err = ENOBUFS;
                        }
                    } else if (got == 0 || errno != EAGAIN) {
                        close(stdio[i].fd);
                        stdio[i].fd = -1;
                    }
                }
            }
            if (killed) break;
        }
        (void)exited;
        while (waitpid(pid, &child_status, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED(child_status)) {
            status = num(ctx, WEXITSTATUS(child_status));
        } else if (WIFSIGNALED(child_status)) {
            const char *name = tv_node_signal_name(WTERMSIG(child_status));
            signal = name ? str(ctx, name) : num(ctx, WTERMSIG(child_status));
        }
    }
    JSValueRef outputs[16];
    for (int i = 0; i < nstdio; i++) {
        if (stdio[i].type == TV_STDIO_PIPE) {
            if (stdio[i].fd >= 0) close(stdio[i].fd);
            outputs[i] = i == 0 || pid == 0 ? JSValueMakeNull(ctx) : (JSValueRef)tv_js_bytes_copy(out[i].data ? out[i].data : "", out[i].len);
        } else {
            outputs[i] = JSValueMakeNull(ctx);
        }
        tv_sb_free(&out[i]);
    }
    JSValueRef items[5] = { num(ctx, pid), status, signal, num(ctx, err), pid == 0 ? JSValueMakeNull(ctx) : (JSValueRef)array(ctx, (size_t)nstdio, outputs) };
    free(file);
    free(cwd);
    tv_node_strings_free(argv);
    tv_node_strings_free(env);
    return array(ctx, 5, items);
}

/* ------------------------------------------------------------------ streams (libuv's uv_stream_t
 * over a pipe or socket fd): reads go to onread(bytes) as they arrive (onread(null, errno) at the
 * end: UV_EOF); writes go out now when the fd takes them, else in order as it does; shutdown
 * follows the writes. A handle stays alive until JS releases it, a loop turn after close, so no
 * event already collected can reach freed memory. */

#define TV_UV_EOF (-4095)

typedef struct tv_node_wreq {
    struct tv_node_wreq *next;
    uint8_t *data;
    size_t len, off;
    JSObjectRef cb;      /* cb(status) once written (protected) */
} tv_node_wreq;

typedef struct {
    tv_io io;
    int fd;
    bool reading, closed, refed, counted, watching, listening, connecting;
    bool tty;
    tv_node_tty_mode *raw_mode;
    bool file;         /* regular file or non-tty device: not poll-watchable, read on the work queue */
    int pending_file;  /* file-read jobs in flight (the struct outlives JS while they're pending) */
    bool zombie;       /* JS released the handle mid-read: free(s) when the last read lands */
    JSObjectRef self, onread;
    JSObjectRef onconnection, connect_cb;   /* listen: onconnection(fd | null, err); connect: cb(status) */
    tv_node_wreq *wq, *wq_tail;
    JSObjectRef shutdown_cb;
    double bytes_read, bytes_written;
} tv_node_stream;

/* a chunk read of a file fd on a worker thread (regular files aren't poll-watchable) */
typedef struct {
    tv_node_job job;
    tv_node_stream *s;
    int fd;
    uint8_t buf[65536];
    ssize_t n;
    int err;
} tv_node_fread;

static void tv_node_fread_work(tv_node_job *j) {
    tv_node_fread *r = (tv_node_fread *)j;
    for (;;) {
        ssize_t n = read(r->fd, r->buf, sizeof r->buf);
        if (n < 0 && errno == EINTR) continue;
        if (n > 0) r->n = n;
        else r->err = n == 0 ? TV_UV_EOF : -errno;
        return;
    }
}

static void tv_node_file_read(JSContextRef ctx, tv_node_stream *s);
static void tv_node_stream_update(tv_node_stream *s);

/* a file job landed after the stream stopped, closed or was released: drop the callback
   (the work loop skips a NULL one) and finish the close/free that waited on it */
static size_t tv_node_file_landed(JSContextRef ctx, tv_node_job *j, tv_node_stream *s) {
    JSValueUnprotect(ctx, j->callback);
    j->callback = NULL;
    if ((s->closed || s->zombie) && s->fd >= 0) { close(s->fd); s->fd = -1; }
    if (s->zombie && !s->pending_file) free(s);
    return 0;
}

static size_t tv_node_fread_done(JSContextRef ctx, tv_node_job *j, JSValueRef *args) {
    tv_node_fread *r = (tv_node_fread *)j;
    tv_node_stream *s = r->s;
    s->pending_file--;
    if (!s->closed && !s->zombie && s->reading) {
        if (r->n > 0) {
            s->bytes_read += (double)r->n;
            args[0] = tv_js_bytes_copy(r->buf, (size_t)r->n);
            tv_node_file_read(ctx, s);   /* (the next chunk, while the stream wants more) */
            return 1;
        }
        s->reading = false;
        tv_node_stream_update(s);
        args[0] = JSValueMakeNull(ctx);
        args[1] = JSValueMakeNumber(ctx, r->err);
        return 2;
    }
    return tv_node_file_landed(ctx, j, s);
}

/* shutdown on a file fd also goes through the work queue: no writable event is coming */
typedef struct {
    tv_node_job job;
    tv_node_stream *s;
    int fd;
    int err;
} tv_node_fshutdown;

static void tv_node_fshutdown_work(tv_node_job *j) {
    tv_node_fshutdown *r = (tv_node_fshutdown *)j;
    r->err = shutdown(r->fd, SHUT_WR) != 0 && errno != ENOTSOCK ? -errno : 0;
}

static size_t tv_node_fshutdown_done(JSContextRef ctx, tv_node_job *j, JSValueRef *args) {
    tv_node_fshutdown *r = (tv_node_fshutdown *)j;
    tv_node_stream *s = r->s;
    s->pending_file--;
    if (s->closed || s->zombie) return tv_node_file_landed(ctx, j, s);
    args[0] = JSValueMakeNumber(ctx, r->err);
    return 1;
}

static void tv_node_file_read(JSContextRef ctx, tv_node_stream *s) {
    tv_node_fread *r = calloc(1, sizeof *r);
    r->job.work = tv_node_fread_work;
    r->job.done = tv_node_fread_done;
    r->s = s;
    r->fd = s->fd;
    s->pending_file++;
    tv_node_queue(ctx, &r->job, s->onread);
}

static void tv_node_stream_finalize(JSObjectRef o) {
    tv_node_stream *s = JSObjectGetPrivate(o);
    if (!s) return;
    if (s->raw_mode) { tv_node_tty_release(s->raw_mode, true); s->raw_mode = NULL; }
    if (s->pending_file) { s->zombie = true; return; }   /* freed in tv_node_file_landed */
    if (s->fd >= 0) close(s->fd);
    free(s);
}

static JSClassRef tv_node_stream_class(void) {
    static JSClassRef cls;
    if (!cls) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = "StreamHandle";
        def.finalize = tv_node_stream_finalize;
        cls = JSClassCreate(&def);
    }
    return cls;
}

static tv_node_stream *tv_node_stream_of(JSContextRef ctx, size_t n, const JSValueRef a[]) {
    if (n < 1 || !JSValueIsObjectOfClass(ctx, a[0], tv_node_stream_class())) return NULL;
    return JSObjectGetPrivate((JSObjectRef)a[0]);
}

static void tv_node_stream_ready(tv_io *h, bool readable, bool writable, bool broken);

/* what the loop watches for, and whether the stream keeps the program running */
static void tv_node_stream_update(tv_node_stream *s) {
    bool want_read = (s->reading || s->listening) && !s->closed && !s->file;
    bool want_write = !s->closed && (s->wq || s->shutdown_cb || s->connecting);
    if (s->fd >= 0 && !s->closed && (want_read || want_write || s->watching)) {
        s->io.ready = tv_node_stream_ready;
        tv_io_set(s->fd, &s->io, want_read, want_write);
        s->watching = want_read || want_write;
    }
    bool active = s->refed && (want_read || want_write);
    if (active != s->counted) {
        tv_io_refs += active ? 1 : -1;
        s->counted = active;
    }
}

static void tv_node_call_args(JSObjectRef fn, size_t n, const JSValueRef *args) {
    JSValueRef exc = NULL;
    JSObjectCallAsFunction(tv_js_ctx, fn, NULL, n, args, &exc);
    if (exc) tv_node_report(exc);
}

static void tv_node_stream_complete(tv_node_wreq *w, int status) {
    JSObjectRef cb = w->cb;
    free(w->data);
    free(w);
    if (cb) {
        JSValueRef arg = JSValueMakeNumber(tv_js_ctx, status);
        tv_node_call_args(cb, 1, &arg);
        JSValueUnprotect(tv_js_ctx, cb);
    }
}

/* writes what the fd takes from the queue; finishes the shutdown once it's empty */
static void tv_node_stream_flush(tv_node_stream *s) {
    while (s->wq && !s->closed) {
        tv_node_wreq *w = s->wq;
        ssize_t r = write(s->fd, w->data + w->off, w->len - w->off);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == EAGAIN) break;
        if (r < 0) {
            int err = errno;
            s->wq = w->next;
            if (!s->wq) s->wq_tail = NULL;
            tv_node_stream_complete(w, -err);
            continue;
        }
        w->off += (size_t)r;
        s->bytes_written += (double)r;
        if (w->off < w->len) break;
        s->wq = w->next;
        if (!s->wq) s->wq_tail = NULL;
        tv_node_stream_complete(w, 0);
    }
    if (!s->wq && s->shutdown_cb && !s->closed) {
        JSObjectRef cb = s->shutdown_cb;
        s->shutdown_cb = NULL;
        int st = shutdown(s->fd, SHUT_WR) == 0 || errno == ENOTSOCK ? 0 : -errno;
        JSValueRef arg = JSValueMakeNumber(tv_js_ctx, st);
        tv_node_call_args(cb, 1, &arg);
        JSValueUnprotect(tv_js_ctx, cb);
    }
}

static void tv_node_stream_ready(tv_io *h, bool readable, bool writable, bool broken) {
    tv_node_stream *s = (tv_node_stream *)h;
    if (s->closed) return;
    if (s->connecting && (writable || broken)) {
        int err = 0;
        socklen_t len = sizeof err;
        if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) err = errno;
        s->connecting = false;
        JSObjectRef cb = s->connect_cb;
        s->connect_cb = NULL;
        tv_node_stream_update(s);
        JSValueRef arg = JSValueMakeNumber(tv_js_ctx, -err);
        tv_node_call_args(cb, 1, &arg);
        JSValueUnprotect(tv_js_ctx, cb);
        return;
    }
    if (s->listening && (readable || broken)) {
        for (int i = 0; i < 64 && s->listening && !s->closed; i++) {
            int c = accept(s->fd, NULL, NULL);
            if (c < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != ECONNABORTED) {
                    JSValueRef args[2] = { JSValueMakeNull(tv_js_ctx), JSValueMakeNumber(tv_js_ctx, -errno) };
                    tv_node_call_args(s->onconnection, 2, args);
                }
                break;
            }
            fcntl(c, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            JSValueRef args[2] = { JSValueMakeNumber(tv_js_ctx, c), JSValueMakeNumber(tv_js_ctx, 0) };
            tv_node_call_args(s->onconnection, 2, args);
        }
        if (!s->closed) tv_node_stream_update(s);
        return;
    }
    if (writable || (broken && s->wq)) tv_node_stream_flush(s);
    if ((readable || broken) && s->reading && !s->closed) {
        static uint8_t buf[65536];
        ssize_t r = read(s->fd, buf, sizeof buf);
        if (r > 0) {
            s->bytes_read += (double)r;
            JSValueRef arg = tv_js_bytes_copy(buf, (size_t)r);
            tv_node_call_args(s->onread, 1, &arg);
        } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
            int err = r == 0 || (s->tty && errno == EIO) ? TV_UV_EOF : -errno;
            s->reading = false;
            JSValueRef args[2] = { JSValueMakeNull(tv_js_ctx), JSValueMakeNumber(tv_js_ctx, err) };
            tv_node_call_args(s->onread, 2, args);
        }
    }
    if (!s->closed) tv_node_stream_update(s);
}

/* open(fd) -> handle (the fd becomes non-blocking) */
NATIVE(n_stream_open) {
    UNUSED;
    static bool sigpipe;
    if (!sigpipe) {
        sigpipe = true;
        signal(SIGPIPE, SIG_IGN);
    }
    int fd = (int)arg_num(ctx, n, a, 0, -1);
    if (fd < 0) return undef(ctx);
    /* Terminal fds can share an open file description with their parent or
       stdio. Reopen so nonblocking reads do not mutate those shared flags.
       Never reopen a PTY master: that would allocate a different terminal. */
    if (isatty(fd) && !tv_node_tty_master(fd)) {
        const char *path = ttyname(fd);
        if (path && strcmp(path, "/dev/ptmx") && strcmp(path, "/dev/pts/ptmx")) {
            int flags = fcntl(fd, F_GETFL);
            if (flags < 0) return undef(ctx);
            int copy = open(path, (flags & O_ACCMODE) | O_NOCTTY | O_CLOEXEC);
            if (copy < 0) return undef(ctx);
            fd = copy;
        }
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    tv_node_stream *s = calloc(1, sizeof *s);
    s->fd = fd;
    s->tty = isatty(fd);
    /* Regular files (and plain devices like /dev/null) can't be watched for
       readability — reads go to the work queue, as libuv's fs reads do.
       TTYs stay watched. */
    struct stat st;
    if (fstat(fd, &st) == 0 && (S_ISREG(st.st_mode) || (S_ISCHR(st.st_mode) && !isatty(fd)))) s->file = true;
    s->refed = true;
    JSObjectRef o = JSObjectMake(ctx, tv_node_stream_class(), s);
    s->self = o;
    JSValueProtect(ctx, o);
    return o;
}

NATIVE(n_stream_set_raw_mode) {
    UNUSED;
    tv_node_stream *s = tv_node_stream_of(ctx, n, a);
    if (!s || s->closed) return num(ctx, -EBADF);
    bool raw = n > 1 && JSValueToBoolean(ctx, a[1]);
    if (raw == (s->raw_mode != NULL)) return num(ctx, 0);
    if (!raw) {
        int err = tv_node_tty_release(s->raw_mode, false);
        if (!err) s->raw_mode = NULL;
        return num(ctx, err);
    }
    struct stat st;
    if (fstat(s->fd, &st) != 0) return num(ctx, -errno);
    int pty_number = -1;
    char slave_path[128] = {0};
#ifdef TIOCGPTN
    unsigned int index;
    if (ioctl(s->fd, TIOCGPTN, &index) == 0) {
        /* Match the slave's identity where available, and otherwise retain
           the master's index rather than merging unrelated PTYs. */
        snprintf(slave_path, sizeof slave_path, "/dev/pts/%u", index);
        struct stat slave;
        if (stat(slave_path, &slave) == 0) st = slave;
        else { pty_number = (int)index; slave_path[0] = 0; }
    }
#endif
#ifdef TIOCPTYGNAME
    struct stat slave;
    if (ioctl(s->fd, TIOCPTYGNAME, slave_path) == 0 && stat(slave_path, &slave) == 0) st = slave;
    else slave_path[0] = 0;
#endif
    for (tv_node_tty_mode *m = tv_node_tty_modes; m; m = m->next) {
        if (m->dev == st.st_dev && m->rdev == st.st_rdev && m->ino == st.st_ino && m->pty_number == pty_number) {
            m->owners++;
            s->raw_mode = m;
            return num(ctx, 0);
        }
    }
    tv_node_tty_mode *m = calloc(1, sizeof *m);
    /* The slave path is only an identity key. Holding a slave here would
       suppress master EOF when the child closes its terminal. */
    m->fd = fcntl(s->fd, F_DUPFD_CLOEXEC, 0);
    if (m->fd < 0) { int err = errno; free(m); return num(ctx, -err); }
    struct termios t;
    if (tcgetattr(m->fd, &t) != 0) {
        int err = errno; close(m->fd); free(m); return num(ctx, -err);
    }
    m->saved = t;
    m->dev = st.st_dev; m->rdev = st.st_rdev; m->ino = st.st_ino;
    m->pty_number = pty_number;
    m->owners = 1;
    t.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag |= ONLCR;
    t.c_cflag |= CS8;
    t.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    /* Publish before enabling raw mode so default termination can always
       restore the original snapshot. Block delivery across this transition. */
    sigset_t before;
    tv_node_tty_block_signals(&before);
    m->next = tv_node_tty_modes;
    tv_node_tty_modes = m;
    int signal_err = tv_node_signal_defaults();
    int err = signal_err ? signal_err : tcsetattr(m->fd, TCSADRAIN, &t) == 0 ? 0 : -errno;
    if (err) {
        tv_node_tty_modes = m->next;
        if (!tv_node_tty_modes) tv_node_signal_defaults_stop();
        close(m->fd);
        free(m);
    } else {
        static bool registered;
        if (!registered) { atexit(tv_node_tty_reset); registered = true; }
        s->raw_mode = m;
    }
    sigprocmask(SIG_SETMASK, &before, NULL);
    if (err) return num(ctx, err);
    return num(ctx, 0);
}

/* readStart(handle, onread) */
NATIVE(n_stream_read_start) {
    UNUSED;
    tv_node_stream *s = tv_node_stream_of(ctx, n, a);
    if (!s || s->closed || n < 2) return num(ctx, -EINVAL);
    if (s->onread) JSValueUnprotect(ctx, s->onread);
    s->onread = (JSObjectRef)a[1];
    JSValueProtect(ctx, a[1]);
    s->reading = true;
    tv_node_stream_update(s);
    if (s->file) tv_node_file_read(ctx, s);
    return num(ctx, 0);
}

NATIVE(n_stream_read_stop) {
    UNUSED;
    tv_node_stream *s = tv_node_stream_of(ctx, n, a);
    if (!s || s->closed) return num(ctx, 0);
    s->reading = false;
    tv_node_stream_update(s);
    return num(ctx, 0);
}

/* write(handle, [bytes...], cb) -> [err, bytes written now, async]: cb(status) only if async.
 * The chunks go out in one writev; what the fd doesn't take waits in the queue. */
#include <sys/uio.h>

NATIVE(n_stream_write) {
    UNUSED;
    tv_node_stream *s = tv_node_stream_of(ctx, n, a);
    if (!s || n < 3 || !JSValueIsObject(ctx, a[1])) return undef(ctx);
    if (s->closed) {
        JSValueRef items[3] = { num(ctx, -EBADF), num(ctx, 0), JSValueMakeBoolean(ctx, false) };
        return array(ctx, 3, items);
    }
    JSObjectRef list = (JSObjectRef)a[1];
    JSStringRef len_key = JSStringCreateWithUTF8CString("length");
    unsigned count = (unsigned)JSValueToNumber(ctx, JSObjectGetProperty(ctx, list, len_key, NULL), NULL);
    JSStringRelease(len_key);
    struct iovec iov[64];
    unsigned niov = 0;
    size_t total = 0;
    for (unsigned i = 0; i < count && niov < 64; i++) {
        uint8_t *p;
        size_t len;
        JSValueRef v = JSObjectGetPropertyAtIndex(ctx, list, i, NULL);
        if (!tv_js_bytes_view(v, &p, &len) || len == 0) continue;
        iov[niov].iov_base = p;
        iov[niov].iov_len = len;
        niov++;
        total += len;
    }
    /* (more than 64 chunks: the rest are joined into the last) */
    tv_sb extra = {0};
    for (unsigned i = 64; i < count; i++) {
        uint8_t *p;
        size_t len;
        JSValueRef v = JSObjectGetPropertyAtIndex(ctx, list, i, NULL);
        if (tv_js_bytes_view(v, &p, &len) && len) tv_sb_add(&extra, (const char *)p, len);
    }
    if (extra.len) {
        tv_sb_add(&extra, "", 0);
        total += extra.len;
    }
    size_t written = 0;
    int err = 0;
    /* (behind queued writes, everything waits its turn) */
    if (!s->wq && !extra.len) {
        while (written < total) {
            ssize_t r = writev(s->fd, iov, (int)niov);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) {
                if (errno != EAGAIN) err = -errno;
                break;
            }
            written += (size_t)r;
            /* drop what went out */
            size_t left = (size_t)r;
            unsigned k = 0;
            while (k < niov && left >= iov[k].iov_len) left -= iov[k++].iov_len;
            memmove(iov, iov + k, (niov - k) * sizeof *iov);
            niov -= k;
            if (niov) {
                iov[0].iov_base = (char *)iov[0].iov_base + left;
                iov[0].iov_len -= left;
            }
            if ((size_t)r == 0) break;
        }
    }
    s->bytes_written += (double)written;
    bool async = written < total && !err;
    if (async) {
        tv_node_wreq *w = calloc(1, sizeof *w);
        w->len = total - written;
        w->data = malloc(w->len);
        size_t off = 0;
        for (unsigned k = 0; k < niov; k++) {
            memcpy(w->data + off, iov[k].iov_base, iov[k].iov_len);
            off += iov[k].iov_len;
        }
        if (extra.len) memcpy(w->data + off, extra.data, extra.len);
        w->cb = (JSObjectRef)a[2];
        JSValueProtect(ctx, a[2]);
        if (s->wq_tail) s->wq_tail->next = w;
        else s->wq = w;
        s->wq_tail = w;
        tv_node_stream_update(s);
    }
    tv_sb_free(&extra);
    JSValueRef items[3] = { num(ctx, err), num(ctx, (double)written), JSValueMakeBoolean(ctx, async) };
    return array(ctx, 3, items);
}

/* shutdown(handle, cb): cb(status) once queued writes are out and the write side is closed */
NATIVE(n_stream_shutdown) {
    UNUSED;
    tv_node_stream *s = tv_node_stream_of(ctx, n, a);
    if (!s || s->closed || n < 2) return num(ctx, -ENOTCONN);
    if (s->shutdown_cb) return num(ctx, -EALREADY);
    if (s->file) {
        tv_node_fshutdown *r = calloc(1, sizeof *r);
        r->job.work = tv_node_fshutdown_work;
        r->job.done = tv_node_fshutdown_done;
        r->s = s;
        r->fd = s->fd;
        s->pending_file++;
        tv_node_queue(ctx, &r->job, a[1]);
        return num(ctx, 0);
    }
    s->shutdown_cb = (JSObjectRef)a[1];
    JSValueProtect(ctx, a[1]);
    tv_node_stream_update(s);
    return num(ctx, 0);
}

/* close(handle): stops it and closes the fd (queued writes fail with ECANCELED) */
NATIVE(n_stream_close) {
    UNUSED;
    tv_node_stream *s = tv_node_stream_of(ctx, n, a);
    if (!s || s->closed) return undef(ctx);
    s->reading = false;
    tv_node_stream_update(s);
    s->closed = true;
    if (s->raw_mode) { tv_node_tty_release(s->raw_mode, true); s->raw_mode = NULL; }
    if (s->counted) {
        tv_io_refs--;
        s->counted = false;
    }
    if (s->fd >= 0) {
        if (s->pending_file) {
            /* a worker still owns a copy of this fd — the last landing
               file job closes it (see tv_node_file_landed) */
        } else {
            close(s->fd);
            s->fd = -1;
        }
    }
    while (s->wq) {
        tv_node_wreq *w = s->wq;
        s->wq = w->next;
        tv_node_stream_complete(w, -ECANCELED);
    }
    s->wq_tail = NULL;
    if (s->shutdown_cb) JSValueUnprotect(ctx, s->shutdown_cb);
    s->shutdown_cb = NULL;
    if (s->onread) JSValueUnprotect(ctx, s->onread);
    s->onread = NULL;
    if (s->onconnection) JSValueUnprotect(ctx, s->onconnection);
    s->onconnection = NULL;
    s->listening = false;
    if (s->connect_cb) {
        /* (a connect in progress ends with ECANCELED) */
        JSObjectRef cb = s->connect_cb;
        s->connect_cb = NULL;
        s->connecting = false;
        JSValueRef arg = JSValueMakeNumber(ctx, -ECANCELED);
        tv_node_call_args(cb, 1, &arg);
        JSValueUnprotect(ctx, cb);
    }
    return undef(ctx);
}

/* ------------------------------------------------------------------ sockets (tcp_wrap, pipe_wrap's
 * unix sockets) on the stream handles */

#include <netinet/tcp.h>
#include <sys/un.h>

/* an address from an ip (4 or 6) or a unix path: its length, 0 if it isn't one */
static socklen_t tv_node_sockaddr(const char *where, int port, int family, struct sockaddr_storage *ss) {
    memset(ss, 0, sizeof *ss);
    if (family == 0) {
        struct sockaddr_un *un = (struct sockaddr_un *)ss;
        if (strlen(where) >= sizeof un->sun_path) return 0;
        un->sun_family = AF_UNIX;
        strcpy(un->sun_path, where);
        return (socklen_t)sizeof *un;
    }
    if (family == 4) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)ss;
        if (inet_pton(AF_INET, where, &v4->sin_addr) != 1) return 0;
        v4->sin_family = AF_INET;
        v4->sin_port = htons((uint16_t)port);
        return (socklen_t)sizeof *v4;
    }
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ss;
    char host[INET6_ADDRSTRLEN + 16];
    snprintf(host, sizeof host, "%s", where);
    char *pct = strchr(host, '%');
    if (pct) {
        *pct = 0;
        v6->sin6_scope_id = if_nametoindex(pct + 1);
    }
    if (inet_pton(AF_INET6, host, &v6->sin6_addr) != 1) return 0;
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons((uint16_t)port);
    return (socklen_t)sizeof *v6;
}

/* socket(family: 4, 6 or 0 for unix) -> fd or -errno */
NATIVE(n_net_socket) {
    UNUSED;
    int family = (int)arg_num(ctx, n, a, 0, 4);
    int fd = socket(family == 4 ? AF_INET : family == 6 ? AF_INET6 : AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return num(ctx, -errno);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return num(ctx, fd);
}

/* bind(fd, address, port, family, ipv6Only) -> 0 or -errno */
NATIVE(n_net_bind) {
    UNUSED;
    int fd = (int)arg_num(ctx, n, a, 0, -1);
    char *where = arg_cstr(ctx, n, a, 1);
    int family = (int)arg_num(ctx, n, a, 3, 4);
    struct sockaddr_storage ss;
    socklen_t len = where ? tv_node_sockaddr(where, (int)arg_num(ctx, n, a, 2, 0), family, &ss) : 0;
    free(where);
    if (!len) return num(ctx, -EINVAL);
    if (family != 0) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (family == 6) {
            int only = n > 4 && JSValueToBoolean(ctx, a[4]) ? 1 : 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof only);
        }
    }
    return num(ctx, bind(fd, (struct sockaddr *)&ss, len) == 0 ? 0 : -errno);
}

/* listen(handle, backlog, onconnection(fd | null, err)) -> 0 or -errno */
NATIVE(n_net_listen) {
    UNUSED;
    tv_node_stream *s = tv_node_stream_of(ctx, n, a);
    if (!s || s->closed || n < 3) return num(ctx, -EINVAL);
    if (listen(s->fd, (int)arg_num(ctx, n, a, 1, 511)) != 0) return num(ctx, -errno);
    if (s->onconnection) JSValueUnprotect(ctx, s->onconnection);
    s->onconnection = (JSObjectRef)a[2];
    JSValueProtect(ctx, a[2]);
    s->listening = true;
    tv_node_stream_update(s);
    return num(ctx, 0);
}

/* connect(handle, address, port, family, cb(status)) -> 0 or -errno (cb later) */
NATIVE(n_net_connect) {
    UNUSED;
    tv_node_stream *s = tv_node_stream_of(ctx, n, a);
    if (!s || s->closed || n < 5) return num(ctx, -EINVAL);
    char *where = arg_cstr(ctx, n, a, 1);
    struct sockaddr_storage ss;
    socklen_t len = where ? tv_node_sockaddr(where, (int)arg_num(ctx, n, a, 2, 0), (int)arg_num(ctx, n, a, 3, 4), &ss) : 0;
    free(where);
    if (!len) return num(ctx, -EINVAL);
    int r;
    while ((r = connect(s->fd, (struct sockaddr *)&ss, len)) != 0 && errno == EINTR) {}
    int err = r == 0 ? 0 : errno == EINPROGRESS ? EINPROGRESS : errno;
    /* (an immediate failure: the binding reports it on a later turn, as libuv does) */
    if (err != 0 && err != EINPROGRESS) return num(ctx, -err);
    s->connect_cb = (JSObjectRef)a[4];
    JSValueProtect(ctx, a[4]);
    s->connecting = true;
    tv_node_stream_update(s);
    return num(ctx, 0);
}

/* name(fd, peer) -> [address, family, port] or -errno */
NATIVE(n_net_name) {
    UNUSED;
    int fd = (int)arg_num(ctx, n, a, 0, -1);
    bool peer = n > 1 && JSValueToBoolean(ctx, a[1]);
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    int r = peer ? getpeername(fd, (struct sockaddr *)&ss, &len) : getsockname(fd, (struct sockaddr *)&ss, &len);
    if (r != 0) return num(ctx, -errno);
    char ip[INET6_ADDRSTRLEN];
    JSValueRef items[3];
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof ip);
        items[0] = str(ctx, ip);
        items[1] = str(ctx, "IPv4");
        items[2] = num(ctx, ntohs(v4->sin_port));
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &v6->sin6_addr, ip, sizeof ip);
        items[0] = str(ctx, ip);
        items[1] = str(ctx, "IPv6");
        items[2] = num(ctx, ntohs(v6->sin6_port));
    } else {
        struct sockaddr_un *un = (struct sockaddr_un *)&ss;
        items[0] = str(ctx, len > offsetof(struct sockaddr_un, sun_path) ? un->sun_path : "");
        items[1] = str(ctx, "unix");
        items[2] = num(ctx, 0);
    }
    return array(ctx, 3, items);
}

/* option(fd, "nodelay" | "keepalive", enable, delay seconds) -> 0 or -errno */
NATIVE(n_net_option) {
    UNUSED;
    int fd = (int)arg_num(ctx, n, a, 0, -1);
    char *what = arg_cstr(ctx, n, a, 1);
    int on = n > 2 && JSValueToBoolean(ctx, a[2]) ? 1 : 0;
    int r = 0;
    if (what && strcmp(what, "nodelay") == 0) {
        r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    } else if (what && strcmp(what, "keepalive") == 0) {
        r = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
        int delay = (int)arg_num(ctx, n, a, 3, 0);
        if (r == 0 && on && delay > 0) {
#ifdef TCP_KEEPALIVE
            r = setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &delay, sizeof delay);
#else
            r = setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &delay, sizeof delay);
#endif
        }
    } else if (what && strcmp(what, "reset") == 0) {
        struct linger l = { 1, 0 };
        r = setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
    }
    free(what);
    return num(ctx, r == 0 ? 0 : -errno);
}

/* release(handle): JS is done with it (after close) */
NATIVE(n_stream_release) {
    UNUSED;
    tv_node_stream *s = tv_node_stream_of(ctx, n, a);
    if (s && s->self) {
        JSValueUnprotect(ctx, s->self);
        s->self = NULL;
    }
    return undef(ctx);
}

/* ref(handle, bool) */
NATIVE(n_stream_ref) {
    UNUSED;
    tv_node_stream *s = tv_node_stream_of(ctx, n, a);
    if (!s) return undef(ctx);
    s->refed = n > 1 && JSValueToBoolean(ctx, a[1]);
    if (!s->closed) tv_node_stream_update(s);
    return undef(ctx);
}

/* info(handle) -> [fd, bytesRead, bytesWritten, write queue size] */
NATIVE(n_stream_info) {
    UNUSED;
    tv_node_stream *s = tv_node_stream_of(ctx, n, a);
    if (!s) return undef(ctx);
    double queued = 0;
    for (tv_node_wreq *w = s->wq; w; w = w->next) queued += (double)(w->len - w->off);
    JSValueRef items[4] = { num(ctx, s->fd), num(ctx, s->bytes_read), num(ctx, s->bytes_written), num(ctx, queued) };
    return array(ctx, 4, items);
}

/* ------------------------------------------------------------------ processes (process_wrap) */

typedef struct {
    tv_io io;
    pid_t pid;
    int pidfd;
    bool exited, refed, counted;
    JSObjectRef onexit;      /* onexit(exitCode, signal) (protected until it exits) */
} tv_node_proc;

static void tv_node_proc_count(tv_node_proc *p) {
    bool active = p->refed && !p->exited;
    if (active != p->counted) {
        tv_io_refs += active ? 1 : -1;
        p->counted = active;
    }
}

static void tv_node_proc_ready(tv_io *h, bool readable, bool writable, bool broken) {
    (void)readable; (void)writable; (void)broken;
    tv_node_proc *p = (tv_node_proc *)h;
    if (p->exited) return;
    int st = 0;
    pid_t r;
    while ((r = waitpid(p->pid, &st, WNOHANG)) < 0 && errno == EINTR) {}
    if (r == 0) return;
    p->exited = true;
    if (p->pidfd >= 0) close(p->pidfd);
    p->pidfd = -1;
    tv_node_proc_count(p);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 0;
    int sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
    JSObjectRef cb = p->onexit;
    p->onexit = NULL;
    JSValueRef args[2] = { JSValueMakeNumber(tv_js_ctx, code), JSValueMakeNumber(tv_js_ctx, sig) };
    tv_node_call_args(cb, 2, args);
    JSValueUnprotect(tv_js_ctx, cb);
}

static void tv_node_proc_finalize(JSObjectRef o) {
    free(JSObjectGetPrivate(o));
}

static JSClassRef tv_node_proc_class(void) {
    static JSClassRef cls;
    if (!cls) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = "ProcessHandle";
        def.finalize = tv_node_proc_finalize;
        cls = JSClassCreate(&def);
    }
    return cls;
}

/* spawn(file, args, env|null, cwd|null, stdio [[type, fd]], detached, onexit) -> [pid or -errno,
 * handle, [parent fd per stdio: the pipes' ends, else -1]] */
NATIVE(n_spawn) {
    UNUSED;
    if (n < 7) return undef(ctx);
    char *file = arg_cstr(ctx, n, a, 0);
    char **argv = tv_node_strings(ctx, a[1]);
    char **env = JSValueIsNull(ctx, a[2]) ? NULL : tv_node_strings(ctx, a[2]);
    char *cwd = JSValueIsNull(ctx, a[3]) || JSValueIsUndefined(ctx, a[3]) ? NULL : arg_cstr(ctx, n, a, 3);
    tv_node_stdio stdio[16];
    int nstdio = tv_node_stdio_specs(ctx, a[4], stdio, 16);
    if (!argv) argv = calloc(2, sizeof *argv), argv[0] = strdup(file ? file : "");
    pid_t pid = file ? tv_node_spawn(file, argv, env, cwd, stdio, nstdio, JSValueToBoolean(ctx, a[5])) : -EINVAL;
    free(file);
    free(cwd);
    tv_node_strings_free(argv);
    tv_node_strings_free(env);
    JSValueRef fds[16];
    for (int i = 0; i < nstdio; i++) fds[i] = num(ctx, pid > 0 && stdio[i].type == TV_STDIO_PIPE ? stdio[i].fd : -1);
    JSValueRef handle = JSValueMakeNull(ctx);
    if (pid > 0) {
        tv_node_proc *p = calloc(1, sizeof *p);
        p->io.ready = tv_node_proc_ready;
        p->pid = pid;
        p->pidfd = -1;
        p->refed = true;
        p->onexit = (JSObjectRef)a[6];
        JSValueProtect(ctx, a[6]);
        JSObjectRef o = JSObjectMake(ctx, tv_node_proc_class(), p);
        handle = o;
        /* (the handle lives while the child does) */
        JSValueProtect(ctx, o);
        if (!tv_io_proc(pid, &p->io, &p->pidfd)) tv_node_proc_ready(&p->io, false, false, false);
        tv_node_proc_count(p);
    }
    JSValueRef items[3] = { num(ctx, pid), handle, array(ctx, (size_t)nstdio, fds) };
    return array(ctx, 3, items);
}

/* kill(handle, signal) -> 0 or -errno */
NATIVE(n_proc_kill) {
    UNUSED;
    if (n < 1 || !JSValueIsObjectOfClass(ctx, a[0], tv_node_proc_class())) return num(ctx, -EINVAL);
    tv_node_proc *p = JSObjectGetPrivate((JSObjectRef)a[0]);
    if (!p || p->exited) return num(ctx, -ESRCH);
    return num(ctx, kill(p->pid, (int)arg_num(ctx, n, a, 1, SIGTERM)) == 0 ? 0 : -errno);
}

/* procRef(handle, bool); procRelease(handle): JS is done with it */
NATIVE(n_proc_ref) {
    UNUSED;
    if (n < 1 || !JSValueIsObjectOfClass(ctx, a[0], tv_node_proc_class())) return undef(ctx);
    tv_node_proc *p = JSObjectGetPrivate((JSObjectRef)a[0]);
    p->refed = n > 1 && JSValueToBoolean(ctx, a[1]);
    tv_node_proc_count(p);
    return undef(ctx);
}

NATIVE(n_proc_release) {
    UNUSED;
    if (n < 1 || !JSValueIsObjectOfClass(ctx, a[0], tv_node_proc_class())) return undef(ctx);
    tv_node_proc *p = JSObjectGetPrivate((JSObjectRef)a[0]);
    /* (a child still running keeps its handle until it exits) */
    if (p->exited) JSValueUnprotect(ctx, a[0]);
    return undef(ctx);
}

/* ------------------------------------------------------------------ fetch (Tov's HTTP client,
 * tov.c, for the fetch() global's Request and Response in internal/tov/fetch.js): a request is
 * a handle whose finalizer frees it; each wait calls back from the loop once its promise settles */

static void tv_node_fetch_finalize(JSObjectRef o) {
    tv_int *id = JSObjectGetPrivate(o);
    if (!id) return;
    tv_native_fetchFree(*id);
    free(id);
}

static JSClassRef tv_node_fetch_class(void) {
    static JSClassRef cls;
    if (!cls) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = "FetchRequest";
        def.finalize = tv_node_fetch_finalize;
        cls = JSClassCreate(&def);
    }
    return cls;
}

static tv_int tv_node_fetch_id(JSContextRef ctx, size_t n, const JSValueRef a[]) {
    if (n < 1 || !JSValueIsObjectOfClass(ctx, a[0], tv_node_fetch_class())) return 0;
    tv_int *id = JSObjectGetPrivate((JSObjectRef)a[0]);
    return id ? *id : 0;
}

static tv_str tv_node_arg_str(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i) {
    uint8_t *p;
    size_t len;
    if (i < n && JSValueIsObject(ctx, a[i]) && tv_js_bytes_view(a[i], &p, &len)) return tv_str_from((const char *)p, len);
    char *s = i < n && !JSValueIsNull(ctx, a[i]) && !JSValueIsUndefined(ctx, a[i]) ? arg_cstr(ctx, n, a, i) : NULL;
    tv_str r = s ? tv_str_from(s, strlen(s)) : TV_EMPTY_STR;
    free(s);
    return r;
}

/* fetchStart(method, url, wire headers, body bytes|string|null, redirect, flags, ca, unix, proxy) -> handle */
NATIVE(n_fetch_start) {
    UNUSED;
    tv_str args[9];
    for (size_t i = 0; i < 9; i++) args[i] = i == 4 || i == 5 ? TV_EMPTY_STR : tv_node_arg_str(ctx, n, a, i);
    tv_int id = tv_native_fetchStart(args[0], args[1], args[2], args[3], (tv_int)arg_num(ctx, n, a, 4, 0), (tv_int)arg_num(ctx, n, a, 5, 1),
                                     args[6], args[7], args[8]);
    for (size_t i = 0; i < 9; i++) tv_str_release(args[i]);
    tv_int *box = malloc(sizeof *box);
    *box = id;
    return JSObjectMake(ctx, tv_node_fetch_class(), box);
}

/* a promise's int result to a JS callback */
typedef struct {
    tv_promise *p;
    JSObjectRef cb;
} tv_node_fetch_wait;

static void tv_node_fetch_settled(void *a, void *b) {
    (void)b;
    tv_node_fetch_wait *w = a;
    double v = w->p->state == TV_FULFILLED ? (double)*(tv_int *)tv_promise_value(w->p) : -1;
    tv_promise_release(w->p);
    tv_io_refs--;
    JSObjectRef cb = w->cb;
    free(w);
    JSValueRef arg = JSValueMakeNumber(tv_js_ctx, v);
    tv_node_call_args(cb, 1, &arg);
    JSValueUnprotect(tv_js_ctx, cb);
}

static JSValueRef tv_node_fetch_on(JSContextRef ctx, tv_promise *p, JSValueRef cb) {
    tv_node_fetch_wait *w = malloc(sizeof *w);
    w->p = p;
    w->cb = (JSObjectRef)cb;
    JSValueProtect(ctx, cb);
    /* (pending JS callbacks keep the program running) */
    tv_io_refs++;
    tv_promise_on(p, tv_node_fetch_settled, w, NULL);
    return undef(ctx);
}

/* fetchWait(handle, cb(0 head in | -1 failed | -2 aborted)) */
NATIVE(n_fetch_wait) {
    UNUSED;
    tv_int id = tv_node_fetch_id(ctx, n, a);
    if (!id || n < 2) return undef(ctx);
    return tv_node_fetch_on(ctx, tv_native_fetchWait(id), a[1]);
}

/* fetchBodyWait(handle, cb(0 | -1 | -2)): the whole body is in */
NATIVE(n_fetch_body_wait) {
    UNUSED;
    tv_int id = tv_node_fetch_id(ctx, n, a);
    if (!id || n < 2) return undef(ctx);
    return tv_node_fetch_on(ctx, tv_native_fetchBodyWait(id), a[1]);
}

/* fetchRead(handle, cb(bytes waiting > 0 | 0 the end | < 0 failed)) */
NATIVE(n_fetch_read) {
    UNUSED;
    tv_int id = tv_node_fetch_id(ctx, n, a);
    if (!id || n < 2) return undef(ctx);
    return tv_node_fetch_on(ctx, tv_native_fetchRead(id), a[1]);
}

/* fetchTake(handle) -> the bytes that have arrived */
NATIVE(n_fetch_take) {
    UNUSED;
    tv_int id = tv_node_fetch_id(ctx, n, a);
    size_t len = 0;
    const char *p = id ? tv_native_fetchData(id, &len) : NULL;
    /* (one copy, straight into the engine's array) */
    JSValueRef r = tv_js_bytes_copy(p ? p : "", len);
    if (id) tv_native_fetchTaken(id);
    return r;
}

/* fetchBody(handle) -> the whole body (moved out) */
NATIVE(n_fetch_body) {
    UNUSED;
    tv_int id = tv_node_fetch_id(ctx, n, a);
    if (!id) return tv_js_bytes_copy("", 0);
    tv_str body = tv_native_fetchBody(id);
    JSValueRef r = tv_js_bytes_copy(body.p->data, (size_t)body.p->len);
    tv_str_release(body);
    return r;
}

static JSValueRef tv_node_str_value(JSContextRef ctx, tv_str s) {
    JSStringRef js = JSStringCreateWithUTF8CString(s.p->data);
    JSValueRef v = JSValueMakeString(ctx, js);
    JSStringRelease(js);
    tv_str_release(s);
    return v;
}

/* fetchInfo(handle) -> [status, statusText, wire headers, url, redirected] */
NATIVE(n_fetch_info) {
    UNUSED;
    tv_int id = tv_node_fetch_id(ctx, n, a);
    if (!id) return undef(ctx);
    JSValueRef items[5] = {
        num(ctx, (double)tv_native_fetchStatus(id)),
        tv_node_str_value(ctx, tv_native_fetchStatusText(id)),
        tv_node_str_value(ctx, tv_native_fetchHeaders(id)),
        tv_node_str_value(ctx, tv_native_fetchUrl(id)),
        JSValueMakeBoolean(ctx, tv_native_fetchRedirected(id)),
    };
    return array(ctx, 5, items);
}

/* fetchError(handle) -> [code, message] */
NATIVE(n_fetch_error) {
    UNUSED;
    tv_int id = tv_node_fetch_id(ctx, n, a);
    if (!id) return undef(ctx);
    JSValueRef items[2] = { tv_node_str_value(ctx, tv_native_fetchErrorCode(id)), tv_node_str_value(ctx, tv_native_fetchErrorMessage(id)) };
    return array(ctx, 2, items);
}

NATIVE(n_fetch_abort) {
    UNUSED;
    tv_int id = tv_node_fetch_id(ctx, n, a);
    if (id) tv_native_fetchAbort(id);
    return undef(ctx);
}

/* tls: whether the program links Tov's TLS client (https works) */
NATIVE(n_fetch_tls) {
    UNUSED;
    return JSValueMakeBoolean(ctx, tv_tls_impl != NULL);
}

/* (JavaScriptCore's, exported though not in its headers) */
extern void JSReportExtraMemoryCost(JSContextRef ctx, size_t size) __attribute__((weak_import));

/* Response bytes arriving: memory the engine's objects hold, which it should count towards its
 * next collection (a response read partway and dropped holds its buffer and connection until
 * then) */
static void tv_node_fetch_bytes(size_t n) {
    /* (in 256 KB steps: the engine's accounting is slow per call) */
    static size_t pending;
    pending += n;
    if (pending < (256 << 10)) return;
    if (JSReportExtraMemoryCost && tv_js_ctx) JSReportExtraMemoryCost(tv_js_ctx, pending);
    pending = 0;
}

static void tv_node_fetch_install(JSContextRef ctx, JSObjectRef native) {
    tv_fetch_on_bytes = tv_node_fetch_bytes;
    JSObjectRef f = JSObjectMake(ctx, NULL, NULL);
    tv_js_def(ctx, f, "start", n_fetch_start);
    tv_js_def(ctx, f, "wait", n_fetch_wait);
    tv_js_def(ctx, f, "bodyWait", n_fetch_body_wait);
    tv_js_def(ctx, f, "read", n_fetch_read);
    tv_js_def(ctx, f, "take", n_fetch_take);
    tv_js_def(ctx, f, "body", n_fetch_body);
    tv_js_def(ctx, f, "info", n_fetch_info);
    tv_js_def(ctx, f, "error", n_fetch_error);
    tv_js_def(ctx, f, "abort", n_fetch_abort);
    tv_js_def(ctx, f, "tls", n_fetch_tls);
    set(ctx, native, "fetch", f);
}

/* ------------------------------------------------------------------ http servers on Tov's native
 * server (internal/tov/http_server.js): it parses requests and manages connections; each request
 * is deferred and handed to JavaScript, whose ServerResponse writes the response bytes raw. */

typedef struct {
    tv_env base;
    JSObjectRef on_request;
} tv_node_http_env;

/* bytes as a JS string, one char per byte (latin1: header bytes survive as they are) */
static JSValueRef tv_node_latin1(JSContextRef ctx, const char *p, size_t n) {
    JSChar stack[512];
    JSChar *buf = n <= 512 ? stack : malloc(n * sizeof(JSChar));
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)p[i];
    JSStringRef s = JSStringCreateWithCharacters(buf, n);
    if (buf != stack) free(buf);
    JSValueRef v = JSValueMakeString(ctx, s);
    JSStringRelease(s);
    return v;
}

static void tv_node_http_request(tv_env *env, tv_str method, tv_str target, tv_str headers, tv_str body) {
    tv_node_http_env *e = (tv_node_http_env *)env;
    JSContextRef ctx = tv_js_ctx;
    tv_int id = tv_native_httpDefer();
    JSValueRef args[6] = {
        JSValueMakeNumber(ctx, (double)id),
        tv_node_latin1(ctx, method.p->data, (size_t)method.p->len),
        tv_node_latin1(ctx, target.p->data, (size_t)target.p->len),
        tv_node_latin1(ctx, headers.p->data, (size_t)headers.p->len),
        body.p->len ? (JSValueRef)tv_js_bytes_copy(body.p->data, (size_t)body.p->len) : JSValueMakeNull(ctx),
        JSValueMakeBoolean(ctx, tv_http_v10),
    };
    tv_node_call_args(e->on_request, 6, args);
}

/* httpListen(port, host, nodeErrors, onRequest(id, method, target, wire headers, body|null, http10)) -> server id, or an
 * error message (nodeErrors: malformed requests are answered as Node.js answers them) */
NATIVE(n_http_listen) {
    UNUSED;
    if (n < 4 || !JSValueIsObject(ctx, a[3])) return undef(ctx);
    tv_node_http_env *e = tv_alloc(sizeof *e);
    memset(e, 0, sizeof *e);
    e->base.rc = 1;
    e->on_request = (JSObjectRef)a[3];
    JSValueProtect(ctx, a[3]);
    char *host = arg_cstr(ctx, n, a, 1);
    tv_str h = host ? tv_str_from(host, strlen(host)) : TV_EMPTY_STR;
    free(host);
    tv_fn fn = { (void *)tv_node_http_request, &e->base };
    tv_int id = tv_native_httpListen((tv_int)arg_num(ctx, n, a, 0, 0), h, fn);
    tv_str_release(h);
    if (id >= 0 && JSValueToBoolean(ctx, a[2])) tv_native_httpNodeErrors(id);
    if (id < 0) {
        tv_str err = tv_native_takeError();
        JSValueRef msg = tv_node_str_value(ctx, err);
        return msg;
    }
    return num(ctx, (double)id);
}

/* a JS string's bytes: latin1 (a byte per char) or UTF-8; the caller frees them */
static char *tv_node_string_bytes(JSContextRef ctx, JSValueRef v, bool latin1, size_t *len) {
    JSStringRef js = JSValueToStringCopy(ctx, v, NULL);
    if (!js) {
        *len = 0;
        return NULL;
    }
    size_t n = JSStringGetLength(js);
    const JSChar *c = JSStringGetCharactersPtr(js);
    char *out = malloc(n * 3 + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned u = c[i];
        if (u < 0x80 || latin1) {
            out[o++] = (char)u;
        } else if (u < 0x800) {
            out[o++] = (char)(0xc0 | (u >> 6));
            out[o++] = (char)(0x80 | (u & 0x3f));
        } else if (u >= 0xd800 && u <= 0xdbff && i + 1 < n && c[i + 1] >= 0xdc00 && c[i + 1] <= 0xdfff) {
            unsigned cp = 0x10000 + ((u - 0xd800) << 10) + (c[i + 1] - 0xdc00);
            i++;
            out[o++] = (char)(0xf0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        } else {
            if (u >= 0xd800 && u <= 0xdfff) u = 0xfffd;
            out[o++] = (char)(0xe0 | (u >> 12));
            out[o++] = (char)(0x80 | ((u >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (u & 0x3f));
        }
    }
    JSStringRelease(js);
    *len = o;
    return out;
}

/* httpWrite(id, bytes|string|null, end: 0 more | 1 done | 2 done and close, latin1) */
NATIVE(n_http_write) {
    UNUSED;
    tv_int id = (tv_int)arg_num(ctx, n, a, 0, 0);
    uint8_t *p = NULL;
    size_t len = 0;
    char *owned = NULL;
    if (n > 1 && JSValueIsString(ctx, a[1])) {
        owned = tv_node_string_bytes(ctx, a[1], n > 3 && JSValueToBoolean(ctx, a[3]), &len);
        p = (uint8_t *)owned;
    } else if (n > 1 && JSValueIsObject(ctx, a[1])) {
        tv_js_bytes_view(a[1], &p, &len);
    }
    tv_native_httpWriteRaw(id, (const char *)p, len, (int)arg_num(ctx, n, a, 2, 0));
    free(owned);
    return undef(ctx);
}

/* httpBuffered(id) -> bytes of the response the socket hasn't taken yet (-1: the client has gone) */
NATIVE(n_http_buffered) {
    UNUSED;
    return num(ctx, (double)tv_native_httpBuffered((tv_int)arg_num(ctx, n, a, 0, 0)));
}

/* httpOnGone(fn): fn(id) for each deferred request whose client goes away before it's answered */
static JSObjectRef tv_node_http_gone_fn;
static void tv_node_http_gone(tv_int id) {
    if (!tv_node_http_gone_fn) return;
    JSValueRef arg = JSValueMakeNumber(tv_js_ctx, (double)id);
    tv_node_call_args(tv_node_http_gone_fn, 1, &arg);
}

NATIVE(n_http_on_gone) {
    UNUSED;
    if (tv_node_http_gone_fn) JSValueUnprotect(ctx, tv_node_http_gone_fn);
    tv_node_http_gone_fn = n > 0 && JSValueIsObject(ctx, a[0]) ? (JSObjectRef)a[0] : NULL;
    if (tv_node_http_gone_fn) JSValueProtect(ctx, tv_node_http_gone_fn);
    tv_http_on_gone = tv_node_http_gone_fn ? tv_node_http_gone : NULL;
    return undef(ctx);
}

/* httpFd(id) -> the connection's fd (-1 if it's gone) */
NATIVE(n_http_fd) {
    UNUSED;
    return num(ctx, tv_native_httpFd((tv_int)arg_num(ctx, n, a, 0, 0)));
}

/* httpTakeover(id) -> [fd, the bytes after the request] or null */
NATIVE(n_http_takeover) {
    UNUSED;
    tv_sb rest = {0};
    int fd = tv_native_httpTakeover((tv_int)arg_num(ctx, n, a, 0, 0), &rest);
    if (fd < 0) {
        tv_sb_free(&rest);
        return JSValueMakeNull(ctx);
    }
    JSValueRef items[2] = { num(ctx, fd), tv_js_bytes_copy(rest.data ? rest.data : "", rest.len) };
    tv_sb_free(&rest);
    return array(ctx, 2, items);
}

NATIVE(n_http_port) {
    UNUSED;
    return num(ctx, (double)tv_native_httpPort((tv_int)arg_num(ctx, n, a, 0, 0)));
}

NATIVE(n_http_stop) {
    UNUSED;
    tv_native_httpStop((tv_int)arg_num(ctx, n, a, 0, 0), n > 1 && JSValueToBoolean(ctx, a[1]));
    return undef(ctx);
}

static void tv_node_http_install(JSContextRef ctx, JSObjectRef native) {
    JSObjectRef h = JSObjectMake(ctx, NULL, NULL);
    tv_js_def(ctx, h, "listen", n_http_listen);
    tv_js_def(ctx, h, "write", n_http_write);
    tv_js_def(ctx, h, "fd", n_http_fd);
    tv_js_def(ctx, h, "onGone", n_http_on_gone);
    tv_js_def(ctx, h, "buffered", n_http_buffered);
    tv_js_def(ctx, h, "takeover", n_http_takeover);
    tv_js_def(ctx, h, "port", n_http_port);
    tv_js_def(ctx, h, "stop", n_http_stop);
    set(ctx, native, "http", h);
}

static void tv_node_streams_install(JSContextRef ctx, JSObjectRef native) {
    JSObjectRef st = JSObjectMake(ctx, NULL, NULL);
    tv_js_def(ctx, st, "open", n_stream_open);
    tv_js_def(ctx, st, "setRawMode", n_stream_set_raw_mode);
    tv_js_def(ctx, st, "readStart", n_stream_read_start);
    tv_js_def(ctx, st, "readStop", n_stream_read_stop);
    tv_js_def(ctx, st, "write", n_stream_write);
    tv_js_def(ctx, st, "shutdown", n_stream_shutdown);
    tv_js_def(ctx, st, "close", n_stream_close);
    tv_js_def(ctx, st, "release", n_stream_release);
    tv_js_def(ctx, st, "ref", n_stream_ref);
    tv_js_def(ctx, st, "info", n_stream_info);
    tv_js_def(ctx, st, "socket", n_net_socket);
    tv_js_def(ctx, st, "bind", n_net_bind);
    tv_js_def(ctx, st, "listen", n_net_listen);
    tv_js_def(ctx, st, "connect", n_net_connect);
    tv_js_def(ctx, st, "name", n_net_name);
    tv_js_def(ctx, st, "option", n_net_option);
    set(ctx, native, "stream", st);
    JSObjectRef pr = JSObjectMake(ctx, NULL, NULL);
    tv_js_def(ctx, pr, "spawn", n_spawn);
    tv_js_def(ctx, pr, "kill", n_proc_kill);
    tv_js_def(ctx, pr, "ref", n_proc_ref);
    tv_js_def(ctx, pr, "release", n_proc_release);
    set(ctx, native, "process", pr);
}

/* ------------------------------------------------------------------ vm (Node.js's node_contextify.cc)
 *
 * Scripts run with JSEvaluateScript, in this context or a new one. A new context is a global
 * context in the same group (values pass freely between them), whose global object's class
 * forwards lookups and assignments to the sandbox object first, as Node.js's contextified
 * global does; the sandbox is held on the global under a symbol key. */

static JSValueRef tv_vm_key;      /* the symbol the sandbox is held under */

/* (reading the key comes back through the global's callbacks: those calls find nothing here and
 * fall through to the property itself) */
static JSObjectRef tv_vm_sandbox(JSContextRef ctx, JSObjectRef global) {
    static bool busy;
    if (!tv_vm_key || busy) return NULL;
    busy = true;
    JSValueRef v = JSObjectGetPropertyForKey(ctx, global, tv_vm_key, NULL);
    busy = false;
    return v && JSValueIsObject(ctx, v) ? (JSObjectRef)v : NULL;
}

static JSValueRef tv_vm_get(JSContextRef ctx, JSObjectRef object, JSStringRef name, JSValueRef *exc) {
    JSObjectRef sandbox = tv_vm_sandbox(ctx, object);
    if (!sandbox || !JSObjectHasProperty(ctx, sandbox, name)) return NULL;
    return JSObjectGetProperty(ctx, sandbox, name, exc);
}

static bool tv_vm_has(JSContextRef ctx, JSObjectRef object, JSStringRef name) {
    JSObjectRef sandbox = tv_vm_sandbox(ctx, object);
    return sandbox && JSObjectHasProperty(ctx, sandbox, name);
}

static bool tv_vm_set(JSContextRef ctx, JSObjectRef object, JSStringRef name, JSValueRef value, JSValueRef *exc) {
    JSObjectRef sandbox = tv_vm_sandbox(ctx, object);
    if (!sandbox) return false;
    JSObjectSetProperty(ctx, sandbox, name, value, kJSPropertyAttributeNone, exc);
    return true;
}

static bool tv_vm_delete(JSContextRef ctx, JSObjectRef object, JSStringRef name, JSValueRef *exc) {
    JSObjectRef sandbox = tv_vm_sandbox(ctx, object);
    if (sandbox) JSObjectDeleteProperty(ctx, sandbox, name, exc);
    return false;
}

static void tv_vm_names(JSContextRef ctx, JSObjectRef object, JSPropertyNameAccumulatorRef names) {
    JSObjectRef sandbox = tv_vm_sandbox(ctx, object);
    if (!sandbox) return;
    JSPropertyNameArrayRef keys = JSObjectCopyPropertyNames(ctx, sandbox);
    size_t count = JSPropertyNameArrayGetCount(keys);
    for (size_t i = 0; i < count; i++) JSPropertyNameAccumulatorAddName(names, JSPropertyNameArrayGetNameAtIndex(keys, i));
    JSPropertyNameArrayRelease(keys);
}

static void tv_vm_context_finalize(JSObjectRef o) {
    JSGlobalContextRef c = JSObjectGetPrivate(o);
    if (c) JSGlobalContextRelease(c);
}

static JSClassRef tv_vm_context_class(void) {
    static JSClassRef cls;
    if (!cls) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = "VMContext";
        def.finalize = tv_vm_context_finalize;
        cls = JSClassCreate(&def);
    }
    return cls;
}

static JSContextRef tv_vm_target(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i) {
    if (i < n && JSValueIsObjectOfClass(ctx, a[i], tv_vm_context_class())) {
        JSGlobalContextRef c = JSObjectGetPrivate((JSObjectRef)a[i]);
        if (c) return c;
    }
    return ctx;
}

/* makeContext(sandbox) -> a context handle; the new global's lookups go to sandbox first */
NATIVE(n_vm_make_context) {
    UNUSED;
    if (n < 1 || !JSValueIsObject(ctx, a[0])) return undef(ctx);
    static JSClassRef global_class;
    if (!global_class) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = "Object";
        def.getProperty = tv_vm_get;
        def.hasProperty = tv_vm_has;
        def.setProperty = tv_vm_set;
        def.deleteProperty = tv_vm_delete;
        def.getPropertyNames = tv_vm_names;
        global_class = JSClassCreate(&def);
    }
    if (!tv_vm_key) {
        JSStringRef d = JSStringCreateWithUTF8CString("vm sandbox");
        tv_vm_key = JSValueMakeSymbol(ctx, d);
        JSStringRelease(d);
        JSValueProtect(ctx, tv_vm_key);
    }
    JSGlobalContextRef c = JSGlobalContextCreateInGroup(JSContextGetGroup(ctx), global_class);
    JSObjectSetPropertyForKey(c, JSContextGetGlobalObject(c), tv_vm_key, a[0], kJSPropertyAttributeDontEnum, NULL);
    return JSObjectMake(ctx, tv_vm_context_class(), c);
}

/* the context's global object */
NATIVE(n_vm_global) {
    UNUSED;
    return JSContextGetGlobalObject(tv_vm_target(ctx, n, a, 0));
}

/* check(code, filename, line) -> null, or the SyntaxError */
NATIVE(n_vm_check) {
    UNUSED;
    JSStringRef code = JSValueToStringCopy(ctx, n > 0 ? a[0] : undef(ctx), exc);
    if (!code) return undef(ctx);
    JSStringRef url = n > 1 ? JSValueToStringCopy(ctx, a[1], NULL) : NULL;
    JSValueRef err = NULL;
    bool ok = JSCheckScriptSyntax(ctx, code, url, (int)arg_num(ctx, n, a, 2, 1), &err);
    JSStringRelease(code);
    if (url) JSStringRelease(url);
    return ok || !err ? JSValueMakeNull(ctx) : err;
}

/* run(code, filename, line, context?) -> the script's completion value (or what it throws) */
NATIVE(n_vm_run) {
    UNUSED;
    JSStringRef code = JSValueToStringCopy(ctx, n > 0 ? a[0] : undef(ctx), exc);
    if (!code) return undef(ctx);
    JSStringRef url = n > 1 ? JSValueToStringCopy(ctx, a[1], NULL) : NULL;
    JSContextRef target = tv_vm_target(ctx, n, a, 3);
    JSValueRef r = JSEvaluateScript(target, code, NULL, url, (int)arg_num(ctx, n, a, 2, 1), exc);
    JSStringRelease(code);
    if (url) JSStringRelease(url);
    return r ? r : undef(ctx);
}

/* fn(params [names], body, filename, line, context?) -> a function, as new Function makes one */
NATIVE(n_vm_fn) {
    UNUSED;
    if (n < 2) return undef(ctx);
    JSContextRef target = tv_vm_target(ctx, n, a, 4);
    JSStringRef names[64];
    unsigned count = 0;
    if (JSValueIsObject(ctx, a[0])) {
        JSStringRef len_key = JSStringCreateWithUTF8CString("length");
        unsigned len = (unsigned)JSValueToNumber(ctx, JSObjectGetProperty(ctx, (JSObjectRef)a[0], len_key, NULL), NULL);
        JSStringRelease(len_key);
        for (unsigned i = 0; i < len && count < 64; i++) {
            JSValueRef v = JSObjectGetPropertyAtIndex(ctx, (JSObjectRef)a[0], i, NULL);
            names[count++] = JSValueToStringCopy(ctx, v, NULL);
        }
    }
    JSStringRef body = JSValueToStringCopy(ctx, a[1], exc);
    JSStringRef url = n > 2 ? JSValueToStringCopy(ctx, a[2], NULL) : NULL;
    JSObjectRef f = body ? JSObjectMakeFunction(target, NULL, count, names, body, url, (int)arg_num(ctx, n, a, 3, 1), exc) : NULL;
    for (unsigned i = 0; i < count; i++) JSStringRelease(names[i]);
    if (body) JSStringRelease(body);
    if (url) JSStringRelease(url);
    return f ? (JSValueRef)f : undef(ctx);
}

static void tv_node_vm_install(JSContextRef ctx, JSObjectRef native) {
    JSObjectRef v = JSObjectMake(ctx, NULL, NULL);
    tv_js_def(ctx, v, "makeContext", n_vm_make_context);
    tv_js_def(ctx, v, "global", n_vm_global);
    tv_js_def(ctx, v, "check", n_vm_check);
    tv_js_def(ctx, v, "run", n_vm_run);
    tv_js_def(ctx, v, "fn", n_vm_fn);
    set(ctx, native, "vm", v);
}

static void tv_node_dns_install(JSContextRef ctx, JSObjectRef native) {
    JSObjectRef d = JSObjectMake(ctx, NULL, NULL);
    tv_js_def(ctx, d, "getaddrinfo", n_dns_getaddrinfo);
    tv_js_def(ctx, d, "getnameinfo", n_dns_getnameinfo);
    tv_js_def(ctx, d, "canonicalizeIP", n_dns_canonicalize);
    tv_js_def(ctx, d, "ipv6Bytes", n_dns_ipv6_bytes);
    tv_js_def(ctx, d, "query", n_dns_query);
    set(ctx, native, "dns", d);
    tv_js_def(ctx, native, "heapStats", n_heap_stats);
}

/* ------------------------------------------------------------------ zlib (runtime/compress.c, through
 * tv_zs: there when the program's bundle has zlib) */

static void tv_node_zs_finalize(JSObjectRef o) {
    tv_zstream *z = JSObjectGetPrivate(o);
    if (z && tv_zs) tv_zs->close(z);
}

static JSClassRef tv_node_zs_class(void) {
    static JSClassRef cls;
    if (!cls) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = "ZlibHandle";
        def.finalize = tv_node_zs_finalize;
        cls = JSClassCreate(&def);
    }
    return cls;
}

static tv_zstream *tv_node_zs_of(JSContextRef ctx, size_t n, const JSValueRef a[]) {
    if (n < 1 || !JSValueIsObjectOfClass(ctx, a[0], tv_node_zs_class())) return NULL;
    return JSObjectGetPrivate((JSObjectRef)a[0]);
}

/* an error as [message, code, errno], or null for none */
static JSValueRef tv_node_zs_error(JSContextRef ctx, tv_zs_error e) {
    if (!e.message) return JSValueMakeNull(ctx);
    JSValueRef items[3] = { str(ctx, e.message), str(ctx, e.code ? e.code : ""), num(ctx, e.err) };
    return array(ctx, 3, items);
}

/* open(mode) -> handle */
NATIVE(n_zlib_open) {
    UNUSED;
    if (!tv_zs) {
        JSValueRef msg = str(ctx, "zlib is not available in this program (it was built without the codecs)");
        *exc = JSObjectMakeError(ctx, 1, &msg, NULL);
        return undef(ctx);
    }
    tv_zstream *z = tv_zs->open((int)arg_num(ctx, n, a, 0, 0));
    return JSObjectMake(ctx, tv_node_zs_class(), z);
}

/* initZlib(handle, windowBits, level, memLevel, strategy, dictionary?) */
NATIVE(n_zlib_init_zlib) {
    UNUSED;
    tv_zstream *z = tv_node_zs_of(ctx, n, a);
    if (!z) return undef(ctx);
    uint8_t *dict = NULL;
    size_t dl = 0;
    if (n > 5) tv_js_bytes_view(a[5], &dict, &dl);
    tv_zs->init_zlib(z, (int)arg_num(ctx, n, a, 1, 15), (int)arg_num(ctx, n, a, 2, -1), (int)arg_num(ctx, n, a, 3, 8), (int)arg_num(ctx, n, a, 4, 0),
                     dict, dict ? dl : 0);
    return undef(ctx);
}

/* (a Uint32Array's values) */
static uint32_t *tv_node_u32s(JSValueRef v, size_t *count) {
    uint8_t *p;
    size_t len;
    if (!tv_js_bytes_view(v, &p, &len)) {
        *count = 0;
        return NULL;
    }
    *count = len / 4;
    return (uint32_t *)p;
}

/* initBrotli(handle, params Uint32Array, dictionary?) -> error or null */
NATIVE(n_zlib_init_brotli) {
    UNUSED;
    tv_zstream *z = tv_node_zs_of(ctx, n, a);
    if (!z || n < 2) return undef(ctx);
    size_t count;
    uint32_t *params = tv_node_u32s(a[1], &count);
    uint8_t *dict = NULL;
    size_t dl = 0;
    if (n > 2) tv_js_bytes_view(a[2], &dict, &dl);
    return tv_node_zs_error(ctx, tv_zs->init_brotli(z, params, count, dict, dict ? dl : 0));
}

/* initZstd(handle, params Uint32Array, pledgedSrcSize (-1: unknown), dictionary?) -> error or null */
NATIVE(n_zlib_init_zstd) {
    UNUSED;
    tv_zstream *z = tv_node_zs_of(ctx, n, a);
    if (!z || n < 2) return undef(ctx);
    size_t count;
    uint32_t *params = tv_node_u32s(a[1], &count);
    double pledged = arg_num(ctx, n, a, 2, -1);
    uint8_t *dict = NULL;
    size_t dl = 0;
    if (n > 3) tv_js_bytes_view(a[3], &dict, &dl);
    return tv_node_zs_error(ctx, tv_zs->init_zstd(z, params, count, pledged < 0 ? UINT64_MAX : (uint64_t)pledged, dict, dict ? dl : 0));
}

/* write(handle, flush, in|null, inOff, inLen, out, outOff, outLen, state Uint32Array) -> error or
 * null; state gets [avail out, avail in] */
NATIVE(n_zlib_write) {
    UNUSED;
    tv_zstream *z = tv_node_zs_of(ctx, n, a);
    if (!z || n < 9) return undef(ctx);
    uint8_t *in = NULL, *out, *state;
    size_t in_cap = 0, out_cap, state_len;
    if (!JSValueIsNull(ctx, a[2])) tv_js_bytes_view(a[2], &in, &in_cap);
    if (!tv_js_bytes_view(a[5], &out, &out_cap) || !tv_js_bytes_view(a[8], &state, &state_len) || state_len < 8) return undef(ctx);
    size_t in_off = in ? (size_t)arg_num(ctx, n, a, 3, 0) : 0, in_len = in ? (size_t)arg_num(ctx, n, a, 4, 0) : 0;
    size_t out_off = (size_t)arg_num(ctx, n, a, 6, 0), out_len = (size_t)arg_num(ctx, n, a, 7, 0);
    if (in_off > in_cap || in_len > in_cap - in_off || out_off > out_cap || out_len > out_cap - out_off) {
        JSValueRef msg = str(ctx, "zlib: write out of bounds");
        *exc = JSObjectMakeError(ctx, 1, &msg, NULL);
        return undef(ctx);
    }
    uint32_t avail_in, avail_out;
    tv_zs->write(z, (int)arg_num(ctx, n, a, 1, 0), in ? in + in_off : NULL, (uint32_t)in_len, out + out_off, (uint32_t)out_len, &avail_in, &avail_out);
    uint32_t *st = (uint32_t *)state;
    st[0] = avail_out;
    st[1] = avail_in;
    return tv_node_zs_error(ctx, tv_zs->check(z));
}

/* params(handle, level, strategy) -> error or null */
NATIVE(n_zlib_params) {
    UNUSED;
    tv_zstream *z = tv_node_zs_of(ctx, n, a);
    if (!z) return undef(ctx);
    return tv_node_zs_error(ctx, tv_zs->params(z, (int)arg_num(ctx, n, a, 1, 0), (int)arg_num(ctx, n, a, 2, 0)));
}

/* reset(handle) -> error or null */
NATIVE(n_zlib_reset) {
    UNUSED;
    tv_zstream *z = tv_node_zs_of(ctx, n, a);
    if (!z) return undef(ctx);
    return tv_node_zs_error(ctx, tv_zs->reset(z));
}

/* close(handle): frees the stream now */
NATIVE(n_zlib_close) {
    UNUSED;
    tv_zstream *z = tv_node_zs_of(ctx, n, a);
    if (!z) return undef(ctx);
    JSObjectSetPrivate((JSObjectRef)a[0], NULL);
    tv_zs->close(z);
    return undef(ctx);
}

/* crc32(bytes or string, value) -> number */
NATIVE(n_zlib_crc32) {
    UNUSED;
    uint32_t value = (uint32_t)arg_num(ctx, n, a, 1, 0);
    if (!tv_zs || n < 1) return num(ctx, value);
    uint8_t *p;
    size_t len;
    if (tv_js_bytes_view(a[0], &p, &len)) return num(ctx, tv_zs->crc32(value, p, len));
    char *s = arg_cstr(ctx, n, a, 0);
    uint32_t r = s ? tv_zs->crc32(value, (const uint8_t *)s, strlen(s)) : value;
    free(s);
    return num(ctx, r);
}

static void tv_node_zlib_install(JSContextRef ctx, JSObjectRef native) {
    JSObjectRef z = JSObjectMake(ctx, NULL, NULL);
    set(ctx, z, "available", JSValueMakeBoolean(ctx, tv_zs != NULL));
    tv_js_def(ctx, z, "open", n_zlib_open);
    tv_js_def(ctx, z, "initZlib", n_zlib_init_zlib);
    tv_js_def(ctx, z, "initBrotli", n_zlib_init_brotli);
    tv_js_def(ctx, z, "initZstd", n_zlib_init_zstd);
    tv_js_def(ctx, z, "write", n_zlib_write);
    tv_js_def(ctx, z, "params", n_zlib_params);
    tv_js_def(ctx, z, "reset", n_zlib_reset);
    tv_js_def(ctx, z, "close", n_zlib_close);
    tv_js_def(ctx, z, "crc32", n_zlib_crc32);
    set(ctx, native, "zlib", z);
}

static void tv_node_crypto_install(JSContextRef ctx, JSObjectRef native) {
    JSObjectRef c = JSObjectMake(ctx, NULL, NULL);
    set(ctx, c, "available", JSValueMakeBoolean(ctx, tv_crypto != NULL));
    tv_js_def(ctx, c, "digestSize", n_crypto_digest_size);
    tv_js_def(ctx, c, "hashNew", n_crypto_hash_new);
    tv_js_def(ctx, c, "hmacNew", n_crypto_hmac_new);
    tv_js_def(ctx, c, "update", n_crypto_update);
    tv_js_def(ctx, c, "digest", n_crypto_digest);
    tv_js_def(ctx, c, "hashCopy", n_crypto_hash_copy);
    tv_js_def(ctx, c, "randomFill", n_crypto_random_fill);
    tv_js_def(ctx, c, "pbkdf2", n_crypto_pbkdf2);
    tv_js_def(ctx, c, "scrypt", n_crypto_scrypt);
    tv_js_def(ctx, c, "hkdf", n_crypto_hkdf);
    tv_js_def(ctx, c, "timingSafeEqual", n_crypto_equal);
    set(ctx, native, "crypto", c);
}

/* ------------------------------------------------------------------ install */


void tv_node_install(JSContextRef ctx, JSObjectRef native) {
    tv_node_sig_thread = pthread_self();
    tv_js_def(ctx, native, "info", n_info);
    tv_js_def(ctx, native, "cwd", n_cwd);
    tv_js_def(ctx, native, "chdir", n_chdir);
    tv_js_def(ctx, native, "exit", n_exit);
    tv_js_def(ctx, native, "umask", n_umask);
    tv_js_def(ctx, native, "hrtime", n_hrtime);
    tv_js_def(ctx, native, "write", n_write);
    tv_js_def(ctx, native, "isatty", n_isatty);
    tv_js_def(ctx, native, "guessHandleType", n_guess_handle_type);
    tv_js_def(ctx, native, "windowSize", n_window_size);
    tv_js_def(ctx, native, "setResetOnExit", n_set_reset_on_exit);
    tv_js_def(ctx, native, "memoryUsage", n_memory_usage);
    tv_js_def(ctx, native, "cpuUsage", n_cpu_usage);
    tv_js_def(ctx, native, "ids", n_ids);
    tv_js_def(ctx, native, "kill", n_kill);
    tv_js_def(ctx, native, "nowMs", n_now_ms);
    tv_js_def(ctx, native, "typedArrayType", n_typed_array_type);
    tv_js_def(ctx, native, "env", n_env);
    tv_js_def(ctx, native, "setTZ", n_set_tz);
    tv_js_def(ctx, native, "signal", n_signal);
    tv_js_def(ctx, native, "signalHandler", n_signal_handler);

    tv_js_def(ctx, native, "now", n_now);
    tv_js_def(ctx, native, "timerSetup", n_timer_setup);
    tv_js_def(ctx, native, "timerSchedule", n_timer_schedule);
    tv_js_def(ctx, native, "timerRef", n_timer_ref);
    tv_js_def(ctx, native, "requestCheck", n_request_check);

    JSObjectRef os = JSObjectMake(ctx, NULL, NULL);
    tv_js_def(ctx, os, "getOSInformation", n_os_info);
    tv_js_def(ctx, os, "getHostname", n_os_hostname);
    tv_js_def(ctx, os, "getHomeDirectory", n_os_homedir);
    tv_js_def(ctx, os, "getUptime", n_os_uptime);
    tv_js_def(ctx, os, "getTotalMem", n_os_totalmem);
    tv_js_def(ctx, os, "getFreeMem", n_os_freemem);
    tv_js_def(ctx, os, "getLoadAvg", n_os_loadavg);
    tv_js_def(ctx, os, "getCPUs", n_os_cpus);
    tv_js_def(ctx, os, "getInterfaceAddresses", n_os_interfaces);
    tv_js_def(ctx, os, "getUserInfo", n_os_userinfo);
    tv_js_def(ctx, os, "getPriority", n_os_getpriority);
    tv_js_def(ctx, os, "setPriority", n_os_setpriority);
    tv_js_def(ctx, os, "getAvailableParallelism", n_os_parallelism);
    set(ctx, native, "os", os);

    tv_node_fs_install(ctx, native);
    tv_node_crypto_install(ctx, native);
    tv_node_zlib_install(ctx, native);
    tv_node_dns_install(ctx, native);
    tv_node_vm_install(ctx, native);
    tv_js_def(ctx, native, "spawnSync", n_spawn_sync);
    tv_js_def(ctx, native, "setFatalHandler", n_set_fatal_handler);
    tv_js_def(ctx, native, "setExitHandler", n_set_exit_handler);
    tv_js_def(ctx, native, "fatal", n_fatal);
    tv_node_streams_install(ctx, native);
    tv_node_fetch_install(ctx, native);
    tv_node_http_install(ctx, native);
}
