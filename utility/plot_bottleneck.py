#!/usr/bin/env python3
"""
plot_bottleneck.py — RocksDB workload bottleneck characterization figures

Produces FOUR separate figures from the command-line CSV list:

  Figure 1  (<stem>_fig1_throughput.<ext>)
            Single panel — throughput for ALL supplied CSVs.

  Figure 2  (<stem>_fig2_diskio_mem.<ext>)
            Two panels side by side (disk I/O left, memory used right).

  Figure 3  (<stem>_fig3_cpu.<ext>)
            Two panels side by side, each panel showing CPU utilization
            (cpu_compute solid, cpu_busy dashed) for one workload.

  Figure 4  (<stem>_fig4_machines.<ext>)
            2 rows × N columns (one column per --csv).
            Row 0 — Throughput (independent y-scale per machine).
            Row 1 — Combined system metrics on a shared % axis:
                    CPU compute (solid blue), CPU busy (dashed blue),
                    Disk read (solid red), Disk write (dashed red),
                    RAM (solid green).
            Disk metrics are expressed as % of --disk-ceiling-read/write
            when those flags are given; otherwise as % of observed peak.
            RAM is expressed as % of observed peak.
            Requires --figures 4 (or --machine-grid) to be built.

Usage
-----
    python3 plot_bottleneck.py \\
        --csv "Machine-A:results_a/summary.csv" \\
        --csv "Machine-B:results_b/summary.csv" \\
        --csv "Machine-C:results_c/summary.csv" \\
        --csv "Machine-D:results_d/summary.csv" \\
        --figures 4 \\
        --out bottleneck_characterization.pdf

Options
-------
--csv  [LABEL:]PATH  Workload summary CSV, optionally prefixed with a display
                     label.  May be repeated up to 8 times.
--out  PATH          Base output path.  Extension sets format (.pdf/.png/.svg).
                     Actual files are written as:
                         <stem>_fig1_throughput.<ext>
                         <stem>_fig2_diskio_mem.<ext>
                         <stem>_fig3_cpu.<ext>
                         <stem>_fig4_machines.<ext>
                     Default: bottleneck_characterization.pdf
--disk-ceiling VAL   Draw a horizontal reference line at VAL MB/s on the disk
                     I/O panel (Figures 1–3).
--no-bands           Use thin error bars instead of shaded std-dev bands.
--normalize-x        Restrict X-axis to thread counts present in ALL workloads.
--width  INCHES      Figure width.  Default: 7.0 (double-column IEEE/ACM).
--dpi    INT         Raster DPI (ignored for PDF/SVG).  Default: 300.
--font-size INT      Base font size.  Default: 12.

Expected CSV columns (produced by saturation_sweep.sh)
------------------------------------------------------
workload_label, threads,
xput_mean, xput_std,
cpu_compute_mean, cpu_compute_std, cpu_compute_min, cpu_compute_max,
cpu_schedule_mean, cpu_schedule_std, cpu_schedule_min, cpu_schedule_max,
disk_read_mb/s, disk_read_mb/s_std, disk_read_mb/s_min, disk_read_mb/s_max,
disk_write_mb/s, disk_write_mb/s_std, disk_write_mb/s_min, disk_write_mb/s_max,
r/s, r/s_std, r/s_min, r/s_max,
w/s, w/s_std, w/s_min, w/s_max,
mem_used_mean, mem_used_std, mem_used_min, mem_used_max,
mem_avail_mean, mem_used_pct_mean,
block_cache_hits, block_cache_misses, block_cache_hit_rate
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
from matplotlib.ticker import FuncFormatter, ScalarFormatter

# ── Color palette ─────────────────────────────────────────────────────────────
# Colorblind-friendly, distinguishable in grayscale print.
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

# Distinct point markers so lines remain distinguishable even without color.
# Order: dot, x, triangle, square, diamond, down-triangle, plus, star.
_MARKERS = ['o', 'x', '^', 's', 'D', 'v', 'P', '*']


def _k_formatter(val, pos):
    """Format large y-axis tick values as '<N>K' (or '<N>M' above 1e6).

    Examples:
        0      → '0'
        50000  → '50K'
        1.5e6  → '1.5M'
    """
    if val == 0:
        return '0'
    abs_v = abs(val)
    if abs_v >= 1e6:
        return f'{val / 1e6:g}M'
    if abs_v >= 1e3:
        return f'{val / 1e3:g}K'
    return f'{val:g}'

# ── CLI ───────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description='Plot RocksDB bottleneck characterization — three separate figures.')
    p.add_argument('--csv', metavar='[LABEL:]PATH', action='append', required=True,
                   dest='csvs',
                   help='Workload summary CSV. May be repeated.')
    p.add_argument('--out', default='bottleneck.pdf',
                   help='Base output path. Extension sets format. Three files are written '
                        'with suffixes _throughput, _diskio_mem, _cpu.')
    p.add_argument('--disk-ceiling', type=float, default=0.0, metavar='MB/s',
                   help='Horizontal reference line on the disk I/O panel in Figures 1–3 (MB/s).')
    p.add_argument('--no-bands', action='store_true',
                   help='Use error bars instead of shaded std-dev bands.')
    p.add_argument('--normalize-x', action='store_true',
                   help='Restrict X-axis to thread counts common to all workloads.')
    p.add_argument('--machine-grid', action='store_true', dest='machine_grid',
                   help='Produce Figure 4 for every supplied CSV: 2 panels (throughput on '
                        'top, all three resource metrics as %% on bottom).  One file per '
                        'machine, named <stem>_machine_<label>.<ext>.')
    p.add_argument('--figures', metavar='N', nargs='+', type=int,
                   default=None,
                   help='Which figures to build, e.g. --figures 4 or --figures 1 2 3 4. '
                        'Figure 4 also requires --machine-grid.  '
                        'Default: 1 2 3  (plus 4 if --machine-grid is set).')
    p.add_argument('--width',  type=float, default=7.0,  metavar='INCHES')
    p.add_argument('--dpi',       type=int, default=300)
    p.add_argument('--font-size', type=int, default=12, dest='font_size')
    return p.parse_args()

# ── Output path helpers ───────────────────────────────────────────────────────

def _out_paths(base: str):
    """Return the three standard output file paths derived from base."""
    root, ext = os.path.splitext(base)
    if not ext:
        ext = '.pdf'
    return (
        f'{root}_throughput{ext}',
        f'{root}_diskio_mem{ext}',
        f'{root}_cpu{ext}',
    )

def _machine_out_path(base: str, label: str) -> str:
    """Return a per-machine output path (legacy, kept for compatibility)."""
    root, ext = os.path.splitext(base)
    if not ext:
        ext = '.pdf'
    safe = label.replace('/', '-').replace(' ', '_').replace(':', '-')
    return f'{root}_machine_{safe}{ext}'

def _fig4_out_path(base: str) -> str:
    """Return the Figure 4 multi-machine grid output path."""
    root, ext = os.path.splitext(base)
    if not ext:
        ext = '.pdf'
    return f'{root}_system_resources_4_all{ext}'

# ── Data loading ──────────────────────────────────────────────────────────────

def load_csv(spec: str):
    """Parse  [LABEL:]PATH  and return (label, DataFrame)."""
    if ':' in spec:
        label, path = spec.split(':', 1)
    else:
        label = None
        path  = spec

    if not os.path.isfile(path):
        sys.exit(f'ERROR: CSV not found: {path}')

    df = pd.read_csv(path).sort_values('threads').reset_index(drop=True)

    # Derive label: CLI > workload_label column > filename stem.
    if label is None:
        if 'workload_label' in df.columns and pd.notna(df['workload_label'].iloc[0]):
            label = str(df['workload_label'].iloc[0])
        else:
            label = os.path.splitext(os.path.basename(path))[0]

    # Ensure all expected columns exist.
    for col in ['xput_mean', 'xput_std',
                'cpu_compute_mean', 'cpu_compute_std',
                'cpu_compute_min', 'cpu_compute_max',
                'cpu_schedule_mean', 'cpu_schedule_std',
                'cpu_schedule_min', 'cpu_schedule_max',
                'disk_read_mb/s', 'disk_read_mb/s_std',
                'disk_read_mb/s_min', 'disk_read_mb/s_max',
                'disk_write_mb/s', 'disk_write_mb/s_std',
                'disk_write_mb/s_min', 'disk_write_mb/s_max',
                'r/s', 'r/s_std', 'r/s_min', 'r/s_max',
                'w/s', 'w/s_std', 'w/s_min', 'w/s_max',
                'mem_used_mean', 'mem_used_std',
                'mem_used_min', 'mem_used_max',
                'mem_avail_mean', 'mem_used_pct_mean',
                'block_cache_hits', 'block_cache_misses',
                'block_cache_hit_rate']:
        if col not in df.columns:
            df[col] = np.nan

    return label, df

# ── Knee detection ────────────────────────────────────────────────────────────

def find_knee(df: pd.DataFrame):
    valid = df.dropna(subset=['xput_mean'])
    if valid.empty:
        return None
    return int(valid.loc[valid['xput_mean'].idxmax(), 'threads'])

# ── Drawing helpers ───────────────────────────────────────────────────────────

def _apply_rcparams(fsz: int):
    plt.rcParams.update({
        'font.size':         fsz,
        'axes.titlesize':    fsz,
        'axes.labelsize':    fsz - 1,
        'xtick.labelsize':   fsz - 2,
        'ytick.labelsize':   fsz - 2,
        'legend.fontsize':   fsz - 2,
        'axes.spines.top':   False,
        'axes.spines.right': False,
        'axes.grid':         True,
        'grid.alpha':        0.28,
        'grid.linewidth':    0.45,
    })

def _plot_line(ax, x, mean, std, color, label, lw, ms, no_bands,
               linestyle='-', marker=None):
    """Plot a mean line ± std-dev.  std may be all-NaN (treated as zero).

    marker: explicit marker override; defaults to 'o' for solid lines, 's' for
            all others so callers can pass any pyplot marker code ('^', 'D', …).
    """
    mean = np.asarray(mean, dtype=float)
    std  = np.asarray(std,  dtype=float)
    std  = np.where(np.isnan(std), 0, std)

    if marker is None:
        marker = 'o' if linestyle == '-' else 's'

    ax.errorbar(x, mean, yerr=std,
                color=color, linewidth=lw, markersize=ms,
                marker=marker, linestyle=linestyle,
                capsize=3, capthick=0.8, elinewidth=1.2,
                label=label, zorder=3)

    if not no_bands:
        ax.fill_between(x, mean - std, mean + std,
                        color=color, alpha=0.12, linewidth=0, zorder=2)

def _knee_vline(ax, knee, color):
    if knee is not None:
        ax.axvline(knee, color=color, linestyle='--',
                   linewidth=0.7, alpha=0.6)

def _set_xticks(ax, x_domain, rotate=True, max_labels=None):
    """Set x ticks for ax.

    max_labels: if given and len(x_domain) exceeds it, tick marks are drawn at
    every position in x_domain but only a sparse subset of labels is shown.
    Labels are chosen by sampling max_labels indices evenly across the domain
    (via linspace), so both endpoints are always included and intermediate
    values are distributed uniformly by position — this works correctly for
    both linear and geometric/power-of-2 client-count sweeps.
    """
    ax.set_xticks(x_domain)
    ax.tick_params(axis='x', labelrotation=45 if (rotate and len(x_domain) > 100) else 0)

    if max_labels is not None and len(x_domain) > max_labels:
        # Evenly-spaced indices including both endpoints.
        indices = set(
            np.round(np.linspace(0, len(x_domain) - 1, max_labels)).astype(int)
        )
        visible = {x_domain[i] for i in indices}
        ax.set_xticklabels([str(x) if x in visible else '' for x in x_domain])

def _resolve_x_domain(datasets, normalize_x: bool):
    if normalize_x:
        common = None
        for _, df in datasets:
            s = set(df['threads'].dropna().astype(int))
            common = s if common is None else common & s
        return sorted(common) if common else []
    else:
        return sorted({int(t) for _, df in datasets
                       for t in df['threads'].dropna()})

def _annotate_knee(ax, df_full, x_domain, normalize_x, idx, color, lw, ms, fsz):
    df = df_full[df_full['threads'].isin(x_domain)] if normalize_x else df_full
    knee = find_knee(df)
    if knee is None:
        return
    knee_row = df[df['threads'] == knee]
    if knee_row.empty or np.isnan(knee_row['xput_mean'].values[0]):
        return
    y_val = knee_row['xput_mean'].values[0]
    x_rng = (x_domain[-1] - x_domain[0]) if len(x_domain) > 1 else 1
    ax.annotate(
        f'▸{knee}T',
        xy=(knee, y_val),
        xytext=(knee + x_rng * 0.04, y_val * 0.84),
        fontsize=fsz - 3, color=color,
        arrowprops=dict(arrowstyle='->', color=color, lw=0.7))

# ── Memory unit auto-scaling ──────────────────────────────────────────────────

def _mem_to_gb(series: pd.Series):
    """Heuristically convert mem_used_mean to GB based on magnitude."""
    valid = series.dropna()
    if valid.empty:
        return series, 'GB'
    mx = valid.max()
    if mx > 1e9:          # bytes → GB
        return series / 1e9, 'GB'
    elif mx > 1e6:        # KB → GB
        return series / 1e6, 'GB'
    elif mx > 1e3:        # MB → GB
        return series / 1e3, 'GB'
    else:                 # already GB or very small
        return series, 'GB'

# ═══════════════════════════════════════════════════════════════════════════════
# Figure 1 — Throughput (all workloads, single panel)
# ═══════════════════════════════════════════════════════════════════════════════

def build_fig1_throughput(datasets, args):
    """Single-panel throughput plot for every supplied CSV."""
    n_wl = len(datasets)
    lw, ms, fsz = 1.5, 4, args.font_size
    fig_h = round(args.width * 0.65, 2)

    _apply_rcparams(fsz)
    fig, ax = plt.subplots(figsize=(args.width*0.85, fig_h*0.85))

    x_domain = _resolve_x_domain(datasets, args.normalize_x)
    legend_handles = []

    for idx, (label, df_full) in enumerate(datasets):
        color  = _PALETTE[idx % len(_PALETTE)]
        marker = _MARKERS[idx % len(_MARKERS)]
        df = df_full[df_full['threads'].isin(x_domain)].copy() \
             if args.normalize_x else df_full.copy()
        x = df['threads'].values

        _plot_line(ax, x, df['xput_mean'], df['xput_std'],
                   color, label, lw, ms, args.no_bands, marker=marker)

        legend_handles.append(
            Line2D([0], [0], color=color, linewidth=lw, marker=marker,
                   markersize=ms, label=label))

    ax.set_ylabel('Throughput (qps)', fontsize=11)
    ax.set_ylim(bottom=0)

    # Y-axis: format large tick values as '<N>K' / '<N>M' instead of trailing zeros.
    ax.yaxis.set_major_formatter(FuncFormatter(_k_formatter))

    # X-axis: log-2 scale so power-of-2 client-count sweeps are evenly spaced.
    # Filter out any non-positive tick values (log scale cannot render 0).
    pos_ticks = [t for t in x_domain if t > 0]
    if pos_ticks:
        ax.set_xscale('log', base=2)
        ax.set_xlim(pos_ticks[0] / 1.1, pos_ticks[-1] * 1.1)
        ax.set_xticks(pos_ticks)
        ax.xaxis.set_major_formatter(ScalarFormatter())
        ax.xaxis.set_minor_formatter(plt.NullFormatter())
        ax.tick_params(axis='x',
                       labelrotation=45 if len(pos_ticks) > 100 else 0)
    else:
        _set_xticks(ax, x_domain)

    ax.legend(handles=legend_handles,
              loc='best', ncol=min(n_wl, 2),
              fontsize=fsz-1, framealpha=0.85)

    fig.supxlabel('Client threads (log\u2082 scale)', fontsize=11, y = 0.05)
    fig.tight_layout()
    return fig

# ═══════════════════════════════════════════════════════════════════════════════
# Figure 2 — Disk I/O and Memory Used (2nd & 3rd CSV, side by side)
# ═══════════════════════════════════════════════════════════════════════════════

def build_fig2_diskio_mem(datasets_23, args):
    """
    Layout: 2 columns × 2 rows via GridSpec.
      Left column  (spans both rows) — disk read and write bandwidth (MB/s).
      Right column, top row          — disk read and write IOPS.
      Right column, bottom row       — memory used (GiB).

    Min/max range is shown as a shaded band instead of std-dev.
    Colors mirror Figure 1: datasets_23[0] gets palette index 1 (2nd CSV),
    datasets_23[1] gets palette index 2 (3rd CSV).
    """
    lw, ms, fsz = 1.5, 4, args.font_size
    fig_h = round(args.width * 0.65, 2)

    _apply_rcparams(fsz)
    fig = plt.figure(figsize=(args.width, fig_h))
    fig.subplots_adjust(left=0.08, right=0.97, top=0.88, bottom=0.12, wspace=0.25)
    gs  = gridspec.GridSpec(5, 2, figure=fig, hspace=0.60)

    ax_disk = fig.add_subplot(gs[:, 0])          # left, full height
    ax_iops = fig.add_subplot(gs[0:3, 1])        # right, top
    ax_mem  = fig.add_subplot(gs[3:5, 1],        # right, bottom — share x with iops
                              sharex=ax_iops)

    x_domain = _resolve_x_domain(datasets_23, args.normalize_x)
    legend_handles = []

    for idx, (label, df_full) in enumerate(datasets_23):
        color   = _PALETTE[idx % len(_PALETTE)]
        color_w = _PALETTE[(idx + 3) % len(_PALETTE)]  # distinct color for write
        df = df_full[df_full['threads'].isin(x_domain)].copy() \
             if args.normalize_x else df_full.copy()
        x = df['threads'].values

        def _band(ax, col_mean, col_min, col_max, c, lbl):
            """Plot mean line + min/max shaded range."""
            mean = np.asarray(df[col_mean], dtype=float)
            lo   = np.asarray(df[col_min],  dtype=float)
            hi   = np.asarray(df[col_max],  dtype=float)
            ax.plot(x, mean, color=c, linewidth=lw, marker='o', markersize=ms, label=lbl, zorder=3)
            if not args.no_bands:
                ax.fill_between(x, lo, hi, color=c, alpha=0.13, linewidth=0, zorder=2)

        # ── Disk bandwidth (MB/s) ─────────────────────────────────────────────
        _band(ax_disk, 'disk_read_mb/s',  'disk_read_mb/s_min',  'disk_read_mb/s_max',
              color,   f'{label} read')
        _band(ax_disk, 'disk_write_mb/s', 'disk_write_mb/s_min', 'disk_write_mb/s_max',
              color_w, f'{label} write')
        if args.disk_ceiling > 0 and idx == 0:
            ax_disk.axhline(args.disk_ceiling, color='black', linestyle=':',
                            linewidth=0.9, alpha=0.7,
                            label=f'ceiling ({args.disk_ceiling:,.0f} MB/s)')

        # ── IOPS ─────────────────────────────────────────────────────────────
        _band(ax_iops, 'r/s', 'r/s_min', 'r/s_max', color,   f'{label} r/s')
        _band(ax_iops, 'w/s', 'w/s_min', 'w/s_max', color_w, f'{label} w/s')

        # ── Memory used (GiB) ────────────────────────────────────────────────
        mem_mean, unit = _mem_to_gb(df['mem_used_mean'])
        mem_lo,   _    = _mem_to_gb(df['mem_used_min'])
        mem_hi,   _    = _mem_to_gb(df['mem_used_max'])
        ax_mem.plot(x, mem_mean, color=color, linewidth=lw,
                    marker='o', markersize=ms, label=label, zorder=3)
        if not args.no_bands:
            ax_mem.fill_between(x, mem_lo, mem_hi,
                                color=color, alpha=0.13, linewidth=0, zorder=2)

        legend_handles.append(
            Line2D([0], [0], color=color, linewidth=lw, marker='o',
                   markersize=ms, label=label))

    # ── Decoration ────────────────────────────────────────────────────────────
    ax_disk.set_ylabel('Disk bandwidth (MB/s)', fontsize=12)
    ax_disk.set_ylim(bottom=0)
    _set_xticks(ax_disk, x_domain, max_labels=4)
    ax_disk.text(0.02, 0.98, '(A) Disk Bandwidth Read/Write',
                 transform=ax_disk.transAxes, fontsize=12, fontweight='bold')
    ax_disk.legend(fontsize=fsz-2, loc='upper left')

    ax_iops.set_ylabel('IOPS', fontsize=12)
    ax_iops.set_ylim(bottom=0)
    plt.setp(ax_iops.get_xticklabels(), visible=False)
    ax_iops.set_xlabel('')
    ax_iops.text(0.02, 0.98, '(B) Disk IOPS Read/Write',
                 transform=ax_iops.transAxes, fontsize=12, fontweight='bold')
    ax_iops.legend(fontsize=fsz-2, loc='upper left')

    ax_mem.set_ylabel(f'Mem used ({unit})', fontsize=12)
    ax_mem.set_ylim(bottom=0)
    _set_xticks(ax_mem, x_domain, max_labels=4)
    ax_mem.text(0.02, 0.98, '(C) Memory Used',
                transform=ax_mem.transAxes, fontsize=12, fontweight='bold')

    fig.supxlabel('Client threads', fontsize=12)
    return fig


# ═══════════════════════════════════════════════════════════════════════════════
# Figure 3 — CPU utilization (2nd & 3rd CSV, one panel each, side by side)
# ═══════════════════════════════════════════════════════════════════════════════

def build_fig3_cpu(datasets_23, args):
    """
    Two panels side by side, one per workload:
      Left  — CPU utilization for the 2nd CSV.
      Right — CPU utilization for the 3rd CSV.
    Each panel shows:
      solid line   — cpu_compute  (1 - idle - iowait)
      dashed line  — cpu_schedule (1 - idle,  incl. iowait)
      hatched band — iowait gap between the two lines
    Min/max shown as light shaded band around each line.
    """
    from matplotlib.patches import Patch

    lw, ms, fsz = 1.5, 4, args.font_size
    fig_h = round(args.width * 0.50, 2)

    _apply_rcparams(fsz)
    fig, axes = plt.subplots(1, len(datasets_23),
                             figsize=(args.width, fig_h), sharey=True)
    if len(datasets_23) == 1:
        axes = [axes]

    x_domain = _resolve_x_domain(datasets_23, args.normalize_x)

    for idx, ((label, df_full), ax) in enumerate(zip(datasets_23, axes)):
        color = _PALETTE[idx % len(_PALETTE)]

        df   = df_full[df_full['threads'].isin(x_domain)].copy() \
               if args.normalize_x else df_full.copy()
        x    = df['threads'].values
        comp     = np.asarray(df['cpu_compute_mean'],  dtype=float)
        sched    = np.asarray(df['cpu_schedule_mean'], dtype=float)
        comp_min = np.asarray(df['cpu_compute_min'],   dtype=float)
        comp_max = np.asarray(df['cpu_compute_max'],   dtype=float)
        sched_min = np.asarray(df['cpu_schedule_min'], dtype=float)
        sched_max = np.asarray(df['cpu_schedule_max'], dtype=float)

        # ── Min/max bands ────────────────────────────────────────────────────
        if not args.no_bands:
            ax.fill_between(x, comp_min,  comp_max,  color=color, alpha=0.10, linewidth=0, zorder=1)
            ax.fill_between(x, sched_min, sched_max, color=color, alpha=0.06, linewidth=0, zorder=1)

        # ── Hatched iowait gap ────────────────────────────────────────────────
        ax.fill_between(x, comp, sched,
                        facecolor='none', edgecolor=color,
                        hatch='xxxx', alpha=0.5, zorder=2)

        # ── Boundary lines ────────────────────────────────────────────────────
        ax.plot(x, comp,  color=color, linewidth=lw, linestyle='-',
                marker='o', markersize=ms, label='cpu_compute',  zorder=3)
        ax.plot(x, sched, color=color, linewidth=lw, linestyle='--',
                marker='s', markersize=ms, label='cpu_schedule', zorder=3)

        ax.set_ylim(0, 105)
        ax.axhline(100, color='black', linestyle=':', linewidth=0.8, alpha=0.55)
        ax.set_title(label, fontsize=fsz)
        _set_xticks(ax, x_domain, max_labels=4)

        if idx == 0:
            ax.set_ylabel('CPU utilization (%)', fontsize=11)
        else:
            plt.setp(ax.get_yticklabels(), visible=False)

    # ── Shared legend above both panels ───────────────────────────────────────
    legend_handles = [
        Line2D([0], [0], color='gray', linestyle='-',  linewidth=lw, marker='o',
               markersize=ms, label='cpu_compute  (1 − idle − iowait)'),
        Line2D([0], [0], color='gray', linestyle='--', linewidth=lw, marker='s',
               markersize=ms, label='cpu_schedule (1 − idle)'),
        Patch(facecolor='none', edgecolor='gray', hatch='xxxx', linewidth=0.6,
              label='iowait (gap)'),
    ]

    fig.tight_layout(rect=[0, 0, 1, 0.88])
    fig.legend(handles=legend_handles,
               loc='upper center',
               ncol=3,
               fontsize=fsz,
               framealpha=0.85,
               bbox_to_anchor=(0.5, 0.99))
    fig.supxlabel('Client threads', fontsize=11)
    return fig

# ═══════════════════════════════════════════════════════════════════════════════
# Figure 4 — 2 × N machine comparison grid
#
# One figure with 2 rows and N columns (one column per CSV).
#
# Layout
# ------
#   Row 0  — Throughput (ops/s) per machine.  Each column has its own y-scale.
#            Dashed vertical knee line spans both rows within the same column.
#   Row 1  — All resource metrics on a % axis per column:
#                • CPU compute  (solid, square)
#                • CPU schedule (solid, triangle) + iowait hatch
#                • Disk BW util (dashed, star)     from disk_bw_avg column
#                • Memory %     (dotted, circle)   from mem_used_pct_mean column
#            100 % reference dotted line.
#   Legend — single shared legend spanning all bottom panels, placed between
#            the two rows.
# ═══════════════════════════════════════════════════════════════════════════════

def build_fig4_machines(datasets, args):
    """
    Single 2 x N figure comparing all machines side by side.

    Parameters
    ----------
    datasets : list of (label: str, df: pd.DataFrame)
        Each DataFrame must contain the pre-computed columns:
          disk_bw_avg       -- (read/C_r + write/C_w) as a fraction 0-1
          disk_bw_stddev    -- propagated std dev of disk_bw_avg, fraction 0-1
          mem_used_pct_mean -- memory usage as a percentage 0-100
    args     : argparse.Namespace
        Relevant flags: --no-bands, --width, --dpi, --font-size.
    """
    from matplotlib.ticker import FuncFormatter
    from matplotlib.patches import Patch

    N   = len(datasets)
    lw, ms, fsz = 1.4, 4, args.font_size

    # Resource encoding — line style + marker (color-neutral).
    # Each machine will use its own _PALETTE colour for ALL resources.
    RS_DISK  = dict(linestyle='-',  marker='x')   # solid  + cross
    RS_CPUC  = dict(linestyle='--', marker='^')    # dashed + triangle
    RS_CPUS  = dict(linestyle='--', marker='s')    # dashed + square
    RS_MEM   = dict(linestyle=':',  marker='o')    # dotted + circle

    _apply_rcparams(fsz)
    plt.rcParams.update({
        'axes.titlesize':   fsz + 1,
        'axes.titleweight': 'normal',
        'axes.labelsize':   fsz,
        'xtick.labelsize':  fsz - 1,
        'ytick.labelsize':  fsz - 1,
        'legend.fontsize':  fsz - 1,
    })

    fig_w = 9
    fig_h = round(fig_w * 0.58, 2)

    fig = plt.figure(figsize=(fig_w, fig_h))
    gs  = gridspec.GridSpec(1, N, figure=fig,
                            wspace=0.12,
                            left=0.0, right=1.0,
                            top=0.90, bottom=0.28)

    axes_top = []

    for i, (label, df_full) in enumerate(datasets):
        # Machine colour — consistent across all figures.
        mc = _PALETTE[i % len(_PALETTE)]

        share_kw = {'sharey': axes_top[0]} if axes_top else {}
        ax0 = fig.add_subplot(gs[0, i], **share_kw)
        axes_top.append(ax0)

        df       = df_full.copy()
        x        = df['threads'].values
        x_domain = sorted(df['threads'].dropna().astype(int).unique())

        ax0.set_title(label, fontsize=fsz+5)

        # -- Knee vline -----------------------------------------------------------
        knee = find_knee(df)
        if knee is not None:
            knee_row = df[df['threads'] == knee]
            if not knee_row.empty and not np.isnan(knee_row['xput_mean'].values[0]):
                ax0.axvline(knee, color='dimgray', linestyle='--',
                           linewidth=0.65, alpha=0.45, zorder=1)

        # -- Resource utilization (%) — all in the machine's colour ---------------
        cpu_comp  = np.asarray(df['cpu_compute_mean'],  dtype=float)
        cpu_sched = np.asarray(df['cpu_schedule_mean'], dtype=float)

        # CPU compute — dashed + triangle
        _plot_line(ax0, x, cpu_comp,
                   np.asarray(df['cpu_compute_std'],  dtype=float),
                   mc, 'CPU compute',  lw, ms+3, args.no_bands,
                   **RS_CPUC)
        # CPU schedule — dashed + square
        _plot_line(ax0, x, cpu_sched,
                   np.asarray(df['cpu_schedule_std'], dtype=float),
                   mc, 'CPU schedule', lw, ms+3, args.no_bands,
                   **RS_CPUS)
        # I/O-wait hatch — machine colour
        ax0.fill_between(x, cpu_comp, cpu_sched,
                         facecolor='none', edgecolor=mc,
                         hatch='xxx', alpha=0.40, linewidth=0, zorder=2)

        # Disk BW — solid + cross
        if 'disk_bw_avg' in df.columns and df['disk_bw_avg'].notna().any():
            disk_mean = np.asarray(df['disk_bw_avg'],    dtype=float) * 100
            disk_std  = np.asarray(df['disk_bw_stddev'], dtype=float) * 100 \
                        if 'disk_bw_stddev' in df.columns else np.zeros(len(df))
            _plot_line(ax0, x, disk_mean, disk_std,
                       mc, 'Disk BW (%)', lw, ms + 3, args.no_bands,
                       **RS_DISK)

        # RAM — dotted + circle
        if 'mem_used_pct_mean' in df.columns and df['mem_used_pct_mean'].notna().any():
            _plot_line(ax0, x,
                       np.asarray(df['mem_used_pct_mean'], dtype=float),
                       np.zeros(len(df)),
                       mc, 'RAM (%)', lw, ms+3, args.no_bands,
                       **RS_MEM)

        ax0.axhline(100, color='black', linestyle=':', linewidth=0.7, alpha=0.45)
        ax0.set_ylim(0, 110)
        if i == 0:
            ax0.set_ylabel('Resource util. (%)', fontsize=fsz+5)
        else:
            ax0.set_ylabel('')
            plt.setp(ax0.get_yticklabels(), visible=False)

        # Log-2 x-axis
        pos_ticks = [t for t in x_domain if t > 0]
        if pos_ticks:
            ax0.set_xscale('log', base=2)
            ax0.set_xlim(pos_ticks[0] / 1.1, pos_ticks[-1] * 1.1)
            if len(pos_ticks) > 4:
                idxs = set(np.round(
                    np.linspace(0, len(pos_ticks) - 1, 4)).astype(int))
                label_set = {pos_ticks[j] for j in idxs}
            else:
                label_set = set(pos_ticks)
            ax0.set_xticks(pos_ticks)
            ax0.xaxis.set_major_formatter(FuncFormatter(
                lambda v, _p, _s=label_set: f'{int(v)}' if int(v) in _s else ''))
            ax0.xaxis.set_minor_formatter(plt.NullFormatter())
        else:
            _set_xticks(ax0, x_domain, max_labels=4)

    fig.supxlabel('Client threads (log\u2082 scale)', fontsize=fsz+5, y=0.14)

    # -- Shared legend — neutral gray shows resource encoding (line style + marker)
    C_LEG = '#555555'   # neutral gray for the legend icons
    legend_handles = [
        Line2D([0],[0], color=C_LEG, lw=lw, ms=ms+1, label='CPU compute',
               **RS_CPUC),
        Line2D([0],[0], color=C_LEG, lw=lw, ms=ms+1, label='CPU compute+IOwait',
               **RS_CPUS),
        Patch(facecolor='none', edgecolor=C_LEG, hatch='xxx', linewidth=0.5,
              label='I/O wait (gap)'),
        Line2D([0],[0], color=C_LEG, lw=lw, ms=ms+2, label='Disk BW',
               **RS_DISK),
        Line2D([0],[0], color=C_LEG, lw=lw, ms=ms+1, label='RAM',
               **RS_MEM),
    ]
    fig.legend(handles=legend_handles,
               loc='lower center',
               bbox_to_anchor=(0.5, 0.00),
               ncol=len(legend_handles),
               prop={'size': fsz + 3},
               framealpha=0.88,
               handlelength=2.0,
               borderpad=0.4,
               columnspacing=1.0)

    return fig


# ── Entry point ───────────────────────────────────────────────────────────────

def _save(fig, path, dpi):
    os.makedirs(os.path.dirname(os.path.abspath(path)) or '.', exist_ok=True)
    fig.savefig(path, bbox_inches='tight', dpi=dpi)
    plt.close(fig)
    print(f'  Saved → {path}')

def main():
    args = parse_args()

    datasets = []
    for spec in args.csvs:
        label, df = load_csv(spec)
        datasets.append((label, df))
        print(f'Loaded: {label!r}  ({len(df)} thread-count points)')

    if not datasets:
        sys.exit('No valid CSV files loaded.')

    if len(datasets) > len(_PALETTE):
        print(f'WARNING: {len(datasets)} workloads but only {len(_PALETTE)} '
              f'colors defined — colors will repeat.')

    if len(datasets) < 3:
        print(f'WARNING: Figures 2 & 3 require at least 3 CSVs; '
              f'only {len(datasets)} supplied.')

    # Resolve which figures to build.
    # --figures overrides everything; otherwise default to 1 2 3 + 4 if --machine-grid.
    if args.figures is not None:
        want = set(args.figures)
    else:
        want = {1, 2, 3}
        if args.machine_grid:
            want.add(4)

    # Implicitly enable --machine-grid when figure 4 is requested.
    if 4 in want:
        args.machine_grid = True

    out1, out2, out3 = _out_paths(args.out)

    if 1 in want:
        print('\nBuilding Figure 1 — Throughput (all workloads) …')
        fig1 = build_fig1_throughput(datasets, args)
        _save(fig1, out1, args.dpi)

    if 2 in want or 3 in want:
        if len(datasets) < 2:
            print(f'\nSkipping Figures 2 & 3: need at least 2 CSVs '
                  f'(only {len(datasets)} supplied).')
        else:
            if 2 in want:
                print('\nBuilding Figure 2 — Disk I/O & Memory (2nd & 3rd workload) …')
                fig2 = build_fig2_diskio_mem(datasets, args)
                _save(fig2, out2, args.dpi)
            if 3 in want:
                print('\nBuilding Figure 3 — CPU utilization (2nd & 3rd workload, side by side) …')
                fig3 = build_fig3_cpu(datasets, args)
                _save(fig3, out3, args.dpi)

    if 4 in want:
        if not datasets:
            print('\nSkipping Figure 4: no datasets loaded.')
        else:
            labels = ', '.join(f'{ds[0]!r}' for ds in datasets)
            print(f'\nBuilding Figure 4 — 2×{len(datasets)} grid ({labels}) …')
            out4 = _fig4_out_path(args.out)
            fig4 = build_fig4_machines(datasets, args)
            _save(fig4, out4, args.dpi)

    print('\nDone.')

if __name__ == '__main__':
    main()
