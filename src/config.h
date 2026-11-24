/*
 * Pure text parsing for the plugin's config file, with no vitasdk or
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

/*
 * off | on | once. "once" is the safety net: the plugin rewrites the line to
 * "off" in its own config file before doing anything risky, so a boot that
 * hangs is recovered by a plain power-cycle.
 */
enum pfe_run {
    PFE_RUN_OFF  = 0,
    PFE_RUN_ON   = 1,
    PFE_RUN_ONCE = 2
};

struct pfe_config {
    int enable;                       /* enable: enum pfe_run - apply the register fields */
    int probe;                        /* probe:  PFE_RUN_OFF or PFE_RUN_ONCE - read-only recon */
    int bench;                        /* benchmark: 1 = once (then reboot), 0 = off */
    int log;                          /* log: 1 = on (default), 0 = off */
    int actlr[PFE_ACTLR_NFIELDS];     /* enum pfe_tri, same order as pfe_actlr_fields */
    int pl310[PFE_PL310_NFIELDS];     /* enum pfe_tri, same order as pfe_pl310_fields */
    int pl310_offset;                 /* -1 = keep, else 0-7, 15, 23 or 31 */
    int bench_clocks_max;             /* benchmark.clocks: 1 = max (default), 0 = keep */
    int bench_at_boot;                /* benchmark.timing: 1 = boot, 0 = home (default) */
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
 * Parses `n` bytes of `key = value` lines from ur0:data/cpu_tweaks.ini.
 *
 * Agnostic to line endings (LF, CRLF and bare CR all work), to a leading
 * UTF-8 BOM, to leading/trailing spaces and tabs around both key and value,
 * and to key/value case. Lines whose first non-blank character is '#' are
 * comments. Unrecognised keys and values are ignored and counted in
 * `ignored`, leaving that field at its default - enable/probe/benchmark off,
 * every register field PFE_KEEP - so a malformed file can never turn
 * something on or change a firmware setting.
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
/*
 * Rewrites every active `enable = once`, `probe = once` and
 * `benchmark = once` line in buf to `... = off`, in place, keeping spacing,
 * case of the key, line endings and any trailing comment. Each value shrinks
 * by one byte, so *io_n only goes down. Returns the number of lines changed.
 * This is what makes "once" mean once.
 */
int pfe_config_consume_once(char *buf, int *io_n);

/* 1 if any of enable / probe / benchmark is "once". */
int pfe_config_has_once(const struct pfe_config *cfg);

#endif /* PFE_CONFIG_H */
