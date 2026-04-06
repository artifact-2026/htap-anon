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

# ── colour palette ─────────────────────────────────────────────────────────────
C_SAT      = '#2196F3'   # blue  — saturation phase
C_SLACK    = '#E91E63'   # pink  — slack phase
C_READ     = '#00BCD4'   # cyan  — disk read
C_WRITE    = '#FF9800'   # amber — disk write
C_COMPUTE  = '#4CAF50'   # green — cpu_compute (excl. iowait)
C_SCHEDULE = '#9C27B0'   # purple — cpu_schedule (incl. iowait)
C_BAND     = 0.15        # alpha for min/max fill bands

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
    ax.axvline(x_div, color='gray', linestyle='--', linewidth=1.2, alpha=0.7)

def set_xticks(ax, xs, labels, phase_boundary_x, rotate=True):
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, fontsize=8,
                       rotation=45 if rotate else 0, ha='right' if rotate else 'center')
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

def build_xaxis(sat, slack, knee_threads):
    """
    Returns (all_xs, all_labels, divider_x, sat_xs, slack_xs).

    X positions are integers: 0..n_sat-1 for saturation,
    then n_sat..n_sat+n_slack-1 for slack.
    A gap of 1 unit is inserted between the phases for the divider line.
    Labels: saturation side uses thread count ("Nt"), slack side uses worker count
    ("+Nw") to make it clear that these are additive workers, NOT YCSB threads.
    """
    sat_threads  = sat['threads'].tolist()
    slack_workers = slack['workers'].tolist()

    n_sat   = len(sat_threads)
    n_slack = len(slack_workers)

    GAP = 1  # extra unit between phases

    sat_xs   = list(range(n_sat))
    slack_xs = list(range(n_sat + GAP, n_sat + GAP + n_slack))

    divider_x = n_sat - 1 + GAP / 2.0   # midpoint of gap

    sat_labels   = [f'{t}T'    for t in sat_threads]
    slack_labels = [f'+{w}W'   for w in slack_workers]

    all_xs     = sat_xs + slack_xs
    all_labels = sat_labels + slack_labels

    return all_xs, all_labels, divider_x, sat_xs, slack_xs

def main():
    args = parse_args()

    sat   = load_csv(args.sat,   'threads')
    slack = load_csv(args.slack, 'workers')

    knee = args.knee if args.knee is not None else int(sat['threads'].max())

    all_xs, all_labels, div_x, sat_xs, slack_xs = build_xaxis(sat, slack, knee)

    # ── figure layout ─────────────────────────────────────────────────────────
    plt.rcParams.update({
        'figure.dpi'        : args.dpi,
        'font.family'       : 'sans-serif',
        'font.size'         : 11,
        'axes.spines.top'   : False,
        'axes.spines.right' : False,
        'axes.grid'         : True,
        'grid.alpha'        : 0.3,
        'axes.labelsize'    : 11,
        'xtick.labelsize'   : 9,
        'ytick.labelsize'   : 10,
    })

    fig = plt.figure(figsize=(14, 12))
    if args.title:
        fig.suptitle(args.title, fontsize=13, fontweight='bold', y=0.98)
    gs  = gridspec.GridSpec(3, 1, hspace=0.55, top=0.93, bottom=0.08)
    ax_xput = fig.add_subplot(gs[0])
    ax_disk = fig.add_subplot(gs[1], sharex=ax_xput)
    ax_cpu  = fig.add_subplot(gs[2], sharex=ax_xput)

    # ── Panel 1: Throughput ───────────────────────────────────────────────────
    ax = ax_xput

    # Saturation phase
    sat_lo  = col(sat, 'xput_min');  sat_hi  = col(sat, 'xput_max')
    band(ax, sat_xs,
         col(sat,'xput_mean') - col(sat,'xput_std', pd.Series([0]*len(sat))),
         col(sat,'xput_mean') + col(sat,'xput_std', pd.Series([0]*len(sat))),
         C_SAT, alpha=0.18)
    band(ax, sat_xs, sat_lo, sat_hi, C_SAT, alpha=0.10)
    errbar(ax, sat_xs, col(sat,'xput_mean'), col(sat,'xput_std'),
           C_SAT, label=f'Saturation (YCSB threads, knee={knee}T)')

    # Slack phase
    slk_lo = col(slack, 'xput_min');  slk_hi = col(slack, 'xput_max')
    band(ax, slack_xs,
         col(slack,'xput_mean') - col(slack,'xput_std', pd.Series([0]*len(slack))),
         col(slack,'xput_mean') + col(slack,'xput_std', pd.Series([0]*len(slack))),
         C_SLACK, alpha=0.18)
    band(ax, slack_xs, slk_lo, slk_hi, C_SLACK, alpha=0.10)
    errbar(ax, slack_xs, col(slack,'xput_mean'), col(slack,'xput_std'),
           C_SLACK, label=f'CPU slack ({knee}T + N iBench workers)')

    add_divider(ax, div_x)
    ax.set_ylabel('Throughput (ops/s)')
    ax.set_title('Write Throughput', fontsize=11, fontweight='bold')
    ax.legend(fontsize=9, loc='upper left')

    # Phase labels inside the plot
    sat_mid_x  = np.mean(sat_xs)
    slk_mid_x  = np.mean(slack_xs)
    ylim = ax.get_ylim()
    ax.text(sat_mid_x,  ylim[1] * 0.95, 'Saturation phase',
            ha='center', va='top', fontsize=8, color='gray', style='italic')
    ax.text(slk_mid_x,  ylim[1] * 0.95, 'Slack phase',
            ha='center', va='top', fontsize=8, color='gray', style='italic')

    # ── Panel 2: Disk bandwidth ───────────────────────────────────────────────
    ax = ax_disk

    for df, xs, phase_alpha in [(sat, sat_xs, 0.15), (slack, slack_xs, 0.15)]:
        # Read
        dr_mean = col(df, 'disk_read_mb/s')
        dr_std  = col(df, 'disk_read_mb/s_std')
        dr_lo   = col(df, 'disk_read_mb/s_min')
        dr_hi   = col(df, 'disk_read_mb/s_max')
        band(ax, xs, dr_lo, dr_hi, C_READ, alpha=phase_alpha)
        band(ax, xs, dr_mean - dr_std, dr_mean + dr_std, C_READ, alpha=0.20)

        # Write
        dw_mean = col(df, 'disk_write_mb/s')
        dw_std  = col(df, 'disk_write_mb/s_std')
        dw_lo   = col(df, 'disk_write_mb/s_min')
        dw_hi   = col(df, 'disk_write_mb/s_max')
        band(ax, xs, dw_lo, dw_hi, C_WRITE, alpha=phase_alpha)
        band(ax, xs, dw_mean - dw_std, dw_mean + dw_std, C_WRITE, alpha=0.20)

    # Plot lines once per colour with labels (sat + slack share colour → one legend entry)
    errbar(ax, sat_xs + slack_xs,
           list(col(sat,'disk_read_mb/s'))  + list(col(slack,'disk_read_mb/s')),
           list(col(sat,'disk_read_mb/s_std')) + list(col(slack,'disk_read_mb/s_std')),
           C_READ,  label='Disk read')
    errbar(ax, sat_xs + slack_xs,
           list(col(sat,'disk_write_mb/s'))  + list(col(slack,'disk_write_mb/s')),
           list(col(sat,'disk_write_mb/s_std')) + list(col(slack,'disk_write_mb/s_std')),
           C_WRITE, label='Disk write')

    add_divider(ax, div_x)
    ax.set_ylabel('Bandwidth (MB/s)')
    ax.set_title('Disk Bandwidth', fontsize=11, fontweight='bold')
    ax.legend(fontsize=9, loc='upper left')

    # ── Panel 3: CPU utilization ──────────────────────────────────────────────
    ax = ax_cpu

    for df, xs in [(sat, sat_xs), (slack, slack_xs)]:
        # cpu_schedule (incl. iowait) — upper envelope
        cs_mean = col(df, 'cpu_schedule_mean')
        cs_std  = col(df, 'cpu_schedule_std')
        cs_lo   = col(df, 'cpu_schedule_min')
        cs_hi   = col(df, 'cpu_schedule_max')
        band(ax, xs, cs_lo, cs_hi, C_SCHEDULE, alpha=0.12)
        band(ax, xs, cs_mean - cs_std, cs_mean + cs_std, C_SCHEDULE, alpha=0.20)

        # cpu_compute (excl. iowait) — lower line below schedule
        cc_mean = col(df, 'cpu_compute_mean')
        cc_std  = col(df, 'cpu_compute_std')
        cc_lo   = col(df, 'cpu_compute_min')
        cc_hi   = col(df, 'cpu_compute_max')
        band(ax, xs, cc_lo, cc_hi, C_COMPUTE, alpha=0.12)
        band(ax, xs, cc_mean - cc_std, cc_mean + cc_std, C_COMPUTE, alpha=0.20)

    errbar(ax, sat_xs + slack_xs,
           list(col(sat,'cpu_schedule_mean')) + list(col(slack,'cpu_schedule_mean')),
           list(col(sat,'cpu_schedule_std'))  + list(col(slack,'cpu_schedule_std')),
           C_SCHEDULE, label='CPU busy (incl. iowait, 100−idle)')
    errbar(ax, sat_xs + slack_xs,
           list(col(sat,'cpu_compute_mean')) + list(col(slack,'cpu_compute_mean')),
           list(col(sat,'cpu_compute_std'))  + list(col(slack,'cpu_compute_std')),
           C_COMPUTE, label='CPU compute (excl. iowait, 100−idle−iowait)')

    add_divider(ax, div_x)
    ax.set_ylabel('CPU utilization (%)')
    ax.set_ylim(bottom=0, top=105)
    ax.set_title('CPU Utilization', fontsize=11, fontweight='bold')
    ax.legend(fontsize=9, loc='upper left')

    # ── Shared x-axis labels ──────────────────────────────────────────────────
    # Apply to the bottom panel only (sharex handles the rest).
    set_xticks(ax_cpu, all_xs, all_labels, div_x)
    ax_cpu.set_xlabel(
        f'← YCSB threads (saturation)   |   iBench workers added at {knee}T (slack) →'
    )
    # Hide tick labels on the upper panels (sharex will propagate them anyway
    # but we only want text on the bottom one).
    plt.setp(ax_xput.get_xticklabels(), visible=False)
    plt.setp(ax_disk.get_xticklabels(), visible=False)

    # ── Phase-boundary annotation (top panel) ────────────────────────────────
    ax_xput.annotate(
        f'Knee\n{knee}T / 0W',
        xy=(div_x, ax_xput.get_ylim()[0]),
        xytext=(div_x + 0.3, ax_xput.get_ylim()[0] +
                0.08 * (ax_xput.get_ylim()[1] - ax_xput.get_ylim()[0])),
        fontsize=8, color='gray',
        arrowprops=dict(arrowstyle='->', color='gray', lw=0.8),
    )

    # ── Legend entries for phase shading ─────────────────────────────────────
    phase_handles = [
        Line2D([0], [0], color=C_SAT,   lw=2, label='Saturation phase'),
        Line2D([0], [0], color=C_SLACK,  lw=2, label='Slack phase'),
    ]

    fig.savefig(args.out, bbox_inches='tight', dpi=args.dpi)
    print(f'Saved → {args.out}')

if __name__ == '__main__':
    main()
