#pragma once
#include "cdp1802.h"
#include "cdp1861.h"
#include "cdp1869.h"
#include "rca.h"
#include "pcspk.h"
#include "tape.h"
#include "gemu/monitor.h"
#include "gemu/vnc.h"
#include <stdlib.h>

/* ── COSMAC VIP machine state ────────────────────────────────────────────── */

#define VIP_MEM_SIZE 65536u

typedef struct RcaVipState {
    Cdp1802  cpu;
    Cdp1861  vdc;
    Cdp1869  vis;
    const RcaConfig *cfg;
    uint8_t  mem[VIP_MEM_SIZE];
    GemuMonitor *monitor;
    GemuVncServer *vnc;
    RcaPcSpeaker *speaker;

    /* Compact cassette tape */
    VipTape  tape;

    /* Pixel framebuffer: 1 byte per pixel (0 or 1), 64 wide × 128 tall */
    uint8_t  vram[CDP1861_DISPLAY_W * CDP1861_DISPLAY_H];
    bool     draw_flag;

    /* Hex keypad state */
    uint8_t  keys[16];    /* 1 = pressed */
    int      key_down;    /* most-recent key held, -1 = none */
    int      ascii_key;   /* pending ASCII key, -1 = none */

    /* Simple front-panel inputs used by VIP loaders/monitors. */
    bool     ef2_down;
} RcaVipState;

/* ── Public API ──────────────────────────────────────────────────────────── */

RcaVipState *rca_vip_create(const RcaConfig *cfg);
void         rca_vip_destroy(RcaVipState *s);
void         rca_machine_run(RcaVipState *s, const RcaConfig *cfg);
