#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>

#define NBUCKETS 64
#define DEFAULT_REPETITIONS 3
#define EXTRA_THREAD_COUNT 16
#define RUN_TIMEOUT_SECONDS 60.0
#define TARGET_SERIAL_SECONDS 2.5
#define CAL_SHARED_ITERS 120000ULL
#define CAL_PRIVATE_BUDGET 8000000ULL
#define MIN_CAL_PRIVATE_ITERS 5000ULL
#define MAX_CAL_PRIVATE_ITERS 180000ULL

/* The serial reference uses ordinary fields. */
typedef struct {
    uint64_t hist[NBUCKETS];
    uint64_t total;
    uint64_t max_score;
} shared_t;

/* Volatile is used only in the intentional no-lock experiment. */
typedef struct {
    volatile uint64_t hist[NBUCKETS];
    volatile uint64_t total;
    volatile uint64_t max_score;
} race_shared_t;

typedef struct {
    double target_f;
    int c;
    int w;
    double tc_ns;
    double a_ns;
    double tw_ns;
    double measured_f;
    double serial_estimate_ns;
    bool within_tolerance;
} calibration_t;

/*
 * Fallback measurements when perf_event_open is denied in a VM:
 *   task-clock       -> per-thread CPU time from CLOCK_THREAD_CPUTIME_ID
 *   context-switches -> per-thread ru_nvcsw + ru_nivcsw from getrusage()
 * These are measured from real kernel accounting; no values are estimated.
 */
typedef struct {
    uint64_t task_clock_ns;
    uint64_t context_switches;
    bool task_clock_available;
    bool context_switches_available;
    int task_clock_errno;
    int context_switches_errno;
} event_measurement_t;

typedef struct {
    struct timespec task_clock_start;
    struct rusage usage_start;
    bool task_clock_started;
    bool context_switches_started;
    int task_clock_errno;
    int context_switches_errno;
} event_snapshot_t;

typedef struct {
    int tid;
    int nthreads;
    int w;
    int c;
    uint64_t total_iters;
    uint64_t processed;
    bool timed_out;
    race_shared_t *shared;
    pthread_barrier_t *barrier;
    event_measurement_t events;
} worker_arg_t;

typedef struct {
    double time_s;
    event_measurement_t events;
    uint64_t ops_min;
    uint64_t ops_max;
    uint64_t actual_total;
    uint64_t actual_hist_sum;
    uint64_t actual_max_score;
    uint64_t bucket_mismatches;
    bool ok;
    bool timeout;
} race_result_t;

static volatile uint64_t calibration_sink;

static double now_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

/* Convert a timespec to an unsigned nanosecond count. */
static uint64_t timespec_to_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

/*
 * Begin kernel-accounting fallback measurements for the calling thread.
 * RUSAGE_THREAD gives voluntary and involuntary context-switch counters.
 * CLOCK_THREAD_CPUTIME_ID measures CPU time actually consumed by this thread.
 */
static void event_snapshot_begin(event_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));

    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID,
                      &snapshot->task_clock_start) == 0) {
        snapshot->task_clock_started = true;
    } else {
        snapshot->task_clock_errno = errno;
    }

#ifdef RUSAGE_THREAD
    if (getrusage(RUSAGE_THREAD, &snapshot->usage_start) == 0) {
        snapshot->context_switches_started = true;
    } else {
        snapshot->context_switches_errno = errno;
    }
#else
    snapshot->context_switches_errno = ENOTSUP;
#endif
}

/* Finish fallback measurements and return the measured deltas. */
static void event_snapshot_finish(const event_snapshot_t *snapshot,
                                  event_measurement_t *measurement)
{
    struct timespec task_clock_end;
    struct rusage usage_end;

    memset(measurement, 0, sizeof(*measurement));
    measurement->task_clock_errno = snapshot->task_clock_errno;
    measurement->context_switches_errno = snapshot->context_switches_errno;

    if (snapshot->task_clock_started) {
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &task_clock_end) == 0) {
            const uint64_t start_ns = timespec_to_ns(&snapshot->task_clock_start);
            const uint64_t end_ns = timespec_to_ns(&task_clock_end);
            if (end_ns >= start_ns) {
                measurement->task_clock_ns = end_ns - start_ns;
                measurement->task_clock_available = true;
            } else {
                measurement->task_clock_errno = EIO;
            }
        } else {
            measurement->task_clock_errno = errno;
        }
    }

#ifdef RUSAGE_THREAD
    if (snapshot->context_switches_started) {
        if (getrusage(RUSAGE_THREAD, &usage_end) == 0) {
            const int64_t start_count =
                (int64_t)snapshot->usage_start.ru_nvcsw +
                (int64_t)snapshot->usage_start.ru_nivcsw;
            const int64_t end_count =
                (int64_t)usage_end.ru_nvcsw + (int64_t)usage_end.ru_nivcsw;
            if (end_count >= start_count) {
                measurement->context_switches =
                    (uint64_t)(end_count - start_count);
                measurement->context_switches_available = true;
            } else {
                measurement->context_switches_errno = EIO;
            }
        } else {
            measurement->context_switches_errno = errno;
        }
    }
#endif
}

static const char *event_status(bool available)
{
    return available ? "ok" : "unavailable";
}

static void print_event_values(const event_measurement_t *events)
{
    if (events->task_clock_available) {
        printf("task-clock=%.3f ms [CLOCK_THREAD_CPUTIME_ID fallback]",
               (double)events->task_clock_ns / 1000000.0);
    } else {
        printf("task-clock=unavailable(errno=%d: %s)",
               events->task_clock_errno,
               events->task_clock_errno ? strerror(events->task_clock_errno)
                                        : "unknown error");
    }

    printf(", ");

    if (events->context_switches_available) {
        printf("context-switches=%" PRIu64
               " [getrusage voluntary+involuntary fallback]",
               events->context_switches);
    } else {
        printf("context-switches=unavailable(errno=%d: %s)",
               events->context_switches_errno,
               events->context_switches_errno
                   ? strerror(events->context_switches_errno)
                   : "unknown error");
    }
}

static bool all_task_clock_available(const event_measurement_t *events,
                                     size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!events[i].task_clock_available) return false;
    }
    return true;
}

static bool all_context_switches_available(const event_measurement_t *events,
                                           size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!events[i].context_switches_available) return false;
    }
    return true;
}

static double median3(double a, double b, double c)
{
    if (a > b) {
        const double tmp = a;
        a = b;
        b = tmp;
    }
    if (b > c) {
        const double tmp = b;
        b = c;
        c = tmp;
    }
    if (a > b) {
        const double tmp = a;
        a = b;
        b = tmp;
    }
    return b;
}

static uint64_t median_u64(uint64_t a, uint64_t b, uint64_t c)
{
    if (a > b) {
        const uint64_t tmp = a;
        a = b;
        b = tmp;
    }
    if (b > c) {
        const uint64_t tmp = b;
        b = c;
        c = tmp;
    }
    if (a > b) {
        const uint64_t tmp = a;
        a = b;
        b = tmp;
    }
    return b;
}

static inline uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Private part: it only uses local variables. */
static uint64_t private_work(uint64_t i, int w)
{
    uint64_t rec[8];
    uint64_t seed = i;
    const uint8_t *p;
    uint64_t h = 1469598103934665603ULL;

    for (int j = 0; j < 8; j++) {
        rec[j] = splitmix64(&seed);
    }

    p = (const uint8_t *)rec;

    for (int r = 0; r < w; r++) {
        for (int j = 0; j < 64; j++) {
            h ^= p[j];
            h *= 1099511628211ULL;
        }
    }

    return h;
}

/* Serial critical-section work, used by the reference run. */
static void shared_work(shared_t *s, uint64_t h, int c)
{
    uint64_t x = h;

    for (int k = 0; k < c; k++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        s->hist[x >> 58]++;
    }

    s->total++;
    if (h > s->max_score) {
        s->max_score = h;
    }
}

/* Same updates, but deliberately without a lock and with volatile fields. */
static void shared_work_no_lock(race_shared_t *s, uint64_t h, int c)
{
    uint64_t x = h;

    for (int k = 0; k < c; k++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        s->hist[x >> 58]++;
    }

    s->total++;
    if (h > s->max_score) {
        s->max_score = h;
    }
}

static uint64_t private_calibration_iterations(int w)
{
    uint64_t denominator = (uint64_t)64 * (uint64_t)w;
    uint64_t count = CAL_PRIVATE_BUDGET / denominator;

    if (count < MIN_CAL_PRIVATE_ITERS) {
        count = MIN_CAL_PRIVATE_ITERS;
    }
    if (count > MAX_CAL_PRIVATE_ITERS) {
        count = MAX_CAL_PRIVATE_ITERS;
    }

    return count;
}

static double measure_private_once(int w)
{
    const uint64_t count = private_calibration_iterations(w);
    uint64_t checksum = 0;
    const double start = now_seconds();

    for (uint64_t i = 0; i < count; i++) {
        checksum ^= private_work(i + 1ULL, w);
    }

    const double end = now_seconds();
    calibration_sink = checksum;
    return (end - start) * 1000000000.0 / (double)count;
}

static double measure_private_ns(int w)
{
    const double a = measure_private_once(w);
    const double b = measure_private_once(w);
    const double c = measure_private_once(w);
    return median3(a, b, c);
}

static double measure_shared_once(int c)
{
    shared_t s = {0};
    const double start = now_seconds();

    for (uint64_t i = 0; i < CAL_SHARED_ITERS; i++) {
        const uint64_t h = (i * 0x9E3779B97F4A7C15ULL) ^ 0xA5A5A5A55A5A5A5AULL;
        shared_work(&s, h, c);
    }

    const double end = now_seconds();
    uint64_t checksum = s.total ^ s.max_score;
    for (size_t bucket = 0; bucket < NBUCKETS; bucket++) {
        checksum += s.hist[bucket] * ((uint64_t)bucket + 1ULL);
    }
    calibration_sink = checksum;
    return (end - start) * 1000000000.0 / (double)CAL_SHARED_ITERS;
}

static double measure_shared_ns(int c)
{
    const double a = measure_shared_once(c);
    const double b = measure_shared_once(c);
    const double d = measure_shared_once(c);
    return median3(a, b, d);
}

static int choose_w(double tc_ns, double a_ns, double target_f)
{
    const double estimate = tc_ns * (1.0 - target_f) / (target_f * a_ns);

    if (estimate >= (double)INT_MAX) {
        fprintf(stderr, "Calculated W is too large.\n");
        exit(EXIT_FAILURE);
    }

    int w = (int)llround(estimate);
    if (w < 1) {
        w = 1;
    }
    return w;
}

static calibration_t calibrate_one(double target_f, int c)
{
    calibration_t result;
    const double tc_ns = measure_shared_ns(c);
    const double a_ns = measure_private_ns(1);
    int w = choose_w(tc_ns, a_ns, target_f);
    double tw_ns = measure_private_ns(w);
    double measured_f = tc_ns / (tw_ns + tc_ns);

    int best_w = w;
    double best_tw_ns = tw_ns;
    double best_f = measured_f;
    double best_error = fabs(measured_f - target_f);

    /* Keep the closest integer W in case adjacent values straddle the target. */
    for (int attempt = 0; attempt < 8; attempt++) {
        if (best_error <= 0.05) {
            break;
        }

        const double scale = ((1.0 - target_f) * measured_f) /
                             (target_f * (1.0 - measured_f));
        double next_value = (double)w * scale;
        if (next_value < 1.0) {
            next_value = 1.0;
        }
        if (next_value > (double)INT_MAX) {
            break;
        }

        int next_w = (int)llround(next_value);
        if (next_w == w) {
            if (measured_f > target_f && w < INT_MAX) {
                next_w = w + 1;
            } else if (measured_f < target_f && w > 1) {
                next_w = w - 1;
            } else {
                break;
            }
        }
        if (next_w < 1) {
            next_w = 1;
        }
        if (next_w == w) {
            break;
        }

        w = next_w;
        tw_ns = measure_private_ns(w);
        measured_f = tc_ns / (tw_ns + tc_ns);
        const double error = fabs(measured_f - target_f);
        if (error < best_error) {
            best_error = error;
            best_w = w;
            best_tw_ns = tw_ns;
            best_f = measured_f;
        }
    }

    result.target_f = target_f;
    result.c = c;
    result.w = best_w;
    result.tc_ns = tc_ns;
    result.a_ns = a_ns;
    result.tw_ns = best_tw_ns;
    result.measured_f = best_f;
    result.serial_estimate_ns = tc_ns + best_tw_ns;
    result.within_tolerance = best_error <= 0.05;
    return result;
}

static uint64_t sum_histogram(const shared_t *s)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < NBUCKETS; i++) {
        sum += s->hist[i];
    }
    return sum;
}

static uint64_t sum_race_histogram(const race_shared_t *s)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < NBUCKETS; i++) {
        sum += s->hist[i];
    }
    return sum;
}

static void write_calibration_csv(const calibration_t *calibrations, size_t count)
{
    FILE *file = fopen("results/calibration.csv", "w");
    if (file == NULL) {
        perror("results/calibration.csv");
        exit(EXIT_FAILURE);
    }

    fprintf(file, "F,C,W,T_C_ns,a_ns,t_W_ns,F_measured,serial_estimate_ns,within_0_05\n");
    for (size_t i = 0; i < count; i++) {
        const calibration_t *p = &calibrations[i];
        fprintf(file, "%.1f,%d,%d,%.3f,%.3f,%.3f,%.6f,%.3f,%d\n",
                p->target_f, p->c, p->w, p->tc_ns, p->a_ns, p->tw_ns,
                p->measured_f, p->serial_estimate_ns,
                p->within_tolerance ? 1 : 0);
    }

    fclose(file);
}

static void print_calibration_table(const calibration_t *calibrations, size_t count)
{
    printf("\nCalibration results (C=64 is the main setting; C=16 is the extra test)\n");
    printf(" F     C       W       T_C(ns)       a(ns)      t_W(ns)   F_measured\n");
    printf("-----------------------------------------------------------------------\n");

    for (size_t i = 0; i < count; i++) {
        const calibration_t *p = &calibrations[i];
        printf(" %.1f   %2d   %6d   %10.2f   %10.2f   %10.2f    %.4f%s\n",
               p->target_f, p->c, p->w, p->tc_ns, p->a_ns, p->tw_ns,
               p->measured_f, p->within_tolerance ? "" : "  (outside +/-0.05)");
    }
}

static long available_cpu_count(void)
{
    /* The homework defines H using nproc, so prefer that exact command. */
    FILE *pipe = popen("nproc", "r");
    if (pipe != NULL) {
        char buffer[64];
        if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
            char *end = NULL;
            const long count = strtol(buffer, &end, 10);
            (void)pclose(pipe);
            if (end != buffer && count > 0) {
                return count;
            }
        } else {
            (void)pclose(pipe);
        }
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int count = CPU_COUNT(&set);
        if (count > 0) {
            return (long)count;
        }
    }

    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    return (count > 0) ? count : 1L;
}

static uint64_t greatest_common_divisor(uint64_t a, uint64_t b)
{
    while (b != 0ULL) {
        const uint64_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

static uint64_t least_common_multiple(uint64_t a, uint64_t b)
{
    return (a / greatest_common_divisor(a, b)) * b;
}

static uint64_t rounded_iterations(uint64_t requested, long h)
{
    /* Keep work evenly divisible for N = 2, 4, H, and the extra N = 16 run. */
    uint64_t divisor = least_common_multiple(2ULL, 4ULL);
    divisor = least_common_multiple(divisor, (uint64_t)h);
    divisor = least_common_multiple(divisor, (uint64_t)EXTRA_THREAD_COUNT);
    if (divisor == 0ULL) {
        return requested;
    }

    const uint64_t remainder = requested % divisor;
    if (remainder == 0ULL) {
        return requested;
    }

    return requested + (divisor - remainder);
}

static uint64_t choose_default_iterations(const calibration_t *calibrations,
                                          size_t count, long h)
{
    double fastest_ns = calibrations[0].serial_estimate_ns;

    for (size_t i = 1; i < count; i++) {
        if (calibrations[i].serial_estimate_ns < fastest_ns) {
            fastest_ns = calibrations[i].serial_estimate_ns;
        }
    }

    if (fastest_ns <= 0.0) {
        return rounded_iterations(1000000ULL, h);
    }

    const double required = TARGET_SERIAL_SECONDS * 1000000000.0 / fastest_ns;
    if (required >= (double)UINT64_MAX) {
        fprintf(stderr, "Automatic TOTAL_ITERS is too large; use --iters.\n");
        exit(EXIT_FAILURE);
    }

    uint64_t iters = (uint64_t)ceil(required);
    if (iters < 10000ULL) {
        iters = 10000ULL;
    }
    return rounded_iterations(iters, h);
}

static bool serial_reference(int w, int c, uint64_t total_iters,
                             shared_t *answer, double *elapsed_s,
                             event_measurement_t *events)
{
    shared_t s = {0};
    event_snapshot_t snapshot;
    event_snapshot_begin(&snapshot);

    const double start = now_seconds();
    const double deadline = start + RUN_TIMEOUT_SECONDS;
    bool timeout = false;

    for (uint64_t i = 0; i < total_iters; i++) {
        const uint64_t h = private_work(i, w);
        shared_work(&s, h, c);

        if ((i & 4095ULL) == 0ULL && now_seconds() > deadline) {
            timeout = true;
            break;
        }
    }

    event_snapshot_finish(&snapshot, events);
    *elapsed_s = now_seconds() - start;
    *answer = s;
    return !timeout && s.total == total_iters;
}

static void *no_lock_worker(void *pointer)
{
    worker_arg_t *arg = (worker_arg_t *)pointer;
    event_snapshot_t snapshot;
    const int barrier_result = pthread_barrier_wait(arg->barrier);
    if (barrier_result != 0 && barrier_result != PTHREAD_BARRIER_SERIAL_THREAD) {
        return NULL;
    }

    /* Begin per-thread kernel accounting immediately after the start barrier. */
    event_snapshot_begin(&snapshot);
    const double deadline = now_seconds() + RUN_TIMEOUT_SECONDS;
    uint64_t processed = 0;
    arg->timed_out = false;

    for (uint64_t i = (uint64_t)arg->tid; i < arg->total_iters;
         i += (uint64_t)arg->nthreads) {
        if ((processed & 4095ULL) == 0ULL && now_seconds() > deadline) {
            arg->timed_out = true;
            break;
        }

        const uint64_t h = private_work(i, arg->w);
        shared_work_no_lock(arg->shared, h, arg->c);
        processed++;
    }

    event_snapshot_finish(&snapshot, &arg->events);
    arg->processed = processed;
    return NULL;
}

static uint64_t expected_histogram_sum(const shared_t *expected)
{
    return sum_histogram(expected);
}

static race_result_t run_no_lock_once(int nthreads, int w, int c,
                                      uint64_t total_iters,
                                      const shared_t *expected)
{
    pthread_t *threads = calloc((size_t)nthreads, sizeof(*threads));
    worker_arg_t *args = calloc((size_t)nthreads, sizeof(*args));
    race_result_t result = {0};
    race_shared_t shared = {0};
    pthread_barrier_t barrier;

    if (threads == NULL || args == NULL) {
        perror("calloc");
        free(threads);
        free(args);
        exit(EXIT_FAILURE);
    }

    if (pthread_barrier_init(&barrier, NULL, (unsigned int)(nthreads + 1)) != 0) {
        fprintf(stderr, "Could not initialize pthread barrier.\n");
        free(threads);
        free(args);
        exit(EXIT_FAILURE);
    }

    for (int t = 0; t < nthreads; t++) {
        args[t].tid = t;
        args[t].nthreads = nthreads;
        args[t].w = w;
        args[t].c = c;
        args[t].total_iters = total_iters;
        args[t].processed = 0;
        args[t].timed_out = false;
        args[t].shared = &shared;
        args[t].barrier = &barrier;
        memset(&args[t].events, 0, sizeof(args[t].events));

        const int create_result = pthread_create(&threads[t], NULL, no_lock_worker, &args[t]);
        if (create_result != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(create_result));
            exit(EXIT_FAILURE);
        }
    }

    /* Main joins the same barrier so the clock starts after all workers are ready. */
    const int barrier_result = pthread_barrier_wait(&barrier);
    if (barrier_result != 0 && barrier_result != PTHREAD_BARRIER_SERIAL_THREAD) {
        fprintf(stderr, "pthread_barrier_wait failed.\n");
        exit(EXIT_FAILURE);
    }

    const double start = now_seconds();
    for (int t = 0; t < nthreads; t++) {
        const int join_result = pthread_join(threads[t], NULL);
        if (join_result != 0) {
            fprintf(stderr, "pthread_join failed: %s\n", strerror(join_result));
            exit(EXIT_FAILURE);
        }
    }
    const double end = now_seconds();

    result.time_s = end - start;
    result.timeout = false;
    result.events.task_clock_available = true;
    result.events.context_switches_available = true;
    for (int t = 0; t < nthreads; t++) {
        if (args[t].timed_out) result.timeout = true;

        if (args[t].events.task_clock_available) {
            result.events.task_clock_ns += args[t].events.task_clock_ns;
        } else {
            result.events.task_clock_available = false;
            if (result.events.task_clock_errno == 0)
                result.events.task_clock_errno = args[t].events.task_clock_errno;
        }

        if (args[t].events.context_switches_available) {
            result.events.context_switches += args[t].events.context_switches;
        } else {
            result.events.context_switches_available = false;
            if (result.events.context_switches_errno == 0)
                result.events.context_switches_errno = args[t].events.context_switches_errno;
        }
    }
    result.actual_total = shared.total;
    result.actual_hist_sum = sum_race_histogram(&shared);
    result.actual_max_score = shared.max_score;

    result.ops_min = UINT64_MAX;
    result.ops_max = 0;
    for (int t = 0; t < nthreads; t++) {
        if (args[t].processed < result.ops_min) {
            result.ops_min = args[t].processed;
        }
        if (args[t].processed > result.ops_max) {
            result.ops_max = args[t].processed;
        }
    }

    result.bucket_mismatches = 0;
    for (size_t b = 0; b < NBUCKETS; b++) {
        if (shared.hist[b] != expected->hist[b]) {
            result.bucket_mismatches++;
        }
    }

    result.ok = !result.timeout &&
                result.actual_total == expected->total &&
                result.actual_hist_sum == expected_histogram_sum(expected) &&
                result.bucket_mismatches == 0 &&
                result.actual_max_score == expected->max_score;

    (void)pthread_barrier_destroy(&barrier);
    free(threads);
    free(args);
    return result;
}

static int compare_int(const void *a, const void *b)
{
    const int x = *(const int *)a;
    const int y = *(const int *)b;
    return (x > y) - (x < y);
}

static size_t build_thread_counts(long h, int *counts, size_t capacity)
{
    /* Task 1 requires N = 2, 4 and H. N = 16 is an extra run requested
       for this version; when H = 8, it is also the assignment's 2H point. */
    const int candidates[4] = {2, 4, (int)h, EXTRA_THREAD_COUNT};
    size_t used = 0;

    for (size_t i = 0; i < 4; i++) {
        bool found = false;
        for (size_t j = 0; j < used; j++) {
            if (counts[j] == candidates[i]) {
                found = true;
                break;
            }
        }
        if (!found && used < capacity) {
            counts[used++] = candidates[i];
        }
    }

    qsort(counts, used, sizeof(counts[0]), compare_int);
    return used;
}

static void append_serial_reference(FILE *file, double f, int c, int w,
                                    int run, double elapsed,
                                    const shared_t *expected, bool ok,
                                    const event_measurement_t *events)
{
    fprintf(file,
            "%.1f,%d,%d,%d,%.6f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%s,",
            f, c, w, run, elapsed, expected->total,
            sum_histogram(expected), expected->max_score,
            ok ? "ok" : "timeout");
    if (events->task_clock_available) fprintf(file, "%.0f", (double)events->task_clock_ns);
    fprintf(file, ",CLOCK_THREAD_CPUTIME_ID,%s,%d,", event_status(events->task_clock_available),
            events->task_clock_errno);
    if (events->context_switches_available) fprintf(file, "%" PRIu64, events->context_switches);
    fprintf(file, ",getrusage_RUSAGE_THREAD,%s,%d\n",
            event_status(events->context_switches_available),
            events->context_switches_errno);
    fflush(file);
}

static double run_serial_repetitions(FILE *serial_csv, const calibration_t *cal,
                                     uint64_t total_iters, shared_t *expected)
{
    double times[DEFAULT_REPETITIONS];
    event_measurement_t events[DEFAULT_REPETITIONS];
    bool all_ok = true;

    for (int run = 1; run <= DEFAULT_REPETITIONS; run++) {
        bool ok = serial_reference(cal->w, cal->c, total_iters, expected,
                                  &times[run - 1], &events[run - 1]);
        if (!ok) {
            all_ok = false;
        }
        append_serial_reference(serial_csv, cal->target_f, cal->c, cal->w,
                                run, times[run - 1], expected, ok, &events[run - 1]);
        printf("  serial run %d: wall=%.3f s, ", run, times[run - 1]);
        print_event_values(&events[run - 1]);
        printf("\n");
    }

    const double median_time = median3(times[0], times[1], times[2]);
    printf("Serial reference: F=%.1f C=%d W=%d, median %.3f s%s\n",
           cal->target_f, cal->c, cal->w, median_time,
           all_ok ? "" : " (timeout: increase resources or reduce --iters)");
    return median_time;
}

static void write_nolock_summary(FILE *file, double f, int c, int w, int nthreads,
                                 const race_result_t results[DEFAULT_REPETITIONS],
                                 const shared_t *expected)
{
    const double median_time = median3(results[0].time_s,
                                       results[1].time_s,
                                       results[2].time_s);
    const uint64_t median_actual_total = median_u64(results[0].actual_total,
                                                     results[1].actual_total,
                                                     results[2].actual_total);
    const uint64_t median_actual_hist = median_u64(results[0].actual_hist_sum,
                                                    results[1].actual_hist_sum,
                                                    results[2].actual_hist_sum);
    const uint64_t median_actual_max = median_u64(results[0].actual_max_score,
                                                   results[1].actual_max_score,
                                                   results[2].actual_max_score);
    int ok_runs = 0;
    for (int i = 0; i < DEFAULT_REPETITIONS; i++) {
        if (results[i].ok) ok_runs++;
    }

    event_measurement_t events[DEFAULT_REPETITIONS];
    for (int i = 0; i < DEFAULT_REPETITIONS; i++) events[i] = results[i].events;
    const bool task_ok = all_task_clock_available(events, DEFAULT_REPETITIONS);
    const bool switches_ok =
        all_context_switches_available(events, DEFAULT_REPETITIONS);
    uint64_t task_values[DEFAULT_REPETITIONS] = {0};
    uint64_t switch_values[DEFAULT_REPETITIONS] = {0};
    for (int i = 0; i < DEFAULT_REPETITIONS; i++) {
        task_values[i] = events[i].task_clock_ns;
        switch_values[i] = events[i].context_switches;
    }

    fprintf(file,
            "nolock,%d,%.1f,%d,%d,%.6f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,%d,",
            nthreads, f, c, w, median_time, median_actual_total, median_actual_hist,
            median_actual_max, expected->total, sum_histogram(expected),
            expected->max_score, ok_runs, DEFAULT_REPETITIONS);
    if (task_ok) fprintf(file, "%" PRIu64, median_u64(task_values[0], task_values[1], task_values[2]));
    fprintf(file, ",CLOCK_THREAD_CPUTIME_ID,%s,", task_ok ? "ok" : "unavailable");
    if (switches_ok) fprintf(file, "%" PRIu64, median_u64(switch_values[0], switch_values[1], switch_values[2]));
    fprintf(file, ",getrusage_RUSAGE_THREAD,%s\n", switches_ok ? "ok" : "unavailable");
    fflush(file);

    printf("  median summary: wall=%.6f s, ", median_time);
    if (task_ok) {
        printf("task-clock=%.3f ms [CLOCK_THREAD_CPUTIME_ID fallback], ",
               (double)median_u64(task_values[0], task_values[1], task_values[2]) /
                   1000000.0);
    } else {
        printf("task-clock=unavailable, ");
    }
    if (switches_ok) {
        printf("context-switches=%" PRIu64
               " [getrusage fallback]\n",
               median_u64(switch_values[0], switch_values[1], switch_values[2]));
    } else {
        printf("context-switches=unavailable\n");
    }
}

static void print_usage(const char *program)
{
    printf("Usage: %s [--iters NUMBER]\n", program);
    printf("\n");
    printf("Runs Task 1 only: calibration and the no-lock race experiment.\n");
    printf("The no-lock thread counts include N=16 in addition to 2, 4 and H.\n");
    printf("Without --iters, TOTAL_ITERS is estimated so the fastest serial case\n");
    printf("should take about %.1f seconds. Runs use %d repetitions.\n",
           TARGET_SERIAL_SECONDS, DEFAULT_REPETITIONS);
}

int main(int argc, char **argv)
{
    const double target_fractions[2] = {0.5, 0.1};
    calibration_t calibrations[2];
    calibration_t run_configs[4];
    size_t calibration_count = 0;
    size_t run_config_count = 0;
    uint64_t requested_iters = 0;
    bool iters_were_set = false;
    const long h = available_cpu_count();
    int thread_counts[4];
    const size_t thread_count_size = build_thread_counts(h, thread_counts, 4);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            const unsigned long long parsed = strtoull(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' || parsed == 0ULL) {
                fprintf(stderr, "Invalid value for --iters.\n");
                return EXIT_FAILURE;
            }
            requested_iters = (uint64_t)parsed;
            iters_were_set = true;
            i++;
            continue;
        }
        fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (h < 4) {
        fprintf(stderr,
                "Warning: only %ld CPU(s) are visible to this process. The assignment recommends at least 4 hardware threads.\n",
                h);
    }

    if (mkdir("results", 0775) != 0 && errno != EEXIST) {
        perror("mkdir results");
        return EXIT_FAILURE;
    }

    printf("Task 1 only - Locks, Contention and Amdahl's Law\n");
    printf("Fallback measurement sources: task-clock = CLOCK_THREAD_CPUTIME_ID; ");
    printf("context-switches = getrusage(RUSAGE_THREAD) voluntary + involuntary deltas.\n");
    printf("Per-worker values are summed for each parallel run; wall time uses CLOCK_MONOTONIC.\n");
    printf("H (CPUs visible to this process) = %ld\n", h);
    printf("Thread counts to test:");
    for (size_t i = 0; i < thread_count_size; i++) {
        printf(" %d", thread_counts[i]);
    }
    printf("\n");

    /* The formal calibration is for the assignment's fixed C=64. */
    for (size_t f = 0; f < 2; f++) {
        calibrations[calibration_count] = calibrate_one(target_fractions[f], 64);
        calibration_count++;
    }

    write_calibration_csv(calibrations, calibration_count);
    print_calibration_table(calibrations, calibration_count);

    /* The C=16 sensitivity run keeps the W found for C=64. */
    for (size_t f = 0; f < calibration_count; f++) {
        run_configs[run_config_count] = calibrations[f];
        run_config_count++;

        run_configs[run_config_count] = calibrations[f];
        run_configs[run_config_count].c = 16;
        run_configs[run_config_count].tc_ns = measure_shared_ns(16);
        run_configs[run_config_count].tw_ns = measure_private_ns(calibrations[f].w);
        run_configs[run_config_count].measured_f =
            run_configs[run_config_count].tc_ns /
            (run_configs[run_config_count].tc_ns + run_configs[run_config_count].tw_ns);
        run_configs[run_config_count].serial_estimate_ns =
            run_configs[run_config_count].tc_ns + run_configs[run_config_count].tw_ns;
        run_configs[run_config_count].within_tolerance =
            fabs(run_configs[run_config_count].measured_f - calibrations[f].target_f) <= 0.05;
        run_config_count++;
    }

    FILE *sensitivity_csv = fopen("results/c16_sensitivity.csv", "w");
    if (sensitivity_csv == NULL) {
        perror("results/c16_sensitivity.csv");
        return EXIT_FAILURE;
    }
    fprintf(sensitivity_csv, "F_target,C,W_kept_from_C64,F_measured_at_C16\n");
    printf("\nC=16 sensitivity check (W is kept from the C=64 calibration):\n");
    for (size_t i = 0; i < run_config_count; i++) {
        if (run_configs[i].c == 16) {
            fprintf(sensitivity_csv, "%.1f,16,%d,%.6f\n",
                    run_configs[i].target_f, run_configs[i].w,
                    run_configs[i].measured_f);
            printf("  target F=%.1f, W=%d -> measured F at C=16: %.4f\n",
                   run_configs[i].target_f, run_configs[i].w,
                   run_configs[i].measured_f);
        }
    }
    fclose(sensitivity_csv);

    uint64_t total_iters = iters_were_set
                               ? rounded_iterations(requested_iters, h)
                               : choose_default_iterations(run_configs,
                                                           run_config_count, h);

    printf("\nTOTAL_ITERS = %" PRIu64 " (divisible by 2, 4, H and 16)\n",
           total_iters);
    if (iters_were_set) {
        printf("TOTAL_ITERS was supplied by the command line. Check the serial times against the 2-second requirement.\n");
    } else {
        printf("TOTAL_ITERS was estimated from calibration to target about %.1f s for the fastest serial case.\n",
               TARGET_SERIAL_SECONDS);
    }

    FILE *serial_csv = fopen("results/serial_reference.csv", "w");
    FILE *raw_csv = fopen("results/nolock_raw.csv", "w");
    FILE *summary_csv = fopen("results/nolock_summary.csv", "w");
    if (serial_csv == NULL || raw_csv == NULL || summary_csv == NULL) {
        perror("Could not open a results CSV file");
        if (serial_csv != NULL) fclose(serial_csv);
        if (raw_csv != NULL) fclose(raw_csv);
        if (summary_csv != NULL) fclose(summary_csv);
        return EXIT_FAILURE;
    }

    fprintf(serial_csv,
            "F,C,W,run,time_s,expected_total,expected_hist_sum,expected_max_score,status,task_clock_ns,task_clock_source,task_clock_status,task_clock_errno,context_switches,context_switches_source,context_switches_status,context_switches_errno\n");
    fprintf(raw_csv,
            "lock,N,F,C,W,run,time_s,ops_min,ops_max,expected_total,actual_total,expected_hist_sum,actual_hist_sum,expected_max_score,actual_max_score,bucket_mismatches,ok,status,task_clock_ns,task_clock_source,task_clock_status,task_clock_errno,context_switches,context_switches_source,context_switches_status,context_switches_errno\n");
    fprintf(summary_csv,
            "lock,N,F,C,W,median_time_s,median_actual_total,median_actual_hist_sum,median_actual_max_score,expected_total,expected_hist_sum,expected_max_score,ok_runs,total_runs,median_task_clock_ns,task_clock_source,task_clock_status,median_context_switches,context_switches_source,context_switches_status\n");

    printf("\nSerial references (three runs per F/C configuration):\n");

    for (size_t case_index = 0; case_index < run_config_count; case_index++) {
        const calibration_t *cal = &run_configs[case_index];
        shared_t expected = {0};
        const double serial_median = run_serial_repetitions(serial_csv, cal,
                                                             total_iters, &expected);

        if (serial_median < 2.0) {
            printf("  NOTE: this serial median is below 2 seconds; consider rerunning with a larger --iters value.\n");
        }
        if (serial_median >= RUN_TIMEOUT_SECONDS) {
            printf("  NOTE: this serial configuration reached the 60-second timeout.\n");
        }

        for (size_t n_index = 0; n_index < thread_count_size; n_index++) {
            const int nthreads = thread_counts[n_index];
            race_result_t runs[DEFAULT_REPETITIONS];

            printf("No-lock runs: F_target=%.1f C=%d W=%d F_measured=%.3f N=%d\n",
                   cal->target_f, cal->c, cal->w, cal->measured_f, nthreads);

            for (int run = 1; run <= DEFAULT_REPETITIONS; run++) {
                runs[run - 1] = run_no_lock_once(nthreads, cal->w, cal->c,
                                                  total_iters, &expected);
                const race_result_t *r = &runs[run - 1];
                fprintf(raw_csv,
                        "nolock,%d,%.1f,%d,%d,%d,%.6f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,%s,",
                        nthreads, cal->target_f, cal->c, cal->w, run, r->time_s,
                        r->ops_min, r->ops_max, expected.total,
                        r->actual_total, sum_histogram(&expected),
                        r->actual_hist_sum, expected.max_score,
                        r->actual_max_score, r->bucket_mismatches,
                        r->ok ? 1 : 0, r->timeout ? "timeout" : "completed");
                if (r->events.task_clock_available)
                    fprintf(raw_csv, "%.0f", (double)r->events.task_clock_ns);
                fprintf(raw_csv, ",CLOCK_THREAD_CPUTIME_ID,%s,%d,",
                        event_status(r->events.task_clock_available),
                        r->events.task_clock_errno);
                if (r->events.context_switches_available)
                    fprintf(raw_csv, "%" PRIu64, r->events.context_switches);
                fprintf(raw_csv, ",getrusage_RUSAGE_THREAD,%s,%d\n",
                        event_status(r->events.context_switches_available),
                        r->events.context_switches_errno);
                fflush(raw_csv);

                printf("  run %d: total=%" PRIu64 "/%" PRIu64
                       ", hist_sum=%" PRIu64 "/%" PRIu64
                       ", wall=%.3f s, ok=%d%s, ",
                       run, r->actual_total, expected.total,
                       r->actual_hist_sum, sum_histogram(&expected),
                       r->time_s, r->ok ? 1 : 0,
                       r->timeout ? " (timeout)" : "");
                print_event_values(&r->events);
                printf("\n");
            }

            write_nolock_summary(summary_csv, cal->target_f, cal->c, cal->w,
                                 nthreads, runs, &expected);
        }
    }

    fclose(serial_csv);
    fclose(raw_csv);
    fclose(summary_csv);

    printf("\nTask 1 finished. CSV files are in ./results/\n");
    printf("No lock was used in the race experiment. No Task 2+ code is included.\n");
    return EXIT_SUCCESS;
}
