'use strict';

const { LibuvStreamWrap, kNative } = require('internal/bindings/stream_wrap');
const native = globalThis.__tov_native ?? require('internal/bootstrap/host_fallback');

class TTY extends LibuvStreamWrap {
  constructor(fd, ctx = {}) {
    super();
    if (!Number.isInteger(fd) || fd < 0 || fd > 2147483647 || !native.isatty(fd)) {
      ctx.code = 'EINVAL';
      ctx.errno = -22;
      ctx.syscall = 'uv_tty_init';
      return;
    }
    this._tovOpen(fd);
    if (this.fd < 0) {
      ctx.code = 'EBADF';
      ctx.errno = -9;
      ctx.syscall = 'uv_tty_init';
    }
  }

  setRawMode(flag) {
    return this[kNative] ? native.stream.setRawMode(this[kNative], !!flag) : -9;
  }

  getWindowSize(out) {
    if (this.fd < 0) return -9;
    const size = native.windowSize(this.fd);
    if (!size) return -25; // UV_ENOTTY
    out[0] = size[0];
    out[1] = size[1];
    return 0;
  }
}

module.exports = { TTY, isTTY: (fd) => native.isatty(fd) };
