/*
 * vita_cpu_tweaks - enable the Cortex-A9 / PL310 hardware prefetchers
 * on a PS Vita, from a config.txt-loaded taiHEN kernel plugin.
 *
 * The Vita's kernel_boot_loader explicitly disables both hardware
 * prefetchers at boot. This plugin sets the prefetch-related fields of the
 * A9's ACTLR and the PL310's PREFETCH_CTRL as its config file says:
 *
 *     ur0:data/cpu_tweaks.ini      (documented: cpu_tweaks.example.ini)
 *
 *     enable = off | on | once
 *     actlr.l2_prefetch_hint = on
 *     pl310.data_prefetch = on
 *     pl310.prefetch_offset = 15
 *     ...one on | off | keep key per register field
 *
 * Parsing lives in config.c and is host-tested. Anything not understood -
 * a missing key, a bad value, an unreadable file - leaves that setting
 * alone: a boot-time module that can't read its own config does nothing.
 *
 * Dead-man's switch: "once" (for enable, probe and benchmark) is rewritten
 * to "off" in the config file, and confirmed by reading it back, before any
 * secure-world work. If that boot hangs or crashes, a plain power cycle
 * comes back with it off. "on" has no such net; it is for settings that
 * have already worked as "once".
 *
 * With nothing enabled the plugin touches NO secure-world state at all.
 *
 * Requires SKGleba's psp2spl (https://github.com/SKGleba/psp2spl) for
 * secure-world (TrustZone) code execution - see the top-level README for
 * the full writeup, the hardware this was verified on, and its limits.
 * PLEASE READ THE "Compatibility" SECTION THERE BEFORE FLASHING THIS ONTO
 * A UNIT WHOSE FIRMWARE WASN'T ALREADY CONFIRMED.
 *
 * Writes a plain-text log to ur0:data/cpu_tweaks.log every boot it runs
 * (log = on, the default), or only when something goes wrong (log = off).
 */
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/power.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/debug.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <stdarg.h>

#define SPL_CFK_LV0SMALL
#define SPL_CFK_TZSFULL
#include "libspl-cfk.h"
#include "config.h"
#include "bench.h"

#define LOG(...) ksceDebugPrintf("[CPUT] " __VA_ARGS__)

void *memset(void *dst, int c, unsigned int n)
{
    volatile unsigned char *p = (volatile unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

#define CONFIG_PATH "ur0:data/cpu_tweaks.ini"
/* A bench run's own copy of the log. The reboot that ends a bench is followed
 * by a normal boot, which - with the plugin enabled - would overwrite LOG_PATH. */
#define BENCH_LOG_PATH "ur0:data/cpu_tweaks_bench.log"
#define LOG_PATH    "ur0:data/cpu_tweaks.log"


/*
 * ACTLR/PL310 set stubs - executed in place, through a fresh execute-only
 * physical alias of wherever the module loader actually put them (see
 * map_own_code_rx() below). Pure register/immediate ARM, no data or
 * external symbol references - position-independent by construction, so
 * it doesn't matter which virtual address they're fetched through, only
 * that the physical bytes are reachable from secure-world execution.
 */
static const unsigned int actlr_set[4] = {
    0xEE110F30u,  /* MRC p15,0,r0,c1,c0,1 (read ACTLR) */
    0xE3800006u,  /* ORR r0,r0,#6          (set bits [1,2]) */
    0xEE010F30u,  /* MCR p15,0,r0,c1,c0,1 (write ACTLR) */
    0xE12FFF1Eu,  /* BX lr */
};

/*
 * The stub actually executed: a read-modify-write of ACTLR with arbitrary
 * set and clear masks.
 *
 * The masks are loaded PC-relative from two literals in the same scratch page
 * as the code, rather than being baked into ORR/BIC immediates - an ARM imm12
 * is an 8-bit value with an even rotation, so it cannot encode every mask
 * (0x106, say). The page is mapped Normal Non-Cacheable, so the literal loads
 * and the instruction fetch both read DRAM directly and no maintenance is
 * needed for either.
 *
 * For an ARM literal load the address is PC + 8 + imm12:
 *   word 1 at +0x04 -> 0x04 + 8 + 0x10 = 0x1C (clear mask)
 *   word 3 at +0x0C -> 0x0C + 8 + 0x0C = 0x20 (set mask)
 */
#define ACTLR_STUB_WORDS 9

static void build_actlr_stub(unsigned int *w, unsigned int set_mask,
                             unsigned int clear_mask)
{
    w[0] = 0xEE110F30u;  /* +00  MRC p15,0,r0,c1,c0,1   read ACTLR      */
    w[1] = 0xE59F1010u;  /* +04  LDR r1,[pc,#16]        r1 = clear_mask */
    w[2] = 0xE1C00001u;  /* +08  BIC r0,r0,r1                           */
    w[3] = 0xE59F100Cu;  /* +0C  LDR r1,[pc,#12]        r1 = set_mask   */
    w[4] = 0xE1800001u;  /* +10  ORR r0,r0,r1                           */
    w[5] = 0xEE010F30u;  /* +14  MCR p15,0,r0,c1,c0,1   write ACTLR     */
    w[6] = 0xE12FFF1Eu;  /* +18  BX lr                                  */
    w[7] = clear_mask;   /* +1C                                         */
    w[8] = set_mask;     /* +20                                         */
}

/* The ACTLR and PL310 field bits themselves live in config.h (PFE_ACTLR_*,
 * PFE_PL310_*), next to the config keys that drive them. */

extern int splCl_smc(unsigned int a0, unsigned int a1, unsigned int a2,
                     unsigned int a3, unsigned int idx);
#define SPLTZ_SMC_EXEC_WITH_SP 0x401u

/* Secure read/write primitives (libspl.h gates these behind an ifdef that may
 * not be set in this build; unconditionally present in the vendored libspl.a,
 * verified with nm). Both take a *secure* VA. */
extern int SPLx_TZS_READ32(unsigned int sva, unsigned int *rval);
extern int SPLx_TZS_WRITE32(unsigned int sva, unsigned int val);

/* PL310 L2C-310 PREFETCH_CTRL. Base 0x1A002000 is identity-mapped secure RW
 * (boot 9 PRINTTBR: L1_SECTn 0x1A000000 1M, AP=001), so its secure VA equals
 * its PA. Reached by a secure write instead of secure code execution. */
#define PL310_PREFETCH_CTRL_SVA 0x1A002F60u

/* Sized for a full SPLx_TZS_PRINTTBR(1) secure page-table dump (both TTBRs,
 * L1 sections plus their L2 pages) on top of the ordinary boot log. */
static char g_log[131072];
static int  g_log_len;

/*
 * log = off: nothing is written to storage on a clean boot. The log still
 * accumulates in memory, and if any line reports a problem (see
 * note_problem) it is written anyway at the end, so an abort or a bad config
 * line is never silent. Benchmark results always get their own file.
 */
static int g_log_to_disk = 1;
static int g_log_problem;

static int contains_word(const char *hay, const char *needle)
{
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

/* Every failure, abort or warning the plugin logs uses one of these words. */
static void note_problem(const char *s)
{
    static const char *const words[] = {
        "abort", "FAIL", "fail", "NOT ", "refused", "WARNING", "IGNORED",
        "TOO LARGE", "not found", "could not",
    };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (contains_word(s, words[i])) { g_log_problem = 1; return; }
}

/* Pure buffer append, no ksceDebugPrintf call - safe to use from inside
 * kprintf_capture_handler() below, which LOG()/log_line() are not (they'd
 * re-enter that handler through ksceDebugPrintf forever). */
static void append_log(const char *s)
{
    note_problem(s);
    int n = 0;
    while (s[n] && g_log_len + n < (int)sizeof(g_log) - 2) n++;
    for (int i = 0; i < n; i++) g_log[g_log_len++] = s[i];
    if (g_log_len < (int)sizeof(g_log) - 1)
        g_log[g_log_len++] = '\n';
}

static void log_line(const char *s)
{
    append_log(s);
    LOG("%s\n", s);
}

/*
 * Writes the current g_log out to disk right now, rather than only once at
 * the end of module_start(). Called as a checkpoint immediately before and
 * after each EXEC_WITH_SP call below - those are genuinely untested on real
 * hardware (every previous boot failed earlier, during SPLx_CFK_INIT()
 * itself), so if one of them wedges or kills the thread outright, the log
 * up to the last checkpoint survives on disk instead of being lost with
 * whatever never made it to a final write.
 */
static void write_log_file(void)
{
    SceUID fd = ksceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        ksceIoWrite(fd, g_log, (SceSize)g_log_len);
        ksceIoClose(fd);
    }
}

/* Checkpoint write: only when logging is on. With log = off the final
 * write_out decides, from g_log_problem, whether anything is written. */
static void flush_log_to_disk(void)
{
    if (g_log_to_disk) write_log_file();
}

/* The last write of a run: always with log = on, and with log = off only
 * when something went wrong. */
static void final_log_write(void)
{
    if (g_log_to_disk || g_log_problem) write_log_file();
}

static int contains(const char *hay, const char *needle)
{
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (needle[j] == '\0') return 1;
    }
    return 0;
}

/* Registered as the system-wide kprintf handler only for the duration of
 * the SPLx_CFK_INIT() call in module_start(), to capture psp2spl's own
 * SPLi_DEBUG/SPLi_ERROR output - which otherwise only reaches
 * ksceDebugPrintf, with nothing reading it back on a unit with no DECI4p
 * host attached. Must call append_log(), never log_line()/LOG(). */
static int kprintf_capture_handler(int unk, const char *format, va_list args)
{
    (void)unk;
    char buf[200];
    va_list args_copy;
    va_copy(args_copy, args);
    vsnprintf(buf, sizeof(buf), format, args);
    int n = 0;
    while (buf[n]) n++;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    append_log(buf);

    /* SPL_TZSGMSV_EBADMOVP: psp2spl's hardcoded table offset is wrong for
     * this firmware build, but the module's .text base (printed as this
     * same message's 3rd arg) is already correctly resolved. Root state is
     * still valid right here - this handler runs synchronously, nested
     * inside the still-failing SPLx_TZS_GETMODSVA call, well before
     * SPLx_INIT's unwind zeroes *spl_root on return. Pull the exact failing
     * SVA straight out of this message's own args and dump a wide window
     * around it via SVA2NSVA directly - NOT GETMODSVA, which would re-enter
     * the same .data resolution block and recurse into the same failure. */
    if (contains(buf, "!movp")) {
        (void)va_arg(args_copy, const char *); /* dbgid */
        (void)va_arg(args_copy, int);          /* mod */
        unsigned int text_base = va_arg(args_copy, unsigned int);
        unsigned int off       = va_arg(args_copy, unsigned int);
        {
            char line[48];
            snprintf(line, sizeof(line), "movp fail off 0x%X", off);
            append_log(line); /* NOT log_hex_line: that calls ksceDebugPrintf,
                                * which would re-enter this handler. */
        }

        /* A +/-0x80 window, then a +/-0x800 window, then a flat 8KB dump
         * from .text's own base (text+0x0-0x2000) found six corroborating
         * reload sites (all under text+0x1488) that decode to the same
         * candidate, 0x002120E4 (text-relative 0x120E4) - but that's past
         * every window dumped so far, so none of this ever looked at the
         * candidate's own target address directly.
         *
         * Tried aiming this exact window at text+0x11000 (to land near
         * 0x120E4) and it came back BYTE-IDENTICAL to the text+0x0 dump,
         * across two independent reboots, despite the built .skprx
         * (verified byte-identical on-device both times, and confirmed via
         * disassembly to compute win_start = text_base+0x11000 correctly).
         * Conclusion: SPLx_TZS_SVA2NSVA's translation can't be trusted far
         * from the one SVA this path has ever successfully resolved
         * (text_base itself) - it silently aliases back to that same
         * window instead of erroring, rather than genuinely translating an
         * arbitrary offset. Don't retry this by just picking a different
         * offset; it needs its own investigation (or SKGleba's input on
         * SVA2NSVA's real valid range) before it can tell us anything about
         * memory this far from .text's base. Left at the one window this
         * mechanism has actually proven to read correctly. */
        unsigned int win_start = text_base;
        unsigned int win_len   = 0x2000u;

        void *nsva = NULL;
        int r = SPLx_TZS_SVA2NSVA(win_start, &nsva);
        if (r >= 0) {
            volatile unsigned int *p = (volatile unsigned int *)nsva;
            for (unsigned int i = 0; i < win_len; i += 32) {
                if (g_log_len >= (int)sizeof(g_log) - 100) {
                    append_log("dump truncated: g_log full");
                    break;
                }
                char line[96];
                unsigned int base_off = (win_start - text_base) + i;
                int n2 = snprintf(line, sizeof(line), "%X:", base_off);
                for (int w = 0; w < 8; w++) {
                    n2 += snprintf(line + n2, (size_t)(sizeof(line) - n2),
                                   " %08X", p[(i / 4) + w]);
                }
                append_log(line);
            }
        } else {
            char line[48];
            snprintf(line, sizeof(line), "dump sva2nsva failed: %d", r);
            append_log(line);
        }
    }
    va_end(args_copy);
    return 0;
}

static int kprintf_noop_handler(int unk, const char *format, va_list args)
{
    (void)unk; (void)format; (void)args;
    return 0;
}

static void log_hex_line(const char *prefix, unsigned int v)
{
    static const char hexd[] = "0123456789ABCDEF";
    char buf[128];                      /* prefix up to 100 + "0x" + 8 digits */
    int n = 0;
    while (prefix[n] && n < 100) { buf[n] = prefix[n]; n++; }
    buf[n++] = '0'; buf[n++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) buf[n++] = hexd[(v >> shift) & 0xF];
    buf[n] = '\0';
    log_line(buf);
}

/*
 * Reads CONFIG_PATH and decides which sites to enable. Any failure to open
 * the file, or any line that isn't a recognized "key=value", leaves that
 * key's flag at its "off" default - a malformed config can never turn
 * something on. Line-ending and BOM handling live in pfe_parse_config().
 *
 * The buffer is deliberately much larger than any sane config, and the file
 * is stat'd first so that a file which does not fit is reported rather than
 * silently truncated. A truncating read is a nasty failure mode here: the
 * keys usually sit *after* the explanatory comments, so a too-small buffer
 * parses nothing but comments and the plugin cleanly does nothing at all,
 * which reads as "the plugin is broken" rather than "fix your config".
 */
static void read_config(struct pfe_config *cfg)
{
    pfe_parse_config(NULL, 0, cfg);   /* initialise every field to its default */

    static char buf[PFE_CONFIG_MAX_BYTES];

    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (ksceIoGetstat(CONFIG_PATH, &st) >= 0 &&
        st.st_size > (SceOff)sizeof(buf)) {
        log_line("config: FILE TOO LARGE - not parsed, nothing will be enabled");
        log_hex_line("config: size ", (unsigned int)st.st_size);
        log_hex_line("config: buffer ", (unsigned int)sizeof(buf));
        return;
    }

    SceUID fd = ksceIoOpen(CONFIG_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        log_hex_line("config: open failed ", (unsigned int)fd);
        return;
    }
    int n = ksceIoRead(fd, buf, sizeof(buf));
    ksceIoClose(fd);
    if (n <= 0) {
        log_line("config: empty file");
        return;
    }

    pfe_parse_config(buf, n, cfg);

    log_hex_line("config: bytes parsed ", (unsigned int)n);
    static const char *const tri[] = { "keep", "on", "off" };
    char line[96];
    static const char *const run[] = { "off", "on", "once" };
    snprintf(line, sizeof(line), "config: enable=%s probe=%s benchmark=%s log=%s",
             run[cfg->enable % 3], run[cfg->probe % 3],
             cfg->bench ? "once" : "off", cfg->log ? "on" : "off");
    log_line(line);
    for (int i = 0; i < PFE_ACTLR_NFIELDS; i++) {
        snprintf(line, sizeof(line), "config: %s=%s",
                 pfe_actlr_fields[i].key, tri[cfg->actlr[i] % 3]);
        log_line(line);
    }
    for (int i = 0; i < PFE_PL310_NFIELDS; i++) {
        snprintf(line, sizeof(line), "config: %s=%s",
                 pfe_pl310_fields[i].key, tri[cfg->pl310[i] % 3]);
        log_line(line);
    }
    if (cfg->pl310_offset < 0)
        log_line("config: pl310.prefetch_offset=keep");
    else {
        snprintf(line, sizeof(line), "config: pl310.prefetch_offset=%d", cfg->pl310_offset);
        log_line(line);
    }
    log_line(cfg->bench_clocks_max ? "config: benchmark.clocks=max" : "config: benchmark.clocks=keep");
    log_line(cfg->bench_at_boot ? "config: benchmark.timing=boot" : "config: benchmark.timing=home");
    if (cfg->ignored) {
        snprintf(line, sizeof(line),
                 "config: %d line(s) IGNORED - unknown key or bad value", cfg->ignored);
        log_line(line);
    }
}

/*
 * libspl.h hides PRINTTBR behind SPLIB_NODEBUG, which this build may define;
 * the symbol is unconditionally present in the vendored libspl.a (verified
 * with nm), and psp2spl's own `typedef int bool` makes int the right type.
 */
extern int SPLx_TZS_PRINTTBR(int printl2);

/*
 * Read-only reconnaissance of the secure world's address space, to find out
 * where EXEC_WITH_SP's `func` can legally point.
 *
 * Boot 8 proved the SMC itself is reached and that SYSMEM's trampoline does
 * `mov sp, r1` / `blx r2` with no translation of either value (disassembled
 * at SYSMEM .text+0x60), so `func` has to be executable under the *secure*
 * page tables. We had been handing it a non-secure kernel VA from
 * ksceKernelGetMemBlockBase(), which secure world has no reason to have
 * mapped - hence the unrecoverable hang.
 *
 * PRINTTBR walks both TTBRs straight through layout->mbase, the ordinary
 * non-secure mapping of the TZ region, and issues no SMC at all, so this
 * cannot wedge the machine the way an EXEC_WITH_SP guess can. It prints
 * XN/PXN/AP per entry, which is what identifies a usable target. SVA2PA is
 * likewise a pure table walk. Our own code page's PA is logged alongside so
 * the dump can be searched offline for a secure mapping that covers it.
 */
static void probe_secure_layout(void)
{
    log_line("---- probe: secure layout (read-only, no SMC) ----");

    uintptr_t own_pa = 0;
    if (ksceKernelVAtoPA((void *)actlr_set, &own_pa) >= 0)
        log_hex_line("probe own actlr_set pa ", (unsigned int)own_pa);
    else
        log_line("probe: VAtoPA(actlr_set) failed");

    static const struct { int mod; const char *name; } mods[2] = {
        { SPLTZ_MODINFO_SYSMEM,  "probe sysmem  " },
        { SPLTZ_MODINFO_INTRMGR, "probe intrmgr " },
    };
    for (int i = 0; i < 2; i++) {
        unsigned int sva = 0, pa = 0;
        if (SPLx_TZS_GETMODSVA(mods[i].mod, 0, 0, &sva) < 0) continue;
        log_hex_line(mods[i].name, sva);
        if (SPLx_TZS_SVA2PA(sva, &pa) >= 0)
            log_hex_line("  -> pa ", pa);
        else
            log_line("  -> SVA2PA failed");
    }

    /*
     * PL310 identification and the registers whose contents decide which
     * knobs are even available. Pure secure reads, no writes.
     *
     * Cache ID (offset 0x000) carries the part number and, in bits [5:0], the
     * RTL release: that is what says whether double linefill is usable, since
     * errata 752271 makes it corrupt data on r3p0/r3p1 and it is only fixed in
     * r3p2. AUX_CTRL (0x104) aliases PREFETCH_CTRL's two prefetch-enable bits,
     * so reading both cross-checks that the aliasing really holds here rather
     * than only in the TRM.
     */
    log_line("---- probe: PL310 registers ----");
    static const struct { unsigned int sva; const char *name; } pl310[6] = {
        { 0x1A002000u, "probe pl310 cache_id      " },
        { 0x1A002004u, "probe pl310 cache_type    " },
        { 0x1A002104u, "probe pl310 aux_ctrl      " },
        { 0x1A002108u, "probe pl310 tag_ram_lat   " },
        { 0x1A00210Cu, "probe pl310 data_ram_lat  " },
        { 0x1A002F60u, "probe pl310 prefetch_ctrl " },
    };
    for (int i = 0; i < 6; i++) {
        unsigned int v = 0, pa = 0;
        if (SPLx_TZS_SVA2PA(pl310[i].sva, &pa) < 0 || pa != pl310[i].sva) {
            log_hex_line("probe: not identity-mapped, skipping ", pl310[i].sva);
            continue;
        }
        if (SPLx_TZS_READ32(pl310[i].sva, &v) >= 0)
            log_hex_line(pl310[i].name, v);
        else
            log_hex_line("probe: secure read failed at ", pl310[i].sva);
    }

    log_line("---- probe: PRINTTBR(1) ----");
    ksceKernelRegisterKprintfHandler(kprintf_capture_handler, NULL);
    int pr = SPLx_TZS_PRINTTBR(1);
    ksceKernelRegisterKprintfHandler(kprintf_noop_handler, NULL);
    log_hex_line("probe printtbr ret ", (unsigned int)pr);
    log_hex_line("probe log used ", (unsigned int)g_log_len);
    flush_log_to_disk(); /* persist the dump before any later probe can die */
}

/*
 * SYSMEM's secure .text tail: the one place a stub can legally live.
 *
 * Boot 9 settled that secure world has no page that is both writable and
 * executable, and no mapping of our own code's PA - so a stub cannot be
 * placed by allocating memory. What worked on the 3.65 devkit (2026-11-23)
 * instead exploits that AP/XN are per-translation-table, not per-physical-
 * page: SYSMEM's own .text is secure R-X (AP=101, no XN), its last page
 * ends in unreferenced 0xB6 fill, and a *second, non-secure* mapping of that
 * same physical page is plainly writable. Write the stub through the NS
 * alias, clean/invalidate to PoU, and EXEC_WITH_SP the secure VA.
 *
 * Boot 9's PRINTTBR shows that region at the same addresses on retail 3.60
 * as on the devkit: sva 0x00214000 [4K] -> pa 0x40188000, AP=101, no XN,
 * i.e. SYSMEM .text + 0x14000 (the devkit's stub sat at +0x140CC).
 *
 * This dump verifies, read-only, that the tail really is fill on *this*
 * firmware before anything writes to it - a wrong "it's probably padding"
 * would corrupt live secure kernel code. Addresses are derived from
 * GETMODSVA rather than hardcoded, since secure module bases move between
 * boots.
 */
static void probe_sysmem_tail(void)
{
    log_line("---- probe: SYSMEM .text tail (read-only) ----");

    unsigned int text_sva = 0;
    if (SPLx_TZS_GETMODSVA(SPLTZ_MODINFO_SYSMEM, 0, 0, &text_sva) < 0) {
        log_line("probe tail: GETMODSVA(SYSMEM,.text) failed");
        return;
    }
    unsigned int tail_sva = text_sva + 0x14000u;
    log_hex_line("probe tail sva ", tail_sva);

    unsigned int tail_pa = 0;
    if (SPLx_TZS_SVA2PA(tail_sva, &tail_pa) >= 0)
        log_hex_line("probe tail pa  ", tail_pa);
    else
        log_line("probe tail: SVA2PA failed");

    static const char hexd[] = "0123456789ABCDEF";
    for (unsigned int off = 0; off < 0x1000u; off += 32u) {
        char line[128];
        int n = 0;
        for (int s = 12; s >= 0; s -= 4) line[n++] = hexd[(off >> s) & 0xF];
        line[n++] = ':';
        int ok = 1;
        for (unsigned int w = 0; w < 32u; w += 4u) {
            unsigned int v = 0;
            if (SPLx_TZS_READ32(tail_sva + off + w, &v) < 0) { ok = 0; break; }
            line[n++] = ' ';
            for (int s = 28; s >= 0; s -= 4) line[n++] = hexd[(v >> s) & 0xF];
        }
        line[n] = '\0';
        if (!ok) { append_log("  read32 failed, stopping tail dump"); break; }
        append_log(line);
    }
    flush_log_to_disk();
}

/*
 * LPDDR0's access-control register block in the main xbar, identity-mapped
 * secure-RW (boot 12 PRINTTBR: L1_SECTn 0xEC000000 1M, AP=001), so the
 * secure read/write primitives reach it with sva == pa.
 *
 * psp2ref: MAIN_XBAR_OFFSET 0xEC000000, family stride 0x10000, device stride
 * 0x400; ACCESS_CONTROL is family 6 and LPDDR0 is device 1, hence
 * 0xEC000000 + 6*0x10000 + 1*0x400. psp2spl's SPLx_TZS_INIT zeroes the u64
 * at +0xA0 of this block to "drop the ddrs firewall" and then assumes the TZ
 * sections are usable from non-secure code.
 *
 * On retail 3.60 that assumption does not hold: boots 13-15 showed NS reads
 * of TZ memory working while NS writes never become visible to secure world.
 * If the firewall is expressed as several range entries and 3.60's monitor
 * armed one other than the single u64 psp2spl clears, that would produce
 * exactly this. Dump the block to find out which entries are live.
 */
#define XBAR_LPDDR0_ACCESS_CTRL_SVA 0xEC060400u
#define XBAR_DDRFWAE_OFF            0xA0u

static void probe_ddr_firewall(void)
{
    log_line("---- probe: LPDDR0 xbar access control (read-only) ----");
    log_hex_line("probe xbar base sva ", XBAR_LPDDR0_ACCESS_CTRL_SVA);
    log_hex_line("probe psp2spl clears u64 at ",
                 XBAR_LPDDR0_ACCESS_CTRL_SVA + XBAR_DDRFWAE_OFF);

    static const char hexd[] = "0123456789ABCDEF";
    for (unsigned int off = 0; off < 0x400u; off += 32u) {
        char line[128];
        int n = 0;
        for (int s = 12; s >= 0; s -= 4) line[n++] = hexd[(off >> s) & 0xF];
        line[n++] = ':';
        int ok = 1;
        for (unsigned int w = 0; w < 32u; w += 4u) {
            unsigned int v = 0;
            if (SPLx_TZS_READ32(XBAR_LPDDR0_ACCESS_CTRL_SVA + off + w, &v) < 0) { ok = 0; break; }
            line[n++] = ' ';
            for (int s = 28; s >= 0; s -= 4) line[n++] = hexd[(v >> s) & 0xF];
        }
        line[n] = '\0';
        if (!ok) { append_log("  read32 failed, stopping xbar dump"); break; }
        append_log(line);
    }
    flush_log_to_disk();
}

/*
 * Cores whose ACTLR came back with both prefetch bits set. Counted so the
 * per-core loop ends in an explicit result line: on boot 25 the worker
 * threads logged nothing at all and the pass looked successful from the
 * surrounding lines alone, which is exactly the failure mode a summary
 * catches.
 */
/*
 * Read-only per-core CPU identification. Plain non-secure MRCs: MIDR, ACTLR,
 * CLIDR and CTR are all readable at PL1 without entering secure world, so
 * this needs no SMC and runs safely in probe mode.
 *
 * MIDR's revision/variant is what pins down which ACTLR fields exist on this
 * part, and reading ACTLR on every core (rather than assuming core 0 speaks
 * for all of them) is the only way to know the firmware configured them
 * uniformly.
 */
static int probe_cpu_core(SceSize args, void *argp)
{
    (void)args; (void)argp;
    unsigned int mpidr = 0, midr = 0, actlr = 0, clidr = 0, ctr = 0;
    __asm__ __volatile__("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    __asm__ __volatile__("mrc p15, 0, %0, c0, c0, 0" : "=r"(midr));
    __asm__ __volatile__("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr));
    __asm__ __volatile__("mrc p15, 1, %0, c0, c0, 1" : "=r"(clidr));
    __asm__ __volatile__("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));

    log_hex_line("  core  ", mpidr & 0xFu);
    log_hex_line("    midr  ", midr);
    log_hex_line("    actlr ", actlr);
    log_hex_line("    clidr ", clidr);
    log_hex_line("    ctr   ", ctr);
    return 0;
}

static void probe_cpu_regs(void)
{
    log_line("---- probe: per-core CPU regs (read-only, no SMC) ----");
    static const int masks[4] = { 0x10000, 0x20000, 0x40000, 0x80000 };
    for (int i = 0; i < 4; i++) {
        SceUID th = ksceKernelCreateThread("pfe_cpuid", probe_cpu_core,
                                           0x10, 0x2000, 0, masks[i], NULL);
        if (th < 0) { log_hex_line("probe: thread create failed ", (unsigned int)th); continue; }
        int st = ksceKernelStartThread(th, 0, NULL);
        if (st >= 0) ksceKernelWaitThreadEnd(th, NULL, NULL);
        else log_hex_line("probe: thread start failed ", (unsigned int)st);
        ksceKernelDeleteThread(th);
    }
    flush_log_to_disk();
}

static int g_actlr_ok;

static int run_actlr_core(SceSize args, void *argp)
{
    (void)args;
    const unsigned int *a = (const unsigned int *)argp;
    unsigned int stub_sva = a[0], stack_sva = a[1];
    unsigned int set_mask = a[2], clear_mask = a[3];

    log_hex_line("  thread entered, stub ", stub_sva);
    unsigned int before = 0, after = 0, mpidr = 0;
    __asm__ __volatile__("mrc p15, 0, %0, c1, c0, 1" : "=r"(before));
    int r = splCl_smc(0, stack_sva, stub_sva, 0, SPLTZ_SMC_EXEC_WITH_SP);
    __asm__ __volatile__("mrc p15, 0, %0, c1, c0, 1" : "=r"(after));
    __asm__ __volatile__("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));

    log_hex_line("  core      ", mpidr & 0xFu);
    log_hex_line("  actlr before ", before);
    log_hex_line("  actlr after  ", after);
    log_hex_line("  smc ret      ", (unsigned int)r);
    /* The write took iff every requested bit moved the way we asked. */
    if ((after & set_mask) == set_mask && (after & clear_mask) == 0)
        g_actlr_ok++;
    return 0;
}

/*
 * The non-secure view of the TZ region. Boot 17 established that NS *reads*
 * of TZ memory work and only writes are firewalled, so the secure page tables
 * can be walked from non-secure code with no SMC at all.
 *
 * Everything here is read out of psp2spl's own resolved layout rather than
 * hardcoded: the region base/length and the two TTBR offsets are per-firmware
 * constants splib already carries, and mbase (its NS window over the region)
 * is freshly allocated each boot. See src/spl_layout.c.
 */
extern unsigned int pfe_tz_layout(unsigned int *rst, unsigned int *len,
                                  unsigned int *ttbr0_off, unsigned int *ttbr1_off);

static unsigned int g_tz_ns_base;    /* NS VA corresponding to PA g_tz_rst */
static unsigned int g_tz_rst;        /* TZ region physical base */
static unsigned int g_tz_len;        /* TZ region length */
static unsigned int g_ttbr_off[2];   /* secure TTBR0/TTBR1 offsets into it */

static int tz_ns_init(void)
{
    unsigned int mbase = pfe_tz_layout(&g_tz_rst, &g_tz_len,
                                       &g_ttbr_off[0], &g_ttbr_off[1]);
    if (!mbase || !g_tz_len) return -1;
    if (!g_ttbr_off[0] || g_ttbr_off[0] >= g_tz_len) return -2;
    if (!g_ttbr_off[1] || g_ttbr_off[1] >= g_tz_len) return -3;
    g_tz_ns_base = mbase;
    return 0;
}

static unsigned int tz_ns(unsigned int pa)
{
    if (!g_tz_ns_base || pa < g_tz_rst || pa >= g_tz_rst + g_tz_len) return 0;
    return g_tz_ns_base + (pa - g_tz_rst);
}

/*
 * Walks the secure tables for sva, returning where its L2 descriptor lives
 * (as a physical address) and the descriptor's current value. Tries both
 * TTBRs, the way psp2spl's own SVA2PA does.
 */
static int ns_walk(unsigned int sva, unsigned int *out_desc_pa, unsigned int *out_desc)
{
    for (int t = 0; t < 2; t++) {
        unsigned int l1 = tz_ns(g_tz_rst + g_ttbr_off[t]);
        if (!l1) continue;
        unsigned int l1d = ((volatile unsigned int *)l1)[sva >> 20];
        if ((l1d & 3u) != 1u) continue;             /* want a coarse page table */
        unsigned int l2_pa = l1d & 0xFFFFFC00u;
        unsigned int l2 = tz_ns(l2_pa);
        if (!l2) continue;
        unsigned int idx = (sva >> 12) & 0xFFu;
        *out_desc_pa = l2_pa + idx * 4u;
        *out_desc    = ((volatile unsigned int *)l2)[idx];
        return 0;
    }
    return -1;
}

#define DESC_IS_SMALL(d) (((d) & 2u) == 2u)
#define DESC_PL1_RW(d)   ((((d) >> 9) & 1u) == 0u && (((d) >> 4) & 3u) == 1u)

/*
 * Small-page descriptor, Normal Non-Cacheable, PL1 read/write, executable:
 * bit1=1 small page, AP[1:0]=0b01 (bits 5:4), TEX=0b001 (bits 8:6), C=B=0,
 * AP[2]=0 (bit 9), S=0, nG=0, XN=0 (bit 0).
 */
#define DESC_NC_RWX(pa)  (((pa) & 0xFFFFF000u) | 0x52u)

/*
 * Is any secure mapping, in either TTBR, backed by this physical page? Used
 * to confirm the scratch page really is free before we write to it - getting
 * this wrong would clobber live secure memory. Plain reads through the NS
 * window, no SMCs.
 */
static int tz_pa_is_mapped(unsigned int pa)
{
    unsigned int page = pa & 0xFFFFF000u;
    for (int t = 0; t < 2; t++) {
        unsigned int l1 = tz_ns(g_tz_rst + g_ttbr_off[t]);
        if (!l1) continue;
        for (unsigned int i = 0; i < 4096u; i++) {
            unsigned int l1d = ((volatile unsigned int *)l1)[i];
            unsigned int type = l1d & 3u;
            if (type == 2u) {
                if (l1d & (1u << 18)) {                      /* supersection, 16 MiB */
                    if ((l1d & 0xFF000000u) == (page & 0xFF000000u)) return 1;
                } else if ((l1d & 0xFFF00000u) == (page & 0xFFF00000u)) {
                    return 1;                                 /* section, 1 MiB */
                }
            } else if (type == 1u) {
                unsigned int l2 = tz_ns(l1d & 0xFFFFFC00u);
                if (!l2) continue;
                for (unsigned int j = 0; j < 256u; j++) {
                    unsigned int d = ((volatile unsigned int *)l2)[j];
                    unsigned int dt = d & 3u;
                    if (dt == 0u) continue;
                    if (dt == 1u) {                           /* large page, 64 KiB */
                        if ((d & 0xFFFF0000u) == (page & 0xFFFF0000u)) return 1;
                    } else if ((d & 0xFFFFF000u) == page) {   /* small page */
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

/*
 * Picking the scratch page at runtime.
 *
 * The stub's bytes have to sit at a physical address inside the TZ region
 * (boot 20: the DDR firewall confines secure DRAM access to it in both
 * directions), at a page nothing secure maps. Boot 21 used a hand-picked
 * 0x401E3000, found by eye in a PRINTTBR dump; this derives the equivalent
 * from the live tables instead, so the choice follows whatever layout the
 * running firmware and model actually have.
 *
 * One pass over both TTBRs builds a bitmap of every TZ page that is in use -
 * mapped by a descriptor, or holding one of the page tables themselves - and
 * the page is then chosen from the holes in it. Requiring both neighbours to
 * be in use is what makes a hole a gap inside live secure DRAM rather than
 * address space that was simply never populated; the search runs downwards
 * because the layouts seen so far keep their code low and their slack high.
 */
#define TZ_MAX_PAGES 2048u                      /* a TZ region of up to 8 MiB */
static unsigned char g_tz_inuse[TZ_MAX_PAGES / 8];

static void tz_mark(unsigned int pa, unsigned int span)
{
    if (pa + span <= g_tz_rst || pa >= g_tz_rst + g_tz_len) return;  /* outside */
    unsigned int lo = (pa > g_tz_rst) ? pa : g_tz_rst;
    unsigned int hi = pa + span;
    if (hi > g_tz_rst + g_tz_len) hi = g_tz_rst + g_tz_len;
    for (unsigned int a = lo & 0xFFFFF000u; a < hi; a += 0x1000u) {
        unsigned int p = (a - g_tz_rst) >> 12;
        if (p < TZ_MAX_PAGES) g_tz_inuse[p >> 3] |= (unsigned char)(1u << (p & 7u));
    }
}

static int tz_inuse(unsigned int p)
{
    return (g_tz_inuse[p >> 3] >> (p & 7u)) & 1u;
}

static unsigned int tz_find_scratch_pa(void)
{
    unsigned int npages = g_tz_len >> 12;
    if (npages < 3u || npages > TZ_MAX_PAGES) return 0;
    memset(g_tz_inuse, 0, sizeof(g_tz_inuse));

    for (int t = 0; t < 2; t++) {
        unsigned int l1 = tz_ns(g_tz_rst + g_ttbr_off[t]);
        if (!l1) continue;
        tz_mark(g_tz_rst + g_ttbr_off[t], 0x4000u);   /* the L1 table itself */
        for (unsigned int i = 0; i < 4096u; i++) {
            unsigned int l1d = ((volatile unsigned int *)l1)[i];
            unsigned int type = l1d & 3u;
            if (type == 2u) {
                if (l1d & (1u << 18))
                    tz_mark(l1d & 0xFF000000u, 0x1000000u);   /* supersection */
                else
                    tz_mark(l1d & 0xFFF00000u, 0x100000u);    /* section */
            } else if (type == 1u) {
                unsigned int l2_pa = l1d & 0xFFFFFC00u;
                tz_mark(l2_pa, 0x400u);                       /* the L2 table itself */
                unsigned int l2 = tz_ns(l2_pa);
                if (!l2) continue;
                for (unsigned int j = 0; j < 256u; j++) {
                    unsigned int d = ((volatile unsigned int *)l2)[j];
                    unsigned int dt = d & 3u;
                    if (dt == 1u)
                        tz_mark(d & 0xFFFF0000u, 0x10000u);   /* large page */
                    else if (dt >= 2u)
                        tz_mark(d & 0xFFFFF000u, 0x1000u);    /* small page: bit1
                                                                 set, bit0 is XN,
                                                                 so 2 *and* 3 */
                }
            }
        }
    }

    for (unsigned int p = npages - 2u; p >= 1u; p--) {
        if (tz_inuse(p)) continue;
        if (!tz_inuse(p - 1u) || !tz_inuse(p + 1u)) continue;
        return g_tz_rst + (p << 12);
    }
    return 0;
}

/*
 * Sets ACTLR's prefetch bits on every core.
 *
 * Staging the stub inside SYSMEM's secure .text is a dead end on retail 3.60:
 * the only way to write that page from non-secure code is an aliased mapping,
 * and the DDR firewall drops those writes before they reach DRAM (boot 17,
 * confirmed via lv0). Secure writes are not firewalled, though - psp2spl uses
 * them for its own SMC table - so instead of writing into secure memory we
 * give secure world a mapping of *our own* page, which lives outside the TZ
 * region and so is trivially writable from here.
 *
 * One secure write adds an L2 descriptor for a previously unmapped secure VA,
 * copying the attributes of SYSMEM .text's own R-X descriptor and swapping in
 * our page's PA. A previously-invalid VA also means no stale TLB entry to
 * worry about. The descriptor is removed again afterwards.
 *
 * Gates, so a wrong guess aborts instead of hanging the way boot 8 did:
 *   - the R-X template descriptor must agree with SVA2PA;
 *   - the target slot must currently be unmapped;
 *   - the secure VA chosen for the descriptor must map the right page and be
 *     PL1-RW, cross-checked by a secure read before any write;
 *   - after the write, secure world must read our instructions back through
 *     the new mapping, or we never execute it.
 */
/*
 * Everything the apply step needs, resolved once. The discovery walks are not
 * cheap - the writable-secure-VA sweep alone probes 4096 addresses through
 * both TTBRs - and the benchmark suite calls the apply step once per
 * configuration, so repeating the search every time would add seconds to
 * boot. The descriptor itself is still injected and removed around each
 * individual apply, so nothing is left mapped between calls.
 */
static struct {
    int resolved;
    unsigned int scratch_pa;
    unsigned int target_sva;
    unsigned int tgt_desc_pa;
    unsigned int tgt_desc;
    unsigned int desc_sva;
    unsigned int stack_sva;
} g_l1;

static int l1_resolve(void)
{
    if (g_l1.resolved) return 0;

    unsigned int text_sva = 0;
    if (SPLx_TZS_GETMODSVA(SPLTZ_MODINFO_SYSMEM, 0, 0, &text_sva) < 0) {
        log_line("l1: GETMODSVA(SYSMEM,.text) failed");
        return -1;
    }
    if (tz_ns_init() < 0) {
        log_line("l1: psp2spl has no resolved TZ layout - aborting");
        return -1;
    }
    log_hex_line("l1 tz rst  ", g_tz_rst);
    log_hex_line("l1 tz len  ", g_tz_len);
    log_hex_line("l1 ttbr0 off ", g_ttbr_off[0]);
    log_hex_line("l1 ttbr1 off ", g_ttbr_off[1]);
    log_hex_line("l1 tz ns base ", g_tz_ns_base);

    /*
     * The stub's bytes have to live at a TZ physical address. Boot 20 settled
     * that: a secure read through an injected mapping of our own out-of-TZ
     * page returns a constant sentinel (0x1F85C021) no matter which page we
     * point it at, while the same mapping pointed at an in-TZ PA reads back
     * that page's real contents. So the firewall confines secure DRAM access
     * to the TZ region in both directions, and staging the stub in our own
     * memory cannot work however the caches are maintained.
     */
    unsigned int scratch_pa = tz_find_scratch_pa();
    if (!scratch_pa) {
        log_line("l1: found no unmapped TZ page between mapped ones - aborting");
        return -1;
    }
    log_hex_line("l1 scratch pa ", scratch_pa);
    /* Independent cross-check of the bitmap scan, by the descriptor walk. */
    if (tz_pa_is_mapped(scratch_pa)) {
        log_line("l1: scratch PA is mapped after all - aborting");
        return -1;
    }
    log_line("l1: scratch page verified unmapped");

    /* Attribute template: SYSMEM .text's last page is secure R-X. Read only to
     * cross-check that our table walk agrees with psp2spl's own SVA2PA. */
    unsigned int rx_desc_pa = 0, rx_desc = 0;
    if (ns_walk(text_sva + 0x14000u, &rx_desc_pa, &rx_desc) < 0 || !DESC_IS_SMALL(rx_desc)) {
        log_line("l1: could not read the R-X template descriptor");
        return -1;
    }
    unsigned int tpa = 0;
    if (SPLx_TZS_SVA2PA(text_sva + 0x14000u, &tpa) < 0 ||
        (rx_desc & 0xFFFFF000u) != (tpa & 0xFFFFF000u)) {
        log_line("l1: template descriptor disagrees with SVA2PA - aborting");
        return -1;
    }
    log_hex_line("l1 rx template desc ", rx_desc);

    /*
     * An unmapped slot to hang the new descriptor off. It only has to be a
     * secure VA whose L2 entry is currently zero; sweep the whole 1 MiB that
     * holds SYSMEM .text, since how much of it the module occupies varies
     * between boots (boot 24 had every slot in the 0x15000-0x1F000 window
     * that worked on boot 21 taken), then any other coarse table in the low
     * 16 MiB if that one is full.
     */
    unsigned int target_sva = 0, tgt_desc_pa = 0, tgt_desc = 0;
    for (unsigned int off = 0; off < 0x100000u; off += 0x1000u) {
        unsigned int cand = (text_sva & 0xFFF00000u) + off;
        unsigned int dpa = 0, d = 0;
        if (!cand) continue;
        if (ns_walk(cand, &dpa, &d) < 0) continue;
        if (d != 0) continue;
        target_sva = cand; tgt_desc_pa = dpa; tgt_desc = d;
        break;
    }
    for (unsigned int cand = 0x1000u; !target_sva && cand < 0x01000000u; cand += 0x1000u) {
        unsigned int dpa = 0, d = 0;
        if (ns_walk(cand, &dpa, &d) < 0) continue;
        if (d != 0) continue;
        target_sva = cand; tgt_desc_pa = dpa; tgt_desc = d;
        break;
    }
    if (!target_sva) {
        log_line("l1: no free slot in any secure L2 table - aborting");
        return -1;
    }
    log_hex_line("l1 target sva ", target_sva);
    log_hex_line("l1 target desc pa ", tgt_desc_pa);

    /* The descriptor must be written with a *secure* write, so find a secure
     * VA that maps the L2 table's page and is PL1-RW. The secure modules sit
     * low in the address space, but not always within the first 2 MiB -
     * SYSMEM .text has been seen at 0x546000 - so sweep 16 MiB. Each probe is
     * two L1 reads through the NS window, so the wider sweep costs little. */
    unsigned int desc_sva = 0;
    for (unsigned int cand = 0x1000u; cand < 0x01000000u; cand += 0x1000u) {
        if (cand >= text_sva && cand < text_sva + 0x20000u) continue;
        unsigned int dpa = 0, d = 0;
        if (ns_walk(cand, &dpa, &d) < 0) continue;
        if (!DESC_IS_SMALL(d) || !DESC_PL1_RW(d)) continue;
        if ((d & 0xFFFFF000u) != (tgt_desc_pa & ~0xFFFu)) continue;
        desc_sva = cand + (tgt_desc_pa & 0xFFFu);
        break;
    }
    if (!desc_sva) {
        log_line("l1: no writable secure VA maps the L2 table - aborting");
        return -1;
    }
    log_hex_line("l1 desc sva ", desc_sva);

    unsigned int sr = 0;
    if (SPLx_TZS_READ32(desc_sva, &sr) < 0 || sr != tgt_desc) {
        log_hex_line("l1: desc_sva cross-check failed, secure read ", sr);
        return -1;
    }

    /* The secure-world scratch stack for the EXEC_WITH_SP trampoline. */
    unsigned int stack_sva = 0;
    if (SPLx_TZS_GETMODSVA(SPLTZ_MODINFO_INTRMGR, 1, 0, &stack_sva) < 0 || !stack_sva) {
        log_line("l1: no INTRMGR .data for scratch sp - aborting");
        return -1;
    }
    stack_sva += 0x200u;
    log_hex_line("l1 scratch sp ", stack_sva);

    g_l1.scratch_pa  = scratch_pa;
    g_l1.target_sva  = target_sva;
    g_l1.tgt_desc_pa = tgt_desc_pa;
    g_l1.tgt_desc    = tgt_desc;
    g_l1.desc_sva    = desc_sva;
    g_l1.stack_sva   = stack_sva;
    g_l1.resolved    = 1;
    return 0;
}

/*
 * Applies one ACTLR set/clear mask pair to all four cores, injecting the
 * secure mapping, writing and verifying the stub, executing it, and then
 * clearing the scratch page and restoring the descriptor. Safe to call
 * repeatedly: nothing stays mapped across calls.
 */
static int apply_l1_actlr(unsigned int set_mask, unsigned int clear_mask)
{
    log_hex_line("actlr set mask   ", set_mask);
    log_hex_line("actlr clear mask ", clear_mask);

    if (l1_resolve() < 0) return -1;

    unsigned int target_sva = g_l1.target_sva;
    unsigned int desc_sva   = g_l1.desc_sva;
    unsigned int tgt_desc   = g_l1.tgt_desc;
    unsigned int stack_sva  = g_l1.stack_sva;

    /*
     * Map the scratch page Normal Non-Cacheable, PL1-RW and executable.
     * Non-cacheable is the point: the secure writes below then go straight to
     * DRAM and the instruction fetch reads DRAM, so the A9/PL310
     * security-tagged-line problem that defeated boots 13-19 cannot arise, and
     * no cp15 maintenance is needed at all. Nothing else maps this page, so no
     * alias can be holding a stale line for it either.
     */
    unsigned int new_desc = DESC_NC_RWX(g_l1.scratch_pa);
    log_hex_line("l1 new desc ", new_desc);
    int wr = SPLx_TZS_WRITE32(desc_sva, new_desc);
    log_hex_line("l1 desc write ret ", (unsigned int)wr);
    unsigned int vr = 0;
    SPLx_TZS_READ32(desc_sva, &vr);
    log_hex_line("l1 desc readback ", vr);

    int visible = (vr == new_desc);
    if (!visible)
        log_line("l1: descriptor did not take - not writing the stub");

    /* Write the stub itself with secure writes, which the firewall permits. */
    unsigned int stub[ACTLR_STUB_WORDS];
    build_actlr_stub(stub, set_mask, clear_mask);

    if (visible) {
        for (unsigned int i = 0; i < ACTLR_STUB_WORDS; i++) {
            if (SPLx_TZS_WRITE32(target_sva + i * 4u, stub[i]) < 0) {
                log_hex_line("l1: secure stub write failed at word ", i);
                visible = 0;
                break;
            }
        }
    }

    if (visible) {
        for (unsigned int i = 0; i < ACTLR_STUB_WORDS; i++) {
            unsigned int v = 0;
            if (SPLx_TZS_READ32(target_sva + i * 4u, &v) < 0 || v != stub[i]) {
                log_hex_line("l1: stub not visible through new mapping, word ", i);
                log_hex_line("     got ", v);
                visible = 0;
                break;
            }
        }
    }

    if (visible) {
        log_line("l1: stub visible through the new secure mapping, executing per core");
        flush_log_to_disk();
        log_hex_line("l1 scratch sp ", stack_sva);
        static const int masks[4] = { 0x10000, 0x20000, 0x40000, 0x80000 };
        unsigned int argp[4] = { target_sva, stack_sva, set_mask, clear_mask };
        g_actlr_ok = 0;
        for (int i = 0; i < 4; i++) {
            SceUID th = ksceKernelCreateThread("pfe_actlr", run_actlr_core,
                                               0x10, 0x2000, 0, masks[i], NULL);
            if (th < 0) { log_hex_line("l1: thread create failed ", (unsigned int)th); continue; }
            int st = ksceKernelStartThread(th, sizeof(argp), argp);
            if (st >= 0)
                ksceKernelWaitThreadEnd(th, NULL, NULL);
            else
                log_hex_line("l1: thread start failed ", (unsigned int)st);
            ksceKernelDeleteThread(th);
            flush_log_to_disk();
        }
        log_hex_line("l1 cores with the requested ACTLR bits, of 4: ",
                     (unsigned int)g_actlr_ok);
        if (g_actlr_ok == 4)
            log_line("actlr: requested bits APPLIED on all cores");
        else
            log_line("actlr: NOT fully applied - see the per-core lines");
    } else {
        log_line("l1: stub NOT visible from secure world - NOT executing (clean abort)");
    }

    /* Zero the stub out of the scratch page, then drop the mapping again. */
    for (unsigned int i = 0; i < ACTLR_STUB_WORDS; i++)
        SPLx_TZS_WRITE32(target_sva + i * 4u, 0);
    SPLx_TZS_WRITE32(desc_sva, tgt_desc);
    log_line("l1: scratch cleared, secure mapping removed");
    return (visible && g_actlr_ok == 4) ? 0 : -1;
}


/*
 * Secure read-modify-write of PL310 PREFETCH_CTRL: clears `clear`, then sets
 * `set`. PL310 is MMIO, not a cp15 register, so no code execution is needed.
 *
 * PL310_PREFETCH_CTRL_SVA is only valid because the 0x1A000000 MMIO section is
 * identity-mapped in the secure tables. That has held on every layout seen so
 * far, but it is an assumption about the firmware's own page tables, so it is
 * checked: if the VA resolves to a different PA, or to nothing, a blind secure
 * write would land somewhere unknown. Returns 0 on success, negative (and
 * writes nothing) otherwise.
 */
static int pl310_prefetch_rmw(unsigned int clear, unsigned int set,
                              unsigned int *out_before, unsigned int *out_after)
{
    static int identity_ok;
    if (!identity_ok) {
        unsigned int pa = 0;
        int pr = SPLx_TZS_SVA2PA(PL310_PREFETCH_CTRL_SVA, &pa);
        log_hex_line("l2 prefetch_ctrl sva->pa ", pa);
        if (pr < 0 || pa != PL310_PREFETCH_CTRL_SVA) {
            log_line("l2: PREFETCH_CTRL is not identity-mapped - not touching it");
            return -1;
        }
        identity_ok = 1;
    }

    unsigned int before = 0;
    if (SPLx_TZS_READ32(PL310_PREFETCH_CTRL_SVA, &before) < 0) {
        log_line("l2: secure read of PREFETCH_CTRL failed - not writing");
        return -2;
    }
    unsigned int want = (before & ~clear) | set;
    int rw = SPLx_TZS_WRITE32(PL310_PREFETCH_CTRL_SVA, want);
    unsigned int after = before;
    SPLx_TZS_READ32(PL310_PREFETCH_CTRL_SVA, &after);
    log_hex_line("l2 prefetch_ctrl before ", before);
    log_hex_line("l2 prefetch_ctrl after  ", after);
    if (out_before) *out_before = before;
    if (out_after)  *out_after  = after;
    if (rw < 0 || after != want) {
        log_hex_line("l2: write did not take, wanted ", want);
        return -3;
    }
    return 0;
}

/*
 * The benchmark suite. Every leg fully specifies ACTLR bits 1, 2 and 8 and
 * PL310 data prefetch plus the prefetch offset, so no leg depends on the one
 * before it, and the first leg is repeated last to expose drift (a clock
 * change, thermal throttling) between the two ends of the run.
 *
 * PL310 prefetch offset 23 is deliberately absent: ARM errata 765569 - the
 * prefetcher can cross a 4 KB boundary with that value - and the Vita's
 * r3p1-50rel0 part predates the fix. Double linefill is likewise never set
 * here (errata 752271, data corruption).
 */
struct bench_leg {
    const char  *name;
    unsigned int actlr_set, actlr_clear;   /* within B_ACTLR_OWNED */
    unsigned int pctl;                      /* value of B_PCTL_OWNED */
};

#define B_AOW   PFE_ACTLR_ALLOC_ONE_WAY
#define B_DSIDE PFE_ACTLR_L1_DSIDE_PREFETCH
#define B_HINT  PFE_ACTLR_L2_PREFETCH_HINT
#define B_PF    (B_DSIDE | B_HINT)
#define B_DPF   PFE_PL310_DATA_PREFETCH
#define B_DROP  PFE_PL310_PREFETCH_DROP
#define B_ACTLR_OWNED (B_AOW | B_PF)
#define B_PCTL_OWNED  (B_DPF | B_DROP | PFE_PL310_OFFSET_MASK)

/*
 * ACTLR bit 8 (alloc one way) stays at the firmware's 1 throughout: boots
 * 33-35 measured it neutral. The dside/hint split and prefetch drop are what
 * the per-field config keys newly expose, so they get their own legs.
 */
static const struct bench_leg g_legs[] = {
    { "base",            B_AOW,           B_PF,   B_DROP },
    { "dside",           B_AOW | B_DSIDE, B_HINT, B_DROP },
    { "hint",            B_AOW | B_HINT,  B_DSIDE, B_DROP },
    { "l1pf",            B_AOW | B_PF,    0,      B_DROP },
    { "l2_o15",          B_AOW,           B_PF,   B_DROP | B_DPF | 15 },
    { "dside_l2_o15",    B_AOW | B_DSIDE, B_HINT, B_DROP | B_DPF | 15 },
    { "l1l2_o0",         B_AOW | B_PF,    0,      B_DROP | B_DPF | 0 },
    { "l1l2_o7",         B_AOW | B_PF,    0,      B_DROP | B_DPF | 7 },
    { "l1l2_o15",        B_AOW | B_PF,    0,      B_DROP | B_DPF | 15 },
    { "l1l2_o31",        B_AOW | B_PF,    0,      B_DROP | B_DPF | 31 },
    { "l1l2_o15_nodrop", B_AOW | B_PF,    0,      B_DPF | 15 },
    { "base_again",      B_AOW,           B_PF,   B_DROP },
};

/* Every leg twice, interleaved, so slow drift and one-off noise show up as
 * disagreement between rounds instead of as a difference between legs. */
#define BENCH_ROUNDS 3

static void log_bench_res(const char *kind, unsigned int param,
                          const struct pfe_bench_res *r)
{
    char line[96];
    snprintf(line, sizeof(line), "bench %s %X cyc %X us %X n %X",
             kind, param, r->cycles, r->us, r->accesses);
    log_line(line);
}

/*
 * Re-applied before every individual test when benchmark.clocks=max. A one-off
 * set does not hold: boot 39 set 500 MHz and the per-leg clock read back
 * ~406 for the first legs and ~330 for the rest - something (PSVshell or
 * ScePower reapplying the shell's request) puts it back within seconds.
 */
static int g_bench_hold_clocks;
static void clocks_max(void);

/* Runs on one pinned core, so the PMU cycle counter is coherent. */
static int bench_leg_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    pfe_bench_pmu_enable();
    struct pfe_bench_res r;
    for (int i = 0; i < PFE_BENCH_WS_COUNT; i++) {
        if (g_bench_hold_clocks) clocks_max();
        pfe_bench_chase(pfe_bench_ws_bytes[i], &r);
        log_bench_res("chase", pfe_bench_ws_bytes[i], &r);
    }
    for (int i = 0; i < PFE_BENCH_STRIDE_COUNT; i++) {
        if (g_bench_hold_clocks) clocks_max();
        pfe_bench_stride(pfe_bench_strides[i], &r);
        log_bench_res("stride", pfe_bench_strides[i], &r);
    }
    pfe_bench_pmu_disable();
    return 0;
}

/*
 * Clocks for benchmark.clocks=max: what games run at. 500 MHz needs a clock
 * plugin (PSVshell turns a 500 request into 444 x 16/15); without one the
 * setter refuses it and 444 is used instead. Each leg logs the clock it
 * actually ran at (cycles / microseconds), so this is checked, not assumed.
 *
 * GPU: on 3.60 kernel code only has the older "ES4" pair, which take two
 * values (one per GPU core) - the getter through pointers. vitasdk names them
 * ...Internal. kscePowerSetGpuClockFrequency (NID 0x717DB06C) is not exported
 * to kernel code on this firmware, and calling the getter with no arguments
 * made it write through a garbage pointer: a kernel data abort at 0xDE
 * (boot 38, captures/boot38_kpanic). PSVshell uses this same pair.
 */
extern int kscePowerGetGpuClockFrequencyInternal(int *core_a, int *core_b);
extern int kscePowerSetGpuClockFrequencyInternal(int core_a, int core_b);

struct clocks { int arm, bus, gpu_a, gpu_b, xbar; };

static void clocks_read(struct clocks *c)
{
    c->gpu_a = c->gpu_b = -1;
    c->arm  = kscePowerGetArmClockFrequency();
    c->bus  = kscePowerGetBusClockFrequency();
    kscePowerGetGpuClockFrequencyInternal(&c->gpu_a, &c->gpu_b);
    c->xbar = kscePowerGetGpuXbarClockFrequency();
}

static void log_clocks(const char *what)
{
    struct clocks c;
    clocks_read(&c);
    char line[96];
    snprintf(line, sizeof(line), "bench clocks %s: arm %d bus %d gpu %d/%d xbar %d",
             what, c.arm, c.bus, c.gpu_a, c.gpu_b, c.xbar);
    log_line(line);
}

static int gpu_clock_valid(int f)
{
    return f == 41 || f == 55 || f == 83 || f == 111 || f == 166 || f == 222;
}

static void clocks_max(void)
{
    static int refused;
    int r = refused ? -1 : kscePowerSetArmClockFrequency(500);
    if (r < 0) {
        if (!refused)
            log_hex_line("bench: arm 500 refused, using 444: ", (unsigned int)r);
        refused = 1;
        kscePowerSetArmClockFrequency(444);
    }
    kscePowerSetBusClockFrequency(222);
    kscePowerSetGpuClockFrequencyInternal(222, 222);
    kscePowerSetGpuXbarClockFrequency(166);
}

static void clocks_restore(const struct clocks *c)
{
    if (c->arm  > 0) kscePowerSetArmClockFrequency(c->arm);
    if (c->bus  > 0) kscePowerSetBusClockFrequency(c->bus);
    if (c->xbar > 0) kscePowerSetGpuXbarClockFrequency(c->xbar);
    if (gpu_clock_valid(c->gpu_a) && gpu_clock_valid(c->gpu_b))
        kscePowerSetGpuClockFrequencyInternal(c->gpu_a, c->gpu_b);
}

static void run_bench_suite(int clocks_to_max)
{
    unsigned int orig_actlr = 0, orig_pctl = 0;
    __asm__ __volatile__("mrc p15, 0, %0, c1, c0, 1" : "=r"(orig_actlr));
    log_hex_line("bench: original actlr ", orig_actlr);
    log_hex_line("bench: uptime ms at start ",
                 (unsigned int)(ksceKernelGetSystemTimeWide() / 1000u));

    int br = pfe_bench_init();
    if (br < 0) {
        log_hex_line("bench: buffer allocation failed ", (unsigned int)br);
        return;
    }
    if (pl310_prefetch_rmw(0, 0, &orig_pctl, NULL) < 0) {
        log_line("bench: cannot read PL310 PREFETCH_CTRL - not running");
        pfe_bench_fini();
        return;
    }

    struct clocks saved;
    clocks_read(&saved);
    log_clocks("before");
    if (clocks_to_max) {
        clocks_max();
        log_clocks("max");
        g_bench_hold_clocks = 1;
    }
    flush_log_to_disk();

    int stop = 0;
    for (int round = 0; round < BENCH_ROUNDS && !stop; round++) {
        log_hex_line("bench round ", (unsigned int)round);
        for (unsigned int i = 0; i < sizeof(g_legs) / sizeof(g_legs[0]); i++) {
            const struct bench_leg *L = &g_legs[i];
            char line[64];
            snprintf(line, sizeof(line), "bench leg %s", L->name);
            log_line(line);
            if (apply_l1_actlr(L->actlr_set, L->actlr_clear) < 0) {
                log_line("bench: ACTLR leg setup failed - stopping the suite");
                stop = 1;
                break;
            }
            if (pl310_prefetch_rmw(B_PCTL_OWNED, L->pctl, NULL, NULL) < 0) {
                log_line("bench: PL310 leg setup failed - stopping the suite");
                stop = 1;
                break;
            }
            flush_log_to_disk();

            SceUID th = ksceKernelCreateThread("pfe_bench", bench_leg_thread,
                                               0x10, 0x4000, 0, 0x10000, NULL);
            if (th < 0) {
                log_hex_line("bench: thread create failed ", (unsigned int)th);
                stop = 1;
                break;
            }
            if (ksceKernelStartThread(th, 0, NULL) >= 0)
                ksceKernelWaitThreadEnd(th, NULL, NULL);
            ksceKernelDeleteThread(th);
            log_clocks("after leg");
            log_line("bench leg end");
            flush_log_to_disk();
        }
    }

    /* Put back exactly what the firmware had. */
    g_bench_hold_clocks = 0;
    log_line("bench: restoring firmware ACTLR, PL310 and clocks");
    apply_l1_actlr(orig_actlr & B_ACTLR_OWNED, ~orig_actlr & B_ACTLR_OWNED);
    pl310_prefetch_rmw(B_PCTL_OWNED, orig_pctl & B_PCTL_OWNED, NULL, NULL);
    if (clocks_to_max) {
        clocks_restore(&saved);
        log_clocks("restored");
    }
    pfe_bench_fini();
    log_hex_line("bench: uptime ms at end ",
                 (unsigned int)(ksceKernelGetSystemTimeWide() / 1000u));
    log_line("bench: done");
}

/*
 * The suite takes tens of seconds. Run inside module_start() it blocks boot
 * for that long, and that is enough to make SceShell's MTP server fail to
 * start (sceMtpStart 0x80540008, interface 0x80543601) - whose error path then
 * calls a null function pointer and takes the shell down. Boots 33 and 34
 * crashed identically that way, after the suite had restored every register.
 * So it runs here instead, once the shell is up, and boot is not held at all.
 */
#define BENCH_START_DELAY_US (30u * 1000u * 1000u)

static int g_bench_clocks_max;

static void save_bench_log(void)
{
    SceUID fd = ksceIoOpen(BENCH_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        ksceIoWrite(fd, g_log, (SceSize)g_log_len);
        ksceIoClose(fd);
    }
}

/*
 * The dead-man's switch: rewrites every enable/probe/benchmark = once line
 * to "off" in the plugin's own config, then reads the file back and checks
 * nothing says "once" any more. Done before any secure-world work, so if
 * this boot hangs, a plain power-cycle comes back with those switched off.
 * Gating: returns 0 only when that is confirmed from disk; otherwise the
 * caller runs nothing this boot.
 */
static int consume_once_in_config(void)
{
    static char buf[PFE_CONFIG_MAX_BYTES];
    SceUID fd = ksceIoOpen(CONFIG_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = ksceIoRead(fd, buf, sizeof(buf));
    ksceIoClose(fd);
    if (n <= 0 || n >= (int)sizeof(buf)) return -2;

    int changed = pfe_config_consume_once(buf, &n);
    log_hex_line("once: lines switched to off ", (unsigned int)changed);
    if (changed > 0) {
        fd = ksceIoOpen(CONFIG_PATH, SCE_O_WRONLY | SCE_O_TRUNC, 0);
        if (fd < 0) return -3;
        int w = ksceIoWrite(fd, buf, (SceSize)n);
        ksceIoClose(fd);
        if (w != n) return -4;
    }

    /* Verify from disk, not from our own buffer. */
    fd = ksceIoOpen(CONFIG_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return -5;
    n = ksceIoRead(fd, buf, sizeof(buf));
    ksceIoClose(fd);
    struct pfe_config check;
    pfe_parse_config(buf, n > 0 ? n : 0, &check);
    return pfe_config_has_once(&check) ? -6 : 0;
}

/*
 * Ends a bench run with a cold reboot, so the console comes back up normally
 * instead of continuing with whatever the run disturbed (a boot-time run
 * otherwise crashes the shell, boots 33-34). Resolved at runtime through
 * taiHEN rather than imported: vita-headers' 3.60 db also lists
 * kscePowerSetGpuClockFrequency, which turned out not to be exported (boot
 * 38), so an export is only called once the kernel has confirmed it exists.
 */
extern int module_get_export_func(SceUID pid, const char *modname,
                                  unsigned int libnid, unsigned int funcnid,
                                  uintptr_t *func);
#define SCE_POWER_FOR_DRIVER_NID       0x1590166Fu
#define KSCE_POWER_REQUEST_COLD_RESET  0x0442D852u

static void bench_reboot(void)
{
    uintptr_t fn = 0;
    int r = module_get_export_func(0x10005 /* KERNEL_PID */, "ScePower",
                                   SCE_POWER_FOR_DRIVER_NID,
                                   KSCE_POWER_REQUEST_COLD_RESET, &fn);
    log_hex_line("bench: cold reset lookup ", (unsigned int)r);
    if (r < 0 || !fn) {
        log_line("bench: kscePowerRequestColdReset not found - not rebooting");
        save_bench_log();
        flush_log_to_disk();
        return;
    }
    log_line("bench: rebooting");
    save_bench_log();
    flush_log_to_disk();
    int rr = ((int (*)(void))fn)();
    /* Only reached if the request is refused or handled asynchronously. */
    log_hex_line("bench: cold reset returned ", (unsigned int)rr);
    save_bench_log();
    flush_log_to_disk();
}

static int bench_main_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    ksceKernelDelayThread(BENCH_START_DELAY_US);
    run_bench_suite(g_bench_clocks_max);
    probe_cpu_regs();
    log_line("==== vita_cpu_tweaks bench done ====");
    save_bench_log();
    final_log_write();
    bench_reboot();
    return 0;
}

/*
 * ---- Surviving sleep ----
 *
 * A sleep powers the CPU cores down, and on wake the secure world resets
 * them and the PL310 - psp2spl's own module notes that "at sleep-resume cmep
 * & tzsmods are reset" and reinstalls itself. So the plugin follows the same
 * protocol as psp2spl's module (module/main.c, spl_sysevent_handler):
 *
 *   - before sleep (event 0x20F, resume = 0): SPLx_CFK_DEINIT, then
 *     SPLx_CFK_USSMALLOC to pre-allocate what the re-init will need;
 *   - on wake (event 0x100000, resume = 1): re-init with SPL_IFL(_RESET).
 *
 * Unlike psp2spl's module, the re-init and the re-apply happen in a worker
 * thread rather than in the handler itself, so they run in the same kind of
 * context as at boot (normal thread, other cores up for the per-core ACTLR
 * stub). Everything the ACTLR path caches is resolved again: secure modules
 * are reloaded on wake and need not land where they were.
 */
#define SYSEVENT_SLEEP_PHASE2   0x20F
#define SYSEVENT_RESUME_PHASE1  0x100000

static struct pfe_masks g_resume_masks;
static SceUID g_resume_evf = -1;
static int g_resume_count;
static int g_spl_down;

static int pfe_sysevent_handler(int resume, int eventid, void *args, void *opt)
{
    (void)args; (void)opt;
    if (eventid == SYSEVENT_SLEEP_PHASE2 && !resume && !g_spl_down) {
        SPLx_CFK_DEINIT(SPL_IFL(_LV0) | SPL_IFL(_TZS));
        SPLx_CFK_USSMALLOC();
        g_spl_down = 1;
    } else if (eventid == SYSEVENT_RESUME_PHASE1 && resume && g_resume_evf >= 0) {
        ksceKernelSetEventFlag(g_resume_evf, 1);
    }
    return 0;
}

static void reapply_after_resume(void)
{
    g_resume_count++;
    /* Keep the boot log, but don't let a long run of sleeps fill the buffer. */
    if (g_log_len > (int)sizeof(g_log) / 2) {
        g_log_len = 0;
        log_line("(log restarted: buffer half full)");
    }
    log_hex_line("==== resume ", (unsigned int)g_resume_count);
    log_line("resume: ACTLR as found after wake:");
    probe_cpu_regs();

    ksceKernelRegisterKprintfHandler(kprintf_capture_handler, NULL);
    int r = SPLx_CFK_INIT(SPL_IFL(_LV0) | SPL_IFL(_TZS) | SPL_IFL(_RESET), 0, 1, 0,
                          SPLV0_COMMBACKUP_CRITICAL);
    ksceKernelRegisterKprintfHandler(kprintf_noop_handler, NULL);
    log_hex_line("resume: cfk re-init ret ", (unsigned int)r);
    if (r < 0) {
        log_line("resume: psp2spl re-init FAILED - settings NOT re-applied after wake");
        final_log_write();
        return;
    }
    g_spl_down = 0;
    flush_log_to_disk();

    g_l1.resolved = 0;          /* secure layout may have moved: find it again */
    if (g_resume_masks.actlr_set | g_resume_masks.actlr_clear)
        apply_l1_actlr(g_resume_masks.actlr_set, g_resume_masks.actlr_clear);
    if (g_resume_masks.pl310_set | g_resume_masks.pl310_clear) {
        if (pl310_prefetch_rmw(g_resume_masks.pl310_clear, g_resume_masks.pl310_set,
                               NULL, NULL) == 0)
            log_line("pl310: requested fields APPLIED");
        else
            log_line("pl310: NOT applied - see the lines above");
    }
    probe_cpu_regs();
    log_line("==== resume done ====");
    final_log_write();
}

static int resume_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        unsigned int bits = 0;
        if (ksceKernelWaitEventFlag(g_resume_evf, 1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR,
                                    &bits, NULL) < 0)
            return 0;
        ksceKernelDelayThread(500 * 1000);      /* let the rest of resume finish */
        reapply_after_resume();
    }
}

static void enable_resume_support(const struct pfe_masks *mk)
{
    g_resume_masks = *mk;
    g_resume_evf = ksceKernelCreateEventFlag("cput_resume", 0, 0, NULL);
    if (g_resume_evf < 0) {
        log_hex_line("resume: event flag FAILED, settings will not survive sleep ",
                     (unsigned int)g_resume_evf);
        return;
    }
    SceUID th = ksceKernelCreateThread("cput_resume", resume_thread, 0x40, 0x4000, 0, 0, NULL);
    if (th < 0 || ksceKernelStartThread(th, 0, NULL) < 0) {
        log_hex_line("resume: thread FAILED, settings will not survive sleep ", (unsigned int)th);
        return;
    }
    int r = ksceKernelRegisterSysEventHandler("cput_sysevent", pfe_sysevent_handler, NULL);
    if (r < 0) {
        log_hex_line("resume: sysevent handler FAILED, settings will not survive sleep ",
                     (unsigned int)r);
        return;
    }
    log_line("resume: settings will be re-applied after every wake from sleep");
}

int module_start(SceSize argc, const void *args)
{
    (void)argc; (void)args;

    g_log_len = 0;
    log_line("==== vita_cpu_tweaks boot start ====");

    struct pfe_config cfg;
    read_config(&cfg);
    g_log_to_disk = cfg.log;

    /*
     * Anything set to "once" is switched off in the config before any
     * secure-world work, so the next boot - including one after a hang -
     * comes up without it. Gating: if that cannot be confirmed, nothing runs.
     */
    if (pfe_config_has_once(&cfg)) {
        int r = consume_once_in_config();
        if (r < 0) {
            log_hex_line("once: switch FAILED, running NOTHING ",
                         (unsigned int)r);
            goto write_out;
        }
        log_line("once: config now says off for the next boot");
        flush_log_to_disk();
    }

    struct pfe_masks mk;
    pfe_config_masks(&cfg, &mk);
    int want_actlr = cfg.enable != PFE_RUN_OFF && (mk.actlr_set | mk.actlr_clear) != 0;
    int want_pl310 = cfg.enable != PFE_RUN_OFF && (mk.pl310_set | mk.pl310_clear) != 0;

    if (!cfg.bench && cfg.probe == PFE_RUN_OFF && !want_actlr && !want_pl310) {
        log_line("nothing to do, exiting cleanly");
        goto write_out;
    }

    {
        SPLx_CFK_PREP(NULL, 0, "[CPUT]");
        ksceKernelRegisterKprintfHandler(kprintf_capture_handler, NULL);
        int ret = SPLx_CFK_INIT(SPL_IFL(_LV0) | SPL_IFL(_TZS), 0, 1, 1,
                                 SPLV0_COMMBACKUP_CRITICAL);
        ksceKernelRegisterKprintfHandler(kprintf_noop_handler, NULL);
        log_hex_line("cfk_init_ret ", (unsigned int)ret);
        if (ret < 0) { log_line("CFK init failed, aborting"); goto write_out; }
        flush_log_to_disk(); /* checkpoint: past the point every prior boot failed at */
    }

    if (cfg.probe != PFE_RUN_OFF) {
        probe_cpu_regs();
        probe_secure_layout();
        probe_sysmem_tail();
        probe_ddr_firewall();
        log_line("probe: done (read-only, no EXEC_WITH_SP calls)");
        flush_log_to_disk();
    }

    if (cfg.bench && cfg.bench_at_boot) {
        /*
         * benchmark.timing=boot: run right here, holding boot, on a near-idle
         * system. Holding boot this long makes SceShell's MTP service fail
         * and the shell crash afterwards (boots 33-34), so the run ends in a
         * reboot instead; the config already says benchmark = off.
         */
        log_line("bench: timing=boot, running now, holding boot");
        run_bench_suite(cfg.bench_clocks_max);
        probe_cpu_regs();
        log_line("==== vita_cpu_tweaks bench done ====");
        save_bench_log();
        flush_log_to_disk();
        bench_reboot();
        goto write_out;
    }

    if (cfg.bench) {
        /* Nothing is applied on a bench boot: the suite sets every leg
         * itself from the firmware's values, restores them, and reboots
         * into a normal boot that applies the config. */
        log_line("bench: timing=home, suite deferred to a thread, starting in 30 s");
        g_bench_clocks_max = cfg.bench_clocks_max;
        SceUID th = ksceKernelCreateThread("pfe_bench_main", bench_main_thread,
                                           0x60, 0x4000, 0, 0, NULL);
        if (th < 0 || ksceKernelStartThread(th, 0, NULL) < 0)
            log_hex_line("bench: could not start the deferred thread ", (unsigned int)th);
        goto write_out;
    }

    /* enable = on | once */
    if (want_actlr) {
        apply_l1_actlr(mk.actlr_set, mk.actlr_clear);
        flush_log_to_disk();
    }

    if (want_pl310) {
        if (mk.pl310_set & PFE_PL310_DOUBLE_LINEFILL)
            log_line("pl310: WARNING double_linefill=on - ARM errata 752271 can corrupt data on this PL310 (r3p1-50rel0)");
        if ((mk.pl310_clear & PFE_PL310_OFFSET_MASK) &&
            (mk.pl310_set & PFE_PL310_OFFSET_MASK) == 23)
            log_line("pl310: WARNING prefetch_offset=23 - ARM errata 765569, prefetch can cross a 4 KB page");
        unsigned int before = 0, after = 0;
        if (pl310_prefetch_rmw(mk.pl310_clear, mk.pl310_set, &before, &after) == 0)
            log_line("pl310: requested fields APPLIED");
        else
            log_line("pl310: NOT applied - see the lines above");
        flush_log_to_disk();
    }

    if (want_actlr || want_pl310)
        enable_resume_support(&mk);

    /*
     * Final per-core read-back, after all the work is done. The per-core
     * apply loop already reads ACTLR immediately after its own write; this
     * re-reads it a few milliseconds later, from a different thread, so that
     * a value which does not stick - something else in the boot re-asserting
     * it - shows up rather than being taken on trust from the write path.
     */
    probe_cpu_regs();

    log_line("==== vita_cpu_tweaks boot done ====");

write_out:
    final_log_write();
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) { (void)argc; (void)args; return SCE_KERNEL_STOP_SUCCESS; }
