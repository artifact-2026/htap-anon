#!/usr/bin/env python3
"""
plot_stitched.py — Stitched saturation + CPU-slack figure
==========================================================

Produces a 3-panel figure that joins the two experiment phases on one x-axis:

  LEFT of the divider  — saturation sweep (x = YCSB thread count)
  RIGHT of the divider — CPU-slack sweep  (x = # iBench workers at KNEE_THREADS)

Panels:
  1. Write throughput (ops/s)     — mean ± std, min/max band where available
  2. Disk bandwidth  (MB/s)       — read & write mean ± std, min/max bands
  3. CPU utilization (%)          — cpu_compute (excl. iowait) and
                                    cpu_schedule (incl. iowait) mean ± std

Usage
-----
  python3 plot_stitched.py \\
      --sat   path/to/saturation/summary.csv \\
      --slack path/to/slack/summary.csv \\
      [--out  stitched.png] \\
      [--knee 32]            # thread count that is the saturation knee
                             # (drawn as a vertical divider; default = max threads)

Column contract
---------------
saturation summary.csv  : threads, xput_mean, xput_std,
                          cpu_compute_mean/std/min/max, cpu_schedule_mean/std/min/max,
                          disk_read_mb/s, disk_read_mb/s_std, disk_read_mb/s_min/max,
                          disk_write_mb/s, disk_write_mb/s_std, disk_write_mb/s_min/max,
                          r/s, r/s_std, r/s_min/max, w/s, w/s_std, w/s_min/max,
                          mem_used_mean, mem_used_std

slack summary.csv       : workers, xput_mean, xput_std, [xput_min, xput_max],
                          (same system-metric columns as above)
"""

import argparse
import sys
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.lines import Line2D
import matplotlib.ticker as mticker

# ── colour palette ─────────────────────────────────────────────────────────────
# Machine palette — same order as plot_bottleneck.py so colours stay consistent
# across all figures for a given machine.
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

C_FADE     = '#999999'   # gray  — post-knee saturation (faded)
C_BAND     = 0.15        # alpha for std-dev fill bands

# ── colour utilities ──────────────────────────────────────────────────────────

def _shade(hex_color, factor):
    """Return a lighter (factor > 1) or darker (factor < 0) variant of hex_color.

    factor is a float in (-1, 1):
      +0.35  → blend 35 % toward white  (lighten)
      -0.30  → blend 30 % toward black  (darken)
    Clamps each channel to [0, 1].
    """
    import matplotlib.colors as mcolors
    r, g, b = mcolors.to_rgb(hex_color)
    if factor >= 0:          # lighten: blend toward white
        r, g, b = r + (1-r)*factor, g + (1-g)*factor, b + (1-b)*factor
    else:                    # darken:  blend toward black
        f = 1 + factor       # e.g. factor=-0.3 → f=0.7
        r, g, b = r*f, g*f, b*f
    return mcolors.to_hex((min(r,1), min(g,1), min(b,1)))

# ── helpers ───────────────────────────────────────────────────────────────────

def col(df, name, default=None):
    """Return df[name] if it exists and is not all-NaN, else default Series."""
    if name in df.columns:
        s = df[name].astype(float)
        if s.notna().any():
            return s
    if default is not None:
        return default
    return pd.Series([np.nan] * len(df), index=df.index)

def band(ax, xs, lo, hi, color, alpha=C_BAND):
    """Fill between lo/hi only if both are non-NaN everywhere."""
    lo_v = np.array(lo, dtype=float)
    hi_v = np.array(hi, dtype=float)
    mask = np.isfinite(lo_v) & np.isfinite(hi_v)
    if mask.any():
        ax.fill_between(np.array(xs)[mask], lo_v[mask], hi_v[mask],
                        color=color, alpha=alpha, linewidth=0)

def errbar(ax, xs, mean, std, color, label=None, lw=2, ms=6, fmt='o-'):
    """Plot mean ± std error bars; skip NaN points gracefully."""
    m = np.array(mean, dtype=float)
    s = np.array(std,  dtype=float)
    s = np.where(np.isfinite(s), s, 0.0)
    ax.errorbar(xs, m, yerr=s,
                fmt=fmt, color=color, linewidth=lw, markersize=ms,
                capsize=3, label=label)

def add_divider(ax, x_div, label='Knee / phase boundary'):
    ax.axvline(x_div, color='#333333', linestyle='-', linewidth=2.0, alpha=0.55, zorder=5)

def set_xticks(ax, xs, labels, rotate=True):
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, rotation=45 if rotate else 0,
                       ha='right' if rotate else 'center')
    ax.set_xlim(xs[0] - 0.7, xs[-1] + 0.7)

# ── main ──────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(description='Stitched saturation + CPU-slack plot')
    p.add_argument('--sat',   required=True, help='saturation summary.csv')
    p.add_argument('--slack', required=True, help='cpu_slack summary.csv')
    p.add_argument('--out',   default='stitched.png', help='output PNG path')
    p.add_argument('--knee',  type=int, default=None,
                   help='Thread count at saturation knee (default: max threads in sat CSV)')
    p.add_argument('--title', default='', help='Optional figure title')
    p.add_argument('--color', default=None,
                   help='Machine colour: a hex code (#2166ac) or an integer index '
                        'into the shared palette (0=blue, 1=red-orange, …). '
                        'Default: palette index 0.')
    p.add_argument('--dpi',   type=int, default=150)
    return p.parse_args()

def load_csv(path, sweep_col):
    try:
        df = pd.read_csv(path)
    except Exception as e:
        sys.exit(f'ERROR: cannot read {path}: {e}')
    if sweep_col not in df.columns:
        sys.exit(f'ERROR: expected column "{sweep_col}" in {path}. '
                 f'Found: {list(df.columns)}')
    df = df.sort_values(sweep_col).reset_index(drop=True)
    return df

def build_xaxis(sat, slack, knee):
    """
    Build a unified x-axis where the slack phase's +0W overlaps the knee point.

    Returns (tick_xs, tick_labels, knee_idx, sat_xs, slack_xs).

    Saturation occupies positions 0..n_sat-1.
    Slack occupies positions knee_idx..knee_idx+n_slack-1, so +0W
    lands on the same x-position as the knee thread count.
    """
    sat_threads   = sat['threads'].tolist()
    slack_workers = slack['workers'].tolist()

    # Locate the knee in the saturation thread-count list.
    try:
        knee_idx = sat_threads.index(knee)
    except ValueError:
        # Fallback: closest thread count ≤ knee, or the last point.
        candidates = [i for i, t in enumerate(sat_threads) if t <= knee]
        knee_idx = candidates[-1] if candidates else len(sat_threads) - 1

    n_sat   = len(sat_threads)
    n_slack = len(slack_workers)

    sat_xs   = list(range(n_sat))

    # Slack positions: log₂(w) + 1 offset from knee_idx (for w ≥ 1).
    #   +0W (synthetic) → knee_idx + 0          (sat knee anchor)
    #   +1W             → knee_idx + log₂(1)+1 = knee_idx + 1.000
    #   +2W             → knee_idx + log₂(2)+1 = knee_idx + 2.000  gap = 1.00
    #   +4W             → knee_idx + 3.000                          gap = 1.00
    #   +8W             → knee_idx + 4.000                          gap = 1.00
    #   +16W            → knee_idx + 5.000                          gap = 1.00
    #   +32W            → knee_idx + 6.000                          gap = 1.00
    #   +40W            → knee_idx + 6.322                          gap = 0.32
    #   +48W            → knee_idx + 6.585                          gap = 0.26
    #   +56W            → knee_idx + 6.807                          gap = 0.22
    #   +64W            → knee_idx + 7.000                          gap = 0.19
    # True doublings stay 1 unit apart; sub-doubling steps compress naturally.
    slack_xs = [knee_idx + np.log2(w) + 1 for w in slack_workers]

    # ── Tick labels ──────────────────────────────────────────────────────────
    labels = {}

    # Pre-knee saturation labels.
    for i in range(knee_idx):
        labels[i] = f'{sat_threads[i]}T'

    # Knee point — this is the +0W anchor shared by both phases.
    labels[knee_idx] = f'{knee}T / +0W'

    # All slack workers get their own label at their log₂-scaled position.
    for j in range(n_slack):
        labels[slack_xs[j]] = f'+{slack_workers[j]}W'

    # Any post-knee saturation positions that fall beyond the slack range
    # still need labels (shown in parentheses to signal "post-knee").
    max_slack_x = slack_xs[-1] if slack_xs else knee_idx
    for i in range(knee_idx + 1, n_sat):
        if float(i) > max_slack_x:
            labels[i] = f'({sat_threads[i]}T)'

    tick_xs    = sorted(labels.keys())
    tick_labels = [labels[x] for x in tick_xs]

    return tick_xs, tick_labels, knee_idx, sat_xs, slack_xs

def main():
    args = parse_args()

    # Resolve machine colour from --color (hex string or palette index).
    if args.color is None:
        MC = _PALETTE[0]
    elif args.color.startswith('#'):
        MC = args.color
    else:
        MC = _PALETTE[int(args.color) % len(_PALETTE)]

    # Two-shade scheme: saturation uses the full (darkened) machine colour;
    # slack uses a lighter tint of the same hue.  Post-knee stays gray (faded).
    MC_SAT   = _shade(MC, -0.15)   # 15 % darker  — saturation phase
    MC_SLACK = _shade(MC, +0.40)   # 40 % lighter — slack phase

    sat   = load_csv(args.sat,   'threads')
    slack = load_csv(args.slack, 'workers')

    knee = args.knee if args.knee is not None else int(sat['threads'].max())

    tick_xs, tick_labels, knee_idx, sat_xs, slack_xs = \
        build_xaxis(sat, slack, knee)

    # Convenience slices into the saturation DataFrame.
    pre_end  = knee_idx + 1                     # exclusive — includes knee row
    pre_xs   = sat_xs[:pre_end]                 # positions for pre-knee (coloured)
    post_xs  = sat_xs[knee_idx:]                # positions for post-knee (faded)

    # ── Augmented slack x/y arrays that treat the sat knee as +0W ────────────
    # The slack line is drawn starting at the knee point (sat data) and then
    # through each slack data point, giving a continuous connection.
    knee_row = sat.iloc[knee_idx]
    slack_xs_aug = [knee_idx] + list(slack_xs)   # prepend the +0W position

    def _aug_slack(slack_col, sat_col, fill_zero=False):
        """Slack array prepended with the sat knee value as the +0W anchor."""
        getter = _seg0 if fill_zero else _seg
        arr = getter(slack, slack_col)
        knee_val = float(knee_row[sat_col]) if sat_col in sat.columns else np.nan
        return np.concatenate([[knee_val], arr])

    # ── figure layout ─────────────────────────────────────────────────────────
    plt.rcParams.update({
        'figure.dpi'        : args.dpi,
        'font.family'       : 'sans-serif',
        'font.size'         : 15,
        'axes.spines.top'   : False,
        'axes.spines.right' : False,
        'axes.grid'         : True,
        'grid.alpha'        : 0.3,
        'axes.labelsize'    : 16,
        'xtick.labelsize'   : 12,
        'ytick.labelsize'   : 12,
        'axes.titlesize'    : 16,
        'legend.fontsize'   : 14,
    })

    fig = plt.figure(figsize=(14, 9))   # reduced from 12 → 10
    if args.title:
        fig.suptitle(args.title, fontsize=14, fontweight='bold', y=0.98)
    gs  = gridspec.GridSpec(3, 1, hspace=0.21, top=0.93, bottom=0.10)  # tighter hspace
    ax_xput = fig.add_subplot(gs[0])
    ax_disk = fig.add_subplot(gs[1], sharex=ax_xput)
    ax_cpu  = fig.add_subplot(gs[2], sharex=ax_xput)

    # ── helpers for phased segments ───────────────────────────────────────────
    def _seg(df, name, slc=None):
        """Column values as float numpy array, optionally sliced."""
        v = col(df, name).values.astype(float)
        return v[slc] if slc is not None else v

    def _seg0(df, name, slc=None):
        """Like _seg but fills NaN with 0 (for std-dev columns)."""
        v = _seg(df, name, slc)
        return np.where(np.isfinite(v), v, 0.0)

    def _phase(ax, xs, mean, std, color, fmt='o-', label=None,
               lo=None, hi=None, band_alpha=0.18, minmax_alpha=0.10):
        """Plot one segment: errorbars + std band + optional min/max band."""
        errbar(ax, xs, mean, std, color, label=label, fmt=fmt)
        s = np.where(np.isfinite(std), std, 0.0)
        m = np.asarray(mean, dtype=float)
        band(ax, xs, m - s, m + s, color, alpha=band_alpha)
        if lo is not None and hi is not None:
            band(ax, xs, lo, hi, color, alpha=minmax_alpha)

    # ── Panel 1: Throughput ───────────────────────────────────────────────────
    ax = ax_xput

    # Pre-knee saturation — darker shade
    _phase(ax, pre_xs,
           _seg(sat, 'xput_mean', slice(0, pre_end)),
           _seg0(sat, 'xput_std',  slice(0, pre_end)),
           MC_SAT,
           lo=_seg(sat, 'xput_min', slice(0, pre_end)),
           hi=_seg(sat, 'xput_max', slice(0, pre_end)))

    # Post-knee saturation (dashed gray)
    _phase(ax, post_xs,
           _seg(sat, 'xput_mean', slice(knee_idx, None)),
           _seg0(sat, 'xput_std',  slice(knee_idx, None)),
           C_FADE, fmt='o--', band_alpha=0.10, minmax_alpha=0.06)

    # Slack phase — lighter tint, starts from sat knee as +0W anchor
    _phase(ax, slack_xs_aug,
           _aug_slack('xput_mean', 'xput_mean'),
           _aug_slack('xput_std',  'xput_std',  fill_zero=True),
           MC_SLACK,
           lo=_aug_slack('xput_min', 'xput_min'),
           hi=_aug_slack('xput_max', 'xput_max'))

    add_divider(ax, knee_idx)
    ax.set_ylabel('Throughput (QPS)')
    ax.set_title('Query Throughput', fontweight='bold')
    ax.yaxis.set_major_formatter(
        mticker.FuncFormatter(lambda x, _: f'{x/1000:.0f}K' if x >= 1000 else f'{x:.0f}')
    )
    # No legend on panel 1.

    # ── Panel 2: Disk BW utilization (disk_bw_avg × 100) ─────────────────────
    ax = ax_disk

    # Pre-knee saturation — darker shade
    _phase(ax, pre_xs,
           _seg(sat, 'disk_bw_avg', slice(0, pre_end)) * 100,
           _seg0(sat, 'disk_bw_stddev', slice(0, pre_end)) * 100,
           MC_SAT)

    # Post-knee saturation (faded)
    _phase(ax, post_xs,
           _seg(sat, 'disk_bw_avg', slice(knee_idx, None)) * 100,
           _seg0(sat, 'disk_bw_stddev', slice(knee_idx, None)) * 100,
           C_FADE, fmt='o--', band_alpha=0.10)

    # Slack — lighter tint, starts from sat knee as +0W anchor
    _phase(ax, slack_xs_aug,
           _aug_slack('disk_bw_avg',    'disk_bw_avg')    * 100,
           _aug_slack('disk_bw_stddev', 'disk_bw_stddev', fill_zero=True) * 100,
           MC_SLACK)

    add_divider(ax, knee_idx)
    ax.set_ylabel('Disk BW utilization (%)')
    ax.set_ylim(bottom=0)
    ax.set_title('Disk Bandwidth', fontweight='bold')
    # No legend on panel 2.

    # ── Panel 3: CPU utilization ──────────────────────────────────────────────
    ax = ax_cpu

    # Pre-knee saturation — compute (triangle) and schedule (square), darker shade.
    cc_pre = _seg(sat, 'cpu_compute_mean',  slice(0, pre_end))
    cs_pre = _seg(sat, 'cpu_schedule_mean', slice(0, pre_end))
    _phase(ax, pre_xs, cc_pre,
           _seg0(sat, 'cpu_compute_std', slice(0, pre_end)), MC_SAT, fmt='^-')
    _phase(ax, pre_xs, cs_pre,
           _seg0(sat, 'cpu_schedule_std', slice(0, pre_end)), MC_SAT, fmt='s-',
           band_alpha=0.10)
    ax.fill_between(pre_xs, cc_pre, cs_pre,
                    facecolor='none', edgecolor=MC_SAT,
                    hatch='xxx', alpha=0.30, linewidth=0, zorder=2)

    # Post-knee saturation — compute and schedule, gray dashed.
    cc_post = _seg(sat, 'cpu_compute_mean',  slice(knee_idx, None))
    cs_post = _seg(sat, 'cpu_schedule_mean', slice(knee_idx, None))
    _phase(ax, post_xs, cc_post,
           _seg0(sat, 'cpu_compute_std', slice(knee_idx, None)),
           C_FADE, fmt='^--', band_alpha=0.06)
    _phase(ax, post_xs, cs_post,
           _seg0(sat, 'cpu_schedule_std', slice(knee_idx, None)),
           C_FADE, fmt='s--', band_alpha=0.06)

    # Slack — lighter tint. Starts from sat knee as +0W anchor.
    cc_slk = _aug_slack('cpu_compute_mean',  'cpu_compute_mean')
    cs_slk = _aug_slack('cpu_schedule_mean', 'cpu_schedule_mean')
    _phase(ax, slack_xs_aug, cc_slk,
           _aug_slack('cpu_compute_std',  'cpu_compute_std',  fill_zero=True),
           MC_SLACK, fmt='^-')
    _phase(ax, slack_xs_aug, cs_slk,
           _aug_slack('cpu_schedule_std', 'cpu_schedule_std', fill_zero=True),
           MC_SLACK, fmt='s-', band_alpha=0.10)
    ax.fill_between(slack_xs_aug, cc_slk, cs_slk,
                    facecolor='none', edgecolor=MC_SLACK,
                    hatch='xxx', alpha=0.30, linewidth=0, zorder=2)

    add_divider(ax, knee_idx)
    ax.set_ylabel('CPU utilization (%)')
    ax.set_ylim(bottom=0, top=105)
    ax.set_title('CPU Utilization', fontweight='bold')

    # Panel 3 legend — explains metric markers + phase encoding.
    from matplotlib.patches import Patch
    C_LEG = '#555555'   # neutral gray for legend icons
    legend_handles = [
        Line2D([0],[0], color=C_LEG, lw=2, marker='s', ms=5, ls='-',
               label='CPU compute+iowait'),
        Line2D([0],[0], color=C_LEG, lw=2, marker='^', ms=5, ls='-',
               label='CPU compute'),
        Patch(facecolor='none', edgecolor=C_LEG, hatch='xxx', linewidth=0.5,
              label='I/O wait (gap)'),
        Line2D([0],[0], color=C_FADE, lw=2, ls='--',
               label='post-knee (faded)'),
    ]
    ax.legend(handles=legend_handles, loc='upper left', ncol=2)

    # ── Shared x-axis labels ──────────────────────────────────────────────────
    set_xticks(ax_cpu, tick_xs, tick_labels)
    ax_cpu.set_xlabel(
        f'\u2190 YCSB threads (saturation)   |   '
        f'iBench workers added at {knee}T (slack) \u2192',
        fontsize=18,
        x=0.465
    )
    plt.setp(ax_xput.get_xticklabels(), visible=False)
    plt.setp(ax_disk.get_xticklabels(), visible=False)

    # ── Knee annotation (top panel) ──────────────────────────────────────────
    ax_xput.annotate(
        f'Knee\n{knee}T / +0W',
        xy=(knee_idx, ax_xput.get_ylim()[0]),
        xytext=(knee_idx + 0.5,
                ax_xput.get_ylim()[0] +
                0.08 * (ax_xput.get_ylim()[1] - ax_xput.get_ylim()[0])),
        fontsize=11, color='gray',
        arrowprops=dict(arrowstyle='->', color='gray', lw=0.8),
    )

    fig.savefig(args.out, bbox_inches='tight', dpi=args.dpi)
    print(f'Saved \u2192 {args.out}')

if __name__ == '__main__':
    main()
