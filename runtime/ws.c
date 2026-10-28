/* ws.c — WebSocket (RFC 6455) on net.c's sockets: Bun.serve's `websocket` and `WebSocket`.
 *
 * A WebSocket is a net.c socket with a framer on it. The server side is an HTTP connection taken
 * over at `server.upgrade(req)` (the 101 response goes out first); a client is a socket that sends
 * the upgrade request once TCP (and TLS) is up and opens on the server's 101. Frames are parsed
 * straight from what was read (only a partial frame is kept), text is checked to be UTF-8, pings
 * are answered, and a close is answered with a close.
 *
 * Writes are frames appended to the socket's output buffer, written when the handler returns
 * (net.c), so a handler's sends go out in one system call. Topics live here too: publish builds
 * a frame once and appends it to every subscriber's buffer.
 *
 * Part of the runtime's translation unit, after net.c (and tov.c, whose HTTP server it takes
 * connections from). */

#include "tov.h"

enum { TV_WE_TEXT = 20, TV_WE_BINARY, TV_WE_PING, TV_WE_PONG, TV_WE_CLOSE, TV_WE_ERROR };

typedef struct tv_ws {
    bool client;
    uint8_t phase;            /* 0 waiting for the 101 (a client), 1 open, 2 close sent, 3 done */
    bool close_told;          /* the close event went to the program */
    char accept[32];          /* a client: the Sec-WebSocket-Accept to expect */
    uint8_t *in;              /* bytes of a frame not all here yet */
    size_t in_len, in_cap;
    uint8_t *msg;             /* a fragmented message so far */
    size_t msg_len, msg_cap;
    uint8_t msg_op;           /* its opcode (0: none in progress) */
    size_t max_payload;
    struct tv_topic **subs;   /* topics subscribed to */
    int nsubs, subs_cap;
    char *protocol;           /* the subprotocol agreed on */
    char *request;            /* a client: the upgrade request, sent when the socket opens */
    int close_code;           /* the close to tell the program once the socket has closed */
    char *close_reason;
    size_t close_reason_len;
} tv_ws;

/* ---------------------------------------------------------------- SHA-1 (the accept key) */

static void tv_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t block[64];
    uint64_t bits = (uint64_t)len * 8;
    size_t total = ((len + 8) / 64 + 1) * 64;
    for (size_t off = 0; off < total; off += 64) {
        for (size_t i = 0; i < 64; i++) {
            size_t k = off + i;
            block[i] = k < len ? data[k] : k == len ? 0x80 : k >= total - 8 ? (uint8_t)(bits >> (8 * (total - 1 - k))) : 0;
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 | (uint32_t)block[4 * i + 2] << 8 | block[4 * i + 3];
        for (int i = 16; i < 80; i++) {
            uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = v << 1 | v >> 31;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
            e = d; d = c; c = b << 30 | b >> 2; b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

/* base64(sha1(key + the RFC's GUID)) into out (29 bytes with its NUL) */
static void tv_ws_accept_for(const char *key, size_t klen, char out[32]) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char buf[128];
    if (klen > sizeof buf - sizeof guid) klen = sizeof buf - sizeof guid;
    memcpy(buf, key, klen);
    memcpy(buf + klen, guid, sizeof guid - 1);
    uint8_t digest[20];
    tv_sha1((const uint8_t *)buf, klen + sizeof guid - 1, digest);
    tv_sb sb = {0};
    tv_base64(&sb, (const char *)digest, 20);
    size_t n = sb.len < 31 ? sb.len : 31;
    memcpy(out, sb.data, n);
    out[n] = 0;
    tv_sb_free(&sb);
}

/* ---------------------------------------------------------------- randomness (masks, keys) */

#if defined(__APPLE__)
static void tv_ws_random(void *p, size_t n) { arc4random_buf(p, n); }
#else
#include <sys/random.h>
static void tv_ws_random(void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        ssize_t r = getrandom(b, n, 0);
        if (r <= 0) { if (errno == EINTR) continue; break; }
        b += r;
        n -= (size_t)r;
    }
}
#endif

/* Masks for a client's frames: from a generator seeded from the system (a mask only has to be
 * unpredictable to whatever sits between client and server). */
static uint64_t tv_ws_mask_state;
static uint32_t tv_ws_next_mask(void) {
    if (!tv_ws_mask_state) {
        tv_ws_random(&tv_ws_mask_state, sizeof tv_ws_mask_state);
        tv_ws_mask_state |= 1;
    }
    uint64_t x = tv_ws_mask_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    tv_ws_mask_state = x;
    return (uint32_t)(x * 0x2545F4914F6CDD1Dull >> 32);
}

/* p[0..n) ^= the mask, starting at mask byte `phase` (masking is its own inverse) */
static void tv_ws_unmask(uint8_t *p, size_t n, const uint8_t key[4]) {
    uint64_t k8;
    uint8_t kb[8] = { key[0], key[1], key[2], key[3], key[0], key[1], key[2], key[3] };
    memcpy(&k8, kb, 8);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        v ^= k8;
        memcpy(p + i, &v, 8);
    }
    for (; i < n; i++) p[i] ^= key[i & 3];
}

/* ---------------------------------------------------------------- sending */

static size_t tv_ws_header(uint8_t *h, int op, size_t n, bool fin) {
    size_t k = 0;
    h[k++] = (uint8_t)((fin ? 0x80 : 0) | op);
    if (n < 126) {
        h[k++] = (uint8_t)n;
    } else if (n < 65536) {
        h[k++] = 126;
        h[k++] = (uint8_t)(n >> 8);
        h[k++] = (uint8_t)n;
    } else {
        h[k++] = 127;
        for (int i = 7; i >= 0; i--) h[k++] = (uint8_t)((uint64_t)n >> (8 * i));
    }
    return k;
}

/* Appends a frame to the socket's output (masked when we're the client). */
static void tv_ws_frame(tv_ns *s, int op, const void *p, size_t n) {
    uint8_t h[14];
    size_t hl = tv_ws_header(h, op, n, true);
    if (!s->ws->client) {
        if (s->out_len + hl + n > s->out_cap || s->out_off) {
            tv_ns_append(s, (const char *)h, hl);
            if (n) tv_ns_append(s, p, n);
        } else {
            memcpy(s->out + s->out_len, h, hl);
            if (n) memcpy(s->out + s->out_len + hl, p, n);
            s->out_len += hl + n;
        }
    } else {
        h[1] |= 0x80;
        uint32_t m = tv_ws_next_mask();
        uint8_t key[4];
        memcpy(key, &m, 4);
        memcpy(h + hl, key, 4);
        hl += 4;
        tv_ns_append(s, (const char *)h, hl);
        if (n) {
            size_t at = s->out_len;
            tv_ns_append(s, p, n);
            tv_ws_unmask((uint8_t *)s->out + at, n, key);
        }
    }
    if (s->state == TV_NS_OPEN) tv_ns_mark(s);
}

/* ---------------------------------------------------------------- topics */

typedef struct tv_topic {
    struct tv_topic *next;
    char *name;
    size_t len;
    tv_ns **subs;
    int n, cap;
} tv_topic;

#define TV_TOPIC_BUCKETS 256
static tv_topic *tv_topics[TV_TOPIC_BUCKETS];

static tv_topic *tv_topic_get(const char *name, size_t len, bool create) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)name[i]) * 16777619u;
    tv_topic **b = &tv_topics[h % TV_TOPIC_BUCKETS];
    for (tv_topic *t = *b; t; t = t->next)
        if (t->len == len && memcmp(t->name, name, len) == 0) return t;
    if (!create) return NULL;
    tv_topic *t = tv_alloc(sizeof *t);
    memset(t, 0, sizeof *t);
    t->name = tv_alloc(len + 1);
    memcpy(t->name, name, len);
    t->name[len] = 0;
    t->len = len;
    t->next = *b;
    *b = t;
    return t;
}

static bool tv_ws_subscribed(tv_ws *w, tv_topic *t) {
    for (int i = 0; i < w->nsubs; i++)
        if (w->subs[i] == t) return true;
    return false;
}

static void tv_ws_unsub(tv_ns *s, tv_topic *t) {
    tv_ws *w = s->ws;
    for (int i = 0; i < w->nsubs; i++) {
        if (w->subs[i] != t) continue;
        w->subs[i] = w->subs[--w->nsubs];
        for (int j = 0; j < t->n; j++) {
            if (t->subs[j] == s) {
                t->subs[j] = t->subs[--t->n];
                break;
            }
        }
        return;
    }
}

/* ---------------------------------------------------------------- receiving */

static void tv_ws_emit(tv_ns *s, int kind, const uint8_t *p, size_t n, bool text, int code) {
    if (!s->bound) return;
    tv_arr bytes = !text && n ? tv_arr_with_capacity(&tv_type_u8, (tv_int)n) : tv_arr_fresh();
    tv_str t = TV_EMPTY_STR;
    if (text) {
        t = tv_str_from((const char *)p, n);
    } else if (n) {
        memcpy(tv_arr_data(bytes), p, n);
        bytes.p->len = (tv_int)n;
    }
    s->busy++;
    ((void (*)(tv_env *, tv_int, tv_arr, tv_str, tv_int))s->cb.fn)(s->cb.env, kind, bytes, t, code);
    s->busy--;
    tv_arr_release(bytes, &tv_type_u8);
    tv_str_release(t);
}

/* The connection is finished: the program hears its close (once) and the socket closes. */
static void tv_ws_finish(tv_ns *s, int code, const char *reason, size_t rlen) {
    tv_ws *w = s->ws;
    w->phase = 3;
    /* the program hears it once the connection has closed (after our answer is out) */
    if (!w->close_told && !w->close_code) {
        w->close_code = code;
        w->close_reason = tv_alloc(rlen + 1);
        memcpy(w->close_reason, reason, rlen);
        w->close_reason_len = rlen;
    }
    /* what's queued (our close frame) goes out first */
    if (s->state == TV_NS_OPEN && s->out_len > s->out_off) {
        s->ending = true;
        s->half_open = false;
        tv_ns_mark(s);
    } else {
        tv_ns_close(s, NULL);
    }
}

/* A protocol error: close with `code` (1002, 1007, 1009) and stop reading. */
static void tv_ws_fail(tv_ns *s, int code, const char *why) {
    tv_ws *w = s->ws;
    if (w->phase == 1) {
        uint8_t body[2] = { (uint8_t)(code >> 8), (uint8_t)code };
        tv_ws_frame(s, 8, body, 2);
    }
    tv_ws_finish(s, code, why, strlen(why));
}

static bool tv_ws_utf8(const uint8_t *p, size_t n) {
    size_t pos = 0;
    return tv_utf8_scan(p, n, &pos) && pos == n;
}

/* One complete message (or control frame). */
static void tv_ws_message(tv_ns *s, int op, uint8_t *p, size_t n) {
    tv_ws *w = s->ws;
    switch (op) {
    case 1:
        if (!tv_ws_utf8(p, n)) { tv_ws_fail(s, 1007, "Invalid UTF-8"); return; }
        tv_ws_emit(s, TV_WE_TEXT, p, n, true, 0);
        return;
    case 2:
        tv_ws_emit(s, TV_WE_BINARY, p, n, false, 0);
        return;
    case 8: {
        int code = 1005;
        const char *reason = "";
        size_t rlen = 0;
        if (n == 1) { tv_ws_fail(s, 1002, "Invalid close frame"); return; }
        if (n >= 2) {
            code = p[0] << 8 | p[1];
            reason = (const char *)p + 2;
            rlen = n - 2;
            bool valid = (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999);
            if (!valid) { tv_ws_fail(s, 1002, "Invalid close code"); return; }
            if (!tv_ws_utf8((const uint8_t *)reason, rlen)) { tv_ws_fail(s, 1007, "Invalid UTF-8"); return; }
        }
        if (w->phase == 1) {
            /* answer with the same code and reason (as Bun does) */
            tv_ws_frame(s, 8, p, code == 1005 ? 0 : n);
        }
        tv_ws_finish(s, code, reason, rlen);
        return;
    }
    case 9:
        tv_ws_frame(s, 10, p, n);
        tv_ws_emit(s, TV_WE_PING, p, n, false, 0);
        return;
    case 10:
        tv_ws_emit(s, TV_WE_PONG, p, n, false, 0);
        return;
    }
}

static void tv_ws_keep(tv_ws *w, const uint8_t *p, size_t n) {
    if (w->in_len + n > w->in_cap) {
        size_t cap = w->in_cap ? w->in_cap * 2 : 4096;
        while (cap < w->in_len + n) cap *= 2;
        w->in = tv_realloc(w->in, cap);
        w->in_cap = cap;
    }
    memcpy(w->in + w->in_len, p, n);
    w->in_len += n;
}

/* The client's handshake: the server's response, up to its blank line. Returns how many bytes it
 * took (0: not all here yet), or -1 if it isn't a 101 with the right accept key. */
static long tv_ws_response(tv_ns *s, const uint8_t *p, size_t n) {
    tv_ws *w = s->ws;
    const uint8_t *end = NULL;
    for (size_t i = 3; i < n; i++) {
        if (p[i] == '\n' && p[i - 1] == '\r' && p[i - 2] == '\n' && p[i - 3] == '\r') { end = p + i + 1; break; }
    }
    if (!end) return n > 16384 ? -1 : 0;
    if (n < 12 || memcmp(p, "HTTP/1.1 101", 12) != 0) return -1;
    bool accepted = false;
    for (const uint8_t *q = p; q < end;) {
        const uint8_t *nl = memchr(q, '\n', (size_t)(end - q));
        if (!nl) break;
        size_t len = (size_t)(nl - q);
        if (len > 21 && strncasecmp((const char *)q, "sec-websocket-accept:", 21) == 0) {
            const char *v = (const char *)q + 21, *e = (const char *)nl;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            while (e > v && (e[-1] == '\r' || e[-1] == ' ')) e--;
            accepted = (size_t)(e - v) == strlen(w->accept) && memcmp(v, w->accept, (size_t)(e - v)) == 0;
        } else if (len > 23 && strncasecmp((const char *)q, "sec-websocket-protocol:", 23) == 0) {
            const char *v = (const char *)q + 23, *e = (const char *)nl;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            while (e > v && (e[-1] == '\r' || e[-1] == ' ')) e--;
            free(w->protocol);
            w->protocol = tv_alloc((size_t)(e - v) + 1);
            memcpy(w->protocol, v, (size_t)(e - v));
            w->protocol[e - v] = 0;
        }
        q = nl + 1;
    }
    return accepted ? (long)(end - p) : -1;
}

/* Bytes read from a WebSocket's socket: frames, handled as they complete. */
static void tv_ws_input(tv_ns *s, const uint8_t *data, size_t n) {
    tv_ws *w = s->ws;
    uint8_t *buf;
    size_t len;
    bool kept = w->in_len > 0;
    if (kept) {
        if (n) tv_ws_keep(w, data, n);
        buf = w->in;
        len = w->in_len;
    } else {
        buf = (uint8_t *)data;   /* (net.c's read buffer: unmasked in place) */
        len = n;
    }
    size_t off = 0;
    if (w->phase == 0) {
        long took = tv_ws_response(s, buf, len);
        if (took < 0) {
            tv_ws_emit(s, TV_WE_ERROR, (const uint8_t *)"Unexpected server response", 26, true, 0);
            w->close_told = true;
            tv_ws_emit(s, TV_WE_CLOSE, NULL, 0, true, 1006);
            tv_ns_close(s, NULL);
            return;
        }
        if (took == 0) {
            if (!kept) tv_ws_keep(w, buf, len);
            return;
        }
        off = (size_t)took;
        w->phase = 1;
        tv_ns_emit(s, TV_NE_OPEN, NULL, 0, w->protocol);
    }
    while (s->state == TV_NS_OPEN && (w->phase == 1 || w->phase == 2)) {
        size_t avail = len - off;
        if (avail < 2) break;
        uint8_t b0 = buf[off], b1 = buf[off + 1];
        int op = b0 & 0x0F;
        bool fin = (b0 & 0x80) != 0, masked = (b1 & 0x80) != 0;
        uint64_t plen = b1 & 0x7F;
        size_t hl = 2;
        if (plen == 126) {
            if (avail < 4) break;
            plen = (uint64_t)buf[off + 2] << 8 | buf[off + 3];
            hl = 4;
        } else if (plen == 127) {
            if (avail < 10) break;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = plen << 8 | buf[off + 2 + i];
            hl = 10;
        }
        if (b0 & 0x70) { tv_ws_fail(s, 1002, "Reserved bits set"); return; }
        if (masked == w->client) { tv_ws_fail(s, 1002, w->client ? "Masked frame from the server" : "Unmasked frame from the client"); return; }
        if (op >= 8 && (!fin || plen > 125)) { tv_ws_fail(s, 1002, "Invalid control frame"); return; }
        if ((op > 2 && op < 8) || op > 10) { tv_ws_fail(s, 1002, "Unknown opcode"); return; }
        if (plen > w->max_payload || (op < 8 && w->msg_len + plen > w->max_payload)) { tv_ws_fail(s, 1009, "Message too big"); return; }
        uint8_t key[4] = {0};
        if (masked) {
            if (avail < hl + 4) break;
            memcpy(key, buf + off + hl, 4);
            hl += 4;
        }
        if (avail - hl < plen) break;
        uint8_t *p = buf + off + hl;
        if (masked) tv_ws_unmask(p, (size_t)plen, key);
        off += hl + (size_t)plen;
        if (op >= 8) {
            tv_ws_message(s, op, p, (size_t)plen);
            continue;
        }
        if (op == 0) {
            if (!w->msg_op) { tv_ws_fail(s, 1002, "Unexpected continuation frame"); return; }
        } else if (w->msg_op) {
            tv_ws_fail(s, 1002, "Expected a continuation frame");
            return;
        }
        if (fin && op != 0) {
            tv_ws_message(s, op, p, (size_t)plen);
            continue;
        }
        /* a fragment: kept until the final one */
        if (op != 0) w->msg_op = (uint8_t)op;
        if (w->msg_len + plen > w->msg_cap) {
            size_t cap = w->msg_cap ? w->msg_cap * 2 : 4096;
            while (cap < w->msg_len + plen) cap *= 2;
            w->msg = tv_realloc(w->msg, cap);
            w->msg_cap = cap;
        }
        memcpy(w->msg + w->msg_len, p, (size_t)plen);
        w->msg_len += (size_t)plen;
        if (fin) {
            int mop = w->msg_op;
            size_t mlen = w->msg_len;
            w->msg_op = 0;
            w->msg_len = 0;
            tv_ws_message(s, mop, w->msg, mlen);
        }
    }
    /* what's left (a partial frame) waits for more */
    if (s->state == TV_NS_CLOSED) return;
    if (kept) {
        if (off) {
            memmove(w->in, w->in + off, w->in_len - off);
            w->in_len -= off;
        }
    } else if (off < len) {
        tv_ws_keep(w, buf + off, len - off);
    }
}

/* The socket opened: a client sends its upgrade request (it opens on the 101). */
static bool tv_ws_socket_opened(tv_ns *s) {
    tv_ws *w = s->ws;
    if (!w || !w->client || w->phase != 0) return false;
    s->state = TV_NS_OPEN;
    s->last_active = tv_loop_update();
    tv_ns_watch(s, true, false);
    if (w->request) {
        tv_ns_append(s, w->request, strlen(w->request));
        free(w->request);
        w->request = NULL;
        tv_ns_mark(s);
    }
    return true;
}

/* The socket closed under the WebSocket (net.c): the program hears 1006 unless it heard a close. */
static void tv_ws_socket_closed(tv_ns *s, const char *err) {
    tv_ws *w = s->ws;
    if (w->close_told) return;
    w->close_told = true;
    if (w->close_code) {
        tv_ws_emit(s, TV_WE_CLOSE, (const uint8_t *)w->close_reason, w->close_reason_len, true, w->close_code);
        return;
    }
    if (w->phase == 0 && err) tv_ws_emit(s, TV_WE_ERROR, (const uint8_t *)err, strlen(err), true, 0);
    tv_ws_emit(s, TV_WE_CLOSE, NULL, 0, true, 1006);
}

static void tv_ws_free(tv_ns *s) {
    tv_ws *w = s->ws;
    while (w->nsubs) tv_ws_unsub(s, w->subs[0]);
    free(w->subs);
    free(w->in);
    free(w->msg);
    free(w->protocol);
    free(w->request);
    free(w->close_reason);
    free(w);
    s->ws = NULL;
}

static size_t tv_ws_pending_input(tv_ns *s) { return s->ws ? s->ws->in_len : 0; }

static tv_ws *tv_ws_new(tv_ns *s, bool client, tv_int max_payload) {
    tv_ws *w = tv_alloc(sizeof *w);
    memset(w, 0, sizeof *w);
    w->client = client;
    w->phase = client ? 0 : 1;
    w->max_payload = max_payload > 0 ? (size_t)max_payload : (size_t)16 << 20;
    s->ws = w;
    return w;
}

/* ---------------------------------------------------------------- natives */

/* wsUpgrade(key, headers, maxPayload): turns the request being handled into a WebSocket (the 101
 * response, with any extra header lines, goes out first). The socket id, or -1 (it isn't an
 * upgrade request, or the connection has responses still owed). */
tv_int tv_native_wsUpgrade(tv_str key, tv_str headers, tv_int max_payload) {
    tv_http_conn *c = tv_http_cur;
    if (!c || c->npending || c->taken || key.p->len == 0 || key.p->len > 64) return -1;
    char accept[32];
    tv_ws_accept_for(key.p->data, (size_t)key.p->len, accept);
    tv_ns *s = tv_ns_new();
    s->io.ready = tv_ns_ready;
    s->fd = c->fd;
    s->server = true;
    s->state = TV_NS_CONNECTING;   /* (opens when it's bound) */
    tv_ws_new(s, false, max_payload);
    /* responses to earlier requests on the connection not yet written go first */
    if (c->out_len > c->out_off) tv_ns_append(s, c->out + c->out_off, c->out_len - c->out_off);
    static const char head[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
    tv_ns_append(s, head, sizeof head - 1);
    tv_ns_append(s, accept, strlen(accept));
    tv_ns_append(s, "\r\n", 2);
    if (headers.p->len) tv_ns_append(s, headers.p->data, (size_t)headers.p->len);
    tv_ns_append(s, "\r\n", 2);
    /* frames sent right behind the request */
    if (tv_http_rest_off < c->in_len) tv_ws_keep(s->ws, (const uint8_t *)c->in + tv_http_rest_off, c->in_len - tv_http_rest_off);
    /* the server lets the connection go (its fd is the socket's now) */
    c->in_len = 0;
    c->out_len = c->out_off = 0;
    c->taken = true;
#ifdef TV_KQUEUE
    struct kevent ev[2];
    EV_SET(&ev[0], c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ev[1], c->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(tv_http_q, ev, 1 + (c->want_write ? 1 : 0), NULL, 0, NULL);
#else
    epoll_ctl(tv_http_q, EPOLL_CTL_DEL, c->fd, NULL);
#endif
    c->want_read = c->want_write = false;
    tv_http_cur = NULL;
    return s->id;
}

/* wsConnect(host, port, path, tls, verify, ca, headers, protocols, maxPayload): a client socket
 * (netBind starts it); it opens once the server's 101 is in. */
tv_int tv_native_wsConnect(tv_str host, tv_int port, tv_str path, bool tls, bool verify, tv_str ca, tv_str headers, tv_str protocols, tv_int max_payload) {
    tv_int id = tv_native_netConnect(host, port, tls ? 1 : 0, verify, ca, TV_EMPTY_STR);
    tv_ns *s = tv_ns_get(id);
    tv_ws *w = tv_ws_new(s, true, max_payload);
    uint8_t nonce[16];
    tv_ws_random(nonce, sizeof nonce);
    tv_sb key = {0};
    tv_base64(&key, (const char *)nonce, sizeof nonce);
    tv_ws_accept_for(key.data, key.len, w->accept);
    tv_sb req = {0};
    tv_sb_push_cstr(&req, "GET ");
    tv_sb_push(&req, path.p->data, (size_t)path.p->len);
    tv_sb_push_cstr(&req, " HTTP/1.1\r\nHost: ");
    tv_sb_push(&req, host.p->data, (size_t)host.p->len);
    bool default_port = port == (tls ? 443 : 80);
    if (!default_port) {
        tv_sb_push_char(&req, ':');
        tv_sb_push_int(&req, port);
    }
    tv_sb_push_cstr(&req, "\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ");
    tv_sb_push(&req, key.data, key.len);
    tv_sb_push_cstr(&req, "\r\n");
    if (protocols.p->len) {
        tv_sb_push_cstr(&req, "Sec-WebSocket-Protocol: ");
        tv_sb_push(&req, protocols.p->data, (size_t)protocols.p->len);
        tv_sb_push_cstr(&req, "\r\n");
    }
    if (headers.p->len) tv_sb_push(&req, headers.p->data, (size_t)headers.p->len);
    tv_sb_push_cstr(&req, "\r\n");
    tv_sb_push_char(&req, 0);
    w->request = tv_alloc(req.len);
    memcpy(w->request, req.data, req.len);
    tv_sb_free(&req);
    tv_sb_free(&key);
    return id;
}

static tv_ns *tv_ws_get(tv_int id) {
    tv_ns *s = tv_ns_get(id);
    return s && s->ws ? s : NULL;
}

/* send: op 1 text, 2 binary, 9 ping, 10 pong. The bytes queued, or -1 if it's closing or closed. */
tv_int tv_native_wsSendText(tv_int id, tv_str text, tv_int op) {
    tv_ns *s = tv_ws_get(id);
    if (!s || s->ws->phase != 1 || s->state == TV_NS_CLOSED) return -1;
    if (op >= 8 && text.p->len > 125) return -1;
    tv_ws_frame(s, (int)op, text.p->data, (size_t)text.p->len);
    return text.p->len;
}

tv_int tv_native_wsSendBytes(tv_int id, tv_arr bytes, tv_int op) {
    tv_ns *s = tv_ws_get(id);
    if (!s || s->ws->phase != 1 || s->state == TV_NS_CLOSED) return -1;
    if (op >= 8 && bytes.p->len > 125) return -1;
    tv_ws_frame(s, (int)op, tv_arr_data(bytes), (size_t)bytes.p->len);
    return bytes.p->len;
}

/* close(code, reason): the close frame goes out; the socket closes when the peer answers (or now,
 * if it has gone). terminate: closes now. */
void tv_native_wsClose(tv_int id, tv_int code, tv_str reason, bool now) {
    tv_ns *s = tv_ws_get(id);
    if (!s || s->state == TV_NS_CLOSED) return;
    tv_ws *w = s->ws;
    if (now || w->phase == 0) {
        tv_ns_close(s, NULL);
        return;
    }
    if (w->phase != 1) return;
    uint8_t body[125];
    size_t n = 0;
    if (code) {
        body[n++] = (uint8_t)(code >> 8);
        body[n++] = (uint8_t)code;
        size_t rl = (size_t)reason.p->len < 123 ? (size_t)reason.p->len : 123;
        memcpy(body + n, reason.p->data, rl);
        n += rl;
    }
    tv_ws_frame(s, 8, body, n);
    w->phase = 2;
}

void tv_native_wsSubscribe(tv_int id, tv_str topic, bool on) {
    tv_ns *s = tv_ws_get(id);
    if (!s || s->state == TV_NS_CLOSED) return;
    tv_ws *w = s->ws;
    tv_topic *t = tv_topic_get(topic.p->data, (size_t)topic.p->len, on);
    if (!t) return;
    if (!on) {
        tv_ws_unsub(s, t);
        return;
    }
    if (tv_ws_subscribed(w, t)) return;
    if (w->nsubs == w->subs_cap) {
        w->subs_cap = w->subs_cap ? w->subs_cap * 2 : 4;
        w->subs = tv_realloc(w->subs, (size_t)w->subs_cap * sizeof *w->subs);
    }
    w->subs[w->nsubs++] = t;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->subs = tv_realloc(t->subs, (size_t)t->cap * sizeof *t->subs);
    }
    t->subs[t->n++] = s;
}

bool tv_native_wsSubscribed(tv_int id, tv_str topic) {
    tv_ns *s = tv_ws_get(id);
    tv_topic *t = tv_topic_get(topic.p->data, (size_t)topic.p->len, false);
    return s && t && tv_ws_subscribed(s->ws, t);
}

tv_arr tv_native_wsTopics(tv_int id) {
    tv_ns *s = tv_ws_get(id);
    tv_arr out = tv_arr_fresh();
    if (!s) return out;
    for (int i = 0; i < s->ws->nsubs; i++) {
        tv_str name = tv_str_from(s->ws->subs[i]->name, s->ws->subs[i]->len);
        tv_arr_push(&out, &tv_type_str, &name);
    }
    return out;
}

/* publish(topic, data, op, except): one frame, appended to every subscriber but `except` (a
 * socket id, or 0). How many it went to. */
static tv_int tv_ws_publish(tv_str topic, const void *p, size_t n, int op, tv_int except) {
    tv_topic *t = tv_topic_get(topic.p->data, (size_t)topic.p->len, false);
    if (!t || t->n == 0) return 0;
    uint8_t h[14];
    size_t hl = tv_ws_header(h, op, n, true);
    tv_int sent = 0;
    for (int i = 0; i < t->n; i++) {
        tv_ns *s = t->subs[i];
        if (s->id == except || s->state != TV_NS_OPEN || s->ws->phase != 1 || s->ws->client) continue;
        tv_ns_append(s, (const char *)h, hl);
        if (n) tv_ns_append(s, p, n);
        tv_ns_mark(s);
        sent++;
    }
    return sent;
}

tv_int tv_native_wsPublishText(tv_str topic, tv_str text, tv_int except) {
    return tv_ws_publish(topic, text.p->data, (size_t)text.p->len, 1, except);
}

tv_int tv_native_wsPublishBytes(tv_str topic, tv_arr bytes, tv_int except) {
    return tv_ws_publish(topic, tv_arr_data(bytes), (size_t)bytes.p->len, 2, except);
}

tv_int tv_native_wsSubscribers(tv_str topic) {
    tv_topic *t = tv_topic_get(topic.p->data, (size_t)topic.p->len, false);
    return t ? t->n : 0;
}

tv_str tv_native_wsProtocol(tv_int id) {
    tv_ns *s = tv_ws_get(id);
    return s && s->ws->protocol ? tv_str_from(s->ws->protocol, strlen(s->ws->protocol)) : TV_EMPTY_STR;
}
