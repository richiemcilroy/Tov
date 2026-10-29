//! Tier 2: checked program → one C translation unit (runtime included), compiled by the system C compiler.
//!
//! Representation
//! - `int` → `tv_int`, `f64` → `double`, sized ints → `<stdint.h>` types, `bool` → `bool`.
//! - String-literal types, `undefined`, `void` and `never` are unit values (`tv_unit`).
//! - `string` → `tv_str`, arrays → `tv_arr`, maps/sets → `tv_map`, functions → `tv_fn` (closures).
//! - Records → C structs (fields sorted by symbol). Unions → `{ uint32_t tag; union { ... } u; }`
//!   with tags in member order, so `switch (s.kind)` becomes a switch on the tag.
//! - Recursive aliases → a pointer to a reference-counted box holding the unfolded value.
//!
//! Ownership: expression results are either *owned* temporaries (released at the end of the
//! statement unless moved somewhere) or *borrowed* views of existing places. Parameters are
//! borrowed; return values are owned; storing a borrowed value retains it.

mod asyncfn;
mod genfn;
pub(crate) mod body;
mod calls;
mod clone;
mod anyval;
mod dynval;
mod frame;
pub(crate) mod class;
mod iface;
mod js;
mod jsloop;
mod json;
mod moves;
mod writes;
mod quiet;
mod range;
mod resize;
mod rejit;

use crate::ast::{self, ExprId, ExprKind, ItemKind, StmtKind};
use crate::check::{Callee, Checker, IdentFact, ModuleFacts};
use crate::diag::Diagnostic;
use crate::hash::{FxMap, FxSet};
use crate::intern::Sym;
use crate::source::Span;
use crate::types::*;
use std::fmt::Write;

pub const RUNTIME_H: &str = include_str!("../../../../runtime/tov.h");
pub const RUNTIME_C: &str = include_str!("../../../../runtime/tov.c");
/// `bigint` (runtime/big.c): part of the runtime's translation unit, after tov.c.
pub const RUNTIME_BIG: &str = include_str!("../../../../runtime/big.c");
/// `decimal` (runtime/dec.c): after big.c, whose limb arithmetic it uses.
pub const RUNTIME_DEC: &str = include_str!("../../../../runtime/dec.c");
/// Sockets (runtime/net.c): after tov.c, whose event loop, DNS and TLS hooks it uses.
pub const RUNTIME_NET: &str = include_str!("../../../../runtime/net.c");
/// WebSockets (runtime/ws.c): after net.c, whose sockets they frame.
pub const RUNTIME_WS: &str = include_str!("../../../../runtime/ws.c");
/// Regular expressions (runtime/regexp.c, QuickJS's libregexp, and PCRE2 for its JIT), compiled for
/// programs that use them. (PCRE2's files under `pcre2/`.)
pub const REGEXP_FILES: &[(&str, &str)] = &[
    ("regexp.c", include_str!("../../../../runtime/regexp.c")),
    ("libregexp.c", include_str!("../../../../vendor/quickjs-regexp/libregexp.c")),
    ("libunicode.c", include_str!("../../../../vendor/quickjs-regexp/libunicode.c")),
    ("cutils.c", include_str!("../../../../vendor/quickjs-regexp/cutils.c")),
    ("libregexp.h", include_str!("../../../../vendor/quickjs-regexp/libregexp.h")),
    ("libregexp-opcode.h", include_str!("../../../../vendor/quickjs-regexp/libregexp-opcode.h")),
    ("libregexp-exec.h", include_str!("../../../../vendor/quickjs-regexp/libregexp-exec.h")),
    ("libunicode.h", include_str!("../../../../vendor/quickjs-regexp/libunicode.h")),
    ("libunicode-table.h", include_str!("../../../../vendor/quickjs-regexp/libunicode-table.h")),
    ("cutils.h", include_str!("../../../../vendor/quickjs-regexp/cutils.h")),
    ("list.h", include_str!("../../../../vendor/quickjs-regexp/list.h")),
    ("dtoa.h", include_str!("../../../../vendor/quickjs-regexp/dtoa.h")),
    ("pcre2/src/config.h", include_str!("../../../../vendor/pcre2/src/config.h")),
    ("pcre2/src/pcre2.h", include_str!("../../../../vendor/pcre2/src/pcre2.h")),
    ("pcre2/src/pcre2_auto_possess.c", include_str!("../../../../vendor/pcre2/src/pcre2_auto_possess.c")),
    ("pcre2/src/pcre2_chartables.c", include_str!("../../../../vendor/pcre2/src/pcre2_chartables.c")),
    ("pcre2/src/pcre2_chkdint.c", include_str!("../../../../vendor/pcre2/src/pcre2_chkdint.c")),
    ("pcre2/src/pcre2_compile.c", include_str!("../../../../vendor/pcre2/src/pcre2_compile.c")),
    ("pcre2/src/pcre2_compile.h", include_str!("../../../../vendor/pcre2/src/pcre2_compile.h")),
    ("pcre2/src/pcre2_compile_cgroup.c", include_str!("../../../../vendor/pcre2/src/pcre2_compile_cgroup.c")),
    ("pcre2/src/pcre2_compile_class.c", include_str!("../../../../vendor/pcre2/src/pcre2_compile_class.c")),
    ("pcre2/src/pcre2_config.c", include_str!("../../../../vendor/pcre2/src/pcre2_config.c")),
    ("pcre2/src/pcre2_context.c", include_str!("../../../../vendor/pcre2/src/pcre2_context.c")),
    ("pcre2/src/pcre2_error.c", include_str!("../../../../vendor/pcre2/src/pcre2_error.c")),
    ("pcre2/src/pcre2_extuni.c", include_str!("../../../../vendor/pcre2/src/pcre2_extuni.c")),
    ("pcre2/src/pcre2_find_bracket.c", include_str!("../../../../vendor/pcre2/src/pcre2_find_bracket.c")),
    ("pcre2/src/pcre2_internal.h", include_str!("../../../../vendor/pcre2/src/pcre2_internal.h")),
    ("pcre2/src/pcre2_intmodedep.h", include_str!("../../../../vendor/pcre2/src/pcre2_intmodedep.h")),
    ("pcre2/src/pcre2_jit_char_inc.h", include_str!("../../../../vendor/pcre2/src/pcre2_jit_char_inc.h")),
    ("pcre2/src/pcre2_jit_compile.c", include_str!("../../../../vendor/pcre2/src/pcre2_jit_compile.c")),
    ("pcre2/src/pcre2_jit_match_inc.h", include_str!("../../../../vendor/pcre2/src/pcre2_jit_match_inc.h")),
    ("pcre2/src/pcre2_jit_misc_inc.h", include_str!("../../../../vendor/pcre2/src/pcre2_jit_misc_inc.h")),
    ("pcre2/src/pcre2_jit_simd_inc.h", include_str!("../../../../vendor/pcre2/src/pcre2_jit_simd_inc.h")),
    ("pcre2/src/pcre2_maketables.c", include_str!("../../../../vendor/pcre2/src/pcre2_maketables.c")),
    ("pcre2/src/pcre2_match.c", include_str!("../../../../vendor/pcre2/src/pcre2_match.c")),
    ("pcre2/src/pcre2_match_data.c", include_str!("../../../../vendor/pcre2/src/pcre2_match_data.c")),
    ("pcre2/src/pcre2_newline.c", include_str!("../../../../vendor/pcre2/src/pcre2_newline.c")),
    ("pcre2/src/pcre2_ord2utf.c", include_str!("../../../../vendor/pcre2/src/pcre2_ord2utf.c")),
    ("pcre2/src/pcre2_pattern_info.c", include_str!("../../../../vendor/pcre2/src/pcre2_pattern_info.c")),
    ("pcre2/src/pcre2_printint_inc.h", include_str!("../../../../vendor/pcre2/src/pcre2_printint_inc.h")),
    ("pcre2/src/pcre2_script_run.c", include_str!("../../../../vendor/pcre2/src/pcre2_script_run.c")),
    ("pcre2/src/pcre2_string_utils.c", include_str!("../../../../vendor/pcre2/src/pcre2_string_utils.c")),
    ("pcre2/src/pcre2_study.c", include_str!("../../../../vendor/pcre2/src/pcre2_study.c")),
    ("pcre2/src/pcre2_tables.c", include_str!("../../../../vendor/pcre2/src/pcre2_tables.c")),
    ("pcre2/src/pcre2_ucd.c", include_str!("../../../../vendor/pcre2/src/pcre2_ucd.c")),
    ("pcre2/src/pcre2_ucp.h", include_str!("../../../../vendor/pcre2/src/pcre2_ucp.h")),
    ("pcre2/src/pcre2_ucptables_inc.h", include_str!("../../../../vendor/pcre2/src/pcre2_ucptables_inc.h")),
    ("pcre2/src/pcre2_util.h", include_str!("../../../../vendor/pcre2/src/pcre2_util.h")),
    ("pcre2/src/pcre2_valid_utf.c", include_str!("../../../../vendor/pcre2/src/pcre2_valid_utf.c")),
    ("pcre2/src/pcre2_xclass.c", include_str!("../../../../vendor/pcre2/src/pcre2_xclass.c")),
    ("pcre2/sljit/sljitConfig.h", include_str!("../../../../vendor/pcre2/sljit/sljitConfig.h")),
    ("pcre2/sljit/sljitConfigCPU.h", include_str!("../../../../vendor/pcre2/sljit/sljitConfigCPU.h")),
    ("pcre2/sljit/sljitConfigInternal.h", include_str!("../../../../vendor/pcre2/sljit/sljitConfigInternal.h")),
    ("pcre2/sljit/sljitLir.c", include_str!("../../../../vendor/pcre2/sljit/sljitLir.c")),
    ("pcre2/sljit/sljitLir.h", include_str!("../../../../vendor/pcre2/sljit/sljitLir.h")),
    ("pcre2/sljit/sljitNativeARM_64.c", include_str!("../../../../vendor/pcre2/sljit/sljitNativeARM_64.c")),
    ("pcre2/sljit/sljitNativeX86_64.c", include_str!("../../../../vendor/pcre2/sljit/sljitNativeX86_64.c")),
    ("pcre2/sljit/sljitNativeX86_common.c", include_str!("../../../../vendor/pcre2/sljit/sljitNativeX86_common.c")),
    ("pcre2/sljit/sljitSerialize.c", include_str!("../../../../vendor/pcre2/sljit/sljitSerialize.c")),
    ("pcre2/sljit/sljitUtils.c", include_str!("../../../../vendor/pcre2/sljit/sljitUtils.c")),
    ("pcre2/sljit/allocator_src/sljitExecAllocatorApple.c", include_str!("../../../../vendor/pcre2/sljit/allocator_src/sljitExecAllocatorApple.c")),
    ("pcre2/sljit/allocator_src/sljitExecAllocatorCore.c", include_str!("../../../../vendor/pcre2/sljit/allocator_src/sljitExecAllocatorCore.c")),
    ("pcre2/sljit/allocator_src/sljitExecAllocatorFreeBSD.c", include_str!("../../../../vendor/pcre2/sljit/allocator_src/sljitExecAllocatorFreeBSD.c")),
    ("pcre2/sljit/allocator_src/sljitExecAllocatorPosix.c", include_str!("../../../../vendor/pcre2/sljit/allocator_src/sljitExecAllocatorPosix.c")),
    ("pcre2/sljit/allocator_src/sljitExecAllocatorWindows.c", include_str!("../../../../vendor/pcre2/sljit/allocator_src/sljitExecAllocatorWindows.c")),
    ("pcre2/sljit/allocator_src/sljitProtExecAllocatorNetBSD.c", include_str!("../../../../vendor/pcre2/sljit/allocator_src/sljitProtExecAllocatorNetBSD.c")),
    ("pcre2/sljit/allocator_src/sljitProtExecAllocatorPosix.c", include_str!("../../../../vendor/pcre2/sljit/allocator_src/sljitProtExecAllocatorPosix.c")),
    ("pcre2/sljit/allocator_src/sljitWXExecAllocatorPosix.c", include_str!("../../../../vendor/pcre2/sljit/allocator_src/sljitWXExecAllocatorPosix.c")),
    ("pcre2/sljit/allocator_src/sljitWXExecAllocatorWindows.c", include_str!("../../../../vendor/pcre2/sljit/allocator_src/sljitWXExecAllocatorWindows.c")),
];
/// The JavaScript bridge's interface (runtime/js.h): in every program's fixed prefix, so the
/// prelude's `JsError` can hold a `Js`; only npm programs link its implementation.
pub const JS_H: &str = include_str!("../../../../runtime/js.h");

const ASYNC_RUN: &str = "    tv_async_run();\n";
const C_PREFIX: &str = "/* Generated by tov. */\n#define _GNU_SOURCE 1\n";

/// The runtime's body without its `#include "tov.h"` (the header is pasted in instead).
fn runtime_body(out: &mut String) {
    for line in RUNTIME_C.lines().chain(RUNTIME_BIG.lines()).chain(RUNTIME_DEC.lines()).chain(RUNTIME_NET.lines()).chain(RUNTIME_WS.lines()) {
        if line.trim_start().starts_with("#include \"tov.h\"") {
            continue;
        }
        out.push_str(line);
        out.push('\n');
    }
}

/// The runtime as one C translation unit, compiled once per compiler and flags and cached.
pub fn runtime_source() -> String {
    let mut out = String::with_capacity(RUNTIME_H.len() + RUNTIME_C.len() + RUNTIME_BIG.len() + RUNTIME_DEC.len() + RUNTIME_NET.len() + RUNTIME_WS.len() + 64);
    out.push_str(C_PREFIX);
    out.push_str(RUNTIME_H);
    out.push('\n');
    runtime_body(&mut out);
    out
}

/// The start every generated program shares (runtime interface, includes, prelude): the build
/// precompiles it once per compiler and flags, so a program's compile parses only its own code.
pub fn fixed_prefix() -> String {
    crate::rt_flags::fixed_prefix(RUNTIME_H, JS_H, PRELUDE)
}

/// A generated program plus the runtime in one file (for `--emit-c`: compiles on its own).
pub fn standalone(program: &str) -> String {
    let head = C_PREFIX.len() + RUNTIME_H.len() + 1;
    let mut out = String::with_capacity(program.len() + RUNTIME_C.len());
    out.push_str(&program[..head]);
    runtime_body(&mut out);
    out.push_str(&program[head..]);
    out
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// A program: call `main()`.
    Run,
    /// Run every `test(...)`.
    Test,
}

/// A generated C program.
pub struct Program {
    pub c: String,
    /// It calls fetch(), so it links TLS (see build.rs).
    pub uses_tls: bool,
    /// It uses regular expressions, so it links libregexp (see build.rs).
    pub uses_regexp: bool,
    /// The npm packages it imports (to bundle; it links runtime/js.c and JavaScriptCore).
    pub npm: Vec<String>,
}

/// Generates the C program, or diagnostics for constructs the backend doesn't support yet.
pub fn generate(c: &mut Checker, mode: Mode) -> Result<Program, Vec<Diagnostic>> {
    let mut g = Gen::new(c);
    g.flatten_recs();
    g.program(mode);
    g.dyn_dispatch();
    g.devirt_helpers();
    for msg in std::mem::take(&mut g.internal) {
        let file = g.c.modules[0].file;
        g.errors.push(Diagnostic::new("C0003", Span::new(file, 0, 0), format!("internal compiler error: {msg}")).note("note", "this is a bug in tov; please report it with the program"));
    }
    if !g.errors.is_empty() {
        g.errors.sort_by_key(|d| (d.span.file, d.span.start));
        g.errors.dedup_by(|a, b| a.span == b.span && a.message == b.message);
        return Err(g.errors);
    }
    let c = g.assemble();
    let mut npm: Vec<String> = if g.js.used { g.c.modules.iter().filter_map(|m| m.npm.clone()).collect() } else { Vec::new() };
    // console.log of a JavaScript value formats it with Node.js's util.inspect
    if g.js.used && g.uses_inspect && !npm.iter().any(|s| s == "node:util") {
        npm.push("node:util".into());
    }
    Ok(Program { c, uses_tls: g.uses_tls, uses_regexp: g.uses_regexp, npm })
}

/// A value produced by an expression.
#[derive(Clone, Debug)]
pub(crate) struct Val {
    pub code: String,
    pub ty: TyId,
    /// An owned temporary (registered for release at the end of the statement).
    pub owned: bool,
}

impl Val {
    fn plain(code: impl Into<String>, ty: TyId) -> Val {
        Val { code: code.into(), ty, owned: false }
    }
}

/// (module, item, type arguments) of a function instance.
/// (module, item, type arguments, parameters fixed to an arrow's C function)
type InstanceKey = (u32, u32, Vec<(u32, TyId)>, Vec<(usize, String)>);

/// A generic function instance to emit.
struct Instance {
    m: u32,
    item: u32,
    subst: FxMap<u32, TyId>,
    cname: String,
    /// Parameters fixed to an arrow's C function (see `instance_with`).
    direct: Vec<(usize, String)>,
}

/// A generic instantiation's type arguments (parameter → type), sorted.
pub(crate) type TypeArgs = Vec<(u32, TyId)>;

pub(crate) struct Gen<'c, 'a> {
    /// The `+` or `-` being compiled has a product among its operands (`h * 31 + c`): in
    /// TypeScript it's rounded past 2^53 too (see TV_JADD; sums of other values stay exact).
    pub(crate) js_mul_operand: bool,
    /// The span of an integer expression whose value only becomes a float (see
    /// `exact_half_sum`): its multiplication needs no JavaScript rounding.
    pub(crate) float_only: Option<crate::source::Span>,
    pub(crate) c: &'c mut Checker<'a>,
    /// JavaScript values (npm packages).
    pub(crate) js: js::JsGen,
    /// Section buffers, assembled in this order.
    typedefs: String,
    structs: String,
    protos: String,
    lits: String,
    helpers: String,
    funcs: String,
    /// Class struct bodies (after records and unions: those only hold class pointers).
    pub(crate) class_structs: String,
    ctypes: FxMap<TyId, String>,
    defined: FxSet<TyId>,
    helpers_done: FxSet<(TyId, u8)>,
    descs: FxSet<TyId>,
    lit_names: FxMap<Vec<u8>, String>,
    loc_names: FxMap<String, String>,
    instances: FxMap<InstanceKey, String>,
    queue: Vec<Instance>,
    fn_values: FxMap<String, String>,
    static_insts: FxMap<(u32, u32, TypeArgs), String>,
    /// Closure environment structs (after all value structs they may contain).
    pub(crate) env_structs: String,
    consts_done: FxSet<(u32, u32)>,
    /// Module variables' (and class statics') initializations: by when JavaScript runs them
    /// (the module's place in the evaluation order, the item, the member), see `init_rank`.
    const_init: Vec<((u32, u32, u32), String)>,
    /// Each module's place in JavaScript's evaluation order (imports first, depth first).
    module_rank: Vec<u32>,
    /// Running a script: its module variables are initialized by the script, in source order.
    script_module: Option<u32>,
    /// Module variables some module changes (union of the facts).
    pub(crate) mutated_globals: FxSet<(u32, u32)>,
    counter: u32,
    pub(crate) errors: Vec<Diagnostic>,
    /// Memo for `inout_leak_free`: (module, item, parameter) → leak-free.
    pub(crate) leak_free: FxMap<(u32, u32, usize), bool>,
    /// Helpers that call generated functions (emitted after them).
    pub(crate) helpers_after: Vec<String>,
    /// Backend bugs found while generating (reported instead of emitting wrong code).
    pub(crate) internal: Vec<String>,
    /// Stack of function bodies being emitted (arrows nest).
    pub(crate) bodies: Vec<body::Body>,
    // Classes (see `class.rs`).
    pub(crate) class_insts: FxMap<TyId, class::ClassInst>,
    pub(crate) class_list: Vec<TyId>,
    pub(crate) ctor_insts: FxMap<TyId, String>,
    pub(crate) method_insts: FxMap<(TyId, u32), String>,
    pub(crate) generic_method_insts: FxMap<(TyId, u32, TypeArgs), String>,
    pub(crate) dispatch_names: FxMap<(TyId, Sym), String>,
    pub(crate) dispatchers: Vec<class::Dispatcher>,
    pub(crate) dispatch_bodies: String,
    pub(crate) overridden: FxMap<(u32, Sym), bool>,
    pub(crate) isa_decls: FxSet<u32>,
    /// `x.constructor.name` read a class id: `tvg_cname` is emitted.
    pub(crate) class_names_used: bool,
    pub(crate) class_work: Vec<class::ClassWork>,
    pub(crate) statics_done: FxSet<(u32, u32)>,
    pub(crate) ctor_move_memo: FxMap<u32, Vec<bool>>,
    pub(crate) weak_memo: FxMap<u32, bool>,
    pub(crate) weak_slot_memo: FxMap<u32, bool>,
    /// Whether the program formats a value through descriptors (console.log of a container, ...).
    pub(crate) uses_inspect: bool,
    /// The types put into `unknown`s, and whether code asks one what it holds (see dynval.rs).
    pub(crate) dyn_types: Vec<TyId>,
    pub(crate) dyn_tables: bool,
    /// Code works on an `any`, and the helpers converting one to a type (see anyval.rs).
    pub(crate) any_tables: bool,
    pub(crate) any_convs: FxMap<TyId, String>,
    /// The `any` switches as last made, and how many boxed types they cover.
    pub(crate) any_code: String,
    pub(crate) any_covered: Option<usize>,
    /// The statement being made (where a conversion from an `any` traps).
    pub(crate) stmt_span: Option<Span>,
    /// Types some code retains (copies with a count increment), and the recursive-alias boxes
    /// defined: a box type no retained value can reach is never shared, so it has no count.
    pub(crate) rc_roots: FxSet<TyId>,
    pub(crate) rc_helper_depth: u32,
    pub(crate) rec_boxes: Vec<(TyId, String)>,
    box_defs: String,
    /// Classes whose objects' class id some code reads (`instanceof`).
    pub(crate) cid_decls: FxSet<u32>,
    /// Helper functions whose prototypes are in `protos`, emitted with the helpers.
    pub(crate) helpers_after_decl: String,
    /// Box types whose struct bodies are still to be defined.
    pub(crate) pending_boxes: Vec<TyId>,
    /// (source type, interface type) → itab name.
    pub(crate) itabs: FxMap<(TyId, TyId), String>,
    /// (source function type, target function type) → adapter function (see `fn_adapter`).
    pub(crate) fn_adapters: FxMap<(TyId, TyId), String>,
    /// Conversions under way (see `coerce`), and the functions made for those met again inside
    /// themselves (between recursive types).
    pub(crate) coerce_stack: Vec<(TyId, TyId)>,
    /// Interface values being unwrapped into unions (see `coerce_inner`): (interface, union).
    pub(crate) unwrapping: Vec<(TyId, TyId)>,
    /// The type of the assignment expression being generated (see `assign`).
    pub(crate) assign_ty: Option<TyId>,
    /// Recursive aliases for arrays, held as the array (see `flatten_recs`).
    pub(crate) rec_flat: FxMap<TyId, TyId>,
    /// A script's static fields whose initializers can throw.
    pub(crate) static_throws: FxSet<(u32, u32)>,
    /// Bodies found quiet or not (see `quiet`), those being looked at, whether a result relied on
    /// one of those, and the calls being made (their arguments' address, quiet).
    pub(crate) quiet_memo: FxMap<(u32, crate::ast::StmtId, bool), bool>,
    pub(crate) quiet_stack: Vec<(u32, crate::ast::StmtId, bool)>,
    pub(crate) quiet_tainted: bool,
    pub(crate) quiet_calls: Vec<(usize, bool)>,
    /// Callback parameters found only called (see `arg_called_only`), and those being looked at.
    pub(crate) called_memo: FxMap<(u32, crate::ast::StmtId, u32), bool>,
    /// Where parameters get their values (see `param_quiet`), and its findings.
    pub(crate) bind_index: Option<std::rc::Rc<quiet::BindIndex>>,
    pub(crate) pquiet_memo: FxMap<(u32, crate::ast::StmtId, u32), bool>,
    /// Parameters bodies take ownership of (see `owned_params`), and the calls being made
    /// (their arguments' address, which arguments are handed over).
    pub(crate) owned_memo: FxMap<(u32, crate::ast::StmtId), Vec<bool>>,
    pub(crate) owned_calls: Vec<(usize, Vec<bool>)>,
    /// What bodies may release (see `writes`), those being looked at, and what types reach.
    pub(crate) writes_memo: FxMap<(u32, crate::ast::StmtId, bool), writes::Writes>,
    pub(crate) writes_stack: Vec<(u32, crate::ast::StmtId, bool)>,
    pub(crate) reach_memo: FxMap<TyId, Option<FxSet<u32>>>,
    /// The closures a function-typed class field can hold (see `field_closures`), and the
    /// arrows whose bodies are being looked at through such a field.
    pub(crate) field_cl_memo: FxMap<crate::intern::Sym, Option<Vec<(u32, crate::ast::ExprId)>>>,
    pub(crate) field_arrows: Vec<(u32, crate::ast::ExprId)>,
    /// A finding looked at a type parameter's value in the body being written out (see
    /// `root_inst_ty`): it holds for that instance only, memoized by its type arguments.
    pub(crate) subst_used: bool,
    /// Each arrow's C functions (one an instance of the body it's in) and their pointer types;
    /// the calls through class fields that test for them (see `devirt_helpers`): the helper's
    /// name, the arrows, its pointer type, result and parameter C types.
    pub(crate) arrow_fns: FxMap<(u32, crate::ast::ExprId), Vec<(String, String)>>,
    /// Each module's `const f = (...) => ...` locals: by key, the arrow (see `const_arrow`).
    pub(crate) const_arrows: FxMap<u32, FxMap<u32, crate::ast::ExprId>>,
    pub(crate) devirt: Vec<(String, Vec<(u32, crate::ast::ExprId)>, String, String, Vec<String>)>,
    pub(crate) arrow_protos: FxMap<String, String>,
    /// Array types given a free function of their own (see `arr_free_fn`).
    pub(crate) arr_frees: FxSet<TyId>,
    /// Module constants found only read as `E.field` (see `const_member`).
    pub(crate) ro_consts: FxMap<IdentFact, bool>,
    /// Field places of objects that can't change before the store (see `class_field_place`).
    pub(crate) stable_places: FxSet<String>,
    pub(crate) reassigned_memo: FxMap<(u32, crate::ast::StmtId), FxSet<u32>>,
    pub(crate) quiet_inst_memo: FxMap<((u32, crate::ast::StmtId, bool), Vec<(u32, TyId)>), bool>,
    /// The call being made, for its arguments (see `call_spares`).
    pub(crate) cur_calls: Vec<(usize, u32, crate::ast::ExprId)>,
    pub(crate) pquiet_stack: Vec<(u32, crate::ast::StmtId, u32)>,
    pub(crate) called_stack: Vec<(u32, crate::ast::StmtId, u32)>,
    pub(crate) rec_cvs: FxMap<(TyId, TyId), String>,
    /// Emit `tvg_uncaught` (main can throw, or tests).
    pub(crate) needs_uncaught: bool,
    /// `tvg_test_fn` (running a test closure, see `__test`) is emitted.
    pub(crate) test_call_done: bool,
    /// The async function whose body `function_body` emits next (see `asyncfn.rs`).
    pub(crate) pending_async: Option<(u32, u32)>,
    /// The next body is a generator's, yielding these.
    pub(crate) pending_gen: Option<TyId>,
    /// The next union projection checks the value is one of its members (see `asserted_reads`).
    pub(crate) checked_project: Option<String>,
    /// The local an arrow being emitted initializes (its key), when the arrow refers to it: the
    /// closure itself, not a capture.
    pub(crate) self_closure: Option<u32>,
    /// What `int` variables can hold (see `range`), worked out when first asked.
    pub(crate) ranges: Option<range::Ranges>,
    /// Module functions that may resize an array they didn't make (see `resize`).
    pub(crate) resizers: FxMap<(u32, u32), bool>,
    /// Per module: `const` locals holding new arrays (see `resize`).
    pub(crate) fresh_arrays: FxMap<u32, FxSet<u32>>,
    /// Objects' record structs (see `is_obj`), and TOV_BOX_ALL=1 (every record an object, to test).
    rec_structs: FxMap<TyId, String>,
    /// `define_struct`s under way, and the classes met in their fields meanwhile (defined after).
    struct_depth: u32,
    pending_class_structs: Vec<TyId>,
    box_all: bool,
    /// The awaited calls embedded in the async body just emitted: (union member, frame type).
    pub(crate) last_children: Vec<(String, String)>,
    /// `next()` of async generators, by the result type: the helper's name.
    pub(crate) agen_helpers: FxMap<u32, String>,
    /// Frame structs of async functions.
    /// The accumulator loop for the function `function_body` emits next (see `tre_candidate`).
    pub(crate) pending_tre: Option<body::Tre>,
    /// The next body's `direct_fns`.
    pub(crate) pending_direct: Option<FxMap<u32, String>>,
    /// How many instances specialized for arrows each function has (they're capped).
    pub(crate) direct_counts: FxMap<(u32, u32), usize>,
    /// Each arrow's most recently made C function and its pointer type (stack closures too).
    pub(crate) arrow_last: FxMap<(u32, ExprId), (String, String)>,
    /// The array local the current statement stores an element of (`xs[i] = ...`).
    pub(crate) store_arr: Option<u32>,
    /// The span of the expression a `return` is evaluating (see `method_on`'s builders).
    pub(crate) return_span: Option<crate::source::Span>,
    /// While a self-recursive function's copy is emitted: its own calls go to this function
    /// instead ((module, item, type arguments) → the next copy; see `emit_function`).
    pub(crate) recur: Option<(u32, u32, Vec<(u32, TyId)>, String)>,
    /// Functions' parameters never assigned (by index), for `direct_args`.
    pub(crate) unassigned_params: FxMap<(u32, u32), Vec<bool>>,
    /// The program calls fetch(): main installs TLS (and the build links it).
    uses_tls: bool,
    pub(crate) uses_regexp: bool,
    pub(crate) frame_defs: Vec<asyncfn::FrameDef>,
}

// Helper kinds for `helpers_done`.
const H_RC: u8 = 0;
const H_EQ: u8 = 1;
const H_HASH: u8 = 2;
const H_STR: u8 = 3;
const H_INSPECT: u8 = 4;
const H_DEFAULT: u8 = 5;
const H_RESOLVE: u8 = 6;
const H_REJECT: u8 = 7;

impl<'c, 'a> Gen<'c, 'a> {
    fn new(c: &'c mut Checker<'a>) -> Self {
        Gen {
            c,
            typedefs: String::new(),
            structs: String::new(),
            protos: String::new(),
            lits: String::new(),
            helpers: String::new(),
            funcs: String::new(),
            class_structs: String::new(),
            ctypes: FxMap::default(),
            defined: FxSet::default(),
            helpers_done: FxSet::default(),
            descs: FxSet::default(),
            lit_names: FxMap::default(),
            loc_names: FxMap::default(),
            instances: FxMap::default(),
            queue: Vec::new(),
            fn_values: FxMap::default(),
            static_insts: FxMap::default(),
            env_structs: String::new(),
            consts_done: FxSet::default(),
            const_init: Vec::new(),
            module_rank: Vec::new(),
            script_module: None,
            mutated_globals: FxSet::default(),
            // (from 1000: a fresh `p12` or `c3` never shadows parameter `p12` or cell `c3`)
            counter: 1000,
            errors: Vec::new(),
            internal: Vec::new(),
            helpers_after: Vec::new(),
            leak_free: FxMap::default(),
            bodies: Vec::new(),
            class_insts: FxMap::default(),
            class_list: Vec::new(),
            ctor_insts: FxMap::default(),
            method_insts: FxMap::default(),
            generic_method_insts: FxMap::default(),
            dispatch_names: FxMap::default(),
            dispatchers: Vec::new(),
            dispatch_bodies: String::new(),
            overridden: FxMap::default(),
            isa_decls: FxSet::default(),
            class_names_used: false,
            class_work: Vec::new(),
            statics_done: FxSet::default(),
            ctor_move_memo: FxMap::default(),
            weak_memo: FxMap::default(),
            weak_slot_memo: FxMap::default(),
            uses_inspect: false,
            dyn_types: Vec::new(),
            dyn_tables: false,
            any_tables: false,
            any_convs: FxMap::default(),
            any_code: String::new(),
            any_covered: None,
            stmt_span: None,
            rc_roots: FxSet::default(),
            rc_helper_depth: 0,
            rec_boxes: Vec::new(),
            box_defs: String::new(),
            cid_decls: FxSet::default(),
            helpers_after_decl: String::new(),
            pending_boxes: Vec::new(),
            itabs: FxMap::default(),
            fn_adapters: FxMap::default(),
            coerce_stack: Vec::new(),
            unwrapping: Vec::new(),
            assign_ty: None,
            rec_flat: FxMap::default(),
            static_throws: FxSet::default(),
            quiet_memo: FxMap::default(),
            quiet_stack: Vec::new(),
            quiet_tainted: false,
            quiet_calls: Vec::new(),
            called_memo: FxMap::default(),
            bind_index: None,
            pquiet_memo: FxMap::default(),
            owned_memo: FxMap::default(),
            owned_calls: Vec::new(),
            writes_memo: FxMap::default(),
            writes_stack: Vec::new(),
            reach_memo: FxMap::default(),
            field_cl_memo: FxMap::default(),
            field_arrows: Vec::new(),
            subst_used: false,
            arrow_fns: FxMap::default(),
            const_arrows: FxMap::default(),
            devirt: Vec::new(),
            arrow_protos: FxMap::default(),
            arr_frees: FxSet::default(),
            ro_consts: FxMap::default(),
            stable_places: FxSet::default(),
            reassigned_memo: FxMap::default(),
            quiet_inst_memo: FxMap::default(),
            cur_calls: Vec::new(),
            pquiet_stack: Vec::new(),
            called_stack: Vec::new(),
            rec_cvs: FxMap::default(),
            needs_uncaught: false,
            test_call_done: false,
            pending_async: None,
            pending_gen: None,
            checked_project: None,
            self_closure: None,
            ranges: None,
            js_mul_operand: false,
            float_only: None,
            resizers: FxMap::default(),
            fresh_arrays: FxMap::default(),
            rec_structs: FxMap::default(),
            struct_depth: 0,
            pending_class_structs: Vec::new(),
            box_all: std::env::var("TOV_BOX_ALL").is_ok_and(|v| v == "1"),
            last_children: Vec::new(),
            agen_helpers: FxMap::default(),
            pending_tre: None,
            pending_direct: None,
            direct_counts: FxMap::default(),
            arrow_last: FxMap::default(),
            store_arr: None,
            return_span: None,
            recur: None,
            unassigned_params: FxMap::default(),
            uses_tls: false,
            uses_regexp: false,
            frame_defs: Vec::new(),
            js: js::JsGen::default(),
        }
    }

    pub(crate) fn fresh(&mut self, prefix: &str) -> String {
        self.counter += 1;
        format!("{prefix}{}", self.counter)
    }

    pub(crate) fn unsupported(&mut self, span: Span, what: &str) {
        self.errors.push(
            Diagnostic::new("C0001", span, format!("{what} is not supported by the native backend yet"))
                .note("note", "`tov check` accepts it; code generation for it is still being built"),
        );
    }

    pub(crate) fn facts(&self, m: u32) -> &ModuleFacts {
        &self.c.facts.as_ref().expect("built with facts")[m as usize]
    }

    pub(crate) fn ast(&self, m: u32) -> &'a ast::Ast {
        &self.c.modules[m as usize].ast
    }

    pub(crate) fn sym(&self, s: Sym) -> &str {
        self.c.interner.get(s)
    }

    pub(crate) fn tget(&self, t: TyId) -> Ty {
        match *self.c.types.get(t) {
            // (`true` as a type is a `bool` that's always true: the same C value)
            Ty::BoolLit(_) => Ty::Bool,
            // (a recursive alias for an array: the array, see `flatten_recs`)
            Ty::Rec(..) if let Some(&b) = self.rec_flat.get(&t) => *self.c.types.get(b),
            // (`any` is held as `unknown` is: a `tv_dyn`)
            Ty::Any => Ty::Unknown,
            // (a rest parameter, `...xs: T[]`: its array)
            Ty::Spread(a) if matches!(self.c.types.get(a), Ty::Array(_)) => *self.c.types.get(a),
            t => t,
        }
    }

    /// A recursive alias whose body is an array (`type Tree = (Tree | undefined)[]`) needs no
    /// box: an array's elements are behind its header whatever they are, so the alias is held
    /// as the array itself.
    fn flatten_recs(&mut self) {
        for i in 0..self.c.types.count() {
            let t = TyId(i);
            if let Ty::Rec(_, args) = *self.c.types.get(t)
                && self.c.types.tys(args).is_empty()
            {
                let body = self.c.unfold(t);
                if matches!(self.c.types.get(body), Ty::Array(_)) {
                    self.rec_flat.insert(t, body);
                }
            }
        }
    }

    /// `t` as held: a flattened recursive alias is its array (see `flatten_recs`).
    pub(crate) fn held(&self, t: TyId) -> TyId {
        self.rec_flat.get(&t).copied().unwrap_or(t)
    }

    // ------------------------------------------------------------ program

    fn program(&mut self, mode: Mode) {
        let modules = self.c.modules;
        let mut main_fn = None;
        let mut tests = Vec::new();
        for (mi, module) in modules.iter().enumerate() {
            for (ii, item) in module.ast.items.iter().enumerate() {
                match &item.kind {
                    ItemKind::Function(f) if self.sym(f.name) == "main" && main_fn.is_none() => main_fn = Some((mi as u32, ii as u32)),
                    ItemKind::Test { name, body, suite, .. } => tests.push((mi as u32, name.clone(), *body, *suite, item.span.start)),
                    _ => {}
                }
            }
        }
        for mi in 0..modules.len() {
            let globals: Vec<(u32, u32)> = self.facts(mi as u32).mutated_globals.iter().copied().collect();
            self.mutated_globals.extend(globals);
        }
        let script = modules.iter().enumerate().find(|(_, m)| m.entry && m.ast.script.is_some()).map(|(mi, m)| (mi as u32, m.ast.script.unwrap()));
        // Then the event loop: timers, tasks and servers the program started.
        let async_run = ASYNC_RUN;
        let mut main_body = String::new();
        // Imported modules' top-level statements first, each module after those it imports.
        let mut order: Vec<u32> = Vec::new();
        fn visit(mods: &[crate::check::Module], m: u32, order: &mut Vec<u32>, seen: &mut FxSet<u32>) {
            if !seen.insert(m) {
                return;
            }
            let mut deps: Vec<u32> = mods[m as usize].imports.values().copied().collect();
            deps.sort();
            for d in deps {
                visit(mods, d, order, seen);
            }
            order.push(m);
        }
        let mut seen = FxSet::default();
        for mi in 0..modules.len() as u32 {
            visit(modules, mi, &mut order, &mut seen);
        }
        for mi in order {
            let module = &modules[mi as usize];
            if module.entry || module.std || module.builtin || module.npm.is_some() {
                continue;
            }
            if let Some(si) = module.ast.script {
                let cname = self.instance(mi, si, FxMap::default());
                let _ = writeln!(main_body, "    {cname}();\n    if (tvg_err) {{ tvg_uncaught(); return 1; }}");
                self.needs_uncaught = true;
            }
        }
        match mode {
            Mode::Run if script.is_some() && self.is_async_fn(script.unwrap().0, script.unwrap().1) => {
                let (m, i) = script.unwrap();
                self.script_module = Some(m);
                let cname = self.instance(m, i, FxMap::default());
                self.async_main(&mut main_body, &cname, VOID);
            }
            Mode::Run if script.is_some() => {
                let (m, i) = script.unwrap();
                self.script_module = Some(m);
                let cname = self.instance(m, i, FxMap::default());
                // A module-level `const x = try …` initializer can also stop the script with
                // `tvg_err` set (consts evaluate out of the script body, so its `throws` sig
                // doesn't cover them).
                let throws = self.c.sigs[&(m, i)].throws != NEVER || !self.c.const_throws.is_empty();
                self.needs_uncaught |= throws;
                let uncaught = if throws { "    if (tvg_err) { tvg_uncaught(); return 1; }\n" } else { "" };
                let _ = writeln!(main_body, "    {cname}();\n{uncaught}{async_run}    tv_out_flush();\n    return 0;");
            }
            Mode::Run => {
                let Some((m, i)) = main_fn else {
                    self.errors.push(Diagnostic::new("C0002", crate::source::Span::new(modules[0].file, 0, 0), "no `function main()` to run"));
                    return;
                };
                let cname = self.instance(m, i, FxMap::default());
                let ret = self.c.sigs[&(m, i)].ret;
                if self.is_async_fn(m, i) {
                    let value = self.async_value_type(ret);
                    self.async_main(&mut main_body, &cname, value);
                    return self.finish_program(main_body);
                }
                let throws = self.c.sigs[&(m, i)].throws != NEVER || !self.c.const_throws.is_empty();
                self.needs_uncaught |= throws;
                let uncaught = if throws { "    if (tvg_err) { tvg_uncaught(); return 1; }\n" } else { "" };
                if ret == INT {
                    let _ = writeln!(main_body, "    tv_int code = {cname}();\n{uncaught}{async_run}    tv_out_flush();\n    return (int)code;");
                } else {
                    let _ = writeln!(main_body, "    {cname}();\n{uncaught}{async_run}    tv_out_flush();\n    return 0;");
                }
            }
            Mode::Test => {
                self.needs_uncaught = true;
                // (in the order they're written: a test item made later, a script's call of a
                // function declaring tests, runs in its place)
                tests.sort_by_key(|t| (t.0, t.4));
                for (m, name, body, suite, _) in tests {
                    let f = self.test_fn(m, body);
                    let _ = writeln!(main_body, "    tv_test_run_ctx({}, {}, tv_test_call, (void *){f}, {suite});", c_string(name.as_bytes()), name.len());
                }
                let _ = writeln!(main_body, "    tv_out_flush();\n    return tv_test_summary();");
            }
        }
        self.finish_program(main_body);
    }

    /// The program's top level (an async `main` or script) runs as the first task.
    fn async_main(&mut self, out: &mut String, cname: &str, value: TyId) {
        self.needs_uncaught = true;
        let desc = self.desc(value);
        let code = if value == INT { "(int)*(tv_int *)tv_promise_value(mp)" } else { "0" };
        let _ = writeln!(
            out,
            "    tv_task *t = tv_task_new(sizeof(AF_{cname}), {cname}_task, {desc});\n    tv_promise *mp = t->promise;\n    tv_promise_retain(mp);\n    tv_task_start(t);\n    tv_async_run();\n    if (mp->state == TV_REJECTED) {{ tvg_err = mp->err; tvg_obj_retain(tvg_err); tv_promise_release(mp); tvg_uncaught(); return 1; }}\n    if (mp->state == TV_PENDING) {{ tv_promise_release(mp); tv_out_flush(); tv_err_cstr(\"warning: the program's top-level await never finished\\n\"); return 13; }}\n    int code = {code};\n    tv_promise_release(mp);\n    tv_out_flush();\n    return code;"
        );
    }

    fn finish_program(&mut self, main_body: String) {
        // Rejections nobody handled are reported like uncaught errors.
        self.needs_uncaught = true;
        self.helpers_after.push("static void tvg_async_retain(void *e) { tvg_obj_retain(e); }\nstatic void tvg_async_release(void *e) { tvg_obj_release(e); }\nstatic void tvg_async_report(void *e) { tvg_err = e; tvg_obj_retain(e); tvg_uncaught(); }\n".to_string());
        let _ = writeln!(self.protos, "static void tvg_async_retain(void *e);\nstatic void tvg_async_release(void *e);\nstatic void tvg_async_report(void *e);");
        if self.needs_uncaught {
            let uncaught = self.uncaught_fn();
            self.helpers_after.push(uncaught);
        }
        // Emit queued functions, constructors and methods (they may queue more); dispatch
        // functions need every instantiated class, so they're recomputed until nothing new appears.
        loop {
            while let Some(inst) = self.queue.pop() {
                self.emit_function(inst);
            }
            if let Some(w) = self.class_work.pop() {
                self.emit_class_work(w);
                continue;
            }
            if !self.refresh_dispatchers() && self.queue.is_empty() && self.class_work.is_empty() {
                // (the `any` switches, over every type boxed so far: what they call is made too,
                // and may box more)
                if self.any_dispatch() {
                    continue;
                }
                break;
            }
        }
        if !self.any_code.is_empty() {
            let code = std::mem::take(&mut self.any_code);
            self.helpers_after.push(code);
        }
        let classes_before = self.class_list.len();
        let tables = self.emit_class_tables();
        // Tables (printing) may call methods not called elsewhere (`toString`): emit them too.
        loop {
            while let Some(inst) = self.queue.pop() {
                self.emit_function(inst);
            }
            if let Some(w) = self.class_work.pop() {
                self.emit_class_work(w);
                continue;
            }
            break;
        }
        if self.class_list.len() != classes_before {
            self.internal.push("a class was first used by a printing method".into());
        }
        while let Some(t) = self.pending_boxes.pop() {
            self.define_struct(t);
        }
        // A never-shared box has no count: its "count" reads as 1, so releasing frees it.
        let shared = self.shared_boxes();
        for (t, name) in &self.rec_boxes {
            if shared.contains(t) {
                let _ = writeln!(self.box_defs, "#define RCF_{name} int64_t rc;\n#define RC_{name}(b) ((b)->rc)");
            } else {
                let _ = writeln!(self.box_defs, "#define RCF_{name}\n#define RC_{name}(b) ((int64_t){{1}})");
            }
        }
        let bare = self.bare_classes(&shared);
        for t in self.class_list.clone() {
            let name = self.class_insts[&t].name.clone();
            if bare.contains(&t) {
                let _ = writeln!(self.box_defs, "#define TVG_HDR_{name}\n#define TVG_INIT_{name}(o, cid) ((void)0)\n#define TVG_DEC_{name}(p) 0");
            } else {
                let _ = writeln!(self.box_defs, "#define TVG_HDR_{name} tvg_obj h;\n#define TVG_INIT_{name}(o, cid) tvg_obj_init(o, cid)\n#define TVG_DEC_{name}(p) (--((tvg_obj *)(p))->rc)");
            }
        }
        let dispatch = std::mem::take(&mut self.dispatch_bodies);
        self.helpers_after.push(dispatch);
        self.helpers_after.push(tables);
        let mut inits = std::mem::take(&mut self.const_init);
        inits.sort_by_key(|x| x.0);
        let mut init: String = inits.into_iter().map(|x| x.1).collect();
        // Programs that never start a task, timer or server skip the event loop entirely, so
        // dead stripping drops it (and the HTTP runtime behind it) from the binary.
        let mut main_body = main_body;
        // A program that fetches links TLS (runtime/tls.c with mbedTLS; see build.rs): this call
        // is what pulls it in.
        if self.funcs.contains("tv_native_fetchStart(") || self.funcs.contains("tv_native_netConnect(") || self.funcs.contains("tv_native_netListen(") || self.funcs.contains("tv_native_wsConnect(") {
            init.insert_str(0, "    tv_tls_install();\n");
            self.uses_tls = true;
        }
        // (std/crypto's hashes and random bytes: the same library's BoringSSL)
        if self.funcs.contains("tv_native_crypto") {
            init.insert_str(0, "    tv_crypto_install();\n");
            self.uses_tls = true;
        }
        // (the program's pattern literals compiled to C, given to the runtime before any is
        // compiled: see rejit.rs)
        if self.uses_regexp && std::env::var_os("TOV_NO_REJIT").is_none() {
            let pats = self.pattern_literals();
            if let Some((code, n)) = rejit::matchers(&pats) {
                self.helpers.push_str(rejit::EXEC_H);
                self.helpers.push_str(rejit::PRELUDE);
                self.helpers.push_str(&code);
                init.insert_str(0, &format!("    tv_rejit_table(tvg_rejits, {n});\n"));
            }
        }
        // (Math.random's seed, read from TOV_SEED once, before the first draw: tv_random has no
        // check of its own)
        if self.funcs.contains("tv_random()") || main_body.contains("tv_random()") {
            init.insert_str(0, "    tv_random_init();\n");
        }
        // A program importing npm packages: JavaScript exceptions become JsError objects.
        if self.js.used {
            // the roots of JavaScript values Tov holds: in this frame, which the collector scans
            init.insert_str(0, "    JSValueRef tv_js_root_keys[1 << 12]; tv_js_roots_init(tv_js_root_keys, 1 << 12);\n    tv_js_make_error = tvg_js_make_error;\n");
        }
        // (an npm program runs the event loop to the end: JavaScript's timers and callbacks,
        // as Node.js does, even when `main` is synchronous)
        if self.uses_event_loop() || main_body.contains("tv_task_") || self.js.used {
            init.insert_str(0, "    tv_async_init(tvg_async_retain, tvg_async_release, tvg_async_report);\n");
        } else {
            main_body = main_body.replace(ASYNC_RUN, "");
        }
        // An npm program's end goes through Node.js's: 'exit' listeners, and process.exitCode
        // (runtime/node.c's tv_node_exit_code), with the status `main` left as the default.
        let (body_name, exit_hook) = if self.js.used {
            ("tvg_program_body", "static int tvg_program_body(void);\nstatic int tvg_program(void) { return tv_node_exit_code(tvg_program_body()); }\n")
        } else {
            ("tvg_program", "")
        };
        // The program runs on a thread with a 1 GiB stack (reserved, committed as used): deep
        // recursion — including freeing long linked structures — has room without per-call checks.
        // On Linux, a stack that size on the main thread instead (tv_run_on_stack), but for an npm
        // program: JavaScriptCore reads the thread's own stack bounds.
        let switch = if self.js.used { "" } else { "#ifdef TV_STACK_SWITCH\n    { int status; if (tv_run_on_stack(tvg_program, (size_t)1 << 30, &status)) return status; }\n#endif\n" };
        let _ = writeln!(
            self.funcs,
            "static int tvg_argc; static char **tvg_argv; static int tvg_exit;\n{exit_hook}static int {body_name}(void) {{\n    tv_init(tvg_argc, tvg_argv);\n{init}{main_body}}}\nstatic void *tvg_thread(void *arg) {{ (void)arg; tvg_exit = tvg_program(); return NULL; }}\nint main(int argc, char **argv) {{\n    tvg_argc = argc; tvg_argv = argv;\n#if defined(__APPLE__) && defined(TVG_MAIN_STACK)\n    /* Linked with a 512 MiB main stack (build.rs): run right here. */\n    return tvg_program();\n#elif defined(__APPLE__)\n    if (pthread_get_stacksize_np(pthread_self()) >= ((size_t)256 << 20)) return tvg_program();\n#endif\n{switch}    pthread_attr_t attr; pthread_t th;\n    if (pthread_attr_init(&attr) == 0 && pthread_attr_setstacksize(&attr, (size_t)1 << 30) == 0 && TVG_THREAD_QOS(&attr) && pthread_create(&th, &attr, tvg_thread, NULL) == 0) {{\n        pthread_join(th, NULL);\n        return tvg_exit;\n    }}\n    return tvg_program();\n}}"
        );
    }

    /// Recursive-alias boxes that can be shared: those reachable from a retained type. Retaining
    /// a record or union retains its members (and, to be safe, a box counts its contents too).
    fn shared_boxes(&mut self) -> FxSet<TyId> {
        let mut seen = FxSet::default();
        let mut stack: Vec<TyId> = self.rc_roots.iter().copied().collect();
        while let Some(t) = stack.pop() {
            if !seen.insert(t) {
                continue;
            }
            match self.tget(t) {
                Ty::Record(fs) => stack.extend(self.c.types.fields(fs).iter().map(|f| f.ty)),
                Ty::Union(ms) => stack.extend(self.c.types.tys(ms).iter().copied()),
                Ty::Rec(..) => stack.push(self.c.unfold(t)),
                _ => {}
            }
        }
        seen
    }

    /// Whether the generated code can leave work for the event loop: tasks, promises,
    /// microtasks, timers or servers.
    fn uses_event_loop(&self) -> bool {
        const HOOKS: [&str; 7] = ["tv_task_", "tv_promise", "tv_queue_microtask", "tv_set_timer", "tv_native_http", "tv_native_net", "tv_native_ws"];
        let parts = [&self.funcs, &self.helpers, &self.helpers_after_decl].into_iter().chain(self.helpers_after.iter());
        parts.into_iter().any(|s| HOOKS.iter().any(|h| s.contains(h)))
    }

    /// The program's C: the runtime header, the prelude and the generated code. It links
    /// against the runtime object (see `runtime_source`); `standalone` makes it self-contained.
    /// The (source, flags) of the program's regular expression literals, each once.
    fn pattern_literals(&self) -> Vec<(String, String)> {
        let Some(re) = self.c.interner.lookup("__reLiteral") else { return Vec::new() };
        let mut out: Vec<(String, String)> = Vec::new();
        for m in self.c.modules.iter() {
            for e in &m.ast.exprs {
                let crate::ast::ExprKind::Call { callee, args, .. } = &e.kind else { continue };
                if !matches!(m.ast.expr(*callee).kind, crate::ast::ExprKind::Ident(n) if n == re) || args.len() != 2 {
                    continue;
                }
                let (crate::ast::ExprKind::Str(p), crate::ast::ExprKind::Str(f)) = (&m.ast.expr(args[0].expr).kind, &m.ast.expr(args[1].expr).kind) else { continue };
                let pair = (self.c.interner.get(*p).to_string(), self.c.interner.get(*f).to_string());
                if !out.contains(&pair) {
                    out.push(pair);
                }
            }
        }
        out
    }

    fn assemble(&self) -> String {
        let mut out = String::with_capacity(RUNTIME_H.len() + JS_H.len() + PRELUDE.len() + self.funcs.len() * 2);
        out.push_str(&fixed_prefix());
        // Descriptors reach their console.log formatter through TVG_INS: NULL unless the program
        // inspects something, so the formatter isn't linked into programs that never do.
        out.push_str(if self.uses_inspect { "#define TVG_INS(f) f\n" } else { "#define TVG_INS(f) NULL\n" });
        out.push_str(&self.box_defs);
        let frames = self.ordered_frames();
        for s in [&self.typedefs, &self.structs, &self.class_structs, &self.env_structs, &frames, &self.lits, &self.protos, &self.helpers, &self.funcs] {
            out.push_str(s);
            out.push('\n');
        }
        out.push_str(&self.helpers_after_decl);
        for h in &self.helpers_after {
            out.push_str(h);
        }
        out
    }

    // ------------------------------------------------------------ names, literals, locations

    /// A decimal literal (checked to fit) as a constant.
    pub(crate) fn dec_lit(&mut self, text: &str) -> String {
        let (c, s) = crate::types::decimal_literal(text).unwrap_or((0, 0));
        let v = ((c as u128) << 8) | s as u128;
        format!("((tv_dec){{UINT64_C({}), UINT64_C({})}})", v as u64, (v >> 64) as u64)
    }

    /// A bigint literal's value (borrowed): a small one is a constant word, a larger one is built
    /// once, on first use, and never freed.
    pub(crate) fn big_lit(&mut self, text: &str) -> String {
        let (digits, radix) = match text.get(..2) {
            Some("0x" | "0X") => (&text[2..], 16),
            Some("0o" | "0O") => (&text[2..], 8),
            Some("0b" | "0B") => (&text[2..], 2),
            _ => (text, 10),
        };
        if let Ok(v) = i128::from_str_radix(digits, radix)
            && v < (1i128 << 62)
        {
            return format!("((tv_big){{INT64_C({})}})", 2 * v + 1);
        }
        let key = [b"\0big:".as_slice(), text.as_bytes()].concat();
        if let Some(n) = self.lit_names.get(&key) {
            return format!("{n}()");
        }
        let n = format!("tvg_big{}", self.lit_names.len());
        let _ = writeln!(self.lits, "static tv_big {n}(void) {{ static tv_big v; if (TV_BIG_UNLIKELY(!v.v)) v = tv_big_lit({}); return v; }}", c_string(text.as_bytes()));
        self.lit_names.insert(key, n.clone());
        format!("{n}()")
    }

    pub(crate) fn lit(&mut self, bytes: &[u8]) -> String {
        if let Some(n) = self.lit_names.get(bytes) {
            return format!("TVG_LIT({n})");
        }
        let n = format!("L{}", self.lit_names.len());
        let flag = if bytes.is_ascii() { "TV_STR_ASCII" } else { "TV_STR_WIDE" };
        let _ = writeln!(self.lits, "TVG_STR_LIT({n}, {}, {flag});", c_string(bytes));
        self.lit_names.insert(bytes.to_vec(), n.clone());
        format!("TVG_LIT({n})")
    }

    /// A C string constant "file.tov:line:col" for traps.
    pub(crate) fn loc(&mut self, span: Span) -> String {
        let file = self.c.sm.get(span.file);
        let (line, col) = file.line_col(span.start);
        let text = format!("{}:{line}:{col}", file.name);
        if let Some(n) = self.loc_names.get(&text) {
            return n.clone();
        }
        let n = format!("LOC{}", self.loc_names.len());
        let _ = writeln!(self.lits, "static const char {n}[] = {};", c_string(text.as_bytes()));
        self.loc_names.insert(text, n.clone());
        n
    }

    // ------------------------------------------------------------ types

    /// Does this type own heap memory (needs retain/release)?
    pub(crate) fn is_rc(&mut self, t: TyId) -> bool {
        match self.tget(t) {
            Ty::Str | Ty::Array(_) | Ty::Map(..) | Ty::Set(_) | Ty::Func(..) | Ty::Rec(..) | Ty::Class(..) | Ty::Interface(..) | Ty::Promise(..) | Ty::Gen(_) | Ty::AsyncGen(_) | Ty::Js | Ty::BigInt | Ty::Unknown => true,
            Ty::Record(_) if self.is_obj(t) => true,
            Ty::Record(fs) => {
                let fs = self.c.types.fields(fs).to_vec();
                fs.iter().any(|f| self.is_rc(f.ty))
            }
            Ty::Union(ms) => {
                let ms = self.c.types.tys(ms).to_vec();
                ms.iter().any(|&m| self.is_rc(m))
            }
            _ => false,
        }
    }

    /// A record type whose values are objects, shared as JavaScript's (the checker's `objects`):
    /// a pointer to a counted box holding the record, rather than the record itself.
    /// Module `m` is TypeScript (`.ts`): its integers are numbers, as JavaScript rounds them past
    /// 2^53 (see TV_JADD).
    pub(crate) fn js_ints(&self, m: u32) -> bool {
        matches!(self.c.modules[m as usize].path.extension().and_then(|e| e.to_str()), Some("ts" | "tsx" | "mts" | "cts"))
    }

    pub(crate) fn is_obj(&self, t: TyId) -> bool {
        let Ty::Record(fs) = *self.c.types.get(t) else { return false };
        if self.box_all || self.c.objects.contains(&t) {
            return true;
        }
        let fs = self.c.types.fields(fs);
        (!self.c.object_fields.is_empty() && fs.iter().any(|f| self.c.object_fields.contains(&f.name)))
            || (!self.c.object_shapes.is_empty() && self.c.object_shapes.contains(&fs.iter().map(|f| f.name).collect::<Vec<_>>()))
    }

    /// The C struct of a record type's fields (inside the box, for an object).
    pub(crate) fn rec_struct(&mut self, t: TyId) -> String {
        let ct = self.ctype(t);
        self.rec_structs.get(&t).cloned().unwrap_or(ct)
    }

    /// A new record value from designated initializers (`.f_x = ...`): an object is boxed.
    pub(crate) fn rec_lit(&mut self, t: TyId, parts: &str) -> String {
        let st = self.rec_struct(t);
        self.rec_wrap(t, &format!("(({st}){{ {parts} }})"))
    }

    /// The value of record type `t` holding the record (struct) `code`: boxed for an object.
    pub(crate) fn rec_wrap(&mut self, t: TyId, code: &str) -> String {
        if self.is_obj(t) {
            let o = self.box_name(t);
            format!("mk_{o}({code})")
        } else {
            code.to_string()
        }
    }

    /// Field `name` of record value `code` (an lvalue when `code` is one).
    pub(crate) fn fld(&self, t: TyId, code: &str, name: &str) -> String {
        if self.is_obj(t) { format!("({code})->v.f_{name}") } else { format!("({code}).f_{name}") }
    }

    pub(crate) fn is_unit(&self, t: TyId) -> bool {
        matches!(self.tget(t), Ty::StrLit(_) | Ty::Undefined | Ty::Null | Ty::Void | Ty::Never)
    }

    /// The C type for a Tov type; defines structs on first use.
    pub(crate) fn ctype(&mut self, t: TyId) -> String {
        if let Some(n) = self.ctypes.get(&t) {
            return n.clone();
        }
        let name: String = match self.tget(t) {
            Ty::Int => "tv_int".into(),
            Ty::F64 => "double".into(),
            Ty::F32 => "float".into(),
            Ty::I8 => "int8_t".into(),
            Ty::I16 => "int16_t".into(),
            Ty::I32 => "int32_t".into(),
            Ty::U8 | Ty::U8C => "uint8_t".into(),
            Ty::U16 => "uint16_t".into(),
            Ty::U32 => "uint32_t".into(),
            Ty::U64 => "uint64_t".into(),
            Ty::Bool | Ty::BoolLit(_) => "bool".into(),
            Ty::Str => "tv_str".into(),
            Ty::BigInt => "tv_big".into(),
            Ty::Decimal => "tv_dec".into(),
            // (a class given as a value: nothing at run time, each call's code is its own)
            Ty::StrLit(_) | Ty::Undefined | Ty::Null | Ty::Void | Ty::Never | Ty::Error | Ty::ClassObj(_) => "tv_unit".into(),
            Ty::Unknown | Ty::Any => "tv_dyn".into(),
            Ty::Array(_) => "tv_arr".into(),
            Ty::Promise(..) => "tv_promise *".into(),
            Ty::Gen(_) | Ty::AsyncGen(_) => "tv_gen *".into(),
            Ty::Js => "JSValueRef".into(),
            Ty::Spread(_) => "tv_int".into(),
            Ty::Map(..) | Ty::Set(_) => "tv_map".into(),
            Ty::Func(..) => "tv_fn".into(),
            Ty::Record(_) if self.is_obj(t) => {
                // (the box is defined after the record: other types only hold pointers to it)
                let sfx = self.type_suffix(t);
                let (r, o) = (format!("R{}{sfx}", t.0), format!("O{}{sfx}", t.0));
                let _ = writeln!(self.typedefs, "typedef struct {r} {r};\ntypedef struct {o} {o};");
                let p = format!("{o}*");
                self.ctypes.insert(t, p.clone());
                self.rec_structs.insert(t, r);
                self.define_struct(t);
                return p;
            }
            Ty::Record(_) => {
                let n = format!("R{}{}", t.0, self.type_suffix(t));
                let _ = writeln!(self.typedefs, "typedef struct {n} {n};");
                self.ctypes.insert(t, n.clone());
                self.define_struct(t);
                return n;
            }
            Ty::Union(ms) if self.niche(t).is_some() => {
                let (pi, _) = self.niche(t).unwrap();
                let m = self.c.types.tys(ms)[pi];
                let p = self.ctype(m);
                self.ctypes.insert(t, p.clone());
                return p;
            }
            Ty::Union(_) => {
                let n = format!("U{}{}", t.0, self.type_suffix(t));
                let _ = writeln!(self.typedefs, "typedef struct {n} {n};");
                self.ctypes.insert(t, n.clone());
                self.define_struct(t);
                return n;
            }
            Ty::Rec(..) => {
                // Boxes are only used through pointers: the body (which holds the value) is defined
                // later, so types that contain the box pointer never wait for it.
                let n = format!("B{}{}", t.0, self.type_suffix(t));
                let _ = writeln!(self.typedefs, "typedef struct {n} {n};");
                let p = format!("{n}*");
                self.ctypes.insert(t, p.clone());
                self.pending_boxes.push(t);
                return p;
            }
            Ty::Class(..) => {
                let ci = self.class_inst(t);
                let n = ci.name.clone();
                let _ = writeln!(self.typedefs, "typedef struct {n} {n};");
                let p = format!("{n}*");
                self.ctypes.insert(t, p.clone());
                // (held by pointer: met in another struct's fields, defined once that struct is,
                // so what the class holds by value never comes before what that struct needs)
                if self.struct_depth > 0 {
                    self.pending_class_structs.push(t);
                } else {
                    self.define_struct(t);
                }
                return p;
            }
            Ty::Interface(..) => "tv_iface".into(),
            Ty::Param(_) | Ty::Namespace(_) | Ty::BuiltinNs(_) | Ty::Expect(_) => "tv_unit".into(),
        };
        self.ctypes.insert(t, name.clone());
        name
    }

    fn type_suffix(&self, t: TyId) -> String {
        match self.c.types.name_of(t) {
            Some(n) => format!("_{}", n.chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect::<String>()),
            None => String::new(),
        }
    }

    /// Box struct name for a recursive alias type (without the pointer).
    pub(crate) fn box_name(&mut self, t: TyId) -> String {
        let p = self.ctype(t);
        p.trim_end_matches('*').to_string()
    }

    fn define_struct(&mut self, t: TyId) {
        self.struct_depth += 1;
        self.define_struct_in(t);
        self.struct_depth -= 1;
        if self.struct_depth == 0 {
            while let Some(c) = self.pending_class_structs.pop() {
                self.struct_depth += 1;
                self.define_struct_in(c);
                self.struct_depth -= 1;
            }
        }
    }

    /// `define_struct` of a type needed complete now (a base class, embedded by value).
    pub(crate) fn define_struct_now(&mut self, t: TyId) {
        self.pending_class_structs.retain(|&c| c != t);
        self.struct_depth += 1;
        self.define_struct_in(t);
        self.struct_depth -= 1;
    }

    fn define_struct_in(&mut self, t: TyId) {
        if !self.defined.insert(t) {
            return;
        }
        let name = self.rec_structs.get(&t).cloned().unwrap_or_else(|| self.ctypes[&t].trim_end_matches('*').to_string());
        let mut body = String::new();
        match self.tget(t) {
            Ty::Record(fs) => {
                let fs = self.c.types.fields(fs).to_vec();
                let _ = writeln!(body, "struct {name} {{");
                if fs.is_empty() {
                    body.push_str("    tv_unit _empty;\n");
                }
                for f in &fs {
                    let ct = self.ctype(f.ty);
                    let _ = writeln!(body, "    {ct} f_{};", self.sym(f.name));
                }
                body.push_str("};\n");
                if self.is_obj(t) {
                    let o = self.box_name(t);
                    let _ = writeln!(self.class_structs, "struct {o} {{ int64_t rc; {name} v; }};\nstatic inline {o} *mk_{o}({name} v) {{ {o} *b = tvg_alloc_small(sizeof({o})); b->rc = 1; b->v = v; return b; }}");
                }
            }
            Ty::Union(ms) => {
                let ms = self.c.types.tys(ms).to_vec();
                let _ = writeln!(body, "struct {name} {{\n    uint32_t tag;\n    union {{");
                let mut any = false;
                for (i, &m) in ms.iter().enumerate() {
                    if self.is_unit(m) {
                        continue;
                    }
                    any = true;
                    let ct = self.ctype(m);
                    let _ = writeln!(body, "        {ct} m{i};");
                }
                if !any {
                    body.push_str("        tv_unit _none;\n");
                }
                body.push_str("    } u;\n};\n");
            }
            Ty::Rec(..) => {
                let inner = self.c.unfold(t);
                let ct = self.ctype(inner);
                // The count is left out when no box of this type is ever shared (see `shared_boxes`).
                let _ = writeln!(body, "struct {name} {{\n    RCF_{name}\n    {ct} v;\n}};");
                self.rec_boxes.push((t, name.clone()));
                self.class_structs.push_str(&body);
                return;
            }
            Ty::Class(..) => {
                let b = self.define_class_struct(t);
                self.class_structs.push_str(&b);
                return;
            }
            _ => {}
        }
        // Struct bodies are appended after their by-value dependencies (defined by the recursive ctype calls above).
        self.structs.push_str(&body);
    }

    /// `C | undefined` for a class `C` is a nullable pointer (`NULL` = undefined) rather than a
    /// tagged union: (index of the class member, index of `undefined`).
    pub(crate) fn niche(&self, u: TyId) -> Option<(usize, usize)> {
        let Ty::Union(ms) = self.tget(u) else { return None };
        let ms = self.c.types.tys(ms);
        if ms.len() != 2 {
            return None;
        }
        // (`C | null` too: NULL for `null`)
        let ui = ms.iter().position(|&m| m == UNDEFINED || m == NULL)?;
        let pi = 1 - ui;
        // A class instance, a recursive alias's box or an object: a pointer, with NULL for
        // `undefined`; an array: its header pointer NULL.
        (matches!(self.tget(ms[pi]), Ty::Class(..) | Ty::Rec(..) | Ty::Array(_) | Ty::Str) || self.is_obj(ms[pi])).then_some((pi, ui))
    }

    /// A niche union holding a string (`string | undefined`): absent is a NULL buffer pointer.
    pub(crate) fn niche_str(&self, u: TyId) -> bool {
        let Some((pi, _)) = self.niche(u) else { return false };
        let Ty::Union(ms) = self.tget(u) else { return false };
        matches!(self.tget(self.c.types.tys(ms)[pi]), Ty::Str)
    }

    /// A niche union holding an array (`T[] | undefined`): absent is a NULL header pointer.
    pub(crate) fn niche_arr(&self, u: TyId) -> bool {
        let Some((pi, _)) = self.niche(u) else { return false };
        let Ty::Union(ms) = self.tget(u) else { return false };
        matches!(self.tget(self.c.types.tys(ms)[pi]), Ty::Array(_))
    }

    /// A niche union's value `code` is there (not its absent `undefined` / `null`).
    pub(crate) fn niche_there(&self, code: &str, u: TyId) -> String {
        if self.niche_arr(u) || self.niche_str(u) { format!("(({code}).p != NULL)") } else { format!("(({code}) != NULL)") }
    }

    /// A niche union's absent value.
    pub(crate) fn niche_none(&self, u: TyId) -> &'static str {
        if self.niche_arr(u) {
            "((tv_arr){NULL})"
        } else if self.niche_str(u) {
            "((tv_str){NULL})"
        } else {
            "NULL"
        }
    }

    /// How a niche union's absent value prints: `undefined`, or `null` for `C | null`.
    pub(crate) fn niche_absent(&self, u: TyId) -> &'static str {
        match self.tget(u) {
            Ty::Union(ms) if self.c.types.tys(ms).contains(&NULL) => "null",
            _ => "undefined",
        }
    }

    /// The recursive-alias or object member of a niche union (`Tree | undefined`), if that's
    /// what it holds (rather than a class instance).
    pub(crate) fn niche_rec(&self, u: TyId) -> Option<TyId> {
        let (pi, _) = self.niche(u)?;
        let Ty::Union(ms) = self.tget(u) else { return None };
        let m = self.c.types.tys(ms)[pi];
        (matches!(self.tget(m), Ty::Rec(..) | Ty::Array(_) | Ty::Str) || self.is_obj(m)).then_some(m)
    }

    /// The tag of a union value (for `switch`).
    pub(crate) fn u_tag(&self, code: &str, u: TyId) -> String {
        match self.niche(u) {
            Some((pi, ui)) => format!("({} ? {pi} : {ui})", self.niche_there(code, u)),
            None => format!("({code}).tag"),
        }
    }

    /// Does a union value hold member `k`?
    pub(crate) fn u_is(&self, code: &str, u: TyId, k: usize) -> String {
        match self.niche(u) {
            Some((pi, _)) if k == pi => self.niche_there(code, u),
            Some(_) => format!("(!{})", self.niche_there(code, u)),
            None => format!("(({code}).tag == {k})"),
        }
    }

    /// The payload of member `k` (an lvalue when `code` is one).
    pub(crate) fn u_payload(&self, code: &str, u: TyId, k: usize) -> String {
        match self.niche(u) {
            Some(_) => format!("({code})"),
            None => format!("({code}).u.m{k}"),
        }
    }

    /// A union value holding member `k` (`payload`: its C value, `None` for unit members).
    pub(crate) fn u_make(&mut self, u: TyId, k: usize, payload: Option<&str>) -> String {
        match self.niche(u) {
            Some((pi, _)) if k == pi => payload.unwrap_or(self.niche_none(u)).to_string(),
            Some(_) => self.niche_none(u).to_string(),
            None => {
                let ct = self.ctype(u);
                match payload {
                    Some(p) => format!("({ct}){{ .tag = {k}, .u.m{k} = {p} }}"),
                    None => format!("({ct}){{ .tag = {k} }}"),
                }
            }
        }
    }

    /// Union members in tag order.
    pub(crate) fn members(&self, t: TyId) -> Vec<TyId> {
        match self.tget(t) {
            Ty::Union(ms) => self.c.types.tys(ms).to_vec(),
            _ => vec![t],
        }
    }

    pub(crate) fn tag_of(&self, union: TyId, member: TyId) -> Option<usize> {
        self.members(union).iter().position(|&m| m == member)
    }

    /// A valid default value (used for declared-but-unassigned variables and empty payloads).
    pub(crate) fn default_value(&mut self, t: TyId) -> String {
        match self.tget(t) {
            Ty::Str => "TV_EMPTY_STR".into(),
            Ty::BigInt => "((tv_big){1})".into(),
            Ty::Decimal => "((tv_dec){0, 0})".into(),
            Ty::Array(_) => "TV_EMPTY_ARR".into(),
            Ty::Map(..) | Ty::Set(_) => "TV_EMPTY_MAP".into(),
            Ty::Func(..) => "((tv_fn){NULL, NULL})".into(),
            Ty::Promise(..) | Ty::Gen(_) | Ty::AsyncGen(_) => "NULL".into(),
            Ty::Bool => "false".into(),
            Ty::Unknown => "((tv_dyn){&tv_type_undefined, NULL})".into(),
            Ty::Class(..) => "NULL".into(),
            Ty::Interface(..) => "((tv_iface){NULL, NULL})".into(),
            Ty::Union(_) if self.niche(t).is_some() => self.niche_none(t).into(),
            Ty::Record(_) | Ty::Union(_) | Ty::Rec(..) => {
                self.ensure_helper(t, H_DEFAULT);
                let n = self.box_name(t);
                format!("df_{n}()")
            }
            _ => "0".into(),
        }
    }

    // ------------------------------------------------------------ per-type helpers

    /// Emits (once) the helper of the given kind for `t`.
    fn ensure_helper(&mut self, t: TyId, kind: u8) {
        if !self.helpers_done.insert((t, kind)) {
            return;
        }
        let ct = self.ctype(t);
        let n = self.box_name(t);
        let mut h = String::new();
        match kind {
            H_RC => {
                // Retains inside rt_ helpers happen only when their type is retained (see `shared_boxes`).
                self.rc_helper_depth += 1;
                let _ = writeln!(self.protos, "static void rt_{n}(void *p);\nstatic void rl_{n}(void *p);");
                let (mut rt, mut rl) = (String::new(), String::new());
                match self.tget(t) {
                    Ty::Record(fs) if self.is_obj(t) => {
                        // An object: counted; its fields are released with it.
                        let fs = self.c.types.fields(fs).to_vec();
                        let mut rel = String::new();
                        for f in fs {
                            if self.is_rc(f.ty) {
                                let _ = writeln!(rel, "    {};", self.release_code(f.ty, &format!("b->v.f_{}", self.sym(f.name))));
                            }
                        }
                        let _ = writeln!(self.protos, "static void fo_{n}({n} *b);");
                        let _ = writeln!(self.helpers_after_decl, "static void fo_{n}({n} *b) {{\n{rel}    tvg_free_small(b, sizeof(*b));\n}}");
                        let _ = writeln!(rt, "    if (*x) (*x)->rc++;");
                        let _ = writeln!(rl, "    if (*x && --(*x)->rc == 0) fo_{n}(*x);");
                    }
                    Ty::Record(fs) => {
                        let fs = self.c.types.fields(fs).to_vec();
                        for f in fs {
                            if self.is_rc(f.ty) {
                                let fname = format!("f_{}", self.sym(f.name));
                                let _ = writeln!(rt, "    {};", self.retain_code(f.ty, &format!("x->{fname}")));
                                let _ = writeln!(rl, "    {};", self.release_code(f.ty, &format!("x->{fname}")));
                            }
                        }
                    }
                    Ty::Union(ms) => {
                        let ms = self.c.types.tys(ms).to_vec();
                        rt.push_str("    switch (x->tag) {\n");
                        rl.push_str("    switch (x->tag) {\n");
                        for (i, &m) in ms.iter().enumerate() {
                            if self.is_rc(m) {
                                let _ = writeln!(rt, "    case {i}: {}; break;", self.retain_code(m, &format!("x->u.m{i}")));
                                let _ = writeln!(rl, "    case {i}: {}; break;", self.release_code(m, &format!("x->u.m{i}")));
                            }
                        }
                        rt.push_str("    default: break;\n    }\n");
                        rl.push_str("    default: break;\n    }\n");
                    }
                    Ty::Rec(..) => {
                        let inner = self.c.unfold(t);
                        let _ = writeln!(rt, "    if (*x) RC_{n}(*x)++;");
                        if self.is_rc(inner) {
                            // Freeing a box can recurse (long lists): bounded depth, see tvg_drop_enter.
                            let rel_inner = format!("{};", self.release_code(inner, "b->v"));
                            let _ = writeln!(self.protos, "static void fb_{n}(void *p);");
                            let _ = writeln!(self.helpers_after_decl, "static void fb_{n}(void *p) {{\n    {ct} b = p;\n    {rel_inner}\n    tvg_free_small(b, sizeof(*b));\n}}");
                            let _ = writeln!(rl, "    if (*x && --RC_{n}(*x) == 0) fb_{n}(*x);");
                        } else {
                            let _ = writeln!(rl, "    if (*x && --RC_{n}(*x) == 0) tvg_free_small(*x, sizeof(**x));");
                        }
                    }
                    _ => {}
                }
                self.rc_helper_depth -= 1;
                let _ = writeln!(h, "static void rt_{n}(void *p) {{\n    {ct} *x = p; (void)x;\n{rt}}}");
                let _ = writeln!(h, "static void rl_{n}(void *p) {{\n    {ct} *x = p; (void)x;\n{rl}}}");
            }
            H_EQ => {
                let _ = writeln!(self.protos, "static bool eq_{n}(const void *a, const void *b);");
                let mut b = String::new();
                match self.tget(t) {
                    // (objects: the same one, as `===` in JavaScript)
                    Ty::Record(_) if self.is_obj(t) => b.push_str("    return *x == *y;\n"),
                    Ty::Record(fs) => {
                        let fs = self.c.types.fields(fs).to_vec();
                        for f in fs {
                            let fname = format!("f_{}", self.sym(f.name));
                            let e = self.eq_code(f.ty, &format!("x->{fname}"), &format!("y->{fname}"));
                            let _ = writeln!(b, "    if (!({e})) return false;");
                        }
                        b.push_str("    return true;\n");
                    }
                    Ty::Union(ms) => {
                        let ms = self.c.types.tys(ms).to_vec();
                        b.push_str("    if (x->tag != y->tag) return false;\n    switch (x->tag) {\n");
                        for (i, &m) in ms.iter().enumerate() {
                            if !self.is_unit(m) {
                                let e = self.eq_code(m, &format!("x->u.m{i}"), &format!("y->u.m{i}"));
                                let _ = writeln!(b, "    case {i}: return {e};");
                            }
                        }
                        b.push_str("    default: return true;\n    }\n");
                    }
                    Ty::Rec(..) => {
                        let inner = self.c.unfold(t);
                        let e = self.eq_code(inner, "(*x)->v", "(*y)->v");
                        let _ = writeln!(b, "    return *x == *y || ({e});");
                    }
                    _ => b.push_str("    return true;\n"),
                }
                let _ = writeln!(h, "static bool eq_{n}(const void *a, const void *b) {{\n    const {ct} *x = a; const {ct} *y = b; (void)x; (void)y;\n{b}}}");
            }
            H_HASH => {
                let _ = writeln!(self.protos, "static uint64_t hs_{n}(const void *p);");
                let mut b = String::from("    uint64_t h = 0x9E3779B97F4A7C15ull;\n");
                match self.tget(t) {
                    Ty::Record(_) if self.is_obj(t) => b.push_str("    h = (uint64_t)(uintptr_t)*x * h;\n"),
                    Ty::Record(fs) => {
                        let fs = self.c.types.fields(fs).to_vec();
                        for f in fs {
                            let fname = format!("f_{}", self.sym(f.name));
                            let e = self.hash_code(f.ty, &format!("x->{fname}"));
                            let _ = writeln!(b, "    h = (h ^ ({e})) * 0x100000001B3ull;");
                        }
                    }
                    Ty::Union(ms) => {
                        let ms = self.c.types.tys(ms).to_vec();
                        b.push_str("    h ^= x->tag;\n    switch (x->tag) {\n");
                        for (i, &m) in ms.iter().enumerate() {
                            if !self.is_unit(m) {
                                let e = self.hash_code(m, &format!("x->u.m{i}"));
                                let _ = writeln!(b, "    case {i}: h = (h ^ ({e})) * 0x100000001B3ull; break;");
                            }
                        }
                        b.push_str("    default: break;\n    }\n");
                    }
                    Ty::Rec(..) => {
                        let inner = self.c.unfold(t);
                        let e = self.hash_code(inner, "(*x)->v");
                        let _ = writeln!(b, "    h ^= {e};");
                    }
                    _ => {}
                }
                b.push_str("    return h;\n");
                let _ = writeln!(h, "static uint64_t hs_{n}(const void *p) {{\n    const {ct} *x = p; (void)x;\n{b}}}");
            }
            H_STR | H_INSPECT => {
                let inspect = kind == H_INSPECT;
                let (fname, sig) = if inspect { (format!("in_{n}"), "tv_sb *sb, const void *p, int depth") } else { (format!("ts_{n}"), "tv_sb *sb, const void *p") };
                let _ = writeln!(self.protos, "static void {fname}({sig});");
                let mut b = String::new();
                match self.tget(t) {
                    Ty::Record(_) if self.tuple_of(t).is_some() => {
                        let fs = self.tuple_of(t).unwrap();
                        let descs: Vec<String> = fs.iter().map(|f| self.desc(f.ty)).collect();
                        let ptrs: Vec<String> = fs.iter().map(|f| format!("&x->f_{}", self.sym(f.name))).collect();
                        if inspect {
                            let _ = writeln!(b, "    const tv_type *const types[] = {{ {} }};", descs.join(", "));
                            let _ = writeln!(b, "    const void *const fields[] = {{ {} }};", ptrs.join(", "));
                            let _ = writeln!(b, "    tv_inspect_tuple(sb, depth, {}, types, fields);", fs.len());
                        } else {
                            // String([a, b]) is "a,b"
                            for (i, f) in fs.iter().enumerate() {
                                if i > 0 {
                                    b.push_str("    tv_sb_push_char(sb, ',');\n");
                                }
                                let e = self.string_code(f.ty, &format!("x->f_{}", self.sym(f.name)));
                                let _ = writeln!(b, "    {e};");
                            }
                        }
                    }
                    Ty::Record(_) => {
                        if !inspect {
                            b.push_str("    tv_sb_push_cstr(sb, \"[object Object]\");\n");
                        } else {
                            // Node's layout (single- or multi-line) is decided by the runtime.
                            let fs = self.c.types.fields_in_order(t);
                            let names: Vec<String> = fs.iter().map(|f| c_string(self.sym(f.name).as_bytes())).collect();
                            let descs: Vec<String> = fs.iter().map(|f| self.desc(f.ty)).collect();
                            let ptrs: Vec<String> = fs.iter().map(|f| format!("&x->f_{}", self.sym(f.name))).collect();
                            if fs.is_empty() {
                                b.push_str("    tv_inspect_record(sb, depth, 0, NULL, NULL, NULL);\n");
                            } else if fs.iter().any(|f| f.optional) {
                                // (an optional field that's absent isn't shown, as JavaScript has no key for it)
                                let n = fs.len();
                                // (a record's index-signature keys, its `$rest` map: shown as its own)
                                let rest = fs.iter().find(|f| self.sym(f.name) == "$rest").map(|f| f.ty);
                                let mut rest_add = String::new();
                                let size = match rest {
                                    Some(rt) => {
                                        let mt = self.c.types.without_undefined(rt);
                                        let place = "x->f_$rest".to_string();
                                        let (present, ml) = match (self.tag_of(rt, UNDEFINED), self.tag_of(rt, mt)) {
                                            (Some(u), Some(k)) if mt != rt => (format!("!({})", self.u_is(&place, rt, u)), self.u_payload(&place, rt, k)),
                                            _ => ("true".to_string(), place.clone()),
                                        };
                                        if let Ty::Map(k, v) = self.tget(mt) {
                                            let (kd, vd) = (self.desc(k), self.desc(v));
                                            let _ = writeln!(b, "    tv_int rn = {present} ? tv_map_size({ml}) : 0;");
                                            rest_add = format!("    if ({present}) {{ tv_int ri = 0; void *kp, *vp; while (tv_map_next({ml}, {kd}, {vd}, &ri, &kp, &vp)) {{ names[n] = ((tv_str *)kp)->p->data; types[n] = {vd}; fields[n] = vp; n++; }} }}\n");
                                            format!("{n} + rn")
                                        } else {
                                            n.to_string()
                                        }
                                    }
                                    None => n.to_string(),
                                };
                                let _ = writeln!(b, "    const char *names[{size}]; const tv_type *types[{size}]; const void *fields[{size}]; int n = 0;");
                                for (i, f) in fs.iter().enumerate() {
                                    if Some(f.ty) == rest && self.sym(f.name) == "$rest" {
                                        continue;
                                    }
                                    let place = format!("x->f_{}", self.sym(f.name));
                                    let add = format!("names[n] = {}; types[n] = {}; fields[n] = {}; n++;", names[i], descs[i], ptrs[i]);
                                    match self.tag_of(f.ty, UNDEFINED).filter(|_| f.optional) {
                                        Some(k) => {
                                            let absent = self.u_is(&place, f.ty, k);
                                            let _ = writeln!(b, "    if (!({absent})) {{ {add} }}");
                                        }
                                        None => {
                                            let _ = writeln!(b, "    {add}");
                                        }
                                    }
                                }
                                b.push_str(&rest_add);
                                b.push_str("    tv_inspect_record(sb, depth, n, names, types, fields);\n");
                            } else {
                                let _ = writeln!(b, "    static const char *const names[] = {{ {} }};", names.join(", "));
                                let _ = writeln!(b, "    const tv_type *const types[] = {{ {} }};", descs.join(", "));
                                let _ = writeln!(b, "    const void *const fields[] = {{ {} }};", ptrs.join(", "));
                                let _ = writeln!(b, "    tv_inspect_record(sb, depth, {}, names, types, fields);", fs.len());
                            }
                        }
                    }
                    Ty::Union(ms) => {
                        let ms = self.c.types.tys(ms).to_vec();
                        b.push_str("    switch (x->tag) {\n");
                        for (i, &m) in ms.iter().enumerate() {
                            let place = format!("x->u.m{i}");
                            let e = if inspect { self.inspect_code(m, &place, "depth") } else { self.string_code(m, &place) };
                            let _ = writeln!(b, "    case {i}: {e}; break;");
                        }
                        b.push_str("    default: break;\n    }\n");
                    }
                    Ty::Rec(..) => {
                        let inner = self.c.unfold(t);
                        let e = if inspect { self.inspect_code(inner, "(*x)->v", "depth") } else { self.string_code(inner, "(*x)->v") };
                        let _ = writeln!(b, "    {e};");
                    }
                    _ => {}
                }
                // (an object's fields are formatted from inside its box)
                let decl = if self.is_obj(t) { format!("const {} *x = &(*(const {ct} *)p)->v;", self.rec_struct(t)) } else { format!("const {ct} *x = p;") };
                let _ = writeln!(h, "static void {fname}({sig}) {{\n    {decl} (void)x; (void)sb;{}\n{b}}}", if inspect { " (void)depth;" } else { "" });
            }
            H_DEFAULT => {
                let _ = writeln!(self.protos, "static {ct} df_{n}(void);");
                let v = match self.tget(t) {
                    Ty::Record(fs) => {
                        let fs = self.c.types.fields(fs).to_vec();
                        let parts: Vec<String> = fs.iter().map(|f| {
                            let d = self.default_value(f.ty);
                            format!(".f_{} = {d}", self.sym(f.name))
                        }).collect();
                        self.rec_lit(t, &parts.join(", "))
                    }
                    Ty::Union(ms) => {
                        let ms = self.c.types.tys(ms).to_vec();
                        // Prefer the `undefined` member, else the first.
                        let i = ms.iter().position(|&m| m == UNDEFINED).unwrap_or(0);
                        if self.is_unit(ms[i]) {
                            format!("({ct}){{ .tag = {i} }}")
                        } else {
                            let d = self.default_value(ms[i]);
                            format!("({ct}){{ .tag = {i}, .u.m{i} = {d} }}")
                        }
                    }
                    Ty::Rec(..) => {
                        let inner = self.c.unfold(t);
                        let d = self.default_value(inner);
                        let bn = self.box_name(t);
                        format!("({{ {bn} *b = tvg_alloc_small(sizeof({bn})); RC_{bn}(b) = 1; b->v = {d}; b; }})")
                    }
                    _ => "0".into(),
                };
                let _ = writeln!(h, "static {ct} df_{n}(void) {{ return {v}; }}");
            }
            _ => {}
        }
        self.helpers.push_str(&h);
    }

    pub(crate) fn retain_code(&mut self, t: TyId, place: &str) -> String {
        if self.rc_helper_depth == 0 {
            self.rc_roots.insert(t);
        }
        match self.tget(t) {
            Ty::Str => format!("tv_str_retain({place})"),
            Ty::BigInt => format!("tv_big_retain({place})"),
            Ty::Array(_) => format!("tv_arr_retain({place})"),
            Ty::Map(..) | Ty::Set(_) => format!("tv_map_retain({place})"),
            Ty::Func(..) => format!("tv_env_retain(({place}).env)"),
            Ty::Promise(..) => format!("tv_promise_retain({place})"),
            Ty::Gen(_) | Ty::AsyncGen(_) => format!("tv_gen_retain({place})"),
            Ty::Js => format!("tv_js_retain({place})"),
            Ty::Unknown => format!("tv_dyn_retain({place})"),
            Ty::Class(..) => format!("tvg_obj_retain({place})"),
            Ty::Interface(..) => format!("tv_iface_retain({place})"),
            Ty::Union(_) if self.niche_rec(t).is_some() => {
                let m = self.niche_rec(t).unwrap();
                let inner = self.retain_code(m, place);
                let there = self.niche_there(place, t);
                format!("({{ if ({there}) {inner}; }})")
            }
            Ty::Union(_) if self.niche(t).is_some() => format!("tvg_obj_retain({place})"),
            _ if self.is_rc(t) => {
                self.ensure_helper(t, H_RC);
                format!("rt_{}(&{place})", self.box_name(t))
            }
            _ => "(void)0".into(),
        }
    }

    /// The function freeing an array of type `t` (elements of type `e`, which hold references)
    /// once its count is 0: each element released by code of its own type, not through the
    /// descriptor's function pointer, so a nest of arrays is freed by direct calls.
    fn arr_free_fn(&mut self, t: TyId, e: TyId) -> String {
        let name = format!("fa{}", t.0);
        if self.arr_frees.insert(t) {
            let ect = self.ctype(e);
            let _ = writeln!(self.protos, "static void {name}(tv_arrbuf *p);");
            let rel = self.release_code(e, "d[i]");
            // (a word of zeros, a hole: nothing to release, as the runtime's release does)
            let rel = format!("if (sizeof({ect}) != sizeof(void *) || *(void **)(void *)&d[i]) {rel};");
            let _ = writeln!(self.helpers, "static void {name}(tv_arrbuf *p) {{\n    {ect} *d = ({ect} *)(void *)tv_arrbuf_data(p);\n    for (int64_t i = 0, n = p->len; i < n; i++) {{ {rel} }}\n    tv_arrbuf_free(p, sizeof({ect}));\n}}");
        }
        name
    }

    pub(crate) fn release_code(&mut self, t: TyId, place: &str) -> String {
        match self.tget(t) {
            Ty::Str => format!("tv_str_release({place})"),
            Ty::BigInt => format!("tv_big_release({place})"),
            Ty::Array(e) if self.is_rc(e) => {
                let f = self.arr_free_fn(t, e);
                format!("tv_arr_release_with({place}, {f})")
            }
            // (numbers and booleans: freed inline)
            Ty::Array(e) if matches!(self.tget(e), Ty::Int | Ty::F64 | Ty::F32 | Ty::Bool | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64) => {
                let ect = self.ctype(e);
                format!("tv_arr_release_plain({place}, sizeof({ect}))")
            }
            Ty::Array(e) => {
                let d = self.desc(e);
                format!("tv_arr_release({place}, {d})")
            }
            Ty::Map(k, v) => {
                let (kd, vd) = (self.desc(k), self.desc(v));
                format!("tv_map_release({place}, {kd}, {vd})")
            }
            Ty::Set(k) => {
                let kd = self.desc(k);
                format!("tv_map_release({place}, {kd}, &tv_type_undefined)")
            }
            Ty::Func(..) => format!("tv_env_release(({place}).env)"),
            Ty::Promise(..) => format!("tv_promise_release({place})"),
            Ty::Gen(_) | Ty::AsyncGen(_) => format!("tv_gen_release({place})"),
            Ty::Js => format!("tv_js_release({place})"),
            Ty::Unknown => format!("tv_dyn_release({place})"),
            Ty::Class(..) => self.class_release_code(t, place),
            Ty::Interface(..) => format!("tv_iface_release({place})"),
            Ty::Union(_) if self.niche_rec(t).is_some() => {
                let m = self.niche_rec(t).unwrap();
                let inner = self.release_code(m, place);
                let there = self.niche_there(place, t);
                format!("({{ if ({there}) {inner}; }})")
            }
            Ty::Union(ms) if self.niche(t).is_some() => {
                let (pi, _) = self.niche(t).unwrap();
                let m = self.c.types.tys(ms)[pi];
                self.class_release_code(m, place)
            }
            _ if self.is_rc(t) => {
                self.ensure_helper(t, H_RC);
                format!("rl_{}(&{place})", self.box_name(t))
            }
            _ => "(void)0".into(),
        }
    }

    pub(crate) fn eq_code(&mut self, t: TyId, a: &str, b: &str) -> String {
        match self.tget(t) {
            Ty::Str => format!("tv_str_eq({a}, {b})"),
            Ty::BigInt => format!("tv_big_eq({a}, {b})"),
            Ty::Decimal => format!("tv_dec_eq({a}, {b})"),
            Ty::StrLit(_) | Ty::Undefined | Ty::Null | Ty::Void | Ty::Never => "true".into(),
            Ty::Array(_) | Ty::Map(..) | Ty::Set(_) => format!("(({a}).p == ({b}).p)"),
            Ty::Func(..) => format!("tvg_fn_eq({a}, {b})"),
            Ty::Unknown => format!("tv_dyn_eq({a}, {b})"),
            Ty::Class(..) | Ty::Promise(..) | Ty::Gen(_) | Ty::AsyncGen(_) => format!("((void *)({a}) == (void *)({b}))"),
            Ty::Interface(..) => format!("(({a}).p == ({b}).p)"),
            Ty::Js => format!("tv_type_js.eq(&({a}), &({b}))"),
            Ty::Union(_) if self.niche_str(t) => format!("((({a}).p == NULL || ({b}).p == NULL) ? (({a}).p == ({b}).p) : tv_str_eq({a}, {b}))"),
            Ty::Union(_) if self.niche_arr(t) => format!("(({a}).p == ({b}).p)"),
            Ty::Union(_) if self.niche_rec(t).is_some() => {
                // values: equal when both are undefined, or both hold equal trees
                let m = self.niche_rec(t).unwrap();
                let inner = self.eq_code(m, a, b);
                let (ta, tb) = (self.niche_there(a, t), self.niche_there(b, t));
                if self.niche_arr(t) {
                    format!("((!{ta} || !{tb}) ? (({a}).p == ({b}).p) : {inner})")
                } else {
                    format!("((!{ta} || !{tb}) ? (({a}) == ({b})) : {inner})")
                }
            }
            Ty::Union(_) if self.niche(t).is_some() => format!("((void *)({a}) == (void *)({b}))"),
            Ty::Record(_) if self.is_obj(t) => format!("(({a}) == ({b}))"),
            Ty::Record(_) | Ty::Union(_) | Ty::Rec(..) => {
                self.ensure_helper(t, H_EQ);
                format!("eq_{}(&({a}), &({b}))", self.box_name(t))
            }
            _ => format!("(({a}) == ({b}))"),
        }
    }

    fn hash_code(&mut self, t: TyId, p: &str) -> String {
        let d = self.desc(t);
        format!("({d})->hash(&({p}))")
    }

    /// Appends `String(x)` of the value at `place` to `sb`.
    pub(crate) fn string_code(&mut self, t: TyId, place: &str) -> String {
        match self.tget(t) {
            Ty::Str => format!("tv_sb_push_str(sb, {place})"),
            Ty::StrLit(s) => {
                // (its length known here: no strlen, and a NUL inside it is kept)
                let text = self.sym(s).to_string();
                format!("tv_sb_push(sb, {}, {})", c_string(text.as_bytes()), text.len())
            }
            Ty::Int if self.bodies.last().is_some_and(|b| self.js_ints(b.m)) => format!("tv_sb_push_jint(sb, {place})"),
            Ty::Int => format!("tv_sb_push_int(sb, {place})"),
            Ty::F64 | Ty::F32 => format!("tv_sb_push_f64(sb, {place})"),
            Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 => format!("tv_sb_push_int(sb, (tv_int)({place}))"),
            Ty::U64 => format!("tv_sb_push_f64(sb, (double)({place}))"),
            Ty::BigInt => format!("tv_sb_push_big(sb, {place})"),
            Ty::Decimal => format!("tv_sb_push_dec(sb, {place})"),
            Ty::Bool => format!("tv_sb_push_cstr(sb, ({place}) ? \"true\" : \"false\")"),
            Ty::Undefined | Ty::Void | Ty::Never => "tv_sb_push_cstr(sb, \"undefined\")".into(),
            Ty::Null => "tv_sb_push_cstr(sb, \"null\")".into(),
            // (`String([1, null, undefined])` is "1,,": an empty element, as `join`'s)
            Ty::Array(e)
                if matches!(self.tget(e), Ty::Union(_))
                    && (self.c.types.has_undefined(e) || self.members(e).contains(&NULL))
                    && self.members(e).iter().all(|&m| matches!(self.tget(m), Ty::Undefined | Ty::Null | Ty::Str | Ty::StrLit(_) | Ty::Bool | Ty::BoolLit(_)) || self.c.types.is_numeric(m)) =>
            {
                let ect = self.ctype(e);
                let (a, i, x) = (self.fresh("sa"), self.fresh("si"), self.fresh("sx"));
                let xv = format!("(*{x})");
                let absent = match self.niche(e) {
                    Some(_) => format!("!({})", self.niche_there(&xv, e)),
                    None => {
                        let mut parts = Vec::new();
                        for m in [UNDEFINED, NULL] {
                            if let Some(k) = self.tag_of(e, m) {
                                parts.push(self.u_is(&xv, e, k));
                            }
                        }
                        format!("({})", parts.join(" || "))
                    }
                };
                let inner = self.string_code(e, &xv);
                format!("({{ tv_arr {a} = {place}; for (tv_int {i} = 0; {i} < tv_arr_len({a}); {i}++) {{ if ({i}) tv_sb_push_cstr(sb, \",\"); {ect} *{x} = &(({ect} *)tv_arr_data({a}))[{i}]; if (!{absent}) {inner}; }} }})")
            }
            Ty::Array(e) => {
                let d = self.desc(e);
                format!("tv_to_str_arr(sb, {place}, {d})")
            }
            Ty::Func(..) => "tv_sb_push_cstr(sb, \"[Function]\")".into(),
            Ty::Class(..) => self.class_string_code(t, place),
            Ty::Interface(..) => format!("(({place}).t ? ({place}).t->to_str(sb, ({place}).p) : tv_sb_push_cstr(sb, \"undefined\"))"),
            Ty::Union(ms) if self.niche(t).is_some() => {
                let (pi, _) = self.niche(t).unwrap();
                let m = self.c.types.tys(ms)[pi];
                let inner = if self.niche_rec(t).is_some() { self.string_code(m, place) } else { self.class_string_code(m, place) };
                let absent = self.niche_absent(t);
                let there = self.niche_there(place, t);
                format!("({{ if ({there}) {inner}; else tv_sb_push_cstr(sb, \"{absent}\"); }})")
            }
            // (as JavaScript's `String(m)`; a `Record` made as an object is one)
            Ty::Map(..) => format!("tv_sb_push_cstr(sb, tv_map_is_object({place}) ? \"[object Object]\" : \"[object Map]\")"),
            Ty::Set(_) => "tv_sb_push_cstr(sb, \"[object Set]\")".into(),
            Ty::Promise(..) => "tv_sb_push_cstr(sb, \"[object Promise]\")".into(),
            Ty::Gen(_) | Ty::AsyncGen(_) => "tv_sb_push_cstr(sb, \"[object Generator]\")".into(),
            Ty::Js => format!("tv_type_js.to_str(sb, &({place}))"),
            Ty::Unknown => format!("tv_type_dyn.to_str(sb, &({place}))"),
            _ => {
                self.ensure_helper(t, H_STR);
                format!("ts_{}(sb, &({place}))", self.box_name(t))
            }
        }
    }

    /// Appends console.log formatting of the value at `place` to `sb`.
    /// `inspect_code` for a value the program itself formats (console.log, test messages): the
    /// first one that goes through descriptors links the formatter in (see `TVG_INS`).
    pub(crate) fn inspect_root(&mut self, t: TyId, place: &str, depth: &str) -> String {
        let code = self.inspect_code(t, place, depth);
        if !matches!(self.tget(t), Ty::Str | Ty::StrLit(_) | Ty::Func(..) | Ty::BigInt | Ty::Decimal | Ty::Int | Ty::F64 | Ty::F32 | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64 | Ty::Bool | Ty::Undefined | Ty::Null | Ty::Void | Ty::Never) {
            self.uses_inspect = true;
        }
        code
    }

    pub(crate) fn inspect_code(&mut self, t: TyId, place: &str, depth: &str) -> String {
        match self.tget(t) {
            Ty::Str if depth == "0" => format!("tv_sb_push_str(sb, {place})"),
            Ty::Str => format!("tv_inspect_str(sb, {place}, {depth})"),
            Ty::StrLit(s) => {
                let lit = self.lit(self.sym(s).to_string().as_bytes());
                format!("tv_inspect_str(sb, {lit}, {depth})")
            }
            // (console.log writes negative zero as -0; String(x) as 0)
            Ty::F64 | Ty::F32 => format!("({{ double v_ = (double)({place}); if (v_ == 0 && signbit(v_)) tv_sb_push_cstr(sb, \"-0\"); else tv_sb_push_f64(sb, v_); }})"),
            Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64 | Ty::Bool | Ty::Undefined | Ty::Null | Ty::Void | Ty::Never => {
                self.string_code(t, place)
            }
            Ty::BigInt => format!("({{ tv_sb_push_big(sb, {place}); tv_sb_push_char(sb, 'n'); }})"),
            Ty::Decimal => format!("tv_sb_push_dec(sb, {place})"),
            Ty::Promise(..) => format!("tv_inspect_value(sb, &tv_type_promise, &({place}), {depth})"),
            Ty::Gen(_) | Ty::AsyncGen(_) => format!("tv_inspect_value(sb, &tv_type_gen, &({place}), {depth})"),
            Ty::Array(e) => {
                let d = self.desc(e);
                // (in TypeScript, an array of sized numbers is a typed array: `Int32Array(2) [ 1, 2 ]`)
                let name = match self.tget(e) {
                    Ty::I8 => "Int8Array", Ty::U8 => "Uint8Array", Ty::U8C => "Uint8ClampedArray", Ty::I16 => "Int16Array",
                    Ty::U16 => "Uint16Array", Ty::I32 => "Int32Array", Ty::U32 => "Uint32Array", Ty::F32 => "Float32Array",
                    _ => "",
                };
                // (a descriptor's formatter is made outside any function: the program's own modules say)
                let ts = match self.bodies.last() {
                    Some(b) => self.js_ints(b.m),
                    None => (0..self.c.modules.len() as u32).any(|m| !self.c.modules[m as usize].builtin && !self.c.modules[m as usize].std && self.js_ints(m)),
                };
                if !name.is_empty() && ts {
                    format!("tv_inspect_typed_arr(sb, {place}, {d}, {depth}, \"{name}\")")
                } else {
                    format!("tv_inspect_arr(sb, {place}, {d}, {depth})")
                }
            }
            Ty::Map(k, v) => {
                let (kd, vd) = (self.desc(k), self.desc(v));
                format!("tv_map_inspect(sb, {place}, {kd}, {vd}, {depth})")
            }
            Ty::Set(k) => {
                let kd = self.desc(k);
                format!("tv_set_inspect(sb, {place}, {kd}, {depth})")
            }
            Ty::Func(..) => "tv_sb_push_cstr(sb, \"[Function (anonymous)]\")".into(),
            Ty::Js => format!("tv_type_js.inspect(sb, &({place}), {depth})"),
            Ty::Unknown => format!("tv_type_dyn.inspect(sb, &({place}), {depth})"),
            Ty::Class(..) => format!("tvg_obj_inspect(sb, (void *)({place}), {depth})"),
            Ty::Interface(..) => format!("(({place}).t ? ({place}).t->inspect(sb, ({place}).p, {depth}) : tv_sb_push_cstr(sb, \"undefined\"))"),
            Ty::Union(_) if self.niche_rec(t).is_some() => {
                let m = self.niche_rec(t).unwrap();
                let inner = self.inspect_code(m, place, depth);
                let absent = self.niche_absent(t);
                let there = self.niche_there(place, t);
                format!("({{ if ({there}) {inner}; else tv_sb_push_cstr(sb, \"{absent}\"); }})")
            }
            Ty::Union(_) if self.niche(t).is_some() => format!("({{ if (({place}) != NULL) tvg_obj_inspect(sb, (void *)({place}), {depth}); else tv_sb_push_cstr(sb, \"{}\"); }})", self.niche_absent(t)),
            _ => {
                self.ensure_helper(t, H_INSPECT);
                format!("in_{}(sb, &({place}), {depth})", self.box_name(t))
            }
        }
    }

    /// A `const tv_type *` expression describing `t` (for containers and printing).
    pub(crate) fn desc(&mut self, t: TyId) -> String {
        // Containers retain through descriptors.
        if self.rc_helper_depth == 0 {
            self.rc_roots.insert(t);
        }
        match self.tget(t) {
            Ty::Int => return "&tv_type_int".into(),
            Ty::F64 => return "&tv_type_f64".into(),
            Ty::F32 => return "&tv_type_f32".into(),
            Ty::Bool => return "&tv_type_bool".into(),
            Ty::Str => return "&tv_type_str".into(),
            Ty::BigInt => return "&tv_type_big".into(),
            Ty::Decimal => return "&tv_type_dec".into(),
            Ty::I8 => return "&tv_type_i8".into(),
            Ty::I16 => return "&tv_type_i16".into(),
            Ty::I32 => return "&tv_type_i32".into(),
            Ty::U8 | Ty::U8C => return "&tv_type_u8".into(),
            Ty::U16 => return "&tv_type_u16".into(),
            Ty::U32 => return "&tv_type_u32".into(),
            Ty::U64 => return "&tv_type_u64".into(),
            Ty::Undefined | Ty::Void | Ty::Never | Ty::Error => return "&tv_type_undefined".into(),
            Ty::Unknown => return "&tv_type_dyn".into(),
            Ty::Null => return "&tv_type_null".into(),
            Ty::Promise(..) => return "&tv_type_promise".into(),
            Ty::Gen(_) | Ty::AsyncGen(_) => return "&tv_type_gen".into(),
            Ty::Js => return "&tv_type_js".into(),
            _ => {}
        }
        let key = self.desc_key(t);
        if self.descs.insert(t) {
            let ct = self.ctype(t);
            let _ = writeln!(self.protos, "static const tv_type {key};");
            // Descriptor wrappers with the runtime's void* signatures.
            let rc = self.is_rc(t);
            let (rt, rl) = if rc {
                let r = self.retain_code(t, &format!("*({ct} *)p"));
                let l = self.release_code(t, &format!("*({ct} *)p"));
                // (an object's retain is the runtime's own, so copies of arrays of them retain
                // inline: a count first, 32 bits for a class's instance, 64 for a boxed record)
                let shared = match self.tget(t) {
                    Ty::Class(..) => Some("tv_retain_rc32"),
                    Ty::Union(_) if self.niche_rec(t).is_none() && self.niche(t).is_some() => Some("tv_retain_rc32"),
                    Ty::Record(_) if self.is_obj(t) => Some("tv_retain_rc64"),
                    _ => None,
                };
                match shared {
                    Some(f) => {
                        let _ = writeln!(self.helpers, "static void {key}_rl(void *p) {{ {l}; }}");
                        let _ = r;
                        (f.to_string(), format!("{key}_rl"))
                    }
                    None => {
                        let _ = writeln!(self.helpers, "static void {key}_rt(void *p) {{ {r}; }}\nstatic void {key}_rl(void *p) {{ {l}; }}");
                        (format!("{key}_rt"), format!("{key}_rl"))
                    }
                }
            } else {
                ("NULL".into(), "NULL".into())
            };
            let eq = self.eq_code(t, &format!("*(const {ct} *)a"), &format!("*(const {ct} *)b"));
            let hash = match self.tget(t) {
                Ty::Class(..) => "((uint64_t)(uintptr_t)(*(void *const *)p) * 0x9E3779B97F4A7C15ull)".into(),
                Ty::Interface(..) => "((uint64_t)(uintptr_t)(((const tv_iface *)p)->p) * 0x9E3779B97F4A7C15ull)".into(),
                // (an array or nothing: by which array, as `===` compares arrays)
                Ty::Union(_) if self.niche_arr(t) => "((uint64_t)(uintptr_t)(*(void *const *)p) * 0x9E3779B97F4A7C15ull)".into(),
                // (a string or nothing: by its text)
                Ty::Union(_) if self.niche_str(t) => "(*(void *const *)p == NULL ? 0x9E3779B97F4A7C15ull : tv_str_hash(*(const tv_str *)p))".into(),
                Ty::Union(_) if self.niche_rec(t).is_some() => {
                    let m = self.niche_rec(t).unwrap();
                    self.ensure_helper(m, H_HASH);
                    format!("(*(void *const *)p == NULL ? 0x9E3779B97F4A7C15ull : hs_{}(p))", self.box_name(m))
                }
                Ty::Union(_) if self.niche(t).is_some() => "((uint64_t)(uintptr_t)(*(void *const *)p) * 0x9E3779B97F4A7C15ull)".into(),
                Ty::Record(_) | Ty::Union(_) | Ty::Rec(..) => {
                    self.ensure_helper(t, H_HASH);
                    format!("hs_{}(p)", self.box_name(t))
                }
                Ty::StrLit(_) => "0".into(),
                _ => "0".into(),
            };
            let ts = self.string_code(t, &format!("*(const {ct} *)p"));
            let ins = self.inspect_code(t, &format!("*(const {ct} *)p"), "depth");
            let extra = match self.tget(t) {
                // (arrays are objects: equal when they're the same one, as `===` in JavaScript)
                Ty::Array(_) => format!("static uint64_t {key}_hs2(const void *p) {{ return (uint64_t)(uintptr_t)((const tv_arr *)p)->p * 0x9E3779B97F4A7C15ull; }}\nstatic bool {key}_eq2(const void *x, const void *y) {{ return ((const tv_arr *)x)->p == ((const tv_arr *)y)->p; }}\n"),
                // (maps and sets too: the same one)
                Ty::Map(..) | Ty::Set(_) => format!("static uint64_t {key}_hs2(const void *p) {{ return (uint64_t)(uintptr_t)((const tv_map *)p)->p * 0x9E3779B97F4A7C15ull; }}\nstatic bool {key}_eq2(const void *x, const void *y) {{ return ((const tv_map *)x)->p == ((const tv_map *)y)->p; }}\n"),
                _ => String::new(),
            };
            let (eq_fn, hash_fn) = if extra.is_empty() {
                let _ = writeln!(self.helpers, "static bool {key}_eq(const void *a, const void *b) {{ (void)a; (void)b; return {eq}; }}\nstatic uint64_t {key}_hs(const void *p) {{ (void)p; return {hash}; }}");
                (format!("{key}_eq"), format!("{key}_hs"))
            } else {
                self.helpers.push_str(&extra);
                (format!("{key}_eq2"), format!("{key}_hs2"))
            };
            let _ = writeln!(
                self.helpers,
                "static void {key}_ts(tv_sb *sb, const void *p) {{ (void)p; {ts}; }}\nstatic void {key}_in(tv_sb *sb, const void *p, int depth) {{ (void)p; (void)depth; {ins}; }}\nstatic const tv_type {key} = {{ sizeof({ct}), {rt}, {rl}, {eq_fn}, {hash_fn}, {key}_ts, TVG_INS({key}_in) }};"
            );
        }
        format!("&{key}")
    }

    fn desc_key(&self, t: TyId) -> String {
        format!("TY{}", t.0)
    }

    // ------------------------------------------------------------ functions

    /// C name of a function instance, queueing it for emission.
    pub(crate) fn instance(&mut self, m: u32, item: u32, subst: FxMap<u32, TyId>) -> String {
        self.instance_with(m, item, subst, Vec::new())
    }

    /// `instance`, specialized: each parameter in `direct` is always passed a closure of that C
    /// function (an arrow written at the call site), so calls through it are direct calls the C
    /// compiler can inline (`bench(name, () => s.includes("x"))`, `retry(() => fetchOnce())`).
    pub(crate) fn instance_with(&mut self, m: u32, item: u32, subst: FxMap<u32, TyId>, direct: Vec<(usize, String)>) -> String {
        let mut key_args: Vec<(u32, TyId)> = subst.iter().map(|(&k, &v)| (k, v)).collect();
        key_args.sort();
        if direct.is_empty()
            && let Some((rm, ri, ra, next)) = &self.recur
            && *rm == m
            && *ri == item
            && *ra == key_args
        {
            return next.clone();
        }
        let key = (m, item, key_args, direct.clone());
        if let Some(n) = self.instances.get(&key) {
            return n.clone();
        }
        let ItemKind::Function(f) = &self.ast(m).items[item as usize].kind else { unreachable!() };
        let name = self.sym(f.name);
        let base = if name == "<script>" { format!("f{m}_script_") } else { format!("f{m}_{name}") };
        let cname = if subst.is_empty() && direct.is_empty() { base } else { format!("{base}_{}", self.instances.len()) };
        self.instances.insert(key, cname.clone());
        // Prototype now, body later.
        let sig = self.c.sigs[&(m, item)].clone();
        if self.is_async_fn(m, item) {
            self.declare_async(&cname, &sig.params, &subst);
        } else {
            let proto = self.fn_proto(&cname, &sig.params, sig.ret, &subst, false);
            let _ = writeln!(self.protos, "static {proto};");
        }
        self.queue.push(Instance { m, item, subst, cname: cname.clone(), direct });
        cname
    }

    /// `ret name(params)` for a function with the given (uninstantiated) signature.
    pub(crate) fn fn_proto(&mut self, cname: &str, params: &[FnParam], ret: TyId, subst: &FxMap<u32, TyId>, with_env: bool) -> String {
        let ret = self.c.types.subst(ret, subst);
        let rct = if ret == VOID { "void".to_string() } else { self.ctype(ret) };
        let mut ps: Vec<String> = Vec::new();
        if with_env {
            ps.push("tv_env *env_".into());
        }
        for (i, p) in params.iter().enumerate() {
            let t = self.c.types.subst(p.ty, subst);
            let ct = self.ctype(t);
            ps.push(if p.inout { format!("{ct} *p{i}") } else { format!("{ct} p{i}") });
        }
        if ps.is_empty() {
            ps.push("void".into());
        }
        format!("{rct} {cname}({})", ps.join(", "))
    }

    /// A `tv_fn` value calling a (non-generic) module function.
    pub(crate) fn fn_value(&mut self, m: u32, item: u32) -> String {
        self.fn_value_with(m, item, FxMap::default())
    }

    /// `fn_value` of a generic function's instance (`subst`: its type arguments).
    pub(crate) fn fn_value_with(&mut self, m: u32, item: u32, subst: FxMap<u32, TyId>) -> String {
        let cname = self.instance(m, item, subst.clone());
        if let Some(n) = self.fn_values.get(&cname) {
            return format!("((tv_fn){{ (void *){n}, NULL }})");
        }
        let sig = self.c.sigs[&(m, item)].clone();
        let wname = format!("fv_{cname}");
        let proto = self.fn_proto(&wname, &sig.params, sig.ret, &subst, true);
        let args: Vec<String> = (0..sig.params.len()).map(|i| format!("p{i}")).collect();
        let call = if self.is_async_fn(m, item) { format!("{cname}_spawn({})", args.join(", ")) } else { format!("{cname}({})", args.join(", ")) };
        let body = if sig.ret == VOID { format!("{call};") } else { format!("return {call};") };
        let _ = writeln!(self.protos, "static {proto};");
        let _ = writeln!(self.funcs, "static {proto} {{ (void)env_; {body} }}");
        self.fn_values.insert(cname, wname.clone());
        format!("((tv_fn){{ (void *){wname}, NULL }})")
    }

    /// A static method as a function value (`compareFn = Comparator.defaultCompareFn`).
    pub(crate) fn static_fn_value(&mut self, c: u32, member: u32, subst: FxMap<u32, TyId>) -> String {
        let cname = self.static_method_instance(c, member, subst.clone());
        if let Some(n) = self.fn_values.get(&cname) {
            return format!("((tv_fn){{ (void *){n}, NULL }})");
        }
        let Some(sm) = self.c.classes[c as usize].statics.iter().find(|m| m.member == member).cloned() else { return "((tv_fn){ NULL, NULL })".into() };
        let ret = self.c.method_ret(&sm);
        let wname = format!("fv_{cname}");
        let proto = self.fn_proto(&wname, &sm.params, ret, &subst, true);
        let args: Vec<String> = (0..sm.params.len()).map(|i| format!("p{i}")).collect();
        let call = format!("{cname}({})", args.join(", "));
        let body = if ret == VOID { format!("{call};") } else { format!("return {call};") };
        let _ = writeln!(self.protos, "static {proto};");
        let _ = writeln!(self.funcs, "static {proto} {{ (void)env_; {body} }}");
        self.fn_values.insert(cname, wname.clone());
        format!("((tv_fn){{ (void *){wname}, NULL }})")
    }

    fn emit_function(&mut self, inst: Instance) {
        if self.is_gen_fn(inst.m, inst.item) {
            self.emit_gen_function(inst);
            return;
        }
        if self.is_async_fn(inst.m, inst.item) {
            self.emit_async_function(inst);
            return;
        }
        // A small function calling itself: three copies of it too, each calling the next and the
        // last the function, so the C compiler inlines the recursion a few levels deep (as GCC
        // does a function into itself; clang won't): fewer calls, `fib` twice as fast.
        let copies = if inst.direct.is_empty() && self.recur.is_none() && self.small_self_recursive(inst.m, inst.item) { 3 } else { 0 };
        let mut key_args: Vec<(u32, TyId)> = inst.subst.iter().map(|(&k, &v)| (k, v)).collect();
        key_args.sort();
        if copies > 0 {
            let sig = self.c.sigs[&(inst.m, inst.item)].clone();
            for k in 1..=copies {
                let proto = self.fn_proto(&format!("{}_r{k}", inst.cname), &sig.params, sig.ret, &inst.subst, false);
                let _ = writeln!(self.protos, "static {proto};");
            }
            self.recur = Some((inst.m, inst.item, key_args.clone(), format!("{}_r1", inst.cname)));
        }
        self.emit_function_body(&inst);
        if copies > 0 {
            for k in 1..=copies {
                let next = if k < copies { format!("{}_r{}", inst.cname, k + 1) } else { inst.cname.clone() };
                self.recur = Some((inst.m, inst.item, key_args.clone(), next));
                let copy = Instance { m: inst.m, item: inst.item, subst: inst.subst.clone(), cname: format!("{}_r{k}", inst.cname), direct: Vec::new() };
                self.emit_function_body(&copy);
            }
            self.recur = None;
        }
    }

    /// Whether module function (m, item) calls itself, small enough to copy (see
    /// `emit_function`): no closures in it, a few dozen expressions.
    fn small_self_recursive(&mut self, m: u32, item: u32) -> bool {
        let ItemKind::Function(f) = &self.ast(m).items[item as usize].kind else { return false };
        let body = f.body;
        let ast = self.ast(m);
        let mut exprs = Vec::new();
        body::collect_exprs_stmt(ast, body, &mut exprs);
        if exprs.len() > 60 {
            return false;
        }
        let facts = self.facts(m);
        let mut calls_self = false;
        for &e in &exprs {
            match &ast.expr(e).kind {
                ExprKind::Arrow(_) | ExprKind::Await(_) | ExprKind::Yield(..) => return false,
                ExprKind::Call { .. } => {
                    if let Some(c) = facts.calls.get(&e)
                        && matches!(c.callee, crate::check::Callee::Fn(cm, ci) if cm == m && ci == item)
                    {
                        calls_self = true;
                    }
                }
                _ => {}
            }
        }
        calls_self
    }

    fn emit_function_body(&mut self, inst: &Instance) {
        let ItemKind::Function(f) = &self.ast(inst.m).items[inst.item as usize].kind else { return };
        let sig = self.c.sigs[&(inst.m, inst.item)].clone();
        let proto = self.fn_proto(&inst.cname, &sig.params, sig.ret, &inst.subst, false);
        let params: Vec<(u32, TyId, bool)> = f.params.iter().zip(&sig.params).map(|(p, fp)| (p.span.start, self.c.types.subst(fp.ty, &inst.subst), fp.inout)).collect();
        let ret = self.c.types.subst(sig.ret, &inst.subst);
        // Leak-free `inout` array parameters arrive uniquely owned (callers guarantee it).
        let mut entry_unique = Vec::new();
        for (i, (key, ty, inout)) in params.iter().enumerate() {
            if *inout && matches!(self.tget(*ty), Ty::Array(_)) && self.inout_leak_free(inst.m, inst.item, i) {
                entry_unique.push(*key);
            }
        }
        if let Some(t) = self.tre_candidate(inst.m, inst.item, &inst.subst, &params, ret) {
            // The looping copy takes the first term, then the parameters; the function's own
            // body (first call) hands its tail to it.
            let looper = format!("{}_tre", inst.cname);
            let mut ps: Vec<String> = vec!["tv_int tre_a0".into()];
            for (i, ty) in t.ptys.iter().enumerate() {
                let ct = self.ctype(*ty);
                ps.push(format!("{ct} p{i}"));
            }
            let lproto = format!("tv_int {looper}({})", ps.join(", "));
            let _ = writeln!(self.protos, "static {lproto};");
            self.pending_tre = Some(body::Tre { entry: None, ..t.clone() });
            let lcode = self.function_body(inst.m, inst.subst.clone(), &params, ret, body::FnBodyKind::Block(f.body), Some(entry_unique.clone()), None, None);
            // (a self-recursive function's copy: its loop inlined into the one before it too)
            let inl = if self.recur.is_some() && inst.cname.rsplit_once("_r").is_some_and(|(_, k)| k.chars().all(|c| c.is_ascii_digit())) { "inline " } else { "" };
            let _ = writeln!(self.funcs, "static {inl}{lproto} {{\n{lcode}}}\n");
            self.pending_tre = Some(body::Tre { entry: Some(looper), ..t });
        }
        if !inst.direct.is_empty() {
            self.pending_direct = Some(inst.direct.iter().filter_map(|(i, c)| params.get(*i).map(|p| (p.0, c.clone()))).collect());
        }
        let code = self.function_body(inst.m, inst.subst.clone(), &params, ret, body::FnBodyKind::Block(f.body), Some(entry_unique), None, None);
        let mut attr = self.callback_inline(&params.iter().map(|p| crate::types::FnParam { ty: p.1, inout: p.2, optional: false }).collect::<Vec<_>>(), &code, &inst.cname);
        // (a self-recursive function with copies: the function itself is called, each copy
        // meant to be inlined into the one before it)
        if self.recur.is_some() {
            let copy = inst.cname.rsplit_once("_r").is_some_and(|(_, k)| k.chars().all(|c| c.is_ascii_digit()));
            attr = if copy { "inline " } else { "" };
        }
        let _ = writeln!(self.funcs, "static {attr}{proto} {{\n{code}}}\n");
    }

    /// Accumulator recursion elimination for checked integer `+`: a function like
    /// `fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2) }` runs its tail
    /// `return a + f(args)` as a loop. Evaluation order is unchanged (`a`, then the arguments),
    /// and overflow still traps exactly when the recursion would: the terms are summed as
    /// 128-bit integers, and each partial sum the recursion computes (a suffix sum: the total
    /// minus a prefix sum) is checked at the end through the smallest and largest prefix sums.
    /// C compilers do this for wrapping `+` but not for a checked one. Only for simple
    /// functions: non-generic, `int` result, scalar by-value parameters, no closures, no `try`,
    /// nothing thrown.
    fn tre_candidate(&mut self, m: u32, item: u32, subst: &FxMap<u32, TyId>, params: &[(u32, TyId, bool)], ret: TyId) -> Option<body::Tre> {
        if !subst.is_empty() || ret != INT || self.c.sigs[&(m, item)].throws != NEVER {
            return None;
        }
        let scalar = |g: &Self, t: TyId| matches!(g.tget(t), Ty::Int | Ty::F64 | Ty::F32 | Ty::Bool | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 | Ty::U64);
        if params.iter().any(|&(_, t, inout)| inout || !scalar(self, t)) {
            return None;
        }
        let ast = self.ast(m);
        let ItemKind::Function(f) = &ast.items[item as usize].kind else { return None };
        // No closures (they might capture the parameters the loop rebinds) and no `try`.
        let mut stmts = vec![f.body];
        let mut tails = Vec::new();
        while let Some(s) = stmts.pop() {
            match &ast.stmt(s).kind {
                StmtKind::Try { .. } => return None,
                StmtKind::Block(ss) => stmts.extend(ss.iter().copied()),
                StmtKind::If(_, t, e) => {
                    stmts.push(*t);
                    stmts.extend(e.iter().copied());
                }
                StmtKind::While(_, b) | StmtKind::DoWhile(b, _) | StmtKind::ForOf { body: b, .. } => stmts.push(*b),
                StmtKind::For { init, body: b, .. } => {
                    stmts.extend(init.iter().copied());
                    stmts.push(*b);
                }
                StmtKind::Switch(_, cases) => stmts.extend(cases.iter().flat_map(|c| c.body.iter().copied())),
                StmtKind::Return(Some(e)) => tails.push(*e),
                _ => {}
            }
        }
        let mut exprs = Vec::new();
        body::collect_exprs_stmt_pub(ast, f.body, &mut exprs);
        if exprs.iter().any(|&e| matches!(ast.expr(e).kind, ExprKind::Arrow(_))) {
            return None;
        }
        let ptys: Vec<TyId> = params.iter().map(|p| p.1).collect();
        for e in tails {
            if let ExprKind::Binary(ast::BinOp::Add, a, call) = &ast.expr(e).kind
                && let ExprKind::Call { args, optional: false, .. } = &ast.expr(*call).kind
                && args.len() == ptys.len()
                && let Some(fact) = self.facts(m).calls.get(call)
                && matches!(fact.callee, Callee::Fn(fm, fi) if fm == m && fi == item)
                && self.facts(m).expr_ty[e as usize] == INT
                && self.facts(m).expr_ty[*a as usize] == INT
            {
                let span = ast.expr(e).span;
                let loc = self.loc(span);
                return Some(body::Tre { m, item, ptys, loc, entry: None });
            }
        }
        None
    }

    /// Prints an error that escaped `main` (stderr) or fails the running test.
    fn uncaught_fn(&mut self) -> String {
        let err = self.c.error_class();
        let text = if err == ERROR { "tv_sb_push_cstr(sb, \"Error\")".to_string() } else {
            let ct = self.ctype(err);
            self.string_code(err, &format!("(({ct})e)"))
        };
        let _ = writeln!(self.protos, "static void tvg_uncaught(void);");
        format!(
            "static void tvg_uncaught(void) {{\n    void *e = tvg_err; tvg_err = NULL;\n    tv_sb sbv = {{0}}; tv_sb *sb = &sbv;\n    tv_sb_push_cstr(sb, \"uncaught \");\n    {text};\n    tv_sb_push_char(sb, '\\0');\n    tv_out_flush();\n    if (tv_test_active()) tv_trap(sbv.data, NULL);\n    sbv.data[sbv.len - 1] = '\\n';\n    tv_write_fd(2, sbv.data, sbv.len);\n}}\n"
        )
    }

    fn test_fn(&mut self, m: u32, arrow: ExprId) -> String {
        let name = self.fresh("test_");
        let ast::ExprKind::Arrow(f) = &self.ast(m).expr(arrow).kind else { return name };
        let body = match &f.body {
            ast::ArrowBody::Block(b) => body::FnBodyKind::Block(*b),
            ast::ArrowBody::Expr(e) => body::FnBodyKind::Expr(*e),
        };
        let code = self.function_body(m, FxMap::default(), &[], VOID, body, None, None, None);
        let _ = writeln!(self.funcs, "static void {name}_body(void) {{\n{code}}}\n");
        let _ = writeln!(self.funcs, "static void {name}(void) {{ {name}_body(); if (tvg_err) tvg_uncaught(); }}\n");
        let _ = writeln!(self.protos, "static void tvg_uncaught(void);");
        name
    }

    // ------------------------------------------------------------ module constants

    /// C expression for a module constant, emitting its global and initializer on first use.
    pub(crate) fn const_ref(&mut self, m: u32, item: u32) -> Val {
        let ty = self.c.consts[&(m, item)];
        let ItemKind::Const { name, init, mutable, .. } = &self.ast(m).items[item as usize].kind else { unreachable!() };
        let mutable = *mutable;
        let gname = format!("g{m}_{}", self.sym(*name));
        if self.consts_done.insert((m, item)) {
            let ct = self.ctype(ty);
            // A number or boolean literal: a C constant, which the C compiler folds into its uses
            // (`x % IM` by a multiplication), a script's too
            if !mutable
                && let Some(lit) = self.literal_const(m, *init, ty)
            {
                let _ = writeln!(self.protos, "static const {ct} {gname} = {lit};");
                return Val::plain(gname, ty);
            }
            let _ = writeln!(self.protos, "static {ct} {gname};");
            // An initializer that awaits runs inline in the (async) script body instead.
            if self.script_module == Some(m) && asyncfn::has_await(self.ast(m), *init) {
                return Val::plain(gname, ty);
            }
            for (cm, ci) in self.consts_read_by_called_fns(m, *init) {
                self.const_ref(cm, ci);
            }
            // Initialize dependencies first (their `const_ref` calls happen while emitting this initializer).
            let code = self.function_body(m, FxMap::default(), &[], ty, body::FnBodyKind::Expr(*init), None, None, None);
            let fname = format!("init_{gname}");
            let _ = writeln!(self.funcs, "static {ct} {fname}(void) {{\n{code}}}\n");
            // A script initializes its own variables where they're declared.
            if self.script_module != Some(m) {
                let rank = self.init_rank(m, item, 0);
                let mut line = format!("    {gname} = {fname}();\n");
                // (an error it throws is uncaught, as JavaScript's module evaluation leaves it)
                if code.contains("tvg_err") {
                    line.push_str("    if (tvg_err) { tvg_uncaught(); return 1; }\n");
                    self.needs_uncaught = true;
                }
                self.const_init.push((rank, line));
            }
        }
        Val::plain(gname, ty)
    }

    /// When JavaScript runs module `m`'s item `item` (member `member` + 1 of a class, 0 for a
    /// variable): modules in evaluation order (each after what it imports, depth first from the
    /// entry modules, the prelude and standard modules first), then in source order.
    pub(crate) fn init_rank(&mut self, m: u32, item: u32, member: u32) -> (u32, u32, u32) {
        if self.module_rank.is_empty() {
            let n = self.c.modules.len();
            let mut rank = vec![u32::MAX; n];
            let mut next = 0u32;
            fn visit(ms: &[crate::check::Module], m: usize, rank: &mut [u32], next: &mut u32, on: &mut Vec<bool>) {
                if rank[m] != u32::MAX || on[m] {
                    return;
                }
                on[m] = true;
                let mut deps: Vec<(u32, u32)> = ms[m].imports.iter().map(|(&i, &d)| (i, d)).collect();
                deps.sort();
                for (_, d) in deps {
                    visit(ms, d as usize, rank, next, on);
                }
                on[m] = false;
                rank[m] = *next;
                *next += 1;
            }
            let mut on = vec![false; n];
            let mods = &self.c.modules;
            for i in 0..n {
                if mods[i].builtin || mods[i].std {
                    visit(mods, i, &mut rank, &mut next, &mut on);
                }
            }
            for i in 0..n {
                if mods[i].entry {
                    visit(mods, i, &mut rank, &mut next, &mut on);
                }
            }
            for i in 0..n {
                visit(mods, i, &mut rank, &mut next, &mut on);
            }
            self.module_rank = rank;
        }
        (self.module_rank[m as usize], item, member)
    }

    fn consts_read_by_called_fns(&self, m: u32, init: ExprId) -> Vec<(u32, u32)> {
        let mut exprs = Vec::new();
        body::collect_exprs_expr(self.ast(m), init, &mut exprs);
        let mut work: Vec<(u32, u32)> = exprs.iter().filter_map(|&e| self.fn_named_by(m, e)).collect();
        let mut seen = FxSet::default();
        let mut consts = Vec::new();
        while let Some((fm, fi)) = work.pop() {
            if !seen.insert((fm, fi)) {
                continue;
            }
            let ast = self.ast(fm);
            let ItemKind::Function(f) = &ast.items[fi as usize].kind else { continue };
            let mut exprs = Vec::new();
            body::collect_exprs_stmt_pub(ast, f.body, &mut exprs);
            for e in exprs {
                if let Some(g) = self.fn_named_by(fm, e) {
                    work.push(g);
                } else if let Some(IdentFact::Const(cm, ci)) = self.ident_fact(fm, e) {
                    consts.push((cm, ci));
                }
            }
        }
        consts
    }

    fn fn_named_by(&self, m: u32, e: ExprId) -> Option<(u32, u32)> {
        match (self.ident_fact(m, e), self.facts(m).calls.get(&e).map(|c| &c.callee)) {
            (Some(IdentFact::Fn(fm, fi)), _) | (_, Some(&Callee::Fn(fm, fi))) => Some((fm, fi)),
            _ => None,
        }
    }

    /// The C text of a module constant's initializer when it's a number or boolean literal
    /// (optionally negated) of the constant's own type.
    /// Module variable (m, item) is a C constant (see `const_ref`).
    pub(crate) fn literal_global(&self, m: u32, item: u32) -> bool {
        let ItemKind::Const { init, mutable: false, .. } = &self.ast(m).items[item as usize].kind else { return false };
        self.c.consts.get(&(m, item)).is_some_and(|&ty| self.literal_const(m, *init, ty).is_some())
    }

    fn literal_const(&self, m: u32, init: ExprId, ty: TyId) -> Option<String> {
        let ast = self.ast(m);
        let (neg, e) = match &ast.expr(init).kind {
            ast::ExprKind::Unary(ast::UnOp::Neg, x) => ("-", *x),
            _ => ("", init),
        };
        let t = *self.c.types.get(ty);
        match (&ast.expr(e).kind, t) {
            (ast::ExprKind::Int(v), Ty::Int) if *v <= i64::MAX as u64 => Some(format!("{neg}INT64_C({v})")),
            (ast::ExprKind::Int(v), Ty::F64) => Some(format!("{neg}{v}.0")),
            (ast::ExprKind::Float(f), Ty::F64) if f.is_finite() => {
                let mut s = format!("{f:?}");
                if !s.contains('.') && !s.contains('e') {
                    s.push_str(".0");
                }
                Some(format!("{neg}{s}"))
            }
            (ast::ExprKind::Bool(b), Ty::Bool) if neg.is_empty() => Some(b.to_string()),
            _ => None,
        }
    }

    /// `E.name` of a module constant `E` that's an object literal of literals (an enum) and only
    /// ever read as `E.field`, nowhere stored to or passed on: the field's literal expression.
    pub(crate) fn const_member(&mut self, m: u32, e: ExprId) -> Option<(u32, ExprId)> {
        let ast::ExprKind::Member { obj, name, optional: false, .. } = self.ast(m).expr(e).kind else { return None };
        let fact = self.ident_fact(m, obj)?;
        let (cm, init) = match fact {
            IdentFact::Const(cm, ci) => {
                let ItemKind::Const { init, mutable: false, .. } = &self.ast(cm).items[ci as usize].kind else { return None };
                (cm, *init)
            }
            // (a script's enum: a local of the function running its statements)
            IdentFact::Local(_) => (m, crate::check::stmt::enum_object(&self.c.modules, m, fact)?.1),
            _ => return None,
        };
        let ast::ExprKind::Object(fs) = &self.ast(cm).expr(init).kind else { return None };
        let v = fs.iter().find(|f| f.name == name)?.value;
        let lit = |x: ExprId| match &self.ast(cm).expr(x).kind {
            ast::ExprKind::Int(_) | ast::ExprKind::Float(_) | ast::ExprKind::Bool(_) | ast::ExprKind::Str(_) => true,
            ast::ExprKind::Unary(ast::UnOp::Neg, y) => matches!(self.ast(cm).expr(*y).kind, ast::ExprKind::Int(_) | ast::ExprKind::Float(_)),
            _ => false,
        };
        if !lit(v) || !self.read_only_const(fact) {
            return None;
        }
        Some((cm, v))
    }

    /// `E.name` of an enum (a module constant object with a type of the same name) whose
    /// member is a string literal: the string (see the checker's `enum_str_member`).
    pub(crate) fn enum_str_member(&self, m: u32, e: ExprId) -> Option<crate::intern::Sym> {
        let ast::ExprKind::Member { obj, name, optional: false, .. } = self.ast(m).expr(e).kind else { return None };
        let fact = self.ident_fact(m, obj)?;
        let (mast, init) = crate::check::stmt::enum_object(&self.c.modules, m, fact)?;
        let ast::ExprKind::Object(fs) = &mast.expr(init).kind else { return None };
        let v = fs.iter().find(|f| f.name == name)?.value;
        match mast.expr(v).kind {
            ast::ExprKind::Str(s) => Some(s),
            _ => None,
        }
    }

    /// Constant `which` (a module's, or a script's local) appears only as the object of
    /// `E.field` reads.
    fn read_only_const(&mut self, which: IdentFact) -> bool {
        if let Some(&r) = self.ro_consts.get(&which) {
            return r;
        }
        let mut ok = true;
        'mods: for m in 0..self.c.modules.len() as u32 {
            let ast = self.ast(m);
            let facts = self.facts(m);
            // (each use, and what it's the object of)
            let mut parent: FxMap<ExprId, ExprId> = FxMap::default();
            for (i, x) in ast.exprs.iter().enumerate() {
                if let ast::ExprKind::Member { obj, .. } = x.kind {
                    parent.insert(obj, i as ExprId);
                }
            }
            let mut targets: FxSet<ExprId> = FxSet::default();
            for x in ast.exprs.iter() {
                match &x.kind {
                    ast::ExprKind::Assign(_, t, _) | ast::ExprKind::Update { target: t, .. } => {
                        targets.insert(*t);
                    }
                    ast::ExprKind::Call { args, .. } => targets.extend(args.iter().filter(|a| a.by_ref.is_some()).map(|a| a.expr)),
                    _ => {}
                }
            }
            for (&e, f) in facts.idents.iter() {
                if *f != which {
                    continue;
                }
                match parent.get(&e) {
                    Some(p) if !targets.contains(p) => {}
                    _ => {
                        ok = false;
                        break 'mods;
                    }
                }
            }
        }
        self.ro_consts.insert(which, ok);
        ok
    }

    pub(crate) fn ident_fact(&self, m: u32, e: ExprId) -> Option<IdentFact> {
        self.facts(m).idents.get(&e).copied()
    }
}

/// A C string literal for arbitrary bytes.
pub(crate) fn c_string(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() + 2);
    out.push('"');
    for &b in bytes {
        match b {
            b'"' => out.push_str("\\\""),
            b'\\' => out.push_str("\\\\"),
            b'\n' => out.push_str("\\n"),
            b'\t' => out.push_str("\\t"),
            b'\r' => out.push_str("\\r"),
            b'?' => out.push_str("\\?"),
            0x20..=0x7e => out.push(b as char),
            _ => {
                let _ = write!(out, "\\{:03o}", b);
            }
        }
    }
    out.push('"');
    out
}

/// Small helpers used by generated code (after the runtime).
const PRELUDE: &str = include_str!("prelude.h");
