#pragma once
#include "mos6502.h"
#include "mos6502cfg.h"
#include "gemu/monitor.h"
#include "gemu/serial.h"
#include "gemu/vnc.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct Apple1Display Apple1Display;

typedef struct Apple1State {
    Mos6502          cpu;
    const MosConfig *cfg;
    GemuMonitor     *monitor;
    GemuVncServer   *vnc;
    Apple1Display   *display;
    GemuSerial       display_serial;
    uint8_t         *vnc_fb;
    uint8_t          mem[0x10000];
    uint8_t          rom_map[0x10000];
    char             mon_line[128];
    int              mon_len;
    uint16_t         mon_last_addr;
    uint8_t          key_data;
    bool             have_monitor_rom;
    bool             native_monitor;
    bool             key_ready;
} Apple1State;

Apple1State *apple1_create (const MosConfig *cfg);
void         apple1_destroy(Apple1State *s);
void         apple1_run    (Apple1State *s, const MosConfig *cfg);
