#!/usr/bin/env python3
"""
add_disk_bw_util.py — append disk bandwidth utilisation columns to a summary CSV.

Added columns
-------------
  disk_bw_avg    = disk_read_mb/s  / disk_ceiling_read
                 + disk_write_mb/s / disk_ceiling_write

  disk_bw_stddev = sqrt( (disk_read_mb/s_std  / disk_ceiling_read)²
                       + (disk_write_mb/s_std / disk_ceiling_write)² )

Both values are fractions (multiply by 100 for %).
The stddev formula assumes read and write variation are independent
(quadrature sum); a simple additive sum would overestimate spread.

Usage
-----
    python3 add_disk_bw_util.py \\
        --input  results/summary.csv \\
        --output results/summary_with_diskbw.csv \\
        --disk-ceiling-read  2800 \\
        --disk-ceiling-write  900
"""

import argparse
import sys
import pandas as pd


def parse_args():
    p = argparse.ArgumentParser(
        description='Add disk_bw_avg and disk_bw_stddev columns to a summary CSV.')
    p.add_argument('--input',  '-i', required=True, metavar='PATH',
                   help='Input CSV file.')
    p.add_argument('--output', '-o', default=None, metavar='PATH',
                   help='Output CSV file.  Defaults to overwriting the input file.')
    p.add_argument('--disk-ceiling-read',  type=float, required=True,
                   metavar='MB/s', dest='disk_ceiling_read',
                   help='Disk read throughput ceiling in MB/s.')
    p.add_argument('--disk-ceiling-write', type=float, required=True,
                   metavar='MB/s', dest='disk_ceiling_write',
                   help='Disk write throughput ceiling in MB/s.')
    return p.parse_args()


def main():
    args = parse_args()

    if args.disk_ceiling_read <= 0:
        sys.exit('ERROR: --disk-ceiling-read must be > 0')
    if args.disk_ceiling_write <= 0:
        sys.exit('ERROR: --disk-ceiling-write must be > 0')

    df = pd.read_csv(args.input)

    required = {'disk_read_mb/s', 'disk_read_mb/s_std',
                'disk_write_mb/s', 'disk_write_mb/s_std'}

    missing = required - set(df.columns)
    if missing:
        sys.exit(f'ERROR: input CSV is missing columns: {sorted(missing)}')

    cr = args.disk_ceiling_read
    cw = args.disk_ceiling_write

    df['disk_bw_avg'] = (
        df['disk_read_mb/s']      / cr +
        df['disk_write_mb/s']     / cw
    )

    df['disk_bw_stddev'] = (
        (df['disk_read_mb/s_std']  / cr) ** 2 +
        (df['disk_write_mb/s_std'] / cw) ** 2
    ) ** 0.5

    out_path = args.output or args.input
    df.to_csv(out_path, index=False)
    print(f'Written → {out_path}')
    print(f'  disk_bw_avg    range: [{df["disk_bw_avg"].min():.4f}, '
          f'{df["disk_bw_avg"].max():.4f}]')
    print(f'  disk_bw_stddev range: [{df["disk_bw_stddev"].min():.4f}, '
          f'{df["disk_bw_stddev"].max():.4f}]')


if __name__ == '__main__':
    main()
