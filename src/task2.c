#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <signal.h>
#include <sys/wait.h>

/* -------------------------------------------------------------------------
 * Task 2: Peterson's lock (two threads)
 *
 * This is a standalone Task 2 program. Running it executes only the
 * Peterson stress test only; no other benchmark is part of this source file.
 *
 * The three Peterson states deliberately differ only in the synchronization
 * variables: ordinary int (A), volatile int (B), and C11 atomic_int with
 * memory_order_seq_cst (C).
 * ------------------------------------------------------------------------- */

#define TASK2_DEFAULT_ITERS_PER_THREAD 1000000ULL
#define TASK2_DEFAULT_REPETITIONS 10
#define TASK2_DEFAULT_TIMEOUT_SECONDS 60.0
#define TASK2_POLL_NANOSECONDS 10000000L

typedef enum {
    TASK2_PLAIN = 0,
    TASK2_VOLATILE = 1,
    TASK2_ATOMIC = 2
} task2_variant_t;

typedef struct {
    int occupied[2];
    int last;
} peterson_plain_t;

typedef struct {
    volatile int occupied[2];
    volatile int last;
} peterson_volatile_t;

typedef struct {
    atomic_int occupied[2];
    atomic_int last;
} peterson_atomic_t;

typedef union {
    peterson_plain_t plain;
    peterson_volatile_t vol;
    peterson_atomic_t fixed;
} peterson_state_t;

typedef struct {
    peterson_state_t lock;
    /*
     * Keep each increment as an observable load/store in all three tests.
     * volatile does NOT make ++ atomic and does NOT provide synchronization;
     * only the Peterson implementation can provide mutual exclusion.
     * This makes lost increments visible if a deliberately broken lock lets
     * both threads enter the critical section.
     */
    volatile uint64_t total;
} task2_shared_t;

typedef struct {
    task2_shared_t *shared;
    pthread_barrier_t *start_barrier;
    task2_variant_t variant;
    int tid;
    uint64_t iterations;
} task2_worker_arg_t;

typedef struct {
    uint64_t actual_total;
    double elapsed_s;
    int error_code;
    int completed;
} task2_child_result_t;

typedef enum {
    TASK2_RESULT_PASS,
    TASK2_RESULT_INCORRECT,
    TASK2_RESULT_TIMEOUT,
    TASK2_RESULT_CRASH,
    TASK2_RESULT_ERROR
} task2_result_status_t;

typedef struct {
    task2_result_status_t status;
    uint64_t actual_total;
    double elapsed_s;
    int error_code;
    int child_exit_code;
    int child_signal;
} task2_run_result_t;

static const char *task2_variant_name(task2_variant_t variant)
{
    switch (variant) {
    case TASK2_PLAIN: return "A_plain";
    case TASK2_VOLATILE: return "B_volatile";
    case TASK2_ATOMIC: return "C_seq_cst";
    }
    return "unknown";
}

static const char *task2_status_name(task2_result_status_t status)
{
    switch (status) {
    case TASK2_RESULT_PASS: return "pass";
    case TASK2_RESULT_INCORRECT: return "incorrect";
    case TASK2_RESULT_TIMEOUT: return "timeout";
    case TASK2_RESULT_CRASH: return "crash";
    case TASK2_RESULT_ERROR: return "error";
    }
    return "unknown";
}

static const char *task2_error_name(int error_code)
{
    if (error_code == 0) return "none";
    if (error_code == ETIMEDOUT) return "ETIMEDOUT";
    if (error_code == EIO) return "EIO";
    return "system_error";
}

/* Variant A: ordinary int state. With -O2 the compiler may hoist a spin read. */
static inline void peterson_plain_lock(peterson_plain_t *lock, int self)
{
    const int other = 1 - self;
    lock->occupied[self] = 1;
    lock->last = other;
    while (lock->occupied[other] && lock->last == other) {
        /* Deliberately empty: do not add volatile or atomic semantics here. */
    }
}

static inline void peterson_plain_unlock(peterson_plain_t *lock, int self)
{
    lock->occupied[self] = 0;
}

/* Variant B: volatile forces accesses to occur, but does not fence the CPU. */
static inline void peterson_volatile_lock(peterson_volatile_t *lock, int self)
{
    const int other = 1 - self;
    lock->occupied[self] = 1;
    lock->last = other;
    while (lock->occupied[other] && lock->last == other) {
        /* Volatile accesses are not inter-thread synchronization. */
    }
}

static inline void peterson_volatile_unlock(peterson_volatile_t *lock, int self)
{
    lock->occupied[self] = 0;
}

/* Variant C: every Peterson-state access is sequentially consistent. */
static inline void peterson_atomic_lock(peterson_atomic_t *lock, int self)
{
    const int other = 1 - self;
    atomic_store_explicit(&lock->occupied[self], 1, memory_order_seq_cst);
    atomic_store_explicit(&lock->last, other, memory_order_seq_cst);
    while (atomic_load_explicit(&lock->occupied[other], memory_order_seq_cst) &&
           atomic_load_explicit(&lock->last, memory_order_seq_cst) == other) {
        /* Sequentially consistent atomic loads. */
    }
}

static inline void peterson_atomic_unlock(peterson_atomic_t *lock, int self)
{
    atomic_store_explicit(&lock->occupied[self], 0, memory_order_seq_cst);
}

static void *task2_worker(void *opaque)
{
    task2_worker_arg_t *arg = (task2_worker_arg_t *)opaque;
    const int barrier_status = pthread_barrier_wait(arg->start_barrier);
    if (barrier_status != 0 && barrier_status != PTHREAD_BARRIER_SERIAL_THREAD) {
        return NULL;
    }

    switch (arg->variant) {
    case TASK2_PLAIN:
        for (uint64_t i = 0; i < arg->iterations; ++i) {
            peterson_plain_lock(&arg->shared->lock.plain, arg->tid);
            arg->shared->total++;
            peterson_plain_unlock(&arg->shared->lock.plain, arg->tid);
        }
        break;
    case TASK2_VOLATILE:
        for (uint64_t i = 0; i < arg->iterations; ++i) {
            peterson_volatile_lock(&arg->shared->lock.vol, arg->tid);
            arg->shared->total++;
            peterson_volatile_unlock(&arg->shared->lock.vol, arg->tid);
        }
        break;
    case TASK2_ATOMIC:
        for (uint64_t i = 0; i < arg->iterations; ++i) {
            peterson_atomic_lock(&arg->shared->lock.fixed, arg->tid);
            arg->shared->total++;
            peterson_atomic_unlock(&arg->shared->lock.fixed, arg->tid);
        }
        break;
    }

    return NULL;
}

static double task2_now_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

/* Runs only inside the disposable child process. */
static void task2_child_execute(task2_variant_t variant,
                                uint64_t iterations_per_thread,
                                task2_child_result_t *result)
{
    task2_shared_t shared;
    pthread_barrier_t barrier;
    pthread_t threads[2];
    task2_worker_arg_t args[2];
    int rc;

    memset(&shared, 0, sizeof(shared));
    shared.total = 0;
    if (variant == TASK2_ATOMIC) {
        atomic_init(&shared.lock.fixed.occupied[0], 0);
        atomic_init(&shared.lock.fixed.occupied[1], 0);
        atomic_init(&shared.lock.fixed.last, 0);
    }

    rc = pthread_barrier_init(&barrier, NULL, 3);
    if (rc != 0) {
        result->error_code = rc;
        return;
    }

    for (int tid = 0; tid < 2; ++tid) {
        args[tid].shared = &shared;
        args[tid].start_barrier = &barrier;
        args[tid].variant = variant;
        args[tid].tid = tid;
        args[tid].iterations = iterations_per_thread;
        rc = pthread_create(&threads[tid], NULL, task2_worker, &args[tid]);
        if (rc != 0) {
            /* The child exits immediately; any already-created worker dies with it. */
            result->error_code = rc;
            return;
        }
    }

    rc = pthread_barrier_wait(&barrier);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        result->error_code = rc;
        return;
    }

    const double start = task2_now_seconds();
    if (start < 0.0) {
        result->error_code = errno ? errno : EIO;
        return;
    }

    for (int tid = 0; tid < 2; ++tid) {
        rc = pthread_join(threads[tid], NULL);
        if (rc != 0) {
            result->error_code = rc;
            return;
        }
    }

    const double end = task2_now_seconds();
    if (end < 0.0) {
        result->error_code = errno ? errno : EIO;
        return;
    }

    result->elapsed_s = end - start;
    result->actual_total = shared.total;
    result->completed = 1;
    (void)pthread_barrier_destroy(&barrier);
}

static bool task2_write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *p = (const unsigned char *)buffer;
    size_t written = 0;
    while (written < length) {
        const ssize_t n = write(fd, p + written, length - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        written += (size_t)n;
    }
    return true;
}

static bool task2_read_all(int fd, void *buffer, size_t length)
{
    unsigned char *p = (unsigned char *)buffer;
    size_t received = 0;
    while (received < length) {
        const ssize_t n = read(fd, p + received, length - received);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        received += (size_t)n;
    }
    return true;
}

/*
 * A plain/volatile Peterson test may hang. Isolate each repetition in a child
 * process so the watchdog can classify the hang as incorrect and continue.
 */
static task2_run_result_t task2_run_one(task2_variant_t variant,
                                        uint64_t iterations_per_thread,
                                        double timeout_seconds)
{
    task2_run_result_t result;
    int pipefd[2];
    pid_t pid;
    int wait_status = 0;
    bool exited = false;
    const double parent_start = task2_now_seconds();
    task2_child_result_t child_result;

    memset(&result, 0, sizeof(result));
    memset(&child_result, 0, sizeof(child_result));
    result.status = TASK2_RESULT_ERROR;

    if (pipe(pipefd) != 0) {
        result.error_code = errno;
        return result;
    }

    pid = fork();
    if (pid < 0) {
        result.error_code = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        return result;
    }

    if (pid == 0) {
        close(pipefd[0]);
        task2_child_execute(variant, iterations_per_thread, &child_result);
        (void)task2_write_all(pipefd[1], &child_result, sizeof(child_result));
        close(pipefd[1]);
        _exit(child_result.error_code == 0 && child_result.completed ? 0 : 2);
    }

    close(pipefd[1]);
    while (!exited) {
        const pid_t waited = waitpid(pid, &wait_status, WNOHANG);
        if (waited == pid) {
            exited = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            result.error_code = errno;
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &wait_status, 0);
            close(pipefd[0]);
            return result;
        }

        const double now = task2_now_seconds();
        if (now < 0.0 || now - parent_start >= timeout_seconds) {
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &wait_status, 0);
            result.status = TASK2_RESULT_TIMEOUT;
            result.elapsed_s = (now >= parent_start && parent_start >= 0.0)
                                   ? now - parent_start
                                   : timeout_seconds;
            /* The watchdog, not the child, ended this run. */
            result.error_code = ETIMEDOUT;
            result.child_exit_code = -1; /* no normal exit code exists */
            result.child_signal = SIGKILL;
            close(pipefd[0]);
            return result;
        }

        struct timespec pause_time = {0, TASK2_POLL_NANOSECONDS};
        (void)nanosleep(&pause_time, NULL);
    }

    if (WIFSIGNALED(wait_status)) {
        result.status = TASK2_RESULT_CRASH;
        result.child_exit_code = -1; /* terminated by a signal, not exit() */
        result.child_signal = WTERMSIG(wait_status);
        close(pipefd[0]);
        return result;
    }

    if (!task2_read_all(pipefd[0], &child_result, sizeof(child_result))) {
        result.status = TASK2_RESULT_CRASH;
        result.child_exit_code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
        close(pipefd[0]);
        return result;
    }
    close(pipefd[0]);

    result.actual_total = child_result.actual_total;
    result.elapsed_s = child_result.elapsed_s;
    result.error_code = child_result.error_code;
    result.child_exit_code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;

    if (child_result.error_code != 0 || !child_result.completed ||
        !WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        result.status = TASK2_RESULT_ERROR;
    } else {
        const uint64_t expected = iterations_per_thread * 2ULL;
        result.status = (child_result.actual_total == expected)
                            ? TASK2_RESULT_PASS
                            : TASK2_RESULT_INCORRECT;
    }
    return result;
}

static void task2_print_usage(const char *program)
{
    printf("Usage: %s [--iterations-per-thread N] [--timeout-seconds S]\n",
           program);
    printf("Defaults: %llu iterations per thread, %d repetitions per variant, %.1f s per-run watchdog.\n",
           (unsigned long long)TASK2_DEFAULT_ITERS_PER_THREAD,
           TASK2_DEFAULT_REPETITIONS, TASK2_DEFAULT_TIMEOUT_SECONDS);
}

static int run_task2(int argc, char **argv)
{
    const task2_variant_t variants[3] = {TASK2_PLAIN, TASK2_VOLATILE, TASK2_ATOMIC};
    uint64_t iterations_per_thread = TASK2_DEFAULT_ITERS_PER_THREAD;
    double timeout_seconds = TASK2_DEFAULT_TIMEOUT_SECONDS;
    const uint64_t max_iterations = UINT64_MAX / 2ULL;

    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            task2_print_usage("./build/task2");
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--iterations-per-thread") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            const unsigned long long value = strtoull(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' || value == 0ULL ||
                value > max_iterations) {
                fprintf(stderr, "Invalid --iterations-per-thread value.\n");
                return EXIT_FAILURE;
            }
            iterations_per_thread = (uint64_t)value;
        } else if (strcmp(argv[i], "--timeout-seconds") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            const double value = strtod(argv[++i], &end);
            if (errno != 0 || end == argv[i] || *end != '\0' ||
                !isfinite(value) || value <= 0.0) {
                fprintf(stderr, "Invalid --timeout-seconds value.\n");
                return EXIT_FAILURE;
            }
            timeout_seconds = value;
        } else {
            fprintf(stderr, "Unknown or incomplete Task 2 option: %s\n", argv[i]);
            task2_print_usage("./build/task2");
            return EXIT_FAILURE;
        }
    }

    if (mkdir("results", 0775) != 0 && errno != EEXIST) {
        perror("mkdir results");
        return EXIT_FAILURE;
    }

    FILE *raw = fopen("results/task2_peterson_raw.csv", "w");
    FILE *summary = fopen("results/task2_peterson_summary.csv", "w");
    if (raw == NULL || summary == NULL) {
        perror("Could not open Task 2 CSV output");
        if (raw != NULL) fclose(raw);
        if (summary != NULL) fclose(summary);
        return EXIT_FAILURE;
    }

    fprintf(raw, "variant,run,iterations_per_thread,expected_total,actual_total,elapsed_s,status,error_code,error_name,child_exit_code,child_signal,termination\n");
    fprintf(summary, "variant,iterations_per_thread,expected_total,repetitions,passed,incorrect,timeouts,crashes,errors,all_10_observed_runs_correct\n");

    const uint64_t expected_total = iterations_per_thread * 2ULL;
    printf("Task 2: Peterson's lock stress test\n");
    printf("CSV format: timeout rows use error_code=ETIMEDOUT, child_exit_code=-1, child_signal=SIGKILL.\n");
    printf("Two threads; %" PRIu64 " increments per thread; expected final total=%" PRIu64 ".\n",
           iterations_per_thread, expected_total);
    printf("Repetitions per variant=%d; watchdog=%.2f s per run.\n",
           TASK2_DEFAULT_REPETITIONS, timeout_seconds);
    printf("Planned tests: 3 variants x %d repetitions = %d tests.\n",
           TASK2_DEFAULT_REPETITIONS, 3 * TASK2_DEFAULT_REPETITIONS);
    printf("A hang or timeout is classified as incorrect; timeout is per run.\n\n");

    int total_tests = 0;
    for (size_t v = 0; v < 3; ++v) {
        int passed = 0;
        int incorrect = 0;
        int timeouts = 0;
        int crashes = 0;
        int errors = 0;

        printf("Variant %s (%s):\n", task2_variant_name(variants[v]),
               v == 0 ? "ordinary int" : (v == 1 ? "volatile int" : "C11 seq_cst atomics"));

        for (int run = 1; run <= TASK2_DEFAULT_REPETITIONS; ++run) {
            const task2_run_result_t r = task2_run_one(variants[v],
                                                       iterations_per_thread,
                                                       timeout_seconds);
            total_tests++;
            const char *status = task2_status_name(r.status);
            if (r.status == TASK2_RESULT_PASS) passed++;
            else if (r.status == TASK2_RESULT_INCORRECT) incorrect++;
            else if (r.status == TASK2_RESULT_TIMEOUT) { timeouts++; incorrect++; }
            else if (r.status == TASK2_RESULT_CRASH) { crashes++; incorrect++; }
            else errors++;

            if (r.status == TASK2_RESULT_TIMEOUT) {
                fprintf(raw,
                        "%s,%d,%" PRIu64 ",%" PRIu64 ",,%.6f,%s,%d,%s,%d,%d,watchdog_killed\n",
                        task2_variant_name(variants[v]), run, iterations_per_thread,
                        expected_total, r.elapsed_s, status, r.error_code,
                        task2_error_name(r.error_code), r.child_exit_code,
                        r.child_signal);
                printf("  run %2d: %s (%.2f s; ETIMEDOUT=%d; child_exit_code=N/A; SIGKILL=%d)\n",
                       run, status, timeout_seconds, r.error_code, r.child_signal);
            } else if (r.status == TASK2_RESULT_CRASH || r.status == TASK2_RESULT_ERROR) {
                const char *termination = r.child_signal != 0 ? "signal" : "child_error";
                fprintf(raw,
                        "%s,%d,%" PRIu64 ",%" PRIu64 ",,%.6f,%s,%d,%s,%d,%d,%s\n",
                        task2_variant_name(variants[v]), run, iterations_per_thread,
                        expected_total, r.elapsed_s, status, r.error_code,
                        task2_error_name(r.error_code), r.child_exit_code,
                        r.child_signal, termination);
                printf("  run %2d: %s (error=%d, child_exit=%d, signal=%d)\n",
                       run, status, r.error_code, r.child_exit_code, r.child_signal);
            } else {
                fprintf(raw,
                        "%s,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f,%s,0,none,0,0,normal_exit\n",
                        task2_variant_name(variants[v]), run, iterations_per_thread,
                        expected_total, r.actual_total, r.elapsed_s, status);
                printf("  run %2d: %s, final=%" PRIu64 "/%" PRIu64 ", time=%.3f s\n",
                       run, status, r.actual_total, expected_total, r.elapsed_s);
            }
            fflush(raw);
        }

        const bool all_correct = passed == TASK2_DEFAULT_REPETITIONS;
        fprintf(summary, "%s,%" PRIu64 ",%" PRIu64 ",%d,%d,%d,%d,%d,%d,%d\n",
                task2_variant_name(variants[v]), iterations_per_thread,
                expected_total, TASK2_DEFAULT_REPETITIONS, passed, incorrect,
                timeouts, crashes, errors, all_correct ? 1 : 0);
        fflush(summary);

        printf("  summary: passed %d/%d; incorrect=%d (timeouts=%d, crashes=%d); errors=%d; all ten correct=%s\n\n",
               passed, TASK2_DEFAULT_REPETITIONS, incorrect, timeouts, crashes,
               errors, all_correct ? "yes" : "no");
    }

    fclose(raw);
    fclose(summary);
    printf("\nTotal test records written: %d/%d.\n",
           total_tests, 3 * TASK2_DEFAULT_REPETITIONS);
    if (total_tests != 3 * TASK2_DEFAULT_REPETITIONS) {
        fprintf(stderr, "ERROR: expected exactly 30 test records.\n");
        return EXIT_FAILURE;
    }
    printf("Task 2 completed. Results: results/task2_peterson_raw.csv and results/task2_peterson_summary.csv\n");
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    return run_task2(argc - 1, argv + 1);
}
