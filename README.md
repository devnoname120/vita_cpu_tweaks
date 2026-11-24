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

## Verified setup

Verified on a PS Vita devkit running firmware 3.65 with Ensō EX and
[psp2spl](https://github.com/SKGleba/psp2spl). The plugin applies its settings
at cold boot.

## Illustrative benchmark

This synthetic test walks a 4 MiB buffer at fixed strides. Each result is
relative to running with both prefetchers off.

| stride | ACTLR-only | PL310-only | both on |
|---|---|---|---|
| 8–16 B | **+19% to +43%** | +19% to +38% | +19% to +38% |
| 32–64 B | flat to slightly positive | flat to slightly negative | flat |
| 128–512 B | flat | **−11% to −18%** | −4% to −12% |

These results illustrate how prefetching behaves in this particular test.
More testing in real games and homebrew is needed to measure its effect on
overall performance.

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

To disable the plugin, comment out or remove its line in `config.txt`.

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
