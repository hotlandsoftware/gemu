#include "chip8.h"
#include "gemu/gemu.h"
#include "gemu/memory.h"
#include "gemu/gtk_menu.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * CHIP-8 hex keypad → GDK keysym mapping (same physical layout as SDL2 backend):
 *
 *   Keypad    Keyboard
 *   1 2 3 C   1 2 3 4
 *   4 5 6 D   Q W E R
 *   7 8 9 E   A S D F
 *   A 0 B F   Z X C V
 */
static const guint key_map[CHIP8_NUM_KEYS] = {
    GDK_KEY_x,  /* 0 */
    GDK_KEY_1,  /* 1 */
    GDK_KEY_2,  /* 2 */
    GDK_KEY_3,  /* 3 */
    GDK_KEY_q,  /* 4 */
    GDK_KEY_w,  /* 5 */
    GDK_KEY_e,  /* 6 */
    GDK_KEY_a,  /* 7 */
    GDK_KEY_s,  /* 8 */
    GDK_KEY_d,  /* 9 */
    GDK_KEY_z,  /* A */
    GDK_KEY_c,  /* B */
    GDK_KEY_4,  /* C */
    GDK_KEY_r,  /* D */
    GDK_KEY_f,  /* E */
    GDK_KEY_v,  /* F */
};

typedef struct {
    GtkWidget          *window;
    GtkWidget          *drawing_area;
    cairo_surface_t    *chip_surf;   /* 64×32 RGB24 surface updated each frame */
    int                 scale;
    uint8_t             keys[CHIP8_NUM_KEYS];
    uint8_t             keys_prev[CHIP8_NUM_KEYS];
    Chip8State         *s;
    const Chip8Config  *cfg;
    bool                display_active;
} GtkCtx;

/* ── Cairo surface update ─────────────────────────────────────────────────── */

static void update_surface(GtkCtx *c, const uint8_t *vram) {
    uint32_t *px     = (uint32_t *)cairo_image_surface_get_data(c->chip_surf);
    int       stride = cairo_image_surface_get_stride(c->chip_surf) / 4;
    for (int y = 0; y < CHIP8_DISPLAY_H; y++)
        for (int x = 0; x < CHIP8_DISPLAY_W; x++)
            px[y * stride + x] = vram[y * CHIP8_DISPLAY_W + x] ? 0xFFFFFFFF : 0xFF000000;
    cairo_surface_mark_dirty(c->chip_surf);
    c->display_active = true;
}

/* ── GTK signal callbacks ─────────────────────────────────────────────────── */

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    (void)w;
    GtkCtx *c = data;

    if (!c->display_active) {
        int pw = CHIP8_DISPLAY_W * c->scale;
        int ph = CHIP8_DISPLAY_H * c->scale;
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        const char *msg = "Guest has not initialized display (yet!)";
        cairo_select_font_face(cr, "monospace",
                               CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, (pw >= 512) ? 12.0 : 7.0);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, msg, &ext);
        cairo_set_source_rgb(cr, 0.67, 0.67, 0.67);
        cairo_move_to(cr,
                      (pw - ext.width)  / 2.0 - ext.x_bearing,
                      (ph - ext.height) / 2.0 - ext.y_bearing);
        cairo_show_text(cr, msg);
        return FALSE;
    }

    cairo_save(cr);
    cairo_scale(cr, c->scale, c->scale);
    cairo_set_source_surface(cr, c->chip_surf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w;
    GtkCtx *c   = data;
    guint   sym = gdk_keyval_to_lower(ev->keyval);
    uint8_t val = (ev->type == GDK_KEY_PRESS) ? 1 : 0;

    if (sym == GDK_KEY_Escape && val) {
        gtk_main_quit();
        return TRUE;
    }
    for (int k = 0; k < CHIP8_NUM_KEYS; k++) {
        if (sym == key_map[k]) { c->keys[k] = val; return TRUE; }
    }
    return FALSE;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer data) {
    (void)w; (void)ev; (void)data;
    gtk_main_quit();
    return TRUE;
}

/* ── 60 Hz frame tick ─────────────────────────────────────────────────────── */

static gboolean frame_tick(gpointer data) {
    GtkCtx     *c = data;
    Chip8State *s = c->s;

    /* Monitor commands */
    GemuMonCmd cmd;
    while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
        if (cmd == GEMU_MON_QUIT) {
            gtk_main_quit();
            return G_SOURCE_REMOVE;
        } else if (cmd == GEMU_MON_RESET) {
            chip8_machine_reset(s, c->cfg);
        } else if (cmd == GEMU_MON_STEP) {
            uint32_t n = gemu_monitor_step_count(s->monitor);
            for (uint32_t i = 0; i < n; i++) chip8_exec_single(s);
        }
        /* STOP/CONT: paused flag updated by gemu_monitor_poll */
    }

    /* Copy key state (for edge detection in wait_key) */
    memcpy(s->keys_prev, s->keys, CHIP8_NUM_KEYS);
    memcpy(s->keys,      c->keys, CHIP8_NUM_KEYS);

    /* Paused by monitor */
    if (gemu_monitor_is_paused(s->monitor))
        return G_SOURCE_CONTINUE;

    /* Handle LD Vx, K stall */
    if (s->wait_key) {
        for (int k = 0; k < CHIP8_NUM_KEYS; k++) {
            if (s->keys[k] && !s->keys_prev[k]) {
                s->V[s->wait_reg] = (uint8_t)k;
                s->wait_key = false;
                s->PC += 2;
                break;
            }
        }
        return G_SOURCE_CONTINUE; /* still stalled or just resumed — skip execution */
    }

    /* Execute instructions for this frame */
    int budget = c->cfg->cpu_hz / CHIP8_TIMER_HZ;
    while (budget > 0 && !s->wait_key) {
        GemuTb *tb = gemu_tb_lookup(&s->tb_cache, s->PC);
        if (!tb) tb = chip8_translate_block(s, s->PC);
        if (!tb) break;
        chip8_execute_tb(s, tb);
        budget -= (int)tb->n_insns;
    }

    /* 60 Hz timers */
    if (s->delay > 0) s->delay--;
    if (s->sound > 0) s->sound--;

    /* Render if display changed */
    if (s->draw_flag) {
        update_surface(c, s->vram);
        gtk_widget_queue_draw(c->drawing_area);
        gemu_vnc_update(s->vnc, s->vram, CHIP8_DISPLAY_W, CHIP8_DISPLAY_H);
        s->draw_flag = false;
    }

    return G_SOURCE_CONTINUE;
}

/* ── GTK run loop (owns main loop, called from machine_run) ───────────────── */

static void gtk_run(Chip8Display *d, Chip8State *s, const Chip8Config *cfg) {
    GtkCtx *c = d->ctx;
    c->s   = s;
    c->cfg = cfg;

    g_timeout_add(1000 / CHIP8_TIMER_HZ, frame_tick, c);
    gtk_main();

    printf("gemu-chip8: %llu instructions executed, %llu TB hits, %llu misses\n",
           (unsigned long long)s->insn_count,
           (unsigned long long)s->tb_hits,
           (unsigned long long)s->tb_misses);
}

static void gtk_render(void *ctx, const uint8_t *vram) {
    /* frame_tick handles rendering; this is a no-op in GTK mode */
    (void)ctx; (void)vram;
}

static void gtk_destroy(void *ctx) {
    GtkCtx *c = ctx;
    cairo_surface_destroy(c->chip_surf);
    free(c);
}

/* ── Display creation ─────────────────────────────────────────────────────── */

Chip8Display *chip8_display_gtk_create(int scale, GemuMonitor *mon) {
    gtk_init(NULL, NULL);

    GtkCtx *c = calloc(1, sizeof(*c));
    c->scale  = scale;

    /* 64×32 Cairo surface for CHIP-8 pixels */
    c->chip_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                              CHIP8_DISPLAY_W, CHIP8_DISPLAY_H);

    /* Top-level window */
    c->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(c->window), "GEMU");
    gtk_window_set_resizable(GTK_WINDOW(c->window), FALSE);

    /* Root vertical box */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(c->window), vbox);

    gemu_gtk_add_action_menu(vbox, mon, NULL, NULL);

    /* ── CHIP-8 drawing area ── */
    c->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(c->drawing_area,
                                CHIP8_DISPLAY_W * scale,
                                CHIP8_DISPLAY_H * scale);
    gtk_box_pack_start(GTK_BOX(vbox), c->drawing_area, TRUE, TRUE, 0);

    /* ── Signals ── */
    gtk_widget_add_events(c->window, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    g_signal_connect(c->window,       "delete-event",      G_CALLBACK(on_delete), NULL);
    g_signal_connect(c->window,       "key-press-event",   G_CALLBACK(on_key),    c);
    g_signal_connect(c->window,       "key-release-event", G_CALLBACK(on_key),    c);
    g_signal_connect(c->drawing_area, "draw",              G_CALLBACK(on_draw),   c);

    gtk_widget_show_all(c->window);

    Chip8Display *d = calloc(1, sizeof(*d));
    d->render  = gtk_render;
    d->destroy = gtk_destroy;
    d->run     = gtk_run;
    d->ctx     = c;

    return d;
}


