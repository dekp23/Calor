"""Plot Calor benchmark results.

    python3 bench/plot.py bench/results-x86-container.csv docs/

Writes two PNGs into the output directory:
  benchmark-crossover.png  query time vs range length (fixed size)
  benchmark-scaling.png    random-range query time vs number of days
Requires matplotlib.
"""
import csv
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

# Validated categorical palette (lightness, chroma, CVD, contrast checks).
COLORS = {"linear": "#2c5da8", "sqlite": "#a87812", "segtree": "#a3306f"}
NAMES = {"linear": "Linear scan", "sqlite": "Indexed SQLite", "segtree": "Segment tree"}
MARKERS = {"linear": "o", "sqlite": "s", "segtree": "D"}
INK, MUTED, GRID = "#17202a", "#5d6670", "#e4e7e1"


def load(path):
    rows = defaultdict(dict)  # (shape, method) -> {days: ns}
    with open(path) as f:
        for r in csv.DictReader(f):
            rows[(r["shape"], r["method"])][int(r["days"])] = float(r["ns_per_op"])
    return rows


def style(ax, title, xlabel):
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_title(title, loc="left", color=INK, fontsize=12, fontweight="bold", pad=12)
    ax.set_xlabel(xlabel, color=MUTED)
    ax.set_ylabel("Time per query (ns, log scale)", color=MUTED)
    ax.grid(True, which="major", color=GRID, linewidth=0.8)
    ax.tick_params(colors=MUTED)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)


def end_label(ax, x, y, method):
    ax.annotate(NAMES[method], (x, y), xytext=(6, 0), textcoords="offset points",
                va="center", color=INK, fontsize=9)


def crossover(rows, out):
    lens = sorted({int(s[3:]) for (s, _m) in rows if s.startswith("len")})
    if not lens:
        return
    n = next(iter(rows[(f"len{lens[0]}", "segtree")]))
    fig, ax = plt.subplots(figsize=(8, 4.6), dpi=150)
    for m in ("linear", "sqlite", "segtree"):
        ys = [rows[(f"len{L}", m)][n] for L in lens]
        ax.plot(lens, ys, color=COLORS[m], linewidth=2, marker=MARKERS[m], markersize=5, label=NAMES[m])
        end_label(ax, lens[-1], ys[-1], m)
    style(ax, f"Range-query time by range length ({n:,} days indexed)", "Days in the queried range (log scale)")
    ax.legend(frameon=False, loc="upper left", labelcolor=INK)
    ax.set_xlim(right=lens[-1] * 4)
    fig.tight_layout()
    fig.savefig(out / "benchmark-crossover.png")
    plt.close(fig)


def scaling(rows, out):
    fig, ax = plt.subplots(figsize=(8, 4.6), dpi=150)
    last = {}
    for m in ("linear", "sqlite", "segtree"):
        pts = sorted(rows[("random", m)].items())
        if not pts:
            continue
        xs, ys = zip(*pts)
        ax.plot(xs, ys, color=COLORS[m], linewidth=2, marker=MARKERS[m], markersize=5, label=NAMES[m])
        last[m] = (xs[-1], ys[-1])
    for m, (x, y) in last.items():
        end_label(ax, x, y, m)
    style(ax, "Random-range query time as history grows", "Days of history (log scale)")
    ax.legend(frameon=False, loc="upper left", labelcolor=INK)
    if last:
        ax.set_xlim(right=max(x for x, _ in last.values()) * 4)
    fig.tight_layout()
    fig.savefig(out / "benchmark-scaling.png")
    plt.close(fig)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    rows = load(sys.argv[1])
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    crossover(rows, out)
    scaling(rows, out)
    print(f"Wrote {out / 'benchmark-crossover.png'} and {out / 'benchmark-scaling.png'}")


if __name__ == "__main__":
    main()
