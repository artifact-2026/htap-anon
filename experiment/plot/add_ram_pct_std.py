"""
add_ram_pct_std.py — append ram utilisation stddev column to a summary CSV.

Added columns
-------------
  mem_used_pct_std = ( mem_used_std  / mem_avail_mean )

Values is fraction (multiply by 100 for %).

Usage
-----
    python3 add_ram_pct_std.py \\
        --input  results/summary.csv \\
        --output results/summary_with_ram_pct_std.csv \\
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
    return p.parse_args()


def main():
    args = parse_args()

    df = pd.read_csv(args.input)

    required = {'mem_used_std', 'mem_avail_mean'}
    missing = required - set(df.columns)
    if missing:
        sys.exit(f'ERROR: input CSV is missing columns: {sorted(missing)}')

    df['mem_used_pct_stddev'] = (
        df['mem_used_std']      / df['mem_avail_mean']
    )

    out_path = args.output or args.input
    df.to_csv(out_path, index=False)
    print(f'Written → {out_path}')
    print(f'  mem_used_pct_stddev range: [{df["mem_used_pct_stddev"].min():.4f}, '
          f'{df["mem_used_pct_stddev"].max():.4f}]')


if __name__ == '__main__':
    main()