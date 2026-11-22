/* Shared PCI/AC'97 plumbing. Controllers run from the machine clock; SDL
 * never reads guest RAM or mutates DMA/interrupt state. */
#include "pci_audio.h"
#include "bus/pci.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t get32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = v >> (8 * i);
}
const char *pci_audio_name(PciAudioModel model) {
    return model == PCI_AUDIO_CS4281 ? "cs4281" : model == PCI_AUDIO_AC97 ? "ac97" : "none";
}

static void codec_gain(PciAudio *s) {
    unsigned master = s->codec[1], pcm = s->codec[0x18 / 2];
    for (unsigned c = 0; c < 2; c++) {
        unsigned shift = c ? 0 : 8;
        unsigned attenuation = ((master >> shift) & 31) + ((pcm >> shift) & 31);
        s->gain[c] = (master | pcm) & 0x8000 ? 0.0f :
                    powf(10.0f, (12.0f - 1.5f * attenuation) / 20.0f);
    }
}
static void codec_reset(PciAudio *s) {
    memset(s->codec, 0, sizeof(s->codec));
    s->codec[0] = 0x0800; /* 18-bit DAC, stereo mixer, no advertised 3D */
    s->codec[1] = s->codec[2] = s->codec[3] = 0x8000;
    for (unsigned reg = 0x0c; reg <= 0x18; reg += 2) s->codec[reg / 2] = 0x8808;
    s->codec[0x0e / 2] = 0x8008;
    s->codec[0x1c / 2] = s->codec[0x1e / 2] = 0x8000;
    s->codec[0x26 / 2] = 0x000f;
    s->codec[0x28 / 2] = s->model == PCI_AUDIO_AC97 ? 1 : 0; /* VRA */
    s->codec[0x2c / 2] = s->codec[0x32 / 2] = s->codec[0x34 / 2] = 48000;
    s->codec[0x7c / 2] = 0x4352; s->codec[0x7e / 2] = 0x5910; /* CS4297A */
    codec_gain(s);
}
uint16_t pci_audio_codec_read(PciAudio *s, unsigned reg) {
    return reg < 0x80 && !(reg & 1) ? s->codec[reg / 2] : 0xffff;
}
void pci_audio_codec_write(PciAudio *s, unsigned reg, uint16_t v) {
    if (reg >= 0x80 || (reg & 1)) return;
    if (reg == 0) { codec_reset(s); return; }
    if (reg == 0x28 || reg >= 0x7c) return;
    if (reg == 2 || reg == 4 || (reg >= 0x10 && reg <= 0x18)) v &= 0x9f1f;
    if (reg == 6) v &= 0x801f;
    if (reg == 0x26) v = (v & 0xff00) | 0x0f;
    if (reg == 0x2a) v &= s->codec[0x28 / 2];
    if (reg == 0x2c || reg == 0x32 || reg == 0x34) {
        if (!(s->codec[0x2a / 2] & 1)) v = 48000;
        else if (v < 8000) v = 8000;
        else if (v > 48000) v = 48000;
    }
    s->codec[reg / 2] = v;
    if (reg == 0x2a && !(v & 1))
        s->codec[0x2c / 2] = s->codec[0x32 / 2] = s->codec[0x34 / 2] = 48000;
    codec_gain(s);
    if (s->debug) fprintf(stderr, "%s: codec[%02x]=%04x\n", pci_audio_name(s->model), reg, v);
}

void pci_audio_irq(PciAudio *s, bool level) {
    if (s->cfg[5] & 4) level = false; /* PCI interrupt disable */
    if (level && !s->irq_level) s->interrupts++;
    bool changed = level != s->irq_level;
    s->irq_level = level;
    if (changed && s->wiring.irq) s->wiring.irq(s->wiring.opaque, level);
}
bool pci_audio_dma_read(PciAudio *s, uint32_t addr, void *buf, unsigned len) {
    if (!(s->cfg[4] & 4) || !s->wiring.read) return false;
    bool ok = s->wiring.read(s->wiring.opaque, addr, buf, len);
    if (ok) s->dma_bytes += len;
    return ok;
}
bool pci_audio_dma_write(PciAudio *s, uint32_t addr, const void *buf, unsigned len) {
    if (!(s->cfg[4] & 4) || !s->wiring.write) return false;
    bool ok = s->wiring.write(s->wiring.opaque, addr, buf, len);
    if (ok) s->dma_bytes += len;
    return ok;
}
void pci_audio_reset(PciAudio *s) {
    PciAudioModel model = s->model;
    PciAudioWiring wiring = s->wiring;
    bool debug = s->debug;
    pci_audio_irq(s, false);
    memset(s, 0, sizeof(*s));
    s->model = model; s->wiring = wiring; s->debug = debug;
    if (!model) return;
    put32(s->cfg, model == PCI_AUDIO_CS4281 ? 0x60051013 : 0x24158086);
    s->cfg[8] = 1; s->cfg[10] = 1; s->cfg[11] = 4;
    s->cfg[0x3c] = 45; s->cfg[0x3d] = 1;
    s->cfg[6] = 0x10; s->cfg[0x34] = 0x40;
    s->cfg[0x40] = 1; s->cfg[0x42] = 2; /* PCI PM capability, D0/D3 */
    put32(s->cfg + 0x2c, model == PCI_AUDIO_CS4281 ? 0x42538086 : 0x00008086);
    if (model == PCI_AUDIO_AC97) s->cfg[0x10] = s->cfg[0x14] = 1;
    codec_reset(s);
    if (model == PCI_AUDIO_CS4281) cs4281_reset(s);
    else ich_audio_reset(s);
    s->mark = wiring.clock ? wiring.clock(wiring.opaque) : 0;
}
void pci_audio_init(PciAudio *s, PciAudioModel model, const PciAudioWiring *wiring) {
    memset(s, 0, sizeof(*s));
    s->model = model; s->wiring = *wiring;
    s->debug = getenv("AUDIO_DEBUG") != NULL;
    pci_audio_reset(s);
}

void pci_audio_poll(PciAudio *s) {
    if (!s->model || !s->wiring.clock || !s->wiring.clock_hz) return;
    uint64_t now = s->wiring.clock(s->wiring.opaque);
    if (now < s->mark) { s->mark = now; s->fraction = 0; }
    uint64_t elapsed = now - s->mark;
    if (elapsed < s->wiring.clock_hz / 48000) return;
    s->mark = now;
    uint64_t scaled = elapsed * 48000 + s->fraction;
    uint64_t frames = scaled / s->wiring.clock_hz;
    s->fraction = scaled % s->wiring.clock_hz;
    if (!(s->cfg[4] & 4) || (s->cfg[0x44] & 3)) return;
    float buf[512];
    size_t n = 0;
    bool audible = false;
    while (frames--) {
        float sample[2] = {0, 0};
        bool active = s->model == PCI_AUDIO_CS4281 ? cs4281_frame(s, sample) : ich_audio_frame(s, sample);
        audible |= active;
        if (active) s->frames++;
        for (unsigned c = 0; c < 2; c++) {
            float v = sample[c] * s->gain[c];
            if (s->codec[0x26 / 2] & 0x8200) v = 0; /* DAC/link power down */
            buf[n * 2 + c] = v > 1 ? 1 : v < -1 ? -1 : v;
        }
        if (buf[n * 2] != 0 || buf[n * 2 + 1] != 0) s->nonzero_frames++;
        if (++n == 256 || !frames) {
            if (audible && s->wiring.output) s->wiring.output(s->wiring.opaque, buf, n);
            n = 0; audible = false;
        }
    }
}

uint32_t pci_audio_cfg_read(PciAudio *s, unsigned reg, unsigned size) {
    if (!s->model || size > 4 || !size || reg + size > 256) return UINT32_MAX;
    uint32_t value = 0;
    for (unsigned i = 0; i < size; i++) {
        uint8_t byte = s->cfg[reg + i];
        if (s->model == PCI_AUDIO_CS4281 && reg + i >= 0xe0) {
            unsigned r = reg + i + 0x300;
            byte = s->u.cs.reg[r / 4] >> (8 * (r & 3));
        }
        if (reg + i == 6) byte = (byte & ~8u) | (s->irq_level ? 8 : 0);
        value |= (uint32_t)byte << (8 * i);
    }
    return value;
}
void pci_audio_cfg_write(PciAudio *s, unsigned reg, unsigned size, uint32_t value) {
    if (!s->model || size > 4 || !size || reg + size > 256) return;
    pci_audio_poll(s);
    for (unsigned i = 0; i < size; i++) {
        unsigned r = reg + i;
        uint8_t v = value >> (8 * i);
        if (s->model == PCI_AUDIO_CS4281 && r >= 0xe0) {
            if (r < 0xe4 || (s->u.cs.reg[0x3e0 / 4] & 0xffff) == 0x4281) {
                unsigned shift = 8 * (r & 3);
                cs4281_write(s, 0x300 + (r & ~3u), (uint32_t)v << shift, 255u << shift);
            }
        } else if (r == 4) s->cfg[r] = v & 7;
        else if (r == 5) s->cfg[r] = v & 4;
        else if (r == 0x3c || r == 0x0c || r == 0x0d) s->cfg[r] = v;
        else if (r == 0x44) s->cfg[r] = v & 3;
        else if (r >= 0x10 && r < 0x18) s->cfg[r] = v;
    }
    uint32_t masks[2] = {s->model == PCI_AUDIO_CS4281 ? 0xfffff000 : 0xffffff00,
                         s->model == PCI_AUDIO_CS4281 ? 0xffff0000 : 0xffffffc0};
    for (unsigned b = 0; b < 2; b++) {
        uint8_t *p = s->cfg + 0x10 + b * 4;
        put32(p, (get32(p) & masks[b]) | (s->model == PCI_AUDIO_AC97));
    }
    if (s->cfg[5] & 4) pci_audio_irq(s, false);
}
bool pci_audio_mem_claims(const PciAudio *s, uint64_t addr) {
    if (s->model != PCI_AUDIO_CS4281 || !(s->cfg[4] & 2) || (s->cfg[0x44] & 3)) return false;
    return addr - get32(s->cfg + 0x10) < 4096 || addr - get32(s->cfg + 0x14) < 65536;
}
bool pci_audio_mem_access(PciAudio *s, bool write, uint64_t addr, unsigned size, uint64_t *value) {
    if (!pci_audio_mem_claims(s, addr) || !size || size > 8) return false;
    uint64_t off = addr - get32(s->cfg + 0x10);
    pci_audio_poll(s);
    if (off < 4096) {
        if ((off & 3) + size > 4) return false;
        unsigned shift = (off & 3) * 8;
        uint32_t mask = (uint32_t)(UINT64_MAX >> (64 - 8 * size)) << shift;
        if (write) cs4281_write(s, off & ~3u, (uint32_t)*value << shift, mask);
        else *value = (cs4281_read(s, off & ~3u, mask) & mask) >> shift;
    } else {
        off = addr - get32(s->cfg + 0x14);
        if (off + size > sizeof(s->u.cs.fifo)) return false;
        if (!write) *value = 0;
        for (unsigned i = 0; i < size; i++) {
            if (write) s->u.cs.fifo[off + i] = *value >> (8 * i);
            else *value |= (uint64_t)s->u.cs.fifo[off + i] << (8 * i);
        }
    }
    return true;
}
bool pci_audio_io_access(PciAudio *s, bool write, uint16_t port, unsigned size, uint64_t *value) {
    if (s->model != PCI_AUDIO_AC97 || !(s->cfg[4] & 1) || (s->cfg[0x44] & 3) || !size || size > 4) return false;
    uint32_t mixer = get32(s->cfg + 0x10) & ~3u, master = get32(s->cfg + 0x14) & ~3u;
    if (port >= mixer && port - mixer + size <= 256) {
        pci_audio_poll(s);
        unsigned r = port - mixer;
        if ((r & 1) || size != 2) { if (!write) *value = UINT32_MAX; return true; }
        if (write) pci_audio_codec_write(s, r, *value);
        else *value = pci_audio_codec_read(s, r);
        s->u.ich.semaphore = 0;
        return true;
    }
    if (port < master || port - master + size > 64) return false;
    pci_audio_poll(s);
    if (!write) *value = 0;
    for (unsigned i = 0; i < size; i++) {
        if (write) ich_audio_write(s, port - master + i, *value >> (8 * i));
        else *value |= (uint64_t)ich_audio_read(s, port - master + i) << (8 * i);
    }
    return true;
}

static bool pci_present_op(const void *d) {
    return ((const PciAudio *)d)->model != PCI_AUDIO_NONE;
}
static uint32_t pci_cfg_read_op(const void *d, unsigned reg, unsigned size) {
    return pci_audio_cfg_read((PciAudio *)d, reg, size);
}
static void pci_cfg_write_op(void *d, unsigned reg, unsigned size, uint32_t val) {
    pci_audio_cfg_write(d, reg, size, val);
}
static bool pci_mem_claims_op(const void *d, uint64_t addr) {
    return pci_audio_mem_claims(d, addr);
}
static bool pci_mem_access_op(void *d, bool write, uint64_t addr, unsigned size, uint64_t *val) {
    return pci_audio_mem_access(d, write, addr, size, val);
}
static bool pci_io_access_op(void *d, bool write, uint16_t port, unsigned size, uint64_t *val) {
    return pci_audio_io_access(d, write, port, size, val);
}
const PciFunctionOps pci_audio_pci_ops = {
    .name = "PCI audio", .present = pci_present_op,
    .cfg_read = pci_cfg_read_op, .cfg_write = pci_cfg_write_op,
    .mem_claims = pci_mem_claims_op, .mem_access = pci_mem_access_op,
    .io_access = pci_io_access_op,
};
