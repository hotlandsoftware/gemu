/*
 * Machine Manager (see gemu/machine_manager.h), GTK 3.
 *
 * The user's machines, like a VM manager's: each is a name, a machine type
 * (one of the data/machine XMLs) and its settings - ROM folder, media,
 * devices, display, extra arguments - kept in <config>/machines/<id>.ini.
 *
 * Left: the machines created so far.  Right: the selected one's settings,
 * saved as they are changed, and the command line they add up to, so the
 * manager also teaches the CLI.  New picks a type from every built-in
 * machine (grouped by family, searchable) and names it.  Start runs the
 * command line as a child gemu process; the manager stays open, passes the
 * machine's stderr through to its own and shows it in a dialog if the
 * machine stops with an error - started from a desktop, there is no
 * terminal to read it in.
 *
 * Tables come from configure: generated/manager_machines.inc (types),
 * generated/devices.inc and generated/romdb.inc.
 */
#ifdef GEMU_GTK
#include "gemu/machine_manager.h"
#include "gemu/args.h"
#include "gemu/gemu.h"
#include "gemu/util.h"
#include "gemu/sha256.h"
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <glib-unix.h>
#include <signal.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

/* ── Built-in tables ─────────────────────────────────────────────────────── */

typedef struct {
    const char *name, *family, *desc, *status, *alias, *long_desc, *devices;
} MachineType;

static const MachineType types[] = {
#include "generated/manager_machines.inc"
};
#define N_TYPES ((int)(sizeof types / sizeof *types))

static const GemuDevDesc3 devices[] = {
#include "generated/devices.inc"
};
#define N_DEVICES ((int)(sizeof devices / sizeof *devices))

/* Same rows as hardware/romdb.c's table: the ROMs each machine knows by SHA-256. */
typedef struct { const char *sha256; uint32_t addr; const char *region, *machine, *label; } KnownRom;
static const KnownRom roms[] = {
#include "generated/romdb.inc"
};
#define N_ROMS ((int)(sizeof roms / sizeof *roms))
#define ROM_MAX_SIZE (64u << 20)                 /* romdb.c skips bigger files too */
#define MAX_MACHINE_ROMS 64

/* Which file options a family's setup accepts ("" = the positional ROM
 * argument), and the key each is saved under. */
typedef struct { const char *opt, *label, *key; } MediaSlot;
typedef struct { const char *family, *label; MediaSlot media[4]; } FamilyInfo;
#define MAX_MEDIA 4

static const FamilyInfo families[] = {
    { "mos",   "MOS 65xx",      { { "-cartridge", "Cartridge", "cartridge" }, { "-rom", "ROM", "rom" },
                                  { "-tape", "Tape", "tape" }, { "-fda", "Floppy", "fda" } } },
    { "ia64",  "Intel Itanium", { { "-hda", "Hard disk", "hda" }, { "-cdrom", "CD-ROM", "cdrom" },
                                  { "-fda", "Floppy", "fda" } } },
    { "x86",   "Intel x86",     { { "-fda", "Floppy", "fda" }, { "-hda", "Hard disk", "hda" },
                                  { "-rom", "ROM", "rom" } } },
    { "z80",   "Zilog Z80",     { { "-rom", "ROM", "rom" } } },
    { "rca",   "RCA COSMAC",    { { "-cartridge", "Cartridge", "cartridge" }, { "-rom", "ROM", "rom" },
                                  { "-tape", "Tape", "tape" } } },
    { "chip8", "CHIP-8",        { { "", "Program", "program" } } },
    { "mitsu", "Mitsubishi",    { { NULL, NULL, NULL } } },
};
#define N_FAMILIES ((int)(sizeof families / sizeof *families))

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

/* A machine with known ROMs takes a folder, so its "-rom FILE" slot goes. */
static bool slot_used(const MachineType *t, int i) {
    const FamilyInfo *fi = family_info(t->family);
    if (!fi || !fi->media[i].label) return false;
    int idx[1];
    return !(strcmp(fi->media[i].opt, "-rom") == 0 && type_roms(t, idx, 1));
}

/* ── State ───────────────────────────────────────────────────────────────── */

/* One of the user's machines. */
typedef struct {
    char       *id;                          /* <config>/machines/<id>.ini */
    char       *name;
    int         type;                        /* types[] index */
    char       *rom_dir;                     /* -rom DIR, NULL = none */
    char       *media[MAX_MEDIA];            /* per family slot, NULL = empty */
    GPtrArray  *devices;                     /* extra -device names */
    char       *display;                     /* NULL = machine default */
    char       *extra;
    bool        running;
    GtkWidget  *row, *row_name, *row_state;
} Vm;

typedef struct Manager {
    const char   *prog;
    GPtrArray    *vms;                       /* Vm * */
    Vm           *sel;
    bool          loading;                   /* filling the form: don't save */
    bool          closing;                   /* window gone: waiting for machines to exit */
    int           running;
    GHashTable   *hash_cache;                /* path -> CachedHash */
    GPtrArray    *argv;                      /* the selected machine's command line */

    GtkWidget    *window, *header, *list, *start, *delete;
    GtkWidget    *empty, *form;              /* right pane: nothing selected / settings */
    GtkWidget    *name, *type_title, *type_sub, *long_desc;
    GtkWidget    *rom_section, *rom_folder, *rom_list, *rom_summary;
    GtkWidget    *media_section, *media_label[MAX_MEDIA], *media_file[MAX_MEDIA], *media_clear[MAX_MEDIA];
    GtkWidget    *devices_box, *devices_default;
    GtkWidget    *display, *extra, *cmdline;
} Manager;

/* One machine the manager started. */
typedef struct {
    Manager    *mg;
    Vm         *vm;
    GPid        pid;
    GIOChannel *err;
    guint       err_watch;
    GString    *err_text;                    /* the last ERR_KEEP bytes of its stderr */
} Child;
#define ERR_KEEP 8192

static void update_cmdline(Manager *mg);
static char *self_exe(const char *prog);

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

static void vm_save(const Vm *vm) {
    const MachineType *t = &types[vm->type];
    const FamilyInfo *fi = family_info(t->family);
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "machine", "name", vm->name);
    g_key_file_set_string(kf, "machine", "type", t->name);
    if (vm->rom_dir) g_key_file_set_string(kf, "machine", "rom_dir", vm->rom_dir);
    if (vm->display) g_key_file_set_string(kf, "machine", "display", vm->display);
    if (vm->extra && vm->extra[0]) g_key_file_set_string(kf, "machine", "extra", vm->extra);
    if (vm->devices->len)
        g_key_file_set_string_list(kf, "machine", "devices",
                                   (const char *const *)vm->devices->pdata, vm->devices->len);
    for (int i = 0; i < MAX_MEDIA; i++)
        if (fi && fi->media[i].key && vm->media[i])
            g_key_file_set_string(kf, "media", fi->media[i].key, vm->media[i]);

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
    vm->devices = g_ptr_array_new_with_free_func(g_free);
    return vm;
}

static void vm_free(Vm *vm) {
    g_free(vm->id);
    g_free(vm->name);
    g_free(vm->rom_dir);
    for (int i = 0; i < MAX_MEDIA; i++) g_free(vm->media[i]);
    g_ptr_array_free(vm->devices, TRUE);
    g_free(vm->display);
    g_free(vm->extra);
    g_free(vm);
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
    vm->rom_dir = g_key_file_get_string(kf, "machine", "rom_dir", NULL);
    vm->display = g_key_file_get_string(kf, "machine", "display", NULL);
    vm->extra   = g_key_file_get_string(kf, "machine", "extra", NULL);
    gsize n = 0;
    char **devs = g_key_file_get_string_list(kf, "machine", "devices", &n, NULL);
    for (gsize i = 0; i < n; i++) g_ptr_array_add(vm->devices, g_strdup(devs[i]));
    g_strfreev(devs);
    const FamilyInfo *fi = family_info(types[ti].family);
    for (int i = 0; i < MAX_MEDIA; i++)
        if (fi && fi->media[i].key)
            vm->media[i] = g_key_file_get_string(kf, "media", fi->media[i].key, NULL);
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
 * name - the machine list shows the whole description under it anyway. */
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

static bool device_fits(const GemuDevDesc3 *d, const MachineType *t) {
    return list_has(d->machines, t->name) || (t->alias[0] && list_has(d->machines, t->alias));
}

/* devices.inc descriptions end in " [status]"; the list doesn't need it */
static char *strip_status(const char *desc) {
    const char *b = strrchr(desc, '[');
    size_t n = strlen(desc);
    if (b && desc[n - 1] == ']' && b > desc && b[-1] == ' ') n = (size_t)(b - 1 - desc);
    return g_strndup(desc, n);
}

static void clear_children(GtkWidget *box) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
}

static void set_str(char **field, const char *v) {
    g_free(*field);
    *field = v && v[0] ? g_strdup(v) : NULL;
}

static GtkWidget *section(const char *text) {
    GtkWidget *l = gtk_label_new(NULL);
    char *m = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(m);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_widget_set_margin_top(l, 12);
    return l;
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

static void message(Manager *mg, GtkMessageType type, const char *title, const char *text) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(mg->window), GTK_DIALOG_DESTROY_WITH_PARENT,
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

    GPtrArray *rootv = g_ptr_array_new_with_free_func(g_free);
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

/* Fill the ROM rows for the selected machine's folder. */
static void show_roms(Manager *mg) {
    Vm *vm = mg->sel;
    const MachineType *t = &types[vm->type];
    int idx[MAX_MACHINE_ROMS];
    int n = type_roms(t, idx, MAX_MACHINE_ROMS);
    char *found[MAX_MACHINE_ROMS];
    int hits = scan_rom_dir(mg, vm->rom_dir, t, idx, n, found);

    clear_children(mg->rom_list);
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
        gtk_container_add(GTK_CONTAINER(mg->rom_list), row);
        g_free(found[i]);
    }
    gtk_widget_show_all(mg->rom_list);

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
    gtk_label_set_text(GTK_LABEL(mg->rom_summary), sum);
    g_free(sum);
}

/* ── Command line ────────────────────────────────────────────────────────── */

static void push(Manager *mg, const char *s) { g_ptr_array_add(mg->argv, g_strdup(s)); }

/* Rebuild mg->argv for the selected machine; false (with a reason in *err)
 * if it can't be run as it stands. */
static bool build_argv(Manager *mg, char **err) {
    g_ptr_array_set_size(mg->argv, 0);
    *err = NULL;
    Vm *vm = mg->sel;
    if (!vm) { *err = g_strdup("No machine selected."); return false; }
    const MachineType *t = &types[vm->type];
    const FamilyInfo *fi = family_info(t->family);
    int idx[1];

    push(mg, mg->prog);
    push(mg, "-M");
    push(mg, t->name);
    if (vm->rom_dir && type_roms(t, idx, 1)) {
        push(mg, "-rom");
        push(mg, vm->rom_dir);
    }
    for (int i = 0; i < MAX_MEDIA; i++)
        if (vm->media[i] && slot_used(t, i)) {
            if (fi->media[i].opt[0]) push(mg, fi->media[i].opt);
            push(mg, vm->media[i]);
        }
    for (guint i = 0; i < vm->devices->len; i++) {
        push(mg, "-device");
        push(mg, g_ptr_array_index(vm->devices, i));
    }
    if (vm->display) {
        push(mg, "-display");
        push(mg, vm->display);
    }
    if (vm->extra && vm->extra[strspn(vm->extra, " \t")]) {
        int n = 0;
        char **v = NULL;
        GError *e = NULL;
        if (!g_shell_parse_argv(vm->extra, &n, &v, &e)) {
            *err = g_strdup_printf("Extra arguments: %s", e->message);
            g_error_free(e);
            return false;
        }
        for (int i = 0; i < n; i++) push(mg, v[i]);
        g_strfreev(v);
    }
    return true;
}

static void update_buttons(Manager *mg) {
    char *err = NULL;
    bool ok = build_argv(mg, &err);
    g_free(err);
    gtk_widget_set_sensitive(mg->start, ok && !mg->sel->running);
    gtk_widget_set_sensitive(mg->delete, mg->sel && !mg->sel->running);
}

static void update_cmdline(Manager *mg) {
    char *err = NULL;
    bool ok = build_argv(mg, &err);
    gtk_widget_set_sensitive(mg->start, ok && !mg->sel->running);
    gtk_widget_set_sensitive(mg->delete, mg->sel && !mg->sel->running);
    if (!ok) {
        gtk_label_set_text(GTK_LABEL(mg->cmdline), err);
        g_free(err);
        return;
    }
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < mg->argv->len; i++) {
        const char *a = g_ptr_array_index(mg->argv, i);
        if (i) g_string_append_c(s, ' ');
        /* quote only what needs it, so the common case reads naturally */
        if (a[0] && !a[strcspn(a, " \t\n'\"\\$`*?[]{}()<>|&;#~!")]) {
            g_string_append(s, a);
        } else {
            char *q = g_shell_quote(a);
            g_string_append(s, q);
            g_free(q);
        }
    }
    gtk_label_set_text(GTK_LABEL(mg->cmdline), s->str);
    g_string_free(s, TRUE);
}

/* A setting changed: keep it and show what it does. */
static void changed(Manager *mg) {
    if (mg->loading || mg->closing || !mg->sel) return;
    vm_save(mg->sel);
    update_cmdline(mg);
}

/* ── Machine list ────────────────────────────────────────────────────────── */

static void update_row(Vm *vm) {
    gtk_label_set_text(GTK_LABEL(vm->row_name), vm->name);
    if (vm->running) {
        gtk_label_set_markup(GTK_LABEL(vm->row_state),
                             "<span foreground='#2e8b3d' weight='bold'>Running</span>");
    } else {
        char *mk = g_markup_printf_escaped("<span alpha='60%%'>%s</span>", types[vm->type].desc);
        gtk_label_set_markup(GTK_LABEL(vm->row_state), mk);
        g_free(mk);
    }
}

static GtkWidget *vm_row(Vm *vm) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    vm->row_name = gtk_label_new(NULL);
    PangoAttrList *bold = pango_attr_list_new();
    pango_attr_list_insert(bold, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(vm->row_name), bold);
    pango_attr_list_unref(bold);
    vm->row_state = gtk_label_new(NULL);
    for (int i = 0; i < 2; i++) {
        GtkWidget *l = i ? vm->row_state : vm->row_name;
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_container_add(GTK_CONTAINER(box), l);
    }
    vm->row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(vm->row), box);
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

/* ── Settings pane ───────────────────────────────────────────────────────── */

static void on_name(GtkEntry *e, gpointer data) {
    Manager *mg = data;
    if (mg->loading || !mg->sel) return;
    const char *v = gtk_entry_get_text(e);
    if (!v[strspn(v, " ")]) return;                 /* keep the old name until there is a new one */
    set_str(&mg->sel->name, v);
    update_row(mg->sel);
    gtk_list_box_invalidate_sort(GTK_LIST_BOX(mg->list));
    changed(mg);
}

static void on_rom_folder(GtkFileChooserButton *b, gpointer data) {
    Manager *mg = data;
    Vm *vm = mg->sel;
    if (mg->loading || !vm) return;
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
    changed(mg);
}

static void on_media(GtkFileChooserButton *b, gpointer data) {
    Manager *mg = data;
    if (mg->loading || !mg->sel) return;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "slot"));
    g_free(mg->sel->media[i]);
    mg->sel->media[i] = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(b));
    changed(mg);
}

static void on_media_clear(GtkButton *b, gpointer data) {
    Manager *mg = data;
    if (!mg->sel) return;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "slot"));
    gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(mg->media_file[i]));
    set_str(&mg->sel->media[i], NULL);
    changed(mg);
}

static void on_device(GtkToggleButton *cb, gpointer data) {
    Manager *mg = data;
    if (mg->loading || !mg->sel) return;
    const char *dev = g_object_get_data(G_OBJECT(cb), "device");
    GPtrArray *d = mg->sel->devices;
    for (guint i = 0; i < d->len; i++)
        if (strcmp(g_ptr_array_index(d, i), dev) == 0) { g_ptr_array_remove_index(d, i); break; }
    if (gtk_toggle_button_get_active(cb)) g_ptr_array_add(d, g_strdup(dev));
    changed(mg);
}

static void on_display(GtkComboBox *c, gpointer data) {
    Manager *mg = data;
    if (mg->loading || !mg->sel) return;
    const char *id = gtk_combo_box_get_active_id(c);
    set_str(&mg->sel->display, id && strcmp(id, "default") != 0 ? id : NULL);
    changed(mg);
}

static void on_extra(GtkEntry *e, gpointer data) {
    Manager *mg = data;
    if (mg->loading || !mg->sel) return;
    set_str(&mg->sel->extra, gtk_entry_get_text(e));
    changed(mg);
}

static bool vm_has_device(const Vm *vm, const char *dev) {
    for (guint i = 0; i < vm->devices->len; i++)
        if (strcmp(g_ptr_array_index(vm->devices, i), dev) == 0) return true;
    return false;
}

/* Fill the settings pane from the selected machine. */
static void show_vm(Manager *mg) {
    Vm *vm = mg->sel;
    gtk_widget_set_visible(mg->empty, vm == NULL);
    gtk_widget_set_visible(mg->form, vm != NULL);
    if (!vm) {
        update_cmdline(mg);
        return;
    }
    mg->loading = true;
    const MachineType *t = &types[vm->type];
    const FamilyInfo *fi = family_info(t->family);

    gtk_entry_set_text(GTK_ENTRY(mg->name), vm->name);
    gtk_label_set_text(GTK_LABEL(mg->type_title), t->desc);
    char *sub = type_subtitle(t);
    gtk_label_set_text(GTK_LABEL(mg->type_sub), sub);
    g_free(sub);
    gtk_label_set_text(GTK_LABEL(mg->long_desc), t->long_desc);
    gtk_widget_set_visible(mg->long_desc, t->long_desc[0] != '\0');

    /* ROMs */
    int idx[1];
    bool has_roms = type_roms(t, idx, 1) > 0;
    gtk_widget_set_visible(mg->rom_section, has_roms);
    if (has_roms) {
        if (vm->rom_dir) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(mg->rom_folder), vm->rom_dir);
        else gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(mg->rom_folder));
        show_roms(mg);
    }

    /* media */
    bool any_media = false;
    for (int i = 0; i < MAX_MEDIA; i++) {
        bool used = slot_used(t, i);
        any_media |= used;
        if (used) {
            gtk_label_set_text(GTK_LABEL(mg->media_label[i]), fi->media[i].label);
            if (vm->media[i]) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(mg->media_file[i]), vm->media[i]);
            else gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(mg->media_file[i]));
        }
        gtk_widget_set_visible(mg->media_label[i], used);
        gtk_widget_set_visible(mg->media_file[i], used);
        gtk_widget_set_visible(mg->media_clear[i], used);
    }
    gtk_widget_set_visible(mg->media_section, any_media);

    /* devices */
    clear_children(mg->devices_box);
    int n_dev = 0;
    for (int i = 0; i < N_DEVICES; i++) {
        const GemuDevDesc3 *d = &devices[i];
        if (!device_fits(d, t)) continue;
        char *desc = strip_status(d->desc);
        char *label = g_strdup_printf("%s - %s", d->name, desc);
        GtkWidget *cb = gtk_check_button_new_with_label(label);
        g_free(label);
        g_free(desc);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb), vm_has_device(vm, d->name));
        g_object_set_data(G_OBJECT(cb), "device", (gpointer)d->name);
        g_signal_connect(cb, "toggled", G_CALLBACK(on_device), mg);
        gtk_container_add(GTK_CONTAINER(mg->devices_box), cb);
        n_dev++;
    }
    if (!n_dev) {
        GtkWidget *l = dim(gtk_label_new("No optional devices for this machine."));
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_container_add(GTK_CONTAINER(mg->devices_box), l);
    }
    gtk_widget_show_all(mg->devices_box);
    char *defs = t->devices[0]
        ? g_strdup_printf("Attached by default: %s. Ticking one adds another.", t->devices)
        : g_strdup("Nothing is attached by default.");
    gtk_label_set_text(GTK_LABEL(mg->devices_default), defs);
    g_free(defs);

    gtk_combo_box_set_active_id(GTK_COMBO_BOX(mg->display), vm->display ? vm->display : "default");
    gtk_entry_set_text(GTK_ENTRY(mg->extra), vm->extra ? vm->extra : "");

    mg->loading = false;
    update_cmdline(mg);
}

static void on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box;
    Manager *mg = data;
    if (mg->closing) return;
    mg->sel = row ? g_object_get_data(G_OBJECT(row), "vm") : NULL;
    if (mg->sel) ini_set("manager", "selected", mg->sel->id);
    show_vm(mg);
}

static void select_vm(Manager *mg, Vm *vm) {
    if (vm) gtk_list_box_select_row(GTK_LIST_BOX(mg->list), GTK_LIST_BOX_ROW(vm->row));
    else gtk_list_box_unselect_all(GTK_LIST_BOX(mg->list));
}

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
    const char *color = NULL;
    if (s && strcmp(s, "working") == 0)          color = "#2e8b3d";
    else if (s && strcmp(s, "imperfect") == 0)   color = "#b8860b";
    else if (s && (strcmp(s, "preliminary") == 0 || strcmp(s, "skeleton") == 0)) color = "#c0392b";
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

/* ── Delete ──────────────────────────────────────────────────────────────── */

static void on_delete(GtkButton *b, gpointer data) {
    (void)b;
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
        message(mg, GTK_MESSAGE_ERROR, "Could not delete the machine", path);
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
    else { mg->sel = NULL; show_vm(mg); }
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

static void update_running(Manager *mg) {
    char *sub = mg->running
        ? g_strdup_printf("v" GEMU_VERSION_STR "  ·  %d running", mg->running)
        : g_strdup("v" GEMU_VERSION_STR);
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(mg->header), sub);
    g_free(sub);
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

static void on_child_exit(GPid pid, gint wait_status, gpointer data) {
    Child *c = data;
    Manager *mg = c->mg;
    while (c->err_watch && on_child_stderr(c->err, G_IO_IN, c)) {}   /* whatever is left */
    if (c->err_watch) g_source_remove(c->err_watch);
    g_io_channel_shutdown(c->err, FALSE, NULL);
    g_io_channel_unref(c->err);
    g_spawn_close_pid(pid);

    mg->running--;
    c->vm->running = false;
    if (!mg->closing) {
        update_row(c->vm);
        update_running(mg);
        if (mg->sel == c->vm) update_buttons(mg);
        GError *e = NULL;
        if (!g_spawn_check_wait_status(wait_status, &e)) {
            char *tail = g_strstrip(g_strdup(c->err_text->str));
            char *title = g_strdup_printf("%s stopped with an error", c->vm->name);
            message(mg, GTK_MESSAGE_ERROR, title, tail[0] ? tail : e->message);
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
    if (!vm || vm->running || !build_argv(mg, &err)) { g_free(err); return; }

    /* run it with this executable, not whatever argv[0] would find */
    char **argv = g_new0(char *, mg->argv->len + 1);
    argv[0] = self_exe(mg->prog);
    for (guint i = 1; i < mg->argv->len; i++) argv[i] = g_ptr_array_index(mg->argv, i);

    GPid pid;
    gint err_fd = -1;
    GError *e = NULL;
    /* stdin and stdout stay the manager's, so -monitor stdio works from a terminal */
    gboolean ok = g_spawn_async_with_pipes(NULL, argv, NULL,
                                           G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CHILD_INHERITS_STDIN,
                                           NULL, NULL, &pid, NULL, NULL, &err_fd, &e);
    g_free(argv[0]);
    g_free(argv);
    if (!ok) {
        char *title = g_strdup_printf("Could not start %s", vm->name);
        message(mg, GTK_MESSAGE_ERROR, title, e->message);
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
    g_child_watch_add(pid, on_child_exit, c);
    vm->running = true;
    mg->running++;
    update_row(vm);
    update_running(mg);
    update_buttons(mg);
}

static void on_start(GtkButton *b, gpointer data) { (void)b; start(data); }

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box; (void)row;
    start(data);
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

static GtkWidget *build_form(Manager *mg) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);

    GtkWidget *nb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_add(GTK_CONTAINER(nb), gtk_label_new_with_mnemonic("_Name"));
    mg->name = gtk_entry_new();
    gtk_widget_set_hexpand(mg->name, TRUE);
    g_signal_connect(mg->name, "changed", G_CALLBACK(on_name), mg);
    gtk_container_add(GTK_CONTAINER(nb), mg->name);
    gtk_container_add(GTK_CONTAINER(box), nb);

    mg->type_title = wrap_label();
    gtk_widget_set_margin_top(mg->type_title, 8);
    PangoAttrList *bold = pango_attr_list_new();
    pango_attr_list_insert(bold, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(mg->type_title), bold);
    pango_attr_list_unref(bold);
    gtk_container_add(GTK_CONTAINER(box), mg->type_title);
    mg->type_sub = dim(wrap_label());
    gtk_container_add(GTK_CONTAINER(box), mg->type_sub);
    mg->long_desc = wrap_label();
    gtk_container_add(GTK_CONTAINER(box), mg->long_desc);

    mg->rom_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(mg->rom_section), section("ROMs"));
    GtkWidget *rf = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_add(GTK_CONTAINER(rf), gtk_label_new("Folder"));
    mg->rom_folder = gtk_file_chooser_button_new("Choose the ROM folder", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    gtk_widget_set_hexpand(mg->rom_folder, TRUE);
    g_signal_connect(mg->rom_folder, "file-set", G_CALLBACK(on_rom_folder), mg);
    gtk_container_add(GTK_CONTAINER(rf), mg->rom_folder);
    gtk_container_add(GTK_CONTAINER(mg->rom_section), rf);
    mg->rom_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(mg->rom_list, 4);
    gtk_container_add(GTK_CONTAINER(mg->rom_section), mg->rom_list);
    mg->rom_summary = dim(wrap_label());
    gtk_container_add(GTK_CONTAINER(mg->rom_section), mg->rom_summary);
    gtk_container_add(GTK_CONTAINER(box), mg->rom_section);

    mg->media_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(mg->media_section), section("Media"));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    for (int i = 0; i < MAX_MEDIA; i++) {
        mg->media_label[i] = gtk_label_new("");
        gtk_widget_set_halign(mg->media_label[i], GTK_ALIGN_END);
        mg->media_file[i] = gtk_file_chooser_button_new("Choose a file", GTK_FILE_CHOOSER_ACTION_OPEN);
        gtk_widget_set_hexpand(mg->media_file[i], TRUE);
        g_object_set_data(G_OBJECT(mg->media_file[i]), "slot", GINT_TO_POINTER(i));
        g_signal_connect(mg->media_file[i], "file-set", G_CALLBACK(on_media), mg);
        mg->media_clear[i] = gtk_button_new_from_icon_name("edit-clear-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(mg->media_clear[i], "Remove");
        g_object_set_data(G_OBJECT(mg->media_clear[i]), "slot", GINT_TO_POINTER(i));
        g_signal_connect(mg->media_clear[i], "clicked", G_CALLBACK(on_media_clear), mg);
        gtk_grid_attach(GTK_GRID(grid), mg->media_label[i], 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), mg->media_file[i],  1, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), mg->media_clear[i], 2, i, 1, 1);
    }
    gtk_container_add(GTK_CONTAINER(mg->media_section), grid);
    gtk_container_add(GTK_CONTAINER(box), mg->media_section);

    gtk_container_add(GTK_CONTAINER(box), section("Devices"));
    mg->devices_default = dim(wrap_label());
    gtk_container_add(GTK_CONTAINER(box), mg->devices_default);
    mg->devices_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(box), mg->devices_box);

    gtk_container_add(GTK_CONTAINER(box), section("Options"));
    GtkWidget *og = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(og), 6);
    gtk_grid_set_column_spacing(GTK_GRID(og), 8);
    GtkWidget *l = gtk_label_new("Display");
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    mg->display = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mg->display), "default", "Machine default");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mg->display), "sdl", "SDL window");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mg->display), "gtk", "GTK window");
    g_signal_connect(mg->display, "changed", G_CALLBACK(on_display), mg);
    gtk_grid_attach(GTK_GRID(og), l, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(og), mg->display, 1, 0, 1, 1);
    l = gtk_label_new("Extra arguments");
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    mg->extra = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(mg->extra), "e.g. -m 32K -monitor stdio");
    gtk_widget_set_hexpand(mg->extra, TRUE);
    g_signal_connect(mg->extra, "changed", G_CALLBACK(on_extra), mg);
    gtk_grid_attach(GTK_GRID(og), l, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(og), mg->extra, 1, 1, 1, 1);
    gtk_container_add(GTK_CONTAINER(box), og);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), box);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    return scroll;
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
    if (!mg->closing) gtk_widget_destroy(mg->window);
    return G_SOURCE_CONTINUE;
}
#endif

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
                   .argv = g_ptr_array_new_with_free_func(g_free),
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

    mg.header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(mg.header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(mg.header), "GEMU Machine Manager");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(mg.header), "v" GEMU_VERSION_STR);
    GtkWidget *add = gtk_button_new_with_mnemonic("_New");
    gtk_widget_set_tooltip_text(add, "Create a machine (Ctrl+N)");
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(mg.window), accel);
    gtk_widget_add_accelerator(add, "clicked", accel, GDK_KEY_n, GDK_CONTROL_MASK, 0);
    g_signal_connect(add, "clicked", G_CALLBACK(on_new), &mg);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(mg.header), add);
    mg.start = gtk_button_new_with_mnemonic("_Start");
    gtk_style_context_add_class(gtk_widget_get_style_context(mg.start), "suggested-action");
    g_signal_connect(mg.start, "clicked", G_CALLBACK(on_start), &mg);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(mg.header), mg.start);
    mg.delete = gtk_button_new_from_icon_name("user-trash-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(mg.delete, "Delete this machine");
    g_signal_connect(mg.delete, "clicked", G_CALLBACK(on_delete), &mg);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(mg.header), mg.delete);
    gtk_window_set_titlebar(GTK_WINDOW(mg.window), mg.header);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);
    gtk_container_add(GTK_CONTAINER(body), build_list(&mg));
    gtk_container_add(GTK_CONTAINER(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(right, TRUE);
    mg.empty = build_empty(&mg);
    gtk_widget_set_vexpand(mg.empty, TRUE);
    gtk_container_add(GTK_CONTAINER(right), mg.empty);
    mg.form = build_form(&mg);
    gtk_container_add(GTK_CONTAINER(right), mg.form);
    gtk_container_add(GTK_CONTAINER(body), right);
    gtk_container_add(GTK_CONTAINER(outer), body);

    /* the command line the settings add up to */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 8);
    gtk_container_add(GTK_CONTAINER(bar), dim(gtk_label_new("Command line:")));
    mg.cmdline = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(mg.cmdline), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(mg.cmdline), PANGO_ELLIPSIZE_START);
    gtk_label_set_xalign(GTK_LABEL(mg.cmdline), 0);
    gtk_widget_set_hexpand(mg.cmdline, TRUE);
    PangoAttrList *mono = pango_attr_list_new();
    pango_attr_list_insert(mono, pango_attr_family_new("monospace"));
    gtk_label_set_attributes(GTK_LABEL(mg.cmdline), mono);
    pango_attr_list_unref(mono);
    gtk_container_add(GTK_CONTAINER(bar), mg.cmdline);
    gtk_container_add(GTK_CONTAINER(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_container_add(GTK_CONTAINER(outer), bar);
    gtk_container_add(GTK_CONTAINER(mg.window), outer);

    gtk_widget_show_all(mg.window);
    show_vm(&mg);

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
