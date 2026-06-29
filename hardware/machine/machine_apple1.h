#pragma once
#include "mos6502.h"
#include "mos6502cfg.h"
#include "gemu/monitor.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct Apple1State {
    Mos6502          cpu;
    const MosConfig *cfg;
    GemuMonitor     *monitor;
    uint8_t          mem[0x10000];
    uint8_t          rom_map[0x10000];
    uint8_t          key_data;
    bool             key_ready;
} Apple1State;

Apple1State *apple1_create (const MosConfig *cfg);
void         apple1_destroy(Apple1State *s);
void         apple1_run    (Apple1State *s, const MosConfig *cfg);
