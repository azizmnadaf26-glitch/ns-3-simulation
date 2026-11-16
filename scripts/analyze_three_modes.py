#!/usr/bin/env python3

import csv
import math
import os
import re
import statistics
from itertools import combinations
from pathlib import Path

BASE = Path(os.environ.get("ENFS_PROJECT_DIR", Path(__file__).resolve().parents[1]))
LOG_DIR = Path(os.environ.get("ENFS_LOG_DIR", BASE / "logs"))
V2_DIR = LOG_DIR / "v2-validation"
RESULT_DIR = Path(os.environ.get("ENFS_RESULT_DIR", BASE / "results"))
PLOT_DIR = Path(os.environ.get("ENFS_PLOT_DIR", BASE / "plots"))

SEEDS = [101, 102, 103, 104, 105]
MODES = ["Control", "ENFS_v1", "ENFS_v2"]

LOG_PATTERNS = {
    "Control": "control-seed{seed}-run1.txt",
    "ENFS_v1": "enfs-seed{seed}-run1.txt",
    "ENFS_v2": "enfs-v2-seed{seed}-p50ms.txt",
}

PATTERNS = {
    "transmitted_packets": r"Transmitted packets:\s*(\d+)",
    "received_packets": r"Received packets:\s*(\d+)",
    "lost_packets": r"Lost packets \(FlowMonitor\):\s*(\d+)",
    "throughput_mbps": r"Aggregate throughput estimate:\s*([\d.]+)\s*Mbps",
    "mean_delay_ms": r"Mean packet delay:\s*([\d.]+)\s*ms",
    "max_queue_packets": r"Maximum queue occupancy:\s*(\d+)\s*packets",
    "congestion_episodes": r"Congestion episodes:\s*(\d+)",
    "feedback_sent": r"Feedback messages sent:\s*(\d+)",
    "feedback_received": r"Feedback messages received:\s*(\d+)",
}

METRICS = [
    "throughput_mbps",
    "mean_delay_ms",
    "lost_packets",
    "congestion_episodes",
    "max_queue_packets",
]

# Two-sided 95% paired t interval, n=5, df=4.
T_CRITICAL_DF4 = 2.776445

DISPLAY_NAMES = {
    "throughput_mbps": "Throughput (Mbps)",
    "mean_delay_ms": "Mean packet delay (ms)",
    "lost_packets": "FlowMonitor lost packets",
    "congestion_episodes": "Congestion episodes",
    "max_queue_packets": "Maximum queue occupancy (packets)",
}


def extract_metrics(path):
    if not path.is_file():
        raise FileNotFoundError(f"Missing log: {path}")

    content = path.read_text(errors="replace")
    result = {}

    for field, pattern in PATTERNS.items():
        matches = re.findall(pattern, content)
        if not matches:
            raise ValueError(f"{field} not found in {path}")

        value = matches[-1]
        result[field] = float(value) if "." in value else int(value)

    return result


def write_csv(path, rows):
    if not rows:
        raise ValueError(f"No rows to write: {path}")

    # Build a union of columns because pairwise comparisons
    # contain different mode-specific fields.
    fields = list(dict.fromkeys(
        key for row in rows for key in row.keys()
    ))

    with path.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=fields,
            extrasaction="raise",
        )
        writer.writeheader()
        writer.writerows(rows)


def mean(values):
    return statistics.mean(values)


def sample_sd(values):
    return statistics.stdev(values) if len(values) > 1 else 0.0


def paired_summary(deltas):
    n = len(deltas)
    avg = mean(deltas)
    sd = sample_sd(deltas)

    if n > 1:
        margin = T_CRITICAL_DF4 * sd / math.sqrt(n)
    else:
        margin = 0.0

    return avg, sd, avg - margin, avg + margin


def main():
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    PLOT_DIR.mkdir(parents=True, exist_ok=True)

    raw_rows = []
    data = {mode: {} for mode in MODES}

    # Extract every run directly from its original log.
    for seed in SEEDS:
        for mode in MODES:
            filename = LOG_PATTERNS[mode].format(seed=seed)
            directory = V2_DIR if mode == "ENFS_v2" else LOG_DIR
            path = directory / filename

            metrics = extract_metrics(path)
            data[mode][seed] = metrics

            raw_rows.append({
                "seed": seed,
                "mode": mode,
                **metrics,
            })

    write_csv(
        RESULT_DIR / "three_mode_seed_results.csv",
        raw_rows,
    )

    # Generate all three paired comparisons.
    pairwise_rows = []

    for baseline, candidate in combinations(MODES, 2):
        for seed in SEEDS:
            row = {
                "seed": seed,
                "baseline": baseline,
                "candidate": candidate,
            }

            for metric in METRICS:
                a = data[baseline][seed][metric]
                b = data[candidate][seed][metric]

                row[f"{baseline}_{metric}"] = a
                row[f"{candidate}_{metric}"] = b
                row[f"delta_{metric}"] = b - a
                row[f"percent_change_{metric}"] = (
                    ((b - a) / a) * 100 if a != 0 else ""
                )

            pairwise_rows.append(row)

    write_csv(
        RESULT_DIR / "three_mode_paired_comparison.csv",
        pairwise_rows,
    )

    # Aggregate descriptive statistics and paired confidence intervals.
    summary_rows = []

    for metric in METRICS:
        for mode in MODES:
            values = [data[mode][seed][metric] for seed in SEEDS]

            summary_rows.append({
                "metric": metric,
                "comparison": "descriptive",
                "mode": mode,
                "mean": mean(values),
                "sample_sd": sample_sd(values),
                "mean_paired_delta": "",
                "paired_delta_sd": "",
                "ci95_lower": "",
                "ci95_upper": "",
                "mean_paired_percent_change": "",
            })

        for baseline, candidate in combinations(MODES, 2):
            baseline_values = [
                data[baseline][seed][metric] for seed in SEEDS
            ]
            candidate_values = [
                data[candidate][seed][metric] for seed in SEEDS
            ]

            deltas = [
                candidate_values[i] - baseline_values[i]
                for i in range(len(SEEDS))
            ]

            percentages = [
                ((candidate_values[i] - baseline_values[i])
                 / baseline_values[i]) * 100
                for i in range(len(SEEDS))
                if baseline_values[i] != 0
            ]

            avg, sd, lower, upper = paired_summary(deltas)

            summary_rows.append({
                "metric": metric,
                "comparison": f"{candidate} minus {baseline}",
                "mode": "",
                "mean": "",
                "sample_sd": "",
                "mean_paired_delta": avg,
                "paired_delta_sd": sd,
                "ci95_lower": lower,
                "ci95_upper": upper,
                "mean_paired_percent_change": (
                    mean(percentages) if percentages else ""
                ),
            })

    write_csv(
        RESULT_DIR / "three_mode_summary.csv",
        summary_rows,
    )

    # Generate five comparison charts.
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        for metric in METRICS:
            fig, ax = plt.subplots(figsize=(8, 5))

            for mode in MODES:
                values = [
                    data[mode][seed][metric] for seed in SEEDS
                ]
                ax.plot(SEEDS, values, marker="o", label=mode)

            ax.set_xlabel("Random seed")
            ax.set_ylabel(DISPLAY_NAMES[metric])
            ax.set_title(DISPLAY_NAMES[metric] + " — three-mode comparison")
            ax.set_xticks(SEEDS)
            ax.grid(True, alpha=0.3)
            ax.legend()
            fig.tight_layout()

            fig.savefig(
                PLOT_DIR / f"three_mode_{metric}.png",
                dpi=300,
            )
            plt.close(fig)

        print("Five three-mode comparison charts generated.")

    except ImportError:
        print("WARNING: matplotlib unavailable; CSV files were generated.")

    print("\nThree-mode analysis completed.")
    print(f"Raw results:       {RESULT_DIR / 'three_mode_seed_results.csv'}")
    print(f"Paired comparison: {RESULT_DIR / 'three_mode_paired_comparison.csv'}")
    print(f"Summary:           {RESULT_DIR / 'three_mode_summary.csv'}")
    print(f"Charts:            {PLOT_DIR}")

    print("\nDescriptive mean results:")
    for metric in METRICS:
        print(f"\n{DISPLAY_NAMES[metric]}")
        for mode in MODES:
            values = [data[mode][seed][metric] for seed in SEEDS]
            print(
                f"  {mode}: mean={mean(values):.4f}, "
                f"sample SD={sample_sd(values):.4f}"
            )


if __name__ == "__main__":
    main()
