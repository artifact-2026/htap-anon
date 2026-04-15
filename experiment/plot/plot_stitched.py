#!/usr/bin/env python3
"""
plot_stitched.py — Multi-machine stitched saturation + CPU-slack figure
=======================================================================

Produces an N × 3 panel figure where each ROW is one machine and the three
COLUMNS are:
  Col 0 — Query throughput (QPS)
  Col 1 — Disk BW utilization (%)   [from disk_bw_avg × 100]
  Col 2 — CPU utilization (%)        [cpu_compute / cpu_schedule]

Each row splices a saturation sweep and a CPU-slack sweep onto a shared x-axis.
The knee point is the divider: left = YCSB threads, right = iBench workers.

Usage
-----
  python3 plot_stitched.py \\
      --sat   machine_a/sat.csv   --slack machine_a/slack.csv  --knee 96  --label "Machine A" \\
      --sat   machine_b/sat.csv   --slack machine_b/slack.csv  --knee 40  --label "Machine B" \\
      [--out  stitched.pdf] \\
      [--title "All-machine comparison"] \\
      [--dpi 150]

For a single machine you may provide just one --sat / --slack / --knee triple.
--knee defaults to the maximum thread count found in the saturation CSV.
--label defaults to the saturation CSV file stem.

Column contract
---------------
saturation summary.csv : threads, xput_mean, xput_std, [xput_min, xput_max],
                         cpu_compute_mean/std/min/max,
                         cpu_schedule_mean/std/min/max,
                         disk_bw_avg (fraction 0-1), disk_bw_stddev

slack summary.csv      : workers, xput_mean, xput_std, [xput_min, xput_max],
                         (same system-metric columns as saturation)
"""

import argparse
import os
import sys
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.lines import Line2D
from matplotlib.patches import Patch
import matplotlib.ticker as mticker

# ── colour palette ──────────────────────────────────────────────────────────
# Same order as plot_bottleneck.py — machine i always gets _PALETTE[i].
_PALETTE = [
    '#2166ac',   # blue
    '#d6604d',   # red-orange
    '#4dac26',   # green
    '#8856a7',   # purple
    '#b35806',   # brown-orange
    '#1a9e77',   # teal
    '#e7298a',   # pink
    '#666666',   # gray
]

C_FADE = '#bbbbbb'   # light gray — post-knee saturation (faded)
C_LEG  = '#444444'   # neutral gray — legend icons


# ── colour utilities ─────────────────────────────────────────────────────────

def _shade(hex_color, factor):
    """Lighten (factor > 0) or darken (factor < 0) a hex colour."""
    import matplotlib.colors as mcolors
    r, g, b = mcolors.to_rgb(hex_color)
    if factor >= 0:
        r, g, b = r + (1-r)*factor, g + (1-g)*factor, b + (1-b)*factor
    else:
        f = 1 + factor
        r, g, b = r*f, g*f, b*f
    return mcolors.to_hex((min(r, 1), min(g, 1), min(b, 1)))


# ── low-level drawing helpers ────────────────────────────────────────────────

def _col(df, name):
    """Return df[name] as float Series, or all-NaN Series if column absent."""
    if name in df.columns:
        s = df[name].astype(float)
        if s.notna().any():
            return s
    return pd.Series([np.nan] * len(df), index=df.index)

def _band(ax, xs, lo, hi, color, alpha=0.15):
    lo_v = np.array(lo, dtype=float)
    hi_v = np.array(hi, dtype=float)
    mask = np.isfinite(lo_v) & np.isfinite(hi_v)
    if mask.any():
        ax.fill_between(np.array(xs)[mask], lo_v[mask], hi_v[mask],
                        color=color, alpha=alpha, linewidth=0)

def _errbar(ax, xs, mean, std, color, fmt='o-', lw=2, ms=6, label=None):
    m = np.array(mean, dtype=float)
    s = np.where(np.isfinite(np.array(std, dtype=float)),
                 np.array(std, dtype=float), 0.0)
    ax.errorbar(xs, m, yerr=s,
                fmt=fmt, color=color, linewidth=lw, markersize=ms,
                capsize=3, label=label)

def _phase(ax, xs, mean, std, color, fmt='o-', lw=2, ms=6,
           lo=None, hi=None, band_alpha=0.18, minmax_alpha=0.10):
    """Plot a phased segment with error bars + std band + optional min/max band."""
    _errbar(ax, xs, mean, std, color, fmt=fmt, lw=lw, ms=ms)
    s = np.where(np.isfinite(np.array(std, dtype=float)),
                 np.array(std, dtype=float), 0.0)
    m = np.asarray(mean, dtype=float)
    _band(ax, xs, m - s, m + s, color, alpha=band_alpha)
    if lo is not None and hi is not None:
        _band(ax, xs, lo, hi, color, alpha=minmax_alpha)

def _divider(ax, x):
    ax.axvline(x, color='#5A3E1B', linestyle='-', linewidth=1.8,
               alpha=0.50, zorder=5)

def _set_xticks(ax, xs, labels):
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, rotation=45, ha='right')
    ax.set_xlim(xs[0] - 0.7, xs[-1] + 0.7)


# ── x-axis construction ──────────────────────────────────────────────────────

def build_xaxis(sat, slack, knee):
    """
    Returns (tick_xs, tick_labels, knee_idx, sat_xs, slack_xs).

    Saturation: integer positions 0..n_sat-1.
    Slack: log₂-spaced positions anchored at knee_idx (+0 W).
    """
    sat_threads   = sat['threads'].tolist()
    slack_workers = slack['workers'].tolist()

    try:
        knee_idx = sat_threads.index(knee)
    except ValueError:
        candidates = [i for i, t in enumerate(sat_threads) if t <= knee]
        knee_idx = candidates[-1] if candidates else len(sat_threads) - 1

    n_sat  = len(sat_threads)
    sat_xs = list(range(n_sat))

    # Log₂-scaled positions for slack workers (true doublings = 1 unit apart).
    slack_xs = [knee_idx + np.log2(w) + 1 for w in slack_workers]

    # Build label dict.
    # Knee label is just the thread count — the divider line marks the boundary.
    # Slack ticks start from the first actual worker count (+1W, +2W, …).
    labels = {}
    for i in range(knee_idx + 1):          # includes knee itself
        labels[i] = f'{sat_threads[i]}T'
    for j, w in enumerate(slack_workers):
        labels[slack_xs[j]] = f'+{w}W'
    max_slack_x = slack_xs[-1] if slack_xs else knee_idx
    for i in range(knee_idx + 1, n_sat):
        if float(i) > max_slack_x:
            labels[float(i)] = f'({sat_threads[i]}T)'

    tick_xs     = sorted(labels.keys())
    tick_labels = [labels[x] for x in tick_xs]

    return tick_xs, tick_labels, knee_idx, sat_xs, slack_xs


# ── data loading ─────────────────────────────────────────────────────────────

def _load(path, sweep_col):
    try:
        df = pd.read_csv(path)
    except Exception as e:
        sys.exit(f'ERROR: cannot read {path}: {e}')
    if sweep_col not in df.columns:
        sys.exit(f'ERROR: expected column "{sweep_col}" in {path}. '
                 f'Got: {list(df.columns)}')
    return df.sort_values(sweep_col).reset_index(drop=True)


# ── single-row (one machine) drawing ────────────────────────────────────────

def _draw_row(axes, sat, slack, knee, mc, row_label, fsz,
              lw=2.0, ms=6, show_xticks=True):
    """
    Fill axes[0..2] (throughput, disk BW, CPU) for one machine.

    Parameters
    ----------
    axes      : sequence of 3 Axes
    sat/slack : DataFrames
    knee      : int  thread count at the saturation knee
    mc        : str  machine colour (from _PALETTE)
    row_label : str  printed as y-axis label on axes[0]
    fsz       : int  base font size
    """
    tick_xs, tick_labels, knee_idx, sat_xs, slack_xs = \
        build_xaxis(sat, slack, knee)

    pre_end = knee_idx + 1
    pre_xs  = sat_xs[:pre_end]
    post_xs = sat_xs[knee_idx:]

    MC_SAT   = _shade(mc, -0.15)
    MC_SLACK = _shade(mc, +0.40)

    knee_row     = sat.iloc[knee_idx]
    slack_xs_aug = [knee_idx] + list(slack_xs)

    def _seg(df, name, slc=None):
        v = _col(df, name).values.astype(float)
        return v[slc] if slc is not None else v

    def _seg0(df, name, slc=None):
        v = _seg(df, name, slc)
        return np.where(np.isfinite(v), v, 0.0)

    def _aug(slack_col, sat_col, zero=False):
        getter = _seg0 if zero else _seg
        arr      = getter(slack, slack_col)
        knee_val = float(knee_row[sat_col]) if sat_col in sat.columns else np.nan
        return np.concatenate([[knee_val], arr])

    # ── Col 0: Throughput ────────────────────────────────────────────────────
    ax = axes[0]
    _phase(ax, pre_xs,
           _seg(sat, 'xput_mean', slice(0, pre_end)),
           _seg0(sat, 'xput_std',  slice(0, pre_end)), MC_SAT, lw=lw, ms=ms,
           lo=_seg(sat, 'xput_min', slice(0, pre_end)),
           hi=_seg(sat, 'xput_max', slice(0, pre_end)))
    _phase(ax, post_xs,
           _seg(sat, 'xput_mean', slice(knee_idx, None)),
           _seg0(sat, 'xput_std',  slice(knee_idx, None)),
           C_FADE, fmt='o--', lw=lw, ms=ms, band_alpha=0.10, minmax_alpha=0.06)
    _phase(ax, slack_xs_aug,
           _aug('xput_mean', 'xput_mean'),
           _aug('xput_std',  'xput_std', zero=True), MC_SLACK, lw=lw, ms=ms,
           lo=_aug('xput_min', 'xput_min'),
           hi=_aug('xput_max', 'xput_max'))
    _divider(ax, knee_idx)
    ax.set_ylim(bottom=0)
    ax.yaxis.set_major_formatter(
        mticker.FuncFormatter(
            lambda v, _: f'{v/1000:.0f}K' if v >= 1000 else f'{v:.0f}'))
    # Row label as ylabel on the throughput column.
    ax.set_ylabel(row_label, fontsize=fsz + 1, fontweight='bold', labelpad=6)

    # ── Col 1: Disk BW utilization (%) ──────────────────────────────────────
    ax = axes[1]
    _phase(ax, pre_xs,
           _seg(sat, 'disk_bw_avg',    slice(0, pre_end)) * 100,
           _seg0(sat, 'disk_bw_stddev', slice(0, pre_end)) * 100,
           MC_SAT, lw=lw, ms=ms)
    _phase(ax, post_xs,
           _seg(sat, 'disk_bw_avg',    slice(knee_idx, None)) * 100,
           _seg0(sat, 'disk_bw_stddev', slice(knee_idx, None)) * 100,
           C_FADE, fmt='o--', lw=lw, ms=ms, band_alpha=0.10)
    _phase(ax, slack_xs_aug,
           _aug('disk_bw_avg',    'disk_bw_avg')    * 100,
           _aug('disk_bw_stddev', 'disk_bw_stddev', zero=True) * 100,
           MC_SLACK, lw=lw, ms=ms)
    _divider(ax, knee_idx)
    ax.set_ylim(bottom=0)
    ax.axhline(100, color='black', linestyle=':', linewidth=0.8, alpha=0.4)

    # ── Col 2: CPU utilization (%) ───────────────────────────────────────────
    ax = axes[2]
    cc_pre  = _seg(sat, 'cpu_compute_mean',  slice(0, pre_end))
    cs_pre  = _seg(sat, 'cpu_schedule_mean', slice(0, pre_end))
    cc_post = _seg(sat, 'cpu_compute_mean',  slice(knee_idx, None))
    cs_post = _seg(sat, 'cpu_schedule_mean', slice(knee_idx, None))
    cc_slk  = _aug('cpu_compute_mean',  'cpu_compute_mean')
    cs_slk  = _aug('cpu_schedule_mean', 'cpu_schedule_mean')

    _phase(ax, pre_xs, cc_pre,
           _seg0(sat, 'cpu_compute_std',  slice(0, pre_end)), MC_SAT, fmt='^-', lw=lw, ms=ms)
    _phase(ax, pre_xs, cs_pre,
           _seg0(sat, 'cpu_schedule_std', slice(0, pre_end)), MC_SAT, fmt='s-', lw=lw, ms=ms,
           band_alpha=0.10)
    ax.fill_between(pre_xs, cc_pre, cs_pre,
                    facecolor='none', edgecolor=MC_SAT,
                    hatch='xxx', alpha=0.30, linewidth=0, zorder=2)

    _phase(ax, post_xs, cc_post,
           _seg0(sat, 'cpu_compute_std',  slice(knee_idx, None)),
           C_FADE, fmt='^--', lw=lw, ms=ms, band_alpha=0.06)
    _phase(ax, post_xs, cs_post,
           _seg0(sat, 'cpu_schedule_std', slice(knee_idx, None)),
           C_FADE, fmt='s--', lw=lw, ms=ms, band_alpha=0.06)

    _phase(ax, slack_xs_aug, cc_slk,
           _aug('cpu_compute_std',  'cpu_compute_std',  zero=True),
           MC_SLACK, fmt='^-', lw=lw, ms=ms)
    _phase(ax, slack_xs_aug, cs_slk,
           _aug('cpu_schedule_std', 'cpu_schedule_std', zero=True),
           MC_SLACK, fmt='s-', lw=lw, ms=ms, band_alpha=0.10)
    ax.fill_between(slack_xs_aug, cc_slk, cs_slk,
                    facecolor='none', edgecolor=MC_SLACK,
                    hatch='xxx', alpha=0.30, linewidth=0, zorder=2)

    _divider(ax, knee_idx)
    ax.set_ylim(bottom=0, top=105)
    ax.axhline(100, color='black', linestyle=':', linewidth=0.8, alpha=0.4)

    # ── Shared x-axis ticks on all three panels ──────────────────────────────
    for a in axes:
        if show_xticks:
            _set_xticks(a, tick_xs, tick_labels)
        else:
            # Set tick positions only; keep labels invisible so spacing is correct.
            a.set_xticks(tick_xs)
            a.set_xlim(tick_xs[0] - 0.7, tick_xs[-1] + 0.7)
            plt.setp(a.get_xticklabels(), visible=False)

    # ── Knee annotation on the throughput panel ──────────────────────────────
    for ax in axes:
        yl = ax.get_ylim()
        ax.annotate(
            f'Knee\n{knee}T',
            xy=(knee_idx, yl[0]),
            xytext=(knee_idx + 0.55,
                yl[0] + 0.10 * (yl[1] - yl[0])),
            fontsize=fsz - 2, color='gray', fontweight='bold',
            arrowprops=dict(arrowstyle='->', color='gray', lw=0.8),
        )


# ── CLI ──────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description='N × 3 stitched saturation + slack figure (one row per machine).',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    p.add_argument('--sat',   action='append', required=True, metavar='PATH',
                   help='Saturation CSV.  Repeat once per machine.')
    p.add_argument('--slack', action='append', required=True, metavar='PATH',
                   help='Slack CSV.  Must be given the same number of times as --sat.')
    p.add_argument('--knee',  action='append', type=int,   metavar='N',
                   default=None,
                   help='Knee thread count for each machine (same order as --sat). '
                        'Omit to use max threads in that CSV.  '
                        'Repeat once per machine, or once for all.')
    p.add_argument('--label', action='append', metavar='LABEL',
                   default=None,
                   help='Row label for each machine (appears as y-axis title of the '
                        'throughput panel).  Default: CSV file stem.  Repeat per machine.')
    p.add_argument('--out',   default='stitched.pdf', help='Output file path.')
    p.add_argument('--title', default='',  help='Optional figure suptitle.')
    p.add_argument('--width', type=float,  default=14.0, help='Figure width in inches.')
    p.add_argument('--row-height', type=float, dest='row_height', default=2.125,
                   help='Height per machine row in inches.  Default: 2.55.')
    p.add_argument('--font-size', type=int, dest='font_size', default=11,
                   help='Base font size.  Default: 11.')
    p.add_argument('--dpi',   type=int,   default=150)
    return p.parse_args()


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    args = parse_args()

    # ── Validate argument lists ───────────────────────────────────────────────
    n = len(args.sat)
    if len(args.slack) != n:
        sys.exit(f'ERROR: {len(args.sat)} --sat but {len(args.slack)} --slack args. '
                 f'Provide one of each per machine.')

    # Normalise --knee (accept fewer than n → broadcast last value).
    knees_raw = args.knee or []
    if len(knees_raw) == 0:
        knees = [None] * n          # all default to CSV max
    elif len(knees_raw) == 1:
        knees = knees_raw * n       # broadcast single value
    elif len(knees_raw) == n:
        knees = knees_raw
    else:
        sys.exit(f'ERROR: --knee must be given 0, 1, or {n} times (got {len(knees_raw)}).')

    # Normalise --label.
    labels_raw = args.label or []
    if len(labels_raw) == 0:
        labels = [os.path.splitext(os.path.basename(p))[0] for p in args.sat]
    elif len(labels_raw) == n:
        labels = labels_raw
    else:
        sys.exit(f'ERROR: --label must be given 0 or {n} times (got {len(labels_raw)}).')

    # ── Load all CSVs ─────────────────────────────────────────────────────────
    machines = []
    for i in range(n):
        sat   = _load(args.sat[i],   'threads')
        slack = _load(args.slack[i], 'workers')
        knee  = knees[i] if knees[i] is not None else int(sat['threads'].max())
        mc    = _PALETTE[i % len(_PALETTE)]
        machines.append(dict(sat=sat, slack=slack, knee=knee,
                             label=labels[i], mc=mc))
        print(f'[{labels[i]}]  sat={len(sat)} pts, slack={len(slack)} pts, knee={knee}T, '
              f'color={mc}')

    # ── Figure layout: N rows × 3 cols ───────────────────────────────────────
    fsz = args.font_size
    plt.rcParams.update({
        'figure.dpi'        : args.dpi,
        'font.family'       : 'sans-serif',
        'font.size'         : fsz,
        'axes.spines.top'   : False,
        'axes.spines.right' : False,
        'axes.grid'         : True,
        'grid.alpha'        : 0.28,
        'axes.labelsize'    : fsz,
        'xtick.labelsize'   : fsz - 1,
        'ytick.labelsize'   : fsz - 1,
        'axes.titlesize'    : fsz + 1,
        'legend.fontsize'   : fsz,
    })

    COL_TITLES = ['Throughput (QPS)', 'Disk BW util. (%)', 'CPU util. (%)']

    fig_h = args.row_height * n + (1.0 if args.title else 0.5)
    fig   = plt.figure(figsize=(args.width, fig_h))

    if args.title:
        fig.suptitle(args.title, fontsize=fsz + 3, fontweight='bold', y=0.995)

    # Reserve bottom space for the shared legend.
    LEGEND_H = 0.07          # fraction of figure height
    gs = gridspec.GridSpec(
        n, 3, figure=fig,
        hspace=0.33, wspace=0.08,
        top=0.96 if not args.title else 0.94,
        bottom=LEGEND_H + 0.03,
        left=0.09, right=0.97,
    )

    # Draw each machine row.  Column titles are applied inside for i==0 only,
    # avoiding duplicate add_subplot calls on the same GridSpec cell.
    all_axes = []
    for i, m in enumerate(machines):
        is_bottom = (i == n - 1)
        row_axes  = [fig.add_subplot(gs[i, j]) for j in range(3)]

        # Column titles on the first row only.
        if i == 0:
            for ax, ct in zip(row_axes, COL_TITLES):
                ax.set_title(ct, fontsize=fsz + 1, fontweight='bold', pad=8)

        _draw_row(row_axes, m['sat'], m['slack'], m['knee'],
                  m['mc'], m['label'], fsz,
                  show_xticks=True)   # every row shows its own tick labels

        all_axes.append(row_axes)

    # ── Shared legend (phase + metric encoding) ──────────────────────────────
    legend_handles = [
        # Phase encoding.
        Line2D([0],[0], color='#555555', lw=2.5, ls='-',  label='Saturation (pre-knee)'),
         Line2D([0],[0], color=C_FADE,    lw=2.0, ls='--', label='Post-knee sat. (faded)'),
        Line2D([0],[0], color='#aaaaaa', lw=2.5, ls='-',  label='Slack phase (lighter)'),
        # X-axis notation glossary.
        Line2D([0],[0], color='none', label='T = YCSB threads'),
        Line2D([0],[0], color='none', label='W = CPU workers (slack)'),
        # Metric encoding (CPU panel).
        Line2D([0],[0], color=C_LEG, lw=2, marker='^', ms=5, ls='-',
               label='CPU compute'),
        Line2D([0],[0], color=C_LEG, lw=2, marker='s', ms=5, ls='-',
               label='CPU scheduled'),

    ]
    fig.legend(
        handles=legend_handles,
        loc='lower center',
        bbox_to_anchor=(0.5, 0.0),
        ncol=8,
        framealpha=0.88,
        handlelength=2.0,
        borderpad=0.5,
        columnspacing=1.2,
        fontsize=fsz - 1,
    )

    fig.savefig(args.out, bbox_inches='tight', dpi=args.dpi)
    print(f'Saved → {args.out}')


if __name__ == '__main__':
    main()
