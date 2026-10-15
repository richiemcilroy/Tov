'use strict';

// The `process` global, over Tov's natives (globalThis.__tov_native, which runtime/node.c
// installs). It costs one native call to make: the fields are plain values, and everything that
// needs another module (EventEmitter, nextTick, warnings, stdio) loads it on first use, so a
// package that only reads process.env starts nothing else. The natives this uses:
//   info()          { argv, execArgv, execPath, pid, ppid, platform, arch, title }
//   env()           the environment, as an object
//   cwd() / chdir(path) / exit(code) / umask(mask)
//   hrtime()        nanoseconds since the program started (a number)
//   write(fd, data) data is a string or a Uint8Array; returns bytes written
//   isatty(fd) / windowSize(fd) -> [columns, rows]
//   memoryUsage()   { rss, heapTotal, heapUsed, external, arrayBuffers, available }
//   cpuUsage()      [user µs, system µs]
//   kill(pid, signal) / ids() -> [uid, gid, euid, egid]

// (outside Tov's runtime, e.g. a bare engine: the host_fallback natives)
const native = globalThis.__tov_native ?? require('internal/bootstrap/host_fallback');
const info = native.info();

const VERSION = 'v26.3.0';

const process = {};
Object.defineProperty(globalThis, 'process', { __proto__: null, value: process, writable: true, enumerable: false, configurable: true });
Object.defineProperty(process, Symbol.toStringTag, { __proto__: null, value: 'process', writable: false, enumerable: false, configurable: true });

function lazyProperty(name, make) {
  Object.defineProperty(process, name, {
    __proto__: null,
    enumerable: true,
    configurable: true,
    get() {
      const value = make();
      Object.defineProperty(process, name, { __proto__: null, value, writable: true, enumerable: true, configurable: true });
      return value;
    },
    set(value) {
      Object.defineProperty(process, name, { __proto__: null, value, writable: true, enumerable: true, configurable: true });
    },
  });
}

process.title = info.title ?? 'tov';
process.version = VERSION;
process.versions = {
  node: VERSION.slice(1),
  tov: info.tovVersion ?? '0.1.0',
  modules: '141',
  uv: '1.51.0',
  boringssl: '0.20260929.0',
  openssl: '3.0.13+quic',
  zlib: '1.3.1',
  brotli: '1.2.0',
  zstd: '1.5.7',
};
process.arch = info.arch;
process.platform = info.platform;
process.release = { name: 'node', lts: undefined };
process.argv = info.argv;
process.execArgv = info.execArgv ?? [];
process.argv0 = info.argv[0] ?? 'tov';
process.execPath = info.execPath;
// (~50 µs of strings: made when first read)
lazyProperty('env', () => {
  const env = native.env ? native.env() : info.env;
  // (assigning TZ changes the time zone at once, as in Node.js; it's listed once it has a value)
  if (native.setTZ) {
    let tz = env.TZ;
    const define = () => Object.defineProperty(env, 'TZ', {
      __proto__: null,
      enumerable: tz !== undefined,
      configurable: true,
      get: () => tz,
      set(v) {
        const listed = tz !== undefined;
        tz = `${v}`;
        native.setTZ(tz);
        if (!listed) define();
      },
    });
    define();
  }
  return env;
});
process.pid = info.pid;
process.ppid = info.ppid;
process.exitCode = undefined;
process.config = { target_defaults: {}, variables: { node_shared_openssl: false, v8_enable_i18n_support: 0 } };
process.features = { inspector: false, debug: false, uv: true, ipv6: true, tls_alpn: true, tls_sni: true, tls_ocsp: false, tls: true, cached_builtins: true, require_module: true, typescript: false };
process.debugPort = 9229;
process.throwDeprecation = false;
process.noDeprecation = false;
process.traceDeprecation = false;
lazyProperty('allowedNodeEnvironmentFlags', () => new Set());

process.cwd = () => native.cwd();
process.chdir = (dir) => {
  require('internal/validators').validateString(dir, 'directory');
  native.chdir(dir);
};
process.umask = (mask) => native.umask(mask === undefined ? -1 : Number(mask));
process.uptime = () => native.hrtime() / 1e9;

function hrtime(previous) {
  const ns = BigInt(Math.round(native.hrtime()));
  let s = Number(ns / 1000000000n);
  let n = Number(ns % 1000000000n);
  if (previous !== undefined) {
    s -= previous[0];
    n -= previous[1];
    if (n < 0) {
      s--;
      n += 1e9;
    }
  }
  return [s, n];
}
hrtime.bigint = () => BigInt(Math.round(native.hrtime()));
process.hrtime = hrtime;

// (the heap's figures cost a walk over it: read at most once a second)
let heapReadAt = -Infinity;
let heap = null;
process.memoryUsage = () => {
  const usage = native.memoryUsage();
  if (native.heapStats) {
    const now = Date.now();
    if (now - heapReadAt >= 1000) {
      heap = native.heapStats();
      heapReadAt = now;
    }
    const [size, capacity, extra] = heap;
    usage.heapTotal = capacity;
    usage.heapUsed = size;
    usage.external = extra;
    usage.arrayBuffers = extra;
  }
  return usage;
};
process.memoryUsage.rss = () => native.memoryUsage().rss;
process.cpuUsage = (previous) => {
  const [user, system] = native.cpuUsage();
  return previous ? { user: user - previous.user, system: system - previous.system } : { user, system };
};
process.resourceUsage = () => {
  const [userCPUTime, systemCPUTime] = native.cpuUsage();
  return { userCPUTime, systemCPUTime, maxRSS: Math.round(native.memoryUsage().rss / 1024) };
};
process.constrainedMemory = () => 0;
process.availableMemory = () => native.memoryUsage().available ?? 0;

const ids = () => native.ids();
process.getuid = () => ids()[0];
process.getgid = () => ids()[1];
process.geteuid = () => ids()[2];
process.getegid = () => ids()[3];
process.getgroups = () => [ids()[1]];

process.kill = (pid, signal = 'SIGTERM') => {
  native.kill(pid, signal);
  return true;
};

process.exit = (code) => {
  if (code !== undefined) process.exitCode = code;
  const exitCode = process.exitCode ?? 0;
  if (!process._exiting) {
    process._exiting = true;
    if (emitter) process.emit('exit', exitCode);
  }
  native.exit(process.exitCode ?? exitCode);
};
process.reallyExit = (code) => native.exit(code);
process.abort = () => native.exit(134);

// nextTick: the tick queue starts on first use
process.nextTick = function nextTick(...args) {
  const { nextTick } = require('internal/bootstrap/ticks')();
  process.nextTick = nextTick;
  return nextTick(...args);
};

process.emitWarning = (...args) => require('internal/process/warning').emitWarning(...args);
process.binding = (name) => {
  const e = new Error(`No such module: ${name}`);
  e.code = 'ERR_INVALID_MODULE';
  throw e;
};
process._getActiveHandles = () => [];
process._getActiveRequests = () => [];
process.getActiveResourcesInfo = () => [];
process.setUncaughtExceptionCaptureCallback = (fn) => { process._uncaughtCapture = fn; };
process.hasUncaughtExceptionCaptureCallback = () => typeof process._uncaughtCapture === 'function';
process.setSourceMapsEnabled = () => {};
lazyProperty('report', () => ({ getReport: () => ({}) }));

// EventEmitter: process becomes one (events loads) when first used as one.
// Its own methods stay and always call EventEmitter's with process as the
// receiver, so a method read before or after that (a named import from
// node:process, read once at its first use) keeps working.
const emitterMethods = [
  'on', 'once', 'off', 'addListener', 'removeListener', 'removeAllListeners', 'emit', 'listeners',
  'rawListeners', 'listenerCount', 'prependListener', 'prependOnceListener', 'eventNames',
  'setMaxListeners', 'getMaxListeners',
];
let emitter = false;
function becomeEmitter() {
  if (emitter) return;
  emitter = true;
  const EventEmitter = require('events');
  Object.setPrototypeOf(process, Object.create(EventEmitter.prototype, {
    [Symbol.toStringTag]: { value: 'process', configurable: true },
  }));
  EventEmitter.init.call(process);
  // signals: the first listener for one starts catching it, the last one's removal stops
  // (Node.js's startListeningIfSignal and stopListeningIfSignal)
  if (typeof native.signal === 'function') {
    native.signalHandler((name, number) => {
      require('internal/bindings/task_queue').callFromHost(() => process.emit(name, name, number));
    });
    process.on('newListener', (type) => {
      if (typeof type !== 'string' || !type.startsWith('SIG') || process.listenerCount(type) > 0) return;
      const err = native.signal(type, true);
      if (err === -22 && signalNumber(type) !== undefined) {
        const e = new Error('uv_signal_start EINVAL');
        e.errno = -22;
        e.code = 'EINVAL';
        e.syscall = 'uv_signal_start';
        throw e;
      }
    });
    process.on('removeListener', (type) => {
      if (typeof type === 'string' && type.startsWith('SIG') && process.listenerCount(type) === 0) native.signal(type, false);
    });
  }
}

function signalNumber(name) {
  return require('os').constants.signals[name];
}
for (const m of emitterMethods) {
  Object.defineProperty(process, m, {
    __proto__: null,
    enumerable: false,
    configurable: true,
    writable: true,
    value: function (...args) {
      becomeEmitter();
      return require('events').prototype[m].apply(process, args);
    },
  });
}
// uncaught exceptions go to 'uncaughtException' listeners, if there are any
native.setFatalHandler?.((err) => {
  if (!emitter || process.listenerCount('uncaughtException') === 0) return false;
  process.emit('uncaughtException', err, 'uncaughtException');
  return true;
});

// the program's natural end: process.exitCode sets the status unless main returned one, and
// 'exit' listeners run (and may still change process.exitCode), as in Node.js
native.setExitHandler?.((code) => {
  if (code === 0 && process.exitCode !== undefined) code = process.exitCode;
  if (!process._exiting) {
    process._exiting = true;
    if (emitter) {
      process.exitCode = code;
      process.emit('exit', code);
      code = process.exitCode;
    }
  }
  return Number(code) | 0;
});

// for the host: whether anything can be listening (to 'exit' and 'beforeExit')
Object.defineProperty(process, '_tovEmitter', { __proto__: null, get: () => emitter, enumerable: false, configurable: true });

// stdio: synchronous writes to fds 1 and 2 (as Node.js does for files, and pipes on macOS)
function stdioStream(fd) {
  if (native.isatty(fd)) {
    const { WriteStream } = require('tty');
    return new WriteStream(fd);
  }
  const { Writable } = require('stream');
  const stream = new Writable({
    write(chunk, encoding, cb) {
      native.write(fd, typeof chunk === 'string' ? chunk : new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength));
      cb();
    },
    decodeStrings: false,
  });
  stream.fd = fd;
  stream._type = 'fs';
  stream._isStdio = true;
  stream.destroySoon = stream.destroy;
  stream._destroy = (err, cb) => cb(err);
  return stream;
}
lazyProperty('stdout', () => stdioStream(1));
lazyProperty('stderr', () => stdioStream(2));
lazyProperty('stdin', () => (native.stdin ? native.stdin() : new (require('stream').Readable)({ read() { this.push(null); } })));

module.exports = process;
