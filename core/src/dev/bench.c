/* Benchmark window and profile: see gemu/dev/bench.h. */
#include "gemu/dev/bench.h"
#include "gemu/dev/scheduler.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define HIST 4096           /* 64 KiB code regions, open addressing */

struct GemuBench {
    GemuBenchOps ops;
    uint64_t warmup, target;
    bool     window_open;
    uint64_t window_count, window_ns;
    bool     profile;
    uint64_t samples, idle;
    uint64_t start_ns, last_ns, last_count;
    unsigned ticks;
    struct { uint64_t key, n; } hist[HIST];
};

static uint64_t env_u64(const char *name) {
    const char *v = getenv(name);
    return v ? strtoull(v, NULL, 0) : 0;
}

GemuBench *gemu_bench_new(const GemuBenchOps *ops) {
    if (!ops || !ops->count || !ops->pc)
        return NULL;
    GemuBench *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->ops = *ops;
    if (!b->ops.unit)
        b->ops.unit = "instructions";
    b->warmup = env_u64("GEMU_BENCH_WARMUP");
    b->target = env_u64("GEMU_BENCH_NINSTS");
    const char *p = getenv("GEMU_PROFILE");
    b->profile = p && strcmp(p, "0") != 0;
    return b;
}

void gemu_bench_free(GemuBench *b) { free(b); }
uint64_t gemu_bench_warmup(const GemuBench *b) { return b->warmup; }
uint64_t gemu_bench_target(const GemuBench *b) { return b->target; }
bool gemu_profile_on(const GemuBench *b) { return b->profile; }

bool gemu_bench_check(GemuBench *b) {
    if (!b->target)
        return false;
    uint64_t n = b->ops.count(b->ops.ud);
    if (!b->window_open) {
        if (n < b->warmup)
            return false;
        b->window_open = true;
        b->window_count = n;
        b->window_ns = gemu_host_ns();
    }
    if (n < b->target)
        return false;
    uint64_t delta = n - b->window_count;
    double elapsed = (double)(gemu_host_ns() - b->window_ns) / 1e9;
    double rate = elapsed > 0 ? (double)delta / elapsed : 0;
    printf("gemu-bench: window=[%" PRIu64 ",%" PRIu64 "] ninsts_delta=%" PRIu64
           " elapsed=%.3fs %s/sec=%.0f", b->window_count, n, delta, elapsed,
           b->ops.unit, rate);
    if (b->ops.report)
        b->ops.report(b->ops.ud, stdout, rate);
    printf("\n");
    fflush(stdout);
    return true;
}

void gemu_profile_sample(GemuBench *b, uint64_t pc, bool idle) {
    if (!b->profile)
        return;
    b->samples++;
    b->idle += idle;
    uint64_t key = (pc >> 16) | 1;
    unsigned h = (unsigned)((key * UINT64_C(0x9E3779B97F4A7C15)) >> 52);
    for (unsigned k = 0; k < HIST; k++, h = (h + 1) & (HIST - 1)) {
        if (b->hist[h].key == key) { b->hist[h].n++; return; }
        if (!b->hist[h].key) { b->hist[h].key = key; b->hist[h].n = 1; return; }
    }
}

static void hist_dump(GemuBench *b, double t) {
    uint64_t total = 0;
    for (unsigned i = 0; i < HIST; i++)
        total += b->hist[i].n;
    if (!total)
        return;
    fprintf(stderr, "profile-ip: t=%6.1f", t);
    for (int k = 0; k < 6; k++) {
        unsigned best = 0;
        for (unsigned i = 1; i < HIST; i++)
            if (b->hist[i].n > b->hist[best].n)
                best = i;
        if (!b->hist[best].n)
            break;
        fprintf(stderr, " %012" PRIX64 "xxxx:%.0f%%", b->hist[best].key & ~UINT64_C(1),
                100.0 * (double)b->hist[best].n / (double)total);
        b->hist[best].n = 0;
    }
    fprintf(stderr, "\n");
    memset(b->hist, 0, sizeof b->hist);
}

void gemu_profile_tick(GemuBench *b) {
    if (!b->profile)
        return;
    uint64_t now = gemu_host_ns();
    uint64_t n = b->ops.count(b->ops.ud);
    if (!b->last_ns) {
        b->start_ns = b->last_ns = now;
        b->last_count = n;
        return;
    }
    if (now - b->last_ns < UINT64_C(1000000000))
        return;
    double dt = (double)(now - b->last_ns) / 1e9;
    double t = (double)(now - b->start_ns) / 1e9;
    fprintf(stderr, "profile: t=%6.1f %7.2f M%s/s idle %5.1f%% ip=%016" PRIX64 "\n", t,
            (double)(n - b->last_count) / dt / 1e6, b->ops.unit,
            b->samples ? 100.0 * (double)b->idle / (double)b->samples : 0.0,
            b->ops.pc(b->ops.ud));
    if (++b->ticks % 10 == 0)
        hist_dump(b, t);
    b->last_ns = now;
    b->last_count = n;
    b->samples = b->idle = 0;
}
