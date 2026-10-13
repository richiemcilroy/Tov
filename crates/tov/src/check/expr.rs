use super::*;
use crate::ast::{Arg, ArrowBody, ArrowFn, AssignOp, BinOp, ExprKind, UnOp};

pub(super) struct CallSig {
    pub tparams: Vec<u32>,
    pub params: Vec<FnParam>,
    pub names: Vec<String>,
    pub rest: Option<TyId>,
    pub ret: TyId,
    /// Errors the callee can throw (`NEVER`: none).
    pub throws: TyId,
}

pub(super) struct Place {
    pub ty: TyId,
    /// The place is exactly this local (`x = ...`).
    pub local: Option<LocalId>,
    /// The local the place is rooted in (`x.a[i]` → `x`).
    pub root: Option<LocalId>,
}

enum Root {
    Local(LocalId),
    /// A module variable: (module, item).
    ModuleConst(u32, u32),
    Temp,
    /// Inside a class instance: changes the shared object, whatever variable holds it.
    Heap,
}

impl<'a> Checker<'a> {
    /// A decimal literal must fit: 34 significant digits, 34 after the point.
    fn check_decimal_literal(&mut self, text: &str, span: Span) {
        if crate::types::decimal_literal(text).is_none() {
            self.report(Diagnostic::new("T0020", span, format!("{text} doesn't fit in `decimal` (34 significant digits, at most 34 after the point)")));
        }
    }

    /// An integer or float literal (possibly negated or in parentheses).
    pub(super) fn is_num_literal(&self, e: ExprId) -> bool {
        match &self.ast().expr(e).kind {
            ExprKind::Int(_) | ExprKind::Float(_) => true,
            ExprKind::Unary(UnOp::Neg, x) | ExprKind::Paren(x) => self.is_num_literal(*x),
            _ => false,
        }
    }

    pub(super) fn is_int_literal(&self, e: ExprId) -> bool {
        match &self.ast().expr(e).kind {
            ExprKind::Int(_) => true,
            ExprKind::Unary(UnOp::Neg, x) | ExprKind::Paren(x) => self.is_int_literal(*x),
            _ => false,
        }
    }

    fn numeric_hint(&mut self, exp: Option<TyId>) -> Option<TyId> {
        let exp = exp?;
        let members = self.flat_members(exp);
        if members.contains(&INT) {
            return Some(INT);
        }
        if members.contains(&DECIMAL) {
            return Some(DECIMAL);
        }
        members.iter().copied().find(|&m| self.types.is_numeric(m))
    }

    pub(super) fn printable(&self, t: TyId) -> bool {
        !matches!(self.types.get(t), Ty::Func(..) | Ty::Namespace(_) | Ty::BuiltinNs(_) | Ty::Expect(_) | Ty::Void | Ty::Never)
    }

    pub(super) fn expr(&mut self, e: ExprId, exp: Option<TyId>) -> TyId {
        let t = self.expr_inner(e, exp);
        if let Some(f) = self.facts_mut() {
            f.expr_ty[e as usize] = t;
        }
        if matches!(self.types.get(self.types_without_undef(t)), Ty::Class(..))
            && let Some(f) = self.fcx.last_mut()
        {
            f.class_valued.insert(e);
        }
        t
    }

    fn expr_inner(&mut self, e: ExprId, exp: Option<TyId>) -> TyId {
        let ast = self.ast();
        let node = ast.expr(e);
        let span = node.span;
        match &node.kind {
            ExprKind::Error => ERROR,
            ExprKind::Int(v) => {
                let t = self.numeric_hint(exp).unwrap_or(INT);
                if let Some((lo, hi)) = int_range(t)
                    && (*v as i128) > hi {
                        let msg = format!("{v} doesn't fit in `{}` (range {lo}..={hi})", self.show(t));
                        self.report(Diagnostic::new("T0020", span, msg));
                    }
                t
            }
            ExprKind::BigInt(_) => BIGINT,
            ExprKind::Decimal(s) => {
                let text = self.name(*s).to_string();
                self.check_decimal_literal(&text, span);
                DECIMAL
            }
            ExprKind::Float(_) => match self.numeric_hint(exp) {
                Some(F32) => F32,
                Some(DECIMAL) => {
                    let text = self.src(span).to_string();
                    self.check_decimal_literal(&text, span);
                    DECIMAL
                }
                _ => F64,
            },
            ExprKind::Str(s) => self.types.str_lit(*s),
            ExprKind::Template(_, exprs) => {
                for &x in exprs {
                    let t = self.expr(x, None);
                    if !self.printable(t) {
                        let msg = format!("a value of type `{}` can't be interpolated into a string", self.show(t));
                        let s = self.ast().expr(x).span;
                        self.report(Diagnostic::new("T0510", s, msg));
                    }
                }
                STR
            }
            ExprKind::Bool(_) => BOOL,
            ExprKind::Undefined => UNDEFINED,
            ExprKind::Null => {
                self.report(
                    Diagnostic::new("X0002", span, "`null` is not supported; Tov has one \"absent\" value, `undefined`")
                        .fix(Applicability::Safe, "use `undefined`", span, "undefined"),
                );
                UNDEFINED
            }
            ExprKind::Ident(sym) => self.ident(e, *sym, span),
            ExprKind::Paren(x) => self.expr(*x, exp),
            ExprKind::Await(x) => self.await_expr(e, *x, exp, span),
            ExprKind::Unary(op, x) => self.unary(*op, *x, exp, span),
            ExprKind::Binary(op, l, r) => self.binary(*op, *l, *r, exp, span),
            ExprKind::Assign(op, t, v) => self.assign(*op, *t, *v, span),
            ExprKind::Update { target, .. } => {
                let Some(place) = self.place(*target, false) else { return ERROR };
                if !self.types.is_num_like(place.ty) && place.ty != ERROR && place.ty != BIGINT {
                    let msg = format!("`++`/`--` need a number, found `{}`", self.show(place.ty));
                    self.report(Diagnostic::new("T0501", span, msg));
                }
                place.ty
            }
            ExprKind::Call { callee, type_args, args, optional } => {
                let t = self.call(e, *callee, type_args, args, *optional, exp, span);
                // The callee may change class instances through any reference.
                self.invalidate_heap();
                if let Some(f) = self.facts_mut()
                    && let Some(c) = f.calls.get_mut(&e)
                {
                    c.ret = t;
                }
                t
            }
            ExprKind::New { callee, type_args, args } => {
                let t = self.new_expr(e, *callee, type_args, args, exp, span);
                self.invalidate_heap();
                t
            }
            ExprKind::Member { obj, name, name_span, optional } => self.member(e, *obj, *name, *name_span, *optional, span),
            ExprKind::Index { obj, index, optional } => self.index(*obj, *index, *optional, span),
            ExprKind::Object(fields) => self.object(fields, exp, span),
            ExprKind::Array(elems) => self.array_lit(elems, exp, span),
            ExprKind::Arrow(f) => self.arrow(f, exp, span),
            ExprKind::Cond(c, a, b) => {
                let fx = super::stmt::effects_of_expr(self.ast(), *a).with(super::stmt::effects_of_expr(self.ast(), *b));
                let entry = self.fcx().locals.len();
                let t = self.cond_expr(*c, *a, *b, exp);
                self.apply_effects(&fx, entry, &[]);
                t
            }
            ExprKind::As(x, te) => {
                if let TypeExprKind::Named { name, ns: None, args, .. } = &self.ast().ty(*te).kind
                    && self.name(*name) == "const" && args.is_empty() {
                        return self.expr(*x, exp);
                    }
                let tscope = self.tscope();
                let target = self.resolve_type(*te, &tscope);
                // The target guides literal typing (`[] as int[]`), like an annotation would.
                let src = self.expr(*x, Some(target));
                if src == ERROR || target == ERROR || src == UNKNOWN || self.assignable(src, target) || self.assignable(target, src) {
                    return target;
                }
                if src == JS {
                    // a checked conversion from JavaScript (traps if the value doesn't fit)
                    if !self.js_receivable(target) {
                        let ts = self.show(target);
                        self.report(
                            Diagnostic::new("T0903", span, format!("a JavaScript value can't be converted to `{ts}`"))
                                .note("why", "conversions from JavaScript produce numbers, strings, booleans, `undefined`, arrays, records and unions of these"),
                        );
                    }
                    return target;
                }
                let (ss, ts) = (self.show(src), self.show(target));
                let text = self.src(self.ast().expr(*x).span).to_string();
                let mut d = Diagnostic::new("T0601", span, format!("can't cast `{ss}` to `{ts}`"))
                    .note("why", "`as` only narrows (e.g. from `unknown` or a union); it never converts values");
                if target == BIGINT && (self.types.is_numeric(src) || self.types.is_string(src)) {
                    d = d.fix(Applicability::Maybe, format!("convert: `BigInt({text})`"), span, format!("BigInt({text})"));
                } else if src == BIGINT && self.types.is_numeric(target) {
                    let conv = if target == F64 { "Number".to_string() } else { ts.clone() };
                    d = d.fix(Applicability::Maybe, format!("convert: `{conv}({text})`"), span, format!("{conv}({text})"));
                } else if self.types.is_numeric(src) && self.types.is_numeric(target) {
                    if self.types.is_int(target) && self.types.is_float(src) {
                        d = d.fix(Applicability::Maybe, format!("convert: `Math.trunc({text})`"), span, format!("Math.trunc({text})"));
                    } else {
                        d = d.fix(Applicability::Maybe, format!("convert: `{ts}({text})`"), span, format!("{ts}({text})"));
                    }
                } else if target == STR {
                    d = d.fix(Applicability::Maybe, format!("convert: `String({text})`"), span, format!("String({text})"));
                }
                self.report(d);
                target
            }
            ExprKind::NonNull(x) => {
                let hint = exp.map(|t| self.types.optional(t));
                let t = self.expr(*x, hint);
                self.types.without_undefined(t)
            }
            ExprKind::Typeof(x) => {
                self.expr(*x, None);
                STR
            }
            ExprKind::This => self.this_expr(e, span),
            ExprKind::Super => {
                self.report(Diagnostic::new("T0801", span, "`super` is only valid as `super(...)` in a constructor or `super.method(...)`"));
                ERROR
            }
            ExprKind::Try(x) => {
                self.cur_frame().try_expr += 1;
                let t = self.expr(*x, exp);
                self.cur_frame().try_expr -= 1;
                t
            }
        }
    }

    fn cond_expr(&mut self, c: ExprId, a: ExprId, b: ExprId, exp: Option<TyId>) -> TyId {
        let (c, a, b) = (&c, &a, &b);
        self.cond(*c);
        let pos = self.narrowings(*c, true);
        let neg = self.narrowings(*c, false);
        self.push_scope();
        self.apply(&pos);
        let ta = self.expr(*a, exp);
        self.pop_scope();
        self.push_scope();
        self.apply(&neg);
        let tb = self.expr(*b, exp.or(Some(ta)));
        self.pop_scope();
        if let Some(x) = exp
            && self.assignable(ta, x) && self.assignable(tb, x) {
                return self.types.union(&[ta, tb]);
            }
        if self.types.is_int(ta) && self.types.is_float(tb) || self.types.is_float(ta) && self.types.is_int(tb) {
            return F64;
        }
        self.types.union(&[ta, tb])
    }

    fn ident(&mut self, e: ExprId, sym: Sym, span: Span) -> TyId {
        if let Some((id, ty)) = self.lookup(sym) {
            // (a script-local standing in for a top-level `const`/`let`: still a global read)
            if let Some((m, i)) = self.local(id).global {
                self.rec_ident(e, IdentFact::Const(m, i));
            } else {
                let key = self.local(id).span.start;
                self.rec_ident(e, IdentFact::Local(key));
            }
            self.fcx().reads.push((id, span.start));
            return ty;
        }
        match self.scopes[self.cur as usize].values.get(&sym).map(|d| d.0) {
            Some(Decl::Fn(m, i)) => self.rec_ident(e, IdentFact::Fn(m, i)),
            Some(Decl::Const(m, i)) => self.rec_ident(e, IdentFact::Const(m, i)),
            Some(Decl::Ns(m)) => self.rec_ident(e, IdentFact::Ns(m)),
            Some(Decl::Class(c)) => self.rec_ident(e, IdentFact::Class(c)),
            Some(Decl::Npm(m, n)) => {
                self.rec_ident(e, IdentFact::Npm(m, n));
                return JS;
            }
            _ => self.rec_ident(e, IdentFact::Builtin),
        }
        match self.scopes[self.cur as usize].values.get(&sym).map(|d| d.0) {
            Some(Decl::Fn(m, i)) => {
                let Some(sig) = self.fn_sig(m, i) else {
                    let n = self.name(sym).to_string();
                    self.report(Diagnostic::new("T0303", span, format!("`{n}` is recursive, so it needs an explicit return type")));
                    return ERROR;
                };
                if !sig.tparams.is_empty() {
                    let n = self.name(sym).to_string();
                    self.report(
                        Diagnostic::new("T0304", span, format!("generic function `{n}` can't be used as a value"))
                            .note("instead", format!("wrap it in an arrow: `(x) => {n}(x)`")),
                    );
                    return ERROR;
                }
                return self.types.func(sig.params, sig.ret);
            }
            Some(Decl::Const(m, i)) => return self.const_type(m, i),
            Some(Decl::Ns(m)) => return self.types.intern(Ty::Namespace(m)),
            Some(Decl::Class(_)) => {
                let n = self.name(sym).to_string();
                self.report(
                    Diagnostic::new("T0803", span, format!("class `{n}` is not a value"))
                        .note("instead", format!("create an instance with `new {n}(...)`, or use its static members (`{n}.name`)")),
                );
                return ERROR;
            }
            _ => {}
        }
        let text = self.name(sym).to_string();
        if self.is_ns(&text) {
            return self.types.intern(Ty::BuiltinNs(sym));
        }
        if let Some(msg) = builtins::removed_global(&text) {
            self.report(Diagnostic::new("X0032", span, msg));
            return ERROR;
        }
        if builtins::is_builtin_fn(&text) {
            self.report(Diagnostic::new("T0203", span, format!("built-in `{text}` must be called")));
            return ERROR;
        }
        if self.scopes[self.cur as usize].types.contains_key(&sym) {
            self.report(Diagnostic::new("N0001", span, format!("`{text}` is a type, not a value")));
            return ERROR;
        }
        let mut candidates: Vec<String> = Vec::new();
        if let Some(fcx) = self.fcx.last() {
            for scope in &fcx.scopes {
                candidates.extend(scope.iter().filter(|(s, _, _)| *s != super::PATH_SYM && *s != super::THIS_SYM).map(|(s, _, _)| self.name(*s).to_string()));
            }
        }
        candidates.extend(self.scopes[self.cur as usize].values.keys().map(|&s| self.name(s).to_string()));
        candidates.extend(builtins::GLOBAL_NAMES.iter().map(|s| s.to_string()));
        let sug = similar(&text, candidates.iter().map(|s| s.as_str()));
        let mut d = Diagnostic::new("N0001", span, format!("unknown name `{text}`"));
        if let Some(first) = sug.first() {
            d = d.note("did you mean", sug.join(", ")).fix(Applicability::Maybe, format!("use `{first}`"), span, first.to_string());
        }
        self.report(d);
        ERROR
    }

    fn unary(&mut self, op: UnOp, x: ExprId, exp: Option<TyId>, span: Span) -> TyId {
        match op {
            UnOp::Neg => {
                let t = self.expr(x, exp);
                if t != ERROR && !self.types.is_num_like(t) && t != BIGINT {
                    let msg = format!("unary `-` needs a number, found `{}`", self.show(t));
                    self.report(Diagnostic::new("T0501", span, msg));
                    return ERROR;
                }
                if matches!(self.types.get(t), Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64) && !self.is_int_literal(x) {
                    let msg = format!("unary `-` on unsigned `{}`", self.show(t));
                    self.report(Diagnostic::new("T0501", span, msg));
                }
                t
            }
            UnOp::Plus => {
                let t = self.expr(x, None);
                let text = self.src(self.ast().expr(x).span).to_string();
                let mut d = Diagnostic::new("X0028", span, "unary `+` is not supported");
                if self.types.is_string(t) {
                    d = d.note("instead", "parse explicitly").fix(Applicability::Maybe, format!("use `Number({text})`"), span, format!("Number({text})"));
                    self.report(d);
                    return self.types.optional(F64);
                }
                d = d.fix(Applicability::Safe, "remove the `+`", span, text);
                self.report(d);
                t
            }
            UnOp::Not => {
                self.cond(x);
                BOOL
            }
            UnOp::BitNot => {
                let t = self.expr(x, exp);
                if t != ERROR && !self.types.is_int(t) && t != BIGINT {
                    let msg = format!("`~` needs an integer, found `{}`", self.show(t));
                    self.report(Diagnostic::new("T0501", span, msg));
                    return ERROR;
                }
                t
            }
            UnOp::Void => {
                self.expr(x, None);
                self.report(Diagnostic::new("X0029", span, "the `void` operator is not supported"));
                UNDEFINED
            }
        }
    }

    /// Span of an operator between two operands, found in the source.
    fn op_span(&self, l: ExprId, r: ExprId, op: &str) -> Span {
        let (ls, rs) = (self.ast().expr(l).span, self.ast().expr(r).span);
        let text = &self.sm.get(ls.file).text[ls.end as usize..rs.start as usize];
        match text.find(op) {
            Some(i) => Span::new(ls.file, ls.end + i as u32, ls.end + (i + op.len()) as u32),
            None => ls.empty_at_end(),
        }
    }

    fn binary(&mut self, op: BinOp, l: ExprId, r: ExprId, exp: Option<TyId>, span: Span) -> TyId {
        use BinOp::*;
        match op {
            And | Or => {
                let left_ok = self.logical_operand(l, op, r);
                let n = self.narrowings(l, op == And);
                let fx = super::stmt::effects_of_expr(self.ast(), r);
                let entry = self.fcx().locals.len();
                self.push_scope();
                self.apply(&n);
                if left_ok {
                    self.logical_operand(r, op, r);
                } else {
                    self.expr(r, None);
                }
                self.pop_scope();
                self.apply_effects(&fx, entry, &[]);
                BOOL
            }
            Nullish => {
                let hint = exp.map(|t| self.types.optional(t));
                let tl = self.expr(l, hint);
                let inner = self.types.without_undefined(tl);
                let tr = self.expr(r, exp.or(Some(inner)));
                if self.types.is_int(inner) && self.types.is_float(tr) || self.types.is_float(inner) && self.types.is_int(tr) {
                    return F64;
                }
                let keeps_literals = exp.map(|x| self.flat_members(x).iter().any(|&m| matches!(self.types.get(m), Ty::StrLit(_)))).unwrap_or(false)
                    || matches!(self.types.get(inner), Ty::StrLit(_));
                let tr = if keeps_literals { tr } else { self.types.widen(tr) };
                self.types.union(&[inner, tr])
            }
            LooseEq | LooseNe => {
                let (bad, good) = if op == LooseEq { ("==", "===") } else { ("!=", "!==") };
                let os = self.op_span(l, r, bad);
                self.report(
                    Diagnostic::new("X0003", os, format!("`{bad}` is not supported"))
                        .note("why", "Tov has no type coercion; equality is always strict")
                        .fix(Applicability::Safe, format!("use `{good}`"), os, good),
                );
                self.equality(l, r, span)
            }
            Eq | Ne => self.equality(l, r, span),
            Lt | Gt | Le | Ge => {
                let (tl, tr) = self.numeric_pair(l, r, None);
                if tl == BIGINT || tr == BIGINT {
                    // bigint with bigint, or with an integer (compared exactly, as in JavaScript)
                    let ok = |t: TyId| t == BIGINT || t == ERROR || self.types.is_int(t);
                    if !ok(tl) || !ok(tr) {
                        self.bigint_mix(op, l, r, tl, tr, span);
                    }
                    return BOOL;
                }
                if tl == DECIMAL || tr == DECIMAL {
                    self.decimal_arith(op, l, r, tl, tr, span);
                    return BOOL;
                }
                let ok = (self.types.is_numeric(tl) && self.types.is_numeric(tr)) || (self.types.is_string(tl) && self.types.is_string(tr));
                if !ok && tl != ERROR && tr != ERROR {
                    let msg = format!("can't compare `{}` with `{}` using `{}`", self.show(tl), self.show(tr), op.as_str());
                    let mut d = Diagnostic::new("T0501", span, msg);
                    if self.types.has_undefined(tl) || self.types.has_undefined(tr) {
                        d = d.note("why", "one side may be `undefined`; narrow it first");
                    }
                    self.report(d);
                } else if ok && self.types.is_int(tl) && self.types.is_int(tr) && tl != tr {
                    self.mixed_ints(tl, tr, span);
                }
                BOOL
            }
            Add => {
                let (tl, tr) = self.numeric_pair(l, r, exp);
                if self.types.is_string(tl) || self.types.is_string(tr) {
                    for (t, x) in [(tl, l), (tr, r)] {
                        if !self.printable(t) {
                            let msg = format!("can't concatenate `{}` to a string", self.show(t));
                            let s = self.ast().expr(x).span;
                            self.report(Diagnostic::new("T0501", s, msg));
                        }
                    }
                    return STR;
                }
                if tl == DECIMAL || tr == DECIMAL {
                    return self.decimal_arith(op, l, r, tl, tr, span);
                }
                if tl == BIGINT || tr == BIGINT {
                    return self.bigint_arith(op, l, r, tl, tr, span);
                }
                self.arith(op, tl, tr, span)
            }
            Sub | Mul | Rem | Pow => {
                let (tl, tr) = self.numeric_pair(l, r, exp);
                if tl == DECIMAL || tr == DECIMAL {
                    return self.decimal_arith(op, l, r, tl, tr, span);
                }
                if tl == BIGINT || tr == BIGINT {
                    return self.bigint_arith(op, l, r, tl, tr, span);
                }
                self.arith(op, tl, tr, span)
            }
            Div => {
                let (tl, tr) = self.numeric_pair(l, r, None);
                if tl == DECIMAL || tr == DECIMAL {
                    return self.decimal_arith(op, l, r, tl, tr, span);
                }
                if tl == BIGINT || tr == BIGINT {
                    return self.bigint_arith(op, l, r, tl, tr, span);
                }
                let t = self.arith(op, tl, tr, span);
                if t == ERROR {
                    ERROR
                } else if tl == F32 && tr == F32 {
                    F32
                } else {
                    F64
                }
            }
            BitAnd | BitOr | BitXor | Shl | Shr | UShr => {
                let (tl, tr) = self.numeric_pair(l, r, exp);
                if tl == ERROR || tr == ERROR {
                    return ERROR;
                }
                if tl == BIGINT || tr == BIGINT {
                    if op == UShr {
                        self.report(Diagnostic::new("T0501", span, "`>>>` doesn't apply to `bigint`: a bigint has no fixed width").note("instead", "`>>` (it rounds toward negative infinity), or `BigInt.asUintN(bits, x) >> n`"));
                        return ERROR;
                    }
                    return self.bigint_arith(op, l, r, tl, tr, span);
                }
                if !self.types.is_int(tl) || !self.types.is_int(tr) {
                    let msg = format!("`{}` needs integers, found `{}` and `{}`", op.as_str(), self.show(tl), self.show(tr));
                    self.report(Diagnostic::new("T0501", span, msg).note("instead", "convert floats first with `Math.trunc(x)`"));
                    return ERROR;
                }
                if tl != tr && !matches!(op, Shl | Shr | UShr) {
                    self.mixed_ints(tl, tr, span);
                }
                tl
            }
            In => {
                self.expr(l, None);
                self.expr(r, None);
                self.report(
                    Diagnostic::new("X0025", span, "the `in` operator is not supported")
                        .note("instead", "records have fixed fields; for dynamic keys use `map.has(key)`"),
                );
                BOOL
            }
            Instanceof => {
                let tl = self.expr(l, None);
                let Some(c) = self.instanceof_class(r) else { return BOOL };
                let members = self.flat_members(tl);
                let possible = tl == ERROR || tl == UNKNOWN || members.iter().any(|&m| match self.class_of(m) {
                    Some((mc, _)) => self.class_descends(mc, c) || self.class_descends(c, mc),
                    None => false,
                });
                if !possible {
                    let (ls, cs) = (self.show(tl), self.class_names[c as usize].clone());
                    self.report(Diagnostic::new("T0502", span, format!("this check is always false: `{ls}` is never a `{cs}`")));
                }
                BOOL
            }
        }
    }

    pub(super) fn cur_frame(&mut self) -> &mut Frame {
        self.fcx().frames.last_mut().expect("in a function")
    }

    /// An error of type `t` leaves the current expression: caught by an enclosing `try` block,
    /// or passed on (a call must then be marked `try`). `callee`: the call's description.
    pub(super) fn on_throw(&mut self, t: TyId, span: Span, callee: Option<&str>, call: Option<ExprId>) {
        let cur = self.cur as usize;
        let Some(frame) = self.fcx.last_mut().and_then(|f| f.frames.last_mut()) else { return };
        if let Some(tf) = frame.try_frames.last_mut() {
            tf.push(t);
            if let (Some(c), Some(f)) = (call, self.facts.as_mut()) {
                f[cur].throwing.insert(c);
            }
            return;
        }
        let (can_throw, marked, decl, module_init) = (frame.can_throw, frame.try_expr > 0, frame.throws_decl, frame.module_init);
        let shown = self.show(t);
        if !can_throw && module_init {
            let what = callee.map(|c| format!("`{c}` can throw `{shown}`")).unwrap_or_else(|| format!("this throws `{shown}`"));
            self.report(
                Diagnostic::new("T0832", span, format!("{what}, but a module constant's initializer can't pass errors on"))
                    .note("instead", "initialize it in a function that can throw, or catch the error in a helper function"),
            );
            return;
        }
        if !can_throw {
            let what = callee.map(|c| format!("`{c}` can throw `{shown}`")).unwrap_or_else(|| format!("this throws `{shown}`"));
            self.report(
                Diagnostic::new("T0832", span, format!("{what}, but this closure's type doesn't let it pass errors on"))
                    .note("instead", "catch it inside the closure: `try { ... } catch (e) { ... }`")
                    .note("or", format!("let the function type throw: `(...) => T throws {shown}`")),
            );
            return;
        }
        if let Some(c) = callee
            && !marked
        {
            let text = self.src(span).to_string();
            self.report(
                Diagnostic::new("T0831", span, format!("`{c}` can throw `{shown}`; mark the call `try` to pass the error on, or catch it"))
                    .note("why", "errors are values in Tov: every call that can fail is marked, so control flow is visible")
                    .fix(Applicability::Safe, format!("pass it on: `try {text}`"), span.empty_at_start(), "try ")
                    .note("or", "wrap it in `try { ... } catch (e) { ... }`"),
            );
        }
        if let Some(d) = decl
            && !self.assignable(t, d)
        {
            let ds = self.show(d);
            self.report(
                Diagnostic::new("T0833", span, format!("this can throw `{shown}`, but the function declares `throws {ds}`"))
                    .note("instead", format!("widen the declaration (`throws {ds} | {shown}`), or catch it here")),
            );
        }
        if let Some(c) = call
            && let Some(f) = self.facts.as_mut()
        {
            f[cur].throwing.insert(c);
        }
        self.cur_frame().thrown.push(t);
    }

    /// Checks that a `throws` type is made of `Error` classes.
    pub(crate) fn check_throws_type(&mut self, t: TyId, span: Span) {
        let err = self.error_class();
        if err != ERROR && t != ERROR && !self.assignable(t, err) {
            let shown = self.show(t);
            self.report(Diagnostic::new("T0834", span, format!("`throws {shown}`: errors must be classes that extend `Error`")).note("example", "`class NotFound extends Error {}`, then `throws NotFound`"));
        }
    }

    /// A built-in namespace (`Math`, `process`, ...; `__native` only in standard-library modules).
    pub(super) fn is_ns(&self, text: &str) -> bool {
        builtins::is_builtin_ns(text) || (text == "__native" && self.modules[self.cur as usize].std)
    }

    /// A class declared by the built-in prelude (`Error`, `SyntaxError`, ...).
    pub(super) fn builtin_class(&mut self, name: &str) -> TyId {
        let b = self.modules.iter().position(|m| m.builtin);
        let sym = self.interner.lookup(name);
        match (b, sym) {
            (Some(b), Some(sym)) => match self.scopes[b].types.get(&sym).map(|d| d.0) {
                Some(Decl::Class(c)) => {
                    self.resolve_class(c);
                    self.classes[c as usize].this_ty
                }
                _ => ERROR,
            },
            _ => ERROR,
        }
    }

    /// The class named on the right of `instanceof`.
    fn instanceof_class(&mut self, r: ExprId) -> Option<u32> {
        if let ExprKind::Ident(s) = self.ast().expr(r).kind
            && self.lookup(s).is_none()
            && let Some(Decl::Class(c)) = self.scopes[self.cur as usize].values.get(&s).map(|d| d.0)
        {
            self.rec_ident(r, IdentFact::Class(c));
            self.resolve_class(c);
            return Some(c);
        }
        let span = self.ast().expr(r).span;
        let text = self.src(span).to_string();
        self.report(
            Diagnostic::new("T0803", span, format!("`instanceof` needs a class name, found `{text}`"))
                .note("note", "records and unions are told apart by a discriminant: `x.kind === \"...\"`"),
        );
        None
    }

    /// Class `c` as a subtype of class type `m` (solving `c`'s type parameters).
    fn downcast_ty(&mut self, m: TyId, c: u32) -> TyId {
        let Some((mc, _)) = self.class_of(m) else { return ERROR };
        let info = self.classes[c as usize].clone();
        let Some(up) = self.upcast_to(info.this_ty, mc) else { return ERROR };
        let mut map = HashMap::default();
        self.unify(up, m, &info.params, &mut map);
        let args: Vec<TyId> = info.params.iter().map(|p| map.get(p).copied().unwrap_or(UNKNOWN)).collect();
        self.types.class(c, &args)
    }

    /// Returns false if an error was reported.
    fn logical_operand(&mut self, e: ExprId, op: BinOp, rhs: ExprId) -> bool {
        let t = self.expr(e, Some(BOOL));
        if t == BOOL || t == ERROR || self.condition_ok(t) {
            return true;
        }
        let span = self.ast().expr(e).span;
        let shown = self.show(t);
        let mut d = Diagnostic::new("T0401", span, format!("`{}` needs `bool` operands, found `{shown}`", op.as_str()))
            .note("why", "no implicit truthiness: `0`, `\"\"` and `undefined` are not `false`");
        if op == BinOp::Or && self.types.has_undefined(t) && rhs != e {
            let os = self.op_span(e, rhs, "||");
            d = d.fix(Applicability::Maybe, "for a default value, use `??`", os, "??");
        }
        self.report(d);
        false
    }

    /// Checks two operands so an integer literal on either side adopts the other side's numeric type.
    fn numeric_pair(&mut self, l: ExprId, r: ExprId, exp: Option<TyId>) -> (TyId, TyId) {
        if self.is_num_literal(l) && !self.is_num_literal(r) {
            let tr = self.expr(r, exp);
            let hint = if self.types.is_num_like(tr) { Some(tr) } else { exp };
            let tl = self.expr(l, hint);
            (tl, tr)
        } else {
            let tl = self.expr(l, exp);
            let hint = if self.types.is_num_like(tl) { Some(tl) } else { None };
            let tr = self.expr(r, hint);
            (tl, tr)
        }
    }

    fn mixed_ints(&mut self, a: TyId, b: TyId, span: Span) {
        let msg = format!("mixed integer types `{}` and `{}`", self.show(a), self.show(b));
        self.report(Diagnostic::new("T0504", span, msg).note("instead", "convert one side explicitly, e.g. `int(x)`"));
    }

    /// An operator with a `decimal` operand: the other is a `decimal` or an integer (converted
    /// exactly). A float never converts implicitly: `0.1` isn't exactly a tenth.
    fn decimal_arith(&mut self, op: BinOp, l: ExprId, r: ExprId, tl: TyId, tr: TyId, span: Span) -> TyId {
        if tl == ERROR || tr == ERROR {
            return ERROR;
        }
        let comparison = matches!(op, BinOp::Lt | BinOp::Gt | BinOp::Le | BinOp::Ge);
        if !comparison && !matches!(op, BinOp::Add | BinOp::Sub | BinOp::Mul | BinOp::Div | BinOp::Rem) {
            let msg = format!("`{}` doesn't apply to `decimal`", op.as_str());
            let mut d = Diagnostic::new("T0501", span, msg);
            if op == BinOp::Pow {
                d = d.note("instead", "multiply, or convert: `decimal(Number(x) ** n)` (inexact)");
            }
            self.report(d);
            return ERROR;
        }
        let ok = |c: &Self, t: TyId| t == DECIMAL || c.types.is_int(t);
        if ok(self, tl) && ok(self, tr) {
            return DECIMAL;
        }
        let (ls, rs) = (self.show(tl), self.show(tr));
        let mut d = Diagnostic::new("T0504", span, format!("can't mix `{ls}` and `{rs}` with `{}`", op.as_str()))
            .note("why", "a `decimal` combines with decimals and integers; a float is binary, so `0.1` isn't exactly a tenth");
        let (other, ot) = if tl == DECIMAL { (r, tr) } else { (l, tl) };
        let os = self.ast().expr(other).span;
        let text = self.src(os).to_string();
        if self.types.is_float(ot) {
            let ds = self.ast().expr(if tl == DECIMAL { l } else { r }).span;
            let dtext = self.src(ds).to_string();
            d = d
                .fix(Applicability::Maybe, format!("convert the number (its shortest decimal form): `decimal({text})`"), os, format!("decimal({text})"))
                .fix(Applicability::Maybe, format!("convert the decimal: `Number({dtext})`"), ds, format!("Number({dtext})"));
        } else if ot == BIGINT {
            d = d.fix(Applicability::Maybe, format!("convert: `decimal({text})`"), os, format!("decimal({text})"));
        }
        self.report(d);
        ERROR
    }

    /// An arithmetic or bitwise operator with a `bigint` operand: both must be `bigint`, as in
    /// JavaScript (where mixing throws a TypeError).
    fn bigint_arith(&mut self, op: BinOp, l: ExprId, r: ExprId, tl: TyId, tr: TyId, span: Span) -> TyId {
        if tl == ERROR || tr == ERROR {
            return ERROR;
        }
        if tl == BIGINT && tr == BIGINT {
            return BIGINT;
        }
        self.bigint_mix(op, l, r, tl, tr, span);
        ERROR
    }

    /// `bigint` mixed with another type: the fix converts the other side (an integer literal
    /// gets its `n`).
    fn bigint_mix(&mut self, op: BinOp, l: ExprId, r: ExprId, tl: TyId, tr: TyId, span: Span) {
        let (ls, rs) = (self.show(tl), self.show(tr));
        let mut d = Diagnostic::new("T0504", span, format!("can't mix `{ls}` and `{rs}` with `{}`", op.as_str()))
            .note("why", "a `bigint` only combines with another `bigint` (JavaScript throws a TypeError)");
        let (other, ot) = if tl == BIGINT { (r, tr) } else { (l, tl) };
        let os = self.ast().expr(other).span;
        let text = self.src(os).to_string();
        if self.is_int_literal(other) && !text.contains('.') {
            d = d.fix(Applicability::Safe, format!("make it a bigint literal: `{text}n`"), os, format!("{text}n"));
        } else if self.types.is_int(ot) || self.types.is_string(ot) {
            d = d.fix(Applicability::Maybe, format!("convert: `BigInt({text})`"), os, format!("BigInt({text})"));
        } else if self.types.is_float(ot) {
            let bs = self.ast().expr(if tl == BIGINT { l } else { r }).span;
            let btext = self.src(bs).to_string();
            d = d.fix(Applicability::Maybe, format!("convert the bigint: `Number({btext})`"), bs, format!("Number({btext})"));
        }
        self.report(d);
    }

    fn arith(&mut self, op: BinOp, tl: TyId, tr: TyId, span: Span) -> TyId {
        if tl == ERROR || tr == ERROR {
            return ERROR;
        }
        if !self.types.is_numeric(tl) || !self.types.is_numeric(tr) {
            let msg = format!("can't apply `{}` to `{}` and `{}`", op.as_str(), self.show(tl), self.show(tr));
            let mut d = Diagnostic::new("T0501", span, msg);
            if self.types.has_undefined(tl) || self.types.has_undefined(tr) {
                d = d.note("why", "one side may be `undefined`; narrow it or use `?? 0`");
            }
            self.report(d);
            return ERROR;
        }
        if tl == tr {
            return tl;
        }
        let (fl, fr) = (self.types.is_float(tl), self.types.is_float(tr));
        match (fl, fr) {
            (true, true) => F64,
            (true, false) if tr == INT => tl,
            (false, true) if tl == INT => tr,
            (false, false) => {
                self.mixed_ints(tl, tr, span);
                ERROR
            }
            _ => {
                let msg = format!("can't mix `{}` and `{}` implicitly", self.show(tl), self.show(tr));
                self.report(Diagnostic::new("T0504", span, msg).note("instead", "convert the integer with `f64(x)`"));
                ERROR
            }
        }
    }

    fn equality(&mut self, l: ExprId, r: ExprId, span: Span) -> TyId {
        let (tl, tr) = if matches!(self.ast().expr(l).kind, ExprKind::Str(_) | ExprKind::Undefined) {
            let tr = self.expr(r, None);
            let tl = self.expr(l, Some(tr));
            (tl, tr)
        } else {
            let tl = self.expr(l, None);
            let tr = self.expr(r, if self.types.is_num_like(tl) { Some(tl) } else { None });
            (tl, tr)
        };
        let undefined_literal = matches!(self.ast().expr(l).kind, ExprKind::Undefined) || matches!(self.ast().expr(r).kind, ExprKind::Undefined);
        if !undefined_literal && !self.comparable(tl, tr) {
            let (ls, rs) = (self.show(tl), self.show(tr));
            let mut d = Diagnostic::new("T0502", span, format!("this comparison is always false: `{ls}` and `{rs}` have no values in common"));
            // `x === 0` on a bigint: the literal wants its `n`
            if tl == BIGINT || tr == BIGINT {
                let other = if tl == BIGINT { r } else { l };
                let os = self.ast().expr(other).span;
                let text = self.src(os).to_string();
                if self.is_int_literal(other) && !text.contains('.') {
                    d = d.fix(Applicability::Safe, format!("make it a bigint literal: `{text}n`"), os, format!("{text}n"));
                }
            }
            // `s.kind === "circel"` → suggest the literal that exists.
            let (lit, other, lit_span) = match (*self.types.get(tl), *self.types.get(tr)) {
                (_, Ty::StrLit(s)) => (Some(s), tl, self.ast().expr(r).span),
                (Ty::StrLit(s), _) => (Some(s), tr, self.ast().expr(l).span),
                _ => (None, tl, span),
            };
            if let Some(lit) = lit {
                let options: Vec<String> = self.flat_members(other).into_iter().filter_map(|m| match self.types.get(m) {
                    Ty::StrLit(s) => Some(self.name(*s).to_string()),
                    _ => None,
                }).collect();
                if !options.is_empty() {
                    let lit_text = self.name(lit).to_string();
                    let quoted: Vec<String> = options.iter().map(|o| format!("\"{o}\"")).collect();
                    d = d.note("valid values", quoted.join(", "));
                    let sug = similar(&lit_text, options.iter().map(|s| s.as_str()));
                    if let Some(first) = sug.first() {
                        d = d.fix(Applicability::Maybe, format!("use \"{first}\""), lit_span, format!("\"{first}\""));
                    }
                }
            }
            self.report(d);
        }
        BOOL
    }

    fn assign(&mut self, op: AssignOp, target: ExprId, value: ExprId, span: Span) -> TyId {
        let t = self.assign_inner(op, target, value, span);
        // After the value: `cur = cur.left` reads the narrowed `cur.left` first.
        self.invalidate_place(target);
        // A field of a class instance may be reached through other references too.
        if !matches!(self.ast().expr(target).kind, ExprKind::Ident(_)) {
            self.invalidate_heap();
        }
        t
    }

    fn assign_inner(&mut self, op: AssignOp, target: ExprId, value: ExprId, span: Span) -> TyId {
        // `js.name = v`, `js[k] op= v`: a JavaScript property (assigned by JavaScript's rules)
        let js_obj = match self.ast().expr(target).kind {
            ExprKind::Member { obj, .. } | ExprKind::Index { obj, .. } => Some(obj),
            _ => None,
        };
        if let Some(obj) = js_obj
            && self.expr_type_peek(obj) == Some(JS)
        {
            self.expr(target, None);
            let v = self.expr(value, Some(JS));
            if !self.js_convertible(v) {
                let shown = self.show(v);
                let s = self.ast().expr(value).span;
                self.report(Diagnostic::new("T0901", s, format!("a `{shown}` can't be passed to JavaScript")));
            }
            let _ = (op, span);
            return JS;
        }
        let Some(place) = self.place(target, false) else {
            self.expr(value, None);
            return ERROR;
        };
        let vspan = self.ast().expr(value).span;
        match op {
            AssignOp::Assign => {
                let tv = self.expr(value, Some(place.ty));
                if !self.try_promote(&place, tv) {
                    self.expect_assignable(tv, place.ty, vspan, None);
                }
                if let Some(local) = place.local
                    && self.assignable(tv, place.ty) && matches!(self.types.get(self.local(local).ty), Ty::Union(_)) {
                        self.narrow(local, tv);
                    }
            }
            AssignOp::Op(BinOp::Nullish) => {
                if !self.types.has_undefined(place.ty) && place.ty != ERROR {
                    let msg = format!("`??=` needs an optional target, found `{}`", self.show(place.ty));
                    self.report(Diagnostic::new("T0503", span, msg));
                }
                let inner = self.types.without_undefined(place.ty);
                let tv = self.expr(value, Some(inner));
                self.expect_assignable(tv, inner, vspan, None);
                if let Some(local) = place.local {
                    self.narrow(local, inner);
                }
            }
            AssignOp::Op(BinOp::And | BinOp::Or) => {
                self.report(Diagnostic::new("X0026", span, "`&&=` and `||=` are not supported; write the `if` explicitly"));
                self.expr(value, None);
            }
            AssignOp::Op(bop) => {
                let hint = if self.types.is_num_like(place.ty) { Some(place.ty) } else { None };
                let tv = self.expr(value, hint);
                let result = if place.ty == DECIMAL || (tv == DECIMAL && !self.types.is_string(place.ty)) {
                    self.decimal_arith(bop, target, value, place.ty, tv, span)
                } else if place.ty == BIGINT || (tv == BIGINT && !self.types.is_string(place.ty)) {
                    if bop == BinOp::UShr {
                        self.report(Diagnostic::new("T0501", span, "`>>>=` doesn't apply to `bigint`: a bigint has no fixed width"));
                        ERROR
                    } else {
                        self.bigint_arith(bop, target, value, place.ty, tv, span)
                    }
                } else if bop == BinOp::Add && self.types.is_string(place.ty) {
                    if !self.printable(tv) {
                        let msg = format!("can't concatenate `{}` to a string", self.show(tv));
                        self.report(Diagnostic::new("T0501", vspan, msg));
                    }
                    STR
                } else if matches!(bop, BinOp::BitAnd | BinOp::BitOr | BinOp::BitXor | BinOp::Shl | BinOp::Shr | BinOp::UShr) {
                    if place.ty != ERROR && tv != ERROR && (!self.types.is_int(place.ty) || !self.types.is_int(tv)) {
                        let msg = format!("`{}=` needs integers, found `{}` and `{}`", bop.as_str(), self.show(place.ty), self.show(tv));
                        self.report(Diagnostic::new("T0501", span, msg));
                    }
                    place.ty
                } else {
                    let r = self.arith(bop, place.ty, tv, span);
                    if bop == BinOp::Div && r != ERROR { if r == F32 { F32 } else { F64 } } else { r }
                };
                if !self.try_promote(&place, result)
                    && result != ERROR && !self.assignable(result, place.ty) {
                        let msg = format!("`{}=` produces `{}`, which can't be stored in `{}`", bop.as_str(), self.show(result), self.show(place.ty));
                        let mut d = Diagnostic::new("T0001", span, msg);
                        if self.types.is_int(place.ty) && self.types.is_float(result) {
                            d = d.note("why", "the variable is an `int`; declare it as `f64` (e.g. `let x = 0.0`) or round the result");
                        }
                        self.report(d);
                    }
            }
        }
        place.ty
    }

    /// `let x = 0; x += 0.5` — records that `x` should be an `f64` and re-checks the function.
    fn try_promote(&mut self, place: &Place, value: TyId) -> bool {
        let Some(local) = place.local else { return false };
        let l = self.local(local);
        if l.promotable && l.ty == INT && self.types.is_float(value) {
            let key = l.span.start;
            self.fcx().pending.insert(key);
            return true;
        }
        false
    }

    fn root_of(&self, e: ExprId) -> Root {
        match &self.ast().expr(e).kind {
            ExprKind::Paren(x) | ExprKind::NonNull(x) => self.root_of(*x),
            ExprKind::This => Root::Heap,
            ExprKind::Member { obj, .. } | ExprKind::Index { obj, .. } => {
                if self.fcx.last().map(|f| f.class_valued.contains(obj)).unwrap_or(false) {
                    return Root::Heap;
                }
                self.root_of(*obj)
            }
            ExprKind::Ident(s) => match self.lookup(*s) {
                // (a script-local standing in for a top-level binding is still a global)
                Some((id, _)) => match self.local(id).global {
                    Some((m, i)) => Root::ModuleConst(m, i),
                    None => Root::Local(id),
                },
                None => match self.scopes[self.cur as usize].values.get(s).map(|d| d.0) {
                    Some(Decl::Const(m, i)) => Root::ModuleConst(m, i),
                    _ => Root::Temp,
                },
            },
            _ => Root::Temp,
        }
    }

    /// Reports an error if the value rooted at `e` can't be modified in place.
    pub(super) fn check_mutable_root(&mut self, e: ExprId, span: Span) -> Option<LocalId> {
        self.check_mutable_root_ext(e, span, false)
    }

    /// `allow_temp`: mutating a temporary (`xs.slice().sort()`) is fine for method calls.
    pub(super) fn check_mutable_root_ext(&mut self, e: ExprId, span: Span, allow_temp: bool) -> Option<LocalId> {
        match self.root_of(e) {
            Root::Temp if allow_temp => None,
            Root::Heap => None,
            Root::Local(id) => {
                let (kind, name, lspan) = {
                    let l = self.local(id);
                    (l.kind, l.name, l.span)
                };
                let n = self.name(name).to_string();
                match kind {
                    LocalKind::Param => {
                        self.report(
                            Diagnostic::new("V0101", span, format!("can't modify parameter `{n}`: parameters are read-only"))
                                .note("to change the caller's value", format!("declare `inout {n}` and call with `&arg`"))
                                .note("to change a local copy", format!("`let copy = {n}` and modify `copy`"))
                                .fix(Applicability::Maybe, format!("make `{n}` an `inout` parameter"), lspan.empty_at_start(), "inout "),
                        );
                        None
                    }
                    LocalKind::LoopVar => {
                        self.report(
                            Diagnostic::new("V0104", span, format!("can't modify loop variable `{n}`: it is a copy of the element"))
                                .note("why", "changes to the copy would not reach the collection (Tov collections hold values)")
                                .note("instead", "loop over indices and assign `xs[i] = ...`, or build a new array with `map`"),
                        );
                        None
                    }
                    _ => {
                        self.fcx().mutations.push((id, span));
                        Some(id)
                    }
                }
            }
            // Module state is mutable, as in TypeScript (`const` only fixes the binding).
            Root::ModuleConst(m, i) => {
                if let Some(f) = self.facts_mut() {
                    f.mutated_globals.insert((m, i));
                }
                None
            }
            Root::Temp => {
                self.report(Diagnostic::new("V0103", span, "can't modify a temporary value").note("instead", "assign it to a variable first"));
                None
            }
        }
    }

    pub(super) fn place(&mut self, e: ExprId, _for_ref: bool) -> Option<Place> {
        let node = self.ast().expr(e);
        let span = node.span;
        match &node.kind {
            ExprKind::Paren(x) | ExprKind::NonNull(x) => self.place(*x, _for_ref),
            ExprKind::Ident(sym) => {
                if let Some((id, ty)) = self.lookup(*sym).filter(|(id, _)| self.local(*id).global.is_none()) {
                    let key = self.local(id).span.start;
                    self.rec_ident(e, IdentFact::Local(key));
                    let declared = self.fcx.last().unwrap().locals[id].ty;
                    if let Some(f) = self.facts_mut() {
                        f.expr_ty[e as usize] = declared;
                    }
                    let (kind, kw_span) = (self.local(id).kind, self.local(id).kw_span);
                    let declared = self.local(id).ty;
                    let n = self.name(*sym).to_string();
                    if kind == LocalKind::Const {
                        let mut d = if _for_ref {
                            Diagnostic::new("V0001", span, format!("`{n}` is a `const`, so it can't be passed as `inout`"))
                                .note("why", "the callee may replace the whole value, which would change a constant")
                        } else {
                            Diagnostic::new("V0001", span, format!("can't reassign constant `{n}`"))
                        };
                        if let Some(kw) = kw_span {
                            d = d.fix(Applicability::Safe, format!("declare `{n}` with `let`"), kw, "let");
                        }
                        self.report(d);
                        return None;
                    }
                    if kind == LocalKind::LoopVar {
                        self.report(Diagnostic::new("V0001", span, format!("can't reassign loop variable `{n}`")));
                        return None;
                    }
                    let _ = ty;
                    return Some(Place { ty: declared, local: Some(id), root: Some(id) });
                }
                let n = self.name(*sym).to_string();
                match self.scopes[self.cur as usize].values.get(sym).map(|d| d.0) {
                    Some(Decl::Const(cm, ci)) => {
                        let ItemKind::Const { mutable, name_span, .. } = &self.modules[cm as usize].ast.items[ci as usize].kind else { return None };
                        if !*mutable || cm != self.cur {
                            let mut d = Diagnostic::new("V0102", span, format!("can't reassign module constant `{n}`"));
                            if cm == self.cur {
                                let kw = self.modules[cm as usize].ast.items[ci as usize].span;
                                let kw = Span::new(kw.file, kw.start, kw.start + if self.modules[cm as usize].ast.items[ci as usize].exported { 12 } else { 5 });
                                d = d.fix(Applicability::Safe, format!("declare `{n}` with `let`"), kw, if self.modules[cm as usize].ast.items[ci as usize].exported { "export let" } else { "let" });
                            } else {
                                d = d.note("why", "imported bindings are read-only; export a function that changes it");
                            }
                            let _ = name_span;
                            self.report(d);
                            return None;
                        }
                        let ty = self.const_type(cm, ci);
                        self.rec_ident(e, IdentFact::Const(cm, ci));
                        if let Some(f) = self.facts_mut() {
                            f.expr_ty[e as usize] = ty;
                            f.mutated_globals.insert((cm, ci));
                        }
                        // The assignment narrows the script-local standing in for it (as a
                        // local's would); the place itself is still the global.
                        let local = (cm == self.cur)
                            .then(|| self.fcx().locals.iter().position(|l| l.global == Some((cm, ci))))
                            .flatten();
                        Some(Place { ty, local, root: local })
                    }
                    Some(Decl::Fn(..)) => {
                        self.report(Diagnostic::new("V0105", span, format!("can't assign to function `{n}`")));
                        None
                    }
                    _ => {
                        self.ident(e, *sym, span);
                        None
                    }
                }
            }
            ExprKind::Member { obj, name, name_span, optional } => {
                if *optional {
                    self.report(Diagnostic::new("V0103", span, "can't assign through `?.`").note("instead", "narrow the value first: `if (x !== undefined) x.field = v`"));
                    return None;
                }
                let n = self.name(*name).to_string();
                let t = self.expr(*obj, None);
                if n == "length" && matches!(self.types.get(t), Ty::Array(_)) {
                    self.report(Diagnostic::new("X0034", span, "assigning an array's `length` is not supported").note("instead", "reassign: `xs = []`, or `xs = xs.slice(0, n)`"));
                    return None;
                }
                if let Some(cls) = self.class_of(self.types_without_undef(t)) {
                    if self.types.has_undefined(t) {
                        let d = self.possibly_undefined(*obj, span);
                        self.report(d);
                    }
                    return self.class_field_place(self.types_without_undef(t), cls.0, *obj, *name, *name_span);
                }
                let ft = self.member_of(t, *name, *name_span, span, *obj)?;
                let root = self.check_mutable_root(*obj, span)?;
                Some(Place { ty: ft, local: None, root: Some(root) })
            }
            ExprKind::Index { obj, index, optional } => {
                if *optional {
                    self.report(Diagnostic::new("V0103", span, "can't assign through `?.[]`"));
                    return None;
                }
                let t = self.expr(*obj, None);
                let elem = match *self.types.get(t) {
                    Ty::Array(e) => {
                        self.index_type(*index);
                        e
                    }
                    Ty::Error => return None,
                    Ty::Map(..) => {
                        let (o, i) = (self.src(self.ast().expr(*obj).span).to_string(), self.src(self.ast().expr(*index).span).to_string());
                        self.report(
                            Diagnostic::new("X0017", span, "maps are not indexed with `[]`")
                                .note("instead", format!("`{o}.set({i}, value)`")),
                        );
                        return None;
                    }
                    _ => {
                        let msg = format!("can't index into `{}`", self.show(t));
                        self.report(Diagnostic::new("T0110", span, msg));
                        return None;
                    }
                };
                let root = self.check_mutable_root(*obj, span)?;
                Some(Place { ty: elem, local: None, root: Some(root) })
            }
            _ => {
                self.expr(e, None);
                self.report(Diagnostic::new("V0103", span, "this expression can't be assigned to"));
                None
            }
        }
    }

    /// `obj.name = ...` on a class instance: a field (not a getter or method), visible, and
    /// `readonly` only inside the owner's constructor through `this`.
    fn class_field_place(&mut self, ty: TyId, c: u32, obj: ExprId, name: Sym, name_span: Span) -> Option<Place> {
        let n = self.name(name).to_string();
        match self.class_member(ty, name) {
            Some(super::class::ClassMemberRef::Field(f)) => {
                self.check_visible(f.owner, f.vis, name, name_span);
                if f.readonly {
                    let in_owner_ctor = self.fcx.last().map(|x| x.ctor && x.class == Some(f.owner)).unwrap_or(false) && matches!(self.ast().expr(obj).kind, ExprKind::This);
                    if !in_owner_ctor {
                        self.report(
                            Diagnostic::new("T0811", name_span, format!("`{n}` is `readonly`"))
                                .note("why", "readonly fields are set once, in the constructor (or by their initializer)"),
                        );
                    }
                }
                Some(Place { ty: f.ty, local: None, root: None })
            }
            Some(super::class::ClassMemberRef::Getter(_)) => {
                self.report(Diagnostic::new("U0019", name_span, format!("`{n}` is a getter; setters are not supported yet")));
                None
            }
            Some(super::class::ClassMemberRef::Method(_)) => {
                self.report(Diagnostic::new("V0105", name_span, format!("can't assign to method `{n}`")));
                None
            }
            None => {
                let cls = self.class_names[c as usize].clone();
                let names = self.class_member_names(ty);
                let mut d = Diagnostic::new("T0107", name_span, format!("class `{cls}` has no field `{n}`"));
                let sug = similar(&n, names.iter().map(|s| s.as_str()));
                if let Some(first) = sug.first() {
                    d = d.note("did you mean", sug.join(", ")).fix(Applicability::Maybe, format!("use `{first}`"), name_span, first.to_string());
                }
                d = d.note("note", "classes have fixed fields: declare every field in the class body");
                self.report(d);
                None
            }
        }
    }

    fn index_type(&mut self, index: ExprId) {
        let ti = self.expr(index, Some(INT));
        if ti == ERROR || self.types.is_int(ti) {
            return;
        }
        let span = self.ast().expr(index).span;
        let text = self.src(span).to_string();
        let mut d = Diagnostic::new("T0111", span, format!("array index must be an integer, found `{}`", self.show(ti)));
        if self.types.is_float(ti) {
            d = d.fix(Applicability::Maybe, format!("use `Math.floor({text})`"), span, format!("Math.floor({text})"));
        }
        self.report(d);
    }

    fn index(&mut self, obj: ExprId, index: ExprId, optional: bool, span: Span) -> TyId {
        let t = self.expr(obj, None);
        let base = if optional {
            self.types.without_undefined(t)
        } else {
            if self.types.has_undefined(t) {
                let d = self.possibly_undefined(obj, span);
                self.report(d);
            }
            self.types.without_undefined(t)
        };
        if base == JS {
            let it = self.expr(index, None);
            if !(self.types.is_numeric(it) || self.assignable(it, STR) || it == JS || it == ERROR) {
                let shown = self.show(it);
                let s = self.ast().expr(index).span;
                self.report(Diagnostic::new("T0902", s, format!("a JavaScript property key must be a number or a string, found `{shown}`")));
            }
            return JS;
        }
        let result = match *self.types.get(base) {
            Ty::Error => return ERROR,
            Ty::Array(e) => {
                self.index_type(index);
                self.types.optional(e)
            }
            Ty::Map(..) => {
                self.expr(index, None);
                let (o, i) = (self.src(self.ast().expr(obj).span).to_string(), self.src(self.ast().expr(index).span).to_string());
                self.report(
                    Diagnostic::new("X0017", span, "maps are not indexed with `[]`")
                        .fix(Applicability::Safe, format!("use `{o}.get({i})`"), span, format!("{o}.get({i})")),
                );
                return ERROR;
            }
            Ty::Str | Ty::StrLit(_) => {
                self.expr(index, None);
                self.report(
                    Diagnostic::new("X0035", span, "strings can't be indexed by position (they are UTF-8)")
                        .note("instead", "`s.chars()[i]` for the i-th character, or `s.slice(start, end)`"),
                );
                return ERROR;
            }
            Ty::Record(fields) => {
                if let ExprKind::Str(key) = self.ast().expr(index).kind {
                    let key = &self.name(key).to_string();
                    let o = self.src(self.ast().expr(obj).span).to_string();
                    if self.types.fields(fields).iter().any(|f| self.name(f.name) == key) {
                        let key = key.clone();
                        self.report(
                            Diagnostic::new("X0017", span, "records are accessed with `.field`, not `[\"field\"]`")
                                .fix(Applicability::Safe, format!("use `{o}.{key}`"), span, format!("{o}.{key}")),
                        );
                        return ERROR;
                    }
                }
                self.expr(index, None);
                self.report(Diagnostic::new("X0017", span, "records can't be indexed").note("instead", "use `.field`, or a `Map<K, V>` for dynamic keys"));
                return ERROR;
            }
            _ => {
                self.expr(index, None);
                let msg = format!("can't index into `{}`", self.show(base));
                self.report(Diagnostic::new("T0110", span, msg));
                return ERROR;
            }
        };
        if optional { self.types.optional(result) } else { result }
    }

    pub(super) fn possibly_undefined(&mut self, obj: ExprId, span: Span) -> Diagnostic {
        let ospan = self.ast().expr(obj).span;
        let text = self.src(ospan).to_string();
        let rest = self.sm.get(span.file).text[ospan.end as usize..span.end as usize].to_string();
        let (chain, insert) = if rest.starts_with('.') { (format!("{text}?{rest}"), "?") } else { (format!("{text}?.{rest}"), "?.") };
        Diagnostic::new("T0120", ospan, format!("`{text}` may be `undefined`"))
            .note("narrow first", format!("`if ({text} !== undefined) {{ ... }}`"))
            .fix(Applicability::Maybe, format!("optional chaining: `{chain}`"), ospan.empty_at_end(), insert)
            .fix(Applicability::Maybe, format!("assert defined (traps if not): `{text}!{rest}`"), ospan.empty_at_end(), "!")
    }

    fn member(&mut self, e: ExprId, obj: ExprId, name: Sym, name_span: Span, optional: bool, span: Span) -> TyId {
        // Namespaces: `ns.value`, `Math.PI`.
        if let ExprKind::Ident(s) = self.ast().expr(obj).kind
            && self.lookup(s).is_none() {
                match self.scopes[self.cur as usize].values.get(&s).map(|d| d.0) {
                    Some(Decl::Ns(m)) => {
                        let scope = &self.scopes[m as usize];
                        match scope.values.get(&name).map(|d| d.0) {
                            Some(Decl::Fn(fm, fi)) => {
                                if let Some(f) = self.facts_mut() {
                                    f.members.insert(e, MemberFact::NsFn(fm, fi));
                                }
                            }
                            Some(Decl::Const(cm, ci)) => {
                                if let Some(f) = self.facts_mut() {
                                    f.members.insert(e, MemberFact::NsConst(cm, ci));
                                }
                            }
                            _ => {}
                        }
                        return self.ns_value(m, name, name_span);
                    }
                    None => {
                        let text = self.name(s).to_string();
                        if self.is_ns(&text) {
                            if let Some(f) = self.facts_mut() {
                                f.members.insert(e, MemberFact::MathConst);
                            }
                            return self.builtin_ns_member(&text, name, name_span);
                        }
                    }
                    _ => {}
                }
            }
        // `C.NAME`: a static readonly field.
        if let ExprKind::Ident(s) = self.ast().expr(obj).kind
            && self.lookup(s).is_none()
            && let Some(Decl::Class(c)) = self.scopes[self.cur as usize].values.get(&s).map(|d| d.0)
        {
            return self.static_member(e, c, name, name_span);
        }
        if matches!(self.ast().expr(obj).kind, ExprKind::Super) {
            let n = self.name(name).to_string();
            self.report(Diagnostic::new("T0801", span, format!("`super.{n}` must be called; base fields are read through `this.{n}`")));
            return ERROR;
        }
        let t = self.expr(obj, None);
        if t == ERROR {
            return ERROR;
        }
        if self.types.without_undefined(t) == JS {
            // a JavaScript property (`undefined` if there's none)
            return JS;
        }
        // `req.params` in a `routes` handler: `{ id: string }` for "/users/:id".
        if self.name(name) == "params"
            && let ExprKind::Ident(s) = self.ast().expr(obj).kind
            && let Some((id, _)) = self.lookup(s)
            && let Some(&(_, key)) = self.fcx.last().unwrap().route_locals.iter().find(|(l, _)| *l == id)
        {
            let path = self.name(key).to_string();
            let mut fields = Vec::new();
            let mut order = Vec::new();
            for seg in path.split('/') {
                if let Some(p) = seg.strip_prefix(':')
                    && let Some(sym) = self.interner.lookup(p)
                    && !order.contains(&sym)
                {
                    fields.push(Field { name: sym, ty: STR, optional: false });
                    order.push(sym);
                }
            }
            let rt = self.types.record(fields);
            self.types.note_field_order(rt, order);
            if let Some(f) = self.facts_mut() {
                f.members.insert(e, MemberFact::RouteParams(rt));
            }
            return rt;
        }
        let base = if optional {
            self.types.without_undefined(t)
        } else {
            if self.types.has_undefined(t) {
                let d = self.possibly_undefined(obj, span);
                self.report(d);
            }
            self.types.without_undefined(t)
        };
        // `process.env.NAME`: an environment variable.
        if let Ty::BuiltinNs(s) = *self.types.get(base)
            && self.name(s) == "env"
        {
            if let Some(f) = self.facts_mut() {
                f.members.insert(e, MemberFact::Env(name));
            }
            return self.types.optional(STR);
        }
        // Class members: visibility, and getters are calls.
        let mut getter = false;
        for m in self.flat_members(base) {
            if self.class_of(m).is_some() {
                match self.class_member(m, name) {
                    Some(super::class::ClassMemberRef::Field(f)) => {
                        self.check_visible(f.owner, f.vis, name, name_span);
                    }
                    Some(super::class::ClassMemberRef::Getter(g)) => {
                        self.check_visible(g.owner, g.vis, name, name_span);
                        getter = true;
                    }
                    Some(super::class::ClassMemberRef::Method(_)) => {
                        let n = self.name(name).to_string();
                        self.report(
                            Diagnostic::new("T0203", name_span, format!("method `{n}` must be called: `{n}(...)`"))
                                .note("instead", format!("to pass it as a function, wrap it: `(x) => obj.{n}(x)`")),
                        );
                        return ERROR;
                    }
                    None => {}
                }
            }
        }
        if getter {
            self.invalidate_heap();
        }
        let Some(ft) = self.member_of(base, name, name_span, span, obj) else { return ERROR };
        if !optional
            && let Some((target, _)) = self.target_of(obj)
        {
            let mut path = target.path.clone();
            path.push(name);
            if let Some(t) = self.lookup_path(target.local, &path) {
                return t;
            }
        }
        if optional { self.types.optional(ft) } else { ft }
    }

    /// `C.NAME` for a static readonly field of class `c`.
    fn static_member(&mut self, e: ExprId, c: u32, name: Sym, name_span: Span) -> TyId {
        self.resolve_class(c);
        let info = self.classes[c as usize].clone();
        if let Some(f) = info.static_fields.iter().find(|f| f.name == name) {
            self.check_visible(f.owner, f.vis, name, name_span);
            if let (Some(fct), Some(mi)) = (self.facts_mut(), f.member) {
                fct.members.insert(e, MemberFact::StaticField(c, mi));
            }
            return f.ty;
        }
        let n = self.name(name).to_string();
        let cls = self.class_names[c as usize].clone();
        if info.statics.iter().any(|m| m.name == name) {
            self.report(Diagnostic::new("T0203", name_span, format!("static method `{cls}.{n}` must be called")));
            return ERROR;
        }
        let names: Vec<String> = info.static_fields.iter().map(|f| f.name).chain(info.statics.iter().map(|m| m.name)).map(|s| self.name(s).to_string()).collect();
        let mut d = Diagnostic::new("T0107", name_span, format!("class `{cls}` has no static member `{n}`"));
        let sug = similar(&n, names.iter().map(|s| s.as_str()));
        if let Some(first) = sug.first() {
            d = d.note("did you mean", sug.join(", ")).fix(Applicability::Maybe, format!("use `{first}`"), name_span, first.to_string());
        }
        if info.fields.iter().any(|f| f.name == name) || info.methods.iter().any(|m| m.name == name) {
            d = d.note("note", format!("`{n}` is an instance member: use it on an instance (`new {cls}(...).{n}`, or `this.{n}` inside the class)"));
        }
        self.report(d);
        ERROR
    }

    fn ns_value(&mut self, m: u32, name: Sym, name_span: Span) -> TyId {
        let scope = &self.scopes[m as usize];
        let decl = scope.values.get(&name).filter(|_| scope.exported_values.contains(&name)).map(|d| d.0);
        match decl {
            Some(Decl::Fn(fm, fi)) => match self.fn_sig(fm, fi) {
                Some(sig) if sig.tparams.is_empty() => self.types.func(sig.params, sig.ret),
                Some(_) => {
                    let n = self.name(name).to_string();
                    self.report(Diagnostic::new("T0304", name_span, format!("generic function `{n}` must be called")));
                    ERROR
                }
                None => ERROR,
            },
            Some(Decl::Const(cm, ci)) => self.const_type(cm, ci),
            _ => {
                let n = self.name(name).to_string();
                let mname = self.modules[m as usize].name.clone();
                let exports: Vec<String> = self.scopes[m as usize].exported_values.iter().map(|&s| self.name(s).to_string()).collect();
                let mut d = Diagnostic::new("N0102", name_span, format!("`{mname}` has no exported value `{n}`"));
                let sug = similar(&n, exports.iter().map(|s| s.as_str()));
                if let Some(first) = sug.first() {
                    d = d.note("did you mean", sug.join(", ")).fix(Applicability::Maybe, format!("use `{first}`"), name_span, first.to_string());
                }
                self.report(d);
                ERROR
            }
        }
    }

    /// Type of `base.name` for a non-optional base.
    pub(super) fn member_of(&mut self, base: TyId, name: Sym, name_span: Span, span: Span, obj: ExprId) -> Option<TyId> {
        let n = self.name(name).to_string();
        let members = self.flat_members(base);
        let mut found = Vec::new();
        let mut lacking = Vec::new();
        for &m in &members {
            match self.field_of(m, name) {
                Some(t) => found.push(t),
                None => lacking.push(m),
            }
        }
        if lacking.is_empty() && !found.is_empty() {
            return Some(self.types.union(&found));
        }
        if let Some(msg) = builtins::member_hint(&self.types, base, &n) {
            self.report(msg.into_diag(name_span));
            return None;
        }
        if builtins::is_method(self, base, &n) {
            self.report(Diagnostic::new("T0203", name_span, format!("method `{n}` must be called: `{n}(...)`")));
            return None;
        }
        let shown = self.show(base);
        if !found.is_empty() {
            // Some union members have the field: suggest narrowing.
            let obj_text = self.src(self.ast().expr(obj).span).to_string();
            let mut d = Diagnostic::new("T0108", name_span, format!("not every variant of `{shown}` has field `{n}`"));
            let disc = self.discriminant_values(&lacking);
            if !disc.is_empty() {
                d = d.note("missing on", disc.join(", "));
            }
            d = d.note("narrow first", format!("`switch ({obj_text}.kind)` or `if ({obj_text}.kind === ...)`"));
            self.report(d);
            let _ = span;
            return None;
        }
        let names = self.field_names(base);
        let is_record = self.flat_members(base).iter().all(|&m| matches!(self.types.get(m), Ty::Record(_) | Ty::Interface(..) | Ty::Class(..)));
        let what = if is_record { "field" } else { "member" };
        let mut d = Diagnostic::new("T0107", name_span, format!("`{shown}` has no {what} `{n}`"));
        let sug = similar(&n, names.iter().map(|s| s.as_str()));
        if let Some(first) = sug.first() {
            d = d.note("did you mean", sug.join(", ")).fix(Applicability::Maybe, format!("use `{first}`"), name_span, first.to_string());
        }
        if !names.is_empty() {
            d = d.note("available", names.join(", "));
        }
        self.report(d);
        None
    }

    fn discriminant_values(&mut self, members: &[TyId]) -> Vec<String> {
        let kind = self.syms.kind;
        let mut out = Vec::new();
        for &m in members {
            if let Ty::Record(fs) = *self.types.get(m)
                && let Some(f) = self.types.fields(fs).iter().find(|f| f.name == kind)
                    && let Ty::StrLit(s) = self.types.get(f.ty) {
                        out.push(format!("\"{}\"", self.name(*s)));
                    }
        }
        out
    }

    fn field_of(&mut self, ty: TyId, name: Sym) -> Option<TyId> {
        let n = self.name(name).to_string();
        match *self.types.get(ty) {
            Ty::Error => Some(ERROR),
            Ty::Record(fs) => self.types.fields(fs).iter().find(|f| f.name == name).map(|f| f.ty),
            Ty::Interface(i, args) => {
                let args = self.types.tys(args).to_vec();
                self.iface_fields_inst(i, &args).into_iter().find(|f| f.name == name).map(|f| f.ty)
            }
            Ty::Param(p) => {
                let b = self.gparam(p).bound?;
                self.field_of(b, name)
            }
            Ty::Class(..) => match self.class_member(ty, name)? {
                super::class::ClassMemberRef::Field(f) => Some(f.ty),
                super::class::ClassMemberRef::Getter(g) => Some(self.method_ret(&g)),
                super::class::ClassMemberRef::Method(_) => None,
            },
            _ => builtins::property(&self.types, ty, &n),
        }
    }

    fn field_names(&mut self, ty: TyId) -> Vec<String> {
        let mut out = Vec::new();
        for m in self.flat_members(ty) {
            match *self.types.get(m) {
                Ty::Record(fs) => out.extend(self.types.fields(fs).iter().map(|f| self.name(f.name).to_string())),
                Ty::Interface(i, args) => {
                    let args = self.types.tys(args).to_vec();
                    let fs = self.iface_fields_inst(i, &args);
                    out.extend(fs.iter().map(|f| self.name(f.name).to_string()));
                }
                Ty::Class(..) => out.extend(self.class_member_names(m)),
                _ => out.extend(builtins::property_names(self, m).into_iter().map(|s| s.to_string())),
            }
        }
        out.sort();
        out.dedup();
        out
    }

    /// std/http's `BunRequest` (a `routes` handler's request).
    fn is_route_request(&self, t: TyId) -> bool {
        match *self.types.get(t) {
            Ty::Class(c, _) => self.class_names[c as usize] == "BunRequest" && self.modules[self.classes[c as usize].module as usize].std,
            _ => false,
        }
    }

    /// The `Record<string, V>` (string-keyed map) an object literal builds, if the context wants one.
    fn map_literal_target(&mut self, exp: TyId) -> Option<TyId> {
        let exp = self.unfold(exp);
        let exp = self.types.without_undefined(exp);
        let members: Vec<TyId> = match self.types.get(exp) {
            Ty::Union(ms) => self.types.tys(*ms).to_vec(),
            _ => vec![exp],
        };
        let mut map = None;
        for &mt in &members {
            match self.types.get(mt) {
                Ty::Map(k, _) if *k == STR => map = Some(mt),
                Ty::Record(_) | Ty::Interface(..) => return None,
                _ => {}
            }
        }
        map
    }

    fn object(&mut self, fields: &[crate::ast::ObjField], exp: Option<TyId>, span: Span) -> TyId {
        let mut seen = HashSet::default();
        for f in fields {
            if !seen.insert(f.name) {
                let n = self.name(f.name).to_string();
                self.report(Diagnostic::new("N0006", f.name_span, format!("duplicate field `{n}`")));
            }
        }
        // `{ "Content-Type": v }` for a `Record<string, V>`: a map literal.
        if let Some(mt) = exp.and_then(|e| self.map_literal_target(e)) {
            let Ty::Map(_, vt) = *self.types.get(mt) else { unreachable!() };
            for f in fields {
                let key = self.name(f.name).to_string();
                let saved = self.route_key.take();
                if key.contains("/:") {
                    self.route_key = Some(f.name);
                }
                let t = self.expr(f.value, Some(vt));
                self.route_key = saved;
                let vspan = self.ast().expr(f.value).span;
                self.expect_assignable(t, vt, vspan, Some(format!("key \"{key}\"")));
            }
            return mt;
        }
        for f in fields {
            let text = self.name(f.name);
            let ident = text.chars().next().is_some_and(|c| c.is_alphabetic() || c == '_' || c == '$') && text.chars().all(|c| c.is_alphanumeric() || c == '_' || c == '$');
            if f.quoted && !ident {
                self.report(
                    Diagnostic::new("X0017", f.name_span, format!("\"{text}\" isn't a field name: records have identifier field names"))
                        .note("instead", "for dynamic keys, give the value a `Record<string, V>` (or `Map<string, V>`) type"),
                );
            }
        }
        let target = exp.and_then(|e| self.pick_record(fields, e, span));
        if let Some((ERROR, _)) = target {
            return ERROR;
        }
        let Some((target_ty, tfields)) = target else {
            if exp.map(|e| self.types.get(e) == &Ty::Error).unwrap_or(false) {
                for f in fields {
                    self.expr(f.value, None);
                }
                return ERROR;
            }
            // No usable context: infer a record type, widening string literals as TS does.
            let mut out = Vec::new();
            for f in fields {
                let t = self.expr(f.value, None);
                let t = self.types.widen(t);
                out.push(Field { name: f.name, ty: t, optional: false });
            }
            out.dedup_by_key(|f| f.name);
            let order: Vec<Sym> = out.iter().map(|f| f.name).collect();
            let t = self.types.record(out);
            self.types.note_field_order(t, order);
            return t;
        };
        let mut provided = HashSet::default();
        for f in fields {
            match tfields.iter().find(|tf| tf.name == f.name) {
                Some(tf) => {
                    provided.insert(f.name);
                    let t = self.expr(f.value, Some(tf.ty));
                    let fname = self.name(f.name).to_string();
                    let vspan = self.ast().expr(f.value).span;
                    self.expect_assignable(t, tf.ty, vspan, Some(format!("field `{fname}`")));
                }
                None => {
                    self.expr(f.value, None);
                    let n = self.name(f.name).to_string();
                    let missing: Vec<String> = tfields.iter().filter(|tf| !fields.iter().any(|f| f.name == tf.name)).map(|tf| self.name(tf.name).to_string()).collect();
                    let shown = self.show(target_ty);
                    let mut d = Diagnostic::new("T0102", f.name_span, format!("`{shown}` has no field `{n}`"));
                    let sug = similar(&n, missing.iter().map(|s| s.as_str()));
                    if let Some(first) = sug.first() {
                        d = d.fix(Applicability::Maybe, format!("rename to `{first}`"), f.name_span, first.to_string());
                    }
                    let all: Vec<String> = tfields.iter().map(|tf| self.name(tf.name).to_string()).collect();
                    d = d.note("fields", all.join(", "));
                    self.report(d);
                }
            }
        }
        let missing: Vec<&Field> = tfields.iter().filter(|tf| !tf.optional && !provided.contains(&tf.name)).collect();
        // Don't report missing fields when a misspelled field already explains it.
        let misspelled = fields.iter().any(|f| !tfields.iter().any(|tf| tf.name == f.name));
        if !missing.is_empty() && !misspelled {
            let names: Vec<String> = missing.iter().map(|f| format!("`{}`", self.name(f.name))).collect();
            let insert: Vec<String> = missing.iter().map(|f| format!("{}: <{}>", self.name(f.name), self.show(f.ty))).collect();
            let shown = self.show(target_ty);
            let close = Span::new(span.file, span.end.saturating_sub(1), span.end.saturating_sub(1));
            let sep = if fields.is_empty() { " " } else { ", " };
            let d = Diagnostic::new("T0101", span, format!("missing field{} {} for `{shown}`", if missing.len() == 1 { "" } else { "s" }, names.join(", ")))
                .fix(Applicability::Placeholder, format!("add {}", insert.join(", ")), close, format!("{sep}{} ", insert.join(", ")));
            self.report(d);
        }
        target_ty
    }

    /// Chooses the record type an object literal should be checked against.
    fn pick_record(&mut self, fields: &[crate::ast::ObjField], exp: TyId, span: Span) -> Option<(TyId, Vec<Field>)> {
        let members = self.flat_members(exp);
        let records: Vec<(TyId, Vec<Field>)> = members
            .iter()
            .filter_map(|&m| match *self.types.get(m) {
                Ty::Record(fs) => Some((m, self.types.fields(fs).to_vec())),
                _ => None,
            })
            .collect();
        if records.is_empty() {
            if let Some(&m) = members.iter().find(|&&m| matches!(self.types.get(m), Ty::Interface(..)))
                && let Ty::Interface(i, args) = *self.types.get(m) {
                    let args = self.types.tys(args).to_vec();
                    let fs = self.iface_fields_inst(i, &args);
                    return Some((m, fs));
                }
            return None;
        }
        if records.len() == 1 {
            let (m, fs) = records.into_iter().next().unwrap();
            let named = if members.len() == 1 { exp } else { m };
            return Some((named, fs));
        }
        // Discriminated union: find a literal-valued field in the object that selects a variant.
        for f in fields {
            let ExprKind::Str(val_sym) = self.ast().expr(f.value).kind else { continue };
            let val = &self.name(val_sym).to_string();
            let is_disc = records.iter().all(|(_, fs)| fs.iter().any(|x| x.name == f.name && matches!(self.types.get(x.ty), Ty::StrLit(_))));
            if !is_disc {
                continue;
            }
            let lit = self.types.str_lit(val_sym);
            if let Some((m, fs)) = records.iter().find(|(_, fs)| fs.iter().any(|x| x.name == f.name && x.ty == lit)) {
                return Some((*m, fs.clone()));
            }
            let options: Vec<String> = records
                .iter()
                .filter_map(|(_, fs)| fs.iter().find(|x| x.name == f.name))
                .filter_map(|x| match self.types.get(x.ty) {
                    Ty::StrLit(s) => Some(self.name(*s).to_string()),
                    _ => None,
                })
                .collect();
            let fname = self.name(f.name).to_string();
            let shown = self.show(exp);
            let vspan = self.ast().expr(f.value).span;
            let quoted: Vec<String> = options.iter().map(|o| format!("\"{o}\"")).collect();
            let mut d = Diagnostic::new("T0103", vspan, format!("`{fname}: \"{val}\"` is not a variant of `{shown}`")).note("valid values", quoted.join(", "));
            let sug = similar(val, options.iter().map(|s| s.as_str()));
            if let Some(first) = sug.first() {
                d = d.fix(Applicability::Maybe, format!("use \"{first}\""), vspan, format!("\"{first}\""));
            }
            self.report(d);
            for f in fields {
                self.expr(f.value, None);
            }
            let _ = span;
            return Some((ERROR, Vec::new()));
        }
        None
    }

    fn array_lit(&mut self, elems: &[ExprId], exp: Option<TyId>, span: Span) -> TyId {
        let elem_exp = exp.and_then(|e| {
            self.flat_members(e).into_iter().find_map(|m| match self.types.get(m) {
                Ty::Array(t) => Some(*t),
                _ => None,
            })
        });
        if let Some(et) = elem_exp {
            for &x in elems {
                let t = self.expr(x, Some(et));
                let s = self.ast().expr(x).span;
                self.expect_assignable(t, et, s, None);
            }
            return self.types.array(et);
        }
        if elems.is_empty() {
            if exp == Some(ERROR) {
                return ERROR;
            }
            self.report(
                Diagnostic::new("T0301", span, "can't infer the element type of an empty array")
                    .note("instead", "annotate the binding: `const xs: T[] = []`"),
            );
            return ERROR;
        }
        let mut tys = Vec::new();
        for &x in elems {
            let t = self.expr(x, None);
            tys.push(self.types.widen(t));
        }
        if tys.iter().all(|&t| self.types.is_numeric(t)) && tys.iter().any(|&t| self.types.is_float(t)) && tys.iter().all(|&t| t == INT || t == F64) {
            return self.types.array(F64);
        }
        let u = self.types.union(&tys);
        self.types.array(u)
    }

    pub(super) fn arrow(&mut self, f: &ArrowFn, exp: Option<TyId>, span: Span) -> TyId {
        let exp_fn = exp.and_then(|e| {
            self.flat_members(e).into_iter().find_map(|m| match *self.types.get(m) {
                Ty::Func(ps, r, _) => Some((self.types.params(ps).to_vec(), r)),
                _ => None,
            })
        });
        // A closure can throw when its context allows it (`(req) => Response throws Error`).
        let exp_throws = exp
            .and_then(|e| {
                self.flat_members(e).into_iter().find_map(|m| match *self.types.get(m) {
                    Ty::Func(_, _, th) => Some(th),
                    _ => None,
                })
            })
            .unwrap_or(NEVER);
        let is_async = f.is_async;
        // An async arrow's context expects `(...) => Promise<T>`: its body returns `T`, and
        // whatever it throws rejects the promise.
        // (The context may also accept a plain value: `Response | Promise<Response, Error>`.)
        let (exp_fn, exp_throws) = match exp_fn {
            Some((ps, r)) if is_async => {
                let promised = self.flat_members(r).into_iter().find_map(|m| match *self.types.get(m) {
                    Ty::Promise(v, err) => Some((v, err)),
                    _ => None,
                });
                match promised {
                    Some((v, err)) => (Some((ps, v)), err),
                    None => (Some((ps, r)), NEVER),
                }
            }
            other => (other, exp_throws),
        };
        let tscope = self.tscope();
        let mut params = Vec::new();
        for (i, p) in f.params.iter().enumerate() {
            let ty = match p.ty {
                Some(t) => self.resolve_type(t, &tscope),
                None => match exp_fn.as_ref().and_then(|(ps, _)| ps.get(i)) {
                    Some(fp) if !self.types.mentions(fp.ty, &self.infer_free) => fp.ty,
                    _ => {
                        if exp != Some(ERROR) {
                            let n = self.name(p.name).to_string();
                            self.report(
                                Diagnostic::new("T0301", p.span, format!("can't infer the type of parameter `{n}`"))
                                    .fix(Applicability::Placeholder, format!("annotate `{n}: T`"), p.span.empty_at_end(), ": T"),
                            );
                        }
                        ERROR
                    }
                },
            };
            let ty = if p.optional { self.types.optional(ty) } else { ty };
            params.push(FnParam { ty, inout: p.inout, optional: p.optional });
        }
        let declared_ret = f.ret.map(|t| self.resolve_type(t, &tscope));
        let declared_ret = match declared_ret {
            Some(r) if is_async => Some(self.async_inner(r, self.ast().ty(f.ret.unwrap()).span)),
            r => r,
        };
        let exp_ret = exp_fn.as_ref().map(|(_, r)| *r).filter(|&r| r != VOID && !self.types.mentions(r, &self.infer_free));
        let ret = declared_ret.or(exp_ret);
        let fcx = self.fcx.last_mut().unwrap();
        // A test's body can throw (an uncaught error fails the test); other closures only when
        // their function type says so.
        let test_body = fcx.test_body && fcx.frames.is_empty();
        let can_throw = test_body || exp_throws != NEVER || is_async;
        let decl = if exp_throws != NEVER && exp_throws != ERROR { Some(exp_throws) } else { None };
        let mut frame = Frame::new(declared_ret, None, can_throw, decl);
        frame.is_async = is_async;
        fcx.frames.push(frame);
        fcx.scopes.push(Vec::new());
        for (p, fp) in f.params.iter().zip(&params) {
            let kind = if p.inout { LocalKind::Inout } else { LocalKind::Param };
            self.declare(p.name, fp.ty, kind, p.span, None, false);
        }
        // A `routes` handler: its request's `params` are typed from the route.
        if let (Some(key), Some(p0), Some(fp0)) = (self.route_key, f.params.first(), params.first())
            && self.is_route_request(fp0.ty)
            && let Some((id, _)) = self.lookup(p0.name)
        {
            self.fcx().route_locals.push((id, key));
        }
        let body_ty = match &f.body {
            ArrowBody::Expr(x) => {
                let t = self.expr(*x, ret);
                if let Some(r) = declared_ret {
                    let s = self.ast().expr(*x).span;
                    self.expect_assignable(t, r, s, None);
                    r
                } else if let Some(r) = exp_ret {
                    if self.assignable(t, r) { r } else { t }
                } else {
                    t
                }
            }
            ArrowBody::Block(b) => {
                if declared_ret.is_none() {
                    self.fcx().frames.last_mut().unwrap().ret = exp_ret;
                }
                self.stmt(*b);
                let frame_ret = self.fcx().frames.last().unwrap().ret;
                match frame_ret {
                    Some(r) => {
                        if r != VOID && r != ERROR && !self.always_returns(*b) && !self.types.has_undefined(r) && !self.fcx.last().unwrap().reported_nonexhaustive {
                            self.report(Diagnostic::new("F0101", span, "arrow function doesn't return a value on every path"));
                        }
                        r
                    }
                    None => {
                        let mut all = std::mem::take(&mut self.fcx().frames.last_mut().unwrap().returns);
                        if all.is_empty() || all.iter().all(|&t| t == UNDEFINED || t == VOID) {
                            VOID
                        } else {
                            if !self.always_returns(*b) {
                                all.push(UNDEFINED);
                            }
                            self.types.union(&all)
                        }
                    }
                }
            }
        };
        let fcx = self.fcx.last_mut().unwrap();
        fcx.scopes.pop();
        let frame = fcx.frames.pop().unwrap();
        let thrown = if test_body || frame.thrown.is_empty() { NEVER } else { self.types.union(&frame.thrown) };
        if is_async {
            let p = self.types.promise(body_ty, thrown);
            return self.types.func(params, p);
        }
        self.types.func_throws(params, body_ty, thrown)
    }

    /// `await e`: the value of a promise (a rejection throws), or `e` itself if it isn't one.
    fn await_expr(&mut self, e: ExprId, x: ExprId, exp: Option<TyId>, span: Span) -> TyId {
        if !self.fcx.last().and_then(|f| f.frames.last()).is_some_and(|f| f.is_async) {
            self.report(
                Diagnostic::new("T0851", span, "`await` is only allowed in an async function (or at the top level of a script)")
                    .note("instead", "mark the enclosing function `async`; it then returns a `Promise`"),
            );
        }
        let exp_p = exp.map(|t| self.types.promise(t, NEVER));
        let t = self.expr(x, exp_p);
        if t == JS {
            // a JavaScript promise (or any value): rejects with a JsError
            let err = self.builtin_class("JsError");
            let text = format!("await {}", self.src(self.ast().expr(x).span));
            self.on_throw(err, span, Some(&text), Some(e));
            return JS;
        }
        let mut values = Vec::new();
        let mut errors = Vec::new();
        for m in self.flat_members(t) {
            match *self.types.get(m) {
                Ty::Promise(v, err) => {
                    values.push(v);
                    if err != NEVER {
                        errors.push(err);
                    }
                }
                _ => values.push(m),
            }
        }
        if !errors.is_empty() {
            let err = self.types.union(&errors);
            let text = format!("await {}", self.src(self.ast().expr(x).span));
            self.on_throw(err, span, Some(&text), Some(e));
        }
        self.types.union(&values)
    }

    /// `new Promise((resolve, reject) => { ... })`. The value type comes from `new Promise<T>`,
    /// the expected type, or else is `void` (`await new Promise((r) => setTimeout(r, 10))`). It can
    /// reject (with an `Error`) only if the executor takes a `reject` parameter.
    fn new_promise(&mut self, e: ExprId, targs: &[TyId], args: &[Arg], exp: Option<TyId>, span: Span) -> TyId {
        let from_exp = exp.and_then(|t| {
            self.flat_members(t).into_iter().find_map(|m| match *self.types.get(m) {
                Ty::Promise(v, _) => Some(v),
                _ => None,
            })
        });
        let value = targs.first().copied().or(from_exp).unwrap_or(VOID);
        let err = self.error_class();
        let [arg] = args else {
            self.report(Diagnostic::new("T0201", span, format!("`new Promise` takes 1 argument (the executor), found {}", args.len())).note("example", "`new Promise((resolve) => setTimeout(resolve, 10))`"));
            for a in args {
                self.expr(a.expr, None);
            }
            return self.types.promise(value, err);
        };
        let rejects = match &self.ast().expr(arg.expr).kind {
            ExprKind::Arrow(f) if f.params.len() < 2 => NEVER,
            _ => err,
        };
        let optional = value == VOID || value == UNDEFINED;
        let resolve = self.types.func(vec![FnParam { ty: value, inout: false, optional }], VOID);
        let reject = self.types.func(vec![FnParam { ty: err, inout: false, optional: false }], VOID);
        let executor = self.types.func(vec![FnParam { ty: resolve, inout: false, optional: false }, FnParam { ty: reject, inout: false, optional: false }], VOID);
        let t = self.expr(arg.expr, Some(executor));
        let s = self.ast().expr(arg.expr).span;
        self.expect_assignable(t, executor, s, Some("the executor".to_string()));
        self.rec_call(e, Callee::NewPromise);
        self.types.promise(value, rejects)
    }

    fn new_expr(&mut self, e: ExprId, callee: ExprId, type_args: &[crate::ast::TypeId], args: &[Arg], exp: Option<TyId>, span: Span) -> TyId {
        // `new X(...)` on a JavaScript constructor (`import { X } from "pkg"`, `new pkg.X()`)
        let js_callee = match self.ast().expr(callee).kind {
            ExprKind::Ident(s) => match self.lookup(s) {
                Some((_, t)) => t == JS,
                None => matches!(self.scopes[self.cur as usize].values.get(&s).map(|d| d.0), Some(Decl::Npm(..))),
            },
            _ => true,
        };
        if js_callee {
            let t = self.expr(callee, None);
            if t == JS || t == ERROR {
                let text = format!("new {}", self.src(self.ast().expr(callee).span));
                return self.js_call(e, None, &text, args, span, true);
            }
        }
        let ExprKind::Ident(s) = self.ast().expr(callee).kind else { return ERROR };
        let name = self.name(s).to_string();
        let tscope = self.tscope();
        let targs: Vec<TyId> = type_args.iter().map(|&t| self.resolve_type(t, &tscope)).collect();
        let from_exp = |c: &mut Self, pick: &dyn Fn(&Ty) -> bool| exp.and_then(|e| c.flat_members(e).into_iter().find(|&m| pick(c.types.get(m))));
        match name.as_str() {
            "Map" => {
                for a in args {
                    self.expr(a.expr, None);
                }
                if !args.is_empty() {
                    self.report(Diagnostic::new("U0017", span, "`new Map(entries)` is not supported yet; create an empty map and `set` entries"));
                }
                if targs.len() == 2 {
                    return self.types.intern(Ty::Map(targs[0], targs[1]));
                }
                if let Some(t) = from_exp(self, &|t| matches!(t, Ty::Map(..))) {
                    return t;
                }
                self.report(Diagnostic::new("T0301", span, "can't infer the key and value types of this map").note("instead", "`new Map<string, int>()`, or annotate the binding"));
                ERROR
            }
            "Set" => {
                let mut elem = targs.first().copied().or_else(|| from_exp(self, &|t| matches!(t, Ty::Set(_))).and_then(|t| match self.types.get(t) {
                    Ty::Set(e) => Some(*e),
                    _ => None,
                }));
                if let Some(a) = args.first() {
                    let hint = elem.map(|e| self.types.array(e));
                    let t = self.expr(a.expr, hint);
                    match *self.types.get(t) {
                        Ty::Array(e) => elem = elem.or(Some(self.types.widen(e))),
                        Ty::Error => {}
                        _ => {
                            let msg = format!("`new Set(...)` takes an array, found `{}`", self.show(t));
                            let s = self.ast().expr(a.expr).span;
                            self.report(Diagnostic::new("T0001", s, msg));
                        }
                    }
                }
                match elem {
                    Some(e) => self.types.intern(Ty::Set(e)),
                    None => {
                        self.report(Diagnostic::new("T0301", span, "can't infer the element type of this set").note("instead", "`new Set<string>()`"));
                        ERROR
                    }
                }
            }
            "Array" => {
                self.report(Diagnostic::new("X0036", span, "`new Array(...)` is not supported").note("instead", "use an array literal: `const xs: T[] = []`"));
                ERROR
            }
            "Promise" if !self.scopes[self.cur as usize].values.contains_key(&s) => self.new_promise(e, &targs, args, exp, span),
            _ => {
                if let Some(Decl::Class(c)) = self.scopes[self.cur as usize].values.get(&s).map(|d| d.0) {
                    self.rec_ident(callee, IdentFact::Class(c));
                    self.resolve_class(c);
                    let info = self.classes[c as usize].clone();
                    if info.is_abstract {
                        self.report(Diagnostic::new("T0804", span, format!("`{name}` is abstract, so it can't be instantiated")).note("instead", "create an instance of a concrete subclass"));
                    }
                    let names = info.ctor.param_names.iter().map(|&p| self.name(p).to_string()).collect();
                    let throws = self.ctor_throws(c);
                    let cs = CallSig { tparams: info.params.clone(), params: info.ctor.params.clone(), names, rest: None, ret: info.this_ty, throws };
                    self.rec_call(e, Callee::New(c));
                    self.pending_call = Some(e);
                    let desc = format!("new {name}");
                    let t = self.call_sig(&cs, &desc, type_args, args, exp, span);
                    let _ = targs;
                    return t;
                }
                let mut d = Diagnostic::new("N0001", self.ast().expr(callee).span, format!("unknown class `{name}`"));
                let classes: Vec<String> = self.scopes[self.cur as usize].values.iter().filter(|(_, d)| matches!(d.0, Decl::Class(_))).map(|(s, _)| self.name(*s).to_string()).collect();
                let sug = similar(&name, classes.iter().map(|s| s.as_str()));
                if let Some(first) = sug.first() {
                    d = d.note("did you mean", sug.join(", ")).fix(Applicability::Maybe, format!("use `{first}`"), self.ast().expr(callee).span, first.to_string());
                }
                if self.scopes[self.cur as usize].types.contains_key(&s) {
                    d = d.note("note", format!("`{name}` is a type; records are created with object literals: `{{ ... }}`"));
                }
                self.report(d);
                for a in args {
                    self.expr(a.expr, None);
                }
                ERROR
            }
        }
    }

    // ---------------------------------------------------------------- calls

    #[allow(clippy::too_many_arguments)]
    fn call(&mut self, e: ExprId, callee: ExprId, type_args: &[crate::ast::TypeId], args: &[Arg], optional: bool, exp: Option<TyId>, span: Span) -> TyId {
        let cnode = self.ast().expr(callee);
        match &cnode.kind {
            ExprKind::Ident(s) if self.lookup(*s).is_none() => {
                let text = self.name(*s).to_string();
                match self.scopes[self.cur as usize].values.get(s).map(|d| d.0) {
                    Some(Decl::Fn(m, i)) => {
                        let Some(sig) = self.fn_sig(m, i) else {
                            self.report(Diagnostic::new("T0303", cnode.span, format!("`{text}` is recursive, so it needs an explicit return type")));
                            for a in args {
                                self.expr(a.expr, None);
                            }
                            return ERROR;
                        };
                        let (fixed, rest) = sig.call_params(&self.types);
                        let cs = CallSig { tparams: sig.tparams, params: fixed, names: sig.param_names.iter().map(|&p| self.name(p).to_string()).collect(), rest, ret: sig.ret, throws: sig.throws };
                        self.rec_call(e, Callee::Fn(m, i));
                        self.pending_call = Some(e);
                        return self.call_sig(&cs, &text, type_args, args, exp, span);
                    }
                    Some(_) => {}
                    None => {
                        self.rec_call(e, Callee::Builtin { ns: None, name: *s });
                        self.pending_call = Some(e);
                        if let Some(t) = self.global_call(&text, cnode.span, type_args, args, exp, span) {
                            return t;
                        }
                    }
                }
            }
            ExprKind::Member { obj, name, name_span, optional: mopt } => {
                let (obj, name, name_span, mopt) = (*obj, *name, *name_span, *mopt);
                let mtext = self.name(name).to_string();
                if let ExprKind::Ident(s) = self.ast().expr(obj).kind
                    && self.lookup(s).is_none() {
                        match self.scopes[self.cur as usize].values.get(&s).map(|d| d.0) {
                            Some(Decl::Ns(m)) => {
                                let scope = &self.scopes[m as usize];
                                if let Some(Decl::Fn(fm, fi)) = scope.values.get(&name).filter(|_| scope.exported_values.contains(&name)).map(|d| d.0) {
                                    let Some(sig) = self.fn_sig(fm, fi) else { return ERROR };
                                    let (fixed, rest) = sig.call_params(&self.types);
                        let cs = CallSig { tparams: sig.tparams, params: fixed, names: sig.param_names.iter().map(|&p| self.name(p).to_string()).collect(), rest, ret: sig.ret, throws: sig.throws };
                                    let desc = format!("{}.{mtext}", self.name(s));
                                    self.rec_call(e, Callee::Fn(fm, fi));
                                    self.pending_call = Some(e);
                                    return self.call_sig(&cs, &desc, type_args, args, exp, span);
                                }
                            }
                            None => {
                                let ns = self.name(s).to_string();
                                if self.is_ns(&ns) {
                                    self.rec_call(e, Callee::Builtin { ns: Some(s), name });
                                    self.pending_call = Some(e);
                                    return self.builtin_ns_call(&ns, &mtext, name_span, args, exp, span);
                                }
                            }
                            Some(Decl::Class(c)) => {
                                self.resolve_class(c);
                                let info = self.classes[c as usize].clone();
                                let cls = self.class_names[c as usize].clone();
                                let Some(mm) = info.statics.iter().find(|m| m.name == name).cloned() else {
                                    self.static_member(obj, c, name, name_span);
                                    for a in args {
                                        self.expr(a.expr, None);
                                    }
                                    return ERROR;
                                };
                                self.check_visible(mm.owner, mm.vis, name, name_span);
                                let ret = self.method_ret(&mm);
                                let names = mm.param_names.iter().map(|&p| self.name(p).to_string()).collect();
                                let throws = self.method_throws(&mm);
                                let cs = CallSig { tparams: mm.tparams.clone(), params: mm.params.clone(), names, rest: None, ret, throws };
                                self.rec_call(e, Callee::StaticMethod(c, mm.member));
                                self.pending_call = Some(e);
                                return self.call_sig(&cs, &format!("{cls}.{mtext}"), type_args, args, exp, span);
                            }
                            _ => {}
                        }
                    }
                if matches!(self.ast().expr(obj).kind, ExprKind::Super) {
                    return self.super_method_call(e, name, name_span, type_args, args, exp, span);
                }
                let recv = self.expr(obj, None);
                if recv == ERROR {
                    for a in args {
                        self.expr(a.expr, None);
                    }
                    return ERROR;
                }
                if self.types.without_undefined(recv) == JS {
                    let desc = format!("{}.{mtext}", self.src(self.ast().expr(obj).span));
                    return self.js_call(e, Some(name), &desc, args, span, false);
                }
                let base = if mopt {
                    self.types.without_undefined(recv)
                } else {
                    if self.types.has_undefined(recv) {
                        let d = self.possibly_undefined(obj, span);
                        self.report(d);
                    }
                    self.types.without_undefined(recv)
                };
                // `process.stdout.write(s)` / `process.stderr.write(s)`.
                if let Ty::BuiltinNs(s) = *self.types.get(base) {
                    let ns = self.name(s).to_string();
                    self.rec_call(e, Callee::Builtin { ns: Some(s), name });
                    self.pending_call = Some(e);
                    return self.builtin_ns_call(&ns, &mtext, name_span, args, exp, span);
                }
                if let Ty::Expect(subject) = *self.types.get(base) {
                    self.rec_call(e, Callee::Matcher(subject));
                    self.pending_call = Some(e);
                    return self.expect_matcher(subject, &mtext, name_span, args, span);
                }
                // Methods of class instances.
                if self.class_of(base).is_some()
                    && let Some(super::class::ClassMemberRef::Method(mm)) = self.class_member(base, name)
                {
                    self.check_visible(mm.owner, mm.vis, name, name_span);
                    let ret = self.method_ret(&mm);
                    let names = mm.param_names.iter().map(|&p| self.name(p).to_string()).collect();
                    let throws = self.method_throws(&mm);
                    let cs = CallSig { tparams: mm.tparams.clone(), params: mm.params.clone(), names, rest: None, ret, throws };
                    self.rec_call(e, Callee::ClassMethod { recv: base, name, sup: false });
                    self.pending_call = Some(e);
                    let cls = self.show(base);
                    let r = self.call_sig(&cs, &format!("{cls}.{mtext}"), type_args, args, exp, span);
                    return if mopt || optional { self.types.optional(r) } else { r };
                }
                let fm = self.flat_members(base);
                if fm.len() > 1 && fm.iter().any(|&m| matches!(self.class_member(m, name), Some(super::class::ClassMemberRef::Method(_)))) {
                    let shown = self.show(base);
                    self.report(
                        Diagnostic::new("T0817", span, format!("can't call method `{mtext}` on `{shown}`"))
                            .note("why", "each class in the union has its own method")
                            .note("instead", "narrow first (`if (x instanceof C)`), or give the classes a common base class that declares the method"),
                    );
                    for a in args {
                        self.expr(a.expr, None);
                    }
                    return ERROR;
                }
                // A record field holding a function.
                let is_field = self.flat_members(base).iter().all(|&m| self.field_of(m, name).is_some());
                let result = if is_field {
                    let ft = self.member_of(base, name, name_span, span, obj).unwrap_or(ERROR);
                    if let Some(f) = self.facts_mut() {
                        f.expr_ty[callee as usize] = ft;
                    }
                    self.rec_call(e, Callee::Value);
                    self.pending_call = Some(e);
                    self.call_value(ft, &mtext, args, exp, span)
                } else if let Some(ms) = builtins::method(self, base, &mtext) {
                    if ms.mutates {
                        self.check_mutable_root_ext(obj, span, true);
                    }
                    let desc = format!("{}.{mtext}", builtins::type_label(&self.types, base));
                    self.rec_call(e, Callee::Method(base));
                    self.pending_call = Some(e);
                    self.call_sig(&ms.sig, &desc, type_args, args, exp, span)
                } else {
                    self.member_of(base, name, name_span, span, obj);
                    for a in args {
                        self.expr(a.expr, None);
                    }
                    ERROR
                };
                return if mopt || optional { self.types.optional(result) } else { result };
            }
            ExprKind::Super => return self.super_ctor_call(e, args, span),
            _ => {}
        }
        let t = self.expr(callee, None);
        let text = self.src(cnode.span).to_string();
        let t = if optional { self.types.without_undefined(t) } else { t };
        if t == JS {
            return self.js_call(e, None, &text, args, span, false);
        }
        self.rec_call(e, Callee::Value);
        self.pending_call = Some(e);
        let r = self.call_value(t, &text, args, exp, span);
        if optional { self.types.optional(r) } else { r }
    }

    /// `super(...)` in a derived class's constructor.
    fn super_ctor_call(&mut self, e: ExprId, args: &[Arg], span: Span) -> TyId {
        let (class, ctor) = self.fcx.last().map(|f| (f.class, f.ctor)).unwrap_or((None, false));
        let base = class.and_then(|c| self.classes[c as usize].base);
        let (Some(_), true, Some(base)) = (class, ctor, base) else {
            self.report(Diagnostic::new("T0801", span, "`super(...)` is only valid in the constructor of a class that `extends` another"));
            for a in args {
                self.expr(a.expr, None);
            }
            return ERROR;
        };
        let (b, bargs) = self.class_of(base).unwrap();
        let binfo = self.classes[b as usize].clone();
        let map: HashMap<u32, TyId> = binfo.params.iter().copied().zip(bargs.iter().copied()).collect();
        let params: Vec<FnParam> = binfo.ctor.params.iter().map(|p| FnParam { ty: self.types.subst(p.ty, &map), ..*p }).collect();
        let names = binfo.ctor.param_names.iter().map(|&p| self.name(p).to_string()).collect();
        let throws = self.ctor_throws(b);
        let throws = self.types.subst(throws, &map);
        let cs = CallSig { tparams: Vec::new(), params, names, rest: None, ret: VOID, throws };
        self.rec_call(e, Callee::SuperCtor(base));
        self.pending_call = Some(e);
        self.call_sig(&cs, "super", &[], args, None, span)
    }

    /// `super.name(...)`: the base class's implementation.
    #[allow(clippy::too_many_arguments)]
    fn super_method_call(&mut self, e: ExprId, name: Sym, name_span: Span, type_args: &[crate::ast::TypeId], args: &[Arg], exp: Option<TyId>, span: Span) -> TyId {
        let class = self.fcx.last().and_then(|f| f.class);
        let base = class.and_then(|c| self.classes[c as usize].base);
        let has_this = self.lookup(super::THIS_SYM).is_some();
        let (Some(base), true) = (base, has_this) else {
            self.report(Diagnostic::new("T0801", span, "`super.method(...)` is only valid in methods of a class that `extends` another"));
            for a in args {
                self.expr(a.expr, None);
            }
            return ERROR;
        };
        let mtext = self.name(name).to_string();
        match self.class_member(base, name) {
            Some(super::class::ClassMemberRef::Method(mm)) => {
                if mm.is_abstract {
                    self.report(Diagnostic::new("T0806", name_span, format!("`super.{mtext}` is abstract in the base class")));
                }
                self.check_visible(mm.owner, mm.vis, name, name_span);
                let ret = self.method_ret(&mm);
                let names = mm.param_names.iter().map(|&p| self.name(p).to_string()).collect();
                let throws = self.method_throws(&mm);
                let cs = CallSig { tparams: Vec::new(), params: mm.params.clone(), names, rest: None, ret, throws };
                self.rec_call(e, Callee::ClassMethod { recv: base, name, sup: true });
                self.pending_call = Some(e);
                self.call_sig(&cs, &format!("super.{mtext}"), type_args, args, exp, span)
            }
            _ => {
                let shown = self.show(base);
                self.report(Diagnostic::new("T0107", name_span, format!("base class `{shown}` has no method `{mtext}`")));
                for a in args {
                    self.expr(a.expr, None);
                }
                ERROR
            }
        }
    }

    fn call_value(&mut self, t: TyId, desc: &str, args: &[Arg], exp: Option<TyId>, span: Span) -> TyId {
        match *self.types.get(t) {
            Ty::Func(params, ret, throws) => {
                let params = self.types.params(params).to_vec();
                let names = (0..params.len()).map(|i| format!("arg{i}")).collect();
                let cs = CallSig { tparams: Vec::new(), params, names, rest: None, ret, throws };
                self.call_sig(&cs, desc, &[], args, exp, span)
            }
            Ty::Error => {
                for a in args {
                    self.expr(a.expr, None);
                }
                ERROR
            }
            _ => {
                for a in args {
                    self.expr(a.expr, None);
                }
                let msg = format!("`{desc}` is not a function (its type is `{}`)", self.show(t));
                self.report(Diagnostic::new("T0202", span, msg));
                ERROR
            }
        }
    }

    pub(super) fn describe_sig(&self, cs: &CallSig, desc: &str) -> String {
        let mut parts: Vec<String> = cs
            .params
            .iter()
            .zip(&cs.names)
            .map(|(p, n)| format!("{}{}{}: {}", if p.inout { "inout " } else { "" }, n, if p.optional { "?" } else { "" }, self.show(if p.optional { self.types_without_undef(p.ty) } else { p.ty })))
            .collect();
        if let Some(r) = cs.rest {
            parts.push(format!("...items: {}[]", self.show(r)));
        }
        format!("{desc}({}): {}", parts.join(", "), self.show(cs.ret))
    }

    // ------------------------------------------------------------ JavaScript values (npm)

    /// Can a Tov value of type `t` go to JavaScript? It's converted on the way: numbers,
    /// strings, booleans, `undefined`, arrays, records and unions of these, `Js` itself, and
    /// functions whose parameters are `Js` (JavaScript calls them back).
    pub(super) fn js_convertible(&mut self, t: TyId) -> bool {
        match *self.types.get(t) {
            Ty::Js | Ty::Error | Ty::Never | Ty::Str | Ty::StrLit(_) | Ty::Bool | Ty::Undefined | Ty::Void => true,
            Ty::Int | Ty::F64 | Ty::F32 | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64 => true,
            Ty::Array(e) => self.js_convertible(e),
            Ty::Record(fs) => {
                let fs = self.types.fields(fs).to_vec();
                fs.iter().all(|f| self.js_convertible(f.ty))
            }
            Ty::Union(ms) => {
                let ms = self.types.tys(ms).to_vec();
                ms.iter().all(|&m| self.js_convertible(m))
            }
            Ty::Func(ps, r, th) => {
                let ps = self.types.params(ps).to_vec();
                th == NEVER && ps.iter().all(|p| p.ty == JS && !p.inout) && self.js_convertible(r)
            }
            _ => false,
        }
    }

    /// The type of `e` if it's a plain name (a local, or an npm import): enough to tell a
    /// JavaScript object without checking `e` twice. Other expressions: `None` (checked normally).
    fn expr_type_peek(&mut self, e: ExprId) -> Option<TyId> {
        match self.ast().expr(e).kind {
            ExprKind::Ident(s) => match self.lookup(s) {
                Some((_, t)) => Some(self.types.without_undefined(t)),
                None => match self.scopes[self.cur as usize].values.get(&s).map(|d| d.0) {
                    Some(Decl::Npm(..)) => Some(JS),
                    Some(Decl::Const(m, i)) => {
                        let t = self.const_type(m, i);
                        Some(self.types.without_undefined(t))
                    }
                    _ => None,
                },
            },
            ExprKind::Member { obj, .. } | ExprKind::Index { obj, .. } => (self.expr_type_peek(obj) == Some(JS)).then_some(JS),
            ExprKind::Paren(x) => self.expr_type_peek(x),
            _ => None,
        }
    }

    /// Can a JavaScript value be converted to `t` (`x as T`)?
    pub(super) fn js_receivable(&mut self, t: TyId) -> bool {
        match *self.types.get(t) {
            Ty::Js | Ty::Error | Ty::Str | Ty::StrLit(_) | Ty::Bool | Ty::Undefined | Ty::Void => true,
            Ty::Int | Ty::F64 | Ty::F32 | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64 => true,
            Ty::Array(e) => self.js_receivable(e),
            Ty::Record(fs) => {
                let fs = self.types.fields(fs).to_vec();
                fs.iter().all(|f| self.js_receivable(f.ty))
            }
            Ty::Union(ms) => {
                let ms = self.types.tys(ms).to_vec();
                ms.iter().all(|&m| self.js_receivable(m))
            }
            _ => false,
        }
    }

    /// An argument to a JavaScript function: arrows get `Js` parameters (and return `Js`).
    fn js_arg(&mut self, a: &Arg) {
        let x = a.expr;
        let exp = match &self.ast().expr(x).kind {
            ExprKind::Arrow(f) => {
                let n = f.params.len();
                let ps = vec![FnParam { ty: JS, inout: false, optional: false }; n];
                // a block that returns nothing: `void` (JavaScript sees `undefined`)
                let ret = match f.body {
                    crate::ast::ArrowBody::Block(b) if !super::returns_value(self.ast(), b) => VOID,
                    _ => JS,
                };
                Some(self.types.func(ps, ret))
            }
            _ => Some(JS),
        };
        let t = self.expr(x, exp);
        if !self.js_convertible(t) {
            let shown = self.show(t);
            let s = self.ast().expr(x).span;
            self.report(
                Diagnostic::new("T0901", s, format!("a `{shown}` can't be passed to JavaScript"))
                    .note("why", "values are converted for JavaScript: numbers, strings, booleans, `undefined`, arrays, records, `Js` values, and functions taking `Js`"),
            );
        }
    }

    /// A call to a JavaScript function or method, or `new` on a JavaScript constructor: returns
    /// a `Js`, and can throw a `JsError`.
    fn js_call(&mut self, e: ExprId, method: Option<Sym>, desc: &str, args: &[Arg], span: Span, new: bool) -> TyId {
        for a in args {
            if let Some(s) = a.by_ref {
                self.report(Diagnostic::new("T0904", s, "JavaScript functions take values, not `&` references"));
            }
            self.js_arg(a);
        }
        self.rec_call(e, if new { Callee::JsNew } else { Callee::Js { method } });
        let err = self.builtin_class("JsError");
        self.on_throw(err, span, Some(desc), Some(e));
        JS
    }

    fn types_without_undef(&self, t: TyId) -> TyId {
        match *self.types.get(t) {
            Ty::Union(ms) if self.types.tys(ms).len() == 2 && self.types.tys(ms).contains(&UNDEFINED) => *self.types.tys(ms).iter().find(|&&m| m != UNDEFINED).unwrap(),
            _ => t,
        }
    }

    pub(super) fn call_sig(&mut self, cs: &CallSig, desc: &str, type_args: &[crate::ast::TypeId], args: &[Arg], exp: Option<TyId>, span: Span) -> TyId {
        let call_expr = self.pending_call.take();
        let required = cs.params.iter().filter(|p| !p.optional).count();
        let max = if cs.rest.is_some() { usize::MAX } else { cs.params.len() };
        if args.len() < required || args.len() > max {
            let expected = if cs.rest.is_some() {
                format!("at least {required}")
            } else if required == cs.params.len() {
                format!("{required}")
            } else {
                format!("{required} to {}", cs.params.len())
            };
            let d = Diagnostic::new("T0201", span, format!("`{desc}` takes {expected} argument{}, found {}", if expected == "1" { "" } else { "s" }, args.len()))
                .note("signature", self.describe_sig(cs, desc));
            self.report(d);
        }
        let mut map: HashMap<u32, TyId> = HashMap::default();
        if !type_args.is_empty() {
            if type_args.len() != cs.tparams.len() {
                let msg = format!("`{desc}` takes {} type argument(s), found {}", cs.tparams.len(), type_args.len());
                self.report(Diagnostic::new("T0010", span, msg));
            }
            let tscope = self.tscope();
            for (&p, &ta) in cs.tparams.iter().zip(type_args) {
                let t = self.resolve_type(ta, &tscope);
                map.insert(p, t);
            }
        }
        // The expected type can fix type parameters the arguments don't mention (`const s: Stack<int> = new Stack()`).
        if type_args.is_empty()
            && !cs.tparams.is_empty()
            && let Some(x) = exp
            && x != ERROR
        {
            let x = self.types.without_undefined(x);
            let free = cs.tparams.clone();
            let ret = self.types.without_undefined(cs.ret);
            self.unify(ret, x, &free, &mut map);
        }
        let saved_free = std::mem::take(&mut self.infer_free);
        self.infer_free = cs.tparams.iter().copied().filter(|p| !map.contains_key(p)).collect();
        let param_for = |i: usize| -> Option<FnParam> { cs.params.get(i).cloned().or_else(|| cs.rest.map(|ty| FnParam { ty, inout: false, optional: false })) };
        let mut arg_tys: Vec<Option<TyId>> = vec![None; args.len()];
        // Pass 1: non-closure arguments drive inference; pass 2: closures get the inferred parameter
        // types. An object literal holding closures (`{ port, data, socket: { open(s) {...} } }`)
        // is both: its other fields infer in pass 1, the whole object is checked in pass 2.
        for pass in 0..2 {
            if pass == 1 {
                // what the arguments left open takes its default (`<T = undefined>`)
                for &p in &cs.tparams {
                    if !map.contains_key(&p)
                        && let Some(d) = self.gparam(p).default
                    {
                        map.insert(p, d);
                    }
                }
            }
            for (i, a) in args.iter().enumerate() {
                let with_closures = matches!(self.ast().expr(a.expr).kind, ExprKind::Object(_)) && self.has_closure(a.expr) && !self.infer_free.is_empty();
                let is_arrow = matches!(self.ast().expr(a.expr).kind, ExprKind::Arrow(_)) || with_closures;
                if pass == 0 && with_closures {
                    if let Some(p) = param_for(i) {
                        let free = self.infer_free.clone();
                        self.infer_from_object(a.expr, p.ty, &free, &mut map);
                    }
                    continue;
                }
                if (pass == 0) == is_arrow {
                    continue;
                }
                let Some(p) = param_for(i) else {
                    self.expr(a.expr, None);
                    continue;
                };
                let pty = self.types.subst(p.ty, &map);
                let still_free: Vec<u32> = self.infer_free.iter().copied().filter(|x| !map.contains_key(x)).collect();
                let hint = if is_arrow || !self.types.mentions(pty, &still_free) { Some(pty) } else { None };
                self.infer_free = still_free;
                let t = self.expr(a.expr, hint);
                arg_tys[i] = Some(t);
                let free = self.infer_free.clone();
                self.unify(p.ty, t, &free, &mut map);
            }
        }
        self.infer_free = saved_free;
        // Check each argument against the instantiated parameter type.
        let mut refs: Vec<(LocalId, Span)> = Vec::new();
        for (i, a) in args.iter().enumerate() {
            let Some(p) = param_for(i) else { continue };
            let Some(t) = arg_tys[i] else { continue };
            let pty = self.types.subst(p.ty, &map);
            let aspan = self.ast().expr(a.expr).span;
            let pname = cs.names.get(i).cloned().unwrap_or_else(|| "items".to_string());
            match (p.inout, a.by_ref) {
                (true, None) => {
                    let text = self.src(aspan).to_string();
                    self.report(
                        Diagnostic::new("V0110", aspan, format!("parameter `{pname}` of `{desc}` is `inout`; pass the argument with `&`"))
                            .fix(Applicability::Safe, format!("pass `&{text}`"), aspan.empty_at_start(), "&"),
                    );
                }
                (false, Some(amp)) => {
                    self.report(
                        Diagnostic::new("V0111", amp.to(aspan), format!("parameter `{pname}` of `{desc}` is not `inout`; pass the value without `&`"))
                            .fix(Applicability::Safe, "remove `&`", amp, ""),
                    );
                }
                (true, Some(_)) => {
                    self.invalidate_place(a.expr);
                    if let Some(place) = self.place(a.expr, true) {
                        if let Some(root) = place.root {
                            if let Some(&(_, prev)) = refs.iter().find(|(r, _)| *r == root) {
                                let n = self.name(self.local(root).name).to_string();
                                let _ = prev;
                                self.report(Diagnostic::new("V0113", aspan, format!("`{n}` is passed as `inout` more than once in this call")).note("why", "each `inout` argument must be a separate value"));
                            }
                            refs.push((root, aspan));
                            // The callee may change the value in place (for V0120).
                            if let Some(f) = self.fcx.last_mut() {
                                f.mutations.push((root, aspan));
                            }
                        }
                        if t != pty && t != ERROR && pty != ERROR {
                            let msg = format!("`inout` argument must be exactly `{}`, found `{}`", self.show(pty), self.show(t));
                            self.report(Diagnostic::new("T0001", aspan, msg));
                        }
                    }
                    continue;
                }
                (false, None) => {}
            }
            if !self.assignable(t, pty) {
                let d = self.mismatch(t, pty, aspan, Some(format!("argument `{pname}` of `{desc}`")));
                self.report(d);
            }
        }
        let unresolved: Vec<u32> = cs.tparams.iter().copied().filter(|p| !map.contains_key(p) && self.types.mentions(cs.ret, &[*p])).collect();
        if !unresolved.is_empty() {
            let names: Vec<String> = unresolved.iter().map(|&p| self.name(self.gparam(p).name).to_string()).collect();
            let _ = exp;
            self.report(
                Diagnostic::new("T0305", span, format!("can't infer type argument `{}` of `{desc}`", names.join("`, `")))
                    .note("instead", format!("pass it explicitly: `{desc}<{}>(...)`", names.join(", "))),
            );
            return ERROR;
        }
        let ret = self.types.subst(cs.ret, &map);
        if cs.throws != NEVER && cs.throws != ERROR {
            let thrown = if cs.throws == UNKNOWN { self.error_class() } else { self.types.subst(cs.throws, &map) };
            self.on_throw(thrown, span, Some(desc), call_expr);
        }
        if let Some(ce) = call_expr
            && self.facts.is_some()
        {
            let params: Vec<FnParam> = cs.params.iter().map(|p| FnParam { ty: self.types.subst(p.ty, &map), ..*p }).collect();
            let rest = cs.rest.map(|r| self.types.subst(r, &map));
            let targs: Vec<(u32, TyId)> = cs.tparams.iter().filter_map(|p| map.get(p).map(|t| (*p, *t))).collect();
            if let Some(f) = self.facts_mut()
                && let Some(c) = f.calls.get_mut(&ce)
            {
                c.params = params;
                c.rest = rest;
                c.targs = targs;
                c.ret = ret;
            }
        }
        ret
    }

    /// Does an object literal hold a closure (at any depth of object literals)?
    fn has_closure(&self, e: ExprId) -> bool {
        match &self.ast().expr(e).kind {
            ExprKind::Arrow(_) => true,
            ExprKind::Object(fs) => fs.iter().any(|f| self.has_closure(f.value)),
            _ => false,
        }
    }

    /// Infers type parameters from an object literal's fields that aren't closures, against the
    /// record type the parameter expects (closures are checked later, with what this finds).
    fn infer_from_object(&mut self, e: ExprId, pattern: TyId, free: &[u32], map: &mut HashMap<u32, TyId>) {
        let ExprKind::Object(fields) = &self.ast().expr(e).kind else { return };
        let pat = self.types.without_undefined(pattern);
        let pat = self.unfold(pat);
        let Ty::Record(fs) = *self.types.get(pat) else { return };
        let fs = self.types.fields(fs).to_vec();
        for f in fields {
            let Some(ft) = fs.iter().find(|x| x.name == f.name).map(|x| x.ty) else { continue };
            match self.ast().expr(f.value).kind {
                ExprKind::Arrow(_) => {}
                ExprKind::Object(_) if self.has_closure(f.value) => self.infer_from_object(f.value, ft, free, map),
                _ => {
                    let open: Vec<u32> = free.iter().copied().filter(|p| !map.contains_key(p)).collect();
                    let pty = self.types.subst(ft, map);
                    let hint = if self.types.mentions(pty, &open) { None } else { Some(pty) };
                    let t = self.expr(f.value, hint);
                    self.unify(ft, t, free, map);
                }
            }
        }
    }

    pub(super) fn unify(&mut self, pattern: TyId, actual: TyId, free: &[u32], map: &mut HashMap<u32, TyId>) {
        if actual == ERROR {
            return;
        }
        match *self.types.get(pattern) {
            Ty::Param(p) if free.contains(&p) => match map.get(&p).copied() {
                None => {
                    let w = self.types.widen(actual);
                    map.insert(p, w);
                }
                Some(bound) => {
                    if !self.assignable(actual, bound) && self.assignable(bound, actual) {
                        let w = self.types.widen(actual);
                        map.insert(p, w);
                    }
                }
            },
            Ty::Array(pe) => {
                if let Ty::Array(ae) = *self.types.get(actual) {
                    self.unify(pe, ae, free, map);
                }
            }
            Ty::Set(pe) => {
                if let Ty::Set(ae) = *self.types.get(actual) {
                    self.unify(pe, ae, free, map);
                }
            }
            Ty::Map(pk, pv) => {
                if let Ty::Map(ak, av) = *self.types.get(actual) {
                    self.unify(pk, ak, free, map);
                    self.unify(pv, av, free, map);
                }
            }
            Ty::Promise(pv, pe) => {
                if let Ty::Promise(av, ae) = *self.types.get(actual) {
                    self.unify(pv, av, free, map);
                    if ae != NEVER {
                        self.unify(pe, ae, free, map);
                    }
                }
            }
            Ty::Func(pps, pr, _) => {
                if let Ty::Func(aps, ar, _) = *self.types.get(actual) {
                    let (pps, aps) = (self.types.params(pps).to_vec(), self.types.params(aps).to_vec());
                    for (pp, ap) in pps.iter().zip(&aps) {
                        self.unify(pp.ty, ap.ty, free, map);
                    }
                    self.unify(pr, ar, free, map);
                }
            }
            Ty::Record(pfs) => {
                if let Ty::Record(afs) = *self.types.get(actual) {
                    let (pfs, afs) = (self.types.fields(pfs).to_vec(), self.types.fields(afs).to_vec());
                    for pf in &pfs {
                        if let Some(af) = afs.iter().find(|f| f.name == pf.name) {
                            self.unify(pf.ty, af.ty, free, map);
                        }
                    }
                }
            }
            Ty::Union(pms) => {
                let pattern_rest: Vec<TyId> = self.types.tys(pms).iter().copied().filter(|&m| m != UNDEFINED).collect();
                if pattern_rest.len() == 1 {
                    let actual_rest = self.types.without_undefined(actual);
                    self.unify(pattern_rest[0], actual_rest, free, map);
                }
            }
            Ty::Class(pd, pargs) => {
                if let Some(up) = self.upcast_to(actual, pd)
                    && let Ty::Class(_, aargs) = *self.types.get(up)
                {
                    let (pargs, aargs) = (self.types.tys(pargs).to_vec(), self.types.tys(aargs).to_vec());
                    for (p, a) in pargs.iter().zip(&aargs) {
                        self.unify(*p, *a, free, map);
                    }
                }
            }
            Ty::Rec(pd, pargs) | Ty::Interface(pd, pargs) => {
                if let Ty::Rec(ad, aargs) | Ty::Interface(ad, aargs) = *self.types.get(actual)
                    && pd == ad {
                        let (pargs, aargs) = (self.types.tys(pargs).to_vec(), self.types.tys(aargs).to_vec());
                        for (p, a) in pargs.iter().zip(&aargs) {
                            self.unify(*p, *a, free, map);
                        }
                    }
            }
            _ => {}
        }
    }

    // ---------------------------------------------------------------- conditions and narrowing

    fn condition_ok(&mut self, t: TyId) -> bool {
        if t == BOOL || t == ERROR || t == JS {
            // (a JavaScript value: its truthiness, as in JavaScript)
            return true;
        }
        if self.types.has_undefined(t) {
            let inner = self.types.without_undefined(t);
            return self.flat_members(inner).iter().all(|&m| !self.types.is_primitive(m) && m != BOOL);
        }
        false
    }

    pub(super) fn cond(&mut self, e: ExprId) {
        let t = self.expr(e, Some(BOOL));
        if self.condition_ok(t) {
            return;
        }
        let span = self.ast().expr(e).span;
        let text = self.src(span).to_string();
        let shown = self.show(t);
        let mut d = Diagnostic::new("T0401", span, format!("condition must be `bool`, found `{shown}`"))
            .note("why", "no implicit truthiness: `0`, `\"\"` and `undefined` are not `false`");
        let inner = self.types.without_undefined(t);
        if self.types.has_undefined(t) {
            d = d.fix(Applicability::Maybe, format!("test for presence: `{text} !== undefined`"), span, format!("{text} !== undefined"));
        } else if self.types.is_numeric(inner) {
            d = d.fix(Applicability::Maybe, format!("`{text} !== 0`"), span, format!("{text} !== 0"));
        } else if inner == DECIMAL {
            d = d.fix(Applicability::Maybe, format!("`{text} !== 0`"), span, format!("{text} !== 0"));
        } else if inner == BIGINT {
            d = d.fix(Applicability::Maybe, format!("`{text} !== 0n`"), span, format!("{text} !== 0n"));
        } else if self.types.is_string(inner) {
            d = d.fix(Applicability::Maybe, format!("`{text} !== \"\"`"), span, format!("{text} !== \"\""));
        } else if matches!(self.types.get(inner), Ty::Array(_)) {
            d = d.fix(Applicability::Maybe, format!("`{text}.length > 0`"), span, format!("{text}.length > 0"));
        }
        self.report(d);
    }

    pub(super) fn apply(&mut self, ns: &[Narrowing]) {
        for n in ns {
            match n {
                Narrowing::Local(l, t) => self.narrow(*l, *t),
                Narrowing::Path(l, p, t, heap) => {
                    self.narrow_path(*l, p.clone(), Some(*t));
                    if *heap {
                        self.fcx().heap_paths.push((*l, p.clone()));
                    }
                }
            }
        }
    }

    /// What we learn about locals when `e` evaluates to `positive`.
    pub(super) fn narrowings(&mut self, e: ExprId, positive: bool) -> Vec<Narrowing> {
        let ast = self.ast();
        match &ast.expr(e).kind {
            ExprKind::Paren(x) => self.narrowings(*x, positive),
            ExprKind::Unary(UnOp::Not, x) => self.narrowings(*x, !positive),
            ExprKind::Binary(BinOp::And, l, r) if positive => {
                let mut v = self.narrowings(*l, true);
                self.push_scope();
                self.apply(&v);
                v.extend(self.narrowings(*r, true));
                self.pop_scope();
                v
            }
            ExprKind::Binary(BinOp::Or, l, r) if !positive => {
                let mut v = self.narrowings(*l, false);
                self.push_scope();
                self.apply(&v);
                v.extend(self.narrowings(*r, false));
                self.pop_scope();
                v
            }
            ExprKind::Binary(op @ (BinOp::Eq | BinOp::Ne | BinOp::LooseEq | BinOp::LooseNe), l, r) => {
                let eq = matches!(op, BinOp::Eq | BinOp::LooseEq) == positive;
                let (l, r) = (*l, *r);
                let (subject, other) = if matches!(ast.expr(l).kind, ExprKind::Undefined | ExprKind::Str(_)) { (r, l) } else { (l, r) };
                match &ast.expr(other).kind {
                    ExprKind::Undefined => {
                        let Some((target, ty)) = self.target_of(subject) else { return Vec::new() };
                        let t = if eq { if self.types.has_undefined(ty) { UNDEFINED } else { NEVER } } else { self.types.without_undefined(ty) };
                        vec![target.with(t)]
                    }
                    ExprKind::Str(lit) => {
                        let lit = *lit;
                        let lit_ty = self.types.str_lit(lit);
                        match &ast.expr(subject).kind {
                            ExprKind::Member { obj, name, optional: false, .. } => {
                                let Some((target, ty)) = self.target_of(*obj) else { return Vec::new() };
                                let members = self.flat_members(ty);
                                let keep: Vec<TyId> = members
                                    .into_iter()
                                    .filter(|&m| match *self.types.get(m) {
                                        Ty::Record(fs) => match self.types.fields(fs).iter().find(|f| f.name == *name) {
                                            Some(f) if matches!(self.types.get(f.ty), Ty::StrLit(_)) => (f.ty == lit_ty) == eq,
                                            _ => true,
                                        },
                                        _ => true,
                                    })
                                    .collect();
                                vec![target.with(self.types.union(&keep))]
                            }
                            ExprKind::Typeof(x) => {
                                let Some((target, ty)) = self.target_of(*x) else { return Vec::new() };
                                let kind = self.name(lit).to_string();
                                let members = self.flat_members(ty);
                                let keep: Vec<TyId> = members.into_iter().filter(|&m| (typeof_name(&self.types, m) == Some(kind.as_str())) == eq).collect();
                                vec![target.with(self.types.union(&keep))]
                            }
                            _ => {
                                let Some((target, ty)) = self.target_of(subject) else { return Vec::new() };
                                let members = self.flat_members(ty);
                                if !members.iter().all(|&m| matches!(self.types.get(m), Ty::StrLit(_)) || m == UNDEFINED) {
                                    if eq {
                                        return vec![target.with(lit_ty)];
                                    }
                                    return Vec::new();
                                }
                                let keep: Vec<TyId> = members.into_iter().filter(|&m| (m == lit_ty) == eq).collect();
                                vec![target.with(self.types.union(&keep))]
                            }
                        }
                    }
                    _ => Vec::new(),
                }
            }
            ExprKind::Binary(BinOp::Instanceof, l, r) => {
                let ExprKind::Ident(s) = ast.expr(*r).kind else { return Vec::new() };
                let Some(Decl::Class(c)) = self.scopes[self.cur as usize].values.get(&s).map(|d| d.0) else { return Vec::new() };
                if self.lookup(s).is_some() {
                    return Vec::new();
                }
                let Some((target, ty)) = self.target_of(*l) else { return Vec::new() };
                let mut keep = Vec::new();
                for m in self.flat_members(ty) {
                    let Some((mc, _)) = self.class_of(m) else {
                        if !positive {
                            keep.push(m);
                        }
                        continue;
                    };
                    let is_c = self.class_descends(mc, c);
                    if positive {
                        if is_c {
                            keep.push(m);
                        } else if self.class_descends(c, mc) {
                            let d = self.downcast_ty(m, c);
                            keep.push(d);
                        }
                    } else if !is_c {
                        keep.push(m);
                    }
                }
                vec![target.with(self.types.union(&keep))]
            }
            ExprKind::Ident(_) | ExprKind::Member { optional: false, .. } => {
                let Some((target, ty)) = self.target_of(e) else { return Vec::new() };
                if !self.types.has_undefined(ty) {
                    return Vec::new();
                }
                let t = if positive { self.types.without_undefined(ty) } else { UNDEFINED };
                vec![target.with(t)]
            }
            _ => Vec::new(),
        }
    }

    /// A narrowable reference: a local, or a field path of a local (`t.left.value`), with its current type.
    fn target_of(&mut self, e: ExprId) -> Option<(Target, TyId)> {
        match &self.ast().expr(e).kind {
            ExprKind::Ident(s) => self.lookup(*s).map(|(id, ty)| (Target { local: id, path: Vec::new(), heap: false }, ty)),
            ExprKind::This => self.lookup(super::THIS_SYM).map(|(id, ty)| (Target { local: id, path: Vec::new(), heap: false }, ty)),
            ExprKind::Paren(x) => self.target_of(*x),
            ExprKind::Member { obj, name, optional: false, .. } => {
                let (mut target, base) = self.target_of(*obj)?;
                target.path.push(*name);
                let base = self.types.without_undefined(base);
                let members = self.flat_members(base);
                // Through a class instance: other references can change it (calls forget it); getters aren't narrowable.
                for &m in &members {
                    if self.class_of(m).is_some() {
                        target.heap = true;
                        if !matches!(self.class_member(m, *name), Some(super::class::ClassMemberRef::Field(_))) {
                            return None;
                        }
                    }
                }
                if let Some(t) = self.lookup_path(target.local, &target.path) {
                    return Some((target, t));
                }
                let mut tys = Vec::new();
                for m in members {
                    tys.push(self.field_of(m, *name)?);
                }
                let t = self.types.union(&tys);
                Some((target, t))
            }
            _ => None,
        }
    }

    /// Forgets narrowings at or under an assigned place (`t.left = x` invalidates `t.left.value`).
    pub(super) fn invalidate_place(&mut self, e: ExprId) {
        let mut path = Vec::new();
        let mut cur = e;
        loop {
            match &self.ast().expr(cur).kind {
                ExprKind::Paren(x) | ExprKind::NonNull(x) => cur = *x,
                ExprKind::Member { obj, name, .. } => {
                    path.push(*name);
                    cur = *obj;
                }
                ExprKind::Index { obj, .. } => {
                    // An element changed: forget everything under the collection's path.
                    path.clear();
                    cur = *obj;
                }
                ExprKind::Ident(s) => {
                    if let Some((id, _)) = self.lookup(*s) {
                        path.reverse();
                        self.narrow_path(id, path, None);
                    }
                    return;
                }
                _ => return,
            }
        }
    }
}

/// A reference that can be narrowed.
#[derive(Clone)]
pub(super) struct Target {
    local: LocalId,
    path: Vec<Sym>,
    /// The path goes through a class instance.
    heap: bool,
}

impl Target {
    fn with(self, ty: TyId) -> Narrowing {
        if self.path.is_empty() { Narrowing::Local(self.local, ty) } else { Narrowing::Path(self.local, self.path, ty, self.heap) }
    }
}

pub(super) enum Narrowing {
    Local(LocalId, TyId),
    Path(LocalId, Vec<Sym>, TyId, bool),
}

fn typeof_name(types: &Types, t: TyId) -> Option<&'static str> {
    if types.is_numeric(t) {
        Some("number")
    } else if t == BIGINT {
        Some("bigint")
    } else if t == DECIMAL {
        Some("decimal")
    } else if types.is_string(t) {
        Some("string")
    } else if t == BOOL {
        Some("boolean")
    } else if t == UNDEFINED {
        Some("undefined")
    } else if matches!(types.get(t), Ty::Func(..)) {
        Some("function")
    } else {
        Some("object")
    }
}

fn int_range(t: TyId) -> Option<(i128, i128)> {
    Some(match t {
        INT => (i64::MIN as i128, i64::MAX as i128),
        I8 => (i8::MIN as i128, i8::MAX as i128),
        I16 => (i16::MIN as i128, i16::MAX as i128),
        I32 => (i32::MIN as i128, i32::MAX as i128),
        U8 => (0, u8::MAX as i128),
        U16 => (0, u16::MAX as i128),
        U32 => (0, u32::MAX as i128),
        U64 => (0, u64::MAX as i128),
        _ => return None,
    })
}
