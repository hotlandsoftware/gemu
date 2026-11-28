/* CHIP-8 on the core (stage K1), with a stand-in program: drawing, the
 * delay and sound timers, RND, a store (Fx55), SKP on a held key and two
 * key waits (Fx0A), with a keypad script.  Built with -DK_HEAD against
 * HEAD's driver it runs HEAD's frame (its SDL run loop's).
 * Run: ninja check-chip8_core */
#include "../hardware/machine/chip8/machine_chip8.c"
#include "gemu/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

/* 200: CLS; VA=0 VB=5 I=font '0'
 * 208: loop: DRW VA,VB,5; VA+=6; if VA!=60 skip to nowrap; VA=0 VB+=6
 * 214: nowrap: V0=3; DT=V0; ST=V0
 * 21A: wait: V1=DT; until V1==0
 * 220: V2=RND; I=300h; store V0-V4; V3=5; if key 5 is held: V4=K
 * 22E: nokey: VC++; back to loop until VC==32; then V5=K; VC=0; CLS; loop */
static const unsigned char standin[] = {
    0x00, 0xE0, 0x6A, 0x00, 0x6B, 0x05, 0xA0, 0x50, 0xDA, 0xB5, 0x7A, 0x06, 0x3A, 0x3C,
    0x12, 0x14, 0x6A, 0x00, 0x7B, 0x06, 0x60, 0x03, 0xF0, 0x15, 0xF0, 0x18, 0xF1, 0x07,
    0x31, 0x00, 0x12, 0x1A, 0xC2, 0xFF, 0xA3, 0x00, 0xF4, 0x55, 0x63, 0x05, 0xE3, 0x9E,
    0x12, 0x2E, 0xF4, 0x0A, 0x7C, 0x01, 0x3C, 0x20, 0x12, 0x08, 0xF5, 0x0A, 0x6C, 0x00,
    0x00, 0xE0, 0x12, 0x08,
};

#ifdef K_HEAD
#define C8(s) (s)
#define MACH Chip8State
/* HEAD's SDL run loop, one frame: the keys, a key wait resolved by a key
 * pressed this frame, the frame's instructions, the timers. */
static void frame(MACH *s, const Chip8Config *cfg, uint32_t held, uint32_t pressed) {
    memcpy(s->keys_prev, s->keys, CHIP8_NUM_KEYS);
    for (int k = 0; k < CHIP8_NUM_KEYS; k++) s->keys[k] = (held >> k) & 1;
    if (s->wait_key) {
        if (!pressed) return;
        s->V[s->wait_reg] = (uint8_t)__builtin_ctz(pressed);
        s->wait_key = false;
        s->PC += 2;
    }
    int budget = cfg->cpu_hz;
    while (budget > 0 && !s->wait_key) {
        GemuTb *tb = gemu_tb_lookup(&s->tb_cache, s->PC);
        if (!tb) tb = chip8_translate_block(s, s->PC);
        if (!tb) break;
        chip8_execute_tb(s, tb);
        budget -= (int)tb->n_insns;
    }
    if (s->delay > 0) s->delay--;
    if (s->sound > 0) s->sound--;
}
#else
#define C8(s) ((s)->cpu)
#define MACH Chip8Machine
static void frame(MACH *s, const Chip8Config *cfg, uint32_t held, uint32_t pressed) {
    (void)cfg;
    chip8_set_keys(s, held);
    chip8_frame(s, pressed);
}
void chip8_core_checks(Chip8Config *c);
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

static void report(MACH *s, int frames) {
    printf("%4d", frames);
    hash("mem", C8(s)->mem, sizeof C8(s)->mem);
    hash("vram", C8(s)->vram, sizeof C8(s)->vram);
    printf(" PC %03X I %03X SP %X DT %02X ST %02X wait %d V", C8(s)->PC, C8(s)->I, C8(s)->SP,
           C8(s)->delay, C8(s)->sound, C8(s)->wait_key);
    for (int r = 0; r < 16; r++) printf(" %02X", C8(s)->V[r]);
    printf(" insn %llu\n", (unsigned long long)C8(s)->insn_count);
}

/* The keypad: key 5 held over frames 40-49 (SKP: the program waits for a
 * key), 7 pressed at 60; A pressed at 200 and 400 (the round's end wait). */
static void keys_at(int f, uint32_t *held, uint32_t *pressed) {
    *held = (f >= 40 && f < 50) ? 1u << 5 : 0;
    *pressed = f == 60 ? 1u << 7 : (f == 200 || f == 400) ? 1u << 0xA : 0;
    if (*pressed) *held |= *pressed;
}

int main(int argc, char **argv) {
    (void)argv;
    FILE *f = fopen("chip8.ch8", "wb");
    CHECK(f && fwrite(standin, 1, sizeof standin, f) == sizeof standin);
    fclose(f);
    Chip8Config c;
    memset(&c, 0, sizeof c);
    c.rom_path = "chip8.ch8";
    c.mem_size = CHIP8_MEM_SIZE;
    c.cpu_hz = CHIP8_DEFAULT_CPF;
    c.display_type = GEMU_DISPLAY_NONE;
    c.fg_argb = 0xFFFFFFFFu;
    c.bg_argb = 0xFF000000u;
    MACH *s = chip8_machine_create(&c);
    CHECK(s);
    static const int at[] = { 1, 10, 45, 61, 100, 200, 300, 500 };
    int done = 0;
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
        while (done < at[i]) {
            uint32_t held, pressed;
            keys_at(done, &held, &pressed);
            frame(s, &c, held, pressed);
            done++;
        }
        if (argc > 1) report(s, done);
    }
    CHECK(C8(s)->V[4] == 7 && C8(s)->V[5] == 0xA);   /* both key waits saw their key */
    chip8_machine_destroy(s);
#ifndef K_HEAD
    if (argc < 2) chip8_core_checks(&c);
#endif
    printf("chip8_core: %u checks passed\n", checks);
    return 0;
}

#ifndef K_HEAD
/* The core machine's own checks (stage K1). */
void chip8_core_checks(Chip8Config *c) {
    Chip8Machine *s = chip8_machine_create(c), *t = chip8_machine_create(c);
    CHECK(s && t);
    /* a frame is 1/60 s of machine time and ten instructions (or the last
     * block's end past them); the timers count down at its end */
    chip8_frame(s, 0);
    CHECK(gemu_machine_time(s->machine) == gemu_clocks(60, 1));
    CHECK(s->cpu->insn_count >= 10 && s->cpu->delay == 2);   /* DT=3, one tick */
    for (int i = 0; i < 59; i++) chip8_frame(s, 0);
    CHECK(gemu_machine_time(s->machine) == gemu_clocks(60, 60));
    /* a key wait idles the frames out; the timers keep counting */
    for (int i = 0; i < 200 && !s->cpu->wait_key; i++) chip8_frame(s, 0);
    CHECK(s->cpu->wait_key);
    uint64_t n = s->cpu->insn_count;
    s->cpu->delay = 5;
    for (int i = 0; i < 3; i++) chip8_frame(s, 0);
    CHECK(s->cpu->insn_count == n && s->cpu->wait_key && s->cpu->delay == 2);
    chip8_frame(s, 1u << 0xB);
    CHECK(!s->cpu->wait_key && s->cpu->insn_count > n);
    /* the screen is the display */
    const GemuBitmap *b = gemu_screen_render(s->screen);
    CHECK(b->width == CHIP8_DISPLAY_W && b->height == CHIP8_DISPLAY_H);
    int lit = 0;
    for (int i = 0; i < CHIP8_DISPLAY_W * CHIP8_DISPLAY_H; i++)
        lit += b->pix[(i / CHIP8_DISPLAY_W) * b->stride + i % CHIP8_DISPLAY_W] == c->fg_argb;
    int on = 0;
    for (int i = 0; i < CHIP8_DISPLAY_W * CHIP8_DISPLAY_H; i++) on += s->cpu->vram[i];
    CHECK(lit == on && on > 0);
    /* state: round trip (RND's generator included); another machine runs on alike */
    uint8_t *a, *bb;
    size_t na, nb;
    CHECK(gemu_machine_save_state(s->machine, &a, &na));
    for (int i = 0; i < 20; i++) chip8_frame(s, 0);
    CHECK(gemu_machine_load_state(s->machine, a, na));
    CHECK(gemu_machine_save_state(s->machine, &bb, &nb));
    CHECK(na == nb && !memcmp(a, bb, na));
    free(bb);
    CHECK(gemu_machine_load_state(t->machine, a, na));
    for (int i = 0; i < 300; i++) { chip8_frame(s, (i % 50 == 0) ? 1u << 3 : 0); chip8_frame(t, (i % 50 == 0) ? 1u << 3 : 0); }
    CHECK(!memcmp(s->cpu->mem, t->cpu->mem, CHIP8_MEM_SIZE) && !memcmp(s->cpu->vram, t->cpu->vram, sizeof s->cpu->vram) &&
          s->cpu->PC == t->cpu->PC && s->cpu->rng == t->cpu->rng && s->cpu->insn_count == t->cpu->insn_count);
    free(a);
    chip8_machine_destroy(t);
    chip8_machine_destroy(s);
}
#endif
