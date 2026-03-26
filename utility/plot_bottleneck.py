#!/usr/bin/env python3
"""
plot_bottleneck.py — RocksDB workload bottleneck characterization figure

Produces a 2×2 figure where each panel shows one metric (throughput, disk I/O,
CPU, block-cache hit rate) and each workload is a distinct colored line.  All
panels share the same X-axis (client thread count).  A single shared legend
identifies the workloads by color.

Usage
-----
    python3 plot_bottleneck.py \\
        --csv "IO-bound:results_workloada/summary.csv" \\
        --csv "CPU-bound:results_workloade/summary.csv" \\
        --csv "Memory-bound:results_workloadc/summary.csv" \\
        --csv "None-bound:results_workloadd/summary.csv" \\
        --out bottleneck_characterization.pdf

    # Label is optional; the workload_label column in the CSV is used as fallback.
    python3 plot_bottleneck.py \\
        --csv results_workloada/summary.csv \\
        --csv results_workloadc/summary.csv \\
        --out figure.pdf

Options
-------
--csv  [LABEL:]PATH  Workload summary CSV, optionally prefixed with a display
                     label.  May be repeated up to 8 times.
--out  PATH          Output file.  Extension sets format: .pdf, .png, .svg.
                     Default: bottleneck_characterization.pdf
--disk-ceiling VAL   Draw a horizontal reference line at VAL MB/s on the disk
                     I/O panel (e.g. measured fio mixed-RW device ceiling).
--no-bands           Use thin error bars instead of shaded std-dev bands.
--normalize-x        Restrict X-axis to thread counts present in ALL workloads
                     (useful when sweeps used different thread sets).
--width  INCHES      Figure width.  Default: 7.0 (double-column IEEE/ACM).
                     Use 3.5 for single-column.
--height INCHES      Figure height.  Default: auto (~0.9 × width for 2×2).
--dpi    INT         Raster DPI (ignored for PDF/SVG).  Default: 300.
--font-size INT      Base font size.  Default: 8.

Expected CSV columns (produced by saturation_sweep.sh)
------------------------------------------------------
workload_label, threads,
xput_mean, xput_std,
cpu_active_mean, cpu_active_std,
disk_total_mean, disk_total_std,
disk_read_mean, disk_read_std, disk_write_mean, disk_write_std,
mem_used_pct_mean,
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

# ── Color palette ─────────────────────────────────────────────────────────────
# Colorblind-friendly, distinguishable in grayscale print.
# Each entry: (solid_color, band_color_same_with_alpha handled via fill_between)
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

# ── CLI ───────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description='Plot RocksDB workload bottleneck characterization (2×2 figure).')
    p.add_argument('--csv', metavar='[LABEL:]PATH', action='append', required=True,
                   dest='csvs',
                   help='Workload summary CSV. May be repeated.')
    p.add_argument('--out', default='bottleneck_characterization.pdf',
                   help='Output file path (extension sets format).')
    p.add_argument('--disk-ceiling', type=float, default=0.0, metavar='MB/s',
                   help='Horizontal reference line on the disk I/O panel (MB/s).')
    p.add_argument('--no-bands', action='store_true',
                   help='Use error bars instead of shaded std-dev bands.')
    p.add_argument('--normalize-x', action='store_true',
                   help='Restrict X-axis to thread counts common to all workloads.')
    p.add_argument('--width',  type=float, default=7.0,  metavar='INCHES')
    p.add_argument('--height', type=float, default=None, metavar='INCHES',
                   help='Figure height. Default: 0.88 × width.')
    p.add_argument('--dpi',       type=int, default=300)
    p.add_argument('--font-size', type=int, default=8, dest='font_size')
    return p.parse_args()

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
                'cpu_active_mean', 'cpu_active_std',
                'disk_total_mean', 'disk_total_std',
                'disk_read_mean', 'disk_read_std',
                'disk_write_mean', 'disk_write_std',
                'mem_used_pct_mean',
                'block_cache_hits', 'block_cache_misses', 'block_cache_hit_rate']:
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

def _plot_line(ax, x, mean, std, color, label, lw, ms, no_bands):
    """Plot a mean line with either a shaded std band or thin error bars."""
    mean = np.asarray(mean, dtype=float)
    std  = np.asarray(std,  dtype=float)
    std  = np.where(np.isnan(std), 0, std)

    if no_bands:
        ax.errorbar(x, mean, yerr=std,
                    color=color, linewidth=lw, markersize=ms,
                    marker='o', linestyle='-',
                    capsize=2, capthick=0.6, elinewidth=0.6,
                    label=label)
    else:
        ax.plot(x, mean, color=color, linewidth=lw, markersize=ms,
                marker='o', linestyle='-', label=label)
        ax.fill_between(x, mean - std, mean + std,
                        color=color, alpha=0.12, linewidth=0)

def _knee_vline(ax, knee, color):
    if knee is not None:
        ax.axvline(knee, color=color, linestyle='--',
                   linewidth=0.7, alpha=0.6)

# ── Main figure builder ───────────────────────────────────────────────────────

def build_figure(datasets, args):
    n_wl  = len(datasets)
    lw    = 1.5     # line width
    ms    = 4       # marker size
    fsz   = args.font_size
    fig_h = args.height if args.height else round(args.width * 0.88, 2)

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

    fig = plt.figure(figsize=(args.width, fig_h))
    gs  = gridspec.GridSpec(2, 2, figure=fig, hspace=0.42, wspace=0.32)

    ax_xput = fig.add_subplot(gs[0, 0])
    ax_disk = fig.add_subplot(gs[0, 1])
    ax_cpu  = fig.add_subplot(gs[1, 0])
    ax_mem  = fig.add_subplot(gs[1, 1])

    # ── Determine X-axis domain ───────────────────────────────────────────────
    if args.normalize_x:
        common = None
        for _, df in datasets:
            s = set(df['threads'].dropna().astype(int))
            common = s if common is None else common & s
        x_domain = sorted(common) if common else []
    else:
        all_threads = sorted({int(t) for _, df in datasets
                               for t in df['threads'].dropna()})
        x_domain = all_threads

    # ── Per-workload lines ────────────────────────────────────────────────────
    legend_handles = []

    for idx, (label, df_full) in enumerate(datasets):
        color = _PALETTE[idx % len(_PALETTE)]

        # Restrict to x_domain.
        if args.normalize_x:
            df = df_full[df_full['threads'].isin(x_domain)].copy()
        else:
            df = df_full.copy()

        x    = df['threads'].values
        knee = find_knee(df)

        # Throughput
        _plot_line(ax_xput, x, df['xput_mean'], df['xput_std'],
                   color, label, lw, ms, args.no_bands)
        _knee_vline(ax_xput, knee, color)

        # Disk I/O — total bandwidth (R+W)
        _plot_line(ax_disk, x, df['disk_total_mean'], df['disk_total_std'],
                   color, label, lw, ms, args.no_bands)
        _knee_vline(ax_disk, knee, color)

        # CPU utilization
        _plot_line(ax_cpu, x, df['cpu_active_mean'], df['cpu_active_std'],
                   color, label, lw, ms, args.no_bands)
        _knee_vline(ax_cpu, knee, color)

        # Block cache hit rate (memory panel)
        if df['block_cache_hit_rate'].notna().any():
            _plot_line(ax_mem, x, df['block_cache_hit_rate'],
                       pd.Series(np.zeros(len(df))),   # no std — cumulative counter
                       color, label, lw, ms, args.no_bands)
        elif df['mem_used_pct_mean'].notna().any():
            # Fallback: show RAM used % scaled to 0–1 for a comparable axis.
            _plot_line(ax_mem, x, df['mem_used_pct_mean'] / 100,
                       pd.Series(np.zeros(len(df))),
                       color, label, lw, ms, args.no_bands)
        _knee_vline(ax_mem, knee, color)

        # Legend handle (one per workload, shared across all panels).
        legend_handles.append(
            Line2D([0], [0], color=color, linewidth=lw, marker='o',
                   markersize=ms, label=label))

    # ── Panel decoration ──────────────────────────────────────────────────────

    # Throughput
    ax_xput.set_title('Throughput', fontweight='bold')
    ax_xput.set_ylabel('ops / sec')
    ax_xput.set_ylim(bottom=0)

    # Disk I/O
    ax_disk.set_title('Disk I/O (read + write)', fontweight='bold')
    ax_disk.set_ylabel('MB / sec')
    ax_disk.set_ylim(bottom=0)
    if args.disk_ceiling > 0:
        ax_disk.axhline(args.disk_ceiling, color='black', linestyle=':',
                        linewidth=0.9, alpha=0.7,
                        label=f'Device ceiling ({args.disk_ceiling:,.0f} MB/s)')

    # CPU
    ax_cpu.set_title('CPU utilization', fontweight='bold')
    ax_cpu.set_ylabel('Active %  (100 − idle)')
    ax_cpu.set_ylim(0, 105)
    ax_cpu.axhline(100, color='black', linestyle=':', linewidth=0.8, alpha=0.55)

    # Memory / cache hit rate
    has_hit_rate = any(df['block_cache_hit_rate'].notna().any()
                       for _, df in datasets)
    if has_hit_rate:
        ax_mem.set_title('Block cache hit rate', fontweight='bold')
        ax_mem.set_ylabel('Hit rate  (0 – 1)')
        ax_mem.set_ylim(0, 1.05)
        ax_mem.axhline(1.0, color='black', linestyle=':', linewidth=0.8, alpha=0.55)
    else:
        ax_mem.set_title('Memory used %  (RAM)', fontweight='bold')
        ax_mem.set_ylabel('RAM used %  (scaled ÷ 100)')
        ax_mem.set_ylim(0, 1.05)
        ax_mem.axhline(1.0, color='black', linestyle=':', linewidth=0.8, alpha=0.55)

    # ── X-axis ticks & labels ─────────────────────────────────────────────────
    for ax in (ax_xput, ax_disk, ax_cpu, ax_mem):
        ax.set_xticks(x_domain)
        ax.tick_params(axis='x', labelrotation=45 if len(x_domain) > 8 else 0)

    ax_cpu.set_xlabel('Client threads')
    ax_mem.set_xlabel('Client threads')

    # Hide x tick labels on top row to reduce clutter.
    plt.setp(ax_xput.get_xticklabels(), visible=False)
    plt.setp(ax_disk.get_xticklabels(), visible=False)

    # ── Knee annotation note ──────────────────────────────────────────────────
    # Add a small "⊳ knee" label only in the throughput panel to avoid clutter.
    for idx, (label, df_full) in enumerate(datasets):
        color = _PALETTE[idx % len(_PALETTE)]
        df    = df_full[df_full['threads'].isin(x_domain)] if args.normalize_x \
                else df_full
        knee  = find_knee(df)
        if knee is None:
            continue
        knee_row = df[df['threads'] == knee]
        if knee_row.empty or np.isnan(knee_row['xput_mean'].values[0]):
            continue
        y_val = knee_row['xput_mean'].values[0]
        x_rng = (x_domain[-1] - x_domain[0]) if len(x_domain) > 1 else 1
        ax_xput.annotate(
            f'▸{knee}T',
            xy=(knee, y_val),
            xytext=(knee + x_rng * 0.04, y_val * 0.84),
            fontsize=fsz - 3, color=color,
            arrowprops=dict(arrowstyle='->', color=color, lw=0.7))

    # ── Shared legend ─────────────────────────────────────────────────────────
    # Place below the figure so it doesn't eat into panel space.
    # Also add the knee marker key.
    legend_handles.append(
        Line2D([0], [0], color='gray', linestyle='--', linewidth=0.7,
               label='Saturation knee'))

    fig.legend(handles=legend_handles,
               loc='lower center',
               ncol=min(n_wl + 1, 5),
               fontsize=fsz - 1,
               framealpha=0.85,
               borderpad=0.5,
               columnspacing=1.0,
               bbox_to_anchor=(0.5, -0.04))

    fig.suptitle('RocksDB Workload Bottleneck Characterization',
                 fontsize=fsz + 1, fontweight='bold', y=1.01)

    return fig

# ── Entry point ───────────────────────────────────────────────────────────────

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

    fig = build_figure(datasets, args)

    out = args.out
    os.makedirs(os.path.dirname(os.path.abspath(out)) or '.', exist_ok=True)
    fig.savefig(out, bbox_inches='tight', dpi=args.dpi)
    plt.close(fig)
    print(f'\nFigure saved → {out}')

if __name__ == '__main__':
    main()
