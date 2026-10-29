//! Async functions. An async function `f` compiles to:
//! - a frame `AF_f`: `pc` (the label to resume at), `ret`, the parameters and, after
//!   `frame::lower`, every local and temporary, plus a union `u` of the frames of the async calls
//!   it awaits directly that can wait themselves (only one is in progress at a time; a call that
//!   can't wait has its frame on the C stack);
//! - `f(AF_f *F)`: runs from `F->pc`, returns true once finished (the result in `F->ret`, or an
//!   error in `tvg_err`) and false when it waits, after arranging to be resumed;
//! - `f_task` / `f_spawn`: runs it as a task of its own, for calls that aren't awaited (the task's
//!   promise is the call's value).
//! A task resumes from its root frame, and every frame re-enters its child at the pending
//! `await`, so no frame needs a pointer to its parent and none ever moves.

use super::{Gen, Instance, Val};
use crate::ast::{self, ExprId, ExprKind};
use crate::check::Callee;
use crate::hash::{FxMap, FxSet};
use crate::types::*;
use std::fmt::Write;

/// A frame struct: (name, frames it embeds, definition).
pub(crate) struct FrameDef {
    pub name: String,
    pub deps: Vec<String>,
    pub text: String,
}

impl<'c, 'a> Gen<'c, 'a> {
    /// Is (module, item) an async function?
    pub(crate) fn is_async_fn(&self, m: u32, item: u32) -> bool {
        matches!(&self.ast(m).items[item as usize].kind, ast::ItemKind::Function(f) if f.is_async && !f.is_gen)
    }

    /// The (instantiated) type an async function's promise resolves to.
    pub(crate) fn async_value_type(&mut self, ret: TyId) -> TyId {
        match self.tget(ret) {
            Ty::Promise(v, _) => v,
            _ => ret,
        }
    }

    pub(super) fn emit_async_function(&mut self, inst: Instance) {
        let ast::ItemKind::Function(f) = &self.ast(inst.m).items[inst.item as usize].kind else { return };
        let sig = self.c.sigs[&(inst.m, inst.item)].clone();
        let cname = inst.cname.clone();
        let frame = format!("AF_{cname}");
        let params: Vec<(u32, TyId, bool)> = f.params.iter().zip(&sig.params).map(|(p, fp)| (p.span.start, self.c.types.subst(fp.ty, &inst.subst), false)).collect();
        let ret_p = self.c.types.subst(sig.ret, &inst.subst);
        let ret = self.async_value_type(ret_p);
        self.pending_async = Some((inst.m, inst.item));
        let code = self.function_body(inst.m, inst.subst.clone(), &params, ret, super::body::FnBodyKind::Block(f.body), None, None, None);
        let ptys: Vec<TyId> = params.iter().map(|p| p.1).collect();
        if !self.async_parts(&cname, &format!("{cname}_task"), &frame, &code, &ptys, ret, &[]) {
            return;
        }
        // Started as a task of its own (a call that isn't awaited).
        let proto = self.spawn_proto(&cname, &params);
        let mut set = String::new();
        for (i, ty) in ptys.iter().enumerate() {
            let r = if self.is_rc(*ty) { format!(" {};", self.retain_code(*ty, &format!("p{i}"))) } else { String::new() };
            let _ = write!(set, " F->p{i} = p{i};{r}");
        }
        let desc = self.desc(ret);
        let _ = writeln!(
            self.funcs,
            "static {proto} {{\n    tv_task *t = tv_task_new(sizeof({frame}), {cname}_task, {desc});\n    {frame} *F = tv_task_frame(t);{set}\n    return tv_task_spawn(t);\n}}\n"
        );
    }

    /// An async arrow: its closure function starts a task (holding the environment) and returns
    /// the task's promise.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn emit_async_arrow(&mut self, cname: &str, m: u32, subst: FxMap<u32, TyId>, params: &[(u32, TyId, bool)], ret_p: TyId, kind: super::body::FnBodyKind, env_locals: FxMap<u32, super::body::Local>) {
        let ret = self.async_value_type(ret_p);
        let frame = format!("AF_{cname}");
        let _ = writeln!(self.typedefs, "typedef struct {frame} {frame};");
        self.pending_async = Some((u32::MAX, self.counter));
        let code = self.arrow_body_pub(m, subst, params, ret, kind, env_locals);
        let ptys: Vec<TyId> = params.iter().map(|p| p.1).collect();
        let resume = format!("{cname}_resume");
        let _ = writeln!(self.protos, "static bool {resume}({frame} *F);\nstatic bool {cname}_task(tv_task *t);");
        let env = [("tv_env *".to_string(), "env_".to_string(), "tv_env_release(F->env_);".to_string())];
        if !self.async_parts(&resume, &format!("{cname}_task"), &frame, &code, &ptys, ret, &env) {
            return;
        }
        let mut ps = vec!["tv_env *env_".to_string()];
        let mut set = String::from(" F->env_ = env_; tv_env_retain(env_);");
        for (i, ty) in ptys.iter().enumerate() {
            ps.push(format!("{} p{i}", self.ctype(*ty)));
            let r = if self.is_rc(*ty) { format!(" {};", self.retain_code(*ty, &format!("p{i}"))) } else { String::new() };
            let _ = write!(set, " F->p{i} = p{i};{r}");
        }
        let desc = self.desc(ret);
        let _ = writeln!(
            self.funcs,
            "static tv_promise *{cname}({}) {{\n    tv_task *t = tv_task_new(sizeof({frame}), {cname}_task, {desc});\n    {frame} *F = tv_task_frame(t);{set}\n    return tv_task_spawn(t);\n}}\n",
            ps.join(", ")
        );
    }

    /// An async method (or static method): its C function (`proto`, what callers and dispatch
    /// call) starts a task holding `self_` and the arguments, and returns the task's promise.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn emit_async_method(&mut self, cname: &str, proto: &str, m: u32, subst: FxMap<u32, TyId>, params: &[(u32, TyId, bool)], ret_p: TyId, body: crate::ast::StmtId, this: Option<(u32, TyId, String)>) {
        let ret = self.async_value_type(ret_p);
        let frame = format!("AF_{cname}");
        let resume = format!("{cname}_resume");
        let _ = writeln!(self.typedefs, "typedef struct {frame} {frame};");
        let _ = writeln!(self.protos, "static bool {resume}({frame} *F);\nstatic bool {cname}_task(tv_task *t);");
        self.pending_async = Some((u32::MAX - 1, self.counter));
        let this_arg = this.as_ref().map(|(k, t, _)| (*k, *t));
        let code = self.function_body(m, subst, params, ret, super::body::FnBodyKind::Block(body), None, this_arg, None);
        let ptys: Vec<TyId> = params.iter().map(|p| p.1).collect();
        let extra: Vec<(String, String, String)> = match &this {
            Some((_, _, st)) => vec![(format!("{st} *"), "self_".to_string(), "tvg_obj_release(F->self_);".to_string())],
            None => Vec::new(),
        };
        if !self.async_parts(&resume, &format!("{cname}_task"), &frame, &code, &ptys, ret, &extra) {
            return;
        }
        let mut set = String::new();
        if let Some((_, t, _)) = &this {
            // The task holds (and later releases, through the class id) its own reference.
            self.rc_roots.insert(*t);
            set.push_str(" F->self_ = self_; tvg_obj_retain(self_);");
        }
        for (i, ty) in ptys.iter().enumerate() {
            let r = if self.is_rc(*ty) { format!(" {};", self.retain_code(*ty, &format!("p{i}"))) } else { String::new() };
            let _ = write!(set, " F->p{i} = p{i};{r}");
        }
        let desc = self.desc(ret);
        let _ = writeln!(self.funcs, "static {proto} {{\n    tv_task *t = tv_task_new(sizeof({frame}), {cname}_task, {desc});\n    {frame} *F = tv_task_frame(t);{set}\n    return tv_task_spawn(t);\n}}\n");
    }

    /// The frame, resume function and task function of an async body; false after an internal
    /// error. `extra`: more frame fields the body uses (C type, name, release when the task
    /// ends): an async arrow's closure environment (`env_`), a method's `self_`.
    #[allow(clippy::too_many_arguments)]
    fn async_parts(&mut self, resume: &str, task: &str, frame: &str, code: &str, params: &[TyId], ret: TyId, extra: &[(String, String, String)]) -> bool {
        let children = std::mem::take(&mut self.last_children);
        let mut fields: Vec<super::frame::Field> = params.iter().enumerate().map(|(i, ty)| super::frame::Field { ty: self.ctype(*ty), name: format!("p{i}") }).collect();
        fields.extend(extra.iter().map(|e| super::frame::Field { ty: e.0.trim_end().to_string(), name: e.1.clone() }));
        let lowered = match super::frame::lower(code, &fields) {
            Ok(l) => l,
            Err(msg) => {
                self.internal.push(format!("async function `{resume}`: {msg}"));
                return false;
            }
        };
        let mut def = format!("struct {frame} {{\n    void *pc;\n");
        if ret != VOID {
            let ct = self.ctype(ret);
            let _ = writeln!(def, "    {ct} ret;");
        }
        for (ct, name, _) in extra {
            let _ = writeln!(def, "    {ct} {name};");
        }
        for (i, ty) in params.iter().enumerate() {
            let ct = self.ctype(*ty);
            let _ = writeln!(def, "    {ct} p{i};");
        }
        for fl in &lowered.fields {
            let _ = writeln!(def, "    {} {};", fl.ty, fl.name);
        }
        let mut deps = Vec::new();
        if !children.is_empty() {
            def.push_str("    union {\n");
            for (member, ty) in &children {
                let _ = writeln!(def, "        {ty} {member};");
                deps.push(ty.clone());
            }
            def.push_str("    } u;\n");
        }
        def.push_str("};\n");
        self.frame_defs.push(FrameDef { name: frame.to_string(), deps, text: def });
        // (A body without `await` never suspends: it has nowhere to resume.)
        let dispatch = if lowered.body.contains("&&aw") { "    if (F->pc) goto *F->pc;\n" } else { "    (void)F;\n" };
        let _ = writeln!(self.funcs, "static bool {resume}({frame} *F) {{\n{}{dispatch}{}    return true;\n}}\n", lowered.prologue, lowered.body);
        // Run as a task: settle the task's promise, then release what the frame was given.
        let mut release = String::new();
        for (i, ty) in params.iter().enumerate() {
            if self.is_rc(*ty) {
                let r = self.release_code(*ty, &format!("F->p{i}"));
                let _ = write!(release, " {r};");
            }
        }
        for (_, _, rel) in extra {
            let _ = write!(release, " {rel}");
        }
        let settle = if ret == VOID { "tv_promise_resolve_move(t->promise, NULL);".to_string() } else { "tv_promise_resolve_move(t->promise, &F->ret);".to_string() };
        let _ = writeln!(
            self.funcs,
            "static bool {task}(tv_task *t) {{\n    {frame} *F = tv_task_frame(t);\n    if (!{resume}(F)) return false;\n    if (tvg_err) {{ tv_promise_reject(t->promise, tvg_err); tvg_err = NULL; }} else {settle}\n   {release}\n    return true;\n}}\n"
        );
        true
    }

    /// `tv_promise *f_spawn(params)`.
    fn spawn_proto(&mut self, cname: &str, params: &[(u32, TyId, bool)]) -> String {
        let ps: Vec<String> = params.iter().enumerate().map(|(i, (_, ty, _))| format!("{} p{i}", self.ctype(*ty))).collect();
        let ps = if ps.is_empty() { "void".to_string() } else { ps.join(", ") };
        format!("tv_promise *{cname}_spawn({ps})")
    }

    /// Declarations for an async function instance (its frame type, resume and spawn functions).
    pub(crate) fn declare_async(&mut self, cname: &str, sig_params: &[FnParam], subst: &FxMap<u32, TyId>) {
        let frame = format!("AF_{cname}");
        let _ = writeln!(self.typedefs, "typedef struct {frame} {frame};");
        let params: Vec<(u32, TyId, bool)> = sig_params.iter().map(|p| (0, self.c.types.subst(p.ty, subst), false)).collect();
        let proto = self.spawn_proto(cname, &params);
        let _ = writeln!(self.protos, "static bool {cname}({frame} *F);\nstatic bool {cname}_task(tv_task *t);\nstatic {proto};");
    }

    /// Frame structs in an order C accepts: embedded frames before the frames embedding them.
    pub(crate) fn ordered_frames(&self) -> String {
        let mut out = String::new();
        let mut done: FxSet<&str> = FxSet::default();
        fn visit<'x>(name: &'x str, defs: &'x [FrameDef], done: &mut FxSet<&'x str>, out: &mut String) {
            if !done.insert(name) {
                return;
            }
            let Some(d) = defs.iter().find(|d| d.name == name) else { return };
            for dep in &d.deps {
                visit(dep, defs, done, out);
            }
            out.push_str(&d.text);
        }
        for d in &self.frame_defs {
            visit(&d.name, &self.frame_defs, &mut done, &mut out);
        }
        out
    }

    /// Does awaiting (module, item) lead back to `target` through direct awaits? Then its frame
    /// can't be embedded (it would contain itself).
    fn awaits_reach(&self, from: (u32, u32), target: (u32, u32), seen: &mut FxSet<(u32, u32)>) -> bool {
        if from == target {
            return true;
        }
        if !seen.insert(from) {
            return false;
        }
        let (m, item) = from;
        let ast::ItemKind::Function(f) = &self.ast(m).items[item as usize].kind else { return false };
        let mut exprs = Vec::new();
        super::body::collect_exprs_stmt(self.ast(m), f.body, &mut exprs);
        for e in exprs {
            let ExprKind::Await(x) = self.ast(m).expr(e).kind else { continue };
            if let Some((cm, ci)) = self.direct_async_call(m, x)
                && self.awaits_reach((cm, ci), target, seen)
            {
                return true;
            }
        }
        false
    }

    /// Can (module, item), an async function, wait? Only an `await` in its body suspends it.
    fn may_suspend(&self, m: u32, item: u32) -> bool {
        let ast::ItemKind::Function(f) = &self.ast(m).items[item as usize].kind else { return true };
        let mut exprs = Vec::new();
        super::body::collect_exprs_stmt(self.ast(m), f.body, &mut exprs);
        exprs.iter().any(|&e| matches!(self.ast(m).expr(e).kind, ExprKind::Await(_)))
    }

    /// `x` is a call to a module async function: (module, item).
    fn direct_async_call(&self, m: u32, mut x: ExprId) -> Option<(u32, u32)> {
        while let ExprKind::Paren(inner) = self.ast(m).expr(x).kind {
            x = inner;
        }
        let ExprKind::Call { .. } = self.ast(m).expr(x).kind else { return None };
        match self.facts(m).calls.get(&x)?.callee {
            Callee::Fn(fm, fi) if self.is_async_fn(fm, fi) => Some((fm, fi)),
            _ => None,
        }
    }

    /// `await x` inside an async function body.
    pub(crate) fn await_expr(&mut self, e: ExprId, x: ExprId) -> Val {
        let m = self.cur_m();
        let ty = self.ty(e);
        let Some(cur) = self.b().async_fn else {
            let span = self.expr_span(e);
            self.unsupported(span, "`await` here");
            return Val::plain("0", ty);
        };
        let n = self.fresh("");
        if let Some((cm, ci)) = self.direct_async_call(m, x)
            && !self.awaits_reach((cm, ci), cur, &mut FxSet::default())
        {
            return self.await_embedded(e, x, n);
        }
        if let Some(v) = self.await_resolved(x, n.clone()) {
            return self.coerce(v, ty);
        }
        let v = self.expr(x);
        let v = self.own(v);
        if v.ty == JS {
            // a JavaScript value: a Tov promise settled with it (or with its rejection)
            let pt = self.c.types.promise(JS, NEVER);
            let pv = self.tmp(pt, &format!("tv_js_await({})", v.code), true);
            let p = pv.code.clone();
            self.line(format!("if (!tv_await_now({p})) {{ F->pc = &&aw{n}; tv_await_suspend({p}); TV_SUSPEND; }}"));
            self.line(format!("aw{n}:;"));
            return self.take_settled(e, &p, JS, ty);
        }
        let value_ty = self.c.types.without_undefined(v.ty);
        let Ty::Promise(inner, _) = self.tget(value_ty) else {
            // `await value`: the value, one tick later.
            self.line(format!("if (!tv_async_eager()) {{ F->pc = &&aw{n}; tv_task_yield(); TV_SUSPEND; }}"));
            self.line(format!("aw{n}:;"));
            return self.coerce(v, ty);
        };
        if value_ty != v.ty {
            let span = self.expr_span(e);
            self.unsupported(span, "awaiting an optional promise");
            return Val::plain("0", ty);
        }
        let p = v.code.clone();
        self.line(format!("if (!tv_await_now({p})) {{ F->pc = &&aw{n}; tv_await_suspend({p}); TV_SUSPEND; }}"));
        self.line(format!("aw{n}:;"));
        self.take_settled(e, &p, inner, ty)
    }

    /// `await Promise.resolve(v)` of a number, string or the like: `await v`, the value one tick
    /// later either way, without making the promise.
    fn await_resolved(&mut self, x: ExprId, n: String) -> Option<Val> {
        let m = self.cur_m();
        let ExprKind::Call { args, .. } = &self.ast(m).expr(x).kind else { return None };
        let arg = match args.as_slice() {
            [] => None,
            [a] if !matches!(self.ast(m).expr(a.expr).kind, ExprKind::Spread(_)) => Some(a.expr),
            _ => return None,
        };
        let Some(Callee::Builtin { ns: Some(ns), name }) = self.facts(m).calls.get(&x).map(|c| c.callee.clone()) else { return None };
        if self.sym(ns) != "Promise" || self.sym(name) != "resolve" {
            return None;
        }
        let xt = self.ty(x);
        let pt = self.c.types.without_undefined(xt);
        let Ty::Promise(value, _) = self.tget(pt) else { return None };
        let plain = matches!(
            self.tget(value),
            Ty::Void | Ty::Undefined | Ty::Null | Ty::Bool | Ty::BoolLit(_) | Ty::Int | Ty::F64 | Ty::F32 | Ty::I8 | Ty::I16 | Ty::I32
                | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64 | Ty::BigInt | Ty::Decimal | Ty::Str | Ty::StrLit(_)
        );
        if !plain {
            return None;
        }
        let v = match arg {
            Some(a) => {
                let v = self.expr(a);
                let v = self.coerce(v, value);
                self.own(v)
            }
            None => Val::plain("0", value),
        };
        self.line(format!("if (!tv_async_eager()) {{ F->pc = &&aw{n}; tv_task_yield(); TV_SUSPEND; }}"));
        self.line(format!("aw{n}:;"));
        Some(v)
    }

    /// The value of a settled promise (a rejection sets `tvg_err`, checked when `e` can throw).
    fn take_settled(&mut self, e: ExprId, p: &str, inner: TyId, ty: TyId) -> Val {
        let m = self.cur_m();
        let throwing = self.facts(m).throwing.contains(&e);
        if inner == VOID || self.is_unit(inner) {
            if throwing {
                self.line(format!("if ({p}->state == TV_REJECTED) {{ tvg_err = {p}->err; tvg_obj_retain(tvg_err); }}"));
                self.error_check();
            }
            return Val::plain("0", ty);
        }
        let ct = self.ctype(inner);
        let d = self.default_value(inner);
        let res = self.tmp(inner, &d, true);
        let r = self.retain_code(inner, &res.code);
        self.line(format!("if ({p}->state == TV_FULFILLED) {{ {} = *({ct} *)tv_promise_value({p}); {r}; }} else {{ tvg_err = {p}->err; tvg_obj_retain(tvg_err); }}", res.code));
        if throwing {
            self.error_check();
        }
        self.coerce(res, ty)
    }

    /// `await f(args)` with `f` an async module function: `f`'s frame is embedded in this one.
    fn await_embedded(&mut self, e: ExprId, x: ExprId, n: String) -> Val {
        let m = self.cur_m();
        let ty = self.ty(e);
        let mut call = x;
        while let ExprKind::Paren(inner) = self.ast(m).expr(call).kind {
            call = inner;
        }
        let ExprKind::Call { args, .. } = &self.ast(m).expr(call).kind else { unreachable!() };
        let fact = self.facts(m).calls.get(&call).cloned().expect("call fact");
        let Callee::Fn(fm, fi) = fact.callee else { unreachable!() };
        let mut subst = FxMap::default();
        for (p, t) in &fact.targs {
            let t = self.inst(*t);
            subst.insert(*p, t);
        }
        let child = self.instance(fm, fi, subst);
        let params: Vec<FnParam> = fact.params.iter().map(|p| FnParam { ty: self.inst(p.ty), ..*p }).collect();
        // The arguments stay alive (owned by this statement) while the call may be suspended.
        let mut argv = Vec::new();
        for (i, a) in args.iter().enumerate() {
            let p = params.get(i).copied().unwrap_or(FnParam { ty: ERROR, inout: false, optional: false });
            let v = self.expr(a.expr);
            let v = self.coerce(v, p.ty);
            let v = self.own(v);
            argv.push(v.code);
        }
        for p in params.iter().skip(args.len()) {
            let v = self.coerce(Val::plain("0", UNDEFINED), p.ty);
            argv.push(v.code);
        }
        let ret = self.inst(fact.ret);
        let inner = self.async_value_type(ret);
        let throwing = self.facts(m).throwing.contains(&e);
        let unit = inner == VOID || self.is_unit(inner);
        // The callee's result is owned (its error path leaves a valid default), and taken now.
        let v = if !self.may_suspend(fm, fi) {
            // A call that can't wait runs to its end now: its frame is a C local (`frame.rs`
            // leaves declarations inside an expression where they are), which the C compiler
            // dissolves into registers when it inlines the call.
            let cf = format!("cf{n}");
            let mut call = format!("({{ AF_{child} {cf} = {{0}};");
            for (i, a) in argv.iter().enumerate() {
                let _ = write!(call, " {cf}.p{i} = {a};");
            }
            let _ = write!(call, " {child}(&{cf}); {} }})", if unit { "0;".to_string() } else { format!("{cf}.ret;") });
            if unit {
                self.line(format!("(void){call};"));
                None
            } else {
                Some(self.tmp(inner, &call, true))
            }
        } else {
            let member = format!("aw{n}");
            self.b().children.push((member.clone(), format!("AF_{child}")));
            let fr = format!("F->u.{member}");
            self.line(format!("memset(&{fr}, 0, sizeof {fr});"));
            for (i, a) in argv.iter().enumerate() {
                self.line(format!("{fr}.p{i} = {a};"));
            }
            // Resume at `aw<n>` while the call is in progress. The first run of the call has a
            // path of its own, so where nothing waits no jump lands between setting its arguments
            // and reading its result, and the C compiler keeps them in registers.
            self.line(format!("if (!{child}(&{fr})) {{ F->pc = &&aw{n}; TV_SUSPEND; }}"));
            self.line(format!("if (0) {{ aw{n}:; if (!{child}(&{fr})) TV_SUSPEND; }}"));
            (!unit).then(|| self.tmp(inner, &format!("{fr}.ret"), true))
        };
        // Once the call has finished, the promise it stands for would take a tick to reach this
        // `await` (its error waits in `e<n>` meanwhile).
        let en = format!("e{n}");
        self.line(format!("void *{en} = NULL;"));
        self.line(format!("if (!tv_async_eager()) {{ {en} = tvg_err; tvg_err = NULL; F->pc = &&aw{n}t; tv_task_yield(); TV_SUSPEND; }}"));
        self.line(format!("if (0) {{ aw{n}t:; tvg_err = {en}; }}"));
        let v = match v {
            Some(v) => self.coerce(v, ty),
            None => Val::plain("0", ty),
        };
        if throwing {
            self.error_check();
        }
        v
    }

    /// `new Promise((resolve, reject) => ...)`: the executor runs now with two closures that
    /// settle the promise (each holds a reference to it).
    pub(crate) fn new_promise(&mut self, args: &[ast::Arg], ty: TyId) -> Val {
        let Ty::Promise(value, _) = self.tget(ty) else { return Val::plain("NULL", ty) };
        let desc = self.desc(value);
        let p = self.tmp(ty, &format!("tv_promise_new({desc})"), true);
        let resolve = self.resolver_thunk(value);
        let Some(arg) = args.first() else { return p };
        let exec = self.expr(arg.expr);
        let (fv, rs, rj) = (self.fresh("fn"), self.fresh("rs"), self.fresh("rj"));
        self.line(format!("tv_fn {fv} = {};", exec.code));
        self.line(format!("tv_fn {rs} = {{ (void *){resolve}, tv_promise_resolver({}) }};", p.code));
        self.line(format!("tv_fn {rj} = {{ (void *)tvg_reject, tv_promise_resolver({}) }};", p.code));
        // (called with the parameters it declares, `(resolve) => ...` with one, and its return type)
        let (np, ret) = match self.tget(exec.ty) {
            Ty::Func(ps, ret, _) => (self.c.types.params(ps).len().min(2), ret),
            _ => (2, VOID),
        };
        let rct = if ret == VOID { "void".to_string() } else { self.ctype(ret) };
        let (ps, argv) = (["tv_env *", "tv_fn", "tv_fn"][..np + 1].join(", "), [format!("{fv}.env"), rs.clone(), rj.clone()][..np + 1].join(", "));
        let call = format!("(({rct} (*)({ps})){fv}.fn)({argv})");
        if ret == VOID {
            self.line(format!("{call};"));
        } else {
            let _ = self.tmp(ret, &call, true);
        }
        self.line(format!("tv_env_release({rs}.env); tv_env_release({rj}.env);"));
        p
    }

    /// `resolve(value)` for promises of `value`'s type.
    fn resolver_thunk(&mut self, value: TyId) -> String {
        let name = format!("tvg_resolve_{}", value.0);
        if self.helpers_done.insert((value, super::H_RESOLVE)) {
            let ct = self.ctype(value);
            let _ = writeln!(self.protos, "static void {name}(tv_env *env, {ct} v);");
            let _ = writeln!(self.helpers, "static void {name}(tv_env *env, {ct} v) {{ tv_promise_resolve(tv_resolver_promise(env), &v); }}");
        }
        if self.helpers_done.insert((VOID, super::H_REJECT)) {
            let _ = writeln!(self.protos, "static void tvg_reject(tv_env *env, void *err);");
            let _ = writeln!(self.helpers, "static void tvg_reject(tv_env *env, void *err) {{ tvg_obj_retain(err); tv_promise_reject(tv_resolver_promise(env), err); }}");
        }
        name
    }

    /// Does evaluating `e` await (outside nested functions), in an async body?
    pub(crate) fn awaits_in(&self, e: ExprId) -> bool {
        self.bodies.last().is_some_and(|b| b.async_fn.is_some()) && has_await(self.ast(self.cur_m()), e)
    }

    /// Can evaluating `e` change what an earlier operand read: does it await (another task may
    /// run), or assign (`x += 1`, `i++`)?
    pub(crate) fn changes_in(&self, e: ExprId) -> bool {
        self.awaits_in(e) || has_assign(self.ast(self.cur_m()), e)
    }

    /// Evaluates `e`; if one of `later` awaits or assigns, reads its value now (JavaScript
    /// evaluates operands left to right: `f(i, i++)` passes the old `i` first).
    pub(crate) fn expr_before(&mut self, e: ExprId, later: &[ExprId]) -> Val {
        let v = self.expr(e);
        // (or calls something that may change what it reads: module state, a static field, a
        // local a closure shares, or anything reached through an object: `f(g, bump())`)
        let later_calls = later.iter().any(|&l| has_call(self.ast(self.cur_m()), l));
        if later.iter().any(|&l| self.changes_in(l)) || (!v.owned && later_calls && self.reads_shared(e)) { self.snapshot(v) } else { v }
    }

    /// Can a call change what `e` reads (see `expr_before`)?
    fn reads_shared(&mut self, e: ExprId) -> bool {
        let m = self.cur_m();
        let ast = self.ast(m);
        match &ast.expr(e).kind {
            ExprKind::Int(_) | ExprKind::Float(_) | ExprKind::Str(_) | ExprKind::BigInt(_) | ExprKind::Decimal(_) | ExprKind::Bool(_) | ExprKind::Null | ExprKind::Undefined | ExprKind::Arrow(_) => false,
            ExprKind::Ident(_) => match self.ident_fact(m, e) {
                Some(super::IdentFact::Local(k)) => self.b().boxed.contains(&k),
                Some(super::IdentFact::Const(cm, ci)) => self.mutated_globals.contains(&(cm, ci)),
                _ => false,
            },
            ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Unary(_, x) | ExprKind::As(x, _) | ExprKind::Typeof(x) => self.reads_shared(*x),
            ExprKind::Binary(_, a, b) => self.reads_shared(*a) || self.reads_shared(*b),
            ExprKind::Cond(a, b, c) => self.reads_shared(*a) || self.reads_shared(*b) || self.reads_shared(*c),
            ExprKind::Template(_, xs) | ExprKind::Array(xs) => xs.iter().any(|&x| self.reads_shared(x)),
            // (their results are temporaries of their own)
            ExprKind::Call { .. } | ExprKind::New { .. } | ExprKind::Await(_) => false,
            _ => true,
        }
    }

    /// Evaluates operands left to right (see `expr_before`).
    pub(crate) fn exprs_in_order(&mut self, es: &[ExprId]) -> Vec<Val> {
        (0..es.len()).map(|i| self.expr_before(es[i], &es[i + 1..])).collect()
    }

    /// The value now, in a temporary of its own.
    pub(crate) fn snapshot(&mut self, v: Val) -> Val {
        if v.owned {
            return v;
        }
        if self.is_rc(v.ty) { self.own(v) } else { self.tmp(v.ty, &v.code.clone(), false) }
    }

    /// A call to an async function that isn't awaited: it runs as a task; its promise is the value.
    pub(crate) fn spawn_call(&mut self, cname: &str, argv: &[String], ret: TyId, ty: TyId) -> Val {
        let v = self.tmp(ret, &format!("{cname}_spawn({})", argv.join(", ")), true);
        self.coerce(v, ty)
    }
}

/// Does `e` call anything (or await) outside nested functions?
pub(crate) fn has_call(ast: &ast::Ast, e: ExprId) -> bool {
    let any = |xs: &[ExprId]| xs.iter().any(|&x| has_call(ast, x));
    match &ast.expr(e).kind {
        // (a `yield` runs the generator's consumer meanwhile, as a call would)
        ExprKind::Call { .. } | ExprKind::New { .. } | ExprKind::Await(_) | ExprKind::Yield(..) => true,
        ExprKind::Arrow(_) => false,
        ExprKind::Unary(_, x) | ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Typeof(x) | ExprKind::Spread(x) | ExprKind::As(x, _) | ExprKind::Try(x) => has_call(ast, *x),
        ExprKind::Binary(_, a, b) | ExprKind::Index { obj: a, index: b, .. } | ExprKind::Assign(_, a, b) => has_call(ast, *a) || has_call(ast, *b),
        ExprKind::Update { target, .. } => has_call(ast, *target),
        ExprKind::Cond(a, b, c) => has_call(ast, *a) || has_call(ast, *b) || has_call(ast, *c),
        ExprKind::Member { obj, .. } => has_call(ast, *obj),
        ExprKind::Template(_, xs) | ExprKind::Array(xs) | ExprKind::Seq(xs) => any(xs),
        ExprKind::Object(fs) => fs.iter().any(|f| has_call(ast, f.value)),
        _ => false,
    }
}

/// Does `e` assign to anything (`=`, `op=`, `++`, `--`) outside nested functions?
pub(crate) fn has_assign(ast: &ast::Ast, e: ExprId) -> bool {
    let any = |xs: &[ExprId]| xs.iter().any(|&x| has_assign(ast, x));
    match &ast.expr(e).kind {
        ExprKind::Assign(..) | ExprKind::Update { .. } => true,
        ExprKind::Arrow(_) => false,
        ExprKind::Unary(_, x) | ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Typeof(x) | ExprKind::Spread(x) | ExprKind::As(x, _) | ExprKind::Try(x) | ExprKind::Await(x) | ExprKind::Yield(Some(x), _) => has_assign(ast, *x),
        ExprKind::Binary(_, a, b) | ExprKind::Index { obj: a, index: b, .. } => has_assign(ast, *a) || has_assign(ast, *b),
        ExprKind::Cond(a, b, c) => has_assign(ast, *a) || has_assign(ast, *b) || has_assign(ast, *c),
        ExprKind::Call { callee, args, .. } | ExprKind::New { callee, args, .. } => has_assign(ast, *callee) || args.iter().any(|a| has_assign(ast, a.expr)),
        ExprKind::Member { obj, .. } => has_assign(ast, *obj),
        ExprKind::Template(_, xs) | ExprKind::Array(xs) | ExprKind::Seq(xs) => any(xs),
        ExprKind::Object(fs) => fs.iter().any(|f| has_assign(ast, f.value)),
        _ => false,
    }
}

/// Does `e` contain an `await` outside nested functions?
pub(crate) fn has_await(ast: &ast::Ast, e: ExprId) -> bool {
    let any = |xs: &[ExprId]| xs.iter().any(|&x| has_await(ast, x));
    match &ast.expr(e).kind {
        ExprKind::Await(_) => true,
        ExprKind::Arrow(_) => false,
        ExprKind::Yield(x, _) => x.is_some_and(|x| has_await(ast, x)),
        ExprKind::Unary(_, x) | ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Typeof(x) | ExprKind::Spread(x) | ExprKind::As(x, _) | ExprKind::Try(x) => has_await(ast, *x),
        ExprKind::Update { target, .. } => has_await(ast, *target),
        ExprKind::Binary(_, a, b) | ExprKind::Assign(_, a, b) | ExprKind::Index { obj: a, index: b, .. } => has_await(ast, *a) || has_await(ast, *b),
        ExprKind::Cond(a, b, c) => has_await(ast, *a) || has_await(ast, *b) || has_await(ast, *c),
        ExprKind::Call { callee, args, .. } | ExprKind::New { callee, args, .. } => has_await(ast, *callee) || args.iter().any(|a| has_await(ast, a.expr)),
        ExprKind::Member { obj, .. } => has_await(ast, *obj),
        ExprKind::Template(_, xs) | ExprKind::Array(xs) | ExprKind::Seq(xs) => any(xs),
        ExprKind::Object(fs) => fs.iter().any(|f| has_await(ast, f.value)),
        ExprKind::Int(_) | ExprKind::Float(_) | ExprKind::BigInt(_) | ExprKind::Decimal(_) | ExprKind::Str(_) | ExprKind::Bool(_) | ExprKind::Undefined | ExprKind::Null | ExprKind::Ident(_) | ExprKind::This | ExprKind::Super | ExprKind::Error => false,
    }
}
