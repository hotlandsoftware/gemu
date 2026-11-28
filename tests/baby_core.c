/* The Manchester Baby on the core (stage O1), with a stand-in program in
 * its store: count line 20 up to line 23's limit (600, about 5,400
 * instructions, five seconds), then STP.  Run is switched on at frame 1;
 * at frame 400, after the stop, Stop and Run again carry on (and stop
 * again at once); at 450 KC obeys one instruction while stopped.  Built
 * with -DO_HEAD against HEAD's driver it runs HEAD's frame.
 * Run: ninja check-baby_core */
#include "../hardware/machine/other/machine_baby.c"
#include "gemu/sha256.h"

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

#ifdef O_HEAD
#define M(s) (&(s)->m)
static void tick(BabyState *s) { baby_run_frame(s); baby_render(s); }
#else
#define M(s) ((s)->m)
static void tick(BabyState *s) { baby_frame(s); baby_render(s); }
void baby_core_checks(void);
#endif

static void hash(const char *label, const void *p, size_t n) {
    uint8_t d[GEMU_SHA256_DIGEST_LEN];
    GemuSha256Ctx c;
    gemu_sha256_init(&c);
    gemu_sha256_update(&c, p, n);
    gemu_sha256_final(&c, d);
    char hex[65];
    gemu_sha256_hex(d, hex);
    printf(" %s %.12s", label, hex);
}

static void report(BabyState *s, int frames) {
    printf("%4d", frames);
    hash("store", M(s)->store, sizeof M(s)->store);
    hash("fb", s->fb, sizeof s->fb);
    printf(" A %08X CI %08X PI %08X stopped %d run %d steps %llu line20 %u\n", (unsigned)M(s)->a,
           (unsigned)M(s)->ci, (unsigned)M(s)->pi, M(s)->stopped, s->run,
           (unsigned long long)M(s)->steps, (unsigned)M(s)->store[20]);
}

/* The program, from line 1 (CI starts at 0). */
static void load_program(BabyState *s) {
    static const uint32_t prog[] = {
        [1] = SSEM_INSN(SSEM_LDN, 20),   /* A = -count */
        [2] = SSEM_INSN(SSEM_SUB, 21),   /* A = -(count + 1) */
        [3] = SSEM_INSN(SSEM_STO, 22),
        [4] = SSEM_INSN(SSEM_LDN, 22),   /* A = count + 1 */
        [5] = SSEM_INSN(SSEM_STO, 20),
        [6] = SSEM_INSN(SSEM_SUB, 23),   /* A = count + 1 - limit */
        [7] = SSEM_INSN(SSEM_CMP, 0),    /* negative: skip the stop */
        [8] = SSEM_INSN(SSEM_STP, 0),
        [9] = SSEM_INSN(SSEM_JMP, 24),   /* CI = 0: line 1 next */
        [21] = 1, [23] = 600, [24] = 0,
    };
    memcpy(M(s)->store, prog, sizeof prog);
}

int main(int argc, char **argv) {
    (void)argv;
    OtherConfig c = { .machine = OTHER_MACHINE_BABY, .display_type = GEMU_DISPLAY_NONE };
    BabyState *s = other_baby_create(&c);
    CHECK(s);
    load_program(s);
    static const int at[] = { 1, 10, 100, 300, 399, 401, 451, 500 };
    int done = 0;
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
        while (done < at[i]) {
            if (done == 1) set_run(s, true);
            if (done == 400) { set_run(s, false); set_run(s, true); }
            if (done == 450) { set_run(s, false); press_kc(s); }
            tick(s);
            done++;
        }
        if (argc > 1) report(s, done);
    }
    CHECK(M(s)->store[20] == 601 && !s->run && M(s)->ci == 0);   /* KC obeyed the JMP */
    other_baby_destroy(s);
#ifndef O_HEAD
    if (argc < 2) baby_core_checks();
#endif
    printf("baby_core: %u checks passed\n", checks);
    return 0;
}

#ifndef O_HEAD
/* The core machine's own checks (stage O1). */
void baby_core_checks(void) {
    OtherConfig c = { .machine = OTHER_MACHINE_BABY, .display_type = GEMU_DISPLAY_NONE };
    BabyState *s = other_baby_create(&c), *t = other_baby_create(&c);
    CHECK(s && t);
    load_program(s);
    /* at Stop the executor idles: machine time passes, nothing is obeyed */
    for (int i = 0; i < 30; i++) baby_frame(s);
    CHECK(s->m->steps == 0 && gemu_machine_time(s->machine) > 0);
    /* at Run, 1,100 instructions a second, exactly */
    set_run(s, true);
    uint64_t c0 = gemu_cycles_to(BABY_IPS, gemu_machine_time(s->machine));
    for (int i = 0; i < 180; i++) baby_frame(s);
    uint64_t c1 = gemu_cycles_to(BABY_IPS, gemu_machine_time(s->machine));
    CHECK(s->m->steps == c1 - c0 && s->m->steps >= 3299 && s->m->steps <= 3301);
    /* state: round trip; another Baby runs on alike (the switch included) */
    uint8_t *a, *b;
    size_t na, nb;
    CHECK(gemu_machine_save_state(s->machine, &a, &na));
    for (int i = 0; i < 20; i++) baby_frame(s);
    CHECK(gemu_machine_load_state(s->machine, a, na));
    CHECK(gemu_machine_save_state(s->machine, &b, &nb));
    CHECK(na == nb && !memcmp(a, b, na));
    free(b);
    CHECK(gemu_machine_load_state(t->machine, a, na) && t->run && gemu_ssem_running(t->cpu));
    for (int i = 0; i < 200; i++) { baby_frame(s); baby_frame(t); }
    CHECK(!memcmp(s->m->store, t->m->store, sizeof s->m->store) && s->m->ci == t->m->ci &&
          s->m->steps == t->m->steps && s->m->stopped && t->m->stopped);
    free(a);
    /* stopped by STP, a frame obeys nothing; KC at Stop obeys one */
    uint64_t n = s->m->steps;
    baby_frame(s);
    CHECK(s->m->steps == n);
    set_run(s, false);
    press_kc(s);
    CHECK(s->m->steps == n + 1 && s->m->ci == 0);
    /* the screen: the tube and panel as drawn */
    const GemuBitmap *bmp = gemu_screen_render(s->screen);
    CHECK(bmp->width == FB_W && bmp->height == FB_H && bmp->pix[0] == COL_BG);
    other_baby_destroy(t);
    other_baby_destroy(s);
}
#endif
