'use strict';

// internalBinding('buffer'): what Node.js does in C++ (src/node_buffer.cc) for Buffer, here in
// JS with the same results. The hot paths (UTF-8, base64, search) move to natives later; the
// results must stay these.

// Encodings, numbered as internalBinding('string_decoder').encodings lists them
const ASCII = 0, UTF8 = 1, BASE64 = 2, UCS2 = 3, LATIN1 = 4, HEX = 5, BUFFER = 6, BASE64URL = 7;

const kMaxLength = 9007199254740991;
const kStringMaxLength = 536870888;

function outOfRange() {
  const e = new RangeError('Index out of range');
  e.code = 'ERR_OUT_OF_RANGE';
  return e;
}

// start/end as the C++ slices take them: defaults 0 and the length; end before start is empty
function bounds(buf, start, end) {
  const len = buf.length;
  start = start === undefined ? 0 : Math.trunc(Number(start)) || 0;
  end = end === undefined ? len : Math.trunc(Number(end)) || 0;
  if (start < 0 || end < 0) throw outOfRange();
  if (end < start) end = start;
  if (end > len) throw outOfRange();
  return [start, end];
}

// ---- UTF-8

const nativeUtf8 = globalThis.__tov_native?.buffer;
const nativeUtf8Length = nativeUtf8?.utf8Length;
const nativeUtf8Write = nativeUtf8?.utf8Write;

function byteLengthUtf8(s) {
  // (natively, but for a few characters: a call into native code costs more than they do)
  if (s.length > 32 && nativeUtf8Length !== undefined) return nativeUtf8Length(s);
  let n = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 0x80) n += 1;
    else if (c < 0x800) n += 2;
    else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
      const d = s.charCodeAt(i + 1);
      if (d >= 0xdc00 && d <= 0xdfff) { n += 4; i++; } else n += 3;
    } else n += 3;
  }
  return n;
}

// Encodes s into buf at offset, up to max bytes, never splitting a character (lone surrogates
// become U+FFFD); returns the bytes written
// (`read`, if given, gets the UTF-16 code units consumed at [0])
function utf8Encode(buf, s, offset, max, read) {
  if (s.length > 32 && read === undefined && nativeUtf8Write !== undefined) {
    const n = nativeUtf8Write(buf, s, offset, max);
    if (n >= 0) return n;
  }
  let o = offset;
  const end = offset + max;
  let i = 0;
  for (; i < s.length; i++) {
    let c = s.charCodeAt(i);
    if (c < 0x80) {
      if (o >= end) break;
      buf[o++] = c;
      continue;
    }
    if (c >= 0xd800 && c <= 0xdfff) {
      const d = i + 1 < s.length ? s.charCodeAt(i + 1) : 0;
      if (c <= 0xdbff && d >= 0xdc00 && d <= 0xdfff) {
        if (o + 4 > end) break;
        const cp = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
        buf[o++] = 0xf0 | (cp >> 18);
        buf[o++] = 0x80 | ((cp >> 12) & 0x3f);
        buf[o++] = 0x80 | ((cp >> 6) & 0x3f);
        buf[o++] = 0x80 | (cp & 0x3f);
        i++;
        continue;
      }
      c = 0xfffd;
    }
    if (c < 0x800) {
      if (o + 2 > end) break;
      buf[o++] = 0xc0 | (c >> 6);
      buf[o++] = 0x80 | (c & 0x3f);
    } else {
      if (o + 3 > end) break;
      buf[o++] = 0xe0 | (c >> 12);
      buf[o++] = 0x80 | ((c >> 6) & 0x3f);
      buf[o++] = 0x80 | (c & 0x3f);
    }
  }
  if (read !== undefined) read[0] = i;
  return o - offset;
}

const fromCharCode = String.fromCharCode;
const CHUNK = 0x2000;

function codeUnitsToString(units, n) {
  if (n <= CHUNK) return fromCharCode.apply(null, n === units.length ? units : units.subarray(0, n));
  let s = '';
  for (let i = 0; i < n; i += CHUNK) s += fromCharCode.apply(null, units.subarray(i, Math.min(i + CHUNK, n)));
  return s;
}

const nativeUtf8Decode = globalThis.__tov_native?.buffer?.utf8Decode;

// UTF-8 to a string, invalid sequences replaced (WHATWG: one U+FFFD per maximal subpart):
// natively, but for a few bytes (a call into native code costs more than they do)
function utf8Decode(b, start, end) {
  if (end - start > 16 && nativeUtf8Decode !== undefined) return nativeUtf8Decode(b, start, end);
  return utf8DecodeJs(b, start, end);
}

function utf8DecodeJs(b, start, end) {
  const units = new Uint16Array(end - start);
  let n = 0;
  let i = start;
  while (i < end) {
    const c = b[i];
    if (c < 0x80) { units[n++] = c; i++; continue; }
    let need = 0, cp = 0, lo = 0x80, hi = 0xbf;
    if (c >= 0xc2 && c <= 0xdf) { need = 1; cp = c & 0x1f; }
    else if (c >= 0xe0 && c <= 0xef) { need = 2; cp = c & 0x0f; if (c === 0xe0) lo = 0xa0; else if (c === 0xed) hi = 0x9f; }
    else if (c >= 0xf0 && c <= 0xf4) { need = 3; cp = c & 0x07; if (c === 0xf0) lo = 0x90; else if (c === 0xf4) hi = 0x8f; }
    else { units[n++] = 0xfffd; i++; continue; }
    let j = 1;
    for (; j <= need; j++) {
      if (i + j >= end) break;
      const d = b[i + j];
      if (d < lo || d > hi) break;
      lo = 0x80; hi = 0xbf;
      cp = (cp << 6) | (d & 0x3f);
    }
    if (j <= need) { units[n++] = 0xfffd; i += j; continue; }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      units[n++] = 0xd800 + (cp >> 10);
      units[n++] = 0xdc00 + (cp & 0x3ff);
    } else {
      units[n++] = cp;
    }
    i += need + 1;
  }
  return codeUnitsToString(units, n);
}

function isUtf8(view) {
  const b = toBytes(view);
  let i = 0;
  while (i < b.length) {
    const c = b[i];
    if (c < 0x80) { i++; continue; }
    let need, lo = 0x80, hi = 0xbf;
    if (c >= 0xc2 && c <= 0xdf) need = 1;
    else if (c >= 0xe0 && c <= 0xef) { need = 2; if (c === 0xe0) lo = 0xa0; else if (c === 0xed) hi = 0x9f; }
    else if (c >= 0xf0 && c <= 0xf4) { need = 3; if (c === 0xf0) lo = 0x90; else if (c === 0xf4) hi = 0x8f; }
    else return false;
    for (let j = 1; j <= need; j++) {
      const d = b[i + j];
      if (d === undefined || d < lo || d > hi) return false;
      lo = 0x80; hi = 0xbf;
    }
    i += need + 1;
  }
  return true;
}

function isAscii(view) {
  const b = toBytes(view);
  for (let i = 0; i < b.length; i++) if (b[i] & 0x80) return false;
  return true;
}

function toBytes(view) {
  if (view instanceof Uint8Array) return view;
  if (ArrayBuffer.isView(view)) return new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
  return new Uint8Array(view);
}

// ---- latin1, ascii, ucs2, hex

function latin1Decode(b, start, end, mask) {
  const units = new Uint16Array(end - start);
  for (let i = start; i < end; i++) units[i - start] = b[i] & mask;
  return codeUnitsToString(units, end - start);
}

function latin1Encode(buf, s, offset, max) {
  const n = Math.min(s.length, max);
  for (let i = 0; i < n; i++) buf[offset + i] = s.charCodeAt(i);
  return n;
}

function ucs2Decode(b, start, end) {
  const n = (end - start) >> 1;
  const units = new Uint16Array(n);
  for (let i = 0; i < n; i++) units[i] = b[start + 2 * i] | (b[start + 2 * i + 1] << 8);
  return codeUnitsToString(units, n);
}

function ucs2Encode(buf, s, offset, max) {
  const n = Math.min(s.length, max >> 1);
  for (let i = 0; i < n; i++) {
    const c = s.charCodeAt(i);
    buf[offset + 2 * i] = c & 0xff;
    buf[offset + 2 * i + 1] = c >> 8;
  }
  return n * 2;
}

const hexChars = '0123456789abcdef';
const hexTable = [];
for (let i = 0; i < 256; i++) hexTable.push(hexChars[i >> 4] + hexChars[i & 15]);

function hexDecode(b, start, end) {
  let s = '';
  for (let i = start; i < end; i++) s += hexTable[b[i]];
  return s;
}

function hexValue(c) {
  if (c >= 48 && c <= 57) return c - 48;
  if (c >= 97 && c <= 102) return c - 87;
  if (c >= 65 && c <= 70) return c - 55;
  return -1;
}

// Writes pairs of hex digits until one isn't (or the room runs out); returns the bytes written
function hexEncode(buf, s, offset, max) {
  const n = Math.min(s.length >> 1, max);
  let i = 0;
  for (; i < n; i++) {
    const a = hexValue(s.charCodeAt(2 * i)), b = hexValue(s.charCodeAt(2 * i + 1));
    if (a < 0 || b < 0) break;
    buf[offset + i] = (a << 4) | b;
  }
  return i;
}

// ---- base64

const b64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const b64url = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
// both alphabets decode in either encoding, as in Node.js
const b64Value = new Int8Array(256).fill(-1);
for (let i = 0; i < 64; i++) {
  b64Value[b64.charCodeAt(i)] = i;
  b64Value[b64url.charCodeAt(i)] = i;
}

function base64Decode(b, start, end, url) {
  const alphabet = url ? b64url : b64;
  let s = '';
  let i = start;
  for (; i + 2 < end; i += 3) {
    const v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
    s += alphabet[v >> 18] + alphabet[(v >> 12) & 63] + alphabet[(v >> 6) & 63] + alphabet[v & 63];
  }
  const rest = end - i;
  if (rest === 1) {
    const v = b[i] << 16;
    s += alphabet[v >> 18] + alphabet[(v >> 12) & 63] + (url ? '' : '==');
  } else if (rest === 2) {
    const v = (b[i] << 16) | (b[i + 1] << 8);
    s += alphabet[v >> 18] + alphabet[(v >> 12) & 63] + alphabet[(v >> 6) & 63] + (url ? '' : '=');
  }
  return s;
}

// Decodes base64 or base64url, skipping characters outside both alphabets and stopping at
// '=' (Node.js's forgiving decoder); returns the bytes written
function base64Encode(buf, s, offset, max) {
  let acc = 0, bits = 0, o = offset;
  const end = offset + max;
  for (let i = 0; i < s.length && o < end; i++) {
    const c = s.charCodeAt(i);
    if (c === 61) break; // '='
    const v = c < 256 ? b64Value[c] : -1;
    if (v < 0) continue;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      buf[o++] = (acc >> bits) & 0xff;
    }
  }
  return o - offset;
}

// The decoded length Node.js reserves for a base64 string (Buffer.byteLength)
function base64ByteLength(s) {
  let n = s.length;
  if (n > 0 && s.charCodeAt(n - 1) === 61) n--;
  if (n > 1 && s.charCodeAt(n - 1) === 61) n--;
  return (n * 3) >>> 2;
}

// ---- the binding's functions

function slice(decode) {
  return (buf, start, end) => {
    [start, end] = bounds(buf, start, end);
    return decode(buf, start, end);
  };
}

function write(encode) {
  return (buf, string, offset, length) => {
    if (typeof string !== 'string') throw new TypeError('argument must be a string');
    offset = offset === undefined ? 0 : offset >>> 0;
    if (offset > buf.length) throw outOfRange();
    const room = buf.length - offset;
    length = length === undefined ? room : Math.min(length >>> 0, room);
    return encode(buf, string, offset, length);
  };
}

// The bytes a string encodes to (for fill and indexOf)
function encodeString(s, enc) {
  let n;
  switch (enc) {
    case UTF8: n = byteLengthUtf8(s); break;
    case UCS2: n = s.length * 2; break;
    case HEX: n = s.length >> 1; break;
    case BASE64: case BASE64URL: n = base64ByteLength(s); break;
    default: n = s.length;
  }
  const out = new Uint8Array(n);
  let w;
  switch (enc) {
    case UTF8: w = utf8Encode(out, s, 0, n); break;
    case UCS2: w = ucs2Encode(out, s, 0, n); break;
    case HEX: w = hexEncode(out, s, 0, n); break;
    case BASE64: case BASE64URL: w = base64Encode(out, s, 0, n); break;
    default: w = latin1Encode(out, s, 0, n);
  }
  return w === n ? out : out.subarray(0, w);
}

const encodingIds = { utf8: UTF8, 'utf-8': UTF8, ucs2: UCS2, 'ucs-2': UCS2, utf16le: UCS2, 'utf-16le': UCS2, latin1: LATIN1, binary: LATIN1, ascii: ASCII, hex: HEX, base64: BASE64, base64url: BASE64URL };

// Fills buf[start, end) with value (a number, bytes, or a string in `encoding`); -1 when a
// string has no bytes to fill with, -2 when the range is out of bounds
function fill(buf, value, start, end, encoding) {
  if (start > end || end > buf.length) return -2;
  if (typeof value === 'number') {
    buf.fill(value & 255, start, end);
    return undefined;
  }
  let bytes;
  if (typeof value === 'string') {
    bytes = encodeString(value, encodingIds[String(encoding).toLowerCase()] ?? UTF8);
  } else {
    bytes = toBytes(value);
  }
  if (bytes.length === 0) return -1;
  for (let i = start; i < end; i += bytes.length) {
    buf.set(bytes.subarray(0, Math.min(bytes.length, end - i)), i);
  }
  return undefined;
}

// Where the search starts, as node_buffer.cc's IndexOfOffset: -1 when nothing can match
function indexOfOffset(length, offset, needleLength, forward) {
  if (offset < 0) {
    if (offset + length >= 0) return length + offset;
    return forward || needleLength === 0 ? 0 : -1;
  }
  if (offset + needleLength <= length) return offset;
  if (needleLength === 0) return length;
  return forward ? -1 : length - 1;
}

function search(hay, haystackLen, needle, offset, forward, step) {
  const nlen = needle.length;
  let pos = indexOfOffset(haystackLen, Math.trunc(offset), nlen, forward);
  if (nlen === 0) return pos;
  if (pos < 0 || haystackLen < nlen) return -1;
  if (forward) {
    for (let i = pos; i + nlen <= haystackLen; i += step) {
      if (matchAt(hay, i, needle)) return i;
    }
  } else {
    if (pos + nlen > haystackLen) pos = haystackLen - nlen;
    pos -= pos % step;
    for (let i = pos; i >= 0; i -= step) {
      if (matchAt(hay, i, needle)) return i;
    }
  }
  return -1;
}

function matchAt(hay, i, needle) {
  for (let j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) return false;
  return true;
}

function haystackLength(buf, end) {
  return end === undefined ? buf.length : Math.max(0, Math.min(buf.length, Math.trunc(end)));
}

function indexOfString(buf, needle, offset, enc, forward, end) {
  return search(buf, haystackLength(buf, end), encodeString(needle, enc), offset, forward, enc === UCS2 ? 2 : 1);
}

function indexOfBuffer(buf, needle, offset, enc, forward, end) {
  return search(buf, haystackLength(buf, end), toBytes(needle), offset, forward, enc === UCS2 ? 2 : 1);
}

function indexOfNumber(buf, value, offset, forward, end) {
  return search(buf, haystackLength(buf, end), Uint8Array.of(value & 255), offset, forward, 1);
}

function compareBytes(a, aStart, aEnd, b, bStart, bEnd) {
  const n = Math.min(aEnd - aStart, bEnd - bStart);
  for (let i = 0; i < n; i++) {
    const x = a[aStart + i], y = b[bStart + i];
    if (x !== y) return x < y ? -1 : 1;
  }
  const la = aEnd - aStart, lb = bEnd - bStart;
  return la < lb ? -1 : la > lb ? 1 : 0;
}

function compare(a, b) {
  a = toBytes(a);
  b = toBytes(b);
  return compareBytes(a, 0, a.length, b, 0, b.length);
}

function compareOffset(source, target, targetStart, sourceStart, targetEnd, sourceEnd) {
  source = toBytes(source);
  target = toBytes(target);
  if (sourceStart > source.length) throw outOfRange();
  if (targetStart > target.length) throw outOfRange();
  sourceEnd = Math.min(sourceEnd, source.length);
  targetEnd = Math.min(targetEnd, target.length);
  if (sourceStart >= sourceEnd) return targetStart >= targetEnd ? 0 : -1;
  if (targetStart >= targetEnd) return 1;
  return compareBytes(source, sourceStart, sourceEnd, target, targetStart, targetEnd);
}

function copy(source, target, targetStart, sourceStart, nb) {
  source = toBytes(source);
  target = toBytes(target);
  target.set(source.subarray(sourceStart, sourceStart + nb), targetStart);
  return nb;
}

function swap(buf, size) {
  for (let i = 0; i + size <= buf.length; i += size) {
    for (let a = i, b = i + size - 1; a < b; a++, b--) {
      const t = buf[a];
      buf[a] = buf[b];
      buf[b] = t;
    }
  }
  return buf;
}

// WHATWG btoa: -1 if a character isn't latin1
function btoa(s) {
  for (let i = 0; i < s.length; i++) if (s.charCodeAt(i) > 255) return -1;
  const bytes = new Uint8Array(s.length);
  latin1Encode(bytes, s, 0, s.length);
  return base64Decode(bytes, 0, bytes.length, false);
}

// WHATWG forgiving-base64 decode: -2 for an invalid character, -1 for a dangling one
function atob(s) {
  s = s.replace(/[\t\n\f\r ]/g, '');
  if (s.length % 4 === 0) {
    if (s.endsWith('==')) s = s.slice(0, -2);
    else if (s.endsWith('=')) s = s.slice(0, -1);
  }
  if (s.length % 4 === 1) return /[^A-Za-z0-9+/]/.test(s) ? -2 : -1;
  if (/[^A-Za-z0-9+/]/.test(s)) return -2;
  const bytes = new Uint8Array((s.length * 3) >> 2);
  const n = base64Encode(bytes, s, 0, bytes.length);
  return latin1Decode(bytes, 0, n, 0xff);
}

module.exports = {
  kMaxLength,
  kStringMaxLength,
  byteLengthUtf8,
  compare,
  compareOffset,
  copy,
  fill,
  isAscii,
  isUtf8,
  indexOfBuffer,
  indexOfNumber,
  indexOfString,
  swap16: (b) => swap(b, 2),
  swap32: (b) => swap(b, 4),
  swap64: (b) => swap(b, 8),
  atob,
  btoa,
  asciiSlice: slice((b, s, e) => latin1Decode(b, s, e, 0x7f)),
  latin1Slice: slice((b, s, e) => latin1Decode(b, s, e, 0xff)),
  base64Slice: slice((b, s, e) => base64Decode(b, s, e, false)),
  base64urlSlice: slice((b, s, e) => base64Decode(b, s, e, true)),
  hexSlice: slice(hexDecode),
  ucs2Slice: slice(ucs2Decode),
  utf8Slice: slice(utf8Decode),
  asciiWriteStatic: write(latin1Encode),
  latin1WriteStatic: write(latin1Encode),
  utf8WriteStatic: write(utf8Encode),
  base64Write: write(base64Encode),
  base64urlWrite: write(base64Encode),
  hexWrite: write(hexEncode),
  ucs2Write: write(ucs2Encode),
  // (for the other bindings: encoding_binding)
  utf8Encode,
  utf8Decode,
  createUnsafeArrayBuffer: (size) => new ArrayBuffer(size),
  // web streams: copy bytes between ArrayBuffers
  copyArrayBuffer(destination, destinationOffset, source, sourceOffset, bytesToCopy) {
    new Uint8Array(destination, destinationOffset, bytesToCopy).set(new Uint8Array(source, sourceOffset, bytesToCopy));
  },
  setDetachKey() {},
  getZeroFillToggle: () => undefined,
};
