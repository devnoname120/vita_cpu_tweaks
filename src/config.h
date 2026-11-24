/*
 * Pure text parsing for the plugin's two config files, with no vitasdk or
 * psp2spl dependency, so src/tests/test_config.c can exercise it on the host.
 * All file I/O stays in main.c; these take and return buffers.
 */
#ifndef PFE_CONFIG_H
#define PFE_CONFIG_H

struct pfe_config {
    int want_l1;
    int want_l2;
    int probe_only;
};

/*
 * Parses `n` bytes of `key = value` lines from ur0:data/prefetch_enabler.txt.
 *
 * Agnostic to line endings (LF, CRLF and bare CR all work), to a leading
 * UTF-8 BOM, to leading/trailing spaces and tabs around both key and value,
 * and to key/value case. Lines whose first non-blank character is '#' are
 * comments. Unrecognised keys and values are ignored, leaving that field at
 * its "off" default - so a malformed file can never turn something on.
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
