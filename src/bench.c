#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>

#include "bench.h"

#define BUF_BYTES   (4u * 1024u * 1024u)        /* 4x the 1 MiB PL310 */
#define HALF_BYTES  (BUF_BYTES / 2u)
#define HALF_WORDS  (HALF_BYTES / 4u)
#define LINE_BYTES  32u                         /* CTR says 32 on this part */
#define LINE_WORDS  (LINE_BYTES / 4u)

/*
 * SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW cannot satisfy 4 MiB here - ScePhyMemPartKD
 * is too small and the allocation fails with 0x80024302. This is the same
 * fallback type psp2prof's ring buffers use.
 */
#define MEMBLOCK_TYPE_RW_BIG 0x1080D006

static SceUID g_blk = -1;
static unsigned int *g_buf;

/*
 * The chase's shuffle scratch lives in the second half of the buffer, so the
 * working set (at most 2 MiB, at the start) never overlaps it and no extra
 * allocation is needed.
 */
#define ORDER_OFF_W HALF_WORDS

const unsigned int pfe_bench_ws_bytes[PFE_BENCH_WS_COUNT] = {
    4096u, 8192u, 16384u, 32768u, 65536u, 262144u, 524288u, 2097152u
};

const unsigned int pfe_bench_strides[PFE_BENCH_STRIDE_COUNT] = {
    4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u
};

int pfe_bench_init(void)
{
    if (g_blk >= 0) return 0;
    g_blk = ksceKernelAllocMemBlock("pfe_bench", MEMBLOCK_TYPE_RW_BIG,
                                    BUF_BYTES, NULL);
    if (g_blk < 0) return (int)g_blk;
    void *base = NULL;
    if (ksceKernelGetMemBlockBase(g_blk, &base) < 0 || !base) {
        ksceKernelFreeMemBlock(g_blk);
        g_blk = -1;
        return -1;
    }
    g_buf = (unsigned int *)base;
    for (unsigned int i = 0; i < BUF_BYTES / 4u; i++) g_buf[i] = i;
    return 0;
}

void pfe_bench_fini(void)
{
    if (g_blk >= 0) ksceKernelFreeMemBlock(g_blk);
    g_blk = -1;
    g_buf = 0;
}

void pfe_bench_pmu_enable(void)
{
    unsigned int pmcr = 0;
    __asm__ __volatile__("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));
    pmcr |= 1u;          /* E: enable counters */
    pmcr &= ~8u;         /* D: count every cycle, not every 64th */
    __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 0" :: "r"(pmcr));
    __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 1" :: "r"(1u << 31)); /* PMCNTENSET.C */
    __asm__ __volatile__("isb");
}

void pfe_bench_pmu_disable(void)
{
    /* PMCNTENCLR.C: stop the cycle counter again; PMCR is left as found. */
    __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 2" :: "r"(1u << 31));
    __asm__ __volatile__("isb");
}

static inline unsigned int ccnt(void)
{
    unsigned int v;
    __asm__ __volatile__("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
    return v;
}

/* Keeps the min-of-reps result, by cycles when the counter runs. */
static void keep_best(struct pfe_bench_res *best, unsigned int cyc,
                      unsigned int us, unsigned int n)
{
    int better = best->accesses == 0 ||
                 (cyc ? cyc < best->cycles : us < best->us);
    if (better) { best->cycles = cyc; best->us = us; best->accesses = n; }
}

static unsigned int g_seed = 0x12345677u;

static unsigned int lcg(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return g_seed >> 8;
}

/*
 * Links the first word of every cache line in [0, ws_bytes) into one random
 * cycle, each word holding the *word index* of the next line.
 *
 * The order array is shuffled by repeated random swaps rather than a true
 * Fisher-Yates, because Fisher-Yates needs a bounded modulo and there is no
 * divide available; the line count is a power of two, so a mask gives a
 * uniform index. Three passes is far more scrambling than is needed to defeat
 * a stride prefetcher, which is all this has to do.
 */
static unsigned int build_chase(unsigned int ws_bytes)
{
    unsigned int nlines = ws_bytes / LINE_BYTES;
    if (nlines < 2u) return 0;
    unsigned int mask = nlines - 1u;          /* nlines is a power of two */
    unsigned int *order = g_buf + ORDER_OFF_W;

    for (unsigned int i = 0; i < nlines; i++) order[i] = i;
    for (int pass = 0; pass < 3; pass++) {
        for (unsigned int i = 0; i < nlines; i++) {
            unsigned int j = lcg() & mask;
            unsigned int t = order[i]; order[i] = order[j]; order[j] = t;
        }
    }
    for (unsigned int k = 0; k < nlines; k++) {
        unsigned int cur  = order[k];
        unsigned int next = order[(k + 1u) & mask];
        g_buf[cur * LINE_WORDS] = next * LINE_WORDS;
    }
    return order[0] * LINE_WORDS;
}

/* Kept volatile so neither loop can be optimised away. */
static volatile unsigned int g_sink;

void pfe_bench_chase(unsigned int ws_bytes, struct pfe_bench_res *r)
{
    r->cycles = r->us = r->accesses = 0;
    if (!g_buf || ws_bytes < 2u * LINE_BYTES || ws_bytes > HALF_BYTES) return;

    unsigned int start = build_chase(ws_bytes);
    volatile unsigned int *p = (volatile unsigned int *)g_buf;

    /* One untimed lap so the timed reps all start from the same warm state. */
    unsigned int idx = start;
    for (unsigned int n = 0; n < ws_bytes / LINE_BYTES; n++) idx = p[idx];

    for (int rep = 0; rep < PFE_BENCH_REPS; rep++) {
        idx = start;
        unsigned int t0 = ksceKernelGetSystemTimeLow();
        unsigned int c0 = ccnt();
        for (unsigned int n = 0; n < PFE_BENCH_CHASE_ACCESSES; n++)
            idx = p[idx];                       /* dependent load */
        unsigned int c1 = ccnt();
        unsigned int t1 = ksceKernelGetSystemTimeLow();
        g_sink = idx;
        keep_best(r, c1 - c0, t1 - t0, PFE_BENCH_CHASE_ACCESSES);
    }
}

/* Reads a 2 MiB half twice, line by line, to push the other half out of L2. */
static void evict_with(unsigned int half)
{
    volatile unsigned int *p = (volatile unsigned int *)g_buf + half * HALF_WORDS;
    unsigned int acc = 0;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned int i = 0; i < HALF_WORDS; i += LINE_WORDS) acc += p[i];
    g_sink = acc;
}

void pfe_bench_stride(unsigned int stride_bytes, struct pfe_bench_res *r)
{
    r->cycles = r->us = r->accesses = 0;
    if (!g_buf || stride_bytes < 4u || (stride_bytes & (stride_bytes - 1u)))
        return;                                      /* power of two only */

    unsigned int step = stride_bytes / 4u;           /* in words */
    /* One pass, no wrap. A shift, since a variable divide does not exist. */
    unsigned int n    = HALF_BYTES >> __builtin_ctz(stride_bytes);

    for (int rep = 0; rep < PFE_BENCH_REPS; rep++) {
        unsigned int walk = (unsigned int)rep & 1u;
        evict_with(walk ^ 1u);
        volatile unsigned int *p = (volatile unsigned int *)g_buf + walk * HALF_WORDS;
        unsigned int acc = 0;
        unsigned int t0 = ksceKernelGetSystemTimeLow();
        unsigned int c0 = ccnt();
        for (unsigned int i = 0; i < HALF_WORDS; i += step)
            acc += p[i];                                  /* independent loads */
        unsigned int c1 = ccnt();
        unsigned int t1 = ksceKernelGetSystemTimeLow();
        g_sink = acc;
        keep_best(r, c1 - c0, t1 - t0, n);
    }
}
