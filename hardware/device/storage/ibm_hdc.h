#pragma once
/*
 * IBM Fixed Disk Adapter (the Xebec S1410-based card IBM sold for the
 * 5150 with the 5161 expansion unit, and fitted to the XT), at ports
 * 320h-323h with its BIOS ROM at C8000h.  Two ST-506/412 drives, 17
 * sectors of 512 bytes per track, data by DMA (channel 3 on the PC) and
 * completion on IRQ 5.
 *
 * Ports:
 *   320h  R/W  data: command bytes, polled parameters, the status byte
 *   321h  R    status    W  controller reset
 *   322h  R    drive-type jumpers (drive 0 in bits 2-3, drive 1 in 0-1)
 *         W    select: starts the command phase
 *   323h  W    DMA and interrupt mask (bit 0 DMA, bit 1 IRQ)
 *
 * Status register (321h): bit 0 REQ, 1 I/O (controller -> host), 2 C/D
 * (command/status rather than data), 3 BSY, 4 DRQ, 5 interrupt.  A
 * command is six bytes: opcode; drive (bit 5) and head; cylinder bits 8-9
 * (bits 6-7) and sector; cylinder bits 0-7; block count or interleave;
 * control.  It ends with one status byte: bit 1 error, bit 5 the drive;
 * "request sense" (03h) returns the error code and the address it
 * happened at.
 *
 * Timing follows the drive: 3600 rpm and a sector slot every 1/17 turn,
 * so sectors are found where the interleave put them; seeks take 3 ms to
 * the next cylinder plus 0.66 ms per cylinder after that (the ST-412's
 * 3 ms track to track and ~205 ms full stroke).  The DMA transfer of a
 * sector takes as long as the PC's DMA cycles do, so with too tight an
 * interleave the next sector has passed by and costs a revolution, as
 * it does on the real card.
 */
#include "gemu/drive.h"
#include <stdbool.h>
#include <stdint.h>

#define IBM_HDC_SPT        17
#define IBM_HDC_SECTOR     512
#define IBM_HDC_LONG       516      /* read/write long: data plus 4 ECC bytes */

typedef struct {
    uint16_t cyls;
    uint8_t  heads;
} IbmHdcGeometry;

typedef struct IbmHdc {
    /* Machine wiring, set before ibm_hdc_reset() */
    GemuDrive     *drive[2];        /* NULL = no drive */
    IbmHdcGeometry geom[2];         /* the drive's own geometry (the image's) */
    uint8_t        interleave[2];   /* how the image's tracks were formatted */
    uint8_t        jumpers;         /* read back at 322h */
    uint32_t       clock_hz;        /* the clock ibm_hdc_tick() counts */
    void (*set_irq)(void *ud, bool level);
    /* One DMA cycle; false when the channel isn't ready.  *tc = terminal count. */
    bool (*dma_to_mem)(void *ud, uint8_t val, bool *tc);
    bool (*dma_from_mem)(void *ud, uint8_t *val, bool *tc);
    void *ud;

    /* State */
    uint64_t now;                   /* clocks since power-on: where the platters are */
    int      phase;
    int      stage;                 /* what the current command does when wait ends */
    uint32_t wait;                  /* clocks until stage runs (0 = not waiting) */
    uint8_t  mask;
    bool     intr;
    uint8_t  cmd[6];
    int      cmd_n;
    uint8_t  pio[8];                /* polled parameter/result bytes */
    int      pio_n, pio_pos;
    uint8_t  buf[IBM_HDC_LONG];     /* the sector buffer */
    int      buf_len, buf_pos;
    bool     to_mem;                /* DMA direction */

    /* The command being run */
    int      drv;
    unsigned cyl, head, sector, count;

    /* Per drive: where the heads are, and the geometry "initialize drive
     * characteristics" gave */
    unsigned pos[2];
    IbmHdcGeometry param[2];

    /* The last command's outcome: its status byte, and what request sense
     * returns (error code with bit 7 = address valid, then the address) */
    uint8_t  status_byte;
    uint8_t  sense[4];
} IbmHdc;

void    ibm_hdc_reset(IbmHdc *h);
uint8_t ibm_hdc_read(IbmHdc *h, unsigned reg);           /* reg = port - 320h, 0-3 */
void    ibm_hdc_write(IbmHdc *h, unsigned reg, uint8_t val);
void    ibm_hdc_tick(IbmHdc *h, uint32_t clocks);
