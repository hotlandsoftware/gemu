/*
 * Block devices: QEMU-compatible -drive parsing, the drive table, and raw
 * image I/O (with snapshot=on copy-on-write in memory).  See gemu/drive.h.
 */
#include "gemu/drive.h"
#include "gemu/monitor.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define gemu_fseek _fseeki64
#  define gemu_ftell _ftelli64
#else
#  define gemu_fseek fseeko
#  define gemu_ftell ftello
#endif

#define MAX_DRIVES      32
#define SNAP_CHUNK      65536u   /* copy-on-write granularity for snapshot=on */

struct GemuDrive {
    GemuDriveInterface iface;
    int            bus, unit, index;
    GemuDriveMedia media;
    bool           readonly;        /* as configured (cdrom is always read-only) */
    bool           snapshot;
    bool           claimed;
    char           name[64];        /* id= or QEMU's default (ide0-hd0, ...) */
    char           file[512];       /* current medium, "" = empty */

    FILE          *f;
    uint64_t       size;
    uint8_t      **snap;            /* snapshot=on: written chunks, else NULL */
    size_t         n_snap;

    void         (*change_cb)(void *ud, bool inserted);
    void          *change_ud;
    GemuMediaDevice *mon_dev;       /* monitor's copy, to keep its file name current */
};

static GemuDrive         *drives[MAX_DRIVES];
static int                n_drives;
static GemuDriveInterface default_if = GEMU_DRIVE_IF_IDE;
static bool               snapshot_all;

static const struct {
    const char *name;
    int         max_devs;   /* units per bus; 0 = one bus, unlimited units */
} if_info[GEMU_DRIVE_IF_COUNT] = {
    [GEMU_DRIVE_IF_NONE]   = { "none",   0 },
    [GEMU_DRIVE_IF_IDE]    = { "ide",    2 },
    [GEMU_DRIVE_IF_SCSI]   = { "scsi",   7 },
    [GEMU_DRIVE_IF_FLOPPY] = { "floppy", 0 },
    [GEMU_DRIVE_IF_PFLASH] = { "pflash", 0 },
    [GEMU_DRIVE_IF_MTD]    = { "mtd",    0 },
    [GEMU_DRIVE_IF_SD]     = { "sd",     0 },
    [GEMU_DRIVE_IF_VIRTIO] = { "virtio", 0 },
};

void gemu_drive_set_default_if(GemuDriveInterface iface) { default_if = iface; }

/* ── Image files ─────────────────────────────────────────────────────────── */

static void close_medium(GemuDrive *d) {
    if (d->f) fclose(d->f);
    d->f = NULL;
    for (size_t i = 0; i < d->n_snap; i++) free(d->snap[i]);
    free(d->snap);
    d->snap = NULL;
    d->n_snap = 0;
    d->size = 0;
    d->file[0] = '\0';
}

/* Open file as d's medium.  On failure leaves d empty and fills err. */
static bool open_medium(GemuDrive *d, const char *file, char *err, size_t err_len) {
    close_medium(d);
    bool ro = d->readonly || d->snapshot;
    FILE *f = fopen(file, ro ? "rb" : "r+b");
    if (!f) {
        snprintf(err, err_len, "Could not open '%s': %s", file, strerror(errno));
        return false;
    }
    char magic[4] = {0};
    if (fread(magic, 1, 4, f) == 4 && memcmp(magic, "QFI\xfb", 4) == 0) {
        fclose(f);
        snprintf(err, err_len, "'%s' is a qcow2 image; only raw images are supported "
                               "(convert with: qemu-img convert -O raw)", file);
        return false;
    }
    if (gemu_fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(err, err_len, "Could not determine the size of '%s'", file);
        return false;
    }
    d->size = (uint64_t)gemu_ftell(f);
    d->f = f;
    snprintf(d->file, sizeof(d->file), "%s", file);
    if (d->snapshot) {
        d->n_snap = (size_t)((d->size + SNAP_CHUNK - 1) / SNAP_CHUNK);
        d->snap = calloc(d->n_snap ? d->n_snap : 1, sizeof(*d->snap));
        if (!d->snap) {
            close_medium(d);
            snprintf(err, err_len, "out of memory");
            return false;
        }
    }
    return true;
}

static bool file_read(GemuDrive *d, uint64_t off, void *buf, size_t len) {
    return gemu_fseek(d->f, (long long)off, SEEK_SET) == 0 &&
           fread(buf, 1, len, d->f) == len;
}

bool gemu_drive_read(GemuDrive *d, uint64_t off, void *buf, size_t len) {
    if (!d || !d->f || off > d->size || len > d->size - off) return false;
    if (!d->snap) return file_read(d, off, buf, len);
    uint8_t *out = buf;
    while (len) {
        size_t ci = (size_t)(off / SNAP_CHUNK), co = (size_t)(off % SNAP_CHUNK);
        size_t n = SNAP_CHUNK - co < len ? SNAP_CHUNK - co : len;
        if (d->snap[ci]) memcpy(out, d->snap[ci] + co, n);
        else if (!file_read(d, off, out, n)) return false;
        out += n; off += n; len -= n;
    }
    return true;
}

bool gemu_drive_write(GemuDrive *d, uint64_t off, const void *buf, size_t len) {
    if (!d || !d->f || d->readonly || off > d->size || len > d->size - off) return false;
    if (!d->snap) {
        bool ok = gemu_fseek(d->f, (long long)off, SEEK_SET) == 0 &&
                  fwrite(buf, 1, len, d->f) == len;
        return ok && fflush(d->f) == 0;
    }
    const uint8_t *in = buf;
    while (len) {
        size_t ci = (size_t)(off / SNAP_CHUNK), co = (size_t)(off % SNAP_CHUNK);
        size_t n = SNAP_CHUNK - co < len ? SNAP_CHUNK - co : len;
        if (!d->snap[ci]) {                 /* first write: copy the chunk */
            uint64_t base = (uint64_t)ci * SNAP_CHUNK;
            size_t have = d->size - base < SNAP_CHUNK ? (size_t)(d->size - base) : SNAP_CHUNK;
            d->snap[ci] = calloc(1, SNAP_CHUNK);
            if (!d->snap[ci] || !file_read(d, base, d->snap[ci], have)) return false;
        }
        memcpy(d->snap[ci] + co, in, n);
        in += n; off += n; len -= n;
    }
    return true;
}

/* ── Accessors ───────────────────────────────────────────────────────────── */

const char *gemu_drive_name(const GemuDrive *d)     { return d ? d->name : ""; }
const char *gemu_drive_filename(const GemuDrive *d) { return d ? d->file : ""; }
GemuDriveMedia gemu_drive_media(const GemuDrive *d) { return d ? d->media : GEMU_DRIVE_MEDIA_DISK; }
bool gemu_drive_inserted(const GemuDrive *d)        { return d && d->f; }
bool gemu_drive_readonly(const GemuDrive *d)        { return d && d->readonly; }
uint64_t gemu_drive_size(const GemuDrive *d)        { return d && d->f ? d->size : 0; }

static bool removable(const GemuDrive *d) {
    return d->media == GEMU_DRIVE_MEDIA_CDROM || d->iface == GEMU_DRIVE_IF_FLOPPY;
}

static void notify(GemuDrive *d) {
    if (d->mon_dev) snprintf(d->mon_dev->file, sizeof(d->mon_dev->file), "%s", d->file);
    if (d->change_cb) d->change_cb(d->change_ud, d->f != NULL);
}

bool gemu_drive_insert(GemuDrive *d, const char *file, char *err, size_t err_len) {
    if (!removable(d)) {
        snprintf(err, err_len, "Device '%s' is not removable", d->name);
        return false;
    }
    bool ok = open_medium(d, file, err, err_len);
    notify(d);
    return ok;
}

bool gemu_drive_eject(GemuDrive *d, char *err, size_t err_len) {
    if (!removable(d)) {
        snprintf(err, err_len, "Device '%s' is not removable", d->name);
        return false;
    }
    close_medium(d);
    notify(d);
    return true;
}

void gemu_drive_set_change_cb(GemuDrive *d, void (*cb)(void *ud, bool inserted), void *ud) {
    if (!d) return;
    d->change_cb = cb;
    d->change_ud = ud;
}

/* ── Monitor ─────────────────────────────────────────────────────────────── */

static GemuMediaResult mon_change(void *ud, const char *arg, char *err, size_t err_len) {
    return gemu_drive_insert(ud, arg, err, err_len) ? GEMU_MEDIA_OK : GEMU_MEDIA_ERR;
}

static GemuMediaResult mon_eject(void *ud, char *err, size_t err_len) {
    return gemu_drive_eject(ud, err, err_len) ? GEMU_MEDIA_OK : GEMU_MEDIA_ERR;
}

static void mon_status(void *ud, char *buf, size_t len) {
    GemuDrive *d = ud;
    if (!d->f)
        snprintf(buf, len, "[not inserted]");
    else
        snprintf(buf, len, "%s (raw%s%s)", d->file, d->readonly ? ", read-only" : "",
                 d->snapshot ? ", snapshot" : "");
}

void gemu_drive_attach_monitor(GemuDrive *d, GemuMonitor *mon) {
    if (!d || !mon) return;
    GemuMediaDevice dev = {
        .name   = d->name,
        .kind   = d->media == GEMU_DRIVE_MEDIA_CDROM ? "cdrom"
                : d->iface == GEMU_DRIVE_IF_FLOPPY   ? "floppy" : "disk",
        .ud     = d,
        .change = removable(d) ? mon_change : NULL,
        .eject  = removable(d) ? mon_eject : NULL,
        .status = mon_status,
    };
    snprintf(dev.file, sizeof(dev.file), "%s", d->file);
    if (gemu_monitor_register_media(mon, &dev))
        d->mon_dev = gemu_monitor_find_media(mon, d->name);
}

/* ── Drive table ─────────────────────────────────────────────────────────── */

static GemuDrive *find(GemuDriveInterface iface, int bus, int unit) {
    for (int i = 0; i < n_drives; i++)
        if (drives[i]->iface == iface && drives[i]->bus == bus && drives[i]->unit == unit)
            return drives[i];
    return NULL;
}

GemuDrive *gemu_drive_get(GemuDriveInterface iface, int bus, int unit) {
    GemuDrive *d = find(iface, bus, unit);
    if (d) d->claimed = true;
    return d;
}

bool gemu_drive_check_unclaimed(const char *machine) {
    bool ok = true;
    for (int i = 0; i < n_drives; i++) {
        GemuDrive *d = drives[i];
        if (d->claimed) continue;
        fprintf(stderr, "gemu: -drive %s: machine type '%s' does not support "
                        "if=%s,bus=%d,unit=%d\n",
                d->file[0] ? d->file : d->name, machine ? machine : "?",
                if_info[d->iface].name, d->bus, d->unit);
        ok = false;
    }
    return ok;
}

void gemu_drive_snapshot_all(void) {
    snapshot_all = true;
    /* Applies to drives given before -snapshot too: reopen them read-only
     * with a write overlay. */
    for (int i = 0; i < n_drives; i++) {
        GemuDrive *d = drives[i];
        if (d->snapshot) continue;
        d->snapshot = true;
        if (d->f) {
            char file[sizeof(d->file)], err[600];
            snprintf(file, sizeof(file), "%s", d->file);
            if (!open_medium(d, file, err, sizeof(err)))
                fprintf(stderr, "gemu: -snapshot: %s\n", err);
        }
    }
}

static void default_name(GemuDrive *d) {
    const char *ifn = if_info[d->iface].name;
    bool cd = d->media == GEMU_DRIVE_MEDIA_CDROM;
    if (if_info[d->iface].max_devs)
        snprintf(d->name, sizeof(d->name), "%s%d-%s%d", ifn, d->bus, cd ? "cd" : "hd", d->unit);
    else
        snprintf(d->name, sizeof(d->name), "%s%d", ifn, d->unit);
}

/* Split "a=b,c=d" honouring QEMU's ",," escape; returns the next option
 * (key and value point into buf) or NULL at the end. */
static char *next_opt(char **p, char **val) {
    char *s = *p;
    if (!*s) return NULL;
    char *out = s, *key = s;
    *val = NULL;
    while (*s) {
        if (*s == ',') {
            if (s[1] == ',') { *out++ = ','; s += 2; continue; }
            s++;
            break;
        }
        if (*s == '=' && !*val) { *out++ = '\0'; *val = out; s++; continue; }
        *out++ = *s++;
    }
    *out = '\0';
    *p = s;
    return key;
}

static bool parse_int(const char *v, int *out) {
    char *end;
    long n = strtol(v, &end, 10);
    if (!*v || *end || n < 0 || n > 1000) return false;
    *out = (int)n;
    return true;
}

static bool parse_bool(const char *v, bool *out) {
    if (!v || !strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "yes")) { *out = true; return true; }
    if (!strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "no"))      { *out = false; return true; }
    return false;
}

#define DRIVE_ERR(...) do { fprintf(stderr, "gemu: -drive %s: ", opts); \
                            fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
                            free(d); return false; } while (0)

bool gemu_drive_add(const char *opts) {
    GemuDrive *d = calloc(1, sizeof(*d));
    if (!d) return false;
    d->iface = default_if;
    d->media = GEMU_DRIVE_MEDIA_DISK;
    d->snapshot = snapshot_all;
    int bus = -1, unit = -1, index = -1;
    bool have_file = false, readonly_set = false;
    char file[512] = "", id[64] = "";

    char buf[1024];
    if (snprintf(buf, sizeof(buf), "%s", opts) >= (int)sizeof(buf)) DRIVE_ERR("options too long");
    char *p = buf, *val;
    for (char *key; (key = next_opt(&p, &val)) != NULL; ) {
        if (!*key) continue;
        if (!strcmp(key, "file")) {
            snprintf(file, sizeof(file), "%s", val ? val : "");
            have_file = val && *val;
        } else if (!strcmp(key, "if")) {
            int i;
            for (i = 0; i < GEMU_DRIVE_IF_COUNT && (!val || strcmp(val, if_info[i].name)); i++) {}
            if (i == GEMU_DRIVE_IF_COUNT) DRIVE_ERR("unsupported interface name: '%s'", val ? val : "");
            d->iface = (GemuDriveInterface)i;
        } else if (!strcmp(key, "bus")) {
            if (!val || !parse_int(val, &bus)) DRIVE_ERR("invalid bus '%s'", val ? val : "");
        } else if (!strcmp(key, "unit")) {
            if (!val || !parse_int(val, &unit)) DRIVE_ERR("invalid unit '%s'", val ? val : "");
        } else if (!strcmp(key, "index")) {
            if (!val || !parse_int(val, &index)) DRIVE_ERR("invalid index '%s'", val ? val : "");
        } else if (!strcmp(key, "media")) {
            if      (val && !strcmp(val, "disk"))  d->media = GEMU_DRIVE_MEDIA_DISK;
            else if (val && !strcmp(val, "cdrom")) d->media = GEMU_DRIVE_MEDIA_CDROM;
            else DRIVE_ERR("'%s' invalid media", val ? val : "");
        } else if (!strcmp(key, "readonly")) {
            if (!parse_bool(val, &d->readonly)) DRIVE_ERR("readonly must be on or off");
            readonly_set = true;
        } else if (!strcmp(key, "snapshot")) {
            if (!parse_bool(val, &d->snapshot)) DRIVE_ERR("snapshot must be on or off");
        } else if (!strcmp(key, "format")) {
            if (!val || strcmp(val, "raw") != 0)
                DRIVE_ERR("unsupported format '%s' (only raw images are supported)", val ? val : "");
        } else if (!strcmp(key, "id")) {
            snprintf(id, sizeof(id), "%s", val ? val : "");
        } else if (!strcmp(key, "cache") || !strcmp(key, "aio") || !strcmp(key, "discard") ||
                   !strcmp(key, "detect-zeroes") || !strcmp(key, "werror") ||
                   !strcmp(key, "rerror") || !strcmp(key, "copy-on-read") ||
                   !strcmp(key, "serial") || !strcmp(key, "addr") ||
                   !strcmp(key, "auto-read-only") || !strcmp(key, "throttling.iops-total") ||
                   !strcmp(key, "throttling.bps-total")) {
            /* accepted for QEMU compatibility; no effect */
        } else {
            DRIVE_ERR("Invalid parameter '%s'", key);
        }
    }

    if (d->media == GEMU_DRIVE_MEDIA_CDROM) {
        if (d->iface == GEMU_DRIVE_IF_FLOPPY) DRIVE_ERR("floppy drives can't hold media=cdrom");
        if (readonly_set && !d->readonly) DRIVE_ERR("CD-ROM media is read-only");
        d->readonly = true;
    }
    if (!have_file && !removable(d))
        DRIVE_ERR("a %s drive needs file= (only removable drives can be empty)",
                  if_info[d->iface].name);

    /* index / bus / unit, as QEMU's drive_new() resolves them */
    int max = if_info[d->iface].max_devs;
    if (index >= 0) {
        if (bus >= 0 || unit >= 0) DRIVE_ERR("index cannot be used with bus and unit");
        bus  = max ? index / max : 0;
        unit = max ? index % max : index;
    }
    if (bus < 0) bus = 0;
    if (unit < 0) {
        unit = 0;
        while (find(d->iface, bus, unit)) {
            unit++;
            if (max && unit >= max) { unit -= max; bus++; }
        }
    }
    if (max && unit >= max) DRIVE_ERR("unit %d too big (max is %d)", unit, max - 1);
    if (find(d->iface, bus, unit))
        DRIVE_ERR("drive with bus=%d, unit=%d (index=%d) exists", bus, unit,
                  max ? bus * max + unit : unit);
    if (n_drives == MAX_DRIVES) DRIVE_ERR("too many drives");
    d->bus = bus;
    d->unit = unit;
    d->index = max ? bus * max + unit : unit;

    if (id[0]) {
        for (int i = 0; i < n_drives; i++)
            if (!strcmp(drives[i]->name, id)) DRIVE_ERR("Duplicate ID '%s' for drive", id);
        snprintf(d->name, sizeof(d->name), "%s", id);
    } else {
        default_name(d);
    }

    if (have_file) {
        char err[600];
        if (!open_medium(d, file, err, sizeof(err))) DRIVE_ERR("%s", err);
    }
    drives[n_drives++] = d;
    return true;
}

bool gemu_drive_add_legacy(GemuDriveInterface iface, int index,
                           GemuDriveMedia media, const char *file) {
    /* Build the -drive string QEMU's drive_add() builds, escaping commas. */
    char esc[1100];
    size_t o = 0;
    for (const char *s = file; *s && o + 2 < sizeof(esc); s++) {
        if (*s == ',') esc[o++] = ',';
        esc[o++] = *s;
    }
    esc[o] = '\0';
    char opts[1200];
    snprintf(opts, sizeof(opts), "file=%s,if=%s,index=%d,media=%s", esc,
             if_info[iface].name, index, media == GEMU_DRIVE_MEDIA_CDROM ? "cdrom" : "disk");
    return gemu_drive_add(opts);
}
