#!/usr/bin/env bash
set -u

mkdir -p results
LOG_FILE='results/perf_task1.log'
TEST_LOG="$(mktemp)"
trap 'rm -f "$TEST_LOG"' EXIT

if ! command -v perf >/dev/null 2>&1; then
    printf 'perf is not installed; the benchmark will use its own wall-clock measurements only.\n' > "$LOG_FILE"
    printf 'perf is not installed; running Task 1 without perf counters.\n'
    "$@"
    exit $?
fi

FULL_EVENTS='task-clock,context-switches,cycles,instructions,cache-misses'
BASIC_EVENTS='task-clock,context-switches'

if perf stat -e "$FULL_EVENTS" -o "$TEST_LOG" -- true >/dev/null 2>&1; then
    SELECTED_EVENTS="$FULL_EVENTS"
elif perf stat -e "$BASIC_EVENTS" -o "$TEST_LOG" -- true >/dev/null 2>&1; then
    SELECTED_EVENTS="$BASIC_EVENTS"
else
    {
        printf 'perf counters are unavailable in this VM/system.\n'
        printf 'The benchmark was run with wall-clock timing from clock_gettime(CLOCK_MONOTONIC).\n'
    } > "$LOG_FILE"
    printf 'perf counters are unavailable; running Task 1 with wall-clock timing only.\n'
    "$@"
    exit $?
fi

printf 'Perf events recorded for the complete Task 1 process: %s\n' "$SELECTED_EVENTS"
perf stat -e "$SELECTED_EVENTS" -o "$LOG_FILE" -- "$@"
