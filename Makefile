CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
         -Wcast-align -Wconversion -Wsign-conversion -Wnull-dereference \
         -D_XOPEN_SOURCE=700 -pthread
LDLIBS = -pthread -lm

# Separate executable and source for each homework task.
TASK1_PROGRAM = build/task1
TASK1_SOURCE = src/task1.c
TASK2_PROGRAM = build/task2
TASK2_SOURCE = src/task2.c
TASK3_PROGRAM = build/task3
TASK3_NOPAUSE_PROGRAM = build/task3_nopause
TASK3_SOURCE = src/task3.c
TASK4_PROGRAM = build/task4
TASK4_SOURCE = src/task4.c
TASK4_PLOT_SCRIPT = scripts/plot_task4.py
TASK5_PROGRAM = build/task5
TASK5_SOURCE = src/task5.c
TASK5_PLOT_SCRIPT = src/plot_task5.py

# Every benchmark uses this same lock. This prevents overlapping experiments,
# including when the user invokes make with -j.
RUN_LOCK = build/.hw1-experiment.lock

.PHONY: all task1-build task2-build task3-build task3-nopause-build task4-build task5-build \
        task1 task1-perf task1-all task1-plots \
        task2 run task2-plots task2-all \
        task3 task3-ttas-no-pause task3-plots task3-all \
        task4 task4-plots task4-all \
        task5 task5-perf task5-plots task5-all task5-clean \
        plots env assembly-check clean check-task3-inputs check-task4-inputs

# Default action: compile the task binaries only; do not run benchmarks.
all: task1-build task2-build task3-build task3-nopause-build task4-build task5-build

task1-build: $(TASK1_PROGRAM)
task2-build: $(TASK2_PROGRAM)
task3-build: $(TASK3_PROGRAM)
task3-nopause-build: $(TASK3_NOPAUSE_PROGRAM)
task4-build: $(TASK4_PROGRAM)
task5-build: $(TASK5_PROGRAM)

$(TASK1_PROGRAM): $(TASK1_SOURCE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

$(TASK2_PROGRAM): $(TASK2_SOURCE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

# Task 3 main build: the assignment explicitly requires USE_PAUSE for all
# spin loops in the main lock comparison.
$(TASK3_PROGRAM): $(TASK3_SOURCE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DUSE_PAUSE $< -o $@ $(LDLIBS)

# A second binary without USE_PAUSE is used ONLY for the required TTAS
# comparison at F=0.1, N=H and N=2H.
$(TASK3_NOPAUSE_PROGRAM): $(TASK3_SOURCE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

# Task 4: Amdahl sweep. All spin locks use the required pause hint.
$(TASK4_PROGRAM): $(TASK4_SOURCE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DUSE_PAUSE $< -o $@ $(LDLIBS)

# Task 5: two cache microbenchmarks (lock hand-off and false sharing).
# The Task 5 source defines its own GNU/XOPEN feature macros and TTAS pause hint.
$(TASK5_PROGRAM): $(TASK5_SOURCE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

# ------------------------------- Task 1 -----------------------------------

task1: task1-build
	@mkdir -p build results
	flock $(RUN_LOCK) ./$(TASK1_PROGRAM) $(if $(ITERS),--iters $(ITERS),)

task1-perf: task1-build
	@mkdir -p build results
	flock $(RUN_LOCK) bash scripts/run_with_perf.sh ./$(TASK1_PROGRAM) $(if $(ITERS),--iters $(ITERS),)

task1-all: task1
	$(MAKE) task1-plots

task1-plots:
	python3 src/plot_task1.py

# ------------------------------- Task 2 -----------------------------------

task2: task2-build
	@mkdir -p build results
	flock $(RUN_LOCK) ./$(TASK2_PROGRAM) \
		$(if $(ITERS_PER_THREAD),--iterations-per-thread $(ITERS_PER_THREAD),) \
		$(if $(TASK2_TIMEOUT),--timeout-seconds $(TASK2_TIMEOUT),)

# Keep the prior alias used by the Task 2 Makefile.
run: task2

task2-plots:
	python3 src/plot_task2.py

task2-all: task2
	$(MAKE) task2-plots

# ------------------------------- Task 3 -----------------------------------

# Task 3 reads the verified F=0.1, C=64 W value and TOTAL_ITERS from Task 1.
check-task3-inputs:
	@test -s results/calibration.csv || { \
		echo "Missing results/calibration.csv. Run 'make task1' first."; exit 1; }
	@test -s results/serial_reference.csv || { \
		echo "Missing results/serial_reference.csv. Run 'make task1' first."; exit 1; }

task3: task3-build
	@mkdir -p build results
	flock $(RUN_LOCK) sh -c '\
		test -s results/calibration.csv || { echo "Missing results/calibration.csv. Run make task1 first."; exit 1; }; \
		test -s results/serial_reference.csv || { echo "Missing results/serial_reference.csv. Run make task1 first."; exit 1; }; \
		exec ./$(TASK3_PROGRAM)'

# The no-pause build runs only the additional TTAS comparison requested by
# the assignment. It does not repeat Task 3a or the fairness experiment.
task3-ttas-no-pause: task3-nopause-build
	@mkdir -p build results
	flock $(RUN_LOCK) sh -c '\
		test -s results/calibration.csv || { echo "Missing results/calibration.csv. Run make task1 first."; exit 1; }; \
		test -s results/serial_reference.csv || { echo "Missing results/serial_reference.csv. Run make task1 first."; exit 1; }; \
		exec ./$(TASK3_NOPAUSE_PROGRAM) --ttas-only'

task3-plots:
	python3 src/plot_task3.py

# Full Task 3 sequence: required USE_PAUSE experiments, then TTAS without
# pause, then plots. Recursive make calls keep the sequence explicit.
task3-all:
	$(MAKE) task3
	$(MAKE) task3-ttas-no-pause
	$(MAKE) task3-plots

# ------------------------------- Task 4 -----------------------------------

# Task 4 needs both verified calibration rows and their matching serial
# reference measurements from Task 1 (F=0.5 and F=0.1, C=64).
check-task4-inputs:
	@test -s results/calibration.csv || { \
		echo "Missing results/calibration.csv. Run 'make task1' first."; exit 1; }
	@test -s results/serial_reference.csv || { \
		echo "Missing results/serial_reference.csv. Run 'make task1' first."; exit 1; }

task4: task4-build check-task4-inputs
	@mkdir -p build results plots
	flock $(RUN_LOCK) ./$(TASK4_PROGRAM)

# Plot generation reads the Task 4 CSV files; it does not run benchmarks.
task4-plots:
	python3 src/plot_task4.py

# Full Task 4 sequence: run the complete Amdahl sweep, then generate plots
# and derived tables. The shared flock prevents overlap with other tasks.
task4-all:
	$(MAKE) task4
	$(MAKE) task4-plots

# ------------------------------- Task 5 -----------------------------------

# Run only the two Task 5 microbenchmarks. CPU topology is read in memory
# by task5.c for placement detection; no lscpu/governor/CPU-info files are written.
task5: task5-build
	@mkdir -p build results plots
	flock $(RUN_LOCK) ./$(TASK5_PROGRAM) results

# Optional perf measurements for Task 5. No CPU-information files are created.
task5-perf: task5-build
	@mkdir -p build results plots
	flock $(RUN_LOCK) perf stat -o results/task5_perf.txt \
		-e task-clock,context-switches,cache-misses -- ./$(TASK5_PROGRAM) results

# Generate Task 5 summary CSVs and charts from the raw measurements.
task5-plots:
	python3 $(TASK5_PLOT_SCRIPT) --input results --output plots

# Run both Task 5 experiments and then generate their plots.
task5-all:
	$(MAKE) task5
	$(MAKE) task5-plots

# Remove only Task 5 result CSVs, optional perf log and plots.
# Existing lscpu/governor/cache information files are not touched.
task5-clean:
	rm -f results/task5a_handoff.csv results/task5b_false_sharing.csv
	rm -f results/task5a_handoff_summary.csv results/task5b_false_sharing_summary.csv
	rm -f results/task5_perf.txt
	rm -f plots/task5a_handoff.png plots/task5b_false_sharing.png

# ----------------------------- All plots ---------------------------------

# Plot generation reads existing CSV files; it does not run benchmarks.
plots: task1-plots task2-plots task3-plots task4-plots task5-plots

env:
	bash scripts/collect_env.sh

# Preserve the Task 1 FNV loop assembly check.
assembly-check: task1-build
	@mkdir -p build
	$(CC) $(CFLAGS) -S $(TASK1_SOURCE) -o build/task1.s
	@grep -q '1099511628211' build/task1.s && echo "OK: the FNV hash multiplier is present in optimized Task 1 assembly."

clean:
	rm -rf build
	rm -f results/calibration.csv results/c16_sensitivity.csv
	rm -f results/serial_reference.csv results/nolock_raw.csv results/nolock_summary.csv
	rm -f results/perf_task1.log
	rm -f results/task2_peterson_raw.csv results/task2_peterson_summary.csv
	rm -f results/task3_serial_raw.csv results/task3_uncontended_raw.csv results/task3_uncontended_summary.csv
	rm -f results/task3_fairness_counts.csv results/task3_fairness_summary.csv
	rm -f results/task3_ttas_pause_raw.csv results/task3_ttas_nopause_raw.csv
	rm -f results/task4_sweep_raw.csv results/task4_serial_raw.csv
	rm -f results/task4_summary.csv results/task4_at_H.csv
	rm -f results/task5a_handoff.csv results/task5b_false_sharing.csv
	rm -f results/task5a_handoff_summary.csv results/task5b_false_sharing_summary.csv
	rm -f results/task5_perf.txt
	rm -f plots/task2_*.png plots/task3_*.png plots/task4_*.png plots/task5_*.png