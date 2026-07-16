#include "i2000.h"
#include "gemu/monitor.h"
#include "gemu/util.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Ia64I2000State {
    GemuMonitor *monitor;

    uint8_t  *ram;
    uint64_t  ram_size;

    uint8_t  *flash;
    char      flash_file[512];
    uint32_t  flash_image_size;   /* bytes actually loaded from the file */
    bool      flash_loaded;
};

/* ── Physical address space ──────────────────────────────────────────────── */

uint8_t ia64_i2000_phys_read8(Ia64I2000State *s, uint64_t addr) {
    if (addr < s->ram_size)
        return s->ram[addr];
    if (addr - I2000_FLASH_BASE < I2000_FLASH_SIZE)
        return s->flash[addr - I2000_FLASH_BASE];
    /* Open bus: nothing decodes this address yet (460GX chipset registers,
     * PCI, legacy I/O all come later). */
    return 0xFF;
}

void ia64_i2000_phys_write8(Ia64I2000State *s, uint64_t addr, uint8_t val) {
    if (addr < s->ram_size)
        s->ram[addr] = val;
    /* Flash writes ignored - programming command cycles are a later
     * problem; everything else is open bus for now. */
}

/* ── Firmware ────────────────────────────────────────────────────────────── */

bool ia64_i2000_load_firmware(Ia64I2000State *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gemu: cannot open firmware '%s'\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || (uint64_t)len > I2000_FLASH_SIZE) {
        fprintf(stderr, "gemu: firmware '%s' is %ld bytes (expected at most %u)\n",
                path, len, I2000_FLASH_SIZE);
        fclose(f);
        return false;
    }

    /* Top-align: the reset bundles live in the file's last bytes and must
     * end exactly at 4 GiB. */
    uint32_t off = I2000_FLASH_SIZE - (uint32_t)len;
    memset(s->flash, 0xFF, off);
    size_t rd = fread(s->flash + off, 1, (size_t)len, f);
    fclose(f);
    if (rd != (size_t)len) {
        fprintf(stderr, "gemu: short read on firmware '%s'\n", path);
        return false;
    }

    snprintf(s->flash_file, sizeof(s->flash_file), "%s", path);
    s->flash_image_size = (uint32_t)len;
    s->flash_loaded = true;
    gemu_monitor_register_rom(s->monitor, (uint32_t)(I2000_FLASH_BASE + off),
                              (uint32_t)len, path);
    return true;
}

/* ── Monitor callbacks ───────────────────────────────────────────────────── */

static void i2000_cpu_state(void *ud, char *buf, size_t buf_len) {
    Ia64I2000State *s = ud;
    uint8_t bundle[16];
    for (int i = 0; i < 16; i++)
        bundle[i] = ia64_i2000_phys_read8(s, IA64_RESET_VECTOR + (uint64_t)i);

    snprintf(buf, buf_len,
             "HP i2000 - Intel Itanium (Merced) [skeleton: no CPU core yet]\n"
             "  RAM   : %" PRIu64 " MiB @ 0x0000000000000000\n"
             "  Flash : %s (%u bytes @ 0x%08" PRIX64 ")\n"
             "  Reset : IP=0x00000000%08" PRIX64 " (PALE_RESET)\n"
             "  Bundle: %02X %02X %02X %02X %02X %02X %02X %02X "
             "%02X %02X %02X %02X %02X %02X %02X %02X\n",
             s->ram_size >> 20,
             s->flash_loaded ? s->flash_file : "(none)",
             s->flash_image_size,
             I2000_FLASH_BASE + (I2000_FLASH_SIZE - s->flash_image_size),
             (uint64_t)IA64_RESET_VECTOR,
             bundle[0], bundle[1], bundle[2],  bundle[3],
             bundle[4], bundle[5], bundle[6],  bundle[7],
             bundle[8], bundle[9], bundle[10], bundle[11],
             bundle[12], bundle[13], bundle[14], bundle[15]);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

Ia64I2000State *ia64_i2000_create(const Ia64Config *cfg) {
    Ia64I2000State *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->ram_size = cfg->ram_size;
    s->ram = calloc(1, (size_t)s->ram_size);
    s->flash = malloc(I2000_FLASH_SIZE);
    if (!s->ram || !s->flash) {
        fprintf(stderr, "gemu: cannot allocate %" PRIu64 " MiB guest RAM\n",
                cfg->ram_size >> 20);
        ia64_i2000_destroy(s);
        return NULL;
    }
    memset(s->flash, 0xFF, I2000_FLASH_SIZE);

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_cpu_state_cb(s->monitor, i2000_cpu_state, s);
    return s;
}

void ia64_i2000_destroy(Ia64I2000State *s) {
    if (!s)
        return;
    gemu_monitor_destroy(s->monitor);
    free(s->flash);
    free(s->ram);
    free(s);
}

void ia64_i2000_run(Ia64I2000State *s, const Ia64Config *cfg) {
    printf("gemu-ia64: HP i2000, Intel Itanium (Merced), 460GX chipset [skeleton]\n"
           "  RAM   : %" PRIu64 " MiB\n"
           "  Flash : %s (mapped at 0x%08" PRIX64 "-0xFFFFFFFF)\n"
           "  Reset : IP=0x00000000%08" PRIX64 " - no CPU core yet, idling\n",
           s->ram_size >> 20,
           s->flash_loaded ? s->flash_file : "(none)",
           (uint64_t)I2000_FLASH_BASE, (uint64_t)IA64_RESET_VECTOR);

    gemu_monitor_start(s->monitor);

    bool running = true;
    while (running) {
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            switch (cmd) {
            case GEMU_MON_QUIT:
                if (cfg->no_shutdown) {
                    gemu_monitor_shutdown_or_pause(s->monitor, true);
                } else {
                    running = false;
                }
                break;
            case GEMU_MON_RESET:
                /* RAM contents survive a reset on real hardware; with no
                 * CPU state to clear yet this is a no-op. */
                printf("i2000: reset (no CPU core yet)\n");
                break;
            case GEMU_MON_STEP:
                printf("i2000: cannot step - no CPU core yet\n");
                break;
            case GEMU_MON_CUSTOM:
                gemu_monitor_unknown_command(s->monitor);
                break;
            default:
                break;
            }
            if (!running)
                break;
        }
        gemu_sleep_ms(10);
    }
    gemu_monitor_stop(s->monitor);
}
