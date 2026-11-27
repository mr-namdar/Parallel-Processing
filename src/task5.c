#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef CACHELINE_FALLBACK
#define CACHELINE_FALLBACK 64
#endif

#define HANDOFF_SECONDS 1.0
#define DEFAULT_RUNS 3
#define DEFAULT_ITERATIONS UINT64_C(20000000)
#define MAX_WORKERS 256
#define SPIN_HINT() __asm__ __volatile__("pause" ::: "memory")

typedef struct {
    int cpu;
    int package;
    int core;
    int node;
} cpu_info_t;

typedef struct {
    atomic_int state;
} ttas_lock_t;

static double now_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int read_int_file(const char *path, int fallback) {
    FILE *f = fopen(path, "r");
    if (!f) return fallback;
    int value = fallback;
    if (fscanf(f, "%d", &value) != 1) value = fallback;
    fclose(f);
    return value;
}

static int get_allowed_cpus(int cpus[], int max_cpus) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        if (n > max_cpus) n = max_cpus;
        for (int i = 0; i < n; ++i) cpus[i] = i;
        return n;
    }
    int n = 0;
    for (int i = 0; i < CPU_SETSIZE && n < max_cpus; ++i) {
        if (CPU_ISSET((size_t)i, &set)) cpus[n++] = i;
    }
    return n;
}

static int cpu_node(int cpu) {
    char path[256];
    for (int n = 0; n < 256; ++n) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/node%d", cpu, n);
        if (access(path, F_OK) == 0) return n;
    }
    return -1;
}

static void load_cpu_info(cpu_info_t *info, const int cpus[], int n) {
    for (int i = 0; i < n; ++i) {
        char path[256];
        int cpu = cpus[i];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
        info[i].package = read_int_file(path, -1);
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
        info[i].core = read_int_file(path, -1);
        info[i].node = cpu_node(cpu);
        info[i].cpu = cpu;
    }
}

static int cache_line_size(void) {
    FILE *p = popen("getconf LEVEL1_DCACHE_LINESIZE 2>/dev/null", "r");
    if (p) {
        int value = 0;
        if (fscanf(p, "%d", &value) == 1 && value >= (int)sizeof(uint64_t)) {
            pclose(p);
            return value;
        }
        pclose(p);
    }
    return CACHELINE_FALLBACK;
}

static bool find_pair(const cpu_info_t *info, int n, int kind, int *a, int *b) {
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            bool match = false;
            if (kind == 0) { /* SMT siblings */
                match = info[i].package >= 0 && info[i].core >= 0 &&
                        info[i].package == info[j].package && info[i].core == info[j].core;
            } else if (kind == 1) { /* different cores, same socket */
                match = info[i].package >= 0 && info[i].core >= 0 &&
                        info[i].package == info[j].package && info[i].core != info[j].core;
            } else if (kind == 2) { /* different NUMA nodes, fallback to different sockets */
                match = (info[i].node >= 0 && info[j].node >= 0 &&
                         info[i].node != info[j].node) ||
                        (info[i].package >= 0 && info[j].package >= 0 &&
                         info[i].package != info[j].package);
            }
            if (match) { *a = info[i].cpu; *b = info[j].cpu; return true; }
        }
    }
    return false;
}

static void ttas_lock(ttas_lock_t *lock) {
    for (;;) {
        while (atomic_load_explicit(&lock->state, memory_order_relaxed) != 0)
            SPIN_HINT();
        int expected = 0;
        if (atomic_compare_exchange_weak_explicit(&lock->state, &expected, 1,
                                                  memory_order_acquire,
                                                  memory_order_relaxed))
            return;
        SPIN_HINT();
    }
}

static void ttas_unlock(ttas_lock_t *lock) {
    atomic_store_explicit(&lock->state, 0, memory_order_release);
}

typedef struct {
    ttas_lock_t *lock;
    uint64_t *shared_counter;
    atomic_bool *go;
    atomic_bool *stop;
    int requested_cpu;
    int affinity_rc;
    uint64_t acquisitions;
    int first_cpu;
    int last_cpu;
    uint64_t migrations;
} handoff_arg_t;

static void *handoff_worker(void *opaque) {
    handoff_arg_t *arg = (handoff_arg_t *)opaque;
    if (arg->requested_cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((size_t)arg->requested_cpu, &set);
        arg->affinity_rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    } else {
        arg->affinity_rc = 0;
    }

    while (!atomic_load_explicit(arg->go, memory_order_acquire)) SPIN_HINT();
    int current = sched_getcpu();
    arg->first_cpu = current;
    arg->last_cpu = current;
    while (!atomic_load_explicit(arg->stop, memory_order_relaxed)) {
        ttas_lock(arg->lock);
        ++(*arg->shared_counter);
        ++arg->acquisitions;
        ttas_unlock(arg->lock);

        if ((arg->acquisitions & UINT64_C(1023)) == 0) {
            current = sched_getcpu();
            if (current >= 0 && arg->last_cpu >= 0 && current != arg->last_cpu)
                ++arg->migrations;
            arg->last_cpu = current;
        }
    }
    current = sched_getcpu();
    if (current >= 0 && arg->last_cpu >= 0 && current != arg->last_cpu)
        ++arg->migrations;
    arg->last_cpu = current;
    return NULL;
}

static void write_handoff_header(FILE *f) {
    fprintf(f, "placement,cpu0,cpu1,run,duration_s,elapsed_s,acq_thread0,acq_thread1,total_acq,ns_per_handoff,affinity_rc0,affinity_rc1,first_cpu0,last_cpu0,migrations0,first_cpu1,last_cpu1,migrations1,status\n");
}

static void run_handoff(FILE *out, const cpu_info_t *info, int allowed_n, int runs) {
    typedef struct { const char *name; int kind; } placement_t;
    const placement_t placements[] = {
        {"smt_siblings", 0},
        {"different_cores_same_socket", 1},
        {"different_numa_or_socket", 2},
        {"unpinned", 3}
    };

    for (size_t p = 0; p < sizeof(placements) / sizeof(placements[0]); ++p) {
        int cpu0 = -1, cpu1 = -1;
        bool supported = placements[p].kind == 3 ||
                         find_pair(info, allowed_n, placements[p].kind, &cpu0, &cpu1);
        for (int run = 1; run <= runs; ++run) {
            if (!supported) {
                fprintf(out, "%s,-1,-1,%d,%.3f,0,0,0,0,0,0,0,-1,-1,0,-1,-1,0,unsupported_topology\n",
                        placements[p].name, run, HANDOFF_SECONDS);
                continue;
            }

            ttas_lock_t lock;
            atomic_init(&lock.state, 0);
            uint64_t shared_counter = 0;
            handoff_arg_t args[2];
            memset(args, 0, sizeof(args));
            args[0].lock = &lock; args[1].lock = &lock;
            args[0].shared_counter = &shared_counter;
            args[1].shared_counter = &shared_counter;
            args[0].requested_cpu = placements[p].kind == 3 ? -1 : cpu0;
            args[1].requested_cpu = placements[p].kind == 3 ? -1 : cpu1;
            atomic_bool go = ATOMIC_VAR_INIT(false);
            atomic_bool stop = ATOMIC_VAR_INIT(false);
            args[0].go = &go; args[1].go = &go;
            args[0].stop = &stop; args[1].stop = &stop;

            pthread_t threads[2];
            int rc0 = pthread_create(&threads[0], NULL, handoff_worker, &args[0]);
            int rc1 = pthread_create(&threads[1], NULL, handoff_worker, &args[1]);
            if (rc0 != 0 || rc1 != 0) {
                atomic_store_explicit(&go, true, memory_order_release);
                atomic_store_explicit(&stop, true, memory_order_release);
                if (rc0 == 0) pthread_join(threads[0], NULL);
                if (rc1 == 0) pthread_join(threads[1], NULL);
                fprintf(out, "%s,%d,%d,%d,%.3f,0,0,0,0,0,-1,-1,-1,-1,0,-1,-1,0,thread_create_error\n",
                        placements[p].name, args[0].requested_cpu, args[1].requested_cpu,
                        run, HANDOFF_SECONDS);
                continue;
            }
            double begin = now_seconds();
            atomic_store_explicit(&go, true, memory_order_release);
            struct timespec pause_time = {1, 0};
            while (nanosleep(&pause_time, &pause_time) != 0 && errno == EINTR) { }
            atomic_store_explicit(&stop, true, memory_order_release);
            pthread_join(threads[0], NULL);
            pthread_join(threads[1], NULL);
            double elapsed = now_seconds() - begin;
            uint64_t total = args[0].acquisitions + args[1].acquisitions;
            double ns_per = total ? elapsed * 1e9 / (double)total : 0.0;
            const char *status = "ok";
            if (args[0].affinity_rc != 0 || args[1].affinity_rc != 0) status = "affinity_error";
            else if (placements[p].kind != 3 &&
                     (args[0].first_cpu != args[0].requested_cpu || args[0].last_cpu != args[0].requested_cpu ||
                      args[1].first_cpu != args[1].requested_cpu || args[1].last_cpu != args[1].requested_cpu))
                status = "pinning_not_observed";
            fprintf(out,
                    "%s,%d,%d,%d,%.3f,%.9f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f,%d,%d,%d,%d,%" PRIu64 ",%d,%d,%" PRIu64 ",%s\n",
                    placements[p].name, args[0].requested_cpu, args[1].requested_cpu, run,
                    HANDOFF_SECONDS, elapsed, args[0].acquisitions, args[1].acquisitions,
                    total, ns_per, args[0].affinity_rc, args[1].affinity_rc,
                    args[0].first_cpu, args[0].last_cpu, args[0].migrations,
                    args[1].first_cpu, args[1].last_cpu, args[1].migrations, status);
        }
    }
}

typedef struct {
    volatile uint64_t *slot;
    uint64_t iterations;
    uint64_t completed;
    atomic_int *gate;
} false_share_arg_t;

static void *false_share_worker(void *opaque) {
    false_share_arg_t *arg = (false_share_arg_t *)opaque;
    int gate;
    do { gate = atomic_load_explicit(arg->gate, memory_order_acquire); if (gate == 0) SPIN_HINT(); } while (gate == 0);
    if (gate == 2) return NULL;
    for (uint64_t i = 0; i < arg->iterations; ++i) {
        (*arg->slot)++;
    }
    arg->completed = arg->iterations;
    return NULL;
}

static uint64_t min_u64(const uint64_t *v, int n) {
    uint64_t m = UINT64_MAX;
    for (int i = 0; i < n; ++i) if (v[i] < m) m = v[i];
    return m;
}

static uint64_t max_u64(const uint64_t *v, int n) {
    uint64_t m = 0;
    for (int i = 0; i < n; ++i) if (v[i] > m) m = v[i];
    return m;
}

static void run_false_sharing(FILE *out, int h, int line_size, int runs, uint64_t iterations) {
    int ns[3] = {2, 4, h};
    for (int ni = 0; ni < 3; ++ni) {
        int n = ns[ni];
        bool duplicate = false;
        for (int k = 0; k < ni; ++k) if (ns[k] == n) duplicate = true;
        if (duplicate || n < 2 || n > MAX_WORKERS) continue;

        for (int layout = 0; layout < 2; ++layout) {
            const char *layout_name = layout == 0 ? "packed" : "padded";
            for (int run = 1; run <= runs; ++run) {
                size_t bytes = (size_t)n * sizeof(uint64_t);
                size_t stride = 1;
                if (layout == 1) {
                    stride = ((size_t)line_size + sizeof(uint64_t) - 1) / sizeof(uint64_t);
                    if (stride < 1) stride = 1;
                    bytes = (size_t)n * stride * sizeof(uint64_t);
                }
                volatile uint64_t *slots = NULL;
                void *memory = NULL;
                if (layout == 1) {
                    size_t alignment = (size_t)line_size;
                    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
                        alignment = CACHELINE_FALLBACK;
                    if (posix_memalign(&memory, alignment, bytes) != 0) memory = NULL;
                    slots = (volatile uint64_t *)memory;
                } else {
                    memory = calloc(1, bytes);
                    slots = (volatile uint64_t *)memory;
                }
                if (!slots) {
                    fprintf(out, "%s,%d,%d,%d,%" PRIu64 ",0,0,0,0,0,allocation_error\n",
                            layout_name, n, run, line_size, iterations);
                    continue;
                }
                memset(memory, 0, bytes);
                false_share_arg_t *args = calloc((size_t)n, sizeof(*args));
                pthread_t *threads = calloc((size_t)n, sizeof(*threads));
                if (!args || !threads) {
                    fprintf(stderr, "allocation failed for false-sharing workers\n");
                    free((void *)slots); free(args); free(threads); continue;
                }
                atomic_int gate = ATOMIC_VAR_INIT(0);
                for (int t = 0; t < n; ++t) {
                    args[t].slot = slots + (size_t)t * stride;
                    args[t].iterations = iterations;
                    args[t].completed = 0;
                    args[t].gate = &gate;
                }
                int create_error = 0;
                int created = 0;
                for (int t = 0; t < n; ++t) {
                    int rc = pthread_create(&threads[t], NULL, false_share_worker, &args[t]);
                    if (rc != 0) { create_error = rc; break; }
                    ++created;
                }
                if (create_error != 0) {
                    fprintf(stderr, "pthread_create: %s\n", strerror(create_error));
                    atomic_store_explicit(&gate, 2, memory_order_release);
                    for (int t = 0; t < created; ++t) pthread_join(threads[t], NULL);
                    fprintf(out, "%s,%d,%d,%d,%" PRIu64 ",0,0,0,0,0,thread_create_error\n",
                            layout_name, n, run, line_size, iterations);
                } else {
                    double begin = now_seconds();
                    atomic_store_explicit(&gate, 1, memory_order_release);
                    for (int t = 0; t < n; ++t) pthread_join(threads[t], NULL);
                    double elapsed = now_seconds() - begin;
                    uint64_t *counts = calloc((size_t)n, sizeof(*counts));
                    uint64_t total = 0;
                    for (int t = 0; t < n; ++t) { counts[t] = args[t].completed; total += args[t].completed; }
                    uint64_t expected = (uint64_t)n * iterations;
                    const char *status = total == expected ? "ok" : "count_mismatch";
                    fprintf(out, "%s,%d,%d,%d,%" PRIu64 ",%.9f,%.3f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%s\n",
                            layout_name, n, run, line_size, iterations, elapsed,
                            elapsed * 1e9 / (double)expected, total,
                            min_u64(counts, n), max_u64(counts, n), status);
                    free(counts);
                }
                    free((void *)slots); free(args); free(threads);
            }
        }
    }
}

static int parse_int_env(const char *name, int fallback) {
    const char *s = getenv(name);
    if (!s || !*s) return fallback;
    char *end = NULL;
    long x = strtol(s, &end, 10);
    if (!end || *end || x < 1 || x > 1000) return fallback;
    return (int)x;
}

static uint64_t parse_u64_env(const char *name, uint64_t fallback) {
    const char *s = getenv(name);
    if (!s || !*s) return fallback;
    char *end = NULL;
    unsigned long long x = strtoull(s, &end, 10);
    if (!end || *end || x < 1) return fallback;
    return (uint64_t)x;
}

int main(int argc, char **argv) {
    const char *out_dir = argc > 1 ? argv[1] : "results";
    if (mkdir(out_dir, 0775) != 0 && errno != EEXIST) {
        perror("mkdir output directory"); return EXIT_FAILURE;
    }
    char path[512];
    int runs = parse_int_env("TASK5_RUNS", DEFAULT_RUNS);
    uint64_t iterations = parse_u64_env("TASK5_ITERATIONS", DEFAULT_ITERATIONS);
    int h = (int)sysconf(_SC_NPROCESSORS_ONLN);
    FILE *np = popen("nproc 2>/dev/null", "r");
    if (np) { int detected = 0; if (fscanf(np, "%d", &detected) == 1 && detected > 0) h = detected; pclose(np); }
    if (h < 1) h = 1;
    if (h > MAX_WORKERS) h = MAX_WORKERS;
    int line_size = cache_line_size();

    int allowed[MAX_WORKERS];
    int allowed_n = get_allowed_cpus(allowed, MAX_WORKERS);
    cpu_info_t *info = calloc((size_t)allowed_n, sizeof(*info));
    if (!info) { perror("calloc cpu info"); return EXIT_FAILURE; }
    load_cpu_info(info, allowed, allowed_n);

    snprintf(path, sizeof(path), "%s/task5a_handoff.csv", out_dir);
    FILE *a = fopen(path, "w");
    if (!a) { perror(path); free(info); return EXIT_FAILURE; }
    write_handoff_header(a);
    run_handoff(a, info, allowed_n, runs);
    fclose(a);

    snprintf(path, sizeof(path), "%s/task5b_false_sharing.csv", out_dir);
    FILE *b = fopen(path, "w");
    if (!b) { perror(path); free(info); return EXIT_FAILURE; }
    fprintf(b, "layout,N,run,cache_line_bytes,iterations_per_thread,elapsed_s,ns_per_increment,total_increments,min_thread_increments,max_thread_increments,status\n");
    run_false_sharing(b, h, line_size, runs, iterations);
    fclose(b);

    printf("Task 5 finished. H=%d, allowed CPUs=%d, cache line=%d bytes, runs=%d, iterations/thread=%" PRIu64 "\n",
           h, allowed_n, line_size, runs, iterations);
    printf("CSV files: %s/task5a_handoff.csv and %s/task5b_false_sharing.csv\n", out_dir, out_dir);
    free(info);
    return EXIT_SUCCESS;
}
