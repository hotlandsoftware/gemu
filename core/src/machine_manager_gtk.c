/*
 * Machine Manager (see gemu/machine_manager.h), GTK 3.
 *
 * The user's machines, like a VM manager's: each is a name, a machine type
 * (one of the data/machine XMLs) and its settings - ROM folder, memory,
 * processor, media, devices, sound, network, display, extra arguments -
 * kept in <config>/machines/<id>.ini.
 *
 * Laid out like a VM manager: menu bar and toolbar (New, Settings, Delete,
 * Start), the machines created so far on the left, and the selected one's
 * details on the right - what it is and how it is set up, down to the
 * command line it adds up to, so the manager also teaches the CLI.
 *
 * Settings edits a copy of the machine in pages (General, System, Storage,
 * Audio, Network, Devices), each offering only what the machine type has;
 * OK keeps the changes, Cancel drops them.  New picks a type from every
 * built-in machine (grouped by family, searchable) and names it.  Start
 * runs the command line as a child gemu process; the manager stays open,
 * passes the machine's stderr through to its own and shows it in a dialog
 * if the machine stops with an error - started from a desktop, there is no
 * terminal to read it in.
 *
 * Tables come from configure: generated/manager_machines.inc (types: their
 * media, memory range, cpu, sound...), manager_features.inc (-M switches),
 * manager_devices.inc (device categories), devices.inc, soundhw.inc and
 * romdb.inc.
 */
#ifdef GEMU_GTK
#include "gemu/machine_manager.h"
#include "gemu/args.h"
#include "gemu/gemu.h"
#include "gemu/util.h"
#include "gemu/sha256.h"
#include "gemu/screendump.h"
#include <glib/gstdio.h>
#include <math.h>
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <glib-unix.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

/* ── Built-in tables ─────────────────────────────────────────────────────── */

typedef struct {
    const char *name, *family, *desc, *status, *alias, *long_desc, *devices;
    const char *cpu, *ram, *ram_min, *ram_max, *soundhw, *chipset, *media;
} MachineType;

static const MachineType types[] = {
#include "generated/manager_machines.inc"
};
#define N_TYPES ((int)(sizeof types / sizeof *types))

static const GemuDevDesc3 devices[] = {
#include "generated/devices.inc"
    { NULL, NULL, NULL }
};
#define N_DEVICES ((int)(sizeof devices / sizeof *devices) - 1)

typedef struct { const char *name, *category; } DeviceCategory;
static const DeviceCategory device_categories[] = {
#include "generated/manager_devices.inc"
    { NULL, NULL }
};

static const GemuDevDesc3 soundhws[] = {
#include "generated/soundhw.inc"
    { NULL, NULL, NULL }
};
#define N_SOUNDHW ((int)(sizeof soundhws / sizeof *soundhws) - 1)

/* -M NAME,OPTS switches: values "" = on/off (on by default, off is "-NAME"),
 * else the choices for NAME=VALUE. */
typedef struct { const char *machine, *name, *desc, *values; } Feature;
static const Feature features[] = {
#include "generated/manager_features.inc"
    { NULL, NULL, NULL, NULL }
};
#define N_FEATURES ((int)(sizeof features / sizeof *features) - 1)

/* Same rows as hardware/romdb.c's table: the ROMs each machine knows by SHA-256. */
typedef struct { const char *sha256; uint32_t addr; const char *region, *machine, *label; } KnownRom;
static const KnownRom roms[] = {
#include "generated/romdb.inc"
};
#define N_ROMS ((int)(sizeof roms / sizeof *roms))
#define ROM_MAX_SIZE (64u << 20)                 /* romdb.c skips bigger files too */
#define MAX_MACHINE_ROMS 64

/* What each family's setup (src/<family>_setup.c) takes: the -cpu names,
 * whether it has -soundhw and -accel jit, and the unit of a bare -m number. */
typedef struct {
    const char *family, *label;
    bool soundhw, jit;
    guint64 ram_unit;
} FamilyInfo;

static const FamilyInfo families[] = {
    { "mos",   "MOS 65xx",      true,  false, 1u << 10 },
    { "ia64",  "Intel Itanium", true,  true,  1u << 20 },
    { "x86",   "Intel x86",     false, false, 1u << 10 },
    { "z80",   "Zilog Z80",     false, false, 1u << 10 },
    { "rca",   "RCA COSMAC",    true,  false, 1u << 10 },
    { "chip8", "CHIP-8",        false, false, 1u << 10 },
    { "mitsu", "Mitsubishi",    false, false, 1u << 10 },
    { "other", "Other",         false, false, 1u << 10 },
};
#define N_FAMILIES ((int)(sizeof families / sizeof *families))

/* The -cpu lists of the setups. */
static const struct { const char *family, *id, *desc; } cpus[] = {
    { "mos",   "6501",    "MOS Technology 6501" },
    { "mos",   "6502",    "MOS Technology 6502" },
    { "mos",   "6507",    "MOS Technology 6507" },
    { "mos",   "2a03",    "Ricoh 2A03 (NTSC NES)" },
    { "mos",   "2a07",    "Ricoh 2A07 (PAL NES)" },
    { "ia64",  "merced",  "Intel Itanium (Merced)" },
    { "x86",   "8086",    "Intel 8086" },
    { "x86",   "8088",    "Intel 8088" },
    { "z80",   "z80",     "Zilog Z80" },
    { "rca",   "cdp1802", "RCA CDP1802 COSMAC" },
    { "chip8", "chip8",   "CHIP-8 interpreter" },
    { "other", "ssem",    "Manchester SSEM (Baby)" },
};
#define N_CPUS ((int)(sizeof cpus / sizeof *cpus))
#define MAX_CPU_COUNT 4                          /* the i2000 is a 4-way board */

/* The drives a machine type's media="slot[:device],..." can name. */
typedef struct {
    const char *key, *opt;                       /* opt "" = the positional argument */
    const char *label, *controller, *where, *icon, *what, *patterns;
    bool disk;                                   /* a hard disk: can create one */
} SlotInfo;

static const SlotInfo slot_info[] = {
    { "cartridge", "-cartridge", "Cartridge", "Cartridge Slot", "", "media-flash,drive-removable-media",
      "Cartridge images", "*.nes;*.unf;*.bin;*.a26;*.rom;*.car;*.st2;*.zip", false },
    { "tape", "-tape", "Cassette", "Cassette Recorder", "", "media-tape,media-flash",
      "Tape images", "*.wav;*.kim;*.pap;*.bin;*.hex;*.cas", false },
    { "fda", "-fda", "Floppy Drive A:", "Floppy Controller", "Drive 0", "media-floppy,drive-removable-media",
      "Disk images", "*.img;*.ima;*.dsk;*.fds;*.atr;*.xfd;*.vfd", false },
    { "fdb", "-fdb", "Floppy Drive B:", "Floppy Controller", "Drive 1", "media-floppy,drive-removable-media",
      "Disk images", "*.img;*.ima;*.dsk;*.vfd", false },
    { "hda", "-hda", "Hard Disk 1", "IDE Controller", "Primary Master", "drive-harddisk",
      "Hard disk images (raw)", "*.img;*.raw;*.hdd;*.dsk", true },
    { "hdb", "-hdb", "Hard Disk 2", "IDE Controller", "Primary Slave", "drive-harddisk",
      "Hard disk images (raw)", "*.img;*.raw;*.hdd;*.dsk", true },
    { "cdrom", "-cdrom", "CD/DVD Drive", "IDE Controller", "Secondary Master", "media-optical,drive-optical",
      "CD/DVD images", "*.iso;*.img", false },
    { "rom", "-rom", "ROM Image", "ROM Socket", "", "media-flash,drive-removable-media",
      "ROM images", "*.bin;*.rom;*.hex", false },
    { "cpm", "-cpm", "CP/M Program", "CP/M", "", "application-x-executable",
      "CP/M programs", "*.com", false },
    { "program", "", "Program", "Program Memory", "", "application-x-executable",
      "CHIP-8 programs", "*.ch8;*.c8;*.rom;*.bin", false },
};
#define N_SLOT_INFO ((int)(sizeof slot_info / sizeof *slot_info))
#define MAX_SLOTS 8

typedef struct { const SlotInfo *info; char need[48]; } Slot;   /* need: -device it takes */

/* Disks a controller only takes in set sizes: Create offers these. */
static const struct { const char *device, *label; guint64 bytes; } disk_presets[] = {
    /* the 5000059 ROM's drive table (17 sectors of 512 bytes a track) */
    { "ibm-fixed-disk", "Type 0: 5 MB (153 cylinders, 4 heads)",  5326848 },
    { "ibm-fixed-disk", "Type 3: 10 MB (306 cylinders, 4 heads)", 10653696 },
    { "ibm-fixed-disk", "Type 2: 15 MB (306 cylinders, 6 heads)", 15980544 },
    { "ibm-fixed-disk", "Type 1: 26 MB (375 cylinders, 8 heads)", 26112000 },
};
#define N_DISK_PRESETS ((int)(sizeof disk_presets / sizeof *disk_presets))

static const FamilyInfo *family_info(const char *family) {
    for (int i = 0; i < N_FAMILIES; i++)
        if (strcmp(families[i].family, family) == 0) return &families[i];
    return NULL;
}

static int find_type(const char *name) {
    for (int i = 0; i < N_TYPES; i++)
        if (strcmp(types[i].name, name) == 0) return i;
    return -1;
}

static int type_roms(const MachineType *t, int *idx, int max) {
    int n = 0;
    for (int i = 0; i < N_ROMS && n < max; i++)
        if (strcmp(roms[i].machine, t->name) == 0) idx[n++] = i;
    return n;
}

static bool has_known_roms(const MachineType *t) {
    int idx[1];
    return type_roms(t, idx, 1) > 0;
}

static int type_slots(const MachineType *t, Slot *out) {
    int n = 0;
    char **v = g_strsplit(t->media, ",", -1);
    for (int i = 0; v[i] && n < MAX_SLOTS; i++) {
        char *item = g_strstrip(v[i]);
        if (!item[0]) continue;
        char *colon = strchr(item, ':');
        if (colon) *colon++ = '\0';
        for (int k = 0; k < N_SLOT_INFO; k++)
            if (strcmp(slot_info[k].key, item) == 0) {
                out[n].info = &slot_info[k];
                snprintf(out[n].need, sizeof out[n].need, "%s", colon ? colon : "");
                n++;
                break;
            }
    }
    g_strfreev(v);
    return n;
}

/* "a, b, c" or "a,b,c" contains name as a whole item */
static bool list_has(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; *p; ) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        const char *t = e;
        while (t > p && t[-1] == ' ') t--;
        if ((size_t)(t - p) == n && strncmp(p, name, n) == 0) return true;
        p = e;
    }
    return false;
}

static bool fits(const char *machines, const MachineType *t) {
    return list_has(machines, t->name) || (t->alias[0] && list_has(machines, t->alias));
}

static const GemuDevDesc3 *find_device(const char *name) {
    for (int i = 0; i < N_DEVICES; i++)
        if (strcmp(devices[i].name, name) == 0) return &devices[i];
    return NULL;
}

static const char *device_category(const char *name) {
    for (int i = 0; device_categories[i].name; i++)
        if (strcmp(device_categories[i].name, name) == 0) return device_categories[i].category;
    return "other";
}

/* devices.inc descriptions end in " [status]"; the lists don't need it */
static char *strip_status(const char *desc) {
    const char *b = strrchr(desc, '[');
    size_t n = strlen(desc);
    if (b && n && desc[n - 1] == ']' && b > desc && b[-1] == ' ') n = (size_t)(b - 1 - desc);
    return g_strndup(desc, n);
}

static char *device_desc(const char *name) {
    const GemuDevDesc3 *d = find_device(name);
    return d ? strip_status(d->desc) : g_strdup(name);
}

static bool ci_has(const char *hay, const char *needle) {
    char *h = g_ascii_strdown(hay, -1);
    bool r = strstr(h, needle) != NULL;
    g_free(h);
    return r;
}

/* Icon names (first one the theme has wins) for a device: by its category,
 * and for input devices by what it is. */
static const char *device_icon(const char *name) {
    const char *cat = device_category(name);
    const GemuDevDesc3 *d = find_device(name);
    const char *desc = d ? d->desc : "";
    if (strcmp(cat, "input") == 0) {
        if (ci_has(name, "mouse") || ci_has(desc, "mouse"))        return "input-mouse";
        if (ci_has(name, "mic") || ci_has(desc, "microphone"))     return "audio-input-microphone,input-gaming";
        if (ci_has(name, "key") || ci_has(desc, "keyboard") || ci_has(desc, "keypad"))
            return "input-keyboard";
        return "input-gaming";
    }
    if (strcmp(cat, "display") == 0) return "video-display";
    if (strcmp(cat, "network") == 0) return "network-wired,network-card";
    if (strcmp(cat, "data") == 0) {
        if (ci_has(desc, "fixed disk") || ci_has(desc, "hard"))    return "drive-harddisk";
        if (ci_has(desc, "cassette") || ci_has(desc, "tape"))      return "media-tape,drive-removable-media";
        if (ci_has(desc, "disk") || ci_has(desc, "810"))           return "media-floppy,drive-removable-media";
        return "drive-removable-media";
    }
    if (ci_has(desc, "terminal")) return "utilities-terminal";
    if (ci_has(desc, "memory") || ci_has(desc, "ram"))             return "media-flash,application-x-addon";
    return "application-x-addon,applications-other";
}

static const char *category_title(const char *cat) {
    if (strcmp(cat, "input") == 0)   return "Input devices";
    if (strcmp(cat, "display") == 0) return "Displays";
    if (strcmp(cat, "data") == 0)    return "Storage and data";
    if (strcmp(cat, "network") == 0) return "Network";
    return "Other devices";
}

static GIcon *icon_gicon(const char *names) {
    char **v = g_strsplit(names, ",", -1);
    GIcon *gi = g_themed_icon_new_from_names(v, -1);
    g_strfreev(v);
    return gi;
}

static GtkWidget *icon_image(const char *names, int px) {
    GIcon *gi = icon_gicon(names);
    GtkWidget *im = gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DND);
    g_object_unref(gi);
    gtk_image_set_pixel_size(GTK_IMAGE(im), px);
    return im;
}

/* The network adapter models (network devices) that fit a type. */
static int type_nics(const MachineType *t, const GemuDevDesc3 **out, int max) {
    int n = 0;
    for (int i = 0; i < N_DEVICES && n < max; i++)
        if (strcmp(device_category(devices[i].name), "network") == 0 && fits(devices[i].machines, t))
            out[n++] = &devices[i];
    return n;
}

/* The sound hardware a type can be given (not "none"). */
static int type_sound(const MachineType *t, const GemuDevDesc3 **out, int max) {
    int n = 0;
    const FamilyInfo *fi = family_info(t->family);
    if (!fi || !fi->soundhw) return 0;
    for (int i = 0; i < N_SOUNDHW && n < max; i++)
        if (strcmp(soundhws[i].name, "none") != 0 && fits(soundhws[i].machines, t))
            out[n++] = &soundhws[i];
    return n;
}

static bool type_has_audio(const MachineType *t) {
    const FamilyInfo *fi = family_info(t->family);
    const GemuDevDesc3 *s[1];
    return fi && fi->soundhw &&
           ((t->soundhw[0] && strcmp(t->soundhw, "none") != 0) || type_sound(t, s, 1) > 0);
}

static const char *cpu_desc(const char *id) {
    for (int i = 0; i < N_CPUS; i++)
        if (strcmp(cpus[i].id, id) == 0) return cpus[i].desc;
    return id;
}

/* ── Sizes ───────────────────────────────────────────────────────────────── */

/* "64K", "512M", "2G"; a bare number is in unit bytes.  0 if unreadable. */
static guint64 size_parse(const char *s, guint64 unit) {
    if (!s || !s[0]) return 0;
    char *end;
    guint64 v = g_ascii_strtoull(s, &end, 10);
    if (end == s) return 0;
    switch (g_ascii_toupper(*end)) {
    case '\0': return v * unit;
    case 'K':  return v << 10;
    case 'M':  return v << 20;
    case 'G':  return v << 30;
    }
    return 0;
}

/* For -m: the biggest unit that is exact. */
static char *size_arg(guint64 b) {
    if (b >= (1ull << 30) && !(b & ((1ull << 30) - 1))) return g_strdup_printf("%" G_GUINT64_FORMAT "G", b >> 30);
    if (b >= (1ull << 20) && !(b & ((1ull << 20) - 1))) return g_strdup_printf("%" G_GUINT64_FORMAT "M", b >> 20);
    return g_strdup_printf("%" G_GUINT64_FORMAT "K", b >> 10);
}

static char *size_human(guint64 b) {
    if (b >= (1ull << 30) && !(b & ((1ull << 30) - 1))) return g_strdup_printf("%" G_GUINT64_FORMAT " GB", b >> 30);
    if (b >= (1ull << 20) && !(b & ((1ull << 20) - 1))) return g_strdup_printf("%" G_GUINT64_FORMAT " MB", b >> 20);
    if (b >= (1ull << 30)) return g_strdup_printf("%.1f GB", (double)b / (1ull << 30));
    if (b >= (1ull << 20)) return g_strdup_printf("%.1f MB", (double)b / (1ull << 20));
    if (b >= (1ull << 10)) return g_strdup_printf("%" G_GUINT64_FORMAT " KB", b >> 10);
    return g_strdup_printf("%" G_GUINT64_FORMAT " bytes", b);
}

static guint64 type_ram_unit(const MachineType *t) {
    const FamilyInfo *fi = family_info(t->family);
    return fi ? fi->ram_unit : 1024;
}

static bool type_ram_settable(const MachineType *t) {
    return t->ram_max[0] && size_parse(t->ram_max, type_ram_unit(t)) > 0;
}

/* ── State ───────────────────────────────────────────────────────────────── */

/* One of the user's machines.  Strings NULL = the machine type's default. */
typedef struct {
    char       *id;                          /* <config>/machines/<id>.ini */
    char       *name;
    int         type;                        /* types[] index */
    char       *rom_dir;                     /* -rom DIR */
    GHashTable *media;                       /* slot key -> image path */
    GPtrArray  *devices;                     /* extra -device names */
    char       *ram, *cpu, *accel, *sound;   /* -m, -cpu, -accel, -soundhw */
    int         cpu_count;                   /* not emulated yet: kept, not passed */
    GHashTable *features;                    /* -M feature -> "off" or a value */
    bool        nic_off;                     /* adapter 1: -nic none */
    char       *nic_attach, *nic_ifname, *nic_model, *nic_mac;
    GPtrArray  *nic_fwd;                     /* hostfwd rules */
    char       *display, *extra;

    bool        running;
    GdkPixbuf  *shot;                        /* its screen, from the preview pipe */
    GtkWidget  *row, *row_name, *row_state;
} Vm;

/* The Settings dialog's widgets, while it is open. */
typedef struct {
    GtkWidget    *dialog, *title, *stack, *warn_box, *warn;
    GtkWidget    *name;
    GtkWidget    *rom_list, *rom_summary;
    GtkWidget    *ram_scale, *ram_spin;
    guint64       ram_unit;
    GtkTreeStore *st_store;
    GtkWidget    *st_tree, *st_attr;
    GtkWidget    *dev_list, *dev_add, *dev_remove;
    GtkWidget    *sound_combo;
    GtkWidget    *nic_box, *nic_ifname, *nic_ifname_label, *nic_mac, *nic_fwd;
    bool          syncing;
} SettingsUi;

typedef struct Manager {
    const char   *prog;
    GPtrArray    *vms;                       /* Vm * */
    Vm           *sel;
    bool          closing;                   /* window gone: waiting for machines to exit */
    int           running;
    GHashTable   *hash_cache;                /* path -> CachedHash */
    GPtrArray    *argv;                      /* the selected machine's command line */

    GtkWidget    *window, *list, *stack, *details, *preview;
    GtkWidget    *tb_settings, *tb_delete, *tb_start;          /* toolbar */
    GtkWidget    *mi_settings, *mi_delete, *mi_start;          /* Machine menu */

    Vm           *edit;                      /* the copy Settings changes */
    SettingsUi   *ui;
    bool          quit_pending;              /* SIGINT/SIGTERM came while Settings was open */
} Manager;

/* One machine the manager started. */
typedef struct {
    Manager    *mg;
    Vm         *vm;
    GPid        pid;
    GIOChannel *err;
    guint       err_watch;
    GString    *err_text;                    /* the last ERR_KEEP bytes of its stderr */
    GIOChannel *prv;                         /* GEMU_PREVIEW_FD pipe: screen frames */
    guint       prv_watch;
    GByteArray *prv_buf;
} Child;
#define ERR_KEEP 8192

static void show_details(Manager *mg);
static void update_row(Vm *vm);
static char *self_exe(const char *prog);

static GHashTable *str_table(void) {
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

static GHashTable *str_table_copy(GHashTable *src) {
    GHashTable *t = str_table();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, src);
    while (g_hash_table_iter_next(&it, &k, &v)) g_hash_table_insert(t, g_strdup(k), g_strdup(v));
    return t;
}

static GPtrArray *str_array(void) { return g_ptr_array_new_with_free_func(g_free); }

static GPtrArray *str_array_copy(GPtrArray *src) {
    GPtrArray *a = str_array();
    for (guint i = 0; i < src->len; i++) g_ptr_array_add(a, g_strdup(g_ptr_array_index(src, i)));
    return a;
}

static void set_str(char **field, const char *v) {
    char *old = *field;
    *field = v && v[0] ? g_strdup(v) : NULL;
    g_free(old);
}

static bool str_array_has(GPtrArray *a, const char *s) {
    for (guint i = 0; i < a->len; i++)
        if (strcmp(g_ptr_array_index(a, i), s) == 0) return true;
    return false;
}

static const char *vm_media(const Vm *vm, const char *slot) {
    return g_hash_table_lookup(vm->media, slot);
}

static void vm_set_media(Vm *vm, const char *slot, const char *path) {
    if (path && path[0]) g_hash_table_insert(vm->media, g_strdup(slot), g_strdup(path));
    else g_hash_table_remove(vm->media, slot);
}

static bool vm_has_device(const Vm *vm, const char *dev) {
    return str_array_has(vm->devices, dev);
}

/* A machine's device list holds everything attached, its type's default
 * <device>s included: a new machine starts with those. */
static void vm_attach_defaults(Vm *vm) {
    char **defs = g_strsplit(types[vm->type].devices, ",", -1);
    for (int i = 0, at = 0; defs[i]; i++) {          /* in front of any already there */
        char *d = g_strstrip(defs[i]);
        if (d[0]) g_ptr_array_insert(vm->devices, at++, g_strdup(d));
    }
    g_strfreev(defs);
}

/* Whether the family's setup takes -device none (no default devices), so
 * a default device can be taken off. */
static bool family_device_none(const char *family) {
    return strcmp(family, "mos") == 0 || strcmp(family, "rca") == 0 ||
           strcmp(family, "ia64") == 0;
}

static bool is_default_device(const MachineType *t, const char *dev) {
    return list_has(t->devices, dev);
}

/* ── Settings files ──────────────────────────────────────────────────────── */

static char *config_file(const char *rel) {
    char path[1024];
    gemu_config_path(path, sizeof path, rel);
    return g_strdup(path);
}

static char *machines_dir(void) { return config_file("machines"); }

static char *ini_get(const char *group, const char *key) {
    char *path = config_file("manager.ini");
    GKeyFile *kf = g_key_file_new();
    char *v = NULL;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
        v = g_key_file_get_string(kf, group, key, NULL);
    g_key_file_free(kf);
    g_free(path);
    return v;
}

static void ini_set(const char *group, const char *key, const char *value) {
    char *path = config_file("manager.ini");
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_string(kf, group, key, value);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

static char *vm_path(const Vm *vm) {
    char *dir = machines_dir();
    char *file = g_strconcat(vm->id, ".ini", NULL);
    char *path = g_build_filename(dir, file, NULL);
    g_free(file);
    g_free(dir);
    return path;
}

static void kf_set(GKeyFile *kf, const char *group, const char *key, const char *v) {
    if (v && v[0]) g_key_file_set_string(kf, group, key, v);
}

static void vm_save(const Vm *vm) {
    const MachineType *t = &types[vm->type];
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "machine", "name", vm->name);
    g_key_file_set_string(kf, "machine", "type", t->name);
    kf_set(kf, "machine", "rom_dir", vm->rom_dir);
    kf_set(kf, "machine", "ram", vm->ram);
    kf_set(kf, "machine", "cpu", vm->cpu);
    if (vm->cpu_count > 1) g_key_file_set_integer(kf, "machine", "cpus", vm->cpu_count);
    kf_set(kf, "machine", "accel", vm->accel);
    kf_set(kf, "machine", "sound", vm->sound);
    kf_set(kf, "machine", "display", vm->display);
    kf_set(kf, "machine", "extra", vm->extra);
    if (vm->devices->len)
        g_key_file_set_string_list(kf, "machine", "devices",
                                   (const char *const *)vm->devices->pdata, vm->devices->len);
    g_key_file_set_boolean(kf, "machine", "devices_include_defaults", TRUE);
    /* the defaults this list was made from: one the type gains later is added on load */
    g_key_file_set_string(kf, "machine", "known_defaults", t->devices);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, vm->media);
    while (g_hash_table_iter_next(&it, &k, &v)) g_key_file_set_string(kf, "media", k, v);
    g_hash_table_iter_init(&it, vm->features);
    while (g_hash_table_iter_next(&it, &k, &v)) g_key_file_set_string(kf, "features", k, v);
    if (vm->nic_off) g_key_file_set_boolean(kf, "network", "enabled", FALSE);
    kf_set(kf, "network", "attach", vm->nic_attach);
    kf_set(kf, "network", "ifname", vm->nic_ifname);
    kf_set(kf, "network", "model", vm->nic_model);
    kf_set(kf, "network", "mac", vm->nic_mac);
    if (vm->nic_fwd->len)
        g_key_file_set_string_list(kf, "network", "forward",
                                   (const char *const *)vm->nic_fwd->pdata, vm->nic_fwd->len);

    char *dir = machines_dir();
    g_mkdir_with_parents(dir, 0755);
    char *path = vm_path(vm);
    GError *e = NULL;
    if (!g_key_file_save_to_file(kf, path, &e)) {
        fprintf(stderr, "gemu: cannot save %s: %s\n", path, e->message);
        g_error_free(e);
    }
    g_free(path);
    g_free(dir);
    g_key_file_free(kf);
}

static Vm *vm_new(const char *id, const char *name, int type) {
    Vm *vm = g_new0(Vm, 1);
    vm->id = g_strdup(id);
    vm->name = g_strdup(name);
    vm->type = type;
    vm->media = str_table();
    vm->devices = str_array();
    vm->features = str_table();
    vm->nic_fwd = str_array();
    return vm;
}

static void vm_free(Vm *vm) {
    g_free(vm->id);
    g_free(vm->name);
    g_free(vm->rom_dir);
    g_hash_table_destroy(vm->media);
    g_ptr_array_free(vm->devices, TRUE);
    g_free(vm->ram);
    g_free(vm->cpu);
    g_free(vm->accel);
    g_free(vm->sound);
    g_hash_table_destroy(vm->features);
    g_free(vm->nic_attach);
    g_free(vm->nic_ifname);
    g_free(vm->nic_model);
    g_free(vm->nic_mac);
    g_ptr_array_free(vm->nic_fwd, TRUE);
    g_free(vm->display);
    g_free(vm->extra);
    if (vm->shot) g_object_unref(vm->shot);
    g_free(vm);
}

/* A copy of vm's settings, for the Settings dialog to change. */
static Vm *vm_clone(const Vm *vm) {
    Vm *c = g_new0(Vm, 1);
    c->id = g_strdup(vm->id);
    c->name = g_strdup(vm->name);
    c->type = vm->type;
    c->rom_dir = g_strdup(vm->rom_dir);
    c->media = str_table_copy(vm->media);
    c->devices = str_array_copy(vm->devices);
    c->ram = g_strdup(vm->ram);
    c->cpu = g_strdup(vm->cpu);
    c->cpu_count = vm->cpu_count;
    c->accel = g_strdup(vm->accel);
    c->sound = g_strdup(vm->sound);
    c->features = str_table_copy(vm->features);
    c->nic_off = vm->nic_off;
    c->nic_attach = g_strdup(vm->nic_attach);
    c->nic_ifname = g_strdup(vm->nic_ifname);
    c->nic_model = g_strdup(vm->nic_model);
    c->nic_mac = g_strdup(vm->nic_mac);
    c->nic_fwd = str_array_copy(vm->nic_fwd);
    c->display = g_strdup(vm->display);
    c->extra = g_strdup(vm->extra);
    return c;
}

/* dst takes src's settings, keeping its own identity and widgets; src is freed. */
static void vm_apply(Vm *dst, Vm *src) {
    Vm old = *dst;
    char *src_id = src->id;
    *dst = *src;
    dst->id = old.id;
    dst->running = old.running;
    dst->shot = old.shot;
    dst->row = old.row;
    dst->row_name = old.row_name;
    dst->row_state = old.row_state;
    *src = old;
    src->id = src_id;
    vm_free(src);
}

static char *kf_str(GKeyFile *kf, const char *group, const char *key) {
    char *v = g_key_file_get_string(kf, group, key, NULL);
    if (v && !v[0]) { g_free(v); v = NULL; }
    return v;
}

static void kf_list(GKeyFile *kf, const char *group, const char *key, GPtrArray *out) {
    gsize n = 0;
    char **v = g_key_file_get_string_list(kf, group, key, &n, NULL);
    for (gsize i = 0; i < n; i++) if (v[i][0]) g_ptr_array_add(out, g_strdup(v[i]));
    g_strfreev(v);
}

static Vm *vm_load(const char *path, const char *id) {
    GKeyFile *kf = g_key_file_new();
    Vm *vm = NULL;
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) goto out;
    char *type = g_key_file_get_string(kf, "machine", "type", NULL);
    int ti = type ? find_type(type) : -1;
    if (ti < 0) {
        fprintf(stderr, "gemu: %s: unknown machine type '%s', skipped\n", path, type ? type : "");
        g_free(type);
        goto out;
    }
    g_free(type);
    char *name = g_key_file_get_string(kf, "machine", "name", NULL);
    vm = vm_new(id, name && name[0] ? name : id, ti);
    g_free(name);
    vm->rom_dir = kf_str(kf, "machine", "rom_dir");
    vm->ram     = kf_str(kf, "machine", "ram");
    vm->cpu     = kf_str(kf, "machine", "cpu");
    vm->cpu_count = g_key_file_get_integer(kf, "machine", "cpus", NULL);
    vm->accel   = kf_str(kf, "machine", "accel");
    vm->sound   = kf_str(kf, "machine", "sound");
    vm->display = kf_str(kf, "machine", "display");
    vm->extra   = kf_str(kf, "machine", "extra");
    kf_list(kf, "machine", "devices", vm->devices);
    /* saved before the list held the defaults: they were attached anyway */
    if (!g_key_file_get_boolean(kf, "machine", "devices_include_defaults", NULL)) {
        vm_attach_defaults(vm);
    } else {
        /* defaults the type has gained since (e.g. the NES's controller,
         * attached by its code all along but listed in its XML later) */
        char *known = g_key_file_get_string(kf, "machine", "known_defaults", NULL);
        char **defs = g_strsplit(types[ti].devices, ",", -1);
        for (int i = 0, at = 0; defs[i]; i++) {
            char *d = g_strstrip(defs[i]);
            if (!d[0]) continue;
            if (!(known && list_has(known, d)) && !str_array_has(vm->devices, d))
                g_ptr_array_insert(vm->devices, at, g_strdup(d));
            at++;
        }
        g_strfreev(defs);
        g_free(known);
    }

    Slot slots[MAX_SLOTS];
    int n = type_slots(&types[ti], slots);
    for (int i = 0; i < n; i++) {
        char *p = kf_str(kf, "media", slots[i].info->key);
        if (p) vm_set_media(vm, slots[i].info->key, p);
        g_free(p);
    }
    for (int i = 0; i < N_FEATURES; i++) {
        if (strcmp(features[i].machine, types[ti].name) != 0) continue;
        char *v = kf_str(kf, "features", features[i].name);
        if (v) g_hash_table_insert(vm->features, g_strdup(features[i].name), v);
    }
    GError *e = NULL;
    gboolean on = g_key_file_get_boolean(kf, "network", "enabled", &e);
    vm->nic_off = !e && !on;
    g_clear_error(&e);
    vm->nic_attach = kf_str(kf, "network", "attach");
    vm->nic_ifname = kf_str(kf, "network", "ifname");
    vm->nic_model  = kf_str(kf, "network", "model");
    vm->nic_mac    = kf_str(kf, "network", "mac");
    kf_list(kf, "network", "forward", vm->nic_fwd);
out:
    g_key_file_free(kf);
    return vm;
}

static void load_vms(Manager *mg) {
    char *dir = machines_dir();
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        for (const char *f; (f = g_dir_read_name(d)); ) {
            if (!g_str_has_suffix(f, ".ini")) continue;
            char *id = g_strndup(f, strlen(f) - 4);
            char *path = g_build_filename(dir, f, NULL);
            Vm *vm = vm_load(path, id);
            if (vm) g_ptr_array_add(mg->vms, vm);
            g_free(path);
            g_free(id);
        }
        g_dir_close(d);
    }
    g_free(dir);
}

/* A file name for a new machine: its type, then -2, -3... */
static char *new_vm_id(Manager *mg, const char *type) {
    for (int n = 1; ; n++) {
        char *id = n == 1 ? g_strdup(type) : g_strdup_printf("%s-%d", type, n);
        bool taken = false;
        for (guint i = 0; i < mg->vms->len && !taken; i++)
            taken = strcmp(((Vm *)g_ptr_array_index(mg->vms, i))->id, id) == 0;
        if (!taken) {
            Vm probe = { .id = id };
            char *path = vm_path(&probe);
            taken = g_file_test(path, G_FILE_TEST_EXISTS);
            g_free(path);
        }
        if (!taken) return id;
        g_free(id);
    }
}

/* A type's description without its "(1977) (NTSC)" tail, for a default
 * name - the details show the whole description anyway. */
static char *short_name(const char *desc) {
    char *s = g_strdup(desc);
    for (;;) {
        g_strchomp(s);
        size_t n = strlen(s);
        char *open = n && s[n - 1] == ')' ? strrchr(s, '(') : NULL;
        if (!open || open == s) break;
        *open = '\0';
    }
    if (!s[0]) { g_free(s); s = g_strdup(desc); }
    return s;
}

/* "Name", or "Name (2)" if the user already has one called that */
static char *unique_name(Manager *mg, const char *base) {
    for (int n = 1; ; n++) {
        char *name = n == 1 ? g_strdup(base) : g_strdup_printf("%s (%d)", base, n);
        bool taken = false;
        for (guint i = 0; i < mg->vms->len && !taken; i++)
            taken = strcmp(((Vm *)g_ptr_array_index(mg->vms, i))->name, name) == 0;
        if (!taken) return name;
        g_free(name);
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void clear_children(GtkWidget *box) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
}

static GtkWidget *wrap_label(void) {
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    return l;
}

static GtkWidget *dim(GtkWidget *w) {
    gtk_style_context_add_class(gtk_widget_get_style_context(w), "dim-label");
    return w;
}

static GtkWidget *note(const char *text) {
    GtkWidget *l = dim(wrap_label());
    gtk_label_set_text(GTK_LABEL(l), text);
    return l;
}

static void message(Manager *mg, GtkWindow *parent, GtkMessageType type, const char *title, const char *text) {
    GtkWidget *d = gtk_message_dialog_new(parent ? parent : GTK_WINDOW(mg->window),
                                          GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                          type, GTK_BUTTONS_CLOSE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show(d);
}

static char *type_subtitle(const MachineType *t) {
    const FamilyInfo *fi = family_info(t->family);
    return g_strdup_printf("%s  ·  %s%s%s%s%s", t->name, fi ? fi->label : t->family,
                           t->status[0] ? "  ·  " : "", t->status,
                           t->alias[0] ? "  ·  variant of " : "", t->alias);
}

static const char *status_color(const char *s) {
    if (strcmp(s, "working") == 0)   return "#2e8b3d";
    if (strcmp(s, "imperfect") == 0) return "#b8860b";
    return "#c0392b";
}

/* ── ROMs ────────────────────────────────────────────────────────────────── *
 * Machines find their ROMs by SHA-256 in the folder given as -rom DIR
 * (romdb_load_dir); the manager hashes the same files to show which of the
 * machine's known ROMs a folder has, and to find a folder by itself. */

typedef struct { gint64 mtime; goffset size; char hex[65]; } CachedHash;

/* SHA-256 of path, cached by mtime and size; false if it isn't a ROM-sized file. */
static bool file_hash(Manager *mg, const char *path, char hex[65]) {
    GStatBuf st;
    if (g_stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (guint64)st.st_size > ROM_MAX_SIZE)
        return false;
    CachedHash *c = g_hash_table_lookup(mg->hash_cache, path);
    if (c && c->mtime == (gint64)st.st_mtime && c->size == (goffset)st.st_size) {
        memcpy(hex, c->hex, 65);
        return true;
    }
    uint8_t digest[GEMU_SHA256_DIGEST_LEN];
    if (!gemu_sha256_file(path, digest)) return false;
    c = g_new(CachedHash, 1);
    c->mtime = (gint64)st.st_mtime;
    c->size = (goffset)st.st_size;
    gemu_sha256_hex(digest, c->hex);
    g_hash_table_replace(mg->hash_cache, g_strdup(path), c);
    memcpy(hex, c->hex, 65);
    return true;
}

/* found[i] = file name in dir holding the type's i-th known ROM (or NULL);
 * returns how many were found. */
static int scan_rom_dir(Manager *mg, const char *dir, const MachineType *t,
                        const int *idx, int n, char **found) {
    for (int i = 0; i < n; i++) found[i] = NULL;
    GDir *d = dir ? g_dir_open(dir, 0, NULL) : NULL;
    if (!d) return 0;
    int hits = 0;
    for (const char *name; (name = g_dir_read_name(d)); ) {
        if (name[0] == '.') continue;
        char *path = g_build_filename(dir, name, NULL);
        char hex[65];
        if (file_hash(mg, path, hex))
            for (int i = 0; i < n; i++)
                if (!found[i] && strcmp(roms[idx[i]].sha256, hex) == 0 &&
                    strcmp(roms[idx[i]].machine, t->name) == 0) {
                    found[i] = g_strdup(name);
                    hits++;
                }
        g_free(path);
    }
    g_dir_close(d);
    return hits;
}

/* Where a type's ROMs probably are: <root>/<type>, <root>/<alias> and <root>
 * itself for each ROM root - the last one picked, ./roms, and roms next to
 * the executable.  NULL if none of them has any. */
static char *find_rom_dir(Manager *mg, const MachineType *t) {
    int idx[MAX_MACHINE_ROMS];
    int n = type_roms(t, idx, MAX_MACHINE_ROMS);
    if (!n) return NULL;

    GPtrArray *rootv = str_array();
    char *last_root = ini_get("manager", "rom_root");
    if (last_root) g_ptr_array_add(rootv, last_root);
    char *cwd = g_get_current_dir();
    g_ptr_array_add(rootv, g_build_filename(cwd, "roms", NULL));
    g_free(cwd);
    char *exe = self_exe(mg->prog);
    if (exe) {
        char *bin = g_path_get_dirname(exe);
        g_ptr_array_add(rootv, g_build_filename(bin, "..", "roms", NULL));
        g_ptr_array_add(rootv, g_build_filename(bin, "roms", NULL));
        g_free(bin);
        g_free(exe);
    }

    char *best = NULL;
    char *found[MAX_MACHINE_ROMS];
    for (guint r = 0; r < rootv->len && !best; r++) {
        const char *root = g_ptr_array_index(rootv, r);
        char *cands[3] = { g_build_filename(root, t->name, NULL),
                           t->alias[0] ? g_build_filename(root, t->alias, NULL) : NULL,
                           g_strdup(root) };
        for (int c = 0; c < 3; c++) {
            if (!best && cands[c] && g_file_test(cands[c], G_FILE_TEST_IS_DIR)) {
                int hits = scan_rom_dir(mg, cands[c], t, idx, n, found);
                for (int i = 0; i < n; i++) g_free(found[i]);
                if (hits) best = g_canonicalize_filename(cands[c], NULL);
            }
            g_free(cands[c]);
        }
    }
    g_ptr_array_free(rootv, TRUE);
    return best;
}

/* How many of the machine's known ROMs its folder has (*total = known). */
static int roms_found(Manager *mg, const Vm *vm, int *total) {
    const MachineType *t = &types[vm->type];
    int idx[MAX_MACHINE_ROMS];
    *total = type_roms(t, idx, MAX_MACHINE_ROMS);
    char *found[MAX_MACHINE_ROMS];
    int hits = scan_rom_dir(mg, vm->rom_dir, t, idx, *total, found);
    for (int i = 0; i < *total; i++) g_free(found[i]);
    return hits;
}

/* ── Command line ────────────────────────────────────────────────────────── */

static void push(GPtrArray *a, const char *s) { g_ptr_array_add(a, g_strdup(s)); }

static bool valid_mac(const char *s) {
    unsigned v[6];
    char tail;
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6)
        return false;
    return strlen(s) == 17 && !(v[0] & 1);                 /* unicast */
}

/* The -M argument: the type plus the feature switches changed from their defaults. */
static char *machine_arg(const Vm *vm) {
    const MachineType *t = &types[vm->type];
    GString *s = g_string_new(t->name);
    for (int i = 0; i < N_FEATURES; i++) {
        const Feature *f = &features[i];
        if (strcmp(f->machine, t->name) != 0) continue;
        const char *v = g_hash_table_lookup(vm->features, f->name);
        if (!v) continue;
        if (!f->values[0]) {
            if (strcmp(v, "off") == 0) g_string_append_printf(s, ",-%s", f->name);
        } else {
            g_string_append_printf(s, ",%s=%s", f->name, v);
        }
    }
    return g_string_free(s, FALSE);
}

/* The -nic argument for adapter 1, NULL if the default (NAT, nothing set) does. */
static char *nic_arg(const Vm *vm) {
    if (vm->nic_off) return g_strdup("none");
    bool tap = vm->nic_attach && strcmp(vm->nic_attach, "tap") == 0;
    if (!tap && !vm->nic_model && !vm->nic_mac && !vm->nic_fwd->len) return NULL;
    GString *s = g_string_new(tap ? "tap" : "user");
    if (tap && vm->nic_ifname) g_string_append_printf(s, ",ifname=%s", vm->nic_ifname);
    if (vm->nic_model) g_string_append_printf(s, ",model=%s", vm->nic_model);
    if (vm->nic_mac) g_string_append_printf(s, ",mac=%s", vm->nic_mac);
    for (guint i = 0; !tap && i < vm->nic_fwd->len; i++)
        g_string_append_printf(s, ",hostfwd=%s", (char *)g_ptr_array_index(vm->nic_fwd, i));
    return g_string_free(s, FALSE);
}

/* vm's command line into out; false (with a reason in *err) if it can't
 * be run as it stands. */
static bool build_argv(Manager *mg, const Vm *vm, GPtrArray *out, char **err) {
    g_ptr_array_set_size(out, 0);
    *err = NULL;
    if (!vm) { *err = g_strdup("No machine selected."); return false; }
    const MachineType *t = &types[vm->type];

    push(out, mg->prog);
    push(out, "-M");
    char *m = machine_arg(vm);
    push(out, m);
    g_free(m);
    if (vm->rom_dir && has_known_roms(t)) {
        push(out, "-rom");
        push(out, vm->rom_dir);
    }
    if (vm->ram && type_ram_settable(t)) {
        push(out, "-m");
        push(out, vm->ram);
    }
    if (vm->cpu) { push(out, "-cpu"); push(out, vm->cpu); }
    if (vm->accel) { push(out, "-accel"); push(out, vm->accel); }
    if (vm->sound && type_has_audio(t)) { push(out, "-soundhw"); push(out, vm->sound); }

    Slot slots[MAX_SLOTS];
    int n = type_slots(t, slots);
    for (int i = 0; i < n; i++) {
        const char *path = vm_media(vm, slots[i].info->key);
        if (!path) continue;
        if (slots[i].need[0] && !vm_has_device(vm, slots[i].need)) {
            char *d = device_desc(slots[i].need);
            *err = g_strdup_printf("%s has an image, but it needs the %s device (%s). "
                                   "Add it on the Devices page or remove the image.",
                                   slots[i].info->label, d, slots[i].need);
            g_free(d);
            return false;
        }
        if (slots[i].info->opt[0]) push(out, slots[i].info->opt);
        push(out, path);
    }
    /* Devices: nothing while the list is the type's defaults (the machine
     * attaches those itself); otherwise -device none and the whole list -
     * or, where the setup has no -device none, what's beyond the defaults. */
    GPtrArray *defs = str_array();
    vm_attach_defaults(&(Vm){ .type = vm->type, .devices = defs });
    bool same = defs->len == vm->devices->len;
    for (guint i = 0; same && i < defs->len; i++)
        same = strcmp(g_ptr_array_index(defs, i), g_ptr_array_index(vm->devices, i)) == 0;
    if (!same) {
        bool none = defs->len && family_device_none(t->family);
        if (none) { push(out, "-device"); push(out, "none"); }
        for (guint i = 0; i < vm->devices->len; i++) {
            const char *d = g_ptr_array_index(vm->devices, i);
            bool dflt = false;                               /* attached anyway: once each */
            for (guint k = 0; !none && k < defs->len && !dflt; k++)
                if (strcmp(g_ptr_array_index(defs, k), d) == 0) {
                    g_ptr_array_remove_index(defs, k);
                    dflt = true;
                }
            if (dflt) continue;
            push(out, "-device");
            push(out, d);
        }
    }
    g_ptr_array_free(defs, TRUE);
    const GemuDevDesc3 *nics[4];
    if (type_nics(t, nics, 4)) {
        if (vm->nic_mac && !valid_mac(vm->nic_mac)) {
            *err = g_strdup_printf("The network adapter's MAC address '%s' isn't a unicast "
                                   "xx:xx:xx:xx:xx:xx address.", vm->nic_mac);
            return false;
        }
        char *nic = nic_arg(vm);
        if (nic) { push(out, "-nic"); push(out, nic); }
        g_free(nic);
    }
    if (vm->display) {
        push(out, "-display");
        push(out, vm->display);
    }
    if (vm->extra && vm->extra[strspn(vm->extra, " \t")]) {
        int argc = 0;
        char **v = NULL;
        GError *e = NULL;
        if (!g_shell_parse_argv(vm->extra, &argc, &v, &e)) {
            *err = g_strdup_printf("Extra arguments: %s", e->message);
            g_error_free(e);
            return false;
        }
        for (int i = 0; i < argc; i++) push(out, v[i]);
        g_strfreev(v);
    }
    return true;
}

static void set_sensitive2(GtkWidget *a, GtkWidget *b, bool on) {
    gtk_widget_set_sensitive(a, on);
    gtk_widget_set_sensitive(b, on);
}

static void update_buttons(Manager *mg) {
    Vm *vm = mg->sel;
    char *err = NULL;
    bool ok = vm && build_argv(mg, vm, mg->argv, &err);
    g_free(err);
    set_sensitive2(mg->tb_start, mg->mi_start, ok && !vm->running);
    set_sensitive2(mg->tb_settings, mg->mi_settings, vm != NULL);
    set_sensitive2(mg->tb_delete, mg->mi_delete, vm && !vm->running);
}

/* The command line as it would be typed: quoting only what needs it, so
 * the common case reads naturally.  NULL (and *err set) if it can't run. */
static char *cmdline_text(Manager *mg, const Vm *vm, char **err) {
    GPtrArray *a = str_array();
    if (!build_argv(mg, vm, a, err)) { g_ptr_array_free(a, TRUE); return NULL; }
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < a->len; i++) {
        const char *arg = g_ptr_array_index(a, i);
        if (i) g_string_append_c(s, ' ');
        if (arg[0] && !arg[strcspn(arg, " \t\n'\"\\$`*?[]{}()<>|&;#~!")]) {
            g_string_append(s, arg);
        } else {
            char *q = g_shell_quote(arg);
            g_string_append(s, q);
            g_free(q);
        }
    }
    g_ptr_array_free(a, TRUE);
    return g_string_free(s, FALSE);
}

/* ── Machine list ────────────────────────────────────────────────────────── */

/* Consoles and arcade boards get a gamepad, everything else a computer. */
static const char *type_icon(const MachineType *t) {
    if (list_has(t->devices, "cx40") || list_has(t->devices, "nes-controller") ||
        strstr(t->desc, "Game") || strstr(t->desc, "Famiclone") || strstr(t->desc, "Poker") ||
        strstr(t->desc, "arcade") || strstr(t->media, "cartridge"))
        return "input-gaming";
    return "computer";
}

static void update_row(Vm *vm) {
    gtk_label_set_text(GTK_LABEL(vm->row_name), vm->name);
    GtkWidget *state = vm->row_state;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(state));
    gtk_image_set_from_icon_name(GTK_IMAGE(kids->data),
                                 vm->running ? "media-playback-start" : "system-shutdown",
                                 GTK_ICON_SIZE_MENU);
    gtk_label_set_text(GTK_LABEL(kids->next->data), vm->running ? "Running" : "Powered Off");
    g_list_free(kids);
}

static GtkWidget *vm_row(Vm *vm) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(row), 6);
    gtk_container_add(GTK_CONTAINER(row), icon_image(type_icon(&types[vm->type]), 40));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    vm->row_name = gtk_label_new(NULL);
    PangoAttrList *bold = pango_attr_list_new();
    pango_attr_list_insert(bold, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(vm->row_name), bold);
    pango_attr_list_unref(bold);
    gtk_label_set_xalign(GTK_LABEL(vm->row_name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(vm->row_name), PANGO_ELLIPSIZE_END);
    gtk_container_add(GTK_CONTAINER(box), vm->row_name);
    vm->row_state = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);     /* state icon + text */
    gtk_container_add(GTK_CONTAINER(vm->row_state), gtk_image_new());
    GtkWidget *st = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(st), 0);
    gtk_container_add(GTK_CONTAINER(vm->row_state), st);
    gtk_container_add(GTK_CONTAINER(box), vm->row_state);
    gtk_container_add(GTK_CONTAINER(row), box);
    gtk_widget_set_tooltip_text(row, types[vm->type].desc);
    vm->row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(vm->row), row);
    g_object_set_data(G_OBJECT(vm->row), "vm", vm);
    update_row(vm);
    gtk_widget_show_all(vm->row);
    return vm->row;
}

static int sort_rows(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data) {
    (void)data;
    const Vm *va = g_object_get_data(G_OBJECT(a), "vm"), *vb = g_object_get_data(G_OBJECT(b), "vm");
    return g_utf8_collate(va->name, vb->name);
}

static void on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box;
    Manager *mg = data;
    if (mg->closing) return;
    mg->sel = row ? g_object_get_data(G_OBJECT(row), "vm") : NULL;
    if (mg->sel) ini_set("manager", "selected", mg->sel->id);
    show_details(mg);
    update_buttons(mg);
}

static void select_vm(Manager *mg, Vm *vm) {
    if (vm) gtk_list_box_select_row(GTK_LIST_BOX(mg->list), GTK_LIST_BOX_ROW(vm->row));
    else gtk_list_box_unselect_all(GTK_LIST_BOX(mg->list));
}

/* ── Settings dialog ─────────────────────────────────────────────────────── *
 * A category list on the left, the category's page (tabs) on the right,
 * OK / Cancel below.  It changes mg->edit, a copy of the machine; every
 * change re-checks the command line and shows what's wrong at the bottom. */

static void edit_changed(Manager *mg) {
    SettingsUi *ui = mg->ui;
    if (!ui || ui->syncing) return;
    GPtrArray *a = str_array();
    char *err = NULL;
    bool ok = build_argv(mg, mg->edit, a, &err);
    g_ptr_array_free(a, TRUE);
    gtk_label_set_text(GTK_LABEL(ui->warn), err ? err : "");
    gtk_widget_set_visible(ui->warn_box, !ok);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(ui->dialog), GTK_RESPONSE_OK, ok);  /* as VirtualBox */
    g_free(err);
}

/* A tab of a notebook: a padded grid for form rows. */
static GtkWidget *tab(GtkWidget *notebook, const char *title) {
    GtkWidget *grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), grid);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll, gtk_label_new_with_mnemonic(title));
    return grid;
}

static int next_row(GtkWidget *grid) {
    int r = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(grid), "rows"));
    g_object_set_data(G_OBJECT(grid), "rows", GINT_TO_POINTER(r + 1));
    return r;
}

/* "Label:  widget" (label NULL: the widget spans both columns) */
static GtkWidget *form_row(GtkWidget *grid, const char *label, GtkWidget *w) {
    int r = next_row(grid);
    GtkWidget *l = NULL;
    if (label) {
        l = gtk_label_new_with_mnemonic(label);
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        gtk_widget_set_valign(l, GTK_ALIGN_BASELINE);
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
        gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), w, 1, r, 1, 1);
    } else {
        gtk_grid_attach(GTK_GRID(grid), w, 0, r, 2, 1);
    }
    gtk_widget_set_hexpand(w, TRUE);
    return l;
}

static GtkWidget *value_label(const char *text) {
    GtkWidget *l = wrap_label();
    gtk_label_set_text(GTK_LABEL(l), text);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_set_can_focus(l, FALSE);
    return l;
}

/* A combo box over (id, label) pairs. */
static GtkWidget *combo(void) { return gtk_combo_box_text_new(); }
static void combo_add(GtkWidget *c, const char *id, const char *label) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), id, label);
}

/* ── General ── */

static void on_name(GtkEntry *e, gpointer data) {
    Manager *mg = data;
    const char *v = gtk_entry_get_text(e);
    if (v[strspn(v, " ")]) {                        /* keep the old name until there is a new one */
        set_str(&mg->edit->name, v);
        char *title = g_strdup_printf("%s - Settings", v);
        gtk_window_set_title(GTK_WINDOW(mg->ui->dialog), title);
        g_free(title);
    }
    edit_changed(mg);
}

/* Fill the ROM rows for the edited machine's folder. */
static void show_roms(Manager *mg) {
    Vm *vm = mg->edit;
    SettingsUi *ui = mg->ui;
    const MachineType *t = &types[vm->type];
    int idx[MAX_MACHINE_ROMS];
    int n = type_roms(t, idx, MAX_MACHINE_ROMS);
    char *found[MAX_MACHINE_ROMS];
    int hits = scan_rom_dir(mg, vm->rom_dir, t, idx, n, found);

    clear_children(ui->rom_list);
    for (int i = 0; i < n; i++) {
        const KnownRom *r = &roms[idx[i]];
        GtkWidget *mark = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(mark), found[i]
            ? "<span foreground='#2e8b3d' weight='bold'>✓</span>"
            : "<span foreground='#c0392b' weight='bold'>✗</span>");
        char *region = r->region ? g_strdup_printf("(%s) ", r->region) : g_strdup("");
        char *mk = found[i] && strcmp(found[i], r->label) != 0
            ? g_markup_printf_escaped("%s <span alpha='60%%'>%s- found as %s</span>",
                                      r->label, region, found[i])
            : g_markup_printf_escaped("%s <span alpha='60%%'>%s</span>", r->label, region);
        GtkWidget *name = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(name), mk);
        g_free(mk);
        g_free(region);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_add(GTK_CONTAINER(row), mark);
        gtk_container_add(GTK_CONTAINER(row), name);
        gtk_widget_set_tooltip_text(row, r->sha256);
        gtk_container_add(GTK_CONTAINER(ui->rom_list), row);
        g_free(found[i]);
    }
    gtk_widget_show_all(ui->rom_list);

    char *sum;
    if (!vm->rom_dir)
        sum = g_strdup("Choose the folder that holds this machine's ROMs. They are "
                       "recognised by their contents, so the file names don't matter.");
    else if (!hits)
        sum = g_strdup("None of this machine's ROMs are in that folder.");
    else if (hits == n)
        sum = g_strdup_printf("All %d found.", n);
    else
        sum = g_strdup_printf("%d of %d found. Some machines need only one of them "
                              "(e.g. one BIOS version).", hits, n);
    gtk_label_set_text(GTK_LABEL(ui->rom_summary), sum);
    g_free(sum);
}

static void on_rom_folder(GtkFileChooserButton *b, gpointer data) {
    Manager *mg = data;
    Vm *vm = mg->edit;
    char *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(b));
    g_free(vm->rom_dir);
    vm->rom_dir = dir;
    if (dir) {
        /* new machines look next to this one: .../roms/kim1 -> .../roms */
        const MachineType *t = &types[vm->type];
        char *base = g_path_get_basename(dir);
        char *root = strcmp(base, t->name) == 0 || (t->alias[0] && strcmp(base, t->alias) == 0)
                     ? g_path_get_dirname(dir) : g_strdup(dir);
        ini_set("manager", "rom_root", root);
        g_free(root);
        g_free(base);
    }
    show_roms(mg);
    edit_changed(mg);
}

static void on_accel(GtkComboBox *c, gpointer data) {
    Manager *mg = data;
    const char *id = gtk_combo_box_get_active_id(c);
    set_str(&mg->edit->accel, id && strcmp(id, "default") != 0 ? id : NULL);
    edit_changed(mg);
}

static void on_feature_toggle(GtkToggleButton *b, gpointer data) {
    Manager *mg = data;
    const char *name = g_object_get_data(G_OBJECT(b), "feature");
    if (gtk_toggle_button_get_active(b)) g_hash_table_remove(mg->edit->features, name);
    else g_hash_table_insert(mg->edit->features, g_strdup(name), g_strdup("off"));
    edit_changed(mg);
}

static void on_feature_choice(GtkComboBox *c, gpointer data) {
    Manager *mg = data;
    const char *name = g_object_get_data(G_OBJECT(c), "feature");
    const char *id = gtk_combo_box_get_active_id(c);
    if (!id || strcmp(id, "auto") == 0) g_hash_table_remove(mg->edit->features, name);
    else g_hash_table_insert(mg->edit->features, g_strdup(name), g_strdup(id));
    edit_changed(mg);
}

static void on_display(GtkComboBox *c, gpointer data) {
    Manager *mg = data;
    const char *id = gtk_combo_box_get_active_id(c);
    set_str(&mg->edit->display, id && strcmp(id, "default") != 0 ? id : NULL);
    edit_changed(mg);
}

static void on_extra(GtkEntry *e, gpointer data) {
    Manager *mg = data;
    set_str(&mg->edit->extra, gtk_entry_get_text(e));
    edit_changed(mg);
}

static GtkWidget *page_general(Manager *mg) {
    Vm *vm = mg->edit;
    SettingsUi *ui = mg->ui;
    const MachineType *t = &types[vm->type];
    GtkWidget *nb = gtk_notebook_new();

    /* Basic */
    GtkWidget *g = tab(nb, "_Basic");
    ui->name = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ui->name), vm->name);
    g_signal_connect(ui->name, "changed", G_CALLBACK(on_name), mg);
    form_row(g, "_Name:", ui->name);

    GtkWidget *tbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_add(GTK_CONTAINER(tbox), icon_image(type_icon(t), 32));
    GtkWidget *tv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *tt = value_label(t->desc);
    PangoAttrList *bold = pango_attr_list_new();
    pango_attr_list_insert(bold, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(tt), bold);
    pango_attr_list_unref(bold);
    gtk_container_add(GTK_CONTAINER(tv), tt);
    char *sub = type_subtitle(t);
    gtk_container_add(GTK_CONTAINER(tv), dim(value_label(sub)));
    g_free(sub);
    gtk_container_add(GTK_CONTAINER(tbox), tv);
    GtkWidget *l = form_row(g, "Machine:", tbox);
    gtk_widget_set_valign(l, GTK_ALIGN_START);
    if (t->status[0]) {
        GtkWidget *st = value_label(NULL);
        char *mk = g_markup_printf_escaped("<span foreground='%s'>%s</span>", status_color(t->status), t->status);
        gtk_label_set_markup(GTK_LABEL(st), mk);
        g_free(mk);
        form_row(g, "Emulation:", st);
    }
    if (t->long_desc[0]) {
        GtkWidget *d = value_label(t->long_desc);
        gtk_label_set_max_width_chars(GTK_LABEL(d), 70);
        gtk_widget_set_margin_top(d, 6);
        l = form_row(g, "Description:", d);
        gtk_widget_set_valign(l, GTK_ALIGN_START);
        gtk_widget_set_margin_top(l, 6);
    }

    /* ROMs */
    if (has_known_roms(t)) {
        g = tab(nb, "_ROMs");
        GtkWidget *folder = gtk_file_chooser_button_new("Choose the ROM folder",
                                                        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
        if (vm->rom_dir) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(folder), vm->rom_dir);
        g_signal_connect(folder, "file-set", G_CALLBACK(on_rom_folder), mg);
        form_row(g, "_Folder:", folder);
        ui->rom_summary = note("");
        form_row(g, NULL, ui->rom_summary);
        ui->rom_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        l = form_row(g, "Known ROMs:", ui->rom_list);
        gtk_widget_set_valign(l, GTK_ALIGN_START);
        show_roms(mg);
    }

    /* Features: how it runs, and the machine's -M switches */
    g = tab(nb, "_Features");
    const FamilyInfo *fi = family_info(t->family);
    GtkWidget *accel = combo();
    if (fi && fi->jit) {
        combo_add(accel, "default", "JIT recompiler (default)");
        combo_add(accel, "interpreter", "Interpreter");
    } else {
        combo_add(accel, "default", "Interpreter");
        gtk_widget_set_sensitive(accel, FALSE);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(accel), vm->accel ? vm->accel : "default");
    g_signal_connect(accel, "changed", G_CALLBACK(on_accel), mg);
    form_row(g, "_Execution engine:", accel);
    form_row(g, NULL, note(fi && fi->jit
        ? "The JIT translates guest code to host code (x86-64 hosts) and is much faster; "
          "the interpreter runs one instruction at a time, which is handy for debugging."
        : "This machine's processor runs in the interpreter."));
    int n_feat = 0;
    for (int i = 0; i < N_FEATURES; i++) {
        const Feature *f = &features[i];
        if (strcmp(f->machine, t->name) != 0) continue;
        if (!n_feat++) {
            GtkWidget *h = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(h), "<b>Machine features</b>");
            gtk_widget_set_halign(h, GTK_ALIGN_START);
            gtk_widget_set_margin_top(h, 10);
            form_row(g, NULL, h);
        }
        const char *v = g_hash_table_lookup(vm->features, f->name);
        if (!f->values[0]) {
            GtkWidget *cb = gtk_check_button_new_with_label(f->desc);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb), !(v && strcmp(v, "off") == 0));
            g_object_set_data(G_OBJECT(cb), "feature", (gpointer)f->name);
            g_signal_connect(cb, "toggled", G_CALLBACK(on_feature_toggle), mg);
            gtk_widget_set_tooltip_text(cb, f->name);
            form_row(g, NULL, cb);
        } else {
            GtkWidget *c = combo();
            combo_add(c, "auto", "Automatic");
            char **vals = g_strsplit(f->values, ",", -1);
            for (int k = 0; vals[k]; k++) combo_add(c, vals[k], vals[k]);
            g_strfreev(vals);
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), v ? v : "auto");
            g_object_set_data(G_OBJECT(c), "feature", (gpointer)f->name);
            g_signal_connect(c, "changed", G_CALLBACK(on_feature_choice), mg);
            char *lab = g_strconcat(f->desc, ":", NULL);
            GtkWidget *fl = form_row(g, lab, c);
            gtk_widget_set_tooltip_text(fl, f->name);
            g_free(lab);
        }
    }

    /* Advanced */
    g = tab(nb, "_Advanced");
    GtkWidget *disp = combo();
    combo_add(disp, "default", "Machine default");
    combo_add(disp, "sdl", "SDL window");
    combo_add(disp, "gtk", "GTK window");
    combo_add(disp, "none", "None (headless)");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(disp), vm->display ? vm->display : "default");
    g_signal_connect(disp, "changed", G_CALLBACK(on_display), mg);
    form_row(g, "_Display:", disp);
    GtkWidget *extra = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(extra), "e.g. -monitor stdio -scale 2");
    gtk_entry_set_text(GTK_ENTRY(extra), vm->extra ? vm->extra : "");
    g_signal_connect(extra, "changed", G_CALLBACK(on_extra), mg);
    form_row(g, "E_xtra arguments:", extra);
    form_row(g, NULL, note("Anything else to pass to gemu, as you would type it. "
                           "They go after the options the other pages make."));
    return nb;
}

/* ── System ── */

static void ram_sync_scale(SettingsUi *ui, guint64 bytes) {
    double v = bytes ? log2((double)bytes) : 0;
    bool was = ui->syncing;
    ui->syncing = true;
    gtk_range_set_value(GTK_RANGE(ui->ram_scale), v);
    ui->syncing = was;
}

static void ram_set(Manager *mg, guint64 bytes) {
    const MachineType *t = &types[mg->edit->type];
    guint64 def = size_parse(t->ram, type_ram_unit(t));
    char *arg = size_arg(bytes);
    set_str(&mg->edit->ram, bytes == def ? NULL : arg);
    g_free(arg);
    edit_changed(mg);
}

static void on_ram_scale(GtkRange *r, gpointer data) {
    Manager *mg = data;
    SettingsUi *ui = mg->ui;
    if (ui->syncing) return;
    guint64 bytes = 1ull << (int)(gtk_range_get_value(r) + 0.5);
    ui->syncing = true;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->ram_spin), (double)(bytes / ui->ram_unit));
    ui->syncing = false;
    ram_set(mg, bytes);
}

static void on_ram_spin(GtkSpinButton *s, gpointer data) {
    Manager *mg = data;
    SettingsUi *ui = mg->ui;
    if (ui->syncing) return;
    guint64 bytes = (guint64)gtk_spin_button_get_value_as_int(s) * ui->ram_unit;
    ram_sync_scale(ui, bytes);
    ram_set(mg, bytes);
}

static void on_cpu(GtkComboBox *c, gpointer data) {
    Manager *mg = data;
    const char *id = gtk_combo_box_get_active_id(c);
    set_str(&mg->edit->cpu, id && strcmp(id, "default") != 0 ? id : NULL);
    edit_changed(mg);
}

static void on_cpu_count(GtkSpinButton *s, gpointer data) {
    Manager *mg = data;
    mg->edit->cpu_count = gtk_spin_button_get_value_as_int(s);
    edit_changed(mg);
}

/* "4 MB" under the scale's ends */
static GtkWidget *scale_ends(guint64 lo, guint64 hi) {
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    char *a = size_human(lo), *z = size_human(hi);
    GtkWidget *la = dim(gtk_label_new(a)), *lz = dim(gtk_label_new(z));
    g_free(a);
    g_free(z);
    gtk_box_pack_start(GTK_BOX(b), la, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(b), lz, FALSE, FALSE, 0);
    return b;
}

static GtkWidget *page_system(Manager *mg) {
    Vm *vm = mg->edit;
    SettingsUi *ui = mg->ui;
    const MachineType *t = &types[vm->type];
    GtkWidget *nb = gtk_notebook_new();

    /* Motherboard */
    GtkWidget *g = tab(nb, "_Motherboard");
    bool any = false;
    guint64 unit = type_ram_unit(t);
    if (type_ram_settable(t)) {
        guint64 lo = size_parse(t->ram_min, unit), hi = size_parse(t->ram_max, unit);
        guint64 def = size_parse(t->ram, unit);
        if (!lo) lo = 1024;
        guint64 cur = vm->ram ? size_parse(vm->ram, unit) : def;
        if (!cur) cur = def ? def : lo;
        ui->ram_unit = hi >= (16ull << 20) ? (1ull << 20) : 1024;

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *sbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand(sbox, TRUE);
        double llo = ceil(log2((double)lo)), lhi = floor(log2((double)hi));
        ui->ram_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, llo, lhi, 1);
        gtk_scale_set_draw_value(GTK_SCALE(ui->ram_scale), FALSE);
        gtk_scale_set_digits(GTK_SCALE(ui->ram_scale), 0);
        for (double e = llo; e <= lhi; e++) gtk_scale_add_mark(GTK_SCALE(ui->ram_scale), e, GTK_POS_BOTTOM, NULL);
        gtk_range_set_round_digits(GTK_RANGE(ui->ram_scale), 0);
        gtk_widget_set_size_request(ui->ram_scale, 280, -1);
        gtk_container_add(GTK_CONTAINER(sbox), ui->ram_scale);
        gtk_container_add(GTK_CONTAINER(sbox), scale_ends(lo, hi));
        gtk_container_add(GTK_CONTAINER(row), sbox);
        ui->ram_spin = gtk_spin_button_new_with_range((double)(lo / ui->ram_unit),
                                                      (double)(hi / ui->ram_unit), 1);
        gtk_spin_button_set_increments(GTK_SPIN_BUTTON(ui->ram_spin), 1, 16);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->ram_spin), (double)(cur / ui->ram_unit));
        gtk_widget_set_valign(ui->ram_spin, GTK_ALIGN_START);
        gtk_container_add(GTK_CONTAINER(row), ui->ram_spin);
        GtkWidget *u = gtk_label_new(ui->ram_unit == 1024 ? "KB" : "MB");
        gtk_widget_set_valign(u, GTK_ALIGN_START);
        gtk_widget_set_margin_top(u, 6);
        gtk_container_add(GTK_CONTAINER(row), u);
        ram_sync_scale(ui, cur);
        g_signal_connect(ui->ram_scale, "value-changed", G_CALLBACK(on_ram_scale), mg);
        g_signal_connect(ui->ram_spin, "value-changed", G_CALLBACK(on_ram_spin), mg);
        GtkWidget *l = form_row(g, "Base _Memory:", row);
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), ui->ram_spin);
        gtk_widget_set_valign(l, GTK_ALIGN_START);
        gtk_widget_set_margin_top(l, 6);
        char *d = size_human(def);
        char *txt = g_strdup_printf("The machine's standard is %s.", d);
        form_row(g, NULL, note(txt));
        g_free(txt);
        g_free(d);
        any = true;
    } else if (t->ram[0]) {
        char *d = size_human(size_parse(t->ram, unit));
        char *txt = g_strdup_printf("%s (fixed)", d);
        form_row(g, "Base Memory:", value_label(txt));
        g_free(txt);
        g_free(d);
        any = true;
    }
    if (t->chipset[0]) {
        GtkWidget *c = combo();
        combo_add(c, "c", t->chipset);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), "c");
        gtk_widget_set_sensitive(c, FALSE);
        form_row(g, "_Chipset:", c);
        any = true;
    }
    if (!any) form_row(g, NULL, note("Nothing to set on this machine's motherboard: "
                                     "its memory and chips are fixed."));

    /* Processor */
    g = tab(nb, "_Processor");
    GtkWidget *cpu = combo();
    int n = 0;
    if (t->cpu[0]) {
        char *lab = g_strdup_printf("%s (default)", cpu_desc(t->cpu));
        combo_add(cpu, "default", lab);
        g_free(lab);
        n++;
    }
    for (int i = 0; i < N_CPUS; i++)
        if (strcmp(cpus[i].family, t->family) == 0 && strcmp(cpus[i].id, t->cpu) != 0) {
            combo_add(cpu, cpus[i].id, cpus[i].desc);
            n++;
        }
    if (!n) combo_add(cpu, "default", "Built in");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(cpu), vm->cpu ? vm->cpu : "default");
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(cpu)) < 0) gtk_combo_box_set_active(GTK_COMBO_BOX(cpu), 0);
    gtk_widget_set_sensitive(cpu, n > 1);
    g_signal_connect(cpu, "changed", G_CALLBACK(on_cpu), mg);
    form_row(g, "P_rocessor:", cpu);
    if (n > 1)
        form_row(g, NULL, note("Software written for the machine expects its own processor; "
                               "another one is mostly useful for testing."));
    if (strcmp(t->family, "ia64") == 0) {
        GtkWidget *count = gtk_spin_button_new_with_range(1, MAX_CPU_COUNT, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(count), vm->cpu_count > 1 ? vm->cpu_count : 1);
        gtk_widget_set_hexpand(count, FALSE);
        GtkWidget *cb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_container_add(GTK_CONTAINER(cb), count);
        g_signal_connect(count, "value-changed", G_CALLBACK(on_cpu_count), mg);
        GtkWidget *l = form_row(g, "Processor(_s):", cb);
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), count);
        form_row(g, NULL, note("Only one processor is emulated so far; this is kept for "
                               "when SMP arrives and isn't passed to the machine yet."));
    }
    return nb;
}

/* ── Storage ── */

enum { ST_ICON, ST_TEXT, ST_SLOT, ST_N };          /* ST_SLOT -1 = a controller */

static void storage_fill(Manager *mg);
static void storage_show(Manager *mg, int slot);
static void devices_fill(Manager *mg);

static int selected_slot(SettingsUi *ui) {
    GtkTreeModel *m;
    GtkTreeIter it;
    int slot = -1;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->st_tree));
    if (gtk_tree_selection_get_selected(sel, &m, &it)) gtk_tree_model_get(m, &it, ST_SLOT, &slot, -1);
    return slot;
}

/* A slot got an image: also add the device its drive needs. */
static void media_set(Manager *mg, int slot, const char *path) {
    Vm *vm = mg->edit;
    Slot slots[MAX_SLOTS];
    type_slots(&types[vm->type], slots);
    vm_set_media(vm, slots[slot].info->key, path);
    if (path && slots[slot].need[0] && !vm_has_device(vm, slots[slot].need)) {
        g_ptr_array_add(vm->devices, g_strdup(slots[slot].need));
        devices_fill(mg);
    }
    storage_fill(mg);
    edit_changed(mg);
}

static void on_media_file(GtkFileChooserButton *b, gpointer data) {
    Manager *mg = data;
    char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(b));
    media_set(mg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "slot")), path);
    g_free(path);
}

static void on_media_eject(GtkButton *b, gpointer data) {
    Manager *mg = data;
    media_set(mg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "slot")), NULL);
}

/* Create an empty raw disk image and put it in the slot. */
static void on_media_create(GtkButton *b, gpointer data) {
    Manager *mg = data;
    SettingsUi *ui = mg->ui;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "slot"));
    Slot slots[MAX_SLOTS];
    type_slots(&types[mg->edit->type], slots);
    const char *need = slots[slot].need;
    GtkWidget *d = gtk_file_chooser_dialog_new("Create a Hard Disk Image", GTK_WINDOW(ui->dialog),
                                               GTK_FILE_CHOOSER_ACTION_SAVE,
                                               "_Cancel", GTK_RESPONSE_CANCEL,
                                               "C_reate", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(d), TRUE);
    char *fname = g_strdup_printf("%s.img", mg->edit->id);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d), fname);
    g_free(fname);
    GtkWidget *extra = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *sl = gtk_label_new_with_mnemonic("_Size:");
    gtk_container_add(GTK_CONTAINER(extra), sl);
    GtkWidget *size = NULL, *preset = NULL;
    for (int i = 0; i < N_DISK_PRESETS; i++) {
        if (strcmp(disk_presets[i].device, need) != 0) continue;
        if (!preset) preset = combo();
        char *id = g_strdup_printf("%" G_GUINT64_FORMAT, disk_presets[i].bytes);
        combo_add(preset, id, disk_presets[i].label);
        g_free(id);
    }
    if (preset) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(preset), 1);
        gtk_label_set_mnemonic_widget(GTK_LABEL(sl), preset);
        gtk_container_add(GTK_CONTAINER(extra), preset);
        char *dd = device_desc(need);
        char *why = g_strdup_printf("(the drive types the %s knows)", dd);
        gtk_container_add(GTK_CONTAINER(extra), dim(gtk_label_new(why)));
        g_free(why);
        g_free(dd);
    } else {
        size = gtk_spin_button_new_with_range(1, 2u << 20, 1);
        gtk_spin_button_set_increments(GTK_SPIN_BUTTON(size), 1, 1024);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(size), 8192);
        gtk_label_set_mnemonic_widget(GTK_LABEL(sl), size);
        gtk_container_add(GTK_CONTAINER(extra), size);
        gtk_container_add(GTK_CONTAINER(extra), gtk_label_new("MB"));
        gtk_container_add(GTK_CONTAINER(extra), dim(gtk_label_new("(a sparse file: it takes no space until written)")));
    }
    gtk_widget_show_all(extra);
    gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(d), extra);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        guint64 bytes = preset
            ? g_ascii_strtoull(gtk_combo_box_get_active_id(GTK_COMBO_BOX(preset)), NULL, 10)
            : (guint64)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(size)) << 20;
        GFile *f = g_file_new_for_path(path);
        GError *e = NULL;
        GFileOutputStream *os = g_file_replace(f, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &e);
        bool ok = os && g_seekable_truncate(G_SEEKABLE(os), (goffset)bytes, NULL, &e) &&
                  g_output_stream_close(G_OUTPUT_STREAM(os), NULL, &e);
        if (os) g_object_unref(os);
        g_object_unref(f);
        if (ok) media_set(mg, slot, path);
        else {
            message(mg, GTK_WINDOW(ui->dialog), GTK_MESSAGE_ERROR, "Could not create the disk image",
                    e ? e->message : path);
            g_clear_error(&e);
        }
        g_free(path);
    }
    gtk_widget_destroy(d);
}

static void storage_select(Manager *mg, int slot) {
    SettingsUi *ui = mg->ui;
    GtkTreeModel *m = GTK_TREE_MODEL(ui->st_store);
    GtkTreeIter c, it;
    for (gboolean okc = gtk_tree_model_get_iter_first(m, &c); okc; okc = gtk_tree_model_iter_next(m, &c))
        for (gboolean ok = gtk_tree_model_iter_children(m, &it, &c); ok; ok = gtk_tree_model_iter_next(m, &it)) {
            int s;
            gtk_tree_model_get(m, &it, ST_SLOT, &s, -1);
            if (s == slot) {
                gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->st_tree)), &it);
                return;
            }
        }
}

/* The tree: controllers, and the drives on each with what's in them. */
static void storage_fill(Manager *mg) {
    SettingsUi *ui = mg->ui;
    Vm *vm = mg->edit;
    int keep = selected_slot(ui);
    Slot slots[MAX_SLOTS];
    int n = type_slots(&types[vm->type], slots);
    bool was = ui->syncing;
    ui->syncing = true;
    gtk_tree_store_clear(ui->st_store);
    for (int i = 0; i < n; i++) {
        char *ctrl = slots[i].need[0] ? device_desc(slots[i].need) : g_strdup(slots[i].info->controller);
        GtkTreeIter parent, it;
        bool found = false;
        GtkTreeModel *m = GTK_TREE_MODEL(ui->st_store);
        for (gboolean ok = gtk_tree_model_get_iter_first(m, &parent); ok && !found;
             ok = gtk_tree_model_iter_next(m, &parent)) {
            char *name;
            gtk_tree_model_get(m, &parent, ST_TEXT, &name, -1);
            char *want = g_markup_printf_escaped("<b>%s</b>", ctrl);
            found = strcmp(name, want) == 0;
            g_free(want);
            g_free(name);
            if (found) break;
        }
        if (!found) {
            char *mk = g_markup_printf_escaped("<b>%s</b>", ctrl);
            GIcon *gi = icon_gicon(slots[i].need[0] ? device_icon(slots[i].need) : "drive-removable-media");
            gtk_tree_store_insert_with_values(ui->st_store, &parent, NULL, -1,
                                              ST_ICON, gi, ST_TEXT, mk, ST_SLOT, -1, -1);
            g_object_unref(gi);
            g_free(mk);
        }
        const char *path = vm_media(vm, slots[i].info->key);
        char *base = path ? g_path_get_basename(path) : NULL;
        char *mk = g_markup_printf_escaped("%s\n<small>%s</small>", slots[i].info->label,
                                           base ? base : "Empty");
        GIcon *gi = icon_gicon(slots[i].info->icon);
        gtk_tree_store_insert_with_values(ui->st_store, &it, &parent, -1,
                                          ST_ICON, gi, ST_TEXT, mk, ST_SLOT, i, -1);
        g_object_unref(gi);
        g_free(mk);
        g_free(base);
        g_free(ctrl);
    }
    gtk_tree_view_expand_all(GTK_TREE_VIEW(ui->st_tree));
    ui->syncing = was;
    storage_select(mg, keep >= 0 ? keep : 0);
}

static GtkWidget *attr_heading(const char *text) {
    GtkWidget *h = gtk_label_new(NULL);
    char *mk = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(h), mk);
    g_free(mk);
    gtk_widget_set_halign(h, GTK_ALIGN_START);
    return h;
}

/* The right side: the selected drive's image and what it is. */
static void storage_show(Manager *mg, int slot) {
    SettingsUi *ui = mg->ui;
    Vm *vm = mg->edit;
    clear_children(ui->st_attr);
    Slot slots[MAX_SLOTS];
    int n = type_slots(&types[vm->type], slots);
    if (slot < 0 || slot >= n) {
        gtk_container_add(GTK_CONTAINER(ui->st_attr),
                          note("Choose a drive on the left to put an image in it."));
        gtk_widget_show_all(ui->st_attr);
        return;
    }
    const Slot *s = &slots[slot];
    const char *path = vm_media(vm, s->info->key);
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    form_row(g, NULL, attr_heading("Attributes"));
    form_row(g, "Drive:", value_label(s->info->label));
    if (s->info->where[0] && !s->need[0]) form_row(g, "Position:", value_label(s->info->where));

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *file = gtk_file_chooser_button_new("Choose an image", GTK_FILE_CHOOSER_ACTION_OPEN);
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, s->info->what);
    char **pats = g_strsplit(s->info->patterns, ";", -1);
    for (int i = 0; pats[i]; i++) {
        gtk_file_filter_add_pattern(ff, pats[i]);
        char *up = g_ascii_strup(pats[i], -1);
        gtk_file_filter_add_pattern(ff, up);
        g_free(up);
    }
    g_strfreev(pats);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file), ff);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file), all);
    if (path) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(file), path);
    gtk_widget_set_hexpand(file, TRUE);
    g_object_set_data(G_OBJECT(file), "slot", GINT_TO_POINTER(slot));
    g_signal_connect(file, "file-set", G_CALLBACK(on_media_file), mg);
    gtk_container_add(GTK_CONTAINER(row), file);
    GtkWidget *eject = gtk_button_new_from_icon_name("media-eject", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(eject, s->info->disk ? "Remove the disk image" : "Eject");
    gtk_widget_set_sensitive(eject, path != NULL);
    g_object_set_data(G_OBJECT(eject), "slot", GINT_TO_POINTER(slot));
    g_signal_connect(eject, "clicked", G_CALLBACK(on_media_eject), mg);
    gtk_container_add(GTK_CONTAINER(row), eject);
    GtkWidget *l = form_row(g, "_Image:", row);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), file);
    if (s->info->disk) {
        GtkWidget *create = gtk_button_new_with_mnemonic("_Create New Disk Image…");
        gtk_widget_set_halign(create, GTK_ALIGN_START);
        g_object_set_data(G_OBJECT(create), "slot", GINT_TO_POINTER(slot));
        g_signal_connect(create, "clicked", G_CALLBACK(on_media_create), mg);
        form_row(g, "", create);
    }

    form_row(g, NULL, attr_heading("Information"));
    if (path) {
        GStatBuf st;
        bool there = g_stat(path, &st) == 0;
        char *sz = !there ? g_strdup("The file is missing")
                 : !S_ISREG(st.st_mode) ? g_strdup("Not a file")
                 : size_human((guint64)st.st_size);
        form_row(g, "Size:", value_label(sz));
        g_free(sz);
        form_row(g, "Location:", value_label(path));
    } else {
        form_row(g, NULL, note(s->info->disk ? "No disk attached." : "Empty."));
    }
    if (s->need[0]) {
        char *dd = device_desc(s->need);
        char *txt = vm_has_device(vm, s->need)
            ? g_strdup_printf("This drive is on the %s (%s).", dd, s->need)
            : g_strdup_printf("This drive is on the %s (%s), which isn't attached: "
                              "choosing an image adds it.", dd, s->need);
        form_row(g, NULL, note(txt));
        g_free(txt);
        g_free(dd);
    }
    gtk_container_add(GTK_CONTAINER(ui->st_attr), g);
    gtk_widget_show_all(ui->st_attr);
}

static void on_storage_sel(GtkTreeSelection *sel, gpointer data) {
    (void)sel;
    Manager *mg = data;
    if (mg->ui->syncing) return;
    storage_show(mg, selected_slot(mg->ui));
}

static GtkWidget *framed(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(NULL);
    GtkWidget *l = gtk_label_new(NULL);
    char *mk = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(l), mk);
    g_free(mk);
    gtk_frame_set_label_widget(GTK_FRAME(f), l);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static GtkWidget *page_storage(Manager *mg) {
    SettingsUi *ui = mg->ui;
    Slot slots[MAX_SLOTS];
    if (!type_slots(&types[mg->edit->type], slots)) {
        GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_set_border_width(GTK_CONTAINER(b), 14);
        gtk_container_add(GTK_CONTAINER(b), note("This machine has no drives or slots: "
                                                 "its software is in its ROMs."));
        return b;
    }
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_set_border_width(GTK_CONTAINER(paned), 6);
    ui->st_store = gtk_tree_store_new(ST_N, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_INT);
    ui->st_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->st_store));
    g_object_unref(ui->st_store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ui->st_tree), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *pr = gtk_cell_renderer_pixbuf_new();
    g_object_set(pr, "stock-size", GTK_ICON_SIZE_LARGE_TOOLBAR, NULL);
    gtk_tree_view_column_pack_start(col, pr, FALSE);
    gtk_tree_view_column_add_attribute(col, pr, "gicon", ST_ICON);
    GtkCellRenderer *tr = gtk_cell_renderer_text_new();
    g_object_set(tr, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_column_pack_start(col, tr, TRUE);
    gtk_tree_view_column_add_attribute(col, tr, "markup", ST_TEXT);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ui->st_tree), col);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->st_tree)), "changed",
                     G_CALLBACK(on_storage_sel), mg);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), ui->st_tree);
    gtk_widget_set_size_request(scroll, 250, -1);
    gtk_paned_pack1(GTK_PANED(paned), framed("Storage Devices", scroll), FALSE, FALSE);

    ui->st_attr = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(ui->st_attr), 10);
    GtkWidget *ascroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ascroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ascroll), ui->st_attr);
    gtk_paned_pack2(GTK_PANED(paned), ascroll, TRUE, FALSE);
    storage_fill(mg);
    storage_show(mg, selected_slot(ui));
    return paned;
}

/* ── Audio ── */

static void on_sound_enable(GtkToggleButton *b, gpointer data) {
    Manager *mg = data;
    SettingsUi *ui = mg->ui;
    bool on = gtk_toggle_button_get_active(b);
    gtk_widget_set_sensitive(ui->sound_combo, on);
    if (!on) set_str(&mg->edit->sound, "none");
    else {
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui->sound_combo));
        set_str(&mg->edit->sound, id && strcmp(id, "default") != 0 ? id : NULL);
    }
    edit_changed(mg);
}

static void on_sound(GtkComboBox *c, gpointer data) {
    Manager *mg = data;
    const char *id = gtk_combo_box_get_active_id(c);
    set_str(&mg->edit->sound, id && strcmp(id, "default") != 0 ? id : NULL);
    edit_changed(mg);
}

static char *sound_label(const char *id) {
    for (int i = 0; i < N_SOUNDHW; i++)
        if (strcmp(soundhws[i].name, id) == 0) return strip_status(soundhws[i].desc);
    /* "ay8910,msm6295": several chips */
    GString *s = g_string_new(NULL);
    char **v = g_strsplit(id, ",", -1);
    for (int i = 0; v[i]; i++) {
        char *one = NULL;
        for (int k = 0; k < N_SOUNDHW && !one; k++)
            if (strcmp(soundhws[k].name, v[i]) == 0) one = strip_status(soundhws[k].desc);
        g_string_append_printf(s, "%s%s", i ? " + " : "", one ? one : v[i]);
        g_free(one);
    }
    g_strfreev(v);
    return g_string_free(s, FALSE);
}

static GtkWidget *page_audio(Manager *mg) {
    Vm *vm = mg->edit;
    SettingsUi *ui = mg->ui;
    const MachineType *t = &types[vm->type];
    GtkWidget *g = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(g), 14);
    gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    if (!type_has_audio(t)) {
        form_row(g, NULL, note("This machine has no sound hardware GEMU can set up."));
        return g;
    }
    bool on = !(vm->sound && strcmp(vm->sound, "none") == 0);
    GtkWidget *en = gtk_check_button_new_with_mnemonic("_Enable Audio");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(en), on);
    form_row(g, NULL, en);
    ui->sound_combo = combo();
    bool has_default = t->soundhw[0] && strcmp(t->soundhw, "none") != 0;
    if (has_default) {
        char *d = sound_label(t->soundhw);
        char *lab = g_strdup_printf("%s (default)", d);
        combo_add(ui->sound_combo, "default", lab);
        g_free(lab);
        g_free(d);
    }
    const GemuDevDesc3 *opts[16];
    int n = type_sound(t, opts, 16);
    for (int i = 0; i < n; i++) {
        if (has_default && strcmp(opts[i]->name, t->soundhw) == 0) continue;
        char *d = strip_status(opts[i]->desc);
        combo_add(ui->sound_combo, opts[i]->name, d);
        g_free(d);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->sound_combo),
                                vm->sound && on ? vm->sound : "default");
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(ui->sound_combo)) < 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui->sound_combo), 0);
    gtk_widget_set_sensitive(ui->sound_combo, on);
    g_signal_connect(ui->sound_combo, "changed", G_CALLBACK(on_sound), mg);
    g_signal_connect(en, "toggled", G_CALLBACK(on_sound_enable), mg);
    GtkWidget *l = form_row(g, "Audio _Controller:", ui->sound_combo);
    gtk_widget_set_margin_start(l, 24);
    return g;
}

/* ── Network ── */

static void nic_sync(Manager *mg) {
    SettingsUi *ui = mg->ui;
    Vm *vm = mg->edit;
    gtk_widget_set_sensitive(ui->nic_box, !vm->nic_off);
    bool tap = vm->nic_attach && strcmp(vm->nic_attach, "tap") == 0;
    gtk_widget_set_visible(ui->nic_ifname, tap);
    gtk_widget_set_visible(ui->nic_ifname_label, tap);
    gtk_widget_set_sensitive(ui->nic_fwd, !tap);
}

static void on_nic_enable(GtkToggleButton *b, gpointer data) {
    Manager *mg = data;
    mg->edit->nic_off = !gtk_toggle_button_get_active(b);
    nic_sync(mg);
    edit_changed(mg);
}

static void on_nic_attach(GtkComboBox *c, gpointer data) {
    Manager *mg = data;
    const char *id = gtk_combo_box_get_active_id(c);
    set_str(&mg->edit->nic_attach, id && strcmp(id, "tap") == 0 ? "tap" : NULL);
    nic_sync(mg);
    edit_changed(mg);
}

static void on_nic_ifname(GtkEntry *e, gpointer data) {
    Manager *mg = data;
    char *v = g_strstrip(g_strdup(gtk_entry_get_text(e)));
    set_str(&mg->edit->nic_ifname, v);
    g_free(v);
    edit_changed(mg);
}

static void on_nic_model(GtkComboBox *c, gpointer data) {
    Manager *mg = data;
    const char *id = gtk_combo_box_get_active_id(c);
    set_str(&mg->edit->nic_model, id && strcmp(id, "default") != 0 ? id : NULL);
    edit_changed(mg);
}

static void on_nic_mac(GtkEntry *e, gpointer data) {
    Manager *mg = data;
    char *v = g_strstrip(g_ascii_strdown(gtk_entry_get_text(e), -1));
    set_str(&mg->edit->nic_mac, v);
    g_free(v);
    edit_changed(mg);
}

static void on_nic_mac_new(GtkButton *b, gpointer data) {
    (void)b;
    Manager *mg = data;
    /* QEMU's prefix, as gemu's own default addresses use */
    char *mac = g_strdup_printf("52:54:00:%02x:%02x:%02x", g_random_int_range(0, 256),
                                g_random_int_range(0, 256), g_random_int_range(0, 256));
    gtk_entry_set_text(GTK_ENTRY(mg->ui->nic_mac), mac);
    g_free(mac);
}

/* Port forwarding: a table of host -> guest rules, user mode (NAT) only. */
enum { PF_PROTO, PF_HOST_IP, PF_HOST_PORT, PF_GUEST_IP, PF_GUEST_PORT, PF_N };

static void on_pf_edited(GtkCellRendererText *r, char *path, char *text, gpointer data) {
    GtkListStore *store = data;
    GtkTreeIter it;
    int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "col"));
    if (gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &it, path))
        gtk_list_store_set(store, &it, col, g_strstrip(text), -1);
}

static void on_pf_add(GtkButton *b, gpointer data) {
    (void)b;
    GtkTreeView *tv = data;
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(tv));
    GtkTreeIter it;
    gtk_list_store_insert_with_values(store, &it, -1, PF_PROTO, "tcp", PF_HOST_IP, "",
                                      PF_HOST_PORT, "", PF_GUEST_IP, "", PF_GUEST_PORT, "", -1);
    GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &it);
    gtk_tree_view_set_cursor(tv, p, gtk_tree_view_get_column(tv, PF_HOST_PORT), TRUE);
    gtk_tree_path_free(p);
}

static void on_pf_remove(GtkButton *b, gpointer data) {
    (void)b;
    GtkTreeView *tv = data;
    GtkTreeModel *m;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(tv), &m, &it))
        gtk_list_store_remove(GTK_LIST_STORE(m), &it);
}

static bool port_ok(const char *s) {
    char *end;
    long v = strtol(s, &end, 10);
    return s[0] && !*end && v > 0 && v < 65536;
}

static void on_nic_forwarding(GtkButton *b, gpointer data) {
    (void)b;
    Manager *mg = data;
    Vm *vm = mg->edit;
    GtkWidget *d = gtk_dialog_new_with_buttons("Port Forwarding Rules", GTK_WINDOW(mg->ui->dialog),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 620, 320);
    GtkListStore *store = gtk_list_store_new(PF_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING);
    for (guint i = 0; i < vm->nic_fwd->len; i++) {
        /* [tcp|udp]:[hostaddr]:hostport-[guestaddr]:guestport */
        char *r = g_strdup(g_ptr_array_index(vm->nic_fwd, i));
        char *dash = strchr(r, '-');
        if (!dash) { g_free(r); continue; }
        *dash = '\0';
        char **h = g_strsplit(r, ":", 3), **gv = g_strsplit(dash + 1, ":", 2);
        if (g_strv_length(h) == 3 && g_strv_length(gv) == 2)
            gtk_list_store_insert_with_values(store, NULL, -1, PF_PROTO, h[0][0] ? h[0] : "tcp",
                                              PF_HOST_IP, h[1], PF_HOST_PORT, h[2],
                                              PF_GUEST_IP, gv[0], PF_GUEST_PORT, gv[1], -1);
        g_strfreev(h);
        g_strfreev(gv);
        g_free(r);
    }
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    static const char *titles[PF_N] = { "Protocol", "Host IP", "Host Port", "Guest IP", "Guest Port" };
    for (int c = 0; c < PF_N; c++) {
        GtkCellRenderer *r;
        if (c == PF_PROTO) {
            GtkListStore *protos = gtk_list_store_new(1, G_TYPE_STRING);
            gtk_list_store_insert_with_values(protos, NULL, -1, 0, "tcp", -1);
            gtk_list_store_insert_with_values(protos, NULL, -1, 0, "udp", -1);
            r = gtk_cell_renderer_combo_new();
            g_object_set(r, "model", protos, "text-column", 0, "has-entry", FALSE, NULL);
            g_object_unref(protos);
        } else {
            r = gtk_cell_renderer_text_new();
        }
        g_object_set(r, "editable", TRUE, NULL);
        g_object_set_data(G_OBJECT(r), "col", GINT_TO_POINTER(c));
        g_signal_connect(r, "edited", G_CALLBACK(on_pf_edited), store);
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(titles[c], r, "text", c, NULL);
        gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), tv);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *add = gtk_button_new_from_icon_name("list-add", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(add, "Add a rule");
    g_signal_connect(add, "clicked", G_CALLBACK(on_pf_add), tv);
    GtkWidget *rem = gtk_button_new_from_icon_name("list-remove", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(rem, "Remove the selected rule");
    g_signal_connect(rem, "clicked", G_CALLBACK(on_pf_remove), tv);
    gtk_container_add(GTK_CONTAINER(side), add);
    gtk_container_add(GTK_CONTAINER(side), rem);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(hb), 8);
    gtk_container_add(GTK_CONTAINER(hb), scroll);
    gtk_container_add(GTK_CONTAINER(hb), side);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_add(GTK_CONTAINER(content), hb);
    GtkWidget *hint = note("Connections to the host port reach the guest port. Leave the "
                           "host IP empty for all addresses and the guest IP empty for the "
                           "guest's DHCP address (10.0.2.15).");
    gtk_widget_set_margin_start(hint, 8);
    gtk_widget_set_margin_end(hint, 8);
    gtk_widget_set_margin_bottom(hint, 8);
    gtk_container_add(GTK_CONTAINER(content), hint);
    gtk_widget_show_all(d);

    while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        GPtrArray *rules = str_array();
        char *bad = NULL;
        GtkTreeIter it;
        GtkTreeModel *m = GTK_TREE_MODEL(store);
        for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok && !bad; ok = gtk_tree_model_iter_next(m, &it)) {
            char *v[PF_N];
            gtk_tree_model_get(m, &it, PF_PROTO, &v[0], PF_HOST_IP, &v[1], PF_HOST_PORT, &v[2],
                               PF_GUEST_IP, &v[3], PF_GUEST_PORT, &v[4], -1);
            if (!v[2][0] && !v[4][0] && !v[1][0] && !v[3][0]) { /* an empty row: skip */ }
            else if (!port_ok(v[2]) || !port_ok(v[4]))
                bad = g_strdup_printf("“%s” → “%s”: the host and guest ports must be numbers "
                                      "from 1 to 65535.", v[2], v[4]);
            else
                g_ptr_array_add(rules, g_strdup_printf("%s:%s:%s-%s:%s", v[0], v[1], v[2], v[3], v[4]));
            for (int c = 0; c < PF_N; c++) g_free(v[c]);
        }
        if (bad) {
            message(mg, GTK_WINDOW(d), GTK_MESSAGE_WARNING, "A rule isn't complete", bad);
            g_free(bad);
            g_ptr_array_free(rules, TRUE);
            continue;
        }
        g_ptr_array_free(vm->nic_fwd, TRUE);
        vm->nic_fwd = rules;
        break;
    }
    gtk_widget_destroy(d);
    edit_changed(mg);
}

static GtkWidget *page_network(Manager *mg) {
    Vm *vm = mg->edit;
    SettingsUi *ui = mg->ui;
    const MachineType *t = &types[vm->type];
    const GemuDevDesc3 *nics[8];
    int n = type_nics(t, nics, 8);
    if (!n) {
        GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_set_border_width(GTK_CONTAINER(b), 14);
        gtk_container_add(GTK_CONTAINER(b), note("This machine has no network adapter."));
        return b;
    }
    GtkWidget *nb = gtk_notebook_new();
    GtkWidget *g = tab(nb, "Adapter _1");
    GtkWidget *en = gtk_check_button_new_with_mnemonic("_Enable Network Adapter");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(en), !vm->nic_off);
    g_signal_connect(en, "toggled", G_CALLBACK(on_nic_enable), mg);
    form_row(g, NULL, en);

    ui->nic_box = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(ui->nic_box), 8);
    gtk_grid_set_column_spacing(GTK_GRID(ui->nic_box), 12);
    gtk_widget_set_margin_start(ui->nic_box, 24);
    form_row(g, NULL, ui->nic_box);
    GtkWidget *attach = combo();
    combo_add(attach, "user", "NAT");
#ifdef __linux__
    combo_add(attach, "tap", "TAP interface");
#endif
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(attach), vm->nic_attach ? vm->nic_attach : "user");
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(attach)) < 0) gtk_combo_box_set_active(GTK_COMBO_BOX(attach), 0);
    g_signal_connect(attach, "changed", G_CALLBACK(on_nic_attach), mg);
    form_row(ui->nic_box, "_Attached to:", attach);
    ui->nic_ifname = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->nic_ifname), "e.g. tap0");
    gtk_entry_set_text(GTK_ENTRY(ui->nic_ifname), vm->nic_ifname ? vm->nic_ifname : "");
    g_signal_connect(ui->nic_ifname, "changed", G_CALLBACK(on_nic_ifname), mg);
    ui->nic_ifname_label = form_row(ui->nic_box, "_Name:", ui->nic_ifname);

    GtkWidget *adv = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(adv), 8);
    gtk_grid_set_column_spacing(GTK_GRID(adv), 12);
    gtk_widget_set_margin_top(adv, 8);
    gtk_widget_set_margin_start(adv, 16);
    GtkWidget *model = combo();
    char *d0 = strip_status(nics[0]->desc);
    char *lab = g_strdup_printf("%s (default)", d0);
    combo_add(model, "default", lab);
    g_free(lab);
    g_free(d0);
    for (int i = 1; i < n; i++) {
        char *d = strip_status(nics[i]->desc);
        combo_add(model, nics[i]->name, d);
        g_free(d);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(model), vm->nic_model ? vm->nic_model : "default");
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(model)) < 0) gtk_combo_box_set_active(GTK_COMBO_BOX(model), 0);
    gtk_widget_set_sensitive(model, n > 1);
    g_signal_connect(model, "changed", G_CALLBACK(on_nic_model), mg);
    form_row(adv, "Adapter _Type:", model);
    GtkWidget *macrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ui->nic_mac = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->nic_mac), "Automatic");
    gtk_entry_set_width_chars(GTK_ENTRY(ui->nic_mac), 20);
    gtk_entry_set_max_length(GTK_ENTRY(ui->nic_mac), 17);
    gtk_entry_set_text(GTK_ENTRY(ui->nic_mac), vm->nic_mac ? vm->nic_mac : "");
    g_signal_connect(ui->nic_mac, "changed", G_CALLBACK(on_nic_mac), mg);
    gtk_container_add(GTK_CONTAINER(macrow), ui->nic_mac);
    GtkWidget *regen = gtk_button_new_from_icon_name("view-refresh", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(regen, "Generate a new MAC address");
    g_signal_connect(regen, "clicked", G_CALLBACK(on_nic_mac_new), mg);
    gtk_container_add(GTK_CONTAINER(macrow), regen);
    GtkWidget *l = form_row(adv, "_MAC Address:", macrow);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), ui->nic_mac);
    ui->nic_fwd = gtk_button_new_with_mnemonic("_Port Forwarding…");
    gtk_widget_set_halign(ui->nic_fwd, GTK_ALIGN_START);
    g_signal_connect(ui->nic_fwd, "clicked", G_CALLBACK(on_nic_forwarding), mg);
    form_row(adv, "", ui->nic_fwd);
    GtkWidget *exp = gtk_expander_new_with_mnemonic("A_dvanced");
    gtk_expander_set_expanded(GTK_EXPANDER(exp), TRUE);
    gtk_container_add(GTK_CONTAINER(exp), adv);
    form_row(ui->nic_box, NULL, exp);
    form_row(g, NULL, note("NAT gives the guest a private network (10.0.2.x) that reaches "
                           "the outside through the host. A TAP interface puts it on a host "
                           "network interface you've set up (ip tuntap add NAME mode tap user $USER)."));
    return nb;
}

/* ── Devices ── */

static void on_dev_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box;
    Manager *mg = data;
    bool added = row && g_object_get_data(G_OBJECT(row), "added");
    gtk_widget_set_sensitive(mg->ui->dev_remove, added);
}

static GtkWidget *device_row(const char *name, const char *sub) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(row), 5);
    gtk_container_add(GTK_CONTAINER(row), icon_image(device_icon(name), 32));
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    char *desc = device_desc(name);
    GtkWidget *t = gtk_label_new(NULL);
    char *mk = g_markup_printf_escaped("<b>%s</b>", desc);
    gtk_label_set_markup(GTK_LABEL(t), mk);
    g_free(mk);
    g_free(desc);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
    gtk_container_add(GTK_CONTAINER(v), t);
    GtkWidget *s = dim(gtk_label_new(sub));
    gtk_label_set_xalign(GTK_LABEL(s), 0);
    gtk_container_add(GTK_CONTAINER(v), s);
    gtk_container_add(GTK_CONTAINER(row), v);
    GtkWidget *r = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(r), row);
    g_object_set_data_full(G_OBJECT(r), "device", g_strdup(name), g_free);
    return r;
}

/* The devices that can be added: they fit the type, and aren't network
 * adapters (the Network page has those) or - unless they're input devices,
 * which can fill several ports - already attached. */
static int addable_devices(const Vm *vm, const GemuDevDesc3 **out, int max) {
    const MachineType *t = &types[vm->type];
    int n = 0;
    for (int i = 0; i < N_DEVICES && n < max; i++) {
        const GemuDevDesc3 *d = &devices[i];
        if (!fits(d->machines, t) || strcmp(d->name, "none") == 0 ||
            strcmp(device_category(d->name), "network") == 0 ||
            /* a second controller goes in the next port; other things come once */
            (str_array_has(vm->devices, d->name) && strcmp(device_category(d->name), "input") != 0))
            continue;
        out[n++] = d;
    }
    return n;
}

static void devices_fill(Manager *mg) {
    SettingsUi *ui = mg->ui;
    Vm *vm = mg->edit;
    const MachineType *t = &types[vm->type];
    clear_children(ui->dev_list);
    for (guint i = 0; i < vm->devices->len; i++) {
        const char *d = g_ptr_array_index(vm->devices, i);
        bool dflt = is_default_device(t, d);
        bool fixed = dflt && !family_device_none(t->family);
        char *sub = g_strdup_printf("%s  ·  %s", d, dflt ? "standard" : "added");
        GtkWidget *r = device_row(d, sub);
        g_free(sub);
        if (fixed) gtk_widget_set_tooltip_text(r, "Built into this machine; it can't be taken off.");
        else g_object_set_data(G_OBJECT(r), "added", GINT_TO_POINTER(1));     /* removable */
        gtk_container_add(GTK_CONTAINER(ui->dev_list), r);
    }
    gtk_widget_show_all(ui->dev_list);
    gtk_widget_set_sensitive(ui->dev_remove, FALSE);
    const GemuDevDesc3 *any[1];
    bool can_add = addable_devices(vm, any, 1) > 0;
    gtk_widget_set_sensitive(ui->dev_add, can_add);
    gtk_widget_set_tooltip_text(ui->dev_add, can_add ? NULL : "No other devices fit this machine.");
}

static void add_header(GtkListBoxRow *row, GtkListBoxRow *before, gpointer data) {
    (void)data;
    const char *cat = device_category(g_object_get_data(G_OBJECT(row), "device"));
    const char *prev = before ? device_category(g_object_get_data(G_OBJECT(before), "device")) : NULL;
    if (prev && strcmp(prev, cat) == 0) { gtk_list_box_row_set_header(row, NULL); return; }
    GtkWidget *h = gtk_label_new(NULL);
    char *mk = g_markup_printf_escaped("<b>%s</b>", category_title(cat));
    gtk_label_set_markup(GTK_LABEL(h), mk);
    g_free(mk);
    gtk_widget_set_halign(h, GTK_ALIGN_START);
    gtk_widget_set_margin_start(h, 6);
    gtk_widget_set_margin_top(h, before ? 10 : 4);
    gtk_widget_set_margin_bottom(h, 2);
    gtk_widget_show(h);
    gtk_list_box_row_set_header(row, h);
}

static int cat_rank(const char *name) {
    static const char *order[] = { "input", "display", "data", "other" };
    const char *c = device_category(name);
    for (int i = 0; i < 4; i++) if (strcmp(order[i], c) == 0) return i;
    return 4;
}

static int add_sort(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data) {
    (void)data;
    const char *na = g_object_get_data(G_OBJECT(a), "device"), *nb = g_object_get_data(G_OBJECT(b), "device");
    int d = cat_rank(na) - cat_rank(nb);
    return d ? d : strcmp(na, nb);
}

static void on_add_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box; (void)row;
    gtk_dialog_response(GTK_DIALOG(data), GTK_RESPONSE_OK);
}

static void on_dev_add(GtkButton *b, gpointer data) {
    (void)b;
    Manager *mg = data;
    Vm *vm = mg->edit;
    const MachineType *t = &types[vm->type];
    const GemuDevDesc3 *cand[64];
    int n = addable_devices(vm, cand, 64);
    if (!n) return;
    GtkWidget *d = gtk_dialog_new_with_buttons("Add Device", GTK_WINDOW(mg->ui->dialog),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_Add", GTK_RESPONSE_OK, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 460, 420);
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), FALSE);
    gtk_list_box_set_header_func(GTK_LIST_BOX(list), add_header, NULL, NULL);
    gtk_list_box_set_sort_func(GTK_LIST_BOX(list), add_sort, NULL, NULL);
    for (int i = 0; i < n; i++) {
        bool dflt = is_default_device(t, cand[i]->name);
        bool again = str_array_has(vm->devices, cand[i]->name);
        char *sub = g_strdup_printf("%s%s", cand[i]->name, again ? "  ·  adds another"
                                                          : dflt ? "  ·  standard on this machine" : "");
        gtk_container_add(GTK_CONTAINER(list), device_row(cand[i]->name, sub));
        g_free(sub);
    }
    g_signal_connect(list, "row-activated", G_CALLBACK(on_add_activated), d);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(scroll), 8);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))), scroll);
    gtk_widget_show_all(d);
    gtk_list_box_select_row(GTK_LIST_BOX(list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
        if (r) {
            g_ptr_array_add(vm->devices, g_strdup(g_object_get_data(G_OBJECT(r), "device")));
            devices_fill(mg);
            if (mg->ui->st_store) storage_fill(mg);
            edit_changed(mg);
        }
    }
    gtk_widget_destroy(d);
}

static void on_dev_remove(GtkButton *b, gpointer data) {
    (void)b;
    Manager *mg = data;
    GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(mg->ui->dev_list));
    if (!r || !g_object_get_data(G_OBJECT(r), "added")) return;
    const char *name = g_object_get_data(G_OBJECT(r), "device");
    GPtrArray *a = mg->edit->devices;
    for (guint i = 0; i < a->len; i++)
        if (strcmp(g_ptr_array_index(a, i), name) == 0) { g_ptr_array_remove_index(a, i); break; }
    devices_fill(mg);
    if (mg->ui->st_store) storage_fill(mg);
    edit_changed(mg);
}

static GtkWidget *page_devices(Manager *mg) {
    SettingsUi *ui = mg->ui;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    ui->dev_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui->dev_list), GTK_SELECTION_SINGLE);
    GtkWidget *ph = note("No devices attached.");
    gtk_widget_set_margin_top(ph, 12);
    gtk_label_set_xalign(GTK_LABEL(ph), 0.5f);
    gtk_widget_show(ph);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(ui->dev_list), ph);
    g_signal_connect(ui->dev_list, "row-selected", G_CALLBACK(on_dev_selected), mg);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), ui->dev_list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(box), framed("Attached Devices", scroll));
    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ui->dev_add = gtk_button_new_with_mnemonic("_Add Device…");
    gtk_button_set_image(GTK_BUTTON(ui->dev_add), gtk_image_new_from_icon_name("list-add", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(ui->dev_add), TRUE);
    g_signal_connect(ui->dev_add, "clicked", G_CALLBACK(on_dev_add), mg);
    ui->dev_remove = gtk_button_new_with_mnemonic("_Remove Device");
    gtk_button_set_image(GTK_BUTTON(ui->dev_remove), gtk_image_new_from_icon_name("list-remove", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(ui->dev_remove), TRUE);
    g_signal_connect(ui->dev_remove, "clicked", G_CALLBACK(on_dev_remove), mg);
    gtk_container_add(GTK_CONTAINER(bb), ui->dev_add);
    gtk_container_add(GTK_CONTAINER(bb), ui->dev_remove);
    gtk_container_add(GTK_CONTAINER(box), bb);
    devices_fill(mg);
    return box;
}

/* ── The dialog ── */

static void on_nav(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box;
    Manager *mg = data;
    if (!row) return;
    const char *page = g_object_get_data(G_OBJECT(row), "page");
    gtk_stack_set_visible_child_name(GTK_STACK(mg->ui->stack), page);
    char *mk = g_markup_printf_escaped("<span size='large' weight='bold'>%s</span>", page);
    gtk_label_set_markup(GTK_LABEL(mg->ui->title), mk);
    g_free(mk);
    ini_set("manager", "settings_page", page);
}

static void open_settings(Manager *mg) {
    if (!mg->sel || mg->ui) return;
    Vm *vm = mg->sel;
    SettingsUi ui = { 0 };
    mg->ui = &ui;
    mg->edit = vm_clone(vm);

    char *title = g_strdup_printf("%s - Settings", vm->name);
    ui.dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(mg->window),
                                            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                            "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
    g_free(title);
    gtk_dialog_set_default_response(GTK_DIALOG(ui.dialog), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(ui.dialog), 900, 600);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(ui.dialog));

    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(hb, TRUE);
    GtkWidget *nav = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(nav), GTK_SELECTION_BROWSE);
    GtkWidget *nscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(nscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(nscroll), nav);
    gtk_widget_set_size_request(nscroll, 180, -1);
    gtk_container_add(GTK_CONTAINER(hb), nscroll);
    gtk_container_add(GTK_CONTAINER(hb), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(right), 10);
    gtk_widget_set_hexpand(right, TRUE);
    ui.title = gtk_label_new(NULL);
    gtk_widget_set_halign(ui.title, GTK_ALIGN_START);
    gtk_container_add(GTK_CONTAINER(right), ui.title);
    ui.stack = gtk_stack_new();
    gtk_widget_set_vexpand(ui.stack, TRUE);
    gtk_container_add(GTK_CONTAINER(right), ui.stack);
    gtk_container_add(GTK_CONTAINER(hb), right);
    gtk_container_add(GTK_CONTAINER(content), hb);

    ui.warn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(ui.warn_box), 8);
    gtk_container_add(GTK_CONTAINER(ui.warn_box),
                      gtk_image_new_from_icon_name("dialog-warning", GTK_ICON_SIZE_LARGE_TOOLBAR));
    ui.warn = wrap_label();
    gtk_label_set_max_width_chars(GTK_LABEL(ui.warn), 100);
    gtk_container_add(GTK_CONTAINER(ui.warn_box), ui.warn);
    gtk_container_add(GTK_CONTAINER(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_container_add(GTK_CONTAINER(content), ui.warn_box);

    ui.syncing = true;                       /* building: the widgets' first values aren't changes */
    struct { const char *page, *icon; GtkWidget *(*build)(Manager *); } pages[] = {
        { "General", "computer",                                      page_general },
        { "System",  "preferences-system,applications-system",        page_system },
        { "Storage", "drive-harddisk",                                page_storage },
        { "Audio",   "audio-card,audio-speakers,audio-volume-high",   page_audio },
        { "Network", "network-wired,network-workgroup",               page_network },
        { "Devices", "input-gaming,input-keyboard",                   page_devices },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(pages); i++) {
        gtk_stack_add_named(GTK_STACK(ui.stack), pages[i].build(mg), pages[i].page);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_container_set_border_width(GTK_CONTAINER(row), 6);
        gtk_container_add(GTK_CONTAINER(row), icon_image(pages[i].icon, 24));
        GtkWidget *l = gtk_label_new(pages[i].page);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_container_add(GTK_CONTAINER(row), l);
        GtkWidget *r = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(r), row);
        g_object_set_data(G_OBJECT(r), "page", (gpointer)pages[i].page);
        gtk_container_add(GTK_CONTAINER(nav), r);
    }
    ui.syncing = false;
    g_signal_connect(nav, "row-selected", G_CALLBACK(on_nav), mg);
    gtk_widget_show_all(ui.dialog);
    if (ui.nic_box) nic_sync(mg);
    edit_changed(mg);

    char *last = ini_get("manager", "settings_page");
    GtkListBoxRow *pick = gtk_list_box_get_row_at_index(GTK_LIST_BOX(nav), 0);
    for (int i = 0; last; i++) {
        GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(nav), i);
        if (!r) break;
        if (strcmp(g_object_get_data(G_OBJECT(r), "page"), last) == 0) { pick = r; break; }
    }
    g_free(last);
    gtk_list_box_select_row(GTK_LIST_BOX(nav), pick);

    int resp = gtk_dialog_run(GTK_DIALOG(ui.dialog));
    gtk_widget_destroy(ui.dialog);
    mg->ui = NULL;
    if (resp == GTK_RESPONSE_OK) {
        vm_apply(vm, mg->edit);
        vm_save(vm);
        update_row(vm);
        gtk_list_box_invalidate_sort(GTK_LIST_BOX(mg->list));
    } else {
        vm_free(mg->edit);
    }
    mg->edit = NULL;
    if (mg->quit_pending) { gtk_widget_destroy(mg->window); return; }
    show_details(mg);
    update_buttons(mg);
}

static void on_settings(GtkWidget *w, gpointer data) { (void)w; open_settings(data); }

/* ── New machine ─────────────────────────────────────────────────────────── */

enum { COL_NAME, COL_DESC, COL_STATUS, COL_INDEX, COL_SEARCH, N_COLS };   /* COL_INDEX -1 = family row */

typedef struct {
    Manager      *mg;
    GtkWidget    *dialog, *search, *tree, *name, *title, *sub, *desc, *create;
    GtkTreeModel *filter;
    int           type;                      /* types[] index, -1 = none */
    bool          name_edited, setting_name;
} Picker;

static void picker_update(Picker *p) {
    const char *n = gtk_entry_get_text(GTK_ENTRY(p->name));
    gtk_widget_set_sensitive(p->create, p->type >= 0 && n[strspn(n, " ")]);
}

static void picker_show(Picker *p, int type) {
    p->type = type;
    if (type < 0) {
        gtk_label_set_text(GTK_LABEL(p->title), "Choose a machine type");
        gtk_label_set_text(GTK_LABEL(p->sub), "");
        gtk_label_set_text(GTK_LABEL(p->desc), "");
    } else {
        const MachineType *t = &types[type];
        gtk_label_set_text(GTK_LABEL(p->title), t->desc);
        char *sub = type_subtitle(t);
        gtk_label_set_text(GTK_LABEL(p->sub), sub);
        g_free(sub);
        gtk_label_set_text(GTK_LABEL(p->desc), t->long_desc);
        if (!p->name_edited) {                      /* suggest a name until the user types one */
            char *base = short_name(t->desc);
            char *name = unique_name(p->mg, base);
            g_free(base);
            p->setting_name = true;
            gtk_entry_set_text(GTK_ENTRY(p->name), name);
            p->setting_name = false;
            g_free(name);
        }
    }
    picker_update(p);
}

static void on_picker_name(GtkEntry *e, gpointer data) {
    (void)e;
    Picker *p = data;
    if (!p->setting_name) p->name_edited = true;
    picker_update(p);
}

static void on_picker_selection(GtkTreeSelection *sel, gpointer data) {
    Picker *p = data;
    GtkTreeModel *model;
    GtkTreeIter it;
    int index = -1;
    if (gtk_tree_selection_get_selected(sel, &model, &it))
        gtk_tree_model_get(model, &it, COL_INDEX, &index, -1);
    picker_show(p, index);
}

static void on_picker_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data) {
    (void)col;
    Picker *p = data;
    GtkTreeIter it;
    int index = -1;
    if (!gtk_tree_model_get_iter(gtk_tree_view_get_model(tv), &it, path)) return;
    gtk_tree_model_get(gtk_tree_view_get_model(tv), &it, COL_INDEX, &index, -1);
    if (index < 0) {                               /* family row: fold/unfold */
        if (gtk_tree_view_row_expanded(tv, path)) gtk_tree_view_collapse_row(tv, path);
        else gtk_tree_view_expand_row(tv, path, FALSE);
    } else if (gtk_widget_get_sensitive(p->create)) {
        gtk_dialog_response(GTK_DIALOG(p->dialog), GTK_RESPONSE_ACCEPT);
    }
}

static gboolean row_visible(GtkTreeModel *model, GtkTreeIter *it, gpointer data) {
    Picker *p = data;
    const char *q = gtk_entry_get_text(GTK_ENTRY(p->search));
    if (!q[0]) return TRUE;
    char *needle = g_utf8_casefold(q, -1);
    int index;
    gtk_tree_model_get(model, it, COL_INDEX, &index, -1);
    gboolean vis = FALSE;
    GtkTreeIter child;
    if (index >= 0) {
        char *hay;
        gtk_tree_model_get(model, it, COL_SEARCH, &hay, -1);
        vis = hay && strstr(hay, needle);
        g_free(hay);
    } else {                                       /* a family shows if any type in it does */
        for (gboolean ok = gtk_tree_model_iter_children(model, &child, it); ok && !vis;
             ok = gtk_tree_model_iter_next(model, &child)) {
            char *hay;
            gtk_tree_model_get(model, &child, COL_SEARCH, &hay, -1);
            vis = hay && strstr(hay, needle);
            g_free(hay);
        }
    }
    g_free(needle);
    return vis;
}

/* Select the type called name (NULL = the first one shown); true if found. */
static bool picker_select(Picker *p, const char *name) {
    GtkTreeModel *model = p->filter;
    GtkTreeIter fam, it;
    for (gboolean f = gtk_tree_model_get_iter_first(model, &fam); f; f = gtk_tree_model_iter_next(model, &fam))
        for (gboolean c = gtk_tree_model_iter_children(model, &it, &fam); c; c = gtk_tree_model_iter_next(model, &it)) {
            int index;
            gtk_tree_model_get(model, &it, COL_INDEX, &index, -1);
            if (index >= 0 && (!name || strcmp(types[index].name, name) == 0)) {
                GtkTreePath *path = gtk_tree_model_get_path(model, &it);
                gtk_tree_view_expand_to_path(GTK_TREE_VIEW(p->tree), path);
                gtk_tree_view_set_cursor(GTK_TREE_VIEW(p->tree), path, NULL, FALSE);
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(p->tree), path, NULL, TRUE, 0.3f, 0);
                gtk_tree_path_free(path);
                return true;
            }
        }
    return false;
}

static void on_picker_search(GtkSearchEntry *e, gpointer data) {
    (void)e;
    Picker *p = data;
    const char *keep = p->type >= 0 ? types[p->type].name : NULL;
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(p->filter));
    if (gtk_entry_get_text(GTK_ENTRY(p->search))[0])
        gtk_tree_view_expand_all(GTK_TREE_VIEW(p->tree));
    if (!(keep && picker_select(p, keep)) && !picker_select(p, NULL))
        picker_show(p, -1);
}

/* Enter can come before search-changed, which GtkSearchEntry delays while
 * typing: filter now, or Enter would take the type selected before. */
static void on_picker_search_activate(GtkEntry *e, gpointer data) {
    Picker *p = data;
    on_picker_search(GTK_SEARCH_ENTRY(e), p);
    if (gtk_widget_get_sensitive(p->create))
        gtk_dialog_response(GTK_DIALOG(p->dialog), GTK_RESPONSE_ACCEPT);
}

static void status_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *model,
                        GtkTreeIter *it, gpointer data) {
    (void)col; (void)data;
    char *s;
    gtk_tree_model_get(model, it, COL_STATUS, &s, -1);
    const char *color = s && s[0] ? status_color(s) : NULL;
    g_object_set(cell, "text", s, "foreground", color, "foreground-set", color != NULL, NULL);
    g_free(s);
}

static void name_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *model,
                      GtkTreeIter *it, gpointer data) {
    (void)col; (void)data;
    char *s;
    int index;
    gtk_tree_model_get(model, it, COL_NAME, &s, COL_INDEX, &index, -1);
    g_object_set(cell, "text", s, "weight", index < 0 ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, NULL);
    g_free(s);
}

static GtkTreeStore *type_store(void) {
    GtkTreeStore *store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_INT, G_TYPE_STRING);
    for (int f = 0; f < N_FAMILIES; f++) {
        GtkTreeIter fam;
        bool have = false;
        for (int i = 0; i < N_TYPES; i++) {
            const MachineType *t = &types[i];
            if (strcmp(t->family, families[f].family) != 0) continue;
            if (!have) {
                gtk_tree_store_insert_with_values(store, &fam, NULL, -1,
                    COL_NAME, families[f].label, COL_DESC, "", COL_STATUS, "",
                    COL_INDEX, -1, COL_SEARCH, "", -1);
                have = true;
            }
            char *raw = g_strdup_printf("%s %s %s %s", t->name, t->desc, t->status, families[f].label);
            char *search = g_utf8_casefold(raw, -1);
            GtkTreeIter it;
            gtk_tree_store_insert_with_values(store, &it, &fam, -1,
                COL_NAME, t->name, COL_DESC, t->desc, COL_STATUS, t->status,
                COL_INDEX, i, COL_SEARCH, search, -1);
            g_free(search);
            g_free(raw);
        }
    }
    return store;
}

static GtkWidget *type_tree(Picker *p) {
    GtkTreeStore *store = type_store();
    p->filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL);
    g_object_unref(store);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(p->filter), row_visible, p, NULL);
    p->tree = gtk_tree_view_new_with_model(p->filter);
    g_object_unref(p->filter);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(p->tree), FALSE);   /* the search box does that */

    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, "Type");
    gtk_tree_view_column_pack_start(c, r, TRUE);
    gtk_tree_view_column_set_cell_data_func(c, r, name_cell, NULL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->tree), c);

    r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    c = gtk_tree_view_column_new_with_attributes("Description", r, "text", COL_DESC, NULL);
    gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->tree), c);

    r = gtk_cell_renderer_text_new();
    c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, "Status");
    gtk_tree_view_column_pack_start(c, r, TRUE);
    gtk_tree_view_column_set_cell_data_func(c, r, status_cell, NULL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->tree), c);

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->tree)), "changed",
                     G_CALLBACK(on_picker_selection), p);
    g_signal_connect(p->tree, "row-activated", G_CALLBACK(on_picker_activated), p);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), p->tree);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    return scroll;
}

static void new_machine(Manager *mg) {
    Picker p = { .mg = mg, .type = -1 };
    p.dialog = gtk_dialog_new_with_buttons("New Machine", GTK_WINDOW(mg->window),
                                           GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT |
                                           GTK_DIALOG_USE_HEADER_BAR,
                                           "_Cancel", GTK_RESPONSE_CANCEL, NULL);
    p.create = gtk_dialog_add_button(GTK_DIALOG(p.dialog), "C_reate", GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(p.create), "suggested-action");
    gtk_window_set_default_size(GTK_WINDOW(p.dialog), 900, 560);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(p.dialog));
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(left), 8);
    p.search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p.search), "Search machine types");
    g_signal_connect(p.search, "search-changed", G_CALLBACK(on_picker_search), &p);
    g_signal_connect(p.search, "activate", G_CALLBACK(on_picker_search_activate), &p);
    gtk_container_add(GTK_CONTAINER(left), p.search);
    gtk_container_add(GTK_CONTAINER(left), type_tree(&p));
    gtk_paned_pack1(GTK_PANED(paned), left, TRUE, FALSE);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(right), 12);
    gtk_widget_set_size_request(right, 300, -1);
    p.title = wrap_label();
    PangoAttrList *big = pango_attr_list_new();
    pango_attr_list_insert(big, pango_attr_scale_new(1.2));
    pango_attr_list_insert(big, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(p.title), big);
    pango_attr_list_unref(big);
    gtk_container_add(GTK_CONTAINER(right), p.title);
    p.sub = dim(wrap_label());
    gtk_container_add(GTK_CONTAINER(right), p.sub);
    p.desc = wrap_label();
    gtk_widget_set_margin_top(p.desc, 6);
    gtk_widget_set_valign(p.desc, GTK_ALIGN_START);
    GtkWidget *dscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(dscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(dscroll), p.desc);
    gtk_widget_set_vexpand(dscroll, TRUE);
    gtk_container_add(GTK_CONTAINER(right), dscroll);
    gtk_paned_pack2(GTK_PANED(paned), right, FALSE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 560);
    gtk_container_add(GTK_CONTAINER(content), paned);

    GtkWidget *nb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(nb), 8);
    gtk_container_add(GTK_CONTAINER(nb), gtk_label_new_with_mnemonic("_Name"));
    p.name = gtk_entry_new();
    gtk_widget_set_hexpand(p.name, TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(p.name), TRUE);
    g_signal_connect(p.name, "changed", G_CALLBACK(on_picker_name), &p);
    gtk_container_add(GTK_CONTAINER(nb), p.name);
    gtk_container_add(GTK_CONTAINER(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_container_add(GTK_CONTAINER(content), nb);
    gtk_dialog_set_default_response(GTK_DIALOG(p.dialog), GTK_RESPONSE_ACCEPT);

    gtk_widget_show_all(p.dialog);
    picker_show(&p, -1);
    char *last = ini_get("manager", "last_type");
    if (!(last && picker_select(&p, last))) picker_select(&p, NULL);
    g_free(last);
    gtk_widget_grab_focus(p.search);

    int resp = gtk_dialog_run(GTK_DIALOG(p.dialog));
    if (resp == GTK_RESPONSE_ACCEPT && p.type >= 0) {
        const MachineType *t = &types[p.type];
        char *id = new_vm_id(mg, t->name);
        char *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(p.name))));
        Vm *vm = vm_new(id, name, p.type);
        vm_attach_defaults(vm);
        vm->rom_dir = find_rom_dir(mg, t);
        vm_save(vm);
        g_ptr_array_add(mg->vms, vm);
        gtk_container_add(GTK_CONTAINER(mg->list), vm_row(vm));
        ini_set("manager", "last_type", t->name);
        g_free(name);
        g_free(id);
        gtk_widget_destroy(p.dialog);
        select_vm(mg, vm);
        return;
    }
    gtk_widget_destroy(p.dialog);
}

static void on_new(GtkButton *b, gpointer data) { (void)b; new_machine(data); }
static void on_new_item(GtkWidget *w, gpointer data) { (void)w; new_machine(data); }

/* ── Delete ──────────────────────────────────────────────────────────────── */

static void on_delete_item(GtkWidget *w, gpointer data) {
    (void)w;
    Manager *mg = data;
    Vm *vm = mg->sel;
    if (!vm || vm->running) return;
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(mg->window), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                          "Delete “%s”?", vm->name);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
        "Only its settings are removed. ROMs, disk images and other files it uses are left alone.");
    gtk_dialog_add_button(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL);
    GtkWidget *del = gtk_dialog_add_button(GTK_DIALOG(d), "_Delete", GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(del), "destructive-action");
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    int resp = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (resp != GTK_RESPONSE_ACCEPT) return;

    char *path = vm_path(vm);
    if (g_remove(path) != 0 && g_file_test(path, G_FILE_TEST_EXISTS)) {
        message(mg, NULL, GTK_MESSAGE_ERROR, "Could not delete the machine", path);
        g_free(path);
        return;
    }
    g_free(path);
    GtkWidget *row = vm->row;
    mg->sel = NULL;
    gtk_widget_destroy(row);                       /* before the Vm it points at goes */
    g_ptr_array_remove(mg->vms, vm);               /* frees it */
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(mg->list), 0);
    if (first) gtk_list_box_select_row(GTK_LIST_BOX(mg->list), first);
    else { mg->sel = NULL; show_details(mg); update_buttons(mg); }
}

/* ── Start ───────────────────────────────────────────────────────────────── */

/* This executable, so the machine runs the same build as the manager. */
static char *self_exe(const char *prog) {
#ifdef _WIN32
    wchar_t w[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, w, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        char *u = g_utf16_to_utf8((const gunichar2 *)w, (glong)n, NULL, NULL, NULL);
        if (u) return u;
    }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return g_strdup(buf);
#else
    /* rebuilt while the manager ran: argv[0] finds the new binary */
    char *link = g_file_read_link("/proc/self/exe", NULL);
    if (link && !g_str_has_suffix(link, " (deleted)")) return link;
    g_free(link);
#endif
    return g_strdup(prog);
}

/* A machine started or stopped. */
static void update_running(Manager *mg, Vm *vm) {
    update_row(vm);
    if (mg->sel == vm) {
        update_buttons(mg);
        if (mg->preview) gtk_widget_queue_draw(mg->preview);
    }
}

/* Pass the machine's stderr through and keep its tail for the error dialog. */
static gboolean on_child_stderr(GIOChannel *ch, GIOCondition cond, gpointer data) {
    Child *c = data;
    if (cond & G_IO_IN) {
        char buf[4096];
        gsize n = 0;
        GIOStatus st = g_io_channel_read_chars(ch, buf, sizeof buf, &n, NULL);
        if (n) {
            fwrite(buf, 1, n, stderr);
            fflush(stderr);
            g_string_append_len(c->err_text, buf, (gssize)n);
            if (c->err_text->len > ERR_KEEP)
                g_string_erase(c->err_text, 0, (gssize)(c->err_text->len - ERR_KEEP));
        }
        if (st == G_IO_STATUS_NORMAL || st == G_IO_STATUS_AGAIN) return TRUE;
    }
    c->err_watch = 0;
    return FALSE;                                   /* EOF or error */
}

/* Frames from the machine's preview pipe (see gemu/screendump.h): keep the
 * newest as its screenshot. */
static gboolean on_child_preview(GIOChannel *ch, GIOCondition cond, gpointer data) {
    Child *c = data;
    if (cond & G_IO_IN) {
        guint8 buf[65536];
        gsize n = 0;
        GIOStatus st = g_io_channel_read_chars(ch, (gchar *)buf, sizeof buf, &n, NULL);
        if (n) g_byte_array_append(c->prv_buf, buf, (guint)n);
        GByteArray *b = c->prv_buf;
        while (b->len >= 16) {
            if (memcmp(b->data, GEMU_PREVIEW_MAGIC, 8) != 0) {     /* out of step: give up */
                g_byte_array_set_size(b, 0);
                c->prv_watch = 0;
                return FALSE;
            }
            guint32 w = GUINT32_FROM_LE(*(guint32 *)(b->data + 8)), h = GUINT32_FROM_LE(*(guint32 *)(b->data + 12));
            if (!w || !h || w > 4096 || h > 4096) { c->prv_watch = 0; return FALSE; }
            gsize len = 16 + (gsize)w * h * 3;
            if (b->len < len) break;
            GBytes *px = g_bytes_new(b->data + 16, len - 16);
            GdkPixbuf *pb = gdk_pixbuf_new_from_bytes(px, GDK_COLORSPACE_RGB, FALSE, 8,
                                                      (int)w, (int)h, (int)w * 3);
            g_bytes_unref(px);
            g_byte_array_remove_range(b, 0, (guint)len);
            if (c->vm->shot) g_object_unref(c->vm->shot);
            c->vm->shot = pb;
            if (c->mg->sel == c->vm && c->mg->preview) gtk_widget_queue_draw(c->mg->preview);
        }
        if (st == G_IO_STATUS_NORMAL || st == G_IO_STATUS_AGAIN) return TRUE;
    }
    c->prv_watch = 0;
    return FALSE;
}

static void on_child_exit(GPid pid, gint wait_status, gpointer data) {
    Child *c = data;
    Manager *mg = c->mg;
    if (c->prv) {
        if (c->prv_watch) g_source_remove(c->prv_watch);
        g_io_channel_shutdown(c->prv, FALSE, NULL);
        g_io_channel_unref(c->prv);
        g_byte_array_free(c->prv_buf, TRUE);
    }
    if (c->vm->shot) { g_object_unref(c->vm->shot); c->vm->shot = NULL; }
    while (c->err_watch && on_child_stderr(c->err, G_IO_IN, c)) {}   /* whatever is left */
    if (c->err_watch) g_source_remove(c->err_watch);
    g_io_channel_shutdown(c->err, FALSE, NULL);
    g_io_channel_unref(c->err);
    g_spawn_close_pid(pid);

    mg->running--;
    c->vm->running = false;
    if (!mg->closing) {
        update_running(mg, c->vm);
        GError *e = NULL;
        if (!g_spawn_check_wait_status(wait_status, &e)) {
            char *tail = g_strstrip(g_strdup(c->err_text->str));
            char *title = g_strdup_printf("%s stopped with an error", c->vm->name);
            message(mg, NULL, GTK_MESSAGE_ERROR, title, tail[0] ? tail : e->message);
            g_free(title);
            g_free(tail);
            g_error_free(e);
        }
    } else if (!mg->running) {
        gtk_main_quit();
    }
    g_string_free(c->err_text, TRUE);
    g_free(c);
}

static void start(Manager *mg) {
    Vm *vm = mg->sel;
    char *err = NULL;
    if (!vm || vm->running || !build_argv(mg, vm, mg->argv, &err)) { g_free(err); return; }

    /* run it with this executable, not whatever argv[0] would find */
    char **argv = g_new0(char *, mg->argv->len + 1);
    argv[0] = self_exe(mg->prog);
    for (guint i = 1; i < mg->argv->len; i++) argv[i] = g_ptr_array_index(mg->argv, i);

    GPid pid;
    gint err_fd = -1, prv_fd = -1;
    GError *e = NULL;
    /* stdin and stdout stay the manager's, so -monitor stdio works from a terminal */
    GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CHILD_INHERITS_STDIN;
    gboolean ok;
#ifndef _WIN32
    /* the screen for the preview comes back on fd 3 (GEMU_PREVIEW_FD) */
    int prv[2];
    if (g_unix_open_pipe(prv, FD_CLOEXEC, NULL)) {
        char **envp = g_environ_setenv(g_get_environ(), "GEMU_PREVIEW_FD", "3", TRUE);
        gint src[1] = { prv[1] }, dst[1] = { 3 };
        ok = g_spawn_async_with_pipes_and_fds(NULL, (const char *const *)argv, (const char *const *)envp,
                                              flags, NULL, NULL, -1, -1, -1, src, dst, 1,
                                              &pid, NULL, NULL, &err_fd, &e);
        g_strfreev(envp);
        close(prv[1]);
        if (ok) prv_fd = prv[0];
        else close(prv[0]);
    } else
#endif
    ok = g_spawn_async_with_pipes(NULL, argv, NULL, flags, NULL, NULL, &pid, NULL, NULL, &err_fd, &e);
    g_free(argv[0]);
    g_free(argv);
    if (!ok) {
        char *title = g_strdup_printf("Could not start %s", vm->name);
        message(mg, NULL, GTK_MESSAGE_ERROR, title, e->message);
        g_free(title);
        g_error_free(e);
        return;
    }

    Child *c = g_new0(Child, 1);
    c->mg = mg;
    c->vm = vm;
    c->pid = pid;
    c->err_text = g_string_new(NULL);
#ifdef _WIN32
    c->err = g_io_channel_win32_new_fd(err_fd);
#else
    c->err = g_io_channel_unix_new(err_fd);
#endif
    g_io_channel_set_encoding(c->err, NULL, NULL);
    g_io_channel_set_close_on_unref(c->err, TRUE);
    c->err_watch = g_io_add_watch(c->err, G_IO_IN | G_IO_HUP | G_IO_ERR, on_child_stderr, c);
#ifndef _WIN32
    if (prv_fd >= 0) {
        c->prv = g_io_channel_unix_new(prv_fd);
        g_io_channel_set_encoding(c->prv, NULL, NULL);
        g_io_channel_set_buffered(c->prv, FALSE);
        g_io_channel_set_close_on_unref(c->prv, TRUE);
        c->prv_buf = g_byte_array_new();
        c->prv_watch = g_io_add_watch(c->prv, G_IO_IN | G_IO_HUP | G_IO_ERR, on_child_preview, c);
    }
#endif
    g_child_watch_add(pid, on_child_exit, c);
    vm->running = true;
    mg->running++;
    update_running(mg, vm);
}

static void on_start_item(GtkWidget *w, gpointer data) { (void)w; start(data); }

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box; (void)row;
    start(data);
}

/* ── Details ─────────────────────────────────────────────────────────────── *
 * The right pane: what the selected machine is and how it is set up, in
 * sections of "key: value" rows.  Read-only; Settings changes it. */

/* A section: icon + bold title, then a grid for detail_row(). */
static GtkWidget *detail_section(GtkWidget *parent, const char *icon, const char *title) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_add(GTK_CONTAINER(head), icon_image(icon, 16));
    GtkWidget *l = gtk_label_new(NULL);
    char *mk = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(l), mk);
    g_free(mk);
    gtk_container_add(GTK_CONTAINER(head), l);
    gtk_container_add(GTK_CONTAINER(box), head);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_widget_set_margin_start(grid, 22);
    gtk_container_add(GTK_CONTAINER(box), grid);
    gtk_container_add(GTK_CONTAINER(parent), box);
    gtk_container_add(GTK_CONTAINER(parent), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    return grid;
}

static GtkWidget *detail_row(GtkWidget *grid, const char *key, const char *value) {
    int row = next_row(grid);
    GtkWidget *v = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(v), 0);
    gtk_label_set_yalign(GTK_LABEL(v), 0);
    gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_widget_set_can_focus(v, FALSE);
    gtk_widget_set_hexpand(v, TRUE);
    if (key) {
        char *k = g_str_has_suffix(key, ":") ? g_strdup(key) : g_strconcat(key, ":", NULL);
        GtkWidget *kl = gtk_label_new(k);
        g_free(k);
        gtk_label_set_xalign(GTK_LABEL(kl), 0);
        gtk_label_set_yalign(GTK_LABEL(kl), 0);
        gtk_grid_attach(GTK_GRID(grid), kl, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
    } else {
        gtk_grid_attach(GTK_GRID(grid), v, 0, row, 2, 1);
    }
    return v;
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* A monitor showing the machine's name (and whether it is running). */
static gboolean draw_preview(GtkWidget *w, cairo_t *cr, gpointer data) {
    Manager *mg = data;
    Vm *vm = mg->sel;
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double fw = MIN(W - 8, 240), fh = fw * 0.62, x = (W - fw) / 2, y = (H - fh) / 2;
    rounded_rect(cr, x, y, fw, fh, 6);                          /* bezel */
    cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    double sx = x + 9, sy = y + 9, sw = fw - 18, sh = fh - 18;  /* screen */
    cairo_pattern_t *g = cairo_pattern_create_linear(sx, sy, sx + sw, sy + sh);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.16, 0.16, 0.18);
    cairo_pattern_add_color_stop_rgb(g, 0.45, 0.02, 0.02, 0.03);
    cairo_pattern_add_color_stop_rgb(g, 1, 0, 0, 0);
    cairo_rectangle(cr, sx, sy, sw, sh);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    if (!vm) return FALSE;

    if (vm->running && vm->shot) {              /* its screen, fitted in ours */
        double iw = gdk_pixbuf_get_width(vm->shot), ih = gdk_pixbuf_get_height(vm->shot);
        double k = MIN(sw / iw, sh / ih);
        cairo_rectangle(cr, sx, sy, sw, sh);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_fill(cr);
        cairo_save(cr);
        cairo_translate(cr, sx + (sw - iw * k) / 2, sy + (sh - ih * k) / 2);
        cairo_scale(cr, k, k);
        gdk_cairo_set_source_pixbuf(cr, vm->shot, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        return FALSE;
    }

    PangoLayout *pl = gtk_widget_create_pango_layout(w, NULL);
    char *mk = vm->running
        ? g_markup_printf_escaped("<b>%s</b>\n<span size='small' foreground='#7fd67f'>Running</span>", vm->name)
        : g_markup_printf_escaped("<b>%s</b>", vm->name);
    pango_layout_set_markup(pl, mk, -1);
    g_free(mk);
    pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
    pango_layout_set_width(pl, (int)((sw - 16) * PANGO_SCALE));
    pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 13");
    pango_layout_set_font_description(pl, fd);
    pango_font_description_free(fd);
    int tw, th;
    pango_layout_get_pixel_size(pl, &tw, &th);
    cairo_move_to(cr, sx + 8, sy + (sh - th) / 2);
    cairo_set_source_rgb(cr, 1, 1, 1);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
    return FALSE;
}

static const char *display_name(const char *d) {
    if (!d) return "Machine default";
    if (strcmp(d, "sdl") == 0) return "SDL window";
    if (strcmp(d, "gtk") == 0) return "GTK window";
    if (strcmp(d, "none") == 0) return "None (headless)";
    return d;
}

static void show_details(Manager *mg) {
    Vm *vm = mg->sel;
    gtk_stack_set_visible_child_name(GTK_STACK(mg->stack), vm ? "details" : "empty");
    mg->preview = NULL;
    clear_children(mg->details);
    if (!vm) return;
    const MachineType *t = &types[vm->type];
    const FamilyInfo *fi = family_info(t->family);

    /* General (+ ROMs) beside the preview */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(left, TRUE);
    GtkWidget *g = detail_section(left, "computer", "General");
    detail_row(g, "Name", vm->name);
    detail_row(g, "Machine", t->desc);
    char *ty = g_strdup_printf("%s (%s)", t->name, fi ? fi->label : t->family);
    detail_row(g, "Type", ty);
    g_free(ty);
    if (t->status[0]) {
        GtkWidget *st = detail_row(g, "Emulation", t->status);
        char *mk = g_markup_printf_escaped("<span foreground='%s'>%s</span>", status_color(t->status), t->status);
        gtk_label_set_markup(GTK_LABEL(st), mk);
        g_free(mk);
    }
    if (t->alias[0]) detail_row(g, "Variant of", t->alias);
    if (has_known_roms(t)) {
        int total = 0, hits = roms_found(mg, vm, &total);
        detail_row(g, "ROM folder", vm->rom_dir ? vm->rom_dir : "Not set");
        char *f = hits == total ? g_strdup_printf("All %d known ROMs", total)
                                : g_strdup_printf("%d of %d known ROMs", hits, total);
        detail_row(g, "ROMs found", f);
        g_free(f);
    }
    gtk_container_add(GTK_CONTAINER(top), left);
    gtk_container_add(GTK_CONTAINER(top), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *pg = detail_section(pbox, "video-display", "Preview");
    mg->preview = gtk_drawing_area_new();
    gtk_widget_set_size_request(mg->preview, 250, 160);
    g_signal_connect(mg->preview, "draw", G_CALLBACK(draw_preview), mg);
    /* under the title, not indented like the key/value rows */
    GtkWidget *psec = gtk_widget_get_parent(pg);
    gtk_container_remove(GTK_CONTAINER(psec), pg);
    gtk_container_add(GTK_CONTAINER(psec), mg->preview);
    gtk_container_add(GTK_CONTAINER(top), pbox);
    gtk_container_add(GTK_CONTAINER(mg->details), top);
    gtk_container_add(GTK_CONTAINER(mg->details), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* System */
    GtkWidget *s = detail_section(mg->details, "preferences-system,applications-system", "System");
    if (t->ram[0] || vm->ram) {
        guint64 unit = type_ram_unit(t);
        char *m = size_human(size_parse(vm->ram ? vm->ram : t->ram, unit));
        detail_row(s, "Base Memory", m);
        g_free(m);
    }
    const char *cpu = vm->cpu ? vm->cpu : t->cpu;
    if (cpu[0]) {
        char *p = vm->cpu_count > 1 ? g_strdup_printf("%s (%d configured, 1 emulated)", cpu_desc(cpu), vm->cpu_count)
                                    : g_strdup(cpu_desc(cpu));
        detail_row(s, "Processor", p);
        g_free(p);
    }
    detail_row(s, "Execution engine", fi && fi->jit && !vm->accel ? "JIT recompiler" : "Interpreter");
    if (t->chipset[0]) detail_row(s, "Chipset", t->chipset);
    if (g_hash_table_size(vm->features)) {
        char *m = machine_arg(vm);
        const char *comma = strchr(m, ',');
        detail_row(s, "Features changed", comma ? comma + 1 : "");
        g_free(m);
    }

    /* Storage */
    Slot slots[MAX_SLOTS];
    int n = type_slots(t, slots);
    if (n) {
        GtkWidget *st = detail_section(mg->details, "drive-harddisk", "Storage");
        for (int i = 0; i < n; i++) {
            const char *path = vm_media(vm, slots[i].info->key);
            char *base = path ? g_path_get_basename(path) : NULL;
            GtkWidget *v = detail_row(st, slots[i].info->label, base ? base : "Empty");
            if (path) gtk_widget_set_tooltip_text(v, path);
            g_free(base);
        }
    }

    /* Audio */
    GtkWidget *a = detail_section(mg->details, "audio-card,audio-speakers,audio-volume-high", "Audio");
    if (!type_has_audio(t)) detail_row(a, NULL, "None");
    else if (vm->sound && strcmp(vm->sound, "none") == 0) detail_row(a, NULL, "Disabled");
    else {
        char *c = sound_label(vm->sound ? vm->sound : t->soundhw);
        detail_row(a, "Controller", c);
        g_free(c);
    }

    /* Network */
    const GemuDevDesc3 *nics[4];
    if (type_nics(t, nics, 4)) {
        GtkWidget *nw = detail_section(mg->details, "network-wired,network-workgroup", "Network");
        if (vm->nic_off) detail_row(nw, "Adapter 1", "Disabled");
        else {
            const GemuDevDesc3 *model = nics[0];
            for (int i = 0; vm->nic_model && i < 4 && nics[i]; i++)
                if (strcmp(nics[i]->name, vm->nic_model) == 0) model = nics[i];
            char *d = strip_status(model->desc);
            bool tap = vm->nic_attach && strcmp(vm->nic_attach, "tap") == 0;
            char *how = tap ? g_strdup_printf("TAP interface%s%s", vm->nic_ifname ? " " : "",
                                              vm->nic_ifname ? vm->nic_ifname : "")
                            : g_strdup_printf("NAT%s", vm->nic_fwd->len ? ", port forwarding" : "");
            char *txt = g_strdup_printf("%s (%s)", d, how);
            detail_row(nw, "Adapter 1", txt);
            g_free(txt);
            g_free(how);
            g_free(d);
        }
    }

    /* Devices */
    GtkWidget *d = detail_section(mg->details, "input-gaming,input-keyboard", "Devices");
    GString *att = g_string_new(NULL);
    for (guint i = 0; i < vm->devices->len; i++) {
        char *dd = device_desc(g_ptr_array_index(vm->devices, i));
        g_string_append_printf(att, "%s%s", i ? "\n" : "", dd);
        g_free(dd);
    }
    detail_row(d, "Attached", att->len ? att->str : "None");
    g_string_free(att, TRUE);

    /* Display / options */
    GtkWidget *o = detail_section(mg->details, "preferences-desktop-display,video-display", "Display");
    detail_row(o, "Display", display_name(vm->display));
    detail_row(o, "Extra arguments", vm->extra && vm->extra[0] ? vm->extra : "None");

    /* Command line */
    GtkWidget *c = detail_section(mg->details, "utilities-terminal", "Command line");
    char *err = NULL;
    char *cl = cmdline_text(mg, vm, &err);
    GtkWidget *cv = detail_row(c, NULL, cl ? cl : err);
    if (cl) {
        PangoAttrList *mono = pango_attr_list_new();
        pango_attr_list_insert(mono, pango_attr_family_new("monospace"));
        gtk_label_set_attributes(GTK_LABEL(cv), mono);
        pango_attr_list_unref(mono);
    }
    g_free(cl);
    g_free(err);

    /* Description */
    GtkWidget *ds = detail_section(mg->details, "help-about", "Description");
    GtkWidget *dl = detail_row(ds, NULL, t->long_desc[0] ? t->long_desc : "None");
    gtk_label_set_max_width_chars(GTK_LABEL(dl), 80);

    gtk_widget_show_all(mg->details);
}

/* ── Window ──────────────────────────────────────────────────────────────── */

static GtkWidget *build_empty(Manager *mg) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    GtkWidget *icon = gtk_image_new_from_icon_name("computer-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
    gtk_container_add(GTK_CONTAINER(box), dim(icon));
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), "<span size='large' weight='bold'>No machine selected</span>");
    gtk_container_add(GTK_CONTAINER(box), l);
    GtkWidget *t = dim(wrap_label());
    gtk_label_set_justify(GTK_LABEL(t), GTK_JUSTIFY_CENTER);
    gtk_label_set_xalign(GTK_LABEL(t), 0.5f);
    gtk_label_set_text(GTK_LABEL(t), "Create a machine, give it its ROMs and media, "
                                     "and start it whenever you like.");
    gtk_container_add(GTK_CONTAINER(box), t);
    GtkWidget *b = gtk_button_new_with_label("New Machine…");   /* Alt+N is the header's */
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "suggested-action");
    gtk_widget_set_halign(b, GTK_ALIGN_CENTER);
    g_signal_connect(b, "clicked", G_CALLBACK(on_new), mg);
    gtk_container_add(GTK_CONTAINER(box), b);
    return box;
}

static GtkWidget *build_list(Manager *mg) {
    mg->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(mg->list), GTK_SELECTION_BROWSE);
    gtk_list_box_set_sort_func(GTK_LIST_BOX(mg->list), sort_rows, NULL, NULL);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(mg->list), FALSE);
    GtkWidget *ph = dim(gtk_label_new("No machines yet"));
    gtk_widget_show(ph);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(mg->list), ph);
    g_signal_connect(mg->list, "row-selected", G_CALLBACK(on_row_selected), mg);
    g_signal_connect(mg->list, "row-activated", G_CALLBACK(on_row_activated), mg);
    for (guint i = 0; i < mg->vms->len; i++)
        gtk_container_add(GTK_CONTAINER(mg->list), vm_row(g_ptr_array_index(mg->vms, i)));

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), mg->list);
    gtk_widget_set_size_request(scroll, 260, -1);
    return scroll;
}

/* Runs before the widgets go, so no callback touches them afterwards.  The
 * machines still running keep the loop alive until they exit: their stderr
 * comes through this process, and closing that pipe under them would kill
 * them with SIGPIPE. */
static void on_destroy(GtkWidget *w, gpointer data) {
    (void)w;
    Manager *mg = data;
    mg->closing = true;
    if (!mg->running) gtk_main_quit();
}

#ifndef _WIN32
/* main() installed the monitor's SIGINT/SIGTERM handlers, which only set a
 * flag this loop never looks at: close the window instead.  The machines
 * get Ctrl+C themselves (same process group); the loop then waits for them. */
static gboolean on_signal(gpointer data) {
    Manager *mg = data;
    if (mg->ui) {                                   /* open_settings() closes the window after it */
        mg->quit_pending = true;
        gtk_dialog_response(GTK_DIALOG(mg->ui->dialog), GTK_RESPONSE_CANCEL);
    } else if (!mg->closing) {
        gtk_widget_destroy(mg->window);
    }
    return G_SOURCE_CONTINUE;
}
#endif

static void on_quit(GtkWidget *w, gpointer data) {
    (void)w;
    gtk_widget_destroy(((Manager *)data)->window);
}

static void on_about(GtkWidget *w, gpointer data) {
    (void)w;
    Manager *mg = data;
    gtk_show_about_dialog(GTK_WINDOW(mg->window),
                          "program-name", "GEMU",
                          "version", GEMU_VERSION_STR,
                          "comments", "Generic EMUlator",
                          "logo-icon-name", "applications-games",
                          NULL);
}

static bool have_graphical_session(void) {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    const char *x = getenv("DISPLAY"), *w = getenv("WAYLAND_DISPLAY");
    return (x && x[0]) || (w && w[0]);
#endif
}

bool gemu_machine_manager_run(const char *prog) {
    if (!have_graphical_session()) return false;
    /* GTK would otherwise switch the C library to the user's locale for the
     * whole run, and with it how every machine parses "1.5" */
    gtk_disable_setlocale();
    if (!gtk_init_check(NULL, NULL)) return false;

    Manager mg = { .prog = prog,
                   .vms = g_ptr_array_new_with_free_func((GDestroyNotify)vm_free),
                   .argv = str_array(),
                   .hash_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free) };
    load_vms(&mg);

    mg.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(mg.window), "GEMU Machine Manager");
    gtk_window_set_default_size(GTK_WINDOW(mg.window), 960, 640);
    gtk_window_set_icon_name(GTK_WINDOW(mg.window), "applications-games");
    g_signal_connect(mg.window, "destroy", G_CALLBACK(on_destroy), &mg);
#ifndef _WIN32
    guint sig_int = g_unix_signal_add(SIGINT, on_signal, &mg);
    guint sig_term = g_unix_signal_add(SIGTERM, on_signal, &mg);
#endif

    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(mg.window), accel);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* menu bar */
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = gtk_menu_new(), *machine = gtk_menu_new(), *help = gtk_menu_new();
    struct { GtkWidget *menu; const char *label; } tops[] = {
        { file, "_File" }, { machine, "_Machine" }, { help, "_Help" },
    };
    for (int i = 0; i < 3; i++) {
        GtkWidget *it = gtk_menu_item_new_with_mnemonic(tops[i].label);
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(it), tops[i].menu);
        gtk_menu_shell_append(GTK_MENU_SHELL(bar), it);
    }
#define MENU_ITEM(menu, label, cb, key, out) do { \
        GtkWidget *it_ = gtk_menu_item_new_with_mnemonic(label); \
        g_signal_connect(it_, "activate", G_CALLBACK(cb), &mg); \
        if (key) gtk_widget_add_accelerator(it_, "activate", accel, key, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE); \
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it_); \
        out = it_; } while (0)
    GtkWidget *it;
    MENU_ITEM(file, "_New Machine…", on_new_item, GDK_KEY_n, it);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    MENU_ITEM(file, "_Quit", on_quit, GDK_KEY_q, it);
    MENU_ITEM(machine, "_Settings…", on_settings, GDK_KEY_s, mg.mi_settings);
    MENU_ITEM(machine, "S_tart", on_start_item, 0, mg.mi_start);
    gtk_menu_shell_append(GTK_MENU_SHELL(machine), gtk_separator_menu_item_new());
    MENU_ITEM(machine, "_Delete…", on_delete_item, 0, mg.mi_delete);
    MENU_ITEM(help, "_About GEMU", on_about, 0, it);
#undef MENU_ITEM
    (void)it;
    gtk_container_add(GTK_CONTAINER(outer), bar);

    /* toolbar */
    GtkWidget *tb = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(tb), GTK_TOOLBAR_BOTH);
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(tb), GTK_ICON_SIZE_DND);
    struct { const char *icon, *label, *tip; GCallback cb; GtkWidget **out; } tools[] = {
        { "list-add",             "New",      "Create a machine (Ctrl+N)",        G_CALLBACK(on_new_item),    NULL },
        { "preferences-system",   "Settings", "Change this machine (Ctrl+S)",     G_CALLBACK(on_settings),    &mg.tb_settings },
        { NULL, NULL, NULL, NULL, NULL },
        { "user-trash",           "Delete",   "Delete this machine",              G_CALLBACK(on_delete_item), &mg.tb_delete },
        { "media-playback-start", "Start",    "Start this machine",               G_CALLBACK(on_start_item),  &mg.tb_start },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(tools); i++) {
        GtkToolItem *ti;
        if (!tools[i].icon) {
            ti = gtk_separator_tool_item_new();
        } else {
            ti = gtk_tool_button_new(gtk_image_new_from_icon_name(tools[i].icon, GTK_ICON_SIZE_DND),
                                     tools[i].label);
            gtk_tool_item_set_tooltip_text(ti, tools[i].tip);
            g_signal_connect(ti, "clicked", tools[i].cb, &mg);
            if (tools[i].out) *tools[i].out = GTK_WIDGET(ti);
        }
        gtk_toolbar_insert(GTK_TOOLBAR(tb), ti, -1);
    }
    gtk_container_add(GTK_CONTAINER(outer), tb);

    /* machine list | details */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    GtkWidget *list = build_list(&mg);
    gtk_paned_pack1(GTK_PANED(paned), list, FALSE, FALSE);
    mg.stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(mg.stack), build_empty(&mg), "empty");
    mg.details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *dscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(dscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(dscroll), mg.details);
    gtk_stack_add_named(GTK_STACK(mg.stack), dscroll, "details");
    gtk_paned_pack2(GTK_PANED(paned), mg.stack, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 340);
    gtk_container_add(GTK_CONTAINER(outer), paned);
    gtk_container_add(GTK_CONTAINER(mg.window), outer);

    gtk_widget_show_all(mg.window);
    show_details(&mg);
    update_buttons(&mg);

    /* back on the machine used last, or the first one */
    char *last = ini_get("manager", "selected");
    Vm *pick = NULL;
    for (guint i = 0; i < mg.vms->len && last; i++)
        if (strcmp(((Vm *)g_ptr_array_index(mg.vms, i))->id, last) == 0) pick = g_ptr_array_index(mg.vms, i);
    g_free(last);
    if (pick) select_vm(&mg, pick);
    else {
        GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(mg.list), 0);
        if (first) gtk_list_box_select_row(GTK_LIST_BOX(mg.list), first);
    }

    gtk_main();
#ifndef _WIN32
    g_source_remove(sig_int);
    g_source_remove(sig_term);
#endif
    g_ptr_array_free(mg.argv, TRUE);
    g_ptr_array_free(mg.vms, TRUE);
    g_hash_table_destroy(mg.hash_cache);
    return true;
}
#endif /* GEMU_GTK */
