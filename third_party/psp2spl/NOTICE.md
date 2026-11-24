# Vendored dependency: psp2spl (splib)

`include/` and `lib/libspl.a` / `lib/libsplCFK.a` in this directory are
prebuilt artifacts from [SKGleba/psp2spl](https://github.com/SKGleba/psp2spl)
(`dev` branch, commit `3c02358`), **plus one local patch** (see
`patches/0001-sysmem-datamp-zero.patch`), used here only for the
secure-world (TrustZone) code-execution primitive this plugin needs. See
the top-level README's "Credits" section for why this project would not
exist without it.

**The patch**: `splib/arm/tzs_offsets.c`'s SYSMEM table entry hardcodes a
`.data`-resolution offset (`.text+0x1C44`) that isn't a real `MOVW`/`MOVT`
pair on retail firmware 3.60, which made `SPLx_TZS_INIT`'s own (`.text`-
only) SYSMEM lookup fail with `SPL_TZSGMSV_EBADMOVP` as a side effect —
every `GETMODSVA` call mandatorily also resolves `.data` on a module's
first lookup if its table entry is nonzero, regardless of which segment
the caller actually wanted. Confirmed via a full read of `spltz.c` that
nothing inside psp2spl itself ever calls `GETMODSVA(SYSMEM, 1, ...)` (the
`.data` segment) — every internal use is `.text`-only, for the
`EXEC_WITH_SP`/`MBALLOC`/`MBFREE` dispatch addresses. The patch sets that
field to `0`, the same sentinel the table's own `BUSERR` entry already
uses for an entry with no `.data` consumer, so the unneeded resolution -
and the firmware-specific offset it depended on - is skipped entirely
rather than corrected to a different (still firmware-specific, still
unverified against ground truth) guess. Full investigation:
`analysis/ebadmovp-investigation/README.md` in this repo.

They are vendored (rather than fetched/built at build time) because
building splib from source needs a MeP cross-toolchain for the F00D
coprocessor's lv0 payload that is not publicly packaged anywhere — a
separate, multi-day undertaking unrelated to this plugin. Vendoring the
build output keeps this repo's own Docker build self-contained. Rebuilding
with the patch applied requires that same toolchain; the patch file is
included so the exact change is reviewable and reproducible without it.

Licensed MIT — `LICENSE.txt` in this directory is copied verbatim from
upstream. If you'd rather build splib yourself from source, see upstream's
own repo for instructions; point `PSP2SPL_DIR` at your own build output
(headers in `<dir>/include`, libs in `<dir>/lib`) to override the vendored
copy — see the top-level `CMakeLists.txt`.
