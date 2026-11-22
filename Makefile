CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
         -Wcast-align -Wconversion -Wsign-conversion -Wnull-dereference \
         -D_XOPEN_SOURCE=700 -pthread
LDLIBS = -pthread -lm

PROGRAM = build/task1
SOURCE = src/task1.c

.PHONY: all task1 task1-perf env assembly-check plots clean

all: $(PROGRAM)

$(PROGRAM): $(SOURCE)
	mkdir -p build
	$(CC) $(CFLAGS) $(SOURCE) -o $(PROGRAM) $(LDLIBS)

task1: $(PROGRAM)
	./$(PROGRAM) $(if $(ITERS),--iters $(ITERS),)

task1-perf: $(PROGRAM)
	bash scripts/run_with_perf.sh ./$(PROGRAM) $(if $(ITERS),--iters $(ITERS),)

env:
	bash scripts/collect_env.sh

assembly-check:
	mkdir -p build
	$(CC) $(CFLAGS) -S $(SOURCE) -o build/task1.s
	@grep -q '1099511628211' build/task1.s && echo "OK: the FNV hash multiplier is present in the optimized assembly."

# Build the Task 1 plots from the CSV files in results/.
plots:
	python3 src/plot_task1.py

clean:
	rm -rf build
	rm -f results/calibration.csv results/c16_sensitivity.csv
	rm -f results/serial_reference.csv results/nolock_raw.csv results/nolock_summary.csv
	rm -f results/perf_task1.log
