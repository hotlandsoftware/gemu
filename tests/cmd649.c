/* The CMD PCI-649's config space and legacy decode.  Run: ninja check-cmd649 */
#include "storage/cmd649.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

int main(void) {
    IdeChannel primary = {0}, secondary = {0};
    Cmd649 c;
    cmd649_init(&c, &primary, &secondary);

    /* Identity and class, read-only. */
    CHECK(cmd649_cfg_read(&c, 0, 4) == UINT32_C(0x06491095));
    CHECK(cmd649_cfg_read(&c, 8, 4) == UINT32_C(0x01018A02));
    cmd649_cfg_write(&c, 0, 4, 0);
    cmd649_cfg_write(&c, 8, 4, 0);
    CHECK(cmd649_cfg_read(&c, 0, 4) == UINT32_C(0x06491095));
    CHECK(cmd649_cfg_read(&c, 8, 4) == UINT32_C(0x01018A02));
    CHECK(cmd649_cfg_read(&c, 0x3C, 2) == 0x010F);
    CHECK(cmd649_cfg_read(&c, 0x51, 1) == 0x0C);

    /* BAR sizing: all ones reads back the size mask, I/O bit set. */
    const uint32_t sizes[5] = { 0xfffffff9, 0xfffffffd, 0xfffffff9, 0xfffffffd, 0xfffffff1 };
    for (unsigned bar = 0; bar < 5; bar++) {
        CHECK(cmd649_cfg_read(&c, 0x10 + bar * 4, 4) == 1);
        cmd649_cfg_write(&c, 0x10 + bar * 4, 4, UINT32_MAX);
        CHECK(cmd649_cfg_read(&c, 0x10 + bar * 4, 4) == sizes[bar]);
    }
    cmd649_cfg_write(&c, 0x10, 4, 0x3049);              /* low bits are fixed */
    CHECK(cmd649_cfg_read(&c, 0x10, 4) == 0x3049);
    CHECK(cmd649_cfg_read(&c, 0x10, 1) == 0x49 && cmd649_cfg_read(&c, 0x11, 1) == 0x30);
    cmd649_cfg_write(&c, 0x24, 4, UINT32_MAX);           /* BAR5: nothing there */
    CHECK(cmd649_cfg_read(&c, 0x24, 4) == 0);
    cmd649_cfg_write(&c, 0x04, 2, 0x0005);               /* command register */
    CHECK(cmd649_cfg_read(&c, 0x04, 2) == 0x0005);
    cmd649_reset(&c);
    CHECK(cmd649_cfg_read(&c, 0x10, 4) == 1 && cmd649_cfg_read(&c, 0x04, 2) == 0);

    /* Legacy decode: both channels' task files and control blocks. */
    unsigned ch, reg;
    CHECK(cmd649_decode(&c, 0x1F0, &ch, &reg) && ch == 0 && reg == 0);
    CHECK(cmd649_decode(&c, 0x1F7, &ch, &reg) && ch == 0 && reg == 7);
    CHECK(cmd649_decode(&c, 0x3F6, &ch, &reg) && ch == 0 && reg == 8);
    CHECK(cmd649_decode(&c, 0x176, &ch, &reg) && ch == 1 && reg == 6);
    CHECK(cmd649_decode(&c, 0x376, &ch, &reg) && ch == 1 && reg == 8);
    CHECK(!cmd649_decode(&c, 0x1F8, &ch, &reg) && !cmd649_decode(&c, 0x3F7, &ch, &reg));
    CHECK(!cmd649_decode(&c, 0x377, &ch, &reg) && !cmd649_decode(&c, 0x16F, &ch, &reg));

    /* Through to the channel: no devices, so it floats (device echoed). */
    uint64_t v = 0xEE;
    CHECK(cmd649_io_write(&c, 0x176, 1, 0xA0));
    CHECK(cmd649_io_read(&c, 0x176, 1, &v) && v == 0xA0);
    CHECK(cmd649_io_read(&c, 0x177, 1, &v) && v == 0);

    /* A channel wired as NULL isn't decoded: the machine keeps its ports. */
    cmd649_init(&c, NULL, &secondary);
    CHECK(!cmd649_decode(&c, 0x1F7, &ch, &reg) && !cmd649_io_read(&c, 0x3F6, 1, &v));
    CHECK(!cmd649_io_write(&c, 0x1F6, 1, 0xA0));
    CHECK(cmd649_decode(&c, 0x177, &ch, &reg) && ch == 1);

    printf("cmd649: %u checks passed\n", checks);
    return 0;
}
