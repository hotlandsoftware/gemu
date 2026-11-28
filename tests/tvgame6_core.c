/* Nintendo's Color TV-Game 6 on the core (stage O2): a script of frames
 * with input through the three games, double paddles, turbo and the
 * smaller paddles.  Built with -DO_HEAD against HEAD's driver it runs
 * HEAD's frame; HEAD's rand() (volleyball's anti-stall pick) is replaced
 * here by the generator the m58815p device keeps in its state, with the
 * same seed, so the two runs pick alike.  Run: ninja check-tvgame6_core */
#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stdlib.h>
#ifdef O_HEAD
static uint32_t head_rng = 0x2545F491u;
static int head_rand(void) {
    head_rng ^= head_rng << 13; head_rng ^= head_rng >> 17; head_rng ^= head_rng << 5;
    return (int)(head_rng & 0x7FFFFFFFu);
}
#define rand() head_rand()
#endif
#include "../hardware/machine/other/machine_tvgame6.c"
#include "gemu/sha256.h"

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

#ifdef O_HEAD
#define G(s) (s)
static void frame(MitsuTvGame6State *s, uint32_t actions) { update_game(s, actions); render_game(s); }
#else
#define G(s) ((s)->g)
static void frame(MitsuTvGame6State *s, uint32_t actions) {
    tvgame6_set_actions(s, actions);
    tvgame6_frame(s);
    render_game(s);
}
void tvgame6_core_checks(void);
#endif

static void hash(const char *label, const void *p, size_t n) {
    uint8_t d[GEMU_SHA256_DIGEST_LEN];
    GemuSha256Ctx c;
    gemu_sha256_init(&c);
    gemu_sha256_update(&c, p, n);
    gemu_sha256_final(&c, d);
    char hex[65];
    gemu_sha256_hex(d, hex);
    printf(" %s %.12s", label, hex);
}

static void report(MitsuTvGame6State *s, int frames) {
    printf("%4d", frames);
    hash("fb", s->fb, sizeof s->fb);
    printf(" mode %d dbl %d turbo %d serve %d L %d/%d R %d/%d ball %d,%d h %+d/%d v %+d/%d/%d/%d"
           " score %d-%d bounces %d\n",
           G(s)->mode, G(s)->double_paddles, G(s)->turbo, G(s)->waiting_serve, G(s)->left_y,
           G(s)->left_h, G(s)->right_y, G(s)->right_h, G(s)->ball_x, G(s)->ball_y, G(s)->h_dir,
           G(s)->h_step, G(s)->v_dir, G(s)->v_div, G(s)->v_step, G(s)->v_acc, G(s)->left_score,
           G(s)->right_score, G(s)->bounce_count);
}

/* The script: each game for 600 frames, served at once and whenever a
 * point is over; the paddles chase the ball (left a little behind it, so
 * points get scored); at 250 double paddles, at 450 turbo and the left
 * paddle small, at 550 both back. */
static uint32_t actions_at(MitsuTvGame6State *s, int f) {
    uint32_t a = 0;
    if (f == 600) a |= ACT_VOLLEY;
    if (f == 1200) a |= ACT_HOCKEY;
    if (f == 1800) a |= ACT_TENNIS;
    if (f % 600 == 250 || f % 600 == 550) a |= ACT_TOGGLE_PADDLES;
    if (f % 600 == 450 || f % 600 == 550) a |= ACT_SPEED_TOGGLE | ACT_RESIZE_L;
    if (G(s)->waiting_serve && (f & 1)) a |= ACT_SERVE;
    int target_l = G(s)->ball_y + ((f / 97) % 3 - 1) * 14;
    if (G(s)->left_y < target_l - 2) a |= ACT_L_DOWN;
    if (G(s)->left_y > target_l + 2) a |= ACT_L_UP;
    if (G(s)->right_y < G(s)->ball_y - 2) a |= ACT_R_DOWN;
    if (G(s)->right_y > G(s)->ball_y + 2) a |= ACT_R_UP;
    return a;
}

int main(int argc, char **argv) {
    (void)argv;
    MitsuConfig c = { .machine = MITSU_MACHINE_TVGAME6, .display_type = GEMU_DISPLAY_NONE };
    MitsuTvGame6State *s = mitsu_tvgame6_create(&c);
    CHECK(s);
#ifndef O_HEAD
    G(s)->rng = 0x2545F491u;   /* HEAD's run: head_rand()'s seed */
#endif
    static const int at[] = { 1, 60, 300, 600, 700, 900, 1200, 1500, 1800, 2000 };
    int done = 0, scored = 0;
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
        while (done < at[i]) {
            frame(s, actions_at(s, done));
            scored |= G(s)->left_score | G(s)->right_score;
            done++;
        }
        if (argc > 1) report(s, done);
    }
    CHECK(scored && G(s)->mode == TVG_TENNIS);
    mitsu_tvgame6_destroy(s);
#ifndef O_HEAD
    if (argc < 2) tvgame6_core_checks();
#endif
    printf("tvgame6_core: %u checks passed\n", checks);
    return 0;
}

#ifndef O_HEAD
/* The core machine's own checks (stage O2). */
void tvgame6_core_checks(void) {
    MitsuConfig c = { .machine = MITSU_MACHINE_TVGAME6, .display_type = GEMU_DISPLAY_NONE };
    MitsuTvGame6State *s = mitsu_tvgame6_create(&c), *t = mitsu_tvgame6_create(&c);
    CHECK(s && t);
    /* frames are the frame timer's ticks: 60 a second, exactly */
    for (int i = 0; i < 60; i++) tvgame6_frame(s);
    CHECK(gemu_machine_time(s->machine) == gemu_clocks(60, 60));
    /* served, the ball moves 3 a frame */
    tvgame6_set_actions(s, ACT_SERVE);
    tvgame6_frame(s);
    tvgame6_set_actions(s, 0);
    int x0 = s->g->ball_x;
    tvgame6_frame(s);
    CHECK(!s->g->waiting_serve && s->g->ball_x - x0 == 3 * s->g->h_dir);
    /* volleyball, played a while; state round trip (the generator
     * included); another machine runs on alike */
    tvgame6_set_actions(s, ACT_VOLLEY);
    tvgame6_frame(s);
    for (int i = 0; i < 300; i++) {
        tvgame6_set_actions(s, s->g->waiting_serve && (i & 1) ? ACT_SERVE : 0);
        tvgame6_frame(s);
    }
    uint8_t *a, *b;
    size_t na, nb;
    CHECK(gemu_machine_save_state(s->machine, &a, &na));
    for (int i = 0; i < 20; i++) tvgame6_frame(s);
    CHECK(gemu_machine_load_state(s->machine, a, na));
    CHECK(gemu_machine_save_state(s->machine, &b, &nb));
    CHECK(na == nb && !memcmp(a, b, na));
    free(b);
    CHECK(gemu_machine_load_state(t->machine, a, na) && t->g->mode == TVG_VOLLEY);
    for (int i = 0; i < 600; i++) {
        uint32_t act = (s->g->waiting_serve && (i & 1) ? ACT_SERVE : 0) |
                       ((i / 40) & 1 ? ACT_R_UP : ACT_R_DOWN);
        tvgame6_set_actions(s, act);
        tvgame6_set_actions(t, act);
        tvgame6_frame(s);
        tvgame6_frame(t);
    }
    CHECK(s->g->ball_x == t->g->ball_x && s->g->ball_y == t->g->ball_y && s->g->rng == t->g->rng &&
          s->g->left_score == t->g->left_score && s->g->right_score == t->g->right_score);
    free(a);
    /* the screen is the picture */
    const GemuBitmap *bmp = gemu_screen_render(s->screen);
    CHECK(bmp->width == FB_W && bmp->height == FB_H && bmp->pix[0] == mode_palette[TVG_VOLLEY].bg);
    mitsu_tvgame6_destroy(t);
    mitsu_tvgame6_destroy(s);
}
#endif
