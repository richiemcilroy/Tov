'use strict';

// internalBinding('fs'): Node.js's src/node_file.cc over Tov's natives
// (globalThis.__tov_native.fs, in runtime/node.c), which are synchronous and throw Node.js's
// errors. Each binding function is called three ways, by its last argument:
//   undefined          synchronous: return the result or throw
//   an FSReqCallback   call req.oncomplete(err, result) on a later loop turn
//   kUsePromises       return a promise, settled on a later loop turn
// (Callbacks and promises complete on the next turn; reads and writes into buffers run on the
// runtime's I/O threads meanwhile, the rest synchronously.)

const native = globalThis.__tov_native?.fs;

const kUsePromises = Symbol('fs_use_promises_symbol');
const kFsStatsFieldsNumber = 18;

class FSReqCallback {
  constructor(bigint) {
    this.bigint = !!bigint;
    this.oncomplete = undefined;
    this.context = undefined;
  }
}

function unavailable() {
  const e = new Error('The file system is not available outside Tov\'s runtime');
  e.code = 'ERR_METHOD_NOT_IMPLEMENTED';
  throw e;
}

function fs() {
  return native ?? unavailable();
}

let setImmediate;
function later(fn) {
  setImmediate ??= require('timers').setImmediate;
  setImmediate(fn);
}

// Runs fn as the call's last argument (req) asks
function run(req, fn) {
  if (req === undefined || req === null) return fn();
  if (req === kUsePromises) {
    return new Promise((resolve, reject) => {
      later(() => {
        try {
          resolve(fn());
        } catch (e) {
          reject(e);
        }
      });
    });
  }
  later(() => {
    let result;
    try {
      result = fn();
    } catch (e) {
      req.oncomplete(e);
      return;
    }
    req.oncomplete(null, result);
  });
  return undefined;
}

// A read or write with a callback or promise: on one of the runtime's I/O threads, as Node.js's
// pool does it, the loop going on meanwhile (a download writing each chunk where it goes would
// otherwise spend the loop in pwrite); `sync` when no thread can start
function io(req, write, fd, buffer, offset, length, pos, sync) {
  if (req === undefined || req === null || typeof native?.io !== 'function') return run(req, sync);
  let resolve, reject;
  const promise = req === kUsePromises ? new Promise((ok, no) => { resolve = ok; reject = no; }) : undefined;
  const done = (err, n) => {
    if (req !== kUsePromises) req.oncomplete(err, n);
    else if (err) reject(err);
    else resolve(n);
  };
  let started;
  try {
    started = native.io(write, fd, buffer, offset, length, position(pos), done);
  } catch (e) {
    // (a bad argument: reported as the synchronous call reports it, on a later turn)
    return run(req, () => { throw e; });
  }
  return started ? promise : run(req, sync);
}

// Older call sites pass a ctx object to fill instead of throwing
function withCtx(ctx, fn) {
  if (ctx === undefined || ctx === null || typeof ctx !== 'object') return fn();
  try {
    return fn();
  } catch (e) {
    ctx.errno = e.errno;
    ctx.code = e.code;
    ctx.syscall = e.syscall;
    if (e.path !== undefined) ctx.path = e.path;
    if (e.dest !== undefined) ctx.dest = e.dest;
    if (ctx.errno === undefined) ctx.error = e;
    return undefined;
  }
}

const pathOf = (p) => (typeof p === 'string' ? p : p instanceof Uint8Array ? p : String(p));
const position = (p) => (p === null || p === undefined ? -1 : typeof p === 'bigint' ? Number(p) : p);

function stats(values, bigint) {
  if (values === undefined) return undefined;
  if (bigint) {
    const out = new BigInt64Array(kFsStatsFieldsNumber);
    for (let i = 0; i < kFsStatsFieldsNumber; i++) out[i] = BigInt(Math.trunc(values[i]));
    return out;
  }
  return Float64Array.from(values);
}

let Buffer;
function encoded(s, encoding) {
  if (encoding !== 'buffer') return s;
  Buffer ??= require('buffer').Buffer;
  return Buffer.from(s);
}

function writeAll(fd, views, pos) {
  let total = 0;
  for (const v of views) {
    const n = fs().write(fd, v, 0, v.byteLength, pos);
    total += n;
    if (pos >= 0) pos += n;
    if (n < v.byteLength) break;
  }
  return total;
}

class FileHandle {
  constructor(fd) {
    this.fd = fd;
  }

  getAsyncId() {
    return -1;
  }

  close() {
    const fd = this.fd;
    this.fd = -1;
    return run(kUsePromises, () => fs().close(fd));
  }

  closeSync() {
    const fd = this.fd;
    this.fd = -1;
    fs().close(fd);
  }

  releaseFD() {
    const fd = this.fd;
    this.fd = -1;
    return fd;
  }
}

module.exports = {
  kUsePromises,
  kFsStatsFieldsNumber,
  FSReqCallback,
  FileHandle,
  statValues: new Float64Array(kFsStatsFieldsNumber * 2),
  bigintStatValues: new BigInt64Array(kFsStatsFieldsNumber * 2),
  statFsValues: new Float64Array(7),
  bigintStatFsValues: new BigInt64Array(7),

  access: (path, mode, req) => run(req, () => fs().access(pathOf(path), mode)),
  existsSync: (path) => (native ? native.exists(pathOf(path)) : false),
  open: (path, flags, mode, req) => run(req, () => fs().open(pathOf(path), flags, mode)),
  openFileHandle: (path, flags, mode, req) => run(req, () => new FileHandle(fs().open(pathOf(path), flags, mode))),
  close: (fd, req) => run(req, () => fs().close(fd)),
  read: (fd, buffer, offset, length, pos, req) => io(req, false, fd, buffer, offset, length, pos, () => fs().read(fd, buffer, offset, length, position(pos))),
  readBuffers: (fd, buffers, pos, req) => run(req, () => {
    let total = 0;
    let p = position(pos);
    for (const b of buffers) {
      const n = fs().read(fd, b, 0, b.byteLength, p);
      total += n;
      if (p >= 0) p += n;
      if (n < b.byteLength) break;
    }
    return total;
  }),
  writeBuffer: (fd, buffer, offset, length, pos, req, ctx) =>
    withCtx(ctx, () => io(req, true, fd, buffer, offset, length, pos, () => fs().write(fd, buffer, offset, length, position(pos)))),
  writeBuffers: (fd, buffers, pos, req, ctx) => withCtx(ctx, () => run(req, () => writeAll(fd, buffers, position(pos)))),
  writeString: (fd, string, pos, encoding, req, ctx) => withCtx(ctx, () => run(req, () => {
    Buffer ??= require('buffer').Buffer;
    const bytes = Buffer.from(string, encoding ?? 'utf8');
    return fs().write(fd, bytes, 0, bytes.byteLength, position(pos));
  })),
  readFileUtf8: (pathOrFd, flags) => fs().readFileUtf8(typeof pathOrFd === 'number' ? pathOrFd : pathOf(pathOrFd), flags),
  writeFileUtf8: (pathOrFd, data, flags, mode) =>
    fs().writeFileUtf8(typeof pathOrFd === 'number' ? pathOrFd : pathOf(pathOrFd), data, flags, mode),

  stat: (path, bigint, req, throwIfNoEntry) =>
    run(req, () => stats(fs().stat(pathOf(path), throwIfNoEntry !== false), bigint)),
  lstat: (path, bigint, req, throwIfNoEntry) =>
    run(req, () => stats(fs().lstat(pathOf(path), throwIfNoEntry !== false), bigint)),
  fstat: (fd, bigint, req, shouldNotThrow) => run(req, () => {
    try {
      return stats(fs().fstat(fd), bigint);
    } catch (e) {
      if (shouldNotThrow) return undefined;
      throw e;
    }
  }),
  statfs: (path, bigint, req) => run(req, () => {
    const v = fs().statfs(pathOf(path));
    return bigint ? BigInt64Array.from(v, (x) => BigInt(Math.trunc(x))) : Float64Array.from(v);
  }),
  internalModuleStat: (a, b) => (native ? native.internalModuleStat(pathOf(b ?? a)) : -2),

  readdir: (path, encoding, withTypes, req) => run(req, () => {
    const r = fs().readdir(pathOf(path), !!withTypes);
    if (encoding !== 'buffer') return r;
    return withTypes ? [r[0].map((n) => encoded(n, encoding)), r[1]] : r.map((n) => encoded(n, encoding));
  }),
  mkdir: (path, mode, recursive, req) => run(req, () => fs().mkdir(pathOf(path), mode, !!recursive)),
  mkdtemp: (prefix, encoding, req) => run(req, () => encoded(fs().mkdtemp(pathOf(prefix)), encoding)),
  rmdir: (path, req) => run(req, () => fs().rmdir(pathOf(path))),
  rmSync: (path, maxRetries, recursive, retryDelay) => fs().rm(pathOf(path), maxRetries, !!recursive, retryDelay),
  unlink: (path, req) => run(req, () => fs().unlink(pathOf(path))),
  rename: (from, to, req) => run(req, () => fs().rename(pathOf(from), pathOf(to))),
  link: (from, to, req) => run(req, () => fs().link(pathOf(from), pathOf(to))),
  symlink: (target, path, flags, req) => run(req, () => fs().symlink(pathOf(target), pathOf(path))),
  readlink: (path, encoding, req) => run(req, () => encoded(fs().readlink(pathOf(path)), encoding)),
  realpath: (path, encoding, req) => run(req, () => encoded(fs().realpath(pathOf(path)), encoding)),
  copyFile: (src, dest, mode, req) => run(req, () => fs().copyFile(pathOf(src), pathOf(dest), mode)),

  chmod: (path, mode, req) => run(req, () => fs().chmod(pathOf(path), mode)),
  fchmod: (fd, mode, req) => run(req, () => fs().fchmod(fd, mode)),
  lchmod: (path, mode, req) => run(req, () => fs().chmod(pathOf(path), mode)),
  chown: (path, uid, gid, req) => run(req, () => fs().chown(pathOf(path), uid, gid)),
  fchown: (fd, uid, gid, req) => run(req, () => fs().fchown(fd, uid, gid)),
  lchown: (path, uid, gid, req) => run(req, () => fs().lchown(pathOf(path), uid, gid)),
  utimes: (path, atime, mtime, req) => run(req, () => fs().utimes(pathOf(path), atime, mtime)),
  futimes: (fd, atime, mtime, req) => run(req, () => fs().futimes(fd, atime, mtime)),
  lutimes: (path, atime, mtime, req) => run(req, () => fs().lutimes(pathOf(path), atime, mtime)),
  fsync: (fd, req) => run(req, () => fs().fsync(fd)),
  fdatasync: (fd, req) => run(req, () => fs().fdatasync(fd)),
  ftruncate: (fd, len, req) => run(req, () => fs().ftruncate(fd, len)),
};
