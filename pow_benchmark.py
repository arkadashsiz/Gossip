#!/usr/bin/env python3
"""
pow_benchmark.py
================
Benchmarks Proof-of-Work nonce generation for various difficulty levels (k).

Definition
----------
A valid nonce satisfies:
    SHA-256(node_id || str(nonce))  starts with k zero hex digits  (i.e., k/4 zero bytes)

Usage
-----
    python3 pow_benchmark.py [--trials N] [--k-values 2 4 6 8]

Output
------
- Console table with mean / std / min / max time and attempts per k
- pow_benchmark_results.csv   (raw data)
- pow_benchmark_plot.png      (bar chart of mean time vs k, with error bars)
"""

import argparse
import hashlib
import random
import time
import statistics
import csv
import os
import sys

# ── optional matplotlib ─────────────────────────────────────────────────────
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    HAS_PLOT = True
except ImportError:
    HAS_PLOT = False
    print("[WARN] matplotlib not found – plot will be skipped.", file=sys.stderr)


# ─────────────────────────────────────────────────────────────────────────────
# Core PoW logic
# ─────────────────────────────────────────────────────────────────────────────

def mine_nonce(node_id: str, k: int, start_nonce: int = 0) -> tuple[int, str, int]:
    """
    Find the smallest nonce >= start_nonce such that
        SHA-256(node_id || str(nonce)) starts with k hex zeros.

    Returns
    -------
    (nonce, digest_hex, attempts)
    """
    prefix = "0" * k
    nonce = start_nonce
    attempts = 0
    while True:
        raw = f"{node_id}{nonce}".encode()
        digest = hashlib.sha256(raw).hexdigest()
        attempts += 1
        if digest.startswith(prefix):
            return nonce, digest, attempts
        nonce += 1


def verify_pow(node_id: str, nonce: int, digest_hex: str, k: int) -> bool:
    """Return True if the digest is correct and starts with k zeros."""
    expected = hashlib.sha256(f"{node_id}{nonce}".encode()).hexdigest()
    return expected == digest_hex and digest_hex.startswith("0" * k)


# ─────────────────────────────────────────────────────────────────────────────
# Benchmark runner
# ─────────────────────────────────────────────────────────────────────────────

def benchmark_k(k: int, trials: int) -> dict:
    """
    Run `trials` independent mining tasks for difficulty k.
    Each trial uses a fresh random node_id and a random start nonce in [0, 1000].
    Returns a dict of timing/attempt statistics.
    """
    times_ms = []
    attempts_list = []

    for _ in range(trials):
        node_id = f"node-{random.getrandbits(64):016x}"
        start   = random.randint(0, 1000)

        t0 = time.perf_counter()
        nonce, digest, attempts = mine_nonce(node_id, k, start)
        elapsed_ms = (time.perf_counter() - t0) * 1000.0

        # Sanity-check every result
        assert verify_pow(node_id, nonce, digest, k), \
            f"PoW verification FAILED for k={k}, nonce={nonce}"

        times_ms.append(elapsed_ms)
        attempts_list.append(attempts)

    return {
        "k":              k,
        "trials":         trials,
        "mean_ms":        statistics.mean(times_ms),
        "stdev_ms":       statistics.stdev(times_ms) if trials > 1 else 0.0,
        "min_ms":         min(times_ms),
        "max_ms":         max(times_ms),
        "median_ms":      statistics.median(times_ms),
        "mean_attempts":  statistics.mean(attempts_list),
        "stdev_attempts": statistics.stdev(attempts_list) if trials > 1 else 0.0,
        "expected_attempts": 16 ** k,   # theoretical E[attempts] = 16^k
    }


# ─────────────────────────────────────────────────────────────────────────────
# Output helpers
# ─────────────────────────────────────────────────────────────────────────────

def print_table(results: list[dict]) -> None:
    header = (
        f"{'k':>4}  {'Mean (ms)':>12}  {'Std (ms)':>10}  "
        f"{'Min (ms)':>10}  {'Max (ms)':>10}  "
        f"{'Mean attempts':>15}  {'Expected':>12}"
    )
    sep = "-" * len(header)
    print("\n" + sep)
    print("  Proof-of-Work Benchmark Results")
    print(sep)
    print(header)
    print(sep)
    for r in results:
        print(
            f"{r['k']:>4}  {r['mean_ms']:>12.2f}  {r['stdev_ms']:>10.2f}  "
            f"{r['min_ms']:>10.2f}  {r['max_ms']:>10.2f}  "
            f"{r['mean_attempts']:>15.0f}  {r['expected_attempts']:>12}"
        )
    print(sep)
    print()


def save_csv(results: list[dict], path: str) -> None:
    fields = [
        "k", "trials", "mean_ms", "stdev_ms", "min_ms", "max_ms",
        "median_ms", "mean_attempts", "stdev_attempts", "expected_attempts",
    ]
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(results)
    print(f"[INFO] Raw results saved → {path}")


def save_plot(results: list[dict], path: str) -> None:
    if not HAS_PLOT:
        return

    ks         = [r["k"]        for r in results]
    means      = [r["mean_ms"]  for r in results]
    stdevs     = [r["stdev_ms"] for r in results]
    expected   = [r["expected_attempts"] for r in results]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
    fig.suptitle("Proof-of-Work: Cost vs Difficulty (k)", fontsize=14, fontweight="bold")

    # ── Left: Time ──────────────────────────────────────────────────────────
    bars = ax1.bar(ks, means, yerr=stdevs, capsize=5,
                   color="#4C72B0", edgecolor="white", alpha=0.85,
                   error_kw={"ecolor": "#222", "lw": 1.5})
    ax1.set_xlabel("Difficulty k  (number of leading hex zeros)", fontsize=11)
    ax1.set_ylabel("Nonce mining time (ms)", fontsize=11)
    ax1.set_title("Mean Mining Time ± 1σ", fontsize=12)
    ax1.set_xticks(ks)
    ax1.set_xticklabels([f"k={k}" for k in ks])
    ax1.yaxis.grid(True, linestyle="--", alpha=0.5)
    ax1.set_axisbelow(True)
    # Annotate bars
    for bar, mean, std in zip(bars, means, stdevs):
        label = f"{mean:.1f}ms" if mean < 1000 else f"{mean/1000:.2f}s"
        ax1.text(bar.get_x() + bar.get_width() / 2,
                 bar.get_height() + std + max(means) * 0.01,
                 label, ha="center", va="bottom", fontsize=9, fontweight="bold")

    # ── Right: Expected attempts (log scale) ────────────────────────────────
    ax2.bar(ks, expected, color="#DD8452", edgecolor="white", alpha=0.85)
    ax2.set_yscale("log")
    ax2.set_xlabel("Difficulty k  (number of leading hex zeros)", fontsize=11)
    ax2.set_ylabel("Expected attempts  (log scale)", fontsize=11)
    ax2.set_title("Theoretical Expected Attempts = 16^k", fontsize=12)
    ax2.set_xticks(ks)
    ax2.set_xticklabels([f"k={k}" for k in ks])
    ax2.yaxis.grid(True, linestyle="--", alpha=0.5)
    ax2.set_axisbelow(True)
    for i, (k, e) in enumerate(zip(ks, expected)):
        ax2.text(i, e * 1.3, f"16^{k}={e:,}", ha="center", va="bottom",
                 fontsize=8, fontweight="bold")

    plt.tight_layout()
    plt.savefig(path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"[INFO] Plot saved → {path}")


def recommend_k(results: list[dict]) -> None:
    """Print a short recommendation for k selection."""
    print("=" * 60)
    print("  PoW Difficulty Recommendation")
    print("=" * 60)
    for r in results:
        k, m = r["k"], r["mean_ms"]
        if m < 1:
            verdict = "⚠  Too fast — virtually no cost for Sybil attackers"
        elif m < 50:
            verdict = "✓  Acceptable — light delay for honest nodes"
        elif m < 500:
            verdict = "✓✓ Recommended range — meaningful cost, usable latency"
        elif m < 5000:
            verdict = "⚠  Expensive — noticeable bootstrap delay"
        else:
            verdict = "✗  Too slow — impractical for legitimate use"
        print(f"  k={k:2d}  ({m:8.1f} ms)  {verdict}")
    print("=" * 60)


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Benchmark SHA-256 Proof-of-Work nonce mining."
    )
    parser.add_argument(
        "--trials", type=int, default=20,
        help="Number of independent trials per difficulty level (default: 20)"
    )
    parser.add_argument(
        "--k-values", type=int, nargs="+", default=[2, 4, 6, 8],
        help="Difficulty levels to benchmark (default: 2 4 6 8)"
    )
    parser.add_argument(
        "--out-dir", default=".",
        help="Directory for output files (default: current directory)"
    )
    args = parser.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    print(f"[INFO] Benchmarking k ∈ {args.k_values} with {args.trials} trials each …")
    print()

    all_results = []
    for k in sorted(args.k_values):
        print(f"       Mining k={k} … ", end="", flush=True)
        r = benchmark_k(k, args.trials)
        all_results.append(r)
        print(f"mean={r['mean_ms']:.2f} ms  (±{r['stdev_ms']:.2f})")

    print_table(all_results)
    recommend_k(all_results)

    csv_path  = os.path.join(args.out_dir, "pow_benchmark_results.csv")
    plot_path = os.path.join(args.out_dir, "pow_benchmark_plot.png")

    save_csv(all_results, csv_path)
    save_plot(all_results, plot_path)


if __name__ == "__main__":
    random.seed()   # system entropy
    main()
