#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NS3_DIR="${NS3_DIR:-$HOME/ENFS/src/ns-3.47}"
LOG_DIR="${ENFS_LOG_DIR:-$PROJECT_DIR/logs/reproduction}"

if [[ ! -x "$NS3_DIR/ns3" ]]; then
    echo "ERROR: ns-3 launcher not found at: $NS3_DIR/ns3"
    echo "Set NS3_DIR to your ns-3.47 source directory."
    exit 1
fi

mkdir -p "$LOG_DIR/v2-validation"
cd "$NS3_DIR"

for seed in 101 102 103 104 105; do
    echo "Running Control, seed $seed"

    ./ns3 run "enfs-control-random-v1 \
    --numberOfSenders=5 \
    --simulationTime=12 \
    --bottleneckMbps=8 \
    --startOffsetMs=50 \
    --seed=$seed \
    --run=1 \
    --jitterMin=0.90 \
    --jitterMax=1.10" \
    > "$LOG_DIR/control-seed${seed}-run1.txt" 2>&1

    echo "Running ENFS v1, seed $seed"

    ./ns3 run "enfs-feedback-random-v1 \
    --numberOfSenders=5 \
    --simulationTime=12 \
    --bottleneckMbps=8 \
    --startOffsetMs=50 \
    --seed=$seed \
    --run=1 \
    --jitterMin=0.90 \
    --jitterMax=1.10 \
    --recoveryMbps=0.25" \
    > "$LOG_DIR/enfs-seed${seed}-run1.txt" 2>&1

    echo "Running ENFS v2, seed $seed"

    ./ns3 run "enfs-feedback-v2/enfs-feedback-v2 \
    --numberOfSenders=5 \
    --simulationTime=12 \
    --bottleneckMbps=8 \
    --startOffsetMs=50 \
    --seed=$seed \
    --run=1 \
    --jitterMin=0.90 \
    --jitterMax=1.10 \
    --recoveryMbps=0.25 \
    --persistenceMs=50" \
    > "$LOG_DIR/v2-validation/enfs-v2-seed${seed}-p50ms.txt" 2>&1

    echo "Completed seed $seed"
done

echo "All 15 simulations completed."
echo "Logs saved in: $LOG_DIR"
