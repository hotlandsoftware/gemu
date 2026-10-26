#include "nes_cheats.h"
#include "gemu/util.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char gg_alpha[] = "APZLGITYEOXUKSVN";

/* NES Game Genie bit layout per nesgg.txt:
 *   val[7]=n0[3]  val[6:4]=n1[2:0]  val[3]=n5[3](6ch)/n7[3](8ch)  val[2:0]=n0[2:0]
 *   addr[14:12]=n3[2:0]  addr[11]=n4[3]   addr[10:8]=n5[2:0]
 *   addr[7]=n1[3]  addr[6:4]=n2[2:0]  addr[3]=n3[3]  addr[2:0]=n4[2:0]
 *   cmp[7]=n6[3]  cmp[6:4]=n7[2:0]  cmp[3]=n5[3]  cmp[2:0]=n6[2:0]  (8-char)
 * n2[3] says which length the code is. */
bool nes_gg_decode(const char *code, uint16_t *addr, uint8_t *val, uint8_t *cmp, bool *has_cmp) {
    int len = (int)strlen(code);
    if (len != 6 && len != 8) return false;
    uint8_t n[8] = {0};
    for (int i = 0; i < len; i++) {
        const char *p = code[i] ? strchr(gg_alpha, toupper((unsigned char)code[i])) : NULL;
        if (!p) return false;
        n[i] = (uint8_t)(p - gg_alpha);
    }
    *addr = 0x8000u
        | ((uint16_t)(n[3] & 7u) << 12)
        | ((uint16_t)(n[4] & 8u) <<  8)
        | ((uint16_t)(n[5] & 7u) <<  8)
        | ((uint16_t)(n[1] & 8u) <<  4)
        | ((uint16_t)(n[2] & 7u) <<  4)
        | ((uint16_t)(n[3] & 8u))
        | ((uint16_t)(n[4] & 7u));
    *has_cmp = (len == 8);
    if (*has_cmp) {
        *val = (uint8_t)(((n[0] & 8u) << 4) | ((n[1] & 7u) << 4) | (n[7] & 8u) | (n[0] & 7u));
        *cmp = (uint8_t)(((n[6] & 8u) << 4) | ((n[7] & 7u) << 4) | (n[5] & 8u) | (n[6] & 7u));
    } else {
        *val = (uint8_t)(((n[0] & 8u) << 4) | ((n[1] & 7u) << 4) | (n[5] & 8u) | (n[0] & 7u));
        *cmp = 0;
    }
    return true;
}

bool nes_gg_encode(uint16_t addr, uint8_t val, int cmp, char out[9]) {
    if (addr < 0x8000u) return false;
    bool eight = cmp >= 0;
    uint8_t c = (uint8_t)(eight ? cmp : 0);
    uint8_t n[8];
    n[0] = (uint8_t)((val & 7u) | ((val >> 4) & 8u));
    n[1] = (uint8_t)(((val >> 4) & 7u) | ((addr >> 4) & 8u));
    n[2] = (uint8_t)(((addr >> 4) & 7u) | (eight ? 8u : 0u));
    n[3] = (uint8_t)(((addr >> 12) & 7u) | (addr & 8u));
    n[4] = (uint8_t)((addr & 7u) | ((addr >> 8) & 8u));
    n[5] = (uint8_t)(((addr >> 8) & 7u) | (eight ? (c & 8u) : (val & 8u)));
    n[6] = (uint8_t)((c & 7u) | ((c >> 4) & 8u));
    n[7] = (uint8_t)(((c >> 4) & 7u) | (val & 8u));
    int len = eight ? 8 : 6;
    for (int i = 0; i < len; i++) out[i] = gg_alpha[n[i]];
    out[len] = '\0';
    return true;
}

/* ── The list and its file ───────────────────────────────────────────────── *
 *
 * <game>.cht holds one cheat per line:  [+|-]CODE name
 * (+ = enabled).  Lines starting with # are comments. */

static void save(NesCheats *c) {
    if (!c->path[0]) return;
    if (c->n == 0) { remove(c->path); return; }
    gemu_ensure_parent_dir(c->path);
    FILE *f = fopen(c->path, "w");
    if (!f) return;
    fprintf(f, "# GEMU Game Genie codes: [+|-]CODE name (+ = on)\n");
    for (int i = 0; i < c->n; i++)
        fprintf(f, "%c%s %s\n", c->list[i].enabled ? '+' : '-', c->list[i].code, c->list[i].name);
    fclose(f);
}

static void changed(NesCheats *c) {
    c->version++;
    save(c);
    if (c->changed) c->changed(c->ud);
}

void nes_cheats_init(NesCheats *c, const char *path, void (*cb)(void *ud), void *ud) {
    memset(c, 0, sizeof(*c));
    snprintf(c->path, sizeof(c->path), "%s", path ? path : "");
    FILE *f = c->path[0] ? fopen(c->path, "r") : NULL;
    char line[256];
    while (f && fgets(line, sizeof(line), f) && c->n < NES_CHEATS_MAX) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] != '+' && line[0] != '-') continue;
        NesCheat *e = &c->list[c->n];
        size_t len = strcspn(line + 1, " \t");
        if (len != 6 && len != 8) continue;
        memcpy(e->code, line + 1, len);
        e->code[len] = '\0';
        uint16_t a; uint8_t v, k; bool h;
        if (!nes_gg_decode(e->code, &a, &v, &k, &h)) continue;
        for (char *p = e->code; *p; p++) *p = (char)toupper((unsigned char)*p);
        const char *name = line + 1 + len;
        while (*name == ' ' || *name == '\t') name++;
        snprintf(e->name, sizeof(e->name), "%s", name);
        e->enabled = line[0] == '+';
        c->n++;
    }
    if (f) fclose(f);
    c->changed = cb;
    c->ud = ud;
    c->version = 1;
}

int nes_cheats_find(const NesCheats *c, const char *code) {
    for (int i = 0; i < c->n; i++)
        if (strcasecmp(c->list[i].code, code) == 0) return i;
    return -1;
}

/* Upper-case copy of a valid code, or false with a reason. */
static bool check_code(const NesCheats *c, const char *code, int self, char out[9],
                       char *err, size_t err_len) {
    uint16_t a; uint8_t v, k; bool h;
    if (!nes_gg_decode(code, &a, &v, &k, &h)) {
        snprintf(err, err_len, "'%s' isn't a Game Genie code (6 or 8 of APZLGITYEOXUKSVN)", code);
        return false;
    }
    int dup = nes_cheats_find(c, code);
    if (dup >= 0 && dup != self) {
        snprintf(err, err_len, "%s is already in the list", code);
        return false;
    }
    size_t i = 0;
    for (; code[i] && i < 8; i++) out[i] = (char)toupper((unsigned char)code[i]);
    out[i] = '\0';
    return true;
}

bool nes_cheats_add(NesCheats *c, const char *code, const char *name, bool enabled,
                    char *err, size_t err_len) {
    char up[9];
    if (!check_code(c, code, -1, up, err, err_len)) return false;
    if (c->n >= NES_CHEATS_MAX) {
        snprintf(err, err_len, "the list is full (%d codes)", NES_CHEATS_MAX);
        return false;
    }
    NesCheat *e = &c->list[c->n++];
    memcpy(e->code, up, sizeof(e->code));
    snprintf(e->name, sizeof(e->name), "%s", name ? name : "");
    e->enabled = enabled;
    changed(c);
    return true;
}

bool nes_cheats_update(NesCheats *c, int i, const char *code, const char *name,
                       char *err, size_t err_len) {
    if (i < 0 || i >= c->n) { snprintf(err, err_len, "no code selected"); return false; }
    char up[9];
    if (!check_code(c, code, i, up, err, err_len)) return false;
    memcpy(c->list[i].code, up, sizeof(up));
    snprintf(c->list[i].name, sizeof(c->list[i].name), "%s", name ? name : "");
    changed(c);
    return true;
}

void nes_cheats_remove(NesCheats *c, int i) {
    if (i < 0 || i >= c->n) return;
    memmove(&c->list[i], &c->list[i + 1], (size_t)(c->n - i - 1) * sizeof(c->list[0]));
    c->n--;
    changed(c);
}

void nes_cheats_set_enabled(NesCheats *c, int i, bool enabled) {
    if (i < 0 || i >= c->n || c->list[i].enabled == enabled) return;
    c->list[i].enabled = enabled;
    changed(c);
}
