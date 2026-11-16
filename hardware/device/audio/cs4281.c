/* Cirrus Logic CS4281 PCI PCM engine.
 * Register semantics: Cirrus DS308PRM1, chapters 6, 10, 11, 18 and 22.
 * DMA and codec transactions are independent of the host audio device.
 * Legacy DOS/FM synthesis and external MIDI are not implemented. */
#include "pci_audio.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define R(s, addr) ((s)->u.cs.reg[(addr) / 4])
#define DMA_ENABLE (1u << 29)
#define DMA_AUTO   (1u << 4)
#define DMA_MONO   (1u << 17)
#define DMA_SIZE8  (1u << 16)
#define DMA_SIZE20 (1u << 20)
#define DMA_UNSIGNED (1u << 19)
#define DMA_BIGEND (1u << 18)
#define DMA_CBC    (1u << 24)
#define DMA_DTC    (1u << 16)
#define DMA_DHTC   (1u << 17)

static uint32_t pending(PciAudio *s) {
    uint32_t bits = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (R(s, 0xf0 + i * 4) & R(s, 0x154 + i * 8) & (DMA_DTC | DMA_DHTC))
            bits |= 1u << (8 + i);
    }
    if (bits) bits |= 1u << 18;
    return bits;
}
static void update_irq(PciAudio *s) {
    uint32_t p = pending(s), mask = R(s, 0x0c);
    pci_audio_irq(s, s->u.cs.intena && !(mask & (1u << 18)) && (p & ~mask & 0xf00));
}
static bool running(PciAudio *s, unsigned c) {
    return (R(s, 0x150 + 8 * c) & DMA_ENABLE) &&
           !(R(s, 0x154 + 8 * c) & 1) && (R(s, 0x180 + 4 * c) & (1u << 31));
}
void cs4281_reset(PciAudio *s) {
    memset(&s->u.cs, 0, sizeof(s->u.cs));
    R(s, 0x0c) = 0x7fffffff;
    R(s, 0x3f0) = 1; /* AC link configuration */
    R(s, 0x3fc) = 0x42538086;
    R(s, 0x428) = R(s, 0x42c) = 3; /* AC'97 primary input/output */
    R(s, 0x494) = 0x80; /* MIDI receive FIFO empty */
    R(s, 0x480) = R(s, 0x488) = R(s, 0x48c) = 0xffff;
    for (unsigned c = 0; c < 4; c++) R(s, 0x154 + c * 8) = 1;
}

uint32_t cs4281_read(PciAudio *s, unsigned addr, uint32_t mask) {
    if (addr >= 0x300 && addr < 0x344) return pci_audio_cfg_read(s, addr - 0x300, 4);
    if (addr == 0) {
        uint32_t sources = pending(s);
        uint32_t v = sources | (s->u.cs.intena ? 0x80000000u : 0);
        /* DS308PRM1 10.1: a status probe with no source pending must not
         * disable interrupts. Drivers probe again after issuing EOI. */
        if (sources) s->u.cs.intena = false; /* HICR EOI re-arms the pin */
        update_irq(s);
        return v;
    }
    if (addr == 8) return s->u.cs.intena ? 1 : 0;
    if (addr >= 0xf0 && addr <= 0xfc) {
        unsigned c = (addr - 0xf0) / 4;
        uint32_t v = R(s, addr) | (running(s, c) ? 0x8000 : 0);
        R(s, addr) &= ~(mask & (DMA_DTC | DMA_DHTC));
        update_irq(s);
        return v;
    }
    if (addr == 0x400) {
        uint32_t v = R(s, addr);
        if (v & 0x10) v |= 1u << 24;
        if (v & 0x20) v |= 1u << 25;
        return v;
    }
    if (addr == 0x464) return (R(s, 0x3ec) & 1) ? (1 | (R(s, addr) & 2)) : 0;
    if (addr == 0x474) return (R(s, 0x3ec) & 1) ? 3 : 0;
    if (addr == 0x47c) { uint32_t v = R(s, addr); R(s, 0x464) &= ~2u; return v; }
    if (addr == 0x4e4 || addr == 0x4f4) return 0; /* no secondary codec */
    if (addr == 0x344) return s->cfg[0x44];
    return R(s, addr);
}
void cs4281_write(PciAudio *s, unsigned addr, uint32_t value, uint32_t mask) {
    uint32_t old = R(s, addr), v = (old & ~mask) | (value & mask);
    if (s->debug) fprintf(stderr, "cs4281: reg[%03x]=%08x mask=%08x\n", addr, v, mask);
    if (addr >= 0x300 && addr < 0x344) return; /* read-only config echo */
    if (addr == 0 || addr == 0x464 || addr == 0x474 || addr == 0x478 || addr == 0x47c) return;
    if (addr == 8) {
        if (value & mask & 2) s->u.cs.intena = (value & 1) != 0;
        update_irq(s);
        return;
    }
    R(s, addr) = v;
    if (addr == 0x3fc)
        for (unsigned i = 0; i < 4; i++) s->cfg[0x2c + i] = v >> (8 * i);
    if (addr == 0x0c || (addr >= 0xf0 && addr <= 0xfc)) update_irq(s);
    if (addr >= 0x110 && addr < 0x150 && (addr & 0xf) >= 8)
        R(s, addr - 8) = (R(s, addr - 8) & ~mask) | (v & mask); /* DBA/DBC load current */
    if (addr >= 0x150 && addr < 0x170) {
        unsigned c = (addr - 0x150) / 8;
        if (!(addr & 4)) {
            if (!(old & DMA_ENABLE) || !(v & DMA_ENABLE)) {
                R(s, 0xf0 + c * 4) = 0;
                s->u.cs.phase[c] = 0;
                s->u.cs.last[c][0] = s->u.cs.last[c][1] = 0;
            }
        }
        update_irq(s);
    }
    if (addr == 0x344) s->cfg[0x44] = v & 3;
    if (addr == 0x3ec && !(v & 1)) {
        pci_audio_codec_write(s, 0, 0);
        R(s, 0x464) = 0;
    }
    if (addr == 0x460 && (v & 8)) {
        unsigned reg = R(s, 0x46c) & 0x7f;
        if (!(v & 0x40)) {
            if (v & 0x10) {
                R(s, 0x478) = reg;
                R(s, 0x47c) = pci_audio_codec_read(s, reg);
                R(s, 0x464) |= 2;
            } else pci_audio_codec_write(s, reg, R(s, 0x470));
        }
        R(s, addr) &= ~8u; /* command completed */
    }
}

static unsigned rate(uint32_t r) {
    static const unsigned special[] = {48000, 44100, 22050, 16000, 11025, 8000};
    r &= 0xffff;
    return r < 6 ? special[r] : r < 32 ? 48000 : 1536000 / r;
}
static float pcm(const uint8_t *p, unsigned bytes, uint32_t mode) {
    uint32_t v = 0;
    for (unsigned i = 0; i < bytes; i++)
        v |= (uint32_t)p[(mode & DMA_BIGEND) ? bytes - 1 - i : i] << (8 * i);
    if (mode & DMA_UNSIGNED) v ^= 1u << (bytes * 8 - 1);
    if (bytes == 1) return (int8_t)v / 128.0f;
    if (bytes == 2) return (int16_t)v / 32768.0f;
    return (int32_t)v / 2147483648.0f;
}
static bool transfer(PciAudio *s, unsigned c, float out[2]) {
    uint32_t mode = R(s, 0x150 + c * 8), a = R(s, 0x110 + c * 16);
    unsigned bytes = mode & DMA_SIZE8 ? 1 : mode & DMA_SIZE20 ? 4 : 2;
    unsigned channels = mode & DMA_MONO ? 1 : 2;
    bool play = (mode & 0xc) == 8;
    uint8_t data[8] = {0};
    /* No host capture source is connected. Unsigned PCM silence is the
     * midpoint of its range, not zero. */
    if (!play && (mode & DMA_UNSIGNED))
        for (unsigned ch = 0; ch < channels; ch++)
            data[ch * bytes + ((mode & DMA_BIGEND) ? 0 : bytes - 1)] = 0x80;
    if (play ? !pci_audio_dma_read(s, a, data, bytes * channels)
             : !pci_audio_dma_write(s, a, data, bytes * channels)) {
        R(s, 0x154 + c * 8) |= 1;
        return false;
    }
    if (play) {
        out[0] = pcm(data, bytes, mode);
        out[1] = channels == 1 ? out[0] : pcm(data + bytes, bytes, mode);
        if (mode & (1u << 22)) { float tmp = out[0]; out[0] = out[1]; out[1] = tmp; }
    }
    unsigned step = bytes * channels;
    R(s, 0x110 + c * 16) = mode & 0x20 ? a - step : a + step;
    unsigned count = (mode & DMA_CBC) ? channels : 1;
    while (count--) {
        uint32_t remaining = R(s, 0x114 + c * 16)--;
        if (!remaining) {
            R(s, 0xf0 + c * 4) |= DMA_DTC;
            if (mode & DMA_AUTO) {
                R(s, 0x110 + c * 16) = R(s, 0x118 + c * 16);
                R(s, 0x114 + c * 16) = R(s, 0x11c + c * 16);
            } else {
                R(s, 0x154 + c * 8) |= 1;
                break;
            }
        } else if (R(s, 0x114 + c * 16) == R(s, 0x11c + c * 16) / 2)
            R(s, 0xf0 + c * 4) |= DMA_DHTC;
    }
    return play;
}
bool cs4281_frame(PciAudio *s, float out[2]) {
    bool active = false;
    for (unsigned c = 0; c < 4; c++) {
        if (!running(s, c)) continue;
        uint32_t mode = R(s, 0x150 + c * 8);
        bool play = (mode & 0xc) == 8;
        s->u.cs.phase[c] += rate(R(s, play ? 0x744 : 0x748));
        if (s->u.cs.phase[c] >= 48000) {
            s->u.cs.phase[c] -= 48000;
            transfer(s, c, s->u.cs.last[c]);
        }
        if (play) {
            active = true;
            out[0] += s->u.cs.last[c][0]; out[1] += s->u.cs.last[c][1];
        }
    }
    /* PCM digital volume, 0 = unity, 31 = -46.5 dB. */
    for (unsigned c = 0; c < 2; c++) {
        unsigned v = R(s, 0x760 + c * 4) & 31;
        if (v) out[c] *= powf(10.0f, -1.5f * v / 20.0f);
    }
    update_irq(s);
    return active;
}
