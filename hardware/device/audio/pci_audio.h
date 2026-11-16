#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PCI controllers share the AC'97 codec and stereo output, not a guest
 * register interface. All host connections are kept in wiring. */
typedef enum { PCI_AUDIO_NONE, PCI_AUDIO_CS4281, PCI_AUDIO_AC97 } PciAudioModel;
typedef struct {
    void *opaque;
    uint64_t clock_hz;
    uint64_t (*clock)(void *);
    bool (*read)(void *, uint64_t, void *, unsigned);
    bool (*write)(void *, uint64_t, const void *, unsigned);
    void (*irq)(void *, bool);
    void (*output)(void *, const float *, size_t frames);
} PciAudioWiring;

typedef struct {
    uint32_t bdbar, address, control;
    uint16_t status, remaining;
    uint8_t civ, lvi, piv, command;
    bool valid;
    uint32_t phase;
    float last[2];
} IchAudioDma;

typedef struct {
    PciAudioModel model;
    uint8_t cfg[256];
    uint16_t codec[64];
    float gain[2];
    uint64_t mark, fraction;
    bool irq_level, debug;
    uint64_t frames, nonzero_frames, dma_bytes, interrupts;
    union {
        struct {
            uint32_t reg[1024];
            uint8_t fifo[65536];
            uint32_t phase[4];
            float last[4][2];
            bool intena;
        } cs;
        struct {
            IchAudioDma dma[3];
            uint32_t control, status;
            uint8_t semaphore;
        } ich;
    } u;
    PciAudioWiring wiring;
} PciAudio;

void pci_audio_init(PciAudio *, PciAudioModel, const PciAudioWiring *);
void pci_audio_reset(PciAudio *);
void pci_audio_poll(PciAudio *);
uint32_t pci_audio_cfg_read(PciAudio *, unsigned reg, unsigned size);
void pci_audio_cfg_write(PciAudio *, unsigned reg, unsigned size, uint32_t);
bool pci_audio_mem_claims(const PciAudio *, uint64_t addr);
bool pci_audio_mem_access(PciAudio *, bool write, uint64_t, unsigned, uint64_t *);
bool pci_audio_io_access(PciAudio *, bool write, uint16_t, unsigned, uint64_t *);
const char *pci_audio_name(PciAudioModel);

/* Private controller interface. */
uint16_t pci_audio_codec_read(PciAudio *, unsigned);
void pci_audio_codec_write(PciAudio *, unsigned, uint16_t);
void pci_audio_irq(PciAudio *, bool);
bool pci_audio_dma_read(PciAudio *, uint32_t, void *, unsigned);
bool pci_audio_dma_write(PciAudio *, uint32_t, const void *, unsigned);
void cs4281_reset(PciAudio *);
uint32_t cs4281_read(PciAudio *, unsigned, uint32_t byte_mask);
void cs4281_write(PciAudio *, unsigned, uint32_t, uint32_t byte_mask);
bool cs4281_frame(PciAudio *, float out[2]);
void ich_audio_reset(PciAudio *);
uint8_t ich_audio_read(PciAudio *, unsigned);
void ich_audio_write(PciAudio *, unsigned, uint8_t);
bool ich_audio_frame(PciAudio *, float out[2]);
