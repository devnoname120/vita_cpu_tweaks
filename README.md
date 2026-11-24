# vita_prefetch_enabler

A taiHEN kernel plugin that re-enables the PS Vita's Cortex-A9 and PL310
hardware prefetchers, which Sony's `kernel_boot_loader` explicitly disables
at every boot. Toggle each one independently with a plain text config file
— no rebuilding, no runtime tool needed.

```
# l1_prefetch: Cortex-A9 ACTLR L1 D-side prefetch + L2 prefetch hint (bits 2:1)
# l2_prefetch: ARM PL310 L2C-310 data prefetch (PREFETCH_CTRL bit 28) -- on/off
l1_prefetch=on
l2_prefetch=off
```

## ⚠️ Compatibility — read before flashing this

**Verified on exactly one configuration: a retail PS Vita on firmware 3.60
via enso** (2026-11-24). Both prefetchers come on there — ACTLR
`0x149 -> 0x14F` on all four cores, PL310 `PREFETCH_CTRL`
`0x21000000 -> 0x31000000` — reproduced across several consecutive boots.
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

Go cautiously on any configuration other than retail 3.60: start with both
config keys `off`, confirm the plugin loads and exits cleanly, *then* flip
one on and read `ur0:data/prefetch_enabler.log` after the next boot.

Note also that the dead-man's switch (below) comments out the plugin's own
`config.txt` line on every boot, so as shipped it applies the settings once
and then disables itself. That is deliberate for testing; using it as a
permanent enabler means removing that safety net.

This also only re-applies on a **cold boot** — not on resume-from-sleep.
Whether sleep/resume clears these bits on this hardware was never safely
testable (attempting it with no other recovery path risks wedging the
unit), so that's an open question, not a confirmed "it persists through
sleep."

## Why bother

A microbenchmark (fixed-stride pointer walk over a 4 MiB buffer, well past
L2 size) across off / ACTLR-only / PL310-only / both-on:

| stride | ACTLR-only | PL310-only | both on |
|---|---|---|---|
| 8–16 B | **+19% to +43%** | +19% to +38% | +19% to +38% |
| 32–64 B | flat to slightly positive | flat to slightly negative | flat |
| 128–512 B | flat | **−11% to −18%** | −4% to −12% |

Takeaways: the win is real but narrow (tight 8–16 byte strides), the L1
prefetcher (ACTLR) does nearly all of the work and never regresses badly,
and the L2 prefetcher (PL310) **alone** actively hurts once the access
pattern is coarser than a line or two. If you only want one, `l1_prefetch`
is the better default. This won't move the needle on pointer-chasing
workloads (object graphs, VM dispatch, tree/BSP traversal) — it helps
roughly-sequential/strided access (streaming decode, copies, linear scans).

## Installing

1. Copy `prefetch_enabler.skprx` (built below, or from a
   [release](../../releases)) to `ur0:tai/prefetch_enabler.skprx`.
2. Add a line for it under `*KERNEL` in `ur0:tai/config.txt`:
   ```
   ur0:tai/prefetch_enabler.skprx
   ```
3. Create `ur0:data/prefetch_enabler.txt` (copy
   [`prefetch_enabler.txt.example`](prefetch_enabler.txt.example)) with
   whichever of `l1_prefetch`/`l2_prefetch` you want `on`.
4. Reboot. `config.txt` is only read at boot, and this plugin only rereads
   its own config file at boot too — so any change to either file needs
   one more reboot to take effect.

**Go cautiously the first time:** start with both keys `off`, confirm the
plugin loads and exits cleanly (check `ur0:data/prefetch_enabler.log` after
boot — it should say `nothing to enable, exiting cleanly`), *then* flip one
on and reboot again. Keep a way to get back into `config.txt` (recovery
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
