#pragma once
/* Debug > Cartridge Pins: the Famicom's 60-pin cartridge connector, drawn
 * as gold contacts that can be clicked open (or flaky), with the CPU clock
 * and the /NMI and /IRQ test points (see nes_pins.h for what a fault does).
 *
 * nes_pin_panel.c draws the window into an ARGB buffer and handles the
 * mouse; the backends only show it: nes_pin_window_sdl.c (SDL builds,
 * Linux and Windows) and nes_pin_window_gtk.c (GTK builds, used with
 * -display gtk). */
#include "nes_pins.h"
#include <stdbool.h>
#include <stdint.h>

#define NES_PIN_PANEL_W 640
#define NES_PIN_PANEL_H 384
#define NES_PIN_CLOCK_MIN 100000u
#define NES_PIN_CLOCK_MAX 4000000u

typedef enum {
    NES_PIN_MOUSE_MOVE, NES_PIN_MOUSE_LEFT, NES_PIN_MOUSE_RIGHT,
    NES_PIN_MOUSE_UP, NES_PIN_MOUSE_LEAVE,
} NesPinMouse;

typedef struct {
    NesPins *pins;
    int      hover_pin;         /* 0 = none */
    int      hover_ctl;         /* a control under the mouse, or -1 */
    bool     dragging;          /* the clock slider */
    uint32_t tp_lit_until[3];   /* /NMI, APU /IRQ, MAP /IRQ */
    uint32_t now;               /* ms, as of the last draw */
} NesPinPanel;

void nes_pin_panel_init (NesPinPanel *p, NesPins *pins);
void nes_pin_panel_draw (NesPinPanel *p, uint32_t *argb, uint32_t now_ms);   /* W x H */
void nes_pin_panel_mouse(NesPinPanel *p, int x, int y, NesPinMouse ev);
void nes_pin_panel_wheel(NesPinPanel *p, int x, int y, int notches);        /* + = up */

typedef struct NesPinWindow {
    void (*show)(struct NesPinWindow *);
    void (*hide)(struct NesPinWindow *);
    bool (*visible)(const struct NesPinWindow *);
    void (*refresh)(struct NesPinWindow *);     /* once per frame */
    void (*destroy)(struct NesPinWindow *);
} NesPinWindow;

NesPinWindow *nes_pin_window_sdl_create(NesPins *pins);
#ifdef GEMU_GTK
NesPinWindow *nes_pin_window_gtk_create(NesPins *pins);
#endif

static inline void nes_pin_window_toggle(NesPinWindow *w) {
    if (!w) return;
    if (w->visible(w)) w->hide(w);
    else w->show(w);
}
