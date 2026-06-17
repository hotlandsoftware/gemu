#pragma once
#include "cdp1802.h"
#include "cdp1861.h"
#include "rca.h"
#include "devices/pcspk.h"
#include "gemu/monitor.h"
#include "gemu/vnc.h"
#include <stdbool.h>
#include <stdint.h>

#define STUDIO2_ROM_SIZE    0x0800u   /* 2 KB built-in ROM (0x0000-0x07FF) */
#define STUDIO2_CART_SIZE   0x0C00u   /* up to 3 KB: 0x0400-0x07FF + 0x0C00-0x0FFF */
#define STUDIO2_RAM_SIZE    0x0200u   /* 512 bytes, mirrored at 0x0800-0x09FF */
#define STUDIO2_RAM_MASK    0x01FFu
#define STUDIO2_DISPLAY_W   64
#define STUDIO2_DISPLAY_H   32
#define STUDIO2_BYTES_PER_LINE (STUDIO2_DISPLAY_W / 8)
#define STUDIO2_CART_PATH_MAX 512

typedef struct RcaStudio2State {
    Cdp1802  cpu;
    Cdp1861  vdc;
    const RcaConfig *cfg;

    uint8_t  rom[STUDIO2_ROM_SIZE];    /* built-in 2 KB ROM */
    uint8_t  cart[STUDIO2_CART_SIZE];  /* cartridge ROM banks */
    bool     cart_loaded;
    bool     cart_c00;                 /* second cart bank at 0x0C00 */
    char     cart_path[STUDIO2_CART_PATH_MAX];

    uint8_t  ram[STUDIO2_RAM_SIZE];    /* 512 bytes, mirrored */

    uint8_t  vram[STUDIO2_DISPLAY_W * STUDIO2_DISPLAY_H];
    bool     draw_flag;

    uint8_t  keylatch;     /* written by OUT 2 (key index 0-9) */
    bool     keys_a[10];   /* Player A: keys 0-9 */
    bool     keys_b[10];   /* Player B: keys 0-9 */

    GemuMonitor  *monitor;
    GemuVncServer *vnc;
    RcaPcSpeaker  *speaker;
} RcaStudio2State;

RcaStudio2State *rca_studio2_create(const RcaConfig *cfg);
void             rca_studio2_reset(RcaStudio2State *s, const RcaConfig *cfg);
void             rca_studio2_destroy(RcaStudio2State *s);
void             rca_studio2_run(RcaStudio2State *s, const RcaConfig *cfg);
