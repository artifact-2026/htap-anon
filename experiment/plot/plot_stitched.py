#!/usr/bin/env python3
"""
plot_stitched.py — Multi-machine stitched saturation + IO/CPU-slack figure
==========================================================================

Produces an N × 2 panel figure where each ROW is one machine and the two
COLUMNS are:

  Col 0 — Disk IO (read + write MB/s)   [from --io]   + QPS overlay (right axis)
  Col 1 — CPU utilization (%)            [from --slack] + QPS overlay (right axis)

Each row splices a saturation sweep and a slack sweep onto a shared x-axis.
The knee point is the divider: left = YCSB threads, right = iBench workers.

Usage
-----
  python3 plot_stitched.py \\
      --sat   machine_a/sat.csv   \\
      --io    machine_a/io.csv    \\
      --slack machine_a/slack.csv \\
      --knee 96 --label "Machine A" \\
      --sat   machine_b/sat.csv   \\
      --io    machine_b/io.csv    \\
      --slack machine_b/slack.csv \\
      --knee 40 --label "Machine B" \\
      [--out  stitched.pdf] \\
      [--title "All-machine comparison"] \\
      [--dpi 150]

For a single machine you may provide just one --sat / --io / --slack / --knee tuple.
--knee defaults to the maximum thread count found in the saturation CSV.
--label defaults to the saturation CSV file stem.

Column contract
---------------
saturation summary.csv : threads, xput_mean, xput_std, [xput_min, xput_max],
                         cpu_compute_mean/std/min/max,
                         cpu_schedule_mean/std/min/max,
                         disk_bw_avg (fraction 0-1), disk_bw_stddev

io summary.csv         : workers, xput_mean, xput_std, [xput_min, xput_max],
                         disk_bw_avg (fraction 0-1), disk_bw_stddev (fraction 0-1),
                         (other columns ignored)

slack summary.csv      : workers, xput_mean, xput_std, [xput_min, xput_max],
                         cpu_compute_mean/std/min/max,
                         cpu_schedule_mean/std/min/max
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

C_FADE   = '#bbbbbb'   # light gray — post-knee saturation (faded)
C_LEG    = '#444444'   # neutral gray — legend icons
QPS_C    = '#e08020'   # warm orange — QPS overlay on both panels


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
           lo=None, hi=None, band_alpha=0.18, minmax_alpha=0.10, label=None):
    """Plot a phased segment with error bars + std band + optional min/max band."""
    _errbar(ax, xs, mean, std, color, fmt=fmt, lw=lw, ms=ms, label=label)
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

def _fmt_kps(v, _):
    return f'{v/1000:.0f}K' if v >= 1000 else f'{v:.0f}'


# ── x-axis construction ──────────────────────────────────────────────────────

def build_xaxis(sat, slack, knee):
    """
    Returns (tick_xs, tick_labels, knee_idx, sat_xs, slack_xs).

    Saturation: integer positions 0..n_sat-1.
    Slack: log₂-spaced positions anchored at knee_idx (+0 W).
    """
    sat_threads   = sat['threads'].tolist()
    # Strip workers==0 — it is the knee anchor, not a sweep point, and
    # log2(0) = -inf which breaks axis limits.
    slack_workers = [w for w in slack['workers'].tolist() if w > 0]

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
    labels = {}
    for i in range(knee_idx + 1):
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

def _draw_row(ax_io, ax_io_r, ax_cpu, ax_cpu_r,
              sat, io_df, slack, knee, mc, row_label, fsz,
              lw=2.0, ms=6, show_xticks=True):
    """
    Draw one machine row into 4 axes:
      ax_io   — IO panel primary  (disk_bw_avg as %)
      ax_io_r — IO panel secondary (QPS, right y-axis)
      ax_cpu  — CPU panel primary  (cpu_compute + cpu_schedule)
      ax_cpu_r— CPU panel secondary (QPS, right y-axis)

    The canonical x-axis is built from sat + slack.  io_df is plotted at
    log₂-scaled positions derived from its own workers column.
    """
    # ── X-axis construction (canonical: sat + slack) ─────────────────────────
    tick_xs, tick_labels, knee_idx, sat_xs, slack_xs = \
        build_xaxis(sat, slack, knee)

    pre_end = knee_idx + 1
    pre_xs  = sat_xs[:pre_end]

    # For both io and slack: workers==0 is the baseline / knee anchor.
    # workers > 0 form the actual sweep.  log2(0) = -inf, so we must exclude
    # workers==0 from position calculations.
    def _split_sweep(df):
        base  = df[df['workers'] == 0]
        sweep = df[df['workers'] >  0].reset_index(drop=True)
        baseline = base.iloc[0] if len(base) > 0 else None
        if len(sweep) == 0:          # no workers==0 row → treat all as sweep
            sweep    = df.copy()
            baseline = None
        return sweep, baseline

    io_df_sweep,    io_baseline    = _split_sweep(io_df)
    slack_df_sweep, slack_baseline = _split_sweep(slack)

    io_xs = [knee_idx + np.log2(w) + 1
             for w in io_df_sweep['workers'].tolist()]
    io_xs_aug    = [knee_idx] + list(io_xs)
    slack_xs_aug = [knee_idx] + list(slack_xs)

    MC_SAT   = _shade(mc, -0.15)
    MC_SLACK = _shade(mc, +0.40)

    sat_knee_row = sat.iloc[knee_idx]

    # ── helpers ───────────────────────────────────────────────────────────────
    def _seg(df, name, slc=None):
        v = _col(df, name).values.astype(float)
        return v[slc] if slc is not None else v

    def _seg0(df, name, slc=None):
        v = _seg(df, name, slc)
        return np.where(np.isfinite(v), v, 0.0)

    def _aug_io(col, zero=False):
        """Augment io sweep array with baseline (workers==0) value at knee."""
        getter = _seg0 if zero else _seg
        arr = getter(io_df_sweep, col)
        if io_baseline is not None and col in io_baseline.index:
            knee_val = float(io_baseline[col])
        else:
            knee_val = np.nan
        return np.concatenate([[knee_val], arr])

    def _aug_slack(col, zero=False):
        """Augment slack sweep (workers>0) with workers==0 baseline at knee.
        Falls back to sat knee row if no workers==0 row exists in slack."""
        getter = _seg0 if zero else _seg
        arr = getter(slack_df_sweep, col)
        if slack_baseline is not None and col in slack_baseline.index:
            knee_val = float(slack_baseline[col])
        elif col in sat.columns:
            knee_val = float(sat_knee_row[col])
        else:
            knee_val = np.nan
        return np.concatenate([[knee_val], arr])

    # ── IO Panel — primary: disk_bw_avg (%) ──────────────────────────────────
    ax = ax_io

    def _pct(arr):
        """Convert fraction 0-1 to percent, preserving NaN."""
        a = np.array(arr, dtype=float)
        return np.where(np.isfinite(a), a * 100.0, np.nan)

    # Saturation phase (pre-knee)
    _phase(ax, pre_xs,
           _pct(_seg(sat,  'disk_bw_avg',    slice(0, pre_end))),
           _pct(_seg0(sat, 'disk_bw_stddev', slice(0, pre_end))),
           MC_SAT, fmt='x-', lw=lw, ms=ms)

    # IO-slack phase
    _phase(ax, io_xs_aug,
           _pct(_aug_io('disk_bw_avg')),
           _pct(_aug_io('disk_bw_stddev', zero=True)),
           _shade(mc, +0.40), fmt='x-', lw=lw, ms=ms)

    _divider(ax, knee_idx)
    ax.set_ylim(0, 110)
    ax.axhline(100, color='black', linestyle=':', linewidth=0.8, alpha=0.4)
    ax.set_ylabel(row_label, fontsize=fsz + 1, fontweight='bold', labelpad=6)

    # ── IO Panel — secondary: QPS (right y-axis) ─────────────────────────────
    ax = ax_io_r
    QPS_SAT   = _shade(QPS_C, -0.10)
    QPS_SLACK = _shade(QPS_C, +0.30)

    _phase(ax, pre_xs,
           _seg(sat, 'xput_mean', slice(0, pre_end)),
           _seg0(sat, 'xput_std',  slice(0, pre_end)),
           QPS_SAT, fmt='D--', lw=lw * 0.8, ms=ms * 0.8)
    _phase(ax, io_xs_aug,
           _aug_io('xput_mean'),
           _aug_io('xput_std', zero=True),
           QPS_SLACK, fmt='D--', lw=lw * 0.8, ms=ms * 0.8,
           lo=_aug_io('xput_min'),
           hi=_aug_io('xput_max'))

    ax.set_ylim(bottom=0)
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(_fmt_kps))
    ax.set_ylabel('QPS', fontsize=fsz, color=QPS_C, labelpad=4)
    ax.tick_params(axis='y', colors=QPS_C, labelsize=fsz - 1)
    ax.spines['right'].set_color(QPS_C)

    # ── CPU Panel — primary: cpu_compute + cpu_schedule ───────────────────────
    ax = ax_cpu
    cc_pre = _seg(sat, 'cpu_compute_mean',  slice(0, pre_end))
    cs_pre = _seg(sat, 'cpu_schedule_mean', slice(0, pre_end))
    cc_slk = _aug_slack('cpu_compute_mean')
    cs_slk = _aug_slack('cpu_schedule_mean')

    _phase(ax, pre_xs, cc_pre,
           _seg0(sat, 'cpu_compute_std',  slice(0, pre_end)),
           MC_SAT, fmt='^-', lw=lw, ms=ms)
    _phase(ax, pre_xs, cs_pre,
           _seg0(sat, 'cpu_schedule_std', slice(0, pre_end)),
           MC_SAT, fmt='s-', lw=lw, ms=ms, band_alpha=0.10)
    ax.fill_between(pre_xs, cc_pre, cs_pre,
                    facecolor='none', edgecolor=MC_SAT,
                    hatch='xxx', alpha=0.30, linewidth=0, zorder=2)

    _phase(ax, slack_xs_aug, cc_slk,
           _aug_slack('cpu_compute_std',  zero=True),
           MC_SLACK, fmt='^-', lw=lw, ms=ms)
    _phase(ax, slack_xs_aug, cs_slk,
           _aug_slack('cpu_schedule_std', zero=True),
           MC_SLACK, fmt='s-', lw=lw, ms=ms, band_alpha=0.10)
    ax.fill_between(slack_xs_aug, cc_slk, cs_slk,
                    facecolor='none', edgecolor=MC_SLACK,
                    hatch='xxx', alpha=0.30, linewidth=0, zorder=2)

    _divider(ax, knee_idx)
    ax.set_ylim(bottom=0, top=105)
    ax.axhline(100, color='black', linestyle=':', linewidth=0.8, alpha=0.4)

    # ── CPU Panel — secondary: QPS (right y-axis) ────────────────────────────
    ax = ax_cpu_r
    _phase(ax, pre_xs,
           _seg(sat, 'xput_mean', slice(0, pre_end)),
           _seg0(sat, 'xput_std',  slice(0, pre_end)),
           QPS_SAT, fmt='D--', lw=lw * 0.8, ms=ms * 0.8)
    _phase(ax, slack_xs_aug,
           _aug_slack('xput_mean'),
           _aug_slack('xput_std', zero=True),
           QPS_SLACK, fmt='D--', lw=lw * 0.8, ms=ms * 0.8,
           lo=_aug_slack('xput_min'),
           hi=_aug_slack('xput_max'))

    ax.set_ylim(bottom=0)
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(_fmt_kps))
    ax.set_ylabel('QPS', fontsize=fsz, color=QPS_C, labelpad=4)
    ax.tick_params(axis='y', colors=QPS_C, labelsize=fsz - 1)
    ax.spines['right'].set_color(QPS_C)

    # ── Shared x-axis ticks (primary axes only; twinx shares x-range) ────────
    for a in (ax_io, ax_cpu):
        if show_xticks:
            _set_xticks(a, tick_xs, tick_labels)
        else:
            a.set_xticks(tick_xs)
            a.set_xlim(tick_xs[0] - 0.7, tick_xs[-1] + 0.7)
            plt.setp(a.get_xticklabels(), visible=False)

    # ── Knee annotations on both primary panels ───────────────────────────────
    for a in (ax_io, ax_cpu):
        yl = a.get_ylim()
        a.annotate(
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
        description='N × 2 stitched saturation + IO/CPU-slack figure (one row per machine).',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    p.add_argument('--sat',   action='append', required=True, metavar='PATH',
                   help='Saturation CSV.  Repeat once per machine.')
    p.add_argument('--io',    action='append', required=True, metavar='PATH',
                   help='IO-slack CSV (disk_bw_avg, disk_bw_stddev as fractions 0-1, xput_mean, …).  '
                        'Repeat once per machine.')
    p.add_argument('--slack', action='append', required=True, metavar='PATH',
                   help='CPU-slack CSV.  Repeat once per machine.')
    p.add_argument('--knee',  action='append', type=int,   metavar='N',
                   default=None,
                   help='Knee thread count for each machine (same order as --sat). '
                        'Omit to use max threads in that CSV.  '
                        'Repeat once per machine, or once for all.')
    p.add_argument('--label', action='append', metavar='LABEL',
                   default=None,
                   help='Row label for each machine.  Default: CSV file stem.  '
                        'Repeat per machine.')
    p.add_argument('--out',   default='stitched.pdf', help='Output file path.')
    p.add_argument('--title', default='',  help='Optional figure suptitle.')
    p.add_argument('--width', type=float,  default=10.0, help='Figure width in inches.')
    p.add_argument('--row-height', type=float, dest='row_height', default=2.5,
                   help='Height per machine row in inches.  Default: 2.5.')
    p.add_argument('--font-size', type=int, dest='font_size', default=11,
                   help='Base font size.  Default: 11.')
    p.add_argument('--dpi',   type=int,   default=150)
    return p.parse_args()


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    args = parse_args()

    # ── Validate argument lists ───────────────────────────────────────────────
    n = len(args.sat)
    if len(args.io) != n:
        sys.exit(f'ERROR: {n} --sat but {len(args.io)} --io args. '
                 f'Provide one of each per machine.')
    if len(args.slack) != n:
        sys.exit(f'ERROR: {n} --sat but {len(args.slack)} --slack args. '
                 f'Provide one of each per machine.')

    # Normalise --knee (accept fewer than n → broadcast last value).
    knees_raw = args.knee or []
    if len(knees_raw) == 0:
        knees = [None] * n
    elif len(knees_raw) == 1:
        knees = knees_raw * n
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
        io_df = _load(args.io[i],    'workers')
        slack = _load(args.slack[i], 'workers')
        knee  = knees[i] if knees[i] is not None else int(sat['threads'].max())
        mc    = _PALETTE[i % len(_PALETTE)]
        machines.append(dict(sat=sat, io=io_df, slack=slack, knee=knee,
                             label=labels[i], mc=mc))
        print(f'[{labels[i]}]  sat={len(sat)} pts, io={len(io_df)} pts, '
              f'slack={len(slack)} pts, knee={knee}T, color={mc}')

    # ── Figure layout: N rows × 2 cols ───────────────────────────────────────
    fsz = args.font_size
    plt.rcParams.update({
        'figure.dpi'        : args.dpi,
        'font.family'       : 'sans-serif',
        'font.size'         : fsz,
        'axes.spines.top'   : False,
        'axes.spines.right' : False,   # twinx axes re-enable their own right spine
        'axes.grid'         : True,
        'grid.alpha'        : 0.28,
        'axes.labelsize'    : fsz,
        'xtick.labelsize'   : fsz - 1,
        'ytick.labelsize'   : fsz - 1,
        'axes.titlesize'    : fsz + 1,
        'legend.fontsize'   : fsz,
    })

    COL_TITLES = ['Disk BW util. (%)  ·  QPS →', 'CPU util. (%)  ·  QPS →']

    fig_h = args.row_height * n + (1.0 if args.title else 0.5)
    fig   = plt.figure(figsize=(args.width, fig_h))

    if args.title:
        fig.suptitle(args.title, fontsize=fsz + 3, fontweight='bold', y=0.995)

    # Reserve bottom space for the shared legend.
    LEGEND_H = 0.09
    gs = gridspec.GridSpec(
        n, 2, figure=fig,
        hspace=0.40, wspace=0.45,   # extra wspace for right-axis labels
        top=0.96 if not args.title else 0.94,
        bottom=LEGEND_H + 0.03,
        left=0.10, right=0.95,
    )

    for i, m in enumerate(machines):
        ax0  = fig.add_subplot(gs[i, 0])
        ax0r = ax0.twinx()
        ax1  = fig.add_subplot(gs[i, 1])
        ax1r = ax1.twinx()

        # Column titles on the first row only.
        if i == 0:
            ax0.set_title(COL_TITLES[0], fontsize=fsz + 1, fontweight='bold', pad=8)
            ax1.set_title(COL_TITLES[1], fontsize=fsz + 1, fontweight='bold', pad=8)

        _draw_row(ax0, ax0r, ax1, ax1r,
                  m['sat'], m['io'], m['slack'], m['knee'],
                  m['mc'], m['label'], fsz,
                  show_xticks=True)

    # ── Shared legend ────────────────────────────────────────────────────────
    mc_ex = _PALETTE[0]   # example colour for legend icons
    legend_handles = [
        # Phase encoding
        Line2D([0],[0], color=_shade(mc_ex,-0.15), lw=2.5, ls='-',
               label='Saturation (pre-knee)'),
        Line2D([0],[0], color=_shade(mc_ex,+0.40), lw=2.5, ls='-',
               label='Slack phase (lighter)'),
        # X-axis notation
        Line2D([0],[0], color='none', label='T = YCSB threads'),
        Line2D([0],[0], color='none', label='W = slack workers'),
        # IO panel metric encoding
        Line2D([0],[0], color=C_LEG, lw=2, marker='x', ms=5, ls='-',
               label='Disk BW util. (%)'),
        # CPU panel metric encoding
        Line2D([0],[0], color=C_LEG, lw=2, marker='^', ms=5, ls='-',
               label='CPU compute'),
        Line2D([0],[0], color=C_LEG, lw=2, marker='s', ms=5, ls='-',
               label='CPU scheduled'),
        # QPS overlay
        Line2D([0],[0], color=QPS_C, lw=2, marker='D', ms=5, ls='--',
               label='QPS (right axis)'),
    ]
    fig.legend(
        handles=legend_handles,
        loc='lower center',
        bbox_to_anchor=(0.5, 0.0),
        ncol=5,
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
