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

enum pfe_mode {
    PFE_MODE_OFF   = 0,  /* do nothing (the default) */
    PFE_MODE_APPLY = 1,  /* apply the register fields below */
    PFE_MODE_PROBE = 2,  /* read-only recon, no secure writes */
    PFE_MODE_BENCH = 3   /* memory benchmark 30 s after boot, then restore */
};

/* The register bits the config can drive. */
#define PFE_ACTLR_L2_PREFETCH_HINT     (1u << 1)
#define PFE_ACTLR_L1_DSIDE_PREFETCH    (1u << 2)
#define PFE_ACTLR_WRITE_FULL_ZEROS     (1u << 3)
#define PFE_ACTLR_ALLOC_ONE_WAY        (1u << 8)

#define PFE_PL310_DOUBLE_LINEFILL      (1u << 30)
#define PFE_PL310_INSTR_PREFETCH       (1u << 29)
#define PFE_PL310_DATA_PREFETCH        (1u << 28)
#define PFE_PL310_PREFETCH_DROP        (1u << 24)
#define PFE_PL310_INCR_DOUBLE_LINEFILL (1u << 23)
#define PFE_PL310_OFFSET_MASK          0x1Fu

/* One on|off|keep key per single-bit hardware field. */
struct pfe_field {
    const char  *key;     /* config key, e.g. "actlr.l1_dside_prefetch" */
    unsigned int bit;     /* mask within its register */
};

#define PFE_ACTLR_NFIELDS 4
#define PFE_PL310_NFIELDS 5
extern const struct pfe_field pfe_actlr_fields[PFE_ACTLR_NFIELDS];
extern const struct pfe_field pfe_pl310_fields[PFE_PL310_NFIELDS];

struct pfe_config {
    int mode;                         /* enum pfe_mode */
    int self_disable;                 /* 1 unless self_disable=no */
    int actlr[PFE_ACTLR_NFIELDS];     /* enum pfe_tri, same order as pfe_actlr_fields */
    int pl310[PFE_PL310_NFIELDS];     /* enum pfe_tri, same order as pfe_pl310_fields */
    int pl310_offset;                 /* -1 = keep, else 0-7, 15, 23 or 31 */
    int bench_clocks_max;             /* bench.clocks: 1 = max (default), 0 = keep */
    int ignored;                      /* lines with an unknown key or bad value */
};

/* Set/clear masks per register, compiled from the tri-states. */
struct pfe_masks {
    unsigned int actlr_set, actlr_clear;
    unsigned int pl310_set, pl310_clear;
};

void pfe_config_masks(const struct pfe_config *cfg, struct pfe_masks *m);

/* 0-7, 15, 23 and 31 are the L2C-310 TRM's valid prefetch offsets. 23 is
 * valid but hits ARM errata 765569 on this PL310 revision - accepted, and
 * warned about in the example config and the log. */
int pfe_l2_offset_valid(int v);

/*
 * Parses `n` bytes of `key = value` lines from ur0:data/prefetch_enabler.txt.
 *
 * Agnostic to line endings (LF, CRLF and bare CR all work), to a leading
 * UTF-8 BOM, to leading/trailing spaces and tabs around both key and value,
 * and to key/value case. Lines whose first non-blank character is '#' are
 * comments. Unrecognised keys and values are ignored and counted in
 * `ignored`, leaving that field at its default - mode off, every register
 * field PFE_KEEP - so a malformed file can never turn something on or change
 * a firmware setting.
 *
 * Every field of *out is initialised, whatever the input.
 */
/* Size of the on-device read buffer. A larger file is refused loudly rather
 * than parsed truncated; the host tests check the shipped example fits even
 * with CRLF line endings. */
#define PFE_CONFIG_MAX_BYTES 16384

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
