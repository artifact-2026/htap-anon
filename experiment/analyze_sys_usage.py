import pandas as pd
import re
import sys

def analyze_logs(vmstat_path, iostat_path):
    # ---- Parse vmstat ----
    vmstat_cols = [
        'r','b','swpd','free','buff','cache','si','so','bi','bo',
        'in','cs','us','sy','id','wa','st'
    ]

    vmstat_data = []
    with open(vmstat_path) as f:
        for line in f:
            if re.match(r"^\s*r\s+b", line):  # header line
                continue
            if line.startswith("procs "):  # repeated section header
                continue
            parts = line.strip().split()
            if len(parts) == len(vmstat_cols):
                vmstat_data.append(parts)

    df_vm = pd.DataFrame(vmstat_data, columns=vmstat_cols).astype(float)

    # ---- Parse iostat ----
    iostat_cols = [
        'Device','r/s','rkB/s','rrqm/s','%rrqm','r_await','rareq-sz',
        'w/s','wkB/s','wrqm/s','%wrqm','w_await','wareq-sz',
        'd/s','dkB/s','drqm/s','%drqm','d_await','dareq-sz',
        'f/s','f_await','aqu-sz','%util'
    ]

    iostat_data = []
    with open(iostat_path) as f:
        for line in f:
            if line.startswith("Device") or line.startswith("Linux") or line.strip() == "":
                continue
            parts = line.strip().split()
            if len(parts) == len(iostat_cols):
                iostat_data.append(parts)

    df_io = pd.DataFrame(iostat_data, columns=iostat_cols).astype({'%util': float, 'rkB/s': float, 'wkB/s': float})

    # Align by index (assuming same sampling rate)
    min_len = min(len(df_vm), len(df_io))
    df_vm = df_vm.iloc[:min_len].reset_index(drop=True)
    df_io = df_io.iloc[:min_len].reset_index(drop=True)

    # ---- Compute averages ----
    avg_user_cpu = df_vm['us'].mean()
    avg_sys_cpu = df_vm['sy'].mean()
    avg_idle_cpu = df_vm['id'].mean()
    avg_io_wait = df_vm['wa'].mean()
    avg_disk_util = df_io['%util'].mean()

    # ---- Verdict ----
    if avg_io_wait > 30 and avg_disk_util > 70:
        verdict = "Likely IO bound"
    elif (avg_user_cpu + avg_sys_cpu) > 70:
        verdict = "Likely CPU bound"
    else:
        verdict = "Not strongly bound by CPU or I/O"

    # ---- Print results ----
    print("=== Analyzing vmstat_log.txt ===")
    print(f"Avg user CPU:   {avg_user_cpu:.4f} %")
    print(f"Avg system CPU: {avg_sys_cpu:.5f} %")
    print(f"Avg idle CPU:   {avg_idle_cpu:.4f} %")
    print(f"Avg IO wait:    {avg_io_wait:.5f} %\n")

    print("=== Analyzing iostat_log.txt ===")
    print(f"Avg disk %util: {avg_disk_util:.4f} %\n")

    print(f"Verdict: {verdict}")


if __name__ == "__main__":
    # Default to vmstat_log.txt and iostat_log.txt in current dir if no args
    if len(sys.argv) == 3:
        vmstat_file = sys.argv[1]
        iostat_file = sys.argv[2]
    else:
        vmstat_file = "vmstat_log.txt"
        iostat_file = "iostat_log.txt"

    analyze_logs(vmstat_file, iostat_file)