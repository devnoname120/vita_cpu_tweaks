/*
 * Two memory benchmarks, used to decide whether the ACTLR/PL310 bits this
 * plugin can flip are actually worth flipping.
 *
 * They answer different questions and the distinction matters:
 *
 *  - pfe_bench_chase() is a *dependent* load chain through a randomised cycle
 *    of cache lines confined to a working set. Each load's address comes from
 *    the previous load, so there is no memory-level parallelism and nothing to
 *    prefetch: the time per access is latency, and the shape of the curve as
 *    the working set grows reveals the *effective capacity* of each cache
 *    level. That is the only way to see what ACTLR's "allocate in one cache
 *    way only" bit costs - it does not change latency, it changes how much of
 *    L1 is usable, so a benchmark that misses in L1 regardless (like a big
 *    strided walk) would show nothing at all.
 *
 *  - pfe_bench_stride() is *independent* strided reads over memory that is
 *    not in any cache. That is what the stride prefetchers act on, so it is
 *    the right measure for the ACTLR prefetch bits and for PL310's data
 *    prefetch and prefetch offset. It walks one 2 MiB half of the buffer
 *    exactly once, after reading the other half twice to evict it: a walk
 *    that wrapped round a buffer would, at large strides, revisit a footprint
 *    small enough to sit in the 1 MiB L2 and measure L2 hits instead.
 *
 * Results carry CPU cycles from the PMU cycle counter (the benchmark thread is
 * pinned to one core, so the counter is coherent) and microseconds from the
 * system timer; the two together also give the clock the run happened at.
 * Each is the minimum of several repetitions, since on a live system noise
 * only ever adds time. Nothing here divides: the Cortex-A9 has no integer
 * divide instruction and this module links -nostdlib, so there is no
 * __aeabi_uidivmod to call. Every size is a power of two and every wrap is a
 * mask; per-access figures are computed on the host.
 */
#ifndef PFE_BENCH_H
#define PFE_BENCH_H

#define PFE_BENCH_CHASE_ACCESSES (1u << 18)
#define PFE_BENCH_REPS           3

#define PFE_BENCH_WS_COUNT     8
#define PFE_BENCH_STRIDE_COUNT 8

extern const unsigned int pfe_bench_ws_bytes[PFE_BENCH_WS_COUNT];
extern const unsigned int pfe_bench_strides[PFE_BENCH_STRIDE_COUNT];

struct pfe_bench_res {
    unsigned int cycles;    /* PMU cycles, 0 if the counter is not running */
    unsigned int us;        /* system-timer microseconds */
    unsigned int accesses;  /* loads performed in the timed region */
};

/* Allocates the buffer. Returns 0, or a negative error. */
int  pfe_bench_init(void);
void pfe_bench_fini(void);

/* Enables the PMU cycle counter on the calling core. */
void pfe_bench_pmu_enable(void);

void pfe_bench_chase(unsigned int ws_bytes, struct pfe_bench_res *r);
void pfe_bench_stride(unsigned int stride_bytes, struct pfe_bench_res *r);

#endif /* PFE_BENCH_H */
