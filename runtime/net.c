/* net.c — TCP and TLS sockets: Bun.connect and Bun.listen (std/http's Socket).
 *
 * A socket is a table entry the program names by id. Its events go to one Tov closure, bound
 * by the Socket object (netBind): open, data (the bytes, copied into a new u8[]), drain, end
 * (the peer finished sending), close, a failed connection, and the TLS handshake's outcome.
 *
 * Writes never block and never come back partial: they go into the socket's output buffer, and
 * the buffer is written once the callback that wrote returns (or before the loop next waits), so
 * many small writes in a callback make one system call. When the kernel can't take everything,
 * the rest waits for the socket to be writable, and `drain` follows once it's all gone.
 *
 * Sockets and listeners keep the program running (tv_io_refs) until they close, or are unref'd.
 * A socket closed while its own events are being delivered is freed after them. This file is
 * part of the runtime's translation unit (after tov.c, whose loop, DNS and TLS hooks it uses). */

#include "tov.h"

enum { TV_NS_RESOLVING, TV_NS_CONNECTING, TV_NS_HANDSHAKE, TV_NS_OPEN, TV_NS_CLOSED };
enum { TV_NE_OPEN = 1, TV_NE_DATA, TV_NE_DRAIN, TV_NE_END, TV_NE_CLOSE, TV_NE_CONNECT_ERROR, TV_NE_HANDSHAKE, TV_NE_ERROR, TV_NE_TIMEOUT };

typedef struct tv_ns {
    tv_io io;
    int fd;
    tv_int id;
    uint8_t state;
    bool bound;              /* cb holds the Socket's closure */
    bool server;             /* accepted by a listener */
    bool refd;               /* counted in tv_io_refs */
    bool watch_read, watch_write, registered;
    bool blocked;            /* the kernel didn't take everything: drain when it has */
    bool ending;             /* end() asked: shut the write side once the buffer is out */
    bool shut;               /* the write side is shut */
    bool peer_ended;         /* the peer finished sending */
    bool half_open;          /* allowHalfOpen: stay open after the peer ends */
    bool dirty;              /* on the flush list */
    bool want_tls;           /* a client that starts TLS once connected */
    bool verify;             /* TLS: reject a certificate that doesn't check out */
    int busy;                /* events being delivered: free afterwards, not now */
    tv_fn cb;
    tv_tls *tls;
    void *tls_server;        /* an accepted TLS socket's listener context */
    char *host;              /* the name connected to (TLS: SNI and verification) */
    char *ca;
    size_t ca_len;
    tv_addrs addrs;
    int ai, port;
    char *out;
    size_t out_len, out_cap, out_off;
    uint64_t timeout_ms, last_active;
    struct tv_ns *dns_next, *dirty_next;
    struct tv_ws *ws;        /* a WebSocket's framing (runtime/ws.c), or NULL */
} tv_ns;

/* (runtime/ws.c, later in the translation unit) */
static void tv_ws_input(tv_ns *s, const uint8_t *data, size_t n);
static bool tv_ws_socket_opened(tv_ns *s);
static void tv_ws_socket_closed(tv_ns *s, const char *err);
static void tv_ws_free(tv_ns *s);
static size_t tv_ws_pending_input(tv_ns *s);

typedef struct tv_nl {
    tv_io io;
    int fd;
    tv_int id;
    int port;
    bool closed, refd;
    tv_fn on_accept;          /* (socket id) => void */
    void *tls;                /* a server TLS context, or NULL */
} tv_nl;

static tv_ns **tv_ns_tab;
static tv_int tv_ns_cap, tv_ns_next = 1;
static tv_nl **tv_nl_tab;
static tv_int tv_nl_cap, tv_nl_next = 1;
static tv_ns *tv_ns_dirty;
static char tv_net_err[256];

static tv_ns *tv_ns_get(tv_int id) { return id > 0 && id < tv_ns_cap ? tv_ns_tab[id] : NULL; }
static tv_nl *tv_nl_get(tv_int id) { return id > 0 && id < tv_nl_cap ? tv_nl_tab[id] : NULL; }

static void tv_net_hooks(void);

static tv_ns *tv_ns_new(void) {
    tv_net_hooks();
    tv_ns *s = tv_alloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->fd = -1;
    /* ids aren't reused: an old id (in a late callback, say) can't reach a new socket */
    if (tv_ns_next >= tv_ns_cap) {
        tv_int cap = tv_ns_cap ? tv_ns_cap * 2 : 64;
        tv_ns_tab = tv_realloc(tv_ns_tab, (size_t)cap * sizeof *tv_ns_tab);
        memset(tv_ns_tab + tv_ns_cap, 0, (size_t)(cap - tv_ns_cap) * sizeof *tv_ns_tab);
        tv_ns_cap = cap;
    }
    s->id = tv_ns_next++;
    tv_ns_tab[s->id] = s;
    s->refd = true;
    tv_io_refs++;
    return s;
}

static void tv_ns_free(tv_ns *s) {
    if (s->ws) tv_ws_free(s);
    if (s->bound) tv_env_release(s->cb.env);
    free(s->out);
    free(s->host);
    free(s->ca);
    tv_ns_tab[s->id] = NULL;
    free(s);
}

/* ---- events */

static void tv_ns_close(tv_ns *s, const char *err);

/* Delivers an event to the Socket (data and text borrowed for the call). */
static void tv_ns_emit(tv_ns *s, int kind, const void *data, size_t n, const char *text) {
    if (!s->bound) return;
    tv_arr bytes = n ? tv_arr_with_capacity(&tv_type_u8, (tv_int)n) : tv_arr_fresh();
    if (n) {
        memcpy(tv_arr_data(bytes), data, n);
        bytes.p->len = (tv_int)n;
    }
    tv_str t = text ? tv_str_from(text, strlen(text)) : TV_EMPTY_STR;
    s->busy++;
    ((void (*)(tv_env *, tv_int, tv_arr, tv_str, tv_int))s->cb.fn)(s->cb.env, kind, bytes, t, 0);
    s->busy--;
    tv_arr_release(bytes, &tv_type_u8);
    tv_str_release(t);
}

static void tv_ns_watch(tv_ns *s, bool read, bool write) {
    if (s->fd < 0 || s->state == TV_NS_CLOSED) return;
    if (s->registered && read == s->watch_read && write == s->watch_write) return;
    s->watch_read = read;
    s->watch_write = write;
    if (!s->registered) {
        s->registered = true;
        tv_io_add(s->fd, &s->io, true, write);
        if (!read) tv_io_set(s->fd, &s->io, false, write);
    } else {
        tv_io_set(s->fd, &s->io, read, write);
    }
}

/* ---- writing */

static void tv_ns_mark(tv_ns *s) {
    if (s->dirty) return;
    s->dirty = true;
    s->dirty_next = tv_ns_dirty;
    tv_ns_dirty = s;
}

static bool tv_ns_flush_held(tv_ns *s);
static void tv_ns_settle(tv_ns *s);

/* Writes out what the buffer holds; false if the socket closed. The socket stays allocated
 * (closed or not) until its caller settles it. */
static bool tv_ns_flush(tv_ns *s) {
    s->busy++;
    bool open = tv_ns_flush_held(s);
    s->busy--;
    return open;
}

static bool tv_ns_flush_held(tv_ns *s) {
    if (s->state != TV_NS_OPEN) return s->state != TV_NS_CLOSED;
    while (s->out_off < s->out_len) {
        size_t n = s->out_len - s->out_off;
        long w;
        if (s->tls) {
            w = tv_tls_impl->write(s->tls, s->out + s->out_off, n);
            if (w == -1 || w == -2) {
                s->blocked = true;
                tv_ns_watch(s, true, true);
                return true;
            }
            if (w <= 0) {
                tv_ns_close(s, "ECONNRESET");
                return false;
            }
        } else {
#ifdef MSG_NOSIGNAL
            w = send(s->fd, s->out + s->out_off, n, MSG_NOSIGNAL);
#else
            w = send(s->fd, s->out + s->out_off, n, 0);
#endif
            if (w < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    s->blocked = true;
                    tv_ns_watch(s, true, true);
                    return true;
                }
                tv_ns_close(s, errno == EPIPE ? "EPIPE" : "ECONNRESET");
                return false;
            }
        }
        s->out_off += (size_t)w;
    }
    s->out_off = s->out_len = 0;
    if (s->out_cap > (1 << 20)) {
        free(s->out);
        s->out = NULL;
        s->out_cap = 0;
    }
    if (s->watch_write) tv_ns_watch(s, true, false);
    if (s->ending && !s->shut) {
        s->shut = true;
        /* end(): all of it is out. Without allowHalfOpen the socket closes now, as Bun's does
         * (its close handler runs before the peer's); with it, only the sending side shuts. */
        if (!s->half_open || s->peer_ended) {
            tv_ns_close(s, NULL);
            return false;
        }
        if (s->tls) tv_tls_impl->close(s->tls, true), s->tls = NULL;
        shutdown(s->fd, SHUT_WR);
    }
    if (s->blocked) {
        s->blocked = false;
        tv_ns_emit(s, TV_NE_DRAIN, NULL, 0, NULL);
    }
    return s->state != TV_NS_CLOSED;
}

static void tv_ns_append(tv_ns *s, const char *p, size_t n) {
    if (s->out_len + n > s->out_cap) {
        if (s->out_off) {
            memmove(s->out, s->out + s->out_off, s->out_len - s->out_off);
            s->out_len -= s->out_off;
            s->out_off = 0;
        }
        if (s->out_len + n > s->out_cap) {
            size_t cap = s->out_cap ? s->out_cap * 2 : 4096;
            while (cap < s->out_len + n) cap *= 2;
            s->out = tv_realloc(s->out, cap);
            s->out_cap = cap;
        }
    }
    memcpy(s->out + s->out_len, p, n);
    s->out_len += n;
}

/* Before the loop waits: whatever was written since. */
static void tv_net_flush_all(void) {
    while (tv_ns_dirty) {
        tv_ns *s = tv_ns_dirty;
        tv_ns_dirty = s->dirty_next;
        s->dirty = false;
        if (s->state != TV_NS_CLOSED) tv_ns_flush(s);
        tv_ns_settle(s);   /* (one closed while on the list, or by its writes, goes here) */
    }
}

/* ---- closing */

/* Closes the socket now (whatever wasn't written is dropped) and tells the Socket, with the
 * error that closed it, if any. Freed once no event of its is being delivered. */
static void tv_ns_close(tv_ns *s, const char *err) {
    if (s->state == TV_NS_CLOSED) return;
    bool was_open = s->state == TV_NS_OPEN || s->state == TV_NS_HANDSHAKE;
    bool connecting = s->state == TV_NS_RESOLVING || s->state == TV_NS_CONNECTING || (s->state == TV_NS_HANDSHAKE && !s->server);
    s->state = TV_NS_CLOSED;
    if (s->tls) {
        tv_tls_impl->close(s->tls, err == NULL);
        s->tls = NULL;
    }
    if (s->fd >= 0) {
        close(s->fd);   /* (closing removes it from kqueue and epoll) */
        s->fd = -1;
    }
    if (s->refd) {
        s->refd = false;
        tv_io_refs--;
    }
    if (s->ws) tv_ws_socket_closed(s, err);
    else if (connecting && err) tv_ns_emit(s, TV_NE_CONNECT_ERROR, NULL, 0, err);
    else if (was_open || err) tv_ns_emit(s, TV_NE_CLOSE, NULL, 0, err);
    if (!s->busy && !s->dirty) tv_ns_free(s);
}

/* A closed socket whose events have all been delivered goes. */
static void tv_ns_settle(tv_ns *s) {
    if (s->state == TV_NS_CLOSED && !s->busy && !s->dirty) tv_ns_free(s);
}

/* ---- reading */

static uint8_t tv_net_buf[65536];

static void tv_ns_read(tv_ns *s) {
    for (int round = 0; round < 8 && s->state == TV_NS_OPEN && !s->peer_ended; round++) {
        long r;
        if (s->tls) {
            r = tv_tls_impl->read(s->tls, tv_net_buf, sizeof tv_net_buf);
            if (r == -1) return;
            if (r == -2) {
                tv_ns_watch(s, true, true);
                return;
            }
            if (r == -3) {
                const char *code = "ECONNRESET";
                char msg[200];
                tv_tls_impl->why(s->tls, NULL, &code, msg, sizeof msg);
                tv_ns_close(s, code);
                return;
            }
        } else {
            r = recv(s->fd, tv_net_buf, sizeof tv_net_buf, 0);
            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                tv_ns_close(s, "ECONNRESET");
                return;
            }
        }
        if (r == 0 && s->ws) {
            tv_ns_close(s, NULL);
            return;
        }
        if (r == 0) {
            s->peer_ended = true;
            tv_ns_emit(s, TV_NE_END, NULL, 0, NULL);
            if (s->state != TV_NS_OPEN) return;
            if (!s->half_open || s->shut) {
                tv_ns_close(s, NULL);
                return;
            }
            tv_ns_watch(s, false, s->watch_write);
            return;
        }
        s->last_active = tv_loop_ms;
        if (s->ws) tv_ws_input(s, tv_net_buf, (size_t)r);
        else tv_ns_emit(s, TV_NE_DATA, tv_net_buf, (size_t)r, NULL);
        if ((size_t)r < sizeof tv_net_buf && !(s->tls && tv_tls_impl->pending(s->tls))) return;
    }
}

/* ---- connecting */

static void tv_ns_opened(tv_ns *s) {
    /* a WebSocket client sends its upgrade request and opens on the 101 */
    if (s->ws && tv_ws_socket_opened(s)) return;
    s->state = TV_NS_OPEN;
    s->last_active = tv_loop_update();
    tv_ns_watch(s, true, s->out_len > s->out_off);
    tv_ns_emit(s, TV_NE_OPEN, NULL, 0, NULL);
    if (s->state == TV_NS_OPEN && s->out_len > s->out_off) tv_ns_mark(s);
    /* a WebSocket upgraded with frames right behind its request */
    if (s->state == TV_NS_OPEN && s->ws && tv_ws_pending_input(s)) tv_ws_input(s, NULL, 0);
}

static void tv_ns_handshake(tv_ns *s) {
    int rc = tv_tls_impl->handshake(s->tls);
    if (rc == 1) { tv_ns_watch(s, true, false); return; }
    if (rc == 2) { tv_ns_watch(s, true, true); return; }
    bool ok = rc == 0 && (s->server || tv_tls_impl->verified(s->tls));
    if (rc < 0 || (!ok && s->verify)) {
        /* "CODE\tmessage": the Socket's error gets both */
        const char *code = rc < 0 ? "ECONNRESET" : "CERT_VERIFY_FAILED";
        char msg[200], text[300];
        tv_tls_impl->why(s->tls, NULL, &code, msg, sizeof msg);
        snprintf(text, sizeof text, "%s\t%s", code, msg);
        tv_ns_emit(s, TV_NE_HANDSHAKE, NULL, 0, text);
        tv_ns_close(s, text);
        return;
    }
    tv_ns_emit(s, TV_NE_HANDSHAKE, NULL, 0, ok ? NULL : "UNABLE_TO_VERIFY_LEAF_SIGNATURE");
    if (s->state == TV_NS_HANDSHAKE) tv_ns_opened(s);
    /* what came in with the handshake's last flight (the peer's first data, read ahead with
     * it) is already off the socket: no event will say it's there */
    if (s->state == TV_NS_OPEN && s->tls && tv_tls_impl->pending(s->tls)) tv_ns_read(s);
}

static void tv_ns_try_connect(tv_ns *s);

/* The TCP connection is up: TLS starts, or the socket opens. */
static void tv_ns_connected(tv_ns *s) {
    int one = 1;
    setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (s->want_tls) {
        if (!tv_tls_impl) {
            tv_ns_close(s, "ERR_TLS_NOT_LINKED");
            return;
        }
        char key[300];
        snprintf(key, sizeof key, "tcp|%s|%d", s->host, s->port);
        s->tls = tv_tls_impl->open(s->fd, s->host, key, s->verify, s->ca ? s->ca : "", s->ca_len);
        if (!s->tls) {
            tv_ns_close(s, "ERR_TLS_INIT");
            return;
        }
        s->state = TV_NS_HANDSHAKE;
        tv_ns_handshake(s);
        return;
    }
    tv_ns_opened(s);
}

static void tv_ns_ready(tv_io *h, bool readable, bool writable, bool broken);

/* Tries the next address; the socket closes with the last error when none is left. */
static void tv_ns_try_connect(tv_ns *s) {
    int err = ECONNREFUSED;
    while (s->ai < s->addrs.n) {
        struct sockaddr_storage *sa = &s->addrs.a[s->ai];
        socklen_t len = s->addrs.len[s->ai];
        s->ai++;
        if (sa->ss_family == AF_INET) ((struct sockaddr_in *)sa)->sin_port = htons((uint16_t)s->port);
        else if (sa->ss_family == AF_INET6) ((struct sockaddr_in6 *)sa)->sin6_port = htons((uint16_t)s->port);
        int fd = socket(sa->ss_family, SOCK_STREAM, 0);
        if (fd < 0) { err = errno; continue; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        int r;
        while ((r = connect(fd, (struct sockaddr *)sa, len)) != 0 && errno == EINTR) {}
        if (r != 0 && errno != EINPROGRESS) {
            err = errno;
            close(fd);
            continue;
        }
        s->fd = fd;
        s->registered = false;
        s->state = TV_NS_CONNECTING;
        if (r == 0) {
            tv_ns_connected(s);
        } else {
            tv_ns_watch(s, false, true);
        }
        return;
    }
    static char msg[64];
    snprintf(msg, sizeof msg, "%s", err == ECONNREFUSED ? "ECONNREFUSED" : err == ETIMEDOUT ? "ETIMEDOUT" : err == ENETUNREACH ? "ENETUNREACH" : err == EHOSTUNREACH ? "EHOSTUNREACH" : "ECONNREFUSED");
    tv_ns_close(s, msg);
}

static void tv_ns_ready(tv_io *h, bool readable, bool writable, bool broken) {
    tv_ns *s = (tv_ns *)h;
    if (s->state == TV_NS_CLOSED) return;
    s->busy++;
    if (s->state == TV_NS_CONNECTING) {
        if (writable || broken) {
            int err = 0;
            socklen_t len = sizeof err;
            if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) err = errno;
            if (err) {
                close(s->fd);
                s->fd = -1;
                s->registered = false;
                tv_ns_try_connect(s);
            } else {
                tv_ns_connected(s);
            }
        }
    } else if (s->state == TV_NS_HANDSHAKE) {
        if (readable || writable || broken) tv_ns_handshake(s);
    } else if (s->state == TV_NS_OPEN) {
        if (writable && s->out_len > s->out_off) tv_ns_flush(s);
        if ((readable || broken) && s->state == TV_NS_OPEN) tv_ns_read(s);
    }
    s->busy--;
    /* what the callbacks wrote goes now */
    if (s->state == TV_NS_OPEN && s->dirty && s->out_len > s->out_off && !s->blocked) tv_ns_flush(s);
    tv_ns_settle(s);
}

/* A DNS lookup for a socket finished (called through tov.c's resolver). */
static void tv_ns_resolved(tv_ns *s, const tv_addrs *addrs, bool failed) {
    if (s->state != TV_NS_RESOLVING) return;
    if (failed || addrs->n == 0) {
        static char msg[300];
        snprintf(msg, sizeof msg, "ENOTFOUND");
        tv_ns_close(s, msg);
        return;
    }
    s->addrs = *addrs;
    s->ai = 0;
    tv_ns_try_connect(s);
}

/* ---- natives */

/* netConnect(host, port, tls, verify, ca, unix) -> socket id; events follow (open, or
 * connectError). tls: 0 plain, 1 TLS. */
tv_int tv_native_netConnect(tv_str host, tv_int port, tv_int tls, bool verify, tv_str ca, tv_str unix_path) {
    tv_ns *s = tv_ns_new();
    s->io.ready = tv_ns_ready;
    s->port = (int)port;
    s->verify = verify;
    s->state = TV_NS_RESOLVING;
    s->want_tls = tls != 0;
    if (tls) {
        s->host = tv_alloc((size_t)host.p->len + 1);
        memcpy(s->host, host.p->data, (size_t)host.p->len + 1);
        if (ca.p->len) {
            s->ca = tv_alloc((size_t)ca.p->len + 1);
            memcpy(s->ca, ca.p->data, (size_t)ca.p->len + 1);
            s->ca_len = (size_t)ca.p->len;
        }
    }
    return s->id;
}

/* (connecting starts once the Socket has bound its events: a failure is reported to it) */
static void tv_ns_start(tv_ns *s, tv_str host, tv_str unix_path) {
    tv_addrs addrs;
    if (unix_path.p->len) {
        struct sockaddr_un *un = (struct sockaddr_un *)&addrs.a[0];
        memset(un, 0, sizeof *un);
        un->sun_family = AF_UNIX;
        if ((size_t)unix_path.p->len >= sizeof un->sun_path) { tv_ns_close(s, "ENAMETOOLONG"); return; }
        memcpy(un->sun_path, unix_path.p->data, (size_t)unix_path.p->len);
        addrs.len[0] = (socklen_t)sizeof *un;
        addrs.n = 1;
        tv_ns_resolved(s, &addrs, false);
        return;
    }
    const char *h = host.p->data;
    if (tv_numeric_host(h, &addrs)) {
        tv_ns_resolved(s, &addrs, false);
        return;
    }
    uint32_t b = tv_hash_cstr(h) % TV_DNS_BUCKETS;
    tv_dns *d = tv_dns_cache[b];
    while (d && strcmp(d->host, h) != 0) d = d->next;
    if (d && !d->resolving && d->expires > tv_loop_update()) {
        tv_ns_resolved(s, &d->addrs, d->err != 0);
        return;
    }
    if (!d) {
        d = tv_alloc(sizeof *d);
        memset(d, 0, sizeof *d);
        size_t hl = strlen(h);
        d->host = tv_alloc(hl + 1);
        memcpy(d->host, h, hl + 1);
        d->next = tv_dns_cache[b];
        tv_dns_cache[b] = d;
    }
    s->dns_next = (tv_ns *)d->net_waiters;
    d->net_waiters = s;
    if (!d->resolving) {
        d->resolving = true;
        tv_dns_queue(d);
    }
}

static void tv_ns_dns_done(tv_dns *d) {
    tv_ns *w = (tv_ns *)d->net_waiters;
    d->net_waiters = NULL;
    while (w) {
        tv_ns *next = w->dns_next;
        w->dns_next = NULL;
        w->busy++;
        tv_ns_resolved(w, &d->addrs, d->err != 0);
        w->busy--;
        tv_ns_settle(w);
        w = next;
    }
}

/* netBind(id, events, host, unix): the Socket's closure for the socket's events; a connecting
 * socket starts connecting now, an accepted one opens (or starts its TLS handshake). */
void tv_native_netBind(tv_int id, tv_fn events, tv_str host, tv_str unix_path) {
    tv_ns *s = tv_ns_get(id);
    if (!s || s->bound) return;
    tv_env_retain(events.env);
    s->cb = events;
    s->bound = true;
    s->busy++;
    if (s->state == TV_NS_RESOLVING) tv_ns_start(s, host, unix_path);
    else if (s->server && s->tls) {
        s->state = TV_NS_HANDSHAKE;
        tv_ns_handshake(s);
    } else if (s->server) tv_ns_opened(s);
    s->busy--;
    tv_ns_settle(s);
}

tv_int tv_native_netWrite(tv_int id, tv_arr bytes) {
    tv_ns *s = tv_ns_get(id);
    if (!s || s->state == TV_NS_CLOSED || s->ending) return -1;
    size_t n = (size_t)bytes.p->len;
    if (n) {
        tv_ns_append(s, tv_arr_data(bytes), n);
        if (s->state == TV_NS_OPEN) tv_ns_mark(s);
    }
    return (tv_int)n;
}

tv_int tv_native_netWriteText(tv_int id, tv_str text) {
    tv_ns *s = tv_ns_get(id);
    if (!s || s->state == TV_NS_CLOSED || s->ending) return -1;
    size_t n = (size_t)text.p->len;
    if (n) {
        tv_ns_append(s, text.p->data, n);
        if (s->state == TV_NS_OPEN) tv_ns_mark(s);
    }
    return (tv_int)n;
}

/* end(): no more writes; the write side shuts once the buffer is out. */
void tv_native_netEnd(tv_int id) {
    tv_ns *s = tv_ns_get(id);
    if (!s || s->state == TV_NS_CLOSED || s->ending) return;
    s->ending = true;
    if (s->state == TV_NS_OPEN) tv_ns_mark(s);
    else if (s->state != TV_NS_HANDSHAKE) tv_ns_close(s, NULL);
}

void tv_native_netFlush(tv_int id) {
    tv_ns *s = tv_ns_get(id);
    if (s && s->state == TV_NS_OPEN && !s->blocked) {
        tv_ns_flush(s);
        tv_ns_settle(s);
    }
}

/* terminate(): closes now, unwritten data dropped. */
void tv_native_netClose(tv_int id) {
    tv_ns *s = tv_ns_get(id);
    if (s) tv_ns_close(s, NULL);
}

void tv_native_netRef(tv_int id, bool ref) {
    tv_ns *s = tv_ns_get(id);
    if (!s || s->state == TV_NS_CLOSED || s->refd == ref) return;
    s->refd = ref;
    tv_io_refs += ref ? 1 : -1;
}

bool tv_native_netNoDelay(tv_int id, bool on) {
    tv_ns *s = tv_ns_get(id);
    if (!s || s->fd < 0) return false;
    int v = on;
    return setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v) == 0;
}

void tv_native_netHalfOpen(tv_int id, bool on) {
    tv_ns *s = tv_ns_get(id);
    if (s) s->half_open = on;
}

/* netAddress(id, which): 0 remote address, 1 remote port, 2 local address, 3 local port */
tv_str tv_native_netAddress(tv_int id, tv_int which) {
    tv_ns *s = tv_ns_get(id);
    if (!s || s->fd < 0) return TV_EMPTY_STR;
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    int r = which < 2 ? getpeername(s->fd, (struct sockaddr *)&ss, &len) : getsockname(s->fd, (struct sockaddr *)&ss, &len);
    if (r != 0) return TV_EMPTY_STR;
    char buf[INET6_ADDRSTRLEN + 8];
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&ss;
        if (which & 1) snprintf(buf, sizeof buf, "%d", ntohs(v4->sin_port));
        else inet_ntop(AF_INET, &v4->sin_addr, buf, sizeof buf);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&ss;
        if (which & 1) snprintf(buf, sizeof buf, "%d", ntohs(v6->sin6_port));
        else inet_ntop(AF_INET6, &v6->sin6_addr, buf, sizeof buf);
    } else {
        return TV_EMPTY_STR;
    }
    return tv_str_from(buf, strlen(buf));
}

tv_int tv_native_netBuffered(tv_int id) {
    tv_ns *s = tv_ns_get(id);
    return s ? (tv_int)(s->out_len - s->out_off) : 0;
}

/* ---- listening */

static void tv_nl_ready(tv_io *h, bool readable, bool writable, bool broken) {
    (void)writable; (void)broken;
    tv_nl *l = (tv_nl *)h;
    if (!readable || l->closed) return;
    for (int i = 0; i < 128 && !l->closed; i++) {
#ifdef __linux__
        int fd = accept4(l->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        int fd = accept(l->fd, NULL, NULL);
#endif
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return;
        }
#ifndef __linux__
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
#ifdef SO_NOSIGPIPE
        int nsp = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nsp, sizeof nsp);
#endif
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        tv_ns *s = tv_ns_new();
        s->io.ready = tv_ns_ready;
        s->fd = fd;
        s->server = true;
        s->state = TV_NS_CONNECTING;   /* (until it's bound: then it opens) */
        if (l->tls) {
            s->tls = tv_tls_impl->accept(fd, l->tls);
            if (!s->tls) { tv_ns_close(s, "ERR_TLS_INIT"); continue; }
        }
        /* the listener's Socket for it binds its events (netBind), which opens it (the socket
         * may close in there: a TLS handshake that fails at once; it's freed below, not then) */
        s->busy++;
        ((void (*)(tv_env *, tv_int))l->on_accept.fn)(l->on_accept.env, s->id);
        s->busy--;
        if (s->state != TV_NS_CLOSED && !s->bound) tv_ns_close(s, NULL);
        tv_ns_settle(s);
    }
}

/* netListen(host, port, cert, key, onAccept) -> listener id, or -1 (the reason in takeError). */
tv_int tv_native_netListen(tv_str host, tv_int port, tv_str cert, tv_str key, tv_str unix_path, tv_fn on_accept) {
    tv_net_hooks();
    void *tls = NULL;
    if (cert.p->len || key.p->len) {
        if (!tv_tls_impl) { snprintf(tv_net_err, sizeof tv_net_err, "TLS isn't linked into this program"); return -1; }
        char err[200];
        tls = tv_tls_impl->server(cert.p->data, (size_t)cert.p->len, key.p->data, (size_t)key.p->len, err, sizeof err);
        if (!tls) { snprintf(tv_net_err, sizeof tv_net_err, "%s", err); return -1; }
    }
    int fd;
    if (unix_path.p->len) {
        struct sockaddr_un un;
        memset(&un, 0, sizeof un);
        un.sun_family = AF_UNIX;
        if ((size_t)unix_path.p->len >= sizeof un.sun_path) { snprintf(tv_net_err, sizeof tv_net_err, "the socket path is too long"); return -1; }
        memcpy(un.sun_path, unix_path.p->data, (size_t)unix_path.p->len);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        unlink(un.sun_path);
        if (fd < 0 || bind(fd, (struct sockaddr *)&un, sizeof un) != 0 || listen(fd, 1024) != 0) {
            snprintf(tv_net_err, sizeof tv_net_err, "Failed to listen at %s", un.sun_path);
            if (fd >= 0) close(fd);
            return -1;
        }
    } else {
        tv_addrs addrs;
        const char *h = host.p->len ? host.p->data : "0.0.0.0";
        struct sockaddr_storage ss;
        socklen_t len;
        if (tv_numeric_host(h, &addrs) || (strcmp(h, "0.0.0.0") == 0)) {
            if (strcmp(h, "0.0.0.0") == 0) {
                struct sockaddr_in *v4 = (struct sockaddr_in *)&ss;
                memset(v4, 0, sizeof *v4);
                v4->sin_family = AF_INET;
                v4->sin_addr.s_addr = htonl(INADDR_ANY);
                len = sizeof *v4;
            } else {
                memcpy(&ss, &addrs.a[0], addrs.len[0]);
                len = addrs.len[0];
            }
        } else {
            struct addrinfo hints, *res = NULL;
            memset(&hints, 0, sizeof hints);
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_PASSIVE;
            if (getaddrinfo(h, NULL, &hints, &res) != 0 || !res) {
                snprintf(tv_net_err, sizeof tv_net_err, "getaddrinfo ENOTFOUND %s", h);
                return -1;
            }
            memcpy(&ss, res->ai_addr, res->ai_addrlen);
            len = res->ai_addrlen;
            freeaddrinfo(res);
        }
        if (ss.ss_family == AF_INET) ((struct sockaddr_in *)&ss)->sin_port = htons((uint16_t)port);
        else ((struct sockaddr_in6 *)&ss)->sin6_port = htons((uint16_t)port);
        fd = socket(ss.ss_family, SOCK_STREAM, 0);
        int one = 1;
        if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (fd < 0 || bind(fd, (struct sockaddr *)&ss, len) != 0 || listen(fd, 1024) != 0) {
            int e = errno;
            snprintf(tv_net_err, sizeof tv_net_err, "Failed to listen at %s:%d (%s)", h, (int)port, e == EADDRINUSE ? "EADDRINUSE" : e == EACCES ? "EACCES" : "EINVAL");
            if (fd >= 0) close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    tv_nl *l = tv_alloc(sizeof *l);
    memset(l, 0, sizeof *l);
    l->io.ready = tv_nl_ready;
    l->fd = fd;
    l->tls = tls;
    tv_env_retain(on_accept.env);
    l->on_accept = on_accept;
    struct sockaddr_storage bound;
    socklen_t bl = sizeof bound;
    if (getsockname(fd, (struct sockaddr *)&bound, &bl) == 0) {
        if (bound.ss_family == AF_INET) l->port = ntohs(((struct sockaddr_in *)&bound)->sin_port);
        else if (bound.ss_family == AF_INET6) l->port = ntohs(((struct sockaddr_in6 *)&bound)->sin6_port);
    }
    if (tv_nl_next >= tv_nl_cap) {
        tv_int cap = tv_nl_cap ? tv_nl_cap * 2 : 16;
        tv_nl_tab = tv_realloc(tv_nl_tab, (size_t)cap * sizeof *tv_nl_tab);
        memset(tv_nl_tab + tv_nl_cap, 0, (size_t)(cap - tv_nl_cap) * sizeof *tv_nl_tab);
        tv_nl_cap = cap;
    }
    l->id = tv_nl_next++;
    tv_nl_tab[l->id] = l;
    l->refd = true;
    tv_io_refs++;
    tv_io_add(fd, &l->io, true, false);
    return l->id;
}

tv_int tv_native_netListenPort(tv_int id) {
    tv_nl *l = tv_nl_get(id);
    return l ? l->port : 0;
}

/* stop(): no more connections; open ones stay (unless closeActive). */
void tv_native_netStop(tv_int id) {
    tv_nl *l = tv_nl_get(id);
    if (!l || l->closed) return;
    l->closed = true;
    close(l->fd);
    if (l->refd) {
        l->refd = false;
        tv_io_refs--;
    }
    tv_env_release(l->on_accept.env);
    tv_nl_tab[id] = NULL;
    /* (the listener struct stays: an event for it may still be in this batch) */
}

void tv_native_netListenRef(tv_int id, bool ref) {
    tv_nl *l = tv_nl_get(id);
    if (!l || l->closed || l->refd == ref) return;
    l->refd = ref;
    tv_io_refs += ref ? 1 : -1;
}

tv_str tv_native_netError(void) { return tv_str_from(tv_net_err, strlen(tv_net_err)); }

/* ---- the loop's hooks */

static void tv_net_hooks(void) {
    static bool done;
    if (done) return;
    done = true;
    tv_loop_before_wait_net = tv_net_flush_all;
    tv_dns_net_done = tv_ns_dns_done;
}
