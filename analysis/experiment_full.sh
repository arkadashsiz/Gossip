#!/bin/bash
# =============================================================================
# experiment_full.sh
#
# Full experiment runner for Phase 3 & 4 analysis.
# Runs Push-only and Hybrid Push-Pull modes for N in {10, 20, 50},
# each repeated with 5 different seeds, and collects all logs.
#
# Usage:
#   ./experiment_full.sh [--fanout N] [--ttl N] [--runtime N]
#
# Output:
#   results/<mode>_N<nodes>_s<seed>/node_*.log
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Default parameters (can be overridden via CLI)
# ---------------------------------------------------------------------------
FANOUT=3
TTL=5
PEER_LIMIT=20
PING_INTERVAL=2
PEER_TIMEOUT=5
PULL_INTERVAL=2        # for Hybrid mode
MAX_IHAVE_IDS=32       # for Hybrid mode
BASE_PORT=5000
INJECT_PORT=6900
RUNTIME=10             # seconds each run stays alive
SETTLE_TIME=1          # seconds to wait before injecting message
BINARY="./gossip_node"

# Seeds used for all runs (5 different seeds)
SEEDS=(42 137 271 999 1234)

# Network sizes to test
NODES_LIST=(10 20 50)

# Modes to test
MODES=("push" "hybrid")

# ---------------------------------------------------------------------------
# Parse CLI arguments
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case $1 in
        --fanout)       FANOUT="$2";        shift 2 ;;
        --ttl)          TTL="$2";           shift 2 ;;
        --runtime)      RUNTIME="$2";       shift 2 ;;
        --peer-limit)   PEER_LIMIT="$2";    shift 2 ;;
        --ping-interval) PING_INTERVAL="$2"; shift 2 ;;
        --peer-timeout) PEER_TIMEOUT="$2";  shift 2 ;;
        --pull-interval) PULL_INTERVAL="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------------
if [[ ! -x "$BINARY" ]]; then
    echo "[ERROR] Binary '$BINARY' not found or not executable."
    echo "        Run 'make' first."
    exit 1
fi

mkdir -p results

echo "============================================================"
echo "  Gossip Protocol — Full Experiment Suite"
echo "============================================================"
echo "  Fanout:         $FANOUT"
echo "  TTL:            $TTL"
echo "  Peer limit:     $PEER_LIMIT"
echo "  Ping interval:  $PING_INTERVAL s"
echo "  Peer timeout:   $PEER_TIMEOUT s"
echo "  Pull interval:  $PULL_INTERVAL s  (Hybrid only)"
echo "  Max IHAVE IDs:  $MAX_IHAVE_IDS    (Hybrid only)"
echo "  Runtime/run:    $RUNTIME s"
echo "  Nodes tested:   ${NODES_LIST[*]}"
echo "  Seeds:          ${SEEDS[*]}"
echo "  Modes:          ${MODES[*]}"
echo "============================================================"
echo ""

# ---------------------------------------------------------------------------
# Helper: kill all node processes started by this script
# ---------------------------------------------------------------------------
cleanup_pids() {
    local pids=("$@")
    for pid in "${pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    # Give processes a moment to exit gracefully
    sleep 1
    for pid in "${pids[@]}"; do
        kill -9 "$pid" 2>/dev/null || true
    done
}

# ---------------------------------------------------------------------------
# Helper: run one experiment trial
#   $1 = mode  ("push" | "hybrid")
#   $2 = N     (number of nodes)
#   $3 = seed
# ---------------------------------------------------------------------------
run_trial() {
    local MODE="$1"
    local N="$2"
    local SEED="$3"

    local LABEL="${MODE}_N${N}_s${SEED}"
    local OUT_DIR="results/${LABEL}"
    mkdir -p "$OUT_DIR"

    echo "  [RUN] mode=$MODE  N=$N  seed=$SEED  -> $OUT_DIR"

    # Build extra args for hybrid mode
    local HYBRID_ARGS=""
    if [[ "$MODE" == "hybrid" ]]; then
        HYBRID_ARGS="--pull-interval $PULL_INTERVAL --max-ihave-ids $MAX_IHAVE_IDS"
    fi

    local PIDS=()

    # --- Start cluster nodes ---
    for (( i=0; i<N; i++ )); do
        local PORT=$(( BASE_PORT + i ))
        local NODE_SEED=$(( SEED + i ))   # unique seed per node

        if [[ $i -eq 0 ]]; then
            # Seed node: no bootstrap
            LOG="$OUT_DIR/node_${PORT}.log" \
            $BINARY \
                --port "$PORT" \
                --fanout "$FANOUT" \
                --ttl "$TTL" \
                --peer-limit "$PEER_LIMIT" \
                --ping-interval "$PING_INTERVAL" \
                --peer-timeout "$PEER_TIMEOUT" \
                --seed "$NODE_SEED" \
                $HYBRID_ARGS \
                > "$OUT_DIR/stdout_${PORT}.txt" 2>&1 &
        else
            LOG="$OUT_DIR/node_${PORT}.log" \
            $BINARY \
                --port "$PORT" \
                --fanout "$FANOUT" \
                --ttl "$TTL" \
                --peer-limit "$PEER_LIMIT" \
                --ping-interval "$PING_INTERVAL" \
                --peer-timeout "$PEER_TIMEOUT" \
                --seed "$NODE_SEED" \
                --bootstrap "127.0.0.1:${BASE_PORT}" \
                $HYBRID_ARGS \
                > "$OUT_DIR/stdout_${PORT}.txt" 2>&1 &
        fi

        PIDS+=($!)
        sleep 0.05   # stagger startup slightly
    done

    # Let the cluster settle and exchange peer lists
    sleep "$SETTLE_TIME"

    # --- Inject gossip message via a temporary injector node ---
    local INJ_SEED=$(( SEED + 10000 ))
    local MSG="experiment_msg_${MODE}_N${N}_s${SEED}"

    LOG="$OUT_DIR/node_${INJECT_PORT}.log" \
    $BINARY \
        --port "$INJECT_PORT" \
        --fanout "$FANOUT" \
        --ttl "$TTL" \
        --peer-limit "$PEER_LIMIT" \
        --ping-interval 999 \
        --peer-timeout 999 \
        --seed "$INJ_SEED" \
        --bootstrap "127.0.0.1:${BASE_PORT}" \
        --message "$MSG" \
        $HYBRID_ARGS \
        > "$OUT_DIR/stdout_injector.txt" 2>&1 &
    local INJ_PID=$!
    PIDS+=($INJ_PID)

    # Let the experiment run
    sleep "$RUNTIME"

    # --- Tear down ---
    cleanup_pids "${PIDS[@]}"

    # Move log files produced by the binary into the output directory.
    # The binary writes node_PORT.log in the current working directory;
    # move them if they ended up there.
    for (( i=0; i<N; i++ )); do
        local PORT=$(( BASE_PORT + i ))
        local LOGFILE="node_${PORT}.log"
        if [[ -f "$LOGFILE" ]]; then
            mv "$LOGFILE" "$OUT_DIR/"
        fi
    done
    local INJ_LOG="node_${INJECT_PORT}.log"
    if [[ -f "$INJ_LOG" ]]; then
        mv "$INJ_LOG" "$OUT_DIR/"
    fi

    echo "       done."
}

# ---------------------------------------------------------------------------
# Main loop: iterate modes × node sizes × seeds
# ---------------------------------------------------------------------------
TOTAL=$(( ${#MODES[@]} * ${#NODES_LIST[@]} * ${#SEEDS[@]} ))
CURRENT=0

for MODE in "${MODES[@]}"; do
    for N in "${NODES_LIST[@]}"; do
        for SEED in "${SEEDS[@]}"; do
            CURRENT=$(( CURRENT + 1 ))
            echo "[$CURRENT/$TOTAL]"
            run_trial "$MODE" "$N" "$SEED"
            # Brief cooldown between runs so OS ports are released
            sleep 2
        done
        echo ""
    done
done

echo "============================================================"
echo "  All experiments complete."
echo "  Logs are in: results/"
echo "  Run:  python3 analyze_full.py"
echo "============================================================"
