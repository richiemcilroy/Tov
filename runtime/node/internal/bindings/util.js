'use strict';

// internalBinding('util'). What V8 alone can see (a promise's state, a proxy's target, the
// entries an iterator has left) isn't visible from JS: those answer as if there were nothing
// to show (`Promise { <pending> }`), until a native gives the real answer.

// Symbols by name, made on first use (V8's private symbols are ordinary ones here)
function symbolTable() {
  return new Proxy({ __proto__: null }, {
    get(cache, name) {
      return cache[name] ??= Symbol(String(name));
    },
  });
}
const privateSymbols = symbolTable();
const perIsolateSymbols = symbolTable();

// V8's PropertyFilter bits, and its promise states
const constants = {
  kPending: 0,
  kFulfilled: 1,
  kRejected: 2,
  kExiting: 0,
  kExitCode: 1,
  kHasExitCode: 2,
  ALL_PROPERTIES: 0,
  ONLY_WRITABLE: 1,
  ONLY_ENUMERABLE: 2,
  ONLY_CONFIGURABLE: 4,
  SKIP_STRINGS: 8,
  SKIP_SYMBOLS: 16,
  kDisallowCloneAndTransfer: 0,
  kTransferable: 1,
  kCloneable: 2,
};

function isArrayIndex(key) {
  if (typeof key !== 'string' || key === '') return false;
  const n = Number(key);
  return n >>> 0 === n && n !== 4294967295 && String(n) === key;
}

function getOwnNonIndexProperties(obj, filter) {
  const out = [];
  for (const key of Reflect.ownKeys(obj)) {
    if (isArrayIndex(key)) continue;
    if (typeof key === 'string' && filter & constants.SKIP_STRINGS) continue;
    if (typeof key === 'symbol' && filter & constants.SKIP_SYMBOLS) continue;
    if (filter & (constants.ONLY_ENUMERABLE | constants.ONLY_WRITABLE | constants.ONLY_CONFIGURABLE)) {
      const d = Reflect.getOwnPropertyDescriptor(obj, key);
      if (!d) continue;
      if (filter & constants.ONLY_ENUMERABLE && !d.enumerable) continue;
      if (filter & constants.ONLY_CONFIGURABLE && !d.configurable) continue;
      if (filter & constants.ONLY_WRITABLE && 'value' in d && !d.writable) continue;
    }
    out.push(key);
  }
  return out;
}

// V8's GetConstructorName: the name of the nearest constructor on the prototype chain
function getConstructorName(obj) {
  for (let p = obj; p !== null; p = Object.getPrototypeOf(p)) {
    const d = Object.getOwnPropertyDescriptor(p, 'constructor');
    if (d && typeof d.value === 'function' && typeof d.value.name === 'string' && d.value.name !== '') return d.value.name;
  }
  return 'Object';
}

// The modules Node.js's files load with defineLazyProperties: literal requires, so the bundler
// includes them
const lazyModules = {
  'internal/blob': () => require('internal/blob'),
  'internal/encoding': () => require('internal/encoding'),
  'internal/file': () => require('internal/file'),
  'internal/fs/dir': () => require('internal/fs/dir'),
  'internal/mime': () => require('internal/mime'),
  'internal/util/diff': () => require('internal/util/diff'),
  'internal/util/parse_args/parse_args': () => require('internal/util/parse_args/parse_args'),
  'internal/util/trace_sigint': () => require('internal/util/trace_sigint'),
};

// Lazily required properties: `target[key]` is require(id)[key], loaded on first use
function defineLazyProperties(target, id, keys, enumerable = true) {
  let mod;
  for (const key of keys) {
    let value;
    let set = false;
    Object.defineProperty(target, key, {
      __proto__: null,
      enumerable,
      configurable: true,
      get() {
        if (!set) {
          mod ??= lazyModules[id] ? lazyModules[id]() : require(id);
          value = mod[key];
          set = true;
        }
        return value;
      },
      set(v) {
        value = v;
        set = true;
      },
    });
  }
}

module.exports = {
  privateSymbols,
  perIsolateSymbols,
  constants,
  getOwnNonIndexProperties,
  getConstructorName,
  defineLazyProperties,
  getPromiseDetails: () => [constants.kPending],
  getProxyDetails: () => undefined,
  previewEntries: (obj, isIterator) => (isIterator ? [[], false] : []),
  getExternalValue: () => 0n,
  constructSharedArrayBuffer: (n) => new SharedArrayBuffer(n),
  // The binding returns Node's enum; internal/util exposes its string name.
  guessHandleType: (fd) => ['TCP', 'TTY', 'UDP', 'FILE', 'PIPE', 'UNKNOWN'].indexOf(
    Number.isInteger(fd) && fd >= 0 && fd <= 2147483647
      ? globalThis.__tov_native?.guessHandleType?.(fd) ?? 'UNKNOWN' : 'UNKNOWN'),
  isInsideNodeModules: () => false,
  sleep(ms) {
    const sab = new Int32Array(new SharedArrayBuffer(4));
    Atomics.wait(sab, 0, 0, ms);
  },
  shouldAbortOnUncaughtToggle: [0],
};
