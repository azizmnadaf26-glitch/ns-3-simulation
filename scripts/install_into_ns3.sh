#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 /path/to/ns-3.47"
    exit 1
fi

NS3_DIR="$(realpath "$1")"

if [[ ! -x "$NS3_DIR/ns3" ]]; then
    echo "ERROR: ns-3 launcher not found at: $NS3_DIR/ns3"
    exit 1
fi

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mkdir -p \
    "$NS3_DIR/scratch/enfs-control-random" \
    "$NS3_DIR/scratch/enfs-feedback-random" \
    "$NS3_DIR/scratch/enfs-feedback-v2"

cp "$REPO_DIR/src/enfs-control-random-v1.cc" \
   "$NS3_DIR/scratch/enfs-control-random/"

cp "$REPO_DIR/src/enfs-feedback-random-v1.cc" \
   "$NS3_DIR/scratch/enfs-feedback-random/"

cp "$REPO_DIR/src/enfs-feedback-v2.cc" \
   "$NS3_DIR/scratch/enfs-feedback-v2/"

echo "ENFS sources installed into: $NS3_DIR"
