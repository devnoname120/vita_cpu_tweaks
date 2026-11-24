# vita_cpu_tweaks

A taiHEN kernel plugin that turns on the PS Vita's hardware data
prefetchers. The firmware switches them off at every boot: the Cortex-A9's
L2 prefetch hint and the PL310 L2 cache controller's data prefetch. With
them on, memory reads that miss the cache get up to 2.5× faster. Every
prefetch-related register field has its own key in a plain INI config file,
so you never need to rebuild anything.

```ini
enable = on                  # off | on | once
actlr.l2_prefetch_hint = on  # Cortex-A9 ACTLR bit 1
pl310.data_prefetch = on     # PL310 PREFETCH_CTRL bit 28
pl310.prefetch_offset = 15   # PL310 PREFETCH_CTRL bits 4:0
```

## Performance

These numbers come from the plugin's own benchmark (`benchmark = once`) on a
retail Vita. The CPU ran at 500 MHz, with bus 222, GPU 222 and GPU crossbar
166 MHz. The benchmark runs during boot, on an otherwise idle system, as
three interleaved rounds. Two separate boots agree to a median of 0.05%.

**Strided loads over memory that is in no cache**, cycles per load:

| stride (bytes) | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 |
|---|---|---|---|---|---|---|---|---|
| stock | 16.6 | 29.6 | 35.0 | 47.4 | 47.4 | 47.6 | 48.0 | 49.4 |
| ACTLR hint only | 9.9 | 18.0 | 21.1 | 37.0 | 37.5 | 38.9 | 40.7 | 44.4 |
| PL310 prefetch only, offset 15 | 7.2 | 11.0 | 14.1 | 20.7 | 20.4 | 27.7 | 36.1 | 44.9 |
| **both, offset 15 (default config)** | **6.6** | **10.0** | **14.0** | **22.7** | **23.3** | **27.7** | **36.6** | **41.0** |
| change vs stock | −60% | −66% | −60% | −52% | −51% | −42% | −24% | −17% |

**Choosing the PL310 prefetch offset**: same test, ACTLR hint on, PL310
data prefetch on:

| offset | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 |
|---|---|---|---|---|---|---|---|---|
| 0 | 9.9 | 18.1 | 21.4 | 36.0 | 41.1 | 42.1 | 43.5 | 46.3 |
| 7 | **6.4** | **9.4** | **13.4** | **22.2** | 26.9 | 35.7 | 38.0 | 46.0 |
| **15** | 6.6 | 10.0 | 14.0 | 22.7 | **23.3** | 27.7 | 36.6 | 41.0 |
| 31 | 7.1 | 11.1 | 15.0 | 24.7 | 24.9 | **25.2** | **30.0** | **38.5** |

**Random (pointer-chasing) access** can't be predicted, but it still gains a
little. Over a 2 MiB working set, cycles per load drop from 33.8 (stock) to
31.3 (default config), or 29.8 with offset 0.

What this means:

- **ACTLR bit 1 (L2 prefetch hint) is the only ACTLR bit that matters.**
  Bit 2, the L1 data-side prefetcher, makes no difference on this chip,
  alone or combined with the others.
- **The prefetch offset is what makes PL310 prefetch pay off.** At offset 0
  it adds nothing over the hint and slows large strides. 15 is the best
  all-rounder: 7 is slightly better for strides under 64 bytes, and 31 is
  better for 128 bytes and up.
- ACTLR "allocate in one way only" and PL310 "prefetch drop" make no
  measurable difference either way.
- Cache hits are unchanged (L1 4 cycles, L2 about 21–25). The gain is for
  roughly sequential or strided access: streaming decode, copies, linear
  scans. Pointer-heavy work like object graphs, VM dispatch or tree
  traversal only sees the small random-access gain.

## Compatibility

**Tested on a retail PS Vita on firmware 3.60 with enso.** The default config
changes ACTLR from `0x149` to `0x14B` on all four cores and PL310
`PREFETCH_CTRL` from `0x21000000` to `0x3100000F`. This holds across boots
and across sleep/wake.

- **Firmware 3.60–3.74 only.** The plugin relies on
  [psp2spl](https://github.com/SKGleba/psp2spl) for secure-world (TrustZone)
  access, and psp2spl only supports that range. Other firmwares inside the
  range are untested.
- **No hardcoded addresses.** Every secure-world address is read from the
  live secure page tables at boot, and every step is checked. If something
  doesn't match what the plugin expects, it aborts that step and logs the
  reason instead of guessing.
- **Sleep/wake.** Waking from sleep resets the cores and the L2 controller
  to the firmware's values. The plugin re-applies its settings after every
  wake.

On anything other than retail 3.60, start with `enable = once` (see below)
before switching to `enable = on`.

## Installing

1. Copy `cpu_tweaks.skprx` ([built as below](#building)) to
   `ur0:tai/cpu_tweaks.skprx`.
2. Add it under `*KERNEL` in `ur0:tai/config.txt`:
   ```
   ur0:tai/cpu_tweaks.skprx
   ```
3. Copy [`cpu_tweaks.ini`](cpu_tweaks.ini) to `ur0:tai/`, next to the plugin.
   It ships with `enable = on` and the best
   measured values from the tables above.
4. Reboot. The plugin reads its config only at boot, so every config change
   needs a reboot to take effect.

To uninstall, remove the plugin's line from `config.txt` and reboot.

## Configuration

`ur0:tai/cpu_tweaks.ini` holds one `key = value` per line, and `#` starts a
comment. Every key is documented right above it in
[`cpu_tweaks.ini`](cpu_tweaks.ini).

| key | values | what it does |
|---|---|---|
| `enable` | `off` · `on` · `once` | Apply the register settings at boot. `once` applies them for the next boot only, then switches itself back to `off`. |
| `probe` | `off` · `once` | Log the CPU and PL310 IDs and the secure page-table layout. Read-only. |
| `benchmark` | `off` · `once` | Run the benchmark at the next boot, then reboot. |
| `log` | `on` · `off` | `off`: write the log only when something fails. |
| `actlr.*`, `pl310.*` | `on` · `off` · `keep` | One key per register field. `keep`, or leaving the key out, leaves the firmware's value alone. |
| `pl310.prefetch_offset` | `0`–`7`, `15`, `23`, `31`, `keep` | How many cache lines ahead the L2 prefetches. |
| `benchmark.clocks` | `max` · `keep` | Raise all clocks to their maximum for the benchmark. |
| `benchmark.timing` | `boot` · `home` | Run the benchmark during boot, or 30 s after boot. |

Two settings are accepted but marked **do not use** in the shipped config,
because they trigger known hardware bugs in this PL310 revision (r3p1-50rel0):

- `pl310.prefetch_offset = 23`: ARM erratum 765569. The prefetcher can cross
  a 4 KB page boundary.
- `pl310.double_linefill = on`: ARM erratum 752271. Can corrupt data.

### `once` is a safety net

Before it touches any register, the plugin rewrites every `once` in its
config file to `off` and reads the file back to confirm. If a `once` boot
ever hangs, a plain power-cycle comes back without the setting. If the
plugin can't confirm the rewrite, it does nothing that boot.

`enable = on` has no such safety net: a setting that hung the system would
hang it on every boot until the plugin's line is removed from `config.txt`.
That is why `once` is the right first step on an untested unit.

### Logs

- `ur0:data/cpu_tweaks.log` is rewritten at every boot. It shows the
  register values before and after, or why a step was skipped.
- `ur0:data/cpu_tweaks_bench.log` holds the results of `benchmark = once`.

## Running the benchmark yourself

Set `benchmark = once` and reboot. With `benchmark.timing = boot` (the
default), the boot pauses for about 35 seconds before the home screen
while the benchmark runs. The plugin then restores the clocks and registers
and reboots by itself. The results are in `ur0:data/cpu_tweaks_bench.log`.
The benchmark runs whatever `enable` is set to, and it ignores the register
keys: it tests its own fixed set of combinations, the ones in the tables
above.

## Building

You need Docker. The included `Dockerfile` pins a vitasdk image with
`arm-vita-eabi-gcc` 10.3.0:

```bash
docker build -t vita_cpu_tweaks .
```

```bash
docker run --rm -v "$PWD":/work vita_cpu_tweaks
```

The plugin ends up in `build/cpu_tweaks.skprx`. After a source change, rerun
only the `docker run` line.

The build links against a vendored copy of psp2spl's `splib` (prebuilt
static libs and headers, see
[`third_party/psp2spl/NOTICE.md`](third_party/psp2spl/NOTICE.md)), so it
needs no other setup. Building `splib` yourself from
[upstream](https://github.com/SKGleba/psp2spl) needs a MeP cross-toolchain
for the F00D payload, which isn't publicly packaged. To use your own build
anyway, pass `cmake -DPSP2SPL_DIR=/path/to/build`, pointing at a directory
with `include/` and `lib/{libspl.a,libsplCFK.a}`.

Host-side tests for the config parser are in `src/tests/run.sh`.

## How it works

Both registers can only be written from the secure world, which the plugin
reaches through psp2spl.

**PL310** is memory-mapped, so a single secure read-modify-write of
`PREFETCH_CTRL` (`0x1A002F60`) is enough. The controller is shared by all
cores, so this happens once.

**ACTLR** is a cp15 register. Only an `MCR` instruction running in secure PL1
can write it, and it has to run on each core. No secure page is both
writable and executable, and the DDR firewall keeps the secure world from
reaching the plugin's own non-secure memory. So the plugin gives the secure
world a new mapping:

1. It picks an unused physical page inside the TrustZone region and an
   unmapped secure virtual address.
2. One secure write adds a single L2 page-table descriptor that maps the
   page there as Normal non-cacheable, PL1 read-write and executable.
   Non-cacheable removes every cache-maintenance concern. A previously
   unmapped address can't have a stale TLB entry.
3. It writes a 9-word stub into the page (read ACTLR, apply the clear and
   set masks stored next to it, write ACTLR back) and reads it back
   through the new mapping to check it.
4. It runs the stub on each core with psp2spl's `EXEC_WITH_SP`.
5. It removes the stub and the descriptor.

All of this runs at every boot, using whatever the config file says that
boot. It runs again after every wake from sleep, following psp2spl's
sleep/resume protocol.

## Credits

This plugin depends entirely on **[@SKGleba](https://github.com/SKGleba)**'s
research into the Vita's lv0/TrustZone internals and on his
**[psp2spl](https://github.com/SKGleba/psp2spl)** library, which provides the
secure-world access. Thanks for publishing both, and for the help.
