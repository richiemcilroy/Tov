'use strict';

// internalBinding('encoding_binding'): UTF-8 for TextEncoder and TextDecoder (Node.js's
// src/encoding_binding.cc), on the UTF-8 code in the buffer binding.

const { utf8Encode, utf8Decode, byteLengthUtf8, isUtf8 } = require('internal/bindings/buffer');

// [UTF-16 code units read, bytes written] by the last encodeInto
const encodeIntoResults = new Uint32Array(2);
const read = [0];

function encodeInto(source, dest) {
  encodeIntoResults[1] = utf8Encode(dest, source, 0, dest.length, read);
  encodeIntoResults[0] = read[0];
}

const nativeUtf8Encode = globalThis.__tov_native?.buffer?.utf8Encode;

function encodeUtf8String(s) {
  if (s.length > 32 && nativeUtf8Encode !== undefined) return nativeUtf8Encode(s);
  const out = new Uint8Array(byteLengthUtf8(s));
  utf8Encode(out, s, 0, out.length);
  return out;
}

function bytesOf(input) {
  if (input instanceof Uint8Array) return input;
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  return new Uint8Array(input);
}

// UTF-8 to a string: invalid bytes become U+FFFD, or throw when fatal; a leading BOM is dropped
// unless ignoreBOM
function decodeUTF8(input, ignoreBOM, fatal) {
  const b = bytesOf(input);
  if (fatal && !isUtf8(b)) {
    const e = new TypeError('The encoded data was not valid for encoding utf-8');
    e.code = 'ERR_ENCODING_INVALID_ENCODED_DATA';
    throw e;
  }
  const start = !ignoreBOM && b.length >= 3 && b[0] === 0xef && b[1] === 0xbb && b[2] === 0xbf ? 3 : 0;
  return utf8Decode(b, start, b.length);
}

module.exports = {
  encodeIntoResults,
  encodeInto,
  encodeUtf8String,
  decodeUTF8,
  toASCII: (s) => s,
  toUnicode: (s) => s,
};
