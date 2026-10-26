/*
 * Debug > Game Genie for GTK builds (see cheat_dialog.h): a GtkTreeView of
 * the codes with an On checkbox column, and Name / Code / Address / Value /
 * Compare entries that encode and decode as you type, with Add / Update /
 * Delete - the same window cheat_dialog_sdl.c draws for SDL.
 */
#ifdef GEMU_GTK
#include "cheat_dialog.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { COL_ON, COL_CODE, COL_NAME, COL_PATCH, N_COLS };
enum { F_NAME, F_CODE, F_ADDR, F_VAL, F_CMP, N_FIELDS };

typedef struct {
    CheatDialog   base;
    NesCheats    *cheats;
    unsigned      seen_version;
    GtkWidget    *window, *view, *status;
    GtkWidget    *field[N_FIELDS];
    GtkListStore *store;
    bool          syncing;      /* filling entries ourselves: don't re-encode */
} GtkCheatDialog;

static int selected(GtkCheatDialog *d) {
    GtkTreeModel *model;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) return -1;
    GtkTreePath *path = gtk_tree_model_get_path(model, &it);
    int i = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    return i;
}

static void set_status(GtkCheatDialog *d, const char *msg) {
    gtk_label_set_text(GTK_LABEL(d->status), msg);
}

static void reload(GtkCheatDialog *d) {
    NesCheats *c = d->cheats;
    int keep = selected(d);
    gtk_list_store_clear(d->store);
    for (int i = 0; i < c->n; i++) {
        uint16_t a; uint8_t v, k; bool h;
        char patch[48] = "";
        if (nes_gg_decode(c->list[i].code, &a, &v, &k, &h)) {
            if (h) snprintf(patch, sizeof(patch), "$%04X = $%02X if $%02X", a, v, k);
            else   snprintf(patch, sizeof(patch), "$%04X = $%02X", a, v);
        }
        GtkTreeIter it;
        gtk_list_store_append(d->store, &it);
        gtk_list_store_set(d->store, &it, COL_ON, c->list[i].enabled, COL_CODE, c->list[i].code,
                           COL_NAME, c->list[i].name, COL_PATCH, patch, -1);
    }
    if (keep >= 0 && keep < c->n) {
        GtkTreePath *path = gtk_tree_path_new_from_indices(keep, -1);
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view)), path);
        gtk_tree_path_free(path);
    }
    d->seen_version = c->version;
}

static void set_field(GtkCheatDialog *d, int f, const char *text) {
    d->syncing = true;
    gtk_entry_set_text(GTK_ENTRY(d->field[f]), text);
    d->syncing = false;
}

static void on_code_changed(GtkEditable *e, gpointer ud) {
    (void)e;
    GtkCheatDialog *d = ud;
    if (d->syncing) return;
    uint16_t a; uint8_t v, k; bool h;
    if (!nes_gg_decode(gtk_entry_get_text(GTK_ENTRY(d->field[F_CODE])), &a, &v, &k, &h)) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%04X", a); set_field(d, F_ADDR, buf);
    snprintf(buf, sizeof(buf), "%02X", v); set_field(d, F_VAL, buf);
    if (h) snprintf(buf, sizeof(buf), "%02X", k); else buf[0] = '\0';
    set_field(d, F_CMP, buf);
}

static void on_patch_changed(GtkEditable *e, gpointer ud) {
    (void)e;
    GtkCheatDialog *d = ud;
    if (d->syncing) return;
    const char *as = gtk_entry_get_text(GTK_ENTRY(d->field[F_ADDR]));
    const char *vs = gtk_entry_get_text(GTK_ENTRY(d->field[F_VAL]));
    const char *cs = gtk_entry_get_text(GTK_ENTRY(d->field[F_CMP]));
    if (!*as || !*vs) return;
    unsigned long a = strtoul(as, NULL, 16), v = strtoul(vs, NULL, 16);
    if (a < 0x8000 || a > 0xFFFF) { set_status(d, "Game Genie codes patch $8000-$FFFF"); return; }
    char code[9];
    if (nes_gg_encode((uint16_t)a, (uint8_t)v, *cs ? (int)strtoul(cs, NULL, 16) : -1, code))
        set_field(d, F_CODE, code);
}

static void on_selection(GtkTreeSelection *sel, gpointer ud) {
    (void)sel;
    GtkCheatDialog *d = ud;
    int i = selected(d);
    if (i < 0 || i >= d->cheats->n) return;
    set_field(d, F_NAME, d->cheats->list[i].name);
    gtk_entry_set_text(GTK_ENTRY(d->field[F_CODE]), d->cheats->list[i].code);   /* decodes */
}

static void on_toggled(GtkCellRendererToggle *cell, gchar *path, gpointer ud) {
    (void)cell;
    GtkCheatDialog *d = ud;
    int i = atoi(path);
    if (i >= 0 && i < d->cheats->n)
        nes_cheats_set_enabled(d->cheats, i, !d->cheats->list[i].enabled);
    reload(d);
}

static void clear_fields(GtkCheatDialog *d) {
    for (int f = 0; f < N_FIELDS; f++) set_field(d, f, "");
}

/* The code to add/update: the Code entry, or a code typed into Name. */
static bool code_to_use(GtkCheatDialog *d) {
    uint16_t a; uint8_t v, k; bool h;
    if (*gtk_entry_get_text(GTK_ENTRY(d->field[F_CODE]))) return true;
    const char *name = gtk_entry_get_text(GTK_ENTRY(d->field[F_NAME]));
    if (nes_gg_decode(name, &a, &v, &k, &h)) {
        char code[9];
        snprintf(code, sizeof(code), "%s", name);
        for (char *p = code; *p; p++) *p = (char)g_ascii_toupper(*p);
        set_field(d, F_NAME, "");
        gtk_entry_set_text(GTK_ENTRY(d->field[F_CODE]), code);   /* decodes */
        return true;
    }
    set_status(d, "Type a Game Genie code in the Code field");
    gtk_widget_grab_focus(d->field[F_CODE]);
    return false;
}

static void on_add(GtkButton *b, gpointer ud) {
    (void)b;
    GtkCheatDialog *d = ud;
    char err[160];
    if (!code_to_use(d)) return;
    if (nes_cheats_add(d->cheats, gtk_entry_get_text(GTK_ENTRY(d->field[F_CODE])),
                       gtk_entry_get_text(GTK_ENTRY(d->field[F_NAME])), true, err, sizeof(err))) {
        gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view)));
        reload(d);
        clear_fields(d);
        set_status(d, "Added and turned on");
        gtk_widget_grab_focus(d->field[F_CODE]);
    } else {
        set_status(d, err);
    }
}

static void on_update(GtkButton *b, gpointer ud) {
    (void)b;
    GtkCheatDialog *d = ud;
    char err[160];
    if (!code_to_use(d)) return;
    if (nes_cheats_update(d->cheats, selected(d), gtk_entry_get_text(GTK_ENTRY(d->field[F_CODE])),
                          gtk_entry_get_text(GTK_ENTRY(d->field[F_NAME])), err, sizeof(err))) {
        reload(d);
        set_status(d, "Updated");
    } else {
        set_status(d, err);
    }
}

static void on_delete(GtkButton *b, gpointer ud) {
    (void)b;
    GtkCheatDialog *d = ud;
    int i = selected(d);
    if (i < 0) { set_status(d, "Pick a code to delete"); return; }
    nes_cheats_remove(d->cheats, i);
    reload(d);
    clear_fields(d);
    set_status(d, "Deleted");
}

/* Enter in an entry: Update the picked code, or Add a new one. */
static void on_activate(GtkEntry *e, gpointer ud) {
    (void)e;
    GtkCheatDialog *d = ud;
    if (selected(d) >= 0) on_update(NULL, d);
    else on_add(NULL, d);
}

static gboolean on_delete_event(GtkWidget *w, GdkEvent *ev, gpointer ud) {
    (void)ev; (void)ud;
    gtk_widget_hide(w);
    return TRUE;
}

static void gtk_show(CheatDialog *base) {
    GtkCheatDialog *d = (GtkCheatDialog *)base;
    reload(d);
    gtk_widget_show_all(d->window);
    gtk_window_present(GTK_WINDOW(d->window));
    gtk_widget_grab_focus(d->field[F_CODE]);
}

static void gtk_hide(CheatDialog *base) {
    gtk_widget_hide(((GtkCheatDialog *)base)->window);
}

static bool gtk_visible(const CheatDialog *base) {
    return gtk_widget_get_visible(((const GtkCheatDialog *)base)->window);
}

static void gtk_refresh(CheatDialog *base) {
    GtkCheatDialog *d = (GtkCheatDialog *)base;
    if (gtk_visible(base) && d->seen_version != d->cheats->version) reload(d);  /* monitor edits */
}

static void gtk_destroy(CheatDialog *base) {
    GtkCheatDialog *d = (GtkCheatDialog *)base;
    if (!d) return;
    gtk_widget_destroy(d->window);
    g_object_unref(d->store);
    free(d);
}

static GtkWidget *entry(GtkCheatDialog *d, int f, int chars) {
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(e), chars);
    gtk_entry_set_max_length(GTK_ENTRY(e), f == F_NAME ? 60 : f == F_CODE ? 8 : f == F_ADDR ? 4 : 2);
    g_signal_connect(e, "activate", G_CALLBACK(on_activate), d);
    d->field[f] = e;
    return e;
}

CheatDialog *cheat_dialog_gtk_create(NesCheats *cheats) {
    GtkCheatDialog *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->base = (CheatDialog){ gtk_show, gtk_hide, gtk_visible, gtk_refresh, gtk_destroy };
    d->cheats = cheats;

    d->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(d->window), "GEMU - Game Genie");
    gtk_window_set_default_size(GTK_WINDOW(d->window), 560, 420);
    g_signal_connect(d->window, "delete-event", G_CALLBACK(on_delete_event), d);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(d->window), box);

    /* the codes */
    d->store = gtk_list_store_new(N_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    d->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->store));
    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
    g_signal_connect(toggle, "toggled", G_CALLBACK(on_toggled), d);
    gtk_tree_view_append_column(GTK_TREE_VIEW(d->view),
        gtk_tree_view_column_new_with_attributes("On", toggle, "active", COL_ON, NULL));
    static const char *titles[] = { NULL, "Code", "Name", "Patch" };
    for (int c = COL_CODE; c < N_COLS; c++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            titles[c], gtk_cell_renderer_text_new(), "text", c, NULL);
        gtk_tree_view_column_set_expand(col, c == COL_NAME);
        gtk_tree_view_append_column(GTK_TREE_VIEW(d->view), col);
    }
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view)), "changed",
                     G_CALLBACK(on_selection), d);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), d->view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    /* fields */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Name:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry(d, F_NAME, 40), 1, 0, 7, 1);
    static const char *labels[] = { NULL, "Code:", "Address:", "Value:", "Compare:" };
    static const int chars[] = { 0, 9, 5, 3, 3 };
    for (int f = F_CODE; f < N_FIELDS; f++) {
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new(labels[f]), (f - F_CODE) * 2, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), entry(d, f, chars[f]), (f - F_CODE) * 2 + 1, 1, 1, 1);
    }
    g_signal_connect(d->field[F_CODE], "changed", G_CALLBACK(on_code_changed), d);
    for (int f = F_ADDR; f < N_FIELDS; f++)
        g_signal_connect(d->field[f], "changed", G_CALLBACK(on_patch_changed), d);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    /* buttons */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    struct { const char *label; GCallback cb; } buttons[] = {
        { "_Add", G_CALLBACK(on_add) }, { "_Update", G_CALLBACK(on_update) },
        { "_Delete", G_CALLBACK(on_delete) },
    };
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        GtkWidget *b = gtk_button_new_with_mnemonic(buttons[i].label);
        g_signal_connect(b, "clicked", buttons[i].cb, d);
        gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    }
    GtkWidget *close = gtk_button_new_with_mnemonic("_Close");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_hide), d->window);
    gtk_box_pack_end(GTK_BOX(row), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    d->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(d->status), 0);
    gtk_box_pack_start(GTK_BOX(box), d->status, FALSE, FALSE, 0);
    return &d->base;
}
#endif /* GEMU_GTK */
