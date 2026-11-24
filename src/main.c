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
/* PL310 maintenance-by-PA. Needed because an NS-side clean to PoC does not
 * evict a *secure*-tagged L2 line for the same address, and the secure read
 * path can keep hitting that stale line. Op completes when bit 0 clears. */
#define PL310_CLEAN_INV_PA_SVA  0x1A0027F0u

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

/* Trims leading/trailing spaces, tabs, and a trailing '\r' from [*s,*len). */
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

    int found = 0;
    int pos = 0;
    while (pos < n) {
        int start = pos;
        while (pos < n && cfg[pos] != '\n') pos++;
        int line_end = pos;

        const char *tl = cfg + start;
        int tlen = line_end - start;
        trim(&tl, &tlen);

        if (tlen > 0 && tl[0] != '#' && contains_n(tl, tlen, SELF_SKPRX_NEEDLE)) {
            if (n >= (int)sizeof(cfg) - 1) {
                *reason = "no room to insert '#'";
                return 0;
            }
            for (int i = n; i > start; i--) cfg[i] = cfg[i - 1];
            cfg[start] = '#';
            n++;
            pos++; /* re-sync: this line's '\n'/EOF just shifted right by 1 */
            found = 1;
        }
        pos++; /* skip the '\n' itself (or step past EOF on the last line) */
    }

    if (!found) {
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

/* Reads CONFIG_PATH and decides which sites to enable. Any failure to open
 * the file, or any line that isn't a recognized "key=value", leaves that
 * key's flag at its "off" default. */
static void read_config(int *want_l1, int *want_l2, int *probe_only)
{
    *want_l1 = 0;
    *want_l2 = 0;
    *probe_only = 0;

    SceUID fd = ksceIoOpen(CONFIG_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        log_hex_line("config: open failed ", (unsigned int)fd);
        return;
    }
    char buf[256];
    memset(buf, 0, sizeof(buf));
    int n = ksceIoRead(fd, buf, sizeof(buf) - 1);
    ksceIoClose(fd);
    if (n <= 0) {
        log_line("config: empty file");
        return;
    }

    int pos = 0;
    while (pos < n) {
        int start = pos;
        while (pos < n && buf[pos] != '\n') pos++;
        const char *line = buf + start;
        int len = pos - start;
        pos++; /* skip the '\n' (or step past EOF on the last line) */

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
        if (streq_ci(key, keylen, "l1_prefetch")) {
            *want_l1 = on;
            log_line(on ? "config: l1_prefetch=on" : "config: l1_prefetch=off");
        } else if (streq_ci(key, keylen, "l2_prefetch")) {
            *want_l2 = on;
            log_line(on ? "config: l2_prefetch=on" : "config: l2_prefetch=off");
        } else if (streq_ci(key, keylen, "probe_only")) {
            *probe_only = on;
            log_line(on ? "config: probe_only=on" : "config: probe_only=off");
        }
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
 * Offset into SYSMEM's .text of the stub slot, inside the unreferenced 0xB6
 * fill at the tail of its last page. Boot 12 measured the live/fill boundary
 * on retail 3.60 at .text+0x140FC (3844 bytes of fill to the page end), so
 * +0x14200 sits 0x104 bytes inside the fill, 32-byte aligned (one cache
 * line for a 16-byte stub), with ~3.5 KiB of slack after it. The 3.65 devkit's
 * boundary was +0x140CC, which is why this is checked at runtime rather than
 * trusted: the fill contents are re-verified immediately before writing.
 */
#define SYSMEM_STUB_OFF 0x14200u

static int run_actlr_core(SceSize args, void *argp)
{
    (void)args;
    unsigned int stub_sva  = *(unsigned int *)argp;
    unsigned int stack_sva = *((unsigned int *)argp + 1);

    unsigned int before = 0, after = 0, mpidr = 0;
    __asm__ __volatile__("mrc p15, 0, %0, c1, c0, 1" : "=r"(before));
    int r = splCl_smc(0, stack_sva, stub_sva, 0, SPLTZ_SMC_EXEC_WITH_SP);
    __asm__ __volatile__("mrc p15, 0, %0, c1, c0, 1" : "=r"(after));
    __asm__ __volatile__("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));

    log_hex_line("  core      ", mpidr & 0xFu);
    log_hex_line("  actlr before ", before);
    log_hex_line("  actlr after  ", after);
    log_hex_line("  smc ret      ", (unsigned int)r);
    return 0;
}

/*
 * Sets ACTLR's prefetch bits on every core, by staging our stub in SYSMEM's
 * secure .text fill and running it through EXEC_WITH_SP.
 *
 * Why this shape (see analysis/ebadmovp-investigation/README.md): secure world
 * has no page that is both writable and executable, and no mapping of our own
 * code's PA, so the stub cannot simply be allocated and pointed at. But AP/XN
 * are per-translation-table, not per-physical-page: SYSMEM's .text is secure
 * R-X, and a second *non-secure* mapping of the same physical page is writable.
 * Write through the NS alias, clean to PoU, execute via the secure VA.
 *
 * Two gates keep a mistake from hanging the machine the way boot 8 did:
 *   - the target must still be all-0xB6 fill, read back through secure world,
 *     or we never write (protects live secure code if the layout moved);
 *   - after writing, secure world must actually read back our instructions,
 *     or we never execute (protects against a cache-coherency failure, which
 *     would otherwise mean secure world executing stale 0xB6 as code).
 * Either gate failing is a clean logged abort.
 */
static void apply_l1_actlr(void)
{
    unsigned int text_sva = 0;
    if (SPLx_TZS_GETMODSVA(SPLTZ_MODINFO_SYSMEM, 0, 0, &text_sva) < 0) {
        log_line("l1: GETMODSVA(SYSMEM,.text) failed");
        return;
    }
    unsigned int stub_sva = text_sva + SYSMEM_STUB_OFF;
    unsigned int stub_pa  = 0;
    if (SPLx_TZS_SVA2PA(stub_sva, &stub_pa) < 0) {
        log_line("l1: SVA2PA(stub) failed");
        return;
    }
    log_hex_line("l1 stub sva ", stub_sva);
    log_hex_line("l1 stub pa  ", stub_pa);

    /* Writable non-secure alias of the same physical page. */
    unsigned int page_pa  = stub_pa & ~0xFFFu;
    unsigned int page_off = stub_pa &  0xFFFu;
    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size  = (unsigned int)sizeof(opt);
    opt.attr  = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_PADDR;
    opt.paddr = page_pa;
    SceUID blk = ksceKernelAllocMemBlock("pfe_stub", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
                                         0x1000, &opt);
    if (blk < 0) {
        log_hex_line("l1: NS alias map failed ", (unsigned int)blk);
        return;
    }
    void *ns_base = NULL;
    ksceKernelGetMemBlockBase(blk, &ns_base);
    unsigned int ns_stub = (unsigned int)ns_base + page_off;
    log_hex_line("l1 ns alias ", ns_stub);

    /*
     * Gate 1, through the NS alias rather than a secure read. Boot 14 showed
     * the alias is a faithful view of the same physical page (it read the same
     * 0xB6 fill), and reading it here avoids pulling the *old* bytes into the
     * secure side's own cache moments before we overwrite them - which is what
     * made boot 13/14's secure read-back keep returning stale fill.
     */
    for (unsigned int i = 0; i < sizeof(actlr_set) / 4; i++) {
        unsigned int v = ((volatile unsigned int *)ns_stub)[i];
        if (v != 0xB6B6B6B6u) {
            log_hex_line("l1: stub slot is NOT fill, aborting; found ", v);
            ksceKernelFreeMemBlock(blk);
            return;
        }
    }
    log_line("l1: stub slot verified as 0xB6 fill (via NS alias)");

    for (unsigned int i = 0; i < sizeof(actlr_set) / 4; i++)
        ((volatile unsigned int *)ns_stub)[i] = actlr_set[i];

    /* Clean the D-cache for the line, then invalidate I-cache and branch
     * predictor, so a secure-side I-fetch of the same physical line sees the
     * new bytes. Clean to PoC as well as PoU: PL310 sits between the two, and
     * the secure read gadget need not share our inner-cache view. 16 bytes at
     * a 32-byte aligned slot is a single line, so one pass each. */
    __asm__ __volatile__("mcr p15, 0, %0, c7, c11, 1" :: "r"(ns_stub)); /* DCCMVAU  -> PoU */
    __asm__ __volatile__("dsb");
    __asm__ __volatile__("mcr p15, 0, %0, c7, c10, 1" :: "r"(ns_stub)); /* DCCMVAC  -> PoC */
    __asm__ __volatile__("dsb");
    __asm__ __volatile__("mcr p15, 0, %0, c7, c14, 1" :: "r"(ns_stub)); /* DCCIMVAC -> PoC */
    __asm__ __volatile__("dsb");
    __asm__ __volatile__("mcr p15, 0, %0, c7, c5, 1"  :: "r"(ns_stub)); /* ICIMVAU */
    __asm__ __volatile__("mcr p15, 0, %0, c7, c5, 6"  :: "r"(ns_stub)); /* BPIMVA  */
    __asm__ __volatile__("dsb");
    __asm__ __volatile__("isb");

    log_hex_line("l1 ns reads after write  ", ((volatile unsigned int *)ns_stub)[0]);

    /* Drop any secure-tagged PL310 line for this address, which our NS-side
     * maintenance cannot reach. Issued through the secure write primitive
     * since PL310 is mapped secure-RW. */
    int pm = SPLx_TZS_WRITE32(PL310_CLEAN_INV_PA_SVA, stub_pa);
    log_hex_line("l1 pl310 clean_inv_pa ret ", (unsigned int)pm);
    for (int spin = 0; spin < 1000; spin++) {
        unsigned int st = 0;
        if (SPLx_TZS_READ32(PL310_CLEAN_INV_PA_SVA, &st) < 0) break;
        if (!(st & 1u)) break;
    }

    /* Gate 2: secure world must see the stub, or we do not execute it. */
    int visible = 1;
    for (unsigned int i = 0; i < sizeof(actlr_set) / 4; i++) {
        unsigned int v = 0;
        if (SPLx_TZS_READ32(stub_sva + i * 4, &v) < 0 || v != actlr_set[i]) {
            log_hex_line("l1: secure readback mismatch at word ", i);
            log_hex_line("     got ", v);
            visible = 0;
            break;
        }
    }

    if (visible) {
        log_line("l1: stub visible from secure world, executing per core");
        flush_log_to_disk();

        /* The stub is MRC/ORR/MCR/BX lr - it touches no memory, so the sp
         * handed to the trampoline is never dereferenced. Use INTRMGR's .data
         * (what psp2spl itself borrows for its own init) rather than SYSMEM's,
         * which no longer resolves now that its DATAMP is 0. */
        unsigned int stack_sva = 0;
        if (SPLx_TZS_GETMODSVA(SPLTZ_MODINFO_INTRMGR, 1, 0, &stack_sva) >= 0 && stack_sva) {
            stack_sva += 0x200u;
            log_hex_line("l1 scratch sp ", stack_sva);

            static const int masks[4] = { 0x10000, 0x20000, 0x40000, 0x80000 };
            unsigned int argp[2] = { stub_sva, stack_sva };
            for (int i = 0; i < 4; i++) {
                SceUID th = ksceKernelCreateThread("pfe_actlr", run_actlr_core,
                                                   0x10, 0x2000, 0, masks[i], NULL);
                if (th < 0) { log_hex_line("l1: thread create failed ", (unsigned int)th); continue; }
                if (ksceKernelStartThread(th, sizeof(argp), argp) >= 0)
                    ksceKernelWaitThreadEnd(th, NULL, NULL);
                ksceKernelDeleteThread(th);
                flush_log_to_disk();
            }
        } else {
            log_line("l1: no INTRMGR .data for scratch sp - not executing");
        }
    } else {
        log_line("l1: stub NOT visible from secure world - NOT executing (clean abort)");
    }

    /* Leave SYSMEM's .text as we found it. */
    for (unsigned int i = 0; i < sizeof(actlr_set) / 4; i++)
        ((volatile unsigned int *)ns_stub)[i] = 0xB6B6B6B6u;
    __asm__ __volatile__("mcr p15, 0, %0, c7, c11, 1" :: "r"(ns_stub));
    __asm__ __volatile__("dsb");
    __asm__ __volatile__("mcr p15, 0, %0, c7, c5, 1"  :: "r"(ns_stub));
    __asm__ __volatile__("dsb");
    __asm__ __volatile__("isb");
    ksceKernelFreeMemBlock(blk);
    log_line("l1: fill restored, NS alias released");
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
        flush_log_to_disk();
    }

    log_line("==== vita_prefetch_enabler boot done ====");

write_out:
    flush_log_to_disk();
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) { (void)argc; (void)args; return SCE_KERNEL_STOP_SUCCESS; }
