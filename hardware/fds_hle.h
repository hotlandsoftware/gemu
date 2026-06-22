#pragma once
#include "fds.h"

/* Fill f->bios[] with a minimal stub ROM:
 *   $E18B  NMI  → JMP ($DFFA)
 *   $E1C7  IRQ  → full byte-transfer dispatch (DATA / SKIP / ACK modes)
 *   $EE24  RST  → hardware init + JMP ($DFFC)
 *   all other addresses → RTS (safe no-op for unimplemented BIOS calls)
 * Vectors: NMI=$E18B  RST=$EE24  IRQ=$E1C7 */
void fds_hle_build_rom(FdsState *f);

/* Parse f->raw_disk and load all PRG/CHR files into f->ram / chr_ram.
 * Must be called after fds_disk_load() sets f->raw_disk.
 * chr_ram must point to FDS_CHR_SIZE (8 KB) of PPU CHR RAM, or NULL. */
void fds_hle_boot(FdsState *f, uint8_t *chr_ram);
