# vita_cpu_tweaks

A taiHEN kernel plugin that re-enables the PS Vita's Cortex-A9 and PL310
hardware prefetchers, which Sony's `kernel_boot_loader` explicitly disables
at every boot. Every prefetch-related register field gets its own key in a
plain INI-style config file — no rebuilding, no runtime tool needed.

```ini
enable = on                  # off | on | once
actlr.l2_prefetch_hint = on  # Cortex-A9 ACTLR bit 1
pl310.data_prefetch = on     # PL310 PREFETCH_CTRL bit 28
pl310.prefetch_offset = 15   # PL310 PREFETCH_CTRL bits 4:0
```

`enable = once` applies the settings for one boot: the plugin rewrites the
line to `enable = off` before doing anything, so if that boot ever hangs, a
plain power-cycle comes back without it. Plus `probe = once` for a
read-only diagnostic log and `benchmark = once` for an on-device benchmark
that reboots when it is done. See
[`cpu_tweaks.example.ini`](cpu_tweaks.example.ini) for every key,
each documented right above it.

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
`probe = once` (read-only), confirm the log looks sane, then `enable = once`,
and only switch to `enable = on` once a `once` boot has worked. `on` has no
safety net: if it ever hung, it would hang on every boot until the plugin's
line is removed from `config.txt`.

Waking from sleep: the secure world resets the cores and the L2 controller
on wake (verified: right after wake ACTLR reads `0x149` and PL310
`0x21000000` again), so the plugin re-applies the settings after every
wake, following psp2spl's own sleep/resume protocol (tear down before
sleep, re-initialise with `RESET` on wake). Verified on retail 3.60.

## Why bother

Measured on the retail unit itself with the plugin's built-in benchmark
(`benchmark = once`, `benchmark.timing = boot`): independent strided loads over
memory that is in no cache, CPU at 500 MHz with bus 222 / GPU 222 /
crossbar 166 MHz (the clocks games use), run during boot on a near-idle
system, three interleaved rounds. Two separate boots agree to a median of
0.05%. Cycles per load:

| stride (bytes) | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 |
|---|---|---|---|---|---|---|---|---|
| stock | 16.6 | 29.6 | 35.0 | 47.4 | 47.4 | 47.6 | 48.0 | 49.4 |
| ACTLR hint only | 9.9 | 18.0 | 21.1 | 37.0 | 37.5 | 38.9 | 40.7 | 44.4 |
| PL310 only, offset 15 | 7.2 | 11.0 | 14.1 | 20.7 | 20.4 | 27.7 | 36.1 | 44.9 |
| **hint + PL310 offset 15** | **6.6** | **10.0** | **14.0** | **22.7** | **23.3** | **27.7** | **36.6** | **41.0** |
| change vs stock | −60% | −66% | −60% | −52% | −51% | −42% | −24% | −17% |

Random (pointer-chasing) access over 2 MiB, which can't be prefetched, still
gains a little: 33.8 → 31.3 cycles per load (−7%).

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
- Cache hits are unchanged (L1 4 cycles, L2 ~21–25). Beyond that small
  random-access gain, this won't move the needle on pointer-chasing workloads (object graphs, VM dispatch,
  tree/BSP traversal) — it helps roughly sequential or strided access
  (streaming decode, copies, linear scans).

The full tables, the ~330 MHz runs and the method are in
`analysis/ebadmovp-investigation/README.md`.

## Installing

1. Copy `cpu_tweaks.skprx` (built below, or from a
   [release](../../releases)) to `ur0:tai/cpu_tweaks.skprx`.
2. Add a line for it under `*KERNEL` in `ur0:tai/config.txt`:
   ```
   ur0:tai/cpu_tweaks.skprx
   ```
3. Copy [`cpu_tweaks.example.ini`](cpu_tweaks.example.ini) to
   `ur0:tai/cpu_tweaks.ini`. It ships with `enable = on` and the
   measured-best register values filled in, so they apply at every boot.
   For a first boot with a safety net, set `enable = once` instead.
4. Reboot. `config.txt` is only read at boot, and this plugin only rereads
   its own config file at boot too — so any change to either file needs
   one more reboot to take effect.

**Go cautiously the first time:** start with everything off, confirm the
plugin loads and exits cleanly (check `ur0:data/cpu_tweaks.log` after
boot — it should say `nothing to do, exiting cleanly`), then
`probe = once`, then `enable = once`, *then* `enable = on`. Keep a way to get back into `config.txt` (recovery
menu, another kernel plugin, etc.) in case anything about your specific
unit/firmware doesn't match what this was built against.

To disable the plugin entirely, just comment out or remove its line in
`config.txt` — ordinary taiHEN usage.

## Building

Needs a Docker image providing vitasdk (the included `Dockerfile` pins one
with `arm-vita-eabi-gcc` 10.3.0, known-good against this code):

```bash
docker build -t vita_cpu_tweaks .
docker run --rm -v "$PWD":/work vita_cpu_tweaks
```

`build/cpu_tweaks.skprx` lands on your own filesystem via the volume
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
a stale TLB entry. A 9-word stub (read ACTLR, clear and set masks loaded
from the same page, write ACTLR) is then written with secure writes, read back through the new mapping as a gate, executed per core via
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
