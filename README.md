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

This was built and hardware-verified on **exactly one physical unit**: a
PS Vita devkit, **firmware 3.65**, with [psp2spl](https://github.com/SKGleba/psp2spl)'s
TrustZone access running there via enso_ex. That is the only configuration
this has ever actually run on.

Two separate things gate whether this will work at all on a different
unit/firmware:

1. **psp2spl's own firmware range.** Its secure-world init is gated to
   firmware `3.60`–`3.74`. Outside that range it won't even get as far as
   trying.
2. **The exact secure-world write target.** This plugin pokes a small set
   of stub bytes into an *unreferenced padding region* inside the SYSMEM
   module's resident secure code, found by statically dumping and
   disassembling that module on the one devkit above. Whether that same
   padding region exists at the same offset on a different firmware build
   of SYSMEM is **not verified** — psp2spl's firmware gate says TrustZone
   access should work across 3.60–3.74, but this plugin's own write target
   has only ever been checked on 3.65.

In other words: getting *into* the secure world is believed to work across
a firmware range; where this plugin then *writes* once it's there has only
been confirmed safe on one specific build. If the padding assumption is
wrong on your firmware, the write could land on live code instead of
padding. Recommended if you're on anything other than 3.65: dump and check
that region yourself first, or treat this as experimental on your unit and
watch a USB/UART console on the first boot after installing it.

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

Uses psp2spl to get secure-world (TrustZone) code execution, then writes a
tiny ARM32 stub (`MRC`/`ORR`/`MCR`/`BX` for ACTLR; a PL310 MMIO
read-modify-write for `PREFETCH_CTRL`) into an unreferenced padding region
inside SYSMEM's resident secure `.text`, flushes it to be I-fetchable, and
executes it via psp2spl's `EXEC_WITH_SP` secure-call gadget — once per core
for ACTLR, once for the shared PL310 controller. All of this runs fresh on
every cold boot, driven by whatever the config file says that boot.

## Credits

None of this would have been possible without **[@SKGleba](https://github.com/SKGleba)**'s
research into the Vita's lv0/TrustZone internals and the
**[psp2spl](https://github.com/SKGleba/psp2spl)** library built on top of
it — the secure-world code execution this plugin relies on entirely. Huge
thanks for publishing both.
