/* MC146818-compatible real-time clock; see mc146818.h. */
#include "timer/mc146818.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { OSC_HZ = 32768, UIP_LEAD = 16, UPDATE_LENGTH = 65 };

enum {
    R_SEC, R_SEC_ALARM, R_MIN, R_MIN_ALARM, R_HOUR, R_HOUR_ALARM,
    R_DOW, R_DOM, R_MON, R_YEAR, R_A, R_B, R_C, R_D,
};

enum {
    B_SET = 0x80, B_PIE = 0x40, B_AIE = 0x20, B_UIE = 0x10, B_SQWE = 0x08,
    B_BINARY = 0x04, B_24H = 0x02, B_DSE = 0x01,
    C_IRQF = 0x80, C_PF = 0x40, C_AF = 0x20, C_UF = 0x10,
    A_UIP = 0x80,
    DV_NORMAL = 2,
    EXT_BANK = 128,
    /* RTC configuration (IFB function 0, C8h). */
    CONF_U128E = 0x04, CONF_L128LOCK = 0x08, CONF_U128LOCK = 0x10,
    LOCKED_FIRST = 0x38, LOCKED_LAST = 0x3f,
    REPORT_COUNT = 16,
};

static void report(Mc146818 *rtc, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void report(Mc146818 *rtc, const char *format, ...) {
    if (!rtc->config.debug || rtc->reported >= REPORT_COUNT)
        return;
    rtc->reported++;
    va_list args;
    va_start(args, format);
    fputs("mc146818: ", stderr);
    vfprintf(stderr, format, args);
    va_end(args);
}

/* The 32.768 kHz count for the board clock. */
static uint64_t osc_now(const Mc146818 *rtc) {
    if (!rtc->config.clock || !rtc->config.clock_hz)
        return 0;
    uint64_t ticks = rtc->config.clock(rtc->config.opaque);
    uint64_t hz = rtc->config.clock_hz;
    return (ticks / hz) * OSC_HZ + (ticks % hz) * OSC_HZ / hz;
}

static bool binary(const Mc146818 *rtc) {
    return (rtc->nv[R_B] & B_BINARY) != 0;
}

static unsigned bcd_to_int(uint8_t value) {
    return (unsigned)(value >> 4) * 10 + (value & 0x0f);
}

static uint8_t int_to_bcd(unsigned value) {
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static unsigned decode(const Mc146818 *rtc, uint8_t value) {
    return binary(rtc) ? value : bcd_to_int(value);
}

static uint8_t encode(const Mc146818 *rtc, unsigned value) {
    return binary(rtc) ? (uint8_t)value : int_to_bcd(value);
}

static void evaluate_irq(Mc146818 *rtc) {
    uint8_t *c = &rtc->nv[R_C];
    uint8_t b = rtc->nv[R_B];
    bool asserted = ((*c & C_PF) && (b & B_PIE)) ||
                    ((*c & C_AF) && (b & B_AIE)) ||
                    ((*c & C_UF) && (b & B_UIE));
    *c = asserted ? (*c | C_IRQF) : (uint8_t)(*c & ~C_IRQF);
    if (asserted != rtc->irq && rtc->config.irq)
        rtc->config.irq(rtc->config.opaque, asserted);
    rtc->irq = asserted;
}

static unsigned days_in(unsigned month, unsigned year) {
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12)
        return 31;
    /* The leap rule of the chip's two-digit year. */
    return month == 2 && year % 4 == 0 ? 29 : days[month - 1];
}

/* The end of one update cycle: the stored time advances a second, the alarms
 * are compared and UF is set. */
static void update_cycle(Mc146818 *rtc) {
    uint8_t *m = rtc->nv;
    if (m[R_B] & B_DSE)
        report(rtc, "daylight-saving updates (register B DSE) are not modelled\n");
    unsigned seconds = decode(rtc, m[R_SEC]) + 1;
    bool carry = seconds >= 60;
    m[R_SEC] = encode(rtc, carry ? 0 : seconds);
    if (carry) {
        unsigned minutes = decode(rtc, m[R_MIN]) + 1;
        carry = minutes >= 60;
        m[R_MIN] = encode(rtc, carry ? 0 : minutes);
    }
    bool new_day = false;
    if (carry) {
        if (m[R_B] & B_24H) {
            unsigned hours = decode(rtc, m[R_HOUR]) + 1;
            new_day = hours >= 24;
            m[R_HOUR] = encode(rtc, new_day ? 0 : hours);
        } else {
            /* Twelve-hour mode: bit 7 is PM; 11 -> 12 flips AM/PM, and the
             * PM -> AM flip starts a new day. */
            bool pm = (m[R_HOUR] & 0x80) != 0;
            unsigned hours = decode(rtc, (uint8_t)(m[R_HOUR] & 0x7f));
            if (hours == 11) { hours = 12; new_day = pm; pm = !pm; }
            else if (hours >= 12) { hours = 1; }
            else { hours++; }
            m[R_HOUR] = (uint8_t)(encode(rtc, hours) | (pm ? 0x80 : 0));
        }
    }
    if (new_day) {
        unsigned dow = decode(rtc, m[R_DOW]);
        m[R_DOW] = encode(rtc, dow >= 7 ? 1 : dow + 1);
        unsigned year = decode(rtc, m[R_YEAR]), month = decode(rtc, m[R_MON]);
        unsigned day = decode(rtc, m[R_DOM]) + 1;
        if (day > days_in(month, year)) {
            m[R_DOM] = encode(rtc, 1);
            if (month >= 12) {
                m[R_MON] = encode(rtc, 1);
                m[R_YEAR] = encode(rtc, year + 1 >= 100 ? 0 : year + 1);
            } else {
                m[R_MON] = encode(rtc, month + 1);
            }
        } else {
            m[R_DOM] = encode(rtc, day);
        }
    }
    /* A byte with both top bits set is a don't-care alarm; the date alarm is
     * register D bits 5:0, with 0 meaning don't care. */
#define ALARM_MATCH(alarm, current) \
    ((m[alarm] & 0xc0) == 0xc0 || m[alarm] == m[current])
    uint8_t date_alarm = m[R_D] & 0x3f;
    if (ALARM_MATCH(R_SEC_ALARM, R_SEC) && ALARM_MATCH(R_MIN_ALARM, R_MIN) &&
        ALARM_MATCH(R_HOUR_ALARM, R_HOUR) &&
        (!date_alarm || date_alarm == m[R_DOM]))
        m[R_C] |= C_AF;
#undef ALARM_MATCH
    m[R_C] |= C_UF;
}

/* The periodic tap of register A's RS, in divider counts: 0001 and 0010 are
 * 3.90625 ms and 7.8125 ms, 0011 to 1111 run 122.070 us doubling to 500 ms. */
static uint64_t periodic_tap(uint8_t a) {
    unsigned rs = a & 0x0f;
    if (!rs)
        return 0;
    if (rs <= 2)
        return rs == 1 ? 128 : 256;
    return UINT64_C(1) << (rs - 1);
}

/* Index of the last update cycle that ended by divider count `d`. */
static uint64_t update_index(uint64_t d) {
    return d >= OSC_HZ + UPDATE_LENGTH ? (d - UPDATE_LENGTH) / OSC_HZ : 0;
}

static void advance(Mc146818 *rtc) {
    uint64_t now = osc_now(rtc);
    uint64_t elapsed = now > rtc->osc_mark ? now - rtc->osc_mark : 0;
    rtc->osc_mark = now;
    if (!rtc->running || !elapsed)
        return;
    uint64_t before = rtc->divider, after = before + elapsed;
    rtc->divider = after;
    uint64_t tap = periodic_tap(rtc->nv[R_A]);
    if (tap && after / tap > before / tap)
        rtc->nv[R_C] |= C_PF;
    uint64_t cycles = update_index(after) - update_index(before);
    /* While SET is written the divider runs on but the time does not. */
    if (rtc->nv[R_B] & B_SET)
        cycles = 0;
    while (cycles--)
        update_cycle(rtc);
    evaluate_irq(rtc);
}

/* UIP: from 488 us before an update cycle to its end; never with SET. */
static bool update_in_progress(const Mc146818 *rtc) {
    if (!rtc->running || (rtc->nv[R_B] & B_SET))
        return false;
    uint64_t phase = rtc->divider % OSC_HZ;
    return (phase >= OSC_HZ - UIP_LEAD && rtc->divider >= OSC_HZ - UIP_LEAD) ||
           (phase < UPDATE_LENGTH && rtc->divider >= OSC_HZ);
}

static void write_register_a(Mc146818 *rtc, uint8_t value) {
    unsigned dv = (value >> 4) & 7;
    bool run = dv == DV_NORMAL;
    if ((dv & 6) == 6) {
        rtc->divider = 0;                       /* 11x: divider reset */
    } else if (!run) {
        report(rtc, "register A DV %u (test bypass or invalid) stops the divider\n", dv);
    } else if (!rtc->running) {
        rtc->divider = OSC_HZ / 2;              /* first update half a second later */
    }
    rtc->running = run;
    rtc->nv[R_A] = value & 0x7f;
    rtc->dirty = true;
}

static uint8_t read_standard(Mc146818 *rtc, unsigned index) {
    if (index > R_D)
        return rtc->nv[index];
    advance(rtc);
    switch (index) {
    case R_A:
        return (uint8_t)(rtc->nv[R_A] | (update_in_progress(rtc) ? A_UIP : 0));
    case R_C: {
        uint8_t value = rtc->nv[R_C];
        rtc->nv[R_C] = 0;                       /* a read clears the flags */
        evaluate_irq(rtc);
        return value;
    }
    case R_D:
        return (uint8_t)(0x80 | (rtc->nv[R_D] & 0x3f));     /* VRT reads 1 */
    default:
        return rtc->nv[index];
    }
}

static void write_standard(Mc146818 *rtc, unsigned index, uint8_t value) {
    if (index > R_D) {
        rtc->nv[index] = value;
        rtc->dirty = true;
        return;
    }
    advance(rtc);
    switch (index) {
    case R_A: write_register_a(rtc, value); break;
    case R_B:
        rtc->nv[R_B] = value;
        rtc->dirty = true;
        evaluate_irq(rtc);
        break;
    case R_C: break;                            /* writes have no effect */
    case R_D:
        rtc->nv[R_D] = value & 0x3f;
        rtc->dirty = true;
        break;
    default:
        /* Writes during an update cycle are ignored. */
        if (update_in_progress(rtc) && rtc->divider % OSC_HZ < UPDATE_LENGTH) {
            report(rtc, "write to register %u during an update cycle ignored\n", index);
            break;
        }
        rtc->nv[index] = value;
        break;
    }
}

bool mc146818_io(Mc146818 *rtc, bool write, uint16_t port, uint8_t *value,
                 uint8_t rtc_conf, bool alt_access) {
    if (port < 0x70 || port > 0x77)
        return false;
    unsigned p = port & 7;
    bool upper_enabled = (rtc_conf & CONF_U128E) != 0;
    if (p == 0 || p == 2 || p == 4 || p == 6) {
        /* Every index register carries NMI_EN in bit 7. 72h and 76h select
         * the extended bank only with the upper 128 bytes enabled; otherwise
         * they alias 70h and 74h. */
        bool extended = (p == 2 || p == 6) && upper_enabled;
        if (write) {
            rtc->nmi_disabled = (*value & 0x80) != 0;
            if (extended) rtc->ext_index = *value & 0x7f;
            else rtc->std_index = *value & 0x7f;
            return true;
        }
        /* Reads of 70h are reserved unless ALT access mode is on. */
        if (p == 0 && !alt_access)
            return false;
        *value = (uint8_t)((rtc->nmi_disabled ? 0x80 : 0) |
                           (extended ? rtc->ext_index : rtc->std_index));
        return true;
    }
    bool extended = (p == 3 || p == 7) && upper_enabled;
    unsigned index = extended ? rtc->ext_index : rtc->std_index;
    /* The lock bits freeze bytes 38h-3Fh of each bank: writes have no
     * effect and reads return no particular value. */
    if (index >= LOCKED_FIRST && index <= LOCKED_LAST &&
        (rtc_conf & (extended ? CONF_U128LOCK : CONF_L128LOCK))) {
        if (!write)
            *value = 0xff;
        return true;
    }
    if (extended) {
        if (write) {
            rtc->nv[EXT_BANK + index] = *value;
            rtc->dirty = true;
        } else {
            *value = rtc->nv[EXT_BANK + index];
        }
    } else if (write) {
        write_standard(rtc, index, *value);
    } else {
        *value = read_standard(rtc, index);
    }
    return true;
}

bool mc146818_power_up(Mc146818 *rtc) {
    advance(rtc);
    bool fresh = false;
    if (!rtc->battery) {
        /* A fresh cell: AIE and AF clear, everything else undefined. */
        rtc->nv[R_B] &= (uint8_t)~B_AIE;
        rtc->nv[R_C] &= (uint8_t)~C_AF;
        rtc->battery = true;
        fresh = true;
    }
    /* Standby power returning clears PIE, AIE, UIE and SQWE, and IRQF, PF and
     * UF; AF only a fresh cell clears. */
    rtc->nv[R_B] &= (uint8_t)~(B_PIE | B_AIE | B_UIE | B_SQWE);
    rtc->nv[R_C] &= C_AF;
    rtc->running = ((rtc->nv[R_A] >> 4) & 7) == DV_NORMAL;
    evaluate_irq(rtc);
    return fresh;
}

void mc146818_core_reset(Mc146818 *rtc) {
    rtc->nmi_disabled = true;
    rtc->std_index = rtc->ext_index = 0;
}

void mc146818_clock_restart(Mc146818 *rtc) {
    advance(rtc);
    rtc->osc_mark = 0;
}

void mc146818_poll(Mc146818 *rtc) {
    advance(rtc);
}

void mc146818_init(Mc146818 *rtc, const Mc146818Config *config) {
    memset(rtc, 0, sizeof(*rtc));
    rtc->config = *config;
    rtc->nmi_disabled = true;
    rtc->osc_mark = osc_now(rtc);
}

void mc146818_set_defaults(Mc146818 *rtc) {
    rtc->nv[R_A] = 0x26;                        /* DV = 010, RS = 0110 */
    rtc->nv[R_B] = B_24H;
    rtc->nv[R_C] = 0;
    rtc->nv[R_D] = 0;
    rtc->running = true;
    rtc->divider = OSC_HZ / 2;
    rtc->dirty = true;
}

void mc146818_get_time(Mc146818 *rtc, struct tm *tm) {
    advance(rtc);
    const uint8_t *m = rtc->nv;
    memset(tm, 0, sizeof(*tm));
    tm->tm_sec = (int)decode(rtc, m[R_SEC]);
    tm->tm_min = (int)decode(rtc, m[R_MIN]);
    unsigned hours = decode(rtc, (uint8_t)(m[R_HOUR] & 0x7f));
    if (!(m[R_B] & B_24H))
        hours = hours % 12 + ((m[R_HOUR] & 0x80) ? 12 : 0);
    tm->tm_hour = (int)hours;
    tm->tm_wday = (int)decode(rtc, m[R_DOW]) - 1;
    tm->tm_mday = (int)decode(rtc, m[R_DOM]);
    tm->tm_mon = (int)decode(rtc, m[R_MON]) - 1;
    unsigned year = decode(rtc, m[R_YEAR]);
    tm->tm_year = (int)(year < 70 ? year + 100 : year);
}

void mc146818_set_time(Mc146818 *rtc, const struct tm *tm) {
    uint8_t *m = rtc->nv;
    m[R_SEC] = encode(rtc, (unsigned)tm->tm_sec);
    m[R_MIN] = encode(rtc, (unsigned)tm->tm_min);
    if (m[R_B] & B_24H) {
        m[R_HOUR] = encode(rtc, (unsigned)tm->tm_hour);
    } else {
        unsigned hours = (unsigned)tm->tm_hour % 12;
        m[R_HOUR] = (uint8_t)(encode(rtc, hours ? hours : 12) |
                              (tm->tm_hour >= 12 ? 0x80 : 0));
    }
    m[R_DOW] = encode(rtc, (unsigned)tm->tm_wday + 1);
    m[R_DOM] = encode(rtc, (unsigned)tm->tm_mday);
    m[R_MON] = encode(rtc, (unsigned)tm->tm_mon + 1);
    m[R_YEAR] = encode(rtc, (unsigned)tm->tm_year % 100);
}

bool mc146818_take_dirty(Mc146818 *rtc) {
    bool dirty = rtc->dirty;
    rtc->dirty = false;
    return dirty;
}
