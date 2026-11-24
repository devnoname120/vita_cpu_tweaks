#include "config.h"

/* Trims leading/trailing spaces and tabs from [*s,*len). Line terminators are
 * handled by the line splitter, so a trailing '\r' is also trimmed here for
 * safety rather than relied upon. */
static void trim(const char **s, int *len)
{
    while (*len > 0 && (**s == ' ' || **s == '\t')) { (*s)++; (*len)--; }
    while (*len > 0 && ((*s)[*len - 1] == ' ' || (*s)[*len - 1] == '\t' ||
                        (*s)[*len - 1] == '\r')) (*len)--;
}

static int streq_ci(const char *a, int alen, const char *b)
{
    int blen = 0;
    while (b[blen]) blen++;
    if (alen != blen) return 0;
    for (int i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return 1;
}

/* A UTF-8 BOM. Windows editors add one silently, and without this the first
 * key of the file would never match. */
static int skip_bom(const char *buf, int n)
{
    if (n >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        return 3;
    return 0;
}

static int is_eol(char c) { return c == '\n' || c == '\r'; }

/*
 * Finds the next line in [*pos, n): sets start and len to its extent,
 * excluding the terminator, and advances *pos past it. Treats LF, CRLF and
 * bare CR alike - a CRLF is consumed as one terminator so it does not yield a
 * spurious empty line. Returns 0 when there is nothing left.
 */
static int next_line(const char *buf, int n, int *pos, int *start, int *len)
{
    if (*pos >= n) return 0;
    *start = *pos;
    while (*pos < n && !is_eol(buf[*pos])) (*pos)++;
    *len = *pos - *start;
    if (*pos < n) {
        char c = buf[*pos];
        (*pos)++;
        if (c == '\r' && *pos < n && buf[*pos] == '\n') (*pos)++;  /* CRLF */
    }
    return 1;
}

const struct pfe_field pfe_actlr_fields[PFE_ACTLR_NFIELDS] = {
    { "actlr.l1_dside_prefetch",    PFE_ACTLR_L1_DSIDE_PREFETCH },
    { "actlr.l2_prefetch_hint",     PFE_ACTLR_L2_PREFETCH_HINT },
    { "actlr.write_full_zeros",     PFE_ACTLR_WRITE_FULL_ZEROS },
    { "actlr.alloc_one_way",        PFE_ACTLR_ALLOC_ONE_WAY },
};

const struct pfe_field pfe_pl310_fields[PFE_PL310_NFIELDS] = {
    { "pl310.data_prefetch",        PFE_PL310_DATA_PREFETCH },
    { "pl310.instr_prefetch",       PFE_PL310_INSTR_PREFETCH },
    { "pl310.prefetch_drop",        PFE_PL310_PREFETCH_DROP },
    { "pl310.double_linefill",      PFE_PL310_DOUBLE_LINEFILL },
    { "pl310.incr_double_linefill", PFE_PL310_INCR_DOUBLE_LINEFILL },
};

/* -1 for anything that is not on/off/keep, so it can be counted as ignored. */
static int parse_tri(const char *val, int vallen)
{
    if (streq_ci(val, vallen, "on"))   return PFE_ON;
    if (streq_ci(val, vallen, "off"))  return PFE_OFF;
    if (streq_ci(val, vallen, "keep")) return PFE_KEEP;
    return -1;
}

int pfe_l2_offset_valid(int v)
{
    return (v >= 0 && v <= 7) || v == 15 || v == 23 || v == 31;
}

/* Decimal 0-99, nothing else; -1 if the value is not that. */
static int parse_small_uint(const char *val, int vallen)
{
    if (vallen < 1 || vallen > 2) return -1;
    int v = 0;
    for (int i = 0; i < vallen; i++) {
        if (val[i] < '0' || val[i] > '9') return -1;
        v = v * 10 + (val[i] - '0');
    }
    return v;
}

/* Sets *slot from an on/off/keep value, or counts the line as ignored. */
static void set_tri(int *slot, const char *val, int vallen, int *ignored)
{
    int t = parse_tri(val, vallen);
    if (t < 0) { (*ignored)++; return; }
    *slot = t;
}

/* on -> 1, off -> 0; anything else leaves the field and counts as ignored.
 * A bad value can therefore never turn something on. */
static void set_run(int *slot, const char *val, int vallen, int *ignored)
{
    if      (streq_ci(val, vallen, "off"))  *slot = PFE_RUN_OFF;
    else if (streq_ci(val, vallen, "on"))   *slot = PFE_RUN_ON;
    else if (streq_ci(val, vallen, "once")) *slot = PFE_RUN_ONCE;
    else (*ignored)++;
}

static void set_onoff(int *slot, const char *val, int vallen, int *ignored)
{
    if      (streq_ci(val, vallen, "on"))  *slot = 1;
    else if (streq_ci(val, vallen, "off")) *slot = 0;
    else (*ignored)++;
}

static void apply_key(struct pfe_config *out, const char *key, int keylen,
                      const char *val, int vallen)
{
    for (int i = 0; i < PFE_ACTLR_NFIELDS; i++)
        if (streq_ci(key, keylen, pfe_actlr_fields[i].key)) {
            set_tri(&out->actlr[i], val, vallen, &out->ignored);
            return;
        }
    for (int i = 0; i < PFE_PL310_NFIELDS; i++)
        if (streq_ci(key, keylen, pfe_pl310_fields[i].key)) {
            set_tri(&out->pl310[i], val, vallen, &out->ignored);
            return;
        }

    if (streq_ci(key, keylen, "enable")) {
        set_run(&out->enable, val, vallen, &out->ignored);
    } else if (streq_ci(key, keylen, "log")) {
        set_onoff(&out->log, val, vallen, &out->ignored);
    } else if (streq_ci(key, keylen, "probe")) {
        /* off | once: a read-only diagnostic has no reason to run every boot */
        if (streq_ci(val, vallen, "on")) out->ignored++;
        else set_run(&out->probe, val, vallen, &out->ignored);
    } else if (streq_ci(key, keylen, "benchmark")) {
        if      (streq_ci(val, vallen, "once")) out->bench = 1;
        else if (streq_ci(val, vallen, "off"))  out->bench = 0;
        else out->ignored++;
    } else if (streq_ci(key, keylen, "pl310.prefetch_offset")) {
        if (streq_ci(val, vallen, "keep")) { out->pl310_offset = -1; return; }
        int v = parse_small_uint(val, vallen);
        if (pfe_l2_offset_valid(v)) out->pl310_offset = v;
        else out->ignored++;
    } else if (streq_ci(key, keylen, "benchmark.timing")) {
        if      (streq_ci(val, vallen, "boot")) out->bench_at_boot = 1;
        else if (streq_ci(val, vallen, "home")) out->bench_at_boot = 0;
        else out->ignored++;
    } else if (streq_ci(key, keylen, "benchmark.clocks")) {
        if      (streq_ci(val, vallen, "max"))  out->bench_clocks_max = 1;
        else if (streq_ci(val, vallen, "keep")) out->bench_clocks_max = 0;
        else out->ignored++;
    } else {
        out->ignored++;
    }
}

void pfe_parse_config(const char *buf, int n, struct pfe_config *out)
{
    out->enable = PFE_RUN_OFF;
    out->probe = PFE_RUN_OFF;
    out->bench = 0;
    out->log = 1;
    for (int i = 0; i < PFE_ACTLR_NFIELDS; i++) out->actlr[i] = PFE_KEEP;
    for (int i = 0; i < PFE_PL310_NFIELDS; i++) out->pl310[i] = PFE_KEEP;
    out->pl310_offset = -1;
    out->bench_clocks_max = 1;
    out->bench_at_boot = 0;
    out->ignored = 0;

    if (!buf || n <= 0) return;

    int pos = skip_bom(buf, n);
    int start = 0, len = 0;

    while (next_line(buf, n, &pos, &start, &len)) {
        const char *line = buf + start;
        trim(&line, &len);
        if (len == 0 || line[0] == '#') continue;

        int eq = -1;
        for (int i = 0; i < len; i++) {
            if (line[i] == '=') { eq = i; break; }
        }
        if (eq < 0) { out->ignored++; continue; }

        const char *key = line;
        int keylen = eq;
        trim(&key, &keylen);
        const char *val = line + eq + 1;
        int vallen = len - eq - 1;
        trim(&val, &vallen);

        /* A trailing "# comment" after the value is allowed. */
        for (int i = 0; i < vallen; i++)
            if (val[i] == '#') { vallen = i; trim(&val, &vallen); break; }

        apply_key(out, key, keylen, val, vallen);
    }
}

void pfe_config_masks(const struct pfe_config *cfg, struct pfe_masks *m)
{
    m->actlr_set = m->actlr_clear = m->pl310_set = m->pl310_clear = 0;
    for (int i = 0; i < PFE_ACTLR_NFIELDS; i++) {
        if (cfg->actlr[i] == PFE_ON)  m->actlr_set   |= pfe_actlr_fields[i].bit;
        if (cfg->actlr[i] == PFE_OFF) m->actlr_clear |= pfe_actlr_fields[i].bit;
    }
    for (int i = 0; i < PFE_PL310_NFIELDS; i++) {
        if (cfg->pl310[i] == PFE_ON)  m->pl310_set   |= pfe_pl310_fields[i].bit;
        if (cfg->pl310[i] == PFE_OFF) m->pl310_clear |= pfe_pl310_fields[i].bit;
    }
    if (cfg->pl310_offset >= 0) {
        m->pl310_clear |= PFE_PL310_OFFSET_MASK;
        m->pl310_set   |= (unsigned int)cfg->pl310_offset & PFE_PL310_OFFSET_MASK;
    }
}

static const char *const once_keys[] = { "enable", "probe", "benchmark" };

/* Finds the value of the next active `<once key> = once` line at or after
 * *pos. Returns its offset in buf, or -1. Advances *pos past that line. */
static int find_once(const char *buf, int n, int *pos)
{
    int start = 0, len = 0;
    while (next_line(buf, n, pos, &start, &len)) {
        const char *line = buf + start;
        int l = len;
        trim(&line, &l);
        if (l == 0 || line[0] == '#') continue;

        int eq = -1;
        for (int i = 0; i < l; i++) if (line[i] == '=') { eq = i; break; }
        if (eq < 0) continue;

        const char *key = line;
        int keylen = eq;
        trim(&key, &keylen);
        int is_once_key = 0;
        for (unsigned k = 0; k < sizeof(once_keys) / sizeof(once_keys[0]); k++)
            if (streq_ci(key, keylen, once_keys[k])) is_once_key = 1;
        if (!is_once_key) continue;

        const char *val = line + eq + 1;
        int vallen = l - eq - 1;
        trim(&val, &vallen);
        for (int i = 0; i < vallen; i++)
            if (val[i] == '#') { vallen = i; trim(&val, &vallen); break; }
        if (streq_ci(val, vallen, "once")) return (int)(val - buf);
    }
    return -1;
}

int pfe_config_consume_once(char *buf, int *io_n)
{
    if (!buf || !io_n || *io_n <= 0) return 0;
    int n = *io_n, changed = 0;
    int pos = skip_bom(buf, n), at;
    while ((at = find_once(buf, n, &pos)) >= 0) {
        /* "once" (4) -> "off" (3): overwrite, then close the 1-byte gap. */
        buf[at] = 'o'; buf[at + 1] = 'f'; buf[at + 2] = 'f';
        for (int i = at + 4; i < n; i++) buf[i - 1] = buf[i];
        n--;
        pos--;                  /* the line we just shortened ends 1 earlier */
        changed++;
    }
    *io_n = n;
    return changed;
}

int pfe_config_has_once(const struct pfe_config *cfg)
{
    return cfg->enable == PFE_RUN_ONCE || cfg->probe == PFE_RUN_ONCE || cfg->bench;
}
