#ifndef _WIN32
#  define _POSIX_C_SOURCE 199309L
#endif
#include "um6578.h"
#include "rp2c02.h"  /* rp2c02_palette_rgb, reused for VNC's 64-colour table */
#include "gemu/screendump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Input action table ─────────────────────────────────────────────────── */

/* Names/keys match nes_actions[] in machine_nes.c exactly (same physical
 * device, same gemu.ini [nes-controller] section — see .ini_section below). */
static const GemuActionDef um6578_actions[UM6578_NUM_ACTIONS] = {
    { "A",      GEMU_ACTION(UM6578_ACT_A),      "z"           },
    { "B",      GEMU_ACTION(UM6578_ACT_B),      "x"           },
    { "Select", GEMU_ACTION(UM6578_ACT_SELECT), "Right Shift" },
    { "Start",  GEMU_ACTION(UM6578_ACT_START),  "Return"      },
    { "Up",     GEMU_ACTION(UM6578_ACT_UP),     "Up"          },
    { "Down",   GEMU_ACTION(UM6578_ACT_DOWN),   "Down"        },
    { "Left",   GEMU_ACTION(UM6578_ACT_LEFT),   "Left"        },
    { "Right",  GEMU_ACTION(UM6578_ACT_RIGHT),  "Right"       },
};

/* MAME's nes_sh6578 INPUT_PORTS: bit0=Button2(B), bit1=Button1(A), bit2=Select,
 * bit3=Start, bit4=Up, bit5=Down, bit6=Left, bit7=Right — all IP_ACTIVE_HIGH
 * (1 = pressed, shifted out directly, no inversion). */
static uint8_t um6578_joypad_byte(uint32_t held) {
    uint8_t v = 0;
    if (held & GEMU_ACTION(UM6578_ACT_B))      v |= 0x01u;
    if (held & GEMU_ACTION(UM6578_ACT_A))      v |= 0x02u;
    if (held & GEMU_ACTION(UM6578_ACT_SELECT)) v |= 0x04u;
    if (held & GEMU_ACTION(UM6578_ACT_START))  v |= 0x08u;
    if (held & GEMU_ACTION(UM6578_ACT_UP))     v |= 0x10u;
    if (held & GEMU_ACTION(UM6578_ACT_DOWN))   v |= 0x20u;
    if (held & GEMU_ACTION(UM6578_ACT_LEFT))   v |= 0x40u;
    if (held & GEMU_ACTION(UM6578_ACT_RIGHT))  v |= 0x80u;
    return v;
}

static GemuPointerState um6578_pointer_state(Um6578State *s) {
    if (s->mouse_synth_active) {
        return (GemuPointerState){
            .x = s->mouse_synth_x,
            .y = s->mouse_synth_y,
            .button = s->mouse_synth_left,
            .right_button = s->mouse_synth_right,
        };
    }
    if (!s->display && s->vnc) {
        GemuVncPointerState p = gemu_vnc_get_pointer(s->vnc);
        return (GemuPointerState){
            .x = p.x,
            .y = p.y,
            .rel_x = p.rel_x,
            .rel_y = p.rel_y,
            .button = p.button,
            .pressed = p.pressed,
            .right_button = p.right_button,
            .right_pressed = p.right_pressed,
        };
    }
    if (!s->display) return (GemuPointerState){ .x = -1, .y = -1 };
    return gemu_display_get_pointer(s->display);
}

static void um6578_queue_ps2_mouse_packet(Um6578State *s, int dx, int dy,
                                          bool left, bool right);

/* UM6578 mouse 24-bit reply — see the protocol table in um6578.h. Computed
 * fresh on every strobe from the accumulated host-pointer delta since the
 * last strobe (s->mouse_legacy_px/py), matching how the mouse reports
 * relative movement each time it's polled. */
static uint32_t um6578_subor_mouse_word(Um6578State *s) {
    uint32_t v = (1u << 21);                    /* E: some real units keep this always set */
    if (s->mouse_stream_enabled)
        return v;

    GemuPointerState ptr = um6578_pointer_state(s);

    int dx = ptr.rel_x;
    int dy = ptr.rel_y;
    if (dx == 0 && dy == 0) {
        if (ptr.x < 0 || ptr.y < 0) return v;
        dx = s->mouse_legacy_have_pos ? ptr.x - s->mouse_legacy_px : 0;
        dy = s->mouse_legacy_have_pos ? ptr.y - s->mouse_legacy_py : 0;
    }
    if (ptr.x >= 0 && ptr.y >= 0) {
        s->mouse_legacy_px = ptr.x;
        s->mouse_legacy_py = ptr.y;
        s->mouse_legacy_have_pos = true;
    }
    bool left = ptr.button || ptr.pressed;
    bool right = ptr.right_button || ptr.right_pressed;
    if (s->trace_io && (dx != 0 || dy != 0 || left || right))
        fprintf(stderr,
                "um6578: mouse sample (legacy $4016) x=%d y=%d rel=%d,%d dx=%d dy=%d left=%d right=%d pc=%04X\n",
                ptr.x, ptr.y, ptr.rel_x, ptr.rel_y, dx, dy,
                left ? 1 : 0, right ? 1 : 0, s->cpu.PC);

    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    if (ax > 127) ax = 127;
    if (ay > 127) ay = 127;

    if (left)               v |= (1u << 23); /* left button */
    if (right)              v |= (1u << 22); /* right button */
    if (dy < 0) v |= (1u << 19);              /* up */
    if (dy > 0) v |= (1u << 18);              /* down */
    if (dx < 0) v |= (1u << 17);              /* left */
    if (dx > 0) v |= (1u << 16);              /* right */
    v |= ((uint32_t)ax & 0x7Fu) << 8;         /* X magnitude, bits 14-8 */
    v |= ((uint32_t)ay & 0x7Fu);              /* Y magnitude, bits 6-0 */
    return v;
}

/* ── IRQ mux ($4032/4033) ────────────────────────────────────────────────── */

#define UM6578_IRQ_TIMER    0x80u
#define UM6578_IRQ_MOUSE    0x40u
#define UM6578_IRQ_KEYBOARD 0x20u

static void irq_recompute(Um6578State *s) {
    s->cpu.irq = (s->irq_pending & (uint8_t)~s->irq_mask) != 0;
}

static void irq_raise(Um6578State *s, uint8_t bit) {
    s->irq_pending |= bit;
    irq_recompute(s);
}

static bool mouse_queue_empty(const Um6578State *s) {
    return s->mouse_qhead == s->mouse_qtail;
}

static void mouse_queue_push(Um6578State *s, uint8_t v) {
    uint8_t next = (uint8_t)((s->mouse_qtail + 1u) & 0x0Fu);
    if (next == s->mouse_qhead)
        s->mouse_qhead = (uint8_t)((s->mouse_qhead + 1u) & 0x0Fu);
    s->mouse_queue[s->mouse_qtail] = v;
    s->mouse_qtail = next;
}

static uint8_t mouse_queue_pop(Um6578State *s) {
    if (mouse_queue_empty(s)) return 0xFFu;
    uint8_t v = s->mouse_queue[s->mouse_qhead];
    s->mouse_qhead = (uint8_t)((s->mouse_qhead + 1u) & 0x0Fu);
    if (mouse_queue_empty(s)) {
        s->irq_pending &= (uint8_t)~UM6578_IRQ_KEYBOARD;
        irq_recompute(s);
    } else {
        irq_raise(s, UM6578_IRQ_KEYBOARD);
    }
    return v;
}

static void mouse_queue_byte(Um6578State *s, uint8_t v) {
    mouse_queue_push(s, v);
    irq_raise(s, UM6578_IRQ_KEYBOARD);
}

static void mouse_set_command_response(Um6578State *s, const uint8_t *bytes, uint8_t len) {
    if (len > sizeof(s->mouse_cmd_resp))
        len = (uint8_t)sizeof(s->mouse_cmd_resp);
    memcpy(s->mouse_cmd_resp, bytes, len);
    s->mouse_cmd_resp_len = len;
    s->mouse_cmd_resp_pos = 0;
}

static void mouse_maybe_queue_command_response(Um6578State *s) {
    if (s->mouse_cmd_resp_pos >= s->mouse_cmd_resp_len || !mouse_queue_empty(s))
        return;
    mouse_queue_byte(s, s->mouse_cmd_resp[s->mouse_cmd_resp_pos++]);
}

static int clamp_ps2_delta(int v) {
    if (v < -127) return -127;
    if (v > 127) return 127;
    return v;
}

static void um6578_queue_ps2_mouse_packet(Um6578State *s, int dx, int dy,
                                          bool left, bool right) {
    if (dx == 0 && dy == 0 &&
        left == s->mouse_prev_left && right == s->mouse_prev_right)
        return;

    int ps2_dx = clamp_ps2_delta(dx);
    int ps2_dy = clamp_ps2_delta(-dy); /* host Y grows downward; PS/2 Y grows upward */
    uint8_t flags = 0x08u;
    if (left) flags |= 0x01u;
    if (right) flags |= 0x02u;
    if (ps2_dx < 0) flags |= 0x10u;
    if (ps2_dy < 0) flags |= 0x20u;
    if (dx < -127 || dx > 127) flags |= 0x40u;
    if (-dy < -127 || -dy > 127) flags |= 0x80u;

    mouse_queue_push(s, flags);
    mouse_queue_push(s, (uint8_t)ps2_dx);
    mouse_queue_push(s, (uint8_t)ps2_dy);
    if (s->trace_io)
        fprintf(stderr, "um6578: queue mouse ps2 %02X %02X %02X dx=%d dy=%d pc=%04X\n",
                flags, (uint8_t)ps2_dx, (uint8_t)ps2_dy, dx, dy, s->cpu.PC);
    s->mouse_prev_left = left;
    s->mouse_prev_right = right;
    /* Connie-chan's IRQ handler services the PS/2 byte queue on bit $20
     * (Keyboard) — confirmed by tracing real reads; UM6578 apparently has
     * no separate dedicated Mouse Port/IRQ the way the (different, only
     * loosely related) SH6578 datasheet describes. See um6578.h. */
    irq_raise(s, UM6578_IRQ_KEYBOARD);
}

static void um6578_mouse_device_command(Um6578State *s, uint8_t val) {
    if (s->trace_io)
        fprintf(stderr, "um6578: mouse cmd %02X pc=%04X\n", val, s->cpu.PC);

    if (s->mouse_param_cmd) {
        static const uint8_t ack[] = { 0xFAu };
        mouse_set_command_response(s, ack, (uint8_t)sizeof(ack));
        s->mouse_param_cmd = 0;
        return;
    }

    switch (val) {
    case 0xFF: /* Reset */
    {
        static const uint8_t reset_resp[] = { 0xFAu, 0xAAu, 0x00u };
        s->mouse_stream_enabled = true;
        s->mouse_prev_left = false;
        s->mouse_prev_right = false;
        s->mouse_have_pos = false;
        s->mouse_legacy_have_pos = false;
        s->mouse_qhead = s->mouse_qtail = 0;
        s->irq_pending &= (uint8_t)~UM6578_IRQ_KEYBOARD;
        irq_recompute(s);
        mouse_set_command_response(s, reset_resp, (uint8_t)sizeof(reset_resp));
        break;
    }
    case 0xF4: /* Enable data reporting */
    {
        static const uint8_t ack[] = { 0xFAu };
        s->mouse_stream_enabled = true;
        mouse_set_command_response(s, ack, (uint8_t)sizeof(ack));
        break;
    }
    case 0xF5: /* Disable data reporting */
    {
        static const uint8_t ack[] = { 0xFAu };
        s->mouse_stream_enabled = false;
        mouse_set_command_response(s, ack, (uint8_t)sizeof(ack));
        break;
    }
    case 0xF3: /* Set sample rate, parameter follows */
    case 0xE8: /* Set resolution, parameter follows */
    {
        static const uint8_t ack[] = { 0xFAu };
        s->mouse_param_cmd = val;
        mouse_set_command_response(s, ack, (uint8_t)sizeof(ack));
        break;
    }
    case 0xF2: /* Read device type */
    {
        static const uint8_t type_resp[] = { 0xFAu, 0x00u };
        mouse_set_command_response(s, type_resp, (uint8_t)sizeof(type_resp));
        break;
    }
    default:
    {
        static const uint8_t ack[] = { 0xFAu };
        mouse_set_command_response(s, ack, (uint8_t)sizeof(ack));
        break;
    }
    }
}

/* Sample the host pointer once per frame, independent of $4016 strobing —
 * a real PS/2 mouse streams movement to the SH6578's Mouse Port ($4024)
 * asynchronously, not on demand from a controller-shift read. */
static void um6578_mouse_frame_tick(Um6578State *s) {
    if (!s->mouse_stream_enabled)
        return;
    if (s->ram_lo[0x36] != 0 || s->ram_lo[0x37] != 1 || !mouse_queue_empty(s)) {
        if (!s->display && s->vnc)
            (void)gemu_vnc_get_pointer(s->vnc);
        return;
    }

    GemuPointerState ptr = um6578_pointer_state(s);

    int dx = ptr.rel_x;
    int dy = ptr.rel_y;
    if (dx == 0 && dy == 0) {
        if (!s->mouse_synth_active)
            return;
        dx = s->mouse_have_pos ? ptr.x - s->mouse_px : 0;
        dy = s->mouse_have_pos ? ptr.y - s->mouse_py : 0;
    }
    if (ptr.x >= 0 && ptr.y >= 0) {
        s->mouse_px = ptr.x;
        s->mouse_py = ptr.y;
        s->mouse_have_pos = true;
    }
    bool left = ptr.button || ptr.pressed;
    bool right = ptr.right_button || ptr.right_pressed;
    if (dx == 0 && dy == 0 && left == s->mouse_prev_left && right == s->mouse_prev_right)
        return;
    if (s->trace_io && (dx != 0 || dy != 0 || left || right))
        fprintf(stderr,
                "um6578: mouse port sample x=%d y=%d rel=%d,%d dx=%d dy=%d left=%d right=%d\n",
                ptr.x, ptr.y, ptr.rel_x, ptr.rel_y, dx, dy,
                left ? 1 : 0, right ? 1 : 0);
    um6578_queue_ps2_mouse_packet(s, dx, dy, left, right);
}

/* ── Timer ($4034/4035/4036) ─────────────────────────────────────────────── */

static void timer_tick(Um6578State *s) {
    if (!(s->timer_ctrl & 0x80)) return; /* E: not enabled */
    uint8_t prescale = s->timer_ctrl & 0x07;
    s->timer_prescale_acc++;
    if (s->timer_prescale_acc < (1u << prescale)) return;
    s->timer_prescale_acc = 0;

    if (s->timer_count == 0) {
        irq_raise(s, UM6578_IRQ_TIMER);
        if (s->timer_ctrl & 0x40) /* R: repeat */
            s->timer_count = s->timer_preset;
        else
            s->timer_ctrl &= (uint8_t)~0x80; /* one-shot: clear E on underflow */
    } else {
        s->timer_count--;
    }
}

static void timer_maybe_tick(Um6578State *s, bool is_scanline_edge) {
    bool source_is_scanline = (s->timer_ctrl & 0x20) != 0; /* S bit */
    if (source_is_scanline == is_scanline_edge)
        timer_tick(s);
}

/* ── CPU bus ─────────────────────────────────────────────────────────────── */

static uint8_t um6578_read(uint16_t addr, void *ud);
static void    um6578_write(uint16_t addr, uint8_t val, void *ud);

static bool um6578_mouse_ram_addr(uint16_t addr) {
    switch (addr) {
    case 0x06: case 0x07: case 0x08: case 0x09:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x36: case 0x37: case 0x38:
    case 0x60: case 0x67:
    case 0x68: case 0x69: case 0x6a: case 0x6b:
    case 0x6c: case 0x6d: case 0x6e: case 0x6f:
        return true;
    default:
        return false;
    }
}

static uint8_t bank_read(Um6578State *s, int bank, uint16_t offset) {
    uint32_t address = (offset & 0x0FFFu) | ((uint32_t)s->bankswitch[bank] << 12);
    return address < s->rom_size ? s->rom[address] : 0xFFu;
}

static void bank_write(Um6578State *s, int bank, uint16_t offset, uint8_t val) {
    uint32_t address = (offset & 0x0FFFu) | ((uint32_t)s->bankswitch[bank] << 12);
    if (address < s->rom_size) s->rom[address] = val;
}

static void do_dma(Um6578State *s) {
    if (!(s->dma_control & 0x80)) return;

    uint16_t source = (uint16_t)(s->dma_source[0] | (s->dma_source[1] << 8));
    uint16_t dest   = (uint16_t)(s->dma_dest[0]   | (s->dma_dest[1]   << 8));
    uint16_t length = (uint16_t)(s->dma_length[0] | (s->dma_length[1] << 8));

    for (int i = 0; i <= (int)length; i++) {
        uint8_t data;
        if (source & 0x8000) {
            uint32_t trueaddr = (uint32_t)(source & 0x7FFFu) | ((uint32_t)(s->dma_bank & 0x1F) * 0x8000u);
            data = trueaddr < s->rom_size ? s->rom[trueaddr] : 0xFFu;
        } else {
            data = um6578_read((uint16_t)(source & 0x7FFFu), s);
        }

        if (s->dma_control & 0x20)
            um6578_write((uint16_t)dest, data, s);
        else
            s->ppu.vram[dest] = data;

        source++;
        dest++;
    }
}

static uint8_t um6578_read(uint16_t addr, void *ud) {
    Um6578State *s = ud;

    if (addr < 0x2000) return s->ram_lo[addr];
    if (addr >= 0x2000 && addr <= 0x2007) return ppu_sh6578_read(&s->ppu, (uint8_t)addr);
    if (addr == 0x2008) return ppu_sh6578_read_ext(&s->ppu);
    if (addr >= 0x2040 && addr <= 0x207F) return ppu_sh6578_palette_read(&s->ppu, (uint8_t)(addr - 0x2040));

    if (addr == 0x4015) return apu2a03_read(&s->apu, addr);
    if (addr == 0x4016) {
        uint8_t v;
        if (s->io_mouse_active) {
            v = (uint8_t)((s->iolatch >> 23) & 1u); /* mouse: MSB (bit23) first */
            s->iolatch <<= 1;
        } else {
            v = (uint8_t)(s->iolatch & 1u);         /* standard pad: LSB first */
            s->iolatch >>= 1;
        }
        if (s->trace_4016)
            fprintf(stderr, "um6578: read  $4016 -> %02X pc=%04X mouse=%d latch=%06X\n",
                    v, s->cpu.PC, s->io_mouse_active ? 1 : 0,
                    (unsigned)(s->iolatch & 0xFFFFFFu));
        return v;
    }
    if (addr == 0x4017) return 0; /* IN1: unused on every title we support */
    if (addr == 0x4020) {
        uint8_t v = mouse_queue_pop(s);
        if (s->trace_io)
            fprintf(stderr, "um6578: read  $4020 -> %02X pc=%04X\n", v, s->cpu.PC);
        return v; /* keyboard/mouse byte queue; empty reads as 0xFF */
    }
    if (addr == 0x4026) {
        uint8_t v = 0xFFu; /* EXT/status lines idle high; mouse buttons arrive in PS/2 packets */
        if (s->trace_io)
            fprintf(stderr, "um6578: read  $4026 -> %02X pc=%04X\n", v, s->cpu.PC);
        return v;
    }
    if (addr == 0x4033) {
        if (s->trace_io && s->irq_pending &&
            (s->trace_timer || (s->irq_pending & (UM6578_IRQ_MOUSE | UM6578_IRQ_KEYBOARD)) != 0))
            fprintf(stderr, "um6578: read  $4033 -> %02X pc=%04X\n", s->irq_pending, s->cpu.PC);
        return s->irq_pending;
    }
    if (addr == 0x4036) return s->timer_count;

    if (addr >= 0x4040 && addr <= 0x4047) return s->bankswitch[addr - 0x4040];
    if (addr >= 0x4048 && addr <= 0x404F) {
        switch (addr - 0x4048) {
        case 0: return s->dma_control & 0x7Fu;
        case 1: return s->dma_bank;
        case 2: return s->dma_source[0];
        case 3: return s->dma_source[1];
        case 4: return s->dma_dest[0];
        case 5: return s->dma_dest[1];
        case 6: return s->dma_length[0];
        case 7: return s->dma_length[1];
        }
    }

    if (addr >= 0x5000 && addr <= 0x7FFF) return s->ram_hi[addr - 0x5000];
    if (addr >= 0x8000) return bank_read(s, (addr - 0x8000) >> 12, addr);

    return 0xFF;
}

static void um6578_write(uint16_t addr, uint8_t val, void *ud) {
    Um6578State *s = ud;

    if (addr < 0x2000) {
        if (s->trace_mouse_state && um6578_mouse_ram_addr(addr) && s->ram_lo[addr] != val)
            fprintf(stderr, "um6578: z%02X <- %02X pc=%04X\n", addr, val, s->cpu.PC);
        s->ram_lo[addr] = val;
        return;
    }
    if (addr >= 0x2000 && addr <= 0x2007) { ppu_sh6578_write(&s->ppu, (uint8_t)addr, val); return; }
    if (addr == 0x2008) { ppu_sh6578_write_ext(&s->ppu, val); return; }
    if (addr >= 0x2040 && addr <= 0x207F) { ppu_sh6578_palette_write(&s->ppu, (uint8_t)(addr - 0x2040), val); return; }

    if (addr == 0x4014) {
        uint8_t page[256];
        uint16_t base = (uint16_t)(val << 8);
        for (int i = 0; i < 256; i++) page[i] = um6578_read((uint16_t)(base + i), s);
        ppu_sh6578_oam_dma(&s->ppu, page);
        return;
    }
    if (addr == 0x4016) {
        bool has_pad   = s->cfg->n_ports > 0 && s->cfg->ports[0] == NES_DEVICE_CONTROLLER;
        bool has_mouse = s->cfg->n_ports > 0 && s->cfg->ports[0] == NES_DEVICE_MOUSE;
        if ((s->prev_io & 1) && !(val & 1)) {
            if (has_mouse) {
                s->io_mouse_active = true;
                s->iolatch = um6578_subor_mouse_word(s);
            } else {
                s->io_mouse_active = false;
                s->iolatch = has_pad ? um6578_joypad_byte(s->held_actions) : 0;
            }
            if (s->trace_4016)
                fprintf(stderr, "um6578: latch $4016 pc=%04X mouse=%d value=%06X\n",
                        s->cpu.PC, s->io_mouse_active ? 1 : 0,
                        (unsigned)(s->iolatch & 0xFFFFFFu));
        }
        if (s->trace_4016)
            fprintf(stderr, "um6578: write $4016 <- %02X pc=%04X\n", val, s->cpu.PC);
        s->prev_io = val;
        return;
    }
    if (addr >= 0x4000 && addr <= 0x4017) { apu2a03_write(&s->apu, addr, val); return; }

    if (addr == 0x4020) {
        if (s->trace_io)
            fprintf(stderr, "um6578: write $4020 <- %02X pc=%04X\n", val, s->cpu.PC);
        return; /* keyboard-related write, unmodelled */
    }
    if (addr == 0x4021) {
        if (s->cfg->n_ports > 0 && s->cfg->ports[0] == NES_DEVICE_MOUSE) {
            um6578_mouse_device_command(s, val);
        } else if (s->trace_io) {
            fprintf(stderr, "um6578: write $4021 <- %02X pc=%04X\n", val, s->cpu.PC);
        }
        return;
    }
    if (addr == 0x4022) {
        if (s->trace_io)
            fprintf(stderr, "um6578: write $4022 <- %02X pc=%04X\n", val, s->cpu.PC);
        if (val == 0x80 && s->cfg->n_ports > 0 && s->cfg->ports[0] == NES_DEVICE_MOUSE &&
            !(s->cpu.PC >= 0xDCF1 && s->cpu.PC <= 0xDD50))
            mouse_maybe_queue_command_response(s);
        return;
    }
    if (addr == 0x4026) {
        if (s->trace_io)
            fprintf(stderr, "um6578: write $4026 <- %02X pc=%04X\n", val, s->cpu.PC);
        return; /* EXT write: stub */
    }
    if (addr == 0x4027) return; /* DAC data register: unmodelled */
    if (addr == 0x4031) return; /* startup protection sequence: logging-only upstream too */
    if (addr == 0x4032) {
        if (s->trace_io &&
            (s->trace_timer || (s->irq_pending & (UM6578_IRQ_MOUSE | UM6578_IRQ_KEYBOARD)) != 0))
            fprintf(stderr, "um6578: write $4032 <- %02X pc=%04X pending=%02X\n",
                    val, s->cpu.PC, s->irq_pending);
        s->irq_mask = val;
        s->irq_pending &= (uint8_t)~val; /* a 1 bit acknowledges (clears) that pending line */
        irq_recompute(s);
        if (!mouse_queue_empty(s))
            irq_raise(s, UM6578_IRQ_KEYBOARD);
        return;
    }
    if (addr == 0x4034) {
        if (s->trace_io)
            fprintf(stderr, "um6578: write $4034 <- %02X pc=%04X\n", val, s->cpu.PC);
        s->timer_ctrl = val;
        s->timer_prescale_acc = 0;
        return;
    }
    if (addr == 0x4035) {
        if (s->trace_io)
            fprintf(stderr, "um6578: write $4035 <- %02X pc=%04X\n", val, s->cpu.PC);
        s->timer_preset = val;
        s->timer_count = val; /* writing the preset also resets the live count */
        return;
    }

    if (addr >= 0x4040 && addr <= 0x4047) { s->bankswitch[addr - 0x4040] = val; return; }
    if (addr >= 0x4048 && addr <= 0x404F) {
        switch (addr - 0x4048) {
        case 0: s->dma_control = val; do_dma(s); break;
        case 1: s->dma_bank = val; break;
        case 2: s->dma_source[0] = val; break;
        case 3: s->dma_source[1] = val; break;
        case 4: s->dma_dest[0] = val; break;
        case 5: s->dma_dest[1] = val; break;
        case 6: s->dma_length[0] = val; break;
        case 7: s->dma_length[1] = val; break;
        }
        return;
    }

    if (addr >= 0x5000 && addr <= 0x7FFF) { s->ram_hi[addr - 0x5000] = val; return; }
    if (addr >= 0x8000) { bank_write(s, (addr - 0x8000) >> 12, addr, val); return; }
}

static uint8_t um6578_apu_mem_read(uint16_t addr, void *ud) {
    return um6578_read(addr, ud);
}

/* ── Input: local display actions + VNC key -> action mapping ──────────── */

#define XK_Up     0xFF52u
#define XK_Down   0xFF54u
#define XK_Left   0xFF51u
#define XK_Right  0xFF53u
#define XK_Return 0xFF0Du
#define XK_Shift_R 0xFFE2u

static void um6578_handle_keys(Um6578State *s, uint32_t held) {
    if (s->display) {
        s->held_actions = held;
    } else if (!s->vnc) {
        s->held_actions = 0;
    }

    if (s->vnc) {
        GemuVncKeyEvent ev;
        while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
            uint32_t bit = 0;
            switch (ev.keysym) {
            case 'z': case 'Z':  bit = GEMU_ACTION(UM6578_ACT_A);      break;
            case 'x': case 'X':  bit = GEMU_ACTION(UM6578_ACT_B);      break;
            case XK_Shift_R:     bit = GEMU_ACTION(UM6578_ACT_SELECT); break;
            case XK_Return:      bit = GEMU_ACTION(UM6578_ACT_START);  break;
            case XK_Up:          bit = GEMU_ACTION(UM6578_ACT_UP);     break;
            case XK_Down:        bit = GEMU_ACTION(UM6578_ACT_DOWN);   break;
            case XK_Left:        bit = GEMU_ACTION(UM6578_ACT_LEFT);   break;
            case XK_Right:       bit = GEMU_ACTION(UM6578_ACT_RIGHT);  break;
            default: break;
            }
            if (!bit) continue;
            if (ev.down) s->held_actions |= bit;
            else         s->held_actions &= ~bit;
        }
    }

    if (s->input_inject_frames > 0) {
        s->held_actions |= s->input_inject_mask;
        s->input_inject_frames--;
    } else {
        s->input_inject_mask = 0;
    }
}

/* ── Screendump ──────────────────────────────────────────────────────────── */

static bool um6578_screendump(void *ud, const char *path) {
    Um6578State *s = ud;
    int w = SH6578_WIDTH, h = SH6578_HEIGHT;
    uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return false;
    for (int i = 0; i < w * h; i++) {
        uint32_t c = s->ppu.pixels_argb[i];
        rgb[i * 3 + 0] = (uint8_t)(c >> 16);
        rgb[i * 3 + 1] = (uint8_t)(c >> 8);
        rgb[i * 3 + 2] = (uint8_t)(c);
    }
    bool ok = gemu_screendump(path, rgb, w, h);
    free(rgb);
    return ok;
}

static bool parse_int_token(const char **p, int *out) {
    while (**p == ' ' || **p == '\t') (*p)++;
    if (!**p) return false;

    char *end = NULL;
    long v = strtol(*p, &end, 0);
    if (end == *p) return false;
    *out = (int)v;
    *p = end;
    return true;
}

static bool parse_button_token(const char **p, const char *name) {
    size_t n = strlen(name);
    while (**p == ' ' || **p == '\t') (*p)++;
    if (strncasecmp(*p, name, n) != 0)
        return false;
    char c = (*p)[n];
    if (c && c != ' ' && c != '\t')
        return false;
    *p += n;
    return true;
}

static bool um6578_mouse_command(Um6578State *s, const char *text) {
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;

    if (strncasecmp(p, "mouseclear", 10) == 0) {
        p += 10;
        while (*p == ' ' || *p == '\t') p++;
        if (*p) return false;
        s->mouse_synth_active = false;
        s->mouse_have_pos = false;
        s->mouse_legacy_have_pos = false;
        printf("mouse: using host pointer\n");
        return true;
    }

    bool relative = false;
    if (strncasecmp(p, "mousemove", 9) == 0) {
        p += 9;
        relative = true;
    } else if (strncasecmp(p, "mouse", 5) == 0) {
        p += 5;
    } else {
        return false;
    }

    int x = 0, y = 0;
    if (!parse_int_token(&p, &x) || !parse_int_token(&p, &y))
        return false;

    bool left = s->mouse_synth_left;
    bool right = s->mouse_synth_right;
    while (true) {
        if (parse_button_token(&p, "left")) {
            left = true;
        } else if (parse_button_token(&p, "right")) {
            right = true;
        } else if (parse_button_token(&p, "none")) {
            left = right = false;
        } else {
            break;
        }
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return false;

    bool was_synthetic = s->mouse_synth_active;
    if (relative && was_synthetic) {
        x += s->mouse_synth_x;
        y += s->mouse_synth_y;
    }
    if (!was_synthetic) {
        s->mouse_px = x;
        s->mouse_py = y;
        s->mouse_have_pos = true;
        s->mouse_legacy_px = x;
        s->mouse_legacy_py = y;
        s->mouse_legacy_have_pos = true;
    }
    s->mouse_synth_active = true;
    s->mouse_synth_x = x;
    s->mouse_synth_y = y;
    s->mouse_synth_left = left;
    s->mouse_synth_right = right;
    printf("mouse: x=%d y=%d left=%d right=%d\n", x, y, left ? 1 : 0, right ? 1 : 0);
    return true;
}

static bool um6578_mousestate_command(Um6578State *s, const char *text) {
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "mousestate", 10) != 0 || (p[10] && p[10] != ' ' && p[10] != '\t'))
        return false;
    p += 10;
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return false;

    printf("mouse-state: host=%s x=%d y=%d left=%d right=%d stream=%d q=%u/%u\n",
           s->mouse_synth_active ? "synthetic" : "display",
           s->mouse_synth_x, s->mouse_synth_y,
           s->mouse_synth_left ? 1 : 0, s->mouse_synth_right ? 1 : 0,
           s->mouse_stream_enabled ? 1 : 0,
           (unsigned)s->mouse_qhead, (unsigned)s->mouse_qtail);
    printf("mouse-state: z20=%02X z21=%02X bounds x=%02X..%02X y=%02X..%02X pos=%02X,%02X parsed=%02X/%02X/%02X last=%02X/%02X changed=%02X hist=%02X idx=%02X raw=%02X stage=%02X busy=%02X flag=%02X\n",
           s->ram_lo[0x20], s->ram_lo[0x21],
           s->ram_lo[0x28], s->ram_lo[0x2a],
           s->ram_lo[0x29], s->ram_lo[0x2b],
           s->ram_lo[0x2c], s->ram_lo[0x2d],
           s->ram_lo[0x31], s->ram_lo[0x32], s->ram_lo[0x33],
           s->ram_lo[0x2f], s->ram_lo[0x60],
           s->ram_lo[0x2e], s->ram_lo[0x68], s->ram_lo[0x67],
           s->ram_lo[0x35], s->ram_lo[0x37], s->ram_lo[0x36], s->ram_lo[0x30]);
    return true;
}

static bool um6578_sendkey_command(Um6578State *s, const char *text) {
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "sendkey", 7) != 0 || (p[7] && p[7] != ' ' && p[7] != '\t'))
        return false;
    p += 7;
    while (*p == ' ' || *p == '\t') p++;

    uint32_t btn = 0;
    if      (strncasecmp(p, "a",      1) == 0 && (p[1] < 'a' || p[1] > 'z')) btn = GEMU_ACTION(UM6578_ACT_A);
    else if (strncasecmp(p, "b",      1) == 0 && (p[1] < 'a' || p[1] > 'z')) btn = GEMU_ACTION(UM6578_ACT_B);
    else if (strncasecmp(p, "start",  5) == 0) btn = GEMU_ACTION(UM6578_ACT_START);
    else if (strncasecmp(p, "select", 6) == 0) btn = GEMU_ACTION(UM6578_ACT_SELECT);
    else if (strncasecmp(p, "up",     2) == 0) btn = GEMU_ACTION(UM6578_ACT_UP);
    else if (strncasecmp(p, "down",   4) == 0) btn = GEMU_ACTION(UM6578_ACT_DOWN);
    else if (strncasecmp(p, "left",   4) == 0) btn = GEMU_ACTION(UM6578_ACT_LEFT);
    else if (strncasecmp(p, "right",  5) == 0) btn = GEMU_ACTION(UM6578_ACT_RIGHT);
    if (!btn) {
        printf("sendkey: unknown button (a b start select up down left right)\n");
        return true;
    }

    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    int frames = (*p >= '1' && *p <= '9') ? (int)strtol(p, NULL, 10) : 5;
    s->input_inject_mask = btn;
    s->input_inject_frames = frames;
    printf("sendkey: holding 0x%08X for %d frame(s)\n", btn, frames);
    return true;
}

/* ── PPU/CPU cycle sync ──────────────────────────────────────────────────── */

static void um6578_sync_ppu(Um6578State *s, uint64_t cpu_cycle) {
    while (s->ppu_synced_cpu_cycle < cpu_cycle) {
        s->ppu_synced_cpu_cycle++;

        timer_maybe_tick(s, false); /* Timer source = CPU M2 */

        ppu_sh6578_tick(&s->ppu);
        if (s->ppu.nmi_pending) {
            s->cpu.nmi = true;
            s->ppu.nmi_pending = false;
        }
        if (s->ppu.scanline_tick) {
            s->ppu.scanline_tick = false;
            timer_maybe_tick(s, true); /* Timer source = Scanline */
        }
        if (s->ppu.dirty)
            return;
    }
}

/* ── Create / destroy ────────────────────────────────────────────────────── */

Um6578State *um6578_create(const MosConfig *cfg) {
    Um6578State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;
    const char *trace_io = getenv("GEMU_UM6578_TRACE_IO");
    s->trace_io = trace_io != NULL;
    s->trace_4016 = trace_io && (strcmp(trace_io, "4016") == 0 || strcmp(trace_io, "all") == 0);
    s->trace_timer = trace_io && strcmp(trace_io, "all") == 0;
    s->trace_mouse_state = getenv("GEMU_UM6578_TRACE_MOUSE") != NULL;

    const char *rom_path = NULL;
    for (int i = 0; i < cfg->n_roms; i++) {
        if (cfg->roms[i].region && !strcmp(cfg->roms[i].region, "maincpu")) {
            rom_path = cfg->roms[i].path;
            break;
        }
    }
    if (!rom_path) {
        fprintf(stderr, "um6578: missing ROM — use -rom roms/um6578\n");
        free(s);
        return NULL;
    }

    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "um6578: cannot open '%s'\n", rom_path);
        free(s);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || (uint32_t)sz > UM6578_ROM_MAX) {
        fprintf(stderr, "um6578: '%s' has invalid size %ld\n", rom_path, sz);
        fclose(f);
        free(s);
        return NULL;
    }
    s->rom = malloc((size_t)sz);
    if (!s->rom || fread(s->rom, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "um6578: read error on '%s'\n", rom_path);
        fclose(f);
        free(s->rom);
        free(s);
        return NULL;
    }
    fclose(f);
    s->rom_size = (uint32_t)sz;

    ppu_sh6578_init(&s->ppu, UM6578_CPU_HZ, UM6578_REFRESH_HZ, UM6578_LINES_TOTAL, UM6578_VBLANK_LINE);

    mos6502_init(&s->cpu);
    s->cpu.mem_read  = um6578_read;
    s->cpu.mem_write = um6578_write;
    s->cpu.mem_ud    = s;
    /* real M6502 core on this hardware — decimal mode stays enabled
     * (decimal_disable defaults false), unlike the NES's 2A03. */

    s->monitor = gemu_monitor_create();
    if (!s->monitor) { free(s->rom); free(s); return NULL; }
    gemu_monitor_set_screendump_cb(s->monitor, um6578_screendump, s);

    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr, SH6578_WIDTH, SH6578_HEIGHT);
        if (s->vnc)
            gemu_vnc_set_palette(s->vnc, rp2c02_palette_rgb, 64);
        else
            fprintf(stderr, "um6578: failed to start VNC at %s\n", cfg->vnc_addr);
    }
    gemu_monitor_set_vnc(s->monitor, s->vnc);

    if (cfg->sound == MOS_SOUND_2A03) {
        if (!apu2a03_init(&s->apu, (uint32_t)(UM6578_CPU_HZ + 0.5)))
            fprintf(stderr, "um6578: APU audio init failed (continuing silently)\n");
        s->apu.mem_read = um6578_apu_mem_read;
        s->apu.mem_ud   = s;
    }

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        /* Same physical device as NES's standard controller — share its
         * gemu.ini section (and the "nes-controller" heading shown at the
         * top of the Tab key-binding overlay) rather than inventing a
         * separate, machine-specific one. */
        bool has_pad = cfg->n_ports > 0 && cfg->ports[0] == NES_DEVICE_CONTROLLER;
        bool has_mouse = cfg->n_ports > 0 && cfg->ports[0] == NES_DEVICE_MOUSE;
        s->display = gemu_display_create(cfg->display_type, &(GemuDisplayConfig){
            .title       = "GEMU",
            .fb_width    = SH6578_WIDTH,
            .fb_height   = SH6578_HEIGHT,
            .scale       = cfg->display_scale,
            .renderer    = cfg->display_renderer,
            .actions     = um6578_actions,
            .n_actions   = UM6578_NUM_ACTIONS,
            .ini_section = has_pad ? "nes-controller" : "um6578",
            .capture_pointer = has_mouse,
        });
    }

    for (int i = 0; i < 8; i++) s->bankswitch[i] = (uint8_t)i;
    mos6502_reset(&s->cpu);
    return s;
}

void um6578_destroy(Um6578State *s) {
    if (!s) return;
    apu2a03_destroy(&s->apu);
    gemu_monitor_destroy(s->monitor);
    gemu_display_destroy(s->display);
    gemu_vnc_destroy(s->vnc);
    free(s->rom);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

void um6578_run(Um6578State *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        uint32_t held = 0;
        if (s->display) {
            held = gemu_display_poll(s->display);
            if (gemu_display_should_quit(s->display)) break;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                for (int i = 0; i < 8; i++) s->bankswitch[i] = (uint8_t)i;
                mos6502_reset(&s->cpu);
            }
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { quit = true; break; }
            } else if (cmd == GEMU_MON_RESET) {
                for (int i = 0; i < 8; i++) s->bankswitch[i] = (uint8_t)i;
                mos6502_reset(&s->cpu);
            } else if (cmd == GEMU_MON_CUSTOM) {
                const char *text = gemu_monitor_command_text(s->monitor);
                if (!um6578_mouse_command(s, text) &&
                    !um6578_mousestate_command(s, text) &&
                    !um6578_sendkey_command(s, text))
                    gemu_monitor_unknown_command(s->monitor);
            }
        }
        if (quit) break;
        if (s->display) gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));

        um6578_handle_keys(s, held);
        um6578_mouse_frame_tick(s);

        if (!gemu_monitor_is_paused(s->monitor)) {
            s->ppu.dirty = false;
            while (!s->ppu.dirty) {
                if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) break;
                uint64_t prev = s->cpu.cycle_count;
                mos6502_step(&s->cpu);
                uint64_t delta = s->cpu.cycle_count - prev;
                for (uint64_t i = 0; i < delta; i++)
                    if (s->apu.audio_dev) apu2a03_tick(&s->apu);
                um6578_sync_ppu(s, s->cpu.cycle_count);
                if (gemu_monitor_is_paused(s->monitor)) break;
            }
            apu2a03_flush(&s->apu);
            s->frame++;
        }

        if (s->display)
            gemu_display_render(s->display, s->ppu.pixels_argb, SH6578_WIDTH, SH6578_HEIGHT);
        if (s->vnc)
            gemu_vnc_update(s->vnc, s->ppu.pixels, SH6578_WIDTH, SH6578_HEIGHT);

        Uint32 dt = SDL_GetTicks() - t0;
        Uint32 frame_ms = 1000u / 60u;
        if (dt < frame_ms) SDL_Delay(frame_ms - dt);
    }

    gemu_monitor_stop(s->monitor);
    printf("um6578: %llu frames, %llu cpu cycles\n",
           (unsigned long long)s->frame, (unsigned long long)s->cpu.cycle_count);
}
