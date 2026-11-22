CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
         -Wcast-align -Wconversion -Wsign-conversion -Wnull-dereference \
         -D_XOPEN_SOURCE=700 -pthread
LDLIBS = -pthread -lm

TASK1_PROGRAM = build/task1
TASK1_SOURCE = src/task1.c
TASK2_PROGRAM = build/task2
TASK2_SOURCE = src/task2.c

RUN_LOCK = build/.hw1-experiment.lock

.PHONY: all task1-build task2-build \
        task1 task1-perf task1-all task1-plots run \
        task2 task2-plots task2-all \
        env assembly-check plots clean

all: task1-build task2-build

task1-build: $(TASK1_PROGRAM)

task2-build: $(TASK2_PROGRAM)

$(TASK1_PROGRAM): $(TASK1_SOURCE)
	mkdir -p build
	$(CC) $(CFLAGS) $(TASK1_SOURCE) -o $(TASK1_PROGRAM) $(LDLIBS)

$(TASK2_PROGRAM): $(TASK2_SOURCE)
	mkdir -p build
	$(CC) $(CFLAGS) $(TASK2_SOURCE) -o $(TASK2_PROGRAM) $(LDLIBS)

task1: task1-build
	@mkdir -p build
	flock $(RUN_LOCK) ./$(TASK1_PROGRAM) $(if $(ITERS),--iters $(ITERS),)

task1-perf: task1-build
	@mkdir -p build
	flock $(RUN_LOCK) bash scripts/run_with_perf.sh ./$(TASK1_PROGRAM) $(if $(ITERS),--iters $(ITERS),)

task1-all: task1
	$(MAKE) task1-plots

task1-plots:
	python3 src/plot_task1.py

run task2: task2-build
	@mkdir -p build
	flock $(RUN_LOCK) ./$(TASK2_PROGRAM) \
		$(if $(ITERS_PER_THREAD),--iterations-per-thread $(ITERS_PER_THREAD),) \
		$(if $(TASK2_TIMEOUT),--timeout-seconds $(TASK2_TIMEOUT),)

task2-plots:
	python3 src/plot_task2.py

task2-all: task2
	$(MAKE) task2-plots

plots: task1-plots task2-plots

env:
	bash scripts/collect_env.sh

# Preserve the original Task 1 assembly check.
assembly-check: task1-build
	mkdir -p build
	$(CC) $(CFLAGS) -S $(TASK1_SOURCE) -o build/task1.s
	@grep -q '1099511628211' build/task1.s && echo "OK: the FNV hash multiplier is present in the optimized Task 1 assembly."

clean:
	rm -rf build
	rm -f results/calibration.csv results/c16_sensitivity.csv
	rm -f results/serial_reference.csv results/nolock_raw.csv results/nolock_summary.csv
	rm -f results/perf_task1.log
	rm -f results/task2_peterson_raw.csv results/task2_peterson_summary.csv
	rm -f plots/task2_*.png