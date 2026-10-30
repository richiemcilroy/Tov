'use strict';

// fetch() and its classes (Headers, Request, Response, FormData), as the WHATWG Fetch Standard
// and Node.js's undici have them, over Tov's own HTTP client (runtime/tov.c, through the
// natives in runtime/node.c): keep-alive connection pools, TLS, decompression and redirects are
// native. A body read whole (text(), json(), arrayBuffer()...) arrives in one piece; `body` is a
// ReadableStream reading it as it arrives.

const { Buffer } = require('buffer');

const native = globalThis.__tov_native?.fetch;

const kHeaders = Symbol('kHeaders');
const kGuard = Symbol('kGuard');
// (on a Headers: its content-type is the default its Response filled in for the body, not one the
// code set; append, set and delete of content-type clear it. Bun.serve writes Bun's value for it.)
const kDefaulted = Symbol('kDefaulted');
const kBody = Symbol('kBody');
const kState = Symbol('kState');
const kDisturbed = Symbol.for('nodejs.stream.disturbed');

let ReadableStreamClass;
let enqueueFreshChunk;
function ReadableStream() {
  if (!ReadableStreamClass) {
    const streams = require('internal/webstreams/readablestream');
    ReadableStreamClass = streams.ReadableStream;
    enqueueFreshChunk = streams._tovEnqueueFresh;
  }
  return ReadableStreamClass;
}
// (a body's chunk, made for its stream alone: see internal/webstreams/readablestream)
const enqueueFresh = (controller, chunk) => enqueueFreshChunk(controller, chunk);

let BlobClass;
function Blob() {
  BlobClass ??= require('internal/blob').Blob;
  return BlobClass;
}

let inspect;

// ---------------------------------------------------------------- Headers

const TOKEN = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
const BAD_VALUE = /[\0\r\n]/;
// (HTTP whitespace off both ends; the value itself when there's none, as there usually isn't)
const isHttpSpace = (c) => c === 32 || c === 9 || c === 10 || c === 13;
function trimValue(v) {
  let s = 0;
  let e = v.length;
  while (s < e && isHttpSpace(v.charCodeAt(s))) s++;
  while (e > s && isHttpSpace(v.charCodeAt(e - 1))) e--;
  return s === 0 && e === v.length ? v : v.slice(s, e);
}

function checkName(name, where) {
  if (!TOKEN.test(name)) throw new TypeError(`Headers.${where}: "${name}" is an invalid header name.`);
}

class Headers {
  constructor(init = undefined) {
    // name (lowercase) -> [original name, [values]]
    this[kHeaders] = new Map();
    this[kGuard] = 'none';
    this[kDefaulted] = false;
    if (init === undefined || init === null) return;
    if (typeof init !== 'object' && typeof init !== 'function') {
      throw new TypeError("Headers constructor: Expected init to be an object.");
    }
    if (init instanceof Headers) {
      for (const [k, [name, values]] of init[kHeaders]) this[kHeaders].set(k, [name, values.slice()]);
      return;
    }
    if (typeof init[Symbol.iterator] === 'function') {
      for (const pair of init) {
        const p = [...pair];
        if (p.length !== 2) throw new TypeError('Headers constructor: expected name/value pair to be length 2, found ' + p.length + '.');
        this.append(p[0], p[1]);
      }
      return;
    }
    for (const key of Reflect.ownKeys(init)) {
      if (typeof key === 'symbol') continue;
      const d = Reflect.getOwnPropertyDescriptor(init, key);
      if (d?.enumerable) this.append(key, init[key]);
    }
  }

  #mutable(where) {
    if (this[kGuard] === 'immutable') throw new TypeError('immutable');
    void where;
  }

  append(name, value) {
    name = `${name}`;
    value = trimValue(`${value}`);
    checkName(name, 'append');
    if (BAD_VALUE.test(value)) throw new TypeError(`Headers.append: "${value}" is an invalid header value.`);
    this.#mutable('append');
    const k = name.toLowerCase();
    if (k === 'content-type') this[kDefaulted] = false;
    const entry = this[kHeaders].get(k);
    if (entry) entry[1].push(value);
    else this[kHeaders].set(k, [name, [value]]);
  }

  delete(name) {
    name = `${name}`;
    checkName(name, 'delete');
    this.#mutable('delete');
    const k = name.toLowerCase();
    if (k === 'content-type') this[kDefaulted] = false;
    this[kHeaders].delete(k);
  }

  get(name) {
    name = `${name}`;
    checkName(name, 'get');
    const entry = this[kHeaders].get(name.toLowerCase());
    return entry ? entry[1].join(', ') : null;
  }

  getSetCookie() {
    return this[kHeaders].get('set-cookie')?.[1].slice() ?? [];
  }

  has(name) {
    name = `${name}`;
    checkName(name, 'has');
    return this[kHeaders].has(name.toLowerCase());
  }

  set(name, value) {
    name = `${name}`;
    value = trimValue(`${value}`);
    checkName(name, 'set');
    if (BAD_VALUE.test(value)) throw new TypeError(`Headers.set: "${value}" is an invalid header value.`);
    this.#mutable('set');
    const k = name.toLowerCase();
    if (k === 'content-type') this[kDefaulted] = false;
    this[kHeaders].set(k, [name, [value]]);
  }

  // sorted, combined, set-cookie values each on their own
  * entries() {
    const names = [...this[kHeaders].keys()].sort();
    for (const k of names) {
      const values = this[kHeaders].get(k)[1];
      if (k === 'set-cookie') {
        for (const v of values) yield [k, v];
      } else {
        yield [k, values.join(', ')];
      }
    }
  }

  * keys() {
    for (const [k] of this.entries()) yield k;
  }

  * values() {
    for (const [, v] of this.entries()) yield v;
  }

  forEach(callback, thisArg = undefined) {
    for (const [k, v] of this.entries()) callback.call(thisArg, v, k, this);
  }

  [Symbol.iterator]() {
    return this.entries();
  }

  get [Symbol.toStringTag]() {
    return 'Headers';
  }

  [Symbol.for('nodejs.util.inspect.custom')](depth, options) {
    inspect ??= require('internal/util/inspect').inspect;
    const obj = {};
    for (const [k, v] of this.entries()) obj[k] = v;
    return `Headers ${inspect(obj, options)}`;
  }
}

// the wire form ("Name: value\r\n"...), as given (names keep their case)
function headersWire(h) {
  let out = '';
  for (const [, [name, values]] of h[kHeaders]) for (const v of values) out += `${name}: ${v}\r\n`;
  return out;
}

function headersFromWire(wire, guard) {
  const h = new Headers();
  // (line by line, without splitting the block)
  for (let i = 0, n = wire.length; i < n;) {
    let e = wire.indexOf('\r\n', i);
    if (e < 0) e = n;
    const c = wire.indexOf(':', i);
    const start = i;
    i = e + 2;
    if (c <= start || c >= e) continue;
    const name = wire.slice(start, c);
    const value = trimValue(wire.slice(c + 1, e));
    const k = name.toLowerCase();
    const entry = h[kHeaders].get(k);
    if (entry) entry[1].push(value);
    else h[kHeaders].set(k, [name, [value]]);
  }
  h[kGuard] = guard;
  return h;
}

// ---------------------------------------------------------------- FormData

class FormData {
  #entries = [];

  constructor(form = undefined) {
    if (form !== undefined) throw new TypeError("FormData constructor: Argument 1 is not supported: there's no HTMLFormElement.");
  }

  static #entry(name, value, filename) {
    name = `${name}`;
    if (value instanceof Blob()) {
      const { File } = require('internal/file');
      if (!(value instanceof File) || filename !== undefined) {
        value = new File([value], filename !== undefined ? `${filename}` : value.name ?? 'blob', { type: value.type });
      }
      return [name, value];
    }
    return [name, `${value}`];
  }

  append(name, value, filename = undefined) {
    this.#entries.push(FormData.#entry(name, value, filename));
  }

  delete(name) {
    name = `${name}`;
    this.#entries = this.#entries.filter(([n]) => n !== name);
  }

  get(name) {
    name = `${name}`;
    return this.#entries.find(([n]) => n === name)?.[1] ?? null;
  }

  getAll(name) {
    name = `${name}`;
    return this.#entries.filter(([n]) => n === name).map(([, v]) => v);
  }

  has(name) {
    name = `${name}`;
    return this.#entries.some(([n]) => n === name);
  }

  set(name, value, filename = undefined) {
    const entry = FormData.#entry(name, value, filename);
    const i = this.#entries.findIndex(([n]) => n === entry[0]);
    if (i < 0) {
      this.#entries.push(entry);
      return;
    }
    this.#entries[i] = entry;
    this.#entries = this.#entries.filter(([n], j) => j <= i || n !== entry[0]);
  }

  * entries() {
    yield* this.#entries.map(([n, v]) => [n, v]);
  }

  * keys() {
    for (const [n] of this.#entries) yield n;
  }

  * values() {
    for (const [, v] of this.#entries) yield v;
  }

  forEach(callback, thisArg = undefined) {
    for (const [n, v] of this.#entries) callback.call(thisArg, v, n, this);
  }

  [Symbol.iterator]() {
    return this.entries();
  }

  get [Symbol.toStringTag]() {
    return 'FormData';
  }
}

// a FormData as a body: its boundary is chosen when it becomes one (it's in the content type)
class FormBody {
  constructor(form) {
    this.form = form;
    this.boundary = `----formdata-tov-${Math.random().toString().slice(2, 13)}`;
  }

  get type() {
    return `multipart/form-data; boundary=${this.boundary}`;
  }
}

// multipart/form-data bytes for a FormData body
async function encodeForm({ form, boundary }) {
  const parts = [];
  const enc = (s) => Buffer.from(s, 'utf8');
  const esc = (s) => s.replace(/\n/g, '%0A').replace(/\r/g, '%0D').replace(/"/g, '%22');
  for (const [name, value] of form) {
    if (typeof value === 'string') {
      parts.push(enc(`--${boundary}\r\nContent-Disposition: form-data; name="${esc(name)}"\r\n\r\n${value.replace(/\r(?!\n)|(?<!\r)\n/g, '\r\n')}\r\n`));
    } else {
      parts.push(enc(`--${boundary}\r\nContent-Disposition: form-data; name="${esc(name)}"; filename="${esc(value.name)}"\r\nContent-Type: ${value.type || 'application/octet-stream'}\r\n\r\n`));
      parts.push(Buffer.from(await value.arrayBuffer()));
      parts.push(enc('\r\n'));
    }
  }
  parts.push(enc(`--${boundary}--\r\n`));
  return Buffer.concat(parts);
}

function parseForm(bytes, contentType) {
  const form = new FormData();
  const type = (contentType ?? '').toLowerCase();
  if (type.startsWith('application/x-www-form-urlencoded')) {
    for (const [k, v] of new URLSearchParams(Buffer.from(bytes).toString('utf8'))) form.append(k, v);
    return form;
  }
  const m = /boundary=(?:"([^"]+)"|([^;\s]+))/i.exec(contentType ?? '');
  if (!type.startsWith('multipart/form-data') || !m) throw new TypeError('Response.formData: Could not parse content as FormData.');
  const boundary = Buffer.from(`--${m[1] ?? m[2]}`);
  const buf = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let at = buf.indexOf(boundary);
  while (at >= 0) {
    const start = at + boundary.length;
    if (buf[start] === 45 && buf[start + 1] === 45) break;
    const headEnd = buf.indexOf('\r\n\r\n', start);
    if (headEnd < 0) break;
    const head = buf.toString('utf8', start + 2, headEnd);
    const next = buf.indexOf(boundary, headEnd + 4);
    if (next < 0) break;
    const content = buf.subarray(headEnd + 4, next - 2);
    const name = /name="([^"]*)"/i.exec(head)?.[1] ?? '';
    const filename = /filename="([^"]*)"/i.exec(head)?.[1];
    if (filename !== undefined) {
      const ctype = /content-type:\s*([^\r\n]+)/i.exec(head)?.[1] ?? 'application/octet-stream';
      const { File } = require('internal/file');
      form.append(name, new File([content], filename, { type: ctype }));
    } else {
      form.append(name, content.toString('utf8'));
    }
    at = next;
  }
  return form;
}

// ---------------------------------------------------------------- bodies

// a body init as [bytes | ReadableStream | null, content type or null, kind]: the kind (string,
// bytes, arraybuffer, blob, form, stream) is what Bun.serve's default content-type depends on
function extractBody(init) {
  if (init === null || init === undefined) return [null, null, null];
  if (typeof init === 'string') return [Buffer.from(init, 'utf8'), 'text/plain;charset=UTF-8', 'string'];
  if (init instanceof URLSearchParams) return [Buffer.from(init.toString(), 'utf8'), 'application/x-www-form-urlencoded;charset=UTF-8', 'form'];
  if (init instanceof ArrayBuffer || (typeof SharedArrayBuffer !== 'undefined' && init instanceof SharedArrayBuffer)) return [new Uint8Array(init.slice(0)), null, 'arraybuffer'];
  if (ArrayBuffer.isView(init)) return [new Uint8Array(init.buffer.slice(init.byteOffset, init.byteOffset + init.byteLength)), null, 'bytes'];
  if (init instanceof Blob()) return [init, init.type || null, 'blob'];
  if (init instanceof FormData) {
    const body = new FormBody(init);
    return [body, body.type, 'form'];
  }
  if (init instanceof ReadableStream()) return [init, null, 'stream'];
  if (typeof init[Symbol.asyncIterator] === 'function') return [ReadableStream().from(init), null, 'stream'];
  return [Buffer.from(`${init}`, 'utf8'), 'text/plain;charset=UTF-8', 'string'];
}

const kFilePart = Symbol.for('tov.filePart');

// all of a body's bytes
async function bodyBytes(body) {
  if (body === null) return new Uint8Array(0);
  if (body instanceof Uint8Array) return body;
  // (bytes(): a Bun.file's are the file as read, where arrayBuffer() copies them again — a
  // 400 MB upload held twice)
  if (body instanceof Blob()) return typeof body.bytes === 'function' ? body.bytes() : new Uint8Array(await body.arrayBuffer());
  if (body instanceof FormBody) return encodeForm(body);
  const chunks = [];
  for await (const c of body) chunks.push(typeof c === 'string' ? Buffer.from(c) : c);
  return Buffer.concat(chunks);
}

// the Body mixin's state: { body (bytes | Blob | FormData | ReadableStream | null), used,
// stream (made on demand), fetched (a native request still arriving) }
class BodyState {
  constructor(body, fetched = null) {
    this.body = body;
    this.used = false;
    this.stream = null;
    this.fetched = fetched;
  }
}

const noop = () => {};

function fetchedStream(handle, signal, done = noop) {
  return new (ReadableStream())({
    type: 'bytes',
    pull(controller) {
      return new Promise((resolve, reject) => {
        native.read(handle, (n) => {
          if (n > 0) {
            enqueueFresh(controller, native.take(handle));
            resolve();
          } else if (n === 0) {
            done();
            controller.close();
            resolve();
          } else {
            done();
            reject(fetchError(handle, n, signal));
          }
        });
      });
    },
    cancel() {
      done();
      native.abort(handle);
    },
  }, { highWaterMark: 0 });
}

function bodyStream(state) {
  if (state.stream) return state.stream;
  const b = state.body;
  if (state.fetched) {
    state.stream = fetchedStream(state.fetched.handle, state.fetched.signal, state.fetched.done);
  } else if (b === null) {
    return null;
  } else if (b instanceof ReadableStream()) {
    state.stream = b;
  } else if (b instanceof Blob()) {
    state.stream = b.stream();
  } else {
    const bytes = b instanceof FormBody ? null : b;
    state.stream = new (ReadableStream())({
      async start(controller) {
        const all = bytes ?? await bodyBytes(b);
        if (all.byteLength) controller.enqueue(new Uint8Array(all));
        controller.close();
      },
    });
  }
  return state.stream;
}

async function consume(owner, kind) {
  const state = owner[kBody];
  if (state.used || state.stream?.locked) throw new TypeError('Body is unusable: Body has already been read');
  state.used = true;
  let bytes;
  if (state.fetched && !state.stream) {
    const { handle, signal, done } = state.fetched;
    const r = await new Promise((resolve) => native.bodyWait(handle, resolve));
    done?.();
    if (r !== 0) throw fetchError(handle, r, signal);
    bytes = native.body(handle);
  } else if (state.stream) {
    bytes = await bodyBytes(state.stream);
  } else {
    bytes = await bodyBytes(state.body);
  }
  switch (kind) {
    case 'text': return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString('utf8').replace(/^﻿/, '');
    case 'json': return JSON.parse(Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString('utf8').replace(/^﻿/, ''));
    case 'arrayBuffer': return bytes.buffer.byteLength === bytes.byteLength && bytes.byteOffset === 0 ? bytes.buffer : bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    case 'bytes': return new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    case 'blob': return new (Blob())([bytes], { type: owner.headers.get('content-type') ?? '' });
    case 'formData': return parseForm(bytes, owner.headers.get('content-type'));
  }
}

const BodyMixin = {
  get body() {
    return bodyStream(this[kBody]);
  },
  get bodyUsed() {
    const s = this[kBody];
    return s.used || (s.stream !== null && (s.stream.locked || !!s.stream[kDisturbed]));
  },
  text() { return consume(this, 'text'); },
  json() { return consume(this, 'json'); },
  arrayBuffer() { return consume(this, 'arrayBuffer'); },
  bytes() { return consume(this, 'bytes'); },
  blob() { return consume(this, 'blob'); },
  formData() { return consume(this, 'formData'); },
};

function mixBody(cls) {
  for (const key of Reflect.ownKeys(BodyMixin)) {
    Object.defineProperty(cls.prototype, key, { ...Object.getOwnPropertyDescriptor(BodyMixin, key), enumerable: true });
  }
}

// a body for a copy: a stream body is teed
function cloneBody(state) {
  if (state.used) throw new TypeError('Response.clone: Body has already been consumed.');
  if (state.fetched || state.stream || state.body instanceof ReadableStream()) {
    const [a, b] = bodyStream(state).tee();
    state.stream = a;
    state.fetched = null;
    const copy = new BodyState(b);
    copy.stream = b;
    return copy;
  }
  return new BodyState(state.body);
}

// ---------------------------------------------------------------- Request

const FORBIDDEN_METHODS = new Set(['CONNECT', 'TRACE', 'TRACK']);
const NORMAL_METHODS = new Set(['DELETE', 'GET', 'HEAD', 'OPTIONS', 'POST', 'PUT', 'PATCH']);

function normalizeMethod(m) {
  m = `${m}`;
  if (!TOKEN.test(m)) throw new TypeError(`'${m}' is not a valid HTTP method.`);
  const upper = m.toUpperCase();
  if (FORBIDDEN_METHODS.has(upper)) throw new TypeError(`'${m}' HTTP method is unsupported.`);
  return NORMAL_METHODS.has(upper) ? upper : m;
}

let urlBinding;
const REQUEST_OPTIONS = ['mode', 'credentials', 'cache', 'referrer', 'referrerPolicy', 'integrity', 'keepalive', 'duplex'];

class Request {
  constructor(input, init = {}) {
    if (init !== null && typeof init !== 'object') throw new TypeError("Request constructor: Expected init to be an object.");
    init ??= {};
    let state;
    if (input instanceof Request) {
      state = { ...input[kState] };
      if (init.body === undefined && input[kBody].used) throw new TypeError('Cannot construct a Request with a Request object that has already been used.');
    } else {
      // (the URL Standard's parse, without making a URL object: the href and its parts' offsets)
      urlBinding ??= require('internal/bindings/url');
      const href = typeof input === 'string' || !(input instanceof URL) ? urlBinding.parse(`${input}`, undefined, false) : input.href;
      if (href === undefined) {
        let cause;
        try {
          new URL(`${input}`);
        } catch (e) {
          cause = e;
        }
        throw new TypeError(`Failed to parse URL from ${input}`, { cause });
      }
      if (typeof input === 'string' || !(input instanceof URL)) {
        const c = urlBinding.urlComponents;
        // (a username: text between "//" and its end; a password: a ":" after it)
        if (c[1] > c[0] + 2 || c[2] > c[1]) throw new TypeError(`Request cannot be constructed from a URL that includes credentials: ${input}`);
      } else if (input.username || input.password) {
        throw new TypeError(`Request cannot be constructed from a URL that includes credentials: ${input}`);
      }
      state = { url: href, method: 'GET', redirect: 'follow', signal: null, mode: 'cors', credentials: 'same-origin', cache: 'default',
        referrer: 'about:client', referrerPolicy: '', integrity: '', keepalive: false, duplex: 'half' };
    }
    if (init.method !== undefined) state.method = normalizeMethod(init.method);
    if (init.redirect !== undefined) {
      if (!['follow', 'error', 'manual'].includes(init.redirect)) throw new TypeError(`Request constructor: ${init.redirect} is not an accepted type. Expected one of follow, error, manual.`);
      state.redirect = init.redirect;
    }
    for (let i = 0; i < REQUEST_OPTIONS.length; i++) {
      const k = REQUEST_OPTIONS[i];
      if (init[k] !== undefined) state[k] = init[k];
    }
    if (init.signal !== undefined) state.signal = init.signal;
    else if (input instanceof Request) state.signal = input.signal;
    this[kState] = state;
    const headers = new Headers(init.headers !== undefined ? init.headers : input instanceof Request ? input.headers : undefined);
    this[kHeaders] = headers;
    let body = init.body !== undefined ? init.body : input instanceof Request ? input[kBody].body : null;
    if (body !== null && body !== undefined && (state.method === 'GET' || state.method === 'HEAD')) {
      throw new TypeError('Request with GET/HEAD method cannot have body.');
    }
    if (init.body !== undefined && init.body !== null) {
      const [bytes, type] = extractBody(init.body);
      body = bytes;
      if (type && !headers.has('content-type')) headers.append('content-type', type);
    } else if (input instanceof Request && body !== null) {
      body = input[kBody].stream ?? body;
      input[kBody].used = true;
    }
    this[kBody] = new BodyState(body ?? null);
  }

  get method() { return this[kState].method; }
  get url() { return this[kState].url; }
  get headers() { return this[kHeaders]; }
  get redirect() { return this[kState].redirect; }
  get signal() {
    const s = this[kState];
    if (s.signal === null) {
      s.controller = new AbortController();
      s.signal = s.controller.signal;
    }
    return s.signal;
  }
  get mode() { return this[kState].mode; }
  get credentials() { return this[kState].credentials; }
  get cache() { return this[kState].cache; }
  get referrer() { return this[kState].referrer; }
  get referrerPolicy() { return this[kState].referrerPolicy; }
  get integrity() { return this[kState].integrity; }
  get keepalive() { return this[kState].keepalive; }
  get destination() { return ''; }
  get duplex() { return this[kState].duplex; }
  get isReloadNavigation() { return false; }
  get isHistoryNavigation() { return false; }

  clone() {
    const r = Object.create(Object.getPrototypeOf(this));
    r[kState] = { ...this[kState] };
    r[kHeaders] = new Headers(this[kHeaders]);
    r[kBody] = cloneBody(this[kBody]);
    return r;
  }

  get [Symbol.toStringTag]() {
    return 'Request';
  }

  [Symbol.for('nodejs.util.inspect.custom')](depth, options) {
    inspect ??= require('internal/util/inspect').inspect;
    const s = this[kState];
    return `Request ${inspect({ method: s.method, url: s.url, headers: this[kHeaders], destination: '', referrer: s.referrer, referrerPolicy: s.referrerPolicy, mode: s.mode, credentials: s.credentials, cache: s.cache, redirect: s.redirect, integrity: s.integrity, keepalive: s.keepalive, isReloadNavigation: false, isHistoryNavigation: false, signal: this.signal }, options)}`;
  }
}
mixBody(Request);

// ---------------------------------------------------------------- Response

const NULL_BODY_STATUS = new Set([101, 103, 204, 205, 304]);
const REDIRECT_STATUS = new Set([301, 302, 303, 307, 308]);

class Response {
  constructor(body = null, init = {}) {
    init ??= {};
    const status = init.status ?? 200;
    if (!Number.isInteger(status) || status < 200 || status > 599) {
      throw new RangeError(`init["status"] must be in the range of 200 to 599, inclusive.`);
    }
    const statusText = `${init.statusText ?? ''}`;
    if (/[^\t\x20-\x7e\x80-\xff]/.test(statusText)) throw new TypeError('Invalid statusText');
    this[kState] = { status, statusText, type: 'default', url: '', redirected: false };
    this[kHeaders] = new Headers(init.headers);
    let bytes = null;
    if (body !== null && body !== undefined) {
      if (NULL_BODY_STATUS.has(status)) throw new TypeError('Response constructor: Invalid response status code ' + status);
      const [b, type, kind] = extractBody(body);
      bytes = b;
      this[kState].bodyKind = kind;
      if (type && !this[kHeaders].has('content-type')) {
        this[kHeaders].append('content-type', type);
        this[kHeaders][kDefaulted] = true;   // (after append, which clears it)
      }
    }
    this[kBody] = new BodyState(bytes);
  }

  static error() {
    const r = new Response();
    r[kState].type = 'error';
    r[kState].status = 0;
    r[kHeaders][kGuard] = 'immutable';
    return r;
  }

  static json(data, init = {}) {
    const text = JSON.stringify(data);
    if (text === undefined) throw new TypeError('Value is not JSON serializable');
    const r = new Response(text, init);
    // (the constructor filled in the string body's type unless the caller gave one: replaced only
    // then, so a caller's content-type survives)
    if (r[kHeaders][kDefaulted]) {
      r[kHeaders].set('content-type', 'application/json');
      r[kHeaders][kDefaulted] = true;
    }
    r[kState].bodyKind = 'json';
    return r;
  }

  static redirect(url, status = 302) {
    let parsed;
    try {
      parsed = new URL(`${url}`);
    } catch (e) {
      throw new TypeError(`Failed to parse URL from ${url}`, { cause: e });
    }
    if (!REDIRECT_STATUS.has(status)) throw new RangeError(`Invalid status code ${status}`);
    const r = new Response(null, { status });
    r[kHeaders].set('location', parsed.href);
    r[kHeaders][kGuard] = 'immutable';
    return r;
  }

  get type() { return this[kState].type; }
  get url() { return this[kState].url; }
  get redirected() { return this[kState].redirected; }
  get status() { return this[kState].status; }
  get ok() { return this[kState].status >= 200 && this[kState].status <= 299; }
  get statusText() { return this[kState].statusText; }
  get headers() { return this[kHeaders]; }

  clone() {
    if (this[kBody].used) throw new TypeError('Response.clone: Body has already been consumed.');
    const r = Object.create(Object.getPrototypeOf(this));
    r[kState] = { ...this[kState] };
    r[kHeaders] = new Headers(this[kHeaders]);
    r[kHeaders][kGuard] = this[kHeaders][kGuard];
    r[kHeaders][kDefaulted] = this[kHeaders][kDefaulted];
    r[kBody] = cloneBody(this[kBody]);
    return r;
  }

  get [Symbol.toStringTag]() {
    return 'Response';
  }

  [Symbol.for('nodejs.util.inspect.custom')](depth, options) {
    inspect ??= require('internal/util/inspect').inspect;
    const s = this[kState];
    return `Response ${inspect({ status: s.status, statusText: s.statusText, headers: this[kHeaders], body: this.body, bodyUsed: this.bodyUsed, ok: this.ok, redirected: s.redirected, type: s.type, url: s.url }, options)}`;
  }
}
mixBody(Response);

// ---------------------------------------------------------------- fetch()

// Tov's client's codes, as Node.js's (undici's) causes have them
const CODES = {
  ConnectionRefused: 'ECONNREFUSED',
  FailedToOpenSocket: 'ECONNREFUSED',
  ConnectionTimeout: 'UND_ERR_CONNECT_TIMEOUT',
  Malformed_HTTP_Response: 'HPE_INVALID_CONSTANT',
};

function abortReason(signal) {
  if (signal?.reason !== undefined) return signal.reason;
  const { DOMException } = require('internal/bindings/messaging');
  return new DOMException('This operation was aborted', 'AbortError');
}

function fetchError(handle, result, signal) {
  if (result === -2 && signal?.aborted) return abortReason(signal);
  const [code, message] = native.error(handle);
  const cause = new Error(message);
  cause.code = CODES[code] ?? code;
  return new TypeError('fetch failed', { cause });
}

async function fetch(input, init = undefined) {
  const request = new Request(input, init);
  const signal = init?.signal ?? (input instanceof Request ? input[kState].signal : null);
  if (signal?.aborted) throw abortReason(signal);
  if (!native) throw new TypeError('fetch failed', { cause: new Error('fetch is not available outside Tov\'s runtime') });
  const s = request[kState];
  // (the href is already a parsed URL's: its scheme is what comes before the first ':')
  const href = s.url;
  const https = href.startsWith('https:');
  if (!https && !href.startsWith('http:')) {
    if (href.startsWith('data:')) return dataResponse(new URL(href));
    throw new TypeError('fetch failed', { cause: new Error('unknown scheme') });
  }
  if (https && !native.tls()) {
    throw new TypeError('fetch failed', { cause: new Error('https needs Tov\'s TLS client, which this program wasn\'t built with') });
  }
  const headers = request[kHeaders];
  let body = request[kBody].body;
  // (a Bun.file: sent from the file a piece at a time, as Bun sends it, not read into memory first
  // — a 400 MB upload otherwise held whole, then copied)
  const part = body !== null && !request[kBody].stream ? body[kFilePart] : undefined;
  let length;
  if (part !== undefined) {
    body = null;
  } else if (body !== null) {
    body = await bodyBytes(request[kBody].stream ?? body);
    length = body.byteLength;
  }
  // (the file named just before the request starts: nothing runs between)
  if (part !== undefined) length = native.bodyFile(part.path, part.start, part.end);
  if (length !== undefined && !headers.has('content-length')) headers.set('content-length', `${length}`);
  const redirect = s.redirect === 'manual' ? 1 : s.redirect === 'error' ? 2 : 0;
  const handle = native.start(s.method, s.url, headersWire(headers), body, redirect, 1, null, null, null);
  // (the signal can outlive the request by far, as AbortSignal.timeout's does: its listener holds
  // the request only weakly, and goes once the body is done)
  let done = noop;
  if (signal) {
    const ref = new WeakRef(handle);
    const onAbort = () => {
      const h = ref.deref();
      if (h !== undefined) native.abort(h);
    };
    signal.addEventListener('abort', onAbort, { once: true });
    done = () => signal.removeEventListener('abort', onAbort);
  }
  const result = await new Promise((resolve) => native.wait(handle, resolve));
  if (result !== 0) {
    done();
    throw fetchError(handle, result, signal);
  }
  const [status, statusText, wire, finalUrl, redirected] = native.info(handle);
  // (no bodyKind, and headers from the wire are never marked defaulted: Bun.serve sends a fetched
  // response's content-type exactly as it came. Keep it so if this moves to the constructor.)
  const response = Object.create(Response.prototype);
  response[kState] = { status, statusText, type: 'basic', url: finalUrl, redirected };
  response[kHeaders] = headersFromWire(wire, 'immutable');
  const noBody = s.method === 'HEAD' || NULL_BODY_STATUS.has(status);
  if (noBody) done();
  response[kBody] = new BodyState(noBody ? null : undefined, noBody ? null : { handle, signal, done });
  if (noBody) response[kBody].body = null;
  return response;
}

// a data: URL's response
function dataResponse(url) {
  const href = url.href;
  const comma = href.indexOf(',');
  let meta = href.slice(5, comma);
  let data = decodeURIComponent(href.slice(comma + 1));
  let bytes;
  if (/;base64$/i.test(meta)) {
    meta = meta.slice(0, -7);
    bytes = Buffer.from(data, 'base64');
  } else {
    bytes = Buffer.from(data, 'latin1');
  }
  const r = new Response(bytes, { headers: { 'content-type': meta || 'text/plain;charset=US-ASCII' } });
  r[kState].url = href;
  r[kState].type = 'basic';
  return r;
}

// for Bun.serve (bun.js): a request from what Tov's server parsed (its body bytes or null), and
// what a response holds, to write it out
function serverRequest(url, method, wire, body, signal) {
  const r = Object.create(Request.prototype);
  r[kState] = { url, method, redirect: 'follow', signal, mode: 'cors', credentials: 'same-origin', cache: 'default',
    referrer: 'about:client', referrerPolicy: '', integrity: '', keepalive: false, duplex: 'half' };
  r[kHeaders] = headersFromWire(wire, 'none');
  r[kBody] = new BodyState(body);
  return r;
}

// a server's request whose client has gone: its signal aborts (made now if it hasn't been)
function abortRequest(request, reason) {
  const s = request[kState];
  if (s.signal === null) {
    s.controller = new AbortController();
    s.signal = s.controller.signal;
  }
  s.controller?.abort(reason);
}

const internals = { kState, kHeaders, kDefaulted, kBody, serverRequest, abortRequest, bodyStream, bodyBytes, FormBody, encodeForm };

module.exports = { fetch, Headers, Request, Response, FormData, internals };
