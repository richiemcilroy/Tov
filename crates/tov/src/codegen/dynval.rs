//! `unknown` values: a value of any type with its descriptor, boxed (see runtime `tv_dyn`).
//! Every type a program puts into an `unknown` is noted (`dyn_types`), so what JavaScript asks of
//! one (`typeof`, `instanceof`, `JSON.stringify`) is answered by a switch over those types.

use super::{c_string, Gen, Val};
use crate::types::*;
use std::fmt::Write;

impl<'c, 'a> Gen<'c, 'a> {
    /// `v` as an `unknown`: boxed with its type's descriptor (a union: its member's).
    pub(crate) fn box_dyn(&mut self, v: Val) -> Val {
        let from = v.ty;
        match self.tget(from) {
            // (an `unknown` or `any` already: itself)
            Ty::Unknown => return Val { ty: UNKNOWN, ..v },
            Ty::Undefined | Ty::Void | Ty::Never => return Val::plain("((tv_dyn){&tv_type_undefined, NULL})", UNKNOWN),
            Ty::Null => return Val::plain("((tv_dyn){&tv_type_null, NULL})", UNKNOWN),
            Ty::StrLit(_) => {
                let s = self.coerce(v, STR);
                return self.box_dyn(s);
            }
            Ty::BoolLit(_) => {
                let b = self.coerce(v, BOOL);
                return self.box_dyn(b);
            }
            // (a recursive alias: what it holds, boxed as that, `typeof` and all)
            Ty::Rec(..) => {
                let owned = v.owned;
                let rel = if owned { Some((v.code.clone(), v.ty)) } else { None };
                let u = self.unfold_val(Val { owned: false, ..v });
                let u = self.own(u);
                let d = self.box_dyn(u);
                if let Some((code, t)) = rel {
                    let r = self.release_code(t, &code);
                    if !r.is_empty() {
                        self.line(format!("{r};"));
                    }
                }
                return d;
            }
            // (a union kept as its one member, `undefined` its niche: that member's box, or undefined)
            Ty::Union(ms) if self.niche(from).is_some() => {
                let (pi, _) = self.niche(from).unwrap();
                let m = self.c.types.tys(ms)[pi];
                let v = self.own(v);
                let ct = self.ctype(from);
                let u = self.fresh("u");
                let code = self.consume(v);
                self.line(format!("{ct} {u} = {code};"));
                let there = self.niche_there(&u, from);
                let r = self.fresh("d");
                self.line(format!("tv_dyn {r};"));
                self.open(&format!("if ({there}) {{"));
                let bv = self.box_dyn(Val { code: u.clone(), ty: m, owned: true });
                let bc = self.consume(bv);
                self.line(format!("{r} = {bc};"));
                self.close(&format!("}} else {r} = (tv_dyn){{&tv_type_undefined, NULL}};"));
                return self.tmp(UNKNOWN, &r, true);
            }
            Ty::Union(ms) if self.niche(from).is_none() => {
                // (the member it holds, boxed as that)
                let ms = self.c.types.tys(ms).to_vec();
                let v = self.own(v);
                let ct = self.ctype(from);
                let u = self.fresh("u");
                let code = self.consume(v);
                self.line(format!("{ct} {u} = {code};"));
                let r = self.fresh("d");
                self.line(format!("tv_dyn {r};"));
                self.line(format!("switch ({u}.tag) {{"));
                for (i, &m) in ms.iter().enumerate() {
                    let payload = if self.is_unit(m) { "0".to_string() } else { format!("{u}.u.m{i}") };
                    self.line(format!("case {i}: {{"));
                    let bv = self.box_dyn(Val { code: payload, ty: m, owned: true });
                    let bc = self.consume(bv);
                    self.line(format!("{r} = {bc}; break; }}"));
                }
                self.line(format!("default: {r} = (tv_dyn){{&tv_type_undefined, NULL}}; break;\n}}"));
                return self.tmp(UNKNOWN, &r, true);
            }
            _ => {}
        }
        if !self.dyn_types.contains(&from) {
            self.dyn_types.push(from);
        }
        // (its class id may be read: `x instanceof C`)
        if let Ty::Class(d, _) = self.tget(from) {
            self.cid_decls.insert(d);
        }
        let v = self.own(v);
        let ct = self.ctype(from);
        let d = self.desc(from);
        let code = self.consume(v);
        let x = self.fresh("bx");
        self.line(format!("{ct} {x} = {code};"));
        self.tmp(UNKNOWN, &format!("tv_dyn_box({d}, &{x})"), true)
    }

    /// An `unknown` narrowed to `to` (`typeof x === "number"`, `x instanceof C`): the value it
    /// holds, as one (what it is was tested).
    pub(crate) fn unbox_dyn(&mut self, v: Val, to: TyId) -> Val {
        let d = self.fresh("dv");
        self.line(format!("tv_dyn {d} = {};", v.code));
        if let Ty::Union(ms) = self.tget(to) {
            // (the member it holds; one of them, as tested)
            let ms = self.c.types.tys(ms).to_vec();
            let ct = self.ctype(to);
            let r = self.fresh("ud");
            let dv = self.default_value(to);
            self.line(format!("{ct} {r} = {dv};"));
            for (i, &m) in ms.iter().enumerate() {
                let test = self.dyn_is(&d, m);
                self.open(&format!("{}if ({test}) {{", if i == 0 { "" } else { "else " }));
                self.push_temps_pub();
                let mv = self.unbox_dyn(Val::plain(d.clone(), UNKNOWN), m);
                let uv = self.coerce(mv, to);
                let code = self.consume(uv);
                self.line(format!("{r} = {code};"));
                self.pop_temps_pub();
                self.close("}");
            }
            return self.tmp(to, &r, true);
        }
        match self.tget(to) {
            Ty::F64 | Ty::F32 | Ty::Int => {
                let code = format!("tv_dyn_number({d})");
                let n = Val::plain(code, F64);
                self.coerce(n, to)
            }
            Ty::Str => self.tmp(STR, &format!("({{ tv_str s_ = *(tv_str *)tv_dyn_data({d}); tv_str_retain(s_); s_; }})"), true),
            // (`false` of `string | false`: the one it was tested to be)
            Ty::Bool => Val::plain(format!("(*(bool *)tv_dyn_data({d}))"), to),
            Ty::Undefined | Ty::Null | Ty::Void => Val::plain("0", to),
            _ => {
                let ct = self.ctype(to);
                let rc = self.is_rc(to);
                let get = format!("(*({ct} *)tv_dyn_data({d}))");
                if rc {
                    let r = self.retain_code(to, "v_");
                    self.tmp(to, &format!("({{ {ct} v_ = {get}; {r}; v_; }})"), true)
                } else {
                    Val::plain(get, to)
                }
            }
        }
    }

    /// Code asks an `unknown` what it holds: the switches over the program's types are made.
    pub(crate) fn dyn_used(&mut self) {
        if !self.dyn_tables {
            self.dyn_tables = true;
            let _ = writeln!(self.protos, "static tv_str tvg_dyn_typeof(tv_dyn d);\nstatic void tvg_dyn_json(tv_sb *sb, const tv_dyn *v, tv_str ind, int lvl);\nstatic void *tvg_dyn_obj(tv_dyn d);\nstatic bool tvg_dyn_is_array(tv_dyn d);\nstatic bool tvg_dyn_has(tv_dyn d, tv_str key);");
        }
    }

    /// C condition: the `unknown` `d` (a C name) holds a `to`.
    pub(crate) fn dyn_is(&mut self, d: &str, to: TyId) -> String {
        match self.tget(to) {
            _ if self.c.types.is_numeric(to) => format!("tv_dyn_is_num({d}.t)"),
            Ty::Str | Ty::StrLit(_) => format!("({d}.t == &tv_type_str)"),
            Ty::Bool if let Ty::BoolLit(b) = *self.c.types.get(to) => format!("({d}.t == &tv_type_bool && *(bool *)tv_dyn_data({d}) == {b})"),
            Ty::Bool => format!("({d}.t == &tv_type_bool)"),
            Ty::Undefined | Ty::Void => format!("({d}.t == &tv_type_undefined)"),
            Ty::Null => format!("({d}.t == &tv_type_null)"),
            Ty::Unknown => "true".into(),
            Ty::Class(c, _) => {
                let isa = self.instanceof_code(Val::plain(d.to_string(), UNKNOWN), c);
                format!("({isa})")
            }
            Ty::Union(ms) => {
                let ms = self.c.types.tys(ms).to_vec();
                let parts: Vec<String> = ms.iter().map(|&m| self.dyn_is(d, m)).collect();
                format!("({})", parts.join(" || "))
            }
            _ => {
                let desc = self.desc(to);
                format!("({d}.t == {desc})")
            }
        }
    }

    /// `x as T` of an `unknown`: what it holds, or a trap if that isn't a `T`.
    pub(crate) fn dyn_cast(&mut self, v: Val, to: TyId, loc: &str) -> Val {
        let d = self.fresh("dv");
        self.line(format!("tv_dyn {d} = {};", v.code));
        let ok = self.dyn_is(&d, to);
        self.line(format!("if (!{ok}) tv_trap(\"`as` cast failed: the value is of a different type\", {loc});"));
        self.unbox_dyn(Val::plain(d, UNKNOWN), to)
    }

    /// `typeof x` of an `unknown`.
    pub(crate) fn dyn_typeof(&mut self, v: Val) -> Val {
        self.dyn_used();
        self.tmp(STR, &format!("tvg_dyn_typeof({})", v.code), true)
    }

    /// `JSON.stringify` of an `unknown`, into `sb`.
    pub(crate) fn dyn_json_code(&mut self, place: &str) -> String {
        self.dyn_used();
        format!("tvg_dyn_json(sb, &({place}), ind, lvl)")
    }

    /// `Array.isArray` of an `unknown`.
    pub(crate) fn dyn_is_array(&mut self, code: &str) -> String {
        self.dyn_used();
        format!("tvg_dyn_is_array({code})")
    }

    /// The class instance an `unknown` holds (`NULL` if it holds none).
    pub(crate) fn dyn_object(&mut self, code: &str) -> String {
        self.dyn_used();
        format!("tvg_dyn_obj({code})")
    }

    /// The program's switches over the types put into `unknown`s (after everything else is made,
    /// so every such type is known).
    pub(crate) fn dyn_dispatch(&mut self) {
        if !self.dyn_tables {
            return;
        }
        let types = self.dyn_types.clone();
        let mut kinds = String::new();
        let mut json = String::new();
        let mut objs = String::new();
        let mut arrays = String::new();
        let mut has = String::new();
        for &t in &types {
            let d = self.desc(t);
            let kind = match self.tget(t) {
                _ if self.c.types.is_numeric(t) => "number",
                Ty::Str | Ty::StrLit(_) => "string",
                Ty::Bool | Ty::BoolLit(_) => "boolean",
                Ty::BigInt => "bigint",
                Ty::Func(..) => "function",
                _ => "object",
            };
            let _ = writeln!(kinds, "    if (d.t == {d}) return {};", self.lit(kind.as_bytes()));
            if !matches!(self.tget(t), Ty::Func(..) | Ty::Promise(..) | Ty::Gen(_) | Ty::AsyncGen(_)) {
                let w = self.json_writer(t);
                let ct = self.ctype(t);
                let _ = writeln!(json, "    if (v->t == {d}) {{ {w}(sb, ({ct} const *)tv_dyn_data(*v), ind, lvl); return; }}");
            }
            if matches!(self.tget(t), Ty::Class(..)) {
                let _ = writeln!(objs, "    if (d.t == {d}) return *(void **)tv_dyn_data(d);");
            }
            if matches!(self.tget(t), Ty::Array(_)) {
                let _ = writeln!(arrays, "    if (d.t == {d}) return true;");
            }
            // (`"k" in x`: a record's or class's names, a map's keys)
            let names: Vec<String> = match self.tget(t) {
                Ty::Record(fs) => self.c.types.fields(fs).iter().map(|f| self.sym(f.name).to_string()).collect(),
                Ty::Class(..) => self.c.class_member_names(t),
                _ => Vec::new(),
            };
            if let Ty::Map(k, mv) = self.tget(t)
                && k == STR
            {
                let (kd, vd) = (self.desc(k), self.desc(mv));
                let _ = writeln!(has, "    if (d.t == {d}) return tv_map_is_object(*(tv_map *)tv_dyn_data(d)) && tv_map_has(*(tv_map *)tv_dyn_data(d), {kd}, {vd}, &key);");
            } else if !names.is_empty() {
                let tests: Vec<String> = names.iter().map(|n| format!("tv_str_eq(key, {})", self.lit(n.as_bytes()))).collect();
                let _ = writeln!(has, "    if (d.t == {d}) return {};", tests.join(" || "));
            }
        }
        // (JSON's arrays and objects, `any`'s: of `unknown`s)
        let _ = writeln!(json, "    if (v->t == &tv_type_dynarr) {{ tv_arr a = *(const tv_arr *)tv_dyn_data(*v); tv_int n = tv_arr_len(a); if (n == 0) {{ tv_sb_push_cstr(sb, \"[]\"); return; }} tv_sb_add_char(sb, '['); for (tv_int i = 0; i < n; i++) {{ if (i) tv_sb_add_char(sb, ','); tvg_js_nl(sb, ind, lvl + 1); const tv_dyn *e = &((const tv_dyn *)tv_arr_data(a))[i]; if (e->t == &tv_type_undefined) tv_sb_push_cstr(sb, \"null\"); else tvg_dyn_json(sb, e, ind, lvl + 1); }} tvg_js_nl(sb, ind, lvl); tv_sb_add_char(sb, ']'); return; }}");
        let _ = writeln!(json, "    if (v->t == &tv_type_dynobj) {{ tv_map m = *(const tv_map *)tv_dyn_data(*v); tv_int i = 0; void *kp, *vp; bool first = true; tv_sb_add_char(sb, '{{'); while (tv_map_next(m, &tv_type_str, &tv_type_dyn, &i, &kp, &vp)) {{ const tv_dyn *e = vp; if (e->t == &tv_type_undefined) continue; if (!first) tv_sb_add_char(sb, ','); first = false; tvg_js_nl(sb, ind, lvl + 1); tv_json_quote(sb, *(tv_str *)kp); if (ind.p->len) tv_sb_add(sb, \": \", 2); else tv_sb_add_char(sb, ':'); tvg_dyn_json(sb, e, ind, lvl + 1); }} if (!first) tvg_js_nl(sb, ind, lvl); tv_sb_add_char(sb, '}}'); return; }}");
        let _ = writeln!(json, "    if (v->t == &tv_type_str) {{ tv_json_quote(sb, *(const tv_str *)tv_dyn_data(*v)); return; }}");
        let _ = writeln!(json, "    if (tv_dyn_is_num(v->t)) {{ tv_json_number(sb, tv_dyn_number(*v)); return; }}");
        let _ = writeln!(json, "    if (v->t == &tv_type_bool) {{ if (*(const bool *)tv_dyn_data(*v)) tv_sb_add(sb, \"true\", 4); else tv_sb_add(sb, \"false\", 5); return; }}");
        let _ = writeln!(arrays, "    if (d.t == &tv_type_dynarr) return true;");
        let _ = writeln!(has, "    if (d.t == &tv_type_dynobj) return tv_map_has(*(tv_map *)tv_dyn_data(d), &tv_type_str, &tv_type_dyn, &key);");
        let (u, n) = (self.lit(b"undefined"), self.lit(b"object"));
        let code = format!(
            "static tv_str tvg_dyn_typeof(tv_dyn d) {{\n    if (d.t == &tv_type_undefined) return {u};\n    if (d.t == &tv_type_null) return {n};\n{kinds}    return {n};\n}}\n\
             static void tvg_dyn_json(tv_sb *sb, const tv_dyn *v, tv_str ind, int lvl) {{\n    (void)ind; (void)lvl;\n{json}    tv_sb_push_cstr(sb, {});\n}}\n\
             static void *tvg_dyn_obj(tv_dyn d) {{\n{objs}    (void)d;\n    return NULL;\n}}\n\
             static bool tvg_dyn_is_array(tv_dyn d) {{\n{arrays}    (void)d;\n    return false;\n}}\n\
             static bool tvg_dyn_has(tv_dyn d, tv_str key) {{\n{has}    (void)d; (void)key;\n    return false;\n}}\n",
            c_string(b"null")
        );
        self.helpers_after.push(code);
    }
}
