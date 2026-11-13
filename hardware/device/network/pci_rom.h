#pragma once

/* PCI expansion ROM helpers for a ROM image held in a malloc'd buffer: read
 * one from a file, retag the PCI IDs in its PCIR data structures, and keep
 * only the EFI image of one machine type (PCI Firmware Specification 3.0,
 * chapter 5). */

#include <stddef.h>
#include <stdint.h>

/* Read the whole file into a new malloc'd buffer; NULL on failure. *len is the
 * file size. The caller frees the buffer. */
uint8_t *pci_rom_load(const char *path, size_t *len);

/* Give every image in a combined ROM the PCI `vendor`/`device` IDs in its PCIR
 * structure and repair the image checksum (the byte sum of an image is 0).
 * A ROM built for another device ID (QEMU's EEPRO100 ROM is built for 1209)
 * then matches the controller it is attached to. Stops at the first
 * structure that is not a valid image. */
void pci_rom_retag(uint8_t *rom, size_t len, uint16_t vendor, uint16_t device);

/* A combined ROM normally begins with a legacy IA-32 image and chains to an
 * EFI image. Keep only the EFI image (code type 3) whose PE machine type is
 * `machine_type`: it is moved to the start of the buffer and its length
 * returned. 0 if there is none, with the buffer left as it was. */
size_t pci_rom_keep_efi(uint8_t *rom, size_t len, uint16_t machine_type);
