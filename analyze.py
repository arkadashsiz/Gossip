import glob
logs = glob.glob("node_*.log")

events = []
nodes = len(logs)

for file in logs:
    with open(file) as f:
        for line in f:
            ts, event, msg_type, msg_id = line.strip().split(",")
            if msg_type == "GOSSIP":
                events.append((int(ts), event, msg_id))

if not events:
    print("No gossip events found.")
    exit()

# group by msg_id
from collections import defaultdict

data = defaultdict(list)

for ts, event, msg_id in events:
    data[msg_id].append((ts, event))

for msg_id, entries in data.items():
    sends = [ts for ts,e in entries if e == "SEND"]
    receives = [ts for ts,e in entries if e == "RECEIVE"]

    if not sends or not receives:
        continue

    start = min(sends)
    receives.sort()

    target = int(0.95 * nodes)
    if len(receives) >= target:
        converge = receives[target-1]
        print("Message:", msg_id)
        print("Convergence time:", converge - start, "ms")
        print("Total sends:", len(sends))
        print()
