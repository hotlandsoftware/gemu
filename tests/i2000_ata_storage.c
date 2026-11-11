/* ROM-free ATA PIO storage regressions on a sparse 4 GiB disk.
 * Run: python3 tools/ia64/run_i2000_ata_storage_test.py
 * Exercise actual port traffic and independently inspect disk bytes. */
#include "../hardware/machine/ia64/machine_i2000.c"
#include <unistd.h>

static unsigned checks, transfers;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

enum { VECTOR = 0x70, SECTORS = 8388608 };

static bool irq(Ia64I2000State *s) {
    return (s->cpu->external_pending[VECTOR >> 3] >> (VECTOR & 7)) & 1;
}

/* IA-64 exposes port I/O through sparse physical MMIO. Test this decoder
 * too, rather than calling the ATA data-port implementation directly. */
static uint64_t port_address(unsigned port) {
    return I2000_IO_BASE + ((port & 0xfffu) | ((uint64_t)(port & ~3u) << 10));
}
static void port_write(Ia64I2000State *s, unsigned port, uint64_t value, unsigned width) {
    bus_write(s, port_address(port), value, width);
}
static uint64_t port_read(Ia64I2000State *s, unsigned port, unsigned width) {
    return bus_read(s, port_address(port), width);
}

static void task(Ia64I2000State *s, uint32_t lba, unsigned count, uint8_t cmd) {
    port_write(s, 0x1f6, 0xe0 | ((lba >> 24) & 15), 1);
    port_write(s, 0x1f2, count & 255, 1);
    port_write(s, 0x1f3, lba & 255, 1);
    port_write(s, 0x1f4, (lba >> 8) & 255, 1);
    port_write(s, 0x1f5, (lba >> 16) & 255, 1);
    port_write(s, 0x1f7, cmd, 1);
}

static void complete(Ia64I2000State *s, uint8_t status) {
    CHECK(!irq(s));
    s->cpu->ninsts += ATAPI_IRQ_LATENCY_NINSTS;
    i2000_poll_interrupts(s);
    CHECK(irq(s));
    merced_ack_external(s->cpu, VECTOR);
    CHECK(port_read(s, 0x1f7, 1) == status);
    i2000_poll_interrupts(s);
    CHECK(!irq(s));
}

static void put(Ia64I2000State *s, const uint8_t *buf, size_t n, unsigned width) {
    for (size_t i = 0; i < n; i += width) {
        uint64_t value = 0;
        for (unsigned j = 0; j < width; j++) value |= (uint64_t)buf[i + j] << (8 * j);
        port_write(s, 0x1f0, value, width);
    }
}

static void get(Ia64I2000State *s, uint8_t *buf, size_t n, unsigned width) {
    for (size_t i = 0; i < n; i += width) {
        uint64_t value = port_read(s, 0x1f0, width);
        for (unsigned j = 0; j < width; j++) buf[i + j] = (uint8_t)(value >> (8 * j));
    }
}

static void roundtrip(Ia64I2000State *s, uint32_t lba, unsigned count,
                      unsigned multiple, unsigned width) {
    size_t len = (size_t)count * 512;
    uint8_t *expected = malloc(len), *actual = malloc(len);
    uint8_t before[512], after[512], scratch[512];
    CHECK(expected && actual);
    uint32_t random = lba ^ (count << 16) ^ (multiple << 8) ^ width ^ 0x12a73e91;
    for (size_t i = 0; i < len; i++) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        expected[i] = random >> 24;
    }
    if (lba) CHECK(gemu_drive_read(s->hda, (uint64_t)(lba - 1) * 512, before, 512));
    if (lba + count < SECTORS)
        CHECK(gemu_drive_read(s->hda, (uint64_t)(lba + count) * 512, after, 512));

    task(s, 0, multiple, 0xc6);
    complete(s, 0x50);
    task(s, lba, count, multiple ? 0xc5 : 0x30);
    CHECK(port_read(s, 0x3f6, 1) == 0x58 && !irq(s));
    unsigned block = multiple ? multiple : 1;
    for (unsigned n = 0; n < count;) {
        unsigned chunk = count - n < block ? count - n : block;
        put(s, expected + n * 512, chunk * 512, width);
        n += chunk;
        complete(s, n == count ? 0x50 : 0x58);
    }
    task(s, 0, 0, 0xe7); /* FLUSH CACHE followed by an independent backing read. */
    complete(s, 0x50);
    FILE *backing = fopen("ata-storage.img", "rb");
    CHECK(backing);
    CHECK(fseeko(backing, (off_t)lba * 512, SEEK_SET) == 0);
    CHECK(fread(actual, 1, len, backing) == len);
    CHECK(fclose(backing) == 0);
    CHECK(memcmp(actual, expected, len) == 0);

    task(s, lba, count, multiple ? 0xc4 : 0x20);
    for (unsigned n = 0; n < count;) {
        unsigned chunk = count - n < block ? count - n : block;
        complete(s, 0x58);
        get(s, actual + n * 512, chunk * 512, width);
        n += chunk;
    }
    CHECK(port_read(s, 0x1f7, 1) == 0x50);
    CHECK(memcmp(actual, expected, len) == 0);
    if (lba) {
        CHECK(gemu_drive_read(s->hda, (uint64_t)(lba - 1) * 512, scratch, 512));
        CHECK(memcmp(before, scratch, 512) == 0);
    }
    if (lba + count < SECTORS) {
        CHECK(gemu_drive_read(s->hda, (uint64_t)(lba + count) * 512, scratch, 512));
        CHECK(memcmp(after, scratch, 512) == 0);
    }
    free(expected); free(actual);
    transfers++;
}

int main(void) {
    FILE *f = fopen("ata-storage.img", "w+b");
    CHECK(f && ftruncate(fileno(f), (off_t)SECTORS * 512) == 0);
    CHECK(fclose(f) == 0);
    CHECK(gemu_drive_add_legacy(GEMU_DRIVE_IF_IDE, 0, GEMU_DRIVE_MEDIA_DISK,
                               "ata-storage.img"));
    Ia64Config cfg = {.ram_size = UINT64_C(64) << 20,
                      .display_type = GEMU_DISPLAY_NONE};
    Ia64I2000State *s = ia64_i2000_create(&cfg);
    CHECK(s && s->hda);
    bus_write(s, IOSAPIC_BASE + IOSAPIC_REGSEL_OFF, 0x2c, 4);
    bus_write(s, IOSAPIC_BASE + IOSAPIC_WINDOW_OFF, VECTOR, 4);
    const unsigned counts[] = {1, 2, 15, 16, 17, 255, 256};
    const unsigned blocks[] = {0, 1, 2, 4, 8, 16};
    for (unsigned width = 1; width <= 8; width *= 2)
        for (unsigned b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++)
            for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
                unsigned n = counts[c];
                const uint32_t starts[] = {0, 255, 65535, 4194303, SECTORS - n};
                for (unsigned i = 0; i < sizeof(starts) / sizeof(starts[0]); i++)
                    roundtrip(s, starts[i], n, blocks[b], width);
            }
    ia64_i2000_destroy(s);
    printf("i2000 ATA storage: %u round trips, %u checks passed\n", transfers, checks);
    return 0;
}
