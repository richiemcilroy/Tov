'use strict';

// tty (Tov's own; Node.js's builds on net.Socket and its tty_wrap handle): isatty, and the
// streams for terminal file descriptors, writing and reading through the natives.

const { primordials } = require('internal/bootstrap');
const { NumberIsInteger } = primordials;
const { Writable } = require('stream');
const { Socket } = require('net');
const { TTY } = require('internal/bindings/tty_wrap');
const { getColorDepth, hasColors } = require('internal/tty');

const native = globalThis.__tov_native ?? require('internal/bootstrap/host_fallback');

function isatty(fd) {
  return NumberIsInteger(fd) && fd >= 0 && fd <= 2147483647 && native.isatty(fd);
}

class WriteStream extends Writable {
  constructor(fd) {
    super({ decodeStrings: false });
    this.fd = fd;
    this._type = 'tty';
    this._isStdio = fd <= 2;
    this.isTTY = true;
    this._refreshSize();
  }

  _write(chunk, encoding, cb) {
    native.write(this.fd, typeof chunk === 'string' ? chunk : new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength));
    cb();
  }

  _refreshSize() {
    const [columns, rows] = native.windowSize(this.fd) ?? [80, 24];
    const changed = columns !== this.columns || rows !== this.rows;
    this.columns = columns;
    this.rows = rows;
    if (changed && this.listenerCount('resize') > 0) this.emit('resize');
  }

  getWindowSize() {
    return [this.columns, this.rows];
  }

  getColorDepth(env) {
    return getColorDepth(env);
  }

  hasColors(count, env) {
    return hasColors(count, env);
  }

  cursorTo(x, y, callback) {
    return require('readline').cursorTo(this, x, y, callback);
  }

  moveCursor(dx, dy, callback) {
    return require('readline').moveCursor(this, dx, dy, callback);
  }

  clearLine(dir, callback) {
    return require('readline').clearLine(this, dir, callback);
  }

  clearScreenDown(callback) {
    return require('readline').clearScreenDown(this, callback);
  }
}

class ReadStream extends Socket {
  constructor(fd, options = {}) {
    const ctx = {};
    const handle = new TTY(fd, ctx);
    if (ctx.code) {
      const e = new Error('TTY initialization failed');
      Object.assign(e, ctx);
      throw e;
    }
    super({ ...options, handle, readable: true, writable: false, manualStart: true });
    this.fd = fd;
    this.isRaw = false;
    this.isTTY = true;
  }

  setRawMode(flag) {
    flag = !!flag;
    const err = this._handle.setRawMode(flag);
    if (err) {
      const e = new Error(`setRawMode: errno ${err}`);
      e.code = 'ERR_TTY_INIT_FAILED';
      e.errno = err;
      throw e;
    }
    this.isRaw = flag;
    return this;
  }
}

module.exports = { isatty, ReadStream, WriteStream };
