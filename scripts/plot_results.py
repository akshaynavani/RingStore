#!/usr/bin/env python3
"""Plot IOPS and p99 latency vs. queue depth from a ringstore_bench CSV.

    python3 scripts/plot_results.py results/sweep.csv --out results/

Produces one PNG per workload with a line per (engine, block size).
Requires pandas and matplotlib.
"""
import argparse
import os

import matplotlib.pyplot as plt
import pandas as pd


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--out", default="results")
    args = ap.parse_args()

    df = pd.read_csv(args.csv)
    os.makedirs(args.out, exist_ok=True)

    for workload, wdf in df.groupby("workload"):
        fig, (ax_iops, ax_lat) = plt.subplots(1, 2, figsize=(12, 4.5))
        for (engine, bs, fixed), g in wdf.groupby(["engine", "bs", "fixed_bufs"]):
            g = g.sort_values("qd")
            label = f"{engine} bs={bs // 1024}k" + (" fixed" if fixed else "")
            ax_iops.plot(g["qd"], g["iops"] / 1000.0, marker="o", label=label)
            ax_lat.plot(g["qd"], g["p99_us"], marker="o", label=label)

        for ax in (ax_iops, ax_lat):
            ax.set_xscale("log", base=2)
            ax.set_xlabel("queue depth")
            ax.grid(True, which="both", alpha=0.3)
        ax_iops.set_ylabel("kIOPS")
        ax_iops.set_title(f"{workload}: throughput")
        ax_lat.set_yscale("log")
        ax_lat.set_ylabel("p99 latency (us)")
        ax_lat.set_title(f"{workload}: tail latency")
        ax_lat.legend(fontsize="small")

        fig.tight_layout()
        path = os.path.join(args.out, f"{workload}.png")
        fig.savefig(path, dpi=120)
        plt.close(fig)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
