/* The PC's display adapters as core devices: see pc_video_dev.h. */
#include "pc_video_dev.h"
#include "cgafont8x8.h"
#include "mdafont8x14.h"
#include "gemu/dev/option.h"
#include "gemu/dev/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every card's device state starts with its screen. */
typedef struct { GemuScreen *screen; } Head;
GemuScreen *gemu_pc_video_screen(GemuDevice *d) { return ((Head *)d->priv)->screen; }

/* The cards render whole frames: straight into the bitmap when the clip is
 * all of it, else into a scratch frame and copied (partial updates). */
static void blit_rows(GemuBitmap *bmp, const GemuRect *clip, void (*draw)(void *ud, uint32_t *argb),
                      void *ud) {
    if (clip->y0 == 0 && clip->y1 == bmp->height && bmp->stride == bmp->width) {
        draw(ud, bmp->pix);
        return;
    }
    uint32_t *tmp = malloc((size_t)bmp->width * (size_t)bmp->height * sizeof *tmp);
    if (!tmp)
        return;
    draw(ud, tmp);
    for (int y = clip->y0; y < clip->y1; y++)
        memcpy(&bmp->pix[(size_t)y * (size_t)bmp->stride + (size_t)clip->x0],
               &tmp[(size_t)y * (size_t)bmp->width + (size_t)clip->x0],
               (size_t)(clip->x1 - clip->x0) * sizeof *tmp);
    free(tmp);
}

static const char *const phosphors[] = { "green", "amber", "white", NULL };   /* MdaPhosphor order */
static const GemuOptionDef mono_opts[] = {
    { "phosphor", "the monitor's phosphor", GEMU_OPT_CHOICE, "green", 0, 0, phosphors },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};

/* ── CGA ──────────────────────────────────────────────────────────────────── */

#define CGA_W 640
#define CGA_H 400

typedef struct { GemuScreen *screen; CgaDevice card; } Cga;

CgaDevice *gemu_cga(GemuDevice *d) { return &((Cga *)d->priv)->card; }

bool gemu_cga_tick(GemuDevice *d, uint32_t clocks) {
    Cga *p = d->priv;
    if (!cga_tick(&p->card, clocks))
        return false;
    gemu_screen_frame_begin(p->screen);
    return true;
}

static void cga_draw(void *ud, uint32_t *argb) { cga_render(ud, argb, CGA_W, CGA_H, cgafont8x8); }
static void cga_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    blit_rows(bmp, clip, cga_draw, &((Cga *)ud)->card);
}

static CgaDevice *cga_of(void *ud) { return &((Cga *)((GemuDevice *)ud)->priv)->card; }
static uint8_t cga_r(void *ud, uint64_t o) { return cga_io_read(cga_of(ud), (uint16_t)(0x3D0 + o)); }
static void cga_w(void *ud, uint64_t o, uint8_t v) { cga_io_write(cga_of(ud), (uint16_t)(0x3D0 + o), v); }
const GemuHandlers gemu_cga_io = { .r8 = cga_r, .w8 = cga_w };

static bool cga_init(GemuDevice *d) {
    Cga *p = d->priv;
    CgaDevice *c = &p->card;
    if (!(p->screen = gemu_screen_new(d, "screen")) || !gemu_screen_set_size(p->screen, CGA_W, CGA_H))
        return false;
    /* 14.318 MHz dots, 912 a line (304 CPU clocks), 262 lines, 200 shown */
    gemu_screen_set_raw(p->screen, 14318180, 912, 0, 640, 262, 0, 200, false);
    gemu_screen_set_update(p->screen, cga_update, p);
    GEMU_STATE_ARRAY(d, "vram", c->vram);
    GEMU_STATE_VAR(d, "mode_ctrl", c->mode_ctrl);
    GEMU_STATE_VAR(d, "color_select", c->color_select);
    GEMU_STATE_VAR(d, "crtc_index", c->crtc_index);
    GEMU_STATE_ARRAY(d, "crtc", c->crtc);
    GEMU_STATE_VAR(d, "beam", c->beam);
    GEMU_STATE_VAR(d, "frame", c->frame);
    return true;
}
static void cga_reset_dev(GemuDevice *d, GemuResetKind k) { (void)k; cga_reset(gemu_cga(d)); }
static void cga_describe(GemuDevice *d, char *buf, size_t len) {
    const CgaDevice *c = gemu_cga(d);
    const char *mode = (c->mode_ctrl & 0x02) ? ((c->mode_ctrl & 0x10) ? "640x200 graphics" : "320x200 graphics")
                                             : ((c->mode_ctrl & 0x01) ? "80x25 text" : "40x25 text");
    snprintf(buf, len, "%s, video %s; mode %02X, colour %02X\n"
             "start %04X, cursor %04X; beam line %u, frame %u",
             mode, (c->mode_ctrl & 0x08) ? "on" : "off", c->mode_ctrl, c->color_select,
             (unsigned)(c->crtc[12] << 8 | c->crtc[13]), (unsigned)(c->crtc[14] << 8 | c->crtc[15]),
             c->beam / CGA_CLOCKS_PER_LINE, c->frame);
}
const GemuDeviceType gemu_cga_type = {
    .name = "cga", .desc = "IBM Color Graphics Adapter", .size = sizeof(Cga),
    .init = cga_init, .reset = cga_reset_dev, .describe = cga_describe,
};

/* ── MDA ──────────────────────────────────────────────────────────────────── */

typedef struct { GemuScreen *screen; MdaDevice card; } Mda;

MdaDevice *gemu_mda(GemuDevice *d) { return &((Mda *)d->priv)->card; }

bool gemu_mda_tick(GemuDevice *d, uint32_t clocks) {
    Mda *p = d->priv;
    if (!mda_tick(&p->card, clocks))
        return false;
    gemu_screen_frame_begin(p->screen);
    return true;
}

static void mda_draw(void *ud, uint32_t *argb) { mda_render(ud, argb, mdafont8x14); }
static void mda_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    blit_rows(bmp, clip, mda_draw, &((Mda *)ud)->card);
}

static MdaDevice *mda_of(void *ud) { return &((Mda *)((GemuDevice *)ud)->priv)->card; }
static uint8_t mda_r(void *ud, uint64_t o) { return mda_io_read(mda_of(ud), (uint16_t)(0x3B0 + o)); }
static void mda_w(void *ud, uint64_t o, uint8_t v) { mda_io_write(mda_of(ud), (uint16_t)(0x3B0 + o), v); }
const GemuHandlers gemu_mda_io = { .r8 = mda_r, .w8 = mda_w };

static bool mda_init(GemuDevice *d) {
    Mda *p = d->priv;
    MdaDevice *c = &p->card;
    c->phosphor = (MdaPhosphor)gemu_opt_choice(d, "phosphor");
    if (!(p->screen = gemu_screen_new(d, "screen")) ||
        !gemu_screen_set_size(p->screen, MDA_WIDTH, MDA_HEIGHT))
        return false;
    gemu_screen_set_aspect(p->screen, 4, 3);
    /* 16.257 MHz dots, 882 a line, 370 lines, 350 shown */
    gemu_screen_set_raw(p->screen, 16257000, 882, 0, 720, 370, 0, 350, false);
    gemu_screen_set_update(p->screen, mda_update, p);
    GEMU_STATE_ARRAY(d, "vram", c->vram);
    GEMU_STATE_VAR(d, "mode_ctrl", c->mode_ctrl);
    GEMU_STATE_VAR(d, "crtc_index", c->crtc_index);
    GEMU_STATE_ARRAY(d, "crtc", c->crtc);
    GEMU_STATE_VAR(d, "beam", c->beam);
    GEMU_STATE_VAR(d, "frame", c->frame);
    return true;
}
static void mda_reset_dev(GemuDevice *d, GemuResetKind k) { (void)k; mda_reset(gemu_mda(d)); }
static void mda_describe(GemuDevice *d, char *buf, size_t len) {
    const MdaDevice *c = gemu_mda(d);
    snprintf(buf, len, "80x25 text, video %s, blink %s; mode %02X; %s phosphor\n"
             "start %04X, cursor %04X; frame %u",
             (c->mode_ctrl & 0x08) ? "on" : "off", (c->mode_ctrl & 0x20) ? "on" : "off",
             c->mode_ctrl, phosphors[c->phosphor],
             (unsigned)(c->crtc[12] << 8 | c->crtc[13]), (unsigned)(c->crtc[14] << 8 | c->crtc[15]),
             c->frame);
}
const GemuDeviceType gemu_mda_type = {
    .name = "mda", .desc = "IBM Monochrome Display Adapter", .size = sizeof(Mda),
    .options = mono_opts, .init = mda_init, .reset = mda_reset_dev, .describe = mda_describe,
};

/* ── Hercules ─────────────────────────────────────────────────────────────── */

typedef struct { GemuScreen *screen; HerculesDevice card; } Herc;

HerculesDevice *gemu_hercules(GemuDevice *d) { return &((Herc *)d->priv)->card; }

bool gemu_hercules_tick(GemuDevice *d, uint32_t clocks, uint32_t cpu_hz) {
    Herc *p = d->priv;
    if (!hercules_tick(&p->card, clocks, cpu_hz))
        return false;
    gemu_screen_frame_begin(p->screen);
    return true;
}

static void herc_draw(void *ud, uint32_t *argb) { hercules_render(ud, argb, mdafont8x14); }
static void herc_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    blit_rows(bmp, clip, herc_draw, &((Herc *)ud)->card);
}

static HerculesDevice *herc_of(void *ud) { return &((Herc *)((GemuDevice *)ud)->priv)->card; }
static uint8_t herc_r(void *ud, uint64_t o) { return hercules_io_read(herc_of(ud), (uint16_t)(0x3B0 + o)); }
static void herc_w(void *ud, uint64_t o, uint8_t v) { hercules_io_write(herc_of(ud), (uint16_t)(0x3B0 + o), v); }
static uint8_t hcfg_r(void *ud, uint64_t o) { return hercules_io_read(herc_of(ud), (uint16_t)(0x3BF + o)); }
static void hcfg_w(void *ud, uint64_t o, uint8_t v) { hercules_io_write(herc_of(ud), (uint16_t)(0x3BF + o), v); }
const GemuHandlers gemu_hercules_io = { .r8 = herc_r, .w8 = herc_w };
const GemuHandlers gemu_hercules_cfg_io = { .r8 = hcfg_r, .w8 = hcfg_w };

static bool herc_init(GemuDevice *d) {
    Herc *p = d->priv;
    HerculesDevice *c = &p->card;
    c->phosphor = (MdaPhosphor)gemu_opt_choice(d, "phosphor");
    if (!(p->screen = gemu_screen_new(d, "screen")) ||
        !gemu_screen_set_size(p->screen, HERCULES_WIDTH, HERCULES_HEIGHT))
        return false;
    gemu_screen_set_aspect(p->screen, 4, 3);
    /* text mode's timing (graphics mode's comes from the CRTC) */
    gemu_screen_set_raw(p->screen, 16257000, 882, 0, 720, 370, 0, 350, false);
    gemu_screen_set_update(p->screen, herc_update, p);
    GEMU_STATE_ARRAY(d, "vram", c->vram);
    GEMU_STATE_VAR(d, "mode_ctrl", c->mode_ctrl);
    GEMU_STATE_VAR(d, "config", c->config);
    GEMU_STATE_VAR(d, "crtc_index", c->crtc_index);
    GEMU_STATE_ARRAY(d, "crtc", c->crtc);
    GEMU_STATE_VAR(d, "dots", c->dots);
    GEMU_STATE_VAR(d, "dot_frac", c->dot_frac);
    GEMU_STATE_VAR(d, "frame", c->frame);
    return true;
}
static void herc_reset_dev(GemuDevice *d, GemuResetKind k) { (void)k; hercules_reset(gemu_hercules(d)); }
static void herc_describe(GemuDevice *d, char *buf, size_t len) {
    const HerculesDevice *c = gemu_hercules(d);
    snprintf(buf, len, "%s, page %d, video %s; mode %02X, configuration %02X; %s phosphor\n"
             "start %04X; frame %u",
             (c->mode_ctrl & 0x02) ? "720x348 graphics" : "80x25 text", (c->mode_ctrl >> 7) & 1,
             (c->mode_ctrl & 0x08) ? "on" : "off", c->mode_ctrl, c->config, phosphors[c->phosphor],
             (unsigned)(c->crtc[12] << 8 | c->crtc[13]), c->frame);
}
const GemuDeviceType gemu_hercules_type = {
    .name = "hercules", .desc = "Hercules Graphics Card", .size = sizeof(Herc),
    .options = mono_opts, .init = herc_init, .reset = herc_reset_dev, .describe = herc_describe,
};
