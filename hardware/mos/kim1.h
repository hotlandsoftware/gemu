#pragma once
#include "mos6502.h"
#include "mos6502cfg.h"
#include "gemu/monitor.h"
#include "gemu/gemu_display.h"
#include "gemu/serial.h"
#include "gemu/vnc.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * KIM-1 single-board computer (MOS Technology, 1976).
 *
 * Memory map (incomplete address decoding; $0000–$1FFF repeats):
 *   $0000–$03FF  1 KB RAM
 *   $0400–$16FF  open bus (reads 0)
 *   $1700–$170F  6530 U2 I/O registers (display / keypad / cassette / TTY)
 *   $1710–$177F  6530 U2 + U3 internal 64-byte RAMs (mirrored)
 *   $1740–$174F  6530 U3 I/O registers
 *   $1800–$1BFF  6530-002 ROM (1 KB)
 *   $1C00–$1FFF  6530-003 ROM (1 KB)
 *   $2000–$FFFF  mirrors of $0000–$1FFF
 *
 * CPU vectors ($FFFA–$FFFF) are decoded to $1FFA–$1FFF thanks to
 * incomplete address decoding - no explicit mirror needed.
 *
 * Framebuffer layout (when visual keypad is enabled via -device kim-keypad):
 *   ┌──────────────────────────────────────┐
 *   │   ─── ─── ─── ───  ─── ───          │  ← 7-segment LED display
 *   │                                      │
 *   │  [GO] [ST] [RS] [SST]               │  ← physical keypad layout
 *   │  [AD] [DA] [PC] [ +]                │
 *   │  [ C] [ D] [ E] [ F]                │
 *   │  [ 8] [ 9] [ A] [ B]                │
 *   │  [ 4] [ 5] [ 6] [ 7]                │
 *   │  [ 0] [ 1] [ 2] [ 3]                │
 *   └──────────────────────────────────────┘
 */

/* ── Framebuffer geometry ───────────────────────────────────────────────── */

#define KIM1_FB_WIDTH    520
#define KIM1_FB_HEIGHT   960

/* 7-segment digits */
#define KIM1_NUM_DIGITS  6
#define KIM1_DIGIT_W     72
#define KIM1_DIGIT_H     110
#define KIM1_DIGIT_GAP    6

/* Visual keypad */
#define KIM1_KEY_W       102
#define KIM1_KEY_H       104
#define KIM1_KEY_GAP      26
#define KIM1_KEYPAD_ROWS  6
#define KIM1_KEYPAD_COLS  4
#define KIM1_KEYPAD_Y    205

#define KIM1_PRESENT_WIDTH   346
#define KIM1_PRESENT_HEIGHT  640

/* ── 6530 register offsets ──────────────────────────────────────────────── */

#define KIM1_U2_BASE   0x1700u
#define KIM1_U3_BASE   0x1740u

#define KIM1_RA        0x00u
#define KIM1_DDRA      0x01u
#define KIM1_RB        0x02u
#define KIM1_DDRB      0x03u
#define KIM1_TL        0x04u
#define KIM1_TH        0x05u
#define KIM1_TW        0x06u
#define KIM1_TIF       0x07u

/* ── Keypad action bits ──────────────────────────────────────────────────── */

#define KIM1_ACT_0      GEMU_ACTION(0)
#define KIM1_ACT_1      GEMU_ACTION(1)
#define KIM1_ACT_2      GEMU_ACTION(2)
#define KIM1_ACT_3      GEMU_ACTION(3)
#define KIM1_ACT_4      GEMU_ACTION(4)
#define KIM1_ACT_5      GEMU_ACTION(5)
#define KIM1_ACT_6      GEMU_ACTION(6)
#define KIM1_ACT_7      GEMU_ACTION(7)
#define KIM1_ACT_8      GEMU_ACTION(8)
#define KIM1_ACT_9      GEMU_ACTION(9)
#define KIM1_ACT_A      GEMU_ACTION(10)
#define KIM1_ACT_B      GEMU_ACTION(11)
#define KIM1_ACT_C      GEMU_ACTION(12)
#define KIM1_ACT_D      GEMU_ACTION(13)
#define KIM1_ACT_E      GEMU_ACTION(14)
#define KIM1_ACT_F      GEMU_ACTION(15)
#define KIM1_ACT_AD     GEMU_ACTION(16)
#define KIM1_ACT_DA     GEMU_ACTION(17)
#define KIM1_ACT_PC     GEMU_ACTION(18)
#define KIM1_ACT_PLUS   GEMU_ACTION(19)
#define KIM1_ACT_GO     GEMU_ACTION(20)
#define KIM1_ACT_ST     GEMU_ACTION(21)
#define KIM1_ACT_RS     GEMU_ACTION(22)

#define KIM1_NUM_ACTIONS  23

/* ── Per-6530 state ──────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  pa, pb;
    uint8_t  ddra, ddrb;
    uint8_t  reload;        /* 8-bit countdown load value */
    uint16_t prescale;      /* 1, 8, 64, or 1024 */
    bool     irq_en;        /* true = drive CPU IRQ line on underflow */
    bool     running;
    bool     irq_flag;
    uint64_t next_fire;     /* cpu cycle count at timer underflow */
} Kim1Rriot;

typedef struct Kim1State {
    Mos6502          cpu;
    const MosConfig *cfg;
    uint8_t          ram[1024];
    uint8_t          rom_002[1024];
    uint8_t          rom_003[1024];

    Kim1Rriot        u2;
    Kim1Rriot        u3;
    uint8_t          rriot_ram[128];

    /* Display */
    uint8_t          seg_cache[KIM1_NUM_DIGITS]; /* segment patterns cached from Port A/B writes */
    bool             display_dirty;
    uint32_t         fb[KIM1_FB_WIDTH * KIM1_FB_HEIGHT];
    uint32_t         keypad_held;
    uint32_t         keypad_prev;
    bool             kim_key_pending;
    uint8_t          kim_key_number;
    bool             kim_address_mode;
    uint16_t         kim_panel_addr;
    uint16_t         kim_saved_pc;
    bool             kim_running_user;
    bool             has_keypad;        /* -device kim-keypad → render visual keypad */
    GemuDisplay     *display;
    GemuVncServer   *vnc;
    uint8_t          vnc_fb[KIM1_PRESENT_WIDTH * KIM1_PRESENT_HEIGHT];

    GemuMonitor     *monitor;

    GemuSerial      *serial;            /* NULL if no serial terminal attached */

    /* Expansion RAM: covers $0400–(ext_ram_top-1), NULL if none.
     * Addresses $2000+ map here without KIM-1 mirror (full address decoding). */
    uint8_t  *ext_ram;
    uint32_t  ext_ram_top;

#if defined(GEMU_GTK) || defined(_WIN32)
    struct HexEditor *hex_editor;
#endif
} Kim1State;

Kim1State *kim1_create (const MosConfig *cfg);
void       kim1_destroy(Kim1State *s);
void       kim1_run    (Kim1State *s, const MosConfig *cfg);

/* Render framebuffer: 7-segment digits + optional visual keypad + blink cursor. */
