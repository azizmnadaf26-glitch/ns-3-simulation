# ENFS Project — Development History

## Overview

This document describes the development progression of the Explicit
Negative Feedback Signaling (ENFS) simulation project using ns-3.47.

The history is reconstructed from preserved source files, simulation
logs, experimental outputs and development artifacts.

## Phase 1 — Initial Implementation and Exploration

Key source artifacts:
- enfs-baseline.cc
- enfs-experiment.cc
- enfs-feedback-v2.cc
- enfs-feedback-v3.cc
- enfs-feedback-v4.cc

Work completed:
- Initial baseline simulation.
- Development of explicit feedback mechanisms.
- Successive implementation experiments.
- Early simulation and debugging runs.

## Phase 2 — TCP Variants and Parameter Investigations

Key artifacts:
- enfs-cubic-v1.cc
- enfs-cubic-standard.cc
- Load-scaling experiment logs.
- Capacity-sweep experiment logs.
- Recovery-sweep experiment logs.

Investigations included:
- TCP NewReno and CUBIC configurations.
- Different sender counts.
- Bottleneck capacities of 3, 5 and 8 Mbps.
- Recovery increments of 0.10, 0.25 and 0.50 Mbps.

## Phase 3 — Repeated and Workload-Sensitive Experiments

Preserved experiments include:
- Repeated five-sender runs.
- Sender-scaling experiments.
- Startup-offset experiments.
- Paired workload-offset experiments from 10 to 100 ms.

These experiments expanded the evaluation beyond a single simulation run.

## Phase 4 — Diagnostic Improvements and Randomized Experiments

Key source artifacts:
- enfs-control-random-v1.cc
- enfs-feedback-random-v1.cc
- enfs-feedback-v4-diagnostic.cc

The randomized comparison used:
- Five senders.
- An 8 Mbps bottleneck.
- A 12-second simulation duration.
- Seeds 101 through 105.
- Pacing jitter between 0.90 and 1.10.
- Matched pre-generated random pacing sequences.
- A recovery increment of 0.25 Mbps.

Control and ENFS v1 were evaluated using matched seed configurations.

## Phase 5 — ENFS v2 Temporal Hysteresis

Key source artifact:
- enfs-feedback-v2/enfs-feedback-v2.cc

The v2 implementation introduced a configurable temporal persistence
interval for congestion and clear-state transitions.

The evaluation used:
- Persistence interval: 50 ms.
- Seeds: 101, 102, 103, 104 and 105.
- The same intended network configuration as the randomized v1 study.

Diagnostic counters recorded:
- Congestion candidates started.
- Congestion candidates cancelled.
- Congestion transitions confirmed.
- Clear candidates started.
- Clear candidates cancelled.
- Clear transitions confirmed.

## Phase 6 — Comparative Analysis and Results

Three configurations were compared:

1. Control
2. ENFS v1
3. ENFS v2

The evaluation included:
- Aggregate throughput.
- Mean packet delay.
- FlowMonitor packet loss.
- Congestion episodes.
- Maximum queue occupancy.

Preserved outputs include:
- Three-mode seed results CSV.
- Three-mode paired comparison CSV.
- Three-mode statistical summary CSV.
- Five comparison plots.
- Individual simulation logs.

## Phase 7 — Evidence Preservation and Reproducibility

The experiment-freeze directory preserves:
- Final simulation source files.
- Earlier experiment logs.
- Analysis scripts.
- CSV results.
- Plots and intermediate artifacts.

A separate evidence inventory records file sizes and SHA-256
checksums for the preserved snapshot.

## Reproducibility and Limitations

The five-seed study is an exploratory simulation evaluation.
Its results should not be interpreted as universal network performance
guarantees.

The three configurations reached the 20-packet queue limit in all
five matched seeds.

ENFS v1 and v2 demonstrate different performance trade-offs.
The results, limitations and statistical assumptions are documented
separately in the research report.
