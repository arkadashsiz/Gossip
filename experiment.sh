#!/bin/bash

NODES=$1
BASE_PORT=5000

echo "Starting $NODES nodes..."

rm -f node_*.log
PIDS=()

# Start cluster
for ((i=0;i<$NODES;i++))
do
    PORT=$((BASE_PORT + i))

    if [ $i -eq 0 ]; then
        ./gossip_node -p $PORT -f 3 -t 5 &
    else
        ./gossip_node -p $PORT -f 3 -t 5 -b 127.0.0.1:$BASE_PORT &
    fi

    PIDS+=($!)
    sleep 0.1
done

echo "Cluster started."


echo "Injecting gossip..."

./gossip_node -p 6000 -f 1 -t 3 -m "experiment_message" -b 127.0.0.1:$BASE_PORT &
INJECT_PID=$!

sleep 6

echo "Stopping injector..."
kill -TERM $INJECT_PID 2>/dev/null

echo "Stopping cluster..."
for PID in "${PIDS[@]}"
do
    kill -TERM $PID 2>/dev/null
done

sleep 2

echo "Experiment done."
