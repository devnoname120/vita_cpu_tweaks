# vita_cpu_tweaks — test build

Re-enables the PS Vita's Cortex-A9 (ACTLR) and PL310 hardware data
prefetchers, which Sony's `kernel_boot_loader` turns off at every boot.

```
build:   742b9f2
sha256:  42252f08c0dd5eab9c8804a7adda13d38579de48ac8aaae8ac652572f5565a65
```

**Verified on exactly one configuration: a retail PS Vita on firmware 3.60
via enso.** No other firmware, and no devkit/testkit/debug unit, has run
this. That is the entire point of you testing it — please don't assume it
works on yours.

## Requirements

- enso (or any taiHEN setup that loads `ur0:tai/config.txt`)
- Firmware **3.60–3.74**. This is a hard limit from psp2spl, the library
  that provides the secure-world access; outside that range it won't even
  try.

## Every test step switches itself off, on purpose

Each step below uses a `... = once` setting. The very first thing the
plugin does with it, before touching anything else, is rewrite that line in
its own `ur0:data/cpu_tweaks.ini` to `... = off`. So:

- Each step takes effect for **that boot only**.
- If a boot ever hangs, just **power-cycle**: the setting already says
  `off`, so the unit comes back normally. You don't need to pull the SD
  card.
- To run the next step, set its key to `once` and reboot. The plugin's line
  in `ur0:tai/config.txt` stays as it is.

It only ever edits its own `cpu_tweaks.ini` and writes logs. Nothing is
written to the boot chain, eMMC partitions, or flash. If it can't *confirm*
the line now says `off`, it does nothing at all that boot rather than
running without that safety net.

## Install

1. Copy `cpu_tweaks.skprx` to `ur0:tai/`.
2. Copy `cpu_tweaks.ini` to `ur0:data/`. It's INI-style so editors
   highlight it. Editing it on Windows is fine — LF, CRLF and bare CR line
   endings all work, and a UTF-8 BOM is ignored.
3. Add to `ur0:tai/config.txt`, under the `*KERNEL` section:

   ```
   *KERNEL
   ur0:tai/cpu_tweaks.skprx
   ```
4. Reboot.

## Please go up this ladder, one reboot at a time

`ur0:data/cpu_tweaks.ini` ships set to step 1. After each boot, read
`ur0:data/cpu_tweaks.log`, then set the next step's key and reboot.

1. **`probe = once`** (as shipped). Read-only: dumps your secure page-table
   layout and makes no secure writes whatsoever. This is the most useful
   step to send back — it says whether the rest would even work on your
   firmware. The log will be large; that's expected.
2. **`enable = once`**. Sets the PL310 fields with one secure MMIO write, and
   the ACTLR bit by injecting a page-table descriptor to get a
   secure-executable scratch page, running a 9-word stub on each core, then
   cleaning up after itself. Expect `actlr before 0x149` / `after 0x14B`
   four times, `actlr: requested bits APPLIED on all cores`,
   `prefetch_ctrl before 0x21000000` / `after 0x3100000F` and
   `pl310: requested fields APPLIED`.
3. **`benchmark = once`**. During boot, it raises the clocks to game levels
   (CPU 500 MHz), runs a memory benchmark across the prefetch settings for
   about 40 seconds - the screen stays on the boot logo meanwhile - puts
   the firmware's settings and clocks back, and reboots the console. Leave
   the Vita alone for two minutes until it is back on the home screen, then
   send `ur0:data/cpu_tweaks_bench.log`
   - it ends with `bench done`. This tells us whether the speed-up holds on
   your unit too. 500 MHz needs a clock plugin such as PSVshell; without
   one it runs at 444 and says so.

Every step is gated: if anything about your layout doesn't match what the
code expects, it logs a reason and aborts instead of guessing. An abort is
a useful result, not a failure — please send the log either way.

## What to send back

`ur0:data/cpu_tweaks.log`, plus your **firmware version** and **model**
(retail / devkit / testkit / debug, and PCH-1000 vs PCH-2000). The log is
plain text and contains no personal data.

Lines worth a quick look yourself:

- `once: config now says off for the next boot` — the safety net worked.
  If you instead see `once: switch FAILED, running NOTHING`, nothing risky
  ran at all.
- `cfk_init_ret 0x00000005` — secure-world access came up. A negative value
  means psp2spl couldn't initialise on your firmware, and nothing after it
  was attempted.
- `l1 ... aborting` / `l2 ... aborting` — a gate stopped it. The message
  says which one.

## Expected caveats

- Settings apply on **cold boot only**, not resume-from-sleep. Whether
  these bits survive sleep on this hardware is untested.
- The benefit is for strided or streaming memory access: on the 3.60 test
  unit at 500 MHz, 60-66% fewer cycles per load at 4-16 byte strides,
  about 50% at 32-64 bytes, falling to 17% at 512. Random pointer-chasing
  gains only ~7%. It is not a general speed-up, and it will not rescue a
  GPU-bound workload.

Source, full technical writeup and the record of every dead end:
<https://github.com/devnoname120/vita_cpu_tweaks>
