# SPL_TZSGMSV_EBADMOVP on retail 3.60 (enso)

Investigation into why `SPLx_CFK_INIT()` fails with `SPL_TZSGMSV_EBADMOVP`
(-36) during `SPLx_TZS_INIT`'s own eager resolution of SYSMEM (module 1),
on the retail PCH-1000 test unit, firmware 3.60 via enso.

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
