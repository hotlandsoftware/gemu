#ifdef HAVE_CACA
#include "rca_display.h"
#include "../hardware/vip.h"
#include "../hardware/studio2.h"
#include "../devices/vip_devices.h"
#include <caca.h>
#include <stdlib.h>
#include <string.h>

static const int vip_key_map[16] = {
    'x', '1', '2', '3', 'q', 'w', 'e', 'a',
    's', 'd', 'z', 'c', '4', 'r', 'f', 'v',
};

static const int studio2_keys_a[10] = {
    ' ', 'q', 'w', 'e', 'a', 's', 'd', 'z', 'x', 'c',
};
static const int studio2_keys_b[10] = {
    '0', '7', '8', '9', '4', '5', '6', '1', '2', '3',
};

/* Half-block chars indexed by (top<<1)|bot: space ▄ ▀ █ */
static const uint32_t half_blk[4] = { ' ', 0x2584, 0x2580, 0x2588 };

typedef struct {
    caca_canvas_t  *cv;
    caca_display_t *dp;
} RcaCacaCtx;

static void caca_render(void *ctx, const uint8_t *vram, int w, int h) {
    RcaCacaCtx *c = ctx;
    int cw = caca_get_canvas_width(c->cv);
    int ch = caca_get_canvas_height(c->cv);

    for (int y = 0; y < h && y / 2 < ch; y += 2) {
        for (int x = 0; x < w && x < cw; x++) {
            int top = vram[y * w + x] ? 1 : 0;
            int bot = (y + 1 < h) ? (vram[(y + 1) * w + x] ? 1 : 0) : 0;
            if (top || bot)
                caca_set_color_ansi(c->cv, CACA_WHITE, CACA_BLACK);
            else
                caca_set_color_ansi(c->cv, CACA_BLACK, CACA_BLACK);
            caca_put_char(c->cv, x, y / 2, half_blk[(top << 1) | bot]);
        }
    }
    caca_refresh_display(c->dp);
}

static void caca_destroy(void *ctx) {
    RcaCacaCtx *c = ctx;
    if (!c) return;
    if (c->dp) caca_free_display(c->dp);
    if (c->cv) caca_free_canvas(c->cv);
    free(c);
}

RcaDisplay *rca_display_caca_create(void) {
    RcaCacaCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->cv = caca_create_canvas(0, 0);
    c->dp = caca_create_display(c->cv);
    if (!c->dp) {
        caca_free_canvas(c->cv);
        free(c);
        return NULL;
    }
    caca_set_display_title(c->dp, "GEMU");

    RcaDisplay *d = calloc(1, sizeof(*d));
    if (!d) { caca_destroy(c); return NULL; }
    d->render  = caca_render;
    d->destroy = caca_destroy;
    d->ctx     = c;
    return d;
}

void rca_display_caca_poll_vip(RcaDisplay *d, RcaVipState *s, bool *quit) {
    RcaCacaCtx *c = d->ctx;

    memset(s->keys, 0, sizeof(s->keys));
    s->key_down = -1;
    s->ef2_down = false;

    caca_event_t ev;
    while (caca_get_event(c->dp, CACA_EVENT_KEY_PRESS, &ev, 0) != 0) {
        int key = caca_get_event_key_ch(&ev);
        if (key == CACA_KEY_ESCAPE || key == 3) { *quit = true; continue; }
        if (key == '\t') s->ef2_down = true;

        if (rca_vip_has_keypad(s->cfg)) {
            for (int k = 0; k < 16; k++) {
                if (key == vip_key_map[k]) {
                    s->keys[k] = 1;
                    s->key_down = k;
                    break;
                }
            }
        }
    }
}

void rca_display_caca_poll_studio2(RcaDisplay *d, RcaStudio2State *s,
                                   bool *quit, bool *reset) {
    RcaCacaCtx *c = d->ctx;

    memset(s->keys_a, 0, sizeof(s->keys_a));
    memset(s->keys_b, 0, sizeof(s->keys_b));

    caca_event_t ev;
    while (caca_get_event(c->dp, CACA_EVENT_KEY_PRESS, &ev, 0) != 0) {
        int key = caca_get_event_key_ch(&ev);
        if (key == CACA_KEY_ESCAPE || key == 3) { *quit = true; continue; }
        if (key == 'r') { *reset = true; continue; }
        for (int i = 0; i < 10; i++) {
            if (key == studio2_keys_a[i]) { s->keys_a[i] = true; break; }
            if (key == studio2_keys_b[i]) { s->keys_b[i] = true; break; }
        }
    }
}

#endif /* HAVE_CACA */
