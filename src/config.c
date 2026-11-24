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

static int parse_tri(const char *val, int vallen)
{
    if (streq_ci(val, vallen, "on"))  return PFE_ON;
    if (streq_ci(val, vallen, "off")) return PFE_OFF;
    return PFE_KEEP;
}

void pfe_parse_config(const char *buf, int n, struct pfe_config *out)
{
    out->want_l1 = 0;
    out->want_l2 = 0;
    out->probe_only = 0;
    out->alloc_one_way = PFE_KEEP;
    out->bench = 0;

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
        if (eq < 0) continue;

        const char *key = line;
        int keylen = eq;
        trim(&key, &keylen);
        const char *val = line + eq + 1;
        int vallen = len - eq - 1;
        trim(&val, &vallen);

        int on = streq_ci(val, vallen, "on");
        if (streq_ci(key, keylen, "l1_prefetch"))      out->want_l1 = on;
        else if (streq_ci(key, keylen, "l2_prefetch")) out->want_l2 = on;
        else if (streq_ci(key, keylen, "probe_only"))  out->probe_only = on;
        else if (streq_ci(key, keylen, "bench"))       out->bench = on;
        else if (streq_ci(key, keylen, "actlr_alloc_one_way"))
            out->alloc_one_way = parse_tri(val, vallen);
    }
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
