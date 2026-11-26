/* Screens (core/src/dev/screen.c).  Run: ninja check-screen */
#include "gemu/dev/line.h"
#include "gemu/dev/screen.h"
#include "gemu/dev/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

/* A TMS9918-like timing: 342 pixels x 262 lines at 5.369318 MHz, vblank
 * from line 192 to the end of the frame. */
#define PIX_HZ 5369318u
#define HT 342
#define VT 262
#define FRAME_PX ((uint64_t)HT * VT)

static GemuMachine *m;

/* The device: draws each line in a colour from its "register" */
typedef struct {
    uint32_t colour;
    unsigned updates;
    GemuRect clips[16];
} Vdp;
static void vdp_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    Vdp *v = ud;
    if (v->updates < 16)
        v->clips[v->updates] = *clip;
    v->updates++;
    for (int y = clip->y0; y < clip->y1; y++)
        for (int x = clip->x0; x < clip->x1; x++)
            bmp->pix[y * bmp->stride + x] = v->colour;
}
static bool vdp_init(GemuDevice *d) { return GEMU_STATE_VAR(d, "colour", ((Vdp *)d->priv)->colour); }
static const GemuDeviceType vdp_type = { .name = "vdp", .size = sizeof(Vdp), .init = vdp_init };

/* The CPU's INT pin: vblank edges, with the time they came */
typedef struct { unsigned rises, falls; GemuTime last_rise, last_fall; } Pin;
static void pin_cb(void *ud, int idx, bool level) {
    (void)idx;
    Pin *p = ud;
    if (level) { p->rises++; p->last_rise = gemu_now(m); }
    else       { p->falls++; p->last_fall = gemu_now(m); }
}
static bool pin_init(GemuDevice *d) {
    Pin *p = d->priv;
    return GEMU_STATE_VAR(d, "rises", p->rises) && gemu_line_in(d, "int", pin_cb, p, 0);
}
static const GemuDeviceType pin_type = { .name = "pin", .size = sizeof(Pin), .init = pin_init };

/* A probe timer that reads the beam when it fires */
static GemuScreen *probe_screen;
static int probe_v, probe_h;
static void probe(void *ud, int param) {
    (void)ud; (void)param;
    probe_v = gemu_screen_vpos(probe_screen);
    probe_h = gemu_screen_hpos(probe_screen);
}

int main(void) {
    m = gemu_machine_new("msx");
    GemuDevice *vd = gemu_device_add(m, &vdp_type, "vdp", NULL, NULL);
    GemuDevice *pd = gemu_device_add(m, &pin_type, "cpu", NULL, NULL);
    CHECK(gemu_machine_init(m));
    Vdp *v = vd->priv;
    Pin *pin = pd->priv;

    /* ── Making one ── */
    GemuScreen *s = gemu_screen_new(vd, "screen");
    CHECK(s && !strcmp(gemu_screen_name(s), "screen") && gemu_screen_device(s) == vd);
    CHECK(!gemu_screen_new(vd, "screen") && !gemu_screen_new(vd, "a b") && !gemu_screen_new(vd, ""));
    CHECK(gemu_line_find_out(vd, "screen-vblank") == gemu_screen_vblank(s));
    CHECK(gemu_screen_set_size(s, 256, 192));
    CHECK(!gemu_screen_set_size(s, 0, 10) && !gemu_screen_set_size(s, 10, -1));
    gemu_screen_set_aspect(s, 4, 3);
    int aw, ah;
    gemu_screen_get_aspect(s, &aw, &ah);
    CHECK(aw == 4 && ah == 3);
    gemu_screen_set_update(s, vdp_update, v);
    CHECK(!gemu_screen_set_raw(s, 0, HT, 0, 256, VT, 0, 192, true));
    CHECK(!gemu_screen_set_raw(s, PIX_HZ, HT, 0, 256, VT, 0, VT + 1, true));
    CHECK(gemu_screen_first(m) == s && !gemu_screen_next(s));
    CHECK(gemu_lines_connect(m, "vdp:screen-vblank", "cpu:int", 0));
    CHECK(gemu_machine_start(m));

    /* ── Self-timed: frames and vblank exact over many frames ── */
    CHECK(gemu_screen_set_raw(s, PIX_HZ, HT, 0, 256, VT, 0, 192, true));
    CHECK(gemu_screen_frame_number(s) == 0 && gemu_screen_frame_start(s) == 0);
    GemuTime end = GEMU_SEC(10);
    CHECK(gemu_machine_run_until(m, end));
    uint64_t frames = gemu_cycles_in(PIX_HZ, end) / FRAME_PX;
    CHECK(gemu_screen_frame_number(s) == frames);                 /* ~599 */
    CHECK(gemu_screen_frame_start(s) == gemu_clocks(PIX_HZ, frames * FRAME_PX));
    /* vblank rises at line 192 of every frame, falls when the next begins */
    CHECK(pin->rises == frames + (gemu_clocks(PIX_HZ, frames * FRAME_PX + 192u * HT) <= end));
    uint64_t last_k = pin->rises - 1;
    CHECK(pin->last_rise == gemu_clocks(PIX_HZ, last_k * FRAME_PX + 192u * HT));
    CHECK(pin->last_fall == gemu_clocks(PIX_HZ, (uint64_t)pin->falls * FRAME_PX));

    /* ── The beam at gemu_now(): from a timer at a known moment ── */
    probe_screen = s;
    GemuTimer *pt = gemu_timer_new(pd, probe, NULL);
    GemuTime fs = gemu_screen_frame_start(s);
    /* line 100, pixel 17 of the frame that starts after fs */
    GemuTime at = gemu_clocks(PIX_HZ, (frames + 1) * FRAME_PX + 100u * HT + 17);
    gemu_timer_start(pt, at - gemu_now(m), 0, 0);
    CHECK(gemu_machine_run_until(m, at + 1));
    CHECK(probe_v == 100 && (probe_h == 17 || probe_h == 16));  /* rounding to whole pixels */
    CHECK(gemu_screen_frame_start(s) > fs);

    /* ── Rendering: partial lines kept, the rest drawn fresh ── */
    v->colour = 0xFF0000FF;                  /* blue */
    v->updates = 0;
    gemu_screen_update_partial(s, 49);       /* lines 0-49 now */
    CHECK(v->updates == 1 && v->clips[0].y0 == 0 && v->clips[0].y1 == 50 && v->clips[0].x1 == 256);
    gemu_screen_update_partial(s, 30);       /* already drawn: nothing */
    CHECK(v->updates == 1);
    v->colour = 0xFFFF0000;                  /* red from line 50 */
    gemu_screen_update_partial(s, 99);
    CHECK(v->updates == 2 && v->clips[1].y0 == 50 && v->clips[1].y1 == 100);
    v->colour = 0xFF00FF00;                  /* green: what's left */
    const GemuBitmap *b = gemu_screen_render(s);
    CHECK(v->updates == 3 && v->clips[2].y0 == 100 && v->clips[2].y1 == 192);
    CHECK(b->width == 256 && b->height == 192 && b->stride == 256);
    CHECK(b->pix[10 * 256 + 5] == 0xFF0000FF && b->pix[70 * 256] == 0xFFFF0000 &&
          b->pix[150 * 256 + 255] == 0xFF00FF00);
    v->colour = 0xFFFFFFFF;                  /* rendering again: the rest is fresh again */
    b = gemu_screen_render(s);
    CHECK(b->pix[150 * 256] == 0xFFFFFFFF && b->pix[10 * 256] == 0xFF0000FF);
    gemu_screen_update_partial(s, 5000);     /* past the bottom: clamped */
    CHECK(v->clips[4].y0 == 100 && v->clips[4].y1 == 192);
    /* a new frame starts the partial drawing over */
    gemu_machine_run_until(m, gemu_screen_frame_start(s) + gemu_clocks(PIX_HZ, FRAME_PX) + 10);
    v->updates = 0;
    gemu_screen_render(s);
    CHECK(v->updates == 1 && v->clips[0].y0 == 0 && v->clips[0].y1 == 192);

    /* ── Save state: the frame count goes with the device; the timers continue ── */
    uint8_t *st;
    size_t n;
    CHECK(gemu_machine_save_state(m, &st, &n));
    GemuTime t1 = gemu_machine_time(m);
    uint64_t f1 = gemu_screen_frame_number(s);
    unsigned r1 = pin->rises;
    gemu_machine_run_until(m, t1 + GEMU_SEC(1));
    uint64_t f2 = gemu_screen_frame_number(s);
    unsigned r2 = pin->rises;
    CHECK(f2 > f1 + 50);
    CHECK(gemu_machine_load_state(m, st, n));
    CHECK(gemu_screen_frame_number(s) == f1 && pin->rises == r1);
    gemu_machine_run_until(m, t1 + GEMU_SEC(1));
    CHECK(gemu_screen_frame_number(s) == f2 && pin->rises == r2);
    free(st);

    /* ── Device-timed: the device says when a frame begins ── */
    GemuScreen *mono = gemu_screen_new(pd, "mono");       /* a second one, elsewhere */
    CHECK(mono && gemu_screen_next(s) == mono);
    CHECK(gemu_screen_set_raw(mono, 16257000, 882, 0, 720, 370, 0, 350, false));
    CHECK(gemu_screen_set_size(mono, 720, 350));
    GemuTime t0 = gemu_machine_time(m);
    gemu_screen_frame_begin(mono);
    CHECK(gemu_screen_frame_number(mono) == 1 && gemu_screen_frame_start(mono) == t0);
    probe_screen = mono;
    gemu_timer_start(pt, gemu_clocks(16257000, 882u * 10 + 3), 0, 0);
    gemu_machine_run_until(m, t0 + GEMU_MSEC(1));
    CHECK(probe_v == 10 && (probe_h == 3 || probe_h == 2));   /* whole pixels, rounded down */
    gemu_screen_set_vblank(mono, true);
    CHECK(gemu_line_get(gemu_screen_vblank(mono)));
    /* no update callback: rendering gives the (black) bitmap */
    b = gemu_screen_render(mono);
    CHECK(b->width == 720 && b->pix[0] == 0xFF000000);
    /* without raw timing the beam is at 0,0 */
    GemuScreen *plain = gemu_screen_new(pd, "plain");
    CHECK(gemu_screen_vpos(plain) == 0 && gemu_screen_hpos(plain) == 0);

    /* ── Destroying the device takes its screens ── */
    gemu_device_destroy(vd);
    CHECK(gemu_screen_first(m) == mono && gemu_screen_next(mono) == plain && !gemu_screen_next(plain));

    gemu_machine_free(m);
    printf("screen: %u checks passed\n", checks);
    return 0;
}
