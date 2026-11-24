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

static int contains_n(const char *hay, int haylen, const char *needle)
{
    for (int i = 0; i < haylen; i++) {
        int j = 0;
        while (needle[j] && i + j < haylen && hay[i + j] == needle[j]) j++;
        if (needle[j] == '\0') return 1;
    }
    return 0;
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

    if (streq_ci(key, keylen, "mode")) {
        if      (streq_ci(val, vallen, "off"))   out->mode = PFE_MODE_OFF;
        else if (streq_ci(val, vallen, "apply")) out->mode = PFE_MODE_APPLY;
        else if (streq_ci(val, vallen, "probe")) out->mode = PFE_MODE_PROBE;
        else if (streq_ci(val, vallen, "bench")) out->mode = PFE_MODE_BENCH;
        else { out->mode = PFE_MODE_OFF; out->ignored++; }
    } else if (streq_ci(key, keylen, "self_disable")) {
        /* Only an explicit "no" turns the dead-man's switch off. */
        if      (streq_ci(val, vallen, "no"))  out->self_disable = 0;
        else if (streq_ci(val, vallen, "yes")) out->self_disable = 1;
        else { out->self_disable = 1; out->ignored++; }
    } else if (streq_ci(key, keylen, "pl310.prefetch_offset")) {
        if (streq_ci(val, vallen, "keep")) { out->pl310_offset = -1; return; }
        int v = parse_small_uint(val, vallen);
        if (pfe_l2_offset_valid(v)) out->pl310_offset = v;
        else out->ignored++;
    } else if (streq_ci(key, keylen, "bench.timing")) {
        if      (streq_ci(val, vallen, "boot")) out->bench_at_boot = 1;
        else if (streq_ci(val, vallen, "home")) out->bench_at_boot = 0;
        else out->ignored++;
    } else if (streq_ci(key, keylen, "bench.clocks")) {
        if      (streq_ci(val, vallen, "max"))  out->bench_clocks_max = 1;
        else if (streq_ci(val, vallen, "keep")) out->bench_clocks_max = 0;
        else out->ignored++;
    } else {
        out->ignored++;
    }
}

void pfe_parse_config(const char *buf, int n, struct pfe_config *out)
{
    out->mode = PFE_MODE_OFF;
    out->self_disable = 1;
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

int pfe_config_disable_bench(char *buf, int *io_n)
{
    if (!buf || !io_n || *io_n <= 0) return 0;
    int n = *io_n, changed = 0;
    int pos = skip_bom(buf, n), start = 0, len = 0;

    while (next_line(buf, n, &pos, &start, &len)) {
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
        if (!streq_ci(key, keylen, "mode")) continue;

        const char *val = line + eq + 1;
        int vallen = l - eq - 1;
        trim(&val, &vallen);
        for (int i = 0; i < vallen; i++)
            if (val[i] == '#') { vallen = i; trim(&val, &vallen); break; }
        if (!streq_ci(val, vallen, "bench")) continue;

        /* "bench" (5) -> "off" (3): overwrite, then close the 2-byte gap. */
        int at = (int)(val - buf);
        buf[at] = 'o'; buf[at + 1] = 'f'; buf[at + 2] = 'f';
        for (int i = at + 5; i < n; i++) buf[i - 2] = buf[i];
        n -= 2;
        pos -= 2;               /* the line we just shortened ends 2 earlier */
        changed++;
    }
    *io_n = n;
    return changed;
}

int pfe_comment_out_lines(char *buf, int *io_n, int cap, const char *needle)
{
    if (!buf || !io_n || *io_n <= 0) return 0;

    int n = *io_n;

    /* Count first, so a buffer that cannot hold every '#' leaves the caller's
     * file completely untouched rather than half-edited. */
    int hits = 0;
    {
        int pos = 0, start = 0, len = 0;
        while (next_line(buf, n, &pos, &start, &len)) {
            const char *tl = buf + start;
            int tlen = len;
            trim(&tl, &tlen);
            if (tlen > 0 && tl[0] != '#' && contains_n(tl, tlen, needle)) hits++;
        }
    }
    if (hits == 0) return 0;
    if (n + hits > cap) return -1;

    int done = 0;
    int pos = 0, start = 0, len = 0;
    while (next_line(buf, n, &pos, &start, &len)) {
        const char *tl = buf + start;
        int tlen = len;
        trim(&tl, &tlen);
        if (!(tlen > 0 && tl[0] != '#' && contains_n(tl, tlen, needle))) continue;

        for (int i = n; i > start; i--) buf[i] = buf[i - 1];
        buf[start] = '#';
        n++;
        pos++;      /* this line's terminator just shifted right by one */
        done++;
    }

    *io_n = n;
    return done;
}
