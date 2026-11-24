"""Reconstruct a flat binary from a kprintf_capture_handler memory dump log.

Usage: python3 reconstruct_dump.py <boot_log> <out.bin> [text_base_hex]

The log's dump lines look like "1440: AABBCCDD ..." where the label is a
.text-relative byte offset and each token is the 32-bit value a (volatile
unsigned int *) load returned at that address (i.e. already the correct
little-endian word). We re-emit each word as 4 little-endian bytes, which
reproduces the real Thumb/Thumb2 instruction byte stream.
"""
import re, struct, sys

def main():
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <boot_log> <out.bin> [text_base_hex]")
        sys.exit(1)
    log_path = sys.argv[1]
    out_path = sys.argv[2]
    text_base = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x200000

    words = {}  # text-relative byte offset -> 32-bit int as loaded
    line_re = re.compile(r'^([0-9A-Fa-f]+):((?:\s+[0-9A-Fa-f]{8})+)\s*$')

    with open(log_path) as f:
        for line in f:
            m = line_re.match(line.strip())
            if not m:
                continue
            base = int(m.group(1), 16)
            toks = m.group(2).split()
            for i, t in enumerate(toks):
                words[base + 4 * i] = int(t, 16)

    if not words:
        print("no dump lines parsed - check log format")
        sys.exit(1)

    lo, hi = min(words), max(words) + 4
    print(f"parsed {len(words)} words, range [0x{lo:X}, 0x{hi:X})")

    buf = bytearray(hi - lo)
    for off, val in words.items():
        buf[off - lo:off - lo + 4] = struct.pack('<I', val)

    with open(out_path, "wb") as f:
        f.write(buf)
    print(f"wrote {len(buf)} bytes to {out_path}")
    print(f"base_text_offset=0x{lo:X} (abs VA 0x{text_base + lo:X}, assuming text_base=0x{text_base:X})")

if __name__ == "__main__":
    main()
