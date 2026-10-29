/* test_runtime.c — standalone tests for the Tov C runtime.
 *
 *   cc -std=c11 -g -fsanitize=address,undefined runtime/test_runtime.c -o /tmp/t -lm && /tmp/t
 *   /tmp/t trap        # exits 101 with "trap: ..." on stderr (checked by runtime/test.sh)
 *
 * Includes tov.c directly (like generated programs do), so internals are reachable.
 * Element ownership is checked with a counting tv_type ("obj") whose retain/release
 * calls are tallied per object: after each group every object must be dead.
 */
#define _GNU_SOURCE 1      /* accept4 (glibc), as tov.c asks for */
#define _DEFAULT_SOURCE 1   /* MAP_ANON (glibc) */
#define _DARWIN_C_SOURCE 1  /* MAP_ANON (macOS) */
#include "tov.h"
#include "tov.c"
#include "big.c"   /* (dec.c's limb arithmetic) */
#include "dec.c"   /* (tov.c's JSON reads decimals) */

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

/* ================================================================== harness */

static int checks, failures;

#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
        }                                                                         \
    } while (0)

/* Compares and RELEASES s. */
static void check_str_(int line, tv_str s, const char *want, size_t wn) {
    checks++;
    if ((size_t)s.p->len != wn || memcmp(s.p->data, want, wn) != 0 || s.p->data[s.p->len] != 0) {
        failures++;
        fprintf(stderr, "%s:%d: got \"%.*s\", want \"%.*s\"\n", __FILE__, line, (int)s.p->len, s.p->data, (int)wn,
                want);
    }
    tv_str_release(s);
}
#define CHECK_STR(s, want) check_str_(__LINE__, (s), (want), sizeof(want) - 1)
#define CHECK_CSTR(s, want) check_str_(__LINE__, (s), (want), strlen(want))

/* Compares and frees sb. */
static void check_sb_(int line, tv_sb *sb, const char *want) {
    checks++;
    size_t wn = strlen(want);
    if (sb->len != wn || memcmp(sb->data, want, wn) != 0) {
        failures++;
        fprintf(stderr, "%s:%d: got \"%.*s\"\n%*swant \"%s\"\n", __FILE__, line, (int)sb->len, sb->data,
                (int)strlen(__FILE__) + 8, "", want);
    }
    tv_sb_free(sb);
}
#define CHECK_SB(sb, want) check_sb_(__LINE__, &(sb), (want))

/* Compares an array of strings to want[] and RELEASES it. */
static void check_str_arr_(int line, tv_arr a, const char *const *want, int n) {
    checks++;
    bool ok = tv_arr_len(a) == n;
    for (int i = 0; ok && i < n; i++) {
        tv_str s = *(tv_str *)tv_arr_at(a, sizeof(tv_str), i, "test");
        ok = (size_t)s.p->len == strlen(want[i]) && memcmp(s.p->data, want[i], (size_t)s.p->len) == 0;
    }
    if (!ok) {
        failures++;
        tv_sb sb = {0};
        tv_inspect_arr(&sb, a, &tv_type_str, 1);
        fprintf(stderr, "%s:%d: string array mismatch, got %.*s\n", __FILE__, line, (int)sb.len, sb.data);
        tv_sb_free(&sb);
    }
    tv_arr_release(a, &tv_type_str);
}
#define CHECK_STR_ARR(a, ...)                                                      \
    do {                                                                           \
        static const char *const want_[] = {__VA_ARGS__};                          \
        check_str_arr_(__LINE__, (a), want_, (int)(sizeof want_ / sizeof *want_)); \
    } while (0)

/* Strings made by S() are released by drain(). */
static tv_str pool[1 << 16];
static int npool;
static tv_str S(const char *c) {
    tv_str s = tv_str_from(c, strlen(c));
    pool[npool++] = s;
    return s;
}
static void drain(void) {
    while (npool) tv_str_release(pool[--npool]);
}

/* Runs fn with traps redirected here; returns true (and the message) if it trapped. */
static bool traps(void (*fn)(void), char *msg, size_t cap) {
    jmp_buf jb;
    tv_test_jmp = &jb;
    tv_test_unwind = tv_test_longjmp;
    if (setjmp(jb) == 0) {
        fn();
        tv_test_jmp = NULL;
        return false;
    }
    tv_test_jmp = NULL;
    snprintf(msg, cap, "%.*s", (int)tv_test_msg.len, tv_test_msg.data);
    tv_sb_free(&tv_test_msg);
    return true;
}
#define CHECK_TRAPS(fn, want)                                                                                   \
    do {                                                                                                        \
        char m_[256] = "";                                                                                      \
        bool t_ = traps(fn, m_, sizeof m_);                                                                     \
        checks++;                                                                                               \
        if (!t_ || strcmp(m_, want) != 0) {                                                                     \
            failures++;                                                                                         \
            fprintf(stderr, "%s:%d: expected trap \"%s\", got %s \"%s\"\n", __FILE__, __LINE__, want,            \
                    t_ ? "trap" : "no trap", m_);                                                               \
        }                                                                                                       \
    } while (0)

/* Runs fn in a child process; captures stdout/stderr; returns the exit status (or -1). */
static int run_child(void (*fn)(void), char *out, size_t outcap, char *err, size_t errcap) {
    int po[2], pe[2];
    tv_out_flush();
    fflush(stdout);
    fflush(stderr);
    if (pipe(po) || pipe(pe)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(po[1], 1);
        dup2(pe[1], 2);
        close(po[0]), close(po[1]), close(pe[0]), close(pe[1]);
        tv_out_tty = false;
        fn();
        exit(0);
    }
    close(po[1]), close(pe[1]);
    size_t n = 0;
    ssize_t r;
    while ((r = read(po[0], out + n, outcap - 1 - n)) > 0) n += (size_t)r;
    out[n] = 0;
    char sink[4096];
    while (read(po[0], sink, sizeof sink) > 0) {} /* drain overflow */
    n = 0;
    while ((r = read(pe[0], err + n, errcap - 1 - n)) > 0) n += (size_t)r;
    err[n] = 0;
    close(po[0]), close(pe[0]);
    int st;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ================================================================== a counting element type */

typedef int64_t obj;
#define MAXOBJ 8192
static int64_t live[MAXOBJ];
static int64_t n_retain, n_release;
static obj next_obj = 1;

static obj obj_new(void) {
    obj o = next_obj++;
    if (o >= MAXOBJ) abort();
    live[o] = 1;
    return o;
}
static void obj_retain(void *p) { live[*(obj *)p]++, n_retain++; }
static void obj_release(void *p) {
    obj o = *(obj *)p;
    if (live[o] <= 0) {
        failures++;
        fprintf(stderr, "over-release of obj %lld\n", (long long)o);
    }
    live[o]--, n_release++;
}
static bool obj_eq(const void *a, const void *b) { return *(const obj *)a == *(const obj *)b; }
static uint64_t obj_hash(const void *p) { return tv_mix64((uint64_t)*(const obj *)p); }
static void obj_to_str(tv_sb *sb, const void *p) {
    tv_sb_push_char(sb, 'o');
    tv_sb_push_int(sb, *(const obj *)p);
}
static void obj_inspect(tv_sb *sb, const void *p, int d) { (void)d, obj_to_str(sb, p); }
static const tv_type obj_type = {sizeof(obj), obj_retain, obj_release, obj_eq, obj_hash, obj_to_str, obj_inspect};

static void obj_drop(obj o) { obj_release(&o); }
static bool all_dead(void) {
    for (int i = 0; i < MAXOBJ; i++)
        if (live[i]) {
            fprintf(stderr, "obj %d has count %lld\n", i, (long long)live[i]);
            return false;
        }
    return true;
}
static int64_t count_of(obj o) { return live[o]; }

/* ================================================================== strings */

TV_STR_LIT(lit_hello, "hello");

static void test_string_basics(void) {
    /* empty string is immortal and NUL-terminated */
    CHECK(tv_empty_strbuf.rc < 0 && tv_empty_strbuf.len == 0 && tv_empty_strbuf.data[0] == 0);
    tv_str e = tv_str_from("", 0);
    CHECK(e.p == &tv_empty_strbuf);
    tv_str_release(e);
    tv_str_release(e); /* immortal: no-op */

    tv_str a = tv_str_from("héllo", strlen("héllo"));
    CHECK(a.p->rc == 1 && tv_str_byte_len(a) == 6 && a.p->data[6] == 0);
    tv_str_retain(a);
    CHECK(a.p->rc == 2);
    tv_str_release(a);
    CHECK(a.p->rc == 1);

    /* single ASCII characters are interned */
    tv_str x1 = tv_str_from("x", 1), x2 = tv_str_from("x", 1);
    CHECK(x1.p == x2.p && x1.p->rc < 0 && x1.p->data[1] == 0);

    CHECK_STR(tv_str_concat(a, S(" world")), "héllo world");
    tv_str c = tv_str_concat(a, TV_EMPTY_STR);
    CHECK(c.p == a.p && a.p->rc == 2);
    tv_str_release(c);
    c = tv_str_concat(TV_EMPTY_STR, a);
    CHECK(c.p == a.p);
    tv_str_release(c);

    CHECK(tv_str_eq(TV_LIT(lit_hello), S("hello")));
    CHECK(!tv_str_eq(TV_LIT(lit_hello), S("hellO")));
    CHECK(!tv_str_eq(S("a"), S("ab")));
    CHECK(tv_str_cmp(S("a"), S("b")) < 0);
    CHECK(tv_str_cmp(S("b"), S("a")) > 0);
    CHECK(tv_str_cmp(S("abc"), S("abc")) == 0);
    CHECK(tv_str_cmp(S("a"), S("ab")) < 0);
    CHECK(tv_str_cmp(S("ab"), S("a")) > 0);
    CHECK(tv_str_cmp(S("é"), S("z")) > 0);          /* code point order */
    CHECK(tv_str_cmp(S("\xef\xbf\xbf"), S("😀")) < 0); /* U+FFFF < U+1F600 */
    CHECK(tv_str_cmp(TV_EMPTY_STR, S("a")) < 0);

    CHECK(tv_str_hash(TV_LIT(lit_hello)) == tv_str_hash(S("hello")));
    CHECK(tv_str_hash(S("a")) != tv_str_hash(S("b")));
    {
        tv_str nul = tv_str_from("\0", 1);
        CHECK(tv_str_hash(TV_EMPTY_STR) != tv_str_hash(nul));
        tv_str_release(nul);
    }
    {   /* all lengths 0..40 hash distinctly and consistently */
        char buf[64];
        uint64_t hs[41];
        for (int n = 0; n <= 40; n++) {
            for (int i = 0; i < n; i++) buf[i] = (char)('a' + i % 7);
            buf[n] = 0;
            hs[n] = tv_str_hash(S(buf));
            CHECK(hs[n] == tv_hash_bytes(buf, (size_t)n));
            for (int m = 0; m < n; m++) CHECK(hs[m] != hs[n]);
        }
    }

    CHECK_STR(tv_str_from_int(0), "0");
    CHECK_STR(tv_str_from_int(-42), "-42");
    CHECK_STR(tv_str_from_int(INT64_MAX), "9223372036854775807");
    CHECK_STR(tv_str_from_int(INT64_MIN), "-9223372036854775808");
    CHECK_STR(tv_str_from_bool(true), "true");
    CHECK_STR(tv_str_from_bool(false), "false");

    tv_sb sb = {0};
    tv_sb_push_cstr(&sb, "n=");
    tv_sb_push_int(&sb, -7);
    tv_sb_push_char(&sb, ' ');
    tv_sb_push_f64(&sb, 0.5);
    tv_sb_push_char(&sb, ' ');
    tv_sb_push_str(&sb, TV_LIT(lit_hello));
    tv_sb_push(&sb, "", 0);
    for (int i = 0; i < 1000; i++) tv_sb_push(&sb, "0123456789", 10);
    tv_str big = tv_str_from_sb(&sb);
    CHECK(sb.data == NULL && sb.len == 0 && sb.cap == 0);
    CHECK(big.p->len == 14 + 10000 && memcmp(big.p->data, "n=-7 0.5 hello01", 16) == 0);
    CHECK(big.p->data[big.p->len] == 0 && big.p->rc == 1);
    tv_str_release(big);
    tv_sb e2 = {0};
    CHECK(tv_str_from_sb(&e2).p == &tv_empty_strbuf);
    tv_sb_push_char(&e2, 'q');
    tv_str q = tv_str_from_sb(&e2);
    CHECK(q.p->len == 1 && q.p->data[0] == 'q' && e2.data == NULL);
    tv_sb e3 = {0};
    tv_sb_push_cstr(&e3, "ab");
    CHECK_STR(tv_str_from_sb(&e3), "ab");
    tv_sb e4 = {0};
    tv_sb_push_cstr(&e4, "discard");
    tv_sb_free(&e4);
    tv_sb_free(&e4);

    tv_str_release(a);
    drain();
}

typedef struct { double v; const char *s; } fmt_case;

static void test_number_format(void) {
    /* expected values produced by Node: String(x) */
    static const fmt_case cases[] = {
        {0, "0"}, {1, "1"}, {-1, "-1"}, {42, "42"}, {-0.0, "0"},
        {0.1 + 0.2, "0.30000000000000004"}, {1e21, "1e+21"}, {1e-7, "1e-7"}, {123e-20, "1.23e-18"},
        {5e-324, "5e-324"}, {1.7976931348623157e308, "1.7976931348623157e+308"}, {NAN, "NaN"},
        {INFINITY, "Infinity"}, {-INFINITY, "-Infinity"}, {0.000001, "0.000001"},
        {123456789012345680000.0, "123456789012345680000"}, {0.1, "0.1"}, {1.0 / 3, "0.3333333333333333"},
        {9007199254740992.0, "9007199254740992"}, {9007199254740994.0, "9007199254740994"},
        {-9223372036854775808.0, "-9223372036854776000"}, {9223372036854775808.0, "9223372036854776000"},
        {1e20, "100000000000000000000"}, {1.5e-6, "0.0000015"}, {1234.5678, "1234.5678"}, {100, "100"},
        {1e100, "1e+100"}, {-1e-100, "-1e-100"}, {4.35, "4.35"}, {0.5, "0.5"},
        {2.2250738585072014e-308, "2.2250738585072014e-308"}, {2.225073858507201e-308, "2.225073858507201e-308"},
        {123.456, "123.456"}, {1e16, "10000000000000000"}, {12345678901234567890.0, "12345678901234567000"},
        {-1.5e300, "-1.5e+300"}, {1e-6, "0.000001"}, {1e-5, "0.00001"},
        {999999999999999900000.0, "999999999999999900000"}, {5e-7, "5e-7"}, {1.5, "1.5"}, {-2.5e-8, "-2.5e-8"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        CHECK_CSTR(tv_str_from_f64(cases[i].v), cases[i].s);
        tv_sb sb = {0};
        tv_sb_push_f64(&sb, cases[i].v);
        check_sb_(__LINE__, &sb, cases[i].s);
    }
    /* f32 prints as the number it holds, as a Float32Array's element does in JavaScript */
    float f = 0.1f;
    tv_sb sb = {0};
    tv_type_f32.to_str(&sb, &f);
    CHECK_SB(sb, "0.10000000149011612");
    f = 16777216.0f;
    tv_type_f32.to_str(&sb, &f);
    CHECK_SB(sb, "16777216");
    f = 123456789.0f;
    tv_type_f32.to_str(&sb, &f);
    CHECK_SB(sb, "123456792");
    f = -0.0f;
    tv_type_f32.to_str(&sb, &f);
    CHECK_SB(sb, "0");
    tv_inspect_value(&sb, &tv_type_f32, &f, 1);
    CHECK_SB(sb, "-0");
    f = 1e-10f;
    tv_type_f32.to_str(&sb, &f);
    CHECK_SB(sb, "1.000000013351432e-10");

    /* toFixed; expected values from Node x.toFixed(0..5) */
    static const struct { double v; const char *s[6]; } fx[] = {
        {2.5, {"3", "2.5", "2.50", "2.500", "2.5000", "2.50000"}},
        {1.005, {"1", "1.0", "1.00", "1.005", "1.0050", "1.00500"}},
        {0.125, {"0", "0.1", "0.13", "0.125", "0.1250", "0.12500"}},
        {1.45, {"1", "1.4", "1.45", "1.450", "1.4500", "1.45000"}},
        {-1.5, {"-2", "-1.5", "-1.50", "-1.500", "-1.5000", "-1.50000"}},
        {1e21, {"1e+21", "1e+21", "1e+21", "1e+21", "1e+21", "1e+21"}},
        {123.456, {"123", "123.5", "123.46", "123.456", "123.4560", "123.45600"}},
        {-0.0, {"0", "0.0", "0.00", "0.000", "0.0000", "0.00000"}},
        {0.5, {"1", "0.5", "0.50", "0.500", "0.5000", "0.50000"}},
        {-0.5, {"-1", "-0.5", "-0.50", "-0.500", "-0.5000", "-0.50000"}},
        {1.5, {"2", "1.5", "1.50", "1.500", "1.5000", "1.50000"}},
        {-2.5, {"-3", "-2.5", "-2.50", "-2.500", "-2.5000", "-2.50000"}},
        {NAN, {"NaN", "NaN", "NaN", "NaN", "NaN", "NaN"}},
        {INFINITY, {"Infinity", "Infinity", "Infinity", "Infinity", "Infinity", "Infinity"}},
        {-0.0001, {"-0", "-0.0", "-0.00", "-0.000", "-0.0001", "-0.00010"}},
        {1e20,
         {"100000000000000000000", "100000000000000000000.0", "100000000000000000000.00",
          "100000000000000000000.000", "100000000000000000000.0000", "100000000000000000000.00000"}},
        {0.000001, {"0", "0.0", "0.00", "0.000", "0.0000", "0.00000"}},
        {5e-324, {"0", "0.0", "0.00", "0.000", "0.0000", "0.00000"}},
        {9.995, {"10", "10.0", "9.99", "9.995", "9.9950", "9.99500"}},
        {99.5, {"100", "99.5", "99.50", "99.500", "99.5000", "99.50000"}},
    };
    for (size_t i = 0; i < sizeof fx / sizeof *fx; i++)
        for (int d = 0; d <= 5; d++) CHECK_CSTR(tv_f64_to_fixed(fx[i].v, d, "t"), fx[i].s[d]);
    CHECK_STR(tv_f64_to_fixed(1.23, 20, "t"), "1.22999999999999998224");
    CHECK_STR(tv_f64_to_fixed(0.1, 25, "t"), "0.1000000000000000055511151");
    CHECK_STR(tv_f64_to_fixed(123.456, 100, "t"),
              "123.456000000000003069544618483632802963256835937500000000000000000000000000000000000000000000000000000"
              "0");
    CHECK_STR(tv_int_to_fixed(42, 0, "t"), "42");
    CHECK_STR(tv_int_to_fixed(-42, 3, "t"), "-42.000");
    drain();
}

static void trap_to_fixed(void) { tv_str s = tv_f64_to_fixed(1, 101, "f.tov:1:1"); tv_str_release(s); }
static void trap_to_fixed_neg(void) { tv_str s = tv_int_to_fixed(1, -1, "f.tov:1:1"); tv_str_release(s); }

static void test_parse(void) {
    double d = -1;
    CHECK(tv_parse_float(S("3.14"), &d) && d == 3.14);
    CHECK(tv_parse_float(S("  -2.5e3 \n"), &d) && d == -2500);
    CHECK(tv_parse_float(S("\xc2\xa0 1 \xe3\x80\x80\xef\xbb\xbf"), &d) && d == 1);
    CHECK(tv_parse_float(S("Infinity"), &d) && d == INFINITY);
    CHECK(tv_parse_float(S("-Infinity"), &d) && d == -INFINITY);
    CHECK(tv_parse_float(S("+Infinity"), &d) && d == INFINITY);
    CHECK(tv_parse_float(S("+.5"), &d) && d == 0.5);
    CHECK(tv_parse_float(S("5."), &d) && d == 5);
    CHECK(tv_parse_float(S("1E+2"), &d) && d == 100);
    CHECK(tv_parse_float(S("-0"), &d) && d == 0 && signbit(d));
    CHECK(tv_parse_float(S("1e400"), &d) && d == INFINITY);
    CHECK(tv_parse_float(S("0.1"), &d) && d == 0.1);
    CHECK(tv_parse_float(S("123456789012345680000"), &d) && d == 123456789012345680000.0);
    d = 7;
    const char *bad[] = {"", "   ", "1e", "e5", ".", "abc", "1.2.3", "0x10", "inf", "nan", "NaN", "12abc", "1 2",
                         "--1", "+-1", "Infinityx", "infinity", "1e+", "1_000"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!tv_parse_float(S(bad[i]), &d));
    CHECK(d == 7);

    tv_int v = -1;
    CHECK(tv_parse_int(S("42"), 0, &v) && v == 42);
    CHECK(tv_parse_int(S("  -17xyz"), 0, &v) && v == -17);
    CHECK(tv_parse_int(S("0x1F"), 0, &v) && v == 31);
    CHECK(tv_parse_int(S("-0X10"), 0, &v) && v == -16);
    CHECK(tv_parse_int(S("ff"), 16, &v) && v == 255);
    CHECK(tv_parse_int(S("0xff"), 16, &v) && v == 255);
    CHECK(tv_parse_int(S("z"), 36, &v) && v == 35);
    CHECK(tv_parse_int(S("1012"), 2, &v) && v == 5);
    CHECK(tv_parse_int(S("12.5"), 10, &v) && v == 12);
    CHECK(tv_parse_int(S("  +7"), 0, &v) && v == 7);
    CHECK(tv_parse_int(S("0x10"), 10, &v) && v == 0); /* parseInt("0x10", 10) === 0 */
    CHECK(tv_parse_int(S("9223372036854775807"), 10, &v) && v == INT64_MAX);
    CHECK(tv_parse_int(S("-9223372036854775808"), 10, &v) && v == INT64_MIN);
    v = 3;
    CHECK(!tv_parse_int(S("9223372036854775808"), 10, &v));
    CHECK(!tv_parse_int(S("abc"), 0, &v));
    CHECK(!tv_parse_int(S(""), 0, &v));
    CHECK(!tv_parse_int(S("0x"), 0, &v));
    CHECK(!tv_parse_int(S("-"), 0, &v));
    CHECK(!tv_parse_int(S("12"), 1, &v));
    CHECK(!tv_parse_int(S("12"), 37, &v));
    CHECK(!tv_parse_int(S("2"), 2, &v));
    CHECK(v == 3);
    drain();
}

static tv_str g_s;
static void trap_repeat(void) { tv_str r = tv_str_repeat(g_s, -1, "s.tov:6:1"); tv_str_release(r); }
static void trap_repeat_huge(void) { tv_str r = tv_str_repeat(g_s, INT64_MAX, "s.tov:7:1"); tv_str_release(r); }

static void test_string_methods(void) {
    tv_str s = S("héllo😀");
    CHECK(tv_str_byte_len(s) == 10 && tv_str_char_count(s) == 6);
    CHECK(tv_str_char_count(S("")) == 0);
    CHECK(tv_str_char_count(S("aaaaaaaaaaéééééééééé😀😀😀x")) == 24);
    CHECK_STR_ARR(tv_str_chars(s), "h", "é", "l", "l", "o", "😀");
    tv_arr no_chars = tv_str_chars(S(""));
    CHECK(tv_arr_len(no_chars) == 0);
    tv_arr_release(no_chars, &tv_type_str);

    tv_str h = S("hello");
    CHECK_STR(tv_str_slice(h, 1, 3, true, "t"), "el");
    CHECK_STR(tv_str_slice(h, -3, 0, false, "t"), "llo");
    CHECK_STR(tv_str_slice(h, -3, -1, true, "t"), "ll");
    CHECK_STR(tv_str_slice(h, 3, 1, true, "t"), "");
    CHECK_STR(tv_str_slice(h, -100, 2, true, "t"), "he");
    CHECK_STR(tv_str_slice(h, 5, 9, true, "t"), "");
    CHECK_STR(tv_str_slice(h, 0, 100, true, "t"), "hello");
    tv_str whole = tv_str_slice(h, 0, 0, false, "t");
    CHECK(whole.p == h.p && (h.p->rc & TV_STR_COUNT) == 2);
    tv_str_release(whole);
    /* indexes count UTF-16 units: "héllo😀" is h é l l o \uD83D \uDE00 */
    CHECK(tv_str_length(s) == 7 && tv_str_length(h) == 5 && tv_str_length(S("")) == 0);
    CHECK(tv_str_length(S("aaaaaaaaaaéééééééééé😀😀😀x")) == 27);
    CHECK_STR(tv_str_slice(s, 1, 2, true, "t"), "é");
    CHECK_STR(tv_str_slice(s, -2, 0, false, "t"), "😀");
    CHECK_STR(tv_str_slice(s, 5, 6, true, "t"), "\xef\xbf\xbd"); /* a lone half: U+FFFD */
    CHECK_STR(tv_str_slice(s, 6, 0, false, "t"), "\xef\xbf\xbd");
    CHECK_STR(tv_str_slice(s, 4, 6, true, "t"), "o\xef\xbf\xbd");
    CHECK_STR(tv_str_substring(s, 5, 1, true), "éllo");
    CHECK_STR(tv_str_substr(s, 1, 3, true), "éll");
    CHECK(tv_str_char_code_at(s, 0) == 'h' && tv_str_char_code_at(s, 1) == 0xE9 && tv_str_char_code_at(s, 5) == 0xD83D);
    CHECK(tv_str_char_code_at(s, 6) == 0xDE00 && tv_str_char_code_at(s, 7) == -1 && tv_str_char_code_at(s, -1) == -1);
    CHECK(tv_str_char_code_at(s, 2) == 'l' && tv_str_char_code_at(s, 4) == 'o'); /* walking back from the cache */
    CHECK(tv_str_code_point_at(s, 5) == 0x1F600 && tv_str_code_point_at(s, 6) == 0xDE00 && tv_str_code_point_at(s, 9) == -1);
    CHECK_STR(tv_str_char_at(s, 1), "é");
    CHECK_STR(tv_str_char_at(s, 70), "");
    CHECK_STR(tv_str_from_char_codes((tv_int[]){0xD83D, 0xDE00, 'a'}, 3), "😀a");
    CHECK_STR(tv_str_from_char_code(0xDE00), "\xef\xbf\xbd");
    CHECK(tv_str_last_index_of(S("é😀é😀"), S("é"), 0, false) == 3 && tv_str_last_index_of(S("é😀é😀"), S("😀"), 3, true) == 1);
    CHECK(tv_str_index_of_from(S("é😀é😀"), S("😀"), 2) == 4 && tv_str_index_of_from(S("é😀é"), S(""), 9) == 4);

    CHECK(tv_str_includes(h, S("ell")) && !tv_str_includes(h, S("elo")) && tv_str_includes(h, S("")));
    CHECK(tv_str_starts_with(h, S("he")) && !tv_str_starts_with(h, S("hello!")) && tv_str_starts_with(h, S("")));
    CHECK(tv_str_ends_with(h, S("llo")) && !tv_str_ends_with(h, S("xhello")) && tv_str_ends_with(h, S("")));
    CHECK(tv_str_index_of(h, S("l")) == 2);
    CHECK(tv_str_index_of(h, S("")) == 0);
    CHECK(tv_str_index_of(h, S("xyz")) == -1);
    CHECK(tv_str_index_of(S("aaab"), S("aab")) == 1);
    CHECK(tv_str_index_of(s, S("😀")) == 5);
    CHECK(tv_str_index_of(S("ab"), S("abc")) == -1);

    CHECK_STR_ARR(tv_str_split(S("a,b,,c"), S(",")), "a", "b", "", "c");
    CHECK_STR_ARR(tv_str_split(S("abc"), S("")), "a", "b", "c");
    CHECK_STR_ARR(tv_str_split(S(""), S(",")), "");
    tv_arr no_parts = tv_str_split(S(""), S(""));
    CHECK(tv_arr_len(no_parts) == 0);
    tv_arr_release(no_parts, &tv_type_str);
    CHECK_STR_ARR(tv_str_split(S("a--b--"), S("--")), "a", "b", "");
    CHECK_STR_ARR(tv_str_split(S("--"), S("--")), "", "");
    CHECK_STR_ARR(tv_str_split(S("abc"), S("x")), "abc");
    CHECK_STR_ARR(tv_str_split(S("é😀"), S("")), "é", "\xef\xbf\xbd", "\xef\xbf\xbd");
    CHECK_STR_ARR(tv_str_split(S("aaa"), S("aa")), "", "a");

    CHECK_STR(tv_str_trim(S(" \t\n\v\f\r hi there \xc2\xa0\xe3\x80\x80\xef\xbb\xbf\xe2\x80\xa8")), "hi there");
    CHECK_STR(tv_str_trim_start(S("  a b  ")), "a b  ");
    CHECK_STR(tv_str_trim_end(S("  a b  ")), "  a b");
    CHECK_STR(tv_str_trim(S("   ")), "");
    CHECK_STR(tv_str_trim(S("\xe2\x80\x8b x")), "\xe2\x80\x8b x"); /* U+200B is not whitespace */
    CHECK_STR(tv_str_trim(S("é")), "é");
    tv_str same = tv_str_trim(h);
    CHECK(same.p == h.p);
    tv_str_release(same);

    CHECK_STR(tv_str_to_upper(S("abcXYZé1")), "ABCXYZé1");
    CHECK_STR(tv_str_to_lower(S("abcXYZÉ1")), "abcxyzÉ1");
    same = tv_str_to_lower(h);
    CHECK(same.p == h.p);
    tv_str_release(same);

    CHECK_STR(tv_str_replace(S("aXbXc"), S("X"), S("-")), "a-bXc");
    CHECK_STR(tv_str_replace_all(S("aXbXc"), S("X"), S("-")), "a-b-c");
    CHECK_STR(tv_str_replace(S("abc"), S(""), S("-")), "-abc");
    CHECK_STR(tv_str_replace_all(S("abc"), S(""), S("-")), "-a-b-c-");
    CHECK_STR(tv_str_replace_all(S("é😀"), S(""), S("|")), "|é|😀|");
    CHECK_STR(tv_str_replace_all(S(""), S(""), S("x")), "x");
    CHECK_STR(tv_str_replace(S("abc"), S("b"), S("[$&|$$|$`|$'|$1|$]")), "a[b|$|a|c|$1|$]c");
    CHECK_STR(tv_str_replace_all(S("aaa"), S("a"), S("$&$&")), "aaaaaa");
    CHECK_STR(tv_str_replace_all(S("aaaa"), S("aa"), S("b")), "bb");
    CHECK_STR(tv_str_replace_all(S("xyz"), S("xyz"), S("")), "");
    same = tv_str_replace(h, S("q"), S("r"));
    CHECK(same.p == h.p);
    tv_str_release(same);
    same = tv_str_replace_all(h, S("q"), S("r"));
    CHECK(same.p == h.p);
    tv_str_release(same);

    CHECK_STR(tv_str_repeat(S("ab"), 3, "t"), "ababab");
    CHECK_STR(tv_str_repeat(S("abc"), 7, "t"), "abcabcabcabcabcabcabc");
    CHECK_STR(tv_str_repeat(S("x"), 0, "t"), "");
    CHECK_STR(tv_str_repeat(S(""), 5, "t"), "");
    CHECK_STR(tv_str_repeat(h, 1, "t"), "hello");
    g_s = h;
    CHECK_TRAPS(trap_repeat, "trap: Invalid count value: -1");
    CHECK_TRAPS(trap_repeat_huge, "trap: Invalid string length");

    CHECK_STR(tv_str_pad_start(S("5"), 3, S("0")), "005");
    CHECK_STR(tv_str_pad_start(S("abc"), 10, S("123")), "1231231abc");
    CHECK_STR(tv_str_pad_end(S("é"), 4, S("ü")), "éüüü");
    CHECK_STR(tv_str_pad_end(S("a"), 4, S("xy")), "axyx");
    CHECK_STR(tv_str_pad_start(S("😀"), 3, S("é😀")), "é😀");
    CHECK_STR(tv_str_pad_start(S("x"), 3, S("😀")), "😀x");
    CHECK_STR(tv_str_pad_start(S("x"), 2, S("😀")), "\xef\xbf\xbdx");
    CHECK_STR(tv_str_pad_start(S("abc"), 2, S("x")), "abc");
    CHECK_STR(tv_str_pad_start(S("abc"), 6, S("")), "abc");
    CHECK_STR(tv_str_pad_end(S("abc"), -5, S("x")), "abc");
    drain();
}

/* ================================================================== arrays */

typedef struct { int64_t key, seq; } rec;
static bool rec_eq(const void *a, const void *b) { return memcmp(a, b, sizeof(rec)) == 0; }
static uint64_t rec_hash(const void *p) { const rec *r = p; return tv_mix64((uint64_t)r->key * 31 + (uint64_t)r->seq); }
static void rec_to_str(tv_sb *sb, const void *p) { tv_sb_push_int(sb, ((const rec *)p)->key); }
static void rec_inspect(tv_sb *sb, const void *p, int d) {
    (void)d;
    tv_sb_push_cstr(sb, "{ key: ");
    tv_sb_push_int(sb, ((const rec *)p)->key);
    tv_sb_push_cstr(sb, ", seq: ");
    tv_sb_push_int(sb, ((const rec *)p)->seq);
    tv_sb_push_cstr(sb, " }");
}
static const tv_type rec_type = {sizeof(rec), NULL, NULL, rec_eq, rec_hash, rec_to_str, rec_inspect};

static int n_cmp_calls;
static double rec_by_key(void *ctx, const void *x, const void *y) {
    (void)ctx;
    n_cmp_calls++;
    return (double)(((const rec *)x)->key - ((const rec *)y)->key);
}
static double int_desc(void *ctx, const void *x, const void *y) {
    *(int *)ctx += 1;
    return (double)(*(const int64_t *)y > *(const int64_t *)x) - (double)(*(const int64_t *)y < *(const int64_t *)x);
}
static double cmp_nan(void *ctx, const void *x, const void *y) { (void)ctx, (void)x, (void)y; return NAN; }

static tv_arr g_arr;
static void trap_at(void) { (void)tv_arr_at(g_arr, sizeof(obj), 3, "a.tov:1:2"); }
static void trap_at_neg(void) { (void)tv_arr_at(g_arr, sizeof(obj), -1, "a.tov:1:3"); }
static void trap_at_mut(void) { (void)tv_arr_at_mut(&g_arr, &obj_type, 10, "a.tov:1:4"); }

static obj oat(tv_arr a, tv_int i) { return *(obj *)tv_arr_at(a, sizeof(obj), i, "t"); }

static void test_array_ownership(void) {
    tv_arr a = TV_EMPTY_ARR;
    CHECK(tv_arr_len(a) == 0 && a.p == &tv_empty_arrbuf);
    obj first = 0;
    for (int i = 0; i < 10; i++) {
        obj o = obj_new();
        if (!i) first = o;
        tv_arr_push(&a, &obj_type, &o);
        if (i == 0) CHECK(a.p->cap == 4);
        if (i == 4) CHECK(a.p->cap == 8);
    }
    CHECK(tv_arr_len(a) == 10 && a.p->cap == 16 && n_retain == 0);

    /* sharing (as JavaScript's arrays): b is a, and a push through b reaches a */
    tv_arr b = a;
    tv_arr_retain(b);
    CHECK(a.p->rc == 2);
    obj extra = obj_new();
    tv_arr_push(&b, &obj_type, &extra);
    CHECK(a.p == b.p && a.p->rc == 2 && tv_arr_len(a) == 11);
    CHECK(n_retain == 0 && count_of(first) == 1 && count_of(extra) == 1);
    tv_arr_release(b, &obj_type);
    CHECK(a.p->rc == 1 && tv_arr_pop(&a, &obj_type, NULL) && tv_arr_len(a) == 10 && count_of(extra) == 0);

    /* mutable access through one reference is to the elements every reference sees */
    b = a;
    tv_arr_retain(b);
    obj *slot = (obj *)tv_arr_at_mut(&b, &obj_type, 0, "t");
    CHECK(b.p == a.p && slot == (obj *)tv_arr_data(a) && count_of(first) == 1);
    tv_arr_release(b, &obj_type);

    /* pop / shift / unshift, on a copy (its own array, sharing elements) */
    obj out = 0;
    b = tv_arr_slice(a, &obj_type, 0, 0, false, false);
    CHECK(b.p != a.p && b.p->rc == 1);
    CHECK(tv_arr_pop(&b, &obj_type, &out) && out == oat(a, 9) && tv_arr_len(b) == 9 && count_of(out) == 2);
    obj_drop(out);
    CHECK(tv_arr_shift(&b, &obj_type, &out) && out == first && oat(b, 0) == oat(a, 1));
    obj_drop(out);
    obj front = obj_new();
    tv_arr_unshift(&b, &obj_type, &front);
    CHECK(oat(b, 0) == front && oat(b, 1) == oat(a, 1) && tv_arr_len(b) == 9);
    CHECK(tv_arr_pop(&b, &obj_type, NULL) && tv_arr_shift(&b, &obj_type, NULL)); /* NULL out releases */
    CHECK(tv_arr_len(b) == 7 && count_of(front) == 0);
    tv_arr_release(b, &obj_type);
    CHECK(tv_arr_len(a) == 10 && oat(a, 0) == first);

    /* bounds traps */
    tv_arr small = tv_arr_slice(a, &obj_type, 0, 3, true, true);
    g_arr = small;
    CHECK_TRAPS(trap_at, "trap: index 3 out of bounds for length 3");
    CHECK_TRAPS(trap_at_neg, "trap: index -1 out of bounds for length 3");
    CHECK_TRAPS(trap_at_mut, "trap: index 10 out of bounds for length 3");
    tv_arr_release(small, &obj_type);

    /* slice: JS semantics incl. negatives */
    tv_arr s1 = tv_arr_slice(a, &obj_type, 1, 3, true, true);
    CHECK(tv_arr_len(s1) == 2 && oat(s1, 0) == oat(a, 1) && count_of(oat(a, 1)) == 2);
    tv_arr s2 = tv_arr_slice(a, &obj_type, -2, 0, true, false);
    CHECK(tv_arr_len(s2) == 2 && oat(s2, 0) == oat(a, 8));
    tv_arr s3 = tv_arr_slice(a, &obj_type, -100, 2, true, true);
    CHECK(tv_arr_len(s3) == 2 && oat(s3, 1) == oat(a, 1));
    tv_arr s4 = tv_arr_slice(a, &obj_type, 3, 1, true, true);
    CHECK(tv_arr_len(s4) == 0);
    tv_arr s5 = tv_arr_slice(a, &obj_type, 0, 0, false, false);
    CHECK(s5.p != a.p && a.p->rc == 1 && tv_arr_len(s5) == 10 && oat(s5, 0) == first); /* a new array */
    tv_arr s6 = tv_arr_slice(a, &obj_type, 2, -7, true, true);
    CHECK(tv_arr_len(s6) == 1 && oat(s6, 0) == oat(a, 2));
    tv_arr s7 = tv_arr_slice(a, &obj_type, 10, 20, true, true);
    CHECK(tv_arr_len(s7) == 0);
    tv_arr_release(s1, &obj_type);
    tv_arr_release(s2, &obj_type);
    tv_arr_release(s3, &obj_type);
    tv_arr_release(s4, &obj_type);
    tv_arr_release(s5, &obj_type);
    tv_arr_release(s6, &obj_type);
    tv_arr_release(s7, &obj_type);

    /* concat */
    tv_arr c = tv_arr_concat(a, a, &obj_type);
    CHECK(tv_arr_len(c) == 20 && count_of(first) == 3 && oat(c, 10) == first);
    tv_arr c2 = tv_arr_concat(a, TV_EMPTY_ARR, &obj_type);
    CHECK(c2.p != a.p && tv_arr_len(c2) == 10);
    tv_arr c3 = tv_arr_concat(TV_EMPTY_ARR, a, &obj_type);
    CHECK(c3.p != a.p && c3.p != c2.p && a.p->rc == 1 && tv_arr_len(c3) == 10); /* new arrays */
    tv_arr_release(c, &obj_type);
    tv_arr_release(c2, &obj_type);
    tv_arr_release(c3, &obj_type);

    /* reverse a copy */
    b = tv_arr_slice(a, &obj_type, 0, 0, false, false);
    tv_arr_reverse(&b, &obj_type);
    CHECK(oat(b, 0) == oat(a, 9) && oat(b, 9) == first && oat(a, 0) == first);
    tv_arr_release(b, &obj_type);

    /* indexOf / lastIndexOf / join / inspect */
    obj o3 = oat(a, 3), missing = MAXOBJ - 1;
    CHECK(tv_arr_index_of(a, &obj_type, &o3) == 3 && tv_arr_last_index_of(a, &obj_type, &o3) == 3);
    CHECK(tv_arr_index_of(a, &obj_type, &missing) == -1 && tv_arr_last_index_of(a, &obj_type, &missing) == -1);
    tv_arr dup = tv_arr_concat(a, a, &obj_type);
    CHECK(tv_arr_index_of(dup, &obj_type, &o3) == 3 && tv_arr_last_index_of(dup, &obj_type, &o3) == 13);
    tv_arr_release(dup, &obj_type);
    tv_arr two = tv_arr_slice(a, &obj_type, 0, 2, true, true);
    tv_str j = tv_arr_join(two, &obj_type, S("-"));
    char want[64];
    snprintf(want, sizeof want, "o%lld-o%lld", (long long)oat(a, 0), (long long)oat(a, 1));
    CHECK_CSTR(j, want);
    tv_arr_release(two, &obj_type);

    /* sort a copy: the original keeps its order */
    b = tv_arr_slice(a, &obj_type, 0, 0, false, false);
    int calls = 0;
    tv_arr_sort(&b, &obj_type, int_desc, &calls); /* obj is an int64 */
    CHECK(calls > 0 && oat(b, 0) == oat(a, 9) && oat(a, 0) == first);
    tv_arr_release(b, &obj_type);

    tv_arr_release(a, &obj_type);
    CHECK(all_dead());
    CHECK(n_retain == n_release - (next_obj - 1)); /* every retain matched; each obj released once more */
    drain();
}

static void test_array_misc(void) {
    /* ints: join / to_str / default sort / comparator sort */
    tv_arr a = TV_EMPTY_ARR;
    int64_t vals[] = {10, 9, 1, 100, 25, -3};
    for (int i = 0; i < 6; i++) tv_arr_push_fast(&a, &tv_type_int, &vals[i]);
    CHECK_STR(tv_arr_join(a, &tv_type_int, S(",")), "10,9,1,100,25,-3");
    CHECK_STR(tv_arr_join(a, &tv_type_int, S("")), "109110025-3");
    tv_sb sb = {0};
    tv_to_str_arr(&sb, a, &tv_type_int);
    CHECK_SB(sb, "10,9,1,100,25,-3");
    tv_arr_sort(&a, &tv_type_int, NULL, NULL); /* JS default: by String(x) */
    CHECK_STR(tv_arr_join(a, &tv_type_int, S(" ")), "-3 1 10 100 25 9");
    int calls = 0;
    tv_arr_sort(&a, &tv_type_int, int_desc, &calls);
    CHECK_STR(tv_arr_join(a, &tv_type_int, S(" ")), "100 25 10 9 1 -3");
    tv_arr_sort(&a, &tv_type_int, cmp_nan, NULL); /* NaN comparator = equal: order kept */
    CHECK_STR(tv_arr_join(a, &tv_type_int, S(" ")), "100 25 10 9 1 -3");
    int64_t nine = 9, seven = 7;
    CHECK(tv_arr_index_of(a, &tv_type_int, &nine) == 3 && tv_arr_index_of(a, &tv_type_int, &seven) == -1);
    tv_arr_reverse(&a, &tv_type_int);
    CHECK_STR(tv_arr_join(a, &tv_type_int, S(" ")), "-3 1 9 10 25 100");
    tv_arr_release(a, &tv_type_int);

    /* stable sort of 16-byte records, large enough to exercise merging */
    tv_arr r = tv_arr_with_capacity(&rec_type, 1000);
    CHECK(r.p->cap == 1000 && tv_arr_len(r) == 0);
    uint64_t x = 12345;
    for (int i = 0; i < 1000; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        rec e = {(int64_t)(x >> 33) % 10, i};
        tv_arr_push_fast(&r, &rec_type, &e);
    }
    CHECK(r.p->cap == 1000);
    tv_arr rs = tv_arr_slice(r, &rec_type, 0, 0, false, false);
    tv_arr_sort(&rs, &rec_type, rec_by_key, NULL);
    bool stable = true;
    for (int i = 1; i < 1000; i++) {
        const rec *p = tv_arr_at(rs, sizeof(rec), i - 1, "t"), *q = tv_arr_at(rs, sizeof(rec), i, "t");
        if (p->key > q->key || (p->key == q->key && p->seq >= q->seq)) stable = false;
    }
    CHECK(stable);
    CHECK(((const rec *)tv_arr_at(r, sizeof(rec), 0, "t"))->seq == 0); /* original untouched */
    n_cmp_calls = 0;
    tv_arr_sort(&rs, &rec_type, rec_by_key, NULL); /* sorted input: n-1 comparisons */
    CHECK(n_cmp_calls == 999);
    tv_arr_sort(&r, &rec_type, NULL, NULL); /* default: String(key), also stable */
    stable = true;
    for (int i = 1; i < 1000; i++) {
        const rec *p = tv_arr_at(r, sizeof(rec), i - 1, "t"), *q = tv_arr_at(r, sizeof(rec), i, "t");
        if (p->key > q->key || (p->key == q->key && p->seq >= q->seq)) stable = false;
    }
    CHECK(stable);
    tv_arr_release(r, &rec_type);
    tv_arr_release(rs, &rec_type);

    /* strings: default sort, join fast path, release of elements */
    tv_arr ss = TV_EMPTY_ARR;
    const char *words[] = {"pear", "apple", "fig", "Apple", "é", "banana", "apple"};
    for (int i = 0; i < 7; i++) {
        tv_str w = tv_str_from(words[i], strlen(words[i]));
        tv_arr_push(&ss, &tv_type_str, &w);
    }
    tv_arr_sort(&ss, &tv_type_str, NULL, NULL);
    CHECK_STR(tv_arr_join(ss, &tv_type_str, S(",")), "Apple,apple,apple,banana,fig,pear,é");
    tv_str key = S("fig");
    CHECK(tv_arr_index_of(ss, &tv_type_str, &key) == 4);
    tv_arr one = tv_arr_slice(ss, &tv_type_str, 4, 5, true, true);
    tv_str j1 = tv_arr_join(one, &tv_type_str, S(","));
    CHECK(j1.p == ((tv_str *)tv_arr_data(one))->p);
    CHECK_STR(j1, "fig");
    tv_arr_release(one, &tv_type_str);
    tv_arr_release(ss, &tv_type_str);

    /* size-0 elements (arrays of undefined) */
    tv_arr u = TV_EMPTY_ARR;
    char dummy = 0;
    for (int i = 0; i < 5; i++) tv_arr_push(&u, &tv_type_undefined, &dummy);
    CHECK(tv_arr_len(u) == 5);
    CHECK(tv_arr_pop(&u, &tv_type_undefined, &dummy) && tv_arr_shift(&u, &tv_type_undefined, NULL));
    tv_arr_unshift(&u, &tv_type_undefined, &dummy);
    CHECK(tv_arr_len(u) == 4);
    CHECK_STR(tv_arr_join(u, &tv_type_undefined, S(",")), ",,,");
    tv_inspect_arr(&sb, u, &tv_type_undefined, 0);
    CHECK_SB(sb, "[ undefined, undefined, undefined, undefined ]");
    tv_arr_reverse(&u, &tv_type_undefined);
    tv_arr_sort(&u, &tv_type_undefined, NULL, NULL);
    CHECK(tv_arr_index_of(u, &tv_type_undefined, &dummy) == 0);
    tv_arr us = tv_arr_slice(u, &tv_type_undefined, 1, 3, true, true);
    tv_arr uc = tv_arr_concat(u, us, &tv_type_undefined);
    CHECK(tv_arr_len(us) == 2 && tv_arr_len(uc) == 6);
    tv_arr_release(us, &tv_type_undefined);
    tv_arr_release(uc, &tv_type_undefined);
    tv_arr_release(u, &tv_type_undefined);

    /* an immortal (static) array is cloned on mutation and never freed */
    static int64_t lit_data[3] = {1, 2, 3};
    static tv_arrbuf lit = {-1, 3, 3, (unsigned char *)lit_data};
    tv_arr la = {&lit};
    tv_arr_retain(la);
    tv_arr_release(la, &tv_type_int);
    int64_t four = 4;
    tv_arr_push(&la, &tv_type_int, &four);
    CHECK(la.p != &lit && lit.rc == -1 && lit_data[2] == 3 && tv_arr_len(la) == 4);
    CHECK_STR(tv_arr_join(la, &tv_type_int, S("+")), "1+2+3+4");
    tv_arr_release(la, &tv_type_int);

    /* make_unique / with_capacity edge cases */
    tv_arr e = tv_arr_with_capacity(&tv_type_int, 0);
    CHECK(e.p->rc == 1 && e.p->len == 0 && e.p->cap == 0);
    tv_arrbuf *e0 = e.p;
    tv_arr_make_unique(&e, &tv_type_int, 0);
    CHECK(e.p == e0);
    tv_arr_make_unique(&e, &tv_type_int, 3);
    CHECK(e.p && e.p->cap == 4 && e.p->len == 0 && e.p->rc == 1);
    tv_arr e2 = e;
    tv_arr_retain(e2);
    tv_arr_make_unique(&e2, &tv_type_int, 0); /* shared: stays the one array */
    CHECK(e2.p == e.p && e.p->rc == 2 && e.p->len == 0);
    int64_t v = 0;
    CHECK(!tv_arr_pop(&e2, &tv_type_int, &v) && !tv_arr_shift(&e2, &tv_type_int, &v));
    tv_arr_release(e, &tv_type_int);
    tv_arr_release(e2, &tv_type_int);
    tv_arr_release(TV_EMPTY_ARR, &tv_type_int);

    /* an array of strings, shared: a push through one reference is seen through the other */
    tv_arr sa = TV_EMPTY_ARR;
    tv_str w = tv_str_from("shared-string", 13);
    tv_arr_push(&sa, &tv_type_str, &w);
    tv_arr sb2 = sa;
    tv_arr_retain(sb2);
    tv_str w2 = tv_str_from("second-string", 13);
    tv_arr_push(&sb2, &tv_type_str, &w2);
    CHECK(w.p->rc == 1 && sa.p == sb2.p && tv_arr_len(sa) == 2);
    tv_arr_release(sa, &tv_type_str);
    CHECK(w.p->rc == 1 && tv_arr_len(sb2) == 2);
    tv_arr_release(sb2, &tv_type_str);
    drain();
    CHECK(all_dead());
}

/* ================================================================== maps and sets */

static int64_t map_int(tv_map m, int64_t k) {
    int64_t *v = tv_map_get(m, &tv_type_int, &tv_type_int, &k);
    return v ? *v : -999;
}
static void map_set_int(tv_map *m, int64_t k, int64_t v) { tv_map_set(m, &tv_type_int, &tv_type_int, &k, &v); }

static void check_order_(int line, tv_map m, const int64_t *want, int n) {
    tv_arr k = tv_map_keys(m, &tv_type_int, &tv_type_int);
    bool ok = tv_arr_len(k) == n && tv_map_size(m) == n;
    for (int i = 0; ok && i < n; i++) ok = *(int64_t *)tv_arr_at(k, 8, i, "t") == want[i];
    checks++;
    if (!ok) {
        failures++;
        tv_sb sb = {0};
        tv_inspect_arr(&sb, k, &tv_type_int, 1);
        fprintf(stderr, "%s:%d: key order %.*s\n", __FILE__, line, (int)sb.len, sb.data);
        tv_sb_free(&sb);
    }
    tv_arr_release(k, &tv_type_int);
}
#define CHECK_ORDER(m, ...)                                                   \
    do {                                                                      \
        static const int64_t w_[] = {__VA_ARGS__};                            \
        check_order_(__LINE__, (m), w_, (int)(sizeof w_ / sizeof *w_));        \
    } while (0)

static void test_map_basics(void) {
    tv_map m = TV_EMPTY_MAP;
    CHECK(tv_map_size(m) == 0 && map_int(m, 1) == -999);
    int64_t k = 1;
    CHECK(!tv_map_delete(&m, &tv_type_int, &tv_type_int, &k) && m.p == NULL);
    map_set_int(&m, 3, 30);
    map_set_int(&m, 1, 10);
    map_set_int(&m, 2, 20);
    CHECK_ORDER(m, 3, 1, 2);
    map_set_int(&m, 3, 33); /* replace keeps position */
    CHECK_ORDER(m, 3, 1, 2);
    CHECK(map_int(m, 3) == 33 && tv_map_size(m) == 3);
    k = 1;
    CHECK(tv_map_delete(&m, &tv_type_int, &tv_type_int, &k));
    CHECK(!tv_map_delete(&m, &tv_type_int, &tv_type_int, &k));
    CHECK(!tv_map_has(m, &tv_type_int, &tv_type_int, &k));
    map_set_int(&m, 1, 11); /* re-insert goes to the end */
    CHECK_ORDER(m, 3, 2, 1);
    CHECK(map_int(m, 1) == 11);

    /* values and iteration */
    tv_arr vs = tv_map_values(m, &tv_type_int, &tv_type_int);
    CHECK_STR(tv_arr_join(vs, &tv_type_int, S(",")), "33,20,11");
    tv_arr_release(vs, &tv_type_int);
    tv_sb sb = {0};
    void *kp, *vp;
    for (tv_int i = 0; tv_map_next(m, &tv_type_int, &tv_type_int, &i, &kp, &vp);) {
        tv_sb_push_int(&sb, *(int64_t *)kp);
        tv_sb_push_char(&sb, ':');
        tv_sb_push_int(&sb, *(int64_t *)vp);
        tv_sb_push_char(&sb, ' ');
    }
    CHECK_SB(sb, "3:33 2:20 1:11 ");
    tv_int cur = 0;
    CHECK(!tv_map_next(TV_EMPTY_MAP, &tv_type_int, &tv_type_int, &cur, &kp, &vp));

    /* sharing (as JavaScript's maps): m2 is m */
    tv_map m2 = m;
    tv_map_retain(m2);
    CHECK(m.p->rc == 2);
    map_set_int(&m2, 4, 40);
    CHECK(m.p == m2.p && tv_map_size(m) == 4);
    k = 4;
    CHECK(tv_map_delete(&m2, &tv_type_int, &tv_type_int, &k) && tv_map_size(m) == 3);
    tv_map_release(m2, &tv_type_int, &tv_type_int);

    /* a copy (new Map(m)) is a map of its own */
    m2 = tv_map_copy(m, &tv_type_int, &tv_type_int);
    map_set_int(&m2, 4, 40);
    CHECK(m.p != m2.p && m.p->rc == 1 && tv_map_size(m) == 3 && tv_map_size(m2) == 4);
    CHECK_ORDER(m2, 3, 2, 1, 4);
    CHECK(!tv_map_eq(m, m2, &tv_type_int, &tv_type_int));
    k = 4;
    CHECK(tv_map_delete(&m2, &tv_type_int, &tv_type_int, &k));
    CHECK(tv_map_eq(m, m2, &tv_type_int, &tv_type_int));
    map_set_int(&m2, 2, 21);
    CHECK(!tv_map_eq(m, m2, &tv_type_int, &tv_type_int));
    tv_map_release(m2, &tv_type_int, &tv_type_int);

    /* equality ignores insertion order */
    tv_map a = TV_EMPTY_MAP, b = TV_EMPTY_MAP;
    map_set_int(&a, 1, 1), map_set_int(&a, 2, 2);
    map_set_int(&b, 2, 2), map_set_int(&b, 1, 1);
    CHECK(tv_map_eq(a, b, &tv_type_int, &tv_type_int) && tv_map_eq(a, a, &tv_type_int, &tv_type_int));
    CHECK(tv_map_eq(TV_EMPTY_MAP, TV_EMPTY_MAP, &tv_type_int, &tv_type_int));
    CHECK(!tv_map_eq(a, TV_EMPTY_MAP, &tv_type_int, &tv_type_int));
    tv_map_release(a, &tv_type_int, &tv_type_int);
    tv_map_release(b, &tv_type_int, &tv_type_int);

    /* delete on a copy leaves the original */
    m2 = tv_map_copy(m, &tv_type_int, &tv_type_int);
    k = 77;
    CHECK(!tv_map_delete(&m2, &tv_type_int, &tv_type_int, &k));
    k = 3;
    CHECK(tv_map_delete(&m2, &tv_type_int, &tv_type_int, &k) && m2.p != m.p && tv_map_size(m) == 3);
    CHECK_ORDER(m2, 2, 1);
    tv_map_release(m2, &tv_type_int, &tv_type_int);

    /* deleting everything, then reuse */
    for (k = 1; k <= 3; k++) CHECK(tv_map_delete(&m, &tv_type_int, &tv_type_int, &k));
    CHECK(tv_map_size(m) == 0 && m.p->b->used == 0);
    map_set_int(&m, 9, 90);
    CHECK_ORDER(m, 9);
    tv_map_clear(&m, &tv_type_int, &tv_type_int);
    CHECK(m.p != NULL && tv_map_size(m) == 0); /* the same map, now empty */
    tv_map_clear(&m, &tv_type_int, &tv_type_int);
    tv_map_release(m, &tv_type_int, &tv_type_int);
    drain();
}

static void test_map_growth(void) {
    enum { N = 100000 };
    tv_map m = TV_EMPTY_MAP;
    for (int64_t i = 0; i < N; i++) map_set_int(&m, i * 7919, i);
    CHECK(tv_map_size(m) == N && m.p->b->cap == 131072);
    bool ok = true;
    for (int64_t i = 0; i < N; i++) ok &= map_int(m, i * 7919) == i;
    CHECK(ok);
    CHECK(map_int(m, 1) == -999);
    /* delete 3 of every 4 entries → tombstones */
    for (int64_t i = 0; i < N; i++) {
        int64_t k = i * 7919;
        if (i % 4 != 3) ok &= tv_map_delete(&m, &tv_type_int, &tv_type_int, &k);
    }
    CHECK(ok && tv_map_size(m) == N / 4 && m.p->b->used == N);
    ok = true;
    for (int64_t i = 0; i < N; i++) ok &= map_int(m, i * 7919) == (i % 4 == 3 ? i : -999);
    CHECK(ok);
    /* fill up: the entry array compacts (same capacity) instead of growing */
    int64_t cap = m.p->b->cap;
    for (int64_t i = N; m.p->b->used < cap; i++) map_set_int(&m, i * 7919, i);
    map_set_int(&m, -1, -1);
    CHECK(m.p->b->cap == cap && m.p->b->used == m.p->b->count);
    /* order survives compaction: surviving originals first, then the new keys */
    void *kp, *vp;
    int64_t prev = -1, n = 0;
    ok = true;
    for (tv_int i = 0; tv_map_next(m, &tv_type_int, &tv_type_int, &i, &kp, &vp); n++) {
        int64_t v = *(int64_t *)vp;
        if (n < N / 4) ok &= v % 4 == 3;
        if (n < tv_map_size(m) - 1) ok &= v > prev;
        prev = v;
    }
    CHECK(ok && prev == -1 && n == tv_map_size(m));
    tv_map_release(m, &tv_type_int, &tv_type_int);

    /* small map: compaction when half the entries are deleted */
    tv_map s = TV_EMPTY_MAP;
    for (int64_t k = 0; k < 8; k++) map_set_int(&s, k, k);
    CHECK(s.p->b->cap == 8);
    for (int64_t k = 0; k < 6; k++) CHECK(tv_map_delete(&s, &tv_type_int, &tv_type_int, &k));
    map_set_int(&s, 100, 100);
    CHECK(s.p->b->cap == 8 && s.p->b->used == 3);
    CHECK_ORDER(s, 6, 7, 100);
    map_set_int(&s, 7, 77);
    CHECK_ORDER(s, 6, 7, 100);
    tv_map_release(s, &tv_type_int, &tv_type_int);

    /* randomized differential test against a direct-mapped reference */
    enum { R = 512 };
    static int64_t ref[R];
    tv_map r = TV_EMPTY_MAP;
    for (int k = 0; k < R; k++) ref[k] = -999;
    uint64_t x = 99;
    ok = true;
    for (int step = 0; step < 200000; step++) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        int64_t k = (int64_t)(x % R);
        if ((x >> 20) % 3 == 0) {
            bool had = ref[k] != -999;
            ok &= tv_map_delete(&r, &tv_type_int, &tv_type_int, &k) == had;
            ref[k] = -999;
        } else {
            map_set_int(&r, k, step);
            ref[k] = step;
        }
        if (step % 997 == 0) {
            int64_t n = 0;
            for (int j = 0; j < R; j++) {
                ok &= map_int(r, j) == ref[j];
                n += ref[j] != -999;
            }
            ok &= tv_map_size(r) == n;
        }
    }
    CHECK(ok);
    tv_map_release(r, &tv_type_int, &tv_type_int);
}

static void test_map_ownership(void) {
    /* string keys, counting values */
    tv_map m = TV_EMPTY_MAP;
    char buf[32];
    obj vals[200];
    int64_t retains_before = n_retain;
    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof buf, "key-%d", i);
        tv_str k = tv_str_from(buf, strlen(buf));
        vals[i] = obj_new();
        tv_map_set(&m, &tv_type_str, &obj_type, &k, &vals[i]);
    }
    CHECK(tv_map_size(m) == 200 && n_retain == retains_before);
    tv_str probe = S("key-123");
    obj *got = tv_map_get(m, &tv_type_str, &obj_type, &probe);
    CHECK(got && *got == vals[123]);

    /* replacing a value releases the old one and the duplicate key */
    tv_str k2 = tv_str_from("key-5", 5);
    obj nv = obj_new();
    tv_map_set(&m, &tv_type_str, &obj_type, &k2, &nv);
    CHECK(count_of(vals[5]) == 0 && count_of(nv) == 1);

    /* a copy retains every key and value; release of the copy balances */
    tv_map c = tv_map_copy(m, &tv_type_str, &obj_type);
    tv_str k3 = tv_str_from("new-key", 7);
    obj v3 = obj_new();
    tv_map_set(&c, &tv_type_str, &obj_type, &k3, &v3);
    CHECK(count_of(vals[0]) == 2 && count_of(nv) == 2 && tv_map_size(c) == 201 && tv_map_size(m) == 200);
    tv_arr keys = tv_map_keys(c, &tv_type_str, &obj_type);
    tv_arr values = tv_map_values(c, &tv_type_str, &obj_type);
    CHECK(tv_arr_len(keys) == 201 && count_of(vals[0]) == 3);
    tv_str last = *(tv_str *)tv_arr_at(keys, sizeof(tv_str), 200, "t");
    CHECK(tv_str_eq(last, S("new-key")) && last.p->rc == 2);
    tv_arr_release(keys, &tv_type_str);
    tv_arr_release(values, &obj_type);
    tv_map_release(c, &tv_type_str, &obj_type);
    CHECK(count_of(vals[0]) == 1 && count_of(v3) == 0);

    /* delete releases key and value */
    tv_str dk = S("key-7");
    CHECK(tv_map_delete(&m, &tv_type_str, &obj_type, &dk));
    CHECK(count_of(vals[7]) == 0);
    tv_map_clear(&m, &tv_type_str, &obj_type);
    tv_map_release(m, &tv_type_str, &obj_type);
    CHECK(all_dead());

    /* obj keys with a set (size-0 values) */
    tv_map s = TV_EMPTY_MAP;
    obj ks[50];
    for (int i = 0; i < 50; i++) {
        ks[i] = obj_new();
        tv_map_set(&s, &obj_type, &tv_type_undefined, &ks[i], NULL);
    }
    obj dupe = ks[3];
    obj_retain(&dupe);
    tv_map_set(&s, &obj_type, &tv_type_undefined, &dupe, NULL); /* already present: key released */
    CHECK(tv_map_size(s) == 50 && count_of(ks[3]) == 1);
    CHECK(tv_map_has(s, &obj_type, &tv_type_undefined, &ks[10]));
    CHECK(tv_map_get(s, &obj_type, &tv_type_undefined, &ks[10]) != NULL);
    tv_map s2 = tv_map_copy(s, &obj_type, &tv_type_undefined);
    CHECK(tv_map_delete(&s2, &obj_type, &tv_type_undefined, &ks[10]));
    CHECK(count_of(ks[10]) == 1 && count_of(ks[11]) == 2);
    CHECK(tv_map_has(s, &obj_type, &tv_type_undefined, &ks[10]) &&
          !tv_map_has(s2, &obj_type, &tv_type_undefined, &ks[10]));
    CHECK(!tv_map_eq(s, s2, &obj_type, &tv_type_undefined));
    tv_map_release(s2, &obj_type, &tv_type_undefined);
    tv_map_release(s, &obj_type, &tv_type_undefined);
    CHECK(all_dead());
    drain();
}

static void test_map_keys_special(void) {
    /* f64 keys use SameValueZero: NaN == NaN, 0 == -0 */
    tv_map m = TV_EMPTY_MAP;
    double k = NAN;
    int64_t v = 1;
    tv_map_set(&m, &tv_type_f64, &tv_type_int, &k, &v);
    k = -NAN;
    v = 2;
    tv_map_set(&m, &tv_type_f64, &tv_type_int, &k, &v);
    k = 0.0;
    v = 3;
    tv_map_set(&m, &tv_type_f64, &tv_type_int, &k, &v);
    k = -0.0;
    v = 4;
    tv_map_set(&m, &tv_type_f64, &tv_type_int, &k, &v);
    CHECK(tv_map_size(m) == 2);
    k = NAN;
    CHECK(*(int64_t *)tv_map_get(m, &tv_type_f64, &tv_type_int, &k) == 2);
    k = 0.0;
    CHECK(*(int64_t *)tv_map_get(m, &tv_type_f64, &tv_type_int, &k) == 4);
    tv_sb sb = {0};
    tv_map_inspect(&sb, m, &tv_type_f64, &tv_type_int, 0);
    CHECK_SB(sb, "Map(2) { NaN => 2, 0 => 4 }");
    tv_map_release(m, &tv_type_f64, &tv_type_int);
    /* but === on the descriptor is JS strict equality */
    double n1 = NAN, z1 = 0.0, z2 = -0.0;
    CHECK(!tv_type_f64.eq(&n1, &n1) && tv_type_f64.eq(&z1, &z2));
    CHECK(tv_type_f64.hash(&z1) == tv_type_f64.hash(&z2));
    float fz1 = 0.0f, fz2 = -0.0f;
    CHECK(tv_type_f32.hash(&fz1) == tv_type_f32.hash(&fz2));

    /* small keys (u8) and bool values: padding inside entries */
    tv_map b = TV_EMPTY_MAP;
    for (int i = 0; i < 256; i++) {
        uint8_t key = (uint8_t)i;
        bool val = i % 3 == 0;
        tv_map_set(&b, &tv_type_u8, &tv_type_bool, &key, &val);
    }
    CHECK(tv_map_size(b) == 256);
    uint8_t key = 255;
    CHECK(*(bool *)tv_map_get(b, &tv_type_u8, &tv_type_bool, &key) == true);
    key = 254;
    CHECK(*(bool *)tv_map_get(b, &tv_type_u8, &tv_type_bool, &key) == false);
    tv_map_release(b, &tv_type_u8, &tv_type_bool);

    /* set of strings inspect */
    tv_map s = TV_EMPTY_MAP;
    const char *ws[] = {"b", "a", "b", "it's"};
    for (int i = 0; i < 4; i++) {
        tv_str w = tv_str_from(ws[i], strlen(ws[i]));
        tv_map_set(&s, &tv_type_str, &tv_type_undefined, &w, NULL);
    }
    tv_set_inspect(&sb, s, &tv_type_str, 0);
    CHECK_SB(sb, "Set(3) { 'b', 'a', \"it's\" }");
    tv_arr keys = tv_map_keys(s, &tv_type_str, &tv_type_undefined);
    CHECK_STR_ARR(keys, "b", "a", "it's");
    tv_map_release(s, &tv_type_str, &tv_type_undefined);
}

/* Header representation: sharing, copy-on-write and exact ownership. */
_Static_assert(sizeof(tv_arrbuf) == 32, "array header is 32 bytes (inline elements stay 16-byte aligned)");
_Static_assert(sizeof(tv_arr) == sizeof(void *), "tv_arr is a pointer to its header");

static void trap_reserve_neg(void) { (void)tv_arr_reserve_tail(&g_arr, &tv_type_int, -1); }

static void test_array_header(void) {
    int64_t r0 = n_retain, x0 = n_release;
    tv_arr a = TV_EMPTY_ARR;
    CHECK(a.p == &tv_empty_arrbuf && a.p->len == 0);
    obj o[3];
    for (int i = 0; i < 3; i++) {
        o[i] = obj_new();
        tv_arr_push(&a, &obj_type, &o[i]);
    }
    CHECK(a.p->len == 3 && a.p->cap == 4 && a.p->rc == 1);

    /* references to one array share its header: a push through one is seen through every one */
    tv_arr b = a;
    tv_arr_retain(b);
    CHECK(b.p == a.p && a.p->rc == 2 && oat(b, 2) == o[2] && n_retain == r0); /* retains the header, not the elements */
    obj extra = obj_new();
    tv_arr_push(&b, &obj_type, &extra);
    CHECK(b.p == a.p && a.p->len == 4 && oat(a, 3) == extra && n_retain == r0);
    tv_arr_release(b, &obj_type);
    CHECK(a.p->rc == 1 && count_of(extra) == 1 && n_release == x0);
    CHECK(tv_arr_pop(&a, &obj_type, NULL) && count_of(extra) == 0 && a.p->len == 3);

    /* pop then push: same header, the popped element is no longer owned */
    tv_arrbuf *buf = a.p;
    obj out = 0;
    CHECK(tv_arr_pop(&a, &obj_type, &out) && out == o[2] && a.p->len == 2 && a.p == buf && a.p->cap == 4);
    obj_drop(out); /* released exactly once: by us, not by the array */
    CHECK(count_of(o[2]) == 0);
    obj o3 = obj_new();
    tv_arr_push(&a, &obj_type, &o3);
    CHECK(a.p == buf && a.p->len == 3 && oat(a, 2) == o3);

    /* growing past the header's room moves the elements to a block of their own; the header,
     * and so every reference, stays */
    tv_arr c = a;
    tv_arr_retain(c);
    CHECK(buf->ext == NULL);
    obj o4 = obj_new(), o5 = obj_new();
    tv_arr_push(&c, &obj_type, &o4);
    tv_arr_push(&c, &obj_type, &o5);
    CHECK(c.p == buf && a.p == buf && buf->ext != NULL && buf->cap >= 5 && a.p->len == 5 && oat(a, 4) == o5);
    tv_arr_release(c, &obj_type);

    /* shift: remaining elements move down; the shifted one moves out */
    CHECK(tv_arr_shift(&a, &obj_type, &out) && out == o[0] && a.p->len == 4 && oat(a, 0) == o[1]);
    obj_drop(out);

    /* popping to empty keeps the header; releasing it releases nothing more */
    while (tv_arr_pop(&a, &obj_type, NULL)) {}
    CHECK(a.p->len == 0 && a.p == buf);
    int64_t x1 = n_release;
    tv_arr_release(a, &obj_type);
    CHECK(n_release == x1 && all_dead());

    /* slice / concat return new arrays sized to their len */
    a = TV_EMPTY_ARR;
    for (int i = 0; i < 5; i++) {
        obj x = obj_new();
        tv_arr_push(&a, &obj_type, &x);
    }
    tv_arr s = tv_arr_slice(a, &obj_type, 1, -1, true, true);
    CHECK(s.p->len == 3 && s.p != a.p && s.p->cap == 3 && s.p->rc == 1 && oat(s, 0) == oat(a, 1));
    tv_arr whole = tv_arr_slice(a, &obj_type, 0, 0, true, false);
    CHECK(whole.p != a.p && whole.p->len == 5 && a.p->rc == 1);
    c = tv_arr_concat(s, a, &obj_type);
    CHECK(c.p->len == 8 && c.p->cap == 8 && oat(c, 3) == oat(a, 0) && count_of(oat(a, 1)) == 5); /* a, s, whole, and twice in c */
    tv_arr c2 = tv_arr_concat(TV_EMPTY_ARR, s, &obj_type);
    CHECK(c2.p != s.p && c2.p->len == 3 && s.p->rc == 1);
    tv_arr_release(c2, &obj_type);
    tv_arr_release(c, &obj_type);
    tv_arr_release(whole, &obj_type);
    tv_arr_release(s, &obj_type);
    CHECK(count_of(oat(a, 1)) == 1);

    /* reserve_tail: capacity for exactly n more, pointer at element len, in the array itself */
    b = a;
    tv_arr_retain(b);
    obj *tail = (obj *)tv_arr_reserve_tail(&b, &obj_type, 2);
    CHECK(b.p == a.p && b.p->cap >= 7 && tail == (obj *)(void *)tv_arrbuf_data(b.p) + 5);
    tail[0] = obj_new();
    tail[1] = obj_new();
    b.p->len += 2;
    CHECK(a.p->len == 7 && count_of(oat(a, 0)) == 1 && count_of(tail[1]) == 1);
    tv_arr_release(b, &obj_type);
    tv_arr_release(a, &obj_type);
    CHECK(all_dead());

    tv_arr r = TV_EMPTY_ARR;
    CHECK(tv_arr_reserve_tail(&r, &tv_type_int, 0) != NULL && r.p != &tv_empty_arrbuf && tv_arr_len(r) == 0);
    int64_t *w = (int64_t *)tv_arr_reserve_tail(&r, &tv_type_int, 5);
    for (int i = 0; i < 5; i++) w[i] = i * i;
    r.p->len += 5;
    CHECK(r.p->cap == 5);
    w = (int64_t *)tv_arr_reserve_tail(&r, &tv_type_int, 1); /* unique: grows in place */
    *w = 99;
    r.p->len++;
    CHECK_STR(tv_arr_join(r, &tv_type_int, S(",")), "0,1,4,9,16,99");
    CHECK(tv_arr_reserve_tail(&r, &tv_type_int, 0) == tv_arrbuf_data(r.p) + 6 * sizeof(int64_t));
    g_arr = r;
    CHECK_TRAPS(trap_reserve_neg, "trap: invalid array reserve count");
    tv_arr_release(r, &tv_type_int);

    /* with_capacity + push_fast never reallocates within capacity */
    tv_arr f = tv_arr_with_capacity(&tv_type_int, 3);
    buf = f.p;
    for (int64_t i = 0; i < 3; i++) tv_arr_push_fast(&f, &tv_type_int, &i);
    CHECK(f.p == buf && f.p->len == 3);
    int64_t three = 3;
    tv_arr_push_fast(&f, &tv_type_int, &three); /* full: falls back to tv_arr_push */
    CHECK(f.p->len == 4 && f.p->cap == 6);
    tv_arr g = f;
    tv_arr_retain(g);
    tv_arr_push_fast(&g, &tv_type_int, &three); /* shared: one array */
    CHECK(g.p == f.p && f.p->len == 5 && f.p->rc == 2);
    tv_arr_release(g, &tv_type_int);
    tv_arr_release(f, &tv_type_int);

    /* arrays built by the runtime carry their len */
    tv_arr ch = tv_str_chars(S("héllo"));
    CHECK(ch.p->len == 5 && ch.p->cap == 5);
    tv_arr_release(ch, &tv_type_str);
    tv_map m = TV_EMPTY_MAP;
    for (int64_t i = 0; i < 10; i++) map_set_int(&m, i, i);
    for (int64_t i = 0; i < 10; i += 3) CHECK(tv_map_delete(&m, &tv_type_int, &tv_type_int, &i));
    tv_arr ks = tv_map_keys(m, &tv_type_int, &tv_type_int);
    CHECK(ks.p->len == 6 && tv_arr_len(ks) == tv_map_size(m));
    CHECK_STR(tv_arr_join(ks, &tv_type_int, S(",")), "1,2,4,5,7,8");
    tv_arr_release(ks, &tv_type_int);
    tv_map_release(m, &tv_type_int, &tv_type_int);

    /* every element retain was matched by a release */
    CHECK(all_dead());
    drain();
}

/* ================================================================== inspect */

static void inspect_arr_int(tv_sb *sb, const void *p, int d) { tv_inspect_arr(sb, *(const tv_arr *)p, &tv_type_int, d); }
static void release_arr_int(void *p) { tv_arr_release(*(tv_arr *)p, &tv_type_int); }
static void retain_arr(void *p) { tv_arr_retain(*(tv_arr *)p); }
static const tv_type arr_int_type = {sizeof(tv_arr), retain_arr, release_arr_int, NULL, NULL, NULL, inspect_arr_int};
static void inspect_arr2(tv_sb *sb, const void *p, int d) { tv_inspect_arr(sb, *(const tv_arr *)p, &arr_int_type, d); }
static void release_arr2(void *p) { tv_arr_release(*(tv_arr *)p, &arr_int_type); }
static const tv_type arr2_type = {sizeof(tv_arr), retain_arr, release_arr2, NULL, NULL, NULL, inspect_arr2};
static void inspect_arr3(tv_sb *sb, const void *p, int d) { tv_inspect_arr(sb, *(const tv_arr *)p, &arr2_type, d); }
static void release_arr3(void *p) { tv_arr_release(*(tv_arr *)p, &arr2_type); }
static const tv_type arr3_type = {sizeof(tv_arr), retain_arr, release_arr3, NULL, NULL, NULL, inspect_arr3};
static void inspect_map_ii(tv_sb *sb, const void *p, int d) {
    tv_map_inspect(sb, *(const tv_map *)p, &tv_type_int, &tv_type_int, d);
}
static void release_map_ii(void *p) { tv_map_release(*(tv_map *)p, &tv_type_int, &tv_type_int); }
static const tv_type map_ii_type = {sizeof(tv_map), NULL, release_map_ii, NULL, NULL, NULL, inspect_map_ii};
static void inspect_set_s(tv_sb *sb, const void *p, int d) { tv_set_inspect(sb, *(const tv_map *)p, &tv_type_str, d); }
static void release_set_s(void *p) { tv_map_release(*(tv_map *)p, &tv_type_str, &tv_type_undefined); }
static const tv_type set_s_type = {sizeof(tv_map), NULL, release_set_s, NULL, NULL, NULL, inspect_set_s};

static tv_arr wrap(const tv_type *t, void *elem) {
    tv_arr a = TV_EMPTY_ARR;
    tv_arr_push(&a, t, elem);
    return a;
}

static void test_inspect(void) {
    tv_sb sb = {0};
    tv_arr a = TV_EMPTY_ARR;
    for (int64_t i = 1; i <= 3; i++) tv_arr_push(&a, &tv_type_int, &i);
    tv_inspect_arr(&sb, a, &tv_type_int, 0);
    CHECK_SB(sb, "[ 1, 2, 3 ]");
    tv_inspect_arr(&sb, TV_EMPTY_ARR, &tv_type_int, 0);
    CHECK_SB(sb, "[]");

    /* strings: unquoted at top level, Node quoting inside containers */
    tv_inspect_str(&sb, S("it's"), 0);
    CHECK_SB(sb, "it's");
    tv_arr ss = TV_EMPTY_ARR;
    const char *words[] = {"a", "it's", "q\"", "both'\"", "all'\"`", "nl\n\ttab\\\x01\x7f\x1b\r\b\f\v", "é", ""};
    for (int i = 0; i < 8; i++) {
        tv_str w = tv_str_from(words[i], strlen(words[i]));
        tv_arr_push(&ss, &tv_type_str, &w);
    }
    tv_inspect_arr(&sb, ss, &tv_type_str, 0);
    CHECK_SB(sb, "[\n  'a',\n  \"it's\",\n  'q\"',\n  `both'\"`,\n  'all\\'\"`',\n"
                 "  'nl\\n\\ttab\\\\\\x01\\x7F\\x1B\\r\\b\\f\\x0B',\n  'é',\n  ''\n]");
    tv_arr_release(ss, &tv_type_str);

    /* numbers, bools, undefined */
    tv_arr f = TV_EMPTY_ARR;
    double ds[] = {-0.0, 0.0, 1.5, NAN, 1e21, -INFINITY};
    for (int i = 0; i < 6; i++) tv_arr_push(&f, &tv_type_f64, &ds[i]);
    tv_inspect_arr(&sb, f, &tv_type_f64, 0);
    CHECK_SB(sb, "[ -0, 0, 1.5, NaN, 1e+21, -Infinity ]");
    tv_to_str_arr(&sb, f, &tv_type_f64);
    CHECK_SB(sb, "0,0,1.5,NaN,1e+21,-Infinity");
    tv_arr_release(f, &tv_type_f64);
    bool t = true;
    tv_inspect_value(&sb, &tv_type_bool, &t, 1);
    tv_inspect_value(&sb, &tv_type_undefined, NULL, 1);
    CHECK_SB(sb, "trueundefined");
    int8_t i8 = -5;
    uint64_t u64 = UINT64_MAX;
    uint32_t u32 = 4000000000u;
    tv_inspect_value(&sb, &tv_type_i8, &i8, 1);
    tv_sb_push_char(&sb, ' ');
    tv_type_u64.to_str(&sb, &u64);
    tv_sb_push_char(&sb, ' ');
    tv_type_u32.to_str(&sb, &u32);
    CHECK_SB(sb, "-5 18446744073709551615 4000000000");
    CHECK(tv_type_int.size == 8 && tv_type_i16.size == 2 && tv_type_u16.size == 2 && tv_type_i32.size == 4 &&
          tv_type_u8.size == 1 && tv_type_bool.size == sizeof(bool) && tv_type_undefined.size == 0 &&
          tv_type_str.retain && tv_type_str.release && !tv_type_int.retain);

    /* nesting: Node shows [Array] below depth 2 */
    tv_arr l1 = a;
    tv_arr_retain(l1);
    tv_arr l2 = wrap(&arr_int_type, &l1);
    tv_arr l3 = wrap(&arr2_type, &l2);
    tv_arr l4 = wrap(&arr3_type, &l3);
    tv_inspect_arr(&sb, l3, &arr2_type, 0);
    CHECK_SB(sb, "[ [ [ 1, 2, 3 ] ] ]");
    tv_inspect_arr(&sb, l4, &arr3_type, 0);
    CHECK_SB(sb, "[ [ [ [Array] ] ] ]");
    tv_arr_release(l4, &arr3_type);

    /* maps and sets */
    tv_map m = TV_EMPTY_MAP;
    tv_str ka = tv_str_from("a", 1), kb = tv_str_from("b", 1);
    int64_t one = 1, two = 2;
    tv_map_set(&m, &tv_type_str, &tv_type_int, &ka, &one);
    tv_map_set(&m, &tv_type_str, &tv_type_int, &kb, &two);
    tv_map_inspect(&sb, m, &tv_type_str, &tv_type_int, 0);
    CHECK_SB(sb, "Map(2) { 'a' => 1, 'b' => 2 }");
    tv_map_release(m, &tv_type_str, &tv_type_int);
    tv_map st = TV_EMPTY_MAP;
    tv_map_set(&st, &tv_type_int, &tv_type_undefined, &one, NULL);
    tv_map_set(&st, &tv_type_int, &tv_type_undefined, &two, NULL);
    tv_map_set(&st, &tv_type_int, &tv_type_undefined, &one, NULL);
    tv_set_inspect(&sb, st, &tv_type_int, 0);
    CHECK_SB(sb, "Set(2) { 1, 2 }");
    tv_map_release(st, &tv_type_int, &tv_type_undefined);
    tv_map_inspect(&sb, TV_EMPTY_MAP, &tv_type_int, &tv_type_int, 0);
    tv_sb_push_char(&sb, ' ');
    tv_set_inspect(&sb, TV_EMPTY_MAP, &tv_type_int, 0);
    CHECK_SB(sb, "Map(0) {} Set(0) {}");

    /* Map nested in arrays: [ [ Map(1) { 1 => 2 } ] ] and [ [ [ [Map] ] ] ] */
    tv_map mi = TV_EMPTY_MAP;
    map_set_int(&mi, 1, 2);
    tv_arr w1 = wrap(&map_ii_type, &mi);
    tv_inspect_arr(&sb, w1, &map_ii_type, 1);
    CHECK_SB(sb, "[ Map(1) { 1 => 2 } ]");
    tv_inspect_arr(&sb, w1, &map_ii_type, 2);
    CHECK_SB(sb, "[ [Map] ]");
    tv_arr_release(w1, &map_ii_type);

    /* Map(1) { 'k' => Set(1) { 'x' } } */
    tv_map inner = TV_EMPTY_MAP;
    tv_str x = tv_str_from("x", 1);
    tv_map_set(&inner, &tv_type_str, &tv_type_undefined, &x, NULL);
    tv_map outer = TV_EMPTY_MAP;
    tv_str kk = tv_str_from("k", 1);
    tv_map_set(&outer, &tv_type_str, &set_s_type, &kk, &inner);
    tv_map_inspect(&sb, outer, &tv_type_str, &set_s_type, 0);
    CHECK_SB(sb, "Map(1) { 'k' => Set(1) { 'x' } }");
    tv_set_inspect(&sb, inner, &tv_type_str, 3);
    CHECK_SB(sb, "[Set]");
    tv_map_release(outer, &tv_type_str, &set_s_type);

    tv_arr_release(a, &tv_type_int);
    drain();
}

/* ================================================================== Node layout (util.inspect) */

/* Expected strings are Node 26's util.inspect output (= console.log), generated from the
 * equivalent JavaScript values. */
static const char want_short_ints[] =
        "[\n"
        "  5,   3,   9, 1,\n"
        "  7, 100, 200\n"
        "]";
static const char want_six_ints[] =
        "[ 1, 2, 3, 4, 5, 6 ]";
static const char want_ints_26[] =
        "[\n"
        "   0,  3,  6,  9, 12, 15, 18, 21,\n"
        "  24, 27, 30, 33, 36, 39, 42, 45,\n"
        "  48, 51, 54, 57, 60, 63, 66, 69,\n"
        "  72, 75\n"
        "]";
static const char want_ints_120[] =
        "[\n"
        "   0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11,\n"
        "  12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,\n"
        "  24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,\n"
        "  36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,\n"
        "  48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59,\n"
        "  60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,\n"
        "  72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83,\n"
        "  84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,\n"
        "  96, 97, 98, 99,\n"
        "  ... 20 more items\n"
        "]";
static const char want_f64_101[] =
        "[\n"
        "    0,   1.5,   3,   4.5,   6,   7.5,   9,  10.5,  12,  13.5,\n"
        "   15,  16.5,  18,  19.5,  21,  22.5,  24,  25.5,  27,  28.5,\n"
        "   30,  31.5,  33,  34.5,  36,  37.5,  39,  40.5,  42,  43.5,\n"
        "   45,  46.5,  48,  49.5,  51,  52.5,  54,  55.5,  57,  58.5,\n"
        "   60,  61.5,  63,  64.5,  66,  67.5,  69,  70.5,  72,  73.5,\n"
        "   75,  76.5,  78,  79.5,  81,  82.5,  84,  85.5,  87,  88.5,\n"
        "   90,  91.5,  93,  94.5,  96,  97.5,  99, 100.5, 102, 103.5,\n"
        "  105, 106.5, 108, 109.5, 111, 112.5, 114, 115.5, 117, 118.5,\n"
        "  120, 121.5, 123, 124.5, 126, 127.5, 129, 130.5, 132, 133.5,\n"
        "  135, 136.5, 138, 139.5, 141, 142.5, 144, 145.5, 147, 148.5,\n"
        "  ... 1 more item\n"
        "]";
static const char want_f64_mix[] =
        "[ 0.1, -0, NaN, 1e+21, -Infinity, 123.456, 7, 8 ]";
static const char want_neg_ints_7[] =
        "[\n"
        "    -1, 20, -300,\n"
        "  4000, -5,   60,\n"
        "    -7\n"
        "]";
static const char want_words_7[] =
        "[ 'apple', 'banana', 'cherry', 'date', 'elderberry', 'fig', 'grape' ]";
static const char want_words_30[] =
        "[\n"
        "  'apple',     'banana',     'cherry',\n"
        "  'date',      'elderberry', 'fig',\n"
        "  'grape',     'honeydew',   'kiwi',\n"
        "  'lemon',     'mango',      'nectarine',\n"
        "  'orange',    'papaya',     'quince',\n"
        "  'raspberry', 'strawberry', 'tangerine',\n"
        "  'ugli',      'vanilla',    'watermelon',\n"
        "  'xigua',     'yam',        'zucchini',\n"
        "  'apricot',   'blueberry',  'cantaloupe',\n"
        "  'durian',    'eggplant',   'feijoa'\n"
        "]";
static const char want_long_strs[] =
        "[\n"
        "  'this is a fairly long string number 0',\n"
        "  'this is a fairly long string number 1',\n"
        "  'this is a fairly long string number 2',\n"
        "  'this is a fairly long string number 3',\n"
        "  'this is a fairly long string number 4',\n"
        "  'this is a fairly long string number 5',\n"
        "  'this is a fairly long string number 6'\n"
        "]";
static const char want_multiline_str[] =
        "[ 'first line of a long string\\nsecond line of it\\nthird', 'short' ]";
static const char want_quotes[] =
        "[\n"
        "  \"it's\",\n"
        "  'say \"hi\"',\n"
        "  `it's \"both\"`,\n"
        "  'it\\'s \"both\" `tick`',\n"
        "  'it\\'s \"both\" ${x}',\n"
        "  'back\\\\slash\\ttab',\n"
        "  '\\x85c1'\n"
        "]";
static const char want_wide[] =
        "[\n"
        "  '日本', '中文字',\n"
        "  'a',    'bb',\n"
        "  '😀',   'ccc',\n"
        "  'dddd', 'é'\n"
        "]";
static const char want_bools_8[] =
        "[\n"
        "  true,  false,\n"
        "  true,  true,\n"
        "  false, false,\n"
        "  true,  false\n"
        "]";
static const char want_u8_16[] =
        "[\n"
        "    0,  37, 74, 111, 148, 185,\n"
        "  222,   3, 40,  77, 114, 151,\n"
        "  188, 225,  6,  43\n"
        "]";
static const char want_shapes[] =
        "[\n"
        "  { kind: 'circle', r: 2 },\n"
        "  { kind: 'square', side: 3 },\n"
        "  { kind: 'rect', w: 2, h: 5 }\n"
        "]";
static const char want_one_shape[] =
        "[ { kind: 'circle', r: 2 } ]";
static const char want_shapes_7[] =
        "[\n"
        "  { kind: 'circle', r: 1 },\n"
        "  { kind: 'square', side: 2 },\n"
        "  { kind: 'rect', w: 3, h: 4 },\n"
        "  { kind: 'circle', r: 4 },\n"
        "  { kind: 'square', side: 5 },\n"
        "  { kind: 'rect', w: 6, h: 7 },\n"
        "  { kind: 'circle', r: 7 }\n"
        "]";
static const char want_small_recs_8[] =
        "[\n"
        "  { x: 0 }, { x: 1 },\n"
        "  { x: 2 }, { x: 3 },\n"
        "  { x: 4 }, { x: 5 },\n"
        "  { x: 6 }, { x: 7 }\n"
        "]";
static const char want_nested_4[] =
        "[ [ [ [Array] ], [ [Array] ] ], [ [ [Array] ] ] ]";
static const char want_nested_deep_empty[] =
        "[ [ [ [] ] ] ]";
static const char want_nested_3[] =
        "[ [ [ 1, 2, 3 ] ], [ [ 4 ] ] ]";
static const char want_pairs_10[] =
        "[\n"
        "  [ 0, 0 ],  [ 1, 1 ],\n"
        "  [ 2, 4 ],  [ 3, 9 ],\n"
        "  [ 4, 16 ], [ 5, 25 ],\n"
        "  [ 6, 36 ], [ 7, 49 ],\n"
        "  [ 8, 64 ], [ 9, 81 ]\n"
        "]";
static const char want_map_small[] =
        "Map(2) { 'a' => 1, 'b' => 2 }";
static const char want_map_30[] =
        "Map(30) {\n"
        "  0 => 'apple',\n"
        "  1 => 'banana',\n"
        "  2 => 'cherry',\n"
        "  3 => 'date',\n"
        "  4 => 'elderberry',\n"
        "  5 => 'fig',\n"
        "  6 => 'grape',\n"
        "  7 => 'honeydew',\n"
        "  8 => 'kiwi',\n"
        "  9 => 'lemon',\n"
        "  10 => 'mango',\n"
        "  11 => 'nectarine',\n"
        "  12 => 'orange',\n"
        "  13 => 'papaya',\n"
        "  14 => 'quince',\n"
        "  15 => 'raspberry',\n"
        "  16 => 'strawberry',\n"
        "  17 => 'tangerine',\n"
        "  18 => 'ugli',\n"
        "  19 => 'vanilla',\n"
        "  20 => 'watermelon',\n"
        "  21 => 'xigua',\n"
        "  22 => 'yam',\n"
        "  23 => 'zucchini',\n"
        "  24 => 'apricot',\n"
        "  25 => 'blueberry',\n"
        "  26 => 'cantaloupe',\n"
        "  27 => 'durian',\n"
        "  28 => 'eggplant',\n"
        "  29 => 'feijoa'\n"
        "}";
static const char want_map_arrays[] =
        "Map(2) {\n"
        "  'evens' => [\n"
        "    0,  2,  4,  6,\n"
        "    8, 10, 12, 14\n"
        "  ],\n"
        "  'odds' => [ 1, 3, 5 ]\n"
        "}";
static const char want_map_120[] =
        "Map(120) {\n"
        "  0 => 0,\n"
        "  1 => 2,\n"
        "  2 => 4,\n"
        "  3 => 6,\n"
        "  4 => 8,\n"
        "  5 => 10,\n"
        "  6 => 12,\n"
        "  7 => 14,\n"
        "  8 => 16,\n"
        "  9 => 18,\n"
        "  10 => 20,\n"
        "  11 => 22,\n"
        "  12 => 24,\n"
        "  13 => 26,\n"
        "  14 => 28,\n"
        "  15 => 30,\n"
        "  16 => 32,\n"
        "  17 => 34,\n"
        "  18 => 36,\n"
        "  19 => 38,\n"
        "  20 => 40,\n"
        "  21 => 42,\n"
        "  22 => 44,\n"
        "  23 => 46,\n"
        "  24 => 48,\n"
        "  25 => 50,\n"
        "  26 => 52,\n"
        "  27 => 54,\n"
        "  28 => 56,\n"
        "  29 => 58,\n"
        "  30 => 60,\n"
        "  31 => 62,\n"
        "  32 => 64,\n"
        "  33 => 66,\n"
        "  34 => 68,\n"
        "  35 => 70,\n"
        "  36 => 72,\n"
        "  37 => 74,\n"
        "  38 => 76,\n"
        "  39 => 78,\n"
        "  40 => 80,\n"
        "  41 => 82,\n"
        "  42 => 84,\n"
        "  43 => 86,\n"
        "  44 => 88,\n"
        "  45 => 90,\n"
        "  46 => 92,\n"
        "  47 => 94,\n"
        "  48 => 96,\n"
        "  49 => 98,\n"
        "  50 => 100,\n"
        "  51 => 102,\n"
        "  52 => 104,\n"
        "  53 => 106,\n"
        "  54 => 108,\n"
        "  55 => 110,\n"
        "  56 => 112,\n"
        "  57 => 114,\n"
        "  58 => 116,\n"
        "  59 => 118,\n"
        "  60 => 120,\n"
        "  61 => 122,\n"
        "  62 => 124,\n"
        "  63 => 126,\n"
        "  64 => 128,\n"
        "  65 => 130,\n"
        "  66 => 132,\n"
        "  67 => 134,\n"
        "  68 => 136,\n"
        "  69 => 138,\n"
        "  70 => 140,\n"
        "  71 => 142,\n"
        "  72 => 144,\n"
        "  73 => 146,\n"
        "  74 => 148,\n"
        "  75 => 150,\n"
        "  76 => 152,\n"
        "  77 => 154,\n"
        "  78 => 156,\n"
        "  79 => 158,\n"
        "  80 => 160,\n"
        "  81 => 162,\n"
        "  82 => 164,\n"
        "  83 => 166,\n"
        "  84 => 168,\n"
        "  85 => 170,\n"
        "  86 => 172,\n"
        "  87 => 174,\n"
        "  88 => 176,\n"
        "  89 => 178,\n"
        "  90 => 180,\n"
        "  91 => 182,\n"
        "  92 => 184,\n"
        "  93 => 186,\n"
        "  94 => 188,\n"
        "  95 => 190,\n"
        "  96 => 192,\n"
        "  97 => 194,\n"
        "  98 => 196,\n"
        "  99 => 198,\n"
        "  ... 20 more items\n"
        "}";
static const char want_map_empty[] =
        "Map(0) {}";
static const char want_set_small[] =
        "Set(2) { 1, 2 }";
static const char want_set_words[] =
        "Set(20) {\n"
        "  'apple',\n"
        "  'banana',\n"
        "  'cherry',\n"
        "  'date',\n"
        "  'elderberry',\n"
        "  'fig',\n"
        "  'grape',\n"
        "  'honeydew',\n"
        "  'kiwi',\n"
        "  'lemon',\n"
        "  'mango',\n"
        "  'nectarine',\n"
        "  'orange',\n"
        "  'papaya',\n"
        "  'quince',\n"
        "  'raspberry',\n"
        "  'strawberry',\n"
        "  'tangerine',\n"
        "  'ugli',\n"
        "  'vanilla'\n"
        "}";
static const char want_set_empty[] =
        "Set(0) {}";
static const char want_set_110[] =
        "Set(110) {\n"
        "  0,\n"
        "  1,\n"
        "  2,\n"
        "  3,\n"
        "  4,\n"
        "  5,\n"
        "  6,\n"
        "  7,\n"
        "  8,\n"
        "  9,\n"
        "  10,\n"
        "  11,\n"
        "  12,\n"
        "  13,\n"
        "  14,\n"
        "  15,\n"
        "  16,\n"
        "  17,\n"
        "  18,\n"
        "  19,\n"
        "  20,\n"
        "  21,\n"
        "  22,\n"
        "  23,\n"
        "  24,\n"
        "  25,\n"
        "  26,\n"
        "  27,\n"
        "  28,\n"
        "  29,\n"
        "  30,\n"
        "  31,\n"
        "  32,\n"
        "  33,\n"
        "  34,\n"
        "  35,\n"
        "  36,\n"
        "  37,\n"
        "  38,\n"
        "  39,\n"
        "  40,\n"
        "  41,\n"
        "  42,\n"
        "  43,\n"
        "  44,\n"
        "  45,\n"
        "  46,\n"
        "  47,\n"
        "  48,\n"
        "  49,\n"
        "  50,\n"
        "  51,\n"
        "  52,\n"
        "  53,\n"
        "  54,\n"
        "  55,\n"
        "  56,\n"
        "  57,\n"
        "  58,\n"
        "  59,\n"
        "  60,\n"
        "  61,\n"
        "  62,\n"
        "  63,\n"
        "  64,\n"
        "  65,\n"
        "  66,\n"
        "  67,\n"
        "  68,\n"
        "  69,\n"
        "  70,\n"
        "  71,\n"
        "  72,\n"
        "  73,\n"
        "  74,\n"
        "  75,\n"
        "  76,\n"
        "  77,\n"
        "  78,\n"
        "  79,\n"
        "  80,\n"
        "  81,\n"
        "  82,\n"
        "  83,\n"
        "  84,\n"
        "  85,\n"
        "  86,\n"
        "  87,\n"
        "  88,\n"
        "  89,\n"
        "  90,\n"
        "  91,\n"
        "  92,\n"
        "  93,\n"
        "  94,\n"
        "  95,\n"
        "  96,\n"
        "  97,\n"
        "  98,\n"
        "  99,\n"
        "  ... 10 more items\n"
        "}";
static const char want_person[] =
        "{ name: 'Ada', age: 36, tags: [ 'math', 'engines' ] }";
static const char want_person_long[] =
        "{\n"
        "  name: 'Grace Brewster Murray Hopper',\n"
        "  age: 85,\n"
        "  tags: [\n"
        "    'apple',      'banana',\n"
        "    'cherry',     'date',\n"
        "    'elderberry', 'fig',\n"
        "    'grape',      'honeydew',\n"
        "    'kiwi',       'lemon',\n"
        "    'mango',      'nectarine'\n"
        "  ]\n"
        "}";
static const char want_people[] =
        "[\n"
        "  { name: 'Ada', age: 36, tags: [ 'math' ] },\n"
        "  { name: 'Alan', age: 41, tags: [ 'logic', 'codes' ] },\n"
        "  { name: 'Grace', age: 85, tags: [] }\n"
        "]";
static const char want_nested_rec[] =
        "{ id: 1, pos: { x: 1.5, y: -2 }, items: [ 1, 2, 3 ] }";
static const char want_deep_rec[] =
        "{ a: { b: { c: [Object] } } }";
static const char want_empty_recs[] =
        "[ {}, {} ]";
static const char want_rec_in_arr_deep[] =
        "[ [ [ [Object] ] ] ]";
static const char want_map_in_arr_deep[] =
        "[ [ [ [Map] ] ] ]";
static const char want_set_in_map[] =
        "Map(1) { 'k' => Set(1) { 'x' } }";
static const char want_odd_keys[] =
        "{ '$x': 1, _ok: 2, a1: 3 }";
static const char want_empty_arr[] =
        "[]";
static const char want_rec_with_map[] =
        "{\n"
        "  name: 'inventory',\n"
        "  counts: Map(2) { 'apples' => 3, 'pears' => 12 },\n"
        "  tags: Set(1) { 'fresh' }\n"
        "}";
static const char want_long_ints_row[] =
        "[\n"
        "          1,       10,\n"
        "        100,     1000,\n"
        "      10000,   100000,\n"
        "    1000000, 10000000,\n"
        "  100000000\n"
        "]";
static const char want_arr_of_maps[] =
        "[\n"
        "  Map(1) { 0 => 'apple' },\n"
        "  Map(1) { 1 => 'banana' },\n"
        "  Map(1) { 2 => 'cherry' }\n"
        "]";
#define ARR_TYPE(name, elem)                                                                              \
    static void name##_insp(tv_sb *sb, const void *p, int d) { tv_inspect_arr(sb, *(const tv_arr *)p, elem, d); } \
    static void name##_rel(void *p) { tv_arr_release(*(tv_arr *)p, elem); }                               \
    static const tv_type name = {sizeof(tv_arr), retain_arr, name##_rel, NULL, NULL, NULL, name##_insp};
#define TEST_MAP_TYPE(name, kt, vt)                                                                            \
    static void name##_insp(tv_sb *sb, const void *p, int d) { tv_map_inspect(sb, *(const tv_map *)p, kt, vt, d); } \
    static void name##_rel(void *p) { tv_map_release(*(tv_map *)p, kt, vt); }                             \
    static const tv_type name = {sizeof(tv_map), NULL, name##_rel, NULL, NULL, NULL, name##_insp};

ARR_TYPE(arr_str_type, &tv_type_str)
ARR_TYPE(arr_map_ii_type, &map_ii_type)
ARR_TYPE(arr2_map_ii_type, &arr_map_ii_type)
TEST_MAP_TYPE(map_si_type, &tv_type_str, &tv_type_int)
TEST_MAP_TYPE(map_is_type, &tv_type_int, &tv_type_str)

/* records, as the compiler would generate them */
typedef struct { int64_t kind, a, b; } shape;
TV_STR_LIT(lit_circle, "circle");
TV_STR_LIT(lit_square, "square");
TV_STR_LIT(lit_rect, "rect");
static void shape_insp(tv_sb *sb, const void *p, int d) {
    const shape *s = p;
    static const char *const n0[] = {"kind", "r"}, *const n1[] = {"kind", "side"}, *const n2[] = {"kind", "w", "h"};
    static const tv_type *const ty[] = {&tv_type_str, &tv_type_int, &tv_type_int};
    tv_str kind = s->kind == 0 ? TV_LIT(lit_circle) : s->kind == 1 ? TV_LIT(lit_square) : TV_LIT(lit_rect);
    const void *const f[] = {&kind, &s->a, &s->b};
    tv_inspect_record(sb, d, s->kind == 2 ? 3 : 2, s->kind == 0 ? n0 : s->kind == 1 ? n1 : n2, ty, f);
}
static const tv_type shape_type = {sizeof(shape), NULL, NULL, NULL, NULL, NULL, shape_insp};

typedef struct { int64_t v; } one_int;
#define ONE_INT_TYPE(name, field)                                                   \
    static void name##_insp(tv_sb *sb, const void *p, int d) {                      \
        static const char *const n[] = {field};                                     \
        static const tv_type *const ty[] = {&tv_type_int};                          \
        const void *const f[] = {p};                                                \
        tv_inspect_record(sb, d, 1, n, ty, f);                                       \
    }                                                                               \
    static const tv_type name = {sizeof(one_int), NULL, NULL, NULL, NULL, NULL, name##_insp};
ONE_INT_TYPE(rec_x_type, "x")
ONE_INT_TYPE(rec_a1_type, "a")
ONE_INT_TYPE(rec_d_type, "d")
#define WRAP_TYPE(name, field, inner)                                               \
    static void name##_insp(tv_sb *sb, const void *p, int d) {                      \
        static const char *const n[] = {field};                                     \
        static const tv_type *const ty[] = {inner};                                 \
        const void *const f[] = {p};                                                \
        tv_inspect_record(sb, d, 1, n, ty, f);                                       \
    }                                                                               \
    static const tv_type name = {sizeof(one_int), NULL, NULL, NULL, NULL, NULL, name##_insp};
WRAP_TYPE(rec_c_type, "c", &rec_d_type)
WRAP_TYPE(rec_b_type, "b", &rec_c_type)
WRAP_TYPE(rec_a_type, "a", &rec_b_type)
ARR_TYPE(arr_rec_a1_type, &rec_a1_type)
ARR_TYPE(arr2_rec_a1_type, &arr_rec_a1_type)

static void empty_rec_insp(tv_sb *sb, const void *p, int d) { (void)p, tv_inspect_record(sb, d, 0, NULL, NULL, NULL); }
static const tv_type empty_rec_type = {0, NULL, NULL, NULL, NULL, NULL, empty_rec_insp};

typedef struct { int64_t x, ok, a1; } odd_rec;
static void odd_insp(tv_sb *sb, const void *p, int d) {
    const odd_rec *r = p;
    static const char *const n[] = {"$x", "_ok", "a1"};
    static const tv_type *const ty[] = {&tv_type_int, &tv_type_int, &tv_type_int};
    const void *const f[] = {&r->x, &r->ok, &r->a1};
    tv_inspect_record(sb, d, 3, n, ty, f);
}

typedef struct { tv_str name; int64_t age; tv_arr tags; } person;
static void person_insp(tv_sb *sb, const void *p, int d) {
    const person *r = p;
    static const char *const n[] = {"name", "age", "tags"};
    static const tv_type *const ty[] = {&tv_type_str, &tv_type_int, &arr_str_type};
    const void *const f[] = {&r->name, &r->age, &r->tags};
    tv_inspect_record(sb, d, 3, n, ty, f);
}
static void person_rel(void *p) {
    person *r = p;
    tv_str_release(r->name);
    tv_arr_release(r->tags, &tv_type_str);
}
static const tv_type person_type = {sizeof(person), NULL, person_rel, NULL, NULL, NULL, person_insp};

typedef struct { double x, y; } point;
static void point_insp(tv_sb *sb, const void *p, int d) {
    const point *r = p;
    static const char *const n[] = {"x", "y"};
    static const tv_type *const ty[] = {&tv_type_f64, &tv_type_f64};
    const void *const f[] = {&r->x, &r->y};
    tv_inspect_record(sb, d, 2, n, ty, f);
}
static const tv_type point_type = {sizeof(point), NULL, NULL, NULL, NULL, NULL, point_insp};
typedef struct { int64_t id; point pos; tv_arr items; } item_rec;
static void item_insp(tv_sb *sb, const void *p, int d) {
    const item_rec *r = p;
    static const char *const n[] = {"id", "pos", "items"};
    static const tv_type *const ty[] = {&tv_type_int, &point_type, &arr_int_type};
    const void *const f[] = {&r->id, &r->pos, &r->items};
    tv_inspect_record(sb, d, 3, n, ty, f);
}

typedef struct { tv_str name; tv_map counts, tags; } inventory;
static void inventory_insp(tv_sb *sb, const void *p, int d) {
    const inventory *r = p;
    static const char *const n[] = {"name", "counts", "tags"};
    static const tv_type *const ty[] = {&tv_type_str, &map_si_type, &set_s_type};
    const void *const f[] = {&r->name, &r->counts, &r->tags};
    tv_inspect_record(sb, d, 3, n, ty, f);
}

static const char *const fruit[30] = {
    "apple", "banana", "cherry", "date", "elderberry", "fig", "grape", "honeydew", "kiwi", "lemon",
    "mango", "nectarine", "orange", "papaya", "quince", "raspberry", "strawberry", "tangerine", "ugli", "vanilla",
    "watermelon", "xigua", "yam", "zucchini", "apricot", "blueberry", "cantaloupe", "durian", "eggplant", "feijoa"};

static tv_str mkstr(const char *c) { return tv_str_from(c, strlen(c)); }
static tv_arr strs(const char *const *ws, size_t n) {
    tv_arr a = TV_EMPTY_ARR;
    for (size_t i = 0; i < n; i++) {
        tv_str s = mkstr(ws[i]);
        tv_arr_push(&a, &tv_type_str, &s);
    }
    return a;
}
static tv_arr ints_of(size_t n, const int64_t *v) {
    tv_arr a = TV_EMPTY_ARR;
    for (size_t i = 0; i < n; i++) tv_arr_push(&a, &tv_type_int, (void *)&v[i]);
    return a;
}
static tv_arr one(const tv_type *t, void *elem) { return wrap(t, elem); }

/* Inspects a top-level value (depth 0), compares, releases. */
static void inspect_arr_check(int line, tv_arr a, const tv_type *t, const char *want) {
    tv_sb sb = {0};
    tv_inspect_arr(&sb, a, t, 0);
    check_sb_(line, &sb, want);
    tv_arr_release(a, t);
}
#define INSPECT_ARR(a, t, want) inspect_arr_check(__LINE__, (a), (t), (want))
static void inspect_val_check(int line, const tv_type *t, void *v, const char *want) {
    tv_sb sb = {0};
    t->inspect(&sb, v, 0);
    check_sb_(line, &sb, want);
    if (t->release) t->release(v);
}
#define INSPECT_VAL(t, v, want) inspect_val_check(__LINE__, (t), (v), (want))

static void test_inspect_node_layout(void) {
    tv_arr a;
    int64_t v[128];

    int64_t short_ints[] = {5, 3, 9, 1, 7, 100, 200};
    INSPECT_ARR(ints_of(7, short_ints), &tv_type_int, want_short_ints);
    int64_t six[] = {1, 2, 3, 4, 5, 6};
    INSPECT_ARR(ints_of(6, six), &tv_type_int, want_six_ints);
    for (int i = 0; i < 26; i++) v[i] = i * 3;
    INSPECT_ARR(ints_of(26, v), &tv_type_int, want_ints_26);
    for (int i = 0; i < 120; i++) v[i] = i;
    INSPECT_ARR(ints_of(120, v), &tv_type_int, want_ints_120);
    a = TV_EMPTY_ARR;
    for (int i = 0; i < 101; i++) {
        double x = i * 1.5;
        tv_arr_push(&a, &tv_type_f64, &x);
    }
    INSPECT_ARR(a, &tv_type_f64, want_f64_101);
    double mix[] = {0.1, -0.0, NAN, 1e21, -INFINITY, 123.456, 7, 8};
    a = TV_EMPTY_ARR;
    for (int i = 0; i < 8; i++) tv_arr_push(&a, &tv_type_f64, &mix[i]);
    INSPECT_ARR(a, &tv_type_f64, want_f64_mix);
    int64_t neg[] = {-1, 20, -300, 4000, -5, 60, -7};
    INSPECT_ARR(ints_of(7, neg), &tv_type_int, want_neg_ints_7);
    for (int i = 0; i < 9; i++) v[i] = (int64_t)pow(10, i);
    INSPECT_ARR(ints_of(9, v), &tv_type_int, want_long_ints_row);

    INSPECT_ARR(strs(fruit, 7), &tv_type_str, want_words_7);
    INSPECT_ARR(strs(fruit, 30), &tv_type_str, want_words_30);
    a = TV_EMPTY_ARR;
    for (int i = 0; i < 7; i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "this is a fairly long string number %d", i);
        tv_str s = mkstr(buf);
        tv_arr_push(&a, &tv_type_str, &s);
    }
    INSPECT_ARR(a, &tv_type_str, want_long_strs);
    static const char *const ml[] = {"first line of a long string\nsecond line of it\nthird", "short"};
    INSPECT_ARR(strs(ml, 2), &tv_type_str, want_multiline_str);
    static const char *const qs[] = {"it's", "say \"hi\"", "it's \"both\"", "it's \"both\" `tick`",
                                     "it's \"both\" ${x}", "back\\slash\ttab", ("\xc2\x85" "c1")};
    INSPECT_ARR(strs(qs, 7), &tv_type_str, want_quotes);
    static const char *const wide_s[] = {"日本", "中文字", "a", "bb", "😀", "ccc", "dddd", "é"};
    INSPECT_ARR(strs(wide_s, 8), &tv_type_str, want_wide);

    a = TV_EMPTY_ARR;
    bool bs[] = {true, false, true, true, false, false, true, false};
    for (int i = 0; i < 8; i++) tv_arr_push(&a, &tv_type_bool, &bs[i]);
    INSPECT_ARR(a, &tv_type_bool, want_bools_8);
    a = TV_EMPTY_ARR;
    for (int i = 0; i < 16; i++) {
        uint8_t b = (uint8_t)((i * 37) % 256);
        tv_arr_push(&a, &tv_type_u8, &b);
    }
    INSPECT_ARR(a, &tv_type_u8, want_u8_16);

    /* records */
    shape sh[] = {{0, 2, 0}, {1, 3, 0}, {2, 2, 5}};
    a = TV_EMPTY_ARR;
    for (int i = 0; i < 3; i++) tv_arr_push(&a, &shape_type, &sh[i]);
    INSPECT_ARR(a, &shape_type, want_shapes);
    INSPECT_ARR(one(&shape_type, &sh[0]), &shape_type, want_one_shape);
    a = TV_EMPTY_ARR;
    for (int i = 0; i < 7; i++) {
        shape s = {i % 3, i + 1, i + 2};
        tv_arr_push(&a, &shape_type, &s);
    }
    INSPECT_ARR(a, &shape_type, want_shapes_7);
    a = TV_EMPTY_ARR;
    for (int i = 0; i < 8; i++) {
        one_int r = {i};
        tv_arr_push(&a, &rec_x_type, &r);
    }
    INSPECT_ARR(a, &rec_x_type, want_small_recs_8);
    person ada = {mkstr("Ada"), 36, strs((const char *const[]){"math", "engines"}, 2)};
    INSPECT_VAL(&person_type, &ada, want_person);
    person grace = {mkstr("Grace Brewster Murray Hopper"), 85, strs(fruit, 12)};
    INSPECT_VAL(&person_type, &grace, want_person_long);
    a = TV_EMPTY_ARR;
    person ps[] = {{mkstr("Ada"), 36, strs((const char *const[]){"math"}, 1)},
                   {mkstr("Alan"), 41, strs((const char *const[]){"logic", "codes"}, 2)},
                   {mkstr("Grace"), 85, TV_EMPTY_ARR}};
    for (int i = 0; i < 3; i++) tv_arr_push(&a, &person_type, &ps[i]);
    INSPECT_ARR(a, &person_type, want_people);
    int64_t items[] = {1, 2, 3};
    item_rec it = {1, {1.5, -2}, ints_of(3, items)};
    tv_sb sb = {0};
    item_insp(&sb, &it, 0);
    CHECK_SB(sb, want_nested_rec);
    tv_arr_release(it.items, &tv_type_int);
    one_int deep = {1};
    INSPECT_VAL(&rec_a_type, &deep, want_deep_rec);
    char dummy[1] = {0};
    a = TV_EMPTY_ARR;
    tv_arr_push(&a, &empty_rec_type, dummy);
    tv_arr_push(&a, &empty_rec_type, dummy);
    INSPECT_ARR(a, &empty_rec_type, want_empty_recs);
    one_int a1 = {1};
    tv_arr l1 = one(&rec_a1_type, &a1), l2 = one(&arr_rec_a1_type, &l1);
    INSPECT_ARR(one(&arr2_rec_a1_type, &l2), &arr2_rec_a1_type, want_rec_in_arr_deep);
    odd_rec odd = {1, 2, 3};
    odd_insp(&sb, &odd, 0);
    CHECK_SB(sb, want_odd_keys);
    INSPECT_ARR(TV_EMPTY_ARR, &tv_type_int, want_empty_arr);

    /* nested arrays */
    int64_t n12[] = {1, 2}, n3[] = {3}, n456[] = {4, 5, 6};
    tv_arr x12 = ints_of(2, n12), x3 = ints_of(1, n3), x456 = ints_of(3, n456);
    tv_arr y12 = one(&arr_int_type, &x12), y3 = one(&arr_int_type, &x3), y456 = one(&arr_int_type, &x456);
    tv_arr z1 = one(&arr2_type, &y12), z2 = one(&arr2_type, &y456);
    tv_arr_push(&z1, &arr2_type, &y3);
    a = one(&arr3_type, &z1);
    tv_arr_push(&a, &arr3_type, &z2);
    INSPECT_ARR(a, &arr3_type, want_nested_4);
    tv_arr e0 = TV_EMPTY_ARR, e1 = one(&arr_int_type, &e0), e2 = one(&arr2_type, &e1);
    INSPECT_ARR(one(&arr3_type, &e2), &arr3_type, want_nested_deep_empty);
    int64_t n123[] = {1, 2, 3}, n4[] = {4};
    tv_arr p123 = ints_of(3, n123), p4 = ints_of(1, n4);
    tv_arr q123 = one(&arr_int_type, &p123), q4 = one(&arr_int_type, &p4);
    a = one(&arr2_type, &q123);
    tv_arr_push(&a, &arr2_type, &q4);
    INSPECT_ARR(a, &arr2_type, want_nested_3);
    a = TV_EMPTY_ARR;
    for (int i = 0; i < 10; i++) {
        int64_t pr[] = {i, i * i};
        tv_arr p = ints_of(2, pr);
        tv_arr_push(&a, &arr_int_type, &p);
    }
    INSPECT_ARR(a, &arr_int_type, want_pairs_10);

    /* maps and sets */
    tv_map m = TV_EMPTY_MAP;
    for (int i = 0; i < 2; i++) {
        tv_str k = mkstr(i ? "b" : "a");
        int64_t val = i + 1;
        tv_map_set(&m, &tv_type_str, &tv_type_int, &k, &val);
    }
    INSPECT_VAL(&map_si_type, &m, want_map_small);
    m = TV_EMPTY_MAP;
    for (int64_t i = 0; i < 30; i++) {
        tv_str s = mkstr(fruit[i]);
        tv_map_set(&m, &tv_type_int, &tv_type_str, &i, &s);
    }
    INSPECT_VAL(&map_is_type, &m, want_map_30);
    m = TV_EMPTY_MAP;
    int64_t evens[] = {0, 2, 4, 6, 8, 10, 12, 14}, odds[] = {1, 3, 5};
    tv_str ke = mkstr("evens"), ko = mkstr("odds");
    tv_arr ae = ints_of(8, evens), ao = ints_of(3, odds);
    tv_map_set(&m, &tv_type_str, &arr_int_type, &ke, &ae);
    tv_map_set(&m, &tv_type_str, &arr_int_type, &ko, &ao);
    tv_map_inspect(&sb, m, &tv_type_str, &arr_int_type, 0);
    CHECK_SB(sb, want_map_arrays);
    tv_map_release(m, &tv_type_str, &arr_int_type);
    m = TV_EMPTY_MAP;
    for (int64_t i = 0; i < 120; i++) map_set_int(&m, i, i * 2);
    INSPECT_VAL(&map_ii_type, &m, want_map_120);
    m = TV_EMPTY_MAP;
    INSPECT_VAL(&map_ii_type, &m, want_map_empty);
    tv_map s = TV_EMPTY_MAP;
    for (int64_t i = 1; i <= 2; i++) tv_map_set(&s, &tv_type_int, &tv_type_undefined, &i, NULL);
    tv_set_inspect(&sb, s, &tv_type_int, 0);
    CHECK_SB(sb, want_set_small);
    tv_map_release(s, &tv_type_int, &tv_type_undefined);
    s = TV_EMPTY_MAP;
    for (int i = 0; i < 20; i++) {
        tv_str w = mkstr(fruit[i]);
        tv_map_set(&s, &tv_type_str, &tv_type_undefined, &w, NULL);
    }
    INSPECT_VAL(&set_s_type, &s, want_set_words);
    s = TV_EMPTY_MAP;
    INSPECT_VAL(&set_s_type, &s, want_set_empty);
    for (int64_t i = 0; i < 110; i++) tv_map_set(&s, &tv_type_int, &tv_type_undefined, &i, NULL);
    tv_set_inspect(&sb, s, &tv_type_int, 0);
    CHECK_SB(sb, want_set_110);
    tv_map_release(s, &tv_type_int, &tv_type_undefined);
    m = TV_EMPTY_MAP;
    map_set_int(&m, 1, 2);
    tv_arr am = one(&map_ii_type, &m), am2 = one(&arr_map_ii_type, &am);
    INSPECT_ARR(one(&arr2_map_ii_type, &am2), &arr2_map_ii_type, want_map_in_arr_deep);
    tv_map inner = TV_EMPTY_MAP, outer = TV_EMPTY_MAP;
    tv_str xs = mkstr("x"), kk = mkstr("k");
    tv_map_set(&inner, &tv_type_str, &tv_type_undefined, &xs, NULL);
    tv_map_set(&outer, &tv_type_str, &set_s_type, &kk, &inner);
    tv_map_inspect(&sb, outer, &tv_type_str, &set_s_type, 0);
    CHECK_SB(sb, want_set_in_map);
    tv_map_release(outer, &tv_type_str, &set_s_type);
    inventory inv = {mkstr("inventory"), TV_EMPTY_MAP, TV_EMPTY_MAP};
    tv_str ka = mkstr("apples"), kp = mkstr("pears"), kf = mkstr("fresh");
    int64_t three = 3, twelve = 12;
    tv_map_set(&inv.counts, &tv_type_str, &tv_type_int, &ka, &three);
    tv_map_set(&inv.counts, &tv_type_str, &tv_type_int, &kp, &twelve);
    tv_map_set(&inv.tags, &tv_type_str, &tv_type_undefined, &kf, NULL);
    inventory_insp(&sb, &inv, 0);
    CHECK_SB(sb, want_rec_with_map);
    tv_str_release(inv.name);
    tv_map_release(inv.counts, &tv_type_str, &tv_type_int);
    tv_map_release(inv.tags, &tv_type_str, &tv_type_undefined);
    a = TV_EMPTY_ARR;
    for (int64_t i = 0; i < 3; i++) {
        tv_map mi = TV_EMPTY_MAP;
        tv_str w = mkstr(fruit[i]);
        tv_map_set(&mi, &tv_type_int, &tv_type_str, &i, &w);
        tv_arr_push(&a, &map_is_type, &mi);
    }
    INSPECT_ARR(a, &map_is_type, want_arr_of_maps);

    /* the layout state is restored after each value, and reset at depth 0 */
    CHECK(tv_ictx.indent == 0);
    tv_ictx.indent = 40; /* as if an earlier inspect had been interrupted by a trap */
    INSPECT_ARR(ints_of(7, short_ints), &tv_type_int, want_short_ints);
}

/* ================================================================== math, random, closures */

static void trap_nan(void) { (void)tv_f64_to_int(NAN, "Math.floor", "m.tov:1:1"); }
static void trap_big(void) { (void)tv_f64_to_int(1e19, "Math.round", "m.tov:2:1"); }
static void trap_inf(void) { (void)tv_f64_to_int(-INFINITY, "Math.trunc", "m.tov:3:1"); }
static void trap_edge(void) { (void)tv_f64_to_int(9223372036854775808.0, "Math.ceil", "m.tov:4:1"); }

static bool dropped;
static void env_drop(tv_env *e) { (void)e, dropped = true; }

static void test_math_misc(void) {
    CHECK(tv_math_round(2.5) == 3 && tv_math_round(-2.5) == -2 && tv_math_round(1.5) == 2);
    CHECK(tv_math_round(-1.5) == -1 && tv_math_round(0.49999999999999994) == 0);
    CHECK(tv_math_round(-0.4) == 0 && signbit(tv_math_round(-0.4)));
    CHECK(signbit(tv_math_round(-0.0)) && !signbit(tv_math_round(0.4)));
    CHECK(tv_math_round(-0.5) == 0 && signbit(tv_math_round(-0.5)));
    CHECK(tv_math_round(4503599627370497.0) == 4503599627370497.0);
    CHECK(tv_math_round(-4503599627370497.0) == -4503599627370497.0);
    CHECK(isnan(tv_math_round(NAN)) && tv_math_round(INFINITY) == INFINITY);
    CHECK(tv_math_round(2.4999999999999996) == 2 && tv_math_round(-2.5000000000000004) == -3);

    CHECK(tv_f64_to_int(3.0, "Math.floor", "t") == 3 && tv_f64_to_int(-3.7, "x", "t") == -3);
    CHECK(tv_f64_to_int(-9223372036854775808.0, "x", "t") == INT64_MIN);
    CHECK(tv_f64_to_int(9223372036854774784.0, "x", "t") == 9223372036854774784LL);
    CHECK_TRAPS(trap_nan, "trap: Math.floor(NaN) is not an integer");
    CHECK_TRAPS(trap_big, "trap: Math.round(10000000000000000000) is out of the int range");
    CHECK_TRAPS(trap_inf, "trap: Math.trunc(-Infinity) is out of the int range");
    CHECK_TRAPS(trap_edge, "trap: Math.ceil(9223372036854776000) is out of the int range");
    CHECK_TRAPS(trap_to_fixed, "trap: toFixed() digits argument must be between 0 and 100");
    CHECK_TRAPS(trap_to_fixed_neg, "trap: toFixed() digits argument must be between 0 and 100");

    /* random: in [0,1), deterministic for a seed, reasonably spread */
    tv_random_seed(0);
    double first[8];
    for (int i = 0; i < 8; i++) first[i] = tv_random();
    tv_random_seed(0);
    bool same = true;
    for (int i = 0; i < 8; i++) same &= tv_random() == first[i];
    CHECK(same && first[0] != first[1]);
    int buckets[10] = {0};
    bool in_range = true;
    for (int i = 0; i < 100000; i++) {
        double r = tv_random();
        in_range &= r >= 0 && r < 1;
        buckets[(int)(r * 10)]++;
    }
    CHECK(in_range);
    for (int i = 0; i < 10; i++) CHECK(buckets[i] > 9000 && buckets[i] < 11000);

    /* closure environments */
    tv_env *e = tv_alloc(sizeof(tv_env));
    e->rc = 1;
    e->size = 0;
    e->drop = env_drop;
    tv_env_retain(e);
    tv_env_release(e);
    CHECK(!dropped);
    tv_env_release(e);
    CHECK(dropped);
    tv_env_retain(NULL);
    tv_env_release(NULL);
    static tv_env immortal = {-1, 0, env_drop};
    dropped = false;
    tv_env_release(&immortal);
    CHECK(!dropped);
    tv_env *e2 = tv_alloc(sizeof(tv_env));
    e2->rc = 1;
    e2->size = 0;
    e2->drop = NULL;
    tv_env_release(e2);

    /* realloc / free wrappers (a size of 0 the compiler can't see: GCC rejects tv_alloc(0)) */
    volatile size_t zero = 0;
    char *p = tv_alloc(zero);
    p = tv_realloc(p, 100);
    memset(p, 1, 100);
    tv_free(p);
    CHECK(tv_argc >= 1 && tv_argv != NULL);
}

/* ================================================================== processes: traps, output, runner */

static void child_trap(void) {
    tv_out_write("partial output\n", 15);
    tv_trap("boom", "main.tov:1:2");
}
static void child_trap_math(void) {
    tv_sb sb = {0};
    tv_sb_push_cstr(&sb, "before");
    tv_out_sb_line(&sb);
    (void)tv_f64_to_int(NAN, "Math.floor", "x.tov:2:3");
}
static void child_trap_noloc(void) { tv_trap("no location", NULL); }
static void child_output(void) {
    for (int i = 0; i < 20000; i++) { /* > 64 KiB, crosses buffer flushes */
        tv_sb sb = {0};
        tv_sb_push_cstr(&sb, "line ");
        tv_sb_push_int(&sb, i);
        tv_out_sb_line(&sb);
    }
    char big[70000];
    memset(big, 'z', sizeof big);
    tv_out_write(big, sizeof big); /* larger than the buffer */
    tv_sb sb = {0};
    tv_sb_push_cstr(&sb, "to stderr");
    tv_err_sb_line(&sb);
    tv_out_write("\nend\n", 5);
    exit(0); /* flushed by the atexit handler */
}

static void t_pass(void) {}
static void t_fail_expect(void) {
    tv_sb sb = {0};
    tv_sb_push_cstr(&sb, "expected 3, received 4");
    tv_expect_fail(&sb, "t.tov:3:5");
}
static void t_trap(void) { (void)tv_arr_at(TV_EMPTY_ARR, 8, 0, "t.tov:7:1"); }
static void t_trap_math(void) { (void)tv_f64_to_int(INFINITY, "Math.floor", "t.tov:9:1"); }
static void child_runner(void) {
    tv_test_run("passes", t_pass);
    tv_test_run("fails expect", t_fail_expect);
    tv_test_run("traps", t_trap);
    tv_test_run("traps again", t_trap_math);
    tv_test_run("passes again", t_pass);
    exit(tv_test_summary());
}
static void child_runner_all_pass(void) {
    tv_test_run("only", t_pass);
    exit(tv_test_summary());
}
static void child_expect_outside_test(void) {
    tv_sb sb = {0};
    tv_sb_push_cstr(&sb, "expected 1, received 2");
    tv_expect_fail(&sb, "e.tov:1:1");
}

static char out[1 << 20], err[1 << 16];

static void test_processes(void) {
    int st = run_child(child_trap, out, sizeof out, err, sizeof err);
    CHECK(st == 101);
    CHECK(strcmp(out, "partial output\n") == 0);
    CHECK(strcmp(err, "trap: boom\n  at main.tov:1:2\n") == 0);

    st = run_child(child_trap_math, out, sizeof out, err, sizeof err);
    CHECK(st == 101 && strcmp(out, "before\n") == 0);
    CHECK(strcmp(err, "trap: Math.floor(NaN) is not an integer\n  at x.tov:2:3\n") == 0);

    st = run_child(child_trap_noloc, out, sizeof out, err, sizeof err);
    CHECK(st == 101 && strcmp(err, "trap: no location\n") == 0);

    st = run_child(child_expect_outside_test, out, sizeof out, err, sizeof err);
    CHECK(st == 101 && strcmp(err, "trap: expected 1, received 2\n  at e.tov:1:1\n") == 0);

    st = run_child(child_output, out, sizeof out, err, sizeof err);
    CHECK(st == 0 && strcmp(err, "to stderr\n") == 0);
    CHECK(strncmp(out, "line 0\nline 1\n", 14) == 0 && strstr(out, "line 19999\nzzz") != NULL);
    size_t n = strlen(out);
    size_t want_n = 70000 + 5;
    for (int i = 0; i < 20000; i++) want_n += (size_t)snprintf(NULL, 0, "line %d\n", i);
    CHECK(n > 5 && strcmp(out + n - 5, "\nend\n") == 0 && n == want_n);

    st = run_child(child_runner, out, sizeof out, err, sizeof err);
    CHECK(st == 1);
    const char *want = "ok   passes\n"
                       "FAIL fails expect\n"
                       "  expected 3, received 4\n"
                       "  at t.tov:3:5\n"
                       "FAIL traps\n"
                       "  trap: index 0 out of bounds for length 0\n"
                       "  at t.tov:7:1\n"
                       "FAIL traps again\n"
                       "  trap: Math.floor(Infinity) is out of the int range\n"
                       "  at t.tov:9:1\n"
                       "ok   passes again\n"
                       "2 passed, 3 failed\n";
    checks++;
    if (strcmp(out, want) != 0) {
        failures++;
        fprintf(stderr, "runner output:\n%s---\n", out);
    }
    CHECK(err[0] == 0);

    st = run_child(child_runner_all_pass, out, sizeof out, err, sizeof err);
    CHECK(st == 0 && strcmp(out, "ok   only\n1 passed, 0 failed\n") == 0);
}

/* ================================================================== main */

/* ================================================================== async */

static tv_sb alog;   /* what ran, in order */

static void alog_push(const char *s) {
    if (alog.len) tv_sb_push_char(&alog, ' ');
    tv_sb_push_cstr(&alog, s);
}

static void cb_log(tv_env *env) { alog_push((const char *)((void **)(env + 1))[0]); }

/* A closure logging `word` (an env with the word after the header). */
static tv_fn log_fn(const char *word) {
    struct { tv_env h; const char *w; } *e = tv_alloc(sizeof *e);
    e->h.rc = 1;
    e->h.size = 0;
    e->h.drop = NULL;
    e->w = word;
    return (tv_fn){(void *)cb_log, &e->h};
}

/* A hand-written async function, as generated code does it: logs `name`, awaits `on` (if any),
 * logs `name` again with a '+' and returns. */
typedef struct {
    void *pc;
    const char *name;
    tv_promise *on;
    char after[32];
} waiter_frame;

static bool waiter_run(tv_task *t) {
    waiter_frame *F = tv_task_frame(t);
    if (F->pc) goto *F->pc;
    alog_push(F->name);
    if (F->on) {
        if (!tv_await_now(F->on)) {
            F->pc = &&resumed;
            tv_await_suspend(F->on);
            return false;
        }
    resumed:;
    }
    snprintf(F->after, sizeof F->after, "%s+", F->name);
    alog_push(F->after);
    tv_promise_resolve_move(t->promise, NULL);
    return true;
}

static tv_task *waiter(const char *name, tv_promise *on) {
    tv_task *t = tv_task_new(sizeof(waiter_frame), waiter_run, &tv_type_undefined);
    waiter_frame *F = tv_task_frame(t);
    F->name = name;
    F->on = on;
    return t;
}

static void test_async(void) {
    /* Microtasks run FIFO, including ones queued while the queue drains. */
    tv_queue_microtask(log_fn("m1"));
    tv_queue_microtask(log_fn("m2"));
    tv_run_microtasks();
    CHECK_SB(alog, "m1 m2");

    /* A spawned task (its caller is still running) yields at an await even when the promise is
     * settled; a task started by the loop continues at once when nothing else is queued. */
    tv_promise *done = tv_promise_new(&tv_type_undefined);
    tv_promise_resolve_move(done, NULL);
    tv_promise *p1 = tv_task_spawn(waiter("a", done));
    alog_push("caller");
    CHECK(p1->state == TV_PENDING);
    tv_run_microtasks();
    CHECK(p1->state == TV_FULFILLED);
    CHECK_SB(alog, "a caller a+");
    tv_task_start(waiter("b", done));
    CHECK_SB(alog, "b b+");
    /* ...but not when a microtask is queued: that one runs first. */
    tv_queue_microtask(log_fn("m"));
    tv_task_start(waiter("c", done));
    tv_run_microtasks();
    CHECK_SB(alog, "c m c+");
    tv_promise_release(p1);

    /* Waiters wake in the order they started waiting, when the promise settles. */
    tv_promise *gate = tv_promise_new(&tv_type_undefined);
    tv_promise *pa = tv_task_spawn(waiter("x", gate));
    tv_promise *pb = tv_task_spawn(waiter("y", gate));
    tv_promise *pc = tv_task_spawn(waiter("z", gate));
    tv_run_microtasks();
    CHECK_SB(alog, "x y z");
    tv_promise_resolve_move(gate, NULL);
    alog_push("resolved");
    tv_run_microtasks();
    CHECK_SB(alog, "resolved x+ y+ z+");
    CHECK(pa->state == TV_FULFILLED && pb->state == TV_FULFILLED && pc->state == TV_FULFILLED);
    tv_promise_release(pa);
    tv_promise_release(pb);
    tv_promise_release(pc);
    tv_promise_release(gate);
    tv_promise_release(done);

    /* Timers fire by due time, then in the order they were set; cleared ones never fire. */
    tv_fn t10 = log_fn("t10"), t1a = log_fn("t1a"), t1b = log_fn("t1b"), tx = log_fn("cleared");
    tv_set_timer(t10, 10, false);
    tv_set_timer(t1a, 1, false);
    tv_set_timer(t1b, 0, false);   /* 0 ms is 1 ms, as in Node */
    tv_clear_timer(tv_set_timer(tx, 2, false));
    tv_env_release(t10.env);
    tv_env_release(t1a.env);
    tv_env_release(t1b.env);
    tv_env_release(tx.env);
    tv_async_run();
    CHECK_SB(alog, "t1a t1b t10");

    /* A promise resolved by a timer wakes its task; values are released once. */
    tv_promise *pv = tv_promise_new(&obj_type);
    obj o = obj_new();
    tv_promise_resolve(pv, &o);
    obj_release(&o);
    CHECK(pv->state == TV_FULFILLED && *(obj *)tv_promise_value(pv) == o);
    tv_promise_release(pv);
    CHECK(all_dead());
}

/* A program done with many small strings of one size, going on to make strings of another: the
 * freed blocks are carved again (tv_spans_make), and nothing made from them overlaps. */
static void test_small_spans(void) {
    enum { N = 400000 };
    tv_str *a = malloc(N * sizeof *a), *b = malloc(N * sizeof *b);
    char buf[64];
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < N; i++) {
        int n = snprintf(buf, sizeof buf, "k%011d", i);
        a[i] = tv_str_from(buf, (size_t)n);
        uintptr_t p = (uintptr_t)a[i].p;
        if (p < lo) lo = p;
        if (p > hi) hi = p;
    }
    for (int i = 0; i < N; i++) tv_str_release(a[i]);
    int inside = 0;
    for (int i = 0; i < N; i++) {
        int n = snprintf(buf, sizeof buf, "longer-key-%013d", i);
        b[i] = tv_str_from(buf, (size_t)n);
        uintptr_t p = (uintptr_t)b[i].p;
        inside += p >= lo && p <= hi;
    }
    /* (every other one freed, and the first size made again between the rest) */
    for (int i = 0; i < N; i += 2) tv_str_release(b[i]);
    for (int i = 0; i < N; i += 2) {
        int n = snprintf(buf, sizeof buf, "k%011d", i);
        a[i] = tv_str_from(buf, (size_t)n);
    }
    bool ok = true;
    for (int i = 0; i < N; i++) {
        int n = i % 2 ? snprintf(buf, sizeof buf, "longer-key-%013d", i) : snprintf(buf, sizeof buf, "k%011d", i);
        tv_str s = i % 2 ? b[i] : a[i];
        ok = ok && s.p->len == n && memcmp(s.p->data, buf, (size_t)n) == 0;
        tv_str_release(s);
    }
    CHECK(ok);
#if !(defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer)))
    CHECK(inside > N / 2);
#else
    (void)inside;
#endif
    free(a);
    free(b);
}

int main(int argc, char **argv) {
    tv_init(argc, argv);
    if (argc > 1 && strcmp(argv[1], "trap") == 0) { /* used by test.sh to check the exit status */
        tv_out_write("flushed before trap\n", 20);
        (void)tv_arr_at(TV_EMPTY_ARR, 8, 5, "cli.tov:1:1");
    }
    test_string_basics();
    test_number_format();
    test_parse();
    test_string_methods();
    test_array_ownership();
    test_array_misc();
    test_array_header();
    test_map_basics();
    test_map_growth();
    test_map_ownership();
    test_map_keys_special();
    test_inspect();
    test_inspect_node_layout();
    test_math_misc();
    test_processes();
    test_async();
    test_small_spans();
    tv_sb_free(&alog);
    tv_sb_free(&tv_test_msg);
    CHECK(all_dead());
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
