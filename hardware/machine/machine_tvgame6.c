#ifndef _WIN32
#  define _POSIX_C_SOURCE 199309L
#endif

#include "tvgame6.h"
#include "gemu/gemu_display.h"
#include "gemu/monitor.h"
#include "gemu/screendump.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static void tvgame6_sleep_frame(void) { Sleep(1000 / 60); }
#else
#  include <time.h>
static void tvgame6_sleep_frame(void) {
    struct timespec ts = {0, 1000000000L / 60};
    nanosleep(&ts, NULL);
}
#endif

/*
 * No M58815P datasheet or die-shot is publicly known to exist, so this is
 * not a gate-level reconstruction of that specific chip. Instead it follows
 * the documented architecture of the wider "Pong-on-a-chip" family it
 * belongs to (e.g. General Instrument AY-3-8500, reverse-engineered from
 * die shots - see nerdstuffbycole.blogspot.com/2018/01/reverse-engineering-ay-3-8500-part-1.html):
 * ball/paddle position held in small binary counters, ball motion driven by
 * a fixed per-frame horizontal step plus a *clock-divided* vertical step
 * (the divider/step/direction chosen from a handful of paddle-zone table
 * entries - not trigonometry; real Pong chips had no multiplier or angle
 * math, only a few discrete bounce rates), and collision sensed via simple
 * rect-overlap comparators. Where the real chip's specific bit widths, zone
 * counts or divider ratios aren't documented, values here are a best effort
 * recreation rather than lifted from a real datasheet - this is a
 * behavioral/structural model, blending known hardware architecture with
 * simulated specifics, not a verified transistor-exact emulation.
 */
#define FB_W 320
#define FB_H 240
#define COURT_X 35
#define COURT_Y 16
#define COURT_W 250
#define COURT_H 203
#define COURT_R (COURT_X + COURT_W)
#define COURT_B (COURT_Y + COURT_H)
#define FIELD_Y 21
#define FIELD_B 214
#define PADDLE_W 8
#define PADDLE_H 33
#define PADDLE_H_SMALL 17 /* resize toggle: ~half height */
#define PADDLE_OFFSET 71  /* doubles: front/back spacing along x */
#define PADDLE_STEP 3     /* paddle position counter step, per frame held */
#define BALL_W 7
#define BALL_H 5
#define MARKER_W 8
#define MARKER_H 6
#define MARKER_STEP 32
#define MARKER_GREEN_X0 149
#define MARKER_GREEN_X1 173
#define MARKER_RED_X0 139
#define MARKER_RED_X1 163
#define MARKER_GREEN_Y0 24
#define MARKER_RED_Y0 40

#define BALL_H_STEP_NORMAL 3 /* horizontal position-counter step per frame */
#define BALL_H_STEP_TURBO  6

/*
 * Tennis mode: decorative center net, no collision. The reference's net
 * draw call draws onto the pre-blit "screen" surface (screen-space x=485),
 * unlike the ball/paddles/volleyball-net which are drawn directly in
 * display-space - so its court-relative position is
 * COURT_X + 485*(COURT_W/1000), landing it at true court-center, not offset
 * by the 140px blit margin.
 */
#define NET_X 156
#define NET_W MARKER_W
#define NET_Y0 25
#define NET_STEP 33
#define NET_H 16

/* hockey mode: solid side walls with a center goal gap */
#define HOCKEY_WALL_L_X 42
#define HOCKEY_WALL_R_X 277
#define HOCKEY_GOAL_Y0 81
#define HOCKEY_GOAL_Y1 149

enum {
    ACT_L_UP = GEMU_ACTION(0),
    ACT_L_DOWN = GEMU_ACTION(1),
    ACT_R_UP = GEMU_ACTION(2),
    ACT_R_DOWN = GEMU_ACTION(3),
    ACT_TENNIS = GEMU_ACTION(4),
    ACT_VOLLEY = GEMU_ACTION(5),
    ACT_HOCKEY = GEMU_ACTION(6),
    ACT_TOGGLE_PADDLES = GEMU_ACTION(7),
    ACT_SERVE = GEMU_ACTION(8),
    ACT_SPEED_TOGGLE = GEMU_ACTION(9),
    ACT_RESIZE_L = GEMU_ACTION(10),
    ACT_RESIZE_R = GEMU_ACTION(11),
};

typedef enum {
    TVG_TENNIS,
    TVG_VOLLEY,
    TVG_HOCKEY,
} TvGame6Mode;

/*
 * The reference's color_list[game-1]: each mode has its own
 * court/background/net (& ball)/paddle/goal/wall palette, not a single
 * shared color scheme. Index by TvGame6Mode - note GEMU's mode *names*
 * don't line up with the reference's game numbers (GEMU "tennis" plays like
 * its game=2, "volley" like game=1, "hockey" like game=3 - see the
 * mode-behavior comment above update_game()), so the palette below follows
 * that same by-behavior mapping rather than the raw game index.
 */
typedef struct {
    uint32_t court, bg, net_ball, paddle, goal_l, goal_r, wall_base, score;
} ModePalette;

static const ModePalette mode_palette[3] = {
    [TVG_TENNIS] = { 0xffc07645, 0xff0e7aab, 0xfffae7b4, 0xffd0ecb2,
                      0xff4cbd7c, 0xffa36878, 0xffdad2b0, 0xffcdf0ff },
    [TVG_VOLLEY] = { 0xff6d70f9, 0xff19b537, 0xffc7dbfd, 0xffa6e6ff,
                      0xff84686a, 0xff408caf, 0xffcddcfb, 0xffedfd84 },
    [TVG_HOCKEY] = { 0xff2cc94c, 0xff505068, 0xff98ff8d, 0xffff9fff,
                      0xff3a687f, 0xff51bc29, 0xff98ff8d, 0xffaefbff },
};

struct MitsuTvGame6State {
    GemuDisplay *display;
    GemuMonitor *monitor;
    uint32_t fb[FB_W * FB_H];
    TvGame6Mode mode;
    bool double_paddles;
    bool waiting_serve;
    bool turbo;

    int left_y, right_y;   /* paddle center-y position counters */
    int left_h, right_h;   /* paddle height (resize toggle) */

    int ball_x, ball_y;    /* ball position counters */
    int h_dir;             /* +1/-1: horizontal direction latch */
    int h_step;            /* horizontal position-counter step/frame */
    int v_dir;             /* -1/0/+1: vertical direction latch */
    int v_div;             /* vertical step fires every v_div frames (0 = never) */
    int v_step;            /* vertical position-counter step, when it fires */
    int v_acc;             /* frames accumulated toward the next vertical step */

    int marker_cooldown;
    int bounce_count;      /* consecutive net-tile bounces (volleyball anti-stall) */
    int left_score;
    int right_score;
    uint32_t prev_actions;
};

static const char *mode_name(TvGame6Mode mode) {
    switch (mode) {
    case TVG_TENNIS: return "tennis";
    case TVG_VOLLEY: return "volley";
    case TVG_HOCKEY: return "hockey";
    }
    return "unknown";
}

static const GemuActionDef tvgame6_actions[] = {
    { "left_up",        ACT_L_UP,          "w" },
    { "left_down",      ACT_L_DOWN,        "s" },
    { "right_up",       ACT_R_UP,          "Up" },
    { "right_down",     ACT_R_DOWN,        "Down" },
    { "tennis",         ACT_TENNIS,        "1" },
    { "volley",         ACT_VOLLEY,        "2" },
    { "hockey",         ACT_HOCKEY,        "3" },
    { "toggle_paddles", ACT_TOGGLE_PADDLES,"4" },
    { "serve",          ACT_SERVE,         "Space" },
    { "toggle_speed",   ACT_SPEED_TOGGLE,  "5" },
    { "resize_left",    ACT_RESIZE_L,      "6" },
    { "resize_right",   ACT_RESIZE_R,      "7" },
};

static void tvgame6_reset_ball(MitsuTvGame6State *s, int dir) {
    s->ball_x = COURT_X + COURT_W / 2;
    s->ball_y = COURT_Y + COURT_H / 2;
    s->h_dir = dir >= 0 ? 1 : -1;
    s->h_step = s->turbo ? BALL_H_STEP_TURBO : BALL_H_STEP_NORMAL;
    s->v_dir = 0;
    s->v_div = 0;
    s->v_step = 0;
    s->v_acc = 0;
    s->marker_cooldown = 0;
    s->bounce_count = 0;
    s->waiting_serve = true;
}

static void tvgame6_reset(MitsuTvGame6State *s) {
    s->mode = TVG_TENNIS;
    s->double_paddles = false;
    s->waiting_serve = true;
    s->turbo = false;
    s->left_h = PADDLE_H;
    s->right_h = PADDLE_H;
    s->left_y = COURT_Y + COURT_H / 2;
    s->right_y = COURT_Y + COURT_H / 2;
    s->left_score = 0;
    s->right_score = 0;
    s->prev_actions = 0;
    tvgame6_reset_ball(s, 1);
}

static void tvgame6_cpu_state(void *ud, char *buf, size_t buf_len) {
    MitsuTvGame6State *s = ud;
    snprintf(buf, buf_len,
             "Nintendo Color TV-Game 6 (behavioral M58815P model)\n"
             "  mode=%s paddles=%s speed=%s score=%d-%d serve=%s\n"
             "  left_y=%d right_y=%d ball=(%d,%d)\n"
             "  h_dir=%+d h_step=%d v_dir=%+d v_div=%d v_step=%d v_acc=%d bounce_count=%d\n",
             mode_name(s->mode), s->double_paddles ? "double" : "single",
             s->turbo ? "turbo" : "normal",
             s->left_score, s->right_score, s->waiting_serve ? "yes" : "no",
             s->left_y, s->right_y, s->ball_x, s->ball_y,
             s->h_dir, s->h_step, s->v_dir, s->v_div, s->v_step, s->v_acc, s->bounce_count);
}

static bool tvgame6_screendump(void *ud, const char *path) {
    MitsuTvGame6State *s = ud;
    return gemu_screendump_argb(path, s->fb, FB_W, FB_H);
}

MitsuTvGame6State *mitsu_tvgame6_create(const MitsuConfig *cfg) {
    MitsuTvGame6State *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    srand((unsigned)time(NULL));
    tvgame6_reset(s);

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_cpu_state_cb(s->monitor, tvgame6_cpu_state, s);
    gemu_monitor_set_screendump_cb(s->monitor, tvgame6_screendump, s);

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        GemuDisplayConfig dc = {
            .title = "GEMU",
            .fb_width = FB_W,
            .fb_height = FB_H,
            .scale = cfg->display_scale,
            .actions = tvgame6_actions,
            .n_actions = (int)(sizeof tvgame6_actions / sizeof *tvgame6_actions),
            .ini_section = "tvgame6",
        };
        s->display = gemu_display_create(cfg->display_type, &dc);
        if (!s->display) {
            gemu_monitor_destroy(s->monitor);
            free(s);
            return NULL;
        }
    }
    return s;
}

void mitsu_tvgame6_destroy(MitsuTvGame6State *s) {
    if (!s)
        return;
    gemu_display_destroy(s->display);
    gemu_monitor_destroy(s->monitor);
    free(s);
}

static void put_px(MitsuTvGame6State *s, int x, int y, uint32_t c) {
    if ((unsigned)x < FB_W && (unsigned)y < FB_H)
        s->fb[y * FB_W + x] = c;
}

static void fill_rect(MitsuTvGame6State *s, int x, int y, int w, int h, uint32_t c) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            put_px(s, xx, yy, c);
}

static void draw_paddle(MitsuTvGame6State *s, int x, int cy, int h, uint32_t c) {
    fill_rect(s, x, cy - h / 2, PADDLE_W, h, c);
}

static void draw_side_paddles(MitsuTvGame6State *s, bool is_left, int x, int cy, int h, uint32_t c) {
    draw_paddle(s, x, cy, h, c);
    if (!s->double_paddles)
        return;
    int x2 = x + (is_left ? PADDLE_OFFSET : -PADDLE_OFFSET);
    draw_paddle(s, x2, cy, h, c);
}

/* Net tile colors are hardcoded in the reference's net-drawing routine
 * rather than taken from color_list - they're the same red/green in every
 * mode. */
static void draw_center_markers(MitsuTvGame6State *s) {
    for (int row = 0; row < 6; row++) {
        int gy = MARKER_GREEN_Y0 + row * MARKER_STEP;
        int ry = MARKER_RED_Y0 + row * MARKER_STEP;
        fill_rect(s, MARKER_GREEN_X0, gy, MARKER_W, MARKER_H, 0xff30ae18);
        fill_rect(s, MARKER_GREEN_X1, gy, MARKER_W, MARKER_H, 0xff30ae18);
        fill_rect(s, MARKER_RED_X0, ry, MARKER_W, MARKER_H, 0xffcc4b49);
        fill_rect(s, MARKER_RED_X1, ry, MARKER_W, MARKER_H, 0xffcc4b49);
    }
}

static void draw_tennis_net(MitsuTvGame6State *s, uint32_t c) {
    for (int i = 0; i < 6; i++)
        fill_rect(s, NET_X, NET_Y0 + i * NET_STEP, NET_W, NET_H, c);
}

/* The reference never renders a visible line for the hockey side walls
 * (only a collision boundary) - we draw one for player legibility, in the
 * mode's net/ball accent color rather than an arbitrary stark white. */
static void draw_hockey_walls(MitsuTvGame6State *s, uint32_t c) {
    fill_rect(s, HOCKEY_WALL_L_X, COURT_Y + 2, 2, HOCKEY_GOAL_Y0 - (COURT_Y + 2), c);
    fill_rect(s, HOCKEY_WALL_L_X, HOCKEY_GOAL_Y1, 2, (COURT_B - 2) - HOCKEY_GOAL_Y1, c);
    fill_rect(s, HOCKEY_WALL_R_X, COURT_Y + 2, 2, HOCKEY_GOAL_Y0 - (COURT_Y + 2), c);
    fill_rect(s, HOCKEY_WALL_R_X, HOCKEY_GOAL_Y1, 2, (COURT_B - 2) - HOCKEY_GOAL_Y1, c);
}

/* Goal strips at the very edge of the court - one color per side, spanning
 * the same y-range as the top/bottom wall bounce boundary
 * (FIELD_Y..FIELD_B). Missing entirely from earlier revisions. */
static void draw_goals(MitsuTvGame6State *s, uint32_t left_c, uint32_t right_c) {
    fill_rect(s, COURT_X, FIELD_Y, 2, FIELD_B - FIELD_Y, left_c);
    fill_rect(s, COURT_R - 2, FIELD_Y, 2, FIELD_B - FIELD_Y, right_c);
}

/*
 * The reference's wall-coloring routine: the top/bottom wall band ramps
 * r/g/b across only its first ~10% of width (the rest stays flat at
 * wherever the ramp ended), with per-mode increment rules. GRAD_W is
 * COURT_W's ~10% (matches the original's 100-of-1000-pixel ramp).
 */
#define GRAD_W 25

static void wall_gradient(TvGame6Mode mode, uint32_t base, uint32_t out[GRAD_W]) {
    int r = (int)((base >> 16) & 0xff);
    int g = (int)((base >> 8) & 0xff);
    int b = (int)(base & 0xff);
    for (int i = 0; i < GRAD_W; i++) {
        switch (mode) {
        case TVG_VOLLEY: /* reference game=1 */
            if (i % 4 == 0 && r < 255) r++;
            if (g % 3 == 0 && g < 255) g++;
            break;
        case TVG_TENNIS: /* reference game=2 */
            if (i % 3 == 0 && g < 255) g++;
            if (i % 5 == 0 && b < 255) b++;
            break;
        case TVG_HOCKEY: /* reference game=3 */
            if (i % 4 == 0 && r < 255) r++;
            if (i % 2 == 0 && b < 255) b++;
            break;
        }
        out[i] = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

static void draw_wall_band(MitsuTvGame6State *s, int y, int h, const uint32_t grad[GRAD_W]) {
    for (int i = 0; i < COURT_W; i++)
        fill_rect(s, COURT_X + i, y, 1, h, grad[i < GRAD_W ? i : GRAD_W - 1]);
}

/*
 * Score digits - the reference renders these with a custom TTF
 * (nintendo.ttf) that isn't a runtime asset here. Its glyphs turned out to
 * be a plain rectilinear 7-segment design (confirmed by dumping the outline
 * contours: every digit is built from axis-aligned rectangles at the same
 * handful of x/y boundaries, no curves at all), so this bitmap was
 * generated once by rasterizing that font's actual vector outlines down to
 * a 7x10 grid (nonzero-winding fill, solid contours minus hole contours)
 * and hand-copied in - a pixel-accurate reproduction of the real glyphs,
 * not a from-scratch approximation, without needing the .ttf file at
 * runtime.
 */
#define DIGIT_SCALE 2
#define DIGIT_COLS 7
#define DIGIT_ROWS 10
#define DIGIT_W (DIGIT_COLS * DIGIT_SCALE)
#define DIGIT_H (DIGIT_ROWS * DIGIT_SCALE)

static const uint8_t digit_font[10][DIGIT_ROWS] = {
    { 0x7F, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x7F }, /* 0 */
    { 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03 }, /* 1 */
    { 0x7F, 0x7F, 0x03, 0x03, 0x7F, 0x60, 0x60, 0x60, 0x7F, 0x7F }, /* 2 */
    { 0x7F, 0x7F, 0x03, 0x03, 0x7F, 0x03, 0x03, 0x03, 0x7F, 0x7F }, /* 3 */
    { 0x63, 0x63, 0x63, 0x63, 0x7F, 0x03, 0x03, 0x03, 0x03, 0x03 }, /* 4 */
    { 0x7F, 0x7F, 0x60, 0x60, 0x7F, 0x03, 0x03, 0x03, 0x7F, 0x7F }, /* 5 */
    { 0x7F, 0x7F, 0x60, 0x60, 0x7F, 0x63, 0x63, 0x63, 0x63, 0x7F }, /* 6 */
    { 0x7F, 0x7F, 0x63, 0x63, 0x63, 0x03, 0x03, 0x03, 0x03, 0x03 }, /* 7 */
    { 0x7F, 0x63, 0x63, 0x63, 0x7F, 0x63, 0x63, 0x63, 0x63, 0x7F }, /* 8 */
    { 0x7F, 0x63, 0x63, 0x63, 0x7F, 0x03, 0x03, 0x03, 0x7F, 0x7F }, /* 9 */
};

static void draw_digit(MitsuTvGame6State *s, int x, int y, int digit, uint32_t c) {
    if (digit < 0 || digit > 9)
        return;
    for (int row = 0; row < DIGIT_ROWS; row++)
        for (int col = 0; col < DIGIT_COLS; col++)
            if (digit_font[digit][row] & (1 << (DIGIT_COLS - 1 - col)))
                fill_rect(s, x + col * DIGIT_SCALE, y + row * DIGIT_SCALE,
                          DIGIT_SCALE, DIGIT_SCALE, c);
}

/* ── Comparators (collision/position sensing) ───────────────────────────── */

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static bool ball_in_rect(const MitsuTvGame6State *s, int rx, int ry, int rw, int rh) {
    return in_rect(s->ball_x, s->ball_y,
                   rx - BALL_W / 2, ry - BALL_H / 2,
                   rw + BALL_W, rh + BALL_H);
}

static bool hit_side_paddles(const MitsuTvGame6State *s, bool left) {
    int x = left ? 48 : 265;
    int cy = left ? s->left_y : s->right_y;
    int h = left ? s->left_h : s->right_h;
    if (in_rect(s->ball_x, s->ball_y, x - 2, cy - h / 2 - 2, PADDLE_W + 4, h + 4))
        return true;
    if (!s->double_paddles)
        return false;
    int x2 = x + (left ? PADDLE_OFFSET : -PADDLE_OFFSET);
    return in_rect(s->ball_x, s->ball_y, x2 - 2, cy - h / 2 - 2, PADDLE_W + 4, h + 4);
}

static bool hit_marker_rect(const MitsuTvGame6State *s, int x, int y) {
    return ball_in_rect(s, x, y, MARKER_W, MARKER_H);
}

static bool hit_barrier(const MitsuTvGame6State *s, int *color, int *zone) {
    for (int row = 0; row < 6; row++) {
        int gy = MARKER_GREEN_Y0 + row * MARKER_STEP;
        int ry = MARKER_RED_Y0 + row * MARKER_STEP;
        int ty = 0, c = -1;
        if (hit_marker_rect(s, MARKER_RED_X0, ry))        { ty = ry; c = 0; }
        else if (hit_marker_rect(s, MARKER_RED_X1, ry))   { ty = ry; c = 0; }
        else if (hit_marker_rect(s, MARKER_GREEN_X0, gy)) { ty = gy; c = 1; }
        else if (hit_marker_rect(s, MARKER_GREEN_X1, gy)) { ty = gy; c = 1; }
        if (c < 0)
            continue;
        int off = s->ball_y - ty;
        if (off < 0) off = 0;
        if (off >= MARKER_H) off = MARKER_H - 1;
        *color = c;
        *zone = off * 3 / MARKER_H;
        return true;
    }
    return false;
}

/* ── Bounce-rate tables ──────────────────────────────────────────────────
 *
 * Real Pong-on-a-chip hardware had no multiplier or angle math: the paddle
 * (or net tile) is divided into a handful of zones, and each zone just picks
 * a vertical clock-divider/step/direction for the ball's position counter -
 * a coarse lookup, not a computed angle. 5 zones for the (taller) paddles,
 * 3 for the (much shorter) net tiles.
 */
static const int paddle_zone_div[5]  = { 1, 2, 0, 2, 1 };
static const int paddle_zone_step[5] = { 2, 1, 0, 1, 2 };
static const int paddle_zone_dir[5]  = { -1, -1, 0, 1, 1 };

static const int net_zone_div[3]  = { 1, 0, 1 };
static const int net_zone_step[3] = { 2, 0, 2 };
static const int net_zone_dir[3]  = { -1, 0, 1 };

static int paddle_zone_of(int ball_y, int paddle_cy, int paddle_h) {
    int off = ball_y - (paddle_cy - paddle_h / 2);
    if (off < 0) off = 0;
    if (off >= paddle_h) off = paddle_h - 1;
    int zone = off * 5 / paddle_h;
    return zone > 4 ? 4 : zone;
}

static void apply_bounce(MitsuTvGame6State *s, int div, int step, int dir) {
    s->h_dir = -s->h_dir;
    s->v_div = div;
    s->v_step = step;
    s->v_dir = dir;
    s->v_acc = 0;
}

static void set_mode(MitsuTvGame6State *s, TvGame6Mode mode) {
    if (s->mode == mode)
        return;
    s->mode = mode;
    s->left_score = 0;
    s->right_score = 0;
    tvgame6_reset_ball(s, 1);
}

static void step_ball(MitsuTvGame6State *s) {
    s->ball_x += s->h_dir * s->h_step;
    if (s->v_div > 0) {
        s->v_acc++;
        if (s->v_acc >= s->v_div) {
            s->v_acc = 0;
            s->ball_y += s->v_dir * s->v_step;
        }
    }
}

static void update_game(MitsuTvGame6State *s, uint32_t actions) {
    uint32_t pressed = actions & ~s->prev_actions;
    if (pressed & ACT_TENNIS) set_mode(s, TVG_TENNIS);
    if (pressed & ACT_VOLLEY) set_mode(s, TVG_VOLLEY);
    if (pressed & ACT_HOCKEY) set_mode(s, TVG_HOCKEY);
    if (pressed & ACT_TOGGLE_PADDLES) s->double_paddles = !s->double_paddles;
    if (pressed & ACT_SERVE) s->waiting_serve = false;
    if (pressed & ACT_SPEED_TOGGLE) {
        s->turbo = !s->turbo;
        s->h_step = s->turbo ? BALL_H_STEP_TURBO : BALL_H_STEP_NORMAL;
    }
    if (pressed & ACT_RESIZE_L)
        s->left_h = (s->left_h > (PADDLE_H + PADDLE_H_SMALL) / 2) ? PADDLE_H_SMALL : PADDLE_H;
    if (pressed & ACT_RESIZE_R)
        s->right_h = (s->right_h > (PADDLE_H + PADDLE_H_SMALL) / 2) ? PADDLE_H_SMALL : PADDLE_H;

    if (actions & ACT_L_UP) s->left_y -= PADDLE_STEP;
    if (actions & ACT_L_DOWN) s->left_y += PADDLE_STEP;
    if (actions & ACT_R_UP) s->right_y -= PADDLE_STEP;
    if (actions & ACT_R_DOWN) s->right_y += PADDLE_STEP;
    /* draw_paddle()'s rect is [cy - h/2, cy - h/2 + h), so for odd heights
     * the bottom edge sits h/2 (rounded up) below center, one pixel more
     * than the top's h/2 (rounded down) above it - clamping both edges by
     * the same h/2 let the paddle clip a pixel through the bottom wall. */
    int lh_top = s->left_h / 2, lh_bot = s->left_h - lh_top;
    int rh_top = s->right_h / 2, rh_bot = s->right_h - rh_top;
    /* The reference lets the paddle travel almost the whole window - well
     * past the wall bands and into the background margin (paddle.top can
     * reach -65, paddle.bottom 1005, against a 960-tall window) - not just
     * to the ball's wall-bounce line. Clamp to the framebuffer edges instead
     * of FIELD_Y/FIELD_B so it isn't stopped early. */
    if (s->left_y - lh_top < 0) s->left_y = lh_top;
    if (s->left_y + lh_bot > FB_H) s->left_y = FB_H - lh_bot;
    if (s->right_y - rh_top < 0) s->right_y = rh_top;
    if (s->right_y + rh_bot > FB_H) s->right_y = FB_H - rh_bot;

    if (!s->waiting_serve) {
        step_ball(s);
        if (s->marker_cooldown > 0)
            s->marker_cooldown--;

        /* Clamp back in bounds and only flip if still heading further out -
         * previously this just flipped v_dir unconditionally every frame the
         * ball sat at/past the line, which (since vertical motion only steps
         * once every v_div frames) could flip it back and forth on
         * successive frames while the ball stayed parked outside the court. */
        if (s->ball_y <= FIELD_Y) {
            s->ball_y = FIELD_Y;
            if (s->v_dir < 0) s->v_dir = -s->v_dir;
        } else if (s->ball_y >= FIELD_B) {
            s->ball_y = FIELD_B;
            if (s->v_dir > 0) s->v_dir = -s->v_dir;
        }

        if ((s->h_dir < 0 && hit_side_paddles(s, true)) ||
            (s->h_dir > 0 && hit_side_paddles(s, false))) {
            bool left_style = s->h_dir < 0;
            int cy = left_style ? s->left_y : s->right_y;
            int h = left_style ? s->left_h : s->right_h;
            int zone = paddle_zone_of(s->ball_y, cy, h);
            apply_bounce(s, paddle_zone_div[zone], paddle_zone_step[zone], paddle_zone_dir[zone]);
            s->bounce_count = 0;
        }

        if (s->mode == TVG_VOLLEY) {
            int color = 0, zone = 0;
            if (s->marker_cooldown == 0 && hit_barrier(s, &color, &zone)) {
                bool left_style = (color == 0);
                bool correct_dir = left_style ? (s->h_dir < 0) : (s->h_dir > 0);
                if (!(correct_dir && s->bounce_count < 10)) {
                    /* Anti-stall: after too many consecutive net bounces (or a
                     * bounce from the "wrong" side), pick a random zone
                     * instead of the one the ball geometrically hit - the
                     * flat (zone 1) entry is what actually lets the ball
                     * slip through the gap between tile rows next frame. */
                    zone = rand() % 3;
                }
                apply_bounce(s, net_zone_div[zone], net_zone_step[zone], net_zone_dir[zone]);
                s->bounce_count++;
                s->marker_cooldown = 8;
            }
        } else if (s->mode == TVG_HOCKEY) {
            bool in_gap = s->ball_y > HOCKEY_GOAL_Y0 && s->ball_y < HOCKEY_GOAL_Y1;
            if (!in_gap) {
                if (s->h_dir < 0 && s->ball_x <= HOCKEY_WALL_L_X + BALL_W / 2) {
                    s->ball_x = HOCKEY_WALL_L_X + BALL_W / 2;
                    s->h_dir = -s->h_dir;
                } else if (s->h_dir > 0 && s->ball_x >= HOCKEY_WALL_R_X - BALL_W / 2) {
                    s->ball_x = HOCKEY_WALL_R_X - BALL_W / 2;
                    s->h_dir = -s->h_dir;
                }
            }
        }

        if (s->ball_x < COURT_X - 8) {
            s->right_score = (s->right_score + 1) % 10;
            tvgame6_reset_ball(s, -1);
        } else if (s->ball_x > COURT_R + 8) {
            s->left_score = (s->left_score + 1) % 10;
            tvgame6_reset_ball(s, 1);
        }
    }
    s->prev_actions = actions;
}

static void render_game(MitsuTvGame6State *s) {
    const ModePalette *pal = &mode_palette[s->mode];

    for (int i = 0; i < FB_W * FB_H; i++)
        s->fb[i] = pal->bg;

    fill_rect(s, COURT_X, COURT_Y, COURT_W, COURT_H, pal->court);
    fill_rect(s, COURT_X, COURT_Y, COURT_W, 1, 0xff000000);
    fill_rect(s, COURT_X, COURT_B - 1, COURT_W, 1, 0xff000000);

    uint32_t grad[GRAD_W];
    wall_gradient(s->mode, pal->wall_base, grad);
    draw_wall_band(s, COURT_Y + 1, 5, grad);
    draw_wall_band(s, COURT_B - 6, 5, grad);

    draw_goals(s, pal->goal_l, pal->goal_r);

    if (s->mode == TVG_VOLLEY)
        draw_center_markers(s);
    else if (s->mode == TVG_TENNIS)
        draw_tennis_net(s, pal->net_ball);
    else if (s->mode == TVG_HOCKEY)
        draw_hockey_walls(s, pal->net_ball);

    /* The reference shows the score continuously while the ball is
     * stationary waiting to be served - not a timed flash, but it reads as
     * "briefly comes up" since play resumes as soon as the point is
     * served. */
    if (s->waiting_serve) {
        draw_digit(s, COURT_X + 30, COURT_Y + 20, s->left_score, pal->score);
        draw_digit(s, COURT_R - 30 - DIGIT_W, COURT_Y + 20, s->right_score, pal->score);
    }

    draw_side_paddles(s, true, 48, s->left_y, s->left_h, pal->paddle);
    draw_side_paddles(s, false, 265, s->right_y, s->right_h, pal->paddle);
    fill_rect(s, s->ball_x - BALL_W / 2, s->ball_y - BALL_H / 2,
              BALL_W, BALL_H, pal->net_ball);
}

void mitsu_tvgame6_run(MitsuTvGame6State *s, const MitsuConfig *cfg) {
    printf("gemu-mitsu: Nintendo Color TV-Game 6 behavioral M58815P model\n");
    render_game(s);
    gemu_monitor_start(s->monitor);

    if (cfg->display_type == GEMU_DISPLAY_NONE) {
        bool running = true;
        while (running) {
            GemuMonCmd cmd;
            while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
                if (cmd == GEMU_MON_RESET) tvgame6_reset(s);
                else if (cmd == GEMU_MON_QUIT) {
                    if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                    else { running = false; break; }
                } else if (cmd == GEMU_MON_STEP) {
                    update_game(s, 0);
                } else if (cmd == GEMU_MON_CUSTOM) {
                    gemu_monitor_unknown_command(s->monitor);
                }
            }
            if (!running)
                break;
            if (!gemu_monitor_is_paused(s->monitor))
                update_game(s, s->waiting_serve ? ACT_SERVE : 0);
            render_game(s);
            tvgame6_sleep_frame();
        }
        gemu_monitor_stop(s->monitor);
        return;
    }

    bool running = true;
    while (running) {
        uint32_t actions = gemu_display_poll(s->display);
        if (gemu_display_should_quit(s->display)) {
            if (cfg->no_shutdown)
                actions = 0;
            else
                running = false;
        }
        if (gemu_display_reset_requested(s->display)) {
            tvgame6_reset(s);
            gemu_display_clear_flags(s->display);
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { running = false; break; }
            } else if (cmd == GEMU_MON_RESET) {
                tvgame6_reset(s);
            } else if (cmd == GEMU_MON_STEP) {
                update_game(s, 0);
            } else if (cmd == GEMU_MON_CUSTOM) {
                gemu_monitor_unknown_command(s->monitor);
            }
        }
        gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));
        if (gemu_monitor_is_paused(s->monitor)) {
            render_game(s);
            gemu_display_render(s->display, s->fb, FB_W, FB_H);
            tvgame6_sleep_frame();
            continue;
        }

        update_game(s, actions);
        render_game(s);
        gemu_display_render(s->display, s->fb, FB_W, FB_H);
        tvgame6_sleep_frame();
    }
    gemu_monitor_stop(s->monitor);
}
