#pragma once

typedef struct MidiOut MidiOut;

MidiOut *midi_out_open (void);   /* creates ALSA seq port; NULL on failure */
void     midi_out_close(MidiOut *m);

void midi_out_note_on (MidiOut *m, int ch, int note, int vel);
void midi_out_note_off(MidiOut *m, int ch, int note);
void midi_out_program (MidiOut *m, int ch, int prog);
void midi_out_all_off (MidiOut *m);  /* CC 123 on all channels */
