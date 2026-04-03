#!/usr/bin/env python3
"""
plot_bottleneck.py — RocksDB workload bottleneck characterization figures

Produces THREE separate figures from the command-line CSV list:

  Figure 1  (<stem>_fig1_throughput.<ext>)
            Single panel — throughput for ALL supplied CSVs.

  Figure 2  (<stem>_fig2_diskio_mem.<ext>)
            Two panels side by side (disk I/O left, memory used right)
            using ONLY the 2nd and 3rd CSV.

  Figure 3  (<stem>_fig3_cpu.<ext>)
            Two panels side by side, each panel showing the CPU utilization
            (cpu_compute solid, cpu_busy dashed) for ONE workload:
            left panel = 2nd CSV, right panel = 3rd CSV.

Usage
-----
    python3 plot_bottleneck.py \\
        --csv "IO-bound:results_workloada/summary.csv" \\
        --csv "CPU-bound:results_workloade/summary.csv" \\
        --csv "Memory-bound:results_workloadc/summary.csv" \\
        --csv "None-bound:results_workloadd/summary.csv" \\
        --out bottleneck_characterization.pdf

    # Figures 2 & 3 always use only the 2nd and 3rd --csv entries.
    # At least 3 CSVs are required to produce all three figures.

Options
-------
--csv  [LABEL:]PATH  Workload summary CSV, optionally prefixed with a display
                     label.  May be repeated up to 8 times.
--out  PATH          Base output path.  Extension sets format (.pdf/.png/.svg).
                     Actual files are written as:
                         <stem>_fig1_throughput.<ext>
                         <stem>_fig2_diskio_mem.<ext>
                         <stem>_fig3_cpu.<ext>
                     Default: bottleneck_characterization.pdf
--disk-ceiling VAL   Draw a horizontal reference line at VAL MB/s on the disk
                     I/O panel (e.g. measured fio mixed-RW device ceiling).
--no-bands           Use thin error bars instead of shaded std-dev bands.
--normalize-x        Restrict X-axis to thread counts present in ALL workloads
                     (useful when sweeps used different thread sets).
--width  INCHES      Figure width.  Default: 7.0 (double-column IEEE/ACM).
                     Use 3.5 for single-column.
--dpi    INT         Raster DPI (ignored for PDF/SVG).  Default: 300.
--font-size INT      Base font size.  Default: 8.

Expected CSV columns (produced by saturation_sweep.sh)
------------------------------------------------------
workload_label, threads,
xput_mean, xput_std,
cpu_compute_mean, cpu_active_std,   # 100 - idle - iowait  (pure compute)
cpu_busy_mean,                       # 100 - idle            (incl. iowait)
cpu_iowait_mean,                     # iowait %  (gap between the two)
disk_read_mb/s, disk_read_std, disk_write_mb/s, disk_write_std,
r/s, r/s_std, w/s, w/s_std,
disk_bandwidth_pct, disk_bandwidth_pct_std, iops_pct, iops_pct_std,
mem_used_mean, mem_used_std, mem_avail_mean, mem_used_pct_mean,
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
        description='Plot RocksDB bottleneck characterization — three separate figures.')
    p.add_argument('--csv', metavar='[LABEL:]PATH', action='append', required=True,
                   dest='csvs',
                   help='Workload summary CSV. May be repeated.')
    p.add_argument('--out', default='bottleneck.pdf',
                   help='Base output path. Extension sets format. Three files are written '
                        'with suffixes _throughput, _diskio_mem, _cpu.')
    p.add_argument('--disk-ceiling', type=float, default=0.0, metavar='MB/s',
                   help='Horizontal reference line on the disk I/O panel (MB/s).')
    p.add_argument('--no-bands', action='store_true',
                   help='Use error bars instead of shaded std-dev bands.')
    p.add_argument('--normalize-x', action='store_true',
                   help='Restrict X-axis to thread counts common to all workloads.')
    p.add_argument('--width',  type=float, default=7.0,  metavar='INCHES')
    p.add_argument('--dpi',       type=int, default=300)
    p.add_argument('--font-size', type=int, default=12, dest='font_size')
    return p.parse_args()

# ── Output path helpers ───────────────────────────────────────────────────────

def _out_paths(base: str):
    """Return the three output file paths derived from base."""
    root, ext = os.path.splitext(base)
    if not ext:
        ext = '.pdf'
    return (
        f'{root}_throughput{ext}',
        f'{root}_diskio_mem{ext}',
        f'{root}_cpu{ext}',
    )

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
                'cpu_compute_mean', 'cpu_active_std',
                'cpu_busy_mean', 'cpu_iowait_mean',
                'disk_read_mb/s', 'disk_read_std',
                'disk_write_mb/s', 'disk_write_std',
                'r/s', 'r/s_std', 'w/s', 'w/s_std',
                'disk_bandwidth_pct', 'disk_bandwidth_pct_std',
                'iops_pct', 'iops_pct_std',
                'mem_used_mean', 'mem_used_std',
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

def _plot_line(ax, x, mean, std, color, label, lw, ms, no_bands, linestyle='-'):
    """Plot a mean line ± std-dev.  std may be all-NaN (treated as zero)."""
    mean = np.asarray(mean, dtype=float)
    std  = np.asarray(std,  dtype=float)
    std  = np.where(np.isnan(std), 0, std)

    marker = 'o' if linestyle == '-' else 's'

    ax.errorbar(x, mean, yerr=std,
                color=color, linewidth=lw, markersize=ms,
                marker=marker, linestyle=linestyle,
                capsize=3, capthick=0.8, elinewidth=0.8,
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
    The step is rounded to the nearest "nice" value (1,2,4,5,8,10,16,20,25,32…)
    so labels fall on clean numbers like 1, 8, 16, 24, 32.
    """
    ax.set_xticks(x_domain)
    ax.tick_params(axis='x', labelrotation=45 if (rotate and len(x_domain) > 100) else 0)

    if max_labels is not None and len(x_domain) > max_labels:
        span = x_domain[-1] - x_domain[0]
        raw_step = span / (max_labels - 1)
        nice = [1, 2, 4, 5, 8, 10, 16, 20, 24, 25, 32, 50, 64, 100]
        step = min(nice, key=lambda s: abs(s - raw_step))
        # Show a label only if the value is the first tick OR a multiple of step.
        visible = {x_domain[0]} | {x for x in x_domain if x % step == 0}
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
        color = _PALETTE[idx % len(_PALETTE)]
        df = df_full[df_full['threads'].isin(x_domain)].copy() \
             if args.normalize_x else df_full.copy()
        x = df['threads'].values
        knee = find_knee(df)

        _plot_line(ax, x, df['xput_mean'], df['xput_std'],
                   color, label, lw, ms, args.no_bands)
        #_annotate_knee(ax, df_full, x_domain, args.normalize_x,
        #               idx, color, lw, ms, fsz)

        legend_handles.append(
            Line2D([0], [0], color=color, linewidth=lw, marker='o',
                   markersize=ms, label=label))

    ax.set_ylabel('Throughput (qps)', fontsize=12)
    ax.set_ylim(bottom=0)
    _set_xticks(ax, x_domain)

    ax.legend(handles=legend_handles,
              loc='best', ncol=min(n_wl, 2),
              fontsize=fsz-1, framealpha=0.85)

    fig.supxlabel('Client threads', fontsize=12)
    fig.tight_layout()
    return fig

# ═══════════════════════════════════════════════════════════════════════════════
# Figure 2 — Disk I/O and Memory Used (2nd & 3rd CSV, side by side)
# ═══════════════════════════════════════════════════════════════════════════════

def build_fig2_diskio_mem(datasets_23, args):
    """
    Layout: 2 columns × 2 rows via GridSpec.
      Left column  (spans both rows) — disk bandwidth utilization %.
      Right column, top row          — IOPS utilization %.
      Right column, bottom row       — memory used %.

    Colors mirror Figure 1: datasets_23[0] gets palette index 1 (2nd CSV),
    datasets_23[1] gets palette index 2 (3rd CSV).
    """
    lw, ms, fsz = 1.5, 4, args.font_size
    # Taller than the old 2-panel figure to give the stacked right panels room.
    fig_h = round(args.width * 0.65, 2)

    _apply_rcparams(fsz)
    fig = plt.figure(figsize=(args.width, fig_h))
    
    # We will use subplots_adjust to force the panels to use the full width,
    # rather than tight_layout which might shrink them.
    fig.subplots_adjust(left=0.08, right=0.97, top=0.88, bottom=0.12, wspace=0.25)
    gs  = gridspec.GridSpec(5, 2, figure=fig, hspace=0.60)

    ax_disk = fig.add_subplot(gs[:, 0])          # left, full height
    ax_iops = fig.add_subplot(gs[0:3, 1])          # right, top
    ax_mem  = fig.add_subplot(gs[3:5, 1],          # right, bottom — share x with iops
                              sharex=ax_iops)

    x_domain = _resolve_x_domain(datasets_23, args.normalize_x)
    legend_handles = []

    for idx, (label, df_full) in enumerate(datasets_23):
        # Palette index 1 for 2nd CSV, 2 for 3rd — mirrors Fig 1.
        color = _PALETTE[(idx) % len(_PALETTE)]
        df = df_full[df_full['threads'].isin(x_domain)].copy() \
             if args.normalize_x else df_full.copy()
        x    = df['threads'].values
        knee = find_knee(df)

        # ── Disk bandwidth % ──────────────────────────────────────────────────
        _plot_line(ax_disk, x, df['disk_bandwidth_pct'], df['disk_bandwidth_pct_std'],
                   color, label, lw, ms, args.no_bands)
        #_knee_vline(ax_disk, knee, color)
        if args.disk_ceiling > 0 and idx == 0:
            ax_disk.axhline(args.disk_ceiling, color='black', linestyle=':',
                            linewidth=0.9, alpha=0.7,
                            label=f'ceiling ({args.disk_ceiling:,.0f} MB/s)')

        # ── IOPS % ───────────────────────────────────────────────────────────
        _plot_line(ax_iops, x, df['iops_pct'], df['iops_pct_std'],
                   color, label, lw, ms, args.no_bands)
        #_knee_vline(ax_iops, knee, color)

        # ── Memory used % ────────────────────────────────────────────────────
        _plot_line(ax_mem, x, df['mem_used_pct_mean'],
                   np.zeros(len(df)),
                   color, label, lw, ms, args.no_bands)
        #_knee_vline(ax_mem, knee, color)

        legend_handles.append(
            Line2D([0], [0], color=color, linewidth=lw, marker='o',
                   markersize=ms, label=label))
    
    # ── Decoration ────────────────────────────────────────────────────────────
    ax_disk.set_ylabel('I/O bandwidth (%)', fontsize=12)
    ax_disk.set_ylim(0, 105)
    ax_disk.axhline(100, color='black', linestyle=':', linewidth=0.8, alpha=0.55)
    _set_xticks(ax_disk, x_domain, max_labels=4)
    ax_disk.text(0.02, 0.98, '(A) Disk I/O Utilization', transform=ax_disk.transAxes, fontsize=12, fontweight='bold')

    ax_iops.set_ylabel('IOPS (%)', fontsize=12)
    ax_iops.set_ylim(bottom=0)          # auto-scale top; values >100% are valid
    ax_iops.axhline(100, color='black', linestyle=':', linewidth=0.8, alpha=0.55)
   
    # Hide x tick labels on the top-right panel; bottom one shows them.
    plt.setp(ax_iops.get_xticklabels(), visible=False)
    ax_iops.set_xlabel('')
    ax_iops.text(0.02, 0.98, '(B) IOPS Utilization', transform=ax_iops.transAxes, fontsize=12, fontweight='bold')

    ax_mem.set_ylabel('Mem used (%)', fontsize=12)
    ax_mem.set_ylim(0, 10)
    ax_mem.axhline(100, color='black', linestyle=':', linewidth=0.8, alpha=0.55)
    _set_xticks(ax_mem, x_domain, max_labels=4)
    ax_mem.text(0.02, 0.98, '(C) Mem Used', transform=ax_mem.transAxes, fontsize=12, fontweight='bold')

    # Single shared legend centered above all panels in one row.
    fig.legend(handles=legend_handles,
               loc='lower center',
               ncol=len(legend_handles),
               fontsize=fsz-1,
               framealpha=0.85,
               mode='expand',
               bbox_to_anchor=(0.08, 0.90, 0.89, 0.08))
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
    Each panel has three semantic fill areas (same colors in both panels):
      light purple — compute  (0 → solid cpu_compute line)
      light pink   — iowait   (cpu_compute → dashed cpu_busy line)
      light brown  — idle     (cpu_busy → 100 %)
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
        # Line color: matches workload palette index
        color = _PALETTE[idx % len(_PALETTE)]

        df   = df_full[df_full['threads'].isin(x_domain)].copy() \
               if args.normalize_x else df_full.copy()
        x    = df['threads'].values
        comp = np.asarray(df['cpu_compute_mean'], dtype=float)
        busy = np.asarray(df['cpu_busy_mean'],    dtype=float)

        # ── Hatched mesh for iowait (between compute and busy line) ───────────
        ax.fill_between(x, comp, busy, facecolor='none', edgecolor=color, hatch='xxxx', alpha=0.5, zorder=1)

        # ── Boundary lines ────────────────────────────────────────────────────
        _plot_line(ax, x, comp, df['cpu_active_std'],
                   color, 'cpu compute',  lw, ms, args.no_bands, linestyle='-')
        _plot_line(ax, x, busy, df['cpu_active_std'],
                   color, 'cpu busy',     lw, ms, args.no_bands, linestyle='--')

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
               markersize=ms, label='cpu compute'),
        Line2D([0], [0], color='gray', linestyle='--', linewidth=lw, marker='s',
               markersize=ms, label='cpu scheduled'),
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

    out1, out2, out3 = _out_paths(args.out)

    print('\nBuilding Figure 1 — Throughput (all workloads) …')
    fig1 = build_fig1_throughput(datasets, args)
    _save(fig1, out1, args.dpi)

    if len(datasets) >= 2:
        datasets_23 = datasets[1:3]  # 2nd and 3rd CSV (0-indexed: 1, 2)

        print('\nBuilding Figure 2 — Disk I/O & Memory (2nd & 3rd workload) …')
        fig2 = build_fig2_diskio_mem(datasets, args)
        _save(fig2, out2, args.dpi)

        print('\nBuilding Figure 3 — CPU utilization (2nd & 3rd workload, side by side) …')
        fig3 = build_fig3_cpu(datasets, args)
        _save(fig3, out3, args.dpi)
    else:
        print('\nSkipping Figures 2 & 3: need at least 2 CSVs for the 2nd and 3rd entries.')

    print('\nDone.')

if __name__ == '__main__':
    main()
