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

static void check_cfg(const char *name, const char *text, int n,
                      int l1, int l2, int probe)
{
    struct pfe_config c;
    if (n < 0) n = (int)strlen(text);
    pfe_parse_config(text, n, &c);
    if (c.want_l1 != l1 || c.want_l2 != l2 || c.probe_only != probe) {
        printf("FAIL %-44s l1=%d l2=%d probe=%d (want %d/%d/%d)\n",
               name, c.want_l1, c.want_l2, c.probe_only, l1, l2, probe);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

static void check_tri(const char *name, const char *text, int want)
{
    struct pfe_config c;
    pfe_parse_config(text, (int)strlen(text), &c);
    if (c.alloc_one_way != want) {
        printf("FAIL %-44s alloc_one_way=%d (want %d)\n",
               name, c.alloc_one_way, want);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

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
        printf("FAIL %-44s ret=%d (want %d)\n", name, ret, want_ret);
        printf("       got  <%.*s>\n", n, buf);
        printf("       want <%s>\n", want);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

int main(void)
{
    /* --- line endings: the whole point of this file --- */
    check_cfg("LF", "l1_prefetch=on\nl2_prefetch=on\n", -1, 1, 1, 0);
    check_cfg("CRLF", "l1_prefetch=on\r\nl2_prefetch=on\r\n", -1, 1, 1, 0);
    check_cfg("bare CR", "l1_prefetch=on\rl2_prefetch=on\r", -1, 1, 1, 0);
    check_cfg("mixed LF/CRLF", "l1_prefetch=on\nl2_prefetch=on\r\n", -1, 1, 1, 0);
    check_cfg("CRLF, no trailing newline",
              "l1_prefetch=on\r\nl2_prefetch=on", -1, 1, 1, 0);
    check_cfg("LF, no trailing newline", "l2_prefetch=on", -1, 0, 1, 0);
    check_cfg("CRLF does not leak into the value",
              "l1_prefetch=on\r\n", -1, 1, 0, 0);
    check_cfg("CRLF blank lines", "\r\n\r\nl1_prefetch=on\r\n\r\n", -1, 1, 0, 0);

    /* --- UTF-8 BOM, as added by Windows editors --- */
    check_cfg("BOM + CRLF", "\xEF\xBB\xBFl1_prefetch=on\r\n", -1, 1, 0, 0);
    check_cfg("BOM + LF", "\xEF\xBB\xBFprobe_only=on\n", -1, 0, 0, 1);
    check_cfg("BOM alone", "\xEF\xBB\xBF", -1, 0, 0, 0);

    /* --- whitespace, case, comments --- */
    check_cfg("spaces around =", "  l1_prefetch  =  on  \n", -1, 1, 0, 0);
    check_cfg("tabs", "\tl2_prefetch\t=\ton\t\n", -1, 0, 1, 0);
    check_cfg("uppercase key and value", "L1_PREFETCH=ON\n", -1, 1, 0, 0);
    check_cfg("mixed case value", "l1_prefetch=On\n", -1, 1, 0, 0);
    check_cfg("comment", "#l1_prefetch=on\n", -1, 0, 0, 0);
    check_cfg("indented comment", "   # l1_prefetch=on\n", -1, 0, 0, 0);
    check_cfg("comment after CRLF", "probe_only=on\r\n#l1_prefetch=on\r\n",
              -1, 0, 0, 1);

    /* --- anything unrecognised must stay off, never on --- */
    check_cfg("empty", "", 0, 0, 0, 0);
    check_cfg("garbage value", "l1_prefetch=yes\n", -1, 0, 0, 0);
    check_cfg("value 1 is not 'on'", "l1_prefetch=1\n", -1, 0, 0, 0);
    check_cfg("no equals", "l1_prefetch on\n", -1, 0, 0, 0);
    check_cfg("unknown key", "l3_prefetch=on\n", -1, 0, 0, 0);
    check_cfg("key is a prefix, not equal", "l1_prefetch_x=on\n", -1, 0, 0, 0);
    check_cfg("empty value", "l1_prefetch=\n", -1, 0, 0, 0);
    check_cfg("last wins", "l1_prefetch=on\nl1_prefetch=off\n", -1, 0, 0, 0);
    check_cfg("last wins, other way",
              "l1_prefetch=off\nl1_prefetch=on\n", -1, 1, 0, 0);
    check_cfg("all three", "probe_only=on\nl1_prefetch=on\nl2_prefetch=on\n",
              -1, 1, 1, 1);

    /* The shipped example file must actually parse - this is the regression
     * test for the handout whose keys sat past the old 256-byte buffer. */
    {
        FILE *f = fopen("prefetch_enabler.txt.example", "rb");
        if (!f) f = fopen("../prefetch_enabler.txt.example", "rb");
        if (!f) {
            printf("FAIL %-44s cannot open example config\n", "example file");
            failures++;
        } else {
            static char big[8192];
            int n = (int)fread(big, 1, sizeof(big), f);
            fclose(f);
            struct pfe_config c;
            pfe_parse_config(big, n, &c);
            /* every key present and recognised => all three default to off */
            if (c.want_l1 || c.want_l2 || c.probe_only) {
                printf("FAIL %-44s example turns something on\n", "example file");
                failures++;
            } else {
                printf("ok   example file parses (%d bytes)\n", n);
            }
        }
    }

    /* --- tri-state: "leave it alone" must differ from "clear it" --- */
    check_tri("tri: absent => KEEP", "l1_prefetch=on\n", PFE_KEEP);
    check_tri("tri: on", "actlr_alloc_one_way=on\n", PFE_ON);
    check_tri("tri: off", "actlr_alloc_one_way=off\n", PFE_OFF);
    check_tri("tri: OFF uppercase", "ACTLR_ALLOC_ONE_WAY=OFF\n", PFE_OFF);
    check_tri("tri: CRLF", "actlr_alloc_one_way=off\r\n", PFE_OFF);
    /* split: 'a' is a hex digit, so it would extend the \xBF escape */
    check_tri("tri: BOM+CRLF", "\xEF\xBB\xBF" "actlr_alloc_one_way=off\r\n", PFE_OFF);
    check_tri("tri: spaces", "  actlr_alloc_one_way = off  \n", PFE_OFF);
    check_tri("tri: garbage => KEEP", "actlr_alloc_one_way=maybe\n", PFE_KEEP);
    check_tri("tri: empty => KEEP", "actlr_alloc_one_way=\n", PFE_KEEP);
    check_tri("tri: commented => KEEP", "#actlr_alloc_one_way=off\n", PFE_KEEP);
    check_tri("tri: last wins",
              "actlr_alloc_one_way=off\nactlr_alloc_one_way=on\n", PFE_ON);
    check_tri("tri: does not match l1_prefetch key",
              "l1_prefetch=off\n", PFE_KEEP);

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
