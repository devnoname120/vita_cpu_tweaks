Draft message for SKGleba re: SPL_TZSGMSV_EBADMOVP on retail 3.60 (enso)

---

Hitting `SPL_TZSGMSV_EBADMOVP` (-36) on a retail PCH-1000, firmware 3.60
via enso, during `SPLx_TZS_INIT`'s own eager resolution of SYSMEM (module
1) - not anything our plugin calls directly. Dug into it with a read-only
diagnostic hook (kprintf capture around `SPLx_CFK_INIT()`, dumping memory
via `SPLx_TZS_SVA2NSVA` before root state gets zeroed on the failure
unwind) and wanted to share what we found, since it's more than "wrong
offset."

**What's actually failing:** `GETMODSVA` caches a `.text` and a `.data`
SVA per module, and the first-ever call for a module unconditionally also
resolves `.data` as a side effect, regardless of which one the caller
wanted. `SPLx_TZS_INIT` only needs SYSMEM's `.text` for its own
`EXEC_WITH_SP` bootstrap, but that first call drags in `.data` resolution
too, and `tz_offsets.c`'s table entry for that (subtype-2, byte offset
0x44: raw value `0x00041C44`) points at `.text+0x1C44` on this firmware,
which isn't a MOVW/MOVT pair.

**Candidate we found:** scanned a full 8KB dump of SYSMEM's `.text`
(`0x0`-`0x2000`) for anything matching the same MOVW/MOVT check psp2spl
itself uses. Six independent, unrelated code offsets all decode to the
identical constant:

```
text+0x0A4A: R4 = 0x002120E4
text+0x0DD8: R7 = 0x002120E4
text+0x102A: R4 = 0x002120E4
text+0x10C6: R4 = 0x002120E4
text+0x1318: R4 = 0x002120E4
text+0x1488: R8 = 0x002120E4
```

Six compiler-emitted reloads of the same value is about as strong as
corroboration gets without a ground-truth disassembly - this is very
likely SYSMEM's real `.data` base on 3.60.

**We tested `text+0x1488` live and it hung the device** on the enso boot
logo (recovered by pulling the SD2VITA and disabling the plugin - no
flash/boot-area state was touched, so it was recoverable, just not
cleanly). Given how strong the corroboration above is, we don't think the
hang was really "wrong address." Two things we think actually happened:

1. We left the table's adjustment constant unchanged (`0x0004`) when we
   only changed the offset. That constant was derived for the *old*,
   wrong offset - there's no reason it transfers to a new one found
   independently. Cross-checking the decode formula against INTRMGR's
   (module 0) own already-successful resolution in the same boot log
   (table entry offset `0x5C`, adjustment `0x0000`, known `.data` =
   `0x00570000`, which only round-trips cleanly with zero adjustment)
   suggests the right adjustment for SYSMEM's entry is probably `0x0000`
   too, giving `0x002120E4` directly - not the `0x002120E0` our tested
   patch actually produced.

2. More importantly: whatever the resolved `.data` SVA is, it gets used
   as `stack_sva = data_sva + 0x200` and fed to `EXEC_WITH_SP` as the
   secure-world stack for `SPLx_TZS_INIT`'s own primitive-install call.
   That's a transient push/pop into *live* SYSMEM `.data` - the kernel's
   own memory-allocator state, plausibly touched by other cores/
   interrupts continuously, this early in boot. Borrowing INTRMGR's
   `.data` the same way (visible in the same boot log) is evidently fine,
   but that doesn't mean SYSMEM's is equally safe to transiently clobber.
   We think this is the more likely cause of the hang, independent of
   whether the borrowed address is "correct."

We also tried to directly inspect `0x002120E4`'s actual contents (rather
than just the code referencing it) by pointing our diagnostic dump at
`text+0x11000` directly. That didn't work: `SPLx_TZS_SVA2NSVA` doesn't
seem to actually translate an SVA it hasn't already resolved - we got the
exact same bytes back as our `text+0x0` dump, twice, across independent
reboots, with a build we verified was really what's on-device. So we
can't get an independent read of that address with the tools we have; if
you know `SVA2NSVA`'s real valid range/behavior for an arbitrary SVA,
that'd help us understand what happened there, though it's a side
question compared to the above.

**What we'd suggest:** rather than just correcting the table entry, could
`SPLx_TZS_INIT`'s internal `EXEC_WITH_SP` bootstrap use scratch memory it
owns (or that's supplied by the caller) instead of borrowing a resolved
module's `.data`? That's the same fix you suggested for the *code* half of
a similar problem we had (`va2pa` our own code + map it RX, instead of
hunting for unreferenced padding in SYSMEM's `.text`) - it would sidestep
the live-memory risk entirely, independent of whatever SYSMEM's real
`.data` address turns out to be. If that's not feasible given how
`EXEC_WITH_SP`'s secure side works, we're happy to try the corrected table
entry instead (offset `0x1488`, adjustment `0x0000` -> `0x002120E4`), but
we'd want your read on whether briefly borrowing SYSMEM's `.data` this way
is actually known-safe first - we don't have the visibility into
SceSysmem's internals to judge that ourselves, and we'd rather not repeat
the hang.

Happy to share the full diagnostic dumps/logs if useful.
