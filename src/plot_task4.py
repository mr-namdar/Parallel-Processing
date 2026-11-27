#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any

import matplotlib.pyplot as plt

LOCK_ORDER = ["TAS", "TTAS", "TTAS_backoff", "Ticket", "pthread_mutex"]


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise SystemExit(f"Required CSV file not found: {path}")
    with path.open("r", newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise SystemExit(f"CSV file is empty: {path}")
    return rows


def number(row: dict[str, str], key: str, default: float = math.nan) -> float:
    value = row.get(key, "")
    try:
        parsed = float(value)
        return parsed if math.isfinite(parsed) else default
    except (TypeError, ValueError):
        return default


def successful(row: dict[str, str]) -> bool:
    status = row.get("status", "").strip().lower()
    ok = row.get("ok", "").strip()
    return ok == "1" and status in {"completed", "ok", "success"}


def fmt(value: float, digits: int = 3) -> str:
    return "NA" if not math.isfinite(value) else f"{value:.{digits}f}"


def median_or_nan(values: list[float]) -> float:
    clean = [v for v in values if math.isfinite(v)]
    return statistics.median(clean) if clean else math.nan


def serial_baselines(serial_rows: list[dict[str, str]]) -> dict[float, float]:
    grouped: dict[float, list[float]] = defaultdict(list)
    for row in serial_rows:
        f_value = number(row, "F")
        elapsed = number(row, "time_s")
        if successful(row) and math.isfinite(f_value) and elapsed > 0:
            grouped[round(f_value, 1)].append(elapsed)

    result = {f_value: median_or_nan(times) for f_value, times in grouped.items()}
    for target in (0.5, 0.1):
        if target not in result or not math.isfinite(result[target]):
            raise SystemExit(f"No successful serial reference measurements for F={target:.1f}.")
        if result[target] < 2.0:
            raise SystemExit(
                f"Serial median for F={target:.1f} is {result[target]:.3f}s; "
                "the assignment requires at least 2 seconds."
            )
    return result


def amdahl_bound(n: int, h: int, f_measured: float) -> float:
    effective_n = min(n, h)  # The assignment caps N>H at S_max(H).
    return 1.0 / (f_measured + (1.0 - f_measured) / effective_n)


def derive_rows(
    raw_rows: list[dict[str, str]],
    serial: dict[float, float],
    h: int,
) -> list[dict[str, Any]]:
    all_grouped: dict[tuple[str, float, int], list[dict[str, str]]] = defaultdict(list)
    for row in raw_rows:
        lock = row.get("lock", "").strip()
        f_value = number(row, "F")
        n_value = number(row, "N")
        if not lock or not math.isfinite(f_value) or not math.isfinite(n_value):
            continue
        n = int(n_value)
        if n != n_value:
            continue
        all_grouped[(lock, round(f_value, 1), n)].append(row)

    derived: list[dict[str, Any]] = []
    for (lock, target_f, n), rows in sorted(
        all_grouped.items(),
        key=lambda item: (item[0][1], item[0][2], LOCK_ORDER.index(item[0][0]) if item[0][0] in LOCK_ORDER else 99),
    ):
        good = [row for row in rows if successful(row)]
        elapsed = median_or_nan([number(row, "time_s") for row in good])
        task_clock = median_or_nan([number(row, "task_clock_ms") for row in good])
        context_switches = median_or_nan([number(row, "context_switches") for row in good])
        serial_time = serial.get(target_f, math.nan)
        speedup = serial_time / elapsed if elapsed > 0 and math.isfinite(serial_time) else math.nan

        # Calibrated F_measured is constant for one target-F configuration.
        measured_values = [number(row, "F_measured") for row in rows]
        f_measured = median_or_nan(measured_values)
        if not (0.0 < f_measured < 1.0):
            f_measured = target_f

        bound = amdahl_bound(n, h, f_measured)
        karp_flatt = math.nan
        if 2 <= n <= h and math.isfinite(speedup) and speedup > 0:
            karp_flatt = (1.0 / speedup - 1.0 / n) / (1.0 - 1.0 / n)

        timeout_count = sum(1 for row in rows if row.get("status", "").strip().lower() == "timeout")
        incorrect_count = sum(1 for row in rows if row.get("status", "").strip().lower() == "incorrect" or
                              (row.get("ok", "").strip() == "0" and row.get("status", "").strip().lower() != "timeout"))
        below_bound = (math.isfinite(speedup) and speedup < bound)

        derived.append({
            "lock": lock,
            "F": target_f,
            "F_measured": f_measured,
            "N": n,
            "runs": len(rows),
            "successful_runs": len(good),
            "timeouts": timeout_count,
            "incorrect_runs": incorrect_count,
            "T_serial_median_s": serial_time,
            "T_parallel_median_s": elapsed,
            "speedup_S": speedup,
            "amdahl_bound": bound,
            "karp_flatt_e": karp_flatt,
            "task_clock_median_ms": task_clock,
            "context_switches_median": context_switches,
            "below_amdahl_bound": below_bound if math.isfinite(speedup) else "NA",
        })
    return derived


def write_table(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def plot_speedup(rows: list[dict[str, Any]], h: int, f_value: float, out_dir: Path) -> Path:
    subset = [row for row in rows if row["F"] == f_value]
    ns = sorted({int(row["N"]) for row in subset})
    if not ns:
        raise SystemExit(f"No Task 4 data found for F={f_value:.1f}.")

    fig, ax = plt.subplots(figsize=(10.5, 6.2))
    for lock in LOCK_ORDER:
        lock_rows = {int(row["N"]): row for row in subset if row["lock"] == lock}
        x_values = [n for n in ns if n in lock_rows and math.isfinite(lock_rows[n]["speedup_S"])]
        y_values = [lock_rows[n]["speedup_S"] for n in x_values]
        if x_values:
            ax.plot(x_values, y_values, marker="o", linewidth=2, label=lock)

    f_measured_values = [row["F_measured"] for row in subset if math.isfinite(row["F_measured"])]
    f_measured = median_or_nan(f_measured_values) if f_measured_values else f_value
    theoretical = [amdahl_bound(n, h, f_measured) for n in ns]
    ax.plot(ns, theoretical, linestyle="--", marker="s", linewidth=2.2,
            label=f"Amdahl bound (F_measured={f_measured:.4f})")

    ax.set_xscale("log", base=2)
    ax.set_xticks(ns, [str(n) for n in ns])
    ax.set_xlabel("Number of threads (N)")
    ax.set_ylabel("Speedup S(N) = T_serial / T_parallel(N)")
    ax.set_title(f"Task 4: measured speedup vs. Amdahl bound (F={f_value:.1f})")
    ax.grid(True, which="both", alpha=0.28)
    ax.legend(loc="best", fontsize=9)
    fig.tight_layout()
    out_path = out_dir / f"task4_speedup_F_{f_value:.1f}.png"
    fig.savefig(out_path, dpi=180)
    plt.close(fig)
    return out_path


def plot_h_metric(
    rows: list[dict[str, Any]], h: int, metric: str, ylabel: str, title: str,
    filename: str, out_dir: Path,
) -> Path:
    selected = [row for row in rows if int(row["N"]) == h]
    locks = [lock for lock in LOCK_ORDER if any(row["lock"] == lock for row in selected)]
    x_positions = list(range(len(locks)))
    width = 0.36
    fig, ax = plt.subplots(figsize=(11, 6.2))

    for offset, f_value in enumerate((0.5, 0.1)):
        values = []
        for lock in locks:
            match = next((row for row in selected if row["lock"] == lock and row["F"] == f_value), None)
            values.append(match[metric] if match is not None else math.nan)
        xs = [x + (offset - 0.5) * width for x in x_positions]
        bars = ax.bar(xs, values, width=width, label=f"F={f_value:.1f}")
        for bar, value in zip(bars, values):
            if math.isfinite(value):
                ax.annotate(f"{value:.2f}" if metric != "context_switches_median" else f"{value:.0f}",
                            (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                            xytext=(0, 3), textcoords="offset points",
                            ha="center", va="bottom", fontsize=8)

    ax.set_xticks(x_positions, locks, rotation=12)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(True, axis="y", alpha=0.28)
    ax.legend()
    fig.tight_layout()
    out_path = out_dir / filename
    fig.savefig(out_path, dpi=180)
    plt.close(fig)
    return out_path


def print_h_table(rows: list[dict[str, Any]], h: int) -> None:
    print(f"\nTask 4 required table at N=H={h}")
    print("Lock               F      T_parallel(s)   S(N)    e(N)       task-clock(ms)  context-switches  OK/3")
    print("-" * 105)
    h_rows = [row for row in rows if int(row["N"]) == h]
    for f_value in (0.5, 0.1):
        for lock in LOCK_ORDER:
            row = next((item for item in h_rows if item["F"] == f_value and item["lock"] == lock), None)
            if row is None:
                continue
            ok_text = f"{row['successful_runs']}/{row['runs']}"
            print(f"{lock:<18} {f_value:.1f}   {fmt(row['T_parallel_median_s'], 3):>12}   "
                  f"{fmt(row['speedup_S'], 3):>5}   {fmt(row['karp_flatt_e'], 4):>8}   "
                  f"{fmt(row['task_clock_median_ms'], 2):>14}  "
                  f"{fmt(row['context_switches_median'], 0):>16}  {ok_text:>5}")


def print_winners_and_bound_checks(rows: list[dict[str, Any]], h: int) -> None:
    for f_value in (0.5, 0.1):
        subset = [row for row in rows if row["F"] == f_value]
        print(f"\nF={f_value:.1f}:")
        for n in (h, 2 * h):
            candidates = [row for row in subset if int(row["N"]) == n and math.isfinite(row["speedup_S"])]
            if candidates:
                best = max(candidates, key=lambda row: row["speedup_S"])
                print(f"  Best measured speedup at N={n}: {best['lock']} (S={best['speedup_S']:.4f})")
            else:
                print(f"  No completed/correct run available at N={n} for a winner comparison.")

        for lock in LOCK_ORDER:
            below = [int(row["N"]) for row in subset
                     if row["lock"] == lock and row["below_amdahl_bound"] is True]
            if below:
                print(f"  {lock}: measured below Amdahl bound at N={', '.join(map(str, below))}.")
            else:
                points = [row for row in subset if row["lock"] == lock and math.isfinite(row["speedup_S"])]
                if points:
                    print(f"  {lock}: no completed point was below the plotted Amdahl bound.")
                else:
                    print(f"  {lock}: no completed points to compare with Amdahl bound.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Create Task 4 Amdahl plots and metrics tables.")
    parser.add_argument("--results-dir", type=Path, default=Path("results"))
    parser.add_argument("--plots-dir", type=Path, default=Path("plots"))
    parser.add_argument("--h", type=int, default=None,
                        help="Guest-visible hardware thread count H. Defaults to max(N)/2 from the raw data.")
    args = parser.parse_args()

    args.plots_dir.mkdir(parents=True, exist_ok=True)
    raw_rows = read_csv(args.results_dir / "task4_sweep_raw.csv")
    serial_rows = read_csv(args.results_dir / "task4_serial_raw.csv")

    observed_ns = sorted({int(number(row, "N")) for row in raw_rows if math.isfinite(number(row, "N"))})
    if not observed_ns:
        raise SystemExit("No valid N values found in task4_sweep_raw.csv.")
    h = args.h if args.h is not None else max(observed_ns) // 2
    if h < 1 or 2 * h not in observed_ns:
        raise SystemExit(f"Cannot infer/validate H={h}; raw data must include N=2H.")

    serial = serial_baselines(serial_rows)
    derived = derive_rows(raw_rows, serial, h)
    if not derived:
        raise SystemExit("No valid Task 4 summary groups found.")

    results_dir = args.results_dir
    results_dir.mkdir(parents=True, exist_ok=True)
    summary_path = results_dir / "task4_summary.csv"
    at_h_path = results_dir / "task4_at_H.csv"
    write_table(summary_path, derived)
    at_h_rows = [row for row in derived if int(row["N"]) == h]
    write_table(at_h_path, at_h_rows)

    print(f"Guest-visible H = {h}; measured thread counts = {observed_ns}")
    print(f"Serial medians: F=0.5 -> {serial[0.5]:.6f}s, F=0.1 -> {serial[0.1]:.6f}s")
    print_h_table(derived, h)

    generated = [
        plot_speedup(derived, h, 0.5, args.plots_dir),
        plot_speedup(derived, h, 0.1, args.plots_dir),
        plot_h_metric(derived, h, "T_parallel_median_s", "Wall-clock time (s)",
                      f"Task 4: wall-clock time at N=H={h}", "task4_wall_time_NH.png", args.plots_dir),
        plot_h_metric(derived, h, "task_clock_median_ms", "Summed thread CPU time / task-clock (ms)",
                      f"Task 4: task-clock at N=H={h}", "task4_task_clock_NH.png", args.plots_dir),
        plot_h_metric(derived, h, "context_switches_median", "Summed context switches", 
                      f"Task 4: context switches at N=H={h}", "task4_context_switches_NH.png", args.plots_dir),
    ]
    print_winners_and_bound_checks(derived, h)

    print("\nGenerated files:")
    print(f"  {summary_path}")
    print(f"  {at_h_path}")
    for path in generated:
        print(f"  {path}")
    print("\nNote: timeout/incorrect runs are retained in raw CSV but excluded from timing medians and speedups.")


if __name__ == "__main__":
    main()
