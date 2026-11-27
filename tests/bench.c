/* Benchmark window (core/src/dev/bench.c).  Run: ninja check-bench */
#include "gemu/dev/bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
static int setenv(const char *k, const char *v, int o) { (void)o; return _putenv_s(k, v); }
static int unsetenv(const char *k) { return _putenv_s(k, ""); }
#endif

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static uint64_t counter, reports;
static double reported_rate;
static uint64_t count(void *ud) { (void)ud; return counter; }
static uint64_t pc(void *ud) { (void)ud; return 0x1234; }
static void report(void *ud, FILE *f, double per_sec) {
    (void)ud;
    reports++;
    reported_rate = per_sec;
    fprintf(f, " extra=1");
}

int main(void) {
    GemuBenchOps ops = { .unit = "slots", .count = count, .pc = pc, .report = report };
    /* no environment: no window, no profile */
    unsetenv("GEMU_BENCH_NINSTS");
    unsetenv("GEMU_BENCH_WARMUP");
    unsetenv("GEMU_PROFILE");
    GemuBench *b = gemu_bench_new(&ops);
    CHECK(b && !gemu_bench_target(b) && !gemu_profile_on(b));
    counter = 1u << 30;
    CHECK(!gemu_bench_check(b));
    gemu_profile_sample(b, 0, true);                  /* off: nothing happens */
    gemu_profile_tick(b);
    gemu_bench_free(b);
    CHECK(!gemu_bench_new(NULL));
    GemuBenchOps no_count = ops;
    no_count.count = NULL;
    CHECK(!gemu_bench_new(&no_count));

    /* a window from 1000 to 5000, read once at creation */
    setenv("GEMU_BENCH_WARMUP", "1000", 1);
    setenv("GEMU_BENCH_NINSTS", "0x1388", 1);          /* 5000 */
    setenv("GEMU_PROFILE", "1", 1);
    b = gemu_bench_new(&ops);
    setenv("GEMU_BENCH_NINSTS", "1", 1);               /* too late: ignored */
    CHECK(gemu_bench_warmup(b) == 1000 && gemu_bench_target(b) == 5000);
    CHECK(gemu_profile_on(b));
    counter = 10;
    CHECK(!gemu_bench_check(b));                       /* before the warm-up */
    counter = 1500;
    CHECK(!gemu_bench_check(b));                       /* the window opens here */
    counter = 4999;
    CHECK(!gemu_bench_check(b));
    CHECK(reports == 0);
    for (int i = 0; i < 100; i++)
        gemu_profile_sample(b, (uint64_t)i << 16, i & 1);
    gemu_profile_tick(b);                              /* the first only starts the clock */
    counter = 6000;
    CHECK(gemu_bench_check(b));                        /* the line: window [1500,6000] */
    CHECK(reports == 1 && reported_rate >= 0);
    gemu_bench_free(b);
    printf("bench: %u checks passed\n", checks);
    return 0;
}
