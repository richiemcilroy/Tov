use super::*;
use crate::ast::{ArrowBody, AssignOp, Case, ExprKind, StmtKind};

enum SwitchMode {
    /// `switch (x.kind)` over a discriminated union: variants are (literal, member type).
    Discriminated { local: LocalId, variants: Vec<(Sym, TyId)> },
    /// `switch (x)` where `x` is a union of string literals.
    Literal { local: Option<LocalId>, lits: Vec<Sym> },
    General,
}

impl<'a> Checker<'a> {
    pub(super) fn stmt(&mut self, s: StmtId) {
        let ast = self.ast();
        let node = ast.stmt(s);
        let span = node.span;
        match &node.kind {
            StmtKind::Empty | StmtKind::Error => {}
            StmtKind::InitGlobal(ii) => {
                let m = self.cur;
                let t = self.const_type(m, *ii);
                // A top-level `const`/`let` lives in module scope, which `lookup` can't see —
                // give the script a marked local too, or `if (x !== undefined)` can't narrow
                // it (and the effects machinery can't track it). `Local::global` keeps its
                // reads and writes resolving as the global, so codegen is unaffected.
                if let ItemKind::Const { name, mutable, name_span, .. } = &self.modules[m as usize].ast.items[*ii as usize].kind {
                    let (name, mutable, name_span) = (*name, *mutable, *name_span);
                    let kind = if mutable { LocalKind::Let } else { LocalKind::Const };
                    let id = self.declare(name, t, kind, name_span, None, false);
                    self.fcx().locals[id].global = Some((m, *ii));
                }
                if self.const_throws.contains(&(m, *ii)) {
                    let err = self.error_class();
                    self.cur_frame().thrown.push(err);
                }
            }
            StmtKind::Expr(e) => {
                self.expr(*e, None);
            }
            StmtKind::Let { mutable, name, name_span, ty, init } => {
                let kw = Span::new(span.file, span.start, span.start + if *mutable { 3 } else { 5 });
                let kind = if *mutable { LocalKind::Let } else { LocalKind::Const };
                let tscope = self.tscope();
                let (t, promotable) = match (ty, init) {
                    (Some(te), init) => {
                        let declared = self.resolve_type(*te, &tscope);
                        if let Some(i) = init {
                            let found = self.expr(*i, Some(declared));
                            let ispan = self.ast().expr(*i).span;
                            self.expect_assignable(found, declared, ispan, None);
                        } else if !*mutable {
                            self.report(Diagnostic::new("T0309", span, "a `const` needs a value"));
                        }
                        (declared, false)
                    }
                    (None, Some(i)) => {
                        let found = self.expr(*i, None);
                        let ispan = self.ast().expr(*i).span;
                        let t = self.check_inferred_binding(found, ispan);
                        let t = if *mutable { self.types.widen(t) } else { t };
                        let promotable = *mutable && t == INT && self.is_int_literal(*i);
                        if promotable && self.fcx.last().unwrap().promoted.contains(&name_span.start) {
                            (F64, false)
                        } else {
                            (t, promotable)
                        }
                    }
                    (None, None) => {
                        let n = self.name(*name).to_string();
                        self.report(
                            Diagnostic::new("T0301", *name_span, format!("`{n}` needs a type annotation or an initial value"))
                                .fix(Applicability::Placeholder, format!("annotate `{n}: T`"), name_span.empty_at_end(), ": T"),
                        );
                        (ERROR, false)
                    }
                };
                self.rec_binding(s, t);
                let id = self.declare(*name, t, kind, *name_span, if *mutable { None } else { Some(kw) }, promotable);
                // `let b = a` of a value TypeScript would share: remember for the copy-then-mutate check.
                if let Some(i) = init
                    && let crate::ast::ExprKind::Ident(src) = self.ast().expr(*i).kind
                    && let Some((src_id, _)) = self.lookup(src)
                    && src_id != id
                    && self.shared_in_ts(t)
                {
                    let pos = self.ast().expr(*i).span.end;
                    self.fcx().aliases.push((id, src_id, pos));
                }
            }
            StmtKind::Block(stmts) => {
                let fx = if self.always_jumps(s) { Effects::default() } else { effects_of_stmts(ast, stmts) };
                let entry = self.fcx().locals.len();
                self.push_scope();
                let mut reported_unreachable = false;
                for (i, &st) in stmts.iter().enumerate() {
                    if i > 0 && !reported_unreachable && self.always_jumps(stmts[i - 1]) {
                        let sp = self.ast().stmt(st).span;
                        self.report(Diagnostic::new("F0102", sp, "unreachable code").note("why", "the previous statement always returns, breaks or continues"));
                        reported_unreachable = true;
                    }
                    self.stmt(st);
                }
                let ends = self.end_types(&fx.assigned, entry);
                self.pop_scope();
                self.apply_effects(&fx, entry, &ends);
            }
            StmtKind::If(c, then, els) => {
                self.cond(*c);
                let pos = self.narrowings(*c, true);
                let neg = self.narrowings(*c, false);
                let then_jumps = self.always_jumps(*then);
                let else_jumps = els.map(|e| self.always_jumps(e)).unwrap_or(false);
                // Effects of the branches that fall through reach the code after the `if`.
                let mut fx = Effects::default();
                if !then_jumps {
                    fx.extend(effects_of_stmt(ast, *then));
                }
                if let Some(e) = els
                    && !else_jumps
                {
                    fx.extend(effects_of_stmt(ast, *e));
                }
                let entry = self.fcx().locals.len();
                let mut branch_ends: Vec<Vec<(LocalId, TyId)>> = Vec::new();
                self.push_scope();
                self.apply(&pos);
                self.stmt(*then);
                if !then_jumps {
                    branch_ends.push(self.end_types(&fx.assigned, entry));
                }
                self.pop_scope();
                self.push_scope();
                self.apply(&neg);
                if let Some(e) = els {
                    self.stmt(*e);
                }
                if !else_jumps {
                    branch_ends.push(self.end_types(&fx.assigned, entry));
                }
                self.pop_scope();
                // Early exits narrow the rest of the enclosing block.
                if then_jumps && !else_jumps {
                    self.apply(&neg);
                } else if else_jumps && !then_jumps {
                    self.apply(&pos);
                }
                // Join: a local assigned in a branch has the union of its types at the branch ends.
                let mut joined: Vec<(LocalId, TyId)> = Vec::new();
                for ends in &branch_ends {
                    for &(id, t) in ends {
                        match joined.iter_mut().find(|(j, _)| *j == id) {
                            Some(entry) => entry.1 = self.types.union(&[entry.1, t]),
                            None => joined.push((id, t)),
                        }
                    }
                }
                self.apply_effects(&fx, entry, &joined);
            }
            StmtKind::While(c, body) => {
                let fx = effects_of_stmt(ast, *body).with(effects_of_expr(ast, *c));
                let entry = self.fcx().locals.len();
                self.push_scope();
                self.reset_assigned(*body);
                self.cond(*c);
                let pos = self.narrowings(*c, true);
                self.apply(&pos);
                self.loop_body(*body);
                self.pop_scope();
                self.apply_effects(&fx, entry, &[]);
            }
            StmtKind::DoWhile(body, c) => {
                let fx = effects_of_stmt(ast, *body).with(effects_of_expr(ast, *c));
                let entry = self.fcx().locals.len();
                self.push_scope();
                self.reset_assigned(*body);
                self.loop_body(*body);
                self.cond(*c);
                self.pop_scope();
                self.apply_effects(&fx, entry, &[]);
            }
            StmtKind::For { init, cond, step, body } => {
                let mut fx = effects_of_stmt(ast, *body);
                for e in [cond, step].into_iter().flatten() {
                    fx.extend(effects_of_expr(ast, *e));
                }
                let entry = self.fcx().locals.len();
                self.push_scope();
                if let Some(i) = init {
                    self.stmt(*i);
                }
                self.reset_assigned(*body);
                if let Some(st) = step {
                    self.reset_assigned_expr(*st);
                }
                if let Some(c) = cond {
                    self.cond(*c);
                    let pos = self.narrowings(*c, true);
                    self.apply(&pos);
                }
                self.loop_body(*body);
                if let Some(st) = step {
                    self.expr(*st, None);
                }
                self.pop_scope();
                self.apply_effects(&fx, entry, &[]);
            }
            StmtKind::ForOf { mutable, name, name_span, iter, body } => {
                let t = self.expr(*iter, None);
                let ispan = self.ast().expr(*iter).span;
                let elem = match *self.types.get(t) {
                    Ty::Array(e) | Ty::Set(e) => e,
                    Ty::Error => ERROR,
                    Ty::Str | Ty::StrLit(_) => {
                        let text = self.src(ispan).to_string();
                        self.report(
                            Diagnostic::new("X0037", ispan, "iterate a string's characters explicitly")
                                .fix(Applicability::Safe, format!("use `{text}.chars()`"), ispan, format!("{text}.chars()")),
                        );
                        STR
                    }
                    Ty::Map(..) => {
                        let text = self.src(ispan).to_string();
                        self.report(
                            Diagnostic::new("X0038", ispan, "iterate a map through `keys()` or `values()`")
                                .fix(Applicability::Maybe, format!("iterate keys: `{text}.keys()`"), ispan, format!("{text}.keys()"))
                                .fix(Applicability::Maybe, format!("iterate values: `{text}.values()`"), ispan, format!("{text}.values()")),
                        );
                        ERROR
                    }
                    // a JavaScript iterable (an array, a Set, a generator...)
                    Ty::Js => JS,
                    _ => {
                        let msg = format!("`for...of` needs an array or set, found `{}`", self.show(t));
                        self.report(Diagnostic::new("T0402", ispan, msg));
                        ERROR
                    }
                };
                let fx = effects_of_stmt(ast, *body);
                let entry = self.fcx().locals.len();
                self.push_scope();
                let kind = if *mutable { LocalKind::Let } else { LocalKind::LoopVar };
                self.rec_binding(s, elem);
                self.declare(*name, elem, kind, *name_span, None, false);
                self.reset_assigned(*body);
                self.loop_body(*body);
                self.pop_scope();
                self.apply_effects(&fx, entry, &[]);
            }
            StmtKind::Switch(disc, cases) => {
                let mut fx = Effects::default();
                for c in cases {
                    fx.extend(effects_of_stmts(ast, &c.body));
                }
                let entry = self.fcx().locals.len();
                self.switch(s, *disc, cases, span);
                self.apply_effects(&fx, entry, &[]);
            }
            StmtKind::Return(v) => {
                let frame_idx = self.fcx.last().unwrap().frames.len() - 1;
                let (ret, fname) = {
                    let f = &self.fcx.last().unwrap().frames[frame_idx];
                    (f.ret, f.name)
                };
                let who = fname.map(|n| format!("`{}`", self.name(n))).unwrap_or_else(|| "this function".to_string());
                match (ret, v) {
                    (Some(r), Some(x)) => {
                        let t = self.expr(*x, Some(r));
                        let xspan = self.ast().expr(*x).span;
                        if r == VOID {
                            if t != VOID && t != UNDEFINED && t != ERROR {
                                self.report(
                                    Diagnostic::new("T0001", xspan, format!("{who} returns `void`, so `return` takes no value"))
                                        .note("found", format!("`{}`", self.show(t))),
                                );
                            }
                        } else {
                            self.expect_assignable(t, r, xspan, Some(format!("return type of {who}")));
                        }
                    }
                    (Some(r), None) => {
                        if r != VOID && r != ERROR && !self.types.has_undefined(r) {
                            self.report(Diagnostic::new("T0001", span, format!("{who} must return a value of type `{}`", self.show(r))));
                        }
                    }
                    (None, Some(x)) => {
                        let t = self.expr(*x, None);
                        self.fcx().frames[frame_idx].returns.push(t);
                    }
                    (None, None) => {
                        self.fcx().frames[frame_idx].returns.push(UNDEFINED);
                    }
                }
            }
            StmtKind::Throw(x) => {
                let t = self.expr(*x, None);
                let err = self.error_class();
                if t != ERROR && err != ERROR && !self.assignable(t, err) {
                    let shown = self.show(t);
                    let xs = self.ast().expr(*x).span;
                    let text = self.src(xs).to_string();
                    let repl = if self.types.is_string(t) { format!("new Error({text})") } else { format!("new Error(String({text}))") };
                    self.report(
                        Diagnostic::new("T0834", xs, format!("can only throw `Error` objects, found `{shown}`"))
                            .fix(Applicability::Maybe, format!("throw an Error: `{repl}`"), xs, repl.clone()),
                    );
                } else if t != ERROR {
                    let t = self.types.without_undefined(t);
                    self.on_throw(t, span, None, None);
                }
            }
            StmtKind::Try { body, catch, finally } => {
                let mut fx = effects_of_stmt(ast, *body);
                if let Some(c) = catch {
                    fx.extend(effects_of_stmt(ast, c.body));
                }
                if let Some(f) = finally {
                    fx.extend(effects_of_stmt(ast, *f));
                }
                let entry = self.fcx().locals.len();
                if catch.is_some() {
                    self.cur_frame().try_frames.push(Vec::new());
                }
                self.stmt(*body);
                if let Some(c) = catch {
                    let caught = self.cur_frame().try_frames.pop().unwrap_or_default();
                    let ety = self.catch_type(&caught);
                    self.push_scope();
                    if let Some((name, nspan, te)) = &c.param {
                        let ety = match te {
                            Some(te) => {
                                let tscope = self.tscope();
                                let declared = self.resolve_type(*te, &tscope);
                                if declared != UNKNOWN && !self.assignable(ety, declared) {
                                    let (a, b) = (self.show(ety), self.show(declared));
                                    self.report(Diagnostic::new("T0001", *nspan, format!("this `catch` receives `{a}`, which isn't `{b}`")));
                                }
                                declared
                            }
                            None => ety,
                        };
                        self.rec_binding(s, ety);
                        self.declare(*name, ety, LocalKind::Const, *nspan, None, false);
                    }
                    self.stmt(c.body);
                    self.pop_scope();
                }
                if let Some(f) = finally {
                    self.stmt(*f);
                }
                // Any part of the `try` may have run: forget what it changed.
                self.apply_effects(&fx, entry, &[]);
            }
            StmtKind::Break => {
                let f = self.fcx.last().unwrap().frames.last().unwrap();
                if f.loops == 0 && f.switches == 0 {
                    self.report(Diagnostic::new("F0201", span, "`break` outside of a loop or `switch`"));
                }
            }
            StmtKind::Continue => {
                let f = self.fcx.last().unwrap().frames.last().unwrap();
                if f.loops == 0 {
                    self.report(Diagnostic::new("F0202", span, "`continue` outside of a loop"));
                }
            }
        }
    }

    /// Current types of the outer locals (declared before `entry`) named in `assigned`.
    pub(super) fn end_types(&self, assigned: &[Sym], entry: usize) -> Vec<(LocalId, TyId)> {
        let mut out = Vec::new();
        for &n in assigned {
            if let Some((id, t)) = self.lookup(n)
                && id < entry
                && !out.iter().any(|(x, _)| *x == id)
            {
                out.push((id, t));
            }
        }
        out
    }

    /// After a construct with its own scope: what it did to outer locals reaches the enclosing
    /// scope. Assigned locals get their type from `ends` (or their declared type), changed values
    /// forget their field-path narrowings, and a call forgets narrowings through class instances.
    pub(super) fn apply_effects(&mut self, fx: &Effects, entry: usize, ends: &[(LocalId, TyId)]) {
        for &n in &fx.assigned {
            if let Some((id, _)) = self.lookup(n)
                && id < entry
            {
                let t = ends.iter().find(|(x, _)| *x == id).map(|(_, t)| *t).unwrap_or(self.local(id).ty);
                self.narrow(id, t);
                self.narrow_path(id, Vec::new(), None);
            }
        }
        for &n in &fx.roots {
            if let Some((id, _)) = self.lookup(n)
                && id < entry
            {
                self.narrow_path(id, Vec::new(), None);
            }
        }
        if fx.call {
            self.invalidate_heap();
        }
    }

    /// Forgets what a call may have changed: every narrowing of a path through a class
    /// instance, and every narrowing of a mutable global (any callee can reassign one).
    pub(super) fn invalidate_heap(&mut self) {
        let paths = std::mem::take(&mut self.fcx().heap_paths);
        for (l, p) in paths {
            self.narrow_path(l, p, None);
        }
        let globals: Vec<LocalId> = self.fcx().locals.iter().enumerate().filter(|(_, l)| l.global.is_some() && l.kind == LocalKind::Let).map(|(i, _)| i).collect();
        for id in globals {
            let declared = self.local(id).ty;
            self.narrow(id, declared);
            self.narrow_path(id, Vec::new(), None);
        }
    }

    /// The type of a `catch` variable: the most specific class covering everything the `try`
    /// block can throw (`Error` if nothing, or unrelated classes).
    fn catch_type(&mut self, caught: &[TyId]) -> TyId {
        let err = self.error_class();
        let mut classes: Vec<TyId> = Vec::new();
        for &t in caught {
            for m in self.flat_members(t) {
                if !classes.contains(&m) {
                    classes.push(m);
                }
            }
        }
        let Some(&first) = classes.first() else { return err };
        let mut cur = Some(first);
        while let Some(c) = cur {
            if classes.iter().all(|&o| self.assignable(o, c)) {
                return c;
            }
            cur = self.class_of(c).and_then(|(d, args)| {
                let base = self.classes[d as usize].base?;
                let map: HashMap<u32, TyId> = self.classes[d as usize].params.iter().copied().zip(args).collect();
                Some(self.types.subst(base, &map))
            });
        }
        err
    }

    fn loop_body(&mut self, body: StmtId) {
        self.fcx().frames.last_mut().unwrap().loops += 1;
        self.push_scope();
        self.stmt(body);
        self.pop_scope();
        self.fcx().frames.last_mut().unwrap().loops -= 1;
    }

    /// Forgets narrowings of locals assigned inside a loop: the loop may run again after the assignment.
    fn reset_assigned(&mut self, body: StmtId) {
        let mut names = Vec::new();
        collect_assigned_stmt(self.ast(), body, &mut names);
        self.reset_names(&names);
    }

    fn reset_assigned_expr(&mut self, e: ExprId) {
        let mut names = Vec::new();
        collect_assigned_expr(self.ast(), e, &mut names);
        self.reset_names(&names);
    }

    fn reset_names(&mut self, names: &[Sym]) {
        for &n in names {
            if let Some((id, _)) = self.lookup(n) {
                let declared = self.local(id).ty;
                self.narrow(id, declared);
                self.narrow_path(id, Vec::new(), None);
            }
        }
    }

    fn switch_mode(&mut self, disc: ExprId, dt: TyId) -> SwitchMode {
        if let ExprKind::Member { obj, name, optional: false, .. } = &self.ast().expr(disc).kind
            && let ExprKind::Ident(s) = &self.ast().expr(*obj).kind
                && let Some((id, ty)) = self.lookup(*s) {
                    let members = self.flat_members(ty);
                    if members.len() > 1 {
                        let mut variants = Vec::new();
                        for &m in &members {
                            let lit = match *self.types.get(m) {
                                Ty::Record(fs) => self.types.fields(fs).iter().find(|f| f.name == *name).and_then(|f| match *self.types.get(f.ty) {
                                    Ty::StrLit(l) => Some(l),
                                    _ => None,
                                }),
                                _ => None,
                            };
                            match lit {
                                Some(l) => variants.push((l, m)),
                                None => {
                                    variants.clear();
                                    break;
                                }
                            }
                        }
                        if !variants.is_empty() {
                            return SwitchMode::Discriminated { local: id, variants };
                        }
                    }
                }
        let members = self.flat_members(dt);
        if !members.is_empty() && members.iter().all(|&m| matches!(self.types.get(m), Ty::StrLit(_))) {
            let lits = members.iter().map(|&m| if let Ty::StrLit(l) = self.types.get(m) { *l } else { unreachable!() }).collect();
            let local = match &self.ast().expr(disc).kind {
                ExprKind::Ident(s) => self.lookup(*s).map(|(id, _)| id),
                _ => None,
            };
            return SwitchMode::Literal { local, lits };
        }
        SwitchMode::General
    }

    fn switch(&mut self, sid: StmtId, disc: ExprId, cases: &[Case], span: Span) {
        let dt = self.expr(disc, None);
        let mode = self.switch_mode(disc, dt);
        let options: Vec<Sym> = match &mode {
            SwitchMode::Discriminated { variants, .. } => variants.iter().map(|v| v.0).collect(),
            SwitchMode::Literal { lits, .. } => lits.clone(),
            SwitchMode::General => Vec::new(),
        };
        let literal_mode = !matches!(mode, SwitchMode::General);
        // Labels first, so `default` can be narrowed to what's left.
        let mut covered: Vec<Sym> = Vec::new();
        let mut labels: Vec<Option<Sym>> = Vec::new();
        let mut has_default = false;
        for case in cases {
            let Some(test) = case.test else {
                has_default = true;
                labels.push(None);
                continue;
            };
            let tspan = self.ast().expr(test).span;
            if literal_mode {
                let ExprKind::Str(lit) = self.ast().expr(test).kind else {
                    self.expr(test, None);
                    self.report(Diagnostic::new("T0104", tspan, "case labels here must be string literals"));
                    labels.push(None);
                    continue;
                };
                let v = self.name(lit).to_string();
                let v = &v;
                if !options.contains(&lit) {
                    let opts: Vec<String> = options.iter().map(|&o| self.name(o).to_string()).collect();
                    let quoted: Vec<String> = opts.iter().map(|o| format!("\"{o}\"")).collect();
                    let shown = self.show(if let SwitchMode::Discriminated { local, .. } = &mode { self.local(*local).ty } else { dt });
                    let mut d = Diagnostic::new("T0103", tspan, format!("\"{v}\" is not a possible value of `{shown}`")).note("valid values", quoted.join(", "));
                    let sug = similar(v, opts.iter().map(|s| s.as_str()));
                    if let Some(first) = sug.first() {
                        d = d.fix(Applicability::Maybe, format!("use \"{first}\""), tspan, format!("\"{first}\""));
                    }
                    self.report(d);
                    labels.push(None);
                    continue;
                }
                if covered.contains(&lit) {
                    self.report(Diagnostic::new("F0303", tspan, format!("duplicate case \"{v}\"")));
                }
                covered.push(lit);
                labels.push(Some(lit));
            } else {
                let t = self.expr(test, Some(dt));
                if !self.comparable(t, dt) {
                    let msg = format!("case of type `{}` can never match a `{}`", self.show(t), self.show(dt));
                    self.report(Diagnostic::new("T0502", tspan, msg));
                }
                labels.push(None);
            }
        }
        // Bodies, grouped: empty cases fall through to the next one.
        self.fcx().frames.last_mut().unwrap().switches += 1;
        let mut group: Vec<Option<Sym>> = Vec::new();
        let mut group_default = false;
        for (i, case) in cases.iter().enumerate() {
            if case.test.is_none() {
                group_default = true;
            }
            group.push(labels[i]);
            if case.body.is_empty() && i + 1 < cases.len() {
                continue;
            }
            self.push_scope();
            let narrowed_lits: Vec<Sym> = if group_default {
                options.iter().copied().filter(|o| !covered.contains(o) || group.contains(&Some(*o))).collect()
            } else {
                group.iter().flatten().copied().collect()
            };
            match &mode {
                SwitchMode::Discriminated { local, variants } => {
                    let tys: Vec<TyId> = variants.iter().filter(|(l, _)| narrowed_lits.contains(l)).map(|(_, t)| *t).collect();
                    if !tys.is_empty() {
                        let t = self.types.union(&tys);
                        self.narrow(*local, t);
                    }
                }
                SwitchMode::Literal { local: Some(local), .. } => {
                    let tys: Vec<TyId> = narrowed_lits.iter().map(|&l| self.types.str_lit(l)).collect();
                    if !tys.is_empty() {
                        let t = self.types.union(&tys);
                        self.narrow(*local, t);
                    }
                }
                _ => {}
            }
            for &st in &case.body {
                self.stmt(st);
            }
            self.pop_scope();
            if i + 1 < cases.len() && !case.body.is_empty() && !self.always_jumps_in_switch(&case.body) {
                let last = *case.body.last().unwrap();
                let lspan = self.ast().stmt(last).span;
                let (line, _) = self.sm.get(lspan.file).line_col(lspan.start);
                let indent: String = self.sm.get(lspan.file).line_text(line).chars().take_while(|c| c.is_whitespace()).collect();
                self.report(
                    Diagnostic::new("F0302", case.span, "this case falls through into the next one")
                        .note("why", "implicit fallthrough is almost always a bug")
                        .fix(Applicability::Safe, "end the case with `break`", lspan.empty_at_end(), format!("\n{indent}break")),
                );
            }
            group.clear();
            group_default = false;
        }
        self.fcx().frames.last_mut().unwrap().switches -= 1;
        if literal_mode && !has_default {
            let missing: Vec<Sym> = options.iter().copied().filter(|o| !covered.contains(o)).collect();
            if missing.is_empty() {
                self.exhaustive.insert((self.cur, sid));
            } else {
                let names: Vec<String> = missing.iter().map(|&m| format!("\"{}\"", self.name(m))).collect();
                let close = Span::new(span.file, span.end.saturating_sub(1), span.end.saturating_sub(1));
                let insert: String = missing.iter().map(|&m| format!("  case \"{}\": <handle>\n", self.name(m))).collect();
                let dspan = self.ast().expr(disc).span;
                self.fcx().reported_nonexhaustive = true;
                self.report(
                    Diagnostic::new("F0301", dspan, format!("`switch` is not exhaustive: missing {}", names.join(", ")))
                        .fix(Applicability::Placeholder, format!("add case{} {}", if missing.len() == 1 { "" } else { "s" }, names.join(", ")), close, insert),
                );
            }
        } else if has_default {
            self.exhaustive.insert((self.cur, sid));
        }
    }

    // ---------------------------------------------------------------- control flow

    /// Control never reaches the statement after `s`.
    pub(super) fn always_jumps(&self, s: StmtId) -> bool {
        match &self.ast().stmt(s).kind {
            StmtKind::Return(_) | StmtKind::Break | StmtKind::Continue | StmtKind::Throw(_) => true,
            StmtKind::Block(ss) => ss.iter().any(|&x| self.always_jumps(x)),
            StmtKind::If(_, t, Some(e)) => self.always_jumps(*t) && self.always_jumps(*e),
            _ => self.always_returns(s),
        }
    }

    fn always_jumps_in_switch(&self, body: &[StmtId]) -> bool {
        body.iter().any(|&s| self.always_jumps(s))
    }

    /// Every path through `s` ends in `return`.
    pub(super) fn always_returns(&self, s: StmtId) -> bool {
        let ast = self.ast();
        match &ast.stmt(s).kind {
            StmtKind::Return(_) => true,
            StmtKind::Block(ss) => {
                for &x in ss {
                    if self.always_returns(x) {
                        return true;
                    }
                    if self.always_jumps(x) {
                        return false;
                    }
                }
                false
            }
            StmtKind::If(_, t, Some(e)) => self.always_returns(*t) && self.always_returns(*e),
            StmtKind::Switch(_, cases) => {
                if !self.exhaustive.contains(&(self.cur, s)) {
                    return false;
                }
                // Each non-empty group must return (a `break` leaves the switch).
                cases.iter().enumerate().all(|(i, c)| {
                    if c.body.is_empty() && i + 1 < cases.len() {
                        return true;
                    }
                    for &x in &c.body {
                        if self.always_returns(x) {
                            return true;
                        }
                        if self.always_jumps(x) {
                            return false;
                        }
                    }
                    false
                })
            }
            StmtKind::While(c, body) => self.is_true(*c) && !has_break(ast, *body),
            StmtKind::For { cond: None, body, .. } => !has_break(ast, *body),
            StmtKind::For { cond: Some(c), body, .. } => self.is_true(*c) && !has_break(ast, *body),
            StmtKind::DoWhile(body, _) => self.always_returns(*body),
            // `throw` never falls through (for "missing return" it ends the function).
            StmtKind::Throw(_) => true,
            StmtKind::Try { body, catch, finally } => {
                finally.map(|f| self.always_returns(f)).unwrap_or(false)
                    || (self.always_returns(*body) && catch.as_ref().map(|c| self.always_returns(c.body)).unwrap_or(true))
            }
            _ => false,
        }
    }

    fn is_true(&self, e: ExprId) -> bool {
        matches!(self.ast().expr(e).kind, ExprKind::Bool(true))
    }
}

/// A `break` that exits this loop (not a nested loop or switch).
fn has_break(ast: &Ast, s: StmtId) -> bool {
    match &ast.stmt(s).kind {
        StmtKind::Break => true,
        StmtKind::Block(ss) => ss.iter().any(|&x| has_break(ast, x)),
        StmtKind::If(_, t, e) => has_break(ast, *t) || e.map(|e| has_break(ast, e)).unwrap_or(false),
        _ => false,
    }
}

fn collect_assigned_stmt(ast: &Ast, s: StmtId, out: &mut Vec<Sym>) {
    match &ast.stmt(s).kind {
        StmtKind::Expr(e) => collect_assigned_expr(ast, *e, out),
        StmtKind::Let { init: Some(e), .. } => collect_assigned_expr(ast, *e, out),
        StmtKind::If(c, t, e) => {
            collect_assigned_expr(ast, *c, out);
            collect_assigned_stmt(ast, *t, out);
            if let Some(e) = e {
                collect_assigned_stmt(ast, *e, out);
            }
        }
        StmtKind::While(c, b) | StmtKind::DoWhile(b, c) => {
            collect_assigned_expr(ast, *c, out);
            collect_assigned_stmt(ast, *b, out);
        }
        StmtKind::For { init, cond, step, body } => {
            if let Some(i) = init {
                collect_assigned_stmt(ast, *i, out);
            }
            for e in [cond, step].into_iter().flatten() {
                collect_assigned_expr(ast, *e, out);
            }
            collect_assigned_stmt(ast, *body, out);
        }
        StmtKind::ForOf { iter, body, .. } => {
            collect_assigned_expr(ast, *iter, out);
            collect_assigned_stmt(ast, *body, out);
        }
        StmtKind::Switch(d, cases) => {
            collect_assigned_expr(ast, *d, out);
            for c in cases {
                for &x in &c.body {
                    collect_assigned_stmt(ast, x, out);
                }
            }
        }
        StmtKind::Return(Some(e)) | StmtKind::Throw(e) => collect_assigned_expr(ast, *e, out),
        StmtKind::Block(ss) => ss.iter().for_each(|&x| collect_assigned_stmt(ast, x, out)),
        StmtKind::Try { body, catch, finally } => {
            collect_assigned_stmt(ast, *body, out);
            if let Some(c) = catch {
                collect_assigned_stmt(ast, c.body, out);
            }
            if let Some(f) = finally {
                collect_assigned_stmt(ast, *f, out);
            }
        }
        _ => {}
    }
}

fn collect_assigned_expr(ast: &Ast, e: ExprId, out: &mut Vec<Sym>) {
    match &ast.expr(e).kind {
        ExprKind::Assign(op, t, v) => {
            if let ExprKind::Ident(s) = ast.expr(*t).kind {
                out.push(s);
            }
            let _: &AssignOp = op;
            collect_assigned_expr(ast, *t, out);
            collect_assigned_expr(ast, *v, out);
        }
        ExprKind::Update { target, .. } => {
            if let ExprKind::Ident(s) = ast.expr(*target).kind {
                out.push(s);
            }
        }
        ExprKind::Unary(_, x) | ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Typeof(x) | ExprKind::As(x, _) | ExprKind::Try(x) | ExprKind::Await(x) => collect_assigned_expr(ast, *x, out),
        ExprKind::Binary(_, l, r) => {
            collect_assigned_expr(ast, *l, out);
            collect_assigned_expr(ast, *r, out);
        }
        ExprKind::Call { callee, args, .. } => {
            collect_assigned_expr(ast, *callee, out);
            for a in args {
                collect_assigned_expr(ast, a.expr, out);
            }
        }
        ExprKind::Member { obj, .. } => collect_assigned_expr(ast, *obj, out),
        ExprKind::Index { obj, index, .. } => {
            collect_assigned_expr(ast, *obj, out);
            collect_assigned_expr(ast, *index, out);
        }
        ExprKind::Cond(a, b, c) => {
            for x in [a, b, c] {
                collect_assigned_expr(ast, *x, out);
            }
        }
        ExprKind::Array(xs) => xs.iter().for_each(|&x| collect_assigned_expr(ast, x, out)),
        ExprKind::Object(fs) => fs.iter().for_each(|f| collect_assigned_expr(ast, f.value, out)),
        ExprKind::Template(_, xs) => xs.iter().for_each(|&x| collect_assigned_expr(ast, x, out)),
        ExprKind::Arrow(f) => match &f.body {
            ArrowBody::Expr(x) => collect_assigned_expr(ast, *x, out),
            ArrowBody::Block(b) => collect_assigned_stmt(ast, *b, out),
        },
        _ => {}
    }
}

/// What a construct may do to the locals around it.
#[derive(Default)]
pub(super) struct Effects {
    /// Locals assigned (`x = ...`, `x++`).
    pub assigned: Vec<Sym>,
    /// Locals changed in place (`x.f = ...`, `x[i] = ...`, `&x`, method calls on `x...`).
    pub roots: Vec<Sym>,
    /// Contains a call (which may change class instances).
    pub call: bool,
}

impl Effects {
    pub(super) fn extend(&mut self, o: Effects) {
        self.assigned.extend(o.assigned);
        self.roots.extend(o.roots);
        self.call |= o.call;
    }

    pub(super) fn with(mut self, o: Effects) -> Effects {
        self.extend(o);
        self
    }
}

pub(super) fn effects_of_stmts(ast: &Ast, ss: &[StmtId]) -> Effects {
    let mut fx = Effects::default();
    for &s in ss {
        fx.extend(effects_of_stmt(ast, s));
    }
    fx
}

pub(super) fn effects_of_stmt(ast: &Ast, s: StmtId) -> Effects {
    let mut fx = Effects::default();
    let mut exprs = Vec::new();
    stmt_exprs(ast, s, &mut exprs);
    for e in exprs {
        effects_into(ast, e, &mut fx);
    }
    fx
}

pub(super) fn effects_of_expr(ast: &Ast, e: ExprId) -> Effects {
    let mut fx = Effects::default();
    effects_into(ast, e, &mut fx);
    fx
}

fn root_sym(ast: &Ast, mut e: ExprId) -> Option<Sym> {
    loop {
        match &ast.expr(e).kind {
            ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Member { obj: x, .. } | ExprKind::Index { obj: x, .. } => e = *x,
            ExprKind::Ident(s) => return Some(*s),
            ExprKind::This => return Some(super::THIS_SYM),
            _ => return None,
        }
    }
}

/// Effects of evaluating `e` (arrow bodies don't run when the arrow is created).
fn effects_into(ast: &Ast, e: ExprId, fx: &mut Effects) {
    match &ast.expr(e).kind {
        ExprKind::Assign(_, t, v) => {
            match &ast.expr(*t).kind {
                ExprKind::Ident(s) => fx.assigned.push(*s),
                _ => fx.roots.extend(root_sym(ast, *t)),
            }
            effects_into(ast, *t, fx);
            effects_into(ast, *v, fx);
        }
        ExprKind::Update { target, .. } => {
            match &ast.expr(*target).kind {
                ExprKind::Ident(s) => fx.assigned.push(*s),
                _ => fx.roots.extend(root_sym(ast, *target)),
            }
            effects_into(ast, *target, fx);
        }
        ExprKind::Call { callee, args, .. } => {
            fx.call = true;
            if let ExprKind::Member { obj, .. } = &ast.expr(*callee).kind {
                fx.roots.extend(root_sym(ast, *obj));
            }
            effects_into(ast, *callee, fx);
            for a in args {
                if a.by_ref.is_some() {
                    match &ast.expr(a.expr).kind {
                        ExprKind::Ident(s) => fx.assigned.push(*s),
                        _ => fx.roots.extend(root_sym(ast, a.expr)),
                    }
                }
                effects_into(ast, a.expr, fx);
            }
        }
        ExprKind::New { args, .. } => {
            fx.call = true;
            for a in args {
                effects_into(ast, a.expr, fx);
            }
        }
        // A getter is a call; any member access through a class may run one.
        ExprKind::Member { obj, .. } => {
            fx.call = true;
            effects_into(ast, *obj, fx);
        }
        ExprKind::Unary(_, x) | ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Typeof(x) | ExprKind::As(x, _) | ExprKind::Try(x) | ExprKind::Await(x) => effects_into(ast, *x, fx),
        ExprKind::Binary(_, l, r) => {
            effects_into(ast, *l, fx);
            effects_into(ast, *r, fx);
        }
        ExprKind::Index { obj, index, .. } => {
            effects_into(ast, *obj, fx);
            effects_into(ast, *index, fx);
        }
        ExprKind::Cond(a, b, c) => {
            for x in [a, b, c] {
                effects_into(ast, *x, fx);
            }
        }
        ExprKind::Array(xs) | ExprKind::Template(_, xs) => xs.iter().for_each(|&x| effects_into(ast, x, fx)),
        ExprKind::Object(fs) => fs.iter().for_each(|f| effects_into(ast, f.value, fx)),
        _ => {}
    }
}

/// The expressions a statement evaluates directly or in nested statements (not inside arrows).
fn stmt_exprs(ast: &Ast, s: StmtId, out: &mut Vec<ExprId>) {
    match &ast.stmt(s).kind {
        StmtKind::Expr(e) | StmtKind::Return(Some(e)) | StmtKind::Throw(e) => out.push(*e),
        StmtKind::Let { init: Some(e), .. } => out.push(*e),
        StmtKind::Try { body, catch, finally } => {
            stmt_exprs(ast, *body, out);
            if let Some(c) = catch {
                stmt_exprs(ast, c.body, out);
            }
            if let Some(f) = finally {
                stmt_exprs(ast, *f, out);
            }
        }
        StmtKind::If(c, t, e) => {
            out.push(*c);
            stmt_exprs(ast, *t, out);
            if let Some(e) = e {
                stmt_exprs(ast, *e, out);
            }
        }
        StmtKind::While(c, b) | StmtKind::DoWhile(b, c) => {
            out.push(*c);
            stmt_exprs(ast, *b, out);
        }
        StmtKind::For { init, cond, step, body } => {
            if let Some(i) = init {
                stmt_exprs(ast, *i, out);
            }
            out.extend([cond, step].into_iter().flatten().copied());
            stmt_exprs(ast, *body, out);
        }
        StmtKind::ForOf { iter, body, .. } => {
            out.push(*iter);
            stmt_exprs(ast, *body, out);
        }
        StmtKind::Switch(d, cases) => {
            out.push(*d);
            for c in cases {
                out.extend(c.test);
                for &x in &c.body {
                    stmt_exprs(ast, x, out);
                }
            }
        }
        StmtKind::Block(ss) => ss.iter().for_each(|&x| stmt_exprs(ast, x, out)),
        _ => {}
    }
}

/// Locals changed inside closures anywhere in a function body: (assigned or changed through —
/// a field, an element, a method call on them —, only assigned).
pub(super) fn closure_mutated_stmt(ast: &Ast, s: StmtId) -> (HashSet<Sym>, HashSet<Sym>) {
    let mut exprs = Vec::new();
    stmt_exprs(ast, s, &mut exprs);
    let mut out = (HashSet::default(), HashSet::default());
    for e in exprs {
        closure_mutated_into(ast, e, false, &mut out);
    }
    out
}

pub(super) fn closure_mutated_expr(ast: &Ast, e: ExprId) -> (HashSet<Sym>, HashSet<Sym>) {
    let mut out = (HashSet::default(), HashSet::default());
    closure_mutated_into(ast, e, false, &mut out);
    out
}

fn closure_mutated_into(ast: &Ast, e: ExprId, inside: bool, out: &mut (HashSet<Sym>, HashSet<Sym>)) {
    let mut kids: Vec<ExprId> = Vec::new();
    match &ast.expr(e).kind {
        ExprKind::Arrow(f) => {
            let mut exprs = Vec::new();
            match &f.body {
                ArrowBody::Expr(x) => exprs.push(*x),
                ArrowBody::Block(b) => stmt_exprs(ast, *b, &mut exprs),
            }
            for x in exprs {
                closure_mutated_into(ast, x, true, out);
            }
            return;
        }
        _ if inside => {
            let fx = effects_of_expr(ast, e);
            out.1.extend(fx.assigned.iter().copied());
            out.0.extend(fx.assigned);
            out.0.extend(fx.roots);
        }
        _ => {}
    }
    collect_children(ast, e, &mut kids);
    for k in kids {
        closure_mutated_into(ast, k, inside, out);
    }
}

fn collect_children(ast: &Ast, e: ExprId, out: &mut Vec<ExprId>) {
    match &ast.expr(e).kind {
        ExprKind::Unary(_, x) | ExprKind::Paren(x) | ExprKind::NonNull(x) | ExprKind::Typeof(x) | ExprKind::As(x, _) | ExprKind::Try(x) | ExprKind::Await(x) => out.push(*x),
        ExprKind::Binary(_, l, r) | ExprKind::Assign(_, l, r) => out.extend([*l, *r]),
        ExprKind::Update { target, .. } => out.push(*target),
        ExprKind::Call { callee, args, .. } | ExprKind::New { callee, args, .. } => {
            out.push(*callee);
            out.extend(args.iter().map(|a| a.expr));
        }
        ExprKind::Member { obj, .. } => out.push(*obj),
        ExprKind::Index { obj, index, .. } => out.extend([*obj, *index]),
        ExprKind::Cond(a, b, c) => out.extend([*a, *b, *c]),
        ExprKind::Array(xs) | ExprKind::Template(_, xs) => out.extend(xs.iter().copied()),
        ExprKind::Object(fs) => out.extend(fs.iter().map(|f| f.value)),
        _ => {}
    }
}
