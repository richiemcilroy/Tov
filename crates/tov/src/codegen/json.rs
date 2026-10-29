//! `JSON.stringify` / `JSON.parse`: a writer and a validating parser generated per type.
//!
//! Writers follow `JSON.stringify`: record fields in declaration order, `undefined` fields
//! omitted (and `null` in arrays), `NaN`/`Infinity` as `null`, pretty printing with an indent.
//! Maps with string keys are written as objects. Parsers check the input against the target type
//! (exact integers, string literals, discriminants, required fields) and fail with a message; extra
//! object keys are ignored, and `null` or a missing key reads as `undefined`.

use super::{c_string, Gen, Val};
use crate::types::*;
use std::fmt::Write;

impl<'c, 'a> Gen<'c, 'a> {
    /// `JSON.stringify(v, undefined, indent)`: an owned string.
    pub(crate) fn json_stringify(&mut self, v: Val, indent: Option<Val>) -> Val {
        let w = self.json_writer(v.ty);
        let ind = match indent {
            None => "TV_EMPTY_STR".to_string(),
            Some(iv) => match self.tget(iv.ty) {
                Ty::Str | Ty::StrLit(_) => {
                    let s = self.coerce(iv, STR);
                    s.code
                }
                _ => {
                    // A number of spaces (capped at 10, as in JavaScript).
                    let n = self.int_code(iv);
                    let t = self.tmp(STR, &format!("tvg_js_spaces({n})"), true);
                    t.code
                }
            },
        };
        let sbv = self.fresh("sb");
        let ct = self.ctype(v.ty);
        let pv = self.fresh("jv");
        self.line(format!("tv_sb {sbv} = {{0}}; {ct} {pv} = {};", v.code));
        self.line(format!("{w}(&{sbv}, &{pv}, {ind}, 0);"));
        self.tmp(STR, &format!("tv_str_from_sb(&{sbv})"), true)
    }

    pub(crate) fn json_writer(&mut self, t: TyId) -> String {
        let name = format!("js_{}", t.0);
        if !self.helpers_done.insert((t, 110)) {
            return name;
        }
        let ct = self.ctype(t);
        let _ = writeln!(self.protos, "static void {name}(tv_sb *sb, {ct} const *v, tv_str ind, int lvl);");
        let body = self.json_write_body(t);
        self.helpers_after.push(format!("static void {name}(tv_sb *sb, {ct} const *v, tv_str ind, int lvl) {{\n    (void)ind; (void)lvl;\n{body}}}\n"));
        name
    }

    /// Code writing `*v` (of type `t`).
    fn json_write_body(&mut self, t: TyId) -> String {
        match self.tget(t) {
            Ty::Int | Ty::I8 | Ty::I16 | Ty::I32 | Ty::U8 | Ty::U8C | Ty::U16 | Ty::U32 => "    tv_sb_push_int(sb, (tv_int)*v);\n".into(),
            Ty::U64 => "    tv_json_number(sb, (double)*v);\n".into(),
            Ty::Decimal => "    tv_sb_push_dec(sb, *v);\n".into(),
            Ty::F64 | Ty::F32 => "    tv_json_number(sb, (double)*v);\n".into(),
            Ty::Bool => "    if (*v) tv_sb_add(sb, \"true\", 4); else tv_sb_add(sb, \"false\", 5);\n".into(),
            Ty::Str => "    tv_json_quote(sb, *v);\n".into(),
            Ty::StrLit(s) => {
                let lit = self.lit(self.sym(s).to_string().as_bytes());
                format!("    tv_json_quote(sb, {lit});\n")
            }
            Ty::Undefined | Ty::Null | Ty::Void | Ty::Never | Ty::Error => "    tv_sb_push_cstr(sb, \"null\");\n".into(),
            Ty::Unknown => {
                let c = self.dyn_json_code("*v");
                format!("    {c};\n")
            }
            Ty::Array(e) => {
                let w = self.json_writer(e);
                let ect = self.ctype(e);
                format!(
                    "    tv_int n = tv_arr_len(*v);\n    if (n == 0) {{ tv_sb_push_cstr(sb, \"[]\"); return; }}\n    tv_sb_add_char(sb, '[');\n    for (tv_int i = 0; i < n; i++) {{\n        if (i) tv_sb_add_char(sb, ',');\n        tvg_js_nl(sb, ind, lvl + 1);\n        {w}(sb, &(({ect} *)tv_arr_data(*v))[i], ind, lvl + 1);\n    }}\n    tvg_js_nl(sb, ind, lvl);\n    tv_sb_add_char(sb, ']');\n"
                )
            }
            // (a `Map` of other keys than strings: no object's, and JSON's `{}` as JavaScript's)
            Ty::Map(k, _) if !self.c.types.is_string(k) => "    (void)v; (void)ind; (void)lvl;\n    tv_sb_push_cstr(sb, \"{}\");\n".to_string(),
            Ty::Map(k, val) => {
                let (kd, vd) = (self.desc(k), self.desc(val));
                let w = self.json_writer(val);
                let (kct, vct) = (self.ctype(k), self.ctype(val));
                let skip = self.json_is_undefined(val, &format!("(*({vct} *)vp)"));
                format!(
                    "    tv_int i = 0; void *kp, *vp; bool first = true;\n    tv_sb_add_char(sb, '{{');\n    while (tv_map_next(*v, {kd}, {vd}, &i, &kp, &vp)) {{\n        if ({skip}) continue;\n        if (!first) tv_sb_add_char(sb, ',');\n        first = false;\n        tvg_js_nl(sb, ind, lvl + 1);\n        tv_json_quote(sb, *({kct} *)kp);\n        if (ind.p->len) tv_sb_add(sb, \": \", 2); else tv_sb_add_char(sb, ':');\n        {w}(sb, ({vct} *)vp, ind, lvl + 1);\n    }}\n    if (!first) tvg_js_nl(sb, ind, lvl);\n    tv_sb_add_char(sb, '}}');\n"
                )
            }
            Ty::Record(_) if self.tuple_of(t).is_some() => {
                // a tuple is written as an array
                let fs = self.tuple_of(t).unwrap();
                let mut b = String::from("    tv_sb_add_char(sb, '[');\n");
                for (i, f) in fs.iter().enumerate() {
                    let w = self.json_writer(f.ty);
                    let ct = self.ctype(f.ty);
                    if i > 0 {
                        b.push_str("    tv_sb_add_char(sb, ',');\n");
                    }
                    let place = self.fld(t, "*v", self.sym(f.name));
                    let _ = writeln!(b, "    tvg_js_nl(sb, ind, lvl + 1);\n    {w}(sb, ({ct} const *)&{place}, ind, lvl + 1);");
                }
                b.push_str("    tvg_js_nl(sb, ind, lvl);\n    tv_sb_add_char(sb, ']');\n");
                b
            }
            Ty::Record(_) => {
                let fields = self.c.types.fields_in_order(t);
                let parts: Vec<(String, TyId, String)> = fields.iter().filter(|f| !matches!(self.tget(f.ty), Ty::Func(..))).map(|f| (self.sym(f.name).to_string(), f.ty, self.fld(t, "*v", self.sym(f.name)))).collect();
                self.json_write_object(&parts)
            }
            // `toJSON()`: what it returns is written instead
            Ty::Class(..) if let Some(r) = self.c.to_json_ret(t) => {
                let sym = self.c.interner.lookup("toJSON").unwrap();
                let call = self.method_call_code("(*v)", t, sym, false, Vec::new());
                let (rct, w) = (self.ctype(r), self.json_writer(r));
                let rel = if self.is_rc(r) { self.release_code(r, "j") } else { String::new() };
                format!("    {rct} j = {call};\n    {w}(sb, &j, ind, lvl);\n    {rel};\n")
            }
            Ty::Class(..) => {
                let fields = self.c.class_as_fields(t);
                let mut parts = Vec::new();
                for f in fields {
                    // (functions and `#private` fields aren't JSON's)
                    if matches!(self.tget(f.ty), Ty::Func(..)) || self.sym(f.name).starts_with('#') {
                        continue;
                    }
                    let Some(crate::check::class::ClassMemberRef::Field(cf)) = self.c.class_member(t, f.name) else { continue };
                    let lv = self.class_field_lv_pub("(*v)", t, &cf);
                    parts.push((self.sym(f.name).to_string(), cf.ty, lv));
                }
                self.json_write_object(&parts)
            }
            Ty::Union(ms) if self.niche(t).is_some() => {
                let (pi, _) = self.niche(t).unwrap();
                let m = self.c.types.tys(ms)[pi];
                let w = self.json_writer(m);
                let mct = self.ctype(m);
                let there = self.niche_there("*v", t);
                format!("    if (!{there}) {{ tv_sb_push_cstr(sb, \"null\"); return; }}\n    {mct} x = *v; {w}(sb, &x, ind, lvl);\n")
            }
            Ty::Union(ms) => {
                let ms = self.c.types.tys(ms).to_vec();
                let mut b = String::from("    switch (v->tag) {\n");
                for (i, &m) in ms.iter().enumerate() {
                    if self.is_unit(m) {
                        let body = self.json_write_body(m).replace("*v", "0");
                        let _ = writeln!(b, "    case {i}: {{ {} }} break;", body.trim());
                    } else {
                        let w = self.json_writer(m);
                        // (a class member is `K *const`: the writer takes `const K **`)
                        let mct = self.ctype(m);
                        let _ = writeln!(b, "    case {i}: {w}(sb, ({mct} const *)&v->u.m{i}, ind, lvl); break;");
                    }
                }
                b.push_str("    default: tv_sb_push_cstr(sb, \"null\"); break;\n    }\n");
                b
            }
            Ty::Rec(..) => {
                let inner = self.c.unfold(t);
                let w = self.json_writer(inner);
                format!("    {w}(sb, &(*v)->v, ind, lvl);\n")
            }
            _ => "    tv_sb_push_cstr(sb, \"null\");\n".into(),
        }
    }

    fn json_write_object(&mut self, parts: &[(String, TyId, String)]) -> String {
        if parts.is_empty() {
            return "    tv_sb_push_cstr(sb, \"{}\");\n".into();
        }
        let mut b = String::from("    bool first = true;\n    tv_sb_add_char(sb, '{');\n");
        for (name, ty, lv) in parts {
            // (a record's index-signature keys, its `$rest` map: as its own keys)
            if name == "$rest"
                && let mt = self.c.types.without_undefined(*ty)
                && let Ty::Map(k, v) = self.tget(mt)
            {
                let (kd, vd, vct) = (self.desc(k), self.desc(v), self.ctype(v));
                let w = self.json_writer(v);
                let present = match self.tag_of(*ty, UNDEFINED) {
                    Some(t) => format!("!({})", self.u_is(lv, *ty, t)),
                    None => "true".into(),
                };
                let ml = match self.tag_of(*ty, mt) {
                    Some(t) if mt != *ty => self.u_payload(lv, *ty, t),
                    _ => lv.clone(),
                };
                let skip = self.json_is_undefined(v, &format!("(*({vct} *)vp)"));
                let _ = writeln!(
                    b,
                    "    if ({present}) {{
        tv_int i = 0; void *kp, *vp;
        while (tv_map_next({ml}, {kd}, {vd}, &i, &kp, &vp)) {{
            if ({skip}) continue;
            if (!first) tv_sb_add_char(sb, ',');
            first = false;
            tvg_js_nl(sb, ind, lvl + 1);
            tv_json_quote(sb, *(tv_str *)kp);
            if (ind.p->len) tv_sb_add(sb, \": \", 2); else tv_sb_add_char(sb, ':');
            {w}(sb, ({vct} *)vp, ind, lvl + 1);
        }}
    }}"
                );
                continue;
            }
            let w = self.json_writer(*ty);
            let ct = self.ctype(*ty);
            let key_text = format!("\"{name}\"");
            let key = format!("{}, {}", c_string(key_text.as_bytes()), key_text.len());
            // (compact, the usual: the comma, the key and the colon in one piece)
            let compact_text = format!(",\"{name}\":");
            let compact = format!("{} + first, {} - first", c_string(compact_text.as_bytes()), compact_text.len());
            let skip = self.json_is_undefined(*ty, lv);
            let _ = writeln!(
                b,
                "    if (!({skip})) {{\n        if (__builtin_expect(!ind.p->len, 1)) tv_sb_add(sb, {compact});\n        else {{\n            if (!first) tv_sb_add_char(sb, ',');\n            tvg_js_nl(sb, ind, lvl + 1);\n            tv_sb_add(sb, {key});\n            tv_sb_add(sb, \": \", 2);\n        }}\n        first = false;\n        {w}(sb, ({ct} const *)&{lv}, ind, lvl + 1);\n    }}"
            );
        }
        b.push_str("    if (!first) tvg_js_nl(sb, ind, lvl);\n    tv_sb_add_char(sb, '}');\n");
        b
    }

    /// A C condition: the value at `place` (of type `t`) is `undefined` (omitted from objects).
    fn json_is_undefined(&mut self, t: TyId, place: &str) -> String {
        if t == UNDEFINED {
            return "true".into();
        }
        match self.tag_of(t, UNDEFINED) {
            Some(k) => self.u_is(place, t, k),
            None => "false".into(),
        }
    }

    // ------------------------------------------------------------ parsing

    /// `JSON.parse(text)` into `t`: an owned value; on bad input sets `tvg_err` to a `SyntaxError`.
    pub(crate) fn json_parse(&mut self, text: Val, t: TyId) -> Val {
        let p = self.json_parser(t);
        let jp = self.fresh("jp");
        let d = self.default_value(t);
        let out = self.tmp(t, &d, true);
        self.line(format!("tv_jp {jp}; tv_jp_init(&{jp}, {});", text.code));
        self.open(&format!("if (!({p}(&{jp}, &{}) && tv_jp_end(&{jp}))) {{", out.code));
        let msg = format!("tv_jp_error(&{jp})");
        let err = self.c.builtin_error_class("SyntaxError");
        if err != ERROR {
            let obj = self.error_object(err, &msg);
            self.line(format!("tvg_err = (void *)({obj});"));
        }
        self.close("}");
        out
    }

    fn json_parser(&mut self, t: TyId) -> String {
        let name = format!("jp_{}", t.0);
        if !self.helpers_done.insert((t, 111)) {
            return name;
        }
        let ct = self.ctype(t);
        let _ = writeln!(self.protos, "static bool {name}(tv_jp *p, {ct} *out);");
        let body = self.json_parse_body(t);
        self.helpers_after.push(format!("static bool {name}(tv_jp *p, {ct} *out) {{\n{body}}}\n"));
        name
    }

    /// Code parsing into `*out` (which holds a valid value; replaced on success).
    fn json_parse_body(&mut self, t: TyId) -> String {
        let int_range = |lo: &str, hi: &str| {
            format!(
                "    double d;\n    if (!tv_jp_number(p, &d)) return false;\n    if (d != (double)(int64_t)d || d < {lo} || d > {hi}) return tv_jp_fail(p, \"an integer\");\n    *out = ({}) d;\n    return true;\n",
                "__typeof__(*out)"
            )
        };
        match self.tget(t) {
            Ty::Int => int_range("-9223372036854775808.0", "9223372036854775807.0"),
            Ty::I8 => int_range("-128.0", "127.0"),
            Ty::I16 => int_range("-32768.0", "32767.0"),
            Ty::I32 => int_range("-2147483648.0", "2147483647.0"),
            Ty::U8 | Ty::U8C => int_range("0.0", "255.0"),
            Ty::U16 => int_range("0.0", "65535.0"),
            Ty::U32 => int_range("0.0", "4294967295.0"),
            Ty::U64 => int_range("0.0", "18446744073709551615.0"),
            Ty::F64 | Ty::F32 => "    double d;\n    if (!tv_jp_number(p, &d)) return false;\n    *out = (__typeof__(*out))d;\n    return true;\n".into(),
            Ty::Decimal => "    return tv_jp_decimal(p, out);\n".into(),
            Ty::Bool => "    char c = tv_jp_peek(p);\n    if (c == 't') { if (!tv_jp_word(p, \"true\")) return false; *out = true; return true; }\n    if (c == 'f') { if (!tv_jp_word(p, \"false\")) return false; *out = false; return true; }\n    return tv_jp_fail(p, \"a boolean\");\n".into(),
            Ty::Str => "    tv_str s;\n    if (!tv_jp_string(p, &s)) return false;\n    tv_str_release(*out);\n    *out = s;\n    return true;\n".into(),
            // (`any`, `unknown`: JSON's value as itself, its objects maps of their keys)
            Ty::Unknown => "    tv_dyn d;\n    if (!tv_jp_dyn(p, &d)) return false;\n    tv_dyn_release(*out);\n    *out = d;\n    return true;\n".into(),
            Ty::StrLit(s) => {
                let text = self.sym(s).to_string();
                let want = c_string(format!("\"{text}\"").as_bytes());
                format!(
                    "    tv_str s;\n    if (!tv_jp_string(p, &s)) return false;\n    bool ok = s.p->len == {} && memcmp(s.p->data, {}, {}) == 0;\n    tv_str_release(s);\n    return ok ? true : tv_jp_fail(p, {want});\n",
                    text.len(),
                    c_string(text.as_bytes()),
                    text.len()
                )
            }
            // (a value nothing reads, `JSON.parse(text);`: any, checked to be JSON)
            Ty::Void => "    (void)out;\n    return tv_jp_skip(p);\n".into(),
            Ty::Undefined | Ty::Null => "    return tv_jp_word(p, \"null\");\n".into(),
            Ty::Array(e) => {
                let pe = self.json_parser(e);
                let (ect, d, ed) = (self.ctype(e), self.desc(e), self.default_value(e));
                let rel = self.release_code(t, "*out");
                let rel_e = self.release_code(e, "x");
                format!(
                    "    if (!tv_jp_char(p, '[')) return false;\n    tv_arr a = tv_arr_fresh();\n    if (!tv_jp_try_char(p, ']')) {{\n        do {{\n            {ect} x = {ed};\n            if (!{pe}(p, &x)) {{ {rel_e}; tv_arr_release(a, {d}); return false; }}\n            TVG_PUSH({ect}, &a, {d}, x);\n        }} while (tv_jp_try_char(p, ','));\n        if (!tv_jp_char(p, ']')) {{ tv_arr_release(a, {d}); return false; }}\n    }}\n    {rel};\n    *out = a;\n    return true;\n"
                )
            }
            Ty::Map(k, v) => {
                let pv = self.json_parser(v);
                let (kd, vd, vct, dv) = (self.desc(k), self.desc(v), self.ctype(v), self.default_value(v));
                let rel_v = self.release_code(v, "x");
                let rel = self.release_code(t, "*out");
                format!(
                    "    if (!tv_jp_char(p, '{{')) return false;\n    tv_map m = tv_map_fresh_object();\n    if (!tv_jp_try_char(p, '}}')) {{\n        do {{\n            tv_str k;\n            if (!tv_jp_string(p, &k)) {{ tv_map_release(m, {kd}, {vd}); return false; }}\n            if (!tv_jp_char(p, ':')) {{ tv_str_release(k); tv_map_release(m, {kd}, {vd}); return false; }}\n            {vct} x = {dv};\n            if (!{pv}(p, &x)) {{ tv_str_release(k); {rel_v}; tv_map_release(m, {kd}, {vd}); return false; }}\n            tv_map_set(&m, {kd}, {vd}, &k, &x);\n        }} while (tv_jp_try_char(p, ','));\n        if (!tv_jp_char(p, '}}')) {{ tv_map_release(m, {kd}, {vd}); return false; }}\n    }}\n    {rel};\n    *out = m;\n    return true;\n"
                )
            }
            Ty::Record(_) if self.tuple_of(t).is_some() => {
                // a tuple is read from an array of exactly its length
                let fs = self.tuple_of(t).unwrap();
                let mut b = String::from("    if (!tv_jp_char(p, '[')) return false;\n");
                for (i, f) in fs.iter().enumerate() {
                    let pf = self.json_parser(f.ty);
                    if i > 0 {
                        b.push_str("    if (!tv_jp_char(p, ',')) return false;\n");
                    }
                    let place = self.fld(t, "*out", self.sym(f.name));
                    let _ = writeln!(b, "    if (!{pf}(p, &{place})) return false;");
                }
                b.push_str("    if (!tv_jp_char(p, ']')) return false;\n    return true;\n");
                b
            }
            Ty::Record(fs) => {
                let fs = self.c.types.fields(fs).to_vec();
                let mut b = String::from("    if (!tv_jp_char(p, '{')) return false;\n");
                for (i, _) in fs.iter().enumerate() {
                    let _ = writeln!(b, "    bool seen{i} = false;");
                }
                b.push_str("    if (!tv_jp_try_char(p, '}')) {\n        do {\n            tv_jp_k k;\n            if (!tv_jp_key(p, &k)) return false;\n            if (!tv_jp_char(p, ':')) { tv_jp_key_done(&k); return false; }\n            ");
                // (an index signature's other keys: into `$rest`, made the first time)
                let mut other = "{ tv_jp_key_done(&k); if (!tv_jp_skip(p)) return false; }".to_string();
                if let Some(rf) = fs.iter().find(|f| self.sym(f.name) == "$rest")
                    && let mt = self.c.types.without_undefined(rf.ty)
                    && let Ty::Map(mk, mv) = self.tget(mt)
                    && self.c.types.is_string(mk)
                {
                    let rt = rf.ty;
                    let place = self.fld(t, "*out", "$rest");
                    let (present, ml, make) = match (self.tag_of(rt, UNDEFINED), self.tag_of(rt, mt)) {
                        (Some(u), Some(k)) if mt != rt => (format!("!({})", self.u_is(&place, rt, u)), self.u_payload(&place, rt, k), self.u_make(rt, k, Some("tv_map_fresh_object()"))),
                        _ => ("true".to_string(), place.clone(), "tv_map_fresh_object()".to_string()),
                    };
                    let pv = self.json_parser(mv);
                    let (vct, vd, dv) = (self.ctype(mv), self.desc(mv), self.default_value(mv));
                    let rel = self.release_code(mv, "x");
                    other = format!("{{ if (!({present})) {place} = {make}; {vct} x = {dv}; if (!{pv}(p, &x)) {{ {rel}; tv_jp_key_done(&k); return false; }} tv_str ks = tv_jp_key_str(&k); tv_map_set(&{ml}, &tv_type_str, {vd}, &ks, &x); }}");
                }
                for (i, f) in fs.iter().enumerate() {
                    let n = self.sym(f.name).to_string();
                    if n == "$rest" {
                        continue;
                    }
                    let pf = self.json_parser(f.ty);
                    let place = self.fld(t, "*out", &n);
                    let _ = write!(
                        b,
                        "if (k.n == {} && memcmp(k.s, {}, {}) == 0) {{ tv_jp_key_done(&k); if (!{pf}(p, &{place})) return false; seen{i} = true; }}\n            else ",
                        n.len(),
                        c_string(n.as_bytes()),
                        n.len()
                    );
                }
                b.push_str(&other);
                b.push_str("\n        } while (tv_jp_try_char(p, ','));\n        if (!tv_jp_char(p, '}')) return false;\n    }\n");
                for (i, f) in fs.iter().enumerate() {
                    if !f.optional && !self.c.types.has_undefined(f.ty) {
                        let n = self.sym(f.name);
                        let msg = c_string(format!("the field \"{n}\"").as_bytes());
                        let _ = writeln!(b, "    if (!seen{i}) return tv_jp_fail(p, {msg});");
                    }
                }
                b.push_str("    return true;\n");
                b
            }
            Ty::Union(ms) => {
                let ms: Vec<TyId> = self.c.types.tys(ms).to_vec();
                self.json_parse_union(t, &ms)
            }
            Ty::Rec(..) => {
                let inner = self.c.unfold(t);
                let pi = self.json_parser(inner);
                let bn = self.box_name(t);
                let (ict, di) = (self.ctype(inner), self.default_value(inner));
                let rel_i = self.release_code(inner, "x");
                let rel = self.release_code(t, "*out");
                format!(
                    "    {ict} x = {di};\n    if (!{pi}(p, &x)) {{ {rel_i}; return false; }}\n    {bn} *b = tvg_alloc_small(sizeof({bn})); RC_{bn}(b) = 1; b->v = x;\n    {rel};\n    *out = b;\n    return true;\n"
                )
            }
            _ => "    return tv_jp_fail(p, \"a supported value\");\n".into(),
        }
    }

    /// A union: `null` for `undefined`, discriminated records by their `kind`-like field,
    /// otherwise the member chosen by the value's first character.
    fn json_parse_union(&mut self, t: TyId, ms: &[TyId]) -> String {
        let mut b = String::from("    char c = tv_jp_peek(p);\n");
        let set = |g: &mut Self, k: usize, m: TyId, b: &mut String| {
            let pm = g.json_parser(m);
            let (mct, dm) = (g.ctype(m), g.default_value(m));
            let rel_m = g.release_code(m, "x");
            let rel = g.release_code(t, "*out");
            let make = g.u_make(t, k, Some("x"));
            let _ = write!(b, "{{ {mct} x = {dm}; if (!{pm}(p, &x)) {{ {rel_m}; return false; }} {rel}; *out = {make}; return true; }}");
        };
        // `null` (into `undefined` when the type has no `null`)
        if let Some(k) = ms.iter().position(|&m| m == NULL).or_else(|| ms.iter().position(|&m| m == UNDEFINED)) {
            let rel = self.release_code(t, "*out");
            let make = self.u_make(t, k, None);
            let _ = writeln!(b, "    if (c == 'n') {{ if (!tv_jp_word(p, \"null\")) return false; {rel}; *out = {make}; return true; }}");
        }
        // Records: by discriminant when there are several.
        let mut records: Vec<(usize, TyId)> = Vec::new();
        for (i, &m) in ms.iter().enumerate() {
            let u = self.c.unfold(m);
            if matches!(self.tget(u), Ty::Record(_)) || matches!(self.tget(m), Ty::Map(..)) {
                records.push((i, m));
            }
        }
        if !records.is_empty() {
            b.push_str("    if (c == '{') ");
            if records.len() == 1 {
                let (k, m) = records[0];
                set(self, k, m, &mut b);
                b.push('\n');
            } else {
                // Find a string-literal field every record has.
                let disc = self.discriminant(&records.iter().map(|r| r.1).collect::<Vec<_>>());
                match disc {
                    Some((field, lits)) => {
                        let fname = c_string(self.sym(field).as_bytes());
                        let _ = write!(b, "{{\n        tv_str dv;\n        if (!tv_jp_find_key(p, {fname}, &dv)) return tv_jp_fail(p, {});\n", c_string(format!("a \"{}\" field", self.sym(field)).as_bytes()));
                        for ((k, m), lit) in records.iter().zip(lits) {
                            let l = self.sym(lit).to_string();
                            let _ = write!(b, "        if (dv.p->len == {} && memcmp(dv.p->data, {}, {}) == 0) {{ tv_str_release(dv); ", l.len(), c_string(l.as_bytes()), l.len());
                            set(self, *k, *m, &mut b);
                            b.push_str(" }\n");
                        }
                        let _ = writeln!(b, "        tv_str_release(dv);\n        return tv_jp_fail(p, {});\n    }}", c_string(format!("a known \"{}\"", self.sym(field)).as_bytes()));
                    }
                    None => {
                        let (k, m) = records[0];
                        set(self, k, m, &mut b);
                        b.push('\n');
                    }
                }
            }
        }
        // By first character.
        let mut string_members = Vec::new();
        let mut numeric = Vec::new();
        for (k, &m) in ms.iter().enumerate() {
            match self.tget(m) {
                Ty::Str | Ty::StrLit(_) => string_members.push((k, m)),
                Ty::Array(_) => {
                    b.push_str("    if (c == '[') ");
                    set(self, k, m, &mut b);
                    b.push('\n');
                }
                Ty::Bool => {
                    b.push_str("    if (c == 't' || c == 'f') ");
                    set(self, k, m, &mut b);
                    b.push('\n');
                }
                _ if self.c.types.is_numeric(m) => numeric.push((k, m)),
                _ => {}
            }
        }
        // A number: with integer and floating-point members (`int | f64`), a whole number is the
        // integer's, any other the floating point's.
        let int = numeric.iter().find(|(_, m)| self.c.types.is_int(*m)).copied();
        let float = numeric.iter().find(|(_, m)| self.c.types.is_float(*m)).copied();
        if let (Some((ki, mi)), Some((kf, mf))) = (int, float) {
            b.push_str("    if (c == '-' || (c >= '0' && c <= '9')) {\n        size_t at = p->i;\n        double d;\n        if (!tv_jp_number(p, &d)) return false;\n        p->i = at;\n        if (d == (double)(int64_t)d) ");
            set(self, ki, mi, &mut b);
            b.push_str("\n        ");
            set(self, kf, mf, &mut b);
            b.push_str("\n    }\n");
        } else if let Some(&(k, m)) = numeric.first() {
            b.push_str("    if (c == '-' || (c >= '0' && c <= '9')) ");
            set(self, k, m, &mut b);
            b.push('\n');
        }
        if !string_members.is_empty() {
            // A string: an exact literal member, else the `string` member.
            b.push_str("    if (c == '\"') {\n        size_t at = p->i;\n");
            let general = string_members.iter().find(|(_, m)| self.tget(*m) == Ty::Str).copied();
            for &(k, m) in &string_members {
                if let Ty::StrLit(s) = self.tget(m) {
                    let l = self.sym(s).to_string();
                    let rel = self.release_code(t, "*out");
                    let make = self.u_make(t, k, None);
                    let _ = writeln!(b, "        {{ tv_str s; if (!tv_jp_string(p, &s)) return false; bool hit = s.p->len == {} && memcmp(s.p->data, {}, {}) == 0; tv_str_release(s); if (hit) {{ {rel}; *out = {make}; return true; }} p->i = at; }}", l.len(), c_string(l.as_bytes()), l.len());
                }
            }
            match general {
                Some((k, m)) => {
                    b.push_str("        ");
                    set(self, k, m, &mut b);
                    b.push('\n');
                }
                None => {
                    let opts: Vec<String> = string_members.iter().filter_map(|(_, m)| if let Ty::StrLit(s) = self.tget(*m) { Some(format!("\"{}\"", self.sym(s))) } else { None }).collect();
                    let _ = writeln!(b, "        return tv_jp_fail(p, {});", c_string(format!("one of {}", opts.join(", ")).as_bytes()));
                }
            }
            b.push_str("    }\n");
        }
        b.push_str("    return tv_jp_fail(p, \"a value of the expected type\");\n");
        b
    }

    /// A field every record has with a distinct string literal (like `kind`).
    fn discriminant(&mut self, records: &[TyId]) -> Option<(crate::intern::Sym, Vec<crate::intern::Sym>)> {
        let first = self.c.unfold(records[0]);
        let Ty::Record(fs) = self.tget(first) else { return None };
        for f in self.c.types.fields(fs).to_vec() {
            let mut lits = Vec::new();
            for &r in records {
                let u = self.c.unfold(r);
                let Ty::Record(rfs) = self.tget(u) else { return None };
                match self.c.types.fields(rfs).iter().find(|x| x.name == f.name).map(|x| self.tget(x.ty)) {
                    Some(Ty::StrLit(l)) if !lits.contains(&l) => lits.push(l),
                    _ => break,
                }
            }
            if lits.len() == records.len() {
                return Some((f.name, lits));
            }
        }
        None
    }
}
