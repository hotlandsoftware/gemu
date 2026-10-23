#ifdef GEMU_GTK
#include "gemu/gtk_menu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The GTK front end for the window menu model: the same menus the SDL
 * backend draws itself (gemu_menubar_sdl.c), as native GTK widgets. */

typedef struct {
    GemuMenu *(*build)(void *ud);
    void      *ud;
    GtkWidget *bar;
    char       titles[512];   /* top-level titles currently shown */
} BarCtx;

/* Copied out of the model so the model can be freed right after building. */
typedef struct {
    void (*activate)(void *ud, int arg);
    void  *ud;
    int    arg;
} ItemCb;

static void on_item_activate(GtkMenuItem *w, gpointer p) {
    (void)w;
    ItemCb *cb = p;
    if (cb->activate) cb->activate(cb->ud, cb->arg);
}

static void populate(GtkWidget *shell, const GemuMenu *m) {
    for (int i = 0; i < m->n; i++) {
        const GemuMenuItem *it = &m->items[i];
        GtkWidget *w;
        switch (it->type) {
        case GEMU_MENU_SEPARATOR:
            w = gtk_separator_menu_item_new();
            break;
        case GEMU_MENU_CHECK:
        case GEMU_MENU_RADIO:
            w = gtk_check_menu_item_new_with_label(it->label);
            gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(w),
                                                  it->type == GEMU_MENU_RADIO);
            /* Before connecting: set_active emits "activate". */
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w),
                                           gemu_menu_item_checked(it));
            break;
        default:
            w = gtk_menu_item_new_with_label(it->label);
            break;
        }
        if (it->type == GEMU_MENU_SUBMENU) {
            GtkWidget *sub = gtk_menu_new();
            populate(sub, it->submenu);
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(w), sub);
        } else if (it->type != GEMU_MENU_SEPARATOR && it->activate) {
            ItemCb *cb = malloc(sizeof(*cb));
            if (cb) {
                *cb = (ItemCb){ it->activate, it->ud, it->arg };
                g_signal_connect_data(w, "activate", G_CALLBACK(on_item_activate),
                                      cb, (GClosureNotify)(void (*)(void))free, 0);
            }
        }
        if (it->shortcut[0]) {
            GtkWidget *label = gtk_bin_get_child(GTK_BIN(w));
            char text[160];
            snprintf(text, sizeof(text), "%s\t%s", it->label, it->shortcut);
            if (GTK_IS_LABEL(label)) gtk_label_set_text(GTK_LABEL(label), text);
        }
        if (it->type != GEMU_MENU_SEPARATOR)
            gtk_widget_set_sensitive(w, gemu_menu_item_enabled(it));
        gtk_menu_shell_append(GTK_MENU_SHELL(shell), w);
    }
    gtk_widget_show_all(shell);
}

static void clear(GtkWidget *shell) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(shell));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(children);
}

/* A top-level menu is about to open: fill it from a fresh model. */
static void on_top_select(GtkMenuItem *top, gpointer p) {
    BarCtx *ctx = p;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(top), "gemu-index"));
    GtkWidget *menu = gtk_menu_item_get_submenu(top);
    GemuMenu *model = ctx->build(ctx->ud);
    if (!model) return;
    if (menu && index < model->n && model->items[index].submenu) {
        clear(menu);
        populate(menu, model->items[index].submenu);
    }
    gemu_menu_free(model);
}

static void rebuild_titles(BarCtx *ctx, const GemuMenu *model, const char *titles) {
    clear(ctx->bar);
    for (int i = 0; i < model->n; i++) {
        if (model->items[i].type != GEMU_MENU_SUBMENU) continue;
        GtkWidget *top = gtk_menu_item_new_with_label(model->items[i].label);
        GtkWidget *menu = gtk_menu_new();
        populate(menu, model->items[i].submenu);
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
        g_object_set_data(G_OBJECT(top), "gemu-index", GINT_TO_POINTER(i));
        g_signal_connect(top, "select", G_CALLBACK(on_top_select), ctx);
        gtk_menu_shell_append(GTK_MENU_SHELL(ctx->bar), top);
    }
    gtk_widget_show_all(ctx->bar);
    snprintf(ctx->titles, sizeof(ctx->titles), "%s", titles);
}

void gemu_gtk_menubar_refresh(GtkWidget *bar) {
    BarCtx *ctx = bar ? g_object_get_data(G_OBJECT(bar), "gemu-menubar") : NULL;
    if (!ctx || gtk_menu_shell_get_selected_item(GTK_MENU_SHELL(bar))) return;
    GemuMenu *model = ctx->build(ctx->ud);
    if (!model) return;
    char titles[512] = "";
    for (int i = 0; i < model->n; i++) {
        size_t len = strlen(titles);
        snprintf(titles + len, sizeof(titles) - len, "%s\n", model->items[i].label);
    }
    if (strcmp(titles, ctx->titles) != 0) rebuild_titles(ctx, model, titles);
    gemu_menu_free(model);
}

GtkWidget *gemu_gtk_menubar_new(GemuMenu *(*build)(void *ud), void *ud) {
    BarCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->build = build;
    ctx->ud = ud;
    ctx->bar = gtk_menu_bar_new();
    g_object_set_data_full(G_OBJECT(ctx->bar), "gemu-menubar", ctx, free);
    gemu_gtk_menubar_refresh(ctx->bar);
    return ctx->bar;
}
#endif /* GEMU_GTK */
