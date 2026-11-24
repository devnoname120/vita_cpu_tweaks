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

int main(void)
{
    struct pfe_config c;
    struct pfe_masks m;

    /* --- defaults: nothing happens unless asked --- */
    pfe_parse_config(NULL, 0, &c);
    expect("default enable off", c.enable, 0);
    expect("default probe off", c.probe, 0);
    expect("default bench off", c.bench, 0);
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
    expect("enable on", enable_of("enable = on\n"), PFE_RUN_ON);
    expect("enable once", enable_of("enable = once\r\n"), PFE_RUN_ONCE);
    expect("enable ONCE", enable_of("ENABLE = ONCE\n"), PFE_RUN_ONCE);
    expect("enable off", enable_of("enable = off\n"), PFE_RUN_OFF);
    { struct pfe_config t; parse("probe = once\n", &t); expect("probe once", t.probe, PFE_RUN_ONCE); }
    { struct pfe_config t; parse("probe = off\n", &t);  expect("probe off", t.probe, PFE_RUN_OFF); }
    { struct pfe_config t; parse("probe = on\n", &t);   expect("probe on is not a value", t.probe, PFE_RUN_OFF); }
    expect("probe on counted as ignored", ignored_of("probe = on\n"), 1);
    expect("old key self_disable ignored", ignored_of("self_disable = no\n"), 1);
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

    /* --- has_once --- */
    parse("enable = on\n", &c);       expect("has_once: on is not once", pfe_config_has_once(&c), 0);
    parse("enable = once\n", &c);     expect("has_once: enable once", pfe_config_has_once(&c), 1);
    parse("probe = once\n", &c);      expect("has_once: probe once", pfe_config_has_once(&c), 1);
    parse("benchmark = once\n", &c);  expect("has_once: benchmark once", pfe_config_has_once(&c), 1);
    parse("", &c);                    expect("has_once: empty", pfe_config_has_once(&c), 0);

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

    /* --- "once" becomes "off": enable, probe and benchmark --- */
    {
        static const struct { const char *name, *in, *want; int ret; } t[] = {
            { "once: benchmark", "benchmark=once\n", "benchmark=off\n", 1 },
            { "once: enable", "enable = once\n", "enable = off\n", 1 },
            { "once: probe", "probe = once\n", "probe = off\n", 1 },
            { "once: spacing and comment kept",
              "enable = once    # off | on | once\r\n",
              "enable = off    # off | on | once\r\n", 1 },
            { "once: uppercase", "BENCHMARK = ONCE\n", "BENCHMARK = off\n", 1 },
            { "once: BOM, no newline", "\xEF\xBB\xBF" "enable=once",
              "\xEF\xBB\xBF" "enable=off", 1 },
            { "once: commented line untouched", "#enable = once\n", "#enable = once\n", 0 },
            { "once: on untouched", "enable = on\n", "enable = on\n", 0 },
            { "once: all three, others untouched",
              "enable = once\nprobe = once\nlog = on\nbenchmark.timing = boot\nbenchmark = once\nbenchmark.clocks = max\n",
              "enable = off\nprobe = off\nlog = on\nbenchmark.timing = boot\nbenchmark = off\nbenchmark.clocks = max\n", 3 },
            { "once: other key with value once untouched",
              "x = once\nbenchmark.timing = once\n", "x = once\nbenchmark.timing = once\n", 0 },
            { "once: two lines CRLF",
              "enable=once\r\nbenchmark=once\r\n", "enable=off\r\nbenchmark=off\r\n", 2 },
            { "once: bare CR", "a=1\rprobe=once\rb=2\r", "a=1\rprobe=off\rb=2\r", 1 },
        };
        for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
            char b[256];
            int n = (int)strlen(t[i].in);
            memcpy(b, t[i].in, (size_t)n);
            int r = pfe_config_consume_once(b, &n);
            int ok = r == t[i].ret && n == (int)strlen(t[i].want) &&
                     memcmp(b, t[i].want, (size_t)n) == 0;
            if (!ok) {
                printf("FAIL %-48s ret=%d got <%.*s>\n", t[i].name, r, n, b);
                failures++;
            } else {
                printf("ok   %s\n", t[i].name);
            }
        }
        /* Afterwards nothing says once, and "on" settings are untouched. */
        char b[160] = "enable = once  # x\r\nprobe = once\r\nbenchmark = once\r\nbenchmark.timing = boot\r\n";
        int n = (int)strlen(b);
        pfe_config_consume_once(b, &n);
        pfe_parse_config(b, n, &c);
        expect("once: nothing once afterwards", pfe_config_has_once(&c), 0);
        expect("once: enable off afterwards", c.enable, PFE_RUN_OFF);
        expect("once: timing kept", c.bench_at_boot, 1);
        expect("once: nothing ignored", c.ignored, 0);
        char b2[64] = "enable = on\nbenchmark = once\n";
        n = (int)strlen(b2);
        pfe_config_consume_once(b2, &n);
        pfe_parse_config(b2, n, &c);
        expect("once: enable = on survives a benchmark", c.enable, PFE_RUN_ON);
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
            pfe_config_masks(&c, &m);
            expect_hex("example: ACTLR 0x149 -> 0x14B if applied",
                       (0x149u & ~m.actlr_clear) | m.actlr_set, 0x14B);
            expect_hex("example: PL310 -> 0x3100000F if applied",
                       (0x21000000u & ~m.pl310_clear) | m.pl310_set, 0x3100000F);
        }
    }

    /* The tester handout's config must parse too, as step 1 (probe once). */
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
            expect("tester config: probe once as shipped", c.probe, PFE_RUN_ONCE);
            expect("tester config: enable off as shipped", c.enable, 0);
            expect("tester config: benchmark off as shipped", c.bench, 0);
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

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
