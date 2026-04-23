#!/usr/bin/env python3
"""
plot_stitched.py — Multi-machine stitched saturation + IO/CPU-slack figure
==========================================================================

Produces an N × 3 panel figure where each ROW is one machine and the three
COLUMNS are:

  Col 0 — Saturation phase:
           QPS full sweep (all thread counts) [from --sat]

  Col 1 — Slack phase: I/O bandwidth:
           QPS (sat pre-knee faded + slack) + Disk util. (%) [from --sat + --io]

  Col 2 — Slack phase: CPU:
           QPS (sat pre-knee faded + slack) + CPU compute (%) [from --sat + --slack]

The knee divider (red line) marks the saturation→slack transition in all columns.

Y-axis layout:
  - Left column:   QPS on left y-axis only
  - Middle column: no y-axis (scale readable from left col)
  - Right column:  right y-axis only (CPU %)

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
from matplotlib.legend_handler import HandlerBase
import matplotlib.ticker as mticker

# ── custom legend handler: vertical knee line with hash marks ────────────────

class _HandlerKnee(HandlerBase):
    """Renders the knee-divider legend icon as a vertical line with hash marks."""
    def create_artists(self, legend, orig_handle,
                       xdescent, ydescent, width, height, fontsize, trans):
        x    = xdescent + width / 2
        y0   = ydescent
        y1   = ydescent + height
        col  = orig_handle.get_color()
        lw   = orig_handle.get_linewidth()
        alp  = 0.70

        vline = Line2D([x, x], [y0, y1],
                       color=col, linewidth=lw, alpha=alp, transform=trans)
        artists = [vline]
        hw = width * 0.22          # half-width of each tick mark
        for frac in (0.25, 0.50, 0.75):
            y = y0 + (y1 - y0) * frac
            tick = Line2D([x - hw, x + hw], [y, y],
                          color=col, linewidth=lw * 0.7, alpha=alp, transform=trans)
            artists.append(tick)
        return artists


# ── fixed metric colours ─────────────────────────────────────────────────────
C_QPS    = '#777777'   # gray  — throughput (QPS), all panels
C_DISK   = '#1b3a8c'   # navy  — disk bandwidth
C_CPU    = '#2d8b37'   # green — CPU compute
C_KNEE   = '#cc2222'   # red   — knee divider line
C_LEG    = '#444444'   # neutral gray — legend icons


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

def _divider_hashed(ax, x):
    """Red knee-divider line with small hash marks."""
    ax.axvline(x, color=C_KNEE, linestyle='-', linewidth=1.8,
               alpha=0.70, zorder=5)
    yl = ax.get_ylim()
    y_mid   = (yl[0] + yl[1]) / 2
    y_range = yl[1] - yl[0]
    hash_spacing = y_range * 0.08
    n_hashes = 5
    for i in range(n_hashes):
        y = y_mid - (n_hashes // 2 - i) * hash_spacing
        if yl[0] <= y <= yl[1]:
            ax.plot([x - 0.15, x + 0.15], [y, y],
                    color=C_KNEE, linewidth=1.2, alpha=0.70, zorder=5)

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

def _draw_row(ax_sat, ax_sat_r, ax_io, ax_io_r, ax_cpu, ax_cpu_r,
              sat, io_df, slack, knee, mc, row_label, fsz,
              lw=2.0, ms=6, show_xticks=True):
    """
    Draw one machine row.

    Color scheme (fixed, not per-machine):
      C_QPS  (gray)  — throughput in all three columns
      C_DISK (navy)  — disk bandwidth (middle column right axis)
      C_CPU  (green) — CPU compute   (right column right axis)
      C_KNEE (red)   — knee divider  (all columns)

    Col 0 — full saturation sweep, QPS only.
    Col 1 — sat pre-knee QPS (same gray) + slack QPS + disk BW on right axis.
    Col 2 — sat pre-knee QPS (same gray) + slack QPS + CPU %  on right axis.
    """
    # ── X-axis construction ──────────────────────────────────────────────────
    tick_xs, tick_labels, knee_idx, sat_xs, slack_xs = \
        build_xaxis(sat, slack, knee)

    pre_end = knee_idx + 1
    pre_xs  = sat_xs[:pre_end]   # positions 0 .. knee_idx (inclusive)

    # Split io / slack DataFrames at workers==0 (knee anchor).
    def _split_sweep(df):
        base  = df[df['workers'] == 0]
        sweep = df[df['workers'] >  0].reset_index(drop=True)
        baseline = base.iloc[0] if len(base) > 0 else None
        if len(sweep) == 0:
            sweep    = df.copy()
            baseline = None
        return sweep, baseline

    io_df_sweep,    io_baseline    = _split_sweep(io_df)
    slack_df_sweep, slack_baseline = _split_sweep(slack)

    io_xs = [knee_idx + np.log2(w) + 1
             for w in io_df_sweep['workers'].tolist()]
    io_xs_aug    = [knee_idx] + list(io_xs)   # starts at knee
    slack_xs_aug = [knee_idx] + list(slack_xs)

    sat_knee_row = sat.iloc[knee_idx]

    # ── helpers ───────────────────────────────────────────────────────────────
    def _seg(df, name, slc=None):
        v = _col(df, name).values.astype(float)
        return v[slc] if slc is not None else v

    def _seg0(df, name, slc=None):
        v = _seg(df, name, slc)
        return np.where(np.isfinite(v), v, 0.0)

    def _pct(arr):
        a = np.array(arr, dtype=float)
        return np.where(np.isfinite(a), a * 100.0, np.nan)

    def _aug_io(col, zero=False):
        getter = _seg0 if zero else _seg
        arr = getter(io_df_sweep, col)
        if io_baseline is not None and col in io_baseline.index:
            knee_val = float(io_baseline[col])
        else:
            knee_val = np.nan
        return np.concatenate([[knee_val], arr])

    def _aug_slack(col, zero=False):
        getter = _seg0 if zero else _seg
        arr = getter(slack_df_sweep, col)
        if slack_baseline is not None and col in slack_baseline.index:
            knee_val = float(slack_baseline[col])
        elif col in sat.columns:
            knee_val = float(sat_knee_row[col])
        else:
            knee_val = np.nan
        return np.concatenate([[knee_val], arr])

    # ═══════════════════════════════════════════════════════════════════════════
    # COLUMN 0: SATURATION — full sweep, QPS only
    # ═══════════════════════════════════════════════════════════════════════════
    ax = ax_sat

    _phase(ax, sat_xs,
           _seg(sat, 'xput_mean'),
           _seg0(sat, 'xput_std'),
           C_QPS, fmt='D-', lw=lw, ms=ms)

    ax.set_ylim(bottom=0)
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(_fmt_kps))
    ax.set_ylabel('QPS', fontsize=fsz, color='black', labelpad=4)

    _divider_hashed(ax, knee_idx)

    if show_xticks:
        sat_threads = sat['threads'].tolist()
        sat_labels  = {i: f'{sat_threads[i]}T' for i in range(len(sat_threads))}
        sat_tick_xs = sorted(sat_labels.keys())
        _set_xticks(ax, sat_tick_xs, [sat_labels[x] for x in sat_tick_xs])
    else:
        ax.set_xticks(sat_xs)
        ax.set_xlim(sat_xs[0] - 0.7, sat_xs[-1] + 0.7)
        plt.setp(ax.get_xticklabels(), visible=False)

    # ═══════════════════════════════════════════════════════════════════════════
    # COLUMN 1: SLACK PHASE — I/O bandwidth
    #   Left axis:  QPS (sat pre-knee + io slack, same C_QPS gray)
    #   Right axis: Disk BW % (navy)
    # ═══════════════════════════════════════════════════════════════════════════
    ax  = ax_io
    ax_r = ax_io_r

    # Pre-knee QPS from the saturation CSV (same color, connects at knee_idx).
    _phase(ax, pre_xs,
           _seg(sat, 'xput_mean', slice(0, pre_end)),
           _seg0(sat, 'xput_std',  slice(0, pre_end)),
           C_QPS, fmt='D-', lw=lw, ms=ms)

    # Connector: line-only bridge from last sat point to first slack point.
    if len(io_xs_aug) > 1:
        ax.plot([pre_xs[-1], io_xs_aug[1]],
                [_seg(sat, 'xput_mean')[knee_idx], _aug_io('xput_mean')[1]],
                '-', color=C_QPS, lw=lw, zorder=2)

    # Post-knee QPS from the IO CSV — skip the [0] knee anchor to avoid a
    # duplicate marker on top of the saturation curve's last point.
    _phase(ax, io_xs_aug[1:],
           _aug_io('xput_mean')[1:],
           _aug_io('xput_std', zero=True)[1:],
           C_QPS, fmt='D-', lw=lw, ms=ms,
           lo=_aug_io('xput_min')[1:],
           hi=_aug_io('xput_max')[1:])

    ax.set_ylim(bottom=0)
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(_fmt_kps))
    ax.set_ylabel('QPS', fontsize=fsz, color='black', labelpad=4)

    # Right axis: Disk BW % — sat pre-knee from sat CSV, then slack phase from io CSV.
    _phase(ax_r, pre_xs,
           _pct(_seg(sat, 'disk_bw_avg',    slice(0, pre_end))),
           _pct(_seg0(sat, 'disk_bw_stddev', slice(0, pre_end))),
           C_DISK, fmt='x-', lw=lw, ms=ms)
    if len(io_xs_aug) > 1:
        ax_r.plot([pre_xs[-1], io_xs_aug[1]],
                  [_pct(_seg(sat, 'disk_bw_avg'))[knee_idx],
                   _pct(_aug_io('disk_bw_avg'))[1]],
                  '-', color=C_DISK, lw=lw, zorder=2)
    _phase(ax_r, io_xs_aug[1:],
           _pct(_aug_io('disk_bw_avg'))[1:],
           _pct(_aug_io('disk_bw_stddev', zero=True))[1:],
           C_DISK, fmt='x-', lw=lw, ms=ms)

    ax_r.set_ylim(0, 110)
    ax_r.axhline(100, color='black', linestyle=':', linewidth=0.8, alpha=0.4)
    ax_r.set_ylabel('Disk BW %', fontsize=fsz, color=C_DISK, labelpad=4)
    ax_r.tick_params(axis='y', colors=C_DISK, labelsize=fsz - 1)
    ax_r.spines['right'].set_color(C_DISK)

    # X-axis ticks: full range (pre-knee T labels + post-knee W labels).
    if show_xticks:
        sat_threads = sat['threads'].tolist()
        io_labels = {i: f'{sat_threads[i]}T' for i in range(pre_end)}
        for j, w in enumerate(io_df_sweep['workers'].tolist()):
            io_labels[io_xs_aug[j + 1]] = f'+{w}W'
        io_tick_xs = sorted(io_labels.keys())
        _set_xticks(ax, io_tick_xs, [io_labels[x] for x in io_tick_xs])
    else:
        combined_io_xs = list(range(pre_end)) + list(io_xs_aug[1:])
        ax.set_xticks(combined_io_xs)
        ax.set_xlim(0 - 0.7, (io_xs_aug[-1] if io_xs_aug else knee_idx) + 0.7)
        plt.setp(ax.get_xticklabels(), visible=False)

    _divider_hashed(ax, knee_idx)

    # ═══════════════════════════════════════════════════════════════════════════
    # COLUMN 2: SLACK PHASE — CPU
    #   Left axis:  QPS (sat pre-knee + cpu slack, same C_QPS gray)
    #   Right axis: CPU compute % (green)
    # ═══════════════════════════════════════════════════════════════════════════
    ax  = ax_cpu
    ax_r = ax_cpu_r

    # Pre-knee QPS from the saturation CSV.
    _phase(ax, pre_xs,
           _seg(sat, 'xput_mean', slice(0, pre_end)),
           _seg0(sat, 'xput_std',  slice(0, pre_end)),
           C_QPS, fmt='D-', lw=lw, ms=ms)

    # Connector: line-only bridge from last sat point to first slack point.
    if len(slack_xs_aug) > 1:
        ax.plot([pre_xs[-1], slack_xs_aug[1]],
                [_seg(sat, 'xput_mean')[knee_idx], _aug_slack('xput_mean')[1]],
                '-', color=C_QPS, lw=lw, zorder=2)

    # Post-knee QPS from the CPU-slack CSV — skip the [0] knee anchor.
    _phase(ax, slack_xs_aug[1:],
           _aug_slack('xput_mean')[1:],
           _aug_slack('xput_std', zero=True)[1:],
           C_QPS, fmt='D-', lw=lw, ms=ms,
           lo=_aug_slack('xput_min')[1:],
           hi=_aug_slack('xput_max')[1:])

    ax.set_ylim(bottom=0)
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(_fmt_kps))
    ax.set_ylabel('QPS', fontsize=fsz, color='black', labelpad=4)

    # Right axis: CPU compute % — sat pre-knee from sat CSV, then slack phase.
    _phase(ax_r, pre_xs,
           _seg(sat, 'cpu_compute_mean',    slice(0, pre_end)),
           _seg0(sat, 'cpu_compute_std',     slice(0, pre_end)),
           C_CPU, fmt='^-', lw=lw, ms=ms)
    if len(slack_xs_aug) > 1:
        ax_r.plot([pre_xs[-1], slack_xs_aug[1]],
                  [_seg(sat, 'cpu_compute_mean')[knee_idx],
                   _aug_slack('cpu_compute_mean')[1]],
                  '-', color=C_CPU, lw=lw, zorder=2)
    _phase(ax_r, slack_xs_aug[1:],
           _aug_slack('cpu_compute_mean')[1:],
           _aug_slack('cpu_compute_std', zero=True)[1:],
           C_CPU, fmt='^-', lw=lw, ms=ms)

    ax_r.set_ylim(0, 105)
    ax_r.axhline(100, color='black', linestyle=':', linewidth=0.8, alpha=0.4)
    ax_r.set_ylabel('Resource Utilization %', fontsize=fsz, color=C_CPU, labelpad=4)
    ax_r.tick_params(axis='y', colors=C_CPU, labelsize=fsz - 1)
    ax_r.spines['right'].set_color(C_CPU)

    # X-axis ticks: full range (pre-knee T labels + post-knee W labels).
    if show_xticks:
        sat_threads = sat['threads'].tolist()
        cpu_labels = {i: f'{sat_threads[i]}T' for i in range(pre_end)}
        for j, w in enumerate(slack_df_sweep['workers'].tolist()):
            cpu_labels[slack_xs_aug[j + 1]] = f'+{w}W'
        cpu_tick_xs = sorted(cpu_labels.keys())
        _set_xticks(ax, cpu_tick_xs, [cpu_labels[x] for x in cpu_tick_xs])
    else:
        combined_cpu_xs = list(range(pre_end)) + list(slack_xs_aug[1:])
        ax.set_xticks(combined_cpu_xs)
        ax.set_xlim(0 - 0.7, (slack_xs_aug[-1] if slack_xs_aug else knee_idx) + 0.7)
        plt.setp(ax.get_xticklabels(), visible=False)

    _divider_hashed(ax, knee_idx)

    # ── Row label on the left column ──────────────────────────────────────────
    ax_sat.set_ylabel(row_label, fontsize=fsz + 1, fontweight='bold', labelpad=6)


# ── CLI ──────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description='N × 3 stitched saturation + IO/CPU-slack figure (one row per machine).',
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
    p.add_argument('--width', type=float,  default=15.0, help='Figure width in inches.')
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
        mc    = '#000000'   # mc kept for API compat but metric colors are now fixed
        machines.append(dict(sat=sat, io=io_df, slack=slack, knee=knee,
                             label=labels[i], mc=mc))
        print(f'[{labels[i]}]  sat={len(sat)} pts, io={len(io_df)} pts, '
              f'slack={len(slack)} pts, knee={knee}T')

    # ── Figure layout: N rows × 3 cols ───────────────────────────────────────
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

    COL_TITLES = ['Saturation phase', 'Slack phase: I/O bandwidth', 'Slack phase: CPU']

    fig_h = args.row_height * n + (1.0 if args.title else 0.5)
    fig   = plt.figure(figsize=(args.width, fig_h))

    if args.title:
        fig.suptitle(args.title, fontsize=fsz + 3, fontweight='bold', y=0.995)

    # Reserve bottom space for the shared legend.
    LEGEND_H = 0.09
    gs = gridspec.GridSpec(
        n, 3, figure=fig,
        hspace=0.40, wspace=0.08,
        top=0.96 if not args.title else 0.94,
        bottom=LEGEND_H + 0.03,
        left=0.10, right=0.95,
    )

    for i, m in enumerate(machines):
        ax0  = fig.add_subplot(gs[i, 0])
        ax0r = ax0.twinx()
        ax1  = fig.add_subplot(gs[i, 1])
        ax1r = ax1.twinx()
        ax2  = fig.add_subplot(gs[i, 2])
        ax2r = ax2.twinx()

        # Column titles on the first row only.
        if i == 0:
            ax0.set_title(COL_TITLES[0], fontsize=fsz + 1, fontweight='bold', pad=8)
            ax1.set_title(COL_TITLES[1], fontsize=fsz + 1, fontweight='bold', pad=8)
            ax2.set_title(COL_TITLES[2], fontsize=fsz + 1, fontweight='bold', pad=8)

        _draw_row(ax0, ax0r, ax1, ax1r, ax2, ax2r,
                  m['sat'], m['io'], m['slack'], m['knee'],
                  m['mc'], m['label'], fsz,
                  show_xticks=True)

        # ── Y-axis visibility per column ──────────────────────────────────────
        # Left col (ax0): left y-axis shown; right y-axis (ax0r) hidden.
        ax0r.tick_params(axis='y', right=False, labelright=False)
        ax0r.set_ylabel('')
        ax0r.spines['right'].set_visible(False)

        # Middle col (ax1 / ax1r): no y-axis on either side.
        ax1.tick_params(axis='y', left=False, labelleft=False)
        ax1.set_ylabel('')
        ax1.spines['left'].set_visible(False)
        ax1r.tick_params(axis='y', right=False, labelright=False)
        ax1r.set_ylabel('')
        ax1r.spines['right'].set_visible(False)

        # Right col (ax2 / ax2r): right y-axis only; hide left (QPS) side.
        ax2.tick_params(axis='y', left=False, labelleft=False)
        ax2.set_ylabel('')
        ax2.spines['left'].set_visible(False)

    # ── Shared legend ────────────────────────────────────────────────────────
    knee_handle = Line2D([0],[0], color=C_KNEE, lw=1.8, ls='-',
                         label='Knee (sat→slack)')
    legend_handles = [
        Line2D([0],[0], color='none', label='T = YCSB threads   W = SOI threads'),
        Line2D([0],[0], color=C_QPS,  lw=2, marker='D', ms=5, ls='-',
               label='Throughput (QPS)'),
        Line2D([0],[0], color=C_DISK, lw=2, marker='x', ms=5, ls='-',
               label='Disk BW %'),
        Line2D([0],[0], color=C_CPU,  lw=2, marker='^', ms=5, ls='-',
               label='CPU compute %'),
        knee_handle,
    ]
    fig.legend(
        handles=legend_handles,
        handler_map={knee_handle: _HandlerKnee()},
        loc='lower center',
        bbox_to_anchor=(0.5, -0.03),
        ncol=5,
        framealpha=0.88,
        handlelength=1.0,   # narrow column so the vertical icon looks right
        handleheight=1.8,   # taller handle gives room for the hash marks
        borderpad=0.5,
        columnspacing=1.2,
        fontsize=fsz - 1,
    )

    fig.savefig(args.out, bbox_inches='tight', dpi=args.dpi)
    print(f'Saved → {args.out}')


if __name__ == '__main__':
    main()
