#pragma once
#include "cpu/mos6502.h"
#include "mos6502cfg.h"
#include "gemu/vnc.h"
#include "gemu/monitor.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Generic 6502 machine: flat address space (default 64 KB, override with -m).
 *   RAM: 0x0000–0xDEFF  (up to mem_size bytes, writable by default)
 *   I/O: 0xDF00–0xDFFF  (memory-mapped peripherals, always present)
 *   ROM: overlaid at load time — those ranges become read-only
 *
 * I/O map:
 *   $DF00  Timer reload low byte   (W) / current count low  (R)
 *   $DF01  Timer reload high byte  (W) / current count high (R)
 *   $DF02  Timer control           (R/W)
 *            bit 0  enable (1=counting)
 *            bit 1  IRQ enable
 *            bit 2  auto-reload on expiry (0=disable on expiry)
 *            bit 7  IRQ pending — write 1 to clear, read to test
 *   $DF10  Serial data             (W=TX→stdout, R=RX from stdin)
 *   $DF11  Serial status           (R)
 *            bit 0  RX byte available
 *            bit 1  TX ready (always 1)
 *   $DF12  Serial control          (R/W)
 *            bit 0  RX IRQ enable
 *
 * Timer counts down in CPU cycles; reload=0 treated as 65536.
 * For interactive use callers should put stdin in raw/no-echo mode.
 *
 * Legacy debug port: write a byte to $F001 → stdout (test ROM compat).
 */

#define GENERIC_IO_BASE 0xDF00u
#define GENERIC_IO_END  0xDFFFu

typedef struct MosGenericState {
    Mos6502          cpu;
    const MosConfig *cfg;
    uint8_t         *mem;      /* flat address space (heap, mem_size bytes) */
    uint8_t         *rom_map;  /* 1 = read-only at that address (heap, mem_size bytes) */
    uint32_t         mem_size; /* size of mem[] and rom_map[] */

    /* Timer ($DF00–$DF02) */
    uint16_t timer_reload;     /* countdown period in CPU cycles (0 = 65536) */
    uint8_t  timer_ctrl;       /* control register (bits 0-2; bit7 is pending, not stored) */
    bool     timer_pending;    /* IRQ pending flag */
    uint64_t timer_next_fire;  /* absolute cpu.cycle_count when next expiry occurs */

    /* Serial ($DF10–$DF12) */
    uint8_t  serial_ctrl;      /* control register */
    uint8_t  serial_rxq[16];   /* RX circular FIFO */
    uint8_t  serial_rxq_r;     /* read index (0-15) */
    uint8_t  serial_rxq_w;     /* write index (0-15) */
    uint8_t  serial_rxq_cnt;   /* bytes currently in FIFO */

    GemuVncServer   *vnc;
    GemuMonitor     *monitor;
} MosGenericState;

MosGenericState *mos_generic_create (const MosConfig *cfg);
void             mos_generic_reset  (MosGenericState *s, const MosConfig *cfg);
void             mos_generic_destroy(MosGenericState *s);
void             mos_generic_run    (MosGenericState *s, const MosConfig *cfg);
