#include "tape.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Bit-stream builder ──────────────────────────────────────────────────── */

static bool bits_push(uint8_t **bits, size_t *n, size_t *cap, uint8_t val) {
    if (*n >= *cap) {
        size_t new_cap = *cap ? *cap * 2 : 4096;
        uint8_t *tmp = realloc(*bits, new_cap);
        if (!tmp) return false;
        *bits = tmp;
        *cap  = new_cap;
    }
    (*bits)[(*n)++] = val;
    return true;
}

/* Append 8 bits of a byte, MSB first. */
static bool bits_push_byte(uint8_t **bits, size_t *n, size_t *cap, uint8_t byte) {
    for (int b = 7; b >= 0; b--)
        if (!bits_push(bits, n, cap, (byte >> b) & 1u))
            return false;
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void vip_tape_init(VipTape *t) {
    memset(t, 0, sizeof(*t));
}

void vip_tape_destroy(VipTape *t) {
    free(t->bits);
    t->bits = NULL;
    t->n_bits = 0;
}

bool vip_tape_load(VipTape *t, const char *path, uint16_t load_addr,
                   uint64_t start_cycle)
{
    /* Read program data */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gemu-rca tape: cannot open '%s'\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 65536) {
        fprintf(stderr, "gemu-rca tape: file '%s' empty or too large (%ld bytes)\n",
                path, fsize);
        fclose(f);
        return false;
    }

    uint8_t *prog = malloc((size_t)fsize);
    if (!prog) { fclose(f); return false; }
    if ((long)fread(prog, 1, (size_t)fsize, f) != fsize) {
        fprintf(stderr, "gemu-rca tape: read error on '%s'\n", path);
        free(prog);
        fclose(f);
        return false;
    }
    fclose(f);

    /* Build bit stream */
    uint8_t *bits = NULL;
    size_t n = 0, cap = 0;

    /* Leader: 512 zero-bits (64 × 0x00).  The VIP monitor synchronises to the
     * stream of fast transitions before reading actual data. */
    for (int i = 0; i < 512; i++)
        if (!bits_push(&bits, &n, &cap, 0)) goto oom;

    /* Header: start-address and end-address, big-endian */
    uint16_t end_addr = (uint16_t)(load_addr + (uint16_t)fsize - 1u);
    uint8_t hdr[4] = {
        (uint8_t)(load_addr >> 8), (uint8_t)(load_addr & 0xFF),
        (uint8_t)(end_addr  >> 8), (uint8_t)(end_addr  & 0xFF),
    };
    for (int i = 0; i < 4; i++)
        if (!bits_push_byte(&bits, &n, &cap, hdr[i])) goto oom;

    /* Data bytes */
    uint8_t cksum = 0;
    for (long i = 0; i < fsize; i++) {
        if (!bits_push_byte(&bits, &n, &cap, prog[i])) goto oom;
        cksum ^= prog[i];
    }
    free(prog);
    prog = NULL;

    /* Checksum (XOR of all data bytes) */
    if (!bits_push_byte(&bits, &n, &cap, cksum)) goto oom;

    /* Tail silence: 64 zero-bits so the monitor can finish reading the last byte */
    for (int i = 0; i < 64; i++)
        if (!bits_push(&bits, &n, &cap, 0)) goto oom;

    /* Install */
    vip_tape_eject(t);
    t->bits         = bits;
    t->n_bits       = n;
    t->bit_pos      = 0;
    t->phase        = 0;
    t->ef3          = 0;
    t->next_toggle  = start_cycle + VIP_TAPE_HP_ZERO;
    t->playing      = true;
    snprintf(t->path, VIP_TAPE_PATH_MAX, "%s", path);

    printf("gemu-rca tape: loaded '%s'  addr=0x%04X  %ld bytes  %zu bits\n",
           path, load_addr, fsize, n);
    return true;

oom:
    free(bits);
    free(prog);
    fprintf(stderr, "gemu-rca tape: out of memory building bit stream\n");
    return false;
}

void vip_tape_eject(VipTape *t) {
    free(t->bits);
    t->bits    = NULL;
    t->n_bits  = 0;
    t->bit_pos = 0;
    t->phase   = 0;
    t->ef3     = 0;
    t->playing = false;
    t->path[0] = '\0';
}

int vip_tape_ef3(VipTape *t, uint64_t cycle_count) {
    if (!t->playing)
        return 0;

    /* Advance state while scheduled toggle is due */
    while (t->playing && cycle_count >= t->next_toggle) {
        /* Toggle EF3 */
        t->ef3 ^= 1;

        /* Determine half-period for the CURRENT bit */
        size_t idx  = t->bit_pos < t->n_bits ? t->bit_pos : t->n_bits - 1;
        unsigned hp = t->bits[idx] ? VIP_TAPE_HP_ONE : VIP_TAPE_HP_ZERO;
        t->next_toggle += hp;

        /* After the second half of this bit, advance to the next bit */
        t->phase ^= 1;
        if (t->phase == 0) {
            t->bit_pos++;
            if (t->bit_pos >= t->n_bits) {
                t->playing = false;
                t->ef3     = 0;
                printf("gemu-rca tape: end of tape '%s'\n", t->path);
            }
        }
    }

    return t->ef3;
}
