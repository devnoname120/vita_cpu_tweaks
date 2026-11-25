#!/usr/bin/env python3
"""Check a plugin's imports against decrypted firmware modules.

For every firmware directory given, reads the export tables of all decrypted
modules under fs_dec/os0 (bootimage included) and reports, per import of the
plugin, whether that library NID and each function NID exist.

  tools/check_fw_imports.py build/cpu_tweaks.velf FW_DIR [FW_DIR...]

FW_DIR is e.g. .../decrypted/external/official/3.73 (needs fs_dec/os0).
taiHEN's own libraries (taihen*) are skipped: they are not firmware.
"""
import struct
import sys
from pathlib import Path


def segments(d):
    e_phoff = struct.unpack_from("<I", d, 0x1C)[0]
    phnum = struct.unpack_from("<H", d, 0x2C)[0]
    return [struct.unpack_from("<8I", d, e_phoff + i * 32) for i in range(phnum)]


def mapper(segs):
    def off(va):
        for p in segs:
            if p[0] == 1 and p[2] <= va < p[2] + max(p[4], p[5]) and va - p[2] < p[4]:
                return p[1] + va - p[2]
        raise ValueError(hex(va))
    return off


def module_info(d, segs):
    e_entry = struct.unpack_from("<I", d, 0x18)[0]
    seg = segs[e_entry >> 30]
    return seg, seg[1] + (e_entry & 0x3FFFFFFF)


def cstr(d, o):
    return d[o:d.index(b"\0", o)].decode(errors="replace")


def imports(path):
    d = Path(path).read_bytes()
    segs = segments(d)
    off = mapper(segs)
    seg, mi = module_info(d, segs)
    top, btm = struct.unpack_from("<II", d, mi + 0x2C)
    p, out = seg[1] + top, []
    while p < seg[1] + btm:
        size, _, _, nfunc = struct.unpack_from("<4H", d, p)
        if size == 0x34:
            lib_nid, name_va, _, fnid_va = struct.unpack_from("<4I", d, p + 0x10)
        else:
            lib_nid, name_va, fnid_va = struct.unpack_from("<3I", d, p + 0x0C)
        nids = [struct.unpack_from("<I", d, off(fnid_va) + 4 * i)[0] for i in range(nfunc)]
        out.append((cstr(d, off(name_va)), lib_nid, nids))
        p += size
    return out


def exports(path):
    """[(module, lib name, lib nid, set(nids))] for one decrypted ELF."""
    d = Path(path).read_bytes()
    if d[:4] != b"\x7fELF":
        return []
    segs = segments(d)
    off = mapper(segs)
    try:
        seg, mi = module_info(d, segs)
        mod = cstr(d, mi + 4)
        top, btm = struct.unpack_from("<II", d, mi + 0x24)
    except (ValueError, IndexError, struct.error):
        return []
    p, out = seg[1] + top, []
    while p < seg[1] + btm:
        size, _, _, nfunc, nvar = struct.unpack_from("<5H", d, p)
        lib_nid, name_va, nid_va = struct.unpack_from("<3I", d, p + 0x10)
        if size == 0:
            break
        try:
            name = cstr(d, off(name_va)) if name_va else "(main)"
            nids = {struct.unpack_from("<I", d, off(nid_va) + 4 * i)[0]
                    for i in range(nfunc + nvar)}
        except ValueError:
            name, nids = "?", set()
        out.append((mod, name, lib_nid, nids))
        p += size
    return out


def main():
    want = [i for i in imports(sys.argv[1]) if not i[0].startswith("taihen")]
    bad_any = False
    for fw in sys.argv[2:]:
        libs = {}
        for elf in sorted(Path(fw, "fs_dec", "os0").rglob("*.elf")):
            for mod, name, nid, nids in exports(elf):
                libs.setdefault(nid, (mod, name, set()))[2].update(nids)
        missing = []
        for name, nid, fnids in want:
            if nid not in libs:
                missing.append(f"{name} 0x{nid:08X}: library not exported")
                continue
            gone = [f"0x{n:08X}" for n in fnids if n not in libs[nid][2]]
            if gone:
                missing.append(f"{name} 0x{nid:08X}: missing " + " ".join(gone))
        print(f"{Path(fw).name}: " + ("all imports resolve" if not missing else ""))
        for m in missing:
            print("    " + m)
        bad_any |= bool(missing)
    sys.exit(1 if bad_any else 0)


if __name__ == "__main__":
    main()
