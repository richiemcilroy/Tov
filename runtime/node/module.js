'use strict';

// module (Tov's own; Node.js's is its whole CommonJS and ES module loader): a Tov program's
// modules are bundled when it's built, so `require` finds them in the bundle
// (globalThis.__tov_modules). createRequire(from) gives a require that resolves as the bundler
// did: built-ins, then paths relative to `from`, then packages any bundled module required.

const path = require('path');
const {
  codes: { ERR_INVALID_ARG_TYPE, ERR_INVALID_ARG_VALUE, ERR_METHOD_NOT_IMPLEMENTED },
} = require('internal/errors');

const builtinModules = [
  '_http_agent', '_http_client', '_http_common', '_http_incoming', '_http_outgoing', '_http_server',
  '_tls_common', '_tls_wrap', 'assert', 'assert/strict', 'async_hooks', 'buffer', 'child_process',
  'cluster', 'console', 'constants', 'crypto', 'dgram', 'diagnostics_channel', 'dns', 'dns/promises',
  'domain', 'events', 'fs', 'fs/promises', 'http', 'http2', 'https', 'inspector',
  'inspector/promises', 'module', 'net', 'os', 'path', 'path/posix', 'path/win32', 'perf_hooks',
  'process', 'punycode', 'querystring', 'readline', 'readline/promises', 'repl', 'stream',
  'stream/consumers', 'stream/promises', 'stream/web', 'string_decoder', 'sys', 'timers',
  'timers/promises', 'tls', 'trace_events', 'tty', 'url', 'util', 'util/types', 'v8', 'vm', 'wasi',
  'worker_threads', 'zlib', 'node:sea', 'node:sqlite', 'node:test', 'node:test/reporters',
];
const builtinSet = new Set(builtinModules);

function isBuiltin(id) {
  return typeof id === 'string' && (builtinSet.has(id) || (id.startsWith('node:') && builtinSet.has(id.slice(5))));
}

const registry = () => globalThis.__tov_modules;

let byName;
function idOf(name) {
  const r = registry();
  if (!r) return undefined;
  if (byName === undefined || byName.size !== r.names.length) {
    byName = new Map();
    for (let i = 0; i < r.names.length; i++) byName.set(r.names[i], i);
  }
  return byName.get(name);
}

const extensions = ['', '.js', '.json', '.cjs', '.mjs', '.ts', '.tsx', '.jsx', '/index.js', '/index.json', '/index.cjs', '/index.mjs'];

function fromPath(from) {
  if (from instanceof URL) from = from.href;
  if (typeof from !== 'string') throw new ERR_INVALID_ARG_VALUE('filename', from, 'must be a file URL object, file URL string, or absolute path string');
  if (from.startsWith('file://')) from = decodeURIComponent(from.slice(7));
  return from;
}

// a module's id in the bundle, or undefined
function resolveId(spec, dir) {
  const r = registry();
  if (!r) return undefined;
  if (isBuiltin(spec)) return idOf('node:' + (spec.startsWith('node:') ? spec.slice(5) : spec));
  if (spec.startsWith('file://')) spec = decodeURIComponent(spec.slice(7));
  if (spec.startsWith('./') || spec.startsWith('../') || spec === '.' || spec === '..' || spec.startsWith('/')) {
    const base = path.resolve(dir, spec);
    for (const ext of extensions) {
      const id = idOf(base + ext);
      if (id !== undefined) return id;
    }
    return undefined;
  }
  // a package: as some bundled module required it, preferring the module nearest `dir`
  let best;
  let bestLen = -1;
  for (let i = 0; i < r.maps.length; i++) {
    const to = r.maps[i][spec];
    if (to === undefined) continue;
    const owner = path.dirname(r.names[i]);
    const len = dir === owner || dir.startsWith(owner + '/') ? owner.length : 0;
    if (len > bestLen) {
      best = to;
      bestLen = len;
    }
  }
  if (best !== undefined) return best;
  for (const base of ['/node_modules/' + spec, '/node_modules/' + spec + '/index']) {
    for (const ext of extensions) {
      const id = idOf(base + ext);
      if (id !== undefined) return id;
    }
  }
  return undefined;
}

function notFound(spec, from) {
  const e = new Error(`Cannot find module '${spec}'\nRequire stack:\n- ${from}`);
  e.code = 'MODULE_NOT_FOUND';
  e.requireStack = [from];
  return e;
}

function createRequire(filename) {
  const from = fromPath(filename);
  const dir = from.endsWith('/') ? from.slice(0, -1) || '/' : path.dirname(from);
  function require(spec) {
    if (typeof spec !== 'string') throw new ERR_INVALID_ARG_TYPE('id', 'string', spec);
    const id = resolveId(spec, dir);
    if (id === undefined) {
      // not in the bundle: on disk where the program runs (a computed name, a native addon)
      const found = registry()?.requireFrom?.(spec.startsWith('file://') ? decodeURIComponent(spec.slice(7)) : spec, from);
      if (found !== undefined) return found.exports;
      throw notFound(spec, from);
    }
    return registry().load(id);
  }
  require.resolve = function resolve(spec) {
    if (isBuiltin(spec)) return spec;
    const id = resolveId(spec, dir);
    if (id === undefined) {
      // not in the bundle: a file on disk where the program runs (a package's .wasm, .so, data)
      const found = registry()?.resolveFrom?.(spec.startsWith('file://') ? decodeURIComponent(spec.slice(7)) : spec, from);
      if (found !== undefined) return found;
      throw notFound(spec, from);
    }
    return registry().names[id];
  };
  require.resolve.paths = (spec) => (isBuiltin(spec) ? null : Module._nodeModulePaths(dir));
  require.cache = Module._cache;
  require.extensions = Module._extensions;
  require.main = undefined;
  return require;
}

function Module(id = '', parent) {
  this.id = id;
  this.path = path.dirname(id);
  this.exports = {};
  this.filename = null;
  this.loaded = false;
  this.children = [];
  this.paths = [];
  this.parent = parent;
}

Module.Module = Module;
Module.builtinModules = builtinModules;
Module.isBuiltin = isBuiltin;
Module.createRequire = createRequire;
Module._cache = { __proto__: null };
Module._pathCache = { __proto__: null };
Module._extensions = {
  __proto__: null,
  '.js'() {},
  '.json'() {},
  '.node'() {},
};
Module.globalPaths = [];
Module.wrapper = ['(function (exports, require, module, __filename, __dirname) { ', '\n});'];
Module.wrap = (script) => Module.wrapper[0] + script + Module.wrapper[1];
Module._nodeModulePaths = function _nodeModulePaths(from) {
  const out = [];
  let dir = path.resolve(from);
  for (;;) {
    if (!dir.endsWith('/node_modules')) out.push(path.join(dir, 'node_modules'));
    const up = path.dirname(dir);
    if (up === dir) break;
    dir = up;
  }
  return out;
};
Module._resolveFilename = function _resolveFilename(request, parent) {
  const from = parent?.filename ?? parent?.id ?? '/';
  return createRequire(from.startsWith('/') ? from : '/' + from).resolve(request);
};
Module._load = function _load(request, parent) {
  const from = parent?.filename ?? parent?.id ?? '/';
  return createRequire(from.startsWith('/') ? from : '/' + from)(request);
};
Module.prototype.require = function (id) {
  return createRequire(this.filename ?? this.id ?? '/')(id);
};
Module.prototype._compile = function () {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Module.prototype._compile');
};
Module.runMain = () => {};
Module.syncBuiltinESMExports = () => {};
Module.register = () => {};
Module.registerHooks = () => ({ deregister() {} });
Module.enableCompileCache = () => ({ status: 3, message: 'Tov compiles ahead of time' });
Module.getCompileCacheDir = () => undefined;
Module.flushCompileCache = () => {};
Module.constants = { compileCacheStatus: { __proto__: null, FAILED: 0, ENABLED: 1, ALREADY_ENABLED: 2, DISABLED: 3 } };
Module.findSourceMap = () => undefined;
Module.getSourceMapsSupport = () => ({ enabled: false, nodeModules: false, generatedCode: false });
Module.setSourceMapsSupport = () => {};
Module.findPackageJSON = () => undefined;
Module.stripTypeScriptTypes = () => {
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.stripTypeScriptTypes');
};
Module.SourceMap = class SourceMap {
  constructor(payload) {
    this.payload = payload;
  }
  findEntry() {
    return {};
  }
  findOrigin() {
    return {};
  }
};

module.exports = Module;
