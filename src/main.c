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
 * To disable the plugin itself (no config read, no secure-world touch, no
 * log write), comment out or remove its line in ur0:tai/config.txt - that
 * is ordinary taiHEN usage, nothing custom needed.
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
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>

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

static const unsigned int pl310_set[9] = {
    0xE59F1018u,  /* LDR r1,[pc,#0x18]      ; r1 = PL310_BASE */
    0xE5910F60u,  /* LDR r0,[r1,#0xF60]     ; PREFETCH_CTRL */
    0xE3800201u,  /* ORR r0,r0,#0x10000000  ; set bit[28] */
    0xF57FF040u,  /* DSB SY */
    0xE5810F60u,  /* STR r0,[r1,#0xF60] */
    0xF57FF040u,  /* DSB SY */
    0xE5910F60u,  /* LDR r0,[r1,#0xF60]     ; read back */
    0xE12FFF1Eu,  /* BX lr */
    0x1A002000u,  /* literal: PL310_BASE */
};

extern int splCl_smc(unsigned int a0, unsigned int a1, unsigned int a2,
                     unsigned int a3, unsigned int idx);
#define SPLTZ_SMC_EXEC_WITH_SP 0x401u

/*
 * Maps a second, execute-only alias of the physical page already backing
 * code_va - our own module's own already-loaded, already-coherent code -
 * instead of injecting a stub into another module's memory. No write, no
 * cache flush: the bytes at that PA are already correct and were already
 * made I-fetchable by the ordinary module loader when we were loaded.
 *
 * Per SKGleba (2026-11-24): va2pa on your own code, map that PA as a
 * kernel RX memblock, feed the result straight to EXEC_WITH_SP - no
 * SPLx_TZS_GETMODSVA/module lookup needed for our own code at all, only
 * for the stack (still sourced from SYSMEM's data segment below).
 *
 * Caller must keep the returned block alive until done executing through
 * *out_exec_va, then free it.
 */
static SceUID map_own_code_rx(const void *code_va, void **out_exec_va)
{
    uintptr_t pa = 0;
    if (ksceKernelVAtoPA(code_va, &pa) < 0) return -1;

    unsigned int page_pa  = (unsigned int)pa & ~0xFFFu;
    unsigned int page_off = (unsigned int)pa &  0xFFFu;

    static const unsigned int rx_types[2] = {
        SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RX,
        SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_TOOL_RX,
    };
    SceUID blk = -1;
    void  *base = NULL;
    for (int i = 0; i < 2; i++) {
        SceKernelAllocMemBlockKernelOpt opt;
        memset(&opt, 0, sizeof(opt));
        opt.size  = (unsigned int)sizeof(opt);
        opt.attr  = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_PADDR;
        opt.paddr = page_pa;
        blk = ksceKernelAllocMemBlock("pfe_rx", rx_types[i], 0x1000, &opt);
        if (blk >= 0) break;
    }
    if (blk < 0) return blk;

    ksceKernelGetMemBlockBase(blk, &base);
    *out_exec_va = (void *)((unsigned int)base + page_off);
    return blk;
}

static char g_log[1024];
static int  g_log_len;

static void log_line(const char *s)
{
    int n = 0;
    while (s[n] && g_log_len + n < (int)sizeof(g_log) - 2) n++;
    for (int i = 0; i < n; i++) g_log[g_log_len++] = s[i];
    g_log[g_log_len++] = '\n';
    LOG("%s\n", s);
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

/* Reads CONFIG_PATH and decides which sites to enable. Any failure to open
 * the file, or any line that isn't a recognized "key=value", leaves that
 * key's flag at its "off" default. */
static void read_config(int *want_l1, int *want_l2)
{
    *want_l1 = 0;
    *want_l2 = 0;

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
        }
    }
}

static int run_actlr_core(SceSize args, void *argp)
{
    unsigned int exec_va = *(unsigned int *)argp;
    unsigned int stack_sva = *((unsigned int *)argp + 1);

    unsigned int before = 0;
    __asm__ __volatile__("mrc p15, 0, %0, c1, c0, 1" : "=r"(before));
    int r = splCl_smc(0, stack_sva, exec_va, 0, SPLTZ_SMC_EXEC_WITH_SP);
    unsigned int after = 0;
    __asm__ __volatile__("mrc p15, 0, %0, c1, c0, 1" : "=r"(after));

    unsigned int mpidr = 0;
    __asm__ __volatile__("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    LOG("actlr core%u: before=%08X after=%08X smc=%d\n", mpidr & 0xF, before, after, r);
    (void)args;
    return 0;
}

int module_start(SceSize argc, const void *args)
{
    (void)argc; (void)args;
    g_log_len = 0;
    log_line("==== vita_prefetch_enabler boot start ====");

    int want_l1 = 0, want_l2 = 0;
    read_config(&want_l1, &want_l2);

    if (!want_l1 && !want_l2) {
        log_line("nothing to enable, exiting cleanly");
        goto write_out;
    }

    {
        SPLx_CFK_PREP(NULL, 0, "[PFE]");
        int ret = SPLx_CFK_INIT(SPL_IFL(_LV0) | SPL_IFL(_TZS), 0, 1, 1,
                                 SPLV0_COMMBACKUP_CRITICAL);
        log_hex_line("cfk_init_ret ", (unsigned int)ret);
        if (ret < 0) { log_line("CFK init failed, aborting"); goto write_out; }
    }

    {
        unsigned int data_sva = 0;
        SPLx_TZS_GETMODSVA(SPLTZ_MODINFO_SYSMEM, 1, 0, &data_sva);
        unsigned int stack_sva = data_sva + 0x200u;
        log_hex_line("sysmem_data ", data_sva);

        if (want_l1) {
            void *exec_va = NULL;
            SceUID blk = map_own_code_rx(actlr_set, &exec_va);
            if (blk < 0) {
                log_hex_line("actlr rx map failed ", (unsigned int)blk);
            } else {
                log_hex_line("actlr exec_va ", (unsigned int)exec_va);
                static const int masks[4] = { 0x10000, 0x20000, 0x40000, 0x80000 };
                unsigned int argp[2] = { (unsigned int)exec_va, stack_sva };
                for (int i = 0; i < 4; i++) {
                    SceUID th = ksceKernelCreateThread("pfe_actlr", run_actlr_core,
                                                       0x10, 0x2000, 0, masks[i], NULL);
                    if (th < 0) continue;
                    if (ksceKernelStartThread(th, sizeof(argp), argp) >= 0)
                        ksceKernelWaitThreadEnd(th, NULL, NULL);
                    ksceKernelDeleteThread(th);
                }
                ksceKernelFreeMemBlock(blk);
                log_line("actlr: set on all cores");
            }
        }

        if (want_l2) {
            void *exec_va = NULL;
            SceUID blk = map_own_code_rx(pl310_set, &exec_va);
            if (blk < 0) {
                log_hex_line("pl310 rx map failed ", (unsigned int)blk);
            } else {
                log_hex_line("pl310 exec_va ", (unsigned int)exec_va);
                int r = splCl_smc(0, stack_sva, (unsigned int)exec_va, 0, SPLTZ_SMC_EXEC_WITH_SP);
                log_hex_line("pl310 smc_ret ", (unsigned int)r);
                ksceKernelFreeMemBlock(blk);
            }
        }
    }

    log_line("==== vita_prefetch_enabler boot done ====");

write_out:
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
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) { (void)argc; (void)args; return SCE_KERNEL_STOP_SUCCESS; }
