#!/usr/bin/env python3
"""Create Task 3 plots from the raw CSV files produced by task3.c."""
from __future__ import annotations

import csv
import math
from pathlib import Path
import statistics

import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
PLOTS = ROOT / "plots"
PLOTS.mkdir(parents=True, exist_ok=True)


def read_csv(name: str) -> list[dict[str, str]]:
    path = RESULTS / name
    if not path.exists():
        raise SystemExit(f"Missing {path}. Run `make task3` first.")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def as_float(row: dict[str, str], key: str) -> float:
    value = row.get(key, "")
    if value in (None, "", "nan", "NaN"):
        return math.nan
    try:
        return float(value)
    except ValueError:
        return math.nan


def plot_uncontended() -> None:
    rows = read_csv("task3_uncontended_summary.csv")
    labels = [r["lock"] for r in rows]
    values = [as_float(r, "overhead_ns_per_lock_pair") for r in rows]
    fig, ax = plt.subplots(figsize=(10, 5.5))
    bars = ax.bar(labels, values)
    ax.axhline(0.0, linewidth=0.8)
    ax.set_title("Task 3a: estimated uncontended lock/unlock cost")
    ax.set_ylabel("Added time per record (ns)")
    ax.set_xlabel("Lock implementation")
    ax.tick_params(axis="x", rotation=18)
    ax.grid(axis="y", alpha=0.25)
    for bar, value in zip(bars, values):
        if math.isfinite(value):
            ax.annotate(f"{value:.2f}", (bar.get_x() + bar.get_width() / 2, value),
                        xytext=(0, 4 if value >= 0 else -13), textcoords="offset points",
                        ha="center", fontsize=8)
    fig.tight_layout()
    fig.savefig(PLOTS / "task3_uncontended_cost.png", dpi=180)
    plt.close(fig)


def fairness_medians(rows: list[dict[str, str]], metric: str) -> tuple[list[str], list[float], list[float]]:
    locks = list(dict.fromkeys(r["lock"] for r in rows))
    ns = sorted({int(r["N"]) for r in rows})
    series: dict[tuple[str, int], list[float]] = {}
    for row in rows:
        value = as_float(row, metric)
        if math.isfinite(value):
            series.setdefault((row["lock"], int(row["N"])), []).append(value)
    # Return the lock labels and medians for the two N values in separate arrays.
    n1_values = [statistics.median(series.get((lock, ns[0]), [math.nan])) for lock in locks]
    n2_values = [statistics.median(series.get((lock, ns[1]), [math.nan])) for lock in locks]
    return locks, n1_values, n2_values


def plot_fairness() -> None:
    rows = read_csv("task3_fairness_summary.csv")
    for metric, filename, title, ylabel in [
        ("jain_index", "task3_fairness_jain.png", "Task 3b: fairness (Jain's index)", "Jain's index (1 = perfectly fair)"),
        ("max_min_ratio", "task3_fairness_ratio.png", "Task 3b: fairness (max/min acquisitions)", "Maximum acquisitions / minimum acquisitions"),
    ]:
        locks, hvals, twohvals = fairness_medians(rows, metric)
        x = list(range(len(locks)))
        width = 0.36
        fig, ax = plt.subplots(figsize=(10, 5.5))
        ax.bar([v - width / 2 for v in x], hvals, width, label=f"N = {min(int(r['N']) for r in rows)} (H)")
        ax.bar([v + width / 2 for v in x], twohvals, width, label=f"N = {max(int(r['N']) for r in rows)} (2H)")
        ax.set_xticks(x, locks, rotation=18)
        ax.set_title(title)
        ax.set_ylabel(ylabel)
        ax.set_xlabel("Lock implementation")
        ax.grid(axis="y", alpha=0.25)
        ax.legend()
        fig.tight_layout()
        fig.savefig(PLOTS / filename, dpi=180)
        plt.close(fig)


def plot_ttas_pause() -> None:
    pause_rows = read_csv("task3_ttas_pause_raw.csv")
    no_pause_rows = read_csv("task3_ttas_nopause_raw.csv")
    data: dict[tuple[int, str], list[float]] = {}
    for label, rows in (("USE_PAUSE", pause_rows), ("No pause", no_pause_rows)):
        for row in rows:
            if row.get("status") == "completed" and row.get("ok") == "1":
                t = as_float(row, "time_s")
                if math.isfinite(t):
                    data.setdefault((int(row["N"]), label), []).append(t)
    ns = sorted({n for n, _ in data})
    if not ns:
        raise SystemExit("No completed, correct TTAS rows found for the pause comparison.")
    x = list(range(len(ns)))
    width = 0.36
    fig, ax = plt.subplots(figsize=(8.5, 5.5))
    for offset, label in ((-width / 2, "USE_PAUSE"), (width / 2, "No pause")):
        medians = [statistics.median(data.get((n, label), [math.nan])) for n in ns]
        ax.bar([v + offset for v in x], medians, width, label=label)
    ax.set_xticks(x, [str(n) for n in ns])
    ax.set_title("Task 3: TTAS with and without the spin hint")
    ax.set_xlabel("Number of threads (N)")
    ax.set_ylabel("Elapsed time (seconds)")
    ax.grid(axis="y", alpha=0.25)
    ax.legend()
    fig.tight_layout()
    fig.savefig(PLOTS / "task3_ttas_pause_comparison.png", dpi=180)
    plt.close(fig)


def main() -> None:
    plot_uncontended()
    plot_fairness()
    plot_ttas_pause()
    print("Task 3 plots written to plots/:")
    for name in (
        "task3_uncontended_cost.png",
        "task3_fairness_jain.png",
        "task3_fairness_ratio.png",
        "task3_ttas_pause_comparison.png",
    ):
        print(f"  {name}")


if __name__ == "__main__":
    main()