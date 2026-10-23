#include "gemu/menu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GemuMenu *gemu_menu_new(const char *title) {
    GemuMenu *m = calloc(1, sizeof(*m));
    if (m) snprintf(m->title, sizeof(m->title), "%s", title ? title : "");
    return m;
}

void gemu_menu_free(GemuMenu *m) {
    if (!m) return;
    for (int i = 0; i < m->n; i++) gemu_menu_free(m->items[i].submenu);
    free(m->items);
    free(m);
}

GemuMenuItem *gemu_menu_add(GemuMenu *m, GemuMenuItemType type, const char *label,
                            void (*activate)(void *ud, int arg), void *ud, int arg) {
    if (!m) return NULL;
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 8;
        GemuMenuItem *items = realloc(m->items, (size_t)cap * sizeof(*items));
        if (!items) return NULL;
        m->items = items;
        m->cap = cap;
    }
    GemuMenuItem *it = &m->items[m->n++];
    memset(it, 0, sizeof(*it));
    it->type = type;
    snprintf(it->label, sizeof(it->label), "%s", label ? label : "");
    it->activate = activate;
    it->ud = ud;
    it->arg = arg;
    return it;
}

void gemu_menu_add_separator(GemuMenu *m) {
    gemu_menu_add(m, GEMU_MENU_SEPARATOR, "", NULL, NULL, 0);
}

GemuMenu *gemu_menu_add_submenu(GemuMenu *m, const char *label) {
    GemuMenuItem *it = gemu_menu_add(m, GEMU_MENU_SUBMENU, label, NULL, NULL, 0);
    if (!it) return NULL;
    it->submenu = gemu_menu_new(label);
    return it->submenu;
}

bool gemu_menu_item_enabled(const GemuMenuItem *it) {
    if (!it || it->type == GEMU_MENU_SEPARATOR) return false;
    if (it->type == GEMU_MENU_SUBMENU) return it->submenu && it->submenu->n > 0;
    return !it->enabled || it->enabled(it->ud, it->arg);
}

bool gemu_menu_item_checked(const GemuMenuItem *it) {
    return it && it->checked && it->checked(it->ud, it->arg);
}

void gemu_menu_item_activate(const GemuMenuItem *it) {
    if (it && it->activate && gemu_menu_item_enabled(it)) it->activate(it->ud, it->arg);
}
