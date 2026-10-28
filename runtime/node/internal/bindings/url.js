'use strict';

// internalBinding('url'): Node.js's src/node_url.cc, which parses with Ada. Here the parser is
// the WHATWG URL Standard's state machine as whatwg-url implements it
// (internal/deps/whatwg-url), and urlComponents are the offsets into the href that Ada's
// url_aggregator reports, which internal/url's getters slice:
//   protocol_end, username_end, host_start, host_end, port, pathname_start, search_start,
//   hash_start, scheme_type (omitted values are 2^32 - 1)

const usm = require('internal/deps/whatwg-url/url-state-machine');

const omitted = 4294967295;
const urlComponents = new Uint32Array(9);
const schemeTypes = { __proto__: null, http: 0, https: 2, ws: 3, ftp: 4, wss: 5, file: 6 };

let ERR_INVALID_URL;
function invalid(input, base) {
  ERR_INVALID_URL ??= require('internal/errors').codes.ERR_INVALID_URL;
  return new ERR_INVALID_URL(input, base);
}

// fills urlComponents for `url` and returns its href
function serialize(url) {
  const href = usm.serializeURL(url);
  const c = urlComponents;
  const protocolEnd = url.scheme.length + 1;
  c[0] = protocolEnd;
  if (url.host !== null) {
    let i = protocolEnd + 2;
    const usernameEnd = i + url.username.length;
    c[1] = usernameEnd;
    let hostStart = usernameEnd;
    if (url.password !== '') hostStart = usernameEnd + 1 + url.password.length;
    c[2] = hostStart;
    i = hostStart + (url.username !== '' || url.password !== '' ? 1 : 0);
    const hostEnd = i + usm.serializeHost(url.host).length;
    c[3] = hostEnd;
    if (url.port !== null) {
      c[4] = url.port;
      c[5] = hostEnd + 1 + String(url.port).length;
    } else {
      c[4] = omitted;
      c[5] = hostEnd;
    }
  } else {
    c[1] = protocolEnd;
    c[2] = protocolEnd;
    c[3] = protocolEnd;
    c[4] = omitted;
    // ("/." keeps a path that starts with "//" from reading as a host)
    c[5] = !usm.hasAnOpaquePath(url) && url.path.length > 1 && url.path[0] === '' ? protocolEnd + 2 : protocolEnd;
  }
  let end = href.length;
  if (url.fragment !== null) {
    end -= url.fragment.length + 1;
    c[7] = end;
  } else {
    c[7] = omitted;
  }
  c[6] = url.query !== null ? end - url.query.length - 1 : omitted;
  c[8] = schemeTypes[url.scheme] ?? 1;
  return href;
}

// The common case without the state machine: an http, https, ws or wss URL that's already
// what the parser would make of it, save the case of its scheme and host and a default port:
// an ASCII host (not punycode, and a number only as a canonical IPv4 address), no userinfo, a
// path without dot segments, and nothing to percent-encode. Anything else returns undefined and
// takes the full parser; what this returns is what that would.
const FAST = /^([A-Za-z]+):\/\/([A-Za-z0-9.-]+)(?::([0-9]{1,5}))?(\/[!$%&'()*+,\-./0-9:;=@A-Z[\]_a-z|~]*)?(\?[!$%&()*+,\-./0-9:;=?@A-Z[\\\]^_`a-z{|}~]*)?(#[!#$%&'()*+,\-./0-9:;=?@A-Z[\\\]^_a-z{|}~]*)?$/;
const DEFAULT_PORTS = { __proto__: null, http: 80, https: 443, ws: 80, wss: 443 };
const IPV4 = /^(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])(\.(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])){3}$/;
const NUMBERISH = /^(0x|[0-9])/;
const NUMBER = /^(0x[0-9a-f]*|[0-9]+)$/;
const DOT_SEGMENT = /\/(\.|%2e){1,2}(\/|$)/i;
const UPPER = /[A-Z]/;

function fastParse(input) {
  if (input.length > 2048) return undefined;
  const m = FAST.exec(input);
  if (m === null) return undefined;
  let scheme = m[1];
  if (UPPER.test(scheme)) scheme = scheme.toLowerCase();
  const defaultPort = DEFAULT_PORTS[scheme];
  if (defaultPort === undefined) return undefined;
  let host = m[2];
  if (UPPER.test(host)) host = host.toLowerCase();
  // (a host that ends in a number is an IPv4 address: only the canonical form is as it reads)
  let end = host.length;
  if (end > 1 && host.charCodeAt(end - 1) === 46) end--;
  const last = host.slice(host.lastIndexOf('.', end - 1) + 1, end);
  if (last === '' || NUMBERISH.test(last) && NUMBER.test(last)) {
    if (!IPV4.test(host)) return undefined;
  }
  if (host.length > 3 && host.includes('xn--')) return undefined;
  let port = null;
  if (m[3] !== undefined) {
    const n = +m[3];
    if (n > 65535) return undefined;
    if (n !== defaultPort) port = n;
  }
  const path = m[4] ?? '/';
  if ((path.includes('/.') || path.includes('%')) && DOT_SEGMENT.test(path)) return undefined;
  const query = m[5];
  const fragment = m[6];
  const c = urlComponents;
  const protocolEnd = scheme.length + 1;
  const hostStart = protocolEnd + 2;
  const hostEnd = hostStart + host.length;
  let href = `${scheme}://${host}`;
  c[0] = protocolEnd;
  c[1] = hostStart;
  c[2] = hostStart;
  c[3] = hostEnd;
  if (port !== null) {
    href += `:${port}`;
    c[4] = port;
  } else {
    c[4] = omitted;
  }
  c[5] = href.length;
  href += path;
  if (query !== undefined) {
    c[6] = href.length;
    href += query;
  } else {
    c[6] = omitted;
  }
  if (fragment !== undefined) {
    c[7] = href.length;
    href += fragment;
  } else {
    c[7] = omitted;
  }
  c[8] = schemeTypes[scheme];
  return href;
}

function parseBase(base) {
  if (base === undefined) return null;
  return usm.basicURLParse(base);
}

// (a path against a base the fast path takes, as servers resolve a request's target)
function fastParseRelative(input, base) {
  if (input.charCodeAt(0) !== 47 || input.charCodeAt(1) === 47 || input.charCodeAt(1) === 92) return undefined;
  const b = fastParse(base);
  if (b === undefined) return undefined;
  return fastParse(b.slice(0, urlComponents[5]) + input);
}

function parse(input, base, raiseException) {
  const href = base === undefined ? fastParse(`${input}`) : fastParseRelative(`${input}`, `${base}`);
  if (href !== undefined) return href;
  const parsedBase = parseBase(base);
  const url = base !== undefined && parsedBase === null ? null : usm.basicURLParse(input, { baseURL: parsedBase });
  if (url === null) {
    if (raiseException) throw invalid(input, base);
    return undefined;
  }
  return serialize(url);
}

function canParse(input, base) {
  if ((base === undefined ? fastParse(`${input}`) : fastParseRelative(`${input}`, `${base}`)) !== undefined) return true;
  const parsedBase = parseBase(base);
  if (base !== undefined && parsedBase === null) return false;
  return usm.basicURLParse(input, { baseURL: parsedBase }) !== null;
}

// Node.js's path-to-file-URL escaping: everything but letters, digits and /;=:@&$,+!*'()._-
function escapePath(path) {
  let out = '';
  let bytes;
  for (const ch of path) {
    if (/^[A-Za-z0-9/;=:@&$,+!*'()._-]$/.test(ch)) {
      out += ch;
      continue;
    }
    bytes ??= new TextEncoder();
    for (const b of bytes.encode(ch)) out += '%' + (b < 16 ? '0' : '') + b.toString(16).toUpperCase();
  }
  return out;
}

function pathToFileURL(path, windows, hostname) {
  let p = escapePath(windows ? path.replace(/\\/g, '/') : path);
  if (windows && !p.startsWith('/')) p = '/' + p;
  const url = usm.basicURLParse(`file://${hostname ?? ''}${p}`);
  if (url === null) throw invalid(path);
  return serialize(url);
}

const actions = ['protocol', 'host', 'hostname', 'port', 'username', 'password', 'pathname', 'search', 'hash', 'href'];

// a setter of the URL Standard on the URL `href`: the new href, or false when an href doesn't parse
function update(href, action, value) {
  if (actions[action] === 'href') {
    const url = usm.basicURLParse(value);
    return url === null ? false : serialize(url);
  }
  const url = usm.basicURLParse(href);
  if (url === null) return false;
  switch (actions[action]) {
    case 'protocol':
      usm.basicURLParse(`${value}:`, { url, stateOverride: 'scheme start' });
      break;
    case 'username':
      if (!usm.cannotHaveAUsernamePasswordPort(url)) usm.setTheUsername(url, value);
      break;
    case 'password':
      if (!usm.cannotHaveAUsernamePasswordPort(url)) usm.setThePassword(url, value);
      break;
    case 'host':
      if (!usm.hasAnOpaquePath(url)) usm.basicURLParse(value, { url, stateOverride: 'host' });
      break;
    case 'hostname':
      if (!usm.hasAnOpaquePath(url)) usm.basicURLParse(value, { url, stateOverride: 'hostname' });
      break;
    case 'port':
      if (usm.cannotHaveAUsernamePasswordPort(url)) break;
      if (value === '') url.port = null;
      else usm.basicURLParse(value, { url, stateOverride: 'port' });
      break;
    case 'pathname':
      if (usm.hasAnOpaquePath(url)) break;
      url.path = [];
      usm.basicURLParse(value, { url, stateOverride: 'path start' });
      break;
    case 'search': {
      if (value === '') {
        url.query = null;
        stripOpaquePathSpaces(url);
        break;
      }
      const input = value[0] === '?' ? value.slice(1) : value;
      url.query = '';
      usm.basicURLParse(input, { url, stateOverride: 'query' });
      break;
    }
    case 'hash': {
      if (value === '') {
        url.fragment = null;
        stripOpaquePathSpaces(url);
        break;
      }
      const input = value[0] === '#' ? value.slice(1) : value;
      url.fragment = '';
      usm.basicURLParse(input, { url, stateOverride: 'fragment' });
      break;
    }
  }
  return serialize(url);
}

// (the URL Standard's "potentially strip trailing spaces from an opaque path")
function stripOpaquePathSpaces(url) {
  if (!usm.hasAnOpaquePath(url) || url.fragment !== null || url.query !== null) return;
  url.path = url.path.replace(/ +$/, '');
}

function domainToASCII(domain) {
  if (domain === '') return '';
  const r = usm.domainToASCII(domain);
  return typeof r === 'string' ? r : '';
}

function domainToUnicode(domain) {
  if (domain === '') return '';
  const r = usm.lazyTr46().toUnicode(domain, {
    checkHyphens: false,
    checkBidi: true,
    checkJoiners: true,
    useSTD3ASCIIRules: false,
    transitionalProcessing: false,
    ignoreInvalidPunycode: false,
  });
  return r.error ? '' : r.domain;
}

function getOrigin(href) {
  const url = usm.basicURLParse(href);
  return url === null ? 'null' : usm.serializeURLOrigin(url);
}

// url.format(URL, options)
function format(href, fragment, unicode, search, auth) {
  const url = usm.basicURLParse(href);
  if (url === null) return href;
  if (!fragment) url.fragment = null;
  if (!search) url.query = null;
  if (!auth) {
    url.username = '';
    url.password = '';
  }
  let out = usm.serializeURL(url);
  if (unicode && url.host !== null && typeof url.host === 'string') {
    const ascii = usm.serializeHost(url.host);
    const uni = domainToUnicode(ascii);
    if (uni && uni !== ascii) {
      const at = out.indexOf(ascii, url.scheme.length + 3);
      if (at >= 0) out = out.slice(0, at) + uni + out.slice(at + ascii.length);
    }
  }
  return out;
}

module.exports = {
  urlComponents,
  _tovFastParse: fastParse,
  _tovFastParseRelative: fastParseRelative,
  parse,
  canParse,
  pathToFileURL,
  update,
  domainToASCII,
  domainToUnicode,
  getOrigin,
  format,
};
