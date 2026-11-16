
# ENFS — Reproduction Guide

This guide explains how to install, build, run, and analyse the ENFS simulations using this repository and a separate ns-3.47 source tree.

## 1. Prerequisites

- Linux environment.
- ns-3.47 source tree.
- C++ compiler and build dependencies required by ns-3.47.
- Python 3.
- Python packages: pandas, matplotlib, numpy.

The ns-3.47 source tree is a separate prerequisite and is not bundled in this repository.

## 2. Repository Layout

- `src/` — three ENFS simulation source files.
- `scripts/` — installation, experiment, and analysis scripts.
- `logs/` — preserved formal experiment logs.
- `results/` — formal CSV analysis outputs.
- `plots/` — formal comparison charts.
- `docs/` — project development history and this guide.

## 3. Install the Simulation Sources

Set the path to your ns-3.47 source tree.

For example:

```bash
export NS3_DIR="$HOME/ns-3.47"
```

Install the ENFS source files:

```bash
cd /path/to/ENFS-ns3
./scripts/install_into_ns3.sh "$NS3_DIR"
```

The helper copies the three simulation files into their required ns-3 scratch directories.

## 4. Build the Simulation Programs

```bash
cd "$NS3_DIR"

./ns3 build enfs-control-random-v1 -j2
./ns3 build enfs-feedback-random-v1 -j2
./ns3 build enfs-feedback-v2/enfs-feedback-v2 -j2
```

## 5. Formal Experiment Configuration

| Parameter | Value |
|---|---:|
| Number of senders | 5 |
| Simulation duration | 12 seconds |
| Bottleneck capacity | 8 Mbps |
| Sender startup offset | 50 ms |
| Random run number | 1 |
| Minimum pacing jitter | 0.90 |
| Maximum pacing jitter | 1.10 |
| Recovery increment | 0.25 Mbps |
| ENFS v2 persistence interval | 50 ms |
| Random seeds | 101–105 |

The Control implementation does not use ENFS recovery parameters. ENFS v1 and v2 use a recovery increment of 0.25 Mbps. ENFS v2 uses a persistence interval of 50 ms.

All experiment parameters are passed explicitly.

## 6. Run Individual Simulations

Run commands from the ns-3.47 directory.

### 6.1 Control

```bash
./ns3 run "enfs-control-random-v1 \
--numberOfSenders=5 \
--simulationTime=12 \
--bottleneckMbps=8 \
--startOffsetMs=50 \
--seed=101 \
--run=1 \
--jitterMin=0.90 \
--jitterMax=1.10"
```

### 6.2 ENFS v1

```bash
./ns3 run "enfs-feedback-random-v1 \
--numberOfSenders=5 \
--simulationTime=12 \
--bottleneckMbps=8 \
--startOffsetMs=50 \
--seed=101 \
--run=1 \
--jitterMin=0.90 \
--jitterMax=1.10 \
--recoveryMbps=0.25"
```

### 6.3 ENFS v2

```bash
./ns3 run "enfs-feedback-v2/enfs-feedback-v2 \
--numberOfSenders=5 \
--simulationTime=12 \
--bottleneckMbps=8 \
--startOffsetMs=50 \
--seed=101 \
--run=1 \
--jitterMin=0.90 \
--jitterMax=1.10 \
--recoveryMbps=0.25 \
--persistenceMs=50"
```

Change the seed to reproduce another experimental run.

## 7. Run All Five Seeds

From the repository root:

```bash
NS3_DIR="$NS3_DIR" ./scripts/run_all_experiments.sh
```

The script runs Control, ENFS v1, and ENFS v2 sequentially for seeds 101–105.

Fresh logs are written separately under:

- `logs/reproduction/`
- `logs/reproduction/v2-validation/`

The preserved formal logs are not overwritten.

## 8. Analyse the Fresh Reproduction

From the repository root:

```bash
ENFS_PROJECT_DIR="$PWD" \
ENFS_LOG_DIR="$PWD/logs/reproduction" \
ENFS_RESULT_DIR="$PWD/results/reproduction" \
ENFS_PLOT_DIR="$PWD/plots/reproduction" \
python3 scripts/analyze_three_modes.py
```

The analysis script generates:

- `three_mode_seed_results.csv`
- `three_mode_paired_comparison.csv`
- `three_mode_summary.csv`

It also generates five comparison charts covering:

- Throughput.
- Mean packet delay.
- Lost packets.
- Congestion episodes.
- Maximum queue occupancy.

## 9. Reproducibility Notes

- Use ns-3.47 and the source files supplied in this repository.
- Use the same seed, run number, and sender-to-stream mapping.
- Pass experimental parameters explicitly.
- Preserve individual logs for every configuration and seed.
- Do not treat different startup offsets as independent random seeds.
- Build messages and runtime timestamps may differ between runs.
- The five-seed evaluation is exploratory; interpret its findings within the limitations described in the project README.
