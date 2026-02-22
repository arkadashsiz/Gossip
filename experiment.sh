#!/bin/bash
# =============================================================
# Gossip Protocol Experiment Script
# Runs Push-only and Hybrid Push-Pull across N=10,20,50
# with 5 different seeds each, collecting log files.
# All node stdout/stderr is suppressed (redirected to /dev/null).
# =============================================================

set -euo pipefail

# ---- Default Parameters ----
FANOUT=3
TTL=5
PEER_LIMIT=20
PING_INTERVAL=2
PEER_TIMEOUT=5
RUNTIME=10
MESSAGE="experiment_message"
BASE_PORT=5000
SEEDS=(42 9999)
SIZES=(10 20 50)
MODE="both"   # push | hybrid | both

# ---- Parse Arguments ----
while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--fanout)         FANOUT="$2";         shift 2 ;;
        -t|--ttl)            TTL="$2";            shift 2 ;;
        -l|--peer-limit)     PEER_LIMIT="$2";     shift 2 ;;
        -pi|--ping-interval) PING_INTERVAL="$2";  shift 2 ;;
        -pt|--peer-timeout)  PEER_TIMEOUT="$2";   shift 2 ;;
        -r|--runtime)        RUNTIME="$2";        shift 2 ;;
        -m|--message)        MESSAGE="$2";        shift 2 ;;
        --mode)              MODE="$2";           shift 2 ;;
        *) echo "Unknown option $1"; exit 1 ;;
    esac
done

BINARY="./gossip_node"
if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: $BINARY not found. Run 'make' first."
    exit 1
fi

echo "===================================="
echo "Fanout:         $FANOUT"
echo "TTL:            $TTL"
echo "Peer Limit:     $PEER_LIMIT"
echo "Ping Interval:  $PING_INTERVAL"
echo "Peer Timeout:   $PEER_TIMEOUT"
echo "Runtime:        $RUNTIME s"
echo "Message:        $MESSAGE"
echo "Mode:           $MODE"
echo "Sizes:          ${SIZES[*]}"
echo "Seeds:          ${SEEDS[*]}"
echo "===================================="

mkdir -p results

# ------------------------------------------------------------------
# kill_all <pid...>  – send SIGTERM then SIGKILL after 3 s
# ------------------------------------------------------------------
kill_all() {
    local pids=("$@")
    # Send SIGTERM to all
    for pid in "${pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    # Wait up to 3 seconds for them to exit
    local deadline=$(( $(date +%s) + 3 ))
    for pid in "${pids[@]}"; do
        local remaining=$(( deadline - $(date +%s) ))
        if [[ $remaining -gt 0 ]]; then
            timeout "$remaining" tail --pid="$pid" -f /dev/null 2>/dev/null || true
        fi
        # Force kill if still alive
        kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
    done
}

# ------------------------------------------------------------------
# run_experiment N SEED PULL_INTERVAL LABEL
# ------------------------------------------------------------------
run_experiment() {
    local N=$1
    local SEED=$2
    local PULL_INTERVAL=$3
    local RUN_LABEL=$4

    # Use a port range that avoids conflicts between parallel calls
    # (we don't run in parallel, but keep ports distinct per seed)
    local CLUSTER_BASE=$BASE_PORT
    local INJECT_PORT=$(( 7000 + (SEED % 100) * 10 + N % 10 ))

    echo "  [RUN] N=$N seed=$SEED mode=$RUN_LABEL"

    # Clean up any stale logs
    rm -f node_*.log

    local PIDS=()

    # ---- Start bootstrap node (node 0) ----
    "$BINARY" -p "$CLUSTER_BASE" \
              -f "$FANOUT" -t "$TTL" \
              -i "$PING_INTERVAL" -o "$PEER_TIMEOUT" \
              -l "$PEER_LIMIT" -s "$SEED" \
              -q "$PULL_INTERVAL" \
              </dev/null >/dev/null 2>&1 &
    PIDS+=($!)
    sleep 0.1   # let it bind before others try to bootstrap

    # ---- Start remaining cluster nodes ----
    for (( i=1; i<N; i++ )); do
        local PORT=$(( CLUSTER_BASE + i ))
        "$BINARY" -p "$PORT" \
                  -f "$FANOUT" -t "$TTL" \
                  -i "$PING_INTERVAL" -o "$PEER_TIMEOUT" \
                  -l "$PEER_LIMIT" -s "$SEED" \
                  -q "$PULL_INTERVAL" \
                  -b "127.0.0.1:$CLUSTER_BASE" \
                  </dev/null >/dev/null 2>&1 &
        PIDS+=($!)
        sleep 0.05
    done

    # Let the cluster stabilise
    sleep 1

    # ---- Inject the gossip message ----
    "$BINARY" -p "$INJECT_PORT" \
              -f 1 -t "$TTL" \
              -q "$PULL_INTERVAL" \
              -m "$MESSAGE" \
              -b "127.0.0.1:$CLUSTER_BASE" \
              </dev/null >/dev/null 2>&1 &
    local INJECT_PID=$!

    # Let it propagate for RUNTIME seconds
    sleep "$RUNTIME"

    # ---- Tear down ----
    kill_all "$INJECT_PID"
    kill_all "${PIDS[@]}"

    # Small pause to let OS release ports
    sleep 1

    # ---- Collect logs ----
    local OUT_DIR="results/${RUN_LABEL}/N${N}/seed${SEED}"
    mkdir -p "$OUT_DIR"
    mv node_*.log "$OUT_DIR/" 2>/dev/null || true

    local got
    got=$(ls "$OUT_DIR"/node_*.log 2>/dev/null | wc -l)
    echo "    -> $got log files saved to $OUT_DIR"
}

# ------------------------------------------------------------------
# Main loop
# ------------------------------------------------------------------
for N in "${SIZES[@]}"; do
    echo ""
    echo "=== N = $N ==="

    if [[ "$MODE" == "push" || "$MODE" == "both" ]]; then
        for SEED in "${SEEDS[@]}"; do
            run_experiment "$N" "$SEED" 0 "push_only"
        done
    fi

    if [[ "$MODE" == "hybrid" || "$MODE" == "both" ]]; then
        for SEED in "${SEEDS[@]}"; do
            run_experiment "$N" "$SEED" 2 "hybrid"
        done
    fi
done

echo ""
echo "All experiments done."
echo "Run:  python3 analyze.py   to compute metrics."
