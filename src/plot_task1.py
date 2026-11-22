from __future__ import annotations

import argparse
import csv
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

import matplotlib.pyplot as plt


def read_csv(path: Path, required_columns: set[str]) -> list[dict[str, str]]:
    if not path.is_file():
        raise FileNotFoundError(f"CSV file not found: {path}")

    with path.open("r", newline="", encoding="utf-8-sig") as file:
        reader = csv.DictReader(file)
        if reader.fieldnames is None:
            raise ValueError(f"CSV file is empty or has no header: {path}")

        missing = required_columns.difference(reader.fieldnames)
        if missing:
            missing_text = ", ".join(sorted(missing))
            raise ValueError(f"Missing column(s) in {path.name}: {missing_text}")

        rows = list(reader)

    if not rows:
        raise ValueError(f"CSV file has a header but no data rows: {path}")
    return rows


def number(row: dict[str, str], column: str, source: str) -> float:
    value = row.get(column, "").strip()
    try:
        return float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(
            f"Invalid number in {source}, column '{column}': {value!r}"
        ) from exc


def load_timed_out_configs(raw_path: Path) -> set[tuple[int, float, int]]:
    """Return (N, F, C) configurations that contain a timeout run."""
    if not raw_path.is_file():
        return set()

    rows = read_csv(
        raw_path,
        {"N", "F", "C", "status"},
    )
    timed_out: set[tuple[int, float, int]] = set()
    for row in rows:
        status = row["status"].strip().lower()
        if status == "timeout":
            key = (
                int(number(row, "N", raw_path.name)),
                number(row, "F", raw_path.name),
                int(number(row, "C", raw_path.name)),
            )
            timed_out.add(key)
    return timed_out


def build_error_rows(
    summary_rows: list[dict[str, str]],
    timed_out: set[tuple[int, float, int]],
) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []

    for row in summary_rows:
        n = int(number(row, "N", "nolock_summary.csv"))
        f_target = number(row, "F", "nolock_summary.csv")
        c = int(number(row, "C", "nolock_summary.csv"))
        key = (n, f_target, c)

        if key in timed_out:
            print(
                f"Skipping N={n}, F={f_target:g}, C={c}: "
                "at least one raw run timed out.",
                file=sys.stderr,
            )
            continue

        expected_total = number(row, "expected_total", "nolock_summary.csv")
        actual_total = number(row, "median_actual_total", "nolock_summary.csv")
        expected_hist = number(row, "expected_hist_sum", "nolock_summary.csv")
        actual_hist = number(row, "median_actual_hist_sum", "nolock_summary.csv")

        if expected_total <= 0 or expected_hist <= 0:
            raise ValueError(
                f"Expected counts must be positive for N={n}, F={f_target:g}, C={c}."
            )

        total_error = abs(expected_total - actual_total) / expected_total * 100.0
        hist_error = abs(expected_hist - actual_hist) / expected_hist * 100.0

        results.append(
            {
                "N": n,
                "F": f_target,
                "C": c,
                "median_time_s": number(row, "median_time_s", "nolock_summary.csv"),
                "expected_total": int(expected_total),
                "median_actual_total": int(actual_total),
                "total_error_percent": total_error,
                "expected_hist_sum": int(expected_hist),
                "median_actual_hist_sum": int(actual_hist),
                "hist_error_percent": hist_error,
                "ok_runs": int(number(row, "ok_runs", "nolock_summary.csv")),
                "total_runs": int(number(row, "total_runs", "nolock_summary.csv")),
            }
        )

    if not results:
        raise ValueError(
            "No completed no-lock configurations were available to plot. "
            "Check nolock_summary.csv and nolock_raw.csv."
        )

    return sorted(results, key=lambda item: (item["C"], item["F"], item["N"]))


def save_error_plot(
    rows: list[dict[str, Any]],
    metric: str,
    ylabel: str,
    title: str,
    output_path: Path,
) -> None:
    grouped: dict[tuple[int, float], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        grouped[(row["C"], row["F"])].append(row)

    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    for (c, f_target), points in sorted(grouped.items()):
        points.sort(key=lambda item: item["N"])
        x_values = [point["N"] for point in points]
        y_values = [point[metric] for point in points]
        ax.plot(
            x_values,
            y_values,
            marker="o",
            linewidth=1.8,
            label=f"F={f_target:g}, C={c}",
        )

    thread_counts = sorted({row["N"] for row in rows})
    ax.set_xticks(thread_counts)
    ax.set_xlabel("Number of threads (N)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(True, linestyle="--", alpha=0.45)
    ax.legend(title="Configuration")
    fig.tight_layout()
    fig.savefig(output_path, dpi=200, bbox_inches="tight")
    plt.close(fig)


def save_calibration_plot(
    calibration_rows: list[dict[str, str]],
    c16_rows: list[dict[str, str]],
    output_path: Path,
) -> None:
    calibration_by_f: dict[float, float] = {}
    for row in calibration_rows:
        if int(number(row, "C", "calibration.csv")) == 64:
            f_target = number(row, "F", "calibration.csv")
            calibration_by_f[f_target] = number(
                row, "F_measured", "calibration.csv"
            )

    c16_by_f: dict[float, float] = {}
    for row in c16_rows:
        f_target = number(row, "F_target", "c16_sensitivity.csv")
        c16_by_f[f_target] = number(
            row, "F_measured_at_C16", "c16_sensitivity.csv"
        )

    targets = sorted(set(calibration_by_f) | set(c16_by_f))
    if not targets:
        raise ValueError("No calibration values were found to plot.")

    missing_c64 = [target for target in targets if target not in calibration_by_f]
    missing_c16 = [target for target in targets if target not in c16_by_f]
    if missing_c64 or missing_c16:
        raise ValueError(
            "Calibration plot needs both C=64 and C=16 values for each target F. "
            f"Missing C=64 values: {missing_c64}; missing C=16 values: {missing_c16}."
        )

    positions = list(range(len(targets)))
    width = 0.24

    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    ax.bar(
        [position - width for position in positions],
        targets,
        width,
        label="Target F",
    )
    ax.bar(
        positions,
        [calibration_by_f[target] for target in targets],
        width,
        label="Measured F (C=64)",
    )
    ax.bar(
        [position + width for position in positions],
        [c16_by_f[target] for target in targets],
        width,
        label="Measured F (C=16)",
    )

    ax.set_xticks(positions)
    ax.set_xticklabels([f"F={target:g}" for target in targets])
    ax.set_ylabel("Serial fraction (F)")
    ax.set_title("Calibration and C=16 sensitivity check")
    ax.grid(True, axis="y", linestyle="--", alpha=0.45)
    ax.legend()
    fig.tight_layout()
    fig.savefig(output_path, dpi=200, bbox_inches="tight")
    plt.close(fig)


def write_derived_csv(rows: list[dict[str, Any]], path: Path) -> None:
    fieldnames = [
        "N",
        "F",
        "C",
        "median_time_s",
        "expected_total",
        "median_actual_total",
        "total_error_percent",
        "expected_hist_sum",
        "median_actual_hist_sum",
        "hist_error_percent",
        "ok_runs",
        "total_runs",
    ]
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create Task 1 plots using the benchmark CSV files."
    )
    parser.add_argument(
        "--input-dir",
        type=Path,
        default=Path("results"),
        help="Folder containing the Task 1 CSV files (default: results).",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("plots"),
        help="Folder where plots and derived CSV will be saved (default: plots).",
    )
    args = parser.parse_args()

    try:
        calibration_rows = read_csv(
            args.input_dir / "calibration.csv",
            {"F", "C", "W", "F_measured", "within_0_05"},
        )
        c16_rows = read_csv(
            args.input_dir / "c16_sensitivity.csv",
            {"F_target", "C", "W_kept_from_C64", "F_measured_at_C16"},
        )
        summary_rows = read_csv(
            args.input_dir / "nolock_summary.csv",
            {
                "N",
                "F",
                "C",
                "median_time_s",
                "median_actual_total",
                "median_actual_hist_sum",
                "expected_total",
                "expected_hist_sum",
                "ok_runs",
                "total_runs",
            },
        )
        timed_out = load_timed_out_configs(args.input_dir / "nolock_raw.csv")
        error_rows = build_error_rows(summary_rows, timed_out)

        args.output_dir.mkdir(parents=True, exist_ok=True)
        save_calibration_plot(
            calibration_rows,
            c16_rows,
            args.output_dir / "calibration_fractions.png",
        )
        save_error_plot(
            error_rows,
            "total_error_percent",
            "Absolute error in total (%)",
            "No-lock race: total counter error",
            args.output_dir / "total_race_error.png",
        )
        save_error_plot(
            error_rows,
            "hist_error_percent",
            "Absolute error in histogram sum (%)",
            "No-lock race: histogram sum error",
            args.output_dir / "histogram_race_error.png",
        )
        write_derived_csv(error_rows, args.output_dir / "task1_plot_data.csv")

    except (FileNotFoundError, ValueError, OSError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    print("Task 1 plots were created:")
    print(f"  {args.output_dir / 'calibration_fractions.png'}")
    print(f"  {args.output_dir / 'total_race_error.png'}")
    print(f"  {args.output_dir / 'histogram_race_error.png'}")
    print(f"  {args.output_dir / 'task1_plot_data.csv'}")
    print("Race-error values use the median counts from nolock_summary.csv.")
    print("Error (%) = abs(expected - actual) / expected * 100.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
