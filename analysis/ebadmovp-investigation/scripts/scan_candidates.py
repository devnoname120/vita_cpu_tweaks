"""Scan a reconstructed SYSMEM .text dump for Thumb2 MOVW+MOVT pairs that
satisfy psp2spl's own SPL_TZSGMSV_EBADMOVP check (loose OR-of-two-paths
mask/byte check per half - see this directory's README for the derivation).

Usage: python3 scan_candidates.py <dump.bin> [base_text_off_hex] [text_base_hex]

base_text_off_hex: the .text-relative byte offset of dump.bin's byte 0
(what reconstruct_dump.py printed as base_text_offset). Default 0x0.
text_base_hex: SYSMEM's resolved .text SVA for this boot. Default 0x200000
(what we observed on retail 3.60/enso - TZSGMS printed "mod 1 .text(0x00200000+...)").
"""
import struct, sys

def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]

def half_lo(data, off):
    return u32(data, off) & 0xFFFF

def byte_hi(data, off):
    return (u32(data, off) >> 20) & 0xFF

def decode_movw_movt(data, off):
    w0 = u32(data, off); w1 = u32(data, off + 4)
    h0_first, h0_second = w0 & 0xFFFF, w0 >> 16
    h1_first, h1_second = w1 & 0xFFFF, w1 >> 16

    i0, imm4_0 = (h0_first >> 10) & 1, h0_first & 0xF
    imm3_0, rd0, imm8_0 = (h0_second >> 12) & 0x7, (h0_second >> 8) & 0xF, h0_second & 0xFF
    imm16_movw = (imm4_0 << 12) | (i0 << 11) | (imm3_0 << 8) | imm8_0

    i1, imm4_1 = (h1_first >> 10) & 1, h1_first & 0xF
    imm3_1, rd1, imm8_1 = (h1_second >> 12) & 0x7, (h1_second >> 8) & 0xF, h1_second & 0xFF
    imm16_movt = (imm4_1 << 12) | (i1 << 11) | (imm3_1 << 8) | imm8_1

    return rd0, imm16_movw, rd1, imm16_movt

def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <dump.bin> [base_text_off_hex] [text_base_hex]")
        sys.exit(1)
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    base_text_off = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x0
    text_base = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x200000

    candidates = []
    for off in range(0, len(data) - 8, 2):
        mask0 = (half_lo(data, off) & 0xFBF0) == 0xF240
        mask1 = (half_lo(data, off + 4) & 0xFBF0) == 0xF2C0
        byte0 = byte_hi(data, off) == 0x30
        byte1 = byte_hi(data, off + 4) == 0x34
        if not ((mask0 or byte0) and (mask1 or byte1)):
            continue

        text_off = base_text_off + off
        rd0, imm_w, rd1, imm_t = decode_movw_movt(data, off)
        addr = (imm_t << 16) | imm_w
        confidence = "mask+mask" if (mask0 and mask1) else (
            "mask+byte" if mask0 else ("byte+mask" if mask1 else "byte+byte"))
        candidates.append({
            "text_off": text_off, "sva": text_base + text_off,
            "rd0": rd0, "rd1": rd1, "same_rd": rd0 == rd1,
            "addr": addr, "confidence": confidence,
        })

    print(f"total raw matches (any OR path): {len(candidates)}")
    print()
    print("candidates with matching Rd between MOVW/MOVT (most plausible class):")
    for c in sorted(candidates, key=lambda c: c["text_off"]):
        if not c["same_rd"]:
            continue
        print(f"  text+0x{c['text_off']:04X} (sva 0x{c['sva']:06X}): "
              f"R{c['rd0']} = 0x{c['addr']:08X}  [{c['confidence']}]")

    print()
    print("candidates with MISMATCHED Rd (lower confidence, listed for completeness):")
    for c in sorted(candidates, key=lambda c: c["text_off"]):
        if c["same_rd"]:
            continue
        print(f"  text+0x{c['text_off']:04X} (sva 0x{c['sva']:06X}): "
              f"MOVW R{c['rd0']}=0x{c['addr'] & 0xFFFF:04X}.. MOVT R{c['rd1']}=..  [{c['confidence']}]")

if __name__ == "__main__":
    main()
