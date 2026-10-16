//! Calls: module functions, closures, built-in methods, `console`, `Math`, conversions and test matchers.

use super::body::is_mutating_method;
use super::{c_string, Gen, Val};
use crate::ast::{Arg, BinOp, ExprId, ExprKind};
use crate::check::Callee;
use crate::hash::FxMap;
use crate::source::Span;
use crate::types::*;

/// A callback argument prepared once, then invoked per element.
pub(crate) struct Callback {
    /// Direct call to an arrow's C function with a stack environment (`None`: through a `tv_fn`).
    direct: Option<(String, String)>,
    /// The `tv_fn` value (for indirect calls).
    value: String,
    params: Vec<FnParam>,
    ret: TyId,
}

impl<'c, 'a> Gen<'c, 'a> {
    pub(crate) fn call(&mut self, e: ExprId) -> Val {
        let v = self.call_inner(e);
        let m = self.cur_m();
        if self.facts(m).throwing.contains(&e) {
            self.error_check();
        }
        v
    }

    fn call_inner(&mut self, e: ExprId) -> Val {
        let m = self.cur_m();
        let ast = self.ast(m);
        let ExprKind::Call { callee, args, optional, .. } = &ast.expr(e).kind else { unreachable!() };
        let span = ast.expr(e).span;
        let ty = self.ty(e);
        let Some(fact) = self.facts(m).calls.get(&e).cloned() else {
            self.unsupported(span, "this call");
            return Val::plain("0", ty);
        };
        let params: Vec<FnParam> = fact.params.iter().map(|p| FnParam { ty: self.inst(p.ty), ..*p }).collect();
        match fact.callee {
            Callee::Fn(fm, fi) => {
                let mut subst = FxMap::default();
                for (p, t) in &fact.targs {
                    let t = self.inst(*t);
                    subst.insert(*p, t);
                }
                let arg_subst = subst.clone();
                let cname = self.instance(fm, fi, subst);
                if self.is_async_fn(fm, fi) {
                    let argv = self.args(args, &params, Some((fm, fi)));
                    let ret = self.inst(fact.ret);
                    return self.spawn_call(&cname, &argv, ret, ty);
                }
                let mut argv = self.args(&args[..args.len().min(params.len())], &params, Some((fm, fi)));
                if let Some(rest) = fact.rest {
                    // `...xs`: the remaining arguments become one array.
                    let rest = self.inst(rest);
                    let arr = self.pack_rest(&args[params.len().min(args.len())..], rest);
                    argv.push(arr);
                }
                let ret = self.inst(fact.ret);
                // through the unwind channel (`unwind.rs`): the arguments first, then the flag
                if self.b().aware && self.unwinds(fm, fi) {
                    for (i, p) in params.iter().enumerate().take(argv.len()) {
                        if !p.inout {
                            let pt = self.c.types.subst(p.ty, &arg_subst);
                            argv[i] = self.tmp(pt, &argv[i], false).code;
                        }
                    }
                    // (an async arrow passed to JavaScript is always on the channel)
                    let aw = if self.b().root { "true" } else { "aw_" };
                    let v = self.finish_call(&format!("(tv_js_direct = {aw}, {cname}({}))", argv.join(", ")), ret, ty);
                    self.unwind_check();
                    return v;
                }
                self.finish_call(&format!("{cname}({})", argv.join(", ")), ret, ty)
            }
            Callee::Value => {
                // `x.m(args)` on an interface value or a class (through a generic bound): a direct call.
                if let ExprKind::Member { obj, name, optional: false, .. } = &ast.expr(*callee).kind {
                    let ot = self.ty(*obj);
                    match self.tget(ot) {
                        Ty::Interface(..) => {
                            let rv = self.expr(*obj);
                            let rv = if self.heap_rooted(*obj) { self.own(rv) } else { rv };
                            let argv = self.args(args, &params, None);
                            if let Some((call, ret)) = self.iface_call(&rv, *name, argv) {
                                return self.finish_call(&call, ret, ty);
                            }
                        }
                        Ty::Class(..) => {
                            if let Some(crate::check::class::ClassMemberRef::Method(mm)) = self.c.class_member(ot, *name) {
                                let rv = self.expr(*obj);
                                let rv = if self.heap_rooted(*obj) { self.own(rv) } else { rv };
                                let argv = self.args(args, &mm.params, None);
                                let ret = self.c.method_ret(&mm);
                                let call = self.method_call_code(&rv.code, ot, *name, false, argv);
                                return self.finish_call(&call, ret, ty);
                            }
                        }
                        _ => {}
                    }
                }
                let fv = self.expr(*callee);
                let fty = self.c.types.without_undefined(fv.ty);
                let fv = if *optional || fty != fv.ty {
                    if *optional {
                        self.unsupported(span, "optional calls `f?.()`");
                    }
                    self.project(fv, fty)
                } else {
                    fv
                };
                let Ty::Func(ps, ret, _) = self.tget(fty) else {
                    self.unsupported(span, "calling this value");
                    return Val::plain("0", ty);
                };
                let ps = self.c.types.params(ps).to_vec();
                let argv = self.args(args, &ps, None);
                let fvn = self.fresh("fn");
                self.line(format!("tv_fn {fvn} = {};", fv.code));
                let cast = self.fn_ptr_type(&ps, ret);
                let mut all = vec![format!("{fvn}.env")];
                all.extend(argv);
                self.finish_call(&format!("(({cast}){fvn}.fn)({})", all.join(", ")), ret, ty)
            }
            Callee::Method(recv) => {
                let recv = self.inst(recv);
                let ExprKind::Member { obj, name, optional: mopt, .. } = &ast.expr(*callee).kind else {
                    self.unsupported(span, "this method call");
                    return Val::plain("0", ty);
                };
                let name = self.sym(*name).to_string();
                if *mopt {
                    // x?.m(): undefined when x is undefined.
                    let base = self.expr(*obj);
                    let present = self.truthy_val(base.clone());
                    let res = self.fresh("t");
                    let ct = self.ctype(ty);
                    self.line(format!("{ct} {res};"));
                    self.open_block(&format!("if ({present}) {{"));
                    let inner_ret = self.c.types.without_undefined(ty);
                    let rv = self.project(base, recv);
                    let r = self.method_on(Some(rv), *obj, recv, &name, args, &params, inner_ret, span);
                    let r = self.coerce(r, ty);
                    let code = self.consume(r);
                    self.line(format!("{res} = {code};"));
                    self.mid_block("} else {");
                    let u = self.coerce(Val::plain("0", UNDEFINED), ty);
                    self.line(format!("{res} = {};", u.code));
                    self.close_block("}");
                    let owned = self.is_rc(ty);
                    if owned {
                        self.b().temps.last_mut().unwrap().push((res.clone(), ty));
                    }
                    return Val { code: res, ty, owned };
                }
                self.method_on(None, *obj, recv, &name, args, &params, ty, span)
            }
            Callee::ClassMethod { recv, name, sup } => {
                let recv = self.inst(recv);
                self.class_method_call(e, recv, name, sup, args, &params, ty, span)
            }
            Callee::StaticMethod(c, mi) => {
                let argv = self.args(args, &params, None);
                let mut subst = FxMap::default();
                for (p, t) in &fact.targs {
                    let t = self.inst(*t);
                    subst.insert(*p, t);
                }
                let cname = self.static_method_instance(c, mi, subst);
                let ret = self.inst(fact.ret);
                self.finish_call(&format!("{cname}({})", argv.join(", ")), ret, ty)
            }
            Callee::New(c) => {
                let _ = c;
                self.new_object(e, ty, args, &params, span)
            }
            Callee::NewPromise => self.new_promise(args, ty),
            Callee::Js { method } => {
                if let Some(v) = self.js_try_fuse(e, ty, span) {
                    return v;
                }
                let target = match (&ast.expr(*callee).kind, method) {
                    (ExprKind::Member { obj, .. }, Some(_)) => *obj,
                    _ => *callee,
                };
                let f = self.js_callee(target);
                self.js_call(f, method, args, false, ty)
            }
            Callee::JsNew => {
                let f = self.js_callee(*callee);
                self.js_call(f, None, args, true, ty)
            }
            Callee::SuperCtor(base) => {
                let base = self.inst(base);
                self.super_ctor_call(base, args, &params);
                Val::plain("0", VOID)
            }
            Callee::Builtin { ns, name } => {
                let ns = ns.map(|s| self.sym(s).to_string());
                let name = self.sym(name).to_string();
                self.builtin(ns.as_deref(), &name, args, ty, span)
            }
            Callee::Matcher(subject) => {
                let subject = self.inst(subject);
                let ExprKind::Member { obj, name, .. } = &ast.expr(*callee).kind else { return Val::plain("0", VOID) };
                let name = self.sym(*name).to_string();
                // `expect(x)`: evaluate x directly.
                let ExprKind::Call { args: eargs, .. } = &ast.expr(*obj).kind else { return Val::plain("0", VOID) };
                let s = self.expr(eargs[0].expr);
                let s = self.coerce(s, subject);
                self.matcher(s, &name, args, span);
                Val::plain("0", VOID)
            }
        }
    }

    /// Evaluates arguments against (instantiated) parameter types.
    fn args(&mut self, args: &[Arg], params: &[FnParam], callee: Option<(u32, u32)>) -> Vec<String> {
        let mut out = Vec::new();
        let has_inout = params.iter().any(|p| p.inout);
        for (i, a) in args.iter().enumerate() {
            let p = params.get(i).copied().unwrap_or(FnParam { ty: ERROR, inout: false, optional: false });
            if p.inout {
                let pl = self.place(a.expr);
                // Leak-free `inout` array parameters expect a uniquely owned buffer.
                if let (Ty::Array(et), Some((fm, fi))) = (self.tget(pl.ty), callee)
                    && self.inout_leak_free(fm, fi, i)
                {
                    let m = self.cur_m();
                    let known = match (&self.ast(m).expr(a.expr).kind, self.ident_fact(m, a.expr)) {
                        (ExprKind::Ident(_), Some(crate::check::IdentFact::Local(k))) => self.b().unique.contains(&k),
                        _ => false,
                    };
                    if !known {
                        let d = self.desc(et);
                        self.line(format!("tvg_make_unique(&{}, {d});", pl.lv));
                    }
                }
                out.push(format!("&{}", pl.lv));
            } else {
                let v = self.expr(a.expr);
                // An argument before one that awaits (or assigns) is read first, as in JavaScript.
                let v = if args[i + 1..].iter().any(|later| self.changes_in(later.expr)) { self.snapshot(v) } else { v };
                // A value reached through a class instance (or a closure's cell) could be released by
                // the callee through another reference: keep it alive for the call.
                let v = if !v.owned && self.is_rc(v.ty) && (self.heap_rooted(a.expr) || (has_inout && self.is_place_expr(a.expr))) { self.own(v) } else { v };
                let v = self.coerce(v, p.ty);
                out.push(v.code);
            }
        }
        // Omitted optional parameters are `undefined`.
        for p in params.iter().skip(args.len()) {
            let v = self.coerce(Val::plain("0", UNDEFINED), p.ty);
            out.push(v.code);
        }
        out
    }

    /// An owned array (a temporary) holding the given arguments, for a rest parameter.
    fn pack_rest(&mut self, args: &[Arg], elem: TyId) -> String {
        let arr_ty = self.c.types.array(elem);
        if args.is_empty() {
            return "TV_EMPTY_ARR".into();
        }
        let d = self.desc(elem);
        let ect = self.ctype(elem);
        let mut codes = Vec::new();
        for a in args {
            let v = self.expr(a.expr);
            let v = self.coerce(v, elem);
            let code = self.consume(v);
            let n = self.fresh("e");
            self.line(format!("{ect} {n} = {code};"));
            codes.push(n);
        }
        let arr = self.tmp(arr_ty, "TV_EMPTY_ARR", true);
        let w = self.fresh("w");
        self.line(format!("{ect} *{w} = ({ect} *)tv_arr_reserve_tail(&{}, {d}, {});", arr.code, codes.len()));
        for (i, c) in codes.iter().enumerate() {
            self.line(format!("{w}[{i}] = {c};"));
        }
        self.line(format!("{}.len = {};", arr.code, codes.len()));
        arr.code
    }

    pub(crate) fn args_pub(&mut self, args: &[Arg], params: &[FnParam]) -> Vec<String> {
        self.args(args, params, None)
    }

    pub(crate) fn finish_call_pub(&mut self, call: &str, ret: TyId, ty: TyId) -> Val {
        self.finish_call(call, ret, ty)
    }

    fn finish_call(&mut self, call: &str, ret: TyId, ty: TyId) -> Val {
        if ret == VOID || ret == NEVER {
            self.line(format!("{call};"));
            return Val::plain("0", if ty == ERROR { VOID } else { ty });
        }
        let v = self.tmp(ret, call, true);
        self.coerce(v, ty)
    }

    pub(crate) fn fn_ptr_type_pub(&mut self, ps: &[FnParam], ret: TyId) -> String {
        self.fn_ptr_type(ps, ret)
    }

    fn fn_ptr_type(&mut self, ps: &[FnParam], ret: TyId) -> String {
        let rct = if ret == VOID { "void".to_string() } else { self.ctype(ret) };
        let mut parts = vec!["tv_env *".to_string()];
        for p in ps {
            let ct = self.ctype(p.ty);
            parts.push(if p.inout { format!("{ct} *") } else { ct });
        }
        format!("{rct} (*)({})", parts.join(", "))
    }

    // ------------------------------------------------------------ callbacks

    pub(crate) fn prepare_callback(&mut self, arg: ExprId) -> Callback {
        let fty = self.ty(arg);
        let Ty::Func(ps, ret, _) = self.tget(fty) else {
            return Callback { direct: None, value: "((tv_fn){NULL, NULL})".into(), params: Vec::new(), ret: VOID };
        };
        let params = self.c.types.params(ps).to_vec();
        let m = self.cur_m();
        let is_arrow = matches!(self.ast(m).expr(arg).kind, ExprKind::Arrow(_));
        if is_arrow && self.b().stack_arrows.contains(&arg) {
            let info = self.emit_arrow(arg, fty, true);
            let env = if info.captures.is_empty() {
                "NULL".to_string()
            } else {
                let ev = self.fresh("env");
                let ptrs: Vec<String> = info
                    .captures
                    .iter()
                    .map(|(k, _, _)| {
                        let access = self.b().locals.get(k).map(|l| l.access.clone()).unwrap_or_else(|| "tv__dummy".into());
                        format!("&({access})")
                    })
                    .collect();
                self.line(format!("{} {ev} = {{ {{-1, NULL}}, {} }};", info.env_type, ptrs.join(", ")));
                format!("(tv_env *)&{ev}")
            };
            return Callback { direct: Some((info.cname, env)), value: String::new(), params, ret };
        }
        let v = self.expr(arg);
        let fvn = self.fresh("cb");
        self.line(format!("tv_fn {fvn} = {};", v.code));
        Callback { direct: None, value: fvn, params, ret }
    }

    /// Calls a prepared callback with (borrowed) arguments; extra arguments are dropped.
    pub(crate) fn invoke(&mut self, cb: &Callback, args: Vec<Val>) -> Val {
        let mut codes = Vec::new();
        for (p, a) in cb.params.clone().iter().zip(args) {
            let v = self.coerce(a, p.ty);
            codes.push(v.code);
        }
        let call = match &cb.direct {
            Some((f, env)) => {
                let mut all = vec![env.clone()];
                all.extend(codes);
                format!("{f}({})", all.join(", "))
            }
            None => {
                let cast = self.fn_ptr_type(&cb.params, cb.ret);
                let mut all = vec![format!("{}.env", cb.value)];
                all.extend(codes);
                format!("(({cast}){}.fn)({})", cb.value, all.join(", "))
            }
        };
        if cb.ret == VOID {
            self.line(format!("{call};"));
            return Val::plain("0", VOID);
        }
        self.tmp(cb.ret, &call, true)
    }

    /// `(a, b) => a - b`, `b - a`, `a.k - b.k` (f64 or int keys): a stable radix sort.
    /// Returns false (emitting nothing) when the comparator isn't one of these idioms.
    fn radix_sort(&mut self, lv: &str, et: TyId, arg: ExprId) -> bool {
        let m = self.cur_m();
        let ast = self.ast(m);
        let ExprKind::Arrow(f) = &ast.expr(arg).kind else { return false };
        if f.params.len() != 2 || f.params.iter().any(|p| p.inout || p.optional) {
            return false;
        }
        let body = match &f.body {
            crate::ast::ArrowBody::Expr(x) => *x,
            crate::ast::ArrowBody::Block(b) => match &ast.stmt(*b).kind {
                crate::ast::StmtKind::Block(ss) if ss.len() == 1 => match &ast.stmt(ss[0]).kind {
                    crate::ast::StmtKind::Return(Some(x)) => *x,
                    _ => return false,
                },
                _ => return false,
            },
        };
        let mut body = body;
        while let ExprKind::Paren(x) = &ast.expr(body).kind {
            body = *x;
        }
        let ExprKind::Binary(crate::ast::BinOp::Sub, l, r) = &ast.expr(body).kind else { return false };
        // Each side: param.field.field... ; returns (param index, path).
        let facts = self.facts(m);
        let side = |mut e: ExprId| -> Option<(usize, Vec<crate::intern::Sym>)> {
            let mut path = Vec::new();
            loop {
                match &ast.expr(e).kind {
                    ExprKind::Paren(x) => e = *x,
                    ExprKind::Member { obj, name, optional: false, .. } => {
                        path.push(*name);
                        e = *obj;
                    }
                    ExprKind::Ident(_) => {
                        let Some(crate::check::IdentFact::Local(k)) = facts.idents.get(&e) else { return None };
                        let i = f.params.iter().position(|p| p.span.start == *k)?;
                        path.reverse();
                        return Some((i, path));
                    }
                    _ => return None,
                }
            }
        };
        let (Some((li, lp)), Some((ri, rp))) = (side(*l), side(*r)) else { return false };
        if lp != rp || li == ri {
            return false;
        }
        let desc = li == 1;
        // Every step of the key path must be a record field (not a built-in property like `byteLength`).
        let mut cur = et;
        for step in &lp {
            let rec = self.c.unfold(cur);
            let Ty::Record(fs) = self.tget(rec) else { return false };
            let Some(f) = self.c.types.fields(fs).iter().find(|f| f.name == *step).copied() else { return false };
            if matches!(self.tget(cur), Ty::Rec(..)) {
                return false;
            }
            cur = f.ty;
        }
        let key_ty = self.ty(*l);
        if key_ty != F64 && key_ty != INT {
            return false;
        }
        let ect = self.ctype(et);
        let d = self.desc(et);
        let data = format!("(({ect} *)tv_arr_data({lv}))");
        self.line(format!("tvg_make_unique(&{lv}, {d});"));
        if lp.is_empty() && key_ty == F64 && et == F64 {
            self.line(format!("tvg_sort_f64({data}, tv_arr_len({lv}), {desc});"));
            return true;
        }
        // Keyed: radix-sort (key, index) pairs, then permute the elements.
        let access: String = lp.iter().map(|s| format!(".f_{}", self.sym(*s))).collect();
        let name = self.fresh("rsort");
        let key_code = if key_ty == F64 {
            format!("tvg_f64_key((double)(a[i]{access}))")
        } else {
            format!("((uint64_t)(a[i]{access}) ^ 0x8000000000000000ull)")
        };
        // Integer keys: `a - b` must not overflow for any pair, else keep the comparator's semantics.
        let guard = if key_ty == INT {
            format!(
                "tv_int lo = INT64_MAX, hi = INT64_MIN; for (tv_int i = 0; i < n; i++) {{ tv_int v = a[i]{access}; if (v < lo) lo = v; if (v > hi) hi = v; }}\n    tv_int span_; if (__builtin_sub_overflow(hi, lo, &span_)) return false;\n"
            )
        } else {
            String::new()
        };
        let code = format!(
            r#"static bool {name}({ect} *a, tv_int n, bool desc) {{
    if (n < 2) return true;
    {guard}uint64_t *k = tv_alloc((size_t)n * sizeof(uint64_t));
    int64_t *ix = tv_alloc((size_t)n * sizeof(int64_t));
    for (tv_int i = 0; i < n; i++) {{ uint64_t key = {key_code}; k[i] = desc ? ~key : key; ix[i] = i; }}
    tvg_radix64(k, ix, n);
    {ect} *b = tv_alloc((size_t)n * sizeof({ect}));
    for (tv_int i = 0; i < n; i++) b[i] = a[ix[i]];
    memcpy(a, b, (size_t)n * sizeof({ect}));
    tv_free(b); tv_free(k); tv_free(ix);
    return true;
}}
"#
        );
        let _ = std::fmt::Write::write_fmt(&mut self.protos, format_args!("static bool {name}({ect} *a, tv_int n, bool desc);\n"));
        self.helpers_after.push(code);
        if key_ty == INT {
            // Fall back to the comparator (merge sort) when a difference could overflow.
            let cb = self.prepare_callback(arg);
            let sorter = self.sort_fn(et, &cb);
            let ctx = match &cb.direct {
                Some((_, env)) => env.clone(),
                None => format!("(tv_env *)&{}", cb.value),
            };
            self.line(format!("if (!{name}({data}, tv_arr_len({lv}), {desc})) {sorter}({data}, tv_arr_len({lv}), (void *)({ctx}));"));
        } else {
            self.line(format!("{name}({data}, tv_arr_len({lv}), {desc});"));
        }
        true
    }

    /// A stable merge sort specialised for one element type and comparator (inlined).
    fn sort_fn(&mut self, et: TyId, cb: &Callback) -> String {
        let ect = self.ctype(et);
        let name = self.fresh("sort");
        let cmp = match &cb.direct {
            Some((f, _)) => format!("((double)({f}((tv_env *)ctx, (x), (y))))"),
            None => {
                let cast = self.fn_ptr_type(&cb.params, cb.ret);
                format!("((double)((({cast})((tv_fn *)ctx)->fn)(((tv_fn *)ctx)->env, (x), (y))))")
            }
        };
        let code = format!(
            r#"static void {name}({ect} *a, tv_int n, void *ctx) {{
#define CMP(x, y) {cmp}
    enum {{ RUN = 16 }};
    for (tv_int lo = 0; lo < n; lo += RUN) {{
        tv_int hi = lo + RUN < n ? lo + RUN : n;
        for (tv_int i = lo + 1; i < hi; i++) {{
            {ect} x = a[i]; tv_int j = i;
            while (j > lo && CMP(a[j - 1], x) > 0) {{ a[j] = a[j - 1]; j--; }}
            a[j] = x;
        }}
    }}
    if (n <= RUN) return;
    {ect} *buf = tv_alloc((size_t)n * sizeof({ect}));
    {ect} *src = a, *dst = buf;
    for (tv_int w = RUN; w < n; w *= 2) {{
        for (tv_int lo = 0; lo < n; lo += 2 * w) {{
            tv_int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            if (mid >= hi || !(CMP(src[mid - 1], src[mid]) > 0)) {{ memcpy(dst + lo, src + lo, (size_t)(hi - lo) * sizeof({ect})); continue; }}
            tv_int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {{
                bool r = CMP(src[i], src[j]) > 0;
                dst[k++] = r ? src[j] : src[i];
                j += r; i += !r;
            }}
            if (i < mid) memcpy(dst + k, src + i, (size_t)(mid - i) * sizeof({ect}));
            if (j < hi) memcpy(dst + k, src + j, (size_t)(hi - j) * sizeof({ect}));
        }}
        {ect} *t = src; src = dst; dst = t;
    }}
    if (src != a) memcpy(a, src, (size_t)n * sizeof({ect}));
    tv_free(buf);
#undef CMP
}}
"#
        );
        let _ = std::fmt::Write::write_fmt(&mut self.protos, format_args!("static void {name}({ect} *a, tv_int n, void *ctx);\n"));
        self.helpers_after.push(code);
        name
    }

    // ------------------------------------------------------------ built-in methods

    /// A built-in method call. `given` is an already-evaluated receiver (for `x?.m()`).
    #[allow(clippy::too_many_arguments)]
    fn method_on(&mut self, given: Option<Val>, obj: ExprId, recv: TyId, name: &str, args: &[Arg], params: &[FnParam], ty: TyId, span: Span) -> Val {
        // Receiver: a place for mutating methods, a value otherwise.
        let mutating = is_mutating_method(name) && matches!(self.tget(recv), Ty::Array(_) | Ty::Map(..) | Ty::Set(_));
        if mutating && given.is_some() {
            self.unsupported(span, "mutating methods through `?.`");
            return Val::plain("0", ty);
        }
        if mutating {
            let p = self.place(obj);
            let narrowed = self.ty(obj);
            let lv = if narrowed != p.ty {
                // Narrowed optional receivers: project the place.
                let v = self.project(Val::plain(p.lv.clone(), p.ty), recv);
                v.code
            } else {
                p.lv
            };
            return self.mutating_method(&lv, recv, name, args, params, ty, span);
        }
        // `(a / b).round(k, "mode")` on decimals: rounded once, straight to k places (see
        // tv_dec_div_round; it divides first when that could differ)
        if given.is_none() && recv == DECIMAL && name == "round" {
            const MODES: [&str; 9] = ["halfEven", "halfExpand", "halfTrunc", "halfCeil", "halfFloor", "ceil", "floor", "trunc", "expand"];
            let m = self.cur_m();
            let mut inner = obj;
            while let ExprKind::Paren(x) = self.ast(m).expr(inner).kind {
                inner = x;
            }
            let mode = match args.get(1).map(|a| &self.ast(m).expr(a.expr).kind) {
                None => Some(0),
                Some(ExprKind::Str(s)) => MODES.iter().position(|md| *md == self.sym(*s)),
                _ => None,
            };
            if let (ExprKind::Binary(BinOp::Div, l, r), Some(mode), Some(a0)) = (&self.ast(m).expr(inner).kind, mode, args.first())
                && self.ty(inner) == DECIMAL
            {
                let (l, r, a0) = (*l, *r, a0.expr);
                let lv = self.expr_before(l, &[r, a0]);
                let lv = self.coerce(lv, DECIMAL);
                let rv = self.expr_before(r, &[a0]);
                let rv = self.coerce(rv, DECIMAL);
                let k = self.expr(a0);
                let present = self.truthy_val(k.clone());
                let k = self.project(k, INT);
                let k = if present == "true" { k.code } else { format!("({present} ? {} : 0)", k.code) };
                let loc = self.loc(span);
                return Val::plain(format!("tv_dec_div_round({}, {}, {k}, {mode}, {loc})", lv.code, rv.code), DECIMAL);
            }
        }
        let rv = match given {
            Some(v) => v,
            None => self.expr(obj),
        };
        let rv = self.coerce(rv, recv);
        match self.tget(recv) {
            Ty::Array(et) => self.array_method(rv, et, name, args, params, ty, span),
            Ty::Map(k, v) => self.map_method(rv, k, v, name, args, ty),
            Ty::Set(k) => self.set_method(rv, k, name, args, ty),
            Ty::Str | Ty::StrLit(_) => {
                let s = self.coerce(rv, STR);
                self.string_method(s, name, args, ty, span)
            }
            Ty::Bool => {
                let t = self.lit(b"true");
                let f = self.lit(b"false");
                Val::plain(format!("(({}) ? {t} : {f})", rv.code), STR)
            }
            Ty::Decimal => {
                let loc = self.loc(span);
                match name {
                    "toFixed" => {
                        let d = self.expr(args[0].expr);
                        let d = self.int_code(d);
                        self.tmp(STR, &format!("tv_dec_to_fixed({}, {d}, {loc})", rv.code), true)
                    }
                    "floor" | "ceil" | "trunc" => Val::plain(format!("tv_dec_{name}({})", rv.code), DECIMAL),
                    "abs" => Val::plain(format!("tv_dec_abs({})", rv.code), DECIMAL),
                    "round" => {
                        let places = match args.first() {
                            Some(a) => {
                                let r = self.expr(a.expr);
                                let present = self.truthy_val(r.clone());
                                let r = self.project(r, INT);
                                if present == "true" { r.code } else { format!("({present} ? {} : 0)", r.code) }
                            }
                            None => "0".into(),
                        };
                        const MODES: [&str; 9] = ["halfEven", "halfExpand", "halfTrunc", "halfCeil", "halfFloor", "ceil", "floor", "trunc", "expand"];
                        let lit_mode = match args.get(1).map(|a| &self.ast(self.cur_m()).expr(a.expr).kind) {
                            None => Some(0),
                            Some(ExprKind::Str(s)) => MODES.iter().position(|m| *m == self.sym(*s)),
                            _ => None,
                        };
                        if let Some(k) = lit_mode {
                            return Val::plain(format!("tv_dec_round_m({}, {places}, {k}, {loc})", rv.code), DECIMAL);
                        }
                        let mode = match args.get(1) {
                            Some(a) => {
                                let m = self.expr(a.expr);
                                let present = self.truthy_val(m.clone());
                                let mt = self.c.types.without_undefined(m.ty);
                                let ms = self.project(m, mt);
                                let s = self.coerce(ms, STR);
                                let even = self.lit(b"halfEven");
                                if present == "true" { s.code } else { format!("({present} ? {} : {even})", s.code) }
                            }
                            None => self.lit(b"halfEven"),
                        };
                        Val::plain(format!("tv_dec_round({}, {places}, {mode}, {loc})", rv.code), DECIMAL)
                    }
                    _ => self.tmp(STR, &format!("tv_dec_to_string({})", rv.code), true),
                }
            }
            Ty::BigInt => {
                let radix = match args.first() {
                    Some(a) => {
                        let r = self.expr(a.expr);
                        let present = self.truthy_val(r.clone());
                        let r = self.project(r, INT);
                        if present == "true" { r.code } else { format!("({present} ? {} : 10)", r.code) }
                    }
                    None => "10".into(),
                };
                let loc = self.loc(span);
                self.tmp(STR, &format!("tv_big_to_string({}, {radix}, {loc})", rv.code), true)
            }
            _ if self.c.types.is_numeric(recv) => match name {
                "toFixed" => {
                    let d = self.expr(args[0].expr);
                    let d = self.int_code(d);
                    let loc = self.loc(span);
                    if self.c.types.is_int(recv) {
                        let x = self.int_code(rv);
                        self.tmp(STR, &format!("tv_int_to_fixed({x}, {d}, {loc})"), true)
                    } else {
                        let x = self.coerce(rv, F64);
                        self.tmp(STR, &format!("tv_f64_to_fixed({}, {d}, {loc})", x.code), true)
                    }
                }
                _ => {
                    if self.c.types.is_int(recv) {
                        let x = self.int_code(rv);
                        self.tmp(STR, &format!("tv_str_from_int({x})"), true)
                    } else {
                        let x = self.coerce(rv, F64);
                        self.tmp(STR, &format!("tv_str_from_f64({})", x.code), true)
                    }
                }
            },
            _ => {
                self.unsupported(span, "this method");
                Val::plain("0", ty)
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn mutating_method(&mut self, lv: &str, recv: TyId, name: &str, args: &[Arg], params: &[FnParam], ty: TyId, span: Span) -> Val {
        match (self.tget(recv), name) {
            (Ty::Array(et), "push" | "unshift") => {
                let d = self.desc(et);
                let ect = self.ctype(et);
                for a in args {
                    let v = self.expr(a.expr);
                    let v = self.coerce(v, et);
                    let code = self.consume(v);
                    if name == "push" {
                        // A local made unique before the loop: no ownership check per push.
                        let b = self.b();
                        let key = b.unique.iter().copied().find(|k| b.locals.get(k).is_some_and(|l| l.access == lv));
                        let exact = key.is_some_and(|k| b.exact_pushes.contains(&k));
                        match key.and_then(|k| b.push_caps.get(&k).cloned()) {
                            // Capacity reserved before the loop for every push it makes.
                            Some(_) if exact => self.line(format!("TVG_PUSH_X({ect}, &{lv}, {code});")),
                            // Only pushes can reallocate it in this loop: compare against a cached capacity.
                            Some(cap) => self.line(format!("TVG_PUSH_C({ect}, &{lv}, {cap}, {d}, {code});")),
                            None => {
                                let mac = if key.is_some() { "TVG_PUSH_U" } else { "TVG_PUSH" };
                                self.line(format!("{mac}({ect}, &{lv}, {d}, {code});"));
                            }
                        }
                    } else {
                        let tmpn = self.fresh("e");
                        self.line(format!("{ect} {tmpn} = {code};"));
                        self.line(format!("tv_arr_unshift(&{lv}, {d}, &{tmpn});"));
                    }
                }
                Val::plain(format!("tv_arr_len({lv})"), INT)
            }
            (Ty::Array(et), "pop" | "shift") => {
                let d = self.desc(et);
                let ect = self.ctype(et);
                let out = self.fresh("e");
                let ok = self.fresh("ok");
                self.line(format!("{ect} {out}; bool {ok} = tv_arr_{name}(&{lv}, {d}, &{out});"));
                self.optional_from(&ok, Val { code: out, ty: et, owned: true }, ty)
            }
            (Ty::Array(et), "reverse") => {
                let d = self.desc(et);
                self.line(format!("tv_arr_reverse(&{lv}, {d});"));
                let v = Val::plain(lv.to_string(), recv);
                let v = self.own(v);
                self.coerce(v, ty)
            }
            (Ty::Array(et), "sort") => {
                let d = self.desc(et);
                match args.first() {
                    None => self.line(format!("tv_arr_sort(&{lv}, {d}, NULL, NULL);")),
                    Some(a) if self.radix_sort(lv, et, a.expr) => {}
                    Some(a) => {
                        let cb = self.prepare_callback(a.expr);
                        let sorter = self.sort_fn(et, &cb);
                        let ctx = match &cb.direct {
                            Some((_, env)) => env.clone(),
                            None => format!("(tv_env *)&{}", cb.value),
                        };
                        let ect = self.ctype(et);
                        self.line(format!("tvg_make_unique(&{lv}, {d}); {sorter}(({ect} *)tv_arr_data({lv}), tv_arr_len({lv}), (void *)({ctx}));"));
                    }
                }
                let v = Val::plain(lv.to_string(), recv);
                let v = self.own(v);
                self.coerce(v, ty)
            }
            (Ty::Map(k, v), "set") => {
                let (kd, vd) = (self.desc(k), self.desc(v));
                let (kct, vct) = (self.ctype(k), self.ctype(v));
                let kv = self.expr(args[0].expr);
                let kv = self.coerce(kv, k);
                let vv = self.expr(args[1].expr);
                let vv = self.coerce(vv, v);
                let kc = self.consume(kv);
                let vc = self.consume(vv);
                let (kn, vn) = (self.fresh("k"), self.fresh("v"));
                self.line(format!("{kct} {kn} = {kc}; {vct} {vn} = {vc};"));
                self.line(format!("tv_map_set(&{lv}, {kd}, {vd}, &{kn}, &{vn});"));
                Val::plain("0", VOID)
            }
            (Ty::Map(k, _), "delete") | (Ty::Set(k), "delete") => {
                let kd = self.desc(k);
                let vd = if let Ty::Map(_, v) = self.tget(recv) { self.desc(v) } else { "&tv_type_undefined".into() };
                let kv = self.expr(args[0].expr);
                let kv = self.coerce(kv, k);
                let kn = self.key_tmp(kv, k);
                self.tmp(BOOL, &format!("tv_map_delete(&{lv}, {kd}, {vd}, &{kn})"), false)
            }
            (Ty::Map(k, v), "clear") => {
                let (kd, vd) = (self.desc(k), self.desc(v));
                self.line(format!("tv_map_clear(&{lv}, {kd}, {vd});"));
                Val::plain("0", VOID)
            }
            (Ty::Set(k), "add") => {
                let kd = self.desc(k);
                let kct = self.ctype(k);
                let kv = self.expr(args[0].expr);
                let kv = self.coerce(kv, k);
                let kc = self.consume(kv);
                let kn = self.fresh("k");
                self.line(format!("{kct} {kn} = {kc};"));
                self.line(format!("tv_map_set(&{lv}, {kd}, &tv_type_undefined, &{kn}, NULL);"));
                Val::plain("0", VOID)
            }
            (Ty::Set(k), "clear") => {
                let kd = self.desc(k);
                self.line(format!("tv_map_clear(&{lv}, {kd}, &tv_type_undefined);"));
                Val::plain("0", VOID)
            }
            _ => {
                let _ = params;
                self.unsupported(span, &format!("the `{name}` method"));
                Val::plain("0", ty)
            }
        }
    }

    /// A key value in an addressable temporary.
    fn key_tmp(&mut self, v: Val, k: TyId) -> String {
        let kct = self.ctype(k);
        let kn = self.fresh("k");
        self.line(format!("{kct} {kn} = {};", v.code));
        kn
    }

    /// `ok ? value : undefined` as a value of the optional type `ty` (takes ownership of `v`).
    fn optional_from(&mut self, ok: &str, v: Val, ty: TyId) -> Val {
        let res = self.fresh("t");
        let ct = self.ctype(ty);
        self.line(format!("{ct} {res};"));
        self.open_block(&format!("if ({ok}) {{"));
        let inner = self.coerce(v, ty);
        let code = self.consume(inner);
        self.line(format!("{res} = {code};"));
        self.mid_block("} else {");
        let u = self.coerce(Val::plain("0", UNDEFINED), ty);
        self.line(format!("{res} = {};", u.code));
        self.close_block("}");
        let owned = self.is_rc(ty);
        if owned {
            self.b().temps.last_mut().unwrap().push((res.clone(), ty));
        }
        Val { code: res, ty, owned }
    }

    pub(crate) fn open_block(&mut self, s: &str) {
        self.line(s);
        self.b().temps.push(Vec::new());
        self.bump(1);
    }

    pub(crate) fn mid_block(&mut self, s: &str) {
        self.flush_block_temps();
        self.bump(-1);
        self.line(s);
        self.bump(1);
        self.b().temps.push(Vec::new());
    }

    pub(crate) fn close_block(&mut self, s: &str) {
        self.flush_block_temps();
        self.bump(-1);
        self.line(s);
    }

    fn flush_block_temps(&mut self) {
        let temps = self.b().temps.pop().unwrap();
        for (n, t) in temps.into_iter().rev() {
            let r = self.release_code(t, &n);
            self.line(format!("{r};"));
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn array_method(&mut self, a: Val, et: TyId, name: &str, args: &[Arg], params: &[FnParam], ty: TyId, span: Span) -> Val {
        let d = self.desc(et);
        let ect = self.ctype(et);
        let arr = a;
        let elem = |i: &str| Val::plain(format!("(({ect} *)tv_arr_data({}))[{i}]", arr.code), et);
        match name {
            "map" | "flatMap" | "filter" | "forEach" | "some" | "every" | "find" | "findIndex" => {
                let cb = self.prepare_callback(args[0].expr);
                let (res, res_ty) = match name {
                    "map" | "flatMap" => {
                        let Ty::Array(rt) = self.tget(ty) else { return Val::plain("0", ty) };
                        let rd = self.desc(rt);
                        let cap = if name == "map" { format!("tv_arr_len({})", arr.code) } else { "0".into() };
                        (self.tmp(ty, &format!("tv_arr_with_capacity({rd}, {cap})"), true), ty)
                    }
                    "filter" => (self.tmp(ty, &format!("tv_arr_with_capacity({d}, 0)"), true), ty),
                    "forEach" => (Val::plain("0", VOID), VOID),
                    "some" => (self.tmp(BOOL, "false", false), BOOL),
                    "every" => (self.tmp(BOOL, "true", false), BOOL),
                    "findIndex" => (self.tmp(INT, "-1", false), INT),
                    _ => {
                        let u = self.coerce(Val::plain("0", UNDEFINED), ty);
                        let code = self.consume(u);
                        (self.tmp(ty, &code, true), ty)
                    }
                };
                let i = self.fresh("i");
                self.open_block(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", arr.code));
                let r = self.invoke(&cb, vec![elem(&i), Val::plain(i.clone(), INT)]);
                match name {
                    "map" => {
                        let Ty::Array(rt) = self.tget(res_ty) else { unreachable!() };
                        let rd = self.desc(rt);
                        let rct = self.ctype(rt);
                        let v = self.coerce(r, rt);
                        let code = self.consume(v);
                        self.line(format!("TVG_PUSH({rct}, &{}, {rd}, {code});", res.code));
                    }
                    "flatMap" => {
                        let Ty::Array(rt) = self.tget(res_ty) else { unreachable!() };
                        let rd = self.desc(rt);
                        let v = self.coerce(r, res_ty);
                        let n = self.fresh("c");
                        self.line(format!("tv_arr {n} = tv_arr_concat({}, {}, {rd}); tv_arr_release({}, {rd}); {} = {n};", res.code, v.code, res.code, res.code));
                    }
                    "filter" => {
                        let c = self.truthy_val(r);
                        let copy = self.retain_code(et, "x_");
                        self.line(format!("if ({c}) {{ {ect} x_ = {}; {copy}; TVG_PUSH({ect}, &{}, {d}, x_); }}", elem(&i).code, res.code));
                    }
                    "some" => {
                        let c = self.truthy_val(r);
                        self.line(format!("if ({c}) {{ {} = true; break; }}", res.code));
                    }
                    "every" => {
                        let c = self.truthy_val(r);
                        self.line(format!("if (!({c})) {{ {} = false; break; }}", res.code));
                    }
                    "findIndex" => {
                        let c = self.truthy_val(r);
                        self.line(format!("if ({c}) {{ {} = {i}; break; }}", res.code));
                    }
                    "find" => {
                        let c = self.truthy_val(r);
                        let cond = self.fresh("hit");
                        self.line(format!("bool {cond} = {c};"));
                        self.flush_block_temps();
                        self.b().temps.push(Vec::new());
                        self.line(format!("if ({cond}) {{"));
                        self.bump(1);
                        let found = self.coerce(elem(&i), ty);
                        let code = self.consume(found);
                        let rel = self.release_code(ty, &res.code);
                        self.line(format!("{rel}; {} = {code};", res.code));
                        self.line("break;");
                        self.bump(-1);
                        self.line("}");
                    }
                    _ => {}
                }
                self.close_block("}");
                res
            }
            "reduce" => {
                let cb = self.prepare_callback(args[0].expr);
                let acc_ty = ty;
                let init = self.expr(args[1].expr);
                let init = self.coerce(init, acc_ty);
                let code = self.consume(init);
                let act = self.ctype(acc_ty);
                let acc = self.fresh("acc");
                self.line(format!("{act} {acc} = {code};"));
                let i = self.fresh("i");
                self.open_block(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", arr.code));
                let r = self.invoke(&cb, vec![Val::plain(acc.clone(), acc_ty), elem(&i), Val::plain(i.clone(), INT)]);
                let r = self.coerce(r, acc_ty);
                let code = self.consume(r);
                let n = self.fresh("n");
                let rel = self.release_code(acc_ty, &acc);
                self.line(format!("{act} {n} = {code}; {rel}; {acc} = {n};"));
                self.close_block("}");
                let owned = self.is_rc(acc_ty);
                if owned {
                    self.b().temps.last_mut().unwrap().push((acc.clone(), acc_ty));
                }
                Val { code: acc, ty: acc_ty, owned }
            }
            "indexOf" | "lastIndexOf" | "includes" => {
                let x = self.expr(args[0].expr);
                let x = self.coerce(x, et);
                let xn = self.key_tmp(x, et);
                let f = if name == "lastIndexOf" { "tv_arr_last_index_of" } else { "tv_arr_index_of" };
                let call = format!("{f}({}, {d}, &{xn})", arr.code);
                if name == "includes" { Val::plain(format!("({call} >= 0)"), BOOL) } else { Val::plain(call, INT) }
            }
            "join" => {
                let sep = match args.first() {
                    Some(a) => {
                        let v = self.expr(a.expr);
                        self.coerce(v, STR).code
                    }
                    None => self.lit(b","),
                };
                self.tmp(STR, &format!("tv_arr_join({}, {d}, {sep})", arr.code), true)
            }
            "slice" => {
                let mut vals = Vec::new();
                for a in args.iter().take(2) {
                    let v = self.expr(a.expr);
                    let opt_int = self.c.types.optional(INT);
                    let v = self.coerce(v, opt_int);
                    vals.push(v);
                }
                let (mut s, mut e2, mut hs, mut he) = ("0".to_string(), "0".to_string(), "false".to_string(), "false".to_string());
                for (idx, v) in vals.iter().enumerate() {
                    let (val, has) = self.optional_int(v);
                    if idx == 0 {
                        s = val;
                        hs = has;
                    } else {
                        e2 = val;
                        he = has;
                    }
                }
                self.tmp(ty, &format!("tv_arr_slice({}, {d}, {s}, {e2}, {hs}, {he})", arr.code), true)
            }
            "concat" => {
                let o = self.expr(args[0].expr);
                let o = self.coerce(o, ty);
                self.tmp(ty, &format!("tv_arr_concat({}, {}, {d})", arr.code, o.code), true)
            }
            "at" => {
                let i = self.expr(args[0].expr);
                let i = self.int_code(i);
                let iv = self.fresh("ix");
                self.line(format!("tv_int {iv} = {i}; if ({iv} < 0) {iv} += tv_arr_len({});", arr.code));
                let ok = format!("({iv} >= 0 && {iv} < tv_arr_len({}))", arr.code);
                let res = self.fresh("t");
                let ct = self.ctype(ty);
                self.line(format!("{ct} {res};"));
                self.open_block(&format!("if ({ok}) {{"));
                let v = self.coerce(elem(&iv), ty);
                let code = self.consume(v);
                self.line(format!("{res} = {code};"));
                self.mid_block("} else {");
                let u = self.coerce(Val::plain("0", UNDEFINED), ty);
                self.line(format!("{res} = {};", u.code));
                self.close_block("}");
                let owned = self.is_rc(ty);
                if owned {
                    self.b().temps.last_mut().unwrap().push((res.clone(), ty));
                }
                Val { code: res, ty, owned }
            }
            _ => {
                let _ = params;
                self.unsupported(span, &format!("the array method `{name}`"));
                Val::plain("0", ty)
            }
        }
    }

    pub(crate) fn optional_int_pub(&mut self, v: &Val) -> (String, String) {
        self.optional_int(v)
    }

    pub(crate) fn optional_from_pub(&mut self, ok: &str, v: Val, ty: TyId) -> Val {
        self.optional_from(ok, v, ty)
    }

    /// Splits an `int | undefined` value into (value, present).
    fn optional_int(&mut self, v: &Val) -> (String, String) {
        match self.tag_of(v.ty, UNDEFINED) {
            Some(k) => {
                let ik = self.tag_of(v.ty, INT).unwrap_or(0);
                (format!("(({}).tag == {k} ? 0 : ({}).u.m{ik})", v.code, v.code), format!("(({}).tag != {k})", v.code))
            }
            None => (v.code.clone(), "true".into()),
        }
    }

    pub(crate) fn truthy_val(&mut self, v: Val) -> String {
        if v.ty == BOOL {
            return v.code;
        }
        match self.tag_of(v.ty, UNDEFINED) {
            Some(k) => format!("(!{})", self.u_is(&v.code, v.ty, k)),
            None => "true".into(),
        }
    }

    fn map_method(&mut self, mv: Val, k: TyId, v: TyId, name: &str, args: &[Arg], ty: TyId) -> Val {
        let (kd, vd) = (self.desc(k), self.desc(v));
        let vct = self.ctype(v);
        match name {
            "get" | "has" => {
                let kv = self.expr(args[0].expr);
                let kv = self.coerce(kv, k);
                let kn = self.key_tmp(kv, k);
                if name == "has" {
                    return Val::plain(format!("tv_map_has({}, {kd}, {vd}, &{kn})", mv.code), BOOL);
                }
                let p = self.fresh("vp");
                self.line(format!("void *{p} = tv_map_get({}, {kd}, {vd}, &{kn});", mv.code));
                let res = self.fresh("t");
                let ct = self.ctype(ty);
                self.line(format!("{ct} {res};"));
                self.open_block(&format!("if ({p}) {{"));
                let found = self.coerce(Val::plain(format!("(*({vct} *){p})"), v), ty);
                let code = self.consume(found);
                self.line(format!("{res} = {code};"));
                self.mid_block("} else {");
                let u = self.coerce(Val::plain("0", UNDEFINED), ty);
                self.line(format!("{res} = {};", u.code));
                self.close_block("}");
                let owned = self.is_rc(ty);
                if owned {
                    self.b().temps.last_mut().unwrap().push((res.clone(), ty));
                }
                Val { code: res, ty, owned }
            }
            "keys" => self.tmp(ty, &format!("tv_map_keys({}, {kd}, {vd})", mv.code), true),
            "values" => self.tmp(ty, &format!("tv_map_values({}, {kd}, {vd})", mv.code), true),
            "forEach" => {
                let cb = self.prepare_callback(args[0].expr);
                let (kct, cur) = (self.ctype(k), self.fresh("i"));
                self.line(format!("tv_int {cur} = 0; void *{cur}k, *{cur}v;"));
                self.open_block(&format!("while (tv_map_next({}, {kd}, {vd}, &{cur}, &{cur}k, &{cur}v)) {{", mv.code));
                let r = self.invoke(&cb, vec![Val::plain(format!("(*({vct} *){cur}v)"), v), Val::plain(format!("(*({kct} *){cur}k)"), k)]);
                let _ = r;
                self.close_block("}");
                Val::plain("0", VOID)
            }
            _ => Val::plain("0", ty),
        }
    }

    fn set_method(&mut self, sv: Val, k: TyId, name: &str, args: &[Arg], ty: TyId) -> Val {
        let kd = self.desc(k);
        match name {
            "has" => {
                let kv = self.expr(args[0].expr);
                let kv = self.coerce(kv, k);
                let kn = self.key_tmp(kv, k);
                Val::plain(format!("tv_map_has({}, {kd}, &tv_type_undefined, &{kn})", sv.code), BOOL)
            }
            "values" => self.tmp(ty, &format!("tv_map_keys({}, {kd}, &tv_type_undefined)", sv.code), true),
            "forEach" => {
                let cb = self.prepare_callback(args[0].expr);
                let (kct, cur) = (self.ctype(k), self.fresh("i"));
                self.line(format!("tv_int {cur} = 0; void *{cur}k, *{cur}v;"));
                self.open_block(&format!("while (tv_map_next({}, {kd}, &tv_type_undefined, &{cur}, &{cur}k, &{cur}v)) {{", sv.code));
                let _ = self.invoke(&cb, vec![Val::plain(format!("(*({kct} *){cur}k)"), k)]);
                self.close_block("}");
                Val::plain("0", VOID)
            }
            _ => Val::plain("0", ty),
        }
    }

    fn string_method(&mut self, s: Val, name: &str, args: &[Arg], ty: TyId, span: Span) -> Val {
        let mut argv: Vec<Val> = Vec::new();
        for a in args {
            argv.push(self.expr(a.expr));
        }
        let str_arg = |g: &mut Self, i: usize| -> String {
            let v = argv[i].clone();
            g.coerce(v, STR).code
        };
        let int_arg = |g: &mut Self, i: usize| -> String {
            let v = argv[i].clone();
            g.int_code(v)
        };
        let loc = self.loc(span);
        let code = match name {
            "chars" => format!("tv_str_chars({})", s.code),
            "slice" => {
                let a0 = int_arg(self, 0);
                let (e, he) = if argv.len() > 1 {
                    let v = argv[1].clone();
                    let opt_int = self.c.types.optional(INT);
                    let v = self.coerce(v, opt_int);
                    self.optional_int(&v)
                } else {
                    ("0".into(), "false".into())
                };
                format!("tv_str_slice({}, {a0}, {e}, {he}, {loc})", s.code)
            }
            "includes" => return Val::plain(format!("tv_str_includes({}, {})", s.code, str_arg(self, 0)), BOOL),
            "startsWith" => return Val::plain(format!("tv_str_starts_with({}, {})", s.code, str_arg(self, 0)), BOOL),
            "endsWith" => return Val::plain(format!("tv_str_ends_with({}, {})", s.code, str_arg(self, 0)), BOOL),
            "indexOf" => return Val::plain(format!("tv_str_index_of({}, {})", s.code, str_arg(self, 0)), INT),
            "split" => format!("tv_str_split({}, {})", s.code, str_arg(self, 0)),
            "trim" => format!("tv_str_trim({})", s.code),
            "trimStart" => format!("tv_str_trim_start({})", s.code),
            "trimEnd" => format!("tv_str_trim_end({})", s.code),
            "toUpperCase" => format!("tv_str_to_upper({})", s.code),
            "toLowerCase" => format!("tv_str_to_lower({})", s.code),
            "replace" => format!("tv_str_replace({}, {}, {})", s.code, str_arg(self, 0), str_arg(self, 1)),
            "replaceAll" => format!("tv_str_replace_all({}, {}, {})", s.code, str_arg(self, 0), str_arg(self, 1)),
            "repeat" => format!("tv_str_repeat({}, {}, {loc})", s.code, int_arg(self, 0)),
            "padStart" | "padEnd" => {
                let fill = if argv.len() > 1 { str_arg(self, 1) } else { self.lit(b" ") };
                let f = if name == "padStart" { "tv_str_pad_start" } else { "tv_str_pad_end" };
                format!("{f}({}, {}, {fill})", s.code, int_arg(self, 0))
            }
            "toString" => {
                let v = self.own(s);
                return self.coerce(v, ty);
            }
            _ => {
                self.unsupported(span, &format!("the string method `{name}`"));
                return Val::plain("0", ty);
            }
        };
        self.tmp(ty, &code, true)
    }

    // ------------------------------------------------------------ console, Math, globals

    fn builtin(&mut self, ns: Option<&str>, name: &str, args: &[Arg], ty: TyId, span: Span) -> Val {
        match (ns, name) {
            (Some("console"), _) => {
                // A lone template literal is written straight into the line, not built as a string first.
                let ast = self.ast(self.cur_m());
                let template = match args {
                    [a] => match &ast.expr(a.expr).kind {
                        ExprKind::Template(parts, exprs) => Some((parts, exprs)),
                        _ => None,
                    },
                    _ => None,
                };
                let parts_vals = template.map(|(parts, exprs)| (parts, self.exprs_in_order(exprs)));
                let arg_exprs: Vec<ExprId> = args.iter().map(|a| a.expr).collect();
                let vals: Vec<Val> = if parts_vals.is_some() { Vec::new() } else { self.exprs_in_order(&arg_exprs) };
                let sbv = self.fresh("sb");
                self.line(format!("tv_sb {sbv} = {{0}};"));
                self.line("{");
                self.bump(1);
                self.line(format!("tv_sb *sb = &{sbv};"));
                if let Some((parts, tvals)) = &parts_vals {
                    self.template_into(parts, tvals);
                }
                for (i, v) in vals.iter().enumerate() {
                    if i > 0 {
                        self.line("tv_sb_push_char(sb, ' ');");
                    }
                    let c = self.inspect_root(v.ty, &v.code, "0");
                    self.line(format!("{c};"));
                }
                self.bump(-1);
                self.line("}");
                let f = if matches!(name, "error" | "warn") { "tv_err_sb_line" } else { "tv_out_sb_line" };
                self.line(format!("{f}(&{sbv}); tv_sb_free(&{sbv});"));
                Val::plain("0", VOID)
            }
            (Some("Math"), _) => self.math(name, args, ty, span),
            (Some("process"), "exit") => {
                let code = match args.first() {
                    Some(a) => {
                        let v = self.expr(a.expr);
                        let oi = self.c.types.optional(INT);
                        let v = self.coerce(v, oi);
                        let (inner, _present) = self.optional_int_pub(&v);
                        inner
                    }
                    None => "0".into(),
                };
                self.line(format!("tv_process_exit({code});"));
                Val::plain("0", ty)
            }
            (Some("process"), "cwd") => self.tmp(STR, "tv_native_cwd()", true),
            (Some("Date"), "now") => Val::plain("tv_date_now()", F64),
            (Some("performance"), "now") => Val::plain("tv_performance_now()", F64),
            (Some("stdout" | "stderr"), "write") => {
                let v = self.expr(args[0].expr);
                let v = self.coerce(v, STR);
                let f = if ns == Some("stdout") { "tv_write_stdout" } else { "tv_write_stderr" };
                self.line(format!("{f}({});", v.code));
                Val::plain("0", VOID)
            }
            (Some("__native"), _) => {
                let (ps, ret) = crate::check::native_sig_pub(&mut self.c.types, name).unwrap_or((Vec::new(), VOID));
                let mut argv = Vec::new();
                for (a, pt) in args.iter().zip(&ps) {
                    let v = self.expr(a.expr);
                    let v = self.coerce(v, *pt);
                    argv.push(v.code);
                }
                let call = format!("tv_native_{name}({})", argv.join(", "));
                if ret == VOID {
                    self.line(format!("{call};"));
                    return Val::plain("0", VOID);
                }
                let v = self.tmp(ret, &call, true);
                self.coerce(v, ty)
            }
            // A settled promise: one allocation, no closures.
            (Some("Promise"), "resolve" | "reject") => {
                let pt = self.c.types.without_undefined(ty);
                let Ty::Promise(value, _) = self.tget(pt) else {
                    self.unsupported(span, "this promise");
                    return Val::plain("NULL", ty);
                };
                let desc = self.desc(value);
                let p = self.tmp(ty, &format!("tv_promise_new({desc})"), true);
                if name == "reject" {
                    let e = self.expr(args[0].expr);
                    self.line(format!("tvg_obj_retain({0}); tv_promise_reject({1}, {0});", e.code, p.code));
                } else if let Some(a) = args.first() {
                    let x = self.expr(a.expr);
                    let x = self.coerce(x, value);
                    let ct = self.ctype(value);
                    let rv = self.fresh("rv");
                    self.line(format!("{ct} {rv} = {};", x.code));
                    self.line(format!("tv_promise_resolve({}, &{rv});", p.code));
                } else {
                    self.line(format!("tv_promise_resolve_move({}, NULL);", p.code));
                }
                p
            }
            (Some("Promise"), "all" | "race") => {
                let pt = self.c.types.without_undefined(ty);
                let Ty::Promise(value, _) = self.tget(pt) else {
                    self.unsupported(span, "this promise");
                    return Val::plain("NULL", ty);
                };
                let arr = self.expr(args[0].expr);
                let arr = self.own(arr);
                if name == "all" {
                    let Ty::Array(elem) = self.tget(value) else { return Val::plain("NULL", ty) };
                    let (ed, ad) = (self.desc(elem), self.desc(value));
                    self.tmp(ty, &format!("tv_promise_all({}, {ed}, {ad})", arr.code), true)
                } else {
                    let ed = self.desc(value);
                    self.tmp(ty, &format!("tv_promise_race({}, {ed})", arr.code), true)
                }
            }
            (Some("JSON"), "stringify") => {
                let v = self.expr(args[0].expr);
                let indent = args.get(2).map(|a| self.expr(a.expr));
                if let Some(a) = args.get(1) {
                    let _ = self.expr(a.expr);
                }
                let r = self.json_stringify(v, indent);
                self.coerce(r, ty)
            }
            (Some("JSON"), "parse") => {
                let v = self.expr(args[0].expr);
                let v = self.coerce(v, STR);
                self.json_parse(v, ty)
            }
            (None, "String") => {
                let v = self.expr(args[0].expr);
                if v.ty == STR {
                    return v;
                }
                let sbv = self.fresh("sb");
                self.line(format!("tv_sb {sbv} = {{0}};"));
                self.line("{");
                self.bump(1);
                self.line(format!("tv_sb *sb = &{sbv};"));
                let c = self.string_code(v.ty, &v.code);
                self.line(format!("{c};"));
                self.bump(-1);
                self.line("}");
                self.tmp(STR, &format!("tv_str_from_sb(&{sbv})"), true)
            }
            (None, "Number") if self.ty(args[0].expr) == BIGINT => {
                let v = self.expr(args[0].expr);
                Val::plain(format!("tv_big_to_f64({})", v.code), F64)
            }
            (None, "Number") if self.ty(args[0].expr) == DECIMAL => {
                let v = self.expr(args[0].expr);
                Val::plain(format!("tv_dec_to_f64({})", v.code), F64)
            }
            (None, "decimal") => {
                let v = self.expr(args[0].expr);
                self.to_decimal(v, span)
            }
            (None, "BigInt") => {
                let v = self.expr(args[0].expr);
                self.to_bigint(v)
            }
            (Some("Array"), "isArray") => {
                let v = self.expr(args[0].expr);
                match self.tget(v.ty) {
                    Ty::Array(_) => Val::plain("true", BOOL),
                    Ty::Union(ms) => {
                        let ms = self.c.types.tys(ms).to_vec();
                        let tag = self.u_tag(&v.code, v.ty);
                        let arrays: Vec<String> = ms.iter().enumerate().filter(|(_, m)| matches!(self.tget(**m), Ty::Array(_))).map(|(i, _)| format!("{tag} == {i}")).collect();
                        Val::plain(if arrays.is_empty() { "false".to_string() } else { format!("({})", arrays.join(" || ")) }, BOOL)
                    }
                    _ => Val::plain("false", BOOL),
                }
            }
            (Some("BigInt"), "asIntN" | "asUintN") => {
                let bits = self.expr(args[0].expr);
                let bits = self.int_code(bits);
                let x = self.expr(args[1].expr);
                let loc = self.loc(span);
                self.tmp(BIGINT, &format!("tv_big_as_n({bits}, {}, {}, {loc})", x.code, name == "asIntN"), true)
            }
            (None, "Number") if !{
                let at = self.ty(args[0].expr);
                self.c.types.is_string(at)
            } => {
                // A number, or `number | string`: numbers convert, strings parse.
                let v = self.expr(args[0].expr);
                let (ok, out) = (self.fresh("ok"), self.fresh("n"));
                self.line(format!("double {out} = 0; bool {ok} = true;"));
                match self.tget(v.ty) {
                    Ty::Union(ms) => {
                        let ms = self.c.types.tys(ms).to_vec();
                        let tag = self.u_tag(&v.code, v.ty);
                        self.open(&format!("switch ({tag}) {{"));
                        for (i, &mt) in ms.iter().enumerate() {
                            let payload = self.u_payload(&v.code, v.ty, i);
                            if self.c.types.is_string(mt) {
                                let s = self.coerce(Val::plain(payload, mt), STR);
                                self.line(format!("case {i}: {ok} = tv_parse_float({}, &{out}); break;", s.code));
                            } else {
                                let n = self.coerce(Val::plain(payload, mt), F64);
                                self.line(format!("case {i}: {out} = {}; break;", n.code));
                            }
                        }
                        self.close("}");
                    }
                    _ => {
                        let n = self.coerce(v, F64);
                        self.line(format!("{out} = {};", n.code));
                    }
                }
                self.optional_from(&ok, Val::plain(out, F64), ty)
            }
            (None, "Number" | "parseFloat" | "parseInt") => {
                let v = self.expr(args[0].expr);
                let s = self.coerce(v, STR);
                let ok = self.fresh("ok");
                let out = self.fresh("n");
                if name == "parseInt" {
                    let radix = match args.get(1) {
                        Some(a) => {
                            let r = self.expr(a.expr);
                            self.int_code(r)
                        }
                        None => "0".into(),
                    };
                    self.line(format!("tv_int {out} = 0; bool {ok} = tv_parse_int({}, {radix}, &{out});", s.code));
                    return self.optional_from(&ok, Val::plain(out, INT), ty);
                }
                self.line(format!("double {out} = 0; bool {ok} = tv_parse_float({}, &{out});", s.code));
                self.optional_from(&ok, Val::plain(out, F64), ty)
            }
            // Timers.
            (None, "setTimeout" | "setInterval") => {
                let cb = self.expr(args[0].expr);
                let ms = match args.get(1) {
                    Some(a) => {
                        let v = self.expr(a.expr);
                        self.coerce(v, F64).code
                    }
                    None => "0".into(),
                };
                let repeat = name == "setInterval";
                self.tmp(INT, &format!("tv_set_timer({}, {ms}, {repeat})", cb.code), false)
            }
            (None, "clearTimeout" | "clearInterval") => {
                let v = self.expr(args[0].expr);
                if self.tget(v.ty) == Ty::Int {
                    self.line(format!("tv_clear_timer({});", v.code));
                } else {
                    let present = self.truthy_val(v.clone());
                    let id = self.project(v, INT);
                    self.line(format!("if ({present}) tv_clear_timer({});", id.code));
                }
                Val::plain("0", VOID)
            }
            (None, "queueMicrotask") => {
                let cb = self.expr(args[0].expr);
                self.line(format!("tv_queue_microtask({});", cb.code));
                Val::plain("0", VOID)
            }
            (None, "isNaN") => {
                let v = self.expr(args[0].expr);
                let v = self.coerce(v, F64);
                Val::plain(format!("isnan({})", v.code), BOOL)
            }
            (None, "expect") => {
                // Only used through a matcher (handled at the matcher call).
                let _ = self.expr(args[0].expr);
                Val::plain("0", ty)
            }
            (None, conv) => {
                let v = self.expr(args[0].expr);
                self.convert(v, conv, ty, span)
            }
            _ => {
                self.unsupported(span, "this built-in");
                Val::plain("0", ty)
            }
        }
    }

    /// `decimal(x)`: from an integer or bigint (exactly), a number (its shortest decimal form;
    /// NaN and the infinities trap), or a string (SyntaxError / RangeError).
    fn to_decimal(&mut self, v: Val, span: Span) -> Val {
        let loc = self.loc(span);
        match self.tget(v.ty) {
            Ty::Decimal => v,
            Ty::BigInt => Val::plain(format!("tv_dec_from_big({}, {loc})", v.code), DECIMAL),
            Ty::F64 | Ty::F32 => {
                let d = self.coerce(v, F64);
                let out = self.fresh("d");
                self.line(format!("tv_dec {out}; if (!tv_dec_from_f64({}, &{out})) tv_trap(\"decimal(): NaN and Infinity have no decimal value\", {loc});", d.code));
                Val::plain(out, DECIMAL)
            }
            Ty::Str | Ty::StrLit(_) => {
                let s = self.coerce(v, STR);
                let (out, rc) = (self.fresh("d"), self.fresh("r"));
                self.line(format!("tv_dec {out} = {{0, 0}}; int {rc} = tv_dec_parse({}, &{out});", s.code));
                self.open(&format!("if ({rc} != 0) {{"));
                for (code, cls, what) in [(1, "SyntaxError", "is not a number"), (2, "RangeError", "has more than 34 digits before the point")] {
                    let err = self.c.builtin_error_class(cls);
                    if err != ERROR {
                        let msg = format!("({{ tv_sb sb_ = {{0}}; tv_sb_push_cstr(&sb_, \"Cannot convert \"); tv_sb_push_str(&sb_, {}); tv_sb_push_cstr(&sb_, \" to a decimal: it {what}\"); tv_str_from_sb(&sb_); }})", s.code);
                        self.open(&format!("if ({rc} == {code}) {{"));
                        let obj = self.error_object(err, &msg);
                        self.line(format!("tvg_err = (void *)({obj});"));
                        self.close("}");
                    }
                }
                self.close("}");
                Val::plain(out, DECIMAL)
            }
            Ty::Union(ms) => {
                let ms = self.c.types.tys(ms).to_vec();
                let out = self.fresh("d");
                self.line(format!("tv_dec {out} = {{0, 0}};"));
                let tag = self.u_tag(&v.code, v.ty);
                self.open(&format!("switch ({tag}) {{"));
                for (i, &mt) in ms.iter().enumerate() {
                    self.open(&format!("case {i}: {{"));
                    self.push_temps_pub();
                    let payload = Val::plain(self.u_payload(&v.code, v.ty, i), mt);
                    let r = self.to_decimal(payload, span);
                    self.line(format!("{out} = {};", r.code));
                    self.pop_temps_pub();
                    self.line("break;");
                    self.close("}");
                }
                self.close("}");
                Val::plain(out, DECIMAL)
            }
            _ => self.coerce(v, DECIMAL),
        }
    }

    /// `BigInt(x)`: a bigint from an integer, a bool, a string (a SyntaxError unless it's one)
    /// or a number (a RangeError unless it's an integer), or a union of these.
    fn to_bigint(&mut self, v: Val) -> Val {
        match self.tget(v.ty) {
            Ty::BigInt => v,
            Ty::Decimal => {
                // an integral decimal, exactly (else a RangeError, as for BigInt(1.5))
                let out = self.tmp(BIGINT, "((tv_big){1})", true);
                let (dv, ok) = (self.fresh("d"), self.fresh("ok"));
                self.line(format!("tv_dec {dv} = tv_dec_trunc({0}); bool {ok} = tv_dec_eq({dv}, {0});", v.code));
                self.open(&format!("if ({ok}) {{"));
                self.line(format!("tv_str s_ = tv_dec_to_string({dv}); (void)tv_big_parse(s_, &{}); tv_str_release(s_);", out.code));
                self.close("}");
                self.open(&format!("if (!{ok}) {{"));
                let err = self.c.builtin_error_class("RangeError");
                if err != ERROR {
                    let msg = format!("({{ tv_sb sb_ = {{0}}; tv_sb_push_cstr(&sb_, \"The number \"); tv_sb_push_dec(&sb_, {}); tv_sb_push_cstr(&sb_, \" cannot be converted to a BigInt because it is not an integer\"); tv_str_from_sb(&sb_); }})", v.code);
                    let obj = self.error_object(err, &msg);
                    self.line(format!("tvg_err = (void *)({obj});"));
                }
                self.close("}");
                out
            }
            Ty::Bool => Val::plain(format!("(({}) ? ((tv_big){{3}}) : ((tv_big){{1}}))", v.code), BIGINT),
            Ty::U64 => self.tmp(BIGINT, &format!("tv_big_from_u64_({})", v.code), true),
            Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 => {
                let x = self.int_code(v);
                self.tmp(BIGINT, &format!("tv_big_from_i64({x})"), true)
            }
            Ty::Str | Ty::StrLit(_) => {
                let s = self.coerce(v, STR);
                let out = self.tmp(BIGINT, "((tv_big){1})", true);
                self.open(&format!("if (!tv_big_parse({}, &{})) {{", s.code, out.code));
                let err = self.c.builtin_error_class("SyntaxError");
                if err != ERROR {
                    let msg = format!("({{ tv_sb sb_ = {{0}}; tv_sb_push_cstr(&sb_, \"Cannot convert \"); tv_sb_push_str(&sb_, {}); tv_sb_push_cstr(&sb_, \" to a BigInt\"); tv_str_from_sb(&sb_); }})", s.code);
                    let obj = self.error_object(err, &msg);
                    self.line(format!("tvg_err = (void *)({obj});"));
                }
                self.close("}");
                out
            }
            Ty::F64 | Ty::F32 => {
                let d = self.coerce(v, F64);
                let dv = self.fresh("d");
                self.line(format!("double {dv} = {};", d.code));
                let out = self.tmp(BIGINT, "((tv_big){1})", true);
                self.open(&format!("if (!tv_big_from_f64({dv}, &{})) {{", out.code));
                let err = self.c.builtin_error_class("RangeError");
                if err != ERROR {
                    let msg = format!("({{ tv_sb sb_ = {{0}}; tv_sb_push_cstr(&sb_, \"The number \"); tv_sb_push_f64(&sb_, {dv}); tv_sb_push_cstr(&sb_, \" cannot be converted to a BigInt because it is not an integer\"); tv_str_from_sb(&sb_); }})");
                    let obj = self.error_object(err, &msg);
                    self.line(format!("tvg_err = (void *)({obj});"));
                }
                self.close("}");
                out
            }
            Ty::Union(ms) => {
                let ms = self.c.types.tys(ms).to_vec();
                let out = self.tmp(BIGINT, "((tv_big){1})", true);
                let tag = self.u_tag(&v.code, v.ty);
                self.open(&format!("switch ({tag}) {{"));
                for (i, &mt) in ms.iter().enumerate() {
                    self.open(&format!("case {i}: {{"));
                    self.push_temps_pub();
                    let payload = Val::plain(self.u_payload(&v.code, v.ty, i), mt);
                    let r = self.to_bigint(payload);
                    let code = self.consume(r);
                    self.line(format!("{} = {code};", out.code));
                    self.pop_temps_pub();
                    self.line("break;");
                    self.close("}");
                }
                self.close("}");
                out
            }
            _ => Val::plain("((tv_big){1})", BIGINT),
        }
    }

    fn convert(&mut self, v: Val, conv: &str, ty: TyId, span: Span) -> Val {
        if v.ty == DECIMAL {
            let loc = self.loc(span);
            if self.c.types.is_float(ty) {
                return self.coerce(Val::plain(format!("tv_dec_to_f64({})", v.code), F64), ty);
            }
            let x = format!("tv_dec_to_int({}, {loc})", v.code);
            if ty == INT {
                return Val::plain(x, ty);
            }
            return self.convert(Val::plain(x, INT), conv, ty, span);
        }
        if v.ty == BIGINT {
            let loc = self.loc(span);
            if self.c.types.is_float(ty) {
                return self.coerce(Val::plain(format!("tv_big_to_f64({})", v.code), F64), ty);
            }
            if ty == U64 {
                return Val::plain(format!("tv_big_to_u64({}, {loc})", v.code), ty);
            }
            let x = format!("tv_big_to_int({}, {loc})", v.code);
            if ty == INT {
                return Val::plain(x, ty);
            }
            return self.convert(Val::plain(x, INT), conv, ty, span);
        }
        if self.c.types.is_float(ty) {
            return self.coerce(v, ty);
        }
        let (lo, hi) = match conv {
            "int" | "i64" => ("INT64_MIN", "INT64_MAX"),
            "i8" => ("INT8_MIN", "INT8_MAX"),
            "i16" => ("INT16_MIN", "INT16_MAX"),
            "i32" => ("INT32_MIN", "INT32_MAX"),
            "u8" => ("0", "UINT8_MAX"),
            "u16" => ("0", "UINT16_MAX"),
            "u32" => ("0", "UINT32_MAX"),
            _ => ("0", "INT64_MAX"),
        };
        let ct = self.ctype(ty);
        let loc = self.loc(span);
        if matches!(self.tget(v.ty), Ty::U64) && ty != U64 {
            return Val::plain(format!("(({ct})({v}))", v = v.code), ty);
        }
        Val::plain(format!("TV_CONV({ct}, {}, {lo}, {hi}, {loc})", v.code), ty)
    }

    fn math(&mut self, name: &str, args: &[Arg], ty: TyId, span: Span) -> Val {
        let vals: Vec<Val> = args.iter().map(|a| self.expr(a.expr)).collect();
        let f64s: Vec<String> = vals.iter().map(|v| self.coerce(v.clone(), F64).code).collect();
        let loc = self.loc(span);
        let code = match name {
            "floor" | "ceil" | "trunc" => format!("tv_f64_to_int({name}({}), \"Math.{name}\", {loc})", f64s[0]),
            "round" => format!("tv_f64_to_int(tv_math_round({}), \"Math.round\", {loc})", f64s[0]),
            "abs" => {
                if self.c.types.is_int(ty) {
                    let ct = self.ctype(ty);
                    format!("({{ {ct} x_ = {}; x_ < 0 ? TV_NEG({ct}, x_, {loc}) : x_; }})", vals[0].code)
                } else {
                    format!("fabs({})", f64s[0])
                }
            }
            "min" | "max" => {
                if self.c.types.is_int(ty) {
                    let ct = self.ctype(ty);
                    let mut acc = vals[0].code.clone();
                    for v in &vals[1..] {
                        let op = if name == "min" { "<" } else { ">" };
                        acc = format!("({{ {ct} a_ = {acc}, b_ = {}; a_ {op} b_ ? a_ : b_; }})", v.code);
                    }
                    acc
                } else {
                    let mut acc = f64s[0].clone();
                    for v in &f64s[1..] {
                        acc = format!("tv_{name}_f64({acc}, {v})");
                    }
                    acc
                }
            }
            "random" => "tv_random()".into(),
            "sign" => format!("({{ double x_ = {}; x_ > 0 ? 1.0 : x_ < 0 ? -1.0 : x_; }})", f64s[0]),
            "atan2" | "pow" | "hypot" => format!("{name}({}, {})", f64s[0], f64s[1]),
            _ => format!("{name}({})", f64s[0]),
        };
        Val::plain(code, ty)
    }

    // ------------------------------------------------------------ test matchers

    fn matcher(&mut self, s: Val, name: &str, args: &[Arg], span: Span) {
        let loc = self.loc(span);
        let (cond, expected) = match name {
            "toBe" | "toEqual" => {
                let x = self.expr(args[0].expr);
                let x = self.coerce(x, s.ty);
                let eq = self.equality(s.clone(), x.clone(), true);
                (eq, Some(x))
            }
            "toBeCloseTo" => {
                let x = self.expr(args[0].expr);
                let x = self.coerce(x, F64);
                let digits = match args.get(1) {
                    Some(a) => {
                        let d = self.expr(a.expr);
                        self.coerce(d, F64).code
                    }
                    None => "2.0".into(),
                };
                let sv = self.coerce(s.clone(), F64);
                (format!("(fabs(({}) - ({})) < pow(10.0, -({digits})) / 2.0)", x.code, sv.code), Some(x))
            }
            "toBeUndefined" | "toBeDefined" => {
                let t = self.truthy_val(s.clone());
                (if name == "toBeUndefined" { format!("!({t})") } else { t }, None)
            }
            _ => ("true".into(), None),
        };
        self.line(format!("if (!({cond})) {{"));
        self.bump(1);
        self.line("tv_sb msg_ = {0}; tv_sb *sb = &msg_;");
        match &expected {
            Some(x) => {
                self.line("tv_sb_push_cstr(sb, \"expected \");");
                let c = self.inspect_root(x.ty, &x.code, "1");
                self.line(format!("{c};"));
                self.line("tv_sb_push_cstr(sb, \", received \");");
            }
            None => {
                let what = if name == "toBeUndefined" { "expected undefined, received " } else { "expected a defined value, received " };
                self.line(format!("tv_sb_push_cstr(sb, {});", c_string(what.as_bytes())));
            }
        }
        let c = self.inspect_root(s.ty, &s.code, "1");
        self.line(format!("{c};"));
        self.line(format!("tv_expect_fail(sb, {loc});"));
        self.bump(-1);
        self.line("}");
    }

    pub(crate) fn bump(&mut self, d: i32) {
        self.b().bump_indent(d);
    }
}
