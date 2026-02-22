#!/bin/bash
# =============================================================
# Gossip Protocol Experiment Script
# Runs Push-only and Hybrid Push-Pull across N=10,20,50
# with 5 different seeds each, collecting log files.
# =============================================================

# ---- Default Parameters ----
FANOUT=3
TTL=5
PEER_LIMIT=20
PING_INTERVAL=2
PEER_TIMEOUT=5
RUNTIME=10
MESSAGE="experiment_message"
BASE_PORT=5000
SEEDS=(42 123 777 2024 9999)
SIZES=(10 20 50)
MODE="both"   # push | hybrid | both

# ---- Parse Arguments ----
while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--fanout)        FANOUT="$2";         shift 2 ;;
        -t|--ttl)           TTL="$2";            shift 2 ;;
        -l|--peer-limit)    PEER_LIMIT="$2";     shift 2 ;;
        -pi|--ping-interval) PING_INTERVAL="$2"; shift 2 ;;
        -pt|--peer-timeout) PEER_TIMEOUT="$2";   shift 2 ;;
        -r|--runtime)       RUNTIME="$2";        shift 2 ;;
        -m|--message)       MESSAGE="$2";        shift 2 ;;
        --mode)             MODE="$2";           shift 2 ;;
        *) echo "Unknown option $1"; exit 1 ;;
    esac
done

INJECT_PORT_BASE=7000   # ports for injector nodes (one per run)

run_experiment() {
    local N=$1
    local SEED=$2
    local PULL_INTERVAL=$3   # 0 = push-only
    local RUN_LABEL=$4

    local INJECT_PORT=$((INJECT_PORT_BASE + SEED % 1000 + N))

    echo "  [RUN] N=$N seed=$SEED mode=$RUN_LABEL inject_port=$INJECT_PORT"

    rm -f node_*.log

    local PIDS=()

    # Start cluster
    for ((i=0; i<N; i++)); do
        PORT=$((BASE_PORT + i))
        if [ $i -eq 0 ]; then
            ./gossip_node -p $PORT -f $FANOUT -t $TTL \
                          -i $PING_INTERVAL -o $PEER_TIMEOUT \
                          -l $PEER_LIMIT -s $SEED \
                          -q $PULL_INTERVAL &
        else
            ./gossip_node -p $PORT -f $FANOUT -t $TTL \
                          -i $PING_INTERVAL -o $PEER_TIMEOUT \
                          -l $PEER_LIMIT -s $SEED \
                          -q $PULL_INTERVAL \
                          -b 127.0.0.1:$BASE_PORT &
        fi
        PIDS+=($!)
        sleep 0.05
    done

    # Let the cluster form
    sleep 1

    # Inject the gossip message via a transient node
    ./gossip_node -p $INJECT_PORT -f 1 -t $TTL \
                  -q $PULL_INTERVAL \
                  -m "$MESSAGE" \
                  -b 127.0.0.1:$BASE_PORT &
    INJECT_PID=$!

    # Let it propagate
    sleep $RUNTIME

    # Stop injector then cluster
    kill -TERM $INJECT_PID 2>/dev/null
    for PID in "${PIDS[@]}"; do
        kill -TERM $PID 2>/dev/null
    done
    wait 2>/dev/null

    # Collect logs
    local OUT_DIR="results/${RUN_LABEL}/N${N}/seed${SEED}"
    mkdir -p "$OUT_DIR"
    mv node_*.log "$OUT_DIR/" 2>/dev/null

    echo "    -> Logs saved to $OUT_DIR"
}

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

for N in "${SIZES[@]}"; do
    echo ""
    echo "=== N = $N ==="

    if [[ "$MODE" == "push" || "$MODE" == "both" ]]; then
        for SEED in "${SEEDS[@]}"; do
            run_experiment $N $SEED 0 "push_only"
        done
    fi

    if [[ "$MODE" == "hybrid" || "$MODE" == "both" ]]; then
        for SEED in "${SEEDS[@]}"; do
            run_experiment $N $SEED 2 "hybrid"
        done
    fi
done

echo ""
echo "All experiments done."
echo "Run: python3 analyze.py  to compute metrics."
