/*
 * Pure text parsing for the plugin's two config files, with no vitasdk or
 * psp2spl dependency, so src/tests/test_config.c can exercise it on the host.
 * All file I/O stays in main.c; these take and return buffers.
 */
#ifndef PFE_CONFIG_H
#define PFE_CONFIG_H

/*
 * Tri-state, for fields where "leave whatever the firmware chose" has to be
 * distinguishable from "actively clear this". Plain on/off keys cannot express
 * that, which matters for any bit the firmware sets rather than clears.
 */
enum pfe_tri {
    PFE_KEEP = 0,   /* absent, or an unrecognised value */
    PFE_ON   = 1,
    PFE_OFF  = 2
};

struct pfe_config {
    int want_l1;        /* l1_prefetch:   ACTLR bits 2 and 1 */
    int want_l2;        /* l2_prefetch:   PL310 PREFETCH_CTRL bit 28 */
    int probe_only;     /* probe_only:    read-only recon, no secure writes */
    int alloc_one_way;  /* actlr_alloc_one_way: ACTLR bit 8, enum pfe_tri */
    int bench;          /* bench:         run the memory benchmark suite */
};

/*
 * Parses `n` bytes of `key = value` lines from ur0:data/prefetch_enabler.txt.
 *
 * Agnostic to line endings (LF, CRLF and bare CR all work), to a leading
 * UTF-8 BOM, to leading/trailing spaces and tabs around both key and value,
 * and to key/value case. Lines whose first non-blank character is '#' are
 * comments. Unrecognised keys and values are ignored, leaving that field at
 * its default - "off" for the booleans, PFE_KEEP for the tri-states - so a
 * malformed file can never turn something on or change a firmware setting.
 *
 * Every field of *out is initialised, whatever the input.
 */
void pfe_parse_config(const char *buf, int n, struct pfe_config *out);

/*
 * Comments out every line of `buf` that is not already a comment and that
 * contains `needle`, by inserting a '#' at the line's first byte. Used for
 * the dead-man's switch on ur0:tai/config.txt.
 *
 * Recognises LF, CRLF and bare CR as line terminators, and preserves
 * whichever was used - only a '#' is ever inserted, nothing is rewritten.
 * Inserting at the raw line start (not the trimmed start) keeps any original
 * indentation intact.
 *
 * `*io_n` is the length on entry and is updated to the new length. `cap` is
 * the total capacity of `buf`. Returns the number of lines commented out, or
 * -1 if there was not enough room, in which case `buf` is left untouched.
 */
int pfe_comment_out_lines(char *buf, int *io_n, int cap, const char *needle);

#endif /* PFE_CONFIG_H */
