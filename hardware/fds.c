#include "fds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Physical disk format helpers ────────────────────────────────────────── */

/* FDS CRC-CCITT: poly 0x8408 (bit-reversed), initial 0x8000, flush 2 zero bytes */
static uint16_t fds_crc_update(uint16_t crc, uint8_t byte) {
    for (int b = 0; b < 8; b++) {
        if ((crc ^ byte) & 1)
            crc = (crc >> 1) ^ 0x8408u;
        else
            crc >>= 1;
        byte >>= 1;
    }
    return crc;
}

static uint16_t fds_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0x8000u;
    for (size_t i = 0; i < len; i++) crc = fds_crc_update(crc, data[i]);
    crc = fds_crc_update(crc, 0);
    crc = fds_crc_update(crc, 0);
    return crc;
}

/* Convert one side from logical .fds block data to the physical byte stream
 * the BIOS expects: [initial gap] ([0x80 sync] [block] [CRC lo] [CRC hi] [gap])* */
/* mask_set: mark physical byte positions [pos, pos+len) as "forward to CPU" */
static void mask_set(uint8_t *mask, size_t pos, size_t len) {
    for (size_t i = pos; i < pos + len; i++)
        mask[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static void fds_expand_side(uint8_t *phys, uint8_t *mask, const uint8_t *raw, size_t raw_len) {
    /* phys/mask are FDS_SIDE_BYTES / FDS_MASK_BYTES of zeroed memory.
     * Gap/sync/CRC positions keep mask bit = 0 (not forwarded to CPU).
     * Block data positions get mask bit = 1 (forwarded via transfer flag). */
    size_t out = 3538; /* initial motor-spin-up gap */
    size_t in  = 0;
    uint16_t last_file_size = 0;
    bool first = true;

    while (in < raw_len && out < FDS_SIDE_BYTES) {
        uint8_t btype = raw[in];
        size_t  blen;
        switch (btype) {
        case 0x01: blen = 56; break;
        case 0x02: blen = 2;  break;
        case 0x03:
            blen = 16;
            if (in + 14 < raw_len)
                last_file_size = (uint16_t)raw[in+13] | ((uint16_t)raw[in+14] << 8);
            break;
        case 0x04: blen = 1u + last_file_size; break;
        default: return; /* 0x00 padding or unknown — done */
        }
        if (in + blen > raw_len) return;

        /* Inter-block gap (before every block except the first) */
        if (!first) out += 122;
        first = false;

        if (out + 1 + blen + 2 > FDS_SIDE_BYTES) return;

        /* Sync byte (mask bit stays 0 — hardware consumes, not forwarded to CPU) */
        phys[out++] = 0x80;

        /* Block data — mark as forwarded, copy bytes */
        mask_set(mask, out, blen);
        memcpy(phys + out, raw + in, blen);
        out += blen;

        /* CRC — forwarded to CPU via $4031; hardware CRC comparator runs in parallel */
        uint16_t crc = fds_crc16(raw + in, blen);
        mask_set(mask, out, 2);
        phys[out++] = (uint8_t)(crc & 0xFF);
        phys[out++] = (uint8_t)(crc >> 8);

        in += blen;
    }
}

/* ── BIOS / disk loading ─────────────────────────────────────────────────── */

bool fds_bios_load(FdsState *f, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "fds: cannot open BIOS '%s'\n", path); return false; }
    size_t n = fread(f->bios, 1, FDS_BIOS_SIZE, fp);
    fclose(fp);
    if (n != FDS_BIOS_SIZE) {
        fprintf(stderr, "fds: BIOS '%s' is %zu bytes, expected %u\n",
                path, n, FDS_BIOS_SIZE);
        return false;
    }
    fprintf(stderr, "fds: loaded BIOS '%s'\n", path);
    return true;
}

bool fds_disk_load(FdsState *f, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "fds: cannot open disk '%s'\n", path); return false; }

    /* Detect fwNES header: magic "FDS\x1a" */
    uint8_t hdr[16];
    size_t  hdr_n = fread(hdr, 1, 16, fp);
    uint8_t sides;

    if (hdr_n == 16 && hdr[0] == 'F' && hdr[1] == 'D' && hdr[2] == 'S' && hdr[3] == 0x1A) {
        sides = hdr[4] ? hdr[4] : 1;
    } else {
        /* Raw format — no header; infer from file size */
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        rewind(fp);
        if (sz <= 0 || (sz % (long)FDS_SIDE_BYTES) != 0) {
            fprintf(stderr, "fds: '%s' is not a valid .fds image (size %ld)\n", path, sz);
            fclose(fp); return false;
        }
        sides = (uint8_t)(sz / (long)FDS_SIDE_BYTES);
    }

    uint8_t *raw = malloc((size_t)sides * FDS_SIDE_BYTES);
    if (!raw) { fclose(fp); return false; }

    /* For raw format rewind is already done; for fwNES we're positioned after header */
    size_t nread = fread(raw, 1, (size_t)sides * FDS_SIDE_BYTES, fp);
    fclose(fp);
    if (nread < (size_t)sides * FDS_SIDE_BYTES) {
        fprintf(stderr, "fds: disk '%s' truncated (%zu/%u bytes)\n",
                path, nread, (unsigned)(sides * FDS_SIDE_BYTES));
        free(raw); return false;
    }

    /* Expand each side from logical block format to physical disk byte stream.
     * fwd_mask marks which physical positions are actual data bytes (forward to CPU).
     * Gap, sync, and CRC positions keep mask bit = 0 (filtered by hardware). */
    uint8_t *phys = calloc((size_t)sides, FDS_SIDE_BYTES);
    uint8_t *fmsk = calloc((size_t)sides, FDS_MASK_BYTES);
    if (!phys || !fmsk) { free(raw); free(phys); free(fmsk); return false; }
    for (uint8_t s = 0; s < sides; s++)
        fds_expand_side(phys + (size_t)s * FDS_SIDE_BYTES,
                        fmsk + (size_t)s * FDS_MASK_BYTES,
                        raw  + (size_t)s * FDS_SIDE_BYTES, FDS_SIDE_BYTES);

    free(f->disk);
    free(f->fwd_mask);
    free(f->raw_disk);
    f->disk          = phys;
    f->fwd_mask      = fmsk;
    f->raw_disk      = raw;   /* keep for HLE file parsing */
    f->disk_sides    = sides;
    f->cur_side      = 0;
    f->disk_pos      = 0;
    f->cyc_acc       = 0;
    f->transfer_flag = false;
    f->end_of_head   = false;
    f->disk_inserted = true;

    fprintf(stderr, "fds: loaded '%s' — %u side%s\n", path, sides, sides == 1 ? "" : "s");
    return true;
}

void fds_disk_eject(FdsState *f) {
    free(f->disk);
    free(f->fwd_mask);
    free(f->raw_disk);
    f->disk          = NULL;
    f->fwd_mask      = NULL;
    f->raw_disk      = NULL;
    f->disk_sides    = 0;
    f->disk_inserted = false;
    f->disk_pos      = 0;
    f->cyc_acc       = 0;
    f->transfer_flag = false;
    f->end_of_head   = false;
    fprintf(stderr, "fds: disk ejected\n");
}

/* ── Per-cycle tick ──────────────────────────────────────────────────────── */

bool fds_tick(FdsState *f) {
    /* ---- Timer IRQ ---- */
    if (f->timer_enabled && f->timer_ctr > 0) {
        if (--f->timer_ctr == 0) {
            f->timer_pending = true;
            if (f->timer_repeat)
                f->timer_ctr = f->timer_latch;
            else
                f->timer_enabled = false;
        }
    }

    /* ---- Disk transfer ----
     * Active when: disk inserted, motor on ($4025 bit 0), not in reset
     * ($4025 bit 1 = 0), read mode ($4025 bit 2), not past end of side. */
    bool motor_on  = (f->drive_ctrl & 0x01) != 0;
    bool not_reset = (f->drive_ctrl & 0x02) == 0;
    bool read_mode = (f->drive_ctrl & 0x04) != 0;

    if (f->disk_inserted && motor_on && not_reset && read_mode && !f->end_of_head) {
        if (++f->cyc_acc >= FDS_CYCLES_PER_BYTE) {
            f->cyc_acc = 0;
            if (f->disk_pos < FDS_SIDE_BYTES) {
                size_t abs = (size_t)f->cur_side * FDS_SIDE_BYTES + f->disk_pos;
                uint8_t byte = f->disk[abs];
                bool forward = f->fwd_mask && ((f->fwd_mask[f->disk_pos >> 3] >> (f->disk_pos & 7)) & 1);
                f->disk_pos++;
                if (forward) {
                    f->read_data     = byte;
                    f->transfer_flag = true;
                }
            } else {
                f->end_of_head = true;
            }
        }
    }

    return f->timer_pending
        || (f->transfer_flag && (f->drive_ctrl & 0x80) != 0);
}

/* ── FDS wavetable synthesizer ───────────────────────────────────────────── */

float fds_audio_tick(FdsState *f) {
    static const int8_t mod_delta[8] = {0, 1, 2, 4, 0, -4, -3, -1};
    static const float  master_vol[4] = {1.0f, 0.667f, 0.5f, 0.4f};

    /* ---- Master envelope clock ---- */
    if (!f->snd_wav_halt && !f->snd_env_halt) {
        uint32_t period = ((uint32_t)f->snd_env_spd + 1u) * 8u;
        if (++f->snd_env_div >= period) {
            f->snd_env_div = 0;

            /* Volume envelope */
            if (!f->snd_vol_dis) {
                if (++f->snd_vol_div >= f->snd_vol_spd) {
                    f->snd_vol_div = 0;
                    if (f->snd_vol_grow) { if (f->snd_vol < 32) f->snd_vol++; }
                    else                 { if (f->snd_vol > 0)  f->snd_vol--; }
                }
            }

            /* Mod gain envelope */
            if (!f->snd_gain_dis) {
                if (++f->snd_gain_div >= f->snd_gain_spd) {
                    f->snd_gain_div = 0;
                    if (f->snd_gain_grow) { if (f->snd_gain < 63) f->snd_gain++; }
                    else                  { if (f->snd_gain > 0)  f->snd_gain--; }
                }
            }
        }
    }

    /* ---- Modulation oscillator ---- */
    if (!f->snd_wav_halt && !f->snd_mod_dis && f->snd_mod_freq) {
        uint8_t old_pos = (uint8_t)((f->snd_mod_phase >> 16) & 63);
        f->snd_mod_phase = (f->snd_mod_phase + f->snd_mod_freq) & 0x3FFFFFu;
        uint8_t new_pos = (uint8_t)((f->snd_mod_phase >> 16) & 63);
        if (new_pos != old_pos) {
            uint8_t step = f->snd_mod[new_pos] & 7;
            if (step == 4) {
                f->snd_mod_cnt = 0;
            } else {
                int c = (int)f->snd_mod_cnt + (int)mod_delta[step];
                if (c >  63) c =  63;
                if (c < -64) c = -64;
                f->snd_mod_cnt = (int8_t)c;
            }
        }
    }

    /* No output in wave write mode or when oscillator halted */
    if (f->snd_wav_write || f->snd_wav_halt)
        return 0.0f;

    /* ---- Compute modulated frequency ---- */
    uint16_t freq = f->snd_freq;
    if (!f->snd_mod_dis && f->snd_mod_cnt) {
        int32_t temp = (int32_t)f->snd_mod_cnt * (int32_t)freq;
        if (temp > 0 && (temp & 0xF)) temp += 0x10;
        temp >>= 4;
        temp = temp * (int32_t)f->snd_gain >> 6;
        int32_t result = (int32_t)freq + temp;
        if (result < 0)      result = 0;
        if (result > 0xFFF)  result = 0xFFF;
        freq = (uint16_t)result;
    }

    /* ---- Main oscillator (6.16 fixed-point: integer=waveform index) ---- */
    f->snd_phase = (f->snd_phase + freq) & 0x3FFFFFu;
    uint8_t sample = f->snd_wav[(f->snd_phase >> 16) & 63] & 63;

    /* ---- Volume and master attenuation ---- */
    uint8_t vol = f->snd_vol < 32 ? f->snd_vol : 32;
    float out = (float)((unsigned)sample * vol) / (63.0f * 32.0f);
    out *= master_vol[f->snd_master_vol & 3];
    return out * 0.5f;   /* scaled to ~APU output level */
}

/* ── Register I/O ────────────────────────────────────────────────────────── */

uint8_t fds_reg_read(FdsState *f, uint16_t addr) {
    /* Waveform table readback */
    if (addr >= 0x4040 && addr <= 0x407F)
        return f->snd_wav[addr - 0x4040u];

    switch (addr) {
    case 0x4030: {
        /* Bit 7: byte transfer flag; bit 6: end-of-head; bit 3: mirroring; bit 0: timer IRQ.
         * Reading here acknowledges both timer and transfer IRQs. */
        uint8_t v = 0;
        if (f->timer_pending)              v |= 0x01;
        if (f->drive_ctrl & 0x08)          v |= 0x08;  /* mirror state read-back */
        if (f->end_of_head)                v |= 0x40;
        if (f->transfer_flag)              v |= 0x80;
        f->timer_pending  = false;
        f->transfer_flag  = false;
        return v;
    }
    case 0x4031:
        f->transfer_flag = false;
        return f->read_data;
    case 0x4032: {
        /* Bit 0: no disk (1=no disk); bit 1: motor not ready (1=not ready); bit 2: write protect */
        uint8_t v = 0;
        if (!f->disk_inserted)          v |= 0x01;
        if ((f->drive_ctrl & 0x01) == 0) v |= 0x02;  /* motor off → not ready */
        return v;
    }
    case 0x4033:
        return 0x80;   /* bit 7: battery OK */
    default:
        return 0;
    }
}

void fds_reg_write(FdsState *f, uint16_t addr, uint8_t val) {
    /* Waveform table: writable only in wave write mode ($4089 bit7) */
    if (addr >= 0x4040 && addr <= 0x407F) {
        if (f->snd_wav_write)
            f->snd_wav[addr - 0x4040u] = val & 0x3Fu;
        return;
    }

    switch (addr) {
    /* ---- FDS sound registers ---- */
    case 0x4080:
        f->snd_vol_dis  = (val & 0x80) != 0;
        f->snd_vol_grow = (val & 0x40) != 0;
        f->snd_vol_spd  = val & 0x3Fu;
        if (f->snd_vol_dis) f->snd_vol = f->snd_vol_spd;
        break;
    case 0x4082:
        f->snd_freq = (f->snd_freq & 0x0F00u) | val;
        break;
    case 0x4083:
        f->snd_freq     = (f->snd_freq & 0x00FFu) | (((uint16_t)val & 0x0Fu) << 8);
        f->snd_wav_halt = (val & 0x80) != 0;
        f->snd_env_halt = (val & 0x40) != 0;
        if (f->snd_wav_halt) f->snd_env_div = 0;
        break;
    case 0x4084:
        f->snd_gain_dis  = (val & 0x80) != 0;
        f->snd_gain_grow = (val & 0x40) != 0;
        f->snd_gain_spd  = val & 0x3Fu;
        if (f->snd_gain_dis) f->snd_gain = f->snd_gain_spd;
        break;
    case 0x4085: {
        uint8_t v = val & 0x7Fu;
        f->snd_mod_cnt = (v & 0x40u) ? (int8_t)(v | 0x80u) : (int8_t)v;
        break;
    }
    case 0x4086:
        f->snd_mod_freq = (f->snd_mod_freq & 0x0F00u) | val;
        break;
    case 0x4087:
        f->snd_mod_freq = (f->snd_mod_freq & 0x00FFu) | (((uint16_t)val & 0x0Fu) << 8);
        f->snd_mod_dis  = (val & 0x80) != 0;
        if (f->snd_mod_dis) f->snd_mod_phase = 0;
        break;
    case 0x4088:
        if (f->snd_mod_dis) {
            uint8_t pos = (uint8_t)((f->snd_mod_phase >> 16) & 63);
            f->snd_mod[pos] = val & 7u;
            f->snd_mod_phase = (f->snd_mod_phase + 0x10000u) & 0x3FFFFFu;
        }
        break;
    case 0x4089:
        f->snd_wav_write  = (val & 0x80) != 0;
        f->snd_master_vol = val & 0x03u;
        break;
    case 0x408A:
        f->snd_env_spd = val;
        f->snd_env_div = 0;
        break;
    /* ---- Disk / timer registers ---- */
    case 0x4020:
        f->timer_latch = (f->timer_latch & 0xFF00u) | val;
        break;
    case 0x4021:
        f->timer_latch = (f->timer_latch & 0x00FFu) | ((uint16_t)val << 8);
        break;
    case 0x4022:
        f->timer_repeat  = (val & 0x01) != 0;
        f->timer_enabled = (val & 0x02) != 0;
        if (f->timer_enabled)
            f->timer_ctr = f->timer_latch;
        else
            f->timer_pending = false;
        break;
    case 0x4023:
        f->disk_io_en = (val & 0x01) != 0;
        break;
    case 0x4024:
        /* Write data register — disk write not yet implemented */
        f->transfer_flag = false;
        break;
    case 0x4025:
        if (val & 0x02) {
            f->disk_pos      = 0;
            f->cyc_acc       = 0;
            f->transfer_flag = false;
            f->end_of_head   = false;
        }
        f->drive_ctrl = val;
        break;
    case 0x4026:
        /* External connector output — ignored */
        break;
    }
}
