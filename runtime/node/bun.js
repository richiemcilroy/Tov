'use strict';

// The `bun` module and the `Bun` global, for packages and apps written for Bun: serve (on Tov's
// native HTTP server, as node:http's servers are: requests come to JavaScript as fetch Requests
// and Responses go back out raw), spawn and spawnSync (over child_process, with web streams),
// file and write (BunFile is a Blob read from the file when it's used), sleep, semver, which,
// and the small helpers. What Bun's own native parts do (its bundler, SQLite, S3, WebSockets,
// the shell) isn't here.

const { Buffer } = require('buffer');

const http = globalThis.__tov_native?.http;
const streams = globalThis.__tov_native?.stream;

let fetchModule;
function fetchApi() {
  fetchModule ??= require('internal/tov/fetch');
  return fetchModule;
}
let fs;
function fsModule() {
  fs ??= require('fs');
  return fs;
}
let BlobClass;
function Blob() {
  BlobClass ??= require('internal/blob').Blob;
  return BlobClass;
}

function notImplemented(what) {
  const e = new Error(`${what} is not implemented in Tov yet`);
  e.code = 'ERR_NOT_IMPLEMENTED';
  return e;
}

// ---------------------------------------------------------------- sleep, small helpers

function sleep(ms) {
  if (ms instanceof Date) ms = ms.getTime() - Date.now();
  return new Promise((resolve) => setTimeout(resolve, Math.max(0, Number(ms) || 0)));
}

function sleepSync(ms) {
  const n = Math.max(0, Number(ms) || 0);
  if (n > 0) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, n);
}

const HTML_ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#x27;' };
function escapeHTML(value) {
  return `${value}`.replace(/[&<>"']/g, (c) => HTML_ESCAPES[c]);
}

// the executable `command` names: a path as it is, else the first match on PATH (null: none)
// (each spawn looks its command up: natively, without an exception per directory that
// hasn't it, and the working directory only read for a relative one)
function which(command, options) {
  command = `${command}`;
  const path = require('path');
  const native = globalThis.__tov_native?.fs?.runnable;
  const runnable = native ?? ((p) => {
    const { accessSync, statSync, constants } = fsModule();
    try {
      accessSync(p, constants.X_OK);
      return statSync(p).isFile();
    } catch {
      return false;
    }
  });
  if (command.includes('/')) {
    const p = command.charCodeAt(0) === 47 ? path.resolve(command) : path.resolve(options?.cwd ?? process.cwd(), command);
    return runnable(p) ? p : null;
  }
  const PATH = `${options?.PATH ?? process.env.PATH ?? ''}`;
  if (native) {
    // (natively, when PATH's directories are absolute as they read)
    const found = globalThis.__tov_native.fs.which(command, PATH);
    if (found !== undefined) return found;
  }
  const dirs = PATH.split(':');
  for (const dir of dirs) {
    if (!dir) continue;
    const p = dir.charCodeAt(0) === 47 ? path.resolve(dir, command) : path.resolve(options?.cwd ?? process.cwd(), dir, command);
    if (runnable(p)) return p;
  }
  return null;
}

// ---------------------------------------------------------------- semver (node-semver's ranges)

const SEMVER = /^\s*[v=]?\s*(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?\s*$/;

function parseVersion(v) {
  const m = SEMVER.exec(`${v}`);
  if (!m) return null;
  return { major: +m[1], minor: +m[2], patch: +m[3], pre: m[4] ? m[4].split('.') : [] };
}

function comparePre(a, b) {
  if (!a.length || !b.length) return a.length ? -1 : b.length ? 1 : 0;
  for (let i = 0; ; i++) {
    if (i >= a.length || i >= b.length) return a.length === b.length ? 0 : i >= a.length ? -1 : 1;
    const x = a[i], y = b[i];
    if (x === y) continue;
    const nx = /^\d+$/.test(x), ny = /^\d+$/.test(y);
    if (nx && ny) return +x < +y ? -1 : 1;
    if (nx) return -1;
    if (ny) return 1;
    return x < y ? -1 : 1;
  }
}

function compareVersions(a, b) {
  return (a.major - b.major) || (a.minor - b.minor) || (a.patch - b.patch) ? Math.sign((a.major - b.major) || (a.minor - b.minor) || (a.patch - b.patch)) : comparePre(a.pre, b.pre);
}

// a partial version ("1", "1.2", "1.x", "*"): numbers, or null where it's a wildcard
const PARTIAL = /^[v=]?(\d+|[xX*])(?:\.(\d+|[xX*]))?(?:\.(\d+|[xX*]))?(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/;
function partial(s) {
  if (s === '' || s === '*' || s === 'x' || s === 'X') return { major: null, minor: null, patch: null, pre: [] };
  const m = PARTIAL.exec(s);
  if (!m) return undefined;
  const n = (x) => (x === undefined || /^[xX*]$/.test(x) ? null : +x);
  const major = n(m[1]);
  const minor = major === null ? null : n(m[2]);
  const patch = minor === null ? null : n(m[3]);
  return { major, minor, patch, pre: m[4] ? m[4].split('.') : [] };
}

const v3 = (major, minor, patch, pre = []) => ({ major, minor, patch, pre });
const ZERO_PRE = ['0'];

// one comparator token as [op, version] pairs ('' : any version)
function desugar(token) {
  const m = /^(\^|~>?|>=|<=|>|<|=)?\s*(.*)$/.exec(token);
  const op = m[1] ?? '';
  const p = partial(m[2]);
  if (p === undefined) return undefined;
  const { major: M, minor: mi, patch: pa, pre } = p;
  if (M === null) return op === '<' || op === '>' ? [['<', v3(0, 0, 0, ZERO_PRE)]] : [];
  const lo = v3(M, mi ?? 0, pa ?? 0, pre);
  if (op === '^') {
    let hi;
    if (M > 0 || mi === null) hi = v3(M + 1, 0, 0, ZERO_PRE);
    else if (mi > 0 || pa === null) hi = v3(0, mi + 1, 0, ZERO_PRE);
    else hi = v3(0, 0, pa + 1, ZERO_PRE);
    return [['>=', lo], ['<', hi]];
  }
  if (op === '~' || op === '~>') {
    return [['>=', lo], ['<', mi === null ? v3(M + 1, 0, 0, ZERO_PRE) : v3(M, mi + 1, 0, ZERO_PRE)]];
  }
  const next = mi === null ? v3(M + 1, 0, 0, ZERO_PRE) : pa === null ? v3(M, mi + 1, 0, ZERO_PRE) : null;
  switch (op) {
    case '>': return next ? [['>=', next]] : [['>', lo]];
    case '>=': return [['>=', lo]];
    case '<': return [['<', next ? v3(M, mi ?? 0, 0, ZERO_PRE) : lo]];
    case '<=': return next ? [['<', next]] : [['<=', lo]];
    default: return next ? [['>=', lo], ['<', next]] : [['=', lo]];
  }
}

function parseRange(range) {
  const sets = [];
  for (let part of `${range}`.split('||')) {
    part = part.trim().replace(/(\^|~>?|>=|<=|>|<|=)\s+/g, '$1');
    const comparators = [];
    const hyphen = /^(\S+)\s+-\s+(\S+)$/.exec(part);
    if (hyphen) {
      const a = desugar(`>=${hyphen[1]}`), b = partial(hyphen[2]);
      if (!a || !b) return null;
      comparators.push(...a);
      if (b.major !== null) comparators.push(...desugar(`<=${hyphen[2]}`));
    } else {
      for (const token of part.split(/\s+/)) {
        if (!token) continue;
        const c = desugar(token);
        if (!c) return null;
        comparators.push(...c);
      }
    }
    sets.push(comparators);
  }
  return sets;
}

function testComparator([op, v], version) {
  const c = compareVersions(version, v);
  switch (op) {
    case '<': return c < 0;
    case '<=': return c <= 0;
    case '>': return c > 0;
    case '>=': return c >= 0;
    default: return c === 0;
  }
}

const semver = {
  satisfies(version, range) {
    const v = parseVersion(version);
    const sets = parseRange(range);
    if (!v || !sets) return false;
    return sets.some((set) => {
      if (!set.every((c) => testComparator(c, v))) return false;
      // (a prerelease satisfies a range only through a comparator on its own major.minor.patch)
      if (v.pre.length === 0) return true;
      return set.some(([, c]) => c.pre.length > 0 && c.pre !== ZERO_PRE && c.major === v.major && c.minor === v.minor && c.patch === v.patch);
    });
  },
  order(a, b) {
    const x = parseVersion(a), y = parseVersion(b);
    if (!x || !y) throw new TypeError(`Invalid SemVer: ${x ? b : a}`);
    return compareVersions(x, y);
  },
};

// ---------------------------------------------------------------- file, write

const MIME = {
  txt: 'text/plain;charset=utf-8', html: 'text/html;charset=utf-8', htm: 'text/html;charset=utf-8', css: 'text/css;charset=utf-8',
  js: 'text/javascript;charset=utf-8', mjs: 'text/javascript;charset=utf-8', cjs: 'text/javascript;charset=utf-8',
  ts: 'text/javascript;charset=utf-8', tsx: 'text/javascript;charset=utf-8', jsx: 'text/javascript;charset=utf-8',
  json: 'application/json;charset=utf-8', xml: 'application/xml', csv: 'text/csv', md: 'text/markdown',
  svg: 'image/svg+xml', png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif', webp: 'image/webp',
  avif: 'image/avif', ico: 'image/x-icon', mp4: 'video/mp4', m4v: 'video/mp4', webm: 'video/webm', mov: 'video/quicktime',
  mkv: 'video/x-matroska', m3u8: 'application/vnd.apple.mpegurl', mp3: 'audio/mpeg', m4a: 'audio/mp4', aac: 'audio/aac',
  wav: 'audio/wav', ogg: 'audio/ogg', opus: 'audio/opus', flac: 'audio/flac', pdf: 'application/pdf', wasm: 'application/wasm',
  zip: 'application/zip', gz: 'application/gzip', tar: 'application/x-tar', woff: 'font/woff', woff2: 'font/woff2',
};
// A Blob's type as Bun.serve sends it: a type in the table above as the table has it (with
// `charset=utf-8` replacing any other for text), anything else as it is.
const BLOB_TYPE = new Map([...Object.values(MIME).map((v) => [v.split(';')[0], v]), ['application/javascript', MIME.js]]);
// (types reaching here come from a Blob, which lower-cases its own type, so an unmatched one is
// already lower-case: see internal/blob.js)
const blobType = (t) => BLOB_TYPE.get(t.split(';')[0].trim()) ?? t;

// The content-type Bun.serve sends for a Response built in JavaScript, or undefined to send its
// headers as they are. Its headers object keeps the Fetch standard's values (as Node.js has them);
// the wire has Bun's: a default only for a type the Response filled in itself (a body's
// kDefaulted content-type) or a body with none, never for one the code set, nor for a fetched
// Response (no bodyKind).
function wireType(res, kState, kHeaders, kDefaulted, kBody) {
  const kind = res[kState].bodyKind;
  if (!kind) return undefined;
  const headers = res[kHeaders];
  if (!headers.has('content-type')) {
    if (kind === 'string' || kind === 'json') return 'text/plain;charset=utf-8';
    if (kind === 'bytes' || kind === 'arraybuffer') return 'application/octet-stream';
    if (kind === 'blob') {
      const t = res[kBody].body?.type;
      return t ? blobType(t) : 'text/plain;charset=utf-8';
    }
    return undefined;
  }
  if (!headers[kDefaulted]) return undefined;
  if (kind === 'string') return 'text/plain;charset=utf-8';
  if (kind === 'json') return 'application/json;charset=utf-8';
  if (kind === 'blob') return blobType(headers.get('content-type'));
  // (a form body's type, URL-encoded with `charset=UTF-8` or multipart with its boundary, is Bun's)
  return undefined;
}

function mimeOf(path) {
  const dot = path.lastIndexOf('.');
  const ext = dot > path.lastIndexOf('/') ? path.slice(dot + 1).toLowerCase() : '';
  return MIME[ext] ?? 'application/octet-stream';
}

let blobInternals;
function blobs() {
  blobInternals ??= require('internal/blob');
  return blobInternals;
}

function definePath(path) {
  if (path instanceof URL) return require('url').fileURLToPath(path);
  if (typeof path === 'string') return path;
  if (typeof path === 'number') return `/dev/fd/${path}`;
  throw new TypeError('Bun.file() expects a path, a file: URL or a file descriptor');
}

// (a Blob read from its file whenever it's read: the size, type and bytes are the file's then)
let BunFileClass;
function BunFile() {
  if (BunFileClass) return BunFileClass;
  const { kHandle, createBlob } = blobs();
  BunFileClass = class BunFile extends Blob() {
    #path;
    #type;
    #start;
    #end;

    constructor(path, type, start = 0, end = Infinity) {
      super();
      this.#path = path;
      this.#type = type;
      this.#start = start;
      this.#end = end;
    }

    #fileSize() {
      try {
        return fsModule().statSync(this.#path).size;
      } catch {
        return 0;
      }
    }

    get size() {
      const size = this.#fileSize();
      return Math.max(0, Math.min(this.#end, size) - Math.min(this.#start, size));
    }

    get type() { return this.#type; }
    get name() { return this.#path; }
    get lastModified() {
      try {
        return fsModule().statSync(this.#path).mtimeMs;
      } catch {
        return Date.now();
      }
    }

    slice(start = 0, end = undefined, type = undefined) {
      const size = this.#end === Infinity ? this.#fileSize() - this.#start : this.#end - this.#start;
      const rel = (n, d) => (n === undefined ? d : n < 0 ? Math.max(0, size + n) : Math.min(n, size));
      const s = rel(start, 0), e = Math.max(s, rel(end, size));
      return new BunFileClass(this.#path, type === undefined ? this.#type : `${type}`, this.#start + s, this.#start + e);
    }

    // the bytes, read now
    async #read() {
      const { promises } = fsModule();
      if (this.#start === 0 && this.#end === Infinity) return promises.readFile(this.#path);
      const fh = await promises.open(this.#path, 'r');
      try {
        const size = (await fh.stat()).size;
        const from = Math.min(this.#start, size), to = Math.min(this.#end, size);
        const buf = Buffer.alloc(Math.max(0, to - from));
        if (buf.length) await fh.read(buf, 0, buf.length, from);
        return buf;
      } finally {
        await fh.close();
      }
    }

    async text() { return (await this.#read()).toString('utf8'); }
    async json() { return JSON.parse(await this.text()); }
    async bytes() { const b = await this.#read(); return new Uint8Array(b.buffer, b.byteOffset, b.byteLength); }
    async arrayBuffer() { const b = await this.#read(); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); }

    stream() {
      const made = blobs().createBlobFromFilePath(this.#path, { type: this.#type });
      if (!(made instanceof Blob())) {
        const path = this.#path;
        return new ReadableStream({
          start(c) {
            const e = new Error(`ENOENT: no such file or directory, open '${path}'`);
            Object.assign(e, { code: 'ENOENT', errno: -2, syscall: 'open', path });
            c.error(e);
          },
        });
      }
      return (this.#start === 0 && this.#end === Infinity ? made : made.slice(this.#start, this.#end)).stream();
    }

    async exists() {
      try {
        return (await fsModule().promises.stat(this.#path)).isFile();
      } catch {
        return false;
      }
    }

    write(data) { return write(this, data); }
    async delete() { await fsModule().promises.unlink(this.#path); }
    unlink() { return this.delete(); }

    writer() {
      const ws = fsModule().createWriteStream(this.#path);
      return fileSink(ws);
    }

    get [Symbol.toStringTag]() { return 'Blob'; }

    // (for fetch: a body sent from the file as it goes, not read into memory first)
    get [Symbol.for('tov.filePart')]() { return { path: this.#path, start: this.#start, end: this.#end === Infinity ? -1 : this.#end }; }
  };
  // (Node.js's own Blob code reads a blob through its handle: a file's, made when asked)
  Object.defineProperty(BunFileClass.prototype, kHandle, {
    __proto__: null,
    configurable: true,
    get() {
      const made = blobs().createBlobFromFilePath(this.name, { type: this.type });
      if (!(made instanceof Blob())) return createBlob(new Uint8Array(0), 0)[kHandle];
      const size = made.size;
      const start = Math.min(this.size === 0 ? 0 : size - this.size, size);
      return made[kHandle].slice(Math.max(0, start), size);
    },
    set() {},
  });
  return BunFileClass;
}

function file(path, options) {
  const p = definePath(path);
  const File = BunFile();
  return new File(p, options?.type !== undefined ? `${options.type}` : mimeOf(p));
}

// a body's bytes (string, bytes, Blob, Response, Request)
async function bytesOf(data) {
  if (typeof data === 'string') return Buffer.from(data, 'utf8');
  if (data instanceof ArrayBuffer || (typeof SharedArrayBuffer !== 'undefined' && data instanceof SharedArrayBuffer)) return Buffer.from(data);
  if (ArrayBuffer.isView(data)) return Buffer.from(data.buffer, data.byteOffset, data.byteLength);
  if (data instanceof Blob()) return Buffer.from(await data.arrayBuffer());
  if (data && typeof data.arrayBuffer === 'function') return Buffer.from(await data.arrayBuffer());
  if (data === null || data === undefined) return Buffer.alloc(0);
  return Buffer.from(`${data}`, 'utf8');
}

async function write(destination, input) {
  const path = destination instanceof Blob() && typeof destination.name === 'string' ? destination.name : definePath(destination);
  const bytes = await bytesOf(input);
  await fsModule().promises.writeFile(path, bytes);
  return bytes.byteLength;
}

// ---------------------------------------------------------------- spawn

// Bun's FileSink over a Node.js Writable
// (as Bun's FileSink: flush() and end() return a promise while writes are pending, settled when
// they're written (end(): and the file closed), so a writer awaiting them holds a chunk or two,
// not everything it was handed faster than the disk took it)
function fileSink(writable) {
  let failed = null;
  writable.on('error', (e) => { failed = e; });
  // settles when what's been written so far is on its way to the file (writes are in order)
  const written = () => new Promise((resolve, reject) => {
    if (failed) return reject(failed);
    writable.write(Buffer.alloc(0), (e) => (e ? reject(e) : resolve()));
  });
  let pending = 0;
  return {
    write(chunk) {
      const b = typeof chunk === 'string' ? Buffer.from(chunk, 'utf8') : ArrayBuffer.isView(chunk) ? Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength) : Buffer.from(chunk);
      writable.write(b);
      pending += b.byteLength;
      return b.byteLength;
    },
    flush() {
      if (failed) return Promise.reject(failed);
      if (writable.writableLength === 0) {
        const n = pending;
        pending = 0;
        return n;
      }
      const n = pending;
      pending = 0;
      return written().then(() => n);
    },
    end(error) {
      if (error) {
        writable.destroy(error);
        return 0;
      }
      const n = pending;
      pending = 0;
      return new Promise((resolve, reject) => {
        if (failed) return reject(failed);
        writable.once('error', reject);
        writable.once('close', () => resolve(n));
        writable.end();
      });
    },
    start() {},
    ref() {},
    unref() {},
  };
}

// a Node.js Readable as a web ReadableStream of Uint8Arrays
function webStream(readable) {
  return new ReadableStream({
    start(controller) {
      readable.on('data', (d) => {
        controller.enqueue(new Uint8Array(d.buffer, d.byteOffset, d.byteLength));
        if (controller.desiredSize <= 0) readable.pause();
      });
      readable.once('end', () => controller.close());
      readable.once('error', (e) => controller.error(e));
    },
    pull() {
      readable.resume();
    },
    cancel() {
      readable.destroy();
    },
  }, { highWaterMark: 65536, size: (chunk) => chunk.byteLength });
}

function spawnError(command, code) {
  const messages = { ENOENT: 'no such file or directory', EACCES: 'permission denied' };
  const e = new Error(`${code}: ${messages[code]}, posix_spawn '${command}'`);
  e.code = code;
  e.errno = code === 'ENOENT' ? -2 : -13;
  e.syscall = 'posix_spawn';
  e.path = command;
  return e;
}

function spawnArgs(cmdOrOptions, options) {
  let cmd;
  if (Array.isArray(cmdOrOptions)) {
    cmd = cmdOrOptions;
    options = options ?? {};
  } else {
    options = cmdOrOptions ?? {};
    cmd = options.cmd;
  }
  if (!Array.isArray(cmd) || cmd.length === 0) throw new TypeError('cmd must be an array of at least one string');
  cmd = cmd.map((a) => `${a}`);
  // (Bun fails at once when there's nothing to run; Node.js's spawn would say so later)
  const env = options.env ?? process.env;
  const file = which(cmd[0], { PATH: env.PATH, cwd: options.cwd });
  if (file === null) {
    let exists = false;
    try {
      exists = fsModule().existsSync(require('path').resolve(options.cwd ?? process.cwd(), cmd[0]));
    } catch {}
    throw spawnError(cmd[0], cmd[0].includes('/') && exists ? 'EACCES' : 'ENOENT');
  }
  return [cmd, options, env, file];
}

// what a stdio option is for child_process ('pipe', 'inherit', 'ignore', a fd), and any input
// to feed a piped stdin
function stdioOf(value, fallback, isInput) {
  if (value === undefined) value = fallback;
  if (value === null || value === 'ignore') return ['ignore'];
  if (value === 'pipe' || value === 'inherit') return [value];
  if (typeof value === 'number') return [value];
  if (isInput) return ['pipe', value];
  if (value instanceof Blob() && typeof value.name === 'string') {
    return [fsModule().openSync(value.name, 'w'), null, true];
  }
  return ['pipe'];
}

async function feed(writable, input) {
  try {
    if (input instanceof ReadableStream) {
      for await (const chunk of input) {
        if (!writable.write(typeof chunk === 'string' ? chunk : Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength))) {
          await new Promise((resolve) => writable.once('drain', resolve));
        }
      }
      writable.end();
    } else {
      const body = input instanceof Response || input instanceof Request ? input : input;
      writable.end(await bytesOf(body));
    }
  } catch (e) {
    writable.destroy(e);
  }
}

// A pipe's end as a web ReadableStream, read on the native handle (each read a fresh copy)
const UV_EOF = -4095;
function pipeStream(fd) {
  const native = globalThis.__tov_native.stream;
  const { callFromHost } = require('internal/bindings/task_queue');
  // (read in batches while the child writes steadily: runtime/node.c, tv_node_stream_batch)
  const h = native.open(fd, true);
  let controller;
  let reading = false;
  let done = false;
  const finish = () => {
    if (done) return;
    done = true;
    reading = false;
    native.close(h);
    native.release(h);
  };
  const onread = (chunk, err) => callFromHost(() => {
    if (done) return;
    if (chunk) {
      controller.enqueue(chunk);
      if (controller.desiredSize <= 0 && reading) {
        reading = false;
        native.readStop(h);
      }
      return;
    }
    finish();
    if (err === UV_EOF) controller.close();
    else controller.error(Object.assign(new Error(`read failed (${err})`), { errno: err }));
  });
  const read = () => {
    if (done || reading) return;
    reading = true;
    native.readStart(h, onread);
  };
  return new ReadableStream({
    start(c) {
      controller = c;
      read();
    },
    pull: read,
    cancel: finish,
  }, { highWaterMark: 65536, size: (chunk) => chunk.byteLength });
}

// The child of a spawn with nothing to feed it: the process's native handle (process_wrap's
// Process), as much of ChildProcess as Subprocess uses
class DirectChild {
  #listeners = { exit: [], error: [] };
  #handle;

  constructor(handle) {
    this.#handle = handle;
    this.pid = handle.pid;
    handle.onexit = (exitCode, signalCode) => {
      handle.close();
      for (const fn of this.#listeners.exit) fn(signalCode ? null : exitCode, signalCode);
    };
  }

  once(event, fn) {
    this.#listeners[event]?.push(fn);
  }

  fail(err) {
    queueMicrotask(() => {
      for (const fn of this.#listeners.error) fn(err);
    });
  }

  kill(signal) {
    const signals = require('os').constants.signals;
    const n = typeof signal === 'number' ? signal : signals[signal];
    if (n === undefined) throw new TypeError(`Unknown signal: ${signal}`);
    return this.#handle.kill(n) === 0;
  }

  ref() { this.#handle.ref(); }
  unref() { this.#handle.unref(); }
}

// (stdio a spawn can run with directly: nothing to feed the child, no file to write)
const direct = (s) => s.length === 1 && (s[0] === 'pipe' || s[0] === 'inherit' || s[0] === 'ignore' || typeof s[0] === 'number');

// Without ChildProcess and Node.js's streams between them: the process's handle, and its pipes
// read straight into web streams
function spawnDirect(cmd, options, env, stdin, stdout, stderr, file) {
  const { Process } = require('internal/bindings/process_wrap');
  const fds = [-1, -1, -1];
  const spec = (s, i) => {
    if (s[0] === 'pipe') return { type: 'pipe', handle: { _tovOpen(fd) { fds[i] = fd; return 0; } } };
    if (s[0] === 'inherit') return { type: 'inherit', fd: i };
    if (typeof s[0] === 'number') return { type: 'fd', fd: s[0] };
    return { type: 'ignore' };
  };
  const envPairs = [];
  for (const k in env) {
    if (env[k] !== undefined) envPairs.push(`${k}=${env[k]}`);
  }
  const handle = new Process();
  const err = handle.spawn({
    // (the path spawnArgs found: the native spawn doesn't search PATH again)
    file: file ?? cmd[0],
    args: [options.argv0 ?? cmd[0], ...cmd.slice(1)],
    envPairs,
    cwd: options.cwd,
    stdio: [spec(stdin, 0), spec(stdout, 1), spec(stderr, 2)],
    detached: !!options.detached,
  });
  const child = new DirectChild(handle);
  if (err < 0) {
    const e = new Error(`spawn ${cmd[0]} failed (${err})`);
    e.errno = err;
    e.syscall = 'spawn';
    child.fail(e);
  }
  const streams = [undefined, fds[1] >= 0 ? pipeStream(fds[1]) : undefined, fds[2] >= 0 ? pipeStream(fds[2]) : undefined];
  return [child, streams];
}

function spawn(cmdOrOptions, maybeOptions) {
  const [cmd, options, env, file] = spawnArgs(cmdOrOptions, maybeOptions);
  const io = options.stdio ?? [];
  const stdin = stdioOf(io[0] ?? options.stdin, 'ignore', true);
  const stdout = stdioOf(io[1] ?? options.stdout, 'pipe', false);
  const stderr = stdioOf(io[2] ?? options.stderr, 'inherit', false);
  if (stdin[0] !== 'pipe' && direct(stdin) && direct(stdout) && direct(stderr)) {
    const [child, streams] = spawnDirect(cmd, options, env, stdin, stdout, stderr, file);
    return subprocess(new Subprocess(child, stdin, stdout, stderr, options, streams), options);
  }
  const childProcess = require('child_process');
  const child = childProcess.spawn(cmd[0], cmd.slice(1), {
    cwd: options.cwd,
    env,
    stdio: [stdin[0], stdout[0], stderr[0]],
    argv0: options.argv0,
    detached: options.detached,
  });
  for (const s of [stdout, stderr]) if (s[2]) fsModule().closeSync(s[0]);
  return subprocess(new Subprocess(child, stdin, stdout, stderr, options), options);
}

// (a spawn's signal and timeout)
function subprocess(proc, options) {
  if (options.signal) {
    const abort = () => proc.kill(options.killSignal);
    if (options.signal.aborted) abort();
    else options.signal.addEventListener('abort', abort, { once: true });
  }
  if (options.timeout > 0) {
    const t = setTimeout(() => proc.kill(options.killSignal), options.timeout);
    t.unref?.();
    proc.exited.then(() => clearTimeout(t), () => clearTimeout(t));
  }
  return proc;
}

const kChild = Symbol('kChild');

class Subprocess {
  constructor(child, stdin, stdout, stderr, options, streams) {
    this[kChild] = child;
    this.pid = child.pid;
    this.exitCode = null;
    this.signalCode = null;
    this.killed = false;
    this.stdin = stdin[0] === 'pipe' ? (stdin.length > 1 ? undefined : fileSink(child.stdin)) : typeof stdin[0] === 'number' ? stdin[0] : undefined;
    if (stdin.length > 1) feed(child.stdin, stdin[1]);
    this.stdout = stdout[0] === 'pipe' ? streams?.[1] ?? webStream(child.stdout) : typeof stdout[0] === 'number' && !stdout[2] ? stdout[0] : undefined;
    this.stderr = stderr[0] === 'pipe' ? streams?.[2] ?? webStream(child.stderr) : typeof stderr[0] === 'number' && !stderr[2] ? stderr[0] : undefined;
    this.exited = new Promise((resolve) => {
      child.once('exit', (code, signal) => {
        this.exitCode = code;
        this.signalCode = signal;
        const signals = require('os').constants.signals;
        resolve(code ?? 128 + (signals[signal] ?? 0));
        if (typeof options.onExit === 'function') {
          queueMicrotask(() => options.onExit(this, code, signal, undefined));
        }
      });
      child.once('error', (e) => {
        if (this.exitCode === null) {
          this.exitCode = -1;
          resolve(-1);
        }
        if (typeof options.onExit === 'function') options.onExit(this, null, null, e);
      });
    });
  }

  get readable() { return this.stdout; }

  kill(signal) {
    if (this.exitCode !== null || this.signalCode !== null) return;
    if (this[kChild].kill(signal ?? 'SIGTERM')) this.killed = true;
  }

  ref() { this[kChild].ref(); }
  unref() { this[kChild].unref(); }
  resourceUsage() { return undefined; }
  async [Symbol.asyncDispose]() {
    this.kill();
    await this.exited;
  }
}

function spawnSync(cmdOrOptions, maybeOptions) {
  const [cmd, options, env] = spawnArgs(cmdOrOptions, maybeOptions);
  const io = options.stdio ?? [];
  const stdinOption = io[0] ?? options.stdin;
  let input;
  let stdin = 'ignore';
  if (stdinOption === 'inherit' || typeof stdinOption === 'number') stdin = stdinOption;
  else if (typeof stdinOption === 'string' && stdinOption !== 'pipe' && stdinOption !== 'ignore') input = Buffer.from(stdinOption);
  else if (ArrayBuffer.isView(stdinOption)) input = Buffer.from(stdinOption.buffer, stdinOption.byteOffset, stdinOption.byteLength);
  if (input !== undefined) stdin = 'pipe';
  const out = (v) => (v === undefined || v === 'pipe' ? 'pipe' : v === null ? 'ignore' : v);
  const stdout = out(io[1] ?? options.stdout), stderr = out(io[2] ?? options.stderr);
  const r = require('child_process').spawnSync(cmd[0], cmd.slice(1), {
    cwd: options.cwd, env, input, stdio: [stdin, stdout, stderr], maxBuffer: Infinity, timeout: options.timeout, killSignal: options.killSignal,
  });
  if (r.error && r.error.code === 'ENOENT') throw spawnError(cmd[0], 'ENOENT');
  return {
    pid: r.pid,
    exitCode: r.status,
    signalCode: r.signal,
    success: r.status === 0,
    stdout: stdout === 'pipe' ? r.stdout : undefined,
    stderr: stderr === 'pipe' ? r.stderr : undefined,
    resourceUsage: undefined,
  };
}

// ---------------------------------------------------------------- serve

const STATUS = {
  100: 'Continue', 101: 'Switching Protocols', 102: 'Processing', 103: 'Early Hints', 200: 'OK', 201: 'Created', 202: 'Accepted',
  203: 'Non-Authoritative Information', 204: 'No Content', 205: 'Reset Content', 206: 'Partial Content', 207: 'Multi-Status',
  208: 'Already Reported', 226: 'IM Used', 300: 'Multiple Choices', 301: 'Moved Permanently', 302: 'Found', 303: 'See Other',
  304: 'Not Modified', 305: 'Use Proxy', 307: 'Temporary Redirect', 308: 'Permanent Redirect', 400: 'Bad Request',
  401: 'Unauthorized', 402: 'Payment Required', 403: 'Forbidden', 404: 'Not Found', 405: 'Method Not Allowed',
  406: 'Not Acceptable', 407: 'Proxy Authentication Required', 408: 'Request Timeout', 409: 'Conflict', 410: 'Gone',
  411: 'Length Required', 412: 'Precondition Failed', 413: 'Payload Too Large', 414: 'URI Too Long',
  415: 'Unsupported Media Type', 416: 'Range Not Satisfiable', 417: 'Expectation Failed', 418: "I'm a Teapot",
  421: 'Misdirected Request', 422: 'Unprocessable Entity', 423: 'Locked', 424: 'Failed Dependency', 425: 'Too Early',
  426: 'Upgrade Required', 428: 'Precondition Required', 429: 'Too Many Requests', 431: 'Request Header Fields Too Large',
  451: 'Unavailable For Legal Reasons', 500: 'Internal Server Error', 501: 'Not Implemented', 502: 'Bad Gateway',
  503: 'Service Unavailable', 504: 'Gateway Timeout', 505: 'HTTP Version Not Supported', 506: 'Variant Also Negotiates',
  507: 'Insufficient Storage', 508: 'Loop Detected', 510: 'Not Extended', 511: 'Network Authentication Required',
};
const NULL_BODY = new Set([101, 103, 204, 205, 304]);

// the Date header, made once a second
let dateSecond = -1;
let dateValue = '';
function httpDate() {
  const now = Date.now();
  const s = Math.floor(now / 1000);
  if (s !== dateSecond) {
    dateSecond = s;
    dateValue = new Date(now).toUTCString();
  }
  return dateValue;
}

// a streamed response waits while more than this is buffered for its connection
const HIGH_WATER = 1 << 20;

let callFromHost;
const requestIds = new WeakMap();
// requests being answered, by native id: their clients can go away (http.onGone)
const live = new Map();
let goneHooked = false;

function onGone(id) {
  const req = live.get(id);
  if (req === undefined) return false;
  live.delete(id);
  const { DOMException } = require('internal/bindings/messaging');
  fetchApi().internals.abortRequest(req, new DOMException('The connection was closed', 'AbortError'));
  return true;
}

function serve(options) {
  if (options === null || typeof options !== 'object') throw new TypeError('Bun.serve expects an object');
  if (!http) throw notImplemented('Bun.serve outside Tov\'s runtime');
  if (options.unix !== undefined) throw notImplemented('Bun.serve on a unix socket');
  if (options.tls !== undefined && options.tls !== null && options.tls !== false) throw notImplemented('Bun.serve with TLS');
  if (typeof options.fetch !== 'function' && options.routes === undefined) throw new TypeError('Bun.serve expects a fetch function or routes');
  return new Server(options);
}

class Server {
  #id;
  #options;
  #routes;
  #stopped = false;
  #pending = 0;

  constructor(options) {
    this.#options = options;
    this.#routes = compileRoutes(options.routes);
    const port = Number(options.port ?? process.env.BUN_PORT ?? process.env.PORT ?? process.env.NODE_PORT ?? 3000);
    const hostname = options.hostname ?? '0.0.0.0';
    const bind = hostname === '0.0.0.0' || hostname === 'localhost' ? '::' : hostname;
    callFromHost ??= require('internal/bindings/task_queue').callFromHost;
    if (!goneHooked && typeof http.onGone === 'function') {
      goneHooked = true;
      const shared = require('internal/tov/http_server');
      shared.goneHandlers.push(onGone);
      if (!shared.isGoneHooked()) {
        shared.setGoneHooked();
        shared.hookGone(callFromHost);
      }
    }
    const id = http.listen(port, bind, false, (rid, method, target, wire, body, http10) => {
      callFromHost(onRequest, this, rid, method, target, wire, body, http10);
    });
    if (typeof id !== 'number') {
      const e = new Error(id.includes('in use') ? `Failed to start server. Is port ${port} in use?` : `Failed to start server: ${id}`);
      e.code = id.includes('in use') ? 'EADDRINUSE' : 'EACCES';
      e.syscall = 'listen';
      throw e;
    }
    this.#id = id;
    this.hostname = hostname === '0.0.0.0' || hostname === '::' ? 'localhost' : hostname;
    this.port = http.port(id);
    this.development = options.development ?? process.env.NODE_ENV !== 'production';
    this.id = options.id ?? '';
  }

  get url() {
    const host = this.hostname.includes(':') ? `[${this.hostname}]` : this.hostname;
    return new URL(`http://${host}:${this.port}/`);
  }

  get pendingRequests() { return this.#pending; }
  get pendingWebSockets() { return 0; }

  // (for the request handler, below)
  get _tovOptions() { return this.#options; }
  get _tovRoutes() { return this.#routes; }
  _tovPending(n) { this.#pending += n; }

  fetch(input, init) {
    const req = input instanceof Request ? input : new Request(input, init);
    return Promise.resolve(this.#options.fetch.call(this, req, this));
  }

  reload(options) {
    this.#options = { ...this.#options, ...options };
    if (options.routes !== undefined) this.#routes = compileRoutes(options.routes);
    return this;
  }

  stop(closeActiveConnections = false) {
    if (!this.#stopped) {
      this.#stopped = true;
      http.stop(this.#id, !!closeActiveConnections);
    }
    return Promise.resolve();
  }

  requestIP(request) {
    const id = requestIds.get(request);
    const fd = id === undefined ? -1 : http.fd(id);
    const peer = fd >= 0 && streams ? streams.name(fd, true) : null;
    if (!Array.isArray(peer)) return null;
    return { address: peer[0], family: peer[1], port: peer[2] };
  }

  timeout() {}
  upgrade() { return false; }
  publish() { return 0; }
  subscriberCount() { return 0; }
  ref() {}
  unref() {}
  async [Symbol.asyncDispose]() { await this.stop(); }
}

// routes: { "/path/:param/*": Response | handler | { GET: ..., POST: ... } }
function compileRoutes(routes) {
  if (!routes) return null;
  const compiled = [];
  for (const [path, value] of Object.entries(routes)) {
    const names = [];
    const pattern = path.replace(/[.+?^${}()|[\]\\]/g, '\\$&').replace(/:([A-Za-z0-9_]+)/g, (_, name) => {
      names.push(name);
      return '([^/]+)';
    }).replace(/\*/g, '.*');
    compiled.push({ re: new RegExp(`^${pattern}$`), names, value, exact: !path.includes(':') && !path.includes('*') });
  }
  // (exact paths first, then parameters, then wildcards)
  compiled.sort((a, b) => (b.exact - a.exact) || (a.re.source.includes('.*') - b.re.source.includes('.*')));
  return compiled;
}

function matchRoute(server, req, pathname) {
  const routes = server._tovRoutes;
  if (!routes) return undefined;
  for (const r of routes) {
    const m = r.re.exec(pathname);
    if (!m) continue;
    let value = r.value;
    if (value && typeof value === 'object' && !(value instanceof Response)) {
      value = value[req.method] ?? (req.method === 'HEAD' ? value.GET : undefined);
      if (value === undefined) continue;
    }
    const params = {};
    r.names.forEach((n, i) => { params[n] = decodeURIComponent(m[i + 1]); });
    req.params = params;
    return value;
  }
  return undefined;
}

function onRequest(server, id, method, target, wire, body, http10) {
  const { serverRequest } = fetchApi().internals;
  let host = '';
  const h = wire.search(/(^|\n)host:/i);
  if (h >= 0) {
    const start = wire.indexOf(':', h) + 1;
    const end = wire.indexOf('\r', start);
    host = wire.slice(start, end < 0 ? undefined : end).trim();
  }
  const url = target.charCodeAt(0) === 47 ? `http://${host || `${server.hostname}:${server.port}`}${target}` : target;
  const req = serverRequest(url, method, wire, body === null ? null : body, null);
  requestIds.set(req, id);
  live.set(id, req);
  const connection = /(^|\n)connection:([^\r]*)/i.exec(wire)?.[2].toLowerCase() ?? '';
  const keep = http10 ? connection.includes('keep-alive') : !connection.includes('close');
  server._tovPending(1);
  let result;
  try {
    const options = server._tovOptions;
    let handler = options.fetch;
    if (server._tovRoutes) {
      const q = target.indexOf('?');
      const route = matchRoute(server, req, q < 0 ? target : target.slice(0, q));
      if (route !== undefined) handler = route;
    }
    if (handler instanceof Response) result = handler.clone();
    else if (typeof handler === 'function') result = handler.call(server, req, server);
    else result = new Response('Not Found', { status: 404 });
  } catch (e) {
    result = onError(server, e);
  }
  if (result !== null && typeof result === 'object' && typeof result.then === 'function') {
    result.then((res) => respond(server, id, res, method, keep, http10), (e) => {
      Promise.resolve(onError(server, e)).then((res) => respond(server, id, res, method, keep, http10), (e2) => fail(server, id, e2));
    });
  } else {
    respond(server, id, result, method, keep, http10);
  }
}

function onError(server, error) {
  const handler = server._tovOptions.error;
  if (typeof handler === 'function') {
    try {
      return handler.call(server, error);
    } catch (e) {
      error = e;
    }
  }
  console.error(error);
  return new Response('Internal Server Error', { status: 500, headers: { 'content-type': 'text/plain;charset=utf-8' } });
}

function fail(server, id, error) {
  live.delete(id);
  console.error(error);
  http.write(id, 'HTTP/1.1 500 Internal Server Error\r\ncontent-length: 0\r\nconnection: close\r\n\r\n', 2, true);
  server._tovPending(-1);
}

function respond(server, id, res, method, keep, http10) {
  // (the response is underway: a client leaving now just cuts it short)
  live.delete(id);
  if (!(res instanceof Response)) {
    if (res !== null && typeof res === 'object' && typeof res.then === 'function') {
      res.then((r) => respond(server, id, r, method, keep, http10), (e) => fail(server, id, e));
      return;
    }
    return respond(server, id, onError(server, new TypeError(`Expected a Response object, but received '${res}'`)), method, keep, http10);
  }
  const { kState, kHeaders, kDefaulted, kBody, bodyStream, FormBody } = fetchApi().internals;
  const state = res[kState];
  const status = state.status;
  let head = `HTTP/1.1 ${status} ${state.statusText || STATUS[status] || ''}\r\n`;
  let length = -1;
  let hasDate = false;
  const type = wireType(res, kState, kHeaders, kDefaulted, kBody);
  if (type !== undefined && !res[kHeaders].has('content-type')) head += `content-type: ${type}\r\n`;
  for (const [k, [name, values]] of res[kHeaders][kHeaders]) {
    if (k === 'content-type' && type !== undefined) {
      head += `${name}: ${type}\r\n`;
      continue;
    }
    if (k === 'content-length') length = Number(values[0]);
    else if (k === 'date') hasDate = true;
    else if (k === 'connection' || k === 'keep-alive') continue;
    else if (k === 'transfer-encoding') continue;   // (the body's framing is ours)
    if (k === 'set-cookie') for (const v of values) head += `${name}: ${v}\r\n`;
    else head += `${name}: ${values.length === 1 ? values[0] : values.join(', ')}\r\n`;
  }
  if (!hasDate) head += `Date: ${httpDate()}\r\n`;
  if (!keep) head += 'Connection: close\r\n';
  const end = keep ? 1 : 2;
  const bodyState = res[kBody];
  let body = bodyState.stream ?? bodyState.body;
  if (bodyState.fetched) body = bodyStream(bodyState);
  const noBody = method === 'HEAD' || NULL_BODY.has(status) || (status >= 100 && status < 200);
  if (body === null || body === undefined) {
    if (length < 0 && !NULL_BODY.has(status) && !(status >= 100 && status < 200)) head += 'Content-Length: 0\r\n';
    http.write(id, `${head}\r\n`, end, true);
    server._tovPending(-1);
    return;
  }
  bodyState.used = true;
  if (body instanceof Uint8Array) {
    if (length < 0) head += `Content-Length: ${body.byteLength}\r\n`;
    if (noBody) {
      http.write(id, `${head}\r\n`, end, true);
    } else {
      http.write(id, `${head}\r\n`, 0, true);
      http.write(id, body, end, false);
    }
    server._tovPending(-1);
    return;
  }
  if (body instanceof FormBody) {
    fetchApi().internals.encodeForm(body).then((bytes) => {
      if (length < 0) head += `Content-Length: ${bytes.byteLength}\r\n`;
      http.write(id, `${head}\r\n`, 0, true);
      http.write(id, noBody ? null : bytes, end, false);
      server._tovPending(-1);
    }, (e) => fail(server, id, e));
    return;
  }
  let stream;
  if (body instanceof Blob()) {
    if (length < 0) length = body.size, head += `Content-Length: ${length}\r\n`;
    if (noBody) {
      http.write(id, `${head}\r\n`, end, true);
      server._tovPending(-1);
      return;
    }
    stream = body.stream();
  } else {
    stream = body;
    if (noBody) {
      http.write(id, `${head}\r\n`, end, true);
      stream.cancel?.().catch?.(() => {});
      server._tovPending(-1);
      return;
    }
    if (length < 0) {
      // (a client on HTTP/1.0 gets the body until the connection closes)
      if (http10) head = head.replace('Connection: close\r\n', '') + 'Connection: close\r\n';
      else head += 'Transfer-Encoding: chunked\r\n';
    }
  }
  const chunked = length < 0 && !http10;
  http.write(id, `${head}\r\n`, 0, true);
  pump(server, id, stream, chunked, length < 0 && http10 ? 2 : end);
}

async function pump(server, id, stream, chunked, end) {
  const reader = stream.getReader();
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      const chunk = typeof value === 'string' ? Buffer.from(value, 'utf8') : ArrayBuffer.isView(value) ? value : new Uint8Array(value);
      if (chunk.byteLength === 0) continue;
      let buffered;
      if (chunked) {
        buffered = http.writeChunk(id, chunk);
      } else {
        http.write(id, chunk, 0, false);
        buffered = http.buffered(id);
      }
      while (buffered > HIGH_WATER) {
        await sleep(1);
        buffered = http.buffered(id);
      }
      if (buffered < 0) {
        reader.cancel().catch(() => {});
        return;
      }
    }
    http.write(id, chunked ? '0\r\n\r\n' : null, end, true);
  } catch (e) {
    console.error(e);
    // (the body failed partway: the connection closes, so the client sees it cut short)
    http.write(id, null, 2, true);
  } finally {
    server._tovPending(-1);
  }
}

// ---------------------------------------------------------------- Bun

const Bun = {
  version: '1.4.0',
  revision: '',
  get env() { return process.env; },
  set env(v) { process.env = v; },
  get argv() { return process.argv; },
  get main() { return process.argv[1]; },
  serve,
  file,
  write,
  spawn,
  spawnSync,
  sleep,
  sleepSync,
  which,
  semver,
  escapeHTML,
  nanoseconds: () => Number(process.hrtime.bigint()),
  gc() {},
  inspect(...args) { return require('util').inspect(...args); },
  deepEquals(a, b) { return require('util').isDeepStrictEqual(a, b); },
  fileURLToPath(url) { return require('url').fileURLToPath(url); },
  pathToFileURL(path) { return require('url').pathToFileURL(path); },
  get stdin() { return file('/dev/stdin'); },
  get stdout() { return file('/dev/stdout'); },
  get stderr() { return file('/dev/stderr'); },
  isMainThread: true,
};
Object.defineProperty(Bun, Symbol.toStringTag, { value: 'Bun', configurable: true });

module.exports = Bun;
