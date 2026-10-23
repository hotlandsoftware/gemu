#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gemu/drive.h"

/* NEC uPD765 floppy disk controller on the IBM PC diskette adapter:
 * 0x3F2 digital output register (drive select, controller reset, DMA/IRQ
 * enable, motors), 0x3F4 main status register, 0x3F5 data register.
 *
 * Implements the command set PC BIOS and DOS use - specify, sense drive
 * status, recalibrate, seek, sense interrupt status, read data, write data,
 * format track, read ID - with data moved by DMA (channel 2 on the PC) and
 * completion signalled on the interrupt line (IRQ6).  Disk images come from
 * the block layer; geometry is inferred from the image size (160K, 180K,
 * 320K, 360K, 720K, 1.2M, 1.44M). */

typedef struct {
    /* Machine wiring */
    GemuDrive *drive[4];                     /* NULL = no drive in that unit */
    void (*set_irq)(void *ud, bool level);
    /* One DMA cycle: memory -> controller (write/format) or controller ->
     * memory (read).  Return false if DMA isn't ready; *tc = terminal count. */
    bool (*dma_from_mem)(void *ud, uint8_t *val, bool *tc);
    bool (*dma_to_mem)(void *ud, uint8_t val, bool *tc);
    void *ud;

    /* State */
    uint8_t  dor;
    uint8_t  cmd[9];
    int      cmd_len, cmd_need;
    uint8_t  res[7];
    int      res_len, res_pos;
    uint8_t  pcn[4];                         /* present cylinder per drive */
    uint32_t busy;                           /* CPU clocks until the result phase */
    bool     irq_after;                      /* raise IRQ when busy ends */
    bool     irq;
    /* Interrupt status for sense interrupt status (reset/seek/recalibrate) */
    uint8_t  int_st0[4], int_pcn[4];
    int      n_int;
} Upd765;

void    upd765_reset(Upd765 *f);
uint8_t upd765_read(Upd765 *f, uint16_t port);
void    upd765_write(Upd765 *f, uint16_t port, uint8_t val);
void    upd765_tick(Upd765 *f, uint32_t cpu_clocks);
