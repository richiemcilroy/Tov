//! JavaScript values from npm packages (`Js`): imports, conversions both ways, property access
//! and calls through the bridge (runtime/js.h).

use super::{c_string, Gen, Val};
use crate::ast::{Arg, ExprId, ExprKind};
use crate::check::Callee;
use crate::check::NpmName;
use crate::hash::FxMap;
use crate::intern::Sym;
use crate::source::Span;
use crate::types::*;
use std::fmt::Write;

/// Per-program state for JavaScript values.
#[derive(Default)]
pub(crate) struct JsGen {
    /// The program imports npm packages (it embeds a bundle and links the bridge).
    pub used: bool,
    /// npm import getters by (module, name).
    imports: FxMap<(u32, NpmName), String>,
    /// Property name constants by name.
    keys: FxMap<String, String>,
    /// Conversion helpers by (type, direction).
    convs: FxMap<(TyId, bool), String>,
    /// `tvg_err_to_js` has been emitted.
    err_to_js: bool,
    /// String literal constants by text.
    lits: FxMap<String, String>,
}

impl<'c, 'a> Gen<'c, 'a> {
    pub(crate) fn js_used(&mut self) {
        if !self.js.used {
            self.js.used = true;
            let make = self.js_error_factory();
            let _ = writeln!(
                self.helpers,
                "static void *tvg_js_make_error(JSValueRef exc) {{ tv_str n, m; tv_js_error_parts(exc, &n, &m); void *e = {make}(n, m, exc); tv_str_release(n); tv_str_release(m); return e; }}"
            );
        }
    }

    /// The C name of the prelude's `__jsError(name, message, value)`.
    fn js_error_factory(&mut self) -> String {
        self.prelude_fn("__jsError")
    }

    /// The C name of a (non-generic) prelude function.
    fn prelude_fn(&mut self, name: &str) -> String {
        let b = self.c.modules.iter().position(|m| m.builtin).expect("prelude") as u32;
        let ast = &self.c.modules[b as usize].ast;
        let item = ast
            .items
            .iter()
            .position(|it| matches!(&it.kind, crate::ast::ItemKind::Function(f) if self.c.interner.get(f.name) == name))
            .unwrap_or_else(|| panic!("{name} in the prelude")) as u32;
        self.instance(b, item, FxMap::default())
    }

    /// `tvg_err_to_js(e)`: the JavaScript exception for an error a Tov closure passes on to
    /// JavaScript (what JavaScript threw, for a `JsError`); takes the error.
    fn err_to_js_fn(&mut self) {
        if self.js.err_to_js {
            return;
        }
        self.js.err_to_js = true;
        let (is_js, value, name, message) = (self.prelude_fn("__isJsError"), self.prelude_fn("__jsErrorValue"), self.prelude_fn("__errorName"), self.prelude_fn("__errorMessage"));
        let err = self.c.error_class();
        let ct = self.ctype(err);
        // (`v` stays alive on the stack, where the collector scans, until JavaScriptCore takes it)
        self.helpers_after.push(format!(
            "static JSValueRef tvg_err_to_js(void *e) {{\n    JSValueRef v;\n    if ({is_js}(({ct})e)) {{\n        v = {value}(({ct})e);\n        tv_js_release(v);\n    }} else {{\n        tv_str n = {name}(({ct})e), m = {message}(({ct})e);\n        v = tv_js_error_value(n, m);\n        tv_str_release(n);\n        tv_str_release(m);\n    }}\n    tvg_obj_release(e);\n    return v;\n}}\n"
        ));
        let _ = writeln!(self.protos, "static JSValueRef tvg_err_to_js(void *e);");
        // (a rejected promise's error: it stays the promise's)
        self.helpers_after.push("static JSValueRef tvg_err_to_js_kept(void *e) { tvg_obj_retain(e); return tvg_err_to_js(e); }\n".to_string());
        let _ = writeln!(self.protos, "static JSValueRef tvg_err_to_js_kept(void *e);");
    }

    /// A property name constant (`&kN`, a `tv_js_name`).
    pub(crate) fn js_key(&mut self, name: &str) -> String {
        let k = match self.js.keys.get(name) {
            Some(k) => k.clone(),
            None => {
                let k = format!("bmk{}", self.js.keys.len());
                let _ = writeln!(self.lits, "static tv_js_name {k} = {{{}}};", c_string(name.as_bytes()));
                self.js.keys.insert(name.to_string(), k.clone());
                k
            }
        };
        format!("&{k}")
    }

    /// The value an npm import names (read once, then kept).
    pub(crate) fn npm_value(&mut self, m: u32, name: NpmName, span: Span, ty: TyId) -> Val {
        self.js_used();
        let f = match self.js.imports.get(&(m, name)) {
            Some(f) => f.clone(),
            None => {
                let f = format!("tvg_npm{}", self.js.imports.len());
                let spec = self.c.modules[m as usize].npm.clone().unwrap_or_default();
                let loc = self.loc(span);
                let spec_c = c_string(spec.as_bytes());
                let body = match name {
                    NpmName::Ns => format!("tv_js_import_as({spec_c}, 0, {loc})"),
                    NpmName::Default => format!("tv_js_import_as({spec_c}, 1, {loc})"),
                    NpmName::Named(s) => {
                        let text = self.sym(s).to_string();
                        let key = self.js_key(&text);
                        format!("tv_js_get_or_trap(tv_js_import_as({spec_c}, 2, {loc}), {key}, {loc})")
                    }
                };
                let _ = writeln!(self.helpers, "static JSValueRef {f}(void) {{ static JSValueRef v; if (!v) v = tv_js_retain({body}); return v; }}");
                self.js.imports.insert((m, name), f.clone());
                f
            }
        };
        // (a variable, not the call: printing takes the value's address)
        let t = self.fresh("j");
        self.line(format!("JSValueRef {t} = {f}();"));
        self.coerce(Val::plain(t, JS), ty)
    }

    // ------------------------------------------------------------ Tov → JavaScript

    /// A Tov value as a JavaScript value (borrowed: kept alive by the stack, not protected).
    pub(crate) fn js_of(&mut self, v: Val) -> Val {
        self.js_used();
        let from = v.ty;
        let code = match self.tget(from) {
            Ty::Js => return Val { ty: JS, ..v },
            Ty::Bool => format!("tv_js_bool({})", v.code),
            Ty::Int | Ty::F64 | Ty::F32 | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64 => format!("tv_js_num((double)({}))", v.code),
            Ty::Str => format!("tv_js_from_str({})", v.code),
            Ty::StrLit(s) => {
                let text = self.sym(s).to_string();
                self.js_lit(&text)
            }
            Ty::Undefined | Ty::Void | Ty::Never | Ty::Error | Ty::Unknown => "tv_js_undefined()".to_string(),
            _ => {
                let f = self.js_conv(from, true);
                format!("{f}({})", v.code)
            }
        };
        let t = self.fresh("j");
        self.line(format!("JSValueRef {t} = {code};"));
        Val::plain(t, JS)
    }

    fn js_lit(&mut self, text: &str) -> String {
        let n = match self.js.lits.get(text) {
            Some(n) => n.clone(),
            None => {
                let n = format!("bmjl{}", self.js.lits.len());
                let _ = writeln!(self.lits, "static JSValueRef {n};");
                self.js.lits.insert(text.to_string(), n.clone());
                n
            }
        };
        format!("tv_js_lit(&{n}, {}, {})", c_string(text.as_bytes()), text.len())
    }

    /// The conversion helper for `t`: to JavaScript (`JSValueRef f(T v)`, `v` borrowed) or from
    /// it (`bool f(JSValueRef v, T *out)`, `*out` owned; false if the value doesn't fit).
    fn js_conv(&mut self, t: TyId, to: bool) -> String {
        if let Some(f) = self.js.convs.get(&(t, to)) {
            return f.clone();
        }
        let f = format!("tvg_{}js{}", if to { "to" } else { "from" }, self.js.convs.len());
        self.js.convs.insert((t, to), f.clone());
        let ct = self.ctype(t);
        if to {
            let _ = writeln!(self.protos, "static JSValueRef {f}({ct} v);");
        } else {
            let _ = writeln!(self.protos, "static bool {f}(JSValueRef v, {ct} *out);");
        }
        let body = if to { self.js_to_body(t) } else { self.js_from_body(t) };
        if to {
            let _ = writeln!(self.helpers, "static JSValueRef {f}({ct} v) {{\n{body}}}");
        } else {
            let _ = writeln!(self.helpers, "static bool {f}(JSValueRef v, {ct} *out) {{\n{body}}}");
        }
        f
    }

    /// An expression converting C value `code` of type `t` to a JSValueRef.
    fn js_to_code(&mut self, t: TyId, code: &str) -> String {
        match self.tget(t) {
            Ty::Js => code.to_string(),
            Ty::Bool => format!("tv_js_bool({code})"),
            Ty::Int | Ty::F64 | Ty::F32 | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64 => format!("tv_js_num((double)({code}))"),
            Ty::Str => format!("tv_js_from_str({code})"),
            Ty::StrLit(s) => {
                let text = self.sym(s).to_string();
                self.js_lit(&text)
            }
            Ty::Undefined | Ty::Void | Ty::Never | Ty::Error | Ty::Unknown => "tv_js_undefined()".to_string(),
            _ => {
                let f = self.js_conv(t, true);
                format!("{f}({code})")
            }
        }
    }

    fn js_to_body(&mut self, t: TyId) -> String {
        let mut b = String::new();
        match self.tget(t) {
            Ty::Array(e) => {
                let ect = self.ctype(e);
                let conv = self.js_to_code(e, "xs[i]");
                let _ = writeln!(b, "    tv_int n = tv_arr_len(v); const {ect} *xs = (const {ect} *)tv_arr_data(v);");
                let _ = writeln!(b, "    JSValueRef small[16]; JSValueRef *items = n <= 16 ? small : tv_alloc((size_t)n * sizeof(JSValueRef));");
                let _ = writeln!(b, "    for (tv_int i = 0; i < n; i++) items[i] = {conv};");
                let _ = writeln!(b, "    JSValueRef r = tv_js_array((size_t)n, items);\n    if (items != small) tv_free(items);\n    return r;");
            }
            Ty::Record(fs) => {
                let _ = fs;
                // one engine call: a compiled (a0, ...) => ({k0: a0, ...}) per record type
                let fields = self.c.types.fields_in_order(t);
                let n = self.fresh("bmgs");
                let keys: Vec<String> = fields.iter().map(|f| c_string(self.sym(f.name).as_bytes())).collect();
                let opts: Vec<&str> = fields.iter().map(|f| if self.c.types.has_undefined(f.ty) { "true" } else { "false" }).collect();
                let _ = writeln!(self.lits, "static const char *const {n}k[] = {{{}}};\nstatic const bool {n}o[] = {{{}}};\nstatic tv_js_shape {n} = {{{}, {n}k, {n}o, NULL}};", keys.join(", "), opts.join(", "), fields.len());
                let mut vals = Vec::new();
                for f in &fields {
                    let name = self.sym(f.name).to_string();
                    vals.push(self.js_to_code(f.ty, &format!("v.f_{name}")));
                }
                if vals.is_empty() {
                    b.push_str("    return tv_js_object();\n");
                } else {
                    let _ = writeln!(b, "    JSValueRef vals[{}] = {{{}}};\n    return tv_js_shape_make(&{n}, vals);", vals.len(), vals.join(", "));
                }
            }
            Ty::Union(_) => {
                let ms = self.members(t);
                let tag = self.u_tag("v", t);
                let _ = writeln!(b, "    switch ({tag}) {{");
                for (k, &m) in ms.iter().enumerate() {
                    let payload = self.u_payload("v", t, k);
                    let conv = if self.is_unit(m) { self.js_to_code(m, "0") } else { self.js_to_code(m, &payload) };
                    let _ = writeln!(b, "    case {k}: return {conv};");
                }
                b.push_str("    default: return tv_js_undefined();\n    }\n");
            }
            Ty::Promise(pv, _) => {
                // settled from Tov's side (tv_js_promise_of): the value converted when it's there
                self.err_to_js_fn();
                let vf = self.fresh("tvg_jspv");
                let conv = if pv == VOID || pv == UNDEFINED || self.is_unit(pv) {
                    "tv_js_undefined()".to_string()
                } else {
                    let vct = self.ctype(pv);
                    self.js_to_code(pv, &format!("(*(const {vct} *)x)"))
                };
                let _ = writeln!(self.helpers, "static JSValueRef {vf}(const void *x) {{\n    (void)x;\n    return {conv};\n}}");
                let _ = writeln!(b, "    return tv_js_promise_of(v, {vf}, tvg_err_to_js_kept);");
            }
            Ty::Func(ps, r, th) => {
                let ps = self.c.types.params(ps).to_vec();
                let tramp = self.js_tramp(&ps, r, th != NEVER);
                let _ = writeln!(b, "    return tv_js_function(v, {tramp});");
            }
            _ => b.push_str("    return tv_js_undefined();\n"),
        }
        b
    }

    /// A trampoline calling a Tov closure of this signature with JavaScript arguments. An error
    /// the closure passes on is thrown into JavaScript (`*exc`, and NULL: JavaScriptCore needs
    /// both); its result is then a default value, which holds nothing to release. So is an
    /// uncaught JavaScript exception, back through the unwind channel (`unwind.rs`) or by a jump
    /// to the catch point (`tv_js_unwind`).
    fn js_tramp(&mut self, ps: &[FnParam], ret: TyId, throws: bool) -> String {
        let f = self.fresh("tvg_jstramp");
        let fp = self.fn_ptr_type_pub(ps, ret);
        let args: Vec<String> = (0..ps.len()).map(|i| format!(", n > {i} ? a[{i}] : tv_js_undefined()")).collect();
        let call = format!("(({fp})fn->fn)(fn->env{})", args.concat());
        let unit = ret == VOID || self.is_unit(ret);
        let mut b = String::new();
        b.push_str("    (void)n; (void)a;\n");
        // (an error left over from code that dropped one is never reported as this call's own)
        b.push_str("    if (tvg_err) { tvg_obj_release(tvg_err); tvg_err = NULL; }\n");
        // the catch point for a JavaScript exception the closure doesn't catch (tv_js_unwind):
        // thrown into the caller; popped before anything else runs. (Nothing here needs
        // `volatile`: `u` lives in memory, its address taken, and no local is changed between the
        // setjmp and a jump back to it; the exception comes back in a global. Keep it so.)
        b.push_str("    tv_js_unwind u;\n    u.prev = tv_js_unwind_top;\n    if (TV_JS_SETJMP(u.jb)) {\n        tv_js_unwind_top = u.prev;\n        tv_js_direct = false;\n        *exc = tv_js_unwind_exc;\n        tv_js_unwind_exc = NULL;\n        tv_js_pending = NULL;\n        return NULL;\n    }\n    tv_js_unwind_top = &u;\n");
        // the closure is called through the channel: it takes the flag first thing (every sync
        // function of an npm program does); cleared after the call anyway, so a callee that
        // doesn't (an async one) can't leave it set: whoever sets it clears it
        if unit {
            let _ = writeln!(b, "    tv_js_direct = true;\n    {call};");
        } else {
            let rct = self.ctype(ret);
            let _ = writeln!(b, "    tv_js_direct = true;\n    {rct} r = {call};");
        }
        b.push_str("    tv_js_direct = false;\n    tv_js_unwind_top = u.prev;\n");
        b.push_str("    if (tv_js_pending) { *exc = tv_js_pending; tv_js_release(*exc); tv_js_pending = NULL; return NULL; }\n");
        if throws {
            self.err_to_js_fn();
            b.push_str("    if (tvg_err) { void *e = tvg_err; tvg_err = NULL; *exc = tvg_err_to_js(e); return NULL; }\n");
        }
        if unit {
            b.push_str("    return tv_js_undefined();\n");
        } else {
            let conv = self.js_to_code(ret, "r");
            let rel = self.release_code(ret, "r");
            let _ = writeln!(b, "    JSValueRef out = {conv};\n    {rel};\n    return out;");
        }
        let _ = writeln!(self.helpers, "static JSValueRef {f}(const tv_fn *fn, size_t n, const JSValueRef *a, JSValueRef *exc) {{\n{b}}}");
        f
    }

    // ------------------------------------------------------------ JavaScript → Tov

    /// `x as T` for a JavaScript `x`: converted, or a trap if it doesn't fit.
    pub(crate) fn tov_of_js(&mut self, v: Val, to: TyId, span: Span) -> Val {
        self.js_used();
        if self.tget(to) == Ty::Js {
            return v;
        }
        let f = self.js_conv(to, false);
        let ct = self.ctype(to);
        let r = self.fresh("r");
        let loc = self.loc(span);
        let want = c_string(self.c.show(to).as_bytes());
        self.line(format!("{ct} {r}; if (!{f}({}, &{r})) tv_js_type_trap({}, {want}, {loc});", v.code, v.code));
        let owned = self.is_rc(to);
        if owned {
            self.b().temps.last_mut().unwrap().push((r.clone(), to));
        }
        Val { code: r, ty: to, owned }
    }

    /// A statement block converting JSValueRef `src` into `*dst` of type `t` (owned); on failure
    /// it returns false from the helper.
    fn js_from_stmt(&mut self, t: TyId, src: &str, dst: &str) -> String {
        match self.tget(t) {
            Ty::Js => format!("{dst} = tv_js_retain({src});"),
            Ty::Str => format!("if (!tv_js_as_str({src}, &{dst})) return false;"),
            Ty::StrLit(s) => {
                let text = self.sym(s).to_string();
                let lit = c_string(text.as_bytes());
                format!("{{ tv_str s; if (!tv_js_as_str({src}, &s)) return false; bool ok = s.p->len == {} && memcmp(s.p->data, {lit}, {}) == 0; tv_str_release(s); if (!ok) return false; {dst} = 0; }}", text.len(), text.len())
            }
            Ty::Bool => format!("if (!tv_js_as_bool({src}, &{dst})) return false;"),
            Ty::F64 => format!("if (!tv_js_as_num({src}, &{dst})) return false;"),
            Ty::F32 => format!("{{ double d; if (!tv_js_as_num({src}, &d)) return false; {dst} = (float)d; }}"),
            Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64 => {
                let ct = self.ctype(t);
                let (lo, hi) = match self.tget(t) {
                    Ty::I8 => ("-128.0", "127.0"),
                    Ty::I16 => ("-32768.0", "32767.0"),
                    Ty::I32 => ("-2147483648.0", "2147483647.0"),
                    Ty::U8 => ("0.0", "255.0"),
                    Ty::U16 => ("0.0", "65535.0"),
                    Ty::U32 => ("0.0", "4294967295.0"),
                    Ty::U64 => ("0.0", "18446744073709549568.0"),
                    _ => ("-9223372036854775808.0", "9223372036854774784.0"),
                };
                format!("{{ double d; if (!tv_js_as_num({src}, &d) || d != (double)(int64_t)d || d < {lo} || d > {hi}) return false; {dst} = ({ct})d; }}")
            }
            Ty::Undefined | Ty::Void => format!("if (!tv_js_is_nullish({src})) return false; {dst} = 0;"),
            _ => {
                let f = self.js_conv(t, false);
                format!("if (!{f}({src}, &{dst})) return false;")
            }
        }
    }

    fn js_from_body(&mut self, t: TyId) -> String {
        let mut b = String::new();
        match self.tget(t) {
            Ty::Array(e) => {
                let ect = self.ctype(e);
                let d = self.desc(e);
                let conv = self.js_from_stmt(e, "x", "w[i]");
                let rel = self.release_code(t, "a");
                let _ = writeln!(b, "    if (!tv_js_is_array(v)) return false;");
                let _ = writeln!(b, "    uint32_t n = tv_js_length(v); tv_arr a = TV_EMPTY_ARR;");
                let _ = writeln!(b, "    {ect} *w = ({ect} *)tv_arr_reserve_tail(&a, {d}, n);");
                // a failing element: release what was converted (the length counts them)
                let conv = conv.replace("return false;", &format!("{{ {rel}; return false; }}"));
                let _ = writeln!(b, "    for (uint32_t i = 0; i < n; i++) {{ JSValueRef exc = NULL; JSValueRef x = tv_js_index(v, i, &exc); if (!x) {{ {rel}; return false; }} {conv} a.len = i + 1; }}");
                let _ = writeln!(b, "    a.len = n; *out = a; return true;");
            }
            Ty::Record(fs) => {
                let fs = self.c.types.fields(fs).to_vec();
                let ct = self.ctype(t);
                let _ = writeln!(b, "    if (tv_js_is_nullish(v) || strcmp(tv_js_typeof(v), \"object\") != 0) return false;");
                let _ = writeln!(b, "    {ct} r;");
                // fields convert in order; on a failure, the ones done are released
                for (i, f) in fs.iter().enumerate() {
                    let name = self.sym(f.name).to_string();
                    let key = self.js_key(&name);
                    let mut undo = String::new();
                    for g in &fs[..i] {
                        let gn = self.sym(g.name).to_string();
                        let rl = self.release_code(g.ty, &format!("r.f_{gn}"));
                        let _ = write!(undo, "{rl}; ");
                    }
                    let conv = self.js_from_stmt(f.ty, "x", &format!("r.f_{name}"));
                    let conv = conv.replace("return false;", &format!("{{ {undo}return false; }}"));
                    let _ = writeln!(b, "    {{ JSValueRef exc = NULL; JSValueRef x = tv_js_name_get(v, {key}, &exc); if (!x) {{ {undo}return false; }} {conv} }}");
                }
                b.push_str("    *out = r; return true;\n");
            }
            Ty::Union(_) => {
                let ms = self.members(t);
                // `undefined` first (a missing value), then the members in order
                let mut order: Vec<usize> = (0..ms.len()).collect();
                order.sort_by_key(|&k| ms[k] != UNDEFINED);
                for k in order {
                    let m = ms[k];
                    if self.is_unit(m) {
                        if m == UNDEFINED {
                            let u = self.u_make(t, k, None);
                            let _ = writeln!(b, "    if (tv_js_is_nullish(v)) {{ *out = {u}; return true; }}");
                        } else {
                            let f = self.js_conv(m, false);
                            let u = self.u_make(t, k, None);
                            let mct = self.ctype(m);
                            let _ = writeln!(b, "    {{ {mct} x; if ({f}(v, &x)) {{ *out = {u}; return true; }} }}");
                        }
                    } else {
                        let mct = self.ctype(m);
                        let f = self.js_conv(m, false);
                        let u = self.u_make(t, k, Some("x"));
                        let _ = writeln!(b, "    {{ {mct} x; if ({f}(v, &x)) {{ *out = {u}; return true; }} }}");
                    }
                }
                b.push_str("    return false;\n");
            }
            _ => {
                let conv = self.js_from_stmt(t, "v", "*out");
                let _ = writeln!(b, "    {conv}\n    return true;");
            }
        }
        b
    }

    // ------------------------------------------------------------ access and calls

    /// A JavaScript read marked `try`: it passes a `JsError` on (`js_read_check` follows it).
    fn js_read_marked(&mut self, e: ExprId) -> bool {
        let m = self.cur_m();
        // (in an async arrow passed to JavaScript, every read: its promise rejects)
        self.b().root || self.facts(m).throwing.contains(&e)
    }

    /// After a JavaScript read marked `try`: its error, if it failed.
    pub(crate) fn js_read_check(&mut self, e: ExprId) {
        if self.js_read_marked(e) {
            self.error_check();
        }
    }

    /// A JavaScript read into `t`: `get` fails with NULL and `x`, the exception. Marked `try`,
    /// its error is passed on; unmarked, it's uncaught (`js_uncaught`). `guard`: `obj?.`'s object.
    fn js_read_into(&mut self, e: ExprId, t: &str, x: &str, guard: Option<&str>, get: String, loc: &str) {
        let pre = guard.map(|o| format!("tv_js_is_nullish({o}) ? tv_js_undefined() : ")).unwrap_or_default();
        self.line(format!("JSValueRef {x} = NULL; JSValueRef {t} = {pre}{get};"));
        if self.js_read_marked(e) {
            self.js_used();
            self.line(format!("if (!{t}) {{ tvg_err = tvg_js_make_error({x}); {t} = tv_js_undefined(); }}"));
        } else {
            self.js_uncaught(t, x, loc);
        }
    }

    /// `if (!r)`: an uncaught JavaScript exception `x`. This frame's values are released first.
    /// Called from JavaScript, the exception goes back there: through the unwind channel when
    /// this frame is on it (`unwind.rs`), else by a jump to the trampoline (`tv_js_unwind`).
    /// Anywhere else it's a trap.
    pub(crate) fn js_uncaught(&mut self, r: &str, x: &str, loc: &str) {
        if self.b().root {
            self.js_used();
            self.line(format!("if (!{r}) {{ tvg_err = tvg_js_make_error({x}); {r} = tv_js_undefined(); }}"));
            self.error_check();
            return;
        }
        self.open(&format!("if (__builtin_expect(!{r}, 0)) {{"));
        self.release_all_temps();
        self.unwind_to(0);
        if self.b().aware {
            // (`aw_` only when something up the channel catches it: the trampoline, or an async
            // arrow passed to JavaScript)
            self.open("if (aw_) {");
            self.line(format!("tv_js_pending = (void *)tv_js_retain({x});"));
            self.channel_return();
            self.close("}");
        }
        self.line(format!("tv_js_throw_trap({x}, {loc});"));
        self.close("}");
    }

    /// `obj.name` on a JavaScript value.
    pub(crate) fn js_member(&mut self, e: ExprId, obj: Val, name: Sym, optional: bool, ty: TyId, span: Span) -> Val {
        let text = self.sym(name).to_string();
        let key = self.js_key(&text);
        let loc = self.loc(span);
        let t = self.fresh("j");
        let x = format!("{t}x");
        let guard = optional.then_some(obj.code.as_str());
        self.js_read_into(e, &t, &x, guard, format!("tv_js_name_get({}, {key}, &{x})", obj.code), &loc);
        self.coerce(Val::plain(t, JS), ty)
    }

    /// `obj[index]` on a JavaScript value.
    pub(crate) fn js_index(&mut self, e: ExprId, obj: Val, index: Val, optional: bool, ty: TyId, span: Span) -> Val {
        let loc = self.loc(span);
        let t = self.fresh("j");
        let x = format!("{t}x");
        let get = match self.tget(index.ty) {
            Ty::Str | Ty::StrLit(_) => {
                let iv = self.coerce(index, STR);
                format!("tv_js_key_exc({}, {}, &{x})", obj.code, iv.code)
            }
            Ty::Js => format!("tv_js_key_exc({o}, tv_js_to_str({i}), &{x})", o = obj.code, i = index.code),
            _ => format!("tv_js_at_exc({}, (double)({}), &{x})", obj.code, index.code),
        };
        let guard = optional.then_some(obj.code.as_str());
        self.js_read_into(e, &t, &x, guard, get, &loc);
        self.coerce(Val::plain(t, JS), ty)
    }

    /// A call on a JavaScript value (`method`: `obj.method(...)`), or `new`.
    pub(crate) fn js_call(&mut self, callee_obj: Val, method: Option<Sym>, args: &[Arg], new: bool, ty: TyId) -> Val {
        let mut vals = Vec::with_capacity(args.len());
        for a in args {
            let v = self.expr(a.expr);
            let j = self.js_of(v);
            vals.push(j.code);
        }
        // (a compound literal, not an array variable: async functions keep their locals in a frame)
        let argv = if vals.is_empty() { "NULL".to_string() } else { format!("(JSValueRef[]){{{}}}", vals.join(", ")) };
        let (x, r) = (self.fresh("jx"), self.fresh("jr"));
        let call = match (new, method) {
            (true, _) => format!("tv_js_new({}, {}, {argv}, &{x})", callee_obj.code, vals.len()),
            (false, Some(m)) => {
                let text = self.sym(m).to_string();
                let key = self.js_key(&text);
                format!("tv_js_name_call({}, {key}, {}, {argv}, &{x})", callee_obj.code, vals.len())
            }
            (false, None) => format!("tv_js_call({}, NULL, {}, {argv}, &{x})", callee_obj.code, vals.len()),
        };
        self.line(format!("JSValueRef {x} = NULL; JSValueRef {r} = {call};"));
        self.line(format!("if (!{r}) {{ tvg_err = tvg_js_make_error({x}); {r} = tv_js_undefined(); }}"));
        self.coerce(Val::plain(r, JS), ty)
    }

    /// The receiver of a JavaScript method call, or the function itself.
    pub(crate) fn js_callee(&mut self, e: ExprId) -> Val {
        let v = self.expr(e);
        self.js_of(v)
    }
}

// ------------------------------------------------------------ assignment and iteration

impl<'c, 'a> Gen<'c, 'a> {
    /// `obj.name op= value` / `obj[key] op= value` on a JavaScript object: one engine call
    /// (`(o, v) => (o.name op= v)`), with JavaScript's semantics for the operator.
    pub(crate) fn js_assign(&mut self, op: &str, target: ExprId, value: ExprId, ty: TyId, span: Span) -> Val {
        self.js_used();
        let m = self.cur_m();
        let mut f = Fused::default();
        f.src.push_str("return (");
        match &self.ast(m).expr(target).kind {
            ExprKind::Member { obj, name, .. } => {
                let (obj, name) = (*obj, *name);
                self.js_fuse_operand(obj, &mut f);
                let text = self.sym(name).to_string();
                js_access(&mut f.src, &text, false);
            }
            ExprKind::Index { obj, index, .. } => {
                let (obj, index) = (*obj, *index);
                self.js_fuse_operand(obj, &mut f);
                f.src.push('[');
                self.js_fuse(index, &mut f);
                f.src.push(']');
            }
            _ => unreachable!("a JavaScript assignment target"),
        }
        let _ = write!(f.src, " {op} ");
        self.js_fuse(value, &mut f);
        f.src.push(')');
        let slot = self.fresh("bmgjf");
        let _ = writeln!(self.lits, "static JSObjectRef {slot};");
        let argv = if f.params.is_empty() { "NULL".to_string() } else { format!("(JSValueRef[]){{{}}}", f.params.join(", ")) };
        let (x, r) = (self.fresh("jx"), self.fresh("jr"));
        let src = c_string(f.src.as_bytes());
        let loc = self.loc(span);
        self.line(format!("JSValueRef {x} = NULL; JSValueRef {r} = tv_js_thunk(&{slot}, {src}, {}, {argv}, &{x});", f.params.len()));
        self.js_uncaught(&r, &x, &loc);
        self.coerce(Val::plain(r, JS), ty)
    }

    /// A JavaScript iterable as an array to index (`Array.from` unless it is one); held until
    /// the enclosing scope ends.
    pub(crate) fn js_array_of(&mut self, v: &str, span: Span) -> String {
        self.js_used();
        let slot = self.fresh("bmgjf");
        let _ = writeln!(self.lits, "static JSObjectRef {slot};");
        let (x, a) = (self.fresh("jx"), self.fresh("ja"));
        let loc = self.loc(span);
        self.line(format!(
            "JSValueRef {x} = NULL; JSValueRef {a} = tv_js_thunk(&{slot}, \"return Array.isArray(a0) ? a0 : Array.from(a0)\", 1, (JSValueRef[]){{{v}}}, &{x});"
        ));
        self.js_uncaught(&a, &x, &loc);
        self.line(format!("tv_js_retain({a});"));
        self.b().scopes.last_mut().unwrap().releases.push(format!("tv_js_release({a});"));
        a
    }
}

// ------------------------------------------------------------ fusion
//
// A JavaScript expression tree in Tov code (`User.safeParse({ name: n, age: a }).success`) is
// compiled into one JavaScript function per site, called once: each engine call costs ~40 ns,
// the work inside it (with the engine's own caches and JIT) much less. Tov values in the tree
// (variables, other expressions) are evaluated first and passed in; literals are inlined.

#[derive(Default)]
struct Fused {
    src: String,
    params: Vec<String>,
    calls: bool,
}

impl<'c, 'a> Gen<'c, 'a> {
    /// `e`, a JavaScript operation, as one fused call if its tree holds two or more operations.
    pub(crate) fn js_try_fuse(&mut self, e: ExprId, ty: TyId, span: Span) -> Option<Val> {
        if self.js_ops(e) < 2 {
            return None;
        }
        self.js_used();
        let mut f = Fused::default();
        f.src.push_str("return ");
        self.js_fuse(e, &mut f);
        let slot = self.fresh("bmgjf");
        let _ = writeln!(self.lits, "static JSObjectRef {slot};");
        let argv = if f.params.is_empty() { "NULL".to_string() } else { format!("(JSValueRef[]){{{}}}", f.params.join(", ")) };
        let (x, r) = (self.fresh("jx"), self.fresh("jr"));
        let src = c_string(f.src.as_bytes());
        self.line(format!("JSValueRef {x} = NULL; JSValueRef {r} = tv_js_thunk(&{slot}, {src}, {}, {argv}, &{x});", f.params.len()));
        // (calls, and reads marked `try`, pass errors on; the caller checks)
        if f.calls || self.js_read_marked(e) {
            self.line(format!("if (!{r}) {{ tvg_err = tvg_js_make_error({x}); {r} = tv_js_undefined(); }}"));
        } else {
            let loc = self.loc(span);
            self.js_uncaught(&r, &x, &loc);
        }
        Some(self.coerce(Val::plain(r, JS), ty))
    }

    fn js_ty(&mut self, e: ExprId) -> TyId {
        let t = self.ty(e);
        self.c.types.without_undefined(t)
    }

    /// JavaScript operations (property reads, calls, `new`) in the fusable tree under `e`.
    fn js_ops(&mut self, e: ExprId) -> usize {
        let m = self.cur_m();
        let ast = self.ast(m);
        match &ast.expr(e).kind {
            ExprKind::Paren(x) | ExprKind::Try(x) => self.js_ops(*x),
            ExprKind::Member { obj, .. } if self.js_ty(*obj) == JS => 1 + self.js_ops(*obj),
            ExprKind::Index { obj, index, .. } if self.js_ty(*obj) == JS => 1 + self.js_ops(*obj) + self.js_ops(*index),
            ExprKind::Call { callee, args, .. } => match self.facts(m).calls.get(&e).map(|c| &c.callee) {
                Some(Callee::Js { method }) => {
                    let target = match (&ast.expr(*callee).kind, method) {
                        (ExprKind::Member { obj, .. }, Some(_)) => *obj,
                        _ => *callee,
                    };
                    1 + self.js_ops(target) + args.iter().map(|a| self.js_ops(a.expr)).sum::<usize>()
                }
                _ => 0,
            },
            ExprKind::New { callee, args, .. } => match self.facts(m).calls.get(&e).map(|c| &c.callee) {
                Some(Callee::JsNew) => 1 + self.js_ops(*callee) + args.iter().map(|a| self.js_ops(a.expr)).sum::<usize>(),
                _ => 0,
            },
            ExprKind::Object(fields) => {
                let t = self.ty(e);
                // (converting a literal costs an engine call too)
                if matches!(self.tget(t), Ty::Record(_)) { 1 + fields.iter().map(|f| self.js_ops(f.value)).sum::<usize>() } else { 0 }
            }
            ExprKind::Array(elems) => 1 + elems.iter().map(|&x| self.js_ops(x)).sum::<usize>(),
            _ => 0,
        }
    }

    fn js_fuse(&mut self, e: ExprId, f: &mut Fused) {
        let m = self.cur_m();
        let ast = self.ast(m);
        match &ast.expr(e).kind {
            ExprKind::Paren(x) | ExprKind::Try(x) => self.js_fuse(*x, f),
            ExprKind::Member { obj, name, optional, .. } if self.js_ty(*obj) == JS => {
                let (obj, name, optional) = (*obj, *name, *optional);
                self.js_fuse_operand(obj, f);
                let text = self.sym(name).to_string();
                js_access(&mut f.src, &text, optional);
            }
            ExprKind::Index { obj, index, optional } if self.js_ty(*obj) == JS => {
                let (obj, index, optional) = (*obj, *index, *optional);
                self.js_fuse_operand(obj, f);
                f.src.push_str(if optional { "?.[" } else { "[" });
                self.js_fuse(index, f);
                f.src.push(']');
            }
            ExprKind::Call { callee, args, optional, .. } if matches!(self.facts(m).calls.get(&e).map(|c| &c.callee), Some(Callee::Js { .. })) => {
                let Some(Callee::Js { method }) = self.facts(m).calls.get(&e).map(|c| c.callee.clone()) else { unreachable!() };
                let (callee, optional) = (*callee, *optional);
                let args: Vec<ExprId> = args.iter().map(|a| a.expr).collect();
                f.calls = true;
                match (&self.ast(m).expr(callee).kind, method) {
                    (ExprKind::Member { obj, name, optional: mopt, .. }, Some(_)) => {
                        let (obj, name, mopt) = (*obj, *name, *mopt);
                        self.js_fuse_operand(obj, f);
                        let text = self.sym(name).to_string();
                        js_access(&mut f.src, &text, mopt);
                    }
                    _ => self.js_fuse_operand(callee, f),
                }
                f.src.push_str(if optional { "?.(" } else { "(" });
                for (i, a) in args.into_iter().enumerate() {
                    if i > 0 {
                        f.src.push_str(", ");
                    }
                    self.js_fuse(a, f);
                }
                f.src.push(')');
            }
            ExprKind::New { callee, args, .. } if matches!(self.facts(m).calls.get(&e).map(|c| &c.callee), Some(Callee::JsNew)) => {
                let callee = *callee;
                let args: Vec<ExprId> = args.iter().map(|a| a.expr).collect();
                f.calls = true;
                f.src.push_str("new (");
                self.js_fuse(callee, f);
                f.src.push_str(")(");
                for (i, a) in args.into_iter().enumerate() {
                    if i > 0 {
                        f.src.push_str(", ");
                    }
                    self.js_fuse(a, f);
                }
                f.src.push(')');
            }
            ExprKind::Object(fields) if { let t = self.ty(e); matches!(self.tget(t), Ty::Record(_)) } => {
                let fields: Vec<(Sym, ExprId)> = fields.iter().map(|x| (x.name, x.value)).collect();
                f.src.push('{');
                for (i, (name, value)) in fields.into_iter().enumerate() {
                    if i > 0 {
                        f.src.push_str(", ");
                    }
                    let key = crate::npm::bundle::js_string(self.sym(name));
                    f.src.push_str(&key);
                    f.src.push_str(": ");
                    self.js_fuse(value, f);
                }
                f.src.push('}');
            }
            ExprKind::Array(elems) => {
                let elems = elems.clone();
                f.src.push('[');
                for (i, x) in elems.into_iter().enumerate() {
                    if i > 0 {
                        f.src.push_str(", ");
                    }
                    self.js_fuse(x, f);
                }
                f.src.push(']');
            }
            ExprKind::Str(s) if { let t = self.ty(e); matches!(self.tget(t), Ty::Str | Ty::StrLit(_) | Ty::Js) } => {
                let text = crate::npm::bundle::js_string(self.sym(*s));
                f.src.push_str(&text);
            }
            ExprKind::Bool(b) => f.src.push_str(if *b { "true" } else { "false" }),
            ExprKind::Undefined => f.src.push_str("undefined"),
            ExprKind::Int(v) if *v < (1u64 << 53) => {
                let _ = write!(f.src, "{v}");
            }
            ExprKind::Float(x) if x.is_finite() => {
                let _ = write!(f.src, "{x:?}");
            }
            _ => self.js_fuse_leaf(e, f),
        }
    }

    /// An operand that is itself an expression: parenthesized unless it's a parameter.
    fn js_fuse_operand(&mut self, e: ExprId, f: &mut Fused) {
        if self.js_ops(e) == 0 {
            self.js_fuse_leaf(e, f);
        } else {
            f.src.push('(');
            self.js_fuse(e, f);
            f.src.push(')');
        }
    }

    /// A Tov value, evaluated here and passed in.
    fn js_fuse_leaf(&mut self, e: ExprId, f: &mut Fused) {
        let v = self.expr(e);
        let j = self.js_of(v);
        let _ = write!(f.src, "a{}", f.params.len());
        f.params.push(j.code);
    }
}

/// `.name`, `?.name`, or `["name"]` / `?.["name"]` for names that aren't identifiers.
fn js_access(out: &mut String, name: &str, optional: bool) {
    let ident = name.chars().next().is_some_and(|c| c.is_ascii_alphabetic() || c == '_' || c == '$') && name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '$');
    match (ident, optional) {
        (true, false) => {
            out.push('.');
            out.push_str(name);
        }
        (true, true) => {
            out.push_str("?.");
            out.push_str(name);
        }
        (false, opt) => {
            out.push_str(if opt { "?.[" } else { "[" });
            out.push_str(&crate::npm::bundle::js_string(name));
            out.push(']');
        }
    }
}
