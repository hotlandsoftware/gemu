#pragma once
/* NES Game Genie cheats: the list the Debug > Game Genie window and the
 * monitor's "gamegenie" command edit (codes, names, on/off), saved per game
 * to <config>/<game>.cht.  The machine turns the enabled ones into ROM
 * patches whenever the list changes (the changed callback). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NES_CHEATS_MAX 64

typedef struct {
    char code[9];        /* 6- or 8-letter Game Genie code, upper case */
    char name[64];
    bool enabled;
} NesCheat;

typedef struct NesCheats {
    NesCheat list[NES_CHEATS_MAX];
    int      n;
    unsigned version;    /* bumped on every change, so views know to reload */
    char     path[512];  /* .cht file; "" = not saved */
    void   (*changed)(void *ud);
    void    *ud;
} NesCheats;

/* Game Genie code <-> patch.  Decode accepts either case; encode writes an
 * 8-letter code when cmp >= 0, else a 6-letter one.  addr is $8000-$FFFF. */
bool nes_gg_decode(const char *code, uint16_t *addr, uint8_t *val, uint8_t *cmp, bool *has_cmp);
bool nes_gg_encode(uint16_t addr, uint8_t val, int cmp, char out[9]);

/* Loads path (if it exists).  changed is called after every edit. */
void nes_cheats_init(NesCheats *c, const char *path, void (*changed)(void *ud), void *ud);

/* Edits; each saves the file and calls changed.  add/update return false
 * with a reason in err for an invalid or duplicate code or a full list. */
bool nes_cheats_add(NesCheats *c, const char *code, const char *name, bool enabled,
                    char *err, size_t err_len);
bool nes_cheats_update(NesCheats *c, int i, const char *code, const char *name,
                       char *err, size_t err_len);
void nes_cheats_remove(NesCheats *c, int i);
void nes_cheats_set_enabled(NesCheats *c, int i, bool enabled);
int  nes_cheats_find(const NesCheats *c, const char *code);   /* -1 if absent */
