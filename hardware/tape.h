#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * COSMAC VIP cassette tape emulator.
 *
 * Encodes a program binary into the VIP monitor's tape bit-stream and feeds
 * it to the EF3 external-flag pin at the correct machine-cycle rate.
 *
 * Protocol (VIP monitor wire format):
 *   Leader : 512 × 0-bits  (half-period = VIP_TAPE_HP_ZERO)
 *   Header : start_addr_hi, start_addr_lo, end_addr_hi, end_addr_lo  (4 bytes)
 *   Data   : program bytes
 *   Tail   : checksum byte (XOR of all data bytes)
 *
 * Encoding: FSK, bits sent MSB-first.
 *   0-bit → EF3 half-period = VIP_TAPE_HP_ZERO machine cycles (~1200 Hz)
 *   1-bit → EF3 half-period = VIP_TAPE_HP_ONE  machine cycles (~2400 Hz)
 *
 * EF3 starts LOW and toggles at the end of each half-period.  The monitor
 * ROM detects bit values by measuring how many cycles EF3 stays in one
 * state (long = 0, short = 1).
 */

/* CDP1802 at 220 000 machine cycles/sec */
#define VIP_TAPE_HP_ZERO  183u   /* 0-bit half-period ≈ 1200 Hz */
#define VIP_TAPE_HP_ONE    92u   /* 1-bit half-period ≈ 2400 Hz */

#define VIP_TAPE_PATH_MAX 512

typedef struct VipTape {
    uint8_t  *bits;        /* bit stream: one byte per bit (0 or 1) */
    size_t    n_bits;      /* total bits in stream */
    size_t    bit_pos;     /* index of next bit to send */
    int       phase;       /* which half of current bit (0 = first, 1 = second) */
    int       ef3;         /* current EF3 output level (0 or 1) */
    uint64_t  next_toggle; /* CPU cycle count when EF3 should next change */
    bool      playing;

    char      path[VIP_TAPE_PATH_MAX];
} VipTape;

void vip_tape_init(VipTape *t);
void vip_tape_destroy(VipTape *t);

/*
 * Load a raw binary file.  load_addr is the address where the program will
 * be stored in VIP memory.  start_cycle should be s->cpu.cycle_count at the
 * time of insertion so the first toggle is scheduled relative to now.
 * Returns true on success.
 */
bool vip_tape_load(VipTape *t, const char *path, uint16_t load_addr,
                   uint64_t start_cycle);

/* Remove the current tape; EF3 returns to 0. */
void vip_tape_eject(VipTape *t);

/*
 * Call once per machine cycle (or at least once per frame tick).
 * Returns the current EF3 level and advances the bit stream when the
 * scheduled toggle time is reached.
 */
int vip_tape_ef3(VipTape *t, uint64_t cycle_count);
