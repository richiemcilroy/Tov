//! ES modules → the bundle's module functions. Imports become requires in a prologue (ES
//! imports are evaluated before the module body); every reference to an imported binding reads
//! through the imported module's exports object, so bindings stay live, including across
//! import cycles. Exports are getters defined before anything runs.
//!
//! The module function's parameters are named `__tov_m`, `__tov_x`, `__tov_r`, `__tov_f` and
//! `__tov_d` (see `ESM_PARAMS`): an ES module has no `require`, `module`, `exports`,
//! `__filename` or `__dirname`, and may declare its own.

use super::bundle::js_string;
use super::parse::{self, Ctx, ExportTarget, ImportName};
use std::fmt::Write;

pub const ESM_PARAMS: &str = "__tov_m, __tov_x, __tov_r, __tov_f, __tov_d";

fn is_identifier(s: &str) -> bool {
    let mut chars = s.chars();
    matches!(chars.next(), Some(c) if c.is_alphabetic() || c == '_' || c == '$') && chars.all(|c| c.is_alphanumeric() || c == '_' || c == '$')
}

/// `ns.name` or `ns["name"]`.
fn member(ns: &str, name: &ImportName) -> String {
    match name {
        ImportName::Namespace => ns.to_string(),
        ImportName::Default => format!("{ns}.default"),
        ImportName::Named(n) if is_identifier(n) => format!("{ns}.{n}"),
        ImportName::Named(n) => format!("{ns}[{}]", js_string(n)),
    }
}

/// Whether a script has ES module syntax: an `import` or `export` declaration, or
/// `import.meta` (Node's syntax detection for `.js` files in packages without a `type`).
pub fn has_module_syntax(src: &str) -> bool {
    let Ok(toks) = super::lex::tokenize(src) else { return false };
    let text = |i: usize| toks.get(i).map(|t: &super::lex::Tok| t.text(src)).unwrap_or("");
    for (i, t) in toks.iter().enumerate() {
        if t.kind != super::lex::Kind::Ident || (i > 0 && matches!(text(i - 1), "." | "?.")) {
            continue;
        }
        let next = toks.get(i + 1);
        match t.text(src) {
            "import" => {
                if let Some(n) = next
                    && (n.kind == super::lex::Kind::Str || matches!(n.text(src), "{" | "*") || (n.kind == super::lex::Kind::Ident && text(i + 2) != ":" && !matches!(text(i + 1), "(")) || (text(i + 1) == "." && text(i + 2) == "meta"))
                {
                    return true;
                }
            }
            "export" => {
                if matches!(text(i + 1), "{" | "*" | "default" | "const" | "let" | "var" | "function" | "class" | "async") {
                    return true;
                }
            }
            _ => {}
        }
    }
    false
}

/// Rewrites an ES module as a module function body (with `ESM_PARAMS`); returns it with the
/// specifiers it requires.
pub fn to_commonjs(src: &str, ts: bool, jsx: Option<&str>) -> Result<(String, Vec<String>), String> {
    let m = parse::parse(src, ts, jsx).map_err(|e| {
        let line = src[..(e.pos as usize).min(src.len())].bytes().filter(|&b| b == b'\n').count() + 1;
        format!("{} (line {line})", e.message)
    })?;
    let ns = |source: usize| format!("__tov_i{source}");

    // edits: (start, end, replacement), applied in order
    let mut edits: Vec<(u32, u32, String)> = Vec::with_capacity(m.refs.len() + m.removals.len() + 8);
    for &(s, e) in &m.removals {
        // keep line numbers: a removed declaration leaves its line breaks
        let nl = src[s as usize..e as usize].bytes().filter(|&b| b == b'\n').count();
        edits.push((s, e, "\n".repeat(nl)));
    }
    for (s, e, r) in &m.replacements {
        edits.push((*s, *e, r.clone()));
    }
    for (at, text) in &m.inserts {
        edits.push((*at, *at, text.clone()));
    }
    for r in &m.refs {
        let b = &m.bindings[r.binding];
        let target = member(&ns(b.source), &b.imported);
        let text = match r.ctx {
            Ctx::Plain => target,
            // a called import has no receiver
            Ctx::Callee if b.imported != ImportName::Namespace => format!("(0, {target})"),
            Ctx::Callee => target,
            Ctx::Shorthand => format!("{}: {target}", b.local),
        };
        edits.push((r.start, r.end, text));
    }
    for &(s, e) in &m.top_this {
        edits.push((s, e, "undefined".into()));
    }
    for &(s, e) in &m.import_meta {
        edits.push((s, e, "__tov_meta".into()));
    }
    for &(s, e) in &m.dynamic_imports {
        edits.push((s, e, "__tov_import".into()));
    }
    // by position; at one position insertions first, then the longest range (a removal
    // swallows the edits inside it)
    edits.sort_by_key(|&(s, e, _)| (s, e != s, std::cmp::Reverse(e)));

    let mut out = String::with_capacity(src.len() + 256 + m.exports.len() * 48);
    out.push_str("\"use strict\"; ");
    // top-level `await`: the module runs as an async function. Its exports are defined before
    // its first `await`, so importers see them (live) at once; values come as it runs on.
    let tla = m.top_level_await.is_some();
    if tla {
        out.push_str("return (async function () { ");
    }
    // exports first: other modules in an import cycle may read them before this body runs
    out.push_str("__tov_esm(__tov_x, {");
    for (name, target) in &m.exports {
        let value = match target {
            ExportTarget::Local(local) => local.clone(),
            ExportTarget::Import { source, name } => member(&ns(*source), name),
        };
        let _ = write!(out, "{}: function () {{ return {value}; }}, ", js_string(name));
    }
    // (as Node.js's require(esm): `__esModule` is an enumerable key when there's a default export)
    out.push_str(if m.exports.iter().any(|(n, _)| n == "default") { "}, 1); " } else { "}); " });
    let mut requires = Vec::with_capacity(m.sources.len() + m.requires.len());
    for (i, spec) in m.sources.iter().enumerate() {
        if !m.used[i] {
            continue;
        }
        let _ = write!(out, "var {} = __tov_ns(__tov_r({})); ", ns(i), js_string(spec));
        requires.push(spec.clone());
    }
    for &s in &m.stars {
        let _ = write!(out, "__tov_star(__tov_x, {}); ", ns(s));
    }
    if !m.import_meta.is_empty() {
        // (`main`: the module is the script the program was started to run, see bundle.rs's __tov_script)
        out.push_str("var __tov_meta = { url: \"file://\" + __tov_f, filename: __tov_f, dirname: __tov_d, main: __tov_f === globalThis.__tov_script_file, resolve: function (s) { return \"file://\" + __tov_r.resolve(s); } }; ");
    }
    if !m.dynamic_imports.is_empty() {
        out.push_str("var __tov_import = function (s) { return new Promise(function (ok) { ok(__tov_ns(__tov_r(s))); }); }; ");
    }
    for r in m.requires {
        if !requires.contains(&r) {
            requires.push(r);
        }
    }
    // the prologue is one line: the module's lines keep their numbers
    let prologue_len = out.len();
    let mut at = 0usize;
    if src.starts_with("#!") {
        out.push_str("//");
        at = 2;
    }
    for (s, e, text) in edits {
        let (s, e) = (s as usize, e as usize);
        if s < at {
            continue; // overlapping edit (inside a removed declaration)
        }
        out.push_str(&src[at..s]);
        out.push_str(&text);
        at = e;
    }
    out.push_str(&src[at..]);
    // `export { x as "module.exports" }`: what require() returns (Node.js 22's interop)
    if let Some((_, target)) = m.exports.iter().find(|(n, _)| n == "module.exports") {
        let value = match target {
            ExportTarget::Local(local) => local.clone(),
            ExportTarget::Import { source, name } => member(&ns(*source), name),
        };
        let _ = write!(out, "\n__tov_m.exports = {value};");
    }
    if tla {
        out.push_str("\n})();");
    }
    debug_assert!(!out[..prologue_len].contains('\n'));
    Ok((out, requires))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn to_commonjs_js(src: &str) -> Result<(String, Vec<String>), String> {
        to_commonjs(src, false, None)
    }

    #[test]
    fn rewrites_references() {
        let (out, reqs) = to_commonjs_js("import d, { a as b } from \"m\";\nimport * as ns from './n.js';\nexport function f() { return b(d, ns.x, { b }); }\nexport default 42;\n").unwrap();
        assert_eq!(reqs, vec!["m", "./n.js"]);
        assert!(out.contains("return (0, __tov_i0.a)(__tov_i0.default, __tov_i1.x, { b: __tov_i0.a });"), "{out}");
        assert!(out.contains("var __tov_default = 42;"), "{out}");
        assert!(out.contains("\"f\": function () { return f; }"), "{out}");
    }

    #[test]
    fn reexports() {
        let (out, _) = to_commonjs_js("export * from 'a';\nexport { x as y, default } from 'b';\nexport * as z from 'c';").unwrap();
        assert!(out.contains("__tov_star(__tov_x, __tov_i0);"), "{out}");
        assert!(out.contains("\"y\": function () { return __tov_i1.x; }"), "{out}");
        assert!(out.contains("\"default\": function () { return __tov_i1.default; }"), "{out}");
        assert!(out.contains("\"z\": function () { return __tov_i2; }"), "{out}");
    }

    fn ts(src: &str) -> String {
        let (out, _) = to_commonjs(src, true, None).unwrap_or_else(|e| panic!("{e}"));
        // the prologue, then the module
        let out = out.replacen("}, 1); ", "}); ", 1);
        out.split_once("}); ").unwrap().1.split_once("__tov_r(").map_or(out.split_once("}); ").unwrap().1, |(_, b)| b.split_once("; ").unwrap().1).lines().map(str::trim_end).filter(|l| !l.trim().is_empty()).collect::<Vec<_>>().join("\n")
    }

    #[test]
    fn strips_types() {
        assert_eq!(ts("let a: number = 1, b!: Map<string, Array<number>>= new Map<string, number[]>();"), "let a = 1, b= new Map();");
        assert_eq!(ts("function f<T extends { x: 1 }>(this: Foo, a?: T, ...rest: T[]): asserts a is T { return a as unknown as T; }"), "function f( a, ...rest) { return a  ; }");
        assert_eq!(ts("type A<T> = { a: T } | ((x: number) => void);\ninterface B extends C<D> { x: string }\nconst x = y!.z satisfies Q;"), "const x = y.z ;");
        assert_eq!(ts("const f = async <T,>(x: T): Promise<T> => x; g<number>(1); h = a < b > c;"), "const f = async (x) => x; g(1); h = a < b > c;");
        assert_eq!(ts("declare const q: number;\ndeclare module \"m\" { export const x: 1 }\nfunction o(a: string): void;\nfunction o(a: any) {}"), "function o(a) {}");
    }

    #[test]
    fn classes() {
        let out = ts("abstract class A<T> extends B<T> implements C, D<E> {\n  private readonly x: number = 1;\n  declare y: string;\n  [k: string]: any;\n  abstract m(): void;\n  constructor(public a: number, private b = 2) { super(); }\n  get v(): T { return this.x; }\n  n?(): void;\n}");
        assert_eq!(out, " class A extends B  {\n    x = 1;\n  constructor( a,  b = 2) { super(); this.a = a; this.b = b; }\n  get v() { return this.x; }\n}");
    }

    #[test]
    fn enums() {
        let out = ts("export enum E { A, B = 4, C, \"d-e\" = \"x\", F = B << 1 }");
        assert!(out.contains("var E = (function (E) { var A = 0; E[E[\"A\"] = A] = \"A\"; var B = 4; E[E[\"B\"] = B] = \"B\"; var C = B + 1; E[E[\"C\"] = C] = \"C\"; E[\"d-e\"] = \"x\"; var F = B << 1; E[E[\"F\"] = F] = \"F\"; return E; })(E || {});"), "{out}");
    }

    #[test]
    fn type_imports_are_dropped() {
        let (out, _) = to_commonjs("import { A, b } from './a';\nimport type { C } from './c';\nimport { type D } from './d';\nimport { E } from './e';\nimport './f';\nlet x: A = b(); let y: E;\nexport { type C as CC, b };\nexport type { D };", true, None).unwrap();
        assert!(out.contains("__tov_r(\"./a\")") && out.contains("__tov_r(\"./f\")"), "{out}");
        assert!(!out.contains("./c") && !out.contains("./d") && !out.contains("./e"), "{out}");
        assert!(out.contains("\"b\": function () { return __tov_i0.b; }") && !out.contains("CC"), "{out}");
    }
}
