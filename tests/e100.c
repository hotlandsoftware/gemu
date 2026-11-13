/* Standalone Intel 82559 model: no CPU, machine, ROM file or host UI is linked.
 * Run: python3 tools/ia64/run_e100_test.py */
#include "network/e100.h"
#include "network/pci_rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

enum {
    CSR_BASE = 0xE0000000u, FLASH_BASE = 0xE0100000u, ROM_BASE = 0xE0200000u,
    IO_BASE = 0x2000u,
    CLOCK_HZ = 1000000u,            /* the fake clock counts microseconds */
    MEM_SIZE = 0x100000u,
};

/* CSR offsets (SDM 6.3.1 Table 11). */
enum {
    CSR_STATUS = 0x00, CSR_ACK = 0x01, CSR_CMD = 0x02, CSR_ICTL = 0x03,
    CSR_GP = 0x04, CSR_PORT = 0x08, CSR_EEPROM = 0x0E, CSR_MDI = 0x10,
    CSR_GSTS = 0x1D,
};

/* STAT/ACK bits. */
enum {
    ST_CX = 0x80, ST_FR = 0x40, ST_CNA = 0x20, ST_RNR = 0x10, ST_MDI = 0x08,
    ST_SWI = 0x04,
};

typedef struct {
    uint64_t ticks;
    bool irq;
    unsigned irq_edges;
    uint8_t mem[MEM_SIZE];          /* guest memory behind the DMA callbacks */
    uint8_t tx[4][2048];            /* frames the device put on the wire */
    size_t txlen[4];
    unsigned ntx;
    uint8_t rx[4][2048];            /* frames waiting to be received */
    size_t rxlen[4];
    unsigned rx_head, nrx;
} Env;

static Env env;
static E100 nic;

static uint64_t clock_read(void *opaque) {
    return ((Env *)opaque)->ticks;
}

static void irq_set(void *opaque, bool level) {
    Env *e = opaque;
    e->irq = level;
    e->irq_edges++;
}

static bool dma_read(void *opaque, uint64_t addr, void *buf, unsigned len) {
    Env *e = opaque;
    if (addr + len > MEM_SIZE) {
        return false;
    }
    memcpy(buf, e->mem + addr, len);
    return true;
}

static bool dma_write(void *opaque, uint64_t addr, const void *buf, unsigned len) {
    Env *e = opaque;
    if (addr + len > MEM_SIZE) {
        return false;
    }
    memcpy(e->mem + addr, buf, len);
    return true;
}

static bool tx_frame(void *opaque, const uint8_t *frame, size_t len) {
    Env *e = opaque;
    CHECK(e->ntx < 4 && len <= sizeof e->tx[0]);
    memcpy(e->tx[e->ntx], frame, len);
    e->txlen[e->ntx++] = len;
    return true;
}

static size_t rx_frame(void *opaque, uint8_t *frame, size_t cap) {
    Env *e = opaque;
    if (!e->nrx) {
        return 0;
    }
    size_t len = e->rxlen[e->rx_head];
    CHECK(len <= cap);
    memcpy(frame, e->rx[e->rx_head], len);
    e->rx_head = (e->rx_head + 1) % 4;
    e->nrx--;
    return len;
}

static const E100Wiring wiring = {
    .clock = clock_read, .clock_hz = CLOCK_HZ, .irq = irq_set,
    .dma_read = dma_read, .dma_write = dma_write, .tx = tx_frame, .rx = rx_frame,
    .opaque = &env,
};

static const uint8_t MAC[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

static E100Config config(void) {
    E100Config c = { .subsys_vendor = 0x8086, .subsys_id = 0x3001, .boot_rom = true };
    memcpy(c.mac, MAC, 6);
    return c;
}

static void power_on_with(const E100Config *c) {
    memset(&env, 0, sizeof env);
    e100_init(&nic, c, &wiring);
}

static void power_on(void) {
    E100Config c = config();
    power_on_with(&c);
}

static uint32_t cfg_r(unsigned reg, unsigned len) {
    return e100_cfg_read(&nic, reg, len);
}

static void cfg_w(unsigned reg, unsigned len, uint32_t val) {
    e100_cfg_write(&nic, reg, len, val);
}

static uint64_t mem_r(uint64_t addr, unsigned len) {
    uint64_t v = 0xDEADDEADDEADDEADull;
    CHECK(e100_mem_access(&nic, false, addr, len, &v));
    return v;
}

static void mem_w(uint64_t addr, unsigned len, uint64_t v) {
    CHECK(e100_mem_access(&nic, true, addr, len, &v));
}

static uint64_t io_r(uint16_t port, unsigned len) {
    uint64_t v = 0xDEADDEADDEADDEADull;
    CHECK(e100_io_access(&nic, false, port, len, &v));
    return v;
}

static bool mem_claimed(uint64_t addr) {
    uint64_t v = 0;
    return e100_mem_access(&nic, false, addr, 1, &v);
}

static uint32_t csr_r(unsigned off, unsigned len) {
    return (uint32_t)mem_r(CSR_BASE + off, len);
}

static void csr_w(unsigned off, unsigned len, uint32_t v) {
    mem_w(CSR_BASE + off, len, v);
}

/* Place the BARs and enable I/O, memory and bus mastering. */
static void map_card(void) {
    cfg_w(0x10, 4, CSR_BASE);
    cfg_w(0x14, 4, IO_BASE);
    cfg_w(0x18, 4, FLASH_BASE);
    cfg_w(0x30, 4, ROM_BASE | 1u);
    cfg_w(0x04, 2, 0x0007);
}

/* Time passes: `us` microseconds of the fake clock, then the device polls. */
static void advance_us(uint64_t us) {
    env.ticks += us;
    e100_poll(&nic);
}

static void advance_ms(uint64_t ms) {
    advance_us(ms * 1000);
}

/* Guest memory, little endian. */
static void m16(uint32_t a, unsigned v) {
    env.mem[a] = (uint8_t)v;
    env.mem[a + 1] = (uint8_t)(v >> 8);
}

static void m32(uint32_t a, uint32_t v) {
    m16(a, v & 0xFFFF);
    m16(a + 2, v >> 16);
}

static unsigned g16(uint32_t a) {
    return env.mem[a] | env.mem[a + 1] << 8;
}

static uint32_t g32(uint32_t a) {
    return g16(a) | (uint32_t)g16(a + 2) << 16;
}

/* ---- PCI configuration, BARs, decode ------------------------------------------ */

static void test_config(void) {
    power_on();
    CHECK(cfg_r(0x00, 4) == 0x12298086u);
    CHECK(cfg_r(0x00, 2) == E100_VENDOR_ID && cfg_r(0x02, 2) == E100_DEVICE_ID);
    CHECK(cfg_r(0x04, 2) == 0x0000);
    CHECK(cfg_r(0x06, 2) == 0x0290);                    /* fast back-to-back, caps, DEVSEL medium */
    CHECK(cfg_r(0x08, 4) == 0x02000008u);               /* class 0200, revision 08 */
    CHECK(cfg_r(0x0E, 1) == 0x00);                      /* header type 0, single function */
    CHECK(cfg_r(0x10, 4) == 0 && cfg_r(0x14, 4) == 1);  /* memory BAR, I/O BAR */
    CHECK(cfg_r(0x2C, 4) == 0x30018086u);               /* subsystem from the EEPROM */
    CHECK(cfg_r(0x34, 1) == 0xDC);
    CHECK(cfg_r(0x3C, 4) == 0x18080100u);               /* Max_Lat, Min_Gnt, INTA#, line */
    CHECK(cfg_r(0xDC, 4) == 0x7E210001u);               /* PM capability, last in the list */
    CHECK(cfg_r(0x100, 4) == 0xFFFFFFFFu);              /* beyond configuration space */
    CHECK(cfg_r(0x40, 4) == 0);

    /* Writable bits: Command 8, 6, 4, 2, 1, 0; Cache Line Size 8 or 16 only;
     * Latency Timer and Interrupt Line; Status 15-12 and 8 clear on write. */
    cfg_w(0x04, 2, 0xFFFF);
    CHECK(cfg_r(0x04, 2) == 0x0157);
    cfg_w(0x04, 2, 0);
    cfg_w(0x0C, 1, 16);
    CHECK(cfg_r(0x0C, 1) == 16);
    cfg_w(0x0C, 1, 12);
    CHECK(cfg_r(0x0C, 1) == 0);
    cfg_w(0x0D, 1, 0x40);
    cfg_w(0x3C, 1, 0x0B);
    CHECK(cfg_r(0x0D, 1) == 0x40 && cfg_r(0x3C, 1) == 0x0B);
    cfg_w(0x00, 4, 0xFFFFFFFFu);                        /* identity is read-only */
    cfg_w(0x2C, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x00, 4) == 0x12298086u && cfg_r(0x2C, 4) == 0x30018086u);

    /* PCI RST# returns the header to power-on. */
    cfg_w(0x04, 2, 0x0007);
    e100_reset(&nic);
    CHECK(cfg_r(0x04, 2) == 0 && cfg_r(0x0D, 1) == 0 && cfg_r(0x3C, 1) == 0);

    /* The header's identity comes from the autoloaded EEPROM word 0Ah (DS
     * 8.1.12): AltID clear reports a 1029h part, ID supplies revision bits
     * 2:0 from bits 10:8, and words 0Bh/0Ch are the subsystem IDs. */
    uint16_t image[64] = { 0 };
    image[0x0A] = 0x4000 | 0x2000 | 0x0300 | 0x0800;
    image[0x0B] = 0xBEEF;
    image[0x0C] = 0xCAFE;
    E100Config c = config();
    c.eeprom_image = image;
    power_on_with(&c);
    CHECK(cfg_r(0x00, 4) == 0x10298086u);
    CHECK(cfg_r(0x08, 1) == 0x0B);
    CHECK(cfg_r(0x2C, 4) == 0xBEEFCAFEu);
    cfg_w(0x30, 4, 0xFFFFFFFFu);                        /* Boot Disable set: no ROM BAR */
    CHECK(cfg_r(0x30, 4) == 0);

    /* No valid signature: the silicon identity stands. */
    memset(image, 0, sizeof image);
    c.device_id = 0x1209;
    c.revision = 0x05;
    power_on_with(&c);
    CHECK(cfg_r(0x00, 4) == 0x12098086u && cfg_r(0x08, 1) == 0x05);
    CHECK(cfg_r(0x2C, 4) == 0);
}

static void test_bars(void) {
    power_on();
    cfg_w(0x10, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x10, 4) == 0xFFFFF000u);               /* 4 KiB memory */
    cfg_w(0x14, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x14, 4) == 0xFFFFFFC1u);               /* 64 bytes I/O */
    cfg_w(0x18, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x18, 4) == 0xFFF00000u);               /* 1 MiB flash */
    for (unsigned reg = 0x1C; reg < 0x28; reg += 4) {
        cfg_w(reg, 4, 0xFFFFFFFFu);
        CHECK(cfg_r(reg, 4) == 0);                      /* not implemented */
    }
    cfg_w(0x30, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x30, 4) == 0xFFF00001u);               /* 1 MiB, enable bit */
    cfg_w(0x30, 4, 0xFFFFFFFEu);
    CHECK(cfg_r(0x30, 4) == 0xFFF00000u);

    /* Addresses: the bits below the size are not settable; BARs take byte
     * enables. */
    cfg_w(0x10, 4, 0xE0000123u);
    CHECK(cfg_r(0x10, 4) == 0xE0000000u);
    cfg_w(0x14, 4, 0x2000u | 0x3Fu);
    CHECK(cfg_r(0x14, 4) == 0x2001u);
    cfg_w(0x12, 2, 0xD000u);                            /* the upper half only */
    CHECK(cfg_r(0x10, 4) == 0xD0000000u);
    cfg_w(0x30, 1, 0x01);                               /* enable with a one-byte write */
    CHECK((cfg_r(0x30, 4) & 1) == 1);

    /* Without a boot ROM the base register is read-only zero. */
    E100Config c = config();
    c.boot_rom = false;
    power_on_with(&c);
    cfg_w(0x30, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x30, 4) == 0);

    /* A BAR that decodes in D0 stops in D3 and reset by D3 -> D0 (SDM 4.1.18). */
    power_on();
    map_card();
    CHECK(mem_claimed(CSR_BASE));
    cfg_w(0xE0, 1, 3);
    CHECK(cfg_r(0xE0, 1) == 3);
    CHECK(!mem_claimed(CSR_BASE));
    uint64_t v;
    CHECK(!e100_io_access(&nic, false, IO_BASE, 1, &v));
    cfg_w(0xE0, 1, 0);
    CHECK(cfg_r(0x04, 2) == 0 && cfg_r(0x10, 4) == 0 && cfg_r(0x14, 4) == 1);
    CHECK(cfg_r(0x30, 4) == 0);
    CHECK(!mem_claimed(CSR_BASE));
}

static void test_decode(void) {
    power_on();
    uint64_t a, as, b, bs, v;
    CHECK(!e100_mem_ranges(&nic, &a, &as, &b, &bs));    /* COMMAND.MEM clear */
    cfg_w(0x04, 2, 0x0002);
    CHECK(e100_mem_ranges(&nic, &a, &as, &b, &bs));
    CHECK(as == 0 && bs == 0);                          /* nothing placed yet */
    CHECK(!mem_claimed(CSR_BASE));
    cfg_w(0x10, 4, CSR_BASE);
    cfg_w(0x18, 4, FLASH_BASE);
    CHECK(e100_mem_ranges(&nic, &a, &as, &b, &bs));
    CHECK(a == CSR_BASE && as == 0x1000 && b == FLASH_BASE && bs == 0x100000);
    CHECK(mem_claimed(CSR_BASE) && mem_claimed(CSR_BASE + 0xFFF));
    CHECK(!mem_claimed(CSR_BASE + 0x1000) && !mem_claimed(CSR_BASE - 1));
    CHECK(mem_claimed(FLASH_BASE + 0xFFFFF));
    CHECK(!e100_mem_access(&nic, false, CSR_BASE, 3, &v));  /* odd access size */

    /* I/O decode needs COMMAND.IO and a placed BAR. */
    CHECK(!e100_io_access(&nic, false, IO_BASE, 4, &v));
    cfg_w(0x14, 4, IO_BASE);
    CHECK(!e100_io_access(&nic, false, IO_BASE, 4, &v));
    cfg_w(0x04, 2, 0x0001);
    CHECK(e100_io_access(&nic, false, IO_BASE, 4, &v));
    CHECK(e100_io_access(&nic, false, IO_BASE + 0x3F, 1, &v));
    CHECK(!e100_io_access(&nic, false, IO_BASE + 0x40, 1, &v));
    CHECK(!e100_io_access(&nic, false, IO_BASE - 1, 1, &v));
    CHECK(!mem_claimed(CSR_BASE));                      /* memory decode is off again */
    CHECK(!e100_mem_ranges(&nic, &a, &as, &b, &bs));

    /* The same registers through memory and I/O. */
    cfg_w(0x04, 2, 0x0003);
    csr_w(CSR_GP, 4, 0x12345678u);
    CHECK(io_r(IO_BASE + CSR_GP, 4) == 0x12345678u);
    CHECK(io_r(IO_BASE + CSR_GP + 1, 1) == 0x56);
    CHECK(io_r(IO_BASE + CSR_GP + 2, 2) == 0x1234);
}

static void test_rom(void) {
    static uint8_t image[0x30000];
    for (size_t i = 0; i < sizeof image; i++) {
        image[i] = (uint8_t)(i * 7 + 3);
    }
    power_on();
    e100_set_rom(&nic, image, 0x1800);
    uint64_t base, size;
    CHECK(!e100_rom_range(&nic, &base, &size));         /* disabled at reset */
    cfg_w(0x30, 4, ROM_BASE | 1u);
    CHECK(!e100_rom_range(&nic, &base, &size));         /* COMMAND.MEM still clear */
    cfg_w(0x04, 2, 0x0002);
    CHECK(e100_rom_range(&nic, &base, &size));
    CHECK(base == ROM_BASE && size == 0x100000);
    CHECK(mem_r(ROM_BASE, 1) == image[0]);
    CHECK(mem_r(ROM_BASE + 0x10, 4) == (image[0x10] | image[0x11] << 8 |
                                         image[0x12] << 16 | (uint32_t)image[0x13] << 24));
    CHECK(mem_r(ROM_BASE + 0x17FF, 1) == image[0x17FF]);
    CHECK(mem_r(ROM_BASE + 0x1800, 1) == 0xFF);         /* past the image: erased */
    CHECK(mem_r(ROM_BASE + 0x20000 + 5, 1) == image[5]); /* 128 KiB part, aliased */
    mem_w(ROM_BASE, 1, 0x00);                           /* read-only */
    CHECK(mem_r(ROM_BASE, 1) == image[0]);

    /* The flash BAR shows the same part. */
    cfg_w(0x18, 4, FLASH_BASE);
    CHECK(mem_r(FLASH_BASE + 0x20, 2) == (unsigned)(image[0x20] | image[0x21] << 8));
    CHECK(mem_r(FLASH_BASE + 0xE0000 + 1, 1) == image[1]);
    mem_w(FLASH_BASE, 1, 0x00);                         /* no command set: ignored */
    CHECK(mem_r(FLASH_BASE, 1) == image[0]);

    /* The image is clamped to the part. */
    e100_set_rom(&nic, image, sizeof image);
    CHECK(mem_r(ROM_BASE + 0x1FFFF, 1) == image[0x1FFFF]);
    CHECK(mem_r(ROM_BASE + 0x20000, 1) == image[0]);

    /* Disabling the ROM BAR or memory decode takes it off the bus. */
    cfg_w(0x30, 4, ROM_BASE);
    CHECK(!e100_rom_range(&nic, &base, &size));
    CHECK(!mem_claimed(ROM_BASE));
    cfg_w(0x30, 4, ROM_BASE | 1u);
    CHECK(mem_claimed(ROM_BASE));

    /* No flash image: erased everywhere. */
    power_on();
    map_card();
    CHECK(mem_r(FLASH_BASE, 4) == 0xFFFFFFFFu);
    CHECK(mem_r(ROM_BASE + 0x80, 2) == 0xFFFF);

    /* Boot Disable: the ROM BAR does not exist whatever the image. */
    E100Config c = config();
    c.boot_rom = false;
    power_on_with(&c);
    e100_set_rom(&nic, image, 0x1000);
    cfg_w(0x04, 2, 0x0002);
    cfg_w(0x30, 4, ROM_BASE | 1u);
    CHECK(!e100_rom_range(&nic, &base, &size));
    CHECK(!mem_claimed(ROM_BASE));
}

/* ---- serial EEPROM ---------------------------------------------------------- */

enum { EE_SK = 1, EE_CS = 2, EE_DI = 4, EE_DO = 8 };

static void ee_put(unsigned v) {
    csr_w(CSR_EEPROM, 1, v);
}

static unsigned ee_get(void) {
    return csr_r(CSR_EEPROM, 1);
}

/* One clock on a selected part; returns EEDO sampled after the rising edge. */
static unsigned ee_clk(unsigned di) {
    unsigned d = di ? EE_DI : 0;
    ee_put(EE_CS | d);
    ee_put(EE_CS | EE_SK | d);
    unsigned out = (ee_get() & EE_DO) ? 1 : 0;
    ee_put(EE_CS | d);
    return out;
}

static void ee_select(void) {
    ee_put(0);
    ee_put(EE_CS);
}

static void ee_deselect(void) {
    ee_put(0);
}

/* Start bit, opcode and address; returns EEDO after the last address bit,
 * which is the part's dummy zero for a read. */
static unsigned ee_cmd(unsigned op, unsigned addr, unsigned abits) {
    unsigned out = 1;
    ee_clk(1);
    ee_clk(op >> 1);
    ee_clk(op & 1);
    for (int i = (int)abits - 1; i >= 0; i--) {
        out = ee_clk((addr >> i) & 1);
    }
    return out;
}

static unsigned ee_read(unsigned addr, unsigned abits) {
    ee_select();
    CHECK(ee_cmd(2, addr, abits) == 0);
    unsigned v = 0;
    for (int i = 0; i < 16; i++) {
        v = v << 1 | ee_clk(0);
    }
    ee_deselect();
    return v;
}

/* The size probe of SDM 6.3.4.2: EEDO reads 1 until the dummy zero. */
static unsigned ee_probe_abits(void) {
    ee_select();
    ee_clk(1);
    ee_clk(1);
    ee_clk(0);
    unsigned n = 0, out;
    do {
        out = ee_clk(0);
        n++;
    } while (out && n < 16);
    ee_deselect();
    return n;
}

/* Program-class commands: EWEN/EWDS/ERAL/WRAL (opcode 00, address bits 11/00/10/01
 * followed by don't-cares), WRITE and ERASE; 16 data bits for WRITE and WRAL. */
static void ee_program(unsigned op, unsigned addr, unsigned abits, bool data, unsigned val) {
    ee_select();
    ee_cmd(op, addr, abits);
    if (data) {
        for (int i = 15; i >= 0; i--) {
            ee_clk((val >> i) & 1);
        }
    }
    ee_deselect();
    ee_select();                                        /* busy/ready: EEDO high */
    CHECK(ee_get() & EE_DO);
    ee_deselect();
}

static void ee_ewen(unsigned abits) { ee_program(0, 3u << (abits - 2), abits, false, 0); }
static void ee_ewds(unsigned abits) { ee_program(0, 0, abits, false, 0); }
static void ee_eral(unsigned abits) { ee_program(0, 2u << (abits - 2), abits, false, 0); }
static void ee_wral(unsigned abits, unsigned v) { ee_program(0, 1u << (abits - 2), abits, true, v); }
static void ee_write(unsigned addr, unsigned abits, unsigned v) { ee_program(1, addr, abits, true, v); }
static void ee_erase(unsigned addr, unsigned abits) { ee_program(3, addr, abits, false, 0); }

/* The image AP-394 Table 1 gives for the test configuration. */
static void expected_eeprom(uint16_t *w, unsigned words) {
    memset(w, 0, words * sizeof *w);
    w[0] = MAC[0] | MAC[1] << 8;
    w[1] = MAC[2] | MAC[3] << 8;
    w[2] = MAC[4] | MAC[5] << 8;
    w[3] = 0x0003;
    w[5] = 0x0201;
    w[6] = 0x4701;
    w[0x0A] = 0x4080;                                   /* signature, AltID; boot ROM enabled */
    w[0x0B] = 0x3001;
    w[0x0C] = 0x8086;
    uint16_t sum = 0;
    for (unsigned i = 0; i + 1 < words; i++) {
        sum = (uint16_t)(sum + w[i]);
    }
    w[words - 1] = (uint16_t)(0xBABA - sum);
}

static void test_eeprom_read(void) {
    power_on();
    map_card();
    CHECK(ee_probe_abits() == 6);
    uint16_t want[256];
    expected_eeprom(want, 64);
    uint16_t sum = 0;
    for (unsigned i = 0; i < 64; i++) {
        unsigned v = ee_read(i, 6);
        CHECK(v == want[i]);
        sum = (uint16_t)(sum + v);
    }
    CHECK(sum == 0xBABA);                               /* AP 2.6.6 */

    /* Read-out continues into the next word while the clock runs. */
    ee_select();
    CHECK(ee_cmd(2, 0, 6) == 0);
    unsigned v0 = 0, v1 = 0;
    for (int i = 0; i < 16; i++) {
        v0 = v0 << 1 | ee_clk(0);
    }
    for (int i = 0; i < 16; i++) {
        v1 = v1 << 1 | ee_clk(0);
    }
    ee_deselect();
    CHECK(v0 == want[0] && v1 == want[1]);

    /* Zeros ahead of the start bit are ignored (Linux e100_eeprom_read). */
    ee_select();
    ee_clk(0);
    ee_clk(0);
    CHECK(ee_cmd(2, 0x0A, 6) == 0);
    unsigned w = 0;
    for (int i = 0; i < 16; i++) {
        w = w << 1 | ee_clk(0);
    }
    ee_deselect();
    CHECK(w == want[0x0A]);

    /* The EEPROM control register through 16-bit accesses, as HP-UX does. */
    csr_w(CSR_EEPROM, 2, EE_CS);
    CHECK((csr_r(CSR_EEPROM, 2) & 0xFFF7) == EE_CS);
    CHECK(csr_r(CSR_EEPROM, 2) & EE_DO);                /* floats high while selected */
    csr_w(CSR_EEPROM, 2, 0);

    /* A 256-word part has 8 address bits. */
    E100Config c = config();
    c.eeprom_words = 256;
    power_on_with(&c);
    map_card();
    CHECK(ee_probe_abits() == 8);
    expected_eeprom(want, 256);
    sum = 0;
    for (unsigned i = 0; i < 256; i++) {
        unsigned v = ee_read(i, 8);
        CHECK(v == want[i]);
        sum = (uint16_t)(sum + v);
    }
    CHECK(sum == 0xBABA);
    CHECK(ee_read(0xFF, 8) == want[255]);
}

static void test_eeprom_write(void) {
    power_on();
    map_card();
    const unsigned A = 6;
    uint16_t want[64];
    expected_eeprom(want, 64);

    /* Powers up write-disabled. */
    ee_write(0x20, A, 0x1234);
    CHECK(ee_read(0x20, A) == want[0x20]);

    ee_ewen(A);
    ee_write(0x20, A, 0x1234);
    CHECK(ee_read(0x20, A) == 0x1234);
    CHECK(ee_read(0x21, A) == want[0x21]);

    /* A write is programmed on the falling edge of CS: one cut short is not. */
    ee_select();
    ee_cmd(1, 0x22, A);
    for (int i = 0; i < 10; i++) {
        ee_clk(1);
    }
    ee_deselect();
    CHECK(ee_read(0x22, A) == want[0x22]);

    ee_erase(0x20, A);
    CHECK(ee_read(0x20, A) == 0xFFFF);

    ee_ewds(A);
    ee_write(0x20, A, 0x4321);
    CHECK(ee_read(0x20, A) == 0xFFFF);
    ee_erase(0x21, A);
    CHECK(ee_read(0x21, A) == want[0x21]);
    ee_eral(A);
    CHECK(ee_read(0x00, A) == want[0x00] && ee_read(0x3F, A) == want[0x3F]);

    ee_ewen(A);
    ee_wral(A, 0xA55A);
    for (unsigned i = 0; i < 64; i++) {
        CHECK(ee_read(i, A) == 0xA55A);
    }
    ee_eral(A);
    for (unsigned i = 0; i < 64; i++) {
        CHECK(ee_read(i, A) == 0xFFFF);
    }
    ee_ewds(A);
    ee_wral(A, 0x0000);
    CHECK(ee_read(7, A) == 0xFFFF);

    /* The 256-word part: the same commands with 8 address bits. */
    E100Config c = config();
    c.eeprom_words = 256;
    power_on_with(&c);
    map_card();
    ee_ewen(8);
    ee_write(0xC3, 8, 0xBEEF);
    CHECK(ee_read(0xC3, 8) == 0xBEEF && ee_read(0x43, 8) == 0x0000);
    ee_wral(8, 0x1111);
    CHECK(ee_read(0x00, 8) == 0x1111 && ee_read(0xFF, 8) == 0x1111);
    ee_ewds(8);

    /* The autoload at PCI RST# takes the station address and header IDs from
     * the (rewritten) EEPROM. */
    power_on();
    map_card();
    ee_ewen(A);
    ee_write(0x0B, A, 0x7777);
    ee_write(0x0C, A, 0x1AF4);
    ee_ewds(A);
    e100_reset(&nic);
    CHECK(cfg_r(0x2C, 4) == 0x77771AF4u);
    map_card();
    CHECK(ee_read(0x0B, A) == 0x7777);                  /* the EEPROM survives the reset */
}

/* ---- MDI and the PHY ------------------------------------------------------------ */

static unsigned mdi(unsigned op, unsigned phy, unsigned reg, unsigned data) {
    csr_w(CSR_MDI, 4, (uint32_t)(op << 26 | phy << 21 | reg << 16 | data));
    uint32_t r = csr_r(CSR_MDI, 4);
    CHECK(r & (1u << 28));                              /* Ready */
    CHECK(((r >> 16) & 0xFFF) == (op << 10 | phy << 5 | reg));
    return r & 0xFFFF;
}

static unsigned phy_r(unsigned reg) { return mdi(2, 1, reg, 0); }
static void phy_w(unsigned reg, unsigned v) { mdi(1, 1, reg, v); }

static void test_phy(void) {
    power_on();
    map_card();
    CHECK(csr_r(CSR_MDI, 4) == (1u << 28));             /* Ready after reset */
    CHECK(phy_r(2) == 0x02A8 && phy_r(3) == 0x0154);    /* PHY identifier */
    CHECK(phy_r(0) == 0x3000);                          /* 100 Mb/s, auto-negotiation on */
    CHECK(phy_r(4) == 0x05E1);
    CHECK(mdi(2, 0, 2, 0) == 0xFFFF);                   /* no PHY at address 0 */
    CHECK(phy_r(18) == 1);
    CHECK(phy_r(31) == 0);                              /* reserved register */

    /* Auto-negotiation has to run: link is down and the exchange not done. */
    CHECK(phy_r(1) == 0x7809);
    CHECK(phy_r(5) == 0);
    CHECK((csr_r(CSR_GSTS, 1) & 1) == 0);
    advance_ms(973);
    CHECK((phy_r(1) & 0x24) == 0);
    advance_ms(2);                                      /* past the 974 ms of link timing */
    CHECK(phy_r(1) == 0x782D);                          /* link up, auto-negotiation complete */
    CHECK(phy_r(5) == 0x41E1);                          /* the partner's word */
    CHECK(phy_r(6) == 0x0003);                          /* page received, partner able; self-clears */
    CHECK(phy_r(6) == 0x0001);
    CHECK(csr_r(CSR_GSTS, 1) == 0x07);                  /* link, 100 Mb/s, full duplex */
    CHECK((phy_r(16) & 3) == 3);

    /* Registers: the advertisement's selector is fixed; writing it and
     * restarting renegotiates from an up link, which first drops the link and
     * holds it down for 1.2 s. */
    phy_w(4, 0x0061);                                   /* 10 half and full */
    CHECK(phy_r(4) == 0x0061);
    phy_w(0, 0x1200);                                   /* auto-negotiation enable + restart */
    CHECK(phy_r(0) == 0x1000);                          /* the restart bit self-clears */
    CHECK(phy_r(1) == 0x7809);                          /* link dropped: latched low, then down */
    advance_ms(2170);
    CHECK((phy_r(1) & 4) == 0);
    advance_ms(10);
    CHECK(phy_r(1) == 0x782D);
    CHECK(csr_r(CSR_GSTS, 1) == 0x05);                  /* link, 10 Mb/s, full duplex */
    CHECK((phy_r(16) & 3) == 1);

    /* Forced mode skips the exchange: the link-fail inhibit timer (750 ms)
     * after the 1.2 s the link is held down. */
    phy_w(0, 0x2100);                                   /* forced 100 Mb/s full duplex */
    CHECK(phy_r(1) == 0x7809);
    advance_ms(1949);
    CHECK((phy_r(1) & 4) == 0);
    advance_ms(2);
    CHECK((phy_r(1) & 0x24) == 0x04);                   /* link, no auto-negotiation done */
    CHECK(csr_r(CSR_GSTS, 1) == 0x07);

    /* A reset through the PHY returns the default and re-negotiates. */
    phy_w(0, 0x8000);
    CHECK(phy_r(0) == 0x3000);
    CHECK((phy_r(1) & 4) == 0);

    /* The MDI interrupt-enable bit posts STAT.MDI. */
    csr_w(CSR_ACK, 1, 0xFF);
    mdi(2, 1, 2, 0);
    CHECK(csr_r(CSR_ACK, 1) == 0 && !env.irq);
    csr_w(CSR_MDI, 4, (uint32_t)(2u << 26 | 1u << 21 | 2u << 16 | 1u << 29));
    CHECK(csr_r(CSR_ACK, 1) == ST_MDI && env.irq);
    csr_w(CSR_ACK, 1, ST_MDI);
    CHECK(!env.irq);
}

/* ---- the SCB: interrupts, Command Unit, Receive Unit ------------------------------ */

static void scb(unsigned cmd) {
    csr_w(CSR_CMD, 1, cmd);
    CHECK(csr_r(CSR_CMD, 1) == 0);                      /* accepted at once */
}

static void load_gp(uint32_t v) {
    csr_w(CSR_GP, 4, v);
}

static void load_bases(uint32_t cu, uint32_t ru) {
    load_gp(cu);
    scb(0x60);
    load_gp(ru);
    scb(0x06);
}

static void test_interrupts(void) {
    power_on();
    map_card();
    CHECK(!env.irq && csr_r(CSR_STATUS, 2) == 0);

    /* Software interrupt: SI in the interrupt control byte latches STAT.SWI. */
    csr_w(CSR_ICTL, 1, 0x02);
    CHECK(csr_r(CSR_ACK, 1) == ST_SWI && env.irq && e100_irq(&nic));
    CHECK(csr_r(CSR_ICTL, 1) == 0);                     /* SI itself reads 0 */
    csr_w(CSR_ACK, 1, ST_SWI);                          /* acknowledge by writing 1 */
    CHECK(csr_r(CSR_ACK, 1) == 0 && !env.irq);

    /* The mask bit M gates the pin, not the status. */
    csr_w(CSR_ICTL, 1, 0x03);
    CHECK(csr_r(CSR_ICTL, 1) == 0x01);
    CHECK(csr_r(CSR_ACK, 1) == ST_SWI && !env.irq);
    csr_w(CSR_ICTL, 1, 0x00);
    CHECK(env.irq);
    csr_w(CSR_ACK, 1, 0xFF);
    CHECK(!env.irq);

    /* The SCB status word and the 32-bit write that updates acknowledge and
     * command together: STAT/ACK in byte 1, command in byte 2, mask in byte 3. */
    csr_w(CSR_ICTL, 1, 0x02);
    csr_w(0x00, 4, (uint32_t)ST_SWI << 8);
    CHECK(csr_r(CSR_ACK, 1) == 0 && !env.irq);

    /* Per-source masks (SDM Table 14): SWI has none, FR/CX/CNA/RNR/ER/FCP do. */
    csr_w(CSR_ICTL, 1, 0x02);
    csr_w(CSR_ACK, 1, 0xFF);
    load_bases(0x1000, 0x3000);
    m32(0x1000, 0x80000000u);                           /* NOP, EL */
    m32(0x1004, 0);
    csr_w(CSR_ICTL, 1, 0x20);                           /* mask CNA */
    load_gp(0);
    scb(0x10);
    CHECK(csr_r(CSR_ACK, 1) == ST_CNA && !env.irq);     /* latched but masked */
    csr_w(CSR_ICTL, 1, 0x00);
    CHECK(env.irq);
    csr_w(CSR_ACK, 1, ST_CNA);
    CHECK(!env.irq);

    /* PCI RST# releases INTA#. */
    csr_w(CSR_ICTL, 1, 0x02);
    CHECK(env.irq);
    e100_reset(&nic);
    CHECK(!env.irq && !e100_irq(&nic));
}

static const uint8_t DEST[6] = { 0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };

/* A 14-byte Ethernet header from `src` to `dst` with an EtherType, then
 * `len - 14` bytes of counting payload. */
static void make_frame(uint8_t *f, size_t len, const uint8_t *dst, const uint8_t *src) {
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    f[12] = 0x08;
    f[13] = 0x00;
    for (size_t i = 14; i < len; i++) {
        f[i] = (uint8_t)(i * 3);
    }
}

/* Bring the link up through the PHY timers and configure the MAC so that it
 * transmits and receives: configure byte 8 bit 0 selects the MII (SDM 6.4.2.3). */
static void link_up_and_configure(void) {
    advance_ms(980);
    CHECK(csr_r(CSR_GSTS, 1) & 1);
    load_bases(0x1000, 0x3000);
    /* CB 0: IA setup; CB 1: configure, ending the list. */
    m32(0x1000, 1u << 16);
    m32(0x1004, 0x0010);
    memcpy(env.mem + 0x1008, MAC, 6);
    static const uint8_t cfg[22] = {
        0x16, 0x08, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0x01, 0x00, 0x2E,
        0x00, 0x60, 0x00, 0xF2, 0xC8, 0x00, 0x40, 0xF2, 0x80, 0x3F, 0x05,
    };
    m32(0x1010, (2u << 16) | (1u << 31));
    m32(0x1014, 0);
    memcpy(env.mem + 0x1018, cfg, sizeof cfg);
    load_gp(0);
    scb(0x10);
    CHECK(g16(0x1000) == 0xA000 && g16(0x1010) == 0xA000);
    csr_w(CSR_ACK, 1, 0xFF);
}

static void test_command_unit(void) {
    power_on();
    map_card();
    advance_ms(980);
    load_bases(0x1000, 0x3000);

    /* IA setup, configure and a NOP that suspends the list and interrupts.
     * CU Start addresses the first CB relative to the CU base. */
    m32(0x1000, 1u << 16);
    m32(0x1004, 0x0010);
    memcpy(env.mem + 0x1008, DEST, 6);
    static const uint8_t cfg[22] = {
        0x16, 0x08, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0x01, 0x00, 0x2E,
        0x00, 0x60, 0x00, 0xF2, 0xC8, 0x00, 0x40, 0xF2, 0x80, 0x3F, 0x05,
    };
    m32(0x1010, 2u << 16);
    m32(0x1014, 0x0030);
    memcpy(env.mem + 0x1018, cfg, sizeof cfg);
    m32(0x1030, (1u << 30) | (1u << 29));               /* NOP, S, I */
    m32(0x1034, 0x0040);
    load_gp(0);
    CHECK((csr_r(CSR_STATUS, 1) >> 6) == 0);            /* idle */
    scb(0x10);                                          /* CU Start */
    CHECK(g16(0x1000) == 0xA000 && g16(0x1010) == 0xA000 && g16(0x1030) == 0xA000);
    CHECK((csr_r(CSR_STATUS, 1) >> 6) == 1);            /* suspended */
    CHECK(csr_r(CSR_ACK, 1) == (ST_CX | ST_CNA) && env.irq);
    csr_w(CSR_ACK, 1, ST_CX | ST_CNA);
    CHECK(!env.irq);

    /* CU Resume re-reads the S bit of the suspending CB (SDM 6.5.3.2): with S
     * still set it stays suspended. */
    scb(0x20);
    CHECK((csr_r(CSR_STATUS, 1) >> 6) == 1);
    m32(0x1030, 1u << 29);                              /* software clears S */

    /* Transmit: simplified TxCB with the frame after the TCB; EL ends the list. */
    uint8_t frame[64];
    make_frame(frame, sizeof frame, DEST, MAC);
    m32(0x1040, (4u << 16) | (1u << 31) | (1u << 29));
    m32(0x1044, 0);
    m32(0x1048, 0xFFFFFFFFu);                           /* no TBD array */
    m32(0x104C, sizeof frame);                          /* TCB byte count */
    memcpy(env.mem + 0x1050, frame, sizeof frame);
    scb(0x20);                                          /* CU Resume */
    CHECK(g16(0x1040) == 0xA000);
    CHECK((csr_r(CSR_STATUS, 1) >> 6) == 0);            /* idle after the EL CB */
    CHECK(env.ntx == 1 && env.txlen[0] == sizeof frame);
    CHECK(!memcmp(env.tx[0], frame, sizeof frame));     /* without FCS */
    CHECK(csr_r(CSR_ACK, 1) == (ST_CX | ST_CNA));
    csr_w(CSR_ACK, 1, 0xFF);

    /* A short frame is padded to the minimum with zeros (configure byte 18). */
    m32(0x1100, (4u << 16) | (1u << 31));
    m32(0x1104, 0);
    m32(0x1108, 0xFFFFFFFFu);
    m32(0x110C, 20);
    make_frame(frame, 20, DEST, MAC);
    memcpy(env.mem + 0x1110, frame, 20);
    load_gp(0x100);
    scb(0x10);
    CHECK(env.ntx == 2 && env.txlen[1] == 60);
    CHECK(!memcmp(env.tx[1], frame, 20));
    for (unsigned i = 20; i < 60; i++) {
        CHECK(env.tx[1][i] == 0);
    }
    CHECK(csr_r(CSR_ACK, 1) == ST_CNA);                 /* no I bit: only CNA */
    csr_w(CSR_ACK, 1, 0xFF);

    /* Flexible mode: a TBD array gathers the header and the payload. */
    make_frame(frame, sizeof frame, DEST, MAC);
    m32(0x1200, (4u << 16) | (1u << 31) | (1u << 19) | (1u << 29));   /* EL, SF, I */
    m32(0x1204, 0);
    m32(0x1208, 0x0300);                                /* TBD array, relative to the CU base */
    m32(0x120C, 2u << 24);                              /* two TBDs, no immediate data */
    m32(0x1300, 0x5000);
    m32(0x1304, 14);
    m32(0x1308, 0x5100);
    m32(0x130C, (1u << 16) | 50);                       /* EL on the last */
    memcpy(env.mem + 0x5000, frame, 14);
    memcpy(env.mem + 0x5100, frame + 14, 50);
    load_gp(0x200);
    scb(0x10);
    CHECK(env.ntx == 3 && env.txlen[2] == 64 && !memcmp(env.tx[2], frame, 64));
    CHECK(csr_r(CSR_ACK, 1) == (ST_CX | ST_CNA));
    csr_w(CSR_ACK, 1, 0xFF);
}

static void test_receive_unit(void) {
    power_on();
    map_card();
    link_up_and_configure();
    CHECK((csr_r(CSR_STATUS, 1) & 0x3C) == 0);          /* RU idle */

    /* Two RFDs: the first links to the second, which ends the list. */
    m32(0x3000, 0);
    m32(0x3004, 0x0800);
    m32(0x3008, 0xFFFFFFFFu);
    m32(0x300C, 1518u << 16);
    m32(0x3800, 1u << 31);                              /* EL */
    m32(0x3804, 0x0000);
    m32(0x3808, 0xFFFFFFFFu);
    m32(0x380C, 1518u << 16);
    load_gp(0);
    scb(0x01);                                          /* RU Start */
    CHECK((csr_r(CSR_STATUS, 1) & 0x3C) == 0x10);       /* Ready */

    uint8_t frame[2][64];
    make_frame(frame[0], 64, MAC, DEST);                /* to the station address */
    make_frame(frame[1], 64, MAC, DEST);
    frame[1][20] ^= 0x55;
    memcpy(env.rx[0], frame[0], 64);
    env.rxlen[0] = 64;
    memcpy(env.rx[1], frame[1], 64);
    env.rxlen[1] = 64;
    env.nrx = 2;

    /* Frames enter at wire rate: one microsecond pays for one frame, not two. */
    advance_us(1);
    CHECK(env.nrx == 1);
    CHECK(g16(0x3000) == 0xA020);                       /* C, OK, type/length field > 1500 */
    CHECK(g16(0x300C) == 0xC040);                       /* EOF, F, 64 bytes */
    CHECK(!memcmp(env.mem + 0x3010, frame[0], 64));
    CHECK((csr_r(CSR_ACK, 1) & ST_FR) && env.irq);
    CHECK((csr_r(CSR_STATUS, 1) & 0x3C) == 0x10);       /* the next RFD is waiting */
    csr_w(CSR_ACK, 1, ST_FR);
    CHECK(!env.irq);

    advance_us(20);
    CHECK(env.nrx == 0);
    CHECK(g16(0x3800) == 0xA020 && g16(0x380C) == 0xC040);
    CHECK(!memcmp(env.mem + 0x3810, frame[1], 64));
    CHECK((csr_r(CSR_STATUS, 1) & 0x3C) == 0x08);       /* the EL RFD was used: no resources */
    CHECK(csr_r(CSR_ACK, 1) == (ST_FR | ST_RNR));
    csr_w(CSR_ACK, 1, 0xFF);

    /* No resources: frames wait in the receive FIFO until the RU restarts. */
    make_frame(frame[0], 100, MAC, DEST);
    memcpy(env.rx[2], frame[0], 100);
    env.rxlen[2] = 100;
    env.rx_head = 2;
    env.nrx = 1;
    advance_us(100);
    CHECK(env.nrx == 0 && g16(0x3000) == 0xA020);       /* old status still there */
    m32(0x3000, 0);
    m32(0x300C, 1518u << 16);
    load_gp(0);
    scb(0x01);
    CHECK(g16(0x3000) == 0xA020 && g16(0x300C) == (0xC000 | 100));
    CHECK(!memcmp(env.mem + 0x3010, frame[0], 100));

    /* A frame to someone else is ignored; broadcast and promiscuous reception
     * follow the configuration. */
    m32(0x3000, 0);
    m32(0x3800, 1u << 31);
    memset(env.mem + 0x3010, 0, 64);
    uint8_t other[6] = { 0x02, 0x99, 0x99, 0x99, 0x99, 0x99 };
    make_frame(frame[0], 64, other, DEST);
    memcpy(env.rx[3], frame[0], 64);
    env.rxlen[3] = 64;
    env.rx_head = 3;
    env.nrx = 1;
    load_gp(0);
    scb(0x04);                                          /* RU Abort */
    scb(0x01);
    advance_us(100);
    CHECK(g16(0x3000) == 0);
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    make_frame(frame[0], 64, bcast, DEST);
    memcpy(env.rx[0], frame[0], 64);
    env.rxlen[0] = 64;
    env.rx_head = 0;
    env.nrx = 1;
    advance_us(100);
    CHECK((g16(0x3000) & 0xA000) == 0xA000);
    CHECK(!memcmp(env.mem + 0x3010, frame[0], 64));

    /* A short frame is flagged bad; with Save Bad Frames off its RFD is
     * reclaimed untouched. */
    make_frame(frame[0], 40, MAC, DEST);
    m32(0x3800, 1u << 31);
    m32(0x3804, 0);
    m32(0x380C, 1518u << 16);
    load_gp(0x800);
    scb(0x04);
    scb(0x01);
    memcpy(env.rx[1], frame[0], 40);
    env.rxlen[1] = 40;
    env.rx_head = 1;
    env.nrx = 1;
    advance_us(100);
    CHECK(g16(0x3800) == 0);                            /* "save bad frames" is off */

    /* RU Abort leaves the RU idle with RNR. */
    scb(0x04);
    CHECK((csr_r(CSR_STATUS, 1) & 0x3C) == 0);
}

static void test_port_and_reset(void) {
    power_on();
    map_card();
    advance_ms(980);
    CHECK(csr_r(CSR_GSTS, 1) & 1);
    load_bases(0x1000, 0x3000);
    csr_w(CSR_ICTL, 1, 0x02);
    CHECK(env.irq);

    /* PORT software reset: interrupt state, bases and mask clear; the PHY
     * keeps its link. */
    csr_w(CSR_PORT, 4, 0);
    CHECK(!env.irq && csr_r(CSR_ACK, 1) == 0 && csr_r(CSR_ICTL, 1) == 0);
    CHECK(csr_r(CSR_MDI, 4) == (1u << 28));
    CHECK(csr_r(CSR_GSTS, 1) & 1);

    /* PORT self-test writes its results to the address in the register. */
    m32(0x2000, 0);
    m32(0x2004, 0xFFFFFFFFu);
    csr_w(CSR_PORT, 4, 0x2000 | 1);
    CHECK(g32(0x2000) != 0 && g32(0x2004) == 0);
    CHECK(csr_r(CSR_PORT, 4) == 0);

    /* Bus mastering off: no DMA, and the work waits for it. */
    cfg_w(0x04, 2, 0x0003);
    load_bases(0x1000, 0x3000);
    m32(0x1000, 0xE0000000u);
    m32(0x1004, 0);
    load_gp(0);
    scb(0x10);
    CHECK(g16(0x1000) == 0);
    CHECK((csr_r(CSR_STATUS, 1) >> 6) == 2);            /* active, waiting */
    cfg_w(0x04, 2, 0x0007);
    advance_us(1);
    CHECK(g16(0x1000) == 0xA000 && (csr_r(CSR_STATUS, 1) >> 6) == 0);
    csr_w(CSR_ACK, 1, 0xFF);

    /* A DMA nobody answers is a master abort: Status bit 13. */
    load_bases(0xFFFF0000u, 0);
    load_gp(0);
    cfg_w(0x06, 2, 0x2000);
    CHECK((cfg_r(0x06, 2) & 0x2000) == 0);
    scb(0x10);
    CHECK(cfg_r(0x06, 2) & 0x2000);
    cfg_w(0x06, 2, 0x2000);
    CHECK((cfg_r(0x06, 2) & 0x2000) == 0);

    /* PCI RST#: the PHY restarts negotiation, so the link drops. */
    e100_reset(&nic);
    CHECK(cfg_r(0x04, 2) == 0 && cfg_r(0x10, 4) == 0);
    cfg_w(0x10, 4, CSR_BASE);
    cfg_w(0x04, 2, 0x0002);
    CHECK((csr_r(CSR_GSTS, 1) & 1) == 0);
    advance_ms(980);
    CHECK((csr_r(CSR_GSTS, 1) & 1) == 0);               /* held down 1.2 s, then negotiating */
    advance_ms(1200);
    CHECK(csr_r(CSR_GSTS, 1) & 1);
}

/* The machine snapshots the state as raw bytes and re-attaches the host
 * pointers afterwards. */
static void test_rebind(void) {
    power_on();
    map_card();
    static const uint8_t rom[4] = { 0x55, 0xAA, 0x12, 0x34 };
    e100_set_rom(&nic, rom, sizeof rom);
    advance_ms(500);
    csr_w(CSR_ICTL, 1, 0x02);
    CHECK(env.irq);

    static E100 copy;
    static Env env2;
    memcpy(&copy, &nic, sizeof copy);
    env2.ticks = env.ticks;
    E100Wiring w2 = wiring;
    w2.opaque = &env2;
    e100_rebind(&copy, &w2);
    e100_set_rom(&copy, rom, sizeof rom);
    CHECK(e100_irq(&copy) && e100_cfg_read(&copy, 0x10, 4) == CSR_BASE);

    /* Acknowledging through the copy releases INTA# on the new wiring only. */
    uint64_t v = ST_SWI;
    CHECK(e100_mem_access(&copy, true, CSR_BASE + CSR_ACK, 1, &v));
    CHECK(!e100_irq(&copy) && !env2.irq && env2.irq_edges == 1);
    CHECK(env.irq && env.irq_edges == 1);
    CHECK(e100_mem_access(&copy, false, FLASH_BASE, 4, &v) && v == 0x3412AA55u);

    /* The link timers carry on where the dump left them. */
    env2.ticks += 480 * 1000;
    CHECK(e100_mem_access(&copy, false, CSR_BASE + CSR_GSTS, 1, &v) && (v & 1));
}

/* ---- PCI ROM helpers --------------------------------------------------------------- */

/* Append a 512-byte-multiple image with a valid header, PCIR and checksum. */
static size_t add_image(uint8_t *rom, size_t at, unsigned blocks, unsigned code_type,
                        unsigned machine, bool last) {
    uint8_t *p = rom + at;
    memset(p, 0x90, blocks * 512u);
    p[0] = 0x55;
    p[1] = 0xAA;
    p[10] = (uint8_t)machine;
    p[11] = (uint8_t)(machine >> 8);
    p[0x18] = 0x40;                                     /* PCIR at +40h */
    p[0x19] = 0x00;
    memcpy(p + 0x40, "PCIR", 4);
    p[0x44] = 0x86;
    p[0x45] = 0x80;
    p[0x46] = 0x09;                                     /* built for 8086:1209 */
    p[0x47] = 0x12;
    p[0x50] = (uint8_t)blocks;
    p[0x51] = 0;
    p[0x54] = (uint8_t)code_type;
    p[0x55] = last ? 0x80 : 0;
    return at + blocks * 512u;
}

static unsigned byte_sum(const uint8_t *p, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) {
        s = (uint8_t)(s + p[i]);
    }
    return s;
}

static void test_pci_rom(void) {
    /* A legacy image, an x86-64 EFI image and an Itanium EFI image. */
    static uint8_t rom[512 * 8];
    memset(rom, 0, sizeof rom);
    size_t at = add_image(rom, 0, 2, 0, 0, false);
    at = add_image(rom, at, 1, 3, 0x8664, false);
    at = add_image(rom, at, 3, 3, 0x0200, true);
    CHECK(at == 512 * 6);
    uint8_t efi[512 * 3];
    memcpy(efi, rom + 512 * 3, sizeof efi);

    pci_rom_retag(rom, sizeof rom, 0x8086, 0x1229);
    for (size_t image = 0; image < at;) {
        size_t len = rom[image + 0x50] * 512u;
        CHECK(rom[image + 0x44] == 0x86 && rom[image + 0x45] == 0x80);
        CHECK(rom[image + 0x46] == 0x29 && rom[image + 0x47] == 0x12);
        CHECK(byte_sum(rom + image, len) == 0);         /* checksum repaired */
        image += len;
    }

    CHECK(pci_rom_keep_efi(rom, sizeof rom, 0x0300) == 0);   /* no such machine type */
    CHECK(rom[0] == 0x55 && rom[0x44] == 0x86);
    size_t kept = pci_rom_keep_efi(rom, sizeof rom, 0x0200);
    CHECK(kept == 512 * 3);
    CHECK(rom[0] == 0x55 && rom[10] == 0x00 && rom[11] == 0x02 && rom[0x54] == 3);
    CHECK(!memcmp(rom + 0x50, efi + 0x50, 4));
    CHECK(byte_sum(rom, kept) == 0);

    /* Not a ROM at all: nothing to retag or keep. */
    uint8_t junk[64];
    memset(junk, 0x11, sizeof junk);
    pci_rom_retag(junk, sizeof junk, 0x8086, 0x1229);
    CHECK(junk[0x10] == 0x11);
    CHECK(pci_rom_keep_efi(junk, sizeof junk, 0x0200) == 0);

    /* Load from a file. */
    char path[] = "e100_rom_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    FILE *f = fdopen(fd, "wb");
    CHECK(f && fwrite(efi, 1, sizeof efi, f) == sizeof efi);
    fclose(f);
    size_t len = 0;
    uint8_t *loaded = pci_rom_load(path, &len);
    CHECK(loaded && len == sizeof efi && !memcmp(loaded, efi, len));
    free(loaded);
    remove(path);
    CHECK(pci_rom_load(path, &len) == NULL);
}

int main(void) {
    test_config();
    test_bars();
    test_decode();
    test_rom();
    test_eeprom_read();
    test_eeprom_write();
    test_phy();
    test_interrupts();
    test_command_unit();
    test_receive_unit();
    test_port_and_reset();
    test_rebind();
    test_pci_rom();
    printf("e100: %u checks passed\n", checks);
    return 0;
}
