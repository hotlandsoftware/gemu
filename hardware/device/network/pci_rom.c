/* PCI expansion ROM helpers (see pci_rom.h). */
#include "pci_rom.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *pci_rom_load(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END) != 0) {
        if (f) {
            fclose(f);
        }
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *rom = malloc((size_t)size);
    if (!rom || fread(rom, 1, (size_t)size, f) != (size_t)size) {
        free(rom);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return rom;
}

/* Where the PCIR structure of the image at `image` is, and the image's length
 * from it; false when the image or its structure is not valid. Reports the
 * structure even if its length field is bad (*image_len = 0). */
static bool pcir_at(const uint8_t *rom, size_t len, size_t image, size_t *pcir,
                    size_t *image_len) {
    if (image + 0x20 > len || rom[image] != 0x55 || rom[image + 1] != 0xaa) {
        return false;
    }
    uint16_t offset = (uint16_t)(rom[image + 0x18] | rom[image + 0x19] << 8);
    if (image + offset + 0x16 > len || memcmp(rom + image + offset, "PCIR", 4) != 0) {
        return false;
    }
    *pcir = image + offset;
    *image_len = (size_t)(rom[*pcir + 0x10] | rom[*pcir + 0x11] << 8) * 512;
    return true;
}

void pci_rom_retag(uint8_t *rom, size_t len, uint16_t vendor, uint16_t device) {
    size_t pcir, image_len;
    for (size_t image = 0; pcir_at(rom, len, image, &pcir, &image_len);) {
        rom[pcir + 4] = (uint8_t)vendor;
        rom[pcir + 5] = (uint8_t)(vendor >> 8);
        rom[pcir + 6] = (uint8_t)device;
        rom[pcir + 7] = (uint8_t)(device >> 8);
        if (!image_len || image + image_len > len) {
            break;
        }
        uint8_t sum = 0;
        for (size_t i = 0; i < image_len; i++) {
            sum = (uint8_t)(sum + rom[image + i]);
        }
        rom[image + image_len - 1] = (uint8_t)(rom[image + image_len - 1] - sum);
        if (rom[pcir + 0x15] & 0x80) {
            break;   /* last image */
        }
        image += image_len;
    }
}

size_t pci_rom_keep_efi(uint8_t *rom, size_t len, uint16_t machine_type) {
    size_t pcir, image_len;
    for (size_t image = 0; pcir_at(rom, len, image, &pcir, &image_len);) {
        if (!image_len || image + image_len > len) {
            break;
        }
        /* EFI PCI expansion ROM header: EFI signature at +4, subsystem at +8
         * and PE machine type at +10. Code type 3 alone is insufficient: an
         * x86-64 EFI image cannot run on an Itanium EFI. */
        uint16_t image_machine = (uint16_t)(rom[image + 10] | rom[image + 11] << 8);
        if (rom[pcir + 0x14] == 3 && image_machine == machine_type) {
            memmove(rom, rom + image, image_len);
            return image_len;
        }
        image += image_len;
    }
    return 0;
}
