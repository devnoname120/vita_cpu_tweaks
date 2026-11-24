# Vendored dependency: psp2spl (splib)

`include/` and `lib/libspl.a` / `lib/libsplCFK.a` in this directory are
prebuilt artifacts from [SKGleba/psp2spl](https://github.com/SKGleba/psp2spl)
(`dev` branch, commit `3c02358`), used here only for the secure-world
(TrustZone) code-execution primitive this plugin needs. See the top-level
README's "Credits" section for why this project would not exist without it.

They are vendored (rather than fetched/built at build time) because
building splib from source needs a MeP cross-toolchain for the F00D
coprocessor's lv0 payload that is not publicly packaged anywhere — a
separate, multi-day undertaking unrelated to this plugin. Vendoring the
build output keeps this repo's own Docker build self-contained.

Licensed MIT — `LICENSE.txt` in this directory is copied verbatim from
upstream. If you'd rather build splib yourself from source, see upstream's
own repo for instructions; point `PSP2SPL_DIR` at your own build output
(headers in `<dir>/include`, libs in `<dir>/lib`) to override the vendored
copy — see the top-level `CMakeLists.txt`.
