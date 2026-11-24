# SPL_TZSGMSV_EBADMOVP on retail 3.60 (enso)

Investigation into why `SPLx_CFK_INIT()` fails with `SPL_TZSGMSV_EBADMOVP`
(-36) during `SPLx_TZS_INIT`'s own eager resolution of SYSMEM (module 1),
on the retail PCH-1000 test unit, firmware 3.60 via enso. See
[[psp2spl-actlr-audit]] and [[feedback-security-check-bypass-risk]] in the
assistant's memory for the earlier, higher-level narrative; this directory
holds the actual data and scripts.

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
(`0x0004`) looks like a fixed adjustment applied to the decoded MOVW/MOVT
immediate to get the final `.data` SVA (not yet confirmed against a
second, correctly-decoding example - we only have the one firmware's
table entries extracted).

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
disable the plugin and recover (see [[feedback-security-check-bypass-risk]]
for the fuller incident writeup and the general lesson about not
bypassing a safety check to test a guess).

At the time, the working theory was "the decoded address might just be
wrong - it matched the bit pattern, but that doesn't mean it's really
SYSMEM's `.data` base." The six-occurrence corroboration above makes that
theory much less likely: it is now hard to believe `0x002120E4` is *not*
a real, important constant in this module. The more likely explanation
for the hang is a **different, previously unexamined risk**: the value
is used as `stack_sva = data_sva + 0x200`, fed to `EXEC_WITH_SP` as the
secure-world stack pointer for our tiny ACTLR-setting stub. That means we
briefly push/pop a stack frame into live SYSMEM `.data` - and SYSMEM is
the kernel's own memory manager, actively read/written by other cores and
interrupt contexts essentially continuously. psp2spl itself borrows
*INTRMGR's* `.data` the same way for its own internal init (visible in
both boot logs, the `TZS3: xsmct:` / `tzs_write32:` lines), and that's
evidently safe - but that does not mean borrowing *SYSMEM's* `.data` the
same way is equally safe. SYSMEM's allocator state may simply be more
sensitive to being transiently clobbered, especially this early in boot.

**Conclusion: finding a "more correct" offset is very likely not the
actual fix.** All six corroborating occurrences decode to the identical
address, so pointing the table at any of the other five instead of
`0x1488` would produce the exact same `data_sva` and, in all likelihood,
the exact same hang - there is no value in re-testing this live with a
different offset. The open question is no longer "which offset" but
"how to get a safe scratch stack for `EXEC_WITH_SP` without borrowing a
live, security-critical module's `.data`" - e.g. carving one from our own
already-allocated memory the same way `map_own_code_rx()` already does
for the *code* (SKGleba's own suggestion for the code half of this
problem - see the comment above `map_own_code_rx` in `src/main.c`), if
that is possible for a stack too. That's a question for SKGleba, who
knows psp2spl's internals; nothing here should be live-tested again
without that figured out first, or some other independent way to confirm
a scratch-stack source is safe.

## Layout

- `scripts/` - reusable capture/analysis/deploy scripts (text, tracked).
- `captures/` - boot logs, reconstructed binaries, and `ur0:tai/config.txt`
  snapshots from the two capture rounds (text tracked; `.bin` gitignored).
- `psp2spl_objs/` - `.o` members extracted from the vendored
  `third_party/psp2spl/lib/libspl.a` (via the toolchain's own
  `arm-vita-eabi-ar` - the host's BSD `ar` can't read the GNU-format
  archive) plus the raw `SPLTZl_OPS` table bytes (`ops.bin`) and IDA
  databases (gitignored).
- `tested-patches/` - the `0x1C44`→`0x1488` patched library variant that
  was flashed and live-tested (and caused the hang), kept for the
  record. **Do not reflash this without re-reading the Findings section
  above and [[feedback-security-check-bypass-risk]] first.**
