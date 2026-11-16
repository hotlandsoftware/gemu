/* Intel 82801AA-compatible native audio mixer / bus master registers.
 * Three 32-entry BDL engines: PCM input, stereo PCM output, microphone.
 * Capture supplies silence; no host microphone is opened. */
#include "pci_audio.h"
#include <string.h>

enum { DCH = 1, CELV = 2, LVBCI = 4, BCIS = 8, FIFOE = 16 };
enum { RUN = 1, RESET = 2, LVBIE = 4, FEIE = 8, IOCE = 16 };

static bool dma_irq(const IchAudioDma *d) {
    return ((d->status & LVBCI) && (d->command & LVBIE)) ||
           ((d->status & BCIS) && (d->command & IOCE)) ||
           ((d->status & FIFOE) && (d->command & FEIE));
}
static void update_irq(PciAudio *s) {
    pci_audio_irq(s, dma_irq(&s->u.ich.dma[0]) || dma_irq(&s->u.ich.dma[1]) || dma_irq(&s->u.ich.dma[2]));
}
void ich_audio_reset(PciAudio *s) {
    memset(&s->u.ich, 0, sizeof(s->u.ich));
    s->u.ich.control = 2;
    for (unsigned i = 0; i < 3; i++) s->u.ich.dma[i].status = DCH;
    update_irq(s);
}
uint8_t ich_audio_read(PciAudio *s, unsigned off) {
    if (off >= 0x2c) {
        if (off < 0x30) return s->u.ich.control >> (8 * (off - 0x2c));
        if (off < 0x34) {
            uint32_t status = s->u.ich.status | 0x100;
            static const unsigned bits[] = {5, 6, 7};
            for (unsigned i = 0; i < 3; i++) if (dma_irq(&s->u.ich.dma[i])) status |= 1u << bits[i];
            return status >> (8 * (off - 0x30));
        }
        if (off == 0x34) { uint8_t v = s->u.ich.semaphore; s->u.ich.semaphore = 1; return v; }
        return 0;
    }
    IchAudioDma *d = &s->u.ich.dma[off / 16];
    switch (off & 15) {
    case 0: case 1: case 2: case 3: return d->bdbar >> (8 * (off & 3));
    case 4: return d->civ;
    case 5: return d->lvi;
    case 6: return d->status;
    case 7: return d->status >> 8;
    case 8: return d->remaining;
    case 9: return d->remaining >> 8;
    case 10: return d->piv;
    case 11: return d->command;
    default: return 0;
    }
}
void ich_audio_write(PciAudio *s, unsigned off, uint8_t v) {
    if (off >= 0x2c) {
        if (off < 0x30) {
            unsigned shift = 8 * (off - 0x2c);
            s->u.ich.control = (s->u.ich.control & ~(255u << shift)) | ((uint32_t)v << shift);
            if (off == 0x2c) {
                if (!(v & 2)) {
                    pci_audio_codec_write(s, 0, 0);
                    for (unsigned i = 0; i < 3; i++) {
                        memset(&s->u.ich.dma[i], 0, sizeof(IchAudioDma));
                        s->u.ich.dma[i].status = DCH;
                    }
                }
                s->u.ich.control &= ~4u; /* warm reset completes */
            }
        } else if (off < 0x34) s->u.ich.status &= ~((uint32_t)v << (8 * (off - 0x30)));
        update_irq(s);
        return;
    }
    IchAudioDma *d = &s->u.ich.dma[off / 16];
    switch (off & 15) {
    case 0: case 1: case 2: case 3: {
        unsigned shift = 8 * (off & 3);
        d->bdbar = ((d->bdbar & ~(255u << shift)) | ((uint32_t)v << shift)) & ~7u;
        break;
    }
    case 5:
        d->lvi = v & 31;
        if ((d->command & RUN) && (d->status & DCH)) {
            d->civ = d->piv;
            d->valid = false;
            d->status &= ~(DCH | CELV);
        }
        break;
    case 6: d->status &= ~(v & (LVBCI | BCIS | FIFOE)); break;
    case 11:
        if (v & RESET) {
            uint8_t enables = d->command & (LVBIE | FEIE | IOCE);
            memset(d, 0, sizeof(*d)); d->status = DCH; d->command = enables;
        } else {
            bool start = (v & RUN) && !(d->command & RUN);
            d->command = v & 0x1d;
            if (!(v & RUN)) d->status |= DCH;
            else if (start) d->status &= ~DCH;
        }
        break;
    default: break;
    }
    update_irq(s);
}
static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void finished(IchAudioDma *d) {
    if (d->control & 0x80000000u) d->status |= BCIS;
    d->valid = false;
    if (d->civ == d->lvi) {
        d->status |= DCH | CELV | LVBCI;
    } else {
        d->civ = d->piv;
        d->status &= ~CELV;
    }
}
static void transfer(PciAudio *s, unsigned engine) {
    IchAudioDma *d = &s->u.ich.dma[engine];
    /* Empty descriptors cannot make a guest-controlled infinite loop. */
    for (unsigned skipped = 0; !d->valid && skipped < 32; skipped++) {
        uint8_t desc[8];
        if (!pci_audio_dma_read(s, d->bdbar + d->civ * 8, desc, 8)) goto fault;
        d->address = le32(desc) & ~3u; d->control = le32(desc + 4);
        d->remaining = d->control & 0xffff;
        d->piv = (d->civ + 1) & 31;
        d->valid = true;
        if (!d->remaining) {
            finished(d);
            if (d->status & DCH) return;
        }
    }
    if (!d->valid) goto fault;
    unsigned samples = engine == 2 ? 1 : 2;
    if (samples > d->remaining) samples = d->remaining;
    uint8_t data[4] = {0};
    if (engine == 1) {
        if (!pci_audio_dma_read(s, d->address, data, samples * 2)) goto fault;
        for (unsigned c = 0; c < 2; c++)
            d->last[c] = (int16_t)(data[2 * c] | data[2 * c + 1] << 8) / 32768.0f;
    } else if (!pci_audio_dma_write(s, d->address, data, samples * 2)) goto fault;
    d->address += samples * 2; d->remaining -= samples;
    if (!d->remaining) finished(d);
    return;
fault:
    d->status |= FIFOE | DCH;
    d->last[0] = d->last[1] = 0;
}
bool ich_audio_frame(PciAudio *s, float out[2]) {
    bool active = false;
    if (!(s->u.ich.control & 2)) return false;
    for (unsigned i = 0; i < 3; i++) {
        IchAudioDma *d = &s->u.ich.dma[i];
        if (!(d->command & RUN)) continue;
        if (!(d->status & DCH)) {
            unsigned r = i == 1 ? 0x2c : i == 0 ? 0x32 : 0x34;
            d->phase += s->codec[r / 2];
            if (d->phase >= 48000) { d->phase -= 48000; transfer(s, i); }
            if (i == 1) {
                active = true;
                out[0] = d->last[0]; out[1] = d->last[1];
            }
        }
    }
    update_irq(s);
    return active;
}
