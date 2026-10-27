#pragma once
/*
 * Aston Martin Lagonda (Series 3, 1985) instrument computer: the Javelina
 * AUDIT (AUtomotive Data Information Terminal).  An STD-bus card cage:
 *
 *   AUDIT CPU   Z80A, 2732 program EPROM (centre) and 2732 graphics EPROM
 *               (edge), 4 x 2114 = 2 KB RAM
 *   AUDIT ATI   ADC0809 8-channel A/D, LM2907 frequency-to-voltage
 *               converters (speed, engine), 2 x 4518 BCD counters (trip)
 *   AUDIT DSI   8255 PPI (warning switches, gear selector, buttons) and an
 *               MM58167 real-time clock
 *   CVDG-48K    3 x TMS9128 VDP with 16 KB each, one per CRT
 *   ASP32K      MSM5205 ADPCM speech from up to eight 2732s (five fitted)
 *
 * Memory: program ROM 0000-0FFF, graphics ROM 1000-1FFF, RAM 2000-23FF
 * and 3000-33FF (each 1 KB mirrored through its 4 KB block), open bus
 * above 4000.
 *
 * I/O (A0-A7), as the program uses it:
 *   00-02  8255 ports A, B, C (inputs; the program never writes the mode)
 *   08-0F  ADC0809: OUT starts a conversion on channel port&7, IN reads
 *   10, 18 trip counter, low and high byte (4 BCD digits, tenths of a mile)
 *   20     output latch, written 00 every pass of the main loop
 *   40-5F  MM58167 registers 00-1F
 *   80-85  VDPs 0-2: even = data, odd = control (VDP n at 80h + 2n)
 *   FE     speech command latch (W)
 *   FF     lamp inputs and two buttons (R)
 *
 * Interrupts are never enabled: the program polls everything in one loop.
 */
#include "z80.h"
#include "z80cfg.h"
#include "tms9918.h"
#include "msm5205.h"
#include "mm58167.h"
#include "adc0809.h"
#include "ppi8255.h"
#include "gemu/monitor.h"
#include "gemu/gemu_display.h"
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define LAG_ROM_SIZE   0x1000
#define LAG_SPEECH_N   8          /* sockets A-H on the ASP32K; A-E fitted */
#define LAG_VDP_N      3
#define LAG_AUDIO_RATE 48000
#define LAG_AUDIO_BUF  4096

/* Gear selector positions, as the PRND21 display orders them. */
enum { LAG_GEAR_P, LAG_GEAR_R, LAG_GEAR_N, LAG_GEAR_D, LAG_GEAR_2, LAG_GEAR_1 };

/* The car around the computer: what its sensors and switches report. */
typedef struct {
    double  speed_mph;     /* speed sensor, through the LM2907 to ADC ch1 */
    double  rpm;           /* ignition pulses, through the LM2907 to ADC ch0 */
    double  fuel_pct;      /* fuel sender, ADC ch2 */
    double  temp;          /* water temperature sender, ADC ch3 (counts) */
    double  battery;       /* battery voltage sense, ADC ch7 (counts) */
    double  adc_extra[3];  /* ADC ch4-6: unused by the program */
    int     gear;
    uint8_t warn_a;        /* 8255 port A pins (warning switches, active low) */
    uint8_t warn_b;        /* 8255 port B pins */
    bool    left_turn, right_turn;   /* indicator stalk (the flasher pulses the lamps) */
    uint8_t lamps;         /* port FF bits 3-5, 7 */
    bool    btn_hour, btn_minute, btn_units, btn_check, btn_cancel;
    double  trip_miles;    /* distance the trip counter has counted */
} LagondaCar;

typedef struct {
    uint8_t  latch;        /* 74LS374 */
    uint16_t counter;      /* 4040: nibble address within the 2 KB message */
    bool     playing;
    Msm5205  msm;
} LagondaSpeech;

typedef struct LagondaState {
    Z80          cpu;
    const Z80Config *cfg;
    double       clock_hz;

    uint8_t      code[LAG_ROM_SIZE], gfx[LAG_ROM_SIZE];
    uint8_t      ram[2][0x400];
    uint8_t      speech_rom[LAG_SPEECH_N][LAG_ROM_SIZE];
    bool         speech_fitted[LAG_SPEECH_N];

    Tms9918      vdp[LAG_VDP_N];
    Ppi8255      ppi;
    Adc0809      adc;
    Mm58167      rtc;
    LagondaSpeech speech;
    uint16_t     trip_bcd;   /* the two 4518s */
    uint8_t      latch20;

    LagondaCar   car;
    uint32_t     held, prev_held;
    uint32_t     inject_mask;
    int          inject_frames;

    /* Bus-cycle schedule of the clocked parts */
    double       next_line, next_vck, next_sample;
    double       line_cycles, vck_cycles, sample_cycles;
    uint64_t     frame_cycle;       /* cycle the current VDP frame began */
    uint64_t     insn_count;

    /* Audio: DAC -> filter -> output */
    SDL_AudioDeviceID audio_dev;
    float        audio_buf[LAG_AUDIO_BUF];
    int          audio_n;
    double       lp1, lp2, dc_x, dc_y;

    uint32_t    *panel_fb;   /* head 0: the three screens side by side */
    GemuDisplay *display;
    GemuMonitor *monitor;
} LagondaState;

LagondaState *lagonda_create (const Z80Config *cfg);
void          lagonda_run    (LagondaState *s, const Z80Config *cfg);
void          lagonda_destroy(LagondaState *s);
