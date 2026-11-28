/* The rca family's standalone core devices (stage R1): cdp1802, cdp1861,
 * cdp1869, tone.  Run: ninja check-rca_devices */
#include "cdp1802_dev.h"
#include "cdp1861_dev.h"
#include "cdp1869_dev.h"
#include "audio/tone.h"
#include "gemu/dev/state.h"
#include "gemu/dev/stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static uint8_t mem[0x10000];
static Cdp1802 *cpu;
static Cdp1861 *vdc;
static GemuDevice *tone_dev;
static unsigned dma_bytes;
static uint16_t bp = 0xFFFF;

static void dma_out(uint8_t *b, void *ud) { (void)b; (void)ud; dma_bytes++; }
static void q_out(uint8_t q, void *ud) { (void)ud; gemu_tone_set_gate(tone_dev, q); }
static uint8_t io_in(uint8_t port, void *ud) {
    (void)ud;
    if (port == 1) cdp1861_set_display(vdc, true);
    return 0xFF;
}
static void cycle(void *ud) { (void)ud; cdp1802_step(cpu); }
static void sync(void *ud) { (void)ud; cdp1861_sync(vdc, cpu); }
static bool stop_before(void *ud) { (void)ud; return cpu->R[cpu->P] == bp; }

static float *cap;
static size_t cap_n;
static void capture(void *ud, const float *s, size_t n) {
    (void)ud;
    cap = realloc(cap, (cap_n + n) * sizeof *cap);
    memcpy(cap + cap_n, s, n * sizeof *cap);
    cap_n += n;
}

int main(void) {
    GemuMachine *m = gemu_machine_new("rca-devices");
    GemuDevice *c = gemu_device_add(m, &gemu_cdp1802_type, "cpu", NULL, NULL);
    GemuDevice *v = gemu_device_add(m, &gemu_cdp1861_type, "vdc", NULL, NULL);
    GemuDevice *vis = gemu_device_add(m, &gemu_cdp1869_type, "vis", NULL, NULL);
    tone_dev = gemu_device_add(m, &gemu_tone_type, "tone", NULL, "freq=440");
    CHECK(c && v && vis && tone_dev);
    CHECK(!gemu_device_add(m, &gemu_tone_type, "bad", NULL, "freq=0"));
    CHECK(gemu_machine_init(m));
    cpu = gemu_cdp1802(c);
    vdc = gemu_cdp1861(v);
    cpu->mem = mem;
    cpu->mem_mask = 0xFFFF;
    cpu->on_sync = sync;
    cpu->q_out = q_out;
    cpu->io_in = io_in;
    gemu_cdp1861_set_dma(v, dma_out, NULL);
    gemu_cdp1802_set_dma_target(c, dma_out, NULL);
    static const GemuCdp1802Hooks hooks = { .stop_before = stop_before, .cycle = cycle };
    CHECK(gemu_cdp1802_exec(c, 220000, &hooks));
    gemu_mixer_set_output(m, 44100, 1, capture, NULL);
    CHECK(gemu_machine_start(m));

    /* the classic setup: R1 the interrupt routine at $0042 (exit at $0040),
     * R2 the stack, R3 the main loop at $0080: IN 1 (display on), then
     * toggle Q every 256 passes */
    static const uint8_t prog[] = {
        0xF8, 0x00, 0xB1, 0xF8, 0x42, 0xA1,      /* R1 = $0042 */
        0xF8, 0x07, 0xB2, 0xF8, 0xFF, 0xA2,      /* R2 = $07FF */
        0xF8, 0x00, 0xB3, 0xF8, 0x80, 0xA3,      /* R3 = $0080 */
        0xD3,                                    /* SEP 3 */
    };
    memcpy(mem, prog, sizeof prog);
    static const uint8_t isr[] = {
        0x72, 0x70,                              /* $40: LDXA; RET */
        0x22, 0x78, 0x22, 0x52,                  /* $42: DEC 2; SAV; DEC 2; STR 2 */
        0xF8, 0x01, 0xB0, 0xF8, 0x00, 0xA0,      /* R0 = $0100 (DMA) */
        0x30, 0x40,                              /* BR $40 */
    };
    memcpy(mem + 0x40, isr, sizeof isr);
    static const uint8_t loop[] = {
        0xE2, 0x69,                              /* $80: SEX 2; INP 1 */
        0x15, 0x85, 0x3A, 0x82,                  /* $82: INC 5; GLO 5; BNZ $82 */
        0x31, 0x8B, 0x7B, 0x30, 0x82,            /* $86: BQ $8B; SEQ; BR $82 */
        0x7A, 0x30, 0x82,                        /* $8B: REQ; BR $82 */
    };
    memcpy(mem + 0x80, loop, sizeof loop);

    /* a frame's machine cycles of machine time run exactly that many */
    const uint64_t frame = CDP1861_MCYCLES_PER_FRAME;
    uint64_t c0 = cpu->cycle_count;
    GemuTime t0 = gemu_machine_time(m);
    for (int i = 0; i < 10; i++)
        gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(220000, frame));
    CHECK(cpu->cycle_count - c0 == 10 * frame);
    CHECK(gemu_machine_time(m) - t0 == 10 * gemu_clocks(220000, frame));
    CHECK(gemu_cdp1802_ended(c) == GEMU_CDP1802_BUDGET);
    CHECK(vdc->display_on && dma_bytes > 0 && cpu->P == 3);

    /* a breakpoint stops at the instruction's fetch */
    bp = 0x0086;
    gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(220000, 100000));
    CHECK(gemu_cdp1802_ended(c) == GEMU_CDP1802_STOPPED && cpu->R[cpu->P] == 0x0086 &&
          cpu->state == CDP1802_S_FETCH);
    bp = 0xFFFF;

    /* the tone: Q's changes reach the stream on their sample */
    gemu_mixer_run(m);
    size_t before = cap_n, on = 0;
    for (int i = 0; i < 30; i++)
        gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(220000, frame));
    gemu_mixer_run(m);
    for (size_t i = before; i < cap_n; i++) on += cap[i] != 0.0f;
    CHECK(cap_n - before > 20000 && on > (cap_n - before) / 8 && on < cap_n - before);

    /* state, saved mid-frame with DMA queued: load it back, the queue's
     * entries go to the target again; the same bytes when saved again */
    while (!cpu->dma_count)
        gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(220000, 1));
    uint8_t *a, *b;
    size_t na, nb;
    CHECK(gemu_machine_save_state(m, &a, &na));
    uint64_t cyc = cpu->cycle_count;
    unsigned queued = cpu->dma_count;
    gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(220000, 5000));
    for (unsigned i = 0; i < CDP1802_DMA_QUEUE_CAP; i++) cpu->dma_queue[i].cb = NULL;
    CHECK(gemu_machine_load_state(m, a, na));
    CHECK(cpu->cycle_count == cyc && cpu->dma_count == queued &&
          cpu->dma_queue[cpu->dma_head].cb == dma_out);
    CHECK(gemu_machine_save_state(m, &b, &nb));
    CHECK(na == nb && !memcmp(a, b, na));
    unsigned bytes = dma_bytes;
    gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(220000, frame));
    CHECK(dma_bytes > bytes);
    free(a);
    free(b);

    char buf[256];
    gemu_device_describe(c, buf, sizeof buf);
    CHECK(strstr(buf, "machine cycle"));
    gemu_device_describe(tone_dev, buf, sizeof buf);
    CHECK(strstr(buf, "440 Hz"));
    free(cap);
    gemu_machine_free(m);
    printf("rca_devices: %u checks passed\n", checks);
    return 0;
}
