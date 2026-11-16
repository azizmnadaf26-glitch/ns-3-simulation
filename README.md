# Explicit Negative Feedback Signaling (ENFS)

## 1. Project Overview

Explicit Negative Feedback Signaling (ENFS) is a network congestion
management approach implemented and evaluated using the ns-3.47
network simulator.

The project investigates whether explicit feedback from a bottleneck
router can help sender applications adapt their offered transmission
rates in response to congestion conditions.

The implementation uses UDP feedback messages to communicate
congestion and clear states to the senders. The senders use this
information to adjust their application-level packet pacing rates.

The project evaluates three configurations:

1. Control — paced senders without ENFS rate adaptation.
2. ENFS v1 — explicit feedback with immediate state-based rate adaptation.
3. ENFS v2 — explicit feedback with temporal hysteresis.

## 2. Problem Statement

In a shared network bottleneck, multiple senders compete for limited
bandwidth. When the offered traffic exceeds the available capacity,
the queue can become congested, potentially increasing packet delay
and packet loss.

This project explores an explicit feedback mechanism in which the
bottleneck router communicates its congestion state directly to
the sending applications.

The objective is to study the trade-offs between throughput,
packet delay, packet loss and congestion-state behaviour.

## 3. Proposed Approach

ENFS uses explicit router-to-sender feedback.

The bottleneck queue is monitored, and its occupancy is used to
identify congestion and clear conditions.

The router sends a UDP feedback message to the sender applications.
Each feedback message contains:

- A 4-byte global sequence number.
- A 1-byte state indicator.

The state indicator represents either:

- CONGESTION
- CLEAR

The sequence number allows a sender to reject feedback messages
that are older than the most recently accepted message.

### 3.1 Feedback and Rate Adaptation

The implementation uses the following queue thresholds:

- Congestion threshold: 15 packets.
- Clear threshold: 10 packets.

A feedback cooldown of 100 ms is used to regulate feedback
transmission.

When congestion feedback is accepted, the sender reduces its
application-level offered rate to 75% of its current rate,
subject to the configured minimum rate.

When clear feedback is accepted, the sender increases its
application-level offered rate by the configured recovery increment,
subject to the configured maximum rate.

The formal randomized experiments use a recovery increment of
0.25 Mbps.

**Important:** ENFS adjusts application-level packet pacing.
It does not directly modify the TCP congestion window.

## 4. ENFS v2 — Temporal Hysteresis

ENFS v2 extends the feedback mechanism with a temporal persistence
requirement.

Instead of confirming a congestion or clear-state transition
immediately after a queue threshold crossing, the queue condition
must persist for a configured interval.

The evaluated v2 configuration uses a persistence interval of 50 ms.

The implementation tracks:
- Congestion candidates started.
- Congestion candidates cancelled.
- Congestion transitions confirmed.
- Clear candidates started.
- Clear candidates cancelled.
- Clear transitions confirmed.

This mechanism is intended to filter short-lived queue threshold
crossings and reduce unnecessary state transitions.

## 5. Network Simulation Configuration

The formal three-mode comparison uses the following configuration:

| Parameter | Value |
|---|---:|
| Simulator | ns-3.47 |
| Number of senders | 5 |
| Bottleneck capacity | 8 Mbps |
| Queue capacity | 20 packets |
| Simulation duration | 12 seconds |
| Intended sender start offset | 50 ms |
| Pacing jitter range | 0.90–1.10 |
| Random seeds | 101–105 |
| ENFS recovery increment | 0.25 Mbps |
| ENFS v2 persistence interval | 50 ms |

The randomized experiments use pre-generated pacing-jitter sequences
with dedicated random-number streams for each sender.

The same seed and sender-to-stream mapping are used for the
Control and ENFS configurations to support matched comparisons.

This is a matched random-number design; it does not imply that
the resulting packet transmission timestamps are identical.

## 6. Experimental Methodology

The evaluation was performed in multiple stages.

### 6.1 Workload-Offset Experiments

Control and ENFS were first compared across sender startup offsets
from 10 ms to 100 ms.

These runs helped investigate workload sensitivity, but different
startup offsets are different workload configurations rather than
independent random replications.

### 6.2 Randomized Matched Experiments

The formal randomized comparison used five seeds:

- 101
- 102
- 103
- 104
- 105

Each seed was evaluated using the Control and ENFS v1 configurations.

### 6.3 ENFS v2 Validation

ENFS v2 was evaluated using the same five seed values and intended
network configuration, with a 50 ms temporal persistence interval.

### 6.4 Performance Metrics

The following metrics were collected:

- Aggregate throughput (Mbps).
- Mean packet delay (ms).
- FlowMonitor lost-packet count.
- Congestion episodes.
- Maximum queue occupancy (packets).

The analysis includes descriptive summaries and paired comparisons.
The paired confidence intervals are exploratory because the study
contains only five matched seeds.

## 7. Results and Discussion

The following values are the means across the five randomized seeds.

| Metric | Control | ENFS v1 | ENFS v2 |
|---|---:|---:|---:|
| Throughput (Mbps) | 7.9180 | 7.5878 | 7.9220 |
| Mean delay (ms) | 30.4774 | 25.3518 | 29.8130 |
| Lost packets | 83.0 | 61.4 | 76.2 |
| Congestion episodes | 7.6 | 11.8 | 2.4 |
| Maximum queue (packets) | 20 | 20 | 20 |

### 7.1 ENFS v1 Compared with Control

Across the five matched seeds, ENFS v1 showed:

- Approximately 4.17% lower mean throughput.
- Approximately 16.84% lower mean packet delay.
- Approximately 25.77% fewer lost packets.
- More congestion episodes on average.

These results indicate a trade-off: ENFS v1 reduced delay and
packet loss in this experiment, while also reducing throughput.

### 7.2 ENFS v2 Compared with Control

Across the five matched seeds, ENFS v2 showed:

- Approximately 0.05% higher mean throughput.
- Approximately 2.18% lower mean packet delay.
- Approximately 8.09% fewer lost packets.
- Approximately 67.70% fewer congestion episodes.

The throughput difference is very small and should not be interpreted
as a meaningful throughput improvement.

The paired throughput confidence interval includes zero.
The paired intervals for delay, lost packets and congestion episodes
exclude zero in this five-seed analysis, but the sample is small and
the comparisons are exploratory.

### 7.3 ENFS v1 Compared with ENFS v2

ENFS v2 maintained throughput closer to the Control configuration
and produced fewer congestion episodes than ENFS v1.

However, ENFS v1 had lower mean delay and fewer lost packets than
ENFS v2 in the evaluated runs.

The two versions therefore demonstrate different performance
trade-offs rather than one version being universally superior.

## 8. Important Observation

All three configurations reached the 20-packet maximum queue
occupancy in all five randomized seeds.

Therefore, the experiments do not demonstrate a reduction in
maximum queue occupancy.

A lower congestion-episode count should not be interpreted as
proof of a smaller peak queue.

## 9. Repository Structure

The final repository is intended to contain:

- `src/` — final simulation source files.
- `scripts/` — analysis and experiment scripts.
- `logs/` — preserved simulation logs.
- `results/` — CSV result files.
- `plots/` — generated comparison plots.
- `docs/` — technical and experimental documentation.
- `evidence/` — supporting experiment artifacts and inventory.

The complete ns-3.47 simulator source tree is not duplicated in
this project repository. ns-3.47 must be installed separately.

## 10. Requirements

- ns-3.47
- A supported C++ build toolchain.
- CMake and the dependencies required by ns-3.47.
- Python 3
- Python packages required by the analysis scripts, including
  pandas and matplotlib if used by the selected script.

The simulation source files are designed to be built and executed
within an ns-3.47 environment.

Detailed installation, build, experiment execution, and analysis
instructions are provided in `docs/REPRODUCTION_GUIDE.md`.
The repository includes scripts to install the simulation sources
into an existing ns-3.47 tree, run the five-seed experiment, and
analyse the resulting logs.

## 11. Limitations

- The evaluation uses a limited five-seed randomized study.
- Results are specific to the simulated topology and parameters.
- The queue reaches its configured maximum occupancy in all
  five matched seeds.
- ENFS changes application-level pacing rather than TCP's
  internal congestion window.
- Feedback is carried over UDP and does not include authentication.
- Sequence numbers help reject stale or reordered feedback but
  do not provide security or sender authentication.
- The current implementation is a simulation prototype and
  has not been validated in a production network.
- The paired statistical intervals are exploratory and are
  not adjusted for multiple comparisons.

## 12. Future Work

Potential future investigations include:

- Testing additional topologies and bottleneck capacities.
- Increasing the number of independent random seeds.
- Evaluating different queue-management policies.
- Studying feedback loss, delay and reordering.
- Investigating feedback authentication and security.
- Comparing ENFS with additional congestion-control mechanisms.
- Evaluating performance under larger sender populations.

## 13. Evidence and Reproducibility

The project preserves simulation logs, source files, analysis
scripts, CSV outputs and generated plots.

The evidence inventory records file sizes and SHA-256 checksums
for the preserved experiment snapshot.

These artifacts are intended to support inspection and
reproducibility of the reported simulation results.

## 14. Conclusion

This project implements and evaluates an explicit negative feedback
mechanism for application-level sender rate adaptation in ns-3.47.

The experiments show that feedback-based adaptation can change
the trade-off between throughput, delay, packet loss and congestion
episodes.

ENFS v1 prioritizes lower delay and packet loss at the cost of
lower throughput in the evaluated configuration.

ENFS v2 introduces temporal hysteresis and produces results closer
to the Control configuration in throughput, with fewer congestion
episodes and modest improvements in delay and packet loss.

Further experiments are required before generalizing these findings
to other network conditions or real-world deployments.
