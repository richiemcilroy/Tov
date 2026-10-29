//! Function bodies: statements, expressions, places, conversions and closures.

use super::{c_string, Gen, Val};
use crate::ast::{self, ArrowBody, AssignOp, BinOp, ExprId, ExprKind, StmtId, StmtKind, UnOp};
use crate::check::{Callee, IdentFact, MemberFact};
use crate::hash::{FxMap, FxSet};
use crate::source::Span;
use crate::types::*;
use std::fmt::Write;

/// Maps a case label literal to a union tag.
type LabelTag = Box<dyn Fn(&Gen, crate::intern::Sym) -> Vec<usize>>;

pub(crate) enum FnBodyKind {
    Block(StmtId),
    Expr(ExprId),
    /// An implicit constructor: only field initializers.
    Init,
}

#[derive(Clone)]
pub(crate) struct Local {
    /// A C lvalue for the variable's value.
    pub access: String,
    pub ty: TyId,
}

pub(crate) struct Scope {
    /// Release statements for variables declared in this scope, in declaration order.
    pub(crate) releases: Vec<String>,
}

#[derive(Clone)]
enum BreakTarget {
    /// A C loop or C switch: plain `break`.
    Native,
    /// An if-chain switch: `goto label`.
    Goto(String),
}

pub(crate) struct Body {
    pub m: u32,
    pub subst: FxMap<u32, TyId>,
    out: String,
    indent: usize,
    pub locals: FxMap<u32, Local>,
    pub(crate) scopes: Vec<Scope>,
    pub(crate) temps: Vec<Vec<(String, TyId)>>,
    ret: TyId,
    /// (continue label, scope depth at loop body entry)
    loops: Vec<(String, usize)>,
    /// Enclosing labelled loops: (label, the label after it, its entry in `loops`, the scope
    /// depth outside it).
    pub labeled: Vec<(crate::intern::Sym, String, usize, usize)>,
    /// A `for...of` variable made for each iteration: its release, at the end of each.
    pub iter_release: Option<String>,
    /// `for (const k in m)` walking the map `m` itself (see `keys_loop`): (the map's code, the
    /// key variable, the current entry's value pointer, the value type). `m[k]` there is that value.
    pub map_loops: Vec<(String, String, String, TyId)>,
    /// (target, scope depth to unwind to)
    breaks: Vec<(BreakTarget, usize)>,
    /// Locals (by declaration offset) that live in heap cells because a heap closure captures them and they change.
    pub boxed: FxSet<u32>,
    /// Arrow expressions passed directly to built-in methods: their environment can live on the stack.
    pub stack_arrows: FxSet<ExprId>,
    /// Array locals made unique before the enclosing loop and never copied inside it:
    /// element writes skip the copy-on-write check.
    pub unique: FxSet<u32>,
    /// Locals changed anywhere in the function (assigned, mutated in place, passed as `inout`).
    pub mutated: FxSet<u32>,
    /// Read-only (non-`inout`) parameters.
    pub params: FxSet<u32>,
    /// In a derived class's constructor: the class whose field initializers run after `super(...)`.
    pub ctor_class: Option<TyId>,
    /// Parameters the caller moved in (a constructor's `this.f = p`, see `ctor_moves`): their one
    /// read takes the reference.
    pub owned_once: FxSet<u32>,
    /// Enclosing `try` blocks of this function: (label of the handler, scope depth to unwind to).
    pub handlers: Vec<(String, usize)>,
    /// An async function's body (module, item): its locals live in a frame (see asyncfn.rs).
    pub async_fn: Option<(u32, u32)>,
    /// A generator's body: what it yields.
    pub gen_elem: Option<TyId>,
    /// Awaited calls embedded in the frame: (union member, frame type).
    pub children: Vec<(String, String)>,
    /// Enclosing `try` blocks that have a `finally`, innermost last: (the `finally` block, scope
    /// depth outside the `try`). `return`, `break` and `continue` run them on the way out.
    pub finallies: Vec<(StmtId, usize)>,
    /// Accumulator recursion elimination (see `Gen::tre_candidate`): `return a + f(args)`
    /// becomes "add a, rebind the parameters, jump to the top".
    pub tre: Option<Tre>,
    /// (index local, array local) pairs of enclosing `for (let i = 0; i < xs.length; i++)`
    /// loops whose bodies change neither: `xs[i]` there is in bounds.
    pub in_bounds: Vec<(u32, u32)>,
    /// Array locals pushed in a loop that nothing else in it can reallocate: the C variable
    /// holding their capacity (pushes compare against it instead of reloading it).
    pub push_caps: FxMap<u32, String>,
    /// Arrays a loop uses only by index and length: their elements' and length's C locals, kept
    /// for the loop (an append through an index refreshes them; nothing else can move them).
    pub arr_cache: FxMap<u32, (String, String)>,
    /// Array locals whose capacity was reserved before a loop for every push it can make
    /// (see `reserve_for_pushes`): those pushes store without a capacity check.
    pub exact_pushes: FxSet<u32>,
    /// The index local of the `for` loop whose step is being emitted, when that step can't
    /// overflow (see `step_cannot_overflow`): its `i++` is a plain add.
    pub nowrap_step: Option<u32>,
    /// Divisor locals of `%` with their reciprocal worked out before the enclosing loop nest
    /// (see `hoist_divisors`): the C variable holding it.
    pub divs: FxMap<u32, String>,
    /// (index local, array locals) of enclosing versioned loops' unchecked copies (see
    /// `version_plan`): `xs[i + k]` there is in bounds, and `i + k` can't overflow.
    pub versioned: Vec<(Vec<u32>, Vec<(u32, String, String)>)>,
    /// Rows of arrays of arrays a counted loop's unchecked copy keeps in locals (see `row_plan`):
    /// (array local, row index as (local, offset) or (none, literal), loop local, elements, length).
    pub rows: Vec<(u32, (Option<u32>, i64), u32, String, String)>,
    /// Locals whose `++`/`--` can't overflow in the code being emitted (a two-pointer loop's
    /// unchecked copy: see `two_pointer_plan`).
    pub nowrap_locals: Vec<u32>,
    /// The function's body statement (for facts about the whole function).
    pub root: Option<StmtId>,
    /// A closure's body statement (see `reassigned`), where `root` is the function's.
    pub arrow_root: Option<StmtId>,
    /// Locals that borrow what they point to (see `cursor_local`): never retained or released.
    pub cursors: FxSet<u32>,
    /// `p = a.f; a.f = null` (see `unlink_pair`): the read that takes a.f's reference, and the
    /// store that then doesn't release it.
    pub take: Option<ExprId>,
    pub no_release: Option<ExprId>,
    /// `const x = P; ...; P = v` being emitted (see `take_plan`): the store's statement, and
    /// where x is declared.
    pub take_slot: Option<(StmtId, usize)>,
    pub take_let: bool,
    /// `x = P; P = v` being emitted: the P read that x takes (see `take_assign_pair`).
    pub take_assign: Option<ExprId>,
    pub take_failed: bool,
    /// Locals holding a reference of their own, and the uses that hand it over (see `moves`;
    /// found when first needed).
    pub owned_locals: FxSet<u32>,
    pub moves: Option<FxSet<ExprId>>,
    /// `const` array locals whose length never changes in the function (see `fixed_lens`).
    /// Arrays of a fixed length: (length, where in the source it holds from).
    pub fixed: Option<FxMap<u32, (u64, u32)>>,
    /// Function-typed locals (parameters) known to hold one arrow's function: called directly
    /// (an instance specialized for the arrows passed at its call site, see `Gen::instance_with`).
    pub direct_fns: FxMap<u32, String>,
    /// In a counted loop's unchecked copy: `xs[i - 1]` read from a local holding what the last
    /// round stored to `xs[i]` (see `carry_plan`): (array, the storing statement, the local,
    /// the array's data pointer, the loop index's access).
    pub carry: Option<(u32, StmtId, String, String, String)>,
    /// `string[]` locals used only to push strings onto, join and count (see `builder_local`):
    /// a string builder each (key → (builder, count, separator)).
    pub builders: FxMap<u32, (String, String, String)>,
}

/// A `for` loop emitted twice (see `Gen::version_plan`): its index local, whether its test is
/// `i <= n`, `n`, and each array it indexes at `i + k` with the least and greatest `k`.
/// A counted loop's rows (see `row_plan`): the loop local, `<=`, its bound, and each row
/// (array local, row index, written, smallest and largest offset of the loop local it's indexed at).
pub(crate) struct RowPlan {
    jk: u32,
    le: bool,
    hi: ExprId,
    rows: Vec<(u32, (Option<u32>, i64), bool, i64, i64)>,
}

pub(crate) struct VersionPlan {
    ik: u32,
    le: bool,
    hi: ExprId,
    arrays: Vec<(u32, i64, i64)>,
    /// Arrays the loop only reads, at other indexes: their elements and length from locals too
    /// (each read still checked, against the local).
    extra: Vec<u32>,
}

/// A function whose `return a + f(args)` self-calls run as a loop: the function (module,
/// item), its parameter types, and the source location of the `+` (for the overflow trap).
#[derive(Clone)]
pub(crate) struct Tre {
    pub m: u32,
    pub item: u32,
    pub ptys: Vec<TyId>,
    pub loc: String,
    /// The first call runs the function's own code, and its tail hands off to this looping
    /// copy (`<name>_tre(a, args...)`): calls that end at once (the leaves) keep a small frame.
    /// `None`: this is the looping copy.
    pub entry: Option<String>,
}

impl Body {
    pub(crate) fn bump_indent(&mut self, d: i32) {
        self.indent = (self.indent as i32 + d).max(0) as usize;
    }
}

impl<'c, 'a> Gen<'c, 'a> {
    // ------------------------------------------------------------ emission helpers

    pub(crate) fn b(&mut self) -> &mut Body {
        self.bodies.last_mut().expect("inside a body")
    }

    pub(crate) fn line(&mut self, s: impl AsRef<str>) {
        let b = self.b();
        for _ in 0..b.indent {
            b.out.push_str("    ");
        }
        b.out.push_str(s.as_ref());
        b.out.push('\n');
    }

    pub(crate) fn open(&mut self, s: &str) {
        self.line(s);
        self.b().indent += 1;
    }

    pub(crate) fn close(&mut self, s: &str) {
        self.b().indent -= 1;
        self.line(s);
    }

    pub(crate) fn cur_m(&self) -> u32 {
        self.bodies.last().unwrap().m
    }

    /// The (instantiated) type the checker recorded for an expression.
    pub(crate) fn ty(&mut self, e: ExprId) -> TyId {
        let m = self.cur_m();
        let t = self.facts(m).expr_ty[e as usize];
        let subst = self.bodies.last().unwrap().subst.clone();
        self.c.types.subst(t, &subst)
    }

    pub(crate) fn inst(&mut self, t: TyId) -> TyId {
        let subst = self.bodies.last().unwrap().subst.clone();
        self.c.types.subst(t, &subst)
    }

    pub(crate) fn expr_span(&self, e: ExprId) -> Span {
        self.ast(self.cur_m()).expr(e).span
    }

    /// Declares a temporary holding `code`. Owned temporaries of heap types are released at
    /// the end of the enclosing statement unless consumed.
    pub(crate) fn tmp(&mut self, ty: TyId, code: &str, owned: bool) -> Val {
        let name = self.fresh("t");
        let ct = self.ctype(ty);
        self.line(format!("{ct} {name} = {code};"));
        let owned = owned && self.is_rc(ty);
        if owned {
            self.b().temps.last_mut().unwrap().push((name.clone(), ty));
        }
        Val { code: name, ty, owned }
    }

    /// Returns code for an owned copy of `v` (retaining borrowed heap values).
    pub(crate) fn consume(&mut self, v: Val) -> String {
        if !self.is_rc(v.ty) {
            return v.code;
        }
        if v.owned {
            let b = self.b();
            for scope in b.temps.iter_mut().rev() {
                if let Some(i) = scope.iter().position(|(n, _)| *n == v.code) {
                    scope.remove(i);
                    break;
                }
            }
            return v.code;
        }
        let name = self.fresh("t");
        let ct = self.ctype(v.ty);
        self.line(format!("{ct} {name} = {};", v.code));
        let r = self.retain_code(v.ty, &name);
        self.line(format!("{r};"));
        name
    }

    /// Makes an owned value (for results that must outlive the statement's temporaries).
    pub(crate) fn own(&mut self, v: Val) -> Val {
        if v.owned || !self.is_rc(v.ty) {
            return v;
        }
        let ty = v.ty;
        let code = self.consume(v);
        self.b().temps.last_mut().unwrap().push((code.clone(), ty));
        Val { code, ty, owned: true }
    }

    pub(crate) fn push_temps(&mut self) {
        self.b().temps.push(Vec::new());
    }

    pub(crate) fn push_temps_pub(&mut self) {
        self.push_temps();
    }

    pub(crate) fn pop_temps_pub(&mut self) {
        self.pop_temps();
    }

    /// Starts emitting a helper function body outside any source function (thunks).
    pub(crate) fn begin_scratch(&mut self, m: u32) {
        self.bodies.push(Body {
            m,
            subst: FxMap::default(),
            out: String::new(),
            indent: 1,
            locals: FxMap::default(),
            scopes: vec![Scope { releases: Vec::new() }],
            temps: vec![Vec::new()],
            ret: VOID,
            loops: Vec::new(),
            labeled: Vec::new(),
            iter_release: None,
            map_loops: Vec::new(),
            breaks: Vec::new(),
            boxed: FxSet::default(),
            stack_arrows: FxSet::default(),
            unique: FxSet::default(),
            mutated: FxSet::default(),
            params: FxSet::default(),
            ctor_class: None,
            owned_once: FxSet::default(),
            handlers: Vec::new(),
            async_fn: None,
            gen_elem: None,
            children: Vec::new(),
            finallies: Vec::new(),
            tre: None,
            in_bounds: Vec::new(),
            push_caps: FxMap::default(),
            arr_cache: FxMap::default(),
            exact_pushes: FxSet::default(),
            nowrap_step: None,
            divs: FxMap::default(),
            versioned: Vec::new(),
            rows: Vec::new(),
            nowrap_locals: Vec::new(),
            root: None,
            arrow_root: None,
            cursors: FxSet::default(),
            take: None,
            take_slot: None,
            take_let: false,
            take_assign: None,
            take_failed: false,
            no_release: None,
            owned_locals: FxSet::default(),
            moves: None,
            fixed: None,
            direct_fns: FxMap::default(),
            carry: None,
            builders: FxMap::default(),
        });
    }

    /// Returns `v` (as `ret`, owned) from a scratch body, releasing its temporaries first.
    pub(crate) fn scratch_return(&mut self, v: Val, ret: TyId) {
        if ret == VOID {
            self.release_all_temps();
            self.line("return;");
            return;
        }
        let v = self.coerce(v, ret);
        let code = self.consume(v);
        let ct = self.ctype(ret);
        self.line(format!("{ct} r_ = {code};"));
        self.release_all_temps();
        self.line("return r_;");
    }

    pub(crate) fn end_scratch(&mut self) -> String {
        self.bodies.pop().unwrap().out
    }

    /// After a call that can throw: on an error, clean up and jump to the handler (or return).
    pub(crate) fn error_check(&mut self) {
        self.open("if (__builtin_expect(tvg_err != NULL, 0)) {");
        self.error_path();
        self.close("}");
    }

    /// Leaves the current point with `tvg_err` set: releases temporaries and the scopes being
    /// left, then jumps to the enclosing `try` handler, or returns (the caller checks `tvg_err`).
    fn error_path(&mut self) {
        self.release_all_temps();
        match self.b().handlers.last().cloned() {
            Some((label, depth)) => {
                self.unwind_to(depth);
                self.line(format!("goto {label};"));
            }
            None => {
                self.unwind_to(0);
                let ret = self.b().ret;
                if ret == VOID {
                    self.line("return;");
                } else {
                    let d = self.default_value(ret);
                    self.line(format!("return {d};"));
                }
            }
        }
    }

    /// The base class type of a class type, if any.
    pub(crate) fn class_base_of(&mut self, cls: TyId) -> Option<TyId> {
        let (c, args) = self.c.class_of(cls)?;
        let info = &self.c.classes[c as usize];
        let base = info.base?;
        let map: FxMap<u32, TyId> = info.params.iter().copied().zip(args).collect();
        Some(self.c.types.subst(base, &map))
    }

    /// A local, parameter or field path of one (not a temporary).
    pub(crate) fn is_place_expr(&self, e: ExprId) -> bool {
        let m = self.cur_m();
        match &self.ast(m).expr(e).kind {
            ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Member { obj: x, .. } | ExprKind::Index { obj: x, .. } => self.is_place_expr(*x),
            ExprKind::Ident(_) | ExprKind::This => true,
            _ => false,
        }
    }

    pub(crate) fn pop_temps(&mut self) {
        let temps = self.b().temps.pop().unwrap();
        for (name, ty) in temps.into_iter().rev() {
            let r = self.release_code(ty, &name);
            self.line(format!("{r};"));
        }
    }

    pub(crate) fn push_scope(&mut self) {
        self.b().scopes.push(Scope { releases: Vec::new() });
    }

    pub(crate) fn pop_scope(&mut self) {
        let s = self.b().scopes.pop().unwrap();
        for r in s.releases.into_iter().rev() {
            self.line(r);
        }
    }

    /// Emits releases for every scope deeper than `depth` (without closing them).
    fn unwind_to(&mut self, depth: usize) {
        let mut lines = Vec::new();
        for scope in self.b().scopes[depth..].iter().rev() {
            for r in scope.releases.iter().rev() {
                lines.push(r.clone());
            }
        }
        for l in lines {
            self.line(l);
        }
    }

    /// Releases the scopes `to..from` (innermost first), for leaving them in steps.
    fn unwind_range(&mut self, from: usize, to: usize) {
        let mut lines = Vec::new();
        for scope in self.b().scopes[to..from].iter().rev() {
            for r in scope.releases.iter().rev() {
                lines.push(r.clone());
            }
        }
        for l in lines {
            self.line(l);
        }
    }

    /// Leaves to scope depth `depth` (0 for `return`), running the `finally` blocks being left,
    /// innermost first: each runs after the scopes inside its `try` are released, with only the
    /// `finally`s and error handlers outside it active.
    fn unwind_for_exit(&mut self, depth: usize) {
        let fins: Vec<(StmtId, usize)> = self.b().finallies.iter().rev().filter(|(_, d)| *d >= depth).copied().collect();
        let mut prev = self.b().scopes.len();
        for (f, d) in fins {
            self.unwind_range(prev, d);
            prev = d;
            let saved_fins = self.b().finallies.clone();
            let saved_handlers = self.b().handlers.clone();
            self.b().finallies.retain(|(_, dd)| *dd < d);
            self.b().handlers.retain(|(_, dd)| *dd < d);
            self.scoped_block(f);
            self.b().finallies = saved_fins;
            self.b().handlers = saved_handlers;
        }
        self.unwind_range(prev, depth);
    }

    pub(crate) fn release_all_temps(&mut self) {
        let mut lines = Vec::new();
        let all: Vec<(String, TyId)> = self.b().temps.iter().flat_map(|s| s.iter().cloned()).collect();
        for (name, ty) in all.into_iter().rev() {
            lines.push(format!("{};", self.release_code(ty, &name)));
        }
        for l in lines {
            self.line(l);
        }
    }

    // ------------------------------------------------------------ bodies

    /// Emits a function body; returns the C statements (without braces).
    /// `this`: (declaration key, class type) for methods and constructors; `ctor_class`: the class
    /// whose field initializers this constructor runs.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn function_body(
        &mut self,
        m: u32,
        subst: FxMap<u32, TyId>,
        params: &[(u32, TyId, bool)],
        ret: TyId,
        kind: FnBodyKind,
        entry_unique: Option<Vec<u32>>,
        this: Option<(u32, TyId)>,
        ctor_class: Option<TyId>,
    ) -> String {
        let (boxed, stack_arrows, mutated) = self.analyze_closures(m, &kind);
        let mut body = Body {
            m,
            subst,
            out: String::new(),
            indent: 1,
            locals: FxMap::default(),
            scopes: vec![Scope { releases: Vec::new() }],
            temps: vec![Vec::new()],
            ret,
            loops: Vec::new(),
            labeled: Vec::new(),
            iter_release: None,
            map_loops: Vec::new(),
            breaks: Vec::new(),
            boxed,
            stack_arrows,
            unique: FxSet::default(),
            mutated,
            params: params.iter().filter(|p| !p.2).map(|p| p.0).collect(),
            ctor_class,
            owned_once: FxSet::default(),
            handlers: Vec::new(),
            async_fn: self.pending_async.take(),
            gen_elem: self.pending_gen.take(),
            children: Vec::new(),
            finallies: Vec::new(),
            tre: self.pending_tre.take(),
            in_bounds: Vec::new(),
            push_caps: FxMap::default(),
            arr_cache: FxMap::default(),
            exact_pushes: FxSet::default(),
            nowrap_step: None,
            divs: FxMap::default(),
            versioned: Vec::new(),
            rows: Vec::new(),
            nowrap_locals: Vec::new(),
            root: None,
            arrow_root: None,
            cursors: FxSet::default(),
            take: None,
            take_slot: None,
            take_let: false,
            take_assign: None,
            take_failed: false,
            no_release: None,
            owned_locals: FxSet::default(),
            moves: None,
            fixed: None,
            direct_fns: self.pending_direct.take().unwrap_or_default(),
            carry: None,
            builders: FxMap::default(),
        };
        for (i, &(key, ty, inout)) in params.iter().enumerate() {
            let access = if inout { format!("(*p{i})") } else { format!("p{i}") };
            body.locals.insert(key, Local { access, ty });
        }
        if let Some((key, ty)) = this {
            body.locals.insert(key, Local { access: "self_".into(), ty });
            body.params.insert(key);
        }
        body.unique.extend(entry_unique.unwrap_or_default());
        if let FnBodyKind::Block(s) = &kind {
            body.root = Some(*s);
        }
        self.bodies.push(body);
        // Constructors of classes without a base class run their field initializers first.
        if let Some(cls) = ctor_class
            && (matches!(kind, FnBodyKind::Init) || self.class_base_of(cls).is_none())
        {
            self.b().ctor_class = None;
            self.emit_field_inits(cls);
        }
        // (a constructor's parameters its caller moved into `this.f = p`)
        if let Some(cls) = ctor_class
            && let Some((decl, _)) = self.c.class_of(cls)
            && let Some(mi) = self.c.classes[decl as usize].ctor.member
        {
            let moves = self.ctor_moves(cls);
            let fd = self.c.member_fn(decl, mi);
            for (i, p) in fd.params.iter().enumerate() {
                if p.prop.is_none() && moves.get(i).copied().unwrap_or(false) && !self.b().boxed.contains(&p.span.start) {
                    self.b().owned_once.insert(p.span.start);
                }
            }
        }
        // (parameters it takes ownership of, see `owned_params`: released unless moved on)
        if let FnBodyKind::Block(s) = &kind {
            let flags = self.owned_params(m, *s);
            for (i, &(key, ty, inout)) in params.iter().enumerate() {
                if flags.get(i).copied().unwrap_or(false) && !inout && self.is_rc(ty) && !self.b().boxed.contains(&key) {
                    let rel = format!("{};", self.release_code(ty, &format!("p{i}")));
                    self.b().scopes[0].releases.push(rel);
                    self.b().params.remove(&key);
                    self.b().owned_locals.insert(key);
                }
            }
        }
        // A parameter the body reassigns is owned from the start (borrowed, it can't be released
        // when replaced).
        let reassigned = self.reassigned_locals(&kind);
        for (i, &(key, ty, inout)) in params.iter().enumerate() {
            if !inout && !self.b().boxed.contains(&key) && reassigned.contains(&key) && self.is_rc(ty) {
                let r = self.retain_code(ty, &format!("p{i}"));
                self.line(format!("{r};"));
                let rel = format!("{};", self.release_code(ty, &format!("p{i}")));
                self.b().scopes[0].releases.push(rel);
                self.b().params.remove(&key);
            }
        }
        // Parameters captured by heap closures and reassigned move into cells.
        for (i, &(key, ty, inout)) in params.iter().enumerate() {
            if !inout && self.b().boxed.contains(&key) {
                let cell = self.cell_type(ty);
                let r = self.retain_code(ty, &format!("p{i}"));
                self.line(format!("{cell} *c{i} = tvg_alloc_small(sizeof({cell})); c{i}->rc = 1; c{i}->v = p{i}; {r};"));
                let rel = self.cell_release(ty, &format!("c{i}"));
                self.b().scopes[0].releases.push(rel);
                self.b().locals.insert(key, Local { access: format!("c{i}->v"), ty });
            }
        }
        match kind {
            FnBodyKind::Block(s) => {
                if self.b().tre.as_ref().is_some_and(|t| t.entry.is_none()) {
                    // The looping copy starts with the first term (parameter `tre_a0`). Every
                    // partial sum the recursion would compute is checked at the end (see
                    // TVG_TRE_ADD): one register while the terms are non-negative, else exact.
                    self.line("tv_int tre_acc = 0; tvg_tre_wide tre_ws;");
                    self.line("TVG_TRE_ADD(tre_a0);");
                    self.line("tre_top:;");
                }
                self.stmt_list_of(s);
                self.unwind_to(0);
                if ret != VOID && !self.always_returns_stmt(m, s) {
                    // Only reachable when the return type admits `undefined`.
                    let v = self.coerce(Val::plain("0", UNDEFINED), ret);
                    let code = self.consume(v);
                    self.line(format!("return {code};"));
                }
            }
            FnBodyKind::Expr(e) => {
                let v = self.expr(e);
                if ret == VOID {
                    self.pop_temps();
                    self.b().temps.push(Vec::new());
                    self.unwind_to(0);
                } else {
                    let v = self.coerce(v, ret);
                    let code = self.consume(v);
                    let ct = self.ctype(ret);
                    self.line(format!("{ct} r_ = {code};"));
                    self.release_all_temps();
                    self.unwind_to(0);
                    self.line("return r_;");
                }
            }
            FnBodyKind::Init => self.unwind_to(0),
        }
        let b = self.bodies.pop().unwrap();
        if b.async_fn.is_some() {
            self.last_children = b.children;
        }
        b.out
    }

    /// `a + f(args)` with `f` the function being emitted as a loop: (a, args).
    fn tre_tail(&self, e: ExprId, tre: &Tre) -> Option<(ExprId, Vec<ExprId>)> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let ExprKind::Binary(crate::ast::BinOp::Add, a, call) = &ast.expr(e).kind else { return None };
        let ExprKind::Call { args, optional: false, .. } = &ast.expr(*call).kind else { return None };
        let fact = self.facts(m).calls.get(call)?;
        if !matches!(fact.callee, Callee::Fn(fm, fi) if fm == tre.m && fi == tre.item) || args.len() != tre.ptys.len() || args.iter().any(|a| a.by_ref.is_some()) {
            return None;
        }
        if self.facts(m).expr_ty[e as usize] != INT || self.facts(m).expr_ty[*a as usize] != INT {
            return None;
        }
        Some((*a, args.iter().map(|a| a.expr).collect()))
    }

    fn always_returns_stmt(&self, m: u32, s: StmtId) -> bool {
        // Mirrors the checker's analysis (switch exhaustiveness is recorded there).
        let ast = self.ast(m);
        match &ast.stmt(s).kind {
            StmtKind::Return(_) => true,
            StmtKind::Block(ss) => ss.iter().any(|&x| self.always_returns_stmt(m, x)),
            StmtKind::If(_, t, Some(e)) => self.always_returns_stmt(m, *t) && self.always_returns_stmt(m, *e),
            StmtKind::Switch(_, cases) => {
                cases.iter().any(|c| c.test.is_none()) && cases.iter().all(|c| c.body.is_empty() || c.body.iter().any(|&x| self.always_returns_stmt(m, x)))
                    || (self.c.exhaustive_switch(m, s) && cases.iter().all(|c| c.body.is_empty() || c.body.iter().any(|&x| self.always_returns_stmt(m, x))))
            }
            StmtKind::While(c, _) | StmtKind::For { cond: Some(c), .. } => matches!(ast.expr(*c).kind, ExprKind::Bool(true)),
            StmtKind::For { cond: None, .. } => true,
            StmtKind::DoWhile(body, _) => self.always_returns_stmt(m, *body),
            StmtKind::Throw(_) => true,
            StmtKind::Try { body, catch, finally } => {
                finally.map(|f| self.always_returns_stmt(m, f)).unwrap_or(false)
                    || (self.always_returns_stmt(m, *body) && catch.as_ref().map(|c| self.always_returns_stmt(m, c.body)).unwrap_or(true))
            }
            _ => false,
        }
    }

    /// A block's statements without opening a C block (the function body scope).
    fn stmt_list_of(&mut self, s: StmtId) {
        let m = self.cur_m();
        if let StmtKind::Block(ss) = &self.ast(m).stmt(s).kind {
            self.stmts(ss);
        } else {
            self.stmt(s);
        }
    }

    /// A block's statements. A local declared here is released right after the last statement
    /// that uses it (directly, or through a local that may borrow from it) rather than at the end
    /// of the block, so dead values don't add to peak memory.
    pub(crate) fn stmts(&mut self, ss: &[StmtId]) {
        let plan = if self.b().async_fn.is_none() && ss.len() > 1 { self.last_uses(ss) } else { Vec::new() };
        let mut skip = false;
        let mut in_group = 0;
        for (i, &x) in ss.iter().enumerate() {
            let m = self.cur_m();
            if in_group > 0 {
                in_group -= 1;
            } else if let Some(g) = self.facts(m).closure_groups.get(&x).cloned() {
                self.closure_group(&g);
                in_group = g.len() - 1;
            } else if skip {
                skip = false;
            } else if let Some(n) = self.map_count(ss, i) {
                skip = n == 2;
            } else if let Some(n) = self.permutation(ss, i) {
                in_group = n - 1;
            } else if let Some((p, t2)) = self.take_assign_pair(ss, i) {
                // `x = P; P = v`: x takes P's reference (see `take_assign_pair`)
                self.b().take_assign = Some(p);
                self.b().take_failed = false;
                self.stmt(x);
                self.b().take_assign = None;
                if !std::mem::take(&mut self.b().take_failed) {
                    self.b().no_release = Some(t2);
                }
                self.stmt(ss[i + 1]);
                self.b().no_release = None;
                skip = true;
            } else if self.b().take_slot.is_none()
                && let Some(j) = self.take_plan(ss, i)
            {
                // (one at a time: a second would replace the first's pending store, which would
                // then release what the first's x holds)
                // `const x = P; ...; P = v`: x takes P's reference (see `take_plan`)
                self.b().take_slot = Some((ss[j], i));
                self.b().take_let = true;
                self.stmt(x);
                self.b().take_let = false;
            } else if self.b().take_slot.is_some_and(|(sj, _)| sj == x) {
                let (_, di) = self.b().take_slot.take().unwrap();
                let StmtKind::Expr(e) = &self.ast(m).stmt(x).kind else { unreachable!() };
                let ExprKind::Assign(_, t, _) = &self.ast(m).expr(*e).kind else { unreachable!() };
                // (and `P = x.f; x.f = null` after it: see `unlink_pair`)
                let unlink = self.unlink_pair(ss, i);
                self.b().no_release = Some(*t);
                self.b().take = unlink.map(|u| u.0);
                self.stmt(x);
                self.b().take = None;
                self.b().no_release = None;
                if let Some((_, t2)) = unlink {
                    self.b().no_release = Some(t2);
                    self.stmt(ss[i + 1]);
                    self.b().no_release = None;
                    skip = true;
                }
                // (from here x holds the reference P held)
                if let StmtKind::Let { name_span, .. } = &self.ast(m).stmt(ss[di]).kind
                    && let Some(local) = self.b().locals.get(&name_span.start).cloned()
                {
                    let rel = format!("{};", self.release_code(local.ty, &local.access));
                    self.b().scopes.last_mut().unwrap().releases.push(rel);
                }
            } else if let Some((v, t2)) = self.unlink_pair(ss, i) {
                // `p = a.f; a.f = null`: the reference moves from a.f to p (see `unlink_pair`)
                self.b().take = Some(v);
                self.stmt(x);
                self.b().take = None;
                self.b().no_release = Some(t2);
                self.stmt(ss[i + 1]);
                self.b().no_release = None;
                skip = true;
            } else {
                self.stmt(x);
            }
            if let Some(keys) = plan.get(i) {
                for &k in keys {
                    self.release_local_now(k);
                }
            }
        }
    }

    /// `const x = P; ...; P = v` (statement `i`, and `j` returned): P a field or element path from
    /// `this` or locals nothing in between changes, of a class type; between them no call (but a
    /// quiet one) and no other store of that field or array, x's last use in the statement just
    /// before `j` or later. Then x takes P's reference instead of retaining one: until `j` the
    /// slot still holds it (an exit leaves x unreleased), and `j` doesn't release the old value,
    /// which x now holds.
    fn take_plan(&mut self, ss: &[StmtId], i: usize) -> Option<usize> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let StmtKind::Let { mutable: false, name_span, init: Some(p), .. } = &ast.stmt(ss[i]).kind else { return None };
        let (key, p) = (name_span.start, *p);
        let ty = self.ty(p);
        if !(matches!(self.tget(ty), Ty::Class(..)) || self.niche(ty).is_some() && !self.niche_str(ty) && !self.niche_arr(ty)) {
            return None;
        }
        if self.b().boxed.contains(&key) || self.b().async_fn.is_some() || self.b().gen_elem.is_some() {
            return None;
        }
        // (the place: fields and elements of `this`, a parameter or a local)
        let mut roots: Vec<u32> = Vec::new();
        let mut fields: Vec<crate::intern::Sym> = Vec::new();
        if !self.simple_place(m, p, &mut roots, &mut fields, true) {
            return None;
        }
        let j = (i + 1..ss.len()).find(|&j| match &ast.stmt(ss[j]).kind {
            StmtKind::Expr(e) => matches!(&ast.expr(*e).kind, ExprKind::Assign(crate::ast::AssignOp::Assign, t, _) if self.same_place(m, *t, p)),
            _ => false,
        })?;
        // (nothing between, nor the store's value, calls out, stores into those fields or
        // arrays, changes the roots or x)
        let StmtKind::Expr(je) = &ast.stmt(ss[j]).kind else { return None };
        let ExprKind::Assign(_, _, jv) = &ast.expr(*je).kind else { return None };
        // (what a `throw` evaluates runs on the way out only, before x is handed on)
        fn outside_throws(ast: &ast::Ast, st: StmtId, out: &mut Vec<ExprId>) {
            if matches!(ast.stmt(st).kind, StmtKind::Throw(_)) {
                return;
            }
            let (es, ss) = super::quiet::stmt_parts(ast, st);
            for e in es {
                collect_exprs_expr(ast, e, out);
            }
            for s in ss {
                outside_throws(ast, s, out);
            }
        }
        let mut exprs = Vec::new();
        for &st in &ss[i + 1..j] {
            outside_throws(ast, st, &mut exprs);
        }
        collect_exprs_expr(ast, *jv, &mut exprs);
        let mut last_use = None;
        for &x in &exprs {
            match &ast.expr(x).kind {
                ExprKind::Call { .. } | ExprKind::New { .. } => return None,
                ExprKind::Await(_) | ExprKind::Yield(..) | ExprKind::Arrow(_) => return None,
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => {
                    let mut tt = *t;
                    while let ExprKind::Paren(y) | ExprKind::NonNull(y) = ast.expr(tt).kind {
                        tt = y;
                    }
                    match &ast.expr(tt).kind {
                        ExprKind::Ident(_) => match self.ident_fact(m, tt) {
                            Some(IdentFact::Local(k)) if k == key || roots.contains(&k) => return None,
                            _ => {}
                        },
                        ExprKind::Member { name, .. } if fields.contains(name) => return None,
                        ExprKind::Index { .. } => return None,
                        _ => {}
                    }
                }
                _ => {}
            }
        }
        // (x's last use: the statement before `j` or later)
        for (k, &st) in ss.iter().enumerate().skip(i + 1) {
            let mut es = Vec::new();
            collect_exprs_stmt(ast, st, &mut es);
            if es.iter().any(|&x| matches!(self.ident_fact(m, x), Some(IdentFact::Local(kk)) if kk == key)) {
                last_use = Some(k);
            }
        }
        if last_use.is_some_and(|k| k + 1 < j) {
            return None;
        }
        Some(j)
    }

    /// `e` is a field or element path from `this` or a local (its roots and field names
    /// collected); `top`: the path itself (an element's index too: a local or a number).
    fn simple_place(&self, m: u32, e: ExprId, roots: &mut Vec<u32>, fields: &mut Vec<crate::intern::Sym>, top: bool) -> bool {
        let ast = self.ast(m);
        match &ast.expr(e).kind {
            ExprKind::Paren(x) | ExprKind::NonNull(x) => self.simple_place(m, *x, roots, fields, top),
            ExprKind::This => !top,
            ExprKind::Ident(_) => !top && match self.ident_fact(m, e) {
                Some(IdentFact::Local(k)) => {
                    roots.push(k);
                    true
                }
                _ => false,
            },
            ExprKind::Member { obj, name, optional: false, .. } => {
                fields.push(*name);
                self.simple_place(m, *obj, roots, fields, false)
            }
            ExprKind::Index { obj, index, optional: false } => {
                let idx_ok = match &ast.expr(*index).kind {
                    ExprKind::Int(_) => true,
                    ExprKind::Ident(_) => match self.ident_fact(m, *index) {
                        Some(IdentFact::Local(k)) => {
                            roots.push(k);
                            true
                        }
                        _ => false,
                    },
                    _ => false,
                };
                idx_ok && self.simple_place(m, *obj, roots, fields, false)
            }
            _ => false,
        }
    }

    /// `x = P; P = v` (statements `i`, `i + 1`): x a local holding its own reference, P a field
    /// or element path (see `simple_place`) of a class type, and `v` calls nothing. Nothing runs
    /// between the two, so x can take P's reference (no retain) and the store skip releasing it.
    fn take_assign_pair(&mut self, ss: &[StmtId], i: usize) -> Option<(ExprId, ExprId)> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let (&s1, &s2) = (ss.get(i)?, ss.get(i + 1)?);
        let (StmtKind::Expr(e1), StmtKind::Expr(e2)) = (&ast.stmt(s1).kind, &ast.stmt(s2).kind) else { return None };
        let ExprKind::Assign(crate::ast::AssignOp::Assign, x, p) = &ast.expr(*e1).kind else { return None };
        let ExprKind::Assign(crate::ast::AssignOp::Assign, t2, v) = &ast.expr(*e2).kind else { return None };
        let Some(IdentFact::Local(k)) = self.ident_fact(m, *x) else { return None };
        if !matches!(ast.expr(*x).kind, ExprKind::Ident(_)) || !self.b().owned_locals.contains(&k) || self.b().boxed.contains(&k) || self.b().cursors.contains(&k) {
            return None;
        }
        if !self.same_place(m, *p, *t2) {
            return None;
        }
        let (mut roots, mut fields) = (Vec::new(), Vec::new());
        if !self.simple_place(m, *p, &mut roots, &mut fields, true) || roots.contains(&k) {
            return None;
        }
        let ty = self.ty(*p);
        if !(matches!(self.tget(ty), Ty::Class(..)) || self.niche(ty).is_some() && !self.niche_str(ty) && !self.niche_arr(ty)) {
            return None;
        }
        let mut es = Vec::new();
        collect_exprs_expr(ast, *v, &mut es);
        if es.iter().any(|&e| matches!(ast.expr(e).kind, ExprKind::Call { .. } | ExprKind::New { .. } | ExprKind::Assign(..) | ExprKind::Update { .. } | ExprKind::Await(_) | ExprKind::Yield(..) | ExprKind::Arrow(_))) {
            return None;
        }
        Some((*p, *t2))
    }

    /// `a` and `b` are the same path (see `simple_place`).
    fn same_place(&self, m: u32, a: ExprId, b: ExprId) -> bool {
        let ast = self.ast(m);
        let strip = |mut e: ExprId| {
            while let ExprKind::Paren(x) | ExprKind::NonNull(x) = ast.expr(e).kind {
                e = x;
            }
            e
        };
        let (a, b) = (strip(a), strip(b));
        match (&ast.expr(a).kind, &ast.expr(b).kind) {
            (ExprKind::This, ExprKind::This) => true,
            (ExprKind::Ident(_), ExprKind::Ident(_)) => matches!((self.ident_fact(m, a), self.ident_fact(m, b)), (Some(IdentFact::Local(x)), Some(IdentFact::Local(y))) if x == y),
            (ExprKind::Int(x), ExprKind::Int(y)) => x == y,
            (ExprKind::Member { obj: o1, name: n1, optional: false, .. }, ExprKind::Member { obj: o2, name: n2, optional: false, .. }) => n1 == n2 && self.same_place(m, *o1, *o2),
            (ExprKind::Index { obj: o1, index: i1, optional: false }, ExprKind::Index { obj: o2, index: i2, optional: false }) => self.same_place(m, *i1, *i2) && self.same_place(m, *o1, *o2),
            _ => false,
        }
    }

    /// `p = a.f; a.f = null` (statements `i`, `i + 1`; `a` a local or `this`, `f` a field of a
    /// class pointer type, `p` some other place): the value read and the place it's stored for
    /// the second. Nothing runs between, so the first store can take the reference a.f holds and
    /// the second skip releasing it.
    fn unlink_pair(&mut self, ss: &[StmtId], i: usize) -> Option<(ExprId, ExprId)> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let (&s1, &s2) = (ss.get(i)?, ss.get(i + 1)?);
        let (StmtKind::Expr(e1), StmtKind::Expr(e2)) = (&ast.stmt(s1).kind, &ast.stmt(s2).kind) else { return None };
        let ExprKind::Assign(crate::ast::AssignOp::Assign, t1, v1) = &ast.expr(*e1).kind else { return None };
        let ExprKind::Assign(crate::ast::AssignOp::Assign, t2, v2) = &ast.expr(*e2).kind else { return None };
        if !matches!(ast.expr(*v2).kind, ExprKind::Null | ExprKind::Undefined) {
            return None;
        }
        // (the field read, `a.f` with `a` a local or `this`, and the same field written)
        let field = |e: ExprId| -> Option<(Option<IdentFact>, bool, crate::intern::Sym)> {
            let mut e = e;
            while let ExprKind::Paren(x) | ExprKind::NonNull(x) = ast.expr(e).kind {
                e = x;
            }
            let ExprKind::Member { obj, name, optional: false, .. } = &ast.expr(e).kind else { return None };
            let mut o = *obj;
            while let ExprKind::Paren(x) | ExprKind::NonNull(x) = ast.expr(o).kind {
                o = x;
            }
            match ast.expr(o).kind {
                ExprKind::This => Some((None, true, *name)),
                ExprKind::Ident(_) => match self.ident_fact(m, o) {
                    Some(f @ IdentFact::Local(_)) => Some((Some(f), false, *name)),
                    _ => None,
                },
                _ => None,
            }
        };
        let (a, b) = (field(*v1)?, field(*t2)?);
        if a != b || field(*t1) == Some(a) {
            return None;
        }
        let vt = self.ty(*v1);
        let ptr = |g: &Self, t: TyId| matches!(g.tget(t), Ty::Class(..)) || g.niche(t).is_some() && !g.niche_str(t) && !g.niche_arr(t);
        if !ptr(self, vt) {
            return None;
        }
        // (the local holding `a` isn't a borrowed view or a cursor: it keeps `a` alive)
        if let Some(IdentFact::Local(k)) = a.0
            && !(self.b().owned_locals.contains(&k) || self.b().params.contains(&k))
        {
            return None;
        }
        Some((*v1, *t2))
    }

    /// `const t = a; a = b; b = t`, `[a, b] = [b, a]` (desugared to hidden constants), and other
    /// runs of constants and assignments that only move values between locals of one counted
    /// type, leaving each value in exactly one of them: plain copies, with no counting. The
    /// constants must be unused after the run. The statements it emitted, or None.
    fn permutation(&mut self, ss: &[StmtId], i: usize) -> Option<usize> {
        let m = self.cur_m();
        let ast = self.ast(m);
        // what each slot holds, as the slot it held at the start; temps are constants of the run
        let mut holds: Vec<(u32, u32)> = Vec::new();
        let mut temps: Vec<u32> = Vec::new();
        let mut steps: Vec<(u32, u32, bool)> = Vec::new(); // (to, from, declares)
        let mut ty = None;
        let mut best = None;
        let local = |e: ExprId| match (&ast.expr(e).kind, self.ident_fact(m, e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(k),
            _ => None,
        };
        for (j, &x) in ss.iter().enumerate().skip(i) {
            let (to, from, declares) = match &ast.stmt(x).kind {
                StmtKind::Let { mutable: false, name_span, ty: None, init: Some(v), .. } => match local(*v) {
                    Some(v) => (name_span.start, v, true),
                    None => break,
                },
                StmtKind::Expr(e) => match &ast.expr(*e).kind {
                    ExprKind::Assign(AssignOp::Assign, t, v) => match (local(*t), local(*v)) {
                        (Some(t), Some(v)) if !temps.contains(&t) => (t, v, false),
                        _ => break,
                    },
                    _ => break,
                },
                _ => break,
            };
            if !declares {
                let Some(l) = self.bodies.last().unwrap().locals.get(&to) else { break };
                if ty.is_some_and(|t| t != l.ty) {
                    break;
                }
                ty = Some(l.ty);
            }
            if !temps.contains(&from) {
                let Some(l) = self.bodies.last().unwrap().locals.get(&from) else { break };
                if ty.is_some_and(|t| t != l.ty) {
                    break;
                }
                ty = Some(l.ty);
            }
            let val = holds.iter().find(|h| h.0 == from).map(|h| h.1).unwrap_or(from);
            match holds.iter_mut().find(|h| h.0 == to) {
                Some(h) => h.1 = val,
                None => holds.push((to, val)),
            }
            if declares {
                temps.push(to);
            }
            steps.push((to, from, declares));
            // (a permutation: each local's start value is in exactly one local)
            let locals: Vec<&(u32, u32)> = holds.iter().filter(|h| !temps.contains(&h.0)).collect();
            let starts: Vec<u32> = holds.iter().map(|h| h.0).filter(|k| !temps.contains(k)).collect();
            let perm = !locals.is_empty()
                && locals.iter().all(|h| starts.contains(&h.1))
                && starts.iter().all(|s| locals.iter().filter(|h| h.1 == *s).count() == 1);
            if perm {
                best = Some(j + 1 - i);
            }
        }
        let n = best?;
        let ty = ty?;
        if n < 2 {
            return None;
        }
        let temps: Vec<u32> = steps[..n].iter().filter(|s| s.2).map(|s| s.0).collect();
        // (a loop's cached view of an array local stays the codegen's own way of assigning)
        let b = self.bodies.last().unwrap();
        if steps[..n].iter().flat_map(|s| [s.0, s.1]).any(|k| b.unique.contains(&k) || b.arr_cache.contains_key(&k) || b.push_caps.contains_key(&k)) {
            return None;
        }
        let mut later = Vec::new();
        for &x in &ss[i + n..] {
            collect_exprs_stmt(ast, x, &mut later);
        }
        if later.iter().any(|&e| local(e).is_some_and(|k| temps.contains(&k))) || !self.is_rc(ty) {
            return None;
        }
        let ct = self.ctype(ty);
        let mut names: FxMap<u32, String> = FxMap::default();
        for &(to, from, declares) in &steps[..n] {
            let src = match names.get(&from) {
                Some(n) => n.clone(),
                None => self.b().locals.get(&from)?.access.clone(),
            };
            if declares {
                let t = self.fresh("mv");
                self.line(format!("{ct} {t} = {src};"));
                names.insert(to, t);
            } else {
                let dst = self.b().locals.get(&to)?.access.clone();
                self.line(format!("{dst} = {src};"));
            }
        }
        Some(n)
    }

    /// `m.set(k, (m.get(k) ?? d) op e)` (a counter or running total in a map, `k` a local, `d`
    /// a literal, `e` a literal or local) as one find-or-insert, and with `const k = s.slice(a, b)`
    /// just before it (and `k` not used after), looked up by the slice's bytes: the key string is
    /// made only when it's added. The statements it emitted (1 or 2), or None.
    fn map_count(&mut self, ss: &[StmtId], i: usize) -> Option<usize> {
        let boxed_ok = |g: &mut Self, k: u32| !g.b().boxed.contains(&k);
        let m = self.cur_m();
        let found = {
            let ast = self.ast(m);
            let local = |e: ExprId| match (&ast.expr(e).kind, self.ident_fact(m, e)) {
                (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(k),
                _ => None,
            };
            let strip = |mut e: ExprId| {
                while let ExprKind::Paren(x) = &ast.expr(e).kind {
                    e = *x;
                }
                e
            };
            // (the same variable, or field of one, read twice: no getter runs)
            let facts = self.facts(m);
            fn same(ast: &ast::Ast, facts: &crate::check::facts::ModuleFacts, a: ExprId, b: ExprId) -> bool {
                match (&ast.expr(a).kind, &ast.expr(b).kind) {
                    (ExprKind::Paren(x), _) => same(ast, facts, *x, b),
                    (_, ExprKind::Paren(y)) => same(ast, facts, a, *y),
                    (ExprKind::Ident(x), ExprKind::Ident(y)) => x == y && facts.idents.get(&a).is_some_and(|f| matches!(f, IdentFact::Local(_) | IdentFact::Const(..))) && facts.idents.get(&a) == facts.idents.get(&b),
                    (ExprKind::This, ExprKind::This) => true,
                    (ExprKind::Member { obj: o1, name: n1, optional: false, .. }, ExprKind::Member { obj: o2, name: n2, optional: false, .. }) => {
                        n1 == n2 && !facts.calls.contains_key(&a) && !facts.calls.contains_key(&b) && !facts.members.contains_key(&a) && same(ast, facts, *o1, *o2)
                    }
                    _ => false,
                }
            }
            let same_path = |a: ExprId, b: ExprId| same(ast, facts, a, b);
            let method = |e: ExprId, name: &str| -> Option<(ExprId, Vec<ExprId>)> {
                let ExprKind::Call { callee, args, optional: false, .. } = &ast.expr(e).kind else { return None };
                let ExprKind::Member { obj, name: n, optional: false, .. } = &ast.expr(*callee).kind else { return None };
                if self.sym(*n) != name || args.iter().any(|a| a.by_ref.is_some()) {
                    return None;
                }
                Some((*obj, args.iter().map(|a| a.expr).collect()))
            };
            let parse = |s: StmtId| -> Option<(ExprId, ExprId, BinOp, ExprId, ExprId, ExprId)> {
                let StmtKind::Expr(call) = &ast.stmt(s).kind else { return None };
                let (mobj, args) = method(*call, "set")?;
                let [k, v] = args[..] else { return None };
                let v = strip(v);
                let ExprKind::Binary(op @ (BinOp::Add | BinOp::Sub | BinOp::Mul), l, e) = &ast.expr(v).kind else { return None };
                let ExprKind::Binary(BinOp::Nullish, g, d) = &ast.expr(strip(*l)).kind else { return None };
                let (gobj, gargs) = method(strip(*g), "get")?;
                let [k2] = gargs[..] else { return None };
                let same = same_path(gobj, mobj) && same_path(k2, k);
                same.then_some((mobj, k, *op, *e, *d, v))
            };
            // this statement, or `const k = s.slice(a, b)` and it next (`k` used nowhere after them)
            let (found, slice) = match parse(ss[i]) {
                Some(f) => (f, None),
                None => {
                    let StmtKind::Let { mutable: false, name_span, init: Some(init), .. } = &ast.stmt(ss[i]).kind else { return None };
                    let f = ss.get(i + 1).and_then(|&n| parse(n))?;
                    if local(f.1) != Some(name_span.start) {
                        return None;
                    }
                    let (sobj, sargs) = method(*init, "slice")?;
                    if sargs.is_empty() || sargs.len() > 2 {
                        return None;
                    }
                    let mut later = Vec::new();
                    for &x in &ss[i + 2..] {
                        collect_exprs_stmt(ast, x, &mut later);
                    }
                    if later.iter().any(|&x| local(x) == Some(name_span.start)) {
                        return None;
                    }
                    (f, Some((*init, sobj, sargs)))
                }
            };
            let (mobj, k, _, e, d, _) = found;
            let (mk, kk) = (local(mobj), local(k));
            if slice.is_some() && kk.is_none() {
                return None;
            }
            let simple = |x: ExprId| matches!(ast.expr(x).kind, ExprKind::Int(_) | ExprKind::Float(_));
            // (`e` runs after the lookup instead of before it: nothing in it may call or read the map)
            let mut es = Vec::new();
            collect_exprs_expr(ast, e, &mut es);
            let e_ok = es.iter().all(|&x| match &ast.expr(x).kind {
                ExprKind::Int(_) | ExprKind::Float(_) | ExprKind::Paren(_) | ExprKind::Binary(..) | ExprKind::Unary(..) | ExprKind::Cond(..) => true,
                ExprKind::Ident(_) => local(x).is_some_and(|x| Some(x) != mk),
                // (a record's field, or a length: no getter runs)
                ExprKind::Member { obj, name, optional: false, .. } => {
                    let ot = self.facts(m).expr_ty.get(*obj as usize).copied().unwrap_or(ERROR);
                    matches!(self.c.types.get(ot), Ty::Record(_)) || matches!(self.sym(*name), "length" | "byteLength") && !matches!(self.c.types.get(ot), Ty::Class(..))
                }
                _ => false,
            });
            if (mk.is_some() && mk == kk) || !simple(d) || !e_ok {
                return None;
            }
            (found, slice, kk)
        };
        let ((mobj, k, op, e, d, v), slice, kk) = found;
        let mt = self.ty(mobj);
        let Ty::Map(kt, vt) = self.tget(mt) else { return None };
        if !(self.c.types.is_int(vt) || self.c.types.is_float(vt)) || self.ty(v) != vt {
            return None;
        }
        if let Some((_, sobj, _)) = &slice {
            let st = self.ty(*sobj);
            if !matches!(self.tget(st), Ty::Str) || !kk.is_some_and(|kk| boxed_ok(self, kk)) {
                return None;
            }
        }
        let two = slice.is_some();
        let span = self.expr_span(v);
        let vct = self.ctype(vt);
        let vd = self.desc(vt);
        self.push_temps();
        let (slot, ins) = (self.fresh("slot"), self.fresh("ins"));
        match slice {
            Some((call, sobj, sargs)) => {
                let sv = self.expr(sobj);
                let a = self.expr(sargs[0]);
                let a = self.int_code(a);
                let (b, has_end) = match sargs.get(1) {
                    Some(&b) => {
                        let b = self.expr(b);
                        (self.int_code(b), "true")
                    }
                    None => ("0".to_string(), "false"),
                };
                let loc = self.loc(self.expr_span(call));
                // (a local narrowed to the map, `if (!m) m = new Map()`: its map, not the union it's kept in)
                let mp = self.place(mobj);
                let mp = self.project_place(mp, mt);
                let lv = self.place_lvalue(&mp);
                self.line(format!("bool {ins}; {vct} *{slot} = ({vct} *)tv_map_slot_slice(&{lv}, {vd}, {}, {a}, {b}, {has_end}, {loc}, &{ins});", sv.code));
            }
            None => {
                let kv = self.expr(k);
                let kv = self.coerce(kv, kt);
                let kc = self.consume(kv);
                let kct = self.ctype(kt);
                let kd = self.desc(kt);
                let key = self.fresh("key");
                self.line(format!("{kct} {key} = {kc};"));
                let mp = self.place(mobj);
                let mp = self.project_place(mp, mt);
                let lv = self.place_lvalue(&mp);
                self.line(format!("bool {ins}; {vct} *{slot} = ({vct} *)tv_map_slot(&{lv}, {kd}, {vd}, &{key}, &{ins});"));
            }
        }
        let dv = self.expr(d);
        let dv = self.coerce(dv, vt);
        self.line(format!("if ({ins}) *{slot} = {};", dv.code));
        let ev = self.expr(e);
        let r = self.arith(op, Val::plain(format!("(*{slot})"), vt), ev, vt, span);
        self.line(format!("*{slot} = {};", r.code));
        self.pop_temps();
        Some(if two { 2 } else { 1 })
    }

    /// For each statement of `ss`, the locals declared in `ss` whose last use it is (none for the
    /// last statement: the block's end releases those anyway).
    fn last_uses(&self, ss: &[StmtId]) -> Vec<Vec<u32>> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let locals_in = |exprs: &[ExprId]| -> FxSet<u32> {
            exprs.iter().filter_map(|&e| match self.ident_fact(m, e) {
                Some(IdentFact::Local(k)) => Some(k),
                _ => None,
            }).collect()
        };
        // A local initialized from an expression that mentions `v` may borrow from `v`: its uses
        // count as uses of `v`.
        let mut bindings = Vec::new();
        for &x in ss {
            collect_bindings(ast, x, &mut bindings);
        }
        let mut deps: FxMap<u32, FxSet<u32>> = FxMap::default();
        for (k, init) in bindings {
            let mut exprs = Vec::new();
            collect_exprs_expr(ast, init, &mut exprs);
            deps.entry(k).or_default().extend(locals_in(&exprs));
        }
        let uses: Vec<FxSet<u32>> = ss.iter().map(|&x| {
            let mut exprs = Vec::new();
            collect_exprs_stmt(ast, x, &mut exprs);
            let mut set = locals_in(&exprs);
            let mut stack: Vec<u32> = set.iter().copied().collect();
            while let Some(k) = stack.pop() {
                if let Some(ds) = deps.get(&k) {
                    for &d in ds {
                        if set.insert(d) {
                            stack.push(d);
                        }
                    }
                }
            }
            set
        }).collect();
        let mut plan = vec![Vec::new(); ss.len()];
        for (i, &x) in ss.iter().enumerate() {
            let StmtKind::Let { name_span, .. } = &ast.stmt(x).kind else { continue };
            let k = name_span.start;
            let last = (i..ss.len()).rev().find(|&j| j == i || uses[j].contains(&k)).unwrap_or(i);
            if last + 1 < ss.len() {
                plan[last].push(k);
            }
        }
        plan
    }

    /// Releases a local now if its release is still pending in the current scope (it owns its
    /// value), and drops that pending release.
    fn release_local_now(&mut self, k: u32) {
        let Some(local) = self.b().locals.get(&k).cloned() else { return };
        if !self.is_rc(local.ty) {
            return;
        }
        let rel = format!("{};", self.release_code(local.ty, &local.access));
        let scope = self.b().scopes.last_mut().unwrap();
        if let Some(pos) = scope.releases.iter().rposition(|r| *r == rel) {
            scope.releases.remove(pos);
            self.line(rel);
        }
    }

    // ------------------------------------------------------------ closures analysis

    /// Which locals must live in heap cells, and which arrows can keep their environment on the stack.
    pub(crate) fn analyze_closures_pub(&mut self, m: u32, kind: &FnBodyKind) -> (FxSet<u32>, FxSet<ExprId>, FxSet<u32>) {
        self.analyze_closures(m, kind)
    }

    fn analyze_closures(&mut self, m: u32, kind: &FnBodyKind) -> (FxSet<u32>, FxSet<ExprId>, FxSet<u32>) {
        let ast = self.ast(m);
        let mut exprs = Vec::new();
        match kind {
            FnBodyKind::Block(s) => collect_exprs_stmt(ast, *s, &mut exprs),
            FnBodyKind::Expr(e) => collect_exprs_expr(ast, *e, &mut exprs),
            FnBodyKind::Init => {}
        }
        let facts = self.facts(m);
        let mut stack_arrows = FxSet::default();
        let mut called_cands: Vec<(ExprId, usize, ExprId)> = Vec::new();
        let mut mutated: FxSet<u32> = FxSet::default();
        let mut heap_captures: FxSet<u32> = FxSet::default();
        let root = |e: ExprId| -> Option<u32> {
            let mut e = e;
            loop {
                match &ast.expr(e).kind {
                    ExprKind::Paren(x) | ExprKind::NonNull(x) => e = *x,
                    ExprKind::Member { obj, .. } | ExprKind::Index { obj, .. } => e = *obj,
                    ExprKind::Ident(_) => {
                        return match facts.idents.get(&e) {
                            Some(IdentFact::Local(k)) => Some(*k),
                            _ => None,
                        };
                    }
                    _ => return None,
                }
            }
        };
        for &e in &exprs {
            match &ast.expr(e).kind {
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => {
                    if let Some(k) = root(*t) {
                        mutated.insert(k);
                    }
                }
                ExprKind::Call { callee, args, .. } => {
                    for a in args {
                        if a.by_ref.is_some()
                            && let Some(k) = root(a.expr)
                        {
                            mutated.insert(k);
                        }
                    }
                    // (a suite's test runs its function, and the `afterEach` one, right there)
                    if let Some(fact) = facts.calls.get(&e)
                        && let Callee::Builtin { ns: None, name } = fact.callee
                        && self.c.interner.get(name) == "__test"
                    {
                        for a in args.iter().skip(1) {
                            let mut nested = Vec::new();
                            collect_exprs_expr(ast, a.expr, &mut nested);
                            if matches!(&ast.expr(a.expr).kind, ExprKind::Arrow(f) if !f.is_async)
                                && !nested.iter().any(|&x| x != a.expr && matches!(ast.expr(x).kind, ExprKind::Arrow(_)))
                            {
                                stack_arrows.insert(a.expr);
                            }
                        }
                    }
                    // (`Array.from(x, (v, i) => ...)` calls its callback while it runs, and keeps it no
                    // longer: an arrow given for it lives on the stack)
                    if let Some(fact) = facts.calls.get(&e)
                        && let Callee::Builtin { ns: Some(ns), name } = fact.callee
                        && self.c.interner.get(ns) == "Array"
                        && self.c.interner.get(name) == "from"
                        && let Some(a) = args.get(1)
                        && matches!(&ast.expr(a.expr).kind, ExprKind::Arrow(f) if !f.is_async)
                    {
                        let mut nested = Vec::new();
                        collect_exprs_expr(ast, a.expr, &mut nested);
                        if !nested.iter().any(|&x| x != a.expr && matches!(ast.expr(x).kind, ExprKind::Arrow(_))) {
                            stack_arrows.insert(a.expr);
                        }
                    }
                    // (a function or method that only calls its callback: an arrow given for it
                    // lives on the stack)
                    if facts.calls.get(&e).is_some_and(|f| matches!(f.callee, Callee::Fn(..) | Callee::ClassMethod { .. } | Callee::StaticMethod(..))) {
                        for (j, a) in args.iter().enumerate() {
                            if matches!(&ast.expr(a.expr).kind, ExprKind::Arrow(f) if !f.is_async) {
                                let mut nested = Vec::new();
                                collect_exprs_expr(ast, a.expr, &mut nested);
                                if !nested.iter().any(|&x| x != a.expr && matches!(ast.expr(x).kind, ExprKind::Arrow(_))) {
                                    called_cands.push((e, j, a.expr));
                                }
                            }
                        }
                    }
                    if let Some(fact) = facts.calls.get(&e)
                        && let ExprKind::Member { obj, name, .. } = &ast.expr(*callee).kind
                            && let Callee::Method(recv) = fact.callee
                        {
                            if is_mutating_method(self.c.interner.get(*name))
                                && let Some(k) = root(*obj)
                            {
                                mutated.insert(k);
                            }
                            // Only callback-consuming built-ins keep a direct arrow on the stack.
                            // Methods such as `push` store their argument instead of invoking it.
                            let sync_callback = match self.tget(recv) {
                                Ty::Array(_) => matches!(self.c.interner.get(*name), "map" | "flatMap" | "filter" | "forEach" | "some" | "every" | "find" | "findIndex" | "reduce" | "sort"),
                                Ty::Map(_, _) | Ty::Set(_) => self.c.interner.get(*name) == "forEach",
                                _ => false,
                            };
                            if sync_callback && let Some(a) = args.first()
                                && matches!(&ast.expr(a.expr).kind, ExprKind::Arrow(f) if !f.is_async)
                            {
                                let mut nested = Vec::new();
                                collect_exprs_expr(ast, a.expr, &mut nested);
                                // A nested closure can escape the call and needs heap captures.
                                if !nested.iter().any(|&x| x != a.expr && matches!(ast.expr(x).kind, ExprKind::Arrow(_))) {
                                    stack_arrows.insert(a.expr);
                                }
                            }
                        }
                }
                _ => {}
            }
        }
        for (call, j, arrow) in called_cands {
            if self.arg_called_only(m, call, j) {
                stack_arrows.insert(arrow);
            }
        }
        let facts = self.facts(m);
        // `const f = (...) => ...` that this body only ever calls, `f(x)`, never passing it on,
        // storing it or using it in another closure: its environment lives on the stack for as
        // long as `f` does, its captures by address, as a callback's (no allocation per
        // evaluation, nor a heap cell per iteration for a loop variable it reads)
        let mut inner: FxSet<ExprId> = FxSet::default();
        for &e in &exprs {
            if matches!(ast.expr(e).kind, ExprKind::Arrow(_)) {
                let mut nested = Vec::new();
                collect_exprs_expr(ast, e, &mut nested);
                inner.extend(nested.into_iter().filter(|&x| x != e));
            }
        }
        let callees: FxSet<ExprId> = exprs.iter().filter_map(|&e| match &ast.expr(e).kind {
            ExprKind::Call { callee, .. } => Some(*callee),
            _ => None,
        }).collect();
        let mut const_arrows: FxMap<u32, ExprId> = FxMap::default();
        for st in ast.stmts.iter() {
            if let crate::ast::StmtKind::Let { mutable: false, name_span, init: Some(i), .. } = &st.kind
                && matches!(&ast.expr(*i).kind, ExprKind::Arrow(a) if !a.is_async)
                && exprs.contains(i)
                && !inner.contains(i)
            {
                const_arrows.insert(name_span.start, *i);
            }
        }
        // (not in an async function or a generator, whose locals live in a frame)
        let framed = self.pending_async.is_some() || self.pending_gen.is_some();
        if !const_arrows.is_empty() && !framed {
            let mut used_otherwise: FxSet<u32> = FxSet::default();
            for &e in &exprs {
                if let ExprKind::Ident(_) = ast.expr(e).kind
                    && let Some(IdentFact::Local(k)) = facts.idents.get(&e)
                    && const_arrows.contains_key(k)
                    && (!callees.contains(&e) || inner.contains(&e))
                {
                    used_otherwise.insert(*k);
                }
            }
            for (k, ae) in const_arrows {
                if used_otherwise.contains(&k) {
                    continue;
                }
                let mut nested = Vec::new();
                collect_exprs_expr(ast, ae, &mut nested);
                if nested.iter().any(|&x| x != ae && matches!(ast.expr(x).kind, ExprKind::Arrow(_))) {
                    continue;
                }
                // (what it captures is declared before it, so it's there when its environment is
                // made; not itself; and it only reads them: the rest of the body is compiled
                // knowing a local no heap closure shares changes only where it's written)
                let at = ast.expr(ae).span.start;
                let caps = arrow_captures(ast, facts, ae);
                if !caps.iter().all(|&c| c != k && c < at) {
                    continue;
                }
                let writes = nested.iter().any(|&x| match &ast.expr(x).kind {
                    ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => local_root(ast, facts, *t).is_some_and(|r| caps.contains(&r)),
                    ExprKind::Call { callee, args, .. } => {
                        args.iter().any(|a| a.by_ref.is_some() && local_root(ast, facts, a.expr).is_some_and(|r| caps.contains(&r)))
                            || matches!(&ast.expr(*callee).kind, ExprKind::Member { obj, name, .. } if is_mutating_method(self.c.interner.get(*name)) && local_root(ast, facts, *obj).is_some_and(|r| caps.contains(&r)))
                    }
                    _ => false,
                });
                // (and only numbers and booleans: the body releases a counted value after its
                // last use there, not knowing a later call of `f` reads it)
                let scalar = |t: TyId| matches!(self.tget(t), Ty::Int | Ty::F64 | Ty::F32 | Ty::Bool | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64);
                let scalars = caps.iter().all(|&c| {
                    exprs.iter().all(|&x| {
                        !matches!(facts.idents.get(&x), Some(IdentFact::Local(l)) if *l == c) || facts.expr_ty.get(x as usize).is_some_and(|&t| scalar(t))
                    })
                });
                if !writes && scalars {
                    stack_arrows.insert(ae);
                }
            }
        }
        for &e in &exprs {
            if matches!(ast.expr(e).kind, ExprKind::Arrow(_)) && !stack_arrows.contains(&e) {
                for k in arrow_captures(ast, facts, e) {
                    heap_captures.insert(k);
                }
            }
        }
        let mut boxed: FxSet<u32> = heap_captures.intersection(&mutated).copied().collect();
        boxed.extend(heap_captures.iter().filter(|k| facts.self_init.contains(k)));
        (boxed, stack_arrows, mutated)
    }

    /// For each local used in `all`: does every use keep an array uniquely owned? Allowed uses:
    /// `x[i]` (read or write), `x.length`, receiving a built-in method that doesn't hand out the
    /// buffer, and `&x` passed to an `inout` parameter that is itself leak-free. Returns the
    /// verdicts and whether each local is passed as `inout`.
    pub(crate) fn uniqueness_uses(&mut self, m: u32, all: &[ExprId]) -> (FxMap<u32, bool>, FxSet<u32>) {
        let ast = self.ast(m);
        let facts = self.facts(m);
        let mut parent: FxMap<ExprId, ExprId> = FxMap::default();
        for &e in all {
            let mut kids = Vec::new();
            match &ast.expr(e).kind {
                ExprKind::Member { obj, .. } => kids.push(*obj),
                ExprKind::Index { obj, index, .. } => {
                    kids.push(*obj);
                    kids.push(*index);
                }
                ExprKind::Call { callee, args, .. } => {
                    kids.push(*callee);
                    kids.extend(args.iter().map(|a| a.expr));
                }
                _ => {}
            }
            for k in kids {
                parent.insert(k, e);
            }
        }
        let mut ok: FxMap<u32, bool> = FxMap::default();
        let mut pending: Vec<(u32, u32, u32, usize)> = Vec::new();
        let mut passed: FxSet<u32> = FxSet::default();
        for &e in all {
            let ExprKind::Ident(_) = ast.expr(e).kind else { continue };
            let Some(IdentFact::Local(k)) = facts.idents.get(&e) else { continue };
            let fine = match parent.get(&e).map(|&p| (p, &ast.expr(p).kind)) {
                Some((_, ExprKind::Index { obj, .. })) if *obj == e => true,
                Some((p, ExprKind::Member { obj, name, .. })) if *obj == e => {
                    let n = self.c.interner.get(*name);
                    let method_call = parent.get(&p).map(|&c| matches!(&ast.expr(c).kind, ExprKind::Call { callee, .. } if *callee == p) && matches!(facts.calls.get(&c).map(|f| &f.callee), Some(Callee::Method(_)))).unwrap_or(false);
                    n == "length" || (method_call && !matches!(n, "slice" | "concat" | "reverse" | "sort" | "fill"))
                }
                Some((c, ExprKind::Call { args, .. })) => {
                    let idx = args.iter().position(|a| a.expr == e && a.by_ref.is_some());
                    match (idx, facts.calls.get(&c).map(|f| &f.callee)) {
                        (Some(i), Some(Callee::Fn(fm, fi))) => {
                            pending.push((*k, *fm, *fi, i));
                            passed.insert(*k);
                            true
                        }
                        _ => false,
                    }
                }
                _ => false,
            };
            let entry = ok.entry(*k).or_insert(true);
            *entry &= fine;
        }
        for &e in all {
            if matches!(ast.expr(e).kind, ExprKind::Arrow(_)) {
                for k in arrow_captures(ast, facts, e) {
                    ok.insert(k, false);
                }
            }
        }
        for (k, fm, fi, i) in pending {
            if !self.inout_leak_free(fm, fi, i) {
                ok.insert(k, false);
            }
        }
        (ok, passed)
    }

    /// Does function (m, item) keep its `inout` parameter `idx` uniquely owned (never copying,
    /// storing or capturing it)? Such parameters are made unique by callers, and the callee
    /// writes elements without copy-on-write checks.
    pub(crate) fn inout_leak_free(&mut self, m: u32, item: u32, idx: usize) -> bool {
        if let Some(&v) = self.leak_free.get(&(m, item, idx)) {
            return v;
        }
        // Recursion: assume it leaks until proven otherwise.
        self.leak_free.insert((m, item, idx), false);
        let ast = self.ast(m);
        let crate::ast::ItemKind::Function(f) = &ast.items[item as usize].kind else { return false };
        let Some(p) = f.params.get(idx) else { return false };
        if !p.inout {
            return false;
        }
        let key = p.span.start;
        let mut all = Vec::new();
        collect_exprs_stmt(ast, f.body, &mut all);
        let (ok, _) = self.uniqueness_uses(m, &all);
        let v = ok.get(&key).copied().unwrap_or(true);
        self.leak_free.insert((m, item, idx), v);
        v
    }

    /// Makes array locals unique before a loop when every use inside keeps them unique, so
    /// element writes (and leak-free `inout` calls) in the loop skip copy-on-write checks.
    fn hoist_unique(&mut self, stmts: &[StmtId], exprs: &[ExprId]) -> Vec<u32> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let mut all = Vec::new();
        for &s in stmts {
            collect_exprs_stmt(ast, s, &mut all);
        }
        for &e in exprs {
            collect_exprs_expr(ast, e, &mut all);
        }
        let facts = self.facts(m);
        let mut written: FxSet<u32> = FxSet::default();
        let mut pushed: FxSet<u32> = FxSet::default();
        for &e in &all {
            // `xs.push(v)` in the loop: pushes then skip the ownership check.
            if let ExprKind::Call { callee, .. } = &ast.expr(e).kind
                && let ExprKind::Member { obj, name, optional: false, .. } = &ast.expr(*callee).kind
                && self.sym(*name) == "push"
                && let (ExprKind::Ident(_), Some(IdentFact::Local(k))) = (&ast.expr(*obj).kind, facts.idents.get(obj))
            {
                written.insert(*k);
                pushed.insert(*k);
            }
            if let ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } = &ast.expr(e).kind {
                let mut cur = *t;
                loop {
                    match &ast.expr(cur).kind {
                        ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Member { obj: x, .. } => cur = *x,
                        ExprKind::Index { obj, .. } => {
                            if let (ExprKind::Ident(_), Some(IdentFact::Local(k))) = (&ast.expr(*obj).kind, facts.idents.get(obj)) {
                                written.insert(*k);
                            }
                            cur = *obj;
                        }
                        _ => break,
                    }
                }
            }
        }
        let (ok, passed) = self.uniqueness_uses(m, &all);
        let mut added = Vec::new();
        // A local in a heap cell can be reassigned by any call into a closure that shares it.
        let boxed = &self.b().boxed;
        let candidates: Vec<u32> = ok.iter().filter(|(k, v)| **v && (written.contains(k) || passed.contains(k)) && !boxed.contains(k)).map(|(k, _)| *k).collect();
        for k in candidates {
            if self.b().unique.contains(&k) {
                continue;
            }
            let Some(local) = self.b().locals.get(&k).cloned() else { continue };
            let Ty::Array(_) = self.tget(local.ty) else { continue };
            // (arrays are shared, so there's nothing to copy first: the only one that can't be
            // changed in place, the empty placeholder, has no capacity and no elements, so a
            // push takes the slow path that replaces it and an index is out of bounds)
            self.b().unique.insert(k);
            added.push(k);
            if pushed.contains(&k) && self.only_push_index_length(&all, k) {
                let cv = self.fresh("cap");
                self.line(format!("tv_int {cv} = {0}.p ? {0}.p->cap : 0;", local.access));
                self.b().push_caps.insert(k, cv);
            }
        }
        // Arrays the loop only indexes (and reads the length of), of plain elements: their
        // elements' address and length in locals, refreshed when an index write appends (no
        // reload of the array after every write that might have)
        let cands: Vec<u32> = added
            .iter()
            .copied()
            .filter(|&k| {
                let lt = self.bodies.last().and_then(|b| b.locals.get(&k)).map(|l| l.ty);
                !pushed.contains(&k)
                    && lt.is_some_and(|t| matches!(self.tget(t), Ty::Array(et) if !self.is_rc(et)))
                    && self.only_push_index_length(&all, k)
            })
            .collect();
        // (not one an inner loop indexes too: that loop keeps its own, and the outer copies only
        // cost it registers)
        let nested: Vec<ExprId> = {
            let ast = self.ast(m);
            let mut inner = Vec::new();
            let span_of = |s: StmtId| ast.stmt(s).span;
            let roots: Vec<Span> = stmts.iter().map(|&s| span_of(s)).collect();
            for st in ast.stmts.iter() {
                if matches!(st.kind, StmtKind::While(..) | StmtKind::DoWhile(..) | StmtKind::For { .. } | StmtKind::ForOf { .. })
                    && roots.iter().any(|r| r.file == st.span.file && r.start <= st.span.start && st.span.end <= r.end && *r != st.span)
                {
                    inner.push(st.span);
                }
            }
            all.iter().copied().filter(|&e| {
                let sp = ast.expr(e).span;
                inner.iter().any(|r| r.file == sp.file && r.start <= sp.start && sp.end <= r.end)
            }).collect()
        };
        let cands: Vec<u32> = cands.into_iter().filter(|&k| !self.indexes(&nested, k)).collect();
        if !cands.is_empty() && !self.may_resize(m, &all, &cands) {
            for k in cands {
                let local = self.b().locals[&k].clone();
                let Ty::Array(et) = self.tget(local.ty) else { continue };
                let ect = self.ctype(et);
                let (d, n) = (self.fresh("cd"), self.fresh("cn"));
                self.line(format!("{ect} *{d} = ({ect} *)tv_arr_data({0}); tv_int {n} = tv_arr_len({0});", local.access));
                self.b().arr_cache.insert(k, (d, n));
            }
        }
        added
    }

    /// `for (let i = 0; i < xs.length; i++)` (or `i += 1`) whose body never assigns `i` and
    /// uses `xs` only as `xs[...]` and `xs.length` (so its length can't change): (i, xs).
    fn counted_loop(&self, init: Option<StmtId>, cond: Option<ExprId>, step: Option<ExprId>, body: StmtId) -> Option<(u32, u32)> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let local = |e: ExprId| match (&ast.expr(e).kind, facts.idents.get(&e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
            _ => None,
        };
        let StmtKind::Let { name_span, init: Some(i0), .. } = &ast.stmt(init?).kind else { return None };
        if !matches!(ast.expr(*i0).kind, ExprKind::Int(0)) || facts.bindings.get(&init?).copied() != Some(INT) {
            return None;
        }
        let ik = name_span.start;
        let ExprKind::Binary(BinOp::Lt, l, r) = &ast.expr(cond?).kind else { return None };
        if local(*l) != Some(ik) {
            return None;
        }
        let ExprKind::Member { obj, name, optional: false, .. } = &ast.expr(*r).kind else { return None };
        if self.sym(*name) != "length" {
            return None;
        }
        let xk = local(*obj)?;
        if !matches!(self.tget(facts.expr_ty[*obj as usize]), Ty::Array(_)) {
            return None;
        }
        let step_ok = match &ast.expr(step?).kind {
            ExprKind::Update { target, inc: true, .. } => local(*target) == Some(ik),
            ExprKind::Assign(AssignOp::Op(BinOp::Add), t, v) => local(*t) == Some(ik) && matches!(ast.expr(*v).kind, ExprKind::Int(1)),
            _ => false,
        };
        if !step_ok {
            return None;
        }
        let mut all = Vec::new();
        collect_exprs_stmt(ast, body, &mut all);
        let mut allowed = FxSet::default();
        for &e in &all {
            match &ast.expr(e).kind {
                // `i` must not change; `xs` itself neither (elements may), nor its length
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } if local(*t) == Some(ik) || local(*t) == Some(xk) => return None,
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } if matches!(&ast.expr(*t).kind, ExprKind::Member { name, .. } if self.sym(*name) == "length") => return None,
                ExprKind::Call { args, .. } if args.iter().any(|a| a.by_ref.is_some() && (local(a.expr) == Some(ik) || local(a.expr) == Some(xk))) => return None,
                ExprKind::Index { obj, .. } if local(*obj) == Some(xk) => {
                    allowed.insert(*obj);
                }
                ExprKind::Member { obj, name, .. } if local(*obj) == Some(xk) && self.sym(*name) == "length" => {
                    allowed.insert(*obj);
                }
                _ => {}
            }
        }
        if all.iter().any(|&e| local(e) == Some(xk) && !allowed.contains(&e)) {
            return None;
        }
        Some((ik, xk))
    }

    /// Works out before a loop the reciprocal of each `int` local `d` that `x % d` divides by in a
    /// loop nested in it, when nothing in the loop assigns `d` (each remainder then multiplies
    /// instead of dividing: TVG_REM_BY). Only for nests, where the setup pays for itself. The
    /// locals it added to `divs`.
    fn hoist_divisors(&mut self, body: StmtId, exprs: &[ExprId]) -> Vec<u32> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let local = |e: ExprId| match (&ast.expr(e).kind, facts.idents.get(&e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
            _ => None,
        };
        let mut all = Vec::new();
        collect_exprs_stmt(ast, body, &mut all);
        for &e in exprs {
            collect_exprs_expr(ast, e, &mut all);
        }
        // the `%`s in loops inside this one
        fn inner_loops(ast: &ast::Ast, s: StmtId, depth: u32, out: &mut Vec<StmtId>) {
            match &ast.stmt(s).kind {
                StmtKind::While(_, b) | StmtKind::DoWhile(b, _) | StmtKind::For { body: b, .. } | StmtKind::ForOf { body: b, .. } => {
                    if depth > 0 {
                        out.push(s);
                    }
                    inner_loops(ast, *b, depth + 1, out);
                }
                StmtKind::If(_, t, e) => {
                    inner_loops(ast, *t, depth, out);
                    if let Some(e) = e {
                        inner_loops(ast, *e, depth, out);
                    }
                }
                StmtKind::Block(xs) => xs.iter().for_each(|&x| inner_loops(ast, x, depth, out)),
                StmtKind::Switch(_, cases) => cases.iter().for_each(|c| c.body.iter().for_each(|&x| inner_loops(ast, x, depth, out))),
                StmtKind::Try { body, catch, finally } => {
                    inner_loops(ast, *body, depth, out);
                    if let Some(c) = catch {
                        inner_loops(ast, c.body, depth, out);
                    }
                    if let Some(f) = finally {
                        inner_loops(ast, *f, depth, out);
                    }
                }
                _ => {}
            }
        }
        // (a C local: an `await` in the nest would resume past it)
        if all.iter().any(|&e| matches!(ast.expr(e).kind, ExprKind::Await(_))) {
            return Vec::new();
        }
        let mut loops = Vec::new();
        inner_loops(ast, body, 1, &mut loops);
        let mut inner = Vec::new();
        for l in loops {
            collect_exprs_stmt(ast, l, &mut inner);
        }
        let ty = |e: ExprId| facts.expr_ty.get(e as usize).copied().unwrap_or(ERROR);
        let mut wanted: Vec<u32> = Vec::new();
        for &e in &inner {
            if let ExprKind::Binary(BinOp::Rem, l, r) = &ast.expr(e).kind
                && ty(e) == INT
                && ty(*l) == INT
                && let Some(k) = local(*r)
                && !wanted.contains(&k)
            {
                wanted.push(k);
            }
        }
        let assigned = |k: u32| {
            all.iter().any(|&e| match &ast.expr(e).kind {
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => local(*t) == Some(k),
                ExprKind::Call { args, .. } => args.iter().any(|a| a.by_ref.is_some() && local(a.expr) == Some(k)),
                _ => false,
            })
        };
        wanted.retain(|&k| !assigned(k));
        let mut added = Vec::new();
        for k in wanted {
            if self.b().divs.contains_key(&k) || self.b().boxed.contains(&k) {
                continue;
            }
            let Some(local) = self.b().locals.get(&k).cloned() else { continue };
            if local.ty != INT {
                continue;
            }
            let dv = self.fresh("dv");
            self.line(format!("tvg_udiv {dv} = tvg_udiv_new({});", local.access));
            self.b().divs.insert(k, dv);
            added.push(k);
        }
        added
    }

    /// `for (;;) {`, unrolled four times when the loop is small, innermost and does checked integer
    /// arithmetic (`total += x`, `n * k`) or calls what may (`total = await step(total, i)`):
    /// each check is a way out of the loop, and LLVM unrolls a loop with more than one only when
    /// told to (awaited async calls summed in a loop: 15.6 ms to 13.5, Rust's unchecked 14.0).
    fn open_loop(&mut self, body: StmtId, also: &[ExprId]) {
        let ast = self.ast(self.cur_m());
        let facts = self.facts(self.cur_m());
        let mut all = Vec::new();
        for &e in also {
            collect_exprs_expr(ast, e, &mut all);
        }
        collect_exprs_stmt(ast, body, &mut all);
        let int = |e: ExprId| facts.expr_ty.get(e as usize).copied() == Some(INT);
        let small = all.len() <= 48 && !stmt_has_loop(ast, body) && !all.iter().any(|&e| matches!(ast.expr(e).kind, ExprKind::Arrow(_)));
        if small
            && all.iter().any(|&e| match &ast.expr(e).kind {
                ExprKind::Binary(BinOp::Add | BinOp::Sub | BinOp::Mul, ..) | ExprKind::Assign(AssignOp::Op(BinOp::Add | BinOp::Sub | BinOp::Mul), ..) | ExprKind::Update { .. } => int(e),
                // (a small function's, once LLVM inlines it)
                ExprKind::Call { .. } => true,
                _ => false,
            })
        {
            self.line("TV_UNROLL");
        }
        self.open("for (;;) {");
    }

    /// A `for` loop's `for (;;) { ... }` (its init already emitted).
    #[allow(clippy::too_many_arguments)]
    fn for_loop(&mut self, cond: Option<ExprId>, step: Option<ExprId>, body: StmtId, cont: &str, bounds: Option<(u32, u32)>, nowrap: Option<u32>, iteration_binding: Option<u32>) {
        let also: Vec<ExprId> = [cond, step].into_iter().flatten().collect();
        self.open_loop(body, &also);
        if let Some(c) = cond {
            self.cond_break(c);
        }
        if let Some(pair) = bounds {
            self.b().in_bounds.push(pair);
        }
        self.loop_body(body, cont);
        if bounds.is_some() {
            self.b().in_bounds.pop();
        }
        self.line(format!("{cont}:;"));
        if let Some(key) = iteration_binding {
            self.copy_iteration_binding(key);
        }
        if let Some(st) = step {
            self.push_temps();
            self.b().nowrap_step = nowrap;
            let _ = self.expr(st);
            self.b().nowrap_step = None;
            self.pop_temps();
        }
        self.close("}");
    }

    /// `for (let i = a; i < n; i++)` (or `i <= n`) whose body reads or writes `xs[i + k]` for
    /// constant `k`s, where nothing in the loop changes `i`, `n` or the length of `xs` (only
    /// indexes it and reads its length): one test before the loop shows every such index in
    /// range, so a copy of the loop can skip their checks (see `version_check`). Small bodies
    /// without `await` only, as the body is emitted twice.
    fn version_plan(&mut self, init: Option<StmtId>, cond: Option<ExprId>, step: Option<ExprId>, body: StmtId) -> Option<VersionPlan> {
        let boxed = self.b().boxed.clone();
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let local = |e: ExprId| match (&ast.expr(e).kind, facts.idents.get(&e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
            _ => None,
        };
        let init = init?;
        let StmtKind::Let { name_span, .. } = &ast.stmt(init).kind else { return None };
        if facts.bindings.get(&init).copied() != Some(INT) {
            return None;
        }
        let ik = name_span.start;
        if boxed.contains(&ik) {
            return None;
        }
        let (le, hi) = match &ast.expr(cond?).kind {
            ExprKind::Binary(BinOp::Lt, l, r) if local(*l) == Some(ik) => (false, *r),
            ExprKind::Binary(BinOp::Le, l, r) if local(*l) == Some(ik) => (true, *r),
            _ => return None,
        };
        if facts.expr_ty.get(hi as usize).copied() != Some(INT) {
            return None;
        }
        let ExprKind::Update { target, inc: true, .. } = &ast.expr(step?).kind else { return None };
        if local(*target) != Some(ik) {
            return None;
        }
        // (no `for` inside: each copy of an outer one would hold two of the inner)
        fn has_loop(ast: &ast::Ast, s: StmtId) -> bool {
            match &ast.stmt(s).kind {
                StmtKind::For { .. } | StmtKind::ForOf { .. } => true,
                StmtKind::While(_, b) | StmtKind::DoWhile(b, _) => has_loop(ast, *b),
                StmtKind::If(_, t, e) => has_loop(ast, *t) || e.is_some_and(|e| has_loop(ast, e)),
                StmtKind::Block(xs) => xs.iter().any(|&x| has_loop(ast, x)),
                StmtKind::Switch(_, cases) => cases.iter().any(|c| c.body.iter().any(|&x| has_loop(ast, x))),
                StmtKind::Try { body, catch, finally } => has_loop(ast, *body) || catch.as_ref().is_some_and(|c| has_loop(ast, c.body)) || finally.is_some_and(|f| has_loop(ast, f)),
                _ => false,
            }
        }
        if has_loop(ast, body) {
            return None;
        }
        self.version_core(ik, le, hi, cond?, body, &FxSet::default())
    }

    /// `while (k < n) { ...; k += s; }` (or `k <= n`; the step the body's last statement, `s` a
    /// local nothing in the loop assigns, or a literal), indexing arrays at `k + c` as
    /// `version_plan`'s loops do: with `s >= 0` (tested with the rest, see `version_check`) `k`
    /// only rises, so the same test before it shows every index in range. The plan, and `s`.
    fn while_version_plan(&mut self, cond: ExprId, body: StmtId) -> Option<(VersionPlan, ExprId)> {
        let boxed = self.b().boxed.clone();
        let int_locals: FxSet<u32> = self.b().locals.iter().filter(|(_, l)| l.ty == INT).map(|(k, _)| *k).collect();
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let local = |e: ExprId| match (&ast.expr(e).kind, facts.idents.get(&e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
            _ => None,
        };
        let (le, k, hi) = match &ast.expr(cond).kind {
            ExprKind::Binary(BinOp::Lt, l, r) => (false, local(*l)?, *r),
            ExprKind::Binary(BinOp::Le, l, r) => (true, local(*l)?, *r),
            _ => return None,
        };
        if facts.expr_ty.get(hi as usize).copied() != Some(INT) || !int_locals.contains(&k) || boxed.contains(&k) {
            return None;
        }
        let StmtKind::Block(ss) = &ast.stmt(body).kind else { return None };
        let &last = ss.last()?;
        let StmtKind::Expr(se) = ast.stmt(last).kind else { return None };
        let ExprKind::Assign(crate::ast::AssignOp::Op(BinOp::Add), t, step) = ast.expr(se).kind else { return None };
        if local(t) != Some(k) {
            return None;
        }
        match &ast.expr(step).kind {
            ExprKind::Int(_) => {}
            ExprKind::Ident(_) if local(step).is_some() && facts.expr_ty.get(step as usize) == Some(&INT) => {
                let sk = local(step)?;
                let mut all = Vec::new();
                collect_exprs_stmt(ast, body, &mut all);
                if boxed.contains(&sk) || all.iter().any(|&e| match &ast.expr(e).kind {
                    ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => local(*t) == Some(sk),
                    ExprKind::Call { args, .. } => args.iter().any(|a| a.by_ref.is_some() && local(a.expr) == Some(sk)),
                    _ => false,
                }) {
                    return None;
                }
            }
            _ => return None,
        }
        let mut sv = Vec::new();
        collect_exprs_stmt(ast, last, &mut sv);
        let skip: FxSet<ExprId> = sv.into_iter().collect();
        // (no other loop inside: the body is written out twice)
        let mut inner = Vec::new();
        collect_exprs_stmt(ast, body, &mut inner);
        fn has_loop(ast: &ast::Ast, s: StmtId) -> bool {
            match &ast.stmt(s).kind {
                StmtKind::For { .. } | StmtKind::ForOf { .. } | StmtKind::While(..) | StmtKind::DoWhile(..) => true,
                StmtKind::If(_, t, e) => has_loop(ast, *t) || e.is_some_and(|e| has_loop(ast, e)),
                StmtKind::Block(xs) => xs.iter().any(|&x| has_loop(ast, x)),
                StmtKind::Switch(_, cases) => cases.iter().any(|c| c.body.iter().any(|&x| has_loop(ast, x))),
                StmtKind::Try { body, catch, finally } => has_loop(ast, *body) || catch.as_ref().is_some_and(|c| has_loop(ast, c.body)) || finally.is_some_and(|f| has_loop(ast, f)),
                _ => false,
            }
        }
        if has_loop(ast, body) {
            return None;
        }
        let plan = self.version_core(k, le, hi, cond, body, &skip)?;
        Some((plan, step))
    }

    /// `for (let j = a; j < n; j++)` over rows of arrays of plain values, `xs[i][j + k]` with `xs`
    /// and `i` the same the whole loop (`i` a local it doesn't change, or a literal), its body
    /// only arithmetic, comparisons, `Math` calls and such reads and writes (so nothing in it can
    /// resize or replace an array): with every row there and long enough, a copy of the loop
    /// reaches each row's elements from a local, unchecked at `j + k`.
    fn row_plan(&mut self, init: Option<StmtId>, cond: Option<ExprId>, step: Option<ExprId>, body: StmtId) -> Option<RowPlan> {
        let boxed = self.b().boxed.clone();
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let local = |e: ExprId| match (&ast.expr(e).kind, facts.idents.get(&e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
            _ => None,
        };
        let init = init?;
        let StmtKind::Let { name_span, .. } = &ast.stmt(init).kind else { return None };
        if facts.bindings.get(&init).copied() != Some(INT) {
            return None;
        }
        let jk = name_span.start;
        if boxed.contains(&jk) {
            return None;
        }
        let (le, hi) = match &ast.expr(cond?).kind {
            ExprKind::Binary(BinOp::Lt, l, r) if local(*l) == Some(jk) => (false, *r),
            ExprKind::Binary(BinOp::Le, l, r) if local(*l) == Some(jk) => (true, *r),
            _ => return None,
        };
        if facts.expr_ty.get(hi as usize).copied() != Some(INT) {
            return None;
        }
        match &ast.expr(step?).kind {
            ExprKind::Update { target, inc: true, .. } if local(*target) == Some(jk) => {}
            _ => return None,
        }
        // (only statements that can't leave the loop some other way or hold another loop)
        fn plain(ast: &ast::Ast, s: StmtId) -> bool {
            match &ast.stmt(s).kind {
                StmtKind::Expr(_) | StmtKind::Let { .. } | StmtKind::Break(None) | StmtKind::Continue(None) => true,
                StmtKind::If(_, t, e) => plain(ast, *t) && e.is_none_or(|e| plain(ast, e)),
                StmtKind::Block(xs) => xs.iter().all(|&x| plain(ast, x)),
                _ => false,
            }
        }
        if !plain(ast, body) {
            return None;
        }
        let mut all = Vec::new();
        collect_exprs_stmt(ast, body, &mut all);
        if all.len() > 400 {
            return None;
        }
        let mut sides = Vec::new();
        collect_exprs_expr(ast, hi, &mut sides);
        // (`hi` the same the whole loop: literals and locals the body doesn't change)
        let assigned = |k: u32| {
            all.iter().any(|&e| match &ast.expr(e).kind {
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => local(*t) == Some(k),
                _ => false,
            })
        };
        for &e in &sides {
            match &ast.expr(e).kind {
                ExprKind::Int(_) | ExprKind::Paren(_) | ExprKind::Binary(BinOp::Add | BinOp::Sub, ..) => {}
                ExprKind::Ident(_) if local(e).is_some_and(|k| !boxed.contains(&k) && !assigned(k)) => {}
                _ => return None,
            }
        }
        if assigned(jk) {
            return None;
        }
        // what the body is made of
        for &e in &all {
            let ok = match &ast.expr(e).kind {
                ExprKind::Int(_) | ExprKind::Float(_) | ExprKind::Bool(_) | ExprKind::Str(_) | ExprKind::Paren(_) | ExprKind::Binary(..) | ExprKind::Unary(..) | ExprKind::Cond(..) | ExprKind::Index { optional: false, .. } | ExprKind::NonNull(_) => true,
                ExprKind::Ident(n) => local(e).is_some_and(|k| !boxed.contains(&k)) || self.sym(*n) == "Math" && !matches!(facts.idents.get(&e), Some(IdentFact::Local(_))),
                ExprKind::Member { name, optional: false, .. } => matches!(self.sym(*name), "length" | "min" | "max" | "abs" | "floor" | "ceil" | "round" | "sqrt" | "trunc" | "sign" | "hypot" | "imul" | "pow"),
                ExprKind::Call { callee, args, .. } => {
                    args.iter().all(|a| a.by_ref.is_none() && !matches!(ast.expr(a.expr).kind, ExprKind::Spread(_)))
                        && matches!(&ast.expr(*callee).kind, ExprKind::Member { obj, .. } if matches!(ast.expr(*obj).kind, ExprKind::Ident(n) if self.sym(n) == "Math"))
                }
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => {
                    let mut cur = *t;
                    while let ExprKind::Paren(x) | ExprKind::NonNull(x) = &ast.expr(cur).kind {
                        cur = *x;
                    }
                    match &ast.expr(cur).kind {
                        ExprKind::Ident(_) => local(cur).is_some_and(|k| !boxed.contains(&k)) && self.c.types.is_numeric(facts.expr_ty[e as usize]),
                        ExprKind::Index { .. } => self.c.types.is_numeric(facts.expr_ty[e as usize]),
                        _ => false,
                    }
                }
                _ => false,
            };
            if !ok {
                return None;
            }
        }
        // the rows: `xs[i]` as the array of `xs[i][...]` (or of `xs[i].length`)
        let mut parent: FxMap<ExprId, ExprId> = FxMap::default();
        for &e in &all {
            match &ast.expr(e).kind {
                ExprKind::Index { obj, index, .. } => {
                    parent.insert(*obj, e);
                    parent.insert(*index, e);
                }
                ExprKind::Member { obj, .. } => {
                    parent.insert(*obj, e);
                }
                _ => {}
            }
        }
        let mut targets: FxSet<ExprId> = FxSet::default();
        for &e in &all {
            if let ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } = &ast.expr(e).kind {
                let mut cur = *t;
                while let ExprKind::Paren(x) | ExprKind::NonNull(x) = &ast.expr(cur).kind {
                    cur = *x;
                }
                targets.insert(cur);
            }
        }
        let mut rows: Vec<(u32, (Option<u32>, i64), bool, i64, i64)> = Vec::new();
        let mut row_bases: FxSet<ExprId> = FxSet::default();
        for &e in &all {
            let ExprKind::Index { obj, index, .. } = &ast.expr(e).kind else { continue };
            let Some(xk) = local(*obj) else { continue };
            let Ty::Array(row_t) = self.tget(facts.expr_ty[*obj as usize]) else { return None };
            let Ty::Array(et) = self.tget(row_t) else { continue };
            if !self.c.types.is_numeric(et) || boxed.contains(&xk) || assigned(xk) {
                return None;
            }
            // (`xs[i]` used only as an array to index or measure)
            let used = match parent.get(&e).map(|&p| (p, &ast.expr(p).kind)) {
                Some((_, ExprKind::Index { obj: o, .. })) if *o == e => true,
                Some((_, ExprKind::Member { obj: o, name, .. })) if *o == e && self.sym(*name) == "length" => true,
                _ => false,
            };
            if !used {
                return None;
            }
            let at = match ast.expr(*index).kind {
                ExprKind::Int(v) if v < 1 << 30 => (None, v as i64),
                _ => {
                    let mut found = None;
                    for &x in &all {
                        if let Some(k) = local(x)
                            && k != jk
                            && !assigned(k)
                            && facts.expr_ty.get(x as usize) == Some(&INT)
                            && let Some(off) = self.index_offset(*index, k)
                        {
                            found = Some((Some(k), off));
                            break;
                        }
                    }
                    found?
                }
            };
            if at.0 == Some(jk) {
                return None;
            }
            row_bases.insert(*obj);
            // (the offsets of `j` it's indexed at)
            let (written, off) = match parent.get(&e).map(|&p| (p, &ast.expr(p).kind)) {
                Some((p, ExprKind::Index { index: ji, .. })) => (targets.contains(&p), self.index_offset(*ji, jk)),
                _ => (false, None),
            };
            // (a write elsewhere than `j + k` could append: no copy)
            if written && off.is_none() {
                return None;
            }
            match rows.iter_mut().find(|r| r.0 == xk && r.1 == at) {
                Some(r) => {
                    r.2 |= written;
                    if let Some(k) = off {
                        r.3 = r.3.min(k);
                        r.4 = r.4.max(k);
                    }
                }
                None => rows.push((xk, at, written, off.unwrap_or(i64::MAX), off.unwrap_or(i64::MIN))),
            }
        }
        // (an array of rows used for nothing else)
        for &e in &all {
            if let Some(k) = local(e)
                && rows.iter().any(|r| r.0 == k)
                && !row_bases.contains(&e)
            {
                return None;
            }
        }
        if rows.is_empty() || !rows.iter().any(|r| r.3 <= r.4) {
            return None;
        }
        Some(RowPlan { jk, le, hi, rows })
    }

    /// `xs[i][...]` on a row a row plan's copy keeps in a local.
    fn row_target(&self, target: ExprId) -> bool {
        let Some(b) = self.bodies.last() else { return false };
        if b.rows.is_empty() {
            return false;
        }
        let m = self.cur_m();
        let ExprKind::Index { obj, .. } = self.ast(m).expr(target).kind else { return false };
        let ExprKind::Index { obj: xs, .. } = self.ast(m).expr(obj).kind else { return false };
        matches!(self.ident_fact(m, xs), Some(IdentFact::Local(xk)) if b.rows.iter().any(|r| r.0 == xk))
    }

    /// `xs[i][index]` in a row plan's copy: the element's C lvalue, unchecked at `j + k`.
    fn row_index(&mut self, obj: ExprId, index: ExprId, span: Span) -> Option<String> {
        if self.b().rows.is_empty() {
            return None;
        }
        let m = self.cur_m();
        let ExprKind::Index { obj: xs, index: at, optional: false } = self.ast(m).expr(obj).kind else { return None };
        let Some(IdentFact::Local(xk)) = self.ident_fact(m, xs) else { return None };
        let rows = self.b().rows.clone();
        let lit = match self.ast(m).expr(at).kind {
            ExprKind::Int(v) => Some(v as i64),
            _ => None,
        };
        let (_, _, jk, d, n) = rows.iter().find(|r| {
            r.0 == xk
                && match r.1 {
                    (None, v) => lit == Some(v),
                    (Some(ik), off) => self.index_offset(at, ik) == Some(off),
                }
        })?;
        if let Some(k) = self.index_offset(index, *jk) {
            let j = self.b().locals.get(jk)?.access.clone();
            return Some(format!("({d}[{j} + {k}])"));
        }
        let i = self.expr(index);
        let i = self.int_code(i);
        let loc = self.loc(span);
        Some(format!("({d}[TV_IDX({i}, {n}, {loc})])"))
    }

    /// The rest of `version_plan`: loop variable `ik` up to `hi` (`le`: inclusive), its step
    /// (expressions `skip`) the only change to it in `body`.
    fn version_core(&mut self, ik: u32, le: bool, hi: ExprId, cond: ExprId, body: StmtId, skip: &FxSet<ExprId>) -> Option<VersionPlan> {
        let boxed = self.b().boxed.clone();
        let unique = self.b().unique.clone();
        let types: FxMap<u32, TyId> = self.b().locals.iter().map(|(k, l)| (*k, l.ty)).collect();
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let local = |e: ExprId| match (&ast.expr(e).kind, facts.idents.get(&e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
            _ => None,
        };
        let mut all = Vec::new();
        collect_exprs_stmt(ast, body, &mut all);
        if all.len() > 400 || all.iter().any(|&e| matches!(ast.expr(e).kind, ExprKind::Await(_))) {
            return None;
        }
        collect_exprs_expr(ast, cond, &mut all);
        // What the loop does with each array local: (indexed at i + k for these k, only indexed
        // or its length read)
        let mut uses: FxMap<u32, (Vec<i64>, bool)> = FxMap::default();
        let mut fine: FxSet<ExprId> = FxSet::default();
        // (the copy reaches the arrays' elements only through the pointers it holds: `xs[i]` read
        // as `T | undefined` reads them through the array instead, so such an array stays out)
        let mut through: FxSet<ExprId> = FxSet::default();
        for &e in &all {
            match &ast.expr(e).kind {
                ExprKind::NonNull(x) => {
                    through.insert(*x);
                }
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => {
                    through.insert(*t);
                }
                // (read as the element type: as `xs[i]!`)
                ExprKind::Index { optional: false, .. } if !self.c.types.has_undefined(facts.expr_ty[e as usize]) => {
                    through.insert(e);
                }
                _ => {}
            }
        }
        for &e in &all {
            match &ast.expr(e).kind {
                ExprKind::Index { obj, .. } if !through.contains(&e) => {
                    if let Some(xk) = local(*obj) {
                        uses.entry(xk).or_insert((Vec::new(), true)).1 = false;
                    }
                }
                _ => {}
            }
        }
        for &e in &all {
            match &ast.expr(e).kind {
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } if local(*t) == Some(ik) && !skip.contains(&e) => return None,
                ExprKind::Call { args, .. } if args.iter().any(|a| a.by_ref.is_some() && local(a.expr) == Some(ik)) => return None,
                ExprKind::Index { obj, index, .. } => {
                    if let Some(xk) = local(*obj) {
                        fine.insert(*obj);
                        let entry = uses.entry(xk).or_insert((Vec::new(), true));
                        if let Some(k) = self.index_offset(*index, ik) {
                            entry.0.push(k);
                        }
                    }
                }
                ExprKind::Member { obj, name, .. } if self.sym(*name) == "length" => {
                    if local(*obj).is_some() {
                        fine.insert(*obj);
                    }
                }
                _ => {}
            }
        }
        for &e in &all {
            if let Some(k) = local(e)
                && !fine.contains(&e)
                && let Some(u) = uses.get_mut(&k)
            {
                u.1 = false;
            }
        }
        // `n`: a literal, a local nothing in the loop assigns, or the length of an array whose
        // length nothing in it changes
        let stable = |k: u32| -> bool {
            !boxed.contains(&k)
                && !all.iter().any(|&e| match &ast.expr(e).kind {
                    ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => local(*t) == Some(k),
                    ExprKind::Call { args, .. } => args.iter().any(|a| a.by_ref.is_some() && local(a.expr) == Some(k)),
                    _ => false,
                })
        };
        // (or a sum of those: `i < start + len`)
        fn sum_of(ast: &ast::Ast, e: ExprId, leaf: &dyn Fn(ExprId) -> bool) -> bool {
            match &ast.expr(e).kind {
                ExprKind::Int(_) => true,
                ExprKind::Paren(x) => sum_of(ast, *x, leaf),
                ExprKind::Binary(BinOp::Add | BinOp::Sub, a, b) => sum_of(ast, *a, leaf) && sum_of(ast, *b, leaf),
                _ => leaf(e),
            }
        }
        match &ast.expr(hi).kind {
            ExprKind::Int(_) => {}
            ExprKind::Ident(_) if local(hi).is_some_and(stable) => {}
            ExprKind::Binary(BinOp::Add | BinOp::Sub, ..) if sum_of(ast, hi, &|e| matches!(ast.expr(e).kind, ExprKind::Ident(_)) && local(e).is_some_and(stable) && facts.expr_ty.get(e as usize) == Some(&INT)) => {}
            ExprKind::Member { obj, name, optional: false, .. } if self.sym(*name) == "length" => {
                let xk = local(*obj)?;
                if !matches!(self.tget(facts.expr_ty[*obj as usize]), Ty::Array(_)) || boxed.contains(&xk) || uses.get(&xk).is_some_and(|u| !u.1) {
                    return None;
                }
                // (and nothing but the loop's own reads of it outside `xs[...]` and `.length`)
                if all.iter().any(|&e| local(e) == Some(xk) && !fine.contains(&e)) {
                    return None;
                }
            }
            _ => return None,
        }
        // (an array the loop writes must be uniquely owned through it, made so before it: a
        // write to a shared one copies it, moving its elements)
        let mut written: FxSet<u32> = FxSet::default();
        for &e in &all {
            if let ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } = &ast.expr(e).kind {
                let mut cur = *t;
                loop {
                    match &ast.expr(cur).kind {
                        ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Member { obj: x, .. } | ExprKind::Index { obj: x, .. } => cur = *x,
                        _ => break,
                    }
                }
                if let Some(k) = local(cur) {
                    written.insert(k);
                }
            }
        }
        let mut arrays = Vec::new();
        let mut extra = Vec::new();
        for (xk, (ks, only)) in uses {
            if !only || boxed.contains(&xk) || !stable(xk) || (written.contains(&xk) && !unique.contains(&xk)) {
                continue;
            }
            let Some(&lt) = types.get(&xk) else { continue };
            if !matches!(self.tget(lt), Ty::Array(_)) {
                continue;
            }
            if ks.is_empty() {
                if !written.contains(&xk) {
                    extra.push(xk);
                }
                continue;
            }
            let (lo, hi) = (*ks.iter().min().unwrap(), *ks.iter().max().unwrap());
            arrays.push((xk, lo, hi));
        }
        if arrays.is_empty() {
            return None;
        }
        arrays.sort();
        extra.sort();
        // (nothing in the loop may resize them: they may be shared)
        let own: Vec<u32> = arrays.iter().map(|a| a.0).collect();
        if self.may_resize(m, &all, &own) {
            return None;
        }
        // (the versioned arrays' writes are in bounds: they append to none)
        let with_own: Vec<u32> = own.iter().chain(&extra).copied().collect();
        if !extra.is_empty() && self.may_resize(m, &all, &with_own) {
            extra.clear();
        }
        Some(VersionPlan { ik, le, hi, arrays, extra })
    }

    /// `while (i < j) { ...; i++; ...; j--; ... }` (one `i++` and one `j--` among the body's own
    /// statements, nothing else changing `i` or `j`) indexing arrays at `i` and `j`, whose lengths
    /// nothing in it changes: `i` only rises and `j` only falls while `i < j`, so `i >= 0` and
    /// `j < xs.length` before it puts every such index in range, and neither step can overflow.
    /// (`i`, `j`, the arrays), for a copy of the loop without those checks.
    fn two_pointer_plan(&mut self, cond: ExprId, body: StmtId) -> Option<(u32, u32, Vec<u32>)> {
        let outer: Vec<u32> = self.b().versioned.iter().flat_map(|v| v.1.iter().map(|a| a.0)).collect();
        let boxed = self.b().boxed.clone();
        let unique = self.b().unique.clone();
        let types: FxMap<u32, TyId> = self.b().locals.iter().map(|(k, l)| (*k, l.ty)).collect();
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let local = |e: ExprId| match (&ast.expr(e).kind, facts.idents.get(&e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
            _ => None,
        };
        let ExprKind::Binary(BinOp::Lt, l, r) = &ast.expr(cond).kind else { return None };
        let (up, down) = (local(*l)?, local(*r)?);
        if up == down || boxed.contains(&up) || boxed.contains(&down) || types.get(&up) != Some(&INT) || types.get(&down) != Some(&INT) {
            return None;
        }
        let StmtKind::Block(ss) = &ast.stmt(body).kind else { return None };
        // the steps: statements of their own
        let step = |s: StmtId, k: u32, inc: bool| match &ast.stmt(s).kind {
            StmtKind::Expr(e) => match &ast.expr(*e).kind {
                ExprKind::Update { target, inc: i, .. } => local(*target) == Some(k) && *i == inc,
                ExprKind::Assign(AssignOp::Op(op), t, v) => local(*t) == Some(k) && *op == if inc { BinOp::Add } else { BinOp::Sub } && matches!(ast.expr(*v).kind, ExprKind::Int(1)),
                _ => false,
            },
            _ => false,
        };
        let steps: Vec<StmtId> = ss.iter().copied().filter(|&s| step(s, up, true) || step(s, down, false)).collect();
        if ss.iter().filter(|&&s| step(s, up, true)).count() != 1 || ss.iter().filter(|&&s| step(s, down, false)).count() != 1 {
            return None;
        }
        fn has_loop(ast: &ast::Ast, s: StmtId) -> bool {
            match &ast.stmt(s).kind {
                StmtKind::While(..) | StmtKind::DoWhile(..) | StmtKind::For { .. } | StmtKind::ForOf { .. } => true,
                StmtKind::If(_, t, e) => has_loop(ast, *t) || e.is_some_and(|e| has_loop(ast, e)),
                StmtKind::Block(xs) => xs.iter().any(|&x| has_loop(ast, x)),
                StmtKind::Switch(_, cases) => cases.iter().any(|c| c.body.iter().any(|&x| has_loop(ast, x))),
                StmtKind::Try { body, catch, finally } => has_loop(ast, *body) || catch.as_ref().is_some_and(|c| has_loop(ast, c.body)) || finally.is_some_and(|f| has_loop(ast, f)),
                _ => false,
            }
        }
        if has_loop(ast, body) {
            return None;
        }
        let mut all = Vec::new();
        for &s in ss.iter().filter(|s| !steps.contains(s)) {
            collect_exprs_stmt(ast, s, &mut all);
        }
        if all.len() > 400 || all.iter().any(|&e| matches!(ast.expr(e).kind, ExprKind::Await(_))) {
            return None;
        }
        // nothing else changes `i` or `j`
        for &e in &all {
            match &ast.expr(e).kind {
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } if local(*t) == Some(up) || local(*t) == Some(down) => return None,
                ExprKind::Call { args, .. } if args.iter().any(|a| a.by_ref.is_some() && (local(a.expr) == Some(up) || local(a.expr) == Some(down))) => return None,
                _ => {}
            }
        }
        // the arrays indexed at `i` or `j`, used only through indexing and `.length`
        let mut fine: FxSet<ExprId> = FxSet::default();
        let mut indexed: Vec<u32> = Vec::new();
        let mut written: FxSet<u32> = FxSet::default();
        for &e in &all {
            match &ast.expr(e).kind {
                ExprKind::Index { obj, index, .. } => {
                    if let Some(xk) = local(*obj) {
                        fine.insert(*obj);
                        if (local(*index) == Some(up) || local(*index) == Some(down)) && !indexed.contains(&xk) {
                            indexed.push(xk);
                        }
                    }
                }
                ExprKind::Member { obj, name, .. } if self.sym(*name) == "length" && local(*obj).is_some() => {
                    fine.insert(*obj);
                }
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => {
                    let mut cur = *t;
                    while let ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Member { obj: x, .. } | ExprKind::Index { obj: x, .. } = &ast.expr(cur).kind {
                        cur = *x;
                    }
                    if let Some(k) = local(cur) {
                        written.insert(k);
                    }
                }
                _ => {}
            }
        }
        let arrays: Vec<u32> = indexed
            .into_iter()
            .filter(|&xk| {
                // (an enclosing versioned loop's pointer reaches it already)
                !outer.contains(&xk)
                    && matches!(types.get(&xk).map(|&t| self.c.types.get(t)), Some(Ty::Array(_)))
                    && !boxed.contains(&xk)
                    && (!written.contains(&xk) || unique.contains(&xk))
                    && !all.iter().any(|&e| local(e) == Some(xk) && !fine.contains(&e))
            })
            .collect();
        if arrays.is_empty() || self.may_resize(m, &all, &arrays) {
            return None;
        }
        Some((up, down, arrays))
    }

    /// `k` when `e` is `i`, `i + k`, `k + i` or `i - k` (for the local `i`, `k` a small literal).
    fn index_offset(&self, e: ExprId, ik: u32) -> Option<i64> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let is_i = |x: ExprId| matches!((&ast.expr(x).kind, self.ident_fact(m, x)), (ExprKind::Ident(_), Some(IdentFact::Local(k))) if k == ik);
        let lit = |x: ExprId| match ast.expr(x).kind {
            ExprKind::Int(v) if v < 1 << 20 => Some(v as i64),
            _ => None,
        };
        match &ast.expr(e).kind {
            ExprKind::Paren(x) => self.index_offset(*x, ik),
            ExprKind::Ident(_) if is_i(e) => Some(0),
            ExprKind::Binary(BinOp::Add, a, b) if is_i(*a) => lit(*b),
            ExprKind::Binary(BinOp::Add, a, b) if is_i(*b) => lit(*a),
            ExprKind::Binary(BinOp::Sub, a, b) if is_i(*a) => lit(*b).map(|k| -k),
            _ => None,
        }
    }

    /// The test that every `xs[i + k]` of `plan` is in range: `i + min k >= 0` now, and
    /// `n + max k (+ 1 for <=) <= xs.length` (no overflow: lengths and `k` are small).
    fn version_check(&mut self, plan: &VersionPlan) -> String {
        let i = self.b().locals[&plan.ik].access.clone();
        self.push_temps();
        let n = self.expr(plan.hi);
        let n = self.int_code(n);
        self.pop_temps();
        let lo = plan.arrays.iter().map(|a| a.1).min().unwrap_or(0);
        let mut parts = vec![format!("{i} >= {}", -lo)];
        for &(xk, _, hi) in &plan.arrays {
            let a = self.b().locals[&xk].access.clone();
            parts.push(format!("{n} <= tv_arr_len({a}) - {}", hi + plan.le as i64));
        }
        parts.join(" && ")
    }

    /// Some expression in `exprs` indexes local `k`.
    fn indexes(&self, exprs: &[ExprId], k: u32) -> bool {
        let m = self.cur_m();
        let ast = self.ast(m);
        exprs.iter().any(|&e| matches!(&ast.expr(e).kind, ExprKind::Index { obj, .. } if matches!(self.ident_fact(m, *obj), Some(IdentFact::Local(x)) if x == k)))
    }

    /// `xs[i] = v` on an array a loop keeps in locals (and no enclosing versioned loop holds:
    /// its writes there are in bounds).
    fn cached_array(&self, target: ExprId) -> bool {
        let m = self.cur_m();
        let Some(b) = self.bodies.last() else { return false };
        match self.ast(m).expr(target).kind {
            ExprKind::Index { obj, .. } => matches!(self.ident_fact(m, obj), Some(IdentFact::Local(k)) if b.arr_cache.contains_key(&k) && !b.versioned.iter().any(|v| v.1.iter().any(|a| a.0 == k))),
            _ => false,
        }
    }

    /// `xs[index]` on an array a loop keeps in locals (see `arr_cache`): the element's C lvalue,
    /// checked against the kept length.
    fn cached_index(&mut self, obj: ExprId, index: ExprId, span: Span) -> Option<String> {
        let m = self.cur_m();
        if !matches!(self.ast(m).expr(obj).kind, ExprKind::Ident(_)) {
            return None;
        }
        let Some(IdentFact::Local(xk)) = self.ident_fact(m, obj) else { return None };
        let (d, n) = self.b().arr_cache.get(&xk).cloned()?;
        let i = self.expr(index);
        let i = self.int_code(i);
        let loc = self.loc(span);
        Some(format!("({d}[TV_IDX({i}, {n}, {loc})])"))
    }

    /// `xs[index]` in a versioned loop's unchecked copy: the element's C lvalue, through the
    /// local holding the array's elements (every access to it, so the C compiler sees one base),
    /// unchecked at `i + k` and checked against the length elsewhere.
    /// A counted loop stepping `i` by one whose body stores `xs[i] = ...` once a round, at its top
    /// level, reading `xs[i - 1]` only up to there (`curr[i] = min(curr[i - 1] + 1, ...)`): the
    /// value stored can be carried to the next round in a local instead of read back from memory
    /// (GCC's predictive commoning, which clang doesn't do: a store-to-load wait each round on
    /// x86). No `continue` (the store would be skipped), no other element stores (they might be
    /// to the same array), no calls but `Math`'s. (array, the storing statement)
    fn carry_plan(&mut self, body: StmtId, step: Option<ExprId>, ik: u32) -> Option<(u32, StmtId)> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let step = step?;
        let local = |g: &Self, e: ExprId| match (&ast.expr(e).kind, g.facts(m).idents.get(&e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
            _ => None,
        };
        let steps_by_one = match &ast.expr(step).kind {
            ExprKind::Update { inc: true, target, .. } => local(self, *target) == Some(ik),
            ExprKind::Assign(AssignOp::Op(crate::ast::BinOp::Add), t, v) => local(self, *t) == Some(ik) && matches!(ast.expr(*v).kind, ExprKind::Int(1)),
            _ => false,
        };
        if !steps_by_one {
            return None;
        }
        let StmtKind::Block(ss) = &ast.stmt(body).kind else { return None };
        let ss = ss.clone();
        // (no `continue` anywhere in it)
        let mut stack = vec![body];
        while let Some(st) = stack.pop() {
            match &ast.stmt(st).kind {
                StmtKind::Continue(_) => return None,
                StmtKind::Block(xs) => stack.extend(xs.iter().copied()),
                StmtKind::If(_, t, e) => {
                    stack.push(*t);
                    stack.extend(e.iter().copied());
                }
                StmtKind::While(_, b) | StmtKind::DoWhile(b, _) | StmtKind::ForOf { body: b, .. } | StmtKind::For { body: b, .. } => stack.push(*b),
                StmtKind::Switch(..) | StmtKind::Try { .. } => return None,
                _ => {}
            }
        }
        // the store
        let (j, xk) = ss.iter().enumerate().find_map(|(j, &st)| {
            let StmtKind::Expr(e) = &ast.stmt(st).kind else { return None };
            let ExprKind::Assign(AssignOp::Assign, t, _) = &ast.expr(*e).kind else { return None };
            let ExprKind::Index { obj, index, optional: false } = &ast.expr(*t).kind else { return None };
            (local(self, *index) == Some(ik)).then_some(())?;
            local(self, *obj).map(|k| (j, k))
        })?;
        let xt = self.b().locals.get(&xk)?.ty;
        let Ty::Array(et) = self.tget(xt) else { return None };
        if !matches!(self.tget(et), Ty::Int | Ty::F64 | Ty::F32 | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64 | Ty::Bool) {
            return None;
        }
        let mut all = Vec::new();
        collect_exprs_stmt(ast, body, &mut all);
        let store_target = match &ast.stmt(ss[j]).kind {
            StmtKind::Expr(e) => match &ast.expr(*e).kind {
                ExprKind::Assign(_, t, _) => *t,
                _ => return None,
            },
            _ => return None,
        };
        for &e in &all {
            match &ast.expr(e).kind {
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } if *t != store_target && matches!(ast.expr(*t).kind, ExprKind::Index { .. }) => return None,
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } if local(self, *t) == Some(xk) || local(self, *t) == Some(ik) => return None,
                ExprKind::Call { callee, .. } => {
                    let math = matches!(&ast.expr(*callee).kind, ExprKind::Member { obj, .. } if matches!(ast.expr(*obj).kind, ExprKind::Ident(n) if self.sym(n) == "Math"));
                    if !math {
                        return None;
                    }
                }
                _ => {}
            }
        }
        // (`xs[i - 1]` read only up to the store)
        let mut after = Vec::new();
        for &st in &ss[j + 1..] {
            collect_exprs_stmt(ast, st, &mut after);
        }
        for &e in &after {
            if let ExprKind::Index { obj, index, .. } = &ast.expr(e).kind
                && local(self, *obj) == Some(xk)
                && self.index_offset(*index, ik) != Some(0)
            {
                return None;
            }
        }
        Some((xk, ss[j]))
    }

    /// A `string[]` local made empty (`const parts: string[] = []`) and then only pushed one
    /// string at a time onto (`parts.push(s)`), joined with one literal separator
    /// (`parts.join("")`) and counted (`parts.length`), by its function itself (no closure, no
    /// other use): a string builder holds the joined text instead of an array of strings, the
    /// separator written as each string is pushed after the first. Its separator, or None.
    /// Whether JavaScript-valued local `key` lives only in its function's stack frame: the
    /// function isn't async or a generator (their frames are on the heap), no closure uses the
    /// local, and every use reads a property of it, calls a method of it, passes it as an
    /// argument (a callee that keeps it roots it itself), compares it, or tests it.
    pub(crate) fn js_stack_local(&mut self, key: u32) -> bool {
        if self.b().async_fn.is_some() || self.b().gen_elem.is_some() {
            return false;
        }
        let m = self.cur_m();
        let Some(root) = self.b().root.or(self.b().arrow_root) else { return false };
        let ast = self.ast(m);
        let mut all = Vec::new();
        collect_exprs_stmt(ast, root, &mut all);
        let facts = self.facts(m);
        let is_k = |e: ExprId| matches!(ast.expr(e).kind, ExprKind::Ident(_)) && matches!(facts.idents.get(&e), Some(IdentFact::Local(k)) if *k == key);
        let mut in_arrows: FxSet<ExprId> = FxSet::default();
        let mut allowed: FxSet<ExprId> = FxSet::default();
        for &e in &all {
            match &ast.expr(e).kind {
                ExprKind::Arrow(f) => {
                    let mut xs = Vec::new();
                    match &f.body {
                        ArrowBody::Expr(x) => collect_exprs_expr(ast, *x, &mut xs),
                        ArrowBody::Block(b) => collect_exprs_stmt(ast, *b, &mut xs),
                    }
                    in_arrows.extend(xs);
                }
                ExprKind::Member { obj, .. } | ExprKind::Index { obj, .. } if is_k(*obj) => {
                    allowed.insert(*obj);
                }
                ExprKind::Call { args, .. } | ExprKind::New { args, .. } => {
                    for a in args {
                        if is_k(a.expr) && a.by_ref.is_none() {
                            allowed.insert(a.expr);
                        }
                    }
                }
                ExprKind::Binary(op, l, r) if matches!(op, BinOp::Eq | BinOp::Ne | BinOp::LooseEq | BinOp::LooseNe) => {
                    for x in [*l, *r] {
                        if is_k(x) {
                            allowed.insert(x);
                        }
                    }
                }
                ExprKind::Unary(UnOp::Not, x) | ExprKind::Typeof(x) if is_k(*x) => {
                    allowed.insert(*x);
                }
                _ => {}
            }
        }
        // (a member written, `r.x = v`: still a read of r; r itself assigned: not this)
        for &e in &all {
            if let ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } = &ast.expr(e).kind
                && is_k(*t)
            {
                return false;
            }
        }
        // (conditions: `if (r)`, `while (r)`)
        let mut conds: FxSet<ExprId> = FxSet::default();
        let mut stmts = vec![root];
        while let Some(st) = stmts.pop() {
            match &ast.stmt(st).kind {
                StmtKind::Block(xs) => stmts.extend(xs.iter().copied()),
                StmtKind::If(c, t, e) => {
                    conds.insert(*c);
                    stmts.push(*t);
                    stmts.extend(e.iter().copied());
                }
                StmtKind::While(c, b) | StmtKind::DoWhile(b, c) => {
                    conds.insert(*c);
                    stmts.push(*b);
                }
                StmtKind::For { init, body, .. } => {
                    stmts.extend(init.iter().copied());
                    stmts.push(*body);
                }
                StmtKind::ForOf { body, .. } => stmts.push(*body),
                StmtKind::Try { .. } | StmtKind::Switch(..) => return false,
                _ => {}
            }
        }
        all.iter().all(|&e| !is_k(e) || ((allowed.contains(&e) || conds.contains(&e)) && !in_arrows.contains(&e)))
    }

    pub(crate) fn builder_local(&mut self, key: u32, init: ExprId, ty: TyId) -> Option<String> {
        let m = self.cur_m();
        let Ty::Array(et) = self.tget(ty) else { return None };
        if !self.c.types.is_string(et) || matches!(self.tget(et), Ty::StrLit(_)) {
            return None;
        }
        let ast = self.ast(m);
        if !matches!(&ast.expr(init).kind, ExprKind::Array(xs) if xs.is_empty()) {
            return None;
        }
        let root = self.b().root.or(self.b().arrow_root)?;
        let mut all = Vec::new();
        collect_exprs_stmt(ast, root, &mut all);
        // (uses inside closures: not this function's alone)
        let mut in_arrows: FxSet<ExprId> = FxSet::default();
        for &e in &all {
            if let ExprKind::Arrow(f) = &ast.expr(e).kind {
                let mut xs = Vec::new();
                match &f.body {
                    ArrowBody::Expr(x) => collect_exprs_expr(ast, *x, &mut xs),
                    ArrowBody::Block(b) => collect_exprs_stmt(ast, *b, &mut xs),
                }
                in_arrows.extend(xs);
            }
        }
        let facts = self.facts(m);
        let is_k = |e: ExprId| matches!(ast.expr(e).kind, ExprKind::Ident(_)) && matches!(facts.idents.get(&e), Some(IdentFact::Local(k)) if *k == key);
        let mut allowed: FxSet<ExprId> = FxSet::default();
        let mut sep: Option<String> = None;
        let mut targets: FxSet<ExprId> = FxSet::default();
        for &e in &all {
            match &ast.expr(e).kind {
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => {
                    targets.insert(*t);
                }
                _ => {}
            }
        }
        for &e in &all {
            match &ast.expr(e).kind {
                ExprKind::Call { callee, args, optional: false, .. } => {
                    let ExprKind::Member { obj, name, optional: false, .. } = &ast.expr(*callee).kind else { continue };
                    if !is_k(*obj) {
                        continue;
                    }
                    match self.c.interner.get(*name) {
                        "push" if args.len() == 1 && args[0].by_ref.is_none() && !matches!(ast.expr(args[0].expr).kind, ExprKind::Spread(_)) => {}
                        "join" if args.len() <= 1 => {
                            let s = match args.first().map(|a| &ast.expr(a.expr).kind) {
                                None => ",".to_string(),
                                Some(ExprKind::Str(x)) => self.c.interner.get(*x).to_string(),
                                _ => return None,
                            };
                            if sep.as_ref().is_some_and(|p| *p != s) {
                                return None;
                            }
                            sep = Some(s);
                        }
                        _ => return None,
                    }
                    allowed.insert(*obj);
                }
                ExprKind::Member { obj, name, .. } if is_k(*obj) && self.c.interner.get(*name) == "length" && !targets.contains(&e) => {
                    allowed.insert(*obj);
                }
                _ => {}
            }
        }
        for &e in &all {
            if is_k(e) && (!allowed.contains(&e) || in_arrows.contains(&e)) {
                return None;
            }
        }
        Some(sep.unwrap_or_else(|| ",".into()))
    }

    fn versioned_index(&mut self, obj: ExprId, index: ExprId, span: Span) -> Option<String> {
        let m = self.cur_m();
        let Some(IdentFact::Local(xk)) = self.ident_fact(m, obj) else { return None };
        let versioned = self.b().versioned.clone();
        let (iks, (_, d, n)) = versioned.iter().rev().find_map(|(iks, arrays)| arrays.iter().find(|a| a.0 == xk).map(|a| (iks.clone(), a.clone())))?;
        for ik in iks {
            if let Some(k) = self.index_offset(index, ik) {
                let i = self.b().locals.get(&ik)?.access.clone();
                return Some(format!("({d}[{i} + {k}])"));
            }
        }
        let i = self.expr(index);
        let i = self.int_code(i);
        let loc = self.loc(span);
        Some(format!("({d}[TV_IDX({i}, {n}, {loc})])"))
    }

    /// `for (let i = a; i < n; i++)` where `n` has `i`'s integer type and nothing in the loop
    /// assigns `i` but the step: every step starts from `i < n <= MAX`, so `i++` can't overflow.
    /// The array locals of the current function (`const`s, or `let`s never reassigned) whose
    /// length is a constant `N` the whole time: initialized with a literal of N elements (or a call to a function that always
    /// returns one), in a function where nothing may resize an array (see `resize`). Told to the
    /// C compiler before each loop, it can unroll by them (as it would with a fixed-size array).
    fn fixed_lens(&mut self) -> FxMap<u32, (u64, u32)> {
        if let Some(f) = &self.b().fixed {
            return f.clone();
        }
        let m = self.cur_m();
        let mut out = FxMap::default();
        // (the loops that fill arrays: their pushes don't count against them)
        let mut fills: FxSet<ExprId> = FxSet::default();
        if let Some(root) = self.b().root {
            let ast = self.ast(m);
            let span = ast.stmt(root).span;
            let inside = |sp: Span| sp.start >= span.start && sp.end <= span.end && sp.file == span.file;
            // (a `let` the function never reassigns is as fixed as a `const`)
            let reassigned = self.reassigned_locals(&FnBodyKind::Block(root));
            let boxed = self.b().boxed.clone();
            let facts = self.facts(m);
            let local = |e: ExprId| match (&ast.expr(e).kind, facts.idents.get(&e)) {
                (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
                _ => None,
            };
            // Arrays filled by a loop that pushes once a round onto each of them, from empty, a
            // fixed number of times (`for (let i = 0; i < n; i++) { a.push(0); b.push(i) }`): that
            // many long after it. (local, (length, the loop's end); the loop's expressions)
            let empty: FxSet<u32> = ast
                .stmts
                .iter()
                .filter(|st| inside(st.span))
                .filter_map(|st| match &st.kind {
                    StmtKind::Let { name_span, init: Some(i), .. } if matches!(&ast.expr(*i).kind, ExprKind::Array(xs) if xs.is_empty()) => Some(name_span.start),
                    _ => None,
                })
                .collect();
            let mut loops: Vec<(Vec<u32>, ExprId, u32, StmtId)> = Vec::new();
            for st in ast.stmts.iter().filter(|st| inside(st.span)) {
                let StmtKind::For { init: Some(init), cond: Some(cond), step: Some(step), body } = &st.kind else { continue };
                let StmtKind::Let { name_span, init: Some(i0), .. } = &ast.stmt(*init).kind else { continue };
                if !matches!(ast.expr(*i0).kind, ExprKind::Int(0)) {
                    continue;
                }
                let ik = name_span.start;
                let ExprKind::Binary(BinOp::Lt, l, r) = &ast.expr(*cond).kind else { continue };
                if local(*l) != Some(ik) || !matches!(&ast.expr(*step).kind, ExprKind::Update { target, inc: true, .. } if local(*target) == Some(ik)) {
                    continue;
                }
                let StmtKind::Block(ss) = &ast.stmt(*body).kind else { continue };
                // (the body: only pushes, of values that call nothing, onto distinct arrays)
                let mut arrays = Vec::new();
                let all_pushes = ss.iter().all(|&s| {
                    let StmtKind::Expr(e) = &ast.stmt(s).kind else { return false };
                    let ExprKind::Call { callee, args, .. } = &ast.expr(*e).kind else { return false };
                    let ExprKind::Member { obj, name, .. } = &ast.expr(*callee).kind else { return false };
                    let Some(k) = local(*obj) else { return false };
                    if self.sym(*name) != "push" || args.len() != 1 || super::asyncfn::has_call(ast, args[0].expr) || matches!(ast.expr(args[0].expr).kind, ExprKind::Spread(_) | ExprKind::Assign(..) | ExprKind::Update { .. }) || arrays.contains(&k) {
                        return false;
                    }
                    arrays.push(k);
                    true
                });
                if !all_pushes || arrays.is_empty() || arrays.iter().any(|k| !empty.contains(k) || reassigned.contains(k) || boxed.contains(k)) {
                    continue;
                }
                loops.push((arrays, *r, st.span.end, *body));
            }
            // (the count: a literal, or a value range analysis knows exactly)
            let mut filled: Vec<(u32, u64, u32)> = Vec::new();
            let mut skip: FxSet<ExprId> = FxSet::default();
            for (arrays, r, end, body) in loops {
                let n = match self.int_range(r) {
                    super::range::R::In(lo, hi) if lo == hi && lo >= 0 => lo as u64,
                    _ => continue,
                };
                for &k in &arrays {
                    filled.push((k, n, end));
                }
                let mut es = Vec::new();
                collect_exprs_stmt(self.ast(m), body, &mut es);
                skip.extend(es);
            }
            let ast = self.ast(m);
            let mut exprs = Vec::new();
            collect_exprs_stmt(ast, root, &mut exprs);
            exprs.retain(|e| !skip.contains(e));
            fills = skip;
            if !self.may_resize(m, &exprs, &[]) {
                for (k, n, from) in filled {
                    out.insert(k, (n, from));
                }
                for st in ast.stmts.iter() {
                    let StmtKind::Let { name_span, init: Some(i), .. } = &st.kind else { continue };
                    if reassigned.contains(&name_span.start) || self.b().boxed.contains(&name_span.start) || !inside(st.span) {
                        continue;
                    }
                    let n = match &ast.expr(*i).kind {
                        ExprKind::Array(xs) if !xs.is_empty() && !xs.iter().any(|&x| matches!(ast.expr(x).kind, ExprKind::Spread(_))) => Some(xs.len() as u64),
                        ExprKind::Call { .. } => match self.facts(m).calls.get(i).map(|f| f.callee.clone()) {
                            Some(Callee::Fn(fm, fi)) => self.fn_const_len(fm, fi),
                            _ => None,
                        },
                        _ => None,
                    };
                    if let Some(n) = n {
                        out.insert(name_span.start, (n, 0));
                    }
                }
            }
        }
        if let Some(root) = self.b().root {
            let m = self.cur_m();
            let mut exprs = Vec::new();
            collect_exprs_stmt(self.ast(m), root, &mut exprs);
            exprs.retain(|e| !fills.contains(e));
            // (`may_resize` lets a function resize the arrays it makes: these it mustn't, by
            // a push, a length, or a write past the end, which appends)
            let ast = self.ast(m);
            let facts = self.facts(m);
            let mut resized: Vec<(u32, Option<ExprId>)> = Vec::new();
            for &e in &exprs {
                match &ast.expr(e).kind {
                    ExprKind::Call { callee, .. } => {
                        if let ExprKind::Member { obj, name, .. } = &ast.expr(*callee).kind
                            && super::resize::RESIZING.contains(&self.sym(*name))
                            && let (ExprKind::Ident(_), Some(IdentFact::Local(k))) = (&ast.expr(*obj).kind, facts.idents.get(obj))
                        {
                            resized.push((*k, None));
                        }
                    }
                    ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => {
                        let plain = matches!(ast.expr(e).kind, ExprKind::Assign(AssignOp::Assign, ..));
                        let (obj, index) = match &ast.expr(*t).kind {
                            ExprKind::Member { obj, name, .. } if self.sym(*name) == "length" => (*obj, None),
                            ExprKind::Index { obj, index, .. } if plain => (*obj, Some(*index)),
                            _ => continue,
                        };
                        if let (ExprKind::Ident(_), Some(IdentFact::Local(k))) = (&ast.expr(obj).kind, facts.idents.get(&obj)) {
                            resized.push((*k, index));
                        }
                    }
                    _ => {}
                }
            }
            for (k, index) in resized {
                let Some(&(n, _)) = out.get(&k) else { continue };
                let inside = index.is_some_and(|i| matches!(self.int_range(i), super::range::R::In(lo, hi) if lo >= 0 && hi < n as i128));
                if !inside {
                    out.remove(&k);
                }
            }
        }
        self.b().fixed = Some(out.clone());
        out
    }

    /// Function (m, i) always returns an array literal of the same length.
    fn fn_const_len(&mut self, m: u32, i: u32) -> Option<u64> {
        let ast = self.ast(m);
        let ast::ItemKind::Function(f) = &ast.items[i as usize].kind else { return None };
        let span = ast.stmt(f.body).span;
        let mut n = None;
        for st in ast.stmts.iter() {
            if !(st.span.start >= span.start && st.span.end <= span.end && st.span.file == span.file) {
                continue;
            }
            let StmtKind::Return(r) = &st.kind else { continue };
            let Some(e) = r else { return None };
            let ExprKind::Array(xs) = &ast.expr(*e).kind else { return None };
            if xs.iter().any(|&x| matches!(ast.expr(x).kind, ExprKind::Spread(_))) || n.is_some_and(|k| k != xs.len() as u64) {
                return None;
            }
            n = Some(xs.len() as u64);
        }
        n
    }

    /// Parameter `pi` of module function (m, i) is assigned as a whole in its body (or passed on
    /// as `inout`): an `inout` argument may come back another value.
    fn param_reassigned(&self, m: u32, i: u32, pi: usize) -> bool {
        let ast = self.ast(m);
        let ast::ItemKind::Function(f) = &ast.items[i as usize].kind else { return true };
        let Some(p) = f.params.get(pi) else { return true };
        let key = p.span.start;
        let facts = self.facts(m);
        let mut exprs = Vec::new();
        collect_exprs_stmt(ast, f.body, &mut exprs);
        let is_p = |e: ExprId| matches!((&ast.expr(e).kind, facts.idents.get(&e)), (ExprKind::Ident(_), Some(IdentFact::Local(k))) if *k == key);
        exprs.iter().any(|&e| match &ast.expr(e).kind {
            ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => is_p(*t),
            ExprKind::Call { args, .. } => args.iter().any(|a| a.by_ref.is_some() && is_p(a.expr)),
            _ => false,
        })
    }

    /// Locals assigned as a whole (`x = ...`, `x++`, `f(&x)`) in a body.
    fn reassigned_locals(&self, kind: &FnBodyKind) -> FxSet<u32> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let mut exprs = Vec::new();
        match kind {
            FnBodyKind::Block(s) => collect_exprs_stmt(ast, *s, &mut exprs),
            FnBodyKind::Expr(e) => collect_exprs_expr(ast, *e, &mut exprs),
            FnBodyKind::Init => {}
        }
        let local = |e: ExprId| match (&ast.expr(e).kind, facts.idents.get(&e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
            _ => None,
        };
        let mut out = FxSet::default();
        for e in exprs {
            match &ast.expr(e).kind {
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => out.extend(local(*t)),
                ExprKind::Call { args, .. } => {
                    // (`f(&x)` where f never replaces that parameter leaves x as it was)
                    let callee = match facts.calls.get(&e).map(|f| &f.callee) {
                        Some(Callee::Fn(fm, fi)) => Some((*fm, *fi)),
                        _ => None,
                    };
                    for (pi, a) in args.iter().enumerate() {
                        if a.by_ref.is_some()
                            && let Some(k) = local(a.expr)
                            && callee.is_none_or(|(fm, fi)| self.param_reassigned(fm, fi, pi))
                        {
                            out.insert(k);
                        }
                    }
                }
                _ => {}
            }
        }
        out
    }

    /// `while (i < n && ...) { ...; i++; ... }`: the one `i++` (a statement of the body's own,
    /// nothing else changing `i`) comes after `i < n` held, so it can't overflow.
    fn guarded_counter(&self, cond: ExprId, body: StmtId) -> Option<u32> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let local = |e: ExprId| match (&ast.expr(e).kind, facts.idents.get(&e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
            _ => None,
        };
        let mut first = cond;
        loop {
            match &ast.expr(first).kind {
                ExprKind::Paren(x) => first = *x,
                ExprKind::Binary(BinOp::And, l, _) => first = *l,
                _ => break,
            }
        }
        let ExprKind::Binary(BinOp::Lt, l, r) = &ast.expr(first).kind else { return None };
        let k = local(*l)?;
        if facts.expr_ty.get(*l as usize) != Some(&INT) || facts.expr_ty.get(*r as usize) != Some(&INT) {
            return None;
        }
        let top: Vec<StmtId> = match &ast.stmt(body).kind {
            StmtKind::Block(ss) => ss.clone(),
            _ => vec![body],
        };
        let is_step = |s: StmtId| matches!(&ast.stmt(s).kind, StmtKind::Expr(x) if matches!(&ast.expr(*x).kind, ExprKind::Update { inc: true, target, .. } if local(*target) == Some(k)));
        if top.iter().filter(|&&s| is_step(s)).count() != 1 {
            return None;
        }
        let mut all = Vec::new();
        collect_exprs_stmt(ast, body, &mut all);
        collect_exprs_expr(ast, cond, &mut all);
        let changes = all
            .iter()
            .filter(|&&e| match &ast.expr(e).kind {
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => local(*t) == Some(k),
                ExprKind::Call { args, .. } => args.iter().any(|a| a.by_ref.is_some() && local(a.expr) == Some(k)),
                _ => false,
            })
            .count();
        (changes == 1).then_some(k)
    }

    fn step_cannot_overflow(&self, init: Option<StmtId>, cond: Option<ExprId>, step: Option<ExprId>, body: StmtId) -> Option<u32> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let local = |e: ExprId| match (&ast.expr(e).kind, facts.idents.get(&e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
            _ => None,
        };
        let init = init?;
        let StmtKind::Let { name_span, .. } = &ast.stmt(init).kind else { return None };
        let ty = facts.bindings.get(&init).copied()?;
        if !self.c.types.is_int(ty) {
            return None;
        }
        let ik = name_span.start;
        let cond = cond?;
        let ExprKind::Binary(BinOp::Lt, l, r) = &ast.expr(cond).kind else { return None };
        if local(*l) != Some(ik) || facts.expr_ty.get(*r as usize).copied() != Some(ty) {
            return None;
        }
        let step = step?;
        let ExprKind::Update { target, inc: true, .. } = &ast.expr(step).kind else { return None };
        if local(*target) != Some(ik) {
            return None;
        }
        let mut all = Vec::new();
        collect_exprs_stmt(ast, body, &mut all);
        collect_exprs_expr(ast, cond, &mut all);
        let changes_i = all.iter().any(|&e| match &ast.expr(e).kind {
            ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => local(*t) == Some(ik),
            ExprKind::Call { args, .. } => args.iter().any(|a| a.by_ref.is_some() && local(a.expr) == Some(ik)),
            _ => false,
        });
        (!changes_i).then_some(ik)
    }

    /// `for (let i = a; i < n; i++) { ...; xs.push(v); ... }` whose body always runs to the end
    /// (no break, continue, return, throw or `try`) and pushes onto `xs` as a statement of its
    /// own: the loop adds at least `n - i` elements, so the array grows to that size once, like
    /// a C program that knows its size up front.
    fn reserve_for_pushes(&mut self, init: Option<StmtId>, cond: Option<ExprId>, step: Option<ExprId>, body: StmtId) {
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let local = |e: ExprId| match (&ast.expr(e).kind, facts.idents.get(&e)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => Some(*k),
            _ => None,
        };
        let Some(init) = init else { return };
        let StmtKind::Let { name_span, .. } = &ast.stmt(init).kind else { return };
        if facts.bindings.get(&init).copied() != Some(INT) {
            return;
        }
        let ik = name_span.start;
        let Some(cond) = cond else { return };
        // (`i < n`, or `i <= n`: one more run)
        let (l, r, inclusive) = match &ast.expr(cond).kind {
            ExprKind::Binary(BinOp::Lt, l, r) => (l, r, false),
            ExprKind::Binary(BinOp::Le, l, r) => (l, r, true),
            _ => return,
        };
        if local(*l) != Some(ik) || facts.expr_ty.get(*r as usize).copied() != Some(INT) {
            return;
        }
        let bound = match &ast.expr(*r).kind {
            ExprKind::Int(_) => None,
            _ => match local(*r) {
                Some(k) => Some(k),
                None => return,
            },
        };
        let step_ok = match step.map(|s| &ast.expr(s).kind) {
            Some(ExprKind::Update { target, inc: true, .. }) => local(*target) == Some(ik),
            Some(ExprKind::Assign(AssignOp::Op(BinOp::Add), t, v)) => local(*t) == Some(ik) && matches!(ast.expr(*v).kind, ExprKind::Int(1)),
            _ => false,
        };
        if !step_ok {
            return;
        }
        let StmtKind::Block(ss) = &ast.stmt(body).kind else { return };
        // Every iteration reaches every top-level statement.
        fn exits(ast: &ast::Ast, s: StmtId) -> bool {
            match &ast.stmt(s).kind {
                StmtKind::Break(_) | StmtKind::Continue(_) | StmtKind::Return(_) | StmtKind::Throw(_) | StmtKind::Try { .. } => true,
                StmtKind::If(_, t, e) => exits(ast, *t) || e.is_some_and(|e| exits(ast, e)),
                StmtKind::While(_, b) | StmtKind::DoWhile(b, _) | StmtKind::For { body: b, .. } | StmtKind::ForOf { body: b, .. } => exits(ast, *b),
                StmtKind::Block(xs) => xs.iter().any(|&x| exits(ast, x)),
                StmtKind::Switch(..) => true,
                _ => false,
            }
        }
        if ss.iter().any(|&x| exits(ast, x)) {
            return;
        }
        let mut all = Vec::new();
        collect_exprs_stmt(ast, body, &mut all);
        if all.iter().any(|&e| match &ast.expr(e).kind {
            ExprKind::Try(_) => true,
            ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => local(*t) == Some(ik) || (bound.is_some() && local(*t) == bound),
            ExprKind::Call { args, .. } => args.iter().any(|a| a.by_ref.is_some() && (local(a.expr) == Some(ik) || (bound.is_some() && local(a.expr) == bound))),
            _ => false,
        }) {
            return;
        }
        let lit = match &ast.expr(*r).kind {
            ExprKind::Int(v) if inclusive => (*v < i64::MAX as u64).then(|| format!("INT64_C({})", v + 1)),
            ExprKind::Int(v) => Some(format!("INT64_C({v})")),
            _ => None,
        };
        // The array a statement pushes onto on every path through it: `xs.push(v)`, or an
        // if/else (chain) whose every branch does (e.g. one push per kind of element).
        fn always_pushes(g: &Gen, ast: &ast::Ast, local: &dyn Fn(ExprId) -> Option<u32>, s: StmtId) -> Option<u32> {
            match &ast.stmt(s).kind {
                StmtKind::Expr(e) => {
                    let ExprKind::Call { callee, args, .. } = &ast.expr(*e).kind else { return None };
                    let ExprKind::Member { obj, name, optional: false, .. } = &ast.expr(*callee).kind else { return None };
                    if g.sym(*name) != "push" || args.len() != 1 || matches!(ast.expr(args[0].expr).kind, ExprKind::Spread(_)) {
                        return None;
                    }
                    local(*obj)
                }
                StmtKind::If(_, t, Some(e)) => {
                    let a = always_pushes(g, ast, local, *t)?;
                    (always_pushes(g, ast, local, *e)? == a).then_some(a)
                }
                StmtKind::Block(xs) => xs.iter().find_map(|&x| always_pushes(g, ast, local, x)),
                _ => None,
            }
        }
        let index_written: FxSet<u32> = all
            .iter()
            .filter_map(|&e| match &ast.expr(e).kind {
                ExprKind::Assign(_, t, _) => match &ast.expr(*t).kind {
                    ExprKind::Index { obj, .. } => local(*obj),
                    _ => None,
                },
                _ => None,
            })
            .collect();
        let mut pushed: Vec<u32> = ss.iter().filter_map(|&x| always_pushes(self, ast, &local, x)).collect();
        pushed.dedup();
        let mut push_counts: FxMap<u32, usize> = FxMap::default();
        for &e in &all {
            if let ExprKind::Call { callee, .. } = &ast.expr(e).kind
                && let ExprKind::Member { obj, name, .. } = &ast.expr(*callee).kind
                && self.sym(*name) == "push"
                && let Some(k) = local(*obj)
            {
                *push_counts.entry(k).or_default() += 1;
            }
        }
        for xk in pushed {
            let Some(cv) = self.b().push_caps.get(&xk).cloned() else { continue };
            let (Some(xl), Some(il)) = (self.b().locals.get(&xk).cloned(), self.b().locals.get(&ik).cloned()) else { continue };
            let Ty::Array(et) = self.tget(xl.ty) else { continue };
            let n = match bound {
                Some(k) => match self.b().locals.get(&k) {
                    // (`i <= n` runs to `n + 1`; never past `INT64_MAX`)
                    Some(l) if inclusive => format!("({0} + ({0} < INT64_MAX))", l.access),
                    Some(l) => l.access.clone(),
                    None => continue,
                },
                None => match &lit {
                    Some(l) => l.clone(),
                    None => continue,
                },
            };
            let d = self.desc(et);
            let (xs, i) = (xl.access, il.access);
            // The loop runs exactly `n - i` times and pushes once per run when that push is the
            // only one onto `xs` in it and no heap closure can change `xs`, `i` or `n`: then the
            // reservation covers every push. (A count too large to add up can't be allocated.)
            let pushes = push_counts.get(&xk).copied().unwrap_or(0);
            // (and nothing else can push onto it meanwhile: it may be shared)
            let alone = !self.may_resize(m, &all, &[xk]);
            // (nor an index write: `xs[xs.length] = v` appends too)
            let indexed = index_written.contains(&xk);
            let b = self.b();
            let exact = alone && !indexed && pushes == 1 && !b.boxed.contains(&xk) && !b.boxed.contains(&ik) && bound.is_none_or(|k| !b.mutated.contains(&k) && !b.boxed.contains(&k));
            if exact {
                self.line(format!("if ({n} > {i}) {{ tv_int need_; if ((uint64_t){n} - (uint64_t){i} > (uint64_t)INT64_MAX || __builtin_add_overflow(tv_arr_len({xs}), (tv_int)((uint64_t){n} - (uint64_t){i}), &need_)) tv_trap(\"array too large\", NULL); if (need_ > {cv}) {{ {xs} = tvg_reserve({xs}, {d}, need_); {cv} = {xs}.p->cap; }} }}"));
                self.b().exact_pushes.insert(xk);
            } else {
                self.line(format!("if ({n} > {i} && tv_arr_len({xs}) + ({n} - {i}) > {cv}) {{ {xs} = tvg_reserve({xs}, {d}, tv_arr_len({xs}) + ({n} - {i})); {cv} = {xs}.p->cap; }}"));
            }
        }
    }

    fn unhoist(&mut self, keys: Vec<u32>) {
        for k in keys {
            self.b().unique.remove(&k);
            self.b().push_caps.remove(&k);
            self.b().arr_cache.remove(&k);
            self.b().exact_pushes.remove(&k);
        }
    }

    /// Is local `k` used in `all` only as `k.push(...)`, `k[...]` or `k.length`? (Then only
    /// pushes can reallocate it.)
    fn only_push_index_length(&self, all: &[ExprId], k: u32) -> bool {
        let m = self.cur_m();
        let ast = self.ast(m);
        let idents = &self.facts(m).idents;
        let is_k = |e: ExprId| matches!((&ast.expr(e).kind, idents.get(&e)), (ExprKind::Ident(_), Some(IdentFact::Local(x))) if *x == k);
        let mut callees = FxSet::default();
        for &e in all {
            match &ast.expr(e).kind {
                ExprKind::Call { callee, args, .. } => {
                    callees.insert(*callee);
                    if args.iter().any(|a| a.by_ref.is_some() && is_k(a.expr)) {
                        return false;
                    }
                }
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } if is_k(*t) => return false,
                _ => {}
            }
        }
        let mut ok_uses = FxSet::default();
        for &e in all {
            match &ast.expr(e).kind {
                ExprKind::Index { obj, .. } if is_k(*obj) => {
                    ok_uses.insert(*obj);
                }
                ExprKind::Member { obj, name, .. } if is_k(*obj) => {
                    let n = self.sym(*name);
                    if n == "length" || (n == "push" && callees.contains(&e)) {
                        ok_uses.insert(*obj);
                    } else {
                        return false;
                    }
                }
                _ => {}
            }
        }
        all.iter().all(|&e| !is_k(e) || ok_uses.contains(&e))
    }

    /// A place path (`p`, `p.a.b`) whose root is a read-only parameter, `this` or a local that
    /// never changes, and whose steps through class instances are `readonly` fields.
    fn stable_path(&mut self, e: ExprId) -> bool {
        let m = self.cur_m();
        let ast = self.ast(m);
        let mut cur = e;
        loop {
            match &ast.expr(cur).kind {
                // (`const r = word as Record`, `const n = node!`: the same object, seen as one)
                ExprKind::Paren(x) | ExprKind::As(x, _) | ExprKind::NonNull(x) => cur = *x,
                ExprKind::This => return true,
                ExprKind::Member { obj, name, optional: false, .. } => {
                    let ot = self.ty(*obj);
                    let ot = self.c.types.without_undefined(ot);
                    if self.c.class_of(ot).is_some() {
                        match self.c.class_member(ot, *name) {
                            Some(crate::check::class::ClassMemberRef::Field(f)) if f.readonly && !f.weak => {}
                            _ => return false,
                        }
                    }
                    cur = *obj
                }
                ExprKind::Ident(_) => {
                    return match self.ident_fact(m, cur) {
                        Some(IdentFact::Local(k)) => {
                            let b = self.bodies.last().unwrap();
                            (b.params.contains(&k) || b.locals.contains_key(&k)) && !b.mutated.contains(&k) && !b.boxed.contains(&k)
                        }
                        _ => false,
                    };
                }
                _ => return false,
            }
        }
    }

    /// `e` reads a path from `this`, a parameter or a local never assigned again (`xs[i]`,
    /// `this.items[k].next`), in a function that releases nothing (see `quiet_body`): what it
    /// reads stays alive as long as the function runs, so a `const` of it can borrow it.
    fn quiet_path(&mut self, e: ExprId) -> bool {
        let m = self.cur_m();
        let ast = self.ast(m);
        let mut cur = e;
        loop {
            match &ast.expr(cur).kind {
                ExprKind::Paren(x) | ExprKind::NonNull(x) => cur = *x,
                ExprKind::Member { obj, optional: false, .. } => cur = *obj,
                ExprKind::Index { obj, .. } => cur = *obj,
                ExprKind::This => break,
                ExprKind::Ident(_) => match self.ident_fact(m, cur) {
                    Some(IdentFact::Local(k)) => {
                        let b = self.bodies.last().unwrap();
                        if b.mutated.contains(&k) || b.boxed.contains(&k) {
                            return false;
                        }
                        break;
                    }
                    _ => return false,
                },
                _ => return false,
            }
        }
        let b = self.bodies.last().unwrap();
        if b.async_fn.is_some() || b.gen_elem.is_some() || b.ctor_class.is_some() {
            return false;
        }
        let Some(root) = b.root else { return false };
        self.quiet_body_pub(m, root)
    }

    pub(crate) fn cell_type(&mut self, ty: TyId) -> String {
        let ct = self.ctype(ty);
        let name = format!("C{}", ty.0);
        if self.helpers_cell_done(ty) {
            // (holding the value by value: defined after every value struct, as environments are)
            let _ = writeln!(self.typedefs, "typedef struct {name} {name};");
            let _ = writeln!(self.env_structs, "struct {name} {{ int64_t rc; {ct} v; }};");
        }
        name
    }

    fn helpers_cell_done(&mut self, ty: TyId) -> bool {
        self.helpers_done.insert((ty, 100))
    }

    fn cell_release(&mut self, ty: TyId, cell: &str) -> String {
        let inner = if self.is_rc(ty) { format!("{};", self.release_code(ty, &format!("{cell}->v"))) } else { String::new() };
        format!("if (--{cell}->rc == 0) {{ {inner} tvg_free_small({cell}, sizeof(*{cell})); }}")
    }

    // ------------------------------------------------------------ statements

    pub(crate) fn stmt(&mut self, s: StmtId) {
        // (a loop's body that's one statement: `for (...) m.set(k, (m.get(k) ?? 0) + 1)`)
        let m = self.cur_m();
        self.stmt_span = Some(self.ast(m).stmt(s).span);
        if matches!(self.ast(m).stmt(s).kind, StmtKind::Expr(_)) && self.map_count(&[s], 0).is_some() {
            return;
        }
        // (a loop: the lengths it can rely on, of arrays nothing in the function resizes)
        if matches!(self.ast(m).stmt(s).kind, StmtKind::For { .. } | StmtKind::ForOf { .. } | StmtKind::While(..) | StmtKind::DoWhile(..)) {
            let fixed = self.fixed_lens();
            // (from where each holds: an array a loop fills, after that loop)
            let at = self.ast(m).stmt(s).span.start;
            // (of the arrays the loop uses: one released before it, after its last use, is gone)
            let mut used = Vec::new();
            collect_exprs_stmt(self.ast(m), s, &mut used);
            let used: FxSet<u32> = used.iter().filter_map(|&x| match self.ident_fact(m, x) {
                Some(IdentFact::Local(k)) => Some(k),
                _ => None,
            }).collect();
            let mut facts: Vec<(String, u64)> = fixed.iter().filter(|(k, (_, from))| at >= *from && used.contains(*k)).filter_map(|(k, (n, _))| self.b().locals.get(k).map(|l| (l.access.clone(), *n))).collect();
            facts.sort();
            for (access, n) in facts {
                self.line(format!("TV_ASSUME(({access}).p->len == {n});"));
            }
        }
        // (a loop that is all JavaScript's: run as one JavaScript function)
        let fused = self.js_loop(s);
        if fused == super::jsloop::JsLoop::Done {
            return;
        }
        // `outer: for (...)`: `break outer` jumps past it, `continue outer` to its next round
        let label = self.ast(m).labels.iter().find(|(st, _)| *st == s).map(|x| x.1);
        let Some(label) = label else {
            self.stmt_inner(s);
            if fused == super::jsloop::JsLoop::Guarded {
                self.close("}");
            }
            return;
        };
        let end = self.fresh("lend");
        let at = self.b().loops.len();
        let depth = self.b().scopes.len();
        self.b().labeled.push((label, end.clone(), at, depth));
        self.stmt_inner(s);
        self.b().labeled.pop();
        self.line(format!("{end}:;"));
    }

    fn stmt_inner(&mut self, s: StmtId) {
        let m = self.cur_m();
        let ast = self.ast(m);
        let node = ast.stmt(s);
        match &node.kind {
            StmtKind::Empty | StmtKind::Error => {}
            StmtKind::InitGlobal(ii) => {
                // (an imported module's variables are set up with every module's, at the start)
                if self.script_module != Some(m) {
                    return;
                }
                // a class: its static fields, where it's declared
                if let ast::ItemKind::Class(_) = &self.ast(m).items[*ii as usize].kind {
                    self.init_script_statics(m, *ii);
                    return;
                }
                // `g = init_g();` here, in source order (a script's module variable); an
                // initializer that awaits is evaluated right here, in the async script body.
                let g = self.const_ref(m, *ii);
                if self.literal_global(m, *ii) {
                    return;
                }
                let ast::ItemKind::Const { init, .. } = &self.ast(m).items[*ii as usize].kind else { unreachable!() };
                if super::asyncfn::has_await(self.ast(m), *init) {
                    self.push_temps();
                    let v = self.expr(*init);
                    let v = self.coerce(v, g.ty);
                    let code = self.consume(v);
                    self.line(format!("{} = {code};", g.code));
                    self.pop_temps();
                    return;
                }
                self.line(format!("{} = init_{}();", g.code, g.code));
                if self.c.const_throws.contains(&(m, *ii)) {
                    self.error_check();
                }
            }
            StmtKind::Expr(e) => {
                self.push_temps();
                // (`xs[i] = ...`: what the statement stores into, for `Math.min`'s order)
                let stored = match &ast.expr(*e).kind {
                    ExprKind::Assign(_, t, _) => match &ast.expr(*t).kind {
                        ExprKind::Index { obj, .. } => match self.ident_fact(m, *obj) {
                            Some(IdentFact::Local(k)) => Some(k),
                            _ => None,
                        },
                        _ => None,
                    },
                    _ => None,
                };
                let outer_store = std::mem::replace(&mut self.store_arr, stored);
                let v = self.expr(*e);
                self.store_arr = outer_store;
                // Keep side effects (and traps) of plain expression values. (A clock read has none:
                // `performance.now();` alone is nothing.)
                if !v.owned && !is_trivial(&v.code) && v.code != "tv_performance_now()" {
                    self.line(format!("(void)({});", v.code));
                }
                self.pop_temps();
                if let Some((_, sid, cv, d, i)) = self.b().carry.clone()
                    && sid == s
                {
                    self.line(format!("{cv} = {d}[{i} + 0];"));
                }
            }
            StmtKind::Let { mutable, name, name_span, init, .. } => {
                let ty = self.facts(m).bindings.get(&s).copied().unwrap_or(ERROR);
                let ty = self.inst(ty);
                // (`const parts: string[] = []`, only pushed onto, joined and counted: a builder)
                if let Some(i) = init
                    && let Some(sep) = self.builder_local(name_span.start, *i, ty)
                {
                    let (sb, n) = (self.fresh("bsb"), self.fresh("bn"));
                    self.line(format!("tv_sb {sb} = {{0}}; tv_int {n} = 0;"));
                    self.b().scopes.last_mut().unwrap().releases.push(format!("tv_sb_free(&{sb});"));
                    self.b().builders.insert(name_span.start, (sb, n, sep));
                    let _ = mutable;
                    let _ = name;
                    return;
                }
                let uniq = self.fresh("");
                let cname = format!("v_{}_{}", self.sym(*name), uniq);
                let ct = self.ctype(ty);
                let key = name_span.start;
                self.push_temps();
                if let Some(i) = init
                    && matches!(ast.expr(*i).kind, ExprKind::Arrow(_))
                {
                    self.self_closure = Some(key);
                }
                // (a closure in the initializer refers to the local: its cell exists first, the
                // value stored into it after)
                if self.facts(m).self_init.contains(&key) && self.b().boxed.contains(&key) {
                    let cell = self.cell_type(ty);
                    let empty = self.default_value(ty);
                    self.line(format!("{cell} *{cname} = tvg_alloc_small(sizeof({cell})); {cname}->rc = 1; {cname}->v = {empty};"));
                    self.b().locals.insert(key, Local { access: format!("{cname}->v"), ty });
                    let v = self.expr(init.unwrap());
                    let v = self.coerce(v, ty);
                    let code = self.consume(v);
                    self.line(format!("{cname}->v = {code};"));
                    self.pop_temps();
                    let rel = self.cell_release(ty, &cname);
                    self.b().scopes.last_mut().unwrap().releases.push(rel);
                    return;
                }
                let value = init.map(|i| {
                    let v = self.expr(i);
                    self.coerce(v, ty)
                });
                self.self_closure = None;
                // `const x = P` taking P's reference (see `take_plan`): no retain, and no release
                // until P is stored to
                if self.b().take_let {
                    self.b().take_let = false;
                    if let Some(v) = &value
                        && !v.owned
                    {
                        self.line(format!("{ct} {cname} = {};", v.code));
                        self.pop_temps();
                        self.b().locals.insert(key, Local { access: cname, ty });
                        self.b().owned_locals.insert(key);
                        return;
                    }
                    self.b().take_slot = None;
                }
                // `let x = list` walked only by `x = x.next` in a function that stores no
                // reference anywhere: x borrows each node (see `cursor_local`)
                if let (true, Some(i), Some(v)) = (*mutable, init, &value)
                    && !v.owned
                    && self.is_rc(ty)
                    && self.cursor_local(key, ty, *i)
                {
                    self.line(format!("{ct} {cname} = {};", v.code));
                    self.pop_temps();
                    self.b().locals.insert(key, Local { access: cname, ty });
                    self.b().cursors.insert(key);
                    return;
                }
                // `const x = <field path of a read-only parameter or unchanging local>` that is never
                // changed itself: x is a borrowed view — no retain now, no release at scope end.
                // (a class instance's fields changing through x is fine: x is the same object)
                let object = matches!(self.tget(ty), Ty::Class(..)) || self.niche(ty).is_some_and(|(pi, _)| matches!(self.tget(ty), Ty::Union(ms) if matches!(self.tget(self.c.types.tys(ms)[pi]), Ty::Class(..))));
                if let (false, Some(i), Some(v)) = (*mutable, init, &value)
                    && !v.owned
                    && (object || !self.b().mutated.contains(&key))
                    && !self.b().boxed.contains(&key)
                    && (self.stable_path(*i) || object && self.quiet_path(*i))
                {
                    self.line(format!("{ct} {cname} = {};", v.code));
                    self.pop_temps();
                    self.b().locals.insert(key, Local { access: cname, ty });
                    return;
                }
                // `const r = lib.call(...)` of a JavaScript value read only here (its properties,
                // passed on, compared): it stays on this function's stack, where the engine's
                // collector finds it by itself, so it needn't be rooted (see `js_stack_local`)
                if let (false, Some(_), Some(v)) = (*mutable, init, &value)
                    && ty == JS
                    && !v.owned
                    && !self.b().boxed.contains(&key)
                    && self.js_stack_local(key)
                {
                    self.line(format!("{ct} {cname} = {};", v.code));
                    self.pop_temps();
                    self.b().locals.insert(key, Local { access: cname, ty });
                    return;
                }
                let code = match value {
                    Some(v) => self.consume(v),
                    // (`let m: T | undefined` starts out `undefined`, as in JavaScript)
                    None if ty != UNDEFINED && self.c.types.has_undefined(ty) => {
                        let v = self.coerce(Val::plain("0", UNDEFINED), ty);
                        self.consume(v)
                    }
                    None if ty != NULL && self.members(ty).contains(&NULL) => {
                        let v = self.coerce(Val::plain("0", NULL), ty);
                        self.consume(v)
                    }
                    None => self.default_value(ty),
                };
                if self.b().boxed.contains(&key) {
                    let cell = self.cell_type(ty);
                    self.line(format!("{cell} *{cname} = tvg_alloc_small(sizeof({cell})); {cname}->rc = 1; {cname}->v = {code};"));
                    self.pop_temps();
                    let rel = self.cell_release(ty, &cname);
                    self.b().scopes.last_mut().unwrap().releases.push(rel);
                    self.b().locals.insert(key, Local { access: format!("{cname}->v"), ty });
                } else {
                    self.line(format!("{ct} {cname} = {code};"));
                    self.pop_temps();
                    if self.is_rc(ty) {
                        let rel = format!("{};", self.release_code(ty, &cname));
                        self.b().scopes.last_mut().unwrap().releases.push(rel);
                        self.b().owned_locals.insert(key);
                    }
                    self.b().locals.insert(key, Local { access: cname, ty });
                }
            }
            StmtKind::Block(ss) => {
                self.open("{");
                self.push_scope();
                self.stmts(ss);
                self.pop_scope();
                self.close("}");
            }
            StmtKind::If(c, t, e) => {
                self.push_temps();
                let cond = self.cond(*c);
                let cv = self.fresh("c");
                self.line(format!("bool {cv} = {cond};"));
                self.pop_temps();
                // (a branch that throws is the unlikely one: laid out away from the rest)
                let ast = self.ast(m);
                if throws_out(ast, *t) && e.is_none_or(|e| !throws_out(ast, e)) {
                    self.open(&format!("if (__builtin_expect({cv}, 0)) {{"));
                } else if e.is_some_and(|e| throws_out(ast, e)) && !throws_out(ast, *t) {
                    self.open(&format!("if (__builtin_expect({cv}, 1)) {{"));
                } else {
                    self.open(&format!("if ({cv}) {{"));
                }
                self.scoped_stmt(*t);
                if let Some(e) = e {
                    self.close("} else {");
                    self.b().indent += 1;
                    self.scoped_stmt(*e);
                }
                self.close("}");
            }
            StmtKind::While(c, body) => {
                let hoisted = self.hoist_unique(&[*body], &[*c]);
                // `while (k <= n) { ... xs[k - 1] ...; k += s }`: a copy without the checks
                if let Some((vplan, step)) = self.while_version_plan(*c, *body) {
                    let check = self.version_check(&vplan);
                    self.push_temps();
                    let sv = self.expr(step);
                    let sv = self.int_code(sv);
                    self.pop_temps();
                    self.open(&format!("if ({sv} >= 0 && {check}) {{"));
                    let mut ptrs = Vec::new();
                    for xk in vplan.arrays.iter().map(|a| a.0).chain(vplan.extra.iter().copied()) {
                        let local = self.b().locals[&xk].clone();
                        let Ty::Array(et) = self.tget(local.ty) else { continue };
                        let ect = self.ctype(et);
                        let (d, n) = (self.fresh("xd"), self.fresh("xn"));
                        self.line(format!("{ect} *{d} = ({ect} *)tv_arr_data({0}); tv_int {n} = tv_arr_len({0});", local.access));
                        ptrs.push((xk, d, n));
                    }
                    self.b().versioned.push((vec![vplan.ik], ptrs));
                    let fast = self.fresh("cont");
                    self.open("for (;;) {");
                    self.cond_break(*c);
                    self.loop_body(*body, &fast);
                    self.line(format!("{fast}:;"));
                    self.close("}");
                    self.b().versioned.pop();
                    self.close("} else {");
                    self.b().indent += 1;
                    let cont = self.fresh("cont");
                    self.open("for (;;) {");
                    self.cond_break(*c);
                    self.loop_body(*body, &cont);
                    self.line(format!("{cont}:;"));
                    self.close("}");
                    self.close("}");
                    self.unhoist(hoisted);
                    return;
                }
                let plan = self.two_pointer_plan(*c, *body);
                if let Some((up, down, arrays)) = &plan {
                    // `i >= 0` and `j < xs.length` now: every `xs[i]` and `xs[j]` it reaches is too
                    let (i, j) = (self.b().locals[up].access.clone(), self.b().locals[down].access.clone());
                    let mut parts = vec![format!("{i} >= 0")];
                    for xk in arrays {
                        parts.push(format!("{j} < tv_arr_len({})", self.b().locals[xk].access));
                    }
                    self.open(&format!("if ({}) {{", parts.join(" && ")));
                    let mut ptrs = Vec::new();
                    for &xk in arrays {
                        let local = self.b().locals[&xk].clone();
                        let Ty::Array(et) = self.tget(local.ty) else { continue };
                        let ect = self.ctype(et);
                        let (d, n) = (self.fresh("xd"), self.fresh("xn"));
                        self.line(format!("{ect} *{d} = ({ect} *)tv_arr_data({0}); tv_int {n} = tv_arr_len({0});", local.access));
                        ptrs.push((xk, d, n));
                    }
                    self.b().versioned.push((vec![*up, *down], ptrs));
                    self.b().nowrap_locals.extend([*up, *down]);
                    // Under 8 apart (a short reversal): a copy LLVM unrolls whole, told what it
                    // can't work out from `i < j` with both moving, that `i` stays under i0 + 4
                    // (both step every round, no `continue` skipping one). fannkuch's flips:
                    // 1.56 s to 1.46 on an M-series Mac, 2.00 to 1.87 on x86 (16 apart: 1.53, 1.85).
                    let short = !stmt_has_continue(self.ast(m), *body);
                    if short {
                        let i0 = self.fresh("i0");
                        self.line(format!("tv_int {i0} = {i};"));
                        self.open(&format!("if ((uint64_t){j} - (uint64_t){i} < 8) {{"));
                        let cont = self.fresh("cont");
                        self.open("for (;;) {");
                        self.cond_break(*c);
                        self.line(format!("if (!({i} - {i0} < 4)) break;"));
                        self.loop_body(*body, &cont);
                        self.line(format!("{cont}:;"));
                        self.close("}");
                        self.close("} else {");
                        self.b().indent += 1;
                    }
                    let cont = self.fresh("cont");
                    self.open("for (;;) {");
                    self.cond_break(*c);
                    self.loop_body(*body, &cont);
                    self.line(format!("{cont}:;"));
                    self.close("}");
                    if short {
                        self.close("}");
                    }
                    let n = self.b().nowrap_locals.len();
                    self.b().nowrap_locals.truncate(n - 2);
                    self.b().versioned.pop();
                    self.close("} else {");
                    self.b().indent += 1;
                }
                let cont = self.fresh("cont");
                let counter = self.guarded_counter(*c, *body);
                self.open_loop(*body, &[*c]);
                self.cond_break(*c);
                if let Some(k) = counter {
                    self.b().nowrap_locals.push(k);
                }
                self.loop_body(*body, &cont);
                if counter.is_some() {
                    self.b().nowrap_locals.pop();
                }
                self.line(format!("{cont}:;"));
                self.close("}");
                if plan.is_some() {
                    self.close("}");
                }
                self.unhoist(hoisted);
            }
            StmtKind::DoWhile(body, c) => {
                let hoisted = self.hoist_unique(&[*body], &[*c]);
                let cont = self.fresh("cont");
                self.open_loop(*body, &[*c]);
                self.loop_body(*body, &cont);
                self.line(format!("{cont}:;"));
                self.cond_break(*c);
                self.close("}");
                self.unhoist(hoisted);
            }
            StmtKind::For { init, cond, step, body } => {
                let cont = self.fresh("cont");
                self.open("{");
                self.push_scope();
                if let Some(i) = init {
                    self.stmt(*i);
                }
                let iteration_binding = init.and_then(|i| match &self.ast(m).stmt(i).kind {
                    StmtKind::Let { mutable: true, name_span, .. } if self.b().boxed.contains(&name_span.start) => Some(name_span.start),
                    _ => None,
                });
                if let Some(key) = iteration_binding {
                    self.copy_iteration_binding(key);
                }
                let loop_exprs: Vec<ExprId> = [cond, step].into_iter().flatten().copied().collect();
                let hoisted = self.hoist_unique(&[*body], &loop_exprs);
                let divs = self.hoist_divisors(*body, &loop_exprs);
                self.reserve_for_pushes(*init, *cond, *step, *body);
                let bounds = self.counted_loop(*init, *cond, *step, *body);
                // (the array's length stays put only if nothing in the loop may resize it)
                let bounds = bounds.filter(|&(_, xk)| {
                    let mut all = Vec::new();
                    collect_exprs_stmt(self.ast(m), *body, &mut all);
                    !self.may_resize(m, &all, &[xk])
                });
                let nowrap = self.step_cannot_overflow(*init, *cond, *step, *body);
                let plan = if iteration_binding.is_none() { self.version_plan(*init, *cond, *step, *body) } else { None };
                if let Some(plan) = plan {
                    // Every `xs[i + k]` in range for every `i` the loop can reach: a copy without the
                    // checks, and the checked loop otherwise.
                    let check = self.version_check(&plan);
                    self.open(&format!("if ({check}) {{"));
                    // (each array's elements, from a local: nothing in the loop moves them)
                    let mut ptrs = Vec::new();
                    for xk in plan.arrays.iter().map(|a| a.0).chain(plan.extra.iter().copied()) {
                        let local = self.b().locals[&xk].clone();
                        let Ty::Array(et) = self.tget(local.ty) else { continue };
                        let ect = self.ctype(et);
                        let (d, n) = (self.fresh("xd"), self.fresh("xn"));
                        self.line(format!("{ect} *{d} = ({ect} *)tv_arr_data({0}); tv_int {n} = tv_arr_len({0});", local.access));
                        ptrs.push((xk, d, n));
                    }
                    let fast_cont = self.fresh("cont");
                    // (`xs[i] = f(xs[i - 1], ...)`: last round's value from a local, not memory)
                    if let Some((xk, sid)) = self.carry_plan(*body, *step, plan.ik)
                        && let Some((_, d, n)) = ptrs.iter().find(|p| p.0 == xk).cloned()
                        && let xt = self.b().locals[&xk].ty
                        && let Ty::Array(et) = self.tget(xt)
                    {
                        let (ect, cv, i) = (self.ctype(et), self.fresh("cy"), self.b().locals[&plan.ik].access.clone());
                        self.line(format!("{ect} {cv} = (uint64_t)({i} - 1) < (uint64_t){n} ? {d}[{i} - 1] : 0;"));
                        self.b().carry = Some((xk, sid, cv, d, i));
                    }
                    self.b().versioned.push((vec![plan.ik], ptrs));
                    self.for_loop(*cond, *step, *body, &fast_cont, bounds, Some(plan.ik), None);
                    self.b().versioned.pop();
                    self.b().carry = None;
                    self.close("} else {");
                    self.b().indent += 1;
                    self.for_loop(*cond, *step, *body, &cont, bounds, nowrap, None);
                    self.close("}");
                } else if iteration_binding.is_none()
                    && let Some(rp) = self.row_plan(*init, *cond, *step, *body)
                {
                    // Every row there and long enough for every `j` the loop can reach: a copy
                    // reaching their elements from locals, and the checked loop otherwise.
                    let j = self.b().locals[&rp.jk].access.clone();
                    self.push_temps();
                    let n = self.expr(rp.hi);
                    let n = self.int_code(n);
                    self.pop_temps();
                    let nv = self.fresh("rn");
                    self.line(format!("tv_int {nv} = {n};"));
                    let mut parts = Vec::new();
                    let mut slots = Vec::new();
                    for (xk, at, written, lo, hi) in &rp.rows {
                        let x = self.b().locals[xk].access.clone();
                        let xt = self.b().locals[xk].ty;
                        let Ty::Array(row_t) = self.tget(xt) else { continue };
                        let Ty::Array(et) = self.tget(row_t) else { continue };
                        let i = match at {
                            (None, v) => format!("{v}"),
                            (Some(ik), off) => format!("({} + {off})", self.b().locals[ik].access),
                        };
                        let rp_ = self.fresh("rp");
                        self.line(format!("tv_arr *{rp_} = (uint64_t){i} < (uint64_t)tv_arr_len({x}) ? &((tv_arr *)tv_arr_data({x}))[{i}] : NULL;"));
                        if *written {
                            let d = self.desc(et);
                            self.line(format!("if ({rp_}) tvg_make_unique({rp_}, {d});"));
                        }
                        parts.push(format!("{rp_}"));
                        if lo <= hi {
                            parts.push(format!("{j} + {lo} >= 0 && {nv} + {} <= tv_arr_len(*{rp_})", hi + rp.le as i64));
                        }
                        slots.push((*xk, *at, rp_, et));
                    }
                    self.open(&format!("if ({}) {{", parts.join(" && ")));
                    let mut rows = Vec::new();
                    for (xk, at, rp_, et) in slots {
                        let ect = self.ctype(et);
                        let (d, n) = (self.fresh("rd"), self.fresh("rl"));
                        self.line(format!("{ect} *{d} = ({ect} *)tv_arr_data(*{rp_}); tv_int {n} = tv_arr_len(*{rp_});"));
                        rows.push((xk, at, rp.jk, d, n));
                    }
                    let saved = std::mem::replace(&mut self.b().rows, rows);
                    let fast_cont = self.fresh("cont");
                    self.for_loop(*cond, *step, *body, &fast_cont, bounds, nowrap, None);
                    self.b().rows = saved;
                    self.close("} else {");
                    self.b().indent += 1;
                    self.for_loop(*cond, *step, *body, &cont, bounds, nowrap, None);
                    self.close("}");
                } else {
                    self.for_loop(*cond, *step, *body, &cont, bounds, nowrap, iteration_binding);
                }
                self.unhoist(hoisted);
                for k in divs {
                    self.b().divs.remove(&k);
                }
                self.pop_scope();
                self.close("}");
            }
            StmtKind::ForOf { name, name_span, iter, body, .. } if self.keys_loop(*iter, *name, *body).is_some() => {
                // `for (const k in m)` / `for (const k of Object.keys(m))` of a map whose keys the
                // body can't change: the map's own entries, in order (no array of keys), and
                // `m[k]` the entry's value
                let (mx, k, v) = self.keys_loop(*iter, *name, *body).unwrap();
                let cont = self.fresh("cont");
                self.open("{");
                self.push_scope();
                self.push_temps();
                let mv = self.expr(mx);
                let mt = mv.ty;
                let snap = self.consume(mv);
                let sn = self.fresh("it");
                self.line(format!("tv_map {sn} = {snap};"));
                self.pop_temps();
                let rel = format!("{};", self.release_code(mt, &sn));
                self.b().scopes.last_mut().unwrap().releases.push(rel);
                let (kd, vd, kct) = (self.desc(k), self.desc(v), self.ctype(k));
                let uniq = self.fresh("");
                let var = format!("v_{}_{}", self.sym(*name), uniq);
                let cur = self.fresh("i");
                self.line(format!("tv_int {cur} = 0; void *{cur}k, *{cur}v;"));
                self.open(&format!("while (tv_map_next({sn}, {kd}, {vd}, &{cur}, &{cur}k, &{cur}v)) {{"));
                self.line(format!("{kct} {var} = *({kct} *){cur}k;"));
                self.b().locals.insert(name_span.start, Local { access: var.clone(), ty: k });
                // (`m[k]` reads the map's code: the variable it's in, unchanged by the body)
                let mcode = self.expr_code_of(mx);
                self.b().map_loops.push((mcode, var, format!("{cur}v"), v));
                let hoisted = self.hoist_unique(&[*body], &[]);
                let binding = self.b().boxed.contains(&name_span.start).then_some(name_span.start);
                self.loop_body_with_binding(*body, &cont, binding);
                self.unhoist(hoisted);
                self.b().map_loops.pop();
                self.line(format!("{cont}:;"));
                self.close("}");
                self.pop_scope();
                self.close("}");
            }
            StmtKind::ForOf { name, name_span, iter, body, .. } => {
                let elem = self.facts(m).bindings.get(&s).copied().unwrap_or(ERROR);
                let elem = self.inst(elem);
                let cont = self.fresh("cont");
                self.open("{");
                self.push_scope();
                self.push_temps();
                // (`for (k in m)` of a map variable: the map, to test each key's still there)
                let for_in_map = match &self.ast(m).expr(*iter).kind {
                    ExprKind::Call { args, .. } if self.ast(m).for_in_keys.contains(iter) && args.len() == 1 && matches!(self.ast(m).expr(args[0].expr).kind, ExprKind::Ident(_)) => {
                        let mx = args[0].expr;
                        let mt = self.ty(mx);
                        match self.tget(mt) {
                            Ty::Map(k, v) if k == STR => {
                                let code = self.expr_code_of(mx);
                                Some((code, self.desc(k), self.desc(v)))
                            }
                            _ => None,
                        }
                    }
                    _ => None,
                };
                let coll = self.expr(*iter);
                // a string: its characters (code points), as JavaScript iterates it
                let coll = if matches!(self.tget(coll.ty), Ty::Str | Ty::StrLit(_)) { self.coerce(coll, STR) } else { coll };
                let coll_ty = coll.ty;
                // Iterate a snapshot (retained), so the loop is unaffected by mutation of the collection.
                let snap = self.consume(coll);
                let ct = self.ctype(coll_ty);
                let sn = self.fresh("it");
                self.line(format!("{ct} {sn} = {snap};"));
                self.pop_temps();
                let rel = format!("{};", self.release_code(coll_ty, &sn));
                self.b().scopes.last_mut().unwrap().releases.push(rel);
                let ect = self.ctype(elem);
                let uniq = self.fresh("");
                let var = format!("v_{}_{}", self.sym(*name), uniq);
                match self.tget(coll_ty) {
                    // (an `any`: what it holds, a string or an array, each)
                    Ty::Unknown => {
                        self.any_used();
                        let span = self.ast(m).stmt(s).span;
                        let (idx, n) = (self.fresh("i"), self.fresh("n"));
                        let loc = self.loc(span);
                        self.line(format!("tv_int {n} = tvg_any_len({sn}); if ({n} < 0) tv_trap(\"TypeError: this value is not iterable\", {loc});"));
                        self.open(&format!("for (tv_int {idx} = 0; {idx} < {n}; {idx}++) {{"));
                        self.line(format!("tv_dyn {var} = ({{ tv_dyn k_ = tv_dyn_box(&tv_type_int, &{idx}); tv_dyn r_ = tvg_any_at({sn}, k_, {loc}); tv_dyn_release(k_); r_; }});"));
                        self.b().iter_release = Some(format!("tv_dyn_release({var});"));
                    }
                    Ty::Js => {
                        let span = self.ast(m).stmt(s).span;
                        let arr = self.js_array_of(&sn, span);
                        let (idx, n) = (self.fresh("i"), self.fresh("n"));
                        let loc = self.loc(span);
                        self.line(format!("uint32_t {n} = tv_js_length({arr});"));
                        self.open(&format!("for (uint32_t {idx} = 0; {idx} < {n}; {idx}++) {{"));
                        self.line(format!("JSValueRef {var} = tv_js_at_or_trap({arr}, (double){idx}, {loc});"));
                    }
                    Ty::Map(k, v) => {
                        // `[key, value]` entries (borrowed)
                        let (kd, vd, kct, vct) = (self.desc(k), self.desc(v), self.ctype(k), self.ctype(v));
                        let f = self.tuple_of(elem).unwrap_or_default();
                        let (f0, f1) = match f.as_slice() {
                            [a, b] => (self.sym(a.name).to_string(), self.sym(b.name).to_string()),
                            _ => ("0".into(), "1".into()),
                        };
                        let cur = self.fresh("i");
                        self.line(format!("tv_int {cur} = 0; void *{cur}k, *{cur}v;"));
                        self.open(&format!("while (tv_map_next({sn}, {kd}, {vd}, &{cur}, &{cur}k, &{cur}v)) {{"));
                        if self.is_obj(elem) {
                            // (an object of its own each time: retained parts, released with it)
                            let (rk, rv) = (self.retain_code(k, &format!("*({kct} *){cur}k")), self.retain_code(v, &format!("*({vct} *){cur}v")));
                            let lit = self.rec_lit(elem, &format!(".f_{f0} = *({kct} *){cur}k, .f_{f1} = *({vct} *){cur}v"));
                            self.line(format!("{rk}; {rv}; {ect} {var} = {lit};"));
                            let rel = format!("{};", self.release_code(elem, &var));
                            self.b().iter_release = Some(rel);
                        } else {
                            self.line(format!("{ect} {var} = ({ect}){{ .f_{f0} = *({kct} *){cur}k, .f_{f1} = *({vct} *){cur}v }};"));
                        }
                    }
                    // (each code point, made as it's reached)
                    Ty::Str => {
                        let cur = self.fresh("i");
                        self.line(format!("tv_int {cur} = 0;"));
                        self.open("for (;;) {");
                        self.line(format!("tv_str {var}; if (!tv_str_next_char({sn}, &{cur}, &{var})) break;"));
                        self.b().iter_release = Some(format!("tv_str_release({var});"));
                    }
                    Ty::Set(_) => {
                        let kd = self.desc(elem);
                        let cur = self.fresh("i");
                        self.line(format!("tv_int {cur} = 0; void *{cur}k, *{cur}v;"));
                        self.open(&format!("while (tv_map_next({sn}, {kd}, &tv_type_undefined, &{cur}, &{cur}k, &{cur}v)) {{"));
                        self.line(format!("{ect} {var} = *({ect} *){cur}k;"));
                    }
                    // a generator: each value it yields (borrowed from it until the next one)
                    Ty::Gen(_) => {
                        self.open("for (;;) {");
                        self.line(format!("if (tv_gen_next({sn})) break;"));
                        self.line(format!("{ect} {var} = *({ect} *){sn}->yv;"));
                    }
                    _ => {
                        let idx = self.fresh("i");
                        self.open(&format!("for (tv_int {idx} = 0; {idx} < tv_arr_len({sn}); {idx}++) {{"));
                        self.line(format!("{ect} {var} = (({ect} *)tv_arr_data({sn}))[{idx}];"));
                        // (`for (k in m)`: a key deleted on the way is skipped, as in JavaScript)
                        if let Some((mcode, kd, vd)) = for_in_map {
                            self.line(format!("if (!tv_map_has({mcode}, {kd}, {vd}, &{var})) continue;"));
                        }
                    }
                }
                self.b().locals.insert(name_span.start, Local { access: var, ty: elem });
                let hoisted = self.hoist_unique(&[*body], &[]);
                let binding = self.b().boxed.contains(&name_span.start).then_some(name_span.start);
                self.loop_body_with_binding(*body, &cont, binding);
                self.unhoist(hoisted);
                self.line(format!("{cont}:;"));
                self.close("}");
                self.pop_scope();
                self.close("}");
            }
            StmtKind::Switch(disc, cases) => self.switch(s, *disc, cases),
            StmtKind::Throw(x) => {
                self.push_temps();
                let v = self.expr(*x);
                // Errors are released through their class id.
                self.rc_roots.insert(v.ty);
                let code = self.consume(v);
                self.line(format!("tvg_err = (void *)({code});"));
                self.error_path();
                self.pop_temps_silent();
            }
            StmtKind::Try { body, catch, finally } => self.try_stmt(s, *body, catch.as_ref(), *finally),
            StmtKind::Return(v) => {
                let ret = self.b().ret;
                if let (Some(x), Some(tre)) = (v, self.b().tre.clone())
                    && let Some((a, args)) = self.tre_tail(*x, &tre)
                {
                    // `return a + f(args)`: acc += a (recording the prefix sum before it), rebind
                    // the parameters (every argument first), and loop.
                    self.push_temps();
                    let av = self.expr(a);
                    let av = self.coerce(av, INT);
                    let code = self.consume(av);
                    let t = self.fresh("tre_a");
                    self.line(format!("tv_int {t} = {code};"));
                    if let Some(looper) = &tre.entry {
                        // The first call: continue in the looping copy.
                        let mut all = vec![t.clone()];
                        for (i, arg) in args.iter().enumerate() {
                            let v = self.expr(*arg);
                            let v = self.coerce(v, tre.ptys[i]);
                            let code = self.consume(v);
                            let ct = self.ctype(tre.ptys[i]);
                            let n = self.fresh("tre_p");
                            self.line(format!("{ct} {n} = {code};"));
                            all.push(n);
                        }
                        let r = self.fresh("r");
                        self.line(format!("tv_int {r} = {looper}({});", all.join(", ")));
                        self.release_all_temps();
                        self.unwind_for_exit(0);
                        self.line(format!("return {r};"));
                        self.b().temps.pop();
                        return;
                    }
                    self.line(format!("TVG_TRE_ADD({t});"));
                    let mut news = Vec::new();
                    for (i, arg) in args.iter().enumerate() {
                        let v = self.expr(*arg);
                        let v = self.coerce(v, tre.ptys[i]);
                        let code = self.consume(v);
                        let ct = self.ctype(tre.ptys[i]);
                        let n = self.fresh("tre_p");
                        self.line(format!("{ct} {n} = {code};"));
                        news.push(n);
                    }
                    for (i, n) in news.iter().enumerate() {
                        self.line(format!("p{i} = {n};"));
                    }
                    self.release_all_temps();
                    self.unwind_for_exit(0);
                    self.line("goto tre_top;");
                    self.b().temps.pop();
                    return;
                }
                match v {
                    Some(x) if ret != VOID && self.b().tre.as_ref().is_some_and(|t| t.entry.is_none()) => {
                        // A base case of a looping function: add the accumulated terms.
                        let loc = self.b().tre.as_ref().unwrap().loc.clone();
                        self.push_temps();
                        let val = self.expr(*x);
                        let val = self.coerce(val, ret);
                        let code = self.consume(val);
                        let r = self.fresh("r");
                        self.line(format!("tv_int {r} = {code};"));
                        self.release_all_temps();
                        self.unwind_for_exit(0);
                        self.line(format!("{r} = TVG_TRE_END({r}, {loc});"));
                        self.line(format!("return {r};"));
                        self.b().temps.pop();
                    }
                    Some(x) if ret != VOID => {
                        self.push_temps();
                        // (`return x` of a narrowed local held as the result's type: the local
                        // itself, not its narrowed view made the result's again)
                        let m = self.cur_m();
                        let held = match (&self.ast(m).expr(*x).kind, self.ident_fact(m, *x)) {
                            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => {
                                let narrowed = self.ty(*x);
                                self.b().locals.get(&k).filter(|l| l.ty == ret && narrowed != ret).cloned()
                            }
                            _ => None,
                        };
                        // (what's returned, by its span: a builder joined there gives its text up)
                        let outer_ret = self.return_span.replace(self.ast(m).expr(*x).span);
                        let val = match held {
                            Some(l) => Val::plain(l.access, l.ty),
                            None => self.expr(*x),
                        };
                        self.return_span = outer_ret;
                        let val = self.coerce(val, ret);
                        let code = self.consume(val);
                        let ct = self.ctype(ret);
                        let r = self.fresh("r");
                        self.line(format!("{ct} {r} = {code};"));
                        self.release_all_temps();
                        self.unwind_for_exit(0);
                        self.line(format!("return {r};"));
                        // The statement's temp scope is gone on this path; keep structure balanced.
                        self.b().temps.pop();
                    }
                    Some(x) => {
                        self.push_temps();
                        let _ = self.expr(*x);
                        self.pop_temps();
                        self.unwind_for_exit(0);
                        self.line("return;");
                    }
                    None => {
                        if ret == VOID {
                            self.unwind_for_exit(0);
                            self.line("return;");
                        } else {
                            let v = self.coerce(Val::plain("0", UNDEFINED), ret);
                            let ct = self.ctype(ret);
                            let r = self.fresh("r");
                            self.line(format!("{ct} {r} = {};", v.code));
                            self.unwind_for_exit(0);
                            self.line(format!("return {r};"));
                        }
                    }
                }
            }
            StmtKind::Break(Some(l)) => {
                let (_, target, _, depth) = self.b().labeled.iter().rev().find(|x| x.0 == *l).cloned().expect("label");
                self.unwind_for_exit(depth);
                self.line(format!("goto {target};"));
            }
            StmtKind::Continue(Some(l)) => {
                let (_, _, at, _) = self.b().labeled.iter().rev().find(|x| x.0 == *l).cloned().expect("label");
                let (label, depth) = self.b().loops[at].clone();
                self.unwind_for_exit(depth);
                self.line(format!("goto {label};"));
            }
            StmtKind::Break(None) => {
                let (target, depth) = self.b().breaks.last().cloned().expect("break target");
                self.unwind_for_exit(depth);
                match target {
                    BreakTarget::Native => self.line("break;"),
                    BreakTarget::Goto(l) => self.line(format!("goto {l};")),
                }
            }
            StmtKind::Continue(None) => {
                let (label, depth) = self.b().loops.last().cloned().expect("continue target");
                self.unwind_for_exit(depth);
                self.line(format!("goto {label};"));
            }
        }
    }

    /// Drops the current temp scope without emitting releases (after a path that already left).
    fn pop_temps_silent(&mut self) {
        self.b().temps.pop();
    }

    /// `try { } catch (e) { } finally { }`: errors in the block jump to the handler label.
    fn try_stmt(&mut self, s: StmtId, body: StmtId, catch: Option<&ast::Catch>, finally: Option<StmtId>) {
        let m = self.cur_m();
        let (lcatch, lend, lfin) = (self.fresh("catch"), self.fresh("tryend"), self.fresh("finerr"));
        let pend = finally.map(|_| {
            let p = self.fresh("pend");
            self.line(format!("void *{p} = NULL;"));
            p
        });
        let depth = self.b().scopes.len();
        if let Some(f) = finally {
            self.b().finallies.push((f, depth));
        }
        self.b().handlers.push((lcatch.clone(), depth));
        self.scoped_block(body);
        self.b().handlers.pop();
        self.line(format!("goto {lend};"));
        self.line(format!("{lcatch}:;"));
        match catch {
            Some(c) => {
                self.open("{");
                self.push_scope();
                let ev = self.fresh("err");
                self.line(format!("void *{ev} = tvg_err; tvg_err = NULL;"));
                match &c.param {
                    Some((name, nspan, _)) => {
                        let ty = self.facts(m).bindings.get(&s).copied().unwrap_or(ERROR);
                        let ty = self.inst(ty);
                        let ct = self.ctype(ty);
                        let uniq = self.fresh("");
                        let var = format!("v_{}_{}", self.sym(*name), uniq);
                        self.line(format!("{ct} {var} = ({ct}){ev};"));
                        let rel = format!("{};", self.release_code(ty, &var));
                        self.b().scopes.last_mut().unwrap().releases.push(rel);
                        self.b().locals.insert(nspan.start, Local { access: var, ty });
                    }
                    None => self.line(format!("tvg_obj_release({ev});")),
                }
                if finally.is_some() {
                    self.b().handlers.push((lfin.clone(), depth));
                }
                let cm = self.cur_m();
                if let StmtKind::Block(ss) = &self.ast(cm).stmt(c.body).kind {
                    for &x in ss {
                        self.stmt(x);
                    }
                } else {
                    self.stmt(c.body);
                }
                if finally.is_some() {
                    self.b().handlers.pop();
                }
                self.pop_scope();
                self.close("}");
                self.line(format!("goto {lend};"));
                if let Some(p) = &pend {
                    self.line(format!("{lfin}:; {p} = tvg_err; tvg_err = NULL;"));
                }
            }
            None => {
                if let Some(p) = &pend {
                    self.line(format!("{p} = tvg_err; tvg_err = NULL;"));
                }
            }
        }
        if finally.is_some() {
            self.b().finallies.pop();
        }
        self.line(format!("{lend}:;"));
        if let (Some(f), Some(p)) = (finally, pend) {
            self.scoped_block(f);
            self.open(&format!("if ({p} != NULL) {{"));
            self.line(format!("tvg_err = {p};"));
            self.error_path();
            self.close("}");
        }
    }

    /// A block statement in its own C block and scope.
    fn scoped_block(&mut self, s: StmtId) {
        self.open("{");
        self.scoped_stmt(s);
        self.close("}");
    }

    /// A statement in its own scope (branches of `if`).
    fn scoped_stmt(&mut self, s: StmtId) {
        self.push_scope();
        let m = self.cur_m();
        if let StmtKind::Block(ss) = &self.ast(m).stmt(s).kind {
            self.stmts(ss);
        } else {
            self.stmt(s);
        }
        self.pop_scope();
    }

    fn cond_break(&mut self, c: ExprId) {
        self.push_temps();
        let cond = self.cond(c);
        let cv = self.fresh("c");
        self.line(format!("bool {cv} = {cond};"));
        self.pop_temps();
        self.line(format!("if (!{cv}) break;"));
    }

    /// Replace the owning cell before an iteration starts, leaving earlier closures on
    /// the old binding. The scope's release refers to the reassigned cell pointer.
    fn copy_iteration_binding(&mut self, key: u32) {
        let local = self.b().locals[&key].clone();
        let cell = local.access.trim_end_matches("->v");
        let ct = self.cell_type(local.ty);
        let next = self.fresh("iteration");
        self.line(format!("{ct} *{next} = tvg_alloc_small(sizeof({ct})); {next}->rc = 1; {next}->v = {};", local.access));
        if self.is_rc(local.ty) {
            let retain = self.retain_code(local.ty, &format!("{next}->v"));
            self.line(format!("{retain};"));
        }
        let release = self.cell_release(local.ty, cell);
        self.line(release);
        self.line(format!("{cell} = {next};"));
    }

    fn loop_body(&mut self, body: StmtId, cont: &str) {
        self.loop_body_with_binding(body, cont, None);
    }

    fn loop_body_with_binding(&mut self, body: StmtId, cont: &str, binding: Option<u32>) {
        self.push_scope();
        if let Some(rel) = self.b().iter_release.take() {
            self.b().scopes.last_mut().unwrap().releases.push(rel);
        }
        if let Some(key) = binding {
            let local = self.b().locals[&key].clone();
            let ct = self.cell_type(local.ty);
            let cell = self.fresh("iteration");
            self.line(format!("{ct} *{cell} = tvg_alloc_small(sizeof({ct})); {cell}->rc = 1; {cell}->v = {};", local.access));
            if self.is_rc(local.ty) {
                let retain = self.retain_code(local.ty, &format!("{cell}->v"));
                self.line(format!("{retain};"));
            }
            let release = self.cell_release(local.ty, &cell);
            self.b().scopes.last_mut().unwrap().releases.push(release);
            self.b().locals.insert(key, Local { access: format!("{cell}->v"), ty: local.ty });
        }
        let depth = self.b().scopes.len();
        // `continue` jumps to a label placed after the body scope was released, so it must release that scope itself (like `break`, depth - 1):
        // otherwise the refcounted locals of the body leak on every `continue`.
        self.b().loops.push((cont.to_string(), depth - 1));
        self.b().breaks.push((BreakTarget::Native, depth - 1));
        let m = self.cur_m();
        if let StmtKind::Block(ss) = &self.ast(m).stmt(body).kind {
            self.stmts(ss);
        } else {
            self.stmt(body);
        }
        self.b().loops.pop();
        self.b().breaks.pop();
        self.pop_scope();
    }

    fn switch(&mut self, _sid: StmtId, disc: ExprId, cases: &[ast::Case]) {
        let m = self.cur_m();
        let ast = self.ast(m);
        // Discriminated (`switch (x.kind)` on a union of records) or literal (`switch (x)` on literal unions):
        // a C switch on the tag. Anything else: an if-chain.
        self.push_temps();
        let (tag_expr, label_tag): (Option<String>, LabelTag) = {
            let mut result: (Option<String>, LabelTag) = (None, Box::new(|_, _| Vec::new()));
            if let ExprKind::Member { obj, name, optional: false, .. } = &ast.expr(disc).kind {
                let base = self.expr(*obj);
                let base = self.unfold_val(base);
                if let Ty::Union(ms) = self.tget(base.ty) {
                    let ms = self.c.types.tys(ms).to_vec();
                    let field = *name;
                    let mut map: Vec<(crate::intern::Sym, usize)> = Vec::new();
                    let mut ok = true;
                    for (i, &mt) in ms.iter().enumerate() {
                        let mt2 = self.c.unfold(mt);
                        match self.tget(mt2) {
                            Ty::Record(fs) => match self.c.types.fields(fs).iter().find(|f| f.name == field).map(|f| *self.c.types.get(f.ty)) {
                                Some(Ty::StrLit(l)) => map.push((l, i)),
                                _ => ok = false,
                            },
                            _ => ok = false,
                        }
                    }
                    if ok {
                        result = (Some(self.u_tag(&base.code, base.ty)), Box::new(move |_, l| map.iter().filter(|(s, _)| *s == l).map(|(_, i)| *i).collect()));
                    }
                }
            }
            if result.0.is_none() {
                let v = self.expr(disc);
                if let Ty::Union(ms) = self.tget(v.ty) {
                    let ms = self.c.types.tys(ms).to_vec();
                    if ms.iter().all(|&x| matches!(self.tget(x), Ty::StrLit(_))) {
                        let lits: Vec<(crate::intern::Sym, usize)> = ms.iter().enumerate().map(|(i, &x)| (if let Ty::StrLit(l) = self.tget(x) { l } else { unreachable!() }, i)).collect();
                        result = (Some(self.u_tag(&v.code, v.ty)), Box::new(move |_, l| lits.iter().filter(|(s, _)| *s == l).map(|(_, i)| *i).collect()));
                    }
                }
                // Every case an integer constant (a literal, an enum's member): a C switch
                if result.0.is_none() && self.ctype(v.ty) == "tv_int" && !cases.is_empty() {
                    let vals: Vec<Option<Option<i64>>> = cases.iter().map(|c| c.test.map(|t| self.case_int(t))).collect();
                    if vals.iter().all(|x| !matches!(x, Some(None))) {
                        let code = v.code.clone();
                        let dv = self.fresh("d");
                        self.line(format!("tv_int {dv} = {code};"));
                        self.pop_temps();
                        let end = self.fresh("swend");
                        let depth = self.b().scopes.len();
                        self.b().breaks.push((BreakTarget::Goto(end.clone()), depth));
                        self.open(&format!("switch ({dv}) {{"));
                        let mut seen: Vec<i64> = Vec::new();
                        for (case, val) in cases.iter().zip(&vals) {
                            match val {
                                Some(Some(x)) => {
                                    if !seen.contains(x) {
                                        seen.push(*x);
                                        self.line(format!("case INT64_C({x}):"));
                                    }
                                }
                                _ => self.line("default:"),
                            }
                            if !case.body.is_empty() {
                                self.open("{");
                                self.push_scope();
                                for &x in &case.body {
                                    self.stmt(x);
                                }
                                self.pop_scope();
                                self.close("}");
                                self.line(format!("goto {end};"));
                            }
                        }
                        self.close("}");
                        self.b().breaks.pop();
                        self.line(format!("{end}:;"));
                        return;
                    }
                }
                // Every case a string literal: matched first, the value borrowed (nothing in
                // the matching can release it, and no case looks at it again)
                if result.0.is_none()
                    && self.ctype(v.ty) == "tv_str"
                    && cases.iter().all(|c| c.test.is_none_or(|t| matches!(ast.expr(t).kind, ExprKind::Str(_))))
                {
                    let target = self.literal_match(&v, cases);
                    self.pop_temps();
                    self.run_cases(&target, cases);
                    return;
                }
                if result.0.is_none() {
                    // General: keep the value for the if-chain.
                    // The discriminant owns one reference for the whole switch and is released with the enclosing scope (so `return` in a case
                    // releases it too). It used to be an `own`ed temporary released by `pop_temps` AND released again after the chain: a string
                    // switched over was freed twice whenever control left through the end of the switch.
                    let ty = v.ty;
                    let code = self.consume(v);
                    let ct = self.ctype(ty);
                    let dv = self.fresh("d");
                    self.line(format!("{ct} {dv} = {code};"));
                    self.pop_temps();
                    if self.is_rc(ty) {
                        let rel = format!("{};", self.release_code(ty, &dv));
                        self.b().scopes.last_mut().unwrap().releases.push(rel);
                    }
                    self.if_chain_switch(&dv, ty, cases, false);
                    return;
                }
            }
            result
        };
        let tag_expr = tag_expr.unwrap();
        let tv = self.fresh("tag");
        self.line(format!("uint32_t {tv} = {tag_expr};"));
        self.pop_temps();
        self.open(&format!("switch ({tv}) {{"));
        let depth = self.b().scopes.len();
        self.b().breaks.push((BreakTarget::Native, depth));
        // (every variant of the case's discriminant, `case 'html'` of two kinds of token: each of
        // their tags, but one an earlier case took)
        let mut taken: Vec<usize> = Vec::new();
        let mut dummy = 0u32;
        for case in cases {
            match case.test {
                Some(t) => {
                    // (a string enum's member: its value, as the checker took it)
                    let lit = match &ast.expr(t).kind {
                        ExprKind::Str(l) => Some(*l),
                        _ => self.enum_str_member(m, t),
                    };
                    let tags: Vec<usize> = lit.map(|l| label_tag(self, l)).unwrap_or_default().into_iter().filter(|i| !taken.contains(i)).collect();
                    if tags.is_empty() {
                        dummy += 1;
                        self.line(format!("case {}u:", 0xFFFF_FFFFu32 - dummy));
                    }
                    for i in tags {
                        taken.push(i);
                        self.line(format!("case {i}:"));
                    }
                }
                None => self.line("default:"),
            }
            if !case.body.is_empty() {
                self.open("{");
                self.push_scope();
                for &x in &case.body {
                    self.stmt(x);
                }
                self.pop_scope();
                self.close("}");
                self.line("break;");
            }
        }
        self.b().breaks.pop();
        self.close("}");
    }

    /// A `case` test that's an integer constant: a literal, its negation, an enum's member.
    fn case_int(&mut self, t: ExprId) -> Option<i64> {
        let m = self.cur_m();
        let (cm, e) = match self.const_member(m, t) {
            Some(x) => x,
            None => (m, t),
        };
        let ast = self.ast(cm);
        let mut e = e;
        while let ExprKind::Paren(x) = ast.expr(e).kind {
            e = x;
        }
        match &ast.expr(e).kind {
            ExprKind::Int(v) if *v <= i64::MAX as u64 => Some(*v as i64),
            ExprKind::Unary(crate::ast::UnOp::Neg, x) => match ast.expr(*x).kind {
                ExprKind::Int(v) if v <= i64::MAX as u64 => Some(-(v as i64)),
                _ => None,
            },
            _ => None,
        }
    }

    /// The index of the case string `v` matches (or the default's, else -1), every case's
    /// test a string literal: a switch on its byte when they're all one ASCII character.
    fn literal_match(&mut self, v: &Val, cases: &[ast::Case]) -> String {
        let m = self.cur_m();
        let ast = self.ast(m);
        let target = self.fresh("sw");
        self.line(format!("int {target} = -1;"));
        let lits: Vec<Option<String>> = cases.iter().map(|c| c.test.map(|t| match &ast.expr(t).kind {
            ExprKind::Str(l) => self.sym(*l).to_string(),
            _ => unreachable!(),
        })).collect();
        let one_byte = lits.iter().flatten().all(|l| l.len() == 1 && l.as_bytes()[0] < 0x80);
        let d = &v.code;
        if one_byte {
            self.open(&format!("if (({d}).p != NULL && ({d}).p->len == 1) switch ((unsigned char)({d}).p->data[0]) {{"));
            let mut seen = Vec::new();
            for (i, l) in lits.iter().enumerate() {
                if let Some(l) = l
                    && !seen.contains(l)
                {
                    seen.push(l.clone());
                    self.line(format!("case {}: {target} = {i}; break;", l.as_bytes()[0]));
                }
            }
            self.close("}");
        } else {
            for (i, l) in lits.iter().enumerate() {
                if let Some(l) = l {
                    let lit = self.lit(l.as_bytes());
                    self.line(format!("if ({target} < 0 && ({d}).p != NULL && tv_str_eq({d}, {lit})) {target} = {i};"));
                }
            }
        }
        if let Some(di) = lits.iter().position(|l| l.is_none()) {
            self.line(format!("if ({target} < 0) {target} = {di};"));
        }
        target
    }

    /// The cases' bodies, from the one numbered `target` on (see `if_chain_switch`).
    fn run_cases(&mut self, target: &str, cases: &[ast::Case]) {
        let end = self.fresh("swend");
        let depth = self.b().scopes.len();
        self.b().breaks.push((BreakTarget::Goto(end.clone()), depth));
        self.open(&format!("switch ({target}) {{"));
        for (i, case) in cases.iter().enumerate() {
            self.line(format!("case {i}:"));
            if !case.body.is_empty() {
                self.open("{");
                self.push_scope();
                for &x in &case.body {
                    self.stmt(x);
                }
                self.pop_scope();
                self.close("}");
                self.line(format!("goto {end};"));
            }
        }
        self.line("default: break;");
        self.close("}");
        self.b().breaks.pop();
        self.line(format!("{end}:;"));
    }

    fn if_chain_switch(&mut self, dv: &str, dty: TyId, cases: &[ast::Case], owned: bool) {
        let end = self.fresh("swend");
        let depth = self.b().scopes.len();
        self.b().breaks.push((BreakTarget::Goto(end.clone()), depth));
        // Match: find the first matching case (or default), then run bodies from there (grouped labels).
        let target = self.fresh("sw");
        self.line(format!("int {target} = -1;"));
        let mut default_idx = None;
        for (i, case) in cases.iter().enumerate() {
            match case.test {
                Some(t) => {
                    self.push_temps();
                    let v = self.expr(t);
                    let eq = self.equality(Val::plain(dv, dty), v, false);
                    self.line(format!("if ({target} < 0 && ({eq})) {target} = {i};"));
                    self.pop_temps();
                }
                None => default_idx = Some(i),
            }
        }
        if let Some(d) = default_idx {
            self.line(format!("if ({target} < 0) {target} = {d};"));
        }
        self.open(&format!("switch ({target}) {{"));
        for (i, case) in cases.iter().enumerate() {
            self.line(format!("case {i}:"));
            if !case.body.is_empty() {
                self.open("{");
                self.push_scope();
                for &x in &case.body {
                    self.stmt(x);
                }
                self.pop_scope();
                self.close("}");
                self.line(format!("goto {end};"));
            }
        }
        self.line("default: break;");
        self.close("}");
        self.b().breaks.pop();
        self.line(format!("{end}:;"));
        if owned && self.is_rc(dty) {
            let r = self.release_code(dty, dv);
            self.line(format!("{r};"));
        }
    }

    // ------------------------------------------------------------ expressions

    /// Emits an expression; the value has the checker's (instantiated) type for it.
    /// Appends a template literal's text to `sb`, given its already evaluated interpolations.
    pub(crate) fn template_into(&mut self, parts: &[String], vals: &[Val]) {
        for (i, part) in parts.iter().enumerate() {
            if !part.is_empty() {
                self.line(format!("tv_sb_push(sb, {}, {});", c_string(part.as_bytes()), part.len()));
            }
            if let Some(v) = vals.get(i) {
                let code = self.string_code(v.ty, &v.code);
                self.line(format!("{code};"));
            }
        }
    }

    pub(crate) fn expr(&mut self, e: ExprId) -> Val {
        // (the read in `p = a.f; a.f = null`: it takes the reference, see `unlink_pair`)
        if self.b().take == Some(e) {
            self.b().take = None;
            let v = self.expr(e);
            return Val { owned: true, ..v };
        }
        let m = self.cur_m();
        let ast = self.ast(m);
        let node = ast.expr(e);
        let span = node.span;
        let ty = self.ty(e);
        match &node.kind {
            ExprKind::Error => Val::plain("0", ERROR),
            ExprKind::Spread(_) => {
                self.unsupported(span, "`...` here");
                Val::plain("0", ty)
            }
            // `(a, b)`: a for its effects, then b's value
            ExprKind::Yield(v, delegate) => self.yield_expr(*v, *delegate, span),
            ExprKind::Seq(xs) => {
                let (last, init) = xs.split_last().expect("a sequence has expressions");
                for &x in init {
                    let v = self.expr(x);
                    if v.ty != VOID && !v.code.is_empty() && !v.owned {
                        self.line(format!("(void)({});", v.code));
                    }
                }
                let v = self.expr(*last);
                if ty == VOID { Val::plain("0", ty) } else { self.coerce(v, ty) }
            }
            ExprKind::Int(v) if ty == DECIMAL => Val::plain(self.dec_lit(&v.to_string()), DECIMAL),
            ExprKind::Float(_) if ty == DECIMAL => {
                let text = self.c.sm.get(span.file).text[span.start as usize..span.end as usize].to_string();
                Val::plain(self.dec_lit(&text), DECIMAL)
            }
            ExprKind::Decimal(s) => {
                let text = self.sym(*s).to_string();
                Val::plain(self.dec_lit(&text), DECIMAL)
            }
            ExprKind::Int(v) => {
                // (in TypeScript, past 2^53 the double nearest it, as JavaScript reads it)
                let v = if ty == INT && *v > 1 << 53 && self.js_ints(m) { (*v as f64) as u64 } else { *v };
                let code = match self.tget(ty) {
                    Ty::F64 | Ty::F32 => format!("{}.0", v),
                    Ty::U64 => format!("UINT64_C({v})"),
                    _ => format!("INT64_C({v})"),
                };
                let code = if matches!(self.tget(ty), Ty::F64 | Ty::F32 | Ty::U64 | Ty::Int) { code } else { format!("(({}){code})", self.ctype(ty)) };
                Val::plain(code, ty)
            }
            ExprKind::Float(f) => {
                let mut s = format!("{f:?}");
                if !s.contains('.') && !s.contains('e') && !s.contains("inf") && !s.contains("NaN") {
                    s.push_str(".0");
                }
                Val::plain(if ty == F32 { format!("{s}f") } else { s }, ty)
            }
            ExprKind::BigInt(s) => {
                let text = self.sym(*s).to_string();
                Val::plain(self.big_lit(&text), BIGINT)
            }
            ExprKind::Str(_) | ExprKind::Undefined | ExprKind::Null => Val::plain("0", ty),
            ExprKind::Bool(b) => Val::plain(if *b { "true" } else { "false" }, BOOL),
            ExprKind::Template(parts, exprs) => {
                let vals: Vec<Val> = self.exprs_in_order(exprs);
                let sbv = self.fresh("sb");
                // (room for it all at once: the literal text, the strings' bytes, a number's worth
                // for anything else; the builder never grows and copies on the way)
                let lit: usize = parts.iter().map(|p| p.len()).sum();
                let mut size = format!("{}", lit + 24 * vals.iter().filter(|v| !matches!(self.tget(v.ty), Ty::Str)).count());
                for v in vals.iter().filter(|v| matches!(self.tget(v.ty), Ty::Str)) {
                    size.push_str(&format!(" + (size_t)tv_str_byte_len({})", v.code));
                }
                let stack = self.stack_builder(&sbv);
                if !vals.is_empty() {
                    if stack.is_some() {
                        self.line(format!("if ({sbv}.cap < (size_t)({size})) tv_sb_grow(&{sbv}, {size});"));
                    } else {
                        self.line(format!("tv_sb_grow(&{sbv}, {size});"));
                    }
                }
                if stack.is_none() {
                    self.open("{");
                }
                self.line(format!("tv_sb *sb = &{sbv};"));
                self.template_into(parts, &vals);
                self.end_builder(&sbv, stack)
            }
            ExprKind::Ident(_) => self.ident(e, ty),
            ExprKind::Paren(x) => self.expr(*x),
            ExprKind::Unary(op, x) => self.unary(*op, *x, ty, span),
            ExprKind::Binary(op @ (BinOp::Add | BinOp::Sub), l, r) if ty == F64 && let Some(v) = self.exact_half_sum(e, *op, *l, *r) => v,
            ExprKind::Binary(op, l, r) => self.binary(*op, *l, *r, ty, span),
            ExprKind::Assign(op, t, v) => {
                self.assign_ty = Some(ty);
                self.assign(*op, *t, *v, span)
            }
            ExprKind::Update { inc, prefix, target } => {
                // (`o.n++` of an `any`, `x++` of one: a number, stored as an `any`)
                let any_obj = match ast.expr(*target).kind {
                    ExprKind::Member { obj, .. } | ExprKind::Index { obj, .. } if self.ty(obj) == ANY => Some(obj),
                    _ => None,
                };
                if any_obj.is_some() || self.ty(*target) == ANY && any_obj.is_none() && !matches!(ast.expr(*target).kind, ExprKind::Member { .. } | ExprKind::Index { .. }) {
                    return self.any_update(*target, *inc, *prefix, span);
                }
                let p = self.place(*target);
                // (a variable held as a union, narrowed to a number: stepped as that number)
                let nt = ty;
                if p.ty != nt && matches!(self.tget(p.ty), Ty::Union(_)) && self.c.types.is_numeric(nt) && !p.weak {
                    let lv = self.place_lvalue(&p);
                    let cur = self.project(Val::plain(lv.clone(), p.ty), nt);
                    let nct = self.ctype(nt);
                    let (old, new) = (self.fresh("o"), self.fresh("n"));
                    let delta = if *inc { "+ 1" } else { "- 1" };
                    self.line(format!("{nct} {old} = {}; {nct} {new} = {old} {delta};", cur.code));
                    let stored = self.coerce(Val::plain(new.clone(), nt), p.ty);
                    self.line(format!("{lv} = {};", stored.code));
                    return Val::plain(if *prefix { new } else { old }, nt);
                }
                let ct = self.ctype(p.ty);
                let loc = self.loc(span);
                let lv = self.place_lvalue(&p);
                if p.ty == BIGINT {
                    let f = if *inc { "tv_big_add_to" } else { "tv_big_sub_to" };
                    if *prefix {
                        self.line(format!("{f}(&{lv}, ((tv_big){{3}}));"));
                        return Val::plain(lv, BIGINT);
                    }
                    let old = self.tmp(BIGINT, &lv, false);
                    self.line(format!("tv_big_retain({});", old.code));
                    let old = Val { owned: true, ..old };
                    self.b().temps.last_mut().unwrap().push((old.code.clone(), BIGINT));
                    self.line(format!("{f}(&{lv}, ((tv_big){{3}}));"));
                    return old;
                }
                let old = self.fresh("o");
                self.line(format!("{ct} {old} = {lv};"));
                if p.ty == DECIMAL {
                    let f = if *inc { "tv_dec_add" } else { "tv_dec_sub" };
                    self.line(format!("{lv} = {f}({old}, tv_dec_from_int(1));"));
                    return Val::plain(if *prefix { lv } else { old }, p.ty);
                }
                // (a typed array's element: a number, wrapped or clamped as it's stored back)
                let elem_of = match self.ast(m).expr(*target).kind {
                    ExprKind::Index { obj, .. } => Some(obj),
                    _ => None,
                };
                if crate::check::promoted_ty(p.ty) == INT && p.ty != INT && elem_of.is_some_and(|o| { let ot = self.ty(o); matches!(self.tget(ot), Ty::Array(_)) }) {
                    let n = Val::plain(format!("((tv_int){old} {} 1)", if *inc { "+" } else { "-" }), INT);
                    let n = self.coerce(n, p.ty);
                    self.line(format!("{lv} = {};", n.code));
                    return Val::plain(if *prefix { lv } else { old }, p.ty);
                }
                // (in TypeScript, never past 2^53 either: see TV_JADD)
                let limit: i128 = if self.js_ints(m) { 1 << 53 } else { i64::MAX as i128 };
                let nowrap = matches!(self.ident_fact(m, *target), Some(IdentFact::Local(k)) if Some(k) == self.b().nowrap_step || self.b().nowrap_locals.contains(&k))
                    || (p.ty == INT && matches!(self.int_range(*target), super::range::R::In(lo, hi) if if *inc { hi < limit } else { lo > -limit }));
                let new = if self.c.types.is_float(p.ty) || nowrap {
                    format!("{old} {} 1", if *inc { "+" } else { "-" })
                } else if p.ty == INT && self.js_ints(m) {
                    format!("{}({old}, 1, {loc})", if *inc { "TV_JADDK" } else { "TV_JSUBK" })
                } else {
                    format!("{}({ct}, {old}, 1, {loc})", if *inc { "TV_ADD" } else { "TV_SUB" })
                };
                self.line(format!("{lv} = {new};"));
                Val::plain(if *prefix { lv } else { old }, p.ty)
            }
            ExprKind::Call { .. } => self.call(e),
            ExprKind::New { callee, args, .. } => {
                if let Some(fact) = self.facts(m).calls.get(&e).cloned()
                    && let Callee::New(_) = fact.callee
                {
                    let params: Vec<FnParam> = fact.params.iter().map(|p| FnParam { ty: self.inst(p.ty), ..*p }).collect();
                    let cls = self.inst(fact.ret);
                    let v = self.new_object(e, cls, args, &params, span);
                    if self.facts(m).throwing.contains(&e) {
                        self.error_check();
                    }
                    return self.coerce(v, ty);
                }
                // `new Q<number>()` of a parameter given a class: that class (each call's own)
                if let Some(fact) = self.facts(m).calls.get(&e).cloned()
                    && let Callee::NewOf(p, sig_params) = &fact.callee
                {
                    let q = self.c.types.intern(Ty::Param(*p));
                    let qi = self.inst(q);
                    let Ty::ClassObj(c) = self.tget(qi) else {
                        self.unsupported(span, "`new` of this constructor");
                        return Val::plain("0", ty);
                    };
                    // (its type arguments: the constructor type's, as this call binds them)
                    let targs: Vec<TyId> = sig_params.iter().map(|sp| fact.targs.iter().find(|(x, _)| x == sp).map(|&(_, t)| self.inst(t)).unwrap_or(ERROR)).collect();
                    let info = self.c.classes[c as usize].clone();
                    let map: FxMap<u32, TyId> = info.params.iter().copied().zip(targs.iter().copied()).collect();
                    let cls = self.c.types.class(c, &targs);
                    let params: Vec<FnParam> = info.ctor.params.iter().map(|p| FnParam { ty: self.c.types.subst(p.ty, &map), ..*p }).collect();
                    let v = self.new_object(e, cls, args, &params, span);
                    if self.facts(m).throwing.contains(&e) {
                        self.error_check();
                    }
                    return self.coerce(v, ty);
                }
                if let Some(fact) = self.facts(m).calls.get(&e)
                    && let Callee::NewPromise = fact.callee
                {
                    return self.new_promise(args, ty);
                }
                if let Some(fact) = self.facts(m).calls.get(&e)
                    && let Callee::JsNew = fact.callee
                {
                    if let Some(v) = self.js_try_fuse(e, ty, span) {
                        if self.facts(m).throwing.contains(&e) {
                            self.error_check();
                        }
                        return v;
                    }
                    let f = self.js_callee(*callee);
                    let v = self.js_call(f, None, args, true, ty);
                    if self.facts(m).throwing.contains(&e) {
                        self.error_check();
                    }
                    return v;
                }
                let name = match &ast.expr(*callee).kind {
                    ExprKind::Ident(s) => match self.sym(*s) {
                        "WeakMap" => "Map".to_string(),
                        "WeakSet" => "Set".to_string(),
                        n => n.to_string(),
                    },
                    _ => String::new(),
                };
                if name == "Array" && self.facts(self.cur_m()).filled_arrays.contains(&e) {
                    // (filled in order by the loop after it: see `filled_arrays`)
                    let Ty::Array(et) = self.tget(ty) else { unreachable!() };
                    let n = self.expr(args[0].expr);
                    let n = self.int_code(n);
                    let d = self.desc(et);
                    return self.tmp(ty, &format!("tv_arr_with_capacity({d}, {n})"), true);
                }
                if name == "Array" || crate::check::typed_array_elem(&name).is_some() {
                    return self.new_array(args, ty, span);
                }
                if name == "Map" && !args.is_empty() {
                    // entries `[key, value]`: set one by one
                    let Ty::Map(k, v) = self.tget(ty) else { return Val::plain("TV_EMPTY_MAP", ty) };
                    let arr = self.expr(args[0].expr);
                    // (another map: a copy of it)
                    let at = self.c.types.without_undefined(arr.ty);
                    if matches!(self.tget(at), Ty::Map(..)) {
                        let (kd, vd) = (self.desc(k), self.desc(v));
                        return self.tmp(ty, &format!("tv_map_copy({}, {kd}, {vd})", arr.code), true);
                    }
                    let Ty::Array(et) = self.tget(arr.ty) else { return Val::plain("TV_EMPTY_MAP", ty) };
                    let f = self.tuple_of(et).unwrap_or_default();
                    if f.len() != 2 {
                        return Val::plain("TV_EMPTY_MAP", ty);
                    }
                    let (f0, f1) = (self.sym(f[0].name).to_string(), self.sym(f[1].name).to_string());
                    let res = self.tmp(ty, "tv_map_fresh()", true);
                    let (kd, vd, kct, vct, ect) = (self.desc(k), self.desc(v), self.ctype(k), self.ctype(v), self.ctype(et));
                    let i = self.fresh("i");
                    self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", arr.code));
                    self.line(format!("{ect} *{i}e = &(({ect} *)tv_arr_data({}))[{i}];", arr.code));
                    let (kx, vx) = (Val::plain(self.fld(et, &format!("(*{i}e)"), &f0), f[0].ty), Val::plain(self.fld(et, &format!("(*{i}e)"), &f1), f[1].ty));
                    let (kx, vx) = (self.coerce(kx, k), self.coerce(vx, v));
                    let (rk, rv) = (self.retain_code(k, &format!("{i}k")), self.retain_code(v, &format!("{i}v")));
                    self.line(format!("{kct} {i}k = {}; {rk}; {vct} {i}v = {}; {rv};", kx.code, vx.code));
                    self.line(format!("tv_map_set(&{}, {kd}, {vd}, &{i}k, &{i}v);", res.code));
                    self.close("}");
                    return res;
                }
                if name == "Set" && !args.is_empty() {
                    let Ty::Set(elem) = self.tget(ty) else { return Val::plain("TV_EMPTY_MAP", ty) };
                    // (a string: its characters)
                    let arr = self.spread_array(args[0].expr);
                    let arr_ty = self.c.types.array(elem);
                    let arr = self.coerce(arr, arr_ty);
                    let res = self.tmp(ty, "tv_map_fresh()", true);
                    let (d, ect) = (self.desc(elem), self.ctype(elem));
                    let i = self.fresh("i");
                    self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", arr.code));
                    let r = self.retain_code(elem, &format!("{i}k"));
                    self.line(format!("{ect} {i}k = (({ect} *)tv_arr_data({}))[{i}]; {r};", arr.code));
                    self.line(format!("tv_map_set(&{}, {d}, &tv_type_undefined, &{i}k, NULL);", res.code));
                    self.close("}");
                    return res;
                }
                // (`new Map()`: a map of its own)
                if matches!(self.tget(ty), Ty::Map(..) | Ty::Set(_)) {
                    return self.tmp(ty, "tv_map_fresh()", true);
                }
                Val::plain("TV_EMPTY_MAP", ty)
            }
            ExprKind::Member { obj, name, optional, .. } => {
                // (an enum's member: its value, a constant the C compiler can see)
                if let Some((cm, v)) = self.const_member(m, e)
                    && cm == m
                    && !self.is_rc(ty)
                {
                    let lv = self.expr(v);
                    return self.coerce(lv, ty);
                }
                self.member(e, *obj, *name, *optional, ty, span)
            }
            // `xs[i]` read as the element type (TypeScript's): as `xs[i]!`
            ExprKind::Index { obj, index, optional: false } if !self.c.types.has_undefined(ty) && let Some(v) = self.element(*obj, *index, ty, span) => v,
            ExprKind::Index { obj, index, optional } => {
                let ot = self.ty(*obj);
                if self.c.types.without_undefined(ot) == JS
                    && let Some(v) = self.js_try_fuse(e, ty, span)
                {
                    if self.facts(self.cur_m()).throwing.contains(&e) {
                        self.error_check();
                    }
                    return v;
                }
                self.index(e, *obj, *index, *optional, ty, span)
            }
            ExprKind::Object(fields) => {
                let target = self.c.unfold(ty);
                // Checked against an interface: build a record with the interface's members, then wrap it.
                let target = if let Ty::Interface(..) = self.tget(target) {
                    let ms = self.iface_members(target);
                    let order: Vec<crate::intern::Sym> = ms.iter().map(|f| f.name).collect();
                    let rt = self.c.types.record(ms);
                    self.c.types.note_field_order(rt, order);
                    rt
                } else {
                    target
                };
                // A `Record<string, V>` literal: a map built key by key.
                if let Ty::Map(k, v) = self.tget(target) {
                    let (kd, vd) = (self.desc(k), self.desc(v));
                    let (kct, vct) = (self.ctype(k), self.ctype(v));
                    // (distinct keys: made at once, at its size)
                    // (values that can't throw or suspend: the arrays are never left half made)
                    let distinct = fields.iter().enumerate().all(|(i, f)| fields[..i].iter().all(|g| g.name != f.name));
                    let m = self.cur_m();
                    let calm = fields.iter().all(|f| {
                        let mut xs = Vec::new();
                        collect_exprs_expr(self.ast(m), f.value, &mut xs);
                        xs.push(f.value);
                        xs.iter().all(|&x| !self.facts(m).throwing.contains(&x) && !matches!(self.ast(m).expr(x).kind, ExprKind::Await(_) | ExprKind::Yield(..)))
                    });
                    // (not where locals live in a frame, an async function's or a generator's: no arrays there)
                    let framed = self.b().async_fn.is_some() || self.b().gen_elem.is_some();
                    let spreads = fields.iter().any(|f| matches!(self.ast(m).expr(f.value).kind, ExprKind::Spread(_)));
                    if k == STR && distinct && calm && !framed && !spreads && fields.len() >= 2 && !matches!(self.tget(v), Ty::Undefined | Ty::Null | Ty::Void) {
                        let (ka, va, ha) = (self.fresh("ks"), self.fresh("vs"), self.fresh("hs"));
                        let keys: Vec<String> = fields.iter().map(|f| self.lit(self.sym(f.name).to_string().as_bytes())).collect();
                        self.line(format!("{kct} {ka}[{}] = {{{}}}; {vct} {va}[{}]; static uint64_t {ha}[{}];", keys.len(), keys.join(", "), keys.len(), keys.len()));
                        for (i, f) in fields.iter().enumerate() {
                            let vv = self.expr(f.value);
                            let vv = self.coerce(vv, v);
                            let vc = self.consume(vv);
                            self.line(format!("{va}[{i}] = {vc};"));
                        }
                        let res = self.tmp(target, &format!("tv_map_from_entries({kd}, {vd}, {}, {ka}, {va}, true, {ha})", fields.len()), true);
                        return self.coerce(res, ty);
                    }
                    let res = self.tmp(target, "tv_map_fresh_object()", true);
                    for f in fields {
                        // (`...m`: its entries; `...r`: its fields; `undefined`: nothing)
                        if let ExprKind::Spread(x) = self.ast(m).expr(f.value).kind {
                            let sv = self.expr(x);
                            let st = self.c.types.without_undefined(sv.ty);
                            let present = if st != sv.ty { self.truthy_val(sv.clone()) } else { "true".to_string() };
                            self.open(&format!("if ({present}) {{"));
                            self.push_temps();
                            let pv = self.project(sv, st);
                            match self.tget(st) {
                                Ty::Map(sk, svt) => {
                                    let (skd, svd, sct) = (self.desc(sk), self.desc(svt), self.ctype(svt));
                                    let (it, kp, vp) = (self.fresh("i"), self.fresh("kp"), self.fresh("vp"));
                                    self.line(format!("tv_int {it} = 0; void *{kp}, *{vp};"));
                                    self.open(&format!("while (tv_map_next({}, {skd}, {svd}, &{it}, &{kp}, &{vp})) {{", pv.code));
                                    self.push_temps();
                                    let ev = self.coerce(Val { code: format!("(*({sct} *){vp})"), ty: svt, owned: false }, v);
                                    let ev = self.own(ev);
                                    let vc = self.consume(ev);
                                    let (kn, vn) = (self.fresh("k"), self.fresh("v"));
                                    self.line(format!("{kct} {kn} = *({kct} *){kp}; tv_str_retain({kn}); {vct} {vn} = {vc};"));
                                    self.line(format!("tv_map_set(&{}, {kd}, {vd}, &{kn}, &{vn});", res.code));
                                    self.pop_temps();
                                    self.close("}");
                                }
                                Ty::Record(_) => {
                                    for fl in self.c.types.fields_in_order(st) {
                                        let fname = self.sym(fl.name).to_string();
                                        let key = self.lit(fname.as_bytes());
                                        let fv = Val { code: self.fld(st, &pv.code, &fname), ty: fl.ty, owned: false };
                                        let fv = self.coerce(fv, v);
                                        let fv = self.own(fv);
                                        let vc = self.consume(fv);
                                        let (kn, vn) = (self.fresh("k"), self.fresh("v"));
                                        self.line(format!("{kct} {kn} = {key}; {vct} {vn} = {vc};"));
                                        self.line(format!("tv_map_set(&{}, {kd}, {vd}, &{kn}, &{vn});", res.code));
                                    }
                                }
                                _ => self.unsupported(span, "this spread"),
                            }
                            self.pop_temps();
                            self.close("}");
                            continue;
                        }
                        let key = self.lit(self.sym(f.name).to_string().as_bytes());
                        let vv = self.expr(f.value);
                        let vv = self.coerce(vv, v);
                        let vc = self.consume(vv);
                        let (kn, vn) = (self.fresh("k"), self.fresh("v"));
                        self.line(format!("{kct} {kn} = {key}; {vct} {vn} = {vc};"));
                        self.line(format!("tv_map_set(&{}, {kd}, {vd}, &{kn}, &{vn});", res.code));
                    }
                    return self.coerce(res, ty);
                }
                let Ty::Record(fs) = self.tget(target) else {
                    self.unsupported(span, "an object literal of this type");
                    return Val::plain("0", ty);
                };
                let tfields = self.c.types.fields(fs).to_vec();
                let mut inits: Vec<(crate::intern::Sym, String)> = Vec::new();
                for f in fields {
                    let fty = tfields.iter().find(|x| x.name == f.name).map(|x| x.ty).unwrap_or(ERROR);
                    let v = self.expr(f.value);
                    let v = self.coerce(v, fty);
                    let code = self.consume(v);
                    inits.push((f.name, code));
                }
                for tf in &tfields {
                    if !inits.iter().any(|(n, _)| *n == tf.name) {
                        let v = self.coerce(Val::plain("0", UNDEFINED), tf.ty);
                        let code = self.consume(v);
                        inits.push((tf.name, code));
                    }
                }
                let parts: Vec<String> = inits.iter().map(|(n, c)| format!(".f_{} = {c}", self.sym(*n))).collect();
                let lit = self.rec_lit(target, &parts.join(", "));
                let v = self.tmp(target, &lit, true);
                if target != ty {
                    return self.coerce(v, ty);
                }
                v
            }
            ExprKind::Array(elems) if self.tuple_of(ty).is_some() => {
                // `[a, b]` as a tuple: the record of its elements
                let fields = self.tuple_of(ty).unwrap();
                let mut parts = Vec::new();
                for (&x, f) in elems.iter().zip(&fields) {
                    let v = self.expr(x);
                    let v = self.coerce(v, f.ty);
                    let code = self.consume(v);
                    parts.push(format!(".f_{} = {code}", self.sym(f.name)));
                }
                let lit = self.rec_lit(ty, &parts.join(", "));
                self.tmp(ty, &lit, true)
            }
            ExprKind::Array(elems) => {
                let Ty::Array(et) = self.tget(ty) else {
                    self.unsupported(span, "an array literal of this type");
                    return Val::plain("TV_EMPTY_ARR", ty);
                };
                // (an array of its own: `[]` given to two variables is two arrays)
                if elems.is_empty() {
                    return self.tmp(ty, "tv_arr_fresh()", true);
                }
                if elems.iter().any(|&x| matches!(self.ast(m).expr(x).kind, ExprKind::Spread(_))) {
                    return self.build_spread(elems, ty);
                }
                // Evaluate the elements, then write them straight into reserved space: the length
                // is a constant the C compiler can see.
                let d = self.desc(et);
                let ect = self.ctype(et);
                let mut codes = Vec::new();
                for &x in elems {
                    let v = self.expr(x);
                    let v = self.coerce(v, et);
                    let code = self.consume(v);
                    let n = self.fresh("e");
                    self.line(format!("{ect} {n} = {code};"));
                    codes.push(n);
                }
                // (allocated inline: the C compiler sees the length, a constant)
                let _ = d;
                let arr = self.tmp(ty, &format!("tv_arr_new_len(sizeof({ect}), {})", codes.len()), true);
                let w = self.fresh("w");
                self.line(format!("{ect} *{w} = ({ect} *)tv_arr_data({});", arr.code));
                for (i, c) in codes.iter().enumerate() {
                    self.line(format!("{w}[{i}] = {c};"));
                }
                arr
            }
            ExprKind::Arrow(_) => self.closure_value(e, ty),
            ExprKind::Cond(c, a, b) => {
                let cond = self.cond(*c);
                let res = self.fresh("t");
                let ct = self.ctype(ty);
                self.line(format!("{ct} {res};"));
                self.open(&format!("if ({cond}) {{"));
                self.push_temps();
                let va = self.expr(*a);
                let va = self.coerce(va, ty);
                let code = self.consume(va);
                self.line(format!("{res} = {code};"));
                self.pop_temps();
                self.close("} else {");
                self.b().indent += 1;
                self.push_temps();
                let vb = self.expr(*b);
                let vb = self.coerce(vb, ty);
                let code = self.consume(vb);
                self.line(format!("{res} = {code};"));
                self.pop_temps();
                self.close("}");
                let owned = self.is_rc(ty);
                if owned {
                    self.b().temps.last_mut().unwrap().push((res.clone(), ty));
                }
                Val { code: res, ty, owned }
            }
            ExprKind::As(x, _) => {
                let v = self.expr(*x);
                if v.ty == JS && ty != JS {
                    return self.tov_of_js(v, ty, span);
                }
                self.downcast(v, ty, span)
            }
            ExprKind::NonNull(x) => {
                // `xs[i]!`: a direct bounds-checked read.
                if let ExprKind::Index { obj, index, optional: false } = &ast.expr(*x).kind {
                    // (of `(T | undefined)[]`, `(T | null)[]`: the element, checked to be a `T`)
                    let ot = self.ty(*obj);
                    let want = match self.tget(ot) {
                        Ty::Array(el) if el != ty && self.c.types.has_nullish(el) => el,
                        _ => ty,
                    };
                    if let Some(v) = self.element(*obj, *index, want, span) {
                        return if v.ty == ty { v } else { self.downcast(v, ty, span) };
                    }
                }
                let v = self.expr(*x);
                self.downcast(v, ty, span)
            }
            ExprKind::Typeof(x) => {
                let v = self.expr(*x);
                self.typeof_str(v)
            }
            ExprKind::This => self.ident(e, ty),
            ExprKind::Try(x) => self.expr(*x),
            ExprKind::Await(x) => self.await_expr(e, *x),
            ExprKind::Super => Val::plain("0", ty),
        }
    }

    fn is_last_use(&mut self, e: ExprId) -> bool {
        if self.b().moves.is_none() {
            let m = self.cur_m();
            let (root, plain) = (self.b().root, self.b().async_fn.is_none() && self.b().gen_elem.is_none());
            let owned: Vec<u32> = self.b().owned_locals.iter().copied().collect();
            let moves = match root {
                Some(root) if plain => self.last_use_moves(m, root, &owned),
                _ => FxSet::default(),
            };
            self.b().moves = Some(moves);
        }
        self.b().moves.as_ref().is_some_and(|mv| mv.contains(&e))
    }

    fn ident(&mut self, e: ExprId, ty: TyId) -> Val {
        let m = self.cur_m();
        match self.ident_fact(m, e) {
            Some(IdentFact::Local(k)) => {
                let Some(local) = self.b().locals.get(&k).cloned() else {
                    let span = self.expr_span(e);
                    self.unsupported(span, "this variable reference");
                    return Val::plain("0", ty);
                };
                if local.ty == ty && self.b().owned_once.remove(&k) {
                    return Val { code: local.access, ty, owned: true };
                }
                // (its last use, handing it over: moved, the local left empty)
                let array = matches!(self.tget(ty), Ty::Array(_)) || self.niche_arr(ty);
                // (narrowed, `C | null` read as `C`: the same pointer)
                let same = local.ty == ty || self.niche(local.ty).is_some() && !self.niche_str(local.ty) && self.ctype(local.ty) == self.ctype(ty);
                let big = ty == BIGINT && local.ty == BIGINT;
                if same && self.b().owned_locals.contains(&k) && self.is_last_use(e) && (matches!(self.tget(ty), Ty::Class(..)) || self.niche(ty).is_some() && !self.niche_str(ty) || array || big) {
                    // (an array's release passes over a NULL header, as a class's over NULL, and
                    // a bigint's over a small integer)
                    let (ct, t) = (self.ctype(ty), self.fresh("mv"));
                    let none = if array { "(tv_arr){NULL}" } else if big { "((tv_big){1})" } else { "NULL" };
                    self.line(format!("{ct} {t} = {0}; {0} = {none};", local.access));
                    return Val { code: t, ty, owned: true };
                }
                self.project(Val::plain(local.access, local.ty), ty)
            }
            // (a class given where a constructor is wanted: nothing at run time)
            Some(IdentFact::Class(_)) if matches!(self.tget(ty), Ty::ClassObj(_) | Ty::Param(_)) => Val::plain("0", ty),
            Some(IdentFact::Fn(fm, fi)) => {
                // (a generic one: the instance where it's used fixes, see `value_targs`)
                let subst: FxMap<u32, TyId> = self.facts(m).value_targs.get(&e).map(|ts| ts.iter().map(|&(p, t)| (p, t)).collect()).unwrap_or_default();
                let subst: FxMap<u32, TyId> = subst.into_iter().map(|(p, t)| (p, self.inst(t))).collect();
                let code = self.fn_value_with(fm, fi, subst);
                Val::plain(code, ty)
            }
            Some(IdentFact::Const(cm, ci)) => {
                let v = self.const_ref(cm, ci);
                self.project(v, ty)
            }
            Some(IdentFact::Npm(nm, name)) => {
                let span = self.expr_span(e);
                self.npm_value(nm, name, span, ty)
            }
            Some(IdentFact::Builtin) if ty == F64 => {
                let ExprKind::Ident(s) = self.ast(m).expr(e).kind else { return Val::plain("0", ty) };
                Val::plain(if self.sym(s) == "NaN" { "((double)NAN)" } else { "((double)INFINITY)" }, F64)
            }
            _ => Val::plain("0", ty),
        }
    }

    /// Unfolds a recursive-alias value to its underlying (borrowed) value.
    pub(crate) fn unfold_val(&mut self, v: Val) -> Val {
        if matches!(self.tget(v.ty), Ty::Rec(..)) {
            let inner = self.c.unfold(v.ty);
            return Val { code: format!("({})->v", v.code), ty: inner, owned: false };
        }
        v
    }

    /// Views a value of a declared type as a narrower (flow-narrowed) type.
    pub(crate) fn project(&mut self, v: Val, to: TyId) -> Val {
        // (narrowed to `never`: code that can't run)
        if v.ty == to || to == ERROR || to == NEVER || self.held(v.ty) == self.held(to) {
            self.checked_project = None;
            return v;
        }
        // (a narrowing a call came after: checked, see `asserted_reads`)
        if let Some(loc) = self.checked_project.take()
            && let Ty::Union(ms) = self.tget(v.ty)
        {
            let ms = self.c.types.tys(ms).to_vec();
            let targets = self.members(to);
            let tests: Vec<String> = ms.iter().enumerate().filter(|(_, m)| targets.contains(m)).map(|(i, _)| self.u_is(&v.code, v.ty, i)).collect();
            if !tests.is_empty() {
                let msg = super::c_string(format!("the value changed since it was checked (a call in between): it's no longer `{}`", self.c.show(to)).as_bytes());
                self.line(format!("if (!({})) tv_trap({msg}, {loc});", tests.join(" || ")));
            }
        }
        self.checked_project = None;
        // A string narrowed to a literal (`if (path === "/")`): the value is known.
        if matches!(self.tget(to), Ty::StrLit(_)) && matches!(self.tget(v.ty), Ty::Str) {
            let d = self.default_value(to);
            return Val::plain(d, to);
        }
        // Narrowed by `instanceof`: a class viewed as a subclass (or a union member downcast).
        if let Ty::Class(tc, _) = self.tget(to) {
            match self.tget(v.ty) {
                Ty::Class(..) => return self.class_cast(v, to),
                Ty::Union(ms) => {
                    let ms = self.c.types.tys(ms).to_vec();
                    if let Some(k) = ms.iter().position(|&m| matches!(self.tget(m), Ty::Class(mc, _) if self.c.class_descends(tc, mc))) {
                        let payload = Val { code: self.u_payload(&v.code, v.ty, k), ty: ms[k], owned: false };
                        return self.class_cast(payload, to);
                    }
                }
                _ => {}
            }
        }
        if matches!(self.tget(v.ty), Ty::Rec(..)) {
            let u = self.unfold_val(v);
            return self.project(u, to);
        }
        if let Ty::Union(_) = self.tget(v.ty) {
            if let Some(k) = self.tag_of(v.ty, to) {
                if self.is_unit(to) {
                    return Val::plain("0", to);
                }
                return Val { code: self.u_payload(&v.code, v.ty, k), ty: to, owned: false };
            }
            // Narrowed to a member the union holds boxed, as a recursive alias (`Split` in
            // `type Layout = Leaf | Split` when `Split` mentions `Layout`): unbox it.
            if let Some((k, m)) = self.boxed_member(v.ty, to) {
                let payload = Val { code: self.u_payload(&v.code, v.ty, k), ty: m, owned: false };
                return self.unfold_val(payload);
            }
            // Narrowed to what one member holds, a recursive alias kept as one member (`Val | F |
            // undefined` narrowed to `Fn`, of `Val`): that member's, projected again.
            if let Ty::Union(fms) = self.tget(v.ty) {
                let fms = self.c.types.tys(fms).to_vec();
                if let Some(k) = fms.iter().position(|&m| m != to && matches!(self.tget(m), Ty::Rec(..) | Ty::Union(_)) && self.c.assignable_pub(to, m) && !self.c.assignable_pub(m, to)) {
                    let payload = Val { code: self.u_payload(&v.code, v.ty, k), ty: fms[k], owned: false };
                    return self.project(payload, to);
                }
            }
            // Narrowed to a smaller union: re-tag (members outside the target can't occur here).
            if let Ty::Union(fms) = self.tget(v.ty)
                && let Ty::Union(_) = self.tget(to)
            {
                let fms = self.c.types.tys(fms).to_vec();
                return self.retag(v, &fms, to);
            }
            return self.coerce(v, to);
        }
        self.coerce(v, to)
    }

    // ------------------------------------------------------------ conversions

    /// Converts `v` to type `to` (widening, union injection, sub-union retagging, boxing).
    pub(crate) fn coerce(&mut self, v: Val, to: TyId) -> Val {
        let from = v.ty;
        if from != to && self.held(from) == self.held(to) {
            return Val { ty: to, ..v };
        }
        if from == to || !(matches!(self.tget(from), Ty::Rec(..)) || matches!(self.tget(to), Ty::Rec(..))) {
            return self.coerce_inner(v, to);
        }
        // (a recursive type into another, `Tree` into `TreeNode`: met again inside itself, a
        // function of its own, which calls itself for the parts)
        if self.coerce_stack.contains(&(from, to)) {
            let name = self.rec_converter(from, to);
            let arg = if v.owned { self.tmp(from, &v.code, true).code } else { v.code };
            return self.tmp(to, &format!("{name}({arg})"), true);
        }
        self.coerce_stack.push((from, to));
        let r = self.coerce_inner(v, to);
        self.coerce_stack.pop();
        r
    }

    /// The function converting a (borrowed) `from` into an owned `to` (see `coerce`).
    fn rec_converter(&mut self, from: TyId, to: TyId) -> String {
        if let Some(n) = self.rec_cvs.get(&(from, to)) {
            return n.clone();
        }
        let name = format!("tvg_cv_{}", self.rec_cvs.len());
        self.rec_cvs.insert((from, to), name.clone());
        let proto = format!("{} {name}({} p0)", self.ctype(to), self.ctype(from));
        let _ = writeln!(self.protos, "static {proto};");
        let m = self.cur_m();
        let saved = std::mem::replace(&mut self.coerce_stack, vec![(from, to)]);
        self.begin_scratch(m);
        let r = self.coerce_inner(Val::plain("p0", from), to);
        self.scratch_return(r, to);
        let body = self.end_scratch();
        self.coerce_stack = saved;
        let _ = writeln!(self.helpers_after_decl, "static {proto} {{\n{body}}}\n");
        name
    }

    fn coerce_inner(&mut self, v: Val, to: TyId) -> Val {
        let from = v.ty;
        // (to `never`: code that can't run, as a `default` after every case)
        if to == NEVER && from != NEVER {
            let d = self.default_value(NEVER);
            return Val::plain(d, NEVER);
        }
        if from == NEVER && to != VOID && to != NEVER && to != ERROR {
            // (no value ever comes: `await` of a promise that only rejects; a valid one of `to`
            // stands in, after what's evaluated)
            let d = self.default_value(to);
            let code = if v.code == "0" { d } else { format!("({{ (void)({}); {d}; }})", v.code) };
            return Val::plain(code, to);
        }
        if from == to || to == ERROR || from == ERROR || from == NEVER || (self.tget(from) == Ty::Bool && self.tget(to) == Ty::Bool) {
            return Val { ty: to, ..v };
        }
        if to == JS {
            return self.js_of(v);
        }
        // (`any` is held as `unknown` is)
        if (from == ANY && to == UNKNOWN) || (from == UNKNOWN && to == ANY) {
            return Val { ty: to, ..v };
        }
        if to == ANY {
            let b = self.box_dyn(v);
            return Val { ty: ANY, ..b };
        }
        if from == ANY {
            let loc = self.any_loc();
            return self.any_to(v, to, &loc);
        }
        if to == UNKNOWN {
            return self.box_dyn(v);
        }
        if from == UNKNOWN {
            return self.unbox_dyn(v, to);
        }
        // (an array where something iterable is wanted: a generator over it)
        if let (Ty::Array(e), Ty::Gen(_)) = (self.tget(from), self.tget(to)) {
            let v = self.own(v);
            let code = self.consume(v);
            let d = self.desc(e);
            return self.tmp(to, &format!("tv_gen_of_arr({code}, {d})"), true);
        }
        // (a typed array's element into a union of numbers: `bytes[i]` read as `int | undefined`)
        if matches!(self.tget(from), Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32)
            && matches!(self.tget(to), Ty::Union(_))
            && self.tag_of(to, from).is_none()
            && self.tag_of(to, INT).is_some()
        {
            let w = Val::plain(format!("((tv_int)({}))", v.code), INT);
            return self.coerce_inner(w, to);
        }
        let (tf, tt) = (self.tget(from), self.tget(to));
        match (tf, tt) {
            (Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64, Ty::F64) => return Val::plain(format!("((double)({}))", v.code), to),
            (Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64 | Ty::F64, Ty::F32) => return Val::plain(format!("((float)({}))", v.code), to),
            (Ty::F32, Ty::F64) => return Val::plain(format!("((double)({}))", v.code), to),
            // (a number passed for a built-in's integer: truncated, NaN is 0)
            (Ty::F64 | Ty::F32, Ty::Int) => return Val::plain(format!("tv_f64_to_int_js({})", v.code), to),
            // (sized integers compute as `int`; a number stored into a sized element wraps, as a
            // typed array's does in JavaScript)
            (Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32, Ty::Int) => return Val::plain(format!("((tv_int)({}))", v.code), to),
            // (into a `Uint8ClampedArray`: clamped instead)
            (Ty::U8 | Ty::U8C, Ty::U8C) => return Val::plain(v.code, to),
            (Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U16 | Ty::U32, Ty::U8C) => return Val::plain(format!("tv_int_to_u8c((tv_int)({}))", v.code), to),
            (Ty::F64 | Ty::F32, Ty::U8C) => return Val::plain(format!("tv_f64_to_u8c({})", v.code), to),
            (Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32, Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32) => {
                let ct = self.ctype(to);
                return Val::plain(format!("(({ct})({}))", v.code), to);
            }
            (Ty::F64 | Ty::F32, Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32) => {
                let ct = self.ctype(to);
                return Val::plain(format!("(({ct})tv_f64_to_i32({}))", v.code), to);
            }
            (Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64, Ty::Decimal) => return Val::plain(format!("tv_dec_make((__int128)({}), 0)", v.code), to),
            (Ty::StrLit(s), Ty::Str) => {
                let text = self.sym(s).to_string();
                return Val::plain(self.lit(text.as_bytes()), STR);
            }
            (Ty::Undefined, Ty::Void) | (Ty::Void, Ty::Undefined) => return Val::plain("0", to),
            _ => {}
        }
        // Class to class (upcast): a pointer cast.
        if let (Ty::Class(..), Ty::Class(..)) = (tf, tt) {
            return self.class_cast(v, to);
        }
        // Into an interface value (a fat pointer).
        if let (Ty::Record(_) | Ty::Class(..) | Ty::Interface(..), Ty::Interface(..)) = (tf, tt)
            && let Some(iv) = self.wrap_iface(v.clone(), to)
        {
            return iv;
        }
        // Recursive aliases: box / unbox.
        if let Ty::Rec(..) = tt {
            let inner = self.c.unfold(to);
            let iv = self.coerce(v, inner);
            let code = self.consume(iv);
            let bn = self.box_name(to);
            let bv = self.fresh("bx");
            self.line(format!("{bn} *{bv} = tvg_alloc_small(sizeof({bn})); RC_{bn}({bv}) = 1; {bv}->v = {code};"));
            let owned = true;
            self.b().temps.last_mut().unwrap().push((bv.clone(), to));
            return Val { code: bv, ty: to, owned };
        }
        // Into a union.
        if let Ty::Union(_) = tt {
            let members = self.members(to);
            // Union → union: retag each member.
            if let Ty::Union(fms) = tf {
                let fms = self.c.types.tys(fms).to_vec();
                // (a member that's a union of its own, `Marked` of `Token`: its members, each to
                // its own, see `retag`)
                if fms.iter().all(|&fm| {
                    members.contains(&fm) || self.member_target(to, fm).is_some() || {
                        let u = self.c.unfold(fm);
                        matches!(self.tget(u), Ty::Union(_))
                    }
                }) {
                    return self.retag(v, &fms, to);
                }
            }
            // (an interface value into a union holding it and types that are it, `Generic` into
            // `Token`: the value it wraps, when that's one of them, as JavaScript's object is
            // the one it is)
            if let Ty::Interface(..) = tf
                && let Some(k) = members.iter().position(|&mt| mt == from)
                && !self.unwrapping.contains(&(from, to))
            {
                let m = self.cur_m();
                let mut cands = Vec::new();
                for s in self.c.flat_members(to) {
                    if s == from || !matches!(self.tget(s), Ty::Record(_) | Ty::Class(..) | Ty::Interface(..)) {
                        continue;
                    }
                    if let Some(itab) = self.itab_for(s, from, m) {
                        cands.push((s, itab));
                    }
                }
                if !cands.is_empty() {
                    self.unwrapping.push((from, to));
                    let v = self.own(v);
                    let ct = self.ctype(to);
                    let res = self.fresh("uw");
                    self.line(format!("{ct} {res};"));
                    for (i, (s, itab)) in cands.iter().enumerate() {
                        self.open(&format!("{}if (({}).t == (const tvg_itab *)&{itab}) {{", if i == 0 { "" } else { "} else " }, v.code));
                        self.push_temps();
                        let sct = self.ctype(*s);
                        let inner = if matches!(self.tget(*s), Ty::Class(..)) || self.is_obj(*s) {
                            format!("(({sct})({}).p)", v.code)
                        } else {
                            let bn = self.iface_box(*s);
                            format!("(({bn} *)({}).p)->v", v.code)
                        };
                        let cv = self.coerce(Val::plain(inner, *s), to);
                        let cv = self.own(cv);
                        let code = self.consume(cv);
                        self.line(format!("{res} = {code};"));
                        self.pop_temps();
                        self.b().indent -= 1;
                    }
                    self.line("} else {");
                    self.b().indent += 1;
                    let code = self.consume(v);
                    let u = self.u_make(to, k, Some(&code));
                    self.line(format!("{res} = {u};"));
                    self.b().indent -= 1;
                    self.line("}");
                    self.unwrapping.pop();
                    return self.tmp(to, &res, true);
                }
            }
            if let Some(k) = members.iter().position(|&mt| mt == from) {
                // (a niche union of the value's own type: the same pointer, borrowed as it was)
                if let Some((pi, _)) = self.niche(to)
                    && pi == k
                    && !v.owned
                {
                    return Val { ty: to, ..v };
                }
                if self.is_unit(from) {
                    let u = self.u_make(to, k, None);
                    // Holds nothing: owned, so consuming it retains nothing (and it needs no release).
                    let t = self.tmp(to, &u, false);
                    return Val { owned: true, ..t };
                }
                let code = self.consume(v);
                let u = self.u_make(to, k, Some(&code));
                return self.tmp(to, &u, true);
            }
            if let Some(mt) = self.member_target(to, from) {
                let mv = self.coerce(v, mt);
                return self.coerce(mv, to);
            }
        }
        // Out of a recursive alias (after union injection, so `Tree` → `Tree | undefined` doesn't unbox).
        if let Ty::Rec(..) = tf {
            let u = self.unfold_val(v);
            return self.coerce(u, to);
        }
        // Union of string literals → string.
        if let (Ty::Union(fms), Ty::Str) = (tf, tt)
            && self.niche(from).is_none()
        {
            let fms = self.c.types.tys(fms).to_vec();
            let res = self.fresh("s");
            self.line(format!("tv_str {res};"));
            self.open(&format!("switch (({}).tag) {{", v.code));
            for (i, &fm) in fms.iter().enumerate() {
                let s = match self.tget(fm) {
                    Ty::StrLit(l) => {
                        let text = self.sym(l).to_string();
                        self.lit(text.as_bytes())
                    }
                    Ty::Str => format!("({}).u.m{i}", v.code),
                    _ => "TV_EMPTY_STR".into(),
                };
                self.line(format!("case {i}: {res} = {s}; break;"));
            }
            self.line(format!("default: {res} = TV_EMPTY_STR; break;"));
            self.close("}");
            return Val::plain(res, STR);
        }
        // A union of classes → a class they all extend (`Error | DOMException` → `Error`): the
        // member it holds, as that class.
        if let (Ty::Union(fms), Ty::Class(..)) = (tf, tt) {
            let fms = self.c.types.tys(fms).to_vec();
            if fms.iter().all(|&m| matches!(self.tget(m), Ty::Class(..))) {
                let ct = self.ctype(to);
                let owned = v.owned;
                let code = if owned { self.consume(v) } else { v.code.clone() };
                let res = self.fresh("c");
                self.line(format!("{ct} {res};"));
                self.open(&format!("switch (({code}).tag) {{"));
                for i in 0..fms.len() {
                    self.line(format!("case {i}: {res} = ({ct})({code}).u.m{i}; break;"));
                }
                self.line(format!("default: {res} = NULL; break;"));
                self.close("}");
                return if owned { self.tmp(to, &res, true) } else { Val::plain(res, to) };
            }
        }
        // (a match as `string[]`: its elements, a group that took no part as "" where the array
        // can't hold `undefined`)
        if let (Ty::Class(..), Ty::Array(te)) = (tf, tt)
            && let Some(items) = self.c.interner.lookup("items")
            && let teb = self.c.types.without_undefined(te)
            && self.c.types.is_string(teb)
            && self.c.class_names.get(match tf { Ty::Class(d, _) => d as usize, _ => 0 }).is_some_and(|n| n == "RegExpExecArray")
            && let Some(it) = self.c.field_of_pub(from, items)
        {
            let span = self.stmt_span.unwrap_or(crate::source::Span::new(crate::source::FileId(0), 0, 0));
            let iv = self.class_field_read(v, items, it, span);
            if self.c.assignable_pub(it, to) {
                return self.coerce(iv, to);
            }
            let Ty::Array(ie) = self.tget(it) else { return iv };
            let (ict, ed) = (self.ctype(ie), self.desc(te));
            let res = self.tmp(to, &format!("tv_arr_with_capacity({ed}, tv_arr_len({}))", iv.code), true);
            let i = self.fresh("i");
            self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", iv.code));
            let e = Val::plain(format!("(({ict} *)tv_arr_data({}))[{i}]", iv.code), ie);
            let undef = self.tag_of(ie, UNDEFINED).map(|k| self.u_is(&e.code, ie, k)).unwrap_or_else(|| "false".into());
            let s = self.project(e, STR);
            self.line(format!("tv_str s_ = ({undef}) ? TV_EMPTY_STR : {}; tv_str_retain(s_); TVG_PUSH(tv_str, &{}, {ed}, s_);", s.code, res.code));
            self.close("}");
            return res;
        }
        // (a union where one of its members' type is wanted, each member convertible to it:
        // `Opts<any> | Opts<string>` as `Opts<string>`: the member it holds, converted)
        if let Ty::Union(fms) = tf
            && !matches!(tt, Ty::Union(_))
            && self.niche(from).is_none()
        {
            let fms = self.c.types.tys(fms).to_vec();
            if fms.iter().all(|&m| m != UNDEFINED && m != NULL && self.c.assignable_pub(m, to)) {
                let v = self.own(v);
                let ct = self.ctype(to);
                let res = self.fresh("cu");
                self.line(format!("{ct} {res};"));
                self.open(&format!("switch (({}).tag) {{", v.code));
                for (i, &fm) in fms.iter().enumerate() {
                    self.open(&format!("case {i}: {{"));
                    self.push_temps();
                    let payload = Val { code: self.u_payload(&v.code, from, i), ty: fm, owned: false };
                    let cv = self.coerce(payload, to);
                    let cv = self.own(cv);
                    let c = self.consume(cv);
                    self.line(format!("{res} = {c};"));
                    self.pop_temps();
                    self.line("break;");
                    self.close("}");
                }
                let dv = self.default_value(to);
                self.line(format!("default: {res} = {dv}; break;"));
                self.close("}");
                return self.tmp(to, &res, true);
            }
        }
        // Structural conversions between records / arrays with assignable parts.
        match (tf, tt) {
            (Ty::Record(ffs), Ty::Record(tfs)) => {
                let (ffs, tfs) = (self.c.types.fields(ffs).to_vec(), self.c.types.fields(tfs).to_vec());
                // (into one with an index signature, fields it doesn't name: kept in its `$rest`)
                let rest = self.c.interner.lookup("$rest");
                if rest.is_some_and(|r| tfs.iter().any(|f| f.name == r) && !ffs.iter().any(|f| f.name == r))
                    && ffs.iter().any(|ff| !tfs.iter().any(|tf| tf.name == ff.name))
                {
                    let v = self.own(v);
                    return self.reopen_as(&Val { owned: false, ..v }, to, None, None);
                }
                let mut parts = Vec::new();
                // (by name: a record with more fields is one with fewer, as TypeScript's are;
                // one it lacks is `undefined`)
                for tfld in &tfs {
                    let code = match ffs.iter().find(|ff| ff.name == tfld.name) {
                        Some(ff) => {
                            let fv = Val { code: self.fld(v.ty, &v.code, self.sym(ff.name)), ty: ff.ty, owned: false };
                            let cv = self.coerce(fv, tfld.ty);
                            self.consume(cv)
                        }
                        None => {
                            let cv = self.coerce(Val::plain("0", UNDEFINED), tfld.ty);
                            self.consume(cv)
                        }
                    };
                    parts.push(format!(".f_{} = {code}", self.sym(tfld.name)));
                }
                let lit = self.rec_lit(to, &parts.join(", "));
                return self.tmp(to, &lit, true);
            }
            (Ty::Array(fe), Ty::Array(te)) => {
                let d = self.desc(te);
                let (fct, tct) = (self.ctype(fe), self.ctype(te));
                let res = self.tmp(to, &format!("tv_arr_with_capacity({d}, tv_arr_len({}))", v.code), true);
                let i = self.fresh("i");
                self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", v.code));
                self.push_temps();
                let ev = Val::plain(format!("(({fct} *)tv_arr_data({}))[{i}]", v.code), fe);
                let cv = self.coerce(ev, te);
                let code = self.consume(cv);
                self.line(format!("TVG_PUSH({tct}, &{}, {d}, {code});", res.code));
                self.pop_temps();
                self.close("}");
                return res;
            }
            (Ty::Map(fk, fv), Ty::Map(tk, tv)) => {
                let (fkd, fvd) = (self.desc(fk), self.desc(fv));
                let (tkd, tvd) = (self.desc(tk), self.desc(tv));
                let (fkct, fvct) = (self.ctype(fk), self.ctype(fv));
                let (tkct, tvct) = (self.ctype(tk), self.ctype(tv));
                let res = self.tmp(to, "tv_map_fresh()", true);
                let (cursor, kp, vp) = (self.fresh("i"), self.fresh("k"), self.fresh("v"));
                self.line(format!("{fkct} *{kp} = NULL; {fvct} *{vp} = NULL;"));
                self.open(&format!("for (tv_int {cursor} = 0; tv_map_next({}, {fkd}, {fvd}, &{cursor}, (void **)&{kp}, (void **)&{vp});) {{", v.code));
                self.push_temps();
                let (key, value) = (self.fresh("k"), self.fresh("v"));
                self.line(format!("{fkct} {key} = *{kp}; {fvct} {value} = *{vp};"));
                let (moved_key, moved_value) = (self.fresh("k"), self.fresh("v"));
                let converted_key = self.coerce(Val::plain(key, fk), tk);
                let key_code = self.consume(converted_key);
                self.line(format!("{tkct} {moved_key} = {key_code};"));
                let converted_value = self.coerce(Val::plain(value, fv), tv);
                let value_code = self.consume(converted_value);
                self.line(format!("{tvct} {moved_value} = {value_code};"));
                self.line(format!("tv_map_set(&{}, {tkd}, {tvd}, &{moved_key}, &{moved_value});", res.code));
                self.pop_temps();
                self.close("}");
                return res;
            }
            // A promise with a narrower error type (or a value of the same representation) is the
            // same promise.
            (Ty::Promise(fv, _), Ty::Promise(tv, _)) if fv == tv || self.ctype(fv) == self.ctype(tv) => {
                return Val { ty: to, ..v };
            }
            (Ty::Func(fps, fret, _), Ty::Func(tps, tret, _)) => {
                // Same C calling convention when parameter and return C types agree (e.g. a result
                // ignored as void); otherwise an adapter converts the arguments and the result.
                let (fps, tps) = (self.c.types.params(fps).to_vec(), self.c.types.params(tps).to_vec());
                let same_params = fps.len() == tps.len() && fps.iter().zip(&tps).all(|(a, b)| a.inout == b.inout && self.ctype(a.ty) == self.ctype(b.ty) && self.c.types.rest_elem(a.ty).is_some() == self.c.types.rest_elem(b.ty).is_some());
                let same_ret = tret == VOID || self.ctype(fret) == self.ctype(tret);
                if same_params && same_ret {
                    return Val { ty: to, ..v };
                }
                let adapter = self.fn_adapter(from, to);
                // (not `fa`: the arrays' free functions are `fa<type>`, see arr_free_fn)
                let fv = self.fresh("fbox");
                self.line(format!("tvg_fnbox *{fv} = tvg_alloc_small(sizeof(tvg_fnbox)); {fv}->h.rc = 1; {fv}->h.size = TVG_ENV_SIZE(sizeof(tvg_fnbox)); {fv}->h.drop = tvg_fnbox_drop; {fv}->f = {}; tv_env_retain({fv}->f.env);", v.code));
                return self.tmp(to, &format!("((tv_fn){{ (void *){adapter}, (tv_env *){fv} }})"), true);
            }
            _ => {}
        }
        // (an object where a string is wanted, in code a narrowing left unreachable:
        // `${e instanceof Error ? e.message : e}`): its `String(x)`, as JavaScript's
        if to == STR && matches!(self.tget(from), Ty::Class(..)) {
            let sbv = self.fresh("sb");
            self.line(format!("tv_sb {sbv} = {{0}};"));
            self.line("{");
            self.bump(1);
            self.line(format!("tv_sb *sb = &{sbv};"));
            let c = self.string_code(from, &v.code);
            self.line(format!("{c};"));
            self.bump(-1);
            self.line("}");
            return self.tmp(STR, &format!("tv_str_from_sb(&{sbv})"), true);
        }
        // (a float where `int | undefined` is wanted: an index, `s.slice(0, n / 2)`, as JavaScript's
        // ToIntegerOrInfinity reads it)
        if self.c.types.is_float(from) && !self.c.types.members(to).iter().any(|&m| self.c.types.is_float(m)) && self.c.types.members(to).contains(&INT) {
            let i = self.int_code(v);
            return self.coerce(Val::plain(i, INT), to);
        }
        let (fs, ts) = (self.c.show(from), self.c.show(to));
        if std::env::var("TOV_DBG").is_ok() { let o = &self.b().out; let st = o.len().saturating_sub(600); eprintln!("ICE at:\n{}", &o[o.floor_char_boundary(st)..]); }
        self.internal.push(format!("no conversion from `{fs}` to `{ts}`"));
        Val { ty: to, ..v }
    }

    /// A C function with `to`'s signature that calls a `from` function (the one in its
    /// `tvg_fnbox` environment): arguments convert to `from`'s parameters (extra ones are
    /// dropped, missing optional ones `undefined`), and the result to `to`'s return type.
    fn fn_adapter(&mut self, from: TyId, to: TyId) -> String {
        if let Some(n) = self.fn_adapters.get(&(from, to)) {
            return n.clone();
        }
        let (Ty::Func(fps, fret, _), Ty::Func(tps, tret, _)) = (self.tget(from), self.tget(to)) else { unreachable!() };
        let (fps, tps) = (self.c.types.params(fps).to_vec(), self.c.types.params(tps).to_vec());
        let name = format!("tvg_adapt_{}", self.fn_adapters.len());
        self.fn_adapters.insert((from, to), name.clone());
        let proto = {
            let rct = if tret == VOID { "void".to_string() } else { self.ctype(tret) };
            let mut ps = vec!["tv_env *env_".to_string()];
            for (i, p) in tps.iter().enumerate() {
                let ct = self.ctype(p.ty);
                ps.push(if p.inout { format!("{ct} *p{i}") } else { format!("{ct} p{i}") });
            }
            format!("{rct} {name}({})", ps.join(", "))
        };
        let _ = writeln!(self.protos, "static {proto};");
        let m = self.cur_m();
        self.begin_scratch(m);
        let mut argv = vec!["inner.env".to_string()];
        let (sf, st) = (fps.last().and_then(|p| self.c.types.rest_elem(p.ty)), tps.last().and_then(|p| self.c.types.rest_elem(p.ty)));
        if sf.is_some() || st.is_some() {
            // (a rest parameter on either side: the arguments packed into its array, or taken
            // out of the caller's)
            let nf = fps.len() - sf.is_some() as usize;
            let nt = tps.len() - st.is_some() as usize;
            let rest_arr = st.map(|_| format!("p{nt}"));
            let arg = |g: &mut Self, j: usize, to: TyId| -> Val {
                if j < nt {
                    g.coerce(Val::plain(format!("p{j}"), tps[j].ty), to)
                } else if let (Some(arr), Some(e)) = (&rest_arr, st) {
                    // (an argument the caller may not have: `undefined` where the parameter
                    // takes it, else its type's default)
                    let k = j - nt;
                    let ect = g.ctype(e);
                    let tct = g.ctype(to);
                    let w = g.fresh("rw");
                    g.line(format!("{tct} {w};"));
                    let u = if g.c.types.has_undefined(to) || to == ANY || to == UNKNOWN { g.coerce(Val::plain("0", UNDEFINED), to).code } else { g.default_value(to) };
                    g.open(&format!("if ({k} < tv_arr_len({arr})) {{"));
                    g.push_temps();
                    let el = g.coerce(Val::plain(format!("(({ect} *)tv_arr_data({arr}))[{k}]"), e), to);
                    let el = g.own(el);
                    let c = g.consume(el);
                    g.line(format!("{w} = {c};"));
                    g.pop_temps();
                    g.close(&format!("}} else {w} = {u};"));
                    let owned = g.is_rc(to);
                    if owned {
                        g.b().temps.last_mut().unwrap().push((w.clone(), to));
                    }
                    Val { code: w, ty: to, owned }
                } else {
                    g.coerce(Val::plain("0", UNDEFINED), to)
                }
            };
            for (j, fp) in fps.iter().enumerate().take(nf) {
                let c = arg(self, j, fp.ty);
                argv.push(c.code);
            }
            if let Some(fe) = sf {
                // (the source's rest: the caller's arguments from there, as an array)
                let at = self.c.types.array(fe);
                let fed = self.desc(fe);
                let fect = self.ctype(fe);
                let r = self.tmp(at, "tv_arr_fresh()", true);
                for j in nf..nt {
                    let v = arg(self, j, fe);
                    let v = self.own(v);
                    let c = self.consume(v);
                    self.line(format!("TVG_PUSH({fect}, &{}, {fed}, {c});", r.code));
                }
                if let (Some(arr), Some(te)) = (&rest_arr, st) {
                    let tect = self.ctype(te);
                    let i = self.fresh("i");
                    self.open(&format!("for (tv_int {i} = {}; {i} < tv_arr_len({arr}); {i}++) {{", nf.saturating_sub(nt)));
                    self.push_temps();
                    let el = self.coerce(Val::plain(format!("(({tect} *)tv_arr_data({arr}))[{i}]"), te), fe);
                    let el = self.own(el);
                    let c = self.consume(el);
                    self.line(format!("TVG_PUSH({fect}, &{}, {fed}, {c});", r.code));
                    self.pop_temps();
                    self.close("}");
                }
                argv.push(r.code);
            }
        } else {
        for (i, fp) in fps.iter().enumerate() {
            // (an optional parameter the caller can't pass: `undefined`, which takes its default)
            let Some(&tp) = tps.get(i) else {
                let u = self.coerce(Val::plain("0", UNDEFINED), fp.ty);
                argv.push(u.code);
                continue;
            };
            if fp.inout {
                argv.push(format!("p{i}"));
            } else {
                let a = self.coerce(Val::plain(format!("p{i}"), tp.ty), fp.ty);
                argv.push(a.code);
            }
        }
        }
        self.line("tv_fn inner = ((tvg_fnbox *)env_)->f;");
        let cast = {
            let rct = if fret == VOID { "void".to_string() } else { self.ctype(fret) };
            let mut ps = vec!["tv_env *".to_string()];
            for p in &fps {
                let ct = self.ctype(p.ty);
                ps.push(if p.inout { format!("{ct} *") } else { ct });
            }
            format!("{rct} (*)({})", ps.join(", "))
        };
        let call = format!("(({cast})inner.fn)({})", argv.join(", "));
        if fret == VOID {
            self.line(format!("{call};"));
            if tret != VOID {
                let v = self.coerce(Val::plain("0", UNDEFINED), tret);
                self.scratch_return(v, tret);
            }
        } else {
            let r = self.tmp(fret, &call, true);
            self.scratch_return(r, tret);
        }
        let body = self.end_scratch();
        let _ = writeln!(self.helpers_after_decl, "static {proto} {{\n{body}}}\n");
        name
    }

    /// The member of union `to` that a value of type `from` converts into.
    fn member_target(&mut self, to: TyId, from: TyId) -> Option<TyId> {
        let members = self.members(to);
        if members.contains(&from) {
            return Some(from);
        }
        // (a union, a recursive alias of one: its members, each to its own, not all to one)
        let uf0 = self.c.unfold(from);
        if matches!(self.tget(uf0), Ty::Union(_)) {
            return None;
        }
        // (a member holding it, `Marked` of `Marked | Generic` for a `Blockquote`, before one it
        // only fits: an open record, `Generic`, takes any with more fields, last)
        let uf = self.c.unfold(from);
        if let Some(mt) = members.iter().copied().find(|&mt| mt != UNDEFINED && { let um = self.c.unfold(mt); matches!(self.tget(um), Ty::Union(_)) } && {
            let fl = self.c.flat_members(mt);
            fl.contains(&from) || fl.contains(&uf)
        }) {
            return Some(mt);
        }
        let open = |g: &mut Self, t: TyId| g.c.interner.lookup("$rest").is_some_and(|r| {
            let u = g.c.unfold(t);
            matches!(g.tget(u), Ty::Record(fs) if g.c.types.fields(fs).iter().any(|f| f.name == r))
        });
        let closed = members.iter().copied().find(|&mt| mt != UNDEFINED && !open(self, mt) && self.c.assignable_pub(from, mt));
        closed.or_else(|| members.into_iter().find(|&mt| mt != UNDEFINED && self.c.assignable_pub(from, mt)))
    }

    /// Converts a union value to another union whose members accept every source member.
    fn retag(&mut self, v: Val, fms: &[TyId], to: TyId) -> Val {
        let ct = self.ctype(to);
        let res = self.fresh("u");
        self.line(format!("{ct} {res};"));
        let tag = self.u_tag(&v.code, v.ty);
        self.open(&format!("switch ({tag}) {{"));
        for (i, &fm) in fms.iter().enumerate() {
            let target = self.member_target(to, fm).or_else(|| {
                // After `instanceof`: a base-class member becomes a subclass member.
                let Ty::Class(fc, _) = self.tget(fm) else { return None };
                self.members(to).into_iter().find(|&t| matches!(self.tget(t), Ty::Class(tc, _) if self.c.class_descends(tc, fc)))
            });
            let Some(mt) = target else {
                // (a member that's a recursive alias or a union of its own, its members among the
                // target's: what it holds, converted)
                let holds_union = match self.tget(fm) {
                    Ty::Union(_) => true,
                    Ty::Rec(..) => {
                        let u = self.c.unfold(fm);
                        matches!(self.tget(u), Ty::Union(_))
                    }
                    _ => false,
                };
                if holds_union {
                    self.open(&format!("case {i}: {{"));
                    self.push_temps();
                    let payload = Val::plain(self.u_payload(&v.code, v.ty, i), fm);
                    let inner = if matches!(self.tget(fm), Ty::Rec(..)) { self.unfold_val(payload) } else { payload };
                    // (narrowing still: what it holds outside the target can't occur here)
                    let cv = self.project(inner, to);
                    let code = self.consume(cv);
                    self.line(format!("{res} = {code};"));
                    self.pop_temps();
                    self.line("break;");
                    self.close("}");
                }
                continue;
            };
            let k = self.tag_of(to, mt).unwrap();
            self.open(&format!("case {i}: {{"));
            self.push_temps();
            if self.is_unit(mt) {
                let u = self.u_make(to, k, None);
                self.line(format!("{res} = {u};"));
            } else if mt == fm && matches!(self.tget(fm), Ty::Interface(..)) {
                // (an interface value: what it wraps, when that's one of the target's own, see
                // `coerce_inner`)
                let payload = Val::plain(self.u_payload(&v.code, v.ty, i), fm);
                let cv = self.coerce(payload, to);
                let code = self.consume(cv);
                self.line(format!("{res} = {code};"));
            } else {
                let payload = if self.is_unit(fm) { Val::plain("0", fm) } else { Val::plain(self.u_payload(&v.code, v.ty, i), fm) };
                let cv = if matches!((self.tget(fm), self.tget(mt)), (Ty::Class(..), Ty::Class(..))) { self.class_cast(payload, mt) } else { self.coerce(payload, mt) };
                let code = self.consume(cv);
                let u = self.u_make(to, k, Some(&code));
                self.line(format!("{res} = {u};"));
            }
            self.pop_temps();
            self.line("break;");
            self.close("}");
        }
        let dv = self.default_value(to);
        self.line(format!("default: {res} = {dv}; break;"));
        self.close("}");
        let owned = self.is_rc(to);
        if owned {
            self.b().temps.last_mut().unwrap().push((res.clone(), to));
        }
        Val { code: res, ty: to, owned }
    }

    /// `x!` / `x as T`: checks the tag at run time, then projects.
    pub(crate) fn downcast(&mut self, v: Val, to: TyId, span: Span) -> Val {
        if v.ty == to {
            return v;
        }
        if v.ty == UNKNOWN {
            let loc = self.loc(span);
            return self.dyn_cast(v, to, &loc);
        }
        if v.ty == ANY {
            if to == UNKNOWN {
                return Val { ty: to, ..v };
            }
            let loc = self.loc(span);
            return self.any_to(v, to, &loc);
        }
        if let (Ty::Class(fc, _), Ty::Class(tc, _)) = (self.tget(v.ty), self.tget(to))
            && !self.c.class_descends(fc, tc)
        {
            let isa = self.instanceof_code(v.clone(), tc);
            let loc = self.loc(span);
            self.line(format!("if (!{isa}) tv_trap(\"`as` cast failed: the object is a different class\", {loc});"));
            return self.class_cast(v, to);
        }
        // `ch as "+" | "-"`: the string compared with each literal (a failed `as` traps)
        if self.c.types.is_string(v.ty)
            && !matches!(self.tget(v.ty), Ty::StrLit(_))
            && let lits = self.members(to)
            && !lits.is_empty()
            && lits.iter().all(|&m| matches!(self.tget(m), Ty::StrLit(_)))
        {
            let ct = self.ctype(to);
            let (res, sv) = (self.fresh("t"), self.fresh("s"));
            // (each literal's value first: making one may take lines of its own)
            let mut arms = Vec::new();
            for &m in &lits {
                let Ty::StrLit(sym) = self.tget(m) else { continue };
                let lit = self.lit(self.sym(sym).to_string().as_bytes());
                let one = self.coerce(Val::plain("0", m), to);
                arms.push((lit, one.code));
            }
            self.line(format!("{ct} {res}; tv_str {sv} = {};", v.code));
            for (i, (lit, one)) in arms.into_iter().enumerate() {
                let eq = self.eq_code(STR, &sv, &lit);
                self.line(format!("{}if ({eq}) {res} = {one};", if i == 0 { "" } else { "else " }));
            }
            let loc = self.loc(span);
            self.line(format!("else tv_trap(\"`as` cast failed: the string is none of the type's\", {loc});"));
            return Val::plain(res, to);
        }
        let v = if matches!(self.tget(v.ty), Ty::Rec(..)) { self.unfold_val(v) } else { v };
        // `args[0] as string[]` of a `Val[]` (or a union holding one): a copy whose elements are
        // each cast (a failed one traps), the array kinds differing in C
        if let Ty::Array(te) = self.tget(to) {
            let src_arr = match self.tget(v.ty) {
                Ty::Array(fe) if fe != te => Some((v.clone(), fe)),
                Ty::Union(_) => {
                    let fms = self.members(v.ty);
                    let arrs: Vec<(usize, TyId)> = fms.iter().enumerate().filter_map(|(i, &m)| match self.tget(m) { Ty::Array(fe) if fe != te => Some((i, m)), _ => None }).collect();
                    if let [(i, at)] = arrs[..]
                        && !fms.contains(&to)
                    {
                        let loc = self.loc(span);
                        let is = self.u_is(&v.code, v.ty, i);
                        self.line(format!("if (!{is}) tv_trap(\"`as` cast failed: the value has a different type\", {loc});"));
                        let Ty::Array(fe) = self.tget(at) else { unreachable!() };
                        Some((self.project(v.clone(), at), fe))
                    } else {
                        None
                    }
                }
                _ => None,
            };
            if let Some((a, fe)) = src_arr {
                let (fct, td) = (self.ctype(fe), self.desc(te));
                let (out, i) = (self.fresh("cast"), self.fresh("i"));
                let tct = self.ctype(te);
                self.line(format!("tv_arr {out} = tv_arr_with_capacity({td}, tv_arr_len({}));", a.code));
                self.open(&format!("for (tv_int {i} = 0; {i} < tv_arr_len({}); {i}++) {{", a.code));
                self.push_temps();
                let e = Val::plain(format!("((({fct} *)tv_arr_data({}))[{i}])", a.code), fe);
                let c = self.downcast(e, te, span);
                let c = self.own(c);
                let code = self.consume(c);
                self.line(format!("TVG_PUSH({tct}, &{out}, {td}, {code});"));
                self.pop_temps();
                self.close("}");
                return self.tmp(to, &out, true);
            }
        }
        if let Ty::Union(_) = self.tget(v.ty) {
            let fms = self.members(v.ty);
            let allowed: Vec<usize> = fms.iter().enumerate().filter(|(_, fm)| **fm == to || self.members(to).contains(fm)).map(|(i, _)| i).collect();
            if !allowed.is_empty() {
                let loc = self.loc(span);
                let conds: Vec<String> = allowed.iter().map(|&i| format!("!{}", self.u_is(&v.code, v.ty, i))).collect();
                let msg = if to == self.c.types.without_nullish(v.ty) { "value is undefined (`!` assertion failed)" } else { "`as` cast failed: the value has a different type" };
                self.line(format!("if ({}) tv_trap({}, {loc});", conds.join(" && "), c_string(msg.as_bytes())));
                // (a value of its own, `return x!` moving x: still its own, as the member)
                if v.owned && self.is_rc(to) {
                    let code = self.consume(v.clone());
                    let p = self.project(Val { code, ..v }, to);
                    return self.tmp(to, &p.code, true);
                }
                return self.project(v, to);
            }
        }
        self.coerce(v, to)
    }

    /// `x` as it's held: a local narrowed by the flow as its own (declared) type, for a test of
    /// what it holds; anything else as usual.
    pub(crate) fn expr_held(&mut self, x: ExprId) -> Val {
        let m = self.cur_m();
        if let (ExprKind::Ident(_), Some(IdentFact::Local(k))) = (&self.ast(m).expr(x).kind, self.ident_fact(m, x))
            && let Some(l) = self.b().locals.get(&k).cloned()
            && l.ty != self.ty(x)
        {
            return Val::plain(l.access, l.ty);
        }
        self.expr(x)
    }

    /// `typeof v === want` as a C condition (on its tag, for a union).
    fn typeof_test(&mut self, v: &Val, want: &str) -> String {
        if matches!(self.tget(v.ty), Ty::Rec(..)) {
            let u = self.unfold_val(v.clone());
            return self.typeof_test(&u, want);
        }
        if let Ty::Union(ms) = self.tget(v.ty) {
            let ms = self.c.types.tys(ms).to_vec();
            let tag = self.u_tag(&v.code, v.ty);
            let mut parts = Vec::new();
            for (i, &m) in ms.iter().enumerate() {
                if matches!(self.tget(m), Ty::Rec(..) | Ty::Union(_)) {
                    let payload = Val { code: self.u_payload(&v.code, v.ty, i), ty: m, owned: false };
                    let inner = self.typeof_test(&payload, want);
                    if inner != "false" {
                        parts.push(format!("({tag} == {i} && {inner})"));
                    }
                } else if self.typeof_name(m) == want {
                    parts.push(format!("{tag} == {i}"));
                }
            }
            return if parts.is_empty() { "false".into() } else { format!("({})", parts.join(" || ")) };
        }
        if self.typeof_name(v.ty) == want { "true".into() } else { "false".into() }
    }

    /// What `typeof` says of a value of (non-union) type `t`.
    fn typeof_name(&self, t: TyId) -> &'static str {
        let types = &self.c.types;
        if types.is_numeric(t) {
            "number"
        } else if t == BIGINT {
            "bigint"
        } else if t == DECIMAL {
            "decimal"
        } else if types.is_string(t) {
            "string"
        } else if t == BOOL || matches!(types.get(t), Ty::BoolLit(_)) {
            "boolean"
        } else if t == UNDEFINED || t == VOID {
            "undefined"
        } else if matches!(types.get(t), Ty::Func(..)) {
            "function"
        } else {
            "object"
        }
    }

    fn typeof_str(&mut self, v: Val) -> Val {
        if v.ty == UNKNOWN || v.ty == ANY {
            return self.dyn_typeof(v);
        }
        let name = |types: &Types, t: TyId| -> &'static str {
            if types.is_numeric(t) {
                "number"
            } else if t == BIGINT {
                "bigint"
            } else if t == DECIMAL {
                "decimal"
            } else if types.is_string(t) {
                "string"
            } else if t == BOOL {
                "boolean"
            } else if t == UNDEFINED {
                "undefined"
            } else if matches!(types.get(t), Ty::Func(..)) {
                "function"
            } else {
                "object"
            }
        };
        if v.ty == JS {
            return self.tmp(STR, &format!("({{ const char *s = tv_js_typeof({}); tv_str_from(s, strlen(s)); }})", v.code), true);
        }
        // (a recursive alias: what it holds)
        if matches!(self.tget(v.ty), Ty::Rec(..)) {
            let u = self.unfold_val(v);
            return self.typeof_str(u);
        }
        if let Ty::Union(ms) = self.tget(v.ty) {
            let ms = self.c.types.tys(ms).to_vec();
            let res = self.fresh("s");
            self.line(format!("tv_str {res};"));
            let tag = self.u_tag(&v.code, v.ty);
            self.open(&format!("switch ({tag}) {{"));
            for (i, &m) in ms.iter().enumerate() {
                // (a member that's a recursive alias or a union itself: the typeof of what it holds)
                if matches!(self.tget(m), Ty::Rec(..) | Ty::Union(_)) {
                    self.open(&format!("case {i}: {{"));
                    let payload = Val { code: self.u_payload(&v.code, v.ty, i), ty: m, owned: false };
                    let inner = self.typeof_str(payload);
                    self.line(format!("{res} = {};", inner.code));
                    self.close("} break;");
                    continue;
                }
                let n = name(&self.c.types, m);
                let l = self.lit(n.as_bytes());
                self.line(format!("case {i}: {res} = {l}; break;"));
            }
            self.line(format!("default: {res} = TV_EMPTY_STR; break;"));
            self.close("}");
            return Val::plain(res, STR);
        }
        let n = name(&self.c.types, v.ty);
        Val::plain(self.lit(n.as_bytes()), STR)
    }

    pub(crate) fn int_code(&mut self, v: Val) -> String {
        if v.ty == ANY {
            // (an `any`'s number, as JavaScript's ToIntegerOrInfinity reads it)
            return format!("tv_f64_to_int_js(tv_any_num({}))", v.code);
        }
        if v.ty == INT {
            v.code
        } else if self.c.types.is_float(v.ty) {
            // (JavaScript's ToIntegerOrInfinity: truncated, NaN is 0)
            format!("tv_f64_to_int_js({})", v.code)
        } else {
            format!("((tv_int)({}))", v.code)
        }
    }

    /// An index: an integer, or a float that must be a whole number.
    pub(crate) fn index_code(&mut self, v: Val, span: Span) -> String {
        if self.c.types.is_float(v.ty) {
            let loc = self.loc(span);
            return format!("tv_f64_index({}, {loc})", v.code);
        }
        self.int_code(v)
    }

    /// A C boolean for a condition (`bool`, or an optional value tested for presence).
    pub(crate) fn cond(&mut self, e: ExprId) -> String {
        let v = self.expr(e);
        self.js_truthy(v)
    }

    /// JavaScript's truthiness of a value: `false`, `0`, `NaN`, `""`, `0n` and `undefined` are
    /// false; objects (even empty arrays) are true.
    pub(crate) fn js_truthy(&mut self, v: Val) -> String {
        let t = v.ty;
        match self.tget(t) {
            Ty::Bool => v.code,
            Ty::Js => format!("tv_js_truthy({})", v.code),
            Ty::Unknown => format!("tv_dyn_truthy({})", v.code),
            Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64 => format!("(({}) != 0)", v.code),
            Ty::F64 | Ty::F32 => format!("tv_f64_truthy({})", v.code),
            Ty::Str => format!("(tv_str_byte_len({}) != 0)", v.code),
            Ty::StrLit(s) => if self.sym(s).is_empty() { "false".into() } else { "true".into() },
            Ty::BigInt => format!("(!tv_big_eq({}, tv_big_small(0)))", v.code),
            Ty::Decimal => format!("(tv_dec_cmp({}, ((tv_dec){{0, 0}})) != 0)", v.code),
            Ty::Undefined | Ty::Null | Ty::Void => "false".into(),
            Ty::Union(_) => {
                let ms = self.members(t);
                // (each member that can be falsy, tested on its own)
                let falsy: Vec<usize> = ms.iter().enumerate().filter(|(_, m)| matches!(self.tget(**m), Ty::Undefined | Ty::Null | Ty::Void | Ty::Bool | Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64 | Ty::F64 | Ty::F32 | Ty::Str | Ty::StrLit(_) | Ty::BigInt | Ty::Decimal)).map(|(i, _)| i).collect();
                if falsy.is_empty() {
                    return "true".into();
                }
                if falsy.iter().all(|&i| matches!(self.tget(ms[i]), Ty::Undefined | Ty::Void)) {
                    return self.truthy(v);
                }
                let x = self.fresh("tr");
                let ct = self.ctype(t);
                self.line(format!("{ct} {x} = {};", v.code));
                let mut parts = Vec::new();
                for (i, &m) in ms.iter().enumerate() {
                    let is = self.u_is(&x, t, i);
                    if !falsy.contains(&i) {
                        parts.push(is);
                        continue;
                    }
                    if matches!(self.tget(m), Ty::Undefined | Ty::Null | Ty::Void) {
                        continue;
                    }
                    let p = Val::plain(self.u_payload(&x, t, i), m);
                    let c = self.js_truthy(p);
                    parts.push(format!("({is} && {c})"));
                }
                if parts.is_empty() { "false".into() } else { format!("({})", parts.join(" || ")) }
            }
            _ => self.truthy(v),
        }
    }

    fn truthy(&mut self, v: Val) -> String {
        if v.ty == BOOL {
            return v.code;
        }
        if v.ty == JS {
            return format!("tv_js_truthy({})", v.code);
        }
        if v.ty == UNKNOWN {
            return format!("({{ tv_dyn d_ = {}; d_.t != &tv_type_undefined && d_.t != &tv_type_null; }})", v.code);
        }
        if v.ty == ANY {
            return format!("tv_dyn_truthy({})", v.code);
        }
        // (in TypeScript, an object read from a hole, `new Array(n)`'s: none, false)
        if self.js_ints(self.cur_m()) {
            match self.tget(v.ty) {
                Ty::Class(..) | Ty::Rec(..) => return format!("(({}) != NULL)", v.code),
                Ty::Interface(..) => return format!("(({}).p != NULL)", v.code),
                Ty::Record(_) if self.c.hole_records.contains(&v.ty) => return format!("(({}) != NULL)", v.code),
                _ => {}
            }
        }
        let v = if matches!(self.tget(v.ty), Ty::Rec(..)) { return "true".into() } else { v };
        // (present: neither `undefined` nor `null`)
        let absent: Vec<String> = [UNDEFINED, NULL].iter().filter_map(|&n| self.tag_of(v.ty, n)).map(|k| self.u_is(&v.code, v.ty, k)).collect();
        match absent.as_slice() {
            [] => "true".into(),
            [one] => format!("(!{one})"),
            _ => format!("(!({}))", absent.join(" || ")),
        }
    }

    // ------------------------------------------------------------ operators

    fn unary(&mut self, op: UnOp, x: ExprId, ty: TyId, span: Span) -> Val {
        match op {
            UnOp::Not => {
                let c = self.cond(x);
                Val::plain(format!("(!{c})"), BOOL)
            }
            UnOp::Neg if ty == BIGINT => {
                let v = self.expr(x);
                // (a literal folds: `-5n` is one constant)
                if let ExprKind::BigInt(s) = self.ast(self.cur_m()).expr(x).kind {
                    let text = format!("-{}", self.sym(s));
                    if let Ok(n) = text.parse::<i128>()
                        && n >= -(1i128 << 62)
                    {
                        return Val::plain(format!("((tv_big){{INT64_C({})}})", 2 * n + 1), BIGINT);
                    }
                }
                self.tmp(BIGINT, &format!("tv_big_neg({})", v.code), true)
            }
            UnOp::Neg if ty == DECIMAL => {
                let v = self.expr(x);
                let v = self.coerce(v, DECIMAL);
                Val::plain(format!("tv_dec_neg({})", v.code), DECIMAL)
            }
            UnOp::BitNot if ty == BIGINT => {
                let v = self.expr(x);
                self.tmp(BIGINT, &format!("tv_big_not({})", v.code), true)
            }
            // (of an `any`: its ToNumber)
            UnOp::Neg | UnOp::Plus | UnOp::BitNot if self.ty(x) == ANY => {
                let v = self.expr(x);
                let n = format!("tv_any_num({})", v.code);
                let code = match op {
                    UnOp::Neg => format!("(-{n})"),
                    UnOp::Plus => n,
                    _ => format!("((tv_int)~tv_f64_to_i32({n}))"),
                };
                let t = if op == UnOp::BitNot { INT } else { F64 };
                self.coerce(Val::plain(code, t), ty)
            }
            UnOp::Neg => {
                let v = self.expr(x);
                let v = self.coerce(v, ty);
                if self.c.types.is_float(ty) {
                    Val::plain(format!("(-{})", v.code), ty)
                } else if v.code.starts_with("INT64_C(") || v.code.starts_with("((") {
                    // Literal: fold the sign (INT64_MIN is written as a literal too).
                    let ct = self.ctype(ty);
                    Val::plain(format!("(({ct})(-{}))", v.code), ty)
                } else if ty == INT && matches!(self.int_range(x), super::range::R::In(lo, _) if lo > i64::MIN as i128) {
                    // (never `INT64_MIN`: negating can't overflow)
                    Val::plain(format!("(-{})", v.code), ty)
                } else {
                    let ct = self.ctype(ty);
                    let loc = self.loc(span);
                    Val::plain(format!("TV_NEG({ct}, {}, {loc})", v.code), ty)
                }
            }
            UnOp::Plus => {
                let v = self.expr(x);
                if self.c.types.is_string(v.ty) {
                    let v = self.coerce(v, STR);
                    return Val::plain(format!("tv_js_number({})", v.code), F64);
                }
                if v.ty == BOOL {
                    return Val::plain(format!("((tv_int)({}))", v.code), INT);
                }
                self.coerce(v, ty)
            }
            UnOp::BitNot => {
                let v = self.expr(x);
                if ty == INT {
                    let a = self.int32_code(&v);
                    return Val::plain(format!("((tv_int)~{a})"), ty);
                }
                Val::plain(format!("(~{})", v.code), ty)
            }
            UnOp::Void => {
                let _ = self.expr(x);
                Val::plain("0", UNDEFINED)
            }
        }
    }

    /// `n / c + t1 - t2 ...` as a float, `n` and the `t`s integers and `c` a power of two up to
    /// 1024 (`(i + j) * (i + j + 1) / 2 + i + 1`): while they're small (|n| <= 2^50, |t| <= 2^40)
    /// every partial sum is exactly a float, so `(n + c*t1 - c*t2 ...) / c`, integer adds and one
    /// conversion, is the same value as the float adds; otherwise the float adds run as written.
    fn exact_half_sum(&mut self, e: ExprId, op: BinOp, l: ExprId, r: ExprId) -> Option<Val> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let ty = |x: ExprId| facts.expr_ty.get(x as usize).copied().unwrap_or(ERROR);
        let strip = |mut x: ExprId| {
            while let ExprKind::Paren(y) = &ast.expr(x).kind {
                x = *y;
            }
            x
        };
        // the spine: ((n / c) ± t1) ± t2 ...
        let mut terms: Vec<(bool, ExprId)> = vec![(op == BinOp::Sub, r)];
        let mut cur = strip(l);
        let (n, c) = loop {
            match &ast.expr(cur).kind {
                ExprKind::Binary(o @ (BinOp::Add | BinOp::Sub), a, b) if ty(cur) == F64 => {
                    terms.push((*o == BinOp::Sub, *b));
                    cur = strip(*a);
                }
                ExprKind::Binary(BinOp::Div, a, b) if ty(cur) == F64 => match ast.expr(strip(*b)).kind {
                    ExprKind::Int(c) if c.is_power_of_two() && c <= 1024 => break (*a, c),
                    _ => return None,
                },
                _ => return None,
            }
        };
        // (an integer literal is a term whatever its context made it)
        let lit = |x: ExprId| match ast.expr(strip(x)).kind {
            ExprKind::Int(v) if v < 1 << 40 => Some(v),
            _ => None,
        };
        if ty(n) != INT || terms.len() > 8 || terms.iter().any(|&(_, t)| ty(t) != INT && lit(t).is_none()) {
            return None;
        }
        let lits: Vec<Option<u64>> = terms.iter().map(|&(_, t)| lit(t)).collect();
        terms.reverse();
        let lits: Vec<Option<u64>> = lits.into_iter().rev().collect();
        // (evaluated in the order the float code would: `n`, then each term; `n` only ever
        // becomes a float, which rounds a product past 2^53 as JavaScript does, so its
        // multiplication needn't round it first)
        let nspan = ast.expr(strip(n)).span;
        let saved = self.float_only.replace(nspan);
        let nv = self.expr(n);
        self.float_only = saved;
        let nc = self.int_code(nv);
        let nn = self.fresh("hn");
        self.line(format!("tv_int {nn} = {nc};"));
        let mut tvars = Vec::new();
        for (&(neg, t), l) in terms.iter().zip(&lits) {
            let code = match l {
                Some(v) => format!("INT64_C({v})"),
                None => {
                    let v = self.expr(t);
                    self.int_code(v)
                }
            };
            let tn = self.fresh("ht");
            self.line(format!("tv_int {tn} = {code};"));
            tvars.push((neg, tn));
        }
        let mut small = vec![format!("(uint64_t){nn} + ((uint64_t)1 << 50) <= ((uint64_t)1 << 51)")];
        let mut exact = nn.clone();
        let mut floats = format!("((double){nn} / {c}.0)");
        for (neg, tn) in &tvars {
            small.push(format!("(uint64_t){tn} + ((uint64_t)1 << 40) <= ((uint64_t)1 << 41)"));
            let sign = if *neg { "-" } else { "+" };
            exact = format!("{exact} {sign} {tn} * {c}");
            floats = format!("({floats} {sign} (double){tn})");
        }
        let _ = e;
        // (the empty asm keeps the float adds a branch apart, not computed alongside and selected)
        Some(Val::plain(format!("(__builtin_expect({}, 1) ? (double)({exact}) / {c}.0 : ({{ __asm__ volatile(\"\"); {floats}; }}))", small.join(" && ")), F64))
    }

    /// Declares the builder `sbv` for a string made in one go: on the stack (TV_SB_STACK) in its
    /// own C block, which `end_builder` closes, so a short string costs one allocation and the
    /// C compiler reuses the buffer's stack slot for the next one. In an async function or a
    /// generator, whose locals move into a frame, an ordinary builder (`None`). Returns the
    /// variable the string is left in.
    fn stack_builder(&mut self, sbv: &str) -> Option<String> {
        let framed = self.b().async_fn.is_some() || self.b().gen_elem.is_some();
        if framed {
            self.line(format!("tv_sb {sbv} = {{0}};"));
            return None;
        }
        let res = self.fresh("t");
        self.line(format!("tv_str {res};"));
        self.open("{");
        self.line(format!("TV_SB_STACK({sbv});"));
        Some(res)
    }

    /// The string `stack_builder`'s builder made (an owned temporary), its block closed.
    fn end_builder(&mut self, sbv: &str, stack: Option<String>) -> Val {
        match stack {
            Some(res) => {
                self.line(format!("{res} = tv_str_from_sb(&{sbv});"));
                self.close("}");
                self.tmp(STR, &res, true)
            }
            None => {
                self.close("}");
                self.tmp(STR, &format!("tv_str_from_sb(&{sbv})"), true)
            }
        }
    }

    pub(crate) fn binary_pub(&mut self, op: BinOp, l: ExprId, r: ExprId, ty: TyId, span: Span) -> Val {
        self.binary(op, l, r, ty, span)
    }

    fn binary(&mut self, op: BinOp, l: ExprId, r: ExprId, ty: TyId, span: Span) -> Val {
        use BinOp::*;
        // (with an `any` operand: JavaScript's operator on what it holds)
        if matches!(op, Add | Sub | Mul | Div | Rem | Pow | BitAnd | BitOr | BitXor | Shl | Shr | UShr | Lt | Gt | Le | Ge) && (self.ty(l) == ANY || self.ty(r) == ANY) {
            let a = self.expr(l);
            let b = self.expr(r);
            return self.any_binop(op, a, b, ty);
        }
        match op {
            // `a || b`, `a && b` as values (not both `bool`): `a` if it decides, else `b`
            And | Or if ty != BOOL => {
                let lv = self.expr(l);
                let lv = self.own(lv);
                let x = self.fresh("x");
                let lct = self.ctype(lv.ty);
                let lty = lv.ty;
                let lc = self.consume(lv);
                self.line(format!("{lct} {x} = {lc};"));
                let c = self.js_truthy(Val::plain(x.clone(), lty));
                let res = self.fresh("t");
                let ct = self.ctype(ty);
                self.line(format!("{ct} {res};"));
                self.open(&format!("if ({}{c}) {{", if op == Or { "" } else { "!" }));
                // (the left value decides: its truthy members for `||`, its falsy ones for `&&`)
                let keep = if lty == ANY {
                    ANY
                } else if op == Or {
                    self.c.types.without_nullish(lty)
                } else {
                    let ms: Vec<TyId> = self.members(lty).into_iter().filter(|&m| m == UNDEFINED || m == NULL || m == BOOL || self.c.types.is_primitive(m)).collect();
                    if ms.is_empty() { NEVER } else { self.c.types.union(&ms) }
                };
                if keep == NEVER {
                    let d = self.default_value(ty);
                    self.line(format!("{res} = {d};"));
                } else {
                    // (x itself becomes the result when the narrowing leaves it as it is; a view of
                    // it, a union's payload, is retained and x released)
                    let kv = self.project(Val { code: x.clone(), ty: lty, owned: false }, keep);
                    let kv = self.coerce(kv, ty);
                    if !kv.owned && (kv.code == x || kv.code == format!("({x})")) {
                        self.line(format!("{res} = {x};"));
                    } else {
                        let kc = self.consume(kv);
                        self.line(format!("{res} = {kc};"));
                        let rel = self.release_code(lty, &x);
                        if !rel.is_empty() {
                            self.line(format!("{rel};"));
                        }
                    }
                }
                self.close("} else {");
                self.b().indent += 1;
                let rel = self.release_code(lty, &x);
                if !rel.is_empty() {
                    self.line(format!("{rel};"));
                }
                self.push_temps();
                let rv = self.expr(r);
                let rv = self.coerce(rv, ty);
                let rc = self.consume(rv);
                self.line(format!("{res} = {rc};"));
                self.pop_temps();
                self.close("}");
                let owned = self.is_rc(ty);
                if owned {
                    self.b().temps.last_mut().unwrap().push((res.clone(), ty));
                }
                Val { code: res, ty, owned }
            }
            And | Or => {
                let lc = self.cond(l);
                let res = self.fresh("b");
                self.line(format!("bool {res} = {lc};"));
                self.open(&format!("if ({}{res}) {{", if op == And { "" } else { "!" }));
                self.push_temps();
                let rc = self.cond(r);
                self.line(format!("{res} = {rc};"));
                self.pop_temps();
                self.close("}");
                Val::plain(res, BOOL)
            }
            Nullish => {
                let lv = self.expr(l);
                let res = self.fresh("t");
                let ct = self.ctype(ty);
                self.line(format!("{ct} {res};"));
                let inner = self.c.types.without_nullish(lv.ty);
                let present = self.truthy(lv.clone());
                self.open(&format!("if ({present}) {{"));
                self.push_temps();
                let pv = self.project(lv, inner);
                let pv = self.coerce(pv, ty);
                let code = self.consume(pv);
                self.line(format!("{res} = {code};"));
                self.pop_temps();
                self.close("} else {");
                self.b().indent += 1;
                self.push_temps();
                let rv = self.expr(r);
                let rv = self.coerce(rv, ty);
                let code = self.consume(rv);
                self.line(format!("{res} = {code};"));
                self.pop_temps();
                self.close("}");
                let owned = self.is_rc(ty);
                if owned {
                    self.b().temps.last_mut().unwrap().push((res.clone(), ty));
                }
                Val { code: res, ty, owned }
            }
            // `x == null` / `x == undefined`: either one
            LooseEq | LooseNe if matches!(self.ast(self.cur_m()).expr(l).kind, ExprKind::Undefined | ExprKind::Null) || matches!(self.ast(self.cur_m()).expr(r).kind, ExprKind::Undefined | ExprKind::Null) => {
                let subject = if matches!(self.ast(self.cur_m()).expr(l).kind, ExprKind::Undefined | ExprKind::Null) { r } else { l };
                let v = self.expr(subject);
                let present = self.truthy(v);
                Val::plain(if op == LooseEq { format!("(!{present})") } else { present }, BOOL)
            }
            Eq | Ne | LooseEq | LooseNe => {
                // (`typeof x === "string"`: a test of what x holds, no string made)
                let m = self.cur_m();
                let typeof_lit = match (&self.ast(m).expr(l).kind, &self.ast(m).expr(r).kind) {
                    (ExprKind::Typeof(x), ExprKind::Str(s)) | (ExprKind::Str(s), ExprKind::Typeof(x)) => Some((*x, *s)),
                    _ => None,
                };
                if let Some((x, want)) = typeof_lit {
                    let xt = self.ty(x);
                    if xt != UNKNOWN && xt != ANY && xt != JS && self.c.types.without_undefined(xt) != JS {
                        let v = self.expr_held(x);
                        let want = self.sym(want).to_string();
                        let t = self.typeof_test(&v, &want);
                        return Val::plain(if matches!(op, Eq | LooseEq) { t } else { format!("(!{t})") }, BOOL);
                    }
                }
                // (`x === null` of a narrowed local: tested as what it holds, no narrowed copy)
                let unit = |c: &Self, e: ExprId| matches!(c.ast(m).expr(e).kind, ExprKind::Null | ExprKind::Undefined);
                if unit(self, r) || unit(self, l) {
                    let (x, u) = if unit(self, r) { (l, r) } else { (r, l) };
                    if matches!(self.ast(m).expr(x).kind, ExprKind::Ident(_)) {
                        let xv = self.expr_held(x);
                        if !matches!(self.tget(xv.ty), Ty::Unknown | Ty::Js) {
                            let uv = self.expr(u);
                            let eq = self.equality(xv, uv, true);
                            return Val::plain(if matches!(op, Eq | LooseEq) { eq } else { format!("(!{eq})") }, BOOL);
                        }
                    }
                }
                // (`xs[i] === x` past either end: false, as JavaScript's `undefined` isn't `x`)
                if let Some(v) = self.compare_past_end(op, l, r) {
                    return v;
                }
                let lv = self.expr_before(l, &[r]);
                let rv = self.expr(r);
                // (`==` of an `any`: JavaScript's loose equality, `"5" == 5`)
                let strict = !(matches!(op, LooseEq | LooseNe) && (lv.ty == ANY || rv.ty == ANY));
                let eq = self.equality(lv, rv, strict);
                Val::plain(if matches!(op, Eq | LooseEq) { eq } else { format!("(!{eq})") }, BOOL)
            }
            Lt | Gt | Le | Ge => {
                if let Some(v) = self.compare_past_end(op, l, r) {
                    return v;
                }
                let lv = self.expr_before(l, &[r]);
                let rv = self.expr(r);
                // (what a type parameter stands for: an object with a `valueOf()`, a `Date`,
                // compares by its value)
                let (lv, rv) = (self.primitive_of(lv), self.primitive_of(rv));
                let o = op.as_str();
                if self.c.types.is_string(lv.ty) {
                    // (against a one-character ASCII literal, `c >= "0"`: its first byte and length)
                    let m = self.cur_m();
                    let one = |g: &Self, e: ExprId| -> Option<u8> {
                        let mut e = e;
                        while let ExprKind::Paren(x) = g.ast(m).expr(e).kind {
                            e = x;
                        }
                        match g.ast(m).expr(e).kind {
                            ExprKind::Str(s) => {
                                let t = g.sym(s).as_bytes();
                                (t.len() == 1 && t[0] < 0x80).then(|| t[0])
                            }
                            _ => None,
                        }
                    };
                    let (lc, rc) = (one(self, l), one(self, r));
                    let (a, b) = (self.coerce(lv, STR), self.coerce(rv, STR));
                    if let Some(c) = rc {
                        return Val::plain(format!("(tv_str_cmp1({}, {c}) {o} 0)", a.code), BOOL);
                    }
                    if let Some(c) = lc {
                        return Val::plain(format!("(0 {o} tv_str_cmp1({}, {c}))", b.code), BOOL);
                    }
                    return Val::plain(format!("(tv_str_cmp({}, {}) {o} 0)", a.code, b.code), BOOL);
                }
                if lv.ty == DECIMAL || rv.ty == DECIMAL {
                    let (a, b) = (self.coerce(lv, DECIMAL), self.coerce(rv, DECIMAL));
                    return Val::plain(format!("(tv_dec_cmp({}, {}) {o} 0)", a.code, b.code), BOOL);
                }
                if lv.ty == BIGINT || rv.ty == BIGINT {
                    // (an integer side is compared exactly, as in JavaScript)
                    return Val::plain(match (lv.ty == BIGINT, rv.ty == BIGINT) {
                        (true, true) => format!("(tv_big_cmp({}, {}) {o} 0)", lv.code, rv.code),
                        // (a number: exactly, and every comparison with NaN false)
                        (true, false) if self.c.types.is_float(rv.ty) => {
                            let b = self.coerce(rv, F64);
                            format!("({{ int c_ = tv_big_cmp_f64({}, {}); c_ != 2 && c_ {o} 0; }})", lv.code, b.code)
                        }
                        (false, true) if self.c.types.is_float(lv.ty) => {
                            let a = self.coerce(lv, F64);
                            format!("({{ int c_ = tv_big_cmp_f64({}, {}); c_ != 2 && 0 {o} c_; }})", rv.code, a.code)
                        }
                        (true, false) => {
                            let b = self.int_code(rv);
                            format!("(tv_big_cmp_int({}, {b}) {o} 0)", lv.code)
                        }
                        _ => {
                            let a = self.int_code(lv);
                            format!("(0 {o} tv_big_cmp_int({}, {a}))", rv.code)
                        }
                    }, BOOL);
                }
                // (what a type parameter stands for: a `boolean` compares as a number, anything
                // else by its `String(x)`, as JavaScript's)
                if lv.ty == BOOL && rv.ty == BOOL {
                    return Val::plain(format!("((int)({}) {o} (int)({}))", lv.code, rv.code), BOOL);
                }
                if !self.c.types.is_numeric(lv.ty) || !self.c.types.is_numeric(rv.ty) {
                    let (a, b) = (self.js_string(lv), self.js_string(rv));
                    return Val::plain(format!("(tv_str_cmp({}, {}) {o} 0)", a.code, b.code), BOOL);
                }
                let (a, b) = self.numeric_operands(lv, rv);
                Val::plain(format!("({} {o} {})", a, b), BOOL)
            }
            Add if ty == STR => {
                // `a + b + c + d` of strings: one string, made once (not one per `+`)
                let m = self.cur_m();
                let mut parts = vec![r];
                let mut cur = l;
                loop {
                    let ast = self.ast(m);
                    match ast.expr(cur).kind {
                        ExprKind::Binary(BinOp::Add, a, b) if self.facts(m).expr_ty.get(cur as usize) == Some(&STR) => {
                            parts.push(b);
                            cur = a;
                        }
                        _ => break,
                    }
                }
                parts.push(cur);
                parts.reverse();
                let all_str = |g: &Self| parts.iter().all(|&x| g.c.types.is_string(g.facts(m).expr_ty.get(x as usize).copied().unwrap_or(ERROR)));
                if parts.len() >= 3 && all_str(self) {
                    let mut codes = Vec::new();
                    for (k, &x) in parts.iter().enumerate() {
                        let v = if k + 1 < parts.len() { self.expr_before(x, &parts[k + 1..]) } else { self.expr(x) };
                        let v = self.coerce(v, STR);
                        codes.push(v.code);
                    }
                    let n = codes.len();
                    return self.tmp(STR, &format!("tv_str_concatn((tv_str[]){{{}}}, {n})", codes.join(", ")), true);
                }
                // (`"v" + k + i`, strings and numbers: each part's text into one builder, once
                // the chain is a string from its first `+` on, as each `+` of it would)
                let plain = |g: &Self, x: ExprId| {
                    let t = g.facts(m).expr_ty.get(x as usize).copied().unwrap_or(ERROR);
                    matches!(g.tget(t), Ty::Str | Ty::StrLit(_) | Ty::Int | Ty::F64 | Ty::Bool)
                };
                if parts.len() >= 3 && parts.iter().all(|&x| plain(self, x)) {
                    let mut vals = Vec::new();
                    for (k, &x) in parts.iter().enumerate() {
                        let v = if k + 1 < parts.len() { self.expr_before(x, &parts[k + 1..]) } else { self.expr(x) };
                        vals.push(v);
                    }
                    let sbv = self.fresh("sb");
                    let stack = self.stack_builder(&sbv);
                    if stack.is_none() {
                        self.open("{");
                    }
                    self.line(format!("tv_sb *sb = &{sbv};"));
                    for v in vals {
                        let c = self.string_code(v.ty, &v.code);
                        self.line(format!("{c};"));
                    }
                    return self.end_builder(&sbv, stack);
                }
                let lv = self.expr_before(l, &[r]);
                let rv = self.expr(r);
                if lv.ty == STR && rv.ty == STR {
                    return self.tmp(STR, &format!("tv_str_concat({}, {})", lv.code, rv.code), true);
                }
                let sbv = self.fresh("sb");
                let stack = self.stack_builder(&sbv);
                if stack.is_none() {
                    self.open("{");
                }
                self.line(format!("tv_sb *sb = &{sbv};"));
                for v in [lv, rv] {
                    let c = self.string_code(v.ty, &v.code);
                    self.line(format!("{c};"));
                }
                self.end_builder(&sbv, stack)
            }
            Add | Sub if ty == BIGINT && (self.big_mul_parts(r).is_some() || (op == Add && self.big_mul_parts(l).is_some())) => {
                // `z + x * y`, `x * y + z`, `z - x * y`: one pass (see tv_big_muladd)
                let sign = if op == Add { 1 } else { -1 };
                let (z, x, y) = if let Some((a, b)) = self.big_mul_parts(r) {
                    let z = self.expr_before(l, &[a, b]);
                    let x = self.expr_before(a, &[b]);
                    (z, x, self.expr(b))
                } else {
                    let (a, b) = self.big_mul_parts(l).unwrap();
                    let x = self.expr_before(a, &[b, r]);
                    let y = self.expr_before(b, &[r]);
                    (self.expr(r), x, y)
                };
                if z.owned {
                    let zc = self.consume(z);
                    let t = self.fresh("t");
                    self.line(format!("tv_big {t} = {zc}; tv_big_muladd_to(&{t}, {}, {}, {sign});", x.code, y.code));
                    let v = self.tmp(BIGINT, &t, true);
                    return v;
                }
                self.tmp(BIGINT, &format!("tv_big_muladd({}, {}, {}, {sign})", x.code, y.code, z.code), true)
            }
            Add | Sub | Mul | Div | Rem | Pow | BitAnd | BitOr | BitXor | Shl | Shr if ty == BIGINT => {
                let lv = self.expr_before(l, &[r]);
                let rv = self.expr(r);
                self.big_op(op, lv, rv, span)
            }
            Add | Sub | Mul | Div | Rem | Pow => {
                let lv = self.expr_before(l, &[r]);
                let rv = self.expr(r);
                if op == Rem && ty == INT && lv.ty == INT && rv.ty == INT {
                    let m = self.cur_m();
                    if let Some(IdentFact::Local(k)) = self.ident_fact(m, r)
                        && let Some(dv) = self.b().divs.get(&k).cloned()
                    {
                        let loc = self.loc(span);
                        return Val::plain(format!("TVG_REM_BY({}, {}, {dv}, {loc})", lv.code, rv.code), INT);
                    }
                }
                // `a % b` of a value that's never negative by a positive one: an unsigned remainder
                // (no sign to fix up), of 32 bits when both fit (`seed % IM` in an LCG)
                if op == Rem && ty == INT && lv.ty == INT && rv.ty == INT
                    && let (super::range::R::In(a0, a1), super::range::R::In(b0, b1)) = (self.int_range(l), self.int_range(r))
                    && a0 >= 0
                    && b0 > 0
                {
                    let ut = if a1 <= u32::MAX as i128 && b1 <= u32::MAX as i128 { "uint32_t" } else { "uint64_t" };
                    return Val::plain(format!("((tv_int)(({ut})({}) % ({ut})({})))", lv.code, rv.code), INT);
                }
                // (no overflow check where the operands' values can't overflow)
                // (`h * 31 + bytes[i]`: smaller integer types widen to `int` exactly)
                if matches!(op, Add | Sub | Mul) && ty == INT && self.c.types.is_int(lv.ty) && self.c.types.is_int(rv.ty) && lv.ty != U64 && rv.ty != U64 && self.cannot_overflow(op, l, r) {
                    let o = match op { Add => "+", Sub => "-", _ => "*" };
                    let wide = |v: &Val| if v.ty == INT { v.code.clone() } else { format!("((tv_int)({}))", v.code) };
                    return Val::plain(format!("({} {o} {})", wide(&lv), wide(&rv)), INT);
                }
                self.js_mul_operand = has_mul(self.ast(self.cur_m()), l) || has_mul(self.ast(self.cur_m()), r);
                self.arith(op, lv, rv, ty, span)
            }
            BitAnd | BitOr | BitXor | Shl | Shr | UShr => {
                let lv = self.expr_before(l, &[r]);
                let rv = self.expr(r);
                Val::plain(self.bitop(op, &lv, &rv, ty), ty)
            }
            Instanceof => {
                let v = self.expr(l);
                let m = self.cur_m();
                match self.ident_fact(m, r) {
                    Some(IdentFact::Class(c)) => {
                        let code = self.instanceof_code(v, c);
                        Val::plain(code, BOOL)
                    }
                    _ => Val::plain("false", BOOL),
                }
            }
            In => {
                // the key (a literal: known by name), then the value
                let lit = match self.ast(self.cur_m()).expr(l).kind {
                    ExprKind::Str(k) => Some(k),
                    _ => None,
                };
                let kv = self.expr_before(l, &[r]);
                let kv = self.coerce(kv, STR);
                let kv = self.own(kv);
                let kn = self.fresh("key");
                let kc = self.consume(kv);
                self.line(format!("tv_str {kn} = {kc};"));
                self.b().temps.last_mut().unwrap().push((kn.clone(), STR));
                let v = self.expr(r);
                let code = self.in_code(v, lit, &kn);
                Val::plain(code, BOOL)
            }
        }
    }

    /// `key in v`: by `v`'s type (a union's by its tag); a map's or an `unknown`'s by what it
    /// holds; an optional field by whether it's there. `lit`: the key, when it's a literal.
    fn in_code(&mut self, v: Val, lit: Option<crate::intern::Sym>, key: &str) -> String {
        match self.tget(v.ty) {
            Ty::Unknown => {
                self.dyn_used();
                format!("tvg_dyn_has({}, {key})", v.code)
            }
            // (a record's keys; a `Map`'s entries aren't properties: `"k" in new Map()` is false)
            Ty::Map(k, mv) if k == STR => {
                let (kd, vd) = (self.desc(k), self.desc(mv));
                format!("(tv_map_is_object({0}) && tv_map_has({0}, {kd}, {vd}, &{key}))", v.code)
            }
            Ty::Rec(..) => {
                let u = self.unfold_val(v);
                self.in_code(u, lit, key)
            }
            Ty::Union(ms) if self.niche(v.ty).is_some() => {
                let (pi, _) = self.niche(v.ty).unwrap();
                let m = self.c.types.tys(ms)[pi];
                let there = self.niche_there(&v.code, v.ty);
                let inner = self.in_code(Val { code: v.code.clone(), ty: m, owned: false }, lit, key);
                format!("({there} && {inner})")
            }
            Ty::Union(ms) => {
                let ms = self.c.types.tys(ms).to_vec();
                let tag = self.u_tag(&v.code, v.ty);
                let mut parts = Vec::new();
                for (i, &m) in ms.iter().enumerate() {
                    let payload = if self.is_unit(m) { Val::plain("0", m) } else { Val { code: self.u_payload(&v.code, v.ty, i), ty: m, owned: false } };
                    let c = self.in_code(payload, lit, key);
                    if c != "false" {
                        parts.push(format!("({tag} == {i} && {c})"));
                    }
                }
                if parts.is_empty() { "false".into() } else { format!("({})", parts.join(" || ")) }
            }
            // (a class without it: an object of a subclass that has it does)
            Ty::Class(c, _) if lit.is_some_and(|n| self.c.has_key(v.ty, n) == Some(false)) => {
                let name = lit.unwrap();
                let n = self.c.classes.len() as u32;
                let mut tests = Vec::new();
                for d in 0..n {
                    if d == c || !self.c.class_descends(d, c) {
                        continue;
                    }
                    let dt = self.c.classes[d as usize].this_ty;
                    if self.c.has_key(dt, name) == Some(true) {
                        tests.push(self.instanceof_code(v.clone(), d));
                    }
                }
                if tests.is_empty() { "false".into() } else { format!("({})", tests.join(" || ")) }
            }
            // (a key of a record with an index signature that isn't one of its fields: in `$rest`)
            Ty::Record(fs) if self.rest_field(v.ty).is_some() && lit.is_none_or(|n| !self.c.types.fields(fs).iter().any(|f| f.name == n)) => {
                let rt = self.rest_field(v.ty).unwrap();
                let mt = self.c.types.without_undefined(rt);
                let Ty::Map(mk, mv) = self.tget(mt) else { return "false".into() };
                let place = self.fld(v.ty, &v.code, "$rest");
                let present = self.truthy_val(Val::plain(place.clone(), rt));
                let m = self.project(Val::plain(place, rt), mt);
                let (kd, vd) = (self.desc(mk), self.desc(mv));
                let mut tests = vec![format!("({present} && tv_map_has({}, {kd}, {vd}, &{key}))", m.code)];
                // (a computed key: one of the fields too)
                if lit.is_none() {
                    let rest = self.c.interner.lookup("$rest");
                    for f in self.c.types.fields(fs).to_vec() {
                        if Some(f.name) == rest {
                            continue;
                        }
                        let l = self.lit(self.sym(f.name).to_string().as_bytes());
                        tests.push(format!("tv_str_eq({key}, {l})"));
                    }
                }
                format!("({})", tests.join(" || "))
            }
            _ => {
                let Some(name) = lit else { return "false".into() };
                match self.c.has_key(v.ty, name) {
                    Some(true) => "true".into(),
                    Some(false) => "false".into(),
                    // (an optional field: there unless it's undefined)
                    None => {
                        let n = self.sym(name).to_string();
                        let ft = match self.tget(v.ty) {
                            Ty::Record(fs) => self.c.types.fields(fs).iter().find(|f| f.name == name).map(|f| f.ty),
                            _ => None,
                        };
                        match ft {
                            Some(ft) => {
                                let place = self.fld(v.ty, &v.code, &n);
                                match self.niche(ft) {
                                    Some(_) => self.niche_there(&place, ft),
                                    None => match self.tag_of(ft, UNDEFINED) {
                                        Some(k) => format!("(!{})", self.u_is(&place, ft, k)),
                                        None => "true".into(),
                                    },
                                }
                            }
                            None => "true".into(),
                        }
                    }
                }
            }
        }
    }

    /// A bitwise operator. On `int` and floats it's JavaScript's: both sides wrap to 32 bits
    /// (ToInt32), shift counts are taken mod 32, and `>>>` gives an unsigned 32-bit result.
    /// Sized integers keep their own width.
    fn bitop(&mut self, op: BinOp, l: &Val, r: &Val, ty: TyId) -> String {
        use BinOp::*;
        let (a, b) = (&l.code, &r.code);
        if ty != INT {
            let ct = self.ctype(ty);
            return match op {
                BitAnd => format!("(({ct})({a} & {b}))"),
                BitOr => format!("(({ct})({a} | {b}))"),
                BitXor => format!("(({ct})({a} ^ {b}))"),
                Shl => format!("(({ct})((uint64_t)({a}) << ((uint64_t)({b}) & 63)))"),
                Shr => format!("(({ct})(({a}) >> ((uint64_t)({b}) & 63)))"),
                _ => format!("(({ct})((uint64_t)({a}) >> ((uint64_t)({b}) & 63)))"),
            };
        }
        let (a, b) = (self.int32_code(l), self.int32_code(r));
        match op {
            BitAnd => format!("((tv_int)({a} & {b}))"),
            BitOr => format!("((tv_int)({a} | {b}))"),
            BitXor => format!("((tv_int)({a} ^ {b}))"),
            Shl => format!("((tv_int)(int32_t)((uint32_t){a} << ({b} & 31)))"),
            Shr => format!("((tv_int)({a} >> ({b} & 31)))"),
            _ => format!("((tv_int)((uint32_t){a} >> ({b} & 31)))"),
        }
    }

    /// A number as JavaScript's ToInt32 sees it (an `int32_t`).
    pub(super) fn int32_code(&self, v: &Val) -> String {
        if self.c.types.is_float(v.ty) {
            format!("tv_f64_to_i32({})", v.code)
        } else if v.ty == INT {
            format!("((int32_t)({}))", v.code)
        } else {
            format!("((int32_t)(tv_int)({}))", v.code)
        }
    }

    /// `x * y` of bigints (through parentheses): its operands.
    fn big_mul_parts(&mut self, e: ExprId) -> Option<(ExprId, ExprId)> {
        let m = self.cur_m();
        let mut e = e;
        while let ExprKind::Paren(x) = self.ast(m).expr(e).kind {
            e = x;
        }
        match self.ast(m).expr(e).kind {
            ExprKind::Binary(BinOp::Mul, a, b) if self.ty(e) == BIGINT => Some((a, b)),
            _ => None,
        }
    }

    /// A bigint operator. An owned left operand is consumed by `+`, `-` and `*`, whose result
    /// reuses its buffer when nothing else holds it (`(a * b + c) * d` allocates once).
    fn big_op(&mut self, op: BinOp, lv: Val, rv: Val, span: Span) -> Val {
        use BinOp::*;
        let (a, b) = (lv.code.clone(), rv.code.clone());
        let code = match op {
            Add | Sub | Mul if lv.owned => {
                let a = self.consume(lv);
                let f = match op { Add => "add", Sub => "sub", _ => "mul" };
                format!("tv_big_{f}_take({a}, {b})")
            }
            // (the right operand's buffer, when it's a temporary: `+` and `*` commute, and
            // `a - b` is `-(b - a)`)
            Add | Sub | Mul if rv.owned => {
                let b = self.consume(rv);
                let f = match op { Add => "add_take", Sub => "rsub_take", _ => "mul_take" };
                format!("tv_big_{f}({b}, {a})")
            }
            Add => format!("tv_big_add({a}, {b})"),
            Sub => format!("tv_big_sub({a}, {b})"),
            Mul => format!("tv_big_mul({a}, {b})"),
            Div | Rem | Pow => {
                let loc = self.loc(span);
                let f = match op { Div => "div", Rem => "rem", _ => "pow" };
                format!("tv_big_{f}({a}, {b}, {loc})")
            }
            BitAnd => format!("tv_big_bitop({a}, {b}, 0)"),
            BitOr => format!("tv_big_bitop({a}, {b}, 1)"),
            BitXor => format!("tv_big_bitop({a}, {b}, 2)"),
            _ => {
                let loc = self.loc(span);
                format!("tv_big_shift({a}, {b}, {}, {loc})", op == Shl)
            }
        };
        self.tmp(BIGINT, &code, true)
    }

    fn numeric_operands(&mut self, a: Val, b: Val) -> (String, String) {
        if a.ty == b.ty {
            return (a.code, b.code);
        }
        let target = if self.c.types.is_float(a.ty) || self.c.types.is_float(b.ty) { F64 } else { INT };
        let a = self.coerce(a, target);
        let b = self.coerce(b, target);
        (a.code, b.code)
    }

    pub(crate) fn arith(&mut self, op: BinOp, lv: Val, rv: Val, ty: TyId, span: Span) -> Val {
        use BinOp::*;
        if ty == DECIMAL {
            let (a, b) = (self.coerce(lv, DECIMAL), self.coerce(rv, DECIMAL));
            let code = match op {
                Add => format!("tv_dec_add({}, {})", a.code, b.code),
                Sub => format!("tv_dec_sub({}, {})", a.code, b.code),
                Mul => format!("tv_dec_mul({}, {})", a.code, b.code),
                Div => format!("tv_dec_div({}, {}, {})", a.code, b.code, self.loc(span)),
                _ => format!("tv_dec_rem({}, {}, {})", a.code, b.code, self.loc(span)),
            };
            return Val::plain(code, DECIMAL);
        }
        let ct = self.ctype(ty);
        if self.c.types.is_float(ty) {
            let a = self.coerce(lv, ty);
            let b = self.coerce(rv, ty);
            let code = match op {
                Add => format!("({} + {})", a.code, b.code),
                Sub => format!("({} - {})", a.code, b.code),
                Mul => format!("({} * {})", a.code, b.code),
                Div => format!("({} / {})", a.code, b.code),
                Rem => format!("tv_fmod({}, {})", a.code, b.code),
                _ => format!("pow({}, {})", a.code, b.code),
            };
            return Val::plain(code, ty);
        }
        let loc = self.loc(span);
        let unsigned = matches!(self.tget(ty), Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64);
        let (a, b) = (lv.code, rv.code);
        let js = ty == INT && self.js_ints(self.cur_m());
        let js_sum = js && std::mem::take(&mut self.js_mul_operand);
        // (`n - 1`: a constant to add or take away, one test)
        let konst = |s: &str| s.strip_prefix("INT64_C(").and_then(|x| x.strip_suffix(')')).and_then(|x| x.parse::<i64>().ok()).filter(|&k| k > 0 && k < 1 << 40);
        let code = match op {
            Add if js && konst(&b).is_some() => format!("TV_JADDK({a}, {b}, {loc})"),
            Add if js && konst(&a).is_some() => format!("TV_JADDK({b}, {a}, {loc})"),
            Sub if js && konst(&b).is_some() => format!("TV_JSUBK({a}, {b}, {loc})"),
            Add if js_sum => format!("TV_JADD({a}, {b}, {loc})"),
            Sub if js_sum => format!("TV_JSUB({a}, {b}, {loc})"),
            Mul if js && self.float_only != Some(span) => format!("TV_JMUL({a}, {b}, {loc})"),
            Add => format!("TV_ADD({ct}, {a}, {b}, {loc})"),
            Sub => format!("TV_SUB({ct}, {a}, {b}, {loc})"),
            Mul => format!("TV_MUL({ct}, {a}, {b}, {loc})"),
            Rem if unsigned => format!("TV_UREM({ct}, {a}, {b}, {loc})"),
            Rem => format!("TV_REM({ct}, {a}, {b}, {loc})"),
            _ => format!("tv_pow_int({a}, {b}, {loc})"),
        };
        Val::plain(code, ty)
    }

    /// `a === b` as a C boolean.
    pub(crate) fn equality(&mut self, a: Val, b: Val, strict: bool) -> String {
        let (ta, tb) = (a.ty, b.ty);
        // (`==` with an `any`: JavaScript's loose equality)
        if !strict && (ta == ANY || tb == ANY) && !matches!(self.tget(if ta == ANY { tb } else { ta }), Ty::Undefined | Ty::Null | Ty::Void) {
            let (x, y) = (self.coerce(a, ANY), self.coerce(b, ANY));
            return format!("tv_any_loose_eq({}, {})", x.code, y.code);
        }
        if ta == UNKNOWN || tb == UNKNOWN || ta == ANY || tb == ANY {
            // (against `undefined`/`null`: what it holds; `==` takes either)
            let (d, other) = if ta == UNKNOWN || ta == ANY { (a, b) } else { (b, a) };
            if matches!(self.tget(other.ty), Ty::Undefined | Ty::Null | Ty::Void) {
                let what = if self.tget(other.ty) == Ty::Null { "tv_type_null" } else { "tv_type_undefined" };
                return if strict {
                    format!("(({}).t == &{what})", d.code)
                } else {
                    format!("({{ tv_dyn d_ = {}; d_.t == &tv_type_undefined || d_.t == &tv_type_null; }})", d.code)
                };
            }
            let o = self.coerce(other, UNKNOWN);
            return format!("tv_dyn_eq({}, {})", d.code, o.code);
        }
        if ta == JS || tb == JS {
            // `=== undefined`: Tov has no `null`; JavaScript's reads as `undefined`
            if ta == UNDEFINED || tb == UNDEFINED {
                let v = if ta == JS { a } else { b };
                return format!("tv_js_is_nullish({})", v.code);
            }
            // JavaScript's ===
            let (a, b) = (self.js_of(a), self.js_of(b));
            return self.eq_code(JS, &a.code, &b.code);
        }
        if (ta == DECIMAL || tb == DECIMAL) && self.c.types.is_num_like(ta) && self.c.types.is_num_like(tb) {
            let (x, y) = (self.coerce(a, DECIMAL), self.coerce(b, DECIMAL));
            return format!("tv_dec_eq({}, {})", x.code, y.code);
        }
        if self.c.types.is_numeric(ta) && self.c.types.is_numeric(tb) {
            let (x, y) = self.numeric_operands(a, b);
            return format!("({x} == {y})");
        }
        if ta == tb {
            return self.eq_code(ta, &a.code, &b.code);
        }
        // Literal vs literal / string.
        match (self.tget(ta), self.tget(tb)) {
            (Ty::StrLit(x), Ty::StrLit(y)) => return if x == y { "true".into() } else { "false".into() },
            (Ty::StrLit(_), Ty::Str) | (Ty::Str, Ty::StrLit(_)) => {
                let a = self.coerce(a, STR);
                let b = self.coerce(b, STR);
                return format!("tv_str_eq({}, {})", a.code, b.code);
            }
            _ => {}
        }
        // (a recursive alias against a plain value, `op === "if"`: what it holds, a union)
        let plain = |c: &Self, t: TyId| matches!(c.tget(t), Ty::StrLit(_) | Ty::Str | Ty::Int | Ty::F64 | Ty::Null | Ty::Undefined | Ty::Bool | Ty::BoolLit(_));
        if matches!(self.tget(ta), Ty::Rec(..)) && plain(self, tb) && is_simple_code(&a.code) {
            let ua = self.unfold_val(a.clone());
            if matches!(self.tget(ua.ty), Ty::Union(_)) {
                return self.equality(ua, b, strict);
            }
        }
        if matches!(self.tget(tb), Ty::Rec(..)) && plain(self, ta) && is_simple_code(&b.code) {
            let ub = self.unfold_val(b.clone());
            if matches!(self.tget(ub.ty), Ty::Union(_)) {
                return self.equality(a, ub, strict);
            }
        }
        // A union compared with one of its members (e.g. `x === undefined`, `s.kind === "circle"`).
        for (u, x) in [(&a, &b), (&b, &a)] {
            if let Ty::Union(_) = self.tget(u.ty) {
                let target = self.member_target(u.ty, x.ty);
                if let Some(mt) = target {
                    let k = self.tag_of(u.ty, mt).unwrap();
                    if self.is_unit(mt) {
                        return self.u_is(&u.code, u.ty, k);
                    }
                    let xv = self.coerce(x.clone(), mt);
                    let inner = self.eq_code(mt, &self.u_payload(&u.code, u.ty, k), &xv.code);
                    return format!("({} && {inner})", self.u_is(&u.code, u.ty, k));
                }
                if let Ty::Str = self.tget(x.ty) {
                    // Union of literals compared with a string: convert the union.
                    let us = self.coerce(u.clone(), STR);
                    return format!("tv_str_eq({}, {})", us.code, x.code);
                }
            }
        }
        // Different unions: convert one to the other if possible.
        if self.c.assignable_pub(ta, tb) {
            let a2 = self.coerce(a, tb);
            return self.eq_code(tb, &a2.code, &b.code);
        }
        if self.c.assignable_pub(tb, ta) {
            let b2 = self.coerce(b, ta);
            return self.eq_code(ta, &a.code, &b2.code);
        }
        "false".into()
    }

    // ------------------------------------------------------------ members and indexing

    /// `.length` of `call`, when it's a string's slice, substring or substr of int arguments:
    /// tv_str_sub_len's answer.
    fn sub_len(&mut self, call: ExprId) -> Option<Val> {
        let ast = self.ast(self.cur_m());
        let ExprKind::Call { callee, args, optional: false, type_args } = &ast.expr(call).kind else { return None };
        let ExprKind::Member { obj: recv, name: mn, optional: false, .. } = &ast.expr(*callee).kind else { return None };
        let kind = match self.sym(*mn) {
            "slice" => 0,
            "substring" => 1,
            "substr" => 2,
            _ => return None,
        };
        let (recv, args) = (*recv, args.iter().map(|a| (a.expr, a.by_ref.is_some())).collect::<Vec<_>>());
        let rt = self.ty(recv);
        if !type_args.is_empty() || args.len() > 2 || args.iter().any(|&(x, r)| r || self.ty(x) != INT) || !matches!(self.tget(rt), Ty::Str | Ty::StrLit(_)) {
            return None;
        }
        let s = self.expr(recv);
        let s = self.coerce(s, STR);
        let mut ints = Vec::new();
        for &(x, _) in &args {
            let v = self.expr(x);
            ints.push(self.int_code(v));
        }
        let a = ints.first().cloned().unwrap_or_else(|| "0".into());
        let (b, has_b) = match ints.get(1) {
            Some(b) => (b.clone(), "true"),
            None => ("0".into(), "false"),
        };
        Some(Val::plain(format!("tv_str_sub_len({}, {kind}, {a}, {b}, {has_b})", s.code), INT))
    }

    fn member(&mut self, e: ExprId, obj: ExprId, name: crate::intern::Sym, optional: bool, ty: TyId, span: Span) -> Val {
        let m = self.cur_m();
        // (a builder's count)
        if !optional
            && self.sym(name) == "length"
            && let Some(IdentFact::Local(k)) = self.ident_fact(m, obj)
            && let Some((_, n, _)) = self.b().builders.get(&k).cloned()
        {
            return Val::plain(n, INT);
        }
        // `s.slice(a, b).length` (substring, substr): the length alone, no string made
        if !optional && self.sym(name) == "length"
            && let Some(v) = self.sub_len(obj)
        {
            return v;
        }
        // (a property of what an `any` holds)
        if self.ty(obj) == ANY {
            let base = self.expr(obj);
            let loc = self.loc(span);
            let text = self.sym(name).to_string();
            return self.any_member(base, &text, optional, ty, &loc);
        }
        if self.facts(m).asserted_reads.contains(&e) {
            let base = self.expr(obj);
            let loc = self.loc(span);
            let ft = self.c.types.without_undefined(base.ty);
            let field_ty = self.c.field_of_pub(ft, name).unwrap_or(ty);
            let raw = self.field(base, name, field_ty, span);
            self.checked_project = Some(loc);
            let v = self.project(raw, ty);
            self.checked_project = None;
            return v;
        }
        let ot = self.ty(obj);
        if self.c.types.without_undefined(ot) == JS {
            if let Some(v) = self.js_try_fuse(e, ty, span) {
                if self.facts(m).throwing.contains(&e) {
                    self.error_check();
                }
                return v;
            }
            let base = self.expr(obj);
            let base = self.js_of(base);
            return self.js_member(e, base, name, optional, ty, span);
        }
        match self.facts(m).members.get(&e).copied() {
            Some(MemberFact::NsFn(fm, fi)) => {
                let code = self.fn_value(fm, fi);
                return Val::plain(code, ty);
            }
            Some(MemberFact::NsConst(cm, ci)) => return self.const_ref(cm, ci),
            Some(MemberFact::StaticField(c, mi)) => return self.static_field_ref(c, mi),
            Some(MemberFact::StaticFn(c, mi)) => {
                let subst: FxMap<u32, TyId> = self.facts(m).value_targs.get(&e).map(|ts| ts.iter().map(|&(p, t)| (p, t)).collect()).unwrap_or_default();
                let subst: FxMap<u32, TyId> = subst.into_iter().map(|(p, t)| (p, self.inst(t))).collect();
                let code = self.static_fn_value(c, mi, subst);
                return Val::plain(code, ty);
            }
            Some(MemberFact::TupleLength(n)) => {
                let v = self.expr(obj);
                if optional && self.c.types.has_nullish(v.ty) {
                    let present = self.truthy(v);
                    let some = self.coerce(Val::plain(format!("INT64_C({n})"), INT), ty);
                    let none = self.coerce(Val::plain("0", UNDEFINED), ty);
                    return Val::plain(format!("(({present}) ? {} : {})", some.code, none.code), ty);
                }
                return Val::plain(format!("INT64_C({n})"), INT);
            }
            Some(MemberFact::ClassName(inst)) => {
                let v = self.expr(inst);
                let Some((decl, _)) = self.c.class_of(v.ty) else { return Val::plain("TV_EMPTY_STR", STR) };
                let has_subclass = (0..self.c.classes.len() as u32).any(|d| d != decl && self.c.class_descends(d, decl));
                if !has_subclass {
                    let name = self.c.class_names[decl as usize].clone();
                    let code = self.lit(name.as_bytes());
                    return Val::plain(code, STR);
                }
                // (the object's class, read from its header)
                self.cid_decls.insert(decl);
                for d in 0..self.c.classes.len() as u32 {
                    if self.c.class_descends(d, decl) {
                        self.cid_decls.insert(d);
                    }
                }
                if !self.class_names_used {
                    self.class_names_used = true;
                    let _ = writeln!(self.protos, "static tv_str tvg_cname(uint32_t cid);");
                }
                return self.tmp(STR, &format!("tvg_cname(((tvg_obj *)({}))->cid)", v.code), true);
            }
            Some(MemberFact::First(coll, values)) => {
                let cv = self.expr(coll);
                let ct = self.c.types.without_undefined(cv.ty);
                let (k, v, set) = match self.tget(ct) {
                    Ty::Map(k, v) => (k, v, false),
                    Ty::Set(k) => (k, UNDEFINED, true),
                    // (an array: its first element, or index 0)
                    Ty::Array(el) => {
                        let (et, res) = (if values { el } else { INT }, self.fresh("t"));
                        let (ect, ct2) = (self.ctype(el), self.ctype(ty));
                        self.line(format!("{ct2} {res};"));
                        self.open_block(&format!("if (tv_arr_len({}) > 0) {{", cv.code));
                        let first = if values { Val::plain(format!("(({ect} *)tv_arr_data({}))[0]", cv.code), et) } else { Val::plain("INT64_C(0)", INT) };
                        let found = self.coerce(first, ty);
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
                        return Val { code: res, ty, owned };
                    }
                    _ => return Val::plain("0", ty),
                };
                let (kd, vd) = (self.desc(k), if set { "&tv_type_undefined".to_string() } else { self.desc(v) });
                let et = if values && !set { v } else { k };
                let ect = self.ctype(et);
                let (cur, kp, vp, res) = (self.fresh("i"), self.fresh("k"), self.fresh("v"), self.fresh("t"));
                self.line(format!("tv_int {cur} = 0; void *{kp}, *{vp};"));
                let ct2 = self.ctype(ty);
                self.line(format!("{ct2} {res};"));
                self.open_block(&format!("if (tv_map_next({}, {kd}, {vd}, &{cur}, &{kp}, &{vp})) {{", cv.code));
                let p = if values && !set { &vp } else { &kp };
                let found = self.coerce(Val::plain(format!("(*({ect} *){p})"), et), ty);
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
                return Val { code: res, ty, owned };
            }
            // `req.params` in a route handler: the record of the route's params, from the map.
            Some(MemberFact::RouteParams(rt)) => {
                let base = self.expr(obj);
                let map_ty = self.c.types.intern(Ty::Map(STR, STR));
                let mv = self.field(base, name, map_ty, span);
                let res = self.fresh("rp");
                let ct = self.ctype(rt);
                if self.is_obj(rt) {
                    let d = self.default_value(rt);
                    self.line(format!("{ct} {res} = {d};"));
                } else {
                    self.line(format!("{ct} {res};"));
                }
                let Ty::Record(fs) = self.tget(rt) else { return Val::plain("0", ty) };
                for f in self.c.types.fields(fs).to_vec() {
                    let fname = self.sym(f.name).to_string();
                    let key = self.lit(fname.as_bytes());
                    let (kn, pn) = (self.fresh("k"), self.fresh("p"));
                    let place = self.fld(rt, &res, &fname);
                    self.line(format!("tv_str {kn} = {key}; tv_str *{pn} = tv_map_get({}, &tv_type_str, &tv_type_str, &{kn});", mv.code));
                    self.line(format!("{place} = {pn} ? *{pn} : TV_EMPTY_STR; tv_str_retain({place});"));
                }
                self.b().temps.last_mut().unwrap().push((res.clone(), rt));
                return self.coerce(Val { code: res, ty: rt, owned: true }, ty);
            }
            Some(MemberFact::Env(var)) => {
                let lit = self.lit(self.sym(var).to_string().as_bytes());
                let (ok, sv) = (self.fresh("ok"), self.fresh("ev"));
                self.line(format!("tv_str {sv} = TV_EMPTY_STR; bool {ok} = tv_process_env({lit}, &{sv});"));
                self.b().temps.last_mut().unwrap().push((sv.clone(), STR));
                let v = Val { code: sv, ty: STR, owned: false };
                return self.optional_from_pub(&ok, v, ty);
            }
            Some(MemberFact::MathConst) if matches!(&self.ast(m).expr(obj).kind, ExprKind::Ident(s) if self.sym(*s) == "process") => {
                return match self.sym(name) {
                    "argv" => self.tmp(ty, "tv_process_argv()", true),
                    "platform" => self.tmp(STR, "tv_str_from(TVG_PLATFORM, sizeof(TVG_PLATFORM) - 1)", true),
                    "arch" => self.tmp(STR, "tv_str_from(TVG_ARCH, sizeof(TVG_ARCH) - 1)", true),
                    "pid" => self.tmp(INT, "tv_process_pid()", false),
                    _ => Val::plain("0", ty),
                };
            }
            Some(MemberFact::MathConst) => {
                let v = match self.sym(name) {
                    "PI" => "3.141592653589793",
                    "E" => "2.718281828459045",
                    "LN2" => "0.6931471805599453",
                    "LN10" => "2.302585092994046",
                    "LOG2E" => "1.4426950408889634",
                    "LOG10E" => "0.4342944819032518",
                    "SQRT2" => "1.4142135623730951",
                    "SQRT1_2" => "0.7071067811865476",
                    "MAX_SAFE_INTEGER" => return Val::plain("INT64_C(9007199254740991)", INT),
                    "MIN_SAFE_INTEGER" => return Val::plain("INT64_C(-9007199254740991)", INT),
                    "EPSILON" => "2.220446049250313e-16",
                    "MAX_VALUE" => "1.7976931348623157e308",
                    "MIN_VALUE" => "5e-324",
                    "NaN" => "((double)NAN)",
                    "POSITIVE_INFINITY" => "((double)INFINITY)",
                    _ => "((double)-INFINITY)",
                };
                return Val::plain(v, F64);
            }
            None => {}
        }
        let base = self.expr(obj);
        if optional {
            // a?.b: undefined when a is undefined.
            let res = self.fresh("t");
            let ct = self.ctype(ty);
            self.line(format!("{ct} {res};"));
            let present = self.truthy(base.clone());
            self.open(&format!("if ({present}) {{"));
            self.push_temps();
            let inner_ty = self.c.types.without_nullish(base.ty);
            let pb = self.project(base, inner_ty);
            // Read at the expression's type (which includes `undefined` unless narrowed):
            // an optional field keeps its own presence tag.
            let fv = self.field(pb, name, ty, span);
            let fv = self.coerce(fv, ty);
            let code = self.consume(fv);
            self.line(format!("{res} = {code};"));
            self.pop_temps();
            self.close("} else {");
            self.b().indent += 1;
            let undef = self.coerce(Val::plain("0", UNDEFINED), ty);
            let code = self.consume(undef);
            self.line(format!("{res} = {code};"));
            self.close("}");
            let owned = self.is_rc(ty);
            if owned {
                self.b().temps.last_mut().unwrap().push((res.clone(), ty));
            }
            return Val { code: res, ty, owned };
        }
        self.field(base, name, ty, span)
    }

    /// `base.name` for a non-optional base.
    fn field(&mut self, base: Val, name: crate::intern::Sym, ty: TyId, span: Span) -> Val {
        let base = self.unfold_val(base);
        let n = self.sym(name).to_string();
        match self.tget(base.ty) {
            Ty::Class(..) => self.class_field_read(base, name, ty, span),
            Ty::Interface(..) => match self.iface_field(base.clone(), name, ty) {
                Some(v) => v,
                None if let Some(v) = self.iface_rest_read(&base, &n, ty) => v,
                None => {
                    self.unsupported(span, "using an interface method as a value");
                    Val::plain("0", ty)
                }
            },
            Ty::Record(fs) => {
                let f = self.c.types.fields(fs).iter().find(|f| f.name == name).copied();
                let Some(f) = f else {
                    if let Some(v) = self.rest_read(&base, &n, ty) {
                        return v;
                    }
                    self.unsupported(span, "this field access");
                    return Val::plain("0", ty);
                };
                let v = Val { code: self.fld(base.ty, &base.code, &n), ty: f.ty, owned: false };
                // The checker may have narrowed this field path (`if (t.left !== undefined) t.left...`).
                let v = if self.c.assignable_pub(ty, f.ty) && ty != f.ty && !self.c.assignable_pub(f.ty, ty) { self.project(v, ty) } else { self.coerce(v, ty) };
                if base.owned && !v.owned && self.is_rc(v.ty) {
                    // Borrowing from a temporary that will be released: keep an owned copy.
                    return self.own(v);
                }
                v
            }
            Ty::Union(ms) => {
                // Every member has the field: select by tag.
                let ms = self.c.types.tys(ms).to_vec();
                let res = self.fresh("f");
                let ct = self.ctype(ty);
                self.line(format!("{ct} {res};"));
                let tag = self.u_tag(&base.code, base.ty);
                self.open(&format!("switch ({tag}) {{"));
                for (i, &mt) in ms.iter().enumerate() {
                    self.open(&format!("case {i}: {{"));
                    self.push_temps();
                    let payload = Val { code: self.u_payload(&base.code, base.ty, i), ty: mt, owned: false };
                    let fv = self.field(payload, name, ty, span);
                    let fv = self.coerce(fv, ty);
                    let code = self.consume(fv);
                    self.line(format!("{res} = {code};"));
                    self.pop_temps();
                    self.line("break;");
                    self.close("}");
                }
                let dv = self.default_value(ty);
                self.line(format!("default: {res} = {dv}; break;"));
                self.close("}");
                let owned = self.is_rc(ty);
                if owned {
                    self.b().temps.last_mut().unwrap().push((res.clone(), ty));
                }
                Val { code: res, ty, owned }
            }
            Ty::Array(_) if n == "length" => Val::plain(format!("tv_arr_len({})", base.code), INT),
            // (an array's own property: its value, as kept for it)
            Ty::Array(_) if self.c.array_props.contains_key(&name) => {
                let key = self.lit(n.as_bytes());
                let d = self.tmp(ANY, &format!("tv_arr_prop_get({}, {key})", base.code), true);
                let loc = self.loc(span);
                self.any_to(d, ty, &loc)
            }
            Ty::Str | Ty::StrLit(_) if n == "byteLength" => {
                let s = self.coerce(base, STR);
                Val::plain(format!("tv_str_byte_len({})", s.code), INT)
            }
            Ty::Str | Ty::StrLit(_) if n == "length" => {
                let s = self.coerce(base, STR);
                Val::plain(format!("tv_str_length({})", s.code), INT)
            }
            Ty::Map(..) | Ty::Set(_) if n == "size" => Val::plain(format!("tv_map_size({})", base.code), INT),
            _ => {
                self.unsupported(span, "this member access");
                Val::plain("0", ty)
            }
        }
    }

    /// `o.name op= value` where `o` (of type `nt`) is a union of records, or a record with an
    /// index signature that lacks the field: the value stored in each member's field (in `$rest`
    /// where a member has none).
    fn record_member_assign(&mut self, op: AssignOp, obj: ExprId, name: crate::intern::Sym, value: ExprId, nt: TyId, span: Span) -> Val {
        let raw = self.place(obj);
        let v = self.expr(value);
        let v = self.own(v);
        let key = self.sym(name).to_string();
        // (from its storage down, through unions and recursive aliases' boxes: each member's,
        // those the narrowing leaves out never holding it)
        let keep = self.c.flat_members(nt);
        self.member_store_in(&raw, &keep, name, &key, op, &v, span, None);
        Val { owned: false, ..v }
    }

    /// `.name op= v` of what `p` holds: each member's of a union whose flattened members are in
    /// `keep` (through recursive aliases' boxes: `Token`, a union of a union and a record).
    #[allow(clippy::too_many_arguments)]
    #[allow(clippy::too_many_arguments)]
    fn member_store_in(&mut self, p: &Place, keep: &[TyId], name: crate::intern::Sym, key: &str, op: AssignOp, v: &Val, span: Span, open: Option<&(String, TyId, usize, TyId)>) {
        if let Ty::Rec(..) = self.tget(p.ty) {
            let inner = self.c.unfold(p.ty);
            let ip = self.project_place(Place { lv: p.lv.clone(), ty: p.ty, weak: false }, inner);
            return self.member_store_in(&ip, keep, name, key, op, v, span, open);
        }
        if !matches!(self.tget(p.ty), Ty::Union(_)) {
            // (`token.type = "paragraph"` of a `Text`, where the union holding it has an open
            // member, `Generic`: the token made one, holding what it held, in the union's place,
            // as JavaScript's object becomes another kind)
            if let Some((ulv, uty, kg, g)) = open
                && matches!(op, AssignOp::Assign)
                && let Some(Ty::StrLit(want)) = self.slot_ty(p.ty, name).map(|t| self.tget(t))
                && v.ty != self.slot_ty(p.ty, name).unwrap()
                && self.c.types.is_stringish(v.ty)
            {
                let sv = self.coerce(Val { owned: false, ..v.clone() }, STR);
                let lit = self.lit(self.sym(want).to_string().as_bytes());
                let eq = self.eq_code(STR, &sv.code, &lit);
                self.open(&format!("if (!({eq})) {{"));
                self.push_temps();
                let ng = self.reopen_as(&Val::plain(p.lv.clone(), p.ty), *g, Some((name, sv)), None);
                let ng = self.own(ng);
                let ngc = self.consume(ng);
                let u = self.u_make(*uty, *kg, Some(&ngc));
                let ct = self.ctype(*uty);
                let rel = self.release_code(*uty, ulv);
                let rel = if rel.is_empty() { String::new() } else { format!("{rel}; ") };
                self.line(format!("{ct} nu_ = {u}; {rel}{ulv} = nu_;"));
                self.pop_temps();
                self.close("}");
                return;
            }
            return self.member_store(p, name, key, op, v, span);
        }
        let ms = self.members(p.ty);
        // (an open member here, an interface with an index signature naming the key as a
        // string: where a member that can't hold another value of it goes)
        let here = ms.iter().enumerate().find_map(|(i, &mt)| {
            let fs = self.open_fields(mt)?;
            fs.iter().any(|f| f.name == name && self.c.types.is_string(f.ty) && !matches!(self.tget(f.ty), Ty::StrLit(_))).then(|| (p.lv.clone(), p.ty, i, mt))
        });
        let open = here.as_ref().or(open);
        let tag = self.u_tag(&p.lv, p.ty);
        self.open(&format!("switch ({tag}) {{"));
        for (i, &mt) in ms.iter().enumerate() {
            if matches!(self.tget(mt), Ty::Undefined | Ty::Null) || !self.c.flat_members(mt).iter().any(|m| keep.contains(m)) {
                continue;
            }
            self.open(&format!("case {i}: {{"));
            self.push_temps();
            let mp = Place { lv: self.u_payload(&p.lv, p.ty, i), ty: mt, weak: false };
            self.member_store_in(&mp, keep, name, key, op, v, span, open);
            self.pop_temps();
            self.line("break;");
            self.close("}");
        }
        self.line("default: break;");
        self.close("}");
    }

    /// The fields of an open type, an interface or record with an index signature (`$rest`),
    /// through a recursive alias.
    fn open_fields(&mut self, t: TyId) -> Option<Vec<crate::types::Field>> {
        let rest = self.c.interner.lookup("$rest")?;
        let u = self.c.unfold(t);
        let fs = match self.tget(u) {
            Ty::Interface(..) => self.iface_members(u),
            Ty::Record(fs) => self.c.types.fields(fs).to_vec(),
            _ => return None,
        };
        fs.iter().any(|f| f.name == rest).then_some(fs)
    }

    /// The type of field `name` of a record or interface, if it has one.
    fn slot_ty(&mut self, t: TyId, name: crate::intern::Sym) -> Option<TyId> {
        match self.tget(t) {
            Ty::Record(fs) => self.c.types.fields(fs).iter().find(|f| f.name == name).map(|f| f.ty),
            Ty::Interface(..) => self.iface_members(t).iter().find(|f| f.name == name).map(|f| f.ty),
            _ => None,
        }
    }

    /// Field `name` of `src` (a record, interface or class value), if it has one.
    pub(crate) fn src_field(&mut self, src: &Val, name: crate::intern::Sym) -> Option<Val> {
        match self.tget(src.ty) {
            Ty::Record(fs) => {
                let f = self.c.types.fields(fs).iter().find(|f| f.name == name).copied()?;
                Some(Val { code: self.fld(src.ty, &src.code, self.sym(name)), ty: f.ty, owned: false })
            }
            Ty::Interface(..) => {
                let t = self.iface_members(src.ty).iter().find(|f| f.name == name).map(|f| f.ty)?;
                if matches!(self.tget(t), Ty::Func(..)) {
                    return None;
                }
                self.iface_field(src.clone(), name, t)
            }
            Ty::Class(..) => {
                let cf = self.class_field(src.ty, name)?;
                Some(Val { code: self.field_lv_of(&src.code, src.ty, &cf), ty: cf.ty, owned: false })
            }
            _ => None,
        }
    }

    /// A new value of the open interface `g` (`Generic`) holding what `src` holds (its fields
    /// `g` doesn't name in `g`'s `$rest`), field `set.0` given `set.1` instead; `rest` a map of
    /// more keys (a `$rest` already made).
    pub(crate) fn reopen_as(&mut self, src: &Val, g: TyId, set: Option<(crate::intern::Sym, Val)>, rest: Option<Val>) -> Val {
        let gu = self.c.unfold(g);
        let (gms, rt) = match self.tget(gu) {
            Ty::Record(fs) => (self.c.types.fields(fs).to_vec(), gu),
            _ => {
                let gms = self.iface_members(gu);
                let order: Vec<crate::intern::Sym> = gms.iter().map(|f| f.name).collect();
                let rt = self.c.types.record(gms.clone());
                self.c.types.note_field_order(rt, order);
                (gms, rt)
            }
        };
        let order: Vec<crate::intern::Sym> = gms.iter().map(|f| f.name).collect();
        let rest_sym = self.c.interner.lookup("$rest");
        let mut parts = Vec::new();
        for gf in &gms {
            let code = if let Some((n, nv)) = &set
                && *n == gf.name
            {
                let cv = self.coerce(Val { owned: false, ..nv.clone() }, gf.ty);
                let cv = self.own(cv);
                self.consume(cv)
            } else if Some(gf.name) == rest_sym {
                let mt = self.c.types.without_undefined(gf.ty);
                let Ty::Map(k, vt) = self.tget(mt) else { continue };
                let (kd, vd, vct) = (self.desc(k), self.desc(vt), self.ctype(vt));
                let mv = self.fresh("rm");
                match &rest {
                    Some(r) => {
                        let rc = self.consume(r.clone());
                        self.line(format!("tv_map {mv} = {rc};"));
                    }
                    None => self.line(format!("tv_map {mv} = tv_map_fresh_object();")),
                }
                let names: Vec<crate::intern::Sym> = match self.tget(src.ty) {
                    Ty::Record(fs) => self.c.types.fields(fs).iter().map(|f| f.name).collect(),
                    Ty::Interface(..) => self.iface_members(src.ty).iter().map(|f| f.name).collect(),
                    Ty::Class(d, _) => self.c.classes[d as usize].fields.iter().map(|f| f.name).collect(),
                    _ => Vec::new(),
                };
                for n in names {
                    if order.contains(&n) || Some(n) == rest_sym {
                        continue;
                    }
                    let Some(fv) = self.src_field(src, n) else { continue };
                    if matches!(self.tget(fv.ty), Ty::Func(..)) {
                        continue;
                    }
                    let cv = self.coerce(fv, vt);
                    let cv = self.own(cv);
                    let code = self.consume(cv);
                    let lit = self.lit(self.sym(n).to_string().as_bytes());
                    self.line(format!("{{ tv_str k_ = {lit}; {vct} x_ = {code}; tv_map_set(&{mv}, {kd}, {vd}, &k_, &x_); }}"));
                }
                let cv = self.coerce(Val { code: mv, ty: mt, owned: true }, gf.ty);
                self.consume(cv)
            } else if let Some(fv) = self.src_field(src, gf.name) {
                let cv = self.coerce(fv, gf.ty);
                let cv = self.own(cv);
                self.consume(cv)
            } else {
                let cv = self.coerce(Val::plain("0", UNDEFINED), gf.ty);
                self.consume(cv)
            };
            parts.push(format!(".f_{} = {code}", self.sym(gf.name)));
        }
        let lit = self.rec_lit(rt, &parts.join(", "));
        let rv = self.tmp(rt, &lit, true);
        self.coerce(rv, g)
    }

    /// The hidden `$rest` map of a record or interface with an index signature: its place.
    fn rest_slot(&mut self, rec: &Place) -> Option<Place> {
        match self.tget(rec.ty) {
            Ty::Record(_) => {
                let rt = self.rest_field(rec.ty)?;
                Some(Place { lv: self.fld(rec.ty, &rec.lv, "$rest"), ty: rt, weak: false })
            }
            Ty::Interface(..) => {
                let rest = self.c.interner.lookup("$rest")?;
                let (lv, ty) = self.iface_field_lv(&Val::plain(rec.lv.clone(), rec.ty), rest)?;
                Some(Place { lv, ty, weak: false })
            }
            _ => None,
        }
    }

    /// One record's `.name op= v` (in its `$rest` when it has no such field).
    fn member_store(&mut self, rec: &Place, name: crate::intern::Sym, key: &str, op: AssignOp, v: &Val, span: Span) {
        // (through a recursive alias: the record in its box)
        if let Ty::Rec(..) = self.tget(rec.ty) {
            let inner = self.c.unfold(rec.ty);
            let p = self.project_place(Place { lv: rec.lv.clone(), ty: rec.ty, weak: false }, inner);
            return self.member_store(&p, name, key, op, v, span);
        }
        let borrowed = Val { owned: false, ..v.clone() };
        let direct = match self.tget(rec.ty) {
            Ty::Record(fs) => self.c.types.fields(fs).iter().find(|f| f.name == name).map(|f| (self.fld(rec.ty, &rec.lv, key), f.ty)),
            // (an interface's: through its table)
            Ty::Interface(..) => self.iface_field_lv(&Val::plain(rec.lv.clone(), rec.ty), name),
            _ => None,
        };
        // (an interface with an index signature: set on the value it holds, by name; one that
        // has no room for it, a new one holding what it held, as JavaScript's object takes any
        // key: `token.type = "paragraph"` of a `Generic` that's a `Text`)
        if matches!(self.tget(rec.ty), Ty::Interface(..)) && self.iface_keyed(rec.ty) && direct.as_ref().is_none_or(|d| self.c.types.is_string(d.1) || d.1 == ANY) {
            let it = self.itab_type(rec.ty);
            let lit = self.lit(key.as_bytes());
            let nv = match op {
                AssignOp::Assign => borrowed,
                AssignOp::Op(b) => {
                    let cur = self.tmp(ANY, &format!("((const {it} *)({0}).t)->g(({0}).p, {lit})", rec.lv), true);
                    self.compound_val(b, cur, borrowed, ANY, span)
                }
            };
            let d = self.coerce(nv, ANY);
            let d = self.own(d);
            let dc = self.consume(d);
            let ct = self.ctype(rec.ty);
            let rel = self.release_code(rec.ty, &rec.lv);
            self.line(format!("{{ tv_dyn d_ = {dc}; tv_dyn_retain(d_); if (((const {it} *)({0}).t)->s(({0}).p, {lit}, d_)) tv_dyn_release(d_); else {{ {ct} o_ = ((const {it} *)({0}).t)->o(({0}).p, {lit}, d_); {rel}; {0} = o_; }} }}", rec.lv));
            return;
        }
        let ft = direct.as_ref().map(|d| d.1);
        let (cur, slot_ty) = match direct {
            Some((lv, ft)) => (Val::plain(lv, ft), ft),
            None => {
                let Some(rp) = self.rest_slot(rec) else {
                    self.unsupported(span, "assigning to this member");
                    return;
                };
                let rt = rp.ty;
                let mt = self.c.types.without_undefined(rt);
                let Ty::Map(_, vt) = self.tget(mt) else { return };
                let read_ty = if vt == ANY || vt == UNKNOWN { vt } else { self.c.types.optional(vt) };
                let cur = if matches!(op, AssignOp::Assign) {
                    Val::plain("0", UNDEFINED)
                } else {
                    self.rest_read_at(rp.lv, rt, key, read_ty).unwrap_or_else(|| Val::plain("0", UNDEFINED))
                };
                (cur, vt)
            }
        };
        // (`token.type = "paragraph"` of a `Text` token, whose `type` is `"text"`: a change of
        // what record it is, which its storage can't hold; the same value stays)
        if let Ty::StrLit(want) = self.tget(slot_ty)
            && matches!(op, AssignOp::Assign)
            && borrowed.ty != slot_ty
            && self.c.types.is_stringish(borrowed.ty)
        {
            let sv = self.coerce(borrowed, STR);
            let lit = self.lit(self.sym(want).to_string().as_bytes());
            let eq = self.eq_code(STR, &sv.code, &lit);
            let loc = self.loc(span);
            self.line(format!("if (!({eq})) tv_trap(\"changing a record's discriminant (`{key}`) is not supported\", {loc});"));
            return;
        }
        let nv = match op {
            AssignOp::Assign => self.coerce(borrowed, slot_ty),
            AssignOp::Op(b) => self.compound_val(b, cur.clone(), borrowed, slot_ty, span),
        };
        let nv = self.own(nv);
        let code = self.consume(nv);
        if ft.is_some() {
            let ct = self.ctype(slot_ty);
            let t = self.fresh("n");
            let rel = self.release_code(slot_ty, &cur.code);
            let rel = if rel.is_empty() { String::new() } else { format!("{rel}; ") };
            self.line(format!("{ct} {t} = {code}; {rel}{} = {t};", cur.code));
            return;
        }
        // (into `$rest`, made the first time)
        let Some(rp) = self.rest_slot(rec) else { return };
        let rt = rp.ty;
        let mt = self.c.types.without_undefined(rt);
        let Ty::Map(k, vt) = self.tget(mt) else { return };
        let present = self.truthy_val(Val::plain(rp.lv.clone(), rt));
        self.open(&format!("if (!{present}) {{"));
        self.push_temps();
        let fresh = self.fresh("rm");
        self.line(format!("tv_map {fresh} = tv_map_fresh_object();"));
        let fm = self.coerce(Val { code: fresh, ty: mt, owned: true }, rt);
        let fm = self.consume(fm);
        self.line(format!("{} = {fm};", rp.lv));
        self.pop_temps();
        self.close("}");
        let ml = self.project_place(rp, mt);
        let (kd, vd, vct) = (self.desc(k), self.desc(vt), self.ctype(vt));
        let lit = self.lit(key.as_bytes());
        let (kv, xv) = (self.fresh("rk"), self.fresh("rx"));
        self.line(format!("tv_str {kv} = {lit}; {vct} {xv} = {code}; tv_map_set(&{}, {kd}, {vd}, &{kv}, &{xv});", ml.lv));
    }

    /// `a op b` of two values, for `x op= y` (`+` of strings: concatenated).
    fn compound_val(&mut self, b: BinOp, cur: Val, rhs: Val, ty: TyId, span: Span) -> Val {
        if cur.ty == ANY || rhs.ty == ANY || ty == ANY {
            let rt = self.c.any_binary(b, cur.ty, rhs.ty);
            let r = self.any_binop(b, cur, rhs, rt);
            return self.coerce(r, ty);
        }
        if b == BinOp::Add && (self.c.types.is_stringish(cur.ty) || self.c.types.is_stringish(rhs.ty)) {
            let sbv = self.fresh("sb");
            self.line(format!("tv_sb {sbv} = {{0}};"));
            self.open("{");
            self.line(format!("tv_sb *sb = &{sbv};"));
            let (c1, c2) = (self.string_code(cur.ty, &cur.code), self.string_code(rhs.ty, &rhs.code));
            self.line(format!("{c1}; {c2};"));
            self.close("}");
            let r = self.tmp(STR, &format!("tv_str_from_sb(&{sbv})"), true);
            return self.coerce(r, ty);
        }
        let cur = self.coerce(cur, ty);
        let rhs = if self.c.types.is_float(ty) { self.coerce(rhs, ty) } else { rhs };
        self.arith(b, cur, rhs, ty, span)
    }

    /// The hidden `$rest` map of a record with an index signature (its type), if it has one.
    pub(crate) fn rest_field(&self, rec: TyId) -> Option<TyId> {
        let rest = self.c.interner.lookup("$rest")?;
        let Ty::Record(fs) = self.tget(rec) else { return None };
        self.c.types.fields(fs).iter().find(|f| f.name == rest).map(|f| f.ty)
    }

    /// `r.k` of a record with an index signature that has no field `k`: its value in `$rest`,
    /// `undefined` if it's not there.
    fn rest_read(&mut self, base: &Val, key: &str, ty: TyId) -> Option<Val> {
        let rt = self.rest_field(base.ty)?;
        let lv = self.fld(base.ty, &base.code, "$rest");
        self.rest_read_at(lv, rt, key, ty)
    }

    /// The same of an interface value, through its `$rest` member.
    fn iface_rest_read(&mut self, base: &Val, key: &str, ty: TyId) -> Option<Val> {
        // (the key of the value it holds: its field of that name, else its own `$rest`'s)
        if self.iface_keyed(base.ty) {
            let it = self.itab_type(base.ty);
            let lit = self.lit(key.as_bytes());
            let d = self.tmp(ANY, &format!("((const {it} *)({0}).t)->g(({0}).p, {lit})", base.code), true);
            let loc = self.loc(self.stmt_span.unwrap_or(crate::source::Span::new(crate::source::FileId(0), 0, 0)));
            return Some(self.any_to(d, ty, &loc));
        }
        let rest = self.c.interner.lookup("$rest")?;
        let (lv, rt) = self.iface_field_lv(base, rest)?;
        self.rest_read_at(lv, rt, key, ty)
    }

    /// `rest_lv` (a `$rest`, of type `rt`)'s value for `key`, as `ty`.
    fn rest_read_at(&mut self, rest_lv: String, rt: TyId, key: &str, ty: TyId) -> Option<Val> {
        let lit = self.lit(key.as_bytes());
        self.rest_read_code(rest_lv, rt, &lit, ty)
    }

    /// `rest_read_at` of a key held in C (`lit`, a `tv_str`).
    pub(crate) fn rest_read_code(&mut self, rest_lv: String, rt: TyId, lit: &str, ty: TyId) -> Option<Val> {
        let mt = self.c.types.without_undefined(rt);
        let Ty::Map(k, v) = self.tget(mt) else { return None };
        let rv = Val { code: rest_lv, ty: rt, owned: false };
        let present = self.truthy_val(rv.clone());
        let (kd, vd, vct, ct) = (self.desc(k), self.desc(v), self.ctype(v), self.ctype(ty));
        let res = self.fresh("rr");
        self.line(format!("{ct} {res};"));
        let undef = |g: &mut Self| {
            let u = g.coerce(Val::plain("0", UNDEFINED), ty);
            g.consume(u)
        };
        self.open(&format!("if ({present}) {{"));
        self.push_temps();
        let mv = self.project(rv, mt);
        let (kv, vp) = (self.fresh("rk"), self.fresh("rp"));
        self.line(format!("tv_str {kv} = {lit}; {vct} *{vp} = tv_map_get({}, {kd}, {vd}, &{kv});", mv.code));
        self.open(&format!("if ({vp}) {{"));
        self.push_temps();
        let x = self.coerce(Val { code: format!("(*{vp})"), ty: v, owned: false }, ty);
        let x = self.own(x);
        let xc = self.consume(x);
        self.line(format!("{res} = {xc};"));
        self.pop_temps();
        self.close("} else {");
        self.bump(1);
        let u = undef(self);
        self.line(format!("{res} = {u};"));
        self.bump(-1);
        self.line("}");
        self.pop_temps();
        self.close("} else {");
        self.bump(1);
        let u = undef(self);
        self.line(format!("{res} = {u};"));
        self.bump(-1);
        self.line("}");
        let owned = self.is_rc(ty);
        if owned {
            self.b().temps.last_mut().unwrap().push((res.clone(), ty));
        }
        Some(Val { code: res, ty, owned })
    }

    fn index(&mut self, e: ExprId, obj: ExprId, index: ExprId, optional: bool, ty: TyId, span: Span) -> Val {
        let ot = self.ty(obj);
        if ot == ANY {
            let base = self.expr(obj);
            let iv = self.expr(index);
            let loc = self.loc(span);
            return self.any_index(base, iv, optional, ty, &loc);
        }
        // (`({ "&": "&amp;", "<": "&lt;" })[c]`: a dictionary of constants, only read: made once)
        if let Some(lit) = self.const_dict(obj) {
            // (a few string keys: compared one by one, no map at all)
            let lt = self.ty(lit);
            let fields_n = match &self.ast(self.cur_m()).expr(lit).kind {
                ExprKind::Object(fs) => fs.len(),
                _ => usize::MAX,
            };
            if let Ty::Map(k, v) = self.tget(lt)
                && k == STR
                && fields_n <= 8
            {
                let m = self.cur_m();
                let ExprKind::Object(fields) = &self.ast(m).expr(lit).kind else { unreachable!() };
                let ov = self.c.types.optional(v);
                let kv = self.expr(index);
                let kv = self.coerce(kv, STR);
                let kn = self.fresh("k");
                self.line(format!("tv_str {kn} = {};", kv.code));
                let res = self.fresh("t");
                let oct = self.ctype(ov);
                self.line(format!("{oct} {res};"));
                for (i, f) in fields.iter().enumerate() {
                    let key = self.lit(self.sym(f.name).to_string().as_bytes());
                    self.open(&format!("{}if (tv_str_eq({kn}, {key})) {{", if i == 0 { "" } else { "} else " }));
                    self.b().indent -= 1;
                    self.b().indent += 1;
                    self.push_temps();
                    let fv = self.expr(f.value);
                    let fv = self.coerce(fv, ov);
                    let code = self.consume(fv);
                    self.line(format!("{res} = {code};"));
                    self.pop_temps();
                    self.b().indent -= 1;
                }
                self.line("} else {");
                self.b().indent += 1;
                let u = self.coerce(Val::plain("0", UNDEFINED), ov);
                let code = self.consume(u);
                self.line(format!("{res} = {code};"));
                self.close("}");
                let owned = self.is_rc(ov);
                if owned {
                    self.b().temps.last_mut().unwrap().push((res.clone(), ov));
                }
                let got = Val { code: res, ty: ov, owned };
                return if self.c.types.has_undefined(ty) { self.coerce(got, ty) } else { self.downcast(got, ty, span) };
            }
            let cm = self.fresh("cmap");
            self.line(format!("static tv_map {cm};"));
            self.open(&format!("if (!{cm}.p) {{"));
            self.push_temps();
            let v = self.expr(lit);
            let c = self.consume(v);
            self.line(format!("{cm} = {c};"));
            self.pop_temps();
            self.close("}");
            let lt = self.ty(lit);
            let a = Val { code: cm, ty: lt, owned: false };
            return self.index_of(a, index, ty, span);
        }
        if self.c.types.without_undefined(ot) == JS {
            let base = self.expr(obj);
            let base = self.js_of(base);
            let iv = self.expr(index);
            return self.js_index(e, base, iv, optional, ty, span);
        }
        let a = self.expr(obj);
        if optional {
            let inner = self.c.types.without_nullish(a.ty);
            if inner != a.ty {
                // a?.[i]: undefined when a is (`i` unevaluated)
                let res = self.fresh("t");
                let ct = self.ctype(ty);
                self.line(format!("{ct} {res};"));
                let present = self.truthy(a.clone());
                self.open(&format!("if ({present}) {{"));
                self.push_temps();
                let pa = self.project(a, inner);
                let v = self.index_of(pa, index, ty, span);
                let v = self.coerce(v, ty);
                let code = self.consume(v);
                self.line(format!("{res} = {code};"));
                self.pop_temps();
                self.close("} else {");
                self.b().indent += 1;
                let u = self.coerce(Val::plain("0", UNDEFINED), ty);
                let code = self.consume(u);
                self.line(format!("{res} = {code};"));
                self.close("}");
                let owned = self.is_rc(ty);
                if owned {
                    self.b().temps.last_mut().unwrap().push((res.clone(), ty));
                }
                return Val { code: res, ty, owned };
            }
        }
        self.index_of(a, index, ty, span)
    }

    /// `for (const k in m)` (`Object.keys(m)`) of a map variable with string keys whose body can't
    /// change which keys it holds or reassign it: (the map, its key and value types).
    fn keys_loop(&mut self, iter: ExprId, key: crate::intern::Sym, body: crate::ast::StmtId) -> Option<(ExprId, TyId, TyId)> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let ExprKind::Call { callee, args, optional: false, .. } = &ast.expr(iter).kind else { return None };
        let ExprKind::Member { obj, name: keys, .. } = ast.expr(*callee).kind else { return None };
        let ExprKind::Ident(o) = ast.expr(obj).kind else { return None };
        if self.sym(keys) != "keys" || self.sym(o) != "Object" || args.len() != 1 {
            return None;
        }
        let mx = args[0].expr;
        let ExprKind::Ident(ms) = ast.expr(mx).kind else { return None };
        let mt = self.ty(mx);
        let Ty::Map(k, v) = self.tget(mt) else { return None };
        if k != STR {
            return None;
        }
        let mut stmts = vec![body];
        let mut exprs = Vec::new();
        while let Some(s) = stmts.pop() {
            match &ast.stmt(s).kind {
                StmtKind::Let { init, .. } => exprs.extend(*init),
                StmtKind::Expr(e) => exprs.push(*e),
                StmtKind::If(c, t, e) => {
                    exprs.push(*c);
                    stmts.push(*t);
                    stmts.extend(*e);
                }
                StmtKind::While(c, b) | StmtKind::DoWhile(b, c) => {
                    exprs.push(*c);
                    stmts.push(*b);
                }
                StmtKind::For { init, cond, step, body } => {
                    stmts.extend(*init);
                    exprs.extend([cond, step].into_iter().flatten().copied());
                    stmts.push(*body);
                }
                StmtKind::ForOf { iter, body, .. } => {
                    exprs.push(*iter);
                    stmts.push(*body);
                }
                StmtKind::Block(ss) => stmts.extend(ss.iter().copied()),
                StmtKind::Break(_) | StmtKind::Continue(_) | StmtKind::Empty => {}
                _ => return None,
            }
        }
        let is_map = |e: ExprId| matches!(ast.expr(e).kind, ExprKind::Ident(n) if n == ms);
        let is_key = |e: ExprId| matches!(ast.expr(e).kind, ExprKind::Ident(n) if n == key);
        while let Some(e) = exprs.pop() {
            let class_ty = |g: &mut Self, x: ExprId| {
                let t = g.ty(x);
                matches!(g.tget(t), Ty::Class(..))
            };
            match &ast.expr(e).kind {
                ExprKind::Int(_) | ExprKind::Float(_) | ExprKind::Str(_) | ExprKind::Bool(_) | ExprKind::Undefined | ExprKind::Null | ExprKind::Ident(_) | ExprKind::BigInt(_) => {}
                // (nothing that may run the program's code: calls, getters, `valueOf`s)
                ExprKind::Call { callee, args, optional: false, .. }
                    if matches!(ast.expr(*callee).kind, ExprKind::Member { obj, .. } if matches!(ast.expr(obj).kind, ExprKind::Ident(n) if self.sym(n) == "Math")) =>
                {
                    exprs.extend(args.iter().map(|a| a.expr));
                }
                ExprKind::Member { obj, .. } => {
                    if class_ty(self, *obj) {
                        return None;
                    }
                    exprs.push(*obj);
                }
                ExprKind::Index { obj, index, .. } => exprs.extend([*obj, *index]),
                ExprKind::Unary(_, x) | ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Typeof(x) => exprs.push(*x),
                ExprKind::Binary(_, a, b) => {
                    if class_ty(self, *a) || class_ty(self, *b) {
                        return None;
                    }
                    exprs.extend([*a, *b]);
                }
                ExprKind::Cond(a, b, c) => exprs.extend([*a, *b, *c]),
                ExprKind::Template(_, xs) => {
                    for &x in xs {
                        if class_ty(self, x) {
                            return None;
                        }
                    }
                    exprs.extend(xs.iter().copied());
                }
                // (an array literal, `m[k] ?? []`: no spread, which could run an iterator)
                ExprKind::Array(xs) => {
                    if xs.iter().any(|&x| matches!(ast.expr(x).kind, ExprKind::Spread(_))) {
                        return None;
                    }
                    exprs.extend(xs.iter().copied());
                }
                // (the map itself only at the loop's key: a value changed in place)
                ExprKind::Assign(_, t, _) | ExprKind::Update { target: t, .. } => {
                    match &ast.expr(*t).kind {
                        ExprKind::Ident(n) if *n == ms || *n == key => return None,
                        ExprKind::Index { obj, index, .. } if is_map(*obj) && !is_key(*index) => return None,
                        _ => {}
                    }
                    exprs.push(*t);
                    if let ExprKind::Assign(_, _, v) = ast.expr(e).kind {
                        exprs.push(v);
                    }
                }
                _ => return None,
            }
        }
        Some((mx, k, v))
    }

    /// In a loop over the keys of the map whose code is `map` (`keys_loop`), `index` that loop's
    /// key: the current entry's value pointer.
    fn map_loop_value(&mut self, map: &str, index: ExprId) -> Option<String> {
        let ExprKind::Ident(_) = self.ast(self.cur_m()).expr(index).kind else { return None };
        let key = self.expr(index).code;
        self.b().map_loops.iter().rev().find(|(mc, kv, ..)| mc == map && *kv == key).map(|l| l.2.clone())
    }

    /// The code of a variable (an identifier): what it's read as.
    fn expr_code_of(&mut self, e: ExprId) -> String {
        self.expr(e).code
    }

    /// An object literal (in parentheses or not) checked as a map whose fields are all constants.
    fn const_dict(&mut self, obj: ExprId) -> Option<ExprId> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let mut x = obj;
        while let ExprKind::Paren(y) = ast.expr(x).kind {
            x = y;
        }
        let ExprKind::Object(fields) = &ast.expr(x).kind else { return None };
        let constant = !fields.is_empty()
            && fields.iter().all(|f| matches!(ast.expr(f.value).kind, ExprKind::Str(_) | ExprKind::Int(_) | ExprKind::Float(_) | ExprKind::Bool(_) | ExprKind::Null | ExprKind::Undefined));
        let t = self.ty(x);
        (constant && matches!(self.tget(t), Ty::Map(..))).then_some(x)
    }

    /// `a[index]` read as `ty`, for a present `a`.
    fn index_of(&mut self, a: Val, index: ExprId, ty: TyId, span: Span) -> Val {
        // a tuple's element: its field
        if let Some(fields) = self.tuple_of(a.ty)
            && let ExprKind::Int(i) = self.ast(self.cur_m()).expr(index).kind
            && let Some(f) = fields.get(i as usize)
        {
            let v = Val { code: self.fld(a.ty, &a.code, self.sym(f.name)), ty: f.ty, owned: false };
            return self.project(v, ty);
        }
        if let Ty::Map(k, v) = self.tget(a.ty) {
            // (`m[k]` in a loop over `m`'s keys: the current entry's value)
            if let Some(p) = self.map_loop_value(&a.code, index) {
                let got = Val::plain(format!("(*({} *){p})", self.ctype(v)), v);
                return self.coerce(got, ty);
            }
            let arg = [crate::ast::Arg { expr: index, by_ref: None }];
            if self.c.types.has_undefined(ty) {
                return self.map_method_pub(a, k, v, "get", &arg, ty);
            }
            // read as present (TypeScript's type): traps when the key is missing
            let ov = self.c.types.optional(v);
            let got = self.map_method_pub(a, k, v, "get", &arg, ov);
            return self.downcast(got, ty, span);
        }
        let (et, len, elem) = match self.tget(a.ty) {
            Ty::Array(et) => (et, format!("tv_arr_len({})", a.code), None),
            Ty::Str | Ty::StrLit(_) => {
                // one UTF-16 unit, as a string
                let sv = self.fresh("s");
                let a = self.coerce(a.clone(), STR);
                self.line(format!("tv_str {sv} = {};", a.code));
                (STR, format!("tv_str_length({sv})"), Some(sv))
            }
            _ => {
                self.unsupported(span, "indexing this type");
                return Val::plain("0", ty);
            }
        };
        let i = self.expr(index);
        let i = self.index_code(i, span);
        let iv = self.fresh("ix");
        self.line(format!("tv_int {iv} = {i};"));
        if let Some(sv) = elem {
            let v = Val { code: format!("tv_str_unit_at({sv}, {iv})"), ty: STR, owned: true };
            if !self.c.types.has_undefined(ty) {
                let loc = self.loc(span);
                self.line(format!("if ((uint64_t){iv} >= (uint64_t){len}) tv_arr_oob({iv}, {len}, {loc});"));
                return self.tmp(STR, &v.code, true);
            }
            return self.present_if(&format!("(uint64_t){iv} < (uint64_t){len}"), v, ty);
        }
        let res = self.fresh("t");
        let ct = self.ctype(ty);
        let ect = self.ctype(et);
        self.line(format!("{ct} {res};"));
        self.open(&format!("if ({iv} >= 0 && {iv} < tv_arr_len({})) {{", a.code));
        self.push_temps();
        let ev = Val::plain(format!("(({ect} *)tv_arr_data({}))[{iv}]", a.code), et);
        let cv = self.coerce(ev, ty);
        // (the element as it's held, `C` read as `C | undefined`: borrowed from the array, as a
        // read of an element in bounds is)
        let borrowed = !cv.owned && ct == ect && self.niche(ty).is_some();
        let code = if borrowed { cv.code } else { self.consume(cv) };
        self.line(format!("{res} = {code};"));
        self.pop_temps();
        self.close("} else {");
        self.b().indent += 1;
        // (past the end: `undefined`, or the `null` an element type that has no `undefined` has)
        let none = if !self.c.types.has_undefined(ty) && self.members(ty).contains(&NULL) { NULL } else { UNDEFINED };
        let u = self.coerce(Val::plain("0", none), ty);
        let code = self.consume(u);
        self.line(format!("{res} = {code};"));
        self.close("}");
        let owned = self.is_rc(ty) && !borrowed;
        if owned {
            self.b().temps.last_mut().unwrap().push((res.clone(), ty));
        }
        Val { code: res, ty, owned }
    }

    /// `String(v)`, as a string temporary.
    fn js_string(&mut self, v: Val) -> Val {
        if self.c.types.is_string(v.ty) {
            return self.coerce(v, STR);
        }
        let v = self.own(v);
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

    /// `xs[i] < x` (either side) on an array of numbers, with `i` maybe past either end: false
    /// there, as JavaScript's `undefined < x` is (`while (xs[--j] > pivot);` stops at the start).
    /// A class instance with a `valueOf()` method: what it returns (else `v` as it is).
    fn primitive_of(&mut self, v: Val) -> Val {
        if !matches!(self.tget(v.ty), Ty::Class(..)) {
            return v;
        }
        let Some(m) = self.c.interner.lookup("valueOf") else { return v };
        let Some(crate::check::class::ClassMemberRef::Method(mm)) = self.c.class_member(v.ty, m) else { return v };
        if !mm.params.iter().all(|p| p.optional) {
            return v;
        }
        let Some(ret) = mm.ret.filter(|&r| self.c.types.is_numeric(r) || self.c.types.is_string(r)) else { return v };
        let call = self.method_call_code(&v.code, v.ty, m, false, Vec::new());
        let rc = self.is_rc(ret);
        self.tmp(ret, &call, rc)
    }

    fn compare_past_end(&mut self, op: BinOp, l: ExprId, r: ExprId) -> Option<Val> {
        let m = self.cur_m();
        let strip = |c: &Self, mut e: ExprId| {
            while let ExprKind::Paren(x) = c.ast(m).expr(e).kind {
                e = x;
            }
            e
        };
        let (le, re) = (strip(self, l), strip(self, r));
        // (`s[i] === s[i - 1]` past either end: `undefined`, equal to nothing but another such read)
        if matches!(op, BinOp::Eq | BinOp::Ne) {
            // (a string's character, or an element of an array of strings, `toks[i] === "*"`)
            let str_read = |c: &Self, e: ExprId| -> Option<(ExprId, ExprId, bool)> {
                let ExprKind::Index { obj, index, optional: false } = c.ast(m).expr(e).kind else { return None };
                let ot = c.facts(m).expr_ty.get(obj as usize).copied().unwrap_or(ERROR);
                if !c.harmless(obj) {
                    return None;
                }
                match *c.c.types.get(ot) {
                    Ty::Str | Ty::StrLit(_) => Some((obj, index, false)),
                    Ty::Array(et) if et == STR => Some((obj, index, true)),
                    _ => None,
                }
            };
            let is_str = |c: &Self, e: ExprId| c.c.types.is_string(c.facts(m).expr_ty.get(e as usize).copied().unwrap_or(ERROR));
            let (rl, rr) = (str_read(self, le), str_read(self, re));
            // (a string's unit against another's, or against a one-unit literal: the UTF-16 code
            // units compared, no strings made)
            let unit_lit = |c: &Self, e: ExprId| -> Option<u32> {
                let t = c.facts(m).expr_ty.get(e as usize).copied().unwrap_or(ERROR);
                let Ty::StrLit(sym) = *c.c.types.get(t) else { return None };
                let mut it = c.c.interner.get(sym).chars();
                let ch = it.next()?;
                (it.next().is_none() && (ch as u32) < 0x10000).then_some(ch as u32)
            };
            let unit_side = |c: &Self, read: Option<(ExprId, ExprId, bool)>, e: ExprId| matches!(read, Some((_, _, false))) || unit_lit(c, e).is_some();
            if (matches!(rl, Some((_, _, false))) || matches!(rr, Some((_, _, false)))) && unit_side(self, rl, le) && unit_side(self, rr, re) {
                let side = |c: &mut Self, read: Option<(ExprId, ExprId, bool)>, e: ExprId| -> (String, String) {
                    match read {
                        Some((obj, index, _)) => {
                            let sv = c.expr(obj);
                            let sv = c.coerce(sv, STR);
                            let sv = if c.changes_in(index) { c.snapshot(sv) } else { sv };
                            let i = c.expr(index);
                            let i = c.int_code(i);
                            let (iv, ok, uv) = (c.fresh("ix"), c.fresh("in"), c.fresh("u"));
                            c.line(format!("tv_int {iv} = {i}; tv_int {uv} = tv_str_char_code_at({}, {iv}); bool {ok} = {uv} >= 0;", sv.code));
                            (ok, uv)
                        }
                        None => ("true".to_string(), format!("{}", unit_lit(c, e).unwrap())),
                    }
                };
                let (okl, lu) = side(self, rl, le);
                let (okr, ru) = side(self, rr, re);
                let eq = format!("((({okl}) && ({okr}) && {lu} == {ru}) || (!({okl}) && !({okr})))");
                return Some(Val::plain(if op == BinOp::Eq { eq } else { format!("(!{eq})") }, BOOL));
            }
            if (rl.is_some() || rr.is_some()) && is_str(self, le) && is_str(self, re) {
                let side = |c: &mut Self, read: Option<(ExprId, ExprId, bool)>, e: ExprId| -> (String, Val) {
                    match read {
                        Some((obj, index, true)) => {
                            let av = c.expr(obj);
                            let i = c.expr(index);
                            let i = c.int_code(i);
                            let (iv, ok) = (c.fresh("ix"), c.fresh("in"));
                            c.line(format!("tv_int {iv} = {i}; bool {ok} = (uint64_t){iv} < (uint64_t)tv_arr_len({});", av.code));
                            let ev = c.fresh("e");
                            c.line(format!("tv_str {ev} = {ok} ? ((tv_str *)tv_arr_data({}))[{iv}] : TV_EMPTY_STR;", av.code));
                            (ok, Val::plain(ev, STR))
                        }
                        Some((obj, index, false)) => {
                            let sv = c.expr(obj);
                            let sv = c.coerce(sv, STR);
                            let i = c.expr(index);
                            let i = c.int_code(i);
                            let (iv, ok) = (c.fresh("ix"), c.fresh("in"));
                            c.line(format!("tv_int {iv} = {i}; bool {ok} = (uint64_t){iv} < (uint64_t)tv_str_length({});", sv.code));
                            let v = c.tmp(STR, &format!("({ok} ? tv_str_unit_at({}, {iv}) : TV_EMPTY_STR)", sv.code), true);
                            (ok, v)
                        }
                        None => {
                            let v = c.expr(e);
                            let v = c.coerce(v, STR);
                            let v = c.own(v);
                            ("true".to_string(), v)
                        }
                    }
                };
                let (okl, lv) = side(self, rl, l);
                let (okr, rv) = side(self, rr, r);
                let eq = format!("((({okl}) && ({okr}) && tv_str_eq({}, {})) || (!({okl}) && !({okr})))", lv.code, rv.code);
                return Some(Val::plain(if op == BinOp::Eq { eq } else { format!("(!{eq})") }, BOOL));
            }
        }
        let read = |c: &Self, e: ExprId| -> Option<(ExprId, ExprId, TyId)> {
            let ExprKind::Index { obj, index, .. } = c.ast(m).expr(e).kind else { return None };
            let ot = c.facts(m).expr_ty.get(obj as usize).copied().unwrap_or(ERROR);
            let Ty::Array(et) = *c.c.types.get(ot) else { return None };
            if !c.c.types.is_numeric(et) || !c.harmless(obj) {
                return None;
            }
            // (a loop's own arrays keep their checks: those reads are in bounds or a bug)
            if let Some(IdentFact::Local(xk)) = c.ident_fact(m, obj) {
                let b = c.bodies.last()?;
                if b.arr_cache.contains_key(&xk) || b.versioned.iter().any(|v| v.1.iter().any(|a| a.0 == xk)) {
                    return None;
                }
                if let Some(IdentFact::Local(ik)) = c.ident_fact(m, index)
                    && b.in_bounds.contains(&(ik, xk))
                {
                    return None;
                }
            }
            Some((obj, index, et))
        };
        let num = |c: &Self, e: ExprId| c.c.types.is_numeric(c.facts(m).expr_ty.get(e as usize).copied().unwrap_or(ERROR));
        let (left, (obj, index, et)) = match (read(self, le), read(self, re)) {
            (Some(x), _) if num(self, re) => (true, x),
            (None, Some(x)) if num(self, le) => (false, x),
            _ => return None,
        };
        let other = if left { r } else { l };
        let before = if left { None } else { Some(self.expr(other)) };
        let before = before.map(|v| self.own(v));
        let a = self.expr(obj);
        let i = self.expr(index);
        let i = self.int_code(i);
        let (iv, ok, ev) = (self.fresh("ix"), self.fresh("in"), self.fresh("e"));
        let ect = self.ctype(et);
        self.line(format!("tv_int {iv} = {i};"));
        self.line(format!("bool {ok} = (uint64_t){iv} < (uint64_t)tv_arr_len({});", a.code));
        self.line(format!("{ect} {ev} = {ok} ? (({ect} *)tv_arr_data({}))[{iv}] : 0;", a.code));
        let elem = Val::plain(ev, et);
        let (lv, rv) = match before {
            Some(b) => (b, elem),
            None => (elem, self.expr(other)),
        };
        let (x, y) = self.numeric_operands(lv, rv);
        // (`===` of nothing is false, `!==` true)
        Some(Val::plain(
            match op {
                BinOp::Eq | BinOp::LooseEq => format!("({ok} && {x} == {y})"),
                BinOp::Ne | BinOp::LooseNe => format!("(!{ok} || {x} != {y})"),
                _ => format!("({ok} && {x} {} {y})", op.as_str()),
            },
            BOOL,
        ))
    }

    /// `xs[i]` known present (`xs[i]!`, or read as the element type): a bounds-checked read
    /// (`None` when `obj` isn't an array).
    fn element(&mut self, obj: ExprId, index: ExprId, ty: TyId, span: Span) -> Option<Val> {
        let m = self.cur_m();
        let ot = self.ty(obj);
        let Ty::Array(et) = self.tget(ot) else { return None };
        if let Some((xk, _, cv, _, _)) = self.b().carry.clone()
            && let Some(IdentFact::Local(k)) = self.ident_fact(m, obj)
            && k == xk
            && let Some(ik) = self.b().versioned.last().and_then(|v| v.0.first().copied())
            && self.index_offset(index, ik) == Some(-1)
        {
            return Some(self.narrowed_to(Val::plain(cv, et), ty));
        }
        if !self.b().versioned.is_empty()
            && let Some(lv) = self.versioned_index(obj, index, span)
        {
            return Some(self.narrowed_to(Val::plain(lv, et), ty));
        }
        if let Some(lv) = self.row_index(obj, index, span) {
            return Some(self.narrowed_to(Val::plain(lv, et), ty));
        }
        if let Some(lv) = self.cached_index(obj, index, span) {
            return Some(self.narrowed_to(Val::plain(lv, et), ty));
        }
        let in_bounds = match (self.ident_fact(m, obj), self.ident_fact(m, index)) {
            (Some(IdentFact::Local(xk)), Some(IdentFact::Local(ik))) => self.b().in_bounds.contains(&(ik, xk)),
            _ => false,
        };
        let a = self.expr(obj);
        let i = self.expr(index);
        let i = self.index_code(i, span);
        let ect = self.ctype(et);
        if in_bounds {
            // `xs[i]` in `for (let i = 0; i < xs.length; i++)`: always in bounds.
            let v = Val::plain(format!("((({ect} *)tv_arr_data({}))[{i}])", a.code), et);
            return Some(self.narrowed_to(v, ty));
        }
        let loc = self.loc(span);
        let v = Val::plain(format!("(*({ect} *)tv_arr_at({}, sizeof({ect}), {i}, {loc}))", a.code), et);
        Some(self.narrowed_to(v, ty))
    }

    /// `u.name` as a place, `u` a union of records (place `p`) whose fields `name` are all of
    /// one C type: the field of the member it holds; `want` the type the place is read as.
    fn union_field_place(&mut self, p: &Place, name: crate::intern::Sym, n: &str, want: TyId, narrowed: TyId) -> Option<Place> {
        let keep = self.c.flat_members(narrowed);
        let mut tys = Vec::new();
        let e = self.union_field_ptr(&p.lv, p.ty, name, n, &keep, want, &mut tys, 0)?;
        let cts: Vec<String> = tys.iter().map(|&t| self.ctype(t)).collect();
        if cts.windows(2).any(|w| w[0] != w[1]) {
            return None;
        }
        let ty = if tys.windows(2).all(|w| w[0] == w[1]) {
            tys[0]
        } else if self.ctype(want) == cts[0] {
            want
        } else {
            return None;
        };
        Some(Place { lv: format!("(*{e})"), ty, weak: false })
    }

    /// A pointer to field `name` of the member union `lv` (of type `ty`) holds, among those
    /// whose flattened members are in `keep` (`undefined` never holds one): `(tag == 0 ? &a.f :
    /// &b.f)`, through recursive aliases' boxes (`Token`, a union of a union and a record).
    #[allow(clippy::too_many_arguments)]
    fn union_field_ptr(&mut self, lv: &str, ty: TyId, name: crate::intern::Sym, n: &str, keep: &[TyId], want: TyId, tys: &mut Vec<TyId>, depth: u32) -> Option<String> {
        if depth > 8 {
            return None;
        }
        let mut parts: Vec<(usize, String)> = Vec::new();
        for (i, mt) in self.members(ty).into_iter().enumerate() {
            if matches!(self.tget(mt), Ty::Undefined | Ty::Null) || !self.c.flat_members(mt).iter().any(|m| keep.contains(m)) {
                continue;
            }
            let payload = self.u_payload(lv, ty, i);
            let (inner_lv, inner) = match self.tget(mt) {
                Ty::Rec(..) => (format!("({payload})->v"), self.c.unfold(mt)),
                _ => (payload, mt),
            };
            let field = match self.tget(inner) {
                Ty::Record(fs) => {
                    let f = self.c.types.fields(fs).iter().find(|f| f.name == name).copied()?;
                    Some((self.fld(inner, &inner_lv, n), f.ty))
                }
                // (an interface's: through its table, where the value is)
                Ty::Interface(..) => Some(self.iface_field_lv(&Val::plain(inner_lv.clone(), inner), name)?),
                _ => None,
            };
            let ptr = match (field, self.tget(inner)) {
                (Some((flv, fty)), _) => {
                    // (an optional one the narrowing says is there: its value's member)
                    let (flv, fty) = if fty != want && want != ERROR && self.tag_of(fty, want).is_some() {
                        let pl = self.project_place(Place { lv: flv, ty: fty, weak: false }, want);
                        (pl.lv, pl.ty)
                    } else {
                        (flv, fty)
                    };
                    tys.push(fty);
                    format!("&({flv})")
                }
                (None, Ty::Union(_)) => self.union_field_ptr(&inner_lv, inner, name, n, keep, want, tys, depth + 1)?,
                _ => return None,
            };
            parts.push((i, ptr));
        }
        let (_, last) = parts.pop()?;
        if parts.is_empty() {
            return Some(last);
        }
        let tag = self.u_tag(lv, ty);
        let mut e = last;
        for (i, ptr) in parts.iter().rev() {
            e = format!("({tag} == {i} ? {ptr} : {e})");
        }
        Some(e)
    }

    /// `v` as `ty`: projected when `ty` is a narrowing of it (`xs[0]` after `"k" in xs[0]`), else
    /// converted.
    fn narrowed_to(&mut self, v: Val, ty: TyId) -> Val {
        if ty != v.ty && self.c.assignable_pub(ty, v.ty) && !self.c.assignable_pub(v.ty, ty) {
            return self.project(v, ty);
        }
        self.coerce(v, ty)
    }

    /// `cond ? v : undefined` as a temporary of the optional type `ty` (`v` is computed only
    /// when `cond` holds).
    pub(crate) fn present_if(&mut self, cond: &str, v: Val, ty: TyId) -> Val {
        let res = self.fresh("t");
        let ct = self.ctype(ty);
        self.line(format!("{ct} {res};"));
        self.open(&format!("if ({cond}) {{"));
        self.push_temps();
        let cv = self.coerce(v, ty);
        let code = self.consume(cv);
        self.line(format!("{res} = {code};"));
        self.pop_temps();
        self.close("} else {");
        self.b().indent += 1;
        let u = self.coerce(Val::plain("0", UNDEFINED), ty);
        let code = self.consume(u);
        self.line(format!("{res} = {code};"));
        self.close("}");
        let owned = self.is_rc(ty);
        if owned {
            self.b().temps.last_mut().unwrap().push((res.clone(), ty));
        }
        Val { code: res, ty, owned }
    }

    // ------------------------------------------------------------ places and assignment

    /// An lvalue for an assignment target or a mutated receiver (makes containers unique on the way).
    pub(crate) fn place(&mut self, e: ExprId) -> Place {
        let m = self.cur_m();
        let ast = self.ast(m);
        let span = ast.expr(e).span;
        match &ast.expr(e).kind {
            ExprKind::Paren(x) => self.place(*x),
            // `xs![i] = v`: xs checked to be there, as a read of it is
            ExprKind::NonNull(x) => {
                let p = self.place(*x);
                let to = self.ty(e);
                if p.ty != to
                    && matches!(self.tget(p.ty), Ty::Union(_))
                    && let Some(k) = self.tag_of(p.ty, to)
                {
                    let (is, loc) = (self.u_is(&p.lv, p.ty, k), self.loc(span));
                    self.line(format!("if (__builtin_expect(!{is}, 0)) tv_trap(\"value is undefined (`!` assertion failed)\", {loc});"));
                    return Place { lv: self.u_payload(&p.lv, p.ty, k), ty: to, weak: false };
                }
                p
            }
            ExprKind::Ident(_) => match self.ident_fact(m, e) {
                Some(IdentFact::Local(k)) => match self.b().locals.get(&k).cloned() {
                    Some(l) => Place { lv: l.access, ty: l.ty, weak: false },
                    None => {
                        self.unsupported(span, "assigning to this variable");
                        Place { lv: "tv__dummy".into(), ty: ERROR, weak: false }
                    }
                },
                Some(IdentFact::Const(cm, ci)) => {
                    let g = self.const_ref(cm, ci);
                    Place { lv: g.code, ty: g.ty, weak: false }
                }
                _ => {
                    self.unsupported(span, "assigning to this name");
                    Place { lv: "tv__dummy".into(), ty: ERROR, weak: false }
                }
            },
            ExprKind::Member { obj, name, .. } => {
                // `C.NAME = v`: the static field's global
                if let Some(MemberFact::StaticField(c, mi)) = self.facts(m).members.get(&e).copied() {
                    let g = self.static_field_ref(c, mi);
                    return Place { lv: g.code, ty: g.ty, weak: false };
                }
                let narrowed = self.ty(*obj);
                let nt = self.c.types.without_undefined(narrowed);
                if self.c.class_of(nt).is_some() {
                    return self.class_field_place(*obj, *name, span);
                }
                if let Ty::Interface(..) = self.tget(nt) {
                    let v = self.expr(*obj);
                    let v = self.project(v, nt);
                    let v = if self.heap_rooted(*obj) { self.own(v) } else { v };
                    if let Some((lv, ty)) = self.iface_field_lv(&v, *name) {
                        return Place { lv, ty, weak: false };
                    }
                    self.unsupported(span, "assigning to this member");
                    return Place { lv: "tv__dummy".into(), ty: ERROR, weak: false };
                }
                // (`tokens.links[k] = v`: through the array's own property, a reference)
                if matches!(self.tget(nt), Ty::Array(_)) && self.sym(*name) != "length" && self.c.array_props.contains_key(name) {
                    let v = self.expr(e);
                    let code = self.consume(v.clone());
                    let t = self.tmp(v.ty, &code, true);
                    return Place { lv: t.code, ty: t.ty, weak: false };
                }
                let raw = self.place(*obj);
                let n = self.sym(*name).to_string();
                // (a field of a union of records, each member's of one C type: through a pointer
                // to the member's it holds, `*(tag == 0 ? &a.f : &b.f)`; the members the
                // narrowing leaves out never hold it)
                let want = self.ty(e);
                if let Ty::Union(_) = self.tget(raw.ty)
                    && let Some(pl) = self.union_field_place(&raw, *name, &n, want, narrowed)
                {
                    return pl;
                }
                let p = self.project_place(raw, narrowed);
                // Through a recursive alias: unbox to reach the record.
                let p = if matches!(self.tget(p.ty), Ty::Rec(..)) {
                    let inner = self.c.unfold(p.ty);
                    self.project_place(p, inner)
                } else {
                    p
                };
                // (a recursive alias of a union, `Token`: its members')
                if let Ty::Union(_) = self.tget(p.ty)
                    && let Some(pl) = self.union_field_place(&p, *name, &n, want, narrowed)
                {
                    return pl;
                }
                let ft = match self.tget(p.ty) {
                    Ty::Record(fs) => self.c.types.fields(fs).iter().find(|f| f.name == *name).map(|f| f.ty),
                    _ => None,
                };
                match ft {
                    Some(ft) => Place { lv: self.fld(p.ty, &p.lv, &n), ty: ft, weak: false },
                    None => {
                        self.unsupported(span, "assigning to this member");
                        Place { lv: "tv__dummy".into(), ty: ERROR, weak: false }
                    }
                }
            }
            ExprKind::Call { callee, args, .. } if args.len() == 1 && matches!(ast.expr(*callee).kind, ExprKind::Member { obj, name, optional: false, .. } if self.sym(name) == "get" && self.is_map(obj)) => {
                let ExprKind::Member { obj, .. } = ast.expr(*callee).kind else { unreachable!() };
                self.map_value_place(obj, args[0].expr, span)
            }
            ExprKind::Index { obj, index, .. } if self.is_map(*obj) => self.map_value_place(*obj, *index, span),
            ExprKind::Index { obj, index, .. } => {
                let p = self.place(*obj);
                let narrowed = self.ty(*obj);
                let p = self.project_place(p, narrowed);
                let Ty::Array(et) = self.tget(p.ty) else {
                    self.unsupported(span, "assigning through this index");
                    return Place { lv: "tv__dummy".into(), ty: ERROR, weak: false };
                };
                let unique = match (&ast.expr(*obj).kind, self.ident_fact(m, *obj)) {
                    (ExprKind::Ident(_), Some(IdentFact::Local(k))) => self.b().unique.contains(&k),
                    _ => false,
                };
                if unique
                    && !self.b().versioned.is_empty()
                    && let Some(lv) = self.versioned_index(*obj, *index, span)
                {
                    return Place { lv, ty: et, weak: false };
                }
                if let Some(lv) = self.row_index(*obj, *index, span) {
                    return Place { lv, ty: et, weak: false };
                }
                if let Some(lv) = self.cached_index(*obj, *index, span) {
                    return Place { lv, ty: et, weak: false };
                }
                let ect = self.ctype(et);
                let i = self.expr(*index);
                let i = self.int_code(i);
                let d = self.desc(et);
                let loc = self.loc(span);
                let pv = self.fresh("ep");
                if unique {
                    self.line(format!("{ect} *{pv} = ({ect} *)tv_arr_at({}, sizeof({ect}), {i}, {loc});", p.lv));
                } else {
                    self.line(format!("{ect} *{pv} = ({ect} *)tvg_at_mut(&{}, {d}, sizeof({ect}), {i}, {loc});", p.lv));
                }
                Place { lv: format!("(*{pv})"), ty: et, weak: false }
            }
            _ => {
                // A temporary receiver (e.g. `xs.slice().sort()`): materialize it.
                let v = self.expr(e);
                let v = self.own(v);
                Place { lv: v.code, ty: v.ty, weak: false }
            }
        }
    }

    /// `xs` is a local that's grown with `push` (or `unshift`, `splice`) somewhere: its index
    /// writes don't append (code that pushes doesn't rely on `xs[xs.length] = v`), so they keep
    /// the plain bounds check, which leaves the array unchanged for loops around it.
    fn is_array(&mut self, e: ExprId) -> bool {
        let t = self.ty(e);
        let t = self.c.types.without_undefined(t);
        matches!(self.tget(t), Ty::Array(_))
    }

    fn is_map(&mut self, e: ExprId) -> bool {
        let t = self.ty(e);
        let t = self.c.types.without_undefined(t);
        matches!(self.tget(t), Ty::Map(..))
    }

    /// `m.get(k)!` / `m[k]` to change in place: the value's slot in the map (made unique), which
    /// must be there (TypeScript would throw reading a member of `undefined`).
    fn map_value_place(&mut self, obj: ExprId, key: ExprId, span: Span) -> Place {
        let p = self.place(obj);
        let narrowed = self.ty(obj);
        let p = self.project_place(p, narrowed);
        let Ty::Map(k, v) = self.tget(p.ty) else {
            self.unsupported(span, "changing this map's value");
            return Place { lv: "tv__dummy".into(), ty: ERROR, weak: false };
        };
        let (kd, vd, vct) = (self.desc(k), self.desc(v), self.ctype(v));
        let kv = self.expr(key);
        let kv = self.coerce(kv, k);
        let kc = kv.code;
        let kn = self.fresh("k");
        let kct = self.ctype(k);
        self.line(format!("{kct} {kn} = {kc};"));
        let loc = self.loc(span);
        let pv = self.fresh("mp");
        self.line(format!("{vct} *{pv} = ({vct} *)tv_map_get_mut(&{}, {kd}, {vd}, &{kn});", p.lv));
        self.line(format!("if (!{pv}) tv_trap(\"no such key in the map\", {loc});"));
        Place { lv: format!("(*{pv})"), ty: v, weak: false }
    }

    /// Narrows a place to a union member (payload lvalue), unboxing recursive aliases.
    fn project_place(&mut self, p: Place, to: TyId) -> Place {
        if p.ty == to || to == ERROR {
            return p;
        }
        if self.held(p.ty) == self.held(to) {
            return Place { ty: to, ..p };
        }
        if matches!(self.tget(p.ty), Ty::Rec(..)) {
            // Through the box, which is shared (as JavaScript's objects are).
            let inner = self.c.unfold(p.ty);
            return self.project_place(Place { lv: format!("({})->v", p.lv), ty: inner, weak: false }, to);
        }
        if let Some(k) = self.tag_of(p.ty, to) {
            return Place { lv: self.u_payload(&p.lv, p.ty, k), ty: to, weak: false };
        }
        if let Some((k, m)) = self.boxed_member(p.ty, to) {
            let boxed = Place { lv: self.u_payload(&p.lv, p.ty, k), ty: m, weak: false };
            return self.project_place(boxed, to);
        }
        p
    }

    /// The member of union `union` that is a recursive alias unfolding to `to`, with its tag.
    fn boxed_member(&mut self, union: TyId, to: TyId) -> Option<(usize, TyId)> {
        if !matches!(self.tget(union), Ty::Union(_)) {
            return None;
        }
        let members = self.members(union);
        members.iter().position(|&m| matches!(self.tget(m), Ty::Rec(..)) && self.c.unfold(m) == to).map(|k| (k, members[k]))
    }

    /// The place as an lvalue to read and write more than once: a variable as it is (its address
    /// isn't taken, so an async function can keep it in a register: see frame.rs), anything else
    /// through a pointer, so what reaching it costs (an index's bounds check, a copy on write)
    /// happens once.
    pub(crate) fn place_lvalue(&mut self, p: &Place) -> String {
        if p.lv.starts_with(|c: char| c.is_ascii_alphabetic() || c == '_') && p.lv.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
            return p.lv.clone();
        }
        // (a field of an object that can't change: written as `o->f`, which tells the C compiler
        // it's that field of that struct, not any double anywhere: a store to `a->vx` then leaves
        // `b->mass` in a register)
        if self.stable_places.contains(&p.lv) {
            return p.lv.clone();
        }
        let ct = self.ctype(p.ty);
        let pv = self.fresh("p");
        self.line(format!("{ct} *{pv} = &{};", p.lv));
        format!("(*{pv})")
    }

    /// `x = x op a op b ...` for a bigint variable `x` (`op` one of `+ - *`, any parentheses):
    /// the operations in order, when none of the other operands can read or change `x`.
    fn big_chain(&mut self, target: ExprId, value: ExprId) -> Option<Vec<(BinOp, ExprId)>> {
        use crate::check::IdentFact::{Const, Local};
        let m = self.cur_m();
        if self.ty(target) != BIGINT || !matches!(self.ast(m).expr(target).kind, ExprKind::Ident(_)) {
            return None;
        }
        let key = match self.ident_fact(m, target) {
            Some(f @ (Local(_) | Const(..))) => f,
            _ => return None,
        };
        let mut chain = Vec::new();
        let mut e = value;
        loop {
            while let ExprKind::Paren(x) = self.ast(m).expr(e).kind {
                e = x;
            }
            match self.ast(m).expr(e).kind {
                ExprKind::Binary(op @ (BinOp::Add | BinOp::Sub | BinOp::Mul), l, r) if self.ty(e) == BIGINT => {
                    if !self.pure_without(r, key) {
                        return None;
                    }
                    chain.push((op, r));
                    e = l;
                }
                ExprKind::Ident(_) if !chain.is_empty() && self.ident_fact(m, e) == Some(key) => break,
                _ => return None,
            }
        }
        chain.reverse();
        Some(chain)
    }

    /// Reading `e` has no effects and doesn't read the variable `key` (identifiers, literals,
    /// arithmetic, fields, `BigInt(...)`).
    fn pure_without(&mut self, e: ExprId, key: crate::check::IdentFact) -> bool {
        let m = self.cur_m();
        match &self.ast(m).expr(e).kind {
            ExprKind::Ident(_) => self.ident_fact(m, e) != Some(key),
            ExprKind::Int(_) | ExprKind::BigInt(_) | ExprKind::Float(_) | ExprKind::Str(_) | ExprKind::Bool(_) => true,
            &ExprKind::Paren(x) | &ExprKind::Unary(_, x) => self.pure_without(x, key),
            &ExprKind::Binary(op, l, r) => !matches!(op, BinOp::Div | BinOp::Rem | BinOp::Pow | BinOp::Shl) && self.pure_without(l, key) && self.pure_without(r, key),
            &ExprKind::Member { obj, optional: false, .. } => self.pure_without(obj, key),
            ExprKind::Call { callee, args, .. } if args.len() == 1 && matches!(self.ast(m).expr(*callee).kind, ExprKind::Ident(_)) && self.ident_fact(m, *callee) == Some(crate::check::IdentFact::Builtin) => {
                let (callee, arg) = (*callee, args[0].expr);
                let ExprKind::Ident(s) = self.ast(m).expr(callee).kind else { return false };
                let at = self.ty(arg);
                self.sym(s) == "BigInt" && matches!(self.tget(at), Ty::Int | Ty::BigInt | Ty::I32 | Ty::U32 | Ty::I16 | Ty::U16 | Ty::I8 | Ty::U8 | Ty::U8C) && self.pure_without(arg, key)
            }
            _ => false,
        }
    }

    /// `value` as an append to `target` (a string path): it starts with `target` (`target + a + b`,
    /// a template beginning with `${target}`, `target.concat(a, b)`), and the rest can't change
    /// `target` (it's a local nothing captures, or the rest calls nothing that could). The rest
    /// of a template's text, and the pieces to append.
    fn self_append(&self, target: ExprId, value: ExprId) -> Option<(Option<Vec<String>>, Vec<ExprId>)> {
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        fn same(ast: &crate::ast::Ast, facts: &crate::check::facts::ModuleFacts, a: ExprId, b: ExprId) -> bool {
            match (&ast.expr(a).kind, &ast.expr(b).kind) {
                (ExprKind::Paren(x), _) => same(ast, facts, *x, b),
                (_, ExprKind::Paren(y)) => same(ast, facts, a, *y),
                (ExprKind::Ident(x), ExprKind::Ident(y)) => x == y && facts.idents.get(&a).is_some() && facts.idents.get(&a) == facts.idents.get(&b),
                (ExprKind::This, ExprKind::This) => true,
                (ExprKind::Member { obj: o1, name: n1, optional: false, .. }, ExprKind::Member { obj: o2, name: n2, optional: false, .. }) => {
                    n1 == n2 && !facts.calls.contains_key(&a) && !facts.calls.contains_key(&b) && same(ast, facts, *o1, *o2)
                }
                _ => false,
            }
        }
        let (tparts, rest): (Option<Vec<String>>, Vec<ExprId>) = match &ast.expr(value).kind {
            ExprKind::Binary(BinOp::Add, ..) => {
                let mut parts = Vec::new();
                let mut cur = value;
                while let ExprKind::Binary(BinOp::Add, a, b) = ast.expr(cur).kind {
                    if facts.expr_ty.get(cur as usize) != Some(&STR) {
                        break;
                    }
                    parts.push(b);
                    cur = a;
                }
                if !same(ast, facts, cur, target) {
                    return None;
                }
                parts.reverse();
                (None, parts)
            }
            ExprKind::Template(ps, xs) if ps.first().is_some_and(|p| p.is_empty()) && xs.first().is_some_and(|&x| same(ast, facts, x, target)) => (Some(ps[1..].to_vec()), xs[1..].to_vec()),
            ExprKind::Call { callee, args, optional: false, .. }
                if matches!(&ast.expr(*callee).kind, ExprKind::Member { obj, name, optional: false, .. } if self.sym(*name) == "concat" && same(ast, facts, *obj, target))
                    && matches!(facts.calls.get(&value).map(|c| &c.callee), Some(Callee::Method(t)) if *t == STR)
                    && args.iter().all(|a| !matches!(ast.expr(a.expr).kind, ExprKind::Spread(_))) =>
            {
                (None, args.iter().map(|a| a.expr).collect())
            }
            _ => return None,
        };
        // (a local nothing captures can only change here)
        let own_local = match (&ast.expr(target).kind, facts.idents.get(&target)) {
            (ExprKind::Ident(_), Some(IdentFact::Local(k))) => !self.bodies.last().is_some_and(|b| b.boxed.contains(k)),
            _ => false,
        };
        if !own_local && !rest.iter().all(|&x| self.harmless(x)) {
            return None;
        }
        Some((tparts, rest))
    }

    /// Can evaluating `e` change no variable or field (no assignment, and no call but built-ins on
    /// numbers and strings)?
    pub(crate) fn harmless(&self, e: ExprId) -> bool {
        let m = self.cur_m();
        let ast = self.ast(m);
        let facts = self.facts(m);
        let mut es = Vec::new();
        collect_exprs_expr(ast, e, &mut es);
        es.iter().all(|&x| match &ast.expr(x).kind {
            ExprKind::Assign(..) | ExprKind::Update { .. } | ExprKind::Await(_) | ExprKind::Yield(..) | ExprKind::Arrow(_) | ExprKind::New { .. } => false,
            ExprKind::Call { .. } => match facts.calls.get(&x).map(|c| &c.callee) {
                Some(Callee::Method(t)) => self.c.types.is_string(*t) || self.c.types.is_numeric(*t) || *t == BOOL,
                Some(Callee::Builtin { ns, name }) => {
                    let n = self.sym(*name);
                    match ns.map(|s| self.sym(s)) {
                        Some("Math") => true,
                        None => matches!(n, "String" | "Number" | "parseInt" | "parseFloat" | "isNaN" | "isFinite") && {
                            let ExprKind::Call { args, .. } = &ast.expr(x).kind else { return false };
                            args.iter().all(|a| {
                                let t = facts.expr_ty.get(a.expr as usize).copied().unwrap_or(ERROR);
                                self.c.types.is_string(t) || self.c.types.is_numeric(t) || t == BOOL
                            })
                        },
                        _ => false,
                    }
                }
                _ => false,
            },
            // (a getter is a call)
            ExprKind::Member { .. } => !facts.calls.contains_key(&x),
            _ => true,
        })
    }

    fn assign(&mut self, op: AssignOp, target: ExprId, value: ExprId, span: Span) -> Val {
        let assign_ty = self.assign_ty.take();
        // a JavaScript property: assigned in JavaScript
        let m = self.cur_m();
        // (a property of what an `any` holds)
        if let ExprKind::Member { obj, .. } | ExprKind::Index { obj, .. } = self.ast(m).expr(target).kind
            && self.ty(obj) == ANY
        {
            return self.any_assign(op, target, value, span);
        }
        // (an array's own property, `this.tokens.links = {}`: kept for it)
        if let ExprKind::Member { obj, name, .. } = self.ast(m).expr(target).kind
            && matches!(op, AssignOp::Assign)
            && let ot = self.ty(obj)
            && matches!(self.tget(ot), Ty::Array(_))
            && self.sym(name) != "length"
            && self.c.array_props.contains_key(&name)
        {
            let ap = self.place(obj);
            let v = self.expr(value);
            let v = self.own(v);
            let d = self.coerce(Val { owned: false, ..v.clone() }, ANY);
            let d = self.own(d);
            let dc = self.consume(d);
            let key = self.lit(self.sym(name).to_string().as_bytes());
            self.line(format!("tv_arr_prop_set(&{}, {key}, {dc});", ap.lv));
            return Val { owned: false, ..v };
        }
        // (a field of a union of records, each member's; a key of a record with an index
        // signature, in its `$rest`)
        if let ExprKind::Member { obj, name, .. } = self.ast(m).expr(target).kind {
            let nt = self.ty(obj);
            let nt = self.c.types.without_undefined(nt);
            let union = matches!(self.tget(nt), Ty::Union(_)) && self.members(nt).iter().all(|&mt| matches!(self.tget(mt), Ty::Record(_)) && self.tuple_of(mt).is_none());
            // (a union of records and interfaces, through recursive aliases: `Token`)
            let un = self.c.unfold(nt);
            let union = union
                || matches!(self.tget(un), Ty::Union(_)) && self.c.flat_members(un).iter().all(|&mt| matches!(self.tget(mt), Ty::Record(_) | Ty::Interface(..)) && self.tuple_of(mt).is_none());
            let rest = matches!(self.tget(nt), Ty::Record(fs) if !self.c.types.fields(fs).iter().any(|f| f.name == name)) && self.rest_field(nt).is_some();
            // (a key an interface with an index signature doesn't name: set by name)
            let ui = self.c.unfold(nt);
            let rest = rest
                || ui != nt && matches!(self.tget(ui), Ty::Record(fs) if !self.c.types.fields(fs).iter().any(|f| f.name == name)) && self.rest_field(ui).is_some()
                || matches!(self.tget(ui), Ty::Interface(..))
                    && self.iface_keyed(ui)
                    && self.iface_members(ui).iter().find(|f| f.name == name).is_none_or(|f| self.c.types.is_string(f.ty) || f.ty == ANY);
            if union || rest {
                return self.record_member_assign(op, obj, name, value, nt, span);
            }
        }
        if let ExprKind::Member { obj, .. } | ExprKind::Index { obj, .. } = self.ast(m).expr(target).kind {
            let ot = self.ty(obj);
            if self.c.types.without_undefined(ot) == JS {
                let js_op = match op {
                    AssignOp::Assign => "=".to_string(),
                    AssignOp::Op(b) => format!("{}=", b.as_str()),
                };
                let ty = self.ty(target);
                return self.js_assign(&js_op, target, value, ty, span);
            }
        }
        // (`x op= v` of an `any`: the operator on what it holds, stored as an `any`)
        if let AssignOp::Op(b) = op
            && !matches!(b, BinOp::Nullish | BinOp::And | BinOp::Or)
            && !matches!(self.ast(m).expr(target).kind, ExprKind::Member { .. } | ExprKind::Index { .. })
        {
            let p = self.place(target);
            // (`n += x` of an `any` `x`: computed by what it holds, stored as `n`'s type)
            if p.ty != ANY && !p.weak && self.ty(value) == ANY {
                let lv = self.place_lvalue(&p);
                let cur = Val::plain(lv.clone(), p.ty);
                let rv = self.expr(value);
                let rt = self.c.any_binary(b, p.ty, ANY);
                let r = self.any_binop(b, cur, rv, rt);
                let r = self.coerce(r, p.ty);
                let r = self.own(r);
                let code = self.consume(r);
                let ct = self.ctype(p.ty);
                let nv = self.fresh("n");
                let rel = self.release_code(p.ty, &lv);
                let rel = if rel.is_empty() { String::new() } else { format!("{rel}; ") };
                self.line(format!("{ct} {nv} = {code}; {rel}{lv} = {nv};"));
                return Val::plain(lv, p.ty);
            }
            if p.ty == ANY && !p.weak {
                let lv = self.place_lvalue(&p);
                let cur = Val::plain(lv.clone(), ANY);
                let rv = self.expr(value);
                let rt = self.c.any_binary(b, ANY, rv.ty);
                let r = self.any_binop(b, cur, rv, rt);
                let d = self.coerce(r, ANY);
                let d = self.own(d);
                let code = self.consume(d);
                let nv = self.fresh("n");
                self.line(format!("tv_dyn {nv} = {code}; tv_dyn_release({lv}); {lv} = {nv};"));
                return Val::plain(lv, ANY);
            }
        }
        // `s = s + a + b`, `s = `${s}...``, `s = s.concat(a)`: appended in place (see
        // `tv_str_append_bytes`), so a loop building a string is linear
        if op == AssignOp::Assign
            && self.ty(target) == STR
            && let Some((tparts, rest)) = self.self_append(target, value)
        {
            let p = self.place(target);
            let lv = self.place_lvalue(&p);
            if p.weak || p.ty != STR {
                // (held in a union or weakly: the whole value, stored as any other; the target
                // is a plain path, so reading it after its place changes nothing)
                let v = self.expr(value);
                if p.weak {
                    return self.weak_store(&p.lv, p.ty, v);
                }
                let v = self.coerce(v, p.ty);
                let code = self.consume(v);
                let ct = self.ctype(p.ty);
                let nv = self.fresh("n");
                self.line(format!("{ct} {nv} = {code};"));
                let r = self.release_code(p.ty, &lv);
                self.line(format!("{r}; {lv} = {nv};"));
                return Val::plain(lv, p.ty);
            }
            let vals: Vec<Val> = self.exprs_in_order(&rest);
            let sbv = self.fresh("sb");
            self.line(format!("tv_sb {sbv} = {{0}};"));
            self.open("{");
            self.line(format!("tv_sb *sb = &{sbv};"));
            match tparts {
                Some(parts) => self.template_into(&parts, &vals),
                None => {
                    for v in &vals {
                        let c = self.string_code(v.ty, &v.code);
                        self.line(format!("{c};"));
                    }
                }
            }
            self.close("}");
            self.line(format!("tv_str_append_bytes(&({lv}), {sbv}.data, {sbv}.len, 0); tv_sb_free(&{sbv});"));
            return Val::plain(lv, STR);
        }
        // `xs.length = n` (`xs.length -= k`): drops the elements from n on
        if let ExprKind::Member { obj, name, .. } = self.ast(m).expr(target).kind
            && self.sym(name) == "length"
            && self.is_array(obj)
        {
            let ap = self.place(obj);
            let narrowed = self.ty(obj);
            let ap = self.project_place(ap, narrowed);
            let Ty::Array(et) = self.tget(ap.ty) else {
                self.unsupported(span, "assigning this length");
                return Val::plain("0", VOID);
            };
            let d = self.desc(et);
            let v = match op {
                AssignOp::Assign => {
                    let v = self.expr(value);
                    self.int_code(v)
                }
                AssignOp::Op(b) => {
                    let cur = Val::plain(format!("tv_arr_len({})", ap.lv), INT);
                    let rv = self.expr(value);
                    // (a float operand: computed as one, then made whole)
                    let rt = if self.c.types.is_float(rv.ty) { F64 } else { INT };
                    let r = self.arith(b, cur, rv, rt, span);
                    self.int_code(r)
                }
            };
            let (n, loc) = (self.fresh("n"), self.loc(span));
            if self.c.types.has_undefined(et) {
                // (grown with `undefined`s, the holes JavaScript would leave)
                let (ect, u) = (self.ctype(et), self.default_value(et));
                let z = self.fresh("z");
                self.line(format!("tv_int {n} = {v}; {ect} {z} = {u}; tv_arr_set_length_fill(&{}, {d}, {n}, &{z}, {loc});", ap.lv));
            } else {
                self.line(format!("tv_int {n} = {v}; tv_arr_set_length(&{}, {d}, {n}, {loc});", ap.lv));
            }
            return Val::plain(n, INT);
        }
        // `counts[w] = n` / `counts[w] += 1` on a map: set (after reading, for `+=`)
        let map_target = match self.ast(m).expr(target).kind {
            ExprKind::Index { obj, index, .. } => {
                let ot = self.ty(obj);
                let ot = self.c.types.without_undefined(ot);
                match self.tget(ot) {
                    Ty::Map(k, v) => Some((obj, index, k, v)),
                    _ => None,
                }
            }
            _ => None,
        };
        if let Some((obj, index, k, v)) = map_target {
            let (kd, vd, kct, vct) = (self.desc(k), self.desc(v), self.ctype(k), self.ctype(v));
            // (a local narrowed to the map: its map, not the union it's kept in)
            let mp = self.place(obj);
            let mt = self.c.types.intern(Ty::Map(k, v));
            let mp = self.project_place(mp, mt);
            let kv = self.expr(index);
            let kv = self.coerce(kv, k);
            let kv = self.own(kv);
            let kc = self.consume(kv);
            let kn = self.fresh("k");
            self.line(format!("{kct} {kn} = {kc};"));
            // `m[k] ??= v`: v when k is missing (or undefined); the value either way
            if matches!(op, AssignOp::Op(BinOp::Nullish)) {
                let (p, ins) = (self.fresh("p"), self.fresh("ins"));
                self.line(format!("bool {ins}; {vct} *{p} = ({vct} *)tv_map_slot(&{}, {kd}, {vd}, &{kn}, &{ins});", mp.lv));
                let absent = match self.tag_of(v, UNDEFINED) {
                    Some(t) => format!("({ins} || {})", self.u_is(&format!("(*{p})"), v, t)),
                    None => ins.clone(),
                };
                self.open_block(&format!("if ({absent}) {{"));
                let vv = self.expr(value);
                let vv = self.coerce(vv, v);
                let vv = self.own(vv);
                let vc = self.consume(vv);
                let vn = self.fresh("v");
                self.line(format!("{vct} {vn} = {vc};"));
                // (the slot is re-read: evaluating v may have changed the map)
                self.line(format!("{p} = ({vct} *)tv_map_slot(&{}, {kd}, {vd}, &{kn}, &{ins});", mp.lv));
                let rel = self.release_code(v, &format!("(*{p})"));
                self.line(format!("if (!{ins}) {{ {rel}; }} *{p} = {vn};"));
                self.close_block("}");
                return Val::plain(format!("(*{p})"), v);
            }
            let nv = match op {
                AssignOp::Assign => {
                    let vv = self.expr(value);
                    self.coerce(vv, v)
                }
                // (`m[k] += x` in a loop over `m`'s keys: the current entry's value, in place)
                AssignOp::Op(bop) if let Some(ep) = self.map_loop_value(&mp.lv, index) => {
                    let cur = Val::plain(format!("(*({vct} *){ep})"), v);
                    let rv = self.expr(value);
                    let r = self.arith(bop, cur, rv, v, span);
                    let r = self.coerce(r, v);
                    let r = self.own(r);
                    let rc = self.consume(r);
                    let vn = self.fresh("v");
                    self.line(format!("{vct} {vn} = {rc};"));
                    let rel = self.release_code(v, &format!("(*({vct} *){ep})"));
                    self.line(format!("{rel}; *({vct} *){ep} = {vn};"));
                    let rel_k = self.release_code(k, &kn);
                    self.line(format!("{rel_k};"));
                    return Val { code: vn, ty: v, owned: false };
                }
                AssignOp::Op(bop) => {
                    // the current value (traps if missing: `undefined + 1` has no value of its type)
                    let loc = self.loc(span);
                    let p = self.fresh("p");
                    self.line(format!("{vct} *{p} = ({vct} *)tv_map_get({}, {kd}, {vd}, &{kn});", mp.lv));
                    self.line(format!("if (!{p}) tv_trap(\"no such key in the map\", {loc});"));
                    let cur = Val::plain(format!("(*{p})"), v);
                    let rv = self.expr(value);
                    let r = self.arith(bop, cur, rv, v, span);
                    self.coerce(r, v)
                }
            };
            let nv = self.own(nv);
            let vc = self.consume(nv);
            let vn = self.fresh("v");
            self.line(format!("{vct} {vn} = {vc};"));
            self.line(format!("tv_map_set(&{}, {kd}, {vd}, &{kn}, &{vn});", mp.lv));
            // (the assignment's value: what was stored, which the map holds)
            return Val { code: vn, ty: v, owned: false };
        }
        match op {
            AssignOp::Assign if let Some(chain) = self.big_chain(target, value) => {
                // `x = (x + a * b) * c`: as `x += a * b; x *= c` (x's old value goes either way, and
                // nothing else reads x), each in place when it can be
                let mut v = Val::plain("0", BIGINT);
                for (bop, r) in chain {
                    v = self.assign(AssignOp::Op(bop), target, r, span);
                }
                v
            }
            // `xs[xs.length] = v`: appends, as in JavaScript (an index past that traps)
            AssignOp::Assign if (self.b().versioned.is_empty() || self.cached_array(target)) && !self.row_target(target) && matches!(self.ast(m).expr(target).kind, ExprKind::Index { obj, optional: false, .. } if self.is_array(obj)) => {
                let ExprKind::Index { obj, index, .. } = self.ast(m).expr(target).kind else { unreachable!() };
                let v = self.expr(value);
                let ap = self.place(obj);
                let narrowed = self.ty(obj);
                let ap = self.project_place(ap, narrowed);
                let Ty::Array(et) = self.tget(ap.ty) else {
                    self.unsupported(span, "assigning through this index");
                    return Val::plain("0", VOID);
                };
                let v = self.coerce(v, et);
                let code = self.consume(v);
                let (ect, d) = (self.ctype(et), self.desc(et));
                let nv = self.fresh("n");
                self.line(format!("{ect} {nv} = {code};"));
                let iv = self.expr(index);
                let iv = self.int_code(iv);
                let loc = self.loc(span);
                let unique = matches!((&self.ast(m).expr(obj).kind, self.ident_fact(m, obj)), (ExprKind::Ident(_), Some(IdentFact::Local(k))) if self.b().unique.contains(&k));
                // (an append moves the capacity a loop's pushes keep in a local)
                let cap = match self.ident_fact(m, obj) {
                    Some(IdentFact::Local(k)) => self.b().push_caps.get(&k).cloned(),
                    _ => None,
                };
                // (a typed array's, in TypeScript: a store past either end is dropped, as
                // JavaScript drops it; its length never changes)
                if matches!(self.tget(et), Ty::I8 | Ty::U8 | Ty::U8C | Ty::I16 | Ty::U16 | Ty::I32 | Ty::U32 | Ty::F32) && self.js_ints(m) {
                    let (i, a) = (self.fresh("i"), ap.lv.clone());
                    self.line(format!("tv_int {i} = {iv};"));
                    self.open(&format!("if (__builtin_expect((uint64_t){i} < (uint64_t)tv_arr_len({a}), 1)) {{"));
                    if !unique {
                        self.line(format!("tvg_make_unique(&{a}, {d});"));
                    }
                    let store = self.store(target, et, &format!("((({ect} *)tv_arr_data({a}))[{i}])"), &nv);
                    self.line(store);
                    self.close("}");
                    let _ = (&loc, &cap);
                    return Val::plain(nv, et);
                }
                if !self.is_rc(et) {
                    // (plain data: the append is the bounds check's slow path)
                    let (i, a) = (self.fresh("i"), ap.lv.clone());
                    self.line(format!("tv_int {i} = {iv};"));
                    if !unique {
                        self.line(format!("tvg_make_unique(&{a}, {d});"));
                    }
                    let recap = cap.as_ref().map(|c| format!(" {c} = {a}.p->cap;")).unwrap_or_default();
                    // (an array the loop keeps in locals: against its kept length, refreshed if it grows)
                    let cached = match self.ident_fact(m, obj) {
                        Some(IdentFact::Local(k)) => self.b().arr_cache.get(&k).cloned(),
                        _ => None,
                    };
                    if let Some((cd, cn)) = cached {
                        self.line(format!("if (__builtin_expect((uint64_t){i} >= (uint64_t){cn}, 0)) {{ {a} = tvg_put_grow({a}, {d}, sizeof({ect}), {i}, {loc});{recap} {cd} = ({ect} *)tv_arr_data({a}); {cn} = tv_arr_len({a}); }}"));
                        let store = self.store(target, et, &format!("({cd}[{i}])"), &nv);
                        self.line(store);
                        return Val::plain(nv, et);
                    }
                    self.line(format!("if (__builtin_expect((uint64_t){i} >= (uint64_t)tv_arr_len({a}), 0)) {{ {a} = tvg_put_grow({a}, {d}, sizeof({ect}), {i}, {loc});{recap} }}"));
                    let store = self.store(target, et, &format!("((({ect} *)tv_arr_data({a}))[{i}])"), &nv);
                    self.line(store);
                    return Val::plain(nv, et);
                }
                let (i, pa) = (self.fresh("i"), self.fresh("pa"));
                self.line(format!("tv_int {i} = {iv}; tv_arr *{pa} = &{};", ap.lv));
                // (in bounds: the common case, first; an index in bounds is of an array of its
                // own, the one array that isn't, the immortal empty one, having no elements)
                let ep0 = self.fresh("ep");
                self.open(&format!("if (__builtin_expect((uint64_t){i} < (uint64_t)tv_arr_len(*{pa}), 1)) {{"));
                self.line(format!("{ect} *{ep0} = ({ect} *)tv_arr_data(*{pa}) + {i};"));
                // (its old value taken already: see `take_plan`)
                let taken = self.b().no_release == Some(target);
                if self.is_rc(et) && !taken {
                    let r = self.release_code(et, &format!("(*{ep0})"));
                    self.line(format!("{r};"));
                }
                let store = self.store(target, et, &format!("(*{ep0})"), &nv);
                self.line(store);
                self.close(&format!("}} else if ({i} == tv_arr_len(*{pa})) {{"));
                self.b().indent += 1;
                self.line(format!("TVG_PUSH({ect}, {pa}, {d}, {nv});"));
                if let Some(c) = &cap {
                    self.line(format!("{c} = {pa}->p->cap;"));
                }
                // (past the end, in TypeScript, of class instances: the array grows to it, the
                // slots between holding none, JavaScript's holes, read as `undefined`)
                if self.js_ints(m) && matches!(self.tget(et), Ty::Class(..)) {
                    self.close(&format!("}} else if ({i} > tv_arr_len(*{pa})) {{"));
                    self.b().indent += 1;
                    self.line(format!("*{pa} = tvg_put_grow(*{pa}, {d}, sizeof({ect}), {i}, {loc});"));
                    if let Some(c) = &cap {
                        self.line(format!("{c} = {pa}->p->cap;"));
                    }
                    let store = self.store(target, et, &format!("((({ect} *)tv_arr_data(*{pa}))[{i}])"), &nv);
                    self.line(store);
                }
                self.close("} else {");
                self.b().indent += 1;
                let ep = self.fresh("ep");
                // (a local made unique before: no copy check)
                if unique {
                    self.line(format!("{ect} *{ep} = ({ect} *)tv_arr_at(*{pa}, sizeof({ect}), {i}, {loc});"));
                } else {
                    self.line(format!("{ect} *{ep} = ({ect} *)tvg_at_mut({pa}, {d}, sizeof({ect}), {i}, {loc});"));
                }
                if self.is_rc(et) && !taken {
                    let r = self.release_code(et, &format!("(*{ep})"));
                    self.line(format!("{r};"));
                }
                let store = self.store(target, et, &format!("(*{ep})"), &nv);
                self.line(store);
                self.close("}");
                Val::plain(nv, et)
            }
            AssignOp::Assign => {
                let v = self.expr(value);
                let p = self.place(target);
                if p.weak {
                    return self.weak_store(&p.lv, p.ty, v);
                }
                // (`x = P` taking P's reference: see `take_assign_pair`)
                if self.b().take_assign == Some(value) {
                    self.b().take_assign = None;
                    let v = self.coerce(v, p.ty);
                    if !v.owned {
                        let lv = self.place_lvalue(&p);
                        let ct = self.ctype(p.ty);
                        let nv = self.fresh("n");
                        let r = self.release_code(p.ty, &lv);
                        self.line(format!("{ct} {nv} = {}; {r}; {lv} = {nv};", v.code));
                        return Val::plain(lv, p.ty);
                    }
                    // (owned already: the plan can't hold, so the store releases as usual)
                    self.b().take_failed = true;
                    let code = self.consume(v);
                    let lv = self.place_lvalue(&p);
                    let ct = self.ctype(p.ty);
                    let nv = self.fresh("n");
                    let r = self.release_code(p.ty, &lv);
                    self.line(format!("{ct} {nv} = {code}; {r}; {lv} = {nv};"));
                    return Val::plain(lv, p.ty);
                }
                // (a cursor: points at the next node, borrowed)
                if let Some(IdentFact::Local(k)) = self.ident_fact(m, target)
                    && self.b().cursors.contains(&k)
                {
                    let v = self.coerce(v, p.ty);
                    if v.owned {
                        self.internal.push("a cursor given an owned value".into());
                    }
                    let lv = self.place_lvalue(&p);
                    // (the value made opaque, by an asm no other matches: `if (c) x = x.left; else
                    // x = x.right` stays two branches, which the processor runs ahead on as it
                    // predicts them; merged into a load whose address waits on `c`, a walk down a
                    // tree out of cache takes each node's miss after the last)
                    let ct = self.ctype(p.ty);
                    if ct.ends_with('*') {
                        let tag = self.fresh("cur");
                        self.line(format!("{lv} = {}; __asm__(\"\" \"/* {tag} */\" : \"+r\"({lv}));", v.code));
                    } else {
                        self.line(format!("{lv} = {};", v.code));
                    }
                    return Val::plain(lv, p.ty);
                }
                let v = self.coerce(v, p.ty);
                let code = self.consume(v);
                let ct = self.ctype(p.ty);
                let lv = self.place_lvalue(&p);
                if self.is_rc(p.ty) && self.b().no_release == Some(target) {
                    // (its value was taken by the store before: see `unlink_pair`)
                    self.line(format!("{lv} = {code};"));
                } else if self.is_rc(p.ty) {
                    let nv = self.fresh("n");
                    self.line(format!("{ct} {nv} = {code};"));
                    let r = self.release_code(p.ty, &lv);
                    self.line(format!("{r}; {lv} = {nv};"));
                } else {
                    let store = self.store(target, p.ty, &lv, &code);
                    self.line(store);
                }
                Val::plain(lv, p.ty)
            }
            AssignOp::Op(bop) => {
                let p = self.place(target);
                let lv = self.place_lvalue(&p);
                // (`let i; i = 0; i += 1`: kept as `number | undefined`, narrowed to the number:
                // computed as that, then kept as the union again)
                let narrowed = assign_ty.unwrap_or(p.ty);
                if matches!(self.tget(p.ty), Ty::Union(_))
                    && narrowed != p.ty
                    && self.c.types.is_numeric(narrowed)
                    && self.tag_of(p.ty, narrowed).is_some()
                    && !matches!(bop, BinOp::Nullish | BinOp::And | BinOp::Or)
                {
                    let cur = self.project(Val::plain(lv.clone(), p.ty), narrowed);
                    let rhs = self.expr(value);
                    let r = self.compound_val(bop, cur, rhs, narrowed, span);
                    let r = self.tmp(narrowed, &r.code, false);
                    let u = self.coerce(r.clone(), p.ty);
                    self.line(format!("{lv} = {};", u.code));
                    return r;
                }
                let cur = Val::plain(lv.clone(), p.ty);
                // `x += await f()` reads `x` before it waits, as JavaScript does.
                let cur = if self.awaits_in(value) { self.snapshot(cur) } else { cur };
                if p.ty == BIGINT && matches!(bop, BinOp::Add | BinOp::Sub)
                    && let Some((a, b)) = self.big_mul_parts(value)
                {
                    // `x += a * b`: one pass, in place
                    let av = self.expr_before(a, &[b]);
                    let bv = self.expr(b);
                    let sign = if bop == BinOp::Add { 1 } else { -1 };
                    self.line(format!("tv_big_muladd_to(&{lv}, {}, {}, {sign});", av.code, bv.code));
                    return cur;
                }
                // `out += s.slice(a, b)`: the units appended, the slice never made
                if p.ty == STR
                    && bop == BinOp::Add
                    && let ExprKind::Call { callee, args, optional: false, .. } = &self.ast(m).expr(value).kind
                    && let ExprKind::Member { obj: so, name: sl, optional: false, .. } = self.ast(m).expr(*callee).kind
                    && self.sym(sl) == "slice"
                    && (1..=2).contains(&args.len())
                    && !args.iter().any(|a| matches!(self.ast(m).expr(a.expr).kind, ExprKind::Spread(_)))
                    && {
                        let st = self.ty(so);
                        matches!(self.tget(st), Ty::Str)
                    }
                {
                    let sv = self.expr_before(so, &args.iter().map(|a| a.expr).collect::<Vec<_>>());
                    let a0 = self.expr(args[0].expr);
                    let a0 = self.coerce(a0, INT);
                    let (a1, has_end) = match args.get(1) {
                        Some(a) => {
                            let v = self.expr(a.expr);
                            (self.coerce(v, INT).code, "true")
                        }
                        None => ("0".to_string(), "false"),
                    };
                    self.line(format!("tv_str_append_slice(&({lv}), {}, {}, {a1}, {has_end});", sv.code, a0.code));
                    return cur;
                }
                // `x ??= v`: v is evaluated only when x is nullish
                if bop == BinOp::Nullish {
                    let present = self.truthy(cur.clone());
                    self.open(&format!("if (!{present}) {{"));
                    self.push_temps();
                    let rhs = self.expr(value);
                    let v = self.coerce(rhs, p.ty);
                    let code = self.consume(v);
                    let r = self.release_code(p.ty, &lv);
                    self.line(format!("{r}; {lv} = {code};"));
                    self.pop_temps();
                    self.close("}");
                    return cur;
                }
                let rhs = self.expr(value);
                if p.ty == BIGINT {
                    // in place: the variable's buffer is reused when it's the only reference
                    match bop {
                        BinOp::Add => self.line(format!("tv_big_add_to(&{lv}, {});", rhs.code)),
                        BinOp::Sub => self.line(format!("tv_big_sub_to(&{lv}, {});", rhs.code)),
                        BinOp::Mul => self.line(format!("tv_big_mul_to(&{lv}, {});", rhs.code)),
                        _ => {
                            let r = self.big_op(bop, cur.clone(), rhs, span);
                            let code = self.consume(r);
                            self.line(format!("tv_big_release({lv}); {lv} = {code};"));
                        }
                    }
                    return cur;
                }
                let result = match bop {
                    BinOp::Add if p.ty == STR => {
                        // (in place when the variable holds the only reference: a loop of `+=`
                        // is linear)
                        if rhs.ty == STR {
                            self.line(format!("tv_str_append(&({lv}), {});", rhs.code));
                            return cur;
                        }
                        let sbv = self.fresh("sb");
                        self.line(format!("tv_sb {sbv} = {{0}};"));
                        self.open("{");
                        self.line(format!("tv_sb *sb = &{sbv};"));
                        let c = self.string_code(rhs.ty, &rhs.code);
                        self.line(format!("{c};"));
                        self.close("}");
                        self.line(format!("tv_str_append_bytes(&({lv}), {sbv}.data, {sbv}.len, 0); tv_sb_free(&{sbv});"));
                        return cur;
                    }
                    BinOp::BitAnd | BinOp::BitOr | BinOp::BitXor | BinOp::Shl | BinOp::Shr | BinOp::UShr => {
                        let ty = if p.ty != INT && p.ty != U8C && self.c.types.is_int(p.ty) { p.ty } else { INT };
                        let code = self.bitop(bop, &cur, &rhs, ty);
                        self.coerce(Val::plain(code, ty), p.ty)
                    }
                    _ => {
                        let arith_ty = if p.ty == DECIMAL {
                            DECIMAL
                        } else if bop == BinOp::Div || self.c.types.is_float(p.ty) || self.c.types.is_float(rhs.ty) {
                            if p.ty == F32 { F32 } else { F64 }
                        } else {
                            p.ty
                        };
                        // (`bytes[i] += 200` in TypeScript: a number, stored wrapped as a typed
                        // array's element is)
                        if arith_ty != INT && super::super::check::promoted_ty(arith_ty) == INT && (self.js_ints(self.cur_m()) || arith_ty == U8C) {
                            let r = self.arith(bop, cur.clone(), rhs, INT, span);
                            self.coerce(r, p.ty)
                        } else if arith_ty == INT && matches!(bop, BinOp::Add | BinOp::Sub) && rhs.ty == INT && self.cannot_overflow(bop, target, value) {
                            // (`z += 1` where z is bounded: see `range`)
                            Val::plain(format!("({} {} {})", cur.code, if bop == BinOp::Add { "+" } else { "-" }, rhs.code), INT)
                        } else {
                            self.js_mul_operand = has_mul(self.ast(self.cur_m()), value);
                            let r = self.arith(bop, cur.clone(), rhs, arith_ty, span);
                            self.coerce(r, p.ty)
                        }
                    }
                };
                let store = self.store(target, p.ty, &lv, &result.code);
                self.line(store);
                cur
            }
        }
    }

    /// `lv = code;` for a plain value. A byte stored into an array goes through a type that
    /// isn't a character type (`TV_U8S`): through `uint8_t` it could change any object as far as
    /// the C compiler knows, so it would reload around it whatever it had in registers (a global
    /// the loop updates, say).
    fn store(&mut self, target: ExprId, ty: TyId, lv: &str, code: &str) -> String {
        let m = self.cur_m();
        let elem = match self.ast(m).expr(target).kind {
            ExprKind::Index { obj, .. } => {
                let ot = self.ty(obj);
                matches!(self.tget(ot), Ty::Array(_))
            }
            _ => false,
        };
        match self.tget(ty) {
            Ty::U8 | Ty::U8C if elem => format!("*(TV_U8S *)&{lv} = (TV_U8S)({code});"),
            Ty::I8 if elem => format!("*(TV_I8S *)&{lv} = (TV_I8S)({code});"),
            _ => format!("{lv} = {code};"),
        }
    }

    // ------------------------------------------------------------ closures

    /// A `tv_fn` value for an arrow expression (environment on the heap).
    fn closure_value(&mut self, e: ExprId, fty: TyId) -> Val {
        // (one only called while its captures are in scope: see `analyze_closures`)
        if self.b().stack_arrows.contains(&e) {
            let info = self.emit_arrow(e, fty, true);
            if info.captures.is_empty() {
                return Val::plain(format!("((tv_fn){{ (void *){}, NULL }})", info.cname), fty);
            }
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
            return Val::plain(format!("((tv_fn){{ (void *){}, (tv_env *)&{ev} }})", info.cname), fty);
        }
        let info = self.emit_arrow(e, fty, false);
        if info.captures.is_empty() {
            return Val::plain(format!("((tv_fn){{ (void *){}, NULL }})", info.cname), fty);
        }
        let env = self.fresh("env");
        let et = info.env_type.clone();
        self.line(format!("{et} *{env} = tvg_alloc_small(sizeof({et})); {env}->h.rc = 1; {env}->h.size = TVG_ENV_SIZE(sizeof({et})); {env}->h.drop = drop_{et};"));
        for (j, (key, ty, boxed)) in info.captures.iter().enumerate() {
            let local = self.b().locals.get(key).cloned();
            let Some(local) = local else { continue };
            if *boxed {
                // Share the cell.
                let cell = local.access.trim_end_matches("->v").to_string();
                self.line(format!("{env}->c{j} = {cell}; {cell}->rc++;"));
            } else {
                let v = Val::plain(local.access.clone(), *ty);
                let code = self.consume(v);
                self.line(format!("{env}->c{j} = {code};"));
            }
        }
        self.tmp(fty, &format!("((tv_fn){{ (void *){}, (tv_env *){env} }})", info.cname), true)
    }

    /// Emits the C function for an arrow. Stack mode: the environment holds pointers to the
    /// captured variables (valid for the duration of a built-in method call).
    pub(crate) fn emit_arrow(&mut self, e: ExprId, fty: TyId, stack: bool) -> ArrowInfo {
        let m = self.cur_m();
        let ast = self.ast(m);
        if !matches!(self.tget(fty), Ty::Func(..)) {
            let span = self.expr_span(e);
            self.unsupported(span, "this function value");
            return ArrowInfo { cname: "NULL".into(), env_type: String::new(), captures: Vec::new() };
        }
        let cname = self.fresh("arrow");
        let me = self.self_closure.take();
        let mut captures_keys = arrow_captures(ast, self.facts(m), e);
        captures_keys.retain(|&k| Some(k) != me);
        let captures = self.capture_list(captures_keys, stack);
        let env_type = format!("E{}", &cname[5..]);
        self.define_env(&env_type, &captures, stack);
        // (the closure calling itself: its own function and environment)
        let siblings: Vec<(u32, String, TyId)> = me.map(|k| (k, cname.clone(), fty)).into_iter().collect();
        self.emit_arrow_fn(e, fty, &cname, &env_type, &captures, stack, &siblings);
        ArrowInfo { cname, env_type, captures }
    }

    /// The captured locals among `keys`: (key, type, boxed).
    fn capture_list(&mut self, keys: Vec<u32>, stack: bool) -> Vec<(u32, TyId, bool)> {
        let boxed = self.b().boxed.clone();
        let mut captures = Vec::new();
        for k in keys {
            if let Some(l) = self.b().locals.get(&k).cloned() {
                captures.push((k, l.ty, boxed.contains(&k) && !stack));
            }
        }
        captures
    }

    /// The environment struct holding `captures` (and its drop function).
    fn define_env(&mut self, env_type: &str, captures: &[(u32, TyId, bool)], stack: bool) {
        if captures.is_empty() {
            return;
        }
        let mut fields = String::new();
        for (j, (_, ty, bx)) in captures.iter().enumerate() {
            let ct = self.ctype(*ty);
            if stack {
                let _ = write!(fields, " {ct} *c{j};");
            } else if *bx {
                let cell = self.cell_type(*ty);
                let _ = write!(fields, " {cell} *c{j};");
            } else {
                let _ = write!(fields, " {ct} c{j};");
            }
        }
        // Environments hold captured values by value: defined after every value struct.
        let _ = writeln!(self.typedefs, "typedef struct {env_type} {env_type};");
        let _ = writeln!(self.env_structs, "struct {env_type} {{ tv_env h;{fields} }};");
        if !stack {
            let mut drops = String::new();
            for (j, (_, ty, bx)) in captures.iter().enumerate() {
                if *bx {
                    let _ = write!(drops, " {}", self.cell_release(*ty, &format!("x->c{j}")));
                } else if self.is_rc(*ty) {
                    let _ = write!(drops, " {};", self.release_code(*ty, &format!("x->c{j}")));
                }
            }
            let _ = writeln!(self.protos, "static void drop_{env_type}(tv_env *e);");
            let _ = writeln!(self.helpers, "static void drop_{env_type}(tv_env *e) {{ {env_type} *x = ({env_type} *)e; (void)x;{drops} }}");
        }
    }

    /// Emits arrow `e`'s C function `cname`, its captured locals mapped into the environment;
    /// `siblings` (key, function, type) are closures sharing it (the arrow itself, when it calls
    /// itself, and the functions beside it that call each other).
    #[allow(clippy::too_many_arguments)]
    fn emit_arrow_fn(&mut self, e: ExprId, fty: TyId, cname: &str, env_type: &str, captures: &[(u32, TyId, bool)], stack: bool, siblings: &[(u32, String, TyId)]) {
        let m = self.cur_m();
        let ast = self.ast(m);
        let ExprKind::Arrow(f) = &ast.expr(e).kind else { unreachable!() };
        let Ty::Func(ps, ret, _) = self.tget(fty) else { unreachable!() };
        let ps = self.c.types.params(ps).to_vec();
        let subst = self.b().subst.clone();
        let mut params: Vec<(u32, TyId, bool)> = f.params.iter().zip(&ps).map(|(p, fp)| (p.span.start, fp.ty, fp.inout)).collect();
        // (`(...args: A) => ...` with `A` a tuple: one C parameter per element, `args` their tuple)
        let mut rest_tuple = None;
        if let Some(last) = f.params.last()
            && last.rest
            && !matches!(ps.get(f.params.len() - 1).map(|p| self.tget(p.ty)), Some(Ty::Array(_)))
            && (ps.len() != f.params.len() || ps.last().is_some_and(|p| !matches!(self.tget(p.ty), Ty::Array(_))))
        {
            let k = f.params.len() - 1;
            params.truncate(k);
            let elems: Vec<TyId> = ps[k..].iter().map(|p| p.ty).collect();
            for (j, fp) in ps[k..].iter().enumerate() {
                params.push((u32::MAX - j as u32, fp.ty, false));
            }
            rest_tuple = Some((last.span.start, k, elems));
        }
        let proto = self.fn_proto(cname, &ps, ret, &FxMap::default(), true);
        let _ = writeln!(self.protos, "static {proto};");
        let cast = self.fn_ptr_type_pub(&ps, ret);
        self.arrow_last.insert((m, e), (cname.to_string(), cast.clone()));
        if !stack {
            self.arrow_fns.entry((m, e)).or_default().push((cname.to_string(), cast));
            self.arrow_protos.insert(cname.to_string(), proto.clone());
        }
        let kind = match &f.body {
            ArrowBody::Block(b) => FnBodyKind::Block(*b),
            ArrowBody::Expr(x) => FnBodyKind::Expr(*x),
        };
        let mut env_locals = FxMap::default();
        for (j, (k, ty, bx)) in captures.iter().enumerate() {
            let access = if stack {
                format!("(*(({env_type} *)env_)->c{j})")
            } else if *bx {
                format!("(({env_type} *)env_)->c{j}->v")
            } else {
                format!("(({env_type} *)env_)->c{j}")
            };
            env_locals.insert(*k, Local { access, ty: *ty });
        }
        for (k, sib, sty) in siblings {
            env_locals.insert(*k, Local { access: format!("((tv_fn){{ (void *){sib}, env_ }})"), ty: *sty });
        }
        if let Some((key, k, elems)) = rest_tuple {
            let tt = self.c.types.tuple(&elems);
            let syms = self.c.types.tuple_syms().to_vec();
            let parts: Vec<String> = (0..elems.len()).map(|j| format!(".f_{} = p{}", self.sym(syms[j]), k + j)).collect();
            let access = self.rec_lit(tt, &parts.join(", "));
            env_locals.insert(key, Local { access, ty: tt });
        }
        if f.is_async {
            self.emit_async_arrow(cname, m, subst, &params, ret, kind, env_locals);
            return;
        }
        // (a captured local that holds one arrow's function: called directly in here too)
        let direct: FxMap<u32, String> = captures.iter().filter_map(|(k, _, _)| self.b().direct_fns.get(k).map(|c| (*k, c.clone()))).collect();
        if !direct.is_empty() {
            self.pending_direct = Some(direct);
        }
        let code = self.arrow_body(m, subst, &params, ret, kind, env_locals);
        let _ = writeln!(self.funcs, "static {proto} {{\n    (void)env_;\n{code}}}\n");
    }

    /// Named local functions that call each other (`check::stmt::closure_groups`): one
    /// environment for all, holding what any of them captures; each is (its function, it).
    fn closure_group(&mut self, group: &[StmtId]) {
        let m = self.cur_m();
        let ast = self.ast(m);
        let mut members = Vec::new();
        for &st in group {
            let StmtKind::Let { name, name_span, init: Some(i), .. } = ast.stmt(st).kind else { return };
            let ty = self.facts(m).bindings.get(&st).copied().unwrap_or(ERROR);
            let ty = self.inst(ty);
            members.push((name, name_span.start, i, ty, self.fresh("arrow")));
        }
        let keys: Vec<u32> = members.iter().map(|x| x.1).collect();
        let mut cap_keys: Vec<u32> = Vec::new();
        for x in &members {
            for k in arrow_captures(ast, self.facts(m), x.2) {
                if !keys.contains(&k) && !cap_keys.contains(&k) {
                    cap_keys.push(k);
                }
            }
        }
        let captures = self.capture_list(cap_keys, false);
        let env_type = format!("E{}", &members[0].4[5..]);
        self.define_env(&env_type, &captures, false);
        let siblings: Vec<(u32, String, TyId)> = members.iter().map(|x| (x.1, x.4.clone(), x.3)).collect();
        for x in &members {
            self.emit_arrow_fn(x.2, x.3, &x.4, &env_type, &captures, false, &siblings);
        }
        self.push_temps();
        let env = if captures.is_empty() {
            "NULL".to_string()
        } else {
            let env = self.fresh("env");
            self.line(format!("{env_type} *{env} = tvg_alloc_small(sizeof({env_type})); {env}->h.rc = {}; {env}->h.size = TVG_ENV_SIZE(sizeof({env_type})); {env}->h.drop = drop_{env_type};", members.len()));
            for (j, (key, ty, boxed)) in captures.iter().enumerate() {
                let Some(local) = self.b().locals.get(key).cloned() else { continue };
                if *boxed {
                    let cell = local.access.trim_end_matches("->v").to_string();
                    self.line(format!("{env}->c{j} = {cell}; {cell}->rc++;"));
                } else {
                    let v = Val::plain(local.access.clone(), *ty);
                    let code = self.consume(v);
                    self.line(format!("{env}->c{j} = {code};"));
                }
            }
            format!("(tv_env *){env}")
        };
        self.pop_temps();
        for (name, key, _, ty, cname) in members {
            let uniq = self.fresh("");
            let v = format!("v_{}_{}", self.sym(name), uniq);
            let ct = self.ctype(ty);
            self.line(format!("{ct} {v} = ((tv_fn){{ (void *){cname}, {env} }});"));
            if self.is_rc(ty) {
                let rel = format!("{};", self.release_code(ty, &v));
                self.b().scopes.last_mut().unwrap().releases.push(rel);
            }
            self.b().locals.insert(key, Local { access: v, ty });
        }
    }

    fn arrow_body(&mut self, m: u32, subst: FxMap<u32, TyId>, params: &[(u32, TyId, bool)], ret: TyId, kind: FnBodyKind, env_locals: FxMap<u32, Local>) -> String {
        // Captured variables are visible as locals; the enclosing function's boxing decisions carry over.
        let outer_boxed = self.b().boxed.clone();
        let outer_stack = self.b().stack_arrows.clone();
        let (inner_boxed, inner_stack, inner_mutated) = self.analyze_closures(m, &kind);
        let mut body = Body {
            m,
            subst,
            out: String::new(),
            indent: 1,
            locals: env_locals,
            scopes: vec![Scope { releases: Vec::new() }],
            temps: vec![Vec::new()],
            ret,
            loops: Vec::new(),
            labeled: Vec::new(),
            iter_release: None,
            map_loops: Vec::new(),
            breaks: Vec::new(),
            boxed: outer_boxed.union(&inner_boxed).copied().collect(),
            stack_arrows: outer_stack.union(&inner_stack).copied().collect(),
            unique: FxSet::default(),
            mutated: inner_mutated,
            params: params.iter().filter(|p| !p.2).map(|p| p.0).collect(),
            ctor_class: None,
            owned_once: FxSet::default(),
            handlers: Vec::new(),
            async_fn: self.pending_async.take(),
            gen_elem: self.pending_gen.take(),
            children: Vec::new(),
            finallies: Vec::new(),
            tre: None,
            in_bounds: Vec::new(),
            push_caps: FxMap::default(),
            arr_cache: FxMap::default(),
            exact_pushes: FxSet::default(),
            nowrap_step: None,
            divs: FxMap::default(),
            versioned: Vec::new(),
            rows: Vec::new(),
            nowrap_locals: Vec::new(),
            root: None,
            arrow_root: None,
            cursors: FxSet::default(),
            take: None,
            take_slot: None,
            take_let: false,
            take_assign: None,
            take_failed: false,
            no_release: None,
            owned_locals: FxSet::default(),
            moves: None,
            fixed: None,
            direct_fns: self.pending_direct.take().unwrap_or_default(),
            carry: None,
            builders: FxMap::default(),
        };
        for (i, &(key, ty, inout)) in params.iter().enumerate() {
            let access = if inout { format!("(*p{i})") } else { format!("p{i}") };
            body.locals.insert(key, Local { access, ty });
        }
        if let FnBodyKind::Block(s) = &kind {
            body.arrow_root = Some(*s);
        }
        self.bodies.push(body);
        // A parameter the body reassigns is owned from the start (borrowed, it can't be changed in
        // place or released when replaced), as a function's is.
        let reassigned = self.reassigned_locals(&kind);
        for (i, &(key, ty, inout)) in params.iter().enumerate() {
            if !inout && !self.b().boxed.contains(&key) && reassigned.contains(&key) && self.is_rc(ty) && self.b().locals.get(&key).is_some_and(|l| l.access == format!("p{i}")) {
                let r = self.retain_code(ty, &format!("p{i}"));
                self.line(format!("{r};"));
                let rel = format!("{};", self.release_code(ty, &format!("p{i}")));
                self.b().scopes[0].releases.push(rel);
                self.b().params.remove(&key);
            }
        }
        match kind {
            FnBodyKind::Block(s) => {
                self.stmt_list_of(s);
                self.unwind_to(0);
                if ret != VOID && !self.always_returns_stmt(m, s) {
                    let v = self.coerce(Val::plain("0", UNDEFINED), ret);
                    let code = self.consume(v);
                    self.line(format!("return {code};"));
                }
            }
            FnBodyKind::Expr(e) => {
                let v = self.expr(e);
                if ret == VOID {
                    self.pop_temps();
                    self.b().temps.push(Vec::new());
                    self.unwind_to(0);
                } else {
                    let v = self.coerce(v, ret);
                    let code = self.consume(v);
                    let ct = self.ctype(ret);
                    self.line(format!("{ct} r_ = {code};"));
                    self.release_all_temps();
                    self.unwind_to(0);
                    self.line("return r_;");
                }
            }
            FnBodyKind::Init => {}
        }
        let b = self.bodies.pop().unwrap();
        if b.async_fn.is_some() {
            self.last_children = b.children;
        }
        b.out
    }

    pub(crate) fn arrow_body_pub(&mut self, m: u32, subst: FxMap<u32, TyId>, params: &[(u32, TyId, bool)], ret: TyId, kind: FnBodyKind, env_locals: FxMap<u32, Local>) -> String {
        self.arrow_body(m, subst, params, ret, kind, env_locals)
    }
}

pub(crate) struct Place {
    pub lv: String,
    pub ty: TyId,
    /// A weak class field (weak counts instead of strong ones).
    pub weak: bool,
}

pub(crate) struct ArrowInfo {
    pub cname: String,
    pub env_type: String,
    /// (declaration key, type, boxed)
    pub captures: Vec<(u32, TyId, bool)>,
}

/// A C expression with no side effects worth keeping (a name, a literal, a field path).
fn is_trivial(code: &str) -> bool {
    code.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '(' | ')' | '*' | '-' | '>' | ' '))
        && !code.contains("->") && !code.contains('(')
        || code == "0"
}

/// A C expression that reads (a name, a field path, a dereference) and calls nothing: safe to
/// write out twice.
fn is_simple_code(code: &str) -> bool {
    let b = code.as_bytes();
    code.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '(' | ')' | '*' | '-' | '>' | ' '))
        && !(1..b.len()).any(|i| b[i] == b'(' && (b[i - 1].is_ascii_alphanumeric() || b[i - 1] == b'_'))
}

pub(crate) fn is_mutating_method(name: &str) -> bool {
    matches!(name, "push" | "pop" | "shift" | "unshift" | "splice" | "reverse" | "sort" | "fill" | "set" | "delete" | "clear" | "add")
}

/// Locals referenced inside an arrow (including nested arrows) but declared outside it.
pub(crate) fn arrow_captures(ast: &ast::Ast, facts: &crate::check::ModuleFacts, arrow: ExprId) -> Vec<u32> {
    let span = ast.expr(arrow).span;
    let mut exprs = Vec::new();
    collect_exprs_expr(ast, arrow, &mut exprs);
    let mut out: Vec<u32> = Vec::new();
    for e in exprs {
        if let ExprKind::Ident(_) | ExprKind::This = ast.expr(e).kind
            && let Some(IdentFact::Local(k)) = facts.idents.get(&e)
            && (*k < span.start || *k >= span.end)
            && !out.contains(k)
        {
            out.push(*k);
        }
    }
    out
}

pub(crate) fn collect_exprs_stmt_pub(ast: &ast::Ast, s: StmtId, out: &mut Vec<ExprId>) {
    collect_exprs_stmt(ast, s, out)
}

/// Locals declared in `s` (outside closures) that may borrow from the locals their initializer
/// mentions: a `const` initialized from a field/index path (see the borrowed views in `stmt`).
/// Every other local owns its value. (A `for-of` variable lives only inside its statement.)
fn collect_bindings(ast: &ast::Ast, s: StmtId, out: &mut Vec<(u32, ExprId)>) {
    fn path_like(ast: &ast::Ast, e: ExprId) -> bool {
        match &ast.expr(e).kind {
            ExprKind::Ident(_) | ExprKind::This => true,
            ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::As(x, _) | ExprKind::Member { obj: x, .. } | ExprKind::Index { obj: x, .. } => path_like(ast, *x),
            _ => false,
        }
    }
    match &ast.stmt(s).kind {
        StmtKind::Let { mutable: false, name_span, init: Some(e), .. } if path_like(ast, *e) => out.push((name_span.start, *e)),
        StmtKind::ForOf { body, .. } => collect_bindings(ast, *body, out),
        StmtKind::If(_, t, e) => {
            collect_bindings(ast, *t, out);
            if let Some(e) = e {
                collect_bindings(ast, *e, out);
            }
        }
        StmtKind::While(_, b) | StmtKind::DoWhile(b, _) => collect_bindings(ast, *b, out),
        StmtKind::For { init, body, .. } => {
            if let Some(i) = init {
                collect_bindings(ast, *i, out);
            }
            collect_bindings(ast, *body, out);
        }
        StmtKind::Switch(_, cases) => cases.iter().flat_map(|c| c.body.iter()).for_each(|&x| collect_bindings(ast, x, out)),
        StmtKind::Block(ss) => ss.iter().for_each(|&x| collect_bindings(ast, x, out)),
        StmtKind::Try { body, catch, finally } => {
            collect_bindings(ast, *body, out);
            if let Some(c) = catch {
                collect_bindings(ast, c.body, out);
            }
            if let Some(f) = finally {
                collect_bindings(ast, *f, out);
            }
        }
        _ => {}
    }
}

pub(crate) fn collect_exprs_stmt(ast: &ast::Ast, s: StmtId, out: &mut Vec<ExprId>) {
    match &ast.stmt(s).kind {
        StmtKind::Expr(e) => collect_exprs_expr(ast, *e, out),
        StmtKind::Let { init: Some(e), .. } => collect_exprs_expr(ast, *e, out),
        StmtKind::If(c, t, e) => {
            collect_exprs_expr(ast, *c, out);
            collect_exprs_stmt(ast, *t, out);
            if let Some(e) = e {
                collect_exprs_stmt(ast, *e, out);
            }
        }
        StmtKind::While(c, b) | StmtKind::DoWhile(b, c) => {
            collect_exprs_expr(ast, *c, out);
            collect_exprs_stmt(ast, *b, out);
        }
        StmtKind::For { init, cond, step, body } => {
            if let Some(i) = init {
                collect_exprs_stmt(ast, *i, out);
            }
            for e in [cond, step].into_iter().flatten() {
                collect_exprs_expr(ast, *e, out);
            }
            collect_exprs_stmt(ast, *body, out);
        }
        StmtKind::ForOf { iter, body, .. } => {
            collect_exprs_expr(ast, *iter, out);
            collect_exprs_stmt(ast, *body, out);
        }
        StmtKind::Switch(d, cases) => {
            collect_exprs_expr(ast, *d, out);
            for c in cases {
                if let Some(t) = c.test {
                    collect_exprs_expr(ast, t, out);
                }
                for &x in &c.body {
                    collect_exprs_stmt(ast, x, out);
                }
            }
        }
        StmtKind::Return(Some(e)) | StmtKind::Throw(e) => collect_exprs_expr(ast, *e, out),
        StmtKind::Block(ss) => ss.iter().for_each(|&x| collect_exprs_stmt(ast, x, out)),
        StmtKind::Try { body, catch, finally } => {
            collect_exprs_stmt(ast, *body, out);
            if let Some(c) = catch {
                collect_exprs_stmt(ast, c.body, out);
            }
            if let Some(f) = finally {
                collect_exprs_stmt(ast, *f, out);
            }
        }
        _ => {}
    }
}

/// A `continue` in `s` (outside closures), for the loop `s` is the body of or one inside it.
fn stmt_has_continue(ast: &ast::Ast, s: StmtId) -> bool {
    match &ast.stmt(s).kind {
        StmtKind::Continue(_) => true,
        StmtKind::For { body: b, .. } | StmtKind::ForOf { body: b, .. } | StmtKind::While(_, b) | StmtKind::DoWhile(b, _) => stmt_has_continue(ast, *b),
        StmtKind::If(_, t, e) => stmt_has_continue(ast, *t) || e.is_some_and(|e| stmt_has_continue(ast, e)),
        StmtKind::Block(xs) => xs.iter().any(|&x| stmt_has_continue(ast, x)),
        StmtKind::Switch(_, cases) => cases.iter().any(|c| c.body.iter().any(|&x| stmt_has_continue(ast, x))),
        StmtKind::Try { body, catch, finally } => stmt_has_continue(ast, *body) || catch.as_ref().is_some_and(|c| stmt_has_continue(ast, c.body)) || finally.is_some_and(|f| stmt_has_continue(ast, f)),
        _ => false,
    }
}

/// A loop statement in `s` (outside closures).
fn stmt_has_loop(ast: &ast::Ast, s: StmtId) -> bool {
    match &ast.stmt(s).kind {
        StmtKind::For { .. } | StmtKind::ForOf { .. } | StmtKind::While(..) | StmtKind::DoWhile(..) => true,
        StmtKind::If(_, t, e) => stmt_has_loop(ast, *t) || e.is_some_and(|e| stmt_has_loop(ast, e)),
        StmtKind::Block(xs) => xs.iter().any(|&x| stmt_has_loop(ast, x)),
        StmtKind::Switch(_, cases) => cases.iter().any(|c| c.body.iter().any(|&x| stmt_has_loop(ast, x))),
        StmtKind::Try { body, catch, finally } => stmt_has_loop(ast, *body) || catch.as_ref().is_some_and(|c| stmt_has_loop(ast, c.body)) || finally.is_some_and(|f| stmt_has_loop(ast, f)),
        _ => false,
    }
}

pub(crate) fn collect_exprs_expr(ast: &ast::Ast, e: ExprId, out: &mut Vec<ExprId>) {
    out.push(e);
    match &ast.expr(e).kind {
        ExprKind::Unary(_, x) | ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Typeof(x) | ExprKind::Spread(x) | ExprKind::As(x, _) | ExprKind::Try(x) | ExprKind::Await(x) | ExprKind::Yield(Some(x), _) => collect_exprs_expr(ast, *x, out),
        ExprKind::Binary(_, l, r) | ExprKind::Assign(_, l, r) => {
            collect_exprs_expr(ast, *l, out);
            collect_exprs_expr(ast, *r, out);
        }
        ExprKind::Update { target, .. } => collect_exprs_expr(ast, *target, out),
        ExprKind::Call { callee, args, .. } | ExprKind::New { callee, args, .. } => {
            collect_exprs_expr(ast, *callee, out);
            for a in args {
                collect_exprs_expr(ast, a.expr, out);
            }
        }
        ExprKind::Member { obj, .. } => collect_exprs_expr(ast, *obj, out),
        ExprKind::Index { obj, index, .. } => {
            collect_exprs_expr(ast, *obj, out);
            collect_exprs_expr(ast, *index, out);
        }
        ExprKind::Cond(a, b, c) => {
            for x in [a, b, c] {
                collect_exprs_expr(ast, *x, out);
            }
        }
        ExprKind::Array(xs) | ExprKind::Template(_, xs) | ExprKind::Seq(xs) => xs.iter().for_each(|&x| collect_exprs_expr(ast, x, out)),
        ExprKind::Object(fs) => fs.iter().for_each(|f| collect_exprs_expr(ast, f.value, out)),
        ExprKind::Arrow(f) => match &f.body {
            ArrowBody::Expr(x) => collect_exprs_expr(ast, *x, out),
            ArrowBody::Block(b) => collect_exprs_stmt(ast, *b, out),
        },
        _ => {}
    }
}

impl Gen<'_, '_> {
    /// A tuple type's elements (`[A, B]`: a record with fields `0`, `1`), in order.
    pub(crate) fn tuple_of(&self, t: TyId) -> Option<Vec<crate::types::Field>> {
        let Ty::Record(fs) = *self.c.types.get(t) else { return None };
        crate::types::tuple_fields(self.c.types.fields(fs), self.c.interner)
    }
}

/// Does `e` multiply (outside nested functions)? (A sum with a product in it is rounded as
/// JavaScript's in TypeScript: see TV_JADD.)
pub(crate) fn has_mul(ast: &ast::Ast, e: ExprId) -> bool {
    match &ast.expr(e).kind {
        ExprKind::Binary(BinOp::Mul | BinOp::Pow, ..) | ExprKind::Assign(AssignOp::Op(BinOp::Mul | BinOp::Pow), ..) => true,
        ExprKind::Binary(_, a, b) => has_mul(ast, *a) || has_mul(ast, *b),
        ExprKind::Paren(x) | ExprKind::Unary(_, x) | ExprKind::NonNull(x) | ExprKind::As(x, _) => has_mul(ast, *x),
        ExprKind::Cond(_, a, b) => has_mul(ast, *a) || has_mul(ast, *b),
        _ => false,
    }
}

/// `s` always ends by throwing (a `throw`, or a block that ends in one).
fn throws_out(ast: &ast::Ast, s: StmtId) -> bool {
    match &ast.stmt(s).kind {
        StmtKind::Throw(_) => true,
        StmtKind::Block(ss) => ss.last().is_some_and(|&l| throws_out(ast, l)),
        StmtKind::If(_, t, Some(e)) => throws_out(ast, *t) && throws_out(ast, *e),
        _ => false,
    }
}

/// The local a place is rooted in (`x`, `x.f`, `x[i]`, `x!`), if it's a local.
fn local_root(ast: &ast::Ast, facts: &crate::check::ModuleFacts, mut e: ExprId) -> Option<u32> {
    loop {
        match &ast.expr(e).kind {
            ExprKind::Paren(x) | ExprKind::NonNull(x) => e = *x,
            ExprKind::Member { obj, .. } | ExprKind::Index { obj, .. } => e = *obj,
            ExprKind::Ident(_) => {
                return match facts.idents.get(&e).copied() {
                    Some(IdentFact::Local(k)) => Some(k),
                    _ => None,
                };
            }
            _ => return None,
        }
    }
}
