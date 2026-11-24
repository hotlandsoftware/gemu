#pragma once
/* Between the NES front end's two files: nes_frontend.c (window, input,
 * dialogs, save files, run loop) and nes_netplay.c. */
#include "nes.h"
#include "gemu/gemu_display.h"
#include <stddef.h>
#include <stdint.h>

/* nes_frontend.c */
void nes_ui_save_persistent(NesState *s);   /* battery or FDS save file, if any */
void nes_ui_game_name(const char *path, char *out, size_t len);   /* "dir/Game.nes" -> "Game" */
/* A save state back, with the Game Genie codes that are on now (not the
 * snapshot's). */
bool nes_ui_load_state(NesState *s, const uint8_t *data, size_t len);

/* nes_netplay.c */
void nes_np_setup(NesState *s, GemuDisplay *display);   /* after the first reset */
void nes_np_destroy(NesState *s);           /* any game ended: the player's own back */
/* Before a frame: false while the other players' input hasn't arrived
 * (run nothing this time round). */
bool nes_np_frame(NesState *s);
void nes_np_after_frame(NesState *s);
/* Reset pressed: true if a netplay game took it (every machine resets on
 * the same frame). */
bool nes_np_take_reset(void);
bool nes_np_active(void);
void nes_np_cart_changed(NesState *s);      /* Change...: renamed, no fresh power-on */
void nes_np_refresh(void);                  /* the dialog, once a frame */
