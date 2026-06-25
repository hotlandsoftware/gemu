#pragma once
#include <stdint.h>
#include <stdbool.h>

#define ROB_ARM_STEPS   12   /* 30° per step, 0–11 */
#define ROB_HEIGHT_MAX   5   /* 0 = down, 5 = fully up (matches reference 0–5 range) */

typedef struct {
    int  arm_step;     /* 0..11 — 30° per step, wraps; left cmd = +1, right = -1 */
    int  arm_height;   /* 0 = down, ROB_HEIGHT_MAX = fully up */
    bool hands_open;
} RobMotorState;

typedef struct {
    RobMotorState state;
    uint16_t      bits;   /* 13-bit shift register (bit 0 = newest frame) */
} RobState;

void rob_init (RobState *rob);
/* Call once per rendered PPU frame. */
void rob_frame(RobState *rob, const uint32_t *pixels_argb, int w, int h);
