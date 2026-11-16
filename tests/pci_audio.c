/* Deterministic PCI audio bus/DMA tests. No SDL or ROMs required. */
#include "audio/pci_audio.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t ram[65536];
    uint64_t now;
    bool irq;
    unsigned edges, count;
    float output[4096];
} Fixture;
static uint64_t clock_now(void *p) { return ((Fixture *)p)->now; }
static bool read_ram(void *p, uint64_t a, void *b, unsigned n) {
    Fixture *f = p;
    if (a > sizeof f->ram || n > sizeof f->ram - a) return false;
    memcpy(b, f->ram + a, n); return true;
}
static bool write_ram(void *p, uint64_t a, const void *b, unsigned n) {
    Fixture *f = p;
    if (a > sizeof f->ram || n > sizeof f->ram - a) return false;
    memcpy(f->ram + a, b, n); return true;
}
static void irq(void *p, bool level) {
    Fixture *f = p; f->edges += level && !f->irq; f->irq = level;
}
static void output(void *p, const float *b, size_t n) {
    Fixture *f = p;
    assert(f->count + n * 2 <= sizeof f->output / sizeof(float));
    memcpy(f->output + f->count, b, n * 2 * sizeof(float)); f->count += n * 2;
}
static void init(PciAudio *s, Fixture *f, PciAudioModel model) {
    memset(f, 0, sizeof(*f));
    PciAudioWiring w = {.opaque=f, .clock_hz=48000, .clock=clock_now,
        .read=read_ram, .write=write_ram, .irq=irq, .output=output};
    pci_audio_init(s, model, &w);
    pci_audio_cfg_write(s, 0x10, 4, model == PCI_AUDIO_CS4281 ? 0x10000000 : 0x1001);
    pci_audio_cfg_write(s, 0x14, 4, model == PCI_AUDIO_CS4281 ? 0x10010000 : 0x2001);
    pci_audio_cfg_write(s, 4, 2, 7);
}
static uint32_t cs_read(PciAudio *s, unsigned r, unsigned bytes) {
    uint64_t v = 0;
    assert(pci_audio_mem_access(s, false, 0x10000000 + r, bytes, &v)); return v;
}
static void cs_write(PciAudio *s, unsigned r, uint32_t v, unsigned bytes) {
    uint64_t value = v;
    assert(pci_audio_mem_access(s, true, 0x10000000 + r, bytes, &value));
}
static void cs_codec(PciAudio *s, unsigned r, uint16_t v) {
    cs_write(s, 0x46c, r, 4); cs_write(s, 0x470, v, 4); cs_write(s, 0x460, 0xe, 4);
    assert(!(cs_read(s, 0x460, 4) & 8));
}
static void step(PciAudio *s, Fixture *f, unsigned frames) { f->now += frames; pci_audio_poll(s); }
static void store32(Fixture *f, unsigned a, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) f->ram[a+i] = v >> (8*i);
}
static void cs_setup(PciAudio *s, unsigned count, uint32_t mode) {
    cs_write(s, 0x3ec, 1, 4); cs_write(s, 0x400, 0x30, 4);
    cs_write(s, 0x118, 0x100, 4); cs_write(s, 0x11c, count - 1, 4);
    cs_write(s, 0x150, (1u << 29) | mode | 8, 4);
    cs_write(s, 0x180, 0x81001f00, 4);
    cs_write(s, 0x154, 0x30000, 4);
    cs_write(s, 0x0c, 0x7ffbfeff, 4); cs_write(s, 8, 3, 4);
    cs_codec(s, 2, 0); cs_codec(s, 0x18, 0x0808);
}
static void test_cs4281(void) {
    PciAudio s; Fixture f;
    init(&s, &f, PCI_AUDIO_CS4281);
    assert(pci_audio_cfg_read(&s, 0, 4) == 0x60051013);
    pci_audio_cfg_write(&s, 0, 4, 0); assert(pci_audio_cfg_read(&s, 0, 4) == 0x60051013);
    pci_audio_cfg_write(&s, 0x10, 4, ~0u); assert(pci_audio_cfg_read(&s, 0x10, 4) == 0xfffff000);
    pci_audio_cfg_write(&s, 0x14, 4, ~0u); assert(pci_audio_cfg_read(&s, 0x14, 4) == 0xffff0000);
    pci_audio_cfg_write(&s, 0x10, 4, 0x10000000); pci_audio_cfg_write(&s, 0x14, 4, 0x10010000);
    assert(cs_read(&s, 0x300, 4) == 0x60051013);
    assert(pci_audio_cfg_read(&s, 0xf0, 4) == 1);
    pci_audio_cfg_write(&s, 0xec, 4, 1); assert(cs_read(&s, 0x3ec, 4) == 0); /* locked */
    pci_audio_cfg_write(&s, 0xe0, 2, 0x4281);
    pci_audio_cfg_write(&s, 0xec, 4, 1); assert(cs_read(&s, 0x3ec, 4) == 1);
    cs_write(&s, 0x3fc, 0x12348086, 4); assert(pci_audio_cfg_read(&s, 0x2c, 4) == 0x12348086);
    cs_write(&s, 0x3ec, 1, 4); cs_write(&s, 0x46c, 0x7c, 4); cs_write(&s, 0x460, 0x1e, 4);
    assert(cs_read(&s, 0x464, 4) == 3); assert(cs_read(&s, 0x47c, 4) == 0x4352);
    assert(cs_read(&s, 0x464, 4) == 1);
    for (unsigned i = 0; i < 8; i++) store32(&f, 0x100+i*4, 0xc0004000);
    cs_setup(&s, 8, 0x10);
    assert(cs_read(&s, 8, 4) == 1);
    assert(cs_read(&s, 0, 4) == 0x80000000 && cs_read(&s, 8, 4) == 1);
    assert(cs_read(&s, 0, 4) == 0x80000000); /* repeated idle probes keep IRQs armed */
    step(&s, &f, 4);
    assert(f.count == 8 && fabsf(f.output[0] - .5f) < 1e-6f && fabsf(f.output[1] + .5f) < 1e-6f);
    assert(cs_read(&s, 0x110, 4) == 0x110 && cs_read(&s, 0x114, 4) == 3);
    assert(f.irq && f.edges == 1);
    assert((cs_read(&s, 0, 4) & 0x80040100) == 0x80040100 && !f.irq);
    cs_read(&s, 0xf0, 1); /* lower-byte reads must not clear upper status */
    assert(cs_read(&s, 0xf2, 1) & 2);
    assert(!(cs_read(&s, 0xf2, 1) & 2)); cs_write(&s, 8, 3, 4);
    assert(cs_read(&s, 0, 4) == 0x80000000 && cs_read(&s, 8, 4) == 1);
    step(&s, &f, 4);
    assert(f.irq && (cs_read(&s, 0xf0, 4) & 0x10000));
    assert(cs_read(&s, 0x110, 4) == 0x100 && cs_read(&s, 0x114, 4) == 7);
    cs_write(&s, 0x154, 1, 1); unsigned before = f.count; step(&s, &f, 10); assert(f.count == before);
    cs_write(&s, 0x154, 0, 1); cs_codec(&s, 2, 0x8000); step(&s, &f, 2);
    assert(f.output[f.count-1] == 0 && f.output[f.count-2] == 0);
    pci_audio_cfg_write(&s, 4, 1, 2); before = cs_read(&s, 0x110, 4); step(&s, &f, 4);
    assert(cs_read(&s, 0x110, 4) == before);
    pci_audio_reset(&s); assert(!f.irq && pci_audio_cfg_read(&s, 4, 2) == 0);

    init(&s, &f, PCI_AUDIO_CS4281);
    f.ram[0x100] = 0; f.ram[0x101] = 128; f.ram[0x102] = 255;
    cs_setup(&s, 3, (1u<<16)|(1u<<17)|(1u<<19)); /* unsigned 8-bit mono */
    step(&s, &f, 3);
    assert(f.count == 6 && f.output[0] == -1 && f.output[1] == -1 && f.output[2] == 0);
    assert(f.output[4] == 127/128.0f && f.output[5] == 127/128.0f);
    assert(cs_read(&s, 0x114, 4) == UINT32_MAX && (cs_read(&s, 0x154, 4) & 1));

    init(&s, &f, PCI_AUDIO_CS4281);
    store32(&f, 0x100, 0xc0004000); cs_setup(&s, 8, 0x10);
    cs_write(&s, 0x744, 1, 4); step(&s, &f, 480); /* 441 source frames at 44.1k */
    assert(s.dma_bytes == 441*4 && s.frames == 480);

    init(&s, &f, PCI_AUDIO_CS4281);
    cs_setup(&s, 3, 0);
    cs_write(&s, 0x150, (1u<<29)|(1u<<16)|(1u<<17)|(1u<<19)|4, 4);
    step(&s, &f, 3); /* unsigned capture silence */
    assert(f.count == 0 && f.ram[0x100] == 128 && f.ram[0x102] == 128);
    printf("CS4281: PCI, codec handshake, stereo/mono PCM, rate conversion, DMA wrap, IRQ acknowledgement, mute and pause passed\n");
}
static void io_write(PciAudio *s, unsigned port, unsigned n, uint64_t v) { assert(pci_audio_io_access(s, true, port, n, &v)); }
static uint32_t io_read(PciAudio *s, unsigned port, unsigned n) { uint64_t v; assert(pci_audio_io_access(s, false, port, n, &v)); return v; }
static void test_ich(void) {
    PciAudio s; Fixture f;
    init(&s, &f, PCI_AUDIO_AC97);
    assert(pci_audio_cfg_read(&s, 0, 4) == 0x24158086);
    io_write(&s, 0x1002, 2, 0); io_write(&s, 0x1018, 2, 0x0808);
    assert(io_read(&s, 0x107c, 2) == 0x4352);
    assert(io_read(&s, 0x2034, 1) == 0 && io_read(&s, 0x2034, 1) == 1);
    io_read(&s, 0x107c, 2); assert(io_read(&s, 0x2034, 1) == 0);
    store32(&f, 0x100, 0x1000); store32(&f, 0x104, 0x80000008);
    store32(&f, 0x108, 0x1020); store32(&f, 0x10c, 0x80000008);
    for (unsigned i = 0; i < 16; i++) store32(&f, 0x1000+i*4, 0xc0004000);
    io_write(&s, 0x2010, 4, 0x100); io_write(&s, 0x2015, 1, 1); io_write(&s, 0x201b, 1, 0x1d);
    step(&s, &f, 4);
    assert(f.count == 8 && f.output[0] == .5f && f.output[1] == -.5f && f.irq);
    assert(io_read(&s, 0x2014, 1) == 1 && (io_read(&s, 0x2016, 2) & 8));
    io_write(&s, 0x2016, 2, 8); assert(!f.irq);
    step(&s, &f, 4); assert(f.irq && (io_read(&s, 0x2016, 2) & 7) == 7);
    assert(f.output[f.count - 1] == -.5f); /* final frame must not be lost */
    io_write(&s, 0x2016, 2, 0x1c); assert(!f.irq);
    unsigned before = f.count;
    uint64_t nonzero = s.nonzero_frames;
    step(&s, &f, 10); assert(f.count == before && s.nonzero_frames == nonzero);
    store32(&f, 0x110, 0x1040); store32(&f, 0x114, 0x80000008);
    io_write(&s, 0x2015, 1, 2); step(&s, &f, 4); assert(f.count == before + 8);
    io_write(&s, 0x201b, 1, 2); assert(io_read(&s, 0x2016, 2) == 1 && !f.irq);
    store32(&f, 0x100, 0xffff0000); store32(&f, 0x104, 8);
    io_write(&s, 0x2010, 4, 0x100); io_write(&s, 0x201b, 1, 9); step(&s, &f, 1);
    assert((io_read(&s, 0x2016, 2) & 17) == 17 && f.irq);
    printf("AC97: PCI, codec semaphore, BDL DMA, final buffer, LVI restart, reset and invalid-DMA IRQ passed\n");
}
int main(void) { test_cs4281(); test_ich(); return 0; }
