/* Screens: see gemu/dev/screen.h. */
#include "dev_priv.h"
#include "gemu/dev/screen.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct GemuScreen {
    GemuMachine *m;
    GemuDevice  *dev;
    char         name[32];
    GemuScreen  *next;
    GemuBitmap   bmp;
    int          aspect_w, aspect_h;
    /* raw timing */
    uint64_t     pixel_hz;
    int          htotal, hbend, hbstart, vtotal, vbend, vbstart;
    bool         self_timed;
    GemuTimer   *t_frame, *t_von, *t_voff;
    void       (*update)(void *ud, GemuBitmap *bmp, const GemuRect *clip);
    void        *ud;
    /* the frame */
    uint64_t     frame;
    GemuTime     frame_start;
    int          next_row;          /* lines [0, next_row) drawn by update_partial */
    GemuLine    *vblank;
};

static bool valid_name(const char *n) {
    if (!n || !*n || strlen(n) >= 24)
        return false;
    for (; *n; n++)
        if (!((*n >= 'a' && *n <= 'z') || (*n >= 'A' && *n <= 'Z') || (*n >= '0' && *n <= '9') ||
              *n == '_' || *n == '-'))
            return false;
    return true;
}

GemuScreen *gemu_screen_new(GemuDevice *d, const char *name) {
    if (!d || !valid_name(name))
        return NULL;
    GemuMachine *m = d->machine;
    for (GemuScreen *x = m->screens; x; x = x->next)
        if (x->dev == d && !strcmp(x->name, name))
            return NULL;
    GemuScreen *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    char line[48];
    snprintf(line, sizeof line, "%s-vblank", name);
    if (!(s->vblank = gemu_line_out(d, line))) {
        free(s);
        return NULL;
    }
    s->m = m;
    s->dev = d;
    snprintf(s->name, sizeof s->name, "%s", name);
    /* the frame count and start are the device's state too */
    char item[64];
    snprintf(item, sizeof item, "%s.frame", name);
    gemu_state_add(d, item, &s->frame, sizeof s->frame, 1);
    snprintf(item, sizeof item, "%s.frame_start", name);
    gemu_state_add(d, item, &s->frame_start, sizeof s->frame_start, 1);
    s->frame_start = gemu_now(m);
    /* append: screens are listed in the order made */
    GemuScreen **pp = &m->screens;
    while (*pp)
        pp = &(*pp)->next;
    *pp = s;
    return s;
}

const char *gemu_screen_name(const GemuScreen *s) { return s->name; }
GemuDevice *gemu_screen_device(const GemuScreen *s) { return s->dev; }
GemuLine *gemu_screen_vblank(GemuScreen *s) { return s->vblank; }
uint64_t gemu_screen_frame_number(const GemuScreen *s) { return s->frame; }
GemuTime gemu_screen_frame_start(const GemuScreen *s) { return s->frame_start; }

bool gemu_screen_set_size(GemuScreen *s, int w, int h) {
    if (w <= 0 || h <= 0 || (size_t)w * (size_t)h > ((size_t)1 << 28))
        return false;
    uint32_t *p = calloc((size_t)w * (size_t)h, sizeof *p);
    if (!p)
        return false;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
        p[i] = 0xFF000000u;
    free(s->bmp.pix);
    s->bmp = (GemuBitmap){ p, w, h, w };
    s->next_row = 0;
    return true;
}

void gemu_screen_set_aspect(GemuScreen *s, int aw, int ah) {
    s->aspect_w = aw > 0 && ah > 0 ? aw : 0;
    s->aspect_h = aw > 0 && ah > 0 ? ah : 0;
}

void gemu_screen_get_aspect(const GemuScreen *s, int *aw, int *ah) {
    *aw = s->aspect_w;
    *ah = s->aspect_h;
}

void gemu_screen_set_update(GemuScreen *s,
        void (*update)(void *ud, GemuBitmap *bmp, const GemuRect *clip), void *ud) {
    s->update = update;
    s->ud = ud;
}

void gemu_screen_frame_begin(GemuScreen *s) {
    s->frame++;
    s->frame_start = gemu_now(s->m);
    s->next_row = 0;
}

void gemu_screen_set_vblank(GemuScreen *s, bool level) { gemu_line_set(s->vblank, level); }

/* Self-timed: the frame starts (line 0), vblank begins (vbstart), ends (vbend). */
static void on_frame(void *ud, int param) {
    (void)param;
    GemuScreen *s = ud;
    gemu_screen_frame_begin(s);
    if (s->vbend == 0)
        gemu_line_set(s->vblank, false);
}
static void on_vblank(void *ud, int level) { gemu_line_set(((GemuScreen *)ud)->vblank, level); }

bool gemu_screen_set_raw(GemuScreen *s, uint64_t pixel_hz, int htotal, int hbend, int hbstart,
                         int vtotal, int vbend, int vbstart, bool self_timed) {
    if (!pixel_hz || htotal <= 0 || vtotal <= 0 || vbstart < 0 || vbstart > vtotal ||
        vbend < 0 || vbend >= vtotal || hbstart < 0 || hbstart > htotal || hbend < 0 || hbend >= htotal)
        return false;
    s->pixel_hz = pixel_hz;
    s->htotal = htotal; s->hbend = hbend; s->hbstart = hbstart;
    s->vtotal = vtotal; s->vbend = vbend; s->vbstart = vbstart;
    s->self_timed = self_timed;
    if (!self_timed) {
        if (s->t_frame) gemu_timer_stop(s->t_frame);
        if (s->t_von)   gemu_timer_stop(s->t_von);
        if (s->t_voff)  gemu_timer_stop(s->t_voff);
        return true;
    }
    if (!s->t_frame) {
        s->t_frame = gemu_timer_new(s->dev, on_frame, s);
        s->t_von = gemu_timer_new(s->dev, on_vblank, s);
        s->t_voff = gemu_timer_new(s->dev, on_vblank, s);
        if (!s->t_frame || !s->t_von || !s->t_voff)
            return false;
    }
    uint64_t frame_px = (uint64_t)htotal * (uint64_t)vtotal;
    /* a frame starts now */
    s->frame_start = gemu_now(s->m);
    s->next_row = 0;
    gemu_line_set(s->vblank, vbend > 0);           /* in the first vbend lines? */
    gemu_timer_start_clocks(s->t_frame, pixel_hz, frame_px, frame_px, 0);
    if (vbstart < vtotal)
        gemu_timer_start_clocks(s->t_von, pixel_hz, (uint64_t)vbstart * (uint64_t)htotal,
                                frame_px, 1);
    else
        gemu_timer_stop(s->t_von);
    if (vbend > 0)
        gemu_timer_start_clocks(s->t_voff, pixel_hz, (uint64_t)vbend * (uint64_t)htotal,
                                frame_px, 0);
    else
        gemu_timer_stop(s->t_voff);
    return true;
}

static uint64_t pixels_in(GemuScreen *s) {
    if (!s->pixel_hz || !s->htotal)
        return 0;
    GemuTime now = gemu_now(s->m);
    return now > s->frame_start ? gemu_cycles_in(s->pixel_hz, now - s->frame_start) : 0;
}

int gemu_screen_vpos(GemuScreen *s) {
    uint64_t v = s->htotal ? pixels_in(s) / (uint64_t)s->htotal : 0;
    return v > INT_MAX ? INT_MAX : (int)v;
}

int gemu_screen_hpos(GemuScreen *s) {
    return s->htotal ? (int)(pixels_in(s) % (uint64_t)s->htotal) : 0;
}

void gemu_screen_update_partial(GemuScreen *s, int line) {
    if (!s->bmp.pix || !s->update)
        return;
    if (line >= s->bmp.height)
        line = s->bmp.height - 1;
    if (line < s->next_row)
        return;
    GemuRect clip = { 0, s->next_row, s->bmp.width, line + 1 };
    s->update(s->ud, &s->bmp, &clip);
    s->next_row = line + 1;
}

const GemuBitmap *gemu_screen_render(GemuScreen *s) {
    if (s->bmp.pix && s->update && s->next_row < s->bmp.height) {
        GemuRect clip = { 0, s->next_row, s->bmp.width, s->bmp.height };
        s->update(s->ud, &s->bmp, &clip);       /* not kept: drawn fresh next time too */
    }
    return &s->bmp;
}

GemuScreen *gemu_screen_first(GemuMachine *m) { return m->screens; }
GemuScreen *gemu_screen_next(GemuScreen *s) { return s->next; }

static void free_screen(GemuScreen *s) {
    free(s->bmp.pix);
    free(s);
}

void gemu_screens_remove_device(GemuDevice *d) {
    for (GemuScreen **pp = &d->machine->screens; *pp;) {
        GemuScreen *s = *pp;
        if (s->dev == d) {
            *pp = s->next;
            free_screen(s);     /* its timers and line go with the device */
        } else {
            pp = &s->next;
        }
    }
}

void gemu_screens_free_all(GemuMachine *m) {
    while (m->screens) {
        GemuScreen *s = m->screens;
        m->screens = s->next;
        free_screen(s);
    }
}
