#ifdef GEMU_GTK
#include "hex_editor.h"
#include "../hardware/nes.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ── Per-tab state ──────────────────────────────────────────────────────── */

typedef enum {
    HEX_TAB_RAM = 0,
    HEX_TAB_PRG,
    HEX_TAB_CHR,
    HEX_TAB_COUNT
} HexTabId;

typedef struct {
    HexTabId       id;
    const char    *label;
    GtkWidget     *scrolled;
    GtkWidget     *text_view;
    GtkTextBuffer *buf;
    GtkTextTag    *hl_tag;       /* highlight for selected byte */
    const uint8_t *data;         /* live pointer, or NULL */
    size_t         size;         /* bytes */
    bool           read_only;
    uint32_t       base_addr;    /* displayed starting address */
} HexEditorTab;

struct HexEditor {
    GtkWidget     *window;
    GtkWidget     *notebook;
    GtkWidget     *addr_entry;
    GtkWidget     *value_entry;
    GtkWidget     *write_btn;
    GtkWidget     *status_label;
    GtkWidget     *goto_entry;
    NesState      *nes;
    bool           visible;
    int            refresh_skip;    /* rebuild only every other frame */
    HexEditorTab   tabs[HEX_TAB_COUNT];
    HexEditorTab  *cur_tab;
    int            cur_page;
    uint32_t       selected_addr;   /* offset into current tab's data */
    bool           has_selection;
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Standard hex line: "XXXX: XX XX ... XX  AAAA...\n"
 *   cols: 0-3=addr  4=':'  5=' '  6-52=hex(16*3-1)  53=' '  54=' '  55-70=ASCII  */
#define HEX_LINE_BYTES   16
#define HEX_LINE_CHARS   71   /* 71 printed chars + '\n' → 72 with newline */
#define HEX_ADDR_COL     0
#define HEX_SEP_COL1     4
#define HEX_SEP_COL2     5
#define HEX_DATA_START   6
#define HEX_ASCII_START  (HEX_DATA_START + HEX_LINE_BYTES * 3 - 1)

/* byte index → starting column in the hex area */
static int hex_byte_col(int i) {
    return HEX_DATA_START + i * 3;
}

/* Given a text offset into the buffer, return the line number and column. */
static void text_offset_to_line_col(GtkTextBuffer *buf, int offset,
                                     int *line, int *col) {
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_offset(buf, &iter, offset);
    *line = gtk_text_iter_get_line(&iter);
    *col  = gtk_text_iter_get_line_offset(&iter);
}

/* Build a formatted hex line for `buf`.  'ascii' must be 16 chars. */
static void append_hex_line(GtkTextBuffer *buf, uint32_t addr,
                             const uint8_t *bytes, int n) {
    char line[128];
    char *p   = line;
    p += snprintf(p, sizeof(line) - (size_t)(p - line), "%04X: ", addr);
    for (int i = 0; i < HEX_LINE_BYTES; i++) {
        if (i < n) {
            p += snprintf(p, sizeof(line) - (size_t)(p - line),
                          "%02X%s", bytes[i], (i == 7) ? "  " : " ");
        } else {
            p += snprintf(p, sizeof(line) - (size_t)(p - line),
                          "  %s", (i == 7) ? " " : " ");
        }
    }
    /* Remove the trailing space after last hex byte, keep the separator */
    if (p > line && p[-1] == ' ')
        p--;

    /* ASCII column */
    p += snprintf(p, sizeof(line) - (size_t)(p - line), " ");
    for (int i = 0; i < HEX_LINE_BYTES; i++) {
        char c = (i < n && bytes[i] >= 32 && bytes[i] < 127) ? (char)bytes[i] : '.';
        *p++ = c;
    }
    *p++ = '\n';
    *p   = '\0';

    gtk_text_buffer_insert_at_cursor(buf, line, -1);
}

/* Rebuild the entire text buffer for a tab from its live data. */
static void hex_tab_rebuild(HexEditorTab *tab) {
    if (!tab->buf || !tab->data || tab->size == 0) {
        gtk_text_buffer_set_text(tab->buf, "(no data)\n", -1);
        return;
    }

    gtk_text_buffer_set_text(tab->buf, "", 0);

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(tab->buf, &end);

    size_t remain = tab->size;
    uint32_t addr = tab->base_addr;
    for (size_t off = 0; off < tab->size; off += HEX_LINE_BYTES, addr += HEX_LINE_BYTES) {
        int n = (int)(remain < HEX_LINE_BYTES ? remain : HEX_LINE_BYTES);
        append_hex_line(tab->buf, addr, tab->data + off, n);
        remain -= (size_t)n;
    }

    /* Apply monospace tag to the whole buffer */
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(tab->buf, &start);
    gtk_text_buffer_get_end_iter(tab->buf, &end);
}

/* Remove highlight from all bytes in a tab. */
static void hex_tab_clear_highlight(HexEditorTab *tab) {
    if (!tab->buf || !tab->hl_tag) return;
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(tab->buf, &s);
    gtk_text_buffer_get_end_iter(tab->buf, &e);
    gtk_text_buffer_remove_tag(tab->buf, tab->hl_tag, &s, &e);
}

/* Highlight a single byte at the given line & byte index. */
static void hex_tab_highlight_byte(HexEditorTab *tab, int line, int byte_idx) {
    if (!tab->buf || !tab->hl_tag) return;
    hex_tab_clear_highlight(tab);

    GtkTextIter s;
    gtk_text_buffer_get_iter_at_line_offset(tab->buf, &s, line,
                                            hex_byte_col(byte_idx));
    GtkTextIter e = s;
    gtk_text_iter_forward_chars(&e, 2);  /* two hex digits */

    gtk_text_buffer_apply_tag(tab->buf, tab->hl_tag, &s, &e);
}

/* Given a text offset in the buffer, determine which byte was clicked.
 * Returns true and sets *byte_idx and *addr if a valid hex byte was clicked. */
static bool hex_tab_byte_at_pos(HexEditorTab *tab, int offset,
                                 int *byte_idx, uint32_t *addr) {
    int line, col;
    text_offset_to_line_col(tab->buf, offset, &line, &col);

    /* Check if click is in the hex data area */
    if (col < HEX_DATA_START) return false;

    int bi = (col - HEX_DATA_START) / 3;
    int ci = (col - HEX_DATA_START) % 3;
    if (bi >= HEX_LINE_BYTES || ci >= 2) return false;

    size_t data_off = (size_t)line * HEX_LINE_BYTES + (size_t)bi;
    if (data_off >= tab->size) return false;

    *byte_idx = bi;
    *addr     = tab->base_addr + (uint32_t)data_off;
    return true;
}

/* Update the address/value entries to reflect the selected byte. */
static void hex_update_entries(HexEditor *he, uint32_t addr, uint8_t val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%04X", addr);
    gtk_entry_set_text(GTK_ENTRY(he->addr_entry), buf);
    snprintf(buf, sizeof(buf), "%02X", val);
    gtk_entry_set_text(GTK_ENTRY(he->value_entry), buf);
}

/* Read current byte value from the active tab's data at the selected address. */
static uint8_t hex_read_byte(const HexEditor *he) {
    if (!he->cur_tab || !he->cur_tab->data) return 0;
    uint32_t off = he->selected_addr - he->cur_tab->base_addr;
    if (off >= he->cur_tab->size) return 0;
    return he->cur_tab->data[off];
}

/* Write a byte to the active tab's data (only if not read-only). */
static bool hex_write_byte(HexEditor *he, uint32_t addr, uint8_t val) {
    if (!he->cur_tab || he->cur_tab->read_only || !he->cur_tab->data)
        return false;
    uint32_t off = addr - he->cur_tab->base_addr;
    if (off >= he->cur_tab->size) return false;

    /* Direct write to NES RAM through the pointer.
     * For CPU RAM ($0000-$07FF), write to s->ram directly.
     * The pointer already points to the right buffer. */
    ((uint8_t *)he->cur_tab->data)[off] = val;
    return true;
}

/* Select a byte: highlight it, update entries. */
static void hex_select_byte(HexEditor *he, int line, int byte_idx, uint32_t addr) {
    he->selected_addr = addr;
    he->has_selection = true;
    if (he->cur_tab)
        hex_tab_highlight_byte(he->cur_tab, line, byte_idx);
    hex_update_entries(he, addr, hex_read_byte(he));

    /* Enable/disable write button based on read-only status */
    gtk_widget_set_sensitive(he->write_btn,
                             he->cur_tab && !he->cur_tab->read_only);

    char status[64];
    snprintf(status, sizeof(status), "Selected $%04X", addr);
    gtk_label_set_text(GTK_LABEL(he->status_label), status);
}

/* ── Tab switching ───────────────────────────────────────────────────────── */

static void hex_update_cur_tab(HexEditor *he) {
    int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(he->notebook));
    if (page < 0 || page >= HEX_TAB_COUNT) {
        he->cur_tab = NULL;
        return;
    }
    he->cur_page = page;
    he->cur_tab  = &he->tabs[page];
    he->has_selection = false;
    gtk_widget_set_sensitive(he->write_btn,
                             he->cur_tab && !he->cur_tab->read_only);
    gtk_label_set_text(GTK_LABEL(he->status_label), "");
}

/* ── Callbacks ───────────────────────────────────────────────────────────── */

static gboolean on_text_click(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    HexEditor *he = ud;
    HexEditorTab *tab = he->cur_tab;
    if (!tab || !tab->buf) return FALSE;

    /* Convert window coords to buffer position */
    gint bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(w),
        GTK_TEXT_WINDOW_WIDGET, (gint)ev->x, (gint)ev->y, &bx, &by);

    GtkTextIter iter;
    gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(w), &iter, bx, by);

    int byte_idx;
    uint32_t addr;
    int line = gtk_text_iter_get_line(&iter);

    if (hex_tab_byte_at_pos(tab, gtk_text_iter_get_offset(&iter), &byte_idx, &addr)) {
        hex_select_byte(he, line, byte_idx, addr);
    }

    return FALSE;
}

static void on_page_switched(GtkNotebook *nb, GtkWidget *page, guint page_num,
                              gpointer ud) {
    (void)nb; (void)page; (void)page_num;
    HexEditor *he = ud;
    hex_update_cur_tab(he);
}

static void on_write_clicked(GtkButton *btn, gpointer ud) {
    (void)btn;
    HexEditor *he = ud;
    if (!he->has_selection || !he->cur_tab || he->cur_tab->read_only) return;

    const char *text = gtk_entry_get_text(GTK_ENTRY(he->value_entry));
    uint32_t val = (uint32_t)strtoul(text, NULL, 16);
    if (val > 0xFF) {
        gtk_label_set_text(GTK_LABEL(he->status_label), "Invalid hex value");
        return;
    }

    if (hex_write_byte(he, he->selected_addr, (uint8_t)val)) {
        char status[64];
        snprintf(status, sizeof(status), "Wrote $%02X to $%04X",
                 (uint8_t)val, he->selected_addr);
        gtk_label_set_text(GTK_LABEL(he->status_label), status);

        /* Refresh the display to reflect the change */
        if (he->cur_tab)
            hex_tab_rebuild(he->cur_tab);

        /* Re-select the byte */
        uint32_t off = he->selected_addr - he->cur_tab->base_addr;
        int line = (int)(off / HEX_LINE_BYTES);
        int bi   = (int)(off % HEX_LINE_BYTES);
        hex_tab_highlight_byte(he->cur_tab, line, bi);
    }
}

static void on_value_activate(GtkEntry *entry, gpointer ud) {
    (void)entry;
    on_write_clicked(NULL, ud);
}

static void on_goto_activate(GtkEntry *entry, gpointer ud) {
    HexEditor *he = ud;
    const char *text = gtk_entry_get_text(entry);
    uint32_t addr = (uint32_t)strtoul(text, NULL, 16);

    if (!he->cur_tab || !he->cur_tab->data || he->cur_tab->size == 0) {
        gtk_label_set_text(GTK_LABEL(he->status_label), "No data to navigate");
        return;
    }

    uint32_t base = he->cur_tab->base_addr;
    uint32_t end  = base + (uint32_t)he->cur_tab->size;
    if (addr < base || addr >= end) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Address $%04X out of range ($%04X-$%04X)",
                 addr, base, end - 1);
        gtk_label_set_text(GTK_LABEL(he->status_label), msg);
        return;
    }

    /* Scroll to the line */
    uint32_t off = addr - base;
    int line = (int)(off / HEX_LINE_BYTES);
    int bi   = (int)(off % HEX_LINE_BYTES);

    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_line(he->cur_tab->buf, &iter, line);
    gtk_text_buffer_place_cursor(he->cur_tab->buf, &iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(he->cur_tab->text_view),
                                  &iter, 0.0, FALSE, 0.0, 0.0);

    hex_select_byte(he, line, bi, addr);
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    HexEditor *he = ud;
    (void)w;

    if (!he->has_selection || !he->cur_tab || !he->cur_tab->data)
        return FALSE;

    uint32_t off = he->selected_addr - he->cur_tab->base_addr;

    switch (ev->keyval) {
    case GDK_KEY_Up: {
        if (off < HEX_LINE_BYTES) return TRUE;
        uint32_t new_addr = he->selected_addr - HEX_LINE_BYTES;
        off = new_addr - he->cur_tab->base_addr;
        int line = (int)(off / HEX_LINE_BYTES);
        int bi   = (int)(off % HEX_LINE_BYTES);
        hex_select_byte(he, line, bi, new_addr);
        /* Scroll if needed */
        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_line(he->cur_tab->buf, &iter, line);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(he->cur_tab->text_view),
                                      &iter, 0.0, FALSE, 0.0, 0.0);
        return TRUE;
    }
    case GDK_KEY_Down: {
        uint32_t new_addr = he->selected_addr + HEX_LINE_BYTES;
        if (new_addr >= he->cur_tab->base_addr + (uint32_t)he->cur_tab->size)
            return TRUE;
        off = new_addr - he->cur_tab->base_addr;
        int line = (int)(off / HEX_LINE_BYTES);
        int bi   = (int)(off % HEX_LINE_BYTES);
        hex_select_byte(he, line, bi, new_addr);
        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_line(he->cur_tab->buf, &iter, line);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(he->cur_tab->text_view),
                                      &iter, 0.0, FALSE, 0.0, 0.0);
        return TRUE;
    }
    case GDK_KEY_Left: {
        if (off == 0) return TRUE;
        uint32_t new_addr = he->selected_addr - 1;
        off = new_addr - he->cur_tab->base_addr;
        int line = (int)(off / HEX_LINE_BYTES);
        int bi   = (int)(off % HEX_LINE_BYTES);
        hex_select_byte(he, line, bi, new_addr);
        return TRUE;
    }
    case GDK_KEY_Right: {
        uint32_t new_addr = he->selected_addr + 1;
        if (new_addr >= he->cur_tab->base_addr + (uint32_t)he->cur_tab->size)
            return TRUE;
        off = new_addr - he->cur_tab->base_addr;
        int line = (int)(off / HEX_LINE_BYTES);
        int bi   = (int)(off % HEX_LINE_BYTES);
        hex_select_byte(he, line, bi, new_addr);
        return TRUE;
    }
    default:
        return FALSE;
    }
}

static gboolean on_value_key_press(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    (void)w;

    /* Allow hex digits and navigation */
    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        on_write_clicked(NULL, ud);
        return TRUE;
    }

    /* Filter: only allow hex digits and control keys */
    if ((ev->keyval >= '0' && ev->keyval <= '9') ||
        (ev->keyval >= 'a' && ev->keyval <= 'f') ||
        (ev->keyval >= 'A' && ev->keyval <= 'F') ||
        ev->keyval == GDK_KEY_BackSpace || ev->keyval == GDK_KEY_Delete ||
        ev->keyval == GDK_KEY_Left || ev->keyval == GDK_KEY_Right ||
        ev->keyval == GDK_KEY_Home || ev->keyval == GDK_KEY_End ||
        ev->keyval == GDK_KEY_Tab ||
        (ev->state & GDK_CONTROL_MASK)) {
        return FALSE;  /* let GTK handle it */
    }
    return TRUE;  /* block non-hex keys */
}

static gboolean on_window_delete(GtkWidget *w, GdkEvent *ev, gpointer ud) {
    (void)w; (void)ev;
    HexEditor *he = ud;
    hex_editor_hide(he);
    return TRUE;
}

/* ── Tab setup ───────────────────────────────────────────────────────────── */

static GtkWidget *hex_create_tab_view(HexEditor *he, HexEditorTab *tab) {
    /* Scrolled window */
    tab->scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scrolled),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    /* Text view */
    tab->text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tab->text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tab->text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tab->text_view), GTK_WRAP_NONE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tab->text_view), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tab->text_view), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tab->text_view), 4);

    /* Buffer with highlight tag */
    tab->buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->text_view));
    tab->hl_tag = gtk_text_buffer_create_tag(tab->buf, "highlight",
        "background", "#FFFF00", "foreground", "#000000", NULL);

    gtk_container_add(GTK_CONTAINER(tab->scrolled), tab->text_view);

    /* Click handler */
    g_signal_connect(tab->text_view, "button-press-event",
                     G_CALLBACK(on_text_click), he);
    /* Key handler for arrow navigation */
    g_signal_connect(tab->text_view, "key-press-event",
                     G_CALLBACK(on_key_press), he);

    /* Enable key events on the text view */
    gtk_widget_add_events(tab->text_view, GDK_KEY_PRESS_MASK);

    return tab->scrolled;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

HexEditor *hex_editor_create(NesState *s) {
    if (!s) return NULL;

    /* Ensure GTK is initialized before creating any widgets. */
    if (!gtk_init_check(NULL, NULL)) return NULL;

    HexEditor *he = calloc(1, sizeof(*he));
    if (!he) return NULL;
    he->nes = s;
    he->visible = false;

    /* ── Window ── */
    he->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(he->window), "GEMU — Hex Editor");
    gtk_window_set_default_size(GTK_WINDOW(he->window), 660, 520);
    g_signal_connect(he->window, "delete-event",
                     G_CALLBACK(on_window_delete), he);

    /* ── Main vertical box ── */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(he->window), vbox);

    /* ── Notebook (tabs) ── */
    he->notebook = gtk_notebook_new();
    g_signal_connect(he->notebook, "switch-page",
                     G_CALLBACK(on_page_switched), he);
    gtk_box_pack_start(GTK_BOX(vbox), he->notebook, TRUE, TRUE, 0);

    /* ── Tab: CPU RAM ── */
    he->tabs[HEX_TAB_RAM] = (HexEditorTab){
        .id        = HEX_TAB_RAM,
        .label     = "CPU RAM ($0000-$07FF)",
        .data      = s->ram,
        .size      = sizeof(s->ram),
        .read_only = false,
        .base_addr = 0x0000,
    };
    hex_create_tab_view(he, &he->tabs[HEX_TAB_RAM]);
    hex_tab_rebuild(&he->tabs[HEX_TAB_RAM]);
    gtk_notebook_append_page(GTK_NOTEBOOK(he->notebook),
        he->tabs[HEX_TAB_RAM].scrolled,
        gtk_label_new(he->tabs[HEX_TAB_RAM].label));

    /* ── Tab: PRG ROM ── */
    he->tabs[HEX_TAB_PRG] = (HexEditorTab){
        .id        = HEX_TAB_PRG,
        .label     = "PRG ROM ($8000+)",
        .data      = s->prg,
        .size      = s->prg ? (size_t)s->cart.prg_banks * 0x4000u : 0,
        .read_only = true,
        .base_addr = 0x8000,
    };
    hex_create_tab_view(he, &he->tabs[HEX_TAB_PRG]);
    hex_tab_rebuild(&he->tabs[HEX_TAB_PRG]);
    gtk_notebook_append_page(GTK_NOTEBOOK(he->notebook),
        he->tabs[HEX_TAB_PRG].scrolled,
        gtk_label_new(he->tabs[HEX_TAB_PRG].label));

    /* ── Tab: CHR ROM/RAM ── */
    he->tabs[HEX_TAB_CHR] = (HexEditorTab){
        .id        = HEX_TAB_CHR,
        .label     = "CHR ROM/RAM",
        .data      = s->chr,
        .size      = s->chr ? (size_t)s->cart.chr_banks * 0x2000u : 0,
        .read_only = !s->chr_is_ram,
        .base_addr = 0x0000,
    };
    hex_create_tab_view(he, &he->tabs[HEX_TAB_CHR]);
    hex_tab_rebuild(&he->tabs[HEX_TAB_CHR]);
    gtk_notebook_append_page(GTK_NOTEBOOK(he->notebook),
        he->tabs[HEX_TAB_CHR].scrolled,
        gtk_label_new(he->tabs[HEX_TAB_CHR].label));

    /* ── Bottom control bar ── */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 4);

    /* Goto */
    GtkWidget *goto_label = gtk_label_new("Goto:");
    gtk_box_pack_start(GTK_BOX(hbox), goto_label, FALSE, FALSE, 0);
    he->goto_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(he->goto_entry), 6);
    gtk_entry_set_placeholder_text(GTK_ENTRY(he->goto_entry), "hex addr");
    g_signal_connect(he->goto_entry, "activate",
                     G_CALLBACK(on_goto_activate), he);
    gtk_box_pack_start(GTK_BOX(hbox), he->goto_entry, FALSE, FALSE, 0);

    /* Separator */
    gtk_box_pack_start(GTK_BOX(hbox),
        gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);

    /* Address */
    GtkWidget *addr_label = gtk_label_new("Addr:");
    gtk_box_pack_start(GTK_BOX(hbox), addr_label, FALSE, FALSE, 0);
    he->addr_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(he->addr_entry), 6);
    gtk_editable_set_editable(GTK_EDITABLE(he->addr_entry), FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), he->addr_entry, FALSE, FALSE, 0);

    /* Value */
    GtkWidget *val_label = gtk_label_new("Value:");
    gtk_box_pack_start(GTK_BOX(hbox), val_label, FALSE, FALSE, 0);
    he->value_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(he->value_entry), 4);
    gtk_entry_set_placeholder_text(GTK_ENTRY(he->value_entry), "00");
    gtk_entry_set_max_length(GTK_ENTRY(he->value_entry), 2);
    g_signal_connect(he->value_entry, "activate",
                     G_CALLBACK(on_value_activate), he);
    g_signal_connect(he->value_entry, "key-press-event",
                     G_CALLBACK(on_value_key_press), he);
    gtk_box_pack_start(GTK_BOX(hbox), he->value_entry, FALSE, FALSE, 0);

    /* Write button */
    he->write_btn = gtk_button_new_with_label("Write");
    gtk_widget_set_sensitive(he->write_btn, FALSE);
    g_signal_connect(he->write_btn, "clicked",
                     G_CALLBACK(on_write_clicked), he);
    gtk_box_pack_start(GTK_BOX(hbox), he->write_btn, FALSE, FALSE, 0);

    /* Spacer */
    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(""), TRUE, TRUE, 0);

    /* Status label */
    he->status_label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(hbox), he->status_label, FALSE, FALSE, 0);

    /* ── Initialize current tab ── */
    hex_update_cur_tab(he);

    return he;
}

void hex_editor_destroy(HexEditor *he) {
    if (!he) return;
    if (he->window)
        gtk_widget_destroy(he->window);
    free(he);
}

void hex_editor_show(HexEditor *he) {
    if (!he || he->visible) return;
    he->visible = true;

    /* Rebuild all tabs with fresh data */
    for (int i = 0; i < HEX_TAB_COUNT; i++) {
        if (he->tabs[i].data)
            hex_tab_rebuild(&he->tabs[i]);
    }

    gtk_widget_show_all(he->window);
    gtk_window_present(GTK_WINDOW(he->window));
}

void hex_editor_hide(HexEditor *he) {
    if (!he || !he->visible) return;
    he->visible = false;
    gtk_widget_hide(he->window);
}

bool hex_editor_is_visible(const HexEditor *he) {
    return he && he->visible;
}

void hex_editor_refresh(HexEditor *he) {
    if (!he || !he->visible) return;

    /* Only rebuild every other frame — halves the text-buffer rebuild cost
     * while keeping the display responsive at ~30 Hz visual refresh. */
    he->refresh_skip = !he->refresh_skip;
    if (he->refresh_skip) {
        /* Still restore highlight if we had a selection */
        if (he->has_selection && he->cur_tab) {
            HexEditorTab *sel_tab = he->cur_tab;
            uint32_t off = he->selected_addr - sel_tab->base_addr;
            if (off < sel_tab->size) {
                int line = (int)(off / HEX_LINE_BYTES);
                int bi   = (int)(off % HEX_LINE_BYTES);
                hex_tab_highlight_byte(sel_tab, line, bi);
            }
        }
        return;
    }

    /* Rebuild the RAM tab (always changes).  Save/restore scroll position
     * so the user's view doesn't jump around. */
    HexEditorTab *ram = &he->tabs[HEX_TAB_RAM];
    if (ram->data && ram->buf) {
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(ram->scrolled));
        double saved = vadj ? gtk_adjustment_get_value(vadj) : 0.0;
        hex_tab_rebuild(ram);
        if (vadj) gtk_adjustment_set_value(vadj, saved);
    }

    /* Rebuild CHR if it's RAM (writable) */
    HexEditorTab *chr = &he->tabs[HEX_TAB_CHR];
    if (chr->read_only == false && chr->data && chr->buf) {
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(chr->scrolled));
        double saved = vadj ? gtk_adjustment_get_value(vadj) : 0.0;
        hex_tab_rebuild(chr);
        if (vadj) gtk_adjustment_set_value(vadj, saved);
    }

    /* PRG ROM doesn't change — no rebuild needed */

    /* Re-apply selection highlight if there was one */
    if (he->has_selection && he->cur_tab) {
        HexEditorTab *sel_tab = he->cur_tab;
        uint32_t off = he->selected_addr - sel_tab->base_addr;
        if (off < sel_tab->size) {
            int line = (int)(off / HEX_LINE_BYTES);
            int bi   = (int)(off % HEX_LINE_BYTES);
            hex_tab_highlight_byte(sel_tab, line, bi);
        }
    }
}

#endif /* GEMU_GTK */
