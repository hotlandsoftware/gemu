#include "rob.h"
#include <stdio.h>
#include <string.h>

/* ── Screen sampling (matches the reference Mesen Lua script) ────────────────
 * 4×4 sparse grid — the same x/y coordinates the Lua script uses.
 * Rec.709 luma; threshold 80 (same as Lua "is_bright").
 * ─────────────────────────────────────────────────────────────────────────── */
static const int XVALS[4] = {53, 103, 153, 203};
static const int YVALS[4] = {60, 100, 140, 180};

static int sample_luma(const uint32_t *px, int w, int h) {
    long sum = 0;
    int  n   = 0;
    for (int xi = 0; xi < 4; xi++) {
        for (int yi = 0; yi < 4; yi++) {
            int x = XVALS[xi], y = YVALS[yi];
            if (x < w && y < h) {
                uint32_t c = px[y * w + x];
                int r = (c >> 16) & 0xFF;
                int g = (c >>  8) & 0xFF;
                int b =  c        & 0xFF;
                /* Rec.709: 0.2126R + 0.7152G + 0.0722B */
                sum += (r * 2126 + g * 7152 + b * 722 + 5000) / 10000;
                n++;
            }
        }
    }
    return n ? (int)(sum / n) : 0;
}

/* ── ROB command protocol ────────────────────────────────────────────────────
 * The game sends a 13-frame bit-serial sequence.  Odd bit-positions (1,3,5,7)
 * are "clock" bits that must be bright.  Even positions (0,2,4,6) carry the
 * 4-bit command W:X:Y:Z.  Three dark lead-in frames must precede the burst.
 *
 * Reference: robert-mesen.lua (Mesen scripting interface).
 * ─────────────────────────────────────────────────────────────────────────── */

static bool bits_follow_pattern(uint16_t bits) {
    /* Test-mode special cases (alternating full patterns) */
    if (bits == 0x1555 || bits == 0x0aaa) return true;

    /* bits_mask  = 0b1111110101010 — positions to check                    */
    /* correct_pattern = 0b0001010101010 — expected values (clock=1, lead=0) */
    return (bits & 0x1faa) == 0x02aa;
}

static int extract_command(uint16_t bits) {
    if (bits == 0x1555 || bits == 0x0aaa) return 0; /* Blink LED / test */
    int w = (bits & 0x40) >> 3; /* bit 6 → bit 3 */
    int x = (bits & 0x10) >> 2; /* bit 4 → bit 2 */
    int y = (bits & 0x04) >> 1; /* bit 2 → bit 1 */
    int z =  bits & 0x01;       /* bit 0 → bit 0 */
    return w | x | y | z;
}

/* ROB command table (Gyromite protocol, 0-indexed) */
static void apply_command(RobMotorState *target, int cmd) {
    switch (cmd) {
    case 1: /* Reset — arm rises to full height, rotation to 0, hands open */
        target->arm_step   = 0;
        target->arm_height = ROB_HEIGHT_MAX;
        target->hands_open = true;
        break;
    case 2: /* Down one */
        if (target->arm_height > 0) target->arm_height--;
        break;
    case 4: /* Left — rotate arm assembly one step counterclockwise */
        target->arm_step = (target->arm_step + 1) % ROB_ARM_STEPS;
        break;
    case 5: /* Up two */
        target->arm_height += 2;
        if (target->arm_height > ROB_HEIGHT_MAX) target->arm_height = ROB_HEIGHT_MAX;
        break;
    case 6: /* Close arms */
        target->hands_open = false;
        break;
    case 8: /* Right — rotate arm assembly one step clockwise */
        target->arm_step = (target->arm_step - 1 + ROB_ARM_STEPS) % ROB_ARM_STEPS;
        break;
    case 10: /* Open arms */
        target->hands_open = true;
        break;
    case 12: /* Up one */
        if (target->arm_height < ROB_HEIGHT_MAX) target->arm_height++;
        break;
    case 13: /* Down two */
        target->arm_height -= 2;
        if (target->arm_height < 0) target->arm_height = 0;
        break;
    /* 0=Blink LED, 9=LED on: visual-only, no motor action */
    default: break;
    }
}

/* ── Gyromite gyro simulation ────────────────────────────────────────────────
 * Column layout (arm_step → column after reset at step 0):
 *   step  0 → col 2  red/A button    (reset position)
 *   step  1 → col 1  blue/B button
 *   step  2 → col 0  spinner stand
 *   step 11 → col 3  gyro tray 1
 *   step 10 → col 4  gyro tray 2
 *
 * A gyro rests at height 3 on the spinner and height 0 everywhere else.
 * btn_a is set when a gyro sits at col 2 (not held, not toppled).
 * btn_b is set when a gyro sits at col 1.
 * ─────────────────────────────────────────────────────────────────────────── */

static int arm_step_to_column(int arm_step) {
    switch (arm_step) {
        case  0: return ROB_COL_A_BTN;
        case  1: return ROB_COL_B_BTN;
        case  2: return ROB_COL_SPINNER;
        case 10: return ROB_COL_TRAY2;
        case 11: return ROB_COL_TRAY1;
        default: return ROB_COL_NONE;
    }
}

static int gyro_rest_height(int col) {
    return (col == ROB_COL_SPINNER) ? 3 : 0;
}

static const char *rob_col_name(int col) {
    switch (col) {
        case ROB_COL_SPINNER: return "spinner";
        case ROB_COL_B_BTN:   return "b-button";
        case ROB_COL_A_BTN:   return "a-button";
        case ROB_COL_TRAY1:   return "tray1";
        case ROB_COL_TRAY2:   return "tray2";
        case ROB_COL_NONE:    return "none";
        default:              return "?";
    }
}

static void rob_debug_state(const char *tag, const RobState *rob) {
    int col = arm_step_to_column(rob->state.arm_step);
    char held_buf[16];
    if (rob->held_gyro >= 0)
        snprintf(held_buf, sizeof held_buf, "gyro%d", rob->held_gyro + 1);
    else
        snprintf(held_buf, sizeof held_buf, "none");

    fprintf(stderr,
            "rob: %s step=%d col=%s(%d) height=%d hands=%s held=%s "
            "target={step=%d,height=%d,hands=%s} "
            "gyro1={col=%s(%d),toppled=%d} gyro2={col=%s(%d),toppled=%d}\n",
            tag,
            rob->state.arm_step, rob_col_name(col), col,
            rob->state.arm_height,
            rob->state.hands_open ? "open" : "closed",
            held_buf,
            rob->target.arm_step,
            rob->target.arm_height,
            rob->target.hands_open ? "open" : "closed",
            rob_col_name(rob->gyros[0].column), rob->gyros[0].column, rob->gyros[0].toppled,
            rob_col_name(rob->gyros[1].column), rob->gyros[1].column, rob->gyros[1].toppled);
}

static float move_toward(float cur, float target, float step) {
    float d = target - cur;
    if (d > -step && d < step)
        return target;
    return cur + (d < 0.0f ? -step : step);
}

static float move_step_circular(float cur, int target_step, float step) {
    float target = (float)target_step;
    float d = target - cur;
    if (d > ROB_ARM_STEPS * 0.5f)
        d -= ROB_ARM_STEPS;
    if (d < -ROB_ARM_STEPS * 0.5f)
        d += ROB_ARM_STEPS;
    if (d > -step && d < step)
        cur = target;
    else
        cur += (d < 0.0f ? -step : step);
    while (cur < 0.0f) cur += (float)ROB_ARM_STEPS;
    while (cur >= (float)ROB_ARM_STEPS) cur -= (float)ROB_ARM_STEPS;
    return cur;
}

static bool nearly(float a, float b) {
    float d = a - b;
    return d > -0.001f && d < 0.001f;
}

static void rob_motion_tick(RobState *rob) {
    const float arm_speed = 0.007f;
    const float height_speed = 0.011f;
    const float hand_speed = 0.020f;

    rob->arm_pos = move_step_circular(rob->arm_pos, rob->target.arm_step, arm_speed);
    rob->arm_height_pos = move_toward(rob->arm_height_pos,
                                      (float)rob->target.arm_height,
                                      height_speed);
    rob->hands_open_pos = move_toward(rob->hands_open_pos,
                                      rob->target.hands_open ? 1.0f : 0.0f,
                                      hand_speed);

    if (nearly(rob->arm_pos, (float)rob->target.arm_step))
        rob->state.arm_step = rob->target.arm_step;
    if (nearly(rob->arm_height_pos, (float)rob->target.arm_height))
        rob->state.arm_height = rob->target.arm_height;
    if (nearly(rob->hands_open_pos, rob->target.hands_open ? 1.0f : 0.0f))
        rob->state.hands_open = rob->target.hands_open;
}

static void rob_update_buttons(RobState *rob) {
    rob->btn_a = rob->btn_b = false;
    for (int i = 0; i < ROB_GYRO_COUNT; i++) {
        if (i == rob->held_gyro) continue;
        const RobGyroState *g = &rob->gyros[i];
        if (g->toppled) continue;
        if (g->column == ROB_COL_A_BTN) rob->btn_a = true;
        if (g->column == ROB_COL_B_BTN) rob->btn_b = true;
    }
}

static void rob_gyro_tick(RobState *rob) {
    bool open = rob->state.hands_open;
    int  col  = arm_step_to_column(rob->state.arm_step);
    int  h    = rob->state.arm_height;

    /* Close edge: try to pick up a gyro at the current position */
    if (rob->prev_hands_open && !open && col != ROB_COL_NONE && rob->held_gyro < 0) {
        for (int i = 0; i < ROB_GYRO_COUNT; i++) {
            RobGyroState *g = &rob->gyros[i];
            if (!g->toppled && g->column == col && gyro_rest_height(col) == h) {
                rob->held_gyro = i;
                rob_debug_state("pickup", rob);
                break;
            }
        }
    }

    /* Open edge: drop the held gyro at the current column */
    if (!rob->prev_hands_open && open && rob->held_gyro >= 0 && col != ROB_COL_NONE) {
        RobGyroState *g = &rob->gyros[rob->held_gyro];
        g->column = col;
        /* Topple if another gyro already occupies this column */
        for (int i = 0; i < ROB_GYRO_COUNT; i++) {
            if (i == rob->held_gyro) continue;
            RobGyroState *o = &rob->gyros[i];
            if (!o->toppled && o->column == col) {
                g->toppled = true;
                break;
            }
        }
        rob->held_gyro = -1;
        rob_debug_state("drop", rob);
    }

    rob->prev_hands_open = open;
    rob_update_buttons(rob);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void rob_init(RobState *rob) {
    memset(rob, 0, sizeof *rob);
    rob->state.hands_open   = true;
    rob->target             = rob->state;
    rob->arm_pos            = (float)rob->state.arm_step;
    rob->arm_height_pos     = (float)rob->state.arm_height;
    rob->hands_open_pos     = rob->state.hands_open ? 1.0f : 0.0f;
    rob->gyros[0].column    = ROB_COL_TRAY1;
    rob->gyros[1].column    = ROB_COL_TRAY2;
    rob->held_gyro          = -1;
    rob->prev_hands_open    = true;
}

void rob_frame(RobState *rob, const uint32_t *pixels_argb, int w, int h) {
    int luma = sample_luma(pixels_argb, w, h);

    /* Shift register: new sample enters at bit 0, oldest falls off bit 12 */
    rob->bits = (rob->bits << 1) & 0x1fff;
    if (luma >= 80) rob->bits |= 1;

    if (bits_follow_pattern(rob->bits)) {
        int cmd = extract_command(rob->bits);
        rob->bits = 0x1fff; /* saturate to prevent immediate re-trigger */
        fprintf(stderr, "rob: cmd=%d\n", cmd);
        apply_command(&rob->target, cmd);
        rob_debug_state("state", rob);
    }

    rob_motion_tick(rob);
    rob_gyro_tick(rob);
}
