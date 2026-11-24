# SPL_TZSGMSV_EBADMOVP on retail 3.60 (enso)

Investigation into why `SPLx_CFK_INIT()` fails with `SPL_TZSGMSV_EBADMOVP`
(-36) during `SPLx_TZS_INIT`'s own eager resolution of SYSMEM (module 1),
on the retail PCH-1000 test unit, firmware 3.60 via enso.

---

## RESOLVED (2026-11-24)

EBADMOVP is **fixed**. The cause was exactly the mandatory-`.data`-resolution
side effect described below: `SPLx_TZS_INIT`'s own `.text`-only SYSMEM lookup
still forces a `.data` resolution because SYSMEM's `tzs_offsets.c` table entry
has a nonzero `DATAMP`, and that offset is not a real `MOVW`/`MOVT` pair on
retail 3.60. Nothing in psp2spl ever actually needs SYSMEM's `.data`, so the
fix is to set SYSMEM's `DATAMP` to `0` (same sentinel `BUSERR` already uses) -
see `patches/0001-sysmem-datamp-zero.patch` and `../../third_party/psp2spl/`.
Boots 8-11 confirm it on hardware: `TZSGMS: m1 .text: 0x00200000 | .data:
0x00000000` (no `.data` resolution attempted) and `cfk_init_ret 0x5` (success).

So the sections below titled "the previously-tested patch used the wrong
adjustment" and "Nothing we can fix from the plugin side" are **superseded** -
they were written before the DATAMP=0 approach. Keep them only for the check
mechanics they explain.

### Post-fix: the EXEC_WITH_SP hang, and the L1/L2 split

With CFK_INIT working, the plugin reached its real job - running the ACTLR and
PL310 setters in secure world via `EXEC_WITH_SP` (SMC 0x401) - and **hung the
machine** (boot 8, unrecoverable except by power-cycle; the dead-man's switch
made the next boot clean). Root cause, nailed down read-only:

* SYSMEM's `exec_with_sp(arg, sp, func)` trampoline (disassembled at SYSMEM
  `.text+0x60`) is literally `mov sp, r1` / `blx r2` - **no address
  translation**, run under the *secure* page tables. So `func` must be
  executable *in secure world*.
* We were handing it `ksceKernelGetMemBlockBase()`'s **non-secure** kernel VA.
  `blx` into unmapped secure memory -> abort in the monitor -> hang.
* `SPLx_TZS_PRINTTBR(1)` (boot 9, `boot_log_9_printtbr.log`, parsed by
  `scripts/analyze_tbr.py`) proves it: our code page's PA (`0x4128F000`) has
  **no secure mapping at all**, and there is **no secure page that is both
  writable and executable**. So neither "alias our own code" nor "allocate a
  page, write a stub, execute it" can work through the page tables.

That splits the two knobs:

* **L2 / PL310 - SOLVED.** PL310 is MMIO, identity-mapped secure-RW at
  `sva 0x1A000000` (1 MiB L1 section, AP=001). No code execution needed: set
  `PREFETCH_CTRL` (`0x1A002F60`) bit 28 with `SPLx_TZS_WRITE32`. Boot 11:
  `0x21000000 -> 0x31000000`, read-back confirms, "data prefetch ENABLED",
  clean boot. This is what the plugin now does.
* **L1 / ACTLR - SOLVED (boot 21).** ACTLR `0x149 -> 0x14F` on all four cores,
  same result the 3.65 devkit got, in the same boot that enables L2. The
  mechanism is *not* the devkit's: inject one L2 descriptor mapping a
  previously-unmapped secure VA to an unused **in-TZ** physical page
  (`0x401E3000`, in the gap between `0x401E2000` and `0x401E4000`) as
  **Normal Non-Cacheable, PL1-RW, executable** (`desc = pa | 0x52`), write the
  stub there with `SPLx_TZS_WRITE32`, `EXEC_WITH_SP` it per core, then clear
  the scratch and remove the descriptor. That one choice of attributes defeats
  every wall found below at once: the PA is in-TZ so the firewall permits it,
  the writes are *secure* so the firewall permits them, non-cacheable means no
  cp15 maintenance is needed and the PL310 security-tagged-line problem cannot
  arise, and a VA that was invalid before cannot have a stale TLB entry. The
  scratch page is re-verified unmapped at runtime (`tz_pa_is_mapped()`) before
  anything is written, and every address is derived per boot - SYSMEM's `.text`
  moved between `0x000E0000`, `0x00200000` and `0x00210000` across these tests.

* **Made layout-agnostic (boots 23-29).** Everything the L1 path had
  hardcoded is now read or derived at runtime, so it follows whatever layout
  the running firmware and model have rather than retail 3.60's:

  * the TZ region base and length and both secure TTBR offsets come from
    psp2spl's own resolved `struct tzs_layout_s` (`statics[]` and `mbase`) via
    `src/spl_layout.c`, instead of being copied out of `tzs_offsets.c`. Boot 23
    read back `0x40000000 / 0x00200000 / 0x108000 / 0x10C000` and `mbase
    0x30600000`, which matches splib's own `BA: 40000000[00200000] <=>
    30600000` log line - an independent confirmation.
  * the scratch page is found by one pass over both TTBRs that builds a bitmap
    of every in-use TZ page (mapped by a descriptor, or holding a page table),
    then takes the highest hole whose neighbours are both in use. On retail
    3.60 it independently derives `0x401E3000` - the same page that was
    hand-picked by eye from boot 21's PRINTTBR dump.
  * the stub's VA slot is any zero L2 entry, swept first across the 1 MiB
    holding SYSMEM `.text` and then across the low 16 MiB. This matters: how
    much of that 1 MiB SYSMEM occupies varies per boot. Boot 24 had every slot
    in the `0x15000-0x1F000` window that worked on boot 21 taken, boots 26-28
    landed on `0x215000`, boot 25 on `0x225000`, and boot 29 fell through to
    the 16 MiB sweep. `desc_sva` differed on every single boot.
  * PL310's `PREFETCH_CTRL` secure VA is gated on `SVA2PA(sva) == sva`, so a
    build whose MMIO section is not identity-mapped aborts cleanly instead of
    taking a blind secure write to an unknown address.

  Result on retail 3.60: 4/4 cores `0x149 -> 0x14F` plus PL310
  `0x21000000 -> 0x31000000` on four consecutive boots (26-29) across three
  different target slots. Two bugs were caught by the gates rather than by a
  hang, which is the whole point of them: the bitmap scan first missed small
  pages encoded `dt == 3` (bit 0 of a small-page descriptor is XN, so an
  unexecutable page reads as 3, not 2), under-marking most data pages and
  proposing a page that `tz_pa_is_mapped()` then rejected; and boot 25's worker
  threads silently did nothing, which the surrounding log lines made look like
  a success. The per-core loop now ends in an explicit `N/4` result line, and a
  failed `ksceKernelStartThread` is logged instead of being skipped silently.
  Boot 25's cause is still unidentified and has not recurred.

* **Hardware identification (boot 31), and what it rules in and out.** A
  read-only probe (`probe_only=on`) now reads per-core CPU registers with
  plain non-secure `MRC`s - no SMC, so it is safe on any unit - plus the PL310
  ID and config registers by secure read:

  | register | value | meaning |
  |---|---|---|
  | MIDR | `0x412FC09A` | ARM Cortex-A9 **r2p10**, identical on all 4 cores |
  | ACTLR | `0x149` | identical on all 4 cores; see below |
  | CTR | `0x83338003` | I and D cache lines both 32 bytes |
  | CLIDR | `0x09200003` | one visible level, separate L1 I/D (PL310 is external) |
  | PL310 Cache ID | `0x410000C7` | part 3 = L310, **RTL release `0x07`** |
  | PL310 AUX_CTRL | `0x60090001` | bit 29 = 1, bit 28 = 0 |
  | PL310 PREFETCH_CTRL | `0x21000000` | bit 29 = 1, bit 28 = 0, offset `[4:0]` = 0 |

  Two conclusions follow directly:

  * **Double linefill must never be enabled on this hardware.** RTL release
    `0x07` is `L310_CACHE_ID_RTL_R3P1_50REL0` in Linux's own table (`R3P2` is
    `0x08`), and errata 752271 - double linefill can corrupt data - affects
    r3p0, r3p1 and r3p1-50rel0, fixed only in r3p2. So the Vita's PL310 is an
    affected part and the only workaround is leaving the feature off. This
    kills what had looked like the most promising "fetch granularity" knob.
  * **The AUX_CTRL / PREFETCH_CTRL aliasing is real here, not just in the
    TRM.** AUX_CTRL bits 29/28 read `1`/`0` and PREFETCH_CTRL bits 29/28 read
    the same, so instruction prefetch is already on and only data prefetch was
    off. That is now confirmed on the hardware rather than assumed.

  Also worth noting: `PREFETCH_CTRL[4:0]`, the L2 prefetch offset, is **0**.

* **ACTLR bit 8 is set by the firmware, and clearing it works (boot 32).**
  `0x149` decodes as FW (bit 0), write-full-line-of-zeros (3), SMP (6) and
  **"allocate in one cache way only" (8)**. The Cortex-A9 TRM's wording for
  bit 8 is "Enable allocation in one cache way only. For use with memory copy
  operations to reduce cache pollution", and - the significant part - **its
  reset value is zero**. So this is not a leftover: the boot chain sets it
  deliberately, on every core, and it confines L1 D-cache linefill allocation
  to one of the A9's four ways.

  The stub is now generated at runtime from a set mask and a clear mask, with
  both loaded PC-relative from literals in the same scratch page (an ARM
  `imm12` cannot encode every mask), so arbitrary bits can be set *or cleared*.
  Verified by disassembling the generated words before ever executing them.
  A new tri-state config key `actlr_alloc_one_way = on|off|keep` drives it;
  `keep` is the default, because for a bit the firmware sets, "off" has to mean
  "actively clear" rather than "don't set".

  Result with `actlr_alloc_one_way=off`: **`0x149 -> 0x04F` on all four
  cores**, re-read a few milliseconds later from a different thread as still
  `0x04F`, with the system booting and running normally. So the bit is
  writable, the clear sticks, nothing re-asserts it, and it is not load-bearing
  for boot.

  **What is still unknown: whether clearing it is a performance win.** That
  claim is unmeasured. Quantifying it needs a memory-access benchmark of the
  kind the 3.65 devkit's `stride/` harness provided, which does not exist in
  this repo. Do not present the AOW clear as a speed-up until it is measured.

  **Measured since (boots 33-35): it makes no difference either way** - see
  the next bullet. Leave it at `keep`.

* **Benchmark results (boots 33-35, `bench=on`).** `src/bench.c` runs two
  tests per configuration ("leg"), timed in PMU cycles on a core-0-pinned
  thread: a dependent-load *chase* through a random cycle of lines in a
  4K..2M working set (latency and effective cache capacity), and a
  single-pass *stride* walk over 2 MiB that has just been evicted by reading
  the other 2 MiB twice (what prefetchers act on). Ten legs, each fully
  specifying ACTLR bits 1/2/8 and PL310 data prefetch + offset, first leg
  repeated last. Tables: `captures/boot33_bench_table.txt`,
  `boot34_bench_table.txt` (both at boot, ~330 MHz, agree within 1-2%) and
  `boot35_bench_table.txt` (same suite with the shell running - noisier,
  same ranking). Cycles per load, boot 33:

  | leg | stride 4 | 16 | 32 | 64 | 128 | 256 | chase 2M |
  |---|---|---|---|---|---|---|---|
  | firmware default | 12.7 | 27.8 | 31.4 | 31.8 | 31.7 | 32.2 | 31.6 |
  | AOW cleared | 12.7 | 27.9 | 31.2 | 31.4 | 31.4 | 32.2 | 31.5 |
  | ACTLR prefetch (bits 1+2) | 7.9 | 17.1 | 27.3 | 27.6 | 28.1 | 29.1 | 31.3 |
  | PL310 prefetch, offset 0 | 8.2 | 22.2 | 32.4 | 34.9 | 34.9 | 35.1 | 28.7 |
  | both, offset 0 | 7.9 | 17.0 | 27.2 | 29.5 | 29.9 | 31.2 | 28.6 |
  | both, offset 7 | 6.2 | 11.6 | 16.5 | 20.3 | 26.9 | 28.6 | 29.3 |
  | both, offset 15 | 6.3 | 12.1 | 17.1 | 17.4 | 21.0 | 27.3 | 29.7 |
  | both, offset 31 | 6.6 | 12.7 | 18.5 | 18.7 | 19.0 | 22.4 | 30.0 |

  * **AOW (ACTLR bit 8) is neutral.** The chase holds 4 cycles out to a
    32 KiB working set with the bit set or cleared, so all of L1 is usable
    either way; it evidently only matters for the memory-copy case its TRM
    description names. Strides do not move either.
  * **PL310 data prefetch at the firmware's offset 0 is close to useless**:
    nothing over ACTLR prefetch alone, and ~10% *slower* on its own at
    strides of 64 bytes and up. The offset is what makes it pay.
  * **ACTLR prefetch + PL310 prefetch at offset 15 is the best all-round
    setting**: 35-55% fewer cycles per load for strides up to 128 bytes, and
    the random 2 MiB chase still slightly faster than stock. Offset 31 wins
    only at 128-256 byte strides; offset 23 was not tested (errata 765569).
  * Cache geometry it shows: L1 hit 4 cycles; PL310 hit ~21 cycles at 64 KiB,
    ~25 at 256-512 KiB (TLB reach); AUX_CTRL `0x60090001` = 16 ways x 64 KiB
    = 1 MiB L2.

* **Never hold up boot for seconds: it crashes SceShell (boots 33-34).** The
  first two benchmark boots ran the ~9 s suite inside `module_start()`. Both
  completed and restored every register, then SceShell died a few seconds
  into the home screen loading: `ScePafJobDefault` called a null function
  pointer, with an identical stack in both core dumps
  (`captures/boot33_crash`, `boot34_crash`, decoded with psp2_core_parse).
  Both dumps' TTY shows the trigger: SceShell's MTP server failed to start
  (`sceMtpStart` `0x80540008`, interface `0x80543601`, and `0x80540008` still
  in R11 of the crashing frame), and its error path is what jumps to null.
  Moving the suite to a thread that starts 30 s after boot (boot 35) removed
  the crash. The normal apply path takes well under a second, which is why
  boots 26-32 never hit this; keep it that way.

* **Per-field config and game-clock benchmarks (boots 37-42).** The config
  became 13 INI-style keys, one per register field (`prefetch_enabler.example.ini`).
  Things learned on the way:

  * **Config size guard fired (boot 37, first try).** The new example is
    4663 bytes with CRLF, over the old 4 KiB buffer; the plugin logged
    `FILE TOO LARGE` and did nothing, as designed. Buffer is now 16 KiB and
    a host test checks the example fits as CRLF.
  * **Kernel panic from a wrong power-API signature (boot 38,
    `captures/boot38_kpanic`).** On 3.60 the kernel GPU clock calls are the
    old ES4 pair: `kscePowerGetGpuClockFrequencyInternal(int *a, int *b)`
    and `...SetGpuClockFrequencyInternal(int a, int b)`. Called with no
    arguments the getter wrote through a garbage pointer (data abort at
    `0xDE`). `kscePowerSetGpuClockFrequency` (NID `0x717DB06C`) is not
    exported to kernel code on 3.60 at all, despite vita-headers' db.
  * **A one-off clock set does not hold (boot 39).** 500 MHz was set, but
    the per-leg measured clock read ~406 then ~330: something (PSVshell or
    ScePower reapplying the shell's request) puts it back within seconds.
    The bench now re-asserts the clocks before every individual test, and
    `tools/bench_table.py LOG 500` drops any test measured >3% off target.
    Boots 40-41: every leg at 493-498 MHz. The GPU core clock stays at
    41 MHz with the GPU idle; irrelevant to a CPU-side test.
  * **Boot can't be held for a cleaner run.** Doing the suite inside
    `module_start` crashes SceShell (boots 33-34, above), so it runs at the
    home screen. With clocks held and 3 interleaved rounds (boot 41) the
    best two of three agree to a median 0.2%, 90th percentile 2.3%. The one
    exception is the 2 MiB random chase, purely DRAM-bound and sharing DRAM
    with the running system: it swings 47-83 cycles between legs with
    identical settings, so no conclusion is drawn from that column.

  Results at 500 MHz (boot 41, `captures/boot41_bench500_table.txt`; boot
  40 agrees), stride walk, cycles per load:

  | leg | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 |
  |---|---|---|---|---|---|---|---|---|
  | stock | 17.9 | 32.0 | 38.2 | 49.6 | 49.5 | 49.2 | 50.0 | 51.4 |
  | ACTLR bit 2 only (dside) | 18.1 | 32.7 | 38.9 | 49.1 | 49.6 | 49.0 | 50.0 | 51.3 |
  | ACTLR bit 1 only (hint) | 10.3 | 19.0 | 21.9 | 37.9 | 38.3 | 39.9 | 41.9 | 46.2 |
  | ACTLR bits 1+2 | 10.5 | 19.0 | 22.2 | 37.7 | 38.0 | 39.5 | 42.0 | 45.6 |
  | PL310 o15 only | 7.3 | 11.5 | 15.3 | 21.6 | 21.4 | 28.2 | 36.7 | 44.9 |
  | bit 2 + PL310 o15 | 7.6 | 11.6 | 15.7 | 21.6 | 20.9 | 28.2 | 37.2 | 44.7 |
  | bits 1+2 + PL310 o0 | 10.4 | 19.0 | 23.1 | 37.9 | 43.3 | 44.9 | 46.9 | 48.4 |
  | bits 1+2 + PL310 o7 | 6.3 | 9.6 | 15.3 | 21.3 | 28.2 | 36.9 | 39.1 | 46.4 |
  | bits 1+2 + PL310 o15 | 6.7 | 10.3 | 15.4 | 22.9 | 24.3 | 28.3 | 37.2 | 41.4 |
  | bits 1+2 + PL310 o31 | 7.1 | 11.5 | 16.1 | 26.2 | 25.3 | 25.7 | 30.4 | 38.9 |
  | same as o15, prefetch drop off | 6.5 | 10.1 | 14.8 | 22.9 | 23.3 | 28.1 | 37.3 | 41.6 |

  * **ACTLR bit 2 (L1 D-side prefetch) does nothing on this chip**, alone or
    combined. **Bit 1 (L2 prefetch hint) carries the whole A9-side gain.**
    The example config therefore sets bit 1 and leaves bit 2 at the
    firmware's 0: boot 42 verified `0x149 -> 0x14B` on all four cores,
    PL310 `0x21000000 -> 0x3100000F`.
  * At 500 MHz PL310 offset 15 is strong even without the hint; the hint
    adds ~10% at strides 4-8 and at 512 and costs ~6-14% at 32-64. Offset 7
    wins below 16 bytes, 31 above 128; 15 is the all-rounder. Prefetch drop
    makes no measurable difference.
  * Absolute numbers are higher in cycles than at ~330 MHz (boots 33-35)
    because DRAM latency is fixed in nanoseconds; the ranking is the same.

* **Bench at boot, then self-disable and reboot (boots 43-44).** Holding
  boot only costs a SceShell crash *after* the suite has finished and
  written its log (boots 33-34), so `bench.timing = boot` (now `benchmark.timing`) runs it inside
  `module_start` on a near-idle system. To keep that from ever crashing or
  repeating, a bench run now (1) rewrites its own config line to
  `mode = off` in place and re-reads the file to confirm, refusing to run
  otherwise - with `self_disable = no` an unconfirmed switch could become a
  reboot loop; (2) saves its log to `ur0:data/prefetch_enabler_bench.log`,
  which normal boots never overwrite; (3) ends with a cold reboot through
  `kscePowerRequestColdReset`, looked up at runtime with taiHEN's
  `module_get_export_func` rather than imported, after boot 38 showed the
  3.60 db lists exports the kernel does not have. Boot 44: config switched
  (CRLF kept), suite ran, reset found and requested, console came back to
  a normal home screen with no crash dump.

  Boot-time data (`captures/boot43_bench500_boot_table.txt`,
  `boot44_bench500_boot_table.txt`) is the cleanest so far: boots 43 and 44
  agree to a median 0.05% (90th percentile 0.7%). Strides read 3-8% fewer
  cycles than at the home screen (less DRAM contention), same ranking. The
  2 MiB random chase, unusable at the home screen, is now stable: 33.8
  cycles stock, 31.3 with hint + PL310 offset 15 (-7%), 30.0 with offset 0.
  The GPU clock also takes at boot (222/222), unlike at the idle home screen.

* **`enable` / `probe` / `benchmark = once` (boots 45-48).** The single
  `mode` key became three switches, so a one-shot benchmark no longer
  knocks out a config meant to stay enabled: `benchmark = once` rewrites
  only its own line to `benchmark = off` and leaves `enable` alone. Boots
  47-48, with `enable = on`, `self_disable = no`, `benchmark = once`,
  `benchmark.timing = boot`: the benchmark boot switched its line (CRLF
  kept), ran, and rebooted; the next boot came up with the plugin still
  enabled and applied ACTLR `0x14B` x4 and PL310 `0x3100000F`. No crash dump.
  (In the code and earlier entries, "bench mode" refers to this.)

  The rest of this bullet is the record of how the dead ends were eliminated.

* **L1 / ACTLR - the dead ends, and why the devkit's technique does not port.**
  ACTLR is a cp15 register, settable only by an `MCR` in secure PL1, per core.
  A non-secure PL1 `MCR` does not work (boot 10 wedged the probe thread), so
  it has to run in secure world via `EXEC_WITH_SP`, which needs a
  secure-executable `func`.

  The 3.65 devkit solved this (2026-11-23) by staging the stub in the
  unreferenced `0xB6` fill at the tail of SYSMEM's own secure `.text` - that
  page is secure R-X - writing it through a *second, non-secure* mapping of
  the same physical page (AP/XN are per-translation-table, not
  per-physical-page), then cleaning to PoU and executing the secure VA.
  **That does not work on retail 3.60.** Boots 12-15, with two safety gates
  that turned every failure into a clean logged abort rather than a hang:

  * boot 12 located the live/fill boundary at `.text+0x140FC` (3844 bytes of
    fill to the page end). Note the devkit's boundary was `+0x140CC` - the
    address must be derived at runtime, not copied.
  * the NS alias genuinely maps the right page: `ATTR_HAS_PADDR` is honoured
    and the alias reads back the same `0xB6` fill (boot 14).
  * the write lands and is visible from the NS side (`0xEE110F30`), but the
    secure side keeps reading `0xB6B6B6B6` - after removing our own
    self-pollution (gate 1 now reads via the NS alias, not a secure read),
    after cleaning to both PoU and PoC, and after a successful PL310
    clean-invalidate-by-PA (boot 15). So **NS writes into the TZ-protected
    region never become visible to secure world on this firmware.**
  * and there is no way around it by aliasing: `PA 0x40188000` appears
    exactly once in the secure tables (`sva 0x00214000`, AP=101 R-X). Its
    neighbours `0x40189000`-`0x4018F000` are writable but `XN`. Writable and
    executable are strictly disjoint across the entire secure address space.

  The plugin therefore attempts L1 and **aborts cleanly** if the stub is not
  visible from secure world; it never executes an unverified `func`, so it
  cannot reproduce boot 8's hang.

  Remaining in-plugin option: use `SPLx_TZS_WRITE32` to temporarily flip that
  one L2 descriptor's AP bits to PL1-RW, write the stub with secure writes,
  restore the descriptor, with TLB maintenance either side - entirely inside
  secure world, so none of the NS/S visibility problem applies. This is a
  real escalation (a corrupted secure descriptor breaks secure world) and is
  **not implemented**. Outside this plugin, the kernel_boot_loader byte-patch
  route sidesteps the whole problem.

`probe_only=on` in `ur0:data/prefetch_enabler.txt` re-runs the read-only
PRINTTBR + non-secure-ACTLR reconnaissance without touching `EXEC_WITH_SP`.

---

## The check

`SPLx_TZS_GETMODSVA` reads two 32-bit words at a hardcoded `.text`-relative
offset (from psp2spl's own `SPLTZl_OPS` table) and requires them to look
like a Thumb2 `MOVW`+`MOVT` pair:

```
w0 = *(u32*)(sva)       fail if (byte(w0>>20) != 0x30) && ((w0 & 0xFFFF) & 0xFBF0) != 0xF240
w1 = *(u32*)(sva+4)     fail if (byte(w1>>20) != 0x34) && ((w1 & 0xFFFF) & 0xFBF0) != 0xF2C0
```

Each half passes via *either* of two independent signals (an OR), not
both. On failure it calls back into our registered kprintf handler with
`(dbgid, mod, text_base, offset, w0, w1, w2, w3)` before returning -36.

For SYSMEM (mod 1) on this unit: `.text base = 0x00200000`, table offset =
`0x1C44`, i.e. it reads at SVA `0x00201C44` - which does **not** satisfy
the check. The table entry (byte offset 0x44 inside the extracted
`SPLTZl_OPS` subtype-2 block, see `psp2spl_objs/ops.bin`) holds
`0x00041C44`: low 16 bits `0x1C44` is the offset itself; the high 16
(`0x0004`) is a fixed adjustment applied to the decoded MOVW/MOVT immediate
to get the final `.data` SVA.

**Why this specific check even runs for SYSMEM** (confirmed via full
decompile of `SPLx_TZS_GETMODSVA`, not just inferred from behavior): this
isn't SYSMEM's `.text` resolution (that already succeeded - it's printed
as the correct, working `0x00200000`). `GETMODSVA` caches a `.text` SVA
and a `.data` SVA per module, and **the first time it's ever called for a
given module, it unconditionally also tries to resolve `.data`, regardless
of which one the caller actually asked for.** `SPLx_TZS_INIT` calls
`GETMODSVA(SPLTZ_MODINFO_SYSMEM, ...)` to get SYSMEM's `.text` (needed for
its own, unrelated `EXEC_WITH_SP` bootstrap step) - and that first-ever
call drags in the mandatory `.data` resolution as a side effect, which is
what actually fails here. The formula for `.data` is: decode the MOVW/MOVT
pair at the table-given offset, then subtract the table entry's high-16
adjustment. This was validated structurally against INTRMGR's (module 0)
own already-successful resolution in the same boot log: its table entry
is offset `0x5C`, adjustment `0x0000`, and INTRMGR's `.data` is
independently known from the log (`TZSGMS: m0 .text: 0x0054E000 | .data:
0x00570000`) - so the MOVW/MOVT pair at INTRMGR's `.text+0x5C` must decode
to exactly `0x00570000` with zero further adjustment, which is consistent
with the formula being right.

## What we did (all read-only, zero secure-world mutation)

`main.c`'s `kprintf_capture_handler()` registers as the system kprintf
handler only for the duration of `SPLx_CFK_INIT()`. On detecting the
`"!movp"` failure message, it uses `va_copy` to pull `text_base`/`offset`
straight out of the *same* va_list psp2spl is about to log, and calls
`SPLx_TZS_SVA2NSVA` directly (never `GETMODSVA`, which would recurse into
the same failing resolution) to dump a window of SYSMEM's real `.text`
while root state is still valid - i.e. before `SPLx_INIT`'s own unwind
zeroes `*spl_root` on failure. This is safe to repeat; it never writes
anything back and never executes anything in secure world.

Two capture rounds, both in `captures/`:

1. `boot_log_1_narrow_window_4kb.log` - window centered on the failing
   offset (text 0x1440-0x2440, i.e. the original ±0x800 widened from an
   earlier failed ±0x80 attempt).
2. `boot_log_2_wide_window_8kb.log` - fixed window from `.text` base
   through +0x2000 (text 0x0-0x2000), taken after widening the capture
   in `main.c` to cover ground the first window couldn't reach.
3. `boot_log_3_data_region_dump.log` / `boot_log_4_second_reboot.log` - an
   attempt to point the same window directly at `text+0x11000` (near the
   candidate `.data` address itself, `text+0x120E4`). See "SVA2NSVA
   doesn't reach that far" below - this did **not** work.

`scripts/reconstruct_dump.py` turns either log back into a flat binary
(`captures/sysmem_text_*.bin`) with correct byte order. The 0x1440-0x2000
overlap between the two captures is byte-identical (checked directly),
confirming SYSMEM's code here is deterministic across boots, as expected
for a fixed kernel module.

`scripts/scan_candidates.py` re-implements the exact OR-of-two-paths
check above and scans a dump for every offset that satisfies it, decoding
the `MOVW`/`MOVT` immediate pair (and flagging whether source/destination
register `Rd` matches between the two - a strong signal of a genuine
pair vs. coincidental byte noise).

## Findings

Scanning the full 8KB window (`sysmem_text_0x0-0x2000.bin`) turned up 18
same-`Rd` candidates. Six of them, at six *different, unrelated* code
offsets, decode to the exact same 32-bit value:

```
text+0x0A4A: R4  = 0x002120E4
text+0x0DD8: R7  = 0x002120E4
text+0x102A: R4  = 0x002120E4
text+0x10C6: R4  = 0x002120E4
text+0x1318: R4  = 0x002120E4
text+0x1488: R8  = 0x002120E4   <- the one we live-tested (see below)
```

Six independent compiler-emitted reloads of the identical constant is
strong corroborating evidence that `0x002120E4` is a real, frequently
referenced pointer in SYSMEM's compiled 3.60 code - much stronger
evidence than the original single-window search had (which only found
this one candidate "numerically plausible" among 4, with no
corroboration). The other candidates in the full scan decode to values
either in the `0x80024xxx`/`0x80020xxx` range (clustered near each other,
almost certainly something else entirely - return addresses or a jump
table, not a `.data` base) or to small values like `0x000DC080`/`0x000DC08C`
(too small to be a sensible absolute SVA for a module data segment).

**Revised understanding of the 2026-11-24 hang incident:** we previously
patched the table to point at `text+0x1488` and tested live; the device
hung on the enso boot logo and needed the user to pull the SD2VITA to
disable the plugin and recover. General lesson from that incident: don't
bypass a safety check to test a guess about the value it's gating, when
the check's whole job is to turn "this looks wrong" into a clean abort -
making the check *pass* isn't the same as making the value *correct*,
and that's exactly what happened here (see below).

At the time, the working theory was "the decoded address might just be
wrong - it matched the bit pattern, but that doesn't mean it's really
SYSMEM's `.data` base." The six-occurrence corroboration above makes that
theory much less likely: it is now hard to believe `0x002120E4` is *not*
a real, important constant in this module. The more likely explanation
for the hang is a **different, previously unexamined risk**: the value
is used as `stack_sva = data_sva + 0x200`, fed to `EXEC_WITH_SP` as the
secure-world stack pointer for `SPLx_TZS_INIT`'s own internal bootstrap
call (installing its `tzspl` primitives for SYSMEM - *not* anything our
plugin calls directly; see "Nothing we can fix from the plugin side"
below). That means it briefly pushes/pops a stack frame into live SYSMEM
`.data` - and SYSMEM is the kernel's own memory manager, actively read/
written by other cores and interrupt contexts essentially continuously.
psp2spl itself borrows *INTRMGR's* `.data` the same way for its own
internal init (visible in both boot logs, the `TZS3: xsmct:` /
`tzs_write32:` lines), and that's evidently safe - but that does not mean
borrowing *SYSMEM's* `.data` the same way is equally safe. SYSMEM's
allocator state may simply be more sensitive to being transiently
clobbered, especially this early in boot.

### The previously-tested patch almost certainly used the wrong adjustment too

Separately from whether the hang is really about *using* `.data` as a
scratch stack: the patch we tested changed the table's offset field
(`0x1C44` → `0x1488`) but left its adjustment field unchanged at `0x0004`.
That adjustment was derived by whoever built this table *for the original,
wrong offset* - there's no reason it transfers to a different offset found
independently. Applying the formula properly (decode MOVW/MOVT at
`0x1488`, subtract the *correct* adjustment) is more likely to need
adjustment `0x0000` (by analogy with INTRMGR's entry, where adjustment is
also `0x0000`), giving `data_sva = 0x002120E4` directly rather than the
`0x002120E0` the tested patch actually produced. This was never isolated
before - it's a second, independent bug in the patch we already know
hung the device, on top of whatever the `.data`-as-stack risk turns out
to be. **Not verified, not worth live-testing on its own** - see below for
why a "more correct" offset doesn't actually address the real risk.

### SVA2NSVA doesn't reach that far

To settle whether `0x002120E4` is really `.data` (as opposed to, say, a
function pointer still inside `.text` - a real, if weaker, doubt raised by
comparing against a *different* firmware build's SYSMEM code-size, see
`psp2spl-actlr-audit` memory), the obvious next step seemed to be: point
this same read-only dump window directly at `text+0x11000` (near
`text+0x120E4`) instead of at the code sites that *reference* it, and see
whether what's actually there reads as code or as state.

That didn't work. Two independent reboots of a build verified
byte-for-byte identical to what was actually on-device, with the window
retargeted to `text+0x11000` (confirmed via disassembly: `win_start` really
is computed as `text_base + 0x11000` in the compiled code), came back
**byte-identical to the original `text+0x0` dump** - not an error, not
garbage, the exact same bytes relabeled. `SPLx_TZS_SVA2NSVA` has, up to
this investigation, only ever been called with one input across every
capture round: `text_base` itself (the dump loop does one translation and
then walks forward in already-translated, ordinary memory - it never calls
`SVA2NSVA` a second time for a different SVA). The moment we asked it to
translate something else, it silently aliased back to the one window it
had already proven out, rather than returning an error or genuinely new
content.

This means **this diagnostic technique cannot be used to directly inspect
`0x002120E4`'s actual content**, and the result of trying is not
informative about what's really there (it doesn't mean "there's code at
`0x120E4`" - it means SVA2NSVA didn't really translate that address at
all). Whether this is a bounds-checked region table silently falling back
on an out-of-range query, a small fixed-size secure/non-secure aliasing
window, or something else is unknown and would need its own
investigation (or SKGleba's own knowledge of `SVA2NSVA`'s real valid
range) before this exact technique could be pushed further. It is not
worth repeating with yet another offset without understanding that first.

Weighing the two data points against each other: the devkit code-size
comparison is cross-firmware (a different build entirely, where debug/
diagnostic code is often larger than retail) and was never more than
circumstantial; the six-way corroboration is same-firmware, same-build,
concrete, and about as strong as independent verification gets without a
direct read of the target address itself. On balance, `0x002120E4` being
SYSMEM's real `.data` base remains the better-supported conclusion - but
see below for why this matters less than it might seem.

### Nothing we can fix from the plugin side

Checked the vendored `third_party/psp2spl/include` headers directly:
`SPLx_TZS_INIT(void)` takes no arguments, and `EXEC_WITH_SP`
(`SPLTZ_SMC_EXEC_WITH_SP`) is an internal SMC index, not a separately
callable, parameterized API. The whole chain that resolves SYSMEM's
`.data` and borrows it as a scratch stack happens *inside* `SPLx_TZS_INIT`
as an unconditional, parameterless bootstrap step - our plugin calls it
once (via `SPLx_CFK_INIT()`) and has no way to supply our own scratch
memory, skip the SYSMEM primitives, or otherwise influence this from
outside the library. There is no plugin-side workaround to pursue further
here; any real fix has to change psp2spl's own source.

## Conclusion / recommended fix direction

**Finding a "more correct" offset is not, by itself, the fix.** All six
corroborating occurrences decode to the identical address, so pointing
the table at any of the other five instead of `0x1488` would produce the
same `data_sva` (modulo the adjustment-constant bug above) and, in all
likelihood, the same hang - there is no value in re-testing this live
with a different offset or a corrected adjustment alone. The actual risk
- briefly using a live, continuously-accessed kernel module's `.data` as
a secure-world stack - is independent of whether the borrowed address is
"correct."

The best fix available is a **design** fix, not an address fix, and it has
to happen in psp2spl itself: have `SPLx_TZS_INIT`'s internal
`EXEC_WITH_SP` bootstrap step use scratch memory it owns (or that the
caller supplies), the same way `map_own_code_rx()` already does for the
*code* half of a related problem (SKGleba's own suggestion - see the
comment above `map_own_code_rx` in `src/main.c`), rather than borrowing a
resolved module's `.data`. That sidesteps the live-memory risk entirely,
independent of whatever SYSMEM's real `.data` address turns out to be.

If that's not feasible for psp2spl's design (e.g. if `EXEC_WITH_SP`'s
secure-world side specifically requires an address inside an already-
mapped, resolved module rather than arbitrary memory), then a real fix
would need both: the corrected table entry (offset `0x1488`, most likely
adjustment `0x0000` → `data_sva = 0x002120E4`, **not independently
verified**), and confirmation from SKGleba (or psp2spl/firmware
documentation we don't have) that briefly borrowing SYSMEM's `.data` this
way is actually safe - something no amount of static analysis or read-only
memory dumping from here can establish on its own.

Given the real hang already produced by one live test, and that nothing
in this repo can safely probe `EXEC_WITH_SP`'s stack-sourcing risk further
without potentially repeating it, **the responsible next step is handing
this complete analysis to SKGleba** - who already offered to update
`tz_offsets.c` - rather than attempting another live test from here. This
writeup, the six-way corroboration, the off-by-4 adjustment concern, and
the `SVA2NSVA` range limitation are the supporting evidence for that
handoff.

## Layout

- `scripts/` - reusable capture/analysis/deploy scripts (text, tracked),
  including `deploy_and_test.py` (re-enables the plugin's self-disabled
  config.txt line, flashes, reboots, waits for the device) and
  `fetch_results.py` (pulls the boot log + config.txt back).
- `captures/` - boot logs, reconstructed binaries, and `ur0:tai/config.txt`
  snapshots from all four capture rounds (text tracked; `.bin` gitignored).
- `psp2spl_objs/` - `.o` members extracted from the vendored
  `third_party/psp2spl/lib/libspl.a` (via the toolchain's own
  `arm-vita-eabi-ar` - the host's BSD `ar` can't read the GNU-format
  archive) plus the raw `SPLTZl_OPS` table bytes (`ops.bin`) and IDA
  databases (gitignored).
- `tested-patches/` - the `0x1C44`→`0x1488` patched library variant that
  was flashed and live-tested (and caused the hang), kept for the
  record. **Do not reflash this without re-reading the Findings section
  above first - the hang is not well explained by "wrong offset," and
  nothing here has addressed the actual suspected cause.**
