#include "midi_out.h"
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>

struct MidiOut {
    snd_seq_t *seq;
    int        port;
};

MidiOut *midi_out_open(void) {
    MidiOut *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    if (snd_seq_open(&m->seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
        fprintf(stderr, "midi: snd_seq_open failed\n");
        free(m); return NULL;
    }
    snd_seq_set_client_name(m->seq, "gemu-mos NES 2A03");
    m->port = snd_seq_create_simple_port(m->seq, "2A03",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (m->port < 0) {
        fprintf(stderr, "midi: port creation failed\n");
        snd_seq_close(m->seq); free(m); return NULL;
    }
    fprintf(stderr, "midi: ALSA %d:0 ready — connect with:  aconnect %d:0 <synth:port>\n",
            snd_seq_client_id(m->seq), snd_seq_client_id(m->seq));
    return m;
}

void midi_out_close(MidiOut *m) {
    if (!m) return;
    snd_seq_close(m->seq);
    free(m);
}

static void send_ev(MidiOut *m, snd_seq_event_t *ev) {
    snd_seq_ev_set_subs(ev);
    snd_seq_ev_set_direct(ev);
    snd_seq_ev_set_source(ev, m->port);
    snd_seq_event_output_direct(m->seq, ev);
}

void midi_out_note_on(MidiOut *m, int ch, int note, int vel) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_noteon(&ev, ch, note, vel);
    send_ev(m, &ev);
}

void midi_out_note_off(MidiOut *m, int ch, int note) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_noteoff(&ev, ch, note, 0);
    send_ev(m, &ev);
}

void midi_out_program(MidiOut *m, int ch, int prog) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_pgmchange(&ev, ch, prog);
    send_ev(m, &ev);
}

void midi_out_all_off(MidiOut *m) {
    for (int ch = 0; ch < 16; ch++) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_controller(&ev, ch, 123, 0);
        send_ev(m, &ev);
    }
}
