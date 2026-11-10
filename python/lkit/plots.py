"""Plot style and family-of-curves helpers for milestone reports (LOMAS Plugin-kit; from Passive EQ)."""
from __future__ import annotations

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.colors import LinearSegmentedColormap  # noqa: E402

# Validated palette (dataviz reference instance), light mode.
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
CATEGORICAL = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
# Ordinal blue ramp, steps 250 -> 700 (lightest still >= 2:1 on the light surface).
_BLUE = ["#86b6ef", "#6da7ec", "#5598e7", "#3987e5", "#2a78d6", "#256abf", "#1c5cab", "#184f95",
         "#104281", "#0d366b"]
ORDINAL = LinearSegmentedColormap.from_list("ordinal_blue", _BLUE)

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": INK2, "axes.labelcolor": INK, "xtick.color": INK2, "ytick.color": INK2,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "axes.spines.top": False,
    "axes.spines.right": False, "lines.linewidth": 1.4, "font.size": 8, "axes.titlesize": 9,
    "legend.frameon": False, "axes.titlelocation": "left",
})


def ordinal(n: int) -> list:
    return [ORDINAL(i / max(n - 1, 1)) for i in range(n)]


def logx(ax, lo: float = 10, hi: float = 22000) -> None:
    ax.set_xscale("log")
    ax.set_xlim(lo, hi)
    ticks = [t for t in [10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000] if lo <= t <= hi]
    ax.set_xticks(ticks)
    ax.set_xticklabels([f"{t // 1000}k" if t >= 1000 else str(t) for t in ticks])
    ax.minorticks_off()


def family(ax, f, curves, values, title, label_fmt="{:g}", legend_title=None, ylabel="dB re flat"):
    """Plot an ordered family of curves (e.g. Boost 0..10) with an ordinal ramp.

    Direct-labels the first and last curve at the right edge; a compact legend names all.
    """
    cols = ordinal(len(curves))
    for c, v, col in zip(curves, values, cols):
        ax.plot(f, c, color=col, linewidth=1.2, label=label_fmt.format(v))
    ax.set_title(title, color=INK)
    ax.set_ylabel(ylabel)
    logx(ax)
    if legend_title is not None:
        ax.legend(title=legend_title, fontsize=6, title_fontsize=7, ncol=2, loc="best",
                  handlelength=1.2, columnspacing=0.8, labelspacing=0.2)


def grid_of(n: int, ncols: int = 2, w: float = 3.9, h: float = 2.6, sharey: bool = True):
    nrows = int(np.ceil(n / ncols))
    fig, axes = plt.subplots(nrows, ncols, figsize=(w * ncols, h * nrows), sharey=sharey, squeeze=False)
    for ax in axes.flat[n:]:
        ax.set_visible(False)
    return fig, list(axes.flat[:n])
