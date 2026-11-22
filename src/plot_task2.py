#!/usr/bin/env python3
"""Create Task 2 Peterson-lock figures from the program's CSV output.

All generated figures are written to the repository's plots/ directory.
The StoreLoad figure is conceptual; the other figures use measured CSV data.
"""

from __future__ import annotations

import csv
import sys
from pathlib import Path

import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
PLOTS = ROOT / "plots"
RAW_CSV = RESULTS / "task2_peterson_raw.csv"
SUMMARY_CSV = RESULTS / "task2_peterson_summary.csv"

VARIANT_ORDER = ["A_plain", "B_volatile", "C_seq_cst"]
VARIANT_LABELS = {
    "A_plain": "A. Plain int",
    "B_volatile": "B. volatile",
    "C_seq_cst": "C. Fixed (seq_cst)",
}


def read_csv(path: Path) -> list[dict[str, str]]:
    try:
        with path.open("r", newline="", encoding="utf-8") as handle:
            return list(csv.DictReader(handle))
    except OSError as exc:
        print(f"Could not read {path}: {exc}", file=sys.stderr)
        return []


def numeric(row: dict[str, str], key: str) -> float | None:
    value = row.get(key, "").strip()
    if not value:
        return None
    try:
        return float(value)
    except ValueError:
        return None


def plot_storeload_timeline() -> Path:
    """Draw the two-thread StoreLoad-reordering scenario described in the PDF."""
    fig, ax = plt.subplots(figsize=(13, 5.8))
    ax.set_xlim(0, 6.1)
    ax.set_ylim(-0.8, 2.25)
    ax.axis("off")

    ax.text(0.0, 2.05, "Conceptual StoreLoad-reordering timeline (volatile Peterson)",
            fontsize=14, fontweight="bold", va="center")

    events = [
        (1.2, "Store occupied[0] = 1\n(may remain buffered)"),
        (2.7, "Store last = 1"),
        (4.15, "Load occupied[1] -> 0\n(stale value)"),
        (5.5, "Enter\ncritical section"),
    ]
    events_t1 = [
        (1.2, "Store occupied[1] = 1\n(may remain buffered)"),
        (2.7, "Store last = 0"),
        (4.15, "Load occupied[0] -> 0\n(stale value)"),
        (5.5, "Enter\ncritical section"),
    ]

    y_positions = [1.35, 0.35]
    labels = ["Thread 0", "Thread 1"]
    for idx, (y, row_events) in enumerate(zip(y_positions, [events, events_t1])):
        ax.text(0.02, y, labels[idx], fontsize=11, fontweight="bold",
                ha="left", va="center")
        right_edges = [0.70, 0.42, 0.62]
        left_edges = [0.42, 0.62, 0.52]
        for j, (x, label) in enumerate(row_events):
            ax.text(x, y, label, ha="center", va="center", fontsize=8.5,
                    bbox={"boxstyle": "round,pad=0.42"})
            if j < len(row_events) - 1:
                next_x = row_events[j + 1][0]
                ax.annotate("", xy=(next_x - left_edges[j], y),
                            xytext=(x + right_edges[j], y),
                            arrowprops={"arrowstyle": "->", "lw": 1.0})

    ax.text(4.15, 1.82, "StoreLoad window", ha="center", va="center", fontsize=9)
    ax.annotate("", xy=(4.15, 1.58), xytext=(4.15, 1.68),
                arrowprops={"arrowstyle": "->", "lw": 1.0})
    ax.text(2.8, -0.32,
            "Both loads can observe the other thread's flag as 0 before the earlier flag store\n"
            "becomes visible to the other core. Both while conditions can then be false.",
            ha="center", va="center", fontsize=9.5)
    ax.text(2.8, -0.67,
            "This is an explanatory CPU-ordering scenario, not a measurement from the user's VM.",
            ha="center", va="center", fontsize=8.5)

    output = PLOTS / "task2_storeload_timeline.png"
    fig.tight_layout()
    fig.savefig(output, dpi=180, bbox_inches="tight")
    plt.close(fig)
    return output


def plot_correctness(summary: list[dict[str, str]]) -> Path | None:
    if not summary:
        return None

    rows = {row.get("variant", ""): row for row in summary}
    variants = [name for name in VARIANT_ORDER if name in rows]
    if not variants:
        return None

    passed: list[float] = []
    not_passed: list[float] = []
    repetitions: list[int] = []
    for variant in variants:
        row = rows[variant]
        rep = int(numeric(row, "repetitions") or 0)
        ok = int(numeric(row, "passed") or 0)
        repetitions.append(rep)
        passed.append(float(ok))
        not_passed.append(float(max(0, rep - ok)))

    fig, ax = plt.subplots(figsize=(9, 5.5))
    x = list(range(len(variants)))
    ax.bar(x, passed, label="Correct final count")
    ax.bar(x, not_passed, bottom=passed,
           label="Incorrect / timeout / crash / error")
    ax.set_xticks(x, [VARIANT_LABELS.get(v, v) for v in variants])
    ax.set_ylabel("Number of runs")
    ax.set_title("Peterson lock: final-count correctness across repetitions")
    ax.set_ylim(0, max(repetitions, default=10) + 1)
    ax.grid(axis="y", alpha=0.25)
    ax.legend()

    for pos, ok, rep in zip(x, passed, repetitions):
        ax.text(pos, rep + 0.12, f"{int(ok)}/{rep}", ha="center", va="bottom")

    fig.tight_layout()
    output = PLOTS / "task2_correctness.png"
    fig.savefig(output, dpi=180, bbox_inches="tight")
    plt.close(fig)
    return output


def plot_count_error(raw: list[dict[str, str]]) -> Path | None:
    if not any(numeric(row, "actual_total") is not None for row in raw):
        return None

    fig, ax = plt.subplots(figsize=(10, 5.5))
    plotted = False
    for variant in VARIANT_ORDER:
        points = [row for row in raw if row.get("variant") == variant]
        points.sort(key=lambda row: int(numeric(row, "run") or 0))
        if not points:
            continue
        run_numbers: list[float | None] = []
        errors: list[float] = []
        for row in points:
            run_number = numeric(row, "run")
            actual = numeric(row, "actual_total")
            expected = numeric(row, "expected_total")
            run_numbers.append(run_number)
            # Preserve gaps for timeouts/crashes instead of connecting over them.
            errors.append(actual - expected if actual is not None and expected is not None
                          else float("nan"))
        ax.plot(run_numbers, errors, marker="o", linewidth=1.3,
                label=VARIANT_LABELS.get(variant, variant))
        plotted = True

    if not plotted:
        plt.close(fig)
        return None

    ax.axhline(0, linestyle="--", linewidth=1)
    ax.set_xlabel("Repetition")
    ax.set_ylabel("Actual total - Expected total")
    ax.set_title("Peterson lock: shared-counter error per completed run")
    ax.grid(alpha=0.25)
    ax.legend()
    fig.tight_layout()
    output = PLOTS / "task2_count_error.png"
    fig.savefig(output, dpi=180, bbox_inches="tight")
    plt.close(fig)
    return output


def plot_elapsed_time(raw: list[dict[str, str]]) -> Path | None:
    """Compare measured completion times; timeout rows are deliberately excluded."""
    fig, ax = plt.subplots(figsize=(9, 5.5))
    plotted = False
    for variant in VARIANT_ORDER:
        points = [row for row in raw
                  if row.get("variant") == variant
                  and row.get("status") in {"pass", "incorrect"}
                  and numeric(row, "elapsed_s") is not None]
        if not points:
            continue
        runs = [numeric(row, "run") for row in points]
        times = [numeric(row, "elapsed_s") for row in points]
        ax.scatter(runs, times, label=VARIANT_LABELS.get(variant, variant))
        plotted = True

    if not plotted:
        plt.close(fig)
        return None

    ax.set_xlabel("Repetition")
    ax.set_ylabel("Elapsed time (seconds)")
    ax.set_title("Peterson lock: elapsed time of completed stress tests")
    ax.grid(alpha=0.25)
    ax.legend()
    fig.tight_layout()
    output = PLOTS / "task2_elapsed_time.png"
    fig.savefig(output, dpi=180, bbox_inches="tight")
    plt.close(fig)
    return output


def main() -> int:
    PLOTS.mkdir(parents=True, exist_ok=True)
    generated: list[Path] = [plot_storeload_timeline()]

    if not RAW_CSV.exists() or not SUMMARY_CSV.exists():
        print("Task 2 CSV files are not present yet.")
        print("Generated the conceptual StoreLoad timeline only.")
        print("Run 'make task2' first, then 'make task2-plots'.")
        print(f"Saved: {generated[0].relative_to(ROOT)}")
        return 0

    raw = read_csv(RAW_CSV)
    summary = read_csv(SUMMARY_CSV)
    for plotter, data in [
        (plot_correctness, summary),
        (plot_count_error, raw),
        (plot_elapsed_time, raw),
    ]:
        output = plotter(data)  # type: ignore[arg-type]
        if output is not None:
            generated.append(output)

    print("Task 2 plots saved in plots/:")
    for path in generated:
        print(f"  {path.relative_to(ROOT)}")
    if len(generated) == 1:
        print("No completed measurements were available for data plots.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
