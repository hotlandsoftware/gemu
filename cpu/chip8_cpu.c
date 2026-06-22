#include "chip8.h"
#include "gemu/tcg.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Decoder ──────────────────────────────────────────────────────────────── */

Chip8Insn chip8_decode(uint16_t op) {
    Chip8Insn ins = {
        .x   = (op >> 8) & 0xF,
        .y   = (op >> 4) & 0xF,
        .n   =  op       & 0xF,
        .kk  =  op       & 0xFF,
        .nnn =  op       & 0xFFF,
        .type = C8_INVALID,
    };

    switch (op >> 12) {
    case 0x0:
        if      (op == 0x00E0) ins.type = C8_CLS;
        else if (op == 0x00EE) ins.type = C8_RET;
        break;
    case 0x1: ins.type = C8_JP_NNN;   break;
    case 0x2: ins.type = C8_CALL_NNN; break;
    case 0x3: ins.type = C8_SE_VX_KK; break;
    case 0x4: ins.type = C8_SNE_VX_KK; break;
    case 0x5:
        if (ins.n == 0) ins.type = C8_SE_VX_VY;
        break;
    case 0x6: ins.type = C8_LD_VX_KK;  break;
    case 0x7: ins.type = C8_ADD_VX_KK; break;
    case 0x8:
        switch (ins.n) {
        case 0x0: ins.type = C8_LD_VX_VY;   break;
        case 0x1: ins.type = C8_OR_VX_VY;   break;
        case 0x2: ins.type = C8_AND_VX_VY;  break;
        case 0x3: ins.type = C8_XOR_VX_VY;  break;
        case 0x4: ins.type = C8_ADD_VX_VY;  break;
        case 0x5: ins.type = C8_SUB_VX_VY;  break;
        case 0x6: ins.type = C8_SHR_VX;     break;
        case 0x7: ins.type = C8_SUBN_VX_VY; break;
        case 0xE: ins.type = C8_SHL_VX;     break;
        }
        break;
    case 0x9:
        if (ins.n == 0) ins.type = C8_SNE_VX_VY;
        break;
    case 0xA: ins.type = C8_LD_I_NNN;   break;
    case 0xB: ins.type = C8_JP_V0_NNN;  break;
    case 0xC: ins.type = C8_RND_VX_KK;  break;
    case 0xD: ins.type = C8_DRW_VX_VY_N; break;
    case 0xE:
        if      (ins.kk == 0x9E) ins.type = C8_SKP_VX;
        else if (ins.kk == 0xA1) ins.type = C8_SKNP_VX;
        break;
    case 0xF:
        switch (ins.kk) {
        case 0x07: ins.type = C8_LD_VX_DT; break;
        case 0x0A: ins.type = C8_LD_VX_K;  break;
        case 0x15: ins.type = C8_LD_DT_VX; break;
        case 0x18: ins.type = C8_LD_ST_VX; break;
        case 0x1E: ins.type = C8_ADD_I_VX; break;
        case 0x29: ins.type = C8_LD_F_VX;  break;
        case 0x33: ins.type = C8_LD_B_VX;  break;
        case 0x55: ins.type = C8_LD_I_VX;  break;
        case 0x65: ins.type = C8_LD_VX_I;  break;
        }
        break;
    }
    return ins;
}

/* ── Translation ──────────────────────────────────────────────────────────── */

GemuTb *chip8_translate_block(Chip8State *s, uint16_t pc) {
    Chip8Insn buf[GEMU_TB_MAX_INSNS];
    uint32_t  n    = 0;
    uint16_t  addr = pc;

    while (n < GEMU_TB_MAX_INSNS && addr + 1 < CHIP8_MEM_SIZE) {
        uint16_t raw = (uint16_t)((s->mem[addr] << 8) | s->mem[addr + 1]);
        buf[n] = chip8_decode(raw);
        n++;
        if (chip8_insn_is_terminal(buf[n - 1].type)) break;
        addr += 2;
    }

    Chip8Insn *insns = malloc(n * sizeof(*insns));
    if (!insns) return NULL;
    memcpy(insns, buf, n * sizeof(*insns));

    s->tb_misses++;
    return gemu_tb_insert(&s->tb_cache, pc, n * 2, n, insns);
}

/* ── Executor ─────────────────────────────────────────────────────────────── */

static inline void exec_insn(Chip8State *s, const Chip8Insn *ins) {  /* used by execute_tb */
    uint8_t *V = s->V;

    switch (ins->type) {

    /* ── Control ──────────────────────────────────────────────────────── */
    case C8_CLS:
        memset(s->vram, 0, sizeof(s->vram));
        s->draw_flag = true;
        s->PC += 2;
        break;
    case C8_RET:
        s->PC = s->stack[--s->SP];
        break;
    case C8_JP_NNN:
        s->PC = ins->nnn;
        break;
    case C8_CALL_NNN:
        s->stack[s->SP++] = s->PC + 2;
        s->PC = ins->nnn;
        break;
    case C8_JP_V0_NNN:
        s->PC = ins->nnn + V[0];
        break;

    /* ── Conditional skip ─────────────────────────────────────────────── */
    case C8_SE_VX_KK:
        s->PC += (V[ins->x] == ins->kk) ? 4 : 2;
        break;
    case C8_SNE_VX_KK:
        s->PC += (V[ins->x] != ins->kk) ? 4 : 2;
        break;
    case C8_SE_VX_VY:
        s->PC += (V[ins->x] == V[ins->y]) ? 4 : 2;
        break;
    case C8_SNE_VX_VY:
        s->PC += (V[ins->x] != V[ins->y]) ? 4 : 2;
        break;
    case C8_SKP_VX:
        s->PC += s->keys[V[ins->x] & 0xF] ? 4 : 2;
        break;
    case C8_SKNP_VX:
        s->PC += s->keys[V[ins->x] & 0xF] ? 2 : 4;
        break;

    /* ── Register load / ALU ──────────────────────────────────────────── */
    case C8_LD_VX_KK:
        V[ins->x] = ins->kk;
        s->PC += 2;
        break;
    case C8_ADD_VX_KK:
        V[ins->x] = (uint8_t)(V[ins->x] + ins->kk);
        s->PC += 2;
        break;
    case C8_LD_VX_VY:
        V[ins->x] = V[ins->y];
        s->PC += 2;
        break;
    case C8_OR_VX_VY:
        V[ins->x] |= V[ins->y];
        s->PC += 2;
        break;
    case C8_AND_VX_VY:
        V[ins->x] &= V[ins->y];
        s->PC += 2;
        break;
    case C8_XOR_VX_VY:
        V[ins->x] ^= V[ins->y];
        s->PC += 2;
        break;
    case C8_ADD_VX_VY: {
        uint16_t sum = (uint16_t)V[ins->x] + V[ins->y];
        uint8_t  vf  = sum > 0xFF ? 1 : 0;
        V[ins->x] = (uint8_t)sum;
        V[0xF]    = vf;
        s->PC += 2;
        break;
    }
    case C8_SUB_VX_VY: {
        uint8_t vf = V[ins->x] >= V[ins->y] ? 1 : 0;
        V[ins->x] = (uint8_t)(V[ins->x] - V[ins->y]);
        V[0xF]    = vf;
        s->PC += 2;
        break;
    }
    case C8_SUBN_VX_VY: {
        uint8_t vf = V[ins->y] >= V[ins->x] ? 1 : 0;
        V[ins->x] = (uint8_t)(V[ins->y] - V[ins->x]);
        V[0xF]    = vf;
        s->PC += 2;
        break;
    }
    case C8_SHR_VX: {
        uint8_t lsb = V[ins->x] & 0x1;
        V[ins->x] >>= 1;
        V[0xF]     = lsb;
        s->PC += 2;
        break;
    }
    case C8_SHL_VX: {
        uint8_t msb = (V[ins->x] >> 7) & 0x1;
        V[ins->x] <<= 1;
        V[0xF]     = msb;
        s->PC += 2;
        break;
    }

    /* ── Index / memory ───────────────────────────────────────────────── */
    case C8_LD_I_NNN:
        s->I = ins->nnn;
        s->PC += 2;
        break;
    case C8_ADD_I_VX:
        s->I = (s->I + V[ins->x]) & 0xFFF;
        s->PC += 2;
        break;
    case C8_LD_F_VX:
        s->I = (uint16_t)(CHIP8_FONT_BASE + (V[ins->x] & 0xF) * 5);
        s->PC += 2;
        break;
    case C8_LD_B_VX: {
        uint8_t v = V[ins->x];
        s->mem[s->I]     = v / 100;
        s->mem[s->I + 1] = (v / 10) % 10;
        s->mem[s->I + 2] = v % 10;
        s->PC += 2;
        break;
    }
    case C8_LD_I_VX:
        for (int i = 0; i <= ins->x; i++)
            s->mem[(s->I + i) & 0xFFF] = V[i];
        s->PC += 2;
        break;
    case C8_LD_VX_I:
        for (int i = 0; i <= ins->x; i++)
            V[i] = s->mem[(s->I + i) & 0xFFF];
        s->PC += 2;
        break;

    /* ── Display ──────────────────────────────────────────────────────── */
    case C8_DRW_VX_VY_N: {
        uint8_t xp = V[ins->x] % CHIP8_DISPLAY_W;
        uint8_t yp = V[ins->y] % CHIP8_DISPLAY_H;
        uint8_t vf = 0;

        for (int row = 0; row < ins->n; row++) {
            uint8_t sprite = s->mem[(s->I + row) & 0xFFF];
            int py = (yp + row) % CHIP8_DISPLAY_H;
            for (int col = 0; col < 8; col++) {
                if (!(sprite & (0x80u >> col))) continue;
                int px  = (xp + col) % CHIP8_DISPLAY_W;
                int idx = py * CHIP8_DISPLAY_W + px;
                vf      |= s->vram[idx];
                s->vram[idx] ^= 1;
            }
        }
        V[0xF] = vf;
        s->draw_flag = true;
        s->PC += 2;
        break;
    }

    /* ── Timers ───────────────────────────────────────────────────────── */
    case C8_LD_VX_DT:
        V[ins->x] = s->delay;
        s->PC += 2;
        break;
    case C8_LD_DT_VX:
        s->delay = V[ins->x];
        s->PC += 2;
        break;
    case C8_LD_ST_VX:
        s->sound = V[ins->x];
        s->PC += 2;
        break;

    /* ── Misc ─────────────────────────────────────────────────────────── */
    case C8_RND_VX_KK: {
        /* Use xorshift32 for speed over rand() */
        static uint32_t rng = 0xDEADBEEF;
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        V[ins->x] = (uint8_t)(rng & 0xFF) & ins->kk;
        s->PC += 2;
        break;
    }
    case C8_LD_VX_K:
        /* Stall: don't advance PC. The main loop detects wait_key and handles
         * the key-press edge, then sets PC += 2 and V[x] = key. */
        s->wait_key = true;
        s->wait_reg = ins->x;
        break;

    case C8_INVALID:
    default:
        /* Skip unknown opcodes silently */
        s->PC += 2;
        break;
    }
}

void chip8_execute_tb(Chip8State *s, GemuTb *tb) {
    const Chip8Insn *insns = (const Chip8Insn *)tb->insns;
    tb->exec_count++;
    s->tb_hits++;
    s->insn_count += tb->n_insns;

    for (uint32_t i = 0; i < tb->n_insns; i++) {
        exec_insn(s, &insns[i]);
        if (s->wait_key) break;
    }
}

void chip8_exec_single(Chip8State *s) {
    if (s->PC + 1 >= CHIP8_MEM_SIZE) {
        printf("step: PC out of bounds (0x%04X)\n", s->PC);
        return;
    }
    uint16_t raw = (uint16_t)((s->mem[s->PC] << 8) | s->mem[s->PC + 1]);
    printf("step @ PC=0x%04X  [%04X]\n", s->PC, raw);
    Chip8Insn ins = chip8_decode(raw);
    exec_insn(s, &ins);
    s->insn_count++;
}
