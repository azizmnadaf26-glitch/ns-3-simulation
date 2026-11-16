#!/usr/bin/env python3

import csv
import re
import statistics
from pathlib import Path

BASE = Path.home() / "ENFS"
LOG_DIR = BASE / "logs"
RESULT_DIR = BASE / "results"
PLOT_DIR = BASE / "plots"

SEEDS = [101, 102, 103, 104, 105]

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

        # Use the final occurrence, corresponding to the aggregate summary.
        value = matches[-1]
        result[field] = float(value) if "." in value else int(value)

    return result


def write_csv(path, rows, fields):
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def mean(values):
    return statistics.mean(values)


def sample_sd(values):
    return statistics.stdev(values) if len(values) > 1 else 0.0


def main():
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    PLOT_DIR.mkdir(parents=True, exist_ok=True)

    raw_rows = []
    paired_rows = []

    for seed in SEEDS:
        control = extract_metrics(
            LOG_DIR / f"control-seed{seed}-run1.txt"
        )
        enfs = extract_metrics(
            LOG_DIR / f"enfs-seed{seed}-run1.txt"
        )

        for mode, values in [("Control", control), ("ENFS", enfs)]:
            raw_rows.append({
                "seed": seed,
                "mode": mode,
                **values,
            })

        row = {"seed": seed}

        for metric in METRICS:
            c = control[metric]
            e = enfs[metric]
            row[f"control_{metric}"] = c
            row[f"enfs_{metric}"] = e
            row[f"delta_{metric}"] = e - c
            row[f"percent_change_{metric}"] = (
                ((e - c) / c) * 100 if c != 0 else ""
            )

        paired_rows.append(row)

    raw_fields = ["seed", "mode", *PATTERNS.keys()]
    write_csv(RESULT_DIR / "seed_results.csv", raw_rows, raw_fields)

    paired_fields = ["seed"]
    for metric in METRICS:
        paired_fields.extend([
            f"control_{metric}",
            f"enfs_{metric}",
            f"delta_{metric}",
            f"percent_change_{metric}",
        ])
    write_csv(
        RESULT_DIR / "paired_comparison.csv",
        paired_rows,
        paired_fields,
    )

    summary_rows = []

    for metric in METRICS:
        control_values = [
            row[f"control_{metric}"] for row in paired_rows
        ]
        enfs_values = [
            row[f"enfs_{metric}"] for row in paired_rows
        ]
        deltas = [
            row[f"delta_{metric}"] for row in paired_rows
        ]
        pct_changes = [
            row[f"percent_change_{metric}"] for row in paired_rows
        ]

        summary_rows.append({
            "metric": metric,
            "control_mean": mean(control_values),
            "control_sample_sd": sample_sd(control_values),
            "enfs_mean": mean(enfs_values),
            "enfs_sample_sd": sample_sd(enfs_values),
            "mean_paired_delta_enfs_minus_control": mean(deltas),
            "paired_delta_sample_sd": sample_sd(deltas),
            "mean_paired_percent_change": mean(pct_changes),
        })

    write_csv(
        RESULT_DIR / "aggregate_summary.csv",
        summary_rows,
        list(summary_rows[0].keys()),
    )

    # Generate charts. If matplotlib is unavailable, CSV analysis remains valid.
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        for metric in METRICS:
            control_values = [
                row[f"control_{metric}"] for row in paired_rows
            ]
            enfs_values = [
                row[f"enfs_{metric}"] for row in paired_rows
            ]

            fig, ax = plt.subplots(figsize=(8, 5))

            x = range(len(SEEDS))
            ax.plot(x, control_values, marker="o", label="Control")
            ax.plot(x, enfs_values, marker="s", label="ENFS")

            ax.set_xticks(list(x))
            ax.set_xticklabels([str(seed) for seed in SEEDS])
            ax.set_xlabel("Random seed")
            ax.set_ylabel(DISPLAY_NAMES[metric])
            ax.set_title(DISPLAY_NAMES[metric] + " — paired runs")
            ax.grid(True, alpha=0.3)
            ax.legend()
            fig.tight_layout()

            fig.savefig(
                PLOT_DIR / f"{metric}_comparison.png",
                dpi=300,
            )
            plt.close(fig)

        print("Charts generated successfully.")

    except ImportError:
        print("WARNING: matplotlib is unavailable; CSV files were generated.")
        print("Install it with: python3 -m pip install matplotlib")

    print("\nAnalysis completed.")
    print(f"Raw results:       {RESULT_DIR / 'seed_results.csv'}")
    print(f"Paired comparison: {RESULT_DIR / 'paired_comparison.csv'}")
    print(f"Aggregate summary: {RESULT_DIR / 'aggregate_summary.csv'}")
    print(f"Charts:            {PLOT_DIR}")
    print("\nAggregate results:")
    for row in summary_rows:
        print(
            f"{row['metric']}: "
            f"Control={row['control_mean']:.3f}, "
            f"ENFS={row['enfs_mean']:.3f}, "
            f"paired change={row['mean_paired_percent_change']:.2f}%"
        )


if __name__ == "__main__":
    main()
