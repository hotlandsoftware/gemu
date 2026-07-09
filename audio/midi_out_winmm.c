#include "midi_out.h"
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>

struct MidiOut {
    HMIDIOUT handle;
};

MidiOut *midi_out_open(void) {
    MidiOut *m = calloc(1, sizeof *m);
    if (!m) return NULL;

    MMRESULT r = midiOutOpen(&m->handle, MIDI_MAPPER, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
        char errbuf[256] = {0};
        midiOutGetErrorTextA(r, errbuf, sizeof errbuf);
        fprintf(stderr, "midi: midiOutOpen failed: %s\n", errbuf);
        free(m);
        return NULL;
    }

    UINT ndevs = midiOutGetNumDevs();
    MIDIOUTCAPSA caps = {0};
    /* MIDI_MAPPER resolves to some device; show which one */
    if (ndevs > 0 && midiOutGetDevCapsA(MIDI_MAPPER, &caps, sizeof caps) == MMSYSERR_NOERROR)
        fprintf(stderr, "midi: Windows MIDI out open - %s\n", caps.szPname);
    else
        fprintf(stderr, "midi: Windows MIDI out open\n");

    return m;
}

void midi_out_close(MidiOut *m) {
    if (!m) return;
    midiOutReset(m->handle);
    midiOutClose(m->handle);
    free(m);
}

static void send_short(MidiOut *m, BYTE status, BYTE d1, BYTE d2) {
    midiOutShortMsg(m->handle, (DWORD)status | ((DWORD)d1 << 8) | ((DWORD)d2 << 16));
}

void midi_out_note_on(MidiOut *m, int ch, int note, int vel) {
    send_short(m, (BYTE)(0x90 | ch), (BYTE)note, (BYTE)vel);
}

void midi_out_note_off(MidiOut *m, int ch, int note) {
    send_short(m, (BYTE)(0x80 | ch), (BYTE)note, 0);
}

void midi_out_program(MidiOut *m, int ch, int prog) {
    send_short(m, (BYTE)(0xC0 | ch), (BYTE)prog, 0);
}

void midi_out_all_off(MidiOut *m) {
    for (int ch = 0; ch < 16; ch++)
        send_short(m, (BYTE)(0xB0 | ch), 123, 0);
}
