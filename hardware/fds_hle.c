#include "fds_hle.h"
#include <string.h>
#include <stdio.h>

/* ── Stub ROM builder ────────────────────────────────────────────────────── */

/* Place bytes at CPU address addr in the BIOS ROM array */
#define ROM(addr, ...) do {                                  \
    static const uint8_t _b[] = {__VA_ARGS__};              \
    size_t _off = (addr) - 0xE000u;                         \
    if (_off + sizeof(_b) <= FDS_BIOS_SIZE)                 \
        memcpy(f->bios + _off, _b, sizeof(_b));             \
} while (0)

void fds_hle_build_rom(FdsState *f) {
    /* Fill with RTS — games that JSR to unimplemented BIOS entries return cleanly */
    memset(f->bios, 0x60, FDS_BIOS_SIZE);

    /* ── NMI handler @ $E18B ──
     * Real BIOS dispatches based on $0100 flags; we just forward to ($DFFA).
     * The stub reset handler sets $0100 = $C0 which the real BIOS also uses
     * to mean "route NMI to game", so games work correctly either way. */
    ROM(0xE18B,
        0x6C, 0xFA, 0xDF   /* JMP ($DFFA) */
    );

    /* ── IRQ handler @ $E1C7 ──
     * Mirrors the real BIOS IRQ dispatch exactly:
     *
     *   BIT $0101        N = bit7, V = bit6
     *   BMI $E1EA        bit7 set → ACK/alt mode
     *   BVC $E1D9        bit6 clear → SKIP mode
     *
     *   DATA mode (bit7=0 bit6=1):
     *     LDX $4031      ; read disk byte into X (clears transfer_flag)
     *     STA $4024      ; write-data register (ignored in read mode)
     *     PLA / PLA / PLA ; pop IRQ stack frame that JSR $E7A3 pushed
     *     TXA            ; A = disk byte
     *     RTS            ; return to JSR $E7A3 caller
     *
     *   SKIP mode (bit7=0 bit6=0):
     *     decrement $0101 counter; if > 0 consume $4031; PLA; RTI
     *
     *   ACK mode (bit7=1 bit6=0):
     *     read $4030 (clears transfer_flag); PLA; RTI
     *
     *   BOTH bits (bit7=1 bit6=1):
     *     JMP ($DFFE)    ; indirect dispatch */

    ROM(0xE1C7,
        /* $E1C7 */ 0x2C, 0x01, 0x01,   /* BIT $0101                         */
        /* $E1CA */ 0x30, 0x1E,          /* BMI $E1EA  (+0x1E from $E1CC)     */
        /* $E1CC */ 0x50, 0x0B,          /* BVC $E1D9  (+0x0B from $E1CE)     */
        /* DATA mode ─────────────────────────────────────────── $E1CE */
        /* $E1CE */ 0xAE, 0x31, 0x40,   /* LDX $4031                         */
        /* $E1D1 */ 0x8D, 0x24, 0x40,   /* STA $4024                         */
        /* $E1D4 */ 0x68,               /* PLA                               */
        /* $E1D5 */ 0x68,               /* PLA                               */
        /* $E1D6 */ 0x68,               /* PLA                               */
        /* $E1D7 */ 0x8A,               /* TXA                               */
        /* $E1D8 */ 0x60,               /* RTS                               */
        /* SKIP mode ─────────────────────────────────────────── $E1D9 */
        /* $E1D9 */ 0x48,               /* PHA                               */
        /* $E1DA */ 0xAD, 0x01, 0x01,   /* LDA $0101                         */
        /* $E1DD */ 0x38,               /* SEC                               */
        /* $E1DE */ 0xE9, 0x01,         /* SBC #$01                          */
        /* $E1E0 */ 0x90, 0x04,         /* BCC $E1E8  (+4 from $E1E2)        */
        /* $E1E2 */ 0x8D, 0x01, 0x01,   /* STA $0101                         */
        /* $E1E5 */ 0xAD, 0x31, 0x40,   /* LDA $4031  (consume byte)         */
        /* $E1E8 */ 0x68,               /* PLA                               */
        /* $E1E9 */ 0x40,               /* RTI                               */
        /* ACK mode ──────────────────────────────────────────── $E1EA */
        /* $E1EA */ 0x50, 0x03,         /* BVC $E1EF  (+3 from $E1EC)        */
        /* $E1EC */ 0x6C, 0xFE, 0xDF,   /* JMP ($DFFE)                       */
        /* $E1EF */ 0x48,               /* PHA                               */
        /* $E1F0 */ 0xAD, 0x30, 0x40,   /* LDA $4030  (ack transfer flag)    */
        /* $E1F3 */ 0x68,               /* PLA                               */
        /* $E1F4 */ 0x40                /* RTI                               */
    );

    /* ── Reset handler @ $EE24 ──
     * Hardware init matching the real BIOS cold-boot register state,
     * then jump to the game's entry point via ($DFFC). */
    ROM(0xEE24,
        0x78,               /* SEI                          */
        0xA9, 0x10,         /* LDA #$10                     */
        0x8D, 0x00, 0x20,  /* STA $2000  NMI off, 8×8 spr  */
        0xA9, 0x06,         /* LDA #$06                     */
        0x85, 0xFE,         /* STA $FE                      */
        0x8D, 0x01, 0x20,  /* STA $2001  rendering off      */
        0xA9, 0x00,         /* LDA #$00                     */
        0x8D, 0x22, 0x40,  /* STA $4022  timer off          */
        0xA9, 0x83,         /* LDA #$83                     */
        0x8D, 0x23, 0x40,  /* STA $4023  enable disk I/O   */
        0xA9, 0x0F,         /* LDA #$0F                     */
        0x8D, 0x15, 0x40,  /* STA $4015  APU channels on    */
        0xA9, 0xC0,         /* LDA #$C0                     */
        0x8D, 0x17, 0x40,  /* STA $4017  frame mode 1       */
        0xA2, 0xFF,         /* LDX #$FF                     */
        0x9A,               /* TXS                          */
        0xA9, 0x2E,         /* LDA #$2E                     */
        0x85, 0xFA,         /* STA $FA                      */
        0x8D, 0x25, 0x40,  /* STA $4025  drive ctrl init    */
        0xA9, 0xFF,         /* LDA #$FF                     */
        0x85, 0xF9,         /* STA $F9                      */
        0x8D, 0x26, 0x40,  /* STA $4026  ext connector      */
        0xA9, 0xC0,         /* LDA #$C0                     */
        0x8D, 0x00, 0x01,  /* STA $0100  NMI → game ($DFFA)*/
        0xA9, 0x80,         /* LDA #$80                     */
        0x8D, 0x01, 0x01,  /* STA $0101  IRQ ACK mode init  */
        0x58,               /* CLI                          */
        0x6C, 0xFC, 0xDF   /* JMP ($DFFC)  game entry       */
    );

    /* ── Interrupt vectors ── */
    f->bios[0x1FFA] = 0x8B; f->bios[0x1FFB] = 0xE1; /* NMI  → $E18B */
    f->bios[0x1FFC] = 0x24; f->bios[0x1FFD] = 0xEE; /* RST  → $EE24 */
    f->bios[0x1FFE] = 0xC7; f->bios[0x1FFF] = 0xE1; /* IRQ  → $E1C7 */

    fprintf(stderr, "fds-hle: stub ROM built (no BIOS required)\n");
}

/* ── Disk file loader ────────────────────────────────────────────────────── */

static void load_side(FdsState *f, uint8_t side, uint8_t *chr_ram) {
    const uint8_t *p   = f->raw_disk + (size_t)side * FDS_SIDE_BYTES;
    size_t         off = 0;

    /* Block $01: disk info (56 bytes) */
    if (p[off] != 0x01) {
        fprintf(stderr, "fds-hle: side %u: bad disk info block\n", side);
        return;
    }
    /* Print game name (bytes 16-19) */
    fprintf(stderr, "fds-hle: side %u — %.4s\n", side, (char *)p + 16);
    off += 56;

    /* Block $02: file count (2 bytes) */
    if (p[off] != 0x02) {
        fprintf(stderr, "fds-hle: side %u: missing file count block\n", side);
        return;
    }
    unsigned nfiles = p[off + 1];
    off += 2;

    for (unsigned fi = 0; fi < nfiles; fi++) {
        /* Block $03: file header (16 bytes) */
        if (off + 16 > FDS_SIDE_BYTES || p[off] != 0x03) {
            fprintf(stderr, "fds-hle: side %u file %u: missing header (off=%zu)\n",
                    side, fi, off);
            return;
        }
        uint16_t load_addr = (uint16_t)p[off + 11] | ((uint16_t)p[off + 12] << 8);
        uint16_t file_size = (uint16_t)p[off + 13] | ((uint16_t)p[off + 14] << 8);
        uint8_t  file_type = p[off + 15];
        off += 16;

        /* Block $04: file data (1 + file_size bytes) */
        if (off + 1 > FDS_SIDE_BYTES || p[off] != 0x04) {
            fprintf(stderr, "fds-hle: side %u file %u: missing data block\n", side, fi);
            return;
        }
        off += 1;

        if (off + file_size > FDS_SIDE_BYTES) {
            fprintf(stderr, "fds-hle: side %u file %u: data truncated\n", side, fi);
            return;
        }

        if (file_type == 0) {
            /* PRG: CPU address space $6000–$DFFF */
            if (load_addr >= 0x6000u && (uint32_t)load_addr + file_size <= 0xE000u) {
                memcpy(f->ram + load_addr - 0x6000u, p + off, file_size);
                fprintf(stderr, "fds-hle:   prg $%04X + $%04X bytes\n",
                        load_addr, file_size);
            } else {
                fprintf(stderr, "fds-hle:   prg $%04X + $%04X — out of range, skip\n",
                        load_addr, file_size);
            }
        } else if (file_type == 1) {
            /* CHR: PPU address space $0000–$1FFF */
            if (chr_ram && (uint32_t)load_addr + file_size <= FDS_CHR_SIZE) {
                memcpy(chr_ram + load_addr, p + off, file_size);
                fprintf(stderr, "fds-hle:   chr $%04X + $%04X bytes\n",
                        load_addr, file_size);
            }
        }
        /* file_type 2 = nametable: rare, skip */

        off += file_size;

        /* Skip $00 padding between file blocks */
        while (off < FDS_SIDE_BYTES && p[off] == 0x00) off++;
    }
}

void fds_hle_boot(FdsState *f, uint8_t *chr_ram) {
    if (!f->raw_disk || f->disk_sides == 0) {
        fprintf(stderr, "fds-hle: no disk inserted\n");
        return;
    }
    for (uint8_t s = 0; s < f->disk_sides; s++)
        load_side(f, s, chr_ram);
}
