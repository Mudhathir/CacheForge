#!/usr/bin/env python3
import sqlite3, re, os, sys
from pathlib import Path
import pandas as pd
import matplotlib.pyplot as plt

DB = Path("DB/funsearch.db")
OUT_DIR = Path("results")
PLOT_DIR = Path("plots")
OUT_DIR.mkdir(exist_ok=True, parents=True)
PLOT_DIR.mkdir(exist_ok=True, parents=True)

def read_db():
    con = sqlite3.connect(DB)
    df = pd.read_sql_query("SELECT workload, policy, policy_description, cpp_file_path, cache_hit_rate, score FROM experiments", con)
    con.close()
    # Keep only known workloads
    keep = {"astar","lbm","mcf","milc","omnetpp","all"}
    df = df[df["workload"].isin(keep)].copy()
    return df

def infer_iter(cpp_path:str)->int:
    # filenames like: 003_triship.cc  →  3
    m = re.match(r".*?([0-9]{3})_", Path(cpp_path).name)
    return int(m.group(1)) if m else -1

def export_all(df):
    df.to_csv(OUT_DIR/"all_runs.csv", index=False)

def pivot_by_policy(df):
    # average across workloads (exclude 'all' rows when computing mean)
    df_wo_all = df[df.workload != "all"].copy()
    piv = df_wo_all.pivot_table(index="policy", columns="workload", values="cache_hit_rate", aggfunc="max")
    piv["average"] = piv.mean(axis=1)
    piv.sort_values("average", ascending=False, inplace=True)
    piv.to_csv(OUT_DIR/"pivot_by_policy.csv")
    return piv

def top_policy_name(piv):
    if len(piv)==0: return None
    return piv.index[0]

def plot_top_policy_bars(piv, title="Top policy vs. workloads (LLC hit rate)"):
    if len(piv)==0: return
    top = piv.iloc[0:1].drop(columns=["average"])
    fig = plt.figure()
    top.T.plot(kind="bar", legend=False)
    plt.ylabel("LLC hit rate")
    plt.ylim(0, 1.0)
    plt.title(f"{title}\n{top.index[0]}")
    plt.tight_layout()
    plt.savefig(PLOT_DIR/"top_policy_per_workload.png", dpi=200)
    plt.close()

def best_so_far_vs_iter(df):
    # Use per-(policy,workload) rows; infer iteration from filename
    df = df.copy()
    df["iter"] = df["cpp_file_path"].apply(infer_iter)
    df = df[(df.iter >= 0) & (df.workload != "all")]
    # For each iteration, compute the mean hit rate of that policy across workloads
    grouped = df.groupby(["iter","policy"])["cache_hit_rate"].mean().reset_index()
    best_per_iter = grouped.sort_values(["iter","cache_hit_rate"], ascending=[True,False]).drop_duplicates(["iter"])
    best_per_iter.to_csv(OUT_DIR/"best_per_iter.csv", index=False)

    fig = plt.figure()
    plt.plot(best_per_iter["iter"], best_per_iter["cache_hit_rate"], marker="o")
    plt.xlabel("Iteration")
    plt.ylabel("Best mean LLC hit rate at iteration")
    plt.ylim(0, 1.0)
    plt.title("Best-so-far mean hit rate vs. iteration")
    plt.grid(True, linestyle="--", alpha=0.4)
    plt.tight_layout()
    plt.savefig(PLOT_DIR/"best_so_far_vs_iteration.png", dpi=200)
    plt.close()

def quick_ablations(df):
    # Iteration effect: already produced as best_so_far_vs_iter()
    # Temperature/Mutation: if you varied and stored in policy name/desc, you can grep below.
    pass

def area_table_example():
    """
    Example: SHiP/RRIP-like metadata accounting to show ≤64KiB budget at LLC scope,
    matching HW1 guidance (2MiB, 16-way, 64B lines ⇒ 32,768 lines; 2,048 sets). 
    Edit bits below for your *actual* best policy.
    """
    import math
    num_lines = 32768
    num_sets  = 2048

    # EDIT to match your best policy’s metadata needs:
    per_line_bits = {
        "RRPV": 2,                 # RRIP 2-bit counter
        "signature_id": 10,        # SHiP signature index
        "segment_bit": 1,          # protect/probation
        "thrash_tag": 1,           # thrash/volatility bit
    }
    per_set_bits = {
        # e.g., set-dueling state, small saturating counters per set
        "duel_counter": 4
    }
    global_bits = {
        # e.g., SHCT table (2 bits × 2048 entries)
        "SHCT": 2 * 2048
    }

    per_line_total = sum(per_line_bits.values())
    per_set_total  = sum(per_set_bits.values())
    global_total   = sum(global_bits.values())

    extra_bits = per_line_total*num_lines + per_set_total*num_sets + global_total
    extra_bytes = extra_bits/8.0
    kib = extra_bytes/1024.0

    rows = []
    for k,v in per_line_bits.items(): rows.append(("per-line", k, v, f"{v*num_lines/8/1024:.2f} KiB"))
    for k,v in per_set_bits.items():  rows.append(("per-set", k, v, f"{v*num_sets/8/1024:.2f} KiB"))
    for k,v in global_bits.items():   rows.append(("global",  k, v, f"{v/8/1024:.2f} KiB"))
    rows.append(("TOTAL", "", extra_bits, f"{kib:.2f} KiB"))

    df = pd.DataFrame(rows, columns=["scope","field","bits","approx_size"])
    df.to_csv(OUT_DIR/"area_table_example.csv", index=False)

def main():
    if not DB.exists():
        print(f"DB not found: {DB}")
        sys.exit(1)

    df = read_db()
    export_all(df)
    piv = pivot_by_policy(df)
    plot_top_policy_bars(piv)
    best_so_far_vs_iter(df)
    area_table_example()

    # Also dump top-5 policies by average
    (piv.head(5)).to_csv(OUT_DIR/"top5_policies.csv")

    # Print quick summary
    top = piv.index[0] if len(piv)>0 else "N/A"
    print(f"[OK] Wrote CSVs to {OUT_DIR}/ and plots to {PLOT_DIR}/")
    print(f"[OK] Top policy by average hit-rate: {top}")

if __name__ == "__main__":
    main()
