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

static int mode_of(const char *t)    { struct pfe_config c; parse(t, &c); return c.mode; }
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
                                    "prefetch_enabler.skprx");
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
    expect("default mode off", c.mode, PFE_MODE_OFF);
    expect("default self_disable on", c.self_disable, 1);
    expect("default offset keep", c.pl310_offset, -1);
    expect("default bench.clocks max", c.bench_clocks_max, 1);
    pfe_config_masks(&c, &m);
    expect_hex("default masks all zero",
               m.actlr_set | m.actlr_clear | m.pl310_set | m.pl310_clear, 0);

    /* --- line endings and BOM: the whole point of this parser --- */
    expect("LF", mode_of("mode=apply\n"), PFE_MODE_APPLY);
    expect("CRLF", mode_of("mode=apply\r\n"), PFE_MODE_APPLY);
    expect("bare CR", mode_of("x=1\rmode=apply\r"), PFE_MODE_APPLY);
    expect("mixed LF/CRLF", mode_of("x=1\nmode=probe\r\n"), PFE_MODE_PROBE);
    expect("no trailing newline", mode_of("mode=bench"), PFE_MODE_BENCH);
    expect("CRLF blank lines", mode_of("\r\n\r\nmode=apply\r\n\r\n"), PFE_MODE_APPLY);
    expect("BOM + CRLF", mode_of("\xEF\xBB\xBF" "mode=apply\r\n"), PFE_MODE_APPLY);
    expect("BOM alone", mode_of("\xEF\xBB\xBF"), PFE_MODE_OFF);
    expect("BOM before dotted key",
           dside_of("\xEF\xBB\xBF" "actlr.l1_dside_prefetch=on\r\n"), PFE_ON);

    /* --- whitespace, case, comments --- */
    expect("spaces around =", mode_of("  mode  =  apply  \n"), PFE_MODE_APPLY);
    expect("tabs", mode_of("\tmode\t=\tapply\t\n"), PFE_MODE_APPLY);
    expect("uppercase", mode_of("MODE=APPLY\n"), PFE_MODE_APPLY);
    expect("comment line", mode_of("#mode=apply\n"), PFE_MODE_OFF);
    expect("indented comment", mode_of("   # mode=apply\n"), PFE_MODE_OFF);
    expect("trailing comment", mode_of("mode = apply   # do it\n"), PFE_MODE_APPLY);
    expect("trailing comment on tri",
           dside_of("actlr.l1_dside_prefetch = on # bit 2\n"), PFE_ON);

    /* --- mode --- */
    expect("mode garbage => off", mode_of("mode=fast\n"), PFE_MODE_OFF);
    expect("mode garbage counted", ignored_of("mode=fast\n"), 1);
    expect("mode last wins", mode_of("mode=apply\nmode=off\n"), PFE_MODE_OFF);
    expect("old key probe_only ignored", mode_of("probe_only=on\n"), PFE_MODE_OFF);
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
    parse("mode=apply\nactlr.l1_dside_prefetch=on\nactlr.l2_prefetch_hint=on\n"
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

    /* --- bench.clocks --- */
    parse("bench.clocks=keep\n", &c);  expect("bench.clocks keep", c.bench_clocks_max, 0);
    parse("bench.clocks=max\n", &c);   expect("bench.clocks max", c.bench_clocks_max, 1);
    parse("bench.clocks=500\n", &c);   expect("bench.clocks garbage => max", c.bench_clocks_max, 1);

    /* --- unknown lines are counted, never acted on --- */
    expect("no equals counted", ignored_of("mode apply\n"), 1);
    expect("unknown key counted", ignored_of("actlr.exclusive_cache=on\n"), 1);
    expect("blank and comment lines not counted", ignored_of("\n# x\n\n"), 0);

    /* The shipped example file must actually parse, every line recognised. */
    {
        FILE *f = fopen("prefetch_enabler.example.ini", "rb");
        if (!f) f = fopen("../prefetch_enabler.example.ini", "rb");
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
            expect("example: mode off as shipped", c.mode, PFE_MODE_OFF);
            expect("example: self_disable on", c.self_disable, 1);
            pfe_config_masks(&c, &m);
            expect_hex("example: ACTLR 0x149 -> 0x14B if applied",
                       (0x149u & ~m.actlr_clear) | m.actlr_set, 0x14B);
            expect_hex("example: PL310 -> 0x3100000F if applied",
                       (0x21000000u & ~m.pl310_clear) | m.pl310_set, 0x3100000F);
        }
    }

    /* --- dead-man's switch --- */
    check_comment("switch: LF",
                  "*KERNEL\nur0:tai/prefetch_enabler.skprx\n",
                  "*KERNEL\n#ur0:tai/prefetch_enabler.skprx\n", 1);
    check_comment("switch: CRLF preserved",
                  "*KERNEL\r\nur0:tai/prefetch_enabler.skprx\r\n",
                  "*KERNEL\r\n#ur0:tai/prefetch_enabler.skprx\r\n", 1);
    check_comment("switch: bare CR",
                  "*KERNEL\rur0:tai/prefetch_enabler.skprx\r",
                  "*KERNEL\r#ur0:tai/prefetch_enabler.skprx\r", 1);
    check_comment("switch: no trailing newline",
                  "*KERNEL\r\nur0:tai/prefetch_enabler.skprx",
                  "*KERNEL\r\n#ur0:tai/prefetch_enabler.skprx", 1);
    check_comment("switch: already commented is left alone",
                  "#ur0:tai/prefetch_enabler.skprx\n",
                  "#ur0:tai/prefetch_enabler.skprx\n", 0);
    check_comment("switch: indentation preserved",
                  "  ur0:tai/prefetch_enabler.skprx\n",
                  "#  ur0:tai/prefetch_enabler.skprx\n", 1);
    check_comment("switch: absent",
                  "*KERNEL\nur0:tai/other.skprx\n",
                  "*KERNEL\nur0:tai/other.skprx\n", 0);
    check_comment("switch: two copies, both commented",
                  "ur0:tai/prefetch_enabler.skprx\nux0:tai/prefetch_enabler.skprx\n",
                  "#ur0:tai/prefetch_enabler.skprx\n#ux0:tai/prefetch_enabler.skprx\n",
                  2);
    check_comment("switch: other plugins untouched",
                  "*KERNEL\na.skprx\nur0:tai/prefetch_enabler.skprx\nb.skprx\n",
                  "*KERNEL\na.skprx\n#ur0:tai/prefetch_enabler.skprx\nb.skprx\n", 1);

    /* No room for the '#': must refuse and change nothing. */
    {
        char tight[31];
        const char *in = "ur0:tai/prefetch_enabler.skprx";  /* exactly 30 */
        int n = (int)strlen(in);
        memcpy(tight, in, (size_t)n);
        int ret = pfe_comment_out_lines(tight, &n, n, "prefetch_enabler.skprx");
        if (ret != -1 || n != 30 || memcmp(tight, in, 30) != 0) {
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
