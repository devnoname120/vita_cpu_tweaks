"""Parses a SPLx_TZS_PRINTTBR(1) dump out of a plugin boot log and answers the
question boot 8's hang raised: where can EXEC_WITH_SP's `func` legally point?

SYSMEM's trampoline does `mov sp, r1` / `blx r2` with no translation, under the
secure page tables, so `func` must be a secure VA that is mapped and not XN.
This reports:
  1. whether any secure mapping covers our own code page's PA (if so we could
     just pass that SVA and keep executing our own bytes),
  2. every executable (non-XN) secure mapping, and which of those are also
     writable - a RWX secure page would be the clean place to put the stub,
  3. the overall PA footprint, to see how much of DRAM secure world can see.

Usage: python3 analyze_tbr.py [boot_log] [own_code_pa_hex]
"""
import re
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, "..", "captures", "boot_log_9_printtbr.log")

SIZES = {"4K": 0x1000, "64K": 0x10000, "1M": 0x100000, "16M": 0x1000000}

# [PFE] I: L2_SMALL 0x00004000 [4K] -> 0x40125000 :  S C B, 0x1 :  XN, 101 : , 0xC
LINE = re.compile(
    r"(L1_L2TBL|L1_SECTn|L1_SUPER|L2_SMALL|L2_LARGE)\s+"
    r"0x([0-9A-Fa-f]{8})\s+\[(4K|64K|1M|16M)\]\s*[-<]>\s*"
    r"0x([0-9A-Fa-f]{8})\s*:(.*)$")


def main():
    own_pa = None
    entries = []   # (ttbr, type, sva, size, pa, xn, pxn, ap, flags)
    ttbr = None

    with open(LOG, "r", errors="replace") as f:
        for raw in f:
            m = re.search(r"probe own actlr_set pa 0x([0-9A-Fa-f]+)", raw)
            if m:
                own_pa = int(m.group(1), 16)
            if "=== TTBR0 ===" in raw:
                ttbr = 0
                continue
            if "=== TTBR1 ===" in raw:
                ttbr = 1
                continue
            m = LINE.search(raw)
            if not m:
                continue
            typ, sva_s, sz_s, pa_s, rest = m.groups()
            # L1_L2TBL lines point at a page table, not a mapping - skip.
            if typ == "L1_L2TBL":
                continue
            ap = ""
            apm = re.search(r",\s*([01]{3})\s*:", rest)
            if apm:
                ap = apm.group(1)
            entries.append({
                "ttbr": ttbr, "type": typ,
                "sva": int(sva_s, 16), "size": SIZES[sz_s],
                "pa": int(pa_s, 16),
                "xn": " XN" in rest, "pxn": "PXN" in rest,
                "ap": ap, "ns": " NS" in rest,
                "raw": raw.strip(),
            })

    if sys.argv[2:]:
        own_pa = int(sys.argv[2], 16)

    print(f"parsed {len(entries)} secure mappings from {os.path.basename(LOG)}")
    if own_pa is not None:
        print(f"our own code page PA: 0x{own_pa:08X} "
              f"(page 0x{own_pa & ~0xFFF:08X})")

    # --- 1. does anything map our own code page? ---
    print("\n=== 1. secure mapping covering our own code page? ===")
    hit = [e for e in entries
           if own_pa is not None and e["pa"] <= own_pa < e["pa"] + e["size"]]
    if hit:
        for e in hit:
            off = own_pa - e["pa"]
            print(f"  YES: sva 0x{e['sva'] + off:08X} "
                  f"(xn={e['xn']} ap={e['ap']}) <- {e['raw']}")
    else:
        print("  NO - secure world has no mapping of our code page's PA.")
        print("  => passing any alias of our own code as `func` can never work.")

    # --- 2. executable secure mappings ---
    execs = [e for e in entries if not e["xn"]]
    print(f"\n=== 2. executable (non-XN) secure mappings: {len(execs)} ===")
    rwx = [e for e in execs if e["ap"] == "001"]
    print(f"  of which writable (AP=001, PL1 RW): {len(rwx)}")
    for e in rwx[:40]:
        print(f"    RWX sva 0x{e['sva']:08X} [{e['size']//1024}K] "
              f"-> pa 0x{e['pa']:08X} ttbr{e['ttbr']} pxn={e['pxn']}")
    if not rwx:
        print("    (none - no secure page is both writable and executable)")
    ro_exec = [e for e in execs if e["ap"] != "001"]
    print(f"  read-only executable: {len(ro_exec)}")
    for e in ro_exec[:10]:
        print(f"    R-X sva 0x{e['sva']:08X} [{e['size']//1024}K] "
              f"-> pa 0x{e['pa']:08X} ap={e['ap']}")

    # --- 3. PA footprint ---
    print("\n=== 3. PA footprint of secure mappings ===")
    if entries:
        pas = sorted((e["pa"], e["pa"] + e["size"]) for e in entries)
        merged = [list(pas[0])]
        for lo, hi in pas[1:]:
            if lo <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], hi)
            else:
                merged.append([lo, hi])
        for lo, hi in merged:
            print(f"  0x{lo:08X} - 0x{hi:08X}  ({(hi - lo) // 1024} KiB)")

    # --- 4. writable mappings, for a stack ---
    wr = [e for e in entries if e["ap"] == "001"]
    print(f"\n=== 4. writable secure mappings (stack candidates): {len(wr)} ===")
    for e in wr[:5]:
        print(f"    RW  sva 0x{e['sva']:08X} [{e['size']//1024}K] "
              f"-> pa 0x{e['pa']:08X} xn={e['xn']}")


if __name__ == "__main__":
    main()
