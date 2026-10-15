//! Syntax tree for one module. Nodes live in flat arenas and refer to each other by index.

use crate::intern::Sym;
use crate::source::Span;

pub type ExprId = u32;
pub type StmtId = u32;
pub type TypeId = u32;

#[derive(Default)]
pub struct Ast {
    pub exprs: Vec<Expr>,
    pub stmts: Vec<Stmt>,
    pub types: Vec<TypeExpr>,
    pub items: Vec<Item>,
    /// A script (a module with top-level statements): the item index of the synthesized
    /// function `<script>` that runs them, with module variables initialized in source order.
    pub script: Option<u32>,
    /// Where the first top-level statement is (for "statements only in the entry file").
    pub script_span: Option<Span>,
}

impl Ast {
    pub fn expr(&self, id: ExprId) -> &Expr {
        &self.exprs[id as usize]
    }
    pub fn stmt(&self, id: StmtId) -> &Stmt {
        &self.stmts[id as usize]
    }
    pub fn ty(&self, id: TypeId) -> &TypeExpr {
        &self.types[id as usize]
    }
}

pub struct Expr {
    pub kind: ExprKind,
    pub span: Span,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    Pow,
    /// `==` (rejected: use `===`)
    LooseEq,
    /// `!=` (rejected: use `!==`)
    LooseNe,
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    And,
    Or,
    Nullish,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    UShr,
    In,
    Instanceof,
}

impl BinOp {
    pub fn as_str(self) -> &'static str {
        use BinOp::*;
        match self {
            Add => "+",
            Sub => "-",
            Mul => "*",
            Div => "/",
            Rem => "%",
            Pow => "**",
            LooseEq => "==",
            LooseNe => "!=",
            Eq => "===",
            Ne => "!==",
            Lt => "<",
            Gt => ">",
            Le => "<=",
            Ge => ">=",
            And => "&&",
            Or => "||",
            Nullish => "??",
            BitAnd => "&",
            BitOr => "|",
            BitXor => "^",
            Shl => "<<",
            Shr => ">>",
            UShr => ">>>",
            In => "in",
            Instanceof => "instanceof",
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum UnOp {
    Neg,
    Plus,
    Not,
    BitNot,
    Void,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum AssignOp {
    Assign,
    Op(BinOp),
}

pub struct Arg {
    pub expr: ExprId,
    /// Passed as `&x` (to an `inout` parameter).
    pub by_ref: Option<Span>,
}

pub struct ObjField {
    pub name: Sym,
    pub name_span: Span,
    pub value: ExprId,
    /// Written as a string literal (`{ "Content-Type": v }`): a key for a `Record<string, V>`.
    pub quoted: bool,
}

pub enum ArrowBody {
    Expr(ExprId),
    Block(StmtId),
}

pub struct Param {
    pub name: Sym,
    pub span: Span,
    pub ty: Option<TypeId>,
    pub inout: bool,
    pub optional: bool,
    /// Constructor parameter property (`constructor(private readonly x: T)`): visibility, readonly.
    pub prop: Option<(Visibility, bool)>,
    /// `...xs: T[]`: collects the remaining arguments (last parameter of a function).
    pub rest: bool,
}

pub struct ArrowFn {
    pub params: Vec<Param>,
    pub ret: Option<TypeId>,
    /// `(): T throws E => ...`: declared with the return type.
    pub throws: Option<TypeId>,
    pub body: ArrowBody,
    /// `async (x) => ...` / `async name() { }`.
    pub is_async: bool,
}

pub enum ExprKind {
    Int(u64),
    Float(f64),
    /// `123n`: the digits as written (decimal, or `0x`/`0o`/`0b` and digits), without `_` or `n`.
    BigInt(Sym),
    /// `19.99m`: the text as written, without `_` or `m`.
    Decimal(Sym),
    /// Interned at parse time, so checking never needs to mutate the interner.
    Str(Sym),
    Template(Vec<String>, Vec<ExprId>),
    Bool(bool),
    Undefined,
    Null,
    Ident(Sym),
    Unary(UnOp, ExprId),
    Binary(BinOp, ExprId, ExprId),
    Assign(AssignOp, ExprId, ExprId),
    Update { inc: bool, prefix: bool, target: ExprId },
    Call { callee: ExprId, type_args: Vec<TypeId>, args: Vec<Arg>, optional: bool },
    New { callee: ExprId, type_args: Vec<TypeId>, args: Vec<Arg> },
    Member { obj: ExprId, name: Sym, name_span: Span, optional: bool },
    Index { obj: ExprId, index: ExprId, optional: bool },
    Object(Vec<ObjField>),
    Array(Vec<ExprId>),
    Arrow(Box<ArrowFn>),
    Cond(ExprId, ExprId, ExprId),
    As(ExprId, TypeId),
    NonNull(ExprId),
    Typeof(ExprId),
    Paren(ExprId),
    /// `this` inside a class body.
    This,
    /// `super` (only as `super(...)` in a constructor or `super.method(...)`).
    Super,
    /// `try f(x)`: passes an error thrown by the call(s) on to the caller.
    Try(ExprId),
    /// `await e`.
    Await(ExprId),
    Error,
}

pub struct Stmt {
    pub kind: StmtKind,
    pub span: Span,
}

pub struct Case {
    pub test: Option<ExprId>,
    pub body: Vec<StmtId>,
    pub span: Span,
}

pub enum StmtKind {
    Let { mutable: bool, name: Sym, name_span: Span, ty: Option<TypeId>, init: Option<ExprId> },
    Expr(ExprId),
    If(ExprId, StmtId, Option<StmtId>),
    While(ExprId, StmtId),
    DoWhile(StmtId, ExprId),
    For { init: Option<StmtId>, cond: Option<ExprId>, step: Option<ExprId>, body: StmtId },
    ForOf { mutable: bool, name: Sym, name_span: Span, iter: ExprId, body: StmtId },
    Switch(ExprId, Vec<Case>),
    Return(Option<ExprId>),
    Break,
    Continue,
    Block(Vec<StmtId>),
    Throw(ExprId),
    Try { body: StmtId, catch: Option<Catch>, finally: Option<StmtId> },
    /// In a script's body: initialize module variable (item index) here, in source order.
    InitGlobal(u32),
    Empty,
    Error,
}

pub struct Catch {
    /// `catch (e)` / `catch (e: T)`; `None` for `catch { ... }`.
    pub param: Option<(Sym, Span, Option<TypeId>)>,
    pub body: StmtId,
}

pub struct TypeExpr {
    pub kind: TypeExprKind,
    pub span: Span,
}

pub struct FieldTy {
    pub name: Sym,
    pub name_span: Span,
    pub optional: bool,
    pub ty: TypeId,
}

pub enum TypeExprKind {
    Named { ns: Option<(Sym, Span)>, name: Sym, name_span: Span, args: Vec<TypeId> },
    Array(TypeId),
    Record(Vec<FieldTy>),
    Union(Vec<TypeId>),
    StrLit(Sym),
    /// `(params) => ret`, with an optional `throws E`.
    Func(Vec<Param>, TypeId, Option<TypeId>),
    Undefined,
    Null,
    Void,
    Error,
}

pub struct TypeParam {
    pub name: Sym,
    pub span: Span,
    pub bound: Option<TypeId>,
    /// `<T = undefined>`: the type when nothing else fixes it.
    pub default: Option<TypeId>,
}

pub struct FnDecl {
    pub name: Sym,
    pub name_span: Span,
    pub tparams: Vec<TypeParam>,
    pub params: Vec<Param>,
    pub ret: Option<TypeId>,
    /// `throws E`.
    pub throws: Option<TypeId>,
    pub body: StmtId,
    /// `async function` / `async method()`; a script whose top level uses `await`.
    pub is_async: bool,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Visibility {
    Public,
    Protected,
    Private,
}

pub struct ClassDecl {
    pub name: Sym,
    pub name_span: Span,
    pub tparams: Vec<TypeParam>,
    pub is_abstract: bool,
    /// `cyclic class`: instances may form reference cycles (collected by the cycle collector).
    pub cyclic: bool,
    pub extends: Option<TypeId>,
    pub implements: Vec<TypeId>,
    pub members: Vec<ClassMember>,
}

pub struct ClassMember {
    pub name: Sym,
    pub name_span: Span,
    pub span: Span,
    pub vis: Visibility,
    pub is_static: bool,
    pub readonly: bool,
    /// `weak parent: Node | undefined`: doesn't keep the target alive.
    pub weak: bool,
    pub is_abstract: bool,
    pub kind: MemberKind,
}

pub enum MemberKind {
    Field { ty: Option<TypeId>, init: Option<ExprId>, optional: bool },
    /// Abstract methods have an empty body block.
    Method(FnDecl),
    Getter(FnDecl),
    Constructor(FnDecl),
}

pub struct Import {
    pub names: Vec<(Sym, Span)>,
    pub namespace: Option<(Sym, Span)>,
    /// `import x from "pkg"`: only npm packages have a default export.
    pub default: Option<(Sym, Span)>,
    pub path: String,
    pub path_span: Span,
}

pub enum ItemKind {
    Import(Import),
    Function(FnDecl),
    TypeAlias { name: Sym, name_span: Span, tparams: Vec<TypeParam>, ty: TypeId },
    Interface { name: Sym, name_span: Span, tparams: Vec<TypeParam>, members: Vec<FieldTy> },
    /// A module-level `const` (or `let`, with `mutable`).
    Const { name: Sym, name_span: Span, ty: Option<TypeId>, init: ExprId, mutable: bool },
    Test { name: String, name_span: Span, body: ExprId },
    Class(ClassDecl),
}

pub struct Item {
    pub kind: ItemKind,
    pub span: Span,
    pub exported: bool,
}

impl Ast {
    /// Rewrites every symbol (used to merge ASTs parsed with per-thread interners).
    pub fn remap_syms(&mut self, f: &impl Fn(Sym) -> Sym) {
        let params = |ps: &mut Vec<Param>| ps.iter_mut().for_each(|p| p.name = f(p.name));
        let tparams = |ps: &mut Vec<TypeParam>| ps.iter_mut().for_each(|p| p.name = f(p.name));
        let fields = |fs: &mut Vec<FieldTy>| fs.iter_mut().for_each(|x| x.name = f(x.name));
        for e in &mut self.exprs {
            match &mut e.kind {
                ExprKind::Ident(s) | ExprKind::Str(s) | ExprKind::BigInt(s) | ExprKind::Decimal(s) => *s = f(*s),
                ExprKind::Member { name, .. } => *name = f(*name),
                ExprKind::Object(fs) => fs.iter_mut().for_each(|x| x.name = f(x.name)),
                ExprKind::Arrow(a) => params(&mut a.params),
                _ => {}
            }
        }
        for s in &mut self.stmts {
            match &mut s.kind {
                StmtKind::Let { name, .. } | StmtKind::ForOf { name, .. } => *name = f(*name),
                StmtKind::Try { catch: Some(Catch { param: Some((name, _, _)), .. }), .. } => *name = f(*name),
                _ => {}
            }
        }
        for t in &mut self.types {
            match &mut t.kind {
                TypeExprKind::Named { ns, name, .. } => {
                    *name = f(*name);
                    if let Some((n, _)) = ns {
                        *n = f(*n);
                    }
                }
                TypeExprKind::Record(fs) => fields(fs),
                TypeExprKind::StrLit(s) => *s = f(*s),
                TypeExprKind::Func(ps, _, _) => params(ps),
                _ => {}
            }
        }
        for item in &mut self.items {
            match &mut item.kind {
                ItemKind::Import(imp) => {
                    imp.names.iter_mut().for_each(|(n, _)| *n = f(*n));
                    if let Some((n, _)) = &mut imp.namespace {
                        *n = f(*n);
                    }
                }
                ItemKind::Function(fd) => {
                    fd.name = f(fd.name);
                    tparams(&mut fd.tparams);
                    params(&mut fd.params);
                }
                ItemKind::TypeAlias { name, tparams: tps, .. } => {
                    *name = f(*name);
                    tparams(tps);
                }
                ItemKind::Interface { name, tparams: tps, members, .. } => {
                    *name = f(*name);
                    tparams(tps);
                    fields(members);
                }
                ItemKind::Const { name, .. } => *name = f(*name),
                ItemKind::Test { .. } => {}
                ItemKind::Class(c) => {
                    c.name = f(c.name);
                    tparams(&mut c.tparams);
                    for mem in &mut c.members {
                        mem.name = f(mem.name);
                        match &mut mem.kind {
                            MemberKind::Method(fd) | MemberKind::Getter(fd) | MemberKind::Constructor(fd) => {
                                fd.name = f(fd.name);
                                tparams(&mut fd.tparams);
                                params(&mut fd.params);
                            }
                            MemberKind::Field { .. } => {}
                        }
                    }
                }
            }
        }
    }
}
