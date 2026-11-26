/*
 * The part of 86Box v6.0's sound.h that sound drivers use, for GEMU's 86Box
 * layer (compat/86box).
 *
 * A handler is called for the samples since the last call (int32 stereo,
 * interleaved, 16-bit scale; it adds its own to the buffer) - in 86Box once
 * a buffer of SOUNDBUFLEN (48 kHz) or MUSICBUFLEN (49716 Hz) samples, here
 * whenever the device's GEMU stream is brought up to date, in pieces of at
 * most a buffer.  sound_pos_global and music_pos_global say how far into the
 * current piece the present moment is: a driver generates up to there before
 * a register change, so the change lands on the right sample.  Each device
 * with handlers gets one stream per kind ("sound", "music") through the
 * machine's mixer.
 *
 * From 86Box (src/include/86box/sound.h), GPLv2 or later.
 */
#ifndef EMU_SOUND_H
#define EMU_SOUND_H
#include <stdint.h>

#define FREQ_44100  44100
#define FREQ_48000  48000
#define FREQ_49716  49716
#define FREQ_88200  88200
#define FREQ_96000  96000

#define SOUND_FREQ  FREQ_48000
#define SOUNDBUFLEN (SOUND_FREQ / 50)

#define MUSIC_FREQ  FREQ_49716
#define MUSICBUFLEN (MUSIC_FREQ / 36)

/* (the CD-audio and wavetable rates: sizes in drivers' structures) */
#define CD_FREQ     FREQ_44100
#define CD_BUFLEN   (CD_FREQ / 10)
#define WT_FREQ     FREQ_44100
#define WTBUFLEN    (WT_FREQ / 45)

#ifdef __cplusplus
extern "C" {
#endif

extern int sound_pos_global;
extern int music_pos_global;

/* In the device's init. */
extern void sound_add_handler(void (*get_buffer)(int32_t *buffer, uint16_t len, void *priv), void *priv);
extern void music_add_handler(void (*get_buffer)(int32_t *buffer, uint16_t len, void *priv), void *priv);
/* The wavetable kind (the AWE's EMU8000) isn't here yet: stops GEMU. */
extern void wavetable_add_handler(void (*get_buffer)(int32_t *buffer, uint16_t len, void *priv), void *priv);

/* A card's mixer filtering the CD audio and the PC speaker: GEMU has no CD
 * audio, and the machine's speaker doesn't go through a card's mixer, so
 * these take note of nothing. */
extern void sound_set_cd_audio_filter(void (*filter)(int channel, double *buffer, void *priv), void *priv);
extern void sound_set_pc_speaker_filter(void (*filter)(int channel, double *buffer, void *priv), void *priv);

#ifdef __cplusplus
}
#endif

#endif /*EMU_SOUND_H*/
