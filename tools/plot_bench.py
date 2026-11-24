#!/usr/bin/env python3
"""Draw the README benchmark charts from a bench table (tools/bench_table.py
output) as SVGs, one light and one dark variant each.

  uv run --with matplotlib tools/plot_bench.py TABLE.txt docs/img
"""
import sys
from pathlib import Path

import matplotlib
matplotlib.use("svg")
import matplotlib.pyplot as plt

STRIDES = ["4", "8", "16", "32", "64", "128", "256", "512"]
SETS = ["4K", "8K", "16K", "32K", "64K", "256K", "512K", "2M"]

THEMES = {
    "light": dict(bg="#ffffff", ink="#1f2328", muted="#59636e", grid="#e1e0d9",
                  stock="#888780"),
    "dark": dict(bg="#0d1117", ink="#e6edf3", muted="#9198a1", grid="#2c2c2a",
                 stock="#9a998f"),
}
BLUE, ORANGE, AQUA, VIOLET = "#2a78d6", "#eb6834", "#1baf7a", "#7a6be0"


def parse(path):
    """{'chase'|'stride': {leg: [8 values]}} from a bench table file."""
    out, kind = {}, None
    for line in Path(path).read_text().splitlines():
        if line.startswith("chase:"):
            kind = "chase"
        elif line.startswith("stride:"):
            kind = "stride"
        elif kind and line and not line.startswith(("leg", " ", "dropped")):
            f = line.split()
            out.setdefault(kind, {})[f[0]] = [float(v) for v in f[1:9]]
        elif not line:
            kind = None
    return out


def axes(t, xlabel, ylabel):
    fig, ax = plt.subplots(figsize=(8, 3.6), dpi=100)
    fig.patch.set_facecolor(t["bg"])
    ax.set_facecolor(t["bg"])
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.spines["bottom"].set_color(t["grid"])
    ax.tick_params(colors=t["muted"], length=0, labelsize=10)
    ax.yaxis.grid(True, color=t["grid"], linewidth=1)
    ax.set_axisbelow(True)
    ax.set_xlabel(xlabel, color=t["muted"], fontsize=10)
    ax.set_ylabel(ylabel, color=t["muted"], fontsize=10)
    return fig, ax


def legend(ax, t, ncol):
    lg = ax.legend(loc="lower center", bbox_to_anchor=(0.5, 1.02), borderaxespad=0.4, ncol=ncol,
                   frameon=False, fontsize=10, handlelength=2.6)
    for txt in lg.get_texts():
        txt.set_color(t["ink"])


def line(ax, y, label, color, ls="-", marker="o", lw=2):
    ax.plot(STRIDES, y, label=label, color=color,
            linestyle=ls, marker=marker, markersize=5, linewidth=lw,
            solid_capstyle="round", dash_capstyle="round")


def save(fig, out, name, theme):
    fig.tight_layout()
    fig.savefig(out / f"{name}-{theme}.svg", facecolor=fig.get_facecolor(),
                bbox_inches="tight", pad_inches=0.15, metadata={"Date": None})
    plt.close(fig)


def main():
    d = parse(sys.argv[1])
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    st, ch = d["stride"], d["chase"]
    plt.rcParams["svg.hashsalt"] = "cpu_tweaks"

    for theme, t in THEMES.items():
        fig, ax = axes(t, "stride (bytes)", "cycles per load")
        line(ax, st["base"], "stock", t["stock"], marker="s")
        line(ax, st["hint"], "ACTLR hint only", BLUE, ls="--", marker="^")
        line(ax, st["l2_o15"], "PL310 prefetch only, offset 15", ORANGE,
             ls=":", marker="D")
        line(ax, st["l1l2_o15"], "both, offset 15 (default)", AQUA, lw=3)
        ax.set_ylim(0, 55)
        legend(ax, t, 2)
        save(fig, out, "stride", theme)

        fig, ax = axes(t, "stride (bytes)", "× faster than stock")
        sp = [a / b for a, b in zip(st["base"], st["l1l2_o15"])]
        bars = ax.bar(STRIDES, sp, color=AQUA, width=0.45)
        for b, v in zip(bars, sp):
            ax.text(b.get_x() + b.get_width() / 2, v + 0.05, f"{v:.1f}×",
                    ha="center", va="bottom", color=t["ink"], fontsize=10)
        ax.set_ylim(0, 3.4)
        save(fig, out, "speedup", theme)

        fig, ax = axes(t, "stride (bytes)", "cycles per load")
        line(ax, st["l1l2_o0"], "offset 0", t["stock"], ls=":", marker="s")
        line(ax, st["l1l2_o7"], "offset 7", ORANGE, ls="--", marker="^")
        line(ax, st["l1l2_o15"], "offset 15 (default)", AQUA, lw=3)
        line(ax, st["l1l2_o31"], "offset 31", VIOLET, ls="-.", marker="D")
        ax.set_ylim(0, 55)
        legend(ax, t, 4)
        save(fig, out, "offset", theme)

        fig, ax = axes(t, "working set", "cycles per load")
        ax.plot(SETS, ch["base"], label="stock", color=t["stock"], marker="s",
                markersize=5, linewidth=2)
        ax.plot(SETS, ch["l1l2_o15"], label="default config", color=AQUA,
                marker="o", markersize=5, linewidth=3)
        ax.set_ylim(0, 38)
        legend(ax, t, 2)
        save(fig, out, "random", theme)


if __name__ == "__main__":
    main()
