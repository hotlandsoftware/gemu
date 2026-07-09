#include "fds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FDS_DISK_CHANGE_CYCLES 500000u
#define FDS_SAVE_MAGIC "GEMUFD3S"   /* v3: raw disk data only, no physical expansion */

/* ── Block state machine helpers ─────────────────────────────────────────── */

/* DSK_INIT=0  DSK_VOLUME=1  DSK_FILECNT=2  DSK_FILEHDR=3  DSK_FILEDATA=4 */
static uint32_t blk_len_for(const FdsState *f, uint8_t type) {
    switch (type) {
    case 1: return 56;
    case 2: return 2;
    case 3: return 16;
    case 4: return 1u + f->blk_filesize;
    default: return 0;
    }
}

static void blk_reset(FdsState *f) {
    f->blk_type     = 0;
    f->blk_start    = 0;
    f->blk_len      = 0;
    f->blk_addr     = 0;
    f->blk_access   = 0;
    f->disk_irq_ctr = (int32_t)FDS_CYCLES_PER_BYTE;
}

static void blk_advance(FdsState *f) {
    /* Commit accumulated bytes and advance to next logical block */
    f->blk_access = 0;
    f->blk_start += f->blk_addr;
    f->blk_addr   = 0;

    f->blk_type++;
    if (f->blk_type > 4)
        f->blk_type = 3;   /* cycle: FILEHDR → FILEDATA → FILEHDR → … */

    f->blk_len = blk_len_for(f, f->blk_type);
    f->disk_irq_ctr = (int32_t)FDS_CYCLES_PER_BYTE;
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

    size_t nread = fread(raw, 1, (size_t)sides * FDS_SIDE_BYTES, fp);
    fclose(fp);
    if (nread < (size_t)sides * FDS_SIDE_BYTES) {
        fprintf(stderr, "fds: disk '%s' truncated (%zu/%u bytes)\n",
                path, nread, (unsigned)(sides * FDS_SIDE_BYTES));
        free(raw); return false;
    }

    free(f->raw_disk);
    f->raw_disk      = raw;
    f->disk_sides    = sides;
    f->cur_side      = 0;
    f->disk_inserted = true;
    f->disk_change_cycles = 0;
    f->drive_ctrl_written = false;
    f->dirty         = false;
    blk_reset(f);
    f->transfer_flag = false;
    f->disk_irq_ctr  = -1;

    fprintf(stderr, "fds: loaded '%s' - %u side%s\n", path, sides, sides == 1 ? "" : "s");
    return true;
}

bool fds_disk_load_save(FdsState *f, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    uint8_t magic[8];
    if (fread(magic, 1, sizeof(magic), fp) != sizeof(magic) ||
        memcmp(magic, FDS_SAVE_MAGIC, 8) != 0) {
        fclose(fp);
        fprintf(stderr, "fds: save '%s' has wrong magic (old format?)\n", path);
        return false;
    }
    uint8_t sides = 0;
    if (fread(&sides, 1, 1, fp) != 1 || sides != f->disk_sides || !f->raw_disk) {
        fclose(fp);
        return false;
    }
    size_t raw_len = (size_t)sides * FDS_SIDE_BYTES;
    bool ok = fread(f->raw_disk, 1, raw_len, fp) == raw_len;
    fclose(fp);
    if (ok) {
        f->dirty = false;
        fprintf(stderr, "fds: loaded save '%s'\n", path);
    }
    return ok;
}

bool fds_disk_save(FdsState *f, const char *path) {
    if (!f->disk_inserted || !f->raw_disk || !path || !path[0])
        return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    size_t raw_len = (size_t)f->disk_sides * FDS_SIDE_BYTES;
    bool ok = fwrite(FDS_SAVE_MAGIC, 1, 8, fp) == 8 &&
              fwrite(&f->disk_sides, 1, 1, fp) == 1 &&
              fwrite(f->raw_disk, 1, raw_len, fp) == raw_len;
    fclose(fp);
    if (ok) {
        f->dirty = false;
        fprintf(stderr, "fds: saved to '%s'\n", path);
    }
    return ok;
}

void fds_disk_eject(FdsState *f) {
    free(f->raw_disk);
    f->raw_disk      = NULL;
    f->disk_sides    = 0;
    f->disk_inserted = false;
    f->disk_change_cycles = 0;
    f->transfer_flag = false;
    f->disk_irq_ctr  = -1;
    f->drive_ctrl_written = false;
    f->dirty         = false;
    blk_reset(f);
    fprintf(stderr, "fds: disk ejected\n");
}

bool fds_disk_flip(FdsState *f) {
    if (!f->disk_inserted || f->disk_sides == 0)
        return false;
    f->cur_side = (uint8_t)((f->cur_side + 1u) % f->disk_sides);
    f->transfer_flag = false;
    f->disk_irq_ctr  = -1;
    f->disk_change_cycles = FDS_DISK_CHANGE_CYCLES;
    blk_reset(f);
    fprintf(stderr, "fds: flipped to side %c (%u/%u)\n",
            (char)('A' + f->cur_side), (unsigned)f->cur_side + 1u,
            (unsigned)f->disk_sides);
    return true;
}

/* ── Per-cycle tick ──────────────────────────────────────────────────────── */

bool fds_tick(FdsState *f) {
    if (f->disk_change_cycles > 0)
        f->disk_change_cycles--;

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

    /* ---- Disk transfer IRQ countdown ----
     * Fires after FDS_CYCLES_PER_BYTE cycles following a block/byte access.
     * Only fires if IRQ enable ($4025 bit 7) is set. */
    if (f->disk_inserted && f->disk_change_cycles == 0 && f->disk_irq_ctr >= 0) {
        if (--f->disk_irq_ctr <= 0) {
            f->disk_irq_ctr = -1;
            if (f->drive_ctrl & 0x80)
                f->transfer_flag = true;
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
        /* Bit 1 = disk transfer IRQ (per FCEUX/nesdev); also mirrored in bit 7.
         * Bit 3 = mirror state (mirrors $4025 bit 3).
         * Bit 4 = CRC error (never set - we don't emulate hardware CRC checker).
         * Bit 0 = timer IRQ.
         * Reading clears both the timer and transfer flags. */
        uint8_t v = 0;
        if (f->timer_pending)    v |= 0x01;
        if (f->transfer_flag)    v |= 0x82;  /* bits 7 and 1 */
        if (f->drive_ctrl & 0x08) v |= 0x08; /* mirror state read-back */
        f->timer_pending  = false;
        f->transfer_flag  = false;
        return v;
    }
    case 0x4031: {
        /* Read next byte from current block (read mode only).
         * In write mode FCEUX returns 0xFF without acknowledging the
         * transfer IRQ; some BIOS write loops use this as a dummy read. */
        bool read_mode = (f->drive_ctrl & 0x04) != 0;
        if (!f->disk_inserted || !f->raw_disk || !read_mode) {
            return 0xFF;
        }
        f->transfer_flag = false;
        uint8_t byte = 0;
        if (f->blk_type > 0 && f->blk_addr < f->blk_len) {
            size_t idx = (size_t)f->cur_side * FDS_SIDE_BYTES
                       + f->blk_start + f->blk_addr;
            byte = f->raw_disk[idx];
            /* Capture file size from file header block */
            if (f->blk_type == 3) {
                if (f->blk_addr == 13) f->blk_filesize = byte;
                if (f->blk_addr == 14) f->blk_filesize |= (uint16_t)byte << 8;
            }
            f->blk_addr++;
        }
        f->read_data = byte;
        /* Schedule next transfer IRQ (real BIOS mode - HLE uses synchronous $E7A3) */
        if (!f->hle_mode)
            f->disk_irq_ctr = (int32_t)FDS_CYCLES_PER_BYTE;
        return byte;
    }
    case 0x4032: {
        uint8_t v = 0;
        if (!f->disk_inserted || f->disk_change_cycles > 0) v |= 0x01;
        if (f->disk_change_cycles > 0 || (f->drive_ctrl & 0x01) == 0)
            v |= 0x02;
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
        if (!f->disk_io_en) {
            f->timer_pending = false;
            f->transfer_flag = false;
        }
        break;

    case 0x4024:
        /* Write disk byte - write mode only ($4025 bit 2 = 0).
         * First write after a block transition is discarded (sync byte equivalent).
         * Does NOT clear transfer_flag (per FCEUX: only $4025/$4030/$4031 do that). */
        {
            bool write_mode = (f->drive_ctrl & 0x04) == 0;
            if (!f->disk_inserted || !f->raw_disk || !write_mode) break;

            if (f->blk_access == 0) {
                /* Discard first byte (sync marker - not stored in raw block data) */
                f->blk_access = 1;
                break;
            }

            if (f->blk_type > 0 && f->blk_addr < f->blk_len) {
                size_t idx = (size_t)f->cur_side * FDS_SIDE_BYTES
                           + f->blk_start + f->blk_addr;
                /* Capture file size when writing header block */
                if (f->blk_type == 3) {
                    if (f->blk_addr == 13) f->blk_filesize = val;
                    if (f->blk_addr == 14) f->blk_filesize |= (uint16_t)val << 8;
                }
                f->raw_disk[idx] = val;
                f->blk_addr++;
                f->dirty = true;
            }
        }
        break;

    case 0x4025:
        /* Clear disk transfer IRQ (per FCEUX behaviour) */
        f->transfer_flag = false;

        if (f->disk_inserted) {
            bool old_b6 = (f->drive_ctrl & 0x40) != 0;
            bool new_b6 = (val & 0x40) != 0;

            if (new_b6 && !old_b6) {
                /* Bit 6: 0→1 → advance to next logical block */
                blk_advance(f);
            }

            if (val & 0x02) {
                /* Bit 1: transfer reset → back to INIT */
                blk_reset(f);
            }

            if (val & 0x40) {
                /* Bit 6 set: always schedule a transfer IRQ */
                f->disk_irq_ctr = (int32_t)FDS_CYCLES_PER_BYTE;
            }
        }

        f->drive_ctrl = val;
        f->drive_ctrl_written = true;
        break;

    case 0x4026:
        /* External connector output - ignored */
        break;
    }
}
