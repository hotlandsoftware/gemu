/* Intel 460GX IFB power management (SSDM 11.2.6-11.2.9 and chapter 16): the
 * APM ports, the ACPI and SMI register block and the GPIO block.
 *
 * Windows, all in PCI function 0:
 *   ACPI block  64 bytes at ACPIBR (40h[15:6]), decoded while 44h bit 0 is set;
 *   GPIO block  64 bytes at GPIOBA (D0h[15:6]), decoded while D4h bit 0 is set;
 *   APM         ports B2h/B3h, always decoded.
 *
 * Power wells (16.2.1). Three lifetimes are modelled:
 *   RTC well     RTC_EN, RI_EN, USB1_EN and AFTERG3 survive every reset.
 *   Resume well  the bits documented as unaffected by a CF9 hard reset
 *                (WAK_STS, PWRBTNOR_STS, RTC_STS, PWRBTN_STS, PWR_FAIL,
 *                RI_STS, USB1_STS, GPE1_EN and SLP_TYP). RSMRST# clears them.
 *   Core         everything else returns to its default on either reset.
 *
 * Time comes from the board clock. Timers count in 8254 ticks (1.193182 MHz);
 * the PM timer runs at exactly three times that, 3.579545 MHz. */
#include "i460gx_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum {
    PIT_HZ = 1193182,
    SWSMI_TICKS = 152727,           /* 128 ms (11.2.8.1 bit 7) */
    MIN_TICKS = 60 * PIT_HZ,        /* the 1 minute timer (bit 6) */
    OVERRIDE_TICKS = 4 * PIT_HZ,    /* power button held 4 s (11.2.7.1 bit 11) */
    PM_TICKS_PER_PIT_TICK = 3,
    /* Do not bother the timers for sub-0.1 ms polls. */
    POLL_DIVISOR = 10000,

    ACPI_BLOCK_SIZE = 64, GPIO_BLOCK_SIZE = 64,
    PORT_APMC = 0xb2, PORT_APMS = 0xb3,

    /* IFB function 0 configuration. */
    CFG_ACPIBR = 0x40, CFG_ACPI_CNTL = 0x44, CFG_SCI_ROUTE = 0x45,
    CFG_GPIOBA = 0xd0, CFG_GPIO_CNTL = 0xd4,
    CFG_BLOCK_ENABLE = 0x01, CFG_BLOCK_BASE_MASK = 0xffc0,
};

/* PM1_STS 00h (11.2.7.1): every defined bit is set by hardware and cleared by
 * writing 1. PM1_EN (02h) puts each enable at the same position. */
enum {
    PM1_TMROF = 0x0001, PM1_GBL = 0x0020, PM1_PWRBTN = 0x0100,
    PM1_RTC = 0x0400, PM1_PWRBTNOR = 0x0800, PM1_WAK = 0x8000,
    PM1_STS_WRITE_CLEAR = 0x8d21,
    PM1_STS_RESUME = PM1_WAK | PM1_PWRBTNOR | PM1_RTC | PM1_PWRBTN,
    PM1_EN_MASK = 0x0521,           /* RTC_EN, PWRBTN_EN, GBL_EN, TMROF_EN */
    PM1_EN_RTC_WELL = PM1_RTC,
};

/* PM1_CNT 04h (11.2.7.3). SLP_EN is write-only and never stored. SLP_TYP
 * keeps whatever was written, reserved encodings included. */
enum {
    CNT_SCI_EN = 0x0001, CNT_GBL_RLS = 0x0004, CNT_SLP_TYP = 0x1c00,
    CNT_SLP_EN = 0x2000, CNT_STORED = 0x1c05,
};

/* GPE0_STS 0Ch (11.2.7.5) and GPE0_EN 0Eh (11.2.7.6). */
enum {
    GPE0_THRM = 0x0001, GPE0_NMI = 0x0002, GPE0_THRMOR = 0x0080,
    GPE0_USB1 = 0x0100, GPE0_RI = 0x0400, GPE0_PWR_FAIL = 0x0800,
    GPE0_STS_WRITE_CLEAR = 0x0d83,
    GPE0_STS_RESUME = GPE0_PWR_FAIL | GPE0_RI | GPE0_USB1,
    GPE0_AFTERG3 = 0x1000,          /* enable register only */
    GPE0_EN_MASK = 0x1503,
    GPE0_EN_RTC_WELL = GPE0_AFTERG3 | GPE0_RI | GPE0_USB1,
    /* The status bits that have an enable. */
    GPE0_EVENTS = GPE0_RI | GPE0_USB1 | GPE0_NMI | GPE0_THRM,
};

/* GPE1_STS 16h and GPE1_EN 18h (11.2.7.7-8): one bit per dedicated GPIO. */
enum { GPE1_MASK = 0x01ff };

/* Global Control and Enable 1Ah (11.2.8.1). BIOS_RLS reads 0 and is not
 * stored. */
enum {
    GC_SMI_EN = 0x0001, GC_BIOS_RLS = 0x0002, GC_THRM_POL = 0x0004,
    GC_EOS = 0x0008, GC_BIOS_EN = 0x0010, GC_1MIN_EN = 0x0040,
    GC_SWSMI_EN = 0x0080, GC_EXTSMI_EN = 0x0100, GC_ECC_EN = 0x0200,
    GC_APMC_EN = 0x0400, GC_ACPITMR_EN = 0x1000, GC_STORED = 0x17dd,
};

/* Global Status 1Ch (11.2.8.2). PM1_REG, GP_REG and LEGACY_USB are ORs
 * computed on read; the rest are hardware-set and write-one-to-clear. */
enum {
    GS_BIOS = 0x0001, GS_LEGACY_USB = 0x0002, GS_APM = 0x0008,
    GS_PM1_REG = 0x0010, GS_GP_REG = 0x0020, GS_1MIN = 0x0040,
    GS_SWSMI = 0x0080, GS_EXTSMI = 0x0100, GS_BIOSWEN = 0x0200,
    GS_ECC = 0x0400, GS_WRITE_CLEAR = 0x07c9,
};

enum {
    GP_ALL = I460GX_GPIO_ALL, GP_DEDICATED = I460GX_GPIO_DEDICATED,
    GP_PULLUP_DEFAULT = 0x03ff, GP_PULLUP_MASK = 0x03ff,
};

/* SMI# causes, SSDM Table 16-2 and 11.2.8.2. */
enum {
    SMI_BIOS, SMI_LEGACY_USB, SMI_APM, SMI_TMROF, SMI_PWRBTN, SMI_THRM,
    SMI_1MIN, SMI_SWSMI, SMI_EXTSMI, SMI_ECC, SMI_BIOSWEN, SMI_CAUSES
};

static const char *const smi_cause_name[SMI_CAUSES] = {
    [SMI_BIOS] = "BIOS_STS", [SMI_LEGACY_USB] = "LEGACY_USB_STS",
    [SMI_APM] = "APM_STS", [SMI_TMROF] = "TMROF_STS",
    [SMI_PWRBTN] = "PWRBTN_STS", [SMI_THRM] = "THRM_STS",
    [SMI_1MIN] = "1MIN_STS", [SMI_SWSMI] = "SWSMI_TMR_STS",
    [SMI_EXTSMI] = "EXTSMI_STS", [SMI_ECC] = "ECC_STS",
    [SMI_BIOSWEN] = "BIOSWEN_STS",
};

static const char *state_name(unsigned state) {
    static const char *const names[] = {
        "ON", "S1 (STPCLK#)", "S1 (STPCLK# and SLP#)", "S3", "S4/S5",
    };
    return state < sizeof(names) / sizeof(names[0]) ? names[state] : "reserved";
}

static void debug(const I460GX *s, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void debug(const I460GX *s, const char *format, ...) {
    if (!s->config.pm_debug)
        return;
    va_list args;
    va_start(args, format);
    fputs("i460gx pm: ", stderr);
    vfprintf(stderr, format, args);
    va_end(args);
}

static void set_bits16(uint16_t *reg, uint32_t value, uint32_t mask) {
    *reg = (uint16_t)((*reg & ~mask) | (value & mask));
}

static void set_bits32(uint32_t *reg, uint32_t value, uint32_t mask) {
    *reg = (*reg & ~mask) | (value & mask);
}

/* The core well is powered in ON and both S1 states. S3-S5 assert SUSB, which
 * removes main power (16.2.6). */
static bool core_powered(const I460GXPm *p) {
    return p->state <= I460GX_PM_S1_SLP;
}

/* ---- GPIO ---------------------------------------------------------------- */

/* The level each pin presents to its input buffer: the board's drive, else
 * the internal pull-up (bits 8:0), else 0. */
static uint32_t gpio_pins_in(const I460GXPm *p) {
    uint32_t pulled = p->gp_pullup & GP_DEDICATED;
    return ((p->ext_level & p->ext_mask) | (pulled & ~p->ext_mask)) & GP_ALL;
}

/* 11.2.9.6: an input's data bit is inverted before entering the register.
 * Invert exists for the dedicated pins only. */
static uint32_t gpio_in_value(const I460GXPm *p) {
    return gpio_pins_in(p) ^ (p->gp_inv & GP_DEDICATED);
}

/* GP Data as read (11.2.9.2): an output returns its latch; an input returns
 * the pin, or with GP Pulse set the edge latch (11.2.9.8). */
static uint32_t gpio_data_read(const I460GXPm *p) {
    uint32_t in = gpio_in_value(p);
    uint32_t value = (in & ~p->gp_pulse) | (p->pulse_latch & p->gp_pulse);
    return ((p->gp_data & p->gp_out) | (value & ~p->gp_out)) & GP_ALL;
}

/* Input-side consequences of the pin and register state: GP Pulse edge
 * latches, GPE1 GPIO_STS (11.2.7.7) and EXTSMI_STS (11.2.8.2 bit 8). Both
 * statuses are level-set, so clearing one while the input is still 1 is
 * undone at once. */
static void gpio_eval(I460GXPm *p) {
    uint32_t inputs = ~p->gp_out & GP_ALL;
    uint32_t level = gpio_in_value(p);
    p->pulse_latch |= level & ~p->in_prev & p->gp_pulse & inputs;
    p->in_prev = level;
    uint32_t high = gpio_data_read(p) & inputs;
    p->gpe1_sts |= (uint16_t)(high & GPE1_MASK);
    if (high & p->gp_smi & GP_DEDICATED)
        p->glb_sts |= GS_EXTSMI;
}

/* ---- THRM# --------------------------------------------------------------- */

/* With THRM_POL low a HIGH on THRM# sets THRM_STS (11.2.8.1 bit 2), on the
 * edge (Tables 16-2 and 16-3). Core-well logic. */
static void thrm_eval(I460GXPm *p) {
    bool polarity = (p->glb_ctl & GC_THRM_POL) != 0;
    bool active = p->thrm_connected && (p->thrm_level != polarity);
    if (active && !p->thrm_prev && core_powered(p))
        p->gpe0_sts |= GPE0_THRM;
    p->thrm_prev = active;
}

/* ---- SCI, SMI#, wake ----------------------------------------------------- */

/* 16.2.3: with SCI_EN set the IFB generates an SCI for each enabled source:
 * TMROF, PWRBTN, GBL, every enabled GPE0 bit and every enabled GPE1 bit.
 * RTC_STS is not a source: RTC_EN enables only a wake event. */
static bool sci_level(const I460GXPm *p) {
    if (!(p->pm1_cnt & CNT_SCI_EN))
        return false;
    return (p->pm1_sts & p->pm1_en & (PM1_TMROF | PM1_GBL | PM1_PWRBTN)) ||
           (p->gpe0_sts & p->gpe0_en & GPE0_EVENTS) ||
           (p->gpe1_sts & p->gpe1_en & GPE1_MASK);
}

/* Every pending, enabled SMI# cause. SMI_EN gates all of them except
 * BIOS_WEN under BLE (11.2.8.1 bit 0). The PM1 and GPE0 events take the SMI#
 * path only while SCI_EN is 0 (11.2.7.3 bit 0); the timer also needs
 * ACPI_TMR_EN. */
static uint32_t smi_causes(const I460GXPm *p) {
    uint16_t control = p->glb_ctl, status = p->glb_sts;
    bool sci_mode = (p->pm1_cnt & CNT_SCI_EN) != 0;
    uint32_t causes = 0;
    if ((status & GS_BIOS) && (control & GC_BIOS_EN))
        causes |= 1u << SMI_BIOS;
    if (p->legacy_usb)
        causes |= 1u << SMI_LEGACY_USB;
    if ((status & GS_APM) && (control & GC_APMC_EN))
        causes |= 1u << SMI_APM;
    if (!sci_mode && (p->pm1_sts & p->pm1_en & PM1_TMROF) &&
        (control & GC_ACPITMR_EN))
        causes |= 1u << SMI_TMROF;
    if (!sci_mode && (p->pm1_sts & p->pm1_en & PM1_PWRBTN))
        causes |= 1u << SMI_PWRBTN;
    if (!sci_mode && (p->gpe0_sts & p->gpe0_en & GPE0_THRM))
        causes |= 1u << SMI_THRM;
    if (status & GS_1MIN)
        causes |= 1u << SMI_1MIN;
    if (status & GS_SWSMI)
        causes |= 1u << SMI_SWSMI;
    if ((status & GS_EXTSMI) && (control & GC_EXTSMI_EN))
        causes |= 1u << SMI_EXTSMI;
    if ((status & GS_ECC) && (control & GC_ECC_EN))
        causes |= 1u << SMI_ECC;
    if (!(control & GC_SMI_EN))
        causes = 0;
    if ((status & GS_BIOSWEN) && p->bioswen_ble)
        causes |= 1u << SMI_BIOSWEN;
    return causes;
}

static void report_smi(I460GX *s, bool asserted) {
    if (s->config.smi)
        s->config.smi(s->config.opaque, asserted);
}

/* The SMI# arbiter (11.2.8.1 bit 3, 16.2.2): SMI# asserts on a pending cause
 * while EOS is 1, and the assertion clears EOS; setting EOS deasserts it. A
 * cause still pending then asserts it again, so the handler must clear every
 * status first. No SMI# from S3-S5. */
static void smi_eval(I460GX *s) {
    I460GXPm *p = &s->pm;
    if (p->smi || !core_powered(p) || !(p->glb_ctl & GC_EOS))
        return;
    uint32_t causes = smi_causes(p);
    if (!causes)
        return;
    p->smi = true;
    p->glb_ctl &= (uint16_t)~GC_EOS;
    if (s->config.pm_debug)
        for (unsigned i = 0; i < SMI_CAUSES; i++)
            if (causes & (1u << i))
                debug(s, "SMI# asserted by %s\n", smi_cause_name[i]);
    report_smi(s, true);
}

/* Wake events: RTC_STS with RTC_EN, every enabled GPE0 and GPE1 bit, and
 * EXTSMI_STS with EXTSMI_EN. The power button wakes unconditionally at the
 * press. */
static bool wake_pending(const I460GXPm *p) {
    return (p->pm1_sts & p->pm1_en & PM1_RTC) ||
           (p->gpe0_sts & p->gpe0_en & GPE0_EVENTS) ||
           (p->gpe1_sts & p->gpe1_en & GPE1_MASK) ||
           ((p->glb_sts & GS_EXTSMI) && (p->glb_ctl & GC_EXTSMI_EN));
}

/* SCI onto the line fn0 45h selects (11.1.10): 000 IRQ9, 011 IRQ8, 100 IRQ0,
 * 101 IRQ10, 110 IRQ11. 001 (the IFB's SCI pin) and 010 (FEMPTY#) leave the
 * IFB on pins whose board wiring is not modelled. */
static void sci_route(I460GX *s) {
    static const int8_t line[8] = { 9, -1, -1, 8, 0, 10, 11, -1 };
    I460GXPm *p = &s->pm;
    int8_t irq = line[s->ifb_cfg[CFG_SCI_ROUTE] & 7];
    bool level = p->sci;
    if (irq < 0 && level)
        debug(s, "SCI routed by 45h=%u has no modelled pin\n",
              s->ifb_cfg[CFG_SCI_ROUTE] & 7);
    if (p->sci_irq >= 0 && p->sci_line && irq != p->sci_irq) {
        if (s->config.sci)
            s->config.sci(s->config.opaque, (unsigned)p->sci_irq, false);
        p->sci_line = false;
    }
    p->sci_irq = irq;
    if (irq >= 0 && level != p->sci_line) {
        if (s->config.sci)
            s->config.sci(s->config.opaque, (unsigned)irq, level);
        p->sci_line = level;
    }
}

static void pm_update(I460GX *s);

/* A wake event: the IFB returns to ON and sets WAK_STS (11.2.7.1 bit 15). */
static void pm_wake(I460GX *s) {
    I460GXPm *p = &s->pm;
    I460GXPmState from = (I460GXPmState)p->state;
    p->state = I460GX_PM_ON;
    p->pm1_sts |= PM1_WAK;
    debug(s, "wake from %s\n", state_name(from));
    if (s->config.wake)
        s->config.wake(s->config.opaque, from);
    pm_update(s);
}

/* SLP_EN with a supported SLP_TYP (11.2.7.3), or a power-button override. */
static void pm_sleep(I460GX *s, I460GXPmState state, bool override) {
    I460GXPm *p = &s->pm;
    if (override) {
        p->pm1_sts |= PM1_PWRBTNOR;
        p->pm1_en &= (uint16_t)~PM1_RTC;
        p->gpe0_en |= GPE0_AFTERG3;
        p->gpe0_en &= (uint16_t)~(GPE0_RI | GPE0_USB1);
    }
    p->state = (uint8_t)state;
    debug(s, "%s: entering %s\n", override ? "power-button override" : "SLP_EN",
          state_name(state));
    if (s->config.sleep)
        s->config.sleep(s->config.opaque, state, override);
    pm_update(s);
}

static void pm_update(I460GX *s) {
    I460GXPm *p = &s->pm;
    gpio_eval(p);
    thrm_eval(p);
    smi_eval(s);
    bool sci = sci_level(p);
    if (sci != p->sci) {
        p->sci = sci;
        debug(s, "SCI %s\n", sci ? "asserted" : "deasserted");
    }
    sci_route(s);
    if (p->state != I460GX_PM_ON && wake_pending(p))
        pm_wake(s);
}

/* ---- Time ---------------------------------------------------------------- */

static uint64_t clock_now(const I460GX *s) {
    return s->config.clock ? s->config.clock(s->config.opaque) : 0;
}

/* Elapsed 8254 ticks: the PM timer, the SW SMI and 1 minute timers and the
 * power-button override timing. */
static void pm_tick(I460GX *s, uint64_t ticks) {
    I460GXPm *p = &s->pm;
    if (!ticks)
        return;
    if (core_powered(p)) {
        /* Bit 22 of the 24-bit timer falling sets TMROF_STS (11.2.7.4): that
         * is a multiple of 2^23, every 2.3435 s. */
        uint64_t before = p->tmr;
        p->tmr += PM_TICKS_PER_PIT_TICK * ticks;
        if ((before >> 23) != (p->tmr >> 23))
            p->pm1_sts |= PM1_TMROF;
        if (p->swsmi_left) {
            if (ticks >= p->swsmi_left) {
                p->swsmi_left = 0;
                p->glb_sts |= GS_SWSMI;
            } else {
                p->swsmi_left -= ticks;
            }
        }
        if (p->glb_ctl & GC_1MIN_EN) {
            p->min_acc += ticks;
            if (p->min_acc >= MIN_TICKS) {
                p->min_acc %= MIN_TICKS;
                p->glb_sts |= GS_1MIN;
            }
        }
    }
    if (p->pwrbtn_down && !p->pwrbtn_overridden) {
        p->pwrbtn_held += ticks;
        if (p->pwrbtn_held >= OVERRIDE_TICKS) {
            p->pwrbtn_overridden = true;
            if (p->state != I460GX_PM_S4S5) {
                pm_sleep(s, I460GX_PM_S4S5, true);
                return;
            }
        }
    }
    pm_update(s);
}

/* Bring the PM timers up to the board clock. */
static void pm_advance(I460GX *s, bool force) {
    I460GXPm *p = &s->pm;
    uint64_t now = clock_now(s);
    if (now <= p->clock_last)
        return;
    uint64_t delta = now - p->clock_last;
    uint64_t hz = s->config.clock_hz;
    if (!force && delta < hz / POLL_DIVISOR)
        return;
    p->clock_last = now;
    uint64_t scaled = (delta % hz) * PIT_HZ + p->clock_rem;
    p->clock_rem = scaled % hz;
    pm_tick(s, (delta / hz) * PIT_HZ + scaled / hz);
}

void i460gx_pm_poll(I460GX *s) {
    pm_advance(s, false);
}

/* ---- Register blocks ----------------------------------------------------- */

typedef struct PmReg {
    uint8_t off, size;
} PmReg;

static const PmReg acpi_regs[] = {
    {0x00, 2}, {0x02, 2}, {0x04, 2}, {0x08, 4}, {0x0c, 2}, {0x0e, 2},
    {0x16, 2}, {0x18, 2}, {0x1a, 2}, {0x1c, 2},
};
static const PmReg gpio_regs[] = {
    {0x00, 4}, {0x04, 4}, {0x08, 4}, {0x0c, 4}, {0x10, 4}, {0x14, 4},
    {0x1c, 4}, {0x20, 4}, {0x24, 4}, {0x28, 4},
};

static const PmReg *find_reg(const PmReg *table, size_t count, uint32_t off) {
    for (size_t i = 0; i < count; i++)
        if (off >= table[i].off && off < (uint32_t)table[i].off + table[i].size)
            return &table[i];
    return NULL;
}

static uint32_t acpi_read(const I460GXPm *p, unsigned off) {
    switch (off) {
    case 0x00: return p->pm1_sts;
    case 0x02: return p->pm1_en;
    case 0x04: return p->pm1_cnt;
    case 0x08: return (uint32_t)(p->tmr & 0x00ffffff);
    case 0x0c: return p->gpe0_sts;
    case 0x0e: return p->gpe0_en;
    case 0x16: return p->gpe1_sts;
    case 0x18: return p->gpe1_en;
    case 0x1a: return p->glb_ctl;
    case 0x1c: {
        uint32_t status = p->glb_sts;
        if (p->legacy_usb) status |= GS_LEGACY_USB;
        if (p->pm1_sts) status |= GS_PM1_REG;      /* an OR of PM1 status */
        if (p->gpe0_sts) status |= GS_GP_REG;      /* an OR of GPE0 status */
        return status;
    }
    default: return 0;
    }
}

static void acpi_write(I460GX *s, unsigned off, uint32_t value, uint32_t mask) {
    I460GXPm *p = &s->pm;
    switch (off) {
    case 0x00:
        p->pm1_sts &= (uint16_t)~(value & mask & PM1_STS_WRITE_CLEAR);
        break;
    case 0x02:
        set_bits16(&p->pm1_en, value, mask & PM1_EN_MASK);
        break;
    case 0x04:
        /* GBL_RLS asks the firmware for an SMI; BIOS_STS records it when
         * BIOS_EN is set (11.2.8.2 bit 0). The bit itself is storage the
         * handler clears. */
        if ((mask & CNT_GBL_RLS) && (value & CNT_GBL_RLS) &&
            (p->glb_ctl & GC_BIOS_EN))
            p->glb_sts |= GS_BIOS;
        set_bits16(&p->pm1_cnt, value, mask & CNT_STORED);
        if ((mask & CNT_SLP_EN) && (value & CNT_SLP_EN)) {
            unsigned type = (p->pm1_cnt & CNT_SLP_TYP) >> 10;
            if (type >= I460GX_PM_S1_STPCLK && type <= I460GX_PM_S4S5)
                pm_sleep(s, (I460GXPmState)type, false);
            else
                debug(s, "SLP_EN with SLP_TYP=%u: the IFB stays ON\n", type);
        }
        break;
    case 0x08:
        break;                              /* PM1_TMR is read-only */
    case 0x0c:
        p->gpe0_sts &= (uint16_t)~(value & mask & GPE0_STS_WRITE_CLEAR);
        break;
    case 0x0e:
        set_bits16(&p->gpe0_en, value, mask & GPE0_EN_MASK);
        break;
    case 0x16:
        p->gpe1_sts &= (uint16_t)~(value & mask & GPE1_MASK);
        break;
    case 0x18:
        set_bits16(&p->gpe1_en, value, mask & GPE1_MASK);
        break;
    case 0x1a: {
        uint16_t old = p->glb_ctl;
        set_bits16(&p->glb_ctl, value, mask & GC_STORED);
        /* BIOS_RLS reads 0; writing 1 sets GBL_STS (11.2.7.1 bit 5). */
        if ((mask & GC_BIOS_RLS) && (value & GC_BIOS_RLS))
            p->pm1_sts |= PM1_GBL;
        /* SW SMI timer (11.2.8.1 bit 7): enabling starts the 128 ms count,
         * clearing cancels it. 1MIN_EN starts a fresh minute. */
        if (!(old & GC_SWSMI_EN) && (p->glb_ctl & GC_SWSMI_EN))
            p->swsmi_left = SWSMI_TICKS;
        else if (!(p->glb_ctl & GC_SWSMI_EN))
            p->swsmi_left = 0;
        if (!(old & GC_1MIN_EN) && (p->glb_ctl & GC_1MIN_EN))
            p->min_acc = 0;
        /* End Of SMI: setting it deasserts SMI#. */
        if ((mask & GC_EOS) && (value & GC_EOS) && p->smi) {
            p->smi = false;
            report_smi(s, false);
        }
        break;
    }
    case 0x1c:
        p->glb_sts &= (uint16_t)~(value & mask & GS_WRITE_CLEAR);
        if (!(p->glb_sts & GS_BIOSWEN))
            p->bioswen_ble = false;
        break;
    default:
        break;
    }
}

static uint32_t gpio_read(const I460GXPm *p, unsigned off) {
    switch (off) {
    case 0x00: return p->gp_out;
    case 0x04: return gpio_data_read(p);
    case 0x08: return p->gp_ttl;
    case 0x0c: return p->gp_blink;
    case 0x10: return p->gp_lock;
    case 0x14: return p->gp_inv;
    case 0x1c: return p->gp_smi;
    case 0x20: return p->gp_pulse;
    case 0x24: return p->gp_core;
    case 0x28: return p->gp_pullup;
    default: return 0;
    }
}

static void gpio_write(I460GX *s, unsigned off, uint32_t value, uint32_t mask) {
    I460GXPm *p = &s->pm;
    /* GP Lock (11.2.9.5) bites only while the pin is an output. */
    uint32_t locked = p->gp_lock & p->gp_out;
    uint32_t writable = mask & ~locked;
    switch (off) {
    case 0x00: set_bits32(&p->gp_out, value, writable & GP_ALL); break;
    case 0x04:
        /* Only outputs take the write; a 1 written to a pulse-mode input
         * clears its edge latch (11.2.9.8). */
        set_bits32(&p->gp_data, value, writable & p->gp_out & GP_ALL);
        p->pulse_latch &= ~(value & mask & ~p->gp_out & p->gp_pulse);
        break;
    case 0x08: set_bits32(&p->gp_ttl, value, writable & GP_ALL); break;
    case 0x0c: set_bits32(&p->gp_blink, value, writable & GP_DEDICATED); break;
    case 0x10: p->gp_lock |= value & mask & GP_ALL; break; /* until PCIRST# */
    case 0x14: set_bits32(&p->gp_inv, value, writable & GP_DEDICATED); break;
    case 0x1c: set_bits32(&p->gp_smi, value, writable & GP_DEDICATED); break;
    case 0x20: set_bits32(&p->gp_pulse, value, writable & GP_DEDICATED); break;
    case 0x24: set_bits32(&p->gp_core, value, writable & GP_DEDICATED); break;
    case 0x28: set_bits32(&p->gp_pullup, value, mask & GP_PULLUP_MASK); break;
    default: break;
    }
}

typedef uint32_t (*RegRead)(const I460GXPm *p, unsigned off);
typedef void (*RegWrite)(I460GX *s, unsigned off, uint32_t value, uint32_t mask);

/* An access may cover parts of several registers (a dword at 00h is PM1_STS
 * and PM1_EN). Each register is written once, with the byte lanes it
 * received as `mask`, so write-one-to-clear bits outside the access are
 * untouched. Bytes between registers read 0 and drop writes. */
static void block_access(I460GX *s, const PmReg *table, size_t count,
                         RegRead read, RegWrite write, bool is_write,
                         uint32_t off, unsigned size, uint64_t *value) {
    uint64_t out = 0;
    unsigned i = 0;
    while (i < size) {
        const PmReg *reg = find_reg(table, count, off + i);
        if (!reg) {
            i++;
            continue;
        }
        unsigned lane = off + i - reg->off;
        unsigned bytes = reg->size - lane;
        if (bytes > size - i)
            bytes = size - i;
        if (is_write) {
            uint32_t v = 0, mask = 0;
            for (unsigned k = 0; k < bytes; k++) {
                v |= (uint32_t)((*value >> (8 * (i + k))) & 0xff) << (8 * (lane + k));
                mask |= 0xffu << (8 * (lane + k));
            }
            write(s, reg->off, v, mask);
        } else {
            uint32_t v = read(&s->pm, reg->off);
            for (unsigned k = 0; k < bytes; k++)
                out |= (uint64_t)((v >> (8 * (lane + k))) & 0xff) << (8 * (i + k));
        }
        i += bytes;
    }
    if (!is_write)
        *value = out;
}

static bool apm_io(I460GX *s, bool write, uint64_t port, unsigned size,
                   uint64_t *value) {
    I460GXPm *p = &s->pm;
    uint64_t out = 0;
    for (unsigned i = 0; i < size; i++) {
        uint64_t at = port + i;
        if (write) {
            uint8_t byte = (uint8_t)(*value >> (8 * i));
            if (at == PORT_APMC) {
                p->apmc = byte;
                /* A write to APMC generates an SMI if APMC_EN is set
                 * (11.2.6.1). */
                if (p->glb_ctl & GC_APMC_EN)
                    p->glb_sts |= GS_APM;
            } else {
                p->apms = byte;
            }
        } else {
            out |= (uint64_t)(at == PORT_APMC ? p->apmc : p->apms) << (8 * i);
        }
    }
    if (!write)
        *value = out;
    return true;
}

bool i460gx_pm_io(I460GX *s, bool write, uint64_t port, unsigned size,
                  uint64_t *value) {
    uint32_t acpi = ((uint32_t)s->ifb_cfg[CFG_ACPIBR] |
                     (uint32_t)s->ifb_cfg[CFG_ACPIBR + 1] << 8) &
                    CFG_BLOCK_BASE_MASK;
    uint32_t gpio = ((uint32_t)s->ifb_cfg[CFG_GPIOBA] |
                     (uint32_t)s->ifb_cfg[CFG_GPIOBA + 1] << 8) &
                    CFG_BLOCK_BASE_MASK;
    bool hit = true;
    if ((s->ifb_cfg[CFG_ACPI_CNTL] & CFG_BLOCK_ENABLE) && acpi &&
        port >= acpi && port - acpi <= ACPI_BLOCK_SIZE - size) {
        pm_advance(s, true);
        block_access(s, acpi_regs, sizeof(acpi_regs) / sizeof(acpi_regs[0]),
                     acpi_read, acpi_write, write, (uint32_t)(port - acpi),
                     size, value);
    } else if ((s->ifb_cfg[CFG_GPIO_CNTL] & CFG_BLOCK_ENABLE) && gpio &&
               port >= gpio && port - gpio <= GPIO_BLOCK_SIZE - size) {
        pm_advance(s, true);
        block_access(s, gpio_regs, sizeof(gpio_regs) / sizeof(gpio_regs[0]),
                     gpio_read, gpio_write, write, (uint32_t)(port - gpio),
                     size, value);
    } else if (size == 1 && (port == PORT_APMC || port == PORT_APMS)) {
        pm_advance(s, true);
        apm_io(s, write, port, size, value);
    } else {
        hit = false;
    }
    if (hit && write)
        pm_update(s);
    return hit;
}

/* ---- Board inputs -------------------------------------------------------- */

void i460gx_pm_set_gpio_pins(I460GX *s, uint32_t mask, uint32_t levels) {
    I460GXPm *p = &s->pm;
    mask &= GP_ALL;
    p->ext_mask |= mask;
    p->ext_level = (p->ext_level & ~mask) | (levels & mask);
    pm_update(s);
}

void i460gx_pm_rtc_alarm(I460GX *s) {
    s->pm.pm1_sts |= PM1_RTC;
    pm_update(s);
}

void i460gx_pm_power_button(I460GX *s, bool pressed) {
    I460GXPm *p = &s->pm;
    if (pressed == p->pwrbtn_down)
        return;
    p->pwrbtn_down = pressed;
    if (!pressed)
        return;
    p->pwrbtn_held = 0;
    p->pwrbtn_overridden = false;
    p->pm1_sts |= PM1_PWRBTN;
    /* The power button can always wake the system from S1-S5 (11.2.7.2). */
    if (p->state != I460GX_PM_ON)
        pm_wake(s);
    else
        pm_update(s);
}

/* ---- Reset --------------------------------------------------------------- */

/* power_on: RSMRST# after a power failure; the resume and core wells go to
 * their defaults, PWR_FAIL is set, and with AFTERG3 the IFB stays in S5.
 * Otherwise PCIRST# alone (a CF9 hard reset, or the power-up after a wake):
 * the resume and RTC wells survive. */
void i460gx_pm_reset(I460GX *s, bool power_on) {
    I460GXPm *p = &s->pm;
    uint16_t pm1_en = p->pm1_en & PM1_EN_RTC_WELL;
    uint16_t gpe0_en = p->gpe0_en & GPE0_EN_RTC_WELL;
    uint16_t pm1_sts = power_on ? 0 : (uint16_t)(p->pm1_sts & PM1_STS_RESUME);
    uint16_t gpe0_sts = power_on ? GPE0_PWR_FAIL
                                 : (uint16_t)(p->gpe0_sts & GPE0_STS_RESUME);
    uint16_t gpe1_en = power_on ? 0 : p->gpe1_en;
    uint16_t slp_typ = power_on ? 0 : (uint16_t)(p->pm1_cnt & CNT_SLP_TYP);
    uint32_t ext_mask = p->ext_mask, ext_level = p->ext_level;
    bool pwrbtn_down = p->pwrbtn_down;

    memset(p, 0, sizeof(*p));
    p->pm1_sts = pm1_sts;
    p->pm1_en = pm1_en;
    p->pm1_cnt = slp_typ;
    p->gpe0_sts = gpe0_sts;
    p->gpe0_en = gpe0_en;
    p->gpe1_en = gpe1_en;
    p->glb_ctl = GC_EOS;
    p->gp_pullup = GP_PULLUP_DEFAULT;
    p->ext_mask = ext_mask;
    p->ext_level = ext_level;
    p->pwrbtn_down = pwrbtn_down;
    p->sci_irq = -1;
    p->clock_last = clock_now(s);
    p->in_prev = gpio_in_value(p);
    p->state = (power_on && (p->gpe0_en & GPE0_AFTERG3)) ? I460GX_PM_S4S5
                                                          : I460GX_PM_ON;
    debug(s, "reset (%s): %s\n", power_on ? "RSMRST#" : "PCIRST#",
          state_name(p->state));
    pm_update(s);
}

/* BIOS_WEN was set (SSDM 11.1.11): BIOSWEN_STS, and with BLE an SMI# whatever
 * SMI_EN says (11.2.8.1 bit 0). */
void i460gx_pm_bios_wen(I460GX *s, bool ble) {
    s->pm.glb_sts |= GS_BIOSWEN;
    if (ble)
        s->pm.bioswen_ble = true;
    pm_update(s);
}
