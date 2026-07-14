#ifndef _WIN32
#  define _POSIX_C_SOURCE 199309L
#endif

#include "tvgame6.h"
#include "gemu/gemu_display.h"
#include "gemu/monitor.h"
#include "gemu/screendump.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

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
 * Field geometry below is a direct 0.25x/0.2506x scale-down of the
 * reference implementation (Color TV-Game 6.py, 1280x960 window), so that
 * this behavioral model reproduces the same paddle/ball/net proportions
 * and physics rather than an independently-tuned approximation.
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
#define PADDLE_HALF (PADDLE_H * 0.5f)
#define PADDLE_OFFSET 71.0f /* doubles: front/back spacing along x */
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

#define BALL_SPEED_NORMAL 2.875f
#define BALL_SPEED_TURBO  5.75f
#define PADDLE_SPEED      2.75f

/* tennis mode: decorative center net, no collision */
#define NET_X 121
#define NET_W MARKER_W
#define NET_Y0 8
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

struct MitsuTvGame6State {
    GemuDisplay *display;
    GemuMonitor *monitor;
    uint32_t fb[FB_W * FB_H];
    TvGame6Mode mode;
    bool double_paddles;
    bool waiting_serve;
    bool turbo;
    float left_y, right_y;
    float left_h, right_h;
    float ball_x, ball_y;
    float ball_vx, ball_vy;
    int marker_cooldown;
    int bounce_count;
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
    s->ball_x = COURT_X + COURT_W * 0.5f;
    s->ball_y = COURT_Y + COURT_H * 0.5f;
    float mag = s->turbo ? BALL_SPEED_TURBO : BALL_SPEED_NORMAL;
    s->ball_vx = dir >= 0 ? mag : -mag;
    s->ball_vy = mag * 0.5f;
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
    s->left_y = COURT_Y + COURT_H * 0.5f;
    s->right_y = COURT_Y + COURT_H * 0.5f;
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
             "  left_y=%.1f right_y=%.1f ball=(%.1f,%.1f) vel=(%.2f,%.2f)\n",
             mode_name(s->mode), s->double_paddles ? "double" : "single",
             s->turbo ? "turbo" : "normal",
             s->left_score, s->right_score, s->waiting_serve ? "yes" : "no",
             s->left_y, s->right_y, s->ball_x, s->ball_y, s->ball_vx, s->ball_vy);
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

static void draw_paddle(MitsuTvGame6State *s, int x, float cy, float h, uint32_t c) {
    fill_rect(s, x, (int)(cy - h * 0.5f), PADDLE_W, (int)h, c);
}

/* Doubles: the second paddle sits at the same y, offset in x toward the
 * net (front/back formation), matching Python's left_paddle2/right_paddle2. */
static void draw_side_paddles(MitsuTvGame6State *s, bool is_left, int x, float cy, float h, uint32_t c) {
    draw_paddle(s, x, cy, h, c);
    if (!s->double_paddles)
        return;
    float x2 = x + (is_left ? PADDLE_OFFSET : -PADDLE_OFFSET);
    draw_paddle(s, (int)x2, cy, h, c);
}

static void draw_center_markers(MitsuTvGame6State *s) {
    for (int row = 0; row < 6; row++) {
        int gy = MARKER_GREEN_Y0 + row * MARKER_STEP;
        int ry = MARKER_RED_Y0 + row * MARKER_STEP;
        fill_rect(s, MARKER_GREEN_X0, gy, MARKER_W, MARKER_H, 0xff30ae18);
        fill_rect(s, MARKER_GREEN_X1, gy, MARKER_W, MARKER_H, 0xff30ae18);
        fill_rect(s, MARKER_RED_X0, ry, MARKER_W, MARKER_H, 0xffcc4b51);
        fill_rect(s, MARKER_RED_X1, ry, MARKER_W, MARKER_H, 0xffcc4b51);
    }
}

static void draw_tennis_net(MitsuTvGame6State *s) {
    for (int i = 0; i < 6; i++)
        fill_rect(s, NET_X, NET_Y0 + i * NET_STEP, NET_W, NET_H, 0xffc7dbfd);
}

static void draw_hockey_walls(MitsuTvGame6State *s) {
    fill_rect(s, HOCKEY_WALL_L_X, COURT_Y + 2, 2, HOCKEY_GOAL_Y0 - (COURT_Y + 2), 0xffffffff);
    fill_rect(s, HOCKEY_WALL_L_X, HOCKEY_GOAL_Y1, 2, (COURT_B - 2) - HOCKEY_GOAL_Y1, 0xffffffff);
    fill_rect(s, HOCKEY_WALL_R_X, COURT_Y + 2, 2, HOCKEY_GOAL_Y0 - (COURT_Y + 2), 0xffffffff);
    fill_rect(s, HOCKEY_WALL_R_X, HOCKEY_GOAL_Y1, 2, (COURT_B - 2) - HOCKEY_GOAL_Y1, 0xffffffff);
}

static bool in_rect(float x, float y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static bool ball_in_rect(const MitsuTvGame6State *s, int rx, int ry, int rw, int rh) {
    return in_rect(s->ball_x, s->ball_y,
                   rx - BALL_W / 2, ry - BALL_H / 2,
                   rw + BALL_W, rh + BALL_H);
}

static bool hit_side_paddles(const MitsuTvGame6State *s, bool left) {
    int x = left ? 48 : 265;
    float cy = left ? s->left_y : s->right_y;
    float h = left ? s->left_h : s->right_h;
    if (in_rect(s->ball_x, s->ball_y, x - 2, (int)(cy - h * 0.5f) - 2, PADDLE_W + 4, (int)h + 4))
        return true;
    if (!s->double_paddles)
        return false;
    float x2 = x + (left ? PADDLE_OFFSET : -PADDLE_OFFSET);
    return in_rect(s->ball_x, s->ball_y, (int)x2 - 2, (int)(cy - h * 0.5f) - 2, PADDLE_W + 4, (int)h + 4);
}

static bool hit_marker_rect(const MitsuTvGame6State *s, int x, int y) {
    return ball_in_rect(s, x, y, MARKER_W, MARKER_H);
}

/* Scans both the red (color=0) and green (color=1) net columns - only
 * called in volleyball mode. Matches Python's net[0]/net[1] tile grid. */
static bool hit_barrier(const MitsuTvGame6State *s, int *hit_x, int *hit_y, int *color) {
    for (int row = 0; row < 6; row++) {
        int gy = MARKER_GREEN_Y0 + row * MARKER_STEP;
        int ry = MARKER_RED_Y0 + row * MARKER_STEP;
        if (hit_marker_rect(s, MARKER_RED_X0, ry)) {
            *hit_x = MARKER_RED_X0; *hit_y = ry; *color = 0;
            return true;
        }
        if (hit_marker_rect(s, MARKER_RED_X1, ry)) {
            *hit_x = MARKER_RED_X1; *hit_y = ry; *color = 0;
            return true;
        }
        if (hit_marker_rect(s, MARKER_GREEN_X0, gy)) {
            *hit_x = MARKER_GREEN_X0; *hit_y = gy; *color = 1;
            return true;
        }
        if (hit_marker_rect(s, MARKER_GREEN_X1, gy)) {
            *hit_x = MARKER_GREEN_X1; *hit_y = gy; *color = 1;
            return true;
        }
    }
    return false;
}

/*
 * Reproduces Python's paddle_collision() angle formula exactly: the bounce
 * angle is +/-50 degrees at the paddle tips, computed via
 * tan(direction) regardless of which side is hit, and always normalized
 * against the nominal (non-resized) paddle half-height - Python divides by
 * the global paddle_height even when bouncing off a net tile, so we do too.
 */
static void compute_bounce(float vx_in, bool left_style, float c_point, float *vx_out, float *vy_out) {
    if (c_point < -1.0f) c_point = -1.0f;
    if (c_point > 1.0f) c_point = 1.0f;
    float angle = c_point * 50.0f;
    float direction;
    if (left_style)
        direction = angle < 0.0f ? 360.0f + angle : angle;
    else
        direction = angle == 0.0f ? 180.0f : 180.0f - angle;
    if (direction >= 181.0f && direction <= 185.0f)
        direction = 180.0f;
    else if (direction >= 355.0f && direction <= 359.0f)
        direction = 0.0f;
    float nvx = -vx_in;
    *vx_out = nvx;
    *vy_out = tanf(direction * (float)(M_PI / 180.0)) * nvx;
}

static void set_mode(MitsuTvGame6State *s, TvGame6Mode mode) {
    if (s->mode == mode)
        return;
    s->mode = mode;
    s->left_score = 0;
    s->right_score = 0;
    tvgame6_reset_ball(s, 1);
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
        float mag = s->turbo ? BALL_SPEED_TURBO : BALL_SPEED_NORMAL;
        s->ball_vx = s->ball_vx < 0 ? -mag : mag;
    }
    if (pressed & ACT_RESIZE_L)
        s->left_h = (s->left_h > PADDLE_H * 0.75f) ? PADDLE_H * 0.5f : PADDLE_H;
    if (pressed & ACT_RESIZE_R)
        s->right_h = (s->right_h > PADDLE_H * 0.75f) ? PADDLE_H * 0.5f : PADDLE_H;

    if (actions & ACT_L_UP) s->left_y -= PADDLE_SPEED;
    if (actions & ACT_L_DOWN) s->left_y += PADDLE_SPEED;
    if (actions & ACT_R_UP) s->right_y -= PADDLE_SPEED;
    if (actions & ACT_R_DOWN) s->right_y += PADDLE_SPEED;
    float lh2 = s->left_h * 0.5f, rh2 = s->right_h * 0.5f;
    if (s->left_y - lh2 < FIELD_Y) s->left_y = FIELD_Y + lh2;
    if (s->left_y + lh2 > FIELD_B) s->left_y = FIELD_B - lh2;
    if (s->right_y - rh2 < FIELD_Y) s->right_y = FIELD_Y + rh2;
    if (s->right_y + rh2 > FIELD_B) s->right_y = FIELD_B - rh2;

    if (!s->waiting_serve) {
        s->ball_x += s->ball_vx;
        s->ball_y += s->ball_vy;
        if (s->marker_cooldown > 0)
            s->marker_cooldown--;
        if (s->ball_y < FIELD_Y || s->ball_y > FIELD_B)
            s->ball_vy = -s->ball_vy;

        if ((s->ball_vx < 0 && hit_side_paddles(s, true)) ||
            (s->ball_vx > 0 && hit_side_paddles(s, false))) {
            bool left_style = s->ball_vx < 0;
            float cy = left_style ? s->left_y : s->right_y;
            float c_point = (s->ball_y - cy) / PADDLE_HALF;
            compute_bounce(s->ball_vx, left_style, c_point, &s->ball_vx, &s->ball_vy);
            s->bounce_count = 0;
        }

        if (s->mode == TVG_VOLLEY) {
            int mx = 0, my = 0, color = 0;
            if (s->marker_cooldown == 0 && hit_barrier(s, &mx, &my, &color)) {
                bool left_style = (color == 0);
                bool correct_dir = left_style ? (s->ball_vx < 0) : (s->ball_vx > 0);
                float c_point;
                if (correct_dir && s->bounce_count < 10) {
                    float ref_cy = my + MARKER_H * 0.5f;
                    c_point = (s->ball_y - ref_cy) / PADDLE_HALF;
                } else {
                    /* Anti-stall: after too many consecutive net bounces (or a
                     * bounce from the "wrong" side), Python randomizes the
                     * escape angle among {-45, 0, +45} instead of deflecting
                     * deterministically. The 0 (flat) option is what actually
                     * lets the ball slip through a gap between tile rows - a
                     * continuously-random angle almost never lands exactly
                     * flat, so the ball would otherwise get re-deflected
                     * forever instead of ever coasting through. */
                    static const float escape_c[3] = { -0.9f, 0.0f, 0.9f };
                    c_point = escape_c[rand() % 3];
                }
                compute_bounce(s->ball_vx, left_style, c_point, &s->ball_vx, &s->ball_vy);
                s->bounce_count++;
                s->marker_cooldown = 8;
            }
        } else if (s->mode == TVG_HOCKEY) {
            bool in_gap = s->ball_y > HOCKEY_GOAL_Y0 && s->ball_y < HOCKEY_GOAL_Y1;
            if (!in_gap) {
                if (s->ball_vx < 0 && s->ball_x <= HOCKEY_WALL_L_X + BALL_W * 0.5f) {
                    s->ball_x = HOCKEY_WALL_L_X + BALL_W * 0.5f;
                    s->ball_vx = -s->ball_vx;
                } else if (s->ball_vx > 0 && s->ball_x >= HOCKEY_WALL_R_X - BALL_W * 0.5f) {
                    s->ball_x = HOCKEY_WALL_R_X - BALL_W * 0.5f;
                    s->ball_vx = -s->ball_vx;
                }
            }
        }

        if (s->ball_x < COURT_X - 8.0f) {
            s->right_score = (s->right_score + 1) % 10;
            tvgame6_reset_ball(s, -1);
        } else if (s->ball_x > COURT_R + 8.0f) {
            s->left_score = (s->left_score + 1) % 10;
            tvgame6_reset_ball(s, 1);
        }
    }
    s->prev_actions = actions;
}

static void render_game(MitsuTvGame6State *s) {
    for (int i = 0; i < FB_W * FB_H; i++)
        s->fb[i] = 0xff19b537;

    fill_rect(s, COURT_X, COURT_Y, COURT_W, COURT_H, 0xff6d70f9);
    fill_rect(s, COURT_X, COURT_Y, COURT_W, 1, 0xff000000);
    fill_rect(s, COURT_X, COURT_Y + 1, COURT_W, 5, 0xffcddcfb);
    fill_rect(s, COURT_X, COURT_B - 5, COURT_W, 5, 0xffcddcfb);
    fill_rect(s, COURT_X, COURT_B - 1, COURT_W, 1, 0xff000000);

    if (s->mode == TVG_VOLLEY)
        draw_center_markers(s);
    else if (s->mode == TVG_TENNIS)
        draw_tennis_net(s);
    else if (s->mode == TVG_HOCKEY)
        draw_hockey_walls(s);

    draw_side_paddles(s, true, 48, s->left_y, s->left_h, 0xffa6e6ff);
    draw_side_paddles(s, false, 265, s->right_y, s->right_h, 0xffa6e6ff);
    fill_rect(s, (int)s->ball_x - BALL_W / 2, (int)s->ball_y - BALL_H / 2,
              BALL_W, BALL_H, 0xffc7dbfd);
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
