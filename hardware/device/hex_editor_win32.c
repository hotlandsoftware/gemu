/*
 * Native Win32 hex editor - GTK-free equivalent of hex_editor.c.
 *
 * One tab-per-region window (SysTabControl32) whose page is a single
 * owner-drawn child (class "GemuHexView") that paints directly from the
 * live data pointer every WM_PAINT - there is no intermediate text buffer
 * to keep in sync, so the diffing machinery the GTK version needs (it
 * mutates a GtkTextBuffer in place to avoid starving the GTK main loop)
 * has no equivalent here: repainting ~25 visible lines from memory is
 * already cheap, so a full invalidate on every refresh tick is enough.
 *
 * Layout: tab control docked to the top, a fixed-height control bar
 * (Goto / Addr / Value / Write / status) docked to the bottom, hex view
 * fills the remainder.
 */
#if defined(_WIN32) && !defined(GEMU_GTK)
#include "hex_editor.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEX_MAX_TABS   8
#define HEX_LINE_BYTES 16
/* Column layout mirrors the GTK version's text-based rendering exactly
 * (character offsets on a "XXXX: XX XX ... XX  AAAA..." line), so the
 * geometry/hit-testing math is identical, just against pixel cells
 * instead of GtkTextIter offsets. */
#define HEX_DATA_START  6
#define HEX_ASCII_START 55

#define IDC_TAB    100
#define IDC_GOTO   101
#define IDC_ADDR   102
#define IDC_VALUE  103
#define IDC_WRITE  104
#define IDC_STATUS 105
#define IDC_VIEW   106

typedef struct {
    const char *label;
    const uint8_t *data;
    size_t   size;
    bool     read_only;
    uint32_t base_addr;
    int      scroll_line;   /* preserved across tab switches */
} HexEditorTab;

struct HexEditor {
    HWND hwnd, tab_ctrl, view, goto_edit, addr_edit, value_edit, write_btn, status;
    HFONT font;
    int   char_w, char_h;
    bool  visible;
    int   refresh_skip;
    int   n_tabs;
    HexEditorTab tabs[HEX_MAX_TABS];
    int   cur_tab;          /* index, -1 = none */
    uint32_t selected_addr;
    bool  has_selection;
    int   sel_line, sel_byte;
    int   edit_nibble;      /* 0 = high hex digit, 1 = low */
};

static ATOM g_view_class;
static ATOM g_win_class;

/* ── Formatting (mirrors hex_editor.c's format_hex_line/hex_byte_col) ────── */

static int hex_byte_col(int i) {
    int col = HEX_DATA_START + i * 3;
    if (i >= 8) col += 1;
    return col;
}

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
    if (p > out && p[-1] == ' ') p--;
    p += snprintf(p, out_sz - (size_t)(p - out), " ");
    for (int i = 0; i < HEX_LINE_BYTES; i++) {
        char c = (i < n && bytes[i] >= 32 && bytes[i] < 127) ? (char)bytes[i] : '.';
        *p++ = c;
    }
    *p = '\0';
    return (int)(p - out);
}

/* ── Small helpers ───────────────────────────────────────────────────────── */

static HexEditorTab *cur_tab(HexEditor *he) {
    return (he->cur_tab >= 0 && he->cur_tab < he->n_tabs)
         ? &he->tabs[he->cur_tab] : NULL;
}

static int tab_line_count(const HexEditorTab *tab) {
    if (!tab->data || tab->size == 0) return 0;
    return (int)((tab->size + HEX_LINE_BYTES - 1) / HEX_LINE_BYTES);
}

static void set_status(HexEditor *he, const char *text) {
    SetWindowTextA(he->status, text);
}

static uint8_t read_byte(const HexEditorTab *tab, uint32_t addr) {
    if (!tab || !tab->data) return 0;
    uint32_t off = addr - tab->base_addr;
    return (off < tab->size) ? tab->data[off] : 0;
}

static bool write_byte(HexEditorTab *tab, uint32_t addr, uint8_t val) {
    if (!tab || tab->read_only || !tab->data) return false;
    uint32_t off = addr - tab->base_addr;
    if (off >= tab->size) return false;
    ((uint8_t *)tab->data)[off] = val;
    return true;
}

static void update_entries(HexEditor *he, uint32_t addr, uint8_t val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%04X", addr);
    SetWindowTextA(he->addr_edit, buf);
    snprintf(buf, sizeof(buf), "%02X", val);
    SetWindowTextA(he->value_edit, buf);
}

static void scroll_to_line(HexEditor *he, HexEditorTab *tab, int line) {
    RECT rc; GetClientRect(he->view, &rc);
    int visible = he->char_h > 0 ? (rc.bottom / he->char_h) : 1;
    int n_lines = tab_line_count(tab);
    if (line < tab->scroll_line) tab->scroll_line = line;
    else if (line >= tab->scroll_line + visible) tab->scroll_line = line - visible + 1;
    if (tab->scroll_line < 0) tab->scroll_line = 0;
    int max_scroll = n_lines - visible; if (max_scroll < 0) max_scroll = 0;
    if (tab->scroll_line > max_scroll) tab->scroll_line = max_scroll;

    SCROLLINFO si = { .cbSize = sizeof(si), .fMask = SIF_RANGE | SIF_PAGE | SIF_POS };
    si.nMin = 0; si.nMax = n_lines > 0 ? n_lines - 1 : 0;
    si.nPage = (UINT)visible; si.nPos = tab->scroll_line;
    SetScrollInfo(he->view, SB_VERT, &si, TRUE);
}

/* Select a byte: update state, entries, status, enable/disable Write. */
static void select_byte(HexEditor *he, int line, int byte_idx, uint32_t addr) {
    he->selected_addr = addr;
    he->has_selection  = true;
    he->sel_line = line;
    he->sel_byte = byte_idx;

    HexEditorTab *tab = cur_tab(he);
    update_entries(he, addr, read_byte(tab, addr));
    EnableWindow(he->write_btn, tab && !tab->read_only);

    char status[64];
    snprintf(status, sizeof(status), "Selected $%04X", addr);
    set_status(he, status);
    InvalidateRect(he->view, NULL, FALSE);
}

/* Move selection by delta bytes if the result stays in range. */
static void move_selection(HexEditor *he, int32_t delta, bool scroll) {
    HexEditorTab *tab = cur_tab(he);
    if (!tab) return;
    uint32_t base = tab->base_addr, size = (uint32_t)tab->size;

    if (delta < 0 && he->selected_addr - base < (uint32_t)(-delta)) return;
    uint32_t new_addr = he->selected_addr + (uint32_t)delta;
    if (new_addr - base >= size) return;

    uint32_t off = new_addr - base;
    int line = (int)(off / HEX_LINE_BYTES), bi = (int)(off % HEX_LINE_BYTES);
    if (scroll) scroll_to_line(he, tab, line);
    select_byte(he, line, bi, new_addr);
}

static void move_cursor_digit(HexEditor *he, int32_t delta) {
    HexEditorTab *tab = cur_tab(he);
    if (!tab) return;
    uint32_t base = tab->base_addr;
    int32_t digit = (int32_t)((he->selected_addr - base) * 2u) + he->edit_nibble + delta;
    int32_t max_digit = (int32_t)(tab->size * 2u) - 1;
    if (digit < 0 || digit > max_digit) return;

    uint32_t new_addr = base + (uint32_t)(digit / 2);
    int32_t byte_delta = (int32_t)(new_addr - base) - (int32_t)(he->selected_addr - base);
    he->edit_nibble = digit & 1;
    move_selection(he, byte_delta, false);
}

static void edit_selected_nibble(HexEditor *he, int nibble_val) {
    HexEditorTab *tab = cur_tab(he);
    if (!tab || !tab->data) return;
    if (tab->read_only) { set_status(he, "Read-only memory"); return; }

    uint8_t old_val = read_byte(tab, he->selected_addr);
    uint8_t new_val = he->edit_nibble == 0
        ? (uint8_t)((old_val & 0x0Fu) | (uint8_t)(nibble_val << 4))
        : (uint8_t)((old_val & 0xF0u) | (uint8_t)nibble_val);
    if (!write_byte(tab, he->selected_addr, new_val)) return;

    uint32_t written_addr = he->selected_addr;
    uint32_t off = he->selected_addr - tab->base_addr;
    int line = (int)(off / HEX_LINE_BYTES), bi = (int)(off % HEX_LINE_BYTES);

    if (he->edit_nibble == 0) {
        he->edit_nibble = 1;
        select_byte(he, line, bi, he->selected_addr);
    } else if (off + 1 < tab->size) {
        he->edit_nibble = 0;
        move_selection(he, 1, true);
    } else {
        he->edit_nibble = 0;
        select_byte(he, line, bi, he->selected_addr);
    }

    char status[64];
    snprintf(status, sizeof(status), "Wrote $%02X to $%04X", new_val, written_addr);
    set_status(he, status);
}

/* ── Hex-view child window ───────────────────────────────────────────────── */

static bool byte_at_pos(HexEditorTab *tab, int line, int col,
                        int *byte_idx, int *nibble, uint32_t *addr) {
    for (int i = 0; i < HEX_LINE_BYTES; i++) {
        int start = hex_byte_col(i);
        if (col < start || col >= start + 2) continue;
        size_t off = (size_t)line * HEX_LINE_BYTES + (size_t)i;
        if (off >= tab->size) return false;
        *byte_idx = i; *nibble = col - start;
        *addr = tab->base_addr + (uint32_t)off;
        return true;
    }
    if (col >= HEX_ASCII_START && col < HEX_ASCII_START + HEX_LINE_BYTES) {
        int i = col - HEX_ASCII_START;
        size_t off = (size_t)line * HEX_LINE_BYTES + (size_t)i;
        if (off >= tab->size) return false;
        *byte_idx = i; *nibble = 0;
        *addr = tab->base_addr + (uint32_t)off;
        return true;
    }
    return false;
}

/* Plain TextOutA sizes its OPAQUE background fill from the font's real
 * glyph-advance metrics, not from he->char_w. Since every fragment below is
 * positioned by hand at "4 + col * he->char_w", any mismatch between the
 * two (ClearType/kerning rounding — even on a "monospace" font) makes
 * adjacent opaque fragments gap or overlap by a pixel, clipping whatever
 * glyph sits at that seam. Force exact per-character pixel width via
 * ExtTextOutA's dx array so hand-computed columns are always right. */
static void hex_text_out(HDC hdc, int x, int y, const char *s, int n, int char_w) {
    static int dx[128];
    if (n > 128) n = 128;
    for (int i = 0; i < n; i++) dx[i] = char_w;
    ExtTextOutA(hdc, x, y, ETO_OPAQUE, NULL, s, (UINT)n, dx);
}

static void view_paint(HexEditor *he, HDC hdc, RECT client) {
    HexEditorTab *tab = cur_tab(he);
    HFONT old_font = (HFONT)SelectObject(hdc, he->font);
    SetBkMode(hdc, OPAQUE);

    if (!tab || !tab->data || tab->size == 0) {
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkColor(hdc, RGB(255, 255, 255));
        hex_text_out(hdc, 4, 2, "(no data)", 9, he->char_w);
        SelectObject(hdc, old_font);
        return;
    }

    int visible_lines = client.bottom / he->char_h + 2;
    int n_lines = tab_line_count(tab);
    char line[128];

    for (int row = 0; row < visible_lines; row++) {
        int line_no = tab->scroll_line + row;
        if (line_no >= n_lines) break;
        size_t off = (size_t)line_no * HEX_LINE_BYTES;
        uint32_t addr = tab->base_addr + (uint32_t)off;
        int n = (int)((tab->size - off) < HEX_LINE_BYTES ? tab->size - off : HEX_LINE_BYTES);
        int len = format_hex_line(line, sizeof(line), addr, tab->data + off, n);

        int y = row * he->char_h;
        bool sel_row = he->has_selection && he->cur_tab >= 0 &&
                      &he->tabs[he->cur_tab] == tab && line_no == he->sel_line;

        if (!sel_row) {
            SetTextColor(hdc, RGB(0, 0, 0));
            SetBkColor(hdc, RGB(255, 255, 255));
            hex_text_out(hdc, 4, y, line, len, he->char_w);
        } else {
            /* Draw in three pieces so the selected hex digits / ASCII char
             * can be highlighted without a second pass. */
            int hex_col = hex_byte_col(he->sel_byte);
            int ascii_col = HEX_ASCII_START + he->sel_byte;

            SetTextColor(hdc, RGB(0, 0, 0));
            SetBkColor(hdc, RGB(255, 255, 255));
            hex_text_out(hdc, 4, y, line, hex_col, he->char_w);
            hex_text_out(hdc, 4 + (hex_col + 2) * he->char_w, y,
                        line + hex_col + 2, len - hex_col - 2, he->char_w);

            SetTextColor(hdc, RGB(0, 0, 0));
            SetBkColor(hdc, RGB(255, 255, 0));
            hex_text_out(hdc, 4 + hex_col * he->char_w, y, line + hex_col, 2, he->char_w);
            hex_text_out(hdc, 4 + ascii_col * he->char_w, y, line + ascii_col, 1, he->char_w);

            if (GetFocus() == he->view) {
                int nib_col = hex_col + he->edit_nibble;
                SetTextColor(hdc, RGB(255, 255, 255));
                SetBkColor(hdc, RGB(0, 0, 0));
                hex_text_out(hdc, 4 + nib_col * he->char_w, y, line + nib_col, 1, he->char_w);
            }
        }
    }
    SelectObject(hdc, old_font);
}

static LRESULT CALLBACK view_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HexEditor *he = (HexEditor *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_ERASEBKGND:
        return 1;   /* WM_PAINT repaints every visible cell; avoid flicker */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP old_bmp = (HBITMAP)SelectObject(mem, bmp);
        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        if (he) view_paint(he, mem, rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old_bmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE: {
        if (!he) break;
        HexEditorTab *tab = cur_tab(he);
        if (tab) scroll_to_line(he, tab, he->sel_line);
        break;
    }
    case WM_VSCROLL: {
        if (!he) break;
        HexEditorTab *tab = cur_tab(he);
        if (!tab) break;
        RECT rc; GetClientRect(hwnd, &rc);
        int visible = rc.bottom / he->char_h;
        int n_lines = tab_line_count(tab);
        int max_scroll = n_lines - visible; if (max_scroll < 0) max_scroll = 0;
        int pos = tab->scroll_line;
        switch (LOWORD(wp)) {
        case SB_LINEUP:   pos -= 1; break;
        case SB_LINEDOWN: pos += 1; break;
        case SB_PAGEUP:   pos -= visible; break;
        case SB_PAGEDOWN: pos += visible; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: {
            SCROLLINFO si = { .cbSize = sizeof(si), .fMask = SIF_TRACKPOS };
            GetScrollInfo(hwnd, SB_VERT, &si);
            pos = si.nTrackPos;
            break;
        }
        default: break;
        }
        if (pos < 0) pos = 0;
        if (pos > max_scroll) pos = max_scroll;
        tab->scroll_line = pos;
        SetScrollPos(hwnd, SB_VERT, pos, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (!he) break;
        int notches = (int)(short)HIWORD(wp) / WHEEL_DELTA;
        HexEditorTab *tab = cur_tab(he);
        if (tab) {
            tab->scroll_line -= notches * 3;
            scroll_to_line(he, tab, tab->scroll_line);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!he) break;
        HexEditorTab *tab = cur_tab(he);
        if (!tab || !tab->data) break;
        SetFocus(hwnd);
        int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        int col = x / he->char_w;
        int row = y / he->char_h;
        int line = tab->scroll_line + row;
        int byte_idx, nibble; uint32_t addr;
        if (byte_at_pos(tab, line, col, &byte_idx, &nibble, &addr)) {
            he->edit_nibble = nibble;
            select_byte(he, line, byte_idx, addr);
        }
        return 0;
    }
    case WM_SETFOCUS: case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN: {
        if (!he || !he->has_selection) break;
        switch (wp) {
        case VK_UP:    move_selection(he, -HEX_LINE_BYTES, true); return 0;
        case VK_DOWN:  move_selection(he,  HEX_LINE_BYTES, true); return 0;
        case VK_LEFT:  move_cursor_digit(he, -1); return 0;
        case VK_RIGHT: move_cursor_digit(he,  1); return 0;
        case VK_BACK:  move_cursor_digit(he, -1); return 0;
        default: break;
        }
        break;
    }
    case WM_CHAR: {
        if (!he || !he->has_selection) break;
        int c = (int)wp, digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit >= 0) { edit_selected_nibble(he, digit); return 0; }
        break;
    }
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ── Bottom-bar edit subclassing (Enter key + hex-digit filtering) ───────── */

static void on_goto(HexEditor *he) {
    HexEditorTab *tab = cur_tab(he);
    char text[32];
    GetWindowTextA(he->goto_edit, text, sizeof(text));
    uint32_t addr = (uint32_t)strtoul(text, NULL, 16);

    if (!tab || !tab->data || tab->size == 0) {
        set_status(he, "No data to navigate");
        return;
    }
    uint32_t base = tab->base_addr, end = base + (uint32_t)tab->size;
    if (addr < base || addr >= end) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Address $%04X out of range ($%04X-$%04X)",
                addr, base, end - 1);
        set_status(he, msg);
        return;
    }
    uint32_t off = addr - base;
    int line = (int)(off / HEX_LINE_BYTES), bi = (int)(off % HEX_LINE_BYTES);
    he->edit_nibble = 0;
    scroll_to_line(he, tab, line);
    select_byte(he, line, bi, addr);
}

static void on_write(HexEditor *he) {
    HexEditorTab *tab = cur_tab(he);
    if (!he->has_selection || !tab || tab->read_only) return;

    char text[8];
    GetWindowTextA(he->value_edit, text, sizeof(text));
    char *end = NULL;
    uint32_t val = (uint32_t)strtoul(text, &end, 16);
    if (end == text || val > 0xFF) { set_status(he, "Invalid hex value"); return; }

    if (write_byte(tab, he->selected_addr, (uint8_t)val)) {
        char status[64];
        snprintf(status, sizeof(status), "Wrote $%02X to $%04X",
                (uint8_t)val, he->selected_addr);
        set_status(he, status);
        InvalidateRect(he->view, NULL, FALSE);
    }
}

static LRESULT CALLBACK goto_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR id, DWORD_PTR ref) {
    (void)id;
    HexEditor *he = (HexEditor *)ref;
    if (msg == WM_CHAR && wp == VK_RETURN) { on_goto(he); return 0; }
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK value_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                       UINT_PTR id, DWORD_PTR ref) {
    (void)id;
    HexEditor *he = (HexEditor *)ref;
    if (msg == WM_CHAR) {
        if (wp == VK_RETURN) { on_write(he); return 0; }
        int c = (int)wp;
        bool hexdigit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
        bool ctrl = (c < 0x20);   /* backspace, delete-via-ctrl, etc. */
        if (!hexdigit && !ctrl) return 0;   /* swallow non-hex printable chars */
    }
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ── Tab switching / layout ──────────────────────────────────────────────── */

#define BAR_HEIGHT 28

static void relayout(HexEditor *he) {
    RECT rc; GetClientRect(he->hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    MoveWindow(he->tab_ctrl, 0, 0, w, h - BAR_HEIGHT, TRUE);

    RECT view_rc = { 0, 0, w, h - BAR_HEIGHT };
    SendMessageA(he->tab_ctrl, TCM_ADJUSTRECT, FALSE, (LPARAM)&view_rc);
    MoveWindow(he->view, view_rc.left, view_rc.top,
              view_rc.right - view_rc.left, view_rc.bottom - view_rc.top, TRUE);

    int y = h - BAR_HEIGHT + 4, x = 4;
    MoveWindow(GetDlgItem(he->hwnd, 900), x, y + 2, 36, 18, TRUE); x += 38;
    MoveWindow(he->goto_edit, x, y, 56, 20, TRUE); x += 64;
    MoveWindow(GetDlgItem(he->hwnd, 901), x, y + 2, 34, 18, TRUE); x += 34;
    MoveWindow(he->addr_edit, x, y, 50, 20, TRUE); x += 58;
    MoveWindow(GetDlgItem(he->hwnd, 902), x, y + 2, 38, 18, TRUE); x += 38;
    MoveWindow(he->value_edit, x, y, 32, 20, TRUE); x += 40;
    MoveWindow(he->write_btn, x, y - 1, 50, 22, TRUE); x += 58;
    MoveWindow(he->status, x, y + 2, (w - x - 4) > 0 ? w - x - 4 : 0, 18, TRUE);
}

static void switch_to_tab(HexEditor *he, int index) {
    if (index < 0 || index >= he->n_tabs) return;
    he->cur_tab = index;
    he->has_selection = false;
    he->edit_nibble = 0;
    EnableWindow(he->write_btn, FALSE);
    set_status(he, "");

    HexEditorTab *tab = &he->tabs[index];
    SCROLLINFO si = { .cbSize = sizeof(si), .fMask = SIF_RANGE | SIF_PAGE | SIF_POS };
    RECT rc; GetClientRect(he->view, &rc);
    int visible = he->char_h > 0 ? (rc.bottom / he->char_h) : 1;
    int n_lines = tab_line_count(tab);
    si.nMin = 0; si.nMax = n_lines > 0 ? n_lines - 1 : 0;
    si.nPage = (UINT)visible; si.nPos = tab->scroll_line;
    SetScrollInfo(he->view, SB_VERT, &si, TRUE);

    InvalidateRect(he->view, NULL, FALSE);
}

static LRESULT CALLBACK win_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HexEditor *he = (HexEditor *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_SIZE:
        if (he) relayout(he);
        return 0;
    case WM_CLOSE:
        if (he) hex_editor_hide(he);
        return 0;
    case WM_COMMAND:
        if (he && LOWORD(wp) == IDC_WRITE && HIWORD(wp) == BN_CLICKED) {
            on_write(he);
            return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (he && nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) {
            int idx = (int)SendMessageA(he->tab_ctrl, TCM_GETCURSEL, 0, 0);
            switch_to_tab(he, idx);
            return 0;
        }
        break;
    }
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

HexEditor *hex_editor_create(const HexRegion *regions, int n_regions) {
    if (!regions || n_regions <= 0 || n_regions > HEX_MAX_TABS) return NULL;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    HINSTANCE hinst = GetModuleHandleA(NULL);

    if (!g_view_class) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc   = view_wndproc;
        wc.hInstance     = hinst;
        wc.lpszClassName = "GemuHexView";
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_IBEAM);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.style         = CS_VREDRAW | CS_HREDRAW;
        g_view_class = RegisterClassA(&wc);
    }
    if (!g_win_class) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc   = win_wndproc;
        wc.hInstance     = hinst;
        wc.lpszClassName = "GemuHexEditorWindow";
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        g_win_class = RegisterClassA(&wc);
    }

    HexEditor *he = calloc(1, sizeof(*he));
    if (!he) return NULL;
    he->cur_tab = -1;

    he->hwnd = CreateWindowExA(0, "GemuHexEditorWindow", "GEMU \xE2\x80\x94 Hex Editor",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 680, 540,
        NULL, NULL, hinst, NULL);
    if (!he->hwnd) { free(he); return NULL; }
    SetWindowLongPtrA(he->hwnd, GWLP_USERDATA, (LONG_PTR)he);

    he->font = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    if (!he->font)
        he->font = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
    {
        HDC hdc = GetDC(he->hwnd);
        HFONT old = (HFONT)SelectObject(hdc, he->font);
        TEXTMETRICA tm;
        GetTextMetricsA(hdc, &tm);
        he->char_w = tm.tmAveCharWidth;
        he->char_h = tm.tmHeight + tm.tmExternalLeading;
        SelectObject(hdc, old);
        ReleaseDC(he->hwnd, hdc);
    }

    he->tab_ctrl = CreateWindowExA(0, "SysTabControl32", "", WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, he->hwnd, (HMENU)IDC_TAB, hinst, NULL);
    SendMessageA(he->tab_ctrl, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

    for (int i = 0; i < n_regions; i++) {
        HexEditorTab *tab = &he->tabs[he->n_tabs++];
        *tab = (HexEditorTab){
            .label = regions[i].label, .data = regions[i].data,
            .size = regions[i].size, .read_only = regions[i].read_only,
            .base_addr = regions[i].base_addr,
        };
        TCITEMA item = { .mask = TCIF_TEXT };
        item.pszText = (char *)regions[i].label;
        SendMessageA(he->tab_ctrl, TCM_INSERTITEMA, (WPARAM)(i), (LPARAM)&item);
    }

    he->view = CreateWindowExA(WS_EX_CLIENTEDGE, "GemuHexView", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
        0, 0, 0, 0, he->hwnd, (HMENU)IDC_VIEW, hinst, NULL);
    SetWindowLongPtrA(he->view, GWLP_USERDATA, (LONG_PTR)he);
    SendMessageA(he->view, WM_SETFONT, (WPARAM)he->font, TRUE);

    #define MKLABEL(id, text) \
        CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, \
                        0, 0, 0, 0, he->hwnd, (HMENU)(id), hinst, NULL)
    MKLABEL(900, "Goto:");
    MKLABEL(901, "Addr:");
    MKLABEL(902, "Value:");
    #undef MKLABEL

    he->goto_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
        he->hwnd, (HMENU)IDC_GOTO, hinst, NULL);
    he->addr_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY, 0, 0, 0, 0,
        he->hwnd, (HMENU)IDC_ADDR, hinst, NULL);
    he->value_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
        he->hwnd, (HMENU)IDC_VALUE, hinst, NULL);
    SendMessageA(he->value_edit, EM_SETLIMITTEXT, 2, 0);
    he->write_btn = CreateWindowExA(0, "BUTTON", "Write",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
        he->hwnd, (HMENU)IDC_WRITE, hinst, NULL);
    EnableWindow(he->write_btn, FALSE);
    he->status = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, he->hwnd, (HMENU)IDC_STATUS, hinst, NULL);

    HFONT gui_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    for (int id = 900; id <= 902; id++)
        SendMessageA(GetDlgItem(he->hwnd, id), WM_SETFONT, (WPARAM)gui_font, TRUE);
    SendMessageA(he->goto_edit, WM_SETFONT, (WPARAM)gui_font, TRUE);
    SendMessageA(he->addr_edit, WM_SETFONT, (WPARAM)gui_font, TRUE);
    SendMessageA(he->value_edit, WM_SETFONT, (WPARAM)gui_font, TRUE);
    SendMessageA(he->write_btn, WM_SETFONT, (WPARAM)gui_font, TRUE);
    SendMessageA(he->status, WM_SETFONT, (WPARAM)gui_font, TRUE);

    SetWindowSubclass(he->goto_edit, goto_subclass, 1, (DWORD_PTR)he);
    SetWindowSubclass(he->value_edit, value_subclass, 2, (DWORD_PTR)he);

    switch_to_tab(he, 0);
    relayout(he);
    return he;
}

void hex_editor_destroy(HexEditor *he) {
    if (!he) return;
    if (he->hwnd) DestroyWindow(he->hwnd);
    if (he->font && he->font != (HFONT)GetStockObject(SYSTEM_FIXED_FONT))
        DeleteObject(he->font);
    free(he);
}

void hex_editor_show(HexEditor *he) {
    if (!he || he->visible) return;
    he->visible = true;
    ShowWindow(he->hwnd, SW_SHOW);
    SetForegroundWindow(he->hwnd);
}

void hex_editor_hide(HexEditor *he) {
    if (!he || !he->visible) return;
    he->visible = false;
    ShowWindow(he->hwnd, SW_HIDE);
}

bool hex_editor_is_visible(const HexEditor *he) {
    return he && he->visible;
}

void hex_editor_refresh(HexEditor *he) {
    if (!he || !he->visible) return;

    /* Throttle to ~15 Hz, matching the GTK implementation. */
    he->refresh_skip = (he->refresh_skip + 1) & 3;
    if (he->refresh_skip != 0) return;

    InvalidateRect(he->view, NULL, FALSE);

    /* Pump this window's message queue here: hex_editor_refresh() is called
     * from each machine's own run loop, which never dispatches messages for
     * any window besides the main display. */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

#endif /* _WIN32 && !GEMU_GTK */
