//! Classes: declarations, members, inheritance, constructors and the reference-cycle check.
//!
//! A class is a nominal reference type. Its `ClassInfo` lists every instance field in layout
//! order (inherited first) and every instance method visible on it (inherited ones substituted
//! into this class's type parameters), so member lookup never walks the base chain.

use super::*;
use crate::ast::{ClassDecl, ClassMember, ExprKind, FnDecl, MemberKind, StmtKind, Visibility};

#[derive(Clone, Debug)]
pub(crate) struct CField {
    pub(crate) name: Sym,
    /// In terms of this class's type parameters (optional fields include `undefined`).
    pub(crate) ty: TyId,
    pub(crate) optional: bool,
    pub(crate) readonly: bool,
    pub(crate) weak: bool,
    pub(crate) vis: Visibility,
    /// The class that declares the field.
    pub(crate) owner: u32,
    /// Member index in the owner's declaration (`None`: a constructor parameter property).
    pub(crate) member: Option<u32>,
    /// Constructor parameter index for parameter properties.
    pub(crate) prop_param: Option<u32>,
    pub(crate) has_init: bool,
    pub(crate) span: Span,
}

#[derive(Clone, Debug)]
pub(crate) struct CMethod {
    pub(crate) name: Sym,
    pub(crate) owner: u32,
    pub(crate) member: u32,
    pub(crate) params: Vec<FnParam>,
    pub(crate) param_names: Vec<Sym>,
    /// A generic static method's own type parameters.
    pub(crate) tparams: Vec<u32>,
    /// `None` until inferred on the owner (see `method_ret`).
    pub(crate) ret: Option<TyId>,
    /// Declared `throws` (or inferred like `ret`; `Some(NEVER)` when the body can't throw).
    pub(crate) throws: Option<TyId>,
    /// Owner's type parameters → this class's types.
    pub(crate) map: Vec<(u32, TyId)>,
    pub(crate) vis: Visibility,
    pub(crate) is_abstract: bool,
    pub(crate) getter: bool,
    pub(crate) span: Span,
    /// An async method: its body's view (the declared `Promise<T>`'s `T`, the declared `throws`).
    /// `ret`/`throws` are what callers see: `Promise<T, E>`, throwing nothing.
    pub(crate) async_body: Option<(Option<TyId>, Option<TyId>)>,
}

#[derive(Clone, Debug)]
pub(crate) struct CCtor {
    /// The class whose constructor body runs (the class itself when it declares none).
    pub(crate) member: Option<u32>,
    /// Errors the constructor can throw (declared, or inferred for the class's own constructor).
    pub(crate) throws: Option<TyId>,
    pub(crate) params: Vec<FnParam>,
    pub(crate) param_names: Vec<Sym>,
}

#[derive(Clone, Debug)]
pub(crate) struct ClassInfo {
    pub(crate) module: u32,
    pub(crate) item: u32,
    pub(crate) params: Vec<u32>,
    pub(crate) this_ty: TyId,
    /// The direct base class, in terms of this class's type parameters.
    pub(crate) base: Option<TyId>,
    pub(crate) fields: Vec<CField>,
    pub(crate) methods: Vec<CMethod>,
    pub(crate) statics: Vec<CMethod>,
    pub(crate) static_fields: Vec<CField>,
    pub(crate) ctor: CCtor,
    pub(crate) is_abstract: bool,
    pub(crate) cyclic: bool,
    pub(crate) resolved: bool,
}

/// A class member found by name, instantiated for a receiver type.
#[derive(Clone, Debug)]
pub(crate) enum ClassMemberRef {
    Field(CField),
    Getter(CMethod),
    Method(CMethod),
}

pub(crate) struct ThisInfo {
    pub(crate) ty: TyId,
    pub(crate) ctor: bool,
}

impl<'a> Checker<'a> {
    pub(crate) fn class_decl(&self, c: u32) -> &'a ClassDecl {
        let info = &self.classes[c as usize];
        match &self.modules[info.module as usize].ast.items[info.item as usize].kind {
            ItemKind::Class(cd) => cd,
            _ => unreachable!("class item"),
        }
    }

    pub(crate) fn class_member_decl(&self, c: u32, member: u32) -> &'a ClassMember {
        &self.class_decl(c).members[member as usize]
    }

    /// The function declaration of a method, getter or constructor member.
    pub(crate) fn member_fn(&self, c: u32, member: u32) -> &'a FnDecl {
        match &self.class_member_decl(c, member).kind {
            MemberKind::Method(f) | MemberKind::Getter(f) | MemberKind::Constructor(f) => f,
            MemberKind::Field { .. } => unreachable!("not a function member"),
        }
    }

    /// An empty span at the keyword `kw` (`class` or `abstract`) of a class declaration.
    fn class_kw_at(&self, c: u32, kw: &str) -> Span {
        let info = &self.classes[c as usize];
        let item_span = self.modules[info.module as usize].ast.items[info.item as usize].span;
        let name = self.class_decl(c).name_span;
        let text = &self.sm.get(item_span.file).text[item_span.start as usize..name.start as usize];
        let off = text.find(kw).unwrap_or(0) as u32;
        Span::new(item_span.file, item_span.start + off, item_span.start + off)
    }

    /// Resolves a class's interface (fields, method signatures, base) once.
    pub(crate) fn resolve_class(&mut self, c: u32) {
        if self.classes[c as usize].resolved {
            return;
        }
        if !self.class_resolving.insert(c) {
            let cd = self.class_decl(c);
            let n = self.name(cd.name).to_string();
            self.done.push(Diagnostic::new("T0805", cd.name_span, format!("class `{n}` extends itself (directly or through its base classes)")));
            return;
        }
        let m = self.classes[c as usize].module;
        let cd = self.class_decl(c);
        let info = self.committed(|ck| ck.with_module(m, |ck| ck.build_class_info(c, cd)));
        Arc::make_mut(&mut self.classes)[c as usize] = info;
        self.class_resolving.remove(&c);
    }

    fn build_class_info(&mut self, c: u32, cd: &'a ClassDecl) -> ClassInfo {
        let (module, item) = (self.classes[c as usize].module, self.classes[c as usize].item);
        let mut tscope = Vec::new();
        let mut params = Vec::new();
        for tp in &cd.tparams {
            let id = self.new_gparam(tp.name, None);
            tscope.push((tp.name, self.types.intern(Ty::Param(id))));
            params.push(id);
        }
        for (tp, &id) in cd.tparams.iter().zip(&params) {
            if let Some(b) = tp.bound {
                let bound = self.resolve_type(b, &tscope);
                let base = self.gparams_base.len();
                self.gparams[id as usize - base].bound = Some(bound);
            }
        }
        let args: Vec<TyId> = tscope.iter().map(|(_, t)| *t).collect();
        let this_ty = self.types.class(c, &args);
        let class_name = self.name(cd.name).to_string();
        let mut info = ClassInfo {
            module,
            item,
            params: params.clone(),
            this_ty,
            base: None,
            fields: Vec::new(),
            methods: Vec::new(),
            statics: Vec::new(),
            static_fields: Vec::new(),
            ctor: CCtor { member: None, throws: Some(NEVER), params: Vec::new(), param_names: Vec::new() },
            is_abstract: cd.is_abstract,
            cyclic: cd.cyclic,
            resolved: true,
        };
        // Base class.
        if let Some(te) = cd.extends {
            let bt = self.resolve_type(te, &tscope);
            match *self.types.get(bt) {
                Ty::Class(b, bargs) => {
                    self.resolve_class(b);
                    let bargs = self.types.tys(bargs).to_vec();
                    let binfo = self.classes[b as usize].clone();
                    if binfo.resolved {
                        let map: HashMap<u32, TyId> = binfo.params.iter().copied().zip(bargs.iter().copied()).collect();
                        info.base = Some(bt);
                        for f in &binfo.fields {
                            let ty = self.types.subst(f.ty, &map);
                            info.fields.push(CField { ty, ..f.clone() });
                        }
                        for mm in &binfo.methods {
                            info.methods.push(self.inherit_method(mm, &map));
                        }
                        let bc = &binfo.ctor;
                        let bparams: Vec<FnParam> = bc.params.iter().map(|p| FnParam { ty: self.types.subst(p.ty, &map), ..*p }).collect();
                        let throws = bc.throws.map(|t| self.types.subst(t, &map));
                        info.ctor = CCtor { member: None, throws, params: bparams, param_names: bc.param_names.clone() };
                    }
                }
                Ty::Error => {}
                _ => {
                    let span = self.ast().ty(te).span;
                    let shown = self.show(bt);
                    self.report(Diagnostic::new("T0805", span, format!("a class can only extend another class, found `{shown}`")).note("instead", "use `implements` for interfaces"));
                }
            }
        }
        let inherited_fields = info.fields.len();
        // Constructor first: parameter properties are the first own fields.
        let mut own_ctor = None;
        for (mi, mem) in cd.members.iter().enumerate() {
            if let MemberKind::Constructor(f) = &mem.kind {
                if own_ctor.is_some() {
                    self.report(Diagnostic::new("N0006", mem.name_span, "a class has at most one constructor").note("instead", "use optional parameters, or static factory methods"));
                    continue;
                }
                own_ctor = Some(mi as u32);
                let (ps, names) = self.member_params(f, &tscope);
                for (pi, p) in f.params.iter().enumerate() {
                    if let Some((vis, readonly)) = p.prop {
                        if p.inout {
                            self.report(Diagnostic::new("T0808", p.span, "a parameter property can't be `inout`"));
                        }
                        info.fields.push(CField {
                            name: p.name,
                            ty: ps[pi].ty,
                            optional: p.optional,
                            readonly,
                            weak: false,
                            vis,
                            owner: c,
                            member: None,
                            prop_param: Some(pi as u32),
                            has_init: true,
                            span: p.span,
                        });
                    }
                }
                let throws = self.member_throws_decl(f, &tscope);
                info.ctor = CCtor { member: Some(mi as u32), throws, params: ps, param_names: names };
            }
        }
        if own_ctor.is_none() && info.base.is_none() {
            info.ctor = CCtor { member: None, throws: Some(NEVER), params: Vec::new(), param_names: Vec::new() };
        }
        // Fields and methods.
        for (mi, mem) in cd.members.iter().enumerate() {
            let mi = mi as u32;
            let n = self.name(mem.name).to_string();
            match &mem.kind {
                MemberKind::Constructor(_) => {}
                MemberKind::Field { ty, init, optional } => {
                    let declared = match ty {
                        Some(t) => {
                            let scope: &[(Sym, TyId)] = if mem.is_static { &[] } else { &tscope };
                            Some(self.resolve_type(*t, scope))
                        }
                        None => None,
                    };
                    let fty = match (declared, init) {
                        (Some(t), _) => t,
                        (None, Some(e)) => {
                            // Infer from the initializer (checked again with `this` in phase B).
                            self.fcx.push(FnCtx::default());
                            self.fcx.last_mut().unwrap().scopes.push(Vec::new());
                            let t = self.expr(*e, None);
                            self.fcx.pop();
                            let span = self.ast().expr(*e).span;
                            let t = self.check_inferred_binding(t, span);
                            self.types.widen(t)
                        }
                        (None, None) => {
                            self.report(
                                Diagnostic::new("T0301", mem.name_span, format!("field `{n}` needs a type annotation or an initial value"))
                                    .fix(Applicability::Placeholder, format!("annotate `{n}: T`"), mem.name_span.empty_at_end(), ": T"),
                            );
                            ERROR
                        }
                    };
                    let fty = if *optional { self.types.optional(fty) } else { fty };
                    if mem.is_static {
                        if !mem.readonly {
                            self.report(
                                Diagnostic::new("T0816", mem.name_span, format!("static field `{n}` must be `static readonly`"))
                                    .note("why", "mutable module-level state is not allowed")
                                    .fix(Applicability::Maybe, "make it `static readonly`", mem.name_span.empty_at_start(), "readonly "),
                            );
                        }
                        if init.is_none() {
                            self.report(Diagnostic::new("T0309", mem.name_span, format!("static field `{n}` needs a value")));
                        }
                        info.static_fields.push(CField {
                            name: mem.name,
                            ty: fty,
                            optional: *optional,
                            readonly: true,
                            weak: false,
                            vis: mem.vis,
                            owner: c,
                            member: Some(mi),
                            prop_param: None,
                            has_init: init.is_some(),
                            span: mem.name_span,
                        });
                        continue;
                    }
                    if mem.weak && !self.weak_ok(fty) {
                        let shown = self.show(fty);
                        self.report(
                            Diagnostic::new("T0815", mem.name_span, format!("a `weak` field must have type `C | undefined` for a class `C`, found `{shown}`"))
                                .note("why", "a weak reference doesn't keep its target alive, so it reads as `undefined` once the target is freed"),
                        );
                    }
                    if let Some(prev) = info.fields.iter().find(|f| f.name == mem.name) {
                        let what = if prev.owner == c { "is already declared in this class" } else { "is already declared in a base class" };
                        self.report(Diagnostic::new("T0808", mem.name_span, format!("field `{n}` {what}")));
                        continue;
                    }
                    info.fields.push(CField {
                        name: mem.name,
                        ty: fty,
                        optional: *optional,
                        readonly: mem.readonly,
                        weak: mem.weak,
                        vis: mem.vis,
                        owner: c,
                        member: Some(mi),
                        prop_param: None,
                        has_init: init.is_some(),
                        span: mem.name_span,
                    });
                }
                MemberKind::Method(f) | MemberKind::Getter(f) => {
                    let getter = matches!(mem.kind, MemberKind::Getter(_));
                    let mut scope: Vec<(Sym, TyId)> = if mem.is_static { Vec::new() } else { tscope.clone() };
                    let mut mtparams = Vec::new();
                    if !getter {
                        for tp in &f.tparams {
                            let id = self.new_gparam(tp.name, None);
                            scope.push((tp.name, self.types.intern(Ty::Param(id))));
                            mtparams.push(id);
                        }
                    }
                    let (ps, names) = self.member_params(f, &scope);
                    if getter && !ps.is_empty() {
                        self.report(Diagnostic::new("T0818", mem.name_span, format!("getter `{n}` can't take parameters")));
                    }
                    let ret = match f.ret {
                        Some(t) => Some(self.resolve_type(t, &scope)),
                        None if mem.is_abstract => {
                            self.report(Diagnostic::new("T0301", mem.name_span, format!("abstract method `{n}` needs a return type annotation")));
                            Some(ERROR)
                        }
                        None if !super::returns_value(self.ast(), f.body) => Some(VOID),
                        None => None,
                    };
                    if mem.is_abstract && !cd.is_abstract {
                        self.report(
                            Diagnostic::new("T0818", mem.name_span, format!("abstract method `{n}` in a class that isn't abstract"))
                                .fix(Applicability::Maybe, format!("declare `abstract class {class_name}`"), self.class_kw_at(c, "class"), "abstract "),
                        );
                    }
                    let map: Vec<(u32, TyId)> = params.iter().copied().zip(args.iter().copied()).collect();
                    let throws = if mem.is_abstract { f.throws.map(|t| self.resolve_type(t, &scope)).or(Some(NEVER)) } else { self.member_throws_decl(f, &scope) };
                    // An async method returns a promise; its errors reject it. Until both the
                    // body's type and errors are known, callers' view is inferred with the body.
                    let (ret, throws, async_body) = if f.is_async && !getter {
                        let inner = match (ret, f.ret) {
                            (Some(r), Some(te)) => Some(self.async_inner(r, self.ast().ty(te).span)),
                            (r, _) => r,
                        };
                        match (inner, throws) {
                            (Some(i), Some(t)) => (Some(self.types.promise(i, t)), Some(NEVER), Some((inner, throws))),
                            _ => (None, None, Some((inner, throws))),
                        }
                    } else {
                        (ret, throws, None)
                    };
                    let method = CMethod {
                        name: mem.name,
                        owner: c,
                        member: mi,
                        params: ps,
                        param_names: names,
                        tparams: mtparams,
                        ret,
                        throws,
                        map,
                        vis: mem.vis,
                        is_abstract: mem.is_abstract,
                        getter,
                        span: mem.name_span,
                        async_body,
                    };
                    if mem.is_static {
                        if info.statics.iter().any(|s| s.name == mem.name) {
                            self.report(Diagnostic::new("N0006", mem.name_span, format!("static member `{n}` is declared twice")));
                        } else {
                            info.statics.push(method);
                        }
                        continue;
                    }
                    if info.fields.iter().any(|f| f.name == mem.name) {
                        self.report(Diagnostic::new("T0808", mem.name_span, format!("`{n}` is already declared as a field")));
                        continue;
                    }
                    match info.methods.iter().position(|x| x.name == mem.name) {
                        Some(i) if info.methods[i].owner == c => {
                            self.report(Diagnostic::new("N0006", mem.name_span, format!("method `{n}` is declared twice")).note("instead", "Tov has no overloads; use optional parameters or different names"));
                        }
                        Some(i) => {
                            let prev = info.methods[i].clone();
                            self.check_override(&prev, &method, &n);
                            info.methods[i] = method;
                        }
                        None => info.methods.push(method),
                    }
                }
            }
        }
        // Fields declared in a base class after parameter properties keep the inherited-first order.
        let _ = inherited_fields;
        info
    }

    fn inherit_method(&mut self, mm: &CMethod, map: &HashMap<u32, TyId>) -> CMethod {
        let params = mm.params.iter().map(|p| FnParam { ty: self.types.subst(p.ty, map), ..*p }).collect();
        let ret = mm.ret.map(|r| self.types.subst(r, map));
        let throws = mm.throws.map(|r| self.types.subst(r, map));
        let composed = mm.map.iter().map(|&(p, t)| (p, self.types.subst(t, map))).collect();
        let async_body = mm.async_body.map(|(r, t)| (r.map(|r| self.types.subst(r, map)), t.map(|t| self.types.subst(t, map))));
        CMethod { params, ret, throws, map: composed, async_body, ..mm.clone() }
    }

    /// A member's declared `throws`, or `None` (inferred later) when its body can throw.
    fn member_throws_decl(&mut self, f: &FnDecl, scope: &[(Sym, TyId)]) -> Option<TyId> {
        match f.throws {
            Some(t) => {
                let ty = self.resolve_type(t, scope);
                let span = self.ast().ty(t).span;
                self.check_throws_type(ty, span);
                Some(ty)
            }
            None if super::may_throw(self.ast(), f.body) => None,
            None => Some(NEVER),
        }
    }

    /// The errors a method can throw (inferring the owner's body when needed).
    pub(crate) fn method_throws(&mut self, mm: &CMethod) -> TyId {
        if let Some(t) = mm.throws {
            return t;
        }
        let own = self.classes[mm.owner as usize].methods.iter().chain(self.classes[mm.owner as usize].statics.iter()).find(|x| x.member == mm.member && x.owner == mm.owner).and_then(|x| x.throws);
        let t = match own {
            Some(t) => t,
            None => {
                self.infer_method(mm.owner, mm.member);
                self.classes[mm.owner as usize].methods.iter().chain(self.classes[mm.owner as usize].statics.iter()).find(|x| x.member == mm.member && x.owner == mm.owner).and_then(|x| x.throws).unwrap_or(ERROR)
            }
        };
        let map: HashMap<u32, TyId> = mm.map.iter().copied().collect();
        self.types.subst(t, &map)
    }

    /// The errors `new C(...)` can throw.
    pub(crate) fn ctor_throws(&mut self, c: u32) -> TyId {
        self.resolve_class(c);
        if let Some(t) = self.classes[c as usize].ctor.throws {
            return t;
        }
        match self.classes[c as usize].ctor.member {
            Some(mi) => {
                if !self.checked_members.contains(&(c, mi)) && !self.checked_members_base.contains(&(c, mi)) {
                    self.check_member_body(c, mi, None);
                }
                self.classes[c as usize].ctor.throws.unwrap_or(ERROR)
            }
            None => NEVER,
        }
    }

    fn member_params(&mut self, f: &FnDecl, scope: &[(Sym, TyId)]) -> (Vec<FnParam>, Vec<Sym>) {
        let mut ps = Vec::new();
        let mut names = Vec::new();
        for p in &f.params {
            if p.rest {
                self.report(Diagnostic::new("U0011", p.span, "rest parameters in methods are not supported yet; take an array parameter"));
            }
            let ty = match p.ty {
                Some(t) => self.resolve_type(t, scope),
                None => {
                    let n = self.name(p.name).to_string();
                    self.report(
                        Diagnostic::new("T0301", p.span, format!("parameter `{n}` needs a type annotation"))
                            .fix(Applicability::Placeholder, format!("annotate `{n}: T`"), p.span.empty_at_end(), ": T"),
                    );
                    ERROR
                }
            };
            let ty = if p.optional { self.types.optional(ty) } else { ty };
            ps.push(FnParam { ty, inout: p.inout, optional: p.optional });
            names.push(p.name);
        }
        (ps, names)
    }

    /// `weak` needs `C | undefined` for a class `C`.
    fn weak_ok(&mut self, ty: TyId) -> bool {
        if ty == ERROR {
            return true;
        }
        let ms = self.types.members(ty);
        ms.contains(&UNDEFINED) && ms.len() == 2 && ms.iter().all(|&m| m == UNDEFINED || matches!(self.types.get(m), Ty::Class(..)))
    }

    fn check_override(&mut self, base: &CMethod, over: &CMethod, n: &str) {
        let span = over.span;
        if !base.tparams.is_empty() || !over.tparams.is_empty() {
            self.report(Diagnostic::new("U0015", span, format!("generic method `{n}` can't be overridden (or override another method) yet")));
            return;
        }
        if base.getter != over.getter {
            let (a, b) = if base.getter { ("a getter", "a method") } else { ("a method", "a getter") };
            self.report(Diagnostic::new("T0807", span, format!("`{n}` is {a} in the base class but {b} here")));
            return;
        }
        let ok_params = base.params.len() == over.params.len()
            && base.params.iter().zip(&over.params).all(|(b, o)| b.inout == o.inout && b.optional == o.optional && self.assignable(b.ty, o.ty));
        // Inferred return types are compared once the class is resolved (see `check_class_interface`).
        let (Some(bret), Some(oret)) = (base.ret, over.ret) else {
            if ok_params {
                self.pending_overrides.push((base.clone(), over.clone()));
            } else {
                let bs = self.method_desc(base, n);
                let os = self.method_desc(over, n);
                self.report(Diagnostic::new("T0807", span, format!("`{n}` doesn't match the method it overrides")).note("base", bs).note("here", os));
            }
            return;
        };
        let ok_ret = oret == ERROR || bret == ERROR || bret == VOID || self.assignable(oret, bret);
        if !ok_params || !ok_ret {
            let bs = self.method_desc(base, n);
            let os = self.method_desc(over, n);
            self.report(
                Diagnostic::new("T0807", span, format!("`{n}` doesn't match the method it overrides"))
                    .note("base", bs)
                    .note("here", os)
                    .note("rule", "an override takes the same parameters (or wider types) and returns the same type (or a narrower one)"),
            );
        }
    }

    fn method_desc(&mut self, mm: &CMethod, n: &str) -> String {
        let ps: Vec<String> = mm.params.iter().zip(&mm.param_names).map(|(p, pn)| format!("{}{}: {}", self.name(*pn), if p.optional { "?" } else { "" }, self.show(p.ty))).collect();
        let r = match mm.ret {
            Some(r) => self.show(r),
            None => "(inferred)".to_string(),
        };
        format!("{n}({}): {r}", ps.join(", "))
    }

    /// A method's return type (inferring the owner's body when it has no annotation).
    pub(crate) fn method_ret(&mut self, mm: &CMethod) -> TyId {
        if let Some(r) = mm.ret {
            return r;
        }
        let own = self.classes[mm.owner as usize].methods.iter().chain(self.classes[mm.owner as usize].statics.iter()).find(|x| x.member == mm.member && x.owner == mm.owner).and_then(|x| x.ret);
        let r = match own {
            Some(r) => r,
            None => self.infer_method(mm.owner, mm.member),
        };
        let map: HashMap<u32, TyId> = mm.map.iter().copied().collect();
        self.types.subst(r, &map)
    }

    /// Infers an unannotated method's return type from its body (phase A).
    fn infer_method(&mut self, c: u32, member: u32) -> TyId {
        if self.checked_members.contains(&(c, member)) || self.checked_members_base.contains(&(c, member)) {
            // Being inferred right now (recursion), or already failed.
            let f = self.member_fn(c, member);
            let n = self.name(f.name).to_string();
            self.report(Diagnostic::new("T0303", f.name_span, format!("method `{n}` is recursive, so it needs an explicit return type")));
            return ERROR;
        }
        let r = self.check_member_body(c, member, None);
        let t = self.inferred_throws.remove(&(c, member)).unwrap_or(NEVER);
        let own = self.classes[c as usize].methods.iter().chain(self.classes[c as usize].statics.iter()).find(|x| x.owner == c && x.member == member).and_then(|x| x.async_body);
        // An async method: callers get a promise of the body's value that rejects with its errors.
        let (r, t) = match own {
            Some((_, declared)) => (self.types.promise(r, declared.unwrap_or(t)), NEVER),
            None => (r, t),
        };
        let info = &mut Arc::make_mut(&mut self.classes)[c as usize];
        for x in info.methods.iter_mut().chain(info.statics.iter_mut()) {
            if x.owner == c && x.member == member {
                if x.ret.is_none() {
                    x.ret = Some(r);
                }
                if x.throws.is_none() {
                    x.throws = Some(t);
                }
            }
        }
        r
    }

    /// Checks a method, getter or constructor body; returns its (declared or inferred) return type.
    pub(crate) fn check_member_body(&mut self, c: u32, member: u32, ret: Option<TyId>) -> TyId {
        self.checked_members.insert((c, member));
        let m = self.classes[c as usize].module;
        let mem = self.class_member_decl(c, member);
        let f = self.member_fn(c, member);
        let info = self.classes[c as usize].clone();
        let (params, ret, throws, is_ctor) = match &mem.kind {
            MemberKind::Constructor(_) => (info.ctor.params.clone(), Some(VOID), info.ctor.throws, true),
            _ => {
                let mm = info.methods.iter().chain(info.statics.iter()).find(|x| x.owner == c && x.member == member).cloned();
                match mm {
                    // An async method's body is checked against its promise's value type.
                    Some(CMethod { async_body: Some((inner, throws)), ref params, .. }) => (params.clone(), ret.or(inner), throws, false),
                    Some(mm) => (mm.params.clone(), ret.or(mm.ret), mm.throws, false),
                    None => return ERROR,
                }
            }
        };
        // The method's own type parameters (a generic method), after the class's.
        let mm = info.methods.iter().chain(info.statics.iter()).find(|x| x.owner == c && x.member == member);
        let own: Vec<(Sym, TyId)> = f.tparams.iter().zip(mm.map(|m| m.tparams.clone()).unwrap_or_default()).map(|(tp, id)| (tp.name, self.types.intern(Ty::Param(id)))).collect();
        let mut tscope: Vec<(Sym, TyId)> = if mem.is_static {
            Vec::new()
        } else {
            self.class_decl(c).tparams.iter().zip(&info.params).map(|(tp, &id)| (tp.name, self.types.intern(Ty::Param(id)))).collect()
        };
        tscope.extend(own);
        let this = if mem.is_static { None } else { Some(ThisInfo { ty: info.this_ty, ctor: is_ctor }) };
        let sig = Sig { tparams: Vec::new(), params, param_names: f.params.iter().map(|p| p.name).collect(), ret: ret.unwrap_or(ERROR), throws: throws.unwrap_or(UNKNOWN), rest: false };
        let (r, errs) = self.check_decl_body(m, f, &sig, &tscope, ret, this, Some(c));
        if throws.is_none() {
            if is_ctor {
                Arc::make_mut(&mut self.classes)[c as usize].ctor.throws = Some(errs);
            } else {
                self.inferred_throws.insert((c, member), errs);
            }
        }
        if is_ctor {
            self.with_module(m, |ck| ck.check_ctor_rules(c, f));
        }
        r
    }

    /// Field initializers run in the constructor: `this` is available, the declared type is expected.
    fn check_field_inits(&mut self, c: u32) {
        let info = self.classes[c as usize].clone();
        let cd = self.class_decl(c);
        let tscope: Vec<(Sym, TyId)> = cd.tparams.iter().zip(&info.params).map(|(tp, &id)| (tp.name, self.types.intern(Ty::Param(id)))).collect();
        for (mi, mem) in cd.members.iter().enumerate() {
            let MemberKind::Field { init: Some(e), .. } = &mem.kind else { continue };
            let (fty, is_static) = match info.fields.iter().find(|f| f.owner == c && f.member == Some(mi as u32)) {
                Some(f) => (f.ty, false),
                None => match info.static_fields.iter().find(|f| f.member == Some(mi as u32)) {
                    Some(f) => (f.ty, true),
                    None => continue,
                },
            };
            let mut fcx = FnCtx { tscope: if is_static { Vec::new() } else { tscope.clone() }, ..Default::default() };
            fcx.scopes.push(Vec::new());
            fcx.frames.push(Frame::new(None, None, false, None));
            fcx.class = Some(c);
            if !is_static {
                fcx.locals.push(Local { name: THIS_SYM, ty: info.this_ty, kind: LocalKind::Param, span: mem.name_span, kw_span: None, promotable: false, frame: 0, global: None });
                fcx.scopes[0].push((THIS_SYM, 0, None));
                fcx.this_key = Some(mem.name_span.start);
                fcx.ctor = true;
            }
            self.fcx.push(fcx);
            let t = self.expr(*e, Some(fty));
            let span = self.ast().expr(*e).span;
            self.expect_assignable(t, fty, span, None);
            self.check_shared_copies();
            self.fcx.pop();
        }
    }

    /// Constructor rules: `super(...)` first in derived classes, every field assigned, and
    /// `this` used only for field writes (and reads of assigned fields) until then.
    fn check_ctor_rules(&mut self, c: u32, f: &'a FnDecl) {
        let info = self.classes[c as usize].clone();
        let ast = self.ast();
        let StmtKind::Block(stmts) = &ast.stmt(f.body).kind else { return };
        let mut rest: &[StmtId] = stmts;
        if info.base.is_some() {
            let first_is_super = stmts.first().map(|&s| self.is_super_call_stmt(s)).unwrap_or(false);
            if !first_is_super {
                let span = stmts.first().map(|&s| ast.stmt(s).span).unwrap_or(f.name_span);
                self.report(
                    Diagnostic::new("T0814", span, "a derived class's constructor must start with `super(...)`")
                        .fix(Applicability::Placeholder, "call the base constructor first", span.empty_at_start(), "super(<args>)\n    "),
                );
            } else {
                rest = &stmts[1..];
            }
        }
        for &s in rest {
            if self.super_call_in_stmt(s) {
                let span = ast.stmt(s).span;
                self.report(Diagnostic::new("T0814", span, "`super(...)` can only be the first statement of the constructor"));
            }
        }
        // Fields that must be assigned: own, without initializer, not optional.
        let mut pending: Vec<Sym> = info.fields.iter().filter(|fl| fl.owner == c && !fl.has_init && !fl.optional && !self.types.has_undefined(fl.ty)).map(|fl| fl.name).collect();
        let mut assigned: Vec<Sym> = info.fields.iter().filter(|fl| fl.owner != c || fl.has_init || fl.optional).map(|fl| fl.name).collect();
        for &s in rest {
            if pending.is_empty() {
                break;
            }
            // `this.f = <value not using this>`: assigns f.
            if let StmtKind::Expr(e) = &ast.stmt(s).kind
                && let ExprKind::Assign(crate::ast::AssignOp::Assign, t, v) = &ast.expr(*e).kind
                && let ExprKind::Member { obj, name, .. } = &ast.expr(*t).kind
                && matches!(ast.expr(*obj).kind, ExprKind::This)
            {
                self.check_this_uses(*v, &assigned, &pending);
                pending.retain(|n| n != name);
                assigned.push(*name);
                continue;
            }
            let mut exprs = Vec::new();
            ctor_stmt_exprs(ast, s, &mut exprs);
            for e in exprs {
                self.check_this_uses(e, &assigned, &pending);
            }
            // Assignments nested in control flow count once they're unconditional; keep it simple.
        }
        for n in pending {
            let fl = info.fields.iter().find(|fl| fl.name == n).unwrap().clone();
            let fname = self.name(n).to_string();
            let shown = self.show(fl.ty);
            self.report(
                Diagnostic::new("T0812", fl.span, format!("field `{fname}` is never assigned in the constructor"))
                    .note("why", "every field needs a value before the object is used")
                    .note("instead", format!("assign `this.{fname} = ...` at the top level of the constructor, give it an initializer, or make it optional (`{fname}?: {shown}`)")),
            );
        }
    }

    fn is_super_call_stmt(&self, s: StmtId) -> bool {
        let ast = self.ast();
        match &ast.stmt(s).kind {
            StmtKind::Expr(e) => matches!(&ast.expr(*e).kind, ExprKind::Call { callee, .. } if matches!(ast.expr(*callee).kind, ExprKind::Super)),
            _ => false,
        }
    }

    fn super_call_in_stmt(&self, s: StmtId) -> bool {
        let ast = self.ast();
        let mut exprs = Vec::new();
        ctor_stmt_exprs(ast, s, &mut exprs);
        exprs.iter().any(|&e| any_expr(ast, e, &|k| matches!(k, ExprKind::Call { callee, .. } if matches!(ast.expr(*callee).kind, ExprKind::Super))))
    }

    /// Reports uses of `this` other than reading already-assigned fields while fields are pending.
    fn check_this_uses(&mut self, e: ExprId, assigned: &[Sym], pending: &[Sym]) {
        if pending.is_empty() {
            return;
        }
        let ast = self.ast();
        let mut bad: Option<Span> = None;
        walk_this(ast, e, None, &mut |span, member| {
            let ok = matches!(member, Some(n) if assigned.contains(&n));
            if !ok && bad.is_none() {
                bad = Some(span);
            }
        });
        if let Some(span) = bad {
            let names: Vec<String> = pending.iter().map(|&n| format!("`{}`", self.name(n))).collect();
            self.report(
                Diagnostic::new("T0813", span, "`this` is used before every field is assigned")
                    .note("unassigned", names.join(", "))
                    .note("why", "methods (and anything `this` is passed to) could read fields that don't have values yet")
                    .note("instead", "assign every field first, then call methods or pass `this`"),
            );
        }
    }

    // ------------------------------------------------------------ phase A / B entry points

    pub(super) fn check_class_interface(&mut self, c: u32) {
        self.resolve_class(c);
        let info = self.classes[c as usize].clone();
        // Infer unannotated method return and error types now, so phase B only reads them.
        for mm in info.methods.iter().chain(info.statics.iter()) {
            if mm.owner == c && mm.ret.is_none() {
                self.method_ret(mm);
            }
            if mm.owner == c && mm.throws.is_none() {
                self.method_throws(mm);
            }
        }
        if info.ctor.throws.is_none() {
            self.ctor_throws(c);
        }
        let pending: Vec<(CMethod, CMethod)> = std::mem::take(&mut self.pending_overrides);
        for (base, over) in pending {
            if over.owner != c {
                self.pending_overrides.push((base, over));
                continue;
            }
            let (bret, oret) = (self.method_ret(&base), self.method_ret(&over));
            if !(oret == ERROR || bret == ERROR || bret == VOID || self.assignable(oret, bret)) {
                let n = self.name(over.name).to_string();
                let (bs, os) = (self.show(bret), self.show(oret));
                self.done.push(Diagnostic::new("T0807", over.span, format!("`{n}` returns `{os}`, but the method it overrides returns `{bs}`")));
            }
        }
        let cd = self.class_decl(c);
        let m = info.module;
        let n = self.name(cd.name).to_string();
        // A concrete class implements every abstract method.
        if !info.is_abstract {
            let missing: Vec<String> = info.methods.iter().filter(|mm| mm.is_abstract).map(|mm| self.name(mm.name).to_string()).collect();
            if !missing.is_empty() {
                self.done.push(
                    Diagnostic::new("T0806", cd.name_span, format!("class `{n}` doesn't implement abstract method{} {}", if missing.len() == 1 { "" } else { "s" }, missing.iter().map(|s| format!("`{s}`")).collect::<Vec<_>>().join(", ")))
                        .fix(Applicability::Maybe, format!("declare `abstract class {n}`"), self.class_kw_at(c, "class"), "abstract "),
                );
            }
        }
        // `implements`: structural check against each interface.
        for &te in &cd.implements {
            let it = self.committed(|ck| ck.with_module(m, |ck| {
                let tscope: Vec<(Sym, TyId)> = cd.tparams.iter().zip(&info.params).map(|(tp, &id)| (tp.name, ck.types.intern(Ty::Param(id)))).collect();
                ck.resolve_type(te, &tscope)
            }));
            let span = self.modules[m as usize].ast.ty(te).span;
            match *self.types.get(it) {
                Ty::Interface(i, args) => {
                    let args = self.types.tys(args).to_vec();
                    let fields = self.iface_fields_inst(i, &args);
                    let mut problems = Vec::new();
                    for f in &fields {
                        match self.class_member(info.this_ty, f.name) {
                            Some(mr) => {
                                let t = self.member_type(&mr);
                                if !self.assignable(t, f.ty) {
                                    problems.push(format!("`{}` has type `{}`, expected `{}`", self.name(f.name), self.show(t), self.show(f.ty)));
                                }
                            }
                            None if f.optional => {}
                            None => problems.push(format!("missing `{}`", self.name(f.name))),
                        }
                    }
                    if !problems.is_empty() {
                        let shown = self.show(it);
                        self.done.push(Diagnostic::new("T0809", span, format!("class `{n}` doesn't implement `{shown}`")).note("problems", problems.join("; ")));
                    }
                }
                Ty::Error => {}
                _ => {
                    let shown = self.show(it);
                    self.done.push(Diagnostic::new("T0809", span, format!("`implements` needs an interface, found `{shown}`")));
                }
            }
        }
    }

    pub(super) fn check_class_bodies(&mut self, c: u32) {
        let cd = self.class_decl(c);
        for (mi, mem) in cd.members.iter().enumerate() {
            let mi = mi as u32;
            match &mem.kind {
                MemberKind::Field { .. } => {}
                MemberKind::Method(_) | MemberKind::Getter(_) | MemberKind::Constructor(_) => {
                    if mem.is_abstract || self.checked_members.contains(&(c, mi)) || self.checked_members_base.contains(&(c, mi)) {
                        continue;
                    }
                    self.check_member_body(c, mi, None);
                }
            }
        }
        let m = self.classes[c as usize].module;
        self.committed(|ck| ck.with_module(m, |ck| ck.check_field_inits(c)));
        // Without a constructor, every field needs an initializer (or `?`).
        let info = self.classes[c as usize].clone();
        if info.ctor.member.is_none() {
            let missing: Vec<CField> = info.fields.iter().filter(|fl| fl.owner == c && !fl.has_init && !fl.optional && !self.types.has_undefined(fl.ty)).cloned().collect();
            for fl in missing {
                let fname = self.name(fl.name).to_string();
                self.done.push(
                    Diagnostic::new("T0812", fl.span, format!("field `{fname}` has no initial value"))
                        .note("instead", format!("give it an initializer (`{fname}: T = ...`), assign it in a constructor, or make it optional (`{fname}?: T`)")),
                );
            }
        }
    }

    // ------------------------------------------------------------ member lookup

    /// The class a type refers to directly (`C<T>`), if any.
    pub(crate) fn class_of(&self, ty: TyId) -> Option<(u32, Vec<TyId>)> {
        match *self.types.get(ty) {
            Ty::Class(c, args) => Some((c, self.types.tys(args).to_vec())),
            _ => None,
        }
    }

    fn inst_map(&self, c: u32, args: &[TyId]) -> HashMap<u32, TyId> {
        self.classes[c as usize].params.iter().copied().zip(args.iter().copied()).collect()
    }

    /// Looks up an instance member on a class type, instantiated for its type arguments.
    pub(crate) fn class_member(&mut self, ty: TyId, name: Sym) -> Option<ClassMemberRef> {
        let (c, args) = self.class_of(ty)?;
        self.resolve_class(c);
        let map = self.inst_map(c, &args);
        let info = &self.classes[c as usize];
        if let Some(f) = info.fields.iter().find(|f| f.name == name).cloned() {
            let ty = self.types.subst(f.ty, &map);
            return Some(ClassMemberRef::Field(CField { ty, ..f }));
        }
        let mm = info.methods.iter().find(|m| m.name == name).cloned()?;
        let inst = self.inherit_method(&mm, &map);
        Some(if inst.getter { ClassMemberRef::Getter(inst) } else { ClassMemberRef::Method(inst) })
    }

    /// The value type of a member access (a method is its function type).
    pub(crate) fn member_type(&mut self, mr: &ClassMemberRef) -> TyId {
        match mr {
            ClassMemberRef::Field(f) => f.ty,
            ClassMemberRef::Getter(m) => self.method_ret(m),
            ClassMemberRef::Method(m) => {
                let r = self.method_ret(m);
                self.types.func(m.params.clone(), r)
            }
        }
    }

    /// Is a member with this owner and visibility accessible from the current code?
    pub(crate) fn check_visible(&mut self, owner: u32, vis: Visibility, name: Sym, span: Span) -> bool {
        let cur = self.fcx.last().and_then(|f| f.class);
        let ok = match vis {
            Visibility::Public => true,
            Visibility::Private => cur == Some(owner),
            Visibility::Protected => cur.map(|c| self.class_descends(c, owner)).unwrap_or(false),
        };
        if !ok {
            let n = self.name(name).to_string();
            let cls = self.class_names[owner as usize].clone();
            let what = if vis == Visibility::Private { "private" } else { "protected" };
            let scope = if vis == Visibility::Private { format!("inside `{cls}`") } else { format!("inside `{cls}` and its subclasses") };
            self.report(Diagnostic::new("T0810", span, format!("`{n}` is {what} to `{cls}`")).note("why", format!("it can only be used {scope}")));
        }
        ok
    }

    /// Is class `a` the same as, or derived from, class `b`?
    pub(crate) fn class_descends(&mut self, a: u32, b: u32) -> bool {
        let mut cur = Some(a);
        let mut guard = 0;
        while let Some(c) = cur {
            if c == b {
                return true;
            }
            guard += 1;
            if guard > 1000 {
                return false;
            }
            self.resolve_class(c);
            cur = self.classes[c as usize].base.and_then(|bt| self.class_of(bt)).map(|(b, _)| b);
        }
        false
    }

    /// `ty` (a class type) viewed as its ancestor class `target`, with type arguments.
    pub(crate) fn upcast_to(&mut self, ty: TyId, target: u32) -> Option<TyId> {
        let mut cur = ty;
        for _ in 0..1000 {
            let (c, args) = self.class_of(cur)?;
            if c == target {
                return Some(cur);
            }
            self.resolve_class(c);
            let base = self.classes[c as usize].base?;
            let map = self.inst_map(c, &args);
            cur = self.types.subst(base, &map);
        }
        None
    }

    /// Class-to-class assignability: nominal, through the base chain, with invariant arguments.
    pub(crate) fn class_assignable(&mut self, src: TyId, dst: TyId) -> bool {
        let Some((dc, _)) = self.class_of(dst) else { return false };
        match self.upcast_to(src, dc) {
            Some(up) => up == dst,
            None => false,
        }
    }

    /// Fields, getters and methods of a class as interface-style fields (for `implements` and interface assignability).
    pub(crate) fn class_as_fields(&mut self, ty: TyId) -> Vec<Field> {
        let Some((c, _)) = self.class_of(ty) else { return Vec::new() };
        self.resolve_class(c);
        let names: Vec<Sym> = {
            let info = &self.classes[c as usize];
            info.fields.iter().filter(|f| f.vis == Visibility::Public).map(|f| f.name).chain(info.methods.iter().filter(|m| m.vis == Visibility::Public).map(|m| m.name)).collect()
        };
        let mut out = Vec::new();
        for n in names {
            if let Some(mr) = self.class_member(ty, n) {
                let t = self.member_type(&mr);
                out.push(Field { name: n, ty: t, optional: false });
            }
        }
        out
    }

    /// Names of instance members (for suggestions).
    pub(crate) fn class_member_names(&mut self, ty: TyId) -> Vec<String> {
        let Some((c, _)) = self.class_of(ty) else { return Vec::new() };
        self.resolve_class(c);
        let info = &self.classes[c as usize];
        info.fields.iter().map(|f| f.name).chain(info.methods.iter().map(|m| m.name)).map(|s| self.interner.get(s).to_string()).collect()
    }

    // ------------------------------------------------------------ reference cycles

    /// Reference counting can't free cycles. Reports class graphs where instances can reference
    /// each other in a cycle through strong fields, unless every field on the cycle is
    /// `readonly` and holds a class reference directly (set once, in the constructor, to an object
    /// that already exists, so no cycle can form).
    pub(super) fn check_class_cycles(&mut self) {
        let n = self.classes.len();
        if n == 0 {
            return;
        }
        for c in 0..n as u32 {
            self.resolve_class(c);
        }
        // Instantiations reachable from field types; nodes are class types.
        let mut nodes: Vec<TyId> = Vec::new();
        let mut index: HashMap<TyId, usize> = HashMap::default();
        let mut work: Vec<TyId> = (0..n as u32).map(|c| self.classes[c as usize].this_ty).collect();
        // Edges: (from node, to class type, frozen, field owner class, field name).
        let mut edges: Vec<(usize, TyId, bool, u32, Sym)> = Vec::new();
        while let Some(t) = work.pop() {
            if index.contains_key(&t) || nodes.len() > 4096 {
                continue;
            }
            index.insert(t, nodes.len());
            nodes.push(t);
            let from = nodes.len() - 1;
            let Some((c, args)) = self.class_of(t) else { continue };
            let map = self.inst_map(c, &args);
            let fields = self.classes[c as usize].fields.clone();
            for f in fields {
                if f.weak {
                    continue;
                }
                let fty = self.types.subst(f.ty, &map);
                let mut direct = Vec::new();
                let mut nested = Vec::new();
                self.class_refs(fty, true, &mut direct, &mut nested, 0);
                for (tgt, is_direct) in direct.into_iter().map(|x| (x, true)).chain(nested.into_iter().map(|x| (x, false))) {
                    edges.push((from, tgt, f.readonly && is_direct, f.owner, f.name));
                    work.push(tgt);
                }
            }
        }
        // A field of type `B` can also hold any subclass of `B`.
        let mut adj: Vec<Vec<(usize, bool, u32, Sym)>> = vec![Vec::new(); nodes.len()];
        for &(from, tgt, frozen, owner, name) in &edges {
            let Some((tc, _)) = self.class_of(tgt) else { continue };
            for (i, &node) in nodes.iter().enumerate() {
                let Some((nc, _)) = self.class_of(node) else { continue };
                if node == tgt || (nc != tc && self.class_descends(nc, tc)) {
                    adj[from].push((i, frozen, owner, name));
                }
            }
        }
        // Strongly connected components (Tarjan, iterative enough for class graphs).
        let sccs = tarjan(&adj);
        // Subclasses of cyclic classes are cyclic too (they inherit the fields that form cycles).
        let _ = &sccs;
        let mut reported: HashSet<u32> = HashSet::default();
        for comp in sccs {
            let in_comp: HashSet<usize> = comp.iter().copied().collect();
            let internal: Vec<(usize, usize, bool, u32, Sym)> =
                comp.iter().flat_map(|&a| adj[a].iter().filter(|e| in_comp.contains(&e.0)).map(move |&(b, fr, o, nm)| (a, b, fr, o, nm))).collect();
            if internal.is_empty() {
                continue;
            }
            // Cycles only through frozen edges can't form at run time.
            let Some(&(a, _, _, owner, fname)) = internal.iter().find(|e| !e.2) else { continue };
            let comp_classes: Vec<u32> = comp.iter().filter_map(|&i| self.class_of(nodes[i]).map(|(c, _)| c)).collect();
            let cyclic = comp_classes.iter().any(|&c| self.classes[c as usize].cyclic);
            if cyclic {
                // Every class on a possible cycle takes part in cycle collection.
                for c in comp_classes {
                    Arc::make_mut(&mut self.classes)[c as usize].cyclic = true;
                }
                continue;
            }
            if !reported.insert(owner) {
                continue;
            }
            let Some((ac, _)) = self.class_of(nodes[a]) else { continue };
            let path = self.cycle_path(&adj, &nodes, &in_comp, a);
            let cls = self.class_names[ac as usize].clone();
            let fl = self.classes[owner as usize].fields.iter().find(|f| f.name == fname && f.owner == owner).cloned();
            let Some(fl) = fl else { continue };
            let fnm = self.name(fname).to_string();
            let owner_decl = self.class_decl(owner);
            let owner_name = self.name(owner_decl.name).to_string();
            let direct = {
                let mut direct = Vec::new();
                let mut nested = Vec::new();
                self.class_refs(fl.ty, true, &mut direct, &mut nested, 0);
                nested.is_empty() && !direct.is_empty()
            };
            let mut d = Diagnostic::new("T0820", fl.span, format!("instances of `{cls}` can reference each other in a cycle, which reference counting alone can't free"))
                .note("cycle", path)
                .note("fix: cycles are real", format!("declare `cyclic class {owner_name}`: its instances are also checked by the cycle collector"));
            if direct {
                d = d
                    .note("fix: a back-reference", format!("mark it `weak {fnm}` (a parent pointer, say): it then doesn't keep its target alive"))
                    .note("fix: set once", format!("make it `readonly {fnm}` and set it in the constructor: objects then only point to older objects"));
            } else {
                d = d.note("fix: a back-reference", "if another field points back (a parent pointer, say), mark that one `weak`");
            }
            let at = if owner_decl.is_abstract { self.class_kw_at(owner, "abstract") } else { self.class_kw_at(owner, "class") };
            d = d.fix(Applicability::Maybe, format!("declare `cyclic class {owner_name}`"), at, "cyclic ");
            self.done.push(d);
        }
        for c in 0..n as u32 {
            if self.classes[c as usize].cyclic {
                continue;
            }
            let mut cur = self.classes[c as usize].base.and_then(|b| self.class_of(b)).map(|(b, _)| b);
            let mut guard = 0;
            while let Some(b) = cur {
                if self.classes[b as usize].cyclic {
                    Arc::make_mut(&mut self.classes)[c as usize].cyclic = true;
                    break;
                }
                guard += 1;
                if guard > 1000 {
                    break;
                }
                cur = self.classes[b as usize].base.and_then(|bt| self.class_of(bt)).map(|(b, _)| b);
            }
        }
    }

    fn cycle_path(&self, adj: &[Vec<(usize, bool, u32, Sym)>], nodes: &[TyId], comp: &HashSet<usize>, start: usize) -> String {
        // BFS back to start within the component.
        let mut prev: HashMap<usize, (usize, Sym)> = HashMap::default();
        let mut queue = std::collections::VecDeque::new();
        queue.push_back(start);
        let mut end = None;
        'outer: while let Some(x) = queue.pop_front() {
            for &(y, _, _, name) in &adj[x] {
                if !comp.contains(&y) {
                    continue;
                }
                if y == start {
                    end = Some((x, name));
                    break 'outer;
                }
                if let std::collections::hash_map::Entry::Vacant(e) = prev.entry(y) {
                    e.insert((x, name));
                    queue.push_back(y);
                }
            }
        }
        let Some((last, last_name)) = end else { return self.show(nodes[start]) };
        let mut steps = vec![(last, last_name)];
        let mut cur = last;
        while cur != start {
            let (p, nm) = prev[&cur];
            steps.push((p, nm));
            cur = p;
        }
        steps.reverse();
        let mut out = self.show(nodes[start]);
        for (i, &(_, nm)) in steps.iter().enumerate() {
            let next = if i + 1 < steps.len() { steps[i + 1].0 } else { start };
            out.push_str(&format!(".{} → {}", self.name(nm), self.show(nodes[next])));
        }
        out
    }

    /// Class types reachable from a field type: `direct` when only unions separate the field from
    /// the reference, `nested` through arrays, maps, sets, records or functions.
    fn class_refs(&mut self, ty: TyId, top: bool, direct: &mut Vec<TyId>, nested: &mut Vec<TyId>, depth: u32) {
        if depth > 16 {
            return;
        }
        match *self.types.get(ty) {
            Ty::Class(..) => {
                if top { direct.push(ty) } else { nested.push(ty) }
            }
            Ty::Union(ms) => {
                for m in self.types.tys(ms).to_vec() {
                    self.class_refs(m, top, direct, nested, depth + 1);
                }
            }
            Ty::Array(e) | Ty::Set(e) | Ty::Promise(e, _) => self.class_refs(e, false, direct, nested, depth + 1),
            Ty::Map(k, v) => {
                self.class_refs(k, false, direct, nested, depth + 1);
                self.class_refs(v, false, direct, nested, depth + 1);
            }
            Ty::Record(fs) => {
                for f in self.types.fields(fs).to_vec() {
                    self.class_refs(f.ty, false, direct, nested, depth + 1);
                }
            }
            Ty::Rec(..) => {
                let u = self.unfold(ty);
                if u != ty {
                    self.class_refs(u, false, direct, nested, depth + 1);
                }
            }
            _ => {}
        }
    }
}

/// Tarjan's strongly connected components.
fn tarjan(adj: &[Vec<(usize, bool, u32, Sym)>]) -> Vec<Vec<usize>> {
    struct St<'g> {
        adj: &'g [Vec<(usize, bool, u32, Sym)>],
        index: Vec<Option<usize>>,
        low: Vec<usize>,
        on: Vec<bool>,
        stack: Vec<usize>,
        next: usize,
        out: Vec<Vec<usize>>,
    }
    fn visit(s: &mut St, v: usize) {
        s.index[v] = Some(s.next);
        s.low[v] = s.next;
        s.next += 1;
        s.stack.push(v);
        s.on[v] = true;
        for i in 0..s.adj[v].len() {
            let w = s.adj[v][i].0;
            match s.index[w] {
                None => {
                    visit(s, w);
                    s.low[v] = s.low[v].min(s.low[w]);
                }
                Some(iw) if s.on[w] => s.low[v] = s.low[v].min(iw),
                _ => {}
            }
        }
        if Some(s.low[v]) == s.index[v] {
            let mut comp = Vec::new();
            while let Some(w) = s.stack.pop() {
                s.on[w] = false;
                comp.push(w);
                if w == v {
                    break;
                }
            }
            s.out.push(comp);
        }
    }
    let n = adj.len();
    let mut s = St { adj, index: vec![None; n], low: vec![0; n], on: vec![false; n], stack: Vec::new(), next: 0, out: Vec::new() };
    for v in 0..n {
        if s.index[v].is_none() {
            visit(&mut s, v);
        }
    }
    s.out
}

/// Expressions a constructor statement evaluates (including nested statements and arrows).
fn ctor_stmt_exprs(ast: &Ast, s: StmtId, out: &mut Vec<ExprId>) {
    match &ast.stmt(s).kind {
        StmtKind::Expr(e) | StmtKind::Return(Some(e)) => out.push(*e),
        StmtKind::Let { init: Some(e), .. } => out.push(*e),
        StmtKind::If(c, t, e) => {
            out.push(*c);
            ctor_stmt_exprs(ast, *t, out);
            if let Some(e) = e {
                ctor_stmt_exprs(ast, *e, out);
            }
        }
        StmtKind::While(c, b) | StmtKind::DoWhile(b, c) => {
            out.push(*c);
            ctor_stmt_exprs(ast, *b, out);
        }
        StmtKind::For { init, cond, step, body } => {
            if let Some(i) = init {
                ctor_stmt_exprs(ast, *i, out);
            }
            out.extend([cond, step].into_iter().flatten().copied());
            ctor_stmt_exprs(ast, *body, out);
        }
        StmtKind::ForOf { iter, body, .. } => {
            out.push(*iter);
            ctor_stmt_exprs(ast, *body, out);
        }
        StmtKind::Switch(d, cases) => {
            out.push(*d);
            for c in cases {
                out.extend(c.test);
                for &x in &c.body {
                    ctor_stmt_exprs(ast, x, out);
                }
            }
        }
        StmtKind::Block(ss) => ss.iter().for_each(|&x| ctor_stmt_exprs(ast, x, out)),
        _ => {}
    }
}

fn children(ast: &Ast, e: ExprId) -> Vec<ExprId> {
    use crate::ast::ArrowBody;
    let mut out = Vec::new();
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
        ExprKind::Arrow(f) => match &f.body {
            ArrowBody::Expr(x) => out.push(*x),
            ArrowBody::Block(b) => ctor_stmt_exprs(ast, *b, &mut out),
        },
        _ => {}
    }
    out
}

fn any_expr(ast: &Ast, e: ExprId, pred: &dyn Fn(&ExprKind) -> bool) -> bool {
    pred(&ast.expr(e).kind) || children(ast, e).into_iter().any(|x| any_expr(ast, x, pred))
}

/// Calls `f(span, Some(field))` for `this.field` reads and `f(span, None)` for any other use of `this`.
fn walk_this(ast: &Ast, e: ExprId, parent_member: Option<(Sym, bool)>, f: &mut dyn FnMut(Span, Option<Sym>)) {
    match &ast.expr(e).kind {
        ExprKind::This => match parent_member {
            Some((name, false)) => f(ast.expr(e).span, Some(name)),
            _ => f(ast.expr(e).span, None),
        },
        ExprKind::Member { obj, name, .. } => walk_this(ast, *obj, Some((*name, false)), f),
        ExprKind::Call { callee, args, .. } => {
            if let ExprKind::Member { obj, .. } = &ast.expr(*callee).kind {
                // `this.m(...)`: a method call passes `this`.
                walk_this(ast, *obj, None, f);
            } else {
                walk_this(ast, *callee, None, f);
            }
            for a in args {
                walk_this(ast, a.expr, None, f);
            }
        }
        _ => {
            for x in children(ast, e) {
                walk_this(ast, x, None, f);
            }
        }
    }
}
