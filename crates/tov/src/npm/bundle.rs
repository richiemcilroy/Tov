//! Bundles npm packages into one script: every module becomes a function in a table, with its
//! static `require(...)` calls resolved at bundle time. The script defines `__tov_npm`, which
//! loads a requested package on first use.

pub use super::lex::{static_requires, unquote};
use super::resolve::{Format, Resolver, Target};
use crate::hash::FxMap;
use std::fmt::Write;
use std::path::{Path, PathBuf};

pub struct Bundle {
    /// The loader and the module table: evaluated when the program starts. Modules are compiled
    /// on first `require` from `sources` (see `blob`), or come inline (see `script`).
    pub prelude: String,
    /// Per module: its display name (stack traces show it) and its code wrapped as a function
    /// expression, `(function (module, exports, ...) {` + code + `\n})`, starting on the
    /// code's first line so line numbers match the file's.
    pub sources: Vec<(String, String)>,
    /// Per module, its static requires (`require(spec)` → module id), encoded as
    /// "spec\x01id\x02spec\x01id...": read when the module is first loaded.
    pub maps: Vec<String>,
    /// Package specifiers the program imports, in the order given.
    pub entries: Vec<String>,
    /// Number of modules (files, JSON and built-ins) in the bundle.
    pub modules: usize,
    pub warnings: Vec<String>,
    /// It holds a Node-API addon (`.node`): the program links runtime/napi.c.
    pub native: bool,
}

struct Module {
    /// Display name: the path relative to the project, or `node:x`.
    name: String,
    /// The module's code as a function body.
    body: String,
    /// Static require specifier → module index.
    map: Vec<(String, usize)>,
    /// Converted from an ES module (its function takes `esm::ESM_PARAMS`).
    esm: bool,
}

/// Bundles the packages `specs` imports, resolving them from `root` (the program's directory).
pub fn bundle(root: &Path, specs: &[String]) -> Result<Bundle, String> {
    let resolver = Resolver::default();
    let mut modules: Vec<Module> = Vec::new();
    let mut index: FxMap<Target, usize> = FxMap::default();
    let mut queue: Vec<(usize, Target)> = Vec::new();
    let mut warnings = Vec::new();
    let mut entry_ids = Vec::new();
    let mut native = false;
    let mut pack = Pack::load(root, specs);
    // Node's globals (`process`, `Buffer`, ...) as a module that runs before the entries
    let globals_id = super::node_shims::shim("__globals").map(|_| add(&mut modules, &mut index, &mut queue, Target::Builtin("internal/bootstrap/globals".into())));
    for spec in specs {
        let t = resolver.resolve(root, spec)?;
        let id = add(&mut modules, &mut index, &mut queue, t);
        entry_ids.push(id);
    }
    while let Some((id, target)) = queue.pop() {
        let mut esm = false;
        let (name, body, requires, dir) = match &target {
            Target::Builtin(b) => {
                let body = match super::node_shims::shim(b) {
                                        Some(src) => src.to_string(),
                    None => format!("module.exports = __tov_missing({});", js_string(b)),
                };
                // shims may require other shims ("node:events")
                let reqs = super::node_shims::shim_requires(b).iter().map(|r| r.to_string()).collect();
                (format!("node:{b}"), body, reqs, root.to_path_buf())
            }
            Target::File(path) if asset_kind(path).is_some() => {
                // stylesheets are `{}`, other assets their path (as Bun's runtime does)
                let name = display(path, root);
                let body = match asset_kind(path) {
                    Some(Asset::Style) => "module.exports = {};".to_string(),
                    _ => format!("module.exports = {};", js_string(&name)),
                };
                (name, body, Vec::new(), root.to_path_buf())
            }
            Target::File(path) => {
                let name = display(path, root);
                let dir = path.parent().unwrap_or(root).to_path_buf();
                let declared = resolver.format(path);
                // (unchanged since the last build: as transformed then, the file not read)
                let stamp = file_stamp(path);
                let (is_esm, t) = match stamp.and_then(|st| pack.get(path, st, declared)) {
                    Some(hit) => hit,
                    None => {
                        let bytes = std::fs::read(path).map_err(|e| format!("can't read {}: {e}", path.display()))?;
                        let src = match String::from_utf8(bytes) {
                            Ok(src) if !is_binary_ext(path) => src,
                            _ => {
                                // a native addon: loaded from where it is (runtime/napi.c); another binary
                                // fails when required, as in Node.js (packages that try one usually catch
                                // it and fall back)
                                let addon = path.extension().is_some_and(|e| e == "node");
                                native |= addon;
                                let msg = format!("Cannot load {name}: binary modules other than Node-API addons aren't supported by Tov");
                                let fail = format!("var e = new Error({}); e.code = \"ERR_DLOPEN_FAILED\"; throw e;", js_string(&msg));
                                let body = if addon { format!("if (typeof __tov_dlopen !== \"function\") {{ {fail} }} module.exports = __tov_dlopen({}, module.exports);", js_string(&name)) } else { fail };
                                modules[id].name = name;
                                modules[id].body = body;
                                continue;
                            }
                        };
                        let ts = super::resolve::is_typescript(path);
                        let jsx = matches!(path.extension().and_then(|e| e.to_str()), Some("tsx" | "jsx"));
                        let format = match declared {
                            Format::Detect if ts || jsx || super::esm::has_module_syntax(&src) => Format::Esm,
                            Format::Detect => Format::CommonJs,
                            f => f,
                        };
                        let made = match format {
                            Format::Json => (false, Transformed { body: format!("module.exports = {};", src.trim_end().trim_start_matches('\u{feff}')), requires: Vec::new(), patterns: Vec::new() }),
                            Format::CommonJs | Format::Detect if !ts && !jsx => {
                                let t = transformed(&src, "cjs", || {
                                    let requires = static_requires(&src)?;
                                    let patterns = super::lex::require_patterns(&src);
                                    Ok(Transformed { body: minified(src.clone()), requires, patterns })
                                })
                                .map_err(|e| format!("{name}: {e}"))?;
                                (false, t)
                            }
                            _ => {
                                let runtime = jsx.then(|| jsx_runtime(path));
                                let kind = format!("esm ts={ts} jsx={runtime:?}");
                                let t = transformed(&src, &kind, || {
                                    let (body, requires) = super::esm::to_commonjs(&src, ts, runtime.as_deref())?;
                                    Ok(Transformed { body: minified(body), requires, patterns: Vec::new() })
                                })
                                .map_err(|e| format!("{name}: {e}"))?;
                                (true, t)
                            }
                        };
                        if let Some(st) = stamp {
                            pack.put(path, st, declared, made.0, &made.1);
                        }
                        made
                    }
                };
                esm = is_esm;
                let mut reqs = t.requires;
                for (prefix, suffix) in &t.patterns {
                    reqs.extend(expand_pattern(&dir, prefix, suffix));
                }
                (name, t.body, reqs, dir)
            }
        };
        let mut map = Vec::with_capacity(requires.len());
        let in_shim = matches!(target, Target::Builtin(_));
        for spec in requires {
            if map.iter().any(|(s, _): &(String, usize)| *s == spec) {
                continue;
            }
            // Inside a shim (Node's own lib/ code) every require names another shim, `internal/...`
            // included; user code can't reach those.
            let resolved = if in_shim {
                let id = spec.strip_prefix("node:").unwrap_or(&spec).to_string();
                if super::node_shims::shim(&id).is_some() || super::resolve::BUILTINS.contains(&id.as_str()) { Ok(Target::Builtin(id)) } else { Err(format!("no shim for \"{spec}\"")) }
            } else {
                resolver.set_esm(esm);
                let r = resolver.resolve(&dir, &spec);
                resolver.set_esm(false);
                r
            };
            match resolved {
                Ok(t) => {
                    let to = add(&mut modules, &mut index, &mut queue, t);
                    map.push((spec, to));
                }
                // Optional dependencies are often required inside try/catch: leave the
                // require to fail at run time, as in Node.
                Err(e) => warnings.push(format!("{name}: {e}")),
            }
        }
        // A native binding's loader (it names a `.node` file, usually by a computed path the
        // bundler can't follow): the program links Node-API for what it loads at run time.
        if !in_shim && (body.contains(".node\"") || body.contains(".node'") || body.contains(".node`")) {
            native = true;
        }
        // Globals whose shims load by a computed name (so programs that don't use them don't
        // carry them): bundled when a package's code mentions one, and mapped there (a require
        // the bundler didn't see finds a module through another's map).
        if !in_shim {
            // (the code's tokens, once a name shows up in its text: comments, strings and
            // `obj.fetch` don't count)
            let mut toks: Option<Vec<super::lex::Tok>> = None;
            for (global, shim) in LAZY_GLOBALS {
                if !body.contains(global) || map.iter().any(|(s, _)| s == shim) || super::node_shims::shim(shim).is_none() {
                    continue;
                }
                let toks = toks.get_or_insert_with(|| super::lex::tokenize(&body).unwrap_or_default());
                // `subtle` is reached as `crypto.subtle`; the others are globals of their own
                let member = *global == "subtle";
                let named = toks.iter().enumerate().any(|(i, t)| {
                    t.kind == super::lex::Kind::Ident && t.text(&body) == *global && {
                        let after_dot = i > 0 && matches!(toks[i - 1].text(&body), "." | "?.");
                        after_dot == member
                    }
                });
                if named {
                    let to = add(&mut modules, &mut index, &mut queue, Target::Builtin(shim.to_string()));
                    map.push((shim.to_string(), to));
                }
            }
        }
        modules[id].name = name;
        modules[id].body = body;
        modules[id].map = map;
        modules[id].esm = esm;
    }
    pack.save();
    let mut js = String::with_capacity(4096 + modules.len() * 64);
    if globals_id.is_none() {
        js.push_str(super::node_shims::globals());
    }
    js.push_str(RUNTIME_HEAD);
    let mut sources = Vec::with_capacity(modules.len());
    for m in &mut modules {
        // `exports` is `this` at a CommonJS module's top level, as in Node.
        let params = if m.esm { super::esm::ESM_PARAMS } else { "module, exports, require, __filename, __dirname" };
        let mut body = std::mem::take(&mut m.body);
        if body.starts_with("#!") {
            body.replace_range(0..2, "//");
        }
        sources.push((std::mem::take(&mut m.name), format!("(function ({params}) {{{body}\n}})")));
    }
    // each module's static requires, read when it's first loaded: "spec\x01id\x02spec\x01id..."
    let maps: Vec<String> = modules.iter().map(|m| m.map.iter().map(|(spec, to)| format!("{spec}\u{1}{to}")).collect::<Vec<_>>().join("\u{2}")).collect();
    js.push_str("globalThis.__tov_npm = {");
    for (spec, id) in specs.iter().zip(&entry_ids) {
        let _ = write!(js, "{}:{},", js_string(spec), id);
    }
    js.push_str("};\n");
    if let Some(id) = globals_id {
        let _ = writeln!(js, "__tov_load({id});");
    }
    js.push_str(RUNTIME_TAIL);
    Ok(Bundle { prelude: js, sources, maps, entries: specs.to_vec(), modules: modules.len(), warnings, native })
}

impl Bundle {
    /// The bundle as one script, every module inline (for engines without Tov's runtime).
    pub fn script(&self) -> String {
        let mut js = String::with_capacity(self.prelude.len() + self.sources.iter().map(|s| s.1.len() + 2).sum::<usize>() + 32);
        js.push_str("var __tov_defs = [\n");
        for (_, src) in &self.sources {
            js.push_str(src);
            js.push_str(",\n");
        }
        // (what runtime/js.c serves from the blob)
        js.push_str("];\nvar __tov_inline = { names: [");
        for (name, _) in &self.sources {
            js.push_str(&js_string(name));
            js.push(',');
        }
        js.push_str("], maps: [");
        for m in &self.maps {
            js.push_str(&js_string(m));
            js.push(',');
        }
        js.push_str("] };\nvar __tov_name = function (id) { return __tov_inline.names[id]; };\nvar __tov_map = function (id) { return __tov_inline.maps[id]; };\nvar __tov_count = __tov_inline.names.length;\n");
        js.push_str(&self.prelude);
        js
    }

    /// The bundle as runtime/js.c reads it: a little-endian u32 index — the module count, the
    /// prelude's offset, then per module the offsets of its name, its source, the source's
    /// length and its requires (see `maps`) — then the strings, each NUL-terminated. Offsets
    /// count from the blob's start.
    pub fn blob(&self) -> Vec<u8> {
        let n = self.sources.len();
        let header = 4 * (2 + 4 * n);
        let mut data: Vec<u8> = Vec::with_capacity(self.prelude.len() + self.sources.iter().map(|s| s.0.len() + s.1.len() + 2).sum::<usize>() + self.maps.iter().map(|m| m.len() + 1).sum::<usize>() + 1);
        let mut index: Vec<u32> = Vec::with_capacity(2 + 4 * n);
        index.push(n as u32);
        index.push(header as u32); // the prelude
        data.extend_from_slice(self.prelude.as_bytes());
        data.push(0);
        for ((name, src), map) in self.sources.iter().zip(&self.maps) {
            index.push((header + data.len()) as u32);
            data.extend_from_slice(name.as_bytes());
            data.push(0);
            index.push((header + data.len()) as u32);
            data.extend_from_slice(src.as_bytes());
            data.push(0);
            index.push(src.len() as u32);
            index.push((header + data.len()) as u32);
            data.extend_from_slice(map.as_bytes());
            data.push(0);
        }
        let mut out = Vec::with_capacity(header + data.len());
        for v in index {
            out.extend_from_slice(&v.to_le_bytes());
        }
        out.extend_from_slice(&data);
        out
    }
}

fn add(modules: &mut Vec<Module>, index: &mut FxMap<Target, usize>, queue: &mut Vec<(usize, Target)>, t: Target) -> usize {
    if let Some(&id) = index.get(&t) {
        return id;
    }
    let id = modules.len();
    modules.push(Module { name: String::new(), body: String::new(), map: Vec::new(), esm: false });
    index.insert(t.clone(), id);
    queue.push((id, t));
    id
}

/// The specifiers a `require(prefix + x + suffix)` can name: the files in prefix's directory
/// that match (bundled in case they're required). At most 2000.
fn expand_pattern(from_dir: &Path, prefix: &str, suffix: &str) -> Vec<String> {
    let (dir_part, name_prefix) = match prefix.rfind('/') {
        Some(i) => (&prefix[..=i], &prefix[i + 1..]),
        // `require(`lightningcss-${platform}`)`: the installed packages named so
        None if !prefix.is_empty() && !prefix.starts_with('.') && suffix.is_empty() => {
            let mut d = Some(from_dir);
            let mut out = Vec::new();
            while let Some(x) = d {
                if let Ok(entries) = std::fs::read_dir(x.join("node_modules")) {
                    for e in entries.flatten() {
                        let n = e.file_name().to_string_lossy().into_owned();
                        if n.starts_with(prefix) && e.path().join("package.json").is_file() && !out.contains(&n) {
                            out.push(n);
                        }
                    }
                }
                d = x.parent();
            }
            out.sort();
            out.truncate(200);
            return out;
        }
        None => return Vec::new(),
    };
    // the directory: relative to the module, or inside a package in node_modules
    let base = if dir_part.starts_with("./") || dir_part.starts_with("../") {
        from_dir.join(dir_part)
    } else if dir_part.starts_with('/') {
        return Vec::new();
    } else if dir_part.starts_with('@') && dir_part.matches('/').count() == 1 && suffix.is_empty() {
        // `require(`@babel/plugin-${name}`)`: the scope's packages that match
        let mut d = Some(from_dir);
        let mut out = Vec::new();
        while let Some(x) = d {
            if let Ok(entries) = std::fs::read_dir(x.join("node_modules").join(dir_part.trim_end_matches('/'))) {
                for e in entries.flatten() {
                    let n = e.file_name().to_string_lossy().into_owned();
                    if n.starts_with(name_prefix) && e.path().join("package.json").is_file() {
                        out.push(format!("{dir_part}{n}"));
                    }
                }
                break;
            }
            d = x.parent();
        }
        out.sort();
        out.truncate(2000);
        return out;
    } else {
        let mut segs = dir_part.trim_end_matches('/').splitn(if dir_part.starts_with('@') { 3 } else { 2 }, '/');
        let pkg = if dir_part.starts_with('@') { format!("{}/{}", segs.next().unwrap_or(""), segs.next().unwrap_or("")) } else { segs.next().unwrap_or("").to_string() };
        let sub = segs.next().unwrap_or("");
        let mut d = Some(from_dir);
        let mut found = None;
        while let Some(x) = d {
            let cand = x.join("node_modules").join(&pkg);
            if cand.is_dir() {
                found = Some(cand.join(sub));
                break;
            }
            d = x.parent();
        }
        match found {
            Some(f) => f,
            None => return Vec::new(),
        }
    };
    let Ok(entries) = std::fs::read_dir(&base) else { return Vec::new() };
    let mut out = Vec::new();
    for e in entries.flatten().take(5000) {
        let file = e.file_name().to_string_lossy().into_owned();
        let code = [".js", ".cjs", ".mjs", ".json", ".ts"].iter().any(|x| file.ends_with(x));
        if !code || !file.starts_with(name_prefix) || !e.path().is_file() {
            continue;
        }
        if suffix.is_empty() {
            // `require(dir + name)`: usually without the extension
            let stem = file.rsplit_once('.').map(|(s, _)| s).unwrap_or(&file);
            out.push(format!("{dir_part}{stem}"));
            out.push(format!("{dir_part}{file}"));
        } else if file.ends_with(suffix) && file.len() >= name_prefix.len() + suffix.len() {
            out.push(format!("{dir_part}{file}"));
        } else if let Some(stem) = file.rsplit_once('.').map(|(s, _)| s)
            && stem.ends_with(suffix)
        {
            out.push(format!("{dir_part}{stem}"));
        }
        if out.len() >= 2000 {
            break;
        }
    }
    out.sort();
    out.dedup();
    out
}

/// (global name, the shim that defines it) for globals globals.js loads by a computed name.
const LAZY_GLOBALS: &[(&str, &str)] = &[("CompressionStream", "internal/webstreams/compression"), ("DecompressionStream", "internal/webstreams/compression"), ("subtle", "crypto"), ("fetch", "internal/tov/fetch"), ("Request", "internal/tov/fetch"), ("Response", "internal/tov/fetch"), ("Headers", "internal/tov/fetch"), ("FormData", "internal/tov/fetch"), ("Bun", "bun")];

/// The module JSX compiles to calls of: the nearest tsconfig.json's `jsxImportSource` (Solid,
/// Preact, ...) + `/jsx-runtime`, else React's.
fn jsx_runtime(file: &Path) -> String {
    let mut dir = file.parent();
    while let Some(d) = dir {
        if let Ok(text) = std::fs::read_to_string(d.join("tsconfig.json")) {
            if let Some(i) = text.find("\"jsxImportSource\"") {
                let rest = &text[i + 17..];
                if let Some(q) = rest.find('"') {
                    let v = &rest[q + 1..];
                    if let Some(e) = v.find('"') {
                        return format!("{}/jsx-runtime", &v[..e]);
                    }
                }
            }
            break;
        }
        if d.join("package.json").is_file() && d.file_name().is_some_and(|n| n != "src") && d.join("node_modules").is_dir() {
            break;
        }
        dir = d.parent();
    }
    "react/jsx-runtime".into()
}

enum Asset {
    Style,
    File,
}

/// Files a JavaScript `require` can name that aren't code.
fn asset_kind(path: &Path) -> Option<Asset> {
    match path.extension().and_then(|e| e.to_str())? {
        "css" | "scss" | "sass" | "less" | "styl" => Some(Asset::Style),
        "png" | "jpg" | "jpeg" | "gif" | "webp" | "avif" | "ico" | "bmp" | "svg" | "woff" | "woff2" | "ttf" | "otf" | "eot" | "mp3" | "mp4" | "webm" | "wav" | "ogg" | "pdf" => Some(Asset::File),
        _ => None,
    }
}

/// A module's code minified (less for the engine to parse at start), when that's sure to mean
/// the same: the minified text must read back as exactly the same tokens (a regular expression
/// the lexer took for division would not). Line breaks stay, so line numbers do too.
fn minified(src: String) -> String {
    if std::env::var_os("TOV_NO_MINIFY").is_some() {
        return src;
    }
    let Ok(min) = super::lex::minify(&src) else { return src };
    if min.len() + min.len() / 10 >= src.len() {
        return src;
    }
    let (Ok(a), Ok(b)) = (super::lex::tokenize(&src), super::lex::tokenize(&min)) else { return src };
    let same = a.len() == b.len() && a.iter().zip(&b).all(|(x, y)| x.kind == y.kind && x.text(&src) == y.text(&min));
    if same { min } else { src }
}

/// Native addons and other binaries (anything not UTF-8 is one too).
fn is_binary_ext(path: &Path) -> bool {
    matches!(path.extension().and_then(|e| e.to_str()), Some("node" | "wasm" | "dylib" | "so" | "dll"))
}

/// A module's name: its real path, as `__filename` in Node.js and Bun (packages find their own
/// files from `__dirname`: binaries, data, templates).
fn display(path: &Path, _root: &Path) -> String {
    path.to_string_lossy().into_owned()
}

/// A JavaScript string literal for `s`.
pub fn js_string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\u{2028}' => out.push_str("\\u2028"),
            '\u{2029}' => out.push_str("\\u2029"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

pub fn root_of(path: &Path) -> PathBuf {
    path.parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("."))
}

/// The loader: modules are evaluated on first `require`, like Node (cycles see the partial
/// `module.exports`).
const RUNTIME_HEAD: &str = r#"// Helpers the modules call: globals, since each module is compiled on its own.
function __tov_missing(name) {
  var fail = function () { throw new Error("The Node module \"" + name + "\" is not supported by Tov yet"); };
  return new Proxy({}, { get: function (t, k) { if (k === "__esModule" || typeof k === "symbol" || k === "then") return undefined; return fail; } });
}
var __tov_esm_set = new WeakSet();
var __tov_ns_cache = new WeakMap();
function __tov_esm(exports, getters, withDefault) {
  __tov_esm_set.add(exports);
  Object.defineProperty(exports, "__esModule", { value: true, enumerable: !!withDefault });
  for (var k in getters) Object.defineProperty(exports, k, { get: getters[k], enumerable: true });
}
function __tov_reexport(to, m, k) { Object.defineProperty(to, k, { get: function () { return m[k]; }, enumerable: true }); }
// An ES module's view of a module: its exports if it was an ES module, or a CommonJS module
// compiled from one (`__esModule`, as Bun and bundlers read it); otherwise `default` is
// `module.exports` and its own keys are named exports.
function __tov_ns(m) {
  if (m === null || (typeof m !== "object" && typeof m !== "function")) return { default: m };
  if (__tov_esm_set.has(m) || m.__esModule) return m;
  var ns = __tov_ns_cache.get(m);
  if (ns) return ns;
  ns = {};
  var keys = Object.keys(m);
  for (var i = 0; i < keys.length; i++) if (keys[i] !== "default") __tov_reexport(ns, m, keys[i]);
  Object.defineProperty(ns, "default", { value: m, enumerable: true });
  __tov_ns_cache.set(m, ns);
  return ns;
}
function __tov_star(to, m) {
  var keys = Object.keys(m);
  for (var i = 0; i < keys.length; i++) {
    var k = keys[i];
    if (k !== "default" && k !== "__esModule" && !Object.prototype.hasOwnProperty.call(to, k)) __tov_reexport(to, m, k);
  }
}
(function () {
// (inline modules come first, from `Bundle::script`; otherwise the runtime compiles them)
var __tov_defs = globalThis.__tov_defs || [];
var __tov_compile = globalThis.__tov_compile;
// (from runtime/js.c, or inline in a script)
var __tov_name = globalThis.__tov_name;
var __tov_map = globalThis.__tov_map;
var __tov_count = globalThis.__tov_count;
var __tov_cache = [];
function __tov_dirname(p) { var i = p.lastIndexOf("/"); return i <= 0 ? "/" : p.slice(0, i); }
// a module's requires, as an object (from its encoded string)
function __tov_map_of(id) {
  var s = __tov_map(id), map = {};
  if (s) {
    var parts = s.split("\x02");
    for (var i = 0; i < parts.length; i++) {
      var p = parts[i].split("\x01");
      map[p[0]] = +p[1];
    }
  }
  return map;
}
// A require the bundler couldn't see (`require(path.join(__dirname, x))`, `require(name)`): a
// bundled module at that path, or the module other code reaches with the same specifier.
var __tov_by_name, __tov_by_spec;
// (deprecated in Node.js, still read)
var __tov_extensions = { ".js": function () {}, ".json": function () {}, ".node": function () {} };
function __tov_join(dir, rel) {
  var parts = (rel[0] === "/" ? rel : dir + "/" + rel).split("/"), out = [];
  for (var i = 0; i < parts.length; i++) {
    var p = parts[i];
    if (p === "" || p === ".") continue;
    if (p === "..") out.pop(); else out.push(p);
  }
  return "/" + out.join("/");
}
function __tov_find(spec, from) {
  if (spec[0] === "/" || spec[0] === ".") {
    if (!__tov_by_name) {
      __tov_by_name = new Map();
      for (var i = 0; i < __tov_count; i++) __tov_by_name.set(__tov_name(i), i);
    }
    var p = __tov_join(__tov_dirname(from), spec);
    var exts = ["", ".js", ".json", ".cjs", ".mjs", ".ts", ".tsx", "/index.js", "/index.json", "/index.ts"];
    for (var k = 0; k < exts.length; k++) {
      var hit = __tov_by_name.get(p + exts[k]);
      if (hit !== undefined) return hit;
    }
    return undefined;
  }
  if (!__tov_by_spec) {
    __tov_by_spec = new Map();
    for (var j = 0; j < __tov_count; j++) {
      var m = __tov_map_of(j);
      for (var s in m) if (s[0] !== "." && s[0] !== "/" && !__tov_by_spec.has(s)) __tov_by_spec.set(s, m[s]);
    }
  }
  var to = __tov_by_spec.get(spec);
  if (to === undefined) to = __tov_by_spec.get(spec.slice(0, 5) === "node:" ? spec.slice(5) : "node:" + spec);
  return to;
}
// A file the bundle doesn't hold, read from disk where the program runs (JSON, or CommonJS:
// `require(path.join(dir, name))`); its own requires go through the same fallbacks.
var __tov_disk_cache = new Map();
// a package installed where the program runs, in the node_modules nearest `from`: the path of
// its entry (main, or exports' require/default), or of the file named under it ("pkg/lib.so")
function __tov_disk_package_path(spec, from) {
  var parts = spec.split("/"), scoped = spec[0] === "@";
  var pkg = parts.slice(0, scoped ? 2 : 1).join("/"), sub = parts.slice(scoped ? 2 : 1).join("/");
  for (var d = __tov_dirname(from); ; d = __tov_dirname(d)) {
    var base = d + "/node_modules/" + pkg, pj = __tov_read_file(base + "/package.json");
    if (pj !== undefined) {
      var target = sub;
      if (!target) {
        var j = JSON.parse(pj), e = j.exports, m = typeof e === "string" ? e : e && (e["."] !== undefined ? e["."] : e);
        while (m && typeof m === "object") m = Array.isArray(m) ? m[0] : (m.require || m.node || m.default || m.import);
        target = typeof m === "string" && m[0] === "." ? m : (j.main || "index.js");
      }
      return __tov_join(base, target);
    }
    if (d === "/" || d === "") return undefined;
  }
}
// a package installed where the program runs: its entry, loaded (from the bundle if it's there)
function __tov_disk_package(spec, from) {
  var path = __tov_disk_package_path(spec, from);
  if (path === undefined) return undefined;
  if (!__tov_by_name) { __tov_by_name = new Map(); for (var i = 0; i < __tov_count; i++) __tov_by_name.set(__tov_name(i), i); }
  var exts = ["", ".js", ".json", "/index.js"];
  for (var k = 0; k < exts.length; k++) {
    var hit = __tov_by_name.get(path + exts[k]);
    if (hit !== undefined) return __tov_load(hit);
  }
  return __tov_disk(path, "/");
}
// require.resolve of a file the bundle doesn't hold (a platform package's native library or
// .wasm, `require.resolve("pkg-linux-x64/lib.so")`): its path where the program runs, found as
// __tov_disk finds what it loads; undefined if there's none
function __tov_disk_resolve(spec, from) {
  if (typeof __tov_read_file !== "function" || spec.slice(0, 5) === "node:") return undefined;
  var p = spec[0] === "/" || spec[0] === "." ? __tov_join(__tov_dirname(from), spec) : __tov_disk_package_path(spec, from);
  if (p === undefined) return undefined;
  var exts = ["", ".js", ".json", ".node", "/index.js", "/index.json"];
  for (var k = 0; k < exts.length; k++) if (__tov_read_file(p + exts[k], true)) return p + exts[k];
  return undefined;
}
function __tov_disk(spec, from) {
  if (typeof __tov_read_file !== "function") return undefined;
  if (spec[0] !== "/" && spec[0] !== ".") return spec.slice(0, 5) === "node:" ? undefined : __tov_disk_package(spec, from);
  var p = __tov_join(__tov_dirname(from), spec);
  if (/\.node$/.test(p) && typeof __tov_dlopen === "function" && __tov_read_file(p, true)) {
    var cachedNative = __tov_disk_cache.get(p);
    if (cachedNative) return cachedNative.exports;
    var native = { exports: {} };
    __tov_disk_cache.set(p, native);
    native.exports = __tov_dlopen(p, native.exports);
    return native.exports;
  }
  var exts = ["", ".js", ".json", ".cjs", "/index.js", "/index.json"];
  for (var k = 0; k < exts.length; k++) {
    var file = p + exts[k];
    var cached = __tov_disk_cache.get(file);
    if (cached) return cached.exports;
    if (/\.(mjs|ts|tsx|mts|node|wasm)$/.test(file)) continue;
    var text = __tov_read_file(file);
    if (text === undefined) continue;
    var module = { exports: {}, id: file, filename: file, loaded: false, children: [], paths: [] };
    __tov_disk_cache.set(file, module);
    if (/\.json$/.test(file)) {
      module.exports = JSON.parse(text);
    } else {
      var req = function (s) {
        var to = __tov_find(s, file);
        if (to !== undefined) return __tov_load(to);
        var d = __tov_disk(s, file);
        if (d !== undefined) return d;
        var e = new Error("Cannot find module '" + s + "' (from " + file + ")");
        e.code = "MODULE_NOT_FOUND";
        throw e;
      };
      req.resolve = function (s) { return __tov_join(__tov_dirname(file), s); };
      req.cache = {};
      req.extensions = __tov_extensions;
      __tov_compile_source(text.charCodeAt(0) === 35 ? "//" + text.slice(2) : text, file).call(module.exports, module, module.exports, req, file, __tov_dirname(file));
    }
    module.loaded = true;
    return module.exports;
  }
  return undefined;
}
function __tov_load(id) {
  var cached = __tov_cache[id];
  if (cached) return cached.exports;
  var name = __tov_name(id);
  var module = { exports: {}, id: name, filename: name, loaded: false, children: [], paths: [] };
  __tov_cache[id] = module;
  var map = __tov_map_of(id);
  var require = function (spec) {
    var to = map[spec];
    if (to === undefined && typeof spec === "string") to = __tov_find(spec, name);
    if (to === undefined && typeof spec === "string") {
      var disk = __tov_disk(spec, name);
      if (disk !== undefined) return disk;
    }
    if (to === undefined) {
      var e = new Error("Cannot find module '" + spec + "' (from " + name + ")");
      e.code = "MODULE_NOT_FOUND";
      throw e;
    }
    return __tov_load(to);
  };
  require.resolve = function (spec) { var to = map[spec]; if (to === undefined && typeof spec === "string") to = __tov_find(spec, name); if (to === undefined && typeof spec === "string") { var disk = __tov_disk_resolve(spec, name); if (disk !== undefined) return disk; } if (to === undefined) { var e = new Error("Cannot find module '" + spec + "'"); e.code = "MODULE_NOT_FOUND"; throw e; } return __tov_name(to); };
  require.cache = {};
  require.main = undefined;
  require.extensions = __tov_extensions;
  var def = __tov_defs[id] || __tov_compile(id);
  __tov_defs[id] = undefined;
  // (a module that throws as it loads is loaded again by the next require, as in Node.js)
  var ok = false;
  try {
    def.call(module.exports, module, module.exports, require, name, __tov_dirname(name));
    ok = true;
  } finally {
    if (!ok) __tov_cache[id] = undefined;
  }
  module.loaded = true;
  return module.exports;
}
"#;

const RUNTIME_TAIL: &str = r#"var entries = globalThis.__tov_npm;
// (for the `module` built-in's createRequire: the bundle's modules by name)
// (names and maps whole only when asked for: createRequire)
var __tov_all_names, __tov_all_maps;
Object.defineProperty(globalThis, "__tov_modules", { value: {
  get names() { if (!__tov_all_names) { __tov_all_names = []; for (var i = 0; i < __tov_count; i++) __tov_all_names.push(__tov_name(i)); } return __tov_all_names; },
  get maps() { if (!__tov_all_maps) { __tov_all_maps = []; for (var i = 0; i < __tov_count; i++) __tov_all_maps.push(__tov_map_of(i)); } return __tov_all_maps; },
  load: __tov_load, loaded: function (id) { return __tov_cache[id]; },
  // require(spec) from file `from` by every means the bundle has: a bundled module, or one on
  // disk where the program runs (packages, JSON, CommonJS, native addons); undefined if none
  requireFrom: function (spec, from) {
    var to = __tov_find(spec, from);
    if (to !== undefined) return { exports: __tov_load(to) };
    var d = __tov_disk(spec, from);
    return d === undefined ? undefined : { exports: d };
  },
  // require.resolve of a file on disk where the program runs (not bundled); undefined if none
  resolveFrom: function (spec, from) { return __tov_disk_resolve(spec, from); } } });
globalThis.__tov_npm = function (spec) { return __tov_load(entries[spec]); };
})();
"#;

/// What transforming a module's text gave: its code as a function body (converted to CommonJS,
/// TypeScript and JSX stripped, minified), its static requires, and its required patterns
/// (`require("./locale/" + x + ".js")`, expanded against the directory as it is).
struct Transformed {
    body: String,
    requires: Vec<String>,
    patterns: Vec<(String, String)>,
}

/// `compute`d, or what it gave for the same text before: cached on disk by the text, `kind` (how
/// it's transformed) and this compiler's build, so a rebuild transforms only the files that
/// changed (Cap's media server: 654 modules, 5 MB, ~90 ms to transform, ~5 ms to read back).
fn transformed(src: &str, kind: &str, compute: impl FnOnce() -> Result<Transformed, String>) -> Result<Transformed, String> {
    let Some((dir, build)) = cache_build() else { return compute() };
    let key = fast_hash(src.as_bytes(), fast_hash(kind.as_bytes(), *build));
    let path = dir.join(format!("{key:016x}"));
    if let Ok(bytes) = std::fs::read(&path)
        && let Some(t) = Transformed::decode(&bytes)
    {
        return Ok(t);
    }
    let t = compute()?;
    let tmp = path.with_extension(format!("tmp{}", std::process::id()));
    if std::fs::write(&tmp, t.encode()).is_ok() {
        let _ = std::fs::rename(&tmp, &path);
    }
    Ok(t)
}

impl Transformed {
    /// "tov-t1", the requires and patterns (each a u32 length and bytes), then the body.
    fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(self.body.len() + 64);
        out.extend_from_slice(b"tov-t1");
        let put = |out: &mut Vec<u8>, s: &str| {
            out.extend_from_slice(&(s.len() as u32).to_le_bytes());
            out.extend_from_slice(s.as_bytes());
        };
        out.extend_from_slice(&(self.requires.len() as u32).to_le_bytes());
        for r in &self.requires {
            put(&mut out, r);
        }
        out.extend_from_slice(&(self.patterns.len() as u32).to_le_bytes());
        for (a, b) in &self.patterns {
            put(&mut out, a);
            put(&mut out, b);
        }
        out.extend_from_slice(self.body.as_bytes());
        out
    }

    fn decode(bytes: &[u8]) -> Option<Transformed> {
        let mut at = bytes.strip_prefix(b"tov-t1")?;
        let u32_ = |at: &mut &[u8]| -> Option<usize> {
            let (n, rest) = at.split_first_chunk::<4>()?;
            *at = rest;
            Some(u32::from_le_bytes(*n) as usize)
        };
        let string = |at: &mut &[u8]| -> Option<String> {
            let (n, rest) = at.split_first_chunk::<4>()?;
            let n = u32::from_le_bytes(*n) as usize;
            let s = std::str::from_utf8(rest.get(..n)?).ok()?.to_string();
            *at = &rest[n..];
            Some(s)
        };
        let nreq = u32_(&mut at)?;
        let mut requires = Vec::with_capacity(nreq);
        for _ in 0..nreq {
            requires.push(string(&mut at)?);
        }
        let npat = u32_(&mut at)?;
        let mut patterns = Vec::with_capacity(npat);
        for _ in 0..npat {
            let a = string(&mut at)?;
            patterns.push((a, string(&mut at)?));
        }
        let body = std::str::from_utf8(at).ok()?.to_string();
        Some(Transformed { body, requires, patterns })
    }
}

/// A 64-bit hash of `bytes` (eight at a time), seeded. For cache keys, not security.
fn fast_hash(bytes: &[u8], seed: u64) -> u64 {
    let mut h = seed ^ 0x9e37_79b9_7f4a_7c15 ^ (bytes.len() as u64).wrapping_mul(0xff51_afd7_ed55_8ccd);
    let mut chunks = bytes.chunks_exact(8);
    for c in &mut chunks {
        let w = u64::from_le_bytes(c.try_into().unwrap());
        h = (h ^ w).wrapping_mul(0x9e37_79b9_7f4a_7c15).rotate_left(29);
    }
    for &b in chunks.remainder() {
        h = (h ^ b as u64).wrapping_mul(0x0000_0100_0000_01b3);
    }
    h ^= h >> 33;
    h = h.wrapping_mul(0xc4ce_b9fe_1a85_ec53);
    h ^ (h >> 29)
}

/// A file's size and modification time (nanoseconds): what says it's unchanged.
fn file_stamp(path: &Path) -> Option<(u64, u64)> {
    let meta = std::fs::metadata(path).ok()?;
    let t = meta.modified().ok()?.duration_since(std::time::UNIX_EPOCH).ok()?;
    Some((meta.len(), t.as_nanos() as u64))
}

/// The project's modules as the last build transformed them, by path, size and modification
/// time: a rebuild reads (and transforms) only the files that changed; the others come from one
/// file (Cap's media server: 654 files, 5 MB, read and hashed in ~25 ms without it).
struct Pack {
    path: Option<PathBuf>,
    entries: FxMap<PathBuf, PackEntry>,
    used: Vec<PathBuf>,
    dirty: bool,
}

struct PackEntry {
    stamp: (u64, u64),
    format: u8,
    esm: bool,
    t: Transformed,
}

fn format_code(f: Format) -> u8 {
    match f {
        Format::CommonJs => 0,
        Format::Esm => 1,
        Format::Detect => 2,
        Format::Json => 3,
    }
}

impl Pack {
    fn load(root: &Path, specs: &[String]) -> Pack {
        let mut pack = Pack { path: None, entries: FxMap::default(), used: Vec::new(), dirty: false };
        let Some((dir, build)) = cache_build() else { return pack };
        let mut key = fast_hash(root.as_os_str().as_encoded_bytes(), *build);
        for s in specs {
            key = fast_hash(s.as_bytes(), key);
        }
        let path = dir.join(format!("pack-{key:016x}"));
        if let Ok(bytes) = std::fs::read(&path) {
            pack.decode(&bytes);
        }
        pack.path = Some(path);
        pack
    }

    fn get(&mut self, path: &Path, stamp: (u64, u64), format: Format) -> Option<(bool, Transformed)> {
        let e = self.entries.get(path)?;
        if e.stamp != stamp || e.format != format_code(format) {
            return None;
        }
        self.used.push(path.to_path_buf());
        Some((e.esm, Transformed { body: e.t.body.clone(), requires: e.t.requires.clone(), patterns: e.t.patterns.clone() }))
    }

    fn put(&mut self, path: &Path, stamp: (u64, u64), format: Format, esm: bool, t: &Transformed) {
        let t = Transformed { body: t.body.clone(), requires: t.requires.clone(), patterns: t.patterns.clone() };
        self.entries.insert(path.to_path_buf(), PackEntry { stamp, format: format_code(format), esm, t });
        self.used.push(path.to_path_buf());
        self.dirty = true;
    }

    /// Written when something changed, with only the modules this build used.
    fn save(&mut self) {
        let Some(path) = &self.path else { return };
        if !self.dirty && self.used.len() == self.entries.len() {
            return;
        }
        let mut out = Vec::new();
        out.extend_from_slice(b"tov-p1");
        let mut seen = crate::hash::FxSet::default();
        for p in &self.used {
            if !seen.insert(p.clone()) {
                continue;
            }
            let Some(e) = self.entries.get(p) else { continue };
            let name = p.as_os_str().as_encoded_bytes();
            let t = e.t.encode();
            out.extend_from_slice(&(name.len() as u32).to_le_bytes());
            out.extend_from_slice(name);
            out.extend_from_slice(&e.stamp.0.to_le_bytes());
            out.extend_from_slice(&e.stamp.1.to_le_bytes());
            out.push(e.format);
            out.push(e.esm as u8);
            out.extend_from_slice(&(t.len() as u32).to_le_bytes());
            out.extend_from_slice(&t);
        }
        let tmp = path.with_extension(format!("tmp{}", std::process::id()));
        if std::fs::write(&tmp, &out).is_ok() {
            let _ = std::fs::rename(&tmp, path);
        }
    }

    fn decode(&mut self, bytes: &[u8]) {
        let Some(mut at) = bytes.strip_prefix(b"tov-p1") else { return };
        fn take<'a>(at: &mut &'a [u8], n: usize) -> Option<&'a [u8]> {
            let (a, b) = (at.get(..n)?, at.get(n..)?);
            *at = b;
            Some(a)
        }
        let u32_ = |at: &mut &[u8]| take(at, 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()) as usize);
        let u64_ = |at: &mut &[u8]| take(at, 8).map(|b| u64::from_le_bytes(b.try_into().unwrap()));
        while !at.is_empty() {
            let Some(entry) = (|| {
                let n = u32_(&mut at)?;
                let name = PathBuf::from(std::str::from_utf8(take(&mut at, n)?).ok()?);
                let stamp = (u64_(&mut at)?, u64_(&mut at)?);
                let flags = take(&mut at, 2)?;
                let (format, esm) = (flags[0], flags[1] != 0);
                let n = u32_(&mut at)?;
                let t = Transformed::decode(take(&mut at, n)?)?;
                Some((name, PackEntry { stamp, format, esm, t }))
            })() else {
                self.entries.clear();
                return;
            };
            self.entries.insert(entry.0, entry.1);
        }
    }
}

/// The transform caches' directory, and this compiler's build (the transforms are its code).
fn cache_build() -> &'static Option<(PathBuf, u64)> {
    static DIR: std::sync::OnceLock<Option<(PathBuf, u64)>> = std::sync::OnceLock::new();
    DIR.get_or_init(|| {
        let exe = std::env::current_exe().ok()?;
        let meta = std::fs::metadata(&exe).ok()?;
        let built = meta.modified().ok()?.duration_since(std::time::UNIX_EPOCH).ok()?.as_nanos() as u64;
        let dir = crate::build::cache_dir().join("npm");
        std::fs::create_dir_all(&dir).ok()?;
        Some((dir, fast_hash(env!("CARGO_PKG_VERSION").as_bytes(), built ^ meta.len())))
    })
}
