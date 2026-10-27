/*
 * Aston Martin Lagonda instrument computer (Javelina AUDIT) - see lagonda.h.
 *
 * The memory and I/O maps are read from what the program does: it boots
 * at 0000h with interrupts off, clears RAM at 2000h and 3000h, programs
 * the three VDPs from tables in the graphics EPROM at 1000h, then loops
 * forever reading the switches, the A/D converter, the trip counter and
 * the clock and redrawing the screens.
 *
 * Speech card (ASP32K): the program writes a command to port FEh - 11h,
 * 13h, 21h ... 53h, the ten messages in priority order.  The 74LS374
 * latches it; its high nibble goes to the 74C42 decoder, whose outputs
 * 1-8 enable EPROM sockets A-H (0, written at power-on, selects none and
 * holds the MSM5205 in reset); bit 1 is the EPROM's A11, picking the
 * message in the lower or upper 2 KB, and bit 0 must be set to play.  The
 * write restarts the 4040 counter, which the MSM5205's 4 kHz VCK clocks:
 * its lowest bit picks the nibble (high first, through the 4019s), the
 * other eleven address the EPROM.  When the counter wraps after 4096
 * nibbles - the whole 2 KB, 1.024 s - the message is over.
 */
#include "lagonda.h"
#include "gemu/screendump.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define VDP_HZ     (10738635.0 / 2.0)   /* pixel clock */
#define VCK_HZ     4000.0               /* 384 kHz resonator / 96 */
#define ADC_CLK_HZ 640000.0             /* ADC0809 typical clock (the ATI card's 555) */
#define FLASHER_HZ 1.5                  /* the car's indicator flasher, 90/min */

#define PANEL_GAP  8
#define PANEL_W    (3 * TMS9918_FB_W + 2 * PANEL_GAP)
#define PANEL_H    TMS9918_FB_H

/* Head 0 shows the three CRTs as they sit in the binnacle: the tachometer
 * (VDP 2) on the left, the centre display (VDP 0), the speedometer (VDP 1). */
static const int panel_order[LAG_VDP_N] = { 2, 0, 1 };

/* ── Actions ─────────────────────────────────────────────────────────────── */

enum {
    ACT_SPEED_UP, ACT_SPEED_DOWN, ACT_RPM_UP, ACT_RPM_DOWN,
    ACT_FUEL_UP, ACT_FUEL_DOWN, ACT_TEMP_UP, ACT_TEMP_DOWN,
    ACT_GEAR_UP, ACT_GEAR_DOWN, ACT_LEFT_TURN, ACT_RIGHT_TURN,
    ACT_MAIN_BEAM, ACT_DIPPED_BEAM, ACT_LAMP5, ACT_SIDELIGHTS,
    ACT_HOUR, ACT_MINUTE, ACT_UNITS, ACT_CHECK, ACT_CANCEL, ACT_TRIP_RESET,
    ACT_REAR_WINDOW,
    ACT_OIL_PRESSURE, ACT_WATER_TEMP, ACT_OIL_TEMP, ACT_BRAKE_FLUID, ACT_BULB, ACT_ICE,
    ACT_PARKING_BRAKE, ACT_SEAT_BELTS, ACT_BOOT,
    ACT_COUNT
};

static const GemuActionDef lagonda_actions[ACT_COUNT] = {
    { "speed_up",      GEMU_ACTION(ACT_SPEED_UP),      "Up" },
    { "speed_down",    GEMU_ACTION(ACT_SPEED_DOWN),    "Down" },
    { "rpm_up",        GEMU_ACTION(ACT_RPM_UP),        "Right" },
    { "rpm_down",      GEMU_ACTION(ACT_RPM_DOWN),      "Left" },
    { "fuel_up",       GEMU_ACTION(ACT_FUEL_UP),       "PageUp" },
    { "fuel_down",     GEMU_ACTION(ACT_FUEL_DOWN),     "PageDown" },
    { "temp_up",       GEMU_ACTION(ACT_TEMP_UP),       "Home" },
    { "temp_down",     GEMU_ACTION(ACT_TEMP_DOWN),     "End" },
    { "gear_up",       GEMU_ACTION(ACT_GEAR_UP),       "[" },
    { "gear_down",     GEMU_ACTION(ACT_GEAR_DOWN),     "]" },
    { "left_turn",     GEMU_ACTION(ACT_LEFT_TURN),     "Q" },
    { "right_turn",    GEMU_ACTION(ACT_RIGHT_TURN),    "E" },
    { "main_beam",     GEMU_ACTION(ACT_MAIN_BEAM),     "H" },
    { "dipped_beam",   GEMU_ACTION(ACT_DIPPED_BEAM),   "J" },
    { "lamp5",         GEMU_ACTION(ACT_LAMP5),         "K" },
    { "sidelights",    GEMU_ACTION(ACT_SIDELIGHTS),    "L" },
    { "hour",          GEMU_ACTION(ACT_HOUR),          "T" },
    { "minute",        GEMU_ACTION(ACT_MINUTE),        "Y" },
    { "units",         GEMU_ACTION(ACT_UNITS),         "U" },
    { "check",         GEMU_ACTION(ACT_CHECK),         "C" },
    { "cancel",        GEMU_ACTION(ACT_CANCEL),        "X" },
    { "trip_reset",    GEMU_ACTION(ACT_TRIP_RESET),    "R" },
    { "rear_window",   GEMU_ACTION(ACT_REAR_WINDOW),   "W" },
    { "oil_pressure",  GEMU_ACTION(ACT_OIL_PRESSURE),  "F1" },
    { "water_temp",    GEMU_ACTION(ACT_WATER_TEMP),    "F2" },
    { "oil_temp",      GEMU_ACTION(ACT_OIL_TEMP),      "F3" },
    { "brake_fluid",   GEMU_ACTION(ACT_BRAKE_FLUID),   "F4" },
    { "bulb_failure",  GEMU_ACTION(ACT_BULB),          "F5" },
    { "ice",           GEMU_ACTION(ACT_ICE),           "F6" },
    { "parking_brake", GEMU_ACTION(ACT_PARKING_BRAKE), "F7" },
    { "seat_belts",    GEMU_ACTION(ACT_SEAT_BELTS),    "F8" },
    { "boot",          GEMU_ACTION(ACT_BOOT),          "F9" },
};

/* ── Switches: name, where the pin is, and its sense ─────────────────────── */

typedef enum { SW_PORT_A, SW_PORT_B, SW_LAMPS, SW_TURN_L, SW_TURN_R, SW_BUTTON } SwKind;

typedef struct {
    const char *name;
    SwKind      kind;
    uint8_t     bit;
    bool        active_low;   /* closing the switch pulls the pin low */
    const char *what;
} SwitchDef;

static const SwitchDef switches[] = {
    { "oil_pressure",  SW_PORT_A, 7, true,  "low oil pressure (message A, speech 11h)" },
    { "water_temp",    SW_PORT_A, 6, true,  "high water temperature (13h)" },
    { "oil_temp",      SW_PORT_A, 5, true,  "high oil temperature (21h)" },
    { "brake_fluid",   SW_PORT_A, 4, true,  "low brake fluid (23h)" },
    { "bulb_failure",  SW_PORT_A, 2, true,  "bulb failure (33h)" },
    { "ice",           SW_PORT_A, 0, true,  "ice warning (43h)" },
    { "seat_belts",    SW_PORT_B, 7, true,  "seat belts (message B)" },
    { "parking_brake", SW_PORT_B, 6, true,  "parking brake" },
    { "choke",         SW_PORT_B, 5, true,  "choke" },
    { "brake_vacuum",  SW_PORT_B, 4, true,  "low brake vacuum" },
    { "boot",          SW_PORT_B, 3, true,  "boot open" },
    { "fuel_flap",     SW_PORT_B, 2, true,  "fuel flap open" },
    { "wash_level",    SW_PORT_B, 1, true,  "low wash level" },
    { "rear_window",   SW_PORT_B, 0, false, "rear window heater on" },
    { "left_turn",     SW_TURN_L, 2, false, "left indicator (flashing)" },
    { "right_turn",    SW_TURN_R, 6, false, "right indicator (flashing)" },
    { "main_beam",     SW_LAMPS,  3, false, "main beam lamp" },
    { "dipped_beam",   SW_LAMPS,  4, false, "dipped beam lamp" },
    { "lamp5",         SW_LAMPS,  5, false, "lamp input 5 (the -D symbol)" },
    { "sidelights",    SW_LAMPS,  7, false, "sidelights (the paired-lamp symbol)" },
    { "hour",          SW_BUTTON, 0, true,  "clock hour button (8255 PC6)" },
    { "minute",        SW_BUTTON, 1, true,  "clock minute button (PC5)" },
    { "units",         SW_BUTTON, 2, true,  "mph/km button (PC7)" },
    { "check",         SW_BUTTON, 3, true,  "system check button: speaks 'all systems go' or 'warning' (port FF bit 0)" },
    { "cancel",        SW_BUTTON, 4, true,  "cancels message centre B until reset (port FF bit 1)" },
};
#define N_SWITCHES (int)(sizeof switches / sizeof *switches)

static bool *button_ptr(LagondaCar *c, int b) {
    switch (b) {
    case 0: return &c->btn_hour;
    case 1: return &c->btn_minute;
    case 2: return &c->btn_units;
    case 3: return &c->btn_check;
    default: return &c->btn_cancel;
    }
}

static bool switch_get(const LagondaState *s, const SwitchDef *d) {
    const LagondaCar *c = &s->car;
    switch (d->kind) {
    case SW_PORT_A: return ((c->warn_a >> d->bit) & 1u) != d->active_low;
    case SW_PORT_B: return ((c->warn_b >> d->bit) & 1u) != d->active_low;
    case SW_LAMPS:  return (c->lamps >> d->bit) & 1u;
    case SW_TURN_L: return c->left_turn;
    case SW_TURN_R: return c->right_turn;
    default:        return *button_ptr((LagondaCar *)c, d->bit);
    }
}

static void switch_set(LagondaState *s, const SwitchDef *d, bool on) {
    LagondaCar *c = &s->car;
    uint8_t m = (uint8_t)(1u << d->bit);
    bool pin_high = on != d->active_low;
    switch (d->kind) {
    case SW_PORT_A: c->warn_a = pin_high ? (c->warn_a | m) : (c->warn_a & ~m); break;
    case SW_PORT_B: c->warn_b = pin_high ? (c->warn_b | m) : (c->warn_b & ~m); break;
    case SW_LAMPS:  c->lamps  = on ? (c->lamps | m) : (c->lamps & ~m); break;
    case SW_TURN_L: c->left_turn = on; break;
    case SW_TURN_R: c->right_turn = on; break;
    default:        *button_ptr(c, d->bit) = on; break;
    }
}

static const SwitchDef *find_switch(const char *name) {
    for (int i = 0; i < N_SWITCHES; i++)
        if (strcmp(switches[i].name, name) == 0) return &switches[i];
    return NULL;
}

/* ── Sensors to pins ─────────────────────────────────────────────────────── *
 * The A/D scaling is the program's: it shows ch1 counts as mph (0-160),
 * ch0 counts / 2 as hundreds of rpm, fills the fuel bar from ch2 (low fuel
 * under 20h) and the temperature bar from ch3 counts 4Bh-8Bh, and flags
 * the battery when ch7 reads under 64h. */

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static void update_adc_inputs(LagondaState *s) {
    const LagondaCar *c = &s->car;
    s->adc.in[0] = clampd(c->rpm / 50.0, 0, 255);
    s->adc.in[1] = clampd(c->speed_mph, 0, 255);
    s->adc.in[2] = clampd(c->fuel_pct * 2.55, 0, 255);
    s->adc.in[3] = clampd(c->temp, 0, 255);
    for (int i = 0; i < 3; i++) s->adc.in[4 + i] = clampd(c->adc_extra[i], 0, 255);
    s->adc.in[7] = clampd(c->battery, 0, 255);
}

static bool flasher_on(const LagondaState *s) {
    double t = (double)s->cpu.cycles / s->clock_hz;
    return fmod(t * FLASHER_HZ, 1.0) < 0.5;
}

/* 8255 port C: PC0-PC4 the gear selector (P on PC4, R PC3, D PC2, 2 PC1,
 * 1 PC0; N closes none), PC5 minute, PC6 hour, PC7 mph/km - all active low. */
static uint8_t ppi_pins(void *ud, int port) {
    LagondaState *s = ud;
    const LagondaCar *c = &s->car;
    switch (port) {
    case 0: return c->warn_a;
    case 1: return c->warn_b;
    default: {
        static const uint8_t gear_pin[6] = { 0x10, 0x08, 0x00, 0x04, 0x02, 0x01 };
        uint8_t v = (uint8_t)(0xFFu & ~gear_pin[c->gear]);
        if (c->btn_minute) v &= (uint8_t)~0x20u;
        if (c->btn_hour)   v &= (uint8_t)~0x40u;
        if (c->btn_units)  v &= (uint8_t)~0x80u;
        return v;
    }
    }
}

static uint8_t port_ff(const LagondaState *s) {
    const LagondaCar *c = &s->car;
    uint8_t v = c->lamps & 0xB8u;
    if (flasher_on(s)) {
        if (c->left_turn)  v |= 0x04;
        if (c->right_turn) v |= 0x40;
    }
    if (!c->btn_cancel) v |= 0x02;
    if (!c->btn_check)  v |= 0x01;
    return v;
}

/* ── Speech card ─────────────────────────────────────────────────────────── */

static void speech_command(LagondaState *s, uint8_t v) {
    LagondaSpeech *sp = &s->speech;
    sp->latch = v;
    sp->counter = 0;
    unsigned sock = v >> 4;
    sp->playing = (v & 1u) && sock >= 1 && sock <= LAG_SPEECH_N;
    msm5205_set_reset(&sp->msm, true);
    msm5205_set_reset(&sp->msm, !sp->playing);
}

static void speech_vck(LagondaState *s) {
    LagondaSpeech *sp = &s->speech;
    if (!sp->playing) return;
    unsigned sock = (sp->latch >> 4) - 1u;
    unsigned a = ((sp->latch & 2u) ? 0x800u : 0u) | (sp->counter >> 1);
    uint8_t byte = s->speech_fitted[sock] ? s->speech_rom[sock][a] : 0xFF;   /* empty socket: pull-ups */
    msm5205_vck(&sp->msm, (sp->counter & 1u) ? (byte & 0x0Fu) : (uint8_t)(byte >> 4));
    if (++sp->counter == 4096) {
        sp->playing = false;
        msm5205_set_reset(&sp->msm, true);
    }
}

/* DAC -> the card's op-amp low-pass -> LM384 -> speaker.  The filter
 * components aren't known: two poles at 1.8 kHz smooth the 4 kHz steps. */
static void audio_sample(LagondaState *s) {
    double x = msm5205_output(&s->speech.msm);
    double a = 1.0 - exp(-2.0 * M_PI * 1800.0 / LAG_AUDIO_RATE);
    s->lp1 += a * (x - s->lp1);
    s->lp2 += a * (s->lp1 - s->lp2);
    double y = s->lp2 - s->dc_x + 0.999 * s->dc_y;
    s->dc_x = s->lp2;
    s->dc_y = y;
    double out = clampd(y * 1.5, -1.0, 1.0);
    if (s->audio_dev && s->audio_n < LAG_AUDIO_BUF) s->audio_buf[s->audio_n++] = (float)out;
}

/* ── Bus ─────────────────────────────────────────────────────────────────── */

static uint8_t lag_read(void *ud, uint16_t a) {
    LagondaState *s = ud;
    gemu_monitor_check_read(s->monitor, a);
    switch (a >> 12) {
    case 0: return s->code[a];
    case 1: return s->gfx[a & 0x0FFFu];
    case 2: return s->ram[0][a & 0x3FFu];
    case 3: return s->ram[1][a & 0x3FFu];
    default: return 0xFF;
    }
}

static void lag_write(void *ud, uint16_t a, uint8_t v) {
    LagondaState *s = ud;
    gemu_monitor_check_write(s->monitor, a);
    if ((a >> 12) == 2) s->ram[0][a & 0x3FFu] = v;
    else if ((a >> 12) == 3) s->ram[1][a & 0x3FFu] = v;
}

static uint8_t bcd_byte(uint16_t bcd, int hi) { return (uint8_t)(hi ? bcd >> 8 : bcd); }

static uint8_t lag_in(void *ud, uint16_t port) {
    LagondaState *s = ud;
    uint8_t p = (uint8_t)port;
    if (p < 0x08) return ppi8255_read(&s->ppi, p & 3u);
    if (p < 0x10) return adc0809_read(&s->adc, s->cpu.cycles);
    if (p < 0x20) return bcd_byte(s->trip_bcd, p >= 0x18);
    if (p >= 0x40 && p < 0x60) {
        mm58167_sync(&s->rtc, s->cpu.cycles, s->clock_hz);
        return mm58167_read(&s->rtc, p & 0x1Fu);
    }
    if (p >= 0x80 && p < 0x86) return tms9918_read(&s->vdp[(p - 0x80) >> 1], p & 1);
    if (p == 0xFF) return port_ff(s);
    return 0xFF;
}

static void lag_out(void *ud, uint16_t port, uint8_t v) {
    LagondaState *s = ud;
    uint8_t p = (uint8_t)port;
    if (p < 0x08) ppi8255_write(&s->ppi, p & 3u, v);
    else if (p < 0x10) adc0809_start(&s->adc, p & 7u, s->cpu.cycles, ADC_CLK_HZ, s->clock_hz);
    else if (p >= 0x20 && p < 0x40) s->latch20 = v;
    else if (p >= 0x40 && p < 0x60) {
        mm58167_sync(&s->rtc, s->cpu.cycles, s->clock_hz);
        mm58167_write(&s->rtc, p & 0x1Fu, v);
    }
    else if (p >= 0x80 && p < 0x86) tms9918_write(&s->vdp[(p - 0x80) >> 1], p & 1, v);
    else if (p == 0xFE) speech_command(s, v);
}

/* ── Trip counter ────────────────────────────────────────────────────────── *
 * Two 4518s count the distance pulses (the speed sensor divided down on
 * the ATI card) in tenths of a mile: four BCD digits, 000.0-999.9. */

static uint16_t bcd_inc(uint16_t v) {
    for (int d = 0; d < 16; d += 4) {
        unsigned digit = (v >> d) & 0xFu;
        if (digit < 9) return (uint16_t)(v + (1u << d));
        v = (uint16_t)(v & ~(0xFu << d));
    }
    return v;
}

static void advance_trip(LagondaState *s, double seconds) {
    LagondaCar *c = &s->car;
    double before = c->trip_miles;
    c->trip_miles += c->speed_mph * seconds / 3600.0;
    long pulses = (long)floor(c->trip_miles * 10.0) - (long)floor(before * 10.0);
    while (pulses-- > 0) s->trip_bcd = bcd_inc(s->trip_bcd);
}

/* ── Stepping ────────────────────────────────────────────────────────────── */

/* Runs one instruction and everything clocked alongside it.  Returns true
 * when the VDPs finished a frame. */
static bool lag_step(LagondaState *s) {
    z80_step(&s->cpu);
    s->insn_count++;
    double now = (double)s->cpu.cycles;
    bool frame = false;
    while (now >= s->next_vck) { speech_vck(s); s->next_vck += s->vck_cycles; }
    while (now >= s->next_sample) { audio_sample(s); s->next_sample += s->sample_cycles; }
    while (now >= s->next_line) {
        for (int i = 0; i < LAG_VDP_N; i++)
            if (tms9918_scanline(&s->vdp[i]) && i == 0) frame = true;
        s->next_line += s->line_cycles;
    }
    if (frame) {
        advance_trip(s, (double)(s->cpu.cycles - s->frame_cycle) / s->clock_hz);
        s->frame_cycle = s->cpu.cycles;
    }
    return frame;
}

static void power_on(LagondaState *s) {
    z80_reset(&s->cpu);
    s->cpu.int_line = s->cpu.nmi_line = false;
    memset(s->ram, 0, sizeof(s->ram));
    for (int i = 0; i < LAG_VDP_N; i++) tms9918_reset(&s->vdp[i]);
    ppi8255_reset(&s->ppi);
    adc0809_init(&s->adc);
    update_adc_inputs(s);
    msm5205_init(&s->speech.msm);
    speech_command(s, 0x00);
    s->latch20 = 0;
}

/* ── Monitor ─────────────────────────────────────────────────────────────── */

static bool lag_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    LagondaState *s = ud;
    if (addr > 0xFFFF) return false;
    uint16_t a = (uint16_t)addr;
    switch (a >> 12) {
    case 0: *val = s->code[a]; return true;
    case 1: *val = s->gfx[a & 0x0FFFu]; return true;
    case 2: *val = s->ram[0][a & 0x3FFu]; return true;
    case 3: *val = s->ram[1][a & 0x3FFu]; return true;
    default: return false;
    }
}

static bool lag_mon_write(void *ud, uint64_t addr, uint8_t val) {
    LagondaState *s = ud;
    if (addr > 0xFFFF) return false;
    uint16_t a = (uint16_t)addr;
    if ((a >> 12) == 2) { s->ram[0][a & 0x3FFu] = val; return true; }
    if ((a >> 12) == 3) { s->ram[1][a & 0x3FFu] = val; return true; }
    return false;
}

static void lag_mon_cpu_state(void *ud, char *buf, size_t len) {
    z80_format_state(&((const LagondaState *)ud)->cpu, buf, len);
}

static uint8_t dis_peek(void *ud, uint16_t a) {
    uint8_t v = 0xFF;
    lag_mon_read(ud, a, &v);
    return v;
}

static GemuMonCmd cmd_disas(GemuMonitor *mon, const char *args, void *ud) {
    LagondaState *s = ud;
    char a1[64] = "", a2[32] = "";
    sscanf(args, "%63s %31s", a1, a2);
    uint64_t addr = s->cpu.pc;
    if (a1[0] && !gemu_monitor_parse_addr(mon, a1, &addr)) {
        gemu_monitor_printf(mon, "disas: bad address '%s'\n", a1);
        return GEMU_MON_NONE;
    }
    int count = a2[0] ? atoi(a2) : 16;
    if (count <= 0 || count > 1000) count = 16;
    uint16_t pc = (uint16_t)addr;
    for (int i = 0; i < count; i++) {
        const char *label = gemu_monitor_symbol_name(mon, pc);
        if (label) gemu_monitor_printf(mon, "        %s:\n", label);
        char text[64], bytes[16] = "";
        int len = z80_disasm(dis_peek, s, pc, text, sizeof(text));
        for (int b = 0; b < len; b++)
            snprintf(bytes + strlen(bytes), sizeof(bytes) - strlen(bytes), "%02X ",
                     dis_peek(s, (uint16_t)(pc + b)));
        gemu_monitor_printf(mon, "%s%04X  %-12s %s\n", pc == s->cpu.pc ? ">" : " ", pc, bytes, text);
        pc = (uint16_t)(pc + len);
    }
    return GEMU_MON_NONE;
}

static GemuMonCmd cmd_vdp(GemuMonitor *mon, const char *args, void *ud) {
    LagondaState *s = ud;
    static const char *const names[LAG_VDP_N] = { "centre display", "speedometer", "tachometer" };
    int only = -1;
    if (args[0] && sscanf(args, "%d", &only) == 1 && (only < 0 || only >= LAG_VDP_N)) {
        gemu_monitor_printf(mon, "vdp: 0-%d\n", LAG_VDP_N - 1);
        return GEMU_MON_NONE;
    }
    for (int i = 0; i < LAG_VDP_N; i++) {
        if (only >= 0 && i != only) continue;
        char buf[512];
        tms9918_describe(&s->vdp[i], buf, sizeof(buf));
        gemu_monitor_printf(mon, "VDP %d (%s, ports %02X/%02X): %s", i, names[i], 0x80 + 2 * i, 0x81 + 2 * i, buf);
    }
    return GEMU_MON_NONE;
}

static const char *const gear_names[6] = { "P", "R", "N", "D", "2", "1" };

static void sensor_list(GemuMonitor *mon, LagondaState *s) {
    const LagondaCar *c = &s->car;
    gemu_monitor_printf(mon,
        "Analog (A/D counts in brackets):\n"
        "  speed    %6.1f mph   [ch1 %3.0f]\n"
        "  rpm      %6.0f       [ch0 %3.0f]\n"
        "  fuel     %6.1f %%     [ch2 %3.0f]  low-fuel warning under 12.5%%\n"
        "  temp     %6.0f       [ch3]  gauge spans 75-139\n"
        "  battery  %6.0f       [ch7]  warning under 100\n"
        "  adc4-6   %.0f %.0f %.0f (unused)\n"
        "  gear     %s\n"
        "  trip     %.2f miles (counter %X%X%X.%X)\n"
        "Speech card: command %02Xh, %s\n"
        "Switches:\n",
        c->speed_mph, s->adc.in[1], c->rpm, s->adc.in[0], c->fuel_pct, s->adc.in[2],
        c->temp, c->battery, c->adc_extra[0], c->adc_extra[1], c->adc_extra[2],
        gear_names[c->gear], c->trip_miles,
        s->trip_bcd >> 12, (s->trip_bcd >> 8) & 15u, (s->trip_bcd >> 4) & 15u, s->trip_bcd & 15u,
        s->speech.latch, s->speech.playing ? "playing" : "idle");
    for (int i = 0; i < N_SWITCHES; i++)
        gemu_monitor_printf(mon, "  %-14s %-3s  %s\n", switches[i].name,
                            switch_get(s, &switches[i]) ? "on" : "off", switches[i].what);
}

static bool parse_on_off(const char *v, bool *out) {
    if (!strcmp(v, "on") || !strcmp(v, "1")) { *out = true; return true; }
    if (!strcmp(v, "off") || !strcmp(v, "0")) { *out = false; return true; }
    return false;
}

static GemuMonCmd cmd_sensor(GemuMonitor *mon, const char *args, void *ud) {
    LagondaState *s = ud;
    LagondaCar *c = &s->car;
    char name[32] = "", val[32] = "";
    int n = sscanf(args, "%31s %31s", name, val);
    if (n <= 0 || !strcmp(name, "list")) { sensor_list(mon, s); return GEMU_MON_NONE; }
    if (n < 2) {
        gemu_monitor_printf(mon, "usage: sensor [list | <name> <value>]\n");
        return GEMU_MON_NONE;
    }
    char *end;
    double d = strtod(val, &end);
    bool num = *end == '\0';
    struct { const char *name; double *v; double lo, hi; } analog[] = {
        { "speed", &c->speed_mph, 0, 255 }, { "rpm", &c->rpm, 0, 12750 },
        { "fuel", &c->fuel_pct, 0, 100 },  { "temp", &c->temp, 0, 255 },
        { "battery", &c->battery, 0, 255 }, { "adc4", &c->adc_extra[0], 0, 255 },
        { "adc5", &c->adc_extra[1], 0, 255 }, { "adc6", &c->adc_extra[2], 0, 255 },
    };
    for (size_t i = 0; i < sizeof analog / sizeof *analog; i++) {
        if (strcmp(name, analog[i].name)) continue;
        if (!num) { gemu_monitor_printf(mon, "sensor: %s needs a number\n", name); return GEMU_MON_NONE; }
        *analog[i].v = clampd(d, analog[i].lo, analog[i].hi);
        update_adc_inputs(s);
        return GEMU_MON_NONE;
    }
    if (!strcmp(name, "gear")) {
        for (int g = 0; g < 6; g++)
            if (!strcasecmp(val, gear_names[g])) { c->gear = g; return GEMU_MON_NONE; }
        gemu_monitor_printf(mon, "sensor: gear is one of P R N D 2 1\n");
        return GEMU_MON_NONE;
    }
    if (!strcmp(name, "trip")) {
        if (!num || d < 0) { gemu_monitor_printf(mon, "sensor: trip needs miles\n"); return GEMU_MON_NONE; }
        /* preset the counter: what the 4518s would hold after that distance */
        long tenths = (long)floor(d * 10.0) % 10000;
        s->trip_bcd = 0;
        for (int sh = 0; sh < 16; sh += 4, tenths /= 10) s->trip_bcd |= (uint16_t)((tenths % 10) << sh);
        c->trip_miles = d;
        return GEMU_MON_NONE;
    }
    const SwitchDef *sw = find_switch(name);
    bool on;
    if (!sw) { gemu_monitor_printf(mon, "sensor: unknown '%s' (sensor list)\n", name); return GEMU_MON_NONE; }
    if (!parse_on_off(val, &on)) { gemu_monitor_printf(mon, "sensor: %s is on or off\n", name); return GEMU_MON_NONE; }
    switch_set(s, sw, on);
    return GEMU_MON_NONE;
}

/* sendkey <action>[-<action>...] [frames]: hold keyboard actions. */
static GemuMonCmd cmd_sendkey(GemuMonitor *mon, const char *args, void *ud) {
    LagondaState *s = ud;
    char keys[256] = "";
    int frames = 10;
    if (sscanf(args, "%255s %d", keys, &frames) < 1) {
        gemu_monitor_printf(mon, "usage: sendkey <action>[-<action>...] [frames]\n");
        return GEMU_MON_NONE;
    }
    uint32_t mask = 0;
    for (char *tok = strtok(keys, "-"); tok; tok = strtok(NULL, "-")) {
        int i;
        for (i = 0; i < ACT_COUNT; i++) if (!strcmp(lagonda_actions[i].name, tok)) break;
        if (i == ACT_COUNT) {
            gemu_monitor_printf(mon, "sendkey: unknown action '%s'; one of:", tok);
            for (i = 0; i < ACT_COUNT; i++) gemu_monitor_printf(mon, " %s", lagonda_actions[i].name);
            gemu_monitor_printf(mon, "\n");
            return GEMU_MON_NONE;
        }
        mask |= lagonda_actions[i].bit;
    }
    s->inject_mask = mask;
    s->inject_frames = frames > 0 ? frames : 1;
    return GEMU_MON_NONE;
}

static bool lag_screendump(void *ud, const char *path) {
    LagondaState *s = ud;
    return gemu_screendump_argb(path, s->panel_fb, PANEL_W, PANEL_H);
}

/* ── Input ───────────────────────────────────────────────────────────────── */

static void handle_input(LagondaState *s, double dt) {
    uint32_t h = s->held;
    if (s->inject_frames > 0) { h |= s->inject_mask; s->inject_frames--; }
    uint32_t pressed = h & ~s->prev_held, released = s->prev_held & ~h;
    s->prev_held = h;
    LagondaCar *c = &s->car;
#define HELD(a) (h & GEMU_ACTION(a))
#define PRESSED(a) (pressed & GEMU_ACTION(a))
    if (HELD(ACT_SPEED_UP))   c->speed_mph = clampd(c->speed_mph + 40.0 * dt, 0, 170);
    if (HELD(ACT_SPEED_DOWN)) c->speed_mph = clampd(c->speed_mph - 60.0 * dt, 0, 170);
    if (HELD(ACT_RPM_UP))     c->rpm = clampd(c->rpm + 3000.0 * dt, 0, 7500);
    if (HELD(ACT_RPM_DOWN))   c->rpm = clampd(c->rpm - 3000.0 * dt, 0, 7500);
    if (HELD(ACT_FUEL_UP))    c->fuel_pct = clampd(c->fuel_pct + 25.0 * dt, 0, 100);
    if (HELD(ACT_FUEL_DOWN))  c->fuel_pct = clampd(c->fuel_pct - 25.0 * dt, 0, 100);
    if (HELD(ACT_TEMP_UP))    c->temp = clampd(c->temp + 30.0 * dt, 0, 255);
    if (HELD(ACT_TEMP_DOWN))  c->temp = clampd(c->temp - 30.0 * dt, 0, 255);
    if (PRESSED(ACT_GEAR_UP) && c->gear > 0) c->gear--;
    if (PRESSED(ACT_GEAR_DOWN) && c->gear < 5) c->gear++;

    /* The indicator stalk and the buttons follow the keys; the rest toggle. */
    static const struct { int act; const char *sw; bool momentary; } keymap[] = {
        { ACT_LEFT_TURN, "left_turn", true },   { ACT_RIGHT_TURN, "right_turn", true },
        { ACT_HOUR, "hour", true },             { ACT_MINUTE, "minute", true },
        { ACT_UNITS, "units", true },           { ACT_CHECK, "check", true },
        { ACT_CANCEL, "cancel", true },
        { ACT_MAIN_BEAM, "main_beam", false },  { ACT_DIPPED_BEAM, "dipped_beam", false },
        { ACT_LAMP5, "lamp5", false },          { ACT_SIDELIGHTS, "sidelights", false },
        { ACT_REAR_WINDOW, "rear_window", false },
        { ACT_OIL_PRESSURE, "oil_pressure", false }, { ACT_WATER_TEMP, "water_temp", false },
        { ACT_OIL_TEMP, "oil_temp", false },    { ACT_BRAKE_FLUID, "brake_fluid", false },
        { ACT_BULB, "bulb_failure", false },    { ACT_ICE, "ice", false },
        { ACT_PARKING_BRAKE, "parking_brake", false }, { ACT_SEAT_BELTS, "seat_belts", false },
        { ACT_BOOT, "boot", false },
    };
    for (size_t i = 0; i < sizeof keymap / sizeof *keymap; i++) {
        const SwitchDef *sw = find_switch(keymap[i].sw);
        uint32_t bit = GEMU_ACTION(keymap[i].act);
        if (keymap[i].momentary) {
            if (pressed & bit) switch_set(s, sw, true);
            if (released & bit) switch_set(s, sw, false);
        } else if (pressed & bit) {
            switch_set(s, sw, !switch_get(s, sw));
        }
    }
    /* Trip reset: the button resets the 4518s directly. */
    if (HELD(ACT_TRIP_RESET)) { s->trip_bcd = 0; c->trip_miles = 0; }
#undef HELD
#undef PRESSED
    update_adc_inputs(s);
}

/* ── Display ─────────────────────────────────────────────────────────────── */

static void build_panel(LagondaState *s) {
    for (int y = 0; y < PANEL_H; y++) {
        uint32_t *o = s->panel_fb + y * PANEL_W;
        for (int x = 0; x < PANEL_W; x++) o[x] = 0xFF202020;
        for (int i = 0; i < LAG_VDP_N; i++)
            memcpy(o + i * (TMS9918_FB_W + PANEL_GAP),
                   s->vdp[panel_order[i]].fb + y * TMS9918_FB_W, TMS9918_FB_W * sizeof(uint32_t));
    }
}

static void present(LagondaState *s) {
    if (!s->display) return;
    build_panel(s);
    if (gemu_display_head_shown(s->display, 0))
        gemu_display_render_head(s->display, 0, s->panel_fb, PANEL_W, PANEL_H);
    for (int i = 0; i < LAG_VDP_N; i++)
        if (gemu_display_head_shown(s->display, 1 + i))
            gemu_display_render_head(s->display, 1 + i, s->vdp[panel_order[i]].fb,
                                     TMS9918_FB_W, TMS9918_FB_H);
}

/* ── Setup ───────────────────────────────────────────────────────────────── */

static bool load_rom(const char *path, uint8_t *dst, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "lagonda: cannot open '%s'\n", path); return false; }
    size_t n = fread(dst, 1, size, f);
    fclose(f);
    if (n != size) { fprintf(stderr, "lagonda: '%s' is not a 2732 image (%zu bytes)\n", path, n); return false; }
    return true;
}

static bool load_roms(LagondaState *s, const Z80Config *cfg) {
    bool have_code = false, have_gfx = false;
    memset(s->gfx, 0xFF, sizeof(s->gfx));
    for (int i = 0; i < cfg->n_roms; i++) {
        const Z80RomLoad *r = &cfg->roms[i];
        if (!r->region) {
            fprintf(stderr, "lagonda: '%s': use -rom DIR so the ROMs are identified by checksum\n", r->path);
            return false;
        }
        if (!strcmp(r->region, "code")) {
            if (!load_rom(r->path, s->code, LAG_ROM_SIZE)) return false;
            have_code = true;
        } else if (!strcmp(r->region, "graphics")) {
            if (!load_rom(r->path, s->gfx, LAG_ROM_SIZE)) return false;
            have_gfx = true;
        } else if (!strncmp(r->region, "speech", 6)) {
            int n = atoi(r->region + 6) - 1;
            if (n < 0 || n >= LAG_SPEECH_N) continue;
            if (!load_rom(r->path, s->speech_rom[n], LAG_ROM_SIZE)) return false;
            s->speech_fitted[n] = true;
        }
    }
    if (!have_code || !have_gfx) {
        fprintf(stderr, "lagonda: the CPU card's program and graphics EPROMs are required\n");
        return false;
    }
    for (int i = 0; i < 5; i++)
        if (!s->speech_fitted[i])
            fprintf(stderr, "lagonda: speech EPROM %c missing - that socket reads as empty\n", 'A' + i);
    return true;
}

static bool audio_open(LagondaState *s) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) return false;
    SDL_AudioSpec want = {
        .freq = LAG_AUDIO_RATE, .format = AUDIO_F32SYS, .channels = 1, .samples = 512,
    }, have;
    s->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!s->audio_dev) { SDL_QuitSubSystem(SDL_INIT_AUDIO); return false; }
    SDL_PauseAudioDevice(s->audio_dev, 0);
    return true;
}

LagondaState *lagonda_create(const Z80Config *cfg) {
    LagondaState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;
    s->clock_hz = cfg->clock_hz;
    s->panel_fb = calloc((size_t)PANEL_W * PANEL_H, sizeof(uint32_t));
    if (!s->panel_fb || !load_roms(s, cfg)) { free(s->panel_fb); free(s); return NULL; }

    z80_init(&s->cpu);
    s->cpu.read = lag_read;  s->cpu.write = lag_write;
    s->cpu.in = lag_in;      s->cpu.out = lag_out;
    s->cpu.ud = s;
    s->ppi.read_pins = ppi_pins;
    s->ppi.ud = s;

    s->line_cycles   = s->clock_hz * TMS9918_CLOCKS_LINE / VDP_HZ;
    s->vck_cycles    = s->clock_hz / VCK_HZ;
    s->sample_cycles = s->clock_hz / LAG_AUDIO_RATE;
    s->next_line = s->line_cycles;
    s->next_vck = s->vck_cycles;
    s->next_sample = s->sample_cycles;

    /* The car as it sits with the ignition on: parked, engine off. */
    LagondaCar *c = &s->car;
    c->fuel_pct = 75;
    c->temp = 0x60;
    c->battery = 0xA0;
    c->gear = LAG_GEAR_P;
    c->warn_a = 0xFF;
    c->warn_b = 0xFE;   /* rear window heater off */

    /* The RTC's battery kept it running: start it at the host's time. */
    mm58167_init(&s->rtc);
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    mm58167_set_time(&s->rtc, tm.tm_mon + 1, tm.tm_mday, tm.tm_wday + 1, tm.tm_hour, tm.tm_min, tm.tm_sec);

    power_on(s);

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = lag_mon_read, .write = lag_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = false });
    gemu_monitor_set_cpu_state_cb(s->monitor, lag_mon_cpu_state, s);
    gemu_monitor_set_screendump_cb(s->monitor, lag_screendump, s);
    gemu_monitor_register_rom(s->monitor, 0x0000, LAG_ROM_SIZE, "program EPROM");
    gemu_monitor_register_rom(s->monitor, 0x1000, LAG_ROM_SIZE, "graphics EPROM");
    gemu_monitor_bind(s->monitor, "disas", cmd_disas, s);
    gemu_monitor_bind(s->monitor, "vdp", cmd_vdp, s);
    gemu_monitor_bind(s->monitor, "sensor", cmd_sensor, s);
    gemu_monitor_bind(s->monitor, "sendkey", cmd_sendkey, s);

    if (gemu_display_wanted(cfg->display_type)) {
        GemuDisplayConfig dc = {
            .fb_width = PANEL_W, .fb_height = PANEL_H,
            .scale = cfg->display_scale,
            .actions = lagonda_actions, .n_actions = ACT_COUNT,
            .ini_section = "lagonda",
            .monitor = s->monitor,
        };
        s->display = gemu_display_create(cfg->display_type, &dc);
        if (!s->display) { lagonda_destroy(s); return NULL; }
        GemuDisplayHead heads[1 + LAG_VDP_N] = {
            { "Instrument panel", PANEL_W, PANEL_H, 0, 0 },
            { "Tachometer (VDP 2)", TMS9918_FB_W, TMS9918_FB_H, 4, 3 },
            { "Centre display (VDP 0)", TMS9918_FB_W, TMS9918_FB_H, 4, 3 },
            { "Speedometer (VDP 1)", TMS9918_FB_W, TMS9918_FB_H, 4, 3 },
        };
        gemu_display_set_heads(s->display, heads, 1 + LAG_VDP_N);
        if (!audio_open(s)) fprintf(stderr, "lagonda: no audio (continuing silently)\n");
    }
    return s;
}

void lagonda_destroy(LagondaState *s) {
    if (!s) return;
    if (s->audio_dev) { SDL_CloseAudioDevice(s->audio_dev); SDL_QuitSubSystem(SDL_INIT_AUDIO); }
    gemu_display_destroy(s->display);
    gemu_monitor_destroy(s->monitor);
    free(s->panel_fb);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

#define AUDIO_QUEUE_MAX (3u * (LAG_AUDIO_RATE / 60u) * (unsigned)sizeof(float))

void lagonda_run(LagondaState *s, const Z80Config *cfg) {
    gemu_monitor_start(s->monitor);
    double carry_ms = 0;
    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();
        s->held = 0;
        if (s->display) {
            s->held = gemu_display_poll(s->display);
            if (gemu_display_should_quit(s->display)) break;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                power_on(s);
            }
        }
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { quit = true; break; }
            } else if (cmd == GEMU_MON_RESET) {
                power_on(s);
            } else if (cmd == GEMU_MON_STEP) {
                for (uint32_t n = gemu_monitor_step_count(s->monitor); n; n--) lag_step(s);
            }
        }
        if (quit) break;
        bool paused = gemu_monitor_is_paused(s->monitor);
        if (s->display) gemu_display_set_paused(s->display, paused);

        handle_input(s, 1.0 / 59.94);

        uint64_t start = s->cpu.cycles;
        if (!paused) {
            for (;;) {
                if (gemu_monitor_check_exec(s->monitor, s->cpu.pc)) break;
                bool frame = lag_step(s);
                if (gemu_monitor_is_paused(s->monitor) || frame) break;
            }
            if (s->audio_dev && s->audio_n)
                SDL_QueueAudio(s->audio_dev, s->audio_buf, (Uint32)(s->audio_n * sizeof(float)));
            s->audio_n = 0;
        }
        present(s);

        if (s->audio_dev && !paused) {
            while (SDL_GetQueuedAudioSize(s->audio_dev) > AUDIO_QUEUE_MAX) SDL_Delay(1);
        } else {
            uint64_t ran = s->cpu.cycles - start;
            double ms = ran ? ran * 1000.0 / s->clock_hz : 16.7;
            carry_ms += ms - (SDL_GetTicks() - t0);
            if (carry_ms > 0) {
                Uint32 d = (Uint32)carry_ms;
                SDL_Delay(d);
                carry_ms -= d;
            } else if (carry_ms < -100) {
                carry_ms = 0;
            }
        }
    }
    gemu_monitor_stop(s->monitor);
    printf("lagonda: %llu T-states, %llu instructions\n",
           (unsigned long long)s->cpu.cycles, (unsigned long long)s->insn_count);
}
