/*
 * vita_prefetch_enabler - enable the Cortex-A9 / PL310 hardware prefetchers
 * on a PS Vita, from a config.txt-loaded taiHEN kernel plugin.
 *
 * The Vita's kernel_boot_loader explicitly disables both hardware
 * prefetchers at boot. This plugin re-enables either or both, every cold
 * boot, controlled by a plain text config file:
 *
 *     ur0:data/prefetch_enabler.txt
 *
 *     # l1_prefetch: Cortex-A9 ACTLR L1 D-side prefetch + L2 prefetch hint (bits 2:1)
 *     # l2_prefetch: ARM PL310 L2C-310 data prefetch (PREFETCH_CTRL bit 28) -- on/off
 *     l1_prefetch=off
 *     l2_prefetch=off
 *
 * Each key independently takes "on" or "off" (case-insensitive, whitespace
 * around '=' ignored). Anything else - a missing key, a bad value, a
 * missing or unreadable file - is treated as "off": a boot-time module that
 * can't read its own config should do nothing, not guess. Lines starting
 * with '#' (after trimming leading whitespace) are comments and ignored.
 *
 * Dead-man's switch: every boot, before touching config or secure-world
 * state, the plugin comments out its OWN line in ur0:tai/config.txt. If
 * this boot hangs or crashes, a plain power cycle recovers normally - the
 * next boot's taiHEN never loads it. To run it again, manually uncomment
 * that line again before rebooting.
 *

 * "off" touches NO secure-world state at all - the hardware is already
 * disabled at this point in boot, so there is nothing to clear. Only
 * "on" for either key runs the secure-world stub below.
 *
 * Requires SKGleba's psp2spl (https://github.com/SKGleba/psp2spl) for
 * secure-world (TrustZone) code execution - see the top-level README for
 * the full writeup, the hardware this was verified on, and its limits.
 * PLEASE READ THE "Compatibility" SECTION THERE BEFORE FLASHING THIS ONTO
 * A UNIT WHOSE FIRMWARE WASN'T ALREADY CONFIRMED.
 *
 * Writes a plain-text log to ur0:data/prefetch_enabler.log every boot, plus
 * a best-effort copy to host0:prefetch_enabler/boot.log when a DECI4p host
 * happens to be attached (debugging convenience only).
 */
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
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

#define LOG(...) ksceDebugPrintf("[PFE] " __VA_ARGS__)

void *memset(void *dst, int c, unsigned int n)
{
    volatile unsigned char *p = (volatile unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

#define CONFIG_PATH "ur0:data/prefetch_enabler.txt"
#define LOG_PATH    "ur0:data/prefetch_enabler.log"
#define HOST_LOG    "host0:prefetch_enabler/boot.log"

#define TAI_CONFIG_PATH   "ur0:tai/config.txt"
#define SELF_SKPRX_NEEDLE "prefetch_enabler.skprx"

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
 * its PA. Bit 28 = data prefetch enable - same bit the old pl310_set stub set,
 * but reached by a secure write instead of secure code execution. */
#define PL310_PREFETCH_CTRL_SVA 0x1A002F60u
#define PL310_DATA_PREFETCH_EN  0x10000000u

/* Sized for a full SPLx_TZS_PRINTTBR(1) secure page-table dump (both TTBRs,
 * L1 sections plus their L2 pages) on top of the ordinary boot log. */
static char g_log[131072];
static int  g_log_len;

/* Pure buffer append, no ksceDebugPrintf call - safe to use from inside
 * kprintf_capture_handler() below, which LOG()/log_line() are not (they'd
 * re-enter that handler through ksceDebugPrintf forever). */
static void append_log(const char *s)
{
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
static void flush_log_to_disk(void)
{
    SceUID fd = ksceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        ksceIoWrite(fd, g_log, (SceSize)g_log_len);
        ksceIoClose(fd);
    }
    ksceIoMkdir("host0:prefetch_enabler", 0777);
    SceUID hfd = ksceIoOpen(HOST_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (hfd >= 0) {
        ksceIoWrite(hfd, g_log, (SceSize)g_log_len);
        ksceIoClose(hfd);
    }
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
    char buf[64];
    int n = 0;
    while (prefix[n] && n < 40) { buf[n] = prefix[n]; n++; }
    buf[n++] = '0'; buf[n++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) buf[n++] = hexd[(v >> shift) & 0xF];
    buf[n] = '\0';
    log_line(buf);
}

/*
 * Dead-man's switch: comments out this plugin's own line(s) in
 * ur0:tai/config.txt before module_start() does anything else. taiHEN only
 * reads config.txt once, at boot, before loading us - editing it now can't
 * affect the boot in progress, only the next one. So if this boot's later
 * secure-world work hangs or crashes, a plain power cycle then skips
 * loading us entirely: no SD2VITA / manual file edit needed to recover.
 * Re-enabling after a clean boot means manually uncommenting the line.
 *
 * Gating, not best-effort: if we can't *confirm* the switch is set, the
 * caller must skip the risky work for this boot too, rather than silently
 * running it with no net. stat-then-read (instead of just capping the
 * read) is what lets us tell "file fits" apart from "file truncated by our
 * own buffer" - writing back a truncated read would destroy the rest of
 * the user's real config.txt.
 */
static int self_disable_in_config(const char **reason)
{
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (ksceIoGetstat(TAI_CONFIG_PATH, &st) < 0) {
        *reason = "stat failed";
        return 0;
    }

    static char cfg[16384];
    if (st.st_size < 0 || st.st_size > (SceOff)sizeof(cfg) - 1) {
        *reason = "config.txt too large for buffer";
        return 0;
    }

    SceUID fd = ksceIoOpen(TAI_CONFIG_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        *reason = "open(r) failed";
        return 0;
    }
    int n = ksceIoRead(fd, cfg, sizeof(cfg) - 1);
    ksceIoClose(fd);
    if (n <= 0) {
        *reason = "read failed/empty";
        return 0;
    }

    int hits = pfe_comment_out_lines(cfg, &n, (int)sizeof(cfg), SELF_SKPRX_NEEDLE);
    if (hits < 0) {
        *reason = "no room to insert '#'";
        return 0;
    }
    if (hits == 0) {
        *reason = "own line not found";
        return 0;
    }

    fd = ksceIoOpen(TAI_CONFIG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        *reason = "open(w) failed";
        return 0;
    }
    int w = ksceIoWrite(fd, cfg, (SceSize)n);
    ksceIoClose(fd);
    if (w != n) {
        *reason = "short write";
        return 0;
    }

    *reason = "ok";
    return 1;
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
static void read_config(int *want_l1, int *want_l2, int *probe_only)
{
    *want_l1 = 0;
    *want_l2 = 0;
    *probe_only = 0;

    static char buf[4096];

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

    struct pfe_config c;
    pfe_parse_config(buf, n, &c);
    *want_l1    = c.want_l1;
    *want_l2    = c.want_l2;
    *probe_only = c.probe_only;

    log_hex_line("config: bytes parsed ", (unsigned int)n);
    log_line(c.probe_only ? "config: probe_only=on"  : "config: probe_only=off");
    log_line(c.want_l1    ? "config: l1_prefetch=on" : "config: l1_prefetch=off");
    log_line(c.want_l2    ? "config: l2_prefetch=on" : "config: l2_prefetch=off");
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
static int g_actlr_ok;

#define ACTLR_PREFETCH_BITS 0x6u   /* [1] = L1 prefetch, [2] = L2 (stride) */

static int run_actlr_core(SceSize args, void *argp)
{
    (void)args;
    unsigned int stub_sva  = *(unsigned int *)argp;
    unsigned int stack_sva = *((unsigned int *)argp + 1);

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
    if ((after & ACTLR_PREFETCH_BITS) == ACTLR_PREFETCH_BITS) g_actlr_ok++;
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
static void apply_l1_actlr(void)
{
    unsigned int text_sva = 0;
    if (SPLx_TZS_GETMODSVA(SPLTZ_MODINFO_SYSMEM, 0, 0, &text_sva) < 0) {
        log_line("l1: GETMODSVA(SYSMEM,.text) failed");
        return;
    }
    if (tz_ns_init() < 0) {
        log_line("l1: psp2spl has no resolved TZ layout - aborting");
        return;
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
        return;
    }
    log_hex_line("l1 scratch pa ", scratch_pa);
    /* Independent cross-check of the bitmap scan, by the descriptor walk. */
    if (tz_pa_is_mapped(scratch_pa)) {
        log_line("l1: scratch PA is mapped after all - aborting");
        return;
    }
    log_line("l1: scratch page verified unmapped");

    /* Attribute template: SYSMEM .text's last page is secure R-X. Read only to
     * cross-check that our table walk agrees with psp2spl's own SVA2PA. */
    unsigned int rx_desc_pa = 0, rx_desc = 0;
    if (ns_walk(text_sva + 0x14000u, &rx_desc_pa, &rx_desc) < 0 || !DESC_IS_SMALL(rx_desc)) {
        log_line("l1: could not read the R-X template descriptor");
        return;
    }
    unsigned int tpa = 0;
    if (SPLx_TZS_SVA2PA(text_sva + 0x14000u, &tpa) < 0 ||
        (rx_desc & 0xFFFFF000u) != (tpa & 0xFFFFF000u)) {
        log_line("l1: template descriptor disagrees with SVA2PA - aborting");
        return;
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
        return;
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
        return;
    }
    log_hex_line("l1 desc sva ", desc_sva);

    unsigned int sr = 0;
    if (SPLx_TZS_READ32(desc_sva, &sr) < 0 || sr != tgt_desc) {
        log_hex_line("l1: desc_sva cross-check failed, secure read ", sr);
        return;
    }

    /*
     * Map the scratch page Normal Non-Cacheable, PL1-RW and executable.
     * Non-cacheable is the point: the secure writes below then go straight to
     * DRAM and the instruction fetch reads DRAM, so the A9/PL310
     * security-tagged-line problem that defeated boots 13-19 cannot arise, and
     * no cp15 maintenance is needed at all. Nothing else maps this page, so no
     * alias can be holding a stale line for it either.
     */
    unsigned int new_desc = DESC_NC_RWX(scratch_pa);
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
    if (visible) {
        for (unsigned int i = 0; i < sizeof(actlr_set) / 4; i++) {
            if (SPLx_TZS_WRITE32(target_sva + i * 4u, actlr_set[i]) < 0) {
                log_hex_line("l1: secure stub write failed at word ", i);
                visible = 0;
                break;
            }
        }
    }

    if (visible) {
        for (unsigned int i = 0; i < sizeof(actlr_set) / 4; i++) {
            unsigned int v = 0;
            if (SPLx_TZS_READ32(target_sva + i * 4u, &v) < 0 || v != actlr_set[i]) {
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
        unsigned int stack_sva = 0;
        if (SPLx_TZS_GETMODSVA(SPLTZ_MODINFO_INTRMGR, 1, 0, &stack_sva) >= 0 && stack_sva) {
            stack_sva += 0x200u;
            log_hex_line("l1 scratch sp ", stack_sva);
            static const int masks[4] = { 0x10000, 0x20000, 0x40000, 0x80000 };
            unsigned int argp[2] = { target_sva, stack_sva };
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
            log_hex_line("l1 cores with prefetch bits set, of 4: ",
                         (unsigned int)g_actlr_ok);
            if (g_actlr_ok == 4)
                log_line("l1_prefetch: ACTLR stride prefetch ENABLED on all cores");
            else
                log_line("l1_prefetch: NOT fully enabled - see the per-core lines");
        } else {
            log_line("l1: no INTRMGR .data for scratch sp - not executing");
        }
    } else {
        log_line("l1: stub NOT visible from secure world - NOT executing (clean abort)");
    }

    /* Zero the stub out of the scratch page, then drop the mapping again. */
    for (unsigned int i = 0; i < sizeof(actlr_set) / 4; i++)
        SPLx_TZS_WRITE32(target_sva + i * 4u, 0);
    SPLx_TZS_WRITE32(desc_sva, tgt_desc);
    log_line("l1: scratch cleared, secure mapping removed");
}

int module_start(SceSize argc, const void *args)
{
    (void)argc; (void)args;

    const char *disable_reason = "";
    int disabled = self_disable_in_config(&disable_reason);

    g_log_len = 0;
    log_line("==== vita_prefetch_enabler boot start ====");
    if (disabled) {
        log_line("selfdisable: own config.txt line commented out for next boot");
    } else {
        log_line("selfdisable: FAILED - aborting before any secure-world work");
        log_line(disable_reason);
        goto write_out;
    }

    int want_l1 = 0, want_l2 = 0, probe_only = 0;
    read_config(&want_l1, &want_l2, &probe_only);

    if (!want_l1 && !want_l2 && !probe_only) {
        log_line("nothing to enable, exiting cleanly");
        goto write_out;
    }

    {
        SPLx_CFK_PREP(NULL, 0, "[PFE]");
        ksceKernelRegisterKprintfHandler(kprintf_capture_handler, NULL);
        int ret = SPLx_CFK_INIT(SPL_IFL(_LV0) | SPL_IFL(_TZS), 0, 1, 1,
                                 SPLV0_COMMBACKUP_CRITICAL);
        ksceKernelRegisterKprintfHandler(kprintf_noop_handler, NULL);
        log_hex_line("cfk_init_ret ", (unsigned int)ret);
        if (ret < 0) { log_line("CFK init failed, aborting"); goto write_out; }
        flush_log_to_disk(); /* checkpoint: past the point every prior boot failed at */
    }

    if (probe_only) {
        probe_secure_layout();
        probe_sysmem_tail();
        probe_ddr_firewall();
        log_line("probe_only: skipping all EXEC_WITH_SP calls");
        goto write_out;
    }

    if (want_l1) {
        apply_l1_actlr();
        flush_log_to_disk();
    }

    if (want_l2) {
        /*
         * PL310 is MMIO, not a cp15 register, so no code execution is needed:
         * flip PREFETCH_CTRL bit 28 with a secure read-modify-write. tzs_*32
         * are already exercised successfully during SPLx_TZS_INIT above.
         */
        /*
         * PL310_PREFETCH_CTRL_SVA is only valid because the 0x1A000000 MMIO
         * section is identity-mapped in the secure tables. That has held on
         * every layout seen so far, but it is an assumption about the
         * firmware's own page tables, so check it: if the VA resolves to a
         * different PA, or to nothing, a blind secure write would land
         * somewhere unknown. Abort instead.
         */
        unsigned int pl310_pa = 0;
        int pr = SPLx_TZS_SVA2PA(PL310_PREFETCH_CTRL_SVA, &pl310_pa);
        log_hex_line("l2 prefetch_ctrl sva->pa ", pl310_pa);
        if (pr < 0 || pl310_pa != PL310_PREFETCH_CTRL_SVA) {
            log_line("l2_prefetch: PREFETCH_CTRL is not identity-mapped - aborting L2");
            goto l2_done;
        }

        unsigned int before = 0;
        int rr = SPLx_TZS_READ32(PL310_PREFETCH_CTRL_SVA, &before);
        log_hex_line("l2 read32 ret ", (unsigned int)rr);
        if (rr < 0) {
            log_line("l2_prefetch: secure read of PREFETCH_CTRL failed - aborting L2");
        } else {
            log_hex_line("l2 prefetch_ctrl before ", before);
            int rw = SPLx_TZS_WRITE32(PL310_PREFETCH_CTRL_SVA,
                                      before | PL310_DATA_PREFETCH_EN);
            log_hex_line("l2 write32 ret ", (unsigned int)rw);
            unsigned int after = before;
            SPLx_TZS_READ32(PL310_PREFETCH_CTRL_SVA, &after);
            log_hex_line("l2 prefetch_ctrl after ", after);
            if (after & PL310_DATA_PREFETCH_EN)
                log_line("l2_prefetch: PL310 data prefetch ENABLED");
            else
                log_line("l2_prefetch: bit did not stick (write ignored?)");
        }
    l2_done:
        flush_log_to_disk();
    }

    log_line("==== vita_prefetch_enabler boot done ====");

write_out:
    flush_log_to_disk();
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) { (void)argc; (void)args; return SCE_KERNEL_STOP_SUCCESS; }
