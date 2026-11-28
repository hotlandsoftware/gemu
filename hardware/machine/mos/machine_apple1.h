#pragma once
#include "mos6502.h"
#include "mos6502cfg.h"
#include "controller/pia6821.h"
#include "gemu/monitor.h"
#include "gemu/serial.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct Apple1Display Apple1Display;

struct Apple1Core;

typedef struct Apple1State {
    struct Apple1Core *core;   /* the core machine (stage M3) */
    Mos6502         *cpu;      /* the cpu device's */
    const MosConfig *cfg;
    GemuMonitor     *monitor;
    Apple1Display   *display;
    GemuSerial       display_serial;
    uint8_t          mem[0x10000];
    uint8_t          rom_map[0x10000];
    uint32_t         ram_size;
    char             tape_path[512];
    uint16_t         tape_addr;
    uint32_t         tape_size;
    char             mon_line[128];
    int              mon_len;
    uint16_t         mon_last_addr;
    /* The 6821 at $D010-$D013: the keyboard on port A (a key's code, bit 7
     * set, latched in key_data; its strobe on CA1), the display on port B
     * (PB0-6 the character, PB7 busy). */
    Pia6821         *pia;      /* the pia device's */
    uint8_t          key_data;
    bool             have_monitor_rom;
    bool             native_monitor;
    bool             quit_requested;
} Apple1State;

Apple1State *apple1_create (const MosConfig *cfg);
void         apple1_destroy(Apple1State *s);
void         apple1_run    (Apple1State *s, const MosConfig *cfg);
