/* Intel 82559 10/100 MBPS Fast Ethernet PCI Controller 
   NOTE: does not actually allow for internet connectivity yet
*/

#include "i82559.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SCB_STATUS = 0x00,
    SCB_COMMAND = 0x02,
    SCB_POINTER = 0x04,
    SCB_PORT = 0x08,
    SCB_EEPROM = 0x0e,
    SCB_MDI = 0x10,
};

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, sizeof(v)); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }

static void i82559_reset_regs(I82559 *n) {
    memset(n->regs, 0, sizeof(n->regs));
    n->scb_pointer = n->cu_base = n->ru_base = n->mdi = 0;
    n->ee_shift = 0;
    n->ee_bits = n->ee_out_bits = 0;
    n->ee_loaded = false;
    n->ee_out = 0;
    n->ee_control = 0;
}

void i82559_init(I82559 *n, bool enabled, uint32_t mmio_base,
                 uint32_t io_base, uint8_t irq, I82559GuestRead read_cb,
                 I82559GuestWrite write_cb, void *opaque) {
    memset(n, 0, sizeof(*n));
    n->enabled = enabled;
    n->guest_read = read_cb;
    n->guest_write = write_cb;
    n->guest_opaque = opaque;
    if (!enabled)
        return;

    put16(n->cfg + 0x00, I82559_PCI_VENDOR_ID);
    put16(n->cfg + 0x02, I82559_PCI_DEVICE_ID);
    put16(n->cfg + 0x04, 0x0007); /* I/O, memory and bus master */
    put16(n->cfg + 0x06, 0x0280); /* medium DEVSEL, fast back-to-back */
    n->cfg[0x08] = 0x0c;          /* late 82559-compatible revision */
    n->cfg[0x0a] = 0x00;
    n->cfg[0x0b] = 0x02;          /* Ethernet controller */
    n->cfg[0x0d] = 0x20;
    put32(n->cfg + 0x10, mmio_base);
    put32(n->cfg + 0x14, io_base | 1u);
    n->cfg[0x3c] = irq;
    n->cfg[0x3d] = 1;
    n->cfg[0x3e] = 8;
    n->cfg[0x3f] = 0x18;

    /* Locally administered, deterministic address 52:54:00:12:29:01. */
    n->eeprom[0] = 0x5452;
    n->eeprom[1] = 0x1200;
    n->eeprom[2] = 0x0129;
    n->eeprom[3] = 0x0003;
    uint16_t sum = 0;
    for (unsigned i = 0; i < 63; i++)
        sum = (uint16_t)(sum + n->eeprom[i]);
    n->eeprom[63] = (uint16_t)(0xbaba - sum);

    n->phy[0] = 0x3000; /* auto-negotiation enabled */
    n->phy[1] = 0x782d; /* 10/100 capabilities, link and auto-neg complete */
    n->phy[2] = 0x02a8;
    n->phy[3] = 0x0150;
    n->phy[4] = 0x01e1;
    n->phy[5] = 0x41e1;
    n->phy[6] = 0x0001;
    i82559_reset_regs(n);
}

uint64_t i82559_pci_read(const I82559 *n, unsigned reg, unsigned size) {
    uint64_t v = 0;
    if (reg + size > sizeof(n->cfg))
        return UINT64_MAX;
    memcpy(&v, n->cfg + reg, size);
    if (size == 4 && (uint32_t)v == UINT32_MAX) {
        if (reg == 0x10) return 0xfffff000u;
        if (reg == 0x14) return 0xffffffc1u;
        if (reg == 0x30) {
            /* Do not advertise an expansion-ROM aperture when no ROM is
             * installed.  Returning the minimum 2 KiB mask here made EFI
             * register an option ROM backed by open-bus 0xff bytes, then
             * spin forever while processing that bogus image after PCI
             * enumeration. */
            if (!n->option_rom || !n->option_rom_size)
                return 0;
            uint32_t span = 1;
            while (span < n->option_rom_size) span <<= 1;
            return (~(span - 1u) & 0xfffff800u) | 1u;
        }
    }
    return v;
}

bool i82559_load_option_rom(I82559 *n, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END) != 0) {
        if (f) fclose(f);
        return false;
    }
    long len = ftell(f);
    if (len <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    uint8_t *rom = malloc((size_t)len);
    if (!rom || fread(rom, 1, (size_t)len, f) != (size_t)len) {
        free(rom);
        fclose(f);
        return false;
    }
    fclose(f);
    free(n->option_rom);
    n->option_rom = rom;
    n->option_rom_size = (size_t)len;

    /* QEMU's combined EEPRO100 ROM is built for PCI ID 1209. The i2000's
     * integrated 82559 uses 1229 but the driver supports the same register
     * interface. Retag every real PCIR image and repair its image checksum. */
    for (size_t image = 0; image + 0x20 <= n->option_rom_size;) {
        if (rom[image] != 0x55 || rom[image + 1] != 0xaa) break;
        uint16_t pcir = (uint16_t)(rom[image + 0x18] |
                                   rom[image + 0x19] << 8);
        if (image + pcir + 0x16 > n->option_rom_size ||
            memcmp(rom + image + pcir, "PCIR", 4) != 0) break;
        size_t p = image + pcir;
        rom[p + 4] = 0x86; rom[p + 5] = 0x80;
        rom[p + 6] = 0x29; rom[p + 7] = 0x12;
        size_t image_len = (size_t)(rom[p + 0x10] | rom[p + 0x11] << 8) * 512;
        if (!image_len || image + image_len > n->option_rom_size) break;
        uint8_t sum = 0;
        for (size_t i = 0; i < image_len; i++) sum = (uint8_t)(sum + rom[image + i]);
        rom[image + image_len - 1] = (uint8_t)(rom[image + image_len - 1] - sum);
        if (rom[p + 0x15] & 0x80) break;
        image += image_len;
    }
    return true;
}

void i82559_destroy(I82559 *n) {
    free(n->option_rom);
    n->option_rom = NULL;
    n->option_rom_size = 0;
}

void i82559_pci_write(I82559 *n, unsigned reg, uint64_t value, unsigned size) {
    if (reg + size > sizeof(n->cfg))
        return;
    if (getenv("I82559_DEBUG"))
        fprintf(stderr, "i82559: pci write reg=%02x size=%u <- %08llx\n",
                reg, size, (unsigned long long)value);
    for (unsigned i = 0; i < size; i++) {
        unsigned off = reg + i;
        if (off >= 4 && !(off >= 8 && off < 16) && off < 0x40)
            n->cfg[off] = (uint8_t)(value >> (i * 8));
    }
}

uint32_t i82559_bar(const I82559 *n, unsigned reg, uint32_t fallback,
                    uint32_t mask) {
    uint32_t bar = 0;
    memcpy(&bar, n->cfg + reg, sizeof(bar));
    if (bar == UINT32_MAX || !(bar & mask))
        return fallback;
    return bar & mask;
}

static void eeprom_write(I82559 *n, uint8_t value) {
    bool old_clk = (n->ee_control & 1) != 0;
    bool cs = (value & 2) != 0;
    bool clk = (value & 1) != 0;
    if (!cs) {
        n->ee_bits = n->ee_out_bits = 0;
        n->ee_loaded = false;
        n->ee_shift = 0;
    } else if (!old_clk && clk) {
        if (!n->ee_out_bits) {
            n->ee_shift = (uint16_t)((n->ee_shift << 1) | ((value >> 2) & 1));
            if (++n->ee_bits == 10) { /* start + READ(2) + six-bit address */
                if ((n->ee_shift & 0x1c0) == 0x180) {
                    n->ee_out = n->eeprom[n->ee_shift & 0x3f];
                    n->ee_out_bits = 16;
                    n->ee_loaded = true;
                }
                n->ee_bits = 0;
                n->ee_shift = 0;
            }
        }
    } else if (old_clk && !clk && n->ee_out_bits) {
        if (n->ee_loaded)
            n->ee_loaded = false;
        else {
            n->ee_out <<= 1;
            n->ee_out_bits--;
        }
    }
    n->ee_control = value;
}

static uint16_t eeprom_read(const I82559 *n) {
    uint16_t v = n->ee_control;
    if (n->ee_out_bits && (n->ee_out & 0x8000))
        v |= 8;
    else
        v &= (uint16_t)~8u;
    return v;
}

static void complete_cb(I82559 *n, uint32_t addr) {
    if (!n->guest_read || !n->guest_write)
        return;
    uint16_t status = (uint16_t)n->guest_read(n->guest_opaque, addr, 2);
    n->guest_write(n->guest_opaque, addr, status | 0xa000u, 2);
    n->regs[1] |= 0xa0; /* command complete + CU no longer active */
}

static void command_write(I82559 *n, uint16_t command) {
    uint8_t ru = command & 0x0f;
    uint8_t cu = command & 0xf0;
    if (ru == 0x06) n->ru_base = n->scb_pointer;
    if (cu == 0x60) n->cu_base = n->scb_pointer;
    if (cu == 0x10) complete_cb(n, n->cu_base + n->scb_pointer);
    /* Hardware clears the command acceptance byte when accepted. */
    n->regs[SCB_COMMAND] = 0;
    n->regs[SCB_COMMAND + 1] = (uint8_t)(command >> 8);
}

static void mdi_write(I82559 *n, uint32_t value) {
    unsigned op = (value >> 26) & 3;
    unsigned phy = (value >> 21) & 0x1f;
    unsigned reg = (value >> 16) & 0x1f;
    uint16_t data = (uint16_t)value;
    if (phy == 1 && op == 2)
        data = n->phy[reg];
    else if (phy == 1 && op == 1) {
        if (reg == 0 && (data & 0x8000)) data = n->phy[0];
        n->phy[reg] = data;
    } else {
        data = 0;
    }
    n->mdi = (value & 0xffff0000u) | data | (1u << 28);
    n->regs[1] |= 0x08;
}

uint64_t i82559_reg_read(I82559 *n, unsigned reg, unsigned size) {
    uint64_t v = 0;
    if (reg + size > sizeof(n->regs))
        return UINT64_MAX;
    if (reg <= SCB_EEPROM && reg + size > SCB_EEPROM) {
        uint16_t ee = eeprom_read(n);
        memcpy(n->regs + SCB_EEPROM, &ee, sizeof(ee));
    }
    if (reg <= SCB_MDI + 3 && reg + size > SCB_MDI)
        memcpy(n->regs + SCB_MDI, &n->mdi, sizeof(n->mdi));
    memcpy(&v, n->regs + reg, size);
    if (getenv("I82559_DEBUG"))
        fprintf(stderr, "i82559: reg read %02x/%u -> %08llx\n", reg, size,
                (unsigned long long)v);
    return v;
}

void i82559_reg_write(I82559 *n, unsigned reg, uint64_t value, unsigned size) {
    if (reg + size > sizeof(n->regs))
        return;
    if (getenv("I82559_DEBUG"))
        fprintf(stderr, "i82559: reg write %02x/%u <- %08llx\n", reg, size,
                (unsigned long long)value);
    memcpy(n->regs + reg, &value, size);
    if (reg <= SCB_POINTER + 3 && reg + size > SCB_POINTER)
        memcpy(&n->scb_pointer, n->regs + SCB_POINTER, 4);
    if (reg <= SCB_EEPROM && reg + size > SCB_EEPROM)
        eeprom_write(n, n->regs[SCB_EEPROM]);
    if (reg <= SCB_MDI + 3 && reg + size >= SCB_MDI + 4) {
        uint32_t mdi;
        memcpy(&mdi, n->regs + SCB_MDI, 4);
        mdi_write(n, mdi);
    }
    if (reg <= SCB_PORT + 3 && reg + size >= SCB_PORT + 4) {
        uint32_t port;
        memcpy(&port, n->regs + SCB_PORT, 4);
        if ((port & 3) == 0 || (port & 3) == 2)
            i82559_reset_regs(n);
        else if ((port & 3) == 1 && n->guest_write) {
            uint32_t addr = port & ~3u;
            n->guest_write(n->guest_opaque, addr, 0, 4);
            n->guest_write(n->guest_opaque, addr + 4, 0, 4);
        }
    }
    if (reg <= SCB_COMMAND + 1 && reg + size > SCB_COMMAND)
        command_write(n, (uint16_t)(n->regs[SCB_COMMAND] |
                                    n->regs[SCB_COMMAND + 1] << 8));
}
