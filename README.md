# vita_prefetch_enabler

A taiHEN kernel plugin that re-enables the PS Vita's Cortex-A9 and PL310
hardware prefetchers, which Sony's `kernel_boot_loader` explicitly disables
at every boot. Every prefetch-related register field gets its own key in a
plain INI-style config file — no rebuilding, no runtime tool needed.

```ini
mode = apply                 # off | apply | probe | bench
actlr.l2_prefetch_hint = on  # Cortex-A9 ACTLR bit 1
pl310.data_prefetch = on     # PL310 PREFETCH_CTRL bit 28
pl310.prefetch_offset = 15   # PL310 PREFETCH_CTRL bits 4:0
```

See [`prefetch_enabler.example.ini`](prefetch_enabler.example.ini) for all
13 keys, each documented right above it.

## ⚠️ Compatibility — read before flashing this

**Verified on exactly one configuration: a retail PS Vita on firmware 3.60
via enso** (2026-11-24). The example config applies there — ACTLR
`0x149 -> 0x14B` on all four cores, PL310 `PREFETCH_CTRL`
`0x21000000 -> 0x3100000F` — reproduced across many consecutive boots.
Nothing else has been tried: no other retail firmware, and no devkit,
testkit or debug unit. The underlying register-level technique was
separately devkit-verified at firmware 3.65 (see "Why bother" below), but
by a *different* delivery mechanism that turned out not to port to retail;
see `analysis/ebadmovp-investigation/README.md` for what this does instead
and why.

Two things gate whether it works at all on a given unit/firmware:

1. **psp2spl's own firmware range.** Its secure-world init is gated to
   firmware `3.60`–`3.74`. Outside that range it won't even get as far as
   trying. This is the hard ceiling: a 3.65 or 3.74 unit is plausible, a
   3.75+ unit is not.
2. **The secure-world layout.** No address is hardcoded — the TZ region
   bounds and secure page-table roots are read out of psp2spl's own
   resolved layout, and the scratch page, stub slot and page-table write
   window are each found by walking the live secure tables at runtime. They
   have to be: they move between boots on a *single* unit, let alone between
   firmwares. Every step is gated, so a layout this doesn't fit aborts with
   a logged reason instead of doing something unpredictable.

Go cautiously on any configuration other than retail 3.60: start with
`mode = probe` (read-only), confirm the plugin loads and the log looks sane,
*then* switch to `mode = apply` and read `ur0:data/prefetch_enabler.log`
after the next boot.

Note also that the dead-man's switch (below) comments out the plugin's own
`config.txt` line on every boot, so by default it applies the settings once
and then disables itself. That is deliberate for testing. `self_disable = no`
keeps it enabled across boots, which removes that safety net: only set it
once `mode = apply` has booted cleanly on your unit.

This also only re-applies on a **cold boot** — not on resume-from-sleep.
Whether sleep/resume clears these bits on this hardware was never safely
testable (attempting it with no other recovery path risks wedging the
unit), so that's an open question, not a confirmed "it persists through
sleep."

## Why bother

Measured on the retail unit itself with the plugin's built-in benchmark
(`mode = bench`): independent strided loads over memory that is in no cache,
CPU at 500 MHz with bus 222 / crossbar 166 MHz (the clocks games use),
three interleaved rounds, best two of three agreeing to a median of 0.2%.
Cycles per load:

| stride (bytes) | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 |
|---|---|---|---|---|---|---|---|---|
| stock | 17.9 | 32.0 | 38.2 | 49.6 | 49.5 | 49.2 | 50.0 | 51.4 |
| ACTLR hint only | 10.3 | 19.0 | 21.9 | 37.9 | 38.3 | 39.9 | 41.9 | 46.2 |
| PL310 only, offset 15 | 7.3 | 11.5 | 15.3 | 21.6 | 21.4 | 28.2 | 36.7 | 44.9 |
| **hint + PL310 offset 15** | **6.7** | **10.3** | **15.4** | **22.9** | **24.3** | **28.3** | **37.2** | **41.4** |
| change vs stock | −63% | −68% | −60% | −54% | −51% | −42% | −26% | −19% |

Takeaways:
- **ACTLR bit 1 ("L2 prefetch hint") is the only ACTLR bit that matters.**
  Bit 2, the L1 data-side prefetcher, measured to do nothing at all on this
  chip, alone or combined; the example config leaves it alone.
- **The PL310 prefetch offset is what makes L2 prefetch pay.** At the
  firmware's offset 0 it adds nothing over the hint and slows large strides;
  at 15 it is the biggest single win. 7 is slightly better below 16-byte
  strides, 31 better above 128; 15 is the best all-rounder.
- ACTLR "allocate in one way only" and PL310 prefetch drop make no
  measurable difference either way.
- Cache hits are unchanged (L1 4 cycles, L2 ~21–25). This won't move the
  needle on pointer-chasing workloads (object graphs, VM dispatch,
  tree/BSP traversal) — it helps roughly sequential or strided access
  (streaming decode, copies, linear scans).

The full tables, the ~330 MHz runs and the method are in
`analysis/ebadmovp-investigation/README.md`.

## Installing

1. Copy `prefetch_enabler.skprx` (built below, or from a
   [release](../../releases)) to `ur0:tai/prefetch_enabler.skprx`.
2. Add a line for it under `*KERNEL` in `ur0:tai/config.txt`:
   ```
   ur0:tai/prefetch_enabler.skprx
   ```
3. Copy [`prefetch_enabler.example.ini`](prefetch_enabler.example.ini) to
   `ur0:data/prefetch_enabler.ini` and set `mode = apply` (it ships as
   `mode = off`, with the measured-best register values already filled in).
4. Reboot. `config.txt` is only read at boot, and this plugin only rereads
   its own config file at boot too — so any change to either file needs
   one more reboot to take effect.

**Go cautiously the first time:** start with `mode = off`, confirm the
plugin loads and exits cleanly (check `ur0:data/prefetch_enabler.log` after
boot — it should say `nothing to do, exiting cleanly`), then `mode = probe`,
*then* `mode = apply`. Keep a way to get back into `config.txt` (recovery
menu, another kernel plugin, etc.) in case anything about your specific
unit/firmware doesn't match what this was built against.

To disable the plugin entirely, just comment out or remove its line in
`config.txt` — ordinary taiHEN usage.

## Building

Needs a Docker image providing vitasdk (the included `Dockerfile` pins one
with `arm-vita-eabi-gcc` 10.3.0, known-good against this code):

```bash
docker build -t vita_prefetch_enabler .
docker run --rm -v "$PWD":/work vita_prefetch_enabler
```

`build/prefetch_enabler.skprx` lands on your own filesystem via the volume
mount. Rerun the `docker run` line after any source change — no need to
rebuild the image itself.

The build links against a vendored copy of psp2spl's `splib` (prebuilt
static libs + headers, see
[`third_party/psp2spl/NOTICE.md`](third_party/psp2spl/NOTICE.md)) so no
other setup is needed. If you'd rather build `splib` yourself from
[upstream](https://github.com/SKGleba/psp2spl) — it needs a MeP
cross-toolchain for the F00D coprocessor payload that isn't publicly
packaged anywhere, so this is a nontrivial undertaking on its own — point
`cmake -DPSP2SPL_DIR=/path/to/your/build` at a directory with the same
`include/` + `lib/{libspl.a,libsplCFK.a}` layout to override the vendored
one.

## How it works, briefly

Uses psp2spl for secure-world (TrustZone) access. The two prefetchers need
quite different amounts of machinery.

**PL310** is memory-mapped, not a cp15 register, so no code execution is
needed at all: one secure read-modify-write sets `PREFETCH_CTRL` bit 28.
The controller is shared, so this happens once.

**ACTLR** is a cp15 register, and only an `MCR` executing in secure PL1 can
write it — once per core. That needs a `func` pointer that is executable
under the *secure* page tables, and on retail 3.60 nothing non-secure can
produce one directly: no secure page is both writable and executable, the
DDR firewall silently drops non-secure writes aimed into the TrustZone
region, and it also blocks secure accesses pointed *out* of that region, so
the module's own memory cannot be used as the stub's home either. What does
work is to give secure world a new mapping instead of new contents: one
secure write adds a single L2 descriptor mapping a previously-unmapped
secure VA to an unused physical page inside the TZ region, as Normal
Non-Cacheable, PL1-RW and executable. Non-cacheable is load-bearing — it
removes every cache-maintenance concern, including PL310's
security-tagged lines — and a VA that was invalid before cannot be holding
a stale TLB entry. The 4-instruction stub is then written with secure
writes, read back through the new mapping as a gate, executed per core via
psp2spl's `EXEC_WITH_SP`, and the scratch page and descriptor are both
cleaned up afterwards.

All of this runs fresh on every cold boot, driven by whatever the config
file says that boot, and every address involved is derived from the live
secure page tables at runtime. The full derivation, including the dead ends
and how each wall was established, is in
`analysis/ebadmovp-investigation/README.md`.

## Credits

None of this would have been possible without **[@SKGleba](https://github.com/SKGleba)**'s
research into the Vita's lv0/TrustZone internals and the
**[psp2spl](https://github.com/SKGleba/psp2spl)** library built on top of
it — the secure-world access this plugin relies on entirely. His suggestion
to execute out of our own memory rather than inject into another module's
is what led to the current mapping-injection approach, after the firewall
ruled out the literal version of it. Huge thanks for publishing both and
for the help.
