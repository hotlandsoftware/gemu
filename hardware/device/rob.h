#pragma once
#include <stdint.h>
#include <stdbool.h>

#define ROB_ARM_STEPS   12   /* 30° per step, 0–11 */
#define ROB_HEIGHT_MAX   5   /* 0 = down, 5 = fully up (matches reference 0–5 range) */

/* Gyromite column indices (arm_step → column mapping after reset at step 0) */
#define ROB_COL_SPINNER  0   /* spinning stand: gyros charged here, rest height = 3 */
#define ROB_COL_B_BTN    1   /* blue button (NES B), rest height = 1 */
#define ROB_COL_A_BTN    2   /* red button (NES A), rest height = 1 - also reset position */
#define ROB_COL_TRAY1    3   /* gyro storage tray 1, rest height = 1 */
#define ROB_COL_TRAY2    4   /* gyro storage tray 2, rest height = 1 */
#define ROB_COL_NONE    -1   /* arm between columns */

#define ROB_GYRO_COUNT   2

typedef struct {
    int  arm_step;     /* 0..11 - 30° per step, wraps; left cmd = +1, right = -1 */
    int  arm_height;   /* 0 = down, ROB_HEIGHT_MAX = fully up */
    bool hands_open;
} RobMotorState;

typedef struct {
    int  column;   /* ROB_COL_* when at rest; meaningless while held */
    bool toppled;  /* knocked over - needs manual replacement */
} RobGyroState;

typedef struct {
    RobMotorState state;
    RobMotorState target;
    float         arm_pos;          /* animated 0..11 step position */
    float         arm_height_pos;   /* animated 0..ROB_HEIGHT_MAX height */
    float         hands_open_pos;   /* animated 0=closed, 1=open */
    uint16_t      bits;              /* 13-bit shift register (bit 0 = newest frame) */
    RobGyroState  gyros[ROB_GYRO_COUNT];
    int           held_gyro;        /* 0 or 1 while carrying, -1 otherwise */
    bool          prev_hands_open;  /* for close/open edge detection */
    bool          btn_a;            /* true → port-2 A bit (red button, col 2) */
    bool          btn_b;            /* true → port-2 B bit (blue button, col 1) */
} RobState;

void rob_init (RobState *rob);
/* Call once per rendered PPU frame. */
void rob_frame(RobState *rob, const uint32_t *pixels_argb, int w, int h);
