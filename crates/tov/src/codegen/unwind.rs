//! The unwind channel: an uncaught JavaScript exception in Tov code that JavaScript called (a
//! property read that throws, unmarked) goes back to the JavaScript caller through the Tov frames
//! in between, each releasing its values on the way, as an error path does.
//!
//! The trampoline (`js_tramp`) sets `tv_js_direct` just before it calls the closure, and so does
//! a direct call to a function that may unwind, just before the call (its arguments evaluated
//! first). The callee takes the flag first thing (`aw_`): it was called through the channel. A
//! failing read in such a frame releases the frame, leaves the exception in `tv_js_pending` and
//! returns; each caller sees it after the call and does the same, up to the trampoline, which
//! throws it into JavaScript. A frame entered any other way (through a function value, a method,
//! the runtime) isn't on the channel: there the exception jumps to the trampoline's catch point
//! (`tv_js_unwind`), and only the frames it jumps over keep their values.

use super::Gen;
use crate::ast::{Ast, ExprId, ExprKind, ItemKind, StmtId, StmtKind};
use crate::check::Callee;
use crate::hash::{FxMap, FxSet};
use crate::types::JS;

/// A function body's JavaScript reads, and the functions it calls directly.
#[derive(Default)]
struct Scan {
    reads: bool,
    calls: Vec<(u32, u32)>,
}

impl<'c, 'a> Gen<'c, 'a> {
    /// Can module function (m, item) leave through the channel: does it read JavaScript
    /// properties, or call a function that can, directly? (Async functions run as tasks: never.)
    pub(crate) fn unwinds(&mut self, m: u32, item: u32) -> bool {
        if self.unwinding.is_none() {
            let set = self.compute_unwinding();
            self.unwinding = Some(set);
        }
        self.unwinding.as_ref().is_some_and(|s| s.contains(&(m, item)))
    }

    fn compute_unwinding(&mut self) -> FxSet<(u32, u32)> {
        let mut out = FxSet::default();
        if !self.npm {
            return out;
        }
        let mut callers: FxMap<(u32, u32), Vec<(u32, u32)>> = FxMap::default();
        let mut work = Vec::new();
        for m in 0..self.c.modules.len() as u32 {
            let ast = self.ast(m);
            for (i, it) in ast.items.iter().enumerate() {
                let ItemKind::Function(f) = &it.kind else { continue };
                if f.is_async {
                    continue;
                }
                let mut scan = Scan::default();
                self.scan_stmt(m, ast, f.body, &mut scan);
                let key = (m, i as u32);
                for c in scan.calls {
                    callers.entry(c).or_default().push(key);
                }
                if scan.reads && out.insert(key) {
                    work.push(key);
                }
            }
        }
        while let Some(k) = work.pop() {
            for &caller in callers.get(&k).map(|v| v.as_slice()).unwrap_or(&[]) {
                if out.insert(caller) {
                    work.push(caller);
                }
            }
        }
        out
    }

    fn is_js_expr(&mut self, m: u32, e: ExprId) -> bool {
        let t = self.facts(m).expr_ty[e as usize];
        self.c.types.without_undefined(t) == JS
    }

    fn scan_stmt(&mut self, m: u32, ast: &'a Ast, s: StmtId, out: &mut Scan) {
        match &ast.stmt(s).kind {
            StmtKind::Let { init: Some(e), .. } | StmtKind::Expr(e) | StmtKind::Return(Some(e)) | StmtKind::Throw(e) => self.scan_expr(m, ast, *e, out),
            StmtKind::If(c, t, e) => {
                self.scan_expr(m, ast, *c, out);
                self.scan_stmt(m, ast, *t, out);
                if let Some(e) = e {
                    self.scan_stmt(m, ast, *e, out);
                }
            }
            StmtKind::While(c, b) | StmtKind::DoWhile(b, c) => {
                self.scan_expr(m, ast, *c, out);
                self.scan_stmt(m, ast, *b, out);
            }
            StmtKind::For { init, cond, step, body } => {
                if let Some(i) = init {
                    self.scan_stmt(m, ast, *i, out);
                }
                for e in [cond, step].into_iter().flatten() {
                    self.scan_expr(m, ast, *e, out);
                }
                self.scan_stmt(m, ast, *body, out);
            }
            StmtKind::ForOf { iter, body, .. } => {
                if self.is_js_expr(m, *iter) {
                    out.reads = true;
                }
                self.scan_expr(m, ast, *iter, out);
                self.scan_stmt(m, ast, *body, out);
            }
            StmtKind::Switch(d, cases) => {
                self.scan_expr(m, ast, *d, out);
                for c in cases {
                    if let Some(t) = c.test {
                        self.scan_expr(m, ast, t, out);
                    }
                    for &x in &c.body {
                        self.scan_stmt(m, ast, x, out);
                    }
                }
            }
            StmtKind::Block(ss) => ss.iter().for_each(|&x| self.scan_stmt(m, ast, x, out)),
            StmtKind::Try { body, catch, finally } => {
                self.scan_stmt(m, ast, *body, out);
                if let Some(c) = catch {
                    self.scan_stmt(m, ast, c.body, out);
                }
                if let Some(f) = finally {
                    self.scan_stmt(m, ast, *f, out);
                }
            }
            _ => {}
        }
    }

    /// (A closure's body is its own function: not scanned as part of this one.)
    fn scan_expr(&mut self, m: u32, ast: &'a Ast, e: ExprId, out: &mut Scan) {
        match &ast.expr(e).kind {
            ExprKind::Member { obj, .. } => {
                if self.is_js_expr(m, *obj) {
                    out.reads = true;
                }
                self.scan_expr(m, ast, *obj, out);
            }
            ExprKind::Index { obj, index, .. } => {
                if self.is_js_expr(m, *obj) {
                    out.reads = true;
                }
                self.scan_expr(m, ast, *obj, out);
                self.scan_expr(m, ast, *index, out);
            }
            ExprKind::Call { callee, args, .. } => {
                if let Some(Callee::Fn(fm, fi)) = self.facts(m).calls.get(&e).map(|c| c.callee.clone()) {
                    out.calls.push((fm, fi));
                }
                self.scan_expr(m, ast, *callee, out);
                args.iter().for_each(|a| self.scan_expr(m, ast, a.expr, out));
            }
            ExprKind::New { callee, args, .. } => {
                self.scan_expr(m, ast, *callee, out);
                args.iter().for_each(|a| self.scan_expr(m, ast, a.expr, out));
            }
            ExprKind::Unary(_, x) | ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Typeof(x) | ExprKind::As(x, _) | ExprKind::Try(x) | ExprKind::Await(x) => {
                self.scan_expr(m, ast, *x, out)
            }
            ExprKind::Binary(_, l, r) | ExprKind::Assign(_, l, r) => {
                self.scan_expr(m, ast, *l, out);
                self.scan_expr(m, ast, *r, out);
            }
            ExprKind::Update { target, .. } => self.scan_expr(m, ast, *target, out),
            ExprKind::Cond(a, b, c) => [*a, *b, *c].into_iter().for_each(|x| self.scan_expr(m, ast, x, out)),
            ExprKind::Array(xs) | ExprKind::Template(_, xs) => xs.iter().for_each(|&x| self.scan_expr(m, ast, x, out)),
            ExprKind::Object(fs) => fs.iter().for_each(|f| self.scan_expr(m, ast, f.value, out)),
            _ => {}
        }
    }

    /// After a direct call through the channel: if the callee left through it, so does this
    /// frame (its values released, a default value returned).
    pub(crate) fn unwind_check(&mut self) {
        if self.b().root {
            // (an async arrow passed to JavaScript: the exception rejects its promise)
            self.js_used();
            self.line("if (__builtin_expect(tv_js_pending != NULL, 0)) { tvg_err = tvg_js_make_error(tv_js_pending); tv_js_release(tv_js_pending); tv_js_pending = NULL; }");
            self.error_check();
            return;
        }
        self.open("if (__builtin_expect(tv_js_pending != NULL, 0)) {");
        self.release_all_temps();
        self.unwind_to(0);
        self.channel_return();
        self.close("}");
    }

    /// `return` (a default value) from the current frame, for the channel.
    pub(crate) fn channel_return(&mut self) {
        let ret = self.b().ret;
        if ret == crate::types::VOID {
            self.line("return;");
        } else {
            let d = self.default_value(ret);
            self.line(format!("return {d};"));
        }
    }
}
