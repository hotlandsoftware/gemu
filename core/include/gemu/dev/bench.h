/*
 * Benchmark window and profile, for any machine's CPU.  (.work/modular-devices,
 * stage G7: the i2000's GEMU_BENCH_* and GEMU_PROFILE, made generic.)
 *
 * Benchmark: GEMU_BENCH_NINSTS=<N> [GEMU_BENCH_WARMUP=<W>] measures the
 * instruction rate from count W to count N (the counter is the CPU's own,
 * from ops.count) and prints
 *   gemu-bench: window=[W',N'] ninsts_delta=D elapsed=Ts <unit>/sec=R<extra>
 * where ops.report appends the machine's extras (a fingerprint, say).  The
 * warm-up exists because a CPU's one-shot fast paths can jump the count by
 * millions in one step: start the window past them.
 *
 * Profile: GEMU_PROFILE=1 prints, once a host second,
 *   profile: t=T R M<unit>/s idle I% ip=PC
 * and every ten seconds the six hottest 64 KiB code regions,
 *   profile-ip: t=T XXXXXXXXXXXXxxxx:P% ...
 * from the samples given to gemu_profile_sample() (one per poll quantum is
 * plenty).  "idle" is the share of samples the machine marked idle.
 *
 * The environment is read once, by gemu_bench_new().
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct GemuBench GemuBench;

typedef struct {
    const char *unit;                          /* "slots", "instructions" */
    uint64_t  (*count)(void *ud);              /* instructions so far */
    uint64_t  (*pc)(void *ud);                 /* for the profile line */
    /* Optional: append to the benchmark line (starts after the rate). */
    void      (*report)(void *ud, FILE *f, double per_sec);
    void       *ud;
} GemuBenchOps;

GemuBench *gemu_bench_new(const GemuBenchOps *ops);
void       gemu_bench_free(GemuBench *b);

/* The window (0: no benchmark). */
uint64_t gemu_bench_warmup(const GemuBench *b);
uint64_t gemu_bench_target(const GemuBench *b);
/* Between runs: opens the window at the warm-up count; at the target prints
 * the line (to stdout) and returns true - the machine then stops. */
bool     gemu_bench_check(GemuBench *b);

bool     gemu_profile_on(const GemuBench *b);
/* A sample: where the CPU is, and whether it's in an idle loop. */
void     gemu_profile_sample(GemuBench *b, uint64_t pc, bool idle);
/* Often (each poll quantum): prints when a host second has passed. */
void     gemu_profile_tick(GemuBench *b);
