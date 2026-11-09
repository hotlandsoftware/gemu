#pragma once
/*
 * Audio output for sound chips that make samples as they run: mono float
 * samples go into a ring buffer, and SDL's audio thread plays them out.
 *
 * When the ring runs dry (the emulator paused, or is waiting on something:
 * a debugger stop, a slow frame, a netplay peer) the last sample fades to
 * silence over a few milliseconds instead of stopping dead, which is what
 * clicks; when samples come back they crossfade in.  gemu_audio_queued()
 * reports what's waiting, as SDL_GetQueuedAudioSize() did, so run loops
 * pace themselves to the audio clock.
 */
#include <stddef.h>
#include <stdint.h>

typedef struct GemuAudioOut GemuAudioOut;

/* Open the default device for `rate` Hz.  NULL (with "who: ..." printed)
 * when there's no audio; callers then run silently. */
GemuAudioOut *gemu_audio_open(const char *who, int rate);
void          gemu_audio_close(GemuAudioOut *a);

void     gemu_audio_queue(GemuAudioOut *a, const float *samples, size_t n);
uint32_t gemu_audio_queued(const GemuAudioOut *a);     /* bytes of float samples waiting */
uint32_t gemu_audio_underruns(const GemuAudioOut *a);  /* times the ring ran dry while playing */
