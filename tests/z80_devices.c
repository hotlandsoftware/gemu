/* The z80 family's standalone core devices (stage Z1): z80, tms9918,
 * ppi8255, adc0809, mm58167, msm5205.  Run: ninja check-z80_devices */
#include "z80_dev.h"
#include "tms9918_dev.h"
#include "msm5205_dev.h"
#include "timer/mm58167_dev.h"
#include "controller/adc0809_dev.h"
#include "controller/ppi8255_dev.h"
#include "gemu/dev/space.h"
#include "gemu/dev/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static uint8_t ram[0x10000];
static GemuSpace *io;
static uint8_t rd(void *ud, uint16_t a) { (void)ud; return ram[a]; }
static void wr(void *ud, uint16_t a, uint8_t v) { (void)ud; ram[a] = v; }
static uint8_t in(void *ud, uint16_t p) { (void)ud; return gemu_space_read8(io, p & 0xFF); }
static void out(void *ud, uint16_t p, uint8_t v) { (void)ud; gemu_space_write8(io, p & 0xFF, v); }

static Z80 *cpu;
static unsigned insns, stop_at;
static void insn(void *ud) { (void)ud; z80_step(cpu); insns++; }
static bool done(void *ud) { (void)ud; return stop_at && insns >= stop_at; }

static uint8_t pins(void *ud, int port) { (void)ud; return port == 0 ? 0x5A : 0xFF; }

int main(void) {
    GemuMachine *m = gemu_machine_new("z80-devices");
    GemuDevice *c = gemu_device_add(m, &gemu_z80_type, "cpu", NULL, NULL);
    GemuDevice *vdp = gemu_device_add(m, &gemu_tms9918_type, "vdp", NULL, NULL);
    GemuDevice *ppi = gemu_device_add(m, &gemu_ppi8255_type, "ppi", NULL, NULL);
    GemuDevice *adc = gemu_device_add(m, &gemu_adc0809_type, "adc", NULL, NULL);
    GemuDevice *rtc = gemu_device_add(m, &gemu_mm58167_type, "rtc", NULL, NULL);
    GemuDevice *msm = gemu_device_add(m, &gemu_msm5205_type, "msm", NULL, NULL);
    CHECK(c && vdp && ppi && adc && rtc && msm);
    CHECK(gemu_machine_init(m));
    cpu = gemu_z80(c);
    cpu->read = rd; cpu->write = wr; cpu->in = in; cpu->out = out;

    /* the I/O space: the 8255 at 00h, the VDP at 80h */
    CHECK((io = gemu_space_new(m, "io", 8, 8, GEMU_LITTLE_ENDIAN)));
    CHECK(gemu_space_map(io, 0x00, 4, &gemu_ppi8255_bus, ppi, 0, 0));
    CHECK(gemu_space_map(io, 0x80, 2, &gemu_tms9918_io, vdp, 0, 0));
    static const GemuZ80Hooks hooks = { .insn = insn, .done = done };
    CHECK(gemu_z80_exec(c, 4000000, &hooks));
    CHECK(gemu_machine_start(m));

    /* a program: LD HL,0100h; IM 1; EI; loop: INC (HL) (11 T); JR loop (12 T)
     * at 0038h: LD A,(0101h); INC A; LD (0101h),A; EI; RET */
    static const uint8_t prog[] = { 0x21, 0x00, 0x01, 0xED, 0x56, 0xFB, 0x34, 0x18, 0xFD };
    static const uint8_t isr[] = { 0x3A, 0x01, 0x01, 0x3C, 0x32, 0x01, 0x01, 0xFB, 0xC9 };
    memcpy(ram, prog, sizeof prog);
    memcpy(ram + 0x38, isr, sizeof isr);
    z80_reset(cpu);
    for (int i = 0; i < 3; i++) insn(NULL);
    uint64_t t0c = cpu->cycles;

    /* the executor: 23,000 T-states run 1,000 loops */
    GemuTime t0 = gemu_machine_time(m);
    gemu_machine_run_until(m, t0 + gemu_clocks(4000000, 23000));
    CHECK(gemu_z80_ended(c) == GEMU_Z80_BUDGET);
    CHECK(cpu->cycles - t0c >= 23000 && cpu->cycles - t0c < 23012);
    CHECK(ram[0x100] == (uint8_t)1000);
    /* done stops the run, and the machine, after that instruction */
    stop_at = insns + 5;
    GemuTime t1 = gemu_machine_time(m);
    gemu_machine_run_until(m, t1 + gemu_clocks(4000000, 100000));
    CHECK(gemu_z80_ended(c) == GEMU_Z80_DONE && insns == stop_at);
    CHECK(gemu_machine_time(m) < t1 + gemu_clocks(4000000, 100));
    stop_at = 0;
    /* /INT at IM 1: the routine runs (RST 38h) */
    cpu->int_line = true;
    gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(4000000, 30));
    cpu->int_line = false;
    gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(4000000, 200));
    CHECK(ram[0x101] >= 1 && cpu->iff1);

    /* the VDP: VRAM written and read back through its ports; register 1;
     * a frame is 262 scanlines and sets the frame flag */
    Tms9918 *v = gemu_tms9918(vdp);
    gemu_space_write8(io, 0x81, 0x00); gemu_space_write8(io, 0x81, 0x40 | 0x10);   /* write 1000h */
    for (int i = 0; i < 4; i++) gemu_space_write8(io, 0x80, (uint8_t)(0xA0 + i));
    CHECK(v->vram[0x1000] == 0xA0 && v->vram[0x1003] == 0xA3);
    gemu_space_write8(io, 0x81, 0x01); gemu_space_write8(io, 0x81, 0x10);           /* read 1001h */
    CHECK(gemu_space_read8(io, 0x80) == 0xA1 && gemu_space_read8(io, 0x80) == 0xA2);
    gemu_space_write8(io, 0x81, 0x60); gemu_space_write8(io, 0x81, 0x81);           /* R1 = 60h */
    CHECK(v->reg[1] == 0x60);
    int frames = 0;
    for (int i = 0; i < TMS9918_LINES; i++) frames += tms9918_scanline(v);
    CHECK(frames == 1 && (tms9918_peek_status(v) & TMS9918_STATUS_F) && v->int_out);
    uint8_t st;
    CHECK(gemu_space_peek8(io, 0x81, &st) && (st & TMS9918_STATUS_F));
    CHECK((gemu_space_read8(io, 0x81) & TMS9918_STATUS_F) && !(tms9918_peek_status(v) & TMS9918_STATUS_F));

    /* the 8255: inputs from the pins at reset (mode 0, all in); all
     * outputs: B reads its latch */
    Ppi8255 *p = gemu_ppi8255(ppi);
    p->read_pins = pins;
    CHECK(gemu_space_read8(io, 0x00) == 0x5A);
    gemu_space_write8(io, 0x03, 0x80);
    gemu_space_write8(io, 0x01, 0x33);
    CHECK(gemu_space_read8(io, 0x01) == 0x33 && p->out[1] == 0x33);

    /* the ADC: a conversion takes 72 of its clocks (640 kHz) = 450 bus
     * cycles at 4 MHz */
    Adc0809 *a = gemu_adc0809(adc);
    a->in[3] = 100.4;
    adc0809_start(a, 3, 1000, 640000.0, 4000000.0);
    CHECK(!adc0809_eoc(a, 1100) && adc0809_eoc(a, 1460) && adc0809_read(a, 1460) == 100);

    /* the clock: a second of bus cycles moves the seconds on */
    Mm58167 *r = gemu_mm58167(rtc);
    mm58167_set_time(r, 6, 15, 3, 12, 34, 56);
    mm58167_sync(r, 0, 4000000.0);
    CHECK(mm58167_read(r, 2) == 0x56);
    mm58167_sync(r, 4000000, 4000000.0);
    CHECK(mm58167_read(r, 2) == 0x57 && mm58167_read(r, 3) == 0x34);

    /* the MSM5205: out of reset, codes move the signal up */
    Msm5205 *s = gemu_msm5205(msm);
    msm5205_set_reset(s, false);
    for (int i = 0; i < 8; i++) msm5205_vck(s, 0x07);
    CHECK(s->signal > 0 && msm5205_output(s) > 0);

    /* state: save, change everything, load: back; save again: the same bytes */
    uint8_t *x, *y;
    size_t nx, ny;
    CHECK(gemu_machine_save_state(m, &x, &nx));
    uint16_t pc = cpu->pc;
    uint64_t cyc = cpu->cycles;
    int16_t sig = s->signal;
    gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(4000000, 500));
    v->vram[0x1000] = 0; v->reg[1] = 0; p->out[1] = 0; a->result = 0; r->cnt[2] = 0;
    msm5205_vck(s, 0x0F);
    CHECK(gemu_machine_load_state(m, x, nx));
    CHECK(cpu->pc == pc && cpu->cycles == cyc && v->vram[0x1000] == 0xA0 && v->reg[1] == 0x60 &&
          p->out[1] == 0x33 && a->result == 100 && r->cnt[2] == 0x57 && s->signal == sig);
    CHECK(gemu_machine_save_state(m, &y, &ny));
    CHECK(nx == ny && !memcmp(x, y, nx));
    free(x);
    free(y);

    char buf[256];
    gemu_device_describe(rtc, buf, sizeof buf);
    CHECK(strstr(buf, "06/15 12:34:57"));
    gemu_device_describe(c, buf, sizeof buf);
    CHECK(strstr(buf, "PC") || strstr(buf, "pc"));
    gemu_machine_free(m);
    printf("z80_devices: %u checks passed\n", checks);
    return 0;
}
