/* octo.c - Octo (.8o) assembler for gemu-chip8 */

#include "octo.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── limits ──────────────────────────────────────────────────────────────── */
#define OCTO_ROM_BASE   0x200
#define MAX_TOKS        32768
#define MAX_LABELS      1024
#define MAX_FWD         4096
#define MAX_ALIAS       256
#define MAX_CONST       256
#define MAX_MACROS      64
#define STACK_SZ        64
#define MACRO_DEPTH     8

/* ── symbol tables ───────────────────────────────────────────────────────── */
typedef struct { char name[64]; int addr; } OLabel;
typedef struct { char name[64]; int reg;  } OAlias;
typedef struct { char name[64]; int val;  } OConst;
typedef struct { char name[64]; int off;  } OFwd;   /* rom byte offset to patch */
typedef struct { int jp_off; }               OIfFr;

typedef struct {
    char  name[64];
    char  params[8][64];
    int   nparams;
    char **body;
    int   nbody;
} OMacro;

/* Saved token stream for macro expansion */
typedef struct { char **toks; int ntoks; int ti; } OTokSave;

typedef struct {
    char  **toks;
    int     ntoks;
    int     ti;
    OTokSave saves[MACRO_DEPTH];
    int      ndepth;

    uint8_t *rom;
    int      pc;
    int      rom_max;

    OLabel  labels[MAX_LABELS]; int nlabels;
    OAlias  alias [MAX_ALIAS];  int nalias;
    OConst  consts[MAX_CONST];  int nconsts;
    OFwd    fwds  [MAX_FWD];    int nfwds;
    OMacro  macros[MAX_MACROS]; int nmacros;

    OIfFr   if_stack  [STACK_SZ]; int if_sp;
    int     loop_stack[STACK_SZ]; int loop_sp;

    char    errmsg[256];
    bool    error;
} OAsm;

/* ── error ───────────────────────────────────────────────────────────────── */
#define ERR(a, ...) do { \
    if (!(a)->error) { snprintf((a)->errmsg, 256, __VA_ARGS__); (a)->error = true; } \
} while(0)

/* ── tokenizer ───────────────────────────────────────────────────────────── */

static char **tokenize(const char *src, int *out_n)
{
    char **toks = malloc(MAX_TOKS * sizeof(char *));
    if (!toks) return NULL;
    int n = 0;
    const char *p = src;
    while (*p && n < MAX_TOKS) {
        /* skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        /* comment */
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        /* self-delimiting single chars */
        if (*p == '{' || *p == '}') {
            char *t = malloc(2); t[0] = *p++; t[1] = '\0';
            toks[n++] = t;
            continue;
        }
        /* normal token: read until whitespace or { or } */
        const char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != '{' && *p != '}' && *p != '#')
            p++;
        int len = (int)(p - start);
        char *t = malloc((size_t)len + 1);
        memcpy(t, start, (size_t)len);
        t[len] = '\0';
        toks[n++] = t;
    }
    *out_n = n;
    return toks;
}

static void free_toks(char **toks, int n)
{
    for (int i = 0; i < n; i++) free(toks[i]);
    free(toks);
}

/* ── token stream ────────────────────────────────────────────────────────── */

static const char *peek(OAsm *a)
{
    if (a->ti >= a->ntoks) return NULL;
    return a->toks[a->ti];
}

static const char *consume(OAsm *a)
{
    if (a->ti >= a->ntoks) return NULL;
    const char *t = a->toks[a->ti++];
    /* pop macro save frame when exhausted */
    while (a->ti >= a->ntoks && a->ndepth > 0) {
        a->ndepth--;
        OTokSave *s = &a->saves[a->ndepth];
        /* free the expanded body - caller's responsibility by convention,
           so just restore the saved stream; expanded body was heap-allocated
           by expand_macro and must be freed after use. */
        free_toks(a->toks, a->ntoks);
        a->toks  = s->toks;
        a->ntoks = s->ntoks;
        a->ti    = s->ti;
    }
    return t;
}

static bool expect(OAsm *a, const char *s)
{
    const char *t = consume(a);
    if (!t || strcmp(t, s) != 0) {
        ERR(a, "expected '%s', got '%s'", s, t ? t : "<eof>");
        return false;
    }
    return true;
}

/* ── emit ────────────────────────────────────────────────────────────────── */

static void emit_b(OAsm *a, uint8_t b)
{
    int off = a->pc - OCTO_ROM_BASE;
    if (off < 0 || off >= a->rom_max) { ERR(a, "ROM overflow at 0x%X", a->pc); return; }
    a->rom[off] = b;
    a->pc++;
}

static void emit2(OAsm *a, uint16_t op)
{
    emit_b(a, (uint8_t)(op >> 8));
    emit_b(a, (uint8_t)(op & 0xFF));
}

static void patch_jp(OAsm *a, int rom_off)
{
    uint16_t addr = (uint16_t)a->pc;
    a->rom[rom_off]   = 0x10 | (uint8_t)((addr >> 8) & 0xF);
    a->rom[rom_off+1] = (uint8_t)(addr & 0xFF);
}

/* ── register parsing ────────────────────────────────────────────────────── */

static int parse_reg(OAsm *a, const char *tok)
{
    if (!tok) return -1;
    if (tok[0] == 'v' || tok[0] == 'V') {
        const char *p = tok + 1;
        if (p[0] == '1' && p[1] >= '0' && p[1] <= '5' && p[2] == '\0')
            return 10 + (p[1] - '0');
        if (isxdigit((unsigned char)p[0]) && p[1] == '\0') {
            int d = (p[0] >= '0' && p[0] <= '9') ? p[0] - '0' : tolower((unsigned char)p[0]) - 'a' + 10;
            return d;
        }
    }
    for (int i = 0; i < a->nalias; i++)
        if (strcmp(a->alias[i].name, tok) == 0) return a->alias[i].reg;
    return -1;
}

/* ── value parsing ───────────────────────────────────────────────────────── */

static int parse_val(OAsm *a, const char *tok)
{
    if (!tok) { ERR(a, "expected value"); return 0; }
    /* hex */
    if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X'))
        return (int)strtol(tok + 2, NULL, 16);
    /* binary */
    if (tok[0] == '0' && (tok[1] == 'b' || tok[1] == 'B'))
        return (int)strtol(tok + 2, NULL, 2);
    /* decimal (including negative) */
    if (isdigit((unsigned char)tok[0]) ||
        (tok[0] == '-' && isdigit((unsigned char)tok[1])))
        return (int)strtol(tok, NULL, 10);
    /* named constant */
    for (int i = 0; i < a->nconsts; i++)
        if (strcmp(a->consts[i].name, tok) == 0) return a->consts[i].val;
    ERR(a, "unknown constant '%s'", tok);
    return 0;
}

/* ── label helpers ───────────────────────────────────────────────────────── */

static void define_label(OAsm *a, const char *name)
{
    if (a->nlabels >= MAX_LABELS) { ERR(a, "too many labels"); return; }
    for (int i = 0; i < a->nlabels; i++)
        if (strcmp(a->labels[i].name, name) == 0) {
            a->labels[i].addr = a->pc; return;
        }
    strncpy(a->labels[a->nlabels].name, name, 63);
    a->labels[a->nlabels++].addr = a->pc;
}

/* emit JP/CALL NNN, with forward-ref support */
static void emit_addr_op(OAsm *a, uint8_t hi_nibble, const char *name)
{
    /* try to resolve now */
    for (int i = 0; i < a->nlabels; i++) {
        if (strcmp(a->labels[i].name, name) == 0) {
            emit2(a, ((uint16_t)hi_nibble << 12) | (uint16_t)(a->labels[i].addr & 0xFFF));
            return;
        }
    }
    /* try as a numeric address */
    if (isdigit((unsigned char)name[0]) || (name[0] == '0' && name[1] == 'x')) {
        int addr = parse_val(a, name);
        emit2(a, ((uint16_t)hi_nibble << 12) | (uint16_t)(addr & 0xFFF));
        return;
    }
    /* forward reference */
    if (a->nfwds >= MAX_FWD) { ERR(a, "too many forward refs"); return; }
    OFwd *f = &a->fwds[a->nfwds++];
    strncpy(f->name, name, 63);
    f->off = a->pc - OCTO_ROM_BASE;
    emit2(a, (uint16_t)hi_nibble << 12); /* placeholder */
}

/* ── condition ───────────────────────────────────────────────────────────── */

typedef struct {
    int  reg;
    int  op;     /* 0=eq, 1=neq, 2=key, 3=nkey */
    int  val;
    bool is_reg;
} OCond;

static OCond parse_cond(OAsm *a)
{
    OCond c = {0};
    c.reg = parse_reg(a, consume(a));
    if (c.reg < 0) { ERR(a, "expected register in condition"); return c; }
    const char *op = consume(a);
    if (!op) { ERR(a, "expected condition operator"); return c; }
    if (strcmp(op, "==") == 0) {
        c.op = 0;
        const char *rhs = consume(a);
        int r = parse_reg(a, rhs);
        if (r >= 0) { c.val = r; c.is_reg = true; }
        else c.val = parse_val(a, rhs);
    } else if (strcmp(op, "!=") == 0) {
        c.op = 1;
        const char *rhs = consume(a);
        int r = parse_reg(a, rhs);
        if (r >= 0) { c.val = r; c.is_reg = true; }
        else c.val = parse_val(a, rhs);
    } else if (strcmp(op, "key") == 0) {
        c.op = 2;
    } else if (strcmp(op, "-key") == 0) {
        c.op = 3;
    } else {
        ERR(a, "unknown condition op '%s'", op);
    }
    return c;
}

/*
 * Skip-if-FALSE: executes following instruction when condition is TRUE.
 * Used for "if cond then STMT".
 */
static void emit_skip_false(OAsm *a, OCond c)
{
    switch (c.op) {
    case 0: /* == */
        if (c.is_reg) emit2(a, 0x9000 | ((uint16_t)c.reg << 8) | ((uint16_t)c.val << 4));
        else          emit2(a, 0x4000 | ((uint16_t)c.reg << 8) | (uint16_t)(c.val & 0xFF));
        break;
    case 1: /* != */
        if (c.is_reg) emit2(a, 0x5000 | ((uint16_t)c.reg << 8) | ((uint16_t)c.val << 4));
        else          emit2(a, 0x3000 | ((uint16_t)c.reg << 8) | (uint16_t)(c.val & 0xFF));
        break;
    case 2: /* key  */ emit2(a, 0xE0A1 | ((uint16_t)c.reg << 8)); break;
    case 3: /* -key */ emit2(a, 0xE09E | ((uint16_t)c.reg << 8)); break;
    }
}

/*
 * Skip-if-TRUE: jumps over the "else/end" JP when condition holds,
 * falling into the then-body. Used for "if cond begin".
 */
static void emit_skip_true(OAsm *a, OCond c)
{
    switch (c.op) {
    case 0:
        if (c.is_reg) emit2(a, 0x5000 | ((uint16_t)c.reg << 8) | ((uint16_t)c.val << 4));
        else          emit2(a, 0x3000 | ((uint16_t)c.reg << 8) | (uint16_t)(c.val & 0xFF));
        break;
    case 1:
        if (c.is_reg) emit2(a, 0x9000 | ((uint16_t)c.reg << 8) | ((uint16_t)c.val << 4));
        else          emit2(a, 0x4000 | ((uint16_t)c.reg << 8) | (uint16_t)(c.val & 0xFF));
        break;
    case 2: emit2(a, 0xE09E | ((uint16_t)c.reg << 8)); break;
    case 3: emit2(a, 0xE0A1 | ((uint16_t)c.reg << 8)); break;
    }
}

/* ── macro expansion ─────────────────────────────────────────────────────── */

static void expand_macro(OAsm *a, OMacro *m, char **args)
{
    if (a->ndepth >= MACRO_DEPTH) { ERR(a, "macro recursion limit"); return; }
    /* build substituted body */
    char **exp = malloc((size_t)m->nbody * sizeof(char *));
    if (!exp) { ERR(a, "OOM"); return; }
    for (int i = 0; i < m->nbody; i++) {
        bool found = false;
        for (int p = 0; p < m->nparams; p++) {
            if (strcmp(m->body[i], m->params[p]) == 0) {
                exp[i] = strdup(args[p]); found = true; break;
            }
        }
        if (!found) exp[i] = strdup(m->body[i]);
    }
    /* save current stream */
    OTokSave *sv = &a->saves[a->ndepth++];
    sv->toks  = a->toks;
    sv->ntoks = a->ntoks;
    sv->ti    = a->ti;
    a->toks   = exp;
    a->ntoks  = m->nbody;
    a->ti     = 0;
}

/* ── main statement parser ───────────────────────────────────────────────── */

static void parse_stmt(OAsm *a);

static void parse_stmt(OAsm *a)
{
    if (a->error) return;
    const char *tok = peek(a);
    if (!tok) return;

    /* ── directives ───────────────────────────────────────────────────── */

    if (strcmp(tok, ":") == 0) {
        consume(a);
        const char *name = consume(a);
        if (!name) { ERR(a, "expected label name after ':'"); return; }
        define_label(a, name);
        return;
    }

    if (strcmp(tok, ":alias") == 0) {
        consume(a);
        const char *name = consume(a);
        const char *reg  = consume(a);
        if (!name || !reg) { ERR(a, ":alias: missing arguments"); return; }
        int r = parse_reg(a, reg);
        if (r < 0) { ERR(a, ":alias: '%s' is not a register", reg); return; }
        if (a->nalias >= MAX_ALIAS) { ERR(a, "too many aliases"); return; }
        strncpy(a->alias[a->nalias].name, name, 63);
        a->alias[a->nalias++].reg = r;
        return;
    }

    if (strcmp(tok, ":const") == 0) {
        consume(a);
        const char *name = consume(a);
        const char *val  = consume(a);
        if (!name || !val) { ERR(a, ":const: missing arguments"); return; }
        if (a->nconsts >= MAX_CONST) { ERR(a, "too many constants"); return; }
        strncpy(a->consts[a->nconsts].name, name, 63);
        a->consts[a->nconsts++].val = parse_val(a, val);
        return;
    }

    if (strcmp(tok, ":macro") == 0) {
        consume(a);
        const char *name = consume(a);
        if (!name) { ERR(a, ":macro: missing name"); return; }
        if (a->nmacros >= MAX_MACROS) { ERR(a, "too many macros"); return; }
        OMacro *m = &a->macros[a->nmacros++];
        memset(m, 0, sizeof(*m));
        strncpy(m->name, name, 63);
        /* collect parameters until '{' */
        while (peek(a) && strcmp(peek(a), "{") != 0) {
            if (m->nparams >= 8) { ERR(a, "macro: too many params"); return; }
            strncpy(m->params[m->nparams++], consume(a), 63);
        }
        if (!expect(a, "{")) return;
        /* collect body tokens until '}' */
        int bstart = a->ti, depth = 1;
        int bi = a->ti;
        while (bi < a->ntoks && depth > 0) {
            if (strcmp(a->toks[bi], "{") == 0) depth++;
            else if (strcmp(a->toks[bi], "}") == 0) { depth--; if (!depth) break; }
            bi++;
        }
        m->nbody = bi - bstart;
        m->body  = malloc((size_t)(m->nbody ? m->nbody : 1) * sizeof(char *));
        if (!m->body) { ERR(a, "OOM"); return; }
        for (int i = 0; i < m->nbody; i++)
            m->body[i] = strdup(a->toks[bstart + i]);
        a->ti = bi + 1; /* skip '}' */
        return;
    }

    if (strcmp(tok, ":org") == 0) {
        consume(a);
        a->pc = parse_val(a, consume(a));
        return;
    }

    if (strcmp(tok, ":byte") == 0) {
        consume(a);
        emit_b(a, (uint8_t)(parse_val(a, consume(a)) & 0xFF));
        return;
    }

    if (strcmp(tok, ":word") == 0) {
        consume(a);
        int v = parse_val(a, consume(a));
        emit_b(a, (uint8_t)(v >> 8)); emit_b(a, (uint8_t)(v & 0xFF));
        return;
    }

    /* ── control flow keywords ────────────────────────────────────────── */

    if (strcmp(tok, "return") == 0 || strcmp(tok, ";") == 0) {
        consume(a); emit2(a, 0x00EE); return;
    }
    if (strcmp(tok, "clear") == 0)       { consume(a); emit2(a, 0x00E0); return; }
    if (strcmp(tok, "hires") == 0)       { consume(a); emit2(a, 0x00FF); return; }
    if (strcmp(tok, "lores") == 0)       { consume(a); emit2(a, 0x00FE); return; }
    if (strcmp(tok, "scroll-right") == 0){ consume(a); emit2(a, 0x00FB); return; }
    if (strcmp(tok, "scroll-left") == 0) { consume(a); emit2(a, 0x00FC); return; }
    if (strcmp(tok, "exit") == 0)        { consume(a); emit2(a, 0x00FD); return; }

    if (strcmp(tok, "scroll-down") == 0) {
        consume(a);
        emit2(a, 0x00C0 | (uint16_t)(parse_val(a, consume(a)) & 0xF));
        return;
    }
    if (strcmp(tok, "scroll-up") == 0) {
        consume(a);
        emit2(a, 0x00D0 | (uint16_t)(parse_val(a, consume(a)) & 0xF));
        return;
    }

    if (strcmp(tok, "loop") == 0) {
        consume(a);
        if (a->loop_sp >= STACK_SZ) { ERR(a, "loop stack overflow"); return; }
        a->loop_stack[a->loop_sp++] = a->pc;
        return;
    }

    if (strcmp(tok, "again") == 0) {
        consume(a);
        if (a->loop_sp <= 0) { ERR(a, "'again' without 'loop'"); return; }
        int target = a->loop_stack[--a->loop_sp];
        emit2(a, 0x1000 | (uint16_t)(target & 0xFFF));
        return;
    }

    if (strcmp(tok, "if") == 0) {
        consume(a);
        OCond cond = parse_cond(a);
        if (a->error) return;
        const char *branch = consume(a);
        if (!branch) { ERR(a, "expected 'then' or 'begin'"); return; }

        if (strcmp(branch, "then") == 0) {
            /* emit skip when condition is false; next token is the "then" stmt */
            emit_skip_false(a, cond);
            return;
        }
        if (strcmp(branch, "begin") == 0) {
            /* emit skip when condition is true; then a JP over the body */
            emit_skip_true(a, cond);
            if (a->if_sp >= STACK_SZ) { ERR(a, "if stack overflow"); return; }
            a->if_stack[a->if_sp].jp_off = a->pc - OCTO_ROM_BASE;
            a->if_sp++;
            emit2(a, 0x1000); /* JP placeholder */
            return;
        }
        ERR(a, "expected 'then' or 'begin', got '%s'", branch);
        return;
    }

    if (strcmp(tok, "else") == 0) {
        consume(a);
        if (a->if_sp <= 0) { ERR(a, "'else' without 'if'"); return; }
        OIfFr *fr = &a->if_stack[a->if_sp - 1];
        int end_jp_off = a->pc - OCTO_ROM_BASE;
        emit2(a, 0x1000); /* JP-to-end placeholder */
        patch_jp(a, fr->jp_off); /* patch begin-JP to current PC (else body) */
        fr->jp_off = end_jp_off;
        return;
    }

    if (strcmp(tok, "end") == 0) {
        consume(a);
        if (a->if_sp <= 0) { ERR(a, "'end' without 'if'"); return; }
        patch_jp(a, a->if_stack[--a->if_sp].jp_off);
        return;
    }

    if (strcmp(tok, "jump") == 0) {
        consume(a);
        emit_addr_op(a, 0x1, consume(a));
        return;
    }
    if (strcmp(tok, "jump0") == 0) {
        consume(a);
        emit_addr_op(a, 0xB, consume(a));
        return;
    }

    /* ── sprite ───────────────────────────────────────────────────────── */

    if (strcmp(tok, "sprite") == 0) {
        consume(a);
        int rx = parse_reg(a, consume(a));
        int ry = parse_reg(a, consume(a));
        int n  = parse_val(a, consume(a));
        if (rx < 0 || ry < 0) { ERR(a, "sprite: bad registers"); return; }
        emit2(a, 0xD000 | ((uint16_t)rx << 8) | ((uint16_t)ry << 4) | (uint16_t)(n & 0xF));
        return;
    }

    /* ── I register ───────────────────────────────────────────────────── */

    if (strcmp(tok, "i") == 0) {
        consume(a);
        const char *op = consume(a);
        if (!op) { ERR(a, "expected operator after 'i'"); return; }
        if (strcmp(op, ":=") == 0) {
            const char *rhs = consume(a);
            if (!rhs) { ERR(a, "expected value after 'i :='"); return; }
            if (strcmp(rhs, "hex") == 0) {
                int r = parse_reg(a, consume(a));
                if (r < 0) { ERR(a, "i := hex: bad register"); return; }
                emit2(a, 0xF029 | ((uint16_t)r << 8));
            } else if (strcmp(rhs, "bighex") == 0) {
                int r = parse_reg(a, consume(a));
                if (r < 0) { ERR(a, "i := bighex: bad register"); return; }
                emit2(a, 0xF030 | ((uint16_t)r << 8));
            } else {
                emit_addr_op(a, 0xA, rhs);
            }
        } else if (strcmp(op, "+=") == 0) {
            int r = parse_reg(a, consume(a));
            if (r < 0) { ERR(a, "i +=: expected register"); return; }
            emit2(a, 0xF01E | ((uint16_t)r << 8));
        } else {
            ERR(a, "unknown i operator '%s'", op);
        }
        return;
    }

    /* ── timer / sound assignments ────────────────────────────────────── */

    if (strcmp(tok, "delay") == 0) {
        consume(a);
        if (!expect(a, ":=")) return;
        int r = parse_reg(a, consume(a));
        if (r < 0) { ERR(a, "delay :=: expected register"); return; }
        emit2(a, 0xF015 | ((uint16_t)r << 8));
        return;
    }
    if (strcmp(tok, "buzzer") == 0 || strcmp(tok, "sound") == 0) {
        consume(a);
        if (!expect(a, ":=")) return;
        int r = parse_reg(a, consume(a));
        if (r < 0) { ERR(a, "buzzer :=: expected register"); return; }
        emit2(a, 0xF018 | ((uint16_t)r << 8));
        return;
    }

    /* ── misc ops ─────────────────────────────────────────────────────── */

    if (strcmp(tok, "bcd") == 0) {
        consume(a);
        int r = parse_reg(a, consume(a));
        if (r < 0) { ERR(a, "bcd: bad register"); return; }
        emit2(a, 0xF033 | ((uint16_t)r << 8));
        return;
    }
    if (strcmp(tok, "load") == 0) {
        consume(a);
        int r = parse_reg(a, consume(a));
        if (r < 0) { ERR(a, "load: bad register"); return; }
        emit2(a, 0xF065 | ((uint16_t)r << 8));
        return;
    }
    if (strcmp(tok, "save") == 0) {
        consume(a);
        int r = parse_reg(a, consume(a));
        if (r < 0) { ERR(a, "save: bad register"); return; }
        emit2(a, 0xF055 | ((uint16_t)r << 8));
        return;
    }
    if (strcmp(tok, "saveflags") == 0) {
        consume(a);
        int r = parse_reg(a, consume(a));
        if (r < 0) { ERR(a, "saveflags: bad register"); return; }
        emit2(a, 0xF075 | ((uint16_t)r << 8));
        return;
    }
    if (strcmp(tok, "loadflags") == 0) {
        consume(a);
        int r = parse_reg(a, consume(a));
        if (r < 0) { ERR(a, "loadflags: bad register"); return; }
        emit2(a, 0xF085 | ((uint16_t)r << 8));
        return;
    }

    /* ── register assignment / arithmetic ─────────────────────────────── */

    {
        int lhs = parse_reg(a, tok);
        if (lhs >= 0) {
            consume(a);
            const char *op = consume(a);
            if (!op) { ERR(a, "expected operator after register"); return; }

            if (strcmp(op, ":=") == 0) {
                const char *rhs = consume(a);
                if (!rhs) { ERR(a, "expected rhs"); return; }
                if (strcmp(rhs, "random") == 0) {
                    int mask = parse_val(a, consume(a));
                    emit2(a, 0xC000 | ((uint16_t)lhs << 8) | (uint16_t)(mask & 0xFF));
                } else if (strcmp(rhs, "key") == 0) {
                    emit2(a, 0xF00A | ((uint16_t)lhs << 8));
                } else if (strcmp(rhs, "delay") == 0) {
                    emit2(a, 0xF007 | ((uint16_t)lhs << 8));
                } else {
                    int rr = parse_reg(a, rhs);
                    if (rr >= 0) emit2(a, 0x8000 | ((uint16_t)lhs << 8) | ((uint16_t)rr << 4));
                    else { int v = parse_val(a, rhs); emit2(a, 0x6000 | ((uint16_t)lhs << 8) | (uint16_t)(v & 0xFF)); }
                }
            } else if (strcmp(op, "+=") == 0) {
                const char *rhs = consume(a);
                int rr = parse_reg(a, rhs);
                if (rr >= 0) emit2(a, 0x8004 | ((uint16_t)lhs << 8) | ((uint16_t)rr << 4));
                else { int v = parse_val(a, rhs); emit2(a, 0x7000 | ((uint16_t)lhs << 8) | (uint16_t)(v & 0xFF)); }
            } else if (strcmp(op, "-=") == 0) {
                const char *rhs = consume(a);
                int rr = parse_reg(a, rhs);
                if (rr >= 0) emit2(a, 0x8005 | ((uint16_t)lhs << 8) | ((uint16_t)rr << 4));
                else { int v = parse_val(a, rhs); emit2(a, 0x7000 | ((uint16_t)lhs << 8) | (uint16_t)((-v) & 0xFF)); }
            } else if (strcmp(op, "=-") == 0) {
                int rr = parse_reg(a, consume(a));
                if (rr < 0) { ERR(a, "=- requires register"); return; }
                emit2(a, 0x8007 | ((uint16_t)lhs << 8) | ((uint16_t)rr << 4));
            } else if (strcmp(op, "|=") == 0) {
                int rr = parse_reg(a, consume(a));
                if (rr < 0) { ERR(a, "|= requires register"); return; }
                emit2(a, 0x8001 | ((uint16_t)lhs << 8) | ((uint16_t)rr << 4));
            } else if (strcmp(op, "&=") == 0) {
                int rr = parse_reg(a, consume(a));
                if (rr < 0) { ERR(a, "&= requires register"); return; }
                emit2(a, 0x8002 | ((uint16_t)lhs << 8) | ((uint16_t)rr << 4));
            } else if (strcmp(op, "^=") == 0) {
                int rr = parse_reg(a, consume(a));
                if (rr < 0) { ERR(a, "^= requires register"); return; }
                emit2(a, 0x8003 | ((uint16_t)lhs << 8) | ((uint16_t)rr << 4));
            } else if (strcmp(op, ">>=") == 0) {
                const char *rhs = consume(a);
                int rr = parse_reg(a, rhs); if (rr < 0) rr = lhs;
                emit2(a, 0x8006 | ((uint16_t)lhs << 8) | ((uint16_t)rr << 4));
            } else if (strcmp(op, "<<=") == 0) {
                const char *rhs = consume(a);
                int rr = parse_reg(a, rhs); if (rr < 0) rr = lhs;
                emit2(a, 0x800E | ((uint16_t)lhs << 8) | ((uint16_t)rr << 4));
            } else {
                ERR(a, "unknown register op '%s'", op);
            }
            return;
        }
    }

    /* ── raw data byte ───────────────────────────────────────────────── */

    {
        const char *t = tok;
        bool is_lit = (t[0] == '0' && (t[1] == 'x' || t[1] == 'X' ||
                                        t[1] == 'b' || t[1] == 'B')) ||
                       isdigit((unsigned char)t[0]) ||
                      (t[0] == '-' && isdigit((unsigned char)t[1]));
        if (is_lit) {
            consume(a);
            emit_b(a, (uint8_t)(parse_val(a, t) & 0xFF));
            return;
        }
    }

    /* ── macro call ──────────────────────────────────────────────────── */

    for (int i = 0; i < a->nmacros; i++) {
        if (strcmp(a->macros[i].name, tok) == 0) {
            consume(a);
            OMacro *m = &a->macros[i];
            char *args[8]; int na = 0;
            for (; na < m->nparams; na++) {
                const char *t = consume(a);
                if (!t) { ERR(a, "macro '%s': not enough args", m->name); return; }
                args[na] = (char *)t; /* borrowed from token stream */
            }
            expand_macro(a, m, args);
            return;
        }
    }

    /* ── label call (subroutine) ─────────────────────────────────────── */

    consume(a);
    emit_addr_op(a, 0x2, tok);
}

/* ── forward-reference resolution ───────────────────────────────────────── */

static bool resolve_fwds(OAsm *a)
{
    for (int i = 0; i < a->nfwds; i++) {
        OFwd *f = &a->fwds[i];
        int addr = -1;
        for (int j = 0; j < a->nlabels; j++) {
            if (strcmp(a->labels[j].name, f->name) == 0) {
                addr = a->labels[j].addr; break;
            }
        }
        if (addr < 0) {
            snprintf(a->errmsg, 256, "undefined label '%s'", f->name);
            return false;
        }
        int off = f->off;
        a->rom[off]   = (a->rom[off] & 0xF0) | (uint8_t)((addr >> 8) & 0xF);
        a->rom[off+1] = (uint8_t)(addr & 0xFF);
    }
    return true;
}

/* ── built-in OCTO_KEY constants (keyboard → hex keypad) ────────────────── */

static const struct { const char *name; int val; } octo_builtins[] = {
    {"OCTO_KEY_1", 0x1}, {"OCTO_KEY_2", 0x2}, {"OCTO_KEY_3", 0x3}, {"OCTO_KEY_4", 0xC},
    {"OCTO_KEY_Q", 0x4}, {"OCTO_KEY_W", 0x5}, {"OCTO_KEY_E", 0x6}, {"OCTO_KEY_R", 0xD},
    {"OCTO_KEY_A", 0x7}, {"OCTO_KEY_S", 0x8}, {"OCTO_KEY_D", 0x9}, {"OCTO_KEY_F", 0xE},
    {"OCTO_KEY_Z", 0xA}, {"OCTO_KEY_X", 0x0}, {"OCTO_KEY_C", 0xB}, {"OCTO_KEY_V", 0xF},
};

/* ── public API ──────────────────────────────────────────────────────────── */

int octo_compile(const char *source, uint8_t *out, int out_max,
                 char *errbuf, int errbuf_size)
{
    OAsm *a = calloc(1, sizeof(OAsm));
    if (!a) { snprintf(errbuf, (size_t)errbuf_size, "OOM"); return -1; }

    a->rom     = out;
    a->rom_max = out_max;
    a->pc      = OCTO_ROM_BASE;

    /* pre-load built-in constants */
    int nb = (int)(sizeof(octo_builtins) / sizeof(octo_builtins[0]));
    for (int i = 0; i < nb && a->nconsts < MAX_CONST; i++) {
        strncpy(a->consts[a->nconsts].name, octo_builtins[i].name, 63);
        a->consts[a->nconsts++].val = octo_builtins[i].val;
    }

    int ntoks = 0;
    a->toks = tokenize(source, &ntoks);
    a->ntoks = ntoks;
    if (!a->toks) { snprintf(errbuf, (size_t)errbuf_size, "OOM"); free(a); return -1; }

    while (!a->error && a->ti < a->ntoks)
        parse_stmt(a);

    if (!a->error) {
        if (a->if_sp > 0)   { snprintf(a->errmsg, 256, "unclosed 'begin'"); a->error = true; }
        if (a->loop_sp > 0) { snprintf(a->errmsg, 256, "unclosed 'loop'");  a->error = true; }
    }
    if (!a->error)
        a->error = !resolve_fwds(a);

    int result = -1;
    if (!a->error) {
        result = a->pc - OCTO_ROM_BASE;
    } else {
        snprintf(errbuf, (size_t)errbuf_size, "%s", a->errmsg);
    }

    /* cleanup */
    free_toks(a->toks, a->ntoks);
    for (int i = 0; i < a->nmacros; i++) {
        free_toks(a->macros[i].body, a->macros[i].nbody);
    }
    free(a);
    return result;
}
