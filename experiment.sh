#!/bin/bash

# =========================
# Default Parameters
# =========================
NODES=10
FANOUT=3
TTL=5
PEER_LIMIT=20
PING_INTERVAL=2
PEER_TIMEOUT=5
RUNTIME=8
MESSAGE="experiment_message"
BASE_PORT=5000

# =========================
# Parse Arguments
# =========================
while [[ $# -gt 0 ]]; do
    case $1 in
        -n|--nodes) NODES="$2"; shift 2 ;;
        -f|--fanout) FANOUT="$2"; shift 2 ;;
        -t|--ttl) TTL="$2"; shift 2 ;;
        -l|--peer-limit) PEER_LIMIT="$2"; shift 2 ;;
        -pi|--ping-interval) PING_INTERVAL="$2"; shift 2 ;;
        -pt|--peer-timeout) PEER_TIMEOUT="$2"; shift 2 ;;
        -r|--runtime) RUNTIME="$2"; shift 2 ;;
        -m|--message) MESSAGE="$2"; shift 2 ;;
        *) echo "Unknown option $1"; exit 1 ;;
    esac
done

echo "===================================="
echo "Nodes:          $NODES"
echo "Fanout:         $FANOUT"
echo "TTL:            $TTL"
echo "Peer Limit:     $PEER_LIMIT"
echo "Ping Interval:  $PING_INTERVAL"
echo "Peer Timeout:   $PEER_TIMEOUT"
echo "Runtime:        $RUNTIME seconds"
echo "Message:        $MESSAGE"
echo "===================================="

rm -f node_*.log
PIDS=()

echo "Starting cluster..."

# =========================
# Start Cluster
# =========================
for ((i=0;i<$NODES;i++))
do
    PORT=$((BASE_PORT + i))

    if [ $i -eq 0 ]; then
    ./gossip_node -p $PORT -f $FANOUT -t $TTL &
else
    ./gossip_node -p $PORT -f $FANOUT -t $TTL -b 127.0.0.1:$BASE_PORT &
fi

    PIDS+=($!)
    sleep 0.1
done

echo "Cluster started."

sleep 0.5

echo "Injecting gossip..."

./gossip_node -p 6000 \
              -f 1 \
              -t $TTL \
              -m "$MESSAGE" \
              -b 127.0.0.1:$BASE_PORT &
INJECT_PID=$!

# =========================
# Let experiment run
# =========================
sleep $RUNTIME

echo "Stopping injector..."
kill -TERM $INJECT_PID 2>/dev/null

echo "Stopping cluster..."
for PID in "${PIDS[@]}"
do
    kill -TERM $PID 2>/dev/null
done

sleep 2

echo "Experiment done."
