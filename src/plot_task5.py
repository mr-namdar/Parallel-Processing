#!/usr/bin/env python3
import argparse
import csv
import os
import statistics
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def read_csv(path):
    if not os.path.exists(path):
        raise FileNotFoundError(f"Missing {path}. Run `make task5` first.")
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def median_and_range(values):
    values = sorted(values)
    med = statistics.median(values)
    return med, med - min(values), max(values) - med


def plot_handoff(input_dir, output_dir):
    rows = read_csv(os.path.join(input_dir, "task5a_handoff.csv"))
    grouped = defaultdict(list)
    for r in rows:
        if r.get("status") == "ok":
            try:
                grouped[r["placement"]].append(float(r["ns_per_handoff"]))
            except (KeyError, ValueError):
                pass

    order = ["smt_siblings", "different_cores_same_socket", "different_numa_or_socket", "unpinned"]
    labels = ["SMT siblings", "Different cores\n(same socket)", "Different NUMA/socket", "Unpinned"]
    summary_path = os.path.join(input_dir, "task5a_handoff_summary.csv")
    with open(summary_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["placement", "valid_runs", "median_ns_per_handoff", "min_ns", "max_ns", "status"])
        for name in order:
            vals = grouped.get(name, [])
            if vals:
                writer.writerow([name, len(vals), f"{statistics.median(vals):.3f}", f"{min(vals):.3f}", f"{max(vals):.3f}", "ok"])
            else:
                statuses = [r.get("status", "") for r in rows if r.get("placement") == name]
                status = statuses[0] if statuses else "not_run"
                writer.writerow([name, 0, "", "", "", status])

    present = [(name, label) for name, label in zip(order, labels) if grouped.get(name)]
    if not present:
        print("No valid Task 5A measurements; plot not generated (check topology/affinity status in CSV).")
        return

    medians, low_err, high_err = [], [], []
    shown = []
    for name, label in present:
        med, lo, hi = median_and_range(grouped[name])
        medians.append(med); low_err.append(lo); high_err.append(hi); shown.append(label)

    fig, ax = plt.subplots(figsize=(9, 5.2))
    x = list(range(len(shown)))
    ax.bar(x, medians, yerr=[low_err, high_err], capsize=5)
    ax.set_xticks(x, shown)
    ax.set_ylabel("Nanoseconds per lock acquisition")
    ax.set_title("Task 5A — Lock hand-off cost")
    ax.grid(axis="y", alpha=0.25)
    ax.set_axisbelow(True)
    for i, v in enumerate(medians):
        ax.annotate(f"{v:.1f} ns", (i, v), textcoords="offset points", xytext=(0, 6), ha="center")
    fig.tight_layout()
    fig.savefig(os.path.join(output_dir, "task5a_handoff.png"), dpi=180)
    plt.close(fig)


def plot_false_sharing(input_dir, output_dir):
    rows = read_csv(os.path.join(input_dir, "task5b_false_sharing.csv"))
    grouped = defaultdict(list)
    for r in rows:
        if r.get("status") == "ok":
            try:
                grouped[(int(r["N"]), r["layout"])].append(float(r["elapsed_s"]))
            except (KeyError, ValueError):
                pass
    ns = sorted({key[0] for key in grouped})
    summary_path = os.path.join(input_dir, "task5b_false_sharing_summary.csv")
    with open(summary_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["N", "layout", "valid_runs", "median_elapsed_s", "median_ns_per_increment", "status"])
        all_keys = sorted({(int(r["N"]), r["layout"]) for r in rows if r.get("N", "").isdigit()})
        for n, layout in all_keys:
            valid = [r for r in rows if r.get("status") == "ok" and r.get("N") == str(n) and r.get("layout") == layout]
            if valid:
                times = [float(r["elapsed_s"]) for r in valid]
                ns_per = [float(r["ns_per_increment"]) for r in valid]
                writer.writerow([n, layout, len(valid), f"{statistics.median(times):.9f}", f"{statistics.median(ns_per):.3f}", "ok"])
            else:
                writer.writerow([n, layout, 0, "", "", "no_valid_run"])
    if not ns:
        print("No valid Task 5B measurements; plot not generated.")
        return

    packed = [statistics.median(grouped[(n, "packed")]) if grouped.get((n, "packed")) else float("nan") for n in ns]
    padded = [statistics.median(grouped[(n, "padded")]) if grouped.get((n, "padded")) else float("nan") for n in ns]
    width = 0.36
    x = list(range(len(ns)))
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    ax.bar([v - width / 2 for v in x], packed, width, label="Packed")
    ax.bar([v + width / 2 for v in x], padded, width, label="Padded")
    ax.set_xticks(x, [str(n) for n in ns])
    ax.set_xlabel("Number of threads (N)")
    ax.set_ylabel("Elapsed time (seconds)")
    ax.set_title("Task 5B — False sharing: packed vs padded counters")
    ax.legend()
    ax.grid(axis="y", alpha=0.25)
    ax.set_axisbelow(True)
    fig.tight_layout()
    fig.savefig(os.path.join(output_dir, "task5b_false_sharing.png"), dpi=180)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description="Plot Task 5 measurements from CSV files.")
    parser.add_argument("--input", default="results", help="Directory containing Task 5 CSV files")
    parser.add_argument("--output", default="plots", help="Directory for PNG plots")
    args = parser.parse_args()
    os.makedirs(args.output, exist_ok=True)
    plot_handoff(args.input, args.output)
    plot_false_sharing(args.input, args.output)
    print(f"Plots (when data are available) are saved in: {args.output}/")


if __name__ == "__main__":
    main()
