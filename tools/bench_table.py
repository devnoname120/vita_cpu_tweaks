"""Turn the plugin's 'bench ...' log lines into per-access tables.

Usage: python3 tools/bench_table.py <cpu_tweaks.log>

Legs that appear in several rounds are merged, keeping each test's minimum.
Prints, per leg, cycles/access and ns/access for the chase (latency vs
working set) and the stride walk (independent loads over uncached memory),
plus the clock each leg ran at (cycles / microseconds).
"""
import re, sys

# Optional 2nd argument: the target clock in MHz. Tests measured more than 3%
# away from it are dropped (and listed) rather than mixed into the minimum.
TARGET = float(sys.argv[2]) if len(sys.argv) > 2 else 0
off_clock = []
legs, cur = [], None
for line in open(sys.argv[1], errors="replace"):
    line = line.strip()
    m = re.match(r"bench leg (\S+)$", line)
    if m and m.group(1) == "end":
        continue
    if m:
        # Legs repeat once per round; merge them by name, keeping the minimum.
        name = m.group(1)
        cur = next((L for L in legs if L["name"] == name), None)
        if cur is None:
            cur = {"name": name, "chase": {}, "stride": {}}
            legs.append(cur)
        continue
    m = re.match(r"bench (chase|stride) ([0-9A-F]+) cyc ([0-9A-F]+) us ([0-9A-F]+) n ([0-9A-F]+)", line)
    if m and cur is not None:
        kind, p, cyc, us, n = m.group(1), *(int(x, 16) for x in m.groups()[1:])
        mhz = cyc / us if us else 0
        if TARGET and abs(mhz - TARGET) > TARGET * 0.03:
            off_clock.append((cur["name"], kind, p, round(mhz)))
            continue        # not at the target clock: not comparable
        old = cur[kind].get(p)
        if old is None or (cyc and cyc < old[0]):
            cur[kind][p] = (cyc, us, n)

def fmt_size(b):
    return f"{b >> 20}M" if b >= 1 << 20 else f"{b >> 10}K" if b >= 1024 else f"{b}"

for kind, title in (("chase", "chase: cycles per dependent load, by working set"),
                    ("stride", "stride: cycles per load over uncached memory, by stride (bytes)")):
    keys = sorted({k for L in legs for k in L[kind]})
    print(title)
    print(f"{'leg':16}" + "".join(f"{fmt_size(k):>8}" for k in keys) + f"{'MHz':>8}")
    for L in legs:
        row = f"{L['name']:16}"
        tc = tu = 0
        for k in keys:
            if k in L[kind]:
                cyc, us, n = L[kind][k]
                row += f"{cyc / n:8.2f}"
                tc += cyc; tu += us
            else:
                row += f"{'-':>8}"
        row += f"{(tc / tu if tu else 0):8.0f}"
        print(row)
    print()

if off_clock:
    print(f"dropped {len(off_clock)} test(s) not within 3% of {TARGET:.0f} MHz:")
    for o in off_clock:
        print("   ", *o)
