#include "rob.h"
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
static void apply_command(RobMotorState *st, int cmd) {
    switch (cmd) {
    case 1: /* Reset — arm rises to full height, rotation to 0, hands open */
        st->arm_step   = 0;
        st->arm_height = ROB_HEIGHT_MAX;
        st->hands_open = true;
        break;
    case 2: /* Down one */
        if (st->arm_height > 0) st->arm_height--;
        break;
    case 4: /* Left — rotate arm assembly one step counterclockwise */
        st->arm_step = (st->arm_step + 1) % ROB_ARM_STEPS;
        break;
    case 5: /* Up two */
        st->arm_height += 2;
        if (st->arm_height > ROB_HEIGHT_MAX) st->arm_height = ROB_HEIGHT_MAX;
        break;
    case 6: /* Close arms */
        st->hands_open = false;
        break;
    case 8: /* Right — rotate arm assembly one step clockwise */
        st->arm_step = (st->arm_step - 1 + ROB_ARM_STEPS) % ROB_ARM_STEPS;
        break;
    case 10: /* Open arms */
        st->hands_open = true;
        break;
    case 12: /* Up one */
        if (st->arm_height < ROB_HEIGHT_MAX) st->arm_height++;
        break;
    case 13: /* Down two */
        st->arm_height -= 2;
        if (st->arm_height < 0) st->arm_height = 0;
        break;
    /* 0=Blink LED, 9=LED on: visual-only, no motor action */
    default: break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void rob_init(RobState *rob) {
    memset(rob, 0, sizeof *rob);
    rob->state.hands_open = true;
}

void rob_frame(RobState *rob, const uint32_t *pixels_argb, int w, int h) {
    int luma = sample_luma(pixels_argb, w, h);

    /* Shift register: new sample enters at bit 0, oldest falls off bit 12 */
    rob->bits = (rob->bits << 1) & 0x1fff;
    if (luma >= 80) rob->bits |= 1;

    if (bits_follow_pattern(rob->bits)) {
        int cmd = extract_command(rob->bits);
        rob->bits = 0x1fff; /* saturate to prevent immediate re-trigger */
        apply_command(&rob->state, cmd);
    }
}
