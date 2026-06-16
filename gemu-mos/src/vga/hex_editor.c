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
    const char    *label;
    GtkWidget     *scrolled;
    GtkWidget     *text_view;
    GtkTextBuffer *buf;
    GtkTextTag    *hl_tag;       /* highlight for selected byte */
    GtkTextTag    *cursor_tag;   /* block cursor for active hex/ASCII cells */
    const uint8_t *data;         /* live pointer, or NULL */
    size_t         size;         /* bytes */
    bool           read_only;
    uint32_t       base_addr;    /* displayed starting address */
    int            hl_line;      /* currently highlighted line, or -1 */
    int            hl_byte;      /* currently highlighted byte index */
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
    uint32_t       selected_addr;   /* offset into current tab's data */
    bool           has_selection;
    int            edit_nibble;      /* 0 = high hex digit, 1 = low */
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Standard hex line: "XXXX: XX XX ... XX  AAAA...\n" */
#define HEX_LINE_BYTES   16
#define HEX_DATA_START   6
#define HEX_ASCII_START  55
#define HEX_FMT_BUF      128   /* big enough for one formatted line + NUL */

/* byte index → starting column of its first hex digit in the data area.
 * Bytes 0-15 are printed as "XX " except a double space after byte 7
 * (a visual gap splitting the line into two groups of 8). */
static int hex_byte_col(int i) {
    int col = HEX_DATA_START + i * 3;
    if (i >= 8) col += 1;  /* the extra separator space after byte 7 */
    return col;
}

static int hex_digit_value(guint keyval) {
    gunichar ch = gdk_keyval_to_unicode(keyval);
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return (int)(ch - 'a') + 10;
    if (ch >= 'A' && ch <= 'F') return (int)(ch - 'A') + 10;
    return -1;
}

/* Given a text offset into the buffer, return the line number and column. */
static void text_offset_to_line_col(GtkTextBuffer *buf, int offset,
                                     int *line, int *col) {
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_offset(buf, &iter, offset);
    *line = gtk_text_iter_get_line(&iter);
    *col  = gtk_text_iter_get_line_offset(&iter);
}

/* Format one hex line into `out` (without trailing newline).  Returns the
 * number of characters written (excluding null terminator). */
static int format_hex_line(char *out, size_t out_sz,
                            uint32_t addr, const uint8_t *bytes, int n) {
    char *p = out;
    p += snprintf(p, out_sz - (size_t)(p - out), "%04X: ", addr);
    for (int i = 0; i < HEX_LINE_BYTES; i++) {
        if (i < n)
            p += snprintf(p, out_sz - (size_t)(p - out),
                          "%02X%s", bytes[i], (i == 7) ? "  " : " ");
        else
            p += snprintf(p, out_sz - (size_t)(p - out),
                          "  %s", (i == 7) ? " " : " ");
    }
    /* Remove the trailing space after the last hex byte */
    if (p > out && p[-1] == ' ') p--;

    p += snprintf(p, out_sz - (size_t)(p - out), " ");
    for (int i = 0; i < HEX_LINE_BYTES; i++) {
        char c = (i < n && bytes[i] >= 32 && bytes[i] < 127) ? (char)bytes[i] : '.';
        *p++ = c;
    }
    *p = '\0';
    return (int)(p - out);
}

/* Rebuild the entire text buffer for a tab from its live data. */
static void hex_tab_rebuild(HexEditorTab *tab) {
    tab->hl_line = -1;  /* set_text wipes all tags; cache must follow */

    if (!tab->buf || !tab->data || tab->size == 0) {
        gtk_text_buffer_set_text(tab->buf, "(no data)\n", -1);
        return;
    }

    gtk_text_buffer_set_text(tab->buf, "", 0);

    size_t remain = tab->size;
    uint32_t addr = tab->base_addr;
    for (size_t off = 0; off < tab->size; off += HEX_LINE_BYTES, addr += HEX_LINE_BYTES) {
        int n = (int)(remain < HEX_LINE_BYTES ? remain : HEX_LINE_BYTES);
        char line[HEX_FMT_BUF];
        int len = format_hex_line(line, sizeof(line), addr, tab->data + off, n);
        line[len] = '\n';
        line[len + 1] = '\0';
        gtk_text_buffer_insert_at_cursor(tab->buf, line, -1);
        remain -= (size_t)n;
    }
}

/* Determine the buffer line range currently visible in the tab's text view
 * (plus a small margin). Returns false if the widget has no allocation yet,
 * in which case the caller should fall back to the full line range. */
static bool hex_tab_visible_line_range(HexEditorTab *tab, int *first, int *last) {
    if (!tab->text_view) return false;
    GdkRectangle visible;
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(tab->text_view), &visible);
    if (visible.height <= 0) return false;

    GtkTextIter top_iter, bot_iter;
    gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(tab->text_view), &top_iter,
                                 visible.y, NULL);
    gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(tab->text_view), &bot_iter,
                                 visible.y + visible.height, NULL);

    *first = gtk_text_iter_get_line(&top_iter);
    *last  = gtk_text_iter_get_line(&bot_iter);
    return true;
}

/* Replace the text of one line (excluding the newline) with new_text,
 * handling the edge case where deleting the last line's terminator leaves
 * the iterator out of range. */
static void hex_buffer_replace_line(GtkTextBuffer *buf, int line, const char *new_text) {
    GtkTextIter ds, de;
    gtk_text_buffer_get_iter_at_line(buf, &ds, line);
    de = ds;
    gtk_text_iter_forward_to_line_end(&de);
    gtk_text_iter_forward_char(&de);  /* skip past \n, or stays at end */
    bool is_last = gtk_text_iter_is_end(&de);
    gtk_text_buffer_delete(buf, &ds, &de);

    if (is_last)
        gtk_text_buffer_get_end_iter(buf, &ds);
    else
        gtk_text_buffer_get_iter_at_line(buf, &ds, line);

    char with_nl[HEX_FMT_BUF + 2];
    snprintf(with_nl, sizeof(with_nl), "%s\n", new_text);
    gtk_text_buffer_insert(buf, &ds, with_nl, -1);
}

static void hex_tab_invalidate_line_tags(HexEditorTab *tab, int line) {
    if (tab->hl_line == line)
        tab->hl_line = -1;
}

/* gtk_text_buffer_get_line_count() includes the final empty line when the
 * buffer ends with '\n'. hex_tab_rebuild() writes one newline per data row,
 * so compare/update against data rows rather than GTK's raw line count. */
static int hex_buffer_data_line_count(GtkTextBuffer *buf) {
    int n_lines = gtk_text_buffer_get_line_count(buf);
    if (n_lines <= 0) return 0;

    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(buf, &start, n_lines - 1);
    end = start;
    gtk_text_iter_forward_to_line_end(&end);

    if (gtk_text_iter_equal(&start, &end))
        n_lines--;
    return n_lines;
}

/* In-place update: compare each line against live data and only replace
 * lines that changed.  Leaves scroll position completely undisturbed.
 *
 * Only lines currently visible (+ a small margin) are touched. RAM in a
 * running game can change on practically every line every frame, so diffing
 * and rewriting the *entire* buffer each refresh was heavy enough to starve
 * the GTK main loop and made the scrollbar/mouse wheel unusable. Restricting
 * the work to the visible viewport keeps the cost constant regardless of
 * buffer size. */
static void hex_tab_update(HexEditorTab *tab) {
    if (!tab->buf || !tab->data || tab->size == 0) return;

    int n_lines = hex_buffer_data_line_count(tab->buf);
    size_t expected = (tab->size + HEX_LINE_BYTES - 1) / HEX_LINE_BYTES;
    if ((size_t)n_lines != expected) {
        hex_tab_rebuild(tab);
        return;
    }

    int first_line = 0, last_line = n_lines - 1;
    if (hex_tab_visible_line_range(tab, &first_line, &last_line)) {
        first_line -= 4;
        last_line  += 4;
        if (first_line < 0) first_line = 0;
        if (last_line >= n_lines) last_line = n_lines - 1;
    }

    for (int line = first_line; line <= last_line; line++) {
        size_t off = (size_t)line * HEX_LINE_BYTES;
        if (off >= tab->size) break;
        uint32_t addr = tab->base_addr + (uint32_t)off;
        int n = (int)((tab->size - off) < (size_t)HEX_LINE_BYTES
                      ? (tab->size - off) : HEX_LINE_BYTES);

        GtkTextIter ls, le;
        gtk_text_buffer_get_iter_at_line(tab->buf, &ls, line);
        le = ls;
        gtk_text_iter_forward_to_line_end(&le);

        char *old_text = gtk_text_buffer_get_text(tab->buf, &ls, &le, FALSE);
        if (!old_text) continue;

        char new_line[HEX_FMT_BUF];
        format_hex_line(new_line, sizeof(new_line), addr, tab->data + off, n);

        if (strcmp(old_text, new_line) != 0) {
            hex_buffer_replace_line(tab->buf, line, new_line);
            hex_tab_invalidate_line_tags(tab, line);
        }
        g_free(old_text);
    }
}

/* Remove highlight from all bytes in a tab. */
static void hex_tab_clear_highlight(HexEditorTab *tab) {
    if (!tab->buf || !tab->hl_tag) return;
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(tab->buf, &s);
    gtk_text_buffer_get_end_iter(tab->buf, &e);
    gtk_text_buffer_remove_tag(tab->buf, tab->hl_tag, &s, &e);
    tab->hl_line = -1;
}

static void hex_tab_clear_cursor(HexEditorTab *tab) {
    if (!tab->buf || !tab->cursor_tag) return;
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(tab->buf, &s);
    gtk_text_buffer_get_end_iter(tab->buf, &e);
    gtk_text_buffer_remove_tag(tab->buf, tab->cursor_tag, &s, &e);
}

/* Highlight a single byte at the given line & byte index.
 *
 * This used to unconditionally do a remove_tag over the *entire* buffer
 * every call, and was being called every single frame (60Hz) regardless of
 * the refresh throttle. For a large tab (e.g. a multi-megabit PRG ROM, tens
 * of thousands of lines) that whole-buffer scan every frame was enough on
 * its own to starve the GTK main loop and make scrolling unusable. Skip the
 * work entirely when the highlighted location hasn't actually moved. */
static void hex_tab_highlight_byte(HexEditorTab *tab, int line, int byte_idx) {
    if (!tab->buf || !tab->hl_tag) return;
    if (tab->hl_line == line && tab->hl_byte == byte_idx) return;
    hex_tab_clear_highlight(tab);

    GtkTextIter s;
    gtk_text_buffer_get_iter_at_line_offset(tab->buf, &s, line,
                                            hex_byte_col(byte_idx));
    GtkTextIter e = s;
    gtk_text_iter_forward_chars(&e, 2);  /* two hex digits */

    gtk_text_buffer_apply_tag(tab->buf, tab->hl_tag, &s, &e);
    tab->hl_line = line;
    tab->hl_byte = byte_idx;
}

static void hex_tab_place_cursor(HexEditorTab *tab, int line, int byte_idx,
                                  int nibble) {
    if (!tab->buf || !tab->text_view || !tab->cursor_tag) return;
    hex_tab_clear_cursor(tab);

    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_line_offset(tab->buf, &s, line,
                                            hex_byte_col(byte_idx) + nibble);
    e = s;
    gtk_text_iter_forward_char(&e);
    gtk_text_buffer_apply_tag(tab->buf, tab->cursor_tag, &s, &e);
    gtk_text_buffer_place_cursor(tab->buf, &s);

    gtk_text_buffer_get_iter_at_line_offset(tab->buf, &s, line,
                                            HEX_ASCII_START + byte_idx);
    e = s;
    gtk_text_iter_forward_char(&e);
    gtk_text_buffer_apply_tag(tab->buf, tab->cursor_tag, &s, &e);
}

/* Given a text offset in the buffer, determine which byte was clicked.
 * Returns true and sets *byte_idx, *nibble and *addr if a valid hex/ASCII
 * cell was clicked.
 * Hit-testing walks hex_byte_col() directly so the click target can never
 * drift out of sync with where bytes are actually highlighted/drawn. */
static bool hex_tab_byte_at_pos(HexEditorTab *tab, int offset,
                                 int *byte_idx, int *nibble, uint32_t *addr) {
    int line, col;
    text_offset_to_line_col(tab->buf, offset, &line, &col);

    for (int i = 0; i < HEX_LINE_BYTES; i++) {
        int start = hex_byte_col(i);
        if (col < start || col >= start + 2) continue;

        size_t data_off = (size_t)line * HEX_LINE_BYTES + (size_t)i;
        if (data_off >= tab->size) return false;

        *byte_idx = i;
        *nibble   = col - start;
        *addr     = tab->base_addr + (uint32_t)data_off;
        return true;
    }

    if (col >= HEX_ASCII_START && col < HEX_ASCII_START + HEX_LINE_BYTES) {
        int i = col - HEX_ASCII_START;
        size_t data_off = (size_t)line * HEX_LINE_BYTES + (size_t)i;
        if (data_off >= tab->size) return false;

        *byte_idx = i;
        *nibble   = 0;
        *addr     = tab->base_addr + (uint32_t)data_off;
        return true;
    }
    return false;
}

static HexEditorTab *hex_tab_for_text_view(HexEditor *he, GtkWidget *text_view) {
    for (int i = 0; i < HEX_TAB_COUNT; i++) {
        if (he->tabs[i].text_view == text_view)
            return &he->tabs[i];
    }
    return NULL;
}

static void hex_sync_cur_tab(HexEditor *he) {
    int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(he->notebook));
    if (page >= 0 && page < HEX_TAB_COUNT)
        he->cur_tab = &he->tabs[page];
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

    ((uint8_t *)he->cur_tab->data)[off] = val;
    return true;
}

/* Select a byte: highlight it, update entries. */
static void hex_select_byte(HexEditor *he, int line, int byte_idx, uint32_t addr) {
    he->selected_addr = addr;
    he->has_selection = true;
    if (he->cur_tab) {
        hex_tab_highlight_byte(he->cur_tab, line, byte_idx);
        hex_tab_place_cursor(he->cur_tab, line, byte_idx, he->edit_nibble);
    }
    hex_update_entries(he, addr, hex_read_byte(he));

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
    he->cur_tab  = &he->tabs[page];
    he->has_selection = false;
    he->edit_nibble = 0;
    gtk_widget_set_sensitive(he->write_btn,
                             he->cur_tab && !he->cur_tab->read_only);
    gtk_label_set_text(GTK_LABEL(he->status_label), "");
}

/* ── Callbacks ───────────────────────────────────────────────────────────── */

static gboolean on_text_click(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    HexEditor *he = ud;
    HexEditorTab *tab = hex_tab_for_text_view(he, w);
    if (!tab || !tab->buf) return FALSE;
    he->cur_tab = tab;

    /* Convert window coords to buffer position */
    gint bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(w),
        GTK_TEXT_WINDOW_WIDGET, (gint)ev->x, (gint)ev->y, &bx, &by);

    GtkTextIter iter;
    gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(w), &iter, bx, by);

    int byte_idx, nibble;
    uint32_t addr;
    int line = gtk_text_iter_get_line(&iter);

    if (hex_tab_byte_at_pos(tab, gtk_text_iter_get_offset(&iter),
                            &byte_idx, &nibble, &addr)) {
        he->edit_nibble = nibble;
        hex_select_byte(he, line, byte_idx, addr);
        gtk_widget_grab_focus(tab->text_view);
        return TRUE;
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
    hex_sync_cur_tab(he);
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

        /* In-place update so scroll is preserved */
        hex_tab_update(he->cur_tab);

        /* Re-select the byte */
        uint32_t off = he->selected_addr - he->cur_tab->base_addr;
        int line = (int)(off / HEX_LINE_BYTES);
        int bi   = (int)(off % HEX_LINE_BYTES);
        hex_tab_highlight_byte(he->cur_tab, line, bi);
        hex_tab_place_cursor(he->cur_tab, line, bi, he->edit_nibble);
    }
}

static void on_value_activate(GtkEntry *entry, gpointer ud) {
    (void)entry;
    on_write_clicked(NULL, ud);
}

static void on_goto_activate(GtkEntry *entry, gpointer ud) {
    HexEditor *he = ud;
    hex_sync_cur_tab(he);
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

    uint32_t off = addr - base;
    int line = (int)(off / HEX_LINE_BYTES);
    int bi   = (int)(off % HEX_LINE_BYTES);

    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_line_offset(he->cur_tab->buf, &iter, line,
                                            hex_byte_col(bi));
    gtk_text_buffer_place_cursor(he->cur_tab->buf, &iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(he->cur_tab->text_view),
                                  &iter, 0.0, FALSE, 0.0, 0.0);

    he->edit_nibble = 0;
    hex_select_byte(he, line, bi, addr);
}

/* Move the selection by `delta` bytes if the result stays in range. */
static void hex_move_selection(HexEditor *he, int32_t delta, bool scroll) {
    uint32_t base = he->cur_tab->base_addr;
    uint32_t size = (uint32_t)he->cur_tab->size;

    if (delta < 0 && he->selected_addr - base < (uint32_t)(-delta)) return;
    uint32_t new_addr = he->selected_addr + (uint32_t)delta;
    if (new_addr - base >= size) return;

    uint32_t off = new_addr - base;
    int line = (int)(off / HEX_LINE_BYTES);
    int bi   = (int)(off % HEX_LINE_BYTES);
    hex_select_byte(he, line, bi, new_addr);

    if (scroll) {
        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_line(he->cur_tab->buf, &iter, line);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(he->cur_tab->text_view),
                                      &iter, 0.0, FALSE, 0.0, 0.0);
    }
}

static void hex_move_cursor_digit(HexEditor *he, int32_t delta) {
    uint32_t base = he->cur_tab->base_addr;
    int32_t digit = (int32_t)((he->selected_addr - base) * 2u)
                    + he->edit_nibble + delta;
    int32_t max_digit = (int32_t)(he->cur_tab->size * 2u) - 1;

    if (digit < 0 || digit > max_digit) return;

    uint32_t new_addr = base + (uint32_t)(digit / 2);
    int32_t byte_delta = (int32_t)(new_addr - base)
                         - (int32_t)(he->selected_addr - base);
    he->edit_nibble = digit & 1;
    hex_move_selection(he, byte_delta, false);
}

static void hex_edit_selected_nibble(HexEditor *he, int nibble_val) {
    if (!he->cur_tab || !he->cur_tab->data)
        return;
    if (he->cur_tab->read_only) {
        gtk_label_set_text(GTK_LABEL(he->status_label), "Read-only memory");
        return;
    }

    uint8_t old_val = hex_read_byte(he);
    uint8_t new_val = he->edit_nibble == 0
        ? (uint8_t)((old_val & 0x0Fu) | (uint8_t)(nibble_val << 4))
        : (uint8_t)((old_val & 0xF0u) | (uint8_t)nibble_val);

    if (!hex_write_byte(he, he->selected_addr, new_val))
        return;

    uint32_t written_addr = he->selected_addr;
    uint32_t off = he->selected_addr - he->cur_tab->base_addr;
    int line = (int)(off / HEX_LINE_BYTES);
    int bi   = (int)(off % HEX_LINE_BYTES);
    hex_tab_update(he->cur_tab);

    if (he->edit_nibble == 0) {
        he->edit_nibble = 1;
        hex_select_byte(he, line, bi, he->selected_addr);
    } else if (off + 1 < he->cur_tab->size) {
        he->edit_nibble = 0;
        hex_move_selection(he, 1, true);
    } else {
        he->edit_nibble = 0;
        hex_select_byte(he, line, bi, he->selected_addr);
    }

    char status[64];
    snprintf(status, sizeof(status), "Wrote $%02X to $%04X", new_val, written_addr);
    gtk_label_set_text(GTK_LABEL(he->status_label), status);
}

static bool hex_key_should_edit(HexEditor *he) {
    if (!he || !he->window || !he->cur_tab || !he->cur_tab->text_view)
        return false;

    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(he->window));
    if (focus && GTK_IS_ENTRY(focus))
        return false;
    return !focus || focus == he->cur_tab->text_view
           || gtk_widget_is_ancestor(focus, he->cur_tab->text_view);
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    HexEditor *he = ud;
    (void)w;

    if (!hex_key_should_edit(he))
        return FALSE;

    hex_sync_cur_tab(he);

    if (!he->has_selection || !he->cur_tab || !he->cur_tab->data)
        return FALSE;

    int digit = hex_digit_value(ev->keyval);
    if (digit >= 0) {
        hex_edit_selected_nibble(he, digit);
        return TRUE;
    }

    switch (ev->keyval) {
    case GDK_KEY_Up:    hex_move_selection(he, -HEX_LINE_BYTES, true);  return TRUE;
    case GDK_KEY_Down:  hex_move_selection(he,  HEX_LINE_BYTES, true);  return TRUE;
    case GDK_KEY_Left:  hex_move_cursor_digit(he, -1);                  return TRUE;
    case GDK_KEY_Right: hex_move_cursor_digit(he,  1);                  return TRUE;
    case GDK_KEY_BackSpace: hex_move_cursor_digit(he, -1);              return TRUE;
    default:            return FALSE;
    }
}

static gboolean on_value_key_press(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    (void)w;

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
    tab->scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scrolled),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    tab->text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tab->text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tab->text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tab->text_view), GTK_WRAP_NONE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tab->text_view), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tab->text_view), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tab->text_view), 4);
    gtk_widget_set_can_focus(tab->text_view, TRUE);

    tab->buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->text_view));
    tab->hl_tag = gtk_text_buffer_create_tag(tab->buf, "highlight",
        "background", "#FFFF00", "foreground", "#000000", NULL);
    tab->cursor_tag = gtk_text_buffer_create_tag(tab->buf, "cursor",
        "background", "#000000", "foreground", "#FFFFFF", NULL);

    gtk_container_add(GTK_CONTAINER(tab->scrolled), tab->text_view);

    g_signal_connect(tab->text_view, "button-press-event",
                     G_CALLBACK(on_text_click), he);
    g_signal_connect(tab->text_view, "key-press-event",
                     G_CALLBACK(on_key_press), he);

    gtk_widget_add_events(tab->text_view,
        GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);

    return tab->scrolled;
}

/* Initialize one tab's data source, build its view, rebuild its contents,
 * and append it to the notebook. */
static void hex_editor_add_tab(HexEditor *he, HexTabId id, const char *label,
                                const uint8_t *data, size_t size,
                                bool read_only, uint32_t base_addr) {
    HexEditorTab *tab = &he->tabs[id];
    *tab = (HexEditorTab){
        .label     = label,
        .data      = data,
        .size      = size,
        .read_only = read_only,
        .base_addr = base_addr,
    };
    hex_create_tab_view(he, tab);
    hex_tab_rebuild(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(he->notebook), tab->scrolled,
                              gtk_label_new(tab->label));
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
    g_signal_connect(he->window, "key-press-event",
                     G_CALLBACK(on_key_press), he);

    /* ── Main vertical box ── */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(he->window), vbox);

    /* ── Notebook (tabs) ── */
    he->notebook = gtk_notebook_new();
    g_signal_connect(he->notebook, "switch-page",
                     G_CALLBACK(on_page_switched), he);
    gtk_box_pack_start(GTK_BOX(vbox), he->notebook, TRUE, TRUE, 0);

    hex_editor_add_tab(he, HEX_TAB_RAM, "CPU RAM ($0000-$07FF)",
                        s->ram, sizeof(s->ram), false, 0x0000);
    hex_editor_add_tab(he, HEX_TAB_PRG, "PRG ROM ($8000+)",
                        s->prg, s->prg ? (size_t)s->cart.prg_banks * 0x4000u : 0,
                        true, 0x8000);
    hex_editor_add_tab(he, HEX_TAB_CHR, "CHR ROM/RAM",
                        s->chr, s->chr ? (size_t)s->cart.chr_banks * 0x2000u : 0,
                        !s->chr_is_ram, 0x0000);

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

    /* Rebuild all tabs with fresh data, preserving scroll positions */
    for (int i = 0; i < HEX_TAB_COUNT; i++) {
        if (!he->tabs[i].data) continue;
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(he->tabs[i].scrolled));
        double saved = vadj ? gtk_adjustment_get_value(vadj) : 0.0;
        hex_tab_rebuild(&he->tabs[i]);
        if (vadj) gtk_adjustment_set_value(vadj, saved);
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

    /* Throttle: rebuild only every 4th frame (~15 Hz) to keep overhead low
     * while still showing live memory changes. */
    he->refresh_skip = (he->refresh_skip + 1) & 3;
    if (he->refresh_skip != 0) {
        /* On skip frames, just restore the selection highlight if any. */
        if (he->has_selection && he->cur_tab) {
            HexEditorTab *sel_tab = he->cur_tab;
            uint32_t off = he->selected_addr - sel_tab->base_addr;
            if (off < sel_tab->size) {
                int line = (int)(off / HEX_LINE_BYTES);
                int bi   = (int)(off % HEX_LINE_BYTES);
                hex_tab_highlight_byte(sel_tab, line, bi);
                hex_tab_place_cursor(sel_tab, line, bi, he->edit_nibble);
            }
        }
        return;
    }

    /* Rebuild all non-read-only tabs with fresh data via in-place line
     * updates — scroll position is naturally preserved because only
     * changed lines are replaced. */
    for (int i = 0; i < HEX_TAB_COUNT; i++) {
        HexEditorTab *tab = &he->tabs[i];
        if (!tab->data || tab->read_only) continue;
        hex_tab_update(tab);
    }

    /* Restore selection highlight on the active tab */
    if (he->has_selection && he->cur_tab) {
        HexEditorTab *sel_tab = he->cur_tab;
        uint32_t off = he->selected_addr - sel_tab->base_addr;
        if (off < sel_tab->size) {
            int line = (int)(off / HEX_LINE_BYTES);
            int bi   = (int)(off % HEX_LINE_BYTES);
            hex_tab_highlight_byte(sel_tab, line, bi);
            hex_tab_place_cursor(sel_tab, line, bi, he->edit_nibble);
        }
    }
}

#endif /* GEMU_GTK */
