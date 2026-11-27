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
#include <strings.h>
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
#define RUN_TIMEOUT_SECONDS 60.0
#define MAX_THREADS 256
#define MAX_CSV_FIELDS 16
#define CALIBRATION_PATH "results/calibration.csv"
#define SERIAL_REFERENCE_PATH "results/serial_reference.csv"
#define OUTPUT_RAW_PATH "results/task4_sweep_raw.csv"
#define OUTPUT_SERIAL_PATH "results/task4_serial_raw.csv"

/* Keep the exact shared-table layout required by the assignment. */
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
    uint64_t processed;
    bool timed_out;
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

typedef struct {
    double target_f;
    double measured_f;
    int w;
    uint64_t total_iters;
} task4_config_t;

static const lock_kind_t ALL_LOCKS[LOCK_COUNT] = {
    LOCK_TAS, LOCK_TTAS, LOCK_TTAS_BACKOFF, LOCK_TICKET, LOCK_MUTEX
};

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
        perror("clock_gettime(CLOCK_MONOTONIC)");
        exit(EXIT_FAILURE);
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint64_t timespec_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * UINT64_C(1000000000) + (uint64_t)ts->tv_nsec;
}

static void event_begin(event_start_t *s)
{
    memset(s, 0, sizeof(*s));
    s->cpu_ok = (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &s->cpu_start) == 0);
#ifdef RUSAGE_THREAD
    s->usage_ok = (getrusage(RUSAGE_THREAD, &s->usage_start) == 0);
#endif
}

static event_values_t event_end(const event_start_t *s)
{
    event_values_t e = {0};
    struct timespec cpu_end;
    struct rusage usage_end;

    e.task_clock_ok = s->cpu_ok &&
                      (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end) == 0);
    if (e.task_clock_ok) {
        const uint64_t before = timespec_ns(&s->cpu_start);
        const uint64_t after = timespec_ns(&cpu_end);
        if (after >= before) e.task_clock_ns = after - before;
        else e.task_clock_ok = false;
    }

#ifdef RUSAGE_THREAD
    e.context_switches_ok = s->usage_ok &&
                            (getrusage(RUSAGE_THREAD, &usage_end) == 0);
    if (e.context_switches_ok) {
        const long voluntary = usage_end.ru_nvcsw - s->usage_start.ru_nvcsw;
        const long involuntary = usage_end.ru_nivcsw - s->usage_start.ru_nivcsw;
        if (voluntary >= 0 && involuntary >= 0) {
            e.context_switches = (uint64_t)voluntary + (uint64_t)involuntary;
        } else {
            e.context_switches_ok = false;
        }
    }
#endif
    return e;
}

static inline uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* These work functions intentionally match the supplied homework benchmark. */
static uint64_t private_work(uint64_t i, int w)
{
    uint64_t rec[8];
    uint64_t seed = i;
    const uint8_t *p;
    uint64_t h = UINT64_C(1469598103934665603);

    for (int j = 0; j < 8; ++j) rec[j] = splitmix64(&seed);
    p = (const uint8_t *)rec;
    for (int r = 0; r < w; ++r) {
        for (int j = 0; j < 64; ++j) {
            h ^= p[j];
            h *= UINT64_C(1099511628211);
        }
    }
    return h;
}

static void shared_work(shared_t *s, uint64_t h, int c)
{
    uint64_t x = h;
    for (int k = 0; k < c; ++k) {
        x = x * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        s->hist[x >> 58]++;
    }
    s->total++;
    if (h > s->max_score) s->max_score = h;
}

static bool shared_equal(const shared_t *a, const shared_t *b)
{
    if (a->total != b->total || a->max_score != b->max_score) return false;
    for (size_t i = 0; i < NBUCKETS; ++i) {
        if (a->hist[i] != b->hist[i]) return false;
    }
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

/* pthread_mutex_timedlock uses CLOCK_REALTIME absolute deadlines. */
static bool mutex_lock_until(pthread_mutex_t *mutex, double monotonic_deadline)
{
    const double remaining = monotonic_deadline - now_seconds();
    if (remaining <= 0.0) return false;

    struct timespec abs_deadline;
    if (clock_gettime(CLOCK_REALTIME, &abs_deadline) != 0) {
        perror("clock_gettime(CLOCK_REALTIME)");
        abort();
    }

    const time_t add_seconds = (time_t)remaining;
    const long add_nanoseconds = (long)((remaining - (double)add_seconds) * 1e9);
    abs_deadline.tv_sec += add_seconds;
    abs_deadline.tv_nsec += add_nanoseconds;
    if (abs_deadline.tv_nsec >= 1000000000L) {
        abs_deadline.tv_sec++;
        abs_deadline.tv_nsec -= 1000000000L;
    }

    const int rc = pthread_mutex_timedlock(mutex, &abs_deadline);
    if (rc == 0) return true;
    if (rc == ETIMEDOUT) return false;
    fprintf(stderr, "pthread_mutex_timedlock: %s\n", strerror(rc));
    abort();
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
            /* A ticket cannot be abandoned out of order. If it expires, wait for
               our turn, advance the queue, and leave without entering the CS. */
            if (!expired_while_waiting && deadline > 0.0 &&
                (++spins & 4095U) == 0U && now_seconds() >= deadline) {
                expired_while_waiting = true;
            }
        }
        if (expired_while_waiting) {
            (void)atomic_fetch_add_explicit(&l->u.ticket.now_serving, 1U, memory_order_release);
            return false;
        }
        return true;
    }

    case LOCK_MUTEX:
        return mutex_lock_until(&l->mutex, deadline);

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
    const double deadline = now_seconds() + RUN_TIMEOUT_SECONDS;

    for (uint64_t i = (uint64_t)a->tid; i < a->total_iters; i += (uint64_t)a->nthreads) {
        if ((a->processed & 4095ULL) == 0ULL && now_seconds() >= deadline) {
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

        /* Another worker may have reached the run timeout while this worker waited. */
        if (atomic_load_explicit(a->stop, memory_order_relaxed)) {
            lock_release(a->lock, a->kind);
            break;
        }

        shared_work(a->shared, h, a->c);
        lock_release(a->lock, a->kind);
        a->processed++;
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
        perror("calloc workers");
        exit(EXIT_FAILURE);
    }

    shared_t shared = {0};
    lock_t lock;
    lock_init(&lock, kind);
    pthread_barrier_t barrier;
    atomic_bool stop;
    atomic_init(&stop, false);

    int rc = pthread_barrier_init(&barrier, NULL, (unsigned int)(nthreads + 1));
    if (rc != 0) {
        fprintf(stderr, "pthread_barrier_init: %s\n", strerror(rc));
        exit(EXIT_FAILURE);
    }

    for (int t = 0; t < nthreads; ++t) {
        args[t] = (worker_arg_t){
            .lock = &lock, .kind = kind, .shared = &shared, .barrier = &barrier,
            .stop = &stop, .total_iters = total_iters, .nthreads = nthreads,
            .tid = t, .w = w, .c = c
        };
        rc = pthread_create(&threads[t], NULL, worker_main, &args[t]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            exit(EXIT_FAILURE);
        }
    }

    rc = pthread_barrier_wait(&barrier);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        fprintf(stderr, "pthread_barrier_wait failed\n");
        exit(EXIT_FAILURE);
    }

    const double start = now_seconds();
    for (int t = 0; t < nthreads; ++t) {
        rc = pthread_join(threads[t], NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_join: %s\n", strerror(rc));
            exit(EXIT_FAILURE);
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

static size_t split_csv(char *line, char **fields, size_t max_fields)
{
    size_t count = 0;
    char *save = NULL;
    char *token = strtok_r(line, ",", &save);
    while (token != NULL && count < max_fields) {
        fields[count++] = token;
        token = strtok_r(NULL, ",", &save);
    }
    return count;
}

static bool parse_bool_field(const char *s)
{
    return s != NULL && (strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0 ||
                         strcasecmp(s, "ok") == 0 || strcasecmp(s, "completed") == 0);
}

static int read_calibration(task4_config_t configs[2])
{
    FILE *f = fopen(CALIBRATION_PATH, "r");
    if (f == NULL) {
        fprintf(stderr, "Cannot open %s. Run Task 1 first.\n", CALIBRATION_PATH);
        return -1;
    }

    char line[512];
    if (fgets(line, sizeof(line), f) == NULL) {
        fprintf(stderr, "%s is empty.\n", CALIBRATION_PATH);
        fclose(f);
        return -1;
    }

    bool found[2] = {false, false};
    while (fgets(line, sizeof(line), f) != NULL) {
        char copy[512];
        (void)snprintf(copy, sizeof(copy), "%s", line);
        char *fields[MAX_CSV_FIELDS] = {0};
        const size_t n = split_csv(copy, fields, MAX_CSV_FIELDS);
        if (n < 9) continue;

        const double target = strtod(fields[0], NULL);
        const long c = strtol(fields[1], NULL, 10);
        const long w = strtol(fields[2], NULL, 10);
        const double measured = strtod(fields[6], NULL);
        fields[8][strcspn(fields[8], "\r\n")] = '\0';
        const bool within = parse_bool_field(fields[8]);
        int index = -1;
        if (fabs(target - 0.5) < 1e-6) index = 0;
        else if (fabs(target - 0.1) < 1e-6) index = 1;

        if (index >= 0 && c == CRITICAL_SIZE && w >= 0 &&
            measured > 0.0 && measured < 1.0 && within) {
            configs[index].target_f = target;
            configs[index].measured_f = measured;
            configs[index].w = (int)w;
            found[index] = true;
        }
    }
    fclose(f);

    if (!found[0] || !found[1]) {
        fprintf(stderr,
                "Need verified C=64 calibration rows for both F=0.5 and F=0.1 in %s.\n",
                CALIBRATION_PATH);
        return -1;
    }
    return 0;
}

static int read_serial_iterations(task4_config_t configs[2])
{
    FILE *f = fopen(SERIAL_REFERENCE_PATH, "r");
    if (f == NULL) {
        fprintf(stderr, "Cannot open %s. Run Task 1 first.\n", SERIAL_REFERENCE_PATH);
        return -1;
    }

    char line[512];
    if (fgets(line, sizeof(line), f) == NULL) {
        fprintf(stderr, "%s is empty.\n", SERIAL_REFERENCE_PATH);
        fclose(f);
        return -1;
    }

    bool found[2] = {false, false};
    while (fgets(line, sizeof(line), f) != NULL) {
        char copy[512];
        (void)snprintf(copy, sizeof(copy), "%s", line);
        char *fields[MAX_CSV_FIELDS] = {0};
        const size_t n = split_csv(copy, fields, MAX_CSV_FIELDS);
        if (n < 9) continue;

        const double target = strtod(fields[0], NULL);
        const long c = strtol(fields[1], NULL, 10);
        const long w = strtol(fields[2], NULL, 10);
        const unsigned long long expected = strtoull(fields[5], NULL, 10);
        fields[8][strcspn(fields[8], "\r\n")] = '\0';

        int index = -1;
        if (fabs(target - 0.5) < 1e-6) index = 0;
        else if (fabs(target - 0.1) < 1e-6) index = 1;

        if (index >= 0 && c == CRITICAL_SIZE && w == configs[index].w &&
            expected > 0ULL && (strcmp(fields[8], "ok") == 0 ||
                                strcmp(fields[8], "completed") == 0)) {
            if (found[index] && configs[index].total_iters != (uint64_t)expected) {
                fprintf(stderr, "Inconsistent TOTAL_ITERS across serial reference rows for F=%.1f.\n",
                        target);
                fclose(f);
                return -1;
            }
            configs[index].total_iters = (uint64_t)expected;
            found[index] = true;
        }
    }
    fclose(f);

    if (!found[0] || !found[1]) {
        fprintf(stderr,
                "Need successful C=64 serial_reference.csv rows matching calibrated W for F=0.5 and F=0.1.\n");
        return -1;
    }
    if (configs[0].total_iters != configs[1].total_iters) {
        fprintf(stderr, "Task 4 expects the same TOTAL_ITERS for F=0.5 and F=0.1.\n");
        return -1;
    }
    return 0;
}

static int available_hardware_threads(void)
{
    FILE *p = popen("nproc", "r");
    if (p != NULL) {
        char buffer[64];
        if (fgets(buffer, sizeof(buffer), p) != NULL) {
            char *end = NULL;
            const long count = strtol(buffer, &end, 10);
            (void)pclose(p);
            if (end != buffer && count > 0 && count <= MAX_THREADS / 2)
                return (int)count;
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

    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1 || count > MAX_THREADS / 2) return 1;
    return (int)count;
}

static void ensure_results_dir(void)
{
    if (mkdir("results", 0775) != 0 && errno != EEXIST) {
        perror("mkdir results");
        exit(EXIT_FAILURE);
    }
}

static double median3(double a, double b, double c)
{
    if (a > b) { double t = a; a = b; b = t; }
    if (b > c) { double t = b; b = c; c = t; }
    if (a > b) { double t = a; a = b; b = t; }
    return b;
}

static void write_serial_header(FILE *f)
{
    fprintf(f, "F,F_measured,W,C,run,total_iters,time_s,task_clock_ms,context_switches,ok,status\n");
}

static size_t build_thread_counts(int h, int unique_counts[6])
{
    /* Section B requires 1, 2, 4, H, 2H, and an additional N=8 if absent. */
    const int requested[6] = {1, 2, 4, h, 2 * h, 8};
    size_t unique_count = 0;
    for (size_t i = 0; i < 6; ++i) {
        bool duplicate = false;
        for (size_t j = 0; j < unique_count; ++j) {
            if (unique_counts[j] == requested[i]) duplicate = true;
        }
        if (!duplicate) unique_counts[unique_count++] = requested[i];
    }
    return unique_count;
}

static void run_task4(task4_config_t configs[2], int h)
{
    int unique_counts[6];
    const size_t unique_count = build_thread_counts(h, unique_counts);

    FILE *serial_csv = fopen(OUTPUT_SERIAL_PATH, "w");
    FILE *raw_csv = fopen(OUTPUT_RAW_PATH, "w");
    if (serial_csv == NULL || raw_csv == NULL) {
        perror("open Task 4 output CSV");
        if (serial_csv != NULL) fclose(serial_csv);
        if (raw_csv != NULL) fclose(raw_csv);
        exit(EXIT_FAILURE);
    }

    write_serial_header(serial_csv);
    fprintf(raw_csv,
            "lock,N,F,F_measured,W,C,run,total_iters,time_s,task_clock_ms,context_switches,expected_total,actual_total,ok,status\n");

    printf("\nTask 4: Amdahl sweep | H=%d | N=", h);
    for (size_t i = 0; i < unique_count; ++i)
        printf("%s%d", i == 0 ? "" : ",", unique_counts[i]);
    printf(" | C=%d | repetitions=%d | timeout=%.0f s\n",
           CRITICAL_SIZE, DEFAULT_REPETITIONS, RUN_TIMEOUT_SECONDS);

#ifdef USE_PAUSE
    printf("Spin hint: enabled (-DUSE_PAUSE).\n");
#else
    printf("Spin hint: disabled. For the standard Task 4 run, compile with -DUSE_PAUSE.\n");
#endif

    for (int fi = 0; fi < 2; ++fi) {
        task4_config_t *cfg = &configs[fi];
        shared_t expected = {0};
        double serial_times[DEFAULT_REPETITIONS] = {0};
        bool serial_ok = true;

        printf("\nSerial baseline: F=%.1f, F_measured=%.6f, W=%d, TOTAL_ITERS=%" PRIu64 "\n",
               cfg->target_f, cfg->measured_f, cfg->w, cfg->total_iters);

        for (int rep = 1; rep <= DEFAULT_REPETITIONS; ++rep) {
            shared_t current = {0};
            event_values_t events = {0};
            bool ok = serial_reference(cfg->w, CRITICAL_SIZE, cfg->total_iters,
                                       &current, &serial_times[rep - 1], &events);
            if (rep == 1) expected = current;
            else if (!shared_equal(&expected, &current)) ok = false;
            if (!ok) serial_ok = false;

            fprintf(serial_csv, "%.1f,%.6f,%d,%d,%d,%" PRIu64 ",%.9f,",
                    cfg->target_f, cfg->measured_f, cfg->w, CRITICAL_SIZE,
                    rep, cfg->total_iters, serial_times[rep - 1]);
            if (events.task_clock_ok) fprintf(serial_csv, "%.3f", (double)events.task_clock_ns / 1e6);
            fprintf(serial_csv, ",");
            if (events.context_switches_ok) fprintf(serial_csv, "%" PRIu64, events.context_switches);
            fprintf(serial_csv, ",%d,%s\n", ok ? 1 : 0, ok ? "completed" : "failed");
            fflush(serial_csv);

            printf("  serial run=%d wall=%.3f s task_clock=%.3f ms context_switches=",
                   rep, serial_times[rep - 1],
                   events.task_clock_ok ? (double)events.task_clock_ns / 1e6 : NAN);
            if (events.context_switches_ok) printf("%" PRIu64, events.context_switches);
            else printf("NA");
            printf(" ok=%d\n", ok ? 1 : 0);
        }

        if (!serial_ok) {
            fprintf(stderr, "Serial baseline failed for F=%.1f; refusing to run parallel comparisons.\n",
                    cfg->target_f);
            fclose(serial_csv);
            fclose(raw_csv);
            exit(EXIT_FAILURE);
        }

        const double serial_median = median3(serial_times[0], serial_times[1], serial_times[2]);
        if (serial_median < 2.0) {
            fprintf(stderr,
                    "ERROR: serial median for F=%.1f is %.3f s (<2 s). Increase Task 1 TOTAL_ITERS and rerun.\n",
                    cfg->target_f, serial_median);
            fclose(serial_csv);
            fclose(raw_csv);
            exit(EXIT_FAILURE);
        }
        printf("  serial median = %.6f s\n", serial_median);

        for (int k = 0; k < LOCK_COUNT; ++k) {
            for (size_t ni = 0; ni < unique_count; ++ni) {
                const int n = unique_counts[ni];
                printf("\n  %-16s F=%.1f N=%d\n", lock_name(ALL_LOCKS[k]), cfg->target_f, n);

                for (int rep = 1; rep <= DEFAULT_REPETITIONS; ++rep) {
                    const bench_result_t r = run_parallel(ALL_LOCKS[k], n, cfg->w,
                                                          CRITICAL_SIZE, cfg->total_iters,
                                                          &expected);
                    const char *status = r.timeout ? "timeout" : (r.correct ? "completed" : "incorrect");

                    fprintf(raw_csv, "%s,%d,%.1f,%.6f,%d,%d,%d,%" PRIu64 ",%.9f,",
                            lock_name(ALL_LOCKS[k]), n, cfg->target_f, cfg->measured_f,
                            cfg->w, CRITICAL_SIZE, rep, cfg->total_iters, r.elapsed_s);
                    if (r.task_clock_ok) fprintf(raw_csv, "%.3f", (double)r.task_clock_ns / 1e6);
                    fprintf(raw_csv, ",");
                    if (r.context_switches_ok) fprintf(raw_csv, "%" PRIu64, r.context_switches);
                    fprintf(raw_csv, ",%" PRIu64 ",%" PRIu64 ",%d,%s\n",
                            cfg->total_iters, r.actual_total, r.correct ? 1 : 0, status);
                    fflush(raw_csv);

                    printf("    run=%d wall=%.3f s task_clock=", rep, r.elapsed_s);
                    if (r.task_clock_ok) printf("%.3f ms", (double)r.task_clock_ns / 1e6);
                    else printf("NA");
                    printf(" context_switches=");
                    if (r.context_switches_ok) printf("%" PRIu64, r.context_switches);
                    else printf("NA");
                    printf(" total=%" PRIu64 "/%" PRIu64 " status=%s\n",
                           r.actual_total, cfg->total_iters, status);
                }
            }
        }
    }

    fclose(serial_csv);
    fclose(raw_csv);
    printf("\nTask 4 completed. Raw data written to:\n  %s\n  %s\n",
           OUTPUT_SERIAL_PATH, OUTPUT_RAW_PATH);
}

int main(void)
{
    ensure_results_dir();
    task4_config_t configs[2] = {0};
    if (read_calibration(configs) != 0) return EXIT_FAILURE;
    if (read_serial_iterations(configs) != 0) return EXIT_FAILURE;

    const int h = available_hardware_threads();
    if (h < 1 || 2 * h > MAX_THREADS) {
        fprintf(stderr, "Invalid H=%d; supported range is 1..%d.\n", h, MAX_THREADS / 2);
        return EXIT_FAILURE;
    }

    int required_counts[6];
    const size_t required_count = build_thread_counts(h, required_counts);
    for (int i = 0; i < 2; ++i) {
        for (size_t j = 0; j < required_count; ++j) {
            if (configs[i].total_iters % (uint64_t)required_counts[j] != 0ULL) {
                fprintf(stderr,
                        "TOTAL_ITERS=%" PRIu64 " is not divisible by required N=%d. Rerun Task 1 with a divisible iteration count.\n",
                        configs[i].total_iters, required_counts[j]);
                return EXIT_FAILURE;
            }
        }
    }

    printf("Task 4 lock sweep | guest-visible H=nproc=%d | C=%d\n", h, CRITICAL_SIZE);
    for (int i = 0; i < 2; ++i) {
        printf("  F=%.1f: W=%d, F_measured=%.6f, TOTAL_ITERS=%" PRIu64 "\n",
               configs[i].target_f, configs[i].w, configs[i].measured_f,
               configs[i].total_iters);
    }
    printf("Metrics: wall-clock=clock_gettime(CLOCK_MONOTONIC); task-clock=sum of per-thread CPU time;\n"
           "         context-switches=sum of per-thread voluntary + involuntary switches.\n");

    run_task4(configs, h);
    return EXIT_SUCCESS;
}
