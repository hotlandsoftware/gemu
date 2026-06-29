#include "machine_apple1.h"
#include "gemu/memory.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APPLE1_KBD     0xD010u
#define APPLE1_KBDCR   0xD011u
#define APPLE1_DSP     0xD012u
#define APPLE1_DSPCR   0xD013u
#define APPLE1_ROM     0xFF00u
#define APPLE1_HZ      1000000u
#define APPLE1_FPS     60u
#define APPLE1_CPF     (APPLE1_HZ / APPLE1_FPS)

/*
 * Tiny GEMU boot monitor, used when no CPU-visible Apple I ROM is supplied.
 * It prints '\' and echoes keyboard input through the Apple I PIA addresses.
 * Real WozMon can replace it by loading a 256-byte ROM at $FF00.
 */
static const uint8_t apple1_boot_rom[0x100] = {
    0xD8,                         /* FF00: CLD        */
    0xA2, 0xFF,                   /*       LDX #$FF   */
    0x9A,                         /*       TXS        */
    0xA9, 0x5C,                   /*       LDA #'\'   */
    0x20, 0x20, 0xFF,             /*       JSR ECHO   */
    0xA9, 0x0D,                   /*       LDA #CR    */
    0x20, 0x20, 0xFF,             /*       JSR ECHO   */
    0x20, 0x30, 0xFF,             /* FF0E: JSR GETKEY */
    0x20, 0x20, 0xFF,             /*       JSR ECHO   */
    0x4C, 0x0E, 0xFF,             /*       JMP $FF0E  */
    [0x20] = 0x8D, 0x12, 0xD0,    /* FF20: STA $D012  */
             0x60,                /*       RTS        */
    [0x30] = 0xAD, 0x11, 0xD0,    /* FF30: LDA $D011  */
             0x10, 0xFB,          /*       BPL $FF30  */
             0xAD, 0x10, 0xD0,    /*       LDA $D010  */
             0x60,                /*       RTS        */
    [0xFA] = 0x00, 0xFF,          /* NMI              */
             0x00, 0xFF,          /* RESET            */
             0x00, 0xFF,          /* IRQ/BRK          */
};

static uint8_t apple1_normalize_key(uint8_t ch) {
    if (ch == '\n') ch = '\r';
    if (ch >= 'a' && ch <= 'z') ch = (uint8_t)toupper(ch);
    return (uint8_t)(ch | 0x80u);
}

static void apple1_poll_keyboard(Apple1State *s) {
    GemuSerial *ser = s->cfg->serial;
    if (!ser) return;
    ser->poll(ser->ud);
    if (!s->key_ready && ser->key_available(ser->ud)) {
        s->key_data = apple1_normalize_key(ser->read_byte(ser->ud));
        s->key_ready = true;
    }
}

static void apple1_write_display(Apple1State *s, uint8_t val) {
    GemuSerial *ser = s->cfg->serial;
    val &= 0x7Fu;
    if (val == '\r') {
        if (ser) {
            ser->write_byte(ser->ud, '\r');
            ser->write_byte(ser->ud, '\n');
        } else {
            putchar('\n');
            fflush(stdout);
        }
        return;
    }
    if (ser) ser->write_byte(ser->ud, val);
    else {
        putchar(val);
        fflush(stdout);
    }
}

static uint8_t apple1_read(uint16_t addr, void *ud) {
    Apple1State *s = ud;
    gemu_monitor_check_read(s->monitor, addr);

    if (addr == APPLE1_KBD) {
        uint8_t v = s->key_ready ? s->key_data : 0;
        s->key_ready = false;
        return v;
    }
    if (addr == APPLE1_KBDCR)
        return s->key_ready ? 0x80u : 0x00u;
    if (addr == APPLE1_DSPCR)
        return 0x80u;

    return s->mem[addr];
}

static void apple1_write(uint16_t addr, uint8_t val, void *ud) {
    Apple1State *s = ud;
    gemu_monitor_check_write(s->monitor, addr);

    if (addr == APPLE1_DSP) {
        apple1_write_display(s, val);
        return;
    }
    if (addr >= APPLE1_KBD && addr <= APPLE1_DSPCR)
        return;

    if (!s->rom_map[addr])
        s->mem[addr] = val;
}

static bool apple1_load_roms(Apple1State *s, const MosConfig *cfg) {
    for (int i = 0; i < cfg->n_roms; i++) {
        const char *region = cfg->roms[i].region;
        if (region && region[0] && strcmp(region, "main") != 0) {
            printf("apple1: recognized auxiliary ROM %s (%s)\n",
                   cfg->roms[i].path, region);
            continue;
        }

        uint32_t addr = cfg->roms[i].addr & 0xFFFFu;
        GemuMemory tmp = {.data = s->mem + addr, .size = 0x10000u - addr};
        size_t len = 0;
        if (!gemu_mem_load_file(&tmp, 0, cfg->roms[i].path, &len)) {
            fprintf(stderr, "apple1: failed to load '%s'\n", cfg->roms[i].path);
            return false;
        }
        memset(s->rom_map + addr, 1, len);
        printf("apple1: %zu bytes @ 0x%04X <- %s\n",
               len, (unsigned)addr, cfg->roms[i].path);
        gemu_monitor_register_rom(s->monitor, addr, (uint32_t)len, cfg->roms[i].path);
    }
    return true;
}

Apple1State *apple1_create(const MosConfig *cfg) {
    Apple1State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg = cfg;
    s->monitor = gemu_monitor_create();
    if (!s->monitor) {
        free(s);
        return NULL;
    }

    memcpy(&s->mem[APPLE1_ROM], apple1_boot_rom, sizeof(apple1_boot_rom));
    memset(&s->rom_map[APPLE1_ROM], 1, sizeof(apple1_boot_rom));
    gemu_monitor_register_rom(s->monitor, APPLE1_ROM,
                              sizeof(apple1_boot_rom), "apple1 built-in boot ROM");

    if (!apple1_load_roms(s, cfg)) {
        apple1_destroy(s);
        return NULL;
    }

    mos6502_init(&s->cpu);
    s->cpu.mem_read = apple1_read;
    s->cpu.mem_write = apple1_write;
    s->cpu.mem_ud = s;
    s->cpu.decimal_disable = false;
    mos6502_reset(&s->cpu);
    if (cfg->has_start_addr)
        s->cpu.PC = cfg->start_addr;

    return s;
}

void apple1_destroy(Apple1State *s) {
    if (!s) return;
    gemu_monitor_destroy(s->monitor);
    free(s);
}

void apple1_run(Apple1State *s, const MosConfig *cfg) {
    (void)cfg;
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();
        GemuSerial *ser = s->cfg->serial;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;
        }
        if (ser && ser->should_quit(ser->ud))
            quit = true;

        apple1_poll_keyboard(s);
        uint64_t target = s->cpu.cycle_count + APPLE1_CPF;
        while (!quit && s->cpu.cycle_count < target) {
            if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) break;
            mos6502_step(&s->cpu);
        }

        Uint32 dt = SDL_GetTicks() - t0;
        Uint32 frame_ms = 1000u / APPLE1_FPS;
        if (dt < frame_ms) SDL_Delay(frame_ms - dt);
    }

    printf("apple1: %llu cpu cycles\n",
           (unsigned long long)s->cpu.cycle_count);
}
