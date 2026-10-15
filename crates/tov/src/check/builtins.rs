//! Built-in types, globals, namespaces and methods.

use super::expr::CallSig;
use super::*;
use crate::ast::Arg;

pub(super) const BUILTIN_TYPES: &[&str] = &[
    "int", "i64", "f64", "number", "f32", "i8", "i16", "i32", "u8", "u16", "u32", "u64", "bool", "boolean", "string", "never", "unknown", "Js", "undefined", "void", "Array", "Map",
    "Set", "Promise", "Record", "bigint", "decimal",
];

pub(super) const GLOBAL_NAMES: &[&str] = &[
    "Math", "console", "expect", "String", "Number", "parseInt", "parseFloat", "isNaN", "int", "f64", "f32", "i8", "i16", "i32", "u8", "u16", "u32", "u64", "process", "Date",
    "performance", "JSON", "Promise", "BigInt", "decimal",
];

/// Runtime calls available to standard-library modules (`__native.name(...)`): parameters, result.
pub(super) fn native_sig(types: &mut Types, name: &str) -> Option<(Vec<TyId>, TyId)> {
    let str_arr = types.array(STR);
    Some(match name {
        "takeError" | "cwd" => (vec![], STR),
        "statInfo" => (vec![STR], STR),
        "access" => (vec![STR, INT], VOID),
        "readFile" => (vec![STR], STR),
        "writeFile" => (vec![STR, STR, BOOL], VOID),
        "exists" => (vec![STR], BOOL),
        "readDir" => (vec![STR], str_arr),
        "mkdir" => (vec![STR, BOOL], VOID),
        "unlink" => (vec![STR], VOID),
        "rm" => (vec![STR, BOOL, BOOL], VOID),
        "httpListen" => {
            let p = |ty| FnParam { ty, inout: false, optional: false };
            let handler = types.func(vec![p(STR), p(STR), p(STR), p(STR)], VOID);
            (vec![INT, STR, handler], INT)
        }
        "httpPort" => (vec![INT], INT),
        "httpStop" => (vec![INT, BOOL], VOID),
        "httpWorkers" => (vec![INT], VOID),
        "urlParse" => (vec![STR, STR], str_arr),
        "urlDecode" => (vec![STR, BOOL], STR),
        "urlEncode" => (vec![STR], STR),
        "requestUrl" => (vec![STR, STR], STR),
        "urlNormalize" => (vec![STR, STR], STR),
        "urlPart" => (vec![STR, INT], STR),
        "httpRespond" => (vec![INT, STR, STR, BOOL], VOID),
        "httpDefer" => (vec![], INT),
        "spawnTail" => (vec![], VOID),
        "httpRespondTo" => (vec![INT, INT, STR, STR, BOOL], VOID),
        "headerIndex" => (vec![STR, STR], INT),
        "headerValue" => (vec![STR, INT], STR),
        "headerRemove" => (vec![STR, STR], STR),
        "headerAppend" => (vec![STR, STR, STR], STR),
        "fetchStart" => (vec![STR, STR, STR, STR, INT, INT, STR, STR, STR], INT),
        "fetchWait" | "fetchBodyWait" | "fetchRead" | "fetchHandle" => (vec![INT], types.promise(INT, NEVER)),
        "fetchTake" => {
            let bytes = types.array(U8);
            (vec![INT], bytes)
        }
        "fetchShare" => (vec![INT], VOID),
        "fetchStatus" => (vec![INT], INT),
        "fetchStatusText" | "fetchHeaders" | "fetchUrl" | "fetchBody" | "fetchErrorCode" | "fetchErrorMessage" => (vec![INT], STR),
        "fetchRedirected" | "fetchBodyClean" => (vec![INT], BOOL),
        "fetchAbort" | "fetchFree" | "timerUnref" => (vec![INT], VOID),
        "bytesToString" => {
            let bytes = types.array(U8);
            (vec![bytes], STR)
        }
        "stringToBytes" => {
            let bytes = types.array(U8);
            (vec![STR], bytes)
        }
        "utf8Clean" => (vec![STR], STR),
        "utf8Complete" => {
            let bytes = types.array(U8);
            (vec![bytes], INT)
        }
        "headerGet" => (vec![STR, STR], STR),
        "byteSlice" => (vec![STR, INT, INT], STR),
        "mimeLower" => (vec![STR], STR),
        "multipartParse" => (vec![STR, STR], str_arr),
        "headerEntries" => (vec![STR], str_arr),
        // sockets (runtime/net.c)
        "netConnect" => (vec![STR, INT, INT, BOOL, STR, STR], INT),
        "netBind" => {
            let p = |ty| FnParam { ty, inout: false, optional: false };
            let bytes = types.array(U8);
            let events = types.func(vec![p(INT), p(bytes), p(STR), p(INT)], VOID);
            (vec![INT, events, STR, STR], VOID)
        }
        "netWrite" => {
            let bytes = types.array(U8);
            (vec![INT, bytes], INT)
        }
        "netWriteText" => (vec![INT, STR], INT),
        "netEnd" | "netFlush" | "netClose" | "netStop" => (vec![INT], VOID),
        "netRef" | "netHalfOpen" | "netListenRef" => (vec![INT, BOOL], VOID),
        "netNoDelay" => (vec![INT, BOOL], BOOL),
        "netAddress" => (vec![INT, INT], STR),
        "netBuffered" | "netListenPort" => (vec![INT], INT),
        "netError" => (vec![], STR),
        "wsUpgrade" => (vec![STR, STR, INT], INT),
        "wsConnect" => (vec![STR, INT, STR, BOOL, BOOL, STR, STR, STR, INT], INT),
        "wsSendText" => (vec![INT, STR, INT], INT),
        "wsSendBytes" => {
            let bytes = types.array(U8);
            (vec![INT, bytes, INT], INT)
        }
        "wsClose" => (vec![INT, INT, STR, BOOL], VOID),
        "wsSubscribe" => (vec![INT, STR, BOOL], VOID),
        "wsSubscribed" => (vec![INT, STR], BOOL),
        "wsTopics" => (vec![INT], str_arr),
        "wsPublishText" => (vec![STR, STR, INT], INT),
        "wsPublishBytes" => {
            let bytes = types.array(U8);
            (vec![STR, bytes, INT], INT)
        }
        "wsSubscribers" => (vec![STR], INT),
        "wsProtocol" => (vec![INT], STR),
        "netListen" => {
            let p = |ty| FnParam { ty, inout: false, optional: false };
            let on_accept = types.func(vec![p(INT)], VOID);
            (vec![STR, INT, STR, STR, STR, on_accept], INT)
        }
        "headerValues" => (vec![STR, STR], str_arr),
        _ => return None,
    })
}

const CONVERSIONS: &[&str] = &["int", "i64", "f64", "f32", "i8", "i16", "i32", "u8", "u16", "u32", "u64"];

pub(super) fn is_builtin_type(name: &str) -> bool {
    BUILTIN_TYPES.contains(&name)
}

pub(super) fn is_builtin_ns(name: &str) -> bool {
    matches!(name, "Math" | "console" | "JSON" | "process" | "Date" | "performance" | "Promise" | "BigInt" | "Array")
}

pub(super) fn is_builtin_fn(name: &str) -> bool {
    matches!(name, "expect" | "String" | "Number" | "BigInt" | "decimal" | "parseInt" | "parseFloat" | "isNaN" | "test")
        || CONVERSIONS.contains(&name)
        || TIMER_FNS.contains(&name)
}

/// Timer globals.
pub(crate) const TIMER_FNS: &[&str] = &["setTimeout", "setInterval", "clearTimeout", "clearInterval", "queueMicrotask"];

pub(super) fn removed_global(name: &str) -> Option<String> {
    Some(match name {
        "arguments" => "`arguments` is not supported; declare the parameters".to_string(),
        "eval" | "Function" => format!("`{name}` is not supported: there is no runtime code evaluation"),
        "globalThis" | "window" | "document" | "global" => format!("`{name}` is not available; Tov compiles to native programs"),
        "require" | "module" | "exports" => "CommonJS is not supported; use `import { name } from \"./file\"`".to_string(),
        "Symbol" | "Proxy" | "Reflect" | "WeakMap" | "WeakSet" => format!("`{name}` is not supported"),
        "Object" => "`Object` is not supported: records have fixed fields; use `Map` for dynamic keys".to_string(),
        "Array" => "`Array` is not a value: `Array.isArray(x)` tests a value, and arrays are made with literals and `map`".to_string(),
        _ => return None,
    })
}

pub(super) struct Hint {
    code: &'static str,
    msg: String,
    note: Option<(&'static str, String)>,
    fix: Option<(String, String)>,
}

impl Hint {
    pub(super) fn into_diag(self, span: Span) -> Diagnostic {
        let mut d = Diagnostic::new(self.code, span, self.msg);
        if let Some((l, t)) = self.note {
            d = d.note(l, t);
        }
        if let Some((label, text)) = self.fix {
            d = d.fix(Applicability::Maybe, label, span, text);
        }
        d
    }
}

/// Explanations for members that exist in TypeScript but not (or differently) in Tov.
pub(super) fn member_hint(types: &Types, base: TyId, name: &str) -> Option<Hint> {
    let ty = types.get(base);
    match (ty, name) {
        (Ty::Str | Ty::StrLit(_), "length") => Some(Hint {
            code: "X0010",
            msg: "strings have no `.length`: it is ambiguous for UTF-8 text".to_string(),
            note: Some(("instead", "`s.byteLength` for bytes, `s.chars().length` for characters".to_string())),
            fix: Some(("use `byteLength`".to_string(), "byteLength".to_string())),
        }),
        (Ty::Array(_), "size") => Some(Hint { code: "T0107", msg: "arrays have `length`, not `size`".to_string(), note: None, fix: Some(("use `length`".to_string(), "length".to_string())) }),
        (Ty::Map(..) | Ty::Set(_), "length") => Some(Hint { code: "T0107", msg: "maps and sets have `size`, not `length`".to_string(), note: None, fix: Some(("use `size`".to_string(), "size".to_string())) }),
        _ => None,
    }
}

pub(super) fn property(types: &Types, ty: TyId, name: &str) -> Option<TyId> {
    match (types.get(ty), name) {
        (Ty::Array(_), "length") => Some(INT),
        (Ty::Str | Ty::StrLit(_), "byteLength") => Some(INT),
        (Ty::Map(..) | Ty::Set(_), "size") => Some(INT),
        _ => None,
    }
}

const ARRAY_METHODS: &[&str] = &[
    "push", "pop", "shift", "unshift", "map", "filter", "forEach", "reduce", "some", "every", "find", "findIndex", "indexOf", "lastIndexOf", "includes", "join", "slice", "concat",
    "reverse", "sort", "at", "flatMap",
];
const STRING_METHODS: &[&str] = &[
    "chars", "slice", "includes", "startsWith", "endsWith", "indexOf", "split", "trim", "trimStart", "trimEnd", "toUpperCase", "toLowerCase", "replace", "replaceAll", "repeat",
    "padStart", "padEnd", "toString",
];
const MAP_METHODS: &[&str] = &["get", "set", "has", "delete", "clear", "keys", "values", "forEach"];
const SET_METHODS: &[&str] = &["add", "has", "delete", "clear", "values", "forEach"];
const NUMBER_METHODS: &[&str] = &["toFixed", "toString"];

fn methods_of(types: &Types, ty: TyId) -> &'static [&'static str] {
    match types.get(ty) {
        Ty::Array(_) => ARRAY_METHODS,
        Ty::Str | Ty::StrLit(_) => STRING_METHODS,
        Ty::Map(..) => MAP_METHODS,
        Ty::Set(_) => SET_METHODS,
        _ if types.is_numeric(ty) => NUMBER_METHODS,
        Ty::Bool | Ty::BigInt => &["toString"],
        Ty::Decimal => &["toString", "toFixed", "round", "floor", "ceil", "trunc", "abs"],
        _ => &[],
    }
}

pub(super) fn is_method(c: &mut Checker, base: TyId, name: &str) -> bool {
    let members = c.flat_members(base);
    members.len() == 1 && methods_of(&c.types, members[0]).contains(&name)
}

pub(super) fn property_names(c: &mut Checker, ty: TyId) -> Vec<&'static str> {
    let mut out: Vec<&'static str> = match c.types.get(ty) {
        Ty::Array(_) => vec!["length"],
        Ty::Str | Ty::StrLit(_) => vec!["byteLength"],
        Ty::Map(..) | Ty::Set(_) => vec!["size"],
        _ => Vec::new(),
    };
    out.extend(methods_of(&c.types, ty).iter().copied());
    out
}

/// Short receiver name for call descriptions (`Array.map`), computed without formatting types.
pub(super) fn type_label(types: &Types, ty: TyId) -> &'static str {
    match types.get(ty) {
        Ty::Array(_) => "Array",
        Ty::Str | Ty::StrLit(_) => "string",
        Ty::Map(..) => "Map",
        Ty::Set(_) => "Set",
        Ty::Bool => "bool",
        Ty::BigInt => "bigint",
        Ty::Decimal => "decimal",
        _ => "number",
    }
}

pub(super) struct MethodSig {
    pub sig: CallSig,
    pub mutates: bool,
}

fn p(ty: TyId) -> FnParam {
    FnParam { ty, inout: false, optional: false }
}

fn opt(c: &mut Checker, ty: TyId) -> FnParam {
    let ty = c.types.optional(ty);
    FnParam { ty, inout: false, optional: true }
}

fn sig(params: Vec<(FnParam, &str)>, ret: TyId) -> CallSig {
    let (params, names) = params.into_iter().map(|(p, n)| (p, n.to_string())).unzip();
    CallSig { tparams: Vec::new(), params, names, rest: None, ret, throws: NEVER }
}

pub(super) fn method(c: &mut Checker, base: TyId, name: &str) -> Option<MethodSig> {
    let members = c.flat_members(base);
    if members.len() != 1 {
        return None;
    }
    let ty = members[0];
    let mk = |sig: CallSig, mutates: bool| Some(MethodSig { sig, mutates });
    match *c.types.get(ty) {
        Ty::Array(e) => {
            let arr = ty;
            let opt_e = c.types.optional(e);
            let pred = c.types.func(vec![p(e), FnParam { ty: INT, inout: false, optional: true }], BOOL);
            match name {
                "push" | "unshift" => mk(CallSig { tparams: Vec::new(), params: Vec::new(), names: Vec::new(), rest: Some(e), ret: INT, throws: NEVER }, true),
                "pop" | "shift" => mk(sig(vec![], opt_e), true),
                "map" | "flatMap" => {
                    let u_name = c.syms.u;
                    let u = c.new_gparam(u_name, None);
                    let ut = c.types.intern(Ty::Param(u));
                    let ret_elem = if name == "map" { ut } else { c.types.array(ut) };
                    let f = c.types.func(vec![p(e), FnParam { ty: INT, inout: false, optional: true }], ret_elem);
                    let ret = c.types.array(ut);
                    mk(CallSig { tparams: vec![u], params: vec![p(f)], names: vec!["fn".into()], rest: None, ret, throws: NEVER }, false)
                }
                "filter" => mk(sig(vec![(p(pred), "predicate")], arr), false),
                "forEach" => {
                    let f = c.types.func(vec![p(e), FnParam { ty: INT, inout: false, optional: true }], VOID);
                    mk(sig(vec![(p(f), "fn")], VOID), false)
                }
                "reduce" => {
                    let u_name = c.syms.u;
                    let u = c.new_gparam(u_name, None);
                    let ut = c.types.intern(Ty::Param(u));
                    let f = c.types.func(vec![p(ut), p(e), FnParam { ty: INT, inout: false, optional: true }], ut);
                    mk(CallSig { tparams: vec![u], params: vec![p(f), p(ut)], names: vec!["fn".into(), "initial".into()], rest: None, ret: ut, throws: NEVER }, false)
                }
                "some" | "every" => mk(sig(vec![(p(pred), "predicate")], BOOL), false),
                "find" => mk(sig(vec![(p(pred), "predicate")], opt_e), false),
                "findIndex" => mk(sig(vec![(p(pred), "predicate")], INT), false),
                "indexOf" | "lastIndexOf" => mk(sig(vec![(p(e), "value")], INT), false),
                "includes" => mk(sig(vec![(p(e), "value")], BOOL), false),
                "join" => {
                    let sep = opt(c, STR);
                    mk(sig(vec![(sep, "separator")], STR), false)
                }
                "slice" => {
                    let (a, b) = (opt(c, INT), opt(c, INT));
                    mk(sig(vec![(a, "start"), (b, "end")], arr), false)
                }
                "concat" => mk(sig(vec![(p(arr), "other")], arr), false),
                "reverse" => mk(sig(vec![], arr), true),
                "sort" => {
                    let cmp = c.types.func(vec![p(e), p(e)], F64);
                    let cmp = opt(c, cmp);
                    mk(sig(vec![(cmp, "compare")], arr), true)
                }
                "at" => mk(sig(vec![(p(INT), "index")], opt_e), false),
                _ => None,
            }
        }
        Ty::Str | Ty::StrLit(_) => {
            let str_arr = c.types.array(STR);
            match name {
                "chars" => mk(sig(vec![], str_arr), false),
                "slice" => {
                    let end = opt(c, INT);
                    mk(sig(vec![(p(INT), "start"), (end, "end")], STR), false)
                }
                "includes" | "startsWith" | "endsWith" => mk(sig(vec![(p(STR), "search")], BOOL), false),
                "indexOf" => mk(sig(vec![(p(STR), "search")], INT), false),
                "split" => mk(sig(vec![(p(STR), "separator")], str_arr), false),
                "trim" | "trimStart" | "trimEnd" | "toUpperCase" | "toLowerCase" | "toString" => mk(sig(vec![], STR), false),
                "replace" | "replaceAll" => mk(sig(vec![(p(STR), "search"), (p(STR), "replacement")], STR), false),
                "repeat" => mk(sig(vec![(p(INT), "count")], STR), false),
                "padStart" | "padEnd" => {
                    let fill = opt(c, STR);
                    mk(sig(vec![(p(INT), "length"), (fill, "fill")], STR), false)
                }
                _ => None,
            }
        }
        Ty::Map(k, v) => {
            let opt_v = c.types.optional(v);
            match name {
                "get" => mk(sig(vec![(p(k), "key")], opt_v), false),
                "set" => mk(sig(vec![(p(k), "key"), (p(v), "value")], VOID), true),
                "has" => mk(sig(vec![(p(k), "key")], BOOL), false),
                "delete" => mk(sig(vec![(p(k), "key")], BOOL), true),
                "clear" => mk(sig(vec![], VOID), true),
                "keys" => {
                    let a = c.types.array(k);
                    mk(sig(vec![], a), false)
                }
                "values" => {
                    let a = c.types.array(v);
                    mk(sig(vec![], a), false)
                }
                "forEach" => {
                    let f = c.types.func(vec![p(v), FnParam { ty: k, inout: false, optional: true }], VOID);
                    mk(sig(vec![(p(f), "fn")], VOID), false)
                }
                _ => None,
            }
        }
        Ty::Set(e) => match name {
            "add" => mk(sig(vec![(p(e), "value")], VOID), true),
            "has" => mk(sig(vec![(p(e), "value")], BOOL), false),
            "delete" => mk(sig(vec![(p(e), "value")], BOOL), true),
            "clear" => mk(sig(vec![], VOID), true),
            "values" => {
                let a = c.types.array(e);
                mk(sig(vec![], a), false)
            }
            "forEach" => {
                let f = c.types.func(vec![p(e)], VOID);
                mk(sig(vec![(p(f), "fn")], VOID), false)
            }
            _ => None,
        },
        Ty::Bool if name == "toString" => mk(sig(vec![], STR), false),
        Ty::Decimal => match name {
            "toString" | "toJSON" => mk(sig(vec![], STR), false),
            "toFixed" => mk(sig(vec![(p(INT), "digits")], STR), false),
            "floor" | "ceil" | "trunc" | "abs" => mk(sig(vec![], DECIMAL), false),
            "round" => {
                let places = opt(c, INT);
                let modes: Vec<TyId> = ["halfEven", "halfExpand", "halfTrunc", "halfCeil", "halfFloor", "ceil", "floor", "trunc", "expand"]
                    .iter()
                    .filter_map(|m| c.interner.lookup(m))
                    .map(|s| c.types.str_lit(s))
                    .collect();
                let mode = c.types.union(&modes);
                let mode = opt(c, mode);
                mk(sig(vec![(places, "places"), (mode, "mode")], DECIMAL), false)
            }
            _ => None,
        },
        Ty::BigInt if name == "toString" => {
            let radix = opt(c, INT);
            mk(sig(vec![(radix, "radix")], STR), false)
        }
        _ if c.types.is_numeric(ty) => match name {
            "toFixed" => mk(sig(vec![(p(INT), "digits")], STR), false),
            "toString" => {
                let radix = opt(c, INT);
                mk(sig(vec![(radix, "radix")], STR), false)
            }
            _ => None,
        },
        _ => None,
    }
}

const MATH_CONSTS: &[&str] = &["PI", "E", "LN2", "LN10", "LOG2E", "LOG10E", "SQRT2", "SQRT1_2"];
const MATH_F64_FNS: &[(&str, usize)] = &[
    ("sqrt", 1),
    ("cbrt", 1),
    ("sin", 1),
    ("cos", 1),
    ("tan", 1),
    ("asin", 1),
    ("acos", 1),
    ("atan", 1),
    ("atan2", 2),
    ("sinh", 1),
    ("cosh", 1),
    ("tanh", 1),
    ("exp", 1),
    ("expm1", 1),
    ("log", 1),
    ("log2", 1),
    ("log10", 1),
    ("log1p", 1),
    ("pow", 2),
    ("hypot", 2),
    ("sign", 1),
    ("random", 0),
];
const MATH_INT_FNS: &[&str] = &["floor", "ceil", "round", "trunc"];

impl<'a> Checker<'a> {
    fn check_args_loose(&mut self, args: &[Arg]) {
        for a in args {
            self.expr(a.expr, None);
        }
    }

    pub(super) fn builtin_ns_member(&mut self, ns: &str, name: Sym, name_span: Span) -> TyId {
        let n = self.name(name).to_string();
        if ns == "Math" && MATH_CONSTS.contains(&n.as_str()) {
            return F64;
        }
        if ns == "process" {
            match n.as_str() {
                "argv" => return self.types.array(STR),
                "platform" | "arch" => return STR,
                "pid" => return INT,
                // `process.env.NAME`, `process.stdout.write(s)`: namespaces of their own.
                "env" | "stdout" | "stderr" => return self.types.intern(Ty::BuiltinNs(name)),
                _ => {}
            }
        }
        let known: Vec<&str> = match ns {
            "Math" => MATH_CONSTS.iter().copied().chain(MATH_F64_FNS.iter().map(|f| f.0)).chain(MATH_INT_FNS.iter().copied()).chain(["abs", "min", "max"]).collect(),
            "process" => vec!["argv", "env", "exit", "cwd", "platform", "arch", "pid", "stdout", "stderr"],
            "Date" | "performance" => vec!["now"],
            "JSON" => vec!["stringify", "parse"],
            "Promise" => vec!["resolve", "reject", "all", "race"],
            "BigInt" => vec!["asIntN", "asUintN"],
            _ => vec!["log", "error", "warn", "info"],
        };
        if known.contains(&n.as_str()) {
            self.report(Diagnostic::new("T0203", name_span, format!("`{ns}.{n}` must be called")));
            return ERROR;
        }
        self.unknown_ns_member(ns, &n, &known, name_span);
        ERROR
    }

    /// `Promise.resolve(v)` (a fulfilled promise) and `Promise.reject(e)` (a rejected one). The
    /// value type comes from the argument or the expected type.
    fn promise_call(&mut self, name: &str, name_span: Span, args: &[Arg], exp: Option<TyId>, span: Span) -> TyId {
        let expected = exp.and_then(|t| {
            self.flat_members(t).into_iter().find_map(|m| match *self.types.get(m) {
                Ty::Promise(v, e) => Some((v, e)),
                _ => None,
            })
        });
        match name {
            "resolve" => {
                let v = match args {
                    [] => expected.map(|(v, _)| v).unwrap_or(VOID),
                    [a] => {
                        let hint = expected.map(|(v, _)| v);
                        let t = self.expr(a.expr, hint);
                        match hint {
                            Some(h) if self.assignable(t, h) => h,
                            _ => self.types.widen(t),
                        }
                    }
                    _ => {
                        self.report(Diagnostic::new("T0201", span, format!("`Promise.resolve` takes 1 argument, found {}", args.len())));
                        self.check_args_loose(args);
                        return ERROR;
                    }
                };
                self.types.promise(v, NEVER)
            }
            "reject" => {
                let [a] = args else {
                    self.report(Diagnostic::new("T0201", span, format!("`Promise.reject` takes 1 argument (an error), found {}", args.len())));
                    self.check_args_loose(args);
                    return ERROR;
                };
                let e = self.expr(a.expr, None);
                let s = self.ast().expr(a.expr).span;
                self.check_throws_type(e, s);
                let v = expected.map(|(v, _)| v).unwrap_or(NEVER);
                self.types.promise(v, e)
            }
            // `Promise.all(ps)`: every value, in order (rejects with the first rejection);
            // `Promise.race(ps)`: the first to settle.
            "all" | "race" => {
                let [a] = args else {
                    self.report(Diagnostic::new("T0201", span, format!("`Promise.{name}` takes 1 argument (an array of promises), found {}", args.len())));
                    self.check_args_loose(args);
                    return ERROR;
                };
                let hint = expected.and_then(|(v, e)| {
                    let elem = if name == "all" {
                        match *self.types.get(v) {
                            Ty::Array(x) => x,
                            _ => return None,
                        }
                    } else {
                        v
                    };
                    let p = self.types.promise(elem, e);
                    Some(self.types.array(p))
                });
                let t = self.expr(a.expr, hint);
                let s = self.ast().expr(a.expr).span;
                let Ty::Array(elem) = *self.types.get(t) else {
                    if t != ERROR {
                        let shown = self.show(t);
                        self.report(Diagnostic::new("T0001", s, format!("`Promise.{name}` takes an array of promises, found `{shown}`")));
                    }
                    return ERROR;
                };
                let (mut values, mut errors) = (Vec::new(), Vec::new());
                for m in self.flat_members(elem) {
                    match *self.types.get(m) {
                        Ty::Promise(v, e) => {
                            values.push(v);
                            if e != NEVER {
                                errors.push(e);
                            }
                        }
                        Ty::Error => return ERROR,
                        _ => {
                            let shown = self.show(elem);
                            self.report(
                                Diagnostic::new("T0001", s, format!("`Promise.{name}` takes an array of promises, found `{shown}[]`"))
                                    .note("instead", "wrap plain values: `Promise.resolve(value)`"),
                            );
                            return ERROR;
                        }
                    }
                }
                let v = self.types.union(&values);
                let e = if errors.is_empty() { NEVER } else { self.types.union(&errors) };
                let v = if name == "all" { self.types.array(v) } else { v };
                self.types.promise(v, e)
            }
            _ => {
                self.check_args_loose(args);
                self.unknown_ns_member("Promise", name, &["resolve", "reject", "all", "race"], name_span);
                ERROR
            }
        }
    }

    fn unknown_ns_member(&mut self, ns: &str, n: &str, known: &[&str], span: Span) {
        let mut d = Diagnostic::new("T0107", span, format!("`{ns}` has no member `{n}`"));
        let sug = similar(n, known.iter().copied());
        if let Some(first) = sug.first() {
            d = d.note("did you mean", sug.join(", ")).fix(Applicability::Maybe, format!("use `{first}`"), span, first.to_string());
        }
        self.report(d);
    }

    pub(super) fn builtin_ns_call(&mut self, ns: &str, name: &str, name_span: Span, args: &[Arg], exp: Option<TyId>, span: Span) -> TyId {
        match ns {
            "console" => {
                let known = ["log", "error", "warn", "info"];
                if !known.contains(&name) {
                    self.unknown_ns_member(ns, name, &known, name_span);
                }
                for a in args {
                    let t = self.expr(a.expr, None);
                    if !self.printable(t) {
                        let msg = format!("a value of type `{}` can't be printed", self.show(t));
                        let s = self.ast().expr(a.expr).span;
                        self.report(Diagnostic::new("T0510", s, msg));
                    }
                }
                VOID
            }
            "JSON" => self.json_call(name, name_span, args, exp, span),
            "Promise" => self.promise_call(name, name_span, args, exp, span),
            "Array" => {
                if name != "isArray" {
                    self.check_args_loose(args);
                    self.report(Diagnostic::new("U0017", name_span, format!("`Array.{name}` is not supported; use array literals and `map`")));
                    return ERROR;
                }
                if args.len() != 1 {
                    self.report(Diagnostic::new("T0201", span, format!("`Array.isArray` takes 1 argument, found {}", args.len())));
                }
                self.check_args_loose(args);
                BOOL
            }
            "BigInt" => {
                if !matches!(name, "asIntN" | "asUintN") {
                    self.check_args_loose(args);
                    self.unknown_ns_member(ns, name, &["asIntN", "asUintN"], name_span);
                    return ERROR;
                }
                let cs = CallSig { tparams: Vec::new(), params: vec![p(INT), p(BIGINT)], names: vec!["bits".into(), "bigint".into()], rest: None, ret: BIGINT, throws: NEVER };
                self.call_sig(&cs, &format!("BigInt.{name}"), &[], args, exp, span)
            }
            "process" => {
                let (params, ret): (Vec<FnParam>, TyId) = match name {
                    "exit" => (vec![opt(self, INT)], NEVER),
                    "cwd" => (vec![], STR),
                    _ => {
                        self.check_args_loose(args);
                        self.unknown_ns_member(ns, name, &["argv", "env", "exit", "cwd", "platform", "arch", "pid", "stdout", "stderr"], name_span);
                        return ERROR;
                    }
                };
                let names = params.iter().map(|_| "code".to_string()).collect();
                let cs = CallSig { tparams: Vec::new(), params, names, rest: None, ret, throws: NEVER };
                self.call_sig(&cs, &format!("process.{name}"), &[], args, exp, span)
            }
            "Date" | "performance" => {
                if name != "now" {
                    self.check_args_loose(args);
                    let msg = if ns == "Date" { "only `Date.now()` is supported (no `Date` objects yet)" } else { "only `performance.now()` is supported" };
                    self.report(Diagnostic::new("U0017", name_span, msg));
                    return ERROR;
                }
                let cs = CallSig { tparams: Vec::new(), params: Vec::new(), names: Vec::new(), rest: None, ret: F64, throws: NEVER };
                self.call_sig(&cs, &format!("{ns}.now"), &[], args, exp, span)
            }
            "stdout" | "stderr" => {
                if name != "write" {
                    self.check_args_loose(args);
                    self.unknown_ns_member(&format!("process.{ns}"), name, &["write"], name_span);
                    return ERROR;
                }
                let cs = CallSig { tparams: Vec::new(), params: vec![p(STR)], names: vec!["text".into()], rest: None, ret: VOID, throws: NEVER };
                self.call_sig(&cs, &format!("process.{ns}.write"), &[], args, exp, span)
            }
            "__native" => match native_sig(&mut self.types, name) {
                Some((ps, ret)) => {
                    let names = ps.iter().enumerate().map(|(i, _)| format!("a{i}")).collect();
                    let cs = CallSig { tparams: Vec::new(), params: ps.into_iter().map(p).collect(), names, rest: None, ret, throws: NEVER };
                    self.call_sig(&cs, &format!("__native.{name}"), &[], args, exp, span)
                }
                None => {
                    self.check_args_loose(args);
                    self.report(Diagnostic::new("T0107", name_span, format!("unknown native function `{name}`")));
                    ERROR
                }
            },
            _ => self.math_call(name, name_span, args, exp, span),
        }
    }

    /// `JSON.stringify(value, undefined?, indent?)` and `JSON.parse(text)` (the result type comes
    /// from context: `const c: Config = JSON.parse(text)` or `JSON.parse(text) as Config`).
    fn json_call(&mut self, name: &str, name_span: Span, args: &[Arg], exp: Option<TyId>, span: Span) -> TyId {
        let call = self.pending_call.take();
        match name {
            "stringify" => {
                if args.is_empty() || args.len() > 3 {
                    self.report(Diagnostic::new("T0201", span, format!("`JSON.stringify` takes 1 to 3 arguments, found {}", args.len())));
                }
                if let Some(a) = args.first() {
                    let t = self.expr(a.expr, None);
                    if !self.json_ok(t, true) {
                        let s = self.ast().expr(a.expr).span;
                        let shown = self.show(t);
                        self.report(Diagnostic::new("T0840", s, format!("`{shown}` can't be converted to JSON")).note("supported", "numbers, strings, booleans, arrays, records, unions of these, `Map<string, T>` and class instances"));
                    }
                }
                if let Some(a) = args.get(1) {
                    let t = self.expr(a.expr, None);
                    if t != UNDEFINED && t != ERROR {
                        let s = self.ast().expr(a.expr).span;
                        self.report(Diagnostic::new("U0017", s, "`JSON.stringify` replacers are not supported; pass `undefined`"));
                    }
                }
                if let Some(a) = args.get(2) {
                    let hint = self.types.union(&[INT, STR]);
                    let t = self.expr(a.expr, Some(INT));
                    let s = self.ast().expr(a.expr).span;
                    self.expect_assignable(t, hint, s, Some("the indentation (spaces, or a string)".into()));
                }
                STR
            }
            "parse" => {
                if args.len() != 1 {
                    self.report(Diagnostic::new("T0201", span, format!("`JSON.parse` takes 1 argument, found {}", args.len())));
                    self.check_args_loose(args);
                    return ERROR;
                }
                let t = self.expr(args[0].expr, Some(STR));
                let s = self.ast().expr(args[0].expr).span;
                self.expect_assignable(t, STR, s, None);
                let Some(target) = exp.filter(|&x| x != ERROR && x != UNKNOWN) else {
                    if exp != Some(ERROR) {
                        self.report(
                            Diagnostic::new("T0841", span, "`JSON.parse` needs to know what type to produce")
                                .note("instead", "annotate the binding (`const c: Config = JSON.parse(text)`) or cast (`JSON.parse(text) as Config`)")
                                .note("why", "the parsed value is checked against the type, so it's safe to use"),
                        );
                    }
                    return ERROR;
                };
                if !self.json_ok(target, false) {
                    let shown = self.show(target);
                    self.report(Diagnostic::new("T0840", span, format!("JSON can't be parsed into `{shown}`")).note("supported", "numbers, strings, booleans, arrays, records, unions of these and `Map<string, T>`"));
                    return ERROR;
                }
                let syntax = self.builtin_class("SyntaxError");
                if syntax != ERROR {
                    self.on_throw(syntax, span, Some("JSON.parse"), call);
                }
                if let (Some(c), Some(f)) = (call, self.facts_mut())
                    && let Some(cf) = f.calls.get_mut(&c)
                {
                    cf.ret = target;
                }
                target
            }
            _ => {
                self.check_args_loose(args);
                self.unknown_ns_member("JSON", name, &["stringify", "parse"], name_span);
                ERROR
            }
        }
    }

    /// Can values of this type go to and (`!allow_class`) come from JSON?
    pub(super) fn json_ok(&mut self, t: TyId, allow_class: bool) -> bool {
        let mut seen = Vec::new();
        self.json_ok_in(t, allow_class, &mut seen)
    }

    fn json_ok_in(&mut self, t: TyId, allow_class: bool, seen: &mut Vec<TyId>) -> bool {
        if seen.contains(&t) {
            return true;
        }
        seen.push(t);
        match *self.types.get(t) {
            Ty::Error | Ty::Int | Ty::F64 | Ty::F32 | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64 | Ty::Decimal | Ty::Bool | Ty::Str | Ty::StrLit(_) | Ty::Undefined => true,
            // A generic `T`: the instantiated type is written (functions come out as `null`).
            Ty::Param(_) => true,
            Ty::Array(e) => self.json_ok_in(e, allow_class, seen),
            Ty::Map(k, v) => k == STR && self.json_ok_in(v, allow_class, seen),
            Ty::Record(fs) => {
                let fs = self.types.fields(fs).to_vec();
                fs.iter().all(|f| self.json_ok_in(f.ty, allow_class, seen))
            }
            Ty::Union(ms) => {
                let ms = self.types.tys(ms).to_vec();
                ms.iter().all(|&m| self.json_ok_in(m, allow_class, seen))
            }
            Ty::Rec(..) => {
                let u = self.unfold(t);
                self.json_ok_in(u, allow_class, seen)
            }
            Ty::Class(..) if allow_class => {
                let fields = self.class_as_fields(t);
                fields.iter().filter(|f| !matches!(self.types.get(f.ty), Ty::Func(..))).map(|f| f.ty).collect::<Vec<_>>().into_iter().all(|ft| self.json_ok_in(ft, allow_class, seen))
            }
            _ => false,
        }
    }

    fn math_call(&mut self, name: &str, name_span: Span, args: &[Arg], exp: Option<TyId>, span: Span) -> TyId {
        let desc = format!("Math.{name}");
        if let Some(&(_, arity)) = MATH_F64_FNS.iter().find(|f| f.0 == name) {
            let names = ["x", "y"];
            let cs = CallSig { tparams: Vec::new(), params: (0..arity).map(|_| p(F64)).collect(), names: names[..arity].iter().map(|s| s.to_string()).collect(), rest: None, ret: F64, throws: NEVER };
            return self.call_sig(&cs, &desc, &[], args, exp, span);
        }
        if MATH_INT_FNS.contains(&name) {
            let cs = CallSig { tparams: Vec::new(), params: vec![p(F64)], names: vec!["x".into()], rest: None, ret: INT, throws: NEVER };
            return self.call_sig(&cs, &desc, &[], args, exp, span);
        }
        match name {
            "abs" => {
                if args.len() != 1 {
                    self.report(Diagnostic::new("T0201", span, format!("`{desc}` takes 1 argument, found {}", args.len())));
                    self.check_args_loose(args);
                    return ERROR;
                }
                let t = self.expr(args[0].expr, exp);
                if t != ERROR && !self.types.is_numeric(t) {
                    let msg = format!("`{desc}` needs a number, found `{}`", self.show(t));
                    let s = self.ast().expr(args[0].expr).span;
                    self.report(Diagnostic::new("T0001", s, msg));
                    return ERROR;
                }
                t
            }
            "min" | "max" => {
                if args.is_empty() {
                    self.report(Diagnostic::new("T0201", span, format!("`{desc}` takes at least 1 argument")));
                    return ERROR;
                }
                let mut tys = Vec::new();
                for a in args {
                    let t = self.expr(a.expr, exp);
                    if t != ERROR && !self.types.is_numeric(t) {
                        let msg = format!("`{desc}` needs numbers, found `{}`", self.show(t));
                        let s = self.ast().expr(a.expr).span;
                        let mut d = Diagnostic::new("T0001", s, msg);
                        if matches!(self.types.get(t), Ty::Array(_)) {
                            d = d.note("note", "spread is not supported; use `xs.reduce((a, b) => Math.max(a, b), xs[0]!)`");
                        }
                        self.report(d);
                        return ERROR;
                    }
                    tys.push(t);
                }
                if tys.iter().all(|&t| t == tys[0]) {
                    tys[0]
                } else if tys.iter().all(|&t| t == INT || t == F64) {
                    F64
                } else {
                    self.report(Diagnostic::new("T0504", span, format!("`{desc}` arguments mix integer types")));
                    ERROR
                }
            }
            _ => {
                self.check_args_loose(args);
                let known: Vec<&str> = MATH_CONSTS.iter().copied().chain(MATH_F64_FNS.iter().map(|f| f.0)).chain(MATH_INT_FNS.iter().copied()).chain(["abs", "min", "max"]).collect();
                self.unknown_ns_member("Math", name, &known, name_span);
                ERROR
            }
        }
    }

    /// Calls to built-in global functions; `None` if `name` isn't one.
    pub(super) fn global_call(&mut self, name: &str, callee_span: Span, type_args: &[crate::ast::TypeId], args: &[Arg], _exp: Option<TyId>, span: Span) -> Option<TyId> {
        let _ = type_args;
        let one = |c: &mut Self| -> Option<TyId> {
            if args.len() != 1 {
                c.report(Diagnostic::new("T0201", span, format!("`{name}` takes 1 argument, found {}", args.len())));
                c.check_args_loose(args);
                return None;
            }
            Some(c.expr(args[0].expr, None))
        };
        let result = match name {
            "expect" => match one(self) {
                Some(t) => self.types.intern(Ty::Expect(t)),
                None => ERROR,
            },
            "String" => {
                if let Some(t) = one(self)
                    && !self.printable(t) {
                        let msg = format!("a value of type `{}` can't be converted to a string", self.show(t));
                        self.report(Diagnostic::new("T0510", span, msg));
                    }
                STR
            }
            // `BigInt(x)`: from an integer (exact), a string (SyntaxError if it isn't one) or a
            // number (RangeError unless it's an integer), as in JavaScript.
            "BigInt" => {
                let call = self.pending_call.take();
                if let Some(t) = one(self) {
                    let mut throws = Vec::new();
                    for m in self.flat_members(t) {
                        if m == ERROR || m == BIGINT || m == BOOL || self.types.is_int(m) {
                            continue;
                        }
                        if self.types.is_string(m) {
                            throws.push("SyntaxError");
                        } else if m == DECIMAL {
                            throws.push("RangeError");
                        } else if self.types.is_string(m) {
                            throws.push("SyntaxError");
                        } else if self.types.is_float(m) {
                            throws.push("RangeError");
                        } else {
                            let s = self.ast().expr(args[0].expr).span;
                            let msg = format!("`BigInt` converts integers, numbers and strings, found `{}`", self.show(t));
                            self.report(Diagnostic::new("T0001", s, msg));
                            throws.clear();
                            break;
                        }
                    }
                    throws.sort();
                    throws.dedup();
                    for cls in throws {
                        let c = self.builtin_class(cls);
                        if c != ERROR {
                            self.on_throw(c, span, Some("BigInt"), call);
                        }
                    }
                }
                BIGINT
            }
            // `decimal(x)`: an integer or bigint (exactly; past 34 digits traps), a number (its
            // shortest decimal form, as it prints: decimal(0.1) is 0.1; NaN and the infinities
            // trap) or a string (`try`: SyntaxError unless it's a number, RangeError past 34
            // digits before the point).
            "decimal" => {
                let call = self.pending_call.take();
                if let Some(t) = one(self) {
                    let mut throws = false;
                    for m in self.flat_members(t) {
                        if m == ERROR || m == DECIMAL || m == BIGINT || self.types.is_numeric(m) {
                            continue;
                        }
                        if self.types.is_string(m) {
                            throws = true;
                        } else {
                            let s = self.ast().expr(args[0].expr).span;
                            let msg = format!("`decimal` converts numbers, bigints and strings, found `{}`", self.show(t));
                            self.report(Diagnostic::new("T0001", s, msg));
                            throws = false;
                            break;
                        }
                    }
                    if throws {
                        for cls in ["RangeError", "SyntaxError"] {
                            let c = self.builtin_class(cls);
                            if c != ERROR {
                                self.on_throw(c, span, Some("decimal"), call);
                            }
                        }
                    }
                }
                DECIMAL
            }
            "Number" | "parseFloat" | "parseInt" => {
                let ret = if name == "parseInt" { INT } else { F64 };
                if args.is_empty() || args.len() > 1 + usize::from(name == "parseInt") {
                    self.report(Diagnostic::new("T0201", span, format!("`{name}` takes 1 argument, found {}", args.len())));
                    self.check_args_loose(args);
                } else {
                    let t = self.expr(args[0].expr, Some(STR));
                    // `Number(x)` of a bigint or a decimal: the nearest number (never fails)
                    if name == "Number" && (t == BIGINT || t == DECIMAL) {
                        return Some(F64);
                    }
                    // `Number(x)` also takes numbers (and `number | string`), as in JavaScript.
                    let number_ok = name == "Number" && self.flat_members(t).iter().all(|&m| self.types.is_string(m) || self.types.is_numeric(m));
                    if t != ERROR && !self.types.is_string(t) && !number_ok {
                        let msg = format!("`{name}` parses a `string`, found `{}`", self.show(t));
                        let s = self.ast().expr(args[0].expr).span;
                        let mut d = Diagnostic::new("T0001", s, msg);
                        if self.types.is_numeric(t) {
                            d = d.note("instead", "numbers convert with `f64(x)`, `int(x)` or `Math.trunc(x)`");
                        }
                        self.report(d);
                    }
                    if let Some(radix) = args.get(1) {
                        let t = self.expr(radix.expr, Some(INT));
                        let s = self.ast().expr(radix.expr).span;
                        self.expect_assignable(t, INT, s, None);
                    }
                }
                self.types.optional(ret)
            }
            // `setTimeout(callback, ms)` / `setInterval(...)`: a timer id for `clearTimeout`.
            "setTimeout" | "setInterval" => {
                let callback = self.types.func(Vec::new(), VOID);
                match args {
                    [cb] | [cb, _] => {
                        let t = self.expr(cb.expr, Some(callback));
                        let s = self.ast().expr(cb.expr).span;
                        self.expect_assignable(t, callback, s, Some("the callback".to_string()));
                        if let Some(ms) = args.get(1) {
                            let t = self.expr(ms.expr, Some(F64));
                            let s = self.ast().expr(ms.expr).span;
                            self.expect_assignable(t, F64, s, Some("the delay in milliseconds".to_string()));
                        }
                    }
                    _ => {
                        self.report(Diagnostic::new("T0201", span, format!("`{name}` takes a callback and a delay in milliseconds, found {} arguments", args.len())));
                        self.check_args_loose(args);
                    }
                }
                INT
            }
            "clearTimeout" | "clearInterval" => {
                let id = self.types.optional(INT);
                if let Some(t) = one(self) {
                    let s = self.ast().expr(args[0].expr).span;
                    self.expect_assignable(t, id, s, Some("a timer id".to_string()));
                }
                VOID
            }
            "queueMicrotask" => {
                let callback = self.types.func(Vec::new(), VOID);
                if let [cb] = args {
                    let t = self.expr(cb.expr, Some(callback));
                    let s = self.ast().expr(cb.expr).span;
                    self.expect_assignable(t, callback, s, Some("the callback".to_string()));
                } else {
                    self.report(Diagnostic::new("T0201", span, format!("`queueMicrotask` takes 1 callback, found {} arguments", args.len())));
                    self.check_args_loose(args);
                }
                VOID
            }
            "isNaN" => {
                if let Some(t) = one(self) {
                    let s = self.ast().expr(args[0].expr).span;
                    self.expect_assignable(t, F64, s, None);
                }
                BOOL
            }
            "test" => {
                self.check_args_loose(args);
                self.report(Diagnostic::new("P0202", callee_span, "`test(...)` is only allowed at the top level of a module"));
                VOID
            }
            _ if CONVERSIONS.contains(&name) => {
                let target = match name {
                    "int" | "i64" => INT,
                    "f64" => F64,
                    "f32" => F32,
                    "i8" => I8,
                    "i16" => I16,
                    "i32" => I32,
                    "u8" => U8,
                    "u16" => U16,
                    "u32" => U32,
                    _ => U64,
                };
                if args.len() != 1 {
                    self.report(Diagnostic::new("T0201", span, format!("`{name}(...)` takes 1 argument, found {}", args.len())));
                    self.check_args_loose(args);
                    return Some(target);
                }
                let t = self.expr(args[0].expr, Some(target));
                if t != ERROR && !self.types.is_numeric(t) && t != BIGINT && t != DECIMAL {
                    let s = self.ast().expr(args[0].expr).span;
                    let mut d = Diagnostic::new("T0001", s, format!("`{name}(...)` converts numbers, found `{}`", self.show(t)));
                    if self.types.is_string(t) {
                        d = d.note("to parse text", if self.types.is_int(target) { "`parseInt(s)`" } else { "`parseFloat(s)`" });
                    }
                    self.report(d);
                } else if self.types.is_float(t) && self.types.is_int(target) {
                    let text = self.src(self.ast().expr(args[0].expr).span).to_string();
                    self.report(
                        Diagnostic::new("T0001", span, format!("`{name}(...)` doesn't round floats"))
                            .note("instead", "choose the rounding explicitly")
                            .fix(Applicability::Maybe, format!("`Math.trunc({text})`"), span, format!("Math.trunc({text})"))
                            .fix(Applicability::Maybe, format!("`Math.round({text})`"), span, format!("Math.round({text})")),
                    );
                }
                target
            }
            _ => return None,
        };
        Some(result)
    }

    pub(super) fn expect_matcher(&mut self, subject: TyId, name: &str, name_span: Span, args: &[Arg], span: Span) -> TyId {
        match name {
            "toBe" | "toEqual" => {
                if args.len() != 1 {
                    self.report(Diagnostic::new("T0201", span, format!("`{name}` takes 1 argument, found {}", args.len())));
                    self.check_args_loose(args);
                    return VOID;
                }
                let t = self.expr(args[0].expr, Some(subject));
                if !self.comparable(t, subject) {
                    let msg = format!("expected value of type `{}` can never equal a `{}`", self.show(t), self.show(subject));
                    let s = self.ast().expr(args[0].expr).span;
                    self.report(Diagnostic::new("T0502", s, msg));
                }
            }
            "toBeCloseTo" => {
                let cs = CallSig { tparams: Vec::new(), params: vec![p(F64), opt(self, INT)], names: vec!["expected".into(), "digits".into()], rest: None, ret: VOID, throws: NEVER };
                self.call_sig(&cs, "toBeCloseTo", &[], args, None, span);
                if subject != ERROR && !self.types.is_numeric(subject) {
                    let msg = format!("`toBeCloseTo` needs a number, found `{}`", self.show(subject));
                    self.report(Diagnostic::new("T0001", name_span, msg));
                }
            }
            "toBeUndefined" | "toBeDefined" | "toBeTruthy" | "toBeFalsy" => {
                self.check_args_loose(args);
                if matches!(name, "toBeTruthy" | "toBeFalsy") {
                    let alt = if name == "toBeTruthy" { "toBe(true)" } else { "toBe(false)" };
                    self.report(Diagnostic::new("X0039", name_span, format!("`{name}` relies on truthiness")).fix(Applicability::Maybe, format!("use `{alt}`"), name_span.to(span.empty_at_end()), alt));
                } else if !self.types.has_undefined(subject) && subject != ERROR {
                    let msg = format!("`{}` is never `undefined`", self.show(subject));
                    self.report(Diagnostic::new("T0503", name_span, msg));
                }
            }
            _ => {
                self.check_args_loose(args);
                let known = ["toBe", "toEqual", "toBeCloseTo", "toBeUndefined", "toBeDefined"];
                let mut d = Diagnostic::new("T0107", name_span, format!("unknown matcher `{name}`")).note("available", known.join(", "));
                let sug = similar(name, known.iter().copied());
                if let Some(first) = sug.first() {
                    d = d.fix(Applicability::Maybe, format!("use `{first}`"), name_span, first.to_string());
                }
                self.report(d);
            }
        }
        VOID
    }
}
