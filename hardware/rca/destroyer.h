#pragma once

#include "cdp1802.h"
#include "cdp1869.h"
#include "rca.h"
#include "gemu/monitor.h"
#include "gemu/gemu_display.h"

#define DESTRYER_MEM_SIZE 65536u

struct RcaDestroyerCore;

typedef struct RcaDestroyerState {
    struct RcaDestroyerCore *core;   /* the core machine (stage R5) */
    Cdp1802 *cpu;                    /* the cpu device's */
    Cdp1869 *vis;                    /* the vis device's */
    uint32_t frame_cycle;            /* machine cycle within the frame (the beam) */
    const RcaConfig *cfg;
    uint8_t mem[DESTRYER_MEM_SIZE];
    uint8_t rotated_bitmap[CDP1869_VISIBLE_W * CDP1869_VISIBLE_H];
    GemuMonitor *monitor;

    uint8_t in0;
    uint8_t in1;
    bool service;
    bool coin1;
    bool coin2;
    bool start1;
    bool start2;
    bool left;
    bool right;
    bool fire;
    int coin1_latch;
    int coin2_latch;
} RcaDestroyerState;

RcaDestroyerState *rca_destroyer_create(const RcaConfig *cfg);
void               rca_destroyer_destroy(RcaDestroyerState *s);
void               rca_destroyer_run(RcaDestroyerState *s, const RcaConfig *cfg);
