#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct RcaPcSpeaker RcaPcSpeaker;

RcaPcSpeaker *rca_pcspk_create(unsigned frequency_hz);
void          rca_pcspk_destroy(RcaPcSpeaker *spk);
void          rca_pcspk_set_gate(RcaPcSpeaker *spk, uint8_t gate);
void          rca_pcspk_set_freq(RcaPcSpeaker *spk, unsigned frequency_hz);
bool          rca_pcspk_is_active(const RcaPcSpeaker *spk);
