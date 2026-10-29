//! Async functions keep all their state in a frame, so they can return at an `await` and resume
//! there later (see `asyncfn.rs`). Their body is generated as ordinary C first; `lower` then
//! moves every local declaration into the frame, rewrites uses to `F->name`, and turns `return`
//! into "store the result; finished". Nothing else changes, so every construct compiles exactly
//! as it does in a synchronous function. Anything it can't parse is an error, never wrong code.
//!
//! A local that fits a register and whose address is never taken is kept in a C variable of the
//! same name while the function runs instead: loaded from the frame on entry and stored back when
//! it suspends (and, for parameters, when it finishes). Resuming jumps into the middle of the body
//! (`goto *F->pc`), and C compilers can't keep a value in a register across such a jump when it
//! lives in memory, so without this a loop around an `await` reads and writes every variable
//! through the frame on each pass.
//!
//! The same jumps make a loop with an `await` in it one a C compiler can't optimize as a loop
//! (it has more than one way in). Such a body is emitted twice: first as the function's first
//! run, with no labels to resume at (it suspends to the labels of the second copy), then as is,
//! for resuming. A run that never waits stays in the first, whose loops are ordinary ones.

use std::collections::HashMap;

#[derive(Clone, Copy, PartialEq, Debug)]
enum Kind {
    Ident,
    Punct,
    /// Whitespace, comments, literals and numbers: copied as they are.
    Other,
}

fn tokenize(src: &str) -> Vec<(Kind, &str)> {
    let b = src.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        let start = i;
        let c = b[i];
        let kind = if c.is_ascii_whitespace() {
            while i < b.len() && b[i].is_ascii_whitespace() {
                i += 1;
            }
            Kind::Other
        } else if c == b'/' && b.get(i + 1) == Some(&b'*') {
            i += 2;
            while i + 1 < b.len() && !(b[i] == b'*' && b[i + 1] == b'/') {
                i += 1;
            }
            i = (i + 2).min(b.len());
            Kind::Other
        } else if c == b'/' && b.get(i + 1) == Some(&b'/') {
            while i < b.len() && b[i] != b'\n' {
                i += 1;
            }
            Kind::Other
        } else if c == b'"' || c == b'\'' {
            i += 1;
            while i < b.len() && b[i] != c {
                if b[i] == b'\\' {
                    i += 1;
                }
                i += 1;
            }
            i = (i + 1).min(b.len());
            Kind::Other
        } else if c.is_ascii_alphabetic() || c == b'_' {
            while i < b.len() && (b[i].is_ascii_alphanumeric() || b[i] == b'_') {
                i += 1;
            }
            Kind::Ident
        } else if c.is_ascii_digit() || (c == b'.' && b.get(i + 1).is_some_and(|d| d.is_ascii_digit())) {
            let hex = c == b'0' && matches!(b.get(i + 1), Some(b'x' | b'X'));
            i += 1;
            while i < b.len() {
                let d = b[i];
                if d.is_ascii_alphanumeric() || d == b'_' || d == b'.' {
                    i += 1;
                } else if (d == b'+' || d == b'-') && !hex && matches!(b[i - 1], b'e' | b'E' | b'p' | b'P') {
                    i += 1;
                } else {
                    break;
                }
            }
            Kind::Other
        } else {
            const OPS: &[&str] = &["<<=", ">>=", "...", "->", "++", "--", "&&", "||", "<<", ">>", "<=", ">=", "==", "!=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="];
            let rest = &src[i..];
            let len = OPS.iter().find(|op| rest.starts_with(**op)).map(|op| op.len()).unwrap_or_else(|| rest.chars().next().map(|ch| ch.len_utf8()).unwrap_or(1));
            i += len;
            Kind::Punct
        };
        out.push((kind, &src[start..i]));
    }
    out
}

/// Words that start statements (never a declaration's type).
const STATEMENT_WORDS: &[&str] = &["return", "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "goto", "sizeof", "TV_SUSPEND"];

/// A local moved into the frame.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Field {
    /// The C type (`tv_str`, `tv_int *`, ...).
    pub ty: String,
    pub name: String,
}

pub(crate) struct Lowered {
    pub fields: Vec<Field>,
    /// The locals and parameters kept in C variables: their declarations, loaded from the frame.
    pub prologue: String,
    pub body: String,
}

/// A declaration found at token index `start`: its type, declarators and where it ends (the `;`).
struct Decl {
    ty: String,
    /// (pointer stars, name, initializer token range)
    declarators: Vec<(usize, String, Option<(usize, usize)>)>,
    end: usize,
}

/// Index of the next significant token at or after `i`.
fn sig(toks: &[(Kind, &str)], mut i: usize) -> usize {
    while i < toks.len() && toks[i].0 == Kind::Other && (toks[i].1.trim().is_empty() || toks[i].1.starts_with("/*") || toks[i].1.starts_with("//")) {
        i += 1;
    }
    i
}

fn parse_decl(toks: &[(Kind, &str)], start: usize) -> Result<Option<Decl>, String> {
    let mut i = sig(toks, start);
    let mut ty = String::new();
    if i < toks.len() && toks[i].1 == "const" {
        ty.push_str("const ");
        i = sig(toks, i + 1);
    }
    if i < toks.len() && toks[i].1 == "struct" {
        ty.push_str("struct ");
        i = sig(toks, i + 1);
    }
    let Some(&(Kind::Ident, tname)) = toks.get(i) else { return Ok(None) };
    if STATEMENT_WORDS.contains(&tname) {
        return Ok(None);
    }
    ty.push_str(tname);
    // `T name`, `T *name`: the type is followed by stars and a name.
    let mut j = sig(toks, i + 1);
    while j < toks.len() && toks[j].1 == "*" {
        j = sig(toks, j + 1);
    }
    let Some(&(Kind::Ident, _)) = toks.get(j) else { return Ok(None) };
    let after = sig(toks, j + 1);
    if !matches!(toks.get(after).map(|t| t.1), Some("=" | ";" | "," | "[")) {
        return Ok(None);
    }
    let mut declarators = Vec::new();
    let mut k = i + 1;
    loop {
        k = sig(toks, k);
        let mut stars = 0;
        while k < toks.len() && toks[k].1 == "*" {
            stars += 1;
            k = sig(toks, k + 1);
        }
        let Some(&(Kind::Ident, name)) = toks.get(k) else { return Err(format!("unexpected declarator near `{}`", toks.get(k).map(|t| t.1).unwrap_or("<end>"))) };
        k = sig(toks, k + 1);
        match toks.get(k).map(|t| t.1) {
            Some("[") => return Err(format!("array local `{name}` in an async function")),
            Some("=") => {
                let from = k + 1;
                let mut depth = 0i32;
                let mut e = from;
                while e < toks.len() {
                    match toks[e].1 {
                        "(" | "[" | "{" => depth += 1,
                        ")" | "]" | "}" => depth -= 1,
                        "," | ";" if depth == 0 => break,
                        _ => {}
                    }
                    e += 1;
                }
                declarators.push((stars, name.to_string(), Some((from, e))));
                k = e;
            }
            _ => declarators.push((stars, name.to_string(), None)),
        }
        match toks.get(k).map(|t| t.1) {
            Some(",") => k += 1,
            Some(";") => return Ok(Some(Decl { ty, declarators, end: k })),
            other => return Err(format!("unexpected `{}` in a declaration", other.unwrap_or("<end>"))),
        }
    }
}

/// C types that fit a register: kept in C variables (see the module comment).
fn register_type(ty: &str) -> bool {
    !ty.starts_with("const ") && (ty.ends_with('*') || matches!(ty, "tv_int" | "double" | "float" | "bool" | "int" | "int32_t" | "int64_t" | "uint8_t" | "uint32_t" | "uint64_t" | "size_t" | "tv_str"))
}

/// Names whose address the body takes (`&x`, `&(x)`, `&x.f`; not `&p->f` or `&p[i]`, which
/// address what `p` points to): a pointer to them may outlive this run of the function.
fn addressed<'a>(toks: &[(Kind, &'a str)]) -> std::collections::HashSet<&'a str> {
    let mut out = std::collections::HashSet::new();
    for i in 0..toks.len() {
        if toks[i].1 != "&" {
            continue;
        }
        let mut j = sig(toks, i + 1);
        let mut parens = false;
        while toks.get(j).is_some_and(|t| t.1 == "(") {
            parens = true;
            j = sig(toks, j + 1);
        }
        let Some(&(Kind::Ident, name)) = toks.get(j) else { continue };
        let next = toks.get(sig(toks, j + 1)).map(|t| t.1);
        if parens || !matches!(next, Some("->" | "[")) {
            out.insert(name);
        }
    }
    out
}

/// The body as its first run (see the module comment) when it has an `await` inside a loop: its
/// resume labels (`aw<n>:`, taken with `&&`) removed, so nothing jumps into it, and its other
/// labels renamed, so they don't clash with the copy that resumes. None otherwise.
fn first_run(body: &str) -> Option<String> {
    let toks = tokenize(body);
    let next = |i: usize| sig(&toks, i + 1);
    // labels defined here: `name:;` where a statement starts
    let mut labels: Vec<(usize, &str)> = Vec::new();
    let mut prev = "";
    for i in 0..toks.len() {
        let (kind, text) = toks[i];
        if kind == Kind::Other {
            continue;
        }
        if kind == Kind::Ident && matches!(prev, "" | ";" | "{" | "}") && text != "default" {
            let c = next(i);
            if toks.get(c).is_some_and(|t| t.1 == ":") && toks.get(next(c)).is_some_and(|t| t.1 == ";") {
                labels.push((i, text));
            }
        }
        prev = text;
    }
    let resumes: std::collections::HashSet<&str> = toks.windows(2).filter(|w| w[0].1 == "&&" && w[1].0 == Kind::Ident).map(|w| w[1].1).collect();
    // the braced bodies of loops: `for (...) {`, `while (...) {`, `do {`
    let close = |open: usize| -> Option<usize> {
        let mut depth = 0i32;
        for (j, t) in toks.iter().enumerate().skip(open) {
            match t.1 {
                "(" | "{" => depth += 1,
                ")" | "}" => {
                    depth -= 1;
                    if depth == 0 {
                        return Some(j);
                    }
                }
                _ => {}
            }
        }
        None
    };
    let mut loops = Vec::new();
    for i in 0..toks.len() {
        let open = match toks[i].1 {
            "for" | "while" if toks[i].0 == Kind::Ident && toks.get(next(i)).is_some_and(|t| t.1 == "(") => close(next(i)).map(next),
            "do" if toks[i].0 == Kind::Ident => Some(next(i)),
            _ => None,
        };
        if let Some(open) = open
            && toks.get(open).is_some_and(|t| t.1 == "{")
            && let Some(end) = close(open)
        {
            loops.push(open..end);
        }
    }
    if !labels.iter().any(|(at, name)| resumes.contains(name) && loops.iter().any(|l| l.contains(at))) {
        return None;
    }
    let renamed: std::collections::HashSet<&str> = labels.iter().map(|l| l.1).filter(|n| !resumes.contains(n)).collect();
    let defined: std::collections::HashSet<usize> = labels.iter().map(|l| l.0).collect();
    let mut out = String::with_capacity(body.len() + 64);
    let mut prev = "";
    let mut i = 0;
    while i < toks.len() {
        let (kind, text) = toks[i];
        if defined.contains(&i) && resumes.contains(text) {
            // (`name:`, leaving the `;`)
            i = next(i) + 1;
            continue;
        }
        out.push_str(text);
        if kind == Kind::Ident && renamed.contains(text) && (prev == "goto" || defined.contains(&i)) {
            out.push_str("_f");
        }
        if kind != Kind::Other {
            prev = text;
        }
        i += 1;
    }
    Some(out)
}

/// Moves `body`'s locals into the frame. `params` are already frame fields (named as in the body).
pub(crate) fn lower(body: &str, params: &[Field]) -> Result<Lowered, String> {
    let toks = tokenize(body);
    // Pass 1: find the declarations (at statement starts and in `for (` headers).
    let mut decls: HashMap<usize, Decl> = HashMap::new();
    let mut fields: Vec<Field> = Vec::new();
    let mut depth = 0i32;
    let mut at_start = true;
    let mut i = 0;
    while i < toks.len() {
        let (kind, text) = toks[i];
        if kind == Kind::Other {
            i += 1;
            continue;
        }
        let for_init = text == "(" && i > 0 && toks[..i].iter().rev().find(|t| t.0 != Kind::Other).map(|t| t.1) == Some("for");
        if at_start && depth == 0 || for_init {
            let from = if for_init { i + 1 } else { i };
            if let Some(d) = parse_decl(&toks, from)? {
                for (stars, name, _) in &d.declarators {
                    let ty = format!("{}{}", d.ty, " *".repeat(*stars).replace("* *", "**"));
                    let ty = ty.trim_end().to_string();
                    match fields.iter().find(|f| f.name == *name) {
                        Some(f) if f.ty != ty => return Err(format!("`{name}` is declared twice with different types (`{}`, `{ty}`)", f.ty)),
                        Some(_) => {}
                        None => fields.push(Field { ty, name: name.clone() }),
                    }
                }
                let end = d.end;
                decls.insert(from, d);
                if for_init {
                    depth += 1;
                    i = end;
                    continue;
                }
                i = end + 1;
                at_start = true;
                continue;
            }
        }
        match text {
            "(" | "[" => depth += 1,
            ")" | "]" => depth -= 1,
            _ => {}
        }
        at_start = depth == 0 && matches!(text, ";" | "{" | "}");
        i += 1;
    }
    // (the frame's: rewritten to `F->name`; the shadowed: C variables)
    let addressed = addressed(&toks);
    let shadowed: Vec<&Field> = params.iter().chain(fields.iter()).filter(|f| register_type(&f.ty) && !addressed.contains(f.name.as_str())).collect();
    let in_c: std::collections::HashSet<&str> = shadowed.iter().map(|f| f.name.as_str()).collect();
    let names: std::collections::HashSet<&str> = fields.iter().chain(params.iter()).map(|f| f.name.as_str()).filter(|n| !in_c.contains(n)).collect();
    // How the body leaves: with C variables, through labels that store them in the frame first.
    let (done, suspend) = if shadowed.is_empty() { ("return true;", "TV_SUSPEND") } else { ("goto tvf_done;", "goto tvf_suspend") };
    let mut suspends = false;

    // Pass 2: rewrite.
    let mut out = String::with_capacity(body.len() + body.len() / 4);
    let mut prev_sig: &str = "";
    let mut i = 0;
    let emit_range = |out: &mut String, from: usize, to: usize, names: &std::collections::HashSet<&str>| {
        let mut prev: &str = "";
        for &(kind, text) in &toks[from..to] {
            if kind == Kind::Ident && names.contains(text) && prev != "." && prev != "->" {
                out.push_str("F->");
            }
            out.push_str(text);
            if kind != Kind::Other {
                prev = text;
            }
        }
    };
    while i < toks.len() {
        if let Some(d) = decls.get(&i) {
            let mut first = true;
            for (stars, name, init) in &d.declarators {
                let Some((from, to)) = *init else { continue };
                if !first {
                    out.push(' ');
                }
                first = false;
                if names.contains(name.as_str()) {
                    out.push_str("F->");
                }
                out.push_str(name);
                out.push_str(" = ");
                let init_start = sig(&toks, from);
                if toks.get(init_start).map(|t| t.1) == Some("{") {
                    // A brace initializer becomes a compound literal.
                    out.push('(');
                    out.push_str(&d.ty);
                    out.push_str(&"*".repeat(*stars));
                    out.push(')');
                }
                emit_range(&mut out, init_start, to, &names);
                out.push(';');
            }
            if first {
                out.push(';');
            }
            i = d.end + 1;
            prev_sig = ";";
            continue;
        }
        let (kind, text) = toks[i];
        if kind == Kind::Ident && text == "return" {
            // `return;` / `return e;` → store the result, then report "finished".
            let mut e = i + 1;
            let mut depth = 0i32;
            while e < toks.len() {
                match toks[e].1 {
                    "(" | "[" | "{" => depth += 1,
                    ")" | "]" | "}" => depth -= 1,
                    ";" if depth == 0 => break,
                    _ => {}
                }
                e += 1;
            }
            let has_value = toks[i + 1..e].iter().any(|t| t.0 != Kind::Other);
            if has_value {
                out.push_str("{ F->ret =");
                emit_range(&mut out, i + 1, e, &names);
                out.push_str("; ");
                out.push_str(done);
                out.push_str(" }");
            } else {
                out.push_str(done);
            }
            i = e + 1;
            prev_sig = ";";
            continue;
        }
        if kind == Kind::Ident && text == "TV_SUSPEND" {
            suspends = true;
            out.push_str(suspend);
            prev_sig = text;
            i += 1;
            continue;
        }
        if kind == Kind::Ident && names.contains(text) && prev_sig != "." && prev_sig != "->" {
            out.push_str("F->");
        }
        out.push_str(text);
        if kind != Kind::Other {
            prev_sig = text;
        }
        i += 1;
    }
    if let Some(first) = first_run(&out) {
        let resumed = std::mem::replace(&mut out, first);
        out.push_str("    ");
        out.push_str(done);
        out.push('\n');
        out.push_str(&resumed);
    }
    let mut prologue = String::new();
    if !shadowed.is_empty() {
        for f in &shadowed {
            let space = if f.ty.ends_with('*') { "" } else { " " };
            prologue.push_str(&format!("    {}{space}{} = F->{};\n", f.ty, f.name, f.name));
        }
        out.push_str("    goto tvf_done;\n");
        if suspends {
            out.push_str("tvf_suspend:\n");
            for f in &shadowed {
                out.push_str(&format!("    F->{} = {};\n", f.name, f.name));
            }
            out.push_str("    return false;\n");
        }
        // (finished: the task releases the parameters it was given, from the frame)
        out.push_str("tvf_done:\n");
        for f in shadowed.iter().filter(|f| params.iter().any(|p| p.name == f.name)) {
            out.push_str(&format!("    F->{} = {};\n", f.name, f.name));
        }
    }
    Ok(Lowered { fields, prologue, body: out })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn field(ty: &str, name: &str) -> Field {
        Field { ty: ty.into(), name: name.into() }
    }

    fn lowered(body: &str, params: &[Field]) -> (String, String, Vec<(String, String)>) {
        let l = lower(body, params).unwrap();
        (l.prologue, l.body, l.fields.into_iter().map(|f| (f.ty, f.name)).collect())
    }

    #[test]
    fn moves_declarations_and_rewrites_uses() {
        let (p, b, f) = lowered("    R1 t1 = p0;\n    tv_sb t2 = tv_sb_of(t1, x.t1);\n    t1.n++;\n", &[field("R1", "p0")]);
        assert_eq!(p, "");
        assert_eq!(b, "    F->t1 = F->p0;\n    F->t2 = tv_sb_of(F->t1, x.t1);\n    F->t1.n++;\n");
        assert_eq!(f, vec![("R1".into(), "t1".into()), ("tv_sb".into(), "t2".into())]);
    }

    #[test]
    fn keeps_register_locals_in_c_variables() {
        let (p, b, f) = lowered("tv_int c3 = 0; void *c3k, *c3v;\ntv_str r;\nr = s;\n", &[]);
        assert_eq!(p, "    tv_int c3 = F->c3;\n    void *c3k = F->c3k;\n    void *c3v = F->c3v;\n    tv_str r = F->r;\n");
        assert_eq!(b, "c3 = 0; ;\n;\nr = s;\n    goto tvf_done;\ntvf_done:\n");
        assert_eq!(f, vec![("tv_int".into(), "c3".into()), ("void *".into(), "c3k".into()), ("void *".into(), "c3v".into()), ("tv_str".into(), "r".into())]);
    }

    #[test]
    fn stores_c_variables_when_suspending_and_parameters_when_done() {
        let (p, b, _) = lowered("tv_int t = p0 + 1;\nif (!f(&F->u.aw1)) TV_SUSPEND;\nreturn t;\n", &[field("tv_int", "p0")]);
        assert_eq!(p, "    tv_int p0 = F->p0;\n    tv_int t = F->t;\n");
        assert_eq!(b, "t = p0 + 1;\nif (!f(&F->u.aw1)) goto tvf_suspend;\n{ F->ret = t; goto tvf_done; }\n    goto tvf_done;\ntvf_suspend:\n    F->p0 = p0;\n    F->t = t;\n    return false;\ntvf_done:\n    F->p0 = p0;\n");
    }

    #[test]
    fn keeps_what_is_addressed_in_the_frame() {
        let (p, b, _) = lowered("tv_int a = 1; tv_int c = 2; tv_int *q = &a;\ng(&(c)); h(&p0->x, &p0[1]);\n", &[field("R1 *", "p0")]);
        assert_eq!(p, "    R1 *p0 = F->p0;\n    tv_int *q = F->q;\n");
        assert_eq!(b, "F->a = 1; F->c = 2; q = &F->a;\ng(&(F->c)); h(&p0->x, &p0[1]);\n    goto tvf_done;\ntvf_done:\n    F->p0 = p0;\n");
    }

    #[test]
    fn for_headers_braces_and_returns() {
        let (p, b, f) = lowered("for (tv_int i4 = 0; i4 < n; i4++) { E7 ev = { {-1, 0, NULL}, &(i4) }; f(&ev); }\nreturn i4;\nreturn;\n", &[]);
        assert_eq!(p, "");
        assert_eq!(b, "for (F->i4 = 0; F->i4 < n; F->i4++) { F->ev = (E7){ {-1, 0, NULL}, &(F->i4) }; f(&F->ev); }\n{ F->ret = F->i4; return true; }\nreturn true;\n");
        assert_eq!(f, vec![("tv_int".into(), "i4".into()), ("E7".into(), "ev".into())]);
    }

    #[test]
    fn leaves_strings_members_labels_and_expression_statements() {
        let (_, b, _) = lowered("R1 x = y;\nputs(\"x = 1;\"); s->x = x; F->pc = &&L2; L2:; *p = x; (void)x;\n", &[]);
        assert_eq!(b, "F->x = y;\nputs(\"x = 1;\"); s->x = F->x; F->pc = &&L2; L2:; *p = F->x; (void)F->x;\n");
    }

    #[test]
    fn a_loop_with_an_await_gets_a_first_run_without_labels() {
        let body = "R1 x = y;\nfor (;;) { if (!f()) { F->pc = &&aw1; TV_SUSPEND; } aw1:; if (g) goto cont2; h(x); cont2:; }\nreturn;\n";
        let (_, b, _) = lowered(body, &[]);
        assert_eq!(
            b,
            "F->x = y;\nfor (;;) { if (!f()) { F->pc = &&aw1; TV_SUSPEND; } ; if (g) goto cont2_f; h(F->x); cont2_f:; }\nreturn true;\n    return true;\n\
             F->x = y;\nfor (;;) { if (!f()) { F->pc = &&aw1; TV_SUSPEND; } aw1:; if (g) goto cont2; h(F->x); cont2:; }\nreturn true;\n"
        );
        // (an `await` outside loops: one copy)
        let (_, b, _) = lowered("if (!f()) { F->pc = &&aw1; TV_SUSPEND; } aw1:;\nfor (;;) { h(); }\n", &[]);
        assert_eq!(b, "if (!f()) { F->pc = &&aw1; TV_SUSPEND; } aw1:;\nfor (;;) { h(); }\n");
    }

    #[test]
    fn rejects_what_it_cannot_move() {
        assert!(lower("char buf[16];\n", &[]).is_err());
        assert!(lower("tv_int a = 1;\n{ double a = 2; }\n", &[]).is_err());
    }
}
