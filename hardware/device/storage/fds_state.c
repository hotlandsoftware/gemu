/* The Famicom Disk System's save state: see fds_state.h. */
#include "storage/fds_state.h"
#include "gemu/dev/state.h"
#include <stdio.h>
#include <string.h>

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* ── the disk: its side count and every byte of it ──
 * raw_disk is the FdsState's own heap copy of the .fds image, and games
 * write to it (save files, high scores): its contents are machine state.
 * The side count must match the disk in the drive (0 = none): a load
 * doesn't swap disks. */

static void disk_save(void *ud, GemuStateWriter *w) {
    FdsState *f = ud;
    uint32_t sides = f->raw_disk ? f->disk_sides : 0;
    gemu_state_write_u32(w, sides);
    if (sides)
        gemu_state_write(w, f->raw_disk, (size_t)sides * FDS_SIDE_BYTES);
}
static bool disk_check(void *ud, GemuStateReader *r) {
    FdsState *f = ud;
    uint32_t sides, have = f->raw_disk ? f->disk_sides : 0;
    if (!gemu_state_read_u32(r, &sides)) {
        gemu_state_refuse(r, "fds disk: truncated");
        return false;
    }
    if (sides != have) {
        gemu_state_refuse(r, "fds disk: the state's disk has %u side%s, the drive's %u",
                          (unsigned)sides, sides == 1 ? "" : "s", (unsigned)have);
        return false;
    }
    if (gemu_state_left(r) != (size_t)sides * FDS_SIDE_BYTES) {
        gemu_state_refuse(r, "fds disk: %llu bytes, %llu expected",
                          (unsigned long long)gemu_state_left(r),
                          (unsigned long long)sides * FDS_SIDE_BYTES);
        return false;
    }
    return true;
}
static bool disk_load(void *ud, GemuStateReader *r) {
    FdsState *f = ud;
    uint32_t sides;
    if (!gemu_state_read_u32(r, &sides))
        return false;
    for (size_t off = 0, n = (size_t)sides * FDS_SIDE_BYTES; off < n;) {
        uint8_t buf[4096];
        size_t k = n - off < sizeof buf ? n - off : sizeof buf;
        if (!gemu_state_read(r, buf, k))
            return false;
        if (memcmp(f->raw_disk + off, buf, k)) {
            memcpy(f->raw_disk + off, buf, k);
            f->dirty = true;    /* differs from the save file now */
        }
        off += k;
    }
    return true;
}

/* ── hle_mode: whether a BIOS is installed or the stub (a setting that
 * decides what the RAM holds): a state from the other kind is refused ── */

static void hle_save(void *ud, GemuStateWriter *w) {
    uint8_t v = ((FdsState *)ud)->hle_mode;
    gemu_state_write(w, &v, 1);
}
static bool hle_check(void *ud, GemuStateReader *r) {
    uint8_t v;
    if (!gemu_state_read(r, &v, 1) || gemu_state_left(r)) {
        gemu_state_refuse(r, "fds hle_mode: bad size");
        return false;
    }
    bool have = ((FdsState *)ud)->hle_mode;
    if ((v != 0) != have) {
        gemu_state_refuse(r, "fds: the state was saved with %s, this machine has %s",
                          v ? "the HLE BIOS stub" : "a BIOS ROM",
                          have ? "the HLE BIOS stub" : "a BIOS ROM");
        return false;
    }
    return true;
}
static bool hle_load(void *ud, GemuStateReader *r) {
    return hle_check(ud, r);
}

/* FdsState, in order.  Excluded:
 *   bios          the BIOS ROM: from the file, not state
 *   raw_disk, disk_sides
 *                 the disk image (a heap buffer the FdsState owns) and its
 *                 size: one custom item, "disk", with every byte of it; a
 *                 load into a drive holding a disk with a different side
 *                 count (or none) is refused
 *   hle_mode      a setting (BIOS or HLE stub): custom item "hle_mode",
 *                 which only refuses a state saved with the other one
 *   dirty         "raw_disk differs from the save file" (a host flag for the
 *                 save on quit): not saved; the disk's load sets it if the
 *                 loaded bytes differ from what was there
 * Everything else is registered: ram, the drive, the block state machine,
 * the timer IRQ and the wavetable sound. */
bool fds_state_add(GemuDevice *d, const char *prefix, FdsState *x) {
    bool ok = true;
    char name[64];
    const char *p = prefix ? prefix : "";
    M(ram);
    snprintf(name, sizeof name, "%sdisk", p);
    ok &= gemu_state_add_custom(d, name, disk_save, disk_check, disk_load, x);
    M(cur_side); M(disk_inserted); M(disk_change_cycles);
    snprintf(name, sizeof name, "%shle_mode", p);
    ok &= gemu_state_add_custom(d, name, hle_save, hle_check, hle_load, x);
    M(timer_latch); M(timer_ctr); M(timer_enabled); M(timer_repeat); M(timer_pending);
    M(disk_io_en);
    M(drive_ctrl); M(drive_ctrl_written);
    M(blk_type); M(blk_start); M(blk_len); M(blk_addr); M(blk_filesize); M(blk_access);
    M(disk_irq_ctr);
    M(read_data); M(transfer_flag);
    M(snd_wav); M(snd_mod);
    M(snd_vol_dis); M(snd_vol_grow); M(snd_vol_spd); M(snd_vol); M(snd_vol_div);
    M(snd_wav_halt); M(snd_env_halt); M(snd_freq); M(snd_phase);
    M(snd_mod_dis); M(snd_gain_dis); M(snd_gain_grow); M(snd_gain_spd); M(snd_gain);
    M(snd_gain_div); M(snd_mod_freq); M(snd_mod_phase); M(snd_mod_cnt);
    M(snd_wav_write); M(snd_master_vol);
    M(snd_env_spd); M(snd_env_div);
    return ok;
}
