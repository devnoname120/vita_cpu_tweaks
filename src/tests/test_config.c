/*
 * Host-side tests for src/config.c. Builds and runs with the system cc, no
 * vitasdk needed:
 *
 *   cc -Wall -Wextra -O1 -o /tmp/t src/config.c src/tests/test_config.c && /tmp/t
 *
 * or just `src/tests/run.sh`.
 */
#include "../config.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void parse(const char *text, struct pfe_config *c)
{
    pfe_parse_config(text, (int)strlen(text), c);
}

static void expect(const char *name, int got, int want)
{
    if (got != want) {
        printf("FAIL %-48s got %d, want %d\n", name, got, want);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

static void expect_hex(const char *name, unsigned int got, unsigned int want)
{
    if (got != want) {
        printf("FAIL %-48s got 0x%08X, want 0x%08X\n", name, got, want);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

static int enable_of(const char *t)  { struct pfe_config c; parse(t, &c); return c.enable; }
static int bench_of(const char *t)   { struct pfe_config c; parse(t, &c); return c.bench; }
static int ignored_of(const char *t) { struct pfe_config c; parse(t, &c); return c.ignored; }
static int offset_of(const char *t)  { struct pfe_config c; parse(t, &c); return c.pl310_offset; }
static int dside_of(const char *t)   { struct pfe_config c; parse(t, &c); return c.actlr[0]; }

static void check_comment(const char *name, const char *in, const char *want,
                          int want_ret)
{
    char buf[512];
    int n = (int)strlen(in);
    memcpy(buf, in, (size_t)n);
    int ret = pfe_comment_out_lines(buf, &n, (int)sizeof(buf),
                                    "cpu_tweaks.skprx");
    int wn = (int)strlen(want);
    if (ret != want_ret || n != wn || memcmp(buf, want, (size_t)wn) != 0) {
        printf("FAIL %-48s ret=%d (want %d)\n", name, ret, want_ret);
        printf("       got  <%.*s>\n", n, buf);
        printf("       want <%s>\n", want);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

int main(void)
{
    struct pfe_config c;
    struct pfe_masks m;

    /* --- defaults: nothing happens unless asked --- */
    pfe_parse_config(NULL, 0, &c);
    expect("default enable off", c.enable, 0);
    expect("default probe off", c.probe, 0);
    expect("default bench off", c.bench, 0);
    expect("default self_disable on", c.self_disable, 1);
    expect("default offset keep", c.pl310_offset, -1);
    expect("default benchmark.clocks max", c.bench_clocks_max, 1);
    pfe_config_masks(&c, &m);
    expect_hex("default masks all zero",
               m.actlr_set | m.actlr_clear | m.pl310_set | m.pl310_clear, 0);

    /* --- line endings and BOM: the whole point of this parser --- */
    expect("LF", enable_of("enable=on\n"), 1);
    expect("CRLF", enable_of("enable=on\r\n"), 1);
    expect("bare CR", enable_of("x=1\renable=on\r"), 1);
    expect("mixed LF/CRLF", enable_of("x=1\nenable=on\r\n"), 1);
    expect("no trailing newline", enable_of("enable=on"), 1);
    expect("CRLF blank lines", enable_of("\r\n\r\nenable=on\r\n\r\n"), 1);
    expect("BOM + CRLF", enable_of("\xEF\xBB\xBF" "enable=on\r\n"), 1);
    expect("BOM alone", enable_of("\xEF\xBB\xBF"), 0);
    expect("BOM before dotted key",
           dside_of("\xEF\xBB\xBF" "actlr.l1_dside_prefetch=on\r\n"), PFE_ON);

    /* --- whitespace, case, comments --- */
    expect("spaces around =", enable_of("  enable  =  on  \n"), 1);
    expect("tabs", enable_of("\tenable\t=\ton\t\n"), 1);
    expect("uppercase", enable_of("ENABLE=ON\n"), 1);
    expect("comment line", enable_of("#enable=on\n"), 0);
    expect("indented comment", enable_of("   # enable=on\n"), 0);
    expect("trailing comment", enable_of("enable = on   # do it\n"), 1);
    expect("trailing comment on tri",
           dside_of("actlr.l1_dside_prefetch = on # bit 2\n"), PFE_ON);

    /* --- enable / probe / bench switches --- */
    expect("enable garbage => off", enable_of("enable=yes\n"), 0);
    expect("enable garbage counted", ignored_of("enable=yes\n"), 1);
    expect("enable garbage keeps earlier value", enable_of("enable=on\nenable=1\n"), 1);
    expect("enable last wins", enable_of("enable=on\nenable=off\n"), 0);
    { struct pfe_config t; parse("probe=on\n", &t); expect("probe on", t.probe, 1); }
    expect("benchmark once", bench_of("benchmark = once\r\n"), 1);
    expect("benchmark ONCE", bench_of("BENCHMARK = ONCE\n"), 1);
    expect("benchmark off", bench_of("benchmark = off\n"), 0);
    expect("benchmark on is not a value", bench_of("benchmark = on\n"), 0);
    expect("benchmark on counted as ignored", ignored_of("benchmark = on\n"), 1);
    expect("benchmark.timing is not benchmark", bench_of("benchmark.timing = boot\n"), 0);
    expect("old key bench ignored", bench_of("bench = on\n"), 0);
    expect("old key mode ignored", ignored_of("mode=apply\n"), 1);
    expect("old key probe_only ignored", enable_of("probe_only=on\n"), 0);
    expect("old keys counted as ignored",
           ignored_of("probe_only=on\nl1_prefetch=on\nl2_prefetch=on\n"), 3);

    /* --- self_disable: only an explicit no turns the switch off --- */
    parse("self_disable=no\n", &c);      expect("self_disable=no", c.self_disable, 0);
    parse("self_disable=NO\r\n", &c);   expect("self_disable=NO CRLF", c.self_disable, 0);
    parse("self_disable=yes\n", &c);     expect("self_disable=yes", c.self_disable, 1);
    parse("self_disable=off\n", &c);     expect("self_disable=off is not no", c.self_disable, 1);
    parse("self_disable=0\n", &c);       expect("self_disable=0 is not no", c.self_disable, 1);
    parse("#self_disable=no\n", &c);     expect("self_disable commented", c.self_disable, 1);

    /* --- tri-states: keep must differ from off --- */
    expect("tri on", dside_of("actlr.l1_dside_prefetch=on\n"), PFE_ON);
    expect("tri off", dside_of("actlr.l1_dside_prefetch=off\n"), PFE_OFF);
    expect("tri keep", dside_of("actlr.l1_dside_prefetch=keep\n"), PFE_KEEP);
    expect("tri garbage => keep", dside_of("actlr.l1_dside_prefetch=yes\n"), PFE_KEEP);
    expect("tri garbage counted", ignored_of("actlr.l1_dside_prefetch=1\n"), 1);
    expect("tri garbage keeps earlier value",
           dside_of("actlr.l1_dside_prefetch=on\nactlr.l1_dside_prefetch=1\n"), PFE_ON);
    expect("prefix of key is not the key", dside_of("actlr.l1_dside=on\n"), PFE_KEEP);
    expect("key without register prefix", dside_of("l1_dside_prefetch=on\n"), PFE_KEEP);

    /* --- every field maps to the right bit --- */
    parse("actlr.l1_dside_prefetch=on\nactlr.l2_prefetch_hint=on\n"
          "actlr.write_full_zeros=off\nactlr.alloc_one_way=off\n", &c);
    pfe_config_masks(&c, &m);
    expect_hex("actlr set", m.actlr_set, 0x6);
    expect_hex("actlr clear", m.actlr_clear, 0x108);
    expect_hex("actlr only touches actlr", m.pl310_set | m.pl310_clear, 0);

    parse("pl310.data_prefetch=on\npl310.instr_prefetch=off\n"
          "pl310.prefetch_drop=off\npl310.double_linefill=on\n"
          "pl310.incr_double_linefill=off\npl310.prefetch_offset=15\n", &c);
    pfe_config_masks(&c, &m);
    expect_hex("pl310 set", m.pl310_set, 0x5000000F);
    expect_hex("pl310 clear", m.pl310_clear, 0x2180001F);
    expect_hex("pl310 only touches pl310", m.actlr_set | m.actlr_clear, 0);

    /* The measured-best settings reproduce boot 36's register values. */
    parse("enable=on\nactlr.l1_dside_prefetch=on\nactlr.l2_prefetch_hint=on\n"
          "pl310.data_prefetch=on\npl310.prefetch_offset=15\n", &c);
    pfe_config_masks(&c, &m);
    expect_hex("best: ACTLR 0x149 -> 0x14F",
               (0x149u & ~m.actlr_clear) | m.actlr_set, 0x14F);
    expect_hex("best: PL310 0x21000000 -> 0x3100000F",
               (0x21000000u & ~m.pl310_clear) | m.pl310_set, 0x3100000F);

    /* --- prefetch offset --- */
    expect("offset 0", offset_of("pl310.prefetch_offset=0\n"), 0);
    expect("offset 7", offset_of("pl310.prefetch_offset=7\n"), 7);
    expect("offset 15", offset_of("pl310.prefetch_offset=15\n"), 15);
    expect("offset 23 accepted (warned about)", offset_of("pl310.prefetch_offset=23\n"), 23);
    expect("offset 31 CRLF", offset_of("pl310.prefetch_offset=31\r\n"), 31);
    expect("offset keep", offset_of("pl310.prefetch_offset=keep\n"), -1);
    expect("offset 8 not a TRM value", offset_of("pl310.prefetch_offset=8\n"), -1);
    expect("offset 32", offset_of("pl310.prefetch_offset=32\n"), -1);
    expect("offset hex", offset_of("pl310.prefetch_offset=0x7\n"), -1);
    expect("offset negative", offset_of("pl310.prefetch_offset=-1\n"), -1);
    expect("offset 3 digits", offset_of("pl310.prefetch_offset=007\n"), -1);
    expect("offset empty", offset_of("pl310.prefetch_offset=\n"), -1);
    expect("offset bad counted", ignored_of("pl310.prefetch_offset=8\n"), 1);
    parse("pl310.prefetch_offset=0\n", &c);
    pfe_config_masks(&c, &m);
    expect_hex("offset 0 still clears the field", m.pl310_clear, 0x1F);

    /* --- log --- */
    parse("", &c);              expect("log default on", c.log, 1);
    parse("log = off\r\n", &c); expect("log off", c.log, 0);
    parse("log = on\n", &c);    expect("log on", c.log, 1);
    parse("log = no\n", &c);    expect("log garbage => stays on", c.log, 1);
    expect("log garbage counted", ignored_of("log = no\n"), 1);
    expect("log.x is not log", ignored_of("log.x = off\n"), 1);

    /* --- benchmark.clocks --- */
    parse("benchmark.clocks=keep\n", &c);  expect("benchmark.clocks keep", c.bench_clocks_max, 0);
    parse("benchmark.clocks=max\n", &c);   expect("benchmark.clocks max", c.bench_clocks_max, 1);
    parse("benchmark.clocks=500\n", &c);   expect("benchmark.clocks garbage => max", c.bench_clocks_max, 1);

    /* --- benchmark.timing --- */
    parse("", &c);                      expect("benchmark.timing default home", c.bench_at_boot, 0);
    parse("benchmark.timing=boot\n", &c);  expect("benchmark.timing boot", c.bench_at_boot, 1);
    parse("benchmark.timing = BOOT\r\n", &c); expect("benchmark.timing BOOT CRLF", c.bench_at_boot, 1);
    parse("benchmark.timing=home\n", &c);  expect("benchmark.timing home", c.bench_at_boot, 0);
    parse("benchmark.timing=now\n", &c);   expect("benchmark.timing garbage => home", c.bench_at_boot, 0);
    expect("benchmark.timing garbage counted", ignored_of("benchmark.timing=now\n"), 1);

    /* --- benchmark = once becomes benchmark = off --- */
    {
        static const struct { const char *name, *in, *want; int ret; } t[] = {
            { "once: plain", "benchmark=once\n", "benchmark=off\n", 1 },
            { "once: spacing and comment kept",
              "benchmark = once    # once | off\r\n",
              "benchmark = off    # once | off\r\n", 1 },
            { "once: uppercase", "BENCHMARK = ONCE\n", "BENCHMARK = off\n", 1 },
            { "once: BOM, no newline", "\xEF\xBB\xBF" "benchmark=once",
              "\xEF\xBB\xBF" "benchmark=off", 1 },
            { "once: commented line untouched", "#benchmark = once\n", "#benchmark = once\n", 0 },
            { "once: already off", "benchmark = off\n", "benchmark = off\n", 0 },
            { "once: enable and sub-keys untouched",
              "enable = on\nbenchmark.timing = boot\nbenchmark = once\nbenchmark.clocks = max\n",
              "enable = on\nbenchmark.timing = boot\nbenchmark = off\nbenchmark.clocks = max\n", 1 },
            { "once: two lines",
              "benchmark=once\r\nbenchmark=once\r\n", "benchmark=off\r\nbenchmark=off\r\n", 2 },
            { "once: bare CR", "a=1\rbenchmark=once\rb=2\r", "a=1\rbenchmark=off\rb=2\r", 1 },
        };
        for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
            char b[256];
            int n = (int)strlen(t[i].in);
            memcpy(b, t[i].in, (size_t)n);
            int r = pfe_config_disable_bench(b, &n);
            int ok = r == t[i].ret && n == (int)strlen(t[i].want) &&
                     memcmp(b, t[i].want, (size_t)n) == 0;
            if (!ok) {
                printf("FAIL %-48s ret=%d got <%.*s>\n", t[i].name, r, n, b);
                failures++;
            } else {
                printf("ok   %s\n", t[i].name);
            }
        }
        /* Afterwards the parser sees the benchmark off and enable untouched. */
        char b[128] = "enable = on\r\nbenchmark = once  # x\r\nbenchmark.timing = boot\r\n";
        int n = (int)strlen(b);
        pfe_config_disable_bench(b, &n);
        pfe_parse_config(b, n, &c);
        expect("once: benchmark off afterwards", c.bench, 0);
        expect("once: enable still on", c.enable, 1);
        expect("once: timing kept", c.bench_at_boot, 1);
        expect("once: nothing ignored", c.ignored, 0);
    }

    /* --- unknown lines are counted, never acted on --- */
    expect("no equals counted", ignored_of("enable on\n"), 1);
    expect("unknown key counted", ignored_of("actlr.exclusive_cache=on\n"), 1);
    expect("blank and comment lines not counted", ignored_of("\n# x\n\n"), 0);

    /* The shipped example file must actually parse, every line recognised. */
    {
        FILE *f = fopen("cpu_tweaks.example.ini", "rb");
        if (!f) f = fopen("../cpu_tweaks.example.ini", "rb");
        if (!f) {
            printf("FAIL %-48s cannot open example config\n", "example file");
            failures++;
        } else {
            static char big[8192];
            int n = (int)fread(big, 1, sizeof(big), f);
            fclose(f);
            pfe_parse_config(big, n, &c);
            expect("example: every line recognised", c.ignored, 0);
            int lines = 0;
            for (int i = 0; i < n; i++) lines += big[i] == '\n';
            expect("example fits the device buffer even as CRLF",
                   n + lines <= PFE_CONFIG_MAX_BYTES, 1);
            expect("example: enable off as shipped", c.enable, 0);
            expect("example: probe off as shipped", c.probe, 0);
            expect("example: benchmark off as shipped", c.bench, 0);
            expect("example: self_disable on", c.self_disable, 1);
            pfe_config_masks(&c, &m);
            expect_hex("example: ACTLR 0x149 -> 0x14B if applied",
                       (0x149u & ~m.actlr_clear) | m.actlr_set, 0x14B);
            expect_hex("example: PL310 -> 0x3100000F if applied",
                       (0x21000000u & ~m.pl310_clear) | m.pl310_set, 0x3100000F);
        }
    }

    /* The tester handout's config must parse too, as step 1 (probe only). */
    {
        FILE *f = fopen("docs/for-testers.config.ini", "rb");
        if (!f) f = fopen("../docs/for-testers.config.ini", "rb");
        if (!f) {
            printf("FAIL %-48s cannot open tester config\n", "tester config");
            failures++;
        } else {
            static char big[8192];
            int n = (int)fread(big, 1, sizeof(big), f);
            fclose(f);
            pfe_parse_config(big, n, &c);
            expect("tester config: every line recognised", c.ignored, 0);
            expect("tester config: probe on as shipped", c.probe, 1);
            expect("tester config: enable off as shipped", c.enable, 0);
            expect("tester config: benchmark off as shipped", c.bench, 0);
            expect("tester config: self_disable on", c.self_disable, 1);
            expect("tester config: log on", c.log, 1);
        }
    }

    /* The tester config must offer every key the example does, with the same
     * values apart from probe - so testers see the whole set of settings. */
    {
        static char ex[8192], te[8192];
        FILE *f1 = fopen("cpu_tweaks.example.ini", "rb");
        if (!f1) f1 = fopen("../cpu_tweaks.example.ini", "rb");
        FILE *f2 = fopen("docs/for-testers.config.ini", "rb");
        if (!f2) f2 = fopen("../docs/for-testers.config.ini", "rb");
        if (!f1 || !f2) {
            printf("FAIL %-48s cannot open configs\n", "tester vs example keys");
            failures++;
        } else {
            int n1 = (int)fread(ex, 1, sizeof(ex) - 1, f1);
            int n2 = (int)fread(te, 1, sizeof(te) - 1, f2);
            ex[n1] = te[n2] = 0;
            /* every "key =" line at column 0 in one file appears in the other */
            int missing = 0;
            for (int pass = 0; pass < 2; pass++) {
                const char *a = pass ? te : ex, *b = pass ? ex : te;
                for (const char *l = a; *l; ) {
                    const char *e = strchr(l, '\n');
                    int len = e ? (int)(e - l) : (int)strlen(l);
                    if (len > 0 && l[0] >= 'a' && l[0] <= 'z') {
                        int k = 0;
                        while (k < len && l[k] != ' ' && l[k] != '=') k++;
                        char key[64];
                        snprintf(key, sizeof(key), "\n%.*s ", k, l);
                        if (!strstr(b, key)) {
                            printf("     key %s missing from %s\n", key + 1,
                                   pass ? "example" : "tester config");
                            missing++;
                        }
                    }
                    l = e ? e + 1 : l + len;
                }
            }
            expect("tester config offers every example key", missing, 0);
            struct pfe_config ce, ct;
            struct pfe_masks me, mt;
            pfe_parse_config(ex, n1, &ce);
            pfe_parse_config(te, n2, &ct);
            pfe_config_masks(&ce, &me);
            pfe_config_masks(&ct, &mt);
            expect("tester config: same register values as example",
                   me.actlr_set == mt.actlr_set && me.actlr_clear == mt.actlr_clear &&
                   me.pl310_set == mt.pl310_set && me.pl310_clear == mt.pl310_clear, 1);
        }
        if (f1) fclose(f1);
        if (f2) fclose(f2);
    }

    /* --- dead-man's switch --- */
    check_comment("switch: LF",
                  "*KERNEL\nur0:tai/cpu_tweaks.skprx\n",
                  "*KERNEL\n#ur0:tai/cpu_tweaks.skprx\n", 1);
    check_comment("switch: CRLF preserved",
                  "*KERNEL\r\nur0:tai/cpu_tweaks.skprx\r\n",
                  "*KERNEL\r\n#ur0:tai/cpu_tweaks.skprx\r\n", 1);
    check_comment("switch: bare CR",
                  "*KERNEL\rur0:tai/cpu_tweaks.skprx\r",
                  "*KERNEL\r#ur0:tai/cpu_tweaks.skprx\r", 1);
    check_comment("switch: no trailing newline",
                  "*KERNEL\r\nur0:tai/cpu_tweaks.skprx",
                  "*KERNEL\r\n#ur0:tai/cpu_tweaks.skprx", 1);
    check_comment("switch: already commented is left alone",
                  "#ur0:tai/cpu_tweaks.skprx\n",
                  "#ur0:tai/cpu_tweaks.skprx\n", 0);
    check_comment("switch: indentation preserved",
                  "  ur0:tai/cpu_tweaks.skprx\n",
                  "#  ur0:tai/cpu_tweaks.skprx\n", 1);
    check_comment("switch: absent",
                  "*KERNEL\nur0:tai/other.skprx\n",
                  "*KERNEL\nur0:tai/other.skprx\n", 0);
    check_comment("switch: two copies, both commented",
                  "ur0:tai/cpu_tweaks.skprx\nux0:tai/cpu_tweaks.skprx\n",
                  "#ur0:tai/cpu_tweaks.skprx\n#ux0:tai/cpu_tweaks.skprx\n",
                  2);
    check_comment("switch: other plugins untouched",
                  "*KERNEL\na.skprx\nur0:tai/cpu_tweaks.skprx\nb.skprx\n",
                  "*KERNEL\na.skprx\n#ur0:tai/cpu_tweaks.skprx\nb.skprx\n", 1);

    /* No room for the '#': must refuse and change nothing. */
    {
        char tight[64];
        const char *in = "ur0:tai/cpu_tweaks.skprx";
        const int len = (int)strlen(in);
        int n = len;
        memcpy(tight, in, (size_t)n);
        /* cap == current length: no room for the extra '#' */
        int ret = pfe_comment_out_lines(tight, &n, len, "cpu_tweaks.skprx");
        if (ret != -1 || n != len || memcmp(tight, in, (size_t)len) != 0) {
            printf("FAIL %-44s ret=%d n=%d\n", "switch: refuses when full", ret, n);
            failures++;
        } else {
            printf("ok   switch: refuses when full, leaves buffer intact\n");
        }
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
