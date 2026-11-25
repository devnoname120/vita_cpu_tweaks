#!/usr/bin/env python3
"""List a .velf's imports: library name, library NID, function NIDs."""
import struct, sys
d = open(sys.argv[1], "rb").read()
e_entry, e_phoff = struct.unpack_from("<II", d, 0x18)
phnum = struct.unpack_from("<H", d, 0x2C)[0]
segs = [struct.unpack_from("<8I", d, e_phoff + i * 32) for i in range(phnum)]
def off(va):
    for p in segs:
        if p[0] == 1 and p[2] <= va < p[2] + p[5]:
            return p[1] + va - p[2]
    raise ValueError(hex(va))
seg0 = segs[e_entry >> 30]
mi = seg0[1] + (e_entry & 0x3FFFFFFF)
imp_top, imp_btm = struct.unpack_from("<II", d, mi + 0x2C)
base = seg0[2]
p = seg0[1] + imp_top
while p < seg0[1] + imp_btm:
    size, ver, attr, nfunc, nvar, ntls = struct.unpack_from("<6H", d, p)
    if size == 0x34:
        lib_nid, name_va, _, fnid_va = struct.unpack_from("<4I", d, p + 0x10)
    else:  # 0x24 layout
        lib_nid, name_va, fnid_va = struct.unpack_from("<3I", d, p + 0x0C)
    name = d[off(name_va):].split(b"\0")[0].decode()
    nids = [struct.unpack_from("<I", d, off(fnid_va) + 4 * i)[0] for i in range(nfunc)]
    print(f"{name} 0x{lib_nid:08X} " + " ".join(f"0x{n:08X}" for n in nids))
    p += size
