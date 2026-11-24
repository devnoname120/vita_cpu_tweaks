# vita_prefetch_enabler — test build

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

## This plugin disables itself every boot, on purpose

The very first thing it does — before touching anything else — is comment
out its own line in `ur0:tai/config.txt`. So:

- Each run takes effect for **that boot only**, and the plugin is disabled
  for the next one.
- If a boot ever hangs, just **power-cycle**: the plugin is already
  disabled, so the unit comes back normally. You don't need to pull the SD
  card.
- To run it again, uncomment the line again.

It only ever edits `ur0:tai/config.txt` and writes a log. Nothing is
written to the boot chain, eMMC partitions, or flash. If it can't *confirm*
it disabled itself, it aborts before doing any of the risky work rather
than running without that safety net.

## Install

1. Copy `prefetch_enabler.skprx` to `ur0:tai/`. Keep the filename — the
   self-disable looks for a line containing `prefetch_enabler.skprx`.
2. Copy `prefetch_enabler.txt` to `ur0:data/`. Editing it on Windows is
   fine — LF, CRLF and bare CR line endings all work, and a UTF-8 BOM is
   ignored.
3. Add to `ur0:tai/config.txt`, under the `*KERNEL` section:

   ```
   *KERNEL
   ur0:tai/prefetch_enabler.skprx
   ```

   It must be `ur0:tai/config.txt` specifically — that's the only path the
   self-disable knows how to edit.
4. Reboot.

## Please go up this ladder, one reboot at a time

`ur0:data/prefetch_enabler.txt` ships set to step 1. After each boot, read
`ur0:data/prefetch_enabler.log`, then re-enable the config.txt line and
move to the next step.

1. **`probe_only=on`** (as shipped). Read-only: dumps your secure page-table
   layout and makes no secure writes whatsoever. This is the most useful
   step to send back — it says whether the rest would even work on your
   firmware. The log will be large; that's expected.
2. **`probe_only=off`, `l2_prefetch=on`**. One secure MMIO write, no code
   execution. Expect `l2_prefetch: PL310 data prefetch ENABLED` and
   `prefetch_ctrl before 0x21000000` / `after 0x3100000F` (data prefetch on,
   `l2_prefetch_offset=15`).
3. **also `l1_prefetch=on`**. The involved one: it injects a page-table
   descriptor to get a secure-executable scratch page, runs a 9-word stub on
   each core, then cleans up after itself. Expect
   `actlr: requested bits APPLIED on all cores` and
   `actlr before 0x149 / actlr after 0x14F` four times.
4. **`bench=on`** (the other keys are ignored in this mode). Boots normally;
   30 seconds after boot it runs a ~10 second memory benchmark across the
   prefetch settings, then puts the firmware's settings back. Leave the Vita
   on the home screen for a minute, then send the log - it ends with
   `bench done`. This tells us whether the speed-up holds on your unit too.

Every step is gated: if anything about your layout doesn't match what the
code expects, it logs a reason and aborts instead of guessing. An abort is
a useful result, not a failure — please send the log either way.

## What to send back

`ur0:data/prefetch_enabler.log`, plus your **firmware version** and **model**
(retail / devkit / testkit / debug, and PCH-1000 vs PCH-2000). The log is
plain text and contains no personal data.

Lines worth a quick look yourself:

- `selfdisable: own config.txt line commented out for next boot` — the
  safety net worked. If you instead see `selfdisable: FAILED`, nothing
  risky ran at all; the following line says why.
- `cfk_init_ret 0x00000005` — secure-world access came up. A negative value
  means psp2spl couldn't initialise on your firmware, and nothing after it
  was attempted.
- `l1 ... aborting` / `l2 ... aborting` — a gate stopped it. The message
  says which one.

## Expected caveats

- Settings apply on **cold boot only**, not resume-from-sleep. Whether
  these bits survive sleep on this hardware is untested.
- The measured benefit is narrow: roughly +19% to +43% on tight 8–16 byte
  strided access, and PL310 on its own can actually *hurt* at 128+ byte
  strides. It is not a general speed-up, and it will not rescue a
  GPU-bound or pointer-chasing workload.

Source, full technical writeup and the record of every dead end:
<https://github.com/devnoname120/vita_prefetch_enabler>
