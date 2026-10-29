//! Calls: module functions, closures, built-in methods, `console`, `Math`, conversions and test matchers.

use super::body::is_mutating_method;
use super::{c_string, Gen, Val};
use crate::ast::{Arg, BinOp, ExprId, ExprKind, StmtId};
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
    /// It can throw: checked after each call.
    throws: bool,
}

impl<'c, 'a> Gen<'c, 'a> {
    pub(crate) fn call(&mut self, e: ExprId) -> Val {
        let m = self.cur_m();
        // (a quiet callee: what's reached through fields stays alive for the call, unretained)
        let key = match &self.ast(m).expr(e).kind {
            ExprKind::Call { args, .. } => args.as_ptr() as usize,
            _ => 0,
        };
        let q = key != 0 && self.quiet_call(m, e);
        self.quiet_calls.push((key, q));
        let owned = if key != 0 { self.owned_args(m, e) } else { Vec::new() };
        self.owned_calls.push((key, owned));
        self.cur_calls.push((key, m, e));
        let v = self.call_inner(e);
        self.cur_calls.pop();
        self.owned_calls.pop();
        self.quiet_calls.pop();
        if self.facts(m).throwing.contains(&e) {
            self.error_check();
        }
        v
    }

    fn call_inner(&mut self, e: ExprId) -> Val {
        if let Some(v) = self.inline_replace(e) {
            return v;
        }
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
                if self.is_async_fn(fm, fi) {
                    let cname = self.instance(fm, fi, subst);
                    let argv = self.args(args, &params, Some((fm, fi)));
                    let ret = self.inst(fact.ret);
                    return self.spawn_call(&cname, &argv, ret, ty);
                }
                let mut argv = self.args(&args[..args.len().min(params.len())], &params, Some((fm, fi)));
                let direct = self.direct_args(fm, fi, args, &params);
                let cname = self.instance_with(fm, fi, subst, direct);
                if let Some(rest) = fact.rest {
                    // `...xs`: the remaining arguments become one array.
                    let rest = self.inst(rest);
                    let arr = self.pack_rest(&args[params.len().min(args.len())..], rest);
                    argv.push(arr);
                }
                // (the function's own result: an overload's, the call's type, may be narrower)
                let ret = self.inst(if fact.own_ret != ERROR { fact.own_ret } else { fact.ret });
                // `/re/g`: each evaluation makes a new RegExp; the site keeps its first, and gives
                // it out again whenever nothing else holds it (lastIndex back at 0, it's as good
                // as new: nothing can tell)
                if matches!(ast.expr(*callee).kind, ExprKind::Ident(f) if self.sym(f) == "__reLiteral")
                    && args.len() == 2
                    && args.iter().all(|a| matches!(ast.expr(a.expr).kind, ExprKind::Str(_)))
                    && self.c.class_of(ret).is_some()
                {
                    let ct = self.ctype(ret);
                    let (cache, v) = (self.fresh("rec"), self.fresh("re"));
                    let call = format!("{cname}({})", argv.join(", "));
                    // (through retain_code: the class is counted, so it keeps its header)
                    let rt = self.retain_code(ret, &v);
                    self.line(format!("static {ct} {cache}; {ct} {v};"));
                    self.line(format!(
                        "if ({cache} && ((tvg_obj *){cache})->rc == 1) {{ {v} = {cache}; {v}->f_lastIndex = 0; {rt}; }} else {{ {v} = {call}; if (!{cache}) {{ {cache} = {v}; {rt}; }} }}"
                    ));
                    let r = self.tmp(ret, &v, true);
                    return self.coerce(r, ty);
                }
                if ret != ty && ret != VOID && ret != NEVER && self.c.types.members(ret).len() > 1 && !self.c.types.members(ty).iter().any(|m| !self.c.types.members(ret).contains(m)) && self.c.types.members(ty).len() < self.c.types.members(ret).len() {
                    let v = self.tmp(ret, &format!("{cname}({})", argv.join(", ")), true);
                    return self.downcast(v, ty, span);
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
                // (`f(x)` of a `const f = (x) => ...` capturing nothing, a module's or a local:
                // its C function, called directly, where the C compiler can inline it)
                if !*optional
                    && let Some((cname, fps, fret)) = self.direct_arrow(*callee)
                {
                    let argv = self.args(args, &fps, None);
                    let mut all = vec!["NULL".to_string()];
                    all.extend(argv);
                    return self.finish_call(&format!("{cname}({})", all.join(", ")), fret, ty);
                }
                // (`f(x)` of a parameter always passed one arrow: its C function, called directly)
                if !*optional
                    && let Some(crate::check::IdentFact::Local(k)) = self.ident_fact(m, *callee)
                    && let Some(cname) = self.b().direct_fns.get(&k).cloned()
                    && let cty = self.ty(*callee)
                    && let Ty::Func(ps, fret, _) = self.tget(cty)
                {
                    let ps = self.c.types.params(ps).to_vec();
                    let fv = self.expr(*callee);
                    let fvn = self.fresh("fn");
                    self.line(format!("tv_fn {fvn} = {};", fv.code));
                    let argv = self.args(args, &ps, None);
                    let mut all = vec![format!("{fvn}.env")];
                    all.extend(argv);
                    return self.finish_call(&format!("{cname}({})", all.join(", ")), fret, ty);
                }
                let fv = self.expr(*callee);
                let fty = self.c.types.without_undefined(fv.ty);
                // `f?.(x)`: undefined when f is (its arguments unevaluated)
                if *optional && fty != fv.ty && self.c.types.has_undefined(ty) {
                    let present = self.truthy_val(fv.clone());
                    let res = self.fresh("t");
                    let ct = self.ctype(ty);
                    self.line(format!("{ct} {res};"));
                    self.open_block(&format!("if ({present}) {{"));
                    let f = self.project(fv, fty);
                    let inner = self.c.types.without_undefined(ty);
                    let r = self.call_fn_value(f, fty, args, inner, span);
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
                let fv = if fty != fv.ty { self.project(fv, fty) } else { fv };
                // (a class field's function: the closures stored there, tested for and called
                // directly, where the C compiler can inline them)
                let known = match &ast.expr(*callee).kind {
                    ExprKind::Member { obj, name, optional: false, .. } => {
                        let ot = self.ty(*obj);
                        let ot = self.c.types.without_nullish(ot);
                        if self.c.class_of(ot).is_some() && self.class_field(ot, *name).is_some() {
                            self.field_closures(*name).filter(|a| !a.is_empty() && a.len() <= 6)
                        } else {
                            None
                        }
                    }
                    _ => None,
                };
                self.call_fn_value_known(fv, fty, args, ty, span, known)
            }            Callee::Method(recv) => {
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
                    // (the method's own result: `m.get(k)` may be undefined itself)
                    let own = self.inst(fact.own_ret);
                    let inner_ret = if own != ERROR && own != VOID && self.c.types.has_undefined(own) { own } else { self.c.types.without_undefined(ty) };
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
                // (a union of string literals: a string)
                if matches!(self.tget(recv), Ty::Union(_)) && self.members(recv).iter().all(|&m| self.c.types.is_string(m)) {
                    let v = self.expr(*obj);
                    let v = self.coerce(v, STR);
                    return self.method_on(Some(v), *obj, STR, &name, args, &params, ty, span);
                }
                self.method_on(None, *obj, recv, &name, args, &params, ty, span)
            }
            Callee::ClassMethod { recv, name, sup } => {
                let recv = self.inst(recv);
                self.class_method_call(e, recv, name, sup, args, &params, ty, span)
            }
            Callee::StaticMethod(c, mi) => {
                let argv = self.args_rest(e, args, &params);
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
            Callee::NewOf(..) => {
                self.unsupported(span, "this call");
                Val::plain("0", ty)
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
            Callee::UnionMethod { recv, name } => {
                let recv = self.inst(recv);
                let ExprKind::Member { obj, .. } = &ast.expr(*callee).kind else {
                    self.unsupported(span, "this method call");
                    return Val::plain("0", ty);
                };
                let v = self.expr(*obj);
                let v = self.own(v);
                let v = self.project(v, recv);
                let has_rest = self.facts(self.cur_m()).calls.get(&e).is_some_and(|f| f.rest.is_some());
                // (each argument once, as the widest signature takes it; then as each member does)
                let (argv, held): (Vec<String>, Vec<Val>) = if has_rest {
                    (self.args_rest(e, args, &params), Vec::new())
                } else {
                    let mut held = Vec::new();
                    for (a, p) in args.iter().zip(&params) {
                        let x = self.expr(a.expr);
                        let x = self.coerce(x, p.ty);
                        let x = self.own(x);
                        held.push(x);
                    }
                    (Vec::new(), held)
                };
                let ms = self.members(recv);
                let tag = self.u_tag(&v.code, recv);
                let res = self.fresh("um");
                let ct = self.ctype(ty);
                self.line(format!("{ct} {res};"));
                self.open(&format!("switch ({tag}) {{"));
                for (i, &mt) in ms.iter().enumerate() {
                    self.open(&format!("case {i}: {{"));
                    self.push_temps();
                    let payload = self.u_payload(&v.code, recv, i);
                    let ret = match self.c.class_member(mt, name) {
                        Some(crate::check::class::ClassMemberRef::Method(mm)) => self.c.method_ret(&mm),
                        _ => VOID,
                    };
                    let mine = if has_rest {
                        argv.clone()
                    } else {
                        let mps = match self.c.class_member(mt, name) {
                            Some(crate::check::class::ClassMemberRef::Method(mm)) => mm.params.clone(),
                            _ => Vec::new(),
                        };
                        let mut out = Vec::new();
                        for (k, p) in mps.iter().enumerate() {
                            let x = match held.get(k) {
                                Some(h) => self.coerce(Val { owned: false, ..h.clone() }, p.ty),
                                None => self.coerce(Val::plain("0", UNDEFINED), p.ty),
                            };
                            out.push(x.code);
                        }
                        out
                    };
                    let code = self.method_call_code(&payload, mt, name, false, mine);
                    let r = self.finish_call_pub(&code, ret, if ty == VOID { VOID } else { ty });
                    if ty != VOID && ty != NEVER {
                        let r = self.own(r);
                        let c = self.consume(r);
                        self.line(format!("{res} = {c};"));
                    }
                    self.pop_temps();
                    self.line("break;");
                    self.close("}");
                }
                let dv = self.default_value(ty);
                if ty != VOID && ty != NEVER {
                    self.line(format!("default: {res} = {dv}; break;"));
                } else {
                    self.line("default: break;");
                }
                self.close("}");
                if ty == VOID || ty == NEVER {
                    return Val::plain("0", VOID);
                }
                let owned = self.is_rc(ty);
                if owned {
                    self.b().temps.last_mut().unwrap().push((res.clone(), ty));
                }
                Val { code: res, ty, owned }
            }
            Callee::Any { method } => {
                let loc = self.loc(span);
                match (&ast.expr(*callee).kind, method) {
                    (ExprKind::Member { obj, .. }, Some(name)) => {
                        let o = self.expr(*obj);
                        let n = self.sym(name).to_string();
                        self.any_call(o, Some(&n), args, ty, &loc)
                    }
                    _ => {
                        let f = self.expr(*callee);
                        self.any_call(f, None, args, ty, &loc)
                    }
                }
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
                // `expect(x).not.name(...)`: negated
                let (obj, negate) = match &ast.expr(*obj).kind {
                    ExprKind::Member { obj: inner, name: not, .. } if self.sym(*not) == "not" => (*inner, true),
                    _ => (*obj, false),
                };
                // `expect(x)`: evaluate x directly.
                let ExprKind::Call { args: eargs, .. } = &ast.expr(obj).kind else { return Val::plain("0", VOID) };
                let s = self.expr(eargs[0].expr);
                let s = self.coerce(s, subject);
                self.matcher(s, &name, args, span, negate);
                Val::plain("0", VOID)
            }
        }
    }

    /// Evaluates arguments against (instantiated) parameter types.
    fn args(&mut self, args: &[Arg], params: &[FnParam], callee: Option<(u32, u32)>) -> Vec<String> {
        let m = self.cur_m();
        // (a function value's rest parameter, `...xs: T[]`: the arguments from there on, packed)
        if let Some(e) = params.last().and_then(|p| self.c.types.rest_elem(p.ty)) {
            let k = params.len() - 1;
            let mut argv = self.args(&args[..args.len().min(k)], &params[..k], callee);
            let arr = self.pack_rest(&args[k.min(args.len())..], e);
            argv.push(arr);
            return argv;
        }
        let spreads_tuple = |s: &mut Self, a: &Arg| match s.ast(m).expr(a.expr).kind {
            ExprKind::Spread(inner) => {
                let t = s.ty(inner);
                s.c.types.tuple_elems(t).map(|_| inner)
            }
            _ => None,
        };
        if args.iter().any(|a| spreads_tuple(self, a).is_some()) {
            // (`f(...args)` with `args` a tuple: its elements, one parameter each)
            let mut out = Vec::new();
            for a in args {
                let at = out.len();
                let pty = |k: usize| params.get(k).map(|p| p.ty).unwrap_or(ERROR);
                if let Some(inner) = spreads_tuple(self, a) {
                    let v = self.expr(inner);
                    let v = if args.len() > 1 { self.snapshot(v) } else { v };
                    let tt = v.ty;
                    let elems = self.c.types.tuple_elems(tt).unwrap_or_default();
                    let syms = self.c.types.tuple_syms().to_vec();
                    for (k, ety) in elems.into_iter().enumerate() {
                        let f = Val::plain(format!("({}).f_{}", v.code, self.sym(syms[k])), ety);
                        let f = self.coerce(f, pty(at + k));
                        out.push(f.code);
                    }
                } else {
                    let v = self.expr(a.expr);
                    let v = if args.len() > 1 { self.snapshot(v) } else { v };
                    let v = self.coerce(v, pty(at));
                    out.push(v.code);
                }
            }
            for p in params.iter().skip(out.len()) {
                let v = self.coerce(Val::plain("0", UNDEFINED), p.ty);
                out.push(v.code);
            }
            return out;
        }
        let mut out = Vec::new();
        let has_inout = params.iter().any(|p| p.inout);
        let quiet = !args.is_empty() && self.quiet_calls.last().is_some_and(|&(k, q)| q && k == args.as_ptr() as usize);
        // (arguments the callee takes ownership of: handed over, see `owned_params`)
        let owned: Vec<bool> = match self.owned_calls.last() {
            Some((k, o)) if !args.is_empty() && *k == args.as_ptr() as usize => o.clone(),
            _ => Vec::new(),
        };
        for (i, a) in args.iter().enumerate() {
            let p = params.get(i).copied().unwrap_or(FnParam { ty: ERROR, inout: false, optional: false });
            if p.inout && a.by_ref.is_none() && !self.is_place_expr(a.expr) {
                // `walk(3, [])`: a fresh value, changed by the callee where only it sees it
                let v = self.expr(a.expr);
                let v = self.coerce(v, p.ty);
                let code = self.consume(v);
                let t = self.tmp(p.ty, &code, true);
                out.push(format!("&{}", t.code));
            } else if p.inout {
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
            } else if self.b().stack_arrows.contains(&a.expr) && matches!(self.ast(self.cur_m()).expr(a.expr).kind, ExprKind::Arrow(_)) {
                // (a callback the callee only calls: on the stack, its captures by address)
                let cb = self.prepare_callback(a.expr);
                let v = match cb.direct {
                    Some((cname, env)) => format!("((tv_fn){{ (void *){cname}, {env} }})"),
                    None => cb.value,
                };
                let at = self.ty(a.expr);
                let v = self.coerce(Val::plain(v, at), p.ty);
                out.push(v.code);
            } else if owned.get(i).copied().unwrap_or(false) && !args[i + 1..].iter().any(|later| self.changes_in(later.expr)) {
                let v = self.expr(a.expr);
                let v = self.coerce(v, p.ty);
                let code = self.consume(v);
                out.push(code);
            } else {
                // (a narrowed local given where its own type goes: the local, not its narrowed view
                // made that type again)
                let held = matches!(self.ast(self.cur_m()).expr(a.expr).kind, ExprKind::Ident(_)) && {
                    let m = self.cur_m();
                    matches!(self.ident_fact(m, a.expr), Some(crate::check::IdentFact::Local(k)) if self.b().locals.get(&k).is_some_and(|l| l.ty == p.ty))
                };
                let v = if held { self.expr_held(a.expr) } else { self.expr(a.expr) };
                // An argument before one that awaits (or assigns) is read first, as in JavaScript.
                let v = if args[i + 1..].iter().any(|later| self.changes_in(later.expr)) { self.snapshot(v) } else { v };
                // A value reached through a class instance (or a closure's cell) could be released by
                // the callee through another reference: keep it alive for the call.
                let v = if !v.owned && self.is_rc(v.ty) && (self.heap_rooted(a.expr) && !quiet && !self.call_spares_arg(args, v.ty) || (has_inout && self.is_place_expr(a.expr))) { self.own(v) } else { v };
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
            return self.tmp(arr_ty, "tv_arr_fresh()", true).code;
        }
        let elems: Vec<ExprId> = args.iter().map(|a| a.expr).collect();
        if elems.iter().any(|&x| matches!(self.ast(self.cur_m()).expr(x).kind, ExprKind::Spread(_))) {
            return self.build_spread(&elems, arr_ty).code;
        }
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
        // (the elements in the header's own block, as an array literal's: one allocation)
        let arr = self.tmp(arr_ty, &format!("tv_arr_new_len(sizeof({ect}), {})", codes.len()), true);
        let w = self.fresh("w");
        self.line(format!("{ect} *{w} = ({ect} *)tv_arr_data({});", arr.code));
        for (i, c) in codes.iter().enumerate() {
            self.line(format!("{w}[{i}] = {c};"));
        }
        arr.code
    }

    pub(crate) fn args_pub(&mut self, args: &[Arg], params: &[FnParam]) -> Vec<String> {
        self.args(args, params, None)
    }

    /// The arguments of call `e`, the ones for a rest parameter packed into one array.
    pub(crate) fn args_rest(&mut self, e: ExprId, args: &[Arg], params: &[FnParam]) -> Vec<String> {
        let m = self.cur_m();
        let rest = self.facts(m).calls.get(&e).and_then(|f| f.rest);
        let Some(rest) = rest else { return self.args(args, params, None) };
        let mut argv = self.args(&args[..args.len().min(params.len())], params, None);
        let rest = self.inst(rest);
        let arr = self.pack_rest(&args[params.len().min(args.len())..], rest);
        argv.push(arr);
        argv
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
        let Ty::Func(ps, ret, th) = self.tget(fty) else {
            return Callback { direct: None, value: "((tv_fn){NULL, NULL})".into(), params: Vec::new(), ret: VOID, throws: false };
        };
        let throws = th != NEVER;
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
                self.line(format!("{} {ev} = {{ {{-1, 0, NULL}}, {} }};", info.env_type, ptrs.join(", ")));
                format!("(tv_env *)&{ev}")
            };
            return Callback { direct: Some((info.cname, env)), value: String::new(), params, ret, throws };
        }
        // (a `const` arrow capturing nothing, `xs.map(identity)`: its C function, called directly)
        if let Some(cname) = self.const_arrow_fn(arg, &params, ret) {
            return Callback { direct: Some((cname, "NULL".into())), value: String::new(), params, ret, throws };
        }
        let v = self.expr(arg);
        let fvn = self.fresh("cb");
        self.line(format!("tv_fn {fvn} = {};", v.code));
        Callback { direct: None, value: fvn, params, ret, throws }
    }

    /// `f` naming a `const f = (...) => ...` that captures nothing (a module's, or a local's made
    /// as one C function): that function, its parameters and its result.
    /// The parameters of a call of function (fm, fi) its instance can be specialized for: each
    /// passed an arrow written here (or a parameter of this instance already fixed to one), of
    /// exactly the parameter's C type, and never assigned in the function. (Sync functions only,
    /// and at most 64 such instances of a function.)
    fn direct_args(&mut self, fm: u32, fi: u32, args: &[Arg], params: &[FnParam]) -> Vec<(usize, String)> {
        if self.is_gen_fn(fm, fi) || self.is_async_fn(fm, fi) {
            return Vec::new();
        }
        let m = self.cur_m();
        let mut out = Vec::new();
        for (i, a) in args.iter().enumerate().take(params.len()) {
            let pt = params[i].ty;
            if params[i].inout || a.by_ref.is_some() {
                continue;
            }
            let Ty::Func(ps, ret, _) = self.tget(pt) else { continue };
            let ps = self.c.types.params(ps).to_vec();
            let want = self.fn_ptr_type(&ps, ret);
            let cname = match &self.ast(m).expr(a.expr).kind {
                ExprKind::Arrow(f) if !f.is_async => self.arrow_last.get(&(m, a.expr)).filter(|(_, cast)| *cast == want).map(|(c, _)| c.clone()),
                ExprKind::Ident(_) if self.ty(a.expr) == pt => match self.ident_fact(m, a.expr) {
                    Some(crate::check::IdentFact::Local(k)) => self.b().direct_fns.get(&k).cloned(),
                    _ => None,
                },
                _ => None,
            };
            let Some(cname) = cname else { continue };
            if !self.param_unassigned(fm, fi, i) {
                continue;
            }
            out.push((i, cname));
        }
        if !out.is_empty() {
            let n = self.direct_counts.entry((fm, fi)).or_default();
            if *n >= 64 {
                return Vec::new();
            }
            *n += 1;
        }
        out
    }

    /// Whether function (fm, fi) never assigns its parameter i (nor a nested closure does).
    fn param_unassigned(&mut self, fm: u32, fi: u32, i: usize) -> bool {
        if let Some(v) = self.unassigned_params.get(&(fm, fi)) {
            return v.get(i).copied().unwrap_or(false);
        }
        let crate::ast::ItemKind::Function(f) = &self.ast(fm).items[fi as usize].kind else { return false };
        let (keys, body): (Vec<u32>, StmtId) = (f.params.iter().map(|p| p.span.start).collect(), f.body);
        let (_, _, mutated) = self.analyze_closures_pub(fm, &super::body::FnBodyKind::Block(body));
        let v: Vec<bool> = keys.iter().map(|k| !mutated.contains(k)).collect();
        let r = v.get(i).copied().unwrap_or(false);
        self.unassigned_params.insert((fm, fi), v);
        r
    }

    fn direct_arrow(&mut self, f: ExprId) -> Option<(String, Vec<FnParam>, TyId)> {
        let m = self.cur_m();
        if !matches!(self.ast(m).expr(f).kind, ExprKind::Ident(_)) {
            return None;
        }
        let fty = self.ty(f);
        let Ty::Func(ps, ret, th) = self.tget(fty) else { return None };
        if th != NEVER {
            return None;
        }
        let params = self.c.types.params(ps).to_vec();
        match self.ident_fact(m, f) {
            Some(crate::check::IdentFact::Const(cm, ci)) => {
                let crate::ast::ItemKind::Const { init, mutable: false, .. } = &self.ast(cm).items[ci as usize].kind else { return None };
                let init = *init;
                if !matches!(&self.ast(cm).expr(init).kind, ExprKind::Arrow(a) if !a.is_async) {
                    return None;
                }
                // (the module variable made, and with it its arrow's function)
                let _ = self.const_ref(cm, ci);
                let fns = self.arrow_fns.get(&(cm, init))?.clone();
                let want = self.fn_ptr_type(&params, ret);
                match fns.as_slice() {
                    [(cname, cast)] if *cast == want => Some((cname.clone(), params, ret)),
                    _ => None,
                }
            }
            Some(crate::check::IdentFact::Local(_)) => self.const_arrow_fn(f, &params, ret).map(|c| (c, params, ret)),
            _ => None,
        }
    }

    /// `f` a local `const f = (...) => ...` whose arrow captures nothing and was made as one C
    /// function of this type: that function.
    fn const_arrow_fn(&mut self, f: ExprId, params: &[FnParam], ret: TyId) -> Option<String> {
        let m = self.cur_m();
        if !matches!(self.ast(m).expr(f).kind, ExprKind::Ident(_)) {
            return None;
        }
        let Some(crate::check::IdentFact::Local(k)) = self.ident_fact(m, f) else { return None };
        if !self.const_arrows.contains_key(&m) {
            let ast = self.ast(m);
            let mut all = FxMap::default();
            for st in ast.stmts.iter() {
                if let crate::ast::StmtKind::Let { mutable: false, name_span, init: Some(i), .. } = &st.kind
                    && matches!(&ast.expr(*i).kind, ExprKind::Arrow(a) if !a.is_async)
                {
                    all.insert(name_span.start, *i);
                }
            }
            self.const_arrows.insert(m, all);
        }
        let ae = *self.const_arrows[&m].get(&k)?;
        if !crate::codegen::body::arrow_captures(self.ast(m), self.facts(m), ae).is_empty() {
            return None;
        }
        let fns = self.arrow_fns.get(&(m, ae))?.clone();
        let want = self.fn_ptr_type(params, ret);
        match fns.as_slice() {
            [(cname, cast)] if *cast == want => Some(cname.clone()),
            _ => None,
        }
    }

    /// Calls a prepared callback with (borrowed) arguments; extra arguments are dropped.
    /// A call of the function value `fv` (of function type `fty`).
    fn call_fn_value(&mut self, fv: Val, fty: TyId, args: &[Arg], ty: TyId, span: Span) -> Val {
        self.call_fn_value_known(fv, fty, args, ty, span, None)
    }

    /// A call of function value `fv`; `known`: the arrows it can be (see `devirt_helpers`).
    fn call_fn_value_known(&mut self, fv: Val, fty: TyId, args: &[Arg], ty: TyId, span: Span, known: Option<Vec<(u32, ExprId)>>) -> Val {
        // (a union of functions: the arguments once, then the one it holds called with them, as
        // its parameters take them)
        if let Ty::Union(ms) = self.tget(fty)
            && self.c.types.tys(ms).iter().all(|&m| matches!(self.tget(m), Ty::Func(..)))
            && niche_free(self, fty)
        {
            let ms = self.c.types.tys(ms).to_vec();
            let fv = self.own(fv);
            let held: Vec<Val> = args
                .iter()
                .map(|a| {
                    let v = self.expr(a.expr);
                    self.own(v)
                })
                .collect();
            let ct = self.ctype(ty);
            let res = self.fresh("uc");
            if ty != VOID {
                self.line(format!("{ct} {res};"));
            }
            let tag = self.u_tag(&fv.code, fty);
            self.open(&format!("switch ({tag}) {{"));
            for (i, &m) in ms.iter().enumerate() {
                let Ty::Func(mps, mret, _) = self.tget(m) else { continue };
                let mps = self.c.types.params(mps).to_vec();
                self.open(&format!("case {i}: {{"));
                self.push_temps();
                let f = Val::plain(self.u_payload(&fv.code, fty, i), m);
                let mut argv = Vec::new();
                for (k, p) in mps.iter().enumerate() {
                    let code = match held.get(k) {
                        Some(h) => {
                            let cv = self.coerce(Val { owned: false, ..h.clone() }, p.ty);
                            cv.code
                        }
                        None => self.coerce(Val::plain("0", UNDEFINED), p.ty).code,
                    };
                    argv.push(code);
                }
                let cast = self.fn_ptr_type(&mps, mret);
                let fvn = self.fresh("fn");
                self.line(format!("tv_fn {fvn} = {};", f.code));
                let mut all = vec![format!("{fvn}.env")];
                all.extend(argv);
                let r = self.finish_call(&format!("(({cast}){fvn}.fn)({})", all.join(", ")), mret, if ty == VOID { VOID } else { ty });
                if ty != VOID {
                    let r = self.coerce(r, ty);
                    let r = self.own(r);
                    let code = self.consume(r);
                    self.line(format!("{res} = {code};"));
                } else if !r.code.is_empty() && r.code != "0" {
                    self.line(format!("(void)({});", r.code));
                }
                self.pop_temps();
                self.line("break;");
                self.close("}");
            }
            if ty != VOID {
                let dv = self.default_value(ty);
                self.line(format!("default: {res} = {dv}; break;"));
            } else {
                self.line("default: break;");
            }
            self.close("}");
            if ty == VOID {
                return Val::plain("0", VOID);
            }
            return self.tmp(ty, &res, true);
        }
        let Ty::Func(ps, ret, _) = self.tget(fty) else {
            self.unsupported(span, "calling this value");
            return Val::plain("0", ty);
        };
        let ps = self.c.types.params(ps).to_vec();
        let argv = self.args(args, &ps, None);
        let fvn = self.fresh("fn");
        self.line(format!("tv_fn {fvn} = {};", fv.code));
        let cast = self.fn_ptr_type(&ps, ret);
        if let Some(arrows) = known {
            let name = self.fresh("dv");
            let rct = if ret == VOID { "void".to_string() } else { self.ctype(ret) };
            let pts: Vec<String> = ps.iter().map(|p| {
                let ct = self.ctype(p.ty);
                if p.inout { format!("{ct} *") } else { ct }
            }).collect();
            self.devirt.push((name.clone(), arrows, cast, rct, pts));
            let mut all = vec![fvn];
            all.extend(argv);
            return self.finish_call(&format!("{name}({})", all.join(", ")), ret, ty);
        }
        let mut all = vec![format!("{fvn}.env")];
        all.extend(argv);
        self.finish_call(&format!("(({cast}){fvn}.fn)({})", all.join(", ")), ret, ty)
    }

    /// The helpers of calls through class fields of a function type (see `field_closures`):
    /// each tests the function for the closures that can be stored there, in each of their C
    /// functions of the call's type, and calls the one it is directly; anything else (a closure
    /// adapted to the field's type, say) is called through its pointer.
    pub(crate) fn devirt_helpers(&mut self) {
        let sites = std::mem::take(&mut self.devirt);
        // (a closure called directly from one or two of these is inlined into them: it's
        // declared again, always inline, before them; its own function stays, for other calls)
        let mut uses: FxMap<String, usize> = FxMap::default();
        for (_, arrows, cast, _, _) in &sites {
            for a in arrows {
                for (fname, fcast) in self.arrow_fns.get(a).cloned().unwrap_or_default() {
                    if &fcast == cast {
                        *uses.entry(fname).or_default() += 1;
                    }
                }
            }
        }
        let mut inlined: Vec<String> = uses.iter().filter(|(_, n)| **n <= 2).map(|(f, _)| f.clone()).collect();
        inlined.sort();
        for fname in inlined {
            if let Some(proto) = self.arrow_protos.get(&fname) {
                let _ = std::fmt::Write::write_fmt(&mut self.helpers, format_args!("static inline __attribute__((always_inline)) {proto};\n"));
            }
        }
        for (name, arrows, cast, rct, pts) in sites {
            let params: Vec<String> = pts.iter().enumerate().map(|(i, t)| format!("{t} a{i}")).collect();
            let args: Vec<String> = std::iter::once("f.env".to_string()).chain((0..pts.len()).map(|i| format!("a{i}"))).collect();
            let call_args = args.join(", ");
            let ret = if rct == "void" { "" } else { "return " };
            let done = if rct == "void" { " return;" } else { "" };
            let mut body = String::new();
            for a in &arrows {
                for (fname, fcast) in self.arrow_fns.get(a).cloned().unwrap_or_default() {
                    if fcast == cast {
                        let _ = std::fmt::Write::write_fmt(&mut body, format_args!("    if (f.fn == (void *){fname}) {{ {ret}{fname}({call_args});{done} }}\n"));
                    }
                }
            }
            let _ = std::fmt::Write::write_fmt(&mut self.helpers, format_args!("static inline __attribute__((always_inline)) {rct} {name}(tv_fn f{}{}) {{\n{body}    {ret}(({cast})f.fn)({call_args});\n}}\n", if params.is_empty() { "" } else { ", " }, params.join(", ")));
        }
    }

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
            if cb.throws {
                self.error_check();
            }
            return Val::plain("0", VOID);
        }
        let v = self.tmp(cb.ret, &call, true);
        if cb.throws {
            self.error_check();
        }
        v
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
        let mut access = String::new();
        for step in &lp {
            let rec = self.c.unfold(cur);
            let Ty::Record(fs) = self.tget(rec) else { return false };
            let Some(f) = self.c.types.fields(fs).iter().find(|f| f.name == *step).copied() else { return false };
            if matches!(self.tget(cur), Ty::Rec(..)) {
                return false;
            }
            access.push_str(&format!("{}f_{}", if self.is_obj(rec) { "->v." } else { "." }, self.sym(*step)));
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
        // (a comparator that threw: the rest compare equal, and the caller passes the error on)
        let cmp = if cb.throws { format!("(tvg_err ? 0.0 : {cmp})") } else { cmp };
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
    {ect} *src = a; {ect} *dst = buf;
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

    /// `xs` is a local made as a `Float64Array` (`new Float64Array(n)`, `Float64Array.from(...)`),
    /// which Tov holds as an `f64[]`.
    fn float64_array(&self, obj: ExprId) -> bool {
        let m = self.cur_m();
        let ast = self.ast(m);
        let made = |e: ExprId| match &ast.expr(e).kind {
            ExprKind::New { callee, .. } => matches!(ast.expr(*callee).kind, ExprKind::Ident(n) if self.sym(n) == "Float64Array"),
            ExprKind::Call { callee, .. } => matches!(&ast.expr(*callee).kind, ExprKind::Member { obj, .. } if matches!(ast.expr(*obj).kind, ExprKind::Ident(n) if self.sym(n) == "Float64Array")),
            _ => false,
        };
        match &ast.expr(obj).kind {
            ExprKind::Paren(x) => return self.float64_array(*x),
            // (`xs.slice()` and the like of one: one too)
            ExprKind::Call { callee, .. } if let ExprKind::Member { obj: inner, name, .. } = &ast.expr(*callee).kind
                && matches!(self.sym(*name), "slice" | "subarray" | "map" | "filter" | "reverse" | "sort" | "fill") =>
            {
                return self.float64_array(*inner);
            }
            _ if made(obj) => return true,
            _ => {}
        }
        let Some(crate::check::IdentFact::Local(k)) = self.ident_fact(m, obj) else { return false };
        let is_f64 = |e: ExprId| match &ast.expr(e).kind {
            ExprKind::New { callee, .. } => matches!(ast.expr(*callee).kind, ExprKind::Ident(n) if self.sym(n) == "Float64Array"),
            ExprKind::Call { callee, .. } => matches!(&ast.expr(*callee).kind, ExprKind::Member { obj, .. } if matches!(ast.expr(*obj).kind, ExprKind::Ident(n) if self.sym(n) == "Float64Array")),
            _ => false,
        };
        ast.stmts.iter().any(|st| match &st.kind {
            crate::ast::StmtKind::Let { name_span, init: Some(i), ty, .. } if name_span.start == k => {
                is_f64(*i) || ty.is_some_and(|t| matches!(&ast.ty(t).kind, crate::ast::TypeExprKind::Named { name, .. } if self.sym(*name) == "Float64Array"))
            }
            _ => false,
        })
    }

    /// A built-in method call. `given` is an already-evaluated receiver (for `x?.m()`).
    #[allow(clippy::too_many_arguments)]
    fn method_on(&mut self, given: Option<Val>, obj: ExprId, recv: TyId, name: &str, args: &[Arg], params: &[FnParam], ty: TyId, span: Span) -> Val {
        // (a builder's push and join: see `builder_local`)
        if given.is_none()
            && let Some(crate::check::IdentFact::Local(k)) = self.ident_fact(self.cur_m(), obj)
            && let Some((sb, n, sep)) = self.b().builders.get(&k).cloned()
        {
            if name == "push" {
                let v = self.expr(args[0].expr);
                let v = self.coerce(v, STR);
                if !sep.is_empty() {
                    let lit = c_string(sep.as_bytes());
                    self.line(format!("if ({n} > 0) tv_sb_push(&{sb}, {lit}, {});", sep.len()));
                }
                self.line(format!("tv_sb_push_str(&{sb}, {}); {n}++;", v.code));
                return Val::plain(n, INT);
            }
            // (`return parts.join("")`: the builder isn't read again, its buffer becomes the
            // string; a `finally` could still push, so not inside one)
            if self.return_span == Some(span) && self.b().finallies.is_empty() {
                return self.tmp(STR, &format!("tv_str_from_sb(&{sb})"), true);
            }
            return self.tmp(STR, &format!("tv_str_from({sb}.data ? {sb}.data : \"\", {sb}.len)"), true);
        }
        // `new Array<T>(n).fill(v)`: made with n copies of v, each element written once
        if given.is_none() && name == "fill" && args.len() == 1
            && !matches!(self.ast(self.cur_m()).expr(args[0].expr).kind, ExprKind::Spread(_))
            && let Some(v) = self.new_filled(obj, recv, args[0].expr, span)
        {
            return v;
        }
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
            // (a typed array's `sort()`: by number, not as text)
            if name == "sort"
                && args.is_empty()
                && let Ty::Array(et) = self.tget(recv)
                && let Some(kind) = match self.tget(et) {
                    Ty::I8 => Some(0),
                    Ty::U8 | Ty::U8C => Some(1),
                    Ty::I16 => Some(2),
                    Ty::U16 => Some(3),
                    Ty::I32 => Some(4),
                    Ty::U32 => Some(5),
                    Ty::F32 => Some(6),
                    Ty::F64 if self.float64_array(obj) => Some(7),
                    _ => None,
                }
            {
                let d = self.desc(et);
                self.line(format!("tvg_make_unique(&{lv}, {d}); tv_sort_typed(tv_arr_data({lv}), tv_arr_len({lv}), {kind});"));
                let v = Val::plain(lv, recv);
                let v = self.own(v);
                return self.coerce(v, ty);
            }
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
        // `xs.map(f).filter(g).reduce(h, 0)`: one loop, without the arrays between
        if given.is_none()
            && matches!(self.tget(recv), Ty::Array(_))
            && let Some(v) = self.fused_chain(obj, name, args, ty)
        {
            return v;
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
            Ty::Gen(e) if name == "next" => self.gen_next(rv, e, ty),
            Ty::AsyncGen(e) if name == "next" || name == "read" => self.agen_next(rv, e, ty),
            Ty::AsyncGen(_) if name == "cancel" || name == "return" => {
                self.line(format!("({})->done = true;", rv.code));
                Val::plain("0", VOID)
            }
            Ty::AsyncGen(_) if name == "releaseLock" => Val::plain("0", VOID),
            Ty::AsyncGen(_) if name == "getReader" => {
                let r = self.own(rv);
                Val { ty, ..r }
            }
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
                "toPrecision" => {
                    let x = self.coerce(rv, F64);
                    let Some(a) = args.first() else { return self.tmp(STR, &format!("tv_str_from_f64({})", x.code), true) };
                    let d = self.expr(a.expr);
                    let oi = self.c.types.optional(INT);
                    let d = self.coerce(d, oi);
                    let (d, present) = self.optional_int(&d);
                    let loc = self.loc(span);
                    self.tmp(STR, &format!("({present} ? tv_f64_to_precision({}, {d}, {loc}) : tv_str_from_f64({}))", x.code, x.code), true)
                }
                "toExponential" => {
                    let x = self.coerce(rv, F64);
                    let loc = self.loc(span);
                    let Some(a) = args.first() else { return self.tmp(STR, &format!("tv_f64_to_exponential({}, 0, false, {loc})", x.code), true) };
                    let d = self.expr(a.expr);
                    let oi = self.c.types.optional(INT);
                    let d = self.coerce(d, oi);
                    let (d, present) = self.optional_int(&d);
                    self.tmp(STR, &format!("tv_f64_to_exponential({}, {d}, {present}, {loc})", x.code), true)
                }
                "toLocaleString" => {
                    let (min, max) = crate::check::builtins::locale_digits(self.ast(self.cur_m()), self.c.interner, args).unwrap_or((0, 3));
                    // (the arguments are literals: nothing to evaluate)
                    if self.c.types.is_int(recv) {
                        let x = self.int_code(rv);
                        self.tmp(STR, &format!("tv_int_to_locale({x}, {min})"), true)
                    } else {
                        let x = self.coerce(rv, F64);
                        self.tmp(STR, &format!("tv_f64_to_locale({}, {min}, {max})", x.code), true)
                    }
                }
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
                _ if !args.is_empty() => {
                    // toString(radix), as BigInt's: an absent optional radix is 10
                    let r = self.expr(args[0].expr);
                    let present = self.truthy_val(r.clone());
                    let r = self.project(r, INT);
                    let radix = if present == "true" { r.code } else { format!("({present} ? {} : 10)", r.code) };
                    let loc = self.loc(span);
                    if self.c.types.is_int(recv) {
                        let x = self.int_code(rv);
                        self.tmp(STR, &format!("tv_int_to_radix({x}, {radix}, {loc})"), true)
                    } else {
                        let x = self.coerce(rv, F64);
                        self.tmp(STR, &format!("tv_f64_to_radix({}, {radix}, {loc})", x.code), true)
                    }
                }
                _ => {
                    if self.c.types.is_int(recv) {
                        let x = self.int_code(rv);
                        let f = if recv == INT && self.js_ints(self.cur_m()) { "tv_str_from_jint" } else { "tv_str_from_int" };
                        self.tmp(STR, &format!("{f}({x})"), true)
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
            (Ty::Array(et), "splice") => {
                let d = self.desc(et);
                let start = self.expr(args[0].expr);
                let start = self.int_code(start);
                let (del, has) = match args.get(1) {
                    Some(a) => {
                        let v = self.expr(a.expr);
                        let oi = self.c.types.optional(INT);
                        let v = self.coerce(v, oi);
                        self.optional_int(&v)
                    }
                    None => ("0".to_string(), "false".to_string()),
                };
                let arr = self.c.types.array(et);
                let items = if args.len() > 2 {
                    let elems: Vec<ExprId> = args[2..].iter().map(|a| a.expr).collect();
                    self.build_spread(&elems, arr).code
                } else {
                    "TV_EMPTY_ARR".to_string()
                };
                let (sv, dv) = (self.fresh("s"), self.fresh("d"));
                self.line(format!("tv_int {sv} = {start}; tv_int {dv} = {del};"));
                self.tmp(arr, &format!("tv_arr_splice(&{lv}, {d}, {sv}, {dv}, {has}, {items})"), true)
            }
            (Ty::Array(et), "push" | "unshift") => {
                let d = self.desc(et);
                let ect = self.ctype(et);
                if args.iter().any(|a| matches!(self.ast(self.cur_m()).expr(a.expr).kind, ExprKind::Spread(_))) {
                    // `xs.push(...ys)`: the arguments as one array, appended (or prepended)
                    let arr = self.c.types.array(et);
                    let elems: Vec<ExprId> = args.iter().map(|a| a.expr).collect();
                    let all = self.build_spread(&elems, arr);
                    if name == "push" {
                        self.line(format!("tv_arr_extend(&{lv}, {d}, {});", all.code));
                        // (a capacity cached for a loop's pushes)
                        let b = self.b();
                        let cap = b.unique.iter().copied().find(|k| b.locals.get(k).is_some_and(|l| l.access == lv)).and_then(|k| b.push_caps.get(&k).cloned());
                        if let Some(cv) = cap {
                            self.line(format!("{cv} = {lv}.p ? {lv}.p->cap : 0;"));
                        }
                    } else {
                        let joined = self.fresh("j");
                        self.line(format!("tv_arr {joined} = tv_arr_concat({}, {lv}, {d}); tv_arr_release({lv}, {d}); {lv} = {joined};", all.code));
                    }
                    return Val::plain(format!("tv_arr_len({lv})"), INT);
                }
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
                if name == "pop" {
                    // (inline: the last element moved out; an array with elements is its own, the
                    // immortal empty one having none)
                    let pb = self.fresh("pb");
                    self.line(format!("{ect} {out}; bool {ok}; {{ tv_arrbuf *{pb} = ({lv}).p; {ok} = {pb}->len > 0; if ({ok}) {out} = (({ect} *)(void *)tv_arrbuf_data({pb}))[--{pb}->len]; }}"));
                } else {
                    self.line(format!("{ect} {out}; bool {ok} = tv_arr_{name}(&{lv}, {d}, &{out});"));
                }
                self.optional_from(&ok, Val { code: out, ty: et, owned: true }, ty)
            }
            // (a typed array's `set`: the source's elements copied in at the offset)
            (Ty::Array(et), "set") => {
                let d = self.desc(et);
                let src = self.expr(args[0].expr);
                let src = self.coerce(src, recv);
                let src = self.own(src);
                let code = self.consume(src);
                let s = self.fresh("s");
                self.line(format!("tv_arr {s} = {code};"));
                let off = match args.get(1) {
                    Some(a) => {
                        let o = self.expr(a.expr);
                        let o = self.coerce(o, INT);
                        o.code
                    }
                    None => "0".into(),
                };
                let err = self.c.builtin_error_class("RangeError");
                if err != ERROR {
                    self.open(&format!("if (!tv_arr_set_from(&{lv}, {d}, {s}, {off})) {{"));
                    let msg = self.lit(b"offset is out of bounds");
                    let obj = self.error_object(err, &msg);
                    self.line(format!("tvg_err = (void *)({obj});"));
                    self.close("}");
                } else {
                    let loc = self.loc(span);
                    self.line(format!("if (!tv_arr_set_from(&{lv}, {d}, {s}, {off})) tv_trap(\"RangeError: offset is out of bounds\", {loc});"));
                }
                let r = self.release_code(recv, &s);
                if !r.is_empty() {
                    self.line(format!("{r};"));
                }
                Val::plain("0", ty)
            }
            (Ty::Array(et), "fill") => {
                let (d, ect) = (self.desc(et), self.ctype(et));
                let v = self.expr(args[0].expr);
                let v = self.coerce(v, et);
                let code = self.consume(v);
                let x = self.fresh("x");
                self.line(format!("{ect} {x} = {code};"));
                let bound = |g: &mut Self, i: usize| -> (String, String) {
                    match args.get(i) {
                        Some(a) => {
                            let b = g.expr(a.expr);
                            let oi = g.c.types.optional(INT);
                            let b = g.coerce(b, oi);
                            g.optional_int(&b)
                        }
                        None => ("0".into(), "false".into()),
                    }
                };
                let (s0, hs) = bound(self, 1);
                let (e0, he) = bound(self, 2);
                self.line(format!("tv_arr_fill(&{lv}, {d}, &{x}, {s0}, {hs}, {e0}, {he});"));
                let r = self.release_code(et, &x);
                if !r.is_empty() {
                    self.line(format!("{r};"));
                }
                let v = Val::plain(lv.to_string(), recv);
                let v = self.own(v);
                self.coerce(v, ty)
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
                        if cb.throws {
                            self.error_check();
                        }
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
                // (the map, borrowed: nothing to release when it isn't used)
                let me = Val::plain(lv.to_string(), recv);
                self.coerce(me, ty)
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
                let me = Val::plain(lv.to_string(), recv);
                self.coerce(me, ty)
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

    /// An arrow whose calls, one element at a time, can't be told apart from calls one array
    /// at a time: it changes nothing and can't trap (no indexing, `!`, casts or bigint
    /// division), takes the element and maybe its index, and doesn't throw.
    fn fusible_arrow(&self, e: ExprId) -> bool {
        let m = self.cur_m();
        let ast = self.ast(m);
        let ExprKind::Arrow(f) = &ast.expr(e).kind else { return false };
        if f.is_async || f.throws.is_some() || f.params.len() > 2 || f.params.iter().any(|p| p.inout || p.optional || p.rest) {
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
        let ty_of = |x: ExprId| self.facts(m).expr_ty.get(x as usize).copied().unwrap_or(ERROR);
        if !matches!(self.tget(ty_of(e)), Ty::Func(_, _, th) if th == NEVER) || !self.harmless(body) {
            return false;
        }
        let mut es = Vec::new();
        super::body::collect_exprs_expr(ast, body, &mut es);
        es.iter().all(|&x| match &ast.expr(x).kind {
            ExprKind::Index { .. } | ExprKind::NonNull(_) | ExprKind::As(..) | ExprKind::Try(_) | ExprKind::Spread(_) => false,
            ExprKind::Binary(BinOp::Div | BinOp::Rem | BinOp::Pow, ..) => matches!(self.tget(ty_of(x)), Ty::Int | Ty::F64),
            _ => true,
        })
    }

    /// `src.map(f).filter(g)…` (the receiver of the call `name(args)`) as one loop ending in that
    /// call, when every callback is a fusible arrow: no array is made for a stage.
    fn fused_chain(&mut self, obj: ExprId, name: &str, args: &[Arg], ty: TyId) -> Option<Val> {
        if !matches!(name, "map" | "filter" | "some" | "every" | "find" | "findIndex" | "reduce") || args.is_empty() || args.len() > if name == "reduce" { 2 } else { 1 } {
            return None;
        }
        if name == "reduce" && args.len() != 2 {
            return None;
        }
        let m = self.cur_m();
        // stages, last first: (filter?, callback, the stage's array type)
        let mut stages: Vec<(bool, ExprId, TyId)> = Vec::new();
        let mut cur = obj;
        loop {
            let ast = self.ast(m);
            let mut e = cur;
            while let ExprKind::Paren(x) = ast.expr(e).kind {
                e = x;
            }
            let ExprKind::Call { callee, args: cargs, optional: false, .. } = &ast.expr(e).kind else { break };
            let ExprKind::Member { obj: inner, name: sname, optional: false, .. } = &ast.expr(*callee).kind else { break };
            let filter = match self.sym(*sname) {
                "map" => false,
                "filter" => true,
                _ => break,
            };
            let Some(Callee::Method(rt)) = self.facts(m).calls.get(&e).map(|c| c.callee.clone()) else { break };
            if cargs.len() != 1 || !matches!(self.tget(rt), Ty::Array(_)) || !self.fusible_arrow(cargs[0].expr) {
                break;
            }
            let et = self.ty(e);
            stages.push((filter, cargs[0].expr, et));
            cur = *inner;
        }
        if stages.is_empty() || !self.fusible_arrow(args[0].expr) {
            return None;
        }
        stages.reverse();
        let cur_ty = self.ty(cur);
        let src_ty = self.c.types.without_undefined(cur_ty);
        let Ty::Array(et0) = self.tget(src_ty) else { return None };
        let filters = stages.iter().filter(|s| s.0).count();
        // the result
        let (res, stop) = match name {
            // (made once the source is: sized by it, below)
            "map" | "filter" => {
                let Ty::Array(_) = self.tget(ty) else { return None };
                (None, None)
            }
            "some" => (Some(self.tmp(BOOL, "false", false)), Some("")),
            "every" => (Some(self.tmp(BOOL, "true", false)), Some("")),
            "findIndex" => (Some(self.tmp(INT, "-1", false)), Some("")),
            "find" => {
                let u = self.coerce(Val::plain("0", UNDEFINED), ty);
                let code = self.consume(u);
                (Some(self.tmp(ty, &code, true)), Some(""))
            }
            _ => (None, None),
        };
        let acc = if name == "reduce" {
            let init = self.expr(args[1].expr);
            let init = self.coerce(init, ty);
            let code = self.consume(init);
            let act = self.ctype(ty);
            let acc = self.fresh("acc");
            self.line(format!("{act} {acc} = {code};"));
            Some(acc)
        } else {
            None
        };
        let src = self.expr(cur);
        // (the callbacks change nothing: the source stays as it is through the loop)
        let src = self.coerce(src, src_ty);
        // (a map's result is the source's length; a filtered one has room for up to 16 of it in
        // its own block, so a short result needs no second allocation for its elements)
        let res = match (name, &self.tget(ty)) {
            ("map" | "filter", Ty::Array(rt)) => {
                let rd = self.desc(*rt);
                let cap = if filters == 0 && name == "map" { format!("tv_arr_len({})", src.code) } else { format!("(tv_arr_len({0}) < 16 ? tv_arr_len({0}) : 16)", src.code) };
                Some(self.tmp(ty, &format!("tv_arr_with_capacity({rd}, {cap})"), true))
            }
            _ => res,
        };
        let cbs: Vec<_> = stages.iter().map(|s| s.1).collect();
        let cbs: Vec<_> = cbs.into_iter().map(|a| self.prepare_callback(a)).collect();
        let sink = self.prepare_callback(args[0].expr);
        let counters: Vec<String> = (0..filters).map(|_| self.fresh("n")).collect();
        for c in &counters {
            self.line(format!("tv_int {c} = 0;"));
        }
        let done = stop.map(|_| self.fresh("done"));
        if let Some(d) = &done {
            self.line(format!("bool {d} = false;"));
        }
        let ect0 = self.ctype(et0);
        let i = self.fresh("i");
        let cond = match &done {
            Some(d) => format!("!{d} && {i} < tv_arr_len({})", src.code),
            None => format!("{i} < tv_arr_len({})", src.code),
        };
        self.open_block(&format!("for (tv_int {i} = 0; {cond}; {i}++) {{"));
        let mut v = Val::plain(format!("(({ect0} *)tv_arr_data({}))[{i}]", src.code), et0);
        let mut idx = i.clone();
        let mut opened = 0;
        let mut counters = counters.into_iter();
        for (k, (filter, _, sty)) in stages.iter().enumerate() {
            let r = self.invoke(&cbs[k], vec![v.clone(), Val::plain(idx.clone(), INT)]);
            if *filter {
                let c = self.truthy_val(r);
                self.open_block(&format!("if ({c}) {{"));
                opened += 1;
                let n = counters.next().unwrap();
                let j = self.fresh("j");
                self.line(format!("tv_int {j} = {n}++; (void){j};"));
                idx = j;
                // (by a type guard: of the members that pass)
                let Ty::Array(rt) = self.tget(*sty) else { return None };
                if v.ty != rt {
                    v = self.project(v, rt);
                }
            } else {
                let Ty::Array(rt) = self.tget(*sty) else { return None };
                v = self.coerce(r, rt);
            }
        }
        // (the element, borrowed: a stage's result is released with its block)
        let elem = Val { owned: false, ..v };
        let r = match &acc {
            Some(acc) => self.invoke(&sink, vec![Val::plain(acc.clone(), ty), elem.clone(), Val::plain(idx.clone(), INT)]),
            None => self.invoke(&sink, vec![elem.clone(), Val::plain(idx.clone(), INT)]),
        };
        match name {
            "map" => {
                let res = res.as_ref().unwrap();
                let Ty::Array(rt) = self.tget(ty) else { unreachable!() };
                let (rd, rct) = (self.desc(rt), self.ctype(rt));
                let v = self.coerce(r, rt);
                let code = self.consume(v);
                self.line(format!("TVG_PUSH({rct}, &{}, {rd}, {code});", res.code));
            }
            "filter" => {
                let res = res.as_ref().unwrap();
                let Ty::Array(rt) = self.tget(ty) else { unreachable!() };
                let (rd, rct) = (self.desc(rt), self.ctype(rt));
                let c = self.truthy_val(r);
                self.open_block(&format!("if ({c}) {{"));
                let x = if elem.ty == rt { elem } else { self.project(elem, rt) };
                let code = self.consume(x);
                self.line(format!("TVG_PUSH({rct}, &{}, {rd}, {code});", res.code));
                self.close_block("}");
            }
            "some" | "every" | "findIndex" => {
                let res = res.as_ref().unwrap();
                let d = done.as_ref().unwrap();
                let c = self.truthy_val(r);
                let (test, val) = match name {
                    "some" => (c, "true".to_string()),
                    "every" => (format!("!({c})"), "false".to_string()),
                    _ => (c, idx.clone()),
                };
                self.line(format!("if ({test}) {{ {} = {val}; {d} = true; }}", res.code));
            }
            "find" => {
                let res = res.as_ref().unwrap();
                let d = done.as_ref().unwrap();
                let c = self.truthy_val(r);
                self.open_block(&format!("if ({c}) {{"));
                let passed = self.c.types.without_undefined(ty);
                let x = if self.c.types.without_undefined(elem.ty) == passed { elem } else { self.project(elem, passed) };
                let x = self.coerce(x, ty);
                let code = self.consume(x);
                let rel = self.release_code(ty, &res.code);
                self.line(format!("{rel}; {} = {code}; {d} = true;", res.code));
                self.close_block("}");
            }
            _ => {
                let acc = acc.as_ref().unwrap();
                let r = self.coerce(r, ty);
                let code = self.consume(r);
                let (act, n) = (self.ctype(ty), self.fresh("n"));
                let rel = self.release_code(ty, acc);
                self.line(format!("{act} {n} = {code}; {rel}; {acc} = {n};"));
            }
        }
        for _ in 0..opened {
            self.close_block("}");
        }
        self.close_block("}");
        if let Some(acc) = acc {
            let owned = self.is_rc(ty);
            if owned {
                self.b().temps.last_mut().unwrap().push((acc.clone(), ty));
            }
            return Some(Val { code: acc, ty, owned });
        }
        res
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
                // (`map` by a callback that can't throw: each result written in place, the
                // length set once at the end, so the C compiler keeps the count in a register)
                if name == "map"
                    && !cb.throws
                    && let Ty::Array(rt) = self.tget(ty)
                {
                    let (rd, rct) = (self.desc(rt), self.ctype(rt));
                    let (n, w, i) = (self.fresh("n"), self.fresh("w"), self.fresh("i"));
                    self.line(format!("tv_int {n} = tv_arr_len({});", arr.code));
                    let res = self.tmp(ty, &format!("tv_arr_with_capacity({rd}, {n})"), true);
                    self.line(format!("{rct} *{w} = ({rct} *)tv_arr_data({}); tv_int {i} = 0;", res.code));
                    self.open_block(&format!("for (; {i} < {n} && {i} < tv_arr_len({}); {i}++) {{", arr.code));
                    let r = self.invoke(&cb, vec![elem(&i), Val::plain(i.clone(), INT)]);
                    let v = self.coerce(r, rt);
                    let code = self.consume(v);
                    self.line(format!("{w}[{i}] = {code};"));
                    self.close_block("}");
                    self.line(format!("if ({i}) {}.p->len = {i};", res.code));
                    return res;
                }
                let (res, res_ty) = match name {
                    "map" | "flatMap" => {
                        let Ty::Array(rt) = self.tget(ty) else { return Val::plain("0", ty) };
                        let rd = self.desc(rt);
                        let cap = if name == "map" { format!("tv_arr_len({})", arr.code) } else { "0".into() };
                        (self.tmp(ty, &format!("tv_arr_with_capacity({rd}, {cap})"), true), ty)
                    }
                    // (room for up to 16 in its own block: see the fused chains)
                    "filter" => (self.tmp(ty, &format!("tv_arr_with_capacity({d}, tv_arr_len({0}) < 16 ? tv_arr_len({0}) : 16)", arr.code), true), ty),
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
                        // (appended in place: concatenating a new array each time was quadratic)
                        self.line(format!("tv_arr_extend(&{}, {rd}, {});", res.code, v.code));
                    }
                    "filter" => {
                        let c = self.truthy_val(r);
                        let Ty::Array(rt) = self.tget(res_ty) else { unreachable!() };
                        if rt == et {
                            let copy = self.retain_code(et, "x_");
                            self.line(format!("if ({c}) {{ {ect} x_ = {}; {copy}; TVG_PUSH({ect}, &{}, {d}, x_); }}", elem(&i).code, res.code));
                        } else {
                            // (by a type guard: of the members that pass)
                            let (rd, rct) = (self.desc(rt), self.ctype(rt));
                            self.open_block(&format!("if ({c}) {{"));
                            let x = self.project(elem(&i), rt);
                            let code = self.consume(x);
                            self.line(format!("TVG_PUSH({rct}, &{}, {rd}, {code});", res.code));
                            self.close_block("}");
                        }
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
                        let passed = self.c.types.without_undefined(ty);
                        let found = if self.c.types.without_undefined(et) == passed { elem(&i) } else { self.project(elem(&i), passed) };
                        let found = self.coerce(found, ty);
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
                let act = self.ctype(acc_ty);
                let acc = self.fresh("acc");
                // (no initial value: the first element; none at all throws in JavaScript)
                let from = if let Some(a) = args.get(1) {
                    let init = self.expr(a.expr);
                    let init = self.coerce(init, acc_ty);
                    let code = self.consume(init);
                    self.line(format!("{act} {acc} = {code};"));
                    0
                } else {
                    let loc = self.loc(span);
                    self.line(format!("if (tv_arr_len({}) == 0) tv_trap(\"reduce of an empty array with no initial value\", {loc});", arr.code));
                    let first = elem("0");
                    let first = self.coerce(first, acc_ty);
                    let code = self.consume(first);
                    self.line(format!("{act} {acc} = {code};"));
                    1
                };
                let i = self.fresh("i");
                self.open_block(&format!("for (tv_int {i} = {from}; {i} < tv_arr_len({}); {i}++) {{", arr.code));
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
                if name == "includes" {
                    return Val::plain(format!("tv_arr_includes({}, {d}, &{xn})", arr.code), BOOL);
                }
                let f = if name == "lastIndexOf" { "tv_arr_last_index_of" } else { "tv_arr_index_of" };
                Val::plain(format!("{f}({}, {d}, &{xn})", arr.code), INT)
            }
            "join" | "toString" => {
                let sep = match args.first().filter(|_| name == "join") {
                    Some(a) => {
                        let v = self.expr(a.expr);
                        self.coerce(v, STR).code
                    }
                    None => self.lit(b","),
                };
                // (`undefined` and `null` elements join as nothing, as in JavaScript)
                let all_absent = et == UNDEFINED || et == NULL;
                let absent: Vec<String> = if all_absent {
                    vec!["true".to_string()]
                } else {
                    [UNDEFINED, NULL].iter().filter_map(|&n| self.tag_of(et, n)).map(|t| self.u_is("(*e_)", et, t)).collect()
                };
                if !absent.is_empty() {
                    let (sbv, i, ect) = (self.fresh("sb"), self.fresh("i"), self.ctype(et));
                    let sv = self.fresh("sep");
                    self.line(format!("tv_sb {sbv} = {{0}}; tv_str {sv} = {sep};"));
                    self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", arr.code));
                    self.line(format!("{ect} *e_ = &(({ect} *)tv_arr_data({}))[{i}];", arr.code));
                    self.line(format!("tv_sb *sb = &{sbv};"));
                    self.line(format!("if ({i}) tv_sb_push_str(sb, {sv});"));
                    if !all_absent {
                        let c = self.string_code(et, "(*e_)");
                        self.line(format!("if (!({})) {{ {c}; }}", absent.join(" || ")));
                    }
                    self.close("}");
                    return self.tmp(STR, &format!("tv_str_from_sb(&{sbv})"), true);
                }
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
                // (one array: copied once with it; else each array's elements or each value, appended)
                let first_ty = args.first().map(|a| self.ty(a.expr));
                if let [a0] = args
                    && first_ty.is_some_and(|t| matches!(self.tget(t), Ty::Array(_)))
                {
                    let o = self.expr(a0.expr);
                    let o = self.coerce(o, ty);
                    return self.tmp(ty, &format!("tv_arr_concat({}, {}, {d})", arr.code, o.code), true);
                }
                let res = self.tmp(ty, &format!("tv_arr_slice({}, {d}, 0, 0, false, false)", arr.code), true);
                for a in args {
                    let at = self.ty(a.expr);
                    let v = self.expr(a.expr);
                    if matches!(self.tget(at), Ty::Array(_)) {
                        let v = self.coerce(v, ty);
                        self.line(format!("tv_arr_extend(&{}, {d}, {});", res.code, v.code));
                    } else {
                        let v = self.coerce(v, et);
                        let code = self.consume(v);
                        let n = self.fresh("e");
                        self.line(format!("{ect} {n} = {code};"));
                        self.line(format!("tv_arr_push(&{}, {d}, &{n});", res.code));
                    }
                }
                res
            }
            "flat" => {
                // one level: each nested array's elements, in order
                let Ty::Array(rt) = self.tget(ty) else { return Val::plain("TV_EMPTY_ARR", ty) };
                let rd = self.desc(rt);
                let out = self.tmp(ty, "tv_arr_fresh()", true);
                let i = self.fresh("i");
                self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", arr.code));
                if matches!(self.tget(et), Ty::Array(_)) && self.held(et) == self.held(ty) {
                    self.line(format!("tv_arr_extend(&{}, {rd}, {});", out.code, elem(&i).code));
                } else if let Ty::Union(ms) = self.tget(et)
                    && self.niche(et).is_none()
                {
                    // (a union of arrays and more: each array's elements, the rest as they are)
                    let ms = self.c.types.tys(ms).to_vec();
                    let rct = self.ctype(rt);
                    let ev = elem(&i).code;
                    let tag = self.u_tag(&ev, et);
                    self.open(&format!("switch ({tag}) {{"));
                    for (k, &m) in ms.iter().enumerate() {
                        self.open(&format!("case {k}: {{"));
                        self.push_temps();
                        let payload = if self.is_unit(m) { Val::plain("0", m) } else { Val::plain(self.u_payload(&ev, et, k), m) };
                        if let Ty::Array(me) = self.tget(m) {
                            let (mct, j) = (self.ctype(me), self.fresh("j"));
                            self.open(&format!("for (tv_int {j} = 0; {j} < tv_arr_len({}); {j}++) {{", payload.code));
                            self.push_temps();
                            let x = Val::plain(format!("((({mct} *)tv_arr_data({}))[{j}])", payload.code), me);
                            let x = self.coerce(x, rt);
                            let x = self.own(x);
                            let c = self.consume(x);
                            self.line(format!("TVG_PUSH({rct}, &{}, {rd}, {c});", out.code));
                            self.pop_temps();
                            self.close("}");
                        } else {
                            let x = self.coerce(payload, rt);
                            let x = self.own(x);
                            let c = self.consume(x);
                            self.line(format!("TVG_PUSH({rct}, &{}, {rd}, {c});", out.code));
                        }
                        self.pop_temps();
                        self.line("break;");
                        self.close("}");
                    }
                    self.line("default: break;");
                    self.close("}");
                } else {
                    let x = self.fresh("x");
                    let r = self.retain_code(et, &x);
                    self.line(format!("{ect} {x} = {}; {r}; tv_arr_push(&{}, {rd}, &{x});", elem(&i).code, out.code));
                }
                self.close("}");
                out
            }
            "entries" | "keys" => {
                // `[index, element]` pairs, or the indexes
                let Ty::Array(rt) = self.tget(ty) else { return Val::plain("TV_EMPTY_ARR", ty) };
                let (rd, rct) = (self.desc(rt), self.ctype(rt));
                let out = self.tmp(ty, &format!("tv_arr_with_capacity({rd}, tv_arr_len({}))", arr.code), true);
                let i = self.fresh("i");
                self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", arr.code));
                if name == "keys" {
                    self.line(format!("TVG_PUSH(tv_int, &{}, {rd}, {i});", out.code));
                } else {
                    let f = self.tuple_of(rt).unwrap_or_default();
                    let x = self.fresh("x");
                    let r = self.retain_code(et, &x);
                    self.line(format!("{ect} {x} = {}; {r};", elem(&i).code));
                    if f.len() == 2 {
                        let (f0, f1) = (self.sym(f[0].name).to_string(), self.sym(f[1].name).to_string());
                        let lit = self.rec_lit(rt, &format!(".f_{f0} = {i}, .f_{f1} = {x}"));
                        self.line(format!("TVG_PUSH({rct}, &{}, {rd}, {lit});", out.code));
                    }
                }
                self.close("}");
                out
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

    /// `s.replace(/re/g, (m) => text)` (rewritten as `__reReplaceFn1(s, re, f)`) with a callback
    /// that can't throw: a loop over the matches here, the callback's body inline, the text built
    /// in one builder (no closure call, no array of positions, no string per piece).
    fn inline_replace(&mut self, e: ExprId) -> Option<Val> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let ExprKind::Call { callee, args, optional: false, .. } = &ast.expr(e).kind else { return None };
        let ExprKind::Ident(f) = ast.expr(*callee).kind else { return None };
        // (`__reReplaceFn`: the callback's parameters after the match are the groups)
        // (`__reReplaceFnG<k>`: k groups, then the offset and the input)
        let fname = self.sym(f).to_string();
        let offset_after = fname.strip_prefix("__reReplaceFnG").and_then(|k| k.parse::<usize>().ok());
        let groups = match fname.as_str() {
            "__reReplaceFn1" => false,
            "__reReplaceFn" => true,
            _ if offset_after.is_some() => true,
            _ => return None,
        };
        if args.len() != 3 {
            return None;
        }
        // (the expression a literal, its flags known: global or not, never sticky)
        let ExprKind::Call { callee: lc, args: la, .. } = &ast.expr(args[1].expr).kind else { return None };
        let ExprKind::Ident(ln) = ast.expr(*lc).kind else { return None };
        if self.sym(ln) != "__reLiteral" || la.len() != 2 {
            return None;
        }
        let ExprKind::Str(flags) = ast.expr(la[1].expr).kind else { return None };
        let flags = self.sym(flags).to_string();
        if flags.contains('y') {
            return None;
        }
        let global = flags.contains('g');
        let ExprKind::Arrow(arrow) = &ast.expr(args[2].expr).kind else { return None };
        let crate::ast::ArrowBody::Expr(body) = arrow.body else { return None };
        if arrow.params.len() > if groups { 10 } else { 1 } || arrow.params.iter().any(|p| p.rest || p.optional) {
            return None;
        }
        let ft = self.ty(args[2].expr);
        let Ty::Func(_, _, throws) = self.tget(ft) else { return None };
        if throws != NEVER {
            return None;
        }
        let params: Vec<u32> = arrow.params.iter().map(|p| p.span.start).collect();
        let sv = self.expr(args[0].expr);
        let sv = self.coerce(sv, STR);
        let rv = self.expr(args[1].expr);
        let (sn, rn, it, sb, last, any, pos) = (self.fresh("rs"), self.fresh("re"), self.fresh("it"), self.fresh("sb"), self.fresh("last"), self.fresh("any"), self.fresh("pos"));
        let rct = self.ctype(rv.ty);
        self.line(format!("tv_str {sn} = {}; {rct} {rn} = {};", sv.code, rv.code));
        self.line(format!("tv_re_iter {it}; tv_re_iter_init(&{it}, ({rn})->f_h, {sn});"));
        self.line(format!("tv_sb {sb} = {{0}}; tv_int {last} = 0, {pos}[2]; bool {any} = false;"));
        self.open(&format!("while (tv_re_iter_next(&{it}, {pos})) {{"));
        self.line(format!("{any} = true;"));
        self.line(format!("tv_sb_push_units(&{sb}, {sn}, {last}, {pos}[0]);"));
        let mv = self.fresh("m");
        self.line(format!("tv_str {mv} = tv_str_slice({sn}, {pos}[0], {pos}[1], true, NULL);"));
        self.push_scope();
        self.b().scopes.last_mut().unwrap().releases.push(format!("tv_str_release({mv});"));
        if let Some(&k) = params.first() {
            self.b().locals.insert(k, crate::codegen::body::Local { access: mv.clone(), ty: STR });
        }
        for (g, &k) in params.iter().enumerate().skip(1) {
            if let Some(ng) = offset_after
                && g > ng
            {
                // (the match's offset, then the input)
                if g == ng + 1 {
                    let ov = self.fresh("off");
                    self.line(format!("tv_int {ov} = {pos}[0];"));
                    self.b().locals.insert(k, crate::codegen::body::Local { access: ov, ty: INT });
                } else {
                    self.b().locals.insert(k, crate::codegen::body::Local { access: sn.clone(), ty: STR });
                }
                continue;
            }
            let gv = self.fresh("g");
            self.line(format!("tv_str {gv} = tv_re_iter_group(&{it}, {sn}, {g});"));
            self.b().scopes.last_mut().unwrap().releases.push(format!("tv_str_release({gv});"));
            self.b().locals.insert(k, crate::codegen::body::Local { access: gv, ty: STR });
        }
        self.push_temps();
        let r = self.expr(body);
        let r = self.coerce(r, STR);
        self.line(format!("tv_sb_push_str(&{sb}, {});", r.code));
        self.pop_temps();
        self.pop_scope();
        self.line(format!("{last} = {pos}[1];"));
        if !global {
            self.line("break;");
        }
        self.close("}");
        self.line(format!("tv_re_iter_done(&{it});"));
        let res = self.fresh("t");
        self.line(format!("tv_str {res};"));
        self.line(format!("if ({any}) {{ tv_sb_push_units(&{sb}, {sn}, {last}, tv_str_length({sn})); {res} = tv_str_from_sb(&{sb}); }} else {{ tv_sb_free(&{sb}); {res} = {sn}; tv_str_retain({res}); }}"));
        if global {
            self.line(format!("({rn})->f_lastIndex = 0;"));
        }
        let ty = self.ty(e);
        let v = self.tmp(STR, &res, true);
        Some(self.coerce(v, ty))
    }

    /// `Array.isArray(v)`: by its type, its union's tag, or (an `unknown`) what it holds; a member
    /// that's a recursive alias or a union by what that holds.
    fn is_array_code(&mut self, v: Val) -> String {
        match self.tget(v.ty) {
            Ty::Array(_) => "true".into(),
            Ty::Unknown => self.dyn_is_array(&v.code),
            Ty::Rec(..) => {
                let u = self.unfold_val(v);
                self.is_array_code(u)
            }
            Ty::Union(ms) => {
                let ms = self.c.types.tys(ms).to_vec();
                let tag = self.u_tag(&v.code, v.ty);
                let mut parts = Vec::new();
                for (i, &m) in ms.iter().enumerate() {
                    match self.tget(m) {
                        Ty::Array(_) => parts.push(format!("{tag} == {i}")),
                        Ty::Rec(..) | Ty::Union(_) => {
                            let payload = Val { code: self.u_payload(&v.code, v.ty, i), ty: m, owned: false };
                            let inner = self.is_array_code(payload);
                            if inner != "false" {
                                parts.push(format!("({tag} == {i} && {inner})"));
                            }
                        }
                        _ => {}
                    }
                }
                if parts.is_empty() { "false".into() } else { format!("({})", parts.join(" || ")) }
            }
            _ => "false".into(),
        }
    }

    pub(crate) fn map_method_pub(&mut self, mv: Val, k: TyId, v: TyId, name: &str, args: &[Arg], ty: TyId) -> Val {
        self.map_method(mv, k, v, name, args, ty)
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
            "entries" => self.map_entries(mv, k, v, ty),
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
        // An optional `int` argument: (value, given).
        let opt_int_arg = |g: &mut Self, i: usize| -> (String, String) {
            if argv.len() <= i {
                return ("0".into(), "false".into());
            }
            let v = argv[i].clone();
            if !g.c.types.has_undefined(v.ty) {
                return (g.int_code(v), "true".into());
            }
            let opt_int = g.c.types.optional(INT);
            let v = g.coerce(v, opt_int);
            g.optional_int(&v)
        };
        let loc = self.loc(span);
        let code = match name {
            "chars" => format!("tv_str_chars({})", s.code),
            "slice" | "substring" | "substr" => {
                let (a0, _) = opt_int_arg(self, 0);
                let (e, he) = opt_int_arg(self, 1);
                match name {
                    "slice" => format!("tv_str_slice({}, {a0}, {e}, {he}, {loc})", s.code),
                    "substring" => format!("tv_str_substring({}, {a0}, {e}, {he})", s.code),
                    _ => format!("tv_str_substr({}, {a0}, {e}, {he})", s.code),
                }
            }
            "includes" | "startsWith" | "endsWith" if argv.len() > 1 => {
                let needle = str_arg(self, 0);
                let (pos, given) = opt_int_arg(self, 1);
                let code = match name {
                    "includes" => format!("tv_str_includes_at({}, {needle}, {given} ? {pos} : 0)", s.code),
                    "startsWith" => format!("tv_str_starts_with_at({}, {needle}, {given} ? {pos} : 0)", s.code),
                    _ => format!("tv_str_ends_with_at({0}, {needle}, {given} ? {pos} : tv_str_length({0}))", s.code),
                };
                return Val::plain(code, BOOL);
            }
            "includes" => return Val::plain(format!("tv_str_includes({}, {})", s.code, str_arg(self, 0)), BOOL),
            "startsWith" => return Val::plain(format!("tv_str_starts_with({}, {})", s.code, str_arg(self, 0)), BOOL),
            "endsWith" => return Val::plain(format!("tv_str_ends_with({}, {})", s.code, str_arg(self, 0)), BOOL),
            "indexOf" if argv.len() > 1 => {
                let needle = str_arg(self, 0);
                let (pos, given) = opt_int_arg(self, 1);
                return Val::plain(format!("tv_str_index_of_from({}, {needle}, {given} ? {pos} : 0)", s.code), INT);
            }
            "indexOf" => return Val::plain(format!("tv_str_index_of({}, {})", s.code, str_arg(self, 0)), INT),
            "lastIndexOf" => {
                let needle = str_arg(self, 0);
                let (pos, given) = opt_int_arg(self, 1);
                return Val::plain(format!("tv_str_last_index_of({}, {needle}, {pos}, {given})", s.code), INT);
            }
            "charCodeAt" => {
                let (i, _) = opt_int_arg(self, 0);
                return Val::plain(format!("tv_str_char_code_at({}, {i})", s.code), INT);
            }
            "charAt" => {
                let (i, _) = opt_int_arg(self, 0);
                format!("tv_str_char_at({}, {i})", s.code)
            }
            "codePointAt" | "at" => {
                // -1 / out of range: undefined
                let sv = self.fresh("s");
                self.line(format!("tv_str {sv} = {};", s.code));
                let i = int_arg(self, 0);
                let iv = self.fresh("ix");
                if name == "at" {
                    self.line(format!("tv_int {iv} = {i}; if ({iv} < 0) {iv} += tv_str_length({sv});"));
                } else {
                    self.line(format!("tv_int {iv} = {i};"));
                }
                let (inner, call) = if name == "at" { (STR, format!("tv_str_unit_at({sv}, {iv})")) } else { (INT, format!("tv_str_code_point_at({sv}, {iv})")) };
                let v = Val { code: call, ty: inner, owned: inner == STR };
                return self.present_if(&format!("(uint64_t){iv} < (uint64_t)tv_str_length({sv})"), v, ty);
            }
            "concat" => {
                let mut cur = Val { code: format!("tv_str_concat({}, TV_EMPTY_STR)", s.code), ty: STR, owned: true };
                for i in 0..argv.len() {
                    let a = str_arg(self, i);
                    let prev = if i == 0 { s.code.clone() } else { cur.code.clone() };
                    cur = self.tmp(STR, &format!("tv_str_concat({prev}, {a})"), true);
                }
                if argv.is_empty() {
                    cur.code
                } else {
                    return cur;
                }
            }
            "split" if argv.len() > 1 && !matches!(self.tget(argv[1].ty), Ty::Undefined) => {
                let sep = str_arg(self, 0);
                let lim = argv[1].clone();
                let lim = if self.c.types.has_undefined(lim.ty) {
                    // (`undefined`: no limit)
                    let ot = self.c.types.optional(F64);
                    let lv = self.coerce(lim, ot);
                    match (self.tag_of(lv.ty, UNDEFINED), self.tag_of(lv.ty, F64)) {
                        (Some(k), Some(fk)) => format!("(({0}).tag == {k} ? 4294967295.0 : ({0}).u.m{fk})", lv.code),
                        _ => lv.code.clone(),
                    }
                } else {
                    let v = self.coerce(lim, F64);
                    v.code
                };
                format!("tv_str_split_limit({}, {sep}, {lim})", s.code)
            }
            "split" => format!("tv_str_split({}, {})", s.code, str_arg(self, 0)),
            "trim" => format!("tv_str_trim({})", s.code),
            "trimStart" => format!("tv_str_trim_start({})", s.code),
            "trimEnd" => format!("tv_str_trim_end({})", s.code),
            "toUpperCase" => format!("tv_str_to_upper({})", s.code),
            "localeCompare" => return Val::plain(format!("tv_str_locale_compare({}, {})", s.code, str_arg(self, 0)), INT),
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
                // (`console.log(...xs)`: the elements, as separate arguments)
                let spread: Vec<bool> = args.iter().map(|a| matches!(ast.expr(a.expr).kind, ExprKind::Spread(_))).collect();
                if spread.iter().any(|&s| s) {
                    let sbv = self.fresh("sb");
                    let sp = self.fresh("sp");
                    self.line(format!("tv_sb {sbv} = {{0}}; bool {sp} = false;"));
                    for a in args {
                        let one = |g: &mut Self, v: &Val| {
                            g.line(format!("if ({sp}) tv_sb_push_char(&{sbv}, ' ');"));
                            g.line(format!("{sp} = true;"));
                            g.line("{");
                            g.line(format!("tv_sb *sb = &{sbv};"));
                            let c = g.inspect_root(v.ty, &v.code, "0");
                            g.line(format!("{c};"));
                            g.line("}");
                        };
                        match ast.expr(a.expr).kind {
                            ExprKind::Spread(inner) => {
                                let xs = self.spread_array(inner);
                                let Ty::Array(et) = self.tget(xs.ty) else { continue };
                                let ect = self.ctype(et);
                                let i = self.fresh("i");
                                self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", xs.code));
                                let v = Val::plain(format!("((({ect} *)tv_arr_data({}))[{i}])", xs.code), et);
                                one(self, &v);
                                self.close("}");
                            }
                            _ => {
                                let v = self.expr(a.expr);
                                one(self, &v);
                            }
                        }
                    }
                    let f = if matches!(name, "error" | "warn") { "tv_err_sb_line" } else { "tv_out_sb_line" };
                    self.line(format!("{f}(&{sbv}); tv_sb_free(&{sbv});"));
                    return Val::plain("0", VOID);
                }
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
                    // (a local made as a `Float64Array`, held as an `f64[]`: printed as one)
                    if v.ty == self.c.types.array(F64) && arg_exprs.get(i).is_some_and(|&x| self.float64_array(x)) {
                        let d = self.desc(F64);
                        self.uses_inspect = true;
                        self.line(format!("tv_inspect_typed_arr(sb, {}, {d}, 0, \"Float64Array\");", v.code));
                        continue;
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
            (Some("performance"), "now") => Val::plain("tv_performance_now()", F64),
            (Some("stdout" | "stderr"), "write") => {
                let v = self.expr(args[0].expr);
                let v = self.coerce(v, STR);
                let f = if ns == Some("stdout") { "tv_write_stdout" } else { "tv_write_stderr" };
                self.line(format!("{f}({});", v.code));
                Val::plain("0", VOID)
            }
            (Some("__native"), _) => {
                if name.starts_with("re") && matches!(name, "reCompile" | "reFlags" | "reExec" | "reExecInto" | "reTest" | "reNames" | "reSplit" | "reReplace" | "reMatchAll") {
                    self.uses_regexp = true;
                }
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
            (None, "Boolean") => {
                let Some(a) = args.first() else { return Val::plain("false", BOOL) };
                let v = self.expr(a.expr);
                let t = self.js_truthy(v);
                self.tmp(BOOL, &t, false)
            }
            (None, "String") => {
                let v = self.expr(args[0].expr);
                if v.ty == STR {
                    return v;
                }
                // (an integer: its digits at once, a small one shared)
                if self.c.types.is_int(v.ty) && v.ty != U64 {
                    let f = if v.ty == INT && self.js_ints(self.cur_m()) { "tv_str_from_jint" } else { "tv_str_from_int" };
                    return self.tmp(STR, &format!("{f}((tv_int)({}))", v.code), true);
                }
                if v.ty == F64 {
                    return self.tmp(STR, &format!("tv_str_from_f64({})", v.code), true);
                }
                // (a union of strings, numbers and the like: each member's text, no builder)
                if let Ty::Union(ms) = self.tget(v.ty)
                    && self.niche(v.ty).is_none()
                {
                    let ms = self.c.types.tys(ms).to_vec();
                    let simple = ms.iter().all(|&m| matches!(self.tget(m), Ty::Str | Ty::StrLit(_) | Ty::F64 | Ty::Int | Ty::Bool | Ty::Undefined | Ty::Null | Ty::BoolLit(_)));
                    if simple {
                        let res = self.fresh("s");
                        let tag = self.u_tag(&v.code, v.ty);
                        self.line(format!("tv_str {res};"));
                        self.open(&format!("switch ({tag}) {{"));
                        for (i, &m) in ms.iter().enumerate() {
                            let p = if self.is_unit(m) { String::new() } else { self.u_payload(&v.code, v.ty, i) };
                            let code = match self.tget(m) {
                                Ty::Str => format!("{p}; tv_str_retain({res})"),
                                Ty::StrLit(sym) => self.lit(self.sym(sym).to_string().as_bytes()),
                                Ty::F64 => format!("tv_str_from_f64({p})"),
                                Ty::Int => format!("{}({p})", if self.js_ints(self.cur_m()) { "tv_str_from_jint" } else { "tv_str_from_int" }),
                                Ty::Bool => format!("(({p}) ? {} : {})", self.lit(b"true"), self.lit(b"false")),
                                Ty::BoolLit(b) => self.lit(if b { b"true" as &[u8] } else { b"false" }),
                                Ty::Undefined => self.lit(b"undefined"),
                                _ => self.lit(b"null"),
                            };
                            self.line(format!("case {i}: {res} = {code}; break;"));
                        }
                        self.line(format!("default: {res} = TV_EMPTY_STR; break;"));
                        self.close("}");
                        return self.tmp(STR, &res, true);
                    }
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
            (Some("Object"), "is") => {
                let (l, r) = (args[0].expr, args[1].expr);
                let (lt, rt) = (self.ty(l), self.ty(r));
                // (numbers: SameValue; anything else is `===`)
                if self.c.types.is_float(lt) || self.c.types.is_float(rt) {
                    let a = self.expr_before(l, &[r]);
                    let b = self.expr(r);
                    let (a, b) = (self.coerce(a, F64), self.coerce(b, F64));
                    return self.tmp(BOOL, &format!("({{ double a_ = {}, b_ = {}; a_ == b_ ? (a_ != 0 || 1 / a_ == 1 / b_) : (a_ != a_ && b_ != b_); }})", a.code, b.code), false);
                }
                return self.binary_pub(BinOp::Eq, l, r, BOOL, span);
            }
            (Some("Object"), "keys" | "values" | "entries") => {
                let v = self.expr(args[0].expr);
                let base = self.c.types.without_undefined(v.ty);
                match self.tget(base) {
                    Ty::Map(k, val) => {
                        let (kd, vd) = (self.desc(k), self.desc(val));
                        match name {
                            "keys" if self.c.types.is_numeric(k) => {
                                // (as strings, an object's keys)
                                let (kct, ks, out, i) = (self.ctype(k), self.fresh("ks"), self.fresh("out"), self.fresh("i"));
                                let conv = if self.c.types.is_float(k) { "tv_str_from_f64" } else { "tv_str_from_int" };
                                self.line(format!("tv_arr {ks} = tv_map_keys({}, {kd}, {vd}); tv_arr {out} = tv_arr_with_capacity(&tv_type_str, tv_arr_len({ks}));", v.code));
                                self.line(format!("for (tv_int {i} = 0; {i} < tv_arr_len({ks}); {i}++) TVG_PUSH(tv_str, &{out}, &tv_type_str, {conv}((({kct} *)tv_arr_data({ks}))[{i}]));"));
                                self.line(format!("tv_arr_release({ks}, {kd});"));
                                self.tmp(ty, &out, true)
                            }
                            "keys" => self.tmp(ty, &format!("tv_map_keys({}, {kd}, {vd})", v.code), true),
                            "values" => self.tmp(ty, &format!("tv_map_values({}, {kd}, {vd})", v.code), true),
                            _ => self.map_entries(v, k, val, ty),
                        }
                    }
                    Ty::Array(et) => {
                        let Ty::Array(rt) = self.tget(ty) else { return Val::plain("TV_EMPTY_ARR", ty) };
                        if name == "values" {
                            let v = self.own(v);
                            return self.coerce(v, ty);
                        }
                        let (rd, rct, ect) = (self.desc(rt), self.ctype(rt), self.ctype(et));
                        let out = self.tmp(ty, &format!("tv_arr_with_capacity({rd}, tv_arr_len({}))", v.code), true);
                        let i = self.fresh("i");
                        self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", v.code));
                        if name == "keys" {
                            self.line(format!("TVG_PUSH(tv_str, &{}, {rd}, tv_str_from_int({i}));", out.code));
                        } else {
                            let f = self.tuple_of(rt).unwrap_or_default();
                            if let [a, b] = f.as_slice() {
                                let (fa, fb) = (self.sym(a.name).to_string(), self.sym(b.name).to_string());
                                let x = self.fresh("x");
                                let r = self.retain_code(et, &x);
                                self.line(format!("{ect} {x} = (({ect} *)tv_arr_data({}))[{i}]; {r};", v.code));
                                let lit = self.rec_lit(rt, &format!(".f_{fa} = tv_str_from_int({i}), .f_{fb} = {x}"));
                                self.line(format!("TVG_PUSH({rct}, &{}, {rd}, {lit});", out.code));
                            }
                        }
                        self.close("}");
                        out
                    }
                    Ty::Record(_) => {
                        // a record's fields, in their declared order
                        let fields = self.c.types.fields_in_order(base);
                        let Ty::Array(rt) = self.tget(ty) else { return Val::plain("TV_EMPTY_ARR", ty) };
                        let (rd, rct) = (self.desc(rt), self.ctype(rt));
                        let out = self.tmp(ty, &format!("tv_arr_with_capacity({rd}, {})", fields.len()), true);
                        let tf = self.tuple_of(rt);
                        for f in &fields {
                            let fname = self.sym(f.name).to_string();
                            let place = self.fld(base, &v.code, &fname);
                            // (the keys of an index signature, its `$rest` map: each entry)
                            if fname == "$rest"
                                && let mt = self.c.types.without_undefined(f.ty)
                                && let Ty::Map(mk, mv) = self.tget(mt)
                            {
                                let present = self.truthy_val(Val::plain(place.clone(), f.ty));
                                self.open(&format!("if ({present}) {{"));
                                self.push_temps();
                                let m = self.project(Val::plain(place.clone(), f.ty), mt);
                                let (mkd, mvd, mvct) = (self.desc(mk), self.desc(mv), self.ctype(mv));
                                let (it, kp, vp) = (self.fresh("i"), self.fresh("kp"), self.fresh("vp"));
                                self.line(format!("tv_int {it} = 0; void *{kp}, *{vp};"));
                                self.open(&format!("while (tv_map_next({}, {mkd}, {mvd}, &{it}, &{kp}, &{vp})) {{", m.code));
                                self.push_temps();
                                let key = format!("({{ tv_str k_ = *(tv_str *){kp}; tv_str_retain(k_); k_; }})");
                                let ev = Val { code: format!("(*({mvct} *){vp})"), ty: mv, owned: false };
                                let item = match name {
                                    "keys" => key,
                                    "values" => {
                                        let x = self.coerce(ev, rt);
                                        let x = self.own(x);
                                        self.consume(x)
                                    }
                                    _ => match &tf {
                                        Some(tf) => {
                                            let x = self.coerce(ev, tf[1].ty);
                                            let x = self.own(x);
                                            let x = self.consume(x);
                                            let parts = format!(".f_{} = {key}, .f_{} = {x}", self.sym(tf[0].name), self.sym(tf[1].name));
                                            self.rec_lit(rt, &parts)
                                        }
                                        None => "0".into(),
                                    },
                                };
                                self.line(format!("TVG_PUSH({rct}, &{}, {rd}, {item});", out.code));
                                self.pop_temps();
                                self.close("}");
                                self.pop_temps();
                                self.close("}");
                                continue;
                            }
                            // (an optional field that's absent: no key)
                            let absent = match self.tag_of(f.ty, UNDEFINED).filter(|_| f.optional) {
                                Some(k) => Some(self.u_is(&place, f.ty, k)),
                                None => None,
                            };
                            if let Some(a) = &absent {
                                self.open(&format!("if (!({a})) {{"));
                            }
                            let key = self.lit(fname.as_bytes());
                            let fv = Val::plain(place, f.ty);
                            let item = match name {
                                "keys" => key,
                                "values" => {
                                    let x = self.coerce(fv, rt);
                                    let x = self.own(x);
                                    self.consume(x)
                                }
                                _ => {
                                    let Some(tf) = &tf else {
                                        if absent.is_some() {
                                            self.close("}");
                                        }
                                        continue;
                                    };
                                    let x = self.coerce(fv, tf[1].ty);
                                    let x = self.own(x);
                                    let x = self.consume(x);
                                    let parts = format!(".f_{} = {key}, .f_{} = {x}", self.sym(tf[0].name), self.sym(tf[1].name));
                                    self.rec_lit(rt, &parts)
                                }
                            };
                            self.line(format!("TVG_PUSH({rct}, &{}, {rd}, {item});", out.code));
                            if absent.is_some() {
                                self.close("}");
                            }
                        }
                        out
                    }
                    _ => Val::plain("TV_EMPTY_ARR", ty),
                }
            }
            (Some("Array"), "of") => {
                let elems: Vec<ExprId> = args.iter().map(|a| a.expr).collect();
                self.build_spread(&elems, ty)
            }
            (Some("Array"), "from") => self.array_from(args, ty, span),
            (Some(n), "from") if crate::check::typed_array_elem(n).is_some() => self.new_array(args, ty, span),
            (Some(n), "of") if crate::check::typed_array_elem(n).is_some() => {
                let elems: Vec<ExprId> = args.iter().map(|a| a.expr).collect();
                self.build_spread(&elems, ty)
            }
            (Some("Array"), "isArray") => {
                let v = self.expr_held(args[0].expr);
                let code = self.is_array_code(v);
                Val::plain(code, BOOL)
            }
            (Some("String"), "fromCharCode" | "fromCodePoint") => {
                let mut codes = Vec::new();
                for a in args {
                    let v = self.expr(a.expr);
                    codes.push(if self.c.types.is_float(v.ty) { format!("(tv_int){}", self.int32_code(&v)) } else { format!("(tv_int)({})", v.code) });
                }
                let code = match (name, codes.as_slice()) {
                    (_, []) => return Val::plain("TV_EMPTY_STR", STR),
                    ("fromCharCode", [c]) => format!("tv_str_from_char_code({c})"),
                    ("fromCharCode", _) => format!("tv_str_from_char_codes((tv_int[]){{{}}}, {})", codes.join(", "), codes.len()),
                    _ => {
                        let loc = self.loc(span);
                        format!("tv_str_from_code_points((tv_int[]){{{}}}, {}, {loc})", codes.join(", "), codes.len())
                    }
                };
                self.tmp(STR, &code, true)
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
                                self.line(format!("case {i}: {out} = tv_js_number({}); break;", s.code));
                            } else {
                                let n = self.coerce(Val::plain(payload, mt), F64);
                                self.line(format!("case {i}: {out} = {}; break;", n.code));
                            }
                        }
                        self.close("}");
                    }
                    _ => {
                        let n = self.coerce(v, ty);
                        return n;
                    }
                }
                Val::plain(format!("({ok} ? {out} : (double)NAN)"), F64)
            }
            (None, "Number" | "parseFloat" | "parseInt") => {
                // `parseFloat(s.slice(a, b))`: parsed where it is, the slice never made
                let m = self.cur_m();
                if name == "parseFloat"
                    && let ExprKind::Call { callee, args: sa, optional: false, .. } = &self.ast(m).expr(args[0].expr).kind
                    && let ExprKind::Member { obj: so, name: sl, optional: false, .. } = self.ast(m).expr(*callee).kind
                    && self.sym(sl) == "slice"
                    && (1..=2).contains(&sa.len())
                    && !sa.iter().any(|a| matches!(self.ast(m).expr(a.expr).kind, ExprKind::Spread(_)))
                    && {
                        let st = self.ty(so);
                        matches!(self.tget(st), Ty::Str)
                    }
                {
                    let sv = self.expr_before(so, &sa.iter().map(|a| a.expr).collect::<Vec<_>>());
                    let a0 = self.expr(sa[0].expr);
                    let a0 = self.coerce(a0, INT);
                    let (a1, has_end) = match sa.get(1) {
                        Some(a) => {
                            let v = self.expr(a.expr);
                            (self.coerce(v, INT).code, "true")
                        }
                        None => ("0".to_string(), "false"),
                    };
                    return self.tmp(F64, &format!("tv_js_parse_float_slice({}, {}, {a1}, {has_end})", sv.code, a0.code), false);
                }
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
                    return Val::plain(format!("({ok} ? (double){out} : (double)NAN)"), F64);
                }
                let _ = (ok, out);
                let f = if name == "Number" { "tv_js_number" } else { "tv_js_parse_float" };
                Val::plain(format!("{f}({})", s.code), F64)
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
            // a test in a suite: run as one of its own (see `tv_test_run_ctx`)
            (None, "__test") => {
                let n = self.expr(args[0].expr);
                let n = self.coerce(n, STR);
                let f = self.expr(args[1].expr);
                let fv = self.fresh("tf");
                self.line(format!("tv_fn {fv} = {};", f.code));
                if !self.test_call_done {
                    self.test_call_done = true;
                    self.protos.push_str("static void tvg_uncaught(void);\nstatic void tvg_test_fn(void *p);\n");
                    self.helpers_after.push("static void tvg_test_fn(void *p) { tv_fn *f = p; ((void (*)(tv_env *))f->fn)(f->env); if (tvg_err) tvg_uncaught(); }\n".to_string());
                }
                self.needs_uncaught = true;
                self.line(format!("tv_test_run_ctx((const char *){0}.p->data, (size_t){0}.p->len, tvg_test_fn, &{fv}, false);", n.code));
                if let Some(a) = args.get(2) {
                    let after = self.expr(a.expr);
                    let av = self.fresh("tf");
                    self.line(format!("tv_fn {av} = {};", after.code));
                    self.line(format!("tv_test_run_ctx((const char *){0}.p->data, (size_t){0}.p->len, tvg_test_fn, &{av}, true);", n.code));
                }
                Val::plain("0", VOID)
            }
            (None, "__delete") => {
                let o = self.expr(args[0].expr);
                let ob = self.c.types.without_undefined(o.ty);
                let Ty::Map(k, v) = self.tget(ob) else { return Val::plain("false", BOOL) };
                let kv = self.expr(args[1].expr);
                let kv = self.coerce(kv, k);
                let kn = self.key_tmp(kv, k);
                let (kd, vd) = (self.desc(k), self.desc(v));
                let place = self.fresh("dm");
                self.line(format!("tv_map {place} = {};", o.code));
                Val::plain(format!("tv_map_delete(&{place}, {kd}, {vd}, &{kn})"), BOOL)
            }
            (None, "__eachArg") => {
                let v = self.expr(args[0].expr);
                match self.tget(v.ty) {
                    Ty::Array(e) => {
                        let ect = self.ctype(e);
                        let loc = self.loc(span);
                        let at = Val::plain(format!("(*({ect} *)tv_arr_at({}, sizeof({ect}), 0, {loc}))", v.code), e);
                        let at = self.own(at);
                        self.coerce(at, ty)
                    }
                    Ty::Record(_) if self.tuple_of(v.ty).is_some() => {
                        let f = self.tuple_of(v.ty).unwrap()[0].clone();
                        let place = self.fld(v.ty, &v.code, self.sym(f.name));
                        let at = self.own(Val::plain(place, f.ty));
                        self.coerce(at, ty)
                    }
                    _ => self.coerce(v, ty),
                }
            }
            (None, "structuredClone") => {
                let v = self.expr(args[0].expr);
                self.structured_clone(v)
            }
            (None, "queueMicrotask") => {
                let cb = self.expr(args[0].expr);
                self.line(format!("tv_queue_microtask({});", cb.code));
                Val::plain("0", VOID)
            }
            (None | Some("Number"), "isNaN" | "isFinite" | "isInteger" | "isSafeInteger") => {
                let v = self.expr(args[0].expr);
                if self.c.types.is_int(v.ty) {
                    let code = match name {
                        "isNaN" => "false".to_string(),
                        "isSafeInteger" => format!("tv_int_is_safe({})", v.code),
                        _ => "true".to_string(),
                    };
                    return Val::plain(code, BOOL);
                }
                let v = self.coerce(v, F64);
                let code = match name {
                    "isNaN" => format!("isnan({})", v.code),
                    "isFinite" => format!("isfinite({})", v.code),
                    "isInteger" => format!("tv_f64_is_integer({})", v.code),
                    _ => format!("tv_f64_is_safe_integer({})", v.code),
                };
                Val::plain(code, BOOL)
            }
            (Some("Number"), "parseFloat" | "parseInt") => self.builtin(None, name, args, ty, span),
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
            Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 => {
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
        if matches!(name, "min" | "max") && args.iter().any(|a| matches!(self.ast(self.cur_m()).expr(a.expr).kind, ExprKind::Spread(_))) {
            return self.min_max_spread(name, args, ty);
        }
        if matches!(name, "min" | "max") && args.is_empty() {
            return Val::plain(if name == "min" { "((double)INFINITY)" } else { "(-(double)INFINITY)" }, F64);
        }
        let mut vals: Vec<Val> = args.iter().map(|a| self.expr(a.expr)).collect();
        // (`Math.min(a, b, c)` is the same in any order: the arguments that read what the loop
        // carries from one round to the next, `curr[i - 1]` of `curr[i] = Math.min(...)`, taken
        // last, so the round's chain of work through them is one compare, not three; only when
        // nothing in them has an effect, as they're then read in another order)
        if matches!(name, "min" | "max") && vals.len() >= 3 {
            let m = self.cur_m();
            let carried: Vec<u32> = self.store_arr.into_iter().chain(self.b().carry.as_ref().map(|c| c.0)).collect();
            let ast = self.ast(m);
            let pure = args.iter().all(|a| {
                let mut xs = Vec::new();
                super::body::collect_exprs_expr(ast, a.expr, &mut xs);
                xs.iter().all(|&x| !matches!(ast.expr(x).kind, ExprKind::Call { .. } | ExprKind::New { .. } | ExprKind::Assign(..) | ExprKind::Update { .. } | ExprKind::Await(_) | ExprKind::Yield { .. }))
            });
            if pure && !carried.is_empty() {
                let reads_carried: Vec<bool> = args
                    .iter()
                    .map(|a| {
                        let mut xs = Vec::new();
                        super::body::collect_exprs_expr(ast, a.expr, &mut xs);
                        xs.iter().any(|&x| matches!(&ast.expr(x).kind, ExprKind::Index { obj, .. } if matches!(self.ident_fact(m, *obj), Some(crate::check::IdentFact::Local(k)) if carried.contains(&k))))
                    })
                    .collect();
                if reads_carried.iter().any(|&r| r) && reads_carried.iter().any(|&r| !r) {
                    let mut order: Vec<usize> = (0..vals.len()).collect();
                    order.sort_by_key(|&i| reads_carried[i]);
                    vals = order.into_iter().map(|i| vals[i].clone()).collect();
                }
            }
        }
        let f64s: Vec<String> = vals.iter().map(|v| self.coerce(v.clone(), F64).code).collect();
        let loc = self.loc(span);
        let code = match name {
            "imul" | "clz32" => {
                let a = self.int32_code(&vals[0]);
                if name == "imul" {
                    let b = vals.get(1).map(|v| self.int32_code(v)).unwrap_or_else(|| "0".into());
                    format!("((tv_int)(int32_t)((uint32_t)({a}) * (uint32_t)({b})))")
                } else {
                    format!("({{ uint32_t c_ = (uint32_t)({a}); (tv_int)(c_ ? __builtin_clz(c_) : 32); }})")
                }
            }
            "fround" => format!("((double)(float)({}))", f64s[0]),
            "floor" | "ceil" | "trunc" if ty == F64 => format!("tv_{name}({})", f64s[0]),
            "round" if ty == F64 => format!("tv_math_round({})", f64s[0]),
            "floor" | "ceil" | "trunc" => format!("tv_f64_to_int(tv_{name}({}), \"Math.{name}\", {loc})", f64s[0]),
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

    /// `Math.max(...xs, y)`: a loop over the spread elements.
    fn min_max_spread(&mut self, name: &str, args: &[Arg], ty: TyId) -> Val {
        let ct = self.ctype(ty);
        let acc = self.fresh("m");
        let float = self.c.types.is_float(ty);
        let start = match (float, name) {
            (true, "max") => "-INFINITY".to_string(),
            (true, _) => "INFINITY".to_string(),
            (false, "max") => "INT64_MIN".to_string(),
            (false, _) => "INT64_MAX".to_string(),
        };
        self.line(format!("{ct} {acc} = {start};"));
        let pick = |a: &str, b: &str| -> String {
            if float {
                format!("tv_{name}_f64({a}, {b})")
            } else {
                let op = if name == "min" { "<" } else { ">" };
                format!("({a} {op} {b} ? {a} : {b})")
            }
        };
        for a in args {
            match self.ast(self.cur_m()).expr(a.expr).kind {
                ExprKind::Spread(inner) => {
                    let xs = self.spread_array(inner);
                    let et = match self.tget(xs.ty) {
                        Ty::Array(e) => e,
                        _ => INT,
                    };
                    let ect = self.ctype(et);
                    let i = self.fresh("i");
                    self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", xs.code));
                    let x = Val::plain(format!("((({ect} *)tv_arr_data({}))[{i}])", xs.code), et);
                    let x = self.coerce(x, ty);
                    let next = pick(&acc, &x.code);
                    self.line(format!("{acc} = {next};"));
                    self.close("}");
                }
                _ => {
                    let v = self.expr(a.expr);
                    let v = self.coerce(v, ty);
                    let next = pick(&acc, &v.code);
                    self.line(format!("{acc} = {next};"));
                }
            }
        }
        Val::plain(acc, ty)
    }

    /// An array literal (or rest arguments) with spreads: `[a, ...xs, b]`.
    pub(crate) fn build_spread(&mut self, elems: &[ExprId], ty: TyId) -> Val {
        let Ty::Array(et) = self.tget(ty) else { return Val::plain("TV_EMPTY_ARR", ty) };
        let d = self.desc(et);
        let ect = self.ctype(et);
        // (no spreads, `Array.of(1, 2, 3)`: its length known, allocated once and written)
        if !elems.is_empty() && !elems.iter().any(|&x| matches!(self.ast(self.cur_m()).expr(x).kind, ExprKind::Spread(_))) {
            let mut codes = Vec::new();
            for &x in elems {
                let v = self.expr(x);
                let v = self.coerce(v, et);
                let code = self.consume(v);
                let n = self.fresh("e");
                self.line(format!("{ect} {n} = {code};"));
                codes.push(n);
            }
            let arr = self.tmp(ty, &format!("tv_arr_new_len(sizeof({ect}), {})", codes.len()), true);
            let w = self.fresh("w");
            self.line(format!("{ect} *{w} = ({ect} *)tv_arr_data({});", arr.code));
            for (i, c) in codes.iter().enumerate() {
                self.line(format!("{w}[{i}] = {c};"));
            }
            return arr;
        }
        // (`[...set]`, `[...xs]`: the set's keys as they're made, or one copy of the array)
        if let [x] = elems
            && let ExprKind::Spread(inner) = self.ast(self.cur_m()).expr(*x).kind
        {
            let src = self.spread_array(inner);
            if matches!(self.tget(src.ty), Ty::Array(e) if e == et) {
                if src.owned {
                    return src;
                }
                return self.tmp(ty, &format!("tv_arr_slice({}, {d}, 0, 0, false, false)", src.code), true);
            }
            let arr = self.tmp(ty, "tv_arr_fresh()", true);
            let Ty::Array(se) = self.tget(src.ty) else { return arr };
            let sct = self.ctype(se);
            let i = self.fresh("i");
            self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", src.code));
            let v = Val::plain(format!("((({sct} *)tv_arr_data({}))[{i}])", src.code), se);
            let v = self.coerce(v, et);
            let n = self.fresh("e");
            self.line(format!("{ect} {n} = {};", v.code));
            let r = self.retain_code(et, &n);
            if !r.is_empty() {
                self.line(format!("{r};"));
            }
            self.line(format!("tv_arr_push(&{}, {d}, &{n});", arr.code));
            self.close("}");
            return arr;
        }
        // (one spread of an array of the same elements, `[...xs, a]`: what comes before it, then
        // it, then one array with room for all of it, its elements copied in before what comes
        // after is evaluated, as JavaScript's order has it)
        let spreads: Vec<usize> = elems.iter().enumerate().filter(|(_, x)| matches!(self.ast(self.cur_m()).expr(**x).kind, ExprKind::Spread(_))).map(|(i, _)| i).collect();
        if let [at] = spreads[..]
            && let ExprKind::Spread(inner) = self.ast(self.cur_m()).expr(elems[at]).kind
            && let it = self.ty(inner)
            && matches!(self.tget(it), Ty::Array(e) if e == et)
        {
            let mut before = Vec::new();
            for &x in &elems[..at] {
                let v = self.expr(x);
                let v = self.coerce(v, et);
                let code = self.consume(v);
                let n = self.fresh("e");
                self.line(format!("{ect} {n} = {code};"));
                before.push(n);
            }
            // (an array is its own spread: the same value, of the type checked above)
            let src = self.spread_array(inner);
            let rest = elems.len() - 1;
            let arr = self.tmp(ty, &format!("tv_arr_with_capacity({d}, tv_arr_len({}) + {rest})", src.code), true);
            for n in &before {
                self.line(format!("tv_arr_push(&{}, {d}, &{n});", arr.code));
            }
            self.line(format!("tv_arr_extend(&{}, {d}, {});", arr.code, src.code));
            for &x in &elems[at + 1..] {
                let v = self.expr(x);
                let v = self.coerce(v, et);
                let code = self.consume(v);
                let n = self.fresh("e");
                self.line(format!("{ect} {n} = {code};"));
                self.line(format!("tv_arr_push(&{}, {d}, &{n});", arr.code));
            }
            return arr;
        }
        let arr = self.tmp(ty, "tv_arr_fresh()", true);
        for &x in elems {
            match self.ast(self.cur_m()).expr(x).kind {
                ExprKind::Spread(inner) => {
                    let src = self.spread_array(inner);
                    let src = if matches!(self.tget(src.ty), Ty::Array(e) if e == et) {
                        src
                    } else {
                        // (elements of another type: converted one by one)
                        let Ty::Array(se) = self.tget(src.ty) else { continue };
                        let sct = self.ctype(se);
                        let i = self.fresh("i");
                        self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", src.code));
                        self.push_temps();
                        let v = Val::plain(format!("((({sct} *)tv_arr_data({}))[{i}])", src.code), se);
                        let v = self.coerce(v, et);
                        let n = self.fresh("e");
                        self.line(format!("{ect} {n} = {};", v.code));
                        let r = self.retain_code(et, &n);
                        if !r.is_empty() {
                            self.line(format!("{r};"));
                        }
                        self.line(format!("tv_arr_push(&{}, {d}, &{n});", arr.code));
                        self.pop_temps();
                        self.close("}");
                        continue;
                    };
                    self.line(format!("tv_arr_extend(&{}, {d}, {});", arr.code, src.code));
                }
                _ => {
                    let v = self.expr(x);
                    let v = self.coerce(v, et);
                    let code = self.consume(v);
                    let n = self.fresh("e");
                    self.line(format!("{ect} {n} = {code};"));
                    self.line(format!("tv_arr_push(&{}, {d}, &{n});", arr.code));
                }
            }
        }
        arr
    }

    /// `new Array<T>(n).fill(v)` (`obj` the `new`, of array type `recv`): the array, made with
    /// n copies of v; `None` when `obj` is something else.
    fn new_filled(&mut self, obj: ExprId, recv: TyId, fill: ExprId, span: Span) -> Option<Val> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let mut o = obj;
        while let ExprKind::Paren(x) = ast.expr(o).kind {
            o = x;
        }
        let ExprKind::New { callee, args, .. } = &ast.expr(o).kind else { return None };
        let ExprKind::Ident(s) = ast.expr(*callee).kind else { return None };
        if self.sym(s) != "Array" || args.len() != 1 || matches!(ast.expr(args[0].expr).kind, ExprKind::Spread(_)) || self.facts(m).filled_arrays.contains(&o) {
            return None;
        }
        let Ty::Array(et) = self.tget(recv) else { return None };
        let at = self.ty(args[0].expr);
        if !(self.c.types.is_int(at) || self.c.types.is_float(at)) || self.ty(o) != recv {
            return None;
        }
        let n_expr = args[0].expr;
        let n = self.expr(n_expr);
        let n = self.int_code(n);
        let v = self.expr(fill);
        let v = self.coerce(v, et);
        let (d, ect) = (self.desc(et), self.ctype(et));
        let loc = self.loc(span);
        // (a fill of zeros: `0`, `false`, `null`; not `-0`, whose bytes aren't)
        if matches!(v.code.as_str(), "0" | "INT64_C(0)" | "0.0" | "false" | "NULL") && !self.is_rc(et) {
            return Some(self.tmp(recv, &format!("tv_arr_zeros(sizeof({ect}), {n}, {loc})"), true));
        }
        let z = self.fresh("z");
        self.line(format!("{ect} {z} = {};", v.code));
        if !self.is_rc(et) {
            // (elements that hold nothing: written here, the count a constant the C compiler
            // can see when it's one)
            let a = self.tmp(recv, &format!("tv_arr_uninit(sizeof({ect}), {n}, {loc})"), true);
            let (dp, i) = (self.fresh("d"), self.fresh("i"));
            self.line(format!("{{ {ect} *{dp} = ({ect} *)tv_arr_data({}); for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {dp}[{i}] = {z}; }}", a.code, a.code));
            return Some(a);
        }
        Some(self.tmp(recv, &format!("tv_arr_make({d}, {n}, &{z}, {loc})"), true))
    }

    /// Every byte of type `t`'s default value is zero (see `default_value`).
    pub(crate) fn zero_default(&mut self, t: TyId) -> bool {
        let zero = self.default_value(t);
        if matches!(zero.as_str(), "0" | "NULL" | "false" | "((tv_arr){NULL})" | "((tv_str){NULL})") {
            return true;
        }
        match self.tget(t) {
            Ty::Union(ms) if self.niche(t).is_none() => {
                let ms = self.c.types.tys(ms).to_vec();
                let i = ms.iter().position(|&m| m == UNDEFINED).unwrap_or(0);
                i == 0 && (self.is_unit(ms[0]) || !self.is_rc(ms[0]) && self.zero_default(ms[0]))
            }
            // (an object's hole: no pointer)
            Ty::Class(..) | Ty::Interface(..) | Ty::Rec(..) => true,
            _ => false,
        }
    }

    /// `new Uint8Array(n)`, `new Float64Array(xs)`, `new Array<number>(n)`, `new Array(a, b)`.
    pub(crate) fn new_array(&mut self, args: &[Arg], ty: TyId, span: Span) -> Val {
        let Ty::Array(et) = self.tget(ty) else { return Val::plain("TV_EMPTY_ARR", ty) };
        if args.len() > 1 {
            let elems: Vec<ExprId> = args.iter().map(|a| a.expr).collect();
            return self.build_spread(&elems, ty);
        }
        let Some(a) = args.first() else { return self.tmp(ty, "tv_arr_fresh()", true) };
        let at = self.ty(a.expr);
        if self.c.types.is_int(at) || self.c.types.is_float(at) {
            let n = self.expr(a.expr);
            let n = self.int_code(n);
            let (d, ect) = (self.desc(et), self.ctype(et));
            let zero = self.default_value(et);
            let loc = self.loc(span);
            // (zeros: `null`, `0`, `false`, an empty array or string slot)
            if self.zero_default(et) {
                return self.tmp(ty, &format!("tv_arr_zeros(sizeof({ect}), {n}, {loc})"), true);
            }
            // (the fill value in a variable: a compound literal can't hold a compound literal)
            let z = self.fresh("z");
            self.line(format!("{ect} {z} = {zero};"));
            return self.tmp(ty, &format!("tv_arr_make({d}, {n}, &{z}, {loc})"), true);
        }
        let src = self.spread_array(a.expr);
        self.convert_array(src, ty)
    }

    /// An array of `ty`'s elements made from one of other elements (converted one by one; a
    /// number stored into a sized integer wraps).
    fn convert_array(&mut self, src: Val, ty: TyId) -> Val {
        if src.ty == ty {
            // (a new array: `Array.from(xs)` isn't `xs`)
            if src.owned {
                return src;
            }
            let d = match self.tget(ty) {
                Ty::Array(et) => self.desc(et),
                _ => return src,
            };
            return self.tmp(ty, &format!("tv_arr_slice({}, {d}, 0, 0, false, false)", src.code), true);
        }
        let (Ty::Array(et), Ty::Array(se)) = (self.tget(ty), self.tget(src.ty)) else { return Val::plain("TV_EMPTY_ARR", ty) };
        let (d, ect, sct) = (self.desc(et), self.ctype(et), self.ctype(se));
        let out = self.tmp(ty, &format!("tv_arr_with_capacity({d}, tv_arr_len({}))", src.code), true);
        let i = self.fresh("i");
        self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", src.code));
        let v = Val::plain(format!("((({sct} *)tv_arr_data({}))[{i}])", src.code), se);
        let v = self.coerce(v, et);
        self.line(format!("TVG_PUSH({ect}, &{}, {d}, {});", out.code, v.code));
        self.close("}");
        out
    }

    /// `Array.from(xs)`, `Array.from(xs, f)` (a `map` of its elements), `Array.from({ length: n }, f)`.
    fn array_from(&mut self, args: &[Arg], ty: TyId, span: Span) -> Val {
        let m = self.cur_m();
        let src = args[0].expr;
        let length = match &self.ast(m).expr(src).kind {
            ExprKind::Object(fs) if fs.len() == 1 && self.sym(fs[0].name) == "length" => Some(fs[0].value),
            _ => None,
        };
        let Ty::Array(rt) = self.tget(ty) else { return Val::plain("TV_EMPTY_ARR", ty) };
        match (length, args.get(1)) {
            (Some(n), Some(f)) => {
                let n = self.expr(n);
                // (JavaScript's ToLength: truncated, and 0 below zero or for NaN)
                let n = if self.c.types.is_float(n.ty) {
                    format!("({{ double d_ = {}; d_ > 0 ? (d_ < 9007199254740991.0 ? (tv_int)d_ : INT64_C(9007199254740991)) : 0; }})", n.code)
                } else {
                    self.int_code(n)
                };
                let cb = self.prepare_callback(f.expr);
                let (rd, rct) = (self.desc(rt), self.ctype(rt));
                let nv = self.fresh("n");
                self.line(format!("tv_int {nv} = {n};"));
                let _ = span;
                self.line(format!("if ({nv} < 0) {nv} = 0;"));
                let out = self.tmp(ty, &format!("tv_arr_with_capacity({rd}, {nv})"), true);
                let i = self.fresh("i");
                self.open_block(&format!("for (tv_int {i} = 0; {i} < {nv}; {i}++) {{"));
                let r = self.invoke(&cb, vec![Val::plain("0", UNDEFINED), Val::plain(i.clone(), INT)]);
                let v = self.coerce(r, rt);
                let code = self.consume(v);
                self.line(format!("TVG_PUSH({rct}, &{}, {rd}, {code});", out.code));
                self.close_block("}");
                out
            }
            (_, Some(f)) => {
                let xs = self.spread_array(src);
                let Ty::Array(se) = self.tget(xs.ty) else { return Val::plain("TV_EMPTY_ARR", ty) };
                self.array_method(xs, se, "map", std::slice::from_ref(f), &[], ty, span)
            }
            _ => {
                let xs = self.spread_array(src);
                self.convert_array(xs, ty)
            }
        }
    }

    /// A map's `[key, value]` entries, as an array of tuples.
    pub(crate) fn map_entries(&mut self, mv: Val, k: TyId, v: TyId, ty: TyId) -> Val {
        let Ty::Array(rt) = self.tget(ty) else { return Val::plain("TV_EMPTY_ARR", ty) };
        let (kd, vd, rd) = (self.desc(k), self.desc(v), self.desc(rt));
        let (kct, vct, rct) = (self.ctype(k), self.ctype(v), self.ctype(rt));
        let f = self.tuple_of(rt).unwrap_or_default();
        if f.len() != 2 {
            return Val::plain("TV_EMPTY_ARR", ty);
        }
        let (f0, f1) = (self.sym(f[0].name).to_string(), self.sym(f[1].name).to_string());
        let m = self.fresh("m");
        self.line(format!("tv_map {m} = {};", mv.code));
        let out = self.tmp(ty, &format!("tv_arr_with_capacity({rd}, tv_map_size({m}))"), true);
        let i = self.fresh("i");
        self.line(format!("tv_int {i} = 0; void *{i}k, *{i}v;"));
        self.open(&format!("while (tv_map_next({m}, {kd}, {vd}, &{i}, &{i}k, &{i}v)) {{"));
        let (rk, rv) = (self.retain_code(k, &format!("{i}x")), self.retain_code(v, &format!("{i}y")));
        self.line(format!("{kct} {i}x = *({kct} *){i}k; {rk}; {vct} {i}y = *({vct} *){i}v; {rv};"));
        let lit = self.rec_lit(rt, &format!(".f_{f0} = {i}x, .f_{f1} = {i}y"));
        self.line(format!("TVG_PUSH({rct}, &{}, {rd}, {lit});", out.code));
        self.close("}");
        out
    }

    /// `...x` as an array: an array itself, a set's values, a string's characters.
    pub(crate) fn spread_array(&mut self, x: ExprId) -> Val {
        let v = self.expr(x);
        let base = self.c.types.without_undefined(v.ty);
        // (an `any`: what it holds, a string's characters or an array's elements, as `any`s)
        if base == ANY {
            self.any_used();
            let arr = self.c.types.array(ANY);
            let loc = self.any_loc();
            let (d, n, i) = (self.fresh("ad"), self.fresh("n"), self.fresh("i"));
            self.line(format!("tv_dyn {d} = {}; tv_int {n} = tvg_any_len({d}); if ({n} < 0) tv_trap(\"TypeError: this value is not iterable\", {loc});", v.code));
            let r = self.tmp(arr, &format!("tv_arr_with_capacity(&tv_type_dyn, {n})"), true);
            self.line(format!("for (tv_int {i} = 0; {i} < {n}; {i}++) {{ tv_dyn k_ = tv_dyn_box(&tv_type_int, &{i}); tv_dyn e_ = tvg_any_at({d}, k_, {loc}); tv_dyn_release(k_); tv_arr_push(&{}, &tv_type_dyn, &e_); }}", r.code));
            return r;
        }
        match self.tget(base) {
            Ty::Set(e) => {
                let arr = self.c.types.array(e);
                let code = format!("tv_map_keys({}, {}, &tv_type_undefined)", v.code, self.desc(e));
                self.tmp(arr, &code, true)
            }
            // (a generator: what it yields, to its end)
            Ty::Gen(e) => {
                let arr = self.c.types.array(e);
                let code = format!("tv_gen_collect({}, {})", v.code, self.desc(e));
                self.tmp(arr, &code, true)
            }
            Ty::Str | Ty::StrLit(_) => {
                let arr = self.c.types.array(STR);
                let s = self.coerce(v, STR);
                self.tmp(arr, &format!("tv_str_chars({})", s.code), true)
            }
            // (a map's `[key, value]` entries)
            Ty::Map(k, val) => {
                let pair = self.c.tuple_ty(&[k, val]);
                let arr = self.c.types.array(pair);
                self.map_entries(v, k, val, arr)
            }
            _ => v,
        }
    }

    // ------------------------------------------------------------ test matchers

    /// `t` holds arrays, records or maps (compared by identity).
    /// A class instance in `t` (in an array, a record, a union).
    fn has_classes(&mut self, t: TyId, stack: &mut Vec<TyId>) -> bool {
        if stack.contains(&t) {
            return false;
        }
        stack.push(t);
        let r = match self.tget(t) {
            Ty::Class(..) => true,
            Ty::Array(e) => self.has_classes(e, stack),
            Ty::Union(ms) => self.c.types.tys(ms).to_vec().into_iter().any(|m| self.has_classes(m, stack)),
            Ty::Record(_) => self.c.types.fields_in_order(t).into_iter().any(|f| self.has_classes(f.ty, stack)),
            _ => false,
        };
        stack.pop();
        r
    }

    fn has_objects(&mut self, t: TyId) -> bool {
        self.members(t).iter().any(|&m| matches!(self.tget(m), Ty::Array(_) | Ty::Map(..) | Ty::Set(_) | Ty::Record(_) | Ty::Rec(..)))
    }

    fn matcher(&mut self, s: Val, name: &str, args: &[Arg], span: Span, negate: bool) {
        // (a literal's type widened: `expect("abc").not.toBe("abd")` compares strings)
        let s = match self.tget(s.ty) {
            Ty::StrLit(_) => self.coerce(s, STR),
            Ty::BoolLit(_) => self.coerce(s, BOOL),
            _ => s,
        };
        let loc = self.loc(span);
        // (what's expected, when there's no value to show: "expected null, received 3")
        let what = match name {
            "toBeUndefined" => "undefined",
            "toBeDefined" => "a defined value",
            "toBeNull" => "null",
            "toBeTruthy" => "a truthy value",
            "toBeFalsy" => "a falsy value",
            "toBeNaN" => "NaN",
            _ => "",
        }
        .to_string();
        let (cond, expected) = match name {
            // (a set or map of plain values: the same entries in any order, as Jest compares them)
            "toEqual" | "toStrictEqual"
                if matches!(self.tget(s.ty), Ty::Set(k) if !self.has_objects(k))
                    || matches!(self.tget(s.ty), Ty::Map(k, v) if !self.has_objects(k) && !self.has_objects(v)) =>
            {
                let x = self.expr(args[0].expr);
                let x = self.coerce(x, s.ty);
                let (kd, vd) = match self.tget(s.ty) {
                    Ty::Set(k) => (self.desc(k), "&tv_type_undefined".to_string()),
                    Ty::Map(k, v) => (self.desc(k), self.desc(v)),
                    _ => unreachable!(),
                };
                (format!("tv_map_eq({}, {}, {kd}, {vd})", s.code, x.code), Some(x))
            }
            // (class instances against objects: alike when their fields print alike, as Jest
            // compares them)
            "toEqual" if crate::check::builtins::has_object_literal(self.ast(self.cur_m()), args[0].expr) && self.has_classes(s.ty, &mut Vec::new()) => {
                let x = self.expr(args[0].expr);
                let (a, b) = (self.fresh("sa"), self.fresh("sb"));
                let ia = self.inspect_code(s.ty, &s.code, "0");
                let ib = self.inspect_code(x.ty, &x.code, "0");
                self.line(format!("tv_sb {a} = {{0}}, {b} = {{0}};"));
                self.line(format!("{{ tv_sb *sb = &{a}; tv_inspect_anon++; {ia}; tv_inspect_anon--; }} {{ tv_sb *sb = &{b}; {ib}; }}"));
                let r = self.fresh("eq");
                self.line(format!("bool {r} = {a}.len == {b}.len && memcmp({a}.data, {b}.data, {a}.len) == 0; tv_sb_free(&{a}); tv_sb_free(&{b});"));
                (r, Some(x))
            }
            // (deep: arrays, records, maps alike when they print alike, as Jest compares them)
            "toEqual" | "toStrictEqual" if self.has_objects(s.ty) => {
                let x = self.expr(args[0].expr);
                let x = self.coerce(x, s.ty);
                let (a, b) = (self.fresh("sa"), self.fresh("sb"));
                let ia = self.inspect_code(s.ty, &s.code, "0");
                let ib = self.inspect_code(s.ty, &x.code, "0");
                self.line(format!("tv_sb {a} = {{0}}, {b} = {{0}};"));
                self.line(format!("{{ tv_sb *sb = &{a}; {ia}; }} {{ tv_sb *sb = &{b}; {ib}; }}"));
                let r = self.fresh("eq");
                self.line(format!("bool {r} = {a}.len == {b}.len && memcmp({a}.data, {b}.data, {a}.len) == 0; tv_sb_free(&{a}); tv_sb_free(&{b});"));
                (r, Some(x))
            }
            // (numbers as `Object.is` compares them: NaN is NaN, 0 isn't -0)
            "toBe" | "toEqual" | "toStrictEqual" if s.ty == F64 => {
                let x = self.expr(args[0].expr);
                let x = self.coerce(x, F64);
                let (a, b) = (self.fresh("a"), self.fresh("b"));
                self.line(format!("double {a} = {}, {b} = {};", s.code, x.code));
                (format!("(({a} == {b} && ({a} != 0 || __builtin_signbit({a}) == __builtin_signbit({b}))) || ({a} != {a} && {b} != {b}))"), Some(x))
            }
            "toBe" | "toEqual" | "toStrictEqual" => {
                let x = self.expr(args[0].expr);
                let x = self.coerce(x, s.ty);
                let eq = self.equality(s.clone(), x.clone(), true);
                (eq, Some(x))
            }
            // (of `undefined` or `null` itself: known)
            "toBeNull" if matches!(self.tget(s.ty), Ty::Undefined | Ty::Null | Ty::Void) => ((if s.ty == NULL { "true" } else { "false" }).into(), None),
            "toBeUndefined" | "toBeDefined" if matches!(self.tget(s.ty), Ty::Undefined | Ty::Null | Ty::Void) => {
                let undef = s.ty != NULL;
                ((if (name == "toBeUndefined") == undef { "true" } else { "false" }).into(), None)
            }
            "toBeNull" => {
                let n = self.coerce(Val::plain("0", NULL), s.ty);
                (self.equality(s.clone(), n, true), None)
            }
            "toBeTruthy" | "toBeFalsy" => {
                let t = self.js_truthy(s.clone());
                (if name == "toBeFalsy" { format!("!({t})") } else { t }, None)
            }
            "toBeNaN" => {
                let v = self.coerce(s.clone(), F64);
                (format!("isnan({})", v.code), None)
            }
            "toBeGreaterThan" | "toBeGreaterThanOrEqual" | "toBeLessThan" | "toBeLessThanOrEqual" => {
                let op = match name {
                    "toBeGreaterThan" => ">",
                    "toBeGreaterThanOrEqual" => ">=",
                    "toBeLessThan" => "<",
                    _ => "<=",
                };
                let x = self.expr(args[0].expr);
                if s.ty == BIGINT {
                    let x = self.coerce(x, BIGINT);
                    (format!("(tv_big_cmp({}, {}) {op} 0)", s.code, x.code), Some(x))
                } else {
                    let x = self.coerce(x, F64);
                    let sv = self.coerce(s.clone(), F64);
                    (format!("(({}) {op} ({}))", sv.code, x.code), Some(x))
                }
            }
            "toHaveLength" => {
                let x = self.expr(args[0].expr);
                let x = self.coerce(x, INT);
                let len = if matches!(self.tget(s.ty), Ty::Array(_)) {
                    format!("tv_arr_len({})", s.code)
                } else {
                    let sv = self.coerce(s.clone(), STR);
                    format!("tv_str_length({})", sv.code)
                };
                let n = self.tmp(INT, &len, false);
                (format!("({} == {})", n.code, x.code), Some(x))
            }
            "toContain" => {
                let x = self.expr(args[0].expr);
                // (of a `T[] | undefined`: not when it's absent)
                let (sv, guard) = if self.c.types.has_nullish(s.ty) {
                    let inner = self.c.types.without_nullish(s.ty);
                    let present = self.truthy_val(s.clone());
                    (self.project(s.clone(), inner), Some(present))
                } else {
                    (s.clone(), None)
                };
                let (cond, x) = if let Ty::Array(et) = self.tget(sv.ty) {
                    let x = self.coerce(x, et);
                    let d = self.desc(et);
                    let xn = self.key_tmp(x.clone(), et);
                    (format!("(tv_arr_index_of({}, {d}, &{xn}) >= 0)", sv.code), Some(x))
                } else {
                    let x = self.coerce(x, STR);
                    let sv = self.coerce(sv, STR);
                    (format!("tv_str_includes({}, {})", sv.code, x.code), Some(x))
                };
                (guard.map(|g| format!("({g} && {cond})")).unwrap_or(cond), x)
            }
            "toThrow" | "toThrowError" => {
                // (called, and what it throws dropped; a trap still fails the test)
                let Ty::Func(ps, ret, _) = self.tget(s.ty) else { return };
                let ps = self.c.types.params(ps).to_vec();
                let cast = self.fn_ptr_type(&ps, ret);
                let fv = self.fresh("fv");
                self.line(format!("tv_fn {fv} = {};", s.code));
                let call = format!("(({cast}){fv}.fn)({fv}.env)");
                if ret == VOID {
                    self.line(format!("{call};"));
                } else {
                    let rct = self.ctype(ret);
                    let r = self.fresh("r");
                    self.line(format!("{rct} {r} = {call};"));
                    if self.is_rc(ret) {
                        let rel = self.release_code(ret, &r);
                        self.line(format!("if (!tvg_err) {rel};"));
                    }
                }
                let thrown = self.fresh("threw");
                // (`toThrow(RangeError)`: an error of that class; `toThrow("empty")`: one whose
                // message contains it)
                let m = self.cur_m();
                let err = self.c.error_class();
                let mut what = String::new();
                let arg_ty = args.first().map(|a| self.ty(a.expr)).unwrap_or(ERROR);
                let check = match args {
                    [a] if let Some(crate::check::IdentFact::Class(c)) = self.ident_fact(m, a.expr) => {
                        what = format!(" a {}", self.c.class_names[c as usize]);
                        let ect = if err == ERROR { "void *".to_string() } else { self.ctype(err) };
                        Some(self.instanceof_code(Val::plain(format!("(({ect})tvg_err)"), err), c))
                    }
                    [a] if err != ERROR && self.c.types.is_string(arg_ty) => {
                        let sv = self.expr(a.expr);
                        let sv = self.coerce(sv, STR);
                        let msym = self.c.interner.lookup("message");
                        match msym.and_then(|ms| self.c.class_member(err, ms)) {
                            Some(crate::check::class::ClassMemberRef::Field(cf)) => {
                                let ect = self.ctype(err);
                                let lv = self.class_field_lv_pub(&format!("(({ect})tvg_err)"), err, &cf);
                                what = " an error with that message".into();
                                Some(format!("tv_str_includes({lv}, {})", sv.code))
                            }
                            _ => None,
                        }
                    }
                    _ => None,
                };
                let ok = match check {
                    Some(c) => format!("(tvg_err != NULL && {c})"),
                    None => "(tvg_err != NULL)".to_string(),
                };
                self.line(format!("bool {thrown} = {ok}; if (tvg_err) {{ tvg_obj_release(tvg_err); tvg_err = NULL; }}"));
                let msg = if negate { format!("expected the function not to throw{what}") } else { format!("expected the function to throw{what}") };
                let cond = if negate { format!("!{thrown}") } else { thrown };
                self.line(format!("if (!({cond})) {{ tv_sb msg_ = {{0}}; tv_sb_push_cstr(&msg_, {}); tv_expect_fail(&msg_, {loc}); }}", c_string(msg.as_bytes())));
                return;
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
            "toBeUndefined" | "toBeDefined" if s.ty == UNKNOWN || s.ty == ANY => {
                let t = format!("(({}).t != &tv_type_undefined)", s.code);
                (if name == "toBeUndefined" { format!("!({t})") } else { t }, None)
            }
            "toBeUndefined" | "toBeDefined" => {
                let t = self.truthy_val(s.clone());
                (if name == "toBeUndefined" { format!("!({t})") } else { t }, None)
            }
            _ => ("true".into(), None),
        };
        let cond = if negate { format!("!({cond})") } else { cond };
        let not = if negate { "not " } else { "" };
        let relation = match name {
            "toBeGreaterThan" => "a value > ",
            "toBeGreaterThanOrEqual" => "a value >= ",
            "toBeLessThan" => "a value < ",
            "toBeLessThanOrEqual" => "a value <= ",
            "toHaveLength" => "length ",
            "toContain" => "to contain ",
            _ => "",
        };
        self.line(format!("if (!({cond})) {{"));
        self.bump(1);
        self.line("tv_sb msg_ = {0}; tv_sb *sb = &msg_;");
        match &expected {
            Some(x) => {
                let head = format!("expected {not}{relation}");
                self.line(format!("tv_sb_push_cstr(sb, {});", c_string(head.as_bytes())));
                let c = self.inspect_root(x.ty, &x.code, "1");
                self.line(format!("{c};"));
                self.line("tv_sb_push_cstr(sb, \", received \");");
            }
            None => {
                let head = format!("expected {not}{what}, received ");
                self.line(format!("tv_sb_push_cstr(sb, {});", c_string(head.as_bytes())));
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

impl Gen<'_, '_> {
    /// The call whose arguments are `args` can't release an object of type `t` (see `writes`).
    pub(crate) fn call_spares_arg(&mut self, args: &[Arg], t: TyId) -> bool {
        match self.cur_calls.last() {
            Some(&(k, m, e)) if !args.is_empty() && k == args.as_ptr() as usize => self.call_spares(m, e, t),
            _ => false,
        }
    }
}

/// `t` is held as a tagged union (not a pointer whose null is one member).
fn niche_free(g: &Gen<'_, '_>, t: TyId) -> bool {
    g.niche(t).is_none()
}
