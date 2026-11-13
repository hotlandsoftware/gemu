#pragma once

/* Intel 82802AC 8 Mbit Firmware Hub (datasheet 290658-004): sixteen 64 KB
 * blocks behind the command user interface of Table 2 (read array,
 * identifier, status, clear status, block erase, program, suspend and
 * resume), and the register space of Table 5 (per-block lock registers, the
 * FGPI pass-through register and the random number generator).
 *
 * The part owns no memory. The caller passes the 1 MB array with each access
 * so the machine's flash image receives whatever the firmware programs, and
 * passes the current time in nanoseconds for program and erase timing. */

#include <stdbool.h>
#include <stdint.h>

enum { FWH82802_SIZE = 0x100000 };

typedef struct Fwh82802 {
    uint8_t gpi;                /* levels on FGPI[4:0] as the board wires them */
    int mode, setup;
    uint8_t status;             /* status register error bits; SR.7 is computed */
    int op;                     /* program or erase: running or suspended */
    uint64_t busy_until, remaining;
    bool suspended;
    uint8_t lock[16];           /* per-block lock registers */
    uint8_t rng_status;         /* RNG hardware status */
    uint32_t rng;               /* RNG state */
    unsigned reports, register_reports;
    bool debug;
    /* The array range the last access programmed or erased, for the board to
     * mirror. The consumer clears changed_len. */
    uint32_t changed_off, changed_len;
} Fwh82802;

/* Power-up state with the given FGPI levels. */
void fwh82802_init(Fwh82802 *fwh, uint8_t gpi, bool debug);
/* RST# or INIT#: lock registers back to write-locked, lock-down cleared, the
 * command interface back to read array. */
void fwh82802_reset(Fwh82802 *fwh);
/* One byte of an FWH memory cycle at `off` (A[19:0]) of the array, or of the
 * register space (`reg` true: the part's alias 4 MB lower, Table 5). */
void fwh82802_access(Fwh82802 *fwh, uint8_t *array, uint64_t now_ns, bool reg,
                     bool write, uint32_t off, uint8_t *value);
