#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(USE_PAUSE) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define SPIN_HINT() _mm_pause()
#elif defined(USE_PAUSE) && (defined(__aarch64__) || defined(__arm__))
#define SPIN_HINT() __asm__ __volatile__("yield")
#else
#define SPIN_HINT() ((void)0)
#endif

#define NBUCKETS 64
#define CRITICAL_SIZE 64
#define DEFAULT_REPETITIONS 3
#define FAIRNESS_SECONDS 1.0
#define RUN_TIMEOUT_SECONDS 180.0
#define MAX_THREADS 256
#define POLL_NS 1000000L
#define DEFAULT_CSV_CONFIG "results/calibration.csv"
#define DEFAULT_SERIAL_CONFIG "results/serial_reference.csv"

/* Keep the exact shared-table layout from the homework; do not pad it. */
typedef struct {
    uint64_t hist[NBUCKETS];
    uint64_t total;
    uint64_t max_score;
} shared_t;

typedef enum {
    LOCK_TAS = 0,
    LOCK_TTAS,
    LOCK_TTAS_BACKOFF,
    LOCK_TICKET,
    LOCK_MUTEX,
    LOCK_COUNT
} lock_kind_t;

typedef struct { atomic_flag flag; } tas_lock_t;
typedef struct { atomic_int state; } ttas_lock_t;
typedef struct { atomic_uint next_ticket; atomic_uint now_serving; } ticket_lock_t;

typedef struct {
    pthread_mutex_t mutex;
    union {
        tas_lock_t tas;
        ttas_lock_t ttas;
        ttas_lock_t backoff;
        ticket_lock_t ticket;
    } u;
} lock_t;

typedef struct {
    uint64_t task_clock_ns;
    uint64_t context_switches;
    bool task_clock_ok;
    bool context_switches_ok;
} event_values_t;

typedef struct {
    struct timespec cpu_start;
    struct rusage usage_start;
    bool cpu_ok;
    bool usage_ok;
} event_start_t;

typedef struct {
    lock_t *lock;
    lock_kind_t kind;
    shared_t *shared;
    pthread_barrier_t *barrier;
    atomic_bool *stop;
    uint64_t total_iters;
    int nthreads;
    int tid;
    int w;
    int c;
    bool fairness_mode;
    bool timed_out;
    uint64_t processed;
    uint64_t acquisitions;
    event_values_t events;
} worker_arg_t;

typedef struct {
    double elapsed_s;
    bool timeout;
    uint64_t actual_total;
    bool correct;
    uint64_t task_clock_ns;
    uint64_t context_switches;
    bool task_clock_ok;
    bool context_switches_ok;
} bench_result_t;

static volatile uint64_t result_sink;

static const char *lock_name(lock_kind_t kind)
{
    static const char *names[LOCK_COUNT] = {
        "TAS", "TTAS", "TTAS_backoff", "Ticket", "pthread_mutex"
    };
    return (kind >= 0 && kind < LOCK_COUNT) ? names[kind] : "unknown";
}

static double now_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint64_t timespec_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static void event_begin(event_start_t *s)
{
    memset(s, 0, sizeof(*s));
    s->cpu_ok = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &s->cpu_start) == 0;
#ifdef RUSAGE_THREAD
    s->usage_ok = getrusage(RUSAGE_THREAD, &s->usage_start) == 0;
#endif
}

static event_values_t event_end(const event_start_t *s)
{
    event_values_t e = {0};
    struct timespec cpu_end;
    struct rusage usage_end;
    e.task_clock_ok = s->cpu_ok && clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end) == 0;
    if (e.task_clock_ok) {
        const uint64_t a = timespec_ns(&s->cpu_start);
        const uint64_t b = timespec_ns(&cpu_end);
        if (b >= a) e.task_clock_ns = b - a;
        else e.task_clock_ok = false;
    }
#ifdef RUSAGE_THREAD
    e.context_switches_ok = s->usage_ok && getrusage(RUSAGE_THREAD, &usage_end) == 0;
    if (e.context_switches_ok) {
        const long nv = usage_end.ru_nvcsw - s->usage_start.ru_nvcsw;
        const long ni = usage_end.ru_nivcsw - s->usage_start.ru_nivcsw;
        if (nv >= 0 && ni >= 0) e.context_switches = (uint64_t)nv + (uint64_t)ni;
        else e.context_switches_ok = false;
    }
#endif
    return e;
}

static inline uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* The two work functions below intentionally match the assignment code. */
static uint64_t private_work(uint64_t i, int w)
{
    uint64_t rec[8];
    uint64_t seed = i;
    const uint8_t *p;
    uint64_t h = 1469598103934665603ULL;
    for (int j = 0; j < 8; ++j) rec[j] = splitmix64(&seed);
    p = (const uint8_t *)rec;
    for (int r = 0; r < w; ++r) {
        for (int j = 0; j < 64; ++j) {
            h ^= p[j];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

static void shared_work(shared_t *s, uint64_t h, int c)
{
    uint64_t x = h;
    for (int k = 0; k < c; ++k) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        s->hist[x >> 58]++;
    }
    s->total++;
    if (h > s->max_score) s->max_score = h;
}

static bool shared_equal(const shared_t *a, const shared_t *b)
{
    if (a->total != b->total || a->max_score != b->max_score) return false;
    for (size_t i = 0; i < NBUCKETS; ++i) if (a->hist[i] != b->hist[i]) return false;
    return true;
}

static void lock_init(lock_t *l, lock_kind_t kind)
{
    memset(l, 0, sizeof(*l));
    int rc = 0;
    switch (kind) {
    case LOCK_TAS:
        l->u.tas.flag = (atomic_flag)ATOMIC_FLAG_INIT;
        break;
    case LOCK_TTAS:
        atomic_init(&l->u.ttas.state, 0);
        break;
    case LOCK_TTAS_BACKOFF:
        atomic_init(&l->u.backoff.state, 0);
        break;
    case LOCK_TICKET:
        atomic_init(&l->u.ticket.next_ticket, 0U);
        atomic_init(&l->u.ticket.now_serving, 0U);
        break;
    case LOCK_MUTEX:
        rc = pthread_mutex_init(&l->mutex, NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_mutex_init: %s\n", strerror(rc));
            exit(EXIT_FAILURE);
        }
        break;
    default:
        exit(EXIT_FAILURE);
    }
}

static void lock_destroy(lock_t *l, lock_kind_t kind)
{
    if (kind == LOCK_MUTEX) {
        const int rc = pthread_mutex_destroy(&l->mutex);
        if (rc != 0) fprintf(stderr, "pthread_mutex_destroy: %s\n", strerror(rc));
    }
}

static inline bool lock_acquire(lock_t *l, lock_kind_t kind, double deadline)
{
    unsigned int spins = 0U;
    switch (kind) {
    case LOCK_TAS:
        while (atomic_flag_test_and_set_explicit(&l->u.tas.flag, memory_order_acquire)) {
            SPIN_HINT();
            if (deadline > 0.0 && (++spins & 4095U) == 0U && now_seconds() >= deadline)
                return false;
        }
        return true;
    case LOCK_TTAS:
        for (;;) {
            while (atomic_load_explicit(&l->u.ttas.state, memory_order_relaxed) != 0) {
                SPIN_HINT();
                if (deadline > 0.0 && (++spins & 4095U) == 0U && now_seconds() >= deadline)
                    return false;
            }
            int expected = 0;
            if (atomic_compare_exchange_weak_explicit(&l->u.ttas.state, &expected, 1,
                                                      memory_order_acquire,
                                                      memory_order_relaxed)) return true;
            SPIN_HINT();
            if (deadline > 0.0 && (++spins & 4095U) == 0U && now_seconds() >= deadline)
                return false;
        }
    case LOCK_TTAS_BACKOFF: {
        unsigned int delay = 16U;
        for (;;) {
            while (atomic_load_explicit(&l->u.backoff.state, memory_order_relaxed) != 0) {
                for (unsigned int i = 0; i < delay; ++i) {
                    SPIN_HINT();
                    if (deadline > 0.0 && (++spins & 4095U) == 0U && now_seconds() >= deadline)
                        return false;
                }
                if (delay < 1024U) delay = (delay * 2U > 1024U) ? 1024U : delay * 2U;
            }
            int expected = 0;
            if (atomic_compare_exchange_weak_explicit(&l->u.backoff.state, &expected, 1,
                                                      memory_order_acquire,
                                                      memory_order_relaxed)) return true;
            for (unsigned int i = 0; i < delay; ++i) {
                SPIN_HINT();
                if (deadline > 0.0 && (++spins & 4095U) == 0U && now_seconds() >= deadline)
                    return false;
            }
            if (delay < 1024U) delay = (delay * 2U > 1024U) ? 1024U : delay * 2U;
        }
    }
    case LOCK_TICKET: {
        const unsigned int mine = atomic_fetch_add_explicit(&l->u.ticket.next_ticket, 1U,
                                                             memory_order_relaxed);
        bool expired_while_waiting = false;
        while (atomic_load_explicit(&l->u.ticket.now_serving, memory_order_acquire) != mine) {
            SPIN_HINT();
            /* A ticket cannot be abandoned out of order: that would block every later ticket.
               Once its turn arrives, an expired waiter advances the queue and exits. */
            if (!expired_while_waiting && deadline > 0.0 &&
                (++spins & 4095U) == 0U && now_seconds() >= deadline)
                expired_while_waiting = true;
        }
        if (expired_while_waiting) {
            (void)atomic_fetch_add_explicit(&l->u.ticket.now_serving, 1U, memory_order_release);
            return false;
        }
        return true;
    }
    case LOCK_MUTEX: {
        const int rc = pthread_mutex_lock(&l->mutex);
        if (rc != 0) {
            fprintf(stderr, "pthread_mutex_lock: %s\\n", strerror(rc));
            abort();
        }
        return true;
    }
    default:
        abort();
    }
}

static inline void lock_release(lock_t *l, lock_kind_t kind)
{
    switch (kind) {
    case LOCK_TAS:
        atomic_flag_clear_explicit(&l->u.tas.flag, memory_order_release);
        break;
    case LOCK_TTAS:
        atomic_store_explicit(&l->u.ttas.state, 0, memory_order_release);
        break;
    case LOCK_TTAS_BACKOFF:
        atomic_store_explicit(&l->u.backoff.state, 0, memory_order_release);
        break;
    case LOCK_TICKET:
        (void)atomic_fetch_add_explicit(&l->u.ticket.now_serving, 1U, memory_order_release);
        break;
    case LOCK_MUTEX: {
        const int rc = pthread_mutex_unlock(&l->mutex);
        if (rc != 0) {
            fprintf(stderr, "pthread_mutex_unlock: %s\n", strerror(rc));
            abort();
        }
        break;
    }
    default:
        abort();
    }
}

static void *worker_main(void *opaque)
{
    worker_arg_t *a = (worker_arg_t *)opaque;
    const int br = pthread_barrier_wait(a->barrier);
    if (br != 0 && br != PTHREAD_BARRIER_SERIAL_THREAD) return NULL;

    event_start_t ev;
    event_begin(&ev);
    a->timed_out = false;
    a->processed = 0;
    a->acquisitions = 0;

    if (a->fairness_mode) {
        while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
            (void)lock_acquire(a->lock, a->kind, 0.0);
            shared_work(a->shared, UINT64_C(1469598103934665603), a->c);
            lock_release(a->lock, a->kind);
            a->acquisitions++;
        }
    } else {
        const double deadline = now_seconds() + RUN_TIMEOUT_SECONDS;
        for (uint64_t i = (uint64_t)a->tid; i < a->total_iters; i += (uint64_t)a->nthreads) {
            if ((a->processed & 4095ULL) == 0ULL && now_seconds() > deadline) {
                a->timed_out = true;
                atomic_store_explicit(a->stop, true, memory_order_relaxed);
                break;
            }
            if (atomic_load_explicit(a->stop, memory_order_relaxed)) break;
            const uint64_t h = private_work(i, a->w);
            if (!lock_acquire(a->lock, a->kind, deadline)) {
                a->timed_out = true;
                atomic_store_explicit(a->stop, true, memory_order_relaxed);
                break;
            }
            shared_work(a->shared, h, a->c);
            lock_release(a->lock, a->kind);
            a->processed++;
        }
    }
    a->events = event_end(&ev);
    return NULL;
}

static void aggregate_events(const worker_arg_t *args, int n, bench_result_t *r)
{
    r->task_clock_ok = true;
    r->context_switches_ok = true;
    for (int i = 0; i < n; ++i) {
        if (args[i].events.task_clock_ok) r->task_clock_ns += args[i].events.task_clock_ns;
        else r->task_clock_ok = false;
        if (args[i].events.context_switches_ok) r->context_switches += args[i].events.context_switches;
        else r->context_switches_ok = false;
        if (args[i].timed_out) r->timeout = true;
    }
}

static bench_result_t run_parallel(lock_kind_t kind, int nthreads, int w, int c,
                                   uint64_t total_iters, const shared_t *expected)
{
    bench_result_t result = {0};
    pthread_t *threads = calloc((size_t)nthreads, sizeof(*threads));
    worker_arg_t *args = calloc((size_t)nthreads, sizeof(*args));
    if (threads == NULL || args == NULL) {
        perror("calloc"); exit(EXIT_FAILURE);
    }
    shared_t shared = {0};
    lock_t lock;
    lock_init(&lock, kind);
    pthread_barrier_t barrier;
    atomic_bool stop;
    atomic_init(&stop, false);
    int rc = pthread_barrier_init(&barrier, NULL, (unsigned int)(nthreads + 1));
    if (rc != 0) {
        fprintf(stderr, "pthread_barrier_init: %s\n", strerror(rc)); exit(EXIT_FAILURE);
    }
    for (int t = 0; t < nthreads; ++t) {
        args[t] = (worker_arg_t){
            .lock = &lock, .kind = kind, .shared = &shared, .barrier = &barrier,
            .stop = &stop, .total_iters = total_iters, .nthreads = nthreads,
            .tid = t, .w = w, .c = c, .fairness_mode = false
        };
        rc = pthread_create(&threads[t], NULL, worker_main, &args[t]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc)); exit(EXIT_FAILURE);
        }
    }
    rc = pthread_barrier_wait(&barrier);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        fprintf(stderr, "pthread_barrier_wait failed\n"); exit(EXIT_FAILURE);
    }
    const double start = now_seconds();
    for (int t = 0; t < nthreads; ++t) {
        rc = pthread_join(threads[t], NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_join: %s\n", strerror(rc)); exit(EXIT_FAILURE);
        }
    }
    result.elapsed_s = now_seconds() - start;
    aggregate_events(args, nthreads, &result);
    result.actual_total = shared.total;
    result.correct = !result.timeout && expected != NULL && shared_equal(&shared, expected);
    (void)pthread_barrier_destroy(&barrier);
    lock_destroy(&lock, kind);
    free(threads);
    free(args);
    return result;
}

static bool serial_reference(int w, int c, uint64_t iters, shared_t *answer,
                             double *elapsed, event_values_t *events)
{
    shared_t s = {0};
    event_start_t ev;
    event_begin(&ev);
    const double start = now_seconds();
    const double deadline = start + RUN_TIMEOUT_SECONDS;
    bool timed_out = false;
    for (uint64_t i = 0; i < iters; ++i) {
        if ((i & 4095ULL) == 0ULL && now_seconds() >= deadline) {
            timed_out = true;
            break;
        }
        const uint64_t h = private_work(i, w);
        shared_work(&s, h, c);
    }
    *elapsed = now_seconds() - start;
    *events = event_end(&ev);
    *answer = s;
    return !timed_out && s.total == iters;
}

static int read_task1_configuration(int *w_out, double *measured_f_out, uint64_t *iters_out)
{
    FILE *f = fopen(DEFAULT_CSV_CONFIG, "r");
    if (f == NULL) {
        fprintf(stderr, "Cannot open %s. Run `make task1` first to generate calibration.csv.\n",
                DEFAULT_CSV_CONFIG);
        return -1;
    }
    char line[512];
    (void)fgets(line, sizeof(line), f); /* header */
    bool found_w = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        char copy[512];
        (void)snprintf(copy, sizeof(copy), "%s", line);
        char *fields[9] = {0};
        char *save = NULL;
        char *token = strtok_r(copy, ",", &save);
        for (size_t column = 0; column < 9 && token != NULL; ++column) {
            fields[column] = token;
            token = strtok_r(NULL, ",", &save);
        }
        if (fields[0] != NULL && fields[1] != NULL && fields[2] != NULL &&
            fields[6] != NULL && fields[8] != NULL) {
            const double target = strtod(fields[0], NULL);
            const long c = strtol(fields[1], NULL, 10);
            const long w = strtol(fields[2], NULL, 10);
            const double measured = strtod(fields[6], NULL);
            const long within = strtol(fields[8], NULL, 10);
            if (fabs(target - 0.1) < 1e-6 && c == CRITICAL_SIZE && w > 0 &&
                within == 1 && fabs(measured - target) <= 0.05) {
                *w_out = (int)w;
                *measured_f_out = measured;
                found_w = true;
                break;
            }
        }
    }
    fclose(f);
    if (!found_w) {
        fprintf(stderr, "No verified F=0.1, C=64 calibration row (within +/-0.05) found in %s.\n",
                DEFAULT_CSV_CONFIG);
        return -1;
    }

    f = fopen(DEFAULT_SERIAL_CONFIG, "r");
    if (f == NULL) {
        fprintf(stderr, "Cannot open %s. Run `make task1` first to generate serial_reference.csv.\n",
                DEFAULT_SERIAL_CONFIG);
        return -1;
    }
    (void)fgets(line, sizeof(line), f); /* header */
    bool found_iters = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        char copy[512];
        (void)snprintf(copy, sizeof(copy), "%s", line);
        char *fields[9] = {0};
        char *save = NULL;
        char *token = strtok_r(copy, ",", &save);
        for (size_t column = 0; column < 9 && token != NULL; ++column) {
            fields[column] = token;
            token = strtok_r(NULL, ",", &save);
        }
        /* Status is column 9 in both the basic and fallback-enabled Task 1 CSVs. */
        if (fields[0] != NULL && fields[1] != NULL && fields[5] != NULL && fields[8] != NULL) {
            const double target = strtod(fields[0], NULL);
            const long c = strtol(fields[1], NULL, 10);
            const unsigned long long expected = strtoull(fields[5], NULL, 10);
            fields[8][strcspn(fields[8], "\r\n")] = '\0';
            if (fabs(target - 0.1) < 1e-6 && c == CRITICAL_SIZE &&
                expected > 0ULL && strcmp(fields[8], "ok") == 0) {
                *iters_out = (uint64_t)expected;
                found_iters = true;
                break;
            }
        }
    }
    fclose(f);
    if (!found_iters) {
        fprintf(stderr, "No F=0.1, C=64 serial reference row found in %s.\n",
                DEFAULT_SERIAL_CONFIG);
        return -1;
    }
    return 0;
}

static int available_hardware_threads(void)
{
    FILE *p = popen("nproc", "r");
    if (p != NULL) {
        char b[64];
        if (fgets(b, sizeof(b), p) != NULL) {
            char *end = NULL;
            long v = strtol(b, &end, 10);
            (void)pclose(p);
            if (end != b && v > 0 && v <= MAX_THREADS / 2) return (int)v;
        } else {
            (void)pclose(p);
        }
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int count = CPU_COUNT(&set);
        if (count > 0 && count <= MAX_THREADS / 2) return count;
    }
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1 || count > MAX_THREADS / 2) return 1;
    return (int)count;
}

static void ensure_results_dir(void)
{
    if (mkdir("results", 0775) != 0 && errno != EEXIST) {
        perror("mkdir results"); exit(EXIT_FAILURE);
    }
}

static void run_uncontended(int w, int c, uint64_t iters)
{
    static const lock_kind_t kinds[LOCK_COUNT] = {
        LOCK_TAS, LOCK_TTAS, LOCK_TTAS_BACKOFF, LOCK_TICKET, LOCK_MUTEX
    };
    double serial_times[DEFAULT_REPETITIONS] = {0};
    shared_t expected = {0};
    event_values_t serial_events[DEFAULT_REPETITIONS];
    FILE *serial_csv = fopen("results/task3_serial_raw.csv", "w");
    FILE *raw = fopen("results/task3_uncontended_raw.csv", "w");
    FILE *summary = fopen("results/task3_uncontended_summary.csv", "w");
    if (!serial_csv || !raw || !summary) { perror("open Task 3 CSV"); exit(EXIT_FAILURE); }
    fprintf(serial_csv, "run,F,W,C,total_iters,time_s,task_clock_ns,context_switches,ok\n");
    fprintf(raw, "lock,run,N,F,W,C,total_iters,time_s,task_clock_ns,context_switches,actual_total,ok,status\n");
    fprintf(summary, "lock,F,W,C,N,total_iters,serial_median_s,locked_median_s,overhead_ns_per_lock_pair,median_task_clock_ms,median_context_switches,ok_runs,total_runs\n");

    printf("\nTask 3a: uncontended cost (F=0.1, C=%d, W=%d, TOTAL_ITERS=%" PRIu64 ")\n",
           c, w, iters);
    bool all_serial_ok = true;
    for (int r = 0; r < DEFAULT_REPETITIONS; ++r) {
        const bool ok = serial_reference(w, c, iters, &expected,
                                         &serial_times[r], &serial_events[r]);
        if (!ok) all_serial_ok = false;
        fprintf(serial_csv, "%d,0.1,%d,%d,%" PRIu64 ",%.9f,", r + 1, w, c,
                iters, serial_times[r]);
        if (serial_events[r].task_clock_ok) fprintf(serial_csv, "%" PRIu64, serial_events[r].task_clock_ns);
        fprintf(serial_csv, ",");
        if (serial_events[r].context_switches_ok) fprintf(serial_csv, "%" PRIu64, serial_events[r].context_switches);
        fprintf(serial_csv, ",%d\n", ok ? 1 : 0);
    }
    if (!all_serial_ok) {
        fclose(serial_csv); fclose(raw); fclose(summary);
        fprintf(stderr, "Task 3 serial reference timed out or was incomplete; no lock-cost comparison was run.\n");
        exit(EXIT_FAILURE);
    }
    double serial_sorted[DEFAULT_REPETITIONS];
    memcpy(serial_sorted, serial_times, sizeof(serial_sorted));
    if (serial_sorted[0] > serial_sorted[1]) { double t=serial_sorted[0]; serial_sorted[0]=serial_sorted[1]; serial_sorted[1]=t; }
    if (serial_sorted[1] > serial_sorted[2]) { double t=serial_sorted[1]; serial_sorted[1]=serial_sorted[2]; serial_sorted[2]=t; }
    if (serial_sorted[0] > serial_sorted[1]) { double t=serial_sorted[0]; serial_sorted[0]=serial_sorted[1]; serial_sorted[1]=t; }
    const double serial_median = serial_sorted[1];
    if (serial_median < 2.0) {
        fclose(serial_csv); fclose(raw); fclose(summary);
        fprintf(stderr, "ERROR: F=0.1 serial median is %.3f s (<2s). Re-run Task 1 with a larger --iters before Task 3.\n",
                serial_median);
        exit(EXIT_FAILURE);
    }

    for (int k = 0; k < LOCK_COUNT; ++k) {
        double lock_times[DEFAULT_REPETITIONS] = {0};
        uint64_t task_ns[DEFAULT_REPETITIONS] = {0};
        uint64_t switches[DEFAULT_REPETITIONS] = {0};
        int ok_runs = 0;
        for (int r = 0; r < DEFAULT_REPETITIONS; ++r) {
            const bench_result_t result = run_parallel(kinds[k], 1, w, c, iters, &expected);
            lock_times[r] = result.elapsed_s;
            task_ns[r] = result.task_clock_ns;
            switches[r] = result.context_switches;
            if (result.correct) ++ok_runs;
            fprintf(raw, "%s,%d,1,0.1,%d,%d,%" PRIu64 ",%.9f,", lock_name(kinds[k]),
                    r + 1, w, c, iters, result.elapsed_s);
            if (result.task_clock_ok) fprintf(raw, "%" PRIu64, result.task_clock_ns);
            fprintf(raw, ",");
            if (result.context_switches_ok) fprintf(raw, "%" PRIu64, result.context_switches);
            fprintf(raw, ",%" PRIu64 ",%d,%s\n", result.actual_total,
                    result.correct ? 1 : 0, result.timeout ? "timeout" : "completed");
        }
        double sorted[DEFAULT_REPETITIONS];
        memcpy(sorted, lock_times, sizeof(sorted));
        if (sorted[0] > sorted[1]) { double t=sorted[0]; sorted[0]=sorted[1]; sorted[1]=t; }
        if (sorted[1] > sorted[2]) { double t=sorted[1]; sorted[1]=sorted[2]; sorted[2]=t; }
        if (sorted[0] > sorted[1]) { double t=sorted[0]; sorted[0]=sorted[1]; sorted[1]=t; }
        const double med = sorted[1];
        uint64_t s0=task_ns[0], s1=task_ns[1], s2=task_ns[2];
        if(s0>s1){uint64_t t=s0;s0=s1;s1=t;} if(s1>s2){uint64_t t=s1;s1=s2;s2=t;} if(s0>s1){uint64_t t=s0;s0=s1;s1=t;}
        uint64_t c0=switches[0], c1=switches[1], c2=switches[2];
        if(c0>c1){uint64_t t=c0;c0=c1;c1=t;} if(c1>c2){uint64_t t=c1;c1=c2;c2=t;} if(c0>c1){uint64_t t=c0;c0=c1;c1=t;}
        const double overhead_ns = (med - serial_median) * 1e9 / (double)iters;
        fprintf(summary, "%s,0.1,%d,%d,1,%" PRIu64 ",%.9f,%.9f,%.3f,%.3f,%.0f,%d,%d\n",
                lock_name(kinds[k]), w, c, iters, serial_median, med, overhead_ns,
                (double)s1 / 1e6, (double)c1, ok_runs, DEFAULT_REPETITIONS);
        printf("  %-16s median=%.3f s, added lock/unlock ~= %.3f ns/record, correctness %d/3\n",
               lock_name(kinds[k]), med, overhead_ns, ok_runs);
    }
    fclose(serial_csv); fclose(raw); fclose(summary);
    printf("Task 3a CSV files written.\n");
}

static void run_fairness(int h, int c)
{
    static const lock_kind_t kinds[LOCK_COUNT] = {
        LOCK_TAS, LOCK_TTAS, LOCK_TTAS_BACKOFF, LOCK_TICKET, LOCK_MUTEX
    };
    FILE *counts_file = fopen("results/task3_fairness_counts.csv", "w");
    FILE *summary = fopen("results/task3_fairness_summary.csv", "w");
    if (!counts_file || !summary) { perror("open fairness CSV"); exit(EXIT_FAILURE); }
    fprintf(counts_file, "lock,N,run,tid,acquisitions,task_clock_ns,context_switches\n");
    fprintf(summary, "lock,N,run,duration_s,min_count,max_count,mean_count,max_min_ratio,jain_index,task_clock_ms,context_switches\n");

    const int thread_counts[2] = {h, 2 * h};
    printf("\nTask 3b: fairness test (W=0, shared_work(C=%d), duration=%.1f s, 3 repetitions)\n",
           c, FAIRNESS_SECONDS);
    for (int k = 0; k < LOCK_COUNT; ++k) {
        for (int ni = 0; ni < 2; ++ni) {
            const int n = thread_counts[ni];
            for (int rep = 1; rep <= DEFAULT_REPETITIONS; ++rep) {
                pthread_t *threads = calloc((size_t)n, sizeof(*threads));
                worker_arg_t *args = calloc((size_t)n, sizeof(*args));
                if (!threads || !args) { perror("calloc fairness"); exit(EXIT_FAILURE); }
                shared_t shared = {0};
                lock_t lock;
                lock_init(&lock, kinds[k]);
                pthread_barrier_t barrier;
                atomic_bool stop;
                atomic_init(&stop, false);
                int rc = pthread_barrier_init(&barrier, NULL, (unsigned int)(n + 1));
                if (rc != 0) { fprintf(stderr, "barrier: %s\n", strerror(rc)); exit(EXIT_FAILURE); }
                for (int t = 0; t < n; ++t) {
                    args[t] = (worker_arg_t){
                        .lock=&lock,.kind=kinds[k],.shared=&shared,.barrier=&barrier,
                        .stop=&stop,.nthreads=n,.tid=t,.w=0,.c=c,.fairness_mode=true
                    };
                    rc = pthread_create(&threads[t], NULL, worker_main, &args[t]);
                    if (rc != 0) { fprintf(stderr, "pthread_create: %s\n", strerror(rc)); exit(EXIT_FAILURE); }
                }
                rc = pthread_barrier_wait(&barrier);
                if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) { fprintf(stderr, "barrier wait failed\n"); exit(EXIT_FAILURE); }
                const double start = now_seconds();
                const double deadline = start + FAIRNESS_SECONDS;
                while (now_seconds() < deadline) {
                    struct timespec nap = {0, POLL_NS};
                    (void)nanosleep(&nap, NULL);
                }
                atomic_store_explicit(&stop, true, memory_order_relaxed);
                for (int t = 0; t < n; ++t) {
                    rc = pthread_join(threads[t], NULL);
                    if (rc != 0) { fprintf(stderr, "pthread_join: %s\n", strerror(rc)); exit(EXIT_FAILURE); }
                }
                const double elapsed = now_seconds() - start;
                uint64_t min_count = UINT64_MAX, max_count = 0, sum = 0, sum_sq = 0;
                uint64_t task_sum = 0, switch_sum = 0;
                for (int t = 0; t < n; ++t) {
                    const uint64_t x = args[t].acquisitions;
                    if (x < min_count) min_count = x;
                    if (x > max_count) max_count = x;
                    sum += x;
                    sum_sq += x * x;
                    task_sum += args[t].events.task_clock_ns;
                    switch_sum += args[t].events.context_switches;
                    fprintf(counts_file, "%s,%d,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                            lock_name(kinds[k]), n, rep, t, x,
                            args[t].events.task_clock_ok ? args[t].events.task_clock_ns : (uint64_t)0,
                            args[t].events.context_switches_ok ? args[t].events.context_switches : (uint64_t)0);
                }
                const double ratio = min_count == 0 ? INFINITY : (double)max_count / (double)min_count;
                const double jain = sum_sq == 0 ? 0.0 : ((double)sum * (double)sum) / ((double)n * (double)sum_sq);
                fprintf(summary, "%s,%d,%d,%.6f,%" PRIu64 ",%" PRIu64 ",%.2f,%.6f,%.6f,%.3f,%" PRIu64 "\n",
                        lock_name(kinds[k]), n, rep, elapsed, min_count, max_count,
                        (double)sum/(double)n, ratio, jain, (double)task_sum/1e6, switch_sum);
                printf("  %-16s N=%-3d run=%d J=%.4f max/min=%.3f\n",
                       lock_name(kinds[k]), n, rep, jain, ratio);
                (void)pthread_barrier_destroy(&barrier);
                lock_destroy(&lock, kinds[k]);
                free(threads); free(args);
            }
        }
    }
    fclose(counts_file); fclose(summary);
    printf("Fairness raw per-thread counts and summary written.\n");
}

static void run_ttas_comparison(int h, int w, int c, uint64_t iters, bool append)
{
    const int thread_counts[2] = {h, 2 * h};
    const char *path =
#ifdef USE_PAUSE
        "results/task3_ttas_pause_raw.csv";
#else
        "results/task3_ttas_nopause_raw.csv";
#endif
    FILE *f = fopen(path, append ? "a" : "w");
    if (!f) { perror(path); exit(EXIT_FAILURE); }
    if (!append) fprintf(f, "lock,N,F,W,C,run,total_iters,time_s,task_clock_ms,context_switches,expected_total,actual_total,ok,status,spin_pause\n");
    printf("\nTTAS F=0.1 comparison, USE_PAUSE=%s\n",
#ifdef USE_PAUSE
           "enabled"
#else
           "disabled"
#endif
    );
    for (int ni = 0; ni < 2; ++ni) {
        const int n = thread_counts[ni];
        shared_t expected = {0};
        double serial_s = 0.0;
        event_values_t serial_ev;
        if (!serial_reference(w, c, iters, &expected, &serial_s, &serial_ev)) {
            fprintf(stderr, "Serial reference failed while preparing TTAS comparison.\n");
            fclose(f); exit(EXIT_FAILURE);
        }
        for (int rep = 1; rep <= DEFAULT_REPETITIONS; ++rep) {
            const bench_result_t r = run_parallel(LOCK_TTAS, n, w, c, iters, &expected);
            fprintf(f, "TTAS,%d,0.1,%d,%d,%d,%" PRIu64 ",%.9f,", n, w, c,
                    rep, iters, r.elapsed_s);
            if (r.task_clock_ok) fprintf(f, "%.3f", (double)r.task_clock_ns/1e6);
            fprintf(f, ",");
            if (r.context_switches_ok) fprintf(f, "%" PRIu64, r.context_switches);
            fprintf(f, ",%" PRIu64 ",%" PRIu64 ",%d,%s,",
                    expected.total, r.actual_total, r.correct ? 1 : 0,
                    r.timeout ? "timeout" : "completed");
#ifdef USE_PAUSE
            fprintf(f, "1\n");
#else
            fprintf(f, "0\n");
#endif
            printf("  N=%d run=%d wall=%.3f s ok=%d%s\n", n, rep, r.elapsed_s,
                   r.correct ? 1 : 0, r.timeout ? " timeout" : "");
        }
    }
    fclose(f);
}

static void usage(const char *name)
{
    printf("Usage: %s [--ttas-only]\n", name);
    printf("Default: Task 3a uncontended cost, Task 3b fairness, and TTAS F=0.1 H/2H comparison.\n");
    printf("Requires results/calibration.csv and results/serial_reference.csv from Task 1.\n");
}

int main(int argc, char **argv)
{
    bool ttas_only = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ttas-only") == 0) ttas_only = true;
        else if (strcmp(argv[i], "--help") == 0) { usage(argv[0]); return EXIT_SUCCESS; }
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); usage(argv[0]); return EXIT_FAILURE; }
    }

    ensure_results_dir();
    int w = 0;
    double measured_f = 0.0;
    uint64_t iters = 0;
    if (read_task1_configuration(&w, &measured_f, &iters) != 0) return EXIT_FAILURE;
    const int h = available_hardware_threads();
    if (h < 1 || 2*h > MAX_THREADS) {
        fprintf(stderr, "Invalid H=%d; supported range is 1..%d.\n", h, MAX_THREADS/2);
        return EXIT_FAILURE;
    }
    printf("Task 3 lock zoo | H=%d | N fairness=%d,%d | F target=0.1 (measured=%.4f) W=%d C=%d | TOTAL_ITERS=%" PRIu64 "\n",
           h, h, 2*h, measured_f, w, CRITICAL_SIZE, iters);
#ifdef USE_PAUSE
    printf("Build mode: -DUSE_PAUSE enabled for every spin loop.\n");
#else
    printf("Build mode: USE_PAUSE disabled (TTAS comparison only).\n");
#endif
    if (ttas_only) {
        run_ttas_comparison(h, w, CRITICAL_SIZE, iters, false);
        return EXIT_SUCCESS;
    }
    run_uncontended(w, CRITICAL_SIZE, iters);
    run_fairness(h, CRITICAL_SIZE);
    run_ttas_comparison(h, w, CRITICAL_SIZE, iters, false);
    return EXIT_SUCCESS;
}
