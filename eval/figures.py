#!/usr/bin/env python3
"""
R26-IT-169 -- presentation figures, generated from eval/out/metrics.json.

Four PNGs at 1600x900, sized and weighted to be read from the back of a room.
Run analyze.py first.

  python eval/figures.py --metrics eval/out/metrics.json --db backend/smart-bin.db \
                         --out-dir eval/out [--node bin01]
"""

import argparse
import json
import os
import sqlite3
from datetime import datetime

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
import pandas as pd

# -- design tokens ----------------------------------------------------------
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e5e4e0"
S1_BLUE = "#2a78d6"
S2_ORANGE = "#eb6834"
S3_AQUA = "#1baf7a"
S4_YELLOW = "#eda100"
S8_RED = "#e34948"

STATE_COLOR = {"STABLE": S1_BLUE, "ACTIVE": S3_AQUA,
               "ALERT": S8_RED, "CONSERVE": S4_YELLOW}

FIGSIZE = (10.667, 6)   # 1600x900 at 150 dpi
DPI = 150

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
    "font.size": 15, "font.family": "sans-serif",
    "text.color": INK, "axes.labelcolor": INK_2, "axes.titlecolor": INK,
    "xtick.color": INK_2, "ytick.color": INK_2,
    "axes.edgecolor": GRID, "grid.color": GRID,
    "axes.grid": True, "grid.linewidth": 0.8, "axes.axisbelow": True,
    "axes.spines.top": False, "axes.spines.right": False,
    "lines.linewidth": 2.0,
})


def rounded_bar(ax, x, height, width, color, radius_frac=0.06, vertical=True):
    """Bar with its data-end rounded and its baseline end square."""
    if height <= 0:
        return
    r = min(width * 0.18, height * radius_frac)
    ax.add_patch(FancyBboxPatch(
        (x - width / 2 + r, r), width - 2 * r, max(height - 2 * r, 1e-9),
        boxstyle=f"round,pad={r}", linewidth=0, facecolor=color, zorder=3))
    # square off the baseline end so the bar sits on the axis
    ax.add_patch(plt.Rectangle((x - width / 2, 0), width, min(r * 2, height),
                               linewidth=0, facecolor=color, zorder=3))


def header(fig, title, subtitle=None):
    """Title and subtitle in figure coordinates.

    set_title(pad=...) works in points while a subtitle placed at an axes
    fraction works in axes height, so the two silently swap order depending on
    figure size. Fixed figure coordinates keep them in the order written.
    """
    fig.text(0.035, 0.945, title, fontsize=20, color=INK, ha="left", va="center")
    if subtitle:
        fig.text(0.035, 0.885, subtitle, fontsize=13.5, color=INK_2,
                 ha="left", va="top", linespacing=1.45)


def finish(fig, path, tight=True):
    if tight:
        fig.tight_layout()
    fig.savefig(path, dpi=DPI)
    plt.close(fig)
    print(f"  wrote {path}")


# ---------------------------------------------------------------------------
# fig 1 -- the headline. One number, with the two rates that produced it.
# ---------------------------------------------------------------------------
def fig_tx(m, out):
    t = m["tcr"]
    b, a = t["baseline"]["per_hour"], t["adaptive"]["per_hour"]

    fig = plt.figure(figsize=FIGSIZE)
    fig.text(0.035, 0.935, "Experiment 1  ·  adaptive policy vs fixed-interval baseline",
             fontsize=20, color=INK, ha="left", va="center")

    # hero number, on its own canvas so it cannot collide with the plot
    ax0 = fig.add_axes([0.035, 0.08, 0.44, 0.78]); ax0.axis("off")
    ax0.set_xlim(0, 1); ax0.set_ylim(0, 1)
    ax0.text(0, 0.72, f"{t['headline_pct']:.1f}%", fontsize=88,
             color=S1_BLUE, ha="left", va="center", fontweight="bold")
    ax0.text(0.005, 0.46, "Transmission Count Reduction", fontsize=19, color=INK)
    ax0.text(0.005, 0.37, "target " + r"$\geq$" + " 60%", fontsize=15, color=INK_2)
    ax0.text(0.005, 0.22,
             f"{t['baseline']['tx']} baseline vs {t['adaptive']['tx']} adaptive\n"
             f"transmissions over {t['baseline']['hours']:.2f} h and "
             f"{t['adaptive']['hours']:.2f} h",
             fontsize=13.5, color=INK_2, va="top", linespacing=1.5)

    # supporting bars
    ax = fig.add_axes([0.60, 0.16, 0.365, 0.62])
    for i, (val, col) in enumerate([(b, S2_ORANGE), (a, S1_BLUE)]):
        rounded_bar(ax, i, val, 0.42, col)
        ax.text(i, val + max(b, a) * 0.04, f"{val:.1f}", ha="center",
                fontsize=19, color=INK, fontweight="bold")
    ax.set_xticks([0, 1])
    ax.set_xticklabels(["BASELINE\nfixed 60 s", "ADAPTIVE\npolicy"], fontsize=14)
    ax.set_xlim(-0.6, 1.6)
    ax.set_ylim(0, max(b, a) * 1.25)
    ax.set_ylabel("transmissions per hour", fontsize=14)
    ax.tick_params(labelsize=13)
    ax.grid(axis="x", visible=False)
    ax.text(0, 1.07, "Transmission rate", transform=ax.transAxes,
            fontsize=16, color=INK)
    finish(fig, os.path.join(out, "fig1_tx_comparison.png"), tight=False)


# ---------------------------------------------------------------------------
# fig 2 -- which mechanism caused each transmission
# ---------------------------------------------------------------------------
def fig_reasons(m, out):
    r = m["tcr"]["adaptive_reasons"]
    fig = plt.figure(figsize=FIGSIZE)
    ax = fig.add_axes([0.17, 0.14, 0.78, 0.62])
    if not r:
        ax.axis("off")
        ax.text(0.5, 0.5, "no adaptive transmissions recorded",
                ha="center", color=INK_2, fontsize=18)
        finish(fig, os.path.join(out, "fig2_reason_breakdown.png"))
        return

    items = sorted(r.items(), key=lambda x: x[1])
    labels = [k for k, _ in items]
    vals = [v for _, v in items]
    total = sum(vals)
    y = range(len(items))
    # position carries identity here, so one hue is correct
    ax.barh(list(y), vals, height=0.55, color=S1_BLUE, zorder=3)
    for i, v in zip(y, vals):
        ax.text(v + max(vals) * 0.015, i, f"{v}  ({100*v/total:.0f}%)",
                va="center", fontsize=15, color=INK)
    ax.set_yticks(list(y)); ax.set_yticklabels(labels, fontsize=15)
    # keep a single bar from filling the frame as one huge block
    ax.set_ylim(-0.6, max(len(items) - 0.4, 2.6))
    ax.set_xlim(0, max(vals) * 1.35)
    ax.set_xlabel("transmissions")
    ax.grid(axis="y", visible=False)
    sub = ("every transmission came from the AoI timer:\nthis run exercised the "
           "interval control only" if set(labels) == {"timer"} else
           "each reason maps to one policy output")
    header(fig, "What caused each adaptive transmission", sub)
    finish(fig, os.path.join(out, "fig2_reason_breakdown.png"), tight=False)


# ---------------------------------------------------------------------------
# fig 3 -- T* over time. The figure the May dataset could not produce.
# ---------------------------------------------------------------------------
def fig_interval(m, out, db, label, node):
    # Run labels are shared by every node, so the node filter is required.
    con = sqlite3.connect(db)
    df = pd.read_sql_query(
        "SELECT rx_ts, interval, state, batt_pct FROM readings "
        "WHERE run_label=? AND node_id=? AND mode='ADAPTIVE' ORDER BY rx_ts",
        con, params=(label, node))
    con.close()

    fig = plt.figure(figsize=FIGSIZE)
    ax = fig.add_axes([0.09, 0.13, 0.87, 0.63])
    if len(df) < 2:
        ax.axis("off")
        ax.text(0.5, 0.5, "not enough adaptive rows to plot",
                ha="center", color=INK_2, fontsize=18)
        finish(fig, os.path.join(out, "fig3_interval_timeline.png"))
        return

    t0 = df["rx_ts"].min()
    mins = (df["rx_ts"] - t0) / 60000.0
    ax.step(mins, df["interval"], where="post", color=S1_BLUE, linewidth=2.4, zorder=4)
    # markers only while they stay separable; past that they smear into a dashed rule
    if len(df) <= 60:
        ax.plot(mins, df["interval"], "o", ms=8, color=S1_BLUE,
                mec=SURFACE, mew=2, zorder=5)

    top = df["interval"].max() * 1.30
    band_h, band_y = top * 0.055, -top * 0.075
    seen = []
    for i in range(len(df)):
        x0 = mins.iloc[i]
        x1 = mins.iloc[i + 1] if i + 1 < len(df) else mins.iloc[-1] + 1
        st = df["state"].iloc[i]
        ax.add_patch(plt.Rectangle((x0, band_y), x1 - x0, band_h, linewidth=0,
                                   facecolor=STATE_COLOR.get(st, INK_2), zorder=3))
        if st not in seen:
            seen.append(st)
            ax.text(x0 + (x1 - x0) / 2, band_y + band_h * 1.9, st, fontsize=12,
                    color=INK, ha="left", va="bottom")

    # mark the battery step-down, which is what makes T* climb
    bt = df["batt_pct"]
    drop = bt[bt.diff() < 0]
    if len(drop):
        xd = mins.iloc[drop.index[0]]
        ax.axvline(xd, color=INK_2, linewidth=1.2, zorder=2)
        ax.text(xd + (mins.max() * 0.008), top * 0.94,
                f"battery {bt.iloc[drop.index[0]-1]:.0f}% "
                f"→ {bt.iloc[drop.index[0]]:.0f}%",
                fontsize=13, color=INK_2, va="top")

    ax.set_ylim(band_y - band_h * 0.5, top)
    ax.set_xlim(0, mins.max() * 1.02)
    ax.set_xlabel("minutes into adaptive phase")
    ax.set_ylabel("T*  transmission interval (s)")
    ax.grid(axis="x", visible=False)
    header(fig, "AIMD interval control, with FSM state",
           "T* rises once the battery falls below the AIMD threshold;\n"
           "the band under the axis is the FSM state")
    finish(fig, os.path.join(out, "fig3_interval_timeline.png"), tight=False)


# ---------------------------------------------------------------------------
# fig 4 -- BLEF, bounded rather than asserted
# ---------------------------------------------------------------------------
def fig_blef(m, out):
    b = m["blef"]
    fig = plt.figure(figsize=FIGSIZE)
    ax = fig.add_axes([0.09, 0.15, 0.87, 0.61])
    if "sensitivity" not in b:
        ax.axis("off")
        ax.text(0.5, 0.5, "BLEF not computed", ha="center", color=INK_2, fontsize=18)
        finish(fig, os.path.join(out, "fig4_blef_sensitivity.png"))
        return

    s = b["sensitivity"]
    xs = [r["t_tx_window_s"] for r in s]
    ys = [r["blef"] for r in s]
    for i, (x, y) in enumerate(zip(xs, ys)):
        rounded_bar(ax, i, y, 0.30, S1_BLUE)
        ax.text(i, y + max(ys) * 0.04, f"{y:.2f}×", ha="center",
                fontsize=19, color=INK, fontweight="bold")
    ax.axhline(2.0, color=S2_ORANGE, linewidth=2, zorder=4)
    ax.text(-0.48, 2.0 + max(ys) * 0.03, "target 2×",
            fontsize=14, color=S2_ORANGE, va="bottom", ha="left")
    ax.set_xticks(range(len(xs)))
    ax.set_xticklabels([f"{x:.1f} s" for x in xs])
    ax.set_xlim(-0.55, len(xs) - 0.45)
    ax.set_xlabel("assumed transmission window (s)  ·  the dominant unknown")
    ax.set_ylabel("battery life extension factor")
    ax.set_ylim(0, max(ys) * 1.26)
    ax.grid(axis="x", visible=False)
    header(fig, "MODELLED battery life extension  (not measured)",
           "Duty-cycle model driven by the measured transmission rate.\n"
           "Deep sleep is disabled during the demo, so battery life was not observed.")
    finish(fig, os.path.join(out, "fig4_blef_sensitivity.png"), tight=False)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--metrics", default="eval/out/metrics.json")
    p.add_argument("--db", default="backend/smart-bin.db")
    p.add_argument("--out-dir", default="eval/out")
    p.add_argument("--node", default=None,
                   help="node_id for the T* timeline; defaults to the node in metrics.json, else bin01")
    a = p.parse_args()

    with open(a.metrics) as f:
        m = json.load(f)
    node = a.node or m.get("node") or "bin01"
    if m.get("node") and node != m["node"]:
        print(f"WARNING: --node {node} differs from metrics.json node {m['node']}; "
              "figures 1, 2 and 4 still show the metrics.json node")
    os.makedirs(a.out_dir, exist_ok=True)

    fig_tx(m, a.out_dir)
    fig_reasons(m, a.out_dir)
    fig_interval(m, a.out_dir, a.db, m["labels"]["adaptive"], node)
    fig_blef(m, a.out_dir)
    print("done", datetime.now().strftime("%H:%M:%S"))


if __name__ == "__main__":
    main()
