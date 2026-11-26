/*
 * The part of 86Box v6.0's midi.h that sound cards use, for GEMU's 86Box
 * layer (compat/86box).  GEMU's machines have nothing plugged into a card's
 * MIDI port yet: what a card sends out goes nowhere, and no input ever
 * arrives (midi_in_handler() is noted and never called back).  That is a
 * card with nothing connected, not a missing feature: software can't tell.
 *
 * From 86Box (src/include/86box/midi.h), GPLv2 or later.
 */
#ifndef EMU_SOUND_MIDI_H
#define EMU_SOUND_MIDI_H
#include <stdint.h>

#define SYSEX_SIZE 8192

#ifdef __cplusplus
extern "C" {
#endif

extern void midi_raw_out_byte(uint8_t val);
extern void midi_clear_buffer(void);
extern void midi_in_handler(int set, void (*msg)(void *priv, uint8_t *msg, uint32_t len),
                            int (*sysex)(void *priv, uint8_t *buffer, uint32_t len, int abort),
                            void *priv);

#ifdef __cplusplus
}
#endif

#endif /*EMU_SOUND_MIDI_H*/
