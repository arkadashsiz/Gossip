#!/usr/bin/env python


import os
import glob
import math
import sys
from collections import defaultdict

# ── config ───────────────────────────────────────────────────────────────────
RESULTS_DIR     = "results"
TARGET_COVERAGE = 0.95
EXPECTED_SIZES  = [10, 20, 50]
# ─────────────────────────────────────────────────────────────────────────────


# ── statistics ────────────────────────────────────────────────────────────────
def _mean(vals):
    return sum(vals) / len(vals) if vals else float("nan")

def _std(vals):
    if len(vals) < 2:
        return 0.0
    m = _mean(vals)
    return math.sqrt(sum((v - m) ** 2 for v in vals) / (len(vals) - 1))
# ─────────────────────────────────────────────────────────────────────────────


def _load_dir(log_dir):
    """
    Read all node_*.log files from log_dir.
    Returns  per_node: dict  port(int) -> [(ts, event, msg_type, msg_id), ...]
    """
    per_node = {}
    for path in glob.glob(os.path.join(log_dir, "node_*.log")):
        try:
            port = int(os.path.basename(path)
                         .replace("node_", "").replace(".log", ""))
        except ValueError:
            continue
        rows = []
        with open(path) as fh:
            for line in fh:
                parts = line.strip().split(",", 3)
                if len(parts) == 4:
                    try:
                        rows.append((int(parts[0]), parts[1],
                                     parts[2], parts[3]))
                    except ValueError:
                        pass
        per_node[port] = rows
    return per_node


def _find_injector(per_node):
    """
    Return (injector_port, msg_id, origin_ts).

    The injector is the node with the globally earliest SEND,GOSSIP
    timestamp.  Ties are broken by picking the highest port number
    (injector is always on a high port like 7xxx while cluster starts
    at 5000).
    """
    best = None   # (ts, port, msg_id)
    for port, rows in per_node.items():
        for ts, ev, mt, mid in rows:
            if ev == "SEND" and mt == "GOSSIP":
                if (best is None
                        or ts < best[0]
                        or (ts == best[0] and port > best[1])):
                    best = (ts, port, mid)
                break   # earliest SEND per file is enough for comparison
    if best is None:
        return None, None, None
    return best[1], best[2], best[0]


def analyze_run(log_dir, declared_n):
    """
    Analyse one seed directory.

    Returns a dict:
        n_nodes         – number of cluster nodes (logs minus injector)
        convergence_ms  – ms to 95% coverage, or None
        total_sent      – total SEND events origin..convergence, or None
        coverage        – fraction of cluster nodes that got the message
    or None if the directory has no log files.
    """
    per_node = _load_dir(log_dir)
    if not per_node:
        return None

    injector_port, msg_id, origin_ts = _find_injector(per_node)
    if msg_id is None:
        return {"n_nodes": declared_n, "convergence_ms": None,
                "total_sent": None, "coverage": 0.0}

    # Cluster = everything that is NOT the injector
    cluster_ports = [p for p in per_node if p != injector_port]
    n_nodes = len(cluster_ports) if cluster_ports else declared_n

    # Earliest RECEIVE,GOSSIP timestamp per cluster node for this msg_id
    receive_times = []
    for port in cluster_ports:
        for ts, ev, mt, mid in per_node[port]:
            if ev == "RECEIVE" and mt == "GOSSIP" and mid == msg_id:
                receive_times.append(ts)
                break   # one per node

    coverage = len(receive_times) / n_nodes

    target_count = math.ceil(TARGET_COVERAGE * n_nodes)
    if len(receive_times) < target_count:
        return {"n_nodes": n_nodes, "convergence_ms": None,
                "total_sent": None, "coverage": coverage}

    receive_times.sort()
    conv_ts = receive_times[target_count - 1]
    conv_ms = conv_ts - origin_ts

    # Overhead = every SEND (any type) across all logs in [origin_ts, conv_ts]
    total_sent = sum(
        1
        for rows in per_node.values()
        for ts, ev, _, _ in rows
        if ev == "SEND" and origin_ts <= ts <= conv_ts
    )

    return {"n_nodes": n_nodes, "convergence_ms": conv_ms,
            "total_sent": total_sent, "coverage": coverage}


# ── discovery ────────────────────────────────────────────────────────────────

def collect_results(results_dir=RESULTS_DIR):
    """
    Walk results_dir and return  data[mode][N] = [run_dict, ...]
    Modes and N values are auto-discovered from the directory tree.
    """
    if not os.path.isdir(results_dir):
        print(f"[!] '{results_dir}' not found – run experiment.sh first.")
        return {}

    modes = sorted(
        d for d in os.listdir(results_dir)
        if os.path.isdir(os.path.join(results_dir, d))
    )
    if not modes:
        print(f"[!] No subdirectories in '{results_dir}'.")
        return {}

    data = {}
    for mode in modes:
        data[mode] = {}
        mode_dir = os.path.join(results_dir, mode)
        # Discover N values
        sizes = []
        for d in os.listdir(mode_dir):
            if d.startswith("N"):
                try:
                    sizes.append(int(d[1:]))
                except ValueError:
                    pass
        for N in sorted(sizes):
            seed_dirs = sorted(glob.glob(
                os.path.join(mode_dir, f"N{N}", "seed*")))
            runs = []
            for sd in seed_dirs:
                result = analyze_run(sd, N)
                if result is None:
                    print(f"  [warn] no logs in {sd}")
                    continue
                result["seed"] = os.path.basename(sd)
                runs.append(result)
            data[mode][N] = runs

    return data


# ── reporting ─────────────────────────────────────────────────────────────────

def print_table(data):
    if not data:
        return
    sep = "─" * 80
    print()
    print(sep)
    print(f"{'Mode':<14} {'N':>4}  {'Runs':>4}  "
          f"{'Conv mean(ms)':>14} {'±std':>7}  "
          f"{'Overhead mean':>14} {'±std':>7}  "
          f"{'Avg cov':>8}")
    print(sep)

    for mode in sorted(data):
        first_row = True
        for N in sorted(data[mode]):
            runs = data[mode][N]
            cv = [r["convergence_ms"] for r in runs if r["convergence_ms"] is not None]
            ov = [r["total_sent"]     for r in runs if r["total_sent"]     is not None]
            covv = [r["coverage"]     for r in runs]

            def f(v):
                return f"{v:>10.1f}" if not math.isnan(v) else "       n/a"

            mode_col = mode if first_row else ""
            first_row = False
            print(f"{mode_col:<14} {N:>4}  {len(runs):>4}  "
                  f"{f(_mean(cv))} {f(_std(cv))}  "
                  f"{f(_mean(ov))} {f(_std(ov))}  "
                  f"{_mean(covv)*100:>7.1f}%")

            # Flag any seed that didn't converge
            for r in runs:
                if r["convergence_ms"] is None:
                    pct = r["coverage"] * 100
                    print(f"    [{r['seed']}]  did not reach "
                          f"{TARGET_COVERAGE*100:.0f}%  "
                          f"(coverage = {pct:.0f}%  "
                          f"nodes = {r['n_nodes']})")
        print()

    print(sep)


def print_comparison(data):
    """Side-by-side push vs hybrid summary (only if both modes present)."""
    if "push_only" not in data or "hybrid" not in data:
        return
    common = sorted(set(data["push_only"]) & set(data["hybrid"]))
    if not common:
        return

    print("\n── Push-only vs Hybrid ──────────────────────────────────────────────")
    print(f"{'N':>4}   {'Push conv':>18}   {'Hybrid conv':>18}   "
          f"{'Push OH':>16}   {'Hybrid OH':>16}")
    print("─" * 80)
    for N in common:
        def s(mode, key):
            vals = [r[key] for r in data[mode][N] if r[key] is not None]
            if not vals:
                return "     n/a"
            return f"{_mean(vals):>7.1f}±{_std(vals):<6.1f}"
        print(f"{N:>4}   {s('push_only','convergence_ms'):>18}   "
              f"{s('hybrid','convergence_ms'):>18}   "
              f"{s('push_only','total_sent'):>16}   "
              f"{s('hybrid','total_sent'):>16}")
    print()


def make_plots(data):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("[!] matplotlib not installed – skipping plots.")
        return
    if not data:
        return

    palette    = ["#e74c3c", "#2980b9", "#27ae60", "#8e44ad"]
    markers    = ["o", "s", "^", "D"]
    label_map  = {"push_only": "Push-only", "hybrid": "Hybrid Push-Pull"}
    all_modes  = sorted(data.keys())
    cmap       = {m: palette[i % len(palette)] for i, m in enumerate(all_modes)}
    mmap       = {m: markers[i % len(markers)] for i, m in enumerate(all_modes)}

    fig, (ax0, ax1) = plt.subplots(1, 2, figsize=(14, 5))

    for mode in all_modes:
        ns, cm, cs, om, os_ = [], [], [], [], []
        for N in sorted(data[mode]):
            cv = [r["convergence_ms"] for r in data[mode][N]
                  if r["convergence_ms"] is not None]
            ov = [r["total_sent"]     for r in data[mode][N]
                  if r["total_sent"]  is not None]
            if cv:
                ns.append(N)
                cm.append(_mean(cv));  cs.append(_std(cv))
                om.append(_mean(ov) if ov else float("nan"))
                os_.append(_std(ov) if len(ov) > 1 else 0.0)

        lbl = label_map.get(mode, mode)
        kw  = dict(color=cmap[mode], marker=mmap[mode],
                   capsize=4, linewidth=2, label=lbl)
        if ns:
            ax0.errorbar(ns, cm, yerr=cs,  **kw)
            ax1.errorbar(ns, om, yerr=os_, **kw)

    for ax, title, ylabel in [
        (ax0, "95% Convergence Time vs N", "Convergence time (ms)"),
        (ax1, "Message Overhead vs N",     "Total messages sent"),
    ]:
        ax.set_title(title, fontsize=12)
        ax.set_xlabel("Number of nodes (N)")
        ax.set_ylabel(ylabel)
        ax.legend()
        ax.grid(True, alpha=0.3)
        # Only set fixed ticks if we have data for those sizes
        all_ns = sorted({N for md in data.values() for N in md})
        if all_ns:
            ax.set_xticks(all_ns)

    plt.tight_layout()
    out = "experiment_results.png"
    plt.savefig(out, dpi=150)
    print(f"\n[+] Plot saved → {out}")


# ── main ─────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    rdir = sys.argv[1] if len(sys.argv) > 1 else RESULTS_DIR
    data = collect_results(rdir)
    print_table(data)
    print_comparison(data)
    make_plots(data)
