#!/usr/bin/env python3
"""
analyze.py – Parse gossip experiment logs and compute:
  - Convergence time (time to 95% coverage)
  - Message overhead (total sent messages up to 95% coverage)
  - Mean + std-dev across seeds for each (N, mode) combination

Directory structure expected (produced by experiment.sh):
  results/
    push_only/
      N10/seed42/node_*.log
      N10/seed123/node_*.log
      ...
    hybrid/
      N10/seed42/node_*.log
      ...

Each log line: <timestamp_ms>,<event>,<msg_type>,<msg_id>
"""

import os
import glob
import math
from collections import defaultdict

# ----------------------------------------------------------------
# Config
# ----------------------------------------------------------------
RESULTS_DIR = "results"
TARGET_COVERAGE = 0.95   # 95 %
MODES = ["push_only", "hybrid"]
SIZES = [10, 20, 50]

# ----------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------

def mean(values):
    if not values:
        return float("nan")
    return sum(values) / len(values)

def std(values):
    if len(values) < 2:
        return 0.0
    m = mean(values)
    return math.sqrt(sum((v - m) ** 2 for v in values) / (len(values) - 1))


def parse_logs(log_dir):
    """
    Read all node_*.log files in log_dir.
    Returns:
      events  – list of (timestamp_ms, event, msg_type, msg_id)
      n_nodes – number of log files found
    """
    log_files = glob.glob(os.path.join(log_dir, "node_*.log"))
    n_nodes = len(log_files)
    events = []
    for f in log_files:
        with open(f) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                parts = line.split(",", 3)
                if len(parts) < 4:
                    continue
                ts, event, msg_type, msg_id = parts
                try:
                    events.append((int(ts), event, msg_type, msg_id))
                except ValueError:
                    pass
    return events, n_nodes


def analyze_run(log_dir):
    """
    Returns a dict with keys:
      convergence_ms  – time to 95% gossip coverage (or None)
      total_sent      – total SEND events up to convergence
      n_nodes         – cluster size
    """
    events, n_nodes = parse_logs(log_dir)
    if not events or n_nodes == 0:
        return None

    # Group GOSSIP events by msg_id
    gossip_sends    = defaultdict(list)   # msg_id -> [ts, ...]
    gossip_receives = defaultdict(list)   # msg_id -> [ts, ...]
    all_sends       = []                  # all SEND timestamps regardless of type

    for ts, event, msg_type, msg_id in events:
        if event == "SEND":
            all_sends.append(ts)
        if msg_type == "GOSSIP":
            if event == "SEND":
                gossip_sends[msg_id].append(ts)
            elif event == "RECEIVE":
                gossip_receives[msg_id].append(ts)

    # Find the message with the most coverage
    best_convergence = None
    best_overhead    = None

    for msg_id, receives in gossip_receives.items():
        sends = gossip_sends.get(msg_id, [])
        if not sends:
            continue

        origin_ts = min(sends)
        target    = math.ceil(TARGET_COVERAGE * n_nodes)
        receives_sorted = sorted(receives)

        if len(receives_sorted) >= target:
            conv_ts  = receives_sorted[target - 1]
            conv_ms  = conv_ts - origin_ts

            # Count ALL sent messages (any type) up to conv_ts
            overhead = sum(1 for ts in all_sends if origin_ts <= ts <= conv_ts)

            if best_convergence is None or conv_ms < best_convergence:
                best_convergence = conv_ms
                best_overhead    = overhead

    if best_convergence is None:
        return {"convergence_ms": None, "total_sent": None,
                "n_nodes": n_nodes}

    return {"convergence_ms": best_convergence,
            "total_sent":     best_overhead,
            "n_nodes":        n_nodes}


# ----------------------------------------------------------------
# Main
# ----------------------------------------------------------------

def collect_results():
    """
    Walk results/ and return a nested dict:
      data[mode][N] = list of {convergence_ms, total_sent}
    """
    data = {mode: {N: [] for N in SIZES} for mode in MODES}

    for mode in MODES:
        for N in SIZES:
            seed_dirs = glob.glob(
                os.path.join(RESULTS_DIR, mode, f"N{N}", "seed*"))
            for sd in seed_dirs:
                result = analyze_run(sd)
                if result and result["convergence_ms"] is not None:
                    data[mode][N].append(result)

    return data


def print_table(data):
    print("\n" + "=" * 72)
    print(f"{'Mode':<12} {'N':>4}  {'Seeds':>5}  "
          f"{'Conv(ms) mean':>14} {'±std':>8}  "
          f"{'Overhead mean':>14} {'±std':>8}")
    print("-" * 72)

    for mode in MODES:
        for N in SIZES:
            runs = data[mode][N]
            conv_vals = [r["convergence_ms"] for r in runs
                         if r["convergence_ms"] is not None]
            over_vals = [r["total_sent"] for r in runs
                         if r["total_sent"] is not None]

            c_mean = mean(conv_vals) if conv_vals else float("nan")
            c_std  = std(conv_vals)  if conv_vals else float("nan")
            o_mean = mean(over_vals) if over_vals else float("nan")
            o_std  = std(over_vals)  if over_vals else float("nan")

            print(f"{mode:<12} {N:>4}  {len(conv_vals):>5}  "
                  f"{c_mean:>14.1f} {c_std:>8.1f}  "
                  f"{o_mean:>14.1f} {o_std:>8.1f}")
        print()


def make_plots(data):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("[!] matplotlib not installed – skipping plots.")
        return

    colors = {"push_only": "#e74c3c", "hybrid": "#2980b9"}
    labels = {"push_only": "Push-only", "hybrid": "Hybrid Push-Pull"}

    # ---- Convergence plot ----
    fig, axes = plt.subplots(1, 2, figsize=(13, 5))

    for mode in MODES:
        ns, means_, stds_ = [], [], []
        for N in SIZES:
            runs = data[mode][N]
            vals = [r["convergence_ms"] for r in runs
                    if r["convergence_ms"] is not None]
            if vals:
                ns.append(N)
                means_.append(mean(vals))
                stds_.append(std(vals))

        axes[0].errorbar(ns, means_, yerr=stds_, marker="o",
                         label=labels[mode], color=colors[mode],
                         capsize=4, linewidth=2)

    axes[0].set_xlabel("Number of Nodes (N)")
    axes[0].set_ylabel("Convergence Time (ms)")
    axes[0].set_title("95% Convergence Time vs Network Size")
    axes[0].legend()
    axes[0].grid(True, alpha=0.3)

    # ---- Overhead plot ----
    for mode in MODES:
        ns, means_, stds_ = [], [], []
        for N in SIZES:
            runs = data[mode][N]
            vals = [r["total_sent"] for r in runs
                    if r["total_sent"] is not None]
            if vals:
                ns.append(N)
                means_.append(mean(vals))
                stds_.append(std(vals))

        axes[1].errorbar(ns, means_, yerr=stds_, marker="s",
                         label=labels[mode], color=colors[mode],
                         capsize=4, linewidth=2)

    axes[1].set_xlabel("Number of Nodes (N)")
    axes[1].set_ylabel("Total Messages Sent")
    axes[1].set_title("Message Overhead vs Network Size")
    axes[1].legend()
    axes[1].grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig("experiment_results.png", dpi=150)
    print("\n[+] Plot saved to experiment_results.png")


if __name__ == "__main__":
    data = collect_results()
    print_table(data)
    make_plots(data)
