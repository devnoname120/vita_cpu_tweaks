#!/usr/bin/env bash
# Disassembles SYSMEM's exec_with_sp(arg, sp, func) trampoline (.text+0x60,
# Thumb) out of the captured SYSMEM .text dump, to determine whether the sp
# and func it is handed are secure or non-secure addresses.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAP="$HERE/../captures"
BIN="sysmem_text_0x0-0x2000.bin"
docker run --rm -v "$CAP":/w -w /w mirrorsedge-vitasdk-base:gcc10 \
  arm-vita-eabi-objdump -D -b binary -m armv7 -M force-thumb \
    --adjust-vma=0x200000 --start-address=0x200058 --stop-address=0x2000c0 "$BIN"
